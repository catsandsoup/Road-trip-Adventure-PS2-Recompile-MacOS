#ifndef PS2_VU1_RECOMP_H
#define PS2_VU1_RECOMP_H

// Native (statically recompiled) VU1 microprograms.
//
// Generated sources (work/vu1gen/vu1rec_<hash>.cpp, produced by
// game/tools/vu1recomp/vu1recomp.py from a locally captured VU1 catalog)
// register themselves here keyed by the FNV-1a hash of the 16 KiB VU1 micro
// memory image.  The MSCAL path looks the image up (hash recomputed only when
// PS2Memory's VU1 code generation changes) and runs the native function for the
// requested start PC, or falls back to VU1Interpreter and counts the fallback.
//
// Env switches:
//   PS2X_VU1_INTERPRETER=1  force the interpreter (comparison / bisecting)
//   PS2X_VU1_DIFF=1         differential test: every MSCAL runs native code and
//                           the interpreter on identical copies and compares
//                           GIF packets, registers, flags, data memory, cycles.
//   PS2X_VU1_DIFF_LOG=<n>   number of mismatching calls to log in detail (default 8)
//   PS2X_VU1_STATS=<sec>    print counters every <sec> seconds (default 5, 0 = off)
//   PS2X_VU1_CAPTURE=<dir>  write sampled MSCAL input snapshots (VU1State + data memory)
//                           for the offline replay test (game/tests/vu1).  Disc-derived:
//                           keep the directory out of git.
//   PS2X_VU1_CAPTURE_EVERY=<n> sample every n-th MSCAL per entry (default 997; first 8 always)

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "runtime/ps2_vu1.h"

class GS;
class PS2Memory;

struct Vu1RecKick
{
    static constexpr uint32_t kBufferSize = 0x10000u;
    bool active = false;
    bool currentTagEop = false;
    uint32_t sourceAddress = 0;
    uint32_t totalBytes = 0;
    uint32_t copiedBytes = 0;
    uint32_t currentTagEnd = 0;
    uint64_t issueCycle = 0;
    uint64_t finishCycle = 0;
    std::array<uint8_t, kBufferSize> packet{};
};

using Vu1RecPacketSink = void (*)(void *user, const uint8_t *data, uint32_t size);

struct Vu1RecContext
{
    VU1State *state = nullptr;
    uint8_t *data = nullptr;          // 16 KiB VU1 data memory
    uint64_t cycle = 0;               // in: interpreter cycle at start; out: at end
    uint32_t maxCycles = 65536;
    uint32_t top = 0;
    uint32_t itop = 0;
    PS2Memory *memory = nullptr;
    GS *gs = nullptr;
    Vu1RecPacketSink sink = nullptr;  // diff mode: capture instead of submit
    void *sinkUser = nullptr;
    bool failed = false;
    uint32_t failPc = 0;
    uint32_t kickErrors = 0;
    Vu1RecKick kick;
};

using Vu1RecFn = void (*)(Vu1RecContext &ctx, uint32_t entryPc);

struct Vu1RecImage
{
    uint64_t hash = 0;
    const uint32_t *entries = nullptr;
    size_t entryCount = 0;
    Vu1RecFn fast = nullptr;
    Vu1RecFn full = nullptr;
    bool hasDebugBits = false; // a reachable D or T bit; native code treats it as FBRST.DE/TE = 0

    bool hasEntry(uint32_t pc) const
    {
        for (size_t i = 0; i < entryCount; ++i)
            if (entries[i] == pc)
                return true;
        return false;
    }
};

void vu1recRegister(const Vu1RecImage &image);
const Vu1RecImage *vu1recFind(uint64_t hash);
size_t vu1recImageCount();

struct Vu1RecRegistrar
{
    Vu1RecRegistrar(uint64_t hash, const uint32_t *entries, size_t count, Vu1RecFn fast, Vu1RecFn full,
                    bool hasDebugBits = false)
    {
        Vu1RecImage image;
        image.hash = hash;
        image.entries = entries;
        image.entryCount = count;
        image.fast = fast;
        image.full = full;
        image.hasDebugBits = hasDebugBits;
        vu1recRegister(image);
    }
};

// PATH1 copy engine (cycle-exact lazy model of VU1Interpreter's XGKICK pipeline).
void vu1rec_kick_start(Vu1RecContext &ctx, uint32_t qwordAddress, uint64_t cycle);
void vu1rec_kick_catchup(Vu1RecContext &ctx, uint64_t cycle);
uint64_t vu1rec_kick_drain(Vu1RecContext &ctx);

// MSCAL/MSCNT front end used by PS2Runtime.
class Vu1NativeDispatcher
{
public:
    Vu1NativeDispatcher();
    ~Vu1NativeDispatcher();

    // Runs the program at startPC (MSCAL).  Returns false if the interpreter
    // must be used (no native code); the call is then counted as a fallback.
    // In diff mode it runs both and always returns true (interpreter result wins).
    bool mscal(VU1Interpreter &vu1, PS2Memory &memory, GS &gs,
               uint32_t startPC, uint32_t top, uint32_t itop, uint32_t maxCycles);
    void noteMscnt();
    void noteInterpreterRun(uint64_t cycles, double seconds)
    {
        m_stats.vu1Cycles += cycles;
        m_vu1Seconds += seconds;
    }
    static double now();
    void printStats(const char *why);

private:
    struct Stats
    {
        uint64_t mscal = 0;
        uint64_t native = 0;
        uint64_t fallbackNoImage = 0;
        uint64_t fallbackNoEntry = 0;
        uint64_t fallbackFailed = 0;
        uint64_t forcedInterpreter = 0;
        uint64_t mscnt = 0;
        uint64_t nativeFailures = 0;
        uint64_t diffCalls = 0;
        uint64_t diffMismatchFull = 0;
        uint64_t diffMismatchFast = 0;
        uint64_t diffInterpBudget = 0;
        uint64_t diffPackets = 0;
        uint64_t diffPacketBytes = 0;
        uint64_t vu1Cycles = 0;
    };

    const Vu1RecImage *currentImage(PS2Memory &memory);
    void runDiff(const Vu1RecImage &image, VU1Interpreter &vu1, PS2Memory &memory, GS &gs,
                 uint32_t startPC, uint32_t top, uint32_t itop, uint32_t maxCycles);
    void maybePrintPeriodic();

    bool m_forceInterpreter = false;
    bool m_diff = false;
    int m_diffLogBudget = 8;
    double m_statsPeriod = 5.0;
    double m_lastStatsTime = 0.0;
    uint64_t m_cachedGeneration = ~0ull;
    const uint8_t *m_cachedCode = nullptr;
    uint64_t m_cachedHash = 0;
    const Vu1RecImage *m_cachedImage = nullptr;
    Stats m_stats;
    Stats m_lastPrinted;
    Vu1RecContext *m_ctx = nullptr;     // heap: holds a 64 KiB kick buffer
    Vu1RecContext *m_ctx2 = nullptr;
    std::vector<uint64_t> m_unknownHashes;
    std::vector<std::pair<uint64_t, uint32_t>> m_unknownEntries;
    void maybeCapture(uint64_t hash, VU1State &st, const uint8_t *data, uint32_t pc, uint32_t top,
                      uint32_t itop, uint32_t maxCycles, uint64_t cycle);
    const char *m_captureDir = nullptr;
    uint32_t m_captureEvery = 997;
    uint64_t m_captureCount = 0;
    std::vector<std::pair<uint32_t, uint64_t>> m_captureSeen;
    PS2Memory *m_memory = nullptr;
    uint64_t m_lastSwaps = 0;
    uint64_t m_lastSwaps2 = 0;
    double m_vu1Seconds = 0.0;
    double m_lastVu1Seconds = 0.0;
};

#endif
