"""Virtual loudspeaker load (N28, evaluation for a later milestone): the presets' output impedance from the M0
reference circuits, and what a real loudspeaker's impedance does to their frequency response compared with the
8 ohm resistor of the reference data.

Small-signal (ngspice AC at the operating point): the input source is set to 0, the 8 ohm load resistor removed and
a 1 A AC current injected into the secondary (node o): V(o) is the output impedance Zout(f), feedback included
(pp-el34-ul). The amplifier is then its Thevenin equivalent, and a load Z changes the response by
    |Z / (Z + Zout)| / |8 / (8 + Zout)|
(checked against a direct AC run with the loudspeaker network in the circuit).

  python speaker_load.py            Zout, damping factor, the response change for the loudspeaker models
  python speaker_load.py --headphones   HD800S against a resistor, the presets and an OTL amplifier (after the above)
"""
import json
import math
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import spice_amps as A  # noqa: E402

OUT = os.path.join(HERE, "data", "speaker")
FB = json.load(open(os.path.join(HERE, "data", "ref", "pp_feedback.json"), encoding="utf-8"))["steps"]


def netlist(preset, fb_db=6):
    if preset == "pp-el34-ul":
        st = next(s for s in FB if s["F"] == fb_db)
        kw = dict(feedback=False) if fb_db == 0 else dict(feedback=True, r13=st["r13"], c9=st["c9"])
        return A.pp_520(**kw)
    return A.PRESETS[preset]()


def ac_run(net, f_lo=10.0, f_hi=100e3):
    a = A.run(net, ["vr(o)", "vi(o)"], 1000.0, 0, ac=(f_lo, f_hi))
    return a[:, 0], a[:, 1] + 1j * a[:, 3]


def zout(preset, fb_db=6):
    net = []
    for l in netlist(preset, fb_db):
        if l.startswith("Vin "):
            l = "Vin in 0 DC 0 AC 0"
        if l.startswith("Rspk "):
            continue
        net.append(l)
    net.append("Iinj 0 o DC 0 AC 1")
    return ac_run(net)


def response(preset, load_lines, fb_db=6):
    """Direct AC with a load network between o and 0 in place of the 8 ohm resistor (the cross-check)."""
    net = []
    for l in netlist(preset, fb_db):
        if l.startswith("Vin "):
            l = "Vin in 0 DC 0 AC 1"
        if l.startswith("Rspk "):
            net.extend(load_lines)
            continue
        net.append(l)
    return ac_run(net)


RHO_C2 = 1.18 * 343.0 ** 2   # air: rho c^2 (Pa), for the box compliance


def speaker(kind="vented"):
    """A typical two-way stand-mount loudspeaker as an electrical network (the impedance a real one presents; the
    parameters are assumptions, its features checked against published measurements in N28):
      woofer 6.5 in: Re 3.4 ohm, Le 0.9 mH (with a 4 ohm eddy-current loss across it), Bl 6.8 T m, Mms 15 g,
      Cms 1.0 mm/N, Rms 2.8 kg/s, Sd 137 cm2;
      vented: 12 l box tuned to 50 Hz (port losses Ql 10); sealed: 9 l box
      crossover 2.5 kHz, second order Linkwitz-Riley for 8 ohm (L 1.02 mH, C 3.98 uF); tweeter Re 3.6 ohm + 0.8 ohm
      padding, Le 0.05 mH, fs 900 Hz (Qms 2, Qes 1.2).
    Vented: peaks 24 Hz 15 ohm and 85 Hz 19 ohm, the port's saddle 49 Hz 4.1 ohm, minimum 4.1 ohm at 221 Hz, 22 ohm
    near the crossover (the LR2 network's own resonance: a model assumption), 4.2 ohm at 10 kHz, 5.5 ohm at 20 kHz,
    phase -45 .. +71 degrees. Sealed: one peak, 80 Hz 20 ohm.
    Returns (spice lines between nodes o and 0, a function f -> complex Z)."""
    Re, Le, Rle, Bl, Mms, Cms, Rms, Sd = 3.4, 0.9e-3, 4.0, 6.8, 0.015, 1.0e-3, 2.8, 0.0137
    Vb = 0.012 if kind == "vented" else 0.009
    Cab = Vb / RHO_C2
    fb, Ql = 50.0, 10.0
    Map = 1.0 / ((2 * math.pi * fb) ** 2 * Cab)
    Rap = 1.0 / (2 * math.pi * fb * Cab * Ql)        # port and leakage losses as a series resistance
    # motional branches (mobility analogy): parallel C = Mms/Bl^2, L = Cms Bl^2, R = Bl^2/Rms; the box (and port)
    # reflected as L_box = Cab Bl^2 / Sd^2 in series with (C_port = Map Sd^2 / Bl^2 || R_port = Bl^2 / (Sd^2 Rap))
    Cmes, Lces, Res = Mms / Bl ** 2, Cms * Bl ** 2, Bl ** 2 / Rms
    Lb = Cab * Bl ** 2 / Sd ** 2
    Cp, Rp = Map * Sd ** 2 / Bl ** 2, Bl ** 2 / (Sd ** 2 * Rap)
    Lx, Cx = 1.0186e-3, 3.979e-6                       # LR2 at 2.5 kHz, 8 ohm
    tRe, tPad, tLe, tfs, tQms, tQes = 3.6, 0.8, 0.05e-3, 900.0, 2.0, 1.2
    tRes = tRe * tQms / tQes
    tL = tRe / (2 * math.pi * tfs * tQes)
    tC = tQes / (2 * math.pi * tfs * tRe)

    def z(f):
        w = 2j * np.pi * np.asarray(f, dtype=float)
        zle = 1.0 / (1.0 / (w * Le) + 1.0 / Rle)
        y_mot = w * Cmes + 1.0 / (w * Lces) + 1.0 / Res
        if kind == "vented":
            y_mot = y_mot + 1.0 / (w * Lb + 1.0 / (w * Cp + 1.0 / Rp))
        else:
            y_mot = y_mot + 1.0 / (w * Lb)
        zw = Re + zle + 1.0 / y_mot
        z_woofer = w * Lx + 1.0 / (w * Cx + 1.0 / zw)        # series L, shunt C
        zt = tRe + w * tLe + 1.0 / (w * tC + 1.0 / (w * tL) + 1.0 / tRes) + tPad
        z_tweeter = 1.0 / (w * Cx) + 1.0 / (1.0 / (w * Lx) + 1.0 / zt)   # series C, shunt L
        return 1.0 / (1.0 / z_woofer + 1.0 / z_tweeter)

    f_ = A.f
    lines = [  # woofer: o -L1- spkw1 (C1 to 0); spkw1 -Re- -Le||Rle- motional to 0
        f"Lspkx1 o spkw1 {f_(Lx)}", f"Cspkx1 spkw1 0 {f_(Cx)}", f"Rspkwe spkw1 spkw2 {f_(Re)}", f"Lspkwe spkw2 spkw3 {f_(Le)}", f"Rspkwle spkw2 spkw3 {f_(Rle)}",
        f"Cspkwm spkw3 0 {f_(Cmes)}", f"Lspkwm spkw3 0 {f_(Lces)}", f"Rspkwm spkw3 0 {f_(Res)}"]
    if kind == "vented":
        lines += [f"Lspkwb spkw3 spkw4 {f_(Lb)}", f"Cspkwp spkw4 0 {f_(Cp)}", f"Rspkwp spkw4 0 {f_(Rp)}"]
    else:
        lines += [f"Lspkwb spkw3 0 {f_(Lb)}"]
    lines += [  # tweeter: o -C2- spkt1 (L2 to 0); spkt1 -pad- -Re- -Le- motional to 0
        f"Cspkx2 o spkt1 {f_(Cx)}", f"Lspkx2 spkt1 0 {f_(Lx)}", f"Rspktp spkt1 spkt2 {f_(tPad + tRe)}", f"Lspkte spkt2 spkt3 {f_(tLe)}",
        f"Cspktm spkt3 0 {f_(tC)}", f"Lspktm spkt3 0 {f_(tL)}", f"Rspktm spkt3 0 {f_(tRes)}"]
    return lines, z


def at(f, z, f0):
    return np.interp(math.log(f0), np.log(f), z.real) + 1j * np.interp(math.log(f0), np.log(f), z.imag)


def headphones():
    """HD800S (N28): a broad 650 ohm resonance at 110 Hz (Stereophile / InnerFidelity) on a 300 or 400 ohm base,
    against a resistor of the base value: each preset (its Zout from the zout_*.csv files) and an OTL headphone
    amplifier of 50 / 100 ohm."""
    fs = np.logspace(1, np.log10(40000), 600)
    band = (fs >= 20) & (fs <= 20000)

    def hd800s(base, peak=650.0, f0=110.0, q=1.5):
        w = fs / f0
        return base + (peak - base) / (1 + 1j * q * (w - 1 / w))

    def zout_of(key):
        a = np.loadtxt(os.path.join(OUT, f"zout_{key}.csv"), delimiter=",", skiprows=1)
        return np.interp(np.log(fs), np.log(a[:, 0]), a[:, 1]) + 1j * np.interp(np.log(fs), np.log(a[:, 0]), a[:, 2])

    out = {}
    for base in (300.0, 400.0):
        z = hd800s(base)
        rows = {}
        srcs = {k: zout_of(k) for k in ("set-300b", "set-2a3", "set-845", "pp-el34-ul_fb6", "pp-el34-ul_fb0", "pp-el34-ul_fb30")}
        srcs.update({f"OTL {r:.0f} ohm": np.full(fs.shape, r, dtype=complex) for r in (50.0, 100.0)})
        for k, zo in srcs.items():
            d = 20 * np.log10(np.abs(z / (z + zo)) / np.abs(base / (base + zo)))
            d -= np.interp(1000, fs, d)
            rows[k] = (float(d[band].min()), float(d[band].max()), float(np.interp(110, fs, d)))
            print(f"base {base:.0f} ohm  {k:<18} {rows[k][0]:+.4f} .. {rows[k][1]:+.4f} dB, at 110 Hz {rows[k][2]:+.4f} dB")
        out[str(int(base))] = rows
    json.dump(out, open(os.path.join(OUT, "hd800s.json"), "w"), indent=1)


if __name__ == "__main__" and "--headphones" in sys.argv:
    headphones()
elif __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    cases = [("set-300b", 6), ("set-2a3", 6), ("set-845", 6)] + [("pp-el34-ul", F) for F in (0, 6, 12, 20, 30)]
    res = {}
    for p, F in cases:
        f, z = zout(p, F)
        key = p if p != "pp-el34-ul" else f"{p} fb{F}"
        row = {}
        for f0 in (50.0, 1000.0, 10000.0):
            zz = at(f, z, f0)
            row[f"{int(f0)}"] = {"re": zz.real, "im": zz.imag, "mag": abs(zz), "df8": 8.0 / abs(zz)}
        res[key] = row
        np.savetxt(os.path.join(OUT, f"zout_{key.replace(' ', '_')}.csv"), np.c_[f, z.real, z.imag],
                   delimiter=",", header="f_Hz,re_ohm,im_ohm", comments="")
        print(f"{key:<18} " + "  ".join(f"{k:>5} Hz |Z| {v['mag']:6.3f} ohm DF {v['df8']:6.2f}" for k, v in row.items()), flush=True)
    json.dump(res, open(os.path.join(OUT, "zout.json"), "w"), indent=1)

    # the loudspeakers: their impedance, and each preset's response into them against 8 ohm
    fs = np.logspace(1, np.log10(40000), 400)
    spk = {}
    for kind in ("vented", "sealed"):
        lines, zf = speaker(kind)
        zs = zf(fs)
        np.savetxt(os.path.join(OUT, f"speaker_{kind}.csv"), np.c_[fs, zs.real, zs.imag], delimiter=",",
                   header="f_Hz,re_ohm,im_ohm", comments="")
        m = np.abs(zs)
        lo = fs < 300
        pk = [(fs[i], m[i]) for i in range(1, len(fs) - 1) if m[i] > m[i - 1] and m[i] > m[i + 1]]
        mn = [(fs[i], m[i]) for i in range(1, len(fs) - 1) if m[i] < m[i - 1] and m[i] < m[i + 1]]
        i_min = int(np.argmin(np.where(fs < 1000, m, 1e9)))
        print(f"speaker {kind}: peaks " + ", ".join(f"{a:.0f} Hz {b:.1f} ohm" for a, b in pk) +
              " | dips " + ", ".join(f"{a:.0f} Hz {b:.1f} ohm" for a, b in mn) +
              f" | min below 1 kHz {m[i_min]:.2f} ohm at {fs[i_min]:.0f} Hz | 20 kHz {np.interp(20000, fs, m):.1f} ohm"
              f" | phase {np.degrees(np.angle(zs)).min():.0f} .. {np.degrees(np.angle(zs)).max():.0f} deg", flush=True)
        spk[kind] = {"f": fs, "z": zs, "lines": lines}

    table = {}
    for p, F in cases:
        key = p if p != "pp-el34-ul" else f"{p} fb{F}"
        fz, z = zout(p, F)
        zo = np.interp(np.log(fs), np.log(fz), z.real) + 1j * np.interp(np.log(fs), np.log(fz), z.imag)
        rows = {}
        for kind, s in spk.items():
            d = 20 * np.log10(np.abs(s["z"] / (s["z"] + zo)) / np.abs(8.0 / (8.0 + zo)))
            d -= np.interp(1000.0, fs, d)   # relative to 1 kHz (the loudness match takes the overall level)
            np.savetxt(os.path.join(OUT, f"delta_{key.replace(' ', '_')}_{kind}.csv"), np.c_[fs, d], delimiter=",",
                       header="f_Hz,dB_vs_8ohm_rel_1kHz", comments="")
            band = (fs >= 20) & (fs <= 20000)
            rows[kind] = {"max_dB": float(d[band].max()), "f_max": float(fs[band][np.argmax(d[band])]),
                          "min_dB": float(d[band].min()), "f_min": float(fs[band][np.argmin(d[band])]),
                          "at": {str(x): float(np.interp(x, fs, d)) for x in (40, 50, 80, 150, 2500, 10000, 20000)}}
        table[key] = rows
        v = rows["vented"]
        print(f"{key:<18} vented: {v['min_dB']:+6.2f} dB @ {v['f_min']:6.0f} Hz .. {v['max_dB']:+6.2f} dB @ {v['f_max']:6.0f} Hz | "
              + " ".join(f"{k}:{x:+.2f}" for k, x in v["at"].items()), flush=True)
    json.dump(table, open(os.path.join(OUT, "delta.json"), "w"), indent=1)

    # cross-check: set-300b with the vented loudspeaker in the circuit, against the Thevenin estimate
    f8, h8 = response("set-300b", ["Rspk o 0 8"])
    fl, hl = response("set-300b", spk["vented"]["lines"])
    direct = 20 * np.log10(np.abs(hl) / np.abs(h8))
    direct -= np.interp(np.log(1000.0), np.log(fl), direct)
    est = np.interp(np.log(fl), np.log(fs), np.loadtxt(os.path.join(OUT, "delta_set-300b_vented.csv"), delimiter=",", skiprows=1)[:, 1])
    band = (fl >= 20) & (fl <= 20000)
    print(f"cross-check set-300b vented: direct AC vs Thevenin, max |difference| {np.max(np.abs(direct - est)[band]):.4f} dB", flush=True)
