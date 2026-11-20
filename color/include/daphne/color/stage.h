#pragma once
// The Tube Color stage (doc/N28): a dsp::Block after the upsampler (PCM core: before the DSD limiter and the final
// quantizer; DSD core: after the modulator pre-filter, before the modulator limiter).
//
//   y = post_m( x~ + DC-block( f(d x~) / d - x~ ) ),   x~ = pre_m(x)
//
// d is the drive, m = response_mix(d) the fade of the amplifier's response (linear.h): the response's corners move in
// from outside the audio band up to 25 % drive, and from there the response is the preset's and d only changes the
// distortion. f is the curve (M1: the test curve), the DC blocker (first order, kDcBlockHz) takes out the offset an
// asymmetric curve adds. At d = 1 this is the reference amplifier: post(f(pre(x))).
//
// Below 8x the rate family all of it runs oversampled (oversampler.h), the round trip delaying the content by D frames.
//
// Drive 0: the stage is idle, a pure delay of D frames (the input samples as they are: with the content delay
// compensated the output is bit for bit the input); nothing else runs, it only keeps the last input frames. Crossing 0
// is done in place, in the stage, so the chain is never rebuilt for it:
//   leaving 0   the oversampler is primed from the history (kHistory frames), the delay path crossfades (kSwitchMs) to
//               the processing path held at kRiseFrom (next to nothing: the two are the same signal while they cross,
//               aligned), then the drive glides (kGlideMs) to its value
//   reaching 0  the drive glides down to kRiseFrom, the processing path crossfades back to the delay path, idle
// Other drive changes glide in place.

#include <atomic>
#include <complex>
#include <vector>

#include "daphne/color/linear.h"
#include "daphne/color/oversampler.h"
#include "daphne/dsp/block.h"

namespace daphne::color {

struct StageParams {
    double drive = 1.0;   // 0 .. kColorDriveMax
    LinearModel linear;   // the preset's pre / post sections (empty: none)
};

class TubeColorStage final : public dsp::Block {
public:
    static constexpr double kDcBlockHz = 2.0;
    static constexpr double kGlideMs = 20.0;
    static constexpr double kSwitchMs = 20.0;   // the crossfade between the delay path and the processing path
    static constexpr double kRiseFrom = 1e-4;   // the drive the processing path crosses at
    static constexpr int kControl = 16;         // frames between updates of the fade while the drive glides
    static constexpr int kHistory = 2 * Oversampler::kTapsPerPhase;   // input frames that prime the oversampler

    explicit TubeColorStage(StageParams p = {});
    void prepare(const dsp::Format& in) override;
    dsp::Format output_format() const override { return fmt_; }
    int process(const double* in, int frames, double* out) override;
    int latency_frames() const override { return 0; }
    int content_delay_frames() const override { return delay_; }
    void reset() override;
    // The drive is 0: idle, or about to be within kGlideMs + kSwitchMs (the reports; the samples are told apart by
    // untouched())
    bool is_identity() const override { return target_.load() <= 0.0; }
    // The last process() call left every sample as it was (the delay path alone): the final quantizer may pass them
    bool untouched() const { return untouched_; }

    // A new drive, gliding there from the current one (any thread; from the next process()); 0 takes the stage idle.
    void set_drive(double d);
    double drive() const { return d_; }   // the current (gliding) drive, on the processing thread
    bool idle() const { return w_ <= 0.0; }
    int oversampling() const { return os_.factor(); }
    // The linear response at hz now (pre and post; 1 while idle), for tests and the M2 tools
    std::complex<double> linear_response(double hz) const { return w_ <= 0.0 ? 1.0 : pre_.response(hz) * post_.response(hz); }

private:
    void update_mix();
    void glide_to(double v);
    void prime();
    void run_frame(const double* x, double* y);      // one frame at the processing rate
    void process_path(const double* x, double* y);   // one input frame through the processing path

    StageParams p_;
    dsp::Format fmt_;
    std::atomic<double> target_;
    double last_target_ = 0.0, d_ = 0.0, step_ = 0.0, mix_d_ = -1.0;
    int left_ = 0, glide_frames_ = 1;
    double w_ = 0.0, w_step_ = 1.0;   // the processing path's share (0: the delay path alone)
    bool primed_ = false, untouched_ = true;
    int delay_ = 0;
    SectionChain pre_, post_;
    Oversampler os_;
    std::vector<double> hi_;                        // L frames at the high rate
    std::vector<double> hist_;                      // the last input frames (ring of hist_frames_)
    int hist_frames_ = 0, hist_at_ = 0, fed_ = 0;   // fed: frames stored since the start (up to the ring's size)
    std::vector<double> xin_, dly_, path_, prime_y_;   // one frame each: the input, the delay path, the processing path
    double dc_r_ = 0.0;                             // the DC blocker's pole
    std::vector<double> dc_x1_, dc_y1_;             // its state per channel (last input, last output)
};

}  // namespace daphne::color
