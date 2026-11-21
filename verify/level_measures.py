"""M2 (N28): which level measure predicts the drive at which a track reaches a given distortion (drive_loudness.py's
results): per track, the drive offset (dB) against 6-vocal-piano for the same residual, against each measure's level.

  python level_measures.py <tracks.txt> <drive_loudness.py's out dir>
"""
import json, math, sys, os
import numpy as np, soundfile as sf
from scipy import signal
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import loudness as LU
OUT = sys.argv[2]
res = json.load(open(os.path.join(OUT, "drive_loudness.json")))
paths = dict(l.rstrip("\n").split("\t") for l in open(sys.argv[1], encoding="utf-8") if l.strip())
tr = res["tracks"]
meas = {}
for k in tr:
    info = sf.info(paths[k]); x, r = sf.read(paths[k], start=int(info.frames * 0.35), frames=int(40 * info.samplerate), always_2d=True); x = x[:, :2]
    b, a = signal.butter(1, 3.29, "highpass", fs=r); x = signal.lfilter(b, a, x, axis=0)
    w = int(3 * r); st = [LU.integrated(x[i:i + w], r) for i in range(0, len(x) - w, r)]
    x2 = x * x
    meas[k] = {"LUFS excerpt": tr[k]["lufs_excerpt"], "RMS (unweighted)": 10 * math.log10(x2.mean()),
               "H2 moment sqrt(var(x^2)/E x^2)": 20 * math.log10(math.sqrt(x2.var() / x2.mean())),
               "99.9th pct |x|": 20 * math.log10(np.quantile(np.abs(x), 0.999)),
               "short-term LUFS, 95th pct": float(np.quantile(st, 0.95))}
for p in ("h2-pure", "set-300b"):
    # each track's residual against log effective drive (all its points); the offset (dB of drive) that aligns it with
    # the median track over the reference's residual range
    curves = {}
    for k in tr:
        pts = sorted({(v["drive_eff"], v["residual_db"]) for m in ("now", "track", "excerpt") for v in tr[k]["presets"][p][m].values()})
        curves[k] = (np.array([20 * math.log10(a) for a, _ in pts]), np.array([b for _, b in pts]))
    ref = "6-vocal-piano"
    off = {}
    for k, (xd, yr) in curves.items():
        # residual levels both cover; for each, drive dB needed by k minus by ref
        lo, hi = max(yr.min(), curves[ref][1].min()), min(yr.max(), curves[ref][1].max())
        lv = np.linspace(lo, hi, 20)
        dk = np.interp(lv, np.maximum.accumulate(yr), xd); dr = np.interp(lv, np.maximum.accumulate(curves[ref][1]), curves[ref][0])
        off[k] = float(np.median(dk - dr))
    print(f"\n{p}: the drive (dB) each track needs, relative to {ref}, for the same distortion; and each measure's level, relative")
    names = list(next(iter(meas.values())))
    print(f"{'track':<14}{'needed':>8}" + "".join(f"{n[:24]:>26}" for n in names))
    for k in tr:
        print(f"{k:<14}{off[k]:8.1f}" + "".join(f"{-(meas[k][n] - meas[ref][n]):26.1f}" for n in names))
    for n in names:
        e = [off[k] - (-(meas[k][n] - meas[ref][n])) for k in tr]
        print(f"  {n:<34} residual misalignment after normalising by it: spread {max(e) - min(e):4.1f} dB")
