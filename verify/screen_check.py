"""EL34: the fitted pentode models against the datasheet graphs that vary the screen voltage, which the plate
curves (Vg2 = 250 and 360 V) only sample at two values. In the ultralinear output stage the screen follows the
anode through the transformer tap, so this is where a wrong screen dependence (mu_g2g1) would show.

  Philips sheet A (page 9)    Ia vs Vg1 at Va / Vg2 = 250 / 250, 350 / 375, 400 / 425 V     tr_eb<Va>_vg2<Vg2>.csv
  Mullard C4 (page 10)        triode connected (g2 to anode): Ia vs Va, Vg1 = 0 .. -35 V     pt_vg<Vg1>_tri.csv
  Mullard C3 (page 9)         triode connected: Ia vs Vg1, Va = 250 .. 500 V                 tr_eb<Va>_tri.csv
  Mullard page 1              triode connected at 250 V, -15.5 V: Ia 70 mA, gm 11.5 mA/V, ra 910 ohm, mu 10.5

Triode connected, the anode and the screen are one electrode. Whether the graphs give Ia alone or Ia + Ig2 the
datasheet does not say, and the models fitted to the anode current do not predict Ig2; so two readings:
  H1  the graph is Ia (model: Ia(Va, Vg2 = Va, Vg1))
  H2  the graph is Ia + Ig2, with Ig2 = 0.15 Ia (Mullard page 1, pentode connection: 15 / 100 mA at Va = Vg2)
Errors in % of the datasheet value, points above 10 mA (below it the relative error of a hand-drawn curve says
little, and the Philips curve 1 crosses a dashed Ig2 curve there).

  python screen_check.py
"""
import os, re
import numpy as np

import koren_fit as K
import model_compare as M

np.seterr(all="ignore")
R_G2 = 0.15
D = os.path.join(K.DATA, "EL34")


def load(pattern):
    out = []
    for f in sorted(os.listdir(D)):
        m = re.match(pattern, f)
        if m:
            out.append((m, np.loadtxt(os.path.join(D, f), delimiter=",", skiprows=1, ndmin=2)))
    return out


def stats(err):
    e = np.array(err)
    return f"rms {np.sqrt(np.mean(e ** 2)):5.1f} %  worst {e[np.argmax(np.abs(e))]:+6.1f} %  (n {len(e)})"


def main():
    src, kind, curves = K.load("EL34")
    M.KOREN_SEED, _ = K.fit(kind, curves)
    fits = {"koren-pentode": M.KOREN_SEED, "derk": M.fit("derk", curves)[0]}
    for name, p in fits.items():
        print(f"\n== {name}")
        # Philips sheet A: pentode, three screen voltages
        print("  Philips page 9, Ia vs Vg1 (pentode):")
        allerr = []
        for m, a in load(r"^tr_eb(\d+)_vg2(\d+)\.csv$"):
            va, vg2 = float(m.group(1)), float(m.group(2))
            sel = a[:, 1] >= 10.0
            model = M.ia(name, p, {"ep": np.full(sel.sum(), va), "eg": a[sel, 0], "eg2": vg2}) * 1e3
            err = list((model - a[sel, 1]) / a[sel, 1] * 100)
            allerr += err
            print(f"    Va {va:4.0f} V, Vg2 {vg2:4.0f} V: {stats(err)}")
        print(f"    all: {stats(allerr)}")
        # Mullard triode connected
        for label, pat, xcol in (("Mullard page 10, Ia vs Va (triode connected)", r"^pt_vg(-?\d+(?:\.\d+)?)_tri\.csv$", "ep"),
                                 ("Mullard page 9, Ia vs Vg1 (triode connected)", r"^tr_eb(\d+)_tri\.csv$", "eg")):
            print(f"  {label}:")
            tot = {"H1": [], "H2": []}
            for m, a in load(pat):
                q = float(m.group(1))
                sel = a[:, 1] >= 10.0
                if xcol == "ep":
                    ep, eg = a[sel, 0], np.full(sel.sum(), q)
                else:
                    ep, eg = np.full(sel.sum(), q), a[sel, 0]
                ia = M.ia(name, p, {"ep": ep, "eg": eg, "eg2": ep}) * 1e3
                row = []
                for h, f in (("H1", 1.0), ("H2", 1.0 + R_G2)):
                    err = list((ia * f - a[sel, 1]) / a[sel, 1] * 100)
                    tot[h] += err
                    row.append(f"{h} {stats(err)}")
                print(f"    {'Vg1' if xcol == 'ep' else 'Va'} {q:5g} V: " + "   ".join(row))
            print(f"    all: H1 {stats(tot['H1'])}   H2 {stats(tot['H2'])}")
        # Mullard page 1, triode connected at 250 V / -15.5 V
        def it(ep, eg):
            return float(M.ia(name, p, {"ep": np.array([ep]), "eg": eg, "eg2": np.array([ep])})[0])
        ep, eg = 250.0, -15.5
        hv, hg = 0.25, 0.0155
        ra = 1.0 / ((it(ep + hv, eg) - it(ep - hv, eg)) / (2 * hv))
        gm = (it(ep, eg + hg) - it(ep, eg - hg)) / (2 * hg)
        print(f"  Mullard page 1, triode connected 250 V / -15.5 V:")
        for h, f in (("H1", 1.0), ("H2", 1.0 + R_G2)):
            i, g, r = it(ep, eg) * f * 1e3, gm * f * 1e3, ra / f
            print(f"    {h}: Ia {i:6.1f} mA ({(i - 70) / 70 * 100:+.1f} %)  gm {g:5.2f} mA/V ({(g - 11.5) / 11.5 * 100:+.1f} %)  "
                  f"ra {r:5.0f} ohm ({(r - 910) / 910 * 100:+.1f} %)  mu {g * r / 1e3:5.2f} ({(g * r / 1e3 - 10.5) / 10.5 * 100:+.1f} %)")


if __name__ == "__main__":
    main()
