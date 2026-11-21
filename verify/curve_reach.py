"""M2 (N28): how much of the music reaches past a preset's calibrated ranges at a drive. u = d x~ (x~: the preset's
pre sections on the stage's oversampled signal, so inter-sample peaks count): the share of samples above u_rated
(past rated power) and above u_no_grid (the output tube's grid conducting), from data/curves/<preset>.json.

  python curve_reach.py <tracks.txt> [preset] [seconds] [drives...]
"""
import json, os, sys

import numpy as np
import soundfile as sf
from scipy import signal

HERE = os.path.dirname(os.path.abspath(__file__))


def factor(rate):
    fam = 44100 if rate % 44100 == 0 else 48000
    l = 1
    while rate * l < 8 * fam:
        l += 1
    return l


def main():
    tracks = sys.argv[1]
    preset = sys.argv[2] if len(sys.argv) > 2 else "set-300b"
    secs = float(sys.argv[3]) if len(sys.argv) > 3 else 40.0
    drives = [float(x) for x in sys.argv[4:]] or [1.0, 1.25, 1.5, 1.75, 2.0]
    d = json.load(open(os.path.join(HERE, "data", "curves", f"{preset}.json")))
    ur, ug = d["calibration"]["u_rated"], d["calibration"]["u_no_grid"]
    pre = d["linear"]["pre"]
    print(f"{preset}: u_rated {ur:.3f}, u_no_grid {ug:.3f}")
    print(f"{'track':<14}" + "".join(f"{f'{x * 100:.0f}% >rated / >grid':>26}" for x in drives))
    rows = {}
    for line in open(tracks, encoding="utf-8"):
        if not line.strip():
            continue
        label, path = line.rstrip("\n").split("\t")
        info = sf.info(path)
        x, rate = sf.read(path, start=int(info.frames * 0.35), frames=int(secs * info.samplerate), always_2d=True)
        l = factor(rate)
        xo = signal.resample_poly(x[:, :2], l, 1, axis=0, window=("kaiser", 12.0))
        hi = rate * l
        for s in pre:
            assert s["kind"] == "HighPass1"
            xo = signal.sosfilt(signal.butter(1, s["f"], "highpass", fs=hi, output="sos"), xo, axis=0)
        a = np.abs(xo)
        r = {str(dr): {"over_rated_pct": 100.0 * float(np.mean(a * dr > ur)), "over_grid_pct": 100.0 * float(np.mean(a * dr > ug))}
             for dr in drives}
        rows[label] = r
        print(f"{label:<14}" + "".join(f"{r[str(dr)]['over_rated_pct']:12.3f} % {r[str(dr)]['over_grid_pct']:9.3f} %" for dr in drives), flush=True)
    json.dump(rows, open(os.path.join(os.path.dirname(tracks), f"{preset}_reach.json"), "w"), indent=1)


if __name__ == "__main__":
    main()
