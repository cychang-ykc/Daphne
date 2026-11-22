// The settings parser on arbitrary bytes (doc/N28: the color.* fields and every other one). Besides not crashing:
//   - what it returns is in range: every color.* field one the stage can take (an invalid one is its default)
//   - written back and read again, the settings parse without an error and come back the same
// A broken invariant aborts (a crash for libFuzzer).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>

#include "daphne/engine/settings.h"

namespace {

void require(bool ok) {
    if (!ok) std::abort();
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::string text(reinterpret_cast<const char*>(data), size);
    const auto r = daphne::settings_from_string(text);
    const auto& c = r.settings.color;
    require(std::isfinite(c.drive) && c.drive >= 0.0 && c.drive <= daphne::kColorDriveMax);
    require(c.feedback_db == 0 || c.feedback_db == 6 || c.feedback_db == 12 || c.feedback_db == 20 || c.feedback_db == 30);
    require(c.load_ohms == 4 || c.load_ohms == 8 || c.load_ohms == 16);
    require(c.load == daphne::ColorLoad::Resistor8 || c.load == daphne::ColorLoad::BookshelfVented || c.load == daphne::ColorLoad::BookshelfSealed ||
            c.load == daphne::ColorLoad::BookshelfVentedSmooth || c.load == daphne::ColorLoad::BookshelfSealedSmooth);
    require(c.align == daphne::ColorAlign::Off || c.align == daphne::ColorAlign::Loudness || c.align == daphne::ColorAlign::NearPeak ||
            c.align == daphne::ColorAlign::Conservative);
    // a bias current: within the range of some preset (the union of their ranges)
    double lo = 1e9, hi = 0.0;
    for (auto p : {daphne::ColorPreset::Set2a3, daphne::ColorPreset::Set300b, daphne::ColorPreset::Set845,
                   daphne::ColorPreset::PpEl34Ul}) {
        lo = std::min(lo, daphne::color_bias_range(p).lo_ma);
        hi = std::max(hi, daphne::color_bias_range(p).hi_ma);
    }
    require(!c.bias_ma || (std::isfinite(*c.bias_ma) && *c.bias_ma >= lo && *c.bias_ma <= hi));
    require(!c.makeup_gain_db ||
            (std::isfinite(*c.makeup_gain_db) && std::abs(*c.makeup_gain_db) <= daphne::kColorMakeupTrimMaxDb));
    require(c.model == daphne::ColorModel::FixedPoint);
    // the settings as written: valid, and the same when read again
    const auto j = daphne::settings_to_json(r.settings);
    const auto again = daphne::settings_from_json(j);
    require(again.ok());
    require(daphne::settings_to_json(again.settings) == j);
    return 0;
}
