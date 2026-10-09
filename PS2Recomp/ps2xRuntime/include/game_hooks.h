#pragma once
// Game hooks: run host code at the entry (and optionally the return) of a recompiled guest function,
// selected by its guest address. Built on the same mechanism as PS2X_TRACE_CALLS: only the function's
// entry slot in the dense function table is swapped; mid-function resume slots are untouched.
// Zero cost when nothing is registered: no slot is swapped and the dispatch path is unchanged.
//
// Usage (before PS2Runtime::run(), or from a game module's install function called by
// ps2x::hooks::installAll):
//   ps2x::hooks::add({0x22b568, &onEnter, &onExit, userData, "starter-car"});
// onEnter may change argument registers (ctx->r[4..7]) before the original runs. It sees the caller's
// return address in HookCall::ra. onExit (optional) runs after the original returns, with the entry
// registers kept in HookCall. Hooks run on the game (EE) thread.
//
// This is the seed for game modules (GOALS M) and later renderer hooks (G4).
#include <cstdint>

class PS2Runtime;
struct R5900Context;

namespace ps2x::hooks
{
    struct HookCall
    {
        uint8_t *rdram;
        R5900Context *ctx;
        uint32_t address; // hooked guest function
        uint32_t ra;      // caller's return address at entry
        uint32_t a[4];    // a0..a3 at entry (after onEnter changed them)
        uint32_t sp;
        void *user;
    };

    using EnterFn = void (*)(HookCall &call);
    using ExitFn = void (*)(HookCall &call);

    struct Hook
    {
        uint32_t address = 0;
        EnterFn onEnter = nullptr;
        ExitFn onExit = nullptr;
        void *user = nullptr;
        const char *name = "";
    };

    // Register a hook. Before installAll(): queued. Several hooks may share an address; they run in
    // registration order (enter) and reverse order (exit). Returns false for address 0.
    bool add(const Hook &hook);

    // Called once from PS2Runtime::run() after PS2X_TRACE_CALLS is installed (hooks wrap trace
    // trampolines, so traces still see the original arguments' effect on the callee). Also calls the
    // built-in game modules' install functions (they decide from their settings whether to add hooks).
    void installAll(PS2Runtime &runtime);

    // Guest RAM helpers for hook code (addresses are masked to the 32 MiB EE RAM).
    uint32_t read32(const uint8_t *rdram, uint32_t address);
    void write32(uint8_t *rdram, uint32_t address, uint32_t value);
    uint32_t reg32(const R5900Context *ctx, int index);
    void setReg32(R5900Context *ctx, int index, uint32_t value); // sign-extends like the EE's 32-bit ops
}
