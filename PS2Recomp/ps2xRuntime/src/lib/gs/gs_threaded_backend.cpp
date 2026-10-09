#include "runtime/gs/gs_threaded_backend.h"

#include "ThreadNaming.h"

#include <algorithm>
#include <cfenv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#if defined(__APPLE__)
#include <pthread/qos.h>
#endif
#if defined(__x86_64__) || defined(_M_X64)
#include <xmmintrin.h>
#endif

enum class GSThreadedBackend::Op : uint32_t
{
    Submit = 1,
    LoadClut,
    BeginTransfer,
    UploadImage,
    TextureFlush,
    ClearFramebuffer,
    WriteVram,
    Reset,
    Present,
    SetFpControl,
};

namespace
{
    struct GsThrRecordHeader
    {
        uint32_t op;
        uint32_t payloadBytes;
    };
    static_assert(sizeof(GsThrRecordHeader) == 8u, "record header must stay 8 bytes");

    struct GsThrClutPayload
    {
        GSTex0Reg tex0;
        GSTexClutReg texclut;
    };

    struct GsThrClearPayload
    {
        GSContext context;
        uint32_t rgba;
    };

    struct GsThrWriteVramPayload
    {
        uint32_t psm, base, bw, x, y, value;
    };

    constexpr size_t gsThrAlign8(size_t n)
    {
        return (n + 7u) & ~static_cast<size_t>(7u);
    }

    template <typename T>
    const T *gsThrPayload(const uint8_t *p)
    {
        return std::launder(reinterpret_cast<const T *>(p));
    }

    // FP control state that affects results: rounding mode and flush-to-zero.
    inline uint64_t gsThrReadFpControl()
    {
#if defined(__aarch64__)
        uint64_t fpcr;
        __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
        return fpcr;
#elif defined(__x86_64__) || defined(_M_X64)
        return (static_cast<uint64_t>(_mm_getcsr() & ~0x3Fu)) |
               (static_cast<uint64_t>(static_cast<uint32_t>(std::fegetround())) << 32);
#else
        return static_cast<uint64_t>(static_cast<uint32_t>(std::fegetround()));
#endif
    }

    inline void gsThrApplyFpControl(uint64_t control)
    {
#if defined(__aarch64__)
        __asm__ volatile("msr fpcr, %0" : : "r"(control));
#elif defined(__x86_64__) || defined(_M_X64)
        _mm_setcsr((_mm_getcsr() & 0x3Fu) | static_cast<uint32_t>(control & ~0x3Full));
        std::fesetround(static_cast<int>(control >> 32));
#else
        std::fesetround(static_cast<int>(control));
#endif
    }

    bool gsThrStatsEnabled()
    {
        static const bool enabled = std::getenv("PS2X_GS_THREAD_STATS") != nullptr;
        return enabled;
    }
}

bool GSThreadedBackend::EnabledByEnvironment()
{
    const char *v = std::getenv("PS2X_GS_THREAD");
    if (!v || v[0] == '\0')
        return true;
    return !(v[0] == '0' && v[1] == '\0');
}

GSThreadedBackend::GSThreadedBackend(std::unique_ptr<GSRasterBackend> inner)
    : m_inner(std::move(inner))
{
    m_thread = std::thread([this]()
                           { ThreadMain(); });
}

GSThreadedBackend::~GSThreadedBackend()
{
    Drain();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = true;
    }
    m_consumerCv.notify_all();
    if (m_thread.joinable())
        m_thread.join();
    if (gsThrStatsEnabled())
    {
        std::printf("[gs-thread] chunks=%llu drains=%llu stalls=%llu presents=%llu pooledBytes=%zu\n",
                    static_cast<unsigned long long>(m_stats.chunksPublished),
                    static_cast<unsigned long long>(m_stats.drains),
                    static_cast<unsigned long long>(m_stats.producerStalls),
                    static_cast<unsigned long long>(m_stats.presents),
                    m_pooledBytes);
        std::fflush(stdout);
    }
    if (m_current)
        m_current = nullptr; // owned by m_owned
}

GSThreadedBackend::Stats GSThreadedBackend::GetStats() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_stats;
}

// ---------------------------------------------------------------- producer side

GSThreadedBackend::Chunk *GSThreadedBackend::AcquireChunk(size_t minBytes)
{
    const size_t wantCapacity = std::max(minBytes, kChunkBytes);
    std::unique_lock<std::mutex> lock(m_mutex);
    bool stalled = false;
    for (;;)
    {
        if (wantCapacity == kChunkBytes && !m_free.empty())
        {
            Chunk *chunk = m_free.back();
            m_free.pop_back();
            chunk->used = 0u;
            return chunk;
        }
        const bool idle = (m_completedSeq == m_publishedSeq);
        if (m_pooledBytes + wantCapacity <= kMaxPooledBytes || idle)
        {
            auto owned = std::make_unique<Chunk>();
            owned->bytes.resize(wantCapacity);
            Chunk *chunk = owned.get();
            m_owned.push_back(std::move(owned));
            m_pooledBytes += wantCapacity;
            return chunk;
        }
        if (!stalled)
        {
            stalled = true;
            ++m_stats.producerStalls;
        }
        m_producerCv.wait(lock);
    }
}

uint8_t *GSThreadedBackend::Reserve(Op op, size_t payloadBytes)
{
    const uint64_t fpControl = gsThrReadFpControl();
    if (fpControl != m_producerFpControl)
    {
        m_producerFpControl = fpControl;
        std::memcpy(Reserve(Op::SetFpControl, sizeof(uint64_t)), &fpControl, sizeof(uint64_t));
    }
    const size_t recordBytes = sizeof(GsThrRecordHeader) + gsThrAlign8(payloadBytes);
    if (m_current && m_current->used + recordBytes > m_current->bytes.size())
        Publish();
    if (!m_current)
        m_current = AcquireChunk(recordBytes);

    uint8_t *p = m_current->bytes.data() + m_current->used;
    GsThrRecordHeader header{static_cast<uint32_t>(op), static_cast<uint32_t>(payloadBytes)};
    std::memcpy(p, &header, sizeof(header));
    m_current->used += recordBytes;
    return p + sizeof(GsThrRecordHeader);
}

void GSThreadedBackend::Publish()
{
    if (!m_current || m_current->used == 0u)
        return;
    bool wake = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_current->seq = ++m_publishedSeq;
        m_ready.push_back(m_current);
        ++m_stats.chunksPublished;
        wake = m_consumerWaiting;
    }
    m_current = nullptr;
    if (wake)
        m_consumerCv.notify_one();
}

void GSThreadedBackend::Drain()
{
    Publish();
    std::unique_lock<std::mutex> lock(m_mutex);
    ++m_stats.drains;
    const uint64_t target = m_publishedSeq;
    m_producerCv.wait(lock, [&]()
                      { return m_completedSeq >= target; });
}

// ---------------------------------------------------------------- GS thread

void GSThreadedBackend::ThreadMain()
{
    ThreadNaming::SetCurrentThreadName("GSThread");
#if defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif

    std::unique_lock<std::mutex> lock(m_mutex);
    for (;;)
    {
        while (m_ready.empty() && !m_stop)
        {
            m_consumerWaiting = true;
            m_consumerCv.wait(lock);
            m_consumerWaiting = false;
        }
        if (m_ready.empty())
            break; // stop requested and nothing left
        Chunk *chunk = m_ready.front();
        m_ready.pop_front();
        lock.unlock();

        Execute(*chunk);

        lock.lock();
        m_completedSeq = chunk->seq;
        if (chunk->bytes.size() > kChunkBytes)
        {
            m_pooledBytes -= chunk->bytes.size();
            auto it = std::find_if(m_owned.begin(), m_owned.end(),
                                   [chunk](const std::unique_ptr<Chunk> &c)
                                   { return c.get() == chunk; });
            if (it != m_owned.end())
                m_owned.erase(it);
        }
        else
        {
            chunk->used = 0u;
            m_free.push_back(chunk);
        }
        m_producerCv.notify_all();
    }
}

void GSThreadedBackend::Execute(const Chunk &chunk)
{
    const uint8_t *p = chunk.bytes.data();
    const uint8_t *const end = p + chunk.used;
    while (p < end)
    {
        GsThrRecordHeader header;
        std::memcpy(&header, p, sizeof(header));
        const uint8_t *payload = p + sizeof(GsThrRecordHeader);
        p = payload + gsThrAlign8(header.payloadBytes);

        switch (static_cast<Op>(header.op))
        {
        case Op::Submit:
            m_inner->Submit(*gsThrPayload<GSPrimitiveBatch>(payload));
            break;
        case Op::LoadClut:
        {
            const GsThrClutPayload *c = gsThrPayload<GsThrClutPayload>(payload);
            m_inner->LoadClut(c->tex0, c->texclut);
            break;
        }
        case Op::BeginTransfer:
            m_inner->BeginTransfer(*gsThrPayload<GSTransferCommand>(payload));
            break;
        case Op::UploadImage:
            m_inner->UploadImage(payload, header.payloadBytes);
            break;
        case Op::TextureFlush:
            m_inner->TextureFlush();
            break;
        case Op::ClearFramebuffer:
        {
            const GsThrClearPayload *c = gsThrPayload<GsThrClearPayload>(payload);
            (void)m_inner->ClearFramebuffer(c->context, c->rgba);
            break;
        }
        case Op::WriteVram:
        {
            const GsThrWriteVramPayload *w = gsThrPayload<GsThrWriteVramPayload>(payload);
            m_inner->WriteVram(w->psm, w->base, w->bw, w->x, w->y, w->value);
            break;
        }
        case Op::Reset:
            m_inner->Reset();
            break;
        case Op::SetFpControl:
        {
            uint64_t control;
            std::memcpy(&control, payload, sizeof(control));
            gsThrApplyFpControl(control);
            break;
        }
        case Op::Present:
        {
            PresentJob *job = nullptr;
            std::memcpy(&job, payload, sizeof(job));
            m_inner->Flush();
            m_inner->Sync(GSSyncReason::Presentation);
            PresentationFrame frame = m_inner->Present(job->request);
            if (job->done)
                job->done(std::move(frame));
            delete job;
            Stats stats{};
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                --m_presentsInFlight;
                stats = m_stats;
            }
            m_producerCv.notify_all();
            if (gsThrStatsEnabled() && (stats.presents % 250u) == 0u)
            {
                std::printf("[gs-thread] presents=%llu chunks=%llu drains=%llu stalls=%llu\n",
                            static_cast<unsigned long long>(stats.presents),
                            static_cast<unsigned long long>(stats.chunksPublished),
                            static_cast<unsigned long long>(stats.drains),
                            static_cast<unsigned long long>(stats.producerStalls));
                std::fflush(stdout);
            }
            break;
        }
        default:
            std::fprintf(stderr, "[gs-thread] corrupt command stream (op=%u)\n", header.op);
            std::fflush(stderr);
            return;
        }
    }
}

// ---------------------------------------------------------------- GSRasterBackend

void GSThreadedBackend::Initialize(uint8_t *vram, uint32_t vramSize)
{
    Drain();
    m_inner->Initialize(vram, vramSize);
    m_hasVram = (vram != nullptr);
}

void GSThreadedBackend::Reset()
{
    Reserve(Op::Reset, 0u);
}

void GSThreadedBackend::Submit(const GSPrimitiveBatch &batch)
{
    if (batch.vertexCount == 0u)
        return; // the CPU backend ignores empty batches too
    ::new (Reserve(Op::Submit, sizeof(GSPrimitiveBatch))) GSPrimitiveBatch(batch);
}

void GSThreadedBackend::LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut)
{
    ::new (Reserve(Op::LoadClut, sizeof(GsThrClutPayload))) GsThrClutPayload{tex0, texclut};
}

void GSThreadedBackend::BeginTransfer(const GSTransferCommand &command)
{
    ::new (Reserve(Op::BeginTransfer, sizeof(GSTransferCommand))) GSTransferCommand(command);
}

void GSThreadedBackend::UploadImage(const uint8_t *data, uint32_t sizeBytes)
{
    if (!data || sizeBytes == 0u)
        return;
    std::memcpy(Reserve(Op::UploadImage, sizeBytes), data, sizeBytes);
}

void GSThreadedBackend::Flush()
{
    Publish();
}

void GSThreadedBackend::TextureFlush()
{
    Reserve(Op::TextureFlush, 0u);
}

void GSThreadedBackend::Sync(GSSyncReason reason)
{
    switch (reason)
    {
    case GSSyncReason::Finish:
    case GSSyncReason::Presentation:
        // Not observable by the EE: start the GS thread on what is queued, don't wait.
        Publish();
        break;
    case GSSyncReason::LocalToHost:
    case GSSyncReason::DebugReadback:
    case GSSyncReason::Reset:
        Drain();
        m_inner->Sync(reason);
        break;
    }
}

PresentationFrame GSThreadedBackend::Present(const GSPresentationRequest &request)
{
    Drain();
    return m_inner->Present(request);
}

bool GSThreadedBackend::PresentAsync(const GSPresentationRequest &request, PresentCallback done)
{
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_presentsInFlight >= kMaxPresentsInFlight)
        {
            ++m_stats.producerStalls;
            lock.unlock();
            Publish(); // whatever precedes the earlier presents must be able to complete
            lock.lock();
            m_producerCv.wait(lock, [&]()
                              { return m_presentsInFlight < kMaxPresentsInFlight; });
        }
        ++m_presentsInFlight;
        ++m_stats.presents;
    }
    PresentJob *job = new PresentJob{request, std::move(done)};
    std::memcpy(Reserve(Op::Present, sizeof(job)), &job, sizeof(job));
    Publish();
    return true;
}

bool GSThreadedBackend::ClearFramebuffer(const GSContext &context, uint32_t rgba)
{
    // Same acceptance test as GSCpuBackend::ClearFramebuffer; it depends only on
    // register state, so the result is known without waiting for the GS thread.
    if (!m_hasVram || context.frame.fbw == 0u)
        return false;
    const uint8_t psm = context.frame.psm;
    if (psm != GS_PSM_CT32 && psm != GS_PSM_CT24 && psm != GS_PSM_CT16 && psm != GS_PSM_CT16S)
        return false;
    ::new (Reserve(Op::ClearFramebuffer, sizeof(GsThrClearPayload))) GsThrClearPayload{context, rgba};
    return true;
}

uint32_t GSThreadedBackend::ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes)
{
    Drain();
    return m_inner->ConsumeLocalToHostBytes(dst, maxBytes);
}

uint32_t GSThreadedBackend::ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const
{
    const_cast<GSThreadedBackend *>(this)->Drain();
    return m_inner->ReadVram(psm, base, bw, x, y);
}

void GSThreadedBackend::WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    ::new (Reserve(Op::WriteVram, sizeof(GsThrWriteVramPayload))) GsThrWriteVramPayload{psm, base, bw, x, y, value};
}

void GSThreadedBackend::SnapshotVram(std::vector<uint8_t> &out) const
{
    const_cast<GSThreadedBackend *>(this)->Drain();
    m_inner->SnapshotVram(out);
}

GSTransferSnapshot GSThreadedBackend::GetTransferSnapshot() const
{
    const_cast<GSThreadedBackend *>(this)->Drain();
    return m_inner->GetTransferSnapshot();
}
