// 1:1 native port of the Road Trip Adventure / Choro Q HG 2 SNDMOD.IRX sound driver
// (Tamsoft "sndiop_cq5", built from sndmod.c).  Function names, structure layouts
// and field names come from the IRX symbol table and .mdebug stabs; behaviour was
// transcribed from the IRX disassembly (see game/docs/SNDMOD.md for the map).
//
// The driver is single-threaded by contract: the owner serialises sifrpc_main(),
// threadSddr() (one 16,666 us alarm period) and threadCdvd() with one mutex, as
// the IOP's priority scheduling did for the original threads.
#pragma once

#include "spu2.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <vector>

namespace ps2x::iop::sndmod
{
    // Disc / host I/O used by easyFileSize / easyFileRead.
    class SndmodIo
    {
    public:
        virtual ~SndmodIo() = default;
        // sceCdRead(lsn, sectors) into destination (sectors * 2048 bytes).
        virtual bool readSectors(uint32_t lsn, uint32_t sectors, uint8_t *destination) = 0;
        // ioman path (used only when SDDRfile.size == 0).
        virtual int32_t hostFileSize(const char *name) = 0;
        virtual bool readHostFile(const char *name, uint32_t offset, uint32_t size, uint8_t *destination) = 0;
    };

#pragma pack(push, 1)
    struct CHWORK
    {
        uint8_t active;
        uint8_t mode;
        uint8_t key;
        uint8_t rev_flag;
        uint16_t tone;
        uint16_t freq;
        int16_t vol_L;
        int16_t vol_R;
        int32_t channel;
    };
    static_assert(sizeof(CHWORK) == 16);

    struct MUWORK
    {
        uint8_t active;
        uint8_t bank;
        uint8_t mode;
        uint8_t pri;
        uint16_t tone;
        uint8_t pan_L;
        uint8_t pan_R;
        int16_t vol_L;
        int16_t vol_R;
        uint16_t cent;
        uint16_t freq;
        uint32_t data_adr; // address in the driver's sequence address space (see rd8)
        int16_t data_cnt;
        uint8_t volume;
        uint8_t rev_flag;
        int16_t main_vol;
        uint16_t pad;
    };
    static_assert(sizeof(MUWORK) == 28);

    struct DIWORK
    {
        int32_t mask;
        int32_t channel;
        int32_t key;
        int32_t ssa;
        int32_t adsr1;
        int32_t adsr2;
        int32_t bank;
        int32_t wave;
        int32_t offset;
        int32_t pitch;
        int32_t vmix;
        int32_t vmixe;
        int32_t voll;
        int32_t volr;
    };
    static_assert(sizeof(DIWORK) == 56);

    struct SDDRfile
    {
        uint32_t bank;
        uint32_t lsn;
        uint32_t size;
        char file[52];
    };
    static_assert(sizeof(SDDRfile) == 64);

    struct SDDRload
    {
        int32_t load;
        SDDRfile file;
    };
    static_assert(sizeof(SDDRload) == 68);

    struct SDDRengine
    {
        int32_t memo_voll;
        int32_t memo_volr;
        int32_t flag_play;
        int32_t flag_mute;
        int32_t play_type;
        int32_t play_disc;
        int32_t play_prog;
        int32_t play_rpms;
        int32_t play_efct;
    };
    static_assert(sizeof(SDDRengine) == 36);

    struct SDDRradio
    {
        int32_t memo_voll;
        int32_t memo_volr;
        int32_t flag_play;
        int32_t flag_wake;
        int32_t flag_mute;
        int32_t flag_load;
        int32_t play_time;
        int32_t play_tune;
        int32_t play_mute;
        int32_t play_loud;
        int32_t play_disc;
        int32_t play_prog;
        int32_t load_disc;
        int32_t load_prog;
        int32_t load_next;
        int32_t load_wait[2];
        SDDRfile prog_file[3][2];
    };
    static_assert(sizeof(SDDRradio) == 452);

    struct SDDRwork
    {
        int32_t thread_main;
        int32_t thread_sddr;
        int32_t thread_cdvd;
        SDDRload loadTvbf[4];
        SDDRload loadTsqf[4];
        SDDRengine engine[2];
        SDDRradio radio;
    };
    static_assert(sizeof(SDDRwork) == 1080);
#pragma pack(pop)
    static_assert(offsetof(SDDRwork, loadTvbf) == 12);
    static_assert(offsetof(SDDRwork, loadTsqf) == 284);
    static_assert(offsetof(SDDRwork, engine) == 556);
    static_assert(offsetof(SDDRwork, radio) == 628);
    static_assert(offsetof(SDDRradio, prog_file) == 68);

    // Optional verification trace sink (per-tick key-on/command events).
    using SndmodTraceFn = std::function<void(const char *line)>;

    // Fill the driver's data tables from the user's SNDMOD.IRX image (returns false if it does not match).
    bool loadDriverTablesFromIrx(const uint8_t *irx, size_t size);
    bool driverTablesLoaded();

    class SndmodDriver
    {
    public:
        static constexpr uint32_t kTsqBufBytes = 0x40000u;   // tsqbuf: int[65536]
        static constexpr uint32_t kEndDatAddr = 0x80000000u; // address of end_dat {0xff,0,0,0}
        static constexpr uint32_t kAlarmMicros = 0x411au;    // 16,666 us (start: USec2SysClock)

        SndmodDriver(Spu2 &spu, SndmodIo &io);

        // start(): sceSdInit + memset(sddrwork) (the alarm/threads belong to the host).
        void start();

        // sifrpc_main(cmnd, gets, size): returns the reply buffer (rpcPutArg) or nullptr.
        const uint8_t *sifrpcMain(uint32_t cmnd, const uint8_t *gets, uint32_t size);
        static constexpr size_t kRpcArgBytes = 4096u;

        // thread_sddr body for one alarm wake-up.
        void threadSddr();
        // thread_cdvd body for one wake-up (load_tvbf, load_tsqf, load_radio).
        void threadCdvd();

        void setTrace(SndmodTraceFn trace) { m_trace = std::move(trace); }
        [[nodiscard]] uint64_t tickCount() const { return m_ticks; }
        [[nodiscard]] bool busy() const;
        [[nodiscard]] int musicChannelsActive() const;
        [[nodiscard]] int seChannelsActive() const;
        [[nodiscard]] uint32_t bgmState() const { return bgmset; }

        // Pitch table (freqtbl @ .data 0x8acc): floor(4096 * 2^((i-48)/12)) clamped to 0x3fff.
        static uint16_t freqtblEntry(uint32_t index);

    private:
        // ---- memory helpers for the sequence address space
        [[nodiscard]] uint8_t rd8(uint32_t address) const;
        [[nodiscard]] uint16_t rd16(uint32_t address) const;
        uint8_t fetch8(MUWORK *ix);

        static uint16_t voiceEntry(int32_t channel);

        // ---- easy* helpers
        int32_t easyFileSize(const SDDRfile &file);
        bool easyFileRead(const SDDRfile &file, uint32_t fofs, uint32_t flen, uint8_t *adrs);
        void progInitWork(int mode);
        void progSpu2Zero();
        int isFileLoad(int bank);
        int isWorkFree(int mode);
        void vol_Engine();
        void vol_Radio();

        // ---- RPC handlers
        void sddrInitSddr();
        void sddrStopSddr();
        void sddrTvbfAdrs(const int32_t *sour);
        void sddrTsqfAdrs(const int32_t *sour);
        void sddrTvbfTrns(const uint8_t *gets);
        void sddrTsqfTrns(const uint8_t *gets);
        void sddrCtrlMono(const int32_t *sour, int32_t *dest);
        void sddrCtrlData(const int32_t *sour);
        void sddrMuteData(const int32_t *sour);
        void sddrAdsrData(const int32_t *sour);
        void sddrHardData(const uint8_t *gets);
        void sddrSoftData(const int32_t *sour);
        void sddrSongData(const int32_t *sour);
        void sddrStopMode(const int32_t *sour);
        void sddrSddrBusy(int32_t *dest);
        void sddrReadData(uint8_t *puts);
        void sddr_EngineInit();
        void sddr_EngineVols(const int32_t *sour);
        void sddr_EngineRpms(const int32_t *sour);
        void sddr_EnginePlay(const int32_t *sour);
        void sddr_EngineStop(const int32_t *sour);
        void sddr_EngineMute(const int32_t *sour);
        void sddr_EngineLoud(const int32_t *sour);
        void sddr_RadioInit();
        void sddr_RadioVols(const int32_t *sour);
        void sddr_RadioFile(const uint8_t *gets);
        void sddr_RadioTune(const int32_t *sour);
        void sddr_RadioPlay();
        void sddr_RadioStop();
        void sddr_RadioMute();
        void sddr_RadioLoud();
        void sddrBugsSddr(uint8_t *puts);

        // ---- sequencer
        void set_cancel(int32_t num);
        void set_volume(int32_t num, int32_t vol);
        void set_reverb(int32_t num, int32_t rev);
        void set_cancel_mode(int32_t mode);
        void set_volume_mode(int32_t mode, int32_t vol);
        void set_reverb_mode(int32_t mode, int32_t rev);
        void reqmus(uint32_t p, int32_t req);
        void reqse(int32_t pri, uint32_t p, int32_t req, int32_t pan);
        void request_que();
        void chgfreq(MUWORK *ix, CHWORK *iy);
        void chgvol(MUWORK *ix, CHWORK *iy);
        void chgrev(MUWORK *ix, CHWORK *iy);
        void mutevol(CHWORK *iy);
        void chgtone(MUWORK *ix, CHWORK *iy);
        void set_rev_mode(int32_t mode);
        void set_rev_depth(int32_t depth);
        void ext_chan(MUWORK *iz);
        void step_t(MUWORK *ix, int32_t cnt);
        void data_jump(MUWORK *ix);
        void pitch_cent(MUWORK *ix);
        void data_end(MUWORK *ix, CHWORK *iy);
        void pri_chg(MUWORK *ix, CHWORK *iy);
        void key_pitch(MUWORK *ix, CHWORK *iy);
        void key_on(MUWORK *ix, CHWORK *iy, int32_t key);
        void key_off(MUWORK *ix, CHWORK *iy);
        void tone_chg(MUWORK *ix);
        void panpot(MUWORK *ix, CHWORK *iy);
        void volume(MUWORK *ix, CHWORK *iy);
        void tempo(MUWORK *ix);
        void pitch(MUWORK *ix, CHWORK *iy);
        void play(MUWORK *ix, CHWORK *iy);
        static void calcVolume(MUWORK *ix);

        // ---- loaders / workers
        void load_tvbf();
        void load_tsqf();
        void load_radio_part(const SDDRfile &file, int32_t part, int32_t disc, int32_t side);
        void load_radio();
        void procRadio();
        void procEngine();
        void procDirect();
        void procDriver();
        void procFade();

        void trace(const char *fmt, ...);

        Spu2 &m_spu;
        SndmodIo &m_io;
        SndmodTraceFn m_trace;
        uint64_t m_ticks = 0;

        SDDRwork sddrwork{};
        std::array<CHWORK, 48> chwork{};
        std::array<MUWORK, 48> muwork{};
        std::array<MUWORK, 48> sework{};
        std::array<DIWORK, 48> diwork{};
        std::array<uint32_t, 1024> wave{};
        std::array<std::array<uint16_t, 2>, 1024> adsr{};
        std::array<int32_t, 4> tvbtbl{};
        std::array<uint32_t, 4> tsqtbl{};
        std::vector<uint8_t> tsqbuf;
        uint32_t bgmset = 0;
        uint32_t bgmbuf = 0;
        int32_t mono_flag = 0;
        int32_t fade_flag = 0;
        int32_t fade_data = 0;
        std::array<uint32_t, 2> sd_s_vmix_mute{};
        std::array<uint32_t, 2> sd_s_vmix{};
        std::array<uint32_t, 2> sd_s_vmixe{};
        int32_t sd_s_adsr_flag = 0;
        int32_t sd_s_adsr1 = 0;
        int32_t sd_s_adsr2 = 0;
        std::array<uint32_t, 2> sd_s_kon{};
        std::array<uint32_t, 2> sd_s_koff{};
        std::array<uint8_t, kRpcArgBytes> rpcPutArg{};
    };
}
