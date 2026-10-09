// Native VU1 microprogram registry, MSCAL dispatcher, PATH1 (XGKICK) model and
// the PS2X_VU1_DIFF differential test.  See include/runtime/ps2_vu1_recomp.h.

#include "runtime/ps2_vu1_recomp.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/gs/ps2_gif_arbiter.h"
#include "runtime/ps2_memory.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace
{
    std::vector<Vu1RecImage> &registry()
    {
        static std::vector<Vu1RecImage> images;
        return images;
    }

    uint64_t fnv1a64(const uint8_t *data, size_t size)
    {
        uint64_t hash = 1469598103934665603ull;
        for (size_t i = 0; i < size; ++i)
        {
            hash ^= data[i];
            hash *= 1099511628211ull;
        }
        return hash;
    }

    double nowSeconds()
    {
        using namespace std::chrono;
        return duration<double>(steady_clock::now().time_since_epoch()).count();
    }

    int envInt(const char *name, int fallback)
    {
        const char *v = std::getenv(name);
        if (!v || !*v)
            return fallback;
        return std::atoi(v);
    }

    double envDouble(const char *name, double fallback)
    {
        const char *v = std::getenv(name);
        if (!v || !*v)
            return fallback;
        return std::atof(v);
    }

    struct PacketCapture
    {
        std::vector<std::vector<uint8_t>> packets;
        static void sink(void *user, const uint8_t *data, uint32_t size)
        {
            auto *self = static_cast<PacketCapture *>(user);
            self->packets.emplace_back(data, data + size);
        }
    };

    void submitPacket(Vu1RecContext &ctx, const uint8_t *data, uint32_t size)
    {
        if (ctx.sink)
            ctx.sink(ctx.sinkUser, data, size);
        else if (ctx.memory)
            ctx.memory->submitGifPacket(GifPathId::Path1, data, size);
        else if (ctx.gs)
            ctx.gs->processGIFPacket(data, size);
    }

    // One PATH1 transfer step: exactly the loop body of VU1Interpreter::progressXgkick.
    // Returns false when the transfer ended (finished or aborted).
    bool kickStep(Vu1RecContext &ctx)
    {
        Vu1RecKick &k = ctx.kick;
        const uint32_t dataSize = PS2_VU1_DATA_SIZE;
        if (k.copiedBytes > Vu1RecKick::kBufferSize - 16u)
        {
            ++ctx.kickErrors;
            ctx.failed = true;
            k.active = false;
            return false;
        }
        const uint64_t eventCycle = k.issueCycle + 1u + 2u * (k.copiedBytes / 16u);
        const uint32_t qwordOffset = k.copiedBytes;
        for (uint32_t i = 0; i < 16u; ++i)
        {
            const uint32_t source = (k.sourceAddress + k.copiedBytes + i) % dataSize;
            k.packet[k.copiedBytes + i] = ctx.data[source];
        }
        k.copiedBytes += 16u;

        if (k.currentTagEnd == 0u)
        {
            uint64_t tagLo = 0;
            std::memcpy(&tagLo, k.packet.data() + qwordOffset, sizeof(tagLo));
            const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
            const uint32_t format = static_cast<uint32_t>((tagLo >> 58) & 0x3u);
            uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFu);
            if (nreg == 0u)
                nreg = 16u;
            uint64_t tagBytes = 16u;
            if (format == 0u)
                tagBytes += static_cast<uint64_t>(nloop) * nreg * 16u;
            else if (format == 1u)
                tagBytes += ((static_cast<uint64_t>(nloop) * nreg + 1u) & ~1ull) * 8u;
            else if (format == 2u)
                tagBytes += static_cast<uint64_t>(nloop) * 16u;
            else
            {
                ++ctx.kickErrors;
                ctx.failed = true;
                k.active = false;
                return false;
            }
            if (tagBytes > Vu1RecKick::kBufferSize - qwordOffset)
            {
                ++ctx.kickErrors;
                ctx.failed = true;
                k.active = false;
                return false;
            }
            k.currentTagEnd = qwordOffset + static_cast<uint32_t>(tagBytes);
            k.currentTagEop = ((tagLo >> 15) & 1u) != 0u;
            if (k.currentTagEop)
                k.totalBytes = k.currentTagEnd;
        }

        if (k.copiedBytes >= k.currentTagEnd)
        {
            if (k.currentTagEop)
            {
                k.active = false;
                k.finishCycle = eventCycle;
                submitPacket(ctx, k.packet.data(), k.totalBytes);
                return false;
            }
            k.currentTagEnd = 0u;
            k.currentTagEop = false;
        }
        return true;
    }
}

void vu1recRegister(const Vu1RecImage &image)
{
    registry().push_back(image);
}

const Vu1RecImage *vu1recFind(uint64_t hash)
{
    for (const Vu1RecImage &image : registry())
        if (image.hash == hash)
            return &image;
    return nullptr;
}

size_t vu1recImageCount()
{
    return registry().size();
}

void vu1rec_kick_start(Vu1RecContext &ctx, uint32_t qwordAddress, uint64_t cycle)
{
    Vu1RecKick &k = ctx.kick;
    k.active = true;
    k.currentTagEop = false;
    k.sourceAddress = (qwordAddress * 16u) % PS2_VU1_DATA_SIZE;
    k.totalBytes = 0u;
    k.copiedBytes = 0u;
    k.currentTagEnd = 0u;
    k.issueCycle = cycle;
}

void vu1rec_kick_catchup(Vu1RecContext &ctx, uint64_t cycle)
{
    Vu1RecKick &k = ctx.kick;
    while (k.active && k.issueCycle + 1u + 2u * (k.copiedBytes / 16u) <= cycle)
    {
        if (!kickStep(ctx))
            break;
    }
}

uint64_t vu1rec_kick_drain(Vu1RecContext &ctx)
{
    while (ctx.kick.active)
    {
        if (!kickStep(ctx))
            break;
    }
    return ctx.kick.finishCycle;
}

// ---------------------------------------------------------------------------

Vu1NativeDispatcher::Vu1NativeDispatcher()
{
    const char *forced = std::getenv("PS2X_VU1_INTERPRETER");
    m_forceInterpreter = forced && forced[0] != '\0' && forced[0] != '0';
    const char *diff = std::getenv("PS2X_VU1_DIFF");
    m_diff = diff && diff[0] != '\0' && diff[0] != '0';
    m_diffLogBudget = envInt("PS2X_VU1_DIFF_LOG", 8);
    m_statsPeriod = envDouble("PS2X_VU1_STATS", 5.0);
    m_captureDir = std::getenv("PS2X_VU1_CAPTURE");
    if (m_captureDir && !*m_captureDir)
        m_captureDir = nullptr;
    m_captureEvery = static_cast<uint32_t>(std::max(1, envInt("PS2X_VU1_CAPTURE_EVERY", 997)));
    m_ctx = new Vu1RecContext();
    m_ctx2 = new Vu1RecContext();
    std::fprintf(stderr, "[vu1rec] %zu native image(s) registered; mode=%s\n", vu1recImageCount(),
                 m_forceInterpreter ? "interpreter (PS2X_VU1_INTERPRETER)" : (m_diff ? "diff (PS2X_VU1_DIFF)" : "native"));
    m_lastStatsTime = nowSeconds();
}

Vu1NativeDispatcher::~Vu1NativeDispatcher()
{
    printStats("exit");
    delete m_ctx;
    delete m_ctx2;
}

const Vu1RecImage *Vu1NativeDispatcher::currentImage(PS2Memory &memory)
{
    const uint8_t *code = memory.getVU1Code();
    const uint64_t generation = memory.getVU1CodeGeneration();
    if (code != m_cachedCode || generation != m_cachedGeneration)
    {
        m_cachedCode = code;
        m_cachedGeneration = generation;
        m_cachedHash = fnv1a64(code, PS2_VU1_CODE_SIZE);
        m_cachedImage = vu1recFind(m_cachedHash);
        if (!m_cachedImage &&
            std::find(m_unknownHashes.begin(), m_unknownHashes.end(), m_cachedHash) == m_unknownHashes.end())
        {
            m_unknownHashes.push_back(m_cachedHash);
            std::fprintf(stderr, "[vu1rec] VU1 image %016llx has no native code (interpreter fallback)\n",
                         static_cast<unsigned long long>(m_cachedHash));
        }
    }
    return m_cachedImage;
}

namespace
{
    // VU1Interpreter::execute() preamble.
    void prepareState(VU1State &st, uint32_t startPC, uint32_t top, uint32_t itop)
    {
        st.pc = startPC & 0x3FFFu;
        st.ebit = false;
        st.haltAfterDelaySlot = false;
        st.stoppedByD = false;
        st.stoppedByT = false;
        st.top = top;
        st.itop = itop;
        st.branchPending = false;
        st.branchTarget = 0;
        st.branchDelay = 0;
        st.vf[0][0] = 0.0f;
        st.vf[0][1] = 0.0f;
        st.vf[0][2] = 0.0f;
        st.vf[0][3] = 1.0f;
    }

    void resetContext(Vu1RecContext &ctx, VU1State *st, uint8_t *data, uint64_t cycle, uint32_t maxCycles,
                      uint32_t top, uint32_t itop, PS2Memory *memory, GS *gs)
    {
        ctx.state = st;
        ctx.data = data;
        ctx.cycle = cycle;
        ctx.maxCycles = maxCycles;
        ctx.top = top;
        ctx.itop = itop;
        ctx.memory = memory;
        ctx.gs = gs;
        ctx.sink = nullptr;
        ctx.sinkUser = nullptr;
        ctx.failed = false;
        ctx.failPc = 0;
        ctx.kickErrors = 0;
        ctx.kick.active = false;
        ctx.kick.finishCycle = 0;
    }

    uint32_t fbits(float f)
    {
        uint32_t b;
        std::memcpy(&b, &f, 4);
        return b;
    }
}

bool Vu1NativeDispatcher::mscal(VU1Interpreter &vu1, PS2Memory &memory, GS &gs,
                                uint32_t startPC, uint32_t top, uint32_t itop, uint32_t maxCycles)
{
    ++m_stats.mscal;
    m_memory = &memory;
    maybePrintPeriodic();
    if (m_forceInterpreter)
    {
        ++m_stats.forcedInterpreter;
        return false;
    }
    const Vu1RecImage *image = currentImage(memory);
    if (!image)
    {
        ++m_stats.fallbackNoImage;
        return false;
    }
    const uint32_t pc = startPC & 0x3FFFu;
    if (!image->hasEntry(pc))
    {
        ++m_stats.fallbackNoEntry;
        const std::pair<uint64_t, uint32_t> key(image->hash, pc);
        if (std::find(m_unknownEntries.begin(), m_unknownEntries.end(), key) == m_unknownEntries.end())
        {
            m_unknownEntries.push_back(key);
            std::fprintf(stderr, "[vu1rec] VU1 image %016llx entry 0x%04x has no native code (interpreter fallback)\n",
                         static_cast<unsigned long long>(image->hash), pc);
        }
        return false;
    }
    if (image->hasDebugBits && (vu1.state().dBitEnabled || vu1.state().tBitEnabled))
    {
        ++m_stats.fallbackFailed; // D/T halts are only modelled by the interpreter
        return false;
    }
    if (m_captureDir)
    {
        prepareState(vu1.state(), startPC, top, itop);
        maybeCapture(image->hash, vu1.state(), memory.getVU1Data(), pc, top, itop, maxCycles, vu1.cycleCount());
    }
    if (m_diff)
    {
        runDiff(*image, vu1, memory, gs, pc, top, itop, maxCycles);
        return true;
    }

    VU1State &st = vu1.state();
    prepareState(st, startPC, top, itop);
    Vu1RecContext &ctx = *m_ctx;
    const uint64_t startCycle = vu1.cycleCount();
    resetContext(ctx, &st, memory.getVU1Data(), startCycle, maxCycles, top, itop, &memory, &gs);
    const double t0 = nowSeconds();
    image->fast(ctx, pc);
    m_vu1Seconds += nowSeconds() - t0;
    if (ctx.failed)
    {
        ++m_stats.nativeFailures;
        std::fprintf(stderr, "[vu1rec] native program failed: image=%016llx entry=0x%04x failPc=0x%04x kickErrors=%u\n",
                     static_cast<unsigned long long>(image->hash), pc, ctx.failPc, ctx.kickErrors);
    }
    ++m_stats.native;
    m_stats.vu1Cycles += ctx.cycle - startCycle;
    vu1.setCycleCount(ctx.cycle);
    return true;
}

void Vu1NativeDispatcher::maybeCapture(uint64_t hash, VU1State &st, const uint8_t *data, uint32_t pc,
                                       uint32_t top, uint32_t itop, uint32_t maxCycles, uint64_t cycle)
{
    uint64_t *seen = nullptr;
    for (auto &e : m_captureSeen)
        if (e.first == pc)
            seen = &e.second;
    if (!seen)
    {
        m_captureSeen.emplace_back(pc, 0u);
        seen = &m_captureSeen.back().second;
    }
    static const uint64_t s_captureSkip = static_cast<uint64_t>(envDouble("PS2X_VU1_CAPTURE_SKIP", 0.0));
    if (m_stats.mscal < s_captureSkip)
        return; // PS2X_VU1_CAPTURE_SKIP=<mscal count>: start capturing later in a run
    const uint64_t n = (*seen)++;
    if (!(n < 8u || (n % m_captureEvery) == 0u) || m_captureCount >= 4096u)
        return;
    char path[1024];
    std::snprintf(path, sizeof(path), "%s/snap_%016llx_%04x_%06llu.bin", m_captureDir,
                  static_cast<unsigned long long>(hash), pc, static_cast<unsigned long long>(m_captureCount++));
    if (FILE *f = std::fopen(path, "wb"))
    {
        const uint32_t header[8] = {0x53315556u /* 'VU1S' */, 1u, pc, top, itop, maxCycles,
                                    static_cast<uint32_t>(sizeof(VU1State)), 0u};
        std::fwrite(header, sizeof(header), 1, f);
        std::fwrite(&hash, sizeof(hash), 1, f);
        std::fwrite(&cycle, sizeof(cycle), 1, f);
        std::fwrite(&st, sizeof(VU1State), 1, f);
        std::fwrite(data, 1, PS2_VU1_DATA_SIZE, f);
        std::fclose(f);
    }
}

double Vu1NativeDispatcher::now() { return nowSeconds(); }

void Vu1NativeDispatcher::noteMscnt()
{
    ++m_stats.mscnt;
}

void Vu1NativeDispatcher::runDiff(const Vu1RecImage &image, VU1Interpreter &vu1, PS2Memory &memory, GS &gs,
                                  uint32_t pc, uint32_t top, uint32_t itop, uint32_t maxCycles)
{
    ++m_stats.diffCalls;
    VU1State &real = vu1.state();
    prepareState(real, pc, top, itop);
    const VU1State s0 = real;
    uint8_t *realData = memory.getVU1Data();
    static std::vector<uint8_t> d0, dFull, dFast;
    d0.assign(realData, realData + PS2_VU1_DATA_SIZE);
    dFull = d0;
    dFast = d0;
    const uint64_t c0 = vu1.cycleCount();

    // native, full-flag variant
    VU1State sFull = s0;
    PacketCapture pFull;
    resetContext(*m_ctx, &sFull, dFull.data(), c0, maxCycles, top, itop, nullptr, nullptr);
    m_ctx->sink = &PacketCapture::sink;
    m_ctx->sinkUser = &pFull;
    image.full(*m_ctx, pc);
    const bool fullFailed = m_ctx->failed;
    const uint64_t fullCycle = m_ctx->cycle;

    // native, fast variant
    VU1State sFast = s0;
    PacketCapture pFast;
    resetContext(*m_ctx2, &sFast, dFast.data(), c0, maxCycles, top, itop, nullptr, nullptr);
    m_ctx2->sink = &PacketCapture::sink;
    m_ctx2->sinkUser = &pFast;
    image.fast(*m_ctx2, pc);
    const bool fastFailed = m_ctx2->failed;
    const uint64_t fastCycle = m_ctx2->cycle;

    // interpreter (oracle) on the real state
    PacketCapture pInterp;
    vu1.setPacketSink(&PacketCapture::sink, &pInterp);
    vu1.execute(memory.getVU1Code(), PS2_VU1_CODE_SIZE, realData, PS2_VU1_DATA_SIZE, gs, &memory, pc, top, itop,
                maxCycles);
    vu1.setPacketSink(nullptr, nullptr);
    if (!vu1.lastRunEnded())
        ++m_stats.diffInterpBudget;
    const uint64_t interpCycle = vu1.cycleCount();
    m_stats.vu1Cycles += interpCycle - c0;

    auto compare = [&](const char *variant, const VU1State &s, const std::vector<uint8_t> &d,
                       const PacketCapture &pk, bool failed, uint64_t cycle, bool checkFlags,
                       bool checkClip) -> bool
    {
        std::vector<std::string> diffs;
        char buf[256];
        if (failed)
            diffs.emplace_back("native-failed");
        for (int r = 0; r < 32; ++r)
            for (int c = 0; c < 4; ++c)
                if (fbits(s.vf[r][c]) != fbits(real.vf[r][c]))
                {
                    std::snprintf(buf, sizeof(buf), "vf%d.%c native=%08x(%g) interp=%08x(%g)", r, "xyzw"[c],
                                  fbits(s.vf[r][c]), s.vf[r][c], fbits(real.vf[r][c]), real.vf[r][c]);
                    diffs.emplace_back(buf);
                }
        for (int c = 0; c < 4; ++c)
            if (fbits(s.acc[c]) != fbits(real.acc[c]))
            {
                std::snprintf(buf, sizeof(buf), "acc.%c native=%08x interp=%08x", "xyzw"[c], fbits(s.acc[c]),
                              fbits(real.acc[c]));
                diffs.emplace_back(buf);
            }
        for (int r = 0; r < 16; ++r)
            if (s.vi[r] != real.vi[r])
            {
                std::snprintf(buf, sizeof(buf), "vi%d native=%d interp=%d", r, s.vi[r], real.vi[r]);
                diffs.emplace_back(buf);
            }
        auto cmpScalar = [&](const char *name, uint32_t a, uint32_t b)
        {
            if (a != b)
            {
                std::snprintf(buf, sizeof(buf), "%s native=%08x interp=%08x", name, a, b);
                diffs.emplace_back(buf);
            }
        };
        cmpScalar("q", fbits(s.q), fbits(real.q));
        cmpScalar("p", fbits(s.p), fbits(real.p));
        cmpScalar("i", fbits(s.i), fbits(real.i));
        cmpScalar("r", s.r, real.r);
        cmpScalar("pc", s.pc, real.pc);
        if (checkFlags)
        {
            cmpScalar("mac", s.mac, real.mac);
            cmpScalar("status", s.status, real.status);
        }
        if (checkClip)
            cmpScalar("clip", s.clip, real.clip);
        if (cycle != interpCycle)
        {
            std::snprintf(buf, sizeof(buf), "cycles native=+%llu interp=+%llu",
                          static_cast<unsigned long long>(cycle - c0),
                          static_cast<unsigned long long>(interpCycle - c0));
            diffs.emplace_back(buf);
        }
        size_t firstMem = SIZE_MAX, memCount = 0;
        for (size_t i = 0; i < PS2_VU1_DATA_SIZE; ++i)
            if (d[i] != realData[i])
            {
                if (firstMem == SIZE_MAX)
                    firstMem = i;
                ++memCount;
            }
        if (memCount)
        {
            std::snprintf(buf, sizeof(buf), "data: %zu bytes differ, first at 0x%04zx (qw 0x%03zx)", memCount,
                          firstMem, firstMem / 16);
            diffs.emplace_back(buf);
        }
        if (pk.packets.size() != pInterp.packets.size())
        {
            std::snprintf(buf, sizeof(buf), "gif: packet count native=%zu interp=%zu", pk.packets.size(),
                          pInterp.packets.size());
            diffs.emplace_back(buf);
        }
        for (size_t i = 0; i < std::min(pk.packets.size(), pInterp.packets.size()); ++i)
        {
            const auto &a = pk.packets[i];
            const auto &b = pInterp.packets[i];
            if (a != b)
            {
                size_t first = 0;
                while (first < a.size() && first < b.size() && a[first] == b[first])
                    ++first;
                std::snprintf(buf, sizeof(buf), "gif: packet %zu differs (sizes %zu/%zu) first byte 0x%zx", i,
                              a.size(), b.size(), first);
                diffs.emplace_back(buf);
                break;
            }
        }
        if (diffs.empty())
            return true;
        if (m_diffLogBudget > 0)
        {
            --m_diffLogBudget;
            std::fprintf(stderr, "[vu1diff] MISMATCH variant=%s image=%016llx entry=0x%04x call=%llu (%zu diffs)\n",
                         variant, static_cast<unsigned long long>(image.hash), pc,
                         static_cast<unsigned long long>(m_stats.diffCalls), diffs.size());
            for (size_t i = 0; i < diffs.size() && i < 24; ++i)
                std::fprintf(stderr, "[vu1diff]   %s\n", diffs[i].c_str());
        }
        return false;
    };

    // The fast variant only writes back flags that some program in the image can
    // read (for the RTA image: clip only; MAC/status have no reader).
    if (!compare("full", sFull, dFull, pFull, fullFailed, fullCycle, true, true))
        ++m_stats.diffMismatchFull;
    if (!compare("fast", sFast, dFast, pFast, fastFailed, fastCycle, false, true))
        ++m_stats.diffMismatchFast;

    m_stats.diffPackets += pInterp.packets.size();
    for (const auto &packet : pInterp.packets)
    {
        m_stats.diffPacketBytes += packet.size();
        memory.submitGifPacket(GifPathId::Path1, packet.data(), static_cast<uint32_t>(packet.size()));
    }
}

void Vu1NativeDispatcher::maybePrintPeriodic()
{
    if (m_statsPeriod <= 0.0)
        return;
    if ((m_stats.mscal & 255u) != 0u)
        return;
    const double t = nowSeconds();
    if (t - m_lastStatsTime >= m_statsPeriod)
    {
        printStats("periodic");
    }
}

void Vu1NativeDispatcher::printStats(const char *why)
{
    const double t = nowSeconds();
    const double dt = std::max(1e-6, t - m_lastStatsTime);
    const Stats &s = m_stats;
    const Stats &l = m_lastPrinted;
    const uint64_t fallbacks = s.fallbackNoImage + s.fallbackNoEntry + s.fallbackFailed;
    const uint64_t swaps = m_memory ? m_memory->dispfbSwapCount() : 0u;
    const double fps = (swaps - m_lastSwaps) / dt;
    m_lastSwaps = swaps;
    const uint64_t swaps2 = m_memory ? m_memory->dispfb2SwapCount() : 0u;
    const double fps2 = (swaps2 - m_lastSwaps2) / dt;
    m_lastSwaps2 = swaps2;
    const double vu1Share = (m_vu1Seconds - m_lastVu1Seconds) / dt;
    m_lastVu1Seconds = m_vu1Seconds;
    std::fprintf(stderr,
                 "[vu1rec] %s: mscal=%llu native=%llu fallback=%llu (noimage=%llu noentry=%llu) forced=%llu "
                 "mscnt=%llu nativeFail=%llu | diff calls=%llu mismatch full=%llu fast=%llu interpBudget=%llu "
                 "| %.1f mscal/s, %.2f Mcyc/s, DISPFB1/2 changes/s %.2f (DISPFB2 %.2f), VU1 time %.1f%% over %.1fs\n",
                 why, (unsigned long long)s.mscal, (unsigned long long)s.native, (unsigned long long)fallbacks,
                 (unsigned long long)s.fallbackNoImage, (unsigned long long)s.fallbackNoEntry,
                 (unsigned long long)s.forcedInterpreter, (unsigned long long)s.mscnt,
                 (unsigned long long)s.nativeFailures, (unsigned long long)s.diffCalls,
                 (unsigned long long)s.diffMismatchFull, (unsigned long long)s.diffMismatchFast,
                 (unsigned long long)s.diffInterpBudget, (s.mscal - l.mscal) / dt,
                 (s.vu1Cycles - l.vu1Cycles) / dt / 1e6, fps, fps2, vu1Share * 100.0, dt);
    m_lastPrinted = m_stats;
    m_lastStatsTime = t;
}
