#pragma once
// Tube Color's level alignment (doc/N28, an M2 experiment): the stage cancels the volume, so its curve sees the
// master's own level, and a loud master is driven harder than a quiet one at the same drive. Aligned, the curve's input
// is turned down by the track's level against the drive's calibration signal and the output turned back up (the
// effective drive is the drive times the gain; the output level is unchanged). Only ever down: a quiet track is not
// driven harder than now.
//
// The measures, from a scan of the whole track before (while) it plays:
//   lufs      ITU-R BS.1770-4 integrated loudness: K-weighting at the track's rate, 400 ms blocks with 75 % overlap,
//             the absolute (-70 LUFS) and relative (-10 LU) gates; every channel weighted 1
//   p999_db   the 99.9th percentile of |x| over every sample of every channel (dBFS; 0.01 dB steps): near the peaks
// Both at the level the curve sees (a DSD file converted to PCM: its +6 dB taken out by the caller, scale).
// The references: the loudness is the drive's calibration signal's (-18 dBFS RMS pink noise, 20 Hz - 20 kHz, in both
// channels; verify/drive_loudness.py). The 99.9th percentile is the median of the six test tracks' (whole tracks,
// 2026-11-21: -3.59 / -3.50 / -1.32 / -0.30 / -0.10 / -0.10 dBFS): the pink noise's peaks sit far below music's (its
// -7.64 dBFS would turn every track down, classical music by 4 dB, and drive 100 % would no longer be the calibrated
// amplifier); against the median an ordinary master is hardly turned down, only one louder than usual (doc/N28).

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

#include "daphne/engine/decoder.h"
#include "daphne/engine/settings.h"

namespace daphne {

struct TrackLevels {
    double lufs = 0.0;      // -inf: silence
    double p999_db = 0.0;
};

inline constexpr double kAlignRefLufs = -14.28;    // the calibration pink noise's integrated loudness
inline constexpr double kAlignRefP999Db = -0.81;   // the median of the six test tracks' 99.9th percentile of |x|

// The gain (dB, <= 0) on the curve's input for a track: off 0; loudness kAlignRefLufs - lufs; near-peak
// kAlignRefP999Db - p999_db; conservative the lower of the two; each at most 0.
double color_align_gain_db(ColorAlign mode, const TrackLevels& l);

// The two measures over a stream of interleaved frames.
class LevelMeter {
public:
    LevelMeter(int rate, int channels);
    void add(const double* x, int frames, double scale = 1.0);
    TrackLevels result() const;

private:
    struct Biquad {
        double b0, b1, b2, a1, a2;
    };
    int rate_, ch_, step_, in_step_ = 0;
    Biquad shelf_{}, hp_{};
    std::vector<double> z_;     // the two stages' state per channel (4 values each)
    double acc_ = 0.0;          // the current 100 ms step's sum of squares (channels summed)
    std::vector<double> steps_; // the mean square of each whole 100 ms step
    std::vector<uint64_t> hist_;   // |x| in 0.01 dB bins from kHistLowDb
    uint64_t samples_ = 0;
};

// Reads the decoder to its end (from where it is). Nullopt when cancelled or the stream gives nothing.
std::optional<TrackLevels> scan_levels(Decoder& d, const std::atomic<bool>& cancel, double scale = 1.0);

// The scans so far, process-wide, by file: its path, size and time of modification (a file that is not on disk: the
// path alone).
std::optional<TrackLevels> cached_levels(const std::filesystem::path& p);
void store_levels(const std::filesystem::path& p, const TrackLevels& l);
void clear_level_cache();   // tests

}  // namespace daphne
