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

def fit(name, curves, ss=None):
    ep = np.concatenate([c["ep"] for c in curves])
    ip = np.concatenate([c["ip"] for c in curves])
    eg = np.concatenate([np.full(len(c["ep"]), c["eg"]) for c in curves])
    eg2 = np.concatenate([np.full(len(c["ep"]), c.get("eg2") or 0.0) for c in curves])
    allc = {"ep": ep, "eg": eg, "eg2": eg2}
    imax = float(ip.max())
    seed = KOREN_SEED

    def resid(q):
        p = unpack(name, q)
        r = (ia(name, p, allc) - ip) / imax
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


if __name__ == "__main__":
    which = sys.argv[1:] or ["300B", "EF86", "EL34"]
    for t in which:
        if t == "300B":
            compare(t, ["koren-triode", "dempwolf"], joint=True)
        else:
            compare(t, ["koren-pentode", "derk", "derkE"])
