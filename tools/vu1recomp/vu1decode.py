#!/usr/bin/env python3
"""VU1 micro instruction decoder for the static recompiler.

This is a line-by-line port of the PS2Recomp-vu1 interpreter's decoder
(ps2xRuntime/src/lib/vu/ps2_vu1_core.cpp: decodeUpperUsage / decodeLowerUsage /
decodeInstructionPair) plus the opcode selection of ps2_vu1_upper.cpp and
ps2_vu1_lower.cpp.  The interpreter is the semantics oracle, so every latency,
read/write set and quirk here mirrors it exactly.  Anything the interpreter
treats as reserved is reported as reserved; the generator refuses to emit code
that reaches it.

Byte PCs are used throughout (pair index * 8), like the interpreter.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field
from typing import List, Optional, Tuple

FMAC_LATENCY = 4
ACC_FORWARD_LATENCY = 1

# Pipelines (mirror VU1Interpreter::Pipeline)
P_NONE, P_FMAC, P_LSU, P_FDIV, P_EFU, P_IALU, P_BRANCH, P_XGKICK = range(8)


def lane_for_component(c: int) -> int:
    return 1 << (3 - c)


def DEST(i):
    return (i >> 21) & 0xF


def FT(i):
    return (i >> 16) & 0x1F


def FS(i):
    return (i >> 11) & 0x1F


def FD(i):
    return (i >> 6) & 0x1F


def VIT(i):
    return (i >> 16) & 0xF


def VIS(i):
    return (i >> 11) & 0xF


def VID(i):
    return (i >> 6) & 0xF


def IMM11(i):
    v = i & 0x7FF
    return v - 0x800 if v & 0x400 else v


def IMM15_IADDIU(i):
    # interpreter: (int16_t)(instr & 0x7FF) | ((instr >> 10) & 0x7800)
    v = (i & 0x7FF) | ((i >> 10) & 0x7800)
    return v  # 15-bit unsigned, always positive as int16


def IMM5(i):
    v = (i >> 6) & 0x1F
    return v - 0x20 if v & 0x10 else v


def IMM12(i):
    return (((i >> 21) & 1) << 11) | (i & 0x7FF)


@dataclass
class Usage:
    vf_read: List[Tuple[int, int]] = field(default_factory=list)  # (reg, lanes)
    vf_write: Tuple[int, int] = (0, 0)
    vi_read: int = 0
    vi_write: int = 0
    acc_read: int = 0
    acc_write: int = 0
    latency: int = 0
    vf_latency: int = 0
    vi_latency: int = 0
    pipeline: int = P_NONE
    wait_q: bool = False
    wait_p: bool = False
    reads_clip: bool = False
    writes_clip: bool = False
    delays_next_branch_read: bool = False
    reserved: bool = False

    def add_vf_read(self, reg, lanes):
        if lanes == 0:
            return
        for idx, (r, l) in enumerate(self.vf_read):
            if r == reg:
                self.vf_read[idx] = (r, l | lanes)
                return
        if len(self.vf_read) < 2:
            self.vf_read.append((reg, lanes))

    def add_vf_write(self, reg, lanes):
        if reg == 0 or lanes == 0:
            return
        if self.vf_write[0] == 0:
            self.vf_write = (reg, lanes)
        elif self.vf_write[0] == reg:
            self.vf_write = (reg, self.vf_write[1] | lanes)

    def vf_read_lanes(self, reg):
        for r, l in self.vf_read:
            if r == reg:
                return l
        return 0

    def eff_vf_latency(self):
        return self.vf_latency if self.vf_latency else self.latency

    def eff_vi_latency(self):
        return self.vi_latency if self.vi_latency else self.latency


PRODUCT_SUM_OPS = {0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
                   0x21, 0x23, 0x25, 0x27, 0x29, 0x2D, 0x2E}


def decode_upper_usage(upper: int) -> Usage:
    u = Usage(pipeline=P_FMAC, latency=FMAC_LATENCY)
    op = upper & 0x3F
    dest = DEST(upper)
    fs, ft, fd = FS(upper), FT(upper), FD(upper)
    if op <= 0x2F:
        u.add_vf_read(fs, dest)
        u.add_vf_write(fd, dest)
        if op <= 0x1B:
            u.add_vf_read(ft, lane_for_component(op & 3))
        elif op >= 0x28:
            u.add_vf_read(ft, 0xE if op == 0x2E else dest)
        if op in PRODUCT_SUM_OPS:
            u.acc_read = dest
        return u
    if op >= 0x3C:
        special = (upper & 3) | ((upper >> 4) & 0x7C)
        writes_acc = (special <= 0x0F or (0x18 <= special <= 0x1C) or special == 0x1E
                      or (0x20 <= special <= 0x2A) or (0x2C <= special <= 0x2E))
        if writes_acc:
            u.add_vf_read(fs, dest)
            if special <= 0x1B:
                u.add_vf_read(ft, lane_for_component(special & 3))
            elif 0x28 <= special <= 0x2E:
                u.add_vf_read(ft, 0xE if special == 0x2E else dest)
            u.acc_write = dest
            if ((0x08 <= special <= 0x0F) or special in (0x21, 0x23, 0x25, 0x27, 0x29, 0x2D)):
                u.acc_read = dest
        elif 0x10 <= special <= 0x17:
            u.add_vf_read(fs, dest)
            u.add_vf_write(ft, dest)
        elif special == 0x1D:
            u.add_vf_read(fs, dest)
            u.add_vf_write(ft, dest)
        elif special == 0x1F:
            u.add_vf_read(fs, 0xE)
            u.add_vf_read(ft, 0x1)
            u.writes_clip = True
        elif special not in (0x2F, 0x30):
            u.reserved = True
        return u
    u.reserved = True
    return u


def decode_lower_usage(lower: int) -> Usage:
    u = Usage()
    if lower == 0 or lower == 0x8000033C:
        return u
    op_hi = (lower >> 25) & 0x7F
    vfT, vfS = FT(lower), FS(lower)
    viT, viS, viD = VIT(lower), VIS(lower), VID(lower)
    dest = DEST(lower)

    def read_vi(r):
        if r:
            u.vi_read |= 1 << r

    def write_vi(r):
        if r:
            u.vi_write |= 1 << r

    if op_hi == 0x00:
        u.pipeline, u.latency = P_LSU, 4
        read_vi(viS); u.add_vf_write(vfT, dest); return u
    if op_hi == 0x01:
        u.pipeline, u.latency = P_LSU, 1
        read_vi(viT); u.add_vf_read(vfS, dest); return u
    if op_hi == 0x04:
        u.pipeline, u.latency = P_LSU, 4
        read_vi(viS); write_vi(viT); return u
    if op_hi == 0x05:
        u.pipeline, u.latency = P_LSU, 1
        read_vi(viS); read_vi(viT); return u
    if op_hi in (0x08, 0x09):
        u.pipeline, u.latency = P_IALU, 1
        u.delays_next_branch_read = True
        read_vi(viS); write_vi(viT); return u
    if op_hi in (0x10, 0x12, 0x13):
        u.pipeline, u.latency = P_IALU, 1
        u.reads_clip = True
        write_vi(1); return u
    if op_hi == 0x11:
        u.pipeline, u.latency = P_FMAC, FMAC_LATENCY
        u.writes_clip = True; return u
    if op_hi in (0x14, 0x16, 0x17):
        u.pipeline, u.latency = P_IALU, 1
        write_vi(viT); return u
    if op_hi == 0x15:
        u.pipeline, u.latency = P_FMAC, FMAC_LATENCY
        return u
    if op_hi in (0x18, 0x1A, 0x1B):
        u.pipeline, u.latency = P_IALU, 1
        read_vi(viS); write_vi(viT); return u
    if op_hi == 0x1C:
        u.pipeline, u.latency = P_IALU, 1
        u.reads_clip = True
        write_vi(viT); return u
    if op_hi == 0x20:
        u.pipeline = P_BRANCH; return u
    if op_hi == 0x21:
        u.pipeline, u.latency = P_BRANCH, 1
        write_vi(viT); return u
    if op_hi == 0x24:
        u.pipeline = P_BRANCH
        read_vi(viS); return u
    if op_hi == 0x25:
        u.pipeline, u.latency = P_BRANCH, 1
        read_vi(viS); write_vi(viT); return u
    if op_hi in (0x28, 0x29):
        u.pipeline = P_BRANCH
        read_vi(viS); read_vi(viT); return u
    if op_hi in (0x2C, 0x2D, 0x2E, 0x2F):
        u.pipeline = P_BRANCH
        read_vi(viS); return u
    if op_hi != 0x40:
        u.reserved = True; return u

    direct = lower & 0x3F
    if direct in (0x30, 0x31, 0x34, 0x35):
        u.pipeline, u.latency = P_IALU, 1
        u.delays_next_branch_read = True
        read_vi(viS); read_vi(viT); write_vi(viD); return u
    if direct == 0x32:
        u.pipeline, u.latency = P_IALU, 1
        u.delays_next_branch_read = True
        read_vi(viS); write_vi(viT); return u
    if direct < 0x3C:
        u.reserved = True; return u

    special = (lower & 3) | ((lower >> 4) & 0x7C)
    if special in (0x30, 0x31):
        u.pipeline, u.latency = P_FMAC, 4
        u.add_vf_read(vfS, 0xF if special == 0x31 else dest)
        u.add_vf_write(vfT, dest)
    elif special in (0x34, 0x36):
        u.pipeline, u.latency, u.vi_latency = P_LSU, 4, 1
        u.delays_next_branch_read = True
        read_vi(viS); write_vi(viS); u.add_vf_write(vfT, dest)
    elif special in (0x35, 0x37):
        u.pipeline, u.latency = P_LSU, 1
        u.delays_next_branch_read = True
        read_vi(viT); write_vi(viT); u.add_vf_read(vfS, dest)
    elif special == 0x38:
        u.pipeline, u.latency = P_FDIV, 7
        u.add_vf_read(vfS, lane_for_component((lower >> 21) & 3))
        u.add_vf_read(vfT, lane_for_component((lower >> 23) & 3))
    elif special == 0x39:
        u.pipeline, u.latency = P_FDIV, 7
        u.add_vf_read(vfT, lane_for_component((lower >> 23) & 3))
    elif special == 0x3A:
        u.pipeline, u.latency = P_FDIV, 13
        u.add_vf_read(vfS, lane_for_component((lower >> 21) & 3))
        u.add_vf_read(vfT, lane_for_component((lower >> 23) & 3))
    elif special == 0x3B:
        u.pipeline = P_FDIV
        u.wait_q = True
    elif special == 0x3C:
        u.pipeline, u.latency = P_IALU, 1
        u.delays_next_branch_read = True
        u.add_vf_read(vfS, lane_for_component((lower >> 21) & 3))
        write_vi(viT)
    elif special == 0x3D:
        u.pipeline, u.latency = P_FMAC, 4
        read_vi(viS); u.add_vf_write(vfT, dest)
    elif special == 0x3E:
        u.pipeline, u.latency = P_LSU, 4
        read_vi(viS); write_vi(viT)
    elif special == 0x3F:
        u.pipeline, u.latency = P_LSU, 1
        read_vi(viS); read_vi(viT)
    elif special in (0x40, 0x41):
        u.pipeline, u.latency = P_FMAC, 4
        u.add_vf_write(vfT, dest)
    elif special in (0x42, 0x43):
        u.pipeline, u.latency = P_IALU, 1
        u.add_vf_read(vfS, lane_for_component((lower >> 21) & 3))
    elif special == 0x64:
        u.pipeline, u.latency = P_FMAC, 4
        u.add_vf_write(vfT, dest)
    elif special in (0x68, 0x69):
        u.pipeline, u.latency = P_IALU, 1
        write_vi(viT)
    elif special == 0x6C:
        u.pipeline, u.latency = P_XGKICK, 2
        read_vi(viS)
    elif special in (0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x7C, 0x7D):
        u.pipeline = P_EFU
        u.latency = {0x70: 11, 0x71: 18, 0x72: 18, 0x77: 18, 0x73: 24, 0x74: 54, 0x75: 54,
                     0x7C: 54, 0x76: 12, 0x78: 12, 0x7A: 12, 0x79: 29, 0x7D: 44}[special]
        if 0x70 <= special <= 0x73:
            u.add_vf_read(vfS, 0xE)
        elif special == 0x74:
            u.add_vf_read(vfS, 0xC)
        elif special == 0x75:
            u.add_vf_read(vfS, 0xA)
        elif special == 0x76:
            u.add_vf_read(vfS, 0xF)
        else:
            u.add_vf_read(vfS, lane_for_component((lower >> 21) & 3))
    elif special == 0x7B:
        u.pipeline = P_EFU
        u.wait_p = True
    else:
        u.reserved = True
    return u


# ---------------------------------------------------------------------------
# Mnemonics (for comments / reports only)
# ---------------------------------------------------------------------------
_BC = "xyzw"
_UPPER = {}
for _i, _n in enumerate(["ADD", "SUB", "MADD", "MSUB", "MAX", "MINI", "MUL"]):
    for _c in range(4):
        _UPPER[_i * 4 + _c] = _n + _BC[_c]
_UPPER.update({0x1C: "MULq", 0x1D: "MAXi", 0x1E: "MULi", 0x1F: "MINIi", 0x20: "ADDq", 0x21: "MADDq",
               0x22: "ADDi", 0x23: "MADDi", 0x24: "SUBq", 0x25: "MSUBq", 0x26: "SUBi", 0x27: "MSUBi",
               0x28: "ADD", 0x29: "MADD", 0x2A: "MUL", 0x2B: "MAX", 0x2C: "SUB", 0x2D: "MSUB",
               0x2E: "OPMSUB", 0x2F: "MINI"})
_USPEC = {}
for _i, _n in enumerate(["ADDA", "SUBA", "MADDA", "MSUBA"]):
    for _c in range(4):
        _USPEC[_i * 4 + _c] = _n + _BC[_c]
for _c, _n in enumerate(["ITOF0", "ITOF4", "ITOF12", "ITOF15"]):
    _USPEC[0x10 + _c] = _n
for _c, _n in enumerate(["FTOI0", "FTOI4", "FTOI12", "FTOI15"]):
    _USPEC[0x14 + _c] = _n
for _c in range(4):
    _USPEC[0x18 + _c] = "MULA" + _BC[_c]
_USPEC.update({0x1C: "MULAq", 0x1D: "ABS", 0x1E: "MULAi", 0x1F: "CLIP", 0x20: "ADDAq", 0x21: "MADDAq",
               0x22: "ADDAi", 0x23: "MADDAi", 0x24: "SUBAq", 0x25: "MSUBAq", 0x26: "SUBAi", 0x27: "MSUBAi",
               0x28: "ADDA", 0x29: "MADDA", 0x2A: "MULA", 0x2C: "SUBA", 0x2D: "MSUBA", 0x2E: "OPMULA",
               0x2F: "NOP", 0x30: "NOP"})
_LHI = {0x00: "LQ", 0x01: "SQ", 0x04: "ILW", 0x05: "ISW", 0x08: "IADDIU", 0x09: "ISUBIU", 0x10: "FCEQ",
        0x11: "FCSET", 0x12: "FCAND", 0x13: "FCOR", 0x14: "FSEQ", 0x15: "FSSET", 0x16: "FSAND", 0x17: "FSOR",
        0x18: "FMEQ", 0x1A: "FMAND", 0x1B: "FMOR", 0x1C: "FCGET", 0x20: "B", 0x21: "BAL", 0x24: "JR",
        0x25: "JALR", 0x28: "IBEQ", 0x29: "IBNE", 0x2C: "IBLTZ", 0x2D: "IBGTZ", 0x2E: "IBLEZ", 0x2F: "IBGEZ"}
_LDIR = {0x30: "IADD", 0x31: "ISUB", 0x32: "IADDI", 0x34: "IAND", 0x35: "IOR"}
_LSPEC = {0x30: "MOVE", 0x31: "MR32", 0x34: "LQI", 0x35: "SQI", 0x36: "LQD", 0x37: "SQD", 0x38: "DIV",
          0x39: "SQRT", 0x3A: "RSQRT", 0x3B: "WAITQ", 0x3C: "MTIR", 0x3D: "MFIR", 0x3E: "ILWR", 0x3F: "ISWR",
          0x40: "RNEXT", 0x41: "RGET", 0x42: "RINIT", 0x43: "RXOR", 0x64: "MFP", 0x68: "XTOP", 0x69: "XITOP",
          0x6C: "XGKICK", 0x70: "ESADD", 0x71: "ERSADD", 0x72: "ELENG", 0x73: "ERLENG", 0x74: "EATANxy",
          0x75: "EATANxz", 0x76: "ESUM", 0x77: "ERSQRT", 0x78: "ESQRT", 0x79: "ESIN", 0x7A: "ERCPR",
          0x7B: "WAITP", 0x7C: "EATAN", 0x7D: "EEXP"}


def dest_str(d):
    return "".join(c for b, c in ((8, "x"), (4, "y"), (2, "z"), (1, "w")) if d & b)


def upper_name(upper: int) -> str:
    op = upper & 0x3F
    if op >= 0x3C:
        sp = (upper & 3) | ((upper >> 4) & 0x7C)
        return _USPEC.get(sp, "?upper")
    return _UPPER.get(op, "?upper")


def lower_name(lower: int) -> str:
    if lower in (0, 0x8000033C):
        return "NOP"
    hi = (lower >> 25) & 0x7F
    if hi != 0x40:
        return _LHI.get(hi, "?lower")
    d = lower & 0x3F
    if d < 0x3C:
        return _LDIR.get(d, "?lower")
    sp = (lower & 3) | ((lower >> 4) & 0x7C)
    return _LSPEC.get(sp, "?lower")


def upper_special(upper: int) -> Optional[int]:
    op = upper & 0x3F
    return (upper & 3) | ((upper >> 4) & 0x7C) if op >= 0x3C else None


def lower_special(lower: int) -> Optional[int]:
    if ((lower >> 25) & 0x7F) != 0x40 or (lower & 0x3F) < 0x3C:
        return None
    return (lower & 3) | ((lower >> 4) & 0x7C)


@dataclass
class Pair:
    pc: int
    lower: int
    upper: int
    ibit: bool
    ebit: bool
    mbit: bool
    dbit: bool
    tbit: bool
    uu: Usage
    lu: Usage
    upper_vf_shadow_reg: int = 0
    suppressed_lower_vf: int = 0

    @property
    def reserved(self):
        return self.uu.reserved or self.lu.reserved

    def text(self) -> str:
        un = upper_name(self.upper)
        u = f"{un}.{dest_str(DEST(self.upper))}" if un not in ("NOP",) else "NOP"
        if self.ibit:
            f = struct.unpack("<f", struct.pack("<I", self.lower))[0]
            l = f"LOI {f:.9g}"
        else:
            ln = lower_name(self.lower)
            l = ln
            if ln in ("B", "BAL", "IBEQ", "IBNE", "IBLTZ", "IBGTZ", "IBLEZ", "IBGEZ"):
                l += f" ->0x{(self.pc + 8 + IMM11(self.lower) * 8) & 0x3FFF:04x}"
        bits = "".join(c for f, c in ((self.ibit, "I"), (self.ebit, "E"), (self.mbit, "M"),
                                      (self.dbit, "D"), (self.tbit, "T")) if f)
        return f"{'[' + bits + '] ' if bits else ''}{u} | {l} ({self.lower:08x} {self.upper:08x})"


def decode_pair(code: bytes, pc: int) -> Pair:
    lower, upper = struct.unpack_from("<II", code, pc)
    p = Pair(pc=pc, lower=lower, upper=upper,
             ibit=bool(upper & 0x80000000), ebit=bool(upper & 0x40000000),
             mbit=bool(upper & 0x20000000), dbit=bool(upper & 0x10000000),
             tbit=bool(upper & 0x08000000),
             uu=decode_upper_usage(upper), lu=Usage())
    if not p.ibit:
        p.lu = decode_lower_usage(lower)
    uw = p.uu.vf_write[0]
    if uw != 0 and (p.lu.vf_read_lanes(uw) != 0 or p.lu.vf_write[0] == uw):
        p.upper_vf_shadow_reg = uw
        if p.lu.vf_write[0] == uw:
            p.suppressed_lower_vf = uw
    return p


def fnv1a64(data: bytes) -> int:
    h = 1469598103934665603
    for b in data:
        h ^= b
        h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h
