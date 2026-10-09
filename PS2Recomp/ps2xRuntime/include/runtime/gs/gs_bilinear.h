#pragma once

#include "runtime/gs/ps2_gs_common.h"

#include <cmath>
#include <cstdint>

#if defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#endif

// The rasteriser's bilinear filter. lerpChannel is the reference; bilinearRgba8 filters the four
// channels of four RGBA8 texels at once and returns exactly the bytes lerpChannel returns per channel
// (checked by ps2xTest/gs_cache "bilinear_matches_scalar").
namespace GSInternal
{
    // With -ffp-contract=on clang fuses each line's multiply into its add: fma(c10 - c00, fx, c00),
    // fma(c11 - c01, fx, c01), then fma(bottom - top, fy, top); std::lround rounds half away from 0.
    inline uint8_t lerpChannel(uint8_t c00, uint8_t c10, uint8_t c01, uint8_t c11, float fx, float fy)
    {
        const float top = static_cast<float>(c00) + (static_cast<float>(c10) - static_cast<float>(c00)) * fx;
        const float bottom = static_cast<float>(c01) + (static_cast<float>(c11) - static_cast<float>(c01)) * fx;
        return clampU8(static_cast<int>(std::lround(top + (bottom - top) * fy)));
    }

    inline uint32_t bilinearRgba8(uint32_t c00, uint32_t c10, uint32_t c01, uint32_t c11, float fx, float fy)
    {
#if defined(__aarch64__) || defined(_M_ARM64)
        // The same three fused multiply-adds per channel (vfmaq_n_f32(a, b, n) = fma(b, n, a)), the
        // same round-half-away conversion (FCVTAS) and a saturating narrow in place of clampU8.
        const uint16x8_t top16 = vmovl_u8(vcreate_u8(static_cast<uint64_t>(c00) | (static_cast<uint64_t>(c10) << 32)));
        const uint16x8_t bottom16 = vmovl_u8(vcreate_u8(static_cast<uint64_t>(c01) | (static_cast<uint64_t>(c11) << 32)));
        const float32x4_t f00 = vcvtq_f32_u32(vmovl_u16(vget_low_u16(top16)));
        const float32x4_t f10 = vcvtq_f32_u32(vmovl_u16(vget_high_u16(top16)));
        const float32x4_t f01 = vcvtq_f32_u32(vmovl_u16(vget_low_u16(bottom16)));
        const float32x4_t f11 = vcvtq_f32_u32(vmovl_u16(vget_high_u16(bottom16)));
        const float32x4_t top = vfmaq_n_f32(f00, vsubq_f32(f10, f00), fx);
        const float32x4_t bottom = vfmaq_n_f32(f01, vsubq_f32(f11, f01), fx);
        const int32x4_t rounded = vcvtaq_s32_f32(vfmaq_n_f32(top, vsubq_f32(bottom, top), fy));
        const uint8x8_t bytes = vqmovn_u16(vcombine_u16(vqmovun_s32(rounded), vdup_n_u16(0)));
        return vget_lane_u32(vreinterpret_u32_u8(bytes), 0);
#else
        uint32_t out = 0u;
        for (uint32_t shift = 0; shift < 32u; shift += 8u)
        {
            out |= static_cast<uint32_t>(lerpChannel(static_cast<uint8_t>(c00 >> shift),
                                                     static_cast<uint8_t>(c10 >> shift),
                                                     static_cast<uint8_t>(c01 >> shift),
                                                     static_cast<uint8_t>(c11 >> shift),
                                                     fx, fy))
                   << shift;
        }
        return out;
#endif
    }
}
