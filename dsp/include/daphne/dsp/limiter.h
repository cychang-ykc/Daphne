#pragma once
// Look-ahead peak limiter: after DSD -> PCM (whose +6 dB lets peaks above the SACD reference level
// exceed full scale) and in front of the DSD modulator (which must stay below its stable input).
//
// Every frame whose peak (largest channel) would exceed the ceiling asks for gain ceiling / peak.
// That request is held for the look-ahead window, released towards 1 with a one-pole release,
// and averaged over the window; the audio is delayed by the window. The average of values that
// are all <= the request reaches the peak exactly when the delayed peak sample does, so no sample
// leaves above the ceiling, and the gain moves in a smooth ramp instead of a step. All channels
// share one gain (the stereo image stays). Below the ceiling the output is the delayed input,
// bit for bit.
//
// knee_db > 0: a soft knee (Tube Color's limiter, doc/N28). The gain asked for follows a quadratic knee in dB around
// the ceiling: from ceiling - knee/2 the peak is bent down gradually and reaches the ceiling at ceiling + knee/2,
// never above it; below the knee the output is the delayed input, bit for bit, as without one.
// Pass-through (set_pass_through): asks for gain 1 whatever the peak (the delay stays, so switching does not move
// the signal); a gain still below 1 goes back to exactly 1 in a straight line over kPassReleaseMs.
// Aligned (set_aligned): its delay reported as content delay with no latency and no tail of its own, like an aligned
// filter (the player feeds content-delay zeros at the end; Tube Color's stage works the same way).

#include <vector>

#include "daphne/dsp/block.h"

namespace daphne::dsp {

class Limiter final : public Block {
public:
    explicit Limiter(double ceiling = 0.999, double lookahead_ms = 2.0, double release_ms = 80.0, double knee_db = 0.0)
        : ceiling_(ceiling), lookahead_ms_(lookahead_ms), release_ms_(release_ms), knee_db_(knee_db) {}

    void prepare(const Format& in) override;
    Format output_format() const override { return fmt_; }
    int process(const double* in, int frames, double* out) override;
    int latency_frames() const override { return aligned_ ? 0 : w_; }
    int content_delay_frames() const override { return w_; }
    int flush(double* out) override;   // the delayed tail (none when aligned)
    void reset() override;
    bool is_identity() const override { return false; }   // it delays the signal

    // Frames since the last reset whose gain was below 1, for reports and tests.
    long long limited_frames() const { return limited_; }
    void set_pass_through(bool on) { pass_ = on; }
    void set_aligned(bool on) { aligned_ = on; }
    static constexpr double kPassReleaseMs = 20.0;
    bool pass_through() const { return pass_; }
    // The gain asked for a peak (the knee; 1 below it): ceiling / peak above the knee.
    double request(double peak) const;

private:
    double ceiling_, lookahead_ms_, release_ms_, knee_db_;
    bool pass_ = false, aligned_ = false;
    double pass_step_ = 1.0;          // the straight release in pass-through, per frame
    Format fmt_{};
    int w_ = 1;                       // look-ahead (delay) in frames
    double rel_ = 0.0;                // release coefficient per frame
    std::vector<double> delay_;       // w_ frames, interleaved ring
    std::vector<double> avg_;         // last w_ released gains (box average)
    std::vector<double> min_val_;     // monotonic queue for the sliding minimum over w_ + 1 requests
    std::vector<long long> min_idx_;
    int qhead_ = 0, qsize_ = 0;
    int pos_ = 0;                     // ring position (delay_, avg_)
    long long n_ = 0;                 // frame counter
    double sum_ = 0.0, env_ = 1.0;
    long long limited_ = 0;
    std::vector<double> zeros_;       // flush input
};

}  // namespace daphne::dsp
