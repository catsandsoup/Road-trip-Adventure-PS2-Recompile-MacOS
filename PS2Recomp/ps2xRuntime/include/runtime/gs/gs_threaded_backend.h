#pragma once

// GSThreadedBackend: MTGS-style wrapper that runs an inner GSRasterBackend (the CPU
// rasteriser) on a dedicated "GSThread", consuming commands in exact submission order.
//
// Threading contract
// - Producer calls (everything except the destructor) must be serialised by the caller.
//   The GS frontend makes every backend call while holding GS::m_stateMutex, so the
//   command stream has one well-defined order even when several host threads present.
// - Commands that only change GS local memory or rasteriser state are queued
//   (Submit, LoadClut, BeginTransfer, UploadImage, TextureFlush, ClearFramebuffer,
//   WriteVram, Reset, PresentAsync). They are appended to a chunk; a chunk is handed
//   to the GS thread when it is full, at Flush()/Sync(Finish|Presentation), and at
//   every presentation. There is no per-primitive lock or wake-up.
// - Readbacks drain the queue first (wait until the GS thread has executed every
//   queued command), then call the inner backend: ReadVram, SnapshotVram,
//   GetTransferSnapshot, ConsumeLocalToHostBytes, Present, and
//   Sync(LocalToHost|DebugReadback|Reset).
// - Sync(Finish) does not drain: the EE cannot observe raster progress except through
//   readbacks, and the frontend raises CSR.FINISH itself (same model as PCSX2's
//   Gif_HandlerAD/Gif_FinishIRQ, which never wait for the MTGS thread).
// - Backpressure: at most kMaxPresentsInFlight presentations and kMaxPooledBytes of
//   queued command data; the producer blocks when either is exceeded.
// - Every command runs under the floating-point control state (rounding mode,
//   flush-to-zero) of the thread that issued it: the producer compares its FP control
//   register on every enqueue and inserts a SetFpControl record when it changes. The game
//   thread runs with round-toward-zero + FZ while host threads (e.g. the render loop's
//   presents) use the default mode, and rasteriser float maths depends on it.

#include "runtime/gs/gs_backend.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class GSThreadedBackend final : public GSRasterBackend
{
public:
    explicit GSThreadedBackend(std::unique_ptr<GSRasterBackend> inner);
    ~GSThreadedBackend() override;

    GSThreadedBackend(const GSThreadedBackend &) = delete;
    GSThreadedBackend &operator=(const GSThreadedBackend &) = delete;

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
    bool PresentAsync(const GSPresentationRequest &request, PresentCallback done) override;

    bool ClearFramebuffer(const GSContext &context, uint32_t rgba) override;
    uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override;

    uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override;
    void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override;
    void SnapshotVram(std::vector<uint8_t> &out) const override;
    GSTransferSnapshot GetTransferSnapshot() const override;

    // PS2X_GS_THREAD=0 disables the GS thread (default on).
    static bool EnabledByEnvironment();

    struct Stats
    {
        uint64_t chunksPublished = 0;
        uint64_t drains = 0;
        uint64_t producerStalls = 0; // waits for a free chunk or a present slot
        uint64_t presents = 0;
    };
    Stats GetStats() const;

    static constexpr uint32_t kMaxPresentsInFlight = 2u;
    static constexpr size_t kChunkBytes = 256u * 1024u;
    static constexpr size_t kMaxPooledBytes = 16u * 1024u * 1024u; // ~2 race frames of commands (8.3 MB each)

private:
    enum class Op : uint32_t;
    struct Chunk
    {
        std::vector<uint8_t> bytes; // capacity >= kChunkBytes (larger for oversized records)
        size_t used = 0;
        uint64_t seq = 0;
    };
    struct PresentJob
    {
        GSPresentationRequest request;
        PresentCallback done;
    };

    // producer side (caller-serialised)
    uint8_t *Reserve(Op op, size_t payloadBytes);
    void Publish();                 // hand the current chunk (if any) to the GS thread
    void Drain();                   // Publish + wait until the GS thread is idle
    Chunk *AcquireChunk(size_t minBytes);

    // GS thread
    void ThreadMain();
    void Execute(const Chunk &chunk);

    std::unique_ptr<GSRasterBackend> m_inner;
    bool m_hasVram = false;
    uint64_t m_producerFpControl = ~0ull; // FP control state of the last enqueued command

    mutable std::mutex m_mutex;
    mutable std::condition_variable m_consumerCv; // work available / stop
    mutable std::condition_variable m_producerCv; // chunk freed / present delivered / chunk done
    std::deque<Chunk *> m_ready;                  // published, not yet executed
    std::vector<Chunk *> m_free;                  // pooled empty chunks
    std::vector<std::unique_ptr<Chunk>> m_owned;
    size_t m_pooledBytes = 0;                     // bytes of all owned chunks
    uint64_t m_publishedSeq = 0;
    uint64_t m_completedSeq = 0;
    uint32_t m_presentsInFlight = 0;
    bool m_stop = false;
    double m_threadCpuSeconds = 0.0; // GS thread CPU time (CLOCK_THREAD_CPUTIME_ID), set when the thread exits
    bool m_consumerWaiting = false;
    mutable Stats m_stats{};

    Chunk *m_current = nullptr; // producer-owned partially filled chunk
    std::thread m_thread;
};
