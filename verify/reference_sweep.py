"""The M0 reference data of the four presets (spec "SPICE flow" 2 - 3), from the spice_amps netlists:

  power sweep        H2 - H7 and THD from 0.1 W to rated (1 kHz, 8 ohm; pp-el34-ul closed loop also at 50 Hz and 10 kHz,
                     and open loop at 1 kHz), Xyce harmonic balance, numfreq 60
  transfer curves    the quasi-static transfer curve at 1 kHz: input and output over one cycle of the steady state,
                     the output flipped where the circuit inverts (every preset in phase with its input, spec)
                     (Xyce harmonic balance, time domain) at 0.1 W, 1 W, rated, and twice the rated drive amplitude.
                     A DC sweep is meaningless through the coupling capacitors and the transformer; mid-band steady
                     state is the curve the program's static stage reproduces.
  response           small-signal frequency response 10 Hz - 200 kHz (ngspice AC; pp-el34-ul closed loop)

Output: data/ref/<preset>_sweep.csv, <preset>_transfer_<label>.csv, <preset>_response.csv, summary.json.
16 parallel processes (one math thread each); every job timed.

  python reference_sweep.py
"""
import json, math, os, subprocess, sys, tempfile, time
for _v in ("OMP_NUM_THREADS", "OPENBLAS_NUM_THREADS", "MKL_NUM_THREADS"):
    os.environ.setdefault(_v, "1")
from concurrent.futures import ProcessPoolExecutor
import numpy as np

import spice_amps as A, xyce_hb as X

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "data", "ref")
RATED = {"set-2a3": 4.83, "set-300b": 5.61, "set-845": 25.1, "pp-el34-ul": 20.0}   # 5 % THD; pp: the book's rating
POWERS = {"set-2a3": (0.1, 0.2, 0.5, 1, 2, 3, 3.5, 4.83), "set-300b": (0.1, 0.2, 0.5, 1, 2, 3, 5, 5.61),
          "set-845": (0.1, 0.2, 0.5, 1, 2, 5, 10, 20, 25.1), "pp-el34-ul": (0.1, 0.2, 0.5, 1, 2, 5, 10, 20, 27)}
SWEEPS = [(b, None, 1000.0) for b in ("set-2a3", "set-300b", "set-845")] + \
         [("pp-el34-ul", {"feedback": True}, f) for f in (1000.0, 50.0, 10000.0)] + [("pp-el34-ul", {"feedback": False}, 1000.0)]


def netlist(b, kw):
    return A.pp_520(**(kw or {})) if b == "pp-el34-ul" else A.PRESETS[b]()


def sweep_job(args):
    b, kw, f0, p = args
    t0 = time.time()
    try:
        r = X.at_power(b, p, f0, kw, numfreq=60)
        return dict(b=b, loop=None if kw is None else ("closed" if kw["feedback"] else "open"), f0=f0, P=p, amp=r["amp"],
                    THD=r["THD"], H=list(map(float, r["H"])), s=time.time() - t0)
    except RuntimeError as e:
        return dict(b=b, loop=None if kw is None else ("closed" if kw["feedback"] else "open"), f0=f0, P=p, error=str(e)[:200], s=time.time() - t0)


def transfer_job(args):
    b, label, amp = args
    t0 = time.time()
    kw = {"feedback": True} if b == "pp-el34-ul" else None
    net = [l.replace("{amp}", A.f(amp)).replace("{f0}", "1000") for l in netlist(b, kw)]
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
    a = np.array(rows)
    # polarity: every preset's output must be in phase with its input (spec); a circuit with an odd number of
    # inverting stages (set-300b) is flipped here, judged by the fundamental's phase
    n = len(a); k = np.exp(-2j * np.pi * np.arange(n) / n)
    inverted = bool(np.real(np.sum(a[:, 2] * k) / np.sum(a[:, 1] * k)) < 0)
    vout = -a[:, 2] if inverted else a[:, 2]
    return dict(b=b, label=label, amp=amp, t=a[:, 0].tolist(), vin=a[:, 1].tolist(), vout=vout.tolist(), inverted=inverted,
                s=time.time() - t0)


def response_job(b):
    t0 = time.time()
    kw = {"feedback": True} if b == "pp-el34-ul" else None
    net = [l if not l.startswith("Vin ") else "Vin in 0 DC 0 AC 1" for l in netlist(b, kw)]
    a = A.run(net, ["vdb(o)", "vp(o)"], 1000.0, 0, ac=(10, 200e3))
    f, db, ph = a[:, 0], a[:, 1], a[:, 3]
    i = np.argmin(abs(f - 1000))
    return dict(b=b, f=f.tolist(), db=(db - db[i]).tolist(), phase_deg=np.degrees(np.unwrap(ph) - np.unwrap(ph)[i]).tolist(),
                s=time.time() - t0)


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    t_all = time.time()
    sweep_cases = [(b, kw, f0, p) for b, kw, f0 in SWEEPS for p in POWERS[b] if not (kw and not kw["feedback"] and p > 20)]
    with ProcessPoolExecutor(16) as ex:
        sw = list(ex.map(sweep_job, sweep_cases))
        t_sweep = time.time() - t_all
        # drive amplitudes for the transfer curves from the 1 kHz sweep (closed loop for pp)
        amps = {}
        for r in sw:
            if r["f0"] == 1000.0 and r["loop"] in (None, "closed") and "amp" in r:
                amps.setdefault(r["b"], {})[r["P"]] = r["amp"]
        tcases = []
        for b, d in amps.items():
            rated = max(d)
            tcases += [(b, "0.1W", d[0.1]), (b, "1W", d[1]), (b, "rated", d[rated] if b != "pp-el34-ul" else d[20]),
                       (b, "2x_rated_drive", 2 * (d[rated] if b != "pp-el34-ul" else d[20]))]
        t1 = time.time()
        tr = list(ex.map(transfer_job, tcases))
        t_tr = time.time() - t1
        t1 = time.time()
        rs = list(ex.map(response_job, list(RATED)))
        t_rs = time.time() - t1
    for b in RATED:
        with open(os.path.join(OUT, f"{b}_sweep.csv"), "w") as h:
            h.write("loop,f_Hz,P_W,input_V_peak,THD_pct,H2_dB,H3_dB,H4_dB,H5_dB,H6_dB,H7_dB\n")
            for r in sw:
                if r["b"] == b and "H" in r:
                    h.write(f"{r['loop'] or ''},{r['f0']:g},{r['P']:g},{r['amp']:.6g},{r['THD']:.5g}," + ",".join(f"{x:.2f}" for x in r["H"]) + "\n")
        for r in tr:
            if r["b"] == b:
                with open(os.path.join(OUT, f"{b}_transfer_{r['label']}.csv"), "w") as h:
                    h.write("t_s,vin_V,vout_V\n")
                    for t, vi, vo in zip(r["t"], r["vin"], r["vout"]):
                        h.write(f"{t:.9g},{vi:.9g},{vo:.9g}\n")
        for r in rs:
            if r["b"] == b:
                with open(os.path.join(OUT, f"{b}_response.csv"), "w") as h:
                    h.write("f_Hz,gain_dB_re_1kHz,phase_deg_re_1kHz\n")
                    for f, d, p in zip(r["f"], r["db"], r["phase_deg"]):
                        h.write(f"{f:.6g},{d:.4f},{p:.3f}\n")
    errors = [r for r in sw if "error" in r]
    summary = dict(rated_W=RATED, n_sweep=len(sw), n_transfer=len(tr), n_response=len(rs), errors=errors,
                   polarity_flipped=sorted({r["b"] for r in tr if r["inverted"]}),
                   seconds=dict(total=time.time() - t_all, sweep=t_sweep, transfer=t_tr, response=t_rs,
                                sweep_job_mean=float(np.mean([r["s"] for r in sw])), transfer_job_mean=float(np.mean([r["s"] for r in tr])),
                                response_job_mean=float(np.mean([r["s"] for r in rs]))))
    with open(os.path.join(OUT, "summary.json"), "w") as h:
        json.dump(summary, h, indent=2)
    for r in sw:
        if "H" in r:
            print(f"{r['b']:<11} {r['loop'] or '':<6} {r['f0']:>6g} Hz {r['P']:>6g} W  THD {r['THD']:8.4f} %  " +
                  " ".join(f"H{i + 2} {x:7.1f}" for i, x in enumerate(r["H"])))
        else:
            print(f"{r['b']:<11} {r['loop'] or '':<6} {r['f0']:>6g} Hz {r['P']:>6g} W  ERROR {r['error'][:80]}")
    for r in rs:
        f, d = np.array(r["f"]), np.array(r["db"])
        print(f"{r['b']:<11} response: " + " ".join(f"{fr:g} Hz {d[np.argmin(abs(f - fr))]:+.2f}" for fr in (10, 20, 50, 10e3, 20e3, 50e3, 100e3, 200e3)))
    print(json.dumps(summary["seconds"], indent=1))
