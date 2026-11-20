"""Loudness of the reference responses (N28): -18 dBFS RMS pink noise through each preset's / feedback step's
frequency response (data/ref/*_response.csv, normalised at 1 kHz), measured with ITU-R BS.1770-4 (K-weighting,
mean square; steady noise, so the gate does not act). The difference to the unfiltered noise is the static makeup
gain each preset / step needs on top of its 1 kHz gain, so that switching keeps loudness within 0.5 dB.

  python loudness.py            prints the makeup per response file and writes data/ref/loudness.json
  python loudness.py --selftest K-weighting against the BS.1770 reference (997 Hz sine at 0 dBFS peak: -3.01 LUFS)
"""
import glob, json, os, sys
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
