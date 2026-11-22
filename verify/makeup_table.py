"""M2 (N28): the drive's automatic makeup for set-300b, per load, as the engine plays it by default (2026-11-22).

The table: for each slider drive D (0 .. 200 %) and load (bookshelf-vented-smooth, resistor-8), the median over the six
test tracks of the loudness the stage takes away: BS.1770-4 integrated loudness of the output before the limiter
against the original (bypass plays the original). Each track at its own level alignment, conservative (its whole-track
measures from `daphne levels`, as the player's scan; the gain passed to render-dsp --color-align-db). Before the
limiter: the stage alone (the digital volume at -12.04 dB, which the stage cancels, so the limiter rests; the output
times 4). The engine's table at the time is included in what it renders: the new table is the old one minus the median
it leaves (one pass from an empty table gives the table; a second pass checks it).

  python makeup_table.py <tracks.txt> [--check] [seconds]
      writes data/curves/set-300b_makeup.json (without --check); --check: the A/B errors with the table in the engine,
      per drive the largest over the six tracks, before the limiter and through it at volume 0 dB
"""
import concurrent.futures as cf, json, math, os, subprocess, sys, tempfile

import numpy as np
import soundfile as sf

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import loudness as LU   # noqa: E402

CLI = os.environ.get("DAPHNE_CLI", os.path.join(os.path.dirname(HERE), "build", "bin", "Release", "daphne.exe"))
OUT = os.path.join(HERE, "data", "curves", "set-300b_makeup.json")
DRIVES = (0.05, 0.1, 0.15, 0.2, 0.25, 0.35, 0.5, 0.65, 0.8, 1.0, 1.2, 1.4, 1.6, 1.8, 2.0)
LOADS = ("bookshelf-vented-smooth", "resistor-8")
REF_LUFS, REF_P999 = -14.28, -0.81   # track_levels.h
VOL_STAGE = -20 * math.log10(4.0)


def levels(path):
    r = subprocess.run([CLI, "levels", path], capture_output=True, text=True, encoding="utf-8")
    if r.returncode:
        raise RuntimeError(r.stderr)
    lufs, p999 = r.stdout.split("\t")[:2]
    return float(lufs.split()[0]), float(p999.split()[0])


def conservative_db(lufs, p999):
    return min(0.0, REF_LUFS - lufs, REF_P999 - p999)


def render(src, rate, load, drive, align_db, vol_db, td, tag):
    dst = os.path.join(td, tag + "_out.wav")
    st = os.path.join(td, tag + ".json")
    json.dump({"core": "pcm", "output": {"rate": "native"},
               "color": {"enabled": True, "preset": "set-300b", "drive": drive, "load": load},
               "volume": {"mode": "digital", "db": vol_db}}, open(st, "w"))
    r = subprocess.run([CLI, "render-dsp", src, dst, "--settings", st, "--float", "--color-align-db", f"{align_db:.4f}"],
                       capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(r.stderr)
    y, _ = sf.read(dst, always_2d=True)
    os.remove(dst); os.remove(st)
    return y * 10 ** (-vol_db / 20)


def main():
    tracks = sys.argv[1]
    check = "--check" in sys.argv
    rest = [a for a in sys.argv[2:] if a != "--check"]
    secs = float(rest[0]) if rest else 40.0
    old = json.load(open(OUT))["tables"] if os.path.exists(OUT) else {}
    items = []
    with tempfile.TemporaryDirectory() as td, cf.ThreadPoolExecutor(max(2, (os.cpu_count() or 4) // 2)) as pool:
        for line in open(tracks, encoding="utf-8"):
            if not line.strip():
                continue
            label, path = line.rstrip("\r\n").split("\t")
            lufs, p999 = levels(path)
            g = conservative_db(lufs, p999)
            info = sf.info(path)
            x, rate = sf.read(path, start=int(info.frames * 0.35), frames=int(secs * info.samplerate), always_2d=True)
            x = x[:, :2]
            src = os.path.join(td, label + ".wav")
            sf.write(src, x.astype(np.float32), rate, subtype="FLOAT")
            l0 = LU.integrated(x, rate)
            jobs = {(load, d, v): pool.submit(render, src, rate, load, d, g, v, td, f"{label}_{load}_{d}_{v}")
                    for load in LOADS for d in DRIVES for v in ((VOL_STAGE, 0.0) if check else (VOL_STAGE,))}
            loss = {k: LU.integrated(f.result(), rate) - l0 for k, f in jobs.items()}
            items.append((label, lufs, p999, g, loss))
            print(f"{label}: {lufs:.2f} LUFS, 99.9 % {p999:.2f} dBFS, conservative {g:+.2f} dB", flush=True)
    res = {"drives": DRIVES, "alignment": "conservative", "tracks": {}, "tables": {}}
    for label, lufs, p999, g, loss in items:
        res["tracks"][label] = {"lufs": lufs, "p999_db": p999, "align_db": g,
                                "change_db": {load: {str(d): loss[(load, d, VOL_STAGE)] for d in DRIVES} for load in LOADS}}
    if check:
        print(f"\nA/B against bypass with the engine's table: the largest error over the six tracks, dB (before the limiter / through it at 0 dB volume)")
        print(f"{'drive':>6}" + "".join(f"{load:>34}" for load in LOADS))
        worst = {load: 0.0 for load in LOADS}
        for d in DRIVES:
            cells = []
            for load in LOADS:
                pre = [it[4][(load, d, VOL_STAGE)] for it in items]
                lim = [it[4][(load, d, 0.0)] for it in items]
                a, b = max(pre, key=abs), max(lim, key=abs)
                worst[load] = max(worst[load], abs(a))
                cells.append(f"{a:+7.2f} / {b:+7.2f} (median {np.median(pre):+.2f})")
            print(f"{d * 100:5.0f}%" + "".join(f"{c:>34}" for c in cells))
        print("largest before the limiter: " + ", ".join(f"{k} {v:.2f} dB" for k, v in worst.items()))
        json.dump(res, open(os.path.join(os.path.dirname(tracks), "set-300b_makeup_check.json"), "w"), indent=1)
        return
    for load in LOADS:
        prev = dict(old.get(load, []))
        rows = []
        for d in DRIVES:
            med = float(np.median([it[4][(load, d, VOL_STAGE)] for it in items]))
            rows.append([d, round(prev.get(d, 0.0) - med, 3)])
        res["tables"][load] = rows
        print(f"{load}: " + "  ".join(f"{d * 100:.0f}% {g:+.2f}" for d, g in rows))
    json.dump({"tables": res["tables"], "drives": DRIVES, "alignment": "conservative",
               "method": "median of six tracks, BS.1770-4 integrated, before the limiter, conservative alignment"},
              open(OUT, "w"), indent=1)
    json.dump(res, open(os.path.join(os.path.dirname(tracks), "set-300b_makeup_tracks.json"), "w"), indent=1)


if __name__ == "__main__":
    main()
