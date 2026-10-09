// PS2X_TRACE_CALLS: log entry/exit of chosen recompiled guest functions (diagnostics only).
//
//   PS2X_TRACE_CALLS="21c920@a0+0:0x200,2820f8"
//     each item: <hex guest address>[@<reg>+<hex offset>:<hex length>]
//     <reg> is a0..a3, v0, sp, gp, s0..s7, or a literal hex address ("@2f0000:0x40").
//   PS2X_TRACE_CALLS_FILE=<path>   (default stderr)
//   PS2X_TRACE_CALLS_SKIP=<n>      skip the first n calls of each hook
//   PS2X_TRACE_CALLS_MAX=<n>       log at most n calls of each hook (default 64)
//   PS2X_TRACE_CALLS_SNAPSHOT=<dir> also write full guest state for every logged call:
//       <dir>/<addr>_<n>_{enter,exit}.ram (32 MiB EE RAM) and .json (GPR/FPU/VU0 registers), the
//       input and expected output of a differential run against a reference R5900 executor.
//       Disc-derived: keep under work/.
//
// Each logged call writes one "enter" line (a0-a3, ra, sp, f12-f15) and one "exit" line (v0, f0),
// each followed by a hex dump of the requested guest range taken at that moment. The hook swaps
// only the function's entry slot in the dense table; mid-function resume slots are untouched.
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace
{
    struct CallTrace
    {
        uint32_t address = 0;
        PS2Runtime::RecompiledFunction original = nullptr;
        int reg = -1;           // GPR index for the dump base, or -1 for a literal address
        uint32_t base = 0;      // literal base or offset added to the register
        uint32_t length = 0;    // bytes to dump (0 = none)
        uint64_t calls = 0;
    };

    std::vector<CallTrace> &traces()
    {
        static std::vector<CallTrace> t;
        return t;
    }
    FILE *g_out = nullptr;
    const char *g_snapshotDir = nullptr;
    uint64_t g_skip = 0, g_max = 64;
    std::mutex g_mutex;

    uint32_t fbits(float f)
    {
        uint32_t b;
        std::memcpy(&b, &f, 4);
        return b;
    }

    int regIndex(const std::string &name)
    {
        static const char *names[32] = {"zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2",
                                        "t3", "t4", "t5", "t6", "t7", "s0", "s1", "s2", "s3", "s4", "s5",
                                        "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"};
        for (int i = 0; i < 32; ++i)
            if (name == names[i])
                return i;
        return -1;
    }

    void dump(uint8_t *rdram, const CallTrace &t, R5900Context *ctx)
    {
        if (!t.length)
            return;
        const uint32_t start = (t.reg >= 0 ? GPR_U32(ctx, t.reg) : 0u) + t.base;
        for (uint32_t off = 0; off < t.length; off += 16)
        {
            std::fprintf(g_out, "  %08x:", start + off);
            for (uint32_t w = 0; w < 16 && off + w < t.length; w += 4)
            {
                const uint32_t a = (start + off + w) & (PS2_RAM_SIZE - 1u);
                uint32_t v;
                std::memcpy(&v, rdram + a, 4);
                std::fprintf(g_out, " %08x", v);
            }
            std::fputc('\n', g_out);
        }
    }


    void writeSnapshot(uint8_t *rdram, const R5900Context *ctx, uint32_t address, uint64_t n, const char *kind)
    {
        char base[1024];
        std::snprintf(base, sizeof(base), "%s/%06x_%llu_%s", g_snapshotDir, address, static_cast<unsigned long long>(n), kind);
        if (FILE *f = std::fopen((std::string(base) + ".ram").c_str(), "wb"))
        {
            std::fwrite(rdram, 1, PS2_RAM_SIZE, f);
            std::fclose(f);
        }
        FILE *j = std::fopen((std::string(base) + ".json").c_str(), "w");
        if (!j)
            return;
        auto u128 = [&](const void *p)
        {
            uint32_t w[4];
            std::memcpy(w, p, 16);
            std::fprintf(j, "[%u,%u,%u,%u]", w[0], w[1], w[2], w[3]);
        };
        std::fprintf(j, "{\"pc\":%u,\"gpr\":[", ctx->pc);
        for (int i = 0; i < 32; ++i)
        {
            u128(&ctx->r[i]);
            std::fputs(i < 31 ? "," : "],", j);
        }
        std::fprintf(j, "\"hi\":%llu,\"lo\":%llu,\"hi1\":%llu,\"lo1\":%llu,\"sa\":%u,\"fpr\":[",
                     static_cast<unsigned long long>(ctx->hi), static_cast<unsigned long long>(ctx->lo),
                     static_cast<unsigned long long>(ctx->hi1), static_cast<unsigned long long>(ctx->lo1), ctx->sa);
        for (int i = 0; i < 32; ++i)
            std::fprintf(j, "%u%s", fbits(ctx->f[i]), i < 31 ? "," : "],");
        std::fprintf(j, "\"facc\":%u,\"fcr31\":%u,\"vf\":[", fbits(ctx->f_acc), ctx->fcr31);
        for (int i = 0; i < 32; ++i)
        {
            u128(&ctx->vu0_vf[i]);
            std::fputs(i < 31 ? "," : "],", j);
        }
        std::fputs("\"vi\":[", j);
        for (int i = 0; i < 16; ++i)
            std::fprintf(j, "%u%s", ctx->vi[i], i < 15 ? "," : "],");
        std::fputs("\"vacc\":", j);
        u128(&ctx->vu0_acc);
        std::fprintf(j, ",\"q\":%u,\"p\":%u,\"i\":%u,\"vstatus\":%u,\"vmac\":%u,\"vclip\":%u}\n",
                     fbits(ctx->vu0_q), fbits(ctx->vu0_p), fbits(ctx->vu0_i), ctx->vu0_status, ctx->vu0_mac_flags,
                     ctx->vu0_clip_flags);
        std::fclose(j);
    }

    void trampoline(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        CallTrace *t = nullptr;
        for (auto &e : traces())
            if (e.address == ctx->pc)
                t = &e;
        if (!t)
            return; // unreachable: only exact entry slots are swapped
        const uint64_t n = t->calls++;
        const bool log = n >= g_skip && n < g_skip + g_max;
        if (log)
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            std::fprintf(g_out, "[call] enter %06x #%llu a0=%08x a1=%08x a2=%08x a3=%08x ra=%08x sp=%08x f12=%08x f13=%08x f14=%08x f15=%08x\n",
                         t->address, static_cast<unsigned long long>(n), GPR_U32(ctx, 4), GPR_U32(ctx, 5), GPR_U32(ctx, 6),
                         GPR_U32(ctx, 7), GPR_U32(ctx, 31), GPR_U32(ctx, 29), fbits(ctx->f[12]), fbits(ctx->f[13]),
                         fbits(ctx->f[14]), fbits(ctx->f[15]));
            dump(rdram, *t, ctx);
            std::fflush(g_out);
            if (g_snapshotDir)
                writeSnapshot(rdram, ctx, t->address, n, "enter");
        }
        const uint32_t sp = GPR_U32(ctx, 29);
        const R5900Context entry = *ctx; // the exit dump uses the entry registers for its base
        (void)entry;
        t->original(rdram, ctx, runtime);
        if (log)
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            std::fprintf(g_out, "[call] exit  %06x #%llu v0=%08x v1=%08x f0=%08x sp=%08x\n", t->address,
                         static_cast<unsigned long long>(n), GPR_U32(ctx, 2), GPR_U32(ctx, 3), fbits(ctx->f[0]), sp);
            dump(rdram, *t, const_cast<R5900Context *>(&entry));
            std::fflush(g_out);
            if (g_snapshotDir)
                writeSnapshot(rdram, ctx, t->address, n, "exit");
        }
    }
}

void ps2InstallCallTraces(PS2Runtime &runtime)
{
    const char *spec = std::getenv("PS2X_TRACE_CALLS");
    if (!spec || !*spec)
        return;
    const char *path = std::getenv("PS2X_TRACE_CALLS_FILE");
    g_out = path ? std::fopen(path, "w") : stderr;
    if (!g_out)
        g_out = stderr;
    if (const char *s = std::getenv("PS2X_TRACE_CALLS_SKIP"))
        g_skip = std::strtoull(s, nullptr, 0);
    g_snapshotDir = std::getenv("PS2X_TRACE_CALLS_SNAPSHOT");
    if (const char *s = std::getenv("PS2X_TRACE_CALLS_MAX"))
        g_max = std::strtoull(s, nullptr, 0);

    std::string all(spec);
    size_t pos = 0;
    while (pos <= all.size())
    {
        size_t comma = all.find(',', pos);
        std::string item = all.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        pos = comma == std::string::npos ? all.size() + 1 : comma + 1;
        if (item.empty())
            continue;
        CallTrace t;
        size_t at = item.find('@');
        t.address = static_cast<uint32_t>(std::strtoul(item.substr(0, at).c_str(), nullptr, 16));
        if (at != std::string::npos)
        {
            std::string rest = item.substr(at + 1);
            size_t colon = rest.find(':');
            std::string where = rest.substr(0, colon);
            t.length = colon == std::string::npos ? 0x40u : static_cast<uint32_t>(std::strtoul(rest.substr(colon + 1).c_str(), nullptr, 0));
            size_t plus = where.find('+');
            std::string regName = where.substr(0, plus);
            t.reg = regIndex(regName);
            if (t.reg < 0)
                t.base = static_cast<uint32_t>(std::strtoul(regName.c_str(), nullptr, 16));
            if (plus != std::string::npos)
                t.base += static_cast<uint32_t>(std::strtoul(where.substr(plus + 1).c_str(), nullptr, 0));
        }
        if (!runtime.hasFunction(t.address))
        {
            std::fprintf(stderr, "[call] no recompiled function at %06x\n", t.address);
            continue;
        }
        t.original = runtime.lookupFunction(t.address);
        traces().push_back(t);
    }
    for (const auto &t : traces())
    {
        runtime.replaceFunction(t.address, &trampoline);
        std::fprintf(stderr, "[call] tracing %06x\n", t.address);
    }
}
