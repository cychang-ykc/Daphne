// Tube Color (doc/N28), M1: the stage with the test curve (test builds only: DAPHNE_COLOR_TEST_CURVE).
#include <catch_amalgamated.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <map>
#include <numbers>
#include <random>
#include <string>
#include <vector>

#include "daphne/color/tube_color.h"
#include "daphne/engine/chain.h"
#include "daphne/dsp/progress.h"

#include "daphne/color/oversampler.h"

// ---- M1 step 8: the local oversampling (every build with Tube Color) ----

namespace {

// |H| in dB of a kernel (DC gain 1) at f, its rate fs
double kernel_db(const std::vector<double>& h, double f, double fs) {
    double c = 0.0, s = 0.0;
    for (size_t i = 0; i < h.size(); ++i) {
        const double w = 2 * 3.14159265358979323846 * f * static_cast<double>(i) / fs;
        c += h[i] * std::cos(w);
        s += h[i] * std::sin(w);
    }
    return 20 * std::log10(std::max(std::hypot(c, s), 1e-30));
}

}  // namespace

TEST_CASE("color oversampler: factor, pass band, stop band, a whole-frame delay", "[color][oversampling]") {
    using daphne::color::Oversampler;
    CHECK(Oversampler::factor(44100) == 8);
    CHECK(Oversampler::factor(88200) == 4);
    CHECK(Oversampler::factor(176400) == 2);
    CHECK(Oversampler::factor(352800) == 1);
    CHECK(Oversampler::factor(705600) == 1);
    CHECK(Oversampler::factor(48000) == 8);
    CHECK(Oversampler::factor(96000) == 4);
    CHECK(Oversampler::factor(384000) == 1);
    for (int rate : {44100, 48000, 88200, 96000, 176400, 192000}) {
        INFO(rate << " Hz");
        Oversampler os;
        const int l = Oversampler::factor(rate);
        os.prepare(rate, l, 1, 4096);
        const auto& h = os.kernel();
        const double fs = static_cast<double>(rate) * l;
        // the round trip is h twice: the pass band to 20 kHz within +-0.01 dB, the stop band (from half the input rate,
        // where images and aliases begin) at least 140 dB down
        double pass_lo = 0.0, pass_hi = -1e9, stop = -1e9;
        for (double f = 10.0; f <= 20000.0; f *= 1.01) {
            const double v = 2 * kernel_db(h, f, fs);
            pass_lo = std::min(pass_lo, v);
            pass_hi = std::max(pass_hi, v);
        }
        for (double f = 0.5 * rate; f <= 0.5 * fs; f += rate / 400.0) stop = std::max(stop, kernel_db(h, f, fs));
        WARN(rate << " Hz, L " << l << ", " << h.size() << " taps: pass band " << pass_lo << " .. " << pass_hi << " dB (round trip), stop band "
                  << stop << " dB");
        CHECK(pass_lo >= -0.01);
        CHECK(pass_hi <= 0.01);
        CHECK(stop <= -140.0);
        // an impulse comes back after exactly delay() frames, symmetric around it (linear phase)
        std::vector<double> hi(static_cast<size_t>(l)), y(400, 0.0);
        for (int n = 0; n < 400; ++n) {
            const double x = n == 0 ? 1.0 : 0.0;
            os.up(&x, hi.data());
            os.down(hi.data(), &y[static_cast<size_t>(n)]);
        }
        const auto peak = static_cast<int>(std::max_element(y.begin(), y.end()) - y.begin());
        CHECK(peak == os.delay());
        CHECK(os.delay() == Oversampler::kTapsPerPhase - 1);
        for (int k = 1; k < 100; ++k) CHECK(y[static_cast<size_t>(peak - k)] == Catch::Approx(y[static_cast<size_t>(peak + k)]).margin(1e-12));
    }
}


// What the curve makes above the input's band must not come back into it: a tone at the high rate between half the input
// rate and the high rate's Nyquist is decimated to less than -120 dB (M1: a synthetic check; 4x vs 8x is M2's).
TEST_CASE("color oversampler: what lies above the input band does not fold back", "[color][oversampling]") {
    using daphne::color::Oversampler;
    const int rate = 44100, l = Oversampler::factor(rate);
    double worst = -1e9, worst_f = 0.0;
    for (double f : {22100.0, 23000.0, 24100.0, 30000.0, 44100.0 - 997.0, 66150.0, 88200.0 + 1000.0, 150000.0, 176000.0}) {
        Oversampler os;
        os.prepare(rate, l, 1, 4096);
        const int frames = rate / 2;
        std::vector<double> hi(static_cast<size_t>(l)), y(static_cast<size_t>(frames));
        const double fs = static_cast<double>(rate) * l;
        for (int n = 0; n < frames; ++n) {
            for (int p = 0; p < l; ++p) hi[static_cast<size_t>(p)] = std::sin(2 * 3.14159265358979323846 * f * (n * l + p) / fs);
            os.down(hi.data(), &y[static_cast<size_t>(n)]);
        }
        double rms = 0.0;
        for (int n = frames / 2; n < frames; ++n) rms += y[static_cast<size_t>(n)] * y[static_cast<size_t>(n)];
        const double db = 10 * std::log10(rms / (frames / 2) * 2 + 1e-300);   // re a full-scale sine
        if (db > worst) worst = db, worst_f = f;
    }
    WARN("decimation: the largest fold-back " << worst << " dB (a full-scale tone at " << worst_f << " Hz)");
    CHECK(worst < -120.0);
}

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

TEST_CASE("color stage: drive 0 is a pure delay, bit for bit", "[color]") {
    // 48 kHz: the stage is oversampled, its content delay the round trip's (223 frames); idle, it delays the input by
    // exactly that, nothing else
    TubeColorStage st(StageParams{.drive = 0.0, .linear = {}});
    CHECK(st.is_identity());
    const auto x = sine(48000, 997.0, 0.9, 20000);
    const auto y = run(st, x, 48000);
    const auto d = static_cast<size_t>(st.content_delay_frames());
    CHECK(d == daphne::color::Oversampler::kTapsPerPhase - 1);
    CHECK(std::equal(x.begin(), x.end() - static_cast<std::ptrdiff_t>(2 * d), y.begin() + static_cast<std::ptrdiff_t>(2 * d)));
    CHECK(st.untouched());
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
        auto y = run(st, x, rate);   // (oversampled at 48 kHz: aligned by its content delay)
        const auto d = static_cast<std::ptrdiff_t>(2 * st.content_delay_frames());
        CHECK(st.oversampling() == 8);
        CHECK(st.content_delay_frames() == daphne::color::Oversampler::kTapsPerPhase - 1);
        y.erase(y.begin(), y.begin() + d);
        y.resize(x.size(), 0.0);
        check(x, y, rate, 1000.0, rate / 2, rate / 2 + rate);
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
    // to 0: down to next to nothing (20 ms), back to the delay path (20 ms), idle: the input as it is (no oversampling at
    // this rate: no delay)
    st.set_drive(0.0);
    const int n = static_cast<int>(0.05 * rate);
    st.process(x.data(), n, y.data());
    CHECK(st.idle());
    CHECK(st.drive() == 0.0);
    st.process(x.data(), 4096, y.data());
    CHECK(st.untouched());
    CHECK(std::equal(x.begin(), x.begin() + 4096, y.begin()));
}

TEST_CASE("color stage: drive 0 skips everything, filters included", "[color][linear]") {
    TubeColorStage st(StageParams{.drive = 0.0, .linear = daphne::color::test_linear_model()});
    CHECK(st.is_identity());
    const auto x = sine(352800, 997.0, 0.9, 40000);
    CHECK(run(st, x, 352800) == x);   // no oversampling at this rate: no delay either
    CHECK(st.untouched());
    st.set_drive(1.0);   // taken in place: it leaves idle
    CHECK_FALSE(st.is_identity());
}

// Crossing 0 in the stage (doc/N28): at 44.1 kHz (oversampled 8x, 223 frames) from idle to 50 % and back. The delay path
// and the processing path are crossfaded aligned: the cross-correlation peak never moves, and both idle stretches are
// the input bit for bit (delayed by the content delay).
TEST_CASE("color stage: crossing 0 in place, aligned, idle bit for bit", "[color][linear]") {
    const int rate = 44100, block = 512;
    const int frames = rate * 2;
    std::vector<double> x(static_cast<size_t>(frames) * 2);
    std::mt19937 g(3);
    std::uniform_real_distribution<double> u(-0.02, 0.02);
    for (int i = 0; i < frames; ++i) x[2 * static_cast<size_t>(i)] = x[2 * static_cast<size_t>(i) + 1] = u(g) + 0.02 * std::sin(2 * kPi * 997.0 * i / rate);
    TubeColorStage st(StageParams{.drive = 0.0, .linear = daphne::color::test_linear_model()});
    st.prepare({rate, 2, block});
    std::vector<double> y(x.size());
    const int on_at = rate / 2, off_at = rate;   // 0.5 s: to 50 %; 1 s: back to 0
    for (int at = 0; at < frames; at += block) {
        if (at == on_at / block * block) st.set_drive(0.5);
        if (at == off_at / block * block) st.set_drive(0.0);
        st.process(x.data() + 2 * static_cast<size_t>(at), std::min(block, frames - at), y.data() + 2 * static_cast<size_t>(at));
    }
    const int d = st.content_delay_frames();
    REQUIRE(d == 223);
    auto same = [&](int from, int to) {   // y[n + d] == x[n] for n in [from, to)
        for (int n = from; n < to; ++n)
            if (y[2 * static_cast<size_t>(n + d)] != x[2 * static_cast<size_t>(n)]) return false;
        return true;
    };
    CHECK(same(0, on_at / block * block - d));                     // idle before
    CHECK(same(off_at / block * block + rate / 10, frames - d));   // idle again, 100 ms after (20 + 20 ms of it the switch)
    CHECK(st.idle());
    // the cross-correlation peak (first differences), 10 ms windows across the whole stretch, at d everywhere
    std::map<int, int> lags;
    const int win = rate / 100;
    for (int at = 1000; at + win + 600 < frames; at += win) {
        double best = -1e300;
        int lag = 0;
        for (int l = d - 300; l <= d + 300; ++l) {
            double c = 0.0;
            for (int i = 0; i < win; ++i) {
                const size_t a = 2 * static_cast<size_t>(at + i), b = 2 * static_cast<size_t>(at + i - l);
                c += (y[a] - y[a - 2]) * (x[b] - x[b - 2]);
            }
            if (c > best) best = c, lag = l;
        }
        ++lags[lag];
    }
    std::string seen;
    for (const auto& [l, n] : lags) seen += std::to_string(l) + " (" + std::to_string(n) + ") ";
    WARN("stage crossing 0: cross-correlation peak by 10 ms window: " << seen);
    CHECK(lags.size() == 1);
    CHECK(lags.begin()->first == d);
}

// The final quantizer on an integer device with a source that fits it (where it is left out without Tube Color): kept,
// passing through while the stage is idle. drive 0 -> 50 % -> 0: the input bit for bit, dithered, the input bit for bit.
// (Tube Color's reserve offset by +1.5 dB of headroom: the gain stage at 0 dB, the stage and the quantizer alone.)
TEST_CASE("color chain: the quantizer kept for Tube Color passes while it is idle", "[color][chain]") {
    for (int bits : {24, 16}) {
        INFO(bits << "-bit output, a " << bits << "-bit source");
        const double scale = std::ldexp(1.0, bits - 1);
        daphne::AudioInfo src;
        src.rate = 44100, src.channels = 2, src.bits = bits, src.lossless_int = true, src.codec = "test";
        daphne::DeviceFormat dev;
        dev.rate = 44100, dev.channels = 2;
        if (bits == 24) dev.type = daphne::SampleType::Int24In32, dev.container_bits = 32, dev.valid_bits = 24;
        else dev.type = daphne::SampleType::Int16, dev.container_bits = 16, dev.valid_bits = 16;
        daphne::Settings s;
        s.color.enabled = true;
        s.color.drive = 0.0;
        s.headroom_db = daphne::kColorHeadroomDb;
        daphne::DspChain c;
        std::string e;
        REQUIRE(c.build(s, src, dev, 4096, &e));
        const auto all = c.all_stages();
        REQUIRE(std::find(all.begin(), all.end(), std::string("quantizer")) != all.end());   // kept for Tube Color
        CHECK(c.transparent());
        // without Tube Color the same chain leaves it out (the condition is the existing one)
        daphne::Settings off;
        daphne::DspChain c_off;
        REQUIRE(c_off.build(off, src, dev, 4096, &e));
        const auto off_all = c_off.all_stages();
        CHECK(std::find(off_all.begin(), off_all.end(), std::string("quantizer")) == off_all.end());
        // with an upsampler it quantizes as always, Tube Color or not
        daphne::DeviceFormat up = dev;
        up.rate = 88200;
        daphne::DspChain c_up;
        REQUIRE(c_up.build(s, src, up, 4096, &e));
        CHECK_FALSE(c_up.transparent());
        // samples on the grid through it (4 s: 43 blocks; 50 % from block 16 to block 32)
        const int frames = 44100 * 4, block = 4096;
        std::vector<double> x(static_cast<size_t>(frames) * 2);
        std::mt19937 g(5);
        std::uniform_real_distribution<double> u(-0.3, 0.3);
        for (auto& v : x) v = std::round(u(g) * scale) / scale;
        std::vector<double> y;
        std::vector<double> o(static_cast<size_t>(c.max_output_frames()) * 2);
        for (int at = 0; at < frames; at += block) {
            if (at == 16 * block) {
                daphne::Settings on = s;
                on.color.drive = 0.5;
                REQUIRE(c.update(on));
            }
            if (at == 32 * block) REQUIRE(c.update(s));
            const int n = c.process(x.data() + 2 * static_cast<size_t>(at), std::min(block, frames - at), o.data());
            y.insert(y.end(), o.begin(), o.begin() + 2 * static_cast<std::ptrdiff_t>(n));
        }
        const int d = c.content_delay();
        REQUIRE(d == 223 + 88);   // the stage (oversampled 8x) and its limiter's 2 ms look-ahead at 44.1 kHz
        auto count_diff = [&](int from, int to) {
            int k = 0;
            for (int n = from; n < to; ++n) k += y[2 * static_cast<size_t>(n + d)] != x[2 * static_cast<size_t>(n)];
            return k;
        };
        auto on_grid = [&](int from, int to) {
            for (int n = from; n < to; ++n) {
                const double v = y[2 * static_cast<size_t>(n + d)] * scale;
                if (v != std::round(v)) return false;
            }
            return true;
        };
        CHECK(count_diff(0, 16 * block - d) == 0);                               // idle: bit for bit
        CHECK(count_diff(20 * block, 30 * block) > 10 * block - 10 * block / 4);   // running: changed (curve, dither)
        CHECK(on_grid(20 * block, 30 * block));                                  // quantized to the word length
        CHECK(count_diff(36 * block, frames - d - block) == 0);                  // idle again: bit for bit
    }
}

TEST_CASE("color stage: crossing drive 0 is taken in place, like any drive change", "[color][chain]") {
    daphne::Settings a;
    a.color.enabled = true;
    a.color.drive = 0.5;
    daphne::Settings b = a, z = a;
    b.color.drive = 1.7;
    z.color.drive = 0.0;
    CHECK(daphne::DspChain::structure_key(a) == daphne::DspChain::structure_key(b));
    CHECK(daphne::DspChain::structure_key(a) == daphne::DspChain::structure_key(z));
    daphne::AudioInfo src;
    src.rate = 44100, src.channels = 2, src.bits = 24, src.lossless_int = true, src.codec = "test";
    daphne::DeviceFormat dev;
    dev.rate = 352800, dev.channels = 2, dev.type = daphne::SampleType::Int32, dev.container_bits = dev.valid_bits = 32;
    daphne::DspChain c;
    std::string e;
    REQUIRE(c.build(a, src, dev, 4096, &e));
    CHECK(c.update(b));
    CHECK(c.update(z));
    daphne::DspChain c0;
    REQUIRE(c0.build(z, src, dev, 4096, &e));
    const auto stages = c0.stages();
    CHECK(std::find(stages.begin(), stages.end(), std::string("tube color")) == stages.end());   // there, idle
    const auto all = c0.all_stages();
    CHECK(std::find(all.begin(), all.end(), std::string("tube color")) != all.end());
    CHECK(c0.update(a));
}


// ---- M1 item 7: the digital volume cancelled around the stage ----

namespace {

struct VolRun {
    std::vector<double> y;   // channel 0, chain output
    int delay = 0;
    int64_t limited = 0;   // frames Tube Color's limiter turned down (these tests keep below it: the stage alone)
};

// x (stereo) through a chain with Tube Color at `drive`, the volume at db0 and, from input block `at_block` on, db1
VolRun run_volume(const std::vector<double>& x, int src_rate, int dev_rate, double drive, double db0, double db1, int at_block, bool dsd = false) {
    daphne::AudioInfo src;
    src.rate = src_rate, src.channels = 2, src.bits = 0, src.lossless_int = false, src.codec = "test", src.dsd_pcm = dsd;
    if (dsd) src.dsd_rate = 2822400;
    daphne::DeviceFormat dev;
    dev.rate = dev_rate, dev.channels = 2, dev.type = daphne::SampleType::Float64, dev.container_bits = dev.valid_bits = 64;
    daphne::Settings s;
    s.filter.taps = 4096;
    s.output.rate = std::to_string(dev_rate);
    s.output.dsd_pcm_limiter = false;
    s.volume.mode = daphne::VolumeMode::Digital;
    s.volume.db = db0;
    s.color.enabled = true;
    s.color.drive = drive;
    daphne::DspChain c;
    std::string e;
    REQUIRE(c.build(s, src, dev, 4096, &e));
    VolRun r;
    r.delay = c.content_delay();
    std::vector<double> o(static_cast<size_t>(c.max_output_frames()) * 2);
    const int frames = static_cast<int>(x.size() / 2);
    for (int at = 0, b = 0; at < frames; at += 4096, ++b) {
        if (b == at_block) {
            s.volume.db = db1;
            REQUIRE(c.update(s));
        }
        const int n = c.process(x.data() + 2 * static_cast<size_t>(at), std::min(4096, frames - at), o.data());
        for (int i = 0; i < n; ++i) r.y.push_back(o[2 * static_cast<size_t>(i)]);
    }
    r.limited = c.take_color_limited();
    return r;
}

std::vector<double> two_tones(int rate, double seconds, double amp) {
    const int n = static_cast<int>(seconds * rate);
    std::vector<double> x(static_cast<size_t>(n) * 2);
    for (int i = 0; i < n; ++i)
        x[2 * static_cast<size_t>(i)] = x[2 * static_cast<size_t>(i) + 1] =
            amp * (0.7 * std::sin(2 * kPi * 997.0 * i / rate) + 0.3 * std::sin(2 * kPi * 3001.0 * i / rate));
    return x;
}

}  // namespace

// The curve sees the music at the same level whatever the digital volume: the output at -20 or -60 dB is the output at
// 0 dB scaled, the distortion included (doc/N28, item 7). Drive 1.4 on peaks of 0.6: the curve's input reaches 0.84,
// into its soft limit, while the output stays below the soft limiter after the stage (which works on the level that
// leaves, so it does not scale with the volume: it is checked to stay idle here, and tested on its own).
TEST_CASE("color volume: the tube character does not change with the volume", "[color][volume]") {
    for (int dev_rate : {44100, 176400}) {   // oversampled 8x in the stage; upsampled 4x before it (its delay between)
        INFO(dev_rate << " Hz");
        const auto x = two_tones(44100, 1.0, 0.6);
        const auto a = run_volume(x, 44100, dev_rate, 1.4, 0.0, 0.0, -1);
        CHECK(a.limited == 0);
        for (double db : {-20.0, -60.0}) {
            const auto b = run_volume(x, 44100, dev_rate, 1.4, db, db, -1);
            // the output's gain against 0 dB's: the volume, and Tube Color's reserve (settings.h) at 0 dB that the
            // volume here already gives
            const double r0 = daphne::kColorHeadroomDb, r = std::max(0.0, daphne::kColorHeadroomDb + db);
            const double g = std::pow(10.0, (db - r + r0) / 20.0);
            double err = 0.0, peak = 0.0;
            for (size_t i = 0; i < std::min(a.y.size(), b.y.size()); ++i) {
                err = std::max(err, std::abs(b.y[i] / g - a.y[i]));
                peak = std::max(peak, std::abs(a.y[i]));
            }
            UNSCOPED_INFO(db << " dB: the output over the gain against 0 dB, " << err / peak << " of its peak");
            CHECK(err < 1e-9 * peak);
        }
    }
}

// A volume change (0 -> -20 dB) mid-stream: the cancellation follows the gain stage's ramp where it reaches the stage
// (the upsampler's delay between, the oversampling's in the stage), so the output is the 0 dB output times the gain as
// heard at the output, through the ramp too.
TEST_CASE("color volume: a volume change is cancelled in step with the gain stage's ramp", "[color][volume]") {
    for (int dev_rate : {44100, 176400}) {
        INFO(dev_rate << " Hz");
        const auto x = two_tones(44100, 1.5, 0.6);   // (below the soft limiter, as above)
        const int at_block = 8;
        const auto a = run_volume(x, 44100, dev_rate, 1.4, 0.0, 0.0, -1);
        const auto b = run_volume(x, 44100, dev_rate, 1.4, 0.0, -20.0, at_block);
        CHECK(a.limited == 0);
        // the gain as heard at the output: the same chains with Tube Color idle (a pure delay, the same content delay)
        const auto a0 = run_volume(x, 44100, dev_rate, 0.0, 0.0, 0.0, -1);
        const auto b0 = run_volume(x, 44100, dev_rate, 0.0, 0.0, -20.0, at_block);
        double peak = 0.0;
        for (double v : a0.y) peak = std::max(peak, std::abs(v));
        auto worst = [&](int shift) {
            double err = 0.0;
            const size_t n = std::min({a.y.size(), b.y.size(), a0.y.size(), b0.y.size()});
            for (size_t i = 200; i + 200 < n; ++i) {
                const size_t j = static_cast<size_t>(static_cast<int64_t>(i) + shift);
                if (std::abs(a0.y[j]) < 0.2 * peak) continue;   // (the gain read where the signal is well above 0)
                const double g = b0.y[j] / a0.y[j];
                err = std::max(err, std::abs(b.y[i] - g * a.y[i]));
            }
            return err / peak;
        };
        const double aligned = worst(0), off = worst(64);
        WARN(dev_rate << " Hz, 0 -> -20 dB: the output against the 0 dB output times the heard gain, " << aligned
                      << " of the peak (the gain read 64 frames off: " << off << ")");
        CHECK(aligned < 1e-3);
        CHECK(off > 10 * aligned);
    }
}

TEST_CASE("color volume: below -80 dB the curve is skipped", "[color][volume]") {
    auto h2_over_h1 = [](const std::vector<double>& y, int rate) {
        auto level = [&](double hz) {
            double c = 0.0, s = 0.0;
            const size_t from = y.size() / 2, to = from + static_cast<size_t>(rate) / 2;   // 0.5 s: whole periods of 1 and 2 kHz
            for (size_t i = from; i < to && i < y.size(); ++i) {
                const double w = 2 * kPi * hz * static_cast<double>(i) / rate;
                c += y[i] * std::cos(w), s += y[i] * std::sin(w);
            }
            return std::hypot(c, s);
        };
        return level(2000.0) / level(1000.0);
    };
    std::vector<double> x(44100 * 2);
    for (int i = 0; i < 44100; ++i) x[2 * static_cast<size_t>(i)] = x[2 * static_cast<size_t>(i) + 1] = 0.8 * std::sin(2 * kPi * 1000.0 * i / 44100);
    const double at70 = h2_over_h1(run_volume(x, 44100, 44100, 1.0, -70.0, -70.0, -1).y, 44100);
    const double at90 = h2_over_h1(run_volume(x, 44100, 44100, 1.0, -90.0, -90.0, -1).y, 44100);
    UNSCOPED_INFO("H2 / H1: " << 20 * std::log10(at70) << " dB at -70 dB volume, " << 20 * std::log10(at90 + 1e-300) << " dB at -90 dB");
    CHECK(at70 > 1e-3);   // the curve's H2 at -70 dB as at full volume
    CHECK(at90 < 1e-7);   // none: the curve skipped
}

// A DSD file converted to PCM comes 6 dB up (50 % modulation -> 0 dBFS): Tube Color takes it out too, so a DSD file and
// a PCM file of the same music get the same drive.
TEST_CASE("color volume: a converted DSD file's +6 dB is cancelled", "[color][volume]") {
    const auto x = two_tones(176400, 0.5, 0.3);   // (the DSD file's output 0.6: below the soft limiter after the stage)
    auto x2 = x;
    for (auto& v : x2) v *= 2.0;
    const auto pcm = run_volume(x, 176400, 176400, 1.0, 0.0, 0.0, -1);
    const auto dsd = run_volume(x2, 176400, 176400, 1.0, 0.0, 0.0, -1, true);
    CHECK(dsd.limited == 0);
    double err = 0.0, peak = 0.0;
    for (size_t i = 0; i < std::min(pcm.y.size(), dsd.y.size()); ++i) {
        err = std::max(err, std::abs(dsd.y[i] / 2.0 - pcm.y[i]));
        peak = std::max(peak, std::abs(pcm.y[i]));
    }
    UNSCOPED_INFO("the DSD file's output over 2 against the PCM file's: " << err / peak << " of the peak");
    CHECK(err < 1e-12 * peak);
}


// ---- the makeup gain; the result whatever the thread count ----

TEST_CASE("color stage: the makeup gain's trim", "[color][makeup]") {
    const int rate = 352800;
    const auto x = sine(rate, 997.0, 0.5, rate / 2);
    TubeColorStage a(StageParams{.drive = 1.0, .linear = {}, .level = 1.0, .volume = 1.0, .trim_db = 0.0});
    TubeColorStage b(StageParams{.drive = 1.0, .linear = {}, .level = 1.0, .volume = 1.0, .trim_db = 3.0});
    const auto ya = run(a, x, rate), yb = run(b, x, rate);
    const double g = std::pow(10.0, 3.0 / 20.0);
    double err = 0.0;
    for (size_t i = 0; i < ya.size(); ++i) err = std::max(err, std::abs(yb[i] - g * ya[i]));
    CHECK(err < 1e-12);
    // changed in place: it glides (no step beyond the sine's own)
    TubeColorStage c(StageParams{.drive = 1.0, .linear = {}, .level = 1.0, .volume = 1.0, .trim_db = 0.0});
    c.prepare({rate, 2, 4096});
    std::vector<double> y(x.size());
    c.process(x.data(), 4096, y.data());
    c.set_trim(6.0);
    c.process(x.data() + 2 * 4096, rate / 4, y.data() + 2 * 4096);
    double steady = 0.0, during = 0.0;
    for (int i = 1; i < 4096; ++i) steady = std::max(steady, std::abs(y[2 * static_cast<size_t>(i)] - y[2 * static_cast<size_t>(i - 1)]));
    for (int i = 4096; i < 4096 + rate / 50; ++i) during = std::max(during, std::abs(y[2 * static_cast<size_t>(i)] - y[2 * static_cast<size_t>(i - 1)]));
    CHECK(during < 2.05 * steady);   // (at most the doubled sine's own: +6 dB)
    // idle (drive 0): no makeup, the input as it is
    TubeColorStage z(StageParams{.drive = 0.0, .linear = {}, .level = 1.0, .volume = 1.0, .trim_db = 3.0});
    CHECK(run(z, x, rate) == x);
}

// The chain's output with Tube Color does not depend on how many threads design the filter (doc/N28): the same bits.
TEST_CASE("color chain: the same output whatever the thread count", "[color][chain]") {
    daphne::AudioInfo src;
    src.rate = 44100, src.channels = 2, src.bits = 24, src.lossless_int = true, src.codec = "test";
    daphne::DeviceFormat dev;
    dev.rate = 176400, dev.channels = 2, dev.type = daphne::SampleType::Int32, dev.container_bits = dev.valid_bits = 32;
    const auto x = sine(44100, 997.0, 0.7, 44100);
    std::vector<std::vector<double>> outs;
    for (int threads : {1, 4}) {
        daphne::Settings s;
        s.filter.taps = 65536;
        s.output.rate = "176400";
        s.design.threads = threads;
        s.color.enabled = true;
        s.color.drive = 1.2;
        daphne::dsp::set_design_threads(threads);
        daphne::DspChain c;
        std::string e;
        REQUIRE(c.build(s, src, dev, 4096, &e));
        std::vector<double> o(static_cast<size_t>(c.max_output_frames()) * 2), y;
        for (int at = 0; at < 44100; at += 4096) {
            const int n = c.process(x.data() + 2 * static_cast<size_t>(at), std::min(4096, 44100 - at), o.data());
            y.insert(y.end(), o.begin(), o.begin() + 2 * static_cast<std::ptrdiff_t>(n));
        }
        outs.push_back(std::move(y));
    }
    daphne::dsp::set_design_threads(0);
    REQUIRE(outs[0].size() > 100000);
    CHECK(outs[0] == outs[1]);
}

#endif

// ---- the presets' own curves (M2, doc/N28): every build with Tube Color ----

#include "daphne/color/curves.h"
#include "daphne/color/stage.h"

namespace {

std::vector<double> run_curve(daphne::color::TubeColorStage& st, const std::vector<double>& x, int rate) {
    st.prepare({rate, 2, 4096});
    std::vector<double> y(x.size());
    const int frames = static_cast<int>(x.size() / 2);
    for (int at = 0; at < frames; at += 4096) {
        const int n = std::min(4096, frames - at);
        REQUIRE(st.process(x.data() + 2 * static_cast<size_t>(at), n, y.data() + 2 * static_cast<size_t>(at)) == n);
    }
    return y;
}

std::pair<double, double> harmonic_of(const std::vector<double>& y, int rate, double hz, int k, int from, int to) {
    double c = 0.0, s = 0.0;
    for (int i = from; i < to; ++i) {
        const double w = 2 * std::numbers::pi * k * hz * i / rate, v = y[2 * static_cast<size_t>(i)];
        c += v * std::cos(w);
        s += v * std::sin(w);
    }
    return {2 * c / (to - from), 2 * s / (to - from)};
}

}  // namespace

// The table as exported (verify/curve_export.py): f(0) = 0, f'(0) = 1, an output that only grows with the input, the
// grid's own values where the cubic passes, its last value beyond the table (defined, not extrapolated).
TEST_CASE("color curves: set-300b's table", "[color][curve]") {
    const auto* c = daphne::color::preset_curve("set-300b");
    REQUIRE(c != nullptr);
    CHECK(daphne::color::preset_curve("set-1000") == nullptr);
    const auto& f = *c;
    CHECK(std::abs(f(0.0)) < 1e-12);
    CHECK((f(1e-3) - f(-1e-3)) / 2e-3 == Catch::Approx(1.0).epsilon(1e-3));
    double prev = f(-c->u_max - 1.0);
    for (double u = -c->u_max - 1.0; u <= c->u_max + 1.0; u += 1e-3) {
        CHECK(f(u) >= prev - 1e-12);
        prev = f(u);
    }
    const double h = 2.0 * c->u_max / (c->n - 1);
    for (int i = 0; i < c->n; i += 97) CHECK(f(-c->u_max + i * h) == Catch::Approx(c->f[i]).margin(1e-12));
    CHECK(f(c->u_max + 3.0) == c->f[c->n - 1]);
    CHECK(f(-c->u_max - 3.0) == c->f[0]);
    // the ranges: rated power inside the grid-current-free part, the data past 400 % x 1.41 (5.64)
    CHECK(c->u_rated < c->u_no_grid);
    CHECK(c->u_data > 5.64);
}

// The stage on set-300b's curve and linear model against the circuit (Xyce, data/curves/set-300b_check.json): a 1 kHz
// sine at 1 W and at rated power (drive 100 %), H2 and H3 within 0.5 dB of the circuit's; the output in phase with the
// input (N28: every preset).
TEST_CASE("color curves: set-300b in the stage against the circuit", "[color][curve]") {
    const auto* c = daphne::color::preset_curve("set-300b");
    REQUIRE(c != nullptr);
    struct Level { double u, h2, h3; };
    for (const auto& lv : {Level{0.3391, -42.47, -49.72}, Level{0.8606, -27.94, -30.64}}) {
        INFO("u " << lv.u);
        daphne::color::StageParams p;
        p.drive = 1.0;
        p.curve = c;
        p.linear = c->linear;
        daphne::color::TubeColorStage st(p);
        const int rate = 44100, frames = 2 * rate;
        std::vector<double> x(2 * static_cast<size_t>(frames));
        for (int i = 0; i < frames; ++i) x[2 * static_cast<size_t>(i)] = x[2 * static_cast<size_t>(i) + 1] = lv.u * std::sin(2 * std::numbers::pi * 1000.0 * i / rate);
        const auto y = run_curve(st, x, rate);
        const int from = frames - 44100 / 2, to = frames;   // the last half second (the coupling and DC blocker settled)
        auto mag = [&](int k) { const auto [a, b] = harmonic_of(y, rate, 1000.0, k, from, to); return std::hypot(a, b); };
        const double h1 = mag(1);
        CHECK(20 * std::log10(mag(2) / h1) == Catch::Approx(lv.h2).margin(0.5));
        CHECK(20 * std::log10(mag(3) / h1) == Catch::Approx(lv.h3).margin(0.5));
        // in phase: the fundamental along the input's sine (the delay taken out)
        const int d = st.content_delay_frames();
        const auto [cs, sn] = harmonic_of(y, rate, 1000.0, 1, from, to);
        const double ph = std::atan2(cs, sn) + 2 * std::numbers::pi * 1000.0 * d / rate;
        CHECK(std::cos(ph) > 0.99);
    }
}

// The virtual loudspeaker load (doc/N28): set-300b into the loudspeaker (bookshelf-vented-smooth: its tilt, no
// resonance) against the 8 ohm resistor. The stage's steady-state response is the load's sections' (relative to 1 kHz,
// within 0.1 dB), and the load is the circuit's tilt: within 2 dB of the circuit with the vented loudspeaker in it
// (ngspice AC, verify/load_filters.py; what the smoothing takes out is the resonances: the port's notch at 50 Hz,
// -2.6 dB, the most). And its fade: from drive 0 to 25 % the load's effect at 250 Hz (its largest, -2.7 dB re 1 kHz)
// grows monotonically from nothing, in small steps.
namespace {

double tone_level(const daphne::color::PresetCurve* c, const std::vector<daphne::color::Section>& load, double drive, double hz) {
    daphne::color::StageParams p;
    p.drive = drive;
    p.curve = c;
    p.linear = c->linear;
    p.load = load;
    daphne::color::TubeColorStage st(p);
    // 0.5 s (the slowest settling: the 2 Hz DC blocker, 80 ms), measured over the last whole periods in 0.2 s
    const int rate = 44100, frames = rate / 2;
    std::vector<double> x(2 * static_cast<size_t>(frames));
    for (int i = 0; i < frames; ++i) x[2 * static_cast<size_t>(i)] = x[2 * static_cast<size_t>(i) + 1] = 0.01 * std::sin(2 * std::numbers::pi * hz * i / rate);
    const auto y = run_curve(st, x, rate);
    const int periods = std::max(1, static_cast<int>(std::floor(hz * 0.2)));
    const int n = static_cast<int>(std::lround(periods * rate / hz));
    const auto [a, b] = harmonic_of(y, rate, hz, 1, frames - n, frames);
    return 20 * std::log10(std::hypot(a, b));
}

}  // namespace

TEST_CASE("color load: set-300b into the loudspeaker, its sections, the circuit's tilt, its fade", "[color][load]") {
    const auto* c = daphne::color::preset_curve("set-300b");
    const auto* l = daphne::color::preset_load("set-300b", -1, "bookshelf-vented-smooth");
    REQUIRE(c != nullptr);
    REQUIRE(l != nullptr);
    CHECK(daphne::color::preset_load("set-300b", -1, "resistor-8") == nullptr);   // the 8 ohm resistor: no sections
    for (const char* gone : {"bookshelf-vented", "bookshelf-sealed", "bookshelf-sealed-smooth"})   // removed (N28)
        CHECK(daphne::color::preset_load("set-300b", -1, gone) == nullptr);
    const std::vector<daphne::color::Section> none;
    daphne::color::SectionChain sc;
    sc.prepare(l->sections, 352800, 1);
    const double at_1k = tone_level(c, l->sections, 1.0, 1000.0) - tone_level(c, none, 1.0, 1000.0);
    auto rel = [&](double hz) { return tone_level(c, l->sections, 1.0, hz) - tone_level(c, none, 1.0, hz) - at_1k; };
    auto sections_rel = [&](double hz) { return 20 * std::log10(std::abs(sc.response(hz)) / std::abs(sc.response(1000.0))); };
    struct P { double hz, circuit_db; };
    for (const auto& pt : {P{40, -1.5932}, P{50, -2.5819}, P{85, 0.5134}, P{150, -1.6162}, P{250, -2.555}, P{2600, 0.7269},
                           P{10000, -2.4308}, P{18000, -3.2664}}) {
        INFO(pt.hz << " Hz");
        const double r = rel(pt.hz);
        CHECK(r == Catch::Approx(sections_rel(pt.hz)).margin(0.1));
        CHECK(r == Catch::Approx(pt.circuit_db).margin(2.0));
    }
    // every pole pair of Q 0.5 .. 0.7 (they set how long it rings), every zero pair of Q <= 1 (verify/load_smooth.py)
    for (const auto& s : l->sections) {
        REQUIRE(s.kind == daphne::color::Section::Kind::General2);
        CHECK(s.q >= 0.5 - 1e-6);
        CHECK(s.q <= 0.7 + 1e-6);
        CHECK(std::sqrt(std::abs(s.c0 * s.c2)) / s.c1 <= 1.02);
    }
    // the fade at 250 Hz
    double prev = 0.0, first = 0.0, biggest = 0.0;
    int prev_pct = 0;
    for (int pct : {1, 2, 3, 5, 8, 12, 16, 20, 25}) {   // (sampled: the sanitizer jobs run it too)
        const double d = pct / 100.0;
        const double e = tone_level(c, l->sections, d, 250.0) - tone_level(c, none, d, 250.0);   // the load's effect at this drive
        if (pct == 1) first = e;
        INFO(pct << " %: " << e << " dB");
        CHECK(std::abs(e) >= std::abs(prev) - 1e-9);   // only larger as the drive rises
        biggest = std::max(biggest, std::abs(e - prev) / (pct - prev_pct));   // per 1 % of drive
        prev = e;
        prev_pct = pct;
    }
    CHECK(std::abs(first) < 0.02);   // next to nothing at 1 %
    CHECK(biggest < 0.3);            // no more than 0.3 dB per 1 % of drive
}

// The level alignment (doc/N28, an M2 experiment): a gain a on the curve's input only. At drive 200 % with a = 0.5 the
// stage is the one at drive 100 % (the response's fade is full at both, the curve at d a = 1), sample for sample; it
// glides to a new value in kAlignGlideMs, or is there at once; at drive 0 the stage passes the samples as they are.
TEST_CASE("color stage: the level alignment", "[color]") {
    const auto* c = daphne::color::preset_curve("set-300b");
    REQUIRE(c != nullptr);
    const int rate = 44100, frames = rate / 2;
    std::mt19937 g(5);
    std::normal_distribution<double> n(0.0, 0.2);
    std::vector<double> x(2 * static_cast<size_t>(frames));
    for (auto& v : x) v = n(g);
    auto params = [&](double drive, double align) {
        daphne::color::StageParams p;
        p.drive = drive;
        p.curve = c;
        p.linear = c->linear;
        p.align = align;
        return p;
    };
    daphne::color::TubeColorStage a(params(2.0, 0.5)), b(params(1.0, 1.0));
    const auto ya = run_curve(a, x, rate), yb = run_curve(b, x, rate);
    double worst = 0.0;
    for (size_t i = 0; i < ya.size(); ++i) worst = std::max(worst, std::abs(ya[i] - yb[i]));
    CHECK(worst < 1e-12);
    // gliding, or at once
    daphne::color::TubeColorStage s(params(1.0, 1.0));
    s.prepare({rate, 2, 4096});
    std::vector<double> y(x.size());
    s.set_align(0.25);
    CHECK(s.align_target() == 0.25);
    const int glide = static_cast<int>(std::lround(daphne::color::TubeColorStage::kAlignGlideMs * 1e-3 * rate));
    s.process(x.data(), glide / 2, y.data());
    CHECK(s.align() < 1.0);
    CHECK(s.align() > 0.25);
    s.process(x.data(), glide, y.data());
    CHECK(s.align() == 0.25);
    s.set_align(0.5, true);
    CHECK(s.align() == 0.5);
    // drive 0: the samples as they are (the delay compensated), whatever the alignment
    daphne::color::TubeColorStage z(params(0.0, 0.3));
    const auto yz = run_curve(z, x, rate);
    const auto d = static_cast<size_t>(z.content_delay_frames());
    bool same = true;
    for (size_t i = 2 * d; i < yz.size(); ++i) same = same && yz[i] == x[i - 2 * d];
    CHECK(same);
}

// Tube Color's reserve (settings.h, doc/N28): max(0, kColorHeadroomDb - the digital volume's attenuation) while it is on,
// 0 off; ReplayGain's headroom does not count. On the chain at drive 0 on a float device: the output is the input times
// the volume and the reserve, and a volume change moves the two together without a step (a constant input: the output
// level ramps; 0 -> -0.5 dB of volume leaves the total where it was, -kColorHeadroomDb).
TEST_CASE("color chain: the reserve, by the digital volume, without a step", "[color][chain]") {
    daphne::Settings s;
    CHECK(daphne::color_reserve_db(s) == 0.0);   // off
    s.color.enabled = true;
    if (!daphne::color_active(s)) SKIP("Tube Color cannot run in this build");
    const double h = daphne::kColorHeadroomDb;
    CHECK(daphne::color_reserve_db(s) == h);   // (the default volume mode: fixed, the host's)
    s.volume.mode = daphne::VolumeMode::Digital;
    CHECK(daphne::color_reserve_db(s) == h);
    s.volume.db = -1.0;
    CHECK(daphne::color_reserve_db(s) == Catch::Approx(h - 1.0));
    s.volume.db = -(h + 0.8);
    CHECK(daphne::color_reserve_db(s) == 0.0);
    s.volume.mode = daphne::VolumeMode::Fixed;   // the host's volume: nothing to count on
    CHECK(daphne::color_reserve_db(s) == h);
    s.volume.mode = daphne::VolumeMode::Digital;
    s.volume.db = 0.0;
    s.headroom_db = -6.0;   // ReplayGain: not the volume
    CHECK(daphne::color_reserve_db(s) == h);
    s.headroom_db = 0.0;
    s.color.drive = 0.0;
    daphne::AudioInfo src;
    src.rate = 44100, src.channels = 2, src.codec = "test";
    daphne::DeviceFormat dev;
    dev.rate = 44100, dev.channels = 2, dev.type = daphne::SampleType::Float32, dev.container_bits = dev.valid_bits = 32;
    daphne::DspChain c;
    std::string e;
    REQUIRE(c.build(s, src, dev, 4096, &e));
    const int block = 4096, frames = 40 * block;
    std::vector<double> x(2 * static_cast<size_t>(frames), 0.5), y, o(static_cast<size_t>(c.max_output_frames()) * 2);
    for (int at = 0; at < frames; at += block) {
        if (at == 10 * block || at == 20 * block || at == 30 * block) {
            daphne::Settings v = s;
            v.volume.db = at == 10 * block ? -0.5 : at == 20 * block ? -(h + 0.8) : 0.0;
            REQUIRE(c.update(v));
        }
        const int n = c.process(x.data() + 2 * static_cast<size_t>(at), block, o.data());
        y.insert(y.end(), o.begin(), o.begin() + 2 * static_cast<std::ptrdiff_t>(n));
    }
    auto db_at = [&](int frame) { return 20 * std::log10(y[2 * static_cast<size_t>(frame)] / 0.5); };
    CHECK(db_at(8 * block) == Catch::Approx(-h).margin(1e-9));          // volume 0: the reserve
    CHECK(db_at(18 * block) == Catch::Approx(-h).margin(1e-9));         // -0.5 dB of volume, the rest reserve
    CHECK(db_at(28 * block) == Catch::Approx(-(h + 0.8)).margin(1e-9)); // more than the room: the volume, no reserve
    CHECK(db_at(38 * block) == Catch::Approx(-h).margin(1e-9));         // back
    double step = 0.0;
    for (int n = 4 * block; n + 1 < static_cast<int>(y.size() / 2); ++n) step = std::max(step, std::abs(db_at(n + 1) - db_at(n)));
    INFO("the largest change from one frame to the next " << step << " dB");
    CHECK(step < 0.01);   // ramped (the gain stage's ramp), never a step
}


// The panel's pictures (doc/N28, chain.h color_preview): what the chain plays. Against a chain built from the same
// settings (48 kHz, a float device, the digital volume at -kColorHeadroomDb: no reserve, the volume cancelled around the
// curve, so it sees what the preview's stage sees): the harmonics of a 1 kHz sine at -18 dBFS RMS within 0.05 dB, and
// the response at three of the preview's frequencies within 0.05 dB (the volume added back; small tones: the curve
// linear). At drive 0: no harmonics, a flat response.
TEST_CASE("color preview: the harmonics and the response the chain plays", "[color][preview]") {
    daphne::Settings s;
    s.color.enabled = true;
    if (!daphne::color_active(s)) SKIP("Tube Color cannot run in this build");
    s.color.drive = 1.0;
    const auto p = daphne::color_preview(s, 1.0);
    REQUIRE(p["available"] == true);
    REQUIRE(p["harmonics"].size() == 6);
    const auto& hz = p["response"]["hz"];
    REQUIRE(hz.size() >= 120);
    CHECK(hz.front().get<double>() == 20.0);
    CHECK(hz.back().get<double>() == Catch::Approx(20000.0).margin(1.0));
    s.volume.mode = daphne::VolumeMode::Digital;
    s.volume.db = -daphne::kColorHeadroomDb;
    REQUIRE(daphne::color_reserve_db(s) == 0.0);
    daphne::AudioInfo src;
    src.rate = 48000, src.channels = 2, src.codec = "test";
    daphne::DeviceFormat dev;
    dev.rate = 48000, dev.channels = 2, dev.type = daphne::SampleType::Float32, dev.container_bits = dev.valid_bits = 32;
    auto through_chain = [&](double hz0, double amp) {
        daphne::DspChain c;
        std::string e;
        REQUIRE(c.build(s, src, dev, 4096, &e));
        const int frames = 48000;
        std::vector<double> x(2 * static_cast<size_t>(frames)), y, o(static_cast<size_t>(c.max_output_frames()) * 2);
        for (int i = 0; i < frames; ++i) x[2 * static_cast<size_t>(i)] = x[2 * static_cast<size_t>(i) + 1] = amp * std::sin(2 * std::numbers::pi * hz0 * i / 48000);
        for (int at = 0; at < frames; at += 4096) {
            const int n = c.process(x.data() + 2 * static_cast<size_t>(at), std::min(4096, frames - at), o.data());
            y.insert(y.end(), o.begin(), o.begin() + 2 * static_cast<std::ptrdiff_t>(n));
        }
        return y;
    };
    const double amp = std::pow(10.0, -18.0 / 20.0) * std::sqrt(2.0);
    const auto y = through_chain(1000.0, amp);
    const int n = static_cast<int>(y.size() / 2), from = n - 12000;
    auto mag = [&](const std::vector<double>& v, double f, int a, int b) { const auto [c, sn] = harmonic_of(v, 48000, f, 1, a, b); return std::hypot(c, sn); };
    const double h1 = mag(y, 1000.0, from, n);
    for (int k = 2; k <= 7; ++k) {
        INFO("H" << k);
        const double want = p["harmonics"][k - 2]["db"].get<double>();
        if (want < -100.0) continue;   // (none to speak of: the test curve's even ones, the far ones)
        CHECK(20 * std::log10(mag(y, 1000.0 * k, from, n) / h1) == Catch::Approx(want).margin(0.05));
    }
    for (size_t i : {size_t{12}, size_t{60}, size_t{100}}) {   // about 40 Hz, 640 Hz, 6.4 kHz
        const double f = hz[i].get<double>();
        INFO(f << " Hz");
        const auto t = through_chain(f, 1e-3);
        const int m = static_cast<int>(t.size() / 2), len = static_cast<int>(std::floor(0.4 * f)) * 48000 / static_cast<int>(std::lround(f));
        const int a = m - std::max(len, 4800);
        CHECK(20 * std::log10(mag(t, f, a, m) / 1e-3) + daphne::kColorHeadroomDb == Catch::Approx(p["response"]["db"][i].get<double>()).margin(0.05));
    }
    const auto z = daphne::color_preview(s, 0.0);
    for (const auto& x : z["harmonics"]) CHECK(x["db"].get<double>() < -150.0);
    for (const auto& d : z["response"]["db"]) CHECK(std::abs(d.get<double>()) < 1e-6);
}
