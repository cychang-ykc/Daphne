"""pp-el34-ul feedback steps (N28): 0, 6, 12, 20 dB and the book's 30 dB, each a fixed reference data set.

Method: the feedback resistor R13 is scaled (beta ~ R5 / (R5 + R13)); C9 is scaled inversely so that R13 C9, the
corner of the book's lead network (86 kHz), stays the same. 30 dB is the book's network (5.6 k // 330 pF, measured
32.7 dB at 1 kHz); 20 / 12 / 6 dB solve R13 so that the measured feedback at 1 kHz (open-loop gain / closed-loop gain)
equals the step; 0 dB is open loop (R13 to ground, loading kept).

Per step: stability (closed-loop peak with 8 ohm, gain and phase margin of the loop gain T = A / closed - 1, gain
margin with the output open), power sweep 0.1 W - 27 W at 1 kHz / 50 Hz / 10 kHz, quasi-static transfer curves and
frequency response (as reference_sweep.py), and THD against output power for the reference-level proposal.
Output data/ref/pp-el34-ul_fb<dB>_*.csv and pp_feedback.json. 16 processes, one math thread each; timed.

  python pp_feedback.py
"""
import json, math, os, sys, time
for _v in ("OMP_NUM_THREADS", "OPENBLAS_NUM_THREADS", "MKL_NUM_THREADS"):
    os.environ.setdefault(_v, "1")
from concurrent.futures import ProcessPoolExecutor
import numpy as np

import spice_amps as A, xyce_hb as X, reference_sweep as RS

R13_BOOK, C9_BOOK = 5.6e3, 330e-12
STEPS = (0, 6, 12, 20, 30)
OUT = RS.OUT


def ac(kw, r_load=8.0):
    A.OPT["r_load"] = r_load
    net = [l if not l.startswith("Vin ") else "Vin in 0 DC 0 AC 1" for l in A.pp_520(**kw)]
    A.OPT["r_load"] = 8.0
    a = A.run(net, ["vr(o)", "vi(o)"], 1000.0, 0, ac=(1, 4e6))
    return a[:, 0], a[:, 1] + 1j * a[:, 3]


def network(r13):
    return dict(r13=r13, c9=C9_BOOK * R13_BOOK / r13)


def feedback_db(r13):
    f, C = ac(dict(feedback=True, **network(r13))); _, Ao = ac(dict(feedback=False, **network(r13)))
    i = np.argmin(abs(f - 1000))
    return 20 * math.log10(abs(Ao[i]) / abs(C[i]))


def solve_r13(target):
    lo, hi = math.log(R13_BOOK), math.log(5e6)          # feedback falls as R13 grows
    for _ in range(30):
        m = 0.5 * (lo + hi)
        lo, hi = (m, hi) if feedback_db(math.exp(m)) > target else (lo, m)
    return math.exp(0.5 * (lo + hi))


def stability(r13, r_load):
    kw = network(r13)
    f, C = ac(dict(feedback=True, **kw), r_load); _, Ao = ac(dict(feedback=False, **kw), r_load)
    i1 = np.argmin(abs(f - 1000))
    db = 20 * np.log10(abs(C) / abs(C[i1]))
    hi = f > 1000
    T = Ao / C - 1
    ang = np.unwrap(np.angle(T))
    gm = next((-20 * math.log10(abs(T[k])) for k in np.nonzero(hi)[0] if abs(ang[k] - ang[i1]) >= math.pi), None)
    kc = np.nonzero(hi & (abs(T) < 1))[0]
    pm = 180 - abs(math.degrees(ang[kc[0]] - ang[i1])) if len(kc) else None
    up = f[np.argmax(hi & (abs(db) > 1))]
    return dict(peak_dB=float(db[hi].max()), peak_f=float(f[hi][np.argmax(db[hi])]), gm_dB=gm, pm_deg=pm, up1dB_Hz=float(up))


def step_setup(F):
    t0 = time.time()
    if F == 0:
        return dict(F=0, r13=None, c9=None, measured_dB=0.0, s=time.time() - t0)
    r13 = R13_BOOK if F == 30 else solve_r13(F)
    return dict(F=F, r13=r13, c9=network(r13)["c9"], measured_dB=feedback_db(r13), s=time.time() - t0)


def stab_job(st):
    t0 = time.time()
    if st["F"] == 0:
        return dict(F=0, s=time.time() - t0)
    return dict(F=st["F"], load8=stability(st["r13"], 8.0), open=stability(st["r13"], 1e9), s=time.time() - t0)


def kw_of(st):
    return dict(feedback=False) if st["F"] == 0 else dict(feedback=True, r13=st["r13"], c9=st["c9"])


def sweep_job(args):
    st, f0, p = args
    t0 = time.time()
    g0 = {0: 2248.0}.get(st["F"], 2248.0 / 10 ** (st["measured_dB"] / 20))
    amp = 4 * math.sqrt(p) / g0
    try:
        for _ in range(3):
            r = X.measure("pp-el34-ul", amp, f0, 15, kw_of(st)); amp *= math.sqrt(p / r["P"])
        r = X.measure("pp-el34-ul", amp, f0, 60, kw_of(st))
        return dict(F=st["F"], f0=f0, P=p, amp=amp, THD=r["THD"], H=list(map(float, r["H"])), s=time.time() - t0)
    except RuntimeError as e:
        return dict(F=st["F"], f0=f0, P=p, error=str(e)[:200], s=time.time() - t0)


def transfer_job(args):
    st, label, amp = args
    import subprocess, tempfile
    t0 = time.time()
    net = [l.replace("{amp}", A.f(amp)).replace("{f0}", "1000") for l in A.pp_520(**kw_of(st))]
    with tempfile.TemporaryDirectory() as td:
        cir = os.path.join(td, "t.cir")
        with open(cir, "w") as h:
            h.write("\n".join(X.to_xyce(net) + [".hb 1000", ".options hbint numfreq=60", ".print hb v(in) v(o)", ".end", ""]))
        subprocess.run([X.XYCE, cir], capture_output=True, text=True, timeout=X.TIMEOUT, cwd=td)
        rows = []
        with open(cir + ".HB.TD.prn") as h:
            for line in h:
                try:
                    rows.append([float(x) for x in line.split()[1:4]])
                except (ValueError, IndexError):
                    continue
    a = np.array(rows); n = len(a); k = np.exp(-2j * np.pi * np.arange(n) / n)
    inverted = bool(np.real(np.sum(a[:, 2] * k) / np.sum(a[:, 1] * k)) < 0)
    return dict(F=st["F"], label=label, t=a[:, 0].tolist(), vin=a[:, 1].tolist(), vout=(-a[:, 2] if inverted else a[:, 2]).tolist(),
                inverted=inverted, s=time.time() - t0)


def response_job(st):
    t0 = time.time()
    net = [l if not l.startswith("Vin ") else "Vin in 0 DC 0 AC 1" for l in A.pp_520(**kw_of(st))]
    a = A.run(net, ["vdb(o)", "vp(o)"], 1000.0, 0, ac=(10, 200e3))
    f, db, ph = a[:, 0], a[:, 1], a[:, 3]
    i = np.argmin(abs(f - 1000))
    return dict(F=st["F"], f=f.tolist(), db=(db - db[i]).tolist(), phase_deg=np.degrees(np.unwrap(ph) - np.unwrap(ph)[i]).tolist(),
                s=time.time() - t0)


P_REF = 1.25    # W: pink-noise peaks (about 16 x) at the 20 W rating, the headroom the single-ended presets keep


if __name__ == "__main__":
    t_all = time.time()
    with ProcessPoolExecutor(16) as ex:
        steps = list(ex.map(step_setup, STEPS))
        t_setup = time.time() - t_all
        stab = {r["F"]: r for r in ex.map(stab_job, steps)}
        powers = (0.05, 0.1, 0.2, 0.5, 1, P_REF, 2, 4 * P_REF, 10, 20, 27)
        cases = [(st, f0, p) for st in steps for f0 in (1000.0, 50.0, 10000.0) for p in powers]
        t1 = time.time()
        sw = list(ex.map(sweep_job, cases))
        t_sweep = time.time() - t1
        amps = {(r["F"], r["P"]): r["amp"] for r in sw if r["f0"] == 1000.0 and "amp" in r}
        t1 = time.time()
        tr = list(ex.map(transfer_job, [(st, lab, amps[(st["F"], p)] * m) for st in steps
                                        for lab, p, m in (("0.1W", 0.1, 1), ("1W", 1, 1), ("rated", 20, 1), ("2x_rated_drive", 20, 2))]))
        rs = list(ex.map(response_job, steps))
        t_tr = time.time() - t1
    # files per step: sweep, transfer curves, response (reference_sweep conventions)
    for st in steps:
        tag = f"pp-el34-ul_fb{st['F']}"
        with open(os.path.join(OUT, f"{tag}_sweep.csv"), "w") as h:
            h.write("f_Hz,P_W,input_V_peak,THD_pct,H2_dB,H3_dB,H4_dB,H5_dB,H6_dB,H7_dB\n")
            for r in sw:
                if r["F"] == st["F"] and "H" in r:
                    h.write(f"{r['f0']:g},{r['P']:g},{r['amp']:.6g},{r['THD']:.5g}," + ",".join(f"{x:.2f}" for x in r["H"]) + "\n")
        for r in tr:
            if r["F"] == st["F"]:
                with open(os.path.join(OUT, f"{tag}_transfer_{r['label']}.csv"), "w") as h:
                    h.write("t_s,vin_V,vout_V\n")
                    for t_, vi, vo in zip(r["t"], r["vin"], r["vout"]):
                        h.write(f"{t_:.9g},{vi:.9g},{vo:.9g}\n")
        for r in rs:
            if r["F"] == st["F"]:
                with open(os.path.join(OUT, f"{tag}_response.csv"), "w") as h:
                    h.write("f_Hz,gain_dB_re_1kHz,phase_deg_re_1kHz\n")
                    for f_, d, ph in zip(r["f"], r["db"], r["phase_deg"]):
                        h.write(f"{f_:.6g},{d:.4f},{ph:.3f}\n")
    json.dump(dict(steps=steps, stability=stab, errors=[r for r in sw if "error" in r], p_ref_W=P_REF,
                   polarity_flipped=sorted({r["F"] for r in tr if r["inverted"]}),
                   seconds=dict(total=time.time() - t_all, setup=t_setup, sweep=t_sweep, transfer_response=t_tr)),
              open(os.path.join(OUT, "pp_feedback.json"), "w"), indent=1, default=float)
    for st in steps:
        s = stab[st["F"]]
        if st["F"]:
            l8, op = s["load8"], s["open"]
            print(f"step {st['F']:>2} dB: R13 {st['r13'] / 1e3:8.2f} k, C9 {st['c9'] * 1e12:6.1f} pF, measured {st['measured_dB']:5.1f} dB | "
                  f"8 ohm: peak {l8['peak_dB']:+.2f} dB @ {l8['peak_f'] / 1e3:.0f} kHz, +-1 dB to {l8['up1dB_Hz'] / 1e3:.0f} kHz, GM {l8['gm_dB']}, PM {l8['pm_deg']} | "
                  f"open: GM {op['gm_dB']}, peak {op['peak_dB']:+.2f}")
        else:
            print("step  0 dB: open loop")
    for st in steps:
        print(f"-- step {st['F']} dB, 1 kHz:")
        for r in sw:
            if r["F"] == st["F"] and r["f0"] == 1000.0:
                print(f"   {r['P']:>5g} W  " + (f"THD {r['THD']:7.4f} %  " + " ".join(f"H{i + 2} {x:6.1f}" for i, x in enumerate(r["H"][:4])) if "H" in r else "ERROR"))
    for r in rs:
        f, d = np.array(r["f"]), np.array(r["db"])
        print(f"response step {r['F']:>2} dB: " + " ".join(f"{fr:g} Hz {d[np.argmin(abs(f - fr))]:+.2f}" for fr in (10, 20, 10e3, 20e3, 50e3, 100e3)))
    print("reference level (P_ref %.2f W) and 200 %% drive (%.2f W), 1 kHz / 50 Hz / 10 kHz THD:" % (P_REF, 4 * P_REF))
    for st in steps:
        row = {(r["f0"], r["P"]): r for r in sw if r["F"] == st["F"] and "H" in r}
        a, b2 = row[(1000.0, P_REF)], row[(1000.0, 4 * P_REF)]
        print(f"  step {st['F']:>2} dB: P_ref THD {a['THD']:.3f} % H2 {a['H'][0]:6.1f} H3 {a['H'][1]:6.1f} H4 {a['H'][2]:6.1f} H5 {a['H'][3]:6.1f}"
              f" (50 Hz {row[(50.0, P_REF)]['THD']:.3f} %, 10 kHz {row[(10000.0, P_REF)]['THD']:.3f} %) | 200 %: THD {b2['THD']:.3f} % H2 {b2['H'][0]:6.1f} H3 {b2['H'][1]:6.1f} H5 {b2['H'][3]:6.1f}")
    print(f"total {time.time() - t_all:.1f} s (setup {t_setup:.1f}, sweep {t_sweep:.1f}, transfer + response {t_tr:.1f})")
