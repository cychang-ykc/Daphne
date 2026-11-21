"""M2 (N28): does the distortion follow the track's loudness? (the h2-pure / set-300b listening: loud pop breaks up at a
drive where classical music does not; the stage cancels the volume, so its curve sees the master's own level).

For each track (tracks.txt as for listening_set.py: local files, not in the repository):
  - BS.1770-4 integrated loudness (loudness.integrated: gated) of the whole track and of the excerpt (40 s from 35 %)
  - per preset and drive, from the engine (render-dsp): the stage alone (the digital volume at -12.04 dB: the stage
    cancels it, so its curve sees the same signal, and the limiter after it rests; the output times 4)
      y       the excerpt at drive D
      y_lin   the excerpt times 1e-4 at the same drive, divided back: the stage's linear part (its curve at u -> 0)
      residual  y - a y_lin, a the least-squares gain (the level compression is not counted as distortion), over the
              excerpt without its first and last second; reported as its energy against a y_lin's, dB
  - three ways to set the drive:
      now      drive D
      track    "loudness-based": the curve's input scaled to the target loudness T and back, i.e. drive D 10^((T - L)/20)
               with L the whole track's integrated loudness (what a tag or a scan gives)
      excerpt  the same with L the excerpt's (the ideal: the loudness of what is playing)
    T = the loudness of the drive's calibration signal: -18 dBFS RMS pink noise (20 Hz - 20 kHz) in both channels.
  - set-300b: the share of (4x oversampled) samples of the curve's input u = D x (after the stage's 3.3 Hz high-pass)
    beyond the rated drive (|u| > 0.86) and the grid-current point (|u| > 1.03), each side; and beyond the table (|u| > 6)

Drives above 400 % (kColorDriveMax) go through the volume route of listening_set.py (input k x, volume 1/k, drive D/k).

  python drive_loudness.py <tracks.txt> <out dir> [seconds]     writes <out>/drive_loudness.json, the plot, the table
"""
import concurrent.futures as cf, json, math, os, subprocess, sys, tempfile

import numpy as np
import soundfile as sf
from scipy import signal

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import loudness as LU   # noqa: E402

CLI = os.environ.get("DAPHNE_CLI", os.path.join(os.path.dirname(HERE), "build", "bin", "Release", "daphne.exe"))
DRIVE_MAX = 4.0
DRIVES = (0.5, 1.0, 1.5, 2.0, 3.0, 4.0)
PRESETS = ("h2-pure", "set-300b")
EPS = 1e-4
VOL_STAGE = -20 * math.log10(4.0)   # the stage's output 12 dB down: the limiter rests
U_RATED, U_GRID, U_TABLE = 0.8605715983500632, 1.0326859180200758, 6.0


def calibration_loudness():
    """Integrated loudness of -18 dBFS RMS pink noise (20 Hz - 20 kHz, no DC) in both channels: the drive's reference."""
    rng = np.random.default_rng(1)
    n = LU.FS * 30
    sp = np.fft.rfft(rng.standard_normal((n, 2)), axis=0)
    fr = np.fft.rfftfreq(n, 1 / LU.FS)
    sp[(fr < 20) | (fr > 20000)] = 0
    sp[fr >= 20] /= np.sqrt(fr[fr >= 20])[:, None]
    pk = np.fft.irfft(sp, n, axis=0)
    pk *= 10 ** (-18 / 20) / np.sqrt(np.mean(pk * pk, axis=0))
    return LU.integrated(pk, LU.FS)


def render(src, rate, preset, drive, td, tag, scale=1.0, vol_db=VOL_STAGE):
    """The excerpt (a float wav on disk) through the engine at `drive` (the volume route above DRIVE_MAX)."""
    x, _ = sf.read(src, always_2d=True)
    k = max(1.0, drive / DRIVE_MAX)
    inp = os.path.join(td, tag + "_in.wav")
    dst = os.path.join(td, tag + "_out.wav")
    st = os.path.join(td, tag + ".json")
    sf.write(inp, (x * scale * k).astype(np.float32), rate, subtype="FLOAT")
    json.dump({"core": "pcm", "output": {"rate": "native"},
               "color": {"enabled": True, "preset": preset, "drive": drive / k, "load": "bookshelf-vented"},
               "volume": {"mode": "digital", "db": vol_db - 20 * math.log10(k)}}, open(st, "w"))
    r = subprocess.run([CLI, "render-dsp", inp, dst, "--settings", st, "--float"], capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(r.stderr)
    y, _ = sf.read(dst, always_2d=True)
    for f in (inp, dst, st):
        os.remove(f)
    return y * 10 ** (-vol_db / 20) / scale


def residual_db(y, yl, rate):
    a0, a1 = rate, len(y) - rate
    y, yl = y[a0:a1], yl[a0:a1]
    a = float(np.sum(y * yl) / np.sum(yl * yl))
    r = y - a * yl
    return 10 * math.log10(np.sum(r * r) / np.sum((a * yl) ** 2)), a


def u_stats(x, rate, drive):
    b, a = signal.butter(1, 3.288325270493705, "highpass", fs=rate)
    xo = signal.resample_poly(signal.lfilter(b, a, x, axis=0), 4, 1, axis=0)
    u = drive * xo
    n = u.size
    return {"above_rated": float(np.sum(u > U_RATED) / n), "below_rated": float(np.sum(u < -U_RATED) / n),
            "above_grid": float(np.sum(u > U_GRID) / n), "below_grid": float(np.sum(u < -U_GRID) / n),
            "beyond_table": float(np.sum(np.abs(u) > U_TABLE) / n), "u_peak": float(np.max(np.abs(u)))}


def main():
    tracks, out = sys.argv[1], sys.argv[2]
    secs = float(sys.argv[3]) if len(sys.argv) > 3 else 40.0
    os.makedirs(out, exist_ok=True)
    T = calibration_loudness()
    print(f"target T (the calibration pink noise, both channels): {T:.2f} LUFS", flush=True)
    res = {"target_lufs": T, "drives": DRIVES, "tracks": {}}
    with tempfile.TemporaryDirectory() as td, cf.ThreadPoolExecutor(max(2, (os.cpu_count() or 4) // 2)) as pool:
        for line in open(tracks, encoding="utf-8"):
            if not line.strip():
                continue
            label, path = line.rstrip("\n").split("\t")
            whole, rate = sf.read(path, always_2d=True)
            whole = whole[:, :2]
            l_track = LU.integrated(whole, rate)
            s0 = int(len(whole) * 0.35)
            x = whole[s0:s0 + int(secs * rate)].copy()
            del whole
            l_ex = LU.integrated(x, rate)
            src = os.path.join(td, label + "_src.wav")
            sf.write(src, x.astype(np.float32), rate, subtype="FLOAT")
            print(f"{label}: track {l_track:.2f} LUFS, excerpt {l_ex:.2f} LUFS, peak {20 * math.log10(np.max(np.abs(x))):.2f} dBFS",
                  flush=True)
            g = {"now": 1.0, "track": 10 ** ((T - l_track) / 20), "excerpt": 10 ** ((T - l_ex) / 20)}
            jobs = {}
            for preset in PRESETS:
                for mode, gm in g.items():
                    for d in DRIVES:
                        de = d * gm
                        jobs[(preset, mode, d)] = pool.submit(render, src, rate, preset, de, td, f"{label}_{preset}_{mode}_{d}")
                        # the linear part depends on the drive only below 25 % (the response's fade): one per drive below
                        key = ("lin", round(de, 6) if de < 0.25 else "full")
                        if key not in jobs:
                            jobs[key] = pool.submit(render, src, rate, "set-300b", de if de < 0.25 else 1.0, td,
                                                    f"{label}_lin_{key[1]}", EPS)
            r = {"lufs_track": l_track, "lufs_excerpt": l_ex, "peak_dbfs": float(20 * math.log10(np.max(np.abs(x)))),
                 "gain_db": {m: 20 * math.log10(v) for m, v in g.items()}, "presets": {}, "u": {}}
            for preset in PRESETS:
                rp = {}
                for mode, gm in g.items():
                    rm = {}
                    for d in DRIVES:
                        de = d * gm
                        yl = jobs[("lin", round(de, 6) if de < 0.25 else "full")].result()
                        db, a = residual_db(jobs[(preset, mode, d)].result(), yl, rate)
                        rm[str(d)] = {"drive_eff": de, "residual_db": db, "gain": a}
                    rp[mode] = rm
                r["presets"][preset] = rp
            for mode, gm in g.items():
                r["u"][mode] = {str(d): u_stats(x, rate, d * gm) for d in DRIVES}
            res["tracks"][label] = r
            os.remove(src)
            print("  " + "  ".join(f"{p} {m}: " + " ".join(f"{r['presets'][p][m][str(d)]['residual_db']:6.1f}" for d in DRIVES)
                                   for p in PRESETS for m in ("now",)), flush=True)
    json.dump(res, open(os.path.join(out, "drive_loudness.json"), "w"), indent=1)
    report(res, out)


def report(res, out):
    T, tr = res["target_lufs"], res["tracks"]
    lines = [f"target T = {T:.2f} LUFS (-18 dBFS RMS pink noise in both channels, BS.1770-4 integrated)", ""]
    lines.append(f"{'track':<14} {'track LUFS':>10} {'excerpt':>8} {'peak':>7} {'gain track':>11} {'gain excerpt':>13}")
    for k, r in tr.items():
        lines.append(f"{k:<14} {r['lufs_track']:10.2f} {r['lufs_excerpt']:8.2f} {r['peak_dbfs']:7.2f} "
                     f"{r['gain_db']['track']:+11.2f} {r['gain_db']['excerpt']:+13.2f}")
    for p in PRESETS:
        for mode in ("now", "track", "excerpt"):
            lines += ["", f"{p}, drive {mode}: residual energy against the signal, dB (spread = max - min over the tracks)"]
            lines.append(f"{'track':<14}" + "".join(f"{d * 100:7.0f}%" for d in res["drives"]))
            for k, r in tr.items():
                lines.append(f"{k:<14}" + "".join(f"{r['presets'][p][mode][str(d)]['residual_db']:8.1f}" for d in res["drives"]))
            sp = [max(r["presets"][p][mode][str(d)]["residual_db"] for r in tr.values()) -
                  min(r["presets"][p][mode][str(d)]["residual_db"] for r in tr.values()) for d in res["drives"]]
            lines.append(f"{'spread':<14}" + "".join(f"{s:8.1f}" for s in sp))
    # the hypothesis: the loud track at D against the quiet one at D 10^(dL/20), interpolated (log drive) over all of the
    # quiet track's points (its "now" drives and its loudness-based ones)
    for p in PRESETS:
        lines += ["", f"{p}: loudest track at D against the quietest at D x 10^(dL/20) (dL = their whole-track loudness difference)"]
        ks = sorted(tr, key=lambda k: tr[k]["lufs_track"])
        quiet, loud = ks[0], ks[-1]
        dl = tr[loud]["lufs_track"] - tr[quiet]["lufs_track"]
        pts = sorted({(v["drive_eff"], v["residual_db"]) for m in ("now", "track", "excerpt")
                      for v in tr[quiet]["presets"][p][m].values()})
        xs, ys = np.log([a for a, _ in pts]), np.array([b for _, b in pts])
        lines.append(f"  {loud} ({tr[loud]['lufs_track']:.1f} LUFS) against {quiet} ({tr[quiet]['lufs_track']:.1f} LUFS), dL {dl:.1f} dB")
        for d in res["drives"]:
            dq = d * 10 ** (dl / 20)
            got = tr[loud]["presets"][p]["now"][str(d)]["residual_db"]
            if xs[0] <= math.log(dq) <= xs[-1]:
                pred = float(np.interp(math.log(dq), xs, ys))
                lines.append(f"  D {d * 100:4.0f}%: loud {got:6.1f} dB; quiet at {dq * 100:5.0f}% {pred:6.1f} dB; difference {got - pred:+5.1f}")
            else:
                lines.append(f"  D {d * 100:4.0f}%: loud {got:6.1f} dB; quiet at {dq * 100:5.0f}%: outside its measured drives")
    lines += ["", "set-300b: share of the curve's input samples (4x oversampled) beyond the rated drive (u > 0.86) and the grid",
              "current point (u > 1.03), each side (+ / -), and beyond the table (|u| > 6), drive now and track-loudness-based"]
    for mode in ("now", "track"):
        lines.append(f"  drive {mode}")
        lines.append(f"  {'track':<14}" + "".join(f"{d * 100:>22.0f}%" for d in res["drives"]))
        for k, r in tr.items():
            cells = []
            for d in res["drives"]:
                s = r["u"][mode][str(d)]
                cells.append(f"{100 * s['above_rated']:5.2f}/{100 * s['below_rated']:5.2f} {100 * s['above_grid']:5.2f}/{100 * s['below_grid']:5.2f}"
                             + ("*" if s["beyond_table"] > 0 else " "))
            lines.append(f"  {k:<14}" + "".join(f"{c:>23}" for c in cells))
        lines.append("  (each cell: rated +/- %, grid +/- %; * = some samples beyond the table)")
    txt = "\n".join(lines)
    open(os.path.join(out, "drive_loudness.txt"), "w", encoding="utf-8").write(txt + "\n")
    print(txt)
    plot(res, out)


def plot(res, out):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    tr = res["tracks"]
    fig, axes = plt.subplots(2, 2, figsize=(13, 9), sharey="row")
    colors = plt.rcParams["axes.prop_cycle"].by_key()["color"]
    for row, p in enumerate(PRESETS):
        for col, mode in enumerate(("now", "track")):
            ax = axes[row, col]
            for i, (k, r) in enumerate(tr.items()):
                ds = res["drives"]
                ax.plot([d * 100 for d in ds], [r["presets"][p][mode][str(d)]["residual_db"] for d in ds], "o-",
                        color=colors[i % len(colors)], label=f"{k} ({r['lufs_track']:.1f} LUFS)")
            ax.set_xscale("log"); ax.minorticks_off()
            ax.set_xticks([50, 100, 150, 200, 300, 400]); ax.set_xticklabels(["50", "100", "150", "200", "300", "400"])
            ax.set_title(f"{p}: drive {'as now' if mode == 'now' else 'loudness-based (whole track, target %.1f LUFS)' % res['target_lufs']}")
            ax.set_xlabel("drive (slider) %"); ax.set_ylabel("distortion residual vs signal, dB")
            ax.grid(True, which="both", alpha=0.3)
            if row == 0 and col == 0:
                ax.legend(fontsize=8)
    fig.tight_layout()
    fig.savefig(os.path.join(out, "drive_loudness.png"), dpi=110)


if __name__ == "__main__":
    if len(sys.argv) > 2 and sys.argv[1] == "--report":
        report(json.load(open(os.path.join(sys.argv[2], "drive_loudness.json"))), sys.argv[2])
    else:
        main()
