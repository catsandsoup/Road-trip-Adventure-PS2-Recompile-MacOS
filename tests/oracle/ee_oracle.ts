/**
 * Test-only R5900 (EE core + COP1 + COP2 macro-mode VU0 + MMI subset) interpreter.
 *
 * Purpose: differential oracle for PS2Recomp's recompiled EE code. It is NOT part of
 * any shipping build and is written independently of ps2xRecomp's translator.
 *
 * Structure follows rtao/rtao/test-support/palScalarMachine.ts (RTAO's bounded PAL
 * executor) but with full entry-state loading (128-bit GPRs, HI/LO/HI1/LO1/SA, FPRs,
 * ACC, FCR31, VU0 VF/VI/ACC/Q), no code whitelist, address translation and a much
 * larger opcode set.
 *
 * Semantics sources (primary):
 *  - PCSX2 interpreter: pcsx2/R5900OpcodeImpl.cpp (GPR writes touch only UD[0] for
 *    non-MMI ops; LWL/LWR/LDL/LDR/SWL/SWR/SDL/SDR mask tables; DIV by zero results),
 *    pcsx2/MMI.cpp (PEXTLW/PEXTUW/PCPYLD/PCPYUD/... lane orders), pcsx2/FPU.cpp
 *    (fpuDouble input clamp, posFmax on overflow and /0, CVT.W.S saturation,
 *    SQRT.S operand in ft, MADD/MSUB/ACC ops), pcsx2/VUops.cpp (macro-mode VU
 *    upper/lower op table, OPMULA/OPMSUB, MR32, DIV/SQRT/RSQRT, FTOI/ITOF).
 *  - Sony "EE Core Instruction Set Manual" / "VU User's Manual" opcode field layout.
 * Floating point: PS2 has no denormals/Inf/NaN; inputs with exponent 0 are zero,
 * exponent 255 are clamped to +-FLT_MAX, results are flushed/clamped. Rounding is
 * selectable: 'rz' (round toward zero, the EE/VU default) or 'rn' (host IEEE).
 */

export type Rounding = 'rz' | 'rn';

export interface OracleOptions {
  fpuRounding?: Rounding;
  vuRounding?: Rounding;
}

const M32 = 0xffffffffn;
const M64 = (1n << 64n) - 1n;
const M128 = (1n << 128n) - 1n;
const HI64 = M128 ^ M64;
export const MAXF_BITS = 0x7f7fffff;

const f32 = new Float32Array(1);
const u32 = new Uint32Array(f32.buffer);
export function bitsToF(b: number): number { u32[0] = b >>> 0; return f32[0]!; }
export function fToBits(f: number): number { f32[0] = f; return u32[0]!; }
const MAXF = bitsToF(MAXF_BITS);

/** PS2 operand read: denormal -> signed zero, exponent 255 -> +-FLT_MAX (PCSX2 fpuDouble/vuDouble). */
export function ps2In(bits: number): number {
  const e = (bits >>> 23) & 0xff;
  const neg = (bits >>> 31) !== 0;
  if (e === 0) return neg ? -0 : 0;
  if (e === 255) return neg ? -MAXF : MAXF;
  return bitsToF(bits);
}

/** Round a double result to PS2 float32 bits with clamp/flush. */
export function ps2Out(d: number, mode: Rounding): number {
  if (Number.isNaN(d)) return MAXF_BITS; // cannot happen with clamped inputs except inf-inf
  let f = Math.fround(d);
  if (mode === 'rz' && f !== d && Math.abs(f) > Math.abs(d)) {
    if (!Number.isFinite(f)) f = d < 0 ? -MAXF : MAXF;
    else {
      const b = fToBits(f);
      f = bitsToF(b - 1); // magnitude one ulp toward zero (sign-magnitude encoding)
    }
  }
  if (!Number.isFinite(f) || Math.abs(f) > MAXF) return (d < 0 ? 0x80000000 : 0) | MAXF_BITS;
  let bits = fToBits(f);
  if (((bits >>> 23) & 0xff) === 0) bits &= 0x80000000; // flush denormal to signed zero
  return bits >>> 0;
}

export interface CallRecord {
  index: number;
  target: number;
  ra: number;
  sp: number;
  depth: number;
  stub?: string;
  args: number[]; // a0..a3, t0..t3 lane0
  fargs: number[]; // f12..f15 bits
  steps: number;
  retSteps?: number;
  v0?: number;
  f0?: number;
}

export type Hook = (m: EeOracle) => boolean | void; // return true => instruction at pc skipped, simulate `jr ra`

export class EeOracle {
  readonly mem = new Uint8Array(0x2000000);
  readonly view = new DataView(this.mem.buffer);
  readonly spr = new Uint8Array(0x4000);
  readonly sprView = new DataView(this.spr.buffer);
  sprTouched = false;

  readonly gpr: bigint[] = Array<bigint>(32).fill(0n); // unsigned 128-bit
  hi = 0n; lo = 0n; // 128-bit: low 64 = HI/LO, high 64 = HI1/LO1
  sa = 0;
  readonly fpr = new Uint32Array(32);
  facc = 0;
  fcr31 = 0;
  readonly vf = Array.from({ length: 32 }, () => new Uint32Array(4));
  readonly vi = new Uint32Array(16);
  readonly vacc = new Uint32Array(4);
  q = 0x3f800000; p = 0; iReg = 0; rReg = 0;
  vstatus = 0; vmac = 0; vclip = 0;

  pc = 0; npc = 4;
  steps = 0;
  readonly fpuRounding: Rounding;
  readonly vuRounding: Rounding;

  readonly unsupported = new Map<string, number>();
  readonly events: string[] = [];
  readonly hooks = new Map<number, Hook>();
  readonly observers = new Map<number, Hook[]>();
  readonly calls: CallRecord[] = [];
  private readonly callStack: CallRecord[] = [];
  stubNames = new Map<number, string>();
  onCall?: (rec: CallRecord, m: EeOracle) => void;
  onReturn?: (rec: CallRecord, m: EeOracle) => void;
  traceFn?: (pc: number, word: number, m: EeOracle) => void;
  writeLog?: (addr: number, size: number, pc: number) => void;

  constructor(opts: OracleOptions = {}) {
    this.fpuRounding = opts.fpuRounding ?? 'rz';
    this.vuRounding = opts.vuRounding ?? 'rz';
    this.vf[0]![3] = 0x3f800000;
  }

  // ---------------- memory ----------------
  private tr(addr: number, size: number, write: boolean): [DataView, number] {
    let a = addr >>> 0;
    if (a >= 0x70000000 && a < 0x70004000) {
      if (a + size > 0x70004000) this.fault(`scratchpad overrun ${a.toString(16)}`);
      this.sprTouched = true;
      return [this.sprView, a - 0x70000000];
    }
    if (a >= 0x80000000 && a < 0xc0000000) a &= 0x1fffffff;
    else if (a >= 0x20000000 && a < 0x40000000) a &= 0x0fffffff;
    if (a + size <= 0x2000000) {
      if (write && this.writeLog) this.writeLog(a, size, this.curPc);
      return [this.view, a];
    }
    this.fault(`unmapped ${write ? 'write' : 'read'} ${size}B at ${(addr >>> 0).toString(16)}`);
  }
  private fault(msg: string): never {
    const m = `pc=${this.curPc.toString(16)}: ${msg}`;
    this.events.push(m);
    throw new Error(m);
  }
  r8(a: number): number { const [v, o] = this.tr(a, 1, false); return v.getUint8(o); }
  r16(a: number): number { if (a & 1) this.fault('unaligned lh'); const [v, o] = this.tr(a, 2, false); return v.getUint16(o, true); }
  r32(a: number): number { if (a & 3) this.fault('unaligned lw'); const [v, o] = this.tr(a, 4, false); return v.getUint32(o, true); }
  r64(a: number): bigint { if (a & 7) this.fault('unaligned ld'); const [v, o] = this.tr(a, 8, false); return v.getBigUint64(o, true); }
  r128(a: number): bigint { const b = (a & ~15) >>> 0; const [v, o] = this.tr(b, 16, false); return v.getBigUint64(o, true) | (v.getBigUint64(o + 8, true) << 64n); }
  w8(a: number, x: number): void { const [v, o] = this.tr(a, 1, true); v.setUint8(o, x & 0xff); }
  w16(a: number, x: number): void { if (a & 1) this.fault('unaligned sh'); const [v, o] = this.tr(a, 2, true); v.setUint16(o, x & 0xffff, true); }
  w32(a: number, x: number): void { if (a & 3) this.fault('unaligned sw'); const [v, o] = this.tr(a, 4, true); v.setUint32(o, x >>> 0, true); }
  w64(a: number, x: bigint): void { if (a & 7) this.fault('unaligned sd'); const [v, o] = this.tr(a, 8, true); v.setBigUint64(o, x & M64, true); }
  w128(a: number, x: bigint): void { const b = (a & ~15) >>> 0; const [v, o] = this.tr(b, 16, true); v.setBigUint64(o, x & M64, true); v.setBigUint64(o + 8, (x >> 64n) & M64, true); }

  // ---------------- register helpers ----------------
  u32(r: number): number { return Number(this.gpr[r]! & M32) >>> 0; }
  s32(r: number): number { return this.u32(r) | 0; }
  u64(r: number): bigint { return this.gpr[r]! & M64; }
  s64(r: number): bigint { return BigInt.asIntN(64, this.gpr[r]!); }
  /** Write low 64 bits (upper 64 preserved, as in PCSX2's UD[0] writes). */
  set64(r: number, v: bigint): void { if (r) this.gpr[r] = (this.gpr[r]! & HI64) | (BigInt.asUintN(64, v)); }
  /** Write sign-extended 32-bit result to low 64. */
  setS32(r: number, v: number): void { this.set64(r, BigInt(v | 0)); }
  set128(r: number, v: bigint): void { if (r) this.gpr[r] = BigInt.asUintN(128, v); }
  lane(r: number, i: number): number { return Number((this.gpr[r]! >> BigInt(i * 32)) & M32) >>> 0; }
  static pack32(l: readonly number[]): bigint { let v = 0n; for (let i = 0; i < 4; i++) v |= BigInt(l[i]! >>> 0) << BigInt(32 * i); return v; }
  lanes(r: number): number[] { return [0, 1, 2, 3].map((i) => this.lane(r, i)); }
  halves(r: number): number[] { const out: number[] = []; for (let i = 0; i < 8; i++) out.push(Number((this.gpr[r]! >> BigInt(16 * i)) & 0xffffn)); return out; }
  static packH(h: readonly number[]): bigint { let v = 0n; for (let i = 0; i < 8; i++) v |= BigInt(h[i]! & 0xffff) << BigInt(16 * i); return v; }

  fprF(i: number): number { return ps2In(this.fpr[i]!); }
  vfF(r: number, l: number): number { return ps2In(this.vf[r]![l]!); }

  private curPc = 0;
  private cond(): boolean { return (this.fcr31 & 0x800000) !== 0; }
  private setCond(c: boolean): void { this.fcr31 = c ? (this.fcr31 | 0x800000) : (this.fcr31 & ~0x800000); this.fcr31 >>>= 0; }

  private unsup(word: number, what: string): never {
    const key = `${what} word=${word.toString(16).padStart(8, '0')} @${this.curPc.toString(16)}`;
    this.unsupported.set(key, (this.unsupported.get(key) ?? 0) + 1);
    this.fault(`unsupported ${key}`);
  }

  // ---------------- run ----------------
  run(stopPc: number, stopSp: number | undefined, maxSteps = 5_000_000): void {
    for (; this.steps < maxSteps;) {
      const pc = this.pc;
      if (pc === stopPc && (stopSp === undefined || this.u32(29) === stopSp)) return;
      this.curPc = pc;
      // returns from tracked calls
      const top = this.callStack[this.callStack.length - 1];
      if (top && pc === top.ra && this.u32(29) === top.sp) {
        this.callStack.pop();
        top.retSteps = this.steps; top.v0 = this.u32(2); top.f0 = this.fpr[0]!;
        this.onReturn?.(top, this);
      }
      // a pending call becomes active when we reach its target (after its delay slot)
      if (this.pendingCall && pc === this.pendingCall.target) {
        const rec = this.pendingCall; this.pendingCall = undefined;
        rec.sp = this.u32(29);
        rec.args = [4, 5, 6, 7, 8, 9, 10, 11].map((r) => this.u32(r));
        rec.fargs = [12, 13, 14, 15].map((r) => this.fpr[r]!);
        rec.steps = this.steps;
        this.callStack.push(rec);
        this.onCall?.(rec, this);
      }
      const obs = this.observers.get(pc);
      if (obs) for (const o of obs) o(this);
      const hook = this.hooks.get(pc);
      if (hook && hook(this) === true) {
        // simulate return: pc = ra
        this.pc = this.u32(31); this.npc = this.pc + 4;
        continue;
      }
      const word = this.r32(pc);
      this.traceFn?.(pc, word, this);
      this.pc = this.npc; this.npc = (this.npc + 4) >>> 0;
      this.exec(word, pc);
      this.gpr[0] = 0n;
      this.steps++;
    }
    throw new Error(`step budget ${maxSteps} exhausted at pc=${this.pc.toString(16)}`);
  }

  private branch(pc: number, take: boolean, likely: boolean, word: number): void {
    const simm = (word << 16) >> 16;
    if (take) this.npc = (pc + 4 + simm * 4) >>> 0;
    else if (likely) { this.pc = (pc + 8) >>> 0; this.npc = (pc + 12) >>> 0; }
  }

  private noteCall(target: number, ra: number): void {
    const rec: CallRecord = {
      index: this.calls.length, target, ra, sp: this.u32(29), depth: this.callStack.length,
      stub: this.stubNames.get(target),
      args: [4, 5, 6, 7, 8, 9, 10, 11].map((r) => this.u32(r)),
      fargs: [12, 13, 14, 15].map((r) => this.fpr[r]!), steps: this.steps,
    };
    // the call's sp is taken at entry (after delay slot), fixed up in the observer below
    this.calls.push(rec);
    this.pendingCall = rec;
  }
  private pendingCall: CallRecord | undefined;

  private exec(word: number, pc: number): void {
    const op = word >>> 26;
    const rs = (word >>> 21) & 31, rt = (word >>> 16) & 31, rd = (word >>> 11) & 31;
    const sh = (word >>> 6) & 31, fn = word & 63, imm = word & 0xffff, simm = (imm << 16) >> 16;
    switch (op) {
      case 0: return this.special(word, pc, rs, rt, rd, sh, fn);
      case 1: { // REGIMM
        const v = this.s64(rs);
        switch (rt) {
          case 0: return this.branch(pc, v < 0n, false, word);
          case 1: return this.branch(pc, v >= 0n, false, word);
          case 2: return this.branch(pc, v < 0n, true, word);
          case 3: return this.branch(pc, v >= 0n, true, word);
          case 16: this.set64(31, BigInt(pc + 8)); return this.branch(pc, v < 0n, false, word);
          case 17: this.set64(31, BigInt(pc + 8)); return this.branch(pc, v >= 0n, false, word);
          case 18: this.set64(31, BigInt(pc + 8)); return this.branch(pc, v < 0n, true, word);
          case 19: this.set64(31, BigInt(pc + 8)); return this.branch(pc, v >= 0n, true, word);
          case 24: this.sa = (this.u32(rs) ^ simm) & 15; return; // MTSAB
          case 25: this.sa = ((this.u32(rs) ^ simm) & 7) * 2; return; // MTSAH
          default: return this.unsup(word, 'regimm');
        }
      }
      case 2: case 3: {
        const target = (((pc + 4) & 0xf0000000) | ((word & 0x3ffffff) << 2)) >>> 0;
        if (op === 3) { this.set64(31, BigInt(pc + 8)); this.noteCall(target, pc + 8); }
        this.npc = target; return;
      }
      case 4: return this.branch(pc, this.u64(rs) === this.u64(rt), false, word);
      case 5: return this.branch(pc, this.u64(rs) !== this.u64(rt), false, word);
      case 6: return this.branch(pc, this.s64(rs) <= 0n, false, word);
      case 7: return this.branch(pc, this.s64(rs) > 0n, false, word);
      case 20: return this.branch(pc, this.u64(rs) === this.u64(rt), true, word);
      case 21: return this.branch(pc, this.u64(rs) !== this.u64(rt), true, word);
      case 22: return this.branch(pc, this.s64(rs) <= 0n, true, word);
      case 23: return this.branch(pc, this.s64(rs) > 0n, true, word);
      case 8: case 9: return this.setS32(rt, (this.u32(rs) + simm) | 0); // ADDI (no overflow trap modelled) / ADDIU
      case 10: return this.set64(rt, this.s64(rs) < BigInt(simm) ? 1n : 0n);
      case 11: return this.set64(rt, this.u64(rs) < BigInt.asUintN(64, BigInt(simm)) ? 1n : 0n);
      case 12: return this.set64(rt, this.u64(rs) & BigInt(imm));
      case 13: return this.set64(rt, this.u64(rs) | BigInt(imm));
      case 14: return this.set64(rt, this.u64(rs) ^ BigInt(imm));
      case 15: return this.setS32(rt, imm << 16);
      case 16: return this.cop0(word, rs, rt, rd);
      case 17: return this.cop1(word, pc, rs, rt, rd, sh, fn);
      case 18: return this.cop2(word, pc, rs, rt, rd);
      case 24: case 25: return this.set64(rt, this.s64(rs) + BigInt(simm)); // DADDI/DADDIU
      case 26: { // LDL
        const a = (this.u32(rs) + simm) >>> 0, k = a & 7, mem = this.r64(a & ~7);
        const shift = BigInt(56 - 8 * k), mask = k === 7 ? 0n : (M64 >> BigInt(8 * (k + 1)));
        return this.set64(rt, (this.u64(rt) & mask) | ((mem << shift) & M64));
      }
      case 27: { // LDR
        const a = (this.u32(rs) + simm) >>> 0, k = a & 7, mem = this.r64(a & ~7);
        const shift = BigInt(8 * k), mask = k === 0 ? 0n : (M64 ^ (M64 >> BigInt(8 * k)));
        return this.set64(rt, (this.u64(rt) & mask) | (mem >> shift));
      }
      case 28: return this.mmi(word, rs, rt, rd, sh, fn);
      case 30: return this.set128(rt, this.r128((this.u32(rs) + simm) >>> 0)); // LQ
      case 31: return this.w128((this.u32(rs) + simm) >>> 0, this.gpr[rt]!); // SQ
      case 32: return this.set64(rt, BigInt((this.r8((this.u32(rs) + simm) >>> 0) << 24) >> 24));
      case 33: return this.set64(rt, BigInt((this.r16((this.u32(rs) + simm) >>> 0) << 16) >> 16));
      case 34: { // LWL
        const a = (this.u32(rs) + simm) >>> 0, k = a & 3, mem = this.r32(a & ~3);
        const masks = [0x00ffffff, 0x0000ffff, 0x000000ff, 0x00000000], shifts = [24, 16, 8, 0];
        return this.setS32(rt, (this.u32(rt) & masks[k]!) | (mem << shifts[k]!));
      }
      case 35: return this.setS32(rt, this.r32((this.u32(rs) + simm) >>> 0));
      case 36: return this.set64(rt, BigInt(this.r8((this.u32(rs) + simm) >>> 0)));
      case 37: return this.set64(rt, BigInt(this.r16((this.u32(rs) + simm) >>> 0)));
      case 38: { // LWR
        const a = (this.u32(rs) + simm) >>> 0, k = a & 3, mem = this.r32(a & ~3);
        const masks = [0x00000000, 0xff000000, 0xffff0000, 0xffffff00], shifts = [0, 8, 16, 24];
        const v = ((this.u32(rt) & masks[k]!) | (mem >>> shifts[k]!)) >>> 0;
        if (k === 0) return this.setS32(rt, v);
        if (rt) this.gpr[rt] = (this.gpr[rt]! & ~M32 & M128) | BigInt(v);
        return;
      }
      case 39: return this.set64(rt, BigInt(this.r32((this.u32(rs) + simm) >>> 0))); // LWU
      case 40: return this.w8((this.u32(rs) + simm) >>> 0, this.u32(rt));
      case 41: return this.w16((this.u32(rs) + simm) >>> 0, this.u32(rt));
      case 42: { // SWL
        const a = (this.u32(rs) + simm) >>> 0, k = a & 3, mem = this.r32(a & ~3);
        const masks = [0xffffff00, 0xffff0000, 0xff000000, 0x00000000], shifts = [24, 16, 8, 0];
        return this.w32(a & ~3, (this.u32(rt) >>> shifts[k]!) | (mem & masks[k]!));
      }
      case 43: return this.w32((this.u32(rs) + simm) >>> 0, this.u32(rt));
      case 44: { // SDL
        const a = (this.u32(rs) + simm) >>> 0, k = a & 7, mem = this.r64(a & ~7);
        const shift = BigInt(56 - 8 * k), mask = k === 7 ? 0n : (M64 ^ (M64 >> BigInt(56 - 8 * k)));
        return this.w64(a & ~7, (this.u64(rt) >> shift) | (mem & mask));
      }
      case 45: { // SDR
        const a = (this.u32(rs) + simm) >>> 0, k = a & 7, mem = this.r64(a & ~7);
        const shift = BigInt(8 * k), mask = k === 0 ? 0n : (M64 >> BigInt(64 - 8 * k));
        return this.w64(a & ~7, ((this.u64(rt) << shift) & M64) | (mem & mask));
      }
      case 46: { // SWR
        const a = (this.u32(rs) + simm) >>> 0, k = a & 3, mem = this.r32(a & ~3);
        const masks = [0x00000000, 0x000000ff, 0x0000ffff, 0x00ffffff], shifts = [0, 8, 16, 24];
        return this.w32(a & ~3, ((this.u32(rt) << shifts[k]!) | (mem & masks[k]!)) >>> 0);
      }
      case 47: return; // CACHE
      case 49: this.fpr[rt] = this.r32((this.u32(rs) + simm) >>> 0); return; // LWC1
      case 51: return; // PREF
      case 54: { // LQC2
        const v = this.r128((this.u32(rs) + simm) >>> 0);
        if (rt) for (let i = 0; i < 4; i++) this.vf[rt]![i] = Number((v >> BigInt(32 * i)) & M32);
        return;
      }
      case 55: return this.set64(rt, this.r64((this.u32(rs) + simm) >>> 0)); // LD
      case 57: return this.w32((this.u32(rs) + simm) >>> 0, this.fpr[rt]!); // SWC1
      case 62: return this.w128((this.u32(rs) + simm) >>> 0, EeOracle.pack32(Array.from(this.vf[rt]!))); // SQC2
      case 63: return this.w64((this.u32(rs) + simm) >>> 0, this.u64(rt)); // SD
      default: return this.unsup(word, `op${op}`);
    }
  }

  private special(word: number, pc: number, rs: number, rt: number, rd: number, sh: number, fn: number): void {
    switch (fn) {
      case 0: return this.setS32(rd, this.u32(rt) << sh);
      case 2: return this.setS32(rd, this.u32(rt) >>> sh);
      case 3: return this.setS32(rd, this.s32(rt) >> sh);
      case 4: return this.setS32(rd, this.u32(rt) << (this.u32(rs) & 31));
      case 6: return this.setS32(rd, this.u32(rt) >>> (this.u32(rs) & 31));
      case 7: return this.setS32(rd, this.s32(rt) >> (this.u32(rs) & 31));
      case 8: this.npc = this.u32(rs); return; // JR
      case 9: { const t = this.u32(rs); this.set64(rd, BigInt(pc + 8)); this.noteCall(t, pc + 8); this.npc = t; return; }
      case 10: if (this.u64(rt) === 0n) this.set64(rd, this.u64(rs)); return; // MOVZ
      case 11: if (this.u64(rt) !== 0n) this.set64(rd, this.u64(rs)); return; // MOVN
      case 12: return this.unsup(word, 'syscall');
      case 13: return this.unsup(word, 'break');
      case 15: return; // SYNC
      case 16: return this.set64(rd, this.hi & M64);
      case 17: this.hi = (this.hi & HI64) | this.u64(rs); return;
      case 18: return this.set64(rd, this.lo & M64);
      case 19: this.lo = (this.lo & HI64) | this.u64(rs); return;
      case 20: return this.set64(rd, this.u64(rt) << BigInt(this.u32(rs) & 63));
      case 22: return this.set64(rd, this.u64(rt) >> BigInt(this.u32(rs) & 63));
      case 23: return this.set64(rd, this.s64(rt) >> BigInt(this.u32(rs) & 63));
      case 24: { // MULT (R5900: also writes rd)
        const p = BigInt(this.s32(rs)) * BigInt(this.s32(rt));
        const l = BigInt.asUintN(64, BigInt.asIntN(32, p)), h = BigInt.asUintN(64, BigInt.asIntN(32, p >> 32n));
        this.lo = (this.lo & HI64) | l; this.hi = (this.hi & HI64) | h; this.set64(rd, l); return;
      }
      case 25: { // MULTU
        const p = BigInt(this.u32(rs)) * BigInt(this.u32(rt));
        const l = BigInt.asUintN(64, BigInt.asIntN(32, p & M32)), h = BigInt.asUintN(64, BigInt.asIntN(32, (p >> 32n) & M32));
        this.lo = (this.lo & HI64) | l; this.hi = (this.hi & HI64) | h; this.set64(rd, l); return;
      }
      case 26: { const [q, r] = div32(this.s32(rs), this.s32(rt)); this.lo = (this.lo & HI64) | BigInt.asUintN(64, BigInt(q)); this.hi = (this.hi & HI64) | BigInt.asUintN(64, BigInt(r)); return; }
      case 27: { const [q, r] = divu32(this.u32(rs), this.u32(rt)); this.lo = (this.lo & HI64) | BigInt.asUintN(64, BigInt(q | 0)); this.hi = (this.hi & HI64) | BigInt.asUintN(64, BigInt(r | 0)); return; }
      case 32: case 33: return this.setS32(rd, (this.u32(rs) + this.u32(rt)) | 0);
      case 34: case 35: return this.setS32(rd, (this.u32(rs) - this.u32(rt)) | 0);
      case 36: return this.set64(rd, this.u64(rs) & this.u64(rt));
      case 37: return this.set64(rd, this.u64(rs) | this.u64(rt));
      case 38: return this.set64(rd, this.u64(rs) ^ this.u64(rt));
      case 39: return this.set64(rd, ~(this.u64(rs) | this.u64(rt)));
      case 40: return this.set64(rd, BigInt(this.sa)); // MFSA
      case 41: this.sa = this.u32(rs) & 15; return; // MTSA
      case 42: return this.set64(rd, this.s64(rs) < this.s64(rt) ? 1n : 0n);
      case 43: return this.set64(rd, this.u64(rs) < this.u64(rt) ? 1n : 0n);
      case 44: case 45: return this.set64(rd, this.u64(rs) + this.u64(rt));
      case 46: case 47: return this.set64(rd, this.u64(rs) - this.u64(rt));
      case 56: return this.set64(rd, this.u64(rt) << BigInt(sh));
      case 58: return this.set64(rd, this.u64(rt) >> BigInt(sh));
      case 59: return this.set64(rd, this.s64(rt) >> BigInt(sh));
      case 60: return this.set64(rd, this.u64(rt) << BigInt(sh + 32));
      case 62: return this.set64(rd, this.u64(rt) >> BigInt(sh + 32));
      case 63: return this.set64(rd, this.s64(rt) >> BigInt(sh + 32));
      default: return this.unsup(word, `special fn${fn}`);
    }
  }

  private cop0(word: number, rs: number, rt: number, rd: number): void {
    // Only harmless reads are modelled; log everything.
    this.events.push(`cop0 word ${word.toString(16)} at ${this.curPc.toString(16)}`);
    if (rs === 0) { this.setS32(rt, 0); return; } // MFC0 -> 0
    if (rs === 16 && (word & 63) === 0x38) return; // EI
    if (rs === 16 && (word & 63) === 0x39) return; // DI
    return this.unsup(word, 'cop0');
  }

  // ---------------- COP1 ----------------
  private fo(d: number): number { return ps2Out(d, this.fpuRounding); }
  private cop1(word: number, pc: number, rs: number, rt: number, rd: number, sh: number, fn: number): void {
    const ft = rt, fs = rd, fd = sh;
    if (rs === 0) return this.setS32(rt, this.fpr[fs]!);
    if (rs === 2) return this.setS32(rt, fs === 31 ? this.fcr31 : fs === 0 ? 0x2e30 : 0);
    if (rs === 4) { this.fpr[fs] = this.u32(rt); return; }
    if (rs === 6) { if (fs === 31) this.fcr31 = this.u32(rt); return; }
    if (rs === 8) {
      const tf = (rt & 1) !== 0, likely = (rt & 2) !== 0;
      return this.branch(pc, this.cond() === tf, likely, word);
    }
    if (rs === 20) {
      if (fn === 32) { this.fpr[fd] = this.fo(this.fpr[fs]! | 0); return; } // CVT.S.W
      return this.unsup(word, 'cop1.w');
    }
    if (rs !== 16) return this.unsup(word, 'cop1');
    const a = this.fprF(fs), b = this.fprF(ft);
    const S = this.fpr;
    switch (fn) {
      case 0: S[fd] = this.fo(a + b); return;
      case 1: S[fd] = this.fo(a - b); return;
      case 2: S[fd] = this.fo(a * b); return;
      case 3: {
        if (((S[ft]! >>> 23) & 0xff) === 0) { S[fd] = (((S[fs]! ^ S[ft]!) & 0x80000000) | MAXF_BITS) >>> 0; this.events.push(`div.s by zero at ${pc.toString(16)}`); return; }
        S[fd] = this.fo(a / b); return;
      }
      case 4: { // SQRT.S fd = sqrt(ft)
        if (((S[ft]! >>> 23) & 0xff) === 0) { S[fd] = S[ft]! & 0x80000000; return; }
        S[fd] = this.fo(Math.sqrt(Math.abs(b))); return;
      }
      case 5: S[fd] = S[fs]! & 0x7fffffff; return;
      case 6: S[fd] = S[fs]!; return;
      case 7: S[fd] = (S[fs]! ^ 0x80000000) >>> 0; return;
      case 22: { // RSQRT.S fd = fs / sqrt(ft)
        if (((S[ft]! >>> 23) & 0xff) === 0) { S[fd] = (((S[fs]! ^ S[ft]!) & 0x80000000) | MAXF_BITS) >>> 0; return; }
        const root = bitsToF(this.fo(Math.sqrt(Math.abs(b))));
        S[fd] = this.fo(a / root); return;
      }
      case 24: this.facc = this.fo(a + b); return; // ADDA
      case 25: this.facc = this.fo(a - b); return; // SUBA
      case 26: this.facc = this.fo(a * b); return; // MULA
      case 28: S[fd] = this.fo(ps2In(this.facc) + bitsToF(this.fo(a * b))); return; // MADD
      case 29: S[fd] = this.fo(ps2In(this.facc) - bitsToF(this.fo(a * b))); return; // MSUB
      case 30: this.facc = this.fo(ps2In(this.facc) + bitsToF(this.fo(a * b))); return; // MADDA
      case 31: this.facc = this.fo(ps2In(this.facc) - bitsToF(this.fo(a * b))); return; // MSUBA
      case 36: { // CVT.W.S
        const v = S[fs]!;
        if ((v & 0x7f800000) <= 0x4e800000) S[fd] = Math.trunc(bitsToF(v)) >>> 0;
        else S[fd] = (v & 0x80000000) ? 0x80000000 : 0x7fffffff;
        return;
      }
      case 40: S[fd] = a >= b ? S[fs]! : S[ft]!; return; // MAX.S
      case 41: S[fd] = a <= b ? S[fs]! : S[ft]!; return; // MIN.S
      case 48: this.setCond(false); return; // C.F
      case 50: this.setCond(a === b); return;
      case 52: this.setCond(a < b); return;
      case 54: this.setCond(a <= b); return;
      default: return this.unsup(word, `cop1.s fn${fn}`);
    }
  }

  // ---------------- COP2 / VU0 macro ----------------
  private vo(d: number): number { return ps2Out(d, this.vuRounding); }
  private cop2(word: number, pc: number, rs: number, rt: number, rd: number): void {
    if (rs === 1) { this.set128(rt, EeOracle.pack32(Array.from(this.vf[rd]!))); return; } // QMFC2
    if (rs === 2) { // CFC2
      return this.setS32(rt, this.readCtl(rd));
    }
    if (rs === 5) { // QMTC2
      if (rd) { const v = this.gpr[rt]!; for (let i = 0; i < 4; i++) this.vf[rd]![i] = Number((v >> BigInt(32 * i)) & M32); }
      return;
    }
    if (rs === 6) { this.writeCtl(rd, this.u32(rt)); return; } // CTC2
    if (rs === 8) { // BC2F/BC2T: VU0 never busy in this oracle
      const tf = (rt & 1) !== 0, likely = (rt & 2) !== 0;
      return this.branch(pc, false === tf, likely, word);
    }
    if (rs < 16) return this.unsup(word, 'cop2');
    return this.vuMacro(word);
  }
  private readCtl(r: number): number {
    if (r < 16) return this.vi[r]! & 0xffff;
    switch (r) {
      case 16: return this.vstatus; case 17: return this.vmac; case 18: return this.vclip;
      case 20: return this.rReg; case 21: return this.iReg; case 22: return this.q;
      default: this.events.push(`cfc2 vi${r} at ${this.curPc.toString(16)}`); return 0;
    }
  }
  private writeCtl(r: number, v: number): void {
    if (r === 0) return;
    if (r < 16) { this.vi[r] = v & 0xffff; return; }
    switch (r) {
      case 16: this.vstatus = v; return; case 17: return; case 18: this.vclip = v; return;
      case 20: this.rReg = v; return; case 21: this.iReg = v; return; case 22: this.q = v; return;
      default: this.events.push(`ctc2 vi${r}=${v.toString(16)} at ${this.curPc.toString(16)}`); return;
    }
  }

  private vuMacro(word: number): void {
    const dest = (word >>> 21) & 15, ft = (word >>> 16) & 31, fs = (word >>> 11) & 31, fd = (word >>> 6) & 31;
    const fn = word & 63, bc = word & 3;
    const on = (i: number): boolean => (dest & (8 >> i)) !== 0;
    const A = (i: number): number => this.vfF(fs, i);
    const B = (i: number): number => this.vfF(ft, i);
    const accF = (i: number): number => ps2In(this.vacc[i]!);
    const Q = ps2In(this.q), I = ps2In(this.iReg);
    const wr = (dst: number, f: (i: number) => number): void => {
      const vals = [0, 1, 2, 3].map((i) => (on(i) ? f(i) : 0));
      if (dst) for (let i = 0; i < 4; i++) if (on(i)) this.vf[dst]![i] = vals[i]!;
    };
    const wacc = (f: (i: number) => number): void => {
      const vals = [0, 1, 2, 3].map((i) => (on(i) ? f(i) : 0));
      for (let i = 0; i < 4; i++) if (on(i)) this.vacc[i] = vals[i]!;
    };
    const mul = (x: number, y: number): number => bitsToF(this.vo(x * y));
    const add = (x: number, y: number): number => this.vo(x + y);
    const sub = (x: number, y: number): number => this.vo(x - y);
    if (fn < 0x3c) {
      switch (fn) {
        case 0x00: case 0x01: case 0x02: case 0x03: return wr(fd, (i) => add(A(i), B(bc)));
        case 0x04: case 0x05: case 0x06: case 0x07: return wr(fd, (i) => sub(A(i), B(bc)));
        case 0x08: case 0x09: case 0x0a: case 0x0b: return wr(fd, (i) => add(accF(i), mul(A(i), B(bc))));
        case 0x0c: case 0x0d: case 0x0e: case 0x0f: return wr(fd, (i) => sub(accF(i), mul(A(i), B(bc))));
        case 0x10: case 0x11: case 0x12: case 0x13: return wr(fd, (i) => fmax(this.vf[fs]![i]!, this.vf[ft]![bc]!));
        case 0x14: case 0x15: case 0x16: case 0x17: return wr(fd, (i) => fmin(this.vf[fs]![i]!, this.vf[ft]![bc]!));
        case 0x18: case 0x19: case 0x1a: case 0x1b: return wr(fd, (i) => this.vo(A(i) * B(bc)));
        case 0x1c: return wr(fd, (i) => this.vo(A(i) * Q));
        case 0x1d: return wr(fd, (i) => fmax(this.vf[fs]![i]!, this.iReg));
        case 0x1e: return wr(fd, (i) => this.vo(A(i) * I));
        case 0x1f: return wr(fd, (i) => fmin(this.vf[fs]![i]!, this.iReg));
        case 0x20: return wr(fd, (i) => add(A(i), Q));
        case 0x21: return wr(fd, (i) => add(accF(i), mul(A(i), Q)));
        case 0x22: return wr(fd, (i) => add(A(i), I));
        case 0x23: return wr(fd, (i) => add(accF(i), mul(A(i), I)));
        case 0x24: return wr(fd, (i) => sub(A(i), Q));
        case 0x25: return wr(fd, (i) => sub(accF(i), mul(A(i), Q)));
        case 0x26: return wr(fd, (i) => sub(A(i), I));
        case 0x27: return wr(fd, (i) => sub(accF(i), mul(A(i), I)));
        case 0x28: return wr(fd, (i) => add(A(i), B(i)));
        case 0x29: return wr(fd, (i) => add(accF(i), mul(A(i), B(i))));
        case 0x2a: return wr(fd, (i) => this.vo(A(i) * B(i)));
        case 0x2b: return wr(fd, (i) => fmax(this.vf[fs]![i]!, this.vf[ft]![i]!));
        case 0x2c: return wr(fd, (i) => sub(A(i), B(i)));
        case 0x2d: return wr(fd, (i) => sub(accF(i), mul(A(i), B(i))));
        case 0x2e: { // VOPMSUB: fd.xyz = ACC.xyz - fs.yzx * ft.zxy
          const v = [sub(accF(0), mul(A(1), B(2))), sub(accF(1), mul(A(2), B(0))), sub(accF(2), mul(A(0), B(1)))];
          if (fd) for (let i = 0; i < 3; i++) if (on(i)) this.vf[fd]![i] = v[i]!;
          return;
        }
        case 0x2f: return wr(fd, (i) => fmin(this.vf[fs]![i]!, this.vf[ft]![i]!));
        case 0x30: if (fd) this.vi[fd] = (this.vi[fs]! + this.vi[ft]!) & 0xffff; return; // VIADD id=fd,is=fs,it=ft
        case 0x31: if (fd) this.vi[fd] = (this.vi[fs]! - this.vi[ft]!) & 0xffff; return;
        case 0x32: { const im = ((word >>> 6) & 31) << 27 >> 27; if (ft) this.vi[ft] = (this.vi[fs]! + im) & 0xffff; return; } // VIADDI
        case 0x34: if (fd) this.vi[fd] = (this.vi[fs]! & this.vi[ft]!) & 0xffff; return;
        case 0x35: if (fd) this.vi[fd] = (this.vi[fs]! | this.vi[ft]!) & 0xffff; return;
        case 0x38: case 0x39: return this.unsup(word, 'VCALLMS (VU0 micro memory not in snapshot)');
        default: return this.unsup(word, `vu upper fn${fn}`);
      }
    }
    const sp = (word & 3) | (((word >>> 6) & 31) << 2);
    switch (sp) {
      case 0x00: case 0x01: case 0x02: case 0x03: return wacc((i) => add(A(i), B(sp & 3)));
      case 0x04: case 0x05: case 0x06: case 0x07: return wacc((i) => sub(A(i), B(sp & 3)));
      case 0x08: case 0x09: case 0x0a: case 0x0b: return wacc((i) => add(accF(i), mul(A(i), B(sp & 3))));
      case 0x0c: case 0x0d: case 0x0e: case 0x0f: return wacc((i) => sub(accF(i), mul(A(i), B(sp & 3))));
      case 0x10: case 0x11: case 0x12: case 0x13: { // VITOF0/4/12/15 ft = fs
        const sc = [1, 16, 4096, 32768][sp & 3]!;
        return wr(ft, (i) => this.vo((this.vf[fs]![i]! | 0) / sc));
      }
      case 0x14: case 0x15: case 0x16: case 0x17: { // VFTOI0/4/12/15
        const sc = [1, 16, 4096, 32768][sp & 3]!;
        return wr(ft, (i) => {
          const x = Math.trunc(A(i) * sc);
          return (x >= 2147483647 ? 0x7fffffff : x <= -2147483648 ? 0x80000000 : x) >>> 0;
        });
      }
      case 0x18: case 0x19: case 0x1a: case 0x1b: return wacc((i) => this.vo(A(i) * B(sp & 3)));
      case 0x1c: return wacc((i) => this.vo(A(i) * Q));
      case 0x1d: return wr(ft, (i) => this.vf[fs]![i]! & 0x7fffffff); // VABS
      case 0x1e: return wacc((i) => this.vo(A(i) * I));
      case 0x1f: { // VCLIPw.xyz fs, ft.w
        const w = Math.abs(B(3));
        let f = 0;
        if (A(0) > w) f |= 1; if (A(0) < -w) f |= 2; if (A(1) > w) f |= 4; if (A(1) < -w) f |= 8; if (A(2) > w) f |= 16; if (A(2) < -w) f |= 32;
        this.vclip = ((this.vclip << 6) | f) & 0xffffff; return;
      }
      case 0x20: return wacc((i) => add(A(i), Q));
      case 0x21: return wacc((i) => add(accF(i), mul(A(i), Q)));
      case 0x22: return wacc((i) => add(A(i), I));
      case 0x23: return wacc((i) => add(accF(i), mul(A(i), I)));
      case 0x24: return wacc((i) => sub(A(i), Q));
      case 0x25: return wacc((i) => sub(accF(i), mul(A(i), Q)));
      case 0x26: return wacc((i) => sub(A(i), I));
      case 0x27: return wacc((i) => sub(accF(i), mul(A(i), I)));
      case 0x28: return wacc((i) => add(A(i), B(i)));
      case 0x29: return wacc((i) => add(accF(i), mul(A(i), B(i))));
      case 0x2a: return wacc((i) => this.vo(A(i) * B(i)));
      case 0x2c: return wacc((i) => sub(A(i), B(i)));
      case 0x2d: return wacc((i) => sub(accF(i), mul(A(i), B(i))));
      case 0x2e: { // VOPMULA: ACC.xyz = fs.yzx * ft.zxy
        const v = [this.vo(A(1) * B(2)), this.vo(A(2) * B(0)), this.vo(A(0) * B(1))];
        for (let i = 0; i < 3; i++) if (on(i)) this.vacc[i] = v[i]!;
        return;
      }
      case 0x2f: return; // VNOP
      case 0x30: return wr(ft, (i) => this.vf[fs]![i]!); // VMOVE
      case 0x31: return wr(ft, (i) => this.vf[fs]![(i + 1) & 3]!); // VMR32
      case 0x38: { // VDIV Q = fs[fsf] / ft[ftf]
        const fsf = (word >>> 21) & 3, ftf = (word >>> 23) & 3;
        const nb = this.vf[fs]![fsf]!, db = this.vf[ft]![ftf]!;
        if (((db >>> 23) & 0xff) === 0) { this.q = (((nb ^ db) & 0x80000000) | MAXF_BITS) >>> 0; this.events.push(`vdiv by zero at ${this.curPc.toString(16)}`); return; }
        this.q = this.vo(ps2In(nb) / ps2In(db)); return;
      }
      case 0x39: { const ftf = (word >>> 23) & 3; this.q = this.vo(Math.sqrt(Math.abs(B(ftf)))); return; } // VSQRT
      case 0x3a: { // VRSQRT Q = fs[fsf] / sqrt(|ft[ftf]|)
        const fsf = (word >>> 21) & 3, ftf = (word >>> 23) & 3;
        const db = this.vf[ft]![ftf]!;
        if (((db >>> 23) & 0xff) === 0) { this.q = ((this.vf[fs]![fsf]! & 0x80000000) | MAXF_BITS) >>> 0; return; }
        const root = Math.sqrt(Math.abs(ps2In(db)));
        this.q = this.vo(A(fsf) / root); return;
      }
      case 0x3b: return; // VWAITQ
      case 0x3c: { const fsf = (word >>> 21) & 3; if (ft) this.vi[ft] = this.vf[fs]![fsf]! & 0xffff; return; } // VMTIR it, fs[fsf]
      case 0x3d: { const v = (this.vi[fs]! << 16) >> 16; return wr(ft, () => v >>> 0); } // VMFIR ft, is
      case 0x40: case 0x41: case 0x42: case 0x43: return this.unsup(word, 'VR* random ops');
      default: return this.unsup(word, `vu special sp${sp}`);
    }
  }

  // ---------------- MMI ----------------
  private mmi(word: number, rs: number, rt: number, rd: number, sa: number, fn: number): void {
    const L = (r: number) => this.lanes(r);
    switch (fn) {
      case 0x00: case 0x01: { // MADD / MADDU (rd also written)
        const acc = (BigInt.asUintN(32, this.hi & M32) << 32n) | (this.lo & M32);
        const p = fn === 0 ? BigInt(this.s32(rs)) * BigInt(this.s32(rt)) : BigInt(this.u32(rs)) * BigInt(this.u32(rt));
        const s = BigInt.asUintN(64, acc + p);
        const l = BigInt.asUintN(64, BigInt.asIntN(32, s & M32)), h = BigInt.asUintN(64, BigInt.asIntN(32, s >> 32n));
        this.lo = (this.lo & HI64) | l; this.hi = (this.hi & HI64) | h; this.set64(rd, l); return;
      }
      case 0x04: { // PLZCW
        const cnt = (x: number): number => { const neg = (x >>> 31) & 1; let n = 0; for (let b = 30; b >= 0 && ((x >>> b) & 1) === neg; b--) n++; return n; };
        return this.set64(rd, BigInt(cnt(this.lane(rs, 0))) | (BigInt(cnt(this.lane(rs, 1))) << 32n));
      }
      case 0x08: return this.mmi0(word, rs, rt, rd, sa);
      case 0x09: return this.mmi2(word, rs, rt, rd, sa);
      case 0x10: return this.set64(rd, this.hi >> 64n); // MFHI1
      case 0x11: this.hi = (this.hi & M64) | (this.u64(rs) << 64n); return;
      case 0x12: return this.set64(rd, this.lo >> 64n); // MFLO1
      case 0x13: this.lo = (this.lo & M64) | (this.u64(rs) << 64n); return;
      case 0x18: case 0x19: { // MULT1 / MULTU1
        const p = fn === 0x18 ? BigInt(this.s32(rs)) * BigInt(this.s32(rt)) : BigInt(this.u32(rs)) * BigInt(this.u32(rt));
        const l = BigInt.asUintN(64, BigInt.asIntN(32, p & M32)), h = BigInt.asUintN(64, BigInt.asIntN(32, (p >> 32n) & M32));
        this.lo = (this.lo & M64) | (l << 64n); this.hi = (this.hi & M64) | (h << 64n); this.set64(rd, l); return;
      }
      case 0x1a: { const [q, r] = div32(this.s32(rs), this.s32(rt)); this.lo = (this.lo & M64) | (BigInt.asUintN(64, BigInt(q)) << 64n); this.hi = (this.hi & M64) | (BigInt.asUintN(64, BigInt(r)) << 64n); return; }
      case 0x1b: { const [q, r] = divu32(this.u32(rs), this.u32(rt)); this.lo = (this.lo & M64) | (BigInt.asUintN(64, BigInt(q | 0)) << 64n); this.hi = (this.hi & M64) | (BigInt.asUintN(64, BigInt(r | 0)) << 64n); return; }
      case 0x28: return this.mmi1(word, rs, rt, rd, sa);
      case 0x29: return this.mmi3(word, rs, rt, rd, sa);
      case 0x34: { const h = this.halves(rt).map((x) => (x << sa) & 0xffff); return this.set128(rd, EeOracle.packH(h)); } // PSLLH
      case 0x36: { const h = this.halves(rt).map((x) => x >>> (sa & 15)); return this.set128(rd, EeOracle.packH(h)); }
      case 0x37: { const h = this.halves(rt).map((x) => ((x << 16) >> 16) >> (sa & 15)); return this.set128(rd, EeOracle.packH(h)); }
      case 0x3c: return this.set128(rd, EeOracle.pack32(L(rt).map((x) => x << sa)));
      case 0x3e: return this.set128(rd, EeOracle.pack32(L(rt).map((x) => x >>> sa)));
      case 0x3f: return this.set128(rd, EeOracle.pack32(L(rt).map((x) => (x | 0) >> sa)));
      default: return this.unsup(word, `mmi fn${fn}`);
    }
  }
  private mmi0(word: number, rs: number, rt: number, rd: number, sa: number): void {
    const a = this.lanes(rs), b = this.lanes(rt);
    switch (sa) {
      case 0x00: return this.set128(rd, EeOracle.pack32(a.map((x, i) => x + b[i]!)));
      case 0x01: return this.set128(rd, EeOracle.pack32(a.map((x, i) => x - b[i]!)));
      case 0x02: return this.set128(rd, EeOracle.pack32(a.map((x, i) => ((x | 0) > (b[i]! | 0) ? -1 : 0))));
      case 0x03: return this.set128(rd, EeOracle.pack32(a.map((x, i) => ((x | 0) > (b[i]! | 0) ? x : b[i]!))));
      case 0x12: return this.set128(rd, EeOracle.pack32([b[0]!, a[0]!, b[1]!, a[1]!])); // PEXTLW
      case 0x13: return this.set128(rd, EeOracle.pack32([b[0]!, b[2]!, a[0]!, a[2]!])); // PPACW
      default: return this.unsup(word, `mmi0 sa${sa}`);
    }
  }
  private mmi1(word: number, rs: number, rt: number, rd: number, sa: number): void {
    const a = this.lanes(rs), b = this.lanes(rt);
    switch (sa) {
      case 0x01: return this.set128(rd, EeOracle.pack32(b.map((x) => ((x | 0) === -2147483648 ? 0x7fffffff : Math.abs(x | 0)))));
      case 0x02: return this.set128(rd, EeOracle.pack32(a.map((x, i) => (x === b[i]! ? -1 : 0))));
      case 0x03: return this.set128(rd, EeOracle.pack32(a.map((x, i) => ((x | 0) < (b[i]! | 0) ? x : b[i]!))));
      case 0x12: return this.set128(rd, EeOracle.pack32([b[2]!, a[2]!, b[3]!, a[3]!])); // PEXTUW
      default: return this.unsup(word, `mmi1 sa${sa}`);
    }
  }
  private mmi2(word: number, rs: number, rt: number, rd: number, sa: number): void {
    const A = this.gpr[rs]!, B = this.gpr[rt]!;
    switch (sa) {
      case 0x08: return this.set128(rd, this.hi); // PMFHI
      case 0x09: return this.set128(rd, this.lo); // PMFLO
      case 0x0e: return this.set128(rd, (B & M64) | ((A & M64) << 64n)); // PCPYLD rd = {rt.lo, rs.lo}
      case 0x12: return this.set128(rd, A & B);
      case 0x13: return this.set128(rd, A ^ B);
      case 0x1e: { const b = this.lanes(rt); return this.set128(rd, EeOracle.pack32([b[2]!, b[1]!, b[0]!, b[3]!])); } // PEXEW
      case 0x1f: { const b = this.lanes(rt); return this.set128(rd, EeOracle.pack32([b[1]!, b[2]!, b[0]!, b[3]!])); } // PROT3W
      default: return this.unsup(word, `mmi2 sa${sa}`);
    }
  }
  private mmi3(word: number, rs: number, rt: number, rd: number, sa: number): void {
    const A = this.gpr[rs]!, B = this.gpr[rt]!;
    switch (sa) {
      case 0x08: this.hi = A; return; // PMTHI
      case 0x09: this.lo = A; return; // PMTLO
      case 0x0e: return this.set128(rd, (A >> 64n) | ((B >> 64n) << 64n)); // PCPYUD rd = {rs.hi, rt.hi}
      case 0x12: return this.set128(rd, A | B);
      case 0x13: return this.set128(rd, ~(A | B) & M128);
      case 0x1b: { const h = this.halves(rt); return this.set128(rd, EeOracle.packH([h[0]!, h[0]!, h[0]!, h[0]!, h[4]!, h[4]!, h[4]!, h[4]!])); } // PCPYH
      case 0x1e: { const b = this.lanes(rt); return this.set128(rd, EeOracle.pack32([b[0]!, b[2]!, b[1]!, b[3]!])); } // PEXCW
      default: return this.unsup(word, `mmi3 sa${sa}`);
    }
  }
}

function fmax(a: number, b: number): number { return ps2In(a) >= ps2In(b) ? a : b; }
function fmin(a: number, b: number): number { return ps2In(a) <= ps2In(b) ? a : b; }

function div32(n: number, d: number): [number, number] {
  if (d === 0) return [n < 0 ? 1 : -1, n];
  if (n === -2147483648 && d === -1) return [-2147483648, 0];
  return [Math.trunc(n / d) | 0, (n % d) | 0];
}
function divu32(n: number, d: number): [number, number] {
  if (d === 0) return [-1 >>> 0, n >>> 0];
  return [Math.floor(n / d) >>> 0, (n % d) >>> 0];
}
