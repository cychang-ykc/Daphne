"""M2 (N28): the loudspeaker loads in time: what rings (the listening: "compressed, split, smeared" with
bookshelf-vented, cleaner with sealed, cleanest with the 8 ohm resistor).

For each load (data/loads/loads.json, the sections the stage runs; at its rate 352.8 kHz):
  - the sections' poles and zeros: natural frequency and Q (a pole pair of high Q next to a zero pair of another Q is
    a resonance that rings; pole and zero alike cancel)
  - the response (dB against 1 kHz) and its group delay (ms), 20 Hz - 20 kHz
  - tone-burst decay: a sine burst (40 cycles) through the load, at each resonance and a few frequencies, against the
    same burst through nothing (the 8 ohm resistor, H = 1): the residual after the burst ends, its level 2 / 10 / 30 ms
    later (dB against the burst) and the time it takes to fall below -40 and -60 dB
  - the impulse response's tail: h minus its first 0.5 ms (the direct part), its energy against the whole and the time
    the envelope takes to fall 60 dB below the peak

  python load_diag.py [preset] [loads ...]      prints the tables; writes data/loads/diag_<preset>.json and a plot
"""
import json, math, os, sys

import numpy as np
from scipy import signal

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import load_filters as LF  # noqa: E402

RATE = 352800
DATA = os.path.join(HERE, "data", "loads")


def digital_sos(secs, rate=RATE):
    """The sections as the stage runs them (TPT = the bilinear transform prewarped at each corner) as an SOS array."""
    sos = []
    for x in secs:
        w0 = 2 * math.pi * x["f"]
        k = w0 / math.tan(math.pi * x["f"] / rate)   # prewarp: s = k (z - 1) / (z + 1), S = s / w0
        c2, c1, c0 = x["c"]
        if x["kind"] == "General2":
            b, a = [c2 / w0 ** 2, c1 / w0, c0], [1 / w0 ** 2, 1 / (x["q"] * w0), 1.0]
        else:
            b, a = [0.0, c1 / w0, c0], [0.0, 1 / w0, 1.0]
        bz, az = signal.bilinear(b, a, fs=k / 2)
        bz = np.pad(bz, (0, 3 - len(bz))); az = np.pad(az, (0, 3 - len(az)))
        sos.append(np.concatenate([bz / az[0], az / az[0]]))
    return np.array(sos)


def roots_of(secs):
    out = []
    for x in secs:
        c2, c1, c0 = x["c"]
        if x["kind"] == "General2":
            zq = math.sqrt(abs(c0 * c2)) / c1 if c2 != 0 and c1 != 0 else float("inf")
            zf = x["f"] * math.sqrt(abs(c0 / c2)) if c2 != 0 else float("nan")
            out.append({"pole_hz": x["f"], "pole_q": x["q"], "zero_hz": zf, "zero_q": zq})
        else:
            out.append({"pole_hz": x["f"], "pole_q": 0.5, "zero_hz": x["f"] * abs(c0 / c1) if c1 else float("nan"), "zero_q": 0.5})
    return out


def envelope_db(y, rate, hz, ref):
    """Sliding RMS over one period (at least 1 ms), dB against ref (an amplitude): a causal measure, no Hilbert smear."""
    n = max(int(rate / hz), int(0.001 * rate))
    c = np.concatenate([[0.0], np.cumsum(y * y)])
    rms = np.sqrt(np.maximum(c[n:] - c[:-n], 0.0) / n) * math.sqrt(2)
    return 20 * np.log10(np.maximum(rms, 1e-20) / ref)


def burst_decay(sos, hz, rate=RATE, cycles=40):
    """A sine burst with raised-cosine edges of one period; what the load leaves after it ends (the 8 ohm resistor: 0)."""
    n_on = int(round(cycles * rate / hz))
    n = n_on + int(0.3 * rate)
    t = np.arange(n) / rate
    ramp = int(rate / hz)
    w = np.ones(n); w[:ramp] = 0.5 - 0.5 * np.cos(np.pi * np.arange(ramp) / ramp)
    w[n_on - ramp:n_on] = 0.5 + 0.5 * np.cos(np.pi * np.arange(ramp) / ramp); w[n_on:] = 0
    x = np.sin(2 * np.pi * hz * t) * w
    y = signal.sosfilt(sos, x)
    level = np.sqrt(np.mean(y[n_on // 2:n_on - ramp] ** 2)) * math.sqrt(2)
    win = max(int(rate / hz), int(0.001 * rate))
    env = envelope_db(y, rate, hz, level)[n_on:]   # windows that start at or after the end
    def after(ms):
        return float(env[min(len(env) - 1, int(ms * 1e-3 * rate))])
    def below(db):
        i = np.where(env > db)[0]
        return float((i[-1] + 1) / rate * 1e3) if len(i) else 0.0
    return {"hz": hz, "at_2ms_db": after(2), "at_10ms_db": after(10), "at_30ms_db": after(30),
            "to_-40db_ms": below(-40), "to_-60db_ms": below(-60)}


def tail_of(sos, rate=RATE, seconds=3.0, above_hz=15.0):
    """The impulse response's decay (Schroeder's backward integral of h^2): the time it takes to fall 20 / 40 / 60 dB,
    of the whole response and of the response above `above_hz` (the subsonic sections' near-cancelling pole-zero pairs
    ring for seconds with next to no energy; a 4th-order high-pass takes them out of the second measure)."""
    n = int(seconds * rate)
    d = np.zeros(n); d[0] = 1.0
    h = signal.sosfilt(sos, d)
    out = {}
    for name, hh in (("all", h), ("above", signal.sosfilt(signal.butter(4, above_hz, "highpass", fs=rate, output="sos"), h))):
        e = np.cumsum((hh ** 2)[::-1])[::-1]
        edc = 10 * np.log10(np.maximum(e / e[0], 1e-30))
        for db in (20, 40, 60):
            i = np.where(edc > -db)[0]
            out[f"{name}_to_-{db}db_ms"] = float((i[-1] + 1) / rate * 1e3)
    return out


def group_delay_ms(sos, f, rate=RATE):
    w, gd = signal.group_delay((np.concatenate([[1.0], [0.0]]), [1.0]), w=f, fs=rate)   # placeholder (overwritten)
    total = np.zeros_like(f)
    for s in sos:
        _, g = signal.group_delay((s[:3], s[3:]), w=f, fs=rate)
        total += g
    return total / rate * 1e3


def main():
    preset = sys.argv[1] if len(sys.argv) > 1 else "set-300b"
    loads = sys.argv[2:] or ["bookshelf-vented", "bookshelf-sealed"]
    data = json.load(open(os.path.join(DATA, "loads.json")))[preset]
    f = np.logspace(math.log10(20), math.log10(20000), 400)
    probes = [24, 40, 49, 60, 85, 120, 250, 500, 1000, 1500, 2500, 4000, 10000]
    res = {}
    for load in loads:
        secs = data[load]["sections"]
        sos = digital_sos(secs)
        _, hz = signal.sosfreqz(sos, worN=f, fs=RATE)
        db = 20 * np.log10(np.abs(hz)); db -= np.interp(1000, f, db)
        gd = group_delay_ms(sos, f)
        r = {"roots": roots_of(secs), "db": db.tolist(), "gd_ms": gd.tolist(),
             "bursts": [burst_decay(sos, p) for p in probes], "tail": tail_of(sos)}
        res[load] = r
        print(f"\n== {preset}, {load}: the sections (poles / zeros: Hz, Q)")
        for x in r["roots"]:
            flag = "  <- Q > 1" if max(x["pole_q"], x["zero_q"] if np.isfinite(x["zero_q"]) else 0) > 1 else ""
            print(f"   pole {x['pole_hz']:9.1f} Hz Q {x['pole_q']:5.2f}   zero {x['zero_hz']:9.1f} Hz Q {x['zero_q']:5.2f}{flag}")
        print(f"   response (dB re 1 kHz): " + "  ".join(f"{p} {np.interp(p, f, db):+.2f}" for p in (30, 50, 85, 150, 250, 2500, 10000, 18000)))
        i = np.argmax(np.abs(gd - np.interp(1000, f, gd)) * (f > 30))
        print(f"   group delay: at 1 kHz {np.interp(1000, f, gd):.3f} ms; largest deviation from it {gd[i] - np.interp(1000, f, gd):+.2f} ms at {f[i]:.0f} Hz;"
              f" at 50 / 85 / 250 / 2500 Hz: " + " / ".join(f"{np.interp(p, f, gd):.2f}" for p in (50, 85, 250, 2500)) + " ms")
        tl = r["tail"]
        print(f"   impulse response, energy decay (Schroeder) to -20 / -40 / -60 dB: {tl['all_to_-20db_ms']:.2f} / {tl['all_to_-40db_ms']:.1f} / "
              f"{tl['all_to_-60db_ms']:.0f} ms; above 15 Hz: {tl['above_to_-20db_ms']:.2f} / {tl['above_to_-40db_ms']:.1f} / {tl['above_to_-60db_ms']:.1f} ms"
              f"  [8 ohm: 0]")
        print(f"   tone bursts (40 cycles): what is left after the burst ends, dB re the burst [8 ohm: nothing]")
        for b in r["bursts"]:
            print(f"     {b['hz']:6.0f} Hz: +2 ms {b['at_2ms_db']:6.1f}  +10 ms {b['at_10ms_db']:6.1f}  +30 ms {b['at_30ms_db']:6.1f}   "
                  f"-40 dB at {b['to_-40db_ms']:6.1f} ms, -60 dB at {b['to_-60db_ms']:6.1f} ms")
    json.dump(res, open(os.path.join(DATA, f"diag_{preset}.json"), "w"))
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    fig, ax = plt.subplots(2, 1, figsize=(10, 7), sharex=True)
    for load, r in res.items():
        ax[0].semilogx(f, r["db"], label=load)
        ax[1].semilogx(f, r["gd_ms"], label=load)
    ax[0].set_ylabel("dB re 1 kHz"); ax[1].set_ylabel("group delay, ms"); ax[1].set_xlabel("Hz")
    for a in ax:
        a.grid(True, which="both", alpha=0.3); a.legend()
    ax[0].set_title(f"{preset}: the loads (the stage's sections at {RATE} Hz)")
    fig.tight_layout()
    fig.savefig(os.path.join(DATA, f"diag_{preset}.png"), dpi=100)


if __name__ == "__main__":
    main()
