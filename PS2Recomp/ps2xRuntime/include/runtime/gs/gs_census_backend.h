#pragma once

// GSCensusBackend (Metal plan M0): a count-only census of the GS command stream.
//
// Enabled with PS2X_GS_CENSUS=<file> (or =1 for "gs_census.txt" in the working directory).
// It wraps the real raster backend and forwards every call unchanged, so it is output-neutral by
// construction; it only observes. It runs where the inner backend runs (the GS thread when
// GSThreadedBackend is on), and it never reads VRAM contents.
//
// Per presented frame it writes one "F" line of counters (primitive types, textured primitives,
// CLUT loads, host->local bytes and transfers, local->local and local->host transfers,
// render-to-texture hits, texture pages re-uploaded after being sampled in the same frame, ...),
// and every 100 frames (and at destruction) a cumulative block of keyed state counts (frame/Z/
// texture/CLUT PSMs, ALPHA, TEST, FBA, PABE, DTHE, COLCLAMP, FBMSK, XYOFFSET/SCISSOR per frame
// buffer, CLD modes, ZBP, ...). Keys hold register-field modes and counts only.
// game/tests/gs_census_report.py summarises the file.

#include "runtime/gs/gs_backend.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <string>

class GSCensusBackend final : public GSRasterBackend
{
public:
    GSCensusBackend(std::unique_ptr<GSRasterBackend> inner, const char *path);
    ~GSCensusBackend() override;

    // nullptr when PS2X_GS_CENSUS is unset
    static const char *PathFromEnvironment();

    void Initialize(uint8_t *vram, uint32_t vramSize) override { m_inner->Initialize(vram, vramSize); }
    void Reset() override { m_inner->Reset(); }

    void Submit(const GSPrimitiveBatch &batch) override;
    void LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut) override;
    void BeginTransfer(const GSTransferCommand &command) override;
    void UploadImage(const uint8_t *data, uint32_t sizeBytes) override;

    void Flush() override { m_inner->Flush(); }
    void TextureFlush() override;
    void Sync(GSSyncReason reason) override { m_inner->Sync(reason); }
    PresentationFrame Present(const GSPresentationRequest &request) override;
    bool PresentAsync(const GSPresentationRequest &request, PresentCallback done) override;

    bool ClearFramebuffer(const GSContext &context, uint32_t rgba) override;
    uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override { return m_inner->ConsumeLocalToHostBytes(dst, maxBytes); }

    uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override { return m_inner->ReadVram(psm, base, bw, x, y); }
    void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override { m_inner->WriteVram(psm, base, bw, x, y, value); }
    void SnapshotVram(std::vector<uint8_t> &out) const override { m_inner->SnapshotVram(out); }
    GSTransferSnapshot GetTransferSnapshot() const override { return m_inner->GetTransferSnapshot(); }

private:
    static constexpr uint32_t kPages = 512u; // 4 MB / 8 KB

    enum Counter : uint32_t
    {
        cPrims,
        cPoint,
        cLine,
        cTri,
        cSprite,
        cTextured,
        cTexturedSprite,
        cUntexturedTri,
        cUntexturedSprite,
        cClutLoads,
        cUploads,
        cUploadBytes,
        cUploadSameDbpTwice,     // a host->local transfer to a DBP that already received one this frame
        cUploadSameDbpNewData,   // ... with different contents (FNV-1a over the payload)
        cUploadAfterSample,      // a host->local transfer into pages sampled earlier this frame
        cUploadIntoTarget,       // a host->local transfer into pages a draw wrote this frame
        cLocalToLocal,
        cLocalToLocalBytes,
        cLocalToLocalFromTarget, // source pages written by a draw this or last frame
        cLocalToHost,
        cRttInFrame,             // textured prim whose texture pages were draw-written earlier this frame
        cRttInFrameSprite,
        cRttPrevFrame,           // ... written by a draw in an earlier drawing frame (any age >= 1)
        cRttPrevFrameSprite,
        cZAsTexture,             // ... pages last written as a Z buffer
        cTexFlush,
        cClears,
        cCount
    };

    struct KeyCount
    {
        uint64_t total = 0;
        uint64_t frames = 0;
        uint64_t lastFrame = ~0ull;
    };

    struct PageState
    {
        uint64_t drawFrame = ~0ull;   // last frame a draw wrote this page (frame or Z)
        uint64_t uploadFrame = ~0ull; // last frame a host->local transfer wrote it
        uint64_t sampleFrame = ~0ull; // last frame a textured prim sampled it
        bool lastDrawWasZ = false;
        bool drawnAfterUpload = false; // the newest write was a draw
    };

    void Count(const std::string &key, uint64_t n = 1);
    void BeginFrameIfNeeded();
    void EndFrame();
    void WriteKeys(const char *tag);
    void TransferPages(uint32_t bp, uint32_t bw, uint8_t psm, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                       uint32_t *pages, uint32_t &count, uint32_t maxPages) const;

    std::unique_ptr<GSRasterBackend> m_inner;
    std::FILE *m_out = nullptr;
    uint64_t m_frame = 0;     // presents
    uint64_t m_drawFrame = 0; // presents that had at least one primitive (page ages count these)
    std::array<uint64_t, cCount> m_frameCounters{};
    std::map<std::string, KeyCount> m_keys;
    std::array<PageState, kPages> m_pages{};
    std::map<uint32_t, uint64_t> m_frameUploadHashByDbp; // this frame
    GSTransferCommand m_transfer{};
    bool m_transferActive = false;
    uint64_t m_transferHash = 0;
    uint32_t m_transferBytes = 0;
};
