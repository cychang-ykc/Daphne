"""M2 experiment (N28): h2-pure, a curve that adds second harmonic and nothing odd, on set-300b's linear model and
loudspeaker loads (only the curve differs: the load's response and the kind of distortion can be judged apart).

  f(u) = u + k x1^2 ln(1 + (u / x1)^2)

For small u, f = u + k u^2: H2 only. f - u is even, so no odd harmonic at any level (no H3, H5 ...), and f is never
clipped. f'(u) = 1 + 2 k u / (1 + (u / x1)^2) >= 1 - k x1: with k x1 = 0.8 it rises everywhere (the slope never below
0.2), not only within the table. Large inputs bend the u^2 term (a log): some H4 (even) appears there.
k: H2 = k d A / 2 for a sine of amplitude A at drive d; A = 0.178 (-18 dBFS RMS) at drive 400 % -> 10 %.

  python h2_pure.py        data/curves/h2-pure.json (the stage's table format) and the H2 table printed
"""
import json, math, os

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "data", "curves")
U_MAX, N = 6.0, 6001
A_REF = 10 ** (-18 / 20) * math.sqrt(2)   # a sine at -18 dBFS RMS (RMS 0.126): amplitude 0.178
K = 0.10 * 2 / (4.0 * A_REF)              # H2 10 % at drive 400 % (small-signal)
X1 = 0.8 / K                              # k x1 = 0.8: the slope never below 0.2


def f(u):
    return u + K * X1 * X1 * np.log1p((u / X1) ** 2)


def harmonics(d, a, n=8192):
    th = 2 * np.pi * np.arange(n) / n
    y = f(d * a * np.cos(th)) / d
    s = np.abs(np.fft.rfft(y)) / n * 2
    return s[1], s[2:7] / s[1]


if __name__ == "__main__":
    u = np.linspace(-U_MAX, U_MAX, N)
    table = f(u)
    slope = np.diff(table) / np.diff(u)
    print(f"k {K:.4f}, x1 {X1:.3f}; f(+-{U_MAX}) = {table[-1]:+.3f} / {table[0]:+.3f}; slope {slope.min():.3f} .. {slope.max():.3f}"
          f" (monotonic: {bool(np.all(slope > 0))}); f(0) {f(np.array(0.0)):.1e}")
    rows = []
    print(f"{'drive':>6} {'H2 %':>7} {'H2 dB':>7} {'H3 dB':>8} {'H4 dB':>8}   (1 kHz sine at -18 dBFS RMS)")
    for d in np.round(np.arange(0.05, 4.0001, 0.05), 4):
        _, h = harmonics(d, A_REF)
        rows.append([float(d), float(100 * h[0])])
        if abs(d * 4 - round(d * 4)) < 1e-9:
            db = lambda v: 20 * math.log10(max(v, 1e-30))
            print(f"{d * 100:5.0f}% {100 * h[0]:7.3f} {db(h[0]):7.1f} {db(h[1]):8.1f} {db(h[2]):8.1f}")
    # at full scale and at 1.41 x (the DSD path's overs), drive 400 %
    for a in (1.0, 1.41):
        _, h = harmonics(4.0, a)
        print(f"drive 400 %, a sine of amplitude {a}: H2 {100 * h[0]:.1f} %, H3 {20 * math.log10(max(h[1], 1e-30)):.0f} dB, H4 {20 * math.log10(h[2]):.1f} dB")
    lin = json.load(open(os.path.join(OUT, "set-300b.json")))["linear"]
    out = {"preset": "h2-pure", "u_max": U_MAX, "n": N, "f": table.tolist(), "linear": lin,
           "calibration": {"u_rated": U_MAX, "u_no_grid": U_MAX, "u_data": U_MAX, "k": K, "x1": X1},
           "h2_table": {"signal": "1 kHz sine at -18 dBFS RMS", "drive_pct": rows}}
    json.dump(out, open(os.path.join(OUT, "h2-pure.json"), "w"), indent=0)
