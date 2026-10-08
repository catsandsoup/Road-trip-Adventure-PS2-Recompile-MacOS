#!/usr/bin/env python3
"""
VU0 libvu0 HLE-stub audit for Road Trip Adventure (SLES-51356).

work/rta.toml lists the game's libvu0 routines (0x275770..0x2763C8) under
`stubs`, so the recompiler emits a trampoline into hand-written C++ in
PS2Recomp/ps2xRuntime/src/lib/Kernel/Stubs/VU.cpp instead of translating the
guest code.  This script

  1. executes the ORIGINAL guest machine code of each routine (read from a RAM
     snapshot) on a small R5900/VU0-macro interpreter written from the VU
     User's Manual / PCSX2 VUops.cpp semantics, and compares it with a faithful
     Python port of the corresponding VU.cpp stub on random inputs;
  2. re-checks the stub conclusion against the full-state snapshots in
     work/explore/snap/ (camera builder 0x220458 and the race call of 0x21F540);
  3. replays the race camera chain 0x220348 -> 0x220458 with stub semantics
     (must reproduce exit RAM 0x1823EF0/0x1823F30/0x1823FB0) and with guest
     semantics (shows what the matrices should be);
  4. demonstrates the macro-mode flag idiom used by the floor builder 0x21E320
     (ctc2 vi16; vsub.xyw vf0,...; cfc2 vi16) under hardware vs generated-code
     semantics.

Run:  python3 game/tests/oracle/vu0_stub_audit.py   (from the project root, the parent of game/)
No third-party modules needed.
"""
import json, math, os, random, struct, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..'))
SNAP = os.path.join(ROOT, 'work', 'explore', 'snap')


def f32(x):
    try:
        return struct.unpack('<f', struct.pack('<f', x))[0]
    except OverflowError:
        return math.copysign(3.4028234663852886e38, x)


def bits(x):
    return struct.unpack('<I', struct.pack('<f', f32(x)))[0]


def fbits(u):
    return struct.unpack('<f', struct.pack('<I', u & 0xffffffff))[0]


FMAX = fbits(0x7f7fffff)

# ----------------------------------------------------------------------------
# Minimal R5900 + VU0 macro interpreter (only what the libvu0 leaf routines use)
# ----------------------------------------------------------------------------
M64 = (1 << 64) - 1
M128 = (1 << 128) - 1


class CPU:
    def __init__(self, ram):
        self.mem = bytearray(ram)
        self.r = [0] * 32               # 128-bit GPRs as python ints
        self.f = [0.0] * 32             # FPRs
        self.fcc = False
        self.vf = [[0.0, 0.0, 0.0, 0.0] for _ in range(32)]
        self.vf[0] = [0.0, 0.0, 0.0, 1.0]
        self.acc = [0.0] * 4
        self.q = 0.0
        self.vi = [0] * 16
        self.status = 0                 # vi16
        self.mac = 0                    # vi17
        self.steps = 0

    # memory
    def rd32(self, a): return struct.unpack_from('<I', self.mem, a & 0x1ffffff)[0]
    def wr32(self, a, v): struct.pack_into('<I', self.mem, a & 0x1ffffff, v & 0xffffffff)
    def rdq(self, a):
        a &= 0x1fffff0
        lo, hi = struct.unpack_from('<QQ', self.mem, a)
        return lo | (hi << 64)
    def wrq(self, a, v):
        a &= 0x1fffff0
        struct.pack_into('<QQ', self.mem, a, v & M64, (v >> 64) & M64)
    def rdvec(self, a): return [fbits(self.rd32(a + 4 * i)) for i in range(4)]
    def wrvec(self, a, v):
        for i in range(4):
            self.wr32(a + 4 * i, bits(v[i]))

    # gpr helpers
    def g32(self, i): return self.r[i] & 0xffffffff
    def sx(self, v): v &= 0xffffffff; return v - (1 << 32) if v & 0x80000000 else v
    def set64s(self, i, v32):       # sign-extend 32 -> 64, keep upper 64 bits
        if i == 0: return
        v = v32 & 0xffffffff
        if v & 0x80000000: v |= 0xffffffff00000000
        self.r[i] = (self.r[i] & ~M64 & M128) | v
    def set64(self, i, v):
        if i == 0: return
        self.r[i] = (self.r[i] & ~M64 & M128) | (v & M64)
    def set128(self, i, v):
        if i == 0: return
        self.r[i] = v & M128

    def vec_to_int(self, v):
        x = 0
        for k in range(4): x |= bits(v[k]) << (32 * k)
        return x
    def int_to_vec(self, x): return [fbits((x >> (32 * k)) & 0xffffffff) for k in range(4)]

    # VU flag update (PCSX2 VU_MAC_UPDATE / SYNCMSFLAGS semantics, macro mode)
    def _flags(self, res, dest):
        mac = 0
        for k in range(4):          # k: 0=x..3=w; MAC bit shift: x=3, y=2, z=1, w=0
            if not dest[k]: continue
            sh = 3 - k
            v = res[k]
            if v == 0.0: mac |= 1 << sh
            if math.copysign(1.0, v) < 0: mac |= 0x10 << sh
        new = (1 if mac & 0xf else 0) | (2 if mac & 0xf0 else 0)
        self.mac = mac
        self.status = (self.status & 0xfc0) | new | (new << 6)

    def _wr_fd(self, fd, res, dest, flags=True):
        if flags: self._flags(res, dest)
        if fd == 0: return               # VF00 is read-only: write discarded (PCSX2 RDzero)
        for k in range(4):
            if dest[k]: self.vf[fd][k] = f32(res[k])

    def run(self, pc, max_steps=100000, sentinel=0xfffffff0):
        self.r[31] = sentinel
        delay_target = None
        while True:
            if pc == sentinel: return
            self.steps += 1
            if self.steps > max_steps: raise RuntimeError('step limit')
            w = self.rd32(pc)
            nxt = self.exec(pc, w)
            if delay_target is not None:
                pc = delay_target; delay_target = None
                if nxt is not None: raise RuntimeError('branch in delay slot')
                continue
            if nxt is None:
                pc += 4
            else:
                # execute delay slot
                taken, target = nxt
                dpc = pc + 4
                pc = dpc
                delay_target = target if taken else dpc + 4

    def exec(self, pc, w):
        op = w >> 26; rs = (w >> 21) & 31; rt = (w >> 16) & 31; rd = (w >> 11) & 31
        sa = (w >> 6) & 31; fn = w & 63; imm = w & 0xffff
        simm = imm - 0x10000 if imm & 0x8000 else imm
        r = self.r
        if w == 0: return None
        if op == 0:
            if fn == 0x08: return (True, self.g32(rs))                       # jr
            if fn == 0x09: t = self.g32(rs); self.set64(rd, pc + 8); return (True, t)
            if fn == 0x2d: self.set64(rd, (r[rs] + r[rt]) & M64); return None  # daddu
            if fn == 0x21: self.set64s(rd, self.g32(rs) + self.g32(rt)); return None
            if fn == 0x25: self.set64(rd, (r[rs] | r[rt]) & M64); return None
            if fn == 0x38: self.set64(rd, ((r[rt] & M64) << sa) & M64); return None  # dsll
            if fn == 0x00: self.set64s(rd, (self.g32(rt) << sa)); return None
            if fn == 0x03: self.set64s(rd, self.sx(self.g32(rt)) >> sa); return None
            raise RuntimeError('special %#x at %#x' % (fn, pc))
        if op == 0x02: return (True, (pc & 0xf0000000) | ((w & 0x3ffffff) << 2))
        if op == 0x03: self.set64(31, pc + 8); return (True, (pc & 0xf0000000) | ((w & 0x3ffffff) << 2))
        if op == 0x04: return ((r[rs] & M64) == (r[rt] & M64), pc + 4 + 4 * simm)
        if op == 0x05: return ((r[rs] & M64) != (r[rt] & M64), pc + 4 + 4 * simm)
        if op in (0x08, 0x09): self.set64s(rt, self.g32(rs) + simm); return None
        if op == 0x0c: self.set64(rt, (r[rs] & M64) & imm); return None
        if op == 0x0d: self.set64(rt, (r[rs] & M64) | imm); return None
        if op == 0x0f: self.set64s(rt, imm << 16); return None
        a = (self.g32(rs) + simm) & 0xffffffff
        if op == 0x23: self.set64s(rt, self.rd32(a)); return None
        if op == 0x2b: self.wr32(a, self.g32(rt)); return None
        if op == 0x25: self.set64(rt, struct.unpack_from('<H', self.mem, a & 0x1ffffff)[0]); return None
        if op == 0x21: self.set64s(rt, struct.unpack_from('<h', self.mem, a & 0x1ffffff)[0]); return None
        if op == 0x37: self.set64(rt, struct.unpack_from('<Q', self.mem, a & 0x1ffffff)[0]); return None
        if op == 0x3f: struct.pack_into('<Q', self.mem, a & 0x1ffffff, r[rt] & M64); return None
        if op == 0x31: self.f[rt] = fbits(self.rd32(a)); return None
        if op == 0x39: self.wr32(a, bits(self.f[rt])); return None
        if op == 0x1e: self.set128(rt, self.rdq(a)); return None
        if op == 0x1f: self.wrq(a, r[rt]); return None
        if op == 0x36:                                                     # lqc2
            if rt: self.vf[rt] = self.int_to_vec(self.rdq(a))
            return None
        if op == 0x3e: self.wrq(a, self.vec_to_int(self.vf[rt])); return None  # sqc2
        if op == 0x1c: return self.mmi(w, rs, rt, rd, sa, fn)
        if op == 0x11: return self.cop1(pc, w, rs, rt, rd, sa, fn, simm)
        if op == 0x12: return self.cop2(w, rs, rt, rd, sa, fn)
        raise RuntimeError('op %#x at %#x (%08x)' % (op, pc, w))

    def mmi(self, w, rs, rt, rd, sa, fn):
        A = self.r[rs]; B = self.r[rt]
        wd = lambda x, i: (x >> (32 * i)) & 0xffffffff
        dw = lambda x, i: (x >> (64 * i)) & M64
        if fn == 0x08 and sa == 0x12:   # pextlw rd = {B0,A0,B1,A1}
            v = wd(B, 0) | wd(A, 0) << 32 | wd(B, 1) << 64 | wd(A, 1) << 96
        elif fn == 0x28 and sa == 0x12:  # pextuw
            v = wd(B, 2) | wd(A, 2) << 32 | wd(B, 3) << 64 | wd(A, 3) << 96
        elif fn == 0x09 and sa == 0x0e:  # pcpyld rd = {B.lo, A.lo}
            v = dw(B, 0) | dw(A, 0) << 64
        elif fn == 0x29 and sa == 0x0e:  # pcpyud rd = {A.hi, B.hi}
            v = dw(A, 1) | dw(B, 1) << 64
        else:
            raise RuntimeError('mmi %08x' % w)
        self.set128(rd, v); return None

    def cop1(self, pc, w, rs, rt, rd, sa, fn, simm):
        if rs == 0: self.set64s(rt, bits(self.f[rd])); return None
        if rs == 4: self.f[rd] = fbits(self.g32(rt)); return None
        if rs == 8:
            tf = (w >> 16) & 1
            return (self.fcc == bool(tf), pc + 4 + 4 * simm)
        if rs == 0x10:
            ft, fs, fd = rt, rd, sa
            F = self.f
            if fn == 0: F[fd] = f32(F[fs] + F[ft])
            elif fn == 1: F[fd] = f32(F[fs] - F[ft])
            elif fn == 2: F[fd] = f32(F[fs] * F[ft])
            elif fn == 3: F[fd] = f32(F[fs] / F[ft]) if F[ft] != 0 else math.copysign(FMAX, F[fs])
            elif fn == 6: F[fd] = F[fs]
            elif fn == 7: F[fd] = -F[fs]
            elif fn == 0x34: self.fcc = F[fs] < F[ft]
            else: raise RuntimeError('cop1.s %#x' % fn)
            return None
        raise RuntimeError('cop1 %08x' % w)

    def cop2(self, w, rs, rt, rd, sa, fn):
        if not (w >> 25) & 1:
            if rs == 1: self.set128(rt, self.vec_to_int(self.vf[rd])); return None   # qmfc2
            if rs == 5:                                                               # qmtc2
                if rd: self.vf[rd] = self.int_to_vec(self.r[rt])
                return None
            if rs == 2:                                                               # cfc2
                v = self.vi[rd] if rd < 16 else {16: self.status, 17: self.mac}.get(rd, 0)
                self.set64s(rt, v); return None
            if rs == 6:                                                               # ctc2
                if rd == 16: self.status = (self.g32(rt) & 0xfc0) | (self.status & 0x3f)   # PCSX2: CTC2 writes sticky bits
                elif 0 < rd < 16: self.vi[rd] = self.g32(rt) & 0xffff
                return None
            raise RuntimeError('cop2 %08x' % w)
        d = (w >> 21) & 15
        dest = [bool(d & 8), bool(d & 4), bool(d & 2), bool(d & 1)]
        ft, fs, fd = rt, rd, sa
        VF = self.vf
        S = VF[fs]; T = VF[ft]
        bc = fn & 3
        if fn < 0x3c:
            g = fn >> 2
            if fn < 0x1c:   # bc forms
                b = T[bc]
                if g == 0: res = [S[k] + b for k in range(4)]
                elif g == 1: res = [S[k] - b for k in range(4)]
                elif g == 2: res = [self.acc[k] + f32(S[k] * b) for k in range(4)]
                elif g == 3: res = [self.acc[k] - f32(S[k] * b) for k in range(4)]
                elif g == 4: res = [max(S[k], b) for k in range(4)]; self._wr_fd(fd, res, dest, False); return None
                elif g == 5: res = [min(S[k], b) for k in range(4)]; self._wr_fd(fd, res, dest, False); return None
                elif g == 6: res = [S[k] * b for k in range(4)]
                self._wr_fd(fd, res, dest); return None
            if fn == 0x1c: res = [S[k] * self.q for k in range(4)]
            elif fn == 0x20: res = [S[k] + self.q for k in range(4)]
            elif fn == 0x24: res = [S[k] - self.q for k in range(4)]
            elif fn == 0x28: res = [S[k] + T[k] for k in range(4)]
            elif fn == 0x2a: res = [S[k] * T[k] for k in range(4)]
            elif fn == 0x2c: res = [S[k] - T[k] for k in range(4)]
            elif fn == 0x29: res = [self.acc[k] + f32(S[k] * T[k]) for k in range(4)]
            elif fn == 0x2e:   # vopmsub
                p = [S[1] * T[2], S[2] * T[0], S[0] * T[1], 0.0]
                res = [self.acc[k] - f32(p[k]) for k in range(4)]
            else: raise RuntimeError('vu s1 %#x' % fn)
            self._wr_fd(fd, res, dest); return None
        op11 = (((w >> 6) & 31) << 2) | (w & 3)
        if op11 < 0x10 or 0x18 <= op11 < 0x1c:
            g = op11 >> 2; b = T[op11 & 3]
            if g == 0: res = [S[k] + b for k in range(4)]
            elif g == 1: res = [S[k] - b for k in range(4)]
            elif g == 2: res = [self.acc[k] + f32(S[k] * b) for k in range(4)]
            elif g == 3: res = [self.acc[k] - f32(S[k] * b) for k in range(4)]
            elif g == 6: res = [S[k] * b for k in range(4)]
            self._flags(res, dest)
            for k in range(4):
                if dest[k]: self.acc[k] = f32(res[k])
            return None
        if op11 == 0x2e:   # vopmula
            res = [S[1] * T[2], S[2] * T[0], S[0] * T[1], 0.0]
            self._flags(res, dest)
            for k in range(4):
                if dest[k]: self.acc[k] = f32(res[k])
            return None
        if op11 in (0x2f, 0x3b): return None                  # vnop, vwaitq
        if op11 == 0x30:                                      # vmove ft <- fs
            if ft:
                for k in range(4):
                    if dest[k]: VF[ft][k] = S[k]
            return None
        if op11 == 0x31:                                      # vmr32
            src = [S[1], S[2], S[3], S[0]]
            if ft:
                for k in range(4):
                    if dest[k]: VF[ft][k] = src[k]
            return None
        if op11 in (0x10, 0x11, 0x14, 0x15):                 # itof0/4, ftoi0/4
            out = list(VF[ft])
            for k in range(4):
                if not dest[k]: continue
                if op11 in (0x10, 0x11):
                    iv = bits(S[k]); iv = iv - (1 << 32) if iv & 0x80000000 else iv
                    out[k] = f32(iv / (16.0 if op11 == 0x11 else 1.0))
                else:
                    x = S[k] * (16.0 if op11 == 0x15 else 1.0)
                    iv = int(x) if abs(x) < 2147483647 else (2147483647 if x > 0 else -2147483648)
                    out[k] = fbits(iv & 0xffffffff)
            if ft: VF[ft] = out
            return None
        if op11 in (0x38, 0x39, 0x3a):                         # vdiv / vsqrt / vrsqrt
            fsf = (w >> 21) & 3; ftf = (w >> 23) & 3
            a = S[fsf]; b = T[ftf]
            if op11 == 0x38: self.q = f32(a / b) if b != 0 else math.copysign(FMAX, a) * math.copysign(1, b)
            elif op11 == 0x39: self.q = f32(math.sqrt(abs(b)))
            else: self.q = f32(a / math.sqrt(abs(b))) if b != 0 else FMAX
            return None
        raise RuntimeError('vu s2 %#x (%08x)' % (op11, w))


# ----------------------------------------------------------------------------
# Python ports of the VU.cpp stubs (PS2Recomp/ps2xRuntime/src/lib/Kernel/Stubs/VU.cpp)
# ----------------------------------------------------------------------------
def stub_mulVuMatrix(lhs, rhs):            # VU.cpp:77-90
    out = [0.0] * 16
    for i in range(4):
        for j in range(4):
            acc = 0.0
            for k in range(4):
                acc = f32(acc + f32(rhs[4 * k + j] * lhs[4 * i + k]))
            out[4 * i + j] = acc
    return out


def stub_rigidInverse(m):                  # VU.cpp:107-119
    out = [0.0] * 16
    for row in range(3):
        for col in range(3): out[4 * row + col] = m[4 * col + row]
        out[4 * row + 3] = 0.0
    tx, ty, tz = m[12], m[13], m[14]
    for col in range(3): out[12 + col] = f32(-(tx * m[4 * col] + ty * m[4 * col + 1] + tz * m[4 * col + 2]))
    out[15] = m[15]
    return out


def stub_MulMatrix(a1, a2): return stub_mulVuMatrix(a1, a2)   # VU.cpp:694-708 (m0=a1, m1=a2)
def stub_ApplyMatrix(m, v):                                    # VU.cpp:208-226
    return [f32(m[0 + j] * v[0] + m[4 + j] * v[1] + m[8 + j] * v[2] + m[12 + j] * v[3]) for j in range(4)]
def stub_Normalize(v):                                         # VU.cpp:726-745
    ln = math.sqrt(sum(x * x for x in v))
    return [f32(x / ln) for x in v] if ln > 1e-6 else [0.0] * 4
def stub_InnerProduct(a, b): return f32(sum(a[k] * b[k] for k in range(4)))   # VU.cpp:552-568
def stub_Invers(m): return stub_rigidInverse(m)                # VU.cpp:606-617
def stub_Transpose(m): return [m[4 * c + r] for r in range(4) for c in range(4)]
def stub_Unit(): return [1.0, 0, 0, 0, 0, 1.0, 0, 0, 0, 0, 1.0, 0, 0, 0, 0, 1.0]
def stub_RotX(m, a):
    c, s = math.cos(a), math.sin(a); r = stub_Unit(); r[5] = c; r[6] = s; r[9] = -s; r[10] = c
    return stub_mulVuMatrix(m, r)
def stub_RotY(m, a):
    c, s = math.cos(a), math.sin(a); r = stub_Unit(); r[0] = c; r[2] = -s; r[8] = s; r[10] = c
    return stub_mulVuMatrix(m, r)
def stub_RotZ(m, a):
    c, s = math.cos(a), math.sin(a); r = stub_Unit(); r[0] = c; r[1] = s; r[4] = -s; r[5] = c
    return stub_mulVuMatrix(m, r)
def stub_RotTransPers(m, v, mode):                             # VU.cpp:149-170, 791-806
    t = [m[0 + j] * v[0] + m[4 + j] * v[1] + m[8 + j] * v[2] + m[12 + j] * v[3] for j in range(4)]
    q = 1.0 / t[3] if t[3] != 0 else 0.0
    t[0] *= q; t[1] *= q; t[2] *= q
    full = (mode != 0)
    return [int(t[0] * 16), int(t[1] * 16), int(t[2] * 16) if full else int(t[2]), int(t[3] * 16) if full else int(t[3])]
def stub_DropShadow(n, lx, ly, lz, mode):                     # VU.cpp sceVu0DropShadowMatrix
    nx, ny, nz = n[0], n[1], n[2]; d = lx * nx + ly * ny + lz * nz
    if mode != 0:
        k = 1.0 - d
        return [lx * nx + k, lx * ny, lx * nz, lx, ly * nx, ly * ny + k, ly * nz, ly, lz * nx, lz * ny, lz * nz + k, lz, -nx, -ny, -nz, -d]
    k = -1.0 / d if d != 0 else 0.0
    return [k * (lx * nx - d), k * lx * ny, k * lx * nz, 0.0, k * ly * nx, k * (ly * ny - d), k * ly * nz, 0.0,
            k * lz * nx, k * lz * ny, k * (lz * nz - d), 0.0, -k * nx, -k * ny, -k * nz, 1.0]
def stub_NormalLight(l0, l1, l2):                              # VU.cpp sceVu0NormalLightMatrix
    def nn(v):
        ln = math.sqrt(sum(x * x for x in v)); inv = 1.0 / ln if ln > 1e-6 else 0.0
        return [-x * inv for x in v]
    m = nn(l0) + nn(l1) + nn(l2) + [0.0, 0.0, 0.0, 1.0]
    return stub_Transpose(m)
def stub_ClipScreen(v):                                        # VU.cpp:172-184, 324-334
    c = 0
    if v[0] > 4096: c |= 1
    if v[0] < -4096: c |= 2
    if v[1] > 4096: c |= 4
    if v[1] < -4096: c |= 8
    return c


# ----------------------------------------------------------------------------
# guest-code runners
# ----------------------------------------------------------------------------
A_OUT, A_IN1, A_IN2 = 0x01F00000, 0x01F00100, 0x01F00200


def guest_call(ram, entry, a0=A_OUT, a1=A_IN1, a2=A_IN2, a3=0, t0=0, f12=0.0, setup=None):
    c = CPU(ram) if not isinstance(ram, CPU) else ram
    c.r[4], c.r[5], c.r[6], c.r[7], c.r[8] = a0, a1, a2, a3, t0
    c.r[29] = 0x01F80000
    c.f[12] = f32(f12)
    if setup: setup(c)
    c.run(entry)
    return c


def rnd_mat(rng, rigid=False):
    if not rigid:
        return [f32(rng.uniform(-10, 10)) for _ in range(16)]
    a, b, g = rng.uniform(-3, 3), rng.uniform(-3, 3), rng.uniform(-3, 3)
    m = stub_RotZ(stub_RotY(stub_RotX(stub_Unit(), a), b), g)
    m[12], m[13], m[14] = [f32(rng.uniform(-500, 500)) for _ in range(3)]
    return m


def wrmat(c, a, m):
    for i in range(16): c.wr32(a + 4 * i, bits(m[i]))


def rdmat(c, a): return [fbits(c.rd32(a + 4 * i)) for i in range(16)]


def maxdiff(a, b): return max(abs(x - y) for x, y in zip(a, b))


def rel(a, b):
    s = max(1.0, max(abs(x) for x in list(a) + list(b)))
    return maxdiff(a, b) / s


def audit(ram):
    rng = random.Random(1234)
    print('=== 1. guest libvu0 code (from RAM) vs VU.cpp stub, random inputs ===')
    results = []

    def check(name, addr, n, fn):
        worst = 0.0; ex = None
        for _ in range(n):
            r, info = fn()
            if r > worst: worst, ex = r, info
        verdict = 'MATCH' if worst < 1e-4 else 'MISMATCH'
        results.append((name, addr, verdict, worst))
        print('%-24s %#08x  %-8s worst rel err %.3g' % (name, addr, verdict, worst))
        if verdict != 'MATCH' and ex: print('      example:', ex)

    def t_mul():
        A, B = rnd_mat(rng, True), rnd_mat(rng)
        c = CPU(ram); wrmat(c, A_IN1, A); wrmat(c, A_IN2, B); guest_call(c, 0x2757A0)
        g = rdmat(c, A_OUT); s = stub_MulMatrix(A, B)
        return rel(g, s), 'guest==a2*a1? %s  stub==a1*a2' % (rel(g, stub_mulVuMatrix(B, A)) < 1e-5)
    check('sceVu0MulMatrix', 0x2757A0, 50, t_mul)

    def t_apply():
        M = rnd_mat(rng); v = [f32(rng.uniform(-9, 9)) for _ in range(4)]
        c = CPU(ram); wrmat(c, A_IN1, M); c.wrvec(A_IN2, v); guest_call(c, 0x275770)
        return rel(c.rdvec(A_OUT), stub_ApplyMatrix(M, v)), None
    check('sceVu0ApplyMatrix', 0x275770, 50, t_apply)

    def t_norm():
        v = [f32(rng.uniform(-9, 9)) for _ in range(3)] + [rng.choice([0.0, 1.0, f32(rng.uniform(-9, 9))])]
        c = CPU(ram); c.wrvec(A_IN1, v); guest_call(c, 0x275830)
        g = c.rdvec(A_OUT); s = stub_Normalize(v)
        return rel(g, s), 'in=%s guest=%s stub=%s' % (['%.4g' % x for x in v], ['%.4g' % x for x in g], ['%.4g' % x for x in s])
    check('sceVu0Normalize', 0x275830, 50, t_norm)

    def t_inner():
        a = [f32(rng.uniform(-9, 9)) for _ in range(4)]; b = [f32(rng.uniform(-9, 9)) for _ in range(4)]
        c = CPU(ram); c.wrvec(A_OUT, a); c.wrvec(A_IN1, b); guest_call(c, 0x275808, a0=A_OUT, a1=A_IN1)
        g = c.f[0]; s = stub_InnerProduct(a, b)
        return abs(g - s) / max(1, abs(g)), 'a=%s b=%s guest(xyz only)=%.5g stub(xyzw)=%.5g' % (['%.3g' % x for x in a], ['%.3g' % x for x in b], g, s)
    check('sceVu0InnerProduct', 0x275808, 50, t_inner)

    def t_inv():
        M = rnd_mat(rng, True)
        c = CPU(ram); wrmat(c, A_IN1, M); guest_call(c, 0x2758B8)
        return rel(rdmat(c, A_OUT), stub_Invers(M)), None
    check('sceVu0InversMatrix', 0x2758B8, 50, t_inv)

    def t_tr():
        M = rnd_mat(rng)
        c = CPU(ram); wrmat(c, A_IN1, M); guest_call(c, 0x275870)
        return rel(rdmat(c, A_OUT), stub_Transpose(M)), None
    check('sceVu0TransposeMatrix', 0x275870, 20, t_tr)

    def t_unit():
        c = CPU(ram); guest_call(c, 0x275A98)
        return rel(rdmat(c, A_OUT), stub_Unit()), None
    check('sceVu0UnitMatrix', 0x275A98, 1, t_unit)

    for nm, addr, sf in (('sceVu0RotMatrixX', 0x275BE0, stub_RotX), ('sceVu0RotMatrixY', 0x275C88, stub_RotY), ('sceVu0RotMatrixZ', 0x275B38, stub_RotZ)):
        def t_rot(addr=addr, sf=sf):
            M = rnd_mat(rng, True); ang = rng.uniform(-3.1, 3.1)
            c = CPU(ram); wrmat(c, A_IN1, M); guest_call(c, addr, a0=A_OUT, a1=A_IN1, f12=ang)
            # libvu0's polynomial sin/cos is ~1e-4 accurate; allow that
            r = rel(rdmat(c, A_OUT), sf(M, ang))
            return (0.0 if r < 2e-3 else r), None
        check(nm, addr, 30, t_rot)

    for mode in (0, 1):
        def t_rtp(mode=mode):
            M = rnd_mat(rng); v = [f32(rng.uniform(-5, 5)) for _ in range(3)] + [1.0]
            M[3], M[7], M[11], M[15] = 0.01, 0.02, 0.03, 5.0
            c = CPU(ram); wrmat(c, A_IN1, M); c.wrvec(A_IN2, v); guest_call(c, 0x276270, a3=mode)
            g = [c.rd32(A_OUT + 4 * k) for k in range(4)]; g = [x - (1 << 32) if x & 0x80000000 else x for x in g]
            s = stub_RotTransPers(M, v, mode)
            return (0.0 if all(abs(a - b) <= 1 for a, b in zip(g, s)) else 1.0), 'mode=%d guest=%s stub=%s' % (mode, g, s)
        check('sceVu0RotTransPers m=%d' % mode, 0x276270, 30, t_rtp)

    for mode in (0, 1):
        def t_ds(mode=mode):
            n = [f32(rng.uniform(-1, 1)) for _ in range(3)] + [f32(rng.uniform(-50, 50))]
            L = [f32(rng.uniform(-1, 1)) for _ in range(3)]
            c = CPU(ram); c.wrvec(A_IN1, n); c.f[13] = L[1]; c.f[14] = L[2]
            guest_call(c, 0x276088, a1=A_IN1, a2=mode, f12=L[0])
            return rel(rdmat(c, A_OUT), stub_DropShadow(n, L[0], L[1], L[2], mode)), None
        check('sceVu0DropShadowMatrix m=%d' % mode, 0x276088, 30, t_ds)

    def t_nl():
        ls = [[f32(rng.uniform(-3, 3)) for _ in range(3)] + [rng.choice([0.0, 1.0])] for _ in range(3)]
        c = CPU(ram); c.wrvec(A_IN1, ls[0]); c.wrvec(A_IN2, ls[1]); c.wrvec(A_IN2 + 0x100, ls[2])
        guest_call(c, 0x275E58, a3=A_IN2 + 0x100)
        return rel(rdmat(c, A_OUT), stub_NormalLight(*ls)), 'light w=%s' % [l[3] for l in ls]
    check('sceVu0NormalLightMatrix', 0x275E58, 30, t_nl)

    def t_clip():
        v = [f32(rng.uniform(-6000, 6000)), f32(rng.uniform(-6000, 6000)), 1.0, f32(rng.uniform(-2, 2))]
        c = CPU(ram); c.wrvec(A_OUT, v); guest_call(c, 0x276320, a0=A_OUT)
        g = c.g32(2) != 0; s = stub_ClipScreen(v) != 0
        return (0.0 if g == s else 1.0), 'v=%s guest clipped=%s stub clipped=%s' % (['%.4g' % x for x in v], g, s)
    check('sceVu0ClipScreen', 0x276320, 50, t_clip)
    return results


# ----------------------------------------------------------------------------
# 2./3. snapshot checks
# ----------------------------------------------------------------------------
def m4(ram, a): return [fbits(struct.unpack_from('<I', ram, a + 4 * i)[0]) for i in range(16)]
def negy(m): return [(-x if i % 4 == 1 else x) for i, x in enumerate(m)]
def pm(tag, m):
    print('  ' + tag)
    for i in range(4): print('     [' + ', '.join('%12.6g' % m[4 * i + j] for j in range(4)) + ']')


def snap_220458():
    print('\n=== 2a. snapshot 220458 #200 (a0=0x164E350): exit RAM vs stub order vs guest order ===')
    X = open(os.path.join(SNAP, '220458_200_exit.ram'), 'rb').read()
    J = json.load(open(os.path.join(SNAP, '220458_200_enter.json')))
    a0 = J['gpr'][4][0]; sp = J['gpr'][29][0] - 0x250
    cam = m4(X, sp + 0x160); V1 = negy(stub_rigidInverse(cam))
    P = m4(X, sp + 0x50); P2 = m4(X, sp + 0x90); V2 = m4(X, sp + 0xD0)
    for off, A, B, nm in ((0x00, P2, V1, 'a0+0x00 = Mul(a1=sp+0x90, a2=V1)'), (0x40, P, V1, 'a0+0x40 = Mul(a1=sp+0x50, a2=V1)'), (0xC0, P, V2, 'a0+0xC0 = Mul(a1=sp+0x50, a2=sp+0xD0)')):
        out = m4(X, a0 + off)
        print('  %-40s rel err vs stub(a1*a2)=%.2g   vs guest(a2*a1)=%.2g' % (nm, rel(out, stub_mulVuMatrix(A, B)), rel(out, stub_mulVuMatrix(B, A))))
    print('  camera matrix sp+0x160 rows 1,2 lengths: %.4f %.4f  (row2.w=%.4f) -> non-unit axes from 4-D sceVu0Normalize stub'
          % (math.sqrt(sum(x * x for x in cam[4:7])), math.sqrt(sum(x * x for x in cam[8:11])), cam[11]))


def race_chain(E, J, sem, ram_for_guest):
    """Replay 0x220348 + 0x220458 (race path, flag 0x1000 clear) for the 21F540 #200 call.
    sem='stub' uses VU.cpp semantics, sem='guest' uses the guest libvu0 code."""
    gp = J['gpr'][28][0]
    F = lambda a: fbits(struct.unpack_from('<I', E, a)[0])
    if sem in ('stub', 'stub_fixmul'):
        Mul = stub_MulMatrix; Norm = stub_Normalize; Inv = stub_Invers; Apply = stub_ApplyMatrix
        RotX = stub_RotX; RotY = stub_RotY
    else:
        def Mul(a1, a2):
            c = CPU(ram_for_guest); wrmat(c, A_IN1, a1); wrmat(c, A_IN2, a2); guest_call(c, 0x2757A0); return rdmat(c, A_OUT)
        def Norm(v):
            c = CPU(ram_for_guest); c.wrvec(A_IN1, v); guest_call(c, 0x275830); return c.rdvec(A_OUT)
        def Inv(m):
            c = CPU(ram_for_guest); wrmat(c, A_IN1, m); guest_call(c, 0x2758B8); return rdmat(c, A_OUT)
        def Apply(m, v):
            c = CPU(ram_for_guest); wrmat(c, A_IN1, m); c.wrvec(A_IN2, v); guest_call(c, 0x275770); return c.rdvec(A_OUT)
        RotX = stub_RotX; RotY = stub_RotY   # matched guest in section 1
    if sem == 'stub_fixmul':                 # stub semantics except MulMatrix in guest order
        Mul = lambda a1, a2: stub_mulVuMatrix(a2, a1)
    def cross(a, b): return [f32(a[1] * b[2] - a[2] * b[1]), f32(a[2] * b[0] - a[0] * b[2]), f32(a[0] * b[1] - a[1] * b[0]), 0.0]
    cam = 0x1824210
    sp21f = J['gpr'][29][0] - 0xE0
    # inputs that 0x21EAC8/0x21D6A0 left in the 21F540 frame (read from exit RAM; see note)
    Min = m4(J['_X'], sp21f + 0x00); Ma3 = m4(J['_X'], sp21f + 0x40)
    # 0x208580: halfwords at cam+0x14 -> ints -> vitof4 -> * k(gp-0x7FE0)
    h = struct.unpack_from('<4h', J['_X'], cam + 0x14)
    k = F(gp - 0x7FE0)
    ax = f32(f32(h[0] / 16.0) * k); ay = f32(f32(((h[1] + h[3]) & 0xffff if ((h[1] + h[3]) & 0xffff) < 0x8000 else ((h[1] + h[3]) & 0xffff) - 0x10000) / 16.0) * k)
    R = RotY(RotX(stub_Unit(), ax), ay)            # s2 = Unit*RX*RY
    RX = RotX(stub_Unit(), ax)                     # s3
    s2 = Mul(Min, R); s3 = Mul(Ma3, RX)            # 0x2203c4 / 0x2203d4
    off = [F(cam + 4 * i) for i in range(4)]
    L0 = Apply(s2, off); L1 = s2[4:8]; L2 = s2[8:12]; L3 = s3[4:8]; L4 = s3[8:12]
    # 0x2204f8.. camera matrix
    row0 = Norm(cross(L1, L2)); row2 = Norm(L2); row1 = cross(row2, row0)
    C = row0 + row1 + row2 + [L0[0], L0[1], L0[2], 1.0]
    V1 = negy(Inv(C))
    fov = F(cam + 0x10)
    f25 = F(gp - 0x7E60) if struct.unpack_from('<I', E, gp - 0x7868)[0] else 1.0
    f24 = F(gp - 0x7E5C) if struct.unpack_from('<I', E, gp - 0x7868)[0] else F(gp - 0x7E58)
    P2 = stub_Unit(); P2[0] = f32(fov * f25); P2[5] = f32(fov * f24); P2[10] = F(gp - 0x7E54); P2[14] = F(gp - 0x7E50)
    P2[8] = F(gp - 0x3E90); P2[9] = F(gp - 0x3E8C); P2[11] = 1.0; P2[15] = 0.0
    s = f32(fov * fbits(0x3B000000))
    P = stub_Unit(); P[0] = s; P[5] = s; P[10] = F(gp - 0x7E4C); P[14] = F(gp - 0x7E48); P[11] = 1.0; P[15] = 0.0
    out00 = Mul(P2, V1); out40 = Mul(P, V1)
    r0 = Norm(cross(L3, L4)); r2 = Norm(L4); r1 = cross(r2, r0)
    V2 = negy(Inv(r0 + r1 + r2 + [0.0, 0.0, 0.0, 1.0]))
    outC0 = Mul(P, V2)
    return dict(EF0=out00, F30=out40, FB0=outC0, eye=L0[:3], cam=C, P=P, P2=P2, angles=(ax, ay))


def snap_race():
    print('\n=== 2b/3. race call 0x21F540 #200: replay 0x220348+0x220458 ===')
    E = open(os.path.join(SNAP, '21f540_200_enter.ram'), 'rb').read()
    X = open(os.path.join(SNAP, '21f540_200_exit.ram'), 'rb').read()
    J = json.load(open(os.path.join(SNAP, '21f540_200_enter.json'))); J['_X'] = X
    st = race_chain(E, J, 'stub', E)
    hw = race_chain(E, J, 'guest', E)
    print('  angles (rad) pitch=%.5f yaw=%.5f ; fov=%.1f' % (st['angles'][0], st['angles'][1], fbits(struct.unpack_from('<I', E, 0x1824220)[0])))
    for key, addr in (('EF0', 0x1823EF0), ('F30', 0x1823F30), ('FB0', 0x1823FB0)):
        ex = m4(X, addr)
        print('  %#x exit RAM: rel err vs replay(stub semantics)=%.2g   vs replay(guest semantics)=%.2g' % (addr, rel(ex, st[key]), rel(ex, hw[key])))
    pm('0x1823F30 in exit RAM (what our build produced)', m4(X, 0x1823F30))
    pm('0x1823F30 with guest libvu0 semantics (what the PS2 produces)', hw['F30'])
    pm('0x1823EF0 with guest libvu0 semantics', hw['EF0'])
    print('  camera eye: stub=%s  guest=%s' % (['%.2f' % x for x in st['eye']], ['%.2f' % x for x in hw['eye']]))
    print('  follow target translation (21F540 frame sp+0x30, from 0x21D6A0):', ['%.2f' % x for x in m4(X, J['gpr'][29][0] - 0xE0 + 0x30)[:3]])
    fx = race_chain(E, J, 'stub_fixmul', E)
    print('  stub semantics with ONLY MulMatrix operand order fixed vs guest semantics: rel err EF0=%.2g F30=%.2g FB0=%.2g'
          % (rel(fx['EF0'], hw['EF0']), rel(fx['F30'], hw['F30']), rel(fx['FB0'], hw['FB0'])))


def flag_demo():
    print('\n=== 4. floor builder 0x21E370..0x21E39C clip idiom (ctc2 zero,vi16; vsub.xyw vf0,vf12,vf0; vsub.xy vf0,vf29,vf12; cfc2 v0,vi16; andi 0xC0) ===')
    for w in (5.0, -5.0):
        c = CPU(b'\0' * 0x2000000)
        c.vf[12] = [100.0, 200.0, 1.0, w]; c.vf[29] = [4096.0, 4096.0, 0.0, 0.0]
        c.status = 0
        c.exec(0x21e374, 0x4ba0602c)    # vsub.xyw vf0, vf12, vf0
        c.exec(0x21e378, 0x4b8ce82c)    # vsub.xy  vf0, vf29, vf12
        print('  vertex w=%5.1f: hardware status&0xC0 = %#x (vf0 stays %s)   generated code: vu0_status&0xC0 = 0x0, vf0 overwritten'
              % (w, c.status & 0xC0, c.vf[0]))


if __name__ == '__main__':
    ram = open(os.path.join(SNAP, '21f540_200_enter.ram'), 'rb').read()
    audit(ram)
    snap_220458()
    snap_race()
    flag_demo()
