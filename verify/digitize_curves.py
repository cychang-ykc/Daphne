"""Plate curves digitized from a scanned datasheet graph: one CSV per grid voltage for koren_fit.py.

Input: verify/data/tubes/<tube>/digitize.json
  { "graphs": [ { "page": 3,                      1-based page of the datasheet PDF (source.json "datasheet.url")
                  "region": [x0, y0, x1, y1],     page-image pixels enclosing the plot frame (a loose box)
                  "x": [0, 700], "y": [0, 360],   values at the frame edges (left, right / bottom, top), as printed
                  "x_step": 25, "y_step": 20,     the grid step as printed (checked against the lines found)
                  "curves": [0, -20, ...],        grid voltages (plate) or plate voltages (other kinds), left to right
                  optional: "kind" (plate | transfer | rp | mu | gm), "rotate" (deg), "url", "open_px", "exclude",
                  "exclude_lines", "line_px", "min_piece_px", "gap_px", "min_extent", "direction", "order", "vg2",
                  "outlier_px", "outlier_half", "trim_top_px", "smooth_px", "join_deg", "grid_piece_px", "drop_vertical", "drop_extra_right",
                  "manual" (a reason: the graph is skipped, its CSVs come from WebPlotDigitizer)
                  "vg2": 250 } ] }                pentodes: the screen voltage of this graph (optional)

Steps:
  1. The page's embedded scan is extracted at its native resolution (scanned datasheets are one image per page;
     rendering at a higher dpi would only interpolate).
  2. Thin lines (grid, frame) and thick lines (the curves) are separated by a 3 x 3 opening: the scans draw the grid
     1 - 2 px wide and the curves 4 - 5 px.
  3. Calibration: the grid's vertical and horizontal line families are found by projecting the dark pixels at
     small angles (the scan's skew, up to +-1.5 deg) and taking the sharpest profile; a uniform comb is fitted to the
     peaks (spacing, offset, skew). Pixel -> value is then affine, with the frame lines as the first and last grid
     line. The grid step in volts / mA follows from the printed range and the number of grid intervals.
  4. The thick-line image, with the frame bands masked, is split into connected components; components shorter
     than 15 % of the plot height are text. They must number len(curves); left to right they get the grid voltages.
  5. Centre line: per pixel row the centre of a short horizontal run (steep parts), per column the centre of a
     short vertical run (flat parts); merged, ordered along the curve and resampled to about every 1 % of the plot
     diagonal.
Output: <tube>/vg<Vg>[_vg2<Vg2>].csv (header "Ep_V,Ip_mA") and <tube>/overlay.png (the extracted points and the
fitted grid drawn on the scan), plus the calibration residuals on stdout.

The scan itself is not stored: the PDF is downloaded into verify/data/tubes/.cache (not under version control).

  python digitize_curves.py 300B          every graph in 300B/digitize.json
  python digitize_curves.py 300B 2 3      only graphs 2 and 3
"""
import io, json, math, os, sys, urllib.request
import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage as ndi

DATA = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data", "tubes")
CACHE = os.path.join(DATA, ".cache")
COLORS = [(230, 25, 75), (60, 180, 75), (0, 130, 200), (245, 130, 48), (145, 30, 180), (70, 190, 190), (240, 50, 230),
          (128, 128, 0), (0, 0, 128), (170, 110, 40), (128, 0, 0), (0, 128, 128)]


def page_image(url, page):
    import pypdf
    os.makedirs(CACHE, exist_ok=True)
    fn = os.path.join(CACHE, url.replace("://", "_").replace("/", "_"))
    if not os.path.exists(fn):
        req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
        with urllib.request.urlopen(req, timeout=60) as r, open(fn + ".part", "wb") as f:
            f.write(r.read())
        os.replace(fn + ".part", fn)
    imgs = pypdf.PdfReader(fn).pages[page - 1].images
    if not imgs:
        raise SystemExit(f"page {page}: no scanned image (a text PDF needs another route)")
    im = max((i.image for i in imgs), key=lambda i: i.size[0] * i.size[1])
    return im.convert("L")


def line_family(thin, vertical):
    """Positions (at the region's first row / column) and slope of a family of straight grid lines."""
    h, w = thin.shape
    ys, xs = np.nonzero(thin)
    best = None
    for deg in np.arange(-1.5, 1.5001, 0.02):
        t = math.tan(math.radians(deg))
        # vertical lines: x drifts by t per row; horizontal: y drifts by t per column
        coord = (xs - ys * t) if vertical else (ys - xs * t)
        prof = np.bincount(np.clip(np.round(coord).astype(int) + 50, 0, None), minlength=(w if vertical else h) + 100)
        score = float(np.sum(prof.astype(np.float64) ** 2))
        if best is None or score > best[0]:
            best = (score, t, prof)
    _, t, prof = best
    span = h if vertical else w
    # a grid line covers at least 35 % of the span (dashed lines included). Local maxima at least 5 px apart, each
    # refined to the weighted centre of +-2 px: a thick (or double) frame line next to a grid line stays two peaks
    from scipy.signal import find_peaks
    pk, _ = find_peaks(prof.astype(np.float64), height=0.35 * span, distance=5)
    peaks = []
    for i in pk:
        idx = np.arange(max(i - 2, 0), min(i + 3, len(prof)))
        wts = prof[idx].astype(np.float64)
        peaks.append(float(np.sum(idx * wts) / np.sum(wts)) - 50)
    return np.array(sorted(peaks)), t


def comb(peaks):
    """Uniform comb through the peaks: index k of each peak, spacing s, offset o (p = o + s k). Peaks more than
    0.25 spacing off the comb (a curve or text crossing the projection) are left out."""
    d = np.diff(peaks)
    med = float(np.median(d))
    # spacing: the mean of the gaps near the median (scans alternate e.g. 11.3 / 11.7 px; a median would drift by a
    # whole interval over 80 lines)
    s = float(np.mean(d[np.abs(d - med) < 0.25 * med]))
    # indices step by step from neighbour to neighbour (a missing line counts 2), then one least-squares comb
    k = np.r_[0, np.cumsum(np.maximum(np.round(d / s), 0))]
    keep = np.r_[True, np.round(d / s) >= 1]   # a peak closer than half a spacing to its neighbour is not a line
    for _ in range(3):
        A = np.stack([np.ones_like(k), k], 1)
        (o, s), *_ = np.linalg.lstsq(A[keep], peaks[keep], rcond=None)
        res = peaks - (o + s * k)
        keep = keep & (np.abs(res) < 0.25 * s)
    peaks, k = peaks[keep], k[keep]
    res = peaks - (o + s * k)
    return k.astype(int), float(s), float(o), res


def calibrate(thin, g):
    vx, tx = line_family(thin, True)
    hy, ty = line_family(thin, False)
    kx, sx, ox, rx = comb(vx)
    ky, sy, oy, ry = comb(hy)
    nx, ny = int(kx.max() - kx.min()), int(ky.max() - ky.min())
    x0k, y0k = int(kx.min()), int(ky.max())   # frame: leftmost and lowest lines
    xv, yv = g["x"], g["y"]

    def to_value(u, v):
        # undo the skew: grid index of the point along each family
        kxu = ((u - v * tx) - ox) / sx
        kyv = ((v - u * ty) - oy) / sy
        x = xv[0] + (kxu - x0k) * (xv[1] - xv[0]) / nx
        y = yv[0] + (y0k - kyv) * (yv[1] - yv[0]) / ny
        return x, y

    def to_pixel(x, y):
        kxu = x0k + (x - xv[0]) * nx / (xv[1] - xv[0])
        kyv = y0k - (y - yv[0]) * ny / (yv[1] - yv[0])
        a, b = ox + sx * kxu, oy + sy * kyv     # u - v tx = a, v - u ty = b
        v = (b + a * ty) / (1 - tx * ty)
        return a + v * tx, v

    info = {"x_lines": len(vx), "x_intervals": nx, "x_step": (xv[1] - xv[0]) / nx, "x_px": sx,
            "x_res_px": float(np.abs(rx).max()), "y_lines": len(hy), "y_intervals": ny, "y_step": (yv[1] - yv[0]) / ny,
            "y_px": sy, "y_res_px": float(np.abs(ry).max()), "skew_deg": math.degrees(math.atan(tx)),
            "skew_h_deg": math.degrees(math.atan(ty))}
    return to_value, to_pixel, info


def centre_points(mask, maxrun):
    """Centre points of a thick line: short runs per row and per column."""
    pts = []
    for axis in (0, 1):   # 0: rows (horizontal runs), 1: columns (vertical runs)
        m = mask if axis == 0 else mask.T
        for i in np.nonzero(m.any(1))[0]:
            row = m[i]
            idx = np.nonzero(row)[0]
            breaks = np.nonzero(np.diff(idx) > 1)[0]
            for seg in np.split(idx, breaks + 1):
                if len(seg) <= maxrun:
                    c = 0.5 * (seg[0] + seg[-1])
                    pts.append((c, i) if axis == 0 else (i, c))
    return np.array(pts, dtype=np.float64)


def drop_outliers(pts, tol=2.5, half=10):
    """Centre points (pixels) that stray from the local line through their neighbours along the curve: the strokes
    of a label touching the curve. Ordered along the main direction; each point is compared with the line fitted
    (PCA) to the 2 * half neighbours around it."""
    if len(pts) < 2 * half + 1:
        return pts
    c = pts - pts.mean(0)
    _, _, vt = np.linalg.svd(c, full_matrices=False)
    p = pts[np.argsort(c @ vt[0], kind="stable")]
    keep = np.ones(len(p), bool)
    for i in range(len(p)):
        a, b = max(0, i - half), min(len(p), i + half + 1)
        q = p[a:b]
        m = np.median(q, 0)
        _, _, w = np.linalg.svd(q - m, full_matrices=False)
        n = np.array([-w[0][1], w[0][0]])
        keep[i] = abs((p[i] - m) @ n) <= tol
    return p[keep]


def smooth_reject(res, px_x, px_y, tol_px, deg=5):
    """Points (values) farther than tol_px (perpendicular, on the page) from a smooth polynomial y(x) through the
    curve, removed one at a time from the worst: a label lying along the curve. The curves are monotonic, so y(x)
    is single valued."""
    keep = np.ones(len(res), bool)
    while keep.sum() > deg + 3:
        x, y = res[keep, 0], res[keep, 1]
        c = np.polyfit(x, y, deg)
        slope = np.polyval(np.polyder(c), res[:, 0]) * (px_y / px_x)   # dy/dx on the page
        dist = np.abs(res[:, 1] - np.polyval(c, res[:, 0])) * px_y / np.sqrt(1 + slope ** 2)
        dist[~keep] = 0
        i = int(np.argmax(dist))
        if dist[i] <= tol_px:
            break
        keep[i] = False
    return res[keep]


def order_and_resample(xy, step):
    """Order points along a monotonic curve (by their projection on the curve's main direction) and average them
    in bins of `step` arc length (normalized plot units)."""
    c = xy - xy.mean(0)
    _, _, vt = np.linalg.svd(c, full_matrices=False)
    xy = xy[np.argsort(c @ vt[0], kind="stable")]
    if xy[-1, 0] < xy[0, 0]:
        xy = xy[::-1]                      # left to right
    seg = np.r_[0.0, np.cumsum(np.hypot(*np.diff(xy, axis=0).T))]
    out = []
    edges = np.arange(0.0, seg[-1] + step, step)
    for a, b in zip(edges[:-1], edges[1:]):
        sel = (seg >= a) & (seg < b)
        if sel.sum() >= 2:
            out.append(xy[sel].mean(0))
    return np.array(out)


def select_curves(cur, g, plot_w, plot_h):
    """The curves as pixel masks, left to right.

    Pieces: connected components at least min_piece_px long (shorter ones are text, dashes of a dashed line,
    specks), and not a straight axis-parallel stretch (a grid line drawn thicker in places). A curve cut by a label written into it or by a masked load line is several pieces: the end of one
    is joined to the start of the next when they are within gap_px and the gap continues both pieces' direction
    (within join_deg, default 35 deg). Pieces are oriented along the curve's direction ("up": rising to the right, the default;
    "down": falling, e.g. rp vs grid voltage). Chains spanning at least min_extent of the plot are the curves.
    """
    lab, n = ndi.label(cur, structure=np.ones((3, 3), bool))
    sl = ndi.find_objects(lab)
    min_piece = g.get("min_piece_px", 30)
    sign = -1.0 if g.get("direction", "up") == "down" else 1.0
    pieces = []
    for k, s in enumerate(sl):
        if s is None or max(s[0].stop - s[0].start, s[1].stop - s[1].start) < min_piece:
            continue
        v, u = np.nonzero(lab[s] == k + 1)
        p = np.stack([u + s[1].start, v + s[0].start], 1).astype(np.float64)
        c = p - p.mean(0)
        _, sv, vt = np.linalg.svd(c, full_matrices=False)
        ax = vt[0]
        # a stretch of a thicker grid line: straight (thin across) and parallel to an axis
        straight = sv[1] / math.sqrt(len(p)) < g.get("grid_piece_px", 1.2)
        if g.get("grid_piece_px", 1.2) > 0 and straight and min(abs(ax[0]), abs(ax[1])) < math.sin(math.radians(1.5)):
            continue
        # vertical straight marks only (pentode plate curves are nearly horizontal and straight: keep those)
        if g.get("drop_vertical") and sv[1] / math.sqrt(len(p)) < 2.0 and abs(ax[0]) < math.sin(math.radians(1.5)):
            continue
        # orient: "up" curves go right and up the page (u grows, v falls), "down" ones right and down
        if ax[0] - sign * ax[1] < 0:
            ax = -ax
        t = c @ ax
        o = np.argsort(t)
        e = max(3, len(o) // 20)
        a, b = p[o[:e]].mean(0), p[o[-e:]].mean(0)
        # end directions from the last / first 25 % of the piece
        q = max(e, len(o) // 4)
        da = p[o[q:2 * q]].mean(0) - a if len(o) >= 2 * q else ax
        db = b - p[o[-2 * q:-q]].mean(0) if len(o) >= 2 * q else ax
        pieces.append({"k": k + 1, "start": a, "end": b, "dstart": da / (np.linalg.norm(da) + 1e-9),
                       "dend": db / (np.linalg.norm(db) + 1e-9)})
    gap = g.get("gap_px", 40)
    links = []
    for i, A in enumerate(pieces):
        for j, B in enumerate(pieces):
            if i == j:
                continue
            v = B["start"] - A["end"]
            dist = float(np.linalg.norm(v))
            if dist > gap or dist < 1e-6:
                if dist < 1e-6:
                    links.append((0.0, i, j))
                continue
            w = v / dist
            cj = math.cos(math.radians(g.get("join_deg", 35)))
            if w @ A["dend"] > cj and w @ B["dstart"] > cj:
                links.append((dist, i, j))
    links.sort()
    nxt, prv = {}, {}
    for _, i, j in links:
        if i in nxt or j in prv:
            continue
        # no cycles
        h = j
        while h in nxt:
            h = nxt[h]
        if h == i:
            continue
        nxt[i], prv[j] = j, i
    masks = []
    for i in range(len(pieces)):
        if i in prv:
            continue
        chain, h = [i], i
        while h in nxt:
            h = nxt[h]
            chain.append(h)
        m = np.isin(lab, [pieces[c]["k"] for c in chain])
        v, u = np.nonzero(m)
        if max((v.max() - v.min()) / plot_h, (u.max() - u.min()) / plot_w) > g.get("min_extent", 0.15):
            masks.append(m)
    # left to right: by the x of each curve's lowest point (default) or its mean x; "top": top to bottom
    def key(m):
        v, u = np.nonzero(m)
        if g.get("order") == "top":
            return v.mean()   # stacked curves (pentode plate curves): top (most current) first
        return u.mean() if g.get("order") == "mean" else u[np.argmax(v)]
    masks.sort(key=key)
    return masks


# what a graph plots: the CSV header and the file name of a curve
KINDS = {
    "plate":    ("Ep_V,Ip_mA",   lambda q: f"vg{q}"),      # Ip vs Ep, parameter Vg
    "transfer": ("Eg_V,Ip_mA",   lambda q: f"tr_eb{q}"),   # Ip vs Eg, parameter Eb
    "rp":       ("Eg_V,rp_ohm",  lambda q: f"rp_eb{q}"),   # plate resistance vs Eg, parameter Eb
    "mu":       ("Eg_V,mu",      lambda q: f"mu_eb{q}"),
    "gm":       ("Eg_V,gm_umho", lambda q: f"gm_eb{q}"),
}


def fmt(v):
    return "0" if v == 0 else (f"+{v:g}" if v > 0 else f"{v:g}")


def digitize(tube):
    d = os.path.join(DATA, tube)
    with open(os.path.join(d, "source.json"), encoding="utf-8") as f:
        src = json.load(f)
    with open(os.path.join(d, "digitize.json"), encoding="utf-8") as f:
        spec = json.load(f)
    plate_vg2 = {g.get("vg2") for g in spec["graphs"] if g.get("kind", "plate") == "plate"}
    overlays = {"overlay.png": [], "overlay_check.png": []}
    only = [int(a) for a in sys.argv[2:] if a.isdigit()]
    for gi, g in enumerate(spec["graphs"]):
        if only and gi + 1 not in only:
            continue
        if g.get("manual"):
            print(f"{tube} graph {gi + 1} (page {g['page']}): digitized by hand ({g['manual']})")
            continue
        kind = g.get("kind", "plate")
        header, fname = KINDS[kind]
        page = page_image(g.get("url", src["datasheet"]["url"]), g["page"])
        if g.get("rotate"):
            page = page.rotate(g["rotate"], expand=True, fillcolor=255)
        x0, y0, x1, y1 = g["region"]
        gray = np.array(page)[y0:y1, x0:x1]
        dark = gray < 128
        op = g.get("open_px", 3)   # wider than the grid lines, narrower than the curves
        thick = ndi.binary_opening(dark, structure=np.ones((op, op), bool))
        # the frame (3 px) survives the opening, so lines are found on the whole dark image: a curve covers far less
        # than 35 % of the span in any projection
        to_value, to_pixel, info = calibrate(dark, g)
        print(f"{tube} graph {gi + 1} ({kind}, page {g['page']}): x {info['x_intervals']} intervals of {info['x_step']:g} "
              f"({info['x_px']:.2f} px, comb residual max {info['x_res_px']:.2f} px), y {info['y_intervals']} intervals "
              f"of {info['y_step']:g} ({info['y_px']:.2f} px, residual max {info['y_res_px']:.2f} px), "
              f"skew {info['skew_deg']:+.3f} / {info['skew_h_deg']:+.3f} deg")
        for ax in ("x", "y"):
            want = g.get(ax + "_step")
            if want and abs(info[ax + "_step"] - want) > 1e-9 * abs(want):
                raise SystemExit(f"{tube} page {g['page']}: {ax} grid step {info[ax + '_step']:g}, expected {want:g} "
                                 f"({info[ax + '_lines']} lines found): check the region or digitize by hand")

        # frame bands (and everything outside the frame) masked: the curves start on the axis
        vv, uu = np.mgrid[0:gray.shape[0], 0:gray.shape[1]]
        X, Y = to_value(uu.astype(np.float64), vv.astype(np.float64))
        mpx = g.get("mask_px", 6)
        bx = mpx * abs(info["x_step"]) / info["x_px"]
        by = mpx * abs(info["y_step"]) / info["y_px"]
        (xa, xb), (ya, yb) = g["x"], g["y"]
        lo_x, hi_x, lo_y, hi_y = min(xa, xb), max(xa, xb), min(ya, yb), max(ya, yb)
        inside = (X > lo_x + bx) & (X < hi_x - bx) & (Y > lo_y + by) & (Y < hi_y - by)
        for box in g.get("exclude", []):   # value-space boxes [x0, y0, x1, y1]: labels touching a curve
            inside &= ~((X >= box[0]) & (X <= box[2]) & (Y >= box[1]) & (Y <= box[3]))
        # straight lines drawn across the curves (a load line): value-space segments [x0, y0, x1, y1], masked
        # line_px either side; the curves they cut are joined again below
        for seg in g.get("exclude_lines", []):
            (pa, qa), (pb, qb) = to_pixel(seg[0], seg[1]), to_pixel(seg[2], seg[3])
            du, dv = pb - pa, qb - qa
            t = np.clip(((uu - pa) * du + (vv - qa) * dv) / (du * du + dv * dv), 0.0, 1.0)
            inside &= np.hypot(uu - (pa + t * du), vv - (qa + t * dv)) > g.get("line_px", 7)
        cur = thick & inside
        plot_h = info["y_px"] * info["y_intervals"]
        plot_w = info["x_px"] * info["x_intervals"]
        masks = select_curves(cur, g, plot_w, plot_h)
        if g.get("drop_extra_right") and len(masks) > len(g["curves"]):
            masks = masks[:len(g["curves"])]   # curves past the last one listed (too crowded to separate) are left out
        if len(masks) != len(g["curves"]):
            raise SystemExit(f"{tube} page {g['page']}: found {len(masks)} curves, expected {len(g['curves'])} "
                             f"(curves touching or crossing: digitize this graph by hand)")

        written = []
        norm = np.array([xb - xa, yb - ya], dtype=np.float64)
        for m, q in zip(masks, g["curves"]):
            pts = centre_points(m, maxrun=g.get("maxrun", 9))
            pts = drop_outliers(pts, tol=g.get("outlier_px", 2.5), half=g.get("outlier_half", 10))
            if g.get("trim_top_px"):
                # a label written on the curve's upper end, in its direction: drop that stretch (pixels, along the curve)
                top = pts[np.argmin(pts[:, 1])]
                pts = pts[np.hypot(*(pts - top).T) > g["trim_top_px"]]
            vals = np.stack(to_value(pts[:, 0], pts[:, 1]), 1)
            # resample every 1 % of the plot side, in plot units
            res = order_and_resample(vals / norm, 0.01) * norm
            if g.get("smooth_px"):
                res = smooth_reject(res, info["x_px"] / abs(info["x_step"]), info["y_px"] / abs(info["y_step"]), g["smooth_px"])
            vg2 = f"_vg2{g['vg2']:g}" if g.get("vg2") and (kind != "plate" or len(plate_vg2) > 1) else ""
            name = fname(fmt(q) if kind == "plate" else f"{q:g}") + vg2 + ".csv"
            with open(os.path.join(d, name), "w", encoding="utf-8", newline="\n") as f:
                f.write(header + "\n")
                for x, y in res:
                    f.write(f"{x:.3f},{y:.4f}\n")
            written.append((q, res, name))
            hx, hy = header.split(",")
            print(f"  {'Vg' if kind == 'plate' else 'Eb'} {q:>6g}: {len(res):>3} points, {hx} {res[:, 0].min():.4g} .. "
                  f"{res[:, 0].max():.4g}, {hy} {res[:, 1].min():.4g} .. {res[:, 1].max():.4g} -> {name}")

        # overlay: the scan, the fitted grid (blue), the extracted points (one colour per curve)
        rgb = Image.fromarray(gray).convert("RGB")
        dr = ImageDraw.Draw(rgb)
        for i in range(info["x_intervals"] + 1):
            xv = xa + i * info["x_step"]
            dr.line([to_pixel(xv, ya), to_pixel(xv, yb)], fill=(120, 170, 255), width=1)
        for i in range(info["y_intervals"] + 1):
            yv = ya + i * info["y_step"]
            dr.line([to_pixel(xa, yv), to_pixel(xb, yv)], fill=(120, 170, 255), width=1)
        for ci, (q, res, name) in enumerate(written):
            c = COLORS[ci % len(COLORS)]
            for x, y in res:
                u, v = to_pixel(x, y)
                dr.ellipse([u - 3, v - 3, u + 3, v + 3], outline=c, width=2)
            u, v = to_pixel(*res[-1])
            dr.text((u + 6, v + 4), f"{q:g} V", fill=c)
        dr.text((8, 4), f"{tube} page {g['page']} ({kind})", fill=(200, 0, 0))
        overlays["overlay.png" if kind == "plate" else "overlay_check.png"].append(rgb)
    for name, imgs in overlays.items():
        if not imgs:
            continue
        w = max(o.size[0] for o in imgs)
        out = Image.new("RGB", (w, sum(o.size[1] for o in imgs)), "white")
        y = 0
        for o in imgs:
            out.paste(o, (0, y))
            y += o.size[1]
        out.save(os.path.join(d, name))
        print(f"  {name}: {os.path.join(d, name)}")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    digitize(sys.argv[1])
