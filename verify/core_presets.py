"""The four presets with the nonlinear core (N28): sensitivity to coercivity and low-flux permeability (x0.5 / x2
around base2), at 50 Hz / 1 W and 20 Hz / rated power; pp-el34-ul at the 0 dB and 6 dB feedback steps.

Single-ended: turns and air gap from data/core/se_design.json (designed with base2: target inductance at the optimum
gap); the output-stage B+ ramps up over 0.2 s, because Xyce's core only magnetises in the transient. Every point
is set to its output power with one correction from the measured fundamental. Xyce transient (the core cannot run in
harmonic balance); the linear model (no core) at the same point in harmonic balance for comparison.
16 processes, one math thread each; timed.

  python core_presets.py
"""
import json, math, os, sys, time
for _v in ("OMP_NUM_THREADS", "OPENBLAS_NUM_THREADS", "MKL_NUM_THREADS"):
    os.environ.setdefault(_v, "1")
from concurrent.futures import ProcessPoolExecutor
import numpy as np

import spice_amps as A, xyce_hb as X, pp_core_lf as P

HERE = os.path.dirname(os.path.abspath(__file__))
FITS = {r["name"]: r for r in json.load(open(os.path.join(HERE, "data", "core", "ja_fit.json")))}
SE = {r["name"]: r for r in json.load(open(os.path.join(HERE, "data", "core", "se_design.json")))}
GEOM = {"set-2a3": (12.2, 19.2), "set-300b": (18.1, 22.8), "set-845": (28.5, 30.0), "pp-el34-ul": (18.1, 22.8)}
RATED = {"set-2a3": 4.83, "set-300b": 5.61, "set-845": 25.1, "pp-el34-ul": 20.0}
BPLUS = {"set-2a3": ("Vb2 ", "Vb2 b2 0", 301.0), "set-300b": ("Vb2 ", "Vb2 b2 0", 431.0), "set-845": ("Vb3 ", "Vb3 b3 0", 1286.0)}
VARIANTS = ("base2", "base2 Hc x0.5", "base2 Hc x2", "base2 mu_low x0.5", "base2 mu_low x2")
T_RAMP = 0.05
T_SIG = 0.15


def setup(variant, preset):
    fit = FITS[variant]
    A.CORE = dict(fit["params"], MS=fit["MS"])
    area, path = GEOM[preset]
    if preset == "pp-el34-ul":
        A.CORE_DESIGN = {preset: dict(N=2222, area=area, path=path, gap_mm=0.0)}
    else:
        A.CORE_DESIGN = {preset: dict(N=SE[preset]["N"], area=area, path=path, gap_mm=SE[preset]["gap_mm"])}


def netlist(preset, kw, amp, f0):
    """The output stage's supply (pp: the whole HT) rises exponentially (time constant T_RAMP): Xyce's core only
    magnetises in the transient, and the corners of a linear ramp (slope steps) make its magnetisation equation fail."""
    net = A.pp_520(**kw) if preset == "pp-el34-ul" else A.PRESETS[preset]()
    t_sig = 0.0
    if preset in BPLUS:                      # single-ended: the DC through the core builds up first
        key, head, v = BPLUS[preset]
        net = [f"{head} EXP(0 {v:g} 0 {T_RAMP:g} 100 1)" if l.startswith(key) else l for l in net]
        t_sig = 6 * T_RAMP
    # the signal fades in (time constant T_SIG) so that the core's minor loops grow symmetrically: a sine at full
    # amplitude from its first half cycle leaves a remanent DC flux that the model never shakes out (the push-pull's
    # supply is not ramped: its DC fields cancel, and a ramp unbalances the halves while the couplings charge)
    net = [f"BVin in 0 V={{{A.f(amp)}*(1-exp(-max(TIME-{t_sig:g},0)/{T_SIG:g}))*sin({2 * math.pi * f0:.9g}*TIME)}}"
           if l.startswith("Vin ") else l for l in net]
    return [l.replace("{amp}", A.f(amp)).replace("{f0}", A.f(f0)) for l in net]


def harm(t, v, f0, n=7):
    t, v = P.uniform(t, v, f0)
    a = [2 * abs(np.mean(v * np.exp(-2j * np.pi * h * f0 * t))) for h in range(1, n + 1)]
    return a[0], [20 * math.log10(max(x, 1e-300) / a[0]) for x in a[1:]]


def job(args):
    preset, F, variant, f0, p_target = args
    kw = (dict(feedback=False) if F == 0 else dict(P.step_kw(F))) if preset == "pp-el34-ul" else None
    t0 = time.time()
    # input amplitude: 50 Hz / 1 W is set by output power (one correction from the core transient below);
    # "20 Hz rated" is the input that gives rated power at 1 kHz, applied at 20 Hz (forcing rated output at 20 Hz
    # overdrives the driver even in the linear model). Linear model at the same input, harmonic balance.
    A.CORE = None
    fset = f0 if f0 != 20.0 else 1000.0
    amp = 4 * math.sqrt(p_target) / X.GAIN0.get(preset, 2248.0 / 10 ** (F / 20) if preset == "pp-el34-ul" else 1.0)
    for _ in range(3):
        amp *= math.sqrt(p_target / X.measure(preset, amp, fset, 15, kw)["P"])
    lin = X.measure(preset, amp, f0, 30, kw)
    setup(variant, preset)
    settle = (6 * T_RAMP if preset in BPLUS else 0.0) + 6 * T_SIG + 0.5
    probe = "n(ymin!kt_b)" if preset != "pp-el34-ul" else "n(ymin!kpp_b)"
    try:
        for it in range(2):
            d = P.transient(netlist(preset, kw, amp, f0), f0, settle, cycles=4, per_cycle=400, probes=("v(o)", probe))
            a1, h = harm(d[:, 0], d[:, 1], f0)
            p = a1 ** 2 / 16
            if it == 0 and f0 != 20.0:
                amp *= math.sqrt(p_target / p)
            elif it == 0:
                break
        b = d[:, 2]
        return dict(preset=preset, F=F, variant=variant, f0=f0, P=p, H=h, THD=100 * math.sqrt(sum(10 ** (x / 10) for x in h)),
                    Bdc=float(b.mean()), Bac=float((b.max() - b.min()) / 2), lin_H=list(map(float, lin["H"][:6])), lin_THD=lin["THD"],
                    lin_P=lin["P"],
                    s=time.time() - t0)
    except RuntimeError as e:
        return dict(preset=preset, F=F, variant=variant, f0=f0, error=str(e)[:300], s=time.time() - t0)


if __name__ == "__main__":
    t_all = time.time()
    cases = []
    for preset in ("set-2a3", "set-300b", "set-845", "pp-el34-ul"):
        for F in ((6, 0) if preset == "pp-el34-ul" else (None,)):
            for v in VARIANTS:
                cases += [(preset, F, v, 50.0, 1.0), (preset, F, v, 20.0, RATED[preset])]
    with ProcessPoolExecutor(16) as ex:
        res = list(ex.map(job, cases))
    json.dump(dict(results=res, seconds=time.time() - t_all), open(os.path.join(HERE, "data", "core", "core_presets.json"), "w"),
              indent=1, default=float)
    for r in res:
        tag = f"{r['preset']}{'' if r['F'] is None else ' fb' + str(r['F'])} {r['variant']:<18} {r['f0']:>4g} Hz"
        if "error" in r:
            print(f"{tag} ERROR {r['error'][:100]}"); continue
        print(f"{tag} P {r['P']:6.2f} W  THD {r['THD']:6.2f} %  H2 {r['H'][0]:6.1f} H3 {r['H'][1]:6.1f} H4 {r['H'][2]:6.1f} H5 {r['H'][3]:6.1f}"
              f" | B dc {r['Bdc']:.3f} ac {r['Bac']:.3f} T | linear: P {r['lin_P']:6.2f} W THD {r['lin_THD']:6.2f} % H2 {r['lin_H'][0]:6.1f} H3 {r['lin_H'][1]:6.1f} | {r['s']:.0f} s", flush=True)
    print(f"total {time.time() - t_all:.0f} s")
