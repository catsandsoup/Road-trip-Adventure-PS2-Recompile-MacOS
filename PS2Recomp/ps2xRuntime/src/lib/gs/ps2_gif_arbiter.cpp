#include "runtime/gs/ps2_gif_arbiter.h"
#include "hfr_recorder.h"
#include <algorithm>
#include <cstring>
#include <unordered_map>

// G4b recorder attribution tags, keyed by the packet's heap buffer (stable while the packet is queued: the
// queue's reallocation and stable_sort move the vectors, not their buffers). Kept here instead of a
// GifArbiterPacket field so the recorder does not change a header every generated TU includes.
// EE thread only; touched only when ps2x::hfr::g_on.
static std::unordered_map<const uint8_t *, uint32_t> s_hfrTags;

// Path of the packet currently being processed (diagnostics: vertex/triangle traces).
thread_local uint8_t g_gifCurrentPath = 0u;

GifArbiter::GifArbiter(ProcessPacketFn processFn)
    : m_processFn(std::move(processFn))
{
}

bool GifArbiter::isImagePacket(const uint8_t *data, uint32_t sizeBytes)
{
    if (!data || sizeBytes < 16u)
        return false;

    uint64_t tagLo = 0;
    std::memcpy(&tagLo, data, sizeof(tagLo));
    const uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3u);
    return flg == 2u;
}

void GifArbiter::submit(GifPathId pathId, const uint8_t *data, uint32_t sizeBytes, bool path2DirectHl)
{
    if (!data || sizeBytes < 16 || !m_processFn)
        return;

    GifArbiterPacket pkt;
    pkt.pathId = pathId;
    pkt.path2DirectHl = (pathId == GifPathId::Path2) && path2DirectHl;
    pkt.path3Image = (pathId == GifPathId::Path3) && isImagePacket(data, sizeBytes);
    const uint32_t hfrTag = ps2x::hfr::g_on ? ps2x::hfr::onGifSubmit(static_cast<uint8_t>(pathId)) : 0u;
    pkt.data.resize(sizeBytes);
    std::memcpy(pkt.data.data(), data, sizeBytes);
    if (ps2x::hfr::g_on)
        s_hfrTags[pkt.data.data()] = hfrTag;
    m_queue.push_back(std::move(pkt));
}

void GifArbiter::drain()
{
    if (!m_processFn)
        return;

    std::stable_sort(m_queue.begin(), m_queue.end(),
                     [](const GifArbiterPacket &a, const GifArbiterPacket &b)
                     {
                         // DIRECTHL cannot preempt PATH3 IMAGE transfers.
                         if (a.path2DirectHl != b.path2DirectHl || a.path3Image != b.path3Image)
                         {
                             if (a.path3Image && b.path2DirectHl)
                                 return true;
                             if (a.path2DirectHl && b.path3Image)
                                 return false;
                         }
                         return pathPriority(a.pathId) < pathPriority(b.pathId);
                     });

    for (size_t i = 0; i < m_queue.size(); ++i)
    {
        auto &pkt = m_queue[i];
        if (!pkt.data.empty())
        {
            g_gifCurrentPath = static_cast<uint8_t>(pkt.pathId);
            if (ps2x::hfr::g_on)
            {
                const auto it = s_hfrTags.find(pkt.data.data());
                ps2x::hfr::onGifDrain(static_cast<uint8_t>(pkt.pathId), pkt.data.data(), static_cast<uint32_t>(pkt.data.size()),
                                      it != s_hfrTags.end() ? it->second : 0u);
            }
            m_processFn(pkt.data.data(), static_cast<uint32_t>(pkt.data.size()));
            g_gifCurrentPath = 0u;
        }
    }
    m_queue.clear();
    if (ps2x::hfr::g_on)
        s_hfrTags.clear();
}

uint8_t GifArbiter::pathPriority(GifPathId id)
{
    return static_cast<uint8_t>(id);
}
