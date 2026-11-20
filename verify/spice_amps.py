"""The four presets' reference amplifiers in ngspice: drivers, output stage, output transformer (linear for now),
global feedback (pp-el34-ul). Tube models and conventions as in spice_ref.py (model.json parameters, B sources,
reltol 1e-5, gear, 1000 points per cycle, FFT over the last 10 whole cycles after settling).

  set-2a3   12AX7 (B+ 300 V, Ra 100 k, Rk 1.5 k) -> 0.1 uF / 470 k -> 6SN7 (B+ 300 V, Ra 47 k, Rk 1.5 k) -> 0.22 uF / 220 k
            -> 2A3 (Koren with m = 0.35, Rk 750, B+ 301 V) -> 2.5 k : 8
  set-300b  6SN7 (B+ 350 V) -> 0.22 uF / 220 k -> 300B (350 V / -74 V / 60 mA: Rk 1.23 k, B+ 431 V) -> 3 k : 8
  set-845   6SN7 (B+ 350 V) -> 0.22 uF / 220 k -> 300B (40 H choke, DCR 250, Rk 1 k, B+ 300 V) -> 0.47 uF / 100 k
            -> 845 (1250 V / -195 V / 80 mA: fixed bias, B+ 1286 V) -> 11 k : 8
  pp-el34-ul  Mullard 5-20 (Circuits for Audio Amplifiers, 1959, ch. 5, Fig. 1): EF86 -> ECC83 cathode-coupled
            phase splitter -> 2 x EL34 ultralinear (20 % taps, 7 k a-a, 470 ohm / valve, 1 k screen resistors),
            30 dB feedback from the secondary (R13 5.6 k // C9 330 pF) into R5 (100 ohm)

Output transformers (assumed values, N28): primary inductance Lp, leakage Ll (coupling k = sqrt(1 - Ll / Lp)),
winding capacitance across the primary with a series loss resistor, core-loss resistor across the primary, primary
DCR (distributed along the 5-20's half primaries by turns), secondary DCR for equal copper loss; 8 ohm load.
The power sweeps run in Xyce harmonic balance (xyce_hb.py); this module's transient is for cross-checks.

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


# Output transformer losses (assumed, N28), the same rules for all four presets:
#   copper: primary DCR per preset; secondary DCR chosen for equal copper loss in both windings (the usual design
#     balance), i.e. r_sec / R_load = r_pri / R_primary
#   core: a resistor across the primary, RC_RATIO x the primary impedance (eddy-current loss at constant voltage is
#     frequency independent; RC_RATIO 50 = core loss 2 % of the output power)
#   winding capacitance: a series resistor RCW (dielectric loss and the resistance of the distributed winding)
#   R_LOAD: the loudspeaker (1e9 = open circuit)
OPT = {"rc_ratio": 50.0, "rcw": 200.0, "r_load": 8.0}   # rcw 200: provisional (N28, high-frequency stability)


def opt_se(name, a, bp, raa, lp, ll, cw, dcr):
    """Single-ended output transformer: primary a .. bp (via DCR), secondary o .. 0 into the 8 ohm loudspeaker."""
    k = math.sqrt(1 - ll / lp)
    ls = lp * 8.0 / raa
    return [f"R{name}p {bp} {name}x {f(dcr)}", f"L{name}p {name}x {a} {f(lp)}",
            f"C{name}w {a} {name}c {f(cw)}", f"R{name}cw {name}c {bp} {f(max(OPT['rcw'], 1e-3))}",
            f"R{name}core {a} {bp} {f(OPT['rc_ratio'] * raa)}",
            f"L{name}s o {name}y {f(ls)}", f"R{name}s {name}y 0 {f(8.0 * dcr / raa)}", f"K{name} L{name}p L{name}s {f(k)}",
            f"Rspk o 0 {f(OPT['r_load'])}"]


# ---- interelectrode capacitances ---------------------------------------------------------------------------
# Direct interelectrode capacitances from the datasheet front pages (pF): grid-plate, grid-cathode (input; Philips
# Cg1 is the grid to all electrodes except the anode), plate-cathode (output). "<" values taken at the limit.
#   6SN7 RCA HB-3 1954 unit 1; 300B WE AB46.300B 1950 p2; 2A3 RCA 1947; 845 RCA 1940; ECC83 Philips 1954 (Ca 0.46,
#   system 1); EF86 Philips 1956; EL34 Philips 1958
CAPS = {"6SN7": (4.0, 2.2, 0.7), "300B": (15.0, 8.5, 4.1), "2A3": (16.5, 7.5, 5.5), "845": (13.5, 6.0, 6.5),
        "ECC83": (1.7, 1.6, 0.46), "EF86": (0.05, 3.8, 5.3), "EL34": (1.1, 15.2, 8.4)}


def tube_caps(name, tube, a, g, k):
    cgp, cgk, cpk = CAPS[tube]
    return [f"C{name}gp {g} {a} {cgp}p", f"C{name}gk {g} {k} {cgk}p", f"C{name}pk {a} {k} {cpk}p"]


# ---- the presets -------------------------------------------------------------------------------------------

def set_driver_6sn7(bp, ra, rk, out_node, rg_next, cc):
    return [f"Vin in 0 SIN(0 {{amp}} {{f0}})", "Rin in 0 1Meg", "Rgs in g1 1k",
            f"Vb1 b1 0 DC {f(bp)}", f"Ra1 b1 a1 {f(ra)}", f"Rk1 k1 0 {f(rk)}", "Ck1 k1 0 1e-4",
            R.triode("V1", "6SN7", "a1", "g1", "k1"), *tube_caps("V1", "6SN7", "a1", "g1", "k1"),
            f"Cc1 a1 {out_node} {f(cc)}", f"Rg2 {out_node} 0 {f(rg_next)}"]


def set_2a3():
    """12AX7 -> 6SN7 -> 2A3: the 2A3 EVO's driver line-up (a pair of 12AX7 and a pair of 6SN7, Stereophile
    specifications page); N28."""
    net = ["* set-2a3", "Vin in 0 SIN(0 {amp} {f0})", "Rin in 0 1Meg", "Rgs in g0 1k",
           "Vb0 b0 0 DC 300", "Ra0 b0 a0 100k", "Rk0 k0 0 1.5k", "Ck0 k0 0 1e-4",
           R.triode("V0", "ECC83", "a0", "g0", "k0"), *tube_caps("V0", "ECC83", "a0", "g0", "k0"),
           "Cc0 a0 g1 0.1e-6", "Rg1 g1 0 470k",
           "Vb1 b1 0 DC 300", "Ra1 b1 a1 47k", "Rk1 k1 0 1.5k", "Ck1 k1 0 1e-4",
           R.triode("V1", "6SN7", "a1", "g1", "k1"), *tube_caps("V1", "6SN7", "a1", "g1", "k1"),
           "Cc1 a1 g2 0.22e-6", "Rg2 g2 0 220k"]
    net += ["Rgs2 g2 g2s 1k", R.triode("V2", "2A3", "a2", "g2s", "k2"),
            *tube_caps("V2", "2A3", "a2", "g2s", "k2"), "Rk2 k2 0 750", "Ck2 k2 0 2.2e-4",
            "Vb2 b2 0 DC 301"] + opt_se("T", "a2", "b2", 2500.0, 10.0, 0.010, 400e-12, 100.0)
    return net


def set_300b():
    net = ["* set-300b"] + set_driver_6sn7(350.0, 47e3, 1.5e3, "g2", 220e3, 0.22e-6)
    net += ["Rgs2 g2 g2s 1k", R.triode("V2", "300B", "a2", "g2s", "k2"),
            *tube_caps("V2", "300B", "a2", "g2s", "k2"), "Rk2 k2 0 1233", "Ck2 k2 0 2.2e-4",
            "Vb2 b2 0 DC 431"] + opt_se("T", "a2", "b2", 3000.0, 15.0, 0.012, 400e-12, 120.0)
    return net


def set_845():
    net = ["* set-845"] + set_driver_6sn7(350.0, 47e3, 1.5e3, "g2", 220e3, 0.22e-6)
    net += ["Rgs2 g2 g2s 1k", R.triode("V2", "300B", "a2", "g2s", "k2"),
            *tube_caps("V2", "300B", "a2", "g2s", "k2"), "Rk2 k2 0 1000", "Ck2 k2 0 2.2e-4",
            "Vb2 b2 0 DC 300", "Rch b2 chx 250", "Lch chx a2 40",
            "Cc2 a2 g3 0.47e-6", "Rg3 g3 nb 100k", "Vnb nb 0 DC -195", "Rgs3 g3 g3s 1k",
            R.triode("V3", "845", "a3", "g3s", "0"), *tube_caps("V3", "845", "a3", "g3s", "0"), "Vb3 b3 0 DC 1286"] + opt_se("T", "a3", "b3", 11000.0, 30.0, 0.020, 300e-12, 450.0)
    return net


def pp_520(feedback=True, ef86_pt=(0.2, 250.0, 140.0, -2.0), ll=0.005, cw=500e-12, drive=None, el34_cutoff_c=0.0,
           r13=5.6e3, c9=330e-12):
    """Mullard 5-20, Fig. 1 values. Feedback R13 5.6 k // C9 330 pF from the secondary to R5 (open loop: the
    feedback resistor goes to ground instead, keeping R5's loading).
    drive (open loop, diagnostics): "splitter" = ideal antiphase sources in place of the ECC83 anodes (DC 307.8 V,
    the coupling capacitors, 470 k grid resistors and 2.2 k stoppers kept); "grid" = ideal sources straight on the
    EL34 grids. {amp} is then the amplitude at each anode / grid.
    el34_cutoff_c: the EL34 sensitivity test only (spice_ref.pentode cutoff_c), never in a preset.
    r13, c9: the feedback network (the book: 5.6 k // 330 pF for 7.5 ohm); the feedback steps of N28 scale them."""
    lp, dcr, tap = 100.0, 170.0, 0.20
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
           *tube_caps("V1", "EF86", "a1", "g1", "k1"),
           # ECC83 cathode-coupled phase splitter, grid of V2A direct from the EF86 anode
           "R11 c5 a2a 180k", "R12 c5 a2b 180k", "R8 kk 0 82k", "R10 a1 g2b 1Meg", "C6 g2b 0 0.25e-6",
           R.triode("V2A", "ECC83", "a2a", "a1", "kk"), R.triode("V2B", "ECC83", "a2b", "g2b", "kk"),
           *tube_caps("V2A", "ECC83", "a2a", "a1", "kk"), *tube_caps("V2B", "ECC83", "a2b", "g2b", "kk"),
           # EL34s
           "C10 a2a g3 0.5e-6", "R14 g3 0 470k", "R18 g3 g3s 2.2k",
           "C11 a2b g4 0.5e-6", "R15 g4 0 470k", "R19 g4 g4s 2.2k",
           "R20 k3 0 470", "C13 k3 0 50e-6", "R21 k4 0 470", "C14 k4 0 50e-6",
           "R24 t3 s3 1k", "R25 t4 s4 1k",
           *R.pentode("V3", "EL34", "a3", "s3", "g3s", "k3", screen="fit", cutoff_c=el34_cutoff_c),
           *R.pentode("V4", "EL34", "a4", "s4", "g4s", "k4", screen="fit", cutoff_c=el34_cutoff_c),
           *tube_caps("V3", "EL34", "a3", "g3s", "k3"), *tube_caps("V4", "EL34", "a4", "g4s", "k4"),
           # output transformer: centre tap c12 via DCR, taps t3 / t4, anodes a3 / a4
           # the DCR is distributed along each half primary: tap x DCR between the centre tap and the screen tap,
           # the rest between the screen tap and the anode (lumping it at the centre tap would starve the screens)
           f"Rd3 c12 ct3 {f(tap * dcr)}", f"Rd4 c12 ct4 {f(tap * dcr)}",
           f"L1 ct3 t3 {f(l1)}", f"L2 t3 a3x {f(l2)}", f"Rd3b a3x a3 {f((1 - tap) * dcr)}",
           f"L3 ct4 t4 {f(l1)}", f"L4 t4 a4x {f(l2)}", f"Rd4b a4x a4 {f((1 - tap) * dcr)}",
           f"L5 o sy {f(ls)}", f"Rsy sy 0 {f(8.0 * 2 * dcr / 7000.0)}", f"Rspk o 0 {f(OPT['r_load'])}",
           f"Cw a3 cwx {f(cw)}", f"Rcw cwx a4 {f(max(OPT['rcw'], 1e-3))}", f"Rcore a3 a4 {f(OPT['rc_ratio'] * 7000.0)}"]
    net += [f"K{a}{b} L{a} L{b} {f(v)}" for (a, b), v in K.items()]
    # wiring capacitance at the nodes the tube capacitances leave floating (screens, the splitter's cathodes); the
    # transient cannot start at a node with no capacitance next to the B sources
    net += ["Cw1 s1 0 5p", "Cw2 s3 0 5p", "Cw3 s4 0 5p", "Cw4 kk 0 5p"]
    net += [f"R13 o fb {f(r13)}", f"C9 o fb {f(c9)}"] if feedback else [f"R13 fbx fb {f(r13)}", "Rfbx fbx 0 1e-3"]
    if drive:
        front = ("Vin ", "R1 ", "R2 ", "R6 ", "R3 ", "C1 ", "R7 ", "C2 ", "R4 ", "C3 ", "R5 ", "BV1", "CV1", "R11 ",
                 "R12 ", "R8 ", "R10 ", "C6 ", "BV2A", "BV2B", "CV2A", "CV2B", "Cw1 ", "Cw4 ", "R13 ", "C9 ", "Rfbx")
        if drive == "grid":
            front += ("C10 ", "R14 ", "R18 ", "C11 ", "R15 ", "R19 ")
            src = ["Vd3 g3s 0 SIN(0 {amp} {f0})", "Vd4 g4s 0 SIN(0 {amp} {f0} 0 0 180)"]
        else:
            src = ["Vd3 a2a 0 SIN(307.8 {amp} {f0})", "Vd4 a2b 0 SIN(307.8 {amp} {f0} 0 0 180)"]
        net = [l for l in net if not l.startswith(front)] + src
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
