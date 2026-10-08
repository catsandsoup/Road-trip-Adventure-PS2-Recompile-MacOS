#!/usr/bin/env python3
"""Decode PS2X_TRACE_CALLS dumps of a race car struct (field offsets from RTAO's PAL race oracle)."""
import re, struct, sys
def blocks(path):
    cur = None
    for line in open(path):
        if line.startswith("[call]"):
            if cur: yield cur
            m = re.match(r"\[call\] (enter|exit)\s+(\w+) #(\d+) (.*)", line)
            cur = {"kind": m.group(1), "n": int(m.group(3)), "regs": m.group(4), "mem": bytearray()}
        elif cur is not None and line.startswith("  "):
            cur["mem"] += b"".join(struct.pack("<I", int(w, 16)) for w in line.split(":")[1].split())
    if cur: yield cur
def f(b, o, n): return struct.unpack_from("<%df" % n, b, o)
def i(b, o, n): return struct.unpack_from("<%di" % n, b, o)
for blk in blocks(sys.argv[1]):
    b = blk["mem"]
    if len(b) < 0x248: continue
    print(f"{blk['kind']:5} #{blk['n']:<5} pos_fx={i(b,0xa0,3)} coord={tuple(round(x,2) for x in f(b,0x90,4))} "
          f"vel={i(b,0xf0,3)} speed={i(b,0x1b8,1)[0]} wheel={i(b,0x1bc,1)[0]} yaw={struct.unpack_from('<H',b,0x1d4)[0]} "
          f"carFlags={struct.unpack_from('<H',b,0x198)[0]:#x} mtxT={tuple(round(x,1) for x in f(b,0x30,4))}")
