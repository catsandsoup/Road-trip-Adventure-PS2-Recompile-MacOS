# Metal GS backend plan (Road Trip Adventure, PAL SLES-51356)

Track M, 2026-10-08: research and design. Track G, 2026-10-08: M0 census and M1 skeleton built in `PS2Recomp-mtgs`; results in §13.

- **Covers:** GOALS G9 (Metal on macOS, CPU backend kept as reference), and the hooks that G1 (internal resolution), G3 (widescreen), G4 (high frame rate), G6 (AA/filtering) and G7 (progressive output) need.
- **Gates it must pass:** C8, E1, E2, E3 and P1–P3.
- **Data policy:** every number below is a count, a register-field mode or an SDK name. There are no frames, texture/CLUT addresses or disc data. The raw census outputs stay in `work/tracks/M/census/` (local only).

## 0. Summary
**Recommendation: option C**, a game-subset Metal renderer behind the existing `GSRasterBackend` interface, run by the existing `GSThreadedBackend` on the GS thread.
- The whole GS pixel pipeline is one specialised fragment shader with **integer maths**. It reads the framebuffer through Apple-GPU **framebuffer fetch** (programmable blending), so blending, the alpha-test fail modes, DATE and fog reproduce `GSCpuBackend::PixelPipeline::Write` instead of approximating it.
- The 4 MB GS local memory stays a **CPU shadow** that is authoritative for uploads, textures and CLUTs. Only the frame and Z pages that draws write are GPU-owned. A page-ownership map triggers a resolve (GPU → shadow) for the rare access that needs it.
- The texture cache is keyed on TBP/TBW/PSM/TW/TH, with page-generation invalidation. Indexed textures stay as indices, with a separate palette texture (PCSX2's "GPU palette conversion").
- The CPU backend remains the oracle. A tee backend feeds the identical command stream to both backends, and SSIM ≥ 0.98 per checkpoint is the gate (C8). Targeted pixel tests come on top.

**Why this game makes C cheap.** The measured subset is small (§2):
- one colour format (CT32) and one depth format (Z24);
- two framebuffers at fixed pages, field-mode 512×256;
- one alpha-blend equation for 3D, plus one FIX-alpha full-screen sprite;
- bilinear everywhere, T4/T8 with CT32/CT16 CLUTs, CT24 direct;
- fog on most 3D, DATE in one scene;
- no TEXFLUSH, and no readback call sites.

The milestones in §8 start by keeping all existing test tooling working: Metal renders offscreen and the frame is read back into today's `PresentationFrame` path. Only later do they move presentation and the app shell to native Metal.

## 1. Where it plugs in (current code, main tree)
- `ps2xRuntime/include/runtime/gs/gs_backend.h` is the `GSRasterBackend` interface:
  - `Submit(GSPrimitiveBatch)` carries **one primitive (≤3 vertices) plus its full `GSDrawState`**;
  - `LoadClut`, `BeginTransfer`/`UploadImage`, `Flush`, `TextureFlush`, `Sync(reason)`, `Present`/`PresentAsync`, `ClearFramebuffer`, `ConsumeLocalToHostBytes`, `ReadVram`/`WriteVram`, `SnapshotVram`, `GetTransferSnapshot`.
- `gs_threaded_backend.{h,cpp}` (track T, default on) records every producer call into 256 KB chunks and replays them on the GS thread in order.
  - Readbacks drain first.
  - Backpressure: 2 presents in flight and 16 MB of queued commands.
  - It also replays the producer's FP control state (round-toward-zero/FZ).
- `gs_frontend.cpp:111` `GS::makeDefaultRasterBackend()` currently builds `GSThreadedBackend(GSCpuBackend)`. **A Metal backend is a second inner backend chosen there** (`PS2X_GS_BACKEND=cpu|metal|tee`).
- The VRAM is the frontend's `m_localMemoryStorage` (4 MB), handed to the backend by `Initialize(vram, size)`. The CPU backend rasterises straight into it.
- Presentation:
  - The backend produces a CPU RGBA `PresentationFrame` (PCRTC merge in `GSCpuBackend::PresentFromLocalMemory`: PMODE, DISPFBn/DISPLAYn, field mode).
  - The frontend mailbox (`deliverHostPresentationFrame`, `latchHostPresentationFrameAndCopy`) hands it to the raylib loop in `ps2_runtime.cpp` (`UpdateTexture` + `DrawTexturePro`, about line 2700).
  - raylib is OpenGL-only on macOS. The F1 debug panel is ImGui through rlImGui.
- The pixel pipeline to reproduce is `gs_cpu_backend.cpp` `PixelPipeline::Write`, in GS order:
  1. scissor;
  2. fog;
  3. alpha test with AFAIL (`classifyAlphaTest`);
  4. DATE;
  5. depth test (NEVER/ALWAYS/GEQUAL/GREATER on the raw integer Z);
  6. blend `((A−B)·C >> 7) + D` with clamp and PABE;
  7. FBA, FBMSK, then frame and Z writes.

  Texels: TEXA expansion and an expanded CLUT palette. Bilinear follows `gs_bilinear.h` `lerpChannel`: float weights, three FMAs, round half away from zero.
- Not implemented by the oracle: DTHE, COLCLAMP=0 (wrap), mipmapping. See the M0 census items in §2.3.

## 2. This game's GS usage (census)
### 2.1 Method
- **Track M traces.**
  - Five headless, muted runs of the main runner (read-only), each with a fresh save dir.
  - Each run used `PS2X_TRACE_TRIS` (a 0.4 s window of rasterised triangles: prim flags, TEX0 fields, TEST, ALPHA, FRAME.FBP), plus `PS2X_TRACE_PRESENT`.
  - Scenes, confirmed by viewing the dumps locally: title (pad read ≈550), race countdown (≈1300), race running (≈1455), Q's Factory (≈2650), Peach Town free roam (≈3425).
  - The runs were real-time while other tracks were compiling (the game ran at 7–37 fps), so each window covers 3–7 frames. The counts show proportions, not per-frame budgets.
  - **Sprites are not traced** by `PS2X_TRACE_TRIS`.
- **Track R census** (`work/tracks/R/runs/census_race/log.txt`, a temporary `[census]` switch in an R build, quick_race, presents 1400–1750). It covers **all** primitives, sprites included, keyed on the full draw state.

### 2.2 Results (counts only)
| Item | Finding |
| --- | --- |
| Frame / depth formats | FRAME.PSM CT32 only; ZBUF.PSM Z24 only (R census, all 30 race state keys) |
| Framebuffers | FBP ∈ {0, 64} pages only, alternating per frame (all 5 traces). PMODE EN2 only (MMOD=1, AMOD=1). DISPFB2 FBW=8 (512 px) CT32; DISPLAY2 MAGH=4 → 512 px wide; SMODE2 INT=1 FFMD=1 (field mode) → **the game draws 512×256 per frame**, line-doubled on output |
| Field offset | Vertex Y of matched triangles in alternate buffers (title, 21 frames) differs by 0 or ±0.06 px (animation), never by 0.5 → **no half-line field offset**; both fields show the same 256-line image |
| Primitive load | Race ≈26.6 k primitives/frame (R census); 49–54 k triangles in each 0.4 s trace window across race/factory/town |
| State changes | Consecutive-triangle state runs: race ≈316/frame, town ≈315/frame, factory ≈64/frame (triangles only; CLUT reloads not included, ≈340 CLUT loads/frame per the design brief) → a few hundred to ~1000 draws/frame after coalescing |
| Blend (ALPHA) | 3D and HUD: **only (Cs−Cd)·As+Cd**. One full-screen sprite per frame uses **(Cs−Cd)·FIX+Cd**. ABE=0 for the clear sprite and a few dozen triangles. 168 blended textured triangles in one race window have vertex alpha > 0x80, and modulated CLUT alpha can exceed 0x80, so **As > 1.0 occurs** and needs exact `>>7` maths |
| Alpha test | ATE GEQUAL AREF=1 AFAIL=KEEP (most 3D); **ATE GEQUAL AREF≈127/128 AFAIL=RGB_ONLY** with ZTST GEQUAL, GREATER or ALWAYS (the dominant free-roam state: ≈34 k of 54 k town triangles); ATST NEVER + RGB_ONLY (176 town triangles); ATE off |
| Depth test | ZTST GEQUAL (dominant), GREATER (coplanar second passes), ALWAYS; ZMSK=0 throughout the race census |
| DATE | **DATE=1 DATM=1** on untextured ATE-off ZALWAYS triangles in Q's Factory (17.6 k in one window); not seen in race |
| Fog | **FGE=1 on most race 3D triangles** (R census). Exception: one T8 key with FGE=0, ABE=0, ZTST ALWAYS (≈11.4 k prims, ≈20.7 M px over 350 frames: the second-largest pixel consumer, sky-like) |
| Textures | T4 and T8 (CLUT CPSM **CT32** for most 3D; **CT16** for HUD sprites and some small 3D triangles, ≈220 k prims and few pixels), CT24 direct; all bilinear (LTF); TFX MODULATE (3D) or DECAL (sprites); TCC 0/1; sizes 16–1024 texels; 73–110 distinct TBP per window |
| Wrap modes | REPEAT and CLAMP (3D); **REGION_CLAMP and REGION_REPEAT** on HUD sprites (CLAMP fields 0xA, 0xC, 0xF, 0x3) |
| Sprites | Per frame: 1 untextured clear sprite (ABE=0) and **1 full-screen CT24 DECAL sprite with FIX alpha** (both ≈512×256 px); FST HUD sprites T4 with CT16 CLUT |
| Never seen | FBA, PABE, FBMSK ≠ 0 (race); TEXFLUSH (R); `sceGsExecStoreImage` (the only local→host path in the HLE stubs) has **0 static callers** in `work/generated` |
| Render-to-texture | **No triangle** samples from the FBP 0/64 regions (all windows). **Unknown for sprites**, in particular the full-screen CT24 sprite (feedback/motion-blur candidate) |
| Projection hook | `sceVu0ViewScreenMatrix` (run as game code) has 1 static caller; `sceVu0ClipScreen`/`ClipScreen3` are also game code |

### 2.3 Census gaps, closed in M0
A count-only `PS2X_GS_CENSUS=<file>` switch inside the backend, per frame, must record:
- sprites;
- TEX0 page overlap with any FRAME/ZBUF page written earlier in the frame or the previous frame, **including the full-screen CT24 sprite**, plus Z used as a texture;
- local→local transfers;
- host→local bytes per frame, and how often one TBP receives different contents within a frame (texture-cache versioning and hit rate, §6.4);
- DTHE, COLCLAMP, PABE, FBA, FBMSK;
- XYOFFSET and SCISSOR per buffer;
- CLUT loads (CLD modes);
- ZBUF page range;
- the VU1 programs (catalogue entries) that receive 4×4 matrices through VIF unpacks (needed for G4, §9.3).

If the CT24 sprite reads a framebuffer, the texture cache must serve GPU targets directly (§6.4). That path is designed in, but it moves from optional to required.

## 3. Prior art (primary sources; GitHub `master`/`main`, fetched 2026-10-08)
### 3.1 PCSX2 hardware renderer and Metal device
- **Metal device** (`pcsx2/GS/Renderers/Metal/GSDeviceMTL.{h,mm}`, `GSMTLDeviceInfo.mm`, `tfx.metal`).
  - One long-lived render encoder (`BeginRenderPass`/`EndRenderPass`). Separate command buffers for draws, texture uploads and vertex uploads (`m_current_render_cmdbuf`, `m_texture_upload_cmdbuf`, `m_vertex_upload_cmdbuf`).
  - Ring-buffer uploads (`UploadBuffer`/`BufferPair` + `UsageTracker`).
  - Pipelines cached by `PipelineSelectorMTL` (VS selector + PS selector + blend/format extras).
  - Depth-stencil and sampler state tables (`m_dss_hw`, `m_sampler_hw`).
  - Readback through `GSDownloadTexture` with a spin fence.
  - Feature detection:
    - `framebuffer_fetch` when `supportsFamily:MTLGPUFamilyApple1`;
    - raster order groups (`rov`) only when framebuffer fetch is off;
    - `primid` with a runtime probe;
    - `unified_memory`;
    - a "broken shader depth (Apple GPUs)" flag, a caution for any design that writes `[[depth]]` from the fragment shader.

  With framebuffer fetch, texture barriers are unnecessary (`textureBarrier` = `memoryBarrierWithScope:RenderTargets`).
- **Shaders** (`tfx.metal`) implement the whole GS pixel pipeline as one specialised shader:
  - `sample_4_index`/`sample_4p` plus manual bilinear for paletted textures;
  - `clamp_wrap_uv` (REGION_CLAMP; REGION_REPEAT by integer mask);
  - `tfx` (MODULATE `trunc(C·T/128)`, DECAL, HIGHLIGHT(2));
  - `atst` and the AFAIL variants;
  - `fog` (`trunc(mix(fog, C, f·255/256))`);
  - DATE (PS_DATE 1–3 primitive-ID, ≥5 RT alpha);
  - `ps_blend` `(A−B)·C+D` with `PS_BLEND_MIX` and colclip;
  - `ps_fbmask`, `ps_dither`;
  - Z as `float(min(z, max_depth))·2⁻³²`;
  - framebuffer fetch through `[[color(0)]]` when `HAS_FBFETCH`.

  Vertex shader: `p − 0.05`, then scale/offset.
- **Draw config** (`pcsx2/GS/Renderers/Common/GSDevice.h`):
  - `GSHWDrawConfig::PSSelector` bit fields (`atst`, `afail`, `date`, `fog`, `fbmask`, `blend_a..d`, `blend_hw`, `colclip`, `pabe`, `dither`, `zclamp`, `region_rect`, …);
  - `DestinationAlphaMode {Off, Stencil, StencilOne, PrimIDTracking, Full}`;
  - an `AlphaPass` struct: the **second pass** used for alpha-test fail modes that fixed-function depth can't express;
  - `require_one_barrier`/`require_full_barrier`.
- **Texture cache** (`pcsx2/GS/Renderers/HW/GSTextureCache.h`):
  - `Target` (render/depth surfaces with `m_valid`, `m_dirty`, `m_end_block`, age);
  - `Source` (keyed by a `HashCacheKey` of TEX0, TEXA, the CLUT hash and the region; with a per-block valid bitmap and `m_from_target` for render-to-texture);
  - `SourceMap` page lists `m_map[GS_MAX_PAGES]`;
  - `InvalidateVideoMem`, `InvalidateLocalMem`, `InvalidateContainedTargets`;
  - `Read`/`ReadbackAll` through download textures;
  - `PaletteMap::LookupPalette` (palette textures keyed by CLUT contents).
- **Settings** (`pcsx2/Config.h`): `AccBlendLevel {Minimum…Maximum}` and the upscaling hack knobs `GSHalfPixelOffset {Off, Normal, Special, SpecialAggressive, Native, NativeWTexOffset}`, `UserHacks_RoundSprite`, `GSNativeScaling`, `UserHacks_TCOffsetX/Y`, `UserHacks_BilinearHack`, `GPUPaletteConversion`, `UpscaleMultiplier`. `GSRendererHW.cpp` has `RoundSpriteOffset`, `MergeSprite`, `ConvertSpriteTextureShuffle` and `IsPossibleChannelShuffle`. The fetch was truncated, so the blend-level internals are not summarised here.
- **Lessons for us:**
  1. On Apple GPUs, framebuffer fetch makes full shader blending cheap. Blend-accuracy levels exist to work around GPUs without it and are moot here.
  2. Most HW-renderer complexity is texture-cache heuristics for games that alias formats and render to textures. Our census shows almost none of that, so we implement the minimal coherent subset and fall back to a resolve.
  3. Upscaling artefacts come from sprites and half-pixel conventions, so the 2D path needs explicit native-pixel snapping (§9.1).

### 3.2 paraLLEl-GS (Themaister; `github.com/Arntzen-Software/parallel-gs`, blog 2024-07-03)
- A Vulkan **compute** GS: 4 MB VRAM in a GPU buffer, a CPU page tracker for hazards, binning plus a tile rasteriser ubershader, CLUT snapshots in a ring.
- Upscaling is **SSAA** (2×–16×) with single-sampled and N super-sampled VRAM copies; sprites snap to native pixels.
- Its author does not claim bit-exactness.
- Status and fit:
  - LGPLv3+;
  - tested on Linux and Windows only (no macOS/MoltenVK statement);
  - the PCSX2 integration is described as experimental;
  - no MSAA or widescreen concept: SSAA keeps the GS's own resolution grid.

### 3.3 RT64 (`github.com/rt64/rt64`), the N64 contrast
- Uses ubershaders (no pipeline-compile stutter), with D3D12/Vulkan/Metal through its own RHI.
- Records a per-frame `Workload`: draw data plus **matrices** (`viewTransforms`, `projTransforms`, `worldTransforms` and `prev*` copies, `transformGroups`, `transformIdMap`).
- High frame rate:
  - `WorkloadQueue::renderThreadLoop` generates `displayFrames = (logicalTicks − displayTicks)/viOriginalRate` frames with `prevFrameWeight`/`curFrameWeight`.
  - `GameFrame::match` pairs transforms by **matrix-group id** (extended GBI `gEXMatrixGroup(id, mode, push, proj, pos, rot, scale, skew, persp, vert, tile, order, edit, aspect, tc, lookat)`), then by physical similarity. It decomposes into position, rotation and scale (`RigidBody::updateLinear/updateAngular`), interpolates texture scroll, and leaves `G_EX_ID_IGNORE` groups uninterpolated.
- Widescreen 2D uses `gEXSetRectAlign(lorigin, rorigin, offsets)` with `G_EX_ORIGIN_LEFT/CENTER/RIGHT`, plus `gEXSetViewportAlign`/`gEXSetScissorAlign`.
- **The key difference:**
  - RT64 sees model/view/projection matrices because the N64 RSP display list carries them.
  - Our GS sees only screen-space vertices that VU1 already transformed.
  - So **interpolation and widescreen can't live in the GS backend**. They hook earlier (§9).

## 4. Options
| | Option | Effort | Risk | Accuracy vs oracle | G1/G3/G6 fit | Evidence to decide |
| --- | --- | --- | --- | --- | --- | --- |
| A | Full PCSX2-style HW renderer (general texture cache, targets, format aliasing, hacks) | Very high | High (heuristics; GPLv3 code can't be pasted without a licence decision) | SSIM-level | Good | Needed only if the M0 census finds heavy aliasing or RTT |
| B | Compute GS with GPU VRAM (paraLLEl-GS design, or paraLLEl-GS itself through MoltenVK) | Very high (own) / medium (port) with unknown MoltenVK risk | High: subgroup/8-bit storage features on MoltenVK; LGPL linking; no MSAA/widescreen | Closest | G1 by SSAA only; G3 hard | A 1-day MoltenVK feasibility spike if C fails its accuracy gate |
| **C** | **Game-subset Metal render pipeline: integer pixel pipeline in one shader with framebuffer fetch; CPU shadow VRAM; page-ownership resolves; per-draw CPU fallback** | Medium | Medium: depth strategy (§6.3) and any RTT (§2.3) | High (same integer maths; differs only in raster coverage and interpolation precision) | Good: vertex scaling, MSAA option, virtual-width targets | M1 prototype numbers (§8) |
| D | Metal presentation only (upload the CPU frame) | Low | Low | Exact | None | Needed anyway as the shell's present path; not a GS backend |

Recommendation: **C**, with **D** as part of the app-shell work. A is a superset of C if a later census finds more. B is the fallback if C can't reach SSIM ≥ 0.98.

## 5. Architecture (option C)
```
game thread ──GS frontend (m_stateMutex)──► GSThreadedBackend chunks ──► GS thread
                                                                       │
                                                         GSMetalBackend (inner)
             ┌───────────────────────────────┬─────────────────────────┼──────────────────────┐
     Recorder (batching,           Texture/CLUT cache        Target/page map         Presenter
     vertex ring, state keys)      (CPU decode → MTLTexture)  (GPU-owned pages,       (PCRTC merge pass,
             │                                                 resolve → shadow)       drawable or readback)
             └──────► MTLCommandBuffer per GS frame, committed at PresentAsync ◄───────┘
```
- **Threading.**
  - The GS thread owns every Metal object of the backend. No Metal call on the game thread.
  - One `MTLCommandBuffer` per presented frame (committed in `PresentAsync`), plus extra commits at Sync(Finish) only when a resolve needs them.
  - Frames in flight are bounded by the existing `kMaxPresentsInFlight = 2` and a `dispatch_semaphore` on transient-buffer reuse.
  - `Sync(DebugReadback|LocalToHost)` = commit + `waitUntilCompleted` + resolve.
  - FP-control replay is irrelevant to the GPU; vertex conversion runs on the GS thread under the replayed mode, as today.
- **Recording.**
  - `Submit` converts the primitive to GPU vertices: XY as GS 12.4 fixed point minus XYOFFSET, kept as `int`; Z as `uint`; STQ/UV, RGBA and fog as-is.
  - Sprites expand to 2 triangles with flat attributes.
  - Vertices go into a per-frame shared-storage ring (unified memory: no copy).
  - A 64–128-bit **state key** (the PS key in §6.3, plus scissor, texture id and palette id) is compared with the previous primitive. Equal keys extend the current draw; a change closes it. This is the coalescing PCSX2 gets from vertex kicks; ours arrives pre-split, one primitive per `Submit`.
- **Clears.** The game clears with a sprite through the normal pipeline (HLE `applyGsClearPacket`), so no special path is needed. `ClearFramebuffer` maps to a full-rect draw with the same state.

### 5.1 Data flow per GS operation
| Call | Metal backend action |
| --- | --- |
| `BeginTransfer`/`UploadImage` (host→local) | Write the CPU shadow exactly as today (the shared `ps2_gs_psm*` swizzlers); bump the generation of each touched page. If a page is GPU-owned (a target), also upload the rect into the target (CT32/Z24 at 1×, scaled by a blit/draw), or mark it "shadow newer" and re-upload on next use |
| `LoadClut` | CPU-side CLUT buffer exactly as the CPU backend (`m_clut`, CSM1/CSM2, CSA); palette texture looked up by (CPSM, CSA, content hash) |
| `Submit` | Record (above). Texture via the cache (§6.4); target pages marked GPU-owned |
| `TextureFlush` | No-op (coherent design, same as the CPU backend and PCSX2 SW) |
| local→local transfer | Resolve source pages if GPU-owned; perform on the shadow; invalidate. If the destination is a target: upload |
| `ConsumeLocalToHostBytes`, `ReadVram`, `SnapshotVram` | Commit, wait, resolve the overlapping GPU-owned pages into the shadow, then serve from the shadow |
| `Present`/`PresentAsync` | Merge pass (PMODE/DISPFB/DISPLAY/field) from the target textures; then either the drawable (native shell) or a blit into a shared buffer delivered as `PresentationFrame` (M1–M5, frame dumps, routes) |

## 6. Key components
### 6.1 Coordinate and rasterisation conventions
- Positions are GS integer 12.4. The vertex shader emits `(x/16 − ofx)·s_x` and `(y/16 − ofy)·s_y` in pixels of the scaled target.
- Matching the oracle needs a calibration test (M2) that compares Metal's rasterisation with `GSCpuBackend`'s barycentric coverage on synthetic edges. It determines:
  - the pixel-centre convention (Metal samples at pixel centres; the GS/oracle at integer coordinates);
  - the top-left rule;
  - any sub-pixel bias (PCSX2 uses −0.05).
- Attributes:
  - Colour: `flat` for IIP=0. For IIP=1, smooth (perspective-correct) or `center_no_perspective` to match the oracle; also decided by calibration.
  - STQ: perspective-correct divide in the shader, as the GS does (S/Q, T/Q).
  - UV (FST=1): `center_no_perspective`.
- `[[position]] invariant` (with `MTLCompileOptions.preserveInvariance`) so the coplanar GEQUAL/GREATER second passes produce identical Z across pipelines.

### 6.2 Targets and VRAM ownership
- The **page-ownership map** covers 512 pages × 8 KB, each with `{owner: shadow|gpu, generation, targetId}`.
- A target is created from a draw's FRAME (FBP, FBW, PSM) or ZBUF (ZBP, PSM) and the scissor extent.
  - Format: colour `RGBA8Uint`; Z as described in §6.3.
  - Size: (FBW·64·s_x) × (scissor height·s_y).
  - For this game that is 2 colour targets (FBP 0/64) and 1 Z target.
- On first GPU use, the target is uploaded from the shadow (decode at 1×, scale by nearest), so colour persisting between frames is correct.
- **Resolve** (GPU → shadow):
  1. a compute kernel downsamples to 1× (sample (0,0) of each s×s block, the native-equivalent sample);
  2. it re-swizzles into the GS block/column layout into a shared `MTLBuffer` (an MSL port of `ps2_gs_psmct32.h`/Z24 addressing);
  3. the CPU copies the bytes into the shadow.
- Expected frequency in this game: **0 per frame** (no readback call sites, no TEXFLUSH; RTT pending M0). `SnapshotVram` (debug panel, snapshots) is the only routine user.

### 6.3 Pixel pipeline shader and the depth decision
- **One fragment function** specialised by **Metal function constants** (`MTLFunctionConstantValues`), from a key holding:
  - TME, texel class (CT32 / CT24 / T8 / T4 / T4HL/HH / T8H / CT16 if ever seen), TFX, TCC;
  - FGE;
  - ATE, ATST, AFAIL;
  - DATE, DATM;
  - ZTE, ZTST, ZMSK;
  - ABE, A/B/C/D, PABE;
  - FBA, FBMSK≠0;
  - WMS, WMT;
  - LTF, IIP, FST.
- Pipelines are compiled lazily and cached on disk (`MTLBinaryArchive`). An ubershader variant is the fallback while a specialisation compiles (RT64's no-stutter rule).
- The census predicts **≈30–60 live keys** for this game.
- **Exact maths**, an MSL port of the oracle:
  - modulate `(T·C)>>7` clamped;
  - fog `(f·C>>8) + ((255−f)·F>>8)`;
  - blend `clamp(((A−B)·C>>7)+D)` on ints, reading Cd/Ad from `[[color(0)]]` (`RGBA8Uint` framebuffer fetch);
  - bilinear: `fma` plus `round` (half away from zero), the same op order as `gs_bilinear.h`;
  - indexed textures: 4 index fetches → 4 palette fetches → manual bilinear (as `tfx.metal` `sample_4p`);
  - REGION_CLAMP/REGION_REPEAT on integer texel coordinates.
- **Depth: decided by the M1 prototype.**
  - **(a) Hardware depth.**
    - `Depth32Float`, Z stored as `z·2⁻²⁴`: Z24 integers are exact in float32, so compares are exact.
    - Depth-stencil states for GEQUAL/GREATER/ALWAYS/NEVER; early-Z/HSR stays on.
    - AFAIL=RGB_ONLY with a depth test needs PCSX2's **two-pass** scheme (`AlphaPass`): pass 1 writes colour+Z where alpha passes; pass 2 writes RGB only where it fails, with Z test and no Z write.
    - Doubles draws for the dominant free-roam state.
    - Compatible with MSAA (G6).
  - **(b) Software depth in a colour attachment.**
    - Z24 lives in an `R32Uint` colour attachment read through framebuffer fetch (`[[color(1)]]`).
    - The shader does the whole GS order (alpha test → DATE → Z test → blend → masked writes) in one pass and writes Z as colour. It mirrors `PixelPipeline::Write` exactly and never writes `[[depth]]` (sidestepping PCSX2's "broken shader depth" flag).
    - Costs: no early-Z/HSR (overdraw shading), and MSAA means sample-rate shading.
    - Fill budget at 4× is ≈0.5 Mpx (1×, measured ≈24 M px written per 50 race frames) × 16 × 50 fps ≈ 400 Mpx/s. That is small for an M2 Pro, but **must be measured**.
  - **Gate for the choice:** M1 pixel diff vs the oracle on the tee harness, and race fps at 1×/4×/8×. Prefer (b) if it is within budget (simplest exact semantics); fall back to (a) per draw for MSAA.
- **DATE** (DATM=1, Q's Factory): with framebuffer fetch it is a per-fragment `discard` on `Cd.a >> 7 != DATM`. That is exact in GS order because Apple framebuffer fetch reads the value written by earlier fragments of the same pass. No stencil or primitive-ID trick is needed (PCSX2 needs them only without fbfetch).

### 6.4 Texture and CLUT cache
- **Sources.**
  - Key: (TBP0, TBW, PSM, TW, TH), plus TEXA for CT24/CT16 alpha expansion.
  - Value: an `MTLTexture`. Direct formats are stored as `RGBA8Uint` after CPU decode (reusing the oracle's `ps2_gs_psm*` readers, so decode equals the oracle by construction); T4/T8 as `R8Uint` indices.
  - The entry records the generation of every page it covers. Lookup re-validates by comparing generations (≤ a few dozen pages per texture), re-decodes only on change, and evicts by LRU over a byte budget.
- **Palettes.** On `LoadClut`, hash the CLUT bytes that the next draw's CSA/CPSM selects (64 B for T4, 1 KB for T8) into a palette `RGBA8Uint` 16×1 or 256×1 texture. Deduplicate by hash (PCSX2 `PaletteMap`). This turns ≈340 CLUT loads/frame into palette lookups and keeps index textures reusable across palettes.
- **From a target (RTT).** If a texture's pages are GPU-owned and the formats are compatible (CT32/CT24 reading a CT32 target at the same TBW), bind the target texture directly, scaled. The UV scale is applied in the shader, and the sample uses the read-before-write copy if source and target are the same (a blit snapshot, as PCSX2 "tex_is_fb"). For any other combination: resolve, then treat it as a shadow texture.
- **Versioning (hazard rule).**
  - Shadow uploads happen at record time; draws execute only when the frame's command buffer commits.
  - So a cache entry is **immutable once a recorded draw references it**. A re-upload of the same TBP within a frame creates a new version: a texture from a pool, or a blit-encoder upload ordered inside the frame's command buffer before the draws that use it. Draws recorded earlier keep sampling the old version.
  - Never `replaceRegion` a texture that an uncommitted draw references.
- **Content-hash tier.** Games often stream textures through a few VRAM slots every frame. On a generation change, hash the covered bytes (xxHash3) and reuse any live texture with the same (hash, PSM, TW, TH, TEXA), as PCSX2's `HashCacheKey` does, so steady-state re-uploads cost a hash, not a decode and upload. M0 measures host→local bytes per frame and how often one TBP takes different contents within a frame.
- **Upload path.** CPU decode into the per-frame ring, then a blit (ordered as above). With unified memory, a later optimisation is to unswizzle the shadow pages in a compute kernel (paraLLEl-GS style). Do it only if profiling demands it.

### 6.5 Per-draw CPU fallback (safety net)
- If a draw's key contains a state the shader doesn't implement, the backend runs the **oracle on that draw**:
  1. resolve the touched target pages;
  2. let `GSCpuBackend` rasterise into the shadow;
  3. re-upload the dirty rect.

  Triggers: COLCLAMP=0, DTHE on a 16-bit frame, CT16 frames, Z16, mipmapping, PRIM AA1, or an unexpected aliasing case.
- It is slow but correct, and every occurrence is logged and counted (`[gsmtl] fallback key=…`). Routes gate on **0 fallbacks**, like the VU1 rule.

## 7. Accuracy strategy and gates
1. **Oracle.** `GSCpuBackend` defines correct output.
   - The current main build reproduces the **boot** golden byte-exactly.
   - race/newgame still hold the pre-SQRT-fix goldens and differ by the known speedometer/AI-car pixels until the user approves the replacement.
   - The tee harness (below) compares against the live CPU backend, not the goldens, so this doesn't affect it.
2. **`GSTeeBackend`** (`PS2X_GS_BACKEND=tee`) forwards every command to both inner backends on the GS thread.
   - At each present (or every Nth), it reads back the Metal frame at 1× and compares it with the CPU frame.
   - Writes to `PS2X_GS_TEE_DIR`: SSIM, exact-match %, max channel diff, bbox of differing pixels, and a diff heat-map PNG (local only).
   - The input stream is identical by construction, so there is no game nondeterminism in the comparison.
3. **Command-stream capture/replay.**
   - `PS2X_GS_CAPTURE=<file>` writes `GSThreadedBackend` chunks. They are disc-derived, so they stay in `work/`.
   - The `gs_replay` tool replays them into either backend: fast, deterministic regression sets per scene (title, menus, race, factory DATE, town, map).
4. **Metrics.**
   - **SSIM ≥ 0.98 per checkpoint** (C8) on deterministic `det_capture` runs, CPU versus Metal at 1×;
   - per-scene floors in the tee harness (target ≥ 0.995; ≥ 99% exact pixels except raster-edge pixels);
   - **0 per-draw fallbacks**.
   - The SSIM tool doesn't exist (no numpy). M1 delivers `game/tests/ssim.py` (pure Python on the PIL L channel, 8×8 Gaussian-windowed SSIM; slow but fine for ≤100 frames) or a small C++ tool next to `compare_frames.py`.
5. **Targeted pixel tests** (`ps2xTest/gs_metal/`, synthetic GS streams, no disc data), Metal vs CPU byte-exact unless noted:
   - the blend equations used, with As ∈ {0, 0x7F, 0x80, 0xFF};
   - ATST × AFAIL × ZTST (the RGB_ONLY + GEQUAL/GREATER coplanar pairs);
   - DATE/DATM;
   - fog f ∈ {0, 1, 128, 255};
   - TFX MODULATE/DECAL × TCC;
   - CT24 + TEXA;
   - T4/T8 × CT32/CT16 CLUT × CSA;
   - REGION_CLAMP/REGION_REPEAT;
   - bilinear weights at texel edges;
   - sprite edges and the top-left fill rule (exactness expected after calibration; a documented tolerance otherwise);
   - Z24 compare boundaries.
6. **Metal self-determinism.** A golden set per (machine, macOS) for the Metal backend's own regressions: byte-exact across runs, like `work/golden_det`.
7. **E2 (renderer-only).** `PS2X_DUMP_RAM_AT` EE RAM at fixed pad reads is identical between CPU and Metal runs, and at every G1/G3 setting. This is valid because the EE never reads GS memory (0 StoreImage callers), so deterministic tick pairing holds.
8. **E3.** At 1×/4:3/original settings, routes pass on the Metal backend and the tee shows SSIM ≥ 0.98 everywhere.
9. **Performance.** `run_route.py quick_race` with `RTA_RUNNER` set to the Metal build: P1 (50 fps locked, 1% low ≥ 49) at 1×, P2 at 4×. GS-thread CPU time per frame is reported alongside (the CPU raster cost should collapse to recording cost).

## 8. Milestones (each with its gate)
| # | Milestone | Gate |
| --- | --- | --- |
| M0 | Census switch `PS2X_GS_CENSUS` (§2.3) in the CPU backend; reports over boot/race/newgame plus a town route | Count-only report committed to docs. Answers RTT for the CT24 sprite, DTHE/COLCLAMP, local→local, VU1 matrix programs. Goldens unchanged (output-neutral) |
| M1 | `GSMetalBackend` skeleton behind `PS2X_GS_BACKEND=metal`: offscreen device; upload-from-shadow targets; untextured triangles plus the full integer pixel pipeline; readback into `PresentationFrame`. `GSTeeBackend`. `ssim.py`. **Depth prototype (a) vs (b)** | Tee on boot (title/menus): SSIM ≥ 0.98. Untextured synthetic tests byte-exact. Depth decision recorded with fps and diff numbers |
| M2 | Rasterisation calibration (pixel centre, top-left, bias); texturing: CT24/CT32 direct, T4/T8 + palette textures, all wrap modes, manual bilinear, fog, DATE | Targeted tests byte-exact (or tolerance documented); tee boot+race+newgame SSIM ≥ 0.98; 0 fallbacks |
| M3 | Texture cache with page generations; CLUT dedupe; per-draw CPU fallback; resolves; capture/replay tool | quick_race real time on Metal at 1× ≥ 50 fps (P1); GS-thread CPU ≤ 25% of one P-core; routes pass; E2 RAM equality |
| M4 | Native presentation: merge pass to `CAMetalLayer` (with the shell work, §10); frame dumps still via readback | Routes pass with dumps; no tearing with v-sync (G5); E3 |
| M5 | Internal resolution s ∈ {1…8} plus progressive Y (G1, G7) and the 2D sprite policy | Checkpoint review at 1×/2×/4×/8× (title, menus, dialogue, race HUD, map): no seams or misaligned fonts. P2 at 4×. E2 |
| M6 | MSAA and filtering (G6): MSAA only with depth option (a) or sample-rate shading; "original" filter keeps fonts exact | UI fonts byte-exact at "original"; fps budget |
| M7 | Widescreen (G3) and interpolation (G4) through game-code hooks (§9); not GS-backend work | E1 deterministic replay identical per logic tick; E2; checkpoint review |
| M8 | Default switch: Metal on; CPU stays `PS2X_GS_BACKEND=cpu` (G9) | All routes on both backends; tee SSIM report attached to the CHANGELOG |

## 9. How the enhancements hook in
### 9.1 Internal resolution (G1) and progressive output (G7)
- Scale factors `s_x = s`, `s_y = 2s` (the game draws 512×256 field-mode frames with **no half-line offset**). So Y×2 recovers full-height progressive detail, and 1× "original" keeps today's line-doubling (E3).
- Targets are allocated scaled. Resolves sample one native-equivalent sample.
- **2D/sprite policy**, from PCSX2's upscaling knobs and paraLLEl-GS's sprite snapping:
  - draws with `PRIM=SPRITE` and FST=1 (HUD, fonts, the full-screen CT24 sprite) rasterise at scaled resolution, with UVs computed as **native-texel-exact**: `u = u0 + (x_native − x0)·du/dx` evaluated per native pixel, then bilinear in texel space with REGION clamps;
  - the result: edges stay sharp and atlas neighbours don't bleed (the equivalent of `RoundSprite`/half-pixel-offset fixes, without per-game hacks).
- The full-screen FIX-alpha sprite (if it is framebuffer feedback) samples the scaled target directly, so the effect stays at full resolution.

### 9.2 Widescreen (G3)
- The extra width must exist **before** VU1:
  1. Hook `sceVu0ViewScreenMatrix` (game code, 1 static caller) through the M3 function-hook API to scale the projection's X term (hor+).
  2. Widen VU1 clip/cull. `sceVu0ClipScreen`/`ClipScreen3` are game code, and the VU1 microcode's clip constants come from EE-built matrices; M0 confirms which.
- Renderer side:
  - a **virtual target width** (683 px at 1× for 16:9; GS X range permitting under XYOFFSET, to verify);
  - the scissor widened for 3D draws.
- 2D/HUD draws are classified (sprite + FST + Z ALWAYS) and remapped by an **anchor table keyed on the drawing function** (left/centre/right, the PS2 analogue of `gEXSetRectAlign`), set through hooks rather than the GS stream.
- The full-screen sprite stretches to the virtual width.
- Gate: E2 RAM equality is **not** expected to hold for widescreen if the hook changes game-visible matrices. So keep the hooked values **renderer-only**:
  - patch the copy that goes to VU1, never EE state the game reads back (e.g. culling or LOD decisions);
  - verify with E1/E2 replays.

### 9.3 High frame rate (G4)
- The GS backend can't interpolate: it only sees VU1 output. Mirroring RT64's design one level up:
  1. **Record** each logic frame's VU1 work at the VIF level: unpacked data and MSCAL program addresses (VU1 is already recompiled and deterministic).
  2. **Match** matrices between frames N−1 and N by (VU1 program, VU1 data address, draw order), the analogue of matrix-group ids. Tag HUD/2D programs as "ignore", like `G_EX_ID_IGNORE`.
  3. **Generate** each display frame by re-running the recompiled VU1 programs on a copy of VU1 memory with matrices blended (decompose into T/R/S as `RigidBody` does, since the EE may pre-multiply local→screen). This produces a fresh GS stream for the Metal backend.
  4. The EE is never touched, so logic stays at 50 Hz (**E1** by construction). The cost is extra VU1 runs plus renders per display frame.
- Census first (M0): which programs receive matrices, and whether they arrive as 4×4 unpacks.
- Fallback: camera-only interpolation (one view matrix), if per-object matching proves unreliable.

### 9.4 AA and filtering (G6)
- MSAA needs hardware depth (option a) or sample-rate shading with option b.
- With `RGBA8Uint`/`R32Uint` attachments, MSAA most likely needs a **custom resolve kernel**: hardware resolve generally doesn't cover integer formats. Verify against Apple's Metal feature-set tables in M6.
- "Original" filtering keeps the exact manual bilinear; "enhanced" uses hardware samplers (anisotropic) for 3D draws only, never for sprites.

## 10. Native app shell
| Need | Choice | Why (sources) |
| --- | --- | --- |
| Window, input, audio device | **SDL3**, replacing raylib | raylib renders through OpenGL (deprecated on macOS; no Metal). `SDL_Metal_CreateView`/`SDL_Metal_GetLayer` give a `CAMetalLayer`. SDL3 has the full gamepad DB (I1–I5) and HiDPI. Zelda64Recomp uses SDL2 (its `CMakeLists.txt`) |
| GS rendering API | **Raw Metal** (ObjC++ in `ps2xRuntime/src/lib/gs/metal/`) | **SDL_GPU exposes no framebuffer fetch/programmable blending** (SDL3 GPU wiki: one portable feature set, no feature queries), and §6.3 depends on it. Port to Vulkan/D3D12 later behind the same `GSRasterBackend` (X5) |
| Settings / launcher UI | **RmlUi**, as the N64Recomp family does; ImGui stays for the developer panel | Zelda64Recomp builds its menus and launcher on RmlUi (`src/ui/ui_launcher.cpp`, `ui_config.cpp`) with lunasvg and FreeType. **RmlUi ships GL/Vulkan/SDL_GPU renderers but no Metal one**, so we write a Metal `RenderInterface` (Zelda's renders through RT64's own RHI). ImGui has a first-party Metal backend (`imgui_impl_metal.mm`). AppKit-native settings would look right on macOS but block Windows/Linux (X5) and gamepad navigation |
| Config | JSON files in `~/Library/Application Support/RoadTripAdventure/` (`general.json`, `graphics.json`, `controls.json`, `sound.json`) | The Zelda64Recomp pattern (`src/game/config.cpp`: nlohmann JSON, `read_json_with_backups`, defaults for missing keys, `portable.txt` override). Meets X3 (corrupt → backup → defaults) |
| First-run disc flow (X1) | A launcher screen: pick ISO or BIN/CUE (`nativefiledialog-extended`, as Zelda) → hash/serial check for SLES-51356 → copy/convert to the app's data dir → run `apply_stub_policy` / table loaders → "Start" | Mirrors N64ModernRuntime `recomp::select_rom` → `RomValidationError {Good, FailedToOpen, NotARom, IncorrectRom, NotYet, IncorrectVersion, OtherError}`, `is_rom_valid`, `check_all_stored_roms`, `load_stored_rom` (`librecomp/include/librecomp/game.hpp`). Clear errors for the wrong disc or region. Nothing disc-derived is shipped |

- **Shell order:**
  1. SDL3 window + `CAMetalLayer`, presenting the CPU frame (option D). This removes raylib and keeps every test path.
  2. The Metal GS backend presents into the same layer (M4).
  3. RmlUi menus on a Metal render interface.
  4. The first-run flow.
- The debug panel moves from rlImGui to `imgui_impl_sdl3` + `imgui_impl_metal`.

## 11. Risks and open questions
- **Unknown RTT on sprites** (§2.3). Mitigated by the target-as-texture path and the resolve fallback; M0 decides.
- **Raster-coverage differences** vs the oracle at triangle edges: calibration in M2. If exactness is impossible, the SSIM/tolerance gate is the contract (C8 already allows SSIM 0.98).
- **Depth option (b) fill cost** at 8×, and MSAA interplay. Measured in M1 and M6.
- **Widescreen hooks** may leak into game logic (culling/LOD read back by the EE). E1/E2 replays guard it.
- **VU1-replay interpolation** is research-grade; the camera-only fallback is cheaper.
- **Licensing:** PCSX2 and paraLLEl-GS are references. Don't paste their code without a licence decision (paraLLEl-GS is LGPLv3+, verified; check the PCSX2 and RT64 licences before any reuse).
- **Evidence limits:** the prior-art summaries come from fetched source files, read through a summarising fetcher. `GSRendererHW.cpp` was truncated, so the internals of the blend levels and sprite fixes are named, not described. Re-read the source before implementing those parts.

## 12. Reproduce the census
```bash
P=<repo>
# census runs (headless, muted, fresh save dir, main runner read-only). Args: name, input, trace start s, alarm s
$P/work/tracks/M/census/run.sh race_t40 quick_race.txt 40 52
$P/work/tracks/M/census/run.sh ng_t85 new_game_intro.txt 85 95
python3 $P/work/tracks/M/census/analyze.py $P/work/tracks/M/census/race_t40/tris.txt   # count-only
python3 $P/work/tracks/M/census/analyze_runs.py $P/work/tracks/M/census/*/tris.txt      # state runs
# track R full-state census (pre-existing log): work/tracks/R/runs/census_race/log.txt
```
The trace window is wall-clock based (`PS2X_TRACE_TRIS` = seconds after process start), so the scene depends on machine load. Map it with the dump mtimes in `<run>/frames` against `<run>/start.txt`.

## 13. Track G results: M0 census and M1 skeleton (2026-10-08)
Built in `PS2Recomp-mtgs`; **merged into the main tree on 2026-10-08** (default backend stays `cpu`). Every number is a count, a register-field mode or a ratio. Raw outputs stay in `work/tracks/G/` (local only).

### 13.1 What was built
- **M0, `GSCensusBackend`** (`gs_census_backend.{h,cpp}`).
  - `PS2X_GS_CENSUS=<file>` wraps the inner raster backend inside `GSThreadedBackend`. It forwards every call unchanged and never reads VRAM, so it is output-neutral by construction.
  - Per present it writes an `F` line of counters. Every 100 presents it writes a cumulative `K` block of keyed modes.
  - `game/tests/gs_census_report.py` summarises the file.
  - Render-to-texture (RTT) test: page ownership per 8 KB page. It records the drawing frame (presents with ≥1 primitive) of the last frame/Z write, the last upload, and the last sample. It reports the age of a textured primitive's source pages, a "core" age (vertex texel box without the bilinear margin), and the source position relative to the draw's own FBP/ZBP (a block delta, not an address).
- **M1, `GSMetalBackend`** (`gs_metal_backend.{h,mm}`, Objective-C++, APPLE only).
  - Selected with `PS2X_GS_BACKEND=metal|tee`. The default stays `cpu`. The factory is `gs_frontend.cpp` `makeInnerRasterBackend`.
  - `.mm` is compiled as CXX with `-x objective-c++ -fobjc-arc`. `enable_language(OBJCXX)` reclassified GLFW's `.m` files and broke the raylib build.
  - Offscreen `MTLDevice` and queue; recording on the GS thread (inner backend of `GSThreadedBackend`).
  - **The CPU VRAM shadow stays authoritative.**
    - Metal-eligible draws collect into a *run* on one target (FBP, ZBP, FBW). The colour and Z planes are `R32Uint` textures holding raw VRAM words.
    - Any other operation closes the run first: commit, wait, then write the dirty rectangle back into the shadow through the CT32/Z24 swizzlers.
    - Targets re-upload from the shadow when a per-page epoch shows a CPU-side write since their last sync. CPU writes are fallback draws, transfers, clears, `WriteVram` and write-backs from other targets.
  - **First draw path:** untextured triangles (lists, strips, fans) and sprites, CT32 frame + Z24.
    - The fragment shader runs the whole integer pipeline with framebuffer fetch: fog, alpha test + AFAIL, DATE, ZTST on raw Z24, blend `((A−B)·C>>7)+D` with PABE, FBA, FBMSK, and RGB_ONLY alpha preservation.
    - **Depth option (b) (Z in an `R32Uint` colour attachment) is implemented. Option (a) was not prototyped.**
  - **Exactness method.** The oracle's float expressions were read from LLVM IR (`-O0 -emit-llvm` of `gs_cpu_backend.cpp`, `llvm.fmuladd` pattern):
    - `W = (fma(A, px−fx2, B·(py−fy2))·winding)·invAbsDenom`;
    - `w2 = (1−w0)−w1`;
    - colour/fog `fma(c2,w2,fma(c0,w0,c1·w1))`;
    - Z `fma(z2,w2,fma(z0,w0,z1·w1))` in double, then `u32(z+0.5)`.

    These draws run under the game thread's FPCR (round toward zero + FZ, replayed on the GS thread), and the GPU rounds to nearest. So the shader:
    - emulates RTZ with error-free transforms (TwoSum, fma residual, and 2^64 scaling for small operands, because the GPU flushes denormal residuals);
    - computes Z with a 64-bit-integer soft double;
    - takes the mode per draw from FPCR.RMode at record time.

    Per-primitive constants (bbox, edge coefficients, 1/|denom|, sprite rectangle and Z) are computed on the CPU with the oracle's own expressions.
  - **Per-draw CPU fallback:** an embedded `GSCpuBackend` on the same shadow, counted by reason (`textured_tri`, `textured_sprite`, `line`, `point`, `frame_psm`, `zbuf_psm`, `fbw`, `scissor`, `vram_range`, `fz_overlap`, `z_not_float`).
- **`GSTeeBackend`** (`gs_tee_backend.{h,cpp}`, `PS2X_GS_BACKEND=tee`).
  - The oracle owns the frontend VRAM and serves every readback and the presented frame. Metal works on a private 4 MB copy and receives the identical stream, Metal first.
  - Per run, it compares raw frame and Z words in the dirty rectangle. Per present, it compares the RGBA frames (exact pixels, max channel difference).
  - Resync is per frame by default (`PS2X_GS_TEE_RESYNC=frame|run|none`).
  - Logs go to `PS2X_GS_TEE_LOG`. PPM pairs go to `PS2X_GS_TEE_DIR`, every `PS2X_GS_TEE_EVERY` frames, plus mismatching frames.
  - Tools: `game/tests/tee_report.py` and `game/tests/ssim.py` (pure Python + PIL; 8×8 uniform window by integral images, or `--gaussian` 11×11 σ 1.5; `--tee DIR` pairs the PPMs).
- **Test switches:**
  - `PS2X_GS_METAL_SELFTEST=<n>` checks the shader's RTZ/RNE add, mul, fma, colour interpolation and soft-double Z against the host FPU;
  - `PS2X_GS_METAL_FAULT=1` injects a 1-LSB error into every Metal colour write (comparator control).

### 13.2 M0 census (deterministic `det_capture` recipe, full reference length; counts only)
Runs: boot 8100 presents (7895 drawing), race 7967 (7856), newgame 8000 (7926). "Per frame" = mean per drawing frame.

| Item | boot | race (quick_race) | newgame |
| --- | --- | --- | --- |
| Primitives per frame (mean / max) | 18 120 / 66 973 | 19 409 / 53 621 | 17 581 / 58 189 |
| Triangles / sprites / lines / points per frame | 17 929 / 191 / 0 / 0 | 19 305 / 104 / 0 / 0 | 17 463 / 118 / 0 / 0 |
| **Untextured share** (M1 Metal path) | 95.0% | 60.5% | 40.8% |
| Textured per frame (of which sprites) | 901 (189) | 7 673 (103) | 10 400 (117) |
| FRAME PSM / ZBUF PSM | CT32 / Z24 (1 primitive with fbw=10, CT32 and Z PSM 0, at boot) | same | same |
| FBP / ZBP | FBP {0, 64}, FBW 8; ZBP one value, ZMSK 0 | same | same |
| ALPHA (ABE=1) | (Cs−Cd)·As+Cd 99.97%; (Cs−0)·As+Cd; (Cs−Cd)·FIX+Cd with FIX 32 (≈1/frame) and 64 (rare); ABE=0 0.01% | same set | same set |
| TEST | ATE GEQUAL AREF 1 KEEP 67%; **DATE=1 DATM=1 33%** (untextured, ZALWAYS); RGB_ONLY AREF 128 | AREF 1 KEEP 61%; AREF 127 RGB_ONLY with ZGEQUAL 30% / ZGREATER 6%; AREF 128 RGB_ONLY ZALWAYS 2% | AREF 127 RGB_ONLY 55% (GEQUAL+GREATER); AREF 1 KEEP 39%; DATE 3.5% |
| FBA / PABE / DTHE / FBMSK≠0 | never | never | never |
| COLCLAMP=0 | 1 primitive (the fbw=10 boot primitive) | same | same |
| XYOFFSET per buffer | FBP 0: OFX 28672, OFY 30720; **FBP 64: OFY 30728 (+8 = +0.5 px)** | same | same |
| SCISSOR | 0,0–511,255 for both buffers (+1 at 639,447 once); race and newgame add sub-rectangles (course-select cards, inset views) | | |
| CLUT loads per frame | 27.5 | 349 | 550 |
| Host→local transfers | 242 total, 1.81 MB, in 7 frames | 580, 3.16 MB, 7 frames | 695, 3.07 MB, 22 frames |
| Upload to a DBP already uploaded this frame | 0 | 0 | 0 |
| Upload into pages sampled earlier in the frame | 12 | 10 | 13 |
| Upload into pages a draw wrote this frame | 0 | 0 | 0 |
| Local→local | 0 | 0 | **42 213 (5.3/frame, 4.6 KB/frame)**: T4→T4 64×128 and 64×16, T8→T8 64×128; 0 from draw-written pages |
| Local→host | 0 | 0 | 0 |
| TEXFLUSH | 16 | 39 | 34 |
| `ClearFramebuffer` calls | 0 | 0 | 0 |

**Render-to-texture (the M0 question):** yes, one case.
- The full-screen sprite (PRIM SPRITE, FST, TEX0 CT24, TW 9/TH 8, TBW 8, ALPHA (Cs−Cd)·FIX+Cd with FIX 32 = 25%) samples **the other frame buffer**. Its TBP is the current FBP ± 2048 blocks (64 pages), with UV = XY (texel = pixel, no shift).
- The race RTT-detail run (`census/race_rtt.txt`, 1795 presents, 2989 such sprites) shows where the source was last drawn:
  - **1940 sprites (one per frame): one drawing frame earlier (core age 1).** That makes it previous-frame feedback, a motion-blur/trail blend.
  - 1029 sprites in about 150 frames (several per frame, menu and course-select screens): the same frame (core age 0).
  - 19 sprites: two frames earlier.
- In the key, `src=fb+2048` is the draw to FBP 0 reading FBP 64. The draw to FBP 64 reading FBP 0 has a delta of −2048 blocks, which the key's range prints as `other`.
- The "age 0" and "Z as texture" hits in the coarse counters are the bilinear margin row: it reaches one page row past the source buffer, into the current frame or Z buffer.
- A few frames with a UV−XY shift of (−2, −1) and up to 50 such sprites a frame exist in menus. They read pages drawn the same frame (core age 0).
- One frame at boot and newgame has 52 textured triangles whose TEX0 is all zero, so they read FBP 0.
- **Consequence:** §6.4 "texture from a target" is **required**. The sprite must sample the other buffer's GPU target directly (it must not be resolved), and the previous frame's target must persist.

**M0 items not covered by this census** (still open from §2.3/§8):
- which VU1 programs receive 4×4 matrices through VIF unpacks (G4, §9.3);
- a town / free-roam route (Peach Town, Q's Factory DATE scenes beyond `new_game_intro`).

The full-route census runs above used the first track-G binary. The final binary (with the RTT core-age and source keys) was census-checked on the 200 s race run (61/61 frames byte-identical). The census only observes, so this doesn't change any count.

**Other findings:**
- XYOFFSET.OFY differs by 8 (0.5 px) between the two buffers. The CPU oracle uses `OFY>>4`, which drops it. This matters for G7/M5 (field offset): re-check §2.2 "no half-line offset" against the register, not only the vertices.
- No transfer touches a target. Local→local appears only in newgame, between texture areas.

### 13.3 M1 results (`det_capture`, deterministic; reference `work/ref_20261008`)
| Run | Result |
| --- | --- |
| GPU self-test (`PS2X_GS_METAL_SELFTEST=1048576`) | 0 mismatches in 1 048 576 cases × {add, mul, fma, colour interp, Z} × {RNE, RTZ}. Before small-operand scaling: 340/65 536 RTZ mul mismatches, all with products < 2^-79 (GPU denormal flush of the residual) |
| Comparator control (`PS2X_GS_METAL_FAULT=1`, tee, boot, 20 frames) | run pixels matching 0.59%, present pixels matching 54.9%, SSIM min 0.99989. The tee and `ssim.py` both detect a 1-LSB error. `ssim.py` sanity: 3-px shift 0.927, Gaussian blur σ 2: 0.671 (uniform) / 0.670 (Gaussian) |
| `PS2X_GS_BACKEND=metal`, boot | **267/267 frames byte-identical** to the reference. Metal draws written back into the shadow, CPU fallback and presentation from the shadow: no ownership error |
| tee, race (1500 s; reached tick ~7100, into the race) | 4558 presents, **4556/4556 compared frames exact**; Metal-handled draws **58.3%** (53.7 M of 92.2 M); 683 029 runs, **1 263 385 898 / 1 263 385 898 run pixels exact** (frame and Z); SSIM 45 PPM pairs: min 1.000000; presented frames 142/142 identical to the reference; fallbacks only `textured_tri` (37.7 M) and `textured_sprite` (0.41 M) |
| tee, boot (1200 s; reached the office) | 4474 presents, **4472/4472 compared frames exact**; Metal-handled draws **94.7%** (69.6 M of 73.4 M); 278 071 runs, **2 973 335 401 / 2 973 335 401 run pixels exact**; SSIM 44 pairs: min 1.000000; presented frames 217/217 identical to the reference; fallbacks only `textured_tri` and `textured_sprite`; 0 crashes or command-buffer errors |
| tee, boot, `PS2X_GS_TEE_RESYNC=none` (Metal copy never re-synchronised) | 4528/4528 frames exact, 281 810 runs, 3 007 560 642 run pixels exact. With no resync, any target-staleness or page-epoch error would have accumulated, and none did |
| Census runs (`PS2X_GS_CENSUS`) | boot 285/285, race 255/255, newgame 216/216 byte-identical (output-neutral) |
| **Gate: default (`cpu`) backend, final binary** | boot **285/285**, race **255/255**, newgame **216/216** byte-identical to `work/ref_20261008` (full reference length) |
| **Gate: `quick_race` real time, default backend** | PASS: fps median 49.9, 3D min 49.9 / median 49.9, 0 VU1 fallbacks (idle machine, 2026-10-08; `work/tracks/G/gates/rt_quick_race_final`) |

**Not measured yet** (deferred to M2/M3, per the M1 scope):
- **fps.** Every run waits on the GPU and writes back, and the tee rasterises twice on the CPU (race tee ≈ 2.7 ticks/s). So no Metal fps or depth (a) vs (b) fill-rate numbers exist.
- **Synthetic pixel tests** (`ps2xTest/gs_metal`).
- **Textured draws.** All of them fall back.
- **MSAA/scale.**

`check_jump_tables.py` does not apply: `work/generated` was not regenerated.

### 13.4 Reproduce
```bash
G=<repo>/work/tracks/G; V=$G/gates/run_variant.sh   # det_capture wrapper, runner = $G/bin/runner
$V census_race race 900 PS2X_GS_CENSUS=$G/census/race.txt && python3 game/tests/gs_census_report.py $G/census/race.txt
$V tee_race race 1500 PS2X_GS_BACKEND=tee PS2X_GS_TEE_EVERY=100 && python3 game/tests/tee_report.py $G/gates/tee_race/tee.log
python3 game/tests/ssim.py --tee $G/gates/tee_race/tee
$V metal_boot boot 900 PS2X_GS_BACKEND=metal          # compares against work/ref_20261008
```

## Sources
- PCSX2 (master, fetched 2026-10-08):
  - `pcsx2/GS/Renderers/Metal/{GSDeviceMTL.h, GSDeviceMTL.mm, GSMTLDeviceInfo.mm, tfx.metal}`;
  - `pcsx2/GS/Renderers/Common/GSDevice.h`;
  - `pcsx2/GS/Renderers/HW/{GSTextureCache.h, GSRendererHW.cpp (truncated)}`;
  - `pcsx2/Config.h`.
- paraLLEl-GS: https://themaister.net/blog/2024/07/03/playstation-2-gs-emulation-the-final-frontier-of-vulkan-compute-emulation and https://github.com/Arntzen-Software/parallel-gs (README).
- RT64 (main): README; `include/rt64_extended_gbi.h`; `src/hle/{rt64_workload.h, rt64_workload_queue.cpp, rt64_game_frame.cpp}`; `src/render/rt64_texture_cache.h`.
- Zelda64Recomp (dev): README, `CMakeLists.txt`, `src/game/config.cpp`, `src/ui/`. N64ModernRuntime: README, `librecomp/include/librecomp/game.hpp`.
- SDL3 GPU API: https://wiki.libsdl.org/SDL3/CategoryGPU. RmlUi README (backend list).
- Project code: `ps2xRuntime/include/runtime/gs/{gs_backend.h, gs_threaded_backend.h, gs_types.h, gs_bilinear.h}`, `src/lib/gs/{gs_cpu_backend.cpp, gs_frontend.cpp}`, `src/lib/Kernel/Stubs/GS.cpp`, `src/lib/ps2_runtime.cpp`.
