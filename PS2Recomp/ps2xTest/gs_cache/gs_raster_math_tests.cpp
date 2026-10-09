#include "gs_test_support.h"
#include "runtime/gs/gs_bilinear.h"

#include <cstring>

using namespace GSTest;

namespace
{
    // bilinearRgba8 must return, for every channel, exactly what lerpChannel returns.
    void bilinearMatchesScalar()
    {
        uint64_t state = 0x2545F4914F6CDD1Dull;
        const auto next = [&]()
        {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            return static_cast<uint32_t>(state >> 16);
        };
        const auto fraction = [&](uint32_t pick) -> float
        {
            // Weights the sampler produces (x - floor(x) of 12.4 fixed point and arbitrary floats),
            // plus the edges of [0, 1) and values around the rounding ties.
            switch (pick % 8u)
            {
            case 0: return 0.0f;
            case 1: return static_cast<float>(next() % 16u) / 16.0f;
            case 2: return 0.5f;
            case 3: return std::nextafter(0.5f, 0.0f);
            case 4: return std::nextafter(0.5f, 1.0f);
            case 5: return std::nextafter(1.0f, 0.0f);
            case 6: return static_cast<float>(next() % 4096u) / 4096.0f;
            default: return static_cast<float>(next() & 0xFFFFFFu) / static_cast<float>(0x1000000);
            }
        };
        uint64_t checked = 0;
        for (uint32_t i = 0; i < 4000000u; ++i)
        {
            uint32_t c[4];
            for (auto& texel : c)
            {
                texel = next();
                if ((next() & 3u) == 0u)
                    texel = (next() & 1u) ? 0xFFFFFFFFu : 0u;
            }
            const float fx = fraction(next());
            const float fy = fraction(next());
            const uint32_t vector = GSInternal::bilinearRgba8(c[0], c[1], c[2], c[3], fx, fy);
            for (uint32_t shift = 0; shift < 32u; shift += 8u)
            {
                const uint8_t scalar = GSInternal::lerpChannel(static_cast<uint8_t>(c[0] >> shift), static_cast<uint8_t>(c[1] >> shift),
                                                               static_cast<uint8_t>(c[2] >> shift), static_cast<uint8_t>(c[3] >> shift), fx, fy);
                if (scalar != static_cast<uint8_t>(vector >> shift))
                {
                    std::ostringstream error;
                    error << "texels " << std::hex << c[0] << ' ' << c[1] << ' ' << c[2] << ' ' << c[3] << std::dec
                          << " fx=" << fx << " fy=" << fy << " shift=" << shift;
                    expectEqual(static_cast<uint8_t>(vector >> shift), scalar, error.str());
                }
                ++checked;
            }
        }
        // Every channel pair at the tie weights.
        for (uint32_t a = 0; a < 256u; ++a)
            for (uint32_t b = 0; b < 256u; ++b)
                for (float w : {0.0f, 0.25f, 0.5f, 0.75f, std::nextafter(0.5f, 0.0f), std::nextafter(0.5f, 1.0f)})
                {
                    const uint32_t c00 = a * 0x01010101u, c10 = b * 0x01010101u;
                    const uint32_t vector = GSInternal::bilinearRgba8(c00, c10, c00, c10, w, w);
                    expectEqual(vector & 0xFFu, GSInternal::lerpChannel(uint8_t(a), uint8_t(b), uint8_t(a), uint8_t(b), w, w), "tie weights");
                    ++checked;
                }
        std::cout << checked << " channels compared\n";
    }
}

int main(int argc, char** argv)
{
    return run(argc, argv, {
        {"bilinear_matches_scalar", bilinearMatchesScalar},
    });
}
