// Game hooks (see include/game_hooks.h): entry/exit host hooks on recompiled guest functions by
// guest address, via the dense function table's entry slot (the PS2X_TRACE_CALLS mechanism).
#include "game_hooks.h"
#include "starter_car.h"
#include "hfr_recorder.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace ps2x::hooks
{
    namespace
    {
        struct Site
        {
            uint32_t address = 0;
            PS2Runtime::RecompiledFunction original = nullptr;
            std::vector<Hook> hooks;
        };

        // Mutated only before/while installAll() runs (before the game thread starts); read-only after.
        std::vector<Hook> &pending()
        {
            static std::vector<Hook> p;
            return p;
        }
        std::vector<Site> &sites()
        {
            static std::vector<Site> s;
            return s;
        }
        bool g_installed = false;

        void trampoline(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t pc = ctx->pc;
            Site *site = nullptr;
            for (auto &s : sites())
                if (s.address == pc)
                {
                    site = &s;
                    break;
                }
            if (!site)
                return; // unreachable: only exact entry slots are swapped

            HookCall call{};
            call.rdram = rdram;
            call.ctx = ctx;
            call.address = pc;
            call.ra = GPR_U32(ctx, 31);
            call.sp = GPR_U32(ctx, 29);
            for (auto &h : site->hooks)
                if (h.onEnter)
                {
                    call.user = h.user;
                    h.onEnter(call);
                }
            for (int i = 0; i < 4; ++i)
                call.a[i] = GPR_U32(ctx, 4 + i);

            site->original(rdram, ctx, runtime);

            for (auto it = site->hooks.rbegin(); it != site->hooks.rend(); ++it)
                if (it->onExit)
                {
                    call.user = it->user;
                    it->onExit(call);
                }
        }
    }

    bool add(const Hook &hook)
    {
        if (hook.address == 0 || g_installed)
        {
            if (g_installed)
                std::fprintf(stderr, "[hooks] %s at %06x registered after install; ignored\n", hook.name, hook.address);
            return false;
        }
        pending().push_back(hook);
        return true;
    }

    void installAll(PS2Runtime &runtime)
    {
        if (g_installed)
            return;
        // Built-in game modules (each adds hooks only when its option is on).
        ps2x::starter::installHooks();
        ps2x::hfr::installHooks(runtime); // G4b recorder: PS2X_HFR_RECORD=1 only

        for (const Hook &h : pending())
        {
            Site *site = nullptr;
            for (auto &s : sites())
                if (s.address == h.address)
                    site = &s;
            if (!site)
            {
                if (!runtime.hasFunction(h.address))
                {
                    std::fprintf(stderr, "[hooks] %s: no recompiled function at %06x\n", h.name, h.address);
                    continue;
                }
                sites().push_back(Site{h.address, nullptr, {}});
                site = &sites().back();
            }
            site->hooks.push_back(h);
        }
        pending().clear();
        // Resolve originals after the vector stops growing (a PS2X_TRACE_CALLS trampoline may already
        // own the slot: it is chained, since both look their entry up by ctx->pc).
        for (auto &s : sites())
        {
            s.original = runtime.lookupFunction(s.address);
            runtime.replaceFunction(s.address, &trampoline);
            for (const auto &h : s.hooks)
                std::fprintf(stderr, "[hooks] %s hooked at %06x\n", h.name, s.address);
        }
        g_installed = true;
    }

    uint32_t read32(const uint8_t *rdram, uint32_t address)
    {
        uint32_t v;
        std::memcpy(&v, rdram + (address & (PS2_RAM_SIZE - 4u)), 4);
        return v;
    }

    void write32(uint8_t *rdram, uint32_t address, uint32_t value)
    {
        std::memcpy(rdram + (address & (PS2_RAM_SIZE - 4u)), &value, 4);
    }

    uint32_t reg32(const R5900Context *ctx, int index)
    {
        return GPR_U32(ctx, index);
    }

    void setReg32(R5900Context *ctx, int index, uint32_t value)
    {
        SET_GPR_U32(ctx, index, value);
    }
}
