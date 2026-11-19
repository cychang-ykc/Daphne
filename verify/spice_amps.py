"""The four presets' reference amplifiers in ngspice: drivers, output stage, output transformer (linear for now),
global feedback (pp-el34-ul). Tube models and conventions as in spice_ref.py (model.json parameters, B sources,
reltol 1e-5, gear, 1000 points per cycle, FFT over the last 10 whole cycles after settling).

  set-2a3   6SN7 (B+ 300 V, Ra 47 k, Rk 1.5 k) -> 0.22 uF / 220 k -> 2A3 (Rk 750, B+ 301 V) -> 2.5 k : 8
  set-300b  6SN7 (B+ 350 V) -> 0.22 uF / 220 k -> 300B (Rk 1.2 k, B+ 433 V) -> 4 k : 8
  set-845   6SN7 (B+ 350 V) -> 0.22 uF / 220 k -> 300B (40 H choke, DCR 250, Rk 1 k, B+ 300 V) -> 0.47 uF / 100 k
            -> 845 (fixed bias -145 V, B+ 1020 V) -> 6 k : 8
  pp-el34-ul  Mullard 5-20 (Circuits for Audio Amplifiers, 1959, ch. 5, Fig. 1): EF86 -> ECC83 cathode-coupled
            phase splitter -> 2 x EL34 ultralinear (20 % taps, 7 k a-a, 470 ohm / valve, 1 k screen resistors),
            30 dB feedback from the secondary (R13 5.6 k // C9 330 pF) into R5 (100 ohm)

Output transformers (assumed values, N28): primary inductance Lp, leakage Ll (coupling k = sqrt(1 - Ll / Lp)),
winding capacitance across the primary, primary DCR; secondary 8 ohm load, 0.3 ohm DCR.

  python spice_amps.py sweep          power sweeps of the four presets (parallel)
  python spice_amps.py 520            the 5-20 checks: open loop 1 / 10 / 20 W, closed loop, response
"""
import math, os, subprocess, sys, tempfile
from concurrent.futures import ProcessPoolExecutor
import numpy as np

import spice_ref as R

f = R.f
TIMEOUT = 600


def run(net, probes, f0, cycles_settle, step_per_cycle=1000, ac=None):
    """Transient: settle for cycles_settle cycles, then 10 more; returns time and the probes."""
    t_stop = (cycles_settle + 10) / f0
    step = 1 / f0 / step_per_cycle
    with tempfile.TemporaryDirectory() as td:
        out = os.path.join(td, "out.txt")
        cir = os.path.join(td, "amp.cir")
        ctl = [f"tran {step:g} {t_stop:g} 0 {step:g}", f"wrdata {out} " + " ".join(probes)]
        if ac:
            ctl = [f"ac dec 20 {ac[0]:g} {ac[1]:g}", f"wrdata {out} " + " ".join(probes)]
        lines = net + [f".options reltol={R.RELTOL:g} method=gear", ".control"] + ctl + ["quit", ".endc", ".end", ""]
        with open(cir, "w") as h:
            h.write("\n".join(lines))
        try:
            r = subprocess.run([R.NG, "-b", cir], capture_output=True, text=True, timeout=TIMEOUT)
        except subprocess.TimeoutExpired:
            raise RuntimeError(f"ngspice did not finish within {TIMEOUT} s")
        if not os.path.exists(out):
            raise RuntimeError(r.stdout[-2000:] + r.stderr[-2000:])
        a = np.loadtxt(out, ndmin=2)
        if not ac and a[-1, 0] < 0.99 * t_stop:
            raise RuntimeError(f"transient stopped at {a[-1, 0]:.3g} s")
    return a


def harmonics(t, v, f0, cycles=10):
    n = 1000
    tt = np.linspace(t[-1] - cycles / f0, t[-1], cycles * n, endpoint=False)
    y = np.interp(tt, t, v)
    y = y - y.mean()
    s = np.fft.rfft(y) / len(y) * 2
    a = np.abs(s[[cycles * h for h in range(1, 8)]])
    return a[0], 20 * np.log10(np.maximum(a[1:], 1e-300) / a[0])


def opt_se(name, a, bp, raa, lp, ll, cw, dcr):
    """Single-ended output transformer: primary a .. bp (via DCR), secondary o .. 0 into 8 ohm."""
    k = math.sqrt(1 - ll / lp)
    ls = lp * 8.0 / raa
    return [f"R{name}p {bp} {name}x {f(dcr)}", f"L{name}p {name}x {a} {f(lp)}", f"C{name}w {a} {bp} {f(cw)}",
            f"L{name}s o {name}y {f(ls)}", f"R{name}s {name}y 0 0.3", f"K{name} L{name}p L{name}s {f(k)}",
            "Rspk o 0 8"]


# ---- the presets -------------------------------------------------------------------------------------------

def set_driver_6sn7(bp, ra, rk, out_node, rg_next, cc):
    return [f"Vin in 0 SIN(0 {{amp}} {{f0}})", "Rin in 0 1Meg", "Rgs in g1 1k",
            f"Vb1 b1 0 DC {f(bp)}", f"Ra1 b1 a1 {f(ra)}", f"Rk1 k1 0 {f(rk)}", "Ck1 k1 0 1e-4",
            R.triode("V1", "6SN7", "a1", "g1", "k1"),
            f"Cc1 a1 {out_node} {f(cc)}", f"Rg2 {out_node} 0 {f(rg_next)}"]


def set_2a3():
    net = ["* set-2a3"] + set_driver_6sn7(300.0, 47e3, 1.5e3, "g2", 220e3, 0.22e-6)
    net += ["Rgs2 g2 g2s 1k", R.triode("V2", "2A3", "a2", "g2s", "k2"), "Rk2 k2 0 750", "Ck2 k2 0 2.2e-4",
            "Vb2 b2 0 DC 301"] + opt_se("T", "a2", "b2", 2500.0, 10.0, 0.010, 400e-12, 100.0)
    return net


def set_300b():
    net = ["* set-300b"] + set_driver_6sn7(350.0, 47e3, 1.5e3, "g2", 220e3, 0.22e-6)
    net += ["Rgs2 g2 g2s 1k", R.triode("V2", "300B", "a2", "g2s", "k2"), "Rk2 k2 0 1200", "Ck2 k2 0 2.2e-4",
            "Vb2 b2 0 DC 433"] + opt_se("T", "a2", "b2", 4000.0, 15.0, 0.012, 400e-12, 150.0)
    return net


def set_845():
    net = ["* set-845"] + set_driver_6sn7(350.0, 47e3, 1.5e3, "g2", 220e3, 0.22e-6)
    net += ["Rgs2 g2 g2s 1k", R.triode("V2", "300B", "a2", "g2s", "k2"), "Rk2 k2 0 1000", "Ck2 k2 0 2.2e-4",
            "Vb2 b2 0 DC 300", "Rch b2 chx 250", "Lch chx a2 40",
            "Cc2 a2 g3 0.47e-6", "Rg3 g3 nb 100k", "Vnb nb 0 DC -145", "Rgs3 g3 g3s 1k",
            R.triode("V3", "845", "a3", "g3s", "0"), "Vb3 b3 0 DC 1020"] + opt_se("T", "a3", "b3", 6000.0, 30.0, 0.020, 300e-12, 250.0)
    return net


def pp_520(feedback=True, ef86_pt=(0.2, 250.0, 140.0, -2.0)):
    """Mullard 5-20, Fig. 1 values. Feedback R13 5.6 k // C9 330 pF from the secondary to R5 (open loop: the
    feedback resistor goes to ground instead, keeping R5's loading)."""
    lp, ll, cw, dcr, tap = 100.0, 0.015, 300e-12, 170.0, 0.20
    lh = lp / 4
    l1, l2 = lh * tap ** 2, lh * (1 - tap) ** 2
    ls = lp * 8.0 / 7000.0
    kps = math.sqrt(1 - ll / lp)                   # primary to secondary (leakage)
    kpp = 0.99999                                  # between the primary sections
    K = {("1", "2"): kpp, ("1", "3"): -kpp, ("1", "4"): -kpp, ("2", "3"): -kpp, ("2", "4"): -kpp, ("3", "4"): kpp,
         ("1", "5"): kps, ("2", "5"): kps, ("3", "5"): -kps, ("4", "5"): -kps}
    net = ["* pp-el34-ul (Mullard 5-20)",
           "Vin in 0 SIN(0 {amp} {f0})", "R1 in 0 1Meg", "R2 in g1 4.7k",
           # supply: C12 node 440 V (ideal), R17 15 k -> C5 (8 uF), R9 270 k -> C4 (8 uF)
           "Vht c12 0 DC 440", "R17 c12 c5 15k", "C5 c5 0 8e-6", "R9 c5 c4 270k", "C4 c4 0 8e-6",
           # EF86
           "R6 c4 a1 100k", "R3 c4 r3 4.7k", "C1 r3 a1 47e-12", "R7 c4 s1 390k", "C2 s1 k1 0.05e-6",
           "R4 k1 fb 2.2k", "C3 k1 fb 50e-6", "R5 fb 0 100",
           *R.pentode("V1", "EF86", "a1", "s1", "g1", "k1", screen="derk", screen_pt=ef86_pt),
           # ECC83 cathode-coupled phase splitter, grid of V2A direct from the EF86 anode
           "R11 c5 a2a 180k", "R12 c5 a2b 180k", "R8 kk 0 82k", "R10 a1 g2b 1Meg", "C6 g2b 0 0.25e-6",
           R.triode("V2A", "ECC83", "a2a", "a1", "kk"), R.triode("V2B", "ECC83", "a2b", "g2b", "kk"),
           # EL34s
           "C10 a2a g3 0.5e-6", "R14 g3 0 470k", "R18 g3 g3s 2.2k",
           "C11 a2b g4 0.5e-6", "R15 g4 0 470k", "R19 g4 g4s 2.2k",
           "R20 k3 0 470", "C13 k3 0 50e-6", "R21 k4 0 470", "C14 k4 0 50e-6",
           "R24 t3 s3 1k", "R25 t4 s4 1k",
           *R.pentode("V3", "EL34", "a3", "s3", "g3s", "k3", screen="fit"),
           *R.pentode("V4", "EL34", "a4", "s4", "g4s", "k4", screen="fit"),
           # output transformer: centre tap c12 via DCR, taps t3 / t4, anodes a3 / a4
           f"Rd3 c12 ct3 {f(dcr)}", f"Rd4 c12 ct4 {f(dcr)}",
           f"L1 ct3 t3 {f(l1)}", f"L2 t3 a3 {f(l2)}", f"L3 ct4 t4 {f(l1)}", f"L4 t4 a4 {f(l2)}",
           f"L5 o sy {f(ls)}", "Rsy sy 0 0.3", "Rspk o 0 8", f"Cw a3 a4 {f(cw)}"]
    net += [f"K{a}{b} L{a} L{b} {f(v)}" for (a, b), v in K.items()]
    # interelectrode and wiring capacitance (datasheets: EL34 Cin 15.2 pF, Cout 8.4 pF; EF86 Cg1 3.8 pF, Ca 5.3 pF;
    # a few pF of wiring elsewhere). Physical, and without them the transient cannot start (nodes with no
    # capacitance at all next to the B sources)
    net += ["Cs1 a1 0 5p", "Cs2 a2a 0 5p", "Cs3 a2b 0 5p", "Cs4 g3s 0 15p", "Cs5 g4s 0 15p", "Cs6 a3 0 8.4p",
            "Cs7 a4 0 8.4p", "Cs8 s1 0 5p", "Cs9 s3 0 10p", "Cs10 s4 0 10p", "Cs11 kk 0 5p", "Cs12 g1 0 4p",
            "Cs13 g2b 0 5p"]
    net += ["R13 o fb 5.6k", "C9 o fb 330e-12"] if feedback else ["R13 fbx fb 5.6k", "Rfbx fbx 0 1e-3"]
    return net


PRESETS = {"set-2a3": set_2a3, "set-300b": set_300b, "set-845": set_845}


def measure(args):
    """(label, netlist builder name, kwargs, amplitude, f0, settle cycles) -> result line."""
    label, builder, kw, amp, f0, settle = args
    net = (pp_520(**kw) if builder == "pp-el34-ul" else PRESETS[builder]())
    net = [l.replace("{amp}", f(amp)).replace("{f0}", f(f0)) for l in net]
    try:
        a = run(net, ["v(o)"], f0, settle)
    except RuntimeError as e:
        return {"label": label, "error": str(e).splitlines()[0]}
    a1, h = harmonics(a[:, 0], a[:, 1], f0)
    p = a1 ** 2 / 2 / 8.0
    thd = 100 * math.sqrt(sum(10 ** (x / 10) for x in h))
    return {"label": label, "amp": amp, "P": p, "H": h, "THD": thd, "gain": a1 / amp}


def show(r):
    if "error" in r:
        print(f"  {r['label']:<40} ERROR {r['error']}")
        return
    hs = " ".join(f"H{i + 2} {v:6.1f}" for i, v in enumerate(r["H"][:6]))
    print(f"  {r['label']:<40} in {r['amp']:8.4f} V  P {r['P']:7.3f} W  THD {r['THD']:7.4f} %  {hs}", flush=True)


def sweep(cases, workers=12):
    with ProcessPoolExecutor(workers) as ex:
        return list(ex.map(measure, cases))
