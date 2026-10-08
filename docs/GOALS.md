# Road Trip Adventure (PAL, SLES-51356): native recomp goals

**Goal.** *Road Trip Adventure* runs **1:1 natively on macOS** (Apple Silicon first; Intel Mac, Windows, Linux and browser later) as an installable app, the way the N64Recomp-family ports (e.g. Zelda64Recomp) do. The game's own code is statically recompiled, with no BIOS. The user's own disc (BIN/CUE or ISO) is the only input. On top of exact original behaviour it offers a **native settings UI** for resolution, aspect ratio, frame rate, controls, audio, mods and so on.

"1:1" means the game logic, physics, timing, music, UI and content behave exactly like the PAL original on a real PS2. Enhancements are opt-in, and each one is proven not to change gameplay.

**Hard rules:** no emulation in the shipping build (§2), and nothing derived from the disc is ever committed or published (§9).

---
**Approvals (user, 2026-10-08):** Metal becomes the default backend when the G2f gate passes (the CPU backend stays as G9's reference); reference frames may be replaced when diffs are explained and verified.

## 1. Definition of done (all must hold)
1. Every success criterion in §3–§8 is met (the §5.7 D-items are opt-in extras: D1–D4 are required, D5–D7 are research deliverables) and checked off, with evidence linked in `CHANGELOG.md`.
2. All gameplay routes in §6 pass in CI-style automated runs on macOS arm64, and the user signs off after a manual playthrough.
3. The shipping build contains no VU1 interpreter, no IOP/R3000 emulator and no physical IRX execution (§2 gate).
4. A clean macOS user can download the app, pick their disc, and play start to finish. Saves persist, and every settings option works and persists.
5. **End deliverable (user, 2026-10-08): the Mac version (priority) ships as a packaged, double-clickable `.app` (in a DMG or similar, unsigned/un-notarised: the user has no Apple Developer licence), so nobody has to see, build or run from source.** Players need no Xcode, CMake, Python, Homebrew or terminal. The recompiled game code is compiled into the app binary, SDL/Metal/FFmpeg and other libraries are bundled, and the first run asks for the user's own disc (no disc data inside the app; see §9). Source, build guide and tools stay in the public repo for those who want them. Same packaging goal for Windows/Linux later.

## 2. Architecture principle: no emulation left at runtime
| PS2 part | Shipped as | Status |
| --- | --- | --- |
| EE (R5900) game code | statically recompiled C++ | done (10,765 functions) |
| EE FPU / VU0 macro / MMI | inline C++/SIMD with PS2-accurate semantics | done (FPU + VU0 semantics fixed) |
| VU1 microcode | statically recompiled per microprogram | done: main tree, all 8 dispatch slots, 0 fallbacks on boot and race |
| GS | native renderer (Metal on macOS; Vulkan/D3D12/WebGPU later), consuming GIF packets | **Metal is the default** (2026-10-09, byte-identical to the CPU reference; 1x-8x scale); CPU backend kept as reference |
| IOP + IRX modules | native host services, no R3000 execution | done: all 7 IRXs native |
| BIOS / kernel | host syscalls (threads, semaphores, interrupts, DMA, SIF) | done (runtime) |
| Mods | named, hookable functions plus asset override folder | not started |

**Gate:** a release build compiled with `PS2X_SHIPPING=ON` doesn't link the VU1 interpreter or the IOP emulator, and the smoke route still passes.

## 3. Correctness ("1:1") success criteria
| ID | Criterion | Target | Evidence |
| --- | --- | --- | --- |
| C1 | **RSR:** every discovered EE function translates and compiles | 100%, 0 compile errors | recompiler + build logs |
| C2 | **Fallback rate:** indirect jumps resolved statically | ≥ 99.7% static; **0** unknown-target hits on all §6 routes | runtime counters |
| C3 | **CER:** recompiled functions match a reference executor | 100% on sampled leaf functions + all RTAO PAL oracles (`npm run test:pal`); no divergence in full-frame state hashes vs reference over 10k frames | `game/tests/` diff harness |
| C4 | **EE FPU / VU0 semantics** | round-toward-zero, ±FLT_MAX clamp, ÷0, saturating conversions all unit-tested against documented PS2 behaviour (PCSX2/ps2tek) | unit tests |
| C5 | **VU1 equivalence:** recompiled vs interpreter | 0 mismatches (GIF bytes, registers, flags, memory) across every catalogued image on all routes; **0 fallbacks** | `PS2X_VU1_DIFF=1`, `PS2X_VU1_STATS` |
| C6 | **Determinism:** same input replay gives the same game state | 100% byte-identical frames per vsync tick across 3 runs of each route | `PS2X_DETERMINISTIC=1` |
| C7 | **Timing:** PAL field rate | 50.00 ± 0.05 vblanks/s over 10 min; no catch-up bursts (no 2 s window > 51 or < 49 outside load screens) | `[fps]` log |
| C8 | **Renderer accuracy** vs reference (Play! capture of the same route) | per-checkpoint SSIM ≥ 0.98; no missing or extra primitives; blending, fog, alpha test, depth, CLUT and texture formats correct | frame-diff tool |
| C9 | **No visual defects** | 0 frames with flicker, blank buffers, missing geometry (e.g. office floor), garbage textures, or wrong field/interlace scaling on any route | `run_route.py` + manual review |
| C10 | **Audio accuracy:** native SNDMOD vs real IRX | identical per-tick key-on/off, pitch, volume and ADSR streams on BGM_01–12, all SE, engine and radio; output spectrally matches the reference (correlation ≥ 0.95 per track) | command-log replay diff, WAV compare |
| C11 | **Audio sync & quality** | no dropouts or underruns in 30 min; audio/video drift < 20 ms over 10 min; clipping < 0.01% of samples | WAV analysis, underrun counter |
| C12 | **Saves** | memory-card save/load is byte-compatible with real PS2 card data; saves survive app restart and update | `save_load` route + hex compare |
| C13 | **Gameplay physics** | race and free-roam vehicle state matches RTAO's PAL oracles (1,800 race-frame updates) within their tolerances | oracle replay |

## 4. Performance success criteria (Apple Silicon M1 baseline)
| ID | Criterion | Target |
| --- | --- | --- |
| P1 | Native 1× resolution, original 50 Hz | **50 fps locked** on every route, 1% low ≥ 49, frame-time p99 ≤ 21 ms |
| P2 | 4× internal resolution, 60 fps output | 60 fps locked, 1% low ≥ 58 |
| P3 | 4K output, high-frame-rate mode | 120 fps on M1 Pro or better, ≥ 60 on base M1 |
| P4 | Startup | cold launch to title screen ≤ 5 s (after first-run setup) |
| P5 | Loading | each load ≤ the original PS2 load time; ≤ 1 s target with fast-load enabled |
| P6 | Resources | ≤ 1 GB RAM, ≤ 25% of one P-core at 1×/50 fps on M1, no memory growth over 30 min |
| P7 | Battery / thermals | no sustained throttling on a fanless MacBook Air in a 30 min session at default settings |

## 5. Native app and enhancement success criteria
In the style of the N64Recomp-family ports: an in-app settings menu (native, not part of the game's UI), opened with Esc/Start+Select or from the menu bar. Every option applies live where possible, persists to a config file, and has a "Reset to defaults" button. **Defaults = exact original presentation.**

Approach to research and verify before building: use web search and Context7 for how Zelda64Recomp/RT64 implement interpolation, widescreen and resolution scaling.

**Settings placement (user decision, 2026-10-08):** the preferred home for graphics options (resolution scale, aspect, frame rate, FOV, view distance) is a new **"Graphics" page inside the game's own Options menu** (and optionally Pause > Settings), drawn with the game's own panel/font art via function hooks (M3), with no change to save data or menu logic. The macOS menu bar (View menu, Cmd+F) and a native settings window are the fallback, and ship first as the cheap path. Medium effort; after Metal M5 and the SDL3 shell.

### 5.1 Graphics
| ID | Option | Success criterion |
| --- | --- | --- |
| G1 | Internal resolution | 1×–8× native (512×448 PAL field-scaled) or "match window". Geometry and 2D UI render sharply at each step with no seams, gaps or misaligned sprites/fonts (checkpoint review at 1×, 2×, 4×, 8×). |
| G2 | Window / fullscreen | Windowed (resizable), borderless fullscreen, exclusive fullscreen; multi-monitor choice; Retina/HiDPI correct (no blur, correct scale); Cmd+F toggles fullscreen. |
| G3 | Aspect ratio | Original 4:3 (pillarboxed), 16:9 and 21:9 widescreen with correct 3D FOV (no stretching). The 2D UI/HUD stays 4:3-correct and anchored (centred or edge-anchored per element). Verified at checkpoints for the title, menus, dialogue, race HUD and the map. |
| G4 | Frame rate | Original (50), 60, 120, 144, display refresh, and uncapped. **Game logic stays at its original rate**; extra frames come from renderer-side interpolation of the camera and object transforms (the RT64-style approach). Must pass gate E1. |
| G5 | V-sync / pacing | V-sync on/off, frame limiter; no tearing with v-sync on; frame-time p99 within 10% of target. |
| G6 | Anti-aliasing / filtering | MSAA 2×/4×/8× and texture filtering (original/bilinear/anisotropic) with no UI artefacts (fonts stay pixel-correct when "original" is chosen). |
| G7 | Interlace / deinterlace | Original field presentation or progressive (no combing, no half-line wobble); default progressive. |
| G8 | Texture packs | Replacement textures by hash from `mods/<pack>/textures/`, hot-reloadable; dump-textures option for pack makers (dumps stay local). |
| G9 | Graphics API | Metal on macOS; the CPU backend stays available as a reference/debug option. |

### 5.2 Controls
| ID | Option | Success criterion |
| --- | --- | --- |
| I1 | Gamepads | Xbox, PlayStation (DualShock 4/DualSense), Switch Pro and MFi controllers via SDL/GameController: hot-plug, correct button glyphs in the settings menu, analog sticks and pressure-sensitive buttons mapped. |
| I2 | Keyboard and mouse | Default layout plus full remapping; no conflicting bindings (conflict warning); mouse navigation of the settings menu. |
| I3 | Remapping | Every PS2 button and analog axis rebindable per device; deadzone and sensitivity sliders; invert axes; bindings persist. |
| I4 | Rumble | DualShock vibration mapped to controller rumble with an intensity slider. |
| I5 | Input latency | ≤ 1 frame added latency versus the original at 50 Hz; measured with an input timestamp to presented frame. |

### 5.3 Audio
| ID | Option | Success criterion |
| --- | --- | --- |
| A1 | Volume | Master, music (BGM/radio), SFX and engine sliders, applied live. |
| A2 | Output device | Device selection; follows the macOS default-device change without restarting. |
| A3 | Quality | Original (SPU2-accurate Gaussian interpolation, reverb modes) or enhanced (high-quality resampling); default original. |
| A4 | Background | Mute when unfocused (option). |

### 5.4 Gameplay / quality of life (all default OFF, all opt-in)
| ID | Option | Success criterion |
| --- | --- | --- |
| Q1 | Fast loading | Skips artificial disc-seek delays; no change to game state (determinism replay identical apart from timing). |
| Q2 | Skip intro logos | Optional; goes straight to the title. |
| Q3 | Language | English/French/German selectable in settings (sets the game's own language choice). |
| Q4 | Save slots / backup | Automatic save backup; import/export of PS2 memory-card saves (.ps2/.max/raw). |
| Q5 | **Starting car chooser** | On a new Adventure, after the name/currency screens, an opt-in extra screen (the game's own UI style, like the body shop's car-body picker) lets the player choose the first car body and colour instead of the random one. Default OFF keeps the original random assignment. Picking writes only the same save fields the game itself writes. | Replay: with the option off the first car is identical to the original for the same seed; with it on the chosen body/colour appears in the office, Q's Factory and the save file. **Root cause (Q5a, 2026-10-09):** no RNG or clock. `FUN_00270550` picks one of six fixed starter bodies (43/0/117/140/114/93) by the game's own main-loop frame counter (u64 at `0x3d9b80`) mod 6, so human button timing decides; this is original PS2 behaviour. The colour is that body's fixed default paint. Hook: replace `a0` of `0x22b568` when called from the six picker cases, so the game writes every save field itself. Report `work/tracks/Q5/REPORT_Q5a.md`. |

### 5.5 Mods and extensibility
| ID | Item | Success criterion |
| --- | --- | --- |
| M1 | Mod folder | `~/Library/Application Support/RoadTripAdventure/mods/`, with per-mod enable/disable in the settings UI. |
| M2 | Asset override | Any disc file (textures, models, sounds, text) can be overridden by a loose file in a mod; the override is loaded instead of the disc file. Verified with a sample text mod and a sample texture mod. |
| M3 | Function hooks | A documented C API to hook named recompiled functions (pre/post/replace), with names from the symbol map; a sample mod (e.g. a speed display) works. |
| M4 | Safety | Mods can't write outside their folder; a broken mod is disabled with an error message, not a crash. |

### 5.6 App / platform
| ID | Item | Success criterion |
| --- | --- | --- |
| X1 | First-run setup | The user picks BIN/CUE or ISO; the app verifies it's SLES-51356 (hash/serial check), extracts locally, and generates/loads everything with no network. A clear error for wrong discs or regions. |
| X2 | Packaging | **Priority deliverable.** **No Apple Developer licence: not notarised, not Developer-ID signed.** Ad-hoc signed (`codesign -s -`, required for arm64 to run at all) arm64 `.app` (universal later), drag-to-Applications DMG, macOS 13+. Gatekeeper will warn, so the README and first-run docs give the exact steps (macOS 15+: System Settings → Privacy & Security → Open Anyway; older: right-click → Open; or `xattr -dr com.apple.quarantine`). Building from source remains the no-warning route. Self-contained: bundled dylibs (`dylibbundler`/`install_name_tool`, no Homebrew paths), no source, scripts or Python needed at run time, no disc-derived data in the bundle, first-run disc picker, saves in `~/Library/Application Support`. A clean-account/clean-machine test launches it by double-click. |
| X3 | Settings persistence | Config file in Application Support; survives updates; corrupted config falls back to defaults. |
| X4 | Crash handling | Crash report with log path; no data loss of saves. |
| X5 | Ports | Windows 10+ (D3D12/Vulkan), Linux (Vulkan, Steam Deck verified), browser (WebGPU + WASM): each passes the §6 routes at its own P1 target. |

### 5.7 Developer tooling and experimental mods (all opt-in)
Prior art to build on (reference and interop only; it has no licence file): `mholeys/roadtrip-choroq-tools` (model/texture/collision extraction, Blender car-modding pipeline, format docs). Target: D1 exports interoperate with its formats, and D6 can load its replacement `Qxx.BIN` cars through the M2 asset-override folder without patching the disc.

Every feature in this section is opt-in. Disc-derived output (WAVs, textures, models, manifests, dumps) stays local under `work/` or the user's app-support folder and is never committed or published. The default game is unchanged: with all D-options off, deterministic replay gives identical frames and game state (E3).

| ID | Feature | Benefit | Success criterion |
| --- | --- | --- | --- |
| D1 | **Decoded audio and model exporters.** Export discovered audio as uncompressed 16-bit 48 kHz WAV, and capture render geometry and textures for local model inspection. Manifests identify each source, export result and failure. | A practical way to inspect the game's sound, textures and geometry while building verified modding and preservation tools. | Every SNDMOD bank/sample and BGM exports with a manifest entry: 0 silent or failed exports unexplained. Captured geometry from a race frame re-renders (CPU or Metal) to SSIM ≥ 0.98 against the game frame. |
| D2 | **Native debug controls and free-cam.** Verified telemetry, debug views, coordinate tracking, speed readouts and free-camera controls on native keyboard hotkeys. | Faster track inspection, visual debugging and reproducible testing, without external memory-hacking tools. | Telemetry matches the game's own values (e.g. the speed readout equals the HUD digits; positions match RTAO car-struct decoding). Free-cam changes only the render camera: game state is identical with free-cam on vs off over a replay. |
| D3 | **Native developer menu.** An opt-in host menu for debug views, free camera, coordinate/speed telemetry, 100% parts unlock and configurable cash. | Developers reach late-game garages, shops, races and parts quickly, with ordinary saves kept separate from development saves. | Dev-menu changes write only to a separate dev save slot or card folder; ordinary saves are byte-identical before and after a dev session. Each unlock is verified in-game (shop lists, cash display). |
| D4 | **Visual overrides.** Per-environment FOV and culling settings, plus local texture replacements. | Experiments with draw distance, presentation and texture packs without changing the default experience or the simulation. | Game state is identical with any override on vs off (E2-style replay). Texture replacement is keyed by hash and hot-reloadable. Overrides are visible at the checkpoints of each environment. |
| D5 | **Expanded starting grids (research).** An isolated prototype that raises the car count beyond the original limit, after memory, AI, collision, camera, UI and renderer behaviour have been validated. | Tests whether larger races can work safely before exposing the feature through a public mod API. | A written analysis of every fixed-size structure involved (car array stride, AI and collision tables, HUD and minimap). A prototype race with N > original completes 3 laps with no memory corruption (guard pages / RAM diff), no crash, and ≥ 50 fps. |
| D6 | **Additive cars and tracks.** An experimental content pipeline that registers one new vehicle or one new track, once its catalogue, collision, AI, camera and UI requirements are documented. | A grounded path toward community content that never replaces original vehicles or courses. | A documented format for one car and one track. Adding one does not change any original content (original routes stay byte-identical with the add-on disabled). The new entry is selectable, drivable and finishable. |
| D7 | **World-streaming research.** Investigate town-transition triggers, loading boundaries and streaming behaviour as a long-term research project. | The technical foundation for exploring fewer loading screens in a future engine-level overhaul. | A research doc mapping every town transition (trigger, loaded files, sizes and timings measured from disc reads) plus a feasibility assessment. No gameplay changes in this phase. |

## 6. Enhancement-safety gates
| ID | Gate | Target |
| --- | --- | --- |
| E1 | Frame-rate independence | At 60/120/uncapped, physics, race lap times, AI, timers, music tempo and cutscene timing are identical to the original 50 Hz: deterministic replay gives identical game state per logic tick, and lap time matches to the tick. |
| E2 | Resolution / aspect independence | Game state is identical across all G1/G3 settings (renderer-only changes). |
| E3 | Defaults are original | With default settings, frames match the 1× original presentation (C8) and audio matches C10. |

## 7. Gameplay routes (automated, `game/tests/run_route.py`)
Each route checks: no flicker, non-black checkpoints, golden-frame match (deterministic mode, local goldens), fps targets, audio RMS in music sections, 0 VU1 fallbacks, and 0 missing-function/RPC/syscall errors.

| Route | Covers | Status |
| --- | --- | --- |
| `boot_to_title` | language → title → menu → name → currency → first cutscene | **passes** (0 VU1 fallbacks; office has full floor + car models; 3D median ~27 fps since geometry now draws) |
| `quick_race` | title → Quick Race → Peach Raceway → race start | **passes**: 49.9 fps real time; speedometer works (session 4) |
| `new_game_intro` | real name entry → intro → first free-roam spawn (Peach Town) | **passes** (name "ACE" → office → Q's Factory → Peach Town free roam) |
| `race_lagoon` | Quick Race → Lagoon, steered 60+ s (Laguna crash regression) | **passes** deterministically; real-time fps measured only under load |
| `free_roam_drive` | 60 s of driving, camera, engine audio, NPCs | **passes** (45.2 fps measured under load; re-measure idle) |
| `adventure_drive_highspeed` | Adventure free roam at top speed on long roads, plus collisions with walls, NPC cars and scenery | **pass** (2026-10-08, real time 49.9, 1% low 49.9; x=1600 seam both ways, FLD/232; one 43 fps second at r~3399 under load 6.7 excluded via `fps_exclude_reads`, idle recheck in G2d) |
| `adventure_talk_npcs` | Drive up to NPC cars and talk to them (dialogue boxes, portraits, text, choices); every town | **pass** for Peach Town NPC (49.9, 1% low 47.3 under load 8); other towns todo |
| `adventure_buildings` | Enter and leave every enterable building and shop in each town (transitions, interiors, menus, exit back to the map) | **partial pass**: Q's Factory + Paint Shop (49.9, 1% low 49.8); Parts, Body, Bar, Policeman, FM todo |
| `adventure_towns` | Travel between towns and areas (loading boundaries, map changes, day/night or weather if present) | **pass** for Peach Town ↔ field 232 seam + return (49.9, 1% low 47.3 under load); cave and other towns todo |
| `shop_and_parts` | buy and fit a part, money change, menu SFX | **passes** (49.9 fps) |
| `race_peach` | Quick Race → Peach Raceway, 3 laps → results (Adventure-mode entry via Q's Factory still todo) | **passes**: 3 laps → results screen (bot finishes 4th) |
| `save_load` | save → quit → relaunch → load, state identical | todo |
| `options_and_ui` | all in-game menus plus the native settings menu, every option toggled | **passes** (49.9 fps) |
| `enhancements_matrix` | each G/I/A option at min/max with E1–E3 checks | todo |
| `soak_30min` | stability, memory, thermals | todo |

**Adventure-mode QA (user requirement, 2026-10-08).** Free roam must be checked as carefully as races. The `adventure_*` routes above must all pass:
- **frame rate:** 3D median ≥ 49 and 1% low ≥ 45 in real time, including high speed and dense town areas;
- **no glitches:** no flicker, black, flat or mostly-one-colour frames, no missing geometry or garbage textures at checkpoints. Contact sheets are reviewed for every new route, and any visual oddity is filed with frame paths;
- **no bugs:** 0 missing-target, unhandled-syscall, unexpected-RPC or crash lines, and 0 VU1 fallbacks;
- **audio:** non-silent music and engine sound (RMS), and dialogue SFX where present;
- **input stays responsive:** pad reads continue; no hang like the old Q's Factory one.

Every glitch found becomes a fishbone + 7-whys entry in CHANGELOG and a regression checkpoint.

## 8. Checklist (phases)
### Phase 0 — Baseline
- [x] Extract the ISO from the BIN; identify ELF/IRX/VU sections
- [x] Survey prior work (RTAO, PS2Recomp, roadtrip-choroq-tools, VGMTrans)
- [ ] Reference captures from Play! for C8 (needs the user, or screen-capture permission)

### Phase 1 — Toolchain
- [x] PS2Recomp builds on arm64 macOS
- [x] Function discovery (Ghidra + EE plugin: 1,473 functions; analyzer: 230 SDK functions; 38 jump tables)
- [x] RTAO symbol seeding (1,010 exact function entries)
- [x] No runtime-loaded EE code

### Phase 2 — Boot (and HLE)
- [x] libvu0 + libm float kernels run as the game's own code (wrong HLE stubs removed; C3)
- [x] Audit/replace the remaining pure-computation HLE stubs (session 4: 17 → guest code incl. cos). Original item: audit/replace the remaining pure-computation HLE stubs (vsprintf, libc strings, memclr, sceGifPkCloseGifTag, cos, malloc family) with the game's own code or differential tests (C3)
- [x] Generated C++ compiles and links (C1)
- [x] All IRXs handled natively (SNDMOD + 6 builtins), no BIOS
- [x] Title, menus, name/currency entry and first 3D cutscene render
- [x] PAL 50 Hz pacing (C7 partially: no bursts; 10 min test pending)
- [x] Deterministic mode (C6: 33/33 identical frames)

### Phase 3 — Graphics
- [x] Flicker root-caused (HLE libgraph stub → the game's own libgraph now runs); field mode fixed
- [x] PS2-accurate EE FPU + VU0 macro semantics incl. MAC/status/clip flags, VF0 write-protect, DIV/SQRT I/D flags (C4; research: game/docs/VU0_MACRO_FLAGS_RESEARCH.md; unit tests pending)
- [x] VU1 static recompiler in the main tree (C5: 0 fallbacks on `boot_to_title` and race; 3D median 41.3 fps; main-tree `PS2X_VU1_DIFF` pass still to run)
- [x] Race 3D missing (C9): wrong HLE libvu0 `sceVu0MulMatrix`; libvu0 un-stubbed, VU0 macro flags fixed. Race renders (`quick_race` flat-frame checks pass)
- [x] Office-floor wedge fixed (C9): VU0 macro status flags and vf0 write-protect. Office frame r3651 is complete
- [~] All areas: free roam (Peach Town), races, interiors (office, Q's Factory), course previews and minimap verified rendering. Open: **speedometer digits stuck at "183"** (user-reported); other areas not visited yet (C8/C9)
- [x] 3D performance in races: 49.9 fps real time (session 4: GS thread, raster ×2, VU1 flag liveness). Earlier note: 19–27 fps, target 50 (P1). Race profile: GS raster on the game thread ~66%, VU1 flag waste ~21%. Session-4 tracks in flight: VU1 flag liveness, GS thread, raster speed-ups; then band-parallel raster. See HANDOFF §0.1/§5.1
- [~] Metal backend (G9): M0 census + M1 skeleton merged (exact, slow, off by default); next M2 textures. Then G1–G7

### Phase 4 — Audio
- [x] Native SNDMOD engine (BGM/SFX audible; BGM_01 timing matches RTAO; tables loaded from the user's IRX)
- [ ] Engine and radio verified in game (C10)
- [ ] Gaussian interpolation + reverb accuracy (A3/C10); sync and quality (C11)
- [x] Automated runs are silent (`PS2X_HEADLESS` implies `PS2X_MUTE`); audio still renders internally

### Phase 5 — Input / saves
- [ ] SDL GameController input (I1–I5)
- [ ] Save compatibility (C12)

### Phase 6 — De-emulation gate (§2)
- [ ] `PS2X_SHIPPING=ON` builds without the VU1 interpreter or IOP emulator; routes pass

### Phase 7 — Native app and enhancements (§5)
- [ ] Settings UI shell (menu, persistence, defaults)
- [ ] Graphics G1–G9, controls I1–I5, audio A1–A4, QoL Q1–Q4
- [ ] Mods M1–M4
- [ ] Enhancement-safety gates E1–E3

### Phase 8 — Packaging and ports
- [ ] X1 first-run disc setup, X2 signed/notarised app, X3 config, X4 crash handling
- [ ] X5 Windows/Linux/browser

### Phase 9 — Decompilation track (ongoing)
- [ ] Replace recompiled functions with clean C, prioritising physics, camera, UI, races, shops and dialogue; each is diff-tested (C3)

### Phase 10: Developer tooling and experimental mods (§5.7, opt-in)
- [ ] D1 exporters (audio WAV + manifest; geometry/texture capture)
- [ ] D2 debug hotkeys, telemetry, free-cam
- [ ] D3 developer menu with separate dev saves
- [ ] D4 visual overrides (FOV, culling, texture packs)
- [ ] D5 expanded grids (research prototype)
- [ ] D6 additive car/track pipeline (experimental)
- [ ] D7 world-streaming research doc

## 9. Rules
- Never commit or publish disc-derived data: generated code, tables, frames, audio, VU snapshots or golden frames. `work/` and `disc/` are local only.
- Read game data at run time from the user's disc.
- Research with web search and Context7; don't rely on memory. Cite sources in `CHANGELOG.md`.
- Every "done" claim needs evidence.

History and evidence: `CHANGELOG.md`. Next-agent instructions: `HANDOFF.md`.
