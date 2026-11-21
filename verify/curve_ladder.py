"""M2 (N28): how far one static curve can stand for a preset. The quasi-static transfer curve (1 kHz, Xyce harmonic
balance, one period of the steady state) at a ladder of drive amplitudes, from well below rated to several times the
rated drive: whether each run converges, its loop (the rising and falling halves of the period apart: memory), and
how its mid-line agrees with the other runs' over the input range they share.

  python curve_ladder.py set-300b      writes data/curves/<preset>_ladder.json and the curves (csv)
"""
import json, os, subprocess, sys, tempfile, time
from concurrent.futures import ProcessPoolExecutor

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import spice_amps as A  # noqa: E402
import xyce_hb as X     # noqa: E402

OUT = os.path.join(HERE, "data", "curves")
RATED_DRIVE = {"set-300b": 4.213, "set-2a3": None, "set-845": None}   # V peak at rated power (data/ref transfer "rated")
MULT = (0.25, 0.5, 1.0, 1.5, 2.0, 3.0, 4.0, 5.0, 6.5, 8.0)


def run(args):
    b, amp, numfreq = args
    net = [l.replace("{amp}", A.f(amp)).replace("{f0}", "1000") for l in A.PRESETS[b]()]
    t0 = time.time()
    with tempfile.TemporaryDirectory() as td:
        cir = os.path.join(td, "t.cir")
        with open(cir, "w") as h:
            h.write("\n".join(X.to_xyce(net) + [".hb 1000", f".options hbint numfreq={numfreq}",
                                                  ".print hb v(in) v(o) v(g2) v(a2) v(k2)", ".end", ""]))
        r = subprocess.run([X.XYCE, cir], capture_output=True, text=True, timeout=X.TIMEOUT, cwd=td)
        p = cir + ".HB.TD.prn"
        if not os.path.exists(p):
            return {"amp": amp, "error": (r.stdout + r.stderr)[-600:], "s": time.time() - t0}
        rows = []
        with open(p) as h:
            for line in h:
                try:
                    rows.append([float(x) for x in line.split()[1:7]])
                except (ValueError, IndexError):
                    continue
    a = np.array(rows)
    n = len(a)
    k = np.exp(-2j * np.pi * np.arange(n) / n)
    inverted = bool(np.real(np.sum(a[:, 2] * k) / np.sum(a[:, 1] * k)) < 0)
    vout = -a[:, 2] if inverted else a[:, 2]
    return {"amp": amp, "t": a[:, 0].tolist(), "vin": a[:, 1].tolist(), "vout": vout.tolist(), "vg": a[:, 3].tolist(),
            "va": a[:, 4].tolist(), "vk": a[:, 5].tolist(), "inverted": inverted, "s": time.time() - t0}


def branches(vin, vout):
    """The rising and falling halves of the period as curves on a common input grid: (grid, rising, falling)."""
    vin, vout = np.asarray(vin), np.asarray(vout)
    i_max, i_min = int(np.argmax(vin)), int(np.argmin(vin))
    idx = np.arange(len(vin))
    def half(a, b):
        sel = idx[a:b + 1] if a <= b else np.r_[idx[a:], idx[:b + 1]]
        x, y = vin[sel], vout[sel]
        o = np.argsort(x)
        return x[o], y[o]
    xr, yr = half(i_min, i_max)
    xf, yf = half(i_max, i_min)
    lo, hi = max(xr.min(), xf.min()), min(xr.max(), xf.max())
    g = np.linspace(lo, hi, 801)
    return g, np.interp(g, xr, yr), np.interp(g, xf, yf)


if __name__ == "__main__":
    b = sys.argv[1] if len(sys.argv) > 1 else "set-300b"
    os.makedirs(OUT, exist_ok=True)
    rated = RATED_DRIVE[b]
    with ProcessPoolExecutor(10) as ex:
        res = list(ex.map(run, [(b, m * rated, 100) for m in MULT]))
    summary = []
    mids = []
    for m, r in zip(MULT, res):
        if "error" in r:
            print(f"{b} x{m:<4} {r['amp']:7.3f} V  ERROR ({r['s']:.1f} s): {r['error'].splitlines()[-1] if r['error'] else ''}")
            summary.append({"mult": m, "amp": r["amp"], "error": True})
            continue
        g, up, dn = branches(r["vin"], r["vout"])
        mid = (up + dn) / 2
        loop = float(np.max(np.abs(up - dn)) / np.max(np.abs(mid)))
        mids.append((m, g, mid))
        vg = np.array(r["vg"]); vk = np.array(r["vk"])
        summary.append({"mult": m, "amp": r["amp"], "s": r["s"], "loop_rel": loop, "vout_min": float(np.min(r["vout"])),
                        "vout_max": float(np.max(r["vout"])), "vgk_max": float(np.max(vg - vk)), "inverted": r["inverted"]})
        np.savetxt(os.path.join(OUT, f"{b}_ladder_x{m:g}.csv"), np.c_[g, mid, up, dn], delimiter=",",
                   header="vin_V,vout_mid_V,vout_rising_V,vout_falling_V", comments="")
        print(f"{b} x{m:<4} {r['amp']:7.3f} V  {r['s']:5.1f} s  out {np.min(r['vout']):8.3f} .. {np.max(r['vout']):7.3f} V"
              f"  loop {100 * loop:6.2f} %  max vg-vk {np.max(vg - vk):+7.2f} V", flush=True)
    # agreement: each run's mid-line against the largest run's, over the smaller run's range
    if mids:
        mb, gb, yb = mids[-1]
        for m, g, y in mids[:-1]:
            yy = np.interp(g, gb, yb)
            span = np.max(np.abs(y))
            print(f"  x{m:<4} vs x{mb:g}: max |difference| {100 * np.max(np.abs(y - yy)) / span:6.2f} % of its output span")
    json.dump(summary, open(os.path.join(OUT, f"{b}_ladder.json"), "w"), indent=1)
