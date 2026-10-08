// Offline harness for the native SNDMOD driver + SPU2 model (no game, no window).
//
//   sndmod_replay bgm <iso> <TSQ name> <TVB name> <request index> <ticks> <out.wav> [trace.txt]
//       Loads SOUND/<TVB> into bank 1 at SPU 0x160000 and SOUND/<TSQ> into bank 1 of
//       tsqbuf (offset 0x10000), exactly as the PAL game does for BGM, then issues
//       SongData select(0x100 | index) + start and renders <ticks> driver ticks.
//   sndmod_replay se <iso> <TSQ> <TVB> <request index> <ticks> <out.wav> [trace.txt]
//       Same loading into bank 0 (SPU 0x1b0000); issues SoftData(0x4040'00xx).
//   sndmod_replay radio <iso> <tune 0..2> <start time ticks> <ticks> <out.wav> [trace.txt]
//       Registers the six radio VAG files the way the PAL game does (RadioFile n =
//       tune*2 + side; tune 0 = 1CH, 1 = 3CH, 2 = 2CH), then RadioInit/Vols/Tune/Play.
//   sndmod_replay engine <iso> <type> <ticks> <out.wav> [trace.txt]
//       Loads CQ_MAIN into bank 0, starts engine 0 and sweeps rpm 0 -> 10000 -> 0
//       with a throttle (disc) flip at the top.
//   sndmod_replay log <iso> <PS2X_SNDMOD_LOG file> <ticks> <out.wav> [trace.txt]
//       Replays a command log recorded with tick stamps (tick=N) at the same ticks.
//
// All game data is read from the user's disc image at run time.
#include "modules/sndmod/sndmod_driver.h"
#include "modules/sndmod/spu2.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace ps2x::iop::sndmod;

namespace
{
    struct IsoIo final : SndmodIo
    {
        std::FILE *f = nullptr;
        bool readSectors(uint32_t lsn, uint32_t sectors, uint8_t *dst) override
        {
            std::fseek(f, static_cast<long>(lsn) * 2048L, SEEK_SET);
            return std::fread(dst, 2048, sectors, f) == sectors;
        }
        int32_t hostFileSize(const char *) override { return 0; }
        bool readHostFile(const char *, uint32_t, uint32_t, uint8_t *) override { return false; }

        // Minimal ISO9660 lookup: returns {lsn, bytes} of /SOUND/<name>.
        bool find(const std::string &dir, const std::string &name, uint32_t &lsn, uint32_t &bytes)
        {
            std::vector<uint8_t> pvd(2048);
            readSectors(16, 1, pvd.data());
            const uint8_t *root = pvd.data() + 156;
            uint32_t dirLsn = root[2] | root[3] << 8 | root[4] << 16 | root[5] << 24;
            uint32_t dirLen = root[10] | root[11] << 8 | root[12] << 16 | root[13] << 24;
            for (const std::string &want : {dir, name})
            {
                std::vector<uint8_t> d(((dirLen + 2047) / 2048) * 2048);
                readSectors(dirLsn, static_cast<uint32_t>(d.size() / 2048), d.data());
                bool found = false;
                for (size_t off = 0; off < d.size();)
                {
                    const uint8_t len = d[off];
                    if (len == 0)
                    {
                        off = (off / 2048 + 1) * 2048;
                        continue;
                    }
                    const uint8_t nameLen = d[off + 32];
                    std::string n(reinterpret_cast<const char *>(&d[off + 33]), nameLen);
                    if (auto semi = n.find(';'); semi != std::string::npos)
                        n.resize(semi);
                    if (n == want)
                    {
                        dirLsn = d[off + 2] | d[off + 3] << 8 | d[off + 4] << 16 | d[off + 5] << 24;
                        dirLen = d[off + 10] | d[off + 11] << 8 | d[off + 12] << 16 | d[off + 13] << 24;
                        found = true;
                        break;
                    }
                    off += len;
                }
                if (!found)
                    return false;
            }
            lsn = dirLsn;
            bytes = dirLen;
            return true;
        }
    };

    struct Wav
    {
        std::FILE *f = nullptr;
        uint32_t bytes = 0;
        void open(const char *p)
        {
            f = std::fopen(p, "wb");
            uint8_t h[44] = {};
            std::fwrite(h, 1, 44, f);
        }
        void write(const int16_t *s, size_t frames)
        {
            std::fwrite(s, 4, frames, f);
            bytes += static_cast<uint32_t>(frames * 4);
        }
        void close()
        {
            auto p32 = [](uint8_t *p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i)); };
            uint8_t h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0,
                             0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 16, 0, 'd', 'a', 't', 'a', 0, 0, 0, 0};
            p32(h + 4, 36 + bytes);
            p32(h + 24, 48000);
            p32(h + 28, 192000);
            p32(h + 40, bytes);
            std::fseek(f, 0, SEEK_SET);
            std::fwrite(h, 1, 44, f);
            std::fclose(f);
        }
    };

    std::vector<uint8_t> cmd(std::initializer_list<uint32_t> words)
    {
        std::vector<uint8_t> b(64, 0);
        size_t i = 0;
        for (uint32_t w : words)
            std::memcpy(b.data() + 4 * i++, &w, 4);
        return b;
    }

    struct Runner
    {
        Spu2 spu;
        IsoIo io;
        SndmodDriver drv{spu, io};
        Wav wav;
        double framesToTick = 0.0;
        std::FILE *trace = nullptr;

        void send(uint32_t fn, const std::vector<uint8_t> &b) { drv.sifrpcMain(fn, b.data(), static_cast<uint32_t>(b.size())); }
        void tick()
        {
            drv.threadSddr();
            drv.threadCdvd();
            framesToTick += 16666.0e-6 * 48000.0;
            const uint32_t n = static_cast<uint32_t>(framesToTick);
            framesToTick -= n;
            std::vector<int16_t> out(static_cast<size_t>(n) * 2);
            spu.render(out.data(), n);
            wav.write(out.data(), n);
        }
        void loadBank(uint32_t bank, uint32_t spuAddr, uint32_t tsqOfs, const std::string &tsq, const std::string &tvb)
        {
            uint32_t lsn, bytes;
            send(0x04, cmd({bank, spuAddr}));
            send(0x05, cmd({bank, tsqOfs}));
            if (!io.find("SOUND", tvb, lsn, bytes))
            {
                std::fprintf(stderr, "missing %s\n", tvb.c_str());
                std::exit(2);
            }
            send(0x06, cmd({bank, lsn, (bytes + 2047) / 2048}));
            if (!io.find("SOUND", tsq, lsn, bytes))
            {
                std::fprintf(stderr, "missing %s\n", tsq.c_str());
                std::exit(2);
            }
            send(0x07, cmd({bank, lsn, (bytes + 2047) / 2048}));
            tick();
        }
    };
}

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "see header comment for usage\n");
        return 1;
    }
    const std::string mode = argv[1];
    Runner r;
    r.io.f = std::fopen(argv[2], "rb");
    if (!r.io.f)
        return 1;
    const int traceArg = mode == "log" ? 6 : mode == "radio" ? 7 : mode == "engine" ? 6 : 8;
    if (argc > traceArg)
    {
        r.trace = std::fopen(argv[traceArg], "w");
        r.drv.setTrace([&](const char *l) { std::fprintf(r.trace, "%s\n", l); });
    }
    r.drv.start();
    r.send(0x00, cmd({}));

    if (mode == "bgm" || mode == "se")
    {
        const bool bgm = mode == "bgm";
        const uint32_t index = static_cast<uint32_t>(std::strtoul(argv[5], nullptr, 0));
        const int ticks = std::atoi(argv[6]);
        r.wav.open(argv[7]);
        if (bgm)
            r.loadBank(1, 0x160000, 0x10000, argv[3], argv[4]);
        else
            r.loadBank(0, 0x1b0000, 0x00000, argv[3], argv[4]);
        if (bgm)
        {
            r.send(0x0e, cmd({0, 0x100u | index}));
            r.send(0x0e, cmd({1}));
        }
        else
        {
            r.send(0x0d, cmd({0x40400000u | index}));
        }
        for (int t = 0; t < ticks; ++t)
            r.tick();
    }
    else if (mode == "radio")
    {
        const uint32_t tune = static_cast<uint32_t>(std::atoi(argv[3]));
        const uint32_t start = static_cast<uint32_t>(std::atoi(argv[4]));
        const int ticks = std::atoi(argv[5]);
        r.wav.open(argv[6]);
        const char *names[3][2] = {{"1CH_L.VAG", "1CH_R.VAG"}, {"3CH_L.VAG", "3CH_R.VAG"}, {"2CH_L.VAG", "2CH_R.VAG"}};
        r.send(0x30, cmd({}));
        for (uint32_t t = 0; t < 3; ++t)
            for (uint32_t side = 0; side < 2; ++side)
            {
                uint32_t lsn, bytes;
                if (!r.io.find("SOUND", names[t][side], lsn, bytes))
                    return 2;
                r.send(0x32, cmd({t * 2 + side, lsn, (bytes + 2047) / 2048}));
            }
        r.send(0x31, cmd({0x3fff, 0x3fff}));
        r.send(0x33, cmd({tune, start}));
        r.send(0x34, cmd({}));
        for (int t = 0; t < ticks; ++t)
        {
            r.tick();
            if (t == 10 && std::getenv("RADIO_DEBUG"))
                for (int v = 4; v < 8; ++v)
                    std::printf("voice%d voll=%04x volr=%04x env=%04x pitch=%04x\n", v, r.spu.getParam(static_cast<uint16_t>(v << 1)),
                                r.spu.getParam(static_cast<uint16_t>((v << 1) | 0x100)), r.spu.getParam(static_cast<uint16_t>((v << 1) | 0x500)),
                                r.spu.getParam(static_cast<uint16_t>((v << 1) | 0x200)));
        }
    }
    else if (mode == "engine")
    {
        const uint32_t type = static_cast<uint32_t>(std::atoi(argv[3]));
        const int ticks = std::atoi(argv[4]);
        r.wav.open(argv[5]);
        r.loadBank(0, 0x1b0000, 0x00000, "CQ_MAIN.TSQ", "CQ_MAIN.TVB");
        r.send(0x20, cmd({}));
        r.send(0x21, cmd({0, 0x1ccc, 0x1ccc}));
        r.send(0x23, cmd({0}));
        for (int t = 0; t < ticks; ++t)
        {
            const int half = ticks / 2;
            const int rpm = t < half ? t * 10000 / half : (ticks - t) * 10000 / half;
            r.send(0x22, cmd({0, type, t < half ? 1u : 0u, static_cast<uint32_t>(rpm), 0}));
            r.tick();
        }
    }
    else if (mode == "log")
    {
        std::ifstream in(argv[3]);
        const int ticks = std::atoi(argv[4]);
        r.wav.open(argv[5]);
        std::multimap<uint64_t, std::pair<uint32_t, std::vector<uint8_t>>> events;
        std::string line;
        while (std::getline(in, line))
        {
            std::istringstream ss(line);
            std::string seq, tickTok, fnTok, sizeTok;
            ss >> seq >> tickTok >> fnTok >> sizeTok;
            if (tickTok.rfind("tick=", 0) != 0)
                continue;
            const uint64_t t = std::stoull(tickTok.substr(5));
            const uint32_t fn = static_cast<uint32_t>(std::stoul(fnTok.substr(3), nullptr, 16));
            std::vector<uint8_t> b(64, 0);
            std::string w;
            size_t i = 0;
            while (ss >> w && i < 16)
            {
                const uint32_t v = static_cast<uint32_t>(std::stoul(w, nullptr, 16));
                std::memcpy(b.data() + 4 * i++, &v, 4);
            }
            if (fn == 0x00)
                continue; // already initialised
            events.emplace(t, std::make_pair(fn, b));
        }
        auto it = events.begin();
        for (int t = 0; t < ticks; ++t)
        {
            while (it != events.end() && it->first <= static_cast<uint64_t>(t))
            {
                r.send(it->second.first, it->second.second);
                ++it;
            }
            r.tick();
        }
    }
    r.wav.close();
    std::printf("ticks=%llu keyons=%llu blocks=%llu\n", static_cast<unsigned long long>(r.drv.tickCount()),
                static_cast<unsigned long long>(r.spu.stats().keyOns), static_cast<unsigned long long>(r.spu.stats().blocksDecoded));
    if (r.trace)
        std::fclose(r.trace);
    return 0;
}
