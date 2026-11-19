"""The reference amplifiers (spice_amps.py netlists) run in Xyce with harmonic balance: the periodic steady state
directly, with no settling transient.

Xyce (Sandia; the Windows build "XyceNF", .tools/xyce-7.10, not in version control) reads the same netlists with
two changes: a behavioural source's expression goes in braces (B1 a k I={...}), and the analysis is .HB at the
signal frequency with NUMFREQ harmonics. Note: Xyce's log() is base 10; the tube models use ln() only.

  python xyce_hb.py set-300b 1.794 [numfreq]     one case: amplitude in V peak
"""
import math, os, re, subprocess, sys, tempfile, time
import numpy as np

import spice_amps as A

HERE = os.path.dirname(os.path.abspath(__file__))
XYCE = os.path.join(os.path.dirname(HERE), ".tools", "xyce-7.10", "bin", "Xyce.exe")
TIMEOUT = 1800


def to_xyce(net):
    out = []
    for line in net:
        m = re.match(r"^(B\S+\s+\S+\s+\S+)\s+I\s*=\s*(.+)$", line)
        out.append(f"{m.group(1)} I={{{m.group(2)}}}" if m else line)
    return out


def run_hb(net, f0, numfreq, extra_options=""):
    """Returns (harmonic amplitudes of v(o): fundamental .. numfreq, seconds)."""
    with tempfile.TemporaryDirectory() as td:
        cir = os.path.join(td, "amp.cir")
        lines = to_xyce(net) + [f".hb {f0:g}", f".options hbint numfreq={numfreq} {extra_options}".rstrip(),
                                ".print hb v(o)", ".end", ""]
        with open(cir, "w") as h:
            h.write("\n".join(lines))
        t0 = time.time()
        r = subprocess.run([XYCE, cir], capture_output=True, text=True, timeout=TIMEOUT, cwd=td)
        dt = time.time() - t0
        fd = cir + ".HB.FD.prn"
        if not os.path.exists(fd):
            raise RuntimeError("Xyce produced no HB output:\n" + (r.stdout + r.stderr)[-2500:])
        a = []
        with open(fd) as h:
            for line in h:
                p = line.split()
                try:
                    a.append([float(x) for x in p[1:4]])
                except (ValueError, IndexError):
                    continue                    # header and the "End of Xyce(TM) Simulation" footer
    amps = {}
    for fr, re_, im in a:
        h = int(round(fr / f0))
        if h >= 1:
            amps[h] = 2 * math.hypot(re_, im)   # two-sided spectrum: positive-frequency line x 2
    return np.array([amps.get(h, 0.0) for h in range(1, numfreq + 1)]), dt


def measure(builder, amp, f0=1000.0, numfreq=15, kw=None, extra_options=""):
    net = A.pp_520(**(kw or {})) if builder == "pp-el34-ul" else A.PRESETS[builder]()
    net = [l.replace("{amp}", A.f(amp)).replace("{f0}", A.f(f0)) for l in net]
    a, dt = run_hb(net, f0, numfreq, extra_options)
    h = 20 * np.log10(np.maximum(a[1:7], 1e-300) / a[0])
    return {"P": a[0] ** 2 / 2 / 8.0, "H": h, "THD": 100 * math.sqrt(sum(10 ** (x / 10) for x in h)), "seconds": dt}


if __name__ == "__main__":
    b, amp = sys.argv[1], float(sys.argv[2])
    nf = int(sys.argv[3]) if len(sys.argv) > 3 else 15
    r = measure(b, amp, numfreq=nf)
    print(f"{b} {amp} V, numfreq {nf}: P {r['P']:.4f} W  THD {r['THD']:.4f} %  " +
          " ".join(f"H{i + 2} {v:.2f}" for i, v in enumerate(r["H"])) + f"   {r['seconds']:.1f} s")
