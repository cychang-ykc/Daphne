"""Core vs linear transformer at 1 kHz (N28, the basis for keeping the core out of the M2 reference data): the four
presets (pp at 6 dB) at P_ref and 1 W. python core_1khz_check.py  (variant inside: base2; the Hc x0.5 run edits it)"""
import os, sys, json, time
for v in ("OMP_NUM_THREADS", "OPENBLAS_NUM_THREADS", "MKL_NUM_THREADS"): os.environ[v] = "1"
sys.path.insert(0, os.getcwd())
from concurrent.futures import ProcessPoolExecutor
import core_presets as C
PREF = {"set-2a3": 0.115, "set-300b": 0.279, "set-845": 0.077, "pp-el34-ul": 1.25}
if __name__ == "__main__":
    cases = [(b, 6 if b == "pp-el34-ul" else None, "base2", 1000.0, p) for b in PREF for p in (PREF[b], 1.0)]
    t0 = time.time()
    with ProcessPoolExecutor(8) as ex:
        res = list(ex.map(C.job, cases))
    json.dump(res, open("data/core/core_1kHz.json", "w"), indent=1, default=float)
    for r in res:
        if "error" in r: print(r["preset"], r["f0"], "ERROR", r["error"][:80]); continue
        d2, d3 = r["H"][0] - r["lin_H"][0], r["H"][1] - r["lin_H"][1]
        print(f"{r['preset']:<11} {r['P']:6.3f} W | core THD {r['THD']:.4f} % H2 {r['H'][0]:6.1f} H3 {r['H'][1]:6.1f} | linear THD {r['lin_THD']:.4f} % H2 {r['lin_H'][0]:6.1f} H3 {r['lin_H'][1]:6.1f} | dH2 {d2:+5.2f} dH3 {d3:+5.2f} dB {'OK' if abs(d2) <= 1.5 and abs(d3) <= 1.5 else 'OVER'} | {r['s']:.0f} s")
    print("%.0f s" % (time.time() - t0))
