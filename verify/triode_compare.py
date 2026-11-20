"""Triode model comparison for the H3 question (N28): Koren, Dempwolf-Zoelzer and Koren with a curvature parameter
(triode_models.py), fitted the same way to the same datasheet curves (2A3 plate curves; 300B plate curves with the
joint rp / gm constraint, as the presets), compared on the output stage alone (ideal drive) in Xyce harmonic balance.

Circuit: the single-ended output stage of spice_amps with an ideal grid source, primary inductance x10;
50 Hz / 1 W (2A3 8 ohm, 300B 4 ohm: the Stereophile conditions) and 1 kHz / 1 W / 8 ohm.
Reported: plate RMS, the datasheet operating points (mu / rp / gm, percent), H2 / H3, and the time of every fit and
Xyce run. Runs in 16 parallel processes, one math thread each.

  python triode_compare.py out.jsonl
"""
import json, math, os, sys, time
for _v in ("OMP_NUM_THREADS", "OPENBLAS_NUM_THREADS", "MKL_NUM_THREADS"):
    os.environ.setdefault(_v, "1")   # one math thread per process (16 processes in parallel)
from concurrent.futures import ProcessPoolExecutor
import numpy as np

import koren_fit as K, model_compare as MC, spice_amps as A, spice_ref as R, xyce_hb as X, triode_models as T

TUBES = {"2A3": dict(va=250, vg=-45, ia=0.060, raa=2500, opt=(10.0, 0.010, 400e-12, 100.0), r50=8.0, joint=False),
         "300B": dict(va=350, vg=-74, ia=0.060, raa=3000, opt=(15.0, 0.012, 400e-12, 120.0), r50=4.0, joint=True)}
MEASURED_H3_50HZ = {"2A3": (-56.0, -55.0), "300B": (-55.0, -53.0)}   # Stereophile, left / right (read off the plots)


def stage(tube, model, p, rl):
    t = TUBES[tube]; lp, ll, cw, dcr = t["opt"]
    A.OPT["r_load"] = rl
    net = ["* output stage", "Vin g2 0 SIN(0 {amp} {f0})", "Rgs2 g2 g2s 1k",
           T.spice_line("V2", model, p, "a2", "g2s", "k2", A.f, R.pos, R.softplus),
           *A.tube_caps("V2", tube, "a2", "g2s", "k2"), f"Rk2 k2 0 {A.f(-t['vg'] / t['ia'])}", "Ck2 k2 0 2.2e-4",
           f"Vb2 b2 0 DC {A.f(t['va'] - t['vg'] + t['ia'] * dcr)}"] + A.opt_se("T", "a2", "b2", t["raa"], lp * 10, ll, cw, dcr)
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
    tube, model, m_fixed = args
    src, kind, curves = K.load(tube)
    seed = [json.load(open(os.path.join(K.DATA, tube, "model.json")))["params"][n] for n in T.NAMES["koren"]]
    t0 = time.time()
    p, rms = T.fit(model, curves, seed, MC.ss_data(tube) if TUBES[tube]["joint"] else None, m_fixed)
    t_fit = time.time() - t0
    times = []
    h50 = harmonics_at_1w(stage(tube, model, p, TUBES[tube]["r50"]), 50.0, TUBES[tube]["r50"], times)
    h1k = harmonics_at_1w(stage(tube, model, p, 8.0), 1000.0, 8.0, times)
    return dict(tube=tube, model=model, m_fixed=m_fixed, rms=rms, p=list(map(float, p)), ops=T.op_errors(model, p, src),
                h50=h50, h1k=h1k, t_fit=t_fit, t_hb=float(np.mean(times)), n_hb=len(times))


def label(r):
    if r["model"] == "koren-m":
        return f"koren-m, m {'free' if r['m_fixed'] is None else r['m_fixed']} ({r['p'][5]:.2f})"
    return r["model"]


if __name__ == "__main__":
    cases = []
    for tube in TUBES:
        cases += [(tube, "koren", None), (tube, "dempwolf", None), (tube, "koren-m", None)]
        cases += [(tube, "koren-m", m) for m in (0.0, 0.25, 0.5, 0.75)]
    t0 = time.time()
    with open(sys.argv[1], "w") as out, ProcessPoolExecutor(16) as ex:
        for r in ex.map(job, cases):
            out.write(json.dumps(r) + "\n"); out.flush()
            ops = "; ".join(f"mu {o['mu']:+.0f} rp {o['rp_ohm']:+.0f} gm {o['gm_umho']:+.0f}" for o in r["ops"].values())
            print(f"{r['tube']:<5} {label(r):<24} RMS {r['rms']:5.2f} % | 50 Hz H2 {r['h50'][0]:6.1f} H3 {r['h50'][1]:6.1f} | "
                  f"1 kHz H2 {r['h1k'][0]:6.1f} H3 {r['h1k'][1]:6.1f} | op % {ops} | fit {r['t_fit']:5.1f} s, Xyce {r['n_hb']} x {r['t_hb']:.2f} s", flush=True)
    print(f"total {time.time() - t0:.1f} s (16 processes)")
