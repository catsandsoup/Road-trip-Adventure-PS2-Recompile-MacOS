#!/usr/bin/env python3
"""Summarise a PS2X_AUDIO_DUMP WAV (s16 stereo): RMS/peak/zero-crossing per window.

Usage: wav_report.py dump.wav [window_seconds]
Header sizes are ignored (the dump can be cut short by a kill).
"""
import array, math, struct, sys

def main():
    path = sys.argv[1]
    win = float(sys.argv[2]) if len(sys.argv) > 2 else 1.0
    raw = open(path, 'rb').read()
    assert raw[:4] == b'RIFF' and raw[8:12] == b'WAVE'
    ch = struct.unpack_from('<H', raw, 22)[0]
    rate = struct.unpack_from('<I', raw, 24)[0]
    a = array.array('h'); data = raw[44:]; a.frombytes(data[: len(data) // (2 * ch) * 2 * ch])
    n = len(a) // ch; w = int(rate * win)
    print(f"file={path} rate={rate} ch={ch} seconds={n/rate:.2f}")
    silent = total = 0; peak_all = 0
    for start in range(0, n, w):
        seg = a[start * ch:(start + w) * ch]
        if not seg: break
        rms = math.sqrt(sum(x * x for x in seg) / len(seg)); peak = max(abs(x) for x in seg)
        left = seg[0::ch]; right = seg[1::ch]
        lr = math.sqrt(sum(x*x for x in left)/len(left)); rr = math.sqrt(sum(x*x for x in right)/len(right))
        zc = sum(1 for i in range(1, len(left)) if (left[i-1] < 0) != (left[i] < 0)) / (len(left) / rate)
        db = 20 * math.log10(rms / 32768) if rms > 0 else -120.0
        total += 1; silent += rms < 30; peak_all = max(peak_all, peak)
        print(f"t={start/rate:7.2f}s rms={rms:8.1f} ({db:6.1f} dBFS) L={lr:7.1f} R={rr:7.1f} peak={peak:6d} zcr={zc:7.0f}/s")
    print(f"windows={total} silent={silent} peak={peak_all}")

if __name__ == '__main__':
    main()
