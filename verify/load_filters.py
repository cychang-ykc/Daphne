"""M2 (N28): the virtual loudspeaker load as filters for the Tube Color stage.

For each preset (and pp-el34-ul feedback step) and load, the response change against the 8 ohm resistor of the
reference data, H = (Z / (Z + Zout)) / (8 / (8 + Zout)) (speaker_load.py: Zout from the circuit, Z the loudspeaker
model), is fitted as a rational function (vector fitting, Gustavsen & Semlyen 1999) and split into first- and
second-order sections the stage runs after its post sections (general sections: a state-variable filter mixing its
high-, band- and low-pass outputs; prewarped at the poles). Checked against the target and against the circuit with
the loudspeaker in it (ngspice AC; acceptance: 0.1 dB, 20 Hz - 20 kHz, at the stage's rate). The loudness change of
each load on the six test tracks (local files) gives its makeup gain (the median, as for the curve's table).

  python load_filters.py [tracks.txt]       data/loads/loads.json (+ the checks printed)
"""
import json, math, os, sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import speaker_load as SL  # noqa: E402

OUT = os.path.join(HERE, "data", "loads")
LOADS = ("bookshelf-vented", "bookshelf-sealed")
CASES = [("set-300b", None), ("set-300b-x", None), ("set-2a3", None), ("set-845", None)] + \
        [("pp-el34-ul", F) for F in (0, 6, 12, 20, 30)]
N_POLES = 14
STAGE_RATES = (352800, 384000, 705600, 768000)
R0 = 352800   # the fit's rate: sections fitted on the bilinear transform's warped frequency at R0 (exact there)


def _basis(s, cp, rp):
    """Real basis: per complex pole p (Im > 0) 1/(s-p) + 1/(s-p*), j/(s-p) - j/(s-p*); per real pole 1/(s-p)."""
    cols = []
    for p in cp:
        a, b = 1.0 / (s - p), 1.0 / (s - np.conj(p))
        cols += [a + b, 1j * a - 1j * b]
    for p in rp:
        cols.append(1.0 / (s - p))
    return np.stack(cols, axis=1)


def _state_space(cp, rp, c):
    """Real (A, b, c) of sum over the poles with the basis coefficients c (vectfit3's block form)."""
    n = 2 * len(cp) + len(rp)
    A = np.zeros((n, n)); b = np.zeros(n)
    for k, p in enumerate(cp):
        A[2 * k:2 * k + 2, 2 * k:2 * k + 2] = [[p.real, p.imag], [-p.imag, p.real]]
        b[2 * k] = 2.0
    for k, p in enumerate(rp):
        i = 2 * len(cp) + k
        A[i, i] = p.real
        b[i] = 1.0
    return A, b, np.asarray(c, dtype=float)


def vector_fit(s, h, w, n, iters=15):
    """h(s) ~ d + sum of the poles' terms (Gustavsen & Semlyen; real arithmetic, stable poles). Returns (complex poles
    with Im > 0, real poles, the basis coefficients, d)."""
    f = np.abs(s.imag)
    beta = np.logspace(math.log10(f.min()), math.log10(f.max()), n // 2)
    cp = [complex(-b / 100, b) for b in beta]
    rp = []
    for _ in range(iters):
        Phi = _basis(s, cp, rp)
        M = np.concatenate([Phi, np.ones((len(s), 1)), -h[:, None] * Phi], axis=1) * w[:, None]
        rhs = h * w
        x, *_ = np.linalg.lstsq(np.concatenate([M.real, M.imag]), np.concatenate([rhs.real, rhs.imag]), rcond=None)
        m = Phi.shape[1]
        A, b, c = _state_space(cp, rp, x[m + 1:])
        z = np.linalg.eigvals(A - np.outer(b, c))
        z = np.where(z.real > 0, -z.real + 1j * z.imag, z)
        cp = sorted([q for q in z if q.imag > 1e-9 * max(1.0, abs(q))], key=abs)
        rp = sorted([q.real for q in z if abs(q.imag) <= 1e-9 * max(1.0, abs(q))], key=abs)
    Phi = _basis(s, cp, rp)
    M = np.concatenate([Phi, np.ones((len(s), 1))], axis=1) * w[:, None]
    rhs = h * w
    x, *_ = np.linalg.lstsq(np.concatenate([M.real, M.imag]), np.concatenate([rhs.real, rhs.imag]), rcond=None)
    return cp, rp, x[:-1], float(x[-1])


def zeros_of(cp, rp, c, d):
    A, b, cc = _state_space(cp, rp, c)
    return np.linalg.eigvals(A - np.outer(b, cc) / d)


def sections_from(poles, zeros, d):
    """Second-order sections (and one first-order if needed): each its poles' natural frequency, Q and the
    normalized numerator c2 S^2 + c1 S + c0 (S = s / w0); the constant d in the first."""
    def split(v):
        c = sorted([x for x in v if x.imag > 1e-6 * max(1, abs(x))], key=abs)
        r = sorted([x.real for x in v if abs(x.imag) <= 1e-6 * max(1, abs(x))], key=abs)
        return c, r
    pc, pr = split(poles)
    zc, zr = split(zeros)
    den = [(p, p.conjugate()) for p in pc] + [(complex(pr[i]), complex(pr[i + 1])) for i in range(0, len(pr) - 1, 2)]
    lone_p = [pr[-1]] if len(pr) % 2 else []
    num = [(z, z.conjugate()) for z in zc] + [(complex(zr[i]), complex(zr[i + 1])) for i in range(0, len(zr) - 1, 2)]
    lone_z = [zr[-1]] if len(zr) % 2 else []
    # pair each denominator with the nearest numerator in the s-plane (on a log-frequency scale): each section then
    # stays near unity, and none carries a large gain another one takes back
    den.sort(key=lambda q: abs(q[0] * q[1]))
    left = list(num)
    paired = []
    for p1, p2 in den:
        if not left:
            paired.append((None, None))
            continue
        def dist(z):
            a, b = abs(z[0] * z[1]) ** 0.5, abs(p1 * p2) ** 0.5
            return abs(math.log(max(a, 1e-9) / max(b, 1e-9))) + 0.1 * abs(math.atan2(abs(z[0].imag), -z[0].real) - math.atan2(abs(p1.imag), -p1.real))
        j = min(range(len(left)), key=lambda i: dist(left[i]))
        paired.append(left.pop(j))
    num = paired
    secs = []
    for i, (p1, p2) in enumerate(den):
        w0 = math.sqrt(abs((p1 * p2).real))
        q = w0 / max(1e-12, -(p1 + p2).real)
        z1, z2 = num[i] if i < len(num) else (None, None)
        if z1 is None:
            c2, c1, c0 = 0.0, 0.0, 1.0
        else:
            c2, c1, c0 = 1.0, float((-(z1 + z2) / w0).real), float(((z1 * z2) / w0 ** 2).real)
        secs.append({"kind": "General2", "f": w0 / (2 * math.pi), "q": q, "c": [c2, c1, c0]})
    if lone_p:
        w = -lone_p[0]
        c1, c0 = (1.0, -lone_z[0] / w) if lone_z else (0.0, 1.0)
        secs.append({"kind": "General1", "f": w / (2 * math.pi), "q": 0.7071, "c": [0.0, c1, c0]})
    # the gain: the sections are monic at infinity where they have zeros; match the fit at 1 kHz
    return secs


def sections_response(secs, f, rate=None):
    s = 1j * 2 * np.pi * f
    h = np.ones_like(s)
    for x in secs:
        w0 = 2 * np.pi * x["f"]
        if rate:   # the stage's TPT: the bilinear transform prewarped at w0
            S = 1j * np.tan(np.pi * f / rate) / math.tan(math.pi * x["f"] / rate)
        else:
            S = s / w0
        c2, c1, c0 = x["c"]
        if x["kind"] == "General2":
            h *= (c2 * S * S + c1 * S + c0) / (S * S + S / x["q"] + 1)
        else:
            h *= (c1 * S + c0) / (S + 1)
    return h


def target(preset, fb, load, f):
    fz, z = SL.zout(preset, fb if fb is not None else 6)
    zo = np.interp(np.log(f), np.log(fz), z.real) + 1j * np.interp(np.log(f), np.log(fz), z.imag)
    _, zf = SL.speaker(load.split("-")[1])
    zs = zf(f)
    return (zs / (zs + zo)) / (8.0 / (8.0 + zo))


def main():
    os.makedirs(OUT, exist_ok=True)
    f = np.logspace(math.log10(5.0), math.log10(24e3), 600)   # (poles and zeros in the audio band: the bilinear transform at the stage's rate warps them by 1 % at 20 kHz, 20 % at 80 kHz)
    band = (f >= 20) & (f <= 20000)
    w = np.where(band, 1.0, 0.2)
    data = {}
    for preset, fb in CASES:
        key = preset if fb is None else f"{preset}:{fb}"
        for load in LOADS:
            h = target(preset, fb, load, f)
            best = None
            for n in (14, 16, 18, 20, 22):
                fw = R0 / np.pi * np.tan(np.pi * f / R0)          # where the stage's sections see f (at R0)
                cp, rp, c, d = vector_fit(1j * 2 * np.pi * fw, h, w, n, iters=20)
                zeros = zeros_of(cp, rp, c, d)
                poles = np.array(list(cp) + [np.conj(q) for q in cp] + [complex(q) for q in rp])
                secs = sections_from(poles, zeros, d)
                for x in secs:   # their corners as the stage takes them: tan(pi fc / R0) = pi F / R0
                    x["f"] = R0 / np.pi * math.atan(np.pi * x["f"] / R0)
                if any(x["f"] >= 0.45 * R0 or not np.isfinite(x["f"]) for x in secs):
                    continue
                g = sections_response(secs, f, R0)
                k = np.interp(1000.0, f, (h / g).real)   # the overall constant
                secs[0]["c"] = [c_ * k for c_ in secs[0]["c"]]
                err = np.max(np.abs(20 * np.log10(np.abs(sections_response(secs, f, R0) / h)))[band])
                worst = max(np.max(np.abs(20 * np.log10(np.abs(sections_response(secs, f, r) / h)))[band]) for r in STAGE_RATES)
                if best is None or worst < best[2]:
                    best = (err, secs, worst)
                if worst < 0.05:
                    break
            err, secs, worst_rate = best
            data.setdefault(key, {})[load] = {"sections": secs, "fit_db": float(err), "at_stage_rates_db": float(worst_rate)}
            print(f"{key:<16} {load:<17} {len(secs):2d} sections  fit {err:.4f} dB  at the stage's rates {worst_rate:.4f} dB", flush=True)
    # acceptance: the sections at the stage's rates against the circuit with the loudspeaker in it (ngspice AC; the
    # Thevenin target itself is 0.004 dB from it, speaker_load.py)
    for preset, fb in CASES:
        key = preset if fb is None else f"{preset}:{fb}"
        net8 = SL.response(preset, ["Rspk o 0 8"], fb if fb is not None else 6)
        for load in LOADS:
            lines, _ = SL.speaker(load.split("-")[1])
            fl, hl = SL.response(preset, lines, fb if fb is not None else 6)
            direct = hl / net8[1]
            sel = (fl >= 20) & (fl <= 20000)
            worst = max(np.max(np.abs(20 * np.log10(np.abs(sections_response(data[key][load]["sections"], fl, r) / direct)))[sel])
                        for r in STAGE_RATES)
            data[key][load]["vs_circuit_db"] = float(worst)
            print(f"{key:<16} {load:<17} against the circuit with the loudspeaker in it: {worst:.4f} dB (20 Hz - 20 kHz, worst stage rate)", flush=True)
    # each load's makeup: the median of its loudness change on the six tracks (linear: the drive does not change it)
    if len(sys.argv) > 1:
        import soundfile as sf
        import listening_set as LS
        tracks = [l.rstrip("\n").split("\t") for l in open(sys.argv[1], encoding="utf-8") if l.strip()]
        clips = []
        for label, path in tracks:
            info = sf.info(path)
            x, rate = sf.read(path, start=int(info.frames * 0.35), frames=int(40 * info.samplerate), always_2d=True)
            clips.append((x[:, :2], rate))
        for key, loads in data.items():
            for load, e in loads.items():
                ch = []
                for x, rate in clips:
                    n = len(x)
                    fx = np.fft.rfftfreq(n, 1.0 / rate)
                    hh = sections_response(e["sections"], np.maximum(fx, 1e-3), R0)
                    y = np.fft.irfft(np.fft.rfft(x, axis=0) * hh[:, None], n, axis=0)
                    ch.append(LS.lufs(x, rate) - LS.lufs(y, rate))
                e["makeup_db"] = float(np.median(ch))
                e["makeup_spread_db"] = [float(min(ch)), float(max(ch))]
                print(f"{key:<16} {load:<17} makeup {e['makeup_db']:+.2f} dB (the tracks {min(ch):+.2f} .. {max(ch):+.2f})", flush=True)
    json.dump(data, open(os.path.join(OUT, "loads.json"), "w"), indent=1)


if __name__ == "__main__":
    main()
