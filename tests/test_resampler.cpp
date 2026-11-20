#include <catch_amalgamated.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <vector>

#include <nlohmann/json.hpp>

#include "daphne/dsp/filter_design.h"
#include "daphne/dsp/progress.h"
#include "daphne/dsp/resampler.h"
#include "daphne/engine/filters.h"
#include "test_util.h"
#include "daphne/engine/settings.h"

using daphne::dsp::Resampler;

namespace {

constexpr double kPi = 3.14159265358979323846;

// runs r over x (interleaved stereo) in chunks of `chunk`, then flushes
std::vector<double> run(Resampler& r, const std::vector<double>& x, int chunk) {
    const int frames = static_cast<int>(x.size() / 2);
    r.prepare({44100, 2, chunk});
    std::vector<double> y(static_cast<size_t>(r.max_output_frames(frames) + r.max_output_frames(chunk)) * 2);
    int done = 0;
    for (int i = 0; i < frames; i += chunk) {
        const int n = std::min(chunk, frames - i);
        done += r.process(x.data() + static_cast<size_t>(i) * 2, n, y.data() + static_cast<size_t>(done) * 2);
    }
    done += r.flush(y.data() + static_cast<size_t>(done) * 2);
    y.resize(static_cast<size_t>(done) * 2);
    return y;
}

std::vector<double> sine(int frames, double f, double fs, double amp) {
    std::vector<double> x(static_cast<size_t>(frames) * 2);
    for (int i = 0; i < frames; ++i) x[2 * i] = x[2 * i + 1] = amp * std::sin(2 * kPi * f * i / fs);
    return x;
}

}  // namespace

TEST_CASE("Resampler: a unit impulse at ratio 1 copies the samples bit for bit", "[resampler]") {
    std::vector<double> h(1024, 0.0);
    h[0] = 1.0;
    Resampler r(h, 1, daphne::dsp::resampler_block_size(1024));
    std::vector<double> x(2 * 5000);
    for (size_t i = 0; i < x.size(); ++i) x[i] = std::sin(0.001 * double(i * i % 9973)) * 0.7 + 1e-9;
    const auto y = run(r, x, 777);
    CHECK(r.is_identity());
    CHECK(y == x);
}

TEST_CASE("Resampler: length, DC and a sine against the ideal upsampled sine", "[resampler]") {
    const int taps = 1024, ratio = 4, L = taps * ratio;
    auto h = daphne::dsp::design_lowpass(taps, ratio, 44100, 176400, 0.0, false);
    Resampler r(h, ratio, daphne::dsp::resampler_block_size(L));
    const int frames = 44100;
    const auto x = sine(frames, 1000.0, 44100, 0.5);
    const auto y = run(r, x, 512);
    REQUIRE(y.size() == static_cast<size_t>(frames) * ratio * 2);   // N in, N * ratio out
    // linear phase: output j is the input at time (j - (L - 1) / 2) / ratio
    double err = 0.0;
    for (int j = L; j < frames * ratio - L; ++j) {
        const double want = 0.5 * std::sin(2 * kPi * 1000.0 * (j - (L - 1) / 2.0) / 176400.0);
        err = std::max(err, std::abs(y[2 * j] - want));
        REQUIRE(std::abs(y[2 * j] - y[2 * j + 1]) < 1e-14);   // real / imaginary halves of one FFT: within rounding
    }
    CHECK(20 * std::log10(err / 0.5) < -130.0);
    CHECK(r.peak_index() == L / 2 - 1);

    // DC: every phase normalized to gain 1
    Resampler d(h, ratio, daphne::dsp::resampler_block_size(L));
    std::vector<double> dc(2 * 20000, 0.25);
    const auto yd = run(d, dc, 4096);
    for (int j = L; j < 20000 * ratio - L; ++j) REQUIRE(std::abs(yd[2 * j] - 0.25) < 1e-15);
}

TEST_CASE("Resampler: intersample peaks above full scale are kept", "[resampler]") {
    const int taps = 512, ratio = 4;
    auto h = daphne::dsp::design_lowpass(taps, ratio, 44100, 176400, 0.0, false);
    Resampler r(h, ratio, daphne::dsp::resampler_block_size(taps * ratio));
    // fs/4 at 45 degrees: the samples are +-0.7071 x 1.2 = 0.85, the waveform peaks at 1.2
    std::vector<double> x(2 * 8192);
    for (int i = 0; i < 8192; ++i) x[2 * i] = x[2 * i + 1] = 1.2 * std::sin(kPi / 2 * i + kPi / 4);
    const auto y = run(r, x, 1024);
    double peak = 0.0, in_peak = 0.0;
    for (double v : y) peak = std::max(peak, std::abs(v));
    for (double v : x) in_peak = std::max(in_peak, std::abs(v));
    CHECK(in_peak < 1.0);
    CHECK(peak > 1.0);   // the continuous waveform peaks above the samples: not clamped
}

TEST_CASE("Resampler: double-double convolution, any thread count gives the same bits", "[resampler]") {
    const int taps = 1024, ratio = 8, L = taps * ratio;
    std::vector<double> lo;
    auto h = daphne::dsp::design_lowpass(taps, ratio, 44100, 352800, 0.5, false, &lo);
    const auto x = sine(30000, 997.0, 44100, 0.5);
    Resampler plain(h, ratio, daphne::dsp::resampler_block_size(L));
    const auto yp = run(plain, x, 1000);
    daphne::dsp::set_design_threads(1);
    Resampler d1(h, ratio, daphne::dsp::resampler_block_size(L), &lo);
    const auto y1 = run(d1, x, 1000);
    daphne::dsp::set_design_threads(8);
    Resampler d8(h, ratio, daphne::dsp::resampler_block_size(L), &lo);
    const auto y8 = run(d8, x, 1000);
    daphne::dsp::set_design_threads(0);
    CHECK(d1.double_double());
    CHECK(y1 == y8);
    // the kernel peak (content delay) is the same on both paths, and is the largest coefficient
    CHECK(d1.peak_index() == plain.peak_index());
    size_t best = 0;
    for (size_t i = 1; i < h.size(); ++i)
        if (std::abs(h[i]) > std::abs(h[best])) best = i;
    CHECK(plain.peak_index() == static_cast<int>(best));
    REQUIRE(y1.size() == yp.size());
    double diff = 0.0;
    for (size_t i = 0; i < y1.size(); ++i) diff = std::max(diff, std::abs(y1[i] - yp[i]));
    CHECK(diff > 0.0);     // a different (more exact) computation
    CHECK(diff < 1e-13);   // double rounding of the plain path only
}

// NaN and infinity (possible through settings_from_json, never in JSON text) are out of every range: an error at the
// field, which keeps its default (a range check written as v < lo || v > hi let NaN through; found by the color
// property test, doc/N28)
TEST_CASE("Settings: NaN and infinity are out of range", "[settings]") {
    using nlohmann::json;
    for (const double bad : {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
                             -std::numeric_limits<double>::infinity()}) {
        const json j = {{"headroom_db", bad},
                        {"volume", {{"mode", "digital"}, {"db", bad}}},
                        {"filter", {{"phase", "custom"}, {"phase_custom", bad}}},
                        {"crossfeed", {{"fc", bad}, {"amount", bad}}},
                        {"eq", {{"preamp_db", bad}, {"bands", json::array({{{"type", "PK"}, {"f", bad}, {"q", bad}, {"gain_db", bad}}})}}}};
        const auto r = daphne::settings_from_json(j);
        CHECK_FALSE(r.ok());
        int errors = 0;
        for (const auto& d : r.diagnostics) errors += d.level == daphne::Diagnostic::Level::Error;
        CHECK(errors == 9);
        const daphne::Settings def;
        CHECK(r.settings.headroom_db == def.headroom_db);
        CHECK(r.settings.volume.db == def.volume.db);
        CHECK(r.settings.filter.phase_custom == def.filter.phase_custom);
        CHECK(r.settings.crossfeed.fc == def.crossfeed.fc);
        CHECK(r.settings.crossfeed.amount == def.crossfeed.amount);
        CHECK(r.settings.eq.preamp_db == def.eq.preamp_db);
        // written back: no NaN or infinity anywhere (the text would carry null)
        CHECK(daphne::settings_from_string(daphne::settings_to_json(r.settings).dump()).ok());
    }
}

TEST_CASE("Output rate for a source rate", "[settings]") {
    using daphne::pcm_output_rate;
    CHECK(pcm_output_rate(44100, "native") == 44100);
    CHECK(pcm_output_rate(44100, "auto") == 705600);
    CHECK(pcm_output_rate(48000, "auto") == 768000);
    CHECK(pcm_output_rate(88200, "auto") == 705600);
    CHECK(pcm_output_rate(384000, "auto") == 768000);
    CHECK(pcm_output_rate(705600, "auto") == 705600);
    CHECK(pcm_output_rate(44100, "2x") == 88200);
    CHECK(pcm_output_rate(96000, "2x") == 192000);
    CHECK(pcm_output_rate(44100, "192000") == 176400);
    CHECK(pcm_output_rate(48000, "192000") == 192000);
    CHECK(pcm_output_rate(96000, "100000") == 96000);   // no ratio fits: the source rate
    CHECK(daphne::modulator_input_rate(44100) == 705600);
    CHECK(daphne::modulator_input_rate(96000) == 768000);
    CHECK(daphne::dsd_clock(128) == 5644800);
}

TEST_CASE("Settings: defaults round trip, errors and warnings", "[settings]") {
    const auto d = daphne::settings_to_json(daphne::Settings{});
    auto r = daphne::settings_from_json(d);
    CHECK(r.ok());
    CHECK(r.diagnostics.empty());
    CHECK(daphne::settings_to_json(r.settings) == d);

    r = daphne::settings_from_string(R"({"filter": {"taps": 8192, "phase": "custom", "phase_custom": 0.3, "apodize": true},
        "core": "dsd", "dsd_modulator": {"order": 5, "osr": 256}, "output": {"rate": "2x", "dsd": "dop"},
        "eq": {"enabled": true, "preamp_db": -3, "bands": [{"type": "LS", "f": 100, "q": 0.7, "gain_db": 4}]},
        "headroom_db": 6.5, "extra": 1})");
    CHECK(r.ok());
    REQUIRE(r.diagnostics.size() == 1);   // "extra": unknown, ignored
    CHECK(r.diagnostics[0].level == daphne::Diagnostic::Level::Warning);
    const auto& s = r.settings;
    CHECK(s.filter.taps == 8192);
    CHECK(s.filter.phase_alpha() == 0.3);
    CHECK(s.filter.apodize);
    CHECK(s.core == daphne::CoreMode::Dsd);
    CHECK(s.modulator.order == 5);
    CHECK(s.modulator.osr == 256);
    CHECK(s.output.rate == "2x");
    CHECK(s.output.dsd == daphne::DsdMode::Dop);
    CHECK(s.eq.preamp_db == -3.0);
    CHECK(s.eq.bands.size() == 1);
    CHECK(s.headroom_db == 6.5);

    r = daphne::settings_from_string(R"({"filter": {"taps": 3}, "dsd_modulator": {"order": 15}, "output": {"rate": "fast"}})");
    CHECK_FALSE(r.ok());
    CHECK(r.diagnostics.size() == 3);
    CHECK(r.settings.filter.taps == 2048);   // invalid fields keep their defaults
    CHECK_FALSE(daphne::settings_from_string("{ not json").ok());
}

TEST_CASE("Settings: output.buffer_ms is 0 (the device default) or up to 200 ms", "[settings]") {
    for (int ms : {0, 10, 50, 200}) {
        const auto r = daphne::settings_from_string(R"({"output": {"buffer_ms": )" + std::to_string(ms) + "}}");
        CHECK(r.ok());
        CHECK(r.settings.output.buffer_ms == ms);
    }
    // beyond 200 ms pause and seek would wait for a whole buffer (N24): an error, the default kept
    const auto r = daphne::settings_from_string(R"({"output": {"buffer_ms": 250}})");
    CHECK_FALSE(r.ok());
    CHECK(r.settings.output.buffer_ms == 0);
}

TEST_CASE("Filter cache: store, load, low parts, prune", "[cache]") {
    const auto dir = test::temp_path("daphne_cache_test");
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    test::set_env("DAPHNE_FILTER_CACHE", &dir);

    daphne::FilterSettings f;
    f.taps = 1024;
    f.phase = daphne::FilterPhase::Minimum;
    std::vector<double> lo1, lo2;
    CHECK_FALSE(daphne::filter_cached(f, 44100, 176400));
    daphne::design_filter(f, 44100, 176400);   // without its low parts: not cached for double-double
    CHECK(daphne::filter_cached(f, 44100, 176400));
    f.double_double = true;
    CHECK_FALSE(daphne::filter_cached(f, 44100, 176400));
    f.double_double = false;
    CHECK(daphne::filter_cached(f, 44100, 44100));   // a unit impulse: nothing to design
    const auto h1 = daphne::design_filter(f, 44100, 176400, &lo1);
    const auto key = daphne::filter_cache_key(f, 44100, 176400);
    std::vector<double> c;
    REQUIRE(daphne::load_cached_filter(key, c));
    CHECK(c == h1);
    REQUIRE(daphne::load_cached_filter(key + "_lo", c));
    CHECK(c == lo1);
    const auto h2 = daphne::design_filter(f, 44100, 176400, &lo2);   // from the cache
    CHECK(h2 == h1);
    CHECK(lo2 == lo1);
    f.double_double = true;
    CHECK(daphne::filter_cached(f, 44100, 176400));
    f.double_double = false;
    // ratio 1 without apodizing is not cached
    daphne::design_filter(f, 44100, 44100);
    CHECK_FALSE(daphne::load_cached_filter(daphne::filter_cache_key(f, 44100, 44100), c));
    // a different phase is a different key
    f.phase_custom = 0.25;
    f.phase = daphne::FilterPhase::Custom;
    CHECK(daphne::filter_cache_key(f, 44100, 176400) != key);
    daphne::design_filter(f, 44100, 176400);

    // a damaged file under the key (another length, a NaN) is designed again, and the result stored over it
    f.phase = daphne::FilterPhase::Minimum;
    f.phase_custom = 0.5;
    REQUIRE(daphne::filter_cache_key(f, 44100, 176400) == key);
    for (const auto& bad : {std::vector<double>(h1.begin(), h1.begin() + 100),
                            [&] { auto v = h1; v[7] = std::numeric_limits<double>::quiet_NaN(); return v; }()}) {
        daphne::store_cached_filter(key, bad);
        CHECK(daphne::design_filter(f, 44100, 176400) == h1);
        REQUIRE(daphne::load_cached_filter(key, c));
        CHECK(c == h1);
    }
    f.phase_custom = 0.25;
    f.phase = daphne::FilterPhase::Custom;

    // prune to zero keeping the first filter
    const auto r = daphne::prune_filter_cache(0, {key, key + "_lo"});
    CHECK(r.removed_files == 1);
    CHECK(r.files == 2);
    CHECK(daphne::load_cached_filter(key, c));

    // the playing track's own rate is kept too when it is not one of the usual ones
    daphne::Settings st;
    st.output.rate = "176400";
    const auto k32 = daphne::filter_cache_key(st.filter, 32000, daphne::pcm_output_rate(32000, st.output.rate));
    const auto usual = daphne::filter_cache_keys_in_use(st), with32 = daphne::filter_cache_keys_in_use(st, 32000);
    CHECK(std::find(usual.begin(), usual.end(), k32) == usual.end());
    CHECK(std::find(with32.begin(), with32.end(), k32) != with32.end());
    CHECK(daphne::filter_cache_keys_in_use(st, 44100) == usual);   // a usual rate adds nothing

    test::set_env("DAPHNE_FILTER_CACHE", nullptr);
    std::filesystem::remove_all(dir, ec);
}
