"""Output-transformer core (N28): Jiles-Atherton parameters of Xyce's CORE model, fitted in Xyce itself.

Material: grain-oriented M-6 (0.35 mm). Nippon Steel catalogue D003jec (2024) p. 8, rolling direction (Epstein,
JIS C 2550): B8 = 1.88 T (H 800 A/m), B25 = 1.94 T (H 2500 A/m). Saturation 2.03 T (Fe-3 % Si, the usual value;
MS = 2.03 / mu0 = 1.615e6 A/m). Not in the catalogue, assumed (rolling direction): coercivity 8 A/m, low-flux
(0.1 T) relative permeability 8000.

EI discount: the yokes of EI laminations, about 1/3 of the magnetic path, run across the grain. With the cross-grain
permeability taken as 1/10 of the rolling direction, the effective permeability is 1 / (2/3 + 1/3 x 10) = 1/4: the
effective material needs 4 x the field of the catalogue for the same B. It is applied as a field scale on every
target (B8 at 3200 A/m, B25 at 10000 A/m, coercivity 32 A/m, low-flux permeability 2000).

Fit: Xyce transient of a test winding (100 turns, path 10 cm, area 1 cm2) driven by a 10 Hz current, B and H from
Xyce (BHSIUNITS=1), last cycle; least squares on A, ALPHA, C, K (log). Variants for the sensitivity test: coercivity
x0.5 / x2, low-flux permeability x0.5 / x2.

  python core_model.py          fits the base and the four variants (parallel), writes data/core/ja_fit.json
"""
import json, math, os, subprocess, sys, tempfile, time
for _v in ("OMP_NUM_THREADS", "OPENBLAS_NUM_THREADS", "MKL_NUM_THREADS"):
    os.environ.setdefault(_v, "1")
from concurrent.futures import ProcessPoolExecutor
import numpy as np
from scipy.optimize import least_squares

import xyce_hb as X

MU0 = 4e-7 * math.pi
MS = 2.03 / MU0
DISCOUNT = 4.0                      # field scale for the EI yokes across the grain
RD = {"B8": 1.88, "B25": 1.94, "Hc": 8.0, "mu_low": 8000.0}
HERE = os.path.dirname(os.path.abspath(__file__))


def targets(hc_scale=1.0, mu_scale=1.0):
    mu = RD["mu_low"] / DISCOUNT * mu_scale
    return {"tip_3200": (800 * DISCOUNT, RD["B8"]), "tip_10000": (2500 * DISCOUNT, RD["B25"]),
            "Hc": RD["Hc"] * DISCOUNT * hc_scale, "low": (0.1 / (MU0 * mu), 0.1)}


def model_line(name, p, area_cm2, path_cm, gap_cm=0.0):
    A_, alpha, c, k = p
    return (f".model {name} CORE (MS={MS:.6g} A={A_:.6g} ALPHA={alpha:.6g} C={c:.6g} K={k:.6g} "
            f"AREA={area_cm2:.6g} PATH={path_cm:.6g} GAP={gap_cm:.6g} BHSIUNITS=1)")


def loop(p, hpk, cycles=3):
    """Last-cycle B-H loop of the fitted material at peak field hpk (A/m): (H, B)."""
    ipk = hpk * 0.10 / 100
    net = ["* B-H", f"Iin 0 1 SIN(0 {ipk:.9g} 10)", "L1 1 0 100", "K1 L1 0.9999 CORE1", "Rp 1 0 1e6",
           model_line("CORE1", p, 1.0, 10.0), f".tran 1e-4 {cycles / 10:g}", ".options timeint method=gear reltol=1e-6",
           ".print tran format=noindex n(ymin!k1_b) n(ymin!k1_h)", ".end", ""]
    with tempfile.TemporaryDirectory() as td:
        cir = os.path.join(td, "bh.cir")
        open(cir, "w").write("\n".join(net))
        subprocess.run([X.XYCE, cir], capture_output=True, text=True, timeout=300, cwd=td)
        rows = [l.split() for l in open(cir + ".prn") if l.split() and l.split()[0][0] in "0123456789-."]
    d = np.array([[float(x) for x in r] for r in rows if len(r) == 3])
    m = d[:, 0] >= (cycles - 1) / 10
    return d[m, 2], d[m, 1]


def features(p, hpk):
    H, B = loop(p, hpk)
    bpk = float(np.max(np.abs(B)))
    # coercivity: H where B crosses zero (mean of both crossings)
    hc = []
    for i in range(len(B) - 1):
        if B[i] == 0 or B[i] * B[i + 1] < 0:
            hc.append(abs(H[i] - B[i] * (H[i + 1] - H[i]) / (B[i + 1] - B[i])))
    return bpk, float(np.mean(hc)) if hc else float("nan")


def fit(variant):
    name, hc_scale, mu_scale = variant
    tg = targets(hc_scale, mu_scale)
    t0 = time.time()

    def resid(q):
        p = np.exp(q)
        try:
            b1, hc = features(p, tg["tip_3200"][0])
            b2, _ = features(p, tg["tip_10000"][0])
            b3, _ = features(p, tg["low"][0])
            r = np.array([(b1 - tg["tip_3200"][1]) / 0.02, (b2 - tg["tip_10000"][1]) / 0.02,
                          math.log(max(hc, 1e-3) / tg["Hc"]) / 0.05, math.log(max(b3, 1e-6) / tg["low"][1]) / 0.05])
        except Exception:
            return np.full(4, 1e3)
        return np.nan_to_num(r, nan=1e3, posinf=1e3, neginf=-1e3)
    best = None
    for p0 in ([200.0, 1e-4, 0.3, 40.0], [60.0, 5e-5, 0.1, 30.0], [600.0, 3e-4, 0.5, 80.0]):
        try:
            r = least_squares(resid, np.log(p0), x_scale=1.0, diff_step=1e-3, max_nfev=300)
        except Exception:
            continue
        c = float(np.sum(r.fun ** 2))
        if best is None or c < best[0]:
            best = (c, np.exp(r.x), r.fun)
    p = best[1]
    b1, hc = features(p, tg["tip_3200"][0]); b2, _ = features(p, tg["tip_10000"][0]); b3, _ = features(p, tg["low"][0])
    return dict(name=name, hc_scale=hc_scale, mu_scale=mu_scale, params=dict(zip(("A", "ALPHA", "C", "K"), map(float, p))),
                MS=MS, targets=tg, achieved=dict(B_3200=b1, B_10000=b2, Hc=hc, B_low=b3, mu_low=b3 / (MU0 * tg["low"][0])),
                cost=best[0], s=time.time() - t0)


VARIANTS = [("base", 1.0, 1.0), ("Hc x0.5", 0.5, 1.0), ("Hc x2", 2.0, 1.0), ("mu_low x0.5", 1.0, 0.5), ("mu_low x2", 1.0, 2.0)]
# second round (N28): the low-flux permeability target doubled (effective 4000 at 0.1 T, 16000 in the rolling
# direction), because with 2000 the hysteresis of the push-pull core at P_ref - 20 dB costs 2.8 dB at 30 Hz (6 dB step)
# against the linear model; the sensitivity variants around the new base
MU_BASE = 2.0
VARIANTS2 = [("base2", 1.0, MU_BASE), ("base2 Hc x0.5", 0.5, MU_BASE), ("base2 Hc x2", 2.0, MU_BASE),
             ("base2 mu_low x0.5", 1.0, MU_BASE * 0.5), ("base2 mu_low x2", 1.0, MU_BASE * 2.0)]

if __name__ == "__main__":
    t0 = time.time()
    second = "--round2" in sys.argv
    fn = os.path.join(HERE, "data", "core", "ja_fit.json")
    with ProcessPoolExecutor(5) as ex:
        res = list(ex.map(fit, VARIANTS2 if second else VARIANTS))
    os.makedirs(os.path.join(HERE, "data", "core"), exist_ok=True)
    if second:                                   # keep the first round, add / replace the second
        old = [r for r in json.load(open(fn)) if not r["name"].startswith("base2")]
        json.dump(old + res, open(fn, "w"), indent=1)
    else:
        json.dump(res, open(fn, "w"), indent=1)
    for r in res:
        a, tg = r["achieved"], r["targets"]
        print(f"{r['name']:<12} A {r['params']['A']:8.2f} ALPHA {r['params']['ALPHA']:.3e} C {r['params']['C']:.3f} K {r['params']['K']:7.2f} | "
              f"B(3200) {a['B_3200']:.3f} (1.88)  B(10000) {a['B_10000']:.3f} (1.94)  Hc {a['Hc']:5.1f} ({tg['Hc']:.0f})  "
              f"mu_low {a['mu_low']:6.0f} ({0.1 / (MU0 * tg['low'][0]):.0f}) | {r['s']:.0f} s")
    print(f"total {time.time() - t0:.0f} s")


# ---- geometry (N28: 0.7 T at rated power and 20 Hz; 5-20 at 30 Hz) and the measured inductance ----------------

GEOM = {"set-2a3": dict(N=1237, area=12.2, path=19.2, raa=2500.0, idc=0.060, lp=10.0),
        "set-300b": dict(N=1154, area=18.1, path=22.8, raa=3000.0, idc=0.060, lp=15.0),
        "set-845": dict(N=2958, area=28.5, path=30.0, raa=11000.0, idc=0.080, lp=30.0),
        "pp-el34-ul": dict(N=2222, area=18.1, path=22.8, raa=7000.0, idc=0.0, lp=100.0)}


def inductance(p, g, gap_cm, f=50.0, b_ac=0.01, cycles=6):
    """Incremental primary inductance (H) at the DC bias idc, small 50 Hz signal of b_ac (T) peak, from a Xyce
    transient: L = V / (2 pi f I) on the last cycle."""
    A_m2, path_m = g["area"] * 1e-4, g["path"] * 1e-2
    i_ac = b_ac * A_m2 * g["N"] / g["lp"]              # first guess from the target inductance
    # the DC bias is ramped up first: Xyce's Jiles-Atherton state only builds up in the transient (a DC operating
    # point leaves the core unmagnetised), and a step makes the magnetisation equation fail to converge. The AC
    # runs from t = 0: a delayed start is a slope step, which fails at high flux (0.5 T) the same way
    t_on = 0.05
    net = ["* L test", f"Idc 0 1 PWL(0 0 {t_on:g} {g['idc']:.9g})", f"Iac 0 1 SIN(0 {i_ac:.9g} {f:g})",
           f"L1 1 0 {g['N']}", "K1 L1 0.9999 CM", "Rp 1 0 1e9", model_line("CM", p, g["area"], g["path"], gap_cm),
           f".tran {1 / f / 400:.6g} {t_on + 0.01 + cycles / f:.6g}",
           ".options timeint method=gear reltol=1e-5", ".print tran format=noindex v(1) n(ymin!k1_b)", ".end", ""]
    with tempfile.TemporaryDirectory() as td:
        cir = os.path.join(td, "l.cir")
        open(cir, "w").write("\n".join(net))
        subprocess.run([X.XYCE, cir], capture_output=True, text=True, timeout=300, cwd=td)
        d = np.array([[float(x) for x in l.split()] for l in open(cir + ".prn") if l.split() and l.split()[0][0] in "0123456789-." and len(l.split()) == 3])
    m = d[:, 0] >= t_on + 0.01 + (cycles - 2.5) / f       # the last two whole cycles (and a margin)
    t, v, b = d[m].T
    import pp_core_lf
    tu, vu = pp_core_lf.uniform(t, v, f)     # Xyce's time steps vary: resample before the Fourier sum
    vamp = 2 * abs(np.mean(vu * np.exp(-2j * np.pi * f * tu)))
    return vamp / (2 * math.pi * f * i_ac), float(np.mean(b)), float(np.max(b) - np.mean(b))


RATED_W = {"set-2a3": 4.83, "set-300b": 5.61, "set-845": 25.1, "pp-el34-ul": 20.0}
B_OK, B_DEEP = 1.5, 1.7      # T: differential permeability about 10 % / 3.5 % of its peak on the fitted curve


def best_gap(p, g, N):
    """Air gap (cm) that maximises the incremental inductance at the DC bias, for N turns; returns (L, gap, Bdc)."""
    gg = dict(g, N=N)
    best = None
    lo, hi = math.log(0.002), math.log(0.3)
    grid = np.exp(np.linspace(lo, hi, 9))
    vals = [(C_ind(p, gg, x), x) for x in grid]
    i = int(np.argmax([v[0][0] for v in vals]))
    a, b = grid[max(i - 1, 0)], grid[min(i + 1, len(grid) - 1)]
    for _ in range(8):                      # golden-section refinement on log(gap)
        x1, x2 = math.exp(math.log(a) + 0.382 * (math.log(b) - math.log(a))), math.exp(math.log(a) + 0.618 * (math.log(b) - math.log(a)))
        if C_ind(p, gg, x1)[0] > C_ind(p, gg, x2)[0]:
            b = x2
        else:
            a = x1
    x = math.sqrt(a * b)
    L, bdc, _ = C_ind(p, gg, x)
    return L, x, bdc


def C_ind(p, g, gap):
    return inductance(p, g, gap)


def design_se(name, p):
    """Turns so that the inductance at the optimum gap equals the target; the flux check at 20 Hz rated power."""
    g = GEOM[name]
    lo, hi = g["N"] / 3, 6 * g["N"]
    for _ in range(12):
        N = math.sqrt(lo * hi)
        L, _, _ = best_gap(p, g, N)
        lo, hi = (N, hi) if L < g["lp"] else (lo, N)
    N = round(math.sqrt(lo * hi))
    L, gap, bdc = best_gap(p, g, N)
    vpk = math.sqrt(2 * RATED_W[name] * g["raa"])
    bac = vpk / (2 * math.pi * 20.0 * N * g["area"] * 1e-4)
    return dict(name=name, N=N, gap_mm=gap * 10, L=L, Bdc=bdc, Bac_20Hz_rated=bac, total=bdc + bac,
                verdict="OK" if bdc + bac <= B_OK else ("entering saturation" if bdc + bac <= B_DEEP else "deep saturation"))
