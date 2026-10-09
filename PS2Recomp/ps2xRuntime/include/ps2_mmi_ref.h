#ifndef PS2_MMI_REF_H
#define PS2_MMI_REF_H
// Reference implementations of the EE MMI instructions whose SIMD one-liners were wrong (track-A
// encoding audit, 2026-10-08). Each helper is a line-by-line transcription of PCSX2
// pcsx2/MMI.cpp (R5900::Interpreter::OpcodeImpl::MMI, master 2026-10) on this runtime's register
// layout: HI = {ctx->hi (bits 63..0), ctx->hi1 (bits 127..64)}, LO = {ctx->lo, ctx->lo1}.
// Deliberate deviation: PMADDW/PMSUBW use the architectural result of the EE Core Instruction Set
// Manual (HI:LO += / -= rs*rt, 64-bit) instead of PCSX2's empirical "+0x70000000 / 4294967295"
// model of the HI word.
// Operands are read in full before rd is written, so rd may alias rs/rt.
#include <cstdint>
#include <cstring>

struct Ps2MmiReg
{
    union
    {
        uint64_t UD[2];
        int64_t SD[2];
        uint32_t UL[4];
        int32_t SL[4];
        uint16_t US[8];
        int16_t SS[8];
    };
};

template <typename Ctx>
inline Ps2MmiReg ps2_mmi_get(const Ctx *ctx, int r)
{
    Ps2MmiReg v{};
    if (r != 0)
        std::memcpy(&v, &ctx->r[r], 16);
    return v;
}
template <typename Ctx>
inline void ps2_mmi_set(Ctx *ctx, int r, const Ps2MmiReg &v)
{
    if (r != 0)
        std::memcpy(&ctx->r[r], &v, 16);
}
template <typename Ctx>
inline Ps2MmiReg ps2_mmi_hi(const Ctx *ctx)
{
    Ps2MmiReg v;
    v.UD[0] = ctx->hi;
    v.UD[1] = ctx->hi1;
    return v;
}
template <typename Ctx>
inline Ps2MmiReg ps2_mmi_lo(const Ctx *ctx)
{
    Ps2MmiReg v;
    v.UD[0] = ctx->lo;
    v.UD[1] = ctx->lo1;
    return v;
}
template <typename Ctx>
inline void ps2_mmi_set_hilo(Ctx *ctx, const Ps2MmiReg &hi, const Ps2MmiReg &lo)
{
    ctx->hi = hi.UD[0];
    ctx->hi1 = hi.UD[1];
    ctx->lo = lo.UD[0];
    ctx->lo1 = lo.UD[1];
}
// rd.UL = {LO.UL[0], HI.UL[0], LO.UL[2], HI.UL[2]} (PMADDH/PHMADH/PMSUBH/PHMSBH/PMULTH tail)
template <typename Ctx>
inline void ps2_mmi_rd_from_hilo_even(Ctx *ctx, int rd, const Ps2MmiReg &hi, const Ps2MmiReg &lo)
{
    Ps2MmiReg d;
    d.UL[0] = lo.UL[0];
    d.UL[1] = hi.UL[0];
    d.UL[2] = lo.UL[2];
    d.UL[3] = hi.UL[2];
    ps2_mmi_set(ctx, rd, d);
}

// ---- shifts by immediate on halfwords: sa & 0xF (PCSX2 _PSLLH/_PSRLH/_PSRAH) ----
template <typename Ctx>
inline void ps2_mmi_psllh(Ctx *ctx, int rd, int rt, int sa)
{
    Ps2MmiReg t = ps2_mmi_get(ctx, rt), d;
    for (int n = 0; n < 8; ++n)
        d.US[n] = static_cast<uint16_t>(t.US[n] << (sa & 0xF));
    ps2_mmi_set(ctx, rd, d);
}
template <typename Ctx>
inline void ps2_mmi_psrlh(Ctx *ctx, int rd, int rt, int sa)
{
    Ps2MmiReg t = ps2_mmi_get(ctx, rt), d;
    for (int n = 0; n < 8; ++n)
        d.US[n] = static_cast<uint16_t>(t.US[n] >> (sa & 0xF));
    ps2_mmi_set(ctx, rd, d);
}
template <typename Ctx>
inline void ps2_mmi_psrah(Ctx *ctx, int rd, int rt, int sa)
{
    Ps2MmiReg t = ps2_mmi_get(ctx, rt), d;
    for (int n = 0; n < 8; ++n)
        d.US[n] = static_cast<uint16_t>(t.SS[n] >> (sa & 0xF));
    ps2_mmi_set(ctx, rd, d);
}

// ---- MMI1: PABSW/PABSH read rt; PADDUH/PSUBUH saturate unsigned ----
template <typename Ctx>
inline void ps2_mmi_pabsw(Ctx *ctx, int rd, int rt)
{
    Ps2MmiReg t = ps2_mmi_get(ctx, rt), d;
    for (int n = 0; n < 4; ++n)
        d.UL[n] = (t.UL[n] == 0x80000000u) ? 0x7FFFFFFFu : (t.SL[n] < 0 ? static_cast<uint32_t>(-t.SL[n]) : t.UL[n]);
    ps2_mmi_set(ctx, rd, d);
}
template <typename Ctx>
inline void ps2_mmi_pabsh(Ctx *ctx, int rd, int rt)
{
    Ps2MmiReg t = ps2_mmi_get(ctx, rt), d;
    for (int n = 0; n < 8; ++n)
        d.US[n] = (t.US[n] == 0x8000u) ? 0x7FFFu : (t.SS[n] < 0 ? static_cast<uint16_t>(-t.SS[n]) : t.US[n]);
    ps2_mmi_set(ctx, rd, d);
}
template <typename Ctx>
inline void ps2_mmi_padduh(Ctx *ctx, int rd, int rs, int rt)
{
    Ps2MmiReg s = ps2_mmi_get(ctx, rs), t = ps2_mmi_get(ctx, rt), d;
    for (int n = 0; n < 8; ++n)
    {
        const int32_t v = static_cast<int32_t>(s.US[n]) + static_cast<int32_t>(t.US[n]);
        d.US[n] = v > 0xFFFF ? 0xFFFFu : static_cast<uint16_t>(v);
    }
    ps2_mmi_set(ctx, rd, d);
}
template <typename Ctx>
inline void ps2_mmi_psubuh(Ctx *ctx, int rd, int rs, int rt)
{
    Ps2MmiReg s = ps2_mmi_get(ctx, rs), t = ps2_mmi_get(ctx, rt), d;
    for (int n = 0; n < 8; ++n)
    {
        const int32_t v = static_cast<int32_t>(s.US[n]) - static_cast<int32_t>(t.US[n]);
        d.US[n] = v <= 0 ? 0u : static_cast<uint16_t>(v);
    }
    ps2_mmi_set(ctx, rd, d);
}

// ---- MMI2/MMI3 variable word shifts: lanes 0 and 2 only, result sign-extended to 64 bits ----
template <typename Ctx>
inline void ps2_mmi_psllvw(Ctx *ctx, int rd, int rs, int rt)
{
    Ps2MmiReg s = ps2_mmi_get(ctx, rs), t = ps2_mmi_get(ctx, rt), d;
    d.SD[0] = static_cast<int32_t>(t.UL[0] << (s.UL[0] & 0x1F));
    d.SD[1] = static_cast<int32_t>(t.UL[2] << (s.UL[2] & 0x1F));
    ps2_mmi_set(ctx, rd, d);
}
template <typename Ctx>
inline void ps2_mmi_psrlvw(Ctx *ctx, int rd, int rs, int rt)
{
    Ps2MmiReg s = ps2_mmi_get(ctx, rs), t = ps2_mmi_get(ctx, rt), d;
    d.SD[0] = static_cast<int32_t>(t.UL[0] >> (s.UL[0] & 0x1F));
    d.SD[1] = static_cast<int32_t>(t.UL[2] >> (s.UL[2] & 0x1F));
    ps2_mmi_set(ctx, rd, d);
}
template <typename Ctx>
inline void ps2_mmi_psravw(Ctx *ctx, int rd, int rs, int rt)
{
    Ps2MmiReg s = ps2_mmi_get(ctx, rs), t = ps2_mmi_get(ctx, rt), d;
    d.SD[0] = static_cast<int64_t>(t.SL[0] >> (s.UL[0] & 0x1F));
    d.SD[1] = static_cast<int64_t>(t.SL[2] >> (s.UL[2] & 0x1F));
    ps2_mmi_set(ctx, rd, d);
}

// ---- HI/LO moves (full 128 bits) ----
template <typename Ctx>
inline void ps2_mmi_pmfhi(Ctx *ctx, int rd) { ps2_mmi_set(ctx, rd, ps2_mmi_hi(ctx)); }
template <typename Ctx>
inline void ps2_mmi_pmflo(Ctx *ctx, int rd) { ps2_mmi_set(ctx, rd, ps2_mmi_lo(ctx)); }
template <typename Ctx>
inline void ps2_mmi_pmthi(Ctx *ctx, int rs)
{
    Ps2MmiReg s = ps2_mmi_get(ctx, rs);
    ctx->hi = s.UD[0];
    ctx->hi1 = s.UD[1];
}
template <typename Ctx>
inline void ps2_mmi_pmtlo(Ctx *ctx, int rs)
{
    Ps2MmiReg s = ps2_mmi_get(ctx, rs);
    ctx->lo = s.UD[0];
    ctx->lo1 = s.UD[1];
}
inline uint16_t ps2_mmi_pmfhl_clamp(int32_t src)
{
    if (src > 0x7FFF)
        return 0x7FFF;
    if (src < -0x8000)
        return 0x8000;
    return static_cast<uint16_t>(src);
}
template <typename Ctx>
inline void ps2_mmi_pmfhl(Ctx *ctx, int rd, int sa)
{
    const Ps2MmiReg hi = ps2_mmi_hi(ctx), lo = ps2_mmi_lo(ctx);
    if (rd == 0)
        return;
    Ps2MmiReg d = ps2_mmi_get(ctx, rd);
    switch (sa)
    {
    case 0: // LW
        d.UL[0] = lo.UL[0]; d.UL[1] = hi.UL[0]; d.UL[2] = lo.UL[2]; d.UL[3] = hi.UL[2];
        break;
    case 1: // UW
        d.UL[0] = lo.UL[1]; d.UL[1] = hi.UL[1]; d.UL[2] = lo.UL[3]; d.UL[3] = hi.UL[3];
        break;
    case 2: // SLW
        for (int k = 0; k < 2; ++k)
        {
            const int64_t t = static_cast<int64_t>((static_cast<uint64_t>(hi.UL[2 * k]) << 32) | lo.UL[2 * k]);
            if (t >= 0x000000007FFFFFFFLL)
                d.UD[k] = 0x000000007FFFFFFFULL;
            else if (t <= -0x80000000LL)
                d.UD[k] = 0xFFFFFFFF80000000ULL;
            else
                d.SD[k] = lo.SL[2 * k];
        }
        break;
    case 3: // LH
        d.US[0] = lo.US[0]; d.US[1] = lo.US[2]; d.US[2] = hi.US[0]; d.US[3] = hi.US[2];
        d.US[4] = lo.US[4]; d.US[5] = lo.US[6]; d.US[6] = hi.US[4]; d.US[7] = hi.US[6];
        break;
    case 4: // SH
        d.US[0] = ps2_mmi_pmfhl_clamp(lo.SL[0]); d.US[1] = ps2_mmi_pmfhl_clamp(lo.SL[1]);
        d.US[2] = ps2_mmi_pmfhl_clamp(hi.SL[0]); d.US[3] = ps2_mmi_pmfhl_clamp(hi.SL[1]);
        d.US[4] = ps2_mmi_pmfhl_clamp(lo.SL[2]); d.US[5] = ps2_mmi_pmfhl_clamp(lo.SL[3]);
        d.US[6] = ps2_mmi_pmfhl_clamp(hi.SL[2]); d.US[7] = ps2_mmi_pmfhl_clamp(hi.SL[3]);
        break;
    default:
        return;
    }
    ps2_mmi_set(ctx, rd, d);
}
template <typename Ctx>
inline void ps2_mmi_pmthl(Ctx *ctx, int rs, int sa)
{
    if (sa != 0)
        return;
    const Ps2MmiReg s = ps2_mmi_get(ctx, rs);
    Ps2MmiReg hi = ps2_mmi_hi(ctx), lo = ps2_mmi_lo(ctx);
    lo.UL[0] = s.UL[0]; hi.UL[0] = s.UL[1]; lo.UL[2] = s.UL[2]; hi.UL[2] = s.UL[3];
    ps2_mmi_set_hilo(ctx, hi, lo);
}

// ---- halfword/word permutes ----
template <typename Ctx>
inline void ps2_mmi_permute_h(Ctx *ctx, int rd, int rt, const int (&idx)[8])
{
    const Ps2MmiReg t = ps2_mmi_get(ctx, rt);
    Ps2MmiReg d;
    for (int n = 0; n < 8; ++n)
        d.US[n] = t.US[idx[n]];
    ps2_mmi_set(ctx, rd, d);
}
template <typename Ctx>
inline void ps2_mmi_pexeh(Ctx *ctx, int rd, int rt) { static const int i[8] = {2, 1, 0, 3, 6, 5, 4, 7}; ps2_mmi_permute_h(ctx, rd, rt, i); }
template <typename Ctx>
inline void ps2_mmi_prevh(Ctx *ctx, int rd, int rt) { static const int i[8] = {3, 2, 1, 0, 7, 6, 5, 4}; ps2_mmi_permute_h(ctx, rd, rt, i); }
template <typename Ctx>
inline void ps2_mmi_pexch(Ctx *ctx, int rd, int rt) { static const int i[8] = {0, 2, 1, 3, 4, 6, 5, 7}; ps2_mmi_permute_h(ctx, rd, rt, i); }
template <typename Ctx>
inline void ps2_mmi_pexew(Ctx *ctx, int rd, int rt)
{
    const Ps2MmiReg t = ps2_mmi_get(ctx, rt);
    Ps2MmiReg d;
    d.UL[0] = t.UL[2]; d.UL[1] = t.UL[1]; d.UL[2] = t.UL[0]; d.UL[3] = t.UL[3];
    ps2_mmi_set(ctx, rd, d);
}
template <typename Ctx>
inline void ps2_mmi_pexcw(Ctx *ctx, int rd, int rt)
{
    const Ps2MmiReg t = ps2_mmi_get(ctx, rt);
    Ps2MmiReg d;
    d.UL[0] = t.UL[0]; d.UL[1] = t.UL[2]; d.UL[2] = t.UL[1]; d.UL[3] = t.UL[3];
    ps2_mmi_set(ctx, rd, d);
}
template <typename Ctx>
inline void ps2_mmi_pinth(Ctx *ctx, int rd, int rs, int rt)
{
    const Ps2MmiReg s = ps2_mmi_get(ctx, rs), t = ps2_mmi_get(ctx, rt);
    Ps2MmiReg d;
    for (int k = 0; k < 4; ++k)
    {
        d.US[2 * k] = t.US[k];
        d.US[2 * k + 1] = s.US[4 + k];
    }
    ps2_mmi_set(ctx, rd, d);
}
template <typename Ctx>
inline void ps2_mmi_pinteh(Ctx *ctx, int rd, int rs, int rt)
{
    const Ps2MmiReg s = ps2_mmi_get(ctx, rs), t = ps2_mmi_get(ctx, rt);
    Ps2MmiReg d;
    for (int k = 0; k < 4; ++k)
    {
        d.US[2 * k] = t.US[2 * k];
        d.US[2 * k + 1] = s.US[2 * k];
    }
    ps2_mmi_set(ctx, rd, d);
}

// ---- word multiply / divide (lanes 0 and 2 -> HI/LO dd = 0, HI1/LO1 dd = 1) ----
template <typename Ctx>
inline void ps2_mmi_pmultw(Ctx *ctx, int rd, int rs, int rt)
{
    const Ps2MmiReg s = ps2_mmi_get(ctx, rs), t = ps2_mmi_get(ctx, rt);
    Ps2MmiReg hi = ps2_mmi_hi(ctx), lo = ps2_mmi_lo(ctx), d = ps2_mmi_get(ctx, rd);
    for (int dd = 0; dd < 2; ++dd)
    {
        const int ss = 2 * dd;
        const int64_t p = static_cast<int64_t>(s.SL[ss]) * static_cast<int64_t>(t.SL[ss]);
        lo.SD[dd] = static_cast<int32_t>(p & 0xFFFFFFFF);
        hi.SD[dd] = static_cast<int32_t>(p >> 32);
        d.SD[dd] = p;
    }
    ps2_mmi_set_hilo(ctx, hi, lo);
    ps2_mmi_set(ctx, rd, d);
}
template <typename Ctx>
inline void ps2_mmi_pmultuw(Ctx *ctx, int rd, int rs, int rt)
{
    const Ps2MmiReg s = ps2_mmi_get(ctx, rs), t = ps2_mmi_get(ctx, rt);
    Ps2MmiReg hi = ps2_mmi_hi(ctx), lo = ps2_mmi_lo(ctx), d = ps2_mmi_get(ctx, rd);
    for (int dd = 0; dd < 2; ++dd)
    {
        const int ss = 2 * dd;
        const uint64_t p = static_cast<uint64_t>(s.UL[ss]) * static_cast<uint64_t>(t.UL[ss]);
        lo.SD[dd] = static_cast<int32_t>(p & 0xFFFFFFFF);
        hi.SD[dd] = static_cast<int32_t>(p >> 32);
        d.UD[dd] = p;
    }
    ps2_mmi_set_hilo(ctx, hi, lo);
    ps2_mmi_set(ctx, rd, d);
}
template <typename Ctx>
inline void ps2_mmi_pmaddw_sub(Ctx *ctx, int rd, int rs, int rt, bool sub)
{
    const Ps2MmiReg s = ps2_mmi_get(ctx, rs), t = ps2_mmi_get(ctx, rt);
    Ps2MmiReg hi = ps2_mmi_hi(ctx), lo = ps2_mmi_lo(ctx), d = ps2_mmi_get(ctx, rd);
    for (int dd = 0; dd < 2; ++dd)
    {
        const int ss = 2 * dd;
        const int64_t p = static_cast<int64_t>(s.SL[ss]) * static_cast<int64_t>(t.SL[ss]);
        const uint64_t acc = (static_cast<uint64_t>(hi.UL[ss]) << 32) | lo.UL[ss];
        const uint64_t r = sub ? acc - static_cast<uint64_t>(p) : acc + static_cast<uint64_t>(p);
        lo.SD[dd] = static_cast<int32_t>(r & 0xFFFFFFFF);
        hi.SD[dd] = static_cast<int32_t>(r >> 32);
        d.UL[2 * dd] = lo.UL[2 * dd];
        d.UL[2 * dd + 1] = hi.UL[2 * dd];
    }
    ps2_mmi_set_hilo(ctx, hi, lo);
    ps2_mmi_set(ctx, rd, d);
}
template <typename Ctx>
inline void ps2_mmi_pmaddw(Ctx *ctx, int rd, int rs, int rt) { ps2_mmi_pmaddw_sub(ctx, rd, rs, rt, false); }
template <typename Ctx>
inline void ps2_mmi_pmsubw(Ctx *ctx, int rd, int rs, int rt) { ps2_mmi_pmaddw_sub(ctx, rd, rs, rt, true); }
template <typename Ctx>
inline void ps2_mmi_pmadduw(Ctx *ctx, int rd, int rs, int rt)
{
    const Ps2MmiReg s = ps2_mmi_get(ctx, rs), t = ps2_mmi_get(ctx, rt);
    Ps2MmiReg hi = ps2_mmi_hi(ctx), lo = ps2_mmi_lo(ctx), d = ps2_mmi_get(ctx, rd);
    for (int dd = 0; dd < 2; ++dd)
    {
        const int ss = 2 * dd;
        const uint64_t r = ((static_cast<uint64_t>(hi.UL[ss]) << 32) | lo.UL[ss]) +
                           static_cast<uint64_t>(s.UL[ss]) * static_cast<uint64_t>(t.UL[ss]);
        lo.SD[dd] = static_cast<int32_t>(r & 0xFFFFFFFF);
        hi.SD[dd] = static_cast<int32_t>(r >> 32);
        d.UD[dd] = r;
    }
    ps2_mmi_set_hilo(ctx, hi, lo);
    ps2_mmi_set(ctx, rd, d);
}
template <typename Ctx>
inline void ps2_mmi_pdivw(Ctx *ctx, int rs, int rt)
{
    const Ps2MmiReg s = ps2_mmi_get(ctx, rs), t = ps2_mmi_get(ctx, rt);
    Ps2MmiReg hi = ps2_mmi_hi(ctx), lo = ps2_mmi_lo(ctx);
    for (int dd = 0; dd < 2; ++dd)
    {
        const int ss = 2 * dd;
        if (s.UL[ss] == 0x80000000u && t.UL[ss] == 0xFFFFFFFFu)
        {
            lo.SD[dd] = static_cast<int32_t>(0x80000000u);
            hi.SD[dd] = 0;
        }
        else if (t.SL[ss] != 0)
        {
            lo.SD[dd] = s.SL[ss] / t.SL[ss];
            hi.SD[dd] = s.SL[ss] % t.SL[ss];
        }
        else
        {
            lo.SD[dd] = (s.SL[ss] < 0) ? 1 : -1;
            hi.SD[dd] = s.SL[ss];
        }
    }
    ps2_mmi_set_hilo(ctx, hi, lo);
}
template <typename Ctx>
inline void ps2_mmi_pdivuw(Ctx *ctx, int rs, int rt)
{
    const Ps2MmiReg s = ps2_mmi_get(ctx, rs), t = ps2_mmi_get(ctx, rt);
    Ps2MmiReg hi = ps2_mmi_hi(ctx), lo = ps2_mmi_lo(ctx);
    for (int dd = 0; dd < 2; ++dd)
    {
        const int ss = 2 * dd;
        if (t.UL[ss] != 0)
        {
            lo.SD[dd] = static_cast<int32_t>(s.UL[ss] / t.UL[ss]);
            hi.SD[dd] = static_cast<int32_t>(s.UL[ss] % t.UL[ss]);
        }
        else
        {
            lo.SD[dd] = -1;
            hi.SD[dd] = s.SL[ss];
        }
    }
    ps2_mmi_set_hilo(ctx, hi, lo);
}
template <typename Ctx>
inline void ps2_mmi_pdivbw(Ctx *ctx, int rs, int rt)
{
    const Ps2MmiReg s = ps2_mmi_get(ctx, rs), t = ps2_mmi_get(ctx, rt);
    Ps2MmiReg hi = ps2_mmi_hi(ctx), lo = ps2_mmi_lo(ctx);
    for (int n = 0; n < 4; ++n)
    {
        if (s.UL[n] == 0x80000000u && t.US[0] == 0xFFFFu)
        {
            lo.SL[n] = static_cast<int32_t>(0x80000000u);
            hi.SL[n] = 0;
        }
        else if (t.US[0] != 0)
        {
            lo.SL[n] = s.SL[n] / t.SS[0];
            hi.SL[n] = s.SL[n] % t.SS[0];
        }
        else
        {
            lo.SL[n] = (s.SL[n] < 0) ? 1 : -1;
            hi.SL[n] = s.SL[n];
        }
    }
    ps2_mmi_set_hilo(ctx, hi, lo);
}

// ---- halfword multiply(-accumulate): lane order LO.UL0 LO.UL1 HI.UL0 HI.UL1 LO.UL2 LO.UL3 HI.UL2 HI.UL3 ----
template <typename Ctx>
inline void ps2_mmi_pmh(Ctx *ctx, int rd, int rs, int rt, int mode) // 0 = PMULTH, 1 = PMADDH, 2 = PMSUBH
{
    const Ps2MmiReg s = ps2_mmi_get(ctx, rs), t = ps2_mmi_get(ctx, rt);
    Ps2MmiReg hi = ps2_mmi_hi(ctx), lo = ps2_mmi_lo(ctx);
    uint32_t *dst[8] = {&lo.UL[0], &lo.UL[1], &hi.UL[0], &hi.UL[1], &lo.UL[2], &lo.UL[3], &hi.UL[2], &hi.UL[3]};
    for (int n = 0; n < 8; ++n)
    {
        const uint32_t p = static_cast<uint32_t>(static_cast<int32_t>(s.SS[n]) * static_cast<int32_t>(t.SS[n]));
        *dst[n] = mode == 0 ? p : (mode == 1 ? *dst[n] + p : *dst[n] - p);
    }
    ps2_mmi_set_hilo(ctx, hi, lo);
    ps2_mmi_rd_from_hilo_even(ctx, rd, hi, lo);
}
template <typename Ctx>
inline void ps2_mmi_pmulth(Ctx *ctx, int rd, int rs, int rt) { ps2_mmi_pmh(ctx, rd, rs, rt, 0); }
template <typename Ctx>
inline void ps2_mmi_pmaddh(Ctx *ctx, int rd, int rs, int rt) { ps2_mmi_pmh(ctx, rd, rs, rt, 1); }
template <typename Ctx>
inline void ps2_mmi_pmsubh(Ctx *ctx, int rd, int rs, int rt) { ps2_mmi_pmh(ctx, rd, rs, rt, 2); }
// PHMADH / PHMSBH: pairs (n, n+1) -> word dd (sum/difference) and dd+1 (firsttemp, ~firsttemp for PHMSBH)
template <typename Ctx>
inline void ps2_mmi_phm(Ctx *ctx, int rd, int rs, int rt, bool sub)
{
    const Ps2MmiReg s = ps2_mmi_get(ctx, rs), t = ps2_mmi_get(ctx, rt);
    Ps2MmiReg hi = ps2_mmi_hi(ctx), lo = ps2_mmi_lo(ctx);
    const int pairs[4][2] = {{0, 0}, {0, 2}, {2, 4}, {2, 6}}; // {dd, n}; even entries -> LO, odd -> HI
    for (int k = 0; k < 4; ++k)
    {
        const int dd = pairs[k][0], n = pairs[k][1];
        const int32_t first = static_cast<int32_t>(s.SS[n + 1]) * static_cast<int32_t>(t.SS[n + 1]);
        const int32_t second = static_cast<int32_t>(s.SS[n]) * static_cast<int32_t>(t.SS[n]);
        const uint32_t res = sub ? static_cast<uint32_t>(first) - static_cast<uint32_t>(second)
                                 : static_cast<uint32_t>(first) + static_cast<uint32_t>(second);
        Ps2MmiReg &dst = (k & 1) ? hi : lo;
        dst.UL[dd] = res;
        dst.UL[dd + 1] = sub ? ~static_cast<uint32_t>(first) : static_cast<uint32_t>(first);
    }
    ps2_mmi_set_hilo(ctx, hi, lo);
    ps2_mmi_rd_from_hilo_even(ctx, rd, hi, lo);
}
template <typename Ctx>
inline void ps2_mmi_phmadh(Ctx *ctx, int rd, int rs, int rt) { ps2_mmi_phm(ctx, rd, rs, rt, false); }
template <typename Ctx>
inline void ps2_mmi_phmsbh(Ctx *ctx, int rd, int rs, int rt) { ps2_mmi_phm(ctx, rd, rs, rt, true); }

#endif // PS2_MMI_REF_H
