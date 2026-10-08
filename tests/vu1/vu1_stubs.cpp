// Link stubs for vu1_replay: runtime symbols referenced by the VU1 sources but
// never reached in the replay (a packet sink is always installed, memory is null).
#include "runtime/ps2_memory.h"
#include "runtime/gs/gs_frontend.h"
#include <cstdio>
#include <cstdlib>

void PS2Memory::submitGifPacket(GifPathId, const uint8_t *, uint32_t, bool, bool) { std::abort(); }
void GS::processGIFPacket(const uint8_t *, uint32_t) { std::abort(); }
