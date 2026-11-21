"""M2 (N28): set-300b with more H2. In the reference circuit (spice_amps.set_300b) the 6SN7 driver's H2 arrives at the
output in antiphase with the 300B's own and takes about 9 dB off it (the output stage alone: H2 -33.6 dB at 1 W; the
amplifier: -42.5 dB). Variants that keep the driver from cancelling it, each a common practice in real 300B amplifiers:

  V0  the reference: 6SN7, 47 k plate resistor, bypassed 1.5 k cathode resistor, RC-coupled (0.22 uF / 220 k)
  V1  the 6SN7 with a constant-current plate load (CCS: a near-ideal current source at the reference's quiescent
      current): a triode into a current source is almost linear, its H2 all but gone, the 300B's own H2 stays
  V2  the 6SN7's cathode resistor unbypassed: local current feedback lowers the driver's H2
  V3  interstage-transformer coupling (1:1, 40 H, the 6SN7 at 8 mA through the primary), its secondary wired so that
      the driver's H2 adds to the 300B's (a transformer can be wired either way round, an RC coupling cannot)
  V3r the same transformer wired the other way round (the driver's H2 against the 300B's again: the check)

  python set300b_variants.py          H2 / H3 / THD at 0.1 W, P_ref (0.279 W), 1 W; rated power (5 % THD)
"""
import json, math, os, sys
from concurrent.futures import ProcessPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import spice_amps as A  # noqa: E402
import spice_ref as R   # noqa: E402
import xyce_hb as X     # noqa: E402

f = A.f
P_REF = 0.279


def output_stage(grid_net):
    """The 300B stage of set_300b (cathode bias 1.233 k, B+ 431 V, 3 k : 8 transformer) on grid node g2."""
    return ["Rgs2 g2 g2s 1k", R.triode("V2", "300B", "a2", "g2s", "k2"), *A.tube_caps("V2", "300B", "a2", "g2s", "k2"),
            "Rk2 k2 0 1233", "Ck2 k2 0 2.2e-4", "Vb2 b2 0 DC 431"] + \
        A.opt_se("T", "a2", "b2", 3000.0, 15.0, 0.012, 400e-12, 120.0, preset="set-300b")


def v0():
    return ["* set-300b V0"] + A.set_driver_6sn7(350.0, 47e3, 1.5e3, "g2", 220e3, 0.22e-6) + output_stage("g2")


def v1():
    # the CCS at V0's quiescent current (CCS_I0): spice_amps.set_300b_x, the preset made from it
    return A.set_300b_x()


def v2():
    net = [l for l in A.set_driver_6sn7(350.0, 47e3, 1.5e3, "g2", 220e3, 0.22e-6) if not l.startswith("Ck1 ")]
    return ["* set-300b V2 (unbypassed)"] + net + output_stage("g2")


def v3(reverse=False):
    # 6SN7, B+ 300 V through the primary (40 H, 600 ohm), cathode 1.2 k bypassed (about 8 mA); 1:1 to the 300B grid
    sec = "0 g2" if reverse else "g2 0"
    net = ["Vin in 0 SIN(0 {amp} {f0})", "Rin in 0 1Meg", "Rgs in g1 1k",
           "Vb1 b1 0 DC 300", "Rp1 b1 p1 600", "Lp1 p1 a1 40", "Rk1 k1 0 1.2k", "Ck1 k1 0 1e-4",
           R.triode("V1", "6SN7", "a1", "g1", "k1"), *A.tube_caps("V1", "6SN7", "a1", "g1", "k1"),
           f"Ls1 {sec} 40", "Rs1 g2 0 100k", "Kit Lp1 Ls1 0.998", "Cit g2 0 150p"]
    return [f"* set-300b V3{'r' if reverse else ''} (interstage transformer)"] + net + output_stage("g2")


CCS_I0 = 3.865e-3   # V0's quiescent current through the 47 k (168.3 V at the plate, 5.80 V at the cathode)
VARIANTS = {"V0": v0, "V1": v1, "V2": v2, "V3": v3, "V3r": lambda: v3(True)}
for k, fn in VARIANTS.items():
    A.PRESETS[f"set-300b-{k}"] = fn


def at_power(args):
    b, p = args
    # the amplitude from the small-signal gain, then refined (xyce_hb.at_power's way)
    amp = 0.3
    r = X.measure(b, amp, 1000.0, 15)
    for _ in range(6):
        amp *= math.sqrt(p / r["P"])
        r = X.measure(b, amp, 1000.0, 30)
        if abs(r["P"] / p - 1) < 1e-3:
            break
    r["amp"] = amp
    return b, p, r


def rated(b):
    """The power at 5 % THD (bisection on the drive amplitude)."""
    lo, hi = 0.5, 20.0
    for _ in range(30):
        mid = math.sqrt(lo * hi)
        try:
            over = X.measure(b, mid, 1000.0, 30)["THD"] > 5.0
        except RuntimeError:   # far past rated harmonic balance may not converge: past it either way
            over = True
        if over:
            hi = mid
        else:
            lo = mid
        if hi / lo < 1.002:
            break
    r = X.measure(b, lo, 1000.0, 30)
    return b, r["P"], lo


if __name__ == "__main__":
    # V1's current: V0's operating point through the 47 k (from its DC: the HB time average at the plate)
    names = [f"set-300b-{k}" for k in VARIANTS]
    jobs = [(b, p) for b in names for p in (0.1, P_REF, 1.0)]
    with ProcessPoolExecutor(12) as ex:
        res = list(ex.map(at_power, jobs))
        rat = dict((b, (p, a)) for b, p, a in ex.map(rated, names))
    out = {}
    print(f"{'variant':<14} {'P W':>6}  {'in V':>7}  {'H2':>7} {'H3':>7} {'H4':>7}  {'THD %':>7}")
    for b, p, r in res:
        out.setdefault(b, {})[str(p)] = {"amp": r["amp"], "H": list(r["H"]), "THD": r["THD"]}
        print(f"{b:<14} {p:6.3f}  {r['amp']:7.3f}  {r['H'][0]:7.1f} {r['H'][1]:7.1f} {r['H'][2]:7.1f}  {r['THD']:7.3f}")
    for b, (p, a) in rat.items():
        out[b]["rated_W"], out[b]["rated_drive_V"] = p, a
        print(f"{b:<14} rated (5 % THD) {p:.2f} W at {a:.3f} V")
    os.makedirs(os.path.join(HERE, "data", "curves"), exist_ok=True)
    json.dump(out, open(os.path.join(HERE, "data", "curves", "set300b_variants.json"), "w"), indent=1)
