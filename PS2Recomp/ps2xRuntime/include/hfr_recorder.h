#pragma once
// G4b: high-frame-rate RECORDER (stage 1 of 60/120 fps, option B in work/tracks/G4/REPORT_G4a.md).
// Off by default; PS2X_HFR_RECORD=1 enables it. When off, every call site is guarded by the single
// static bool g_on (set once at start-up), no game hook is installed and nothing is recorded.
//
// Per EE vblank (= the det frame-dump tick; record N closes at VBlankStart and is named N = vsyncTick+1
// like frame_* dumps) one immutable TickRecord holds:
//  - the ordered GIF stream exactly as handed to the GS frontend (GifArbiter::drain order), path-tagged,
//    each PATH1 packet attributed to the VU1 MSCAL that produced it and to its setup group;
//  - the raw VIF1 byte stream (all chunks, in processing order) with UNPACK records (VU1 qw address,
//    count, offset into the stream) and MSCAL/MSCNT records (entry pc, TOP/ITOP, preceding unpacks);
//  - setup groups: one per MSCAL to a setup entry (0x00 camera block, 0x10 object A, 0x70 object A +
//    viewport), plus group 0 = state carried in from the previous record; each with its object key
//    and camera upload;
//  - object keys (caller fn, caller instance, upload ra, occurrence) from game hooks on the matrix
//    uploads 0x227AA0/0x2281F8, the camera upload 0x227670 and the draw callers, matched to the
//    UNPACK that carried the matrix by content (keys may be created one tick and executed the next);
//  - replay state at record open: VU1 data memory, VIF1 registers and VU1 registers.
// Published into a 3-slot ring (shared_ptr<const TickRecord>, atomic store); a render thread reads with
// latest(); the EE thread never waits. PS2X_HFR_DUMP=<dir> writes <dir>/ticks.jsonl (one summary per tick).
#include <cstdint>
#include <memory>
#include <vector>

class PS2Runtime;

namespace ps2x::hfr
{
    extern bool g_on; // PS2X_HFR_RECORD=1; constant after start-up

    struct GifPacket
    {
        uint8_t path = 0;    // 1, 2, 3
        uint32_t offset = 0; // into TickRecord::gif
        uint32_t size = 0;
        int32_t mscal = -1;  // PATH1: index into TickRecord::mscals (-1: none, or produced in an earlier record)
        int32_t group = -1;  // PATH1: index into TickRecord::groups (0 = carried-in state)
    };
    struct Unpack
    {
        uint32_t cmd = 0;
        uint32_t vuAddr = 0; // qw
        uint32_t num = 0;    // vectors written
        uint32_t cycle = 0;  // STCYCL (CL | WL << 8)
        uint32_t vifOffset = 0; // offset of the VIFcode in TickRecord::vif
        uint32_t bytes = 0;
    };
    struct Mscal
    {
        uint32_t pc = 0; // byte address; 0xFFFFFFFF = MSCNT
        uint32_t top = 0, itop = 0;
        uint32_t firstUnpack = 0, unpackCount = 0; // unpacks since the previous MSCAL/MSCNT
        int32_t group = -1;
        uint32_t path1Packets = 0, path1Bytes = 0;
    };
    struct Key
    {
        uint32_t uploadFn = 0;  // 0x227AA0 or 0x2281F8
        uint32_t callerFn = 0;  // innermost hooked draw caller (0 = none)
        uint32_t inst = 0;      // caller instance id
        uint32_t ra = 0;        // upload call's return address
        uint32_t occ = 0;       // occurrence of (callerFn, inst, ra) within the creating tick
        uint32_t parentFn = 0, parentInst = 0;
        uint64_t createdTick = 0;
        int32_t group = -1;
        uint32_t regs[10] = {}; // 0x224AB8 family: a0, s0..s7, ra of the 0x224AB8 call (diagnostics)
        float m[16] = {};       // matrix A (as uploaded)
    };
    struct Camera
    {
        uint32_t src = 0, ra = 0; // 0x227670 a0 (camera struct) and its return address
        uint64_t createdTick = 0;
        float bc[32] = {};        // B (qw 4-7) and C (qw 8-B)
    };
    struct Group
    {
        uint32_t entry = 0;   // setup entry pc (0x00, 0x10, 0x70); carried group: entry of the inherited setup
        int32_t mscal = -1;   // -1 for the carried group
        int32_t key = -1;     // index into TickRecord::keys (entry 0x10/0x70)
        int32_t camera = -1;  // index into TickRecord::cameras (-1: camera block not from 0x227670 / unmatched)
        uint32_t cameraSrc = 0;
        uint32_t path1Packets = 0, path1Bytes = 0;
        uint32_t mscalCount = 0; // MSCALs (incl. the setup one) run under this group
        uint32_t entryMask = 0;  // bit (pc>>4) for each MSCAL entry run under this group
    };
    struct TickRecord
    {
        uint64_t tick = 0;     // vsyncTick+1 at close (matches frame_* dump numbering)
        uint64_t dispfb2 = 0;
        std::vector<uint8_t> vif, gif;
        std::vector<GifPacket> packets;
        std::vector<Unpack> unpacks;
        std::vector<Mscal> mscals;
        std::vector<Group> groups;
        std::vector<Key> keys;     // keys whose setup MSCAL ran in this record
        std::vector<Camera> cameras;
        // replay state at record open
        std::vector<uint8_t> vu1DataAtOpen; // 16 KiB
        std::vector<uint8_t> vif1RegsAtOpen;
        float vu1vf[32][4] = {};
        int32_t vu1vi[16] = {};
        // counters
        uint32_t camProducerCalls = 0, cameraUploads = 0, keysCreated = 0, keysOrphaned = 0, keysCarriedIn = 0;
        uint32_t path1Submitted = 0, path1Drained = 0, path1NoGroup = 0, cameraUnmatched = 0, setupNoKey = 0;
        double recordMs = 0.0;
    };

    // Called by ps2x::hooks::installAll (before the game thread runs): reads the env and adds hooks.
    void installHooks(PS2Runtime &runtime);

    // Hot-path entry points (call only when g_on).
    void onVifChunk(const uint8_t *data, uint32_t size);
    void onUnpack(uint32_t cmd, uint32_t vuAddr, uint32_t num, uint32_t cycle, const uint8_t *src, uint32_t bytes);
    void onMscalBegin(uint32_t pc, uint32_t top, uint32_t itop);
    void onMscalEnd();
    uint32_t onGifSubmit(uint8_t path); // returns a tag stored with the queued arbiter packet
    void onGifDrain(uint8_t path, const uint8_t *data, uint32_t size, uint32_t tag);
    void onVBlank(uint64_t tick, uint64_t dispfb2);

    // Render-thread side: the newest published record (back = 0), or older ones (back = 1, 2); null if none.
    std::shared_ptr<const TickRecord> latest(unsigned back = 0);
}
