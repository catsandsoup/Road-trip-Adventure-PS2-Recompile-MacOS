// Offline VU1 equivalence test: replays MSCAL snapshots captured in-game with
// PS2X_VU1_CAPTURE=<dir> through the VU1Interpreter (oracle) and through the
// statically recompiled native code (fast and full-flag variants) and compares
// GIF packets, VF/VI/ACC/Q/P/I/R/pc, MAC/status/clip, data memory and cycles.
//
// Optional fuzzing (--fuzz N): each snapshot is additionally replayed N times
// with VF registers and float words of VU data memory perturbed towards IEEE
// special values (denormals, +-0, Inf, NaN, huge, tiny) to exercise the
// operand clamp / result flush / flag paths.
//
// Usage: vu1_replay <catalog-dir> <snapshot-dir>[,<dir>...] [--fuzz N] [--no-ftz] [--live-exit mcs] [--poison N]
// Build: game/tests/vu1/build.sh   (links against the PS2Recomp-vu1 runtime sources
//        and the generated work/vu1gen/*.cpp; nothing disc-derived is in git)

#include "runtime/ps2_memory.h"
#include "runtime/ps2_vu1.h"
#include "runtime/ps2_vu1_recomp.h"

#include <algorithm>
#include <cfenv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <random>
#include <string>
#include <vector>

namespace
{
    struct Capture
    {
        std::vector<std::vector<uint8_t>> packets;
        static void sink(void *u, const uint8_t *d, uint32_t n)
        {
            static_cast<Capture *>(u)->packets.emplace_back(d, d + n);
        }
    };

    struct Snapshot
    {
        uint32_t pc, top, itop, maxCycles;
        uint64_t hash, cycle;
        VU1State state;
        std::vector<uint8_t> data;
    };

    bool loadSnapshot(const std::string &path, Snapshot &s)
    {
        FILE *f = std::fopen(path.c_str(), "rb");
        if (!f)
            return false;
        uint32_t h[8];
        bool ok = std::fread(h, sizeof(h), 1, f) == 1 && h[0] == 0x53315556u && h[6] == sizeof(VU1State);
        if (ok)
        {
            s.pc = h[2];
            s.top = h[3];
            s.itop = h[4];
            s.maxCycles = h[5];
            s.data.resize(PS2_VU1_DATA_SIZE);
            ok = std::fread(&s.hash, 8, 1, f) == 1 && std::fread(&s.cycle, 8, 1, f) == 1 &&
                 std::fread(&s.state, sizeof(VU1State), 1, f) == 1 &&
                 std::fread(s.data.data(), 1, PS2_VU1_DATA_SIZE, f) == PS2_VU1_DATA_SIZE;
        }
        std::fclose(f);
        return ok;
    }

    uint32_t fb(float f)
    {
        uint32_t b;
        std::memcpy(&b, &f, 4);
        return b;
    }

    struct Result
    {
        VU1State st;
        std::vector<uint8_t> data;
        Capture pk;
        uint64_t cycle = 0;
        bool failed = false;
        bool ended = true;
    };

    enum : unsigned
    {
        kMac = 1u,
        kStatus = 2u,
        kClip = 4u,
        kAllFlags = 7u
    };

    int compare(const char *tag, const Result &a, const Result &ref, unsigned flags, bool verbose)
    {
        int n = 0;
        auto rep = [&](const char *fmt, auto... args)
        {
            if (verbose && n < 12)
            {
                std::printf("    [%s] ", tag);
                std::printf(fmt, args...);
                std::printf("\n");
            }
            ++n;
        };
        if (a.failed)
            rep("%s", "native failed");
        for (int r = 0; r < 32; ++r)
            for (int c = 0; c < 4; ++c)
                if (fb(a.st.vf[r][c]) != fb(ref.st.vf[r][c]))
                    rep("vf%d.%c %08x vs %08x", r, "xyzw"[c], fb(a.st.vf[r][c]), fb(ref.st.vf[r][c]));
        for (int c = 0; c < 4; ++c)
            if (fb(a.st.acc[c]) != fb(ref.st.acc[c]))
                rep("acc.%c %08x vs %08x", "xyzw"[c], fb(a.st.acc[c]), fb(ref.st.acc[c]));
        for (int r = 0; r < 16; ++r)
            if (a.st.vi[r] != ref.st.vi[r])
                rep("vi%d %d vs %d", r, a.st.vi[r], ref.st.vi[r]);
        if (fb(a.st.q) != fb(ref.st.q))
            rep("q %08x vs %08x", fb(a.st.q), fb(ref.st.q));
        if (fb(a.st.p) != fb(ref.st.p))
            rep("p");
        if (fb(a.st.i) != fb(ref.st.i))
            rep("i");
        if (a.st.r != ref.st.r)
            rep("r");
        if (a.st.pc != ref.st.pc)
            rep("pc %x vs %x", a.st.pc, ref.st.pc);
        if ((flags & kMac) && a.st.mac != ref.st.mac)
            rep("mac %04x vs %04x", a.st.mac, ref.st.mac);
        if ((flags & kStatus) && a.st.status != ref.st.status)
            rep("status %03x vs %03x", a.st.status, ref.st.status);
        if ((flags & kClip) && a.st.clip != ref.st.clip)
            rep("clip %06x vs %06x", a.st.clip, ref.st.clip);
        if (a.cycle != ref.cycle)
            rep("cycles %llu vs %llu", (unsigned long long)a.cycle, (unsigned long long)ref.cycle);
        for (size_t i = 0; i < a.data.size(); ++i)
            if (a.data[i] != ref.data[i])
            {
                rep("data first diff at 0x%04zx", i);
                break;
            }
        if (a.pk.packets != ref.pk.packets)
            rep("gif packets differ (%zu vs %zu)", a.pk.packets.size(), ref.pk.packets.size());
        return n;
    }

    void perturb(Snapshot &s, std::mt19937 &rng)
    {
        static const uint32_t specials[] = {0x00000001u, 0x807FFFFFu, 0x00000000u, 0x80000000u, 0x7F800000u,
                                            0xFF800000u, 0x7FC00000u, 0xFFFFFFFFu, 0x7F7FFFFFu, 0x00800000u,
                                            0x80800001u, 0x7F000000u, 0x1E800000u, 0x0C000000u};
        auto pick = [&]() -> uint32_t
        {
            const uint32_t sp = specials[rng() % (sizeof(specials) / sizeof(specials[0]))];
            switch (rng() % 3)
            {
            case 0:
                return sp;
            case 1:
                return sp ^ (rng() & 0x007FFFFFu);
            default:
                return (rng() & 0x80000000u) | ((rng() % 2) ? 0x01000000u : 0x7E000000u) | (rng() & 0x00FFFFFFu);
            }
        };
        for (int k = 0; k < 6; ++k)
        {
            const int r = 1 + static_cast<int>(rng() % 31);
            const int c = static_cast<int>(rng() % 4);
            const uint32_t b = pick();
            std::memcpy(&s.state.vf[r][c], &b, 4);
        }
        // Perturb float-looking words in data memory (exponent field plausible for a float).
        for (int k = 0; k < 64; ++k)
        {
            const size_t off = (rng() % (PS2_VU1_DATA_SIZE / 4)) * 4;
            uint32_t w;
            std::memcpy(&w, s.data.data() + off, 4);
            const uint32_t e = (w >> 23) & 0xFFu;
            if (e >= 0x60u && e <= 0x9Fu)
            {
                w = pick();
                std::memcpy(s.data.data() + off, &w, 4);
            }
        }
    }
}

namespace
{
    Result runInterp(const std::vector<uint8_t> &code, const Snapshot &s, GS &gs)
    {
        Result r;
        VU1Interpreter vu(VU1Interpreter::Unit::VU1);
        vu.state() = s.state;
        vu.setCycleCount(s.cycle);
        r.data = s.data;
        vu.setPacketSink(&Capture::sink, &r.pk);
        vu.execute(const_cast<uint8_t *>(code.data()), PS2_VU1_CODE_SIZE, r.data.data(), PS2_VU1_DATA_SIZE, gs,
                   nullptr, s.pc, s.top, s.itop, s.maxCycles);
        r.st = vu.state();
        r.cycle = vu.cycleCount();
        r.ended = vu.lastRunEnded();
        return r;
    }

    Result runNative(Vu1RecFn fn, const Snapshot &s)
    {
        static Vu1RecContext ctx;
        Result r;
        r.st = s.state;
        r.data = s.data;
        ctx = Vu1RecContext{};
        ctx.state = &r.st;
        ctx.data = r.data.data();
        ctx.cycle = s.cycle;
        ctx.maxCycles = s.maxCycles;
        ctx.top = s.top;
        ctx.itop = s.itop;
        ctx.sink = &Capture::sink;
        ctx.sinkUser = &r.pk;
        fn(ctx, s.pc);
        r.cycle = ctx.cycle;
        r.failed = ctx.failed;
        return r;
    }

    unsigned parseFlagMask(const char *v)
    {
        unsigned m = 0;
        for (; *v; ++v)
            m |= *v == 'm' ? kMac : *v == 's' ? kStatus : *v == 'c' ? kClip : 0u;
        return m;
    }
}

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr,
                     "usage: %s <catalog-dir> <snapshot-dir>[,<dir>...] [--fuzz N] [--no-ftz] [--live-exit mcs] "
                     "[--poison N]\n"
                     "  --live-exit: flags the FAST variant stores at exit (generator's live-at-exit line; "
                     "m=mac s=status c=clip; default c). Those are compared bit-exactly.\n"
                     "  --poison N: per snapshot, N extra runs with random input MAC/status. Fast and the\n"
                     "              interpreter itself must reproduce the unpoisoned interpreter on all state\n"
                     "              except the dead flags (proves MAC/status are not live into any entry).\n",
                     argv[0]);
        return 2;
    }
    const std::string catalog = argv[1];
    int fuzz = 0, poison = 0;
    bool ftz = true;
    unsigned liveExit = kClip;
    for (int i = 3; i < argc; ++i)
    {
        if (!std::strcmp(argv[i], "--fuzz") && i + 1 < argc)
            fuzz = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--poison") && i + 1 < argc)
            poison = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--live-exit") && i + 1 < argc)
            liveExit = parseFlagMask(argv[++i]);
        else if (!std::strcmp(argv[i], "--no-ftz"))
            ftz = false;
    }
    // Match the game thread: round toward zero + flush-to-zero (ps2_runtime.cpp gameThread).
    std::fesetround(FE_TOWARDZERO);
#if defined(__aarch64__)
    if (ftz)
    {
        uint64_t fpcr = 0;
        __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
        fpcr |= (1ull << 24);
        __asm__ volatile("msr fpcr, %0" : : "r"(fpcr));
    }
#endif

    std::vector<std::string> files;
    {
        std::string dirs = argv[2];
        size_t pos = 0;
        while (pos <= dirs.size())
        {
            size_t end = dirs.find(',', pos);
            if (end == std::string::npos)
                end = dirs.size();
            const std::string snapDir = dirs.substr(pos, end - pos);
            pos = end + 1;
            if (snapDir.empty())
                continue;
            std::vector<std::string> one;
            if (DIR *d = opendir(snapDir.c_str()))
            {
                while (dirent *e = readdir(d))
                    if (!std::strncmp(e->d_name, "snap_", 5))
                        one.push_back(snapDir + "/" + e->d_name);
                closedir(d);
            }
            std::sort(one.begin(), one.end());
            std::printf("vu1_replay: %s: %zu snapshots\n", snapDir.c_str(), one.size());
            files.insert(files.end(), one.begin(), one.end());
        }
    }
    std::printf("vu1_replay: %zu snapshots, %zu native images, fuzz=%d, poison=%d, ftz=%d, fast live-exit=%s%s%s\n",
                files.size(), vu1recImageCount(), fuzz, poison, ftz ? 1 : 0, (liveExit & kMac) ? "m" : "",
                (liveExit & kStatus) ? "s" : "", (liveExit & kClip) ? "c" : "");

    std::vector<uint8_t> code(PS2_VU1_CODE_SIZE);
    uint64_t loadedHash = 0;
    uint64_t runs = 0, mismatchFull = 0, mismatchFast = 0, skipped = 0, interpBudget = 0, packets = 0;
    uint64_t poisonRuns = 0, poisonFast = 0, poisonInterp = 0, poisonFull = 0;
    uint64_t entryHist[8] = {};
    alignas(16) static uint8_t gsDummy[64];
    GS &gs = *reinterpret_cast<GS *>(gsDummy); // never dereferenced: packet sink is always set
    std::mt19937 rng(12345);
    int verboseBudget = 10;

    for (const std::string &path : files)
    {
        Snapshot base;
        if (!loadSnapshot(path, base))
        {
            ++skipped;
            continue;
        }
        if (base.hash != loadedHash)
        {
            char name[64];
            std::snprintf(name, sizeof(name), "/%016llx.vu1", (unsigned long long)base.hash);
            FILE *f = std::fopen((catalog + name).c_str(), "rb");
            if (!f || std::fread(code.data(), 1, code.size(), f) != code.size())
            {
                if (f)
                    std::fclose(f);
                ++skipped;
                continue;
            }
            std::fclose(f);
            loadedHash = base.hash;
        }
        const Vu1RecImage *image = vu1recFind(base.hash);
        if (!image || !image->hasEntry(base.pc))
        {
            ++skipped;
            continue;
        }
        if (base.pc < 0x80u)
            ++entryHist[base.pc >> 4];
        for (int variantIdx = 0; variantIdx <= fuzz; ++variantIdx)
        {
            Snapshot s = base;
            if (variantIdx > 0)
                perturb(s, rng);

            const Result ref = runInterp(code, s, gs);
            if (!ref.ended)
            {
                ++interpBudget;
                continue;
            }
            const Result full = runNative(image->full, s);
            const Result fast = runNative(image->fast, s);
            ++runs;
            packets += ref.pk.packets.size();
            const bool verbose = verboseBudget > 0;
            const int nf = compare("full", full, ref, kAllFlags, verbose);
            const int nq = compare("fast", fast, ref, liveExit, verbose);
            if (nf || nq)
            {
                if (verbose)
                {
                    std::printf("  MISMATCH %s variant %d (entry 0x%04x)\n", path.c_str(), variantIdx, s.pc);
                    --verboseBudget;
                }
                mismatchFull += nf ? 1 : 0;
                mismatchFast += nq ? 1 : 0;
            }
            // Poisoned input MAC/status: dead-at-exit flags must not be live into any entry.
            for (int k = 0; k < poison; ++k)
            {
                Snapshot sp = s;
                sp.state.mac = rng() & 0xFFFFu;
                sp.state.status = rng() & 0xFFFu;
                const Result ip = runInterp(code, sp, gs);
                const Result fp = runNative(image->fast, sp);
                const Result up = runNative(image->full, sp);
                ++poisonRuns;
                const unsigned observed = liveExit;
                const int a = compare("poison-interp", ip, ref, observed, verbose);
                const int b = compare("poison-fast", fp, ref, observed, verbose);
                const int c = compare("poison-full", up, ip, kAllFlags, verbose);
                if (a || b || c)
                {
                    if (verbose)
                    {
                        std::printf("  POISON MISMATCH %s variant %d (entry 0x%04x) mac=%04x status=%03x\n",
                                    path.c_str(), variantIdx, s.pc, sp.state.mac, sp.state.status);
                        --verboseBudget;
                    }
                    poisonInterp += a ? 1 : 0;
                    poisonFast += b ? 1 : 0;
                    poisonFull += c ? 1 : 0;
                }
            }
        }
    }
    std::printf("vu1_replay: entries 0x00..0x70:");
    for (int e = 0; e < 8; ++e)
        std::printf(" %llu", (unsigned long long)entryHist[e]);
    std::printf("\n");
    std::printf("vu1_replay: runs=%llu mismatch(full)=%llu mismatch(fast)=%llu interpreter-budget-stops=%llu "
                "skipped=%llu gif-packets=%llu\n",
                (unsigned long long)runs, (unsigned long long)mismatchFull, (unsigned long long)mismatchFast,
                (unsigned long long)interpBudget, (unsigned long long)skipped, (unsigned long long)packets);
    if (poison)
        std::printf("vu1_replay: poison runs=%llu mismatch(interp-vs-unpoisoned)=%llu mismatch(fast-vs-unpoisoned)=%llu "
                    "mismatch(full-vs-poisoned-interp)=%llu\n",
                    (unsigned long long)poisonRuns, (unsigned long long)poisonInterp, (unsigned long long)poisonFast,
                    (unsigned long long)poisonFull);
    return (mismatchFull || mismatchFast || poisonInterp || poisonFast || poisonFull) ? 1 : 0;
}
