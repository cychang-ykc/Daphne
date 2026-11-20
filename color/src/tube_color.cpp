#include "daphne/color/tube_color.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>

#include "daphne/color/stage.h"
#include "daphne/color/test_curve.h"

namespace daphne::color {

void TubeColorStage::prepare(const dsp::Format& in) {
    fmt_ = in;
    dc_r_ = in.rate > 0 ? std::exp(-2.0 * std::numbers::pi * kDcBlockHz / in.rate) : 0.0;
    dc_x1_.assign(static_cast<size_t>(std::max(0, in.channels)), 0.0);
    dc_y1_.assign(dc_x1_.size(), 0.0);
}

void TubeColorStage::reset() {
    std::fill(dc_x1_.begin(), dc_x1_.end(), 0.0);
    std::fill(dc_y1_.begin(), dc_y1_.end(), 0.0);
}

int TubeColorStage::process(const double* in, int frames, double* out) {
    const int ch = fmt_.channels;
    const auto n = static_cast<std::ptrdiff_t>(frames) * ch;
#if defined(DAPHNE_COLOR_TEST_CURVE)
    const double d = p_.drive;
    if (d > 0.0) {
        const TestCurve f;
        for (std::ptrdiff_t i = 0; i < n; ++i) {
            const auto c = static_cast<size_t>(i % ch);
            const double x = in[i];
            const double r = f(d * x) / d - x;                       // the distortion the curve adds
            const double y = r - dc_x1_[c] + dc_r_ * dc_y1_[c];      // without its DC
            dc_x1_[c] = r;
            dc_y1_[c] = y;
            out[i] = x + y;
        }
        return frames;
    }
#endif
    if (in != out) std::copy(in, in + n, out);
    return frames;
}

}  // namespace daphne::color
