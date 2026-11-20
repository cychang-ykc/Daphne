#include "daphne/color/tube_color.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>

#include "daphne/color/stage.h"
#include "daphne/color/test_curve.h"

namespace daphne::color {

TubeColorStage::TubeColorStage(StageParams p) : p_(std::move(p)), identity_(p_.drive <= 0.0), target_(p_.drive) {
    last_target_ = d_ = p_.drive;
}

void TubeColorStage::prepare(const dsp::Format& in) {
    fmt_ = in;
    const int l = identity_ ? 1 : Oversampler::factor(in.rate);   // drive 0: nothing runs, not even the oversampling
    if (l > 1) os_.prepare(in.rate, l, in.channels, in.max_frames);
    else os_ = Oversampler{};
    hi_.assign(static_cast<size_t>(l) * static_cast<size_t>(std::max(0, in.channels)), 0.0);
    const int rate = in.rate * l;   // where the curve and the sections run
    dc_r_ = rate > 0 ? std::exp(-2.0 * std::numbers::pi * kDcBlockHz / rate) : 0.0;
    dc_x1_.assign(static_cast<size_t>(std::max(0, in.channels)), 0.0);
    dc_y1_.assign(dc_x1_.size(), 0.0);
    glide_frames_ = std::max(1, static_cast<int>(std::lround(kGlideMs * 1e-3 * rate)));
    pre_.prepare(p_.linear.pre, rate, in.channels);
    post_.prepare(p_.linear.post, rate, in.channels);
    mix_d_ = -1.0;
    update_mix();
}

void TubeColorStage::run_frame(const double* x, double* y) {
    if (left_ > 0) {   // the drive glides (the fade's corners follow every kControl frames)
        d_ = --left_ == 0 ? last_target_ : d_ + step_;
        if (left_ == 0 || left_ % kControl == 0) update_mix();
    }
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

void TubeColorStage::reset() {
    std::fill(dc_x1_.begin(), dc_x1_.end(), 0.0);
    std::fill(dc_y1_.begin(), dc_y1_.end(), 0.0);
    pre_.reset();
    post_.reset();
    os_.reset();
    // a reset (seek, new track) lands on the target
    d_ = last_target_ = target_.load();
    left_ = 0;
    update_mix();
}

void TubeColorStage::hold_at_rise() {
    if (identity_) return;
    target_.store(kRiseFrom);
    d_ = last_target_ = kRiseFrom;
    left_ = 0;
    update_mix();
}

void TubeColorStage::set_drive(double d) {
    if (identity_ || d <= 0.0) return;
    target_.store(d);
}

void TubeColorStage::update_mix() {
    if (d_ == mix_d_) return;
    mix_d_ = d_;
    const double m = response_mix(d_);
    pre_.set_mix(m);
    post_.set_mix(m);
}

int TubeColorStage::process(const double* in, int frames, double* out) {
    const int ch = fmt_.channels;
    if (identity_) {
        if (in != out) std::copy(in, in + static_cast<std::ptrdiff_t>(frames) * ch, out);
        return frames;
    }
    if (const double t = target_.load(); t != last_target_) {   // a new drive: glide there from where it is
        last_target_ = t;
        left_ = glide_frames_;
        step_ = (t - d_) / glide_frames_;
    }
    const int l = os_.factor();
    for (int i = 0; i < frames; ++i) {
        const double* x = in + static_cast<std::ptrdiff_t>(i) * ch;
        double* y = out + static_cast<std::ptrdiff_t>(i) * ch;
        if (l == 1) {
            run_frame(x, y);
            continue;
        }
        os_.up(x, hi_.data());
        for (int p = 0; p < l; ++p) run_frame(hi_.data() + static_cast<size_t>(p) * static_cast<size_t>(ch), hi_.data() + static_cast<size_t>(p) * static_cast<size_t>(ch));
        os_.down(hi_.data(), y);
    }
    return frames;
}

}  // namespace daphne::color
