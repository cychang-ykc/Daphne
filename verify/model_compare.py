"""Koren against two extended tube models, on the same digitized curves with the same fitting procedure.

Models (implemented from the published equations; the parameters are fitted to verify/data/tubes only):
  koren-triode    N. Koren 1996 / 2003 (koren_fit.py)
  dempwolf        K. Dempwolf, U. Zoelzer, "A physically-motivated triode model for circuit simulations",
                  DAFx-11 (2011), eq. (10): Ik = G (ln(1 + exp(C (Va / mu + Vg))) / C)^gamma. With the grid
                  negative the grid current (eq. 11) is negligible, so Ia = Ik: parameters G, C, mu, gamma.
  koren-pentode   N. Koren (koren_fit.py)
  derk, derkE     D. Reefman, "Spice models for vacuum tubes using the uTracer" (2019), sec. 4.4 / 4.5:
                  I_Koren = E1^x, E1 = (Vg2 / kp) ln(1 + exp(kp (1/mu + Vg / sqrt(kVB + Vg2^2))))  (eq. 15)
                  Ia = I_Koren (1/kg1 - 1/kg2 + A Va / kg1 - knee(Va) (alpha / kg1 + alpha_s / kg2))  (eq. 25 / 30)
                  with Ia(0) = 0 (eq. 26), which for the anode current alone reduces to
                  Ia = I_Koren (K (1 - knee(Va)) + D Va),  knee = 1 / (1 + beta Va) (derk) or exp(-(beta Va)^1.5) (derkE)
                  K = 1/kg1 - 1/kg2, D = A / kg1: parameters mu, x, kp, kVB, K, beta, D (the screen-current split
                  needs screen-current curves, which these datasheets do not give for every graph).

Fitting: as koren_fit.fit (least squares on the plate curves, residual / I_max, soft-L1, several starting points,
the best plain RMS wins). "+ss" fits the 300B rp / gm graphs jointly (relative error, weight 0.05: plan C).

  python model_compare.py            300B, EF86, EL34
  python model_compare.py --final    every tube: the chosen model (source.json "fit") -> <tube>/model.json
"""
import math, os, sys
import numpy as np
from scipy.optimize import least_squares

import koren_fit as K

np.seterr(all="ignore")


# ---- models: Ia in A from (p, ep, eg, eg2) ------------------------------------------------------------------

def dempwolf(p, ep, eg, eg2=None):
    G, C, mu, gamma = p
    v = np.logaddexp(0.0, C * (ep / mu + eg)) / C
    return G * np.power(np.maximum(v, 0.0), gamma)


def koren_current(p4, eg2, eg):
    mu, x, kp, kvb = p4
    e1 = eg2 / kp * np.logaddexp(0.0, kp * (1.0 / mu + eg / np.sqrt(kvb + eg2 * eg2)))
    return np.power(np.maximum(e1, 0.0), x)


def derk(p, ep, eg, eg2):
    mu, x, kp, kvb, k, beta, d = p
    knee = 1.0 / (1.0 + beta * ep)
    return koren_current((mu, x, kp, kvb), eg2, eg) * (k * (1.0 - knee) + d * ep)


def derk_e(p, ep, eg, eg2):
    mu, x, kp, kvb, k, beta, d = p
    knee = np.exp(-np.power(np.maximum(beta * ep, 0.0), 1.5))
    return koren_current((mu, x, kp, kvb), eg2, eg) * (k * (1.0 - knee) + d * ep)


def koren_t(p, ep, eg, eg2=None):
    return K.triode(p, ep, eg)


def koren_p(p, ep, eg, eg2):
    return K.pentode(p, ep, eg, eg2)


# parameters fitted in log space (positive) except the ones listed as linear
MODELS = {
    "koren-triode": (koren_t, K.NAMES, ()),
    "dempwolf": (dempwolf, ("G", "C", "MU", "GAMMA"), ()),
    "koren-pentode": (koren_p, K.NAMES, ()),
    "derk": (derk, ("MU", "X", "KP", "KVB", "K", "BETA", "D"), (6,)),
    # anode current as "derk"; the screen-current parameters G2 = 1 / kg2 and AS (derk_screen) ride along
    "derk+screen": (lambda p, ep, eg, eg2: derk(p[:7], ep, eg, eg2), ("MU", "X", "KP", "KVB", "K", "BETA", "D", "G2", "AS"), (6,)),
    "derkE": (derk_e, ("MU", "X", "KP", "KVB", "K", "BETA", "D"), (6,)),
}


def ia(name, p, c):
    f = MODELS[name][0]
    eg2 = c.get("eg2")
    if isinstance(eg2, str):   # triode connected
        eg2 = c["ep"]
    return f(p, c["ep"], c["eg"], eg2)


def unpack(name, q):
    lin = MODELS[name][2]
    return np.array([q[i] if i in lin else math.exp(min(max(q[i], -60.0), 60.0)) for i in range(len(q))])


def pack(name, p):
    lin = MODELS[name][2]
    return np.array([p[i] if i in lin else math.log(p[i]) for i in range(len(p))])


def small_signal(name, p, ep, eg, eg2=None):
    def f(a, b):
        return float(ia(name, p, {"ep": np.array([a]), "eg": b, "eg2": eg2})[0])
    hv, hg = 1e-3 * max(abs(ep), 1.0), 1e-3 * max(abs(eg), 1.0)
    dp = (f(ep + hv, eg) - f(ep - hv, eg)) / (2 * hv)
    dg = (f(ep, eg + hg) - f(ep, eg - hg)) / (2 * hg)
    rp = 1.0 / dp if dp > 0 else float("inf")
    return {"ip_mA": f(ep, eg) * 1e3, "mu": dg * rp, "rp_ohm": rp, "gm_umho": dg * 1e6}


# ---- starting points --------------------------------------------------------------------------------------

def starts(name, curves, seed):
    """seed: the Koren fit of the same tube (triode or pentode parameters)."""
    if name in ("koren-triode", "koren-pentode"):
        yield from K.starts("triode" if name == "koren-triode" else "pentode")
        return
    if name == "dempwolf":
        mu = seed[0]
        ep = np.concatenate([c["ep"] for c in curves]); eg = np.concatenate([np.full(len(c["ep"]), c["eg"]) for c in curves])
        ip = np.concatenate([c["ip"] for c in curves])
        for gamma in (1.3, 1.5):
            for C in (0.03, 0.1, 0.3, 1.0, 3.0):
                veff = np.maximum(ep / mu + eg, 1e-3)
                sel = veff > 0.2 * veff.max()
                G = float(np.median(ip[sel] / veff[sel] ** gamma))
                yield np.array([G, C, mu, gamma])
        return
    mu, x, kg1, kp, kvb = seed   # Koren pentode
    for kvb_d in (1.0, 100.0):
        for bscale in (0.3, 1.0, 3.0):
            for d in (0.0, 1e-4 * math.pi / kg1):
                yield np.array([mu, x, kp, kvb_d, math.pi / kg1, bscale / kvb, d])


# ---- fitting ----------------------------------------------------------------------------------------------

def fit(name, curves, ss=None, boost=None):
    """boost: (below_mA, factor): plate-curve residuals below that current weigh factor times more (the
    absolute-current residual otherwise gives the region near cutoff little weight)."""
    ep = np.concatenate([c["ep"] for c in curves])
    ip = np.concatenate([c["ip"] for c in curves])
    eg = np.concatenate([np.full(len(c["ep"]), c["eg"]) for c in curves])
    eg2 = np.concatenate([np.full(len(c["ep"]), c.get("eg2") or 0.0) for c in curves])
    allc = {"ep": ep, "eg": eg, "eg2": eg2}
    imax = float(ip.max())
    seed = KOREN_SEED
    w = np.full(len(ip), 1.0 / imax)
    if boost:
        w = np.where(ip < boost[0] * 1e-3, boost[1], 1.0) / imax

    def resid(q):
        p = unpack(name, q)
        r = (ia(name, p, allc) - ip) * w
        if ss:
            extra = []
            for what, eb, g, v in ss:
                m = small_signal(name, p, eb, g)
                extra.append((m["rp_ohm" if what == "rp" else "gm_umho"] - v) / v)
            r = np.r_[r, 0.05 * np.array(extra)]
        return np.nan_to_num(r, nan=1e3, posinf=1e3, neginf=-1e3)

    best = None
    for p0 in starts(name, curves, seed):
        try:
            r = least_squares(resid, pack(name, p0), loss="soft_l1", f_scale=0.01, x_scale="jac", max_nfev=4000)
        except (ValueError, FloatingPointError, OverflowError):
            continue
        cost = float(np.sum(resid(r.x) ** 2))
        if np.isfinite(cost) and (best is None or cost < best[0]):
            best = (cost, r.x, r.status)
    p = unpack(name, best[1])
    return p, imax


def plate_errors(name, p, curves, imax):
    e = np.concatenate([(ia(name, p, c) - c["ip"]) * 1e3 for c in curves])
    return float(np.sqrt(np.mean(e * e))), float(np.abs(e).max()), float(np.sqrt(np.mean(e * e)) / (imax * 1e3) * 100)


def op_errors(name, p, src):
    out = []
    for op in src.get("operating_points", []):
        if "mu" not in op and "rp_ohm" not in op:
            continue
        m = small_signal(name, p, op["ep_V"], op["eg_V"], op.get("eg2_V"))
        row = {}
        for key in ("mu", "rp_ohm", "gm_umho", "ip_mA"):
            if op.get(key) is not None:
                row[key] = (m[key] - op[key]) / op[key] * 100
        out.append((op["name"], row))
    return out


def graph_checks(tube, name, p, src):
    """300B rp / gm graphs: worst error per current band (as koren_fit.check_curves)."""
    d = os.path.join(K.DATA, tube)
    imax = src["plot"]["ip_max_mA"]
    res = {}
    for fn in sorted(os.listdir(d)):
        m = K.CHECK.match(fn)
        if not m or m.group(1) not in ("rp", "gm"):
            continue
        what, eb = m.group(1), float(m.group(2))
        for g, v in np.loadtxt(os.path.join(d, fn), delimiter=",", skiprows=1, ndmin=2):
            mm = small_signal(name, p, eb, g)
            err = (mm["rp_ohm" if what == "rp" else "gm_umho"] - v) / v * 100
            band = next(b[2] for b in K.BANDS if b[0] * imax <= mm["ip_mA"] < b[1] * imax)
            cur = res.setdefault((what, band), [0.0, []])
            cur[1].append(err)
    return {k: (max(v[1], key=abs), float(np.sqrt(np.mean(np.square(v[1]))))) for k, v in res.items()}


def ss_data(tube):
    d = os.path.join(K.DATA, tube)
    out = []
    for fn in os.listdir(d):
        m = K.CHECK.match(fn)
        if m and m.group(1) in ("rp", "gm"):
            for g, v in np.loadtxt(os.path.join(d, fn), delimiter=",", skiprows=1, ndmin=2):
                out.append((m.group(1), float(m.group(2)), g, v))
    return out


KOREN_SEED = None


def compare(tube, names, joint=False):
    global KOREN_SEED
    src, kind, curves = K.load(tube)
    KOREN_SEED, _ = K.fit(kind, curves)
    rows = []
    for name in names:
        for use_ss in ((False, True) if joint else (False,)):
            p, imax = fit(name, curves, ss_data(tube) if use_ss else None)
            rms, mx, pct = plate_errors(name, p, curves, imax)
            rows.append((name + (" +ss" if use_ss else ""), p, rms, mx, pct, op_errors(name, p, src),
                         graph_checks(tube, name, p, src) if joint else None))
    print(f"\n== {tube} ({len(curves)} curves)")
    for label, p, rms, mx, pct, ops, gc in rows:
        print(f"  {label:<18} plate rms {rms:7.3f} mA ({pct:4.2f} %)  max {mx:7.2f} mA   params "
              + " ".join(f"{v:.4g}" for v in p))
        for opname, r in ops:
            print(f"      {opname:<34} " + "  ".join(f"{k} {v:+6.1f} %" for k, v in r.items()))
        if gc:
            for key in (("gm", "low"), ("gm", "mid"), ("rp", "low"), ("rp", "mid"), ("gm", "high"), ("rp", "high")):
                if key in gc:
                    w, r = gc[key]
                    print(f"      {key[0]} {key[1]:<4}: worst {w:+7.1f} %  rms {r:5.1f} %")
    return rows


REFS = {
    "koren-triode": "N. Koren, Improved vacuum tube models for SPICE simulations (1996 / 2003), triode equations",
    "derk": "D. Reefman, Spice models for vacuum tubes using the uTracer (2019), sec. 4.4, eq. 15, 16, 25 - 27 "
            "(anode current; implemented from the equations, parameters fitted to verify/data/tubes)",
    "derk+screen": "D. Reefman, Spice models for vacuum tubes using the uTracer (2019), sec. 4.4, eq. 15, 16, 23, "
                   "25 - 27 (anode and screen current, AS from eq. 27; implemented from the equations, parameters fitted to "
                   "verify/data/tubes)",
}


def derk_screen(p, ep, eg, eg2):
    """Screen current of the Derk model (Reefman eq. 23): I_K G2 (1 + AS / (1 + beta Va)), G2 = 1 / kg2."""
    mu, x, kp, kvb, k, beta, d, g2, a_s = p
    return koren_current((mu, x, kp, kvb), eg2, eg) * g2 * (1.0 + a_s / (1.0 + beta * ep))


ALPHA_FIXED = None   # Reefman: alpha is "not a fitting parameter", a very small reduction; None = fitted


def fit_derk_screen(tube, curves, boost):
    """Derk anode and screen current fitted together: the plate curves (anode current), and the transfer graphs
    tr_eb<Va>_vg2<Vg2>.csv (anode current vs Eg) and sc_eb<Va>_vg2<Vg2>.csv (screen current vs Eg): Philips EL34
    sheet A at Vg2 = 250 / 375 / 425 V, which also reach the screen voltages of the ultralinear stages; and the
    triode-connected curves pt_vg<Vg1>_tri.csv (Ia + Ig2 at Vg2 = Va, Mullard C4), the only data on how the
    current divides at low Va, where the ultralinear stage swings at full power. Same
    weighting as the plate fit (residual / I_max, times boost[1] below boost[0] mA)."""
    import re
    d = os.path.join(K.DATA, tube)
    A, S = [], []   # (ep, eg, eg2, i) arrays: anode, screen
    for c in curves:
        A.append((c["ep"], np.full(len(c["ep"]), c["eg"]), np.full(len(c["ep"]), c["eg2"]), c["ip"]))
    for fn in sorted(os.listdir(d)):
        m = re.match(r"^(tr|sc)_eb(\d+)_vg2(\d+)\.csv$", fn)
        if not m:
            continue
        a = np.loadtxt(os.path.join(d, fn), delimiter=",", skiprows=1, ndmin=2)
        a = a[a[:, 1] >= 1.0]
        row = (np.full(len(a), float(m.group(2))), a[:, 0], np.full(len(a), float(m.group(3))), a[:, 1] * 1e-3)
        (A if m.group(1) == "tr" else S).append(row)
    T = []   # triode connected (Mullard C4, page 10): Ia + Ig2 at Vg2 = Va, down to low Va
    for fn in sorted(os.listdir(d)):
        m = re.match(r"^pt_vg(-?\d+(?:\.\d+)?)_tri\.csv$", fn)
        if m:
            a = np.loadtxt(os.path.join(d, fn), delimiter=",", skiprows=1, ndmin=2)
            a = a[a[:, 1] >= 1.0]
            T.append((a[:, 0], np.full(len(a), float(m.group(1))), a[:, 0], a[:, 1] * 1e-3))
    cat = lambda L, i: np.concatenate([r[i] for r in L])
    ea, ga, g2a, ia_ = (cat(A, i) for i in range(4))
    es, gs, g2s, is_ = (cat(S, i) for i in range(4))
    et, gt, g2t, it_ = (cat(T, i) for i in range(4)) if T else (np.zeros(0),) * 4
    imax = float(max(c["ip"].max() for c in curves))
    wa = np.where(ia_ < boost[0] * 1e-3, boost[1], 1.0) / imax
    ws = np.where(is_ < boost[0] * 1e-3, boost[1], 1.0) / imax
    wt = np.where(it_ < boost[0] * 1e-3, boost[1], 1.0) / imax
    lin = (6,)

    def unpack9(q):
        """q: 7 anode parameters, log G2, logit alpha. Reefman eq. 26 / 27 (the anode current is zero at Va = 0)
        ties the screen current to the anode parameters: AS G2 = K - alpha (K + G2), alpha in [0, 1) the small fall
        of the total current at low Va. Fitting AS freely lets the high-Va data of sheet A drop it to zero."""
        p7 = [q[i] if i in lin else math.exp(min(max(q[i], -60.0), 60.0)) for i in range(7)]
        g2 = math.exp(min(max(q[7], -60.0), 60.0))
        alpha = ALPHA_FIXED if ALPHA_FIXED is not None else 1.0 / (1.0 + math.exp(-min(max(q[8], -60.0), 60.0)))
        a_s = (p7[4] - alpha * (p7[4] + g2)) / g2
        return np.array(p7 + [g2, a_s])

    def resid(q):
        p = unpack9(q)
        r = np.r_[(derk(p[:7], ea, ga, g2a) - ia_) * wa, (derk_screen(p, es, gs, g2s) - is_) * ws,
                  (derk(p[:7], et, gt, g2t) + derk_screen(p, et, gt, g2t) - it_) * wt]
        return np.nan_to_num(r, nan=1e3, posinf=1e3, neginf=-1e3)

    best = None
    for p0 in starts("derk", curves, KOREN_SEED):
        for g2 in (p0[4] * 0.1, p0[4] * 0.3):
            for alpha in (0.02, 0.1, 0.3):
                q0 = np.r_[[math.log(v) if i not in lin else v for i, v in enumerate(p0)], math.log(g2), math.log(alpha / (1 - alpha))]
                try:
                    r = least_squares(resid, q0, loss="soft_l1", f_scale=0.01, x_scale="jac", max_nfev=3000)
                except (ValueError, FloatingPointError, OverflowError):
                    continue
                c = float(np.sum(resid(r.x) ** 2))
                if np.isfinite(c) and (best is None or c < best[0]):
                    best = (c, r.x)
    p = unpack9(best[1])
    rep = {}
    for lab, L, fn_ in (("anode (plate + sheet A)", A, lambda p, e, g, g2: derk(p[:7], e, g, g2)), ("screen (sheet A)", S, derk_screen),
                        ("triode connected Ia + Ig2 (Mullard C4)", T, lambda p, e, g, g2: derk(p[:7], e, g, g2) + derk_screen(p, e, g, g2))):
        if not L:
            continue
        e = np.concatenate([(fn_(p, r[0], r[1], r[2]) - r[3]) * 1e3 for r in L])
        rep[lab] = (float(np.sqrt(np.mean(e * e))), float(np.abs(e).max()))
    return p, imax, rep


def final(tube):
    """The model chosen for the tube (source.json "fit"), fitted as decided, written to <tube>/model.json: the
    parameters the SPICE references and the program use."""
    global KOREN_SEED
    import json
    src, kind, curves = K.load(tube)
    f = src.get("fit") or {"model": "koren-triode" if kind == "triode" else "derk"}
    KOREN_SEED, _ = K.fit(kind, curves)
    name = f["model"]
    extra = {}
    if name == "koren-m":
        import triode_models as T
        seed = KOREN_SEED
        p, pct = T.fit("koren-m", curves, seed, ss_data(tube) if f.get("joint_rp_gm") else None, f.get("m"))
        e = np.concatenate([(T.current("koren-m", p, c["ep"], np.full(len(c["ep"]), c["eg"]))[0] - c["ip"]) * 1e3 for c in curves])
        imax = max(float(c["ip"].max()) for c in curves)
        out = {"tube": tube, "model": name, "params": dict(zip(T.NAMES[name], map(float, p))), "fit": f,
               "equations": "N. Koren, Improved vacuum tube models for SPICE simulations (1996 / 2003), triode equations, "
                            "with the softplus knee width scaled as Va^M / KP (triode_models.py; M = 1 is Koren)",
               "plate_rms_mA": float(np.sqrt(np.mean(e * e))), "plate_rms_pct": pct, "plate_max_mA": float(np.abs(e).max()),
               "operating_points": T.op_errors(name, p, src)}
        with open(os.path.join(K.DATA, tube, "model.json"), "w", encoding="utf-8") as h:
            json.dump(out, h, indent=2, ensure_ascii=False)
            h.write(chr(10))
        print(f"{tube}: {name} m {p[5]:.2f}  plate rms {pct:.2f} %  " +
              " | ".join(f"{n}: " + " ".join(f"{k} {v:+.1f}%" for k, v in r.items()) for n, r in out["operating_points"].items()))
        return name, p
    if name == "derk+screen":
        global ALPHA_FIXED
        ALPHA_FIXED = f.get("alpha")
        p, imax, extra = fit_derk_screen(tube, curves, tuple(f.get("boost") or (0, 1)))
    else:
        p, imax = fit(name, curves, ss_data(tube) if f.get("joint_rp_gm") else None, tuple(f["boost"]) if f.get("boost") else None)
    rms, mx, pct = plate_errors(name, p, curves, imax)
    out = {"tube": tube, "model": name, "params": dict(zip(MODELS[name][1], map(float, p))), "fit": f,
           "equations": REFS[name], "plate_rms_mA": rms, "plate_rms_pct": pct, "plate_max_mA": mx,
           "operating_points": {n: r for n, r in op_errors(name, p, src)}, "fit_errors_mA": extra}
    with open(os.path.join(K.DATA, tube, "model.json"), "w", encoding="utf-8") as h:
        json.dump(out, h, indent=2, ensure_ascii=False)
        h.write("\n")
    if extra:
        print(f"{tube}: rms / max mA: " + "; ".join(f"{k} {v[0]:.2f} / {v[1]:.1f}" for k, v in extra.items()))
    print(f"{tube}: {name} {f}  plate rms {pct:.2f} % (max {mx:.2f} mA)  " +
          " | ".join(f"{n}: " + " ".join(f"{k} {v:+.1f}%" for k, v in r.items()) for n, r in op_errors(name, p, src)))
    return name, p


if __name__ == "__main__":
    if "--final" in sys.argv:
        for t in [a for a in sys.argv[1:] if not a.startswith("-")] or sorted(
                d for d in os.listdir(K.DATA) if os.path.isfile(os.path.join(K.DATA, d, "source.json"))):
            final(t)
        sys.exit(0)
    which = sys.argv[1:] or ["300B", "EF86", "EL34"]
    for t in which:
        if t == "300B":
            compare(t, ["koren-triode", "dempwolf"], joint=True)
        else:
            compare(t, ["koren-pentode", "derk", "derkE"])
