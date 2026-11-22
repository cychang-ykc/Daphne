"""M3 (N28): is an adjustable operating point worth it for set-300b? Nine circuits, bias cool / standard / hot x the
output transformer's tap 4 / 8 / 16 ohm (the loudspeaker stays 8 ohm: the tap changes the turns ratio, the primary
sees 3 k x 8 / tap), at the drive 100 % calibration level (a 1 kHz sine at -18 dBFS RMS: u = 0.178, the input
0.178 x u1_input_V); conservative alignment brings music to about this level.

Per circuit (ngspice, spice_amps.py's set-300b with the bias set by the cathode resistor, found by bisection on the
operating point; B+ as the reference):
  - the operating point (cathode current, plate-cathode voltage)
  - transient into an 8 ohm resistor (the engine's curve is the 8 ohm one; the load is linear after it): output
    power, H2 .. H7, THD
  - the output impedance at the loudspeaker terminals (AC injection) at 50 Hz / 1 kHz / 10 kHz
  - the response into the vented loudspeaker (speaker_load.speaker), against standard / 8 ohm's: the level at 1 kHz,
    and the shape relative to 1 kHz, 1/2-octave smoothed (as load_smooth.py)

  python m3_eval.py           prints the tables; writes data/curves/set-300b_m3_eval.json
"""
import json, math, os, re, subprocess, sys, tempfile
from concurrent.futures import ProcessPoolExecutor

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import spice_amps as A    # noqa: E402
import spice_ref as R     # noqa: E402
import speaker_load as SL # noqa: E402
import load_smooth as SM  # noqa: E402

CAL = json.load(open(os.path.join(HERE, "data", "curves", "set-300b.json")))["calibration"]
VIN = 10 ** (-18 / 20) * math.sqrt(2) * CAL["u1_input_V"]   # the drive 100 % calibration sine's peak at the input
BIAS = {"cool": 0.8 * 59.8, "standard": 59.8, "hot": min(1.1 * 59.8, 92.5)}   # mA (settings.cpp color_bias_range)
TAPS = (4, 8, 16)


def circuit(rk, tap, load_lines=None, ac_in=False, inject=False):
    net = ["* set-300b (m3_eval)"] + A.set_driver_6sn7(350.0, 47e3, 1.5e3, "g2", 220e3, 0.22e-6)   # (the first line: the title)
    net += ["Rgs2 g2 g2s 1k", R.triode("V2", "300B", "a2", "g2s", "k2"),
            *A.tube_caps("V2", "300B", "a2", "g2s", "k2"), f"Rk2 k2 0 {rk:.6g}", "Ck2 k2 0 2.2e-4", "Vb2 b2 0 DC 431"]
    raa, lp, ll, cw, dcr = 3000.0, 15.0, 0.012, 400e-12, 120.0
    k = math.sqrt(1 - ll / lp)
    ls = lp * tap / raa                         # the tap: 3 k : tap
    net += [f"RTp b2 Tx {dcr:.6g}", f"LTp Tx a2 {lp:.6g}", f"CTw a2 Tc {cw:.6g}", f"RTcw Tc b2 {A.OPT['rcw']:.6g}",
            f"RTcore a2 b2 {A.OPT['rc_ratio'] * raa:.6g}", f"LTs o Ty {ls:.6g}", f"RTs Ty 0 {tap * dcr / raa:.6g}",
            f"KT LTp LTs {k:.6g}"]
    if load_lines is not None:
        net += load_lines
    elif not inject:
        net.append("Rspk o 0 8")
    out = []
    for l in net:
        if l.startswith("Vin "):
            l = "Vin in 0 DC 0 AC 1" if ac_in else ("Vin in 0 DC 0 AC 0" if inject else l)
        out.append(l)
    if inject:
        out.append("Iinj 0 o DC 0 AC 1")
    return out


def op_current(rk, tap=8):
    """The 300B's cathode current (mA) at rk, from ngspice's operating point."""
    net = [l.replace("{amp}", "0").replace("{f0}", "1000") for l in circuit(rk, tap)]
    with tempfile.TemporaryDirectory() as td:
        cir = os.path.join(td, "op.cir")
        open(cir, "w").write("\n".join(net + [".options reltol=1e-6", ".control", "op", "print v(k2) v(a2)", "quit", ".endc", ".end", ""]))
        r = subprocess.run([R.NG, "-b", cir], capture_output=True, text=True, timeout=120)
    vk = float(re.search(r"v\(k2\)\s*=\s*([-\d.eE+]+)", r.stdout).group(1))
    va = float(re.search(r"v\(a2\)\s*=\s*([-\d.eE+]+)", r.stdout).group(1))
    return 1e3 * vk / rk, va - vk


def rk_for(ma):
    lo, hi = 300.0, 5000.0        # more resistance, less current
    for _ in range(40):
        mid = math.sqrt(lo * hi)
        i, _ = op_current(mid)
        lo, hi = (mid, hi) if i > ma else (lo, mid)
    return math.sqrt(lo * hi)


def evaluate(args):
    bias, ma, rk, tap = args
    i, vak = op_current(rk, tap)
    net = [l.replace("{amp}", f"{VIN:.6g}").replace("{f0}", "1000") for l in circuit(rk, tap)]
    a = A.run(net, ["v(o)"], 1000.0, 60)
    a1, h = A.harmonics(a[:, 0], a[:, 1], 1000.0)
    p = a1 ** 2 / 2 / 8.0
    thd = 100 * math.sqrt(sum(10 ** (x / 10) for x in h))
    zr = A.run(circuit(rk, tap, inject=True), ["vr(o)", "vi(o)"], 1000.0, 0, ac=(10.0, 40e3))
    zo = zr[:, 1] + 1j * zr[:, 3]
    lines, _ = SL.speaker("vented")
    rs = A.run(circuit(rk, tap, load_lines=lines, ac_in=True), ["vr(o)", "vi(o)"], 1000.0, 0, ac=(10.0, 40e3))
    hs = rs[:, 1] + 1j * rs[:, 3]
    return {"bias": bias, "bias_ma": ma, "rk": rk, "tap": tap, "ik_ma": i, "vak": vak, "P_W": p, "H_db": h.tolist(), "THD_pct": thd,
            "f": zr[:, 0].tolist(), "zout": np.abs(zo).tolist(), "speaker_re": hs.real.tolist(), "speaker_im": hs.imag.tolist(), "gain": a1 / VIN}


def main():
    rks = {b: rk_for(ma) for b, ma in BIAS.items()}
    print("cathode resistors: " + ", ".join(f"{b} {ma:.1f} mA -> {rks[b]:.0f} ohm" for b, ma in BIAS.items()), flush=True)
    cases = [(b, BIAS[b], rks[b], t) for b in BIAS for t in TAPS]
    with ProcessPoolExecutor(9) as ex:
        res = list(ex.map(evaluate, cases))
    std = next(r for r in res if r["bias"] == "standard" and r["tap"] == 8)
    f = np.array(std["f"])
    hstd = np.array(std["speaker_re"]) + 1j * np.array(std["speaker_im"])
    fw = np.logspace(math.log10(10), math.log10(40000), 800)
    probe = (30, 50, 100, 250, 1000, 3000, 10000, 20000)
    print(f"\ninput {VIN:.3f} V peak (a 1 kHz sine at -18 dBFS RMS, drive 100 %); 8 ohm resistor for the harmonics")
    print(f"{'bias':<9}{'tap':>4}{'Ik mA':>7}{'Vak':>6}{'P W':>7}{'H2':>7}{'H3':>7}{'H4':>7}{'H5':>7}{'THD %':>7}"
          f"{'Zout 50':>8}{'1k':>6}{'10k':>6}{'level':>7}  shape re standard / 8 ohm, re 1 kHz (" + " ".join(str(p) for p in probe) + ")")
    for r in res:
        h = np.array(r["speaker_re"]) + 1j * np.array(r["speaker_im"])
        ratio = h / hstd
        rr = np.interp(np.log(fw), np.log(f), ratio.real) + 1j * np.interp(np.log(fw), np.log(f), ratio.imag)
        sm = SM.smoothed_db(fw, rr)
        sm -= np.interp(math.log(1000), np.log(fw), sm)
        lvl = 20 * math.log10(abs(np.interp(1000, f, h.real) + 1j * np.interp(1000, f, h.imag)) /
                              abs(np.interp(1000, f, hstd.real) + 1j * np.interp(1000, f, hstd.imag)))
        r["level_db"] = lvl
        r["shape_db"] = {str(p): float(np.interp(math.log(p), np.log(fw), sm)) for p in probe}
        zo = np.array(r["zout"])
        r["zout_at"] = {str(p): float(np.interp(p, f, zo)) for p in (50, 1000, 10000)}
        H = r["H_db"]
        print(f"{r['bias']:<9}{r['tap']:>4}{r['ik_ma']:7.1f}{r['vak']:6.0f}{r['P_W']:7.3f}{H[0]:7.1f}{H[1]:7.1f}{H[2]:7.1f}{H[3]:7.1f}{r['THD_pct']:7.2f}"
              f"{r['zout_at']['50']:8.2f}{r['zout_at']['1000']:6.2f}{r['zout_at']['10000']:6.2f}{lvl:+7.2f}  " +
              " ".join(f"{r['shape_db'][str(p)]:+.2f}" for p in probe), flush=True)
    for r in res:
        for k in ("f", "zout", "speaker_re", "speaker_im"):
            r.pop(k)
    json.dump({"input_V": VIN, "results": res}, open(os.path.join(HERE, "data", "curves", "set-300b_m3_eval.json"), "w"), indent=1)


if __name__ == "__main__":
    main()
