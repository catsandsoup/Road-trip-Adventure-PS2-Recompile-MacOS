#include "runtime/gs/gs_tee_backend.h"

#if defined(__APPLE__)

#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/gs_metal_backend.h"
#include "runtime/gs/ps2_gs_memory.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

void GSTeeBackend::RunTotals::Add(const RunTotals &o)
{
    runs += o.runs;
    mismatchRuns += o.mismatchRuns;
    px += o.px;
    pxFrameMatch += o.pxFrameMatch;
    pxZMatch += o.pxZMatch;
    pxBothMatch += o.pxBothMatch;
    maxChannelDiff = std::max(maxChannelDiff, o.maxChannelDiff);
}

GSTeeBackend::GSTeeBackend(std::unique_ptr<GSCpuBackend> oracle, std::unique_ptr<GSMetalBackend> metal)
    : m_oracle(std::move(oracle)), m_metal(std::move(metal))
{
    if (const char *v = std::getenv("PS2X_GS_TEE_RESYNC"))
    {
        if (std::strcmp(v, "none") == 0)
            m_resync = Resync::None;
        else if (std::strcmp(v, "run") == 0)
            m_resync = Resync::Run;
    }
    if (const char *v = std::getenv("PS2X_GS_TEE_LOG"))
        m_log = std::fopen(v, "w");
    if (const char *v = std::getenv("PS2X_GS_TEE_DIR"))
        m_dumpDir = v;
    if (const char *v = std::getenv("PS2X_GS_TEE_EVERY"))
        m_dumpEvery = static_cast<uint32_t>(std::max(0, std::atoi(v)));
    if (const char *v = std::getenv("PS2X_GS_TEE_MAX_MISMATCH_DUMPS"))
        m_maxMismatchDumps = static_cast<uint32_t>(std::max(0, std::atoi(v)));
    m_metal->SetRunObserver([this](const GSMetalBackend::RunInfo &r)
                            { OnRun(r.fbp, r.zbp, r.fbw, r.x0, r.y0, r.x1, r.y1); });
    if (const char *v = std::getenv("PS2X_GS_METAL_SELFTEST"))
        m_metal->SelfTest(static_cast<uint32_t>(std::max(1024, std::atoi(v))));
}

GSTeeBackend::~GSTeeBackend()
{
    std::FILE *out = m_log ? m_log : stderr;
    const GSMetalBackend::Stats &s = m_metal->TotalStats();
    std::fprintf(out,
                 "[gs-tee] TOTAL frames=%llu frames_exact=%llu frame_px_match=%.6f submits=%llu metal=%llu metal_frac=%.4f runs=%llu "
                 "mismatch_runs=%llu run_px=%llu run_px_match=%.6f run_frame_match=%.6f run_z_match=%.6f max_diff=%u\n",
                 (unsigned long long)m_framesCompared, (unsigned long long)m_framesExact,
                 m_framePx ? double(m_framePxMatch) / double(m_framePx) : 1.0, (unsigned long long)s.submits,
                 (unsigned long long)s.metalPrims, s.submits ? double(s.metalPrims) / double(s.submits) : 0.0,
                 (unsigned long long)m_totalRuns.runs, (unsigned long long)m_totalRuns.mismatchRuns, (unsigned long long)m_totalRuns.px,
                 m_totalRuns.px ? double(m_totalRuns.pxBothMatch) / double(m_totalRuns.px) : 1.0,
                 m_totalRuns.px ? double(m_totalRuns.pxFrameMatch) / double(m_totalRuns.px) : 1.0,
                 m_totalRuns.px ? double(m_totalRuns.pxZMatch) / double(m_totalRuns.px) : 1.0, m_totalRuns.maxChannelDiff);
    for (const auto &[k, v] : s.fallbacks)
        std::fprintf(out, "[gs-tee] TOTAL fallback %s=%llu\n", k.c_str(), (unsigned long long)v);
    std::fflush(out);
    if (m_log)
        std::fclose(m_log);
}

void GSTeeBackend::Initialize(uint8_t *vram, uint32_t vramSize)
{
    m_vram = vram;
    m_vramSize = vramSize;
    m_oracle->Initialize(vram, vramSize);
    m_shadow.assign(vramSize, 0u);
    if (vram)
        std::memcpy(m_shadow.data(), vram, vramSize);
    m_metal->Initialize(vram ? m_shadow.data() : nullptr, vramSize);
}

void GSTeeBackend::Reset()
{
    m_metal->Reset();
    m_oracle->Reset();
}

void GSTeeBackend::Submit(const GSPrimitiveBatch &batch)
{
    m_metal->Submit(batch);
    m_oracle->Submit(batch);
}

void GSTeeBackend::LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut)
{
    m_metal->LoadClut(tex0, texclut);
    m_oracle->LoadClut(tex0, texclut);
}

void GSTeeBackend::BeginTransfer(const GSTransferCommand &command)
{
    m_metal->BeginTransfer(command);
    m_oracle->BeginTransfer(command);
}

void GSTeeBackend::UploadImage(const uint8_t *data, uint32_t sizeBytes)
{
    m_metal->UploadImage(data, sizeBytes);
    m_oracle->UploadImage(data, sizeBytes);
}

void GSTeeBackend::Flush()
{
    m_metal->Flush();
    m_oracle->Flush();
}

void GSTeeBackend::TextureFlush()
{
    m_metal->TextureFlush();
    m_oracle->TextureFlush();
}

void GSTeeBackend::Sync(GSSyncReason reason)
{
    m_metal->Sync(reason);
    m_oracle->Sync(reason);
}

void GSTeeBackend::OnRun(uint32_t fbp, uint32_t zbp, uint32_t fbw, int x0, int y0, int x1, int y1)
{
    if (!m_vram)
        return;
    const GSMem::SwizzledSurface<GSMem::C32> cs(fbp << 5, fbw);
    const GSMem::SwizzledSurface<GSMem::Z24> zs(zbp << 5, fbw);
    RunTotals r;
    r.runs = 1;
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x)
        {
            const uint32_t ca = cs.Locate(uint32_t(x), uint32_t(y)).byteAddress;
            const uint32_t za = zs.Locate(uint32_t(x), uint32_t(y)).byteAddress;
            uint32_t fm, fo, zm, zo;
            std::memcpy(&fm, m_shadow.data() + ca, 4);
            std::memcpy(&fo, m_vram + ca, 4);
            std::memcpy(&zm, m_shadow.data() + za, 4);
            std::memcpy(&zo, m_vram + za, 4);
            ++r.px;
            const bool fOk = fm == fo, zOk = zm == zo;
            r.pxFrameMatch += fOk;
            r.pxZMatch += zOk;
            r.pxBothMatch += (fOk && zOk);
            if (!fOk)
                for (int c = 0; c < 4; ++c)
                {
                    const int d = std::abs(int((fm >> (8 * c)) & 0xFFu) - int((fo >> (8 * c)) & 0xFFu));
                    r.maxChannelDiff = std::max<uint32_t>(r.maxChannelDiff, uint32_t(d));
                }
        }
    if (r.pxBothMatch != r.px)
    {
        r.mismatchRuns = 1;
        if (m_resync == Resync::Run)
            Resync();
    }
    m_frameRuns.Add(r);
}

void GSTeeBackend::Resync()
{
    // Called from inside the Metal backend's run flush (observer) or between commands: the Metal
    // backend has no open run at either point.
    std::memcpy(m_shadow.data(), m_vram, m_vramSize);
    m_metal->MarkShadowChanged();
}

void GSTeeBackend::WritePpm(const std::string &path, const PresentationFrame &f) const
{
    std::FILE *fp = std::fopen(path.c_str(), "wb");
    if (!fp)
        return;
    std::fprintf(fp, "P6\n%u %u\n255\n", f.width, f.height);
    std::vector<uint8_t> row(size_t(f.width) * 3u);
    for (uint32_t y = 0; y < f.height; ++y)
    {
        for (uint32_t x = 0; x < f.width; ++x)
            std::memcpy(&row[size_t(x) * 3u], &f.pixels[(size_t(y) * f.width + x) * 4u], 3);
        std::fwrite(row.data(), 1, row.size(), fp);
    }
    std::fclose(fp);
}

PresentationFrame GSTeeBackend::Present(const GSPresentationRequest &request)
{
    PresentationFrame mf = m_metal->Present(request);
    PresentationFrame cf = m_oracle->Present(request);
    const GSMetalBackend::Stats s = m_metal->TakeFrameStats();

    uint64_t px = 0, match = 0;
    uint32_t maxDiff = 0;
    const bool comparable = static_cast<bool>(cf) && cf.width == mf.width && cf.height == mf.height && cf.pixels.size() == mf.pixels.size();
    if (comparable)
    {
        px = uint64_t(cf.width) * cf.height;
        for (uint64_t i = 0; i < px; ++i)
        {
            const uint8_t *a = &cf.pixels[i * 4u];
            const uint8_t *b = &mf.pixels[i * 4u];
            if (std::memcmp(a, b, 4) == 0)
                ++match;
            else
                for (int c = 0; c < 4; ++c)
                    maxDiff = std::max<uint32_t>(maxDiff, uint32_t(std::abs(int(a[c]) - int(b[c]))));
        }
    }
    if (comparable)
        ++m_framesCompared;
    if (comparable && match == px)
        ++m_framesExact;
    m_framePx += px;
    m_framePxMatch += match;

    std::FILE *out = m_log ? m_log : stderr;
    std::fprintf(out,
                 "[gs-tee] frame=%llu submits=%llu metal=%llu fallback=%llu metal_frac=%.4f runs=%llu run_px=%llu run_px_match=%llu "
                 "run_frame_match=%llu run_z_match=%llu run_max_diff=%u present_px=%llu present_match=%llu present_max_diff=%u%s\n",
                 (unsigned long long)m_frame, (unsigned long long)s.submits, (unsigned long long)s.metalPrims,
                 (unsigned long long)s.fallbackPrims, s.submits ? double(s.metalPrims) / double(s.submits) : 0.0,
                 (unsigned long long)m_frameRuns.runs, (unsigned long long)m_frameRuns.px, (unsigned long long)m_frameRuns.pxBothMatch,
                 (unsigned long long)m_frameRuns.pxFrameMatch, (unsigned long long)m_frameRuns.pxZMatch, m_frameRuns.maxChannelDiff,
                 (unsigned long long)px, (unsigned long long)match, maxDiff, comparable ? "" : " size_mismatch");
    if ((m_frame % 100u) == 99u)
    {
        // cumulative fallback reasons (the destructor's TOTAL lines do not run when a test run ends on SIGALRM)
        const GSMetalBackend::Stats &t = m_metal->TotalStats();
        std::fprintf(out, "[gs-tee] CUM frame=%llu submits=%llu metal=%llu fallback=%llu", (unsigned long long)m_frame,
                     (unsigned long long)t.submits, (unsigned long long)t.metalPrims, (unsigned long long)t.fallbackPrims);
        for (const auto &[k, v] : t.fallbacks)
            std::fprintf(out, " %s=%llu", k.c_str(), (unsigned long long)v);
        std::fprintf(out, "\n");
    }
    std::fflush(out);

    if (!m_dumpDir.empty() && comparable)
    {
        const bool mismatch = match != px;
        const bool periodic = m_dumpEvery != 0u && (m_frame % m_dumpEvery) == 0u;
        const bool extra = mismatch && m_mismatchDumps < m_maxMismatchDumps;
        if (periodic || extra)
        {
            if (extra && !periodic)
                ++m_mismatchDumps;
            char name[64];
            std::snprintf(name, sizeof(name), "/f%06llu_cpu.ppm", (unsigned long long)m_frame);
            WritePpm(m_dumpDir + name, cf);
            std::snprintf(name, sizeof(name), "/f%06llu_mtl.ppm", (unsigned long long)m_frame);
            WritePpm(m_dumpDir + name, mf);
        }
    }

    m_totalRuns.Add(m_frameRuns);
    m_frameRuns = RunTotals{};
    ++m_frame;
    if (m_resync != Resync::None)
        Resync();
    return cf;
}

bool GSTeeBackend::ClearFramebuffer(const GSContext &context, uint32_t rgba)
{
    m_metal->ClearFramebuffer(context, rgba);
    return m_oracle->ClearFramebuffer(context, rgba);
}

uint32_t GSTeeBackend::ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes)
{
    std::vector<uint8_t> scratch(maxBytes);
    m_metal->ConsumeLocalToHostBytes(scratch.data(), maxBytes);
    return m_oracle->ConsumeLocalToHostBytes(dst, maxBytes);
}

uint32_t GSTeeBackend::ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const
{
    m_metal->FlushRun();
    return m_oracle->ReadVram(psm, base, bw, x, y);
}

void GSTeeBackend::WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    m_metal->WriteVram(psm, base, bw, x, y, value);
    m_oracle->WriteVram(psm, base, bw, x, y, value);
}

void GSTeeBackend::SnapshotVram(std::vector<uint8_t> &out) const
{
    m_metal->FlushRun();
    m_oracle->SnapshotVram(out);
}

GSTransferSnapshot GSTeeBackend::GetTransferSnapshot() const
{
    m_metal->FlushRun();
    return m_oracle->GetTransferSnapshot();
}

#endif
