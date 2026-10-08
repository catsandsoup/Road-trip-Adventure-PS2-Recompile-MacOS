# VU1 static recompiler

Status (2026-10-06): working. On the smoke route the shipping runner runs all VU1 microprograms as native C++. The interpreter is never used (0 fallbacks). The differential test reports 0 mismatches, bit for bit, against `VU1Interpreter`. In the first 3D scene, game fps goes from **~9.5 to ~41**.

## Pieces

| What | Where |
|---|---|
| Decoder (line-by-line port of the interpreter's `decodeUpperUsage`/`decodeLowerUsage`/`decodeInstructionPair`) | `game/tools/vu1recomp/vu1decode.py` |
| Generator | `game/tools/vu1recomp/vu1recomp.py` |
| Runtime patch script (idempotent; applies the integration edits to any PS2Recomp tree) | `game/tools/vu1recomp/apply_runtime_patch.py` |
| Offline replay + fuzz test | `game/tests/vu1/{vu1_replay.cpp,vu1_stubs.cpp,build.sh}` |
| Registry, MSCAL dispatcher, PATH1 model, diff test, capture, stats | `PS2Recomp-vu1/ps2xRuntime/src/lib/vu/ps2_vu1_recomp.cpp` and `include/runtime/ps2_vu1_recomp.h` |
| Inline helpers used by generated code | `PS2Recomp-vu1/ps2xRuntime/include/runtime/ps2_vu1_recomp_support.h` |
| Generated code (disc-derived, never committed) | `work/vu1gen/vu1rec_<hash>.cpp` |

## Build and run

```sh
R=<project root>
cmake -S $R/PS2Recomp-vu1 -B $R/PS2Recomp-vu1/out/rta -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DPS2X_RUNNER_DIR=$R/work/generated -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_STUDIO=OFF \
  -DPS2X_ENABLE_AGRESSIVE_LOGS=OFF -DPS2X_ENABLE_LTO=OFF \
  -DPS2X_VU1_CATALOG_DIR=$R/work/vu1cat \
  -DPS2X_VU1_GENERATOR=$R/game/tools/vu1recomp/vu1recomp.py \
  -DPS2X_VU1GEN_DIR=$R/work/vu1gen
cmake --build $R/PS2Recomp-vu1/out/rta --target ps2EntryRunner
```

- The generator runs at configure time from the local catalog. The catalog is captured with `PS2X_VU1_CATALOG=<dir>`.
- CMake re-runs the generator whenever `index.txt` or the generator scripts change.
- Every `vu1rec_*.cpp` in `PS2X_VU1GEN_DIR` is compiled into `ps2EntryRunner` with:
  - no unity build and no PCH;
  - `-O2 -ffp-contract=off -frounding-math`.
- It goes into `ps2EntryRunner`, not into the `ps2_runtime` static library, so the static registrars survive linking.
- Without `PS2X_VU1GEN_DIR`, nothing changes and every call goes to the interpreter (counted as `noimage` fallbacks).

Runtime environment variables:

| Variable | Effect |
|---|---|
| `PS2X_VU1_INTERPRETER=1` | Force the interpreter (comparison runs). |
| `PS2X_VU1_DIFF=1` | Differential test on every MSCAL. The interpreter result is what the game keeps. |
| `PS2X_VU1_DIFF_LOG=<n>` | Log the first n mismatching calls in detail. |
| `PS2X_VU1_STATS=<sec>` | Print the `[vu1rec]` counters every `<sec>` seconds (default 5): MSCAL count, native, fallbacks (no image / no entry), MSCNT, native failures, diff mismatches, MSCAL/s, VU1 Mcycles/s, DISPFB2 changes/s (= game fps), share of wall time spent in VU1. |
| `PS2X_VU1_CAPTURE=<dir>` | Write sampled MSCAL input snapshots for the offline test. They are disc-derived and must stay out of git. |

Offline test:

```sh
game/tests/vu1/build.sh
work/vu1test/vu1_replay work/vu1cat work/vu1snap [--fuzz N] [--no-ftz]
```

## Design

**The program as a graph of (pc, pipeline state) nodes.**
- `resetScheduler()` runs at every MSCAL, so every program starts with an empty pipeline.
- The generator therefore runs the interpreter's scheduler *abstractly* at generation time:
  - stalls from `calculatePairReadyCycle`;
  - `markPairWrites`;
  - the eight-slot MAC/status/clip flag pipeline with 4-cycle commit, including `FSSET`/`FCSET` cancelling same-cycle entries;
  - FDIV latency 7/13 and EFU latencies with resource throughput;
  - the VI branch backup quirk;
  - branch delay slots;
  - the E-bit delay slot.
- Each node is one instruction pair under a fully known pipeline state. As a result these are all compile-time constants:
  - every stall;
  - every commit point of MAC/status/clip/Q/P;
  - every cycle-counter increment.
- The program reads Q without `WAITQ` (`MULq` after `DIV`) and clip via `FCAND`, so this timing model is necessary for exactness, not optional.
- The only data-dependent timing is an XGKICK stalling behind a previous XGKICK. It becomes a run-time `switch` over the bounded number of stall cycles, and each case continues in its own node.
- For the RTA image: 211 reachable pairs, 228 nodes (almost no state duplication), about 4k lines of C++ for both variants.

**Registers.**
- VF/VI/ACC read-after-write hazards always stall in the interpreter, so register semantics are sequential.
- Pair rules:
  - both halves read pre-pair state (the upper result is written back after the lower executes);
  - when upper and lower write the same VF register, the lower write is dropped entirely;
  - with the I bit set, the upper sees the old I;
  - VI writes are truncated to int16;
  - vf0 and vi0 are constants.
- VF registers live in NEON locals (`float32x4_t`), with masked writes via `vbslq`.

**Float semantics: bit-identical to the interpreter's exact-FMAC path.**
- The code runs under `FE_TOWARDZERO`, as `VU1Interpreter::run` does.
- Clang compiles the interpreter's `acc ± vs*vt` lines to `fmadd`/`fmsub` (checked in the object code: 32 `fmadd` and 35 `fmsub` in `ps2_vu1_upper.o`). The generator therefore emits `vfmaq_f32`/`vfmsq_f32` for MADD*, MSUB* and OPMSUB, and plain mul/add elsewhere. The generated TU is built with `-ffp-contract=off` so nothing else gets fused.
- Operands are clamped (denormal → ±0, Inf/NaN → ±FLT_MAX) only for lanes not statically known to be normalized. A forward dataflow pass tracks this per lane; FMAC/MAX/MINI/ITOF/ABS results are known to be normalized.
- Results flush denormals to signed zero.
- Why this matches the interpreter: with clamped operands, every operation is a single correctly rounded (RTZ) operation. Its result is zero iff the exact result is zero, below FLT_MIN iff the exact result is, and saturates to FLT_MAX on overflow. That is exactly what `normalizeFmacExactResult` produces from its long-double exact value.
- MAX/MINI are compare+select (`(a>b)?a:b` returns b for +0/−0), not `fmax`.
- FTOI uses `fcvtzs` on the power-of-two-scaled value. ITOF uses `scvtf` under RTZ.
- DIV/SQRT/RSQRT/EFU are copied from the interpreter. EFU spells out the contraction the interpreter's object code shows. One gap: the `ESADD`/`ELENG` sum-of-squares shape follows LLVM's folding rule, but no RTA program uses EFU, so the diff test does not cover it.

**Flags.**
- MAC/status/clip and the clip working register are variables.
- A backward liveness pass over the node graph deletes every flag computation that no `FM*`/`FS*`/`FC*` reader can observe.
- At exit, a flag is live only if some program in *any* cataloged image reads it. `VU1State` persists across MSCALs and across code uploads, so the generator takes the union of readers over all images. An image that fails to compile counts as reading every flag. The RTA image reads only clip:
  - all 111 FMAC flag producers are eliminated in the fast variant;
  - the 2 CLIPs stay.
- A *full* variant keeps every flag live, computed exactly like the interpreter (including product sticky bits and FDIV D/I). It is only used by the diff test, to prove that the flag model is exact too.

**XGKICK / PATH1.**
- PATH1 is a lazy but cycle-exact model of `progressXgkick`: qword k is copied at issue+1+2k.
- Before every VU data store, the engine is advanced to the current cycle. Packets are therefore byte-identical even if a program overwrites a buffer that is still being sent. RTA does exactly that with an `ISWR` right after its double XGKICK.
- Packets go through the same `submitGifPacket(Path1, …)` call, in the same order.
- The final cycle count includes the end-of-program flush and the kick drain. The interpreter's cycle counter is advanced identically, so mixing native and interpreted calls stays consistent.

**Integration.**
- Image lookup is keyed by the catalog's FNV-1a hash of the 16 KiB micro memory. The hash is recomputed only when `PS2Memory::getVU1CodeGeneration()` changes.
- An unknown image or start PC falls back to the interpreter and is counted (and logged once per image).
- Loop heads check the 65536-cycle budget, so a runaway program fails instead of hanging the game.
- MSCNT is counted and always resumes in the interpreter. This is safe because native runs always complete, so the interpreter's pipelines are empty.

## Results

**Differential test** (`PS2X_VU1_DIFF=1`, replay `boot_to_title.txt`, both variants against the interpreter):
- Compared: GIF packets byte for byte, VF/VI/ACC/Q/P/I/R/pc, MAC/status/clip (full variant), all 16 KiB of data memory, and the cycle count.
- Final binary: **425,727 MSCALs, 0 mismatches** (full: 0, fast: 0; interpreter budget stops: 0).
- An earlier run on the pre-resync build: 474,623 calls, 0 mismatches.
- There are no float-rounding-only differences to excuse; results are exact.

**Offline replay** (538 captured snapshots across all three entries):
- 0 mismatches.
- Fuzzing VF registers and float words in data memory towards denormal, ±0, Inf, NaN and huge/tiny values:
  - 16,538 runs with FTZ, 0 mismatches;
  - 5,866 runs without FTZ, 0 mismatches.
- Some fuzzed inputs produce invalid GIF tags (format 3). In those cases the interpreter reports a reserved instruction and stops, while native code flags the call as failed. These runs (140 + 52) are excluded from the comparison. This is an error-path difference only and never happens on real data.

**Fallbacks on the smoke route:** 0. All MSCALs are native (`noimage=0 noentry=0 nativeFail=0`); MSCNT was never seen.

**Performance** (same binary, same replay, 3D scene after ~60 s, 5 s windows; game fps = DISPFB2 changes per second, which is one per game frame — DISPFB1 never changes):

| | Interpreter (`PS2X_VU1_INTERPRETER=1`) | Native |
|---|---|---|
| Game fps | 9.4–9.7 | 38–46 (typically ~41) |
| Wall time spent in VU1 | ~78 % | ~5 % |
| VU1 time per frame | ~82 ms | ~1.3 ms |
| MSCAL/s | ~21,300 | ~90,000 (same ~2,200 per frame) |
| VU1 Mcycles/s | 7.2 | 31 |

- That is roughly 4.3× game fps, and VU1 execution itself is about 60× faster.
- VU1 is no longer the bottleneck. The remaining ~95 % of frame time is elsewhere (EE, GS rasterization, …).

## Files changed in PS2Recomp-vu1 (to merge into the main tree)

New:
- `ps2xRuntime/include/runtime/ps2_vu1_recomp.h`
- `ps2xRuntime/include/runtime/ps2_vu1_recomp_support.h`
- `ps2xRuntime/src/lib/vu/ps2_vu1_recomp.cpp`

Modified (small, anchored edits; `game/tools/vu1recomp/apply_runtime_patch.py <PS2Recomp>` applies all of them and copies the new files):

| File | Change |
|---|---|
| `ps2xRuntime/include/runtime/ps2_vu1.h` | `setPacketSink`, `cycleCount`/`setCycleCount`, `lastRunEnded`, three members |
| `ps2xRuntime/src/lib/vu/ps2_vu1_core.cpp` | `finishXgkick` honours the packet sink; `run()` records `m_lastRunEnded`. The interpreter's semantics are otherwise unchanged. |
| `ps2xRuntime/include/runtime/ps2_memory.h` | DISPFB swap counters (`dispfbSwapCount`, `dispfb2SwapCount`, `noteGsPrivWrite`) |
| `ps2xRuntime/src/lib/ps2_memory.cpp` | `noteGsPrivWrite` at the three GS privileged-register write sites (fps meter) |
| `ps2xRuntime/src/lib/ps2_runtime.cpp` | include; `vu1NativeDispatcher()`; MSCAL tries native first, else the interpreter; MSCNT counted; `catalogVu1Program` merges an existing `index.txt` |
| `ps2xRuntime/CMakeLists.txt` | `ps2_vu1_recomp.cpp` added to `ps2_runtime`; `PS2X_VU1_CATALOG_DIR` / `PS2X_VU1_GENERATOR` / `PS2X_VU1GEN_DIR` block |

Everything else in PS2Recomp-vu1 matches the main PS2Recomp tree as of 2026-10-06 (re-synced; the interpreter sources are unchanged upstream).

## Open issues / not verified

- **Catalog coverage.** Only one VU1 image (`0d8469a652275b91`, entries 0x10/0x20/0x50) appeared on this route.
  - Other areas of the game may upload other microcode. Those calls fall back to the interpreter (counted, logged once per image) until the catalog is re-captured there and regenerated.
  - Recommendation: keep `PS2X_VU1_CATALOG=work/vu1cat` on during longer play sessions.
- **Catalog merging (fixed).** `catalogVu1Program` used to rewrite `index.txt` with only the current session's entries, so a short session could drop entries from codegen. It now loads and merges the existing index first (verified: counts accumulate across runs). Two earlier diff runs had already replaced the counts; the entry set was unaffected.
- **`PS2X_VU1_CATALOG` costs performance.** It FNV-hashes the 16 KiB image on every MSCAL. With native VU1, a catalog-enabled run drops to ~14.6 fps in the 3D scene (vs ~41). Use it for capture runs only, never for measurements.
- **Uncataloged images.** If an uncataloged image runs in the interpreter and reads MAC/status before writing them, it sees the stale values the fast variant leaves. No such image is known; re-cataloging and regenerating makes those flags live automatically.
- **Visual check.** A native-mode frame dump of the 3D office scene (`PS2X_DUMP_FRAMES`) renders the scene. VU1 output is byte-identical to the interpreter's, so remaining visual issues there (e.g. a large black floor area) are not caused by VU1.
- **Supported but not exercised by RTA:** JR/JALR dispatch over BAL/JALR return labels (no JR is reachable in this image), EFU, FM*/FS* readers, FSSET/FCSET.
- **Refused by the generator** (the whole image then falls back): D/T bits, branches in delay slots, and an upper write to vf0 read by the same-pair lower.
- **Error paths differ.**
  - Budget overrun or an invalid GIF tag makes native code mark the call failed (`nativeFail` counter, logged), where the interpreter stops mid-program.
  - Never seen with real data.
- **Fast-variant flags.** Because no cataloged program reads them, the fast variant leaves `VU1State.mac`/`status` stale. The full variant proves the flag model itself is exact.
- **Further speedups not done.** Propagating normalized lanes through MOVE/MR32, and hoisting loop-invariant operand clamps (LLVM probably does part of this). With VU1 at ~5 % of frame time, this is low priority.
