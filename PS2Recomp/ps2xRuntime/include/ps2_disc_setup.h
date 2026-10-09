#pragma once
// PK1: first-run disc setup for the packaged app (GOALS X1). Used only when the runner is started
// without a guest ELF argument (the .app case). The legacy argv/env flow (run.sh, tests) never calls it.
//
// The user's own disc image (ISO, or BIN/CUE with 2352-byte raw sectors) is validated as SLES-51356
// (ISO9660 PVD, SYSTEM.CNF BOOT2 = SLES_513.56, boot ELF present, image not truncated), its path is
// stored in <config dir>/general.json, and its files are extracted once into <config dir>/disc so every
// downstream reader (CD search, VFS, IOP, SNDMOD) runs the exact code path the gates cover. A raw BIN is
// also converted once to <config dir>/disc.iso because SNDMOD and the LBN fallback read 2048-byte sectors.
// Nothing disc-derived is ever shipped: all of it is produced on the user's machine from their disc.
#include <filesystem>
#include <string>

namespace ps2x::disc
{
    // PS2X_CONFIG_DIR, else ~/Library/Application Support/RoadTripAdventure (same rule as graphics.json).
    std::filesystem::path configDir();

    enum class Status
    {
        Good,
        FailedToOpen,
        NotAPs2Disc,
        IncorrectGame,
        Truncated,
        PrepareFailed,
    };

    struct Result
    {
        Status status = Status::FailedToOpen;
        std::string message; // user-facing text (empty when Good)
        bool ok() const { return status == Status::Good; }
    };

    // A .cue resolves to the image named by its FILE line; anything else is returned unchanged.
    std::filesystem::path resolveImagePath(const std::filesystem::path &picked);

    // Structure + serial check of the image (no writes).
    Result validate(const std::filesystem::path &image);

    // general.json {"version":1,"disc":"<path>"}; false when missing, corrupt or without a disc path.
    bool loadStoredDisc(std::filesystem::path &imageOut);
    bool storeDisc(const std::filesystem::path &image);

    // Validate, then extract (and convert a raw BIN) into the config dir unless an up-to-date copy exists.
    // On success elfOut = the boot ELF on the host, isoOut = a 2048-byte-sector image of the disc.
    Result prepare(const std::filesystem::path &image, std::filesystem::path &elfOut, std::filesystem::path &isoOut);

    // main(): no guest ELF argument. Picks the disc (PS2X_CD_IMAGE, else general.json, else the
    // first-run picker unless PS2X_HEADLESS=1), prepares it, sets PS2X_CD_IMAGE and (when unset)
    // PS2X_MC_ROOT=<config dir>/mc0, and returns the ELF path to boot. False: print/abort (exit 1).
    bool resolveLaunch(std::string &elfPathOut);

    // Executable lives in a .app bundle (…/X.app/Contents/MacOS/…).
    bool runningFromAppBundle();

    // Child process entry for the macOS picker (argv[1] == kPickerArg). Returns the process exit code.
    inline constexpr const char *kPickerArg = "--ps2x-pick-disc";
    int runPickerProcess(int argc, char *argv[]);
}
