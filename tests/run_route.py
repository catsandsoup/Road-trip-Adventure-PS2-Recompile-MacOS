#!/usr/bin/env python3
"""Run a scripted gameplay route against the native build and check it.

Usage:
  game/tests/run_route.py <route-name> [--seconds N] [--every K] [--golden-capture]

Routes live in game/tests/routes/<name>.json:
  {
    "input": "input/boot_to_title.txt",       # PS2X_INPUT_SCRIPT, relative to game/tests
    "seconds": 100,                            # wall-clock run time
    "min_fps": 45,                             # required steady game fps outside loads (menus/2D)
    "min_fps_3d": 45,                          # required fps once 3D starts (raise as VU1 work lands)
    "checkpoints": [                           # frame-index checkpoints (presented-frame index)
      {"name": "title", "frame": 700, "not_black": true}
    ]
  }

Checks (all reported, non-zero exit on failure):
  - no blank/content alternation (flicker) over any 20-frame window
  - no fully black frame at a checkpoint marked not_black; no near-uniform frame at one with max_flat
  - checkpoints are matched by "frame" (presented index) or "read" (pad-read clock, from the _r<n> tag)
  - optional golden comparison (mean abs diff) against work/golden/<route>/<checkpoint>.png (local only)
  - fps from [fps] lines >= thresholds (after warm-up)
  - zero "function not found"/lookupFunction misses, unexpected IOP RPCs, unhandled syscalls
  - optional "exit_after_ticks": N (PS2X_EXIT_AFTER_TICKS) ends the run cleanly instead of by alarm
  - optional "audio": [{"name": "race_music", "from_read": 1500, "to_read": 2400, "min_rms": 100}]
    dumps the final mix (PS2X_AUDIO_DUMP=<out>/audio.wav; the host output stays muted) and requires the median
    RMS of the 0.5 s windows (game/tests/audio/wav_report.py) inside the read window to reach min_rms.
    With "min_active_frac": f the rule is instead "at least fraction f of those windows reach min_rms" (for
    intermittent sound such as menu SFX separated by digital silence, where a median is a coin toss).
    WAV time is wall-clock time since the dump was opened, so reads are mapped to WAV time through the
    modification times of the dumped frames (their _r<read> tags).
  - optional "car": {"from_frame": .., "to_frame": .., "min_distance": .., "max_stall_frames": ..,
                     "ref": "<path under work/>", "max_ref_dist": ..}
    traces player car 0 once per race frame (hook on 0x21F540, whose call index is the race-frame clock;
    car 0 struct at 0x1820470: +0x90 x, +0x98 z, +0xF0/+0xF8 velocity) and checks the distance driven, stalls,
    that the trace reaches to_frame and, when the local (disc-derived, work/ only) reference line exists,
    the distance from it.
    Extra car keys (Adventure QA, 2026-10-08): "zones": [{"name", "poly": [[x,z]..] | "xz": [x,z], "r", "after",
    "before"}] (car must enter each zone within the call window), "min_top_speed" (|v| velocity units),
    "min_impacts" (sudden speed losses: |v| drops by >= impact_drop (default 2500) within 6 calls).
  - optional "cd_reads": [{"name", "file": "<regex on ISO path>", "after": call, "before": call}]: traces
    sceCdRead (0x27C048, a0 = LSN) and requires a read of a matching file (names via cd_files.py) between the
    given 0x21F540 call indices (the game-frame clock of the car trace).
  - optional "min_fps_1pct_low_3d": 1st percentile of the per-second 3D [fps] samples (a proxy: the runner only
    prints per-second rates); "fps_exclude_reads": [[a, b], ...] drops [fps] samples whose preceding frame dump
    has a pad read inside a declared loading window.
  - checkpoint "region_rgb": {"box": [fx0, fy0, fx1, fy1], "rgb": [r, g, b], "tol": 35, "what": ".."} checks the mean
    colour of a fractional box (UI presence, e.g. the dialogue box), with PIL.
  - optional "min_final_read": the last dumped frame's pad read must reach this (pad stays responsive).
  - machine load (os.getloadavg) before/after is recorded in the report.
The run is wrapped in `caffeinate -d -i` when available: with the display asleep (DarkWake) GLFW finds no
monitor and the runner segfaults before the first frame.
Nothing derived from the disc is written outside work/.
"""
import argparse
import glob
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
GAME = os.path.dirname(HERE)
PORT = os.path.dirname(GAME)
WORK = os.path.join(PORT, "work")
RUN = os.path.join(GAME, "tools", "run.sh")

BAD_LOG_PATTERNS = [
    (re.compile(r"function not found|lookupFunction.*miss|no function registered", re.I), "missing-function"),
    (re.compile(r"unexpected RPC", re.I), "unexpected-rpc"),
    (re.compile(r"guest-branch:missing-target", re.I), "missing-target"),  # the Lagoon crash signature (2026-10-08)
    (re.compile(r"unhandled syscall|TODO_NAMED", re.I), "unhandled-syscall"),
    (re.compile(r"terminate\]|Error during program execution", re.I), "crash"),
]


def png_luma_stats(path, with_flat=False):
    """Return (mean_luma, nonblack_fraction[, flat_fraction]) for an 8-bit RGBA PNG (raylib ExportImage output).

    flat_fraction is the share of sampled pixels equal (to 4 bits per channel) to the most common colour:
    a frame that is only a clear colour plus a small HUD scores close to 1."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        return 0.0, 0.0
    pos, width, height, idat = 8, 0, 0, b""
    while pos < len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        ctype = data[pos + 4:pos + 8]
        chunk = data[pos + 8:pos + 8 + length]
        if ctype == b"IHDR":
            width, height = struct.unpack(">II", chunk[:8])
        elif ctype == b"IDAT":
            idat += chunk
        pos += 12 + length
    raw = zlib.decompress(idat)
    bpp, stride = 4, width * 4
    prev = bytearray(stride)
    total = nonblack = 0
    luma_sum = 0
    colours = {}
    i = 0
    for _ in range(height):
        ftype = raw[i]
        line = bytearray(raw[i + 1:i + 1 + stride])
        i += 1 + stride
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if ftype == 1:
                line[x] = (line[x] + a) & 0xFF
            elif ftype == 2:
                line[x] = (line[x] + b) & 0xFF
            elif ftype == 3:
                line[x] = (line[x] + ((a + b) >> 1)) & 0xFF
            elif ftype == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 0xFF
        for x in range(0, stride, 16):  # sample every 4th pixel for speed
            r, g, bb = line[x], line[x + 1], line[x + 2]
            l = (r * 3 + g * 6 + bb) // 10
            luma_sum += l
            total += 1
            nonblack += l > 12
            key = (r >> 4, g >> 4, bb >> 4)
            colours[key] = colours.get(key, 0) + 1
        prev = line
    if with_flat:
        flat = max(colours.values()) / max(total, 1) if colours else 1.0
        return luma_sum / max(total, 1), nonblack / max(total, 1), flat
    return luma_sum / max(total, 1), nonblack / max(total, 1)


def frame_times(files):
    """{pad read: wall-clock mtime} from the dumped frames' _r<read> tags."""
    out = {}
    for f in files:
        m = re.search(r"_r(\d+)\.png$", f)
        if m:
            out[int(m.group(1))] = os.path.getmtime(f)
    return out


def check_audio(specs, wav_path, files, report):
    """Median RMS of the final-mix WAV over read windows (see the module docstring)."""
    res = report.setdefault("audio", {})
    if not os.path.exists(wav_path) or os.path.getsize(wav_path) <= 44:
        report["failures"].append("audio: no WAV dump written")
        return
    st = os.stat(wav_path)
    t0 = getattr(st, "st_birthtime", None) or st.st_ctime
    text = subprocess.run([sys.executable, os.path.join(HERE, "audio", "wav_report.py"), wav_path, "0.5"],
                          capture_output=True, text=True).stdout
    wins = [(float(m.group(1)), float(m.group(2))) for m in re.finditer(r"t=\s*([0-9.]+)s rms=\s*([0-9.]+)", text)]
    m = re.search(r"seconds=([0-9.]+)", text)
    res["wav_seconds"] = float(m.group(1)) if m else 0.0
    times = frame_times(files)
    if not times:
        report["failures"].append("audio: no _r-tagged frames to map reads to WAV time")
        return

    def wav_t(read):
        k = min(times, key=lambda r: abs(r - read))
        return times[k] - t0 + (read - k) / 50.0

    for spec in specs:
        a, b = wav_t(spec["from_read"]), wav_t(spec["to_read"])
        sel = sorted(r for t, r in wins if a <= t < b)
        entry = {"wav_from_s": round(a, 2), "wav_to_s": round(b, 2), "windows": len(sel)}
        if sel:
            entry["median_rms"] = sel[len(sel) // 2]
            entry["max_rms"] = sel[-1]
        res[spec["name"]] = entry
        need = spec.get("min_rms", 100)
        frac = spec.get("min_active_frac")  # MX1: optional rule for intermittent sound (menu SFX between silences)
        if sel and frac is not None:
            entry["active_frac"] = round(sum(1 for r in sel if r >= need) / len(sel), 3)
        if not sel:
            report["failures"].append(f"audio {spec['name']}: WAV has no samples in {a:.1f}-{b:.1f} s")
        elif frac is not None:
            if entry["active_frac"] < frac:
                report["failures"].append(f"audio {spec['name']}: only {entry['active_frac']:.2f} of windows reach RMS {need} "
                                          f"(< {frac}; silent?)")
        elif entry["median_rms"] < need:
            report["failures"].append(f"audio {spec['name']}: median RMS {entry['median_rms']} < {need} (silent?)")


def parse_car_trace(path):
    """[(call#, x, z, vx, vz)] for player car 0 from 0x21F540 enter records (dump of 0x1820500, 0x70 bytes)."""
    rows, cur, words = [], None, []

    def flush():
        if cur is not None and len(words) >= 0x70 // 4:
            b = struct.pack("<%dI" % len(words), *words)
            x, _, z = struct.unpack_from("<3f", b, 0)
            vx, vz = struct.unpack_from("<i", b, 0x60)[0], struct.unpack_from("<i", b, 0x68)[0]
            rows.append((cur, x, z, vx, vz))

    if not os.path.exists(path):
        return rows
    for line in open(path, errors="replace"):
        if line.startswith("[call]"):
            flush()
            m = re.match(r"\[call\] enter\s+21f540 #(\d+)", line)
            cur, words = (int(m.group(1)) if m else None), []
        elif cur is not None and line.startswith("  ") and ":" in line:
            try:
                words += [int(w, 16) for w in line.split(":", 1)[1].split()]
            except ValueError:
                pass
    flush()
    return rows


def parse_cd_reads(path):
    """[(car call#, lsn, sectors)] for sceCdRead (0x27C048) enter records, keyed to the last 0x21F540 call."""
    out, last = [], 0
    if not os.path.exists(path):
        return out
    for line in open(path, errors="replace"):
        m = re.match(r"\[call\] enter\s+21f540 #(\d+)", line)
        if m:
            last = int(m.group(1))
            continue
        m = re.match(r"\[call\] enter\s+27c048 #\d+ a0=([0-9a-f]+) a1=([0-9a-f]+)", line)
        if m:
            out.append((last, int(m.group(1), 16), int(m.group(2), 16)))
    return out


def check_cd_reads(specs, calls_path, report):
    sys.path.insert(0, HERE)
    from cd_files import build, lookup
    iso = os.environ.get("PS2X_CD_IMAGE") or os.path.join(os.environ.get("RTA_DISC_DIR", os.path.join(PORT, "disc")),
                                                          "rta.iso")
    reads = parse_cd_reads(calls_path)
    if not os.path.exists(iso):
        report["failures"].append(f"cd_reads: disc image {iso} not found")
        return
    m = build(iso)
    named = [(c, lookup(m, lsn), n) for c, lsn, n in reads]
    report["cd_reads"] = [f"{c}:{p}:{n}" for c, p, n in named]
    for spec in specs:
        rx = re.compile(spec["file"])
        hit = [c for c, p, _ in named if rx.search(p) and spec.get("after", -1) <= c <= spec.get("before", 1 << 30)]
        report.setdefault("cd_checks", {})[spec["name"]] = hit[:5]
        if not hit:
            report["failures"].append(f"cd_reads {spec['name']}: no read of /{spec['file']}/ between calls "
                                      f"{spec.get('after', '-')}..{spec.get('before', '-')}")


def in_zone(z, x, y):
    import math
    if "poly" in z:
        P, sg = z["poly"], None
        for i in range(len(P)):
            (ax, az), (bx, bz) = P[i], P[(i + 1) % len(P)]
            cr = (bx - ax) * (y - az) - (bz - az) * (x - ax)
            if sg is None:
                sg = cr > 0
            elif (cr > 0) != sg:
                return False
        return True
    return math.hypot(x - z["xz"][0], y - z["xz"][1]) <= z.get("r", 2.0)


def check_car(spec, calls_path, report):
    """Player-car checks over race frames [from_frame, to_frame] (0x21F540 call index = race frame)."""
    import math
    rows = [r for r in parse_car_trace(calls_path) if spec.get("from_frame", 0) <= r[0] <= spec.get("to_frame", 1 << 30)]
    res = report.setdefault("car", {})
    res["frames"] = len(rows)
    if len(rows) < 2:
        report["failures"].append("car: no player-car trace (race never ran?)")
        return
    dist = sum(math.hypot(b[1] - a[1], b[2] - a[2]) for a, b in zip(rows, rows[1:]) if b[0] == a[0] + 1)
    stall = worst = 0
    for r in rows:
        stall = stall + 1 if math.hypot(r[3], r[4]) < spec.get("stall_speed", 2000) else 0
        worst = max(worst, stall)
    res.update({"first": rows[0][0], "last": rows[-1][0], "distance": round(dist, 1), "max_stall_frames": worst,
                "end_pos": [round(rows[-1][1], 1), round(rows[-1][2], 1)]})
    if rows[-1][0] < spec.get("to_frame", 0):
        report["failures"].append(f"car: trace ends at race frame {rows[-1][0]} < {spec['to_frame']} (crash or stall?)")
    if dist < spec.get("min_distance", 0):
        report["failures"].append(f"car: drove {dist:.0f} units < {spec['min_distance']}")
    if worst > spec.get("max_stall_frames", 1 << 30):
        report["failures"].append(f"car: stalled for {worst} frames > {spec['max_stall_frames']}")
    allrows = parse_car_trace(calls_path)
    for z in spec.get("zones", []):
        hit = [r[0] for r in allrows if z.get("after", -1) <= r[0] <= z.get("before", 1 << 30) and in_zone(z, r[1], r[2])]
        res.setdefault("zones", {})[z["name"]] = hit[0] if hit else None
        if not hit:
            report["failures"].append(f"car: never entered zone {z['name']}")
    speeds = [(r[0], math.hypot(r[3], r[4])) for r in rows]
    res["top_speed"] = int(max(v for _, v in speeds))
    if res["top_speed"] < spec.get("min_top_speed", 0):
        report["failures"].append(f"car: top speed {res['top_speed']} < {spec['min_top_speed']}")
    impacts, i = [], 0
    drop = spec.get("impact_drop", 2500)
    while i < len(speeds) - 6:
        c, v = speeds[i]
        if speeds[i + 6][0] == c + 6 and v - min(w for _, w in speeds[i + 1:i + 7]) >= drop:
            impacts.append(c)
            i += 30
        else:
            i += 1
    res["impacts"] = impacts
    if len(impacts) < spec.get("min_impacts", 0):
        report["failures"].append(f"car: {len(impacts)} impacts < {spec['min_impacts']}")
    ref_path = os.path.join(WORK, spec["ref"]) if spec.get("ref") else None
    if ref_path and os.path.exists(ref_path):  # local, disc-derived reference line (AI car); optional
        pts = json.load(open(ref_path))["lap"]
        far = 0.0
        for r in rows[::5]:
            far = max(far, min(math.hypot(p[0] - r[1], p[1] - r[2]) for p in pts))
        res["max_ref_dist"] = round(far, 1)
        if far > spec.get("max_ref_dist", 1e9):
            report["failures"].append(f"car: left the racing line by {far:.1f} > {spec['max_ref_dist']}")
    elif ref_path:
        res["ref"] = "missing (local reference line not present; check skipped)"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("route")
    ap.add_argument("--seconds", type=int)
    ap.add_argument("--every", type=int, default=1, help="dump every Nth presented frame")
    ap.add_argument("--out", help="output dir (default work/routes/<route>; use your own to avoid clobbering "
                                  "a parallel run of the same route)")
    ap.add_argument("--det", action="store_true", help="force PS2X_DETERMINISTIC=1 (development/verification runs; "
                                                       "fps numbers are then meaningless)")
    args = ap.parse_args()

    route_path = os.path.join(HERE, "routes", args.route + ".json")
    route = json.load(open(route_path))
    seconds = args.seconds or route.get("seconds", 100)
    out = os.path.abspath(args.out) if args.out else os.path.join(WORK, "routes", args.route)
    frames = os.path.join(out, "frames")
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(frames)

    env = dict(os.environ)
    if not (route.get("deterministic") or args.det):
        env.pop("PS2X_DETERMINISTIC", None)  # unpaced mode makes fps meaningless
    else:
        env["PS2X_DETERMINISTIC"] = "1"
    env.update({
        "PS2X_INPUT_SCRIPT": os.path.join(HERE, route["input"]),
        "PS2X_DUMP_FRAMES": frames,
        "PS2X_DUMP_EVERY": str(args.every),
        "PS2X_TRACE_TICKS": "1",
        "PS2X_SNDMOD_LOG": os.path.join(out, "sndmod.log"),
        "PS2X_VU1_STATS": "5",
        "PS2X_HEADLESS": os.environ.get("PS2X_HEADLESS", "1"),
    })
    env.setdefault("PS2X_MUTE", "1")  # test runs are never audible (the WAV dump is internal)
    # Each route gets its own memory card (optionally seeded from a local fixture in work/), so
    # tests never touch the player's saves. Other runner processes are left alone.
    mc = os.path.join(out, "mc0")
    fixture = route.get("mc_fixture")
    if fixture:
        shutil.copytree(os.path.join(WORK, fixture), mc)
    else:
        os.makedirs(mc)
    env["RTA_SAVE_DIR"] = mc
    if route.get("exit_after_ticks"):
        env["PS2X_EXIT_AFTER_TICKS"] = str(route["exit_after_ticks"])
    wav_path = os.path.join(out, "audio.wav")
    if route.get("audio"):
        env["PS2X_AUDIO_DUMP"] = wav_path
        env.setdefault("PS2X_MUTE", "1")
    calls_path = os.path.join(out, "calls.txt")
    if route.get("car") or route.get("cd_reads"):
        env["PS2X_TRACE_CALLS"] = "21f540@1820500:0x70" + (",27c048" if route.get("cd_reads") else "")
        env["PS2X_TRACE_CALLS_FILE"] = calls_path
        env["PS2X_TRACE_CALLS_MAX"] = "1000000"
    log_path = os.path.join(out, "run.log")
    cmd = ["perl", "-e", f"alarm {seconds}; exec @ARGV", RUN]
    if shutil.which("caffeinate"):
        cmd = ["caffeinate", "-d", "-i"] + cmd
    report_load = [round(v, 1) for v in os.getloadavg()]
    with open(log_path, "w") as log:
        rc = subprocess.run(cmd, cwd=WORK, env=env, stdout=log, stderr=subprocess.STDOUT, check=False).returncode

    report = {"route": args.route, "seconds": seconds, "failures": [], "checkpoints": {},
              "load_before": report_load, "load_after": [round(v, 1) for v in os.getloadavg()]}
    files = sorted(glob.glob(os.path.join(frames, "*.png")))
    report["frames"] = len(files)
    if not files:
        report["failures"].append("no frames presented")

    # flicker: blank/content alternation
    sizes = [os.path.getsize(f) for f in files]
    blank = [s < 6000 for s in sizes]
    worst = 0
    for i in range(0, max(0, len(blank) - 20)):
        window = blank[i:i + 20]
        flips = sum(1 for a, b in zip(window, window[1:]) if a != b)
        worst = max(worst, flips)
    report["max_flips_per_20_frames"] = worst
    if args.every == 1 and worst >= 6:
        report["failures"].append(f"flicker: {worst} blank/content flips within 20 frames")

    # checkpoints
    index = {int(re.search(r"frame_(\d+)", os.path.basename(f)).group(1)): f for f in files}
    by_read = {}
    for f in files:
        m = re.search(r"_r(\d+)\.png$", f)
        if m:
            by_read[int(m.group(1))] = f
    for cp in route.get("checkpoints", []):
        # "read" checkpoints use the pad-read clock (stable across runs); "frame" uses the presented index.
        table = by_read if "read" in cp else index
        target = cp["read"] if "read" in cp else cp["frame"]
        nearest = min(table, key=lambda k: abs(k - target)) if table else None
        if nearest is None:
            report["failures"].append(f"checkpoint {cp['name']}: no frame")
            continue
        path = table[nearest]
        mean, nonblack, flat = png_luma_stats(path, with_flat=True)
        report["checkpoints"][cp["name"]] = {"frame": os.path.basename(path), "mean_luma": round(mean, 1),
                                             "nonblack": round(nonblack, 3), "flat": round(flat, 3)}
        if cp.get("not_black") and nonblack < 0.05:
            report["failures"].append(f"checkpoint {cp['name']}: {os.path.basename(path)} is black")
        if "max_flat" in cp and flat > cp["max_flat"]:
            report["failures"].append(f"checkpoint {cp['name']}: {os.path.basename(path)} is {flat:.0%} one colour "
                                      f"(> {cp['max_flat']:.0%}; missing geometry?)")
        if "region_rgb" in cp:  # UI presence (dialogue box, HUD): mean colour of a box in fractional coords
            spec = cp["region_rgb"]
            try:
                from PIL import Image
                im = Image.open(path).convert("RGB")
                w, h = im.size
                fx0, fy0, fx1, fy1 = spec["box"]
                box = im.crop((int(fx0 * w), int(fy0 * h), int(fx1 * w), int(fy1 * h)))
                mean = list(box.resize((1, 1), Image.BOX).getpixel((0, 0)))
                report["checkpoints"][cp["name"]]["region_rgb"] = mean
                if max(abs(m - t) for m, t in zip(mean, spec["rgb"])) > spec.get("tol", 35):
                    report["failures"].append(f"checkpoint {cp['name']}: region {spec['box']} mean RGB {mean} != "
                                              f"{spec['rgb']} (+-{spec.get('tol', 35)}); {spec.get('what', 'UI element')} missing?")
            except ImportError:
                report["checkpoints"][cp["name"]]["region_rgb"] = "skipped (no PIL)"
        golden = os.path.join(WORK, "golden", args.route, cp["name"] + ".png")
        if os.path.exists(golden):
            gmean, gnon = png_luma_stats(golden)
            report["checkpoints"][cp["name"]]["golden_luma_delta"] = round(abs(gmean - mean), 1)
            if abs(gmean - mean) > cp.get("golden_tolerance", 12):
                report["failures"].append(f"checkpoint {cp['name']}: luma differs from golden by {abs(gmean-mean):.1f}")

    # fps + bad log lines
    text = open(log_path, errors="replace").read()
    fps = [float(m) for m in re.findall(r"\[fps\] game=([0-9.]+)", text)]
    report["fps_samples"] = fps
    steady = fps[3:] if len(fps) > 3 else fps
    if steady:
        report["fps_min"] = min(steady)
        report["fps_median"] = sorted(steady)[len(steady) // 2]
        if report["fps_median"] < route.get("min_fps", 45):
            report["failures"].append(f"median fps {report['fps_median']} < {route.get('min_fps', 45)}")
    # VU1: 0 interpreter fallbacks, native code must actually run, no uncatalogued images.
    # 3D windows are tagged per stats window (mscal/s above a threshold), and each [fps] line
    # is attributed to the most recent window's tag.
    vu1_rx = re.compile(r"\[vu1rec\] [^:]*: mscal=(\d+) native=(\d+) fallback=(\d+).*?\| ([0-9.]+) mscal/s")
    vu1 = [(int(m.group(1)), int(m.group(2)), int(m.group(3))) for m in vu1_rx.finditer(text)]
    if vu1:
        report["vu1_mscal"], report["vu1_native"], report["vu1_fallbacks"] = vu1[-1]
        if vu1[-1][2] > 0:
            report["failures"].append(f"vu1: {vu1[-1][2]} interpreter fallbacks")
        if route.get("expects_3d") and vu1[-1][1] == 0:
            report["failures"].append("vu1: no native VU1 calls in a 3D route")
    elif route.get("expects_3d"):
        report["failures"].append("vu1: no [vu1rec] stats lines (recompiler not active?)")
    noimg = sorted(set(re.findall(r"VU1 image ([0-9a-f]{16}) has no native code", text)) |
                   set(m.group(1) + "@" + m.group(2) for m in re.finditer(r"VU1 image ([0-9a-f]{16}) entry (0x[0-9a-f]+) has no native code", text)))
    if noimg:
        report["vu1_uncatalogued_images"] = noimg
        report["failures"].append(f"vu1: {len(noimg)} uncatalogued image(s)/entries: {', '.join(noimg)}")
    fps3d, in3d, cur_read, excluded = [], False, None, 0
    excl = route.get("fps_exclude_reads", [])
    for line in text.splitlines():
        m = vu1_rx.search(line)
        if m:
            in3d = float(m.group(4)) >= route.get("mscal_3d_threshold", 1000)
        m = re.search(r"_r(\d+)\.png\] File saved", line)
        if m:
            cur_read = int(m.group(1))
        m = re.search(r"\[fps\] game=([0-9.]+)", line)
        if m and in3d:
            if cur_read is not None and any(a <= cur_read <= b for a, b in excl):
                excluded += 1
                continue
            fps3d.append(float(m.group(1)))
    report["fps3d_excluded_load_samples"] = excluded
    steady3d = fps3d[2:] if len(fps3d) > 2 else fps3d
    report["fps3d_samples"] = len(fps3d)
    if steady3d:
        report["fps3d_min"] = min(steady3d)
        report["fps3d_median"] = sorted(steady3d)[len(steady3d) // 2]
        if report["fps3d_median"] < route.get("min_fps_3d", 0):
            report["failures"].append(f"3D median fps {report['fps3d_median']} < {route['min_fps_3d']}")
        srt = sorted(steady3d)
        report["fps3d_1pct_low"] = srt[int(len(srt) * 0.01)]
        if report["fps3d_1pct_low"] < route.get("min_fps_1pct_low_3d", 0):
            report["failures"].append(f"3D 1% low fps {report['fps3d_1pct_low']} < {route['min_fps_1pct_low_3d']}")
    elif route.get("expects_3d"):
        report["failures"].append("no 3D fps samples")
    for rx, tag in BAD_LOG_PATTERNS:
        hits = rx.findall(text)
        if hits:
            report["failures"].append(f"{tag}: {len(hits)} log hits")

    report["exit_code"] = rc
    if rc >= 128 + 1:  # a wrapper (shell/caffeinate) may report a signal death as 128+N
        rc = -(rc - 128)
    if rc < 0 and rc != -14:  # killed by a signal other than the alarm (SIGALRM)
        report["failures"].append(f"crash: runner killed by signal {-rc}")
    if route.get("audio"):
        check_audio(route["audio"], wav_path, files, report)
    if route.get("car"):
        check_car(route["car"], calls_path, report)
    if route.get("cd_reads"):
        check_cd_reads(route["cd_reads"], calls_path, report)
    if by_read:
        report["final_read"] = max(by_read)
        if max(by_read) < route.get("min_final_read", 0):
            report["failures"].append(f"pad: last frame at read {max(by_read)} < {route['min_final_read']} (hang?)")

    report["passed"] = not report["failures"]
    json.dump(report, open(os.path.join(out, "report.json"), "w"), indent=1)
    print(json.dumps({k: v for k, v in report.items() if k != "fps_samples"}, indent=1))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
