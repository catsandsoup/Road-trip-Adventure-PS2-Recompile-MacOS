#ifndef PS2_VU1_RECOMP_SUPPORT_H
#define PS2_VU1_RECOMP_SUPPORT_H

// Inline helpers used only by generated VU1 microprogram code
// (work/vu1gen/vu1rec_<hash>.cpp).  Every helper mirrors one piece of
// VU1Interpreter semantics (ps2_vu1_core/upper/lower.cpp); scalar helpers
// that the interpreter computes with clang-contracted expressions are copied
// verbatim so the same contraction happens.  Generated TUs are compiled with
// -ffp-contract=off -frounding-math; fused operations are spelled explicitly
// (vfmaq_f32 / vfmsq_f32) exactly where the interpreter's code is fused.
//
// All generated code runs with FE_TOWARDZERO (as VU1Interpreter::run does).

#include "runtime/ps2_vu1_recomp.h"

#include <arm_neon.h>
#include <cfenv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

struct Vu1RecRoundingGuard
{
    int previous;
    bool ok;
    Vu1RecRoundingGuard() : previous(std::fegetround()), ok(std::fesetround(FE_TOWARDZERO) == 0) {}
    ~Vu1RecRoundingGuard()
    {
        if (ok && previous != -1)
            std::fesetround(previous);
    }
};

static inline float vr_bits2f(uint32_t b)
{
    float f;
    std::memcpy(&f, &b, 4);
    return f;
}

static inline uint32_t vr_f2bits(float f)
{
    uint32_t b;
    std::memcpy(&b, &f, 4);
    return b;
}

static inline float32x4_t vr_load_vf(const float *p) { return vld1q_f32(p); }
static inline void vr_store_vf(float *p, float32x4_t v) { vst1q_f32(p, v); }

// normalizeOperand: exponent 0 -> signed zero, exponent 255 -> signed FLT_MAX.
static inline float32x4_t vr_norm(float32x4_t v)
{
    uint32x4_t b = vreinterpretq_u32_f32(v);
    const uint32x4_t e = vandq_u32(b, vdupq_n_u32(0x7F800000u));
    const uint32x4_t sign = vandq_u32(b, vdupq_n_u32(0x80000000u));
    b = vbslq_u32(vceqzq_u32(e), sign, b);
    b = vbslq_u32(vceqq_u32(e, vdupq_n_u32(0x7F800000u)), vorrq_u32(sign, vdupq_n_u32(0x7F7FFFFFu)), b);
    return vreinterpretq_f32_u32(b);
}

// FMAC result normalization.  With clamped operands and round-toward-zero the
// only case that occurs is a denormal (|exact| < FLT_MIN) -> signed zero; the
// exponent-255 clamp is kept for completeness (it is one select).
static inline float32x4_t vr_flush(float32x4_t v) { return vr_norm(v); }

static inline float vr_normf(float v)
{
    uint32_t b = vr_f2bits(v);
    const uint32_t e = (b >> 23) & 0xFFu;
    if (e == 0u)
        b &= 0x80000000u;
    else if (e == 0xFFu)
        b = (b & 0x80000000u) | 0x7F7FFFFFu;
    return vr_bits2f(b);
}

// normalizeResult (value part)
static inline float vr_normf_result(float v) { return vr_normf(v); }

template <unsigned D>
static inline float32x4_t vr_blend(float32x4_t n, float32x4_t o)
{
    const uint32x4_t m = {(D & 8u) ? 0xFFFFFFFFu : 0u, (D & 4u) ? 0xFFFFFFFFu : 0u,
                          (D & 2u) ? 0xFFFFFFFFu : 0u, (D & 1u) ? 0xFFFFFFFFu : 0u};
    return vbslq_f32(m, n, o);
}

template <int L>
static inline float32x4_t vr_dup(float32x4_t v) { return vdupq_laneq_f32(v, L); }

// MAX/MINI: (a > b) ? a : b  /  (a < b) ? a : b on clamped operands.
static inline float32x4_t vr_max(float32x4_t a, float32x4_t b) { return vbslq_f32(vcgtq_f32(a, b), a, b); }
static inline float32x4_t vr_min(float32x4_t a, float32x4_t b) { return vbslq_f32(vcltq_f32(a, b), a, b); }

// OPMSUB: acc.xyz - vs.yzx * vt.zxy (fused, as clang compiles the interpreter), w = 0.
static inline float32x4_t vr_yzxw(float32x4_t v)
{
    const uint8x16_t idx = {4, 5, 6, 7, 8, 9, 10, 11, 0, 1, 2, 3, 12, 13, 14, 15};
    return vreinterpretq_f32_u8(vqtbl1q_u8(vreinterpretq_u8_f32(v), idx));
}
static inline float32x4_t vr_zxyw(float32x4_t v)
{
    const uint8x16_t idx = {8, 9, 10, 11, 0, 1, 2, 3, 4, 5, 6, 7, 12, 13, 14, 15};
    return vreinterpretq_f32_u8(vqtbl1q_u8(vreinterpretq_u8_f32(v), idx));
}
static inline float32x4_t vr_opmsub(float32x4_t acc, float32x4_t s, float32x4_t t)
{
    return vsetq_lane_f32(0.0f, vfmsq_f32(acc, vr_yzxw(s), vr_zxyw(t)), 3);
}
static inline float32x4_t vr_opmula(float32x4_t s, float32x4_t t)
{
    return vsetq_lane_f32(0.0f, vmulq_f32(vr_yzxw(s), vr_zxyw(t)), 3);
}

// Memory (VU1 data memory, 16 KiB, addresses already masked).
static inline float32x4_t vr_ld(const uint8_t *p) { return vreinterpretq_f32_u8(vld1q_u8(p)); }

template <unsigned D>
static inline void vr_st(uint8_t *p, float32x4_t v)
{
    if constexpr (D == 0xFu)
    {
        vst1q_u8(p, vreinterpretq_u8_f32(v));
    }
    else
    {
        const uint32x4_t b = vreinterpretq_u32_f32(v);
        if constexpr ((D & 8u) != 0u)
        {
            const uint32_t w = vgetq_lane_u32(b, 0);
            std::memcpy(p + 0, &w, 4);
        }
        if constexpr ((D & 4u) != 0u)
        {
            const uint32_t w = vgetq_lane_u32(b, 1);
            std::memcpy(p + 4, &w, 4);
        }
        if constexpr ((D & 2u) != 0u)
        {
            const uint32_t w = vgetq_lane_u32(b, 2);
            std::memcpy(p + 8, &w, 4);
        }
        if constexpr ((D & 1u) != 0u)
        {
            const uint32_t w = vgetq_lane_u32(b, 3);
            std::memcpy(p + 12, &w, 4);
        }
    }
}

template <unsigned D>
static inline void vr_stw(uint8_t *p, uint32_t w)
{
    if constexpr ((D & 8u) != 0u)
        std::memcpy(p + 0, &w, 4);
    if constexpr ((D & 4u) != 0u)
        std::memcpy(p + 4, &w, 4);
    if constexpr ((D & 2u) != 0u)
        std::memcpy(p + 8, &w, 4);
    if constexpr ((D & 1u) != 0u)
        std::memcpy(p + 12, &w, 4);
}

static inline uint32_t vr_ld32(const uint8_t *p)
{
    uint32_t w;
    std::memcpy(&w, p, 4);
    return w;
}

// CLIP (raw bits, as VU1Interpreter::execUpper special 0x1F).
static inline uint32_t vr_clip_flags(float32x4_t vs, float32x4_t vt)
{
    const uint32_t wBits = vgetq_lane_u32(vreinterpretq_u32_f32(vt), 3);
    const int32_t limit = (wBits & 0x7F800000u) != 0u ? static_cast<int32_t>(wBits & 0x7FFFFFFFu) : 0x007FFFFF;
    const uint32x4_t b = vreinterpretq_u32_f32(vs);
    const int32x4_t pos = vreinterpretq_s32_u32(b);
    const int32x4_t neg = vreinterpretq_s32_u32(veorq_u32(b, vdupq_n_u32(0x80000000u)));
    const int32x4_t lim = vdupq_n_s32(limit);
    const uint32x4_t gp = vcgtq_s32(pos, lim);
    const uint32x4_t gn = vcgtq_s32(neg, lim);
    uint32_t flags = 0u;
    flags |= (vgetq_lane_u32(gp, 0) & 0x01u);
    flags |= (vgetq_lane_u32(gn, 0) & 0x02u);
    flags |= (vgetq_lane_u32(gp, 1) & 0x04u);
    flags |= (vgetq_lane_u32(gn, 1) & 0x08u);
    flags |= (vgetq_lane_u32(gp, 2) & 0x10u);
    flags |= (vgetq_lane_u32(gn, 2) & 0x20u);
    return flags;
}

// R register
static inline uint32_t vr_rnext(uint32_t r)
{
    const uint32_t x = (r >> 4) & 1u;
    const uint32_t y = (r >> 22) & 1u;
    r = ((r << 1) ^ x ^ y) & 0x007FFFFFu;
    return r | 0x3F800000u;
}

// FDIV unit (copied from VU1Interpreter::execLower; operands already clamped).
static inline float vr_div(float num, float den, uint32_t &di)
{
    uint32_t statusDi = 0u;
    float result = 0.0f;
    if (den == 0.0f)
    {
        statusDi = num == 0.0f ? 0x10u : 0x20u;
        result = std::signbit(num) != std::signbit(den)
                     ? -std::numeric_limits<float>::max()
                     : std::numeric_limits<float>::max();
    }
    else
    {
        result = num / den;
    }
    di = statusDi & 0x30u;
    return vr_normf(vr_normf(result));
}

static inline float vr_sqrt(float val, uint32_t &di)
{
    di = (val < 0.0f ? 0x10u : 0u) & 0x30u;
    return vr_normf(std::sqrt(std::fabs(val)));
}

static inline float vr_rsqrt(float num, float radicand, uint32_t &di)
{
    const float den = std::sqrt(std::fabs(radicand));
    uint32_t statusDi = radicand < 0.0f ? 0x10u : 0u;
    float result = 0.0f;
    if (den != 0.0f)
        result = num / den;
    else
    {
        statusDi = num == 0.0f ? 0x10u : 0x20u;
        result = std::signbit(num) ? -std::numeric_limits<float>::max() : std::numeric_limits<float>::max();
    }
    di = statusDi & 0x30u;
    return vr_normf(vr_normf(result));
}

// EFU (copied from ps2_vu1_lower.cpp, including expression shapes so clang
// contracts them the same way).  Generated TUs are built with
// -ffp-contract=off, so the contraction the interpreter gets from clang's
// default (-ffp-contract=on) is spelled out with std::fma where the
// interpreter object code shows fmadd.
namespace vr_efu_detail
{
    static inline float eatan(float value)
    {
        constexpr float coefficients[] = {
            0.999999344348907f, -0.333298563957214f, 0.199465364217758f, -0.13085337519646f,
            0.096420042216778f, -0.055909886956215f, 0.021861229091883f, -0.004054057877511f};
        constexpr float quarterPi = 0.785398185253143f;
        const float squared = value * value;
        float polynomial = coefficients[7];
        for (int index = 6; index >= 0; --index)
            polynomial = std::fma(squared, polynomial, coefficients[index]);
        return std::fma(value, polynomial, quarterPi);
    }
    static inline float esin(float value)
    {
        constexpr float coefficients[] = {
            1.0f, -0.166666567325592f, 0.008333025500178f, -0.000198074136279f, 0.000002601886990f};
        const float squared = value * value;
        float polynomial = coefficients[4];
        for (int index = 3; index >= 0; --index)
            polynomial = std::fma(squared, polynomial, coefficients[index]);
        return value * polynomial;
    }
    static inline float eexp(float value)
    {
        constexpr float coefficients[] = {
            0.249998688697815f, 0.031257584691048f, 0.002591371303424f,
            0.000171562001924f, 0.000005430199963f, 0.000000690600018f};
        float polynomial = coefficients[5];
        for (int index = 4; index >= 0; --index)
            polynomial = std::fma(value, polynomial, coefficients[index]);
        polynomial = std::fma(value, polynomial, 1.0f);
        polynomial *= polynomial;
        polynomial *= polynomial;
        return polynomial != 0.0f ? 1.0f / polynomial : std::numeric_limits<float>::max();
    }
    static inline float sumsq(float32x4_t v)
    {
        const float x = vr_normf(vgetq_lane_f32(v, 0));
        const float y = vr_normf(vgetq_lane_f32(v, 1));
        const float z = vr_normf(vgetq_lane_f32(v, 2));
        // interpreter: x * x + y * y + z * z; clang emits fmul + fmadd + fmadd
        // (LLVM folds fadd(fmul a, fmul b) into fma(a.., fmul b)).  Not reached by
        // the RTA microcode, so this shape is not covered by the diff test.
        return std::fma(z, z, std::fma(x, x, y * y));
    }
}

static inline float vr_efu_esadd(float32x4_t v, int) { return vr_efu_detail::sumsq(v); }
static inline float vr_efu_ersadd(float32x4_t v, int)
{
    const float sum = vr_efu_detail::sumsq(v);
    return sum != 0.0f ? 1.0f / sum : sum;
}
static inline float vr_efu_eleng(float32x4_t v, int) { return std::sqrt(vr_efu_detail::sumsq(v)); }
static inline float vr_efu_erleng(float32x4_t v, int)
{
    const float len = std::sqrt(vr_efu_detail::sumsq(v));
    return len != 0.0f ? 1.0f / len : len;
}
static inline float vr_efu_eatanxy(float32x4_t v, int)
{
    const float x = vr_normf(vgetq_lane_f32(v, 0));
    const float y = vr_normf(vgetq_lane_f32(v, 1));
    return x != 0.0f ? vr_efu_detail::eatan(y / x) : 0.0f;
}
static inline float vr_efu_eatanxz(float32x4_t v, int)
{
    const float x = vr_normf(vgetq_lane_f32(v, 0));
    const float z = vr_normf(vgetq_lane_f32(v, 2));
    return x != 0.0f ? vr_efu_detail::eatan(z / x) : 0.0f;
}
static inline float vr_efu_esum(float32x4_t v, int)
{
    float sum = 0.0f;
    sum += vr_normf(vgetq_lane_f32(v, 0));
    sum += vr_normf(vgetq_lane_f32(v, 1));
    sum += vr_normf(vgetq_lane_f32(v, 2));
    sum += vr_normf(vgetq_lane_f32(v, 3));
    return sum;
}
static inline float vr_lane(float32x4_t v, int c)
{
    float tmp[4];
    vst1q_f32(tmp, v);
    return tmp[c & 3];
}
static inline float vr_efu_ersqrt(float32x4_t v, int c)
{
    const float value = vr_normf(vr_lane(v, c));
    float result = value;
    if (result >= 0.0f)
    {
        result = std::sqrt(result);
        if (result != 0.0f)
            result = 1.0f / result;
    }
    return result;
}
static inline float vr_efu_esqrt(float32x4_t v, int c)
{
    const float value = vr_normf(vr_lane(v, c));
    return value >= 0.0f ? std::sqrt(value) : value;
}
static inline float vr_efu_esin(float32x4_t v, int c) { return vr_efu_detail::esin(vr_normf(vr_lane(v, c))); }
static inline float vr_efu_ercpr(float32x4_t v, int c)
{
    const float value = vr_normf(vr_lane(v, c));
    return value != 0.0f ? 1.0f / value : value;
}
static inline float vr_efu_eatan(float32x4_t v, int c) { return vr_efu_detail::eatan(vr_normf(vr_lane(v, c))); }
static inline float vr_efu_eexp(float32x4_t v, int c) { return vr_efu_detail::eexp(vr_normf(vr_lane(v, c))); }

// ---------------------------------------------------------------------------
// FMAC flags (MAC / status current / product sticky), only emitted where a
// flag reader can observe them (or in the full-flag variant).  Mirrors
// calculateFmacExactResult + normalizeFmacExactResult + updateFmacFlags +
// calculateFmacProductSticky.  The interpreter's long double expressions are
// contracted by clang (fmadd on double); std::fma reproduces that.
// ---------------------------------------------------------------------------
namespace vr_flag_detail
{
    static inline uint8_t exactFlags(long double exactResult)
    {
        const bool negative = std::signbit(exactResult);
        const long double magnitude = std::fabs(exactResult);
        const long double maximum = static_cast<long double>(std::numeric_limits<float>::max());
        const long double minimum = static_cast<long double>(std::numeric_limits<float>::min());
        uint8_t flags = negative ? 0x2u : 0u;
        if (magnitude == 0.0L)
            flags |= 0x1u;
        else if (magnitude > maximum)
            flags |= 0x8u;
        else if (magnitude < minimum)
            flags |= 0x5u;
        return flags;
    }

    static inline long double madd(long double a, long double s, long double t) { return std::fma(s, t, a); }
    static inline long double msub(long double a, long double s, long double t) { return std::fma(-s, t, a); }

    template <uint32_t UPPER>
    static inline bool exact(uint32_t c, const float *vs, const float *vt, const float *acc, float qf, float i_f,
                             long double &result)
    {
        constexpr uint32_t op = UPPER & 0x3Fu;
        constexpr uint32_t special = op >= 0x3Cu ? ((UPPER & 3u) | ((UPPER >> 4) & 0x7Cu)) : 0xFFu;
        constexpr uint32_t sel = op >= 0x3Cu ? special : op;
        const long double S = vs[c];
        const auto T = [&](uint32_t l) { return static_cast<long double>(vt[l]); };
        const long double A = acc[c];
        const long double q = qf;
        const long double i = i_f;
        if constexpr (sel <= 0x03u)
            result = S + T(sel & 3u);
        else if constexpr (sel <= 0x07u)
            result = S - T(sel & 3u);
        else if constexpr (sel <= 0x0Bu)
            result = madd(A, S, T(sel & 3u));
        else if constexpr (sel <= 0x0Fu)
            result = msub(A, S, T(sel & 3u));
        else if constexpr (sel >= 0x18u && sel <= 0x1Bu)
            result = S * T(sel & 3u);
        else if constexpr (sel == 0x1Cu)
            result = S * q;
        else if constexpr (sel == 0x1Eu)
            result = S * i;
        else if constexpr (sel == 0x20u)
            result = S + q;
        else if constexpr (sel == 0x21u)
            result = madd(A, S, q);
        else if constexpr (sel == 0x22u)
            result = S + i;
        else if constexpr (sel == 0x23u)
            result = madd(A, S, i);
        else if constexpr (sel == 0x24u)
            result = S - q;
        else if constexpr (sel == 0x25u)
            result = msub(A, S, q);
        else if constexpr (sel == 0x26u)
            result = S - i;
        else if constexpr (sel == 0x27u)
            result = msub(A, S, i);
        else if constexpr (sel == 0x28u)
            result = S + T(c);
        else if constexpr (sel == 0x29u)
            result = madd(A, S, T(c));
        else if constexpr (sel == 0x2Au)
            result = S * T(c);
        else if constexpr (sel == 0x2Cu)
            result = S - T(c);
        else if constexpr (sel == 0x2Du)
            result = msub(A, S, T(c));
        else if constexpr (sel == 0x2Eu)
        {
            constexpr uint8_t left[4] = {1u, 2u, 0u, 3u};
            constexpr uint8_t right[4] = {2u, 0u, 1u, 3u};
            if constexpr (op == 0x2Eu)
                result = c == 3u ? 0.0L : msub(acc[c], vs[left[c]], T(right[c]));
            else
                result = c == 3u ? 0.0L : static_cast<long double>(vs[left[c]]) * T(right[c]);
        }
        else
            return false;
        return true;
    }
}

template <uint32_t UPPER>
static inline void vr_fmac_flags(float32x4_t vsv, float32x4_t vtv, float32x4_t accv, float q, float i,
                                 uint32_t &macOut, uint32_t &statusOut, uint32_t &stickyOut)
{
    using namespace vr_flag_detail;
    constexpr uint32_t dest = (UPPER >> 21) & 0xFu;
    constexpr uint32_t op = UPPER & 0x3Fu;
    constexpr uint32_t special = op >= 0x3Cu ? ((UPPER & 3u) | ((UPPER >> 4) & 0x7Cu)) : 0xFFu;
    float vs[4], vt[4], acc[4];
    vst1q_f32(vs, vsv);
    vst1q_f32(vt, vtv);
    vst1q_f32(acc, accv);
    uint32_t mac = 0u, status = 0u;
    for (uint32_t c = 0; c < 4u; ++c)
    {
        const uint32_t lane = 1u << (3u - c);
        if ((dest & lane) == 0u)
            continue;
        long double e = 0.0L;
        uint32_t flags = 0u;
        if (exact<UPPER>(c, vs, vt, acc, q, i, e))
            flags = exactFlags(e);
        if (flags & 0x1u)
            mac |= lane;
        if (flags & 0x2u)
            mac |= lane << 4;
        if (flags & 0x4u)
            mac |= lane << 8;
        if (flags & 0x8u)
            mac |= lane << 12;
        status |= flags;
    }
    // product sticky (calculateFmacProductSticky)
    uint32_t extra = 0u;
    constexpr bool productSum =
        (op >= 0x08u && op <= 0x0Fu) || op == 0x21u || op == 0x23u || op == 0x25u || op == 0x27u ||
        op == 0x29u || op == 0x2Du || op == 0x2Eu || (special >= 0x08u && special <= 0x0Fu) ||
        special == 0x21u || special == 0x23u || special == 0x25u || special == 0x27u || special == 0x29u ||
        special == 0x2Du;
    if constexpr (productSum)
    {
        constexpr uint8_t crossLeft[4] = {1u, 2u, 0u, 3u};
        constexpr uint8_t crossRight[4] = {2u, 0u, 1u, 3u};
        for (uint32_t c = 0; c < 4u; ++c)
        {
            if ((dest & (1u << (3u - c))) == 0u)
                continue;
            const uint32_t lc = op == 0x2Eu ? crossLeft[c] : c;
            const float left = vs[lc];
            float right = 0.0f;
            if ((op >= 0x08u && op <= 0x0Fu) || (special >= 0x08u && special <= 0x0Fu))
                right = vt[((op >= 0x08u && op <= 0x0Fu) ? op : special) & 3u];
            else if (op == 0x21u || op == 0x25u || special == 0x21u || special == 0x25u)
                right = q;
            else if (op == 0x23u || op == 0x27u || special == 0x23u || special == 0x27u)
                right = i;
            else if (op == 0x2Eu)
                right = vt[crossRight[c]];
            else
                right = vt[c];
            const long double exactProduct = static_cast<long double>(left) * static_cast<long double>(right);
            extra |= exactFlags(exactProduct) & 0xFu;
        }
    }
    macOut = mac;
    statusOut = status;
    stickyOut = extra;
}

#endif
