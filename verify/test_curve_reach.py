"""(N28) Tube Color M1 test curve at drive 100 % / 200 %: how far music and pink noise reach into the soft limit, and whether
the engine (render-dsp) matches an independent model of the stage (framework check: oversampling, DC blocker, filters,
makeup, volume cancellation)."""
import json
import os
import subprocess
import sys

import numpy as np
import soundfile as sf
from scipy import signal

A2, A3, KNEE, MAX_IN = 0.1, -0.02, 0.8, 2.5
HERE = os.path.dirname(os.path.abspath(__file__))
# working folder (inputs, renders, results) and the test-curve build's CLI; tracks.txt there lists local music files,
# one path per line (not in the repository: someone's own library)
OUT = os.environ.get("TC_REACH_DIR", os.path.join(HERE, "data", "test_curve_reach"))
CLI = os.environ.get("DAPHNE_CLI", os.path.join(os.path.dirname(HERE), "build-tc", "bin", "Release", "daphne.exe"))


def poly(u):
    u = np.clip(u, -MAX_IN, MAX_IN)
    return u + A2 * u * u + A3 * u * u * u


def soft(p):
    a = np.abs(p)
    y = np.where(a <= KNEE, a, KNEE + (1 - KNEE) * np.tanh((a - KNEE) / (1 - KNEE)))
    return np.sign(p) * y


def curve(u):
    return soft(poly(u))


def factor(rate):
    fam = 44100 if rate % 44100 == 0 else 48000
    l = 1
    while rate * l < 8 * fam:
        l += 1
    return l


def bilinear_sections(rate):
    """pre: HP1 5 Hz; post: HP2 20.4 Hz + LP2 26.6 kHz (Q 0.7071), prewarped at their corners (TPT = bilinear)."""
    pre = signal.butter(1, 5.0, 'highpass', fs=rate, output='sos')
    post = np.vstack([signal.butter(2, 20.4, 'highpass', fs=rate, output='sos'),
                      signal.butter(2, 26600.0, 'lowpass', fs=rate, output='sos')])
    return pre, post


def dc_block(g, rate):
    r = np.exp(-2 * np.pi * 2.0 / rate)
    return signal.lfilter([1, -1], [1, -r], g, axis=0)


def model(x, rate, d):
    """The stage as specified: y = post(x~ + DCB(f(d x~)/d - x~)), x~ = pre(x), all at L x rate."""
    l = factor(rate)
    hi = rate * l
    pre, post = bilinear_sections(hi)
    xo = signal.resample_poly(x, l, 1, axis=0, window=('kaiser', 12.0)) if l > 1 else x.copy()
    xt = signal.sosfilt(pre, xo, axis=0)
    g = curve(d * xt) / d - xt
    s = xt + dc_block(g, hi)
    yo = signal.sosfilt(post, s, axis=0)
    return signal.resample_poly(yo, 1, l, axis=0, window=('kaiser', 12.0)) if l > 1 else yo


def reach(x, rate, d, chunk_s=20):
    """How far d * x~ (oversampled, inter-sample peaks included) gets into the curve's limit."""
    l = factor(rate)
    hi = rate * l
    pre, _ = bilinear_sections(hi)
    zi = None
    n = 0
    knee = clamp = 0
    win = int(0.05 * hi)
    wins = wins_hit = 0
    e_sig = e_poly = e_lim = 0.0
    peak = 0.0
    step = int(chunk_s * rate)
    for a in range(0, len(x), step):
        xo = signal.resample_poly(x[a:a + step], l, 1, axis=0, window=('kaiser', 12.0)) if l > 1 else x[a:a + step]
        if zi is None:
            zi = np.zeros((pre.shape[0], 2, x.shape[1]))
        xt, zi = signal.sosfilt(pre, xo, axis=0, zi=zi)
        u = d * xt
        p = poly(u)
        y = soft(p)
        peak = max(peak, float(np.max(np.abs(u))))
        hit = np.abs(p) > KNEE
        knee += int(hit.sum())
        clamp += int((np.abs(u) > MAX_IN).sum())
        n += u.size
        m = (len(u) // win) * win
        h = hit[:m].reshape(-1, win, u.shape[1]).any(axis=(1, 2))
        wins += len(h)
        wins_hit += int(h.sum())
        e_sig += float(np.sum(u * u))
        e_poly += float(np.sum((p - u) ** 2))       # the polynomial's own distortion (what the curve should add)
        e_lim += float(np.sum((y - p) ** 2))        # what the soft limit adds on top (the clipping-like part)
    db = lambda e: 10 * np.log10(e / e_sig) if e > 0 else -np.inf
    return {"peak_in": peak, "knee_frac": knee / n, "clamp_frac": clamp / n, "windows_50ms_hit": wins_hit / max(1, wins),
            "poly_dist_db": db(e_poly), "limit_dist_db": db(e_lim)}


def pink(seconds, rate, rms_db, seed=1):
    rng = np.random.default_rng(seed)
    n = int(seconds * rate)
    out = np.zeros((n, 2))
    for c in range(2):
        w = np.fft.rfft(rng.standard_normal(n))
        f = np.fft.rfftfreq(n, 1 / rate)
        w[1:] /= np.sqrt(f[1:])
        w[0] = 0
        w[f < 10] = 0
        out[:, c] = np.fft.irfft(w, n)
    out *= 10 ** (rms_db / 20) / np.sqrt(np.mean(out ** 2))
    return out


def lufs_like(x):
    return 20 * np.log10(np.sqrt(np.mean(x ** 2)) + 1e-30)


def render(x, rate, settings, tag):
    src = os.path.join(OUT, tag + "_in.wav")
    dst = os.path.join(OUT, tag + "_out.wav")
    st = os.path.join(OUT, tag + ".json")
    sf.write(src, x.astype(np.float32), rate, subtype='FLOAT')
    json.dump(settings, open(st, "w"))
    r = subprocess.run([CLI, "render-dsp", src, dst, "--settings", st, "--float"], capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(r.stderr)
    y, _ = sf.read(dst, always_2d=True)
    return y


def align(y, ref):
    n = min(len(y), len(ref), 1 << 18)
    c = signal.correlate(y[:n, 0], ref[:n, 0], mode='full', method='fft')
    lag = int(np.argmax(c)) - (n - 1)
    if lag >= 0:
        y = y[lag:]
    else:
        ref = ref[-lag:]
    m = min(len(y), len(ref))
    return y[:m], ref[:m], lag


def settings(drive, vol_db=None):
    s = {"core": "pcm", "output": {"rate": "native"}, "color": {"enabled": True, "drive": drive}}
    if vol_db is not None:
        s["volume"] = {"mode": "digital", "db": vol_db}
    return s


def main():
    os.makedirs(OUT, exist_ok=True)
    tracks = [l.strip() for l in open(os.path.join(OUT, "tracks.txt"), encoding="utf-8") if l.strip()]
    rows = []
    if len(sys.argv) > 1 and sys.argv[1] == "checks":
        return checks_only(tracks)
    for name, rms in (("pink -18 dBFS RMS", -18.0), ("pink -12 dBFS RMS", -12.0)):
        x = pink(30, 44100, rms)
        for d in (1.0, 2.0):
            rows.append({"signal": name, "drive": d, "rms_dbfs": rms, **reach(x, 44100, d)})
    for path in tracks:
        x, rate = sf.read(path, always_2d=True)
        x = x[:, :2]
        name = os.path.basename(os.path.dirname(path))[-28:] + " / " + os.path.basename(path)[:34]
        for d in (1.0, 2.0):
            rows.append({"signal": name, "rate": rate, "drive": d, "rms_dbfs": lufs_like(x), **reach(x, rate, d)})
        print("measured", name, file=sys.stderr)
    json.dump(rows, open(os.path.join(OUT, "reach.json"), "w"), indent=1, ensure_ascii=False)
    checks_only(tracks)


def checks_only(tracks):
    # framework check: the engine against the model, on pink noise and on a stretch of loud music
    checks = []
    loud = tracks[2]
    xm, rm = sf.read(loud, always_2d=True)
    xm = xm[:, :2]
    a = len(xm) // 3
    xm = xm[a:a + 20 * rm]
    for tag, x, rate in (("pink12", pink(12, 44100, -12.0, seed=7), 44100), ("music", xm, rm)):
        for d in (1.0, 2.0):
            y = render(x, rate, settings(d), f"{tag}_d{int(d * 100)}")
            ref = model(x, rate, d)
            ya, ra, lag = align(y, ref)
            sl = slice(int(0.5 * rate), len(ya) - int(0.5 * rate))   # (the filters' start)
            lin = model(x, rate, 1e-6)                               # the same filters, the curve's linear part only
            la, ra2, _ = align(lin, ref)
            m = min(len(ya), len(la))
            err = ya[sl][:m] - ra[sl][:m]
            dist = ra[sl][:m] - la[sl][:m]
            rmsdb = lambda v: 20 * np.log10(np.sqrt(np.mean(v ** 2)) + 1e-30)
            checks.append({"signal": tag, "drive": d, "lag_frames": lag, "out_rms_db": rmsdb(ya[sl]),
                           "distortion_rms_db": rmsdb(dist), "engine_minus_model_db": rmsdb(err),
                           "null_vs_distortion_db": rmsdb(err) - rmsdb(dist), "out_peak": float(np.max(np.abs(ya)))})
        # the volume cancellation: -20 dB digital volume gives the same output, 20 dB lower (the character unchanged)
        y0 = render(x, rate, settings(2.0), f"{tag}_v0")
        y20 = render(x, rate, settings(2.0, -20.0), f"{tag}_v20")
        m = min(len(y0), len(y20))
        e = y20[:m] * 10 - y0[:m]
        checks.append({"signal": tag, "check": "volume -20 dB x 10 vs 0 dB, drive 200 %",
                       "max_abs_err": float(np.max(np.abs(e[int(0.5 * rate):]))), "out_peak": float(np.max(np.abs(y0)))})
    json.dump(checks, open(os.path.join(OUT, "checks.json"), "w"), indent=1, ensure_ascii=False)
    print(json.dumps(checks, indent=1, ensure_ascii=False))


if __name__ == "__main__":
    main()
