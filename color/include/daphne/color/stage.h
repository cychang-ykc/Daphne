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
// Drive 0 is the identity, by skipping everything (not by arithmetic): a stage built at drive 0 stays the identity, and
// crossing 0 rebuilds the chain (structure_key). Other drive changes glide (kGlideMs, linear) in place (set_drive).

#include <atomic>
#include <complex>
#include <vector>

#include "daphne/color/linear.h"
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
    static constexpr int kControl = 16;   // frames between updates of the fade while the drive glides

    explicit TubeColorStage(StageParams p = {});
    void prepare(const dsp::Format& in) override;
    dsp::Format output_format() const override { return fmt_; }
    int process(const double* in, int frames, double* out) override;
    int latency_frames() const override { return 0; }
    void reset() override;
    bool is_identity() const override { return identity_; }

    // A new drive, gliding there from the current one (any thread; from the next process()). Ignored by a stage built
    // at drive 0, and a target of 0 is not taken (crossing 0 is a rebuild).
    void set_drive(double d);
    double drive() const { return d_; }   // the current (gliding) drive, on the processing thread
    // The linear response at hz now (pre and post; 1 at drive 0), for tests and the M2 tools
    std::complex<double> linear_response(double hz) const { return identity_ ? 1.0 : pre_.response(hz) * post_.response(hz); }

private:
    void update_mix();

    StageParams p_;
    bool identity_;
    dsp::Format fmt_;
    std::atomic<double> target_;
    double last_target_ = 0.0, d_ = 0.0, step_ = 0.0, mix_d_ = -1.0;
    int left_ = 0, glide_frames_ = 1;
    SectionChain pre_, post_;
    double dc_r_ = 0.0;                    // the DC blocker's pole
    std::vector<double> dc_x1_, dc_y1_;    // its state per channel (last input, last output)
};

}  // namespace daphne::color
