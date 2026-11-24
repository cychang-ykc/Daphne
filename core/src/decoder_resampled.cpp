// A decoder that resamples another one to a rate the device has (make_resampling_decoder).
//
// The player's last resort for an output that neither offers the track's rate nor converts by itself (ASIO: a
// 192 kHz track on a DAC that stops at 96 kHz, 44.1 kHz on a 48 kHz-only one). The chain only upsamples by whole
// factors, so the track is brought to a rate the device takes before the chain: to the rest of the player it is a
// source at that rate. Never used while the device can lock the track's own rate.
//
// A polyphase windowed-sinc filter (Kaiser, beta 12: stop band below -110 dB), 32 zero crossings each side at the
// lower of the two rates, pass band to 0.94 of the lower Nyquist. Every phase is normalized to unity DC gain.
#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

#include "daphne/engine/decoder.h"

namespace daphne {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kZeros = 32;        // zero crossings each side
constexpr double kBeta = 12.0;
constexpr double kPass = 0.94;    // of the lower Nyquist
constexpr int kRead = 4096;       // frames taken from the source at a time

double bessel_i0(double x) {
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 64; ++k) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < 1e-17 * sum) break;
    }
    return sum;
}

class Resampled final : public ResamplingDecoder {
public:
    Resampled(std::unique_ptr<Decoder> inner, int rate) : in_(std::move(inner)) {
        const AudioInfo& si = in_->info();
        src_rate_ = si.rate;
        info_ = si;
        info_.rate = rate;
        info_.resampled_from = si.rate;
        info_.lossless_int = false;   // the samples are no longer the file's
        info_.bits = 0;
        const int g = std::gcd(rate, si.rate);
        up_ = rate / g;
        down_ = si.rate / g;
        info_.frames = si.frames >= 0 ? out_frames(si.frames) : -1;
        ch_ = si.channels;
        // the filter: cut at the lower Nyquist; x in source samples
        const double base = std::min(1.0, static_cast<double>(up_) / down_);
        const double fc = kPass * base;
        half_ = static_cast<int>(std::ceil(kZeros / base));
        taps_ = 2 * half_;
        table_.assign(static_cast<size_t>(up_) * taps_, 0.0);
        const double i0b = bessel_i0(kBeta);
        for (int p = 0; p < up_; ++p) {
            const double frac = static_cast<double>(p) / up_;
            double* row = &table_[static_cast<size_t>(p) * taps_];
            double sum = 0.0;
            for (int k = 0; k < taps_; ++k) {
                const double x = (k - half_ + 1) - frac;          // the tap's distance from the output instant
                const double w = x / half_;
                if (std::abs(w) >= 1.0) continue;
                const double a = kPi * fc * x;
                const double sinc = std::abs(a) < 1e-12 ? 1.0 : std::sin(a) / a;
                row[k] = fc * sinc * bessel_i0(kBeta * std::sqrt(1.0 - w * w)) / i0b;
                sum += row[k];
            }
            if (sum != 0.0)
                for (int k = 0; k < taps_; ++k) row[k] /= sum;
        }
        chunk_.assign(static_cast<size_t>(kRead) * ch_, 0.0);
        restart(0);
    }

    bool open(const std::filesystem::path&, std::string*) override { return true; }   // the source is open already
    void close() override { in_->close(); }
    int source_rate() const override { return src_rate_; }
    std::unique_ptr<Decoder> release() override { return std::move(in_); }

    int read(double* out, int frames) override {
        int done = 0;
        for (; done < frames; ++done) {
            if (info_.frames >= 0 && pos_ >= info_.frames) break;
            const int64_t at = pos_ * down_;
            const int64_t c = at / up_;                 // the source frame at or before the output instant
            const int p = static_cast<int>(at % up_);
            if (!fill(c + half_)) {
                if (c >= src_end_) break;               // past the last source frame: the end
            }
            const double* row = &table_[static_cast<size_t>(p) * taps_];
            const int64_t first = c - half_ + 1;
            double* o = out + static_cast<size_t>(done) * ch_;
            for (int q = 0; q < ch_; ++q) o[q] = 0.0;
            for (int k = 0; k < taps_; ++k) {
                const int64_t i = first + k;
                if (i < base_ || i >= base_ + have_) continue;   // before the start / after the end: silence
                const double* f = &buf_[static_cast<size_t>(i - base_) * ch_];
                const double h = row[k];
                for (int q = 0; q < ch_; ++q) o[q] += h * f[q];
            }
            ++pos_;
            // frames no output needs any more
            const int64_t keep = first;
            if (keep - base_ > 4 * kRead) drop(keep - base_);
        }
        return done;
    }

    bool seek(int64_t frame) override {
        if (frame < 0) frame = 0;
        if (info_.frames >= 0 && frame > info_.frames) frame = info_.frames;
        const int64_t c = frame * down_ / up_;
        const int64_t from = std::max<int64_t>(0, c - half_ + 1);
        if (!in_->seek(from)) return false;
        restart(from);
        pos_ = frame;
        return true;
    }

private:
    int64_t out_frames(int64_t in) const { return (in * up_ + down_ - 1) / down_; }

    void restart(int64_t from) {
        base_ = from;
        have_ = 0;
        buf_.clear();
        eof_ = false;
        src_end_ = INT64_MAX;
        pos_ = 0;
    }

    void drop(int64_t n) {
        n = std::min(n, have_);
        if (n <= 0) return;
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(n) * ch_);
        base_ += n;
        have_ -= n;
    }

    // Source frames up to `last` (inclusive) in the buffer; false when the source ended before it.
    bool fill(int64_t last) {
        while (!eof_ && base_ + have_ <= last) {
            const int got = in_->read(chunk_.data(), kRead);
            if (got <= 0) {
                eof_ = true;
                src_end_ = base_ + have_;
                break;
            }
            buf_.insert(buf_.end(), chunk_.begin(), chunk_.begin() + static_cast<std::ptrdiff_t>(got) * ch_);
            have_ += got;
        }
        return base_ + have_ > last;
    }

    std::unique_ptr<Decoder> in_;
    int src_rate_ = 0, up_ = 1, down_ = 1, ch_ = 0, half_ = 0, taps_ = 0;
    std::vector<double> table_, buf_, chunk_;
    int64_t base_ = 0, have_ = 0;   // buf_ holds source frames [base_, base_ + have_)
    int64_t src_end_ = INT64_MAX;   // the source's length, once it ended
    bool eof_ = false;
};

}  // namespace

std::unique_ptr<ResamplingDecoder> make_resampling_decoder(std::unique_ptr<Decoder> inner, int rate) {
    if (!inner || rate <= 0 || inner->info().rate <= 0 || inner->info().channels <= 0 || inner->info().rate == rate) return nullptr;
    return std::make_unique<Resampled>(std::move(inner), rate);
}

std::vector<int> fallback_rates(int rate) {
    std::vector<int> r;
    auto add = [&](int v) {
        if (v > 0 && v != rate && std::find(r.begin(), r.end(), v) == r.end()) r.push_back(v);
    };
    // the same family first, halved down to its base (192 -> 96 -> 48 kHz): a whole ratio
    for (int base : {44100, 48000})
        if (rate % base == 0)
            for (int v = rate / 2; v >= base; v /= 2) add(v);
    // then the standard rates at or below it, the highest first, and the two base rates whatever the track's
    for (int v : {192000, 176400, 96000, 88200, 48000, 44100})
        if (v <= rate) add(v);
    add(48000);
    add(44100);
    return r;
}

}  // namespace daphne
