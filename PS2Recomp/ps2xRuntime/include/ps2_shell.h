#pragma once
// SH1: native macOS shell (SDL3 window + CAMetalLayer present, SDL3 gamepads/audio, NSMenu menu bar,
// graphics.json). Opt-in with PS2X_SHELL=sdl3; the raylib shell stays the default. Plain C++ header:
// no raylib and no Cocoa types, so it can be included from both sides.
#include <cstddef>
#include <cstdint>

namespace ps2x::shell
{
#if defined(__APPLE__)
    // PS2X_SHELL=sdl3 (read once). Headless runs still use SDL3 for audio, but never SDL video.
    bool sdl3Selected();
    // sdl3Selected() and a window will be opened (not PS2X_HEADLESS no-window mode).
    bool sdl3Window();

    // Main thread. window=false: SDL audio only (no NSApp, no Dock icon, no gamepads).
    bool init(const char *title, bool window);
    void shutdown();

    // Main thread: the latest 1x display frame (RGBA, GS alpha ignored). hiKey != 0 pairs it with the
    // Metal backend's direct scaled texture (PS2X_GS_SCALE > 1), presented instead when available.
    void submitFrame(const uint8_t *rgba, uint32_t strideBytes, uint32_t rows, uint32_t width, uint32_t height, uint64_t hiKey);
    // True when submitFrame wants hiKey (Metal direct scaled present is on).
    bool wantsHiKey();
    // Main thread: pump events, update input, present into the layer (v-synced). False = quit requested.
    bool pumpAndPresent();

    // Audio: an SDL3 stream pulling 48 kHz s16 stereo from cb (on SDL's audio thread).
    using AudioCallback = void (*)(void *buffer, unsigned int frames);
    bool startAudio(AudioCallback cb, int sampleRate, int channels, bool muted);
    void stopAudio();

    // Input snapshot (written on the main thread by pumpAndPresent, read from the game thread).
    // Key and button codes are raylib's (KEY_*, GAMEPAD_BUTTON_*, GAMEPAD_AXIS_*) so the existing
    // mapping in ps2_pad.cpp is reused unchanged.
    bool keyDown(int raylibKey);
    bool gamepadAvailable(int index);
    bool gamepadButtonDown(int index, int raylibButton);
    float gamepadAxis(int index, int raylibAxis);
#else
    inline bool sdl3Selected() { return false; }
    inline bool sdl3Window() { return false; }
    inline bool init(const char *, bool) { return false; }
    inline void shutdown() {}
    inline void submitFrame(const uint8_t *, uint32_t, uint32_t, uint32_t, uint32_t, uint64_t) {}
    inline bool wantsHiKey() { return false; }
    inline bool pumpAndPresent() { return true; }
    using AudioCallback = void (*)(void *buffer, unsigned int frames);
    inline bool startAudio(AudioCallback, int, int, bool) { return false; }
    inline void stopAudio() {}
    inline bool keyDown(int) { return false; }
    inline bool gamepadAvailable(int) { return false; }
    inline bool gamepadButtonDown(int, int) { return false; }
    inline float gamepadAxis(int, int) { return 0.0f; }
#endif
}
