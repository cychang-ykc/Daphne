"""ngspice reference output stages from the fitted tube models (verify/data/tubes/<tube>/model.json).

First step of M0: the output stage of each preset at the datasheet's typical operating conditions, driven by an
ideal grid source, to see the power it gives and compare with what such amplifiers are rated at. The drivers,
the output transformer's inductances and saturation and the feedback loop come after the circuits are agreed.

  Single-ended: the plate feeds the reflected load through a 100 uF coupling capacitor and gets its DC through a
  100 H choke (an ideal transformer at 1 kHz). Rated power = the drive at which the grid's positive peak reaches
  0 V (the end of class A1: no grid current).
  Push-pull ultralinear: two pentodes on a centre-tapped primary with screen taps (coupled inductors, k = 0.99999),
  separate cathode bias; same end point (each grid reaches 0 V).

Tube models as B sources: Koren triode, Reefman "Derk" pentode (anode current), with the screen current taken as
0.15 of the anode current (Mullard EL34 data, page 1: 15 / 100 mA at Va = Vg2): the anode-current fit does not
determine it. Tolerances tightened (reltol 1e-9); ln(1 + exp(z)) written in its stable form.

  python spice_ref.py
"""
import json, os, subprocess, tempfile
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "data", "tubes")
NG = os.path.join(os.path.dirname(HERE), ".tools", "ngspice-47", "Spice64", "bin",
                  "ngspice_con.exe" if os.name == "nt" else "ngspice")
F0 = 1000.0


def f(v):
    return format(float(v), ".12g")


def softplus(z):
    return f"(max({z},0)+ln(1+exp(-abs({z}))))"


def model(tube):
    with open(os.path.join(DATA, tube, "model.json"), encoding="utf-8") as h:
        return json.load(h)


def triode(name, tube, a, g, k):
    """Koren triode between nodes a (plate), g, k."""
    p = model(tube)["params"]
    mu, ex, kg1, kp, kvb = (f(p[n]) for n in ("MU", "EX", "KG1", "KP", "KVB"))
    vak, vgk = f"v({a},{k})", f"v({g},{k})"
    z = f"{kp}*(1/{mu}+{vgk}/sqrt({kvb}+{vak}*{vak}))"
    e1 = f"max({vak}/{kp}*{softplus(z)},1e-300)"
    return f"B{name} {a} {k} I = 2*exp({ex}*ln({e1}))/{kg1}"


def pentode(name, tube, a, g2, g, k, r_g2=0.15):
    """Derk pentode: anode current a -> k, screen current r_g2 times it, g2 -> k."""
    p = model(tube)["params"]
    mu, x, kp, kvb, kk, beta, d = (f(p[n]) for n in ("MU", "X", "KP", "KVB", "K", "BETA", "D"))
    vak, vg2k, vgk = f"v({a},{k})", f"v({g2},{k})", f"v({g},{k})"
    z = f"{kp}*(1/{mu}+{vgk}/sqrt({kvb}+{vg2k}*{vg2k}))"
    ik = f"exp({x}*ln(max({vg2k}/{kp}*{softplus(z)},1e-300)))"
    ia = f"{ik}*({kk}*(1-1/(1+{beta}*max({vak},0)))+{d}*max({vak},0))"
    return [f"B{name}a {a} {k} I = {ia}", f"B{name}s {g2} {k} I = {f(r_g2)}*{ia}"]


def run(netlist, probes, t_stop, t_step):
    with tempfile.TemporaryDirectory() as td:
        out = os.path.join(td, "out.txt")
        cir = os.path.join(td, "ref.cir")
        lines = netlist + [".options reltol=1e-9 abstol=1e-15 vntol=1e-9 method=gear maxord=2",
                           ".control", f"tran {t_step} {t_stop} 0 {t_step}",
                           f"wrdata {out} " + " ".join(probes), "quit", ".endc", ".end", ""]
        open(cir, "w").write("\n".join(lines))
        r = subprocess.run([NG, "-b", cir], capture_output=True, text=True)
        if not os.path.exists(out):
            raise SystemExit(r.stdout[-2000:] + r.stderr[-2000:])
        a = np.loadtxt(out, ndmin=2)
        if len(a) < 100:
            raise SystemExit("transient stopped early:\n" + r.stdout[-2500:] + r.stderr[-1500:])
    return a[:, 0], a[:, 1::2]


def harmonics(t, v, cycles=10):
    """Fundamental amplitude and H2..H7 in dB re the fundamental, over the last `cycles` periods."""
    n_per = 400
    tt = np.linspace(t[-1] - cycles / F0, t[-1], cycles * n_per, endpoint=False)
    y = np.interp(tt, t, v)
    y = y - y.mean()
    s = np.fft.rfft(y) / len(y) * 2
    a = np.abs(s[[cycles * h for h in range(1, 8)]])
    return a[0], 20 * np.log10(np.maximum(a[1:], 1e-30) / a[0])


def se_stage(tube, b_plus, rk, bias_fixed, r_load, vin_peak):
    """Single-ended output stage. bias_fixed: grid bias in V (fixed bias, rk = 0) or None (cathode bias rk)."""
    net = ["* single-ended", f"Vb bp 0 DC {f(b_plus)}", "Lp bp a 100", "Cc a o 1e-4", f"Rl o 0 {f(r_load)}",
           f"Vin in 0 SIN(0 {f(vin_peak)} {f(F0)})",
           f"Vbias g in DC {f(bias_fixed)}" if bias_fixed is not None else "Rg g in 1",
           triode("1", tube, "a", "g", "k")]
    net += ["Rk k 0 1e-3"] if bias_fixed is not None else [f"Rk k 0 {f(rk)}", "Ck k 0 1e-3"]
    t, y = run(net, ["v(o)", "v(a)", "v(k)", "i(Vb)"], 0.03, 1 / F0 / 400)
    vo, va, vk, ib = y[:, 0], y[:, 1], y[:, 2], -y[:, 3]
    a1, h = harmonics(t, vo)
    return {"P_W": a1 ** 2 / 2 / r_load, "H": h, "Ia_mA": float(np.mean(ib[-4000:]) * 1e3),
            "Va_V": float(np.mean(va[-4000:] - vk[-4000:])), "Vk_V": float(np.mean(vk[-4000:]))}


def pp_ul_stage(tube, b_plus, rk, r_g2, raa, tap, r_spk, vin_peak):
    """Push-pull ultralinear, separate cathode bias. tap: screen tap as a fraction of each half primary (from
    the centre tap). Primary a-a inductance 100 H here (ideal at 1 kHz); secondary to give raa on r_spk."""
    lp = 100.0
    lh = lp / 4                                   # each half: (N/2)^2
    l_ct_tap = lh * tap ** 2                      # centre tap .. screen tap
    l_tap_a = lh * (1 - tap) ** 2                 # screen tap .. anode
    ls = lp * r_spk / raa
    net = ["* push-pull ultralinear", f"Vb bp 0 DC {f(b_plus)}",
           f"L1 bp s1 {f(l_ct_tap)}", f"L2 s1 a1 {f(l_tap_a)}",
           f"L3 bp s2 {f(l_ct_tap)}", f"L4 s2 a2 {f(l_tap_a)}",
           f"L5 o 0 {f(ls)}", f"Rl o 0 {f(r_spk)}",
           # L1, L2 wound one way from the centre tap, L3, L4 the other (dot convention by node order)
           "K12 L1 L2 0.99999", "K13 L1 L3 -0.99999", "K14 L1 L4 -0.99999", "K15 L1 L5 0.99999",
           "K23 L2 L3 -0.99999", "K24 L2 L4 -0.99999", "K25 L2 L5 0.99999",
           "K34 L3 L4 0.99999", "K35 L3 L5 -0.99999", "K45 L4 L5 -0.99999",
           f"Rs1 s1 g21 {f(r_g2)}", f"Rs2 s2 g22 {f(r_g2)}",
           f"Vin1 g1 0 SIN(0 {f(vin_peak)} {f(F0)})", f"Vin2 g2 0 SIN(0 {f(-vin_peak)} {f(F0)})",
           f"Rk1 k1 0 {f(rk)}", "Ck1 k1 0 1e-3", f"Rk2 k2 0 {f(rk)}", "Ck2 k2 0 1e-3"]
    net += pentode("1", tube, "a1", "g21", "g1", "k1") + pentode("2", tube, "a2", "g22", "g2", "k2")
    t, y = run(net, ["v(o)", "v(k1)", "i(Vb)"], 0.03, 1 / F0 / 400)
    vo, vk, ib = y[:, 0], y[:, 1], -y[:, 2]
    a1, h = harmonics(t, vo)
    return {"P_W": a1 ** 2 / 2 / r_spk, "H": h, "Ib_mA": float(np.mean(ib[-4000:]) * 1e3), "Vk_V": float(np.mean(vk[-4000:]))}


def show(label, r):
    hs = " ".join(f"H{i + 2} {v:6.1f}" for i, v in enumerate(r["H"][:4]))
    thd = 100 * np.sqrt(np.sum(10 ** (r["H"] / 10)))
    extra = " ".join(f"{k} {v:.1f}" for k, v in r.items() if k not in ("P_W", "H"))
    print(f"  {label:<28} P {r['P_W']:6.2f} W  THD {thd:5.2f} %  {hs} dB   {extra}")


if __name__ == "__main__":
    print("set-2a3: 2A3, RCA class A1 (250 V, -45 V, 60 mA, 2500 ohm, 3.5 W at 5 % H2); cathode bias 750 ohm (RCA)")
    for v in (5, 20, 45):
        show(f"drive {v} V peak", se_stage("2A3", 295.0, 750.0, None, 2500.0, v))
    print("set-300b: 300B, WE class A1 (350 V, -74 V, 60 mA, 4000 ohm, 7 W at 5 %); fixed bias as in the datasheet")
    for v in (5, 30, 74):
        show(f"drive {v} V peak", se_stage("300B", 350.0, 0, -74.0, 4000.0, v))
    print("set-845: 845, RCA class A1 (1000 V, -145 V, 6000 ohm, U.P.O. 24 W at 5 % H2); fixed bias")
    for v in (10, 60, 145):
        show(f"drive {v} V peak", se_stage("845", 1000.0, 0, -145.0, 6000.0, v))
    print("pp-el34-ul: 2 x EL34, Mullard 43 % tap (430 V, 470 ohm / valve, Rg2 1 k, 6.0 k a-a; 34 W at 2.5 %)")
    for v in (5, 20, 35):
        show(f"drive {v} V peak g1-k", pp_ul_stage("EL34", 430.0, 470.0, 1000.0, 6000.0, 0.43, 8.0, v))
    print("pp-el34-ul: Mullard 5-20 output stage (20 % tap, 440 V, 470 ohm / valve, Rg2 1 k, 7.0 k a-a; rated 20 W)")
    for v in (5, 20, 35):
        show(f"drive {v} V peak g1-k", pp_ul_stage("EL34", 440.0, 470.0, 1000.0, 7000.0, 0.20, 8.0, v))
