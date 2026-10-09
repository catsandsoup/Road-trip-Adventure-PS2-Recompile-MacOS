#pragma once
// GOALS Q5: opt-in starting-car chooser for a new Adventure.
//
// Original behaviour (Q5a): FUN_00270550 picks one of six fixed starter bodies by the game's main-loop
// frame counter mod 6 and calls 0x22b568(body), which writes every save field (ownership bit, paint
// word, body bytes +0x04/+0x0c, per-body paint). With a choice set we only replace a0 of that call when
// it comes from the six picker cases, so the game itself writes the save. An optional colour replaces
// the low 24 bits (body tones) of the paint word the game just wrote; the wheel byte is kept.
//
// Settings: PS2X_STARTER_BODY=<id|random> and PS2X_STARTER_PAINT=<hex low-24-bit paint word|default>
// win; otherwise interactive runs (not PS2X_HEADLESS, not PS2X_DETERMINISTIC) read "starter_body" /
// "starter_paint" from general.json in the settings dir (PS2X_CONFIG_DIR overrides). Default: random
// (original game). Writes merge keys: other general.json keys are preserved.
#include <cstdint>
#include <string>

namespace ps2x::starter
{
    constexpr int kRandom = -1;        // original random pick
    constexpr int64_t kDefaultPaint = -1; // the body's own default colour

    // Menu model: entry 0 = Random (original), then the six original starter bodies.
    int optionCount();
    int optionBody(int index);
    std::string optionLabel(int index);

    int paintOptionCount();
    int64_t paintOptionWord(int index);
    const char *paintOptionLabel(int index);

    int selectedBody();
    int64_t selectedPaint();
    // Thread-safe; applies at the next new Adventure; persisted to general.json.
    void selectBody(int body);
    void selectPaint(int64_t paintWord);

    // Called from ps2x::hooks::installAll: adds the 0x22b568 hook when a choice is set, or when the
    // interactive sdl3 menu can set one later.
    void installHooks();
}
