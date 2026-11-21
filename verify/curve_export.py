"""M2 data export (N28): a preset's static curve f and linear model for the Tube Color stage, from the M0 circuits.

The stage is y = post(x~ + DCB(f(d x~)/d - x~)), x~ = pre(x): f is a static curve with f(0) = 0, f'(0) = 1, on the
digital scale u = d x (u = 1: the drive at which -18 dBFS RMS pink noise gives the preset's reference power P_ref at
drive 100 %, N28 candidate B).

Curve (Xyce harmonic balance, 1 kHz, one period of the steady state, a ladder of drive amplitudes):
  1. each run's output is de-embedded from the amplifier's linear response at every harmonic (the small-signal
     response from data/ref/<preset>_response.csv and the fundamental's own phase): what is left is the static
     curve plus whatever memory the nonlinearity has (grid current charging the coupling capacitor at high drive)
  2. the rising and falling halves of the period are averaged: the mid-line
  3. the curve is stitched by level: up to the largest amplitude without grid current (the calibrated range), the
     mid-line of that run; beyond, each run's outer part (between the previous run's peak and its own), the shape
     the peaks have at that drive. Monotonic, smoothed, and flat beyond the last run (defined, never extrapolated)
Linear model: the small-signal response fitted with the stage's sections (pre: the interstage coupling; post: the
output transformer's low and high end).

  python curve_export.py set-300b       data/curves/<preset>.json (+ the checks printed)
"""
import json, math, os, sys
from concurrent.futures import ProcessPoolExecutor

import numpy as np
from scipy import optimize, signal

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import curve_ladder as L  # noqa: E402

OUT = os.path.join(HERE, "data", "curves")
REF = os.path.join(HERE, "data", "ref")
# N28 candidate B: the reference output power at drive 100 % (-18 dBFS RMS pink noise, RMS 0.126 of full scale)
P_REF = {"set-300b": 0.279, "set-300b-x": 0.279, "set-845": 0.077, "set-2a3": 0.115}   # set-300b-x: the same P_ref
PINK_RMS = 10 ** (-18 / 20)
U_MAX = 6.0           # the table: 400 % x 1.41 = 5.64 inside
N_TABLE = 6001        # 0.002 per step
LADDER = (0.25, 0.5, 0.75, 1.0, 1.1, 1.2, 1.3, 1.5, 1.75, 2.0, 2.5, 3.0, 4.0, 5.0, 6.5, 8.0)


def response(preset):
    a = np.loadtxt(os.path.join(REF, f"{preset}_response.csv"), delimiter=",", skiprows=1)
    return a[:, 0], 10 ** (a[:, 1] / 20) * np.exp(1j * np.radians(a[:, 2]))


def h_at(f, h, f0):
    lf = np.log(f)
    return np.interp(math.log(f0), lf, np.abs(h)) * np.exp(1j * np.interp(math.log(f0), lf, np.unwrap(np.angle(h))))


def deembed(run, f, h, phase1):
    """The output with the linear response taken out at every harmonic (relative to 1 kHz, plus the fundamental's
    small-signal phase), on the input's time base."""
    vin, vout = np.asarray(run["vin"]), np.asarray(run["vout"])
    n = len(vin)
    Y = np.fft.rfft(vout)
    for k in range(1, len(Y)):
        Y[k] /= h_at(f, h, 1000.0 * k) * np.exp(1j * phase1 * k) if 1000.0 * k <= f[-1] else 1.0
    return vin, np.fft.irfft(Y, n)


def mid_line(vin, z):
    g, up, dn = L.branches(vin, z)
    return g, (up + dn) / 2, float(np.max(np.abs(up - dn)) / max(1e-12, np.max(np.abs((up + dn) / 2))))


def fit_linear(preset):
    """pre: HP1 at the interstage coupling (computed); post: HP2 + LP2 (+ LP1) fitted to the rest of the response."""
    f, h = response(preset)
    pre_fc = {"set-300b": 1 / (2 * math.pi * 0.22e-6 * 220e3), "set-300b-x": 1 / (2 * math.pi * 0.22e-6 * 220e3)}.get(preset, 3.3)
    sel = (f >= 10) & (f <= 200e3)
    fs, hs = f[sel], h[sel]
    s = 2j * np.pi * fs

    def model(p):
        f_hp, q_hp, f_lp, q_lp, f_lp1 = p
        w_hp, w_lp, w_l1 = 2 * np.pi * f_hp, 2 * np.pi * f_lp, 2 * np.pi * f_lp1
        pre = s / (s + 2 * np.pi * pre_fc)
        hp2 = s * s / (s * s + s * w_hp / q_hp + w_hp ** 2)
        lp2 = w_lp ** 2 / (s * s + s * w_lp / q_lp + w_lp ** 2)
        lp1 = w_l1 / (s + w_l1)
        return pre * hp2 * lp2 * lp1

    s1k = 2j * np.pi * 1000.0

    def err(p):
        m = model(p)
        m1 = model_at(p, s1k)
        r = m / m1 / hs
        wgt = np.where((fs >= 20) & (fs <= 20000), 3.0, 1.0)
        return np.r_[wgt * 20 * np.log10(np.abs(r)), wgt * np.degrees(np.angle(r)) / 10]

    def model_at(p, ss):
        f_hp, q_hp, f_lp, q_lp, f_lp1 = p
        w_hp, w_lp, w_l1 = 2 * np.pi * f_hp, 2 * np.pi * f_lp, 2 * np.pi * f_lp1
        return (ss / (ss + 2 * np.pi * pre_fc)) * (ss * ss / (ss * ss + ss * w_hp / q_hp + w_hp ** 2)) * \
               (w_lp ** 2 / (ss * ss + ss * w_lp / q_lp + w_lp ** 2)) * (w_l1 / (ss + w_l1))

    r = optimize.least_squares(err, [5.0, 0.7, 60e3, 0.7, 150e3], bounds=([0.5, 0.3, 10e3, 0.3, 20e3], [40, 3, 400e3, 3, 2e6]))
    p = r.x
    m = model(p) / model_at(p, s1k)
    band = (fs >= 20) & (fs <= 20000)
    d_db = 20 * np.log10(np.abs(m / hs))
    d_deg = np.degrees(np.angle(m / hs))
    lin = {"pre": [{"kind": "HighPass1", "f": pre_fc, "q": 0.7071}],
           "post": [{"kind": "HighPass2", "f": p[0], "q": p[1]}, {"kind": "LowPass2", "f": p[2], "q": p[3]},
                    {"kind": "LowPass1", "f": p[4], "q": 0.7071}]}
    fit = {"max_dB_20_20k": float(np.max(np.abs(d_db[band]))), "max_deg_20_20k": float(np.max(np.abs(d_deg[band]))),
           "max_dB_10_200k": float(np.max(np.abs(d_db)))}
    return lin, fit


if __name__ == "__main__":
    b = sys.argv[1] if len(sys.argv) > 1 else "set-300b"
    os.makedirs(OUT, exist_ok=True)
    f, h = response(b)
    lin, fit = fit_linear(b)
    print(f"linear model: {json.dumps(lin)}\n  fit: {fit}")

    rated = L.RATED_DRIVE[b]
    with ProcessPoolExecutor(12) as ex:
        runs = list(ex.map(L.run, [(b, m * rated, 100) for m in LADDER]))
    bad = [r["amp"] for r in runs if "error" in r]
    if bad:
        raise SystemExit(f"runs failed at {bad}")
    # small-signal gain and phase at 1 kHz (the smallest run's fundamental)
    r0 = runs[0]
    X0, Y0 = np.fft.rfft(r0["vin"])[1], np.fft.rfft(r0["vout"])[1]
    G = abs(Y0 / X0)
    phase1 = float(np.angle(Y0 / X0))
    # u = 1: the input amplitude scale at which -18 dBFS RMS pink noise gives P_ref (small-signal gain)
    v_out_rms = math.sqrt(P_REF[b] * 8.0)
    V1 = v_out_rms / G / PINK_RMS
    print(f"small-signal gain {G:.4f}, phase at 1 kHz {math.degrees(phase1):+.2f} deg; u = 1 is {V1:.3f} V at the input "
          f"(rated drive {rated:.3f} V = u {rated / V1:.3f})")

    curves = []
    for m, r in zip(LADDER, runs):
        vin, z = deembed(r, f, h, phase1)
        g, mid, loop = mid_line(vin, z)
        vg, vk = np.array(r["vg"]), np.array(r["vk"])
        grid = float(np.max(vg - vk))
        curves.append({"mult": m, "amp": r["amp"], "g": g, "mid": mid, "loop": loop, "vgk_max": grid})
        print(f"  x{m:<5} {r['amp']:7.3f} V (u {r['amp'] / V1:5.2f})  loop after de-embedding {100 * loop:6.2f} %  max vg-vk {grid:+7.2f} V")
    # the calibrated range: up to the largest run with the grid still negative
    valid = [c for c in curves if c["vgk_max"] < 0.0]
    inner = valid[-1]
    u_valid = inner["amp"] / V1
    # stitch: the inner run's mid-line, then each larger run's outer part
    xs, ys = [inner["g"]], [inner["mid"]]
    prev = inner["amp"]
    for c in curves[curves.index(inner) + 1:]:
        g, mid = c["g"], c["mid"]
        sel = (np.abs(g) > prev) & (np.abs(g) <= c["amp"])
        xs.append(g[sel]); ys.append(mid[sel])
        prev = c["amp"]
    x = np.concatenate(xs); y = np.concatenate(ys)
    o = np.argsort(x); x, y = x[o], y[o]
    u = x / V1
    fu = y / (G * V1)
    # monotonic (an output that grows with the input) and smoothed
    grid_u = np.linspace(-U_MAX, U_MAX, N_TABLE)
    u_data = float(min(-u.min(), u.max()))
    fu_m = np.maximum.accumulate(fu)
    table = np.interp(grid_u, u, fu_m)        # flat beyond the data (np.interp holds the end values)
    # the stitching's joins smoothed; the calibrated range lightly (a steeper smoothing there, or none at its edge,
    # moved H5 at the edge further from the circuit: 2.7 dB against 1.7)
    sm = signal.savgol_filter(table, 31, 3)
    core = np.abs(grid_u) <= 0.9 * u_valid
    sm[core] = signal.savgol_filter(table, 9, 3)[core]
    table = np.maximum.accumulate(sm)
    table -= np.interp(0.0, grid_u, table)    # f(0) = 0
    slope0 = (np.interp(0.01, grid_u, table) - np.interp(-0.01, grid_u, table)) / 0.02
    table /= slope0                           # f'(0) = 1 (the small-signal gain is the linear model's)
    print(f"calibrated range |u| <= {u_valid:.3f} (no grid current up to {inner['mult']} x rated drive); data to |u| {u_data:.2f};"
          f" table to {U_MAX}; f(+-{U_MAX}) = {table[-1]:+.3f} / {table[0]:+.3f}")
    out = {"preset": b, "u_max": U_MAX, "n": N_TABLE, "f": table.tolist(), "linear": lin, "linear_fit": fit,
           # the ranges (N28): verified against the circuit up to rated power (u_rated); no grid current up to u_no_grid
           # (the transition); beyond it the curve is defined, not calibrated (a static curve cannot follow the grid
           # current's bias shift); data to u_data, flat beyond
           "calibration": {"P_ref_W": P_REF[b], "pink_rms": PINK_RMS, "gain_1k": G, "u1_input_V": V1, "rated_drive_V": rated,
                           "u_rated": rated / V1, "u_no_grid": u_valid, "u_calibrated": u_valid, "u_data": u_data},
           "ladder": [{k: c[k] for k in ("mult", "amp", "loop", "vgk_max")} for c in curves]}
    json.dump(out, open(os.path.join(OUT, f"{b}.json"), "w"), indent=0)
