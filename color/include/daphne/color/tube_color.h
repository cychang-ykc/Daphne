#pragma once
// Tube Color (doc/N28): an effect stage after the upsampler that adds the harmonic signature of a tube amplifier.
// Not part of accurate playback: with color.enabled false (the default) it is not in the chain at all.
//
// M1 builds the stage's machinery (insertion, glide, crossfade, volume cancellation, local oversampling) around a
// polynomial test curve; the test curve exists only in test builds (DAPHNE_COLOR_TEST_CURVE). In every other build
// Tube Color is unavailable until M2 brings the real curves of the four presets.

namespace daphne::color {

// True when this build can run the stage: before M2 only test builds (the M1 test curve).
constexpr bool available() {
#if defined(DAPHNE_COLOR_TEST_CURVE)
    return true;
#else
    return false;
#endif
}

// The reason shown on the playback path when color.enabled is true but the stage cannot run.
inline constexpr const char* kUnavailableReason = "Tube Color unavailable (before M2)";

}  // namespace daphne::color
