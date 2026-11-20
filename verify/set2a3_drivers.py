"""set-2a3 with two-stage drivers (N28): the Triode Lab 2A3 EVO uses a pair of 12AX7 and a pair of 6SN7 ahead of the
2A3 (Stereophile specifications page). The 2A3 stays the Koren model; the 12AX7 is the ECC83 model.

Every variant: full circuit in Xyce harmonic balance, 1 kHz / 1 W / 8 ohm (the preset transformer) and 50 Hz / 1 W /
8 ohm (primary inductance x10, as the Stereophile comparison). Against the 2A3 EVO: 1 kHz 1 W 0.85 %; 50 Hz H2 -42 / -50,
H3 -56 / -55 dB. 16 parallel processes, one math thread each; every run timed.

  python set2a3_drivers.py
"""
import math, os, sys, time
for _v in ("OMP_NUM_THREADS", "OPENBLAS_NUM_THREADS", "MKL_NUM_THREADS"):
    os.environ.setdefault(_v, "1")
from concurrent.futures import ProcessPoolExecutor
import numpy as np

import spice_amps as A, spice_ref as R, xyce_hb as X


def ck(name, tube, g, a, k, b, bp, ra, rk, ck_uF):
    """Common-cathode stage: grid g, anode a, cathode k, supply node b."""
    net = [f"V{name}b {b} 0 DC {A.f(bp)}", f"R{name}a {b} {a} {A.f(ra)}", f"R{name}k {k} 0 {A.f(rk)}",
           R.triode(f"V{name}", tube, a, g, k), *A.tube_caps(f"V{name}", tube, a, g, k)]
    if ck_uF:
        net.append(f"C{name}k {k} 0 {A.f(ck_uF * 1e-6)}")
    return net


def output_stage(drive_node):
    return ["Cc2 " + drive_node + " g2 0.22e-6", "Rg2 g2 0 220k", "Rgs2 g2 g2s 1k", R.triode("V2", "2A3", "a2", "g2s", "k2"),
            *A.tube_caps("V2", "2A3", "a2", "g2s", "k2"), "Rk2 k2 0 750", "Ck2 k2 0 2.2e-4", "Vb2 b2 0 DC 301"] + \
        A.opt_se("T", "a2", "b2", 2500.0, 10.0, 0.010, 400e-12, 100.0)


def variant(v):
    net = ["* set-2a3 two-stage driver", "Vin in 0 SIN(0 {amp} {f0})", "Rin in 0 1Meg", "Rgs in g0 1k"]
    s1 = dict(ra=100e3, rk=1.5e3, ck=100.0, bp=300.0)
    s2 = dict(ra=47e3, rk=1.5e3, ck=100.0, bp=300.0)
    if v == "B": s1["ck"] = 0
    if v == "D": s2.update(ra=22e3, rk=680.0, bp=330.0)
    if v == "E": s1["ra"] = 220e3
    net += ck("D1", "ECC83", "g0", "a0", "k0", "b0", s1["bp"], s1["ra"], s1["rk"], s1["ck"])
    if v == "C":
        # direct-coupled cathode follower from the 12AX7 anode (about 150 V); cathode resistor to ground
        net += ["Vfb bf 0 DC 300", R.triode("VD3", "6SN7", "bf", "a0", "kf"), *A.tube_caps("VD3", "6SN7", "bf", "a0", "kf"),
                "Rkf kf 0 22k"]
        return net + output_stage("kf")
    net += ["Cc1 a0 g1 0.1e-6", "Rg1 g1 0 470k"]
    net += ck("D2", "6SN7", "g1", "a1", "k1", "b1", s2["bp"], s2["ra"], s2["rk"], s2["ck"])
    return net + output_stage("a1")


VARIANTS = {"A": "12AX7 CK (100 k, Rk 1.5 k bypassed) -> 6SN7 CK (47 k, 1.5 k bypassed)",
            "B": "as A, 12AX7 cathode unbypassed",
            "C": "12AX7 CK -> 6SN7 cathode follower (direct, 22 k) -> 2A3",
            "D": "as A, 6SN7 22 k / 680 ohm, B+ 330 V (more current)",
            "E": "as A, 12AX7 load 220 k",
            "now": "preset: 6SN7 CK only"}


def at_1w(net, f0, k_lp, times):
    orig = A.opt_se
    amp = 0.01
    for nf in (15, 15, 15, 15, 15, 30):
        t0 = time.time()
        a, _ = X.run_hb([l.replace("{amp}", A.f(amp)).replace("{f0}", A.f(f0)) for l in net], f0, nf)
        times.append(time.time() - t0)
        if nf == 15:
            amp *= math.sqrt(1.0 / (a[0] ** 2 / 16))
    return 100 * math.sqrt((a[1:] ** 2).sum()) / a[0], [float(x) for x in 20 * np.log10(a[1:5] / a[0])], amp


def job(v):
    times = []
    net = A.set_2a3() if v == "now" else variant(v)
    thd1, h1, amp1 = at_1w(net, 1000.0, 1, times)
    orig = A.opt_se
    A.opt_se = lambda n, a, bp, raa, lp, ll, cw, dcr: orig(n, a, bp, raa, lp * 10, ll, cw, dcr)
    net50 = A.set_2a3() if v == "now" else variant(v)
    A.opt_se = orig
    thd50, h50, _ = at_1w(net50, 50.0, 10, times)
    return v, thd1, h1, h50, amp1, float(np.mean(times)), len(times)


if __name__ == "__main__":
    t0 = time.time()
    with ProcessPoolExecutor(16) as ex:
        for v, thd1, h1, h50, amp, th, n in ex.map(job, list(VARIANTS)):
            print(f"{v:<4} {VARIANTS[v]:<62} 1 kHz THD {thd1:5.2f} % H2 {h1[0]:6.1f} H3 {h1[1]:6.1f} | 50 Hz H2 {h50[0]:6.1f} H3 {h50[1]:6.1f} "
                  f"| input {amp * 1e3 / math.sqrt(2):7.1f} mV rms | Xyce {n} x {th:.2f} s", flush=True)
    print(f"total {time.time() - t0:.1f} s")
