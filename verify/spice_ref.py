"""ngspice reference output stages from the fitted tube models (verify/data/tubes/<tube>/model.json).

First step of M0: the output stage of each preset, driven by an ideal grid source, for the power it gives and its
harmonics. The drivers, the transformer's inductances and saturation and the feedback loop come next.

  Single-ended: the plate feeds the reflected load through a 100 uF coupling capacitor and gets its DC through a
  20 H choke (an ideal transformer at 1 kHz). Rated power = the drive at which the grid's positive peak reaches
  0 V (the end of class A1: no grid current).
  Push-pull ultralinear: two pentodes on a centre-tapped primary with screen taps (coupled inductors, k = 0.99999),
  separate cathode bias (100 uF bypass); same end point (each grid reaches 0 V).

Tube models as B sources: Koren triode, Reefman "Derk" pentode (anode current), the screen current taken as 0.15 of
the anode current (Mullard EL34 data, page 1: 15 / 100 mA at Va = Vg2; the anode-current fit does not determine
it). Written to converge: ln(1 + exp(z)) in its stable form, the electrode voltages through a smooth positive part
(10 mV corner) instead of max(v, 0), the logarithm's argument kept above zero.

Simulation: transient with reltol 1e-5, gear integration, at least 1000 points per 1 kHz cycle, 0.25 s so that the
coupling capacitors, chokes and cathode bias settle, then an FFT over the last 10 whole cycles. check_tolerance()
verifies the tolerance by its numerical floor (a tiny signal) and by tightening it one step.

  python spice_ref.py             output stages
  python spice_ref.py --check     the tolerance checks
"""
import json, os, subprocess, sys, tempfile
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "data", "tubes")
NG = os.path.join(os.path.dirname(HERE), ".tools", "ngspice-47", "Spice64", "bin",
                  "ngspice_con.exe" if os.name == "nt" else "ngspice")
F0 = 1000.0
RELTOL = 1e-5
T_STOP = 0.25
TIMEOUT = 300


def f(v):
    return format(float(v), ".12g")


def softplus(z):
    return f"(max({z},0)+ln(1+exp(-abs({z}))))"


def pos(v, s=0.01):
    """Smooth max(v, 0), 10 mV corner: no kink in the derivative where an electrode voltage crosses zero."""
    return f"({s}*{softplus(f'({v})/{s}')})"


def model(tube):
    with open(os.path.join(DATA, tube, "model.json"), encoding="utf-8") as h:
        return json.load(h)


def triode(name, tube, a, g, k):
    """Koren triode between nodes a (plate), g, k. Koren's (1 + sgn E1) is not needed: E1 > 0 by construction."""
    p = model(tube)["params"]
    mu, ex, kg1, kp, kvb = (f(p[n]) for n in ("MU", "EX", "KG1", "KP", "KVB"))
    vak, vgk = f"v({a},{k})", f"v({g},{k})"
    z = f"{kp}*(1/{mu}+{vgk}/sqrt({kvb}+{vak}*{vak}))"
    e1 = f"({pos(vak)}/{kp}*{softplus(z)}+1e-30)"
    return f"B{name} {a} {k} I = 2*exp({ex}*ln({e1}))/{kg1}"


def screen_kg2(tube, ratio=0.15, va=250.0, vg2=250.0, vg=-12.2):
    """1 / kg2 for the varying screen current: Ig2 / Ia = ratio at the datasheet point (EL34: Mullard page 1)."""
    p = model(tube)["params"]
    kk, beta, d = p["K"], p["BETA"], p["D"]
    ia_per_ik = kk * beta * va / (1 + beta * va) + d * va
    return ratio * ia_per_ik - kk / (1 + beta * va)


def pentode(name, tube, a, g2, g, k, r_g2=0.15, screen="fixed", cutoff_c=0.0):
    """Derk pentode: anode current a -> k; screen current g2 -> k.
    screen "fixed": r_g2 times the anode current.
    screen "fit": the fitted screen current, model.json G2 and AS (Reefman eq. 23).
    screen "derk": constant space current (Reefman sec. 4.3): the total current I_K (1/kg1 + D Va) minus the anode
    current, Ig2 = I_K (1/kg2 + K / (1 + beta Va)): it rises at low Va as the anode current falls. 1/kg2 from the
    datasheet's Ig2 / Ia at one point (screen_kg2)."""
    p = model(tube)["params"]
    mu, x, kp, kvb, kk, beta, d = (f(p[n]) for n in ("MU", "X", "KP", "KVB", "K", "BETA", "D"))
    vak, vg2k, vgk = f"v({a},{k})", f"v({g2},{k})", f"v({g},{k})"
    z = f"{kp}*(1/{mu}+{vgk}/sqrt({kvb}+{vg2k}*{vg2k}))"
    ik = f"exp({x}*ln({pos(vg2k)}/{kp}*{softplus(z)}+1e-30))"
    va = pos(vak)
    ia = f"{ik}*({kk}*{beta}*{va}/(1+{beta}*{va})+{d}*{va})"
    if screen == "fit":
        # the fitted screen current (Reefman eq. 23; G2, AS in model.json, fitted with Philips sheet A)
        ig2 = f"{ik}*{f(p['G2'])}*(1+{f(p['AS'])}/(1+{beta}*{va}))"
    elif screen == "derk":
        ig2 = f"{ik}*({f(screen_kg2(tube))}+{kk}/(1+{beta}*{va}))"
    else:
        ig2 = f"{f(r_g2)}*{ia}"
    if cutoff_c:
        # SENSITIVITY TEST ONLY, never in a preset: both currents scaled by 1 - c exp(-Ia / 15 mA), lowering the
        # model's current near cutoff towards the datasheet curves
        corr = f"(1-{f(cutoff_c)}*exp(-({ia})/0.015))"
        ia, ig2 = f"({ia})*{corr}", f"({ig2})*{corr}"
    return [f"B{name}a {a} {k} I = {ia}", f"B{name}s {g2} {k} I = {ig2}"]


def run(netlist, probes, reltol=None):
    reltol = reltol or RELTOL
    step = 1 / F0 / 1000
    with tempfile.TemporaryDirectory() as td:
        out = os.path.join(td, "out.txt")
        cir = os.path.join(td, "ref.cir")
        lines = netlist + [f".options reltol={reltol:g} abstol=1e-12 vntol=1e-6 method=gear",
                           ".control", f"tran {step:g} {T_STOP:g} 0 {step:g}",
                           f"wrdata {out} " + " ".join(probes), "quit", ".endc", ".end", ""]
        with open(cir, "w") as h:
            h.write("\n".join(lines))
        try:
            r = subprocess.run([NG, "-b", cir], capture_output=True, text=True, timeout=TIMEOUT)
        except subprocess.TimeoutExpired:
            raise SystemExit(f"ngspice did not finish within {TIMEOUT} s")
        if not os.path.exists(out):
            raise SystemExit(r.stdout[-2000:] + r.stderr[-2000:])
        a = np.loadtxt(out, ndmin=2)
        if a[-1, 0] < 0.99 * T_STOP:
            raise SystemExit(f"transient stopped at {a[-1, 0]:.3g} s:\n" + r.stdout[-2500:])
    return a[:, 0], a[:, 1::2]


def harmonics(t, v, cycles=10):
    """Fundamental amplitude and H2..H7 in dB re the fundamental, over the last `cycles` whole periods."""
    n_per = 1000
    tt = np.linspace(t[-1] - cycles / F0, t[-1], cycles * n_per, endpoint=False)
    y = np.interp(tt, t, v)
    y = y - y.mean()
    s = np.fft.rfft(y) / len(y) * 2
    a = np.abs(s[[cycles * h for h in range(1, 8)]])
    return a[0], 20 * np.log10(np.maximum(a[1:], 1e-300) / a[0])


def se_stage(tube, b_plus, rk, bias_fixed, r_load, vin_peak, reltol=None):
    """Single-ended output stage. bias_fixed: fixed grid bias in V (rk unused) or None (cathode bias rk)."""
    net = ["* single-ended", f"Vb bp 0 DC {f(b_plus)}", "Lp bp a 20", "Cc a o 1e-4", f"Rl o 0 {f(r_load)}",
           f"Vin in 0 SIN(0 {f(vin_peak)} {f(F0)})",
           f"Vbias g in DC {f(bias_fixed)}" if bias_fixed is not None else "Rg g in 1",
           triode("1", tube, "a", "g", "k")]
    net += ["Rk k 0 1e-3"] if bias_fixed is not None else [f"Rk k 0 {f(rk)}", "Ck k 0 1e-4"]
    t, y = run(net, ["v(o)", "v(a)", "v(k)", "i(Vb)"], reltol)
    vo, va, vk, ib = y[:, 0], y[:, 1], y[:, 2], -y[:, 3]
    a1, h = harmonics(t, vo)
    last = t > t[-1] - 10 / F0
    return {"P_W": a1 ** 2 / 2 / r_load, "H": h, "Ia_mA": float(np.mean(ib[last]) * 1e3),
            "Vak_V": float(np.mean(va[last] - vk[last])), "Vk_V": float(np.mean(vk[last]))}


def pp_ul_stage(tube, b_plus, rk, r_g2, raa, tap, r_spk, vin_peak, reltol=None, k=0.99999, lp=100.0, screen="fixed", cutoff_c=0.0):
    """Push-pull ultralinear, separate cathode bias. tap: screen tap as a fraction of each half primary (from the
    centre tap). Coupled inductors: each half primary is two windings (centre tap .. screen tap .. anode), the
    secondary a fifth; every pair coupled with k (sign by winding sense: the two halves are wound so that the
    anodes swing in opposition). lp: anode-to-anode inductance; the secondary gives raa on r_spk.
    Returns also the quiescent values (before the signal: the first 0.1 ms) and the screen-tap check: the tap's
    swing over its anode's, which must be +tap (in phase) for ultralinear feedback."""
    lh = lp / 4
    l_ct_tap, l_tap_a = lh * tap ** 2, lh * (1 - tap) ** 2
    ls = lp * r_spk / raa
    kp_, kn = f(k), f(-k)
    net = ["* push-pull ultralinear", f"Vb bp 0 DC {f(b_plus)}",
           f"L1 bp s1 {f(l_ct_tap)}", f"L2 s1 a1 {f(l_tap_a)}",
           f"L3 bp s2 {f(l_ct_tap)}", f"L4 s2 a2 {f(l_tap_a)}",
           f"L5 o 0 {f(ls)}", f"Rl o 0 {f(r_spk)}",
           f"K12 L1 L2 {kp_}", f"K13 L1 L3 {kn}", f"K14 L1 L4 {kn}", f"K15 L1 L5 {kp_}",
           f"K23 L2 L3 {kn}", f"K24 L2 L4 {kn}", f"K25 L2 L5 {kp_}",
           f"K34 L3 L4 {kp_}", f"K35 L3 L5 {kn}", f"K45 L4 L5 {kn}",
           f"Rs1 s1 g21 {f(r_g2)}", f"Rs2 s2 g22 {f(r_g2)}",
           f"Vin1 g1 0 SIN(0 {f(vin_peak)} {f(F0)})", f"Vin2 g2 0 SIN(0 {f(-vin_peak)} {f(F0)})",
           f"Rk1 k1 0 {f(rk)}", "Ck1 k1 0 1e-4", f"Rk2 k2 0 {f(rk)}", "Ck2 k2 0 1e-4"]
    net += (pentode("1", tube, "a1", "g21", "g1", "k1", screen=screen, cutoff_c=cutoff_c)
            + pentode("2", tube, "a2", "g22", "g2", "k2", screen=screen, cutoff_c=cutoff_c))
    t, y = run(net, ["v(o)", "v(k1)", "i(Vb)", "v(a1)", "v(s1)", "v(g21)"], reltol)
    vo, vk, ib, va, vs, vg2 = y[:, 0], y[:, 1], -y[:, 2], y[:, 3], y[:, 4], y[:, 5]
    a1, h = harmonics(t, vo)
    last = t > t[-1] - 10 / F0
    q = t < 1e-4
    ac_a, ac_s = va[last] - va[last].mean(), vs[last] - vs[last].mean()
    tap_ratio = float(np.dot(ac_s, ac_a) / np.dot(ac_a, ac_a))
    return {"P_W": a1 ** 2 / 2 / r_spk, "H": h, "Ib_mA": float(np.mean(ib[last]) * 1e3), "Vk_V": float(np.mean(vk[last])),
            "q_Ik_mA": float(np.mean(vk[q]) / rk * 1e3), "q_Vk_V": float(np.mean(vk[q])), "q_Va_V": float(np.mean(va[q] - vk[q])),
            "q_Vg2_V": float(np.mean(vg2[q] - vk[q])), "tap": tap_ratio}


def show(label, r):
    hs = " ".join(f"H{i + 2} {v:7.1f}" for i, v in enumerate(r["H"][:4]))
    thd = 100 * np.sqrt(np.sum(10 ** (r["H"] / 10)))
    extra = " ".join(f"{k} {v:.1f}" for k, v in r.items() if k not in ("P_W", "H"))
    print(f"  {label:<26} P {r['P_W']:7.3f} W  THD {thd:6.3f} %  {hs} dB   {extra}", flush=True)
    return thd


def check_tolerance():
    """Numerical floor: a tiny signal (H2 should scale with the level, H3 with its square; anything flat near the
    bottom is the solver's noise) and the nominal case with the tolerance one step tighter."""
    print("tolerance check, set-300b output stage")
    for v in (0.01, 0.1, 1.0):
        show(f"{v} V peak, reltol {RELTOL:g}", se_stage("300B", 350.0, 0, -74.0, 4000.0, v))
    a = se_stage("300B", 350.0, 0, -74.0, 4000.0, 30.0)
    b = se_stage("300B", 350.0, 0, -74.0, 4000.0, 30.0, reltol=RELTOL / 10)
    show(f"30 V peak, reltol {RELTOL:g}", a)
    show(f"30 V peak, reltol {RELTOL / 10:g}", b)
    print(f"  H2..H5 change: " + " ".join(f"{x:+.4f}" for x in (b["H"] - a["H"])[:4]) + " dB")


if __name__ == "__main__":
    if "--check" in sys.argv:
        check_tolerance()
        sys.exit(0)
    print("set-2a3: 2A3, RCA class A1 (250 V, -45 V, 60 mA, 2500 ohm, 3.5 W at 5 % H2); cathode bias 750 ohm (RCA)")
    for v in (5, 20, 45):
        show(f"drive {v} V peak", se_stage("2A3", 295.0, 750.0, None, 2500.0, v))
    print("set-300b: 300B, WE class A1 (350 V, -74 V, 60 mA, 4000 ohm, 7 W at 5 %); fixed bias")
    for v in (5, 30, 74):
        show(f"drive {v} V peak", se_stage("300B", 350.0, 0, -74.0, 4000.0, v))
    print("set-845: 845, RCA class A1 (1000 V, -145 V, 6000 ohm, U.P.O. 24 W at 5 % H2); fixed bias")
    for v in (10, 60, 145):
        show(f"drive {v} V peak", se_stage("845", 1000.0, 0, -145.0, 6000.0, v))
    print("pp-el34-ul: Mullard 5-20 output stage (20 % tap, 440 V, 470 ohm / valve, Rg2 1 k, 7.0 k a-a; rated 20 W)")
    for v in (5, 20, 35):
        show(f"drive {v} V peak g1-k", pp_ul_stage("EL34", 440.0, 470.0, 1000.0, 7000.0, 0.20, 8.0, v))
    print("EL34 check: Mullard data page 2, 43 % tap (430 V, 470 ohm / valve, Rg2 1 k, 6.0 k a-a; 34 W at 2.5 %)")
    for v in (5, 20, 35):
        show(f"drive {v} V peak g1-k", pp_ul_stage("EL34", 430.0, 470.0, 1000.0, 6000.0, 0.43, 8.0, v))
