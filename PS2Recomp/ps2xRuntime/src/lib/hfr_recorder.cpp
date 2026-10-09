// G4b high-frame-rate recorder (see include/hfr_recorder.h). Everything here runs on the EE thread except
// latest(), which only loads a published shared_ptr.
#include "hfr_recorder.h"
#include "game_hooks.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <unordered_map>

namespace ps2x::hfr
{
    bool g_on = false;

    namespace
    {
        using Clock = std::chrono::steady_clock;

        PS2Runtime *g_runtime = nullptr;
        FILE *g_dump = nullptr;

        // current (open) record
        std::unique_ptr<TickRecord> g_rec;
        uint64_t g_openTick = 0; // tick number the open record will get (0 until the first vblank)
        uint64_t g_gen = 1;      // record generation (tags packets queued in the GIF arbiter)
        double g_msAcc = 0.0;    // recorder time spent for the open record

        // VIF1 stream position
        uint32_t g_chunkBase = 0;
        const uint8_t *g_chunkPtr = nullptr;
        uint32_t g_chunkSize = 0;
        uint32_t g_unpackSinceMscal = 0;

        // matrix A unpack (qw 0, 4 x V4-32) since the previous MSCAL
        bool g_lastAValid = false;
        uint8_t g_lastA[64];

        // current VU1-side state
        int32_t g_curMscal = -1;
        int32_t g_curGroup = 0;
        int32_t g_curCamera = -1;
        uint32_t g_curCameraSrc = 0;

        // EE-side pending uploads (created by hooks, consumed when their VIF1 data executes)
        std::deque<Key> g_pendingKeys;
        std::deque<Camera> g_pendingCams;
        uint32_t g_keysOrderMatched = 0;

        // draw-caller stack
        struct Frame
        {
            uint32_t fn, inst, parentFn, parentInst;
            uint32_t regs[10];
        };
        std::vector<Frame> g_stack;
        std::unordered_map<uint64_t, uint32_t> g_occ;
        uint32_t g_camProducer = 0, g_camUploads = 0, g_keysCreated = 0;

        // ring
        std::shared_ptr<const TickRecord> g_ring[3];
        std::atomic<uint64_t> g_published{0};

        // stats
        uint64_t g_statTicks = 0;
        double g_statMs = 0.0, g_statMB = 0.0;
        uint64_t g_statOrphans = 0, g_statNoGroup = 0, g_statSetupNoKey = 0, g_statCamUnmatched = 0, g_statCross = 0;

        inline uint32_t mix(uint32_t h, uint32_t v)
        {
            h ^= v + 0x9E3779B9u + (h << 6) + (h >> 2);
            return h;
        }

        bool readGuest(const uint8_t *rdram, uint32_t addr, void *out, uint32_t n)
        {
            const uint32_t phys = addr & 0x1FFFFFFFu;
            if (phys + n > PS2_RAM_SIZE)
                return false;
            std::memcpy(out, rdram + phys, n);
            return true;
        }

        struct Timer
        {
            Clock::time_point t0 = Clock::now();
            ~Timer() { g_msAcc += std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }
        };

        void openRecord(const TickRecord *prev)
        {
            auto rec = std::make_unique<TickRecord>();
            if (prev)
            {
                rec->vif.reserve(prev->vif.size() + prev->vif.size() / 4);
                rec->gif.reserve(prev->gif.size() + prev->gif.size() / 4);
                rec->packets.reserve(prev->packets.size() + 64);
                rec->unpacks.reserve(prev->unpacks.size() + 64);
                rec->mscals.reserve(prev->mscals.size() + 64);
                rec->groups.reserve(prev->groups.size() + 16);
                rec->keys.reserve(prev->keys.size() + 16);
            }
            if (g_runtime)
            {
                const uint8_t *vu1 = g_runtime->memory().getVU1Data();
                if (vu1)
                    rec->vu1DataAtOpen.assign(vu1, vu1 + PS2_VU1_DATA_SIZE);
                const auto &regs = g_runtime->memory().vif1_regs;
                rec->vif1RegsAtOpen.resize(sizeof(regs));
                std::memcpy(rec->vif1RegsAtOpen.data(), &regs, sizeof(regs));
                const auto &st = g_runtime->vu1().state();
                std::memcpy(rec->vu1vf, st.vf, sizeof(rec->vu1vf));
                std::memcpy(rec->vu1vi, st.vi, sizeof(rec->vu1vi));
            }
            // group 0 = setup state carried in from the previous record
            Group carried;
            if (prev && !prev->groups.empty())
            {
                const Group &last = prev->groups[static_cast<size_t>(g_curGroup)];
                carried.entry = last.entry;
                if (last.key >= 0)
                {
                    Key k = prev->keys[static_cast<size_t>(last.key)];
                    k.group = 0;
                    rec->keys.push_back(k);
                    carried.key = 0;
                }
                if (last.camera >= 0)
                {
                    rec->cameras.push_back(prev->cameras[static_cast<size_t>(last.camera)]);
                    carried.camera = 0;
                }
                carried.cameraSrc = last.cameraSrc;
            }
            rec->groups.push_back(carried);
            rec->keysCarriedIn = static_cast<uint32_t>(g_pendingKeys.size());
            g_curGroup = 0;
            g_curCamera = carried.camera;
            g_curCameraSrc = carried.cameraSrc;
            g_curMscal = -1;
            g_unpackSinceMscal = 0;
            g_chunkBase = 0;
            g_chunkPtr = nullptr;
            g_chunkSize = 0;
            g_rec = std::move(rec);
        }

        void writeDump(const TickRecord &r, double recordMB)
        {
            if (!g_dump)
                return;
            uint32_t pn[4] = {}, pb[4] = {};
            for (const auto &p : r.packets)
            {
                pn[p.path & 3]++;
                pb[p.path & 3] += p.size;
            }
            std::fprintf(g_dump, "{\"tick\":%llu,\"dispfb2\":\"%llx\",\"vif_bytes\":%zu,\"gif_bytes\":%zu,\"record_mb\":%.4f,\"record_ms\":%.4f,"
                                 "\"gif\":[[%u,%u],[%u,%u],[%u,%u]],",
                         (unsigned long long)r.tick, (unsigned long long)r.dispfb2, r.vif.size(), r.gif.size(), recordMB, r.recordMs,
                         pn[1], pb[1], pn[2], pb[2], pn[3], pb[3]);
            std::unordered_map<uint32_t, uint32_t> byEntry;
            for (const auto &m : r.mscals)
                byEntry[m.pc]++;
            std::fprintf(g_dump, "\"mscal\":{");
            bool first = true;
            for (const auto &kv : byEntry)
            {
                std::fprintf(g_dump, "%s\"%x\":%u", first ? "" : ",", kv.first, kv.second);
                first = false;
            }
            std::fprintf(g_dump, "},\"groups\":[");
            for (size_t i = 0; i < r.groups.size(); ++i)
            {
                const Group &g = r.groups[i];
                std::fprintf(g_dump, "%s[%u,%d,%d,%u,%u,%u,%u,%u]", i ? "," : "", g.entry, g.key, g.camera, g.cameraSrc, g.path1Packets, g.path1Bytes,
                             g.mscalCount, g.entryMask);
            }
            std::fprintf(g_dump, "],\"cameras\":[");
            for (size_t i = 0; i < r.cameras.size(); ++i)
            {
                const Camera &c = r.cameras[i];
                std::fprintf(g_dump, "%s[%u,%u,%llu]", i ? "," : "", c.src, c.ra, (unsigned long long)c.createdTick);
            }
            std::fprintf(g_dump, "],\"keys\":[");
            for (size_t i = 0; i < r.keys.size(); ++i)
            {
                const Key &k = r.keys[i];
                std::fprintf(g_dump, "%s[%u,%u,%u,%u,%u,%u,%u,%llu,%d,[", i ? "," : "", k.uploadFn, k.callerFn, k.inst, k.ra, k.occ, k.parentFn,
                             k.parentInst, (unsigned long long)k.createdTick, k.group);
                for (int j = 0; j < 16; ++j)
                    std::fprintf(g_dump, "%s%.7g", j ? "," : "", static_cast<double>(k.m[j]));
                std::fprintf(g_dump, "]");
                if (k.callerFn == 0x224AB8u)
                {
                    std::fprintf(g_dump, ",[");
                    for (int j = 0; j < 10; ++j)
                        std::fprintf(g_dump, "%s%u", j ? "," : "", k.regs[j]);
                    std::fprintf(g_dump, "]");
                }
                std::fprintf(g_dump, "]");
            }
            std::fprintf(g_dump, "],\"c\":{\"cam_producer\":%u,\"cam_uploads\":%u,\"keys_created\":%u,\"keys_orphaned\":%u,\"keys_carried_in\":%u,"
                                 "\"keys_order_matched\":%u,\"p1_sub\":%u,\"p1_drain\":%u,\"p1_nogroup\":%u,\"cam_unmatched\":%u,\"setup_nokey\":%u,"
                                 "\"unpacks\":%zu,\"mscals\":%zu}}\n",
                         r.camProducerCalls, r.cameraUploads, r.keysCreated, r.keysOrphaned, r.keysCarriedIn, g_keysOrderMatched, r.path1Submitted,
                         r.path1Drained, r.path1NoGroup, r.cameraUnmatched, r.setupNoKey, r.unpacks.size(), r.mscals.size());
            std::fflush(g_dump);
        }

        // ---- game hooks (EE thread)
        bool isParent224(uint32_t fn) { return fn == 0x224CE0u || fn == 0x224FF0u || fn == 0x225368u || fn == 0x225648u; }

        void onCallerEnter(ps2x::hooks::HookCall &call)
        {
            Frame f{};
            f.fn = call.address;
            const R5900Context *ctx = call.ctx;
            const uint32_t a0 = GPR_U32(ctx, 4);
            if (!g_stack.empty())
            {
                f.parentFn = g_stack.back().fn;
                f.parentInst = g_stack.back().inst;
            }
            if (isParent224(f.fn))
            {
                // 0x224AB8's parents keep their arguments in s-registers (0x224FF0: a0->s1, a1->s2, a2->s3,
                // a3->s0, t0->s5; 0x224CE0: a0->s6, a1->s4, a2->s3): the instance is the whole argument tuple.
                uint32_t h = mix(0u, a0);
                for (int r : {5, 6, 7, 8})
                    h = mix(h, GPR_U32(ctx, r));
                f.inst = h;
            }
            else if (f.fn == 0x224AB8u)
            {
                // Instance = s0 at entry: in 0x224CE0 s0 = the effect slot 0x17D41F0 + 16*i (also passed as a2),
                // in 0x224FF0 s0 = its a3 state pointer. a0 = (s6 >> 2) & 3 is an animation phase that changes
                // every tick, so it must not be part of the key (G4b race: s0 94.3% exact vs 48% with a0;
                // the rest are effects spawning/dying, see REPORT_G4b.md). inst is s0 only; the 0x224AB8 call ra is
                // NOT in the key (regs[9] keeps it for diagnostics); the key's ra is the upload's (0x224B8C/0x224CAC).
                f.inst = GPR_U32(ctx, 16);
                f.regs[0] = a0;
                for (int i = 0; i < 8; ++i)
                    f.regs[1 + i] = GPR_U32(ctx, 16 + i);
                f.regs[9] = call.ra;
            }
            else
            {
                f.inst = a0;
            }
            if (g_stack.size() < 64)
                g_stack.push_back(f);
        }

        void onCallerExit(ps2x::hooks::HookCall &call)
        {
            // pop the matching frame (tolerate imbalance)
            for (size_t i = g_stack.size(); i-- > 0;)
                if (g_stack[i].fn == call.address)
                {
                    g_stack.resize(i);
                    return;
                }
        }

        void onUploadEnter(ps2x::hooks::HookCall &call)
        {
            Timer t;
            Key k;
            k.uploadFn = call.address;
            k.ra = call.ra;
            if (!g_stack.empty())
            {
                const Frame &f = g_stack.back();
                k.callerFn = f.fn;
                k.inst = f.inst;
                k.parentFn = f.parentFn;
                k.parentInst = f.parentInst;
                std::memcpy(k.regs, f.regs, sizeof(k.regs));
            }
            const uint64_t okey = (static_cast<uint64_t>(mix(mix(k.callerFn, k.inst), k.uploadFn)) << 32) | k.ra;
            k.occ = g_occ[okey]++;
            k.createdTick = g_openTick;
            readGuest(call.rdram, GPR_U32(call.ctx, 4), k.m, 64);
            g_pendingKeys.push_back(k);
            ++g_keysCreated;
            if (g_pendingKeys.size() > 8192)
            {
                g_pendingKeys.pop_front();
                if (g_rec)
                    ++g_rec->keysOrphaned;
            }
        }

        void onCameraUploadEnter(ps2x::hooks::HookCall &call)
        {
            Timer t;
            Camera c;
            c.src = GPR_U32(call.ctx, 4);
            c.ra = call.ra;
            c.createdTick = g_openTick;
            readGuest(call.rdram, c.src, c.bc, 128);
            g_pendingCams.push_back(c);
            ++g_camUploads;
            if (g_pendingCams.size() > 64)
                g_pendingCams.pop_front();
        }

        void onCameraBlockEnter(ps2x::hooks::HookCall &call) // 0x227340: REF 15 qw from a0 -> VU1 TOPS+4
        {
            onCameraUploadEnter(call);
        }

        void onCameraProducerEnter(ps2x::hooks::HookCall &) { ++g_camProducer; }

        bool envOn(const char *name)
        {
            const char *v = std::getenv(name);
            return v && v[0] != '\0' && std::strcmp(v, "0") != 0;
        }
    }

    void installHooks(PS2Runtime &runtime)
    {
        if (!envOn("PS2X_HFR_RECORD"))
            return;
        g_runtime = &runtime;
        g_on = true;
        if (const char *dir = std::getenv("PS2X_HFR_DUMP"); dir && dir[0])
        {
            const std::string path = std::string(dir) + "/ticks.jsonl";
            g_dump = std::fopen(path.c_str(), "w");
            if (!g_dump)
                std::fprintf(stderr, "[hfr] cannot open %s\n", path.c_str());
        }
        using ps2x::hooks::add;
        static const uint32_t kCallers[] = {0x222710u, 0x2232E8u, 0x223830u, 0x224AB8u, 0x244898u, 0x244338u, 0x222370u, 0x25F788u,
                                            0x243898u, 0x243F98u, 0x226590u, 0x224CE0u, 0x224FF0u, 0x225368u, 0x225648u,
                                            // roam uploads with no caller from the list above (G4b: ra 0x244CB0/0x244D44, 0x25E9CC)
                                            0x244B60u, 0x25E800u};
        for (uint32_t fn : kCallers)
            add({fn, &onCallerEnter, &onCallerExit, nullptr, "hfr-caller"});
        add({0x227AA0u, &onUploadEnter, nullptr, nullptr, "hfr-upload-A"});
        add({0x2281F8u, &onUploadEnter, nullptr, nullptr, "hfr-upload-A70"});
        add({0x227670u, &onCameraUploadEnter, nullptr, nullptr, "hfr-camera"});
        add({0x227340u, &onCameraBlockEnter, nullptr, nullptr, "hfr-camera-block"});
        add({0x220458u, &onCameraProducerEnter, nullptr, nullptr, "hfr-camera-producer"});
        g_stack.reserve(64);
        openRecord(nullptr);
        std::fprintf(stderr, "[hfr] recorder on%s\n", g_dump ? " (dump)" : "");
        std::fflush(stderr);
    }

    void onVifChunk(const uint8_t *data, uint32_t size)
    {
        Timer t;
        g_chunkBase = static_cast<uint32_t>(g_rec->vif.size());
        g_chunkPtr = data;
        g_chunkSize = size;
        g_rec->vif.insert(g_rec->vif.end(), data, data + size);
    }

    void onUnpack(uint32_t cmd, uint32_t vuAddr, uint32_t num, uint32_t cycle, const uint8_t *src, uint32_t bytes)
    {
        Unpack u;
        u.cmd = cmd;
        u.vuAddr = vuAddr;
        u.num = num;
        u.cycle = cycle;
        u.bytes = bytes;
        if (g_chunkPtr && src >= g_chunkPtr + 4 && src <= g_chunkPtr + g_chunkSize)
            u.vifOffset = g_chunkBase + static_cast<uint32_t>(src - 4 - g_chunkPtr);
        g_rec->unpacks.push_back(u);
        ++g_unpackSinceMscal;
        const uint32_t opcode = (cmd >> 24) & 0x7Fu;
        const bool v4_32 = (opcode & 0x6Fu) == 0x6Cu; // UNPACK V4-32 (mask bit ignored)
        if (!v4_32)
            return;
        if (vuAddr == 0u && num == 4u && bytes >= 64u)
        {
            std::memcpy(g_lastA, src, 64);
            g_lastAValid = true;
        }
        else if (num >= 8u && bytes >= 128u && (vuAddr == 4u || num == 15u))
        {
            // camera block: 0x227670 (8 qw to qw 4) or 0x227340 (15 qw to TOPS+4, entry 0x00's own block)
            Timer t;
            g_curCamera = -1;
            g_curCameraSrc = 0;
            for (auto it = g_pendingCams.begin(); it != g_pendingCams.end(); ++it)
                if (std::memcmp(it->bc, src, 128) == 0)
                {
                    g_rec->cameras.push_back(*it);
                    g_curCamera = static_cast<int32_t>(g_rec->cameras.size() - 1);
                    g_curCameraSrc = it->src;
                    g_pendingCams.erase(g_pendingCams.begin(), it + 1);
                    break;
                }
            if (g_curCamera < 0)
                ++g_rec->cameraUnmatched;
        }
    }

    void onMscalBegin(uint32_t pc, uint32_t top, uint32_t itop)
    {
        Timer t;
        TickRecord &r = *g_rec;
        Mscal m;
        m.pc = pc;
        m.top = top;
        m.itop = itop;
        m.unpackCount = g_unpackSinceMscal;
        m.firstUnpack = static_cast<uint32_t>(r.unpacks.size()) - g_unpackSinceMscal;
        g_unpackSinceMscal = 0;
        const int32_t mi = static_cast<int32_t>(r.mscals.size());
        if (pc == 0x00u || pc == 0x10u || pc == 0x70u)
        {
            Group g;
            g.entry = pc;
            g.mscal = mi;
            g.camera = g_curCamera;
            g.cameraSrc = g_curCameraSrc;
            if (pc != 0x00u)
            {
                const uint32_t want = (pc == 0x10u) ? 0x227AA0u : 0x2281F8u;
                if (g_lastAValid)
                {
                    auto hit = g_pendingKeys.end();
                    for (auto it = g_pendingKeys.begin(); it != g_pendingKeys.end(); ++it)
                        if (it->uploadFn == want && std::memcmp(it->m, g_lastA, 64) == 0)
                        {
                            hit = it;
                            break;
                        }
                    if (hit == g_pendingKeys.end())
                    {
                        for (auto it = g_pendingKeys.begin(); it != g_pendingKeys.end(); ++it)
                            if (it->uploadFn == want)
                            {
                                hit = it;
                                ++g_keysOrderMatched;
                                break;
                            }
                    }
                    if (hit != g_pendingKeys.end())
                    {
                        Key k = *hit;
                        std::memcpy(k.m, g_lastA, 64); // as uploaded
                        k.group = static_cast<int32_t>(r.groups.size());
                        r.keys.push_back(k);
                        g.key = static_cast<int32_t>(r.keys.size() - 1);
                        g_pendingKeys.erase(hit);
                    }
                    else
                        ++r.setupNoKey;
                }
                else
                    ++r.setupNoKey;
            }
            r.groups.push_back(g);
            g_curGroup = static_cast<int32_t>(r.groups.size() - 1);
        }
        g_lastAValid = false;
        m.group = g_curGroup;
        Group &cg = r.groups[static_cast<size_t>(g_curGroup)];
        ++cg.mscalCount;
        if (pc != 0xFFFFFFFFu)
            cg.entryMask |= 1u << ((pc >> 4) & 31u);
        r.mscals.push_back(m);
        g_curMscal = mi;
    }

    void onMscalEnd() { g_curMscal = -1; }

    uint32_t onGifSubmit(uint8_t path)
    {
        if (path == 1u)
            ++g_rec->path1Submitted;
        // 4 bits generation | 14 bits mscal+1 | 14 bits group+1 (saturating; out-of-range => unattributed)
        const uint32_t ms = (g_curMscal >= 0 && g_curMscal < 0x3FFE) ? static_cast<uint32_t>(g_curMscal + 1) : 0u;
        const uint32_t gr = (g_curGroup >= 0 && g_curGroup < 0x3FFE) ? static_cast<uint32_t>(g_curGroup + 1) : 0u;
        return (static_cast<uint32_t>(g_gen & 0xFu) << 28) | (ms << 14) | gr;
    }

    void onGifDrain(uint8_t path, const uint8_t *data, uint32_t size, uint32_t tag)
    {
        Timer t;
        TickRecord &r = *g_rec;
        GifPacket p;
        p.path = path;
        p.offset = static_cast<uint32_t>(r.gif.size());
        p.size = size;
        r.gif.insert(r.gif.end(), data, data + size);
        if (path == 1u)
        {
            ++r.path1Drained;
            if ((tag >> 28) == (g_gen & 0xFu))
            {
                p.mscal = static_cast<int32_t>((tag >> 14) & 0x3FFFu) - 1;
                p.group = static_cast<int32_t>(tag & 0x3FFFu) - 1;
            }
            else
                ++g_statCross;
            if (p.group < 0)
                ++r.path1NoGroup;
            else
            {
                r.groups[static_cast<size_t>(p.group)].path1Packets++;
                r.groups[static_cast<size_t>(p.group)].path1Bytes += size;
            }
            if (p.mscal >= 0)
            {
                r.mscals[static_cast<size_t>(p.mscal)].path1Packets++;
                r.mscals[static_cast<size_t>(p.mscal)].path1Bytes += size;
            }
        }
        r.packets.push_back(p);
    }

    void onVBlank(uint64_t tick, uint64_t dispfb2)
    {
        const auto t0 = Clock::now();
        std::unique_ptr<TickRecord> rec = std::move(g_rec);
        rec->tick = tick;
        rec->dispfb2 = dispfb2;
        rec->camProducerCalls = g_camProducer;
        rec->cameraUploads = g_camUploads;
        rec->keysCreated = g_keysCreated;
        g_camProducer = g_camUploads = g_keysCreated = 0;
        // keys whose upload never executed within two records are orphans
        while (!g_pendingKeys.empty() && g_pendingKeys.front().createdTick + 2u < tick)
        {
            g_pendingKeys.pop_front();
            ++rec->keysOrphaned;
        }
        g_occ.clear();
        const double mb = static_cast<double>(rec->vif.size() + rec->gif.size() + rec->vu1DataAtOpen.size() +
                                              rec->packets.size() * sizeof(GifPacket) + rec->unpacks.size() * sizeof(Unpack) +
                                              rec->mscals.size() * sizeof(Mscal) + rec->groups.size() * sizeof(Group) +
                                              rec->keys.size() * sizeof(Key)) /
                          (1024.0 * 1024.0);
        std::shared_ptr<const TickRecord> pub(std::move(rec));
        const TickRecord &r = *pub;
        g_statOrphans += r.keysOrphaned;
        g_statNoGroup += r.path1NoGroup;
        g_statSetupNoKey += r.setupNoKey;
        g_statCamUnmatched += r.cameraUnmatched;

        // open the next record (snapshots VU1/VIF state) before publishing: the published one is never touched again
        g_openTick = tick + 1u;
        ++g_gen;
        openRecord(&r);
        const double closeMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        const_cast<TickRecord &>(r).recordMs = g_msAcc + closeMs; // still private to this thread here
        g_msAcc = 0.0;

        const uint64_t n = g_published.load(std::memory_order_relaxed);
        std::atomic_store_explicit(&g_ring[n % 3u], pub, std::memory_order_release);
        g_published.store(n + 1u, std::memory_order_release);

        writeDump(r, mb);
        ++g_statTicks;
        g_statMs += r.recordMs;
        g_statMB += mb;
        if ((g_statTicks % 500u) == 0u)
        {
            std::fprintf(stderr, "[hfr] tick=%llu avg_record_ms=%.3f avg_MB=%.3f orphans=%llu p1_nogroup=%llu setup_nokey=%llu cam_unmatched=%llu cross=%llu order_matched=%u\n",
                         (unsigned long long)tick, g_statMs / 500.0, g_statMB / 500.0, (unsigned long long)g_statOrphans,
                         (unsigned long long)g_statNoGroup, (unsigned long long)g_statSetupNoKey, (unsigned long long)g_statCamUnmatched,
                         (unsigned long long)g_statCross, g_keysOrderMatched);
            std::fflush(stderr);
            g_statMs = g_statMB = 0.0;
        }
    }

    std::shared_ptr<const TickRecord> latest(unsigned back)
    {
        const uint64_t n = g_published.load(std::memory_order_acquire);
        if (back >= 3u || back >= n)
            return nullptr;
        return std::atomic_load_explicit(&g_ring[(n - 1u - back) % 3u], std::memory_order_acquire);
    }
}
