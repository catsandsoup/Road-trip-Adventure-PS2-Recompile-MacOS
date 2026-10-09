// Differential fuzz for GSCpuBackend: replays a seeded random stream of draws, CLUT loads and local
// memory writes, and prints a hash of local memory every N operations. Build the same source against
// two runtimes (a reference snapshot and the candidate) and diff the outputs; any rasteriser change
// that claims to be output-neutral must produce identical lines.
//
//   gs_raster_diff <seed> <operations> [hash-every=256] [first-hashed-op=0]
//
// The stream deliberately covers far more state than the game uses: every primitive type, every
// storage mode (including reserved ones) for textures, frames and Z, all TEST/ALPHA/CLAMP fields,
// fractional, negative and out-of-scissor coordinates, render-to-texture on overlapping pages and
// CLUT reloads with every CLD/CSM/CSA/CPSM and TEXA-only changes between draws.
#include "gs_test_support.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
    struct Random
    {
        uint64_t state;

        uint32_t next()
        {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            return static_cast<uint32_t>(state >> 16);
        }

        uint32_t below(uint32_t n) { return n ? next() % n : 0u; }
        bool chance(uint32_t percent) { return below(100u) < percent; }
        float unit() { return static_cast<float>(next() & 0xFFFFFFu) / static_cast<float>(0x1000000); }
        float range(float lo, float hi) { return lo + (hi - lo) * unit(); }

        template <size_t N>
        uint8_t pick(const uint8_t (&values)[N]) { return values[below(N)]; }
    };

    constexpr uint8_t kTexturePsms[] = {GS_PSM_CT32, GS_PSM_CT24, GS_PSM_CT16, GS_PSM_CT16S, GS_PSM_T8, GS_PSM_T8H,
                                        GS_PSM_T4, GS_PSM_T4HL, GS_PSM_T4HH, GS_PSM_Z32, GS_PSM_Z24, GS_PSM_Z16,
                                        GS_PSM_Z16S, 0x05u, 0x3Fu};
    constexpr uint8_t kIndexedPsms[] = {GS_PSM_T8, GS_PSM_T8H, GS_PSM_T4, GS_PSM_T4HL, GS_PSM_T4HH};
    constexpr uint8_t kClutPsms[] = {GS_PSM_CT32, GS_PSM_CT24, GS_PSM_CT16, GS_PSM_CT16S, 0x05u};
    constexpr uint8_t kFramePsms[] = {GS_PSM_CT32, GS_PSM_CT24, GS_PSM_CT16, GS_PSM_CT16S, GS_PSM_Z24, GS_PSM_Z32, GS_PSM_Z16, 0x3Fu};
    constexpr uint8_t kDepthPsms[] = {GS_PSM_Z24, GS_PSM_Z32, GS_PSM_Z16, GS_PSM_Z16S, GS_PSM_CT32, 0x3Fu};
    constexpr GSPrimType kPrimTypes[] = {GS_PRIM_POINT, GS_PRIM_LINE, GS_PRIM_LINESTRIP, GS_PRIM_TRIANGLE,
                                         GS_PRIM_TRISTRIP, GS_PRIM_TRIFAN, GS_PRIM_SPRITE};

    GSTex0Reg randomTex0(Random &rng, uint32_t frameBlock)
    {
        GSTex0Reg tex{};
        // Often render-to-texture on the current frame pages, or nearby overlapping pages.
        tex.tbp0 = rng.chance(25) ? frameBlock + rng.below(64u) : rng.below(16384u);
        tex.tbw = static_cast<uint8_t>(rng.chance(5) ? 0u : rng.below(12u));
        tex.psm = rng.chance(60) ? rng.pick(kIndexedPsms) : rng.pick(kTexturePsms);
        tex.tw = static_cast<uint8_t>(rng.below(11u));
        tex.th = static_cast<uint8_t>(rng.below(11u));
        tex.tcc = static_cast<uint8_t>(rng.below(2u));
        tex.tfx = static_cast<uint8_t>(rng.below(4u));
        tex.cbp = rng.below(16384u);
        tex.cpsm = rng.pick(kClutPsms);
        tex.csm = static_cast<uint8_t>(rng.below(2u));
        tex.csa = static_cast<uint8_t>(rng.below(32u));
        tex.cld = static_cast<uint8_t>(rng.below(8u));
        return tex;
    }

    float coordinate(Random &rng, float lo, float hi)
    {
        if (rng.chance(3))
            return rng.range(-4096.0f, 4096.0f);
        float v = rng.range(lo, hi);
        if (rng.chance(50))
            v = static_cast<float>(static_cast<int>(v * 16.0f)) / 16.0f; // 12.4 fixed point positions
        return v;
    }

    GSVertex randomVertex(Random &rng, float x0, float y0, float spread)
    {
        GSVertex v{};
        v.x = coordinate(rng, x0 - 8.0f, x0 + spread);
        v.y = coordinate(rng, y0 - 8.0f, y0 + spread);
        switch (rng.below(4u))
        {
        case 0: v.z = rng.below(0x1000000u); break;
        case 1: v.z = static_cast<double>(rng.next()); break;
        case 2: v.z = rng.below(0x10000u) + rng.unit(); break;
        default: v.z = 0x1000000u + rng.below(0x1000u); break;
        }
        v.r = static_cast<uint8_t>(rng.next());
        v.g = static_cast<uint8_t>(rng.next());
        v.b = static_cast<uint8_t>(rng.next());
        v.a = static_cast<uint8_t>(rng.chance(30) ? 0x80u : rng.next());
        v.q = rng.chance(5) ? (rng.chance(50) ? 0.0f : -rng.range(0.1f, 2.0f)) : rng.range(0.05f, 4.0f);
        v.s = rng.range(-0.5f, 1.5f) * v.q;
        v.t = rng.range(-0.5f, 1.5f) * v.q;
        v.u = static_cast<uint16_t>(rng.below(rng.chance(80) ? 4096u : 65536u));
        v.v = static_cast<uint16_t>(rng.below(rng.chance(80) ? 4096u : 65536u));
        v.fog = static_cast<uint8_t>(rng.next());
        return v;
    }

    GSPrimitiveBatch randomBatch(Random &rng, const GSTexaReg &texa, const GSTex0Reg &boundTex0)
    {
        GSPrimitiveBatch batch{};
        GSDrawState &state = batch.state;
        GSContext &ctx = state.context;

        // Half the draws use the game's dominant target pair (CT32 colour, Z24 depth).
        const bool hotTargets = rng.chance(50);
        ctx.frame.fbp = rng.below(rng.chance(80) ? 64u : 512u);
        ctx.frame.fbw = rng.below(12u);
        ctx.frame.psm = hotTargets ? GS_PSM_CT32 : rng.pick(kFramePsms);
        ctx.frame.fbmsk = rng.chance(85) ? 0u : (rng.chance(50) ? 0xFF000000u : rng.next());
        ctx.zbuf.zbp = rng.below(rng.chance(80) ? 128u : 512u);
        ctx.zbuf.psm = hotTargets ? GS_PSM_Z24 : rng.pick(kDepthPsms);
        ctx.zbuf.zmask = rng.chance(25);

        ctx.scissor.x0 = static_cast<uint16_t>(rng.below(64u));
        ctx.scissor.y0 = static_cast<uint16_t>(rng.below(64u));
        ctx.scissor.x1 = static_cast<uint16_t>(rng.chance(5) ? rng.below(64u) : ctx.scissor.x0 + rng.below(400u));
        ctx.scissor.y1 = static_cast<uint16_t>(rng.chance(5) ? rng.below(64u) : ctx.scissor.y0 + rng.below(300u));
        ctx.xyoffset.ofx = static_cast<uint16_t>(rng.chance(50) ? 0u : rng.below(0x10000u));
        ctx.xyoffset.ofy = static_cast<uint16_t>(rng.chance(50) ? 0u : rng.below(0x10000u));

        ctx.tex0 = rng.chance(70) ? boundTex0 : randomTex0(rng, ctx.frame.fbp << 5u);
        ctx.clamp = (static_cast<uint64_t>(rng.below(4u))) |
                    (static_cast<uint64_t>(rng.below(4u)) << 2) |
                    (static_cast<uint64_t>(rng.below(1024u)) << 4) |
                    (static_cast<uint64_t>(rng.below(1024u)) << 14) |
                    (static_cast<uint64_t>(rng.below(1024u)) << 24) |
                    (static_cast<uint64_t>(rng.below(1024u)) << 34);
        ctx.alpha = static_cast<uint64_t>(rng.below(256u)) | (static_cast<uint64_t>(rng.below(256u)) << 32);
        ctx.test = static_cast<uint64_t>(rng.next() & 0x7FFFFu);
        if (rng.chance(50))
            ctx.test = (ctx.test & ~(3ull << 17)) | (2ull << 17) | (1ull << 16); // ZTE, GEQUAL as in-game
        ctx.fba = rng.below(2u);

        state.prim.type = kPrimTypes[rng.below(7u)];
        if (rng.chance(40))
            state.prim.type = rng.chance(50) ? GS_PRIM_TRIANGLE : GS_PRIM_SPRITE;
        state.prim.iip = rng.chance(60);
        state.prim.tme = rng.chance(70);
        state.prim.fge = rng.chance(40);
        state.prim.abe = rng.chance(60);
        state.prim.fst = rng.chance(40);
        state.prim.ctxt = false;
        state.texa = texa;
        state.pabe = rng.chance(15);
        state.fogR = static_cast<uint8_t>(rng.next());
        state.fogG = static_cast<uint8_t>(rng.next());
        state.fogB = static_cast<uint8_t>(rng.next());
        state.textureWidth = static_cast<uint16_t>(1u << std::min<uint32_t>(ctx.tex0.tw, 10u));
        state.textureHeight = static_cast<uint16_t>(1u << std::min<uint32_t>(ctx.tex0.th, 10u));
        state.linearFilter = rng.chance(70);

        const float x0 = static_cast<float>(ctx.xyoffset.ofx >> 4) + rng.range(-16.0f, 320.0f);
        const float y0 = static_cast<float>(ctx.xyoffset.ofy >> 4) + rng.range(-16.0f, 240.0f);
        const float spread = rng.chance(60) ? rng.range(1.0f, 12.0f) : rng.range(12.0f, 300.0f);
        batch.vertexCount = 3;
        for (auto &v : batch.vertices)
            v = randomVertex(rng, x0, y0, spread);
        if (state.prim.type == GS_PRIM_SPRITE && rng.chance(50))
        {
            // Screen-aligned blits, like the game's full-screen passes.
            batch.vertices[0].x = x0;
            batch.vertices[0].y = y0;
            batch.vertices[1].x = x0 + static_cast<float>(1 + rng.below(320u));
            batch.vertices[1].y = y0 + static_cast<float>(1 + rng.below(240u));
        }
        return batch;
    }

    uint64_t hashBytes(const std::vector<uint8_t> &bytes)
    {
        uint64_t h = 1469598103934665603ull;
        const uint64_t *words = reinterpret_cast<const uint64_t *>(bytes.data());
        for (size_t i = 0; i < bytes.size() / 8u; ++i)
        {
            h ^= words[i];
            h *= 1099511628211ull;
        }
        return h;
    }
}

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: %s <seed> <operations> [hash-every=256] [first-hashed-op=0]\n", argv[0]);
        return 2;
    }
    const uint64_t seed = std::strtoull(argv[1], nullptr, 0);
    const uint32_t operations = static_cast<uint32_t>(std::strtoul(argv[2], nullptr, 0));
    const uint32_t hashEvery = argc > 3 ? static_cast<uint32_t>(std::strtoul(argv[3], nullptr, 0)) : 256u;
    const uint32_t firstHashed = argc > 4 ? static_cast<uint32_t>(std::strtoul(argv[4], nullptr, 0)) : 0u;

    Random rng{seed * 0x9E3779B97F4A7C15ull + 1u};
    std::vector<uint8_t> vram(GSTest::kVramSize);
    for (size_t i = 0; i < vram.size(); i += 4)
    {
        const uint32_t v = rng.next();
        std::memcpy(vram.data() + i, &v, 4);
    }
    GSCpuBackend backend;
    backend.Initialize(vram.data(), static_cast<uint32_t>(vram.size()));

    GSTexaReg texa{};
    texa.ta0 = 0x80;
    texa.ta1 = 0x40;
    GSTex0Reg bound = randomTex0(rng, 0u);
    GSTexClutReg texclut{};

    for (uint32_t op = 0; op < operations; ++op)
    {
        const uint32_t kind = rng.below(100u);
        if (kind < 6u)
        {
            // TEX0 write with a CLUT load (the frontend calls LoadClut on every TEX0/TEX2 write).
            bound = randomTex0(rng, rng.below(512u) << 5u);
            texclut.cbw = static_cast<uint8_t>(rng.below(8u));
            texclut.cou = static_cast<uint8_t>(rng.below(16u));
            texclut.cov = static_cast<uint16_t>(rng.below(64u));
            backend.LoadClut(bound, texclut);
        }
        else if (kind < 9u)
        {
            // TEXA-only change between draws.
            texa.ta0 = static_cast<uint8_t>(rng.next());
            texa.ta1 = static_cast<uint8_t>(rng.next());
            texa.aem = rng.chance(50);
        }
        else if (kind < 11u)
        {
            // Same TEX0 but switch between 4-bit and 8-bit indices without a reload.
            bound.psm = rng.pick(kIndexedPsms);
            bound.cld = 0;
        }
        else if (kind < 14u)
        {
            const uint32_t psm = rng.pick(kTexturePsms);
            const uint32_t base = rng.below(16384u);
            const uint32_t bw = rng.below(12u);
            for (uint32_t i = 0; i < 64u; ++i)
                backend.WriteVram(psm, base, bw, rng.below(256u), rng.below(256u), rng.next());
        }
        else if (kind < 15u)
        {
            backend.TextureFlush();
        }
        else
        {
            backend.Submit(randomBatch(rng, texa, bound));
        }

        if (op >= firstHashed && ((op + 1u) % hashEvery == 0u || op + 1u == operations))
        {
            std::vector<uint8_t> snapshot;
            backend.SnapshotVram(snapshot);
            std::printf("op %u hash %016llx\n", op + 1u, static_cast<unsigned long long>(hashBytes(snapshot)));
        }
    }
    return 0;
}
