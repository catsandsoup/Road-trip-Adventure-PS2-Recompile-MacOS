#pragma once

// Pull-model bridge between native IOP sound services (e.g. the SNDMOD
// replacement) and the host audio device.  The runtime registers nothing here;
// it only calls renderNativeAudio() from its audio-device callback and reports
// that it is doing so via markNativeAudioDeviceActive().
#include <cstdint>

namespace ps2x::iop
{
    constexpr uint32_t kNativeAudioSampleRate = 48000u;
    constexpr uint32_t kNativeAudioChannels = 2u;

    // Renders interleaved stereo s16 frames.  Called on the host audio thread.
    using NativeAudioRenderFn = void (*)(void *user, int16_t *interleaved, uint32_t frames);

    void setNativeAudioSource(NativeAudioRenderFn fn, void *user);
    void clearNativeAudioSource(void *user);

    // Fills `interleaved` (frames * 2 samples).  Returns false and writes
    // silence when no native source is registered.
    bool renderNativeAudio(int16_t *interleaved, uint32_t frames);

    // True once the host has an audio stream pulling renderNativeAudio().
    void markNativeAudioDeviceActive(bool active);
    [[nodiscard]] bool nativeAudioDeviceActive();
}
