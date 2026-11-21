#include "daphne/color/linear.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace daphne::color {

double response_mix(double drive) {
    if (drive <= 0.0) return 0.0;
    const double u = std::min(1.0, drive / kResponseRampEnd);
    return u * u * (3.0 - 2.0 * u);
}

void SectionFilter::prepare(const Section& s, int rate) {
    s_ = s;
    rate_ = rate;
    k_ = 1.0 / s.q;
    // the bilinear transform reaches the corner only below Nyquist (with a margin): above it the section waits for the
    // local oversampling (M1 step 8), which runs the stage at 8x the family rate
    active_ = rate > 0 && s.fc_hz < 0.45 * rate;
    if (!active_) return;
    g_ref_ = std::tan(std::numbers::pi * s.fc_hz / rate);   // prewarped: the reference corner is exact
    set_mix(1.0);
}

void SectionFilter::set_mix(double m) {
    if (!active_) return;
    const bool first = s_.kind == Section::Kind::HighPass1 || s_.kind == Section::Kind::LowPass1 || s_.kind == Section::Kind::General1;
    if (s_.general()) {   // always the whole section: the stage fades the load as a whole
        g_ = g_ref_;
        n2_ = s_.c2, n1_ = s_.c1, n0_ = s_.c0;
    } else {
        const double c = std::sqrt(std::clamp(m, 1e-24, 1.0));
        g_ = s_.high() ? g_ref_ / c : g_ref_ * c;
    }
    a_ = first ? g_ / (1.0 + g_) : 1.0 / (1.0 + g_ * (g_ + k_));
}

double SectionFilter::process(double x, double* st) const {
    switch (s_.kind) {
        case Section::Kind::HighPass1:
        case Section::Kind::LowPass1: {
            const double v = (x - st[0]) * a_, lp = v + st[0];
            st[0] = lp + v;
            return s_.kind == Section::Kind::LowPass1 ? lp : x - lp;
        }
        case Section::Kind::HighPass2:
        case Section::Kind::LowPass2: {
            const double hp = (x - (g_ + k_) * st[0] - st[1]) * a_;
            const double bp = g_ * hp + st[0];
            st[0] = g_ * hp + bp;
            const double lp = g_ * bp + st[1];
            st[1] = g_ * bp + lp;
            return s_.kind == Section::Kind::LowPass2 ? lp : hp;
        }
        case Section::Kind::General1: {
            const double v = (x - st[0]) * a_, lp = v + st[0];
            st[0] = lp + v;
            return n1_ * (x - lp) + n0_ * lp;
        }
        case Section::Kind::General2: {
            const double hp = (x - (g_ + k_) * st[0] - st[1]) * a_;
            const double bp = g_ * hp + st[0];
            st[0] = g_ * hp + bp;
            const double lp = g_ * bp + st[1];
            st[1] = g_ * bp + lp;
            return n2_ * hp + n1_ * bp + n0_ * lp;
        }
    }
    return x;
}

std::complex<double> SectionFilter::response(double hz) const {
    if (!active_) return 1.0;
    const std::complex<double> s(0.0, std::tan(std::numbers::pi * hz / rate_) / g_);
    switch (s_.kind) {
        case Section::Kind::HighPass1: return s / (s + 1.0);
        case Section::Kind::LowPass1: return 1.0 / (s + 1.0);
        case Section::Kind::HighPass2: return s * s / (s * s + k_ * s + 1.0);
        case Section::Kind::LowPass2: return 1.0 / (s * s + k_ * s + 1.0);
        case Section::Kind::General1: return (n1_ * s + n0_) / (s + 1.0);
        case Section::Kind::General2: return (n2_ * s * s + n1_ * s + n0_) / (s * s + k_ * s + 1.0);
    }
    return 1.0;
}

std::complex<double> SectionChain::response(double hz) const {
    std::complex<double> h = 1.0;
    for (const auto& f : f_) h *= f.response(hz);
    return h;
}

void SectionChain::prepare(const std::vector<Section>& v, int rate, int channels) {
    f_.clear();
    for (const auto& s : v) {
        SectionFilter f;
        f.prepare(s, rate);
        if (f.active()) f_.push_back(f);
    }
    st_.assign(static_cast<size_t>(std::max(0, channels)) * f_.size() * 2, 0.0);
}

void SectionChain::set_mix(double m) {
    for (auto& f : f_) f.set_mix(m);
}

double SectionChain::process(double x, int ch) {
    double* st = st_.data() + static_cast<size_t>(ch) * f_.size() * 2;
    for (const auto& f : f_) {
        x = f.process(x, st);
        st += 2;
    }
    return x;
}

void SectionChain::reset() { std::fill(st_.begin(), st_.end(), 0.0); }

}  // namespace daphne::color
