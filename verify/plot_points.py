"""The points koren_fit.py fits, drawn on the datasheet scans: every vg*.csv of a tube (automatic and hand-traced
alike, after merging) on the plate graph it came from, with the grid calibrated as in digitize_curves.py.

One colour per curve, the grid voltage at the curve's last point; the blue grid is the calibration. Output:
verify/data/tubes/<tube>/review_p<page>.png (full resolution).

  python plot_points.py                every tube with a digitize.json
  python plot_points.py EF86 EL34
"""
import json, os, re, sys
import numpy as np
from PIL import Image, ImageDraw

import digitize_curves as D

NAME = re.compile(r"^vg([+-]?\d+(?:\.\d+)?)(?:_vg2(\d+(?:\.\d+)?))?\.csv$")


def review(tube):
    d = os.path.join(D.DATA, tube)
    src = json.load(open(os.path.join(d, "source.json"), encoding="utf-8"))
    spec = json.load(open(os.path.join(d, "digitize.json"), encoding="utf-8"))
    plates = [g for g in spec["graphs"] if g.get("kind", "plate") == "plate"]
    files = sorted(f for f in os.listdir(d) if NAME.match(f))
    for g in plates:
        page = D.page_image(g.get("url", src["datasheet"]["url"]), g["page"])
        if g.get("rotate"):
            page = page.rotate(g["rotate"], expand=True, fillcolor=255)
        x0, y0, x1, y1 = g["region"]
        gray = np.array(page)[y0:y1, x0:x1]
        to_value, to_pixel, info = D.calibrate(gray < 128, g)
        im = Image.fromarray(gray).convert("RGB")
        dr = ImageDraw.Draw(im)
        (xa, xb), (ya, yb) = g["x"], g["y"]
        for i in range(info["x_intervals"] + 1):
            xv = xa + i * info["x_step"]
            dr.line([to_pixel(xv, ya), to_pixel(xv, yb)], fill=(170, 200, 255), width=1)
        for i in range(info["y_intervals"] + 1):
            yv = ya + i * info["y_step"]
            dr.line([to_pixel(xa, yv), to_pixel(xb, yv)], fill=(170, 200, 255), width=1)
        n = 0
        mine = []
        for f in files:
            m = NAME.match(f)
            vg2 = float(m.group(2)) if m.group(2) else None
            if g.get("vg2") and vg2 is not None and vg2 != g["vg2"]:
                continue
            mine.append((float(m.group(1)), f))
        for ci, (vg, f) in enumerate(sorted(mine, reverse=True)):
            a = np.loadtxt(os.path.join(d, f), delimiter=",", skiprows=1, ndmin=2)
            c = D.COLORS[ci % len(D.COLORS)]
            for x, y in a:
                u, v = to_pixel(x, y)
                dr.ellipse([u - 4, v - 4, u + 4, v + 4], outline=c, width=2)
            u, v = to_pixel(*a[-1])
            dr.text((u + 7, v - 12), f"{vg:g} V", fill=c)
            n += len(a)
        title = f"{tube} page {g['page']}" + (f", Vg2 = {g['vg2']:g} V" if g.get("vg2") else "") + f": {len(mine)} curves, {n} points"
        dr.text((8, 4), title, fill=(200, 0, 0))
        out = os.path.join(d, f"review_p{g['page']}.png")
        im.save(out)
        print(f"{title} -> {out}")


if __name__ == "__main__":
    tubes = sys.argv[1:] or sorted(t for t in os.listdir(D.DATA) if os.path.isfile(os.path.join(D.DATA, t, "digitize.json")))
    for t in tubes:
        review(t)
