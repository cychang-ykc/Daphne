#include "daphne/engine/track_levels.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <numbers>
#include <string>
#include <system_error>

#include "daphne/platform/platform.h"

namespace daphne {

namespace {

constexpr double kHistLowDb = -140.0, kHistStepDb = 0.01;
constexpr int kHistBins = static_cast<int>((20.0 - kHistLowDb) / kHistStepDb);   // up to +20 dBFS

std::mutex g_cache_m;
std::map<std::string, TrackLevels> g_cache;

std::string cache_key(const std::filesystem::path& p) {
    std::string k = platform::path_to_utf8(p);
    std::error_code ec;
    const auto size = std::filesystem::file_size(p, ec);
    if (!ec) {
        k += "|" + std::to_string(size);
        const auto t = std::filesystem::last_write_time(p, ec);
        if (!ec) k += "|" + std::to_string(static_cast<long long>(t.time_since_epoch().count()));   // (macOS: an __int128 count)
    }
    return k;
}

}  // namespace

double color_align_gain_db(ColorAlign mode, const TrackLevels& l) {
    const double loud = std::isfinite(l.lufs) ? std::min(0.0, kAlignRefLufs - l.lufs) : 0.0;
    const double peak = std::isfinite(l.p999_db) ? std::min(0.0, kAlignRefP999Db - l.p999_db) : 0.0;
    switch (mode) {
        case ColorAlign::Loudness: return loud;
        case ColorAlign::NearPeak: return peak;
        case ColorAlign::Conservative: return std::min(loud, peak);
        case ColorAlign::Off: break;
    }
    return 0.0;
}

// BS.1770's two stages designed at the rate from their analog prototypes (at 48 kHz the published coefficients;
// verify/loudness.py k_weighting)
LevelMeter::LevelMeter(int rate, int channels)
    : rate_(std::max(1, rate)), ch_(std::max(1, channels)), step_(std::max(1, static_cast<int>(std::lround(0.1 * rate_)))) {
    {
        const double fc = 1681.974450955533, g = 3.999843853973347, q = 0.7071752369554196;
        const double k = std::tan(std::numbers::pi * fc / rate_), vh = std::pow(10.0, g / 20.0), vb = std::pow(vh, 0.4996667741545416);
        const double a0 = 1.0 + k / q + k * k;
        shelf_ = {(vh + vb * k / q + k * k) / a0, 2.0 * (k * k - vh) / a0, (vh - vb * k / q + k * k) / a0, 2.0 * (k * k - 1.0) / a0,
                  (1.0 - k / q + k * k) / a0};
    }
    {
        const double fc = 38.13547087602444, q = 0.5003270373238773;
        const double k = std::tan(std::numbers::pi * fc / rate_), a0 = 1.0 + k / q + k * k;
        hp_ = {1.0, -2.0, 1.0, 2.0 * (k * k - 1.0) / a0, (1.0 - k / q + k * k) / a0};
    }
    z_.assign(static_cast<size_t>(ch_) * 4, 0.0);
    hist_.assign(static_cast<size_t>(kHistBins) + 1, 0);
}

void LevelMeter::add(const double* x, int frames, double scale) {
    for (int i = 0; i < frames; ++i) {
        for (int c = 0; c < ch_; ++c) {
            const double v = x[static_cast<size_t>(i) * ch_ + c] * scale;
            double* z = z_.data() + static_cast<size_t>(c) * 4;   // transposed direct form II, per stage
            const double s = shelf_.b0 * v + z[0];
            z[0] = shelf_.b1 * v - shelf_.a1 * s + z[1];
            z[1] = shelf_.b2 * v - shelf_.a2 * s;
            const double y = hp_.b0 * s + z[2];
            z[2] = hp_.b1 * s - hp_.a1 * y + z[3];
            z[3] = hp_.b2 * s - hp_.a2 * y;
            acc_ += y * y;
            const double a = std::abs(v);
            int bin = 0;
            if (a > 0.0) bin = std::clamp(static_cast<int>((20.0 * std::log10(a) - kHistLowDb) / kHistStepDb), 0, kHistBins);
            ++hist_[static_cast<size_t>(bin)];
        }
        if (++in_step_ == step_) {
            steps_.push_back(acc_ / step_);
            acc_ = 0.0;
            in_step_ = 0;
        }
    }
    samples_ += static_cast<uint64_t>(frames) * static_cast<uint64_t>(ch_);
}

TrackLevels LevelMeter::result() const {
    TrackLevels r;
    // the 400 ms blocks (four steps), gated
    std::vector<double> z;
    for (size_t j = 0; j + 4 <= steps_.size(); ++j) z.push_back((steps_[j] + steps_[j + 1] + steps_[j + 2] + steps_[j + 3]) / 4.0);
    auto loud = [](double ms) { return ms > 0.0 ? -0.691 + 10.0 * std::log10(ms) : -HUGE_VAL; };
    double sum = 0.0;
    int n = 0;
    for (double b : z)
        if (loud(b) > -70.0) sum += b, ++n;
    if (n == 0) {
        r.lufs = -HUGE_VAL;
    } else {
        const double rel = loud(sum / n) - 10.0;
        double s2 = 0.0;
        int n2 = 0;
        for (double b : z)
            if (loud(b) > -70.0 && loud(b) > rel) s2 += b, ++n2;
        r.lufs = loud(s2 / n2);
    }
    // the 99.9th percentile: the smallest level that 99.9 % of the samples are at or below
    if (samples_ == 0) {
        r.p999_db = -HUGE_VAL;
    } else {
        const uint64_t above = samples_ / 1000;   // samples allowed above it
        uint64_t seen = 0;
        int bin = kHistBins;
        for (; bin > 0; --bin) {
            if (seen + hist_[static_cast<size_t>(bin)] > above) break;
            seen += hist_[static_cast<size_t>(bin)];
        }
        r.p999_db = bin == 0 ? -HUGE_VAL : kHistLowDb + (bin + 1) * kHistStepDb;   // the bin's upper edge
    }
    return r;
}

std::optional<TrackLevels> scan_levels(Decoder& d, const std::atomic<bool>& cancel, double scale) {
    const AudioInfo& in = d.info();
    if (in.rate <= 0 || in.channels <= 0) return std::nullopt;
    LevelMeter m(in.rate, in.channels);
    std::vector<double> buf(static_cast<size_t>(8192) * static_cast<size_t>(in.channels));
    uint64_t total = 0;
    for (;;) {
        if (cancel.load(std::memory_order_relaxed)) return std::nullopt;
        const int got = d.read(buf.data(), 8192);
        if (got <= 0) break;
        m.add(buf.data(), got, scale);
        total += static_cast<uint64_t>(got);
    }
    if (total == 0) return std::nullopt;
    return m.result();
}

std::optional<TrackLevels> cached_levels(const std::filesystem::path& p) {
    const std::string k = cache_key(p);
    std::lock_guard<std::mutex> l(g_cache_m);
    const auto it = g_cache.find(k);
    if (it == g_cache.end()) return std::nullopt;
    return it->second;
}

void store_levels(const std::filesystem::path& p, const TrackLevels& lv) {
    const std::string k = cache_key(p);
    std::lock_guard<std::mutex> l(g_cache_m);
    g_cache[k] = lv;
}

void clear_level_cache() {
    std::lock_guard<std::mutex> l(g_cache_m);
    g_cache.clear();
}

}  // namespace daphne
