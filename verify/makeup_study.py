"""M2 (N28): the static makeup table, two ways, and how well each aligns the loudness on music (A/B against bypass).
  pink    the curve's loudness loss on -18 dBFS RMS pink noise (20 Hz - 20 kHz, no DC)
  median  the median of the curve's loss on the six test tracks
Both from the loudness before the limiter (the curve's loss only). The output then goes through the limiter (b2:
ceiling -0.3 dBFS, knee from -0.5 dBFS, limiter_study.limiter) with the table's makeup applied before it, as in the
engine; the error is the result's loudness against the original (bypass plays the original).

  python makeup_study.py <tracks.txt> [preset] [seconds]
"""
import json, math, os, sys, tempfile

import numpy as np
import soundfile as sf

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import limiter_study as S   # noqa: E402
import loudness as LU       # noqa: E402

DRIVES = (0.5, 1.0, 1.5, 2.0, 3.0, 4.0)
CEIL, KNEE = 10 ** (-0.3 / 20), 0.4


def main():
    tracks = sys.argv[1]
    preset = sys.argv[2] if len(sys.argv) > 2 else "set-300b"
    secs = float(sys.argv[3]) if len(sys.argv) > 3 else 40.0
    vol = -20 * math.log10(4.0)
    with tempfile.TemporaryDirectory() as td:
        rng = np.random.default_rng(1)
        n = LU.FS * 30
        sp = np.fft.rfft(rng.standard_normal((n, 2)), axis=0)
        fr = np.fft.rfftfreq(n, 1 / LU.FS)
        sp[(fr < 20) | (fr > 20000)] = 0
        sp[fr >= 20] /= np.sqrt(fr[fr >= 20])[:, None]
        pk = np.fft.irfft(sp, n, axis=0)
        pk *= 10 ** (-18 / 20) / np.sqrt(np.mean(pk * pk))
        pink = {}
        for d in DRIVES:
            yf, _ = S.render(pk, LU.FS, preset, d, vol, td, f"pink_{d}")
            pink[d] = S.lufs(pk, LU.FS) - S.lufs(yf * 4.0, LU.FS)
        # pass 1: every track's limiter-free output (kept on disk) and its curve loss
        items = []
        for line in open(tracks, encoding="utf-8"):
            if not line.strip():
                continue
            label, path = line.rstrip("\n").split("\t")
            info = sf.info(path)
            x, rate = sf.read(path, start=int(info.frames * 0.35), frames=int(secs * info.samplerate), always_2d=True)
            x = x[:, :2]
            l0 = S.lufs(x, rate)
            loss = {}
            for d in DRIVES:
                yf, _ = S.render(x, rate, preset, d, vol, td, f"{label}_{d}")
                yf *= 4.0
                np.save(os.path.join(td, f"{label}_{d}.npy"), yf)
                loss[d] = l0 - S.lufs(yf, rate)
            items.append((label, rate, l0, loss))
            print("measured", label, flush=True)
        median = {d: float(np.median([it[3][d] for it in items])) for d in DRIVES}
        # pass 2: each table through the limiter
        res = {"pink_table_db": pink, "median_table_db": median, "tracks": {}}
        for label, rate, l0, loss in items:
            r = {}
            for d in DRIVES:
                yf = np.load(os.path.join(td, f"{label}_{d}.npy"))
                e = {"curve_loss_db": loss[d]}
                for name, table in (("pink", pink), ("median", median)):
                    yy, lim = S.limiter(yf * 10 ** (table[d] / 20), rate, CEIL, KNEE)
                    e[name] = S.lufs(yy, rate) - l0
                r[str(d)] = e
            res["tracks"][label] = r
    out = os.path.join(os.path.dirname(tracks), f"{preset}_makeup_study.json")
    json.dump(res, open(out, "w"), indent=1, default=float)
    print(f"{'drive':>6} {'pink table':>11} {'median':>8}   worst A/B error over the six (pink / median), and which track")
    for d in DRIVES:
        wp = max(res["tracks"].items(), key=lambda kv: abs(kv[1][str(d)]["pink"]))
        wm = max(res["tracks"].items(), key=lambda kv: abs(kv[1][str(d)]["median"]))
        print(f"{d * 100:5.0f}% {pink[d]:+10.2f} {median[d]:+8.2f}   {wp[1][str(d)]['pink']:+6.2f} ({wp[0]})   {wm[1][str(d)]['median']:+6.2f} ({wm[0]})")


if __name__ == "__main__":
    main()
