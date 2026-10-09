#pragma once

// GSTeeBackend (Metal plan M1, PS2X_GS_BACKEND=tee): feeds one command stream to two backends.
//
// - The oracle (GSCpuBackend) owns the frontend's VRAM; every readback and the presented frame come
//   from it, so the game sees exactly the CPU backend.
// - The Metal backend works on a private 4 MB copy of VRAM and receives the identical commands
//   (Metal first, then the oracle, so a Metal run that is closed by command N is compared after the
//   oracle has executed commands 0..N-1, i.e. exactly the same draws).
// - Per Metal run: raw frame and Z words in the run's dirty rectangle are compared against the
//   oracle (exact-pixel match on the Metal-handled draws).
// - Per presented frame: the two RGBA frames are compared (exact-pixel %, max channel difference),
//   a line is logged, and optionally PPM pairs are written for game/tests/ssim.py.
// - Resync policy (PS2X_GS_TEE_RESYNC): "frame" (default) copies the oracle VRAM into the Metal copy
//   after each present, so every frame measures only its own draws; "run" also resyncs after any
//   mismatching run; "none" lets divergence accumulate.
// Env: PS2X_GS_TEE_LOG=<file> (default stderr), PS2X_GS_TEE_DIR=<dir> (PPM dumps),
//      PS2X_GS_TEE_EVERY=<n> (dump every n-th frame, default 50; mismatching frames are always dumped
//      up to PS2X_GS_TEE_MAX_MISMATCH_DUMPS, default 20).

#include "runtime/gs/gs_backend.h"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

class GSCpuBackend;
class GSMetalBackend;

class GSTeeBackend final : public GSRasterBackend
{
public:
    GSTeeBackend(std::unique_ptr<GSCpuBackend> oracle, std::unique_ptr<GSMetalBackend> metal);
    ~GSTeeBackend() override;

    void Initialize(uint8_t *vram, uint32_t vramSize) override;
    void Reset() override;

    void Submit(const GSPrimitiveBatch &batch) override;
    void LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut) override;
    void BeginTransfer(const GSTransferCommand &command) override;
    void UploadImage(const uint8_t *data, uint32_t sizeBytes) override;

    void Flush() override;
    void TextureFlush() override;
    void Sync(GSSyncReason reason) override;
    PresentationFrame Present(const GSPresentationRequest &request) override;

    bool ClearFramebuffer(const GSContext &context, uint32_t rgba) override;
    uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override;

    uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override;
    void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override;
    void SnapshotVram(std::vector<uint8_t> &out) const override;
    GSTransferSnapshot GetTransferSnapshot() const override;

private:
    struct RunTotals
    {
        uint64_t runs = 0, mismatchRuns = 0;
        uint64_t px = 0, pxFrameMatch = 0, pxZMatch = 0, pxBothMatch = 0;
        uint32_t maxChannelDiff = 0;
        void Add(const RunTotals &o);
    };

    void OnRun(uint32_t fbp, uint32_t zbp, uint32_t fbw, int x0, int y0, int x1, int y1);
    void Resync();
    void WritePpm(const std::string &path, const PresentationFrame &f) const;

    std::unique_ptr<GSCpuBackend> m_oracle;
    std::unique_ptr<GSMetalBackend> m_metal;
    uint8_t *m_vram = nullptr;
    uint32_t m_vramSize = 0;
    std::vector<uint8_t> m_shadow; // the Metal backend's VRAM

    enum class Resync
    {
        None,
        Frame,
        Run
    } m_resync = Resync::Frame;
    std::FILE *m_log = nullptr;
    std::string m_dumpDir;
    uint32_t m_dumpEvery = 50;
    uint32_t m_maxMismatchDumps = 20;
    uint32_t m_mismatchDumps = 0;
    uint64_t m_frame = 0;

    RunTotals m_frameRuns, m_totalRuns;
    uint64_t m_framesCompared = 0, m_framesExact = 0;
    uint64_t m_framePx = 0, m_framePxMatch = 0;
};
