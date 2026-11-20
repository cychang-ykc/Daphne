#include "daphne/color/oversampler.h"

#include <algorithm>

#include "daphne/dsp/filter_design.h"

namespace daphne::color {

int Oversampler::factor(int rate) {
    if (rate <= 0) return 1;
    const int family = rate % 44100 == 0 ? 44100 : 48000;
    const long target = static_cast<long>(kFamilyMultiple) * family;
    if (rate >= target) return 1;
    return static_cast<int>((target + rate - 1) / rate);
}

void Oversampler::prepare(int rate, int factor, int channels, int max_frames) {
    (void)max_frames;
    l_ = std::max(1, factor);
    ch_ = channels;
    if (l_ == 1) return;
    h_ = dsp::design_lowpass(kTapsPerPhase, l_, rate, static_cast<double>(rate) * l_, 0.0, false);
    n_ = static_cast<int>(h_.size());
    const int t = kTapsPerPhase;
    // up: y[nL + p] = L sum_k h[kL + p] x[n - k]
    hup_.assign(static_cast<size_t>(n_), 0.0);
    for (int p = 0; p < l_; ++p)
        for (int k = 0; k < t; ++k) hup_[static_cast<size_t>(p * t + k)] = l_ * h_[static_cast<size_t>(k * l_ + p)];
    // down: y[n] = sum_j h[j] u[nL + L - 1 - j]: stored reversed against the ring's oldest-first order
    hdown_.assign(static_cast<size_t>(n_), 0.0);
    for (int j = 0; j < n_; ++j) hdown_[static_cast<size_t>(n_ - 1 - j)] = h_[static_cast<size_t>(j)];
    xin_.assign(static_cast<size_t>(2 * t * ch_), 0.0);
    xhi_.assign(static_cast<size_t>(2 * n_ * ch_), 0.0);
    pin_ = phi_ = 0;
}

void Oversampler::reset() {
    std::fill(xin_.begin(), xin_.end(), 0.0);
    std::fill(xhi_.begin(), xhi_.end(), 0.0);
    pin_ = phi_ = 0;
}

void Oversampler::up(const double* x, double* hi) {
    const int t = kTapsPerPhase;
    // the input ring, doubled so that the last t samples are contiguous: newest at pin_ + t - 1
    for (int c = 0; c < ch_; ++c) {
        double* r = xin_.data() + static_cast<size_t>(c) * 2 * t;
        r[pin_] = r[pin_ + t] = x[c];
    }
    pin_ = (pin_ + 1) % t;
    for (int c = 0; c < ch_; ++c) {
        const double* r = xin_.data() + static_cast<size_t>(c) * 2 * t + pin_;   // oldest .. newest
        for (int p = 0; p < l_; ++p) {
            const double* hp = hup_.data() + static_cast<size_t>(p * t);
            double s = 0.0;
            for (int k = 0; k < t; ++k) s += hp[k] * r[t - 1 - k];
            hi[static_cast<size_t>(p * ch_ + c)] = s;
        }
    }
}

void Oversampler::down(const double* hi, double* y) {
    const int m = n_;
    for (int p = 0; p < l_; ++p) {
        for (int c = 0; c < ch_; ++c) {
            double* r = xhi_.data() + static_cast<size_t>(c) * 2 * m;
            r[phi_] = r[phi_ + m] = hi[static_cast<size_t>(p * ch_ + c)];
        }
        phi_ = (phi_ + 1) % m;
    }
    for (int c = 0; c < ch_; ++c) {
        const double* r = xhi_.data() + static_cast<size_t>(c) * 2 * m + phi_;   // oldest .. newest
        double s = 0.0;
        for (int j = 0; j < m; ++j) s += hdown_[static_cast<size_t>(j)] * r[j];
        y[c] = s;
    }
}

}  // namespace daphne::color
