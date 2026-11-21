"""M2 (N28): Tube Color's soft limiter on music. For each track (local files: tracks.txt as for listening_set.py) and
drive: how often it turns the output down (render-dsp's color_limited, frames) and what it changes (its distortion:
the output against the same output 6 dB lower, where the limiter all but rests: the volume cancellation keeps the
curve's input the same, so twice that render is the output without the limiter).

  python limiter_stats.py <tracks.txt> [preset] [seconds]
"""
import json, math, os, re, subprocess, sys, tempfile

import numpy as np
import soundfile as sf

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import listening_set as LS  # noqa: E402


def render(x, rate, preset, drive, vol_db, td, tag):
    src, dst, st = (os.path.join(td, tag + e) for e in ("_in.wav", "_out.wav", ".json"))
    sf.write(src, x.astype(np.float32), rate, subtype="FLOAT")
    s = {"core": "pcm", "output": {"rate": "native"}, "color": {"enabled": True, "preset": preset, "drive": drive}}
    if vol_db:
        s["volume"] = {"mode": "digital", "db": vol_db}
    json.dump(s, open(st, "w"))
    r = subprocess.run([LS.CLI, "render-dsp", src, dst, "--settings", st, "--float"], capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(r.stderr)
    m = re.search(r"rendered (\d+) frames, clipped (\d+)(?:, color_limited (\d+))?", r.stdout)
    y, _ = sf.read(dst, always_2d=True)
    return y, int(m.group(1)), int(m.group(3) or 0)


def main():
    tracks = sys.argv[1]
    preset = sys.argv[2] if len(sys.argv) > 2 else "set-300b"
    secs = float(sys.argv[3]) if len(sys.argv) > 3 else 40.0
    rows = []
    with tempfile.TemporaryDirectory() as td:
        for line in open(tracks, encoding="utf-8"):
            if not line.strip():
                continue
            label, path = line.rstrip("\n").split("\t")
            info = sf.info(path)
            x, rate = sf.read(path, start=int(info.frames * 0.35), frames=int(secs * info.samplerate), always_2d=True)
            x = x[:, :2]
            for d in (1.0, 2.0):
                y, n, lim = render(x, rate, preset, d, 0.0, td, f"{label}_{d}")
                y6, _, lim6 = render(x, rate, preset, d, -20 * math.log10(2), td, f"{label}_{d}_6")
                free = 2 * y6                                  # the same output without the limiter (lim6: its own rest)
                m = min(len(y), len(free))
                diff = y[:m] - free[:m]
                rms = lambda v: math.sqrt(float(np.mean(v * v))) + 1e-30
                rows.append({"track": label, "drive": d, "limited_pct": 100.0 * lim / max(1, n), "limited_pct_at_-6dB": 100.0 * lim6 / max(1, n),
                             "distortion_db": 20 * math.log10(rms(diff) / rms(free[:m])),
                             "peak_reduction_db": 20 * math.log10(float(np.max(np.abs(free[:m]))) / float(np.max(np.abs(y[:m])))),
                             "peak_free": float(np.max(np.abs(free[:m])))})
                r = rows[-1]
                print(f"{label:<14} drive {d * 100:3.0f} %  limited {r['limited_pct']:6.2f} % of frames  "
                      f"its distortion {r['distortion_db']:6.1f} dB  peak {r['peak_free']:.3f} -> -{r['peak_reduction_db']:.2f} dB"
                      f"  (at -6 dB: {r['limited_pct_at_-6dB']:.2f} %)", flush=True)
    json.dump(rows, open(os.path.join(os.path.dirname(tracks), "limiter_stats.json"), "w"), indent=1)


if __name__ == "__main__":
    main()
