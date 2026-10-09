// GSMetalBackend: Metal plan M1 skeleton. See gs_metal_backend.h for the design summary and
// game/docs/METAL_BACKEND_PLAN.md (section 13) for the measurements.
//
// Exactness notes (the oracle is gs_cpu_backend.cpp, Raster<NoTexels, C32, Z24>):
// - Clang compiles the oracle with -ffp-contract=on; the LLVM IR (checked with -emit-llvm) is
//     W  = ((fma(A, px - fx2, B * (py - fy2))) * winding) * invAbsDenom
//     w2 = (1 - w0) - w1
//     c  = fma(c2, w2, fma(c0, w0, c1 * w1))          (colour and fog, float)
//     z  = fma(z2, w2, fma(z0, w0, z1 * w1))          (double; the products are exact)
//   and the draw runs under the FPCR of the thread that issued it: round toward zero + FZ for the
//   game thread. The GPU rounds to nearest even, so the shader emulates RTZ with error-free
//   transformations (TwoSum / fma residuals) and Z with a 64-bit-integer soft double.
// - Per-primitive constants (edge coefficients, 1/|denom|, bounding box, sprite rectangle, sprite
//   Z) are computed on the CPU with the oracle's own expressions under the same FPCR.

#import <Metal/Metal.h>
#import <Foundation/Foundation.h>

#include "runtime/gs/gs_metal_backend.h"
#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/ps2_gs_common.h"
#include "runtime/gs/ps2_gs_memory.h"

#include <algorithm>
#include <array>
#include <bitset>
#include <cfenv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <atomic>
#include <mutex>
#include <random>
#include <string>
#include <unistd.h>
#include <CommonCrypto/CommonDigest.h>

// MX1: build-time metallib (generated gs_metallib_data.cpp; size 0 when built without the Metal toolchain)
extern "C" const unsigned char g_gsmtlMetallib[];
extern "C" const size_t g_gsmtlMetallibSize;
extern "C" const char g_gsmtlMetallibSrcSha256[];
#include <unordered_map>
#include <vector>

namespace
{
    // Shared with the shader (all 4-byte fields).
    struct GPUPrim
    {
        float fx2, fy2;
        float a0, b0, a1, b1;
        float winding, invAbsDenom;
        float z0, z1, z2;
        uint32_t rgba0, rgba1, rgba2;
        uint32_t fog;    // fog0 | fog1 << 8 | fog2 << 16
        uint32_t flags;  // 1 sprite, 2 iip, 4 rtz, 8 fge, 16 abe, 32 pabe, 64 fba, 128 zmask, 256 fault injection
        uint32_t test;   // TEST bits 0..31
        uint32_t alpha;  // A | B << 2 | C << 4 | D << 6 | FIX << 8
        uint32_t fbmsk;
        uint32_t fogcol; // r | g << 8 | b << 16
        uint32_t spriteZ;
        // textured triangles (G2a)
        float s0, t0, q0, s1, t1, q1, s2, t2, q2;
        uint32_t uv0, uv1, uv2; // u | v << 16 (raw 12.4)
        uint32_t tflags;        // 1 textured, 2 fst, 4 linear, 8 indexed, tfx << 4, tcc << 6, wms << 8, wmt << 10, 4096 raw target texture, 8192 CT24 (TEXA)
        uint32_t texdim;        // wrap size: w | h << 16
        uint32_t regU;          // minU | maxU << 16
        uint32_t regV;
        uint32_t palOff;        // word offset into the palette buffer (indexed textures)
        // G2b: sprites reuse s0,t0,s1,t1 = u0f,v0f,u1f,v1f; q0,q1 = sprite W,H; uv0,uv1 = unclipped x0,y0 (int bits)
        uint32_t texa;          // ta0 | aem << 8 (CT24 from a framebuffer target)
        uint32_t fbw;
    };
    static_assert(sizeof(GPUPrim) == 84 + 19 * 4, "GPUPrim layout");

    struct GPUVertex
    {
        float x, y;
        uint32_t prim;
    };

    const char *kShaderSource = R"METAL(
#include <metal_stdlib>
using namespace metal;

struct Prim {
    float fx2, fy2;
    float a0, b0, a1, b1;
    float winding, invAbsDenom;
    float z0, z1, z2;
    uint rgba0, rgba1, rgba2;
    uint fog;
    uint flags;
    uint test;
    uint alpha;
    uint fbmsk;
    uint fogcol;
    uint spriteZ;
    float s0, t0, q0, s1, t1, q1, s2, t2, q2;
    uint uv0, uv1, uv2;
    uint tflags;
    uint texdim;
    uint regU;
    uint regV;
    uint palOff;
    uint texa;
    uint fbw;
};

struct Vtx { float x; float y; uint prim; };

struct VOut {
    float4 pos [[position]];
    uint prim [[flat]];
};

// ---------------------------------------------------------------- RTZ float emulation
// r is the round-to-nearest result, err has the sign of (exact - r).
inline float tz_fix(float r, float err)
{
    if (err == 0.0f || r == 0.0f || isinf(r))
        return r;
    if ((err < 0.0f) != (r < 0.0f))
        return as_type<float>(as_type<uint>(r) - 1u);
    return r;
}

inline float two_sum_err(float a, float b, float s)
{
    float bb = s - a;
    return (a - (s - bb)) + (b - bb);
}

// GPU arithmetic flushes denormals, so residuals of operations on small values (a product's residual
// can be 2^-47 of the product) are computed on operands scaled by 2^64 (exact, no overflow below 2^60).
constant float kTiny = 0x1p-60f;
constant float kBig = 0x1p60f;
constant float kScale = 0x1p64f;

inline float f_add(float a, float b, bool rtz)
{
    float s = a + b;
    if (!rtz)
        return s;
    const float lo = min(fabs(a), fabs(b));
    if (lo != 0.0f && lo < kTiny && max(fabs(a), fabs(b)) < kBig) {
        const float as = a * kScale, bs = b * kScale;
        return tz_fix(s, two_sum_err(as, bs, as + bs));
    }
    return tz_fix(s, two_sum_err(a, b, s));
}

inline float f_sub(float a, float b, bool rtz) { return f_add(a, -b, rtz); }

inline float f_mul(float a, float b, bool rtz)
{
    float p = a * b;
    if (!rtz)
        return p;
    if (fabs(p) < kTiny) {
        const float as = (fabs(a) < fabs(b)) ? a * kScale : a;
        const float bs = (fabs(a) < fabs(b)) ? b : b * kScale;
        const float ps = as * bs;
        return tz_fix(p, fma(as, bs, -ps));
    }
    return tz_fix(p, fma(a, b, -p));
}

// sign of (a*b + c - r), r = fma(a, b, c) rounded to nearest
inline float fma_residual(float a, float b, float c, float r)
{
    float p = a * b;
    float pe = fma(a, b, -p);       // a*b = p + pe exactly
    float s = p + c;
    float se = two_sum_err(p, c, s); // p + c = s + se exactly
    float d = s - r;                 // exact (s and r are close)
    float u = d + se;
    float ue = two_sum_err(d, se, u);
    float v = u + pe;
    return (v != 0.0f) ? v : ue;
}

inline float f_fma(float a, float b, float c, bool rtz)
{
    float r = fma(a, b, c);
    if (!rtz)
        return r;
    const float ap = fabs(a * b), ac = fabs(c);
    const float lo = (ap == 0.0f) ? ac : ((ac == 0.0f) ? ap : min(ap, ac));
    if (lo != 0.0f && lo < kTiny && max(max(ap, ac), fabs(r)) < kBig) {
        const bool sa = fabs(a) < fabs(b);
        return tz_fix(r, fma_residual(sa ? a * kScale : a, sa ? b : b * kScale, c * kScale, r * kScale));
    }
    return tz_fix(r, fma_residual(a, b, c, r));
}

// ---------------------------------------------------------------- soft double (value = +-m * 2^e)
struct SD { ulong m; int e; bool neg; };

inline SD sd_prod(float z, float w)
{
    uint zb = as_type<uint>(z), wb = as_type<uint>(w);
    uint ze = (zb >> 23) & 0xFFu, we = (wb >> 23) & 0xFFu;
    SD r;
    r.neg = ((zb ^ wb) >> 31) != 0u;
    if (ze == 0u || we == 0u) { r.m = 0ul; r.e = 0; return r; } // zero / flushed denormal
    ulong zm = ulong((zb & 0x7FFFFFu) | 0x800000u);
    ulong wm = ulong((wb & 0x7FFFFFu) | 0x800000u);
    r.m = zm * wm;
    r.e = int(ze) - 150 + int(we) - 150;
    return r;
}

inline SD sd_norm62(SD a)
{
    int sh = int(clz(a.m)) - 1;
    a.m <<= ulong(sh);
    a.e -= sh;
    return a;
}

// a + b rounded to a 53-bit significand (nearest-even, or toward zero).
inline SD sd_add(SD a, SD b, bool rtz)
{
    if (a.m == 0ul) return b;
    if (b.m == 0ul) return a;
    a = sd_norm62(a);
    b = sd_norm62(b);
    if (a.e < b.e || (a.e == b.e && a.m < b.m)) { SD t = a; a = b; b = t; }
    int d = a.e - b.e;
    ulong bm;
    bool sticky;
    if (d == 0) { bm = b.m; sticky = false; }
    else if (d < 64) { bm = b.m >> ulong(d); sticky = (b.m << ulong(64 - d)) != 0ul; }
    else { bm = 0ul; sticky = true; }
    ulong R;
    if (a.neg == b.neg) R = a.m + bm;
    else R = a.m - bm - (sticky ? 1ul : 0ul);
    SD r;
    r.neg = a.neg;
    r.e = a.e;
    if (R == 0ul && !sticky) { r.m = 0ul; r.e = 0; r.neg = false; return r; }
    int L = 64 - int(clz(R));
    if (L > 53) {
        int sh = L - 53;
        ulong rem = R & ((1ul << ulong(sh)) - 1ul);
        ulong halfv = 1ul << ulong(sh - 1);
        R >>= ulong(sh);
        r.e += sh;
        if (!rtz) {
            bool up = rem > halfv || (rem == halfv && (sticky || (R & 1ul) != 0ul));
            if (up) {
                R += 1ul;
                if (R == (1ul << 53)) { R >>= 1ul; r.e += 1; }
            }
        }
    }
    r.m = R;
    return r;
}

inline uint sd_to_u32(SD x)
{
    if (x.m == 0ul || x.neg) return 0u;
    if (x.e >= 0) {
        int L = 64 - int(clz(x.m));
        if (L + x.e > 32) return 0xFFFFFFFFu;
        return uint(x.m << ulong(x.e));
    }
    int s = -x.e;
    if (s >= 64) return 0u;
    ulong v = x.m >> ulong(s);
    return v > 0xFFFFFFFFul ? 0xFFFFFFFFu : uint(v);
}

inline uint tri_z(float z0, float z1, float z2, float w0, float w1, float w2, bool rtz)
{
    SD p0 = sd_prod(z0, w0), p1 = sd_prod(z1, w1), p2 = sd_prod(z2, w2);
    SD s = sd_add(p0, p1, rtz);
    s = sd_add(p2, s, rtz);
    SD h; h.m = 1ul; h.e = -1; h.neg = false;
    s = sd_add(s, h, rtz);
    return sd_to_u32(s);
}

inline uint interp_u8(uint c0, uint c1, uint c2, float w0, float w1, float w2, bool rtz)
{
    float v = f_fma(float(c2), w2, f_fma(float(c0), w0, f_mul(float(c1), w1, rtz), rtz), rtz);
    int i = int(v);
    return uint(clamp(i, 0, 255));
}

// ---------------------------------------------------------------- texture path (G2a)
// fcvtzs: float -> int32, truncate toward zero, saturating, NaN -> 0 (what the oracle's static_cast<int> compiles to).
inline int cvt_s32(float x)
{
    if (isnan(x)) return 0;
    if (x >= 2147483648.0f) return 0x7FFFFFFF;
    if (x <= -2147483648.0f) return int(0x80000000u);
    return int(x);
}

// sitofp under the draw's rounding mode (only differs from the nearest conversion above 2^24).
inline float i2f(int c, bool rtz)
{
    float f = float(c);
    if (rtz && (c > 16777216 || c < -16777216)) {
        long back = (fabs(f) >= 2147483648.0f) ? ((f < 0.0f) ? -2147483648L : 2147483648L) : long(f);
        long ex = long(c);
        if ((back < 0 ? -back : back) > (ex < 0 ? -ex : ex))
            f = as_type<float>(as_type<uint>(f) - 1u);
    }
    return f;
}

// 1.0f / b, correctly rounded in the draw's rounding mode (RTZ or RNE), flush-to-zero results.
// Metal's division is only accurate to a few ULP, so the quotient is corrected with exact fma residuals.
inline float f_rcp(float b, bool rtz)
{
    const float ab = fabs(b);
    float q;
    if (ab > 0x1p126f) {
        q = 0.0f;
    } else {
        q = 1.0f / ab;
        for (int i = 0; i < 4; ++i) {
            if (fma(-q, ab, 1.0f) < 0.0f) q = as_type<float>(as_type<uint>(q) - 1u); else break;
        }
        for (int i = 0; i < 4; ++i) {
            const float qn = as_type<float>(as_type<uint>(q) + 1u);
            if (fma(-qn, ab, 1.0f) >= 0.0f) q = qn; else break;
        }
        if (!rtz) {
            const float qn = as_type<float>(as_type<uint>(q) + 1u);
            const float r2 = 2.0f * fma(-q, ab, 1.0f);
            const float h = ab * (qn - q);
            if (r2 > h || (r2 == h && (as_type<uint>(q) & 1u) != 0u)) q = qn;
        }
    }
    return (b < 0.0f) ? -q : q;
}

// a / b for finite a > 0, b > 0, correctly rounded in the draw's rounding mode (the sprite axis division).
inline float f_div(float a, float b, bool rtz)
{
    float q = a / b;
    for (int i = 0; i < 4; ++i) {
        if (fma(-q, b, a) < 0.0f) q = as_type<float>(as_type<uint>(q) - 1u); else break;
    }
    for (int i = 0; i < 4; ++i) {
        const float qn = as_type<float>(as_type<uint>(q) + 1u);
        if (fma(-qn, b, a) >= 0.0f) q = qn; else break;
    }
    if (!rtz) {
        const float qn = as_type<float>(as_type<uint>(q) + 1u);
        const float r2 = 2.0f * fma(-q, b, a);
        const float h = b * (qn - q);
        if (r2 > h || (r2 == h && (as_type<uint>(q) & 1u) != 0u)) q = qn;
    }
    return q;
}

inline float interp3f(float a0, float a1, float a2, float w0, float w1, float w2, bool rtz)
{
    return f_fma(a2, w2, f_fma(a0, w0, f_mul(a1, w1, rtz), rtz), rtz);
}

// Sampler::Coord for the non-FST path: st * (1/fabsQ(q)) * size
inline float tex_coord_stq(float st, float q, uint size, bool rtz)
{
    const float aq = (fabs(q) > 1.0e-8f) ? q : 1.0f;
    return f_mul(f_mul(st, f_rcp(aq, rtz), rtz), float(size), rtz);
}

inline int wrap_coord(int c, uint size, uint mode, uint rmin, uint rmax)
{
    switch (mode & 3u) {
    case 0u: return int(uint(c) & (size - 1u));
    case 1u: return (c < 0) ? 0 : ((c > int(size) - 1) ? int(size) - 1 : c);
    case 2u: return min(max(c, int(rmin)), int(rmax));
    default: return int((uint(c) & rmin) | rmax);
    }
}

struct Axis { uint i0; uint i1; float frac; };

inline Axis prep_axis(float coord, uint size, uint mode, uint rmin, uint rmax, bool linear, bool rtz)
{
    Axis a;
    if (!linear) {
        a.i0 = uint(wrap_coord(cvt_s32(coord), size, mode, rmin, rmax));
        a.i1 = 0u;
        a.frac = 0.0f;
        return a;
    }
    const float sample = f_sub(coord, 0.5f, rtz);
    const int c0 = cvt_s32(floor(sample));
    a.frac = f_sub(sample, i2f(c0, rtz), rtz);
    a.i0 = uint(wrap_coord(c0, size, mode, rmin, rmax));
    a.i1 = uint(wrap_coord(int(uint(c0) + 1u), size, mode, rmin, rmax));
    return a;
}

inline uint fetch_texel(texture2d<uint, access::read> tex, const device uint *pal, uint palOff, uint tflags, uint texa, uint fbw, uint u, uint v)
{
    if ((tflags & 4096u) != 0u) {
        // a framebuffer target (raw 32-bit words): texel (u,v) is pixel (u,v); anything outside the target has
        // a bilinear weight the host proved negligible (see TryFbTexture)
        uint word = 0u;
        if (u < tex.get_width() && v < tex.get_height())
            word = tex.read(uint2(u, v)).x;
        if ((tflags & 8192u) != 0u) {
            const uint rgb = word & 0xFFFFFFu;
            const uint a = (((texa >> 8) & 1u) != 0u && rgb == 0u) ? 0u : (texa & 0xFFu);
            return rgb | (a << 24);
        }
        return word;
    }
    const uint4 t = tex.read(uint2(u, v));
    return ((tflags & 8u) != 0u) ? pal[palOff + (t.x & 0xFFu)] : (t.x | (t.y << 8) | (t.z << 16) | (t.w << 24));
}

// GSInternal::bilinearRgba8 (the NEON path: three fused multiply-adds per channel, round half away, saturate)
inline uint bilinear8(uint c00, uint c10, uint c01, uint c11, float fx, float fy, bool rtz)
{
    uint out = 0u;
    for (uint sh = 0u; sh < 32u; sh += 8u) {
        const float f00 = float((c00 >> sh) & 255u), f10 = float((c10 >> sh) & 255u);
        const float f01 = float((c01 >> sh) & 255u), f11 = float((c11 >> sh) & 255u);
        const float top = f_fma(f10 - f00, fx, f00, rtz);
        const float bottom = f_fma(f11 - f01, fx, f01, rtz);
        const float v = f_fma(f_sub(bottom, top, rtz), fy, top, rtz);
        const int i = cvt_s32(round(v));
        out |= uint(clamp(i, 0, 255)) << sh;
    }
    return out;
}

// ---------------------------------------------------------------- write-back scatter (G2d)
// Copies a rectangle of a plane (raw 32-bit VRAM words) straight into the swizzled CPU shadow:
// exactly GSMem::SwizzledSurface<C32|Z24>::Locate for a page-aligned base (page, page table, 4 MiB wrap).
struct WbParams { uint x0, y0, w, h, basePage, pagesPerRow; };
kernel void wb_scatter(texture2d<uint, access::read> src [[texture(0)]], device uint *vram [[buffer(0)]],
                       const device ushort *tbl [[buffer(1)]], constant WbParams &p [[buffer(2)]],
                       uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= p.w || gid.y >= p.h)
        return;
    const uint x = p.x0 + gid.x, y = p.y0 + gid.y;
    const uint page = p.basePage + (y >> 5) * p.pagesPerRow + (x >> 6);
    const uint pixel = page * 2048u + uint(tbl[(y & 31u) * 64u + (x & 63u)]);
    vram[pixel & 0xFFFFFu] = src.read(uint2(x, y)).r;
}

// ---------------------------------------------------------------- self-test kernels
kernel void selftest_f(device const float4 *in [[buffer(0)]], device uint4 *out [[buffer(1)]],
                       constant uint &rtz [[buffer(2)]], uint id [[thread_position_in_grid]])
{
    float4 v = in[id];
    bool r = rtz != 0u;
    out[id] = uint4(as_type<uint>(f_add(v.x, v.y, r)), as_type<uint>(f_mul(v.x, v.y, r)),
                    as_type<uint>(f_fma(v.x, v.y, v.z, r)), interp_u8(uint(v.w) & 255u, (uint(v.w) >> 8) & 255u, 7u, v.x, v.y, v.z, r));
}

kernel void selftest_z(device const float4 *zin [[buffer(0)]], device const float4 *win [[buffer(1)]],
                       device uint *out [[buffer(2)]], constant uint &rtz [[buffer(3)]], uint id [[thread_position_in_grid]])
{
    float4 z = zin[id];
    float4 w = win[id];
    out[id] = tri_z(z.x, z.y, z.z, w.x, w.y, w.z, rtz != 0u);
}

kernel void selftest_t(device const float4 *in [[buffer(0)]], device const uint4 *corners [[buffer(1)]],
                       device uint4 *out [[buffer(2)]], constant uint &rtz [[buffer(3)]], uint id [[thread_position_in_grid]])
{
    const float4 v = in[id];
    const uint4 c = corners[id];
    const bool r = rtz != 0u;
    out[id] = uint4(as_type<uint>(f_rcp(v.x, r)), as_type<uint>(tex_coord_stq(v.y, v.x, 256u, r)),
                    bilinear8(c.x, c.y, c.z, c.w, v.z, v.w, r), as_type<uint>(i2f(int(as_type<uint>(v.y)), r)));
}

// ---------------------------------------------------------------- draw
vertex VOut vs_main(uint vid [[vertex_id]], const device Vtx *v [[buffer(0)]], constant float2 &size [[buffer(1)]])
{
    VOut o;
    float2 p = float2(v[vid].x, v[vid].y);
    o.pos = float4(p.x / size.x * 2.0f - 1.0f, 1.0f - p.y / size.y * 2.0f, 0.0f, 1.0f);
    o.prim = v[vid].prim;
    return o;
}

struct FOut {
    uint frame [[color(0)]];
    uint depth [[color(1)]];
};

inline int pick_blend(uint sel, int cs, int cd) { return sel == 0u ? cs : (sel == 1u ? cd : 0); }

// G2e: HI = the display-only scaled pass (PS2X_GS_SCALE). HI=false is the exact 1x pass, unchanged.
struct HiP { float sx, sy; uint isx, isy, dbg; uint wide; float spad; };

// G3a: xf.xy = (kx, ox) maps the native x of this prim to the padded widescreen plane (x' = kx*x + ox); xf.zw maps a
// padded framebuffer source's native coordinate the same way (dest padded) or by its pad (dest not padded).
template <bool HI>
inline FOut fs_impl(VOut in, const device Prim *prims, const device uint *pal, texture2d<uint, access::read> tex, uint fb, uint zb, HiP hp,
                    float4 xf)
{
    const Prim P = prims[in.prim];
    const float hnx = HI ? (in.pos.x / hp.sx - xf.y) / xf.x : in.pos.x; // native x of this sample (HI only)
    const bool rtz = HI ? false : ((P.flags & 4u) != 0u);
    uint r, g, b, a, fog, z;
    float w0 = 0.0f, w1 = 0.0f, w2 = 0.0f;
    if ((P.flags & 1u) != 0u) {
        r = P.rgba0 & 0xFFu; g = (P.rgba0 >> 8) & 0xFFu; b = (P.rgba0 >> 16) & 0xFFu; a = P.rgba0 >> 24;
        fog = P.fog & 0xFFu;
        z = P.spriteZ;
    } else {
        // HI: sub-sample centres, nudged off the 1/16 vertex grid so that samples rarely lie exactly on a shared edge
        // (the 1x edge tolerance, kept so rounding never opens a seam, would cover them twice)
        float px = HI ? hnx + 0.0009765625f : in.pos.x, py = HI ? in.pos.y / hp.sy + 0.00048828125f : in.pos.y; // pixel centre = integer + 0.5
        if (HI && (hp.dbg & 2u) != 0u) { px = floor(px) + 0.5f; py = floor(py) + 0.5f; } // debug: triangles at native centres
        const float dx = f_sub(px, P.fx2, rtz), dy = f_sub(py, P.fy2, rtz);
        w0 = f_mul(f_mul(f_fma(P.a0, dx, f_mul(P.b0, dy, rtz), rtz), P.winding, rtz), P.invAbsDenom, rtz);
        w1 = f_mul(f_mul(f_fma(P.a1, dx, f_mul(P.b1, dy, rtz), rtz), P.winding, rtz), P.invAbsDenom, rtz);
        w2 = f_sub(f_sub(1.0f, w0, rtz), w1, rtz);
        if (w0 < -1.0e-4f || w1 < -1.0e-4f || w2 < -1.0e-4f)
            discard_fragment();
        if (HI) {
            // display-only Z: exact for constant-Z triangles at any sub-sample (the 1x soft-double sum of
            // z*w terms rounds to Z-1 at some off-centre samples: coplanar GEQUAL overlays then lose dots)
            const float d = fma(w0, P.z0 - P.z2, w1 * (P.z1 - P.z2));
            z = uint(clamp(int(P.z2) + int(rint(d)), 0, 0xFFFFFF));
        } else
            z = tri_z(P.z0, P.z1, P.z2, w0, w1, w2, rtz);
        if ((P.flags & 2u) != 0u) {
            r = interp_u8(P.rgba0 & 0xFFu, P.rgba1 & 0xFFu, P.rgba2 & 0xFFu, w0, w1, w2, rtz);
            g = interp_u8((P.rgba0 >> 8) & 0xFFu, (P.rgba1 >> 8) & 0xFFu, (P.rgba2 >> 8) & 0xFFu, w0, w1, w2, rtz);
            b = interp_u8((P.rgba0 >> 16) & 0xFFu, (P.rgba1 >> 16) & 0xFFu, (P.rgba2 >> 16) & 0xFFu, w0, w1, w2, rtz);
            a = interp_u8(P.rgba0 >> 24, P.rgba1 >> 24, P.rgba2 >> 24, w0, w1, w2, rtz);
        } else {
            r = P.rgba2 & 0xFFu; g = (P.rgba2 >> 8) & 0xFFu; b = (P.rgba2 >> 16) & 0xFFu; a = P.rgba2 >> 24;
        }
        fog = interp_u8(P.fog & 0xFFu, (P.fog >> 8) & 0xFFu, (P.fog >> 16) & 0xFFu, w0, w1, w2, rtz);

    }

    if ((P.tflags & 1u) != 0u) {
        {
            const bool fst = (P.tflags & 2u) != 0u, linear = (P.tflags & 4u) != 0u, indexed = (P.tflags & 8u) != 0u;
            const uint tfx = (P.tflags >> 4) & 3u, tcc = (P.tflags >> 6) & 1u;
            const uint wms = (P.tflags >> 8) & 3u, wmt = (P.tflags >> 10) & 3u;
            const uint texW = P.texdim & 0xFFFFu, texH = P.texdim >> 16;
            float cu, cv;
            if ((P.flags & 1u) != 0u) {
                // Raster::Sprite: per-axis position t = (x - x0 + 0.5) / W, coord = u0 + (u1 - u0) * t
                float tx, ty;
                if (!HI) {
                    const int sx = int(in.pos.x), sy = int(in.pos.y);
                    tx = f_div(float(sx - as_type<int>(P.uv0)) + 0.5f, P.q0, rtz);
                    ty = f_div(float(sy - as_type<int>(P.uv1)) + 0.5f, P.q1, rtz);
                } else {
                    // sprite policy: a framebuffer source samples the scaled target continuously; a decoded texture is
                    // sampled native-texel-exact (as 1x) unless the axis is magnified >= 1.25x, then sub-pixel but
                    // clamped to the 1x first/last pixel so no texel outside what 1x reads can bleed in
                    float nx = hnx, ny = in.pos.y / hp.sy;
                    if ((P.tflags & 4096u) == 0u) {
                        const uint gy = as_type<uint>(P.q2);
                        const float gx0 = float(P.uv2 & 0xFFFFu) + 0.5f, gx1 = float(P.uv2 >> 16) + 0.5f;
                        const float gy0 = float(gy & 0xFFFFu) + 0.5f, gy1 = float(gy >> 16) + 0.5f;
                        const bool mag = (hp.dbg & 1u) == 0u; // debug bit 1: always native-texel-exact
                        nx = (mag && abs(P.s1 - P.s0) * 1.25f <= P.q0) ? clamp(nx, gx0, gx1) : floor(nx) + 0.5f;
                        ny = (mag && abs(P.t1 - P.t0) * 1.25f <= P.q1) ? clamp(ny, gy0, gy1) : floor(ny) + 0.5f;
                    }
                    tx = (nx - float(as_type<int>(P.uv0))) / P.q0;
                    ty = (ny - float(as_type<int>(P.uv1))) / P.q1;
                }
                const float tu = f_fma(f_sub(P.s1, P.s0, rtz), tx, P.s0, rtz);
                const float tv = f_fma(f_sub(P.t1, P.t0, rtz), ty, P.t0, rtz);
                if (HI && (P.tflags & 4096u) != 0u) {
                    cu = tu;
                    cv = tv;
                } else if (fst) {
                    cu = float(clamp(cvt_s32(f_fma(tu, 16.0f, 0.5f, rtz)), 0, 65535)) * 0.0625f;
                    cv = float(clamp(cvt_s32(f_fma(tv, 16.0f, 0.5f, rtz)), 0, 65535)) * 0.0625f;
                } else {
                    // texUf / texW * (1 / fabsQ(1)) * texW with power-of-two texW
                    const float rw = as_type<float>(0x3F800000u - (ctz(texW) << 23));
                    const float rh = as_type<float>(0x3F800000u - (ctz(texH) << 23));
                    cu = f_mul(f_mul(tu, rw, rtz), float(texW), rtz);
                    cv = f_mul(f_mul(tv, rh, rtz), float(texH), rtz);
                }
            } else if (fst) {
                const float fu = interp3f(float(P.uv0 & 0xFFFFu), float(P.uv1 & 0xFFFFu), float(P.uv2 & 0xFFFFu), w0, w1, w2, rtz);
                const float fv = interp3f(float(P.uv0 >> 16), float(P.uv1 >> 16), float(P.uv2 >> 16), w0, w1, w2, rtz);
                cu = float(uint(cvt_s32(fu)) & 0xFFFFu) * 0.0625f;
                cv = float(uint(cvt_s32(fv)) & 0xFFFFu) * 0.0625f;
            } else {
                const float is = interp3f(P.s0, P.s1, P.s2, w0, w1, w2, rtz);
                const float it = interp3f(P.t0, P.t1, P.t2, w0, w1, w2, rtz);
                const float iq = interp3f(P.q0, P.q1, P.q2, w0, w1, w2, rtz);
                cu = tex_coord_stq(is, iq, texW, rtz);
                cv = tex_coord_stq(it, iq, texH, rtz);
            }
            uint sW = texW, sH = texH, mU = wms, mV = wmt;
            uint rU0 = P.regU & 0xFFFFu, rU1 = P.regU >> 16, rV0 = P.regV & 0xFFFFu, rV1 = P.regV >> 16;
            if (HI && (P.tflags & 4096u) != 0u) {
                // the scaled framebuffer target: hi texel = native coordinate * s (the 1x proof rules out wrapping)
                if ((P.tflags & 16384u) != 0u) {
                    // G3a: a padded (widescreen) source plane: sample where this prim's own mapping puts the
                    // coordinate, so screen copies (feedback, fades) stay identity copies across the whole width
                    cu = cu * xf.z + xf.w;
                    rU0 = uint(max(float(rU0) * xf.z + xf.w, 0.0f));
                    rU1 = uint(max(float(rU1 + 1u) * xf.z + xf.w - 1.0f, 0.0f));
                }
                cu *= hp.sx;
                cv *= hp.sy;
                sW = (texW + (((P.tflags & 16384u) != 0u) ? 2u * uint(hp.spad) : 0u)) * hp.isx;
                sH = texH * hp.isy;
                mU = (wms == 2u) ? 2u : 1u;
                mV = (wmt == 2u) ? 2u : 1u;
                rU0 *= hp.isx; rU1 = rU1 * hp.isx + hp.isx - 1u;
                rV0 *= hp.isy; rV1 = rV1 * hp.isy + hp.isy - 1u;
            }
            const Axis au = prep_axis(cu, sW, mU, rU0, rU1, linear, rtz);
            const Axis av = prep_axis(cv, sH, mV, rV0, rV1, linear, rtz);
            uint texel;
            if (!linear) {
                texel = fetch_texel(tex, pal, P.palOff, P.tflags, P.texa, P.fbw, au.i0, av.i0);
            } else {
                const uint c00 = fetch_texel(tex, pal, P.palOff, P.tflags, P.texa, P.fbw, au.i0, av.i0);
                const uint c10 = fetch_texel(tex, pal, P.palOff, P.tflags, P.texa, P.fbw, au.i1, av.i0);
                const uint c01 = fetch_texel(tex, pal, P.palOff, P.tflags, P.texa, P.fbw, au.i0, av.i1);
                const uint c11 = fetch_texel(tex, pal, P.palOff, P.tflags, P.texa, P.fbw, au.i1, av.i1);
                texel = bilinear8(c00, c10, c01, c11, au.frac, av.frac, rtz);
            }
            const uint tr = texel & 0xFFu, tg = (texel >> 8) & 0xFFu, tb = (texel >> 16) & 0xFFu, ta = texel >> 24;
            const uint vr = r, vg = g, vb = b, va = a;
            uint nr = tr, ng = tg, nb = tb, na = (tcc != 0u) ? ta : va;
            switch (tfx) {
            case 0u: // MODULATE
                nr = min((tr * vr) >> 7, 255u); ng = min((tg * vg) >> 7, 255u); nb = min((tb * vb) >> 7, 255u);
                na = (tcc != 0u) ? min((ta * va) >> 7, 255u) : va;
                break;
            case 1u: // DECAL
                break;
            case 2u: // HIGHLIGHT
                nr = min(((tr * vr) >> 7) + va, 255u); ng = min(((tg * vg) >> 7) + va, 255u); nb = min(((tb * vb) >> 7) + va, 255u);
                na = (tcc != 0u) ? min(ta + va, 255u) : va;
                break;
            default: // HIGHLIGHT2
                nr = min(((tr * vr) >> 7) + va, 255u); ng = min(((tg * vg) >> 7) + va, 255u); nb = min(((tb * vb) >> 7) + va, 255u);
                na = (tcc != 0u) ? ta : va;
                break;
            }
            r = nr; g = ng; b = nb; a = na;
        }
    }

    // fog
    if ((P.flags & 8u) != 0u) {
        const uint inv = 255u - fog;
        r = ((fog * r) >> 8) + ((inv * (P.fogcol & 0xFFu)) >> 8);
        g = ((fog * g) >> 8) + ((inv * ((P.fogcol >> 8) & 0xFFu)) >> 8);
        b = ((fog * b) >> 8) + ((inv * ((P.fogcol >> 16) & 0xFFu)) >> 8);
        r &= 0xFFu; g &= 0xFFu; b &= 0xFFu;
    }

    // alpha test (CT32 frame)
    bool wRgb = true, wA = true, wZ = true;
    const uint test = P.test;
    if ((test & 1u) != 0u) {
        const uint atst = (test >> 1) & 7u, aref = (test >> 4) & 0xFFu;
        bool pass;
        switch (atst) {
        case 0u: pass = false; break;
        case 1u: pass = true; break;
        case 2u: pass = a < aref; break;
        case 3u: pass = a <= aref; break;
        case 4u: pass = a == aref; break;
        case 5u: pass = a >= aref; break;
        case 6u: pass = a > aref; break;
        default: pass = a != aref; break;
        }
        if (!pass) {
            switch ((test >> 12) & 3u) {
            case 1u: wRgb = true; wA = true; wZ = false; break;  // FB_ONLY
            case 2u: wRgb = false; wA = false; wZ = true; break; // ZB_ONLY
            case 3u: wRgb = true; wA = false; wZ = false; break; // RGB_ONLY (CT32)
            default: wRgb = false; wA = false; wZ = false; break; // KEEP
            }
        }
    }
    if (!wRgb && !wA && !wZ)
        discard_fragment();

    // destination alpha test
    if (((test >> 14) & 1u) != 0u) {
        const uint datm = (test >> 15) & 1u;
        if (((fb >> 31) & 1u) != datm)
            discard_fragment();
    }

    // depth test on the raw Z24 value
    const uint zcur = zb & 0xFFFFFFu;
    const uint ztst = (test >> 17) & 3u;
    bool zpass = (ztst == 1u) || (ztst == 2u && z >= zcur) || (ztst == 3u && z > zcur);
    if (!zpass)
        discard_fragment();

    FOut o;
    o.frame = fb;
    o.depth = zb;
    const bool writesFb = wRgb || wA;
    if (writesFb) {
        const bool preserveDestAlpha = wRgb && !wA;
        if ((P.flags & 16u) != 0u && !((P.flags & 32u) != 0u && (a & 0x80u) == 0u)) {
            const int dr = int(fb & 0xFFu), dg = int((fb >> 8) & 0xFFu), db = int((fb >> 16) & 0xFFu), da = int(fb >> 24);
            const uint asel = P.alpha & 3u, bsel = (P.alpha >> 2) & 3u, csel = (P.alpha >> 4) & 3u, dsel = (P.alpha >> 6) & 3u;
            const int fix = int((P.alpha >> 8) & 0xFFu);
            const int ca = csel == 0u ? int(a) : (csel == 1u ? da : fix);
            r = uint(clamp(((pick_blend(asel, int(r), dr) - pick_blend(bsel, int(r), dr)) * ca >> 7) + pick_blend(dsel, int(r), dr), 0, 255));
            g = uint(clamp(((pick_blend(asel, int(g), dg) - pick_blend(bsel, int(g), dg)) * ca >> 7) + pick_blend(dsel, int(g), dg), 0, 255));
            b = uint(clamp(((pick_blend(asel, int(b), db) - pick_blend(bsel, int(b), db)) * ca >> 7) + pick_blend(dsel, int(b), db), 0, 255));
        }
        if (wA && (P.flags & 64u) != 0u)
            a |= 0x80u;
        uint pixel = r | (g << 8) | (b << 16) | ((a & 0xFFu) << 24);
        if (P.fbmsk != 0u)
            pixel = (pixel & ~P.fbmsk) | (fb & P.fbmsk);
        if (preserveDestAlpha)
            pixel = (pixel & 0x00FFFFFFu) | (fb & 0xFF000000u);
        o.frame = pixel;
        if ((P.flags & 256u) != 0u)
            o.frame ^= 1u; // PS2X_GS_METAL_FAULT: deliberate error to prove the tee comparison sees it
    }
    if (wZ && (P.flags & 128u) == 0u)
        o.depth = (zb & 0xFF000000u) | (z & 0x00FFFFFFu);
    return o;
}

fragment FOut fs_main(VOut in [[stage_in]], const device Prim *prims [[buffer(0)]],
                      const device uint *pal [[buffer(1)]], texture2d<uint, access::read> tex [[texture(0)]],
                      uint fb [[color(0)]], uint zb [[color(1)]])
{
    HiP one;
    one.sx = 1.0f; one.sy = 1.0f; one.isx = 1u; one.isy = 1u; one.dbg = 0u; one.wide = 0u; one.spad = 0.0f;
    return fs_impl<false>(in, prims, pal, tex, fb, zb, one, float4(1.0f, 0.0f, 1.0f, 0.0f));
}

fragment FOut fs_hi(VOut in [[stage_in]], const device Prim *prims [[buffer(0)]],
                    const device uint *pal [[buffer(1)]], constant HiP &hp [[buffer(2)]], const device float4 *xf [[buffer(3)]],
                    texture2d<uint, access::read> tex [[texture(0)]], uint fb [[color(0)]], uint zb [[color(1)]])
{
    return fs_impl<true>(in, prims, pal, tex, fb, zb, hp, hp.wide != 0u ? xf[in.prim] : float4(1.0f, 0.0f, 1.0f, 0.0f));
}

// G2e: nearest upscale of a native rectangle of a plane into its scaled copy (refills from the shadow)
// G3a: s.z = the plane's widescreen pad (native px; the hi copy keeps the native origin at x = pad)
kernel void hi_upscale(texture2d<uint, access::read> src [[texture(0)]], texture2d<uint, access::write> dst [[texture(1)]],
                       constant uint4 &r [[buffer(0)]], constant uint4 &s [[buffer(1)]], uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= r.z * s.x || gid.y >= r.w * s.y)
        return;
    const uint2 d = uint2((r.x + s.z) * s.x + gid.x, r.y * s.y + gid.y);
    dst.write(src.read(uint2(d.x / s.x - s.z, d.y / s.y)), d);
}

// G2e: the scaled display rectangle -> RGBA8 (p: origin x, y, out w, h, row num, row den)
kernel void hi_present(texture2d<uint, access::read> src [[texture(0)]], device uint *out [[buffer(0)]],
                       constant uint *p [[buffer(1)]], uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= p[2] || gid.y >= p[3])
        return;
    const uint x = p[0] + gid.x, y = p[1] + (gid.y * p[4]) / p[5];
    uint v = 0u;
    if (x < src.get_width() && y < src.get_height())
        v = src.read(uint2(x, y)).x;
    out[gid.y * p[2] + gid.x] = v | 0xFF000000u;
}
)METAL";

    constexpr uint32_t kPages = 512u;

    struct PageRange
    {
        uint32_t pages[512];
        uint32_t count = 0;
    };

    // Pages covered by a pixel rectangle of a 32-bit (64x32-page) or other PSM surface.
    void pagesOfRect(uint32_t blockBase, uint32_t bw, uint8_t psm, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, PageRange &out)
    {
        out.count = 0;
        uint32_t pw = 64u, ph = 32u;
        switch (psm)
        {
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
        case GS_PSM_Z16:
        case GS_PSM_Z16S:
            ph = 64u;
            break;
        case GS_PSM_T8:
            pw = 128u;
            ph = 64u;
            break;
        case GS_PSM_T4:
            pw = 128u;
            ph = 128u;
            break;
        default:
            break;
        }
        const uint32_t ppr = std::max<uint32_t>(1u, (std::max<uint32_t>(bw, 1u) * 64u) / pw);
        const uint32_t base = blockBase >> 5;
        const uint32_t extra = (blockBase & 31u) != 0u ? 1u : 0u;
        for (uint32_t py = y0 / ph; py <= y1 / ph; ++py)
            for (uint32_t px = x0 / pw; px <= x1 / pw + extra; ++px)
            {
                if (out.count >= 512u)
                    return;
                out.pages[out.count++] = (base + py * ppr + px) % kPages;
            }
    }

    uint64_t nowNs()
    {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now().time_since_epoch())
                                         .count());
    }

    uint64_t readFpcr()
    {
#if defined(__aarch64__)
        uint64_t fpcr;
        __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
        return fpcr;
#else
        return 0;
#endif
    }

    bool fpcrIsTowardZero(uint64_t fpcr)
    {
#if defined(__aarch64__)
        return ((fpcr >> 22) & 3u) == 3u; // RMode = RZ
#else
        (void)fpcr;
        return std::fegetround() == FE_TOWARDZERO;
#endif
    }
}

void GSMetalBackend::Stats::Add(const Stats &o)
{
    submits += o.submits;
    metalPrims += o.metalPrims;
    metalTexPrims += o.metalTexPrims;
    metalFbPrims += o.metalFbPrims;
    metalSelfPrims += o.metalSelfPrims;
    fallbackPrims += o.fallbackPrims;
    runs += o.runs;
    targetUploads += o.targetUploads;
    readbackPixels += o.readbackPixels;
    gpuWaitNs += o.gpuWaitNs;
    presents += o.presents;
    texDecodes += o.texDecodes;
    texHits += o.texHits;
    texTexels += o.texTexels;
    palettes += o.palettes;
    paletteHits += o.paletteHits;
    for (uint32_t i = 0; i < kFlushWhyCount; ++i)
    {
        flushes[i] += o.flushes[i];
        waits[i] += o.waits[i];
    }
    commits += o.commits;
    partialUploads += o.partialUploads;
    uploadPixels += o.uploadPixels;
    recordNs += o.recordNs;
    texNs += o.texNs;
    uploadNs += o.uploadNs;
    encodeNs += o.encodeNs;
    writebackNs += o.writebackNs;
    fallbackNs += o.fallbackNs;
    for (const auto &[k, v] : o.fallbacks)
        fallbacks[k] += v;
}

void GSMetalPrintStats(const char *tag, const GSMetalBackend::Stats &s)
{
    std::fprintf(stderr, "[gsmtl] %s submits=%llu metal=%llu (textured=%llu) fallback=%llu metal_frac=%.4f runs=%llu uploads=%llu readback_px=%llu gpu_wait_ms=%.1f presents=%llu\n",
                 tag, (unsigned long long)s.submits, (unsigned long long)s.metalPrims, (unsigned long long)s.metalTexPrims, (unsigned long long)s.fallbackPrims,
                 s.submits ? double(s.metalPrims) / double(s.submits) : 0.0, (unsigned long long)s.runs,
                 (unsigned long long)s.targetUploads, (unsigned long long)s.readbackPixels, double(s.gpuWaitNs) / 1e6, (unsigned long long)s.presents);
    std::fprintf(stderr, "[gsmtl] %s textures decodes=%llu hits=%llu texels=%llu palettes=%llu palette_hits=%llu fb_target_sprites=%llu self_snapshot_sprites=%llu\n", tag, (unsigned long long)s.texDecodes,
                 (unsigned long long)s.texHits, (unsigned long long)s.texTexels, (unsigned long long)s.palettes, (unsigned long long)s.paletteHits,
                 (unsigned long long)s.metalFbPrims, (unsigned long long)s.metalSelfPrims);
    static const char *const kWhy[GSMetalBackend::kFlushWhyCount] = {"target", "tex_overlap", "clut", "transfer", "fallback", "present", "readback", "other", "palette", "limit"};
    std::fprintf(stderr, "[gsmtl] %s run_closes(encodes)", tag);
    for (uint32_t i = 0; i < GSMetalBackend::kFlushWhyCount; ++i)
        std::fprintf(stderr, " %s=%llu", kWhy[i], (unsigned long long)s.flushes[i]);
    std::fprintf(stderr, "\n[gsmtl] %s gpu_waits", tag);
    uint64_t wsum = 0;
    for (uint32_t i = 0; i < GSMetalBackend::kFlushWhyCount; ++i)
    {
        std::fprintf(stderr, " %s=%llu", kWhy[i], (unsigned long long)s.waits[i]);
        wsum += s.waits[i];
    }
    std::fprintf(stderr, " total=%llu commits=%llu full_uploads=%llu partial_uploads=%llu upload_px=%llu\n", (unsigned long long)wsum, (unsigned long long)s.commits,
                 (unsigned long long)s.targetUploads, (unsigned long long)s.partialUploads, (unsigned long long)s.uploadPixels);
    if (s.recordNs || s.texNs || s.uploadNs || s.encodeNs || s.writebackNs || s.fallbackNs)
        std::fprintf(stderr, "[gsmtl] %s time_ms record=%.1f tex_decode=%.1f target_upload=%.1f encode=%.1f gpu_wait=%.1f writeback=%.1f cpu_fallback=%.1f\n", tag,
                     double(s.recordNs) / 1e6, double(s.texNs) / 1e6, double(s.uploadNs) / 1e6, double(s.encodeNs) / 1e6, double(s.gpuWaitNs) / 1e6,
                     double(s.writebackNs) / 1e6, double(s.fallbackNs) / 1e6);
    for (const auto &[k, v] : s.fallbacks)
        std::fprintf(stderr, "[gsmtl] %s fallback %s=%llu\n", tag, k.c_str(), (unsigned long long)v);
    std::fflush(stderr);
}

// G2e: the newest scaled display frame (PS2X_GS_SCALE > 1), for frame dumps and the host window.
namespace
{
    // a short ring keyed by a hash of the 1x frame, so a dump pairs its 1x and scaled frames exactly
    struct HiFrame
    {
        std::vector<uint8_t> px;
        uint32_t w = 0, h = 0;
        uint64_t key = 0, serial = 0;
    };
    std::mutex g_hiMu;
    HiFrame g_hiRing[3];
    uint64_t g_hiSerial = 0;
}

uint64_t GSMetalFrameKey(const uint8_t *px, uint32_t w, uint32_t h, uint32_t strideBytes)
{
    uint64_t k = 1469598103934665603ull ^ (uint64_t(w) << 32) ^ h;
    for (uint32_t y = 0; y < h; ++y)
    {
        const uint64_t *row = reinterpret_cast<const uint64_t *>(px + size_t(y) * strideBytes);
        for (uint32_t i = 0; i < (w * 4u) / 8u; ++i)
            k = (k ^ row[i]) * 1099511628211ull;
    }
    return k;
}

void GSMetalPublishHires(std::vector<uint8_t> &&px, uint32_t w, uint32_t h, uint64_t key)
{
    std::lock_guard<std::mutex> lk(g_hiMu);
    HiFrame &e = g_hiRing[g_hiSerial % 3u];
    e.px = std::move(px);
    e.w = w;
    e.h = h;
    e.key = key;
    e.serial = ++g_hiSerial;
}

// key != 0: the frame whose 1x frame hashed to key (false when none); key == 0: the newest.
bool GSMetalCopyHiresFrame(std::vector<uint8_t> &px, uint32_t &w, uint32_t &h, uint64_t key)
{
    std::lock_guard<std::mutex> lk(g_hiMu);
    const HiFrame *best = nullptr;
    for (const HiFrame &e : g_hiRing)
        if (!e.px.empty() && (key == 0u ? (!best || e.serial > best->serial) : (e.key == key && (!best || e.serial > best->serial))))
            best = &e;
    if (!best)
        return false;
    px = best->px;
    w = best->w;
    h = best->h;
    return true;
}

// SH1 (M4): direct GPU present of the scaled frame into the sdl3 shell's CAMetalLayer. With the flag on
// (sdl3 window, no PS2X_DUMP_FRAMES_HIRES), BuildHiFrame encodes hi_present into a texture ring and
// commits without a GPU wait or CPU readback; the shell draws the texture on the same command queue.
namespace
{
    struct DirectHi
    {
        id<MTLTexture> tex = nil;
        uint32_t w = 0, h = 0;
        uint64_t key = 0, serial = 0;
    };
    std::mutex g_directMu;
    DirectHi g_directRing[3];
    uint64_t g_directSerial = 0;
    std::atomic<bool> g_directOn{false};
    std::atomic<bool> g_hiActive{false};
    std::atomic<float> g_wsFactor{1.0f}; // G3a: presented hi frame width / 4:3 width (1 = 4:3)
    id<MTLCommandQueue> g_sharedQueue = nil;
}

// G3a: the widescreen factor of the newest presented scaled frame (the shell letterboxes at 4:3 * factor).
float GSMetalWideFactor() { return g_wsFactor.load(std::memory_order_acquire); }

// G3a: PS2X_WIDESCREEN = "16:9" | "21:9" | "<w>:<h>" | "<ratio>" -> pad (native px each side of a 512-px 4:3 frame).
// Unset, "4:3", "0" or anything not wider than 4:3 -> 0 (off; nothing changes).
static uint32_t GSMetalWidescreenPad(uint32_t W)
{
    const char *e = std::getenv("PS2X_WIDESCREEN");
    if (!e || !*e)
        return 0u;
    double ratio = 0.0;
    if (const char *c = std::strchr(e, ':'))
    {
        const double a = std::atof(e), b = std::atof(c + 1);
        ratio = b > 0.0 ? a / b : 0.0;
    }
    else
        ratio = std::atof(e);
    if (ratio <= 4.0 / 3.0 + 1e-3 || ratio > 4.0)
        return 0u;
    const double k = ratio / (4.0 / 3.0);
    return uint32_t(std::lround(double(W) * (k - 1.0) * 0.5));
}

void GSMetalSetDirectHi(bool on) { g_directOn.store(on, std::memory_order_release); }
id<MTLCommandQueue> GSMetalSharedQueue() { return g_sharedQueue; }
bool GSMetalDirectHiOn() { return g_directOn.load(std::memory_order_acquire) && g_hiActive.load(std::memory_order_acquire); }

// key != 0: the texture whose 1x frame hashed to key (nil when none).
id<MTLTexture> GSMetalDirectHiTexture(uint64_t key, uint32_t &w, uint32_t &h)
{
    std::lock_guard<std::mutex> lk(g_directMu);
    const DirectHi *best = nullptr;
    for (const DirectHi &e : g_directRing)
        if (e.tex && e.key == key && (!best || e.serial > best->serial))
            best = &e;
    if (!best)
        return nil;
    w = best->w;
    h = best->h;
    return best->tex;
}

struct GSMetalBackend::Impl
{
    // One 32-bit-word VRAM surface kept on the GPU (colour planes use the C32 swizzle, depth planes Z24).
    // Colour and depth are separate planes so that targets sharing a Z buffer share one texture.
    struct Plane
    {
        uint32_t base = 0; // first VRAM page
        uint32_t fbw = 0;
        bool depth = false;
        uint32_t width = 0, height = 0;
        id<MTLTexture> tex = nil;
        id<MTLBuffer> wb = nil; // write-back destination (width*height words)
        std::bitset<kPages> span;  // pages of the whole plane
        std::bitset<kPages> dirty; // pages the GPU wrote since the last write-back to the shadow
        int dx0 = 0, dy0 = 0, dx1 = -1, dy1 = -1; // bounding box of the GPU-newer pixels (inclusive)
        int lpx0 = 1, lpy0 = 1, lpx1 = 0, lpy1 = 0; // last marked page rectangle (fast path)
        uint64_t syncEpoch = 0;
        bool valid = false;
        id<MTLTexture> hiTex = nil; // G2e display-only scaled copy (mirrors every write to tex)
        bool hiValid = false;
        uint32_t hiPad = 0; // G3a: widescreen pad (native px each side) of the scaled copy; 0 = not padded
        bool anyDirty() const { return dx1 >= dx0; }
    };

    struct Target
    {
        uint32_t fbp = 0, zbp = 0, fbw = 0;
        Plane *c = nullptr, *z = nullptr;
    };

    // ---- textured triangles (G2a)
    struct TexKey
    {
        uint32_t tbp = 0, tbw = 0, psm = 0, w = 0, h = 0, texa = 0;
        bool operator==(const TexKey &o) const
        {
            return tbp == o.tbp && tbw == o.tbw && psm == o.psm && w == o.w && h == o.h && texa == o.texa;
        }
    };
    struct TexKeyHash
    {
        size_t operator()(const TexKey &k) const
        {
            uint64_t h = 0x9E3779B97F4A7C15ull;
            for (uint32_t v : {k.tbp, k.tbw, k.psm, k.w, k.h, k.texa})
            {
                h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
                h *= 0xFF51AFD7ED558CCDull;
            }
            return static_cast<size_t>(h ^ (h >> 29));
        }
    };
    struct TexEntry
    {
        id<MTLTexture> tex = nil;
        std::vector<uint16_t> pages; // VRAM pages the texels can come from
        uint64_t epoch = 0;          // global epoch at decode time
        uint64_t checked = 0;        // global epoch at the last validation
        uint64_t lastUse = 0;
        size_t bytes = 0;
    };
    struct TexSetup
    {
        bool on = false;
        TexKey key;
        uint32_t texW = 1, texH = 1;
        uint32_t wms = 0, wmt = 0, minU = 0, maxU = 0, minV = 0, maxV = 0;
        uint32_t tfx = 0, tcc = 0;
        bool indexed = false, fst = false, linear = false;
    };
    struct Segment
    {
        uint32_t first = 0;
        id<MTLTexture> tex = nil;
        id<MTLTexture> hiTex = nil; // scaled-pass binding when it differs (framebuffer sources)
    };

    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    id<MTLRenderPipelineState> pipeline = nil;
    id<MTLComputePipelineState> selftestF = nil;
    id<MTLComputePipelineState> selftestZ = nil;
    id<MTLComputePipelineState> selftestT = nil;
    id<MTLLibrary> lib = nil;
    // G2d: GPU write-back straight into the shadow (no CPU swizzle). PS2X_GS_METAL_CPU_WRITEBACK=1 forces the CPU loop.
    id<MTLComputePipelineState> wbScatter = nil;
    id<MTLBuffer> shadowBuf = nil; // no-copy alias of the page-rounded shadow range
    size_t shadowOff = 0;          // offset of vram[0] inside shadowBuf
    id<MTLBuffer> tblC32 = nil, tblZ24 = nil;
    bool cpuWriteback = false;
    // G2e: display-only scaled pass (PS2X_GS_SCALE=s: sx = s, sy = 2s with PS2X_GS_PROGRESSIVE, the default for s > 1)
    bool hi = false;
    uint32_t hsx = 1, hsy = 1;
    id<MTLBuffer> directBuf = nil; // SH1: hi_present output for the direct present (one buffer; same-queue ordering)
    id<MTLTexture> directTex[3] = {nil, nil, nil};
    uint32_t directSlot = 0;
    uint32_t hiDbg = std::getenv("PS2X_GS_HI_DEBUG") ? uint32_t(std::atoi(std::getenv("PS2X_GS_HI_DEBUG"))) : 0u;
    id<MTLRenderPipelineState> hiPipeline = nil;
    id<MTLComputePipelineState> hiUpscale = nil, hiPresent = nil;
    id<MTLTexture> snapHi = nil, primTexHi = nil;
    id<MTLBuffer> hiOut = nil;
    uint64_t hiUpscalePx = 0, hiPresents = 0, hiFallbacks = 0, hiPasses = 0;
    // G3a widescreen (PS2X_WIDESCREEN=16:9|21:9|<ratio>): display planes of width wsW get a scaled copy padded by
    // wsPad native px on each side. Display-only (the hi pass); the 1x pass, the shadow and the EE never see it.
    uint32_t wsPad = 0, wsW = 512u;
    std::vector<GPUVertex> vertsHi; // parallel to verts: the same prims, positions in padded native x
    std::vector<float> xfHi;        // per prim (kx, ox, skx, sox): padded x = kx * native x + ox; same for a padded fb source
    bool fbSrcPadded = false;
    uint64_t wsCls[5] = {0, 0, 0, 0, 0}; // prims by class: 3D, left, centre, right, stretch
    // 2D anchoring. Full-width layers: a 2D GIF packet (by its order in the frame) whose x-coverage (8-px bins)
    // spans the whole width is a background layer and stretches. Everything else is anchored by the connected
    // component (8-px cells, previous frame; this frame's prim when new) that contains the prim's centre: the
    // pieces of one widget (dialogue box, its caps, name tag, text) move together; a component left of 45% of the
    // width anchors left, right of 55% anchors right, anything else stays centred.
    struct WsAnchor
    {
        static constexpr int G = 64; // cells per axis (8 px over 512)
        uint32_t lastPacket = ~0u;
        int ordinal = -1;
        std::vector<uint64_t> cur, prev;
        // [0] 2D prims, [1] PATH1 overlays without a depth test (minimap; also sky pieces, which form one wide
        // component and so stay 3D)
        std::bitset<G * G> occ[2];                                          // this frame's cells
        std::vector<int16_t> label[2] = {std::vector<int16_t>(G * G, -1), std::vector<int16_t>(G * G, -1)}; // previous frame
        std::vector<std::array<int, 2>> compX[2];                           // component x-range in cells
        static uint64_t Bins(int x0, int x1, int W)
        {
            const int b0 = std::max(0, std::min(63, x0 * 64 / W)), b1 = std::max(0, std::min(63, x1 * 64 / W));
            return (b1 >= 63 ? ~0ull : ((2ull << b1) - 1ull)) & ~((1ull << b0) - 1ull);
        }
        static int Cell(int v, int W) { return std::max(0, std::min(G - 1, v * G / W)); }
        // 0 keep as 3D (overlay test only), 1 left, 2 centre, 3 right, 4 stretch
        int Classify(uint32_t packet, int x0, int x1, int y0, int y1, int W, bool overlay)
        {
            if (packet != lastPacket)
            {
                lastPacket = packet;
                ++ordinal;
                if (cur.size() <= size_t(ordinal))
                    cur.resize(size_t(ordinal) + 1, 0ull);
            }
            const uint64_t mine = Bins(x0, x1, W);
            cur[size_t(ordinal)] |= mine;
            if (!overlay)
            {
                if ((x1 - x0 + 1) * 100 >= W * 97)
                    return 4;
                uint64_t m = cur[size_t(ordinal)];
                if (size_t(ordinal) < prev.size() && (prev[size_t(ordinal)] & mine))
                    m |= prev[size_t(ordinal)];
                if (m == ~0ull)
                    return 4; // a layer across the whole width (tiled backgrounds): stretch with it
            }
            const int cx0 = Cell(x0, W), cx1 = Cell(x1, W), cy0 = Cell(y0, W), cy1 = Cell(y1, W);
            const int g = overlay ? 1 : 0;
            for (int y = cy0; y <= cy1; ++y)
                for (int x = cx0; x <= cx1; ++x)
                    occ[g].set(size_t(y * G + x));
            int lo = cx0, hi = cx1;
            const int l = label[g][size_t(Cell((y0 + y1) / 2, W) * G + Cell((x0 + x1) / 2, W))];
            if (l >= 0)
            {
                lo = std::min(lo, compX[g][size_t(l)][0]);
                hi = std::max(hi, compX[g][size_t(l)][1]);
            }
            if ((hi + 1) * 100 <= G * 45)
                return 1;
            if (lo * 100 >= G * 55)
                return 3;
            return overlay ? 0 : 2;
        }
        void EndFrame()
        {
            if (ordinal >= 0)
            {
                cur.resize(size_t(ordinal) + 1);
                prev.swap(cur);
                cur.assign(prev.size(), 0ull);
                for (int g = 0; g < 2; ++g) // only after a frame that drew (presents without drawing keep the labels)
                    Label(occ[g], label[g], compX[g]);
            }
            ordinal = -1;
            lastPacket = ~0u;
        }
        // label the frame's cells (4-connected), then clear them
        static void Label(std::bitset<G * G> &occ, std::vector<int16_t> &label, std::vector<std::array<int, 2>> &compX)
        {
            {
                std::fill(label.begin(), label.end(), int16_t(-1));
                compX.clear();
                std::vector<int> stack;
                for (int i = 0; i < G * G; ++i)
                {
                    if (!occ.test(size_t(i)) || label[size_t(i)] >= 0 || compX.size() >= 32000)
                        continue;
                    const int16_t id = int16_t(compX.size());
                    compX.push_back({G, -1});
                    stack.assign(1, i);
                    label[size_t(i)] = id;
                    while (!stack.empty())
                    {
                        const int c = stack.back();
                        stack.pop_back();
                        const int cx = c % G, cy = c / G;
                        compX.back()[0] = std::min(compX.back()[0], cx);
                        compX.back()[1] = std::max(compX.back()[1], cx);
                        const int nb[4] = {cx > 0 ? c - 1 : -1, cx < G - 1 ? c + 1 : -1, cy > 0 ? c - G : -1, cy < G - 1 ? c + G : -1};
                        for (int n : nb)
                            if (n >= 0 && occ.test(size_t(n)) && label[size_t(n)] < 0)
                            {
                                label[size_t(n)] = id;
                                stack.push_back(n);
                            }
                    }
                }
                occ.reset();
            }
        }
    } wsAnchor;
    double gpuBusyS = 0.0;
    uint64_t lastPmode = ~0ull, lastSmode2 = ~0ull;
    uint32_t modeLogs = 0;

    void EnsureSelftestPipelines()
    {
        NSError *err = nil;
        if (!selftestF)
            selftestF = [device newComputePipelineStateWithFunction:[lib newFunctionWithName:@"selftest_f"] error:&err];
        if (!selftestZ)
            selftestZ = [device newComputePipelineStateWithFunction:[lib newFunctionWithName:@"selftest_z"] error:&err];
        if (!selftestT)
            selftestT = [device newComputePipelineStateWithFunction:[lib newFunctionWithName:@"selftest_t"] error:&err];
    }

    void BindShadow()
    {
        shadowBuf = nil;
        shadowOff = 0;
        if (cpuWriteback || !wbScatter || !vram || vramSize != 4u * 1024u * 1024u)
            return;
        const uintptr_t pg = uintptr_t(getpagesize());
        const uintptr_t a = reinterpret_cast<uintptr_t>(vram);
        const uintptr_t lo = a & ~(pg - 1u), hi = (a + vramSize + pg - 1u) & ~(pg - 1u);
        shadowBuf = [device newBufferWithBytesNoCopy:reinterpret_cast<void *>(lo) length:size_t(hi - lo)
                                             options:MTLResourceStorageModeShared deallocator:nil];
        if (shadowBuf)
            shadowOff = size_t(a - lo);
        else
            std::fprintf(stderr, "[gsmtl] shadow alias failed: CPU write-back\n");
    }

    std::unique_ptr<GSCpuBackend> cpu;
    uint8_t *vram = nullptr;
    uint32_t vramSize = 0;
    bool timing = false;
    bool flushAll = false; // PS2X_GS_METAL_FLUSH_ALL=1: close the run before every CPU-side operation (M1 behaviour)

    std::array<uint64_t, kPages> pageEpoch{};
    uint64_t epoch = 1;
    std::vector<std::unique_ptr<Plane>> planes;
    std::vector<std::unique_ptr<Target>> targets;
    bool anyDirtyPlane = false;
    uint32_t clutCbpMirror[2] = {0xFFFFFFFFu, 0xFFFFFFFFu}; // mirrors GSCpuBackend::m_clutCbp (cld 4/5 skip rule)
    bool eager = false; // resolve (wait + write back) after every run: M1 behaviour, tee comparisons
    id<MTLCommandBuffer> cur = nil;
    std::vector<id<MTLCommandBuffer>> inflightCmds;
    std::vector<id<MTLBuffer>> freeBufs, inflightBufs;
    size_t inflightBytes = 0;

    // open run
    Target *run = nullptr;
    std::vector<GPUVertex> verts;
    std::vector<GPUPrim> prims;
    std::vector<Segment> segments;
    id<MTLTexture> curTex = nil;
    std::bitset<kPages> runPages; // pages of the run target's colour and Z planes (conservative)
    uint32_t runPrims = 0;

    // texture cache, palette buffer
    TexSetup ts;
    std::unordered_map<TexKey, TexEntry, TexKeyHash> texCache;
    size_t texBytes = 0;
    uint64_t useTick = 0;
    id<MTLTexture> dummyTex = nil;
    std::vector<uint8_t> scratch8;
    std::vector<uint32_t> scratch32;
    static constexpr uint32_t kPalSlots = 16384u;
    id<MTLBuffer> palBuf = nil;
    uint32_t palUsed = 0;
    std::unordered_map<uint64_t, uint32_t> palMap;
    bool lastPalValid = false;
    uint64_t lastPalGen = 0, lastPalKey = 0;
    uint32_t lastPalOff = 0;
    uint32_t curPalOff = 0;
    id<MTLTexture> primTex = nil;
    // textured sprites (G2b)
    id<MTLTexture> snapTex = nil;  // read-before-write copy of the run target for a feedback sprite
    Target *snapTarget = nullptr;
    bool fbSrc = false, fbCt24 = false;

    // transfer page tracking
    GSTransferCommand transfer{};
    PageRange transferPages{};

    std::function<void(const RunInfo &)> observer;
    Stats frame;
    Stats total;

    void MarkPages(const PageRange &r)
    {
        ++epoch;
        for (uint32_t i = 0; i < r.count; ++i)
            pageEpoch[r.pages[i]] = epoch;
    }

    void MarkAll()
    {
        ++epoch;
        pageEpoch.fill(epoch);
    }

    static std::bitset<kPages> BitsOf(const PageRange &r)
    {
        std::bitset<kPages> b;
        if (r.count >= kPages)
            b.set();
        else
            for (uint32_t i = 0; i < r.count; ++i)
                b.set(r.pages[i]);
        return b;
    }

    // ---------------------------------------------------------------- planes, command buffers, pools
    void ComputeSpan(Plane &p)
    {
        PageRange r;
        pagesOfRect(p.base << 5, p.fbw, GS_PSM_CT32, 0u, 0u, p.width - 1u, p.height - 1u, r);
        p.span = BitsOf(r);
    }

    void AllocatePlane(Plane &p, uint32_t height)
    {
        p.width = p.fbw * 64u;
        p.height = height;
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Uint
                                                                                     width:p.width
                                                                                    height:p.height
                                                                                 mipmapped:NO];
        d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        d.storageMode = MTLStorageModePrivate;
        p.tex = [device newTextureWithDescriptor:d];
        p.wb = [device newBufferWithLength:size_t(p.width) * p.height * 4u options:MTLResourceStorageModeShared];
        p.hiTex = nil;
        p.hiValid = false;
        p.hiPad = (hi && wsPad && p.width == wsW && p.height >= 224u) ? wsPad : 0u;
        if (hi)
        {
            if ((p.width + 2u * p.hiPad) * hsx > 16384u || p.height * hsy > 16384u)
            {
                std::fprintf(stderr, "[gsmtl-hi] plane %ux%u too large at %ux%u: scaled pass off\n", p.width, p.height, hsx, hsy);
                std::fflush(stderr);
                hi = false;
            }
            else
            {
                MTLTextureDescriptor *hd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Uint
                                                                                              width:(p.width + 2u * p.hiPad) * hsx
                                                                                             height:p.height * hsy
                                                                                          mipmapped:NO];
                hd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
                hd.storageMode = MTLStorageModePrivate;
                p.hiTex = [device newTextureWithDescriptor:hd];
            }
        }
        p.valid = false;
        p.syncEpoch = 0;
        p.dirty.reset();
        p.dx0 = p.dy0 = 0;
        p.dx1 = p.dy1 = -1;
        p.lpx0 = p.lpy0 = 1;
        p.lpx1 = p.lpy1 = 0;
        ComputeSpan(p);
    }

    Plane *GetPlane(uint32_t base, uint32_t fbw, bool depth, uint32_t needHeight)
    {
        const uint32_t height = std::max<uint32_t>(32u, (needHeight + 31u) & ~31u);
        for (auto &q : planes)
            if (q->base == base && q->fbw == fbw && q->depth == depth)
            {
                if (q->height >= height)
                    return q.get();
                // growing loses the texture: its GPU-newer pixels must reach the shadow first
                std::vector<Plane *> one{q.get()};
                if (q->anyDirty())
                    ResolveList(one, kFlushTarget);
                AllocatePlane(*q, height);
                return q.get();
            }
        auto q = std::make_unique<Plane>();
        q->base = base;
        q->fbw = fbw;
        q->depth = depth;
        AllocatePlane(*q, height);
        planes.push_back(std::move(q));
        return planes.back().get();
    }

    Target *GetTarget(uint32_t fbp, uint32_t zbp, uint32_t fbw, uint32_t needHeight)
    {
        Plane *c = GetPlane(fbp, fbw, false, needHeight);
        Plane *z = GetPlane(zbp, fbw, true, needHeight);
        for (auto &t : targets)
            if (t->fbp == fbp && t->zbp == zbp && t->fbw == fbw)
                return t.get();
        auto t = std::make_unique<Target>();
        t->fbp = fbp;
        t->zbp = zbp;
        t->fbw = fbw;
        t->c = c;
        t->z = z;
        targets.push_back(std::move(t));
        return targets.back().get();
    }

    bool PlaneStale(const Plane &p) const
    {
        if (!p.valid)
            return true;
        if (p.syncEpoch == epoch)
            return false;
        for (uint32_t i = 0; i < kPages; ++i)
            if (p.span.test(i) && pageEpoch[i] > p.syncEpoch)
                return true;
        return false;
    }

    void ComputeRunPages()
    {
        runPages.reset();
        if (run)
            runPages = run->c->span | run->z->span;
    }

    // The run's target for this draw (closing the open run when it is another target or too short).
    Target *SelectRun(const GSContext &ctx)
    {
        const uint32_t needHeight = uint32_t(ctx.scissor.y1) + 1u;
        if (run && run->fbp == ctx.frame.fbp && run->zbp == ctx.zbuf.zbp && run->fbw == ctx.frame.fbw && run->c->height >= needHeight &&
            run->z->height >= needHeight)
            return run;
        {
            static const bool dbg = std::getenv("PS2X_GS_METAL_DBG") != nullptr;
            static int shown = 0;
            if (dbg && run && shown++ < 40)
            {
                std::fprintf(stderr, "[gsmtl-dbg] switch (fbp=%u zbp=%u fbw=%u prims=%u) -> (fbp=%u zbp=%u fbw=%u need=%u)\n", run->fbp, run->zbp, run->fbw, runPrims,
                             ctx.frame.fbp, ctx.zbuf.zbp, ctx.frame.fbw, needHeight);
                std::fflush(stderr);
            }
        }
        CloseRun(kFlushTarget);
        Target *t = GetTarget(ctx.frame.fbp, ctx.zbuf.zbp, ctx.frame.fbw, needHeight);
        run = t;
        ComputeRunPages();
        // GPU-newer pixels of any other plane sharing pages with this target must reach the shadow first
        if (anyDirtyPlane)
            ResolvePlanes(&runPages, false, t->c, t->z, kFlushTarget);
        return t;
    }

    // ---- GPU resources: one command buffer is filled by consecutive runs and committed without waiting;
    // buffers it uses come from a pool and are recycled only after a wait.
    id<MTLCommandBuffer> GetCmd()
    {
        if (!cur)
            cur = [queue commandBuffer];
        return cur;
    }

    void CommitCur()
    {
        if (!cur)
            return;
        [cur commit];
        inflightCmds.push_back(cur);
        cur = nil;
        ++frame.commits;
    }

    id<MTLBuffer> GrabBuf(size_t bytes)
    {
        size_t best = SIZE_MAX;
        for (size_t i = 0; i < freeBufs.size(); ++i)
            if (freeBufs[i].length >= bytes && (best == SIZE_MAX || freeBufs[i].length < freeBufs[best].length))
                best = i;
        id<MTLBuffer> b;
        if (best != SIZE_MAX)
        {
            b = freeBufs[best];
            freeBufs.erase(freeBufs.begin() + std::ptrdiff_t(best));
        }
        else
        {
            size_t len = std::max<size_t>(bytes + bytes / 2u, size_t(256u) << 10);
            len = (len + 65535u) & ~size_t(65535u);
            b = [device newBufferWithLength:len options:MTLResourceStorageModeShared];
        }
        inflightBufs.push_back(b);
        inflightBytes += b.length;
        return b;
    }

    // Wait for every committed command buffer (they run in order); recycles pool buffers and the palette ring.
    void WaitAll(FlushWhy why)
    {
        if (!inflightCmds.empty())
        {
            const uint64_t t0 = nowNs();
            for (id<MTLCommandBuffer> c : inflightCmds)
            {
                [c waitUntilCompleted];
                gpuBusyS += c.GPUEndTime - c.GPUStartTime;
                if (c.status == MTLCommandBufferStatusError)
                {
                    std::fprintf(stderr, "[gsmtl] command buffer error: %s\n", c.error.localizedDescription.UTF8String);
                    std::fflush(stderr);
                }
            }
            frame.gpuWaitNs += nowNs() - t0;
            ++frame.waits[why];
            inflightCmds.clear();
        }
        for (id<MTLBuffer> b : inflightBufs)
            freeBufs.push_back(b);
        inflightBufs.clear();
        inflightBytes = 0;
        ResetPalettes();
    }

    void ResetPalettes()
    {
        if (inflightCmds.empty() && prims.empty() && !cur)
        {
            palUsed = 0;
            palMap.clear();
            lastPalValid = false;
        }
    }

    // ---- uploading the shadow into a plane (stale pages only)
    struct PageRect
    {
        uint32_t x, y, w, h;
    };

    void EncodeRefill(Plane &p)
    {
        if (p.valid && p.syncEpoch == epoch)
            return;
        std::vector<PageRect> rects;
        if (!p.valid)
            rects.push_back({0u, 0u, p.width, p.height});
        else
        {
            const uint32_t rows = p.height / 32u;
            for (uint32_t py = 0; py < rows; ++py)
            {
                int run0 = -1;
                for (uint32_t px = 0; px <= p.fbw; ++px)
                {
                    const bool st = px < p.fbw && pageEpoch[(p.base + py * p.fbw + px) % kPages] > p.syncEpoch;
                    if (st && run0 < 0)
                        run0 = int(px);
                    else if (!st && run0 >= 0)
                    {
                        rects.push_back({uint32_t(run0) * 64u, py * 32u, (px - uint32_t(run0)) * 64u, 32u});
                        run0 = -1;
                    }
                }
            }
        }
        p.syncEpoch = epoch;
        const bool wasValid = p.valid;
        p.valid = true;
        if (rects.empty())
            return;
        const uint64_t tu0 = timing ? nowNs() : 0;
        size_t words = 0;
        for (const PageRect &r : rects)
            words += size_t(r.w) * r.h;
        id<MTLBuffer> up = GrabBuf(words * 4u);
        uint32_t *dst = static_cast<uint32_t *>(up.contents);
        const GSMem::SwizzledSurface<GSMem::C32> cs(p.base << 5, p.fbw);
        const GSMem::SwizzledSurface<GSMem::Z24> zs(p.base << 5, p.fbw);
        size_t off = 0;
        for (const PageRect &r : rects)
        {
            for (uint32_t y = 0; y < r.h; ++y)
                for (uint32_t x = 0; x < r.w; ++x)
                {
                    const uint32_t byteAddress = p.depth ? zs.Locate(r.x + x, r.y + y).byteAddress : cs.Locate(r.x + x, r.y + y).byteAddress;
                    std::memcpy(&dst[off + size_t(y) * r.w + x], vram + byteAddress, 4);
                }
            off += size_t(r.w) * r.h;
        }
        id<MTLCommandBuffer> cmd = GetCmd();
        id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
        off = 0;
        for (const PageRect &r : rects)
        {
            [blit copyFromBuffer:up sourceOffset:off * 4u sourceBytesPerRow:r.w * 4u sourceBytesPerImage:size_t(r.w) * r.h * 4u
                      sourceSize:MTLSizeMake(r.w, r.h, 1) toTexture:p.tex destinationSlice:0 destinationLevel:0
               destinationOrigin:MTLOriginMake(r.x, r.y, 0)];
            off += size_t(r.w) * r.h;
        }
        [blit endEncoding];
        if (hi && p.hiTex)
        {
            if (!p.hiValid)
            {
                rects.clear();
                rects.push_back({0u, 0u, p.width, p.height});
            }
            EncodeUpscale(cmd, p, rects);
            p.hiValid = true;
        }
        if (!wasValid)
            ++frame.targetUploads;
        else
            ++frame.partialUploads;
        frame.uploadPixels += words;
        if (timing)
            frame.uploadNs += nowNs() - tu0;
    }

    void EncodeUpscale(id<MTLCommandBuffer> cmd, Plane &p, const std::vector<PageRect> &rects)
    {
        id<MTLComputeCommandEncoder> ce = [cmd computeCommandEncoder];
        [ce setComputePipelineState:hiUpscale];
        [ce setTexture:p.tex atIndex:0];
        [ce setTexture:p.hiTex atIndex:1];
        const uint32_t sc[4] = {hsx, hsy, p.hiPad, 0u};
        [ce setBytes:sc length:sizeof(sc) atIndex:1];
        for (const PageRect &r : rects)
        {
            const uint32_t rr[4] = {r.x, r.y, r.w, r.h};
            [ce setBytes:rr length:sizeof(rr) atIndex:0];
            [ce dispatchThreads:MTLSizeMake(r.w * hsx, r.h * hsy, 1) threadsPerThreadgroup:MTLSizeMake(32, 8, 1)];
            hiUpscalePx += uint64_t(r.w) * r.h;
        }
        [ce endEncoding];
    }

    // ---- GPU-newer pixels -> shadow
    void MarkDirty(Plane &p, int x0, int y0, int x1, int y1)
    {
        if (p.dx1 < p.dx0)
        {
            p.dx0 = x0;
            p.dy0 = y0;
            p.dx1 = x1;
            p.dy1 = y1;
        }
        else
        {
            p.dx0 = std::min(p.dx0, x0);
            p.dy0 = std::min(p.dy0, y0);
            p.dx1 = std::max(p.dx1, x1);
            p.dy1 = std::max(p.dy1, y1);
        }
        anyDirtyPlane = true;
        const int px0 = x0 >> 6, px1 = x1 >> 6, py0 = y0 >> 5, py1 = y1 >> 5;
        if (px0 >= p.lpx0 && px1 <= p.lpx1 && py0 >= p.lpy0 && py1 <= p.lpy1)
            return;
        for (int py = py0; py <= py1; ++py)
            for (int px = px0; px <= px1; ++px)
                p.dirty.set((p.base + uint32_t(py) * p.fbw + uint32_t(px)) % kPages);
        p.lpx0 = px0;
        p.lpx1 = px1;
        p.lpy0 = py0;
        p.lpy1 = py1;
    }

    // Brings the shadow up to date for the given planes: encodes the open run when it renders to one of
    // them, copies each plane's dirty rectangle into its write-back buffer, waits, swizzles it into the
    // shadow and marks the pages written.
    void ResolveList(std::vector<Plane *> &L, FlushWhy why)
    {
        if (L.empty())
            return;
        @autoreleasepool
        {
            if (run)
                for (Plane *p : L)
                    if (p == run->c || p == run->z)
                    {
                        CloseRun(why);
                        break;
                    }
            L.erase(std::remove_if(L.begin(), L.end(), [](Plane *q) { return !q->anyDirty(); }), L.end());
            if (L.empty())
                return;
            for (Plane *p : L)
                EncodeRefill(*p);
            id<MTLCommandBuffer> cmd = GetCmd();
            if (shadowBuf)
            {
                // Planes may share pages: barriers keep the list order of the CPU loop.
                id<MTLComputeCommandEncoder> ce = [cmd computeCommandEncoderWithDispatchType:MTLDispatchTypeConcurrent];
                [ce setComputePipelineState:wbScatter];
                [ce setBuffer:shadowBuf offset:shadowOff atIndex:0];
                bool first = true;
                for (Plane *p : L)
                {
                    if (!first)
                        [ce memoryBarrierWithScope:MTLBarrierScopeBuffers];
                    first = false;
                    const uint32_t w = uint32_t(p->dx1 - p->dx0 + 1), h = uint32_t(p->dy1 - p->dy0 + 1);
                    const uint32_t prm[6] = {uint32_t(p->dx0), uint32_t(p->dy0), w, h, p->base, p->fbw};
                    [ce setTexture:p->tex atIndex:0];
                    [ce setBuffer:(p->depth ? tblZ24 : tblC32) offset:0 atIndex:1];
                    [ce setBytes:prm length:sizeof(prm) atIndex:2];
                    [ce dispatchThreads:MTLSizeMake(w, h, 1) threadsPerThreadgroup:MTLSizeMake(32, 8, 1)];
                }
                [ce endEncoding];
            }
            else
            {
                id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
                for (Plane *p : L)
                {
                    const uint32_t w = uint32_t(p->dx1 - p->dx0 + 1), h = uint32_t(p->dy1 - p->dy0 + 1);
                    [blit copyFromTexture:p->tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(p->dx0, p->dy0, 0)
                               sourceSize:MTLSizeMake(w, h, 1) toBuffer:p->wb destinationOffset:0
                      destinationBytesPerRow:w * 4u destinationBytesPerImage:size_t(w) * h * 4u];
                }
                [blit endEncoding];
            }
            CommitCur();
            WaitAll(why);
        }
        for (Plane *p : L)
        {
            const uint64_t tw0 = timing ? nowNs() : 0;
            const uint32_t w = uint32_t(p->dx1 - p->dx0 + 1), h = uint32_t(p->dy1 - p->dy0 + 1);
            const uint32_t *src = static_cast<const uint32_t *>(p->wb.contents);
            if (shadowBuf)
            {
                // already scattered into the shadow by the GPU
            }
            else if (p->depth)
            {
                const GSMem::SwizzledSurface<GSMem::Z24> zs(p->base << 5, p->fbw);
                for (uint32_t y = 0; y < h; ++y)
                    for (uint32_t x = 0; x < w; ++x)
                        std::memcpy(vram + zs.Locate(uint32_t(p->dx0) + x, uint32_t(p->dy0) + y).byteAddress, &src[size_t(y) * w + x], 4);
            }
            else
            {
                const GSMem::SwizzledSurface<GSMem::C32> cs(p->base << 5, p->fbw);
                for (uint32_t y = 0; y < h; ++y)
                    for (uint32_t x = 0; x < w; ++x)
                        std::memcpy(vram + cs.Locate(uint32_t(p->dx0) + x, uint32_t(p->dy0) + y).byteAddress, &src[size_t(y) * w + x], 4);
            }
            frame.readbackPixels += uint64_t(w) * h;
            PageRange r;
            pagesOfRect(p->base << 5, p->fbw, GS_PSM_CT32, uint32_t(p->dx0), uint32_t(p->dy0), uint32_t(p->dx1), uint32_t(p->dy1), r);
            MarkPages(r);
            p->syncEpoch = epoch; // the plane equals the shadow again
            RunInfo info;
            info.fbp = p->base;
            info.zbp = p->depth ? p->base : 0u;
            info.fbw = p->fbw;
            info.x0 = p->dx0;
            info.y0 = p->dy0;
            info.x1 = p->dx1;
            info.y1 = p->dy1;
            info.prims = 0;
            p->dirty.reset();
            p->dx0 = p->dy0 = 0;
            p->dx1 = p->dy1 = -1;
            p->lpx0 = p->lpy0 = 1;
            p->lpx1 = p->lpy1 = 0;
            ++frame.runs;
            if (timing)
                frame.writebackNs += nowNs() - tw0;
            if (observer && !p->depth)
                observer(info);
        }
        anyDirtyPlane = false;
        for (auto &q : planes)
            if (q->anyDirty())
            {
                anyDirtyPlane = true;
                break;
            }
    }

    // Resolve every plane with GPU-newer pixels on `pages` (nullptr = anywhere); optionally colour planes only.
    bool ResolvePlanes(const std::bitset<kPages> *pages, bool colorOnly, const Plane *skipA, const Plane *skipB, FlushWhy why)
    {
        if (!anyDirtyPlane)
            return false;
        std::vector<Plane *> L;
        for (auto &q : planes)
        {
            if (!q->anyDirty() || (colorOnly && q->depth) || q.get() == skipA || q.get() == skipB)
                continue;
            if (pages && (q->dirty & *pages).none())
                continue;
            L.push_back(q.get());
        }
        if (L.empty())
            return false;
        ResolveList(L, why);
        return true;
    }

    void ResolveAll(FlushWhy why)
    {
        CloseRun(why);
        ResolvePlanes(nullptr, false, nullptr, nullptr, why);
    }

    // Everything submitted has finished (destructor, public FlushRun).
    void Drain(FlushWhy why)
    {
        ResolveAll(why);
        if (!inflightCmds.empty() || cur)
        {
            CommitCur();
            WaitAll(why);
        }
    }

    // A CPU read of VRAM pages (CLUT, texture decode, transfer source).
    void CpuRead(const PageRange &r, FlushWhy why)
    {
        if (!anyDirtyPlane)
            return;
        const std::bitset<kPages> b = BitsOf(r);
        ResolvePlanes(&b, false, nullptr, nullptr, why);
    }

    // A CPU write of VRAM pages: GPU-newer pixels there reach the shadow first, and a run rendering to
    // those pages ends so that its target is refilled afterwards.
    void CpuWrite(const PageRange &r, FlushWhy why)
    {
        if (!run && !anyDirtyPlane)
            return;
        const std::bitset<kPages> b = BitsOf(r);
        ResolvePlanes(&b, false, nullptr, nullptr, why);
        if (run && (runPages & b).any())
            CloseRun(why);
    }

    // Encode (and commit, without waiting) the open run.
    void CloseRun(FlushWhy why = kFlushOther)
    {
        if (!run)
            return;
        Target &t = *run;
        const bool draw = !prims.empty();
        if (draw)
        {
            @autoreleasepool
            {
                ++frame.flushes[why];
                const uint64_t te0 = timing ? nowNs() : 0;
                EncodeRefill(*t.c);
                EncodeRefill(*t.z);
                id<MTLCommandBuffer> cmd = GetCmd();
                if (snapTex && snapTarget == &t)
                {
                    id<MTLBlitCommandEncoder> sb = [cmd blitCommandEncoder];
                    [sb copyFromTexture:t.c->tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                             sourceSize:MTLSizeMake(t.c->width, t.c->height, 1) toTexture:snapTex destinationSlice:0 destinationLevel:0
                      destinationOrigin:MTLOriginMake(0, 0, 0)];
                    [sb endEncoding];
                }
                id<MTLBuffer> vb = GrabBuf(verts.size() * sizeof(GPUVertex));
                std::memcpy(vb.contents, verts.data(), verts.size() * sizeof(GPUVertex));
                id<MTLBuffer> pb = GrabBuf(prims.size() * sizeof(GPUPrim));
                std::memcpy(pb.contents, prims.data(), prims.size() * sizeof(GPUPrim));
                MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
                rp.colorAttachments[0].texture = t.c->tex;
                rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
                rp.colorAttachments[0].storeAction = MTLStoreActionStore;
                rp.colorAttachments[1].texture = t.z->tex;
                rp.colorAttachments[1].loadAction = MTLLoadActionLoad;
                rp.colorAttachments[1].storeAction = MTLStoreActionStore;
                id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];
                [enc setRenderPipelineState:pipeline];
                MTLViewport vp = {0.0, 0.0, double(t.c->width), double(t.c->height), 0.0, 1.0};
                [enc setViewport:vp];
                const float size[2] = {float(t.c->width), float(t.c->height)};
                [enc setVertexBuffer:vb offset:0 atIndex:0];
                [enc setVertexBytes:size length:sizeof(size) atIndex:1];
                [enc setFragmentBuffer:pb offset:0 atIndex:0];
                [enc setFragmentBuffer:palBuf offset:0 atIndex:1];
                if (segments.empty())
                {
                    [enc setFragmentTexture:dummyTex atIndex:0];
                    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:verts.size()];
                }
                else
                {
                    for (size_t i = 0; i < segments.size(); ++i)
                    {
                        const uint32_t first = segments[i].first;
                        const uint32_t last = (i + 1 < segments.size()) ? segments[i + 1].first : uint32_t(verts.size());
                        if (last <= first)
                            continue;
                        [enc setFragmentTexture:segments[i].tex atIndex:0];
                        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:first vertexCount:last - first];
                    }
                }
                [enc endEncoding];
                if (hi && t.c->hiTex && t.z->hiTex && t.c->hiPad == t.z->hiPad)
                {
                    id<MTLBuffer> vbh = vb, xb = nil;
                    if (wsPad && vertsHi.size() == verts.size() && xfHi.size() == prims.size() * 4u)
                    {
                        vbh = GrabBuf(vertsHi.size() * sizeof(GPUVertex));
                        std::memcpy(vbh.contents, vertsHi.data(), vertsHi.size() * sizeof(GPUVertex));
                        xb = GrabBuf(xfHi.size() * sizeof(float));
                        std::memcpy(xb.contents, xfHi.data(), xfHi.size() * sizeof(float));
                    }
                    EncodeHiPass(cmd, t, vbh, pb, xb);
                }
                CommitCur();
                if (timing)
                    frame.encodeNs += nowNs() - te0;
            }
        }
        run = nullptr;
        snapTex = nil;
        snapTarget = nullptr;
        verts.clear();
        prims.clear();
        vertsHi.clear();
        xfHi.clear();
        segments.clear();
        curTex = nil;
        runPages.reset();
        runPrims = 0;
        if (draw)
        {
            if (inflightBytes > (size_t(256) << 20))
                WaitAll(kFlushLimit);
            else if (eager)
                ResolveAll(why);
        }
    }

    // G2e: the same run into the scaled planes (display only; nothing here reaches the shadow)
    void EncodeHiPass(id<MTLCommandBuffer> cmd, Target &t, id<MTLBuffer> vb, id<MTLBuffer> pb, id<MTLBuffer> xb)
    {
        const uint32_t padW = t.c->width + 2u * t.c->hiPad; // G3a: padded plane width (native px)
        if (snapTex && snapTarget == &t && snapHi)
        {
            id<MTLBlitCommandEncoder> sb = [cmd blitCommandEncoder];
            [sb copyFromTexture:t.c->hiTex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                     sourceSize:MTLSizeMake(t.c->hiTex.width, t.c->hiTex.height, 1) toTexture:snapHi destinationSlice:0 destinationLevel:0
              destinationOrigin:MTLOriginMake(0, 0, 0)];
            [sb endEncoding];
        }
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = t.c->hiTex;
        rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        rp.colorAttachments[1].texture = t.z->hiTex;
        rp.colorAttachments[1].loadAction = MTLLoadActionLoad;
        rp.colorAttachments[1].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];
        [enc setRenderPipelineState:hiPipeline];
        MTLViewport vp = {0.0, 0.0, double(padW * hsx), double(t.c->height * hsy), 0.0, 1.0};
        [enc setViewport:vp];
        const float size[2] = {float(padW), float(t.c->height)}; // native positions: the viewport scales them
        struct
        {
            float sx, sy;
            uint32_t isx, isy, dbg;
            uint32_t wide;
            float spad;
        } hp = {float(hsx), float(hsy), hsx, hsy, hiDbg, xb ? 1u : 0u, float(wsPad)};
        [enc setVertexBuffer:vb offset:0 atIndex:0];
        [enc setVertexBytes:size length:sizeof(size) atIndex:1];
        [enc setFragmentBuffer:pb offset:0 atIndex:0];
        [enc setFragmentBuffer:palBuf offset:0 atIndex:1];
        [enc setFragmentBytes:&hp length:sizeof(hp) atIndex:2];
        if (xb)
            [enc setFragmentBuffer:xb offset:0 atIndex:3];
        else
        {
            const float one[4] = {1.0f, 0.0f, 1.0f, 0.0f};
            [enc setFragmentBytes:one length:sizeof(one) atIndex:3];
        }
        if (segments.empty())
        {
            [enc setFragmentTexture:dummyTex atIndex:0];
            [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:verts.size()];
        }
        else
            for (size_t i = 0; i < segments.size(); ++i)
            {
                const uint32_t first = segments[i].first;
                const uint32_t last = (i + 1 < segments.size()) ? segments[i + 1].first : uint32_t(verts.size());
                if (last <= first)
                    continue;
                [enc setFragmentTexture:(segments[i].hiTex ? segments[i].hiTex : segments[i].tex) atIndex:0];
                [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:first vertexCount:last - first];
            }
        [enc endEncoding];
        ++hiPasses;
    }

    // G2e: the scaled display frame, from the plane the 1x present chose (its PCRTC decision is reused).
    // Falls back to a nearest upscale of the 1x frame when that plane is not available.
    void BuildHiFrame(const GSPresentationRequest &req, const PresentationFrame &f)
    {
        if (!f)
            return;
        ++hiPresents;
        const bool en1 = (req.pmode & 1u) != 0u, en2 = (req.pmode & 2u) != 0u;
        const bool field = (req.smode2 & 1u) != 0u && (req.smode2 & 2u) != 0u;
        if ((req.pmode != lastPmode || req.smode2 != lastSmode2) && modeLogs < 40u)
        {
            ++modeLogs;
            std::fprintf(stderr, "[gsmtl-hi] pmode=0x%llx smode2=0x%llx dispfb1=0x%llx dispfb2=0x%llx frame %ux%u src=%u disp=%u field=%d tick=%llu\n",
                         (unsigned long long)req.pmode, (unsigned long long)req.smode2, (unsigned long long)req.dispfb1, (unsigned long long)req.dispfb2,
                         f.width, f.height, f.sourceFbp, f.displayFbp, int(field), (unsigned long long)req.vsyncTick);
            std::fflush(stderr);
        }
        lastPmode = req.pmode;
        lastSmode2 = req.smode2;
        wsAnchor.EndFrame();
        uint32_t outW = f.width * hsx;
        const uint32_t outH = f.height * hsx;
        uint32_t framePad = 0; // G3a: the padded plane's pad when the presented plane is a widescreen one
        std::vector<uint8_t> px; // sized only on the CPU-copy paths (the SH1 direct present needs none)
        bool ok = false;
        const uint64_t dfb = (en1 && !en2) ? req.dispfb1 : ((!en1 && en2) ? req.dispfb2 : 0ull);
        const uint32_t dFbp = uint32_t(dfb & 0x1FFu), dFbw = uint32_t((dfb >> 9) & 0x3Fu), dPsm = uint32_t((dfb >> 15) & 0x1Fu);
        if (dfb != 0ull && (dPsm == GS_PSM_CT32 || dPsm == GS_PSM_CT24))
        {
            const bool same = f.sourceFbp == dFbp && !f.usedPreferred;
            Plane *P = nullptr;
            for (auto &q : planes)
                if (!q->depth && q->base == f.sourceFbp && q->hiTex && q->valid && (!same || q->fbw == dFbw))
                {
                    P = q.get();
                    break;
                }
            if (P)
            {
                if (P->hiPad)
                {
                    framePad = P->hiPad;
                    outW = (f.width + 2u * framePad) * hsx;
                }
                @autoreleasepool
                {
                    if (PlaneStale(*P) && !(run && (P == run->c) && !prims.empty()))
                        EncodeRefill(*P); // the shadow is newer (CPU-side writes): the scaled copy follows it
                    const uint32_t ox = (same ? uint32_t((dfb >> 32) & 0x7FFu) : 0u) * hsx;
                    const uint32_t oy = (same ? uint32_t((dfb >> 43) & 0x7FFu) : 0u) * hsy;
                    const uint32_t scan = field ? std::max<uint32_t>(1u, f.height / 2u) : f.height;
                    const uint32_t prm[6] = {ox, oy, outW, outH, scan * hsy, outH};
                    // PS2X_GS_DIRECT_VERIFY=1 (tests, any shell): also build the direct texture and compare it with the CPU copy.
                    static const bool s_directVerify = std::getenv("PS2X_GS_DIRECT_VERIFY") != nullptr;
                    id<MTLTexture> verifyTex = nil;
                    uint64_t verifyKey = 0;
                    if (g_directOn.load(std::memory_order_acquire) || s_directVerify)
                    {
                        // SH1 direct present: GPU only, no wait, no readback, no CPU frame.
                        const size_t bytes = size_t(outW) * outH * 4u;
                        if (!directBuf || directBuf.length < bytes)
                            directBuf = [device newBufferWithLength:bytes options:MTLResourceStorageModePrivate];
                        const uint32_t slot = directSlot++ % 3u;
                        id<MTLTexture> dt = directTex[slot];
                        if (!dt || dt.width != outW || dt.height != outH)
                        {
                            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                                          width:outW
                                                                                                         height:outH
                                                                                                      mipmapped:NO];
                            td.storageMode = MTLStorageModePrivate;
                            td.usage = MTLTextureUsageShaderRead;
                            dt = directTex[slot] = [device newTextureWithDescriptor:td];
                        }
                        id<MTLCommandBuffer> cmd = GetCmd();
                        id<MTLComputeCommandEncoder> ce = [cmd computeCommandEncoder];
                        [ce setComputePipelineState:hiPresent];
                        [ce setTexture:P->hiTex atIndex:0];
                        [ce setBuffer:directBuf offset:0 atIndex:0];
                        [ce setBytes:prm length:sizeof(prm) atIndex:1];
                        [ce dispatchThreads:MTLSizeMake(outW, outH, 1) threadsPerThreadgroup:MTLSizeMake(32, 8, 1)];
                        [ce endEncoding];
                        id<MTLBlitCommandEncoder> be = [cmd blitCommandEncoder];
                        [be copyFromBuffer:directBuf sourceOffset:0 sourceBytesPerRow:size_t(outW) * 4u sourceBytesPerImage:bytes
                                sourceSize:MTLSizeMake(outW, outH, 1) toTexture:dt destinationSlice:0 destinationLevel:0
                         destinationOrigin:MTLOriginMake(0, 0, 0)];
                        [be endEncoding];
                        CommitCur();
                        std::lock_guard<std::mutex> lk(g_directMu);
                        DirectHi &e = g_directRing[g_directSerial % 3u];
                        e.tex = dt;
                        e.w = outW;
                        e.h = outH;
                        e.key = GSMetalFrameKey(f.pixels.data(), f.width, f.height, 640u * 4u);
                        e.serial = ++g_directSerial;
                        if (!s_directVerify)
                            return;
                        verifyTex = dt;
                        verifyKey = e.key;
                    }
                    px.resize(size_t(outW) * outH * 4u);
                    if (!hiOut || hiOut.length < px.size())
                        hiOut = [device newBufferWithLength:px.size() options:MTLResourceStorageModeShared];
                    id<MTLCommandBuffer> cmd = GetCmd();
                    id<MTLComputeCommandEncoder> ce = [cmd computeCommandEncoder];
                    [ce setComputePipelineState:hiPresent];
                    [ce setTexture:P->hiTex atIndex:0];
                    [ce setBuffer:hiOut offset:0 atIndex:0];
                    [ce setBytes:prm length:sizeof(prm) atIndex:1];
                    [ce dispatchThreads:MTLSizeMake(outW, outH, 1) threadsPerThreadgroup:MTLSizeMake(32, 8, 1)];
                    [ce endEncoding];
                    CommitCur();
                    WaitAll(kFlushPresent);
                    std::memcpy(px.data(), hiOut.contents, px.size());
                    ok = true;
                    if (verifyTex)
                    {
                        static uint64_t s_vFrames = 0, s_vBad = 0, s_vLookupBad = 0;
                        id<MTLBuffer> vb = [device newBufferWithLength:px.size() options:MTLResourceStorageModeShared];
                        id<MTLCommandBuffer> vc = [queue commandBuffer];
                        id<MTLBlitCommandEncoder> vbe = [vc blitCommandEncoder];
                        [vbe copyFromTexture:verifyTex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(outW, outH, 1)
                                    toBuffer:vb destinationOffset:0 destinationBytesPerRow:size_t(outW) * 4u destinationBytesPerImage:px.size()];
                        [vbe endEncoding];
                        [vc commit];
                        [vc waitUntilCompleted];
                        ++s_vFrames;
                        if (std::memcmp(vb.contents, px.data(), px.size()) != 0)
                            ++s_vBad;
                        uint32_t lw = 0, lh = 0;
                        if (GSMetalDirectHiTexture(verifyKey, lw, lh) != verifyTex || lw != outW || lh != outH)
                            ++s_vLookupBad;
                        if (s_vFrames % 50u == 1u)
                        {
                            std::fprintf(stderr, "[gsmtl-direct] verify frames=%llu mismatched=%llu lookup_bad=%llu size=%ux%u\n", (unsigned long long)s_vFrames,
                                         (unsigned long long)s_vBad, (unsigned long long)s_vLookupBad, outW, outH);
                            std::fflush(stderr);
                        }
                    }
                }
            }
        }
        if (!ok)
        {
            ++hiFallbacks;
            if (wsPad && !framePad)
            {
                framePad = wsPad; // keep the window's widescreen frame size; the 4:3 picture is pillarboxed
                outW = (f.width + 2u * framePad) * hsx;
            }
            px.assign(size_t(outW) * outH * 4u, 0u);
            for (uint32_t y = 0; y < outH; ++y)
                for (uint32_t x = 0; x < outW; ++x)
                {
                    const int nx = int(x / hsx) - int(framePad);
                    if (nx >= 0 && nx < int(f.width))
                        std::memcpy(&px[(size_t(y) * outW + x) * 4u], &f.pixels[(size_t(y / hsx) * 640u + uint32_t(nx)) * 4u], 4);
                    else
                        px[(size_t(y) * outW + x) * 4u + 3u] = 0xFFu;
                }
        }
        g_wsFactor.store(f.width ? float(f.width + 2u * framePad) / float(f.width) : 1.0f, std::memory_order_release);
        GSMetalPublishHires(std::move(px), outW, outH, GSMetalFrameKey(f.pixels.data(), f.width, f.height, 640u * 4u));
    }

    // compatibility name used throughout
    void FlushRun(FlushWhy why = kFlushOther) { ResolveAll(why); }

    // CPU-side writes: mark pages the operation can touch.
    void MarkCpuDraw(const GSPrimitiveBatch &batch)
    {
        const GSContext &ctx = batch.state.context;
        PageRange r;
        const uint32_t sx1 = std::min<uint32_t>(ctx.scissor.x1, 2047u), sy1 = std::min<uint32_t>(ctx.scissor.y1, 2047u);
        pagesOfRect(ctx.frame.fbp << 5, ctx.frame.fbw, ctx.frame.psm, ctx.scissor.x0, ctx.scissor.y0, std::max<uint32_t>(sx1, ctx.scissor.x0),
                    std::max<uint32_t>(sy1, ctx.scissor.y0), r);
        MarkPages(r);
        pagesOfRect(ctx.zbuf.zbp << 5, ctx.frame.fbw, ctx.zbuf.psm, ctx.scissor.x0, ctx.scissor.y0, std::max<uint32_t>(sx1, ctx.scissor.x0),
                    std::max<uint32_t>(sy1, ctx.scissor.y0), r);
        MarkPages(r);
    }

    // ---------------------------------------------------------------- textured triangles
    // Fills `ts` for a textured triangle; returns a fallback reason or nullptr.
    const char *ParseTexture(const GSPrimitiveBatch &batch)
    {
        const GSDrawState &st = batch.state;
        const GSTex0Reg &tex = st.context.tex0;
        ts = TexSetup{};
        if (tex.psm != GS_PSM_T8 && tex.psm != GS_PSM_T4 && tex.psm != GS_PSM_CT32 && tex.psm != GS_PSM_CT24)
            return "tex_psm";
        ts.texW = st.textureWidth;
        ts.texH = st.textureHeight;
        if (ts.texW < 1u || ts.texW > 1024u || ts.texH < 1u || ts.texH > 1024u || (ts.texW & (ts.texW - 1u)) != 0u || (ts.texH & (ts.texH - 1u)) != 0u)
            return "tex_size";
        const uint64_t clamp = st.context.clamp;
        ts.wms = uint32_t(clamp & 3u);
        ts.wmt = uint32_t((clamp >> 2) & 3u);
        ts.minU = uint32_t((clamp >> 4) & 0x3FFu);
        ts.maxU = uint32_t((clamp >> 14) & 0x3FFu);
        ts.minV = uint32_t((clamp >> 24) & 0x3FFu);
        ts.maxV = uint32_t((clamp >> 34) & 0x3FFu);
        auto bound = [](uint32_t mode, uint32_t size, uint32_t rmin, uint32_t rmax) -> uint32_t
        {
            switch (mode & 3u)
            {
            case 0u:
            case 1u:
                return size - 1u;
            case 2u:
                return rmax;
            default:
                return rmin | rmax;
            }
        };
        ts.key.w = std::min<uint32_t>(bound(ts.wms, ts.texW, ts.minU, ts.maxU) + 1u, 1024u);
        ts.key.h = std::min<uint32_t>(bound(ts.wmt, ts.texH, ts.minV, ts.maxV) + 1u, 1024u);
        ts.key.tbp = tex.tbp0;
        ts.key.tbw = tex.tbw;
        ts.key.psm = tex.psm;
        ts.key.texa = (tex.psm == GS_PSM_CT24) ? (uint32_t(st.texa.ta0) | (st.texa.aem ? 0x100u : 0u) | 0x10000u) : 0u;
        ts.tfx = tex.tfx & 3u;
        ts.tcc = tex.tcc & 1u;
        ts.indexed = (tex.psm == GS_PSM_T8 || tex.psm == GS_PSM_T4);
        ts.fst = st.prim.fst;
        ts.linear = st.linearFilter;
        ts.on = true;
        return nullptr;
    }

    // VRAM pages the texels of a texture can come from (exact page arithmetic of GSMem::SwizzledSurface).
    static void TexturePages(const TexKey &k, std::vector<uint16_t> &out)
    {
        uint32_t pw = 64u, ph = 32u;
        if (k.psm == GS_PSM_T8)
        {
            pw = 128u;
            ph = 64u;
        }
        else if (k.psm == GS_PSM_T4)
        {
            pw = 128u;
            ph = 128u;
        }
        const uint32_t ppr = (k.tbw * 64u) / pw; // may be 0 (the oracle aliases the page rows then)
        const uint32_t base = k.tbp / 32u;
        const bool spill = (k.tbp % 32u) != 0u;
        const uint32_t rows = (k.h + ph - 1u) / ph, cols = (k.w + pw - 1u) / pw;
        std::bitset<kPages> set;
        for (uint32_t r = 0; r < rows; ++r)
            for (uint32_t c = 0; c < cols; ++c)
            {
                const uint32_t p = (base + r * ppr + c) % kPages;
                set.set(p);
                if (spill)
                    set.set((p + 1u) % kPages);
            }
        out.clear();
        for (uint32_t p = 0; p < kPages; ++p)
            if (set.test(p))
                out.push_back(uint16_t(p));
    }

    id<MTLTexture> DecodeTexture(const TexKey &k, size_t &bytesOut)
    {
        using namespace GSMem;
        const uint32_t W = k.w, H = k.h;
        const bool idx = (k.psm == GS_PSM_T8 || k.psm == GS_PSM_T4);
        const uint8_t *src = nullptr;
        if (k.psm == GS_PSM_T8)
        {
            scratch8.resize(size_t(W) * H);
            const SwizzledSurface<P8> sw(k.tbp, k.tbw);
            for (uint32_t y = 0; y < H; ++y)
                for (uint32_t x = 0; x < W; ++x)
                    scratch8[size_t(y) * W + x] = static_cast<uint8_t>(sw.Read(vram, x, y));
            src = scratch8.data();
        }
        else if (k.psm == GS_PSM_T4)
        {
            scratch8.resize(size_t(W) * H);
            const SwizzledSurface<P4> sw(k.tbp, k.tbw);
            for (uint32_t y = 0; y < H; ++y)
                for (uint32_t x = 0; x < W; ++x)
                    scratch8[size_t(y) * W + x] = static_cast<uint8_t>(sw.Read(vram, x, y));
            src = scratch8.data();
        }
        else if (k.psm == GS_PSM_CT32)
        {
            scratch32.resize(size_t(W) * H);
            const SwizzledSurface<C32> sw(k.tbp, k.tbw);
            for (uint32_t y = 0; y < H; ++y)
                for (uint32_t x = 0; x < W; ++x)
                    scratch32[size_t(y) * W + x] = sw.Read(vram, x, y);
            src = reinterpret_cast<const uint8_t *>(scratch32.data());
        }
        else // CT24 + TEXA (applyTexa of the oracle)
        {
            scratch32.resize(size_t(W) * H);
            const SwizzledSurface<C24> sw(k.tbp, k.tbw);
            const uint32_t ta0 = k.texa & 0xFFu;
            const bool aem = (k.texa & 0x100u) != 0u;
            for (uint32_t y = 0; y < H; ++y)
                for (uint32_t x = 0; x < W; ++x)
                {
                    const uint32_t texel = sw.Read(vram, x, y);
                    const bool rgbZero = (texel & 0x00FFFFFFu) == 0u;
                    const uint32_t a = (aem && rgbZero) ? 0u : ta0;
                    scratch32[size_t(y) * W + x] = (texel & 0x00FFFFFFu) | (a << 24);
                }
            src = reinterpret_cast<const uint8_t *>(scratch32.data());
        }
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:(idx ? MTLPixelFormatR8Uint : MTLPixelFormatRGBA8Uint)
                                                                                     width:W
                                                                                    height:H
                                                                                 mipmapped:NO];
        d.usage = MTLTextureUsageShaderRead;
        d.storageMode = MTLStorageModeShared;
        id<MTLTexture> tex = [device newTextureWithDescriptor:d];
        const size_t bpp = idx ? 1u : 4u;
        [tex replaceRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0 withBytes:src bytesPerRow:W * bpp];
        bytesOut = size_t(W) * H * bpp;
        frame.texTexels += uint64_t(W) * H;
        return tex;
    }

    void EvictTextures()
    {
        constexpr size_t kBudget = 384u << 20;
        if (texBytes <= kBudget)
            return;
        std::vector<std::pair<uint64_t, TexKey>> order;
        order.reserve(texCache.size());
        for (const auto &[k, e] : texCache)
            order.emplace_back(e.lastUse, k);
        std::sort(order.begin(), order.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
        for (const auto &[use, k] : order)
        {
            if (texBytes <= kBudget / 2)
                break;
            auto it = texCache.find(k);
            texBytes -= it->second.bytes;
            texCache.erase(it);
        }
    }

    // Finds, validates (page epochs) or (re)decodes the draw's texture, closing the open run first
    // when the texture's pages overlap the run's target. Leaves the result in primTex / curPalOff.
    void PrepareTexture(const GSPrimitiveBatch &batch, const GSContext &ctx)
    {
        const uint64_t t0 = timing ? nowNs() : 0;
        auto [it, inserted] = texCache.try_emplace(ts.key);
        TexEntry &e = it->second;
        if (inserted)
            TexturePages(ts.key, e.pages);
        e.lastUse = ++useTick;
        if (anyDirtyPlane)
        {
            // texels the GPU has rendered since the last write-back must reach the shadow before decoding
            bool hit = false;
            for (auto &q : planes)
            {
                if (!q->anyDirty())
                    continue;
                for (uint16_t p : e.pages)
                    if (q->dirty.test(p))
                    {
                        hit = true;
                        break;
                    }
                if (hit)
                    break;
            }
            if (hit)
            {
                std::bitset<kPages> b;
                for (uint16_t p : e.pages)
                    b.set(p);
                ResolvePlanes(&b, false, nullptr, nullptr, kFlushTexOverlap);
                SelectRun(ctx);
            }
        }
        bool fresh = !e.tex;
        if (!fresh && e.checked != epoch)
        {
            for (uint16_t p : e.pages)
                if (pageEpoch[p] > e.epoch)
                {
                    fresh = true;
                    break;
                }
            if (!fresh)
                e.checked = epoch;
        }
        if (fresh)
        {
            size_t bytes = 0;
            id<MTLTexture> tex = DecodeTexture(ts.key, bytes);
            texBytes -= e.bytes;
            e.tex = tex;
            e.bytes = bytes;
            texBytes += bytes;
            e.epoch = epoch;
            e.checked = epoch;
            ++frame.texDecodes;
            primTex = tex;
            EvictTextures();
        }
        else
        {
            ++frame.texHits;
            primTex = e.tex;
        }

        curPalOff = 0;
        if (ts.indexed)
        {
            uint64_t gen = 0, key = 0;
            const uint32_t *pal = cpu->PaletteFor(batch.state, gen, key);
            const uint32_t count = (ts.key.psm == GS_PSM_T4) ? 16u : 256u;
            if (lastPalValid && gen == lastPalGen && key == lastPalKey)
            {
                curPalOff = lastPalOff;
                ++frame.paletteHits;
            }
            else
            {
                uint64_t h = 0xCBF29CE484222325ull ^ count;
                for (uint32_t i = 0; i < count; ++i)
                {
                    h ^= pal[i];
                    h *= 0x100000001B3ull;
                    h ^= h >> 29;
                }
                uint32_t slot = UINT32_MAX;
                auto pit = palMap.find(h);
                if (pit != palMap.end() && std::memcmp(static_cast<uint32_t *>(palBuf.contents) + size_t(pit->second) * 256u, pal, count * 4u) == 0)
                {
                    slot = pit->second;
                    ++frame.paletteHits;
                }
                else
                {
                    if (palUsed >= kPalSlots)
                    {
                        CloseRun(kFlushPalette);
                        CommitCur();
                        WaitAll(kFlushPalette);
                        SelectRun(ctx);
                    }
                    slot = palUsed++;
                    std::memcpy(static_cast<uint32_t *>(palBuf.contents) + size_t(slot) * 256u, pal, count * 4u);
                    palMap[h] = slot;
                    ++frame.palettes;
                }
                curPalOff = slot * 256u;
                lastPalValid = true;
                lastPalGen = gen;
                lastPalKey = key;
                lastPalOff = curPalOff;
            }
        }
        if (timing)
            frame.texNs += nowNs() - t0;
    }

    // Start (or continue) the draw segment that binds `tex` for the primitives recorded next.
    void BindSegment(id<MTLTexture> tex, id<MTLTexture> hiTex = nil)
    {
        if (!tex)
        {
            if (segments.empty())
            {
                segments.push_back({0u, dummyTex});
                curTex = dummyTex;
            }
            return;
        }
        if (segments.empty() || curTex != tex)
        {
            const uint32_t first = uint32_t(verts.size());
            if (!segments.empty() && segments.back().first == first)
            {
                segments.back().tex = tex;
                segments.back().hiTex = hiTex;
            }
            else
                segments.push_back({first, tex, hiTex});
            curTex = tex;
        }
    }

    // ---------------------------------------------------------------- textured sprites (G2b)
    struct SpriteGeom
    {
        bool ok = false;
        int un0x = 0, un0y = 0; // unclipped top-left (Raster::Sprite's unclippedX0/Y0)
        float w = 1.0f, h = 1.0f;
        float u0f = 0.0f, v0f = 0.0f, u1f = 0.0f, v1f = 0.0f;
        int gx0 = 0, gy0 = 0, gx1 = -1, gy1 = -1; // drawn (scissored) rectangle, inclusive
    };
    SpriteGeom sg;

    static float FabsQ(float q) { return (std::fabs(q) > 1.0e-8f) ? q : 1.0f; }

    // Raster::Sprite's setup, the oracle's own expressions (run under the draw's FPCR).
    void ComputeSpriteGeom(const GSPrimitiveBatch &batch)
    {
        using GSInternal::clampInt;
        const GSContext &ctx = batch.state.context;
        const GSVertex v0 = batch.vertices[0];
        const GSVertex v1 = batch.vertices[1];
        const int ofx = ctx.xyoffset.ofx >> 4, ofy = ctx.xyoffset.ofy >> 4;
        int x0 = static_cast<int>(v0.x) - ofx, y0 = static_cast<int>(v0.y) - ofy;
        int x1 = static_cast<int>(v1.x) - ofx, y1 = static_cast<int>(v1.y) - ofy;
        if (x0 > x1)
            std::swap(x0, x1);
        if (y0 > y1)
            std::swap(y0, y1);
        const int spanX = std::max(1, x1 - x0), spanY = std::max(1, y1 - y0);
        const int ux1 = x0 + spanX - 1, uy1 = y0 + spanY - 1;
        sg = SpriteGeom{};
        if (ux1 < ctx.scissor.x0 || x0 > ctx.scissor.x1 || uy1 < ctx.scissor.y0 || y0 > ctx.scissor.y1)
            return;
        sg.un0x = x0;
        sg.un0y = y0;
        sg.gx0 = clampInt(x0, ctx.scissor.x0, ctx.scissor.x1);
        sg.gy0 = clampInt(y0, ctx.scissor.y0, ctx.scissor.y1);
        sg.gx1 = clampInt(ux1, ctx.scissor.x0, ctx.scissor.x1);
        sg.gy1 = clampInt(uy1, ctx.scissor.y0, ctx.scissor.y1);
        sg.w = static_cast<float>(spanX);
        sg.h = static_cast<float>(spanY);
        if (ts.fst)
        {
            sg.u0f = static_cast<float>(v0.u >> 4);
            sg.v0f = static_cast<float>(v0.v >> 4);
            sg.u1f = static_cast<float>(v1.u >> 4);
            sg.v1f = static_cast<float>(v1.v >> 4);
        }
        else
        {
            const float q0 = FabsQ(v0.q);
            const float q1 = FabsQ(v1.q);
            sg.u0f = (v0.s / q0) * static_cast<float>(ts.texW);
            sg.v0f = (v0.t / q0) * static_cast<float>(ts.texH);
            sg.u1f = (v1.s / q1) * static_cast<float>(ts.texW);
            sg.v1f = (v1.t / q1) * static_cast<float>(ts.texH);
        }
        sg.ok = true;
    }

    // The texel-space coordinate of the sprite pixel `pix` on one axis (Raster::Sprite::columnAxis / row).
    static float SpriteCoord(int pix, int un0, float size, float a0, float a1, bool fst)
    {
        const float t = (static_cast<float>(pix - un0) + 0.5f) / size;
        const float tex = a0 + (a1 - a0) * t;
        if (fst)
        {
            const int fixed = static_cast<int>((tex * 16.0f) + 0.5f);
            return static_cast<float>(static_cast<uint16_t>(GSInternal::clampInt(fixed, 0, 0xFFFF))) / 16.0f;
        }
        return tex;
    }

    static int WrapIdx(int c, int size, uint32_t mode, uint32_t rmin, uint32_t rmax)
    {
        switch (mode & 3u)
        {
        case 0u: return int(uint32_t(c) & uint32_t(size - 1));
        case 1u: return c < 0 ? 0 : (c > size - 1 ? size - 1 : c);
        case 2u: return std::min(std::max(c, int(rmin)), int(rmax));
        default: return int((uint32_t(c) & rmin) | rmax);
        }
    }

    // The texels one axis of a sprite can read. The coordinate is monotone along the axis, so the
    // first and last drawn pixel bound every texel (REPEAT is only handled while no wrapping occurs,
    // REGION_REPEAT not at all; CLAMP and REGION_CLAMP are monotone). lo/hi are the wrapped extremes
    // including the bilinear neighbour, hi0 those of the nearest texel; fracHi is the bilinear weight of
    // the neighbour of the largest-coordinate column.
    struct AxisRange
    {
        bool ok = false;
        int lo = 0, hi = 0, hi0 = 0, wa = 0, wb = 0;
        bool ident = false;
        float fracHi = 0.0f;
        int hiEff = 0; // hi without a neighbour read whose bilinear weight is exactly zero
    };
    static AxisRange SpriteAxis(float cA, float cB, bool linear, int size, uint32_t mode, uint32_t rmin, uint32_t rmax)
    {
        AxisRange r;
        const auto c0 = [&](float c) { return linear ? static_cast<int>(std::floor(c - 0.5f)) : static_cast<int>(c); };
        const int a = c0(cA), b = c0(cB);
        if ((mode & 3u) == 3u)
            return r;
        if ((mode & 3u) == 0u && (std::min(a, b) < 0 || std::max(a, b) + (linear ? 1 : 0) > size - 1))
            return r;
        r.wa = WrapIdx(a, size, mode, rmin, rmax);
        r.wb = WrapIdx(b, size, mode, rmin, rmax);
        r.lo = std::min(r.wa, r.wb);
        r.hi0 = r.hi = std::max(r.wa, r.wb);
        if (linear)
        {
            const int na = WrapIdx(a + 1, size, mode, rmin, rmax), nb = WrapIdx(b + 1, size, mode, rmin, rmax);
            r.lo = std::min(r.lo, std::min(na, nb));
            r.hi = std::max(r.hi, std::max(na, nb));
            r.fracHi = (std::max(cA, cB) - 0.5f) - static_cast<float>(std::max(a, b));
        }
        r.ident = (a == r.wa && b == r.wb);
        r.hiEff = (linear && r.hi > r.hi0 && r.fracHi == 0.0f) ? r.hi0 : r.hi; // weight exactly 0: fma(x, 0, f00) == f00 whatever the neighbour holds
        r.ok = true;
        return r;
    }

    // Exactness proof for sampling a target texture directly: every texel read lies inside the target;
    // the one exception is the +1 bilinear neighbour just past the target edge when its weight is
    // exactly zero (always the case for FST coordinates, which are multiples of 1/16), so its value
    // cannot matter. A feedback sprite (source == destination) must in
    // addition be a 1:1 point-sampled copy, so each pixel is read before it is written.
    static bool AxisFits(const AxisRange &r, bool linear, int tdim, int pixA, int pixB, bool self)
    {
        if (!r.ok || r.lo < 0 || r.hi0 >= tdim)
            return false;
        if (self && (linear || !r.ident || r.wa != pixA || r.wb != pixB))
            return false;
        if (r.hi < tdim)
            return true;
        return linear && r.fracHi == 0.0f;
    }

    // Samples a framebuffer target's own MTLTexture (the full-screen CT24/CT32 "previous frame" sprite)
    // instead of decoding the shadow. Leaves the texture in primTex / fbSrc; false = use the normal path.
    bool TryFbTexture(const GSContext &ctx)
    {
        static const bool dbg = std::getenv("PS2X_GS_METAL_DBG") != nullptr;
        fbSrc = false;
        const auto reject = [&](const char *why)
        {
            if (dbg)
            {
                const uint64_t n = ++frame.fallbacks[std::string("dbg_fb_") + why];
                if (n <= 3)
                {
                    std::fprintf(stderr, "[gsmtl-dbg] fb reject %s: tbp=%u tbw=%u psm=%u tex=%ux%u wms=%u wmt=%u lin=%d fst=%d | run fbp=%u zbp=%u fbw=%u | sprite %d,%d..%d,%d u=%.3f..%.3f v=%.3f..%.3f | targets:",
                                 why, ts.key.tbp, ts.key.tbw, ts.key.psm, ts.texW, ts.texH, ts.wms, ts.wmt, ts.linear, ts.fst, run ? run->fbp : 0u, run ? run->zbp : 0u,
                                 run ? run->fbw : 0u, sg.gx0, sg.gy0, sg.gx1, sg.gy1, sg.u0f, sg.u1f, sg.v0f, sg.v1f);
                    for (auto &q : planes)
                        std::fprintf(stderr, " (%s base=%u fbw=%u h=%u valid=%d stale=%d)", q->depth ? "z" : "c", q->base, q->fbw, q->height, q->valid, PlaneStale(*q));
                    std::fprintf(stderr, "\n");
                    std::fflush(stderr);
                }
            }
            return false;
        };
        if (!sg.ok || (ts.key.psm != GS_PSM_CT32 && ts.key.psm != GS_PSM_CT24) || ts.wms == 3u || ts.wmt == 3u || !run)
            return reject("psm_wrap");
        Plane *T = nullptr;
        bool self = false;
        if ((run->fbp << 5) == ts.key.tbp && run->fbw == ts.key.tbw)
        {
            T = run->c;
            self = true;
        }
        else
        {
            for (auto &q : planes)
                if (q.get() != run->c && !q->depth && q->valid && (q->base << 5) == ts.key.tbp && q->fbw == ts.key.tbw && !PlaneStale(*q))
                {
                    T = q.get();
                    break;
                }
            if (!T)
                return reject("no_target");
            if ((T->span & runPages).any())
                return reject("run_overlap");
            // pixels another plane has drawn on these pages are not in this texture
            for (auto &q : planes)
                if (q.get() != T && q->anyDirty() && (q->dirty & T->span).any())
                    return reject("dirty_other");
        }
        const bool fst = ts.fst;
        const float cu0 = SpriteCoord(sg.gx0, sg.un0x, sg.w, sg.u0f, sg.u1f, fst), cu1 = SpriteCoord(sg.gx1, sg.un0x, sg.w, sg.u0f, sg.u1f, fst);
        const float cv0 = SpriteCoord(sg.gy0, sg.un0y, sg.h, sg.v0f, sg.v1f, fst), cv1 = SpriteCoord(sg.gy1, sg.un0y, sg.h, sg.v0f, sg.v1f, fst);
        const AxisRange ru = SpriteAxis(cu0, cu1, ts.linear, int(ts.texW), ts.wms, ts.minU, ts.maxU);
        const AxisRange rv = SpriteAxis(cv0, cv1, ts.linear, int(ts.texH), ts.wmt, ts.minV, ts.maxV);
        if (!AxisFits(ru, ts.linear, int(T->width), sg.gx0, sg.gx1, self) || !AxisFits(rv, ts.linear, int(T->height), sg.gy0, sg.gy1, self))
            return reject(self ? "proof_self" : "proof");
        if (self)
        {
            if (!prims.empty())
            {
                CloseRun(kFlushTexOverlap);
                SelectRun(ctx);
                T = run->c;
            }
            if (!snapTex || snapTex.width != T->width || snapTex.height != T->height)
            {
                MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Uint
                                                                                             width:T->width
                                                                                            height:T->height
                                                                                         mipmapped:NO];
                d.usage = MTLTextureUsageShaderRead;
                d.storageMode = MTLStorageModePrivate;
                snapTex = [device newTextureWithDescriptor:d];
            }
            snapTarget = run;
            primTex = snapTex;
            if (hi && T->hiTex)
            {
                if (!snapHi || snapHi.width != T->hiTex.width || snapHi.height != T->hiTex.height)
                {
                    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Uint
                                                                                                 width:T->hiTex.width
                                                                                                height:T->hiTex.height
                                                                                             mipmapped:NO];
                    d.usage = MTLTextureUsageShaderRead;
                    d.storageMode = MTLStorageModePrivate;
                    snapHi = [device newTextureWithDescriptor:d];
                }
                primTexHi = snapHi;
            }
            ++frame.metalSelfPrims;
        }
        else
        {
            primTex = T->tex;
            primTexHi = T->hiTex;
            ++frame.metalFbPrims;
        }
        fbSrc = true;
        fbSrcPadded = T->hiPad != 0u;
        fbCt24 = ts.key.psm == GS_PSM_CT24;
        curPalOff = 0;
        return true;
    }

    // Do the texture's pages overlap the run target (a possible feedback read of pixels this draw writes)?
    bool TextureOverlapsRun() const
    {
        // The texels the sprite can read lie between those of its first and last pixel (monotone axes).
        const bool fst = ts.fst;
        const AxisRange ru = SpriteAxis(SpriteCoord(sg.gx0, sg.un0x, sg.w, sg.u0f, sg.u1f, fst), SpriteCoord(sg.gx1, sg.un0x, sg.w, sg.u0f, sg.u1f, fst),
                                        ts.linear, int(ts.texW), ts.wms, ts.minU, ts.maxU);
        const AxisRange rv = SpriteAxis(SpriteCoord(sg.gy0, sg.un0y, sg.h, sg.v0f, sg.v1f, fst), SpriteCoord(sg.gy1, sg.un0y, sg.h, sg.v0f, sg.v1f, fst),
                                        ts.linear, int(ts.texH), ts.wmt, ts.minV, ts.maxV);
        if (ru.ok && rv.ok && ru.hiEff < 2048 && rv.hiEff < 2048)
        {
            PageRange r;
            pagesOfRect(ts.key.tbp, ts.key.tbw, uint8_t(ts.key.psm), uint32_t(ru.lo), uint32_t(rv.lo), uint32_t(ru.hiEff), uint32_t(rv.hiEff), r);
            if (r.count < kPages)
            {
                for (uint32_t i = 0; i < r.count; ++i)
                    if (runPages.test(r.pages[i]))
                    {
                        return true;
                    }
                return false;
            }
        }
        std::vector<uint16_t> pages;
        TexturePages(ts.key, pages);
        for (uint16_t p : pages)
            if (runPages.test(p))
                return true;
        return false;
    }

    void FillTex(GPUPrim &p, const GSDrawState &state) const
    {
        p.tflags = 1u | (ts.fst ? 2u : 0u) | (ts.linear ? 4u : 0u) | (ts.indexed ? 8u : 0u) | (ts.tfx << 4) | (ts.tcc << 6) | (ts.wms << 8) | (ts.wmt << 10) |
                   (fbSrc ? (4096u | (fbCt24 ? 8192u : 0u) | (fbSrcPadded ? 16384u : 0u)) : 0u);
        p.texdim = ts.texW | (ts.texH << 16);
        p.regU = ts.minU | (ts.maxU << 16);
        p.regV = ts.minV | (ts.maxV << 16);
        p.palOff = curPalOff;
        p.texa = uint32_t(state.texa.ta0) | (state.texa.aem ? 0x100u : 0u);
        p.fbw = ts.key.tbw;
    }

    const char *Eligible(const GSPrimitiveBatch &batch)
    {
        const GSDrawState &st = batch.state;
        const GSContext &ctx = st.context;
        ts.on = false;
        const bool tri = st.prim.type == GS_PRIM_TRIANGLE || st.prim.type == GS_PRIM_TRISTRIP || st.prim.type == GS_PRIM_TRIFAN;
        const bool sprite = st.prim.type == GS_PRIM_SPRITE;
        if (!tri && !sprite)
            return (st.prim.type == GS_PRIM_POINT) ? "point" : "line";
        if (ctx.frame.psm != GS_PSM_CT32)
            return "frame_psm";
        if (ctx.zbuf.psm != GS_PSM_Z24)
            return "zbuf_psm";
        if (ctx.frame.fbw == 0u || ctx.frame.fbw > 32u)
            return "fbw";
        if (ctx.scissor.x1 >= ctx.frame.fbw * 64u || ctx.scissor.y1 >= 2048u || ctx.scissor.x0 > ctx.scissor.x1 || ctx.scissor.y0 > ctx.scissor.y1)
            return "scissor";
        // frame and Z page spans must not overlap and must fit in VRAM
        const uint32_t pagesHigh = (uint32_t(ctx.scissor.y1) + 32u) / 32u;
        const uint32_t span = ctx.frame.fbw * pagesHigh;
        const uint32_t f0 = ctx.frame.fbp, f1 = f0 + span, z0 = ctx.zbuf.zbp, z1 = z0 + span;
        if (f1 > kPages || z1 > kPages)
            return "vram_range";
        if (f0 < z1 && z0 < f1)
            return "fz_overlap";
        const uint32_t vc = sprite ? 2u : 3u;
        // Triangle Z is interpolated in float (it must be exact); a sprite's Z is the integer of its second vertex.
        for (uint32_t i = 0; i < (sprite ? 0u : vc); ++i)
        {
            const double z = batch.vertices[i].z;
            if (static_cast<double>(static_cast<float>(z)) != z)
                return "z_not_float";
        }
        if (st.prim.tme)
            return ParseTexture(batch);
        return nullptr;
    }

    void Record(const GSPrimitiveBatch &batch, bool rtz)
    {
        using GSInternal::clampInt;
        const GSDrawState &state = batch.state;
        const GSContext &ctx = state.context;
        GPUPrim p{};
        p.flags = (rtz ? 4u : 0u) | (state.prim.fge ? 8u : 0u) | (state.prim.abe ? 16u : 0u) | (state.pabe ? 32u : 0u) |
                  ((ctx.fba & 1ull) ? 64u : 0u) | (ctx.zbuf.zmask ? 128u : 0u);
        static const bool s_fault = std::getenv("PS2X_GS_METAL_FAULT") != nullptr;
        if (s_fault)
            p.flags |= 256u;
        p.test = static_cast<uint32_t>(ctx.test);
        p.alpha = static_cast<uint32_t>(ctx.alpha & 0xFFu) | (static_cast<uint32_t>((ctx.alpha >> 32) & 0xFFu) << 8);
        p.fbmsk = ctx.frame.fbmsk;
        p.fogcol = uint32_t(state.fogR) | (uint32_t(state.fogG) << 8) | (uint32_t(state.fogB) << 16);

        int x0, y0, x1, y1; // inclusive pixel rectangle to cover
        int rawMinX = 0, rawMaxX = -1; // G3a: triangle x bounds before the scissor clamp
        const int ofx = ctx.xyoffset.ofx >> 4;
        const int ofy = ctx.xyoffset.ofy >> 4;
        if (state.prim.type == GS_PRIM_SPRITE)
        {
            // Raster::Sprite, untextured
            const GSVertex v0 = batch.vertices[0];
            const GSVertex v1 = batch.vertices[1];
            int sx0 = static_cast<int>(v0.x) - ofx;
            int sy0 = static_cast<int>(v0.y) - ofy;
            int sx1 = static_cast<int>(v1.x) - ofx;
            int sy1 = static_cast<int>(v1.y) - ofy;
            const uint32_t z1 = static_cast<uint32_t>(v1.z);
            if (sx0 > sx1)
                std::swap(sx0, sx1);
            if (sy0 > sy1)
                std::swap(sy0, sy1);
            const int spanX = std::max(1, sx1 - sx0);
            const int spanY = std::max(1, sy1 - sy0);
            const int ux1 = sx0 + spanX - 1;
            const int uy1 = sy0 + spanY - 1;
            if (ux1 < ctx.scissor.x0 || sx0 > ctx.scissor.x1 || uy1 < ctx.scissor.y0 || sy0 > ctx.scissor.y1)
                return;
            x0 = clampInt(sx0, ctx.scissor.x0, ctx.scissor.x1);
            y0 = clampInt(sy0, ctx.scissor.y0, ctx.scissor.y1);
            x1 = clampInt(ux1, ctx.scissor.x0, ctx.scissor.x1);
            y1 = clampInt(uy1, ctx.scissor.y0, ctx.scissor.y1);
            p.flags |= 1u;
            p.rgba0 = uint32_t(v1.r) | (uint32_t(v1.g) << 8) | (uint32_t(v1.b) << 16) | (uint32_t(v1.a) << 24);
            p.fog = v1.fog;
            p.spriteZ = z1;
            if (ts.on)
            {
                p.s0 = sg.u0f;
                p.t0 = sg.v0f;
                p.s1 = sg.u1f;
                p.t1 = sg.v1f;
                p.q0 = sg.w;
                p.q1 = sg.h;
                p.uv0 = uint32_t(sg.un0x);
                p.uv1 = uint32_t(sg.un0y);
                if (hi)
                {
                    // drawn rectangle for the scaled pass (unused by the 1x pass)
                    p.uv2 = uint32_t(x0) | (uint32_t(x1) << 16);
                    const uint32_t gy = uint32_t(y0) | (uint32_t(y1) << 16);
                    std::memcpy(&p.q2, &gy, 4);
                }
                FillTex(p, state);
            }
        }
        else
        {
            // drawBatch triangle setup, verbatim
            const GSVertex &v0 = batch.vertices[0];
            const GSVertex &v1 = batch.vertices[1];
            const GSVertex &v2 = batch.vertices[2];
            float fx0 = v0.x - static_cast<float>(ofx);
            float fy0 = v0.y - static_cast<float>(ofy);
            float fx1 = v1.x - static_cast<float>(ofx);
            float fy1 = v1.y - static_cast<float>(ofy);
            float fx2 = v2.x - static_cast<float>(ofx);
            float fy2 = v2.y - static_cast<float>(ofy);
            int minX = static_cast<int>(std::floor(std::min({fx0, fx1, fx2})));
            int maxX = static_cast<int>(std::ceil(std::max({fx0, fx1, fx2})));
            int minY = static_cast<int>(std::floor(std::min({fy0, fy1, fy2})));
            int maxY = static_cast<int>(std::ceil(std::max({fy0, fy1, fy2})));
            rawMinX = minX;
            rawMaxX = maxX;
            minX = clampInt(minX, ctx.scissor.x0, ctx.scissor.x1);
            maxX = clampInt(maxX, ctx.scissor.x0, ctx.scissor.x1);
            minY = clampInt(minY, ctx.scissor.y0, ctx.scissor.y1);
            maxY = clampInt(maxY, ctx.scissor.y0, ctx.scissor.y1);
            float denom = (fy1 - fy2) * (fx0 - fx2) + (fx2 - fx1) * (fy0 - fy2);
            if (std::fabs(denom) < 0.001f)
                return;
            const float winding = (denom < 0.0f) ? -1.0f : 1.0f;
            const float invAbsDenom = 1.0f / std::fabs(denom);
            p.fx2 = fx2;
            p.fy2 = fy2;
            p.a0 = fy1 - fy2;
            p.b0 = fx2 - fx1;
            p.a1 = fy2 - fy0;
            p.b1 = fx0 - fx2;
            p.winding = winding;
            p.invAbsDenom = invAbsDenom;
            p.z0 = static_cast<float>(v0.z);
            p.z1 = static_cast<float>(v1.z);
            p.z2 = static_cast<float>(v2.z);
            p.rgba0 = uint32_t(v0.r) | (uint32_t(v0.g) << 8) | (uint32_t(v0.b) << 16) | (uint32_t(v0.a) << 24);
            p.rgba1 = uint32_t(v1.r) | (uint32_t(v1.g) << 8) | (uint32_t(v1.b) << 16) | (uint32_t(v1.a) << 24);
            p.rgba2 = uint32_t(v2.r) | (uint32_t(v2.g) << 8) | (uint32_t(v2.b) << 16) | (uint32_t(v2.a) << 24);
            p.fog = uint32_t(v0.fog) | (uint32_t(v1.fog) << 8) | (uint32_t(v2.fog) << 16);
            if (state.prim.iip)
                p.flags |= 2u;
            if (ts.on)
            {
                p.s0 = v0.s;
                p.t0 = v0.t;
                p.q0 = v0.q;
                p.s1 = v1.s;
                p.t1 = v1.t;
                p.q1 = v1.q;
                p.s2 = v2.s;
                p.t2 = v2.t;
                p.q2 = v2.q;
                p.uv0 = uint32_t(v0.u) | (uint32_t(v0.v) << 16);
                p.uv1 = uint32_t(v1.u) | (uint32_t(v1.v) << 16);
                p.uv2 = uint32_t(v2.u) | (uint32_t(v2.v) << 16);
                FillTex(p, state);
            }
            x0 = minX;
            x1 = maxX;
            y0 = minY;
            y1 = maxY;
            // MX1 debug probe: PS2X_GS_HI_PROBE="x,y" logs triangles whose bounds touch 1x pixel (x..x+1, y)
            static const char *s_probe = std::getenv("PS2X_GS_HI_PROBE");
            static int s_px = s_probe ? std::atoi(s_probe) : -1, s_py = s_probe && std::strchr(s_probe, ',') ? std::atoi(std::strchr(s_probe, ',') + 1) : -1;
            static long s_probeLines = 0;
            if (s_probe && s_probeLines < 20000 && std::min({fx0, fx1, fx2}) <= float(s_px + 2) && std::max({fx0, fx1, fx2}) >= float(s_px - 1) &&
                std::min({fy0, fy1, fy2}) <= float(s_py + 1) && std::max({fy0, fy1, fy2}) >= float(s_py))
            {
                ++s_probeLines;
                std::fprintf(stderr, "[probe] fbp=%u tme=%d tflags=%#x texdim=%ux%u iip=%d xy=(%.4f,%.4f)(%.4f,%.4f)(%.4f,%.4f) uv=(%u,%u)(%u,%u)(%u,%u) stq=(%g,%g,%g)(%g,%g,%g)(%g,%g,%g) rgba=%08x,%08x,%08x\n",
                             ctx.frame.fbp, int(ts.on), p.tflags, p.texdim & 0xFFFFu, p.texdim >> 16, int(state.prim.iip), fx0, fy0, fx1, fy1, fx2, fy2,
                             p.uv0 & 0xFFFFu, p.uv0 >> 16, p.uv1 & 0xFFFFu, p.uv1 >> 16, p.uv2 & 0xFFFFu, p.uv2 >> 16,
                             p.s0, p.t0, p.q0, p.s1, p.t1, p.q1, p.s2, p.t2, p.q2, p.rgba0, p.rgba1, p.rgba2);
                std::fflush(stderr);
            }
        }
        if (x1 < x0 || y1 < y0)
            return;
        const uint32_t index = static_cast<uint32_t>(prims.size());
        prims.push_back(p);
        const float fx0 = float(x0), fy0 = float(y0), fx1 = float(x1 + 1), fy1 = float(y1 + 1);
        verts.push_back({fx0, fy0, index});
        verts.push_back({fx1, fy0, index});
        verts.push_back({fx0, fy1, index});
        verts.push_back({fx1, fy0, index});
        verts.push_back({fx1, fy1, index});
        verts.push_back({fx0, fy1, index});
        if (wsPad)
        {
            // G3a: the same prim in the padded widescreen plane. 3D (PATH1) keeps its geometry, centred, and is no
            // longer clipped at the 4:3 scissor; 2D is anchored left/centre/right at 4:3 size; full-width 2D stretches.
            float kx = 1.0f, ox = 0.0f, hx0 = fx0, hx1 = fx1;
            const uint32_t pad = run->c->hiPad;
            if (pad)
            {
                const int W = int(run->c->width);
                // PATH1 without a depth test (ZTE off or ZTST ALWAYS) is a 3D overlay (the minimap): anchored like 2D
                const uint64_t ztest = (ctx.test >> 16) & 7u; // ZTE | ZTST << 1
                const bool zt = (ztest & 1u) != 0u && (ztest >> 1) != 1u;
                const int oc = (state.gifPath == 1u && !zt) ? wsAnchor.Classify(state.gifPacket, x0, x1, y0, y1, W, true) : 0;
                if (state.gifPath == 1u && oc == 0)
                {
                    ox = float(pad);
                    ++wsCls[0];
                    if ((p.flags & 1u) == 0u && ctx.scissor.x0 == 0u && int(ctx.scissor.x1) == W - 1)
                    {
                        hx0 = float(clampInt(rawMinX, -int(pad), W - 1 + int(pad)));
                        hx1 = float(clampInt(rawMaxX, -int(pad), W - 1 + int(pad)) + 1);
                    }
                }
                else
                {
                    const int c = state.gifPath == 1u ? oc : wsAnchor.Classify(state.gifPacket, x0, x1, y0, y1, W, false);
                    ++wsCls[c];
                    if (c == 4)
                        kx = float(W + 2 * int(pad)) / float(W);
                    else
                        ox = float(pad) * float(c - 1);
                }
            }
            const float a0 = kx * hx0 + ox, a1 = kx * hx1 + ox;
            vertsHi.push_back({a0, fy0, index});
            vertsHi.push_back({a1, fy0, index});
            vertsHi.push_back({a0, fy1, index});
            vertsHi.push_back({a1, fy0, index});
            vertsHi.push_back({a1, fy1, index});
            vertsHi.push_back({a0, fy1, index});
            xfHi.push_back(kx);
            xfHi.push_back(ox);
            // padded framebuffer source: identity copies follow the prim's own mapping; an unpadded target reads
            // the source's native area (offset by its pad)
            const bool srcPad = ts.on && fbSrc && fbSrcPadded;
            xfHi.push_back(pad ? kx : 1.0f);
            xfHi.push_back(pad ? ox : (srcPad ? float(wsPad) : 0.0f));
        }
        MarkDirty(*run->c, x0, y0, x1, y1);
        if (!ctx.zbuf.zmask)
            MarkDirty(*run->z, x0, y0, x1, y1);
    }
};

GSMetalBackend::GSMetalBackend() : m(std::make_unique<Impl>()) {}

GSMetalBackend::~GSMetalBackend()
{
    if (m)
    {
        m->Drain(kFlushOther);
        Stats t = m->total;
        t.Add(m->frame);
        GSMetalPrintStats("total", t);
        std::fprintf(stderr, "[gsmtl-hi] total scale=%ux%u passes=%llu presents=%llu fallbacks=%llu upscale_px=%llu gpu_busy_s=%.3f\n", m->hsx, m->hsy,
                     (unsigned long long)m->hiPasses, (unsigned long long)m->hiPresents, (unsigned long long)m->hiFallbacks,
                     (unsigned long long)m->hiUpscalePx, m->gpuBusyS);
        if (m->wsPad)
            std::fprintf(stderr, "[gsmtl-ws] total pad=%u prims 3d=%llu left=%llu centre=%llu right=%llu stretch=%llu\n", m->wsPad,
                         (unsigned long long)m->wsCls[0], (unsigned long long)m->wsCls[1], (unsigned long long)m->wsCls[2],
                         (unsigned long long)m->wsCls[3], (unsigned long long)m->wsCls[4]);
        std::fflush(stderr);
    }
}

std::unique_ptr<GSMetalBackend> GSMetalBackend::Create()
{
    @autoreleasepool
    {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device)
        {
            std::fprintf(stderr, "[gsmtl] no Metal device\n");
            return nullptr;
        }
        if (![device supportsFamily:MTLGPUFamilyApple1])
        {
            std::fprintf(stderr, "[gsmtl] device lacks framebuffer fetch (Apple GPU family)\n");
            return nullptr;
        }
        MTLCompileOptions *opts = [MTLCompileOptions new];
        if (@available(macOS 15.0, *))
            opts.mathMode = MTLMathModeSafe;
        else
        {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            opts.fastMathEnabled = NO;
#pragma clang diagnostic pop
        }
        NSError *err = nil;
        std::string src = kShaderSource;
        const char *nonce = std::getenv("PS2X_GS_METAL_SHADER_NONCE"); // forces a cold compile (measurement)
        if (nonce)
            src += std::string("\n// nonce ") + nonce + "\n";
        const auto tc0 = std::chrono::steady_clock::now();
        // MX1: the embedded build-time metallib when it was built from exactly this kShaderSource (SHA-256 match);
        // otherwise (no toolchain at build time, stale, load failure, PS2X_GS_METAL_METALLIB=0) the runtime source compile.
        id<MTLLibrary> lib = nil;
        std::string libKey; // pipeline-archive key: which library the functions come from
        const char *mlEnv = std::getenv("PS2X_GS_METAL_METALLIB");
        if (g_gsmtlMetallibSize > 0 && !(mlEnv && std::strcmp(mlEnv, "0") == 0))
        {
            unsigned char dg[CC_SHA256_DIGEST_LENGTH];
            CC_SHA256(kShaderSource, CC_LONG(std::strlen(kShaderSource)), dg);
            char hex[2 * CC_SHA256_DIGEST_LENGTH + 1];
            for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; ++i)
                std::snprintf(hex + 2 * i, 3, "%02x", dg[i]);
            if (std::strcmp(hex, g_gsmtlMetallibSrcSha256) == 0)
            {
                dispatch_data_t dd = dispatch_data_create(g_gsmtlMetallib, g_gsmtlMetallibSize, nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
                lib = [device newLibraryWithData:dd error:&err];
                if (lib)
                {
                    // archive key = the metallib bytes (dev and release builds compile the same source to different
                    // metallibs, e.g. -mmacosx-version-min, and must not share/overwrite one archive)
                    unsigned char bd[CC_SHA256_DIGEST_LENGTH];
                    CC_SHA256(g_gsmtlMetallib, CC_LONG(g_gsmtlMetallibSize), bd);
                    char bh[2 * CC_SHA256_DIGEST_LENGTH + 1];
                    for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; ++i)
                        std::snprintf(bh + 2 * i, 3, "%02x", bd[i]);
                    libKey = std::string("metallib ") + bh + (nonce ? std::string(" nonce ") + nonce : std::string());
                }
                else
                    std::fprintf(stderr, "[gsmtl] embedded metallib failed to load (%s); compiling the shader source\n",
                                 err ? err.localizedDescription.UTF8String : "?");
            }
            else
                std::fprintf(stderr, "[gsmtl] embedded metallib is stale (source hash mismatch); compiling the shader source\n");
        }
        const bool fromMetallib = lib != nil;
        if (!lib)
        {
            err = nil;
            lib = [device newLibraryWithSource:[NSString stringWithUTF8String:src.c_str()] options:opts error:&err];
            libKey = src;
        }
        const auto tc1 = std::chrono::steady_clock::now();
        if (!lib)
        {
            std::fprintf(stderr, "[gsmtl] shader compile failed: %s\n", err.localizedDescription.UTF8String);
            return nullptr;
        }
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [lib newFunctionWithName:@"vs_main"];
        pd.fragmentFunction = [lib newFunctionWithName:@"fs_main"];
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatR32Uint;
        pd.colorAttachments[1].pixelFormat = MTLPixelFormatR32Uint;
        MTLComputePipelineDescriptor *cd = [MTLComputePipelineDescriptor new];
        cd.computeFunction = [lib newFunctionWithName:@"wb_scatter"];
        // Pipeline cache (G2d): a binary archive of our own pipelines in ~/Library/Caches/RoadTripAdventure, keyed by
        // the shader source and the GPU, so the backend compile of the pipelines happens once per machine.
        id<MTLBinaryArchive> archive = nil;
        NSURL *archiveUrl = nil;
        const char *pc = std::getenv("PS2X_GS_METAL_PIPELINE_CACHE");
        const char *home = std::getenv("HOME");
        if (home && !(pc && std::strcmp(pc, "0") == 0))
        {
            uint64_t h = 1469598103934665603ull;
            for (const char *c : {libKey.c_str(), device.name.UTF8String})
                for (; *c; ++c)
                    h = (h ^ uint8_t(*c)) * 1099511628211ull;
            char name[64];
            std::snprintf(name, sizeof(name), "gsmtl_pipelines_%016llx.metalar", (unsigned long long)h);
            NSString *dir = [NSString stringWithFormat:@"%s/Library/Caches/RoadTripAdventure", home];
            [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
            archiveUrl = [NSURL fileURLWithPath:[dir stringByAppendingPathComponent:[NSString stringWithUTF8String:name]]];
            MTLBinaryArchiveDescriptor *ad = [MTLBinaryArchiveDescriptor new];
            if ([[NSFileManager defaultManager] fileExistsAtPath:archiveUrl.path])
                ad.url = archiveUrl;
            archive = [device newBinaryArchiveWithDescriptor:ad error:&err];
            if (!archive && ad.url)
            {
                ad.url = nil; // unreadable archive: start a fresh one
                archive = [device newBinaryArchiveWithDescriptor:ad error:&err];
            }
        }
        // Every startup pipeline goes through the archive (G2d main + scatter; MX1 adds the s>1 hi pipelines):
        // look up with FailOnBinaryArchiveMiss, build misses normally and add them, serialize once if anything was added.
        const char *scEnv = std::getenv("PS2X_GS_SCALE");
        const int hiScale = std::min(8, std::max(1, scEnv ? std::atoi(scEnv) : 1));
        const char *pgEnv = std::getenv("PS2X_GS_PROGRESSIVE");
        const bool hiProg = pgEnv ? std::atoi(pgEnv) != 0 : hiScale > 1;
        const uint32_t wsPadEnv = GSMetalWidescreenPad(512u);
        const bool wantHi = hiScale > 1 || hiProg || wsPadEnv > 0u;
        MTLRenderPipelineDescriptor *hd = nil;
        MTLComputePipelineDescriptor *hud = nil, *hpd = nil;
        if (wantHi)
        {
            hd = [MTLRenderPipelineDescriptor new];
            hd.vertexFunction = pd.vertexFunction;
            hd.fragmentFunction = [lib newFunctionWithName:@"fs_hi"];
            hd.colorAttachments[0].pixelFormat = MTLPixelFormatR32Uint;
            hd.colorAttachments[1].pixelFormat = MTLPixelFormatR32Uint;
            hud = [MTLComputePipelineDescriptor new];
            hud.computeFunction = [lib newFunctionWithName:@"hi_upscale"];
            hpd = [MTLComputePipelineDescriptor new];
            hpd.computeFunction = [lib newFunctionWithName:@"hi_present"];
        }
        int archiveMisses = 0, archiveAdded = 0;
        NSError *herr = nil;
        auto makeRender = [&](MTLRenderPipelineDescriptor *d, NSError **e) -> id<MTLRenderPipelineState> {
            id<MTLRenderPipelineState> p = nil;
            if (archive)
            {
                d.binaryArchives = @[ archive ];
                p = [device newRenderPipelineStateWithDescriptor:d options:MTLPipelineOptionFailOnBinaryArchiveMiss reflection:nil error:nil];
            }
            if (!p)
            {
                p = [device newRenderPipelineStateWithDescriptor:d error:e];
                if (archive)
                    ++archiveMisses;
            }
            return p;
        };
        auto makeCompute = [&](MTLComputePipelineDescriptor *d, NSError **e) -> id<MTLComputePipelineState> {
            id<MTLComputePipelineState> p = nil;
            if (archive)
            {
                d.binaryArchives = @[ archive ];
                p = [device newComputePipelineStateWithDescriptor:d options:MTLPipelineOptionFailOnBinaryArchiveMiss reflection:nil error:nil];
            }
            if (!p)
            {
                p = [device newComputePipelineStateWithDescriptor:d options:MTLPipelineOptionNone reflection:nil error:e];
                if (archive)
                    ++archiveMisses;
            }
            return p;
        };
        id<MTLRenderPipelineState> pso = makeRender(pd, &err);
        id<MTLComputePipelineState> wbs = pso ? makeCompute(cd, &err) : nil;
        id<MTLRenderPipelineState> hiPso = nil;
        id<MTLComputePipelineState> hiUp = nil, hiPr = nil;
        if (pso && wantHi)
        {
            hiPso = makeRender(hd, &herr);
            hiUp = makeCompute(hud, &herr);
            hiPr = makeCompute(hpd, &herr);
        }
        const auto tc2 = std::chrono::steady_clock::now();
        if (archive && archiveMisses > 0 && pso && wbs)
        {
            // Additions to an archive loaded from a URL are not kept by serializeToURL (measured: the s>1 hi pipelines
            // missed on every launch), so a miss rebuilds a fresh archive holding every pipeline this launch uses.
            NSError *aerr = nil;
            MTLBinaryArchiveDescriptor *fd = [MTLBinaryArchiveDescriptor new];
            id<MTLBinaryArchive> fresh = [device newBinaryArchiveWithDescriptor:fd error:&aerr];
            if (fresh)
            {
                archiveAdded += [fresh addRenderPipelineFunctionsWithDescriptor:pd error:nil] ? 1 : 0;
                archiveAdded += [fresh addComputePipelineFunctionsWithDescriptor:cd error:nil] ? 1 : 0;
                if (hiPso && hiUp && hiPr)
                {
                    archiveAdded += [fresh addRenderPipelineFunctionsWithDescriptor:hd error:nil] ? 1 : 0;
                    archiveAdded += [fresh addComputePipelineFunctionsWithDescriptor:hud error:nil] ? 1 : 0;
                    archiveAdded += [fresh addComputePipelineFunctionsWithDescriptor:hpd error:nil] ? 1 : 0;
                }
                archive = fresh;
            }
            // temp + rename: concurrent runs never see a half-written archive
            NSURL *tmp = [NSURL fileURLWithPath:[archiveUrl.path stringByAppendingFormat:@".tmp%d", int(getpid())]];
            if ([archive serializeToURL:tmp error:&aerr])
                std::rename(tmp.path.UTF8String, archiveUrl.path.UTF8String);
            else
                std::remove(tmp.path.UTF8String);
        }
        const auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
        std::fprintf(stderr, "[gsmtl] shaders: library %.1f ms (%s), pipelines %.1f ms (archive %s, %d miss, %d saved)\n", ms(tc0, tc1),
                     fromMetallib ? "metallib" : "source", ms(tc1, tc2), archive ? (archiveMisses ? "miss" : "hit") : "off",
                     archiveMisses, archiveAdded);
        if (!pso)
        {
            std::fprintf(stderr, "[gsmtl] pipeline failed: %s\n", err.localizedDescription.UTF8String);
            return nullptr;
        }
        std::unique_ptr<GSMetalBackend> b(new GSMetalBackend());
        b->m->device = device;
        b->m->queue = [device newCommandQueue];
        g_sharedQueue = b->m->queue; // SH1: the shell presents direct scaled frames on this queue
        b->m->pipeline = pso;
        b->m->lib = lib; // self-test pipelines are created on first use
        b->m->wbScatter = wbs;
        b->m->cpuWriteback = std::getenv("PS2X_GS_METAL_CPU_WRITEBACK") != nullptr;
        {
            b->m->hsx = uint32_t(hiScale);
            b->m->hsy = uint32_t(hiScale) * (hiProg ? 2u : 1u);
            b->m->wsPad = wsPadEnv;
            if (b->m->hsx > 1u || b->m->hsy > 1u || wsPadEnv > 0u)
            {
                b->m->hiPipeline = hiPso;
                b->m->hiUpscale = hiUp;
                b->m->hiPresent = hiPr;
                b->m->hi = b->m->hiPipeline && b->m->hiUpscale && b->m->hiPresent;
                g_hiActive.store(b->m->hi, std::memory_order_release);
                std::fprintf(stderr, "[gsmtl-hi] display scale %ux%u %s%s\n", b->m->hsx, b->m->hsy, b->m->hi ? "on" : "FAILED: ",
                             b->m->hi ? "" : (herr ? herr.localizedDescription.UTF8String : "?"));
                if (wsPadEnv)
                    std::fprintf(stderr, "[gsmtl-ws] widescreen %s: pad %u px each side (%u px at 1x)\n", std::getenv("PS2X_WIDESCREEN"), wsPadEnv,
                                 512u + 2u * wsPadEnv);
                if (!b->m->hi)
                    b->m->wsPad = 0u;
            }
        }
        b->m->cpu = std::make_unique<GSCpuBackend>();
        // page tables (filled by the GSCpuBackend constructor above) for the write-back scatter
        {
            std::vector<uint16_t> tc(64u * 32u), tz(64u * 32u);
            const auto &c32 = GSMem::PageTable<GSMem::C32>()[0];
            const auto &z24 = GSMem::PageTable<GSMem::Z24>()[0];
            for (uint32_t y = 0; y < 32u; ++y)
                for (uint32_t x = 0; x < 64u; ++x)
                {
                    tc[y * 64u + x] = uint16_t(c32[y][x]);
                    tz[y * 64u + x] = uint16_t(z24[y][x]);
                }
            b->m->tblC32 = [device newBufferWithBytes:tc.data() length:tc.size() * 2u options:MTLResourceStorageModeShared];
            b->m->tblZ24 = [device newBufferWithBytes:tz.data() length:tz.size() * 2u options:MTLResourceStorageModeShared];
        }
        b->m->timing = std::getenv("PS2X_GS_METAL_TIMING") != nullptr;
        b->m->flushAll = std::getenv("PS2X_GS_METAL_FLUSH_ALL") != nullptr;
        b->m->eager = b->m->flushAll;
        b->m->palBuf = [device newBufferWithLength:size_t(Impl::kPalSlots) * 256u * 4u options:MTLResourceStorageModeShared];
        {
            MTLTextureDescriptor *dd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Uint width:1 height:1 mipmapped:NO];
            dd.usage = MTLTextureUsageShaderRead;
            dd.storageMode = MTLStorageModeShared;
            b->m->dummyTex = [device newTextureWithDescriptor:dd];
            const uint32_t zero = 0;
            [b->m->dummyTex replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&zero bytesPerRow:4];
        }
        std::fprintf(stderr, "[gsmtl] Metal backend on %s\n", device.name.UTF8String);
        std::fflush(stderr);
        return b;
    }
}

void GSMetalBackend::Initialize(uint8_t *vram, uint32_t vramSize)
{
    m->ResolveAll(kFlushOther);
    m->vram = vram;
    m->vramSize = vramSize;
    m->BindShadow();
    m->cpu->Initialize(vram, vramSize);
    m->MarkAll();
}

void GSMetalBackend::Reset()
{
    m->ResolveAll(kFlushOther);
    m->cpu->Reset();
}

void GSMetalBackend::Submit(const GSPrimitiveBatch &batch)
{
    if (!m->vram || batch.vertexCount == 0u)
        return;
    ++m->frame.submits;
    const uint64_t t0 = m->timing ? nowNs() : 0;
    const auto fallback = [&](const char *reason)
    {
        m->ResolveAll(kFlushFallback);
        m->cpu->Submit(batch);
        m->MarkCpuDraw(batch);
        ++m->frame.fallbackPrims;
        ++m->frame.fallbacks[reason];
        if (m->timing)
            m->frame.fallbackNs += nowNs() - t0;
    };
    m->fbSrc = false;
    m->primTexHi = nil;
    if (const char *reason = m->Eligible(batch))
    {
        fallback(reason);
        return;
    }
    const GSContext &ctx = batch.state.context;
    const bool rtz = fpcrIsTowardZero(readFpcr());
    m->SelectRun(ctx);
    uint64_t tTex = 0;
    if (m->ts.on)
    {
        const uint64_t a = m->timing ? nowNs() : 0;
        bool prepared = false;
        if (batch.state.prim.type == GS_PRIM_SPRITE)
        {
            m->ComputeSpriteGeom(batch);
            prepared = m->TryFbTexture(ctx);
            if (!prepared && m->sg.ok && m->TextureOverlapsRun())
            {
                // The sprite may read pixels it has already written: only the oracle's live VRAM is exact.
                static const bool dbg = std::getenv("PS2X_GS_METAL_DBG") != nullptr;
                static int shown = 0;
                if (dbg && shown++ < 12)
                {
                    const auto &sgm = m->sg;
                    std::fprintf(stderr, "[gsmtl-dbg] tex_self: tbp=%u tbw=%u psm=%u tex=%ux%u wms=%u wmt=%u lin=%d fst=%d | run fbp=%u zbp=%u fbw=%u h=%u | sprite %d,%d..%d,%d u=%.3f..%.3f v=%.3f..%.3f\n",
                                 m->ts.key.tbp, m->ts.key.tbw, m->ts.key.psm, m->ts.texW, m->ts.texH, m->ts.wms, m->ts.wmt, m->ts.linear, m->ts.fst,
                                 m->run ? m->run->fbp : 0u, m->run ? m->run->zbp : 0u, m->run ? m->run->fbw : 0u, m->run ? m->run->c->height : 0u, sgm.gx0, sgm.gy0, sgm.gx1,
                                 sgm.gy1, sgm.u0f, sgm.u1f, sgm.v0f, sgm.v1f);
                    {
                        const bool fst2 = m->ts.fst;
                        const auto ru = Impl::SpriteAxis(Impl::SpriteCoord(sgm.gx0, sgm.un0x, sgm.w, sgm.u0f, sgm.u1f, fst2), Impl::SpriteCoord(sgm.gx1, sgm.un0x, sgm.w, sgm.u0f, sgm.u1f, fst2),
                                                         m->ts.linear, int(m->ts.texW), m->ts.wms, m->ts.minU, m->ts.maxU);
                        std::fprintf(stderr, "[gsmtl-dbg]   u ok=%d lo=%d hi=%d minU=%u maxU=%u runPages=%zu", ru.ok, ru.lo, ru.hi, m->ts.minU, m->ts.maxU, m->runPages.count());
                        PageRange pr;
                        pagesOfRect(m->ts.key.tbp, m->ts.key.tbw, uint8_t(m->ts.key.psm), uint32_t(ru.lo), 0u, uint32_t(ru.hi), 255u, pr);
                        for (uint32_t i = 0; i < pr.count; ++i)
                            if (m->runPages.test(pr.pages[i]))
                            {
                                std::fprintf(stderr, " overlap page %u", pr.pages[i]);
                                break;
                            }
                        std::fprintf(stderr, "\n");
                    }
                    std::fflush(stderr);
                }
                fallback("tex_self");
                return;
            }
        }
        if (!prepared)
            m->PrepareTexture(batch, ctx);
        tTex = m->timing ? nowNs() - a : 0;
    }
    ++m->frame.metalPrims;
    if (m->ts.on)
        ++m->frame.metalTexPrims;
    ++m->runPrims;
    m->BindSegment(m->ts.on ? m->primTex : nil, m->ts.on ? m->primTexHi : nil);
    m->Record(batch, rtz);
    if (m->prims.size() >= 65536u)
        m->CloseRun(kFlushLimit);
    if (m->timing)
        m->frame.recordNs += nowNs() - t0 - tTex;
}

void GSMetalBackend::LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut)
{
    // Same filter as GSCpuBackend::LoadClut: only indexed textures with a CLD that loads read VRAM here.
    bool loads = tex0.psm == GS_PSM_T4 || tex0.psm == GS_PSM_T4HL || tex0.psm == GS_PSM_T4HH || tex0.psm == GS_PSM_T8 || tex0.psm == GS_PSM_T8H;
    if (loads)
    {
        switch (tex0.cld)
        {
        case 1u: break;
        case 2u: m->clutCbpMirror[0] = tex0.cbp; break;
        case 3u: m->clutCbpMirror[1] = tex0.cbp; break;
        case 4u:
            if (m->clutCbpMirror[0] == tex0.cbp) loads = false; else m->clutCbpMirror[0] = tex0.cbp;
            break;
        case 5u:
            if (m->clutCbpMirror[1] == tex0.cbp) loads = false; else m->clutCbpMirror[1] = tex0.cbp;
            break;
        default: loads = false; break;
        }
    }
    if (loads && m->anyDirtyPlane)
    {
        // The CLUT is read from VRAM here (CSM1: a 16x16 block at CBP; CSM2: a row at COU/COV).
        PageRange r;
        if (tex0.csm == 0u)
            pagesOfRect(tex0.cbp, 1u, tex0.cpsm, 0u, 0u, 15u, 15u, r);
        else
            pagesOfRect(tex0.cbp, std::max<uint32_t>(texclut.cbw, 1u), tex0.cpsm, uint32_t(texclut.cou) << 4, texclut.cov,
                        (uint32_t(texclut.cou) << 4) + 255u, texclut.cov, r);
        {
            static const bool dbg = std::getenv("PS2X_GS_METAL_DBG") != nullptr;
            static int shown = 0;
            if (dbg && shown < 30)
            {
                const std::bitset<kPages> b = Impl::BitsOf(r);
                for (auto &q : m->planes)
                    if (q->anyDirty() && (q->dirty & b).any())
                    {
                        std::fprintf(stderr, "[gsmtl-dbg] clut hit: cbp=%u cpsm=%u csm=%u cld=%u psm=%u csa=%u pages=%u first=%u | plane %s base=%u fbw=%u h=%u dirty bbox %d,%d..%d,%d\n", tex0.cbp, tex0.cpsm, tex0.csm,
                                     tex0.cld, tex0.psm, tex0.csa, r.count, r.count ? r.pages[0] : 0u, q->depth ? "z" : "c", q->base, q->fbw, q->height, q->dx0, q->dy0, q->dx1, q->dy1);
                        ++shown;
                    }
                std::fflush(stderr);
            }
        }
        m->CpuRead(r, kFlushClut);
    }
    m->cpu->LoadClut(tex0, texclut);
}

void GSMetalBackend::BeginTransfer(const GSTransferCommand &command)
{
    const GSBitBltBuf &b = command.bitbltbuf;
    const bool rect = command.trxreg.rrw && command.trxreg.rrh;
    PageRange src, dst;
    src.count = dst.count = 0;
    // pages the transfer reads (local->host, local->local) and writes (host->local, local->local)
    if (command.direction == 1u || command.direction == 2u)
        pagesOfRect(b.sbp, b.sbw, b.spsm, command.trxpos.ssax, command.trxpos.ssay, command.trxpos.ssax + std::max<uint32_t>(command.trxreg.rrw, 1u) - 1u,
                    command.trxpos.ssay + std::max<uint32_t>(command.trxreg.rrh, 1u) - 1u, src);
    if ((command.direction == 0u || command.direction == 2u) && rect)
        pagesOfRect(b.dbp, b.dbw, b.dpsm, command.trxpos.dsax, command.trxpos.dsay, command.trxpos.dsax + command.trxreg.rrw - 1u,
                    command.trxpos.dsay + command.trxreg.rrh - 1u, dst);
    if (m->run || m->anyDirtyPlane)
    {
        m->CpuRead(src, kFlushTransfer);
        m->CpuWrite(dst, kFlushTransfer);
        if (command.direction > 2u)
            m->ResolveAll(kFlushTransfer);
    }
    m->cpu->BeginTransfer(command);
    m->transfer = command;
    m->transferPages = dst;
    if (dst.count)
    {
        if (dst.count >= 512u)
            m->MarkAll();
        else
            m->MarkPages(dst);
    }
}

void GSMetalBackend::UploadImage(const uint8_t *data, uint32_t sizeBytes)
{
    if (m->run || m->anyDirtyPlane)
        m->CpuWrite(m->transferPages, kFlushTransfer);
    m->cpu->UploadImage(data, sizeBytes);
    if (m->transferPages.count >= 512u)
        m->MarkAll();
    else if (m->transferPages.count)
        m->MarkPages(m->transferPages);
}

void GSMetalBackend::Flush()
{
    m->CloseRun(kFlushOther);
}

void GSMetalBackend::TextureFlush()
{
    // Texture reads validate against page epochs (and GPU-newer pages resolve on demand): nothing to do.
    m->cpu->TextureFlush();
}

void GSMetalBackend::Sync(GSSyncReason reason)
{
    // Presentation/Finish: the present itself resolves what the display needs.
    if (reason != GSSyncReason::Presentation && reason != GSSyncReason::Finish)
        m->ResolveAll(kFlushReadback);
    m->cpu->Sync(reason);
}

PresentationFrame GSMetalBackend::Present(const GSPresentationRequest &request)
{
    // The display reads the shadow: write the colour planes back once (depth stays on the GPU).
    m->ResolvePlanes(nullptr, true, nullptr, nullptr, kFlushPresent);
    ++m->frame.presents;
    static const uint32_t s_statsEvery = std::getenv("PS2X_GS_METAL_STATS") ? uint32_t(std::max(1, std::atoi(std::getenv("PS2X_GS_METAL_STATS")))) : 0u;
    if (s_statsEvery && m->frame.presents % s_statsEvery == 0u)
    {
        Stats t = m->total;
        t.Add(m->frame);
        GSMetalPrintStats("progress", t);
        std::fprintf(stderr, "[gsmtl-hi] scale=%ux%u passes=%llu presents=%llu fallbacks=%llu upscale_px=%llu gpu_busy_s=%.3f\n", m->hsx, m->hsy,
                     (unsigned long long)m->hiPasses, (unsigned long long)m->hiPresents, (unsigned long long)m->hiFallbacks,
                     (unsigned long long)m->hiUpscalePx, m->gpuBusyS);
        std::fflush(stderr);
    }
    PresentationFrame result = m->cpu->Present(request);
    if (m->hi)
        m->BuildHiFrame(request, result);
    return result;
}

bool GSMetalBackend::ClearFramebuffer(const GSContext &context, uint32_t rgba)
{
    m->ResolveAll(kFlushOther);
    const bool r = m->cpu->ClearFramebuffer(context, rgba);
    m->MarkAll();
    return r;
}

uint32_t GSMetalBackend::ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes)
{
    m->ResolveAll(kFlushReadback);
    return m->cpu->ConsumeLocalToHostBytes(dst, maxBytes);
}

uint32_t GSMetalBackend::ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const
{
    m->ResolveAll(kFlushReadback);
    return m->cpu->ReadVram(psm, base, bw, x, y);
}

void GSMetalBackend::WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    m->ResolveAll(kFlushOther);
    m->cpu->WriteVram(psm, base, bw, x, y, value);
    m->MarkAll();
}

void GSMetalBackend::SnapshotVram(std::vector<uint8_t> &out) const
{
    m->ResolveAll(kFlushReadback);
    m->cpu->SnapshotVram(out);
}

GSTransferSnapshot GSMetalBackend::GetTransferSnapshot() const
{
    m->ResolveAll(kFlushReadback);
    return m->cpu->GetTransferSnapshot();
}

void GSMetalBackend::SetRunObserver(std::function<void(const RunInfo &)> observer)
{
    m->observer = std::move(observer);
    m->eager = m->flushAll || static_cast<bool>(m->observer);
}

void GSMetalBackend::MarkShadowChanged()
{
    m->ResolveAll(kFlushOther);
    m->MarkAll();
}

void GSMetalBackend::FlushRun()
{
    m->Drain(kFlushOther);
}

GSMetalBackend::Stats GSMetalBackend::TakeFrameStats()
{
    Stats s = m->frame;
    m->total.Add(s);
    m->frame = Stats{};
    return s;
}

const GSMetalBackend::Stats &GSMetalBackend::TotalStats() const
{
    return m->total;
}

namespace
{
    // Host reference, compiled with the same -ffp-contract=on expressions as gs_cpu_backend.cpp.
    __attribute__((noinline)) void hostRefF(const float *in, uint32_t *out)
    {
        const float a = in[0], b = in[1], c = in[2];
        const float s = a + b;
        const float p = a * b;
        const float f = std::fma(a, b, c);
        std::memcpy(&out[0], &s, 4);
        std::memcpy(&out[1], &p, 4);
        std::memcpy(&out[2], &f, 4);
        const uint32_t c0 = uint32_t(in[3]) & 255u, c1 = (uint32_t(in[3]) >> 8) & 255u, c2 = 7u;
        const float w0 = a, w1 = b, w2 = c;
        out[3] = GSInternal::clampU8(static_cast<int>(c0 * w0 + c1 * w1 + c2 * w2));
    }

    __attribute__((noinline)) uint32_t hostRefZ(const float *z, const float *w)
    {
        const double z0 = z[0], z1 = z[1], z2 = z[2];
        const float w0 = w[0], w1 = w[1], w2 = w[2];
        double zz = z0 * w0 + z1 * w1 + z2 * w2;
        return static_cast<uint32_t>(zz + 0.5);
    }
}

uint64_t GSMetalBackend::SelfTest(uint32_t cases)
{
    uint64_t mismatches = 0;
    @autoreleasepool
    {
        std::mt19937 rng(12345u);
        std::uniform_real_distribution<float> coord(-64.0f, 1100.0f);
        std::uniform_real_distribution<float> weight(-0.0002f, 1.0002f);
        std::uniform_int_distribution<uint32_t> bits(0u, 0xFFFFFFFFu);
        std::uniform_int_distribution<uint32_t> zdist(0u, 0xFFFFFFu);
        std::vector<float> fin(size_t(cases) * 4u), zin(size_t(cases) * 4u), win(size_t(cases) * 4u);
        for (uint32_t i = 0; i < cases; ++i)
        {
            float *v = &fin[size_t(i) * 4u];
            switch (i % 4u)
            {
            case 0: // edge-function-like operands (1/16 fractions)
                v[0] = std::round(coord(rng) * 16.0f) / 16.0f + 0.5f;
                v[1] = -std::round(coord(rng) * 16.0f) / 16.0f;
                v[2] = coord(rng) * coord(rng);
                break;
            case 1: // weights
                v[0] = weight(rng);
                v[1] = weight(rng);
                v[2] = weight(rng);
                break;
            default: // random finite normal floats
                for (int k = 0; k < 3; ++k)
                {
                    uint32_t u;
                    do
                    {
                        u = bits(rng);
                        u = (u & 0x807FFFFFu) | ((((u >> 23) & 0xFFu) % 120u + 68u) << 23);
                    } while (false);
                    std::memcpy(&v[k], &u, 4);
                }
                break;
            }
            v[3] = float(bits(rng) & 0xFFFFu);
            float *z = &zin[size_t(i) * 4u];
            float *w = &win[size_t(i) * 4u];
            for (int k = 0; k < 3; ++k)
            {
                z[k] = float(zdist(rng));
                w[k] = weight(rng);
            }
            z[3] = w[3] = 0.0f;
        }
        for (uint32_t mode = 0; mode < 2; ++mode)
        {
            const uint32_t rtz = mode;
            id<MTLBuffer> bin = [m->device newBufferWithBytes:fin.data() length:fin.size() * 4u options:MTLResourceStorageModeShared];
            id<MTLBuffer> bout = [m->device newBufferWithLength:size_t(cases) * 16u options:MTLResourceStorageModeShared];
            id<MTLBuffer> bz = [m->device newBufferWithBytes:zin.data() length:zin.size() * 4u options:MTLResourceStorageModeShared];
            id<MTLBuffer> bw = [m->device newBufferWithBytes:win.data() length:win.size() * 4u options:MTLResourceStorageModeShared];
            id<MTLBuffer> bzo = [m->device newBufferWithLength:size_t(cases) * 4u options:MTLResourceStorageModeShared];
            id<MTLCommandBuffer> cmd = [m->queue commandBuffer];
            m->EnsureSelftestPipelines();
            id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
            [enc setComputePipelineState:m->selftestF];
            [enc setBuffer:bin offset:0 atIndex:0];
            [enc setBuffer:bout offset:0 atIndex:1];
            [enc setBytes:&rtz length:4 atIndex:2];
            [enc dispatchThreads:MTLSizeMake(cases, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
            [enc setComputePipelineState:m->selftestZ];
            [enc setBuffer:bz offset:0 atIndex:0];
            [enc setBuffer:bw offset:0 atIndex:1];
            [enc setBuffer:bzo offset:0 atIndex:2];
            [enc setBytes:&rtz length:4 atIndex:3];
            [enc dispatchThreads:MTLSizeMake(cases, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
            [enc endEncoding];
            [cmd commit];
            [cmd waitUntilCompleted];

            const int oldRound = std::fegetround();
            const uint64_t oldFpcr = readFpcr();
            if (rtz)
            {
                std::fesetround(FE_TOWARDZERO);
#if defined(__aarch64__)
                const uint64_t f = readFpcr() | (1ull << 24);
                __asm__ volatile("msr fpcr, %0" : : "r"(f));
#endif
            }
            uint64_t bad[5] = {0, 0, 0, 0, 0};
            const uint32_t *go = static_cast<const uint32_t *>(bout.contents);
            const uint32_t *gz = static_cast<const uint32_t *>(bzo.contents);
            for (uint32_t i = 0; i < cases; ++i)
            {
                uint32_t ref[4];
                hostRefF(&fin[size_t(i) * 4u], ref);
                for (int k = 0; k < 4; ++k)
                    if (ref[k] != go[size_t(i) * 4u + k])
                    {
                        if (bad[k] < 3u)
                            std::fprintf(stderr, "[gsmtl] selftest mismatch op=%d a=%a b=%a c=%a host=%08x gpu=%08x\n", k, fin[size_t(i) * 4u],
                                         fin[size_t(i) * 4u + 1], fin[size_t(i) * 4u + 2], ref[k], go[size_t(i) * 4u + k]);
                        ++bad[k];
                    }
                if (hostRefZ(&zin[size_t(i) * 4u], &win[size_t(i) * 4u]) != gz[i])
                    ++bad[4];
            }
#if defined(__aarch64__)
            __asm__ volatile("msr fpcr, %0" : : "r"(oldFpcr));
#endif
            std::fesetround(oldRound);
            const uint64_t sum = bad[0] + bad[1] + bad[2] + bad[3] + bad[4];
            mismatches += sum;
            std::fprintf(stderr, "[gsmtl] selftest %s cases=%u add=%llu mul=%llu fma=%llu interp=%llu z=%llu\n", rtz ? "rtz" : "rne", cases,
                         (unsigned long long)bad[0], (unsigned long long)bad[1], (unsigned long long)bad[2], (unsigned long long)bad[3],
                         (unsigned long long)bad[4]);
        }
        std::fflush(stderr);
    }
    return mismatches;
}

// ---------------------------------------------------------------------------------------------
// Differential test: random textured triangles through GSMetalBackend and the oracle GSCpuBackend
// on identical VRAM; the frame and Z planes must stay byte-identical after every draw.
// ---------------------------------------------------------------------------------------------
uint64_t GSMetalBackend::SelfTestDraws(uint32_t draws, uint32_t seed)
{
    std::unique_ptr<GSMetalBackend> mb = GSMetalBackend::Create();
    if (!mb)
        return ~0ull;
    std::vector<uint8_t> vramA(4u << 20), vramB(4u << 20);
    std::mt19937 rng(seed);
    auto rnd = [&](uint32_t n) { return n ? uint32_t(rng() % n) : 0u; };
    auto rndf = [&](float lo, float hi) { return lo + (hi - lo) * (float(rng() & 0xFFFFFFu) / float(0x1000000)); };
    auto refill = [&]()
    {
        for (size_t i = 0; i < vramA.size(); i += 4)
        {
            uint32_t v = static_cast<uint32_t>(rng());
            if (rng() & 1u)
                v = (v & 0x00FFFFFFu) | 0x80000000u; // opaque-ish
            std::memcpy(&vramA[i], &v, 4);
        }
        vramB = vramA;
        mb->Initialize(vramA.data(), uint32_t(vramA.size()));
    };
    GSCpuBackend ref;
    ref.Initialize(vramB.data(), uint32_t(vramB.size()));
    refill();

    uint64_t bad = 0;
    uint64_t counts[2] = {0, 0};
    for (uint32_t d = 0; d < draws; ++d)
    {
        if (d % 64u == 0u)
        {
            refill();
            ref.Initialize(vramB.data(), uint32_t(vramB.size()));
        }
        const bool rtz = (d & 1u) == 0u;
        GSPrimitiveBatch batch;
        GSDrawState &st = batch.state;
        GSContext &c = st.context;
        c.frame.fbp = 0;
        c.frame.fbw = 8;
        c.frame.psm = GS_PSM_CT32;
        c.frame.fbmsk = (rnd(8) == 0) ? static_cast<uint32_t>(rng()) : 0u;
        c.zbuf.zbp = 128;
        c.zbuf.psm = GS_PSM_Z24;
        c.zbuf.zmask = rnd(8) == 0;
        c.scissor = {0, 511, 0, 255};
        c.scissor.x0 = 0; c.scissor.x1 = 511; c.scissor.y0 = 0; c.scissor.y1 = 255;
        c.xyoffset.ofx = 28672;
        c.xyoffset.ofy = 30720;
        static const uint8_t psms[] = {GS_PSM_T8, GS_PSM_T4, GS_PSM_CT32, GS_PSM_CT24, GS_PSM_T8, GS_PSM_T4};
        // 0-3 triangle, 4-6 sprite (generic), 7-8 sprite sampling another framebuffer target, 9 feedback sprite (own target)
        const uint32_t kind = rnd(10);
        GSTex0Reg &t = c.tex0;
        t.psm = psms[rnd(6)];
        t.tw = uint8_t(4 + rnd(5));
        t.th = uint8_t(4 + rnd(5));
        t.tbw = uint8_t(1 + rnd(8));
        t.tbp0 = 8192u + rnd(7000u);
        if (rnd(4) == 0)
            t.tbp0 &= ~31u;
        t.tcc = rnd(4) != 0;
        t.tfx = rnd(5) == 0 ? rnd(4) : 0u;
        t.cbp = 4200u + rnd(2000u);
        t.cpsm = rnd(5) == 0 ? GS_PSM_CT16 : GS_PSM_CT32;
        t.csm = 0;
        t.csa = uint8_t(rnd(16));
        t.cld = 1;
        if (kind >= 7)
        {
            t.psm = rnd(2) ? GS_PSM_CT32 : GS_PSM_CT24;
            t.tbw = 8;
            t.tbp0 = (kind == 9) ? 0u : 2048u;
            t.tw = rnd(3) == 0 ? 10 : 9;
            t.th = rnd(3) == 0 ? 9 : 8;
        }
        st.textureWidth = uint16_t(1u << t.tw);
        st.textureHeight = uint16_t(1u << t.th);
        st.linearFilter = rnd(6) != 0;
        if (kind == 9)
            st.linearFilter = rnd(4) == 0;
        st.texa.ta0 = uint8_t(rng());
        st.texa.ta1 = uint8_t(rng());
        st.texa.aem = rnd(2) != 0;
        auto region = [&]() { return uint64_t(rnd(1024)); };
        uint64_t clamp = 0;
        uint64_t wms = rnd(7) == 0 ? rnd(4) : rnd(2), wmt = rnd(7) == 0 ? rnd(4) : rnd(2);
        clamp = wms | (wmt << 2);
        if (wms == 2u || wms == 3u)
            clamp |= (wms == 2u ? (region() & 0x3FFu) | (uint64_t(rnd(1024)) << 10) : (uint64_t(rnd(1024)) & 0x3C0u)) << 4;
        c.clamp = clamp | (region() << 24) | (region() << 34);
        c.clamp = (wms) | (wmt << 2) | ((wms == 2u ? uint64_t(rnd(8)) : uint64_t(rnd(1024) & 0x3F0u)) << 4) |
                  ((wms == 2u ? uint64_t(32 + rnd(224)) : uint64_t(rnd(64))) << 14) |
                  ((wmt == 2u ? uint64_t(rnd(8)) : uint64_t(rnd(1024) & 0x3F0u)) << 24) |
                  ((wmt == 2u ? uint64_t(32 + rnd(224)) : uint64_t(rnd(64))) << 34);
        if (kind >= 7 && rnd(6) != 0)
            c.clamp = (wms & 1u) | ((wmt & 1u) << 2);
        const uint32_t atst = rnd(8), afail = rnd(4);
        c.test = (rnd(3) != 0 ? 1ull : 0ull) | (uint64_t(atst) << 1) | (uint64_t(rnd(256)) << 4) | (uint64_t(afail) << 12) |
                 (uint64_t(rnd(4) == 0) << 14) | (uint64_t(rnd(2)) << 15) | (uint64_t(1) << 16) | (uint64_t(1 + rnd(3)) << 17);
        c.alpha = uint64_t(rnd(3)) | (uint64_t(rnd(3)) << 2) | (uint64_t(rnd(3)) << 4) | (uint64_t(rnd(3)) << 6) | (uint64_t(rnd(256)) << 32);
        st.pabe = rnd(8) == 0;
        st.fogR = uint8_t(rng());
        st.fogG = uint8_t(rng());
        st.fogB = uint8_t(rng());
        st.prim.type = kind >= 4 ? GS_PRIM_SPRITE : (rnd(2) ? GS_PRIM_TRIANGLE : GS_PRIM_TRISTRIP);
        st.prim.iip = rnd(4) != 0;
        st.prim.tme = true;
        st.prim.fge = rnd(3) != 0;
        st.prim.abe = rnd(3) != 0;
        st.prim.fst = rnd(6) == 0 || (kind >= 7 && rnd(2) == 0);
        batch.vertexCount = kind >= 4 ? 2 : 3;
        const float cx = rndf(1792.0f + 20.0f, 1792.0f + 490.0f), cy = rndf(1920.0f + 10.0f, 1920.0f + 240.0f);
        const float ext = rnd(5) == 0 ? 160.0f : 40.0f;
        for (int i = 0; i < 3; ++i)
        {
            GSVertex &v = batch.vertices[i];
            v.x = std::round((cx + rndf(-ext, ext)) * 16.0f) / 16.0f;
            v.y = std::round((cy + rndf(-ext, ext)) * 16.0f) / 16.0f;
            v.z = double(rnd(0x1000000u));
            v.r = uint8_t(rng());
            v.g = uint8_t(rng());
            v.b = uint8_t(rng());
            v.a = uint8_t(rng());
            v.fog = uint8_t(rng());
            float q = rndf(0.25f, 6.0f);
            if (rnd(40) == 0)
                q = rndf(-3.0f, 3.0f);
            if (rnd(200) == 0)
                q = rndf(-1e-7f, 1e-7f);
            v.q = q;
            v.s = rndf(-0.5f, 2.5f) * q;
            v.t = rndf(-0.5f, 2.5f) * q;
            v.u = uint16_t(rnd(1024u * 16u));
            v.v = uint16_t(rnd(1024u * 16u));
        }
        if (kind >= 4)
        {
            GSVertex &a = batch.vertices[0];
            GSVertex &b = batch.vertices[1];
            int rx0, ry0, rw, rh;
            if (kind >= 7)
            {
                if (rnd(4) == 0)
                {
                    rx0 = 0; ry0 = 0; rw = 512; rh = 256;
                }
                else
                {
                    rx0 = int(rnd(300)); ry0 = int(rnd(150)); rw = 1 + int(rnd(212)); rh = 1 + int(rnd(106));
                }
            }
            else
            {
                rx0 = int(rnd(500)); ry0 = int(rnd(250));
                rw = 1 + int(rnd(rnd(3) == 0 ? 400 : 60));
                rh = 1 + int(rnd(rnd(3) == 0 ? 200 : 40));
            }
            a.x = 1792.0f + float(rx0); a.y = 1920.0f + float(ry0);
            b.x = 1792.0f + float(rx0 + rw); b.y = 1920.0f + float(ry0 + rh);
            if (kind < 7 && rnd(8) == 0)
                std::swap(a.x, b.x);
            if (kind < 7 && rnd(8) == 0)
                std::swap(a.y, b.y);
            if (rnd(2) == 0)
            {
                a.z = double(rng()); // full 32-bit sprite Z (not float-exact, above 2^24)
                b.z = double(rng());
            }
            if (kind >= 7)
            {
                // 1:1 texel = pixel copy (the game's full-screen feedback sprite), optionally offset / scaled
                const uint32_t j = rnd(4);
                const int du = (j == 2) ? int(rnd(49)) - 24 : 0, dv = (j == 2) ? int(rnd(49)) - 24 : 0;
                const int su = (j == 3) ? int(rnd(40)) - 20 : 0, sv = (j == 3) ? int(rnd(40)) - 20 : 0;
                const int fu0 = rx0 * 16 + du, fv0 = ry0 * 16 + dv, fu1 = (rx0 + rw + su) * 16 + du, fv1 = (ry0 + rh + sv) * 16 + dv;
                a.u = uint16_t(std::max(fu0, 0)); a.v = uint16_t(std::max(fv0, 0));
                b.u = uint16_t(std::max(fu1, 0)); b.v = uint16_t(std::max(fv1, 0));
                const float tw = float(st.textureWidth), th = float(st.textureHeight);
                a.q = 1.0f; b.q = 1.0f;
                a.s = (float(fu0) / 16.0f) / tw; a.t = (float(fv0) / 16.0f) / th;
                b.s = (float(fu1) / 16.0f) / tw; b.t = (float(fv1) / 16.0f) / th;
            }
        }
        // CLUT: both backends load it from their (identical) VRAM
        GSTexClutReg tc{};
        mb->LoadClut(t, tc);
        ref.LoadClut(t, tc);
        if (kind == 7 || kind == 8)
        {
            // make the "other" framebuffer (fbp 64, the next buffer: TBP = FBP + 2048 blocks) a valid GPU-owned target
            for (int k = 0; k < 2; ++k)
            {
                GSPrimitiveBatch bb{};
                GSContext &bc = bb.state.context;
                bc.frame.fbp = 64; bc.frame.fbw = 8; bc.frame.psm = GS_PSM_CT32; bc.frame.fbmsk = 0;
                bc.zbuf.zbp = 192; bc.zbuf.psm = GS_PSM_Z24; bc.zbuf.zmask = false;
                bc.scissor.x0 = 0; bc.scissor.x1 = 511; bc.scissor.y0 = 0; bc.scissor.y1 = 255;
                bc.xyoffset = c.xyoffset;
                bc.test = (1ull << 16) | (1ull << 17);
                bb.state.prim.type = GS_PRIM_SPRITE;
                bb.vertexCount = 2;
                const int bx0 = int(rnd(400)), by0 = int(rnd(200));
                bb.vertices[0].x = 1792.0f + float(bx0); bb.vertices[0].y = 1920.0f + float(by0);
                bb.vertices[1].x = 1792.0f + float(bx0 + 1 + int(rnd(110))); bb.vertices[1].y = 1920.0f + float(by0 + 1 + int(rnd(56)));
                for (int i = 0; i < 2; ++i)
                {
                    bb.vertices[i].z = double(rnd(0x1000000u));
                    bb.vertices[i].r = uint8_t(rng()); bb.vertices[i].g = uint8_t(rng()); bb.vertices[i].b = uint8_t(rng()); bb.vertices[i].a = uint8_t(rng());
                }
                mb->Submit(bb);
                ref.Submit(bb);
            }
            mb->FlushRun();
        }
        const uint64_t oldFpcr = readFpcr();
        const int oldRound = std::fegetround();
        if (rtz)
        {
            std::fesetround(FE_TOWARDZERO);
#if defined(__aarch64__)
            const uint64_t f = readFpcr() | (1ull << 24);
            __asm__ volatile("msr fpcr, %0" : : "r"(f));
#endif
        }
        mb->Submit(batch);
        ref.Submit(batch);
#if defined(__aarch64__)
        __asm__ volatile("msr fpcr, %0" : : "r"(oldFpcr));
#endif
        std::fesetround(oldRound);
        mb->FlushRun();
        ++counts[rtz ? 1 : 0];
        if (std::memcmp(vramA.data(), vramB.data(), vramA.size()) != 0)
        {
            ++bad;
            size_t first = 0, n = 0;
            for (size_t i = 0; i < vramA.size(); ++i)
                if (vramA[i] != vramB[i])
                {
                    if (!n)
                        first = i;
                    ++n;
                }
            if (bad <= 12)
                std::fprintf(stderr, "[gsmtl] draw-test MISMATCH draw=%u kind=%u rtz=%d psm=%u tw=%u th=%u tbw=%u tfx=%u tcc=%u lin=%d fst=%d wms=%llu wmt=%llu prim=%d iip=%d bytes=%zu first=0x%zx\n",
                             d, kind, rtz, t.psm, t.tw, t.th, t.tbw, t.tfx, t.tcc, st.linearFilter, st.prim.fst, (unsigned long long)wms,
                             (unsigned long long)wmt, st.prim.type, st.prim.iip, n, first);
            vramA = vramB; // continue from the oracle's state
            mb->MarkShadowChanged();
        }
    }
    std::fprintf(stderr, "[gsmtl] draw-test draws=%u (rtz=%llu default=%llu) mismatching=%llu\n", draws, (unsigned long long)counts[1],
                 (unsigned long long)counts[0], (unsigned long long)bad);
    {
        mb->FlushRun();
        GSMetalBackend::Stats fs = mb->TakeFrameStats();
        GSMetalBackend::Stats ts2 = mb->TotalStats();
        ts2.Add(fs);
        GSMetalPrintStats("drawtest", ts2);
    }
    return bad;
}
