#include <catch_amalgamated.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

#include "../dsp/src/fft.h"
#include "daphne/dsp/limiter.h"
#include "daphne/dsp/modulator.h"
#include "daphne/engine/settings.h"

using daphne::dsp::DsdModulator;

namespace {

const double kPi = 3.14159265358979323846;

// amplitude of the f component of one channel of the DSD bytes (stereo interleaved), skipping the start
double tone_level(const std::vector<uint8_t>& out, int channels, int c, double f, int dsd_rate, int64_t skip_bits) {
    double re = 0, im = 0;
    int64_t count = 0;
    const double w = 2 * kPi * f / dsd_rate;
    for (size_t k = c; k < out.size(); k += channels)
        for (int bit = 0; bit < 8; ++bit) {
            const int64_t t = static_cast<int64_t>(k / channels) * 8 + bit;
            if (t < skip_bits) continue;
            const double s = (out[k] >> (7 - bit) & 1) ? 1.0 : -1.0;
            re += s * std::cos(w * t);
            im += s * std::sin(w * t);
            ++count;
        }
    return 2 * std::hypot(re, im) / count;
}

// Noise power of one channel of the DSD bytes in [f1, f2] Hz, in dB re the SACD reference (a sine at 50% modulation):
// Hann-windowed power spectrum of n bits after skip_bits, without the bins around f0 and its harmonics (f0 = 0: none).
double band_noise_db(const std::vector<uint8_t>& out, int channels, int dsd_rate, int64_t skip_bits, int n, double f1, double f2,
                     double f0) {
    std::vector<std::complex<double>> x(static_cast<size_t>(n));
    double wsum = 0;
    for (int t = 0; t < n; ++t) {
        const int64_t b = skip_bits + t;
        const uint8_t byte = out[static_cast<size_t>(b / 8) * channels];
        const double w = 0.5 - 0.5 * std::cos(2 * kPi * t / n);
        x[static_cast<size_t>(t)] = (byte >> (7 - b % 8) & 1 ? w : -w);
        wsum += w * w;
    }
    daphne::dsp::Fft(n).run(x.data(), false);
    const double bin = static_cast<double>(dsd_rate) / n;
    double ms = 0;
    for (int k = static_cast<int>(std::ceil(f1 / bin)); k <= static_cast<int>(f2 / bin); ++k) {
        bool tone = false;
        for (double h = f0; f0 > 0 && h <= f2 + 10 * bin; h += f0) tone = tone || std::abs(k * bin - h) <= 10 * bin;
        if (!tone) ms += 2 * std::norm(x[static_cast<size_t>(k)]) / (static_cast<double>(n) * wsum);
    }
    return 10 * std::log10(ms / 0.125 + 1e-300);
}

}  // namespace

TEST_CASE("Modulator: stays within the scaled range and encodes the signal at the reference gain", "[modulator]") {
    const int fs = 44100, n = 8192;
    for (int order : {5, 8}) {
        std::vector<double> x(static_cast<size_t>(n) * 2);
        for (int i = 0; i < n; ++i) x[2 * i] = x[2 * i + 1] = 0.9 * std::sin(2 * kPi * 1000 * i / fs);   // -0.9 dBFS
        DsdModulator m(fs, 64, order);
        CHECK(m.order() == order);
        CHECK(m.dsd_rate() == 2822400);
        std::vector<uint8_t> out;
        m.process(x.data(), n, 2, out);
        REQUIRE(out.size() == static_cast<size_t>(n) * 8 * 2);
        CHECK(m.clamped_bits() == 0);
        CHECK(m.loop_resets() == 0);
        CHECK(m.input_clips() == 0);
        CHECK(tone_level(out, 2, 0, 1000, m.dsd_rate(), 64 * 1024) == Catch::Approx(0.9 * DsdModulator::kGain).margin(0.01));
    }
    // far beyond full scale: the input is clipped, the loop never needs a reset
    std::vector<double> loud(static_cast<size_t>(n) * 2);
    for (int i = 0; i < n; ++i) loud[2 * i] = loud[2 * i + 1] = 4.0 * std::sin(2 * kPi * 3000 * i / fs);
    DsdModulator m(fs, 64, 8);
    std::vector<uint8_t> out;
    m.process(loud.data(), n, 2, out);
    CHECK(m.input_clips() > 0);
    CHECK(m.loop_resets() == 0);
}

TEST_CASE("Modulator: worker thread and uneven chunks give the same bytes", "[modulator]") {
    const int fs = 705600, n = 1 << 16;
    std::vector<double> x(static_cast<size_t>(n) * 2);
    for (int i = 0; i < n; ++i) {
        x[2 * i] = 0.7 * std::sin(2 * kPi * 997 * i / fs);
        x[2 * i + 1] = 0.5 * std::sin(2 * kPi * 3000 * i / fs + 1.0);
    }
    DsdModulator a(fs, 128, 8), b(fs, 128, 8);
    b.set_threaded(false);
    std::vector<uint8_t> oa, ob, oc;
    for (int k = 0; k < n; k += 4099) {   // uneven chunks: partial bytes carry over between calls
        const int m = std::min(4099, n - k);
        a.process(x.data() + 2 * static_cast<size_t>(k), m, 2, oa);
        b.process(x.data() + 2 * static_cast<size_t>(k), m, 2, ob);
    }
    DsdModulator c(fs, 128, 8);
    c.process(x.data(), n, 2, oc);
    CHECK(oa == ob);
    CHECK(oa == oc);
    CHECK(oa.size() == static_cast<size_t>(n) * 2);   // 5.6448 MHz / 705.6 kHz = 8 bits = one byte per frame and channel
}

TEST_CASE("Modulator: a rate that is not a power-of-two ratio (interpolated phase)", "[modulator]") {
    const int fs = 768000, n = 768000 / 4;   // 48 kHz family on the 44.1 kHz-family clock
    std::vector<double> x(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) x[i] = 0.5 * std::sin(2 * kPi * 1000 * i / fs);
    DsdModulator m(fs, 128, 8);
    std::vector<uint8_t> out;
    m.process(x.data(), n, 1, out);
    // 0.25 s at 5.6448 MHz: 176400 bytes (the phase accumulates: within one byte)
    CHECK(std::abs(static_cast<long long>(out.size()) - 176400) <= 1);
    CHECK(m.clamped_bits() == 0);
    CHECK(tone_level(out, 1, 0, 1000, m.dsd_rate(), 64 * 1024) == Catch::Approx(0.5 * DsdModulator::kGain).margin(0.01));
}

TEST_CASE("Modulator: 40 - 100 kHz below -20 dB re the SACD reference (Scarlet Book D4), a quiet audio band", "[modulator]") {
    // DSD64 / DSD128, orders 5 and 8, silence and a sine near 1 kHz at -20 / -6 dBFS at the chain's 705.6 kHz. Order 8 at DSD64
    // takes an optimized NTF (tools/sdm_design.py, H_OPT): synthesizeNTF's measured -18.5 dB there
    const int fs = 705600, n = 1 << 20;
    for (int osr : {64, 128})
        for (int order : {5, 8})
            for (double db : {-300.0, -20.0, -6.0}) {
                DsdModulator m(fs, osr, order);
                const int64_t skip = m.dsd_rate() / 2;   // the DC blocker (3.5 Hz) settles from the start of the sine
                const int frames = static_cast<int>((skip + n) * static_cast<int64_t>(fs) / m.dsd_rate()) + 64;
                const double a = db < -200 ? 0.0 : std::pow(10.0, db / 20);
                const double f0 = std::round(1000.0 * n / m.dsd_rate()) * m.dsd_rate() / n;   // on a bin: no window leakage
                std::vector<double> x(static_cast<size_t>(frames) * 2);
                for (int i = 0; i < frames; ++i) x[2 * i] = x[2 * i + 1] = a * std::sin(2 * kPi * f0 * i / fs);
                std::vector<uint8_t> out;
                m.process(x.data(), frames, 2, out);
                INFO("order " << order << " DSD" << osr << " at " << db << " dBFS");
                CHECK(m.clamped_bits() == 0);
                CHECK(band_noise_db(out, 2, m.dsd_rate(), skip, n, 40000, 100000, 0) < -20.0);
                // audio band: order 8 DSD64 measured -134 dB at -20 dBFS, the others lower or at least -115
                CHECK(band_noise_db(out, 2, m.dsd_rate(), skip, n, 20, 20000, a > 0 ? f0 : 0.0) < (order == 8 ? -128.0 : -110.0));
            }
}

TEST_CASE("Limiter: no sample above the ceiling, bit-exact delay below it", "[limiter]") {
    daphne::dsp::Limiter lim(0.9, 2.0, 80.0);
    lim.prepare({48000, 2, 4800});
    const int w = lim.latency_frames();
    CHECK(w == 96);
    const int n = 48000 * 3;
    std::vector<double> x(2 * n), y(2 * n);
    for (int i = 0; i < n; ++i) x[2 * i] = x[2 * i + 1] = (i > 10000 && i < 12000 ? 1.4 : 0.5) * std::sin(2 * kPi * 440 * i / 48000);
    for (int off = 0; off < n; off += 4800) lim.process(x.data() + 2 * off, 4800, y.data() + 2 * off);
    double peak = 0;
    for (double v : y) peak = std::max(peak, std::abs(v));
    CHECK(peak <= 0.9 + 1e-12);
    CHECK(lim.limited_frames() > 0);
    // before the loud part: the input delayed by w frames, bit for bit
    for (int i = w; i < 9000; ++i) REQUIRE(y[2 * i] == x[2 * (i - w)]);
    // 0.4 s after it the one-pole release (80 ms) is within 0.05 dB of unity ...
    for (int i = 12000 + 19200; i < 12000 + 20000; ++i) REQUIRE(std::abs(y[2 * i] - x[2 * (i - w)]) <= 0.006 * std::abs(x[2 * (i - w)]) + 1e-15);
    // ... and once it is within 1e-9 (about 21 time constants, 1.7 s) it snaps to 1: bit for bit again
    for (int i = 12000 + 48000 * 2; i < n; ++i) REQUIRE(y[2 * i] == x[2 * (i - w)]);
}

// Tube Color's soft limiter (doc/N28): a knee in dB around the ceiling. The gain asked for is continuous and never lets
// a peak out above the ceiling; below the knee nothing changes; pass-through keeps the delay and is bit-exact.
TEST_CASE("Limiter: the soft knee", "[limiter]") {
    const double ceiling = daphne::kColorLimitCeiling, knee = daphne::kColorLimitKneeDb;
    daphne::dsp::Limiter lim(ceiling, 2.0, 80.0, knee);
    const double t = 20 * std::log10(ceiling);
    // the static curve: 1 below the knee, peak * gain <= ceiling everywhere, continuous and monotonic in output
    double prev_out = 0.0, prev_g = 1.0;
    for (double x_db = t - 10; x_db <= t + 12; x_db += 0.01) {
        const double peak = std::pow(10.0, x_db / 20), g = lim.request(peak);
        INFO(x_db);
        if (x_db <= t - knee / 2 - 1e-9) CHECK(g == 1.0);
        CHECK(peak * g <= ceiling * (1 + 1e-12));
        CHECK(peak * g >= prev_out - 1e-12);          // more in, never less out
        CHECK(std::abs(g - prev_g) < 2e-3);           // no jump (0.01 dB steps)
        prev_out = peak * g, prev_g = g;
    }
    CHECK(lim.request(std::pow(10.0, (t + knee / 2) / 20)) == Catch::Approx(1.0 / std::pow(10.0, knee / 2 / 20)).epsilon(1e-9));
    // on a signal: overs bent down below the ceiling; quiet passages bit-exact (delayed)
    lim.prepare({48000, 2, 4800});
    const int w = lim.latency_frames(), n = 48000;
    std::vector<double> x(2 * n), y(2 * n);
    for (int i = 0; i < n; ++i) {
        const double a = i < n / 2 ? 0.5 : 1.4;   // a quiet half, then 3 dB above full scale
        x[2 * i] = x[2 * i + 1] = a * std::sin(2 * 3.141592653589793 * 440.0 * i / 48000.0);
    }
    lim.process(x.data(), n, y.data());
    double over = 0.0;
    for (double v : y) over = std::max(over, std::abs(v));
    CHECK(over <= ceiling * (1 + 1e-12));
    CHECK(lim.limited_frames() > 0);
    bool exact = true;
    for (int i = w; i < n / 2 - 200; ++i) exact = exact && y[2 * i] == x[2 * (i - w)];   // before the overs' look-ahead
    CHECK(exact);
    // pass-through: the delay stays, the samples are the input's, bit for bit, once the gain has released
    lim.set_pass_through(true);
    std::vector<double> z(2 * n);
    lim.process(x.data(), n, z.data());
    exact = true;
    const int back = static_cast<int>(daphne::dsp::Limiter::kPassReleaseMs * 48) + 2 * w;   // the straight release, the window
    for (int i = back; i < n; ++i) exact = exact && z[2 * i] == x[2 * (i - w)] && z[2 * i + 1] == x[2 * (i - w) + 1];
    CHECK(exact);
}
