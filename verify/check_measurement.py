"""M0 check of the measurement scripts (spec: error on known signals < 0.1 dB), N28.

Known signals: a fundamental with H2 -40 dB, H3 -60 dB, H5 -80 dB (and random phases), through every analysis path:
  - spice_ref.harmonics / spice_amps.harmonics (ngspice transient: interpolation onto a uniform grid, FFT)
  - pp_core_lf.uniform + Fourier sum (core_presets.harm; Xyce transient)
each with uniform and with variable time steps like Xyce's (dense near the waveform's fast parts), and the old raw
mean over variable steps for comparison; and xyce_hb.run_hb on a behavioural source with a known polynomial,
v = x + a x^2 + b x^3 for x = A sin: H2 = a A / 2, H3 = b A^2 / 4 (exact, + the b A^3 term in the fundamental).

  python check_measurement.py
"""
import math, sys
import numpy as np

import spice_ref as R, spice_amps as A, pp_core_lf as P, core_presets as CP, xyce_hb as X

TRUE = {2: -40.0, 3: -60.0, 5: -80.0}


def signal(t, f0, rng):
    v = np.sin(2 * np.pi * f0 * t)
    for h, db in TRUE.items():
        v += 10 ** (db / 20) * np.sin(2 * np.pi * h * f0 * t + rng.uniform(0, 2 * np.pi))
    return v


def time_axes(f0, cycles, rng):
    T = cycles / f0
    uni = np.linspace(0, T, cycles * 1000 + 1)
    # variable steps: density follows |slope| of the fundamental, as an adaptive solver does, with jitter
    u = np.sort(rng.uniform(0, 1, cycles * 1500))
    var = T * (u + 0.15 * np.sin(2 * np.pi * cycles * u) / (2 * np.pi * cycles))
    var = np.unique(np.clip(np.r_[0, var, T], 0, T))
    return uni, var


def errors(levels):
    return max(abs(levels[h] - db) for h, db in TRUE.items())


def main():
    rng = np.random.default_rng(3)
    f0, cycles = 1000.0, 10
    out = []
    for name, (t_kind) in (("uniform steps", 0), ("variable steps", 1)):
        t = time_axes(f0, cycles, rng)[t_kind]
        v = signal(t, f0, rng)
        R.F0 = f0
        _, h = R.harmonics(t, v, cycles=cycles)
        out.append((f"spice_ref.harmonics, {name}", errors({k: h[k - 2] for k in TRUE})))
        _, h = A.harmonics(t, v, f0, cycles=cycles)
        out.append((f"spice_amps.harmonics, {name}", errors({k: h[k - 2] for k in TRUE})))
        _, h = CP.harm(t, v, f0)
        out.append((f"core_presets.harm (uniform resampling), {name}", errors({k: h[k - 2] for k in TRUE})))
        a = [2 * abs(np.mean(v * np.exp(-2j * np.pi * k * f0 * t))) for k in range(1, 6)]
        out.append((f"old raw mean (before the fix), {name}", errors({k: 20 * math.log10(a[k - 1] / a[0]) for k in TRUE})))
    # harmonic balance: polynomial behavioural source
    amp, a2, b3 = 1.0, 0.02, 0.004
    net = ["* poly", "Vin x 0 SIN(0 {amp} {f0})", "Rx x 0 1k", f"Bo o 0 V={{v(x)+{a2}*v(x)*v(x)+{b3}*v(x)*v(x)*v(x)}}", "Ro o 0 1k"]
    net = [l.replace("{amp}", str(amp)).replace("{f0}", "1000") for l in net]
    am, _ = X.run_hb(net, 1000.0, 15)
    h1 = amp + 0.75 * b3 * amp ** 3
    true = {2: 20 * math.log10(a2 * amp ** 2 / 2 / h1), 3: 20 * math.log10(b3 * amp ** 3 / 4 / h1)}
    meas = {2: 20 * math.log10(am[1] / am[0]), 3: 20 * math.log10(am[2] / am[0])}
    out.append(("xyce_hb.run_hb (polynomial source)", max(abs(meas[k] - true[k]) for k in true)))
    ok = True
    for name, e in out:
        flag = "" if "old raw" in name else ("ok" if e < 0.1 else "FAIL")
        ok &= flag != "FAIL"
        print(f"  {name:<52} worst error {e:9.4f} dB {flag}")
    return ok


if __name__ == "__main__":
    sys.exit(0 if main() else 1)
