"""M2 (N28): Tube Color's soft limiter on music, and three ways to set it up with the automatic makeup gain.

For each track (local files, tracks.txt as for listening_set.py) and drive (50 / 100 / 200 / 400 %), from the engine
(render-dsp):
  y_eng   the output as the engine has it now (the limiter after the stage, no automatic makeup yet)
  y_free  the same output without the limiter: rendered with the digital volume at -12.04 dB (the stage cancels the
          volume, so its curve sees the same signal; its output 12 dB lower, where the limiter rests) times 4
The limiter is then re-run here (dsp::Limiter ported line by line, checked against y_eng) on y_free times the makeup
gain, as the engine will run it once the makeup exists: the makeup is applied in the stage, before the limiter.

  (a) ceiling -0.3 dBFS, knee 4 dB (from -2.3 dBFS): now
  (b) ceiling -0.3 dBFS, knee 1.4 dB (from -1.0 dBFS)
  (b2) ceiling -0.3 dBFS, knee 0.4 dB (from -0.5 dBFS)
  (c) as (a), the makeup 3 dB short (headroom kept in the makeup)
The makeup table: the curve's loudness loss on -18 dBFS RMS pink noise (the spec's measure), per drive. The pink noise
is band-limited to 20 Hz - 20 kHz with no DC: loudness.py's pink() carries a DC and sub-5 Hz component that dominates its
RMS (its DC bin takes the first bin's level), which the stage's high-pass removes (2.2 dB of "loss" at 50 % drive).
The ported limiter adds its own look-ahead on top of the engine's (resting) one in y_free: compared one window later.
Loudness: BS.1770's K-weighting over the excerpt (loudness.py; no gating).

  python limiter_study.py <tracks.txt> [preset] [seconds]
"""
import json, math, os, re, subprocess, sys, tempfile

import numba
import numpy as np
import soundfile as sf

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import listening_set as LS  # noqa: E402
import loudness as LU        # noqa: E402

DRIVES = (0.5, 1.0, 2.0, 4.0)
CEIL = 10 ** (-0.3 / 20)
SCHEMES = {"a": (CEIL, 4.0, 0.0), "b": (CEIL, 1.4, 0.0), "b2": (CEIL, 0.4, 0.0), "c": (CEIL, 4.0, 3.0)}   # ceiling, knee dB, makeup short dB


@numba.njit(cache=True)
def limiter(x, rate, ceiling, knee_db, lookahead_ms=2.0, release_ms=80.0):
    """dsp::Limiter (dsp/src/limiter.cpp) on interleaved frames x[n, ch]: returns (y, limited frames)."""
    n, ch = x.shape
    w = max(1, int(round(lookahead_ms * 1e-3 * rate)))
    rel = 1.0 - math.exp(-1.0 / (release_ms * 1e-3 * rate))
    t = 20.0 * math.log10(ceiling)
    delay = np.zeros((w, ch))
    avg = np.ones(w)
    cap = w + 2
    qv = np.ones(cap)
    qi = np.zeros(cap, dtype=np.int64)
    qh = 0
    qs = 0
    pos = 0
    s = float(w)
    env = 1.0
    lim = 0
    y = np.empty_like(x)
    for i in range(n):
        peak = 0.0
        for c in range(ch):
            a = abs(x[i, c])
            if a > peak:
                peak = a
        req = 1.0
        if knee_db <= 0.0:
            if peak > ceiling:
                req = ceiling / peak
        elif peak > 0.0:
            xd = 20.0 * math.log10(peak)
            if xd >= t + 0.5 * knee_db:
                req = ceiling / peak
            elif xd > t - 0.5 * knee_db:
                d = xd - t + 0.5 * knee_db
                req = min(1.0, 10.0 ** (-d * d / (2.0 * knee_db) / 20.0))
        while qs > 0 and qv[(qh + qs - 1) % cap] >= req:
            qs -= 1
        tl = (qh + qs) % cap
        qv[tl] = req
        qi[tl] = i
        qs += 1
        while qi[qh] < i - w:
            qh = (qh + 1) % cap
            qs -= 1
        hold = qv[qh]
        env = min(hold, env + (1.0 - env) * rel)
        if env > 1.0 - 1e-9 and hold == 1.0:
            env = 1.0
        s += env - avg[pos]
        avg[pos] = env
        g = min(1.0, s / w)
        if g < 1.0:
            lim += 1
        for c in range(ch):
            dl = delay[pos, c]
            delay[pos, c] = x[i, c]
            y[i, c] = dl * g if g < 1.0 else dl
        pos += 1
        if pos == w:
            pos = 0
            s = 0.0
            for k in range(w):
                s += avg[k]
    return y, lim


def render(x, rate, preset, drive, vol_db, td, tag):
    src, dst, st = (os.path.join(td, tag + e) for e in ("_in.wav", "_out.wav", ".json"))
    sf.write(src, x.astype(np.float32), rate, subtype="FLOAT")
    s = {"core": "pcm", "output": {"rate": "native"}, "color": {"enabled": drive > 0, "preset": preset, "drive": drive}}
    if vol_db:
        s["volume"] = {"mode": "digital", "db": vol_db}
    json.dump(s, open(st, "w"))
    r = subprocess.run([LS.CLI, "render-dsp", src, dst, "--settings", st, "--float"], capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(r.stderr)
    m = re.search(r"color_limited (\d+)", r.stdout)
    y, _ = sf.read(dst, always_2d=True)
    return y.astype(np.float64), int(m.group(1)) if m else 0


def lufs(x, rate):
    return LS.lufs(x, rate)


def rms_db(v):
    return 10 * math.log10(float(np.mean(v * v)) + 1e-300)


def main():
    tracks = sys.argv[1]
    preset = sys.argv[2] if len(sys.argv) > 2 else "set-300b"
    secs = float(sys.argv[3]) if len(sys.argv) > 3 else 40.0
    vol = -20 * math.log10(4.0)
    out = {"makeup_db": {}, "tracks": {}, "check": {}}
    with tempfile.TemporaryDirectory() as td:
        # the makeup table: the curve's loss on -18 dBFS RMS pink noise (44.1 kHz)
        rng = np.random.default_rng(1)
        n_pk = LU.FS * 30
        spec = np.fft.rfft(rng.standard_normal((n_pk, 2)), axis=0)
        fr = np.fft.rfftfreq(n_pk, 1 / LU.FS)
        spec[(fr < 20) | (fr > 20000)] = 0
        spec[fr >= 20] /= np.sqrt(fr[fr >= 20])[:, None]
        pk = np.fft.irfft(spec, n_pk, axis=0)
        pk *= 10 ** (-18 / 20) / np.sqrt(np.mean(pk * pk))
        for d in DRIVES:
            yf, _ = render(pk, LU.FS, preset, d, vol, td, f"pink_{d}")
            yf *= 4.0
            out["makeup_db"][str(d)] = lufs(pk, LU.FS) - lufs(yf, LU.FS)
        print("makeup (pink noise -18 dBFS RMS):", {k: round(v, 2) for k, v in out["makeup_db"].items()}, flush=True)
        worst_check = 0.0
        for line in open(tracks, encoding="utf-8"):
            if not line.strip():
                continue
            label, path = line.rstrip("\n").split("\t")
            info = sf.info(path)
            x, rate = sf.read(path, start=int(info.frames * 0.35), frames=int(secs * info.samplerate), always_2d=True)
            x = x[:, :2]
            l0 = lufs(x, rate)
            rows = {}
            for d in DRIVES:
                ye, lim_e = render(x, rate, preset, d, 0.0, td, f"{label}_{d}")
                yf, lim_f = render(x, rate, preset, d, vol, td, f"{label}_{d}_free")
                yf *= 4.0
                n = min(len(ye), len(yf))
                ye, yf = ye[:n], yf[:n]
                # the port against the engine (float32 files: ~1e-7); its output one look-ahead later
                w = max(1, int(round(2.0e-3 * rate)))
                yp, lim_p = limiter(yf, rate, CEIL, 4.0)
                chk = float(np.max(np.abs(yp[w:] - ye[:-w])))
                worst_check = max(worst_check, chk)
                r = {"engine_limited_pct": 100.0 * lim_e / n, "port_limited_pct": 100.0 * lim_p / n, "free_rested": lim_f == 0,
                     "engine_limiter_dist_db": rms_db(ye - yf) - rms_db(yf),
                     "loss_total_db": l0 - lufs(ye, rate), "loss_curve_db": l0 - lufs(yf, rate),
                     "peak_free": float(np.max(np.abs(yf)))}
                r["loss_limiter_db"] = r["loss_total_db"] - r["loss_curve_db"]
                g_table = out["makeup_db"][str(d)]
                for name, (ceil, knee, short) in SCHEMES.items():
                    g = 10 ** ((g_table - short) / 20)
                    yy, lim = limiter(yf * g, rate, ceil, knee)
                    r[name] = {"limited_pct": 100.0 * lim / n, "dist_db": rms_db(yy[w:] - yf[:-w] * g) - rms_db(yf * g),
                               "ab_db": lufs(yy, rate) - l0}   # against bypass (drive 0: the original)
                rows[str(d)] = r
                print(f"{label:<14} {d * 100:3.0f} %  engine now: limited {r['engine_limited_pct']:6.2f} %  dist {r['engine_limiter_dist_db']:6.1f} dB"
                      f"  loss {r['loss_total_db']:+5.2f} (curve {r['loss_curve_db']:+5.2f}, limiter {r['loss_limiter_db']:+5.2f})"
                      + "".join(f" | {k}: {r[k]['limited_pct']:5.1f} % {r[k]['dist_db']:6.1f} dB A/B {r[k]['ab_db']:+5.2f}" for k in SCHEMES),
                      flush=True)
            out["tracks"][label] = rows
        out["check"]["port_vs_engine_max_abs"] = worst_check
        print(f"port against the engine: max |difference| {worst_check:.2e}")
    json.dump(out, open(os.path.join(os.path.dirname(tracks), "limiter_study.json"), "w"), indent=1)


if __name__ == "__main__":
    main()
