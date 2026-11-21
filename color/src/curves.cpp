#include "daphne/color/curves.h"

namespace daphne::color {

namespace {
#include "preset_curves.inc"
}  // namespace

const PresetLoad* preset_load(std::string_view preset, int feedback_db, std::string_view load) {
    for (const PresetLoad& l : k_preset_loads)
        if (preset == l.preset && feedback_db == l.feedback_db && load == l.load) return &l;
    return nullptr;
}

const PresetCurve* preset_curve(std::string_view id) {
    for (const PresetCurve* c : k_preset_curves)
        if (c && id == c->id) return c;
    return nullptr;
}

}  // namespace daphne::color
