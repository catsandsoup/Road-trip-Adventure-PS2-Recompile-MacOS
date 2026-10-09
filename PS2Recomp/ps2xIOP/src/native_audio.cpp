#include "ps2x/iop/native_audio.h"

#include <atomic>
#include <cstring>
#include <mutex>

namespace ps2x::iop
{
    namespace
    {
        std::mutex g_sourceMutex;
        NativeAudioRenderFn g_fn = nullptr;
        void *g_user = nullptr;
        std::atomic<bool> g_deviceActive{false};
    }

    void setNativeAudioSource(NativeAudioRenderFn fn, void *user)
    {
        std::lock_guard<std::mutex> lock(g_sourceMutex);
        g_fn = fn;
        g_user = user;
    }

    void clearNativeAudioSource(void *user)
    {
        std::lock_guard<std::mutex> lock(g_sourceMutex);
        if (g_user == user)
        {
            g_fn = nullptr;
            g_user = nullptr;
        }
    }

    bool renderNativeAudio(int16_t *interleaved, uint32_t frames)
    {
        std::lock_guard<std::mutex> lock(g_sourceMutex);
        if (!g_fn)
        {
            std::memset(interleaved, 0, static_cast<size_t>(frames) * kNativeAudioChannels * sizeof(int16_t));
            return false;
        }
        g_fn(g_user, interleaved, frames);
        return true;
    }

    void markNativeAudioDeviceActive(bool active)
    {
        g_deviceActive.store(active);
    }

    bool nativeAudioDeviceActive()
    {
        return g_deviceActive.load();
    }
}
