#!/usr/bin/env python3
"""Static recompiler for PS2 VU1 microprograms (Road Trip Adventure port).

Input : a VU1 catalog directory produced by the runtime (PS2X_VU1_CATALOG=<dir>):
        <hash>.vu1 (16 KiB micro memory image) and index.txt ("<hash> <pc> <calls>").
Output: one C++ file per image hash (vu1rec_<hash>.cpp) in the output directory,
        each registering a fast and a full-flag native implementation with the
        runtime registry (ps2xRuntime/include/runtime/ps2_vu1_recomp.h).

Semantics oracle: the PS2Recomp VU1 interpreter (ps2_vu1_core/upper/lower.cpp).

Design (see game/docs/VU1_RECOMP.md for the long version):
  * The interpreter's scheduler (scoreboard stalls, 4-cycle flag pipeline,
    FDIV/EFU latencies, VI branch backup, branch delay slots, E-bit) is
    executed *abstractly* at generation time.  A node of the generated program
    is (pc, abstract pipeline state); because every MSCAL starts with a clean
    scheduler, all stall counts, commit points of MAC/status/clip/Q/P and the
    cycle counter advance are compile-time constants per node.  The only
    data-dependent timing is the XGKICK-behind-XGKICK stall, which is resolved
    at run time with a switch over the (bounded) number of stall cycles.
  * VF/VI/ACC hazards always stall in the interpreter, so register semantics
    are sequential (with the pair rules: both halves read pre-pair state, the
    upper wins a same-register VF write, I-bit upper sees the old I).
  * Float math: NEON float32 under FE_TOWARDZERO (as the interpreter), fused
    multiply-add exactly where clang fuses the interpreter's expressions
    (MADD*/MSUB*/OPMSUB), operand clamping (denormal->0, Inf/NaN->+-FLT_MAX)
    only where a lane is not statically known to be normalized, result flush of
    denormals to signed zero.  This is bit-identical to the interpreter's
    exact-FMAC path (proof sketch in the doc).
  * MAC/status/clip flags are tracked as variables; a backward liveness pass
    removes every flag computation that no reader (FM*/FS*/FC*) can observe.
    The "full" variant keeps all flags live at exit (used by the diff test).
  * XGKICK is emulated lazily but cycle-exactly: the PATH1 copy engine is
    advanced to the current cycle before every VU data store, so packets are
    byte-identical to the interpreter's even if the program overwrites a
    buffer that is still being transferred.
"""

from __future__ import annotations

import argparse
import os
import struct
import sys
from collections import defaultdict
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vu1decode import (  # noqa: E402
    ACC_FORWARD_LATENCY, DEST, FD, FMAC_LATENCY, FS, FT, IMM5, IMM11, IMM12, IMM15_IADDIU,
    P_BRANCH, P_EFU, P_FDIV, P_XGKICK, VID, VIS, VIT, Pair, decode_pair, fnv1a64,
    lane_for_component, lower_name, lower_special, upper_name, upper_special)

CODE_SIZE = 0x4000
PC_MASK = 0x3FFF
MAX_NODES = 60000


class GenError(Exception):
    pass


# ---------------------------------------------------------------------------
# Abstract scheduler state
# ---------------------------------------------------------------------------
# All times are "remaining cycles until readyCycle" relative to the current
# cycle (the cycle at which the next pair would issue without stalls).
# Entries whose remaining time reached 0 have been committed and are dropped.

@dataclass(frozen=True)
class FlagEntry:
    rem: int
    w_mac: bool
    w_status: bool
    w_sticky: bool
    w_clip: bool


@dataclass(frozen=True)
class AState:
    vf_ready: Tuple[Tuple[int, int, int], ...] = ()   # (reg, comp, rem)
    vi_ready: Tuple[Tuple[int, int], ...] = ()        # (reg, rem)
    acc_ready: Tuple[Tuple[int, int], ...] = ()       # (comp, rem)
    flags: Tuple[Optional[FlagEntry], ...] = (None,) * 8
    fdiv: int = 0                                     # rem, 0 = invalid
    efu: Tuple[int, int] = (0, 0)
    efu_res: int = 0
    backup_reg: int = 0                               # VI branch backup (prev pair)
    branch: Optional[Tuple[str, int, int]] = None     # (kind, target, delay)
    ebit: bool = False

    def max_rem(self) -> int:
        m = 0
        for _, _, r in self.vf_ready:
            m = max(m, r)
        for _, r in self.vi_ready:
            m = max(m, r)
        for _, r in self.acc_ready:
            m = max(m, r)
        for f in self.flags:
            if f:
                m = max(m, f.rem)
        m = max(m, self.fdiv, self.efu[0], self.efu[1], self.efu_res)
        return m

    def pending_any(self) -> bool:
        return self.max_rem() > 0


class MState:
    """Mutable working copy of AState used while stepping one pair."""

    def __init__(self, s: AState):
        self.vf = {(r, c): t for r, c, t in s.vf_ready}
        self.vi = {r: t for r, t in s.vi_ready}
        self.acc = {c: t for c, t in s.acc_ready}
        self.flags = [None if f is None else dict(rem=f.rem, w_mac=f.w_mac, w_status=f.w_status,
                                                    w_sticky=f.w_sticky, w_clip=f.w_clip, new=False)
                      for f in s.flags]
        self.fdiv = s.fdiv
        self.efu = list(s.efu)
        self.efu_res = s.efu_res
        self.backup_reg = s.backup_reg
        self.branch = s.branch
        self.ebit = s.ebit

    def freeze(self) -> AState:
        return AState(
            vf_ready=tuple(sorted((r, c, t) for (r, c), t in self.vf.items() if t > 0)),
            vi_ready=tuple(sorted((r, t) for r, t in self.vi.items() if t > 0)),
            acc_ready=tuple(sorted((c, t) for c, t in self.acc.items() if t > 0)),
            flags=tuple(None if f is None else FlagEntry(f['rem'], f['w_mac'], f['w_status'],
                                                           f['w_sticky'], f['w_clip'])
                        for f in self.flags),
            fdiv=self.fdiv, efu=(self.efu[0], self.efu[1]), efu_res=max(0, self.efu_res),
            backup_reg=self.backup_reg, branch=self.branch, ebit=self.ebit)

    def advance_one(self, out: List[tuple]):
        """advanceOneCycle(): decrement, then commitReadyPipelines() in interpreter order."""
        for k in list(self.vf):
            self.vf[k] -= 1
            if self.vf[k] <= 0:
                del self.vf[k]
        for k in list(self.vi):
            self.vi[k] -= 1
            if self.vi[k] <= 0:
                del self.vi[k]
        for k in list(self.acc):
            self.acc[k] -= 1
            if self.acc[k] <= 0:
                del self.acc[k]
        for slot, f in enumerate(self.flags):
            if f is None:
                continue
            f['rem'] -= 1
            if f['rem'] <= 0:
                # updateFlagPipeline commit order: mac, status (current+sticky OR), FSSET sticky, clip.
                # Split per register so liveness can kill MAC (a full overwrite) independently of
                # the read-modify-write status update.
                if f['w_mac']:
                    out.append(('commit_mac', slot))
                if f['w_status']:
                    out.append(('commit_status', slot))
                if f['w_sticky']:
                    out.append(('commit_sticky', slot))
                if f['w_clip']:
                    out.append(('commit_clip', slot))
                self.flags[slot] = None
        if self.fdiv > 0:
            self.fdiv -= 1
            if self.fdiv == 0:
                out.append(('commit_fdiv',))
        for k in range(2):
            if self.efu[k] > 0:
                self.efu[k] -= 1
                if self.efu[k] == 0:
                    out.append(('commit_efu', k))
        if self.efu_res > 0:
            self.efu_res -= 1
        out.append(('cycle',))

    def advance(self, n: int, out: List[tuple]):
        for _ in range(n):
            self.advance_one(out)

    def max_pending(self) -> int:
        return self.freeze().max_rem()


# ---------------------------------------------------------------------------
# Node graph
# ---------------------------------------------------------------------------

@dataclass
class Node:
    nid: int
    pc: int
    state: AState
    phase: str                     # 'pre' (stall + body) or 'body' (after dynamic stall)
    pair: Pair = None
    stall_ops: List[tuple] = field(default_factory=list)
    dyn_stall: Optional[dict] = None   # XGKICK: {'s0':, 'kmax':, 'cases': {s: (ops, target_nid)}}
    body_ops: List[tuple] = field(default_factory=list)
    post_ops: List[tuple] = field(default_factory=list)
    succ: List[Tuple[str, int]] = field(default_factory=list)   # (cond, nid)
    term: Optional[tuple] = None   # ('goto', nid) | ('cond', taken_nid, fall_nid) | ('dyn', [(pc,nid)]) | ('end', flush_ops, kmax)
    new_slots: Dict[str, int] = field(default_factory=dict)
    backward_target: bool = False


class Generator:
    def __init__(self, code: bytes, entries: List[int], name: str):
        if len(code) != CODE_SIZE:
            raise GenError(f"image size {len(code)} != 16 KiB")
        self.code = code
        self.entries = entries
        self.name = name
        self.pairs: Dict[int, Pair] = {}
        self.nodes: List[Node] = []
        self.node_index: Dict[Tuple[int, AState, str], int] = {}
        self.worklist: List[int] = []
        self.link_labels = self._find_link_labels()
        self.entry_nodes: Dict[int, int] = {}
        self.failed_entries: Dict[int, str] = {}
        self.dt_pcs: set = set()

    def pair(self, pc: int) -> Pair:
        p = self.pairs.get(pc)
        if p is None:
            p = decode_pair(self.code, pc)
            self.pairs[pc] = p
        return p

    def _find_link_labels(self) -> List[int]:
        labels = set()
        for pc in range(0, CODE_SIZE, 8):
            p = decode_pair(self.code, pc)
            if not p.ibit and lower_name(p.lower) in ('BAL', 'JALR'):
                labels.add((pc + 16) & PC_MASK)
        return sorted(labels)

    def get_node(self, pc: int, state: AState, phase: str = 'pre') -> int:
        key = (pc, state, phase)
        nid = self.node_index.get(key)
        if nid is None:
            nid = len(self.nodes)
            if nid >= MAX_NODES:
                raise GenError("node limit exceeded (pipeline state explosion)")
            self.nodes.append(Node(nid=nid, pc=pc, state=state, phase=phase))
            self.node_index[key] = nid
            self.worklist.append(nid)
        return nid

    # -- stall computation (calculatePairReadyCycle) ------------------------
    @staticmethod
    def static_stall(p: Pair, m: MState) -> int:
        ready = 0
        for u in (p.uu, p.lu):
            for reg, lanes in u.vf_read:
                for c in range(4):
                    if lanes & lane_for_component(c):
                        ready = max(ready, m.vf.get((reg, c), 0))
            for reg in range(1, 16):
                if u.vi_read & (1 << reg):
                    ready = max(ready, m.vi.get(reg, 0))
            for c in range(4):
                if u.acc_read & lane_for_component(c):
                    ready = max(ready, m.acc.get(c, 0))
        lu = p.lu
        if lu.pipeline == P_FDIV and m.fdiv > 0:
            ready = max(ready, m.fdiv)
        if lu.pipeline == P_EFU:
            ready = max(ready, m.efu_res)
        if lu.wait_q and m.fdiv > 0:
            ready = max(ready, m.fdiv)
        if lu.wait_p:
            for t in m.efu:
                if t > 0:
                    ready = max(ready, t)
        return ready

    def build(self):
        for e in self.entries:
            try:
                self.entry_nodes[e] = self.get_node(e & PC_MASK, AState())
            except GenError as ex:
                self.failed_entries[e] = str(ex)
        while self.worklist:
            nid = self.worklist.pop()
            self.step(self.nodes[nid])
        # backward targets (loop heads) get a cycle-budget check
        for n in self.nodes:
            for t in self.node_targets(n):
                if self.nodes[t].pc <= n.pc:
                    self.nodes[t].backward_target = True

    def node_targets(self, n: Node) -> List[int]:
        out = []
        if n.dyn_stall:
            out.extend(t for _, t in n.dyn_stall['cases'].values())
        if n.term:
            k = n.term[0]
            if k == 'goto':
                out.append(n.term[1])
            elif k == 'cond':
                out.extend([n.term[1], n.term[2]])
            elif k == 'dyn':
                out.extend(t for _, t in n.term[1])
        return out

    def step(self, n: Node):
        p = self.pair(n.pc)
        n.pair = p
        if p.reserved:
            raise GenError(f"reserved instruction reachable at pc 0x{n.pc:04x}: {p.text()}")
        if p.dbit or p.tbit:
            # With FBRST.DE/TE clear (the normal case) the interpreter ignores D/T bits, so the
            # native code does too. The image is flagged and the runtime dispatcher sends every
            # MSCAL to the interpreter while DE or TE is set, before any side effect.
            self.dt_pcs.add(n.pc)
        m = MState(n.state)
        if n.phase == 'pre':
            s0 = self.static_stall(p, m)
            if p.lu.pipeline == P_XGKICK:
                # Dynamic stall: issue = max(static ready, previous-kick finish).
                kmax = max(s0, m.max_pending())
                cases = {}
                for s in range(s0, kmax + 1):
                    mm = MState(n.state)
                    ops = []
                    mm.advance(s, ops)
                    cases[s] = (ops, self.get_node(n.pc, mm.freeze(), 'body'))
                n.dyn_stall = {'s0': s0, 'kmax': kmax, 'cases': cases}
                return
            m.advance(s0, n.stall_ops)
        self.exec_pair(n, p, m)

    def exec_pair(self, n: Node, p: Pair, m: MState):
        ops = n.body_ops
        lu, uu = p.lu, p.uu
        # -- execUpper / execLower semantics as ops (code emitted later) --
        new_slots = {}
        backup_in = m.backup_reg
        if p.upper_vf_shadow_reg == 0 and uu.vf_write[0] == 0 and not p.ibit:
            pass
        # Quirk: an upper write to vf0 is visible to a lower in the same pair.
        un = upper_name(p.upper)
        if (not p.ibit and FD(p.upper) == 0 and DEST(p.upper) and (p.upper & 0x3F) <= 0x2F
                and un not in ('NOP',)):
            if p.lu.vf_read_lanes(0):
                raise GenError(f"upper writes vf0 while lower reads vf0 at 0x{n.pc:04x}")
        sp = upper_special(p.upper)
        if sp is not None and 0x10 <= sp <= 0x1D and FT(p.upper) == 0 and p.lu.vf_read_lanes(0):
            raise GenError(f"upper writes vf0 while lower reads vf0 at 0x{n.pc:04x}")

        # upper flag-pipeline entries
        upper_flag_kind = self.upper_flag_kind(p.upper)
        if upper_flag_kind == 'fmac' and DEST(p.upper) != 0:
            slot = self.alloc_slot(m, n.pc)
            m.flags[slot] = dict(rem=FMAC_LATENCY, w_mac=True, w_status=True, w_sticky=False,
                                 w_clip=False, new=True)
            new_slots['fmac'] = slot
        elif upper_flag_kind == 'clip':
            slot = self.alloc_slot(m, n.pc)
            m.flags[slot] = dict(rem=FMAC_LATENCY, w_mac=False, w_status=False, w_sticky=False,
                                 w_clip=True, new=True)
            new_slots['clip'] = slot
        ops.append(('upper',))

        ln = 'LOI' if p.ibit else lower_name(p.lower)
        lsp = None if p.ibit else lower_special(p.lower)
        if ln == 'FSSET':
            for f in m.flags:
                if f is not None and f['new']:
                    f['w_status'] = False
            slot = self.alloc_slot(m, n.pc)
            m.flags[slot] = dict(rem=FMAC_LATENCY, w_mac=False, w_status=False, w_sticky=True,
                                 w_clip=False, new=True)
            new_slots['fsset'] = slot
        elif ln == 'FCSET':
            for f in m.flags:
                if f is not None and f['new']:
                    f['w_clip'] = False
            slot = self.alloc_slot(m, n.pc)
            m.flags[slot] = dict(rem=FMAC_LATENCY, w_mac=False, w_status=False, w_sticky=False,
                                 w_clip=True, new=True)
            new_slots['fcset'] = slot
        elif ln in ('DIV', 'SQRT', 'RSQRT'):
            m.fdiv = lu.latency
        elif lu.pipeline == P_EFU and not lu.wait_p:
            free = [k for k in range(2) if m.efu[k] == 0]
            if not free:
                raise GenError(f"EFU queue overflow at 0x{n.pc:04x}")
            k = free[0]
            m.efu[k] = lu.latency
            m.efu_res = lu.latency - 1 if lu.latency > 0 else 0
            new_slots['efu'] = k
        for f in m.flags:
            if f is not None:
                f['new'] = False
        n.new_slots = new_slots
        ops.append(('lower', ln, lsp))

        # markPairWrites
        lw = lu.vf_write
        if lw[0] != 0 and p.suppressed_lower_vf != lw[0]:
            lat = lu.eff_vf_latency()
            for c in range(4):
                if lw[1] & lane_for_component(c):
                    m.vf[(lw[0], c)] = lat
        uw = uu.vf_write
        if uw[0] != 0:
            lat = uu.eff_vf_latency()
            for c in range(4):
                if uw[1] & lane_for_component(c):
                    m.vf[(uw[0], c)] = lat
        written_vi = 0
        for reg in range(1, 16):
            if lu.vi_write & (1 << reg):
                m.vi[reg] = lu.eff_vi_latency()
                if written_vi == 0:
                    written_vi = reg
        for c in range(4):
            if uu.acc_write & lane_for_component(c):
                m.acc[c] = ACC_FORWARD_LATENCY
        m.backup_reg = written_vi if (written_vi and lu.delays_next_branch_read) else 0
        n.save_backup = m.backup_reg

        # branch / pc update
        next_pc = (n.pc + 8) & PC_MASK
        if ln in ('B', 'BAL', 'JR', 'JALR', 'IBEQ', 'IBNE', 'IBLTZ', 'IBGTZ', 'IBLEZ', 'IBGEZ'):
            if m.branch is not None:
                raise GenError(f"branch in branch delay slot at 0x{n.pc:04x}")
            if m.ebit:
                raise GenError(f"branch in E-bit delay slot at 0x{n.pc:04x}")
            if ln in ('JR', 'JALR'):
                kind, target = 'dyn', 0
            else:
                target = (n.pc + 8 + IMM11(p.lower) * 8) & PC_MASK
                kind = 'static' if ln in ('B', 'BAL') else 'cond'
            new_branch = (kind, target, 1)
            n.branch_backup_in = backup_in
        else:
            new_branch = None
        succ_pc = next_pc
        take = None
        if m.branch is not None:
            kind, target, delay = m.branch
            if delay == 0:
                take = (kind, target)
                m.branch = None
            else:
                m.branch = (kind, target, delay - 1)
        if new_branch is not None:
            # branchDelay is decremented in the same pair it was set
            m.branch = (new_branch[0], new_branch[1], 0)

        ended = False
        if m.ebit:
            ended = True            # this pair was the E-bit delay slot
        elif p.ebit:
            m.ebit = True
        # advanceOneCycle after the pair
        m.advance(1, n.post_ops)
        if ended:
            if take is not None:
                raise GenError(f"branch resolves in E-bit delay slot at 0x{n.pc:04x}")
            flush = []
            kmax = m.max_pending()
            m.advance(kmax, flush)
            n.term = ('end', flush, kmax)
            return
        st = m.freeze()
        if take is None:
            n.term = ('goto', self.get_node(succ_pc, st))
        elif take[0] == 'static':
            n.term = ('goto', self.get_node(take[1], st))
        elif take[0] == 'cond':
            n.term = ('cond', self.get_node(take[1], st), self.get_node(succ_pc, st))
        else:
            n.term = ('dyn', [(lab, self.get_node(lab, st)) for lab in self.link_labels])

    @staticmethod
    def upper_flag_kind(upper: int) -> Optional[str]:
        op = upper & 0x3F
        if op <= 0x2F:
            if (0x10 <= op <= 0x17) or op in (0x1D, 0x1F, 0x2B, 0x2F):
                return None   # MAX/MINI: applyDest, no flags
            return 'fmac'
        if op >= 0x3C:
            sp = (upper & 3) | ((upper >> 4) & 0x7C)
            if sp <= 0x0F or (0x18 <= sp <= 0x1C) or sp == 0x1E or (0x20 <= sp <= 0x2A) or (0x2C <= sp <= 0x2E):
                return 'fmac'
            if sp == 0x1F:
                return 'clip'
        return None

    @staticmethod
    def alloc_slot(m: MState, pc: int) -> int:
        for k, f in enumerate(m.flags):
            if f is None:
                return k
        raise GenError(f"flag pipeline overflow at 0x{pc:04x}")


# ---------------------------------------------------------------------------
# Dataflow: lane normalization (forward) and flag liveness (backward)
# ---------------------------------------------------------------------------

def all_targets(g: Generator, n: Node) -> List[int]:
    return g.node_targets(n)


def upper_writes(p: Pair):
    """(reg or 'acc', lanes, normalized?) written by the upper instruction."""
    if p.uu.reserved:
        return None
    op = p.upper & 0x3F
    dest = DEST(p.upper)
    if op <= 0x2F:
        return (FD(p.upper), dest, True)   # FMAC results and MAX/MINI of clamped operands
    sp = upper_special(p.upper)
    if sp is None:
        return None
    if 0x10 <= sp <= 0x13:
        return (FT(p.upper), dest, True)   # ITOF: ints convert to normal floats
    if 0x14 <= sp <= 0x17:
        return (FT(p.upper), dest, False)  # FTOI: integer bit patterns
    if sp == 0x1D:
        return (FT(p.upper), dest, True)   # ABS of clamped operand
    if p.uu.acc_write:
        return ('acc', dest, True)
    return None


def lower_vf_write(p: Pair):
    if p.ibit:
        return None
    reg, lanes = p.lu.vf_write
    if reg == 0 or p.suppressed_lower_vf == reg:
        return None
    ln = lower_name(p.lower)
    normalized = ln in ('MFP', 'RNEXT', 'RGET')   # P is normalized; R is 1.xxx
    return (reg, lanes, normalized)


def compute_norm(g: Generator):
    """Per node: bitmask per VF register (and ACC as index 32) of lanes known to
    hold normalized values (exponent neither 0 nor 255 unless the value is +-0)."""
    N = len(g.nodes)
    full = [0xF] * 33
    nin: List[Optional[List[int]]] = [None] * N
    entry_state = [0] * 33
    entry_state[0] = 0xF
    work = []
    for e, nid in g.entry_nodes.items():
        nin[nid] = list(entry_state)
        work.append(nid)
    preds_seen = set()
    while work:
        nid = work.pop()
        n = g.nodes[nid]
        cur = list(nin[nid])
        if n.phase == 'pre' and n.dyn_stall is not None:
            out = cur
        else:
            out = list(cur)
            p = n.pair
            lw = lower_vf_write(p)
            uw = upper_writes(p)
            if lw:
                reg, lanes, nz = lw
                out[reg] = (out[reg] | lanes) if nz else (out[reg] & ~lanes)
            if uw:
                reg, lanes, nz = uw
                idx = 32 if reg == 'acc' else reg
                if idx != 0:
                    out[idx] = (out[idx] | lanes) if nz else (out[idx] & ~lanes)
            out[0] = 0xF
        for t in all_targets(g, n):
            if nin[t] is None:
                nin[t] = list(out)
                work.append(t)
            else:
                merged = [a & b for a, b in zip(nin[t], out)]
                if merged != nin[t]:
                    nin[t] = merged
                    work.append(t)
    for n in g.nodes:
        n.norm_in = nin[n.nid] if nin[n.nid] is not None else [0] * 33
        n.norm_in[0] = 0xF


def node_flag_ops(n: Node) -> List[tuple]:
    """Ordered flag-relevant ops of a node (for liveness)."""
    seq = []
    seq.extend(n.stall_ops)
    if n.dyn_stall is not None:
        return seq   # cases are attached to edges; handled separately
    p = n.pair
    ns = n.new_slots
    if 'fmac' in ns:
        seq.append(('def_fmac', ns['fmac']))
    if 'clip' in ns:
        seq.append(('def_clip', ns['clip']))
    ln = 'LOI' if p.ibit else lower_name(p.lower)
    if ln in ('FMEQ', 'FMAND', 'FMOR'):
        seq.append(('use', 'mac'))
    elif ln in ('FSEQ', 'FSAND', 'FSOR'):
        seq.append(('use', 'status'))
    elif ln in ('FCEQ', 'FCAND', 'FCOR', 'FCGET'):
        seq.append(('use', 'clip'))
    elif ln == 'FSSET':
        seq.append(('def_fsset', ns['fsset']))
    elif ln == 'FCSET':
        seq.append(('def_fcset', ns['fcset']))
    elif ln in ('DIV', 'SQRT', 'RSQRT'):
        seq.append(('def_qdi',))
    seq.extend(n.post_ops)
    if n.term and n.term[0] == 'end':
        seq.extend(n.term[1])
        seq.append(('exit',))
    return seq


def op_defs_uses(op: tuple):
    k = op[0]
    if k == 'commit_mac':
        # m_state.mac = entry.mac: a full definition (kill)
        return {'mac'}, {f'fm{op[1]}'}, False
    if k == 'commit_status':
        # status = (status & 0xFF0) | cur | ((cur | extraSticky) << 6): read-modify-write
        s = op[1]
        return {'status'}, {'status', f'fs{s}', f'fx{s}'}, True
    if k == 'commit_sticky':
        # FSSET: status = (status & 0x03F) | (entry.status & 0xFC0): read-modify-write
        return {'status'}, {'status', f'fs{op[1]}'}, True
    if k == 'commit_clip':
        # m_state.clip = entry.clip (kept as update semantics: clip is live everywhere anyway)
        return {'clip'}, {f'fc{op[1]}'}, True
    if k == 'commit_fdiv':
        return {'status'}, {'status', 'qdi'}, True
    if k == 'def_fmac':
        s = op[1]
        return {f'fm{s}', f'fs{s}', f'fx{s}'}, set(), False
    if k == 'def_clip':
        s = op[1]
        return {f'fc{s}', 'wclip'}, {'wclip'}, False
    if k == 'def_fcset':
        s = op[1]
        return {f'fc{s}', 'wclip'}, set(), False
    if k == 'def_fsset':
        s = op[1]
        return {f'fs{s}'}, set(), False
    if k == 'def_qdi':
        return {'qdi'}, set(), False
    if k == 'use':
        return set(), {op[1]}, False
    return set(), set(), False


def compute_flag_liveness(g: Generator, live_out_exit: set):
    """Backward liveness over flag variables.  Results stored as n.needed: set of
    op ids (index into the node's flag op sequence) that must be emitted."""
    N = len(g.nodes)
    seqs = [node_flag_ops(n) for n in g.nodes]
    # edge ops for dynamic stall cases: (from nid, case s) ops executed on the edge
    live_in = [set() for _ in range(N)]
    preds = defaultdict(set)
    for n in g.nodes:
        for t in all_targets(g, n):
            preds[t].add(n.nid)

    def transfer(nid, live_after: set):
        n = g.nodes[nid]
        live = set(live_after)
        seq = seqs[nid]
        for op in reversed(seq):
            if op[0] == 'exit':
                live |= live_out_exit
                continue
            d, u, upd = op_defs_uses(op)
            if d & live or op[0] == 'use':
                if not upd:
                    live -= d
                live |= u
        return live

    def node_live_out(nid):
        n = g.nodes[nid]
        if n.dyn_stall is not None:
            acc = set()
            for s, (ops, t) in n.dyn_stall['cases'].items():
                live = set(live_in[t])
                for op in reversed(ops):
                    d, u, upd = op_defs_uses(op)
                    if d & live:
                        if not upd:
                            live -= d
                        live |= u
                acc |= live
            return acc
        acc = set()
        for t in all_targets(g, n):
            acc |= live_in[t]
        return acc

    changed = True
    work = list(range(N))
    inq = set(work)
    while work:
        nid = work.pop()
        inq.discard(nid)
        lo = node_live_out(nid)
        n = g.nodes[nid]
        if n.dyn_stall is not None:
            li = set(lo)
            for op in reversed(n.stall_ops):
                d, u, upd = op_defs_uses(op)
                if d & li:
                    if not upd:
                        li -= d
                    li |= u
        else:
            li = transfer(nid, lo)
        if li != live_in[nid]:
            live_in[nid] = li
            for pr in preds[nid]:
                if pr not in inq:
                    work.append(pr)
                    inq.add(pr)
    g.flag_live_in = live_in

    # Now decide, per op occurrence, whether it is needed.
    def needed_ops(ops, live_after):
        live = set(live_after)
        need = [False] * len(ops)
        for i in range(len(ops) - 1, -1, -1):
            op = ops[i]
            if op[0] == 'exit':
                live |= live_out_exit
                continue
            d, u, upd = op_defs_uses(op)
            if d & live or op[0] == 'use':
                need[i] = True
                if not upd:
                    live -= d
                live |= u
        return need, live

    for n in g.nodes:
        lo = node_live_out(n.nid)
        if n.dyn_stall is not None:
            n.case_needed = {}
            for s, (ops, t) in n.dyn_stall['cases'].items():
                need, _ = needed_ops(ops, live_in[t])
                n.case_needed[s] = need
            need, _ = needed_ops(n.stall_ops, lo)
            n.flag_need = {'stall': need}
            continue
        seq = seqs[n.nid]
        need, _ = needed_ops(seq, lo)
        # split back into sections
        res = {}
        i = 0
        res['stall'] = need[i:i + len(n.stall_ops)]; i += len(n.stall_ops)
        body = seq[i:len(seq) - len(n.post_ops) - (len(n.term[1]) + 1 if n.term and n.term[0] == 'end' else 0)]
        res['body'] = {op: need[i + j] for j, op in enumerate(body)}
        i += len(body)
        res['post'] = need[i:i + len(n.post_ops)]; i += len(n.post_ops)
        if n.term and n.term[0] == 'end':
            res['flush'] = need[i:i + len(n.term[1])]
        n.flag_need = res


# ---------------------------------------------------------------------------
# C++ emission
# ---------------------------------------------------------------------------

LANE = "xyzw"


def fhex(v: float) -> str:
    bits = struct.unpack("<I", struct.pack("<f", v))[0]
    return f"vr_bits2f(0x{bits:08x}u)"


class Emitter:
    def __init__(self, g: Generator, variant: str, live_out_exit: set):
        self.g = g
        self.variant = variant
        self.live_out_exit = live_out_exit
        self.lines: List[str] = []
        self.commit_counts: Dict[str, int] = defaultdict(int)

    def w(self, s: str):
        self.lines.append(s)

    # -- helpers -------------------------------------------------------------
    @staticmethod
    def vf(r: int) -> str:
        return "V0" if r == 0 else f"v{r}"

    @staticmethod
    def vi(r: int) -> str:
        return "0" if r == 0 else f"i{r}"

    def opnd(self, n: Node, r: int, lanes: int) -> str:
        """Clamped operand (normalizeOperand) for the given lanes."""
        if r == 0:
            return "V0"
        if (n.norm_in[r] & lanes) == lanes:
            return self.vf(r)
        return f"vr_norm({self.vf(r)})"

    def acc_opnd(self, n: Node, lanes: int) -> str:
        if (n.norm_in[32] & lanes) == lanes:
            return "acc"
        return "vr_norm(acc)"

    def emit_commit(self, op: tuple, needed: bool):
        k = op[0]
        if k == 'cycle':
            return
        if k in ('commit_mac', 'commit_status', 'commit_sticky', 'commit_clip'):
            self.commit_counts[k + '_total'] += 1
            if not needed:
                return
            self.commit_counts[k] += 1
            s = op[1]
            if k == 'commit_mac':
                self.w(f"    mac = fm{s};")
            elif k == 'commit_status':
                self.w(f"    status = (status & 0xFF0u) | (fs{s} & 0xFu) | (((fs{s} & 0xFu) | fx{s}) << 6);")
            elif k == 'commit_sticky':
                self.w(f"    status = (status & 0x03Fu) | (fs{s} & 0xFC0u);")
            else:
                self.w(f"    clip = fc{s};")
        elif k == 'commit_fdiv':
            self.w("    q = qpend;")
            self.commit_counts['commit_fdiv_status_total'] += 1
            if needed:
                self.commit_counts['commit_fdiv_status'] += 1
                self.w("    status = (status & 0xFCFu) | qdi | (qdi << 6);")
        elif k == 'commit_efu':
            self.w(f"    p = ppend{op[1]};")

    def emit_ops(self, ops: List[tuple], needs: List[bool]):
        cyc = 0
        for op, nd in zip(ops, needs):
            if op[0] == 'cycle':
                cyc += 1
            else:
                self.emit_commit(op, nd)
        return cyc

    def label(self, nid: int) -> str:
        return f"N{nid}"

    # -- main ------------------------------------------------------------------
    def emit_function(self, fname: str):
        g = self.g
        w = self.w
        w(f"static void {fname}(Vu1RecContext &ctx, uint32_t entryPc)")
        w("{")
        w("    Vu1RecRoundingGuard roundingGuard;")
        w("    VU1State &st = *ctx.state;")
        w("    uint8_t *const mem = ctx.data;")
        w("    uint64_t cyc = ctx.cycle;")
        w("    const uint64_t budgetEnd = ctx.cycle + ctx.maxCycles;")
        w("    const float32x4_t V0 = vr_load_vf(st.vf[0]);")
        regs = ", ".join(f"v{r} = vr_load_vf(st.vf[{r}])" for r in range(1, 32))
        w(f"    float32x4_t {regs};")
        w("    float32x4_t acc = vr_load_vf(st.acc);")
        w("    int32_t " + ", ".join(f"i{r} = st.vi[{r}]" for r in range(1, 16)) + ";")
        w("    float q = st.q, p = st.p, I = st.i;")
        w("    uint32_t R = st.r, mac = st.mac, status = st.status, clip = st.clip;")
        w("    uint32_t wclip = st.clip;")
        w("    float qpend = 0.0f, ppend0 = 0.0f, ppend1 = 0.0f;")
        w("    uint32_t qdi = 0;")
        w("    int32_t vibk = 0;")
        w("    bool brc = false;")
        w("    uint32_t brt = 0;")
        w("    uint32_t " + ", ".join(f"fm{s} = 0, fs{s} = 0, fx{s} = 0, fc{s} = 0" for s in range(8)) + ";")
        w("    (void)qdi; (void)vibk; (void)brc; (void)brt; (void)ppend0; (void)ppend1; (void)wclip;")
        w("    switch (entryPc)")
        w("    {")
        for e, nid in sorted(g.entry_nodes.items()):
            w(f"    case 0x{e:04x}u: goto {self.label(nid)};")
        w("    default: ctx.failed = true; ctx.failPc = entryPc; return;")
        w("    }")
        order = sorted(g.nodes, key=lambda n: (n.pc, n.phase != 'pre', n.nid))
        for n in order:
            self.emit_node(n)
        w("EXIT:")
        for r in range(1, 32):
            w(f"    vr_store_vf(st.vf[{r}], v{r});")
        w("    vr_store_vf(st.acc, acc);")
        for r in range(1, 16):
            w(f"    st.vi[{r}] = i{r};")
        w("    st.q = q; st.p = p; st.i = I; st.r = R;")
        if self.variant == 'full':
            w("    st.mac = mac; st.status = status; st.clip = clip;")
        else:
            for f in ('mac', 'status', 'clip'):
                if f in self.live_out_exit:
                    w(f"    st.{f} = {f};")
        w("    ctx.cycle = cyc;")
        w("    return;")
        w("FAIL:")
        w("    ctx.failed = true;")
        w("    goto EXIT;")
        w("}")

    def emit_node(self, n: Node):
        w = self.w
        p = n.pair
        w(f"{self.label(n.nid)}: // pc 0x{n.pc:04x} {n.phase} :: {p.text()}")
        if n.backward_target:
            w(f"    if (cyc >= budgetEnd) {{ ctx.failPc = 0x{n.pc:04x}u; goto FAIL; }}")
        stall_need = n.flag_need['stall']
        c = self.emit_ops(n.stall_ops, stall_need)
        if c:
            w(f"    cyc += {c};")
        if n.dyn_stall is not None:
            ds = n.dyn_stall
            w("    {")
            w("        const uint64_t kickEnd = vu1rec_kick_drain(ctx);")
            w(f"        uint64_t s = {ds['s0']}u;")
            w("        if (kickEnd > cyc && kickEnd - cyc > s) s = kickEnd - cyc;")
            w(f"        const uint32_t sc = s > {ds['kmax']}u ? {ds['kmax']}u : (uint32_t)s;")
            w("        cyc += s;")
            w("        switch (sc)")
            w("        {")
            for s, (ops, t) in sorted(ds['cases'].items()):
                w(f"        case {s}u:")
                need = n.case_needed[s]
                for op, nd in zip(ops, need):
                    if op[0] != 'cycle':
                        self.emit_commit(op, nd)
                w(f"            goto {self.label(t)};")
            w("        default: goto FAIL;")
            w("        }")
            w("    }")
            return
        w("    {")
        self.emit_pair(n, p)
        c = self.emit_ops(n.post_ops, n.flag_need['post'])
        w(f"    cyc += {c};")
        t = n.term
        if t[0] == 'goto':
            w(f"    goto {self.label(t[1])};")
        elif t[0] == 'cond':
            w(f"    if (brc) goto {self.label(t[1])};")
            w(f"    goto {self.label(t[2])};")
        elif t[0] == 'dyn':
            w("    switch (brt)")
            w("    {")
            for lab, nid in t[1]:
                w(f"    case 0x{lab:04x}u: goto {self.label(nid)};")
            w(f"    default: ctx.failPc = brt; goto FAIL;")
            w("    }")
        elif t[0] == 'end':
            flush, kmax = t[1], t[2]
            c = self.emit_ops(flush, n.flag_need['flush'])
            w(f"    cyc += {c};")
            w("    {")
            w("        const uint64_t kickEnd = vu1rec_kick_drain(ctx);")
            w("        if (kickEnd > cyc) cyc = kickEnd;")
            w("    }")
            w(f"    st.pc = 0x{(n.pc + 8) & PC_MASK:04x}u;")
            w("    goto EXIT;")
        w("    }")

    # -- pair body -----------------------------------------------------------
    def emit_pair(self, n: Node, p: Pair):
        w = self.w
        need = n.flag_need['body']
        ns = n.new_slots
        # 1. upper result into temporaries (reads pre-pair state)
        uwb = self.emit_upper(n, p, need)
        # 2. lower
        if p.ibit:
            imm = struct.unpack("<f", struct.pack("<I", p.lower))[0]
            bits = p.lower
            # normalizeOperand(immediate)
            e = (bits >> 23) & 0xFF
            if e == 0:
                bits &= 0x80000000
            elif e == 0xFF:
                bits = (bits & 0x80000000) | 0x7F7FFFFF
            uwb.append(f"    I = vr_bits2f(0x{bits:08x}u);")
        else:
            self.emit_lower(n, p, need)
        # 3. upper write-back
        for s in uwb:
            w(s)

    def bc_vec(self, n: Node, ft: int, bc: int) -> str:
        return f"vr_dup<{bc}>({self.opnd(n, ft, lane_for_component(bc))})"

    def emit_upper(self, n: Node, p: Pair, need) -> List[str]:
        w = self.w
        up = p.upper
        op = up & 0x3F
        dest = DEST(up)
        fs, ft, fd = FS(up), FT(up), FD(up)
        ns = n.new_slots
        wb: List[str] = []
        fmac_needed = 'fmac' in ns and need.get(('def_fmac', ns['fmac']), False)
        clip_needed = 'clip' in ns and need.get(('def_clip', ns['clip']), False)
        if self.variant == 'full':
            fmac_needed = 'fmac' in ns
            clip_needed = 'clip' in ns

        def opS(lanes=dest):
            return self.opnd(n, fs, lanes)

        q_s = "vr_normf(q)"
        i_s = "vr_normf(I)"

        def finish_fmac(expr: str, to_acc: bool, kind: str, tsrc: str):
            # result flush (denormal -> signed zero) and destination write
            w(f"    const float32x4_t ur = vr_flush({expr});")
            if fmac_needed:
                self.commit_counts['vr_fmac_flags'] += 1
                s = ns['fmac']
                w(f"    vr_fmac_flags<0x{up:08x}u>({opS(0xF)}, {self.opnd(n, ft, 0xF)}, {self.acc_opnd(n, 0xF)}, "
                  f"{q_s}, {i_s}, fm{s}, fs{s}, fx{s});")
            if to_acc:
                wb.append(f"    acc = {self.blend('ur', 'acc', dest)};")
            elif fd != 0:
                wb.append(f"    {self.vf(fd)} = {self.blend('ur', self.vf(fd), dest)};")

        if op <= 0x2F:
            if op <= 0x1B:
                bc = op & 3
                T = self.bc_vec(n, ft, bc)
                grp = op >> 2
                tsrc = self.opnd(n, ft, 0xF)
                if grp == 0:
                    finish_fmac(f"vaddq_f32({opS()}, {T})", False, 'VR_ADD_BC', tsrc)
                elif grp == 1:
                    finish_fmac(f"vsubq_f32({opS()}, {T})", False, 'VR_SUB_BC', tsrc)
                elif grp == 2:
                    finish_fmac(f"vfmaq_f32({self.acc_opnd(n, dest)}, {opS()}, {T})", False, 'VR_MADD_BC', tsrc)
                elif grp == 3:
                    finish_fmac(f"vfmsq_f32({self.acc_opnd(n, dest)}, {opS()}, {T})", False, 'VR_MSUB_BC', tsrc)
                elif grp == 4:
                    w(f"    const float32x4_t ur = vr_max({opS()}, {T});")
                    if fd:
                        wb.append(f"    {self.vf(fd)} = {self.blend('ur', self.vf(fd), dest)};")
                elif grp == 5:
                    w(f"    const float32x4_t ur = vr_min({opS()}, {T});")
                    if fd:
                        wb.append(f"    {self.vf(fd)} = {self.blend('ur', self.vf(fd), dest)};")
                else:
                    finish_fmac(f"vmulq_f32({opS()}, {T})", False, 'VR_MUL_BC', tsrc)
                return wb
            tsrc = self.opnd(n, ft, 0xF)
            tfull = self.opnd(n, ft, dest)
            A = self.acc_opnd(n, dest)
            qv = f"vdupq_n_f32({q_s})"
            iv = f"vdupq_n_f32({i_s})"
            table = {
                0x1C: (f"vmulq_f32({opS()}, {qv})", 'VR_MULQ'),
                0x1E: (f"vmulq_f32({opS()}, {iv})", 'VR_MULI'),
                0x20: (f"vaddq_f32({opS()}, {qv})", 'VR_ADDQ'),
                0x21: (f"vfmaq_f32({A}, {opS()}, {qv})", 'VR_MADDQ'),
                0x22: (f"vaddq_f32({opS()}, {iv})", 'VR_ADDI'),
                0x23: (f"vfmaq_f32({A}, {opS()}, {iv})", 'VR_MADDI'),
                0x24: (f"vsubq_f32({opS()}, {qv})", 'VR_SUBQ'),
                0x25: (f"vfmsq_f32({A}, {opS()}, {qv})", 'VR_MSUBQ'),
                0x26: (f"vsubq_f32({opS()}, {iv})", 'VR_SUBI'),
                0x27: (f"vfmsq_f32({A}, {opS()}, {iv})", 'VR_MSUBI'),
                0x28: (f"vaddq_f32({opS()}, {tfull})", 'VR_ADD'),
                0x29: (f"vfmaq_f32({A}, {opS()}, {tfull})", 'VR_MADD'),
                0x2A: (f"vmulq_f32({opS()}, {tfull})", 'VR_MUL'),
                0x2C: (f"vsubq_f32({opS()}, {tfull})", 'VR_SUB'),
                0x2D: (f"vfmsq_f32({A}, {opS()}, {tfull})", 'VR_MSUB'),
            }
            if op in table:
                expr, kind = table[op]
                finish_fmac(expr, False, kind, tsrc)
                return wb
            if op == 0x2E:   # OPMSUB
                S = self.opnd(n, fs, 0x7 << 1)
                T = self.opnd(n, ft, 0xE)
                finish_fmac(f"vr_opmsub({self.acc_opnd(n, 0xE)}, {S}, {T})", False, 'VR_OPMSUB', T)
                return wb
            sel = {0x1D: ('max', iv), 0x1F: ('min', iv), 0x2B: ('max', tfull), 0x2F: ('min', tfull)}
            if op in sel:
                f, T = sel[op]
                w(f"    const float32x4_t ur = vr_{f}({opS()}, {T});")
                if fd:
                    wb.append(f"    {self.vf(fd)} = {self.blend('ur', self.vf(fd), dest)};")
                return wb
            raise GenError(f"unhandled upper op 0x{op:02x}")

        sp = upper_special(up)
        tsrc = self.opnd(n, ft, 0xF)
        A = self.acc_opnd(n, dest)
        qv = f"vdupq_n_f32({q_s})"
        iv = f"vdupq_n_f32({i_s})"
        if sp <= 0x0F or 0x18 <= sp <= 0x1B:
            bc = sp & 3
            T = self.bc_vec(n, ft, bc)
            grp = sp >> 2
            expr, kind = {
                0: (f"vaddq_f32({opS()}, {T})", 'VR_ADD_BC'),
                1: (f"vsubq_f32({opS()}, {T})", 'VR_SUB_BC'),
                2: (f"vfmaq_f32({A}, {opS()}, {T})", 'VR_MADD_BC'),
                3: (f"vfmsq_f32({A}, {opS()}, {T})", 'VR_MSUB_BC'),
                6: (f"vmulq_f32({opS()}, {T})", 'VR_MUL_BC'),
            }[grp]
            finish_fmac(expr, True, kind, tsrc)
            return wb
        if 0x10 <= sp <= 0x13:
            scale = {0x10: None, 0x11: 16.0, 0x12: 4096.0, 0x13: 32768.0}[sp]
            src = self.vf(fs)
            e = f"vcvtq_f32_s32(vreinterpretq_s32_f32({src}))"
            if scale:
                e = f"vmulq_n_f32({e}, {1.0 / scale!r}f)"
            w(f"    const float32x4_t ur = {e};")
            if ft:
                wb.append(f"    {self.vf(ft)} = {self.blend('ur', self.vf(ft), dest)};")
            return wb
        if 0x14 <= sp <= 0x17:
            scale = {0x14: 1.0, 0x15: 16.0, 0x16: 4096.0, 0x17: 32768.0}[sp]
            S = opS()
            if scale == 1.0:
                e = f"vreinterpretq_f32_s32(vcvtq_s32_f32({S}))"
            else:
                e = f"vreinterpretq_f32_s32(vcvtq_s32_f32(vmulq_n_f32({S}, {scale!r}f)))"
            w(f"    const float32x4_t ur = {e};")
            if ft:
                wb.append(f"    {self.vf(ft)} = {self.blend('ur', self.vf(ft), dest)};")
            return wb
        if sp == 0x1D:
            w(f"    const float32x4_t ur = vabsq_f32({opS()});")
            if ft:
                wb.append(f"    {self.vf(ft)} = {self.blend('ur', self.vf(ft), dest)};")
            return wb
        if sp == 0x1F:   # CLIP (raw bits)
            if clip_needed:
                s = ns['clip']
                w(f"    wclip = ((wclip << 6) | vr_clip_flags({self.vf(fs)}, {self.vf(ft)})) & 0xFFFFFFu;")
                w(f"    fc{s} = wclip;")
            return wb
        table = {
            0x1C: (f"vmulq_f32({opS()}, {qv})", 'VR_MULQ'),
            0x1E: (f"vmulq_f32({opS()}, {iv})", 'VR_MULI'),
            0x20: (f"vaddq_f32({opS()}, {qv})", 'VR_ADDQ'),
            0x21: (f"vfmaq_f32({A}, {opS()}, {qv})", 'VR_MADDQ'),
            0x22: (f"vaddq_f32({opS()}, {iv})", 'VR_ADDI'),
            0x23: (f"vfmaq_f32({A}, {opS()}, {iv})", 'VR_MADDI'),
            0x24: (f"vsubq_f32({opS()}, {qv})", 'VR_SUBQ'),
            0x25: (f"vfmsq_f32({A}, {opS()}, {qv})", 'VR_MSUBQ'),
            0x26: (f"vsubq_f32({opS()}, {iv})", 'VR_SUBI'),
            0x27: (f"vfmsq_f32({A}, {opS()}, {iv})", 'VR_MSUBI'),
            0x28: (f"vaddq_f32({opS()}, {self.opnd(n, ft, dest)})", 'VR_ADD'),
            0x29: (f"vfmaq_f32({A}, {opS()}, {self.opnd(n, ft, dest)})", 'VR_MADD'),
            0x2A: (f"vmulq_f32({opS()}, {self.opnd(n, ft, dest)})", 'VR_MUL'),
            0x2C: (f"vsubq_f32({opS()}, {self.opnd(n, ft, dest)})", 'VR_SUB'),
            0x2D: (f"vfmsq_f32({A}, {opS()}, {self.opnd(n, ft, dest)})", 'VR_MSUB'),
        }
        if sp in table:
            expr, kind = table[sp]
            finish_fmac(expr, True, kind, tsrc)
            return wb
        if sp == 0x2E:   # OPMULA
            S = self.opnd(n, fs, 0xE)
            T = self.opnd(n, ft, 0xE)
            finish_fmac(f"vr_opmula({S}, {T})", True, 'VR_OPMULA', T)
            return wb
        if sp in (0x2F, 0x30):
            return wb
        raise GenError(f"unhandled upper special 0x{sp:02x}")

    @staticmethod
    def blend(new: str, old: str, dest: int) -> str:
        if dest == 0xF:
            return new
        if dest == 0:
            return old
        return f"vr_blend<0x{dest:x}>({new}, {old})"

    def addr_imm(self, base: int, imm: int) -> str:
        return f"((uint32_t)({self.vi(base)} + ({imm})) * 16u & 0x3FFFu)"

    def addr_reg(self, base: int) -> str:
        return f"((uint32_t)(uint16_t){self.vi(base)} * 16u & 0x3FFFu)"

    def set_vi(self, r: int, expr: str, save_backup: bool):
        if r == 0:
            return
        if save_backup:
            self.w(f"    vibk = {self.vi(r)};")
        self.w(f"    {self.vi(r)} = (int16_t)({expr});")

    def branch_vi(self, n: Node, r: int) -> str:
        if r == 0:
            return "0"
        if getattr(n, 'branch_backup_in', 0) == r:
            return "vibk"
        return self.vi(r)

    def store_guard(self):
        self.w("    if (ctx.kick.active) vu1rec_kick_catchup(ctx, cyc);")

    def emit_lower(self, n: Node, p: Pair, need):
        w = self.w
        lo = p.lower
        ln = lower_name(lo)
        dest = DEST(lo)
        vfT, vfS = FT(lo), FS(lo)
        viT, viS, viD = VIT(lo), VIS(lo), VID(lo)
        sb = bool(p.lu.delays_next_branch_read)
        ns = n.new_slots
        lvw = lower_vf_write(p)
        suppressed = p.suppressed_lower_vf != 0 and p.suppressed_lower_vf == p.lu.vf_write[0]

        def vfw(reg: int, expr: str):
            if reg == 0 or suppressed:
                return
            w(f"    {self.vf(reg)} = {self.blend(expr, self.vf(reg), dest)};")

        if ln == 'NOP':
            return
        if ln == 'LQ':
            w(f"    {{ const float32x4_t t = vr_ld(mem + {self.addr_imm(viS, IMM11(lo))});")
            vfw(vfT, 't')
            w("    }")
            return
        if ln == 'SQ':
            self.store_guard()
            w(f"    vr_st<0x{dest:x}>(mem + {self.addr_imm(viT, IMM11(lo))}, {self.vf(vfS)});")
            return
        if ln in ('ILW', 'ILWR'):
            comp = 0 if dest & 8 else 1 if dest & 4 else 2 if dest & 2 else 3
            a = self.addr_imm(viS, IMM11(lo)) if ln == 'ILW' else self.addr_reg(viS)
            self.set_vi(viT, f"vr_ld32(mem + {a} + {comp * 4}u) & 0xFFFFu", False)
            return
        if ln in ('ISW', 'ISWR'):
            a = self.addr_imm(viS, IMM11(lo)) if ln == 'ISW' else self.addr_reg(viS)
            self.store_guard()
            w(f"    vr_stw<0x{dest:x}>(mem + {a}, (uint32_t)(uint16_t){self.vi(viT)});")
            return
        if ln == 'IADDIU':
            self.set_vi(viT, f"{self.vi(viS)} + {IMM15_IADDIU(lo)}", sb)
            return
        if ln == 'ISUBIU':
            self.set_vi(viT, f"{self.vi(viS)} - {IMM15_IADDIU(lo)}", sb)
            return
        if ln == 'IADD':
            self.set_vi(viD, f"{self.vi(viS)} + {self.vi(viT)}", sb)
            return
        if ln == 'ISUB':
            self.set_vi(viD, f"{self.vi(viS)} - {self.vi(viT)}", sb)
            return
        if ln == 'IADDI':
            self.set_vi(viT, f"{self.vi(viS)} + ({IMM5(lo)})", sb)
            return
        if ln == 'IAND':
            self.set_vi(viD, f"{self.vi(viS)} & {self.vi(viT)}", sb)
            return
        if ln == 'IOR':
            self.set_vi(viD, f"{self.vi(viS)} | {self.vi(viT)}", sb)
            return
        if ln == 'FCEQ':
            self.set_vi(1, f"(clip & 0xFFFFFFu) == 0x{lo & 0xFFFFFF:x}u ? 1 : 0", False)
            return
        if ln == 'FCAND':
            self.set_vi(1, f"(clip & 0x{lo & 0xFFFFFF:x}u) != 0u ? 1 : 0", False)
            return
        if ln == 'FCOR':
            self.set_vi(1, f"((clip | 0x{lo & 0xFFFFFF:x}u) == 0xFFFFFFu) ? 1 : 0", False)
            return
        if ln == 'FCGET':
            self.set_vi(viT, "clip & 0x0FFFu", False)
            return
        if ln == 'FCSET':
            s = ns['fcset']
            if self.variant == 'full' or need.get(('def_fcset', s), False):
                w(f"    wclip = 0x{lo & 0xFFFFFF:x}u; fc{s} = wclip;")
            return
        if ln == 'FSEQ':
            self.set_vi(viT, f"(status & 0xFFFu) == 0x{IMM12(lo):x}u ? 1 : 0", False)
            return
        if ln == 'FSAND':
            self.set_vi(viT, f"(status & 0xFFFu) & 0x{IMM12(lo):x}u", False)
            return
        if ln == 'FSOR':
            self.set_vi(viT, f"(status & 0xFFFu) | 0x{IMM12(lo):x}u", False)
            return
        if ln == 'FSSET':
            s = ns['fsset']
            w(f"    fs{s} = 0x{IMM12(lo) & 0xFC0:x}u;")
            return
        if ln == 'FMEQ':
            self.set_vi(viT, f"(mac & 0xFFFFu) == (uint32_t)(uint16_t){self.vi(viS)} ? 1 : 0", False)
            return
        if ln == 'FMAND':
            self.set_vi(viT, f"mac & (uint32_t)(uint16_t){self.vi(viS)}", False)
            return
        if ln == 'FMOR':
            self.set_vi(viT, f"mac | (uint32_t)(uint16_t){self.vi(viS)}", False)
            return
        if ln in ('B', 'BAL'):
            if ln == 'BAL':
                self.set_vi(viT, f"{(n.pc + 16) // 8}", False)
            return
        if ln in ('JR', 'JALR'):
            w(f"    brt = ((uint32_t)(uint16_t){self.branch_vi(n, viS)} * 8u) & 0x3FFFu;")
            if ln == 'JALR':
                self.set_vi(viT, f"{(n.pc + 16) // 8}", False)
            return
        conds = {'IBEQ': "==", 'IBNE': "!="}
        if ln in conds:
            w(f"    brc = (int16_t){self.branch_vi(n, viS)} {conds[ln]} (int16_t){self.branch_vi(n, viT)};")
            return
        conds1 = {'IBLTZ': "< 0", 'IBGTZ': "> 0", 'IBLEZ': "<= 0", 'IBGEZ': ">= 0"}
        if ln in conds1:
            w(f"    brc = (int16_t){self.branch_vi(n, viS)} {conds1[ln]};")
            return
        if ln == 'MOVE':
            vfw(vfT, self.vf(vfS))
            return
        if ln == 'MR32':
            vfw(vfT, f"vextq_f32({self.vf(vfS)}, {self.vf(vfS)}, 1)")
            return
        if ln in ('LQI', 'LQD', 'SQI', 'SQD'):
            base = viS if ln[0] == 'L' else viT
            if ln in ('LQD', 'SQD'):
                self.set_vi(base, f"{self.vi(base)} - 1", sb)
            a = self.addr_reg(base)
            if ln[0] == 'L':
                w(f"    {{ const float32x4_t t = vr_ld(mem + {a});")
                vfw(vfT, 't')
                w("    }")
            else:
                self.store_guard()
                w(f"    vr_st<0x{dest:x}>(mem + {a}, {self.vf(vfS)});")
            if ln in ('LQI', 'SQI'):
                self.set_vi(base, f"{self.vi(base)} + 1", sb)
            return
        if ln in ('DIV', 'RSQRT'):
            fsf, ftf = (lo >> 21) & 3, (lo >> 23) & 3
            num = f"vr_normf(vgetq_lane_f32({self.vf(vfS)}, {fsf}))"
            den = f"vr_normf(vgetq_lane_f32({self.vf(vfT)}, {ftf}))"
            fn = 'vr_div' if ln == 'DIV' else 'vr_rsqrt'
            w(f"    qpend = {fn}({num}, {den}, qdi);")
            return
        if ln == 'SQRT':
            ftf = (lo >> 23) & 3
            w(f"    qpend = vr_sqrt(vr_normf(vgetq_lane_f32({self.vf(vfT)}, {ftf})), qdi);")
            return
        if ln in ('WAITQ', 'WAITP'):
            return
        if ln == 'MTIR':
            comp = (lo >> 21) & 3
            self.set_vi(viT, f"vr_f2bits(vgetq_lane_f32({self.vf(vfS)}, {comp})) & 0xFFFFu", sb)
            return
        if ln == 'MFIR':
            vfw(vfT, f"vreinterpretq_f32_s32(vdupq_n_s32((int32_t)(int16_t){self.vi(viS)}))")
            return
        if ln == 'MFP':
            vfw(vfT, "vdupq_n_f32(p)")
            return
        if ln == 'RNEXT':
            w("    R = vr_rnext(R);")
            vfw(vfT, "vdupq_n_f32(vr_bits2f(R))")
            return
        if ln == 'RGET':
            vfw(vfT, "vdupq_n_f32(vr_bits2f(R))")
            return
        if ln in ('RINIT', 'RXOR'):
            comp = (lo >> 21) & 3
            b = f"vr_f2bits(vgetq_lane_f32({self.vf(vfS)}, {comp}))"
            if ln == 'RINIT':
                w(f"    R = 0x3F800000u | ({b} & 0x007FFFFFu);")
            else:
                w(f"    R = 0x3F800000u | ((R ^ {b}) & 0x007FFFFFu);")
            return
        if ln == 'XTOP':
            self.set_vi(viT, "ctx.top & 0x3FFu", False)
            return
        if ln == 'XITOP':
            self.set_vi(viT, "ctx.itop & 0x3FFu", False)
            return
        if ln == 'XGKICK':
            w(f"    vu1rec_kick_start(ctx, (uint32_t)(uint16_t){self.vi(viS)}, cyc);")
            return
        efu = {'ESADD', 'ERSADD', 'ELENG', 'ERLENG', 'EATANxy', 'EATANxz', 'ESUM', 'ERSQRT',
               'ESQRT', 'ESIN', 'ERCPR', 'EATAN', 'EEXP'}
        if ln in efu:
            k = ns['efu']
            comp = (lo >> 21) & 3
            S = self.vf(vfS)
            w(f"    ppend{k} = vr_normf_result(vr_efu_{ln.lower()}({S}, {comp}));")
            return
        raise GenError(f"unhandled lower op {ln} at 0x{n.pc:04x}")


# ---------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------

HEADER = """// GENERATED by game/tools/vu1recomp/vu1recomp.py -- do not edit, do not commit.
// Derived from a locally captured VU1 micro memory image (disc data).
#include "runtime/ps2_vu1_recomp_support.h"

namespace {
"""


FLAG_REGS = ('mac', 'status', 'clip')


def entry_live_in(g: Generator, live_exit: set, pcs) -> Dict[int, set]:
    """Flag registers live into each entry pc (pipeline empty, as after resetScheduler()).

    wclip is the interpreter's m_workingClip, which resetScheduler() reloads from
    m_state.clip (the emitted prologue does wclip = st.clip), so a live wclip at an entry
    means the previous program's clip is observed."""
    compute_flag_liveness(g, live_exit)
    out = {}
    for pc in pcs:
        nid = g.entry_nodes.get(pc)
        if nid is None:
            continue
        li = set(g.flag_live_in[nid])
        if 'wclip' in li:
            li.discard('wclip')
            li.add('clip')
        stray = li - set(FLAG_REGS)
        if stray:
            raise GenError(f"pipeline-slot flag state live at entry 0x{pc:04x}: {sorted(stray)}")
        out[pc] = li
    return out


def end_continuations(g: Generator) -> List[int]:
    """PCs where an MSCNT after a completed program resumes (TPC = pc after the E-bit delay slot)."""
    return sorted({(n.pc + 8) & PC_MASK for n in g.nodes if n.term and n.term[0] == 'end'})


def analyze_image(code: bytes, entries: List[int], name: str) -> dict:
    """Liveness-only analysis of one image: what each program entry (and each MSCNT
    continuation point) may read from the flag state left by the previous program."""
    g = Generator(code, entries, name)
    g.build()
    if not g.entry_nodes:
        raise GenError("no entry could be generated")
    info = {'g': g, 'conts': end_continuations(g), 'cont_g': None, 'cont_error': None}
    conts = [c for c in info['conts'] if c not in g.entry_nodes]
    if conts:
        try:
            gc = Generator(code, sorted(set(entries) | set(conts)), name)
            gc.build()
            missing = [c for c in conts if c not in gc.entry_nodes]
            if missing:
                raise GenError(f"continuations not analysable: {['0x%04x' % c for c in missing]}")
            info['cont_g'] = gc
        except GenError as ex:
            info['cont_error'] = str(ex)
    return info


def image_entry_reads(info: dict, live_exit: set) -> Tuple[set, Dict[int, set], Dict[int, set]]:
    g = info['g']
    ent = entry_live_in(g, live_exit, sorted(g.entry_nodes))
    reads = set().union(*ent.values()) if ent else set()
    cont = {}
    if info['cont_error'] is not None:
        reads |= set(FLAG_REGS)    # MSCNT could resume into code we cannot analyse
    elif info['cont_g'] is not None:
        cont = entry_live_in(info['cont_g'], live_exit, [c for c in info['conts']])
        for li in cont.values():
            reads |= li
    else:
        cont = {c: ent.get(c, set()) for c in info['conts']}
    return reads, ent, cont


def compute_live_exit(images_info: List[Optional[dict]]) -> Tuple[set, list]:
    """Least fixpoint: a flag register is live when a program exits iff some program entry
    (of any compiled image) or MSCNT continuation reads it before redefining it, given that
    exit liveness. An image we cannot compile runs in the interpreter and may read anything."""
    live = set()
    if any(info is None for info in images_info):
        live = set(FLAG_REGS)
    log = []
    while True:
        new = set(live)
        log = []
        for info in images_info:
            if info is None:
                continue
            reads, ent, cont = image_entry_reads(info, live)
            new |= reads
            log.append((info['g'].name, ent, cont, info['cont_error']))
        if new == live:
            return live, log
        live = new


def generate_image(code: bytes, entries: List[int], name: str, live_exit: Optional[set] = None) -> Tuple[str, dict]:
    g = Generator(code, entries, name)
    g.build()
    if not g.entry_nodes:
        raise GenError("no entry could be generated")
    compute_norm(g)
    # Which flags are read anywhere in the (reachable) image?
    readers = set()
    for n in g.nodes:
        for op in node_flag_ops(n):
            if op[0] == 'use':
                readers.add(op[1])
    # VU1State persists across MSCALs and across code images: a flag is live at exit
    # iff a later program can read it before redefining it (compute_live_exit).
    fast_live = set(FLAG_REGS) if live_exit is None else set(live_exit)
    full_live = set(FLAG_REGS)
    out = [HEADER]
    stats = {'nodes': len(g.nodes), 'pairs': len({n.pc for n in g.nodes}),
             'readers': sorted(readers), 'entries': sorted(g.entry_nodes),
             'failed': g.failed_entries, 'dt_pcs': sorted(g.dt_pcs), 'live_exit': sorted(fast_live)}
    for variant, live in (('fast', fast_live), ('full', full_live)):
        compute_flag_liveness(g, live)
        em = Emitter(g, variant, live)
        em.emit_function(f"vu1rec_{name}_{variant}")
        out.extend(l + "\n" for l in em.lines)
        out.append("\n")
        if variant == 'fast':
            dead = sum(1 for n in g.nodes if 'fmac' in n.new_slots and not n.flag_need.get('body', {}).get(('def_fmac', n.new_slots['fmac']), False))
            total = sum(1 for n in g.nodes if 'fmac' in n.new_slots)
            stats['fmac_flag_producers'] = total
            stats['fmac_flag_producers_dead'] = dead
            cl = sum(1 for n in g.nodes if 'clip' in n.new_slots)
            cl_live = sum(1 for n in g.nodes if 'clip' in n.new_slots and n.flag_need.get('body', {}).get(('def_clip', n.new_slots['clip']), False))
            stats['clip_producers'] = cl
            stats['clip_producers_live'] = cl_live
            stats['fast_commits'] = dict(sorted(em.commit_counts.items()))
    ent = ", ".join(f"0x{e:04x}u" for e in sorted(g.entry_nodes))
    out.append(f"const uint32_t kEntries[] = {{{ent}}};\n")
    out.append(f"const Vu1RecRegistrar kRegistrar(0x{name}ull, kEntries, sizeof(kEntries) / sizeof(kEntries[0]),\n"
               f"                                 &vu1rec_{name}_fast, &vu1rec_{name}_full, {'true' if g.dt_pcs else 'false'});\n")
    out.append("} // namespace\n")
    return "".join(out), stats


def read_catalog(cat: str) -> Dict[str, List[int]]:
    entries: Dict[str, List[int]] = defaultdict(list)
    with open(os.path.join(cat, "index.txt")) as f:
        for line in f:
            parts = line.split()
            if len(parts) < 2:
                continue
            entries[parts[0]].append(int(parts[1], 16))
    return entries


def jump_table_entries(code: bytes) -> List[int]:
    """Byte PCs of a leading dispatch table: consecutive (B target / NOP) pairs at 0x00, 0x10, ...

    Games commonly start a VU1 image with such a table and pick a slot per MSCAL. Every slot
    is a valid entry even if a catalogue run never saw it called, so compile them all."""
    out = []
    for slot in range(0, len(code) // 16):
        lo0, up0 = struct.unpack_from("<II", code, slot * 16)
        lo1, up1 = struct.unpack_from("<II", code, slot * 16 + 8)
        is_b = (lo0 >> 25) == 0x20          # lower opcode 0x20 = B
        nop_lower = lo1 == 0x8000033C        # NOP lower
        upper_nop = (up0 & 0x7FF) == 0x2FF and (up1 & 0x7FF) == 0x2FF and not (up0 & 0x40000000)
        if not (is_b and nop_lower and upper_nop):
            break
        out.append(slot * 16)
    return out if len(out) >= 2 else []


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--catalog", required=True, help="VU1 catalog dir (index.txt + <hash>.vu1)")
    ap.add_argument("--out", required=True, help="output directory for generated C++")
    ap.add_argument("--dump", action="store_true", help="print node graph statistics")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    cat = read_catalog(args.catalog)
    images = []
    for h, entries in sorted(cat.items()):
        path = os.path.join(args.catalog, h + ".vu1")
        if not os.path.exists(path):
            print(f"[vu1recomp] {h}: image file missing, skipped", file=sys.stderr)
            continue
        code = open(path, "rb").read()
        if f"{fnv1a64(code):016x}" != h:
            print(f"[vu1recomp] {h}: hash mismatch, skipped", file=sys.stderr)
            continue
        jt = jump_table_entries(code)
        extra = sorted(set(jt) - set(entries))
        if extra:
            print(f"[vu1recomp] {h}: adding uncatalogued jump-table entries {['0x%04x' % e for e in extra]}")
        images.append((h, code, sorted(set(entries) | set(jt))))
    # pass 1: liveness-only analysis of every image, then the live-at-exit fixpoint
    infos = []
    for h, code, entries in images:
        try:
            infos.append(analyze_image(code, entries, h))
        except GenError as ex:
            # an image we cannot compile runs in the interpreter, which may read any flag
            print(f"[vu1recomp] {h}: not compilable ({ex}); all flags live at exit", file=sys.stderr)
            infos.append(None)
    live_exit, live_log = compute_live_exit(infos)
    fmt = lambda d: " ".join(f"0x{pc:04x}:{''.join(sorted(x[0] for x in v)) or '-'}" for pc, v in sorted(d.items()))
    for name, ent, cont, cerr in live_log:
        print(f"[vu1recomp] {name}: entry flag live-in (m=mac s=status c=clip) {fmt(ent)}; "
              f"MSCNT continuations {fmt(cont) if cerr is None else 'UNANALYSABLE: ' + cerr}")
    written = []
    for h, code, entries in images:
        try:
            src, stats = generate_image(code, entries, h, live_exit)
        except GenError as ex:
            print(f"[vu1recomp] {h}: FAILED: {ex}", file=sys.stderr)
            continue
        dst = os.path.join(args.out, f"vu1rec_{h}.cpp")
        old = open(dst).read() if os.path.exists(dst) else None
        if old != src:
            with open(dst, "w") as f:
                f.write(src)
        written.append(dst)
        print(f"[vu1recomp] {h}: entries={['0x%04x' % e for e in stats['entries']]} nodes={stats['nodes']} "
              f"pairs={stats['pairs']} flag-readers={stats['readers']} live-at-exit={stats['live_exit']} "
              f"fmac-flag-producers={stats['fmac_flag_producers']} (dead in fast: {stats['fmac_flag_producers_dead']}) "
              f"clip-producers={stats['clip_producers']} (live: {stats['clip_producers_live']}) "
              f"fast-commits-emitted={stats['fast_commits']}"
              + (f" failed={stats['failed']}" if stats['failed'] else ""))
    # remove stale generated files
    for fn in os.listdir(args.out):
        p = os.path.join(args.out, fn)
        if fn.startswith("vu1rec_") and fn.endswith(".cpp") and p not in written:
            os.remove(p)
    return 0


if __name__ == "__main__":
    sys.exit(main())
