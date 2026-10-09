// Native port of SNDMOD.IRX (sndmod.c).  Each function below corresponds to the
// IRX function of the same name; the IRX virtual address is given in brackets.
// Deviations from the original are marked "NATIVE:" (bounds checks where the
// original would index out of its static arrays, and host I/O plumbing).
#include "sndmod_driver.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstring>

namespace ps2x::iop::sndmod
{
    namespace
    {
        // Driver data tables. They are read at start-up from the user's own SNDMOD.IRX (.data
        // section) by loadDriverTablesFromIrx(); nothing from the disc is compiled into this file.
        //   end_dat  [.data 0x8b60]  4 bytes
        //   bats.24  [.data 0x8b64]  sddrInitSddr's sceSdProcBatchEx table, 46 x {u16 func, u16 entry, u32 value}
        //   bats.27  [.data 0x8cd4]  sddrStopSddr's table, 6 entries
        //   wave.168 [.data 0x8d04]  procEngine {mute, loud} tone pairs, 8 x 2 int32
        //   data.169 [.data 0x8d44]  procEngine rpm -> pitch curve {rpms, btms, tops}, 5 x 3 int32
        SdBatch kBatsInit[46] = {};
        SdBatch kBatsStop[6] = {};
        int32_t kEngineWave[8][2] = {};
        int32_t kEngineCurve[5][3] = {};
        uint8_t kEndDat[4] = {};
        bool g_tablesLoaded = false;

        inline int32_t rdWord(const uint8_t *p, size_t index)
        {
            int32_t v;
            std::memcpy(&v, p + index * 4, 4);
            return v;
        }
    }

    bool loadDriverTablesFromIrx(const uint8_t *irx, size_t size)
    {
        // IRX = ELF with one loadable segment (vaddr 0); map module addresses to file offsets.
        if (!irx || size < 0x34 || std::memcmp(irx, "\x7f" "ELF", 4) != 0)
            return false;
        uint32_t phoff = 0;
        uint16_t phentsize = 0, phnum = 0;
        std::memcpy(&phoff, irx + 0x1c, 4);
        std::memcpy(&phentsize, irx + 0x2a, 2);
        std::memcpy(&phnum, irx + 0x2c, 2);
        uint32_t segOffset = 0, segVaddr = 0, segFilesz = 0;
        bool found = false;
        for (uint16_t i = 0; i < phnum && phoff + (i + 1u) * phentsize <= size; ++i)
        {
            const uint8_t *ph = irx + phoff + i * phentsize;
            uint32_t type = 0;
            std::memcpy(&type, ph, 4);
            if (type != 1u)
                continue;
            std::memcpy(&segOffset, ph + 4, 4);
            std::memcpy(&segVaddr, ph + 8, 4);
            std::memcpy(&segFilesz, ph + 16, 4);
            found = true;
            break;
        }
        if (!found)
            return false;
        const auto copyFrom = [&](uint32_t vaddr, void *dst, size_t bytes) -> bool
        {
            if (vaddr < segVaddr || vaddr + bytes > segVaddr + segFilesz)
                return false;
            const size_t off = static_cast<size_t>(segOffset) + (vaddr - segVaddr);
            if (off + bytes > size)
                return false;
            std::memcpy(dst, irx + off, bytes);
            return true;
        };
        static_assert(sizeof(SdBatch) == 8, "SdBatch must match the IRX layout");
        const bool ok = copyFrom(0x8b60u, kEndDat, sizeof(kEndDat)) &&
                        copyFrom(0x8b64u, kBatsInit, sizeof(kBatsInit)) &&
                        copyFrom(0x8cd4u, kBatsStop, sizeof(kBatsStop)) &&
                        copyFrom(0x8d04u, kEngineWave, sizeof(kEngineWave)) &&
                        copyFrom(0x8d44u, kEngineCurve, sizeof(kEngineCurve));
        g_tablesLoaded = ok;
        return ok;
    }

    bool driverTablesLoaded() { return g_tablesLoaded; }

    uint16_t SndmodDriver::freqtblEntry(uint32_t index)
    {
        if (index < 73u)
        {
            const double v = std::floor(4096.0 * std::pow(2.0, (static_cast<double>(index) - 48.0) / 12.0) + 1e-9);
            return static_cast<uint16_t>(std::min(v, 16383.0));
        }
        // NATIVE: keys 73..95 read past freqtbl into end_dat / bats.24 in the IRX.
        // Reproduce those halfwords so out-of-table keys behave identically.
        const uint32_t k = index - 73u;
        if (k == 0u)
            return 0x00ff;
        if (k == 1u)
            return 0x0000;
        const uint32_t half = k - 2u;
        const SdBatch &b = kBatsInit[(half / 4u) % 46u];
        switch (half % 4u)
        {
        case 0u:
            return b.func;
        case 1u:
            return b.entry;
        case 2u:
            return static_cast<uint16_t>(b.value & 0xffffu);
        default:
            return static_cast<uint16_t>(b.value >> 16);
        }
    }

    SndmodDriver::SndmodDriver(Spu2 &spu, SndmodIo &io)
        : m_spu(spu), m_io(io), tsqbuf(kTsqBufBytes, 0u)
    {
    }

    void SndmodDriver::trace(const char *fmt, ...)
    {
        if (!m_trace)
            return;
        char line[512];
        const int head = std::snprintf(line, sizeof(line), "%llu ", static_cast<unsigned long long>(m_ticks));
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(line + head, sizeof(line) - static_cast<size_t>(head), fmt, args);
        va_end(args);
        m_trace(line);
    }

    // ------------------------------------------------------------------ memory
    uint8_t SndmodDriver::rd8(uint32_t address) const
    {
        if (address < kTsqBufBytes)
            return tsqbuf[address];
        if (address >= kEndDatAddr && address < kEndDatAddr + 4u)
            return kEndDat[address - kEndDatAddr];
        return 0xff; // NATIVE: wild sequence pointers read as end-of-data
    }

    uint16_t SndmodDriver::rd16(uint32_t address) const
    {
        return static_cast<uint16_t>(rd8(address) | (rd8(address + 1u) << 8));
    }

    uint8_t SndmodDriver::fetch8(MUWORK *ix)
    {
        const uint8_t b = rd8(ix->data_adr);
        ix->data_adr += 1u;
        return b;
    }

    // libsd voice entry: ((ch % 24) << 1) | (ch / 24)
    uint16_t SndmodDriver::voiceEntry(int32_t channel)
    {
        return static_cast<uint16_t>((((channel % 24) << 1) | (channel / 24)) & 0xffff);
    }

    // ------------------------------------------------------------------ easy*
    // easyFileSize [0x128]
    int32_t SndmodDriver::easyFileSize(const SDDRfile &file)
    {
        if (file.size != 0u)
            return static_cast<int32_t>(file.size << 11);
        char name[53];
        std::memcpy(name, file.file, 52);
        name[52] = '\0';
        return m_io.hostFileSize(name);
    }

    // easyFileRead [0x1d4]
    bool SndmodDriver::easyFileRead(const SDDRfile &file, uint32_t fofs, uint32_t flen, uint8_t *adrs)
    {
        if (file.size != 0u)
        {
            (void)fofs;
            (void)flen;
            return m_io.readSectors(file.lsn, file.size, adrs); // sceCdRead(lsn, size, adrs, &cd_r_mode)
        }
        char name[53];
        std::memcpy(name, file.file, 52);
        name[52] = '\0';
        return m_io.readHostFile(name, fofs, flen, adrs);
    }

    // progInitWork [0x2b4]
    void SndmodDriver::progInitWork(int mode)
    {
        if (mode != 0 && mode != 1)
            return;
        if (mode == 0)
        {
            sd_s_adsr_flag = 0;
            sd_s_adsr1 = 0x13ff;
            sd_s_adsr2 = 0x5fc5;
            mono_flag = 0;
            for (int cont = 0; cont < 2; ++cont)
            {
                sd_s_vmix_mute[cont] = 0;
                sd_s_vmix[cont] = 0;
                sd_s_vmixe[cont] = 0;
            }
        }
        chwork = {};
        muwork = {};
        sework = {};
        diwork = {};
        for (int cont = 0; cont < 48; ++cont)
            chwork[cont].channel = cont;
        bgmset = 0;
        bgmbuf = 0;
        fade_flag = 0;
        fade_data = 0;
    }

    // progSpu2Zero [0x498]: zero all of sound RAM in 0x20000-byte DMA chunks.
    void SndmodDriver::progSpu2Zero()
    {
        m_spu.fillRam(0, 0, Spu2::kRamBytes);
    }

    // isFileLoad [0x590]
    int SndmodDriver::isFileLoad(int bank)
    {
        if (bank < 0 || bank >= 4) // NATIVE: bounds (IRX indexes past loadTvbf)
            return 0;
        if (sddrwork.loadTvbf[bank].load != 2)
            return 0;
        if (sddrwork.loadTsqf[bank].load != 2)
            return 0;
        return 1;
    }

    // isWorkFree [0x640]
    int SndmodDriver::isWorkFree(int mode)
    {
        const std::array<MUWORK, 48> &mu = mode == 0 ? muwork : sework;
        for (int i = 8; i < 48; ++i)
            if (mu[i].active != 0)
                return 0;
        return 1;
    }

    // vol_Engine [0x70c]
    void SndmodDriver::vol_Engine()
    {
        for (int play = 0; play < 2; ++play)
        {
            const SDDRengine &self = sddrwork.engine[play];
            int32_t voll = self.memo_voll;
            int32_t volr = self.memo_volr;
            if (mono_flag != 0)
            {
                volr = (voll + volr) / 2;
                voll = volr;
            }
            m_spu.setParam(voiceEntry(play * 2) | sd::VP_VOLL, static_cast<uint16_t>(voll));
            m_spu.setParam(voiceEntry(play * 2) | sd::VP_VOLR, static_cast<uint16_t>(volr));
            m_spu.setParam(voiceEntry(play * 2 + 1) | sd::VP_VOLL, static_cast<uint16_t>(voll));
            m_spu.setParam(voiceEntry(play * 2 + 1) | sd::VP_VOLR, static_cast<uint16_t>(volr));
        }
    }

    // vol_Radio [0xa20]: radio voices 4/5 carry the left stream, 6/7 the right.
    void SndmodDriver::vol_Radio()
    {
        const SDDRradio &self = sddrwork.radio;
        int32_t voll = self.memo_voll;
        int32_t volr = self.memo_volr;
        if (mono_flag != 0)
        {
            volr = (voll + volr) / 2;
            voll = volr;
            m_spu.setParam(8, static_cast<uint16_t>(voll));
            m_spu.setParam(0x108, static_cast<uint16_t>(volr));
            m_spu.setParam(10, static_cast<uint16_t>(voll));
            m_spu.setParam(0x10a, static_cast<uint16_t>(volr));
            m_spu.setParam(12, static_cast<uint16_t>(voll));
            m_spu.setParam(0x10c, static_cast<uint16_t>(volr));
            m_spu.setParam(14, static_cast<uint16_t>(voll));
            m_spu.setParam(0x10e, static_cast<uint16_t>(volr));
        }
        else
        {
            m_spu.setParam(8, static_cast<uint16_t>(voll));
            m_spu.setParam(0x108, 0);
            m_spu.setParam(10, static_cast<uint16_t>(voll));
            m_spu.setParam(0x10a, 0);
            m_spu.setParam(12, 0);
            m_spu.setParam(0x10c, static_cast<uint16_t>(volr));
            m_spu.setParam(14, 0);
            m_spu.setParam(0x10e, static_cast<uint16_t>(volr));
        }
    }

    // ------------------------------------------------------------------ RPC
    void SndmodDriver::start()
    {
        // start [0x86f8]: sceSdInit(0); memset(&sddrwork, 0, sizeof sddrwork).
        m_spu.reset();
        std::memset(&sddrwork, 0, sizeof(sddrwork));
    }

    // sddrInitSddr [0xbfc]
    void SndmodDriver::sddrInitSddr()
    {
        SdEffectAttr attr{};
        m_spu.setAddr(sd::A_EEA | 0, 0x1dffff);
        attr.core = 0;
        attr.mode = 0x100;
        attr.depthL = 0x7fff;
        attr.depthR = 0x7fff;
        attr.delay = 63;
        attr.feedback = 63;
        m_spu.setEffectAttr(0, attr);
        m_spu.setCoreAttr(sd::C_EFFECT_ENABLE | 0, 1);
        m_spu.setAddr(sd::A_EEA | 1, 0x1fffff);
        attr.core = 1;
        m_spu.setEffectAttr(1, attr);
        m_spu.setCoreAttr(sd::C_EFFECT_ENABLE | 1, 1);
        m_spu.procBatch(kBatsInit, 46);
    }

    // sddrStopSddr [0xd28]
    void SndmodDriver::sddrStopSddr()
    {
        m_spu.procBatch(kBatsStop, 6);
    }

    // sddrTvbfAdrs [0xe10]
    void SndmodDriver::sddrTvbfAdrs(const int32_t *sour)
    {
        if (sour[0] >= 0 && sour[0] < 4) // NATIVE: bounds
            tvbtbl[sour[0]] = sour[1];
    }

    // sddrTsqfAdrs [0xe8c]: tsqtbl[n] = &tsqbuf[byte offset]
    void SndmodDriver::sddrTsqfAdrs(const int32_t *sour)
    {
        if (sour[0] >= 0 && sour[0] < 4)
            tsqtbl[sour[0]] = static_cast<uint32_t>(sour[1]);
    }

    // sddrTvbfTrns [0xf14]
    void SndmodDriver::sddrTvbfTrns(const uint8_t *gets)
    {
        const int32_t bank = rdWord(gets, 0);
        if (bank < 0 || bank >= 4)
            return;
        SDDRload &load = sddrwork.loadTvbf[bank];
        if (std::memcmp(&load.file, gets, sizeof(SDDRfile)) != 0)
        {
            load.load = 1;
            std::memcpy(&load.file, gets, sizeof(SDDRfile));
            trace("tvbf_req bank=%d lsn=%u sectors=%u", bank, load.file.lsn, load.file.size);
        }
    }

    // sddrTsqfTrns [0x102c]
    void SndmodDriver::sddrTsqfTrns(const uint8_t *gets)
    {
        const int32_t bank = rdWord(gets, 0);
        if (bank < 0 || bank >= 4)
            return;
        SDDRload &load = sddrwork.loadTsqf[bank];
        if (std::memcmp(&load.file, gets, sizeof(SDDRfile)) != 0)
        {
            load.load = 1;
            std::memcpy(&load.file, gets, sizeof(SDDRfile));
            trace("tsqf_req bank=%d lsn=%u sectors=%u", bank, load.file.lsn, load.file.size);
        }
    }

    // sddrCtrlMono [0x1144]
    void SndmodDriver::sddrCtrlMono(const int32_t *sour, int32_t *dest)
    {
        if (sour[0] == 0)
        {
            mono_flag = sour[1];
            vol_Engine();
            vol_Radio();
        }
        else if (sour[0] == 1)
        {
            dest[0] = mono_flag;
        }
    }

    // sddrCtrlData [0x1234]
    void SndmodDriver::sddrCtrlData(const int32_t *sour)
    {
        if (sour[0] == 0)
            set_volume(sour[1], sour[2]);
        else if (sour[0] == 1)
            set_reverb(sour[1], sour[2]);
    }

    // sddrMuteData [0x12fc]
    void SndmodDriver::sddrMuteData(const int32_t *sour)
    {
        switch (sour[0])
        {
        case 0:
            sd_s_vmix_mute[0] &= ~static_cast<uint32_t>(sour[1]);
            sd_s_vmix_mute[1] &= ~static_cast<uint32_t>(sour[2]);
            break;
        case 1:
            sd_s_vmix_mute[0] |= static_cast<uint32_t>(sour[1]);
            sd_s_vmix_mute[1] |= static_cast<uint32_t>(sour[2]);
            break;
        case 2:
            sd_s_vmix_mute[0] = static_cast<uint32_t>(sour[1]);
            sd_s_vmix_mute[1] = static_cast<uint32_t>(sour[2]);
            break;
        default:
            break;
        }
    }

    // sddrAdsrData [0x1478]
    void SndmodDriver::sddrAdsrData(const int32_t *sour)
    {
        switch (sour[0])
        {
        case 0:
            sd_s_adsr_flag = 0;
            break;
        case 1:
            sd_s_adsr_flag = 1;
            break;
        case 2:
            sd_s_adsr_flag = 1;
            sd_s_adsr1 = sour[1];
            sd_s_adsr2 = sour[2];
            break;
        default:
            break;
        }
    }

    // sddrHardData [0x157c]: copy a DIWORK into diwork[di->channel]
    void SndmodDriver::sddrHardData(const uint8_t *gets)
    {
        const int32_t channel = rdWord(gets, 1);
        if (channel < 0 || channel >= 48)
            return;
        std::memcpy(&diwork[channel], gets, sizeof(DIWORK));
    }

    // sddrSoftData [0x1614]: one packed request word.
    //   bit15 set -> set_cancel(data & 0xfff)
    //   else bank = (data >> 8) & 0xf, index = data & 0xff; directory entry
    //   tsqtbl[bank] + index*4 = {u16 prio, u16 offset}; prio 0 = music.
    //   data >> 16 = pan word (pan_L << 8 | pan_R) for SE requests.
    void SndmodDriver::sddrSoftData(const int32_t *sour)
    {
        const uint32_t data = static_cast<uint32_t>(sour[0]);
        if (data & 0x8000u)
        {
            trace("se_cancel %03x", data & 0xfffu);
            set_cancel(static_cast<int32_t>(data & 0xfffu));
            return;
        }
        const uint32_t bank = (data & 0xf00u) >> 8;
        uint32_t adrs = tsqtbl[bank & 3u] + ((data & 0xffu) << 2);
        if (bank >= 4u)
            adrs = kEndDatAddr + 8u; // NATIVE: tsqtbl has 4 entries
        const uint16_t prio = rd16(adrs);
        adrs += 2u;
        if (prio != 0u)
        {
            if (isFileLoad(static_cast<int>(bank)) != 0)
            {
                trace("se_req %03x pri=%u pan=%04x", data & 0xfffu, prio, data >> 16);
                reqse(prio, adrs, static_cast<int32_t>(data & 0xfffu), static_cast<int32_t>(data >> 16));
            }
            else
            {
                trace("se_drop %03x (bank %u not loaded)", data & 0xfffu, bank);
            }
        }
        else
        {
            trace("bgm_req %03x", data & 0xfffu);
            set_cancel_mode(0);
            bgmbuf = data & 0xfffu;
            bgmset = 2;
        }
    }

    // sddrSongData [0x17ac]: BGM transport (jump table @ .rodata 0x8aa0).
    void SndmodDriver::sddrSongData(const int32_t *sour)
    {
        const uint32_t cmd = static_cast<uint32_t>(sour[0]);
        if (cmd >= 7u)
            return;
        trace("song cmd=%u arg=%08x", cmd, static_cast<uint32_t>(sour[1]));
        switch (cmd)
        {
        case 0: // select
            set_cancel_mode(0);
            bgmbuf = static_cast<uint32_t>(sour[1]) & 0xfffu;
            bgmset = 0;
            fade_flag = 0;
            fade_data = 0;
            break;
        case 1: // start
            bgmset |= 0x2u;
            bgmset &= ~0x4u;
            break;
        case 2: // stop
            bgmset |= 0x4u;
            bgmset &= ~0x2u;
            break;
        case 3: // mute
            bgmset |= 0x8u;
            bgmset &= ~0x10u;
            break;
        case 4: // unmute
            bgmset |= 0x10u;
            bgmset &= ~0x8u;
            break;
        case 5: // fade out
            bgmset |= 0x20u;
            bgmset &= ~0x40u;
            break;
        case 6: // fade in
            bgmset |= 0x40u;
            bgmset &= ~0x20u;
            break;
        default:
            break;
        }
    }

    // sddrStopMode [0x19e8]
    void SndmodDriver::sddrStopMode(const int32_t *sour)
    {
        set_cancel_mode(sour[0]);
    }

    // sddrSddrBusy [0x1a50]: 1 while any TVB/TSQ load is requested but not done.
    void SndmodDriver::sddrSddrBusy(int32_t *dest)
    {
        for (int bank = 0; bank < 4; ++bank)
            if (sddrwork.loadTvbf[bank].load == 1)
            {
                dest[0] = 1;
                return;
            }
        for (int bank = 0; bank < 4; ++bank)
            if (sddrwork.loadTsqf[bank].load == 1)
            {
                dest[0] = 1;
                return;
            }
        dest[0] = 0;
    }

    bool SndmodDriver::busy() const
    {
        for (int bank = 0; bank < 4; ++bank)
            if (sddrwork.loadTvbf[bank].load == 1 || sddrwork.loadTsqf[bank].load == 1)
                return true;
        return false;
    }

    // sddrReadData [0x1c0c]: sceSdBlockTrans read of SPU2 capture memory, two
    // 32-byte slices.  NATIVE: capture buffers are not modelled; returns zeros.
    void SndmodDriver::sddrReadData(uint8_t *puts)
    {
        std::memset(puts, 0, 64);
    }

    // sddr_EngineInit [0x1d70]
    void SndmodDriver::sddr_EngineInit()
    {
        uint8_t *base = reinterpret_cast<uint8_t *>(&sddrwork);
        for (int play = 0; play < 2; ++play)
        {
            // The IRX clears 72 bytes (two SDDRengine) per iteration; for play == 1
            // that also clears the first 36 bytes of the radio work.  Kept as is.
            std::memset(base + offsetof(SDDRwork, engine) + static_cast<size_t>(play) * sizeof(SDDRengine), 0, 72);
            SDDRengine &self = sddrwork.engine[play];
            self.flag_play = 1;
            self.flag_mute = 1;
            self.play_prog = -1;
            self.memo_voll = 0x3fff;
            self.memo_volr = 0x3fff;
            m_spu.setParam(voiceEntry(play * 2) | sd::VP_VOLL, 0x8013);
            m_spu.setParam(voiceEntry(play * 2) | sd::VP_VOLR, 0x8013);
            m_spu.setParam(voiceEntry(play * 2 + 1) | sd::VP_VOLL, 0x8013);
            m_spu.setParam(voiceEntry(play * 2 + 1) | sd::VP_VOLR, 0x8013);
        }
    }

    // sddr_EngineVols [0x210c]
    void SndmodDriver::sddr_EngineVols(const int32_t *sour)
    {
        const int32_t play = sour[0];
        if (play < 0 || play >= 2)
            return;
        sddrwork.engine[play].memo_voll = sour[1];
        sddrwork.engine[play].memo_volr = sour[2];
        vol_Engine();
    }

    // sddr_EngineRpms [0x21e8]: {play, type, disc, rpms, efct}
    void SndmodDriver::sddr_EngineRpms(const int32_t *sour)
    {
        const int32_t play = sour[0];
        if (play < 0 || play >= 2)
            return;
        SDDRengine &self = sddrwork.engine[play];
        self.play_type = sour[1];
        self.play_disc = sour[2];
        self.play_rpms = sour[3];
        self.play_efct = sour[4];
    }

    // sddr_EnginePlay [0x2318]
    void SndmodDriver::sddr_EnginePlay(const int32_t *sour)
    {
        SDDRengine &self = sddrwork.engine[sour[0] & 1];
        self.flag_play = 2;
        self.flag_mute = 1;
    }

    // sddr_EngineStop [0x23cc]
    void SndmodDriver::sddr_EngineStop(const int32_t *sour)
    {
        SDDRengine &self = sddrwork.engine[sour[0] & 1];
        self.flag_play = 1;
        self.play_prog = -1;
    }

    // sddr_EngineMute [0x2480]
    void SndmodDriver::sddr_EngineMute(const int32_t *sour)
    {
        const int32_t play = sour[0];
        if (play < 0 || play >= 2)
            return;
        if (sddrwork.engine[play].flag_play == 2)
        {
            sddrwork.engine[play].flag_mute = 2;
            sddr_EngineStop(sour);
        }
    }

    // sddr_EngineLoud [0x2554]
    void SndmodDriver::sddr_EngineLoud(const int32_t *sour)
    {
        const int32_t play = sour[0];
        if (play < 0 || play >= 2)
            return;
        if (sddrwork.engine[play].flag_mute == 2)
            sddr_EnginePlay(sour);
    }

    // sddr_RadioInit [0x2600]
    void SndmodDriver::sddr_RadioInit()
    {
        std::memset(&sddrwork.radio, 0, sizeof(SDDRradio));
        SDDRradio &self = sddrwork.radio;
        self.flag_play = 1;
        self.play_prog = -1;
        self.load_wait[0] = -1;
        self.load_wait[1] = -1;
        self.memo_voll = 0x3fff;
        self.memo_volr = 0x3fff;
        m_spu.setParam(8, 0x8013);
        m_spu.setParam(0x108, 0xa014);
        m_spu.setParam(10, 0x8013);
        m_spu.setParam(0x10a, 0xa014);
        m_spu.setParam(12, 0xa014);
        m_spu.setParam(0x10c, 0x8013);
        m_spu.setParam(14, 0xa014);
        m_spu.setParam(0x10e, 0x8013);
    }

    // sddr_RadioVols [0x2730]
    void SndmodDriver::sddr_RadioVols(const int32_t *sour)
    {
        sddrwork.radio.memo_voll = sour[0];
        sddrwork.radio.memo_volr = sour[1];
        vol_Radio();
    }

    // sddr_RadioFile [0x27c0]: prog_file[n >> 1][n & 1] = SDDRfile
    void SndmodDriver::sddr_RadioFile(const uint8_t *gets)
    {
        const uint32_t n = static_cast<uint32_t>(rdWord(gets, 0));
        if ((n >> 1) >= 3u)
            return;
        std::memcpy(&sddrwork.radio.prog_file[n >> 1][n & 1u], gets, sizeof(SDDRfile));
    }

    // sddr_RadioTune [0x2874]: {tune, time}
    void SndmodDriver::sddr_RadioTune(const int32_t *sour)
    {
        sddrwork.radio.play_tune = sour[0];
        sddrwork.radio.play_time = sour[1];
        sddrwork.radio.play_prog = -1;
        trace("radio_tune tune=%d time=%d", sour[0], sour[1]);
    }

    // sddr_RadioPlay [0x28fc]
    void SndmodDriver::sddr_RadioPlay()
    {
        sddrwork.radio.flag_play = 2;
        sddrwork.radio.flag_mute = 1;
    }

    // sddr_RadioStop [0x2954]
    void SndmodDriver::sddr_RadioStop()
    {
        sddrwork.radio.flag_play = 1;
        sddrwork.radio.play_prog = -1;
    }

    // sddr_RadioMute [0x29ac]
    void SndmodDriver::sddr_RadioMute()
    {
        if (sddrwork.radio.flag_play == 2)
        {
            sddrwork.radio.flag_mute = 2;
            sddr_RadioStop();
        }
    }

    // sddr_RadioLoud [0x2a2c]
    void SndmodDriver::sddr_RadioLoud()
    {
        if (sddrwork.radio.flag_mute == 2)
            sddr_RadioPlay();
    }

    // sddrBugsSddr [0x2aa0]: debug dump of chwork
    void SndmodDriver::sddrBugsSddr(uint8_t *puts)
    {
        std::memcpy(puts, chwork.data(), sizeof(chwork));
    }

    // sifrpc_main [0x7ea8]
    const uint8_t *SndmodDriver::sifrpcMain(uint32_t cmnd, const uint8_t *gets, uint32_t size)
    {
        (void)size;
        const int32_t *sour = reinterpret_cast<const int32_t *>(gets);
        int32_t *dest = reinterpret_cast<int32_t *>(rpcPutArg.data());
        switch (cmnd)
        {
        case 0x00:
            progInitWork(0);
            progSpu2Zero();
            sddrInitSddr();
            return nullptr;
        case 0x01:
            progInitWork(1);
            sddrStopSddr();
            return nullptr;
        case 0x02: // sddrHeapInit
        case 0x03: // sddrQuitSddr
            return nullptr;
        case 0x04:
            sddrTvbfAdrs(sour);
            return nullptr;
        case 0x05:
            sddrTsqfAdrs(sour);
            return nullptr;
        case 0x06:
            sddrTvbfTrns(gets);
            return nullptr;
        case 0x07:
            sddrTsqfTrns(gets);
            return nullptr;
        case 0x08:
            sddrCtrlMono(sour, dest);
            return rpcPutArg.data();
        case 0x09:
            sddrCtrlData(sour);
            return nullptr;
        case 0x0a:
            sddrMuteData(sour);
            return nullptr;
        case 0x0b:
            sddrAdsrData(sour);
            return nullptr;
        case 0x0c:
            sddrHardData(gets);
            return nullptr;
        case 0x0d:
            sddrSoftData(sour);
            return nullptr;
        case 0x0e:
            sddrSongData(sour);
            return nullptr;
        case 0x0f:
            sddrStopMode(sour);
            return nullptr;
        case 0x10:
            sddrSddrBusy(dest);
            return rpcPutArg.data();
        case 0x1f:
            sddrReadData(rpcPutArg.data());
            return rpcPutArg.data();
        case 0x20:
            sddr_EngineInit();
            return nullptr;
        case 0x21:
            sddr_EngineVols(sour);
            return nullptr;
        case 0x22:
            sddr_EngineRpms(sour);
            return nullptr;
        case 0x23:
            sddr_EnginePlay(sour);
            return nullptr;
        case 0x24:
            sddr_EngineStop(sour);
            return nullptr;
        case 0x25:
            sddr_EngineMute(sour);
            return nullptr;
        case 0x26:
            sddr_EngineLoud(sour);
            return nullptr;
        case 0x30:
            sddr_RadioInit();
            return nullptr;
        case 0x31:
            sddr_RadioVols(sour);
            return nullptr;
        case 0x32:
            sddr_RadioFile(gets);
            return nullptr;
        case 0x33:
            sddr_RadioTune(sour);
            return nullptr;
        case 0x34:
            sddr_RadioPlay();
            return nullptr;
        case 0x35:
            sddr_RadioStop();
            return nullptr;
        case 0x36:
            sddr_RadioMute();
            return nullptr;
        case 0x37:
            sddr_RadioLoud();
            return nullptr;
        case 0xfff0:
            sddrBugsSddr(rpcPutArg.data());
            return rpcPutArg.data();
        default:
            return nullptr;
        }
    }

    // ------------------------------------------------------------------ sequencer
    // Match helper shared by set_cancel / set_volume / set_reverb:
    // (s16)(bank << 8 | active) & mask == num, mask = 0xff00 when num & 0xff == 0.
    namespace
    {
        inline bool matches(const MUWORK &w, int32_t num, int32_t mask)
        {
            const int16_t id = static_cast<int16_t>(w.active | (w.bank << 8));
            return (id & mask) == num;
        }
    }

    void SndmodDriver::calcVolume(MUWORK *ix)
    {
        ix->vol_L = static_cast<int16_t>((ix->volume * ix->pan_L * ix->main_vol) / 256);
        ix->vol_R = static_cast<int16_t>((ix->volume * ix->pan_R * ix->main_vol) / 256);
    }

    // set_cancel [0x2b0c]
    void SndmodDriver::set_cancel(int32_t num)
    {
        const int32_t mask = (num & 0xff) ? 0xffff : 0xff00;
        for (int i = 8; i < 48; ++i)
        {
            if (matches(sework[i], num, mask))
            {
                sework[i].data_adr = kEndDatAddr;
                sework[i].data_cnt = 0;
            }
            if (matches(muwork[i], num, mask))
            {
                muwork[i].data_adr = kEndDatAddr;
                muwork[i].data_cnt = 0;
            }
        }
    }

    // set_volume [0x2c50]
    void SndmodDriver::set_volume(int32_t num, int32_t vol)
    {
        const int32_t mask = (num & 0xff) ? 0xffff : 0xff00;
        for (int i = 8; i < 48; ++i)
        {
            for (MUWORK *ix : {&sework[i], &muwork[i]})
            {
                if (matches(*ix, num, mask))
                {
                    ix->main_vol = static_cast<int16_t>(vol);
                    calcVolume(ix);
                }
            }
        }
    }

    // set_reverb [0x2ee0]
    void SndmodDriver::set_reverb(int32_t num, int32_t rev)
    {
        const int32_t mask = (num & 0xff) ? 0xffff : 0xff00;
        for (int i = 8; i < 48; ++i)
        {
            if (matches(sework[i], num, mask))
                sework[i].rev_flag = static_cast<uint8_t>(rev);
            if (matches(muwork[i], num, mask))
                muwork[i].rev_flag = static_cast<uint8_t>(rev);
        }
    }

    // set_cancel_mode [0x3010]
    void SndmodDriver::set_cancel_mode(int32_t mode)
    {
        std::array<MUWORK, 48> &mu = mode == 0 ? muwork : sework;
        for (int i = 8; i < 48; ++i)
        {
            mu[i].data_adr = kEndDatAddr;
            mu[i].data_cnt = 0;
        }
    }

    // set_volume_mode [0x30c8]
    void SndmodDriver::set_volume_mode(int32_t mode, int32_t vol)
    {
        std::array<MUWORK, 48> &mu = mode == 0 ? muwork : sework;
        for (int i = 8; i < 48; ++i)
        {
            mu[i].main_vol = static_cast<int16_t>(vol);
            calcVolume(&mu[i]);
        }
    }

    // set_reverb_mode [0x3228]
    void SndmodDriver::set_reverb_mode(int32_t mode, int32_t rev)
    {
        std::array<MUWORK, 48> &mu = mode == 0 ? muwork : sework;
        for (int i = 8; i < 48; ++i)
            mu[i].rev_flag = static_cast<uint8_t>(rev);
    }

    // reqmus [0x32d8]: start a BGM: 36 channel descriptors {pri, -, u16 offset}
    // for music channels 8..43.
    void SndmodDriver::reqmus(uint32_t p, int32_t req)
    {
        const uint32_t base = tsqtbl[(req >> 8) & 3] + rd16(p);
        uint32_t q = base;
        const uint32_t ofs = base;
        for (int i = 8; i < 44; ++i)
        {
            MUWORK &ix = muwork[i];
            ix.mode = 0;
            ix.pri = rd8(q);
            if (rd8(q) == 0)
                ix.pri = 1;
            q += 1;
            q += 1;
            const uint16_t sbuf = static_cast<uint16_t>(rd8(q) | (rd8(q + 1) << 8));
            q += 2;
            ix.data_adr = ofs + sbuf;
            ix.data_cnt = 0;
            ix.active = static_cast<uint8_t>(req);
            ix.bank = static_cast<uint8_t>((req >> 8) & 0xf);
            ix.pan_R = 64;
            ix.pan_L = 64;
            ix.volume = 200;
            ix.main_vol = 0x100;
            ix.rev_flag = 0;
        }
        trace("bgm_start %03x seq=0x%x", req, base);
    }

    // reqse [0x34c4]: allocate the lowest-priority voice channel (8..47) with
    // priority <= pri (last one wins on ties) and start the SE there.
    void SndmodDriver::reqse(int32_t pri, uint32_t p, int32_t req, int32_t pan)
    {
        int32_t j = -1;
        int32_t k = pri;
        for (int i = 8; i < 48; ++i)
        {
            if (!(k < chwork[i].active))
            {
                j = i;
                k = chwork[i].active;
            }
        }
        if (j == -1)
            return;
        MUWORK &ix = sework[j];
        CHWORK &iy = chwork[j];
        ix.pri = static_cast<uint8_t>(pri);
        iy.active = static_cast<uint8_t>(pri);
        ix.mode = 1;
        iy.mode = 1;
        ix.data_adr = tsqtbl[(req >> 8) & 3] + rd16(p);
        ix.data_cnt = 0;
        ix.active = static_cast<uint8_t>(req);
        ix.bank = static_cast<uint8_t>((req >> 8) & 0xf);
        ix.pan_R = static_cast<uint8_t>(pan);
        ix.pan_L = static_cast<uint8_t>(pan >> 8);
        ix.volume = 200;
        ix.main_vol = 0x100;
        ix.rev_flag = 0;
    }

    // request_que [0x36fc]: deferred BGM transport, run at the top of every tick.
    void SndmodDriver::request_que()
    {
        if (bgmset & 0x2u)
        {
            const uint32_t data = bgmbuf;
            const uint32_t bank = (data & 0xf00u) >> 8;
            uint32_t adrs = (bank < 4u ? tsqtbl[bank] : kEndDatAddr + 8u) + ((data & 0xffu) << 2);
            const uint16_t prio = rd16(adrs);
            adrs += 2u;
            if (prio == 0u && isFileLoad(static_cast<int>(bank)) != 0 && isWorkFree(0) != 0)
            {
                reqmus(adrs, static_cast<int32_t>(data & 0xfffu));
                bgmset &= ~0x2u;
                bgmset |= 0x1u;
            }
        }
        if (bgmset & 0x1u)
        {
            if (bgmset & 0x4u)
            {
                bgmset &= ~0x4u;
                set_cancel_mode(0);
            }
            if (bgmset & 0x8u)
            {
                bgmset &= ~0x8u;
                set_volume_mode(0, 0);
            }
            if (bgmset & 0x10u)
            {
                bgmset &= ~0x10u;
                set_volume_mode(0, 0x100);
            }
            if (bgmset & 0x20u)
            {
                bgmset &= ~0x20u;
                fade_flag = 0x100;
                fade_data = 4;
            }
            if (bgmset & 0x40u)
            {
                bgmset &= ~0x40u;
                fade_flag = -0x100;
                fade_data = 4;
            }
        }
    }

    // chgfreq [0x39ac]
    void SndmodDriver::chgfreq(MUWORK *ix, CHWORK *iy)
    {
        if (iy->freq == ix->freq)
            return;
        iy->freq = ix->freq;
        m_spu.setParam(voiceEntry(iy->channel) | sd::VP_PITCH, iy->freq);
    }

    // chgvol [0x3aa4]
    void SndmodDriver::chgvol(MUWORK *ix, CHWORK *iy)
    {
        if (iy->vol_L == ix->vol_L && iy->vol_R == ix->vol_R)
            return;
        iy->vol_L = ix->vol_L;
        iy->vol_R = ix->vol_R;
        m_spu.setParam(voiceEntry(iy->channel) | sd::VP_VOLL, static_cast<uint16_t>(iy->vol_L));
        m_spu.setParam(voiceEntry(iy->channel) | sd::VP_VOLR, static_cast<uint16_t>(iy->vol_R));
    }

    // chgrev [0x3c64]
    void SndmodDriver::chgrev(MUWORK *ix, CHWORK *iy)
    {
        if (iy->rev_flag == ix->rev_flag)
            return;
        iy->rev_flag = ix->rev_flag;
        const uint32_t unit = static_cast<uint32_t>(iy->channel / 24);
        const uint32_t bit = 1u << (iy->channel % 24);
        if (iy->rev_flag == 1)
            sd_s_vmixe[unit] |= bit;
        else
            sd_s_vmixe[unit] &= ~bit;
    }

    // mutevol [0x3e1c] (unreferenced in the IRX)
    void SndmodDriver::mutevol(CHWORK *iy)
    {
        iy->vol_L = 0;
        iy->vol_R = 0;
        m_spu.setParam(voiceEntry(iy->channel) | sd::VP_VOLL, 0);
        m_spu.setParam(voiceEntry(iy->channel) | sd::VP_VOLR, 0);
    }

    // chgtone [0x3f80]
    void SndmodDriver::chgtone(MUWORK *ix, CHWORK *iy)
    {
        if (iy->tone == ix->tone)
            return;
        iy->tone = ix->tone;
        const uint16_t entry = voiceEntry(iy->channel);
        if (iy->tone != 0)
        {
            const uint32_t tone = iy->tone;
            const bool inRange = tone < wave.size(); // NATIVE: bounds
            m_spu.setAddr(entry | sd::VA_SSA, inRange ? wave[tone] : 0u);
            if (sd_s_adsr_flag != 0)
            {
                m_spu.setParam(entry | sd::VP_ADSR1, static_cast<uint16_t>(sd_s_adsr1));
                m_spu.setParam(entry | sd::VP_ADSR2, static_cast<uint16_t>(sd_s_adsr2));
            }
            else
            {
                m_spu.setParam(entry | sd::VP_ADSR1, inRange ? adsr[tone][0] : 0);
                m_spu.setParam(entry | sd::VP_ADSR2, inRange ? adsr[tone][1] : 0);
            }
        }
        else
        {
            m_spu.setParam(entry | sd::VP_ADSR1, 0);
            m_spu.setParam(entry | sd::VP_ADSR2, 0);
        }
    }

    // set_rev_mode [0x446c] (sequence opcode E6)
    void SndmodDriver::set_rev_mode(int32_t mode)
    {
        trace("rev_mode %d", mode);
        for (int core = 0; core < 2; ++core)
        {
            SdEffectAttr attr = m_spu.getEffectAttr(core);
            attr.mode = mode | 0x100;
            m_spu.setEffectAttr(core, attr);
            m_spu.setCoreAttr(static_cast<uint16_t>(sd::C_EFFECT_ENABLE | core), 1);
        }
    }

    // set_rev_depth [0x4518] (sequence opcode E7)
    void SndmodDriver::set_rev_depth(int32_t depth)
    {
        trace("rev_depth %d", depth);
        for (int core = 0; core < 2; ++core)
        {
            SdEffectAttr attr = m_spu.getEffectAttr(core);
            attr.depthL = static_cast<int16_t>(depth << 8);
            attr.depthR = static_cast<int16_t>(depth << 8);
            m_spu.setEffectAttr(core, attr);
        }
    }

    // ext_chan [0x45d4] (opcode F9): fork the current channel into a free SE slot.
    void SndmodDriver::ext_chan(MUWORK *iz)
    {
        const uint8_t lo = fetch8(iz);
        const uint8_t hi = fetch8(iz);
        const int16_t sbuf = static_cast<int16_t>(lo | (hi << 8));
        int32_t j = -1;
        int32_t k = iz->pri;
        for (int i = 8; i < 48; ++i)
        {
            if (!(k < chwork[i].active))
            {
                j = i;
                k = chwork[i].active;
            }
        }
        if (j == -1)
            return;
        MUWORK *ix = &sework[j];
        CHWORK *iy = &chwork[j];
        if (ix != iz)
            std::memcpy(ix, iz, sizeof(MUWORK));
        iy->active = ix->pri;
        iy->mode = ix->mode;
        ix->data_adr = ix->data_adr + static_cast<uint32_t>(static_cast<int32_t>(sbuf));
        ix->data_cnt = 0;
    }

    // step_t [0x47f0]
    void SndmodDriver::step_t(MUWORK *ix, int32_t cnt)
    {
        ix->data_cnt = static_cast<int16_t>(cnt - 1);
    }

    // data_jump [0x482c] (F8): signed 16-bit displacement from the post-operand pc
    void SndmodDriver::data_jump(MUWORK *ix)
    {
        const uint8_t lo = fetch8(ix);
        const uint8_t hi = fetch8(ix);
        const int16_t disp = static_cast<int16_t>(lo | (hi << 8));
        ix->data_adr += static_cast<uint32_t>(static_cast<int32_t>(disp));
        if (m_trace && disp < 0)
        {
            const bool music = ix >= muwork.data() && ix < muwork.data() + muwork.size();
            const long index = music ? (ix - muwork.data()) : (ix - sework.data());
            trace("jump %s ch=%ld to=0x%x", music ? "mu" : "se", index, ix->data_adr);
        }
    }

    // pitch_cent [0x48bc] (EA)
    void SndmodDriver::pitch_cent(MUWORK *ix)
    {
        const uint8_t lo = fetch8(ix);
        const uint8_t hi = fetch8(ix);
        ix->cent = static_cast<uint16_t>(lo | (hi << 8));
    }

    // data_end [0x4940] (FF and any unknown opcode)
    void SndmodDriver::data_end(MUWORK *ix, CHWORK *iy)
    {
        if (ix->mode == iy->mode)
        {
            iy->active = 0;
            iy->key = 1;
            iy->mode = 0;
        }
        ix->active = 0;
    }

    // pri_chg [0x49b4] (F1)
    void SndmodDriver::pri_chg(MUWORK *ix, CHWORK *iy)
    {
        const int8_t pri = static_cast<int8_t>(fetch8(ix));
        ix->pri = static_cast<uint8_t>(pri);
        if (ix->mode == iy->mode)
            iy->active = static_cast<uint8_t>(pri);
    }

    namespace
    {
        inline uint16_t applyCent(uint16_t freq, uint16_t cent)
        {
            if (cent == 0)
                return freq;
            return static_cast<uint16_t>((static_cast<int32_t>(freq) * static_cast<int32_t>(cent)) / 4096);
        }
    }

    // key_pitch [0x4a40] (E5): key on with an explicit pitch word
    void SndmodDriver::key_pitch(MUWORK *ix, CHWORK *iy)
    {
        const uint8_t lo = fetch8(ix);
        const uint8_t hi = fetch8(ix);
        ix->freq = applyCent(static_cast<uint16_t>(lo | (hi << 8)), ix->cent);
        if (ix->mode == iy->mode)
        {
            iy->active = ix->pri;
            iy->rev_flag = ix->rev_flag;
            chgfreq(ix, iy);
            chgvol(ix, iy);
            chgtone(ix, iy);
            iy->key = 2;
        }
    }

    // key_on [0x4bac] (0x80..0xDF): key index -> freqtbl
    void SndmodDriver::key_on(MUWORK *ix, CHWORK *iy, int32_t key)
    {
        ix->freq = applyCent(freqtblEntry(static_cast<uint32_t>(key)), ix->cent);
        if (ix->mode == iy->mode)
        {
            iy->active = ix->pri;
            iy->rev_flag = ix->rev_flag;
            chgfreq(ix, iy);
            chgvol(ix, iy);
            chgtone(ix, iy);
            iy->key = 2;
        }
    }

    // key_off [0x4ce4] (F0)
    void SndmodDriver::key_off(MUWORK *ix, CHWORK *iy)
    {
        if (ix->mode == iy->mode)
            iy->key = 1;
    }

    // tone_chg [0x4d34] (E2): tone = (s8)byte | bank << 8; resets the cent multiplier
    void SndmodDriver::tone_chg(MUWORK *ix)
    {
        const int8_t b = static_cast<int8_t>(fetch8(ix));
        ix->tone = static_cast<uint16_t>(static_cast<int16_t>(b) | static_cast<int16_t>(ix->bank << 8));
        ix->cent = 0;
    }

    // panpot [0x4dac] (E1)
    void SndmodDriver::panpot(MUWORK *ix, CHWORK *iy)
    {
        ix->pan_L = fetch8(ix);
        ix->pan_R = fetch8(ix);
        calcVolume(ix);
        if (mono_flag != 0)
        {
            ix->vol_L = static_cast<int16_t>((ix->vol_L + ix->vol_R) / 2);
            ix->vol_R = ix->vol_L;
        }
        if (ix->mode == iy->mode)
            chgvol(ix, iy);
    }

    // volume [0x4f74] (E0)
    void SndmodDriver::volume(MUWORK *ix, CHWORK *iy)
    {
        ix->volume = fetch8(ix);
        calcVolume(ix);
        if (mono_flag != 0)
        {
            ix->vol_L = static_cast<int16_t>((ix->vol_L + ix->vol_R) / 2);
            ix->vol_R = ix->vol_L;
        }
        if (ix->mode == iy->mode)
            chgvol(ix, iy);
    }

    // tempo [0x5110] (E3): two operand bytes, no effect in this driver
    void SndmodDriver::tempo(MUWORK *ix)
    {
        (void)fetch8(ix);
        (void)fetch8(ix);
    }

    // pitch [0x5184] (E4): change pitch without retrigger
    void SndmodDriver::pitch(MUWORK *ix, CHWORK *iy)
    {
        const uint8_t lo = fetch8(ix);
        const uint8_t hi = fetch8(ix);
        ix->freq = applyCent(static_cast<uint16_t>(lo | (hi << 8)), ix->cent);
        if (ix->mode == iy->mode)
            chgfreq(ix, iy);
    }

    // play [0x5294]: per-channel interpreter, one call per tick.
    void SndmodDriver::play(MUWORK *ix, CHWORK *iy)
    {
        ix->data_cnt = static_cast<int16_t>(static_cast<uint16_t>(ix->data_cnt) - 1u);
        if (ix->data_cnt != -1)
        {
            if (ix->mode == iy->mode)
            {
                chgvol(ix, iy);
                chgrev(ix, iy);
            }
            return;
        }
        for (;;)
        {
            const uint8_t cmd = fetch8(ix);
            if (static_cast<int8_t>(cmd) >= 0)
            {
                step_t(ix, cmd);
                return;
            }
            if (cmd < 0xe0)
            {
                key_on(ix, iy, cmd & 0x7f);
                continue;
            }
            switch (cmd)
            {
            case 0xe0:
                volume(ix, iy);
                break;
            case 0xe1:
                panpot(ix, iy);
                break;
            case 0xe2:
                tone_chg(ix);
                break;
            case 0xe3:
                tempo(ix);
                break;
            case 0xe4:
                pitch(ix, iy);
                break;
            case 0xe5:
                key_pitch(ix, iy);
                break;
            case 0xe6:
                set_rev_mode(static_cast<int8_t>(fetch8(ix)));
                break;
            case 0xe7:
                set_rev_depth(static_cast<int8_t>(fetch8(ix)));
                break;
            case 0xe8:
                ix->rev_flag = 1;
                break;
            case 0xe9:
                ix->rev_flag = 0;
                break;
            case 0xea:
                pitch_cent(ix);
                break;
            case 0xf0:
                key_off(ix, iy);
                break;
            case 0xf1:
                pri_chg(ix, iy);
                break;
            case 0xf8:
                data_jump(ix);
                break;
            case 0xf9:
                ext_chan(ix);
                break;
            default:
                data_end(ix, iy);
                return;
            }
        }
    }

    // ------------------------------------------------------------------ loaders
    // load_tvbf [0x5678]: TVB = 0x800-byte header {u32 wave[256], u32 rsda[256]}
    // followed by PS-ADPCM data DMA'd to SPU2 RAM at tvbtbl[bank].
    void SndmodDriver::load_tvbf()
    {
        for (int bank = 0; bank < 4; ++bank)
        {
            SDDRload &load = sddrwork.loadTvbf[bank];
            if (load.load != 1)
                continue;
            const SDDRfile file = load.file;
            const int32_t flen = easyFileSize(file);
            std::vector<uint8_t> adrs(static_cast<size_t>(std::max(flen, 0x800)), 0u);
            const bool ok = easyFileRead(file, 0, static_cast<uint32_t>(flen), adrs.data());
            for (int cont = 0; cont < 256; ++cont)
                wave[static_cast<size_t>((bank << 8) + cont)] = static_cast<uint32_t>(rdWord(adrs.data(), static_cast<size_t>(cont)) + tvbtbl[bank]);
            for (int cont = 0; cont < 256; ++cont)
            {
                const uint32_t rsda = static_cast<uint32_t>(rdWord(adrs.data(), static_cast<size_t>(256 + cont)));
                const uint32_t v = (~rsda & 0xdffffff0u) | (rsda & 0xfu);
                adsr[static_cast<size_t>((bank << 8) + cont)] = {static_cast<uint16_t>(v), static_cast<uint16_t>(v >> 16)};
            }
            if (flen > 0x800)
                m_spu.writeRam(static_cast<uint32_t>(tvbtbl[bank]), adrs.data() + 0x800, static_cast<size_t>(flen - 0x800));
            load.load = 2;
            trace("tvbf_done bank=%d spu=0x%06x bytes=%d ok=%d", bank, tvbtbl[bank], flen - 0x800, ok ? 1 : 0);
        }
    }

    // load_tsqf [0x59a8]: TSQ read straight into tsqbuf at tsqtbl[bank].
    void SndmodDriver::load_tsqf()
    {
        for (int bank = 0; bank < 4; ++bank)
        {
            SDDRload &load = sddrwork.loadTsqf[bank];
            if (load.load != 1)
                continue;
            const SDDRfile file = load.file;
            const int32_t flen = easyFileSize(file);
            std::vector<uint8_t> data(static_cast<size_t>(std::max(flen, 0)), 0u);
            const bool ok = easyFileRead(file, 0, static_cast<uint32_t>(flen), data.data());
            const uint32_t dst = tsqtbl[bank];
            for (size_t i = 0; i < data.size(); ++i)
                if (dst + i < kTsqBufBytes) // NATIVE: tsqbuf bounds
                    tsqbuf[dst + i] = data[i];
            load.load = 2;
            trace("tsqf_done bank=%d ofs=0x%05x bytes=%d ok=%d", bank, dst, flen, ok ? 1 : 0);
        }
    }

    // load_radio_part [0x5b28]: one 30 s part of a 12 kHz mono VAG stream
    // (205,714 bytes per part after the 64-byte VAG header) -> SPU2 RAM
    // 0x5000 + ((side * 2 + disc) << 18), 0x40000 bytes.
    void SndmodDriver::load_radio_part(const SDDRfile &file, int32_t part, int32_t disc, int32_t side)
    {
        const int32_t offset = (part * 205714 + 64) & -16;
        std::vector<uint8_t> adrs;
        size_t from = 0;
        if (file.size != 0u)
        {
            // NATIVE: the IRX reads 128 sectors into a 0x40800 buffer and transfers
            // 0x40000 bytes from (offset % 2048), so the last few hundred bytes are
            // stale heap.  One extra sector is read so they are real stream data.
            adrs.assign(0x40800u, 0u);
            from = static_cast<size_t>(offset % 2048);
            const uint32_t lsn = static_cast<uint32_t>(offset / 2048) + file.lsn;
            (void)m_io.readSectors(lsn, 129u, adrs.data());
        }
        else
        {
            adrs.assign(0x40000u, 0u);
            char name[53];
            std::memcpy(name, file.file, 52);
            name[52] = '\0';
            (void)m_io.readHostFile(name, static_cast<uint32_t>(offset), 0x40000u, adrs.data());
        }
        m_spu.writeRam(static_cast<uint32_t>(((side * 2 + disc) << 18) + 0x5000), adrs.data() + from, 0x40000u);
    }

    // load_radio [0x5d68]
    void SndmodDriver::load_radio()
    {
        SDDRradio &self = sddrwork.radio;
        if (self.flag_load == 0)
            return;
        const int32_t disc = self.load_disc & 1;
        self.load_wait[disc] = -1;
        const int32_t prog = self.load_prog;
        const int32_t tune = prog / 120;
        const int32_t part = prog % 120;
        if (tune >= 0 && tune < 3)
        {
            const SDDRfile left = self.prog_file[tune][0];
            const SDDRfile right = self.prog_file[tune][1];
            load_radio_part(left, part, disc, 0);
            load_radio_part(right, part, disc, 1);
        }
        self.load_wait[disc] = prog;
        self.flag_load = 0;
        trace("radio_load prog=%d tune=%d part=%d disc=%d", prog, tune, part, disc);
    }

    // ------------------------------------------------------------------ workers
    // procRadio [0x5f70]: two-buffer radio streamer on voices 4..7.
    void SndmodDriver::procRadio()
    {
        SDDRradio &self = sddrwork.radio;
        DIWORK di{};
        auto put = [&](int channel) {
            di.channel = channel;
            diwork[static_cast<size_t>(channel)] = di;
        };
        if (self.flag_play == 1)
        {
            self.flag_play = 0;
            self.flag_wake = 0;
            di.mask = 1;
            di.key = 1;
            put(4);
            put(5);
            put(6);
            put(7);
            return;
        }
        if (self.flag_play != 2)
            return;

        const int32_t tune = self.play_tune * 120;
        const int32_t part = self.play_time / 1800;
        if (self.play_prog != tune + part)
        {
            if (self.play_mute == 0)
            {
                self.play_mute = 1;
                di.mask = 1;
                di.key = 1;
                put(4);
                put(5);
                put(6);
                put(7);
            }
            if (self.load_wait[0] == tune + part)
            {
                self.play_prog = tune + part;
                self.play_mute = 0;
                self.play_loud = 1;
                self.play_disc = 0;
            }
            else if (self.load_wait[1] == tune + part)
            {
                self.play_prog = tune + part;
                self.play_mute = 0;
                self.play_loud = 1;
                self.play_disc = 1;
            }
            else if (self.flag_load == 0)
            {
                self.load_prog = tune + part;
                self.load_disc = self.play_disc == 0 ? 1 : 0;
                self.flag_load = 1;
            }
        }
        else if (self.flag_load == 0)
        {
            self.load_prog = part < 119 ? tune + part + 1 : tune;
            self.load_disc = self.play_disc == 0 ? 1 : 0;
            if (self.load_wait[self.load_disc & 1] != self.load_prog)
                self.flag_load = 1;
        }

        if (self.play_loud != 0)
        {
            const int32_t time = self.play_time - part * 1800;
            if (time >= 0)
            {
                self.play_loud = 0;
                const int32_t adrs = ((time * 114) & -16) + 0x5000;
                di = DIWORK{};
                di.mask = 239;
                di.key = 2;
                di.adsr1 = self.flag_wake != 0 ? 0x13ff : 0x35ff;
                di.adsr2 = 0x5fc5;
                di.pitch = 0x400;
                di.vmix = 1;
                di.vmixe = 0;
                di.ssa = adrs + (self.play_disc << 18);
                put(self.play_disc + 4);
                di.ssa = adrs + ((self.play_disc + 2) << 18);
                put(self.play_disc + 6);
                self.flag_wake = 1;
                trace("radio_play prog=%d disc=%d time=%d", self.play_prog, self.play_disc, time);
            }
        }
        self.play_time += 1;
        if (self.play_time == 216000)
            self.play_time = 0;
    }

    // procEngine [0x6754]: two engines on voices {0,1} and {2,3}.
    void SndmodDriver::procEngine()
    {
        for (int play = 0; play < 2; ++play)
        {
            SDDRengine &self = sddrwork.engine[play];
            DIWORK di{};
            if (self.flag_play == 1)
            {
                self.flag_play = 0;
                di.mask = 1;
                di.key = 1;
                di.channel = play * 2;
                diwork[static_cast<size_t>(di.channel)] = di;
                di.channel = play * 2 + 1;
                diwork[static_cast<size_t>(di.channel)] = di;
                continue;
            }
            if (self.flag_play != 2)
                continue;

            if (self.play_prog != self.play_disc)
            {
                self.play_prog = self.play_disc;
                di.mask = 253;
                di.key = 2;
                di.bank = 0;
                di.offset = 0;
                di.adsr1 = 0x13ff;
                di.adsr2 = 0x5fc5;
                di.vmix = 1;
                di.vmixe = 0;
            }
            else
            {
                di.mask = 160;
            }
            if (kEngineCurve[4][0] < self.play_rpms)
                self.play_rpms = kEngineCurve[4][0];
            for (int cont = 0;; ++cont)
            {
                if (kEngineCurve[cont + 1][0] < self.play_rpms)
                    continue;
                const int32_t span = kEngineCurve[cont + 1][0] - kEngineCurve[cont][0];
                di.pitch = (self.play_rpms - kEngineCurve[cont][0]) * (kEngineCurve[cont][2] - kEngineCurve[cont][1]) / span + kEngineCurve[cont][1];
                break;
            }
            di.vmixe = self.play_efct;
            int32_t key0, key1;
            if (self.play_prog == 0)
            {
                key0 = 2;
                key1 = 1;
            }
            else
            {
                key0 = 1;
                key1 = 2;
            }
            const int type = self.play_type & 7; // NATIVE: wave.168 has 8 entries
            di.channel = play * 2;
            di.wave = kEngineWave[type][0];
            di.key = key0;
            diwork[static_cast<size_t>(di.channel)] = di;
            di.channel = play * 2 + 1;
            di.wave = kEngineWave[type][1];
            di.key = key1;
            diwork[static_cast<size_t>(di.channel)] = di;
        }
    }

    // procDirect [0x6cc0]: apply diwork[0..7] (engine + radio voices).
    // mask bits: 1 key, 2 ssa, 4 adsr1, 8 adsr2, 0x10 tone, 0x20 pitch,
    //            0x40 vmix, 0x80 vmixe, 0x100 voll, 0x200 volr.
    void SndmodDriver::procDirect()
    {
        for (int cont = 0; cont < 8; ++cont)
        {
            DIWORK &di = diwork[static_cast<size_t>(cont)];
            const uint32_t tone = static_cast<uint32_t>((di.bank << 8) + di.wave);
            const int unit = cont / 24;
            const uint16_t voice = voiceEntry(cont);
            const uint32_t shift = 1u << (cont % 24);
            if (di.mask == 0)
                continue;
            if (di.mask & 0x1)
            {
                if (di.key == 2)
                    sd_s_kon[unit] |= shift;
                else if (di.key == 1)
                    sd_s_koff[unit] |= shift;
            }
            if (di.mask & 0x2)
            {
                m_spu.setAddr(voice | sd::VA_SSA, static_cast<uint32_t>(di.ssa));
                if (sd_s_adsr_flag != 0)
                {
                    m_spu.setParam(voice | sd::VP_ADSR1, static_cast<uint16_t>(sd_s_adsr1));
                    m_spu.setParam(voice | sd::VP_ADSR2, static_cast<uint16_t>(sd_s_adsr2));
                }
                else
                {
                    if (di.mask & 0x4)
                        m_spu.setParam(voice | sd::VP_ADSR1, static_cast<uint16_t>(di.adsr1));
                    if (di.mask & 0x8)
                        m_spu.setParam(voice | sd::VP_ADSR2, static_cast<uint16_t>(di.adsr2));
                }
            }
            else if (di.mask & 0x10)
            {
                const bool inRange = tone < wave.size();
                m_spu.setAddr(voice | sd::VA_SSA, (inRange ? wave[tone] : 0u) + static_cast<uint32_t>(di.offset));
                if (sd_s_adsr_flag != 0)
                {
                    m_spu.setParam(voice | sd::VP_ADSR1, static_cast<uint16_t>(sd_s_adsr1));
                    m_spu.setParam(voice | sd::VP_ADSR2, static_cast<uint16_t>(sd_s_adsr2));
                }
                else
                {
                    if (di.mask & 0x4)
                        m_spu.setParam(voice | sd::VP_ADSR1, static_cast<uint16_t>(di.adsr1));
                    else
                        m_spu.setParam(voice | sd::VP_ADSR1, inRange ? adsr[tone][0] : 0);
                    if (di.mask & 0x8)
                        m_spu.setParam(voice | sd::VP_ADSR2, static_cast<uint16_t>(di.adsr2));
                    else
                        m_spu.setParam(voice | sd::VP_ADSR2, inRange ? adsr[tone][1] : 0);
                }
            }
            if (di.mask & 0x40)
            {
                if (di.vmix != 0)
                    sd_s_vmix[unit] |= shift;
                else
                    sd_s_vmix[unit] &= ~shift;
            }
            if (di.mask & 0x80)
            {
                if (di.vmixe != 0)
                    sd_s_vmixe[unit] |= shift;
                else
                    sd_s_vmixe[unit] &= ~shift;
            }
            if (di.mask & 0x20)
                m_spu.setParam(voice | sd::VP_PITCH, static_cast<uint16_t>(di.pitch));
            if (di.mask & 0x100)
                m_spu.setParam(voice | sd::VP_VOLL, static_cast<uint16_t>(di.voll));
            if (di.mask & 0x200)
                m_spu.setParam(voice | sd::VP_VOLR, static_cast<uint16_t>(di.volr));
            di.mask = 0;
        }
    }

    // procDriver [0x74c8]: sequencer tick for voice channels 8..47.
    void SndmodDriver::procDriver()
    {
        request_que();
        for (int cont = 8; cont < 48; ++cont)
        {
            MUWORK *se = &sework[static_cast<size_t>(cont)];
            MUWORK *mu = &muwork[static_cast<size_t>(cont)];
            CHWORK *ch = &chwork[static_cast<size_t>(cont)];
            const int unit = cont / 24;
            const uint32_t shift = 1u << (cont % 24);
            ch->key = 0;
            if (se->active != 0)
                play(se, ch);
            if (mu->active != 0)
                play(mu, ch);
            if (ch->key == 2)
            {
                sd_s_vmix[unit] |= shift;
                if (ch->rev_flag == 1)
                    sd_s_vmixe[unit] |= shift;
                else
                    sd_s_vmixe[unit] &= ~shift;
                sd_s_kon[unit] |= shift;
                trace("kon ch=%d mode=%d tone=%u freq=0x%04x vol=%d/%d adsr=%04x%04x ssa=0x%06x", cont, ch->mode, ch->tone, ch->freq,
                      ch->vol_L, ch->vol_R, m_spu.getParam(voiceEntry(cont) | sd::VP_ADSR2), m_spu.getParam(voiceEntry(cont) | sd::VP_ADSR1),
                      ch->tone < wave.size() ? wave[ch->tone] : 0u);
            }
            else if (ch->key == 1)
            {
                sd_s_koff[unit] |= shift;
                ch->tone = 0;
                trace("koff ch=%d", cont);
            }
        }
    }

    // procFade [0x7830]
    void SndmodDriver::procFade()
    {
        if (fade_flag > 0)
        {
            fade_flag -= fade_data;
            if (fade_flag <= 0)
                fade_flag = 0;
            set_volume_mode(0, fade_flag);
        }
        else if (fade_flag < 0)
        {
            fade_flag += fade_data;
            if (fade_flag >= 0)
                fade_flag = 0;
            set_volume_mode(0, fade_flag + 256);
        }
        for (int play = 0; play < 2; ++play)
            for (int data = play + 6; data < play + 17; data += 2)
                set_reverb(data, sddrwork.engine[play].play_efct);
    }

    // thread_sddr [0x79e0]: one alarm period.
    void SndmodDriver::threadSddr()
    {
        ++m_ticks;
        for (int cont = 0; cont < 2; ++cont)
        {
            sd_s_kon[cont] = 0;
            sd_s_koff[cont] = 0;
        }
        procRadio();
        procEngine();
        procDirect();
        procDriver();
        procFade();
        for (int cont = 0; cont < 2; ++cont)
        {
            const uint16_t core = static_cast<uint16_t>(cont % 2);
            const uint32_t keep = ~sd_s_vmix_mute[cont];
            m_spu.setSwitch(core | sd::S_VMIXL, keep & sd_s_vmix[cont]);
            m_spu.setSwitch(core | sd::S_VMIXR, keep & sd_s_vmix[cont]);
            m_spu.setSwitch(core | sd::S_VMIXEL, keep & sd_s_vmixe[cont]);
            m_spu.setSwitch(core | sd::S_VMIXER, keep & sd_s_vmixe[cont]);
            if (sd_s_koff[cont] != 0)
                m_spu.setSwitch(core | sd::S_KOFF, sd_s_koff[cont]);
            if (sd_s_kon[cont] != 0)
                m_spu.setSwitch(core | sd::S_KON, sd_s_kon[cont]);
        }
    }

    // thread_cdvd [0x5f0c]: one wake-up.
    void SndmodDriver::threadCdvd()
    {
        load_tvbf();
        load_tsqf();
        load_radio();
    }

    int SndmodDriver::musicChannelsActive() const
    {
        int n = 0;
        for (int i = 8; i < 48; ++i)
            n += muwork[i].active != 0 ? 1 : 0;
        return n;
    }

    int SndmodDriver::seChannelsActive() const
    {
        int n = 0;
        for (int i = 8; i < 48; ++i)
            n += sework[i].active != 0 ? 1 : 0;
        return n;
    }
}
