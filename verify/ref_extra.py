"""M2 (N28): the M0 reference data (reference_sweep.py's formats) for a preset added in M2 (set-300b-x): the
small-signal response (ngspice AC) and the 1 kHz power sweep (Xyce harmonic balance).

  python ref_extra.py set-300b-x 0.1 0.2 0.5 1 2 3 4.71
"""
import os, sys
from concurrent.futures import ProcessPoolExecutor

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import reference_sweep as RS    # noqa: E402
import set300b_variants as V    # noqa: E402


def sweep(args):
    b, p = args
    _, _, r = V.at_power((b, p))
    return p, r


if __name__ == "__main__":
    b = sys.argv[1]
    powers = [float(x) for x in sys.argv[2:]]
    r = RS.response_job(b)
    with open(os.path.join(RS.OUT, f"{b}_response.csv"), "w") as h:
        h.write("f_Hz,gain_dB_re_1kHz,phase_deg_re_1kHz\n")
        for fz, db, ph in zip(r["f"], r["db"], r["phase_deg"]):
            h.write(f"{fz:g},{db:.4f},{ph:.3f}\n")
    with ProcessPoolExecutor(8) as ex:
        sw = list(ex.map(sweep, [(b, p) for p in powers]))
    with open(os.path.join(RS.OUT, f"{b}_sweep.csv"), "w") as h:
        h.write("loop,f_Hz,P_W,input_V_peak,THD_pct,H2_dB,H3_dB,H4_dB,H5_dB,H6_dB,H7_dB\n")
        for p, x in sw:
            h.write(f",1000,{p:g},{x['amp']:.6g},{x['THD']:.5g}," + ",".join(f"{v:.2f}" for v in x["H"][:6]) + "\n")
            print(f"{b} {p:5g} W  in {x['amp']:.4f} V  THD {x['THD']:.3f} %  H2 {x['H'][0]:.1f}  H3 {x['H'][1]:.1f}")
    i10, i20k = int(np.argmin(np.abs(np.array(r["f"]) - 10))), int(np.argmin(np.abs(np.array(r["f"]) - 20000)))
    print(f"response: 10 Hz {r['db'][i10]:+.2f} dB, 20 kHz {r['db'][i20k]:+.2f} dB")
