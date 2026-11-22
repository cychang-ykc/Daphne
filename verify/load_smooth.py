"""M2 (N28): the loudspeaker loads, smoothed: only their overall tilt, no resonance.

The listening: bookshelf-vented sounds "compressed, split, smeared" (its resonances ring: load_diag.py), the 8 ohm
resistor clean; the warmth the loads bring (the highs drawn in, the lows full) is wanted. So, for each preset (and pp
feedback step) and loudspeaker:
  1. the response change H = (Z / (Z + Zout)) / (8 / (8 + Zout)) (load_filters.target: the circuit's Zout), its power
     smoothed over 1/2 octave (a rectangular window on a log-frequency axis)
  2. fitted (least squares in dB, 20 Hz - 20 kHz, even on a log axis) with a few minimum-phase filters: a second-order
     low shelf, a second-order high shelf and up to three peaking filters, nothing below 30 Hz. Every pole pair of Q
     0.5 .. 0.7 (the poles alone set how long a filter rings; below 0.5 a pair splits into two real poles, the lower
     of which decays more slowly than the pair at 0.5, critically damped, the fastest: a first fit with Q down to 0.2
     rang longer than the load itself), every zero pair of Q <= 1.0 (zeros do not ring). Both Qs of a peaking filter
     in 0.5 .. 0.7 would hold it to +-2.9 dB: too little for the loads' tilt (250 Hz -1.6 dB of -2.5, 2.5 kHz +1.4 dB
     of +0.7, and longer ringing); with the zeros up to 1.0 the tilt is kept and the ringing shortest (N28). The
     number of peaking filters: the fewest that bring the fit within 0.3 dB RMS of the smoothed curve (three at most)
  3. as the stage's sections (General2: the poles' frequency and Q, the numerator in S = s / w_pole), checked at the
     stage's rates against the smoothed curve
  4. each one's makeup: the median of its loudness change on the six test tracks (as load_filters.py)

  python load_smooth.py [tracks.txt]     adds "<load>-smooth" to data/loads/loads.json
"""
import json, math, os, sys

import numpy as np
from scipy import optimize

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import load_filters as LF  # noqa: E402

OCT = 0.5
QMAX = 0.7
QMIN = 0.5
ZQMAX = 1.0   # the peaking filters' zeros
BASES = ("bookshelf-vented", "bookshelf-sealed")


def smoothed_db(f, h, frac=OCT):
    p = np.abs(h) ** 2
    lf = np.log2(f)
    out = np.empty_like(f)
    for i, x in enumerate(lf):
        sel = np.abs(lf - x) <= frac / 2
        out[i] = np.mean(p[sel])
    return 10 * np.log10(out)


# ---- the filters, as analog (b, a) in s (rad/s): b2 s^2 + b1 s + b0 over a2 s^2 + a1 s + a0
def low_shelf(fc, g_db, q):
    A = 10 ** (g_db / 40); w = 2 * math.pi * fc
    # RBJ: A (S^2 + sqrt(A)/Q S + A) / (A S^2 + sqrt(A)/Q S + 1), S = s / w
    return [A / w ** 2, A * math.sqrt(A) / q / w, A * A], [A / w ** 2, math.sqrt(A) / q / w, 1.0]


def high_shelf(fc, g_db, q):
    A = 10 ** (g_db / 40); w = 2 * math.pi * fc
    # RBJ: A (A S^2 + sqrt(A)/Q S + 1) / (S^2 + sqrt(A)/Q S + A)
    return [A * A / w ** 2, A * math.sqrt(A) / q / w, A], [1.0 / w ** 2, math.sqrt(A) / q / w, A]


def peak(fc, g_db, q):
    A = 10 ** (g_db / 40); w = 2 * math.pi * fc
    return [1.0 / w ** 2, A / q / w, 1.0], [1.0 / w ** 2, 1.0 / (A * q) / w, 1.0]


def resp(filters, f):
    s = 1j * 2 * np.pi * f
    h = np.ones_like(s)
    for b, a in filters:
        h *= (b[0] * s * s + b[1] * s + b[2]) / (a[0] * s * s + a[1] * s + a[2])
    return h


def to_section(b, a):
    """(b, a) as the stage's General2: the poles' frequency and Q, the numerator in S = s / w_pole."""
    wp = math.sqrt(a[2] / a[0])
    q = math.sqrt(a[2] * a[0]) / a[1]
    return {"kind": "General2", "f": wp / (2 * math.pi), "q": q, "c": [b[0] * wp * wp / a[2], b[1] * wp / a[2], b[2] / a[2]]}


def pole_zero_q(b, a):
    qp = math.sqrt(a[2] * a[0]) / a[1]
    qz = math.sqrt(abs(b[2] * b[0])) / b[1] if b[1] > 0 else float("inf")
    return qp, qz


def build(x, n_peaks):
    g0, lf, lg, hf, hg, lq, hq = x[:7]
    fl = [low_shelf(lf, lg, lq), high_shelf(hf, hg, hq)]
    for k in range(n_peaks):
        pf, pg, qp = x[7 + 3 * k:10 + 3 * k]   # qp: its poles' Q (peak(): pole Q = q A, zero Q = q / A = qp / A^2)
        fl.append(peak(pf, pg, qp / 10 ** (pg / 40)))
    return g0, fl


def zero_q_excess(x, n_peaks):
    """How far each peaking filter's zeros' Q is above ZQMAX (0 within)."""
    return [max(0.0, x[9 + 3 * k] / 10 ** (x[8 + 3 * k] / 20) - ZQMAX) for k in range(n_peaks)]


def fit(f, target_db, n_peaks):
    lo = [-6, 30, -8, 1000, -8, QMIN, QMIN] + [30, -6, QMIN] * n_peaks
    hi = [6, 600, 8, 20000, 8, QMAX, QMAX] + [8000, 6, QMAX] * n_peaks
    best = None
    rng = np.random.default_rng(2)
    for trial in range(40):
        x0 = [0, 120, -1, 6000, -2, 0.6, 0.6]
        for k in range(n_peaks):
            x0 += [float(np.exp(rng.uniform(np.log(40), np.log(5000)))), float(rng.uniform(-3, 3)), 0.6]
        if trial:
            x0[:5] = [rng.uniform(-1, 1), float(np.exp(rng.uniform(np.log(30), np.log(500)))), rng.uniform(-3, 3),
                      float(np.exp(rng.uniform(np.log(2000), np.log(16000)))), rng.uniform(-4, 1)]
        x0 = np.clip(x0, lo, hi)

        def err(x):
            g0, fl = build(x, n_peaks)
            return np.concatenate([20 * np.log10(np.abs(resp(fl, f))) + g0 - target_db, 20 * np.array(zero_q_excess(x, n_peaks))])
        r = optimize.least_squares(err, x0, bounds=(lo, hi), max_nfev=3000)
        if max(zero_q_excess(r.x, n_peaks), default=0.0) > 0.01:
            continue
        res = r.fun[:len(f)]
        rms = float(np.sqrt(np.mean(res ** 2)))
        if best is None or rms < best[1]:
            best = (float(np.max(np.abs(res))), rms, r.x)
    return best


def main():
    path = os.path.join(LF.OUT, "loads.json")
    data = json.load(open(path))
    f = np.logspace(math.log10(20), math.log10(20000), 400)
    fw = np.logspace(math.log10(10), math.log10(40000), 800)   # the smoothing window needs room beyond the band
    for preset, fb in LF.CASES:
        key = preset if fb is None else f"{preset}:{fb}"
        for base in BASES:
            h = LF.target(preset, fb, base, fw)
            sm = np.interp(np.log(f), np.log(fw), smoothed_db(fw, h))
            for n_peaks in (0, 1, 2, 3):
                e, rms, x = fit(f, sm, n_peaks)
                if rms <= 0.3:
                    break
            g0, fl = build(x, n_peaks)
            secs = [to_section(b, a) for b, a in fl]
            secs[0]["c"] = [c * 10 ** (g0 / 20) for c in secs[0]["c"]]
            qs = [pole_zero_q(b, a) for b, a in fl]
            worst = max(np.max(np.abs(20 * np.log10(np.abs(LF.sections_response(secs, f, r))) - sm)) for r in LF.STAGE_RATES)
            raw = 20 * np.log10(np.abs(LF.target(preset, fb, base, f)))
            name = base + "-smooth"
            data[key][name] = {"sections": secs, "smoothing_octaves": OCT, "peaks": n_peaks, "fit_db": float(e), "fit_rms_db": rms,
                               "at_stage_rates_db": float(worst), "pole_q": [float(min(q[0] for q in qs)), float(max(q[0] for q in qs))],
                               "max_zero_q": float(max(q[1] for q in qs)),
                               "vs_unsmoothed_db": float(np.max(np.abs(20 * np.log10(np.abs(LF.sections_response(secs, f, LF.R0))) - raw)))}
            print(f"{key:<16} {name:<24} {len(secs)} sections ({n_peaks} peaking), fit {e:.3f} dB (RMS {rms:.3f}), at the stage's rates {worst:.3f} dB,"
                  f" pole Q {data[key][name]['pole_q'][0]:.2f} .. {data[key][name]['pole_q'][1]:.2f}, zero Q <= {data[key][name]['max_zero_q']:.2f}; against the unsmoothed load {data[key][name]['vs_unsmoothed_db']:.2f} dB", flush=True)
    if len(sys.argv) > 1:
        import soundfile as sf
        import listening_set as LS
        clips = []
        for line in open(sys.argv[1], encoding="utf-8"):
            if not line.strip():
                continue
            label, p = line.rstrip("\n").split("\t")
            info = sf.info(p)
            x, rate = sf.read(p, start=int(info.frames * 0.35), frames=int(40 * info.samplerate), always_2d=True)
            clips.append((x[:, :2], rate))
        for key, loads in data.items():
            for load, e in loads.items():
                if not load.endswith("-smooth"):
                    continue
                ch = []
                for x, rate in clips:
                    n = len(x)
                    fx = np.fft.rfftfreq(n, 1.0 / rate)
                    hh = LF.sections_response(e["sections"], np.maximum(fx, 1e-3), LF.R0)
                    y = np.fft.irfft(np.fft.rfft(x, axis=0) * hh[:, None], n, axis=0)
                    ch.append(LS.lufs(x, rate) - LS.lufs(y, rate))
                e["makeup_db"] = float(np.median(ch))
                e["makeup_spread_db"] = [float(min(ch)), float(max(ch))]
                print(f"{key:<16} {load:<24} makeup {e['makeup_db']:+.2f} dB (the tracks {min(ch):+.2f} .. {max(ch):+.2f})", flush=True)
    json.dump(data, open(path, "w"), indent=1)


if __name__ == "__main__":
    main()
