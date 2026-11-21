#pragma once
// Tube Color (doc/N28): an effect stage after the upsampler that adds the harmonic signature of a tube amplifier.
// Not part of accurate playback: with color.enabled false (the default) it is not in the chain at all.
//
// M1 built the stage's machinery (insertion, glide, crossfade, volume cancellation, local oversampling) around a
// polynomial test curve, which exists only in test builds (DAPHNE_COLOR_TEST_CURVE: every preset runs on it there).
// M2 brings the presets' curves from the M0 circuits one by one (curves.h): in every other build a preset runs once
// its curve is exported, and is unavailable until then.

#include <string_view>

namespace daphne::color {

constexpr bool test_curve() {
#if defined(DAPHNE_COLOR_TEST_CURVE)
    return true;
#else
    return false;
#endif
}

// True when this build can run the stage for the preset (its settings name, "set-300b").
bool available(std::string_view preset);
// For at least one preset.
bool available_any();

// The reasons shown on the playback path when color.enabled is true but the stage cannot run.
inline constexpr const char* kUnavailableReason = "Tube Color unavailable (before M2)";
inline constexpr const char* kUnavailablePresetReason = "Tube Color unavailable for this preset (its curve comes later in M2)";

}  // namespace daphne::color
