/**
 * TS ports of the ps2xRuntime HLE stubs that replace guest library functions in OUR build
 * (PS2Recomp/ps2xRuntime/src/lib/Kernel/Stubs/VU.cpp and LibC.cpp, as of 2026-10-07).
 * Used by run_call.ts --mode stub to reproduce what the recompiled build computes, so the
 * difference between "guest" mode (real libvu0/libm code executed by the oracle) and
 * "stub" mode isolates stub-semantics bugs from translator bugs.
 *
 * Host float math in the runtime runs under FE_TOWARDZERO + FZ (ps2_runtime.cpp:2593), so
 * each float op here rounds with the oracle's selected rounding. FMA contraction by clang
 * is not modelled (ULP-level differences only).
 */
import { EeOracle, bitsToF, fToBits, ps2Out } from './ee_oracle.ts';
import type { Rounding } from './ee_oracle.ts';

type Stub = (m: EeOracle) => void;

export interface StubFixes { mulMatrix?: boolean; normalize?: boolean }

export function makeStubs(rounding: Rounding, fixes: StubFixes = {}): Map<string, Stub> {
  const R = (d: number): number => bitsToF(ps2Out(d, rounding));
  const mul = (a: number, b: number): number => R(a * b);
  const add = (a: number, b: number): number => R(a + b);
  const sub = (a: number, b: number): number => R(a - b);
  const readV = (m: EeOracle, a: number, n = 4): number[] => Array.from({ length: n }, (_, i) => bitsToF(m.r32(a + 4 * i)));
  const writeV = (m: EeOracle, a: number, v: readonly number[]): void => v.forEach((x, i) => m.w32(a + 4 * i, fToBits(x)));
  const ret0 = (m: EeOracle): void => m.setS32(2, 0);
  const a0 = (m: EeOracle) => m.u32(4), a1 = (m: EeOracle) => m.u32(5), a2 = (m: EeOracle) => m.u32(6);
  // Row-major product as in VU.cpp mulVuMatrix: out[4i+j] += rhs[4k+j]*lhs[4i+k], k ascending.
  const mulVuMatrix = (lhs: number[], rhs: number[]): number[] => {
    const out = Array<number>(16).fill(0);
    for (let i = 0; i < 4; i++) for (let j = 0; j < 4; j++) for (let k = 0; k < 4; k++)
      out[4 * i + j] = add(out[4 * i + j]!, mul(rhs[4 * k + j]!, lhs[4 * i + k]!));
    return out;
  };
  const ident = (): number[] => [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];
  const cosf = (x: number): number => R(Math.cos(x));
  const sinf = (x: number): number => R(Math.sin(x));
  const s = new Map<string, Stub>();

  s.set('sceVu0ApplyMatrix', (m) => {
    const M = readV(m, a1(m), 16), v = readV(m, a2(m));
    const out = [0, 1, 2, 3].map((i) => add(add(add(mul(M[i]!, v[0]!), mul(M[4 + i]!, v[1]!)), mul(M[8 + i]!, v[2]!)), mul(M[12 + i]!, v[3]!)));
    writeV(m, a0(m), out); ret0(m);
  });
  s.set('sceVu0MulMatrix', (m) => {
    // VU.cpp:694: mulVuMatrix(m0 = *a1, m1 = *a2, out) -> out = A1 . A2 (row-major)
    const A1 = readV(m, a1(m), 16), A2 = readV(m, a2(m), 16);
    // fixes.mulMatrix: guest libvu0 0x2757a0 computes out[i] = sum_k A1[k] * A2[i].k = A2 . A1
    writeV(m, a0(m), fixes.mulMatrix ? mulVuMatrix(A2, A1) : mulVuMatrix(A1, A2)); ret0(m);
  });
  s.set('sceVu0Normalize', (m) => {
    const v = readV(m, a1(m));
    if (fixes.normalize) { // guest 0x275830: Q = 1/sqrt(x^2+y^2+z^2); out.xyz = v.xyz*Q, out.w = 0
      const len = R(Math.sqrt(add(add(mul(v[0]!, v[0]!), mul(v[1]!, v[1]!)), mul(v[2]!, v[2]!))));
      const q = R(1 / len);
      writeV(m, a0(m), [mul(v[0]!, q), mul(v[1]!, q), mul(v[2]!, q), 0]); ret0(m); return;
    }
    const len = R(Math.sqrt(add(add(add(mul(v[0]!, v[0]!), mul(v[1]!, v[1]!)), mul(v[2]!, v[2]!)), mul(v[3]!, v[3]!))));
    let out = [0, 0, 0, 0];
    if (len > 1.0e-6) { const inv = R(1 / len); out = v.map((x) => mul(x, inv)); }
    writeV(m, a0(m), out); ret0(m);
  });
  s.set('sceVu0InversMatrix', (m) => {
    const inp = readV(m, a1(m), 16), out = Array<number>(16).fill(0);
    for (let row = 0; row < 3; row++) { for (let col = 0; col < 3; col++) out[4 * row + col] = inp[4 * col + row]!; out[4 * row + 3] = 0; }
    const tx = inp[12]!, ty = inp[13]!, tz = inp[14]!;
    for (let col = 0; col < 3; col++) out[12 + col] = -add(add(mul(tx, inp[4 * col]!), mul(ty, inp[4 * col + 1]!)), mul(tz, inp[4 * col + 2]!));
    out[15] = inp[15]!;
    writeV(m, a0(m), out); ret0(m);
  });
  s.set('sceVu0UnitMatrix', (m) => { writeV(m, a0(m), ident()); ret0(m); });
  const rot = (axis: 0 | 1 | 2) => (m: EeOracle) => {
    const src = readV(m, a1(m), 16), angle = bitsToF(m.fpr[12]!);
    const r = ident(), c = cosf(angle), sn = sinf(angle);
    if (axis === 0) { r[5] = c; r[6] = sn; r[9] = -sn; r[10] = c; }
    else if (axis === 1) { r[0] = c; r[2] = -sn; r[8] = sn; r[10] = c; }
    else { r[0] = c; r[1] = sn; r[4] = -sn; r[5] = c; }
    writeV(m, a0(m), mulVuMatrix(src, r)); ret0(m);
  };
  s.set('sceVu0RotMatrixX', rot(0));
  s.set('sceVu0RotMatrixY', rot(1));
  s.set('sceVu0RotMatrixZ', rot(2));
  s.set('sceVu0CopyMatrix', (m) => { for (let i = 0; i < 16; i++) m.w32(a0(m) + 4 * i, m.r32(a1(m) + 4 * i)); ret0(m); });
  s.set('sceVu0TransposeMatrix', (m) => {
    const src = readV(m, a1(m), 16), out = Array<number>(16).fill(0);
    for (let r = 0; r < 4; r++) for (let c = 0; c < 4; c++) out[4 * r + c] = src[4 * c + r]!;
    writeV(m, a0(m), out); ret0(m);
  });
  s.set('sceVu0TransMatrix', (m) => {
    const src = readV(m, a1(m), 16), v = readV(m, a2(m)), out = src.slice();
    out[12] = add(src[12]!, v[0]!); out[13] = add(src[13]!, v[1]!); out[14] = add(src[14]!, v[2]!);
    writeV(m, a0(m), out); ret0(m);
  });
  s.set('sceVu0InnerProduct', (m) => {
    const l = readV(m, a0(m)), r = readV(m, a1(m));
    const dot = add(add(add(mul(l[0]!, r[0]!), mul(l[1]!, r[1]!)), mul(l[2]!, r[2]!)), mul(l[3]!, r[3]!));
    m.fpr[0] = fToBits(dot); m.setS32(2, fToBits(dot));
  });
  s.set('__kernel_sinf', (m) => {
    const x = bitsToF(m.fpr[12]!), y = bitsToF(m.fpr[13]!), iy = m.s32(4);
    m.fpr[0] = fToBits(sinf(add(x, iy !== 0 ? y : 0)));
  });
  s.set('__kernel_cosf', (m) => {
    const x = bitsToF(m.fpr[12]!), y = bitsToF(m.fpr[13]!);
    m.fpr[0] = fToBits(cosf(add(x, y)));
  });
  s.set('__ieee754_rem_pio2f', (m) => {
    const x = bitsToF(m.fpr[12]!);
    const kPi = Math.fround(3.14159265358979323846), kHalfPi = Math.fround(kPi * 0.5), kInv = Math.fround(2 / kPi);
    const n = Math.round(mul(x, kInv)) | 0; // nearbyintf (ties-to-even ignored)
    const y0 = sub(x, mul(Math.fround(n), kHalfPi));
    m.w32(a0(m), fToBits(y0)); m.w32(a0(m) + 4, 0); m.setS32(2, n);
  });
  s.set('cos', (m) => { m.fpr[0] = fToBits(cosf(bitsToF(m.fpr[12]!))); });
  s.set('memcpy', (m) => { const d = a0(m), sr = a1(m), n = a2(m); const tmp = Array.from({ length: n }, (_, i) => m.r8(sr + i)); tmp.forEach((b, i) => m.w8(d + i, b)); m.setS32(2, d); });
  s.set('memmove', s.get('memcpy')!);
  s.set('memset', (m) => { const d = a0(m), v = m.u32(5) & 0xff, n = a2(m); for (let i = 0; i < n; i++) m.w8(d + i, v); m.setS32(2, d); });
  s.set('memclr', (m) => { const d = a0(m), n = a1(m); for (let i = 0; i < n; i++) m.w8(d + i, 0); m.setS32(2, d); });
  return s;
}
