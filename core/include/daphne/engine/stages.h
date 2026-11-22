#pragma once
// Processing-chain stages from the settings.

#include <memory>
#include <vector>

#include "daphne/dsp/crossfeed.h"
#include "daphne/dsp/eq.h"
#include "daphne/engine/settings.h"

namespace daphne {

std::vector<dsp::PeqBand> peq_bands(const EqSettings& s);

// EQ parameters for the rate the EQ runs at: the sections, the automatic preamp (a manual one
// only lowers it), on when enabled with at least one enabled band.
std::unique_ptr<dsp::PeqParams> make_peq_params(const EqSettings& s, double fs);

void configure_crossfeed(dsp::Crossfeed& c, const CrossfeedSettings& s);

// The gain stage's level: digital volume plus the track's headroom, less Tube Color's reserve (settings.h).
inline double gain_db(const Settings& s) {
    return (s.volume.mode == VolumeMode::Digital ? s.volume.db : 0.0) + s.headroom_db - color_reserve_db(s);
}

}  // namespace daphne
