#include "runtime/gs/gs_census_backend.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace
{
    const char *const kCounterNames[] = {
        "prims", "point", "line", "tri", "sprite", "tex", "tex_sprite", "untex_tri", "untex_sprite",
        "clut_loads", "uploads", "upload_bytes", "upload_same_dbp", "upload_same_dbp_newdata",
        "upload_after_sample", "upload_into_target", "l2l", "l2l_bytes", "l2l_from_target", "l2h",
        "rtt_inframe", "rtt_inframe_sprite", "rtt_older", "rtt_older_sprite", "z_as_tex",
        "texflush", "clears"};

    struct PageDims
    {
        uint32_t w, h;
    };

    PageDims pageDims(uint8_t psm)
    {
        switch (psm)
        {
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
        case GS_PSM_Z16:
        case GS_PSM_Z16S:
            return {64u, 64u};
        case GS_PSM_T8:
            return {128u, 64u};
        case GS_PSM_T4:
            return {128u, 128u};
        default:
            return {64u, 32u}; // 32-bit layouts, incl. T8H/T4HL/T4HH
        }
    }

    uint32_t bytesPerPixelTimes2(uint8_t psm)
    {
        switch (psm)
        {
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
        case GS_PSM_Z16:
        case GS_PSM_Z16S:
            return 4u;
        case GS_PSM_T8:
            return 2u;
        case GS_PSM_T4:
            return 1u;
        default:
            return 8u;
        }
    }

    uint64_t fnv1a(uint64_t h, const uint8_t *p, size_t n)
    {
        for (size_t i = 0; i < n; ++i)
        {
            h ^= p[i];
            h *= 1099511628211ull;
        }
        return h;
    }

    constexpr uint64_t kFnvBasis = 1469598103934665603ull;
}

const char *GSCensusBackend::PathFromEnvironment()
{
    const char *v = std::getenv("PS2X_GS_CENSUS");
    if (!v || v[0] == '\0' || (v[0] == '0' && v[1] == '\0'))
        return nullptr;
    if (v[0] == '1' && v[1] == '\0')
        return "gs_census.txt";
    return v;
}

GSCensusBackend::GSCensusBackend(std::unique_ptr<GSRasterBackend> inner, const char *path)
    : m_inner(std::move(inner))
{
    m_out = std::fopen(path, "w");
    if (m_out)
    {
        std::fprintf(m_out, "# gs census v1 (counts only). F lines: frame index then counters; K lines: key total frames\n");
        std::fprintf(m_out, "C");
        for (const char *n : kCounterNames)
            std::fprintf(m_out, " %s", n);
        std::fprintf(m_out, "\n");
        std::fflush(m_out);
    }
    std::fprintf(stderr, "[gs-census] writing %s (%s)\n", path, m_out ? "ok" : "FAILED");
}

GSCensusBackend::~GSCensusBackend()
{
    if (m_out)
    {
        WriteKeys("FINAL");
        std::fclose(m_out);
    }
}

void GSCensusBackend::Count(const std::string &key, uint64_t n)
{
    KeyCount &k = m_keys[key];
    k.total += n;
    if (k.lastFrame != m_frame)
    {
        k.lastFrame = m_frame;
        ++k.frames;
    }
}

void GSCensusBackend::BeginFrameIfNeeded() {}

void GSCensusBackend::TransferPages(uint32_t bp, uint32_t bw, uint8_t psm, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                    uint32_t *pages, uint32_t &count, uint32_t maxPages) const
{
    count = 0;
    if (w == 0u || h == 0u)
        return;
    const PageDims d = pageDims(psm);
    const uint32_t ppr = std::max<uint32_t>(1u, (std::max<uint32_t>(bw, 1u) * 64u) / d.w);
    const uint32_t base = bp >> 5;
    const bool offset = (bp & 31u) != 0u;
    const uint32_t px0 = x / d.w, px1 = (x + w - 1u) / d.w;
    const uint32_t py0 = y / d.h, py1 = (y + h - 1u) / d.h;
    for (uint32_t py = py0; py <= py1; ++py)
        for (uint32_t px = px0; px <= px1 + (offset ? 1u : 0u); ++px)
        {
            if (count >= maxPages)
                return;
            const uint32_t p = (base + py * ppr + px) % kPages;
            bool seen = false;
            for (uint32_t i = 0; i < count && !seen; ++i)
                seen = pages[i] == p;
            if (!seen)
                pages[count++] = p;
        }
}

void GSCensusBackend::Submit(const GSPrimitiveBatch &batch)
{
    m_inner->Submit(batch);
    if (!m_out || batch.vertexCount == 0u)
        return;

    const GSDrawState &st = batch.state;
    const GSContext &ctx = st.context;
    const GSPrimReg &prim = st.prim;
    char key[384];

    ++m_frameCounters[cPrims];
    const bool sprite = prim.type == GS_PRIM_SPRITE;
    const bool tri = prim.type == GS_PRIM_TRIANGLE || prim.type == GS_PRIM_TRISTRIP || prim.type == GS_PRIM_TRIFAN;
    if (sprite)
        ++m_frameCounters[cSprite];
    else if (tri)
        ++m_frameCounters[cTri];
    else if (prim.type == GS_PRIM_LINE || prim.type == GS_PRIM_LINESTRIP)
        ++m_frameCounters[cLine];
    else
        ++m_frameCounters[cPoint];

    const bool textured = prim.tme && (sprite || tri);
    if (textured)
    {
        ++m_frameCounters[cTextured];
        if (sprite)
            ++m_frameCounters[cTexturedSprite];
    }
    else if (sprite)
        ++m_frameCounters[cUntexturedSprite];
    else if (tri)
        ++m_frameCounters[cUntexturedTri];

    std::snprintf(key, sizeof(key), "prim type=%u tme=%d iip=%d fge=%d abe=%d aa1=%d fst=%d ctxt=%d",
                  static_cast<unsigned>(prim.type), prim.tme, prim.iip, prim.fge, prim.abe, prim.aa1, prim.fst, prim.ctxt);
    Count(key);
    std::snprintf(key, sizeof(key), "frame fpsm=%u fbw=%u fbp=%u fbmsk=%08x", ctx.frame.psm, ctx.frame.fbw, ctx.frame.fbp, ctx.frame.fbmsk);
    Count(key);
    std::snprintf(key, sizeof(key), "zbuf zpsm=%u zbp=%u zmsk=%d", ctx.zbuf.psm, ctx.zbuf.zbp, ctx.zbuf.zmask);
    Count(key);
    const uint32_t a = static_cast<uint32_t>(ctx.alpha & 0xFFu);
    if (prim.abe)
    {
        if (((a >> 4) & 3u) == 2u)
            std::snprintf(key, sizeof(key), "alpha a=%u b=%u c=%u d=%u fix=%u", a & 3u, (a >> 2) & 3u, (a >> 4) & 3u, (a >> 6) & 3u,
                          static_cast<unsigned>((ctx.alpha >> 32) & 0xFFu));
        else
            std::snprintf(key, sizeof(key), "alpha a=%u b=%u c=%u d=%u", a & 3u, (a >> 2) & 3u, (a >> 4) & 3u, (a >> 6) & 3u);
        Count(key);
    }
    else
        Count("alpha abe=0");
    const uint64_t t = ctx.test;
    std::snprintf(key, sizeof(key), "test ate=%u atst=%u aref=%u afail=%u date=%u datm=%u zte=%u ztst=%u",
                  static_cast<unsigned>(t & 1u), static_cast<unsigned>((t >> 1) & 7u), static_cast<unsigned>((t >> 4) & 0xFFu),
                  static_cast<unsigned>((t >> 12) & 3u), static_cast<unsigned>((t >> 14) & 1u), static_cast<unsigned>((t >> 15) & 1u),
                  static_cast<unsigned>((t >> 16) & 1u), static_cast<unsigned>((t >> 17) & 3u));
    Count(key);
    std::snprintf(key, sizeof(key), "misc fba=%u pabe=%d dthe=%u colclamp=%u scanmsk=%u",
                  static_cast<unsigned>(ctx.fba & 1u), st.pabe, static_cast<unsigned>(st.dthe & 1u),
                  static_cast<unsigned>(st.colclamp & 1u), static_cast<unsigned>(st.scanmsk & 3u));
    Count(key);
    std::snprintf(key, sizeof(key), "xyoff fbp=%u ofx=%u ofy=%u scissor=%u,%u,%u,%u", ctx.frame.fbp, ctx.xyoffset.ofx, ctx.xyoffset.ofy,
                  ctx.scissor.x0, ctx.scissor.y0, ctx.scissor.x1, ctx.scissor.y1);
    Count(key);

    // Pages this primitive can write (vertex bbox clamped to the scissor).
    float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
    const uint32_t vc = std::min<uint32_t>(batch.vertexCount, sprite ? 2u : 3u);
    for (uint32_t i = 0; i < vc; ++i)
    {
        minX = std::min(minX, batch.vertices[i].x);
        maxX = std::max(maxX, batch.vertices[i].x);
        minY = std::min(minY, batch.vertices[i].y);
        maxY = std::max(maxY, batch.vertices[i].y);
    }
    const int ofx = ctx.xyoffset.ofx >> 4, ofy = ctx.xyoffset.ofy >> 4;
    const int x0 = std::clamp(static_cast<int>(std::floor(minX)) - ofx, static_cast<int>(ctx.scissor.x0), static_cast<int>(ctx.scissor.x1));
    const int x1 = std::clamp(static_cast<int>(std::ceil(maxX)) - ofx, static_cast<int>(ctx.scissor.x0), static_cast<int>(ctx.scissor.x1));
    const int y0 = std::clamp(static_cast<int>(std::floor(minY)) - ofy, static_cast<int>(ctx.scissor.y0), static_cast<int>(ctx.scissor.y1));
    const int y1 = std::clamp(static_cast<int>(std::ceil(maxY)) - ofy, static_cast<int>(ctx.scissor.y0), static_cast<int>(ctx.scissor.y1));

    // Texture read: render-to-texture and Z-as-texture against earlier draw writes.
    if (textured)
    {
        const GSTex0Reg &tex = ctx.tex0;
        std::snprintf(key, sizeof(key), "tex tpsm=%u cpsm=%u csm=%u tfx=%u tcc=%u tw=%u th=%u lin=%d", tex.psm, tex.cpsm, tex.csm, tex.tfx,
                      tex.tcc, tex.tw, tex.th, st.linearFilter);
        Count(key);
        std::snprintf(key, sizeof(key), "texwrap wms=%u wmt=%u", static_cast<unsigned>(ctx.clamp & 3u),
                      static_cast<unsigned>((ctx.clamp >> 2) & 3u));
        Count(key);
        std::snprintf(key, sizeof(key), "tex1 lcm=%u mxl=%u mmag=%u mmin=%u", static_cast<unsigned>(ctx.tex1 & 1u),
                      static_cast<unsigned>((ctx.tex1 >> 2) & 7u), static_cast<unsigned>((ctx.tex1 >> 5) & 1u),
                      static_cast<unsigned>((ctx.tex1 >> 6) & 7u));
        Count(key);

        // Sampled texel rectangle (vertex texel bbox, +1 for bilinear; full axis for repeat modes).
        const int texW = std::max<int>(1, st.textureWidth), texH = std::max<int>(1, st.textureHeight);
        float u0 = 1e30f, u1 = -1e30f, v0 = 1e30f, v1 = -1e30f;
        bool finite = true;
        for (uint32_t i = 0; i < vc; ++i)
        {
            const GSVertex &v = batch.vertices[i];
            float u, w;
            if (prim.fst)
            {
                u = static_cast<float>(v.u) / 16.0f;
                w = static_cast<float>(v.v) / 16.0f;
            }
            else
            {
                const float q = v.q;
                u = v.s / q * static_cast<float>(texW);
                w = v.t / q * static_cast<float>(texH);
            }
            if (!std::isfinite(u) || !std::isfinite(w))
                finite = false;
            u0 = std::min(u0, u);
            u1 = std::max(u1, u);
            v0 = std::min(v0, w);
            v1 = std::max(v1, w);
        }
        const auto axis = [&](uint32_t mode, float lo, float hi, int size, uint32_t rmin, uint32_t rmax, int &outLo, int &outHi)
        {
            if (!finite || mode == 3u)
            {
                outLo = 0;
                outHi = size - 1;
                return;
            }
            int l = static_cast<int>(std::floor(lo)) - 1, h = static_cast<int>(std::ceil(hi)) + 1;
            if (mode == 0u && (l < 0 || h >= size))
            {
                outLo = 0;
                outHi = size - 1;
                return;
            }
            const int cl = mode == 2u ? static_cast<int>(rmin) : 0;
            const int ch = mode == 2u ? static_cast<int>(rmax) : size - 1;
            outLo = std::clamp(l, cl, std::max(cl, ch));
            outHi = std::clamp(h, cl, std::max(cl, ch));
        };
        int tu0, tu1, tv0, tv1;
        axis(static_cast<uint32_t>(ctx.clamp & 3u), u0, u1, texW, static_cast<uint32_t>((ctx.clamp >> 4) & 0x3FFu),
             static_cast<uint32_t>((ctx.clamp >> 14) & 0x3FFu), tu0, tu1);
        axis(static_cast<uint32_t>((ctx.clamp >> 2) & 3u), v0, v1, texH, static_cast<uint32_t>((ctx.clamp >> 24) & 0x3FFu),
             static_cast<uint32_t>((ctx.clamp >> 34) & 0x3FFu), tv0, tv1);

        uint32_t pages[256];
        uint32_t n = 0;
        TransferPages(tex.tbp0, tex.tbw, tex.psm, static_cast<uint32_t>(tu0), static_cast<uint32_t>(tv0),
                      static_cast<uint32_t>(tu1 - tu0 + 1), static_cast<uint32_t>(tv1 - tv0 + 1), pages, n, 256u);
        bool inFrame = false, prevFrame = false, fromZ = false;
        uint64_t minAge = ~0ull;
        for (uint32_t i = 0; i < n; ++i)
        {
            PageState &p = m_pages[pages[i]];
            if (p.drawnAfterUpload && p.drawFrame != ~0ull)
            {
                const uint64_t age = m_drawFrame - p.drawFrame;
                minAge = std::min(minAge, age);
                if (age == 0u)
                    inFrame = true;
                else
                    prevFrame = true;
                if (p.lastDrawWasZ)
                    fromZ = true;
            }
            p.sampleFrame = m_frame;
        }
        if (inFrame)
            prevFrame = false;
        // "core" texel rectangle: the vertex texel bbox without the bilinear/rounding margin
        uint64_t coreAge = ~0ull;
        bool coreZ = false;
        if (finite && (inFrame || prevFrame))
        {
            const int cu0 = std::clamp(static_cast<int>(std::floor(u0)), 0, texW - 1), cu1 = std::clamp(static_cast<int>(std::ceil(u1)) - 1, 0, texW - 1);
            const int cv0 = std::clamp(static_cast<int>(std::floor(v0)), 0, texH - 1), cv1 = std::clamp(static_cast<int>(std::ceil(v1)) - 1, 0, texH - 1);
            uint32_t cpages[256];
            uint32_t cn = 0;
            TransferPages(tex.tbp0, tex.tbw, tex.psm, static_cast<uint32_t>(std::min(cu0, cu1)), static_cast<uint32_t>(std::min(cv0, cv1)),
                          static_cast<uint32_t>(std::abs(cu1 - cu0) + 1), static_cast<uint32_t>(std::abs(cv1 - cv0) + 1), cpages, cn, 256u);
            for (uint32_t i = 0; i < cn; ++i)
            {
                const PageState &p = m_pages[cpages[i]];
                if (p.drawnAfterUpload && p.drawFrame != ~0ull)
                {
                    coreAge = std::min(coreAge, m_drawFrame - p.drawFrame);
                    coreZ |= p.lastDrawWasZ;
                }
            }
        }
        if (inFrame)
        {
            ++m_frameCounters[cRttInFrame];
            if (sprite)
                ++m_frameCounters[cRttInFrameSprite];
        }
        else if (prevFrame)
        {
            ++m_frameCounters[cRttPrevFrame];
            if (sprite)
                ++m_frameCounters[cRttPrevFrameSprite];
        }
        if (fromZ)
            ++m_frameCounters[cZAsTexture];
        if (inFrame || prevFrame)
        {
            const uint32_t fbBlock = ctx.frame.fbp << 5;
            const char *age = minAge == 0u ? "0" : minAge == 1u ? "1" : minAge == 2u ? "2" : minAge < 10u ? "3-9" : "10+";
            // where the texture starts relative to the draw's own frame buffer and Z buffer (block deltas, not addresses)
            const int64_t dF = static_cast<int64_t>(tex.tbp0) - static_cast<int64_t>(fbBlock);
            const int64_t dZ = static_cast<int64_t>(tex.tbp0) - static_cast<int64_t>(ctx.zbuf.zbp << 5);
            char rel[64];
            if (dF >= -64 && dF <= 2048)
                std::snprintf(rel, sizeof(rel), "fb%+lld", static_cast<long long>(dF));
            else if (dZ >= -64 && dZ <= 2048)
                std::snprintf(rel, sizeof(rel), "zb%+lld", static_cast<long long>(dZ));
            else
                std::snprintf(rel, sizeof(rel), "other");
            int uvdx = 0, uvdy = 0;
            if (sprite && prim.fst)
            {
                uvdx = static_cast<int>(batch.vertices[0].u >> 4) - (static_cast<int>(batch.vertices[0].x) - ofx);
                uvdy = static_cast<int>(batch.vertices[0].v >> 4) - (static_cast<int>(batch.vertices[0].y) - ofy);
            }
            const char *cage = coreAge == ~0ull ? "none" : coreAge == 0u ? "0" : coreAge == 1u ? "1" : coreAge == 2u ? "2" : coreAge < 10u ? "3-9" : "10+";
            std::snprintf(key, sizeof(key), "rtt age=%s core_age=%s core_writer=%s prim=%u tpsm=%u tw=%u th=%u tbw=%u fpsm=%u src=%s fst=%d uv_minus_xy=%d,%d writer=%s pages=%u",
                          age, cage, coreZ ? "z" : "frame", static_cast<unsigned>(prim.type), tex.psm, tex.tw, tex.th, tex.tbw,
                          ctx.frame.psm, rel, prim.fst, uvdx, uvdy, fromZ ? "z" : "frame", n);
            Count(key);
        }
    }

    // Record the draw's frame and Z page writes.
    if (x1 >= x0 && y1 >= y0)
    {
        uint32_t pages[256];
        uint32_t n = 0;
        TransferPages(ctx.frame.fbp << 5, ctx.frame.fbw, ctx.frame.psm, static_cast<uint32_t>(x0), static_cast<uint32_t>(y0),
                      static_cast<uint32_t>(x1 - x0 + 1), static_cast<uint32_t>(y1 - y0 + 1), pages, n, 256u);
        for (uint32_t i = 0; i < n; ++i)
        {
            PageState &p = m_pages[pages[i]];
            p.drawFrame = m_drawFrame;
            p.lastDrawWasZ = false;
            p.drawnAfterUpload = true;
        }
        const uint32_t ztst = static_cast<uint32_t>((t >> 17) & 3u);
        if (!ctx.zbuf.zmask && ztst != 0u)
        {
            TransferPages(ctx.zbuf.zbp << 5, ctx.frame.fbw, ctx.zbuf.psm, static_cast<uint32_t>(x0), static_cast<uint32_t>(y0),
                          static_cast<uint32_t>(x1 - x0 + 1), static_cast<uint32_t>(y1 - y0 + 1), pages, n, 256u);
            for (uint32_t i = 0; i < n; ++i)
            {
                PageState &p = m_pages[pages[i]];
                p.drawFrame = m_drawFrame;
                p.lastDrawWasZ = true;
                p.drawnAfterUpload = true;
            }
        }
    }
}

void GSCensusBackend::LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut)
{
    m_inner->LoadClut(tex0, texclut);
    if (!m_out)
        return;
    ++m_frameCounters[cClutLoads];
    char key[128];
    std::snprintf(key, sizeof(key), "clut cld=%u cpsm=%u csm=%u tpsm=%u", tex0.cld, tex0.cpsm, tex0.csm, tex0.psm);
    Count(key);
}

void GSCensusBackend::BeginTransfer(const GSTransferCommand &command)
{
    m_inner->BeginTransfer(command);
    if (!m_out)
        return;
    char key[160];
    const GSBitBltBuf &b = command.bitbltbuf;
    if (m_transferActive)
    {
        // finalise the previous host->local transfer's content hash
        auto it = m_frameUploadHashByDbp.find(m_transfer.bitbltbuf.dbp);
        if (it != m_frameUploadHashByDbp.end() && it->second != 0u && it->second != m_transferHash)
            ++m_frameCounters[cUploadSameDbpNewData];
        m_frameUploadHashByDbp[m_transfer.bitbltbuf.dbp] = m_transferHash;
        m_transferActive = false;
    }
    if (command.direction == 0u)
    {
        ++m_frameCounters[cUploads];
        std::snprintf(key, sizeof(key), "xfer h2l dpsm=%u dbw=%u w=%u h=%u", b.dpsm, b.dbw, command.trxreg.rrw, command.trxreg.rrh);
        Count(key);
        if (m_frameUploadHashByDbp.count(b.dbp))
            ++m_frameCounters[cUploadSameDbpTwice];
        uint32_t pages[512];
        uint32_t n = 0;
        TransferPages(b.dbp, b.dbw, b.dpsm, command.trxpos.dsax, command.trxpos.dsay, command.trxreg.rrw, command.trxreg.rrh, pages, n, 512u);
        bool afterSample = false, intoTarget = false;
        for (uint32_t i = 0; i < n; ++i)
        {
            PageState &p = m_pages[pages[i]];
            afterSample |= p.sampleFrame == m_frame;
            intoTarget |= p.drawnAfterUpload && p.drawFrame == m_drawFrame;
            p.uploadFrame = m_frame;
            p.drawnAfterUpload = false;
        }
        if (afterSample)
            ++m_frameCounters[cUploadAfterSample];
        if (intoTarget)
            ++m_frameCounters[cUploadIntoTarget];
        m_transfer = command;
        m_transferActive = true;
        m_transferHash = kFnvBasis;
        m_transferBytes = 0;
    }
    else if (command.direction == 2u)
    {
        ++m_frameCounters[cLocalToLocal];
        const uint64_t bytes = static_cast<uint64_t>(command.trxreg.rrw) * command.trxreg.rrh * bytesPerPixelTimes2(b.spsm) / 8u;
        m_frameCounters[cLocalToLocalBytes] += bytes;
        std::snprintf(key, sizeof(key), "xfer l2l spsm=%u dpsm=%u sbw=%u dbw=%u w=%u h=%u sbp_eq_dbp=%d", b.spsm, b.dpsm, b.sbw, b.dbw,
                      command.trxreg.rrw, command.trxreg.rrh, b.sbp == b.dbp);
        Count(key);
        uint32_t pages[512];
        uint32_t n = 0;
        TransferPages(b.sbp, b.sbw, b.spsm, command.trxpos.ssax, command.trxpos.ssay, command.trxreg.rrw, command.trxreg.rrh, pages, n, 512u);
        bool fromTarget = false;
        for (uint32_t i = 0; i < n; ++i)
        {
            const PageState &p = m_pages[pages[i]];
            fromTarget |= p.drawnAfterUpload && p.drawFrame != ~0ull;
        }
        if (fromTarget)
            ++m_frameCounters[cLocalToLocalFromTarget];
        TransferPages(b.dbp, b.dbw, b.dpsm, command.trxpos.dsax, command.trxpos.dsay, command.trxreg.rrw, command.trxreg.rrh, pages, n, 512u);
        for (uint32_t i = 0; i < n; ++i)
        {
            m_pages[pages[i]].uploadFrame = m_frame;
            m_pages[pages[i]].drawnAfterUpload = false;
        }
    }
    else if (command.direction == 1u)
    {
        ++m_frameCounters[cLocalToHost];
        std::snprintf(key, sizeof(key), "xfer l2h spsm=%u w=%u h=%u", b.spsm, command.trxreg.rrw, command.trxreg.rrh);
        Count(key);
    }
}

void GSCensusBackend::UploadImage(const uint8_t *data, uint32_t sizeBytes)
{
    m_inner->UploadImage(data, sizeBytes);
    if (!m_out || !data)
        return;
    m_frameCounters[cUploadBytes] += sizeBytes;
    if (m_transferActive)
    {
        m_transferHash = fnv1a(m_transferHash, data, sizeBytes);
        m_transferBytes += sizeBytes;
    }
}

void GSCensusBackend::TextureFlush()
{
    m_inner->TextureFlush();
    if (m_out)
        ++m_frameCounters[cTexFlush];
}

bool GSCensusBackend::ClearFramebuffer(const GSContext &context, uint32_t rgba)
{
    if (m_out)
        ++m_frameCounters[cClears];
    return m_inner->ClearFramebuffer(context, rgba);
}

PresentationFrame GSCensusBackend::Present(const GSPresentationRequest &request)
{
    if (m_out)
    {
        char key[160];
        std::snprintf(key, sizeof(key), "present pmode=%llx smode2=%llx dispfb1=%llx dispfb2=%llx",
                      static_cast<unsigned long long>(request.pmode & 0xFFFFull), static_cast<unsigned long long>(request.smode2 & 3ull),
                      static_cast<unsigned long long>(request.dispfb1 & 0x7FFFFFFFull),
                      static_cast<unsigned long long>(request.dispfb2 & 0x7FFFFFFFull));
        Count(key);
    }
    PresentationFrame frame = m_inner->Present(request);
    EndFrame();
    return frame;
}

bool GSCensusBackend::PresentAsync(const GSPresentationRequest &request, PresentCallback done)
{
    // Only a wrapper that queues (GSThreadedBackend) returns true; the census sits inside it.
    return m_inner->PresentAsync(request, std::move(done));
}

void GSCensusBackend::EndFrame()
{
    if (!m_out)
        return;
    if (m_transferActive)
    {
        auto it = m_frameUploadHashByDbp.find(m_transfer.bitbltbuf.dbp);
        if (it != m_frameUploadHashByDbp.end() && it->second != 0u && it->second != m_transferHash)
            ++m_frameCounters[cUploadSameDbpNewData];
        m_transferActive = false;
    }
    std::fprintf(m_out, "F %llu", static_cast<unsigned long long>(m_frame));
    for (uint64_t c : m_frameCounters)
        std::fprintf(m_out, " %llu", static_cast<unsigned long long>(c));
    std::fprintf(m_out, "\n");
    std::fflush(m_out);
    if (m_frameCounters[cPrims] != 0u)
        ++m_drawFrame;
    m_frameCounters.fill(0u);
    m_frameUploadHashByDbp.clear();
    ++m_frame;
    if ((m_frame % 100u) == 0u)
        WriteKeys("BLOCK");
}

void GSCensusBackend::WriteKeys(const char *tag)
{
    std::fprintf(m_out, "BEGIN %s %llu\n", tag, static_cast<unsigned long long>(m_frame));
    for (const auto &[k, v] : m_keys)
        std::fprintf(m_out, "K %llu %llu %s\n", static_cast<unsigned long long>(v.total), static_cast<unsigned long long>(v.frames), k.c_str());
    std::fprintf(m_out, "END %s %llu\n", tag, static_cast<unsigned long long>(m_frame));
    std::fflush(m_out);
}
