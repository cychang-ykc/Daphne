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
    dc_r_ = in.rate > 0 ? std::exp(-2.0 * std::numbers::pi * kDcBlockHz / in.rate) : 0.0;
    dc_x1_.assign(static_cast<size_t>(std::max(0, in.channels)), 0.0);
    dc_y1_.assign(dc_x1_.size(), 0.0);
    glide_frames_ = std::max(1, static_cast<int>(std::lround(kGlideMs * 1e-3 * in.rate)));
    pre_.prepare(p_.linear.pre, in.rate, in.channels);
    post_.prepare(p_.linear.post, in.rate, in.channels);
    mix_d_ = -1.0;
    update_mix();
}

void TubeColorStage::reset() {
    std::fill(dc_x1_.begin(), dc_x1_.end(), 0.0);
    std::fill(dc_y1_.begin(), dc_y1_.end(), 0.0);
    pre_.reset();
    post_.reset();
    // a reset (seek, new track) lands on the target
    d_ = last_target_ = target_.load();
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
#if defined(DAPHNE_COLOR_TEST_CURVE)
    const TestCurve f;
#endif
    for (int i = 0; i < frames; ++i) {
        if (left_ > 0) {
            d_ = --left_ == 0 ? last_target_ : d_ + step_;
            if (left_ == 0 || i % kControl == 0) update_mix();
        }
        const double d = d_;
        for (int c = 0; c < ch; ++c) {
            const auto k = static_cast<size_t>(i) * static_cast<size_t>(ch) + static_cast<size_t>(c);
            const double x = pre_.process(in[k], c);
#if defined(DAPHNE_COLOR_TEST_CURVE)
            const double r = f(d * x) / d - x;                                 // the distortion the curve adds
#else
            const double r = 0.0 * d;                                          // (no curve before M2 outside test builds)
#endif
            const double y = r - dc_x1_[static_cast<size_t>(c)] + dc_r_ * dc_y1_[static_cast<size_t>(c)];   // without its DC
            dc_x1_[static_cast<size_t>(c)] = r;
            dc_y1_[static_cast<size_t>(c)] = y;
            out[k] = post_.process(x + y, c);
        }
    }
    return frames;
}

}  // namespace daphne::color
