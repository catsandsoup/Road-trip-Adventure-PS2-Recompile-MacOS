# Native sound engine (SNDMOD.IRX replacement)

The game's IOP sound driver SNDMOD.IRX is replaced by native C++: a 1:1 port of the driver
(sequencer, SE allocation, BGM transport, fades, engine, radio streaming) running on a software
SPU2. No IOP/R3000 code runs. All sound data (TVB banks, TSQ sequences, radio VAGs) is read at run
time from the user's disc image via the LSNs the game sends. Protocol + structure: `SNDMOD.md`.

## Architecture

| IRX piece | Native replacement |
| --- | --- |
| `thread_main` / `sifrpc_main` | `SndmodService::handleRpc` → `SndmodDriver::sifrpcMain` (same command numbers, same reply buffer semantics: only CtrlMono/Busy/ReadData/Bugs write a reply) |
| `thread_sddr` woken by `SetAlarm(16,666 µs)` | `SndmodDriver::threadSddr()` run from the **audio render clock**: one tick per 799.968 rendered frames at 48 kHz |
| `thread_cdvd` (load_tvbf / load_tsqf / load_radio) | host worker thread woken after every tick; reads from `PS2X_CD_IMAGE` with the driver mutex released during the read (as the IRX blocked in `sceCdSync`) |
| LIBSD + SPU2 hardware | `Spu2` class: libsd-compatible `setParam/setSwitch/setAddr/setCoreAttr/setEffectAttr/procBatch`, 2 MiB sound RAM, 2×24 voices |
| audio out | `ps2x::iop::renderNativeAudio()` pulled by a raylib `AudioStream` callback (48 kHz s16 stereo; miniaudio/CoreAudio resamples if the device rate differs) |

A single mutex serialises RPCs, ticks/rendering and cdvd state changes (the role the IOP's
priority scheduler played). If no audio device pulls samples for 250 ms (headless), a wall-clock
fallback thread renders and ticks so busy polls and BGM timing still progress.

### SPU2 model (`spu2.cpp`)
* PS-ADPCM decode (5 filters, shift 13–15 → 9), loop flags: loop-start sets LSAX, loop-end jumps
  to LSAX, loop-end without repeat stops the voice and sets ENDX.
* Pitch: 0x1000 = 48 kHz, capped at 0x3fff, 12-bit fractional counter.
* ADSR: attack/decay/sustain/release per the documented SPU envelope rules (rate→step/counter,
  exponential attack slow-down above 0x6000, exponential decay/release scaling); key-off → release.
* Volumes: fixed (value·2) or sweep mode (bit 15) using the same envelope stepper (radio/engine init
  uses sweeps 0x8013/0xa014).
* Mixing: VMIXL/R dry, VMIXEL/R wet, MMIX gates, MVOL per core, core0 output → core1 external input
  (× BVOL), final = core1 output, clamped to s16.

## Verified (and how)

1. **Sequencer timing/pitch/volume vs RTAO's PAL BGM_01 witness** — offline harness
   `game/tests/audio/sndmod_replay bgm <iso> BGM_01.TSQ BGM.TVB 1 8000 out.wav trace.txt`:
   * sequence offset 0xD0, 36 channels; first channel-0 backward F8 exactly **7,367 ticks** after
     start, targets 0x193 / 0x357 / 0x51A (start+3) — all match;
   * first audible key-on of channel 0 at **tick 460**: tone slot 12, pitch **0x035A**, volume
     **1798/3712**, ADSR **0x0D0D1EEE**, SSA = 0x160000 + **119344** (TVB slot 12 span start); key-off
     at **tick 470** — all match RTAO's documented values.
   * All 12 BGM_xx.TSQ start, key on (966–3080 key-ons per 3000 ticks) and produce non-silent output.
2. **Game route** (`boot_to_title.txt`, 99 s, `PS2X_AUDIO_DUMP`): 4 TVB + 3 TSQ loads from the ISO
   (busy poll clears after one tick), title/menu SEs (0x328, 0x01f, 0x01a, 0x022, 0x023, 0x025, 0x032)
   key on and are audible from ~5 s, BGM 0x104 (ROOM_1 index 4) selected and started at tick 3275
   and playing until the end (−11…−13 dBFS RMS). Non-silent 19/20 five-second windows.
   **Output path:** trace counters show every rendered frame came from the raylib/CoreAudio device
   callback (`device_frames=4,319,040`, `fallback_frames=0` at tick 5400). A second run with
   `PS2X_NATIVE_AUDIO=0` (no host stream) ran entirely on the fallback clock (`device_frames=0`,
   `fallback_frames=4,319,071`), still loaded all banks at tick 1 and started BGM 0x104 at tick 3276.
   Per-command log with tick stamps (`PS2X_SNDMOD_LOG`), driver trace (`PS2X_SNDMOD_TRACE`).
3. **Radio** (`sndmod_replay radio <iso> <tune> <time> <ticks>`): double-buffered 30 s parts load
   from the ISO, part switch at time 1800 is seamless (no gap at 10 ms resolution), 1CH gives
   distinct L/R; 3CH L/R files are nearly identical on disc (98,519 differing bytes), so its output
   is near-mono by content.
4. **SE vs RTAO's SFX table** (`sndmod_replay se <iso> CQ_MAIN.TSQ CQ_MAIN.TVB 0x1a 120`):
   confirm request 0x001A gets priority 144, tone 5, SSA 0x1b0000 + **41216** (RTAO span
   41216..42640); the sequence forks (F9) a second channel, giving an L-only and an R-only voice.
5. **Engine** (`sndmod_replay engine <iso> 0 600`): pitch follows rpm through the 5-point curve
   (zero-crossing rate 290→1800→185 /s over a 0→10000→0 sweep); disc flip swaps mute/loud tone.

## Approximate / unverified

* **Interpolation**: 4-point Catmull-Rom instead of the hardware Gaussian table.
* **Reverb**: SPU2 reverb presets are not reproduced; a Schroeder approximation per libsd mode is
  used. No BGM sequence issues E6/E7, so the effect mode stays OFF (as on hardware) and nothing
  reaches the wet path in practice. Reverb work-area writes to sound RAM are not modelled.
* **Mix gain chain** follows the documented register semantics but is not compared against real
  hardware output. Loud BGM passages clip occasionally (0.011 % of samples in BGM_01, 0.039 % on the
  boot route).
* Not modelled: noise generator and pitch modulation (the driver only ever clears NON/PMON), IRQ,
  SPU2 capture buffers (`sddrReadData`, 0x1f, returns zeros), key-on latency, ADMA.
* Engine/radio RPCs are verified only through the harness; the scripted game route never sends
  EngineRpms/RadioTune/RadioPlay. In-race behaviour needs a driving route.
* NATIVE deviations (marked in the source): bounds checks where the IRX would index past its static
  arrays (bank ≥ 4, tone ≥ 1024, channel ≥ 48 …; wild sequence pointers read as end-of-data); radio
  parts read one extra sector so the last bytes of the 256 KiB buffer are stream data instead of
  stale heap; keys 73–95 reproduce the halfwords that follow `freqtbl` in .data.
* Timing derives from rendered audio; if the host device rate drifts from the game clock, music runs
  at audio-clock speed (as on hardware, where the IOP alarm is independent of the EE).

## Files changed in PS2Recomp-audio (merge list)

New:
* `ps2xIOP/src/modules/sndmod/spu2.h`, `spu2.cpp` — SPU2 model
* `ps2xIOP/src/modules/sndmod/sndmod_driver.h`, `sndmod_driver.cpp` — driver port
* `ps2xIOP/include/ps2x/iop/native_audio.h`, `ps2xIOP/src/native_audio.cpp` — host audio bridge

Modified:
* `ps2xIOP/src/modules/sndmod.cpp` — stub replaced by the service hosting the engine
* `ps2xIOP/CMakeLists.txt` — adds the three new .cpp files
* `ps2xRuntime/src/lib/ps2_runtime.cpp` — `#include "ps2x/iop/native_audio.h"`, the
  `startNativeAudioStream/stopNativeAudioStream` helpers (anonymous namespace before
  `~PS2Runtime`), `stopNativeAudioStream()` at the top of the destructor, `startNativeAudioStream()`
  after `InitAudioDevice()`. (Everything else in that file was synced from the main tree.)

Other files that differ between PS2Recomp-audio and the main tree (`gs_cpu_backend.cpp`,
`gs_frontend.cpp`, `ps2_gif_arbiter.cpp`, `EeScheduler.cpp`, `fpu_translator.cpp`, and the
baseline-modified `Pad.cpp`, `RPC.cpp`, `iop_rpc.*`, …) are **older copies, not audio changes —
do not copy them over the main tree**.

`ps2xRuntime/include/ps2_runtime_macros.h` was copied unchanged from the main tree (FPU helpers
needed by the regenerated code); no audio change there.

Game project: `game/docs/AUDIO_ENGINE.md`, `game/docs/SNDMOD.md` (protocol section),
`game/tests/audio/{sndmod_replay.cpp, build.sh, wav_report.py}`.

## Driver data tables (read from the user's disc at start-up)

`freqtbl` is computed by formula. The small parameter tables in SNDMOD's .data section (`end_dat`, the LIBSD
batch tables `bats.24` / `bats.27`, the engine tone pairs `wave.168` and the engine curve `data.169`) are
**not** in the source: `sndmod_driver.cpp` loads them at start-up from `SNDMOD.IRX` inside the user's own
ISO (ISO9660 root lookup + ELF segment mapping), verified byte-identical to the values the port was first
developed against. No samples, sequences, tables or IRX code bytes are included in the repository.

## Build / run

```
cd PS2Recomp-audio
cmake -S . -B out/rta -G Ninja -DCMAKE_BUILD_TYPE=Release -DPS2X_RUNNER_DIR=<project root>/work/generated \
  -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_STUDIO=OFF -DPS2X_ENABLE_AGRESSIVE_LOGS=OFF -DPS2X_ENABLE_LTO=OFF
cmake --build out/rta --target ps2EntryRunner
cd ../work && RTA_RUNNER=$PWD/../PS2Recomp-audio/out/rta/ps2xRuntime/ps2EntryRunner \
  PS2X_SNDMOD_LOG=$PWD/audio/cmds.log PS2X_SNDMOD_TRACE=$PWD/audio/trace.log PS2X_AUDIO_DUMP=$PWD/audio/out.wav \
  PS2X_INPUT_SCRIPT=../game/tests/input/boot_to_title.txt perl -e 'alarm 100; exec @ARGV' ../game/tools/run.sh
python3 ../game/tests/audio/wav_report.py audio/out.wav 5
```
Switches: `PS2X_SNDMOD_STUB=1` (old silent acknowledging stub), `PS2X_NATIVE_AUDIO=0` (no host
stream; the fallback clock still runs the driver). Offline harness: `game/tests/audio/build.sh`
then `work/audio/sndmod_replay bgm|se|radio|engine|log …` (usage in the source header).
