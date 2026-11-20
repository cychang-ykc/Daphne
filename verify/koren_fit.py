"""Koren model parameters fitted to digitized datasheet plate curves (least squares).

Data: verify/data/tubes/<tube>/ (layout in verify/data/tubes/README.md)
  source.json        the datasheet (URL, page, figure), the model ("triode" | "pentode"), the graph's axis maxima ("plot"),
                     the operating points to check, Vg2 for pentodes
  vg<Vg>.csv         one plate curve per grid voltage, exported by WebPlotDigitizer: x = plate voltage, y = plate current
                     (file name: vg-40.csv, vg0.csv, vg+5.csv; pentodes at another screen voltage: vg-10_vg2-250.csv)

Models (N. Koren, "Improved vacuum tube models for SPICE simulations", 1996 / 2003):
  triode   E1 = Ep/KP * ln(1 + exp(KP * (1/MU + Eg / sqrt(KVB + Ep^2))))
           Ip = E1^EX / KG1 * (1 + sgn(E1))
  pentode  E1 = Eg2/KP * ln(1 + exp(KP * (1/MU + Eg / Eg2)))
           Ip = E1^EX / KG1 * (1 + sgn(E1)) * atan(Ep / KVB)
Volts and amperes. The parameters come only from these fits; Koren's published values are not used here
(cross-check them by hand against fit.json if wanted).

Fit: scipy least_squares (trust region, soft-L1 loss) over log-parameters, from several starting points; the
residual is (model - data) / I_max of the tube, so every curve weighs by its absolute error, as a load line sees it.

Reported per tube and per curve:
  rms    RMS error in mA          max   largest error in mA
  rms%   RMS error / I_max        n     points
The residual weighs little near cutoff, where the curvature that makes the distortion is, so the fit is also
checked at operating points (source.json "operating_points": the datasheet's typical conditions, later each
preset's): mu, rp and gm of the fitted model (central differences) against the printed values; a difference above
10 % is flagged.

Units: the CSV columns are plate voltage in V and plate current in mA. A header, when present, must say so
("Ep_V,Ip_mA"); every file is also checked against the graph's axes (source.json "plot"), so a file exported in A
or kV is refused instead of fitted 1000 times off.

  python koren_fit.py                 every tube with data, writes <tube>/fit.json
  python koren_fit.py 300B EL34       only these
  python koren_fit.py --selftest      synthetic curves with known parameters: the fit must recover them
"""
import json, math, os, re, sys
import numpy as np
from scipy.optimize import least_squares

DATA = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data", "tubes")
NAMES = ("MU", "EX", "KG1", "KP", "KVB")
CURVE = re.compile(r"^vg([+-]?\d+(?:\.\d+)?)(?:_vg2([+-]?\d+(?:\.\d+)?))?\.csv$", re.I)


def triode(p, ep, eg):
    mu, ex, kg1, kp, kvb = p
    x = kp * (1.0 / mu + eg / np.sqrt(kvb + ep * ep))
    e1 = ep / kp * np.logaddexp(0.0, x)            # ln(1 + exp(x)) without overflow
    return np.where(e1 > 0, 2.0 * np.power(np.maximum(e1, 0.0), ex) / kg1, 0.0)


def pentode(p, ep, eg, eg2):
    mu, ex, kg1, kp, kvb = p
    x = kp * (1.0 / mu + eg / eg2)
    e1 = eg2 / kp * np.logaddexp(0.0, x)
    return np.where(e1 > 0, 2.0 * np.power(np.maximum(e1, 0.0), ex) / kg1 * np.arctan(ep / kvb), 0.0)


def model(kind, p, c):
    return triode(p, c["ep"], c["eg"]) if kind == "triode" else pentode(p, c["ep"], c["eg"], c["eg2"])


# starting points (log-spaced over the range real tubes span); the best fit wins
def starts(kind):
    mus = (3.0, 8.0, 20.0, 60.0) if kind == "triode" else (5.0, 10.0, 20.0)
    for mu in mus:
        for ex in (1.3, 1.5):
            for kp in (30.0, 300.0):
                yield np.array([mu, ex, 1000.0, kp, 300.0 if kind == "triode" else 15.0])


def fit(kind, curves):
    ep = np.concatenate([c["ep"] for c in curves])
    ip = np.concatenate([c["ip"] for c in curves])
    eg = np.concatenate([np.full(len(c["ep"]), c["eg"]) for c in curves])
    eg2 = np.concatenate([np.full(len(c["ep"]), c.get("eg2") or 0.0) for c in curves])
    allc = {"ep": ep, "eg": eg, "eg2": eg2}
    imax = float(ip.max())

    def resid(lp):
        return (model(kind, np.exp(lp), allc) - ip) / imax

    best = None
    for p0 in starts(kind):
        try:
            r = least_squares(resid, np.log(p0), loss="soft_l1", f_scale=0.01, x_scale="jac", max_nfev=20000)
        except (ValueError, FloatingPointError):
            continue
        # judge by the plain RMS, not the robust loss
        cost = float(np.sqrt(np.mean(resid(r.x) ** 2)))
        if np.isfinite(cost) and (best is None or cost < best[0]):
            best = (cost, r.x)
    if best is None:
        raise SystemExit("no fit converged")
    return np.exp(best[1]), imax


def errors(kind, p, curves, imax):
    rows, alle = [], []
    for c in curves:
        e = (model(kind, p, c) - c["ip"]) * 1e3   # mA
        alle.append(e)
        rows.append({"eg": c["eg"], "eg2": c.get("eg2"), "n": len(e), "rms_mA": float(np.sqrt(np.mean(e * e))),
                     "max_mA": float(np.abs(e).max())})
    e = np.concatenate(alle)
    total = {"n": len(e), "rms_mA": float(np.sqrt(np.mean(e * e))), "max_mA": float(np.abs(e).max()),
             "rms_pct": float(np.sqrt(np.mean(e * e)) / (imax * 1e3) * 100.0)}
    return total, rows


def load(tube):
    d = os.path.join(DATA, tube)
    with open(os.path.join(d, "source.json"), encoding="utf-8") as f:
        src = json.load(f)
    kind = src["model"]
    if kind not in ("triode", "pentode"):
        raise SystemExit(f"{tube}: model must be triode or pentode")
    plot = src.get("plot")
    if not plot:
        raise SystemExit(f"{tube}: source.json needs \"plot\": {{\"ep_max_V\": .., \"ip_max_mA\": ..}} (the graph's axis maxima)")
    curves = []
    for name in sorted(os.listdir(d)):
        m = CURVE.match(name)
        if not m:
            continue
        rows = []
        with open(os.path.join(d, name), encoding="utf-8-sig") as f:
            for ln, line in enumerate(f):
                if not line.strip():
                    continue
                parts = [s.strip() for s in re.split(r"[,;\t]", line.strip())]
                try:
                    rows.append((float(parts[0]), float(parts[1])))
                except (ValueError, IndexError):
                    # a header may only name the units this script reads
                    if ln == 0 and [s.lower() for s in parts[:2]] in (["ep_v", "ip_ma"], ["x", "y"]):
                        continue
                    raise SystemExit(f"{tube}/{name}: line {ln + 1} {line.strip()!r}: expected the header Ep_V,Ip_mA "
                                     "or two numbers (plate voltage in V, plate current in mA)")
        if len(rows) < 3:
            raise SystemExit(f"{tube}/{name}: fewer than 3 points")
        a = np.array(sorted(rows))
        # against the graph's axes: a file in A or kV is 1000 times off
        for col, unit, top in ((0, "V", plot["ep_max_V"]), (1, "mA", plot["ip_max_mA"])):
            hi = float(a[:, col].max())
            if hi > 1.05 * top or hi < 0.02 * top:
                raise SystemExit(f"{tube}/{name}: column {col + 1} reaches {hi:g}, but the graph's axis ends at {top:g} {unit}: "
                                 "wrong unit? (the CSV is in V and mA)")
        eg2 = float(m.group(2)) if m.group(2) else src.get("vg2")
        if kind == "pentode" and not eg2:
            raise SystemExit(f"{tube}/{name}: a pentode curve needs Vg2 (source.json \"vg2\" or _vg2<V> in the file name)")
        curves.append({"eg": float(m.group(1)), "eg2": eg2, "ep": a[:, 0], "ip": np.maximum(a[:, 1], 0.0) * 1e-3})
    if not curves:
        print(f"{tube}: no vg*.csv yet")
    need = src.get("require_vg2")
    if need and curves:
        have = {c["eg2"] for c in curves}
        missing = [v for v in need if v not in have]
        if missing:
            print(f"{tube}: no curves at Vg2 = {', '.join(f'{v:g}' for v in missing)} V yet: not fitted "
                  "(the screen dependence needs every listed screen voltage)")
            return src, kind, []
    curves.sort(key=lambda c: (-(c["eg2"] or 0.0), -c["eg"]))
    return src, kind, curves


def small_signal(kind, p, ep, eg, eg2=None):
    """Ip (mA), mu, rp (ohm), gm (umho) of the fitted model at a bias point, by central differences.
    eg2 = "anode": a pentode triode-connected (the screen follows the plate)."""
    def ip(a, b):
        return float(model(kind, p, {"ep": np.array([a]), "eg": b, "eg2": a if eg2 == "anode" else eg2})[0])
    hv, hg = 1e-3 * max(abs(ep), 1.0), 1e-3 * max(abs(eg), 1.0)
    dp = (ip(ep + hv, eg) - ip(ep - hv, eg)) / (2 * hv)
    dg = (ip(ep, eg + hg) - ip(ep, eg - hg)) / (2 * hg)
    rp = 1.0 / dp if dp > 0 else float("inf")
    return {"ip_mA": ip(ep, eg) * 1e3, "mu": dg * rp, "rp_ohm": rp, "gm_umho": dg * 1e6}


def check_points(kind, p, src):
    """The fitted model at the operating points in source.json against the values printed there."""
    out = []
    for op in src.get("operating_points", []):
        m = small_signal(kind, p, op["ep_V"], op["eg_V"], op.get("eg2_V"))
        if kind == "pentode":
            m["mu_g2g1"] = float(p[0])   # the pentode model's MU is the screen-to-grid amplification factor
        row = {"name": op.get("name", ""), "ep_V": op["ep_V"], "eg_V": op["eg_V"], "model": m, "datasheet": {}, "diff_pct": {}}
        for key in ("ip_mA", "mu", "rp_ohm", "gm_umho", "mu_g2g1"):
            if op.get(key) is not None:
                row["datasheet"][key] = op[key]
                row["diff_pct"][key] = (m[key] - op[key]) / op[key] * 100.0
        row["over_10pct"] = [k for k, v in row["diff_pct"].items() if abs(v) > 10.0]
        out.append(row)
    return out


CHECK = re.compile(r"^(rp|mu|gm|tr)_eb(\d+(?:\.\d+)?)(?:_vg2(\d+(?:\.\d+)?))?(_tri)?\.csv$", re.I)
CHECK_KEY = {"rp": "rp_ohm", "mu": "mu", "gm": "gm_umho", "tr": "ip_mA"}
# current bands (fraction of the plate graph's current axis) in which the errors are summarized: the low band is
# where the curves bend most (cutoff, crossover in push-pull)
BANDS = ((0.0, 0.03, "low"), (0.03, 0.10, "mid"), (0.10, 10.0, "high"))


def check_curves(tube, kind, p, src):
    """The fitted model against the datasheet's other graphs: rp, mu, gm (rp_eb*.csv, mu_eb*.csv, gm_eb*.csv: the
    quantity vs Eg at a plate voltage Eb) and transfer curves (tr_eb*.csv: Ip vs Eg). _vg2<V>: a pentode's screen
    voltage; _tri: triode connected. Errors in % of the datasheet value, grouped by the plate current there (the
    model's), bands of the plate graph's current axis."""
    d = os.path.join(DATA, tube)
    imax = src["plot"]["ip_max_mA"]
    out = {}
    for name in sorted(os.listdir(d)):
        m = CHECK.match(name)
        if not m:
            continue
        what, eb = m.group(1).lower(), float(m.group(2))
        eg2 = "anode" if m.group(4) else (float(m.group(3)) if m.group(3) else src.get("vg2"))
        a = np.loadtxt(os.path.join(d, name), delimiter=",", skiprows=1, ndmin=2)
        for eg, val in a:
            if what == "tr":
                ep = eb
                mm = small_signal(kind, p, ep, eg, eg2)
                if val < 0.01 * imax:
                    continue   # the relative error of a near-zero current says nothing
            else:
                mm = small_signal(kind, p, eb, eg, eg2)
            err = (mm[CHECK_KEY[what]] - val) / val * 100.0
            band = next(b[2] for b in BANDS if b[0] * imax <= mm["ip_mA"] < b[1] * imax)
            out.setdefault(what, {}).setdefault(band, []).append((err, eb, eg, mm["ip_mA"]))
    res = {}
    for what, bands in out.items():
        res[what] = {}
        for b in BANDS:
            e = bands.get(b[2])
            if not e:
                continue
            arr = np.array([x[0] for x in e])
            worst = max(e, key=lambda x: abs(x[0]))
            res[what][b[2]] = {"band_mA": [b[0] * imax, min(b[1], 1.0) * imax], "n": len(e),
                               "rms_pct": float(np.sqrt(np.mean(arr ** 2))), "max_pct": float(worst[0]),
                               "worst_at": {"eb_V": worst[1], "eg_V": worst[2], "ip_mA": worst[3]},
                               "over_10pct": bool(abs(worst[0]) > 10.0)}
    return res


def report(tube, kind, p, total, rows, write=True, src=None, points=None, checks=None):
    print(f"\n{tube} ({kind}, {len(rows)} curves, {total['n']} points)")
    print("  " + "  ".join(f"{n}={v:.6g}" for n, v in zip(NAMES, p)))
    print(f"  {'Vg':>8} {'Vg2':>6} {'n':>5} {'rms mA':>8} {'max mA':>8}")
    for r in rows:
        print(f"  {r['eg']:>8g} {r['eg2'] or '':>6} {r['n']:>5} {r['rms_mA']:>8.3f} {r['max_mA']:>8.3f}")
    print(f"  total rms {total['rms_mA']:.3f} mA ({total['rms_pct']:.2f}% of I_max), max {total['max_mA']:.3f} mA")
    for r in points or []:
        print(f"  operating point {r['name']} (Ep {r['ep_V']:g} V, Eg {r['eg_V']:g} V):")
        for key, label in (("ip_mA", "Ip mA"), ("mu", "mu"), ("rp_ohm", "rp ohm"), ("gm_umho", "gm umho"), ("mu_g2g1", "mu g2g1")):
            if key not in r["model"]:
                continue
            line = f"    {label:<8} model {r['model'][key]:>9.4g}"
            if key in r["datasheet"]:
                flag = "  <-- over 10 %" if key in r["over_10pct"] else ""
                line += f"   datasheet {r['datasheet'][key]:>9.4g}   {r['diff_pct'][key]:+6.1f} %{flag}"
            print(line)
    if checks:
        print(f"  datasheet graphs vs model (error in % of the datasheet value; bands by plate current):")
        names = {"rp": "rp", "mu": "mu", "gm": "gm", "tr": "Ip vs Eg"}
        for what, bands in checks.items():
            for band, r in bands.items():
                w = r["worst_at"]
                flag = "  <-- over 10 %" if r["over_10pct"] else ""
                print(f"    {names[what]:<8} {band:<4} {r['band_mA'][0]:6.1f} .. {r['band_mA'][1]:6.1f} mA  n {r['n']:>4}  "
                      f"rms {r['rms_pct']:5.1f} %  worst {r['max_pct']:+6.1f} % (Eb {w['eb_V']:g} V, Eg {w['eg_V']:.1f} V, "
                      f"Ip {w['ip_mA']:.1f} mA){flag}")
    if write:
        out = {"tube": tube, "model": kind, "params": dict(zip(NAMES, map(float, p))), "error": total, "curves": rows,
               "operating_points": points or [], "graph_checks": checks or {}, "datasheet": (src or {}).get("datasheet")}
        with open(os.path.join(DATA, tube, "fit.json"), "w", encoding="utf-8") as f:
            json.dump(out, f, indent=2, ensure_ascii=False)
            f.write("\n")


def selftest():
    """Synthetic curves from parameters chosen here (not from any publication), 0.5 % noise: the fit must
    reproduce the curves to well under the noise and land near the parameters."""
    rng = np.random.default_rng(1)
    cases = [
        ("triode", np.array([3.8, 1.35, 1400.0, 45.0, 600.0]), [0, -20, -40, -60, -80, -100], None, 450.0),
        ("triode", np.array([19.0, 1.3, 1100.0, 280.0, 250.0]), [0, -2, -4, -6, -8, -10, -12], None, 400.0),
        ("pentode", np.array([10.0, 1.35, 600.0, 50.0, 18.0]), [0, -5, -10, -15, -20, -25, -30], 250.0, 500.0),
    ]
    fails = 0
    for kind, p, grids, vg2, vmax in cases:
        curves = []
        for eg in grids:
            ep = np.linspace(5.0, vmax, 25)
            c = {"ep": ep, "eg": float(eg), "eg2": vg2}
            ip = model(kind, p, c)
            keep = ip > 1e-4 * model(kind, p, {"ep": np.array([vmax]), "eg": 0.0, "eg2": vg2})[0]
            c["ep"], c["ip"] = ep[keep], ip[keep] * (1 + 0.005 * rng.standard_normal(keep.sum()))
            if len(c["ep"]) >= 3:
                curves.append(c)
        q, imax = fit(kind, curves)
        total, rows = errors(kind, q, curves, imax)
        report(f"selftest {kind} mu={p[0]:g}", kind, q, total, rows, write=False)
        # the fitted model against the noiseless truth over the whole grid
        worst = 0.0
        for c in curves:
            worst = max(worst, float(np.abs(model(kind, q, c) - model(kind, p, c)).max() / imax))
        ok = total["rms_pct"] < 0.6 and worst < 0.01
        print(f"  vs true curves: max {worst * 100:.3f}% of I_max -> {'PASS' if ok else 'FAIL'}")
        print("  true  " + "  ".join(f"{n}={v:.6g}" for n, v in zip(NAMES, p)))
        fails += not ok
    return fails


def main(argv):
    if "--selftest" in argv:
        sys.exit(1 if selftest() else 0)
    tubes = [a for a in argv if not a.startswith("-")] or sorted(
        t for t in os.listdir(DATA) if os.path.isfile(os.path.join(DATA, t, "source.json")))
    done = 0
    for tube in tubes:
        src, kind, curves = load(tube)
        if not curves:
            continue
        p, imax = fit(kind, curves)
        total, rows = errors(kind, p, curves, imax)
        report(tube, kind, p, total, rows, src=src, points=check_points(kind, p, src), checks=check_curves(tube, kind, p, src))
        done += 1
    if not done:
        print("no digitized curves yet (verify/data/tubes/<tube>/vg*.csv)")


if __name__ == "__main__":
    np.seterr(over="ignore", invalid="ignore", divide="ignore")
    main(sys.argv[1:])
