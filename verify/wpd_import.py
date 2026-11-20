"""A WebPlotDigitizer project (JSON) to plate-curve CSVs for koren_fit.py: the graphs digitize_curves.py cannot
separate are traced by hand.

Each dataset is one curve; its name is the grid voltage ("-32V", "-4 V", "0V"). The values are taken as the
project computed them from its own axis calibration, which must be linear XY axes in V (x) and mA (y). The
project file is copied next to the CSVs, so the source of every point stays in the repository.

  python wpd_import.py EL34 path/to/EL34.json --vg2 360 [--page 11]
    -> verify/data/tubes/EL34/vg-32_vg2360.csv ... and EL34/wpd_p11.json
"""
import argparse, json, os, re, shutil
import numpy as np

DATA = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data", "tubes")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tube")
    ap.add_argument("project")
    ap.add_argument("--vg2", type=float, help="screen voltage of this graph (pentodes)")
    ap.add_argument("--page", type=int, help="datasheet page, for the copied project's name")
    a = ap.parse_args()
    d = os.path.join(DATA, a.tube)
    with open(a.project, encoding="utf-8") as f:
        j = json.load(f)
    axes = {x["name"]: x for x in j["axesColl"]}
    for x in axes.values():
        if x["type"] != "XYAxes" or x.get("isLogX") or x.get("isLogY"):
            raise SystemExit(f"axes {x['name']}: linear XY axes expected")
    for ds in j["datasetColl"]:
        m = re.fullmatch(r"\s*([+-]?\d+(?:[.,]\d+)?)\s*V?\s*", ds["name"])
        if not m:
            raise SystemExit(f"dataset {ds['name']!r}: name it by its grid voltage, e.g. -8V")
        vg = float(m.group(1).replace(",", "."))
        v = np.array([p["value"] for p in ds["data"]], dtype=np.float64)
        v = v[np.argsort(v[:, 0])]
        q = "0" if vg == 0 else (f"+{vg:g}" if vg > 0 else f"{vg:g}")
        name = f"vg{q}" + (f"_vg2{a.vg2:g}" if a.vg2 else "") + ".csv"
        with open(os.path.join(d, name), "w", encoding="utf-8", newline="\n") as f:
            f.write("Ep_V,Ip_mA\n")
            for x, y in v:
                f.write(f"{x:.3f},{y:.4f}\n")
        print(f"  Vg {vg:>6g}: {len(v):>3} points, Ep {v[0, 0]:.1f} .. {v[-1, 0]:.1f} V, Ip {v[:, 1].min():.1f} .. "
              f"{v[:, 1].max():.1f} mA -> {name}")
    dst = os.path.join(d, f"wpd_p{a.page}.json" if a.page else "wpd.json")
    shutil.copyfile(a.project, dst)
    print(f"  project copied to {dst}")


if __name__ == "__main__":
    main()
