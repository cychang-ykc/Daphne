// Tube Color (doc/N28), M1: the stage with the test curve (test builds only: DAPHNE_COLOR_TEST_CURVE).
#include <catch_amalgamated.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>
#include <string>
#include <vector>

#include "daphne/color/tube_color.h"
#include "daphne/engine/chain.h"

#if defined(DAPHNE_COLOR_TEST_CURVE)

#include "daphne/color/linear.h"
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


// ---- M1 step 6: the drive glide, the response fade (moving corners), drive 0 skipped ----

namespace {

using daphne::color::LinearModel;
using daphne::color::Section;
using daphne::color::SectionChain;

// the stage's linear response at hz (dB) for a drive, from its coefficients (exact: TPT sections are the prewarped
// bilinear transform; the stage's processing is checked against it below)
std::vector<double> stage_response(double drive, const std::vector<double>& hz, int rate) {
    TubeColorStage st(StageParams{.drive = drive, .linear = daphne::color::test_linear_model()});
    st.prepare({rate, 1, 4096});
    std::vector<double> db;
    for (double f : hz) db.push_back(20 * std::log10(std::abs(st.linear_response(f))));
    return db;
}

}  // namespace

TEST_CASE("color stage: the response fades in with the drive, without a jump", "[color][linear]") {
    const int rate = 352800;   // what the local oversampling gives the stage (8x the 44.1 kHz family)
    const std::vector<double> hz = {10, 15, 20, 30, 50, 100, 300, 1000, 5000, 10000, 15000, 20000, 30000, 40000};
    std::vector<std::vector<double>> r;
    for (int k = 0; k <= 200; ++k) r.push_back(stage_response(k / 100.0, hz, rate));
    // drive 0: flat (the stage is skipped); 100 %: the reference, the analog sections through the bilinear transform
    // prewarped at their corners (the frequency warping makes 20 kHz -1.17 dB here, -1.20 dB analog: M2 fits at the
    // stage's rate)
    for (double v : r[0]) CHECK(v == 0.0);
    for (size_t j = 0; j < hz.size(); ++j) {
        auto w = [&](double fc) { return std::complex<double>(0.0, std::tan(kPi * hz[j] / rate) / std::tan(kPi * fc / rate)); };
        const auto a = w(5.0), lo = w(20.4), hi = w(26600.0);
        const auto hp1 = a / (a + 1.0);
        const auto hp2 = lo * lo / (lo * lo + lo / 0.7071 + 1.0), lp2 = 1.0 / (hi * hi + hi / 0.7071 + 1.0);
        const double want = 20 * std::log10(std::abs(hp1 * hp2 * lp2));
        INFO(hz[j] << " Hz at 100 %: " << r[100][j] << " dB, expected " << want << " dB");
        CHECK(r[100][j] == Catch::Approx(want).margin(1e-9));
    }
    // 1 %: hardly anything yet
    double first = 0.0;
    for (size_t j = 0; j < hz.size(); ++j) first = std::max(first, std::abs(r[1][j] - r[0][j]));
    UNSCOPED_INFO("drive 0 -> 1 %: largest change " << first << " dB");
    CHECK(first < 0.01);
    // every 1 % step, at every frequency: at most 1.4 x an even fade (smoothstep: 1.5 / 25 % per unit of drive, i.e.
    // 0.06 of that frequency's full deviation per 1 %; the corner mapping c = sqrt(m) stays within 1.31 x of it);
    // from 25 % on, no change at all
    double worst = 0.0;
    int worst_k = 0;
    size_t worst_f = 0;
    for (int k = 1; k <= 200; ++k)
        for (size_t j = 0; j < hz.size(); ++j) {
            const double step = std::abs(r[static_cast<size_t>(k)][j] - r[static_cast<size_t>(k - 1)][j]);
            const double bound = 1.4 * 0.06 * std::abs(r[100][j]) + 1e-3;   // (0.001 dB: where there is next to no deviation)
            if (k <= 25) CHECK(step <= bound);
            else CHECK(step == 0.0);
            if (step > worst) worst = step, worst_k = k, worst_f = j;
        }
    WARN("response fade: largest change per 1 % of drive " << worst << " dB (at " << worst_k << " %, " << hz[worst_f] << " Hz); 0 -> 1 %: "
                                                           << first << " dB");
    // and monotonic: every frequency moves one way only
    for (size_t j = 0; j < hz.size(); ++j) {
        int ups = 0, downs = 0;
        for (int k = 1; k <= 25; ++k) {
            const double dv = r[static_cast<size_t>(k)][j] - r[static_cast<size_t>(k - 1)][j];
            ups += dv > 0.0, downs += dv < 0.0;
        }
        INFO(hz[j] << " Hz: " << ups << " steps up, " << downs << " down");
        CHECK((ups == 0 || downs == 0));
    }
}

TEST_CASE("color stage: what it does to a small sine is its linear response", "[color][linear]") {
    const int rate = 352800, frames = rate * 3;
    for (double drive : {0.05, 0.1, 0.25, 1.0, 2.0})
        for (double hz : {20.0, 1000.0, 20000.0}) {
            TubeColorStage st(StageParams{.drive = drive, .linear = daphne::color::test_linear_model()});
            st.prepare({rate, 1, 4096});
            std::vector<double> x(static_cast<size_t>(frames)), y(x.size());
            for (int i = 0; i < frames; ++i) x[static_cast<size_t>(i)] = 1e-4 * std::sin(2 * kPi * hz * i / rate);
            for (int at = 0; at < frames; at += 4096) st.process(x.data() + at, std::min(4096, frames - at), y.data() + at);
            const int from = frames - static_cast<int>(rate / hz) * static_cast<int>(hz);   // the last second, whole periods
            double c = 0.0, s = 0.0;
            for (int i = from; i < frames; ++i) {
                const double w = 2 * kPi * hz * i / rate;
                c += y[static_cast<size_t>(i)] * std::cos(w);
                s += y[static_cast<size_t>(i)] * std::sin(w);
            }
            const double got = 20 * std::log10(std::hypot(c, s) * 2 / (frames - from) / 1e-4);
            const double want = 20 * std::log10(std::abs(st.linear_response(hz)));
            INFO("drive " << drive << ", " << hz << " Hz: " << got << " dB, response " << want << " dB");
            CHECK(got == Catch::Approx(want).margin(0.002));
        }
}

TEST_CASE("color stage: drive 100 % is the reference amplifier, post(f(pre(x)))", "[color][linear]") {
    const int rate = 352800;
    const double hz = 1000.0, amp = 0.5;
    const int frames = rate;   // 1 s
    std::vector<double> x(static_cast<size_t>(frames));
    for (int i = 0; i < frames; ++i) x[static_cast<size_t>(i)] = amp * std::sin(2 * kPi * hz * i / rate);
    TubeColorStage st(StageParams{.drive = 1.0, .linear = daphne::color::test_linear_model()});
    st.prepare({rate, 1, 4096});
    std::vector<double> y(x.size());
    for (int at = 0; at < frames; at += 4096) st.process(x.data() + at, std::min(4096, frames - at), y.data() + at);
    // the reference, computed apart: the same sections, the curve, no DC blocker
    const auto m = daphne::color::test_linear_model();
    SectionChain pre, post;
    pre.prepare(m.pre, rate, 1);
    post.prepare(m.post, rate, 1);
    const TestCurve f;
    std::vector<double> ref(x.size());
    for (size_t i = 0; i < x.size(); ++i) ref[i] = post.process(f(pre.process(x[i], 0)), 0);
    auto level = [&](const std::vector<double>& v, int k) {
        double c = 0.0, s = 0.0;
        for (int i = frames / 2; i < frames; ++i) {
            const double w = 2 * kPi * k * hz * i / rate;
            c += v[static_cast<size_t>(i)] * std::cos(w);
            s += v[static_cast<size_t>(i)] * std::sin(w);
        }
        return 20 * std::log10(std::hypot(c, s) / (frames / 2) * 2);
    };
    for (int k = 1; k <= 3; ++k) {
        INFO("harmonic " << k << ": stage " << level(y, k) << " dB, reference " << level(ref, k) << " dB");
        CHECK(std::abs(level(y, k) - level(ref, k)) < 0.01);
    }
}

TEST_CASE("color stage: the drive glides over 20 ms", "[color][linear]") {
    const int rate = 352800;
    TubeColorStage st(StageParams{.drive = 0.5, .linear = daphne::color::test_linear_model()});
    st.prepare({rate, 1, 4096});
    const int frames = rate / 10;
    std::vector<double> x(static_cast<size_t>(frames)), y(x.size());
    for (int i = 0; i < frames; ++i) x[static_cast<size_t>(i)] = 0.7 * std::sin(2 * kPi * 997.0 * i / rate);
    st.process(x.data(), 4096, y.data());
    st.set_drive(1.5);
    const int half = static_cast<int>(0.010 * rate);
    st.process(x.data() + 4096, half, y.data() + 4096);
    CHECK(st.drive() == Catch::Approx(1.0).margin(0.01));   // half way after 10 ms
    st.process(x.data() + 4096 + half, frames - 4096 - half, y.data() + 4096 + half);
    CHECK(st.drive() == 1.5);
    // no step in the output: the largest sample-to-sample change stays that of the steady sine (+5 %)
    double steady = 0.0, during = 0.0;
    for (int i = 1; i < 4096; ++i) steady = std::max(steady, std::abs(y[static_cast<size_t>(i)] - y[static_cast<size_t>(i - 1)]));
    for (int i = 4096; i < 4096 + 2 * half; ++i) during = std::max(during, std::abs(y[static_cast<size_t>(i)] - y[static_cast<size_t>(i - 1)]));
    CHECK(during < 1.05 * steady);
    st.set_drive(0.0);   // not taken: crossing 0 is a rebuild
    st.process(x.data(), 4096, y.data());
    CHECK(st.drive() == 1.5);
}

TEST_CASE("color stage: drive 0 skips everything, filters included", "[color][linear]") {
    TubeColorStage st(StageParams{.drive = 0.0, .linear = daphne::color::test_linear_model()});
    CHECK(st.is_identity());
    st.set_drive(1.0);   // ignored: a stage built at 0 stays the identity
    const auto x = sine(352800, 997.0, 0.9, 40000);
    CHECK(run(st, x, 352800) == x);
    CHECK(st.is_identity());
}

TEST_CASE("color stage: crossing drive 0 rebuilds the chain, other drive changes do not", "[color][chain]") {
    daphne::Settings a;
    a.color.enabled = true;
    a.color.drive = 0.5;
    daphne::Settings b = a, z = a;
    b.color.drive = 1.7;
    z.color.drive = 0.0;
    CHECK(daphne::DspChain::structure_key(a) == daphne::DspChain::structure_key(b));
    CHECK(daphne::DspChain::structure_key(a) != daphne::DspChain::structure_key(z));
    daphne::AudioInfo src;
    src.rate = 44100, src.channels = 2, src.bits = 24, src.lossless_int = true, src.codec = "test";
    daphne::DeviceFormat dev;
    dev.rate = 352800, dev.channels = 2, dev.type = daphne::SampleType::Int32, dev.container_bits = dev.valid_bits = 32;
    daphne::DspChain c;
    std::string e;
    REQUIRE(c.build(a, src, dev, 4096, &e));
    CHECK(c.update(b));         // in place, gliding
    CHECK_FALSE(c.update(z));   // to 0: a new chain
    daphne::DspChain c0;
    REQUIRE(c0.build(z, src, dev, 4096, &e));
    const auto stages = c0.stages();
    CHECK(std::find(stages.begin(), stages.end(), std::string("tube color")) == stages.end());   // there, but passing through
    const auto all = c0.all_stages();
    CHECK(std::find(all.begin(), all.end(), std::string("tube color")) != all.end());
    CHECK_FALSE(c0.update(a));
}

#endif
