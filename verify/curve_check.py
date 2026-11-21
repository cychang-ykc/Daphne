"""M2 (N28): the exported curve against the circuit it came from. A 1 kHz sine through the static model
post(f(pre(u))) (u = amplitude / u1_input_V at drive 100 %) against Xyce harmonic balance on the full circuit:
H2 - H5 relative to the fundamental, at the power sweep's levels (data/ref/<preset>_sweep.csv) and along the drive
ladder (beyond the calibrated range: how far the static curve is from the circuit there).

  python curve_check.py set-300b
"""
import json, math, os, sys
from concurrent.futures import ProcessPoolExecutor

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import curve_export as E  # noqa: E402
import curve_ladder as L  # noqa: E402
import xyce_hb as X       # noqa: E402


def model_harmonics(d, amp_u, f_hz=1000.0, n=4096):
    """The static model's H2..H5 (dB re the fundamental) for a sine of amplitude amp_u (u units at drive 1)."""
    th = 2 * np.pi * np.arange(n) / n
    u = amp_u * np.cos(th)
    grid = np.linspace(-d["u_max"], d["u_max"], d["n"])
    y = np.interp(u, grid, np.asarray(d["f"]))
    Y = np.abs(np.fft.rfft(y))
    # the linear model's magnitude at the harmonics against the fundamental (the post sections; pre ~ flat here)
    f, h = E.response(d["preset"])
    lin = [abs(E.h_at(f, h, f_hz * k)) / abs(E.h_at(f, h, f_hz)) for k in range(1, 6)]
    return [20 * math.log10(max(Y[k] * lin[k - 1], 1e-300) / (Y[1] * lin[0])) for k in range(2, 6)]


def spice(args):
    b, amp = args
    r = X.measure(b, amp, 1000.0, 30)
    return amp, r["P"], list(r["H"][:4])


if __name__ == "__main__":
    b = sys.argv[1] if len(sys.argv) > 1 else "set-300b"
    d = json.load(open(os.path.join(E.OUT, f"{b}.json")))
    V1 = d["calibration"]["u1_input_V"]
    uc = d["calibration"]["u_calibrated"]
    sw = np.genfromtxt(os.path.join(E.REF, f"{b}_sweep.csv"), delimiter=",", names=True, dtype=None, encoding=None)
    rows = []
    amps = sorted({float(a) for a in sw["input_V_peak"]})
    rated = L.RATED_DRIVE[b]
    amps += [m * rated for m in (1.2, 1.5, 2.0, 3.0, 4.0, 5.0, 6.5)]
    with ProcessPoolExecutor(12) as ex:
        res = list(ex.map(spice, [(b, a) for a in amps]))
    worst_in = 0.0
    print(f"{'input V':>8} {'u':>6} {'P W':>8}   SPICE H2..H5 (dB)             model H2..H5 (dB)             max |diff| (H2-H5, H only where > -90 dB)")
    for amp, p, hs in res:
        u = amp / V1
        hm = model_harmonics(d, u)
        diffs = [abs(a - c) for a, c in zip(hs, hm) if a > -90]
        dmax = max(diffs) if diffs else 0.0
        inside = u <= uc + 1e-9
        if inside:
            worst_in = max(worst_in, dmax)
        rows.append({"amp": amp, "u": u, "P": p, "spice": hs, "model": hm, "max_diff": dmax, "calibrated": inside})
        print(f"{amp:8.3f} {u:6.3f} {p:8.3f}   " + " ".join(f"{x:7.1f}" for x in hs) + "   " + " ".join(f"{x:7.1f}" for x in hm)
              + f"   {dmax:5.2f}{'' if inside else '  (outside the calibrated range)'}")
    print(f"worst H2-H5 difference inside the calibrated range: {worst_in:.2f} dB")
    json.dump(rows, open(os.path.join(E.OUT, f"{b}_check.json"), "w"), indent=1)
