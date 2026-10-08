# VU0 Macro-Mode (COP2) Flags: Research Notes

Read-only research for the COP2/VU0-macro translation in the static recompiler.
Every claim below cites a primary source.

## Sources

- **[VUM]** Sony *VU User's Manual*, Version 6.0 (SCE). Archive.org item
  [`ps2-development-docs`](https://archive.org/details/ps2-development-docs), file `VU User's manual.pdf`.
  Page numbers are the printed page numbers, which match the PDF page index (checked on pp. 41, 42 and 215).
  The manual is copyrighted, so it is paraphrased here rather than quoted.
- **[EEISA]** Sony *EE Core Instruction Set Manual*, same archive.org item.
- **[ps2tek]** <https://psi-rockin.github.io/ps2tek/>, section "VU Registers" (MAC Flags, Clip Flags, Status Flags) and "IEEE 754 Differences".
- **PCSX2**, pinned to commit `9fffbdbd59b962d63a2259b150f419ad3773e7b4`. All line links below use that commit.
  PCSX2 has **two** macro-mode implementations, and they disagree in places (see §8):
  - **Interpreter (interp).** The `V*()` wrappers in `VUops.cpp` (L3887+) call the shared `_vu*()` op, then a `SYNC*()` helper. COP2 transfers are in `VU0.cpp`.
  - **microVU macro (mVU).** `microVU_Macro.inl` reuses the microVU emitters with `mVU.cop2 = 1`. This is what the x86 EE recompiler uses.

---

## TL;DR: model to implement

1. **vf0 destination.** Compute the result and update the flags, then throw the value away. vf0 stays `(0,0,0,1.0)`.
   This applies to every FMAC op, including `vsub.xyw vf0, a, b` (the compare idiom).
   Ops that set no flags (MOVE, MR32, FTOI, ITOF, ABS, MAX, MINI, QMTC2, LQC2) simply skip the write.
   LQC2 still performs its memory read.
2. **MAC (vi17), 16 bits.** `O[15:12] U[11:8] S[7:4] Z[3:0]`. Within each nibble x is bit 3 and w is bit 0.
   - Computed for the fields in `dest` only. Fields outside `dest` read 0.
   - The 4-bit `dest` value `(code >> 21) & 0xF` is already in the same x=bit3..w=bit0 order.
3. **Status (vi16), 12 bits.** `DS IS OS US SS ZS D I O U S Z` (bit 11 down to bit 0). After each FMAC op:
   - `st = (st & 0xFF0) | nz;`
   - `st |= nz << 6;`
   - Here `nz` = (Z any, S any, U any, O any) taken from the new MAC, as a 4-bit value.
   - FMAC ops never touch I, D, IS or DS.
   - DIV and RSQRT replace I and D, and OR them into IS and DS. SQRT clears D, sets I from its input, and ORs I into IS.
4. **CTC2.** Writing vi16 changes **only bits 6..11**; bits 0..5 are preserved.
   vi17 is read-only (the write is ignored). vi18 takes the low 24 bits.
5. **CFC2.** Reading vi16, vi17 or vi18 returns the 12-, 16- or 24-bit value, zero in the upper bits, sign-extended to 64 bits (in practice zero-extended).
6. **Visibility.** PCSX2 makes flags visible to the very next CFC2 (immediate model) and fixes the games that depend on hardware pipeline delay with per-game patches.
   Road Trip (SLUS-20398) and Road Trip Adventure (SLES-51356) have **no** such patches in GameDB.
7. **Numerics.**
   - Denormal inputs are treated as signed 0.
   - Underflowed results become signed 0 with Z=1 and U=1.
   - Overflowed results become ±MAX with O=1 and Z=U=0.
   - S is always the sign bit of the clamped result.
   - Rounding is toward zero.
   - PCSX2 uses ±0x7F7FFFFF for MAX. The hardware MAX is probably 0x7FFFFFFF; see §5.

---

## 1. VF00 as the destination

**Hardware.**
- VF00 is a constant register holding x=y=z=0.0 and w=1.0 [VUM p.32; ps2tek "VU Registers"].
- The manual's SUB entry says that using VF00 as the destination turns the instruction into a comparison of fs against ft [VUM p.118, SUB "Remarks"]. That only makes sense if the flags update and the register does not.
- Macro mode does no hazard checks on VF00 or VI00 [VUM p.211].

**PCSX2 interpreter.** The flags are updated and the result goes to a dummy register. There is no early return and no restore of VF[0].
- `_getDst()` returns `&VU->ACC` for ACC ops, a static scratch vector `RDzero` when `_Fd_ == 0`, and `&VF[_Fd_]` otherwise ([VUops L35][ops35], [L519-528][opsGetDst]).
- `applyBinaryMACOp` and `applyTernaryMACOp` assign `dst->i.x = VU_MACx_UPDATE(VU, result)` for each enabled field, then call `VU_STAT_UPDATE` ([L530-539][opsBin], [L682-691][opsTer]).
  The MAC update is part of producing the value, so it happens whether or not `dst` is the scratch vector.
- The macro wrapper (for example `VSUB()`) then calls `SYNCMSFLAGS()`, which publishes `VI[16]` and `VI[17]` ([L3926][opsVSUB], [L3889-3893][opsSync]).
- `VOPMSUB` follows the same scratch-vector (`RDzero`) path ([L849-871][opsOPMSUB]).
- Ops that set no flags return early when the destination is 0:
  - MAX and MINI: `if (_Fd_ == 0) return;` ([L795-818][opsMinMax]).
  - FTOI, ITOF and ABS: `applyUnaryFunction`, `if (_Ft_ == 0) return;` ([L495-505][opsUnary]).
  - MOVE ([L1086-1095][opsMove]) and MR32 ([L1118-1129][opsMR32]).
- COP2 transfers ([VU0.cpp][vu0]):
  - `LQC2` with `ft == 0` still calls `memRead128` into a local and discards it ([L94-103][vu0lqc2]). The memory access and its side effects still happen.
  - `QMTC2` returns early when the VF index is 0 ([L128-138][vu0qmtc2]).
  - `CTC2` returns early for vi0 ([L163-170][vu0ctc2]).

**PCSX2 microVU macro.**
- FMAC ops allocate the destination with `VFreg = 0`. The register allocator comments that vf0 "will not be written back" ([microVU_IR.h L209][ir209]).
- `mVUupdateFlags()` runs on the computed xmm before the register is released ([microVU_Upper L231-283][upFMACa]).
- LQC2 to vf00 is "tossed away" after the load ([microVU_Macro L838][macLQC2]). QMTC2 returns early for index 0 ([L686-692][macQMTC2]).

**Worked example.** `vsub.xyw vf0, vfA, vfB` has `dest = 0b1101` (x, y, w).
- MAC.z bits are all 0.
- For each of x, y and w: Z = (result == ±0) and S = result bit 31.
- If x == 0, y < 0 and w > 0, then MAC = Zx|Sy = 0x0008|0x0040 = **0x0048**.
- Status: low nibble = 0b0011 (Z, S); sticky |= 0b0011 << 6.

Two caveats:
- For identical operand registers (`vsub vf0, vfA, vfA`), mVU forces the result to +0 ([`doSafeSub`, microVU_Upper L173-189][upSafeSub]). An exponent-255 PS2 value held as host Inf would otherwise give Inf − Inf = NaN, and Z would never be set.
- A recompiler must clamp operands, or special-case this, so the compare idiom still produces Z.

## 2. MAC flag register (vi17 / CCR[2,17])

**Layout** [VUM p.39 and p.201; ps2tek]. Bit 15 down to bit 0:
`Ox Oy Oz Ow | Ux Uy Uz Uw | Sx Sy Sz Sw | Zx Zy Zz Zw`. So Zw is bit 0, Zx bit 3, Sx bit 7, Ux bit 11, Ox bit 15.
PCSX2 encodes this with shift 3 for x, 2 for y, 1 for z and 0 for w. The masks are Z=`0x0001<<sh`, S=`0x0010<<sh`, U=`0x0100<<sh` and O=`0x1000<<sh` ([VUflags L15-66][vf15]).

**Which ops update MAC** [VUM p.41, "Flag Changes for Each Instruction"]:

| Updates MAC and status Z/S/U/O (non-sticky and sticky) | No MAC/status change |
|---|---|
| ADD\*, ADDA\*, SUB\*, SUBA\*, MUL\*, MULA\*, MADD\*, MADDA\*, MSUB\*, MSUBA\*, OPMULA, OPMSUB (every bc/i/q variant) | ABS, CLIP (clip flag only), FTOI\*, ITOF\*, MAX\*, MINI\*, NOP, MOVE, MR32, MFIR, MTIR, IADD etc., loads and stores |

DIV, SQRT and RSQRT change only status D, I, DS and IS (§7).

PCSX2 agrees on the op list:
- Interp wrappers call `SYNCMSFLAGS` only for the flag-setting ops ([VUops L3911-4005][opsWrap]).
- mVU macro mode tags FMAC ops with mode `0x110` (writes status and MAC). It uses `0x0` for MAX, MINI, FTOI, ITOF, ABS, MOVE and MR32, `0x108` for CLIP, and `0x112` for DIV, SQRT and RSQRT ([microVU_Macro L164-282][macTable]).
- The EE recompiler's per-op classifier matches this ([`cop2flags`, iR5900.cpp L1418-1475][r5900flags]).

**Masking.** The manual says flags for FMAC units that do not operate are cleared to 0; for example, ADD.xyz leaves Zw, Sw, Uw and Ow at 0 [VUM p.39 and p.42 footnote].
- Interp: the `else VU_MAC?_CLEAR` branches clear all four bits of an unmasked field ([VUflags L69-86][vfClear]).
- mVU: the result is ANDed with `_X_Y_Z_W` ([microVU_Upper L10, L64-71][upFlags]).
- Interp bug: `_vuOPMULA` and `_vuOPMSUB` update only x, y and z and never clear the w bits, so stale w bits leak through ([VUops L841-871][opsOPMULA]). OPMULA and OPMSUB are encoded with dest = xyz, so w must read 0.

**Bit rules.**
- **Z.** Set when the result is 0 [VUM p.39]. A result of −0 counts.
  - Interp tests `f == 0`, which is true for −0 ([VUflags L26-30][vf26]).
  - mVU uses `CMPEQ.PS` against 0.0, which is also true for −0 ([microVU_Upper L61-71][upFlags]).
  - ps2tek: Z is set for 0.0 or −0.0, and this also clears O and U.
- **S.** The sign of the clamped result [VUM p.39: Z and S follow the clamped ±MAX/±0].
  Both PCSX2 paths use bit 31: interp at [VUflags L21-24][vf15], mVU with `MOVMSKPS`. So −0 gives Z=1 **and** S=1.
- **U.** Set by exponent underflow. The result becomes ±0 and **Z=1 as well** [VUM p.28 exception table; p.42 "Underflow" row: Z=1, S=sign, U=1, O=0].
  Interp: exponent 0 with a nonzero value sets Z|U and returns a signed zero ([VUflags L34-36][vf34]).
  In practice PCSX2 never sets U (see §5 and §8).
- **O.** Set by exponent overflow. The result becomes ±MAX with Z=0, U=0 and S=sign [VUM p.28; p.42 "Overflow" row].
  - Interp: a host exponent of 255 sets O, clears Z and U, and returns `sign|0x7F7FFFFF` when VU0 overflow clamping is on (the default) ([VUflags L37-42][vf37]; [Pcsx2Config L459][cfg459]).
  - mVU computes O **only** under the `VUOverflowHack` gamefix. The source comment says it cannot be detected with x86 range limits ([microVU_Upper L77-94][upOvf]).
- **MADD/MSUB detail** [VUM pp.42-43].
  - MAC and the non-sticky status reflect the final add/subtract.
  - The sticky bits also OR in the flags produced by the multiply stage.
  - An ACC that is already ±MAX forces a ±MAX result with O=1.
  - For MSUB, an overflowing product contributes with its sign inverted.
  - PCSX2 models none of the multiply-stage effects.

## 3. Status flag register (vi16 / CCR[2,16])

**Layout** [VUM p.39; ps2tek]. The status flag occupies the low 12 bits of CCR[2,16]; the rest is 0 [VUM p.201].

| bit | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| flag | Z | S | U | O | I | D | ZS | SS | US | OS | IS | DS |

PCSX2 agrees: `0x10` is I (invalid) and `0x20` is D (divide by zero) ([VUops `_vuDIV` L931-956][opsDIV]).

**Update rules.**
- Z, S, U and O are each the OR of the matching MAC nibble from the latest flag-setting FMAC op [VUM pp.39-40].
- The six sticky flags accumulate: each new value is ORed into the old one [VUM p.40].
- The per-instruction table [VUM p.41] shows that FMAC ops update `--XXXX` in **both** the sticky and the non-sticky columns, so I, D, IS and DS are left alone.
  When reading that table, note that the first status column is the sticky one (labelled DIOUSZ over SSSSSS) and the second is the non-sticky one.
- FDIV rows: DIV and RSQRT have sticky `XX----` and non-sticky `XX----`. SQRT has sticky `-X----` and non-sticky `0X----`: D is cleared, I is updated, DS is untouched and IS is ORed.
- The text on p.40 says the same thing: D is cleared by SQRT regardless of its result.

```
// after FMAC op (nz = {O_any,U_any,S_any,Z_any} from new MAC)
status = (status & ~0x00F) | nz;      // Z,S,U,O replaced; I,D (bits 4,5) kept
status |= nz << 6;                    // ZS,SS,US,OS accumulate
// after DIV/RSQRT (i,d computed per §7)
status = (status & ~0x030) | (i<<4) | (d<<5);
status |= (i<<10) | (d<<11);
// after SQRT
status = (status & ~0x030) | (i<<4);  // D forced 0
status |= (i<<10);
```

- **mVU** matches this.
  - It keeps status in a "denormalized" internal layout ([microVU_Alloc L47-77][alloc47]).
  - FMAC ops clear only the non-sticky Z, S, U and O bits (`AND 0xfffc00ff`), then OR in the new sticky and non-sticky bits ([microVU_Upper L54-59, L97-109][upFlags]).
  - In COP2 mode, DIV, SQRT and RSQRT clear only the non-sticky I and D (`AND ~0xC0000`), then OR in `divI = 0x1040000` (I + IS) or `divD = 0x2080000` (D + DS) ([microVU_Lower L33-155][low33]; [microVU_Misc.h L63-64][misc63]).
- **The interp deviates twice** (see §8):
  - `SYNCMSFLAGS` keeps only `old & 0xFC0`, so **every FMAC op clears I and D** ([VUops L3889-3893][opsSync]).
  - `SYNCFDIV` keeps only `old & 0x3CF`, so **IS and DS are not sticky**; they are overwritten with the current I and D ([L3905-3909][opsSyncFdiv]).

**CTC2 to vi16.** Writes to the low 6 bits (D, I, O, U, S, Z) are ignored, so only bits 6..11 change [VUM p.201].
- Macro mode has no FSSET; status is set with CTC2 instead [VUM p.209, where the text says CFC2, evidently a typo].
- mVU: `status = (status & 0x3F) | (rt & 0xFC0)`, after which the status is re-denormalized into the microVU flag instances ([microVU_Macro L489-511][macCTC2st]).
- Interp: no `REG_STATUS_FLAG` case, so the write falls through to `default:` and stores the full 32 bits ([VU0.cpp L163-207][vu0ctc2]). This is a deviation.
- Real use: Tekken Tag Tournament clears status with CTC2 and later reads the sticky bits with CFC2 ([iR5900Analysis.cpp L76-97][ana76]).

**CTC2 to vi17 (MAC).** MAC is read-only [VUM p.201: "r/-"]. Both PCSX2 paths ignore the write ([VU0.cpp L173-176][vu0ctc2]; [microVU_Macro L479-482][macCTC2]).

**CTC2 to vi18 (clip).** Clip is readable and writable and is 24 bits wide [VUM pp.201-202].
- Interp writes `VU0.clipflag` and `VI[18]` with the full value. mVU writes the 32-bit value to `VI[18]`.
- Recommendation: mask to 24 bits.

**CFC2** [VUM p.234]. CFC2 moves the 32-bit CCR value to the GPR, sign-extended. VI00..VI15 are 16-bit and effectively zero-extended.
- vi16, vi17 and vi18 have zeros above bit 11, 15 and 23 respectively, so sign extension never fires.
- Interp: [VU0.cpp L140-161][vu0cfc2]. The R register is masked to `0x7FFFFF`; everything else is the 32-bit value sign-extended into the upper word.
- mVU: `MOVSX` for index 16 and above, `MOVZX` of 16 bits for VI0..15, and R masked to `0x7FFFFF` ([microVU_Macro L404-458][macCFC2]).

**I/D in macro mode.** The manual defines VDIV, VSQRT and VRSQRT as identical to their micro-mode counterparts [VUM p.252 for VDIV], so the status rules above apply.
- Both PCSX2 paths update I and D in macro mode: interp `SYNCFDIV`, and mVU's `if (mVU.cop2)` branch.
- The EE analysis pass notes that these ops update status unconditionally ([iR5900Analysis.cpp L141-145][ana141]).

## 4. When flags become visible in macro mode

**Hardware.**
- The macro pipeline is `I Q R A T X Y Z S`. Flags and results are written in the S stage [VUM p.210, Fig 5-1].
- The control-register table marks status, MAC and clip as `r*/w*` when VU0 is operating, meaning a VNOP may be needed for timing [VUM p.201].
- The "Status flag read/write" figures [VUM p.215] show:
  - **COP2 op + 5×VNOP + CFC2 vi16** for the CFC2 to see that op's status (the CFC2 is dual-issued with the 5th VNOP and stalls one cycle);
  - **COP2 op + 2×VNOP + CTC2 vi16** for a status write.
- VNOPs do not change flags [VUM p.39].
- The CFC2 page [VUM p.234 "Remarks"] says CFC2 stalls until a preceding macro instruction that writes the same CCR or CPR register completes.
  Read together with p.215 and the GameDB patches below, that interlock evidently does not cover the implicit flag writes.
  A CFC2 too soon after a flag-setting op returns the **older** flag value.
- ps2tek describes the same pipeline effect for micro mode: flags change at the FMAC writeback stage, about 4 cycles later, and clip flags are delayed the same way.

**PCSX2 uses an immediate model in both macro paths.**
- Interp: `SYNCMSFLAGS`, `SYNCCLIPFLAG` and `SYNCFDIV` run inside each `V*()` wrapper ([VUops L3887-3909][opsSync]).
- mVU macro: MAC is written straight to `VI[REG_MAC_FLAG]`, because `fInstance >= 4` means macroVU ([microVU_Alloc L84-89][alloc84]). Status is normalized into `VI[16]` in `endMacroOp` ([microVU_Macro L77-105][macEnd]).
- By contrast, microVU **micro** mode models four flag instances; the source comment calls that "the correct behavior of the VU's" ([microVU_Misc.h L272-281][misc272]).
- The EE recompiler states that it emulates the R5900 as fully interlocked and notes "CFC2 flag register after arithmetic operation: 5 cycles" ([iR5900.cpp L1870-1883][r5900timing]).
  It only **warns** ("Possible old value used in COP2 code") when a CFC2 of a flag register follows a flag-writing COP2 op within a small window: fewer than 4 COP2 ops, fewer than 5 COP2 moves, and within 10 instructions ([L1918-1953][r5900warn]).
- Games that depend on stale flags are fixed with GameDB instruction reorders (§9). There is no gamefix flag for it.
- Flag-elision speed hack (`vuFlagHack`, on by default):
  - The COP2 analysis computes flags only for ops whose flags a later CFC2 consumes.
  - It forces all flags up to date before VCALLMS/VCALLMSR and before SB/SH/SW, since a store could trigger VIF0→VU0 ([iR5900Analysis.cpp L55-170][ana55]; [microVU_Macro L23-75][macSetup]).
  - A static recompiler can use the same liveness analysis.

**Recommendation.** Use PCSX2's immediate model: flags change at the end of each COP2 op. For this game GameDB lists no COP2 reorder patches; Road Trip SLUS-20398 and Road Trip Adventure SLES-51356 carry only `roundSprite` GS fixes ([GameIndex L66282-66286][gdbRT], [L17788-17793][gdbRTA]).
Optionally, run PCSX2's lookahead heuristic offline to list risky CFC2 sites.

## 5. Overflow, clamping, denormals, Inf/NaN

**Hardware** [VUM pp.26-29; ps2tek "IEEE 754 Differences"].
- The VU has no NaN, Inf or denormals. Exponent 255 is an **ordinary** value (scale 2^128) [VUM p.26 table].
- A value with exponent 0 is 0, whatever its mantissa. Denormal inputs therefore act as (signed) zero.
- Exceptions do not trap. The flag is set, the result is clamped, and execution continues [p.28]:
  - Exponent overflow gives ±MAX, sets O and OS, and leaves Z=0, U=0 [p.42].
  - Exponent underflow gives ±0 and sets U, Z, US and ZS, with S = sign [p.42].
  - Float→fixed conversion overflow gives ±MAX and sets no flags [p.28]. In practice this is 0x7FFFFFFF/0x80000000; FTOI sets no flags.
- Rounding is approximately truncation toward zero, and the LSB may differ from IEEE [p.27, p.29].
- Overflow and underflow are judged on the exponent only [p.29].

**Value of MAX (inferred).** The VU manual gives no hex value for "+MAX".
- Exponent 255 is a valid finite VU value [p.26], so the largest magnitude is 0x7FFFFFFF.
- The EE FPU uses 0x7FFFFFFF/0xFFFFFFFF for ±maximum ([EEISA pp.359-360, MADD.S `maxmin`]).
- So hardware ±MAX is very likely **0x7FFFFFFF/0xFFFFFFFF**, and real O fires only beyond that range.

**PCSX2** approximates this because the host has IEEE Inf and NaN.
- Operands go through `vuDouble()`. Exponent 0 becomes a signed zero; exponent 255 becomes `sign|0x7F7FFFFF` when VU0 overflow clamping is on (the default) ([VUops L440-459][opsDouble]; [Pcsx2Config L459][cfg459]).
- Results that overflow to host Inf are clamped to `sign|0x7F7FFFFF` and set O in the interp ([VUflags L37-42][vf37]).
- mVU clamps operands only on selected ops. Per the source comments these are Kingdom Hearts I (VU0) SUB, ICO (COP2) MADDw, and the TOTA/DoM/Superman cases ([microVU_Upper L596-638][upClampComments]). It never sets O without `VUOverflowHack` (Superman Returns; [Config.h L1526][cfg1526]).
- The default EE and VU MXCSR is DAZ + FTZ + round-toward-zero ("Chop") ([Pcsx2Config.cpp L31-41][cfgFPCR]). The host therefore never produces a denormal, so **in practice PCSX2 never sets U**: an underflow yields Z=1, U=0.
- **Net effect:** PCSX2 faithfully emulates only **Z and S** (plus O in the interp and the Superman hack). That is what shipping games observably depend on.

**For the recompiler.**
- Run SSE with DAZ, FTZ and RZ.
- Clamp ±Inf/NaN operands to ±0x7F7FFFFF, matching PCSX2.
- After each FMAC lane:
  - If `|r| > FLT_MAX` (Inf or NaN): `r = sign|0x7F7FFFFF`, set O, clear Z.
  - Else if the result was flushed to ±0 from a nonzero exact result (optional; needs an underflow check): set U and Z.
  - Else: Z = (r == 0), S = sign bit.

## 6. VCLIP (clip flag, vi18)

- **Operation** [VUM p.75 CLIP; p.251 VCLIP is "same as micro CLIP"].
  - First CF = CF << 6, keeping 24 bits. Then six flags are set from strict comparisons against |VF[ft].w|:
  - +x if x > +|w|; −x if x < −|w|; and the same for y and z.
  - The encoding is always dest = xyz with ft's w field.
- **Bit order** [VUM p.40 and the p.76 example; ps2tek]. Bits 5..0 hold the current judgment: `-z +z -y +y -x +x`.
  - So bit 0 is +x, 1 is −x, 2 is +y, 3 is −y, 4 is +z and 5 is −z.
  - Bits 11..6 hold the previous judgment, 17..12 the one before, and 23..18 the one before that.
  - The p.76 example (x > |w|, z < −|w|) yields current bits `100001`.
- **Flags.** CLIP changes only the clip flag; MAC and status are untouched [VUM p.41, p.75].
- **PCSX2.**
  - Interp ([VUops L909-925][opsCLIP]):
    - It takes `value = |w|` as raw bits. If w's exponent is 0, value is `0x007FFFFF`, so denormals compare like 0.
    - It sets bit 0 if `(s32)x_bits > value` and bit 1 if `(s32)(x_bits ^ 0x80000000) > value`; y and z work the same way.
    - It then masks `& 0xFFFFFF`, and `SYNCCLIPFLAG` copies the result to `VI[18]` ([L4005][opsWrap]).
    - Integer compares on the bit patterns handle exponent-255 values correctly without host Inf.
  - mVU: shift the old clip value by 6, mask to 24 bits, flush denormal fs to 0, compare against `|w|` with PCMPGTD, and pack ([microVU_Upper L533-575][upCLIP]).

## 7. VDIV / VSQRT / VRSQRT (Q register and I/D)

| Op | Condition | Q | Non-sticky I, D | Sticky |
|---|---|---|---|---|
| DIV fs/ft | ft == 0, fs == 0 | ±MAX, sign = sign(fs) XOR sign(ft) | I=1, D=0 | IS \|= 1 |
| DIV | ft == 0, fs != 0 | ±MAX, sign = sign(fs) XOR sign(ft) | I=0, D=1 | DS \|= 1 |
| DIV | otherwise | fs/ft (RZ; denormal → ±0; overflow → ±MAX) | I=0, D=0 | — |
| SQRT ft | ft < 0 | sqrt(\|ft\|) | I=1, **D=0** | IS \|= 1 |
| SQRT | otherwise | sqrt(ft) | I=0, **D=0** | — |
| RSQRT fs/sqrt(ft) | ft < 0 | fs / sqrt(\|ft\|) | I=1 (D=0) | IS \|= 1 |
| RSQRT | ft == 0, fs != 0 | ±MAX | D=1 | DS \|= 1 |
| RSQRT | ft == 0, fs == 0 | ±MAX (mVU) | I=1 (mVU) | IS \|= 1 |

Sources:
- Exception results and flags [VUM p.28]:
  - 0/0 gives ±MAX and sets I.
  - Square root of a negative x gives sqrt(|x|) and sets I.
  - Division by zero gives ±MAX and sets D, and D is not set for 0/0.
- I and D definitions [VUM p.40]:
  - I is set for 0/0 in DIV and for a negative argument in SQRT or RSQRT.
  - D is set for division by zero (except 0/0) in DIV or RSQRT, and cleared by SQRT.
  - The flag table is on p.41, and the DIV, RSQRT and SQRT pages are pp.129, 188 and 193.
- FDIV ops leave MAC and status Z/S/U/O untouched [p.41]. Denormal inputs are 0, so a denormal divisor counts as division by zero (interp `vuDouble`; mVU under host DAZ).
- PCSX2 interp: [VUops L931-1009][opsDIV], published by `SYNCFDIV`. mVU: [microVU_Lower L23-155][low23].

Discrepancies inside PCSX2:
- **RSQRT 0/0.** The interp sets **both** D and I and returns **±0** (sign = XOR). mVU sets I only and returns ±MAX with the sign of fs. mVU matches the manual (D "except 0/0"; 0/0 gives ±MAX with I).
- **−0 radicand in SQRT/RSQRT.** mVU tests the sign bit, so −0 sets I. The interp tests `ft < 0.0`, so −0 does not set I. The manual does not settle this.
- **±MAX.** Both paths use 0x7F7FFFFF; hardware is probably 0x7FFFFFFF (§5).

**Q timing.**
- Q is written after 7 cycles (DIV, SQRT) or 13 cycles (RSQRT).
- Q has no hazard check; use VWAITQ before reading it [VUM p.210, p.218].
- PCSX2 macro writes Q immediately: interp `SYNCFDIV`, mVU `endMacroOp`.
- Games that read Q too early are patched in GameDB (Everblue 2, Snoopy vs. the Red Baron "DIV Q timings"; §9).

## 8. Discrepancies: Sony manual vs PCSX2 interp vs mVU

| Behavior | Sony manual | PCSX2 interp (macro) | PCSX2 mVU macro | Follow |
|---|---|---|---|---|
| FMAC op effect on I/D (bits 4,5) | unchanged (p.41) | **cleared** (`SYNCMSFLAGS` keeps `&0xFC0`) | unchanged | manual/mVU |
| DIV/SQRT/RSQRT effect on IS/DS | sticky OR (p.40) | **overwritten** (`SYNCFDIV` keeps `&0x3CF`) | sticky OR | manual/mVU |
| CTC2 vi16 | bits 0..5 ignored (p.201) | **full write** | `&0xFC0`, low 6 bits kept | manual/mVU |
| OPMULA/OPMSUB MAC w bits | cleared (p.39) | **stale** | cleared | manual/mVU |
| RSQRT 0/0 | ±MAX, I (p.28, p.40) | ±0, I+D | ±MAX, I | manual/mVU |
| U flag | Z=1, U=1 on underflow (p.42) | dead under default DAZ/FTZ | never | manual (optional) |
| O flag | ±MAX, O=1 (p.42) | set on host Inf | only with VUOverflowHack | manual/interp |
| MADD multiply-stage sticky | ORed into sticky (pp.42-43) | no | no | optional |
| Flag visibility to CFC2 | delayed, about 4-5 slots (p.215) | immediate | immediate | PCSX2 (immediate) |
| ±MAX value | "MAX"; exponent 255 valid (p.26) | 0x7F7FFFFF | 0x7F7FFFFF | 0x7F7FFFFF is safe; 0x7FFFFFFF is hardware (inferred) |

## 9. PCSX2 game-fix notes about macro-mode flags

**GameDB instruction reorders** ([GameIndex.yaml][gdb]).
- These patches move a `cfc2 rX, vi17/vi18` **before** the flag-setting `vsub` or `vclip`. That proves hardware returned the **older** flag value.
- Decoded example: Kessen II writes `48438800` (`cfc2 v1, vi17`) and `4BE521AC` (`vsub.xyzw vf6, vf4, vf5`) to the swapped slots.

| Game (serial) | Patch note | Flag |
|---|---|---|
| Kessen II (SLUS-20275, SLES-50578/9/80, SLPM-65015, SLPM-65031, SLPM-67531) | MAC check just after a vsub, rearranged | MAC |
| Dynasty Warriors 3 (SLUS-20277) / Zhen San Guo Wu Shuang 2 (SLPM-65163) | MAC check after an irrelevant vsub, rearranged | MAC |
| The Mark of Kri (SCUS-97140) | vsub pair with `cfc2.i`; vclips without enough cycles before reading clip | MAC, clip |
| Forbidden Siren 2 (SCES-53851, SCPS-15106), Primal (SCUS-97142), Run Like Hell (SLES-51345, SLUS-20037) | COP2 op swapped so flags work without delays | MAC/clip |
| Ghost in the Shell: SAC (SLUS-21006, SLES-53020, SCPS-15064, SCKA-20027) | COP2 ops rearranged around a MAC-flag dependency | MAC |
| Ace Combat 04 (SLUS-20152) | COP2 ops rearranged; the comment refers to Ace Combat 5 | MAC |
| Sprint Cars (SLUS-21418, SLES-54463), Sprint Cars 2 (SLUS-21601) | MAC-flag dependency; fixes physics / bouncing vehicles | MAC |
| MX vs. ATV Unleashed / Untamed (SLUS-21104, SLES-53106, SLUS-21701, SLES-55050/1) | avoid clip flag response; fixes SPS | clip |
| Shadow Hearts (SLUS-20347), Initial D Special Stage (SLPM-65268) | bad clip-flag COP2 ordering | clip |
| Everblue 2 (SLUS-20598, SLES-51381), Snoopy vs. the Red Baron (SLUS-21380) | DIV Q timing | Q |

**Code-level notes.**
- **Superman Returns** relies on the overflow flag. The gamefix `VUOverflowHack` exists because PCSX2 cannot detect PS2-range overflow without soft floats ([Config.h L1112, L1526][cfg1112]; [microVU_Upper L77-94][upOvf]).
- **Tekken Tag Tournament** clears status with CTC2, runs COP2 ops, then reads the sticky bits with CFC2, so it needs accurate sticky flags ([iR5900Analysis.cpp L76-97][ana76]).
- **Crash Twinsanity and the Ratchet games** need VU0 synchronization, not flags. The relevant settings are `FullVU0SyncHack` and `VUSyncHack` ([iR5900Analysis.cpp L231][ana231]; [microVU_Macro L344-346, L544][macRatchet]).
- **Kingdom Hearts I, ICO, TOTA, DoM and Superman: Shadow of Apokolips** need COP2 operand clamping (values, not flags) ([microVU_Upper L596-638][upClampComments]).
- **`OPHFlagHack`** (Bleach Blade Battlers) and the `mvuFlag: 0` GameDB entries concern the microVU flag speedhack and VU micro programs. They are not specific to macro-mode semantics ([Config.h L1102][cfg1102]).

---

[ops35]: https://github.com/PCSX2/pcsx2/blob/9fffbdbd59b962d63a2259b150f419ad3773e7b4/pcsx2/VUops.cpp#L35
[opsGetDst]: https://github.com/PCSX2/pcsx2/blob/9fffbdbd59b962d63a2259b150f419ad3773e7b4/pcsx2/VUops.cpp#L519-L528
[opsBin]: https://github.com/PCSX2/pcsx2/blob/9fffbdbd59b962d63a2259b150f419ad3773e7b4/pcsx2/VUops.cpp#L530-L539
[opsTer]: https://github.com/PCSX2/pcsx2/blob/9fffbdbd59b962d63a2259b150f419ad3773e7b4/pcsx2/VUops.cpp#L682-L691
[opsMinMax]: https://github.com/PCSX2/pcsx2/blob/9fffbdbd59b962d63a2259b150f419ad3773e7b4/pcsx2/VUops.cpp#L795-L818
[opsUnary]: https://github.com/PCSX2/pcsx2/blob/9fffbdbd59b962d63a2259b150f419ad3773e7b4/pcsx2/VUops.cpp#L495-L505
[opsOPMULA]: https://github.com/PCSX2/pcsx2/blob/9fffbdbd59b962d63a2259b150f419ad3773e7b4/pcsx2/VUops.cpp#L841-L871
[opsOPMSUB]: https://github.com/PCSX2/pcsx2/blob/9fffbdbd59b962d63a2259b150f419ad3773e7b4/pcsx2/VUops.cpp#L849-L871
[opsMove]: https://github.com/PCSX2/pcsx2/blob/9fffbdbd59b962d63a2259b150f419ad3773e7b4/pcsx2/VUops.cpp#L1086-L1095
[opsMR32]: https://github.com/PCSX2/pcsx2/blob/9fffbdbd59b962d63a2259b150f419ad3773e7b4/pcsx2/VUops.cpp#L1118-L1129
[opsDouble]: https://github.com/PCSX2/pcsx2/blob/9fffbdbd59b962d63a2259b150f419ad3773e7b4/pcsx2/VUops.cpp#L440-L459
[opsCLIP]: https://github.com/PCSX2/pcsx2/blob/9fffbdbd59b962d63a2259b150f419ad3773e7b4/pcsx2/VUops.cpp#L909-L925
[opsDIV]: https://github.com/PCSX2/pcsx2/blob/9fffbdbd59b962d63a2259b150f419ad3773e7b4/pcsx2/VUops.cpp#L931-L1009
[opsSync]: https://github.com/PCSX2/pcsx2/blob/9fffbdbd59b962d63a2259b150f419ad3773e7b4/pcsx2/VUops.cpp#L3887-L3909
[opsSyncFdiv]: https://github.com/PCSX2/pcsx2/blob/9fffbdbd59b962d63a2259b150f419ad3773e7b4/pcsx2/VUops.cpp#L3905-L3909
[opsVSUB]: https://github.com/PCSX2/pcsx2/blob/9fffbdbd59b962d63a2259b150f419ad3773e7b4/pcsx2/VUops.cpp#L3926
[opsWrap]: https://github.com/PCSX2/pcsx2/blob/9fffbdbd59b962d63a2259b150f419ad3773e7b4/pcsx2/VUops.cpp#L3911-L4009
[vf15]: https://github.com/PCSX2/pcsx2/blob/9fffbdbd59b962d63a2259b150f419ad3773e7b4/pcsx2/VUflags.cpp#L15-L66
[vf26]: https://github.com/PCSX2/pcsx2/blob/9fffbdbd59b962d63a2259b150f419ad3773e7b4/pcsx2/VUflags.cpp#L26-L30
[vf34]: https://github.com/PCSX2/pcsx2/blob/9fffbdbd59b962d63a2259b150f419ad3773e7b4/pcsx2/VUflags.cpp#L34-L36
[vf37]: https://github.com/PCSX2/pcsx2/blob/9fffbdbd59b962d63a2259b150f419ad3773e7b4/pcsx2/VUflags.cpp#L37-L42
[vfClear]: https://github.com/PCSX2/pcsx2/blob/9fffbdbd59b962d63a2259b150f419ad3773e7b4/pcsx2/VUflags.cpp#L69-L97
[vu0]: https://github.com/PCSX2/pcsx2/blob/9fffbdbd59b962d63a2259b150f419ad3773e7b4/pcsx2/VU0.cpp
[vu0lqc2]: https://github.com/PCSX2/pcsx2/blob/9fffbdbd59b962d63a2259b150f419ad3773e7b4/pcsx2/VU0.cpp#L94-L103
[vu0qmtc2]: https://github.com/PCSX2/pcsx2/blob/9fffbdbd59b962d63a2259b150f419ad3773e7b4/pcsx2/VU0.cpp#L128-L138
[vu0cfc2]: https://github.com/PCSX2/pcsx2/blob/9fffbdbd59b962d63a2259b150f419ad3773e7b4/pcsx2/VU0.cpp#L140-L161
[vu0ctc2]: https://github.com/PCSX2/pcsx2/blob/9fffbdbd59b962d63a2259b150f419ad3773e7b4/pcsx2/VU0.cpp#L163-L207
[ir209]: https://github.com/PCSX2/pcsx2/blob/9fffbdbd59b962d63a2259b150f419ad3773e7b4/pcsx2/x86/microVU_IR.h#L209
[upFlags]: https://github.com/PCSX2/pcsx2/blob/9fffbdbd59b962d63a2259b150f419ad3773e7b4/pcsx2/x86/microVU_Upper.inl#L10-L117
[upOvf]: https://github.com/PCSX2/pcsx2/blob/9fffbdbd59b962d63a2259b150f419ad3773e7b4/pcsx2/x86/microVU_Upper.inl#L77-L94
[upSafeSub]: https://github.com/PCSX2/pcsx2/blob/9fffbdbd59b962d63a2259b150f419ad3773e7b4/pcsx2/x86/microVU_Upper.inl#L173-L189
[upFMACa]: https://github.com/PCSX2/pcsx2/blob/9fffbdbd59b962d63a2259b150f419ad3773e7b4/pcsx2/x86/microVU_Upper.inl#L231-L283
[upCLIP]: https://github.com/PCSX2/pcsx2/blob/9fffbdbd59b962d63a2259b150f419ad3773e7b4/pcsx2/x86/microVU_Upper.inl#L533-L575
[upClampComments]: https://github.com/PCSX2/pcsx2/blob/9fffbdbd59b962d63a2259b150f419ad3773e7b4/pcsx2/x86/microVU_Upper.inl#L596-L638
[low23]: https://github.com/PCSX2/pcsx2/blob/9fffbdbd59b962d63a2259b150f419ad3773e7b4/pcsx2/x86/microVU_Lower.inl#L23-L155
[low33]: https://github.com/PCSX2/pcsx2/blob/9fffbdbd59b962d63a2259b150f419ad3773e7b4/pcsx2/x86/microVU_Lower.inl#L33-L155
[alloc47]: https://github.com/PCSX2/pcsx2/blob/9fffbdbd59b962d63a2259b150f419ad3773e7b4/pcsx2/x86/microVU_Alloc.inl#L47-L77
[alloc84]: https://github.com/PCSX2/pcsx2/blob/9fffbdbd59b962d63a2259b150f419ad3773e7b4/pcsx2/x86/microVU_Alloc.inl#L84-L89
[misc63]: https://github.com/PCSX2/pcsx2/blob/9fffbdbd59b962d63a2259b150f419ad3773e7b4/pcsx2/x86/microVU_Misc.h#L63-L64
[misc272]: https://github.com/PCSX2/pcsx2/blob/9fffbdbd59b962d63a2259b150f419ad3773e7b4/pcsx2/x86/microVU_Misc.h#L272-L281
[macSetup]: https://github.com/PCSX2/pcsx2/blob/9fffbdbd59b962d63a2259b150f419ad3773e7b4/pcsx2/x86/microVU_Macro.inl#L23-L75
[macEnd]: https://github.com/PCSX2/pcsx2/blob/9fffbdbd59b962d63a2259b150f419ad3773e7b4/pcsx2/x86/microVU_Macro.inl#L77-L105
[macTable]: https://github.com/PCSX2/pcsx2/blob/9fffbdbd59b962d63a2259b150f419ad3773e7b4/pcsx2/x86/microVU_Macro.inl#L164-L282
[macRatchet]: https://github.com/PCSX2/pcsx2/blob/9fffbdbd59b962d63a2259b150f419ad3773e7b4/pcsx2/x86/microVU_Macro.inl#L344-L346
[macCFC2]: https://github.com/PCSX2/pcsx2/blob/9fffbdbd59b962d63a2259b150f419ad3773e7b4/pcsx2/x86/microVU_Macro.inl#L404-L458
[macCTC2]: https://github.com/PCSX2/pcsx2/blob/9fffbdbd59b962d63a2259b150f419ad3773e7b4/pcsx2/x86/microVU_Macro.inl#L460-L482
[macCTC2st]: https://github.com/PCSX2/pcsx2/blob/9fffbdbd59b962d63a2259b150f419ad3773e7b4/pcsx2/x86/microVU_Macro.inl#L489-L511
[macQMTC2]: https://github.com/PCSX2/pcsx2/blob/9fffbdbd59b962d63a2259b150f419ad3773e7b4/pcsx2/x86/microVU_Macro.inl#L686-L692
[macLQC2]: https://github.com/PCSX2/pcsx2/blob/9fffbdbd59b962d63a2259b150f419ad3773e7b4/pcsx2/x86/microVU_Macro.inl#L836-L839
[r5900flags]: https://github.com/PCSX2/pcsx2/blob/9fffbdbd59b962d63a2259b150f419ad3773e7b4/pcsx2/x86/ix86-32/iR5900.cpp#L1418-L1475
[r5900timing]: https://github.com/PCSX2/pcsx2/blob/9fffbdbd59b962d63a2259b150f419ad3773e7b4/pcsx2/x86/ix86-32/iR5900.cpp#L1870-L1883
[r5900warn]: https://github.com/PCSX2/pcsx2/blob/9fffbdbd59b962d63a2259b150f419ad3773e7b4/pcsx2/x86/ix86-32/iR5900.cpp#L1918-L1953
[ana55]: https://github.com/PCSX2/pcsx2/blob/9fffbdbd59b962d63a2259b150f419ad3773e7b4/pcsx2/x86/iR5900Analysis.cpp#L55-L170
[ana76]: https://github.com/PCSX2/pcsx2/blob/9fffbdbd59b962d63a2259b150f419ad3773e7b4/pcsx2/x86/iR5900Analysis.cpp#L76-L97
[ana141]: https://github.com/PCSX2/pcsx2/blob/9fffbdbd59b962d63a2259b150f419ad3773e7b4/pcsx2/x86/iR5900Analysis.cpp#L141-L145
[ana231]: https://github.com/PCSX2/pcsx2/blob/9fffbdbd59b962d63a2259b150f419ad3773e7b4/pcsx2/x86/iR5900Analysis.cpp#L229-L232
[cfg1102]: https://github.com/PCSX2/pcsx2/blob/9fffbdbd59b962d63a2259b150f419ad3773e7b4/pcsx2/Config.h#L1102
[cfg1112]: https://github.com/PCSX2/pcsx2/blob/9fffbdbd59b962d63a2259b150f419ad3773e7b4/pcsx2/Config.h#L1111-L1115
[cfg1526]: https://github.com/PCSX2/pcsx2/blob/9fffbdbd59b962d63a2259b150f419ad3773e7b4/pcsx2/Config.h#L1526
[cfg459]: https://github.com/PCSX2/pcsx2/blob/9fffbdbd59b962d63a2259b150f419ad3773e7b4/pcsx2/Pcsx2Config.cpp#L459
[cfgFPCR]: https://github.com/PCSX2/pcsx2/blob/9fffbdbd59b962d63a2259b150f419ad3773e7b4/pcsx2/Pcsx2Config.cpp#L31-L41
[gdb]: https://github.com/PCSX2/pcsx2/blob/9fffbdbd59b962d63a2259b150f419ad3773e7b4/bin/resources/GameIndex.yaml
[gdbRT]: https://github.com/PCSX2/pcsx2/blob/9fffbdbd59b962d63a2259b150f419ad3773e7b4/bin/resources/GameIndex.yaml#L66282-L66286
[gdbRTA]: https://github.com/PCSX2/pcsx2/blob/9fffbdbd59b962d63a2259b150f419ad3773e7b4/bin/resources/GameIndex.yaml#L17788-L17793
