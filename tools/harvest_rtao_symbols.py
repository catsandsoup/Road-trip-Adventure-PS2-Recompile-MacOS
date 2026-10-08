#!/usr/bin/env python3
"""Map addresses cited in RTAO's archaeology onto Ghidra function boundaries.

Output (local, gitignored): work/rtao_symbols.json
  { "0x00218888": {"function_start": "...", "notes": ["...context..."], "sources": [...]}, ... }
"""
import bisect, csv, json, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RTAO = os.path.join(ROOT, "..", "rtao")
WORK = os.path.join(ROOT, "..", "work")
ADDR = re.compile(r"0x0{0,2}(2[0-9A-Fa-f]{5}|3[0-9A-Fa-f]{5})\b")


def load_functions(path):
    funcs = []
    with open(path) as f:
        for row in csv.DictReader(f):
            funcs.append((int(row["Start"], 16), int(row["End"], 16), row["Name"]))
    funcs.sort()
    return funcs


def containing(funcs, starts, addr):
    i = bisect.bisect_right(starts, addr) - 1
    if i >= 0 and funcs[i][0] <= addr < funcs[i][1]:
        return funcs[i]
    return None


def main():
    funcs = load_functions(os.path.join(WORK, "rta_functions.csv"))
    starts = [f[0] for f in funcs]
    out = {}
    for base in ("docs", "rtao/src", "tools"):
        for dp, _, fns in os.walk(os.path.join(RTAO, base)):
            for fn in fns:
                if not fn.endswith((".md", ".ts", ".py", ".mjs", ".txt")):
                    continue
                p = os.path.join(dp, fn)
                rel = os.path.relpath(p, RTAO)
                try:
                    text = open(p, encoding="utf-8", errors="replace").read()
                except OSError:
                    continue
                for m in ADDR.finditer(text):
                    a = int(m.group(1), 16)
                    key = "0x%08X" % a
                    s = max(0, m.start() - 160)
                    ctx = " ".join(text[s : m.end() + 200].split())
                    e = out.setdefault(key, {"notes": [], "sources": set()})
                    if len(e["notes"]) < 6 and ctx not in e["notes"]:
                        e["notes"].append(ctx)
                    e["sources"].add(rel)
    text_lo, text_hi = 0x200000, 0x292EBC
    n_code = n_entry = 0
    for key, e in out.items():
        a = int(key, 16)
        e["sources"] = sorted(e["sources"])
        if text_lo <= a < text_hi:
            n_code += 1
            f = containing(funcs, starts, a)
            if f:
                e["function_start"] = "0x%08X" % f[0]
                e["ghidra_name"] = f[2]
                if f[0] == a:
                    n_entry += 1
            e["kind"] = "code"
        else:
            e["kind"] = "data"
    os.makedirs(WORK, exist_ok=True)
    dst = os.path.join(WORK, "rtao_symbols.json")
    json.dump(out, open(dst, "w"), indent=1, sort_keys=True)
    print(f"{len(out)} addresses, {n_code} in .text, {n_entry} exact function entries -> {dst}")


if __name__ == "__main__":
    sys.exit(main())
