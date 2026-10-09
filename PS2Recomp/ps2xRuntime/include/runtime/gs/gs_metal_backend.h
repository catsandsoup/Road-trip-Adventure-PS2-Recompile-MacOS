#pragma once

// GSMetalBackend (Metal plan M1 skeleton; macOS only, PS2X_GS_BACKEND=metal|tee).
//
// - Offscreen MTLDevice + queue; all Metal work happens on the thread that calls the backend
//   (the GS thread under GSThreadedBackend).
// - The 4 MB CPU VRAM shadow (the buffer passed to Initialize) stays authoritative. Metal-handled
//   draws are recorded into a "run" against one GPU target (CT32 frame + Z24 depth, both kept as raw
//   32-bit VRAM words in R32Uint textures). A run is closed (committed, waited for, and its dirty
//   rectangle written back into the shadow) before any other operation, so every CPU-side reader
//   and the per-draw CPU fallback always see the GPU results. A target is re-uploaded from the
//   shadow when a CPU-side write touched one of its pages since the last sync.
// - First draw path: untextured triangles (incl. strips/fans) and sprites, CT32 frame, Z24.
//   Coverage, attribute interpolation and Z reproduce GSCpuBackend's float/double maths exactly in
//   the fragment shader (round-toward-zero emulation for draws recorded under the game thread's
//   FPCR, soft double for Z), and the integer GS pixel pipeline (fog, alpha test + AFAIL, DATE,
//   depth test, blend, PABE, FBA, FBMSK) runs with framebuffer fetch.
// - Everything else falls back to GSCpuBackend per draw; fallbacks are counted by reason.

#include "runtime/gs/gs_backend.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>

class GSMetalBackend final : public GSRasterBackend
{
public:
    struct RunInfo
    {
        uint32_t fbp = 0, zbp = 0, fbw = 0;
        int x0 = 0, y0 = 0, x1 = -1, y1 = -1; // dirty rectangle (inclusive), target pixels
        uint32_t prims = 0;
    };

    // Why a run was closed (commit + wait + write-back).
    enum FlushWhy : uint32_t
    {
        kFlushTarget = 0, // next draw uses another target / run too short
        kFlushTexOverlap, // a texture samples pages the open run renders to
        kFlushClut,       // CLUT source pages overlap the run's target
        kFlushTransfer,   // image transfer touches the run's target pages
        kFlushFallback,   // per-draw CPU fallback
        kFlushPresent,
        kFlushReadback,   // Read/Snapshot/GetTransferSnapshot/ConsumeLocalToHost
        kFlushOther,      // Initialize/Reset/Flush/Sync/Clear/WriteVram
        kFlushPalette,    // palette ring full
        kFlushLimit,      // prim / in-flight byte limits
        kFlushWhyCount
    };

    struct Stats
    {
        uint64_t submits = 0;
        uint64_t metalPrims = 0;    // primitives recorded on the Metal path
        uint64_t metalTexPrims = 0; // ... of which textured (triangles and sprites)
        uint64_t metalFbPrims = 0;  // ... of which sprites sampling another framebuffer target directly
        uint64_t metalSelfPrims = 0; // ... of which feedback sprites sampling a snapshot of their own target
        uint64_t fallbackPrims = 0; // primitives rasterised by the CPU fallback
        uint64_t runs = 0;
        uint64_t targetUploads = 0;
        uint64_t readbackPixels = 0;
        uint64_t gpuWaitNs = 0;
        uint64_t presents = 0;
        uint64_t texDecodes = 0, texHits = 0, texTexels = 0, palettes = 0, paletteHits = 0;
        uint64_t flushes[kFlushWhyCount] = {}; // runs encoded, by why
        uint64_t waits[kFlushWhyCount] = {};   // GPU round trips (commit + wait), by why
        uint64_t commits = 0, partialUploads = 0, uploadPixels = 0;
        // PS2X_GS_METAL_TIMING=1: where the GS thread spends its time (ns)
        uint64_t recordNs = 0, texNs = 0, uploadNs = 0, encodeNs = 0, writebackNs = 0, fallbackNs = 0;
        std::map<std::string, uint64_t> fallbacks; // reason -> primitives
        void Add(const Stats &o);
    };

    // nullptr when no Metal device is available or the shaders fail to compile.
    static std::unique_ptr<GSMetalBackend> Create();
    ~GSMetalBackend() override;

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

    // Called after each run has been written back to the shadow (tee comparison hook).
    void SetRunObserver(std::function<void(const RunInfo &)> observer);
    // The shadow was modified behind the backend's back (tee resync): re-upload targets on next use.
    void MarkShadowChanged();
    // Close any open run (commit, wait, write back).
    void FlushRun();

    Stats TakeFrameStats();
    const Stats &TotalStats() const;

    // GPU self-test of the round-toward-zero float and soft-double Z helpers against the host FPU.
    // Returns the number of mismatches over `cases` random inputs (prints a summary line).
    uint64_t SelfTest(uint32_t cases);
    // Differential test of the textured triangle and sprite paths against GSCpuBackend (the oracle) on random
    // draw states, under round-toward-zero + FZ and under the default FP mode. Returns the number of
    // mismatching draws (prints a summary).
    uint64_t SelfTestDraws(uint32_t draws, uint32_t seed);

private:
    GSMetalBackend();
    struct Impl;
    std::unique_ptr<Impl> m;
};

// Logs the per-session totals of a metal backend (used by the factory at exit).
void GSMetalPrintStats(const char *tag, const GSMetalBackend::Stats &stats);
