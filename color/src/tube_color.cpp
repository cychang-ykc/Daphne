#include "daphne/color/tube_color.h"

// The stage (dsp::Block) arrives in the next M1 steps; this unit keeps the library non-empty meanwhile.
namespace daphne::color {
namespace {
[[maybe_unused]] constexpr bool kAvailable = available();
}  // namespace
}  // namespace daphne::color
