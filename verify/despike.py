"""Remove spikes from digitized plate curves: points that stick out of the curve, mostly short stretches of a
thicker grid line picked up where a curve crosses it, or a blot on the scan.

Pentodes only. Past the knee (Ep >= 30 V) every pentode curve is a single-valued function of Ep: a point's deviation is its vertical
distance, in pixels of the scan, from the running median of 7 neighbours in Ep. The worst point above tol_px (4) is
removed and the medians recomputed, until none is left. Graphs traced by hand ("manual" in digitize.json) are left
alone: their bends (the EL34's secondary-emission dip) are the tube's.

The pixel scale of each curve comes from digitize.json (the graph it was digitized on).

  python despike.py                 dry run: every tube, list what would go
  python despike.py EL34 --write    remove them
"""
import json, os, re, sys
import numpy as np

import digitize_curves as D

NAME = re.compile(r"^vg([+-]?\d+(?:\.\d+)?)(?:_vg2(\d+(?:\.\d+)?))?\.csv$")


def scales(tube):
    """(vg2 or None) -> (px per x unit, px per y unit) for the plate graphs of the tube."""
    d = os.path.join(D.DATA, tube)
    src = json.load(open(os.path.join(d, "source.json"), encoding="utf-8"))
    spec = json.load(open(os.path.join(d, "digitize.json"), encoding="utf-8"))
    out = {}
    for g in spec["graphs"]:
        if g.get("kind", "plate") != "plate":
            continue
        page = D.page_image(g.get("url", src["datasheet"]["url"]), g["page"])
        if g.get("rotate"):
            page = page.rotate(g["rotate"], expand=True, fillcolor=255)
        x0, y0, x1, y1 = g["region"]
        _, _, info = D.calibrate(np.array(page)[y0:y1, x0:x1] < 128, g)
        out[g.get("vg2")] = (info["x_px"] / abs(info["x_step"]), info["y_px"] / abs(info["y_step"]), bool(g.get("manual")))
    return out


def spikes(a, sx, sy, tol, knee=30.0, win=7):
    """Points more than tol px (vertical, on the scan) off the running median of their curve, past the knee
    (Ep >= knee V, where every curve is a single-valued function of Ep). The worst goes first, then the medians
    are recomputed: a vertical spike of several points at one Ep leaves the median where the curve is."""
    order = np.argsort(a[:, 0], kind="stable")
    keep = [int(i) for i in order]
    gone = []
    while True:
        q = a[keep]
        ys = q[:, 1]
        h = win // 2
        # symmetric windows, narrower towards the ends (a one-sided window lags a rising curve)
        med = np.array([np.median(ys[i - k:i + k + 1]) for i in range(len(ys)) for k in [min(h, i, len(ys) - 1 - i)]])
        dev = np.abs(ys - med) * sy
        dev[q[:, 0] < knee] = 0.0
        i = int(np.argmax(dev))
        if dev[i] <= tol:
            break
        gone.append((keep[i], float(dev[i])))
        del keep[i]
    return keep, gone


def run(tube, write, tol):
    d = os.path.join(D.DATA, tube)
    if json.load(open(os.path.join(d, "source.json"), encoding="utf-8"))["model"] != "pentode":
        print(f"{tube}: triode, skipped (steep curves: neighbours differ by far more than a spike)")
        return
    sc = scales(tube)
    total = 0
    for f in sorted(os.listdir(d)):
        m = NAME.match(f)
        if not m:
            continue
        vg2 = float(m.group(2)) if m.group(2) else None
        sx, sy, manual = sc.get(vg2) or sc.get(None) or next(iter(sc.values()))
        if manual:
            continue   # traced by hand: its bends are the tube's (EL34 page 11)
        a = np.loadtxt(os.path.join(d, f), delimiter=",", skiprows=1, ndmin=2)
        keep, gone = spikes(a, sx, sy, tol)
        if not gone:
            continue
        total += len(gone)
        print(f"  {tube}/{f}: " + ", ".join(f"{a[i, 0]:.0f} V {a[i, 1]:.3g} mA ({dv:.0f} px)" for i, dv in sorted(gone)))
        if write:
            with open(os.path.join(d, f), "w", encoding="utf-8", newline="\n") as h:
                h.write("Ep_V,Ip_mA\n")
                for x, y in a[sorted(keep)]:
                    h.write(f"{x:.3f},{y:.4f}\n")
    print(f"{tube}: {total} points" + (" removed" if write else " would go"))


if __name__ == "__main__":
    write = "--write" in sys.argv
    tol = 4.0
    tubes = [a for a in sys.argv[1:] if not a.startswith("-")] or sorted(
        t for t in os.listdir(D.DATA) if os.path.isfile(os.path.join(D.DATA, t, "digitize.json")))
    for t in tubes:
        run(t, write, tol)
