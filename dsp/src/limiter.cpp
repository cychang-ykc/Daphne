#include "daphne/dsp/limiter.h"

#include <algorithm>
#include <cmath>

namespace daphne::dsp {

void Limiter::prepare(const Format& in) {
    fmt_ = in;
    w_ = std::max(1, static_cast<int>(std::lround(lookahead_ms_ * 1e-3 * in.rate)));
    rel_ = 1.0 - std::exp(-1.0 / (release_ms_ * 1e-3 * in.rate));
    pass_step_ = 1.0 / std::max(1.0, kPassReleaseMs * 1e-3 * in.rate);
    delay_.assign(static_cast<size_t>(w_) * in.channels, 0.0);
    avg_.assign(static_cast<size_t>(w_), 1.0);
    min_val_.assign(static_cast<size_t>(w_) + 2, 1.0);
    min_idx_.assign(static_cast<size_t>(w_) + 2, 0);
    zeros_.assign(static_cast<size_t>(w_) * in.channels, 0.0);
    reset();
}

void Limiter::reset() {
    std::fill(delay_.begin(), delay_.end(), 0.0);
    std::fill(avg_.begin(), avg_.end(), 1.0);
    qhead_ = qsize_ = 0;
    pos_ = 0;
    n_ = 0;
    sum_ = static_cast<double>(w_);
    env_ = 1.0;
    limited_ = 0;
}

double Limiter::request(double peak) const {
    if (knee_db_ <= 0.0) return peak > ceiling_ ? ceiling_ / peak : 1.0;
    if (peak <= 0.0) return 1.0;
    // in dB: y = x below T - W/2; y = x - (x - T + W/2)^2 / (2 W) in the knee; y = T above (T the ceiling, W the knee)
    const double t = 20.0 * std::log10(ceiling_), w = knee_db_;
    const double x = 20.0 * std::log10(peak);
    if (x <= t - 0.5 * w) return 1.0;
    if (x >= t + 0.5 * w) return ceiling_ / peak;
    const double d = x - t + 0.5 * w;
    return std::min(1.0, std::pow(10.0, -d * d / (2.0 * w) / 20.0));
}

int Limiter::process(const double* in, int frames, double* out) {
    const int ch = fmt_.channels;
    const int cap = static_cast<int>(min_val_.size());
    for (int i = 0; i < frames; ++i, ++n_) {
        const double* x = in + static_cast<size_t>(i) * ch;
        double req = 1.0;
        if (!pass_) {
            double peak = 0.0;
            for (int c = 0; c < ch; ++c) peak = std::max(peak, std::abs(x[c]));
            req = request(peak);
        }
        // sliding minimum of the requests over the last w_ + 1 frames (monotonic queue)
        while (qsize_ > 0 && min_val_[static_cast<size_t>((qhead_ + qsize_ - 1) % cap)] >= req) --qsize_;
        const int tail = (qhead_ + qsize_) % cap;
        min_val_[static_cast<size_t>(tail)] = req;
        min_idx_[static_cast<size_t>(tail)] = n_;
        ++qsize_;
        while (min_idx_[static_cast<size_t>(qhead_)] < n_ - w_) {
            qhead_ = (qhead_ + 1) % cap;
            --qsize_;
        }
        const double hold = min_val_[static_cast<size_t>(qhead_)];
        // instant attack to the held request, one-pole release towards 1 (in pass-through: a straight line to exactly 1)
        env_ = pass_ ? std::min(1.0, env_ + pass_step_) : std::min(hold, env_ + (1.0 - env_) * rel_);
        if (env_ > 1.0 - 1e-9 && hold == 1.0) env_ = 1.0;   // the release ends exactly at 1 (bit for bit again)
        // box average over w_ frames: a smooth ramp that reaches the request with the delayed peak
        sum_ += env_ - avg_[static_cast<size_t>(pos_)];
        avg_[static_cast<size_t>(pos_)] = env_;
        const double g = std::min(1.0, sum_ / w_);
        double* d = delay_.data() + static_cast<size_t>(pos_) * ch;
        double* y = out + static_cast<size_t>(i) * ch;
        if (g < 1.0) ++limited_;
        for (int c = 0; c < ch; ++c) {
            const double delayed = d[c];
            d[c] = x[c];
            y[c] = g < 1.0 ? delayed * g : delayed;
        }
        if (++pos_ == w_) {
            pos_ = 0;
            double s = 0.0;   // re-sum once per window: no drift in the running sum
            for (double v : avg_) s += v;
            sum_ = s;
        }
    }
    return frames;
}

int Limiter::flush(double* out) {
    if (aligned_) return 0;
    const int n = std::min(w_, fmt_.max_frames > 0 ? fmt_.max_frames : w_);
    return process(zeros_.data(), n, out);
}

}  // namespace daphne::dsp
