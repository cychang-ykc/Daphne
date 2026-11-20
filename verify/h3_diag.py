"""H3 diagnostic (N28): can the datasheet curves decide H3? For 2A3 and 300B, refit the Koren triode with
different weightings and 20 bootstrap resamples of the digitized points (plate RMS nearly equal), then compute H2 / H3
in the preset circuit at 1 kHz / 1 W / 8 ohm and 50 Hz / 1 W (2A3 8 ohm, 300B 4 ohm; primary inductance x10).
  python h3_diag.py out.jsonl      (about an hour: the 300B joint rp / gm fits are slow)
"""
import sys, os, math, json
sys.path.insert(0, os.getcwd())
import numpy as np
from scipy.optimize import least_squares
from concurrent.futures import ProcessPoolExecutor
import model_compare as MC, koren_fit as K, spice_ref as R, spice_amps as A, xyce_hb as X

NAME = "koren-triode"
COND = {"2A3": ("set-2a3", 8.0), "300B": ("set-300b", 4.0)}

def wfit(curves, weight="abs", ss=None, loss="soft_l1"):
    ep = np.concatenate([c["ep"] for c in curves]); ip = np.concatenate([c["ip"] for c in curves])
    eg = np.concatenate([np.full(len(c["ep"]), c["eg"]) for c in curves])
    allc = {"ep": ep, "eg": eg, "eg2": np.zeros_like(ep)}
    imax = float(ip.max())
    w = {"abs": np.full(len(ip), 1.0),
         "low x3": np.where(ip < 0.2 * imax, 3.0, 1.0),
         "high x3": np.where(ip > 0.5 * imax, 3.0, 1.0),
         "relative": imax / np.maximum(ip, 0.1 * imax)}[weight] / imax
    def resid(q):
        p = MC.unpack(NAME, q); r = (MC.ia(NAME, p, allc) - ip) * w
        if ss:
            r = np.r_[r, 0.05 * np.array([(MC.small_signal(NAME, p, eb, g)["rp_ohm" if wh == "rp" else "gm_umho"] - v) / v for wh, eb, g, v in ss])]
        return np.nan_to_num(r, nan=1e3, posinf=1e3, neginf=-1e3)
    best = None
    for p0 in MC.starts(NAME, curves, None):
        try: r = least_squares(resid, MC.pack(NAME, p0), loss=loss, f_scale=0.01, x_scale="jac", max_nfev=4000)
        except (ValueError, FloatingPointError, OverflowError): continue
        c = float(np.sum(resid(r.x) ** 2))
        if np.isfinite(c) and (best is None or c < best[0]): best = (c, r.x)
    return MC.unpack(NAME, best[1])

def h_at(tube, params, f0, rl):
    orig_model, orig_opt = R.model, A.opt_se
    R.model = lambda t: {"params": dict(zip(("MU", "EX", "KG1", "KP", "KVB"), map(float, params)))} if t == tube else orig_model(t)
    A.opt_se = lambda n, a, bp, raa, lp, ll, cw, dcr: orig_opt(n, a, bp, raa, lp * 10, ll, cw, dcr)
    A.OPT["r_load"] = rl
    try:
        net0 = A.PRESETS[COND[tube][0]](); amp = 0.5
        for _ in range(5):
            a, _ = X.run_hb([l.replace("{amp}", A.f(amp)).replace("{f0}", A.f(f0)) for l in net0], f0, 15); amp *= math.sqrt(1.0 / (a[0] ** 2 / 2 / rl))
        a, _ = X.run_hb([l.replace("{amp}", A.f(amp)).replace("{f0}", A.f(f0)) for l in net0], f0, 30)
        return 20 * np.log10(a[1:3] / a[0])
    finally:
        R.model, A.opt_se, A.OPT["r_load"] = orig_model, orig_opt, 8.0

def job(args):
    tube, label, weight, ss_on, boot, loss = args
    src, kind, curves = K.load(tube)
    fc = curves
    if boot is not None:
        rng = np.random.default_rng(boot)
        fc = []
        for c in curves:
            i = np.sort(rng.integers(0, len(c["ep"]), len(c["ep"])))
            fc.append(dict(c, ep=c["ep"][i], ip=c["ip"][i]))
    p = wfit(fc, weight, MC.ss_data(tube) if ss_on else None, loss)
    imax = max(float(c["ip"].max()) for c in curves)
    rms = MC.plate_errors(NAME, p, curves, imax)[2]
    h1k = h_at(tube, p, 1000.0, 8.0); h50 = h_at(tube, p, 50.0, COND[tube][1])
    return tube, label, rms, h1k, h50, list(map(float, p))

if __name__ == "__main__":
    cases = []
    for tube, ss in (("2A3", False), ("300B", True)):
        for wname in ("abs", "low x3", "high x3", "relative"):
            cases.append((tube, f"weight {wname}", wname, ss, None, "soft_l1"))
        cases.append((tube, "weight abs, linear loss", "abs", ss, None, "linear"))
        if tube == "300B": cases.append((tube, "weight abs, no rp/gm", "abs", False, None, "soft_l1"))
        cases += [(tube, f"bootstrap {b:2d}", "abs", ss, b, "soft_l1") for b in range(20)]
    out = open(sys.argv[1], "w")
    with ProcessPoolExecutor(14) as ex:
        for tube, label, rms, h1k, h50, p in ex.map(job, cases):
            print(f"{tube:<5} {label:<26} RMS {rms:5.2f} %  1 kHz/8: H2 {h1k[0]:6.1f} H3 {h1k[1]:6.1f}  50 Hz: H2 {h50[0]:6.1f} H3 {h50[1]:6.1f}", flush=True)
            out.write(json.dumps({"tube": tube, "label": label, "rms": rms, "h1k": list(map(float, h1k)), "h50": list(map(float, h50)), "p": p}) + "\n")
