"""Triode model comparison for the H3 question (N28): Koren vs Dempwolf-Zoelzer, fitted the same way to the same
datasheet curves, compared on the output stage alone (ideal drive) in Xyce harmonic balance.

  Koren:     Ia = E1^x / kg1, E1 = Va / kp ln(1 + exp(kp (1/mu + Vg / sqrt(kvb + Va^2))))   (Reefman's ExtractModel
             uses this same triode form)
  Dempwolf:  Ia = G (ln(1 + exp(C (Va/mu + Vg))) / C)^gamma   (A Physically-motivated Triode Model, DAFx-11)

Fits: absolute residual / Imax, soft_l1, the multistarts of model_compare; the 300B with and without the joint rp / gm
constraint. Circuit: spice_amps single-ended output stage with an ideal grid source, primary inductance x10;
50 Hz / 1 W (2A3 8 ohm, 300B 4 ohm, the Stereophile conditions) and 1 kHz / 1 W / 8 ohm. Every fit and every Xyce run
is timed. Fits run in parallel (16 processes).

  python triode_compare.py out.jsonl
"""
import json, math, os, sys, time
# one math thread per process: the fits run as 16 parallel processes, and multithreaded BLAS in each of them
# oversubscribes the cores (the first H3 diagnostic took about an hour instead of minutes because of that)
for _v in ("OMP_NUM_THREADS", "OPENBLAS_NUM_THREADS", "MKL_NUM_THREADS"):
    os.environ.setdefault(_v, "1")
from concurrent.futures import ProcessPoolExecutor
import numpy as np
from scipy.optimize import least_squares

import koren_fit as K, model_compare as MC, spice_amps as A, spice_ref as R, xyce_hb as X

TUBES = {"2A3": dict(va=250, vg=-45, ia=0.060, raa=2500, opt=(10.0, 0.010, 400e-12, 100.0), r50=8.0),
         "300B": dict(va=350, vg=-74, ia=0.060, raa=3000, opt=(15.0, 0.012, 400e-12, 120.0), r50=4.0)}


def fit(name, curves, seed, ss=None):
    ep = np.concatenate([c["ep"] for c in curves]); ip = np.concatenate([c["ip"] for c in curves])
    eg = np.concatenate([np.full(len(c["ep"]), c["eg"]) for c in curves])
    allc = {"ep": ep, "eg": eg, "eg2": np.zeros_like(ep)}
    imax = float(ip.max())

    def resid(q):
        p = MC.unpack(name, q); r = (MC.ia(name, p, allc) - ip) / imax
        if ss:
            r = np.r_[r, 0.05 * np.array([(MC.small_signal(name, p, eb, g)["rp_ohm" if w == "rp" else "gm_umho"] - v) / v
                                          for w, eb, g, v in ss])]
        return np.nan_to_num(r, nan=1e3, posinf=1e3, neginf=-1e3)
    best = None
    for p0 in MC.starts(name, curves, seed):
        try:
            r = least_squares(resid, MC.pack(name, p0), loss="soft_l1", f_scale=0.01, x_scale="jac", max_nfev=4000)
        except (ValueError, FloatingPointError, OverflowError):
            continue
        c = float(np.sum(resid(r.x) ** 2))
        if np.isfinite(c) and (best is None or c < best[0]):
            best = (c, r.x)
    p = MC.unpack(name, best[1])
    return p, MC.plate_errors(name, p, curves, imax)[2]


def triode_line(name, model, p, a, g, k):
    f = A.f
    if model == "koren-triode":
        mu, ex, kg1, kp, kvb = (f(v) for v in p)
        z = f"{kp}*(1/{mu}+v({g},{k})/sqrt({kvb}+v({a},{k})*v({a},{k})))"
        e1 = f"({R.pos(f'v({a},{k})')}/{kp}*{R.softplus(z)}+1e-30)"
        return f"B{name} {a} {k} I = 2*exp({ex}*ln({e1}))/{kg1}"
    G, C, mu, gamma = (f(v) for v in p)
    z = f"{C}*(v({a},{k})/{mu}+v({g},{k}))"
    return f"B{name} {a} {k} I = {G}*exp({gamma}*ln({R.softplus(z)}/{C}+1e-30))"


def stage(tube, model, p, rl):
    t = TUBES[tube]; lp, ll, cw, dcr = t["opt"]
    bp = t["va"] - t["vg"] + t["ia"] * dcr
    A.OPT["r_load"] = rl
    net = ["* output stage", "Vin g2 0 SIN(0 {amp} {f0})", "Rgs2 g2 g2s 1k", triode_line("V2", model, p, "a2", "g2s", "k2"),
           *A.tube_caps("V2", tube, "a2", "g2s", "k2"), f"Rk2 k2 0 {A.f(-t['vg'] / t['ia'])}", "Ck2 k2 0 2.2e-4",
           f"Vb2 b2 0 DC {A.f(bp)}"] + A.opt_se("T", "a2", "b2", t["raa"], lp * 10, ll, cw, dcr)
    A.OPT["r_load"] = 8.0
    return net


def harmonics_at_1w(net, f0, rl, times):
    amp = 20.0
    for nf in (15, 15, 15, 15, 15, 30):
        t0 = time.time()
        a, _ = X.run_hb([l.replace("{amp}", A.f(amp)).replace("{f0}", A.f(f0)) for l in net], f0, nf)
        times.append(time.time() - t0)
        if nf == 15:
            amp *= math.sqrt(1.0 / (a[0] ** 2 / 2 / rl))
    return [float(x) for x in 20 * np.log10(a[1:5] / a[0])]


def job(args):
    tube, model, joint = args
    src, kind, curves = K.load(tube)
    seed = [json.load(open(os.path.join(K.DATA, tube, "model.json")))["params"][n] for n in ("MU", "EX", "KG1", "KP", "KVB")]
    t0 = time.time()
    p, rms = fit(model, curves, seed, MC.ss_data(tube) if joint else None)
    t_fit = time.time() - t0
    times = []
    h50 = harmonics_at_1w(stage(tube, model, p, TUBES[tube]["r50"]), 50.0, TUBES[tube]["r50"], times)
    h1k = harmonics_at_1w(stage(tube, model, p, 8.0), 1000.0, 8.0, times)
    return dict(tube=tube, model=model, joint=joint, rms=rms, p=list(map(float, p)), h50=h50, h1k=h1k,
                t_fit=t_fit, t_hb_mean=float(np.mean(times)), n_hb=len(times))


if __name__ == "__main__":
    cases = [("2A3", "koren-triode", False), ("2A3", "dempwolf", False),
             ("300B", "koren-triode", True), ("300B", "dempwolf", True),
             ("300B", "koren-triode", False), ("300B", "dempwolf", False)]
    t0 = time.time()
    with open(sys.argv[1], "w") as out, ProcessPoolExecutor(16) as ex:
        for r in ex.map(job, cases):
            out.write(json.dumps(r) + "\n"); out.flush()
            print(f"{r['tube']:<5} {r['model']:<13} {'rp/gm joint' if r['joint'] else 'plate only':<11} RMS {r['rms']:5.2f} % | "
                  f"50 Hz H2..H5 {' '.join(f'{x:6.1f}' for x in r['h50'])} | 1 kHz H2..H5 {' '.join(f'{x:6.1f}' for x in r['h1k'])} | "
                  f"fit {r['t_fit']:6.0f} s, Xyce {r['n_hb']} runs x {r['t_hb_mean']:.2f} s", flush=True)
    print(f"total {time.time() - t0:.0f} s")
