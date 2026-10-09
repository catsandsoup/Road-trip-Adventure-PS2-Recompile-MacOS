// Native replacement for Road Trip Adventure / Choro Q HG 2's SNDMOD.IRX RPC server
// (SID 0x01234567).  The physical IRX is never loaded; instead this service hosts a
// C++ port of the driver (modules/sndmod/sndmod_driver.*) on a software SPU2
// (modules/sndmod/spu2.*), and replaces the IRX's three IOP threads:
//
//   thread_main  -> handleRpc() (EE thread, serialised by m_mutex)
//   thread_sddr  -> SndmodDriver::threadSddr(), run from the audio render clock every
//                   16,666 us of rendered audio (799.968 frames at 48 kHz)
//   thread_cdvd  -> a host worker thread woken after every tick; disc reads come from
//                   the user's image (PS2X_CD_IMAGE) with m_mutex released while reading
//
// Output reaches the host through ps2x::iop::renderNativeAudio().  When no audio
// device pulls samples (headless runs), a wall-clock fallback keeps the driver ticking.
//
// Environment:
//   PS2X_SNDMOD_LOG=<file>    RPC command log (one line per non-poll command)
//   PS2X_SNDMOD_TRACE=<file>  driver trace (loads, BGM/SE requests, key-ons per tick)
//   PS2X_AUDIO_DUMP=<file>    WAV dump (s16 stereo 48 kHz) of the final mix
//   PS2X_SNDMOD_STUB=1        old behaviour: acknowledge everything, no audio
// See game/docs/SNDMOD.md and game/docs/AUDIO_ENGINE.md.
#include "module_factories.h"
#include "rpc_reply.h"

#include "modules/sndmod/sndmod_driver.h"
#include "modules/sndmod/spu2.h"
#include "ps2x/iop/native_audio.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace ps2x::iop::detail
{
    namespace
    {
        constexpr uint32_t kSndmodSid = 0x01234567u;
        constexpr uint32_t kRpcBusy = 0x10u;
        constexpr uint32_t kMaxLogsPerCommand = 8u;
        constexpr double kFramesPerTick = 16666.0e-6 * 48000.0; // 799.968

        constexpr std::array<uint32_t, 1> kSids{kSndmodSid};
        constexpr std::array<std::string_view, 1> kModuleAliases{"sndmod"};

        class WavWriter
        {
        public:
            ~WavWriter() { close(); }

            bool open(const char *path)
            {
                m_file = std::fopen(path, "wb");
                if (!m_file)
                    return false;
                writeHeader();
                return true;
            }

            void write(const int16_t *samples, size_t frames)
            {
                if (!m_file)
                    return;
                std::fwrite(samples, sizeof(int16_t) * 2, frames, m_file);
                m_dataBytes += static_cast<uint32_t>(frames * 4u);
                // Keep the header valid even if the process is killed.
                if (++m_writes % 16u == 0u)
                    patchHeader();
            }

            void close()
            {
                if (!m_file)
                    return;
                patchHeader();
                std::fclose(m_file);
                m_file = nullptr;
            }

        private:
            static void put32(uint8_t *p, uint32_t v)
            {
                p[0] = static_cast<uint8_t>(v);
                p[1] = static_cast<uint8_t>(v >> 8);
                p[2] = static_cast<uint8_t>(v >> 16);
                p[3] = static_cast<uint8_t>(v >> 24);
            }

            void writeHeader()
            {
                uint8_t h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ',
                                 16, 0, 0, 0, 1, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 16, 0,
                                 'd', 'a', 't', 'a', 0, 0, 0, 0};
                put32(h + 24, 48000u);
                put32(h + 28, 48000u * 4u);
                put32(h + 4, 36u + m_dataBytes);
                put32(h + 40, m_dataBytes);
                std::fwrite(h, 1, sizeof(h), m_file);
            }

            void patchHeader()
            {
                const long pos = std::ftell(m_file);
                std::fseek(m_file, 0, SEEK_SET);
                writeHeader();
                std::fseek(m_file, pos, SEEK_SET);
                std::fflush(m_file);
            }

            std::FILE *m_file = nullptr;
            uint32_t m_dataBytes = 0;
            uint32_t m_writes = 0;
        };

        class SndmodService final : public IopService, private sndmod::SndmodIo
        {
        public:
            explicit SndmodService(IopHost &host)
                : m_host(host), m_driver(m_spu, *this)
            {
                if (const char *path = std::getenv("PS2X_SNDMOD_LOG"); path && path[0] != '\0')
                    m_commandLog = std::fopen(path, "w");
                if (const char *path = std::getenv("PS2X_SNDMOD_TRACE"); path && path[0] != '\0')
                {
                    m_traceLog = std::fopen(path, "w");
                    if (m_traceLog)
                        m_driver.setTrace([this](const char *line) {
                            std::fputs(line, m_traceLog);
                            std::fputc('\n', m_traceLog);
                        });
                }
                if (const char *stub = std::getenv("PS2X_SNDMOD_STUB"); stub && stub[0] == '1')
                    m_stubOnly = true;
                if (const char *path = std::getenv("PS2X_AUDIO_DUMP"); path && path[0] != '\0')
                    m_dumpEnabled = m_dump.open(path);

                m_driver.start();
            }

            // The service object exists for every title; threads and the audio
            // source only start once SNDMOD is actually in use.
            void ensureStarted()
            {
                if (m_stubOnly || m_started)
                    return;
                m_started = true;
                m_isoPath = m_host.hostPath(HostPathKind::CdImage);
                if (m_isoPath.empty())
                    if (const char *env = std::getenv("PS2X_CD_IMAGE"); env && env[0] != '\0')
                        m_isoPath = env;
                if (!loadDriverTablesFromDisc())
                {
                    m_host.log(LogLevel::Error, "[SNDMOD:native] could not read SNDMOD.IRX tables from the disc image; audio disabled");
                    m_stubOnly = true;
                    return;
                }
                m_running = true;
                m_cdvdThread = std::thread([this] { cdvdLoop(); });
                m_clockThread = std::thread([this] { clockLoop(); });
                setNativeAudioSource(&SndmodService::renderThunk, this);
                m_host.log(LogLevel::Info, std::string("[SNDMOD:native] sound engine started, disc image: ") + m_isoPath);
            }

            ~SndmodService() override
            {
                clearNativeAudioSource(this);
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_running = false;
                    m_cdvdWake = true;
                }
                m_cdvdCv.notify_all();
                if (m_cdvdThread.joinable())
                    m_cdvdThread.join();
                if (m_clockThread.joinable())
                    m_clockThread.join();
                m_dump.close();
                if (m_traceLog)
                    std::fclose(m_traceLog);
                if (m_commandLog)
                    std::fclose(m_commandLog);
                if (m_iso)
                    std::fclose(m_iso);
            }

            [[nodiscard]] std::string_view name() const override { return "sndmod"; }
            [[nodiscard]] std::span<const uint32_t> sids() const override { return kSids; }
            [[nodiscard]] std::span<const std::string_view> moduleAliases() const override { return kModuleAliases; }
            [[nodiscard]] bool replacesPhysicalModule() const override { return true; }

            void reset() override
            {
                {
                    std::lock_guard<std::mutex> lock(m_logMutex);
                    m_commandCounts.clear();
                    m_totalCommands = 0u;
                }
                std::lock_guard<std::mutex> lock(m_mutex);
                m_driver.start();
            }

            [[nodiscard]] RpcResult handleRpc(const RpcRequest &request) override
            {
                RpcResult result;
                if (request.sid != kSndmodSid)
                    return result;

                result.handled = true;
                result.resultAddress = request.receive.address;

                // rpcGetArg is 4 KiB in the IRX.
                alignas(4) std::array<uint8_t, sndmod::SndmodDriver::kRpcArgBytes> gets{};
                const size_t sendBytes = std::min<size_t>(request.send.size, gets.size());
                if (request.send.address != 0u && sendBytes != 0u)
                    (void)m_host.readGuest(request.send.address, gets.data(), sendBytes);

                ensureStarted();
                uint64_t tick = 0;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    tick = m_driver.tickCount();
                }
                recordCommand(request, gets.data(), sendBytes, tick);

                if (m_stubOnly)
                {
                    if (request.receive.address != 0u && request.receive.size != 0u)
                    {
                        std::array<uint32_t, 16> reply{};
                        (void)writeRpcWords(m_host, request.receive, reply);
                    }
                    return result;
                }

                alignas(4) std::array<uint8_t, sndmod::SndmodDriver::kRpcArgBytes> reply{};
                bool haveReply = false;
                {
                    std::unique_lock<std::mutex> lock(m_mutex);
                    const uint8_t *data = m_driver.sifrpcMain(request.function, gets.data(), static_cast<uint32_t>(sendBytes));
                    if (data)
                    {
                        std::memcpy(reply.data(), data, reply.size());
                        haveReply = true;
                    }
                }
                // sceSifRpcLoop copies rsize bytes from the returned buffer; a NULL return sends nothing.
                if (haveReply && request.receive.address != 0u && request.receive.size != 0u)
                {
                    const size_t bytes = std::min<size_t>(request.receive.size, reply.size());
                    (void)m_host.writeGuest(request.receive.address, reply.data(), bytes);
                }
                return result;
            }

            void appendDebugMetrics(std::vector<DebugMetric> &metrics) const override
            {
                {
                    std::lock_guard<std::mutex> lock(m_logMutex);
                    metrics.push_back({"commands", m_totalCommands, false});
                    metrics.push_back({"distinct_commands", static_cast<uint64_t>(m_commandCounts.size()), false});
                }
                std::lock_guard<std::mutex> lock(m_mutex);
                metrics.push_back({"ticks", m_driver.tickCount(), false});
                metrics.push_back({"spu_key_ons", m_spu.stats().keyOns, false});
                metrics.push_back({"spu_active_voices", static_cast<uint64_t>(m_spu.activeVoices()), false});
                metrics.push_back({"bgm_channels", static_cast<uint64_t>(m_driver.musicChannelsActive()), false});
            }

        private:
            // ---------------------------------------------------------- audio clock
            static void renderThunk(void *user, int16_t *out, uint32_t frames)
            {
                auto *self = static_cast<SndmodService *>(user);
                self->m_lastDevicePull.store(std::chrono::steady_clock::now().time_since_epoch().count());
                self->m_deviceFrames += frames;
                self->render(out, frames);
            }

            void render(int16_t *out, uint32_t frames)
            {
                bool woke = false;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    uint32_t done = 0;
                    while (done < frames)
                    {
                        if (m_framesToTick <= 0.0)
                        {
                            m_driver.threadSddr();
                            if (m_traceLog && m_driver.tickCount() % 600u == 0u)
                                std::fprintf(m_traceLog, "%llu clock device_frames=%llu fallback_frames=%llu\n",
                                             static_cast<unsigned long long>(m_driver.tickCount()),
                                             static_cast<unsigned long long>(m_deviceFrames.load()),
                                             static_cast<unsigned long long>(m_fallbackFrames.load()));
                            m_cdvdWake = true;
                            woke = true;
                            m_framesToTick += kFramesPerTick;
                        }
                        uint32_t n = static_cast<uint32_t>(m_framesToTick + 0.999999);
                        n = std::min(n, frames - done);
                        if (n == 0u)
                            n = 1u;
                        m_spu.render(out + static_cast<size_t>(done) * 2u, n);
                        done += n;
                        m_framesToTick -= n;
                    }
                    if (m_dumpEnabled)
                        m_dump.write(out, frames);
                    m_framesRendered += frames;
                }
                if (woke)
                    m_cdvdCv.notify_one();
            }

            // Keeps the driver alive when no host audio device pulls samples.
            void clockLoop()
            {
                using clock = std::chrono::steady_clock;
                std::vector<int16_t> scratch;
                auto last = clock::now();
                double pending = 0.0;
                while (m_running)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    const auto now = clock::now();
                    const double elapsed = std::chrono::duration<double>(now - last).count();
                    last = now;
                    const auto lastPull = clock::time_point(clock::duration(m_lastDevicePull.load()));
                    const bool deviceAlive = (now - lastPull) < std::chrono::milliseconds(250);
                    if (deviceAlive)
                    {
                        pending = 0.0;
                        continue;
                    }
                    pending += elapsed * 48000.0;
                    const uint32_t frames = static_cast<uint32_t>(std::min(pending, 48000.0));
                    if (frames == 0u)
                        continue;
                    pending -= frames;
                    scratch.resize(static_cast<size_t>(frames) * 2u);
                    m_fallbackFrames += frames;
                    render(scratch.data(), frames);
                }
            }

            // ---------------------------------------------------------- thread_cdvd
            void cdvdLoop()
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                while (true)
                {
                    m_cdvdCv.wait(lock, [this] { return m_cdvdWake || !m_running; });
                    if (!m_running)
                        break;
                    m_cdvdWake = false;
                    m_cdLock = &lock;
                    m_driver.threadCdvd();
                    m_cdLock = nullptr;
                }
            }

            // ---------------------------------------------------------- SndmodIo
            // Locate SNDMOD.IRX in the ISO9660 root directory of the user's disc image and hand its
            // bytes to the driver so its .data tables come from the disc, not from this source.
            bool loadDriverTablesFromDisc()
            {
                if (sndmod::driverTablesLoaded())
                    return true;
                if (m_isoPath.empty())
                    return false;
                std::FILE *f = std::fopen(m_isoPath.c_str(), "rb");
                if (!f)
                    return false;
                const auto readAt = [f](uint64_t offset, void *dst, size_t bytes)
                {
                    return std::fseek(f, static_cast<long>(offset), SEEK_SET) == 0 && std::fread(dst, 1, bytes, f) == bytes;
                };
                constexpr uint32_t kSector = 2048u;
                std::array<uint8_t, kSector> pvd{};
                bool ok = readAt(16ull * kSector, pvd.data(), kSector) && pvd[0] == 1 && std::memcmp(pvd.data() + 1, "CD001", 5) == 0;
                std::vector<uint8_t> irx;
                if (ok)
                {
                    uint32_t rootLba = 0, rootSize = 0;
                    std::memcpy(&rootLba, pvd.data() + 156 + 2, 4);
                    std::memcpy(&rootSize, pvd.data() + 156 + 10, 4);
                    std::vector<uint8_t> dir(rootSize);
                    ok = rootSize != 0u && rootSize < (1u << 20) && readAt(static_cast<uint64_t>(rootLba) * kSector, dir.data(), dir.size());
                    for (size_t pos = 0; ok && pos < dir.size();)
                    {
                        const uint8_t len = dir[pos];
                        if (len == 0u)
                        {
                            pos = (pos / kSector + 1u) * kSector; // records never span sectors
                            continue;
                        }
                        const uint8_t nameLen = dir[pos + 32];
                        const std::string name(reinterpret_cast<const char *>(&dir[pos + 33]), nameLen);
                        if (name == "SNDMOD.IRX;1" || name == "SNDMOD.IRX")
                        {
                            uint32_t lba = 0, size = 0;
                            std::memcpy(&lba, &dir[pos + 2], 4);
                            std::memcpy(&size, &dir[pos + 10], 4);
                            irx.resize(size);
                            ok = size != 0u && size < (1u << 22) && readAt(static_cast<uint64_t>(lba) * kSector, irx.data(), size);
                            break;
                        }
                        pos += len;
                    }
                }
                std::fclose(f);
                return ok && !irx.empty() && sndmod::loadDriverTablesFromIrx(irx.data(), irx.size());
            }

            bool openIso()
            {
                if (m_iso)
                    return true;
                if (m_isoFailed)
                    return false;
                if (!m_isoPath.empty())
                    m_iso = std::fopen(m_isoPath.c_str(), "rb");
                if (!m_iso)
                {
                    m_isoFailed = true;
                    m_host.log(LogLevel::Error, "[SNDMOD:native] cannot open disc image for sound data (PS2X_CD_IMAGE)");
                    return false;
                }
                return true;
            }

            // Called from thread_cdvd with m_mutex held; the lock is released for the
            // duration of the read (the IRX blocks in sceCdSync the same way).
            bool readSectors(uint32_t lsn, uint32_t sectors, uint8_t *destination) override
            {
                const size_t bytes = static_cast<size_t>(sectors) * 2048u;
                std::vector<uint8_t> buffer(bytes, 0u);
                bool ok = false;
                unlockForIo([&] {
                    std::lock_guard<std::mutex> isoLock(m_isoMutex);
                    if (!openIso())
                        return;
                    if (std::fseek(m_iso, static_cast<long>(lsn) * 2048L, SEEK_SET) != 0)
                        return;
                    ok = std::fread(buffer.data(), 1, bytes, m_iso) == bytes;
                });
                std::memcpy(destination, buffer.data(), bytes);
                return ok;
            }

            int32_t hostFileSize(const char *name) override
            {
                int32_t size = 0;
                const std::string path = m_host.translateGuestPath(name);
                unlockForIo([&] {
                    if (std::FILE *f = std::fopen(path.c_str(), "rb"))
                    {
                        std::fseek(f, 0, SEEK_END);
                        size = static_cast<int32_t>(std::ftell(f));
                        std::fclose(f);
                    }
                });
                return size;
            }

            bool readHostFile(const char *name, uint32_t offset, uint32_t size, uint8_t *destination) override
            {
                std::vector<uint8_t> buffer(size, 0u);
                bool ok = false;
                const std::string path = m_host.translateGuestPath(name);
                unlockForIo([&] {
                    if (std::FILE *f = std::fopen(path.c_str(), "rb"))
                    {
                        std::fseek(f, static_cast<long>(offset), SEEK_SET);
                        ok = std::fread(buffer.data(), 1, size, f) == size;
                        std::fclose(f);
                    }
                });
                std::memcpy(destination, buffer.data(), size);
                return ok;
            }

            template <typename Fn>
            void unlockForIo(Fn &&fn)
            {
                if (m_cdLock && m_cdLock->owns_lock())
                {
                    m_cdLock->unlock();
                    fn();
                    m_cdLock->lock();
                }
                else
                {
                    fn();
                }
            }

            // ---------------------------------------------------------- logging
            void recordCommand(const RpcRequest &request, const uint8_t *payload, size_t payloadBytes, uint64_t tick)
            {
                std::lock_guard<std::mutex> lock(m_logMutex);
                ++m_totalCommands;
                uint32_t &count = m_commandCounts[request.function];
                ++count;

                const size_t words = std::min<size_t>(16u, payloadBytes / sizeof(uint32_t));
                if (m_commandLog && request.function != kRpcBusy)
                {
                    std::fprintf(m_commandLog, "%llu tick=%llu fn=0x%02x size=%u",
                                 static_cast<unsigned long long>(m_totalCommands), static_cast<unsigned long long>(tick),
                                 request.function, request.send.size);
                    for (size_t i = 0; i < words; ++i)
                    {
                        uint32_t w;
                        std::memcpy(&w, payload + i * 4u, 4u);
                        std::fprintf(m_commandLog, " %08x", w);
                    }
                    std::fputc('\n', m_commandLog);
                    std::fflush(m_commandLog);
                }

                if (count <= kMaxLogsPerCommand && request.function != kRpcBusy)
                {
                    std::ostringstream message;
                    message << "[SNDMOD:native] rpc=0x" << std::hex << request.function
                            << " send=0x" << request.send.size << " recv=0x" << request.receive.size;
                    for (size_t i = 0; i < std::min<size_t>(4u, words); ++i)
                    {
                        uint32_t w;
                        std::memcpy(&w, payload + i * 4u, 4u);
                        message << ' ' << w;
                    }
                    m_host.log(LogLevel::Info, message.str());
                }
            }

            IopHost &m_host;
            sndmod::Spu2 m_spu;
            sndmod::SndmodDriver m_driver;
            mutable std::mutex m_mutex; // serialises driver + SPU (the IOP scheduler's role)
            mutable std::mutex m_logMutex;
            std::mutex m_isoMutex;
            std::condition_variable m_cdvdCv;
            bool m_cdvdWake = false;
            std::atomic<bool> m_running{false};
            std::unique_lock<std::mutex> *m_cdLock = nullptr;
            std::thread m_cdvdThread;
            std::thread m_clockThread;
            std::atomic<int64_t> m_lastDevicePull{0};
            std::atomic<uint64_t> m_deviceFrames{0};
            std::atomic<uint64_t> m_fallbackFrames{0};
            double m_framesToTick = 0.0;
            uint64_t m_framesRendered = 0;
            bool m_stubOnly = false;
            bool m_dumpEnabled = false;
            WavWriter m_dump;
            std::FILE *m_iso = nullptr;
            bool m_isoFailed = false;
            bool m_started = false;
            std::string m_isoPath;
            std::unordered_map<uint32_t, uint32_t> m_commandCounts;
            uint64_t m_totalCommands = 0u;
            std::FILE *m_commandLog = nullptr;
            std::FILE *m_traceLog = nullptr;
        };
    }

    std::unique_ptr<IopService> createSndmodService(IopHost &host)
    {
        return std::make_unique<SndmodService>(host);
    }
}
