#!/usr/bin/env python3
"""ISO9660 extent map (code only, no disc data): lsn -> file path. Used by run_route.py to name sceCdRead targets.
Usage: cd_files.py <iso> [hexlsn ...]"""
import struct, sys
def walk(f, lsn, size, path, out):
    f.seek(lsn * 2048); data = f.read(size); i = 0
    while i < len(data):
        n = data[i]
        if n == 0:
            i = (i // 2048 + 1) * 2048; continue
        rec = data[i:i + n]; ext = struct.unpack_from("<I", rec, 2)[0]; ln = struct.unpack_from("<I", rec, 10)[0]
        flags = rec[25]; nl = rec[32]; name = rec[33:33 + nl].decode("latin1").split(";")[0]
        if name not in ("\x00", "\x01"):
            p = path + "/" + name
            if flags & 2: walk(f, ext, ln, p, out)
            else: out.append((ext, (ln + 2047) // 2048, p))
        i += n
def build(iso):
    f = open(iso, "rb"); f.seek(16 * 2048); pvd = f.read(2048)
    root = pvd[156:190]; out = []
    walk(f, struct.unpack_from("<I", root, 2)[0], struct.unpack_from("<I", root, 10)[0], "", out)
    return sorted(out)
def lookup(m, lsn):
    for e, n, p in m:
        if e <= lsn < e + n: return f"{p}+{lsn - e}"
    return "?"
if __name__ == "__main__":
    m = build(sys.argv[1])
    if len(sys.argv) == 2:
        for e in m: print(*e)
    for a in sys.argv[2:]: print(a, lookup(m, int(a, 16)))
