"""M2 listening set (N28): excerpts of local music through the engine (render-dsp) at several drives of one preset,
loudness-aligned (BS.1770), plus the same preset into a two-way loudspeaker (speaker_load.py) for comparison.

Drives above the engine's limit (kColorDriveMax) go through the stage's volume cancellation: the input k*x, the
digital volume 1/k, drive D/k. The gain stage takes the input back to x, the stage divides the volume out (its curve
sees k*x at drive D/k, i.e. x at drive D) and multiplies it back: the output is the engine's at drive D, and the soft
limiter after the stage sees that output, as at drive D. Checked here on a drive inside the limit (k = 2: exact).
Loudness: BS.1770's K-weighting over the excerpt (loudness.py; no gating), relative within a track.

  python listening_set.py <tracks.txt> <out dir> [preset] [seconds]

tracks.txt: one "label<TAB>path" per line (local files: not in the repository). Writes <out>/<label>_<setting>.wav
(24-bit, the source's rate) and <out>/README.txt.
"""
import json, math, os, subprocess, sys, tempfile

import numpy as np
import soundfile as sf
from scipy import signal

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import loudness as LU      # noqa: E402
import speaker_load as SL  # noqa: E402

CLI = os.environ.get("DAPHNE_CLI", os.path.join(os.path.dirname(HERE), "build", "bin", "Release", "daphne.exe"))
DRIVE_MAX = 4.0   # kColorDriveMax: 400 % from 2026-11-21 (the volume route only for drives above it)


def render(x, rate, preset, drive, td, tag, k=None):
    """x through the engine at `drive`: above DRIVE_MAX (or with k given) the input k*x, the volume 1/k, drive / k."""
    if k is None:
        k = max(1.0, drive / DRIVE_MAX)
    src = os.path.join(td, tag + "_in.wav")
    dst = os.path.join(td, tag + "_out.wav")
    st = os.path.join(td, tag + ".json")
    sf.write(src, (x * k).astype(np.float32), rate, subtype="FLOAT")
    s = {"core": "pcm", "output": {"rate": "native"}, "color": {"enabled": drive > 0, "preset": preset, "drive": drive / k}}
    if k != 1.0:
        s["volume"] = {"mode": "digital", "db": -20 * math.log10(k)}
    json.dump(s, open(st, "w"))
    r = subprocess.run([CLI, "render-dsp", src, dst, "--settings", st, "--float"], capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(r.stderr)
    y, _ = sf.read(dst, always_2d=True)
    return y


def lufs(x, rate):
    if rate != 48000:
        g = math.gcd(rate, 48000)
        x = signal.resample_poly(x, 48000 // g, rate // g, axis=0)
    return LU.lufs(x)


def speaker_filter(y, rate, preset):
    """The response change into the vented two-way loudspeaker against 8 ohm (complex, exact per FFT bin)."""
    n = 1 << int(math.ceil(math.log2(len(y) + rate)))
    fz = np.loadtxt(os.path.join(SL.OUT, f"zout_{preset}.csv"), delimiter=",", skiprows=1)
    f = np.fft.rfftfreq(n, 1.0 / rate)
    ff = np.maximum(f, 1.0)
    zo = np.interp(np.log(ff), np.log(fz[:, 0]), fz[:, 1]) + 1j * np.interp(np.log(ff), np.log(fz[:, 0]), fz[:, 2])
    _, zf = SL.speaker("vented")
    zs = zf(ff)
    h = (zs / (zs + zo)) / (8.0 / (8.0 + zo))
    h[0] = h[1].real
    Y = np.fft.rfft(y, n, axis=0) * h[:, None]
    return np.fft.irfft(Y, n, axis=0)[:len(y)]


def main():
    tracks, out = sys.argv[1], sys.argv[2]
    preset = sys.argv[3] if len(sys.argv) > 3 else "set-300b"
    secs = float(sys.argv[4]) if len(sys.argv) > 4 else 40.0
    os.makedirs(out, exist_ok=True)
    drives = (0.0, 0.5, 1.0, 2.0, 3.0, 4.0)
    notes = []
    with tempfile.TemporaryDirectory() as td:
        # the identity, checked once: drive 1 on x against drive 0.5 on 2x, halved
        rng = np.random.default_rng(1)
        t = rng.standard_normal((44100 * 2, 2)) * 0.15
        a = render(t, 44100, preset, 1.0, td, "id_a", k=1.0)
        b = render(t, 44100, preset, 1.0, td, "id_b", k=2.0)   # input 2x, volume -6.02 dB, drive 50 %
        ident = float(np.max(np.abs(a - b)))
        notes.append(f"check: drive 100 % against input 2x / volume 1/2 / drive 50 %: max difference {ident:.2e}")
        if ident > 1e-6:
            raise SystemExit(f"the volume route is not the engine at that drive: {ident:.2e}")
        for line in open(tracks, encoding="utf-8"):
            if not line.strip():
                continue
            label, path = line.rstrip("\n").split("\t")
            info = sf.info(path)
            start = int(info.frames * 0.35)
            x, rate = sf.read(path, start=start, frames=int(secs * info.samplerate), always_2d=True)
            x = x[:, :2]
            fade = int(0.02 * rate)
            x[:fade] *= np.linspace(0, 1, fade)[:, None]
            x[-fade:] *= np.linspace(1, 0, fade)[:, None]
            outs = {}
            for d in drives:
                outs[f"d{int(d * 100):03d}"] = render(x, rate, preset, d, td, f"{label}_{d}")
            outs["d100_speaker"] = speaker_filter(outs["d100"], rate, preset)
            ref = lufs(outs["d000"], rate)
            # all at the original's loudness, lowered together so that no file needs more than -0.1 dBFS
            gains = {k: ref - lufs(v, rate) for k, v in outs.items()}
            head = min(0.0, min(-0.1 - 20 * math.log10(np.max(np.abs(v)) * 10 ** (gains[k] / 20)) for k, v in outs.items()))
            for k, v in outs.items():
                g = gains[k] + head
                sf.write(os.path.join(out, f"{label}_{k}.wav"), v * 10 ** (g / 20), rate, subtype="PCM_24")
                notes.append(f"{label}_{k}.wav: loudness gain {g:+.2f} dB (match {gains[k]:+.2f}, common headroom {head:+.2f})")
            print("done", label, flush=True)
    open(os.path.join(out, "README.txt"), "w", encoding="utf-8").write(
        f"Tube Color M2 listening set: {preset}, drives 0 / 50 / 100 / 200 / 300 / 400 %, and drive 100 % into a two-way\n"
        "loudspeaker (d100_speaker, against the 8 ohm resistor of d100). Every file of a track at the same BS.1770 loudness\n"
        "(the original's, all lowered together where one needed it to stay below -0.1 dBFS).\n\n" + "\n".join(notes) + "\n")


if __name__ == "__main__":
    main()
