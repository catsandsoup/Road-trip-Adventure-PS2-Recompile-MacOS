// Native SPU2 model used by the SNDMOD replacement.
//
// The SNDMOD driver talks to the sound hardware exclusively through LIBSD
// (sceSdSetParam / sceSdSetSwitch / sceSdSetAddr / sceSdSetCoreAttr /
// sceSdSetEffectAttr / sceSdProcBatchEx / DMA transfers).  This class
// implements those entry points against a software SPU2: 2 MiB of sound RAM,
// 2 cores x 24 voices, PS-ADPCM decoding with loop flags, the SPU ADSR and
// volume-sweep envelope rules, per-voice dry/wet routing and the core
// mixing chain, rendered at the SPU2's native 48 kHz.
//
// Entry/parameter encodings follow the open-source PS2SDK libsd.h:
//   entry = core | (voice << 1) | register   (voice 31 == SD_VOICE_XX: all voices)
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ps2x::iop::sndmod
{
    namespace sd
    {
        // Voice parameters (sceSdSetParam).
        constexpr uint16_t VP_VOLL = 0x0000;
        constexpr uint16_t VP_VOLR = 0x0100;
        constexpr uint16_t VP_PITCH = 0x0200;
        constexpr uint16_t VP_ADSR1 = 0x0300;
        constexpr uint16_t VP_ADSR2 = 0x0400;
        constexpr uint16_t VP_ENVX = 0x0500;
        constexpr uint16_t VP_VOLXL = 0x0600;
        constexpr uint16_t VP_VOLXR = 0x0700;
        // Core parameters.
        constexpr uint16_t P_MMIX = 0x0800;
        constexpr uint16_t P_MVOLL = 0x0980;
        constexpr uint16_t P_MVOLR = 0x0a80;
        constexpr uint16_t P_EVOLL = 0x0b80;
        constexpr uint16_t P_EVOLR = 0x0c80;
        constexpr uint16_t P_AVOLL = 0x0d80;
        constexpr uint16_t P_AVOLR = 0x0e80;
        constexpr uint16_t P_BVOLL = 0x0f80;
        constexpr uint16_t P_BVOLR = 0x1080;
        // Switches (sceSdSetSwitch).
        constexpr uint16_t S_PMON = 0x1300;
        constexpr uint16_t S_NON = 0x1400;
        constexpr uint16_t S_KON = 0x1500;
        constexpr uint16_t S_KOFF = 0x1600;
        constexpr uint16_t S_ENDX = 0x1700;
        constexpr uint16_t S_VMIXL = 0x1800;
        constexpr uint16_t S_VMIXEL = 0x1900;
        constexpr uint16_t S_VMIXR = 0x1a00;
        constexpr uint16_t S_VMIXER = 0x1b00;
        // Addresses (sceSdSetAddr), byte addresses into sound RAM.
        constexpr uint16_t A_ESA = 0x1c00;
        constexpr uint16_t A_EEA = 0x1d00;
        constexpr uint16_t A_TSA = 0x1e00;
        constexpr uint16_t VA_SSA = 0x2040;
        constexpr uint16_t VA_LSAX = 0x2140;
        constexpr uint16_t VA_NAX = 0x2240;
        // Core attributes (sceSdSetCoreAttr); low bit selects the core.
        constexpr uint16_t C_EFFECT_ENABLE = 0x0002;
        constexpr uint16_t C_IRQ_ENABLE = 0x0004;
        constexpr uint16_t C_MUTE_ENABLE = 0x0006;
        constexpr uint16_t C_NOISE_CLK = 0x0008;
        constexpr uint16_t C_SPDIF_MODE = 0x000a;
        // sceSdProcBatch function codes as used by SNDMOD's static batch tables.
        constexpr uint16_t BSET_PARAM = 1;
        constexpr uint16_t BSET_SWITCH = 2;
        constexpr uint16_t BSET_ADDR = 3;
        constexpr uint16_t BSET_CORE = 4;
    }

    struct SdBatch
    {
        uint16_t func;
        uint16_t entry;
        uint32_t value;
    };

    struct SdEffectAttr
    {
        int32_t core = 0;
        int32_t mode = 0;
        int16_t depthL = 0;
        int16_t depthR = 0;
        int32_t delay = 0;
        int32_t feedback = 0;
    };

    // Per-key-on statistics for verification logs.
    struct Spu2Stats
    {
        uint64_t keyOns = 0;
        uint64_t keyOffs = 0;
        uint64_t blocksDecoded = 0;
        uint64_t loopEndsStopped = 0;
    };

    class Spu2
    {
    public:
        static constexpr uint32_t kRamBytes = 2u * 1024u * 1024u;
        static constexpr int kCores = 2;
        static constexpr int kVoicesPerCore = 24;
        static constexpr int kVoices = kCores * kVoicesPerCore;
        static constexpr uint32_t kSampleRate = 48000u;

        Spu2();

        void reset();

        // LIBSD-equivalent register interface.
        void setParam(uint16_t entry, uint16_t value);
        [[nodiscard]] uint16_t getParam(uint16_t entry) const;
        void setSwitch(uint16_t entry, uint32_t value);
        [[nodiscard]] uint32_t getSwitch(uint16_t entry) const;
        void setAddr(uint16_t entry, uint32_t value);
        void setCoreAttr(uint16_t entry, uint16_t value);
        void setEffectAttr(int core, const SdEffectAttr &attr);
        [[nodiscard]] SdEffectAttr getEffectAttr(int core) const;
        void procBatch(const SdBatch *batch, size_t count);

        // DMA into / out of sound RAM (byte addresses, wrapping at 2 MiB).
        void writeRam(uint32_t address, const uint8_t *source, size_t size);
        void fillRam(uint32_t address, uint8_t value, size_t size);
        void readRam(uint32_t address, uint8_t *destination, size_t size) const;

        // Render interleaved stereo s16 at 48 kHz.
        void render(int16_t *out, size_t frames);

        [[nodiscard]] const Spu2Stats &stats() const { return m_stats; }
        [[nodiscard]] int activeVoices() const;
        [[nodiscard]] uint32_t endxMask(int core) const;

    private:
        struct Envelope
        {
            int32_t counter = 0;
            uint32_t counterIncrement = 0;
            int32_t step = 0;
            uint8_t rate = 0;
            bool decreasing = false;
            bool exponential = false;

            void reset(uint8_t rate, uint8_t rateMask, bool decreasing, bool exponential);
            // Advances one sample; returns false once the target level is reached.
            bool tick(int16_t &level);
        };

        enum class AdsrPhase : uint8_t
        {
            Off,
            Attack,
            Decay,
            Sustain,
            Release,
        };

        struct VolumeChannel
        {
            uint16_t reg = 0;
            int16_t level = 0;
            bool sweeping = false;
            Envelope env;
            void set(uint16_t value);
            void tick();
        };

        struct Voice
        {
            uint32_t ssa = 0;
            uint32_t lsax = 0;
            uint32_t nax = 0;
            uint16_t pitch = 0;
            uint16_t adsr1 = 0;
            uint16_t adsr2 = 0;
            VolumeChannel volL;
            VolumeChannel volR;

            AdsrPhase phase = AdsrPhase::Off;
            int16_t envLevel = 0;
            Envelope env;
            int16_t sustainLevel = 0;

            uint32_t counter = 0; // 12.12 fixed point sample position within the decoded block
            std::array<int16_t, 28 + 3> samples{}; // [0..2] = tail of previous block
            int16_t hist1 = 0;
            int16_t hist2 = 0;
            uint8_t blockFlags = 0;
            bool keyOnPending = false;
            bool lsaxManual = false;
        };

        struct Core
        {
            std::array<Voice, kVoicesPerCore> voices{};
            uint32_t vmixl = 0, vmixr = 0, vmixel = 0, vmixer = 0;
            uint32_t endx = 0;
            uint32_t pmon = 0, non = 0;
            uint16_t mmix = 0;
            VolumeChannel mvolL, mvolR;
            int16_t evolL = 0, evolR = 0;
            int16_t avolL = 0, avolR = 0;
            int16_t bvolL = 0, bvolR = 0;
            uint32_t esa = 0, eea = 0;
            bool effectEnable = false;
            SdEffectAttr effect{};
            // Approximate reverb state (see AUDIO_ENGINE.md).
            std::vector<float> combL, combR, apL, apR;
            std::array<size_t, 4> combLen{}, combPos{};
            std::array<size_t, 2> apLen{}, apPos{};
            std::array<float, 8> combStore{};
            float reverbFeedback = 0.0f;
            int reverbMode = 0;
        };

        void keyOn(Core &core, Voice &voice);
        void keyOff(Voice &voice);
        void decodeBlock(Core &core, int coreIndex, int voiceIndex, Voice &voice);
        int16_t voiceSample(Core &core, int coreIndex, int voiceIndex, Voice &voice);
        void tickAdsr(Voice &voice);
        void configureReverb(Core &core);
        void processReverb(Core &core, float inL, float inR, float &outL, float &outR);

        template <typename Fn>
        void forEachVoice(uint16_t entry, Fn &&fn);

        std::vector<uint8_t> m_ram;
        std::array<Core, kCores> m_cores{};
        Spu2Stats m_stats{};
    };
}
