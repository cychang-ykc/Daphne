#pragma once
// The M1 test curve (doc/N28): a stand-in for a tube amplifier's transfer curve, so the stage's machinery can be
// built and tested before M2 brings the real ones. Test builds only (DAPHNE_COLOR_TEST_CURVE); never in a release.
//
//   p(x) = x + a2 x^2 + a3 x^3, then a soft limit: |p| below kKnee passes, above it bends (tanh) towards 1
//
// The x^2 term makes it asymmetric (a positive peak comes out larger than a negative one, as in a single-ended
// stage) and gives the output a DC offset the stage removes (its DC blocker): both are what the tests need to see.

#if defined(DAPHNE_COLOR_TEST_CURVE)

#include <algorithm>
#include <cmath>

#include "daphne/color/linear.h"

namespace daphne::color {

struct TestCurve {
    static constexpr double kA2 = 0.1, kA3 = -0.02;
    static constexpr double kKnee = 0.8;   // the soft limit's start; its output stays below 1

    static double soft_limit(double p) {
        const double a = std::abs(p);
        if (a <= kKnee) return p;
        const double y = kKnee + (1.0 - kKnee) * std::tanh((a - kKnee) / (1.0 - kKnee));
        return p < 0.0 ? -y : y;
    }
    // (since the 400 % limit, doc/N28: drive 400 % puts a full-scale sample at 4, past kMaxIn: clamped, flat, still
    // defined; the test curve is for the machinery, not for how 400 % sounds)
    // Monotonic: p'(x) = 1 + 2 a2 x + 3 a3 x^2 > 0 for |x| <= kMaxIn (drive 200 % on a full-scale sample, with room
    // for overs; p'(-2.5) = 0.125), and flat beyond it (the input is clamped there).
    static constexpr double kMaxIn = 2.5;
    double operator()(double x) const {
        x = std::clamp(x, -kMaxIn, kMaxIn);
        return soft_limit(x + kA2 * x * x + kA3 * x * x * x);
    }
};

// The M1 test response, shaped like the M2 references (doc/N28): a coupling capacitor (5 Hz, first order) before the
// curve; after it an output transformer's low end (second order, 20.4 Hz: -3.27 dB at 20 Hz, like pp-el34-ul at 6 dB
// of feedback) and high end (second order, 26.6 kHz: -1.2 dB at 20 kHz, like set-2a3).
inline LinearModel test_linear_model() {
    LinearModel m;
    m.pre = {{Section::Kind::HighPass1, 5.0, 0.7071}};
    m.post = {{Section::Kind::HighPass2, 20.4, 0.7071}, {Section::Kind::LowPass2, 26600.0, 0.7071}};
    return m;
}

}  // namespace daphne::color

#endif
