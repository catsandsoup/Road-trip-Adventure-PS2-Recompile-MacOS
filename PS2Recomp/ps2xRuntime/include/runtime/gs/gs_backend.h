#pragma once

#include "runtime/gs/gs_types.h"

#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

class GSRasterBackend
{
public:
    virtual ~GSRasterBackend() = default;

    virtual void Initialize(uint8_t *vram, uint32_t vramSize) = 0;
    virtual void Reset() = 0;

    virtual void Submit(const GSPrimitiveBatch &batch) = 0;
    virtual void LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut) = 0;

    virtual void BeginTransfer(const GSTransferCommand &command) = 0;
    virtual void UploadImage(const uint8_t *data, uint32_t sizeBytes) = 0;

    virtual void Flush() = 0;
    virtual void TextureFlush() = 0;
    virtual void Sync(GSSyncReason reason) = 0;
    virtual PresentationFrame Present(const GSPresentationRequest &request) = 0;

    // Presentation captured in stream order. A backend that rasterises on its own thread
    // (GSThreadedBackend) queues the request behind every earlier command and runs `done`
    // on that thread once the frame exists; `done` must not take GS frontend state locks.
    // Returns false when the backend only presents synchronously; the caller then uses
    // Present() and `done` is not called.
    using PresentCallback = std::function<void(PresentationFrame &&)>;
    virtual bool PresentAsync(const GSPresentationRequest &request, PresentCallback done)
    {
        (void)request;
        (void)done;
        return false;
    }

    virtual bool ClearFramebuffer(const GSContext &context, uint32_t rgba) = 0;
    virtual uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) = 0;

    virtual uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const = 0;
    virtual void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) = 0;
    virtual void SnapshotVram(std::vector<uint8_t> &out) const = 0;
    virtual GSTransferSnapshot GetTransferSnapshot() const = 0;
};
