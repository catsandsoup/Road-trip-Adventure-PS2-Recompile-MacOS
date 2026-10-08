# Building and running (developer preview, macOS arm64)

You need **your own disc**, extracted locally. Nothing generated from it may be committed or shared. Set `R` to your checkout root (this repo as `game/`, the PS2Recomp fork as `PS2Recomp/`, local-only `disc/` and `work/` beside them).

```bash
R=<your-root>
# recompiler tools
cmake -S $R/PS2Recomp -B $R/PS2Recomp/out/build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build $R/PS2Recomp/out/build --target ps2_recomp ps2_analyzer
# game (dev config: LTO off; VU1 generator wired in at configure time)
cmake -S $R/PS2Recomp -B $R/PS2Recomp/out/rta -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DPS2X_RUNNER_DIR=$R/work/generated -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_STUDIO=OFF \
  -DPS2X_ENABLE_AGRESSIVE_LOGS=OFF -DPS2X_ENABLE_LTO=OFF \
  -DPS2X_VU1_CATALOG_DIR=$R/work/vu1cat -DPS2X_VU1_GENERATOR=$R/game/tools/vu1recomp/vu1recomp.py -DPS2X_VU1GEN_DIR=$R/work/vu1gen
cmake --build $R/PS2Recomp/out/rta --target ps2EntryRunner     # full ~10-20 min (any runtime header change = full); incremental ~1 min
# regenerate EE C++ after a translator or rta.toml change. NO -a/-t on rsync: unchanged files keep their mtimes, so ninja rebuilds only changed units
cd $R/work && rm -rf generated_next && ../PS2Recomp/out/build/ps2xRecomp/ps2_recomp rta.toml && rsync -r --checksum --delete generated_next/ generated/
# play (window; F1 = debug panel; keys: arrows, X=Cross, C=Circle, Z=Square, V=Triangle, Enter=Start)
cd $R/work && ../game/tools/run.sh
# automated route (headless; writes work/routes/<route>/report.json and frames)
python3 $R/game/tests/run_route.py boot_to_title --every 5
# output-neutral gate + throughput (deterministic golden recipe; runner defaults to the main tree, or pass a track's runner)
$R/game/tests/det_capture.sh /tmp/x race [$R/PS2Recomp-gs/out/rta/ps2xRuntime/ps2EntryRunner] [seconds]
```
Gotchas:
- **After ANY regeneration of `work/rta.toml`** (analyzer/Ghidra rerun, fresh setup, the future first-run flow), run `python3 game/tools/apply_stub_policy.py work/rta.toml --functions work/rta_functions.csv`, then `--check`. Without it, the wrong HLE stubs come back and every session-3 fix (race 3D, floor, models, Adventure hang) silently regresses. The policy (`game/config/stub_policy.toml`, names only, publishable) lists 46 run-as-game-code functions (libgraph ×12, libvu0 ×29, libm float ×5) and 46 forced host stubs (kernel/SIF wrappers).
- `work/generated` is shared by `PS2Recomp/out/rta` and `PS2Recomp-gs/out/rta`: **never regenerate while the `-gs` track is mid-build or mid-measurement.**
- **Never kill or disturb the user's own game sessions.** Test runs use `PS2X_HEADLESS=1` (hidden window **and muted audio**) and a separate save dir (`RTA_SAVE_DIR`). Scripted input is exclusive by default.
- **Never let a test run be audible.** Every tree must carry the `hostAudioMuted()` patch in `ps2_runtime.cpp`.
- **Never renice jobs down.** The user wants full speed, and on macOS a non-root process can't renice back up.
- Profiling recipe: start `det_capture.sh` in the background, wait ~75 s (race section), then `sample $(pgrep -n -f "^<runner path>") 12 -f out.txt`. Note that `<deduplicated_symbol>` called from `vu1rec_*` is the folded `vr_fmac_flags<>` (VU1), not GS code.
- After moving folders, delete `out/*` and reconfigure (CMake/FetchContent cache absolute paths).
- After an **interrupted** build plus patch, touch the patched files or rebuild clean. A stale `ps2_runtime.cpp.o` once produced 1.7M bogus VU1 fallbacks.
- Use `perl -e 'alarm N; exec …'` as the timeout (no `timeout` binary). Trace code must `fflush`, because SIGALRM kills buffered stdio.
- zsh: don't `echo ======` (zsh expands `=word`).
- Homebrew dependencies: cmake, ninja, pkgconf, ffmpeg, sdl2. Python has PIL but no numpy. Node 22 runs `.ts` directly (`node file.ts`).


## Environment switches and test tools
See `tests/` (`run_route.py`, `det_capture.sh`, `compare_frames.py`, `check_jump_tables.py`) and the comments at the top of each script.
