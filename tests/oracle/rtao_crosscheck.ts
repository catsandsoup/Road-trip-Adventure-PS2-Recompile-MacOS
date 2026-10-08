/**
 * Independent cross-check with RTAO's own R5900/VU0 executor (rtao/rtao/test-support/
 * palScalarMachine.ts + palVuMachine.ts, imported read-only; not modified).
 *
 * For every call to a libvu0 routine that our build replaces with an HLE stub
 * (sceVu0MulMatrix 0x2757a0, sceVu0ApplyMatrix 0x275770, sceVu0InversMatrix 0x2758b8,
 * sceVu0Normalize 0x275830) made during a snapshot call, capture the exact inputs in our
 * oracle (guest mode), then execute the ORIGINAL guest routine from the PAL ELF in RTAO's
 * machine on the same inputs and compare: RTAO-guest vs our-oracle-guest vs TS stub port.
 *
 * Usage: node game/tests/oracle/rtao_crosscheck.ts work/explore/snap/21f540_200
 */
import fs from 'node:fs';
import path from 'node:path';
import { register } from 'node:module';
import { bitsToF, fToBits } from './ee_oracle.ts';
import { loadOracle, loadStubMap, cloneOracle, hex, fstr, ulp } from './run_call.ts';
import { makeStubs } from './hle_stubs.ts';

// RTAO's test-support imports './palVuMachine' without an extension (vitest resolves it);
// add a resolve hook so plain node can load it unmodified.
register('data:text/javascript,' + encodeURIComponent(`
export async function resolve(spec, ctx, next) {
  try { return await next(spec, ctx); }
  catch (e) { if (spec.startsWith('.') && !/\\.[a-z]+$/.test(spec)) return next(spec + '.ts', ctx); throw e; }
}`));

const ROOT = path.resolve(path.dirname(new URL(import.meta.url).pathname), '../../..');
const { PalScalarMachine } = await import(path.join(ROOT, 'rtao/rtao/test-support/palScalarMachine.ts'));

const prefix = path.resolve(process.argv[2] ?? 'work/explore/snap/21f540_200');
const elf = new Uint8Array(fs.readFileSync(path.join(ROOT, 'disc/extracted/SLES_513.56')));
const { m, snap } = loadOracle(prefix, { fpu: 'rz', vu: 'rz' });
m.stubNames = loadStubMap();
const stubs = makeStubs('rz');
const WATCH: Record<number, { name: string; inA1: number; inA2: number; out: number }> = {
  0x2757a0: { name: 'sceVu0MulMatrix', inA1: 64, inA2: 64, out: 64 },
  0x275770: { name: 'sceVu0ApplyMatrix', inA1: 64, inA2: 16, out: 16 },
  0x2758b8: { name: 'sceVu0InversMatrix', inA1: 64, inA2: 0, out: 64 },
  0x275830: { name: 'sceVu0Normalize', inA1: 16, inA2: 0, out: 16 },
};
const words = (mm: { r32(a: number): number }, a: number, n: number) => Array.from({ length: n / 4 }, (_, i) => mm.r32(a + 4 * i));
let worst = 0;
m.onCall = (rec, mm) => {
  const w = WATCH[rec.target];
  if (!w) return;
  const [a0, a1, a2] = rec.args as [number, number, number];
  const in1 = words(mm, a1, w.inA1), in2 = w.inA2 ? words(mm, a2, w.inA2) : [];
  // our oracle, guest body
  const g = cloneOracle(mm); g.run(rec.ra, rec.sp, 100000);
  const oursGuest = words(g, a0, w.out);
  // TS port of our HLE stub
  const s = cloneOracle(mm); stubs.get(w.name)!(s);
  const stubOut = words(s, a0, w.out);
  // RTAO executor running the ELF's routine (fresh machine, fixed scratch addresses)
  const r = new PalScalarMachine(elf);
  (r as unknown as { codeRanges: number[][] }).codeRanges = [[rec.target, rec.target + 0x100]];
  const A0 = 0x1e00000, A1 = 0x1e00100, A2 = 0x1e00200;
  in1.forEach((x, i) => r.view.setUint32(A1 + 4 * i, x, true));
  in2.forEach((x, i) => r.view.setUint32(A2 + 4 * i, x, true));
  if (rec.target === 0x2757a0 && (a0 === a1 || a0 === a2)) {
    // in-place call: the guest loop reads A2 rows after writing earlier output rows; emulate aliasing
    if (a0 === a2) r.run(rec.target, [A2, A1, A2]); else r.run(rec.target, [A1, A1, A2]);
  } else r.run(rec.target, [A0, A1, A2]);
  const outAddr = rec.target === 0x2757a0 && a0 === a2 ? A2 : rec.target === 0x2757a0 && a0 === a1 ? A1 : A0;
  const rtao = Array.from({ length: w.out / 4 }, (_, i) => r.view.getUint32(outAddr + 4 * i, true));
  const u1 = Math.max(...rtao.map((x, i) => ulp(x, oursGuest[i]!)));
  const absStub = Math.max(...rtao.map((x, i) => Math.abs(bitsToF(x) - bitsToF(stubOut[i]!))));
  worst = Math.max(worst, absStub);
  console.log(`#${rec.index} ${w.name} a0=${hex(a0)} a1=${hex(a1)} a2=${hex(a2)}: RTAO-guest vs oracle-guest max ulp ${u1}; RTAO-guest vs our-stub max |diff| ${absStub.toPrecision(4)}`);
  if (absStub > 1e-3) {
    const rows = (v: number[]) => Array.from({ length: v.length / 4 }, (_, i) => `(${v.slice(4 * i, 4 * i + 4).map(fstr).join(', ')})`).join(' ');
    console.log(`    A1    = ${rows(in1)}`);
    if (in2.length) console.log(`    A2    = ${rows(in2)}`);
    console.log(`    guest = ${rows(rtao)}`);
    console.log(`    stub  = ${rows(stubOut)}`);
  }
};
m.run(snap.gpr[31]![0]!, snap.gpr[29]![0]!, 20_000_000);
console.log(`done; worst RTAO-guest vs stub abs diff ${worst}`);
void fToBits;
