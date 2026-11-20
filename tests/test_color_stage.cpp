// Tube Color (doc/N28), M1: the stage with the test curve (test builds only: DAPHNE_COLOR_TEST_CURVE).
#include <catch_amalgamated.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <string>
#include <vector>

#include "daphne/color/tube_color.h"
#include "daphne/engine/chain.h"

#if defined(DAPHNE_COLOR_TEST_CURVE)

#include "daphne/color/stage.h"
#include "daphne/color/test_curve.h"

using daphne::color::StageParams;
using daphne::color::TestCurve;
using daphne::color::TubeColorStage;

namespace {

constexpr double kPi = std::numbers::pi;

// interleaved stereo: a sine on both channels
std::vector<double> sine(int rate, double hz, double amp, int frames) {
    std::vector<double> x(static_cast<size_t>(frames) * 2);
    for (int i = 0; i < frames; ++i) x[2 * static_cast<size_t>(i)] = x[2 * static_cast<size_t>(i) + 1] = amp * std::sin(2 * kPi * hz * i / rate);
    return x;
}

std::vector<double> run(TubeColorStage& st, const std::vector<double>& x, int rate, int block = 4096) {
    st.prepare({rate, 2, block});
    std::vector<double> y(x.size());
    const int frames = static_cast<int>(x.size() / 2);
    for (int at = 0; at < frames; at += block) {
        const int n = std::min(block, frames - at);
        REQUIRE(st.process(x.data() + 2 * static_cast<size_t>(at), n, y.data() + 2 * static_cast<size_t>(at)) == n);
    }
    return y;
}

// the cosine and sine parts of harmonic k of channel 0 over [from, to) (whole periods of hz)
std::pair<double, double> harmonic(const std::vector<double>& y, int rate, double hz, int k, int from, int to) {
    double c = 0.0, s = 0.0;
    for (int i = from; i < to; ++i) {
        const double w = 2 * kPi * k * hz * i / rate, v = y[2 * static_cast<size_t>(i)];
        c += v * std::cos(w);
        s += v * std::sin(w);
    }
    const double n = to - from;
    return {2 * c / n, 2 * s / n};
}

}  // namespace

TEST_CASE("color test curve: shape", "[color][curve]") {
    const TestCurve f;
    CHECK(f(0.0) == 0.0);
    // slope 1 at 0 (in phase), monotonic, below full scale, asymmetric (x^2)
    CHECK((f(1e-6) - f(-1e-6)) / 2e-6 == Catch::Approx(1.0).epsilon(1e-6));
    double prev = f(-TestCurve::kMaxIn);
    int falls = 0, overs = 0;
    for (int i = 1; i <= 5000; ++i) {
        const double y = f(-TestCurve::kMaxIn + i * 0.001);
        falls += y <= prev;
        overs += std::abs(y) >= 1.0;
        prev = y;
    }
    CHECK(falls == 0);
    CHECK(overs == 0);
    CHECK(f(0.5) > -f(-0.5));
    CHECK(f(0.5) == Catch::Approx(0.5 + TestCurve::kA2 * 0.25 + TestCurve::kA3 * 0.125));   // below the knee: the polynomial
    CHECK(f(10.0) == f(TestCurve::kMaxIn));                                               // flat beyond its input range
    CHECK(f(-10.0) == f(-TestCurve::kMaxIn));
}

TEST_CASE("color stage: drive 0 is the identity, bit for bit", "[color]") {
    TubeColorStage st(StageParams{.drive = 0.0});
    CHECK(st.is_identity());
    const auto x = sine(48000, 997.0, 0.9, 20000);
    CHECK(run(st, x, 48000) == x);
    TubeColorStage on;
    CHECK_FALSE(on.is_identity());
}

// The x^2 term puts an offset on the output (a2 A^2 / 2 for a sine of amplitude A); the DC blocker takes it out, and only
// it: the signal itself does not go through the filter.
TEST_CASE("color stage: the DC blocker removes the curve's offset", "[color]") {
    const int rate = 48000;
    const double amp = 0.5, hz = 1000.0;
    const auto x = sine(rate, hz, amp, rate * 2);   // 2 s
    TubeColorStage st;
    const auto y = run(st, x, rate);
    // without the blocker: the curve's own offset
    const TestCurve f;
    double raw = 0.0;
    for (int i = 0; i < 48; ++i) raw += f(x[2 * static_cast<size_t>(i)]) / 48.0;   // one period
    CHECK(raw == Catch::Approx(TestCurve::kA2 * amp * amp / 2).epsilon(0.01));
    // with it: the last second (whole periods), after the 80 ms time constant has settled
    double dc = 0.0;
    for (int i = rate; i < 2 * rate; ++i) dc += y[2 * static_cast<size_t>(i)] / rate;
    UNSCOPED_INFO("offset without the blocker " << raw << ", with it " << dc);
    CHECK(std::abs(dc) < 1e-6);
    // the fundamental is the input's, the 2nd harmonic is the curve's (the blocker barely touches 2 kHz)
    const auto [c1, s1] = harmonic(y, rate, hz, 1, rate, 2 * rate);
    const auto [c2, s2] = harmonic(y, rate, hz, 2, rate, 2 * rate);
    CHECK(std::hypot(c1, s1) == Catch::Approx(amp + 0.75 * TestCurve::kA3 * amp * amp * amp).epsilon(1e-4));
    CHECK(std::hypot(c2, s2) == Catch::Approx(TestCurve::kA2 * amp * amp / 2).epsilon(1e-3));
}

// Polarity (doc/N28): a symmetric curve would not show an inversion done twice (-f(-x) = f(x)); the test curve's x^2
// term does. A sine in: the output follows it (positive correlation), the positive peak is the larger one, and the 2nd
// harmonic has the sign x^2 gives it (-cos 2wt for sin wt).
TEST_CASE("color stage: polarity, with an asymmetric curve", "[color]") {
    auto check = [](const std::vector<double>& x, const std::vector<double>& y, int rate, double hz, int from, int to) {
        double xy = 0.0, hi = 0.0, lo = 0.0;
        for (int i = from; i < to; ++i) {
            xy += x[2 * static_cast<size_t>(i)] * y[2 * static_cast<size_t>(i)];
            hi = std::max(hi, y[2 * static_cast<size_t>(i)]);
            lo = std::min(lo, y[2 * static_cast<size_t>(i)]);
        }
        const auto [c2, s2] = harmonic(y, rate, hz, 2, from, to);
        UNSCOPED_INFO("peaks +" << hi << " / " << lo << ", 2nd harmonic cos " << c2 << " sin " << s2);
        CHECK(xy > 0.0);
        CHECK(hi > -lo);
        CHECK(c2 < 0.0);
        CHECK(std::abs(c2) > 10 * std::abs(s2));
    };
    SECTION("the stage") {
        const int rate = 48000;
        const auto x = sine(rate, 1000.0, 0.6, rate * 2);
        TubeColorStage st;
        check(x, run(st, x, rate), rate, 1000.0, rate, 2 * rate);
    }
    SECTION("the chain: after the upsampler, before the final quantizer") {
        daphne::Settings s;
        s.color.enabled = true;
        daphne::AudioInfo a;
        a.rate = 44100, a.channels = 2, a.bits = 24, a.lossless_int = true, a.codec = "test";
        daphne::DeviceFormat d;
        d.rate = 44100, d.channels = 2, d.type = daphne::SampleType::Float64, d.container_bits = d.valid_bits = 64;
        daphne::DspChain c;
        std::string e;
        REQUIRE(c.build(s, a, d, 4096, &e));
        const auto stages = c.stages();
        REQUIRE(std::find(stages.begin(), stages.end(), std::string("tube color")) != stages.end());
        const int frames = 44100 * 2;
        const auto x = sine(44100, 441.0, 0.6, frames);
        std::vector<double> y;
        std::vector<double> o(static_cast<size_t>(c.max_output_frames()) * 2);
        for (int at = 0; at < frames; at += 4096) {
            const int n = c.process(x.data() + 2 * static_cast<size_t>(at), std::min(4096, frames - at), o.data());
            y.insert(y.end(), o.begin(), o.begin() + 2 * static_cast<std::ptrdiff_t>(n));
        }
        REQUIRE(static_cast<int>(y.size()) >= 2 * frames - 2 * 4096);
        const int delay = c.content_delay();
        std::vector<double> xs(x.begin(), x.end() - 2 * static_cast<std::ptrdiff_t>(delay));
        std::vector<double> ys(y.begin() + 2 * static_cast<std::ptrdiff_t>(delay), y.begin() + 2 * static_cast<std::ptrdiff_t>(delay) + static_cast<std::ptrdiff_t>(xs.size()));
        const int n = static_cast<int>(xs.size() / 2);
        check(xs, ys, 44100, 441.0, n - 44100, n - 44100 + 44100 / 441 * 400);
    }
}

#endif
