// Tube Color's level alignment (doc/N28, an M2 experiment): the track measures and the gain (track_levels.h).
#include <catch_amalgamated.hpp>

#include <atomic>
#include <cmath>
#include <numbers>
#include <random>
#include <utility>
#include <vector>

#include "daphne/engine/track_levels.h"

using daphne::ColorAlign;
using daphne::LevelMeter;
using daphne::TrackLevels;

namespace {

// a stereo 1 kHz sine in segments of (dBFS, seconds), both channels alike (EBU Tech 3341's signals)
std::vector<double> segments(int rate, const std::vector<std::pair<double, double>>& segs) {
    std::vector<double> x;
    long long n0 = 0;
    for (const auto& [db, sec] : segs) {
        const long long n = std::llround(sec * rate);
        const double a = std::pow(10.0, db / 20.0);
        for (long long i = 0; i < n; ++i) {
            const double v = a * std::sin(2.0 * std::numbers::pi * 1000.0 * static_cast<double>(n0 + i) / rate);
            x.push_back(v);
            x.push_back(v);
        }
        n0 += n;
    }
    return x;
}

TrackLevels measure(const std::vector<double>& x, int rate, int chunk = 4096) {
    LevelMeter m(rate, 2);
    const int frames = static_cast<int>(x.size() / 2);
    for (int at = 0; at < frames; at += chunk) m.add(x.data() + 2 * static_cast<size_t>(at), std::min(chunk, frames - at));
    return m.result();
}

}  // namespace

// BS.1770-4 integrated loudness on the synthetic signals of EBU Tech 3341 (also ITU-R BS.2217's): cases 3 and 4 hold
// segments the relative and the absolute gate must leave out (without them: -24.6 and -24.8). verify/loudness.py's
// self-test measures the same (0.024 LU at most); here within the cases' 0.1 LU.
TEST_CASE("track levels: integrated loudness on EBU Tech 3341's signals", "[levels]") {
    const std::vector<std::pair<std::vector<std::pair<double, double>>, double>> cases = {
        {{{-23, 20}}, -23.0},
        {{{-33, 20}}, -33.0},
        {{{-36, 10}, {-23, 60}, {-36, 10}}, -23.0},
        {{{-72, 10}, {-36, 10}, {-23, 60}, {-36, 10}, {-72, 10}}, -23.0},
        {{{-26, 20}, {-20, 20.1}, {-26, 20}}, -23.0},
    };
    for (int rate : {48000, 44100, 96000}) {
        for (size_t k = 0; k < cases.size(); ++k) {
            INFO(rate << " Hz, case " << k + 1);
            CHECK(measure(segments(rate, cases[k].first), rate).lufs == Catch::Approx(cases[k].second).margin(0.1));
        }
    }
}

// The 99.9th percentile of |x|: a sine's is next to its amplitude (|sin| is above 0.99999 of it for 0.3 % of the time);
// gaussian noise's 3.29 sigma; reading in chunks of any size gives the same; silence gives -inf for both.
TEST_CASE("track levels: the 99.9th percentile, chunks, silence", "[levels]") {
    const int rate = 44100;
    const auto s = segments(rate, {{-6.0, 5.0}});
    CHECK(measure(s, rate).p999_db == Catch::Approx(-6.0).margin(0.02));
    std::mt19937 g(3);
    std::normal_distribution<double> n(0.0, 0.1);
    std::vector<double> w(2 * 10 * static_cast<size_t>(rate));
    for (auto& v : w) v = n(g);
    CHECK(measure(w, rate).p999_db == Catch::Approx(20.0 * std::log10(0.1 * 3.2905)).margin(0.05));
    const auto a = measure(w, rate, 4096), b = measure(w, rate, 333);
    CHECK(a.lufs == b.lufs);
    CHECK(a.p999_db == b.p999_db);
    const auto z = measure(std::vector<double>(2 * 44100, 0.0), rate);
    CHECK(std::isinf(z.lufs));
    CHECK(std::isinf(z.p999_db));
}

// The gain: only ever down, each mode its measure against the calibration's, conservative the lower; a track as loud
// as the calibration signal (or quieter) is not turned down.
TEST_CASE("track levels: the alignment's gain", "[levels]") {
    const TrackLevels loud{-4.1, -1.0}, quiet{-16.0, -9.0}, crest{-14.3, -2.0};
    CHECK(daphne::color_align_gain_db(ColorAlign::Off, loud) == 0.0);
    CHECK(daphne::color_align_gain_db(ColorAlign::Loudness, loud) == Catch::Approx(daphne::kAlignRefLufs + 4.1));
    CHECK(daphne::color_align_gain_db(ColorAlign::NearPeak, loud) == Catch::Approx(daphne::kAlignRefP999Db + 1.0));
    CHECK(daphne::color_align_gain_db(ColorAlign::Conservative, loud) == Catch::Approx(daphne::kAlignRefLufs + 4.1));
    for (auto m : {ColorAlign::Off, ColorAlign::Loudness, ColorAlign::NearPeak, ColorAlign::Conservative})
        CHECK(daphne::color_align_gain_db(m, quiet) == 0.0);
    // as loud as the calibration, its peaks higher: near-peak (and conservative) turn it down, loudness hardly
    CHECK(daphne::color_align_gain_db(ColorAlign::Loudness, crest) == Catch::Approx(0.0).margin(0.05));
    CHECK(daphne::color_align_gain_db(ColorAlign::Conservative, crest) == Catch::Approx(daphne::kAlignRefP999Db + 2.0));
    CHECK(daphne::color_align_gain_db(ColorAlign::Loudness, TrackLevels{-HUGE_VAL, -HUGE_VAL}) == 0.0);
}

// The cache: by path; a file that is not on disk by its path alone; cleared.
TEST_CASE("track levels: the cache", "[levels]") {
    daphne::clear_level_cache();
    CHECK(!daphne::cached_levels("no such file.flac"));
    daphne::store_levels("no such file.flac", {-7.0, -1.5});
    const auto c = daphne::cached_levels("no such file.flac");
    REQUIRE(c);
    CHECK(c->lufs == -7.0);
    CHECK(c->p999_db == -1.5);
    daphne::clear_level_cache();
    CHECK(!daphne::cached_levels("no such file.flac"));
}
