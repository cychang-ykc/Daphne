"""pp-el34-ul stage by stage (N28): H2 / H3 at the EF86 anode, the splitter's differential output (the EL34 grid
drive) and the loudspeaker, the splitter's balance (amplitude ratio, phase, common-mode component), the EL34 bias
currents; and the scan of V2B's anode load R12 that shows where the push-pull's H2 comes from (the long-tailed pair's
own imbalance with equal anode loads). Xyce harmonic balance, 1 kHz.

  python pp_stages.py            stages at 1.25 W and 20 W (open loop and the 6 dB step), R12 scan, balance point
"""
import json, math, os, subprocess, sys, tempfile
import numpy as np

import spice_amps as A, xyce_hb as X

HERE = os.path.dirname(os.path.abspath(__file__))
PROBES = ["v(o)", "v(a1)", "v(a2a)", "v(a2b)", "v(g3s)", "v(g4s)", "v(k3)", "v(k4)", "v(a3)", "v(a4)"]


def hb(kw, amp, nf=60):
    net = [l.replace("{amp}", A.f(amp)).replace("{f0}", "1000") for l in A.pp_520(**kw)]
    with tempfile.TemporaryDirectory() as td:
        c = os.path.join(td, "a.cir")
        with open(c, "w") as h:
            h.write("\n".join(X.to_xyce(net) + [".hb 1000", f".options hbint numfreq={nf}", ".print hb " + " ".join(PROBES), ".end", ""]))
        subprocess.run([X.XYCE, c], capture_output=True, text=True, cwd=td)
        rows = [l.split() for l in open(c + ".HB.FD.prn") if l.split() and l.split()[0].isdigit()]
    S = {p: {} for p in PROBES}
    for r in rows:
        h = round(float(r[1]) / 1000)
        if h < 0:
            continue
        for i, p in enumerate(PROBES):
            S[p][h] = complex(float(r[2 + 2 * i]), float(r[3 + 2 * i]))
    S["diff split"] = {h: S["v(a2a)"][h] - S["v(a2b)"][h] for h in S["v(o)"]}
    S["common split"] = {h: (S["v(a2a)"][h] + S["v(a2b)"][h]) / 2 for h in S["v(o)"]}
    return S


def at_p(kw, p, g0):
    amp = 4 * math.sqrt(p) / g0
    for _ in range(3):
        amp *= math.sqrt(p / X.measure("pp-el34-ul", amp, 1000.0, 15, kw)["P"])
    return amp


def db(d, h, ref):
    return 20 * math.log10(max(abs(d[h]), 1e-300) / abs(ref))


def imbalance_db(r12):
    d = hb(dict(feedback=False, r12=r12), 0.002, 15)
    return 20 * math.log10(abs(d["v(a2a)"][1]) / abs(d["v(a2b)"][1]))


def balance_r12(lo=180e3, hi=195e3):
    for _ in range(20):
        m = 0.5 * (lo + hi)
        lo, hi = (m, hi) if imbalance_db(m) > 0 else (lo, m)
    return 0.5 * (lo + hi)


def report(kw, p, g0, label):
    S = hb(kw, at_p(kw, p, g0))
    print(f"== {label}, {p} W")
    for k in ("v(a1)", "diff split", "v(o)"):
        d = S[k]
        print(f"   {k:<12} {2 * abs(d[1]):9.3f} V pk  H2 {db(d, 2, d[1]):7.1f}  H3 {db(d, 3, d[1]):7.1f}  H4 {db(d, 4, d[1]):7.1f}  H5 {db(d, 5, d[1]):7.1f} dB")
    a, b = S["v(a2a)"][1], S["v(a2b)"][1]
    print(f"   splitter |a2a|/|a2b| {20 * math.log10(abs(a) / abs(b)):+.3f} dB, phase {math.degrees(np.angle(a / -b)):+.2f} deg from antiphase, "
          f"common mode {db(S['common split'], 1, S['diff split'][1]):.1f} dB re differential; "
          f"EL34 bias {S['v(k3)'][0].real / 470 * 1e3:.2f} / {S['v(k4)'][0].real / 470 * 1e3:.2f} mA")


if __name__ == "__main__":
    steps = {s["F"]: s for s in json.load(open(os.path.join(HERE, "data", "ref", "pp_feedback.json")))["steps"]}
    for r12, tag in ((A.R12_BOOK, "book R12 180 k"), (A.R12_BALANCED, f"balanced R12 {A.R12_BALANCED / 1e3:.2f} k")):
        for name, kw, g0 in (("open loop", dict(feedback=False), 2248.0),
                             ("6 dB step", dict(feedback=True, r13=steps[6]["r13"], c9=steps[6]["c9"]), 1124.0)):
            for p in (1.25, 20.0):
                report(dict(kw, r12=r12), p, g0, f"{tag}, {name}")
    print("R12 scan (open loop, 1.25 W):")
    for r12 in (180e3, 184.9e3, A.R12_BALANCED, 189e3):
        kw = dict(feedback=False, r12=r12)
        d = hb(kw, at_p(kw, 1.25, 2248.0))["v(o)"]
        print(f"   R12 {r12 / 1e3:6.2f} k: imbalance {imbalance_db(r12):+.3f} dB, output H2 {db(d, 2, d[1]):6.1f} H3 {db(d, 3, d[1]):6.1f} dB")
    print(f"balance point: R12 = {balance_r12() / 1e3:.2f} k")
