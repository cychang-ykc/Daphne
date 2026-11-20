#include "daphne/color/tube_color.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>

#include "daphne/color/stage.h"
#include "daphne/color/test_curve.h"

namespace daphne::color {

TubeColorStage::TubeColorStage(StageParams p) : p_(std::move(p)), target_(std::max(0.0, p_.drive)) {
    // built at a drive above 0: running from the start; at 0: idle
    d_ = last_target_ = target_.load();
    w_ = d_ > 0.0 ? 1.0 : 0.0;
}

void TubeColorStage::prepare(const dsp::Format& in) {
    fmt_ = in;
    const int ch = std::max(0, in.channels);
    const auto chs = static_cast<size_t>(ch);
    const int l = Oversampler::factor(in.rate);
    if (l > 1) os_.prepare(in.rate, l, ch, in.max_frames);
    else os_ = Oversampler{};
    delay_ = os_.delay();
    hi_.assign(static_cast<size_t>(l) * chs, 0.0);
    xin_.assign(chs, 0.0), dly_.assign(chs, 0.0), path_.assign(chs, 0.0), prime_y_.assign(chs, 0.0);
    hist_frames_ = kHistory + delay_ + 1;
    hist_.assign(static_cast<size_t>(hist_frames_) * chs, 0.0);
    hist_at_ = fed_ = 0;
    const int rate = in.rate * l;   // where the curve and the sections run
    dc_r_ = rate > 0 ? std::exp(-2.0 * std::numbers::pi * kDcBlockHz / rate) : 0.0;
    dc_x1_.assign(chs, 0.0);
    dc_y1_.assign(chs, 0.0);
    glide_frames_ = std::max(1, static_cast<int>(std::lround(kGlideMs * 1e-3 * in.rate)));
    w_step_ = 1.0 / std::max(1.0, std::round(kSwitchMs * 1e-3 * in.rate));
    pre_.prepare(p_.linear.pre, rate, ch);
    post_.prepare(p_.linear.post, rate, ch);
    mix_d_ = -1.0;
    update_mix();
    primed_ = w_ > 0.0;   // running from the start: its state builds up with the music
}

void TubeColorStage::reset() {
    std::fill(dc_x1_.begin(), dc_x1_.end(), 0.0);
    std::fill(dc_y1_.begin(), dc_y1_.end(), 0.0);
    std::fill(hist_.begin(), hist_.end(), 0.0);
    hist_at_ = fed_ = 0;
    pre_.reset();
    post_.reset();
    os_.reset();
    // a reset (seek, new track) lands where the drive is going
    const double t = target_.load();
    d_ = last_target_ = t;
    left_ = 0;
    w_ = t > 0.0 ? 1.0 : 0.0;
    primed_ = w_ > 0.0;
    update_mix();
}

void TubeColorStage::set_drive(double d) { target_.store(std::max(0.0, d)); }

void TubeColorStage::update_mix() {
    if (d_ == mix_d_ || d_ <= 0.0) return;
    mix_d_ = d_;
    const double m = response_mix(d_);
    pre_.set_mix(m);
    post_.set_mix(m);
}

void TubeColorStage::glide_to(double v) {
    last_target_ = v;
    left_ = glide_frames_;
    step_ = (v - d_) / glide_frames_;
}

// The processing path picks up where a path that had run all along would be: the oversampler's two filters are filled
// from the history (up to kHistory frames, the newest last); the curve, sections and DC blocker start from rest at next
// to no drive.
void TubeColorStage::prime() {
    const auto ch = static_cast<size_t>(fmt_.channels);
    std::fill(dc_x1_.begin(), dc_x1_.end(), 0.0);
    std::fill(dc_y1_.begin(), dc_y1_.end(), 0.0);
    pre_.reset();
    post_.reset();
    d_ = last_target_ = kRiseFrom;
    left_ = 0;
    update_mix();
    if (os_.factor() > 1) {
        os_.reset();
        const int n = std::min(fed_, kHistory);
        for (int k = n; k >= 1; --k) {   // the frame k before the one about to come
            const int at = ((hist_at_ - k) % hist_frames_ + hist_frames_) % hist_frames_;
            os_.up(hist_.data() + static_cast<size_t>(at) * ch, hi_.data());
            os_.down(hi_.data(), prime_y_.data());
        }
    }
    primed_ = true;
}

void TubeColorStage::run_frame(const double* x, double* y) {
    const double d = d_;
#if defined(DAPHNE_COLOR_TEST_CURVE)
    const TestCurve f;
#endif
    for (int c = 0; c < fmt_.channels; ++c) {
        const auto k = static_cast<size_t>(c);
        const double v = pre_.process(x[c], c);
#if defined(DAPHNE_COLOR_TEST_CURVE)
        const double r = f(d * v) / d - v;                       // the distortion the curve adds
#else
        const double r = 0.0 * d;                                // (no curve before M2 outside test builds)
#endif
        const double w = r - dc_x1_[k] + dc_r_ * dc_y1_[k];      // without its DC
        dc_x1_[k] = r;
        dc_y1_[k] = w;
        y[c] = post_.process(v + w, c);
    }
}

void TubeColorStage::process_path(const double* x, double* y) {
    const int l = os_.factor();
    if (l == 1) {
        run_frame(x, y);
        return;
    }
    os_.up(x, hi_.data());
    for (int p = 0; p < l; ++p) {
        double* h = hi_.data() + static_cast<size_t>(p) * static_cast<size_t>(fmt_.channels);
        run_frame(h, h);
    }
    os_.down(hi_.data(), y);
}

int TubeColorStage::process(const double* in, int frames, double* out) {
    const int ch = fmt_.channels;
    const auto chs = static_cast<size_t>(ch);
    const double t = target_.load();
    const bool active = t > 0.0;
    bool untouched = true;
    for (int i = 0; i < frames; ++i) {
        const double* x = in + static_cast<std::ptrdiff_t>(i) * ch;
        double* y = out + static_cast<std::ptrdiff_t>(i) * ch;   // (may be x: everything of x is read first)
        // the input, and the delay path's frame: the input delay_ frames ago (the input itself without oversampling)
        double* slot = hist_.data() + static_cast<size_t>(hist_at_) * chs;
        const int dat = ((hist_at_ - delay_) % hist_frames_ + hist_frames_) % hist_frames_;
        for (size_t c = 0; c < chs; ++c) {
            xin_[c] = x[c];
            dly_[c] = delay_ ? hist_[static_cast<size_t>(dat) * chs + c] : x[c];
        }
        // where it is going
        if (active) {
            if (w_ < 1.0) {   // leaving 0: prime, then cross over at next to no drive
                if (!primed_) prime();
                w_ = std::min(1.0, w_ + w_step_);
            } else if (last_target_ != t) {
                glide_to(t);   // then the drive (a newer one while gliding: from where it is)
            }
        } else if (w_ > 0.0) {   // reaching 0: the drive down first, then cross back
            if (last_target_ != kRiseFrom) glide_to(kRiseFrom);
            else if (left_ == 0) w_ = std::max(0.0, w_ - w_step_);
        }
        if (left_ > 0) {
            d_ = --left_ == 0 ? last_target_ : d_ + step_;
            if (left_ == 0 || left_ % kControl == 0) update_mix();
        }
        if (w_ <= 0.0) {   // idle: the delay path alone
            primed_ = false;
            if (!active) d_ = last_target_ = 0.0;
            for (size_t c = 0; c < chs; ++c) y[c] = dly_[c];
        } else {
            process_path(xin_.data(), path_.data());
            if (w_ >= 1.0) {
                for (size_t c = 0; c < chs; ++c) y[c] = path_[c];
            } else {   // the crossfade between the two paths (raised cosine), aligned: both are the input delay_ frames ago
                const double g = 0.5 - 0.5 * std::cos(std::numbers::pi * w_);
                for (size_t c = 0; c < chs; ++c) y[c] = (1.0 - g) * dly_[c] + g * path_[c];
            }
            untouched = false;
        }
        for (size_t c = 0; c < chs; ++c) slot[c] = xin_[c];
        hist_at_ = (hist_at_ + 1) % hist_frames_;
        fed_ = std::min(fed_ + 1, hist_frames_);
    }
    untouched_ = untouched;
    return frames;
}

}  // namespace daphne::color
