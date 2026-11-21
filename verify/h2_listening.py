"""M2 (N28): three listening files for the loudness-based drive (drive_loudness.py), h2-pure at drive 200 %:
  <loud>_d200_now        the loudest track, drive as now
  <loud>_d200_loudness   the same, drive loudness-based: D 10^((T - L)/20), L the whole track's integrated loudness
  <quiet>_d200_now       a classical track, drive as now (the reference)
The stage alone, as the listening at low volume heard it (the digital volume at -12.04 dB: the curve sees the master's
level, the limiter after the stage rests), the set-300b loudspeaker load (bookshelf-vented, the default). All three at
the same BS.1770-4 integrated loudness, as loud as the one with the least headroom allows with peaks at -1 dBFS.

  python h2_listening.py <tracks.txt> <out dir> <loud label> <classical label> [seconds]
"""
import math, os, sys, tempfile

import numpy as np
import soundfile as sf

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import drive_loudness as DL   # noqa: E402
import loudness as LU         # noqa: E402


def main():
    tracks, out, loud, quiet = sys.argv[1:5]
    secs = float(sys.argv[5]) if len(sys.argv) > 5 else 40.0
    os.makedirs(out, exist_ok=True)
    paths = dict(line.rstrip("\n").split("\t") for line in open(tracks, encoding="utf-8") if line.strip())
    T = DL.calibration_loudness()
    outs, notes = {}, [f"target T {T:.2f} LUFS (the drive's calibration: -18 dBFS RMS pink noise, both channels)"]
    with tempfile.TemporaryDirectory() as td:
        for label, modes in ((loud, ("now", "loudness")), (quiet, ("now",))):
            whole, rate = sf.read(paths[label], always_2d=True)
            whole = whole[:, :2]
            L = LU.integrated(whole, rate)
            s0 = int(len(whole) * 0.35)
            x = whole[s0:s0 + int(secs * rate)].copy()
            fade = int(0.02 * rate)
            x[:fade] *= np.linspace(0, 1, fade)[:, None]
            x[-fade:] *= np.linspace(1, 0, fade)[:, None]
            src = os.path.join(td, label + ".wav")
            sf.write(src, x.astype(np.float32), rate, subtype="FLOAT")
            for m in modes:
                d = 2.0 if m == "now" else 2.0 * 10 ** ((T - L) / 20)
                outs[f"{label}_h2-pure_d200_{m}"] = (DL.render(src, rate, "h2-pure", d, td, f"{label}_{m}"), rate)
                notes.append(f"{label}_h2-pure_d200_{m}: track {L:.2f} LUFS, the curve at drive {d * 100:.0f} %")
    lv = {k: LU.integrated(y, r) for k, (y, r) in outs.items()}
    level = min(-14.0, min(lv[k] - 1.0 - 20 * math.log10(np.max(np.abs(y))) for k, (y, _) in outs.items()))
    for k, (y, r) in outs.items():
        g = level - lv[k]
        sf.write(os.path.join(out, k + ".wav"), y * 10 ** (g / 20), r, subtype="PCM_24")
        notes.append(f"{k}.wav: {lv[k]:.2f} -> {level:.2f} LUFS (gain {g:+.2f} dB), peak {20 * math.log10(np.max(np.abs(y))) + g:.2f} dBFS")
    open(os.path.join(out, "README.txt"), "w", encoding="utf-8").write(
        "Tube Color M2: h2-pure at drive 200 %, as now and loudness-based (doc/N28). The stage alone (the limiter rests),\n"
        "load bookshelf-vented; every file at the same BS.1770-4 integrated loudness.\n\n" + "\n".join(notes) + "\n")
    print("\n".join(notes))


if __name__ == "__main__":
    main()
