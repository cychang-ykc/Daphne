"""Loudness of the reference responses (N28): -18 dBFS RMS pink noise through each preset's / feedback step's
frequency response (data/ref/*_response.csv, normalised at 1 kHz), measured with ITU-R BS.1770-4 (K-weighting,
mean square; steady noise, so the gate does not act). The difference to the unfiltered noise is the static makeup
gain each preset / step needs on top of its 1 kHz gain, so that switching keeps loudness within 0.5 dB.

  python loudness.py            prints the makeup per response file and writes data/ref/loudness.json
  python loudness.py --selftest K-weighting against the BS.1770 reference (997 Hz sine at 0 dBFS peak: -3.01 LUFS), the
                                K-weighting at any rate against the 48 kHz coefficients, and integrated() on the synthetic
                                test signals of EBU Tech 3341 / ITU-R BS.2217 (cases 1 - 5), at 48 and 44.1 kHz

lufs(): K-weighted mean square without gating (steady signals; versions of one excerpt). integrated(): BS.1770-4's
integrated loudness, for comparing tracks: 400 ms blocks with 75 % overlap, the absolute gate (-70 LUFS) and the
relative gate (-10 LU), the channels (L, R: weight 1) summed; the K-weighting designed for the signal's own rate.
"""
import glob, json, math, os, sys
import numpy as np
from scipy.signal import lfilter

FS = 48000
# BS.1770-4, 48 kHz: pre-filter (high shelf) and RLB high pass
B1, A1 = [1.53512485958697, -2.69169618940638, 1.19839281085285], [1.0, -1.69065929318241, 0.73248077421585]
B2, A2 = [1.0, -2.0, 1.0], [1.0, -1.99004745483398, 0.99007225036621]
HERE = os.path.dirname(os.path.abspath(__file__))


def lufs(x):
    y = lfilter(B2, A2, lfilter(B1, A1, x))
    return -0.691 + 10 * np.log10(np.mean(y * y))


def k_weighting(rate):
    """BS.1770's two stages at any rate, from their analog prototypes (the published 48 kHz coefficients exactly)."""
    def shelf(fc, g, q):
        k = math.tan(math.pi * fc / rate); vh = 10 ** (g / 20); vb = vh ** 0.4996667741545416
        a0 = 1 + k / q + k * k
        return [(vh + vb * k / q + k * k) / a0, 2 * (k * k - vh) / a0, (vh - vb * k / q + k * k) / a0],                [1.0, 2 * (k * k - 1) / a0, (1 - k / q + k * k) / a0]
    def hp(fc, q):
        k = math.tan(math.pi * fc / rate); a0 = 1 + k / q + k * k
        return [1.0, -2.0, 1.0], [1.0, 2 * (k * k - 1) / a0, (1 - k / q + k * k) / a0]
    return shelf(1681.974450955533, 3.999843853973347, 0.7071752369554196), hp(38.13547087602444, 0.5003270373238773)


def integrated(x, rate):
    """BS.1770-4 integrated loudness (LUFS) of x[n] or x[n, ch] (front channels, weight 1)."""
    x = np.asarray(x, dtype=float)
    if x.ndim == 1:
        x = x[:, None]
    (b1, a1), (b2, a2) = k_weighting(rate)
    y = lfilter(b2, a2, lfilter(b1, a1, x, axis=0), axis=0)
    step = int(round(0.1 * rate))
    n_blocks = (len(y) - 4 * step) // step + 1   # 400 ms blocks every 100 ms
    if n_blocks < 1:
        return float("-inf")
    c = np.concatenate([np.zeros((1, y.shape[1])), np.cumsum(y * y, axis=0)])
    starts = np.arange(n_blocks) * step
    z = (c[starts + 4 * step] - c[starts]) / (4 * step)            # [block, ch]
    zs = z.sum(axis=1)
    with np.errstate(divide="ignore"):
        lj = -0.691 + 10 * np.log10(zs)
    keep = lj > -70.0
    if not keep.any():
        return float("-inf")
    rel = -0.691 + 10 * np.log10(zs[keep].mean()) - 10.0
    keep &= lj > rel
    return float(-0.691 + 10 * np.log10(zs[keep].mean()))


def pink(seconds=30, seed=1):
    n = FS * seconds
    spec = np.fft.rfft(np.random.default_rng(seed).standard_normal(n))
    f = np.fft.rfftfreq(n, 1 / FS); f[0] = f[1]
    x = np.fft.irfft(spec / np.sqrt(f), n)
    return x / np.sqrt(np.mean(x * x)) * 10 ** (-18 / 20)       # -18 dBFS RMS (full scale = 1.0 peak sine RMS 0.707)


def through(x, f, db, ph):
    n = len(x); fx = np.fft.rfftfreq(n, 1 / FS)
    mag = 10 ** (np.interp(fx, f, db, left=db[0], right=db[-1]) / 20)
    phase = np.radians(np.interp(fx, f, ph, left=ph[0], right=ph[-1]))
    return np.fft.irfft(np.fft.rfft(x) * mag * np.exp(1j * phase), n)


if __name__ == "__main__":
    if "--selftest" in sys.argv:
        t = np.arange(FS * 10) / FS
        print(f"997 Hz sine, 0 dBFS peak: {lufs(np.sin(2 * np.pi * 997 * t)):.3f} LUFS (BS.1770-4 reference: -3.01)")
        (b1, a1), (b2, a2) = k_weighting(48000)
        err = max(np.max(np.abs(np.array(b1) - B1)), np.max(np.abs(np.array(a1) - A1)), np.max(np.abs(np.array(b2) - B2)),
                  np.max(np.abs(np.array(a2) - A2)))
        print(f"K-weighting designed at 48 kHz against BS.1770-4's coefficients: max difference {err:.1e}")
        # EBU Tech 3341 (also ITU-R BS.2217's synthetic signals): stereo 1 kHz sine, segments (dBFS, seconds); each -> its LUFS
        cases = {1: ([(-23, 20)], -23.0), 2: ([(-33, 20)], -33.0), 3: ([(-36, 10), (-23, 60), (-36, 10)], -23.0),
                 4: ([(-72, 10), (-36, 10), (-23, 60), (-36, 10), (-72, 10)], -23.0), 5: ([(-26, 20), (-20, 20.1), (-26, 20)], -23.0)}
        worst = 0.0
        for rate in (48000, 44100):
            for k, (segs, want) in cases.items():
                parts, n0 = [], 0
                for db, s in segs:
                    n = int(round(s * rate))
                    parts.append(10 ** (db / 20) * np.sin(2 * np.pi * 1000 * (n0 + np.arange(n)) / rate)); n0 += n
                m = np.concatenate(parts)
                got = integrated(np.stack([m, m], axis=1), rate)
                worst = max(worst, abs(got - want))
                print(f"  {rate} Hz, case {k}: {got:7.3f} LUFS (expected {want:.1f}, tolerance +-0.1)")
        print(f"worst error {worst:.3f} LU: {'PASS' if worst <= 0.1 else 'FAIL'}")
        sys.exit()
    x = pink(); ref = lufs(x)
    out = {}
    print(f"pink noise -18 dBFS RMS: {ref:.2f} LUFS")
    for fn in sorted(glob.glob(os.path.join(HERE, "data", "ref", "*_response.csv"))):
        a = np.loadtxt(fn, delimiter=",", skiprows=1)
        d = lufs(through(x, a[:, 0], a[:, 1], a[:, 2])) - ref
        name = os.path.basename(fn)[:-len("_response.csv")]
        out[name] = {"loudness_change_dB": float(d), "makeup_dB": float(-d)}
        print(f"  {name:<20} loudness {d:+.2f} dB -> makeup {-d:+.2f} dB")
    json.dump(out, open(os.path.join(HERE, "data", "ref", "loudness.json"), "w"), indent=1)
