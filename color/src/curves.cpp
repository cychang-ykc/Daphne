#include "daphne/color/curves.h"

namespace daphne::color {

namespace {
#include "preset_curves.inc"
}  // namespace

const PresetCurve* preset_curve(std::string_view id) {
    for (const PresetCurve* c : k_preset_curves)
        if (c && id == c->id) return c;
    return nullptr;
}

}  // namespace daphne::color
