#include "daphne/color/tube_color.h"

#include <algorithm>
#include <cstddef>

#include "daphne/color/stage.h"

namespace daphne::color {

int TubeColorStage::process(const double* in, int frames, double* out) {
    if (in != out) std::copy(in, in + static_cast<std::ptrdiff_t>(frames) * fmt_.channels, out);
    return frames;
}

}  // namespace daphne::color
