"""M2 (N28): how much room the Tube Color output needs below full scale (in place of "set the digital volume to -3 dB").

The six test tracks, whole, through the engine (render-dsp) with the defaults (set-300b, bookshelf-vented-smooth,
conservative alignment from `daphne levels`, makeup auto), the stage alone (the digital volume at -12.04 dB: the
stage cancels it, the limiter rests; the output times 4): at each drive the output's true peak (4x oversampled,
BS.1770-4 annex 2 style) and its sample peak, dBFS. Drive 0 is the source itself (the stage idles).
The room needed: the largest true peak above 0 dBFS plus 0.5 dB (the limiter's knee starts at -0.5 dBFS).

  python headroom_study.py <tracks.txt> [--limiter H]
      --limiter H: instead, the full chain at digital volume 0 dB with the engine's reserve (the build's
      kColorHeadroomDb, H for the report): what the soft limiter does (render-dsp's color_limited, the loudness change)
"""
import concurrent.futures as cf, json, math, os, re, subprocess, sys, tempfile

import numpy as np
import soundfile as sf
from scipy import signal

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import loudness as LU          # noqa: E402
import makeup_table as MT      # noqa: E402

DRIVES = (0.0, 0.1, 0.25, 0.5, 1.0, 1.5, 2.0)
VOL_STAGE = -20 * math.log10(4.0)


def true_peak_db(y):
    up = signal.resample_poly(y, 4, 1, axis=0)
    return 20 * math.log10(max(np.max(np.abs(up)), 1e-20)), 20 * math.log10(max(np.max(np.abs(y)), 1e-20))


def render(path, drive, align_db, vol_db, td, tag):
    dst = os.path.join(td, tag + ".wav")
    st = os.path.join(td, tag + ".json")
    json.dump({"core": "pcm", "output": {"rate": "native"},
               "color": {"enabled": True, "preset": "set-300b", "drive": drive, "load": "bookshelf-vented-smooth"},
               "volume": {"mode": "digital", "db": vol_db}}, open(st, "w"))
    r = subprocess.run([MT.CLI, "render-dsp", path, dst, "--settings", st, "--float", "--color-align-db", f"{align_db:.4f}"],
                       capture_output=True, text=True, encoding="utf-8", errors="replace")
    if r.returncode:
        raise RuntimeError(r.stderr)
    m = re.search(r"color_limited (\d+)", r.stdout)
    y, rate = sf.read(dst, always_2d=True)
    os.remove(dst); os.remove(st)
    return y, rate, int(m.group(1)) if m else 0


def main():
    tracks = [l.rstrip("\r\n").split("\t") for l in open(sys.argv[1], encoding="utf-8") if l.strip()]
    lim = "--limiter" in sys.argv
    res = {}
    with tempfile.TemporaryDirectory() as td, cf.ThreadPoolExecutor(4) as pool:
        jobs = {}
        for label, path in tracks:
            lufs, p999 = MT.levels(path)
            g = MT.conservative_db(lufs, p999)
            res[label] = {"align_db": g}
            for d in DRIVES:
                vol = 0.0 if lim else VOL_STAGE
                jobs[(label, d)] = (pool.submit(render, path, d, g, vol, td, f"{label}_{d}"), vol)
        for label, path in tracks:
            x, rate = sf.read(path, always_2d=True)
            l0 = LU.integrated(x[:, :2], rate)
            for d in DRIVES:
                fut, vol = jobs[(label, d)]
                y, rate, limited = fut.result()
                y = y * 10 ** (-vol / 20)
                tp, sp = true_peak_db(y)
                res[label][str(d)] = {"true_peak_db": tp, "sample_peak_db": sp, "limited_frames": limited,
                                      "limited_s": limited / rate, "loudness_change_db": LU.integrated(y, rate) - l0}
            print("done", label, flush=True)
    if lim:
        print("\nthe full chain at digital volume 0 dB, with the engine's reserve: the soft limiter's work per track and drive "
              "(seconds limited / output loudness against the original, dB)")
    else:
        print("\nbefore the limiter: true peak / sample peak, dBFS (drive 0: the source)")
    print(f"{'track':<14}" + "".join(f"{d * 100:>16.0f}%" for d in DRIVES))
    for label, r in res.items():
        if lim:
            print(f"{label:<14}" + "".join(f"{r[str(d)]['limited_s']:9.2f}s/{r[str(d)]['loudness_change_db']:+5.2f}" for d in DRIVES))
        else:
            print(f"{label:<14}" + "".join(f"{r[str(d)]['true_peak_db']:+8.2f}/{r[str(d)]['sample_peak_db']:+6.2f}" for d in DRIVES))
    if not lim:
        worst = max(r[str(d)]["true_peak_db"] for r in res.values() for d in DRIVES)
        worst_src = max(r["0.0"]["true_peak_db"] for r in res.values())
        worst_drive = {d: max(r[str(d)]["true_peak_db"] for r in res.values()) for d in DRIVES}
        print("largest true peak per drive: " + "  ".join(f"{d * 100:.0f}% {v:+.2f}" for d, v in worst_drive.items()))
        print(f"largest true peak: {worst:+.2f} dBTP (the sources alone: {worst_src:+.2f}); room needed = {max(0.0, worst) + 0.5:.2f} dB")
    out = os.path.join(os.path.dirname(sys.argv[1]), "headroom_limiter.json" if lim else "headroom_study.json")
    json.dump(res, open(out, "w"), indent=1)


if __name__ == "__main__":
    main()
