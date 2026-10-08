/**
 * Differential runner: load a PS2X_TRACE_CALLS_SNAPSHOT enter snapshot into the test-only
 * R5900 oracle, run from the entry pc until it returns to the entry ra (same sp), then diff
 * the oracle's RAM/registers against OUR recompiled build's exit snapshot.
 *
 * Usage (from repo root):
 *   node game/tests/oracle/run_call.ts work/explore/snap/21f540_200 [options]
 * Options:
 *   --mode guest|stub   guest (default): execute the real guest bodies of every function, including
 *                       those our build replaces with ps2xRuntime HLE stubs.
 *                       stub: emulate our HLE stubs (hle_stubs.ts) at those addresses instead.
 *   --fpu rz|rn --vu rz|rn   rounding (default rz = PS2 round-toward-zero)
 *   --calls             print every call made (with HLE-stub names)
 *   --stubcmp           at every call to an address our build stubs, run BOTH the guest body (in
 *                       a cloned oracle) and the TS stub port on identical inputs; report diffs
 *   --max-words N       words shown per differing RAM range (default 24)
 *   --dump <file>       write the oracle's final state as a snapshot JSON (+ .ram)
 *   --stop-pc <hex> [--stop-hit n]  stop at the n-th (default 1) arrival at pc, dump state
 *   --watch <hexaddr>:<len>  log every oracle write into that range (pc, value)
 */
import fs from 'node:fs';
import path from 'node:path';
import { EeOracle, bitsToF } from './ee_oracle.ts';
import type { CallRecord, Rounding } from './ee_oracle.ts';
import { makeStubs } from './hle_stubs.ts';

const ROOT = path.resolve(path.dirname(new URL(import.meta.url).pathname), '../../..');

interface Snap {
  pc: number; gpr: number[][]; hi: bigint; lo: bigint; hi1: bigint; lo1: bigint; sa: number;
  fpr: number[]; facc: number; fcr31: number; vf: number[][]; vi: number[]; vacc: number[];
  q: number; p: number; i: number; vstatus: number; vmac: number; vclip: number;
}

export function loadSnapJson(file: string): Snap {
  const text = fs.readFileSync(file, 'utf8');
  const j = JSON.parse(text);
  const big = (k: string): bigint => { const m = new RegExp(`"${k}":(\\d+)`).exec(text); return m ? BigInt(m[1]!) : 0n; };
  return { ...j, hi: big('hi'), lo: big('lo'), hi1: big('hi1'), lo1: big('lo1') };
}

export function loadStubMap(): Map<number, string> {
  const cache = path.join(ROOT, 'game/tests/oracle/.stubmap.json');
  const gen = path.join(ROOT, 'work/generated');
  try {
    const st = fs.statSync(cache), gst = fs.statSync(gen);
    if (st.mtimeMs > gst.mtimeMs) return new Map(Object.entries(JSON.parse(fs.readFileSync(cache, 'utf8'))).map(([k, v]) => [Number(k), v as string]));
  } catch { /* rebuild */ }
  const map = new Map<number, string>();
  for (const f of fs.readdirSync(gen)) {
    const m = /^FUN_([0-9a-f]+)_0x[0-9a-f]+\.cpp$/.exec(f);
    if (!m) continue;
    const s = fs.readFileSync(path.join(gen, f), 'utf8');
    if (s.length > 4000) continue;
    const st = /ps2_(?:stubs|syscalls)::([A-Za-z0-9_]+)/.exec(s);
    if (st) map.set(parseInt(m[1]!, 16), st[1]!);
  }
  try { fs.writeFileSync(cache, JSON.stringify(Object.fromEntries(map))); } catch { /* ignore */ }
  return map;
}

export function loadOracle(prefix: string, opts: { fpu: Rounding; vu: Rounding }): { m: EeOracle; snap: Snap } {
  const m = new EeOracle({ fpuRounding: opts.fpu, vuRounding: opts.vu });
  const ram = fs.readFileSync(`${prefix}_enter.ram`);
  if (ram.length !== 0x2000000) throw new Error('enter.ram must be 32 MiB');
  m.mem.set(ram);
  const s = loadSnapJson(`${prefix}_enter.json`);
  applyRegs(m, s);
  return { m, snap: s };
}

export function applyRegs(m: EeOracle, s: Snap): void {
  for (let r = 0; r < 32; r++) m.gpr[r] = EeOracle.pack32(s.gpr[r]!);
  m.gpr[0] = 0n;
  m.hi = (s.hi & ((1n << 64n) - 1n)) | (s.hi1 << 64n);
  m.lo = (s.lo & ((1n << 64n) - 1n)) | (s.lo1 << 64n);
  m.sa = s.sa;
  for (let i = 0; i < 32; i++) m.fpr[i] = s.fpr[i]!;
  m.facc = s.facc; m.fcr31 = s.fcr31;
  for (let r = 0; r < 32; r++) for (let l = 0; l < 4; l++) m.vf[r]![l] = s.vf[r]![l]!;
  for (let i = 0; i < 16; i++) m.vi[i] = s.vi[i]!;
  for (let l = 0; l < 4; l++) m.vacc[l] = s.vacc[l]!;
  m.q = s.q; m.p = s.p; m.iReg = s.i; m.vstatus = s.vstatus; m.vmac = s.vmac; m.vclip = s.vclip;
  m.pc = s.pc; m.npc = s.pc + 4;
}

export function cloneOracle(src: EeOracle): EeOracle {
  const m = new EeOracle({ fpuRounding: src.fpuRounding, vuRounding: src.vuRounding });
  m.mem.set(src.mem); m.spr.set(src.spr);
  for (let r = 0; r < 32; r++) m.gpr[r] = src.gpr[r]!;
  m.hi = src.hi; m.lo = src.lo; m.sa = src.sa; m.fpr.set(src.fpr); m.facc = src.facc; m.fcr31 = src.fcr31;
  for (let r = 0; r < 32; r++) m.vf[r]!.set(src.vf[r]!);
  m.vi.set(src.vi); m.vacc.set(src.vacc); m.q = src.q; m.p = src.p; m.iReg = src.iReg; m.rReg = src.rReg;
  m.vstatus = src.vstatus; m.vmac = src.vmac; m.vclip = src.vclip; m.pc = src.pc; m.npc = src.npc;
  m.stubNames = src.stubNames;
  return m;
}

export function snapOf(m: EeOracle): Snap {
  const M64 = (1n << 64n) - 1n;
  return {
    pc: m.pc, gpr: Array.from({ length: 32 }, (_, r) => m.lanes(r)), hi: m.hi & M64, lo: m.lo & M64, hi1: m.hi >> 64n, lo1: m.lo >> 64n,
    sa: m.sa, fpr: Array.from(m.fpr), facc: m.facc, fcr31: m.fcr31, vf: m.vf.map((v) => Array.from(v)), vi: Array.from(m.vi),
    vacc: Array.from(m.vacc), q: m.q, p: m.p, i: m.iReg, vstatus: m.vstatus, vmac: m.vmac, vclip: m.vclip,
  };
}

export function writeSnap(file: string, m: EeOracle): void {
  const s = snapOf(m);
  fs.writeFileSync(file.replace(/\.json$/, '') + '.json', JSON.stringify(s, (_, v) => (typeof v === 'bigint' ? Number(v) : v)));
  fs.writeFileSync(file.replace(/\.json$/, '') + '.ram', m.mem);
}

export const hex = (x: number, w = 8): string => (x >>> 0).toString(16).padStart(w, '0');
export const fstr = (b: number): string => { const f = bitsToF(b); return Number.isFinite(f) ? (Math.abs(f) >= 1e-4 && Math.abs(f) < 1e7 || f === 0 ? f.toPrecision(7) : f.toExponential(4)) : String(f); };
export function ulp(a: number, b: number): number {
  const key = (x: number) => ((x >>> 31) ? -(x & 0x7fffffff) : (x & 0x7fffffff));
  return Math.abs(key(a) - key(b));
}

/** Diff oracle state vs expected snapshot (ours). Returns printable lines. */
export function diffState(m: EeOracle, expRam: Uint8Array, exp: Snap, enterRam: Uint8Array, maxWords = 24): { lines: string[]; ramRanges: number; regDiffs: number; maxUlp: number } {
  const lines: string[] = [];
  let regDiffs = 0;
  const mine = snapOf(m);
  for (let r = 1; r < 32; r++) {
    const a = mine.gpr[r]!, b = exp.gpr[r]!;
    if (a.some((x, i) => x !== b[i])) { regDiffs++; lines.push(`  gpr[${r}] oracle=${a.map((x) => hex(x)).join(':')} ours=${b.map((x) => hex(x)).join(':')}`); }
  }
  for (const k of ['hi', 'lo', 'hi1', 'lo1'] as const) if (mine[k] !== exp[k]) { regDiffs++; lines.push(`  ${k} oracle=${mine[k]} ours=${exp[k]}`); }
  for (let i = 0; i < 32; i++) if (mine.fpr[i] !== exp.fpr[i]) { regDiffs++; lines.push(`  f${i} oracle=${hex(mine.fpr[i]!)} (${fstr(mine.fpr[i]!)}) ours=${hex(exp.fpr[i]!)} (${fstr(exp.fpr[i]!)}) ulp=${ulp(mine.fpr[i]!, exp.fpr[i]!)}`); }
  if (mine.facc !== exp.facc) { regDiffs++; lines.push(`  facc oracle=${fstr(mine.facc)} ours=${fstr(exp.facc)}`); }
  if (mine.fcr31 !== exp.fcr31) { regDiffs++; lines.push(`  fcr31 oracle=${hex(mine.fcr31)} ours=${hex(exp.fcr31)}`); }
  for (let r = 0; r < 32; r++) {
    const a = mine.vf[r]!, b = exp.vf[r]!;
    if (a.some((x, i) => x !== b[i])) { regDiffs++; lines.push(`  vf${r} oracle=(${a.map(fstr).join(', ')}) ours=(${b.map(fstr).join(', ')})`); }
  }
  if (mine.vacc.some((x, i) => x !== exp.vacc[i])) { regDiffs++; lines.push(`  vacc oracle=(${mine.vacc.map(fstr).join(', ')}) ours=(${exp.vacc.map(fstr).join(', ')})`); }
  if (mine.vi.some((x, i) => x !== exp.vi[i])) { regDiffs++; lines.push(`  vi oracle=${mine.vi.join(',')} ours=${exp.vi.join(',')}`); }
  if (mine.q !== exp.q) { regDiffs++; lines.push(`  Q oracle=${fstr(mine.q)} ours=${fstr(exp.q)}`); }

  const A = new Uint32Array(m.mem.buffer), B = new Uint32Array(expRam.buffer, expRam.byteOffset, expRam.byteLength / 4), E = new Uint32Array(enterRam.buffer, enterRam.byteOffset, enterRam.byteLength / 4);
  const ranges: [number, number][] = [];
  for (let w = 0; w < A.length; w++) {
    if (A[w] !== B[w]) {
      const ad = w * 4;
      const last = ranges[ranges.length - 1];
      if (last && ad - last[1] <= 16) last[1] = ad + 4; else ranges.push([ad, ad + 4]);
    }
  }
  let maxUlp = 0;
  for (const [s, e] of ranges) {
    let rmax = 0;
    for (let a = s; a < e; a += 4) if (A[a / 4] !== B[a / 4]) rmax = Math.max(rmax, ulp(A[a / 4]!, B[a / 4]!));
    maxUlp = Math.max(maxUlp, rmax);
    lines.push(`  RAM ${hex(s)}..${hex(e - 1)} (${e - s} bytes, max ulp ${rmax})`);
    let shown = 0;
    for (let a = s; a < e && shown < maxWords; a += 4) {
      const i = a / 4;
      if (A[i] === B[i]) continue;
      shown++;
      lines.push(`    ${hex(a)}: enter=${hex(E[i]!)} (${fstr(E[i]!)})  ours=${hex(B[i]!)} (${fstr(B[i]!)})  oracle=${hex(A[i]!)} (${fstr(A[i]!)})`);
    }
  }
  return { lines, ramRanges: ranges.length, regDiffs, maxUlp };
}

function parseArgs(argv: string[]) {
  const o: Record<string, string | boolean> = {};
  const pos: string[] = [];
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i]!;
    if (a.startsWith('--')) {
      const k = a.slice(2);
      if (['calls', 'stubcmp', 'quiet', 'ignore-below-sp', 'ram-only'].includes(k)) o[k] = true; else o[k] = argv[++i]!;
    } else pos.push(a);
  }
  return { o, pos };
}

function fmtCall(c: CallRecord): string {
  return `${'  '.repeat(c.depth)}#${c.index} ${hex(c.target, 6)}${c.stub ? ` [HLE:${c.stub}]` : ''} a0=${hex(c.args[0]!)} a1=${hex(c.args[1]!)} a2=${hex(c.args[2]!)} a3=${hex(c.args[3]!)} f12=${fstr(c.fargs[0]!)} ra=${hex(c.ra, 6)}`
    + (c.retSteps !== undefined ? ` -> v0=${hex(c.v0!)} f0=${fstr(c.f0!)}` : '');
}

/** Compare guest body vs TS stub port on identical state; returns diff lines. */
export function stubCompare(m: EeOracle, rec: CallRecord, stubs: ReturnType<typeof makeStubs>): string[] {
  const stub = stubs.get(rec.stub!);
  if (!stub) return [`    (no TS port for ${rec.stub})`];
  const g = cloneOracle(m), s = cloneOracle(m);
  g.calls.length = 0;
  g.run(rec.ra, rec.sp, 2_000_000);
  stub(s);
  const out: string[] = [];
  const A = new Uint32Array(g.mem.buffer), B = new Uint32Array(s.mem.buffer);
  for (let w = 0; w < A.length; w++) if (A[w] !== B[w]) {
    const u = ulp(A[w]!, B[w]!);
    out.push(`    mem ${hex(w * 4)}: guest=${hex(A[w]!)} (${fstr(A[w]!)}) stub=${hex(B[w]!)} (${fstr(B[w]!)}) ulp=${u}`);
  }
  if (g.fpr[0] !== s.fpr[0] && /sin|cos|Inner/.test(rec.stub!)) out.push(`    f0: guest=${fstr(g.fpr[0]!)} stub=${fstr(s.fpr[0]!)} ulp=${ulp(g.fpr[0]!, s.fpr[0]!)}`);
  if (/rem_pio2/.test(rec.stub!) && g.u32(2) !== s.u32(2)) out.push(`    v0: guest=${g.s32(2)} stub=${s.s32(2)}`);
  return out;
}

function main(): void {
  const { o, pos } = parseArgs(process.argv.slice(2));
  const prefix = path.resolve(pos[0] ?? 'work/explore/snap/21f540_200');
  const fpu = (o.fpu as Rounding) ?? 'rz', vu = (o.vu as Rounding) ?? 'rz';
  const mode = (o.mode as string) ?? 'guest';
  const { m, snap } = loadOracle(prefix, { fpu, vu });
  m.stubNames = loadStubMap();
  const fixList = String(o.fix ?? '').split(',');
  const stubs = makeStubs(fpu, { mulMatrix: fixList.includes('mulmatrix'), normalize: fixList.includes('normalize') });
  const entryRa = snap.gpr[31]![0]!, entrySp = snap.gpr[29]![0]!;
  console.log(`[oracle] ${path.basename(prefix)} entry pc=${hex(snap.pc, 6)} ra=${hex(entryRa, 6)} sp=${hex(entrySp)} mode=${mode} fpu=${fpu} vu=${vu}`);

  if (mode === 'stub') {
    for (const [addr, name] of m.stubNames) {
      const fn = stubs.get(name);
      m.hooks.set(addr, (mm) => {
        if (!fn) throw new Error(`stub mode: no TS port for HLE ${name} at ${hex(addr, 6)}`);
        fn(mm); return true;
      });
    }
  }
  const stubLog: string[] = [];
  if (o.stubcmp) {
    m.onCall = (rec, mm) => {
      if (!rec.stub) return;
      const d = stubCompare(mm, rec, stubs);
      const maxU = d.reduce((x, l) => { const q = /ulp=(\d+)/.exec(l); return Math.max(x, q ? Number(q[1]) : 1e9); }, 0);
      stubLog.push(`  call #${rec.index} ${rec.stub} a0=${hex(rec.args[0]!)} a1=${hex(rec.args[1]!)} a2=${hex(rec.args[2]!)} f12=${fstr(rec.fargs[0]!)}: ${d.length === 0 ? 'identical' : `${d.length} diffs, max ulp ${maxU}`}`);
      if (d.length && maxU > 4) stubLog.push(...d.slice(0, 24));
    };
  }
  if (o.watch) {
    const [a, l] = (o.watch as string).split(':');
    const lo = parseInt(a!, 16), hi = lo + parseInt(l ?? '16', 0);
    m.writeLog = (addr, size, pc) => {
      if (addr + size > lo && addr < hi) {
        const v = size === 4 ? fstr(m.view.getUint32(addr, true)) : '';
        stubLog.push(`  write ${hex(addr)} size ${size} at pc ${hex(pc, 6)} (pre-store value ${v})`);
      }
    };
  }
  let stopPc = entryRa, stopSp: number | undefined = entrySp;
  if (o['stop-pc']) {
    const target = parseInt(o['stop-pc'] as string, 16), hit = Number(o['stop-hit'] ?? 1);
    let n = 0;
    m.observers.set(target, [(mm) => { if (++n === hit) throw Object.assign(new Error('stop'), { stopAt: true }); }]);
  }
  try {
    m.run(stopPc, stopSp, Number(o.max ?? 20_000_000));
  } catch (e) {
    const err = e as Error & { stopAt?: boolean };
    if (!err.stopAt) {
      console.log(`[oracle] FAULT after ${m.steps} steps: ${err.message}`);
      for (const [k, v] of m.unsupported) console.log(`  unsupported: ${k} x${v}`);
      if (o.calls) for (const c of m.calls) console.log(fmtCall(c));
      process.exitCode = 2;
      return;
    }
    console.log(`[oracle] stopped at ${o['stop-pc']} after ${m.steps} steps`);
    if (o.dump) writeSnap(o.dump as string, m);
    return;
  }
  console.log(`[oracle] returned to ${hex(entryRa, 6)} after ${m.steps} steps, ${m.calls.length} calls, scratchpad touched=${m.sprTouched}`);
  if (m.events.length) console.log(`[oracle] events:\n  ${m.events.slice(0, 40).join('\n  ')}`);
  if (o.calls) for (const c of m.calls) console.log(fmtCall(c));
  if (stubLog.length) console.log(`[oracle] stub comparisons / watch:\n${stubLog.join('\n')}`);
  const stubCalls = m.calls.filter((c) => c.stub);
  const hist = new Map<string, number>();
  for (const c of stubCalls) hist.set(c.stub!, (hist.get(c.stub!) ?? 0) + 1);
  console.log(`[oracle] calls into functions our build replaces with HLE stubs: ${[...hist].map(([k, v]) => `${k} x${v}`).join(', ') || 'none'}`);
  if (o.dump) writeSnap(o.dump as string, m);
  const expRam = fs.readFileSync(`${prefix}_exit.ram`), enterRam = fs.readFileSync(`${prefix}_enter.ram`);
  const exp = loadSnapJson(`${prefix}_exit.json`);
  let against = new Uint8Array(expRam.buffer, expRam.byteOffset, expRam.byteLength), againstSnap = exp, label = 'OUR exit snapshot';
  if (o.against) { // diff against another oracle dump (e.g. guest-mode run) instead of our exit snapshot
    const r = fs.readFileSync(`${o.against}.ram`); against = new Uint8Array(r.buffer, r.byteOffset, r.byteLength);
    againstSnap = loadSnapJson(`${o.against}.json`); label = `${o.against} (labelled "ours" below)`;
  }
  if (o['ignore-below-sp']) { // dead stack below the entry sp is callee scratch: blank it on both sides
    const lo = entrySp - 0x10000;
    against = against.slice(); against.fill(0, lo, entrySp); m.mem.fill(0, lo, entrySp);
  }
  const d = diffState(m, against, againstSnap, new Uint8Array(enterRam.buffer, enterRam.byteOffset, enterRam.byteLength), Number(o['max-words'] ?? 24));
  console.log(`[oracle] diff vs ${label}: ${d.regDiffs} register diffs, ${d.ramRanges} RAM ranges (max ulp ${d.maxUlp})`);
  console.log(o['ram-only'] ? d.lines.filter((l) => /^ {2}RAM| {4}[0-9a-f]{8}:/.test(l)).join('\n') : d.lines.join('\n'));
}

if (process.argv[1] && path.resolve(process.argv[1]) === path.resolve(new URL(import.meta.url).pathname)) main();
