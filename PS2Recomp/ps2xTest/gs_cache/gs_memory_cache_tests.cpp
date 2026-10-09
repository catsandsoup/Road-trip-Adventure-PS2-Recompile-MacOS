#include "gs_test_support.h"
#include "runtime/gs/ps2_gs_memory.h"

#include <cstring>

using namespace GSTest;

// GSMem::SwizzledSurface is the rasteriser's inlined form of the Read*/Write* handlers. These tests
// check it against the handlers (through ReadVram/WriteVram) for every storage mode, every texel of a
// page at every 256-byte base offset, odd and zero buffer widths, and the 4 MiB wrap.
namespace
{
    std::vector<uint8_t> randomVram(uint32_t seed)
    {
        std::vector<uint8_t> vram(kVramSize);
        uint32_t random = seed;
        for (auto& byte : vram)
        {
            random ^= random << 13;
            random ^= random >> 17;
            random ^= random << 5;
            byte = static_cast<uint8_t>(random);
        }
        return vram;
    }

    template<uint8_t Psm, class Check>
    uint32_t forEachCoordinate(Check&& check)
    {
        constexpr auto mode = static_cast<GSMem::PixelStorageMode>(Psm);
        constexpr auto extent = GSMem::PixelStorageTraits<mode>::PageExtent();
        uint32_t checked = 0;
        // Every local texel, for every 256-byte base offset inside an 8 KiB page.
        for (uint32_t offset = 0; offset < 32; ++offset)
            for (uint32_t y = 0; y < extent.y; ++y)
                for (uint32_t x = 0; x < extent.x; ++x, ++checked)
                    check(32 + offset, 2, x, y);

        for (uint32_t base : {0u, 31u, 32u, 12160u, 16256u, 16383u})
            for (uint32_t bw : {0u, 1u, 2u, 3u, 7u, 8u, 10u, 63u})
                for (uint32_t y : {0u, 1u, 7u, 8u, 15u, 16u, 31u, 32u, 63u, 64u, 127u, 128u, 255u, 256u, 511u, 512u, 1023u, 2047u, 4095u, 65535u})
                    for (uint32_t x : {0u, 1u, 7u, 8u, 15u, 16u, 31u, 32u, 63u, 64u, 127u, 128u, 255u, 256u, 511u, 512u, 1023u, 2047u, 4095u, 65535u})
                    {
                        check(base, bw, x, y);
                        ++checked;
                    }
        return checked;
    }

    std::string where(uint8_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y)
    {
        std::ostringstream text;
        text << "PSM=" << unsigned(psm) << " BP=" << base << " BW=" << bw << " XY=" << x << ',' << y;
        return text.str();
    }

    template<uint8_t Psm>
    void addressCoverage()
    {
        constexpr auto mode = static_cast<GSMem::PixelStorageMode>(Psm);
        BackendFixture f;
        f.vram = randomVram(0x51375A9Du);
        f.backend.Initialize(f.vram.data(), static_cast<uint32_t>(f.vram.size()));

        // Reads.
        const uint32_t reads = forEachCoordinate<Psm>([&](uint32_t base, uint32_t bw, uint32_t x, uint32_t y)
        {
            const GSMem::SwizzledSurface<mode> surface(base, bw);
            const uint32_t direct = f.backend.ReadVram(Psm, base, bw, x, y);
            const uint32_t inlined = surface.Read(f.vram.data(), x, y);
            if (inlined != direct)
                expectEqual(inlined, direct, where(Psm, base, bw, x, y));
        });

        // Writes: the same sequence through WriteVram on one copy and SwizzledSurface on another.
        std::vector<uint8_t> mirror = f.vram;
        uint32_t value = 0x9E3779B9u;
        const uint32_t writes = forEachCoordinate<Psm>([&](uint32_t base, uint32_t bw, uint32_t x, uint32_t y)
        {
            value = value * 1664525u + 1013904223u;
            const GSMem::SwizzledSurface<mode> surface(base, bw);
            f.backend.WriteVram(Psm, base, bw, x, y, value);
            surface.Write(mirror.data(), x, y, static_cast<typename GSMem::SwizzledSurface<mode>::PackedT>(value));
            const uint32_t at = surface.Locate(x, y).byteAddress & ~7u;
            if (std::memcmp(f.vram.data() + at, mirror.data() + at, 8) != 0)
                throw std::runtime_error("write differs at " + where(Psm, base, bw, x, y));
        });
        require(f.vram == mirror, "inlined writes leave local memory identical to WriteVram");
        std::cout << reads << " reads and " << writes << " writes compared\n";
    }

    // Local memory is coherent: a read always sees the latest write, whatever PSM view wrote it.
    void aliasLanes()
    {
        BackendFixture f;
        constexpr uint32_t base = 31;
        const auto read = [&](uint32_t psm)
        {
            return f.backend.ReadVram(psm, base, 2, 8, 0);
        };
        f.backend.WriteVram(GS_PSM_CT32, base, 2, 8, 0, 0xAB123456);
        expectEqual(read(GS_PSM_T8H), 0xAB, "8H lane on a crossing page");
        f.backend.WriteVram(GS_PSM_CT24, base, 2, 8, 0, 0x654321);
        expectEqual(read(GS_PSM_CT32), 0xAB654321, "CT24 upload preserves alpha");
        f.backend.WriteVram(GS_PSM_T4HL, base, 2, 8, 0, 5);
        expectEqual(read(GS_PSM_T4HL), 5, "low nibble");
        expectEqual(read(GS_PSM_T4HH), 10, "high nibble preserved");
        expectEqual(read(GS_PSM_CT24), 0x654321, "RGB plane preserved");
    }

    void physicalTagAliases()
    {
        BackendFixture f;
        f.backend.WriteVram(GS_PSM_CT32, 32, 2, 0, 0, kRed);
        expectEqual(f.backend.ReadVram(GS_PSM_CT32, 31, 2, 8, 0), kRed, "alias descriptor reads the same physical texel");
        f.backend.WriteVram(GS_PSM_CT32, 32, 2, 0, 0, kGreen);
        expectEqual(f.backend.ReadVram(GS_PSM_CT32, 31, 2, 8, 0), kGreen, "alias descriptor sees the new write");
    }
}

int main(int argc, char** argv)
{
    return run(argc, argv, {
        {"ct32", addressCoverage<GS_PSM_CT32>}, {"ct24", addressCoverage<GS_PSM_CT24>},
        {"ct16", addressCoverage<GS_PSM_CT16>}, {"ct16s", addressCoverage<GS_PSM_CT16S>},
        {"t8", addressCoverage<GS_PSM_T8>}, {"t4", addressCoverage<GS_PSM_T4>},
        {"t8h", addressCoverage<GS_PSM_T8H>}, {"t4hl", addressCoverage<GS_PSM_T4HL>},
        {"t4hh", addressCoverage<GS_PSM_T4HH>}, {"z32", addressCoverage<GS_PSM_Z32>},
        {"z24", addressCoverage<GS_PSM_Z24>}, {"z16", addressCoverage<GS_PSM_Z16>},
        {"z16s", addressCoverage<GS_PSM_Z16S>}, {"alias_lanes", aliasLanes},
        {"physical_tag_aliases", physicalTagAliases}
    });
}
