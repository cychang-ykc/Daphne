"""pp-el34-ul with the nonlinear core (N28): low-frequency response at the 6 dB and 0 dB feedback steps, output
level P_ref (1.25 W at 1 kHz), P_ref - 20 dB and P_ref - 40 dB; 20 / 30 / 50 Hz relative to 1 kHz at the same input,
against the linear 100 H model (data/ref/pp-el34-ul_fb<dB>_response.csv); and the core flux at each level.

Xyce transient (the core cannot run in harmonic balance): settle, then the fundamental over the last cycles; core
flux from n(ymin!kpp_b). 16 processes, one math thread each; timed.

  python pp_core_lf.py [variant]      variant: a name in data/core/ja_fit.json (default base)
"""
import csv, json, math, os, subprocess, sys, tempfile, time
for _v in ("OMP_NUM_THREADS", "OPENBLAS_NUM_THREADS", "MKL_NUM_THREADS"):
    os.environ.setdefault(_v, "1")
from concurrent.futures import ProcessPoolExecutor
import numpy as np

import spice_amps as A, xyce_hb as X

HERE = os.path.dirname(os.path.abspath(__file__))
P_REF = 1.25
LEVELS = {"P_ref": P_REF, "P_ref -20 dB": P_REF / 100, "P_ref -40 dB": P_REF / 10000}
FREQS = (20.0, 30.0, 50.0, 1000.0)


def core_setup(variant):
    fit = {r["name"]: r for r in json.load(open(os.path.join(HERE, "data", "core", "ja_fit.json")))}[variant]
    A.CORE = dict(fit["params"], MS=fit["MS"])
    A.CORE_DESIGN = {"pp-el34-ul": dict(N=2222, area=18.1, path=22.8, gap_mm=0.0)}


def step_kw(F):
    if F == 0:
        return dict(feedback=False)
    st = {s["F"]: s for s in json.load(open(os.path.join(HERE, "data", "ref", "pp_feedback.json")))["steps"]}[F]
    return dict(feedback=True, r13=st["r13"], c9=st["c9"])


def transient(net, f0, settle_s, cycles=4, per_cycle=200, probes=("v(o)", "n(ymin!kpp_b)")):
    t_stop = settle_s + cycles / f0
    with tempfile.TemporaryDirectory() as td:
        cir = os.path.join(td, "c.cir")
        with open(cir, "w") as h:
            h.write("\n".join(X.to_xyce(net) + [f".tran {1 / f0 / per_cycle:.6g} {t_stop:.6g} 0 {1 / f0 / per_cycle:.6g}",
                                                ".options timeint method=gear reltol=1e-5",
                                                ".print tran format=noindex " + " ".join(probes), ".end", ""]))
        r = subprocess.run([X.XYCE, cir], capture_output=True, text=True, timeout=3600, cwd=td)
        rows = []
        if os.path.exists(cir + ".prn"):
            for line in open(cir + ".prn"):
                s = line.split()
                if len(s) == len(probes) + 1 and s[0][0] in "0123456789":
                    rows.append([float(x) for x in s])
    d = np.array(rows)
    if len(d) == 0 or d[-1, 0] < 0.999 * t_stop:
        raise RuntimeError("transient stopped: " + (r.stdout + r.stderr)[-400:])
    m = d[:, 0] >= settle_s
    return d[m]


def uniform(t, v, f0, per_cycle=1000):
    """Xyce prints at its own (variable) time steps: resample onto a uniform grid over whole cycles before any
    Fourier sum (a plain mean over the raw samples weights the dense regions and distorts the spectrum)."""
    n_cyc = math.floor((t[-1] - t[0]) * f0 + 1e-6)
    tt = t[-1] - np.arange(n_cyc * per_cycle, 0, -1) / (f0 * per_cycle)
    return tt, np.interp(tt, t, v)


def fundamental(t, v, f0):
    tt, vv = uniform(t, v, f0)
    return 2 * abs(np.mean(vv * np.exp(-2j * np.pi * f0 * tt)))


def job(args):
    variant, F, level, f0, amp = args
    core_setup(variant)
    t0 = time.time()
    net = [l.replace("{amp}", A.f(amp)).replace("{f0}", A.f(f0)) for l in A.pp_520(**step_kw(F))]
    settle = 1.2 if f0 < 100 else 0.4
    try:
        d = transient(net, f0, settle)
        t, vo, b = d[:, 0], d[:, 1], d[:, 2]
        return dict(variant=variant, F=F, level=level, f0=f0, vo=fundamental(t, vo, f0), b_pk=float(np.max(np.abs(b - b.mean()))),
                    s=time.time() - t0)
    except RuntimeError as e:
        return dict(variant=variant, F=F, level=level, f0=f0, error=str(e)[:300], s=time.time() - t0)


def linear_response(F, f0):
    rows = list(csv.DictReader(open(os.path.join(HERE, "data", "ref", f"pp-el34-ul_fb{F}_response.csv"))))
    f = np.array([float(r["f_Hz"]) for r in rows]); db = np.array([float(r["gain_dB_re_1kHz"]) for r in rows])
    return float(np.interp(math.log(f0), np.log(f), db))


if __name__ == "__main__":
    variant = sys.argv[1] if len(sys.argv) > 1 else "base"
    t_all = time.time()
    A.CORE = None
    amps = {}
    for F in (6, 0):                         # input amplitude for each output level at 1 kHz (linear model, harmonic balance)
        for name, p in LEVELS.items():
            g0 = 2248.0 / 10 ** (F / 20)
            amp = 4 * math.sqrt(p) / g0
            for _ in range(3):
                amp *= math.sqrt(p / X.measure("pp-el34-ul", amp, 1000.0, 15, step_kw(F))["P"])
            amps[(F, name)] = amp
    cases = [(variant, F, name, f0, amps[(F, name)]) for F in (6, 0) for name in LEVELS for f0 in FREQS]
    with ProcessPoolExecutor(16) as ex:
        res = list(ex.map(job, cases))
    out = []
    for F in (6, 0):
        for name in LEVELS:
            rr = {r["f0"]: r for r in res if r["F"] == F and r["level"] == name}
            if any("error" in r for r in rr.values()):
                print(f"step {F} dB {name}: ERROR " + "; ".join(r.get("error", "")[:120] for r in rr.values() if "error" in r)); continue
            ref = rr[1000.0]["vo"]
            line = []
            for f0 in FREQS[:-1]:
                db = 20 * math.log10(rr[f0]["vo"] / ref); lin = linear_response(F, f0)
                line.append(f"{f0:g} Hz {db:+6.2f} (linear {lin:+6.2f}, diff {db - lin:+5.2f})")
                out.append(dict(variant=variant, F=F, level=name, f0=f0, core_dB=db, linear_dB=lin, diff_dB=db - lin, b_pk=rr[f0]["b_pk"]))
            print(f"step {F} dB {name:<13}: " + " | ".join(line) + " | core B peak " +
                  " / ".join(f"{rr[f0]['b_pk']:.4f}" for f0 in FREQS) + " T (20/30/50/1k Hz)", flush=True)
    os.makedirs(os.path.join(HERE, "data", "core"), exist_ok=True)
    json.dump(dict(results=out, raw=res, seconds=time.time() - t_all, job_seconds=[r["s"] for r in res]),
              open(os.path.join(HERE, "data", "core", f"pp_core_lf_{variant.replace(' ', '_')}.json"), "w"), indent=1)
    print(f"total {time.time() - t_all:.0f} s; transient jobs {min(r['s'] for r in res):.0f} - {max(r['s'] for r in res):.0f} s each")
