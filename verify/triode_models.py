"""Triode plate-current models with analytic partial derivatives, their fits and SPICE lines (N28, the H3 question).

  koren     Ia = 2 E1^x / kg1,  E1 = Va/kp ln(1 + exp(kp (1/mu + Vg / s))),  s = sqrt(kvb + Va^2)
            N. Koren, Improved vacuum tube models for SPICE simulations (1996 / 2003). Reefman's ExtractModel
            (Spice models for vacuum tubes using the uTracer, 2019, sec. 3.1) uses this same triode form.
  dempwolf  Ia = G (ln(1 + exp(C (Va/mu + Vg))) / C)^gamma
            K. Dempwolf, U. Zoelzer, A Physically-motivated Triode Model for Circuit Simulations (DAFx-11)
  koren-m   Koren with one curvature parameter m: the width of the softplus knee scales as Va^m / kp,
            E1 = w ln(1 + exp(u / w)),  w = Va^m / kp,  u = Va/mu + Vg Va / s.
            m = 1 is Koren exactly; m = 0 is a constant knee width as in Dempwolf. (Our own extension, N28.)

Each model returns (Ia, dIa/dVa, dIa/dVg) for Va > 0, so rp and gm in the joint rp / gm fits are analytic: the
earlier joint fits differentiated numerically, one scalar call per point.
"""
import math, time
import numpy as np
from scipy.optimize import least_squares

NAMES = {"koren": ("MU", "EX", "KG1", "KP", "KVB"), "dempwolf": ("G", "C", "MU", "GAMMA"),
         "koren-m": ("MU", "EX", "KG1", "KP", "KVB", "M")}
LINEAR = {"koren-m": (5,)}          # fitted linearly (m may be 0); the rest in log space


def _sp(z):
    return np.logaddexp(0.0, z)


def _sig(z):
    return 0.5 * (1.0 + np.tanh(0.5 * z))


def current(model, p, va, vg):
    va = np.maximum(np.asarray(va, float), 1e-6); vg = np.asarray(vg, float)
    if model == "dempwolf":
        G, C, mu, g = p
        u = va / mu + vg
        v = _sp(C * u) / C; s = _sig(C * u)
        ia = G * v ** g
        di = G * g * v ** (g - 1) * s
        return ia, di / mu, di
    if model == "koren":
        mu, x, kg1, kp, kvb = p; m = 1.0
    else:
        mu, x, kg1, kp, kvb, m = p
    s = np.sqrt(kvb + va * va)
    u = va / mu + vg * va / s
    w = va ** m / kp
    z = u / w
    e1 = w * _sp(z); sg = _sig(z)
    du_dva = 1.0 / mu + vg * kvb / s ** 3
    dw_dva = m * va ** (m - 1) / kp
    de_dva = dw_dva * (_sp(z) - sg * z) + sg * du_dva
    de_dvg = sg * va / s
    ia = 2.0 * e1 ** x / kg1
    di = 2.0 * x * e1 ** (x - 1) / kg1
    return ia, di * de_dva, di * de_dvg


def small_signal(model, p, va, vg):
    ia, dva, dvg = current(model, p, va, vg)
    rp = 1.0 / dva
    return {"ip_mA": float(ia) * 1e3, "mu": float(dvg * rp), "rp_ohm": float(rp), "gm_umho": float(dvg) * 1e6}


def _pack(model, p):
    lin = LINEAR.get(model, ())
    return np.array([v if i in lin else math.log(v) for i, v in enumerate(p)])


def _unpack(model, q):
    lin = LINEAR.get(model, ())
    return np.array([v if i in lin else math.exp(min(v, 700.0)) for i, v in enumerate(q)])


def _starts(model, seed_koren, m_fixed):
    mus, exs, kps = (3.0, 8.0, 20.0, 60.0), (1.3, 1.5), (30.0, 300.0)
    if model == "dempwolf":
        for gamma in (1.3, 1.5):
            for C in (0.03, 0.1, 0.3, 1.0, 3.0):
                yield [None, C, seed_koren[0], gamma]          # G from the data (filled in by fit)
        return
    ms = (m_fixed,) if m_fixed is not None else (0.0, 0.5, 1.0)
    for mu in mus + (seed_koren[0],):
        for ex in exs:
            for kp in kps:
                for m in (ms if model == "koren-m" else (None,)):
                    p = [mu, ex, 1000.0, kp, 300.0]
                    yield p if model == "koren" else p + [m]


def fit(model, curves, seed_koren, ss=None, m_fixed=None):
    """Absolute residual / Imax, soft_l1, multistart (as model_compare). ss: datasheet rp / gm points
    [(what, Va, Vg, value)], weight 0.05 each as before. m_fixed: koren-m with m held."""
    ep = np.concatenate([c["ep"] for c in curves]); ip = np.concatenate([c["ip"] for c in curves])
    eg = np.concatenate([np.full(len(c["ep"]), c["eg"]) for c in curves])
    imax = float(ip.max())
    ss = ss or []
    sva = np.array([s[1] for s in ss]); svg = np.array([s[2] for s in ss])
    swhat = np.array([s[0] == "rp" for s in ss], bool); sval = np.array([s[3] for s in ss])

    def full(q):
        p = _unpack(model, q)
        if m_fixed is not None:
            p[5] = m_fixed
        return p

    def resid(q):
        p = full(q)
        with np.errstate(all="ignore"):
            r = (current(model, p, ep, eg)[0] - ip) / imax
            if ss:
                _, dva, dvg = current(model, p, sva, svg)
                model_v = np.where(swhat, 1.0 / dva, dvg * 1e6)
                r = np.r_[r, 0.05 * (model_v - sval) / sval]
        return np.nan_to_num(r, nan=1e3, posinf=1e3, neginf=-1e3)

    best = None
    for p0 in _starts(model, seed_koren, m_fixed):
        if model == "dempwolf":
            G0 = float(np.median(ip[ip > 0.2 * imax] / (np.maximum(ep[ip > 0.2 * imax] / p0[2] + eg[ip > 0.2 * imax], 1e-3) ** p0[3])))
            p0 = [G0] + p0[1:]
        q0 = _pack(model, p0)
        try:
            r = least_squares(resid, q0, loss="soft_l1", f_scale=0.01, x_scale="jac", max_nfev=4000)
        except (ValueError, FloatingPointError, OverflowError):
            continue
        c = float(np.sum(resid(r.x) ** 2))
        if np.isfinite(c) and (best is None or c < best[0]):
            best = (c, r.x)
    p = full(best[1])
    e = (current(model, p, ep, eg)[0] - ip) * 1e3
    return p, float(np.sqrt(np.mean(e * e)) / (imax * 1e3) * 100)


def op_errors(model, p, src):
    """Model against the datasheet's typical operating points (percent)."""
    out = {}
    for op in src.get("operating_points", []):
        m = small_signal(model, p, op["ep_V"], op["eg_V"])
        out[op["name"]] = {k: (m[k] - op[k]) / op[k] * 100 for k in ("mu", "rp_ohm", "gm_umho", "ip_mA") if op.get(k)}
    return out


def spice_line(name, model, p, a, g, k, f, pos, softplus):
    """B source for spice_ref-style netlists (f: number format, pos / softplus: the smooth helpers)."""
    va, vg = f"v({a},{k})", f"v({g},{k})"
    if model == "dempwolf":
        G, C, mu, gamma = (f(v) for v in p)
        return f"B{name} {a} {k} I = {G}*exp({gamma}*ln({softplus(f'{C}*({va}/{mu}+{vg})')}/{C}+1e-30))"
    mu, x, kg1, kp, kvb = (f(v) for v in p[:5])
    m = f(p[5]) if model == "koren-m" else "1"
    vap = pos(va)
    w = f"(exp({m}*ln({vap}+1e-12))/{kp})"
    u = f"({vap}/{mu}+{vg}*{vap}/sqrt({kvb}+{va}*{va}))"
    e1 = f"({w}*{softplus(f'{u}/{w}')}+1e-30)"
    return f"B{name} {a} {k} I = 2*exp({x}*ln({e1}))/{kg1}"


def check_derivatives():
    """Analytic derivatives against central differences."""
    rng = np.random.default_rng(1)
    cases = {"koren": [4.2, 1.54, 2400.0, 46.0, 1e-3], "dempwolf": [1e-3, 0.3, 4.0, 1.5],
             "koren-m": [4.2, 1.54, 2400.0, 46.0, 300.0, 0.4]}
    worst = 0.0
    for model, p in cases.items():
        for _ in range(50):
            va, vg = rng.uniform(50, 400), rng.uniform(-80, 0)
            ia, dva, dvg = current(model, p, va, vg)
            h = 1e-4
            nva = (current(model, p, va + h, vg)[0] - current(model, p, va - h, vg)[0]) / (2 * h)
            nvg = (current(model, p, va, vg + h)[0] - current(model, p, va, vg - h)[0]) / (2 * h)
            for a, n in ((dva, nva), (dvg, nvg)):
                if abs(n) > 1e-9:
                    worst = max(worst, abs(a - n) / abs(n))
    # koren-m with m = 1 reproduces Koren
    p = cases["koren"]
    d = np.max(np.abs(current("koren-m", p[:4] + [300.0, 1.0], np.linspace(10, 400, 50), -20)[0]
                      - current("koren", p[:4] + [300.0], np.linspace(10, 400, 50), -20)[0]))
    return worst, d


if __name__ == "__main__":
    t0 = time.time(); w, d = check_derivatives()
    print(f"analytic vs numerical derivative: worst relative difference {w:.2e}; koren-m(m=1) - koren: {d:.1e} A ({time.time() - t0:.2f} s)")
