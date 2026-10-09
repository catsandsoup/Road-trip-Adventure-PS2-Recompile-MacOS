#include "spu2.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ps2x::iop::sndmod
{
    namespace
    {
        constexpr int16_t kEnvMax = 0x7fff;
        constexpr int kAdpcmPos[5] = {0, 60, 115, 98, 122};
        constexpr int kAdpcmNeg[5] = {0, 0, -52, -55, -60};

        inline int16_t clamp16(int32_t v)
        {
            return static_cast<int16_t>(std::clamp<int32_t>(v, -32768, 32767));
        }

        inline int coreOf(uint16_t entry) { return entry & 1; }
        inline int voiceOf(uint16_t entry) { return (entry >> 1) & 0x1f; }
    }

    // ---------------------------------------------------------------- envelope
    // SPU envelope stepping (shared by ADSR and volume sweeps).  rate is the
    // 7-bit (shift << 2 | step) value; the counter/step rules are implemented
    // from the documented SPU hardware behaviour (nocash psx-spx, "SPU Volume and
    // ADSR Generator"), the same public specification other SPU emulators follow.
    void Spu2::Envelope::reset(uint8_t newRate, uint8_t rateMask, bool dec, bool expo)
    {
        rate = newRate;
        decreasing = dec;
        exponential = expo;
        counter = 0;
        counterIncrement = 0x8000u;
        const int32_t baseStep = 7 - (rate & 3);
        step = decreasing ? ~baseStep : baseStep;
        if (rate < 44)
            step <<= (11 - (rate >> 2));
        else if (rate >= 48)
            counterIncrement >>= ((rate >> 2) - 11);
        if ((rate & rateMask) == rateMask)
            counterIncrement = 0;
    }

    bool Spu2::Envelope::tick(int16_t &level)
    {
        uint32_t increment = counterIncrement;
        int32_t thisStep = step;
        if (exponential)
        {
            if (decreasing)
            {
                thisStep = (thisStep * level) >> 15;
            }
            else if (level >= 0x6000)
            {
                if (rate < 40)
                {
                    thisStep >>= 2;
                }
                else if (rate >= 44)
                {
                    increment >>= 2;
                }
                else
                {
                    thisStep >>= 1;
                    increment >>= 1;
                }
            }
        }

        counter += static_cast<int32_t>(increment);
        if (!(counter & 0x8000))
            return true;
        counter = 0;
        level = static_cast<int16_t>(std::clamp<int32_t>(level + thisStep, 0, kEnvMax));
        return decreasing ? (level > 0) : (level < kEnvMax);
    }

    void Spu2::VolumeChannel::set(uint16_t value)
    {
        reg = value;
        if (value & 0x8000u)
        {
            sweeping = true;
            env.reset(static_cast<uint8_t>(value & 0x7fu), 0x7f, (value & 0x2000u) != 0u, (value & 0x4000u) != 0u);
        }
        else
        {
            sweeping = false;
            level = static_cast<int16_t>(static_cast<uint16_t>(value << 1));
        }
    }

    void Spu2::VolumeChannel::tick()
    {
        if (sweeping)
            env.tick(level);
    }

    // ---------------------------------------------------------------- core
    Spu2::Spu2()
        : m_ram(kRamBytes, 0u)
    {
        reset();
    }

    void Spu2::reset()
    {
        std::fill(m_ram.begin(), m_ram.end(), 0u);
        for (Core &core : m_cores)
            core = Core{};
        for (Core &core : m_cores)
            configureReverb(core);
        m_stats = {};
    }

    template <typename Fn>
    void Spu2::forEachVoice(uint16_t entry, Fn &&fn)
    {
        Core &core = m_cores[coreOf(entry)];
        const int v = voiceOf(entry);
        if (v == 31) // SD_VOICE_XX
        {
            for (Voice &voice : core.voices)
                fn(voice);
        }
        else if (v < kVoicesPerCore)
        {
            fn(core.voices[static_cast<size_t>(v)]);
        }
    }

    void Spu2::setParam(uint16_t entry, uint16_t value)
    {
        const uint16_t reg = entry & 0xff80u;
        Core &core = m_cores[coreOf(entry)];
        switch (reg & 0xff00u)
        {
        case sd::VP_VOLL:
            forEachVoice(entry, [&](Voice &v) { v.volL.set(value); });
            return;
        case sd::VP_VOLR:
            forEachVoice(entry, [&](Voice &v) { v.volR.set(value); });
            return;
        case sd::VP_PITCH:
            forEachVoice(entry, [&](Voice &v) { v.pitch = value; });
            return;
        case sd::VP_ADSR1:
            forEachVoice(entry, [&](Voice &v) {
                v.adsr1 = value;
                if (v.phase == AdsrPhase::Attack)
                    v.env.reset(static_cast<uint8_t>((value >> 8) & 0x7f), 0x7f, false, (value & 0x8000u) != 0u);
                else if (v.phase == AdsrPhase::Decay)
                    v.env.reset(static_cast<uint8_t>(((value >> 4) & 0xf) << 2), 0x1f << 2, true, true);
                v.sustainLevel = static_cast<int16_t>(std::min<int32_t>(((value & 0xf) + 1) * 0x800, kEnvMax));
            });
            return;
        case sd::VP_ADSR2:
            forEachVoice(entry, [&](Voice &v) {
                v.adsr2 = value;
                if (v.phase == AdsrPhase::Sustain)
                    v.env.reset(static_cast<uint8_t>((value >> 6) & 0x7f), 0x7f, (value & 0x4000u) != 0u, (value & 0x8000u) != 0u);
                else if (v.phase == AdsrPhase::Release)
                    v.env.reset(static_cast<uint8_t>((value & 0x1f) << 2), 0x1f << 2, true, (value & 0x20u) != 0u);
            });
            return;
        case sd::VP_ENVX:
            forEachVoice(entry, [&](Voice &v) { v.envLevel = static_cast<int16_t>(value & 0x7fff); });
            return;
        default:
            break;
        }

        switch (reg)
        {
        case sd::P_MMIX:
            core.mmix = value;
            break;
        case sd::P_MVOLL:
            core.mvolL.set(value);
            break;
        case sd::P_MVOLR:
            core.mvolR.set(value);
            break;
        case sd::P_EVOLL:
            core.evolL = static_cast<int16_t>(value);
            break;
        case sd::P_EVOLR:
            core.evolR = static_cast<int16_t>(value);
            break;
        case sd::P_AVOLL:
            core.avolL = static_cast<int16_t>(value);
            break;
        case sd::P_AVOLR:
            core.avolR = static_cast<int16_t>(value);
            break;
        case sd::P_BVOLL:
            core.bvolL = static_cast<int16_t>(value);
            break;
        case sd::P_BVOLR:
            core.bvolR = static_cast<int16_t>(value);
            break;
        default:
            break;
        }
    }

    uint16_t Spu2::getParam(uint16_t entry) const
    {
        const Core &core = m_cores[coreOf(entry)];
        const int v = voiceOf(entry);
        if (v >= kVoicesPerCore)
            return 0;
        const Voice &voice = core.voices[static_cast<size_t>(v)];
        switch (entry & 0xff00u)
        {
        case sd::VP_VOLL:
            return voice.volL.reg;
        case sd::VP_VOLR:
            return voice.volR.reg;
        case sd::VP_PITCH:
            return voice.pitch;
        case sd::VP_ADSR1:
            return voice.adsr1;
        case sd::VP_ADSR2:
            return voice.adsr2;
        case sd::VP_ENVX:
            return static_cast<uint16_t>(voice.envLevel);
        case sd::VP_VOLXL:
            return static_cast<uint16_t>(voice.volL.level);
        case sd::VP_VOLXR:
            return static_cast<uint16_t>(voice.volR.level);
        default:
            return 0;
        }
    }

    void Spu2::setSwitch(uint16_t entry, uint32_t value)
    {
        Core &core = m_cores[coreOf(entry)];
        value &= 0xffffffu;
        switch (entry & 0xff00u)
        {
        case sd::S_PMON:
            core.pmon = value;
            break;
        case sd::S_NON:
            core.non = value;
            break;
        case sd::S_KON:
            for (int i = 0; i < kVoicesPerCore; ++i)
                if (value & (1u << i))
                    keyOn(core, core.voices[static_cast<size_t>(i)]);
            core.endx &= ~value;
            break;
        case sd::S_KOFF:
            for (int i = 0; i < kVoicesPerCore; ++i)
                if (value & (1u << i))
                    keyOff(core.voices[static_cast<size_t>(i)]);
            break;
        case sd::S_ENDX:
            core.endx = value;
            break;
        case sd::S_VMIXL:
            core.vmixl = value;
            break;
        case sd::S_VMIXEL:
            core.vmixel = value;
            break;
        case sd::S_VMIXR:
            core.vmixr = value;
            break;
        case sd::S_VMIXER:
            core.vmixer = value;
            break;
        default:
            break;
        }
    }

    uint32_t Spu2::getSwitch(uint16_t entry) const
    {
        const Core &core = m_cores[coreOf(entry)];
        switch (entry & 0xff00u)
        {
        case sd::S_ENDX:
            return core.endx;
        case sd::S_VMIXL:
            return core.vmixl;
        case sd::S_VMIXEL:
            return core.vmixel;
        case sd::S_VMIXR:
            return core.vmixr;
        case sd::S_VMIXER:
            return core.vmixer;
        default:
            return 0;
        }
    }

    void Spu2::setAddr(uint16_t entry, uint32_t value)
    {
        value &= (kRamBytes - 1u);
        Core &core = m_cores[coreOf(entry)];
        switch (entry & 0xffc0u)
        {
        case sd::VA_SSA:
            forEachVoice(entry, [&](Voice &v) { v.ssa = value & ~0xfu; });
            return;
        case sd::VA_LSAX:
            forEachVoice(entry, [&](Voice &v) {
                v.lsax = value & ~0xfu;
                v.lsaxManual = true;
            });
            return;
        case sd::VA_NAX:
            forEachVoice(entry, [&](Voice &v) { v.nax = value & ~0xfu; });
            return;
        default:
            break;
        }
        switch (entry & 0xff00u)
        {
        case sd::A_ESA:
            core.esa = value;
            break;
        case sd::A_EEA:
            core.eea = value;
            break;
        default:
            break;
        }
    }

    void Spu2::setCoreAttr(uint16_t entry, uint16_t value)
    {
        Core &core = m_cores[coreOf(entry)];
        if ((entry & 0xfffeu) == sd::C_EFFECT_ENABLE)
            core.effectEnable = value != 0u;
    }

    void Spu2::setEffectAttr(int coreIndex, const SdEffectAttr &attr)
    {
        Core &core = m_cores[static_cast<size_t>(coreIndex & 1)];
        core.effect = attr;
        core.effect.core = coreIndex & 1;
        core.effect.mode &= 0xff; // SD_REV_MODE_CLEAR_WA (0x100) is an action flag
        core.evolL = attr.depthL;
        core.evolR = attr.depthR;
        configureReverb(core);
    }

    SdEffectAttr Spu2::getEffectAttr(int coreIndex) const
    {
        return m_cores[static_cast<size_t>(coreIndex & 1)].effect;
    }

    void Spu2::procBatch(const SdBatch *batch, size_t count)
    {
        for (size_t i = 0; i < count; ++i)
        {
            const SdBatch &b = batch[i];
            switch (b.func)
            {
            case sd::BSET_PARAM:
                setParam(b.entry, static_cast<uint16_t>(b.value));
                break;
            case sd::BSET_SWITCH:
                setSwitch(b.entry, b.value);
                break;
            case sd::BSET_ADDR:
                setAddr(b.entry, b.value);
                break;
            case sd::BSET_CORE:
                setCoreAttr(b.entry, static_cast<uint16_t>(b.value));
                break;
            default:
                break;
            }
        }
    }

    void Spu2::writeRam(uint32_t address, const uint8_t *source, size_t size)
    {
        for (size_t i = 0; i < size; ++i)
            m_ram[(address + i) & (kRamBytes - 1u)] = source[i];
    }

    void Spu2::fillRam(uint32_t address, uint8_t value, size_t size)
    {
        for (size_t i = 0; i < size; ++i)
            m_ram[(address + i) & (kRamBytes - 1u)] = value;
    }

    void Spu2::readRam(uint32_t address, uint8_t *destination, size_t size) const
    {
        for (size_t i = 0; i < size; ++i)
            destination[i] = m_ram[(address + i) & (kRamBytes - 1u)];
    }

    int Spu2::activeVoices() const
    {
        int n = 0;
        for (const Core &core : m_cores)
            for (const Voice &v : core.voices)
                n += v.phase != AdsrPhase::Off ? 1 : 0;
        return n;
    }

    uint32_t Spu2::endxMask(int core) const
    {
        return m_cores[static_cast<size_t>(core & 1)].endx;
    }

    // ---------------------------------------------------------------- voices
    void Spu2::keyOn(Core &core, Voice &voice)
    {
        (void)core;
        ++m_stats.keyOns;
        voice.nax = voice.ssa;
        voice.lsaxManual = false;
        voice.counter = 0;
        voice.hist1 = 0;
        voice.hist2 = 0;
        voice.samples.fill(0);
        voice.envLevel = 0;
        voice.phase = AdsrPhase::Attack;
        voice.env.reset(static_cast<uint8_t>((voice.adsr1 >> 8) & 0x7f), 0x7f, false, (voice.adsr1 & 0x8000u) != 0u);
        voice.sustainLevel = static_cast<int16_t>(std::min<int32_t>(((voice.adsr1 & 0xf) + 1) * 0x800, kEnvMax));
        voice.keyOnPending = true; // first block decoded on the next rendered sample
    }

    void Spu2::keyOff(Voice &voice)
    {
        if (voice.phase == AdsrPhase::Off || voice.phase == AdsrPhase::Release)
            return;
        ++m_stats.keyOffs;
        voice.phase = AdsrPhase::Release;
        voice.env.reset(static_cast<uint8_t>((voice.adsr2 & 0x1f) << 2), 0x1f << 2, true, (voice.adsr2 & 0x20u) != 0u);
    }

    void Spu2::decodeBlock(Core &core, int coreIndex, int voiceIndex, Voice &voice)
    {
        (void)coreIndex;
        const uint32_t addr = voice.nax & (kRamBytes - 1u);
        const uint8_t header = m_ram[addr];
        const uint8_t flags = m_ram[(addr + 1u) & (kRamBytes - 1u)];
        int shift = header & 0x0f;
        if (shift > 12)
            shift = 9;
        int filter = (header >> 4) & 0x07;
        if (filter > 4)
            filter = 4;
        const int f0 = kAdpcmPos[filter];
        const int f1 = kAdpcmNeg[filter];

        // Keep the last three decoded samples for interpolation.
        voice.samples[0] = voice.samples[28];
        voice.samples[1] = voice.samples[29];
        voice.samples[2] = voice.samples[30];
        for (int i = 0; i < 28; ++i)
        {
            const uint8_t byte = m_ram[(addr + 2u + static_cast<uint32_t>(i >> 1)) & (kRamBytes - 1u)];
            const int nibble = (i & 1) ? (byte >> 4) : (byte & 0x0f);
            int32_t sample = static_cast<int16_t>(static_cast<uint16_t>(nibble << 12)) >> shift;
            sample += (voice.hist1 * f0 + voice.hist2 * f1 + 32) >> 6;
            const int16_t s = clamp16(sample);
            voice.hist2 = voice.hist1;
            voice.hist1 = s;
            voice.samples[static_cast<size_t>(i + 3)] = s;
        }
        voice.blockFlags = flags;
        if ((flags & 0x04u) && !voice.lsaxManual)
            voice.lsax = addr;
        ++m_stats.blocksDecoded;
        (void)core;
        (void)voiceIndex;
    }

    void Spu2::tickAdsr(Voice &voice)
    {
        switch (voice.phase)
        {
        case AdsrPhase::Attack:
            if (!voice.env.tick(voice.envLevel))
            {
                voice.phase = AdsrPhase::Decay;
                voice.env.reset(static_cast<uint8_t>(((voice.adsr1 >> 4) & 0xf) << 2), 0x1f << 2, true, true);
            }
            break;
        case AdsrPhase::Decay:
            voice.env.tick(voice.envLevel);
            if (voice.envLevel <= voice.sustainLevel)
            {
                voice.phase = AdsrPhase::Sustain;
                voice.env.reset(static_cast<uint8_t>((voice.adsr2 >> 6) & 0x7f), 0x7f, (voice.adsr2 & 0x4000u) != 0u, (voice.adsr2 & 0x8000u) != 0u);
            }
            break;
        case AdsrPhase::Sustain:
            voice.env.tick(voice.envLevel);
            break;
        case AdsrPhase::Release:
            voice.env.tick(voice.envLevel);
            if (voice.envLevel <= 0)
            {
                voice.envLevel = 0;
                voice.phase = AdsrPhase::Off;
            }
            break;
        case AdsrPhase::Off:
            break;
        }
    }

    int16_t Spu2::voiceSample(Core &core, int coreIndex, int voiceIndex, Voice &voice)
    {
        if (voice.keyOnPending)
        {
            voice.keyOnPending = false;
            decodeBlock(core, coreIndex, voiceIndex, voice);
        }

        // 4-point cubic (Catmull-Rom) interpolation between samples n-2 and n-1;
        // the hardware uses a 4-tap Gaussian table over the same window.
        const uint32_t index = voice.counter >> 12;
        const float t = static_cast<float>(voice.counter & 0xfffu) * (1.0f / 4096.0f);
        const float p0 = voice.samples[index + 0];
        const float p1 = voice.samples[index + 1];
        const float p2 = voice.samples[index + 2];
        const float p3 = voice.samples[index + 3];
        const float out = p1 + 0.5f * t * (p2 - p0 + t * (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3 + t * (3.0f * (p1 - p2) + p3 - p0)));
        const int32_t sample = clamp16(static_cast<int32_t>(std::lrint(out)));

        uint32_t step = voice.pitch;
        if (step > 0x3fffu)
            step = 0x3fffu;
        voice.counter += step;
        while ((voice.counter >> 12) >= 28u)
        {
            voice.counter -= 28u << 12;
            // Leaving the current block: honour its loop flags.
            const uint8_t flags = voice.blockFlags;
            if (flags & 0x01u)
            {
                core.endx |= 1u << voiceIndex;
                voice.nax = voice.lsax;
                if (!(flags & 0x02u))
                {
                    ++m_stats.loopEndsStopped;
                    voice.phase = AdsrPhase::Off;
                    voice.envLevel = 0;
                }
            }
            else
            {
                voice.nax = (voice.nax + 16u) & (kRamBytes - 1u);
            }
            decodeBlock(core, coreIndex, voiceIndex, voice);
        }
        return static_cast<int16_t>(sample);
    }

    // ---------------------------------------------------------------- reverb
    // Approximation: the SPU2 reverb is a fixed-point network driven by
    // per-mode register presets.  Here a small Schroeder network (4 combs +
    // 2 allpasses per side) is tuned per libsd mode.  See AUDIO_ENGINE.md.
    void Spu2::configureReverb(Core &core)
    {
        const int mode = core.effect.mode & 0xff;
        core.reverbMode = mode;
        struct Preset
        {
            float scale;
            float feedback;
        };
        static constexpr Preset kPresets[10] = {
            {0.0f, 0.0f},   // OFF
            {0.35f, 0.55f}, // ROOM
            {0.30f, 0.50f}, // STUDIO_A
            {0.50f, 0.60f}, // STUDIO_B
            {0.70f, 0.66f}, // STUDIO_C
            {1.00f, 0.76f}, // HALL
            {1.40f, 0.84f}, // SPACE
            {2.00f, 0.50f}, // ECHO
            {2.00f, 0.00f}, // DELAY
            {0.25f, 0.70f}, // PIPE
        };
        const Preset p = kPresets[(mode >= 0 && mode <= 9) ? mode : 0];
        core.reverbFeedback = p.feedback;
        static constexpr size_t kCombBase[4] = {1557, 1617, 1491, 1422};
        static constexpr size_t kApBase[2] = {556, 225};
        size_t total = 0;
        for (size_t i = 0; i < 4; ++i)
        {
            core.combLen[i] = std::max<size_t>(16, static_cast<size_t>(static_cast<float>(kCombBase[i]) * p.scale * 2.0f));
            core.combPos[i] = 0;
            total += core.combLen[i];
        }
        if (mode == 7 || mode == 8)
        {
            // ECHO / DELAY use the delay attribute (0..127) as the echo time.
            const size_t len = static_cast<size_t>(std::clamp(core.effect.delay, 1, 127)) * 375u; // ~7.8 ms units
            for (size_t i = 0; i < 4; ++i)
                core.combLen[i] = len;
            core.reverbFeedback = mode == 7 ? static_cast<float>(std::clamp(core.effect.feedback, 0, 127)) / 160.0f : 0.0f;
            total = len * 4;
        }
        core.combL.assign(total, 0.0f);
        core.combR.assign(total, 0.0f);
        core.combStore.fill(0.0f);
        size_t apTotal = 0;
        for (size_t i = 0; i < 2; ++i)
        {
            core.apLen[i] = kApBase[i];
            core.apPos[i] = 0;
            apTotal += kApBase[i];
        }
        core.apL.assign(apTotal, 0.0f);
        core.apR.assign(apTotal, 0.0f);
    }

    void Spu2::processReverb(Core &core, float inL, float inR, float &outL, float &outR)
    {
        outL = 0.0f;
        outR = 0.0f;
        if (core.reverbMode <= 0 || core.combL.empty())
            return;
        size_t base = 0;
        const bool delayOnly = core.reverbMode == 7 || core.reverbMode == 8;
        const int combs = delayOnly ? 1 : 4;
        for (int i = 0; i < combs; ++i)
        {
            const size_t len = core.combLen[static_cast<size_t>(i)];
            size_t &pos = core.combPos[static_cast<size_t>(i)];
            float &yl = core.combL[base + pos];
            float &yr = core.combR[base + pos];
            const float dl = yl;
            const float dr = yr;
            yl = inL + dl * core.reverbFeedback;
            yr = inR + dr * core.reverbFeedback;
            outL += dl;
            outR += dr;
            pos = (pos + 1) % len;
            base += len;
        }
        if (delayOnly)
            return;
        outL *= 0.25f;
        outR *= 0.25f;
        base = 0;
        for (size_t i = 0; i < 2; ++i)
        {
            const size_t len = core.apLen[i];
            size_t &pos = core.apPos[i];
            float &bl = core.apL[base + pos];
            float &br = core.apR[base + pos];
            const float yl = bl - 0.5f * outL;
            const float yr = br - 0.5f * outR;
            bl = outL + 0.5f * yl;
            br = outR + 0.5f * yr;
            outL = yl;
            outR = yr;
            pos = (pos + 1) % len;
            base += len;
        }
    }

    // ---------------------------------------------------------------- mixing
    void Spu2::render(int16_t *out, size_t frames)
    {
        for (size_t f = 0; f < frames; ++f)
        {
            int32_t extL = 0;
            int32_t extR = 0;
            int32_t finalL = 0;
            int32_t finalR = 0;
            for (int c = 0; c < kCores; ++c)
            {
                Core &core = m_cores[static_cast<size_t>(c)];
                int32_t dryL = 0, dryR = 0, wetL = 0, wetR = 0;
                for (int v = 0; v < kVoicesPerCore; ++v)
                {
                    Voice &voice = core.voices[static_cast<size_t>(v)];
                    voice.volL.tick();
                    voice.volR.tick();
                    if (voice.phase == AdsrPhase::Off)
                        continue;
                    const int32_t raw = voiceSample(core, c, v, voice);
                    const int32_t enveloped = (raw * voice.envLevel) >> 15;
                    tickAdsr(voice);
                    const int32_t l = (enveloped * voice.volL.level) >> 15;
                    const int32_t r = (enveloped * voice.volR.level) >> 15;
                    const uint32_t bit = 1u << v;
                    if (core.vmixl & bit)
                        dryL += l;
                    if (core.vmixr & bit)
                        dryR += r;
                    if (core.vmixel & bit)
                        wetL += l;
                    if (core.vmixer & bit)
                        wetR += r;
                }

                // MMIX gates: bit11/10 voice dry L/R, bit9/8 voice wet L/R,
                // bit3/2 external (core0 -> core1) dry L/R, bit1/0 external wet.
                const uint16_t mmix = core.mmix;
                int32_t mixL = (mmix & 0x800u) ? dryL : 0;
                int32_t mixR = (mmix & 0x400u) ? dryR : 0;
                int32_t effL = (mmix & 0x200u) ? wetL : 0;
                int32_t effR = (mmix & 0x100u) ? wetR : 0;
                if (c == 1)
                {
                    const int32_t inL = (extL * core.bvolL) >> 15;
                    const int32_t inR = (extR * core.bvolR) >> 15;
                    if (mmix & 0x8u)
                        mixL += inL;
                    if (mmix & 0x4u)
                        mixR += inR;
                    if (mmix & 0x2u)
                        effL += inL;
                    if (mmix & 0x1u)
                        effR += inR;
                }
                if (core.effectEnable && core.reverbMode > 0)
                {
                    float rl = 0.0f, rr = 0.0f;
                    processReverb(core, static_cast<float>(effL), static_cast<float>(effR), rl, rr);
                    mixL += (static_cast<int32_t>(rl) * core.evolL) >> 15;
                    mixR += (static_cast<int32_t>(rr) * core.evolR) >> 15;
                }
                core.mvolL.tick();
                core.mvolR.tick();
                const int32_t outL = clamp16((clamp16(mixL) * core.mvolL.level) >> 15);
                const int32_t outR = clamp16((clamp16(mixR) * core.mvolR.level) >> 15);
                if (c == 0)
                {
                    extL = outL;
                    extR = outR;
                }
                else
                {
                    finalL = outL;
                    finalR = outR;
                }
            }
            out[f * 2 + 0] = clamp16(finalL);
            out[f * 2 + 1] = clamp16(finalR);
        }
    }
}
