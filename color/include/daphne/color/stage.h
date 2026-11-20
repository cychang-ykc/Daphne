#pragma once
// The Tube Color stage (doc/N28): a dsp::Block after the upsampler (PCM core: before the DSD limiter and the final
// quantizer; DSD core: after the modulator pre-filter, before the modulator limiter).
//
// What it adds is the curve's distortion, not the curve: y = x + DC-block( f(d x) / d - x ), with d the drive. The
// signal itself never goes through a filter (the frequency response stays the chain's); the DC blocker (first order,
// kDcBlockHz) takes out the offset an asymmetric curve adds (its x^2 term). d = 0 is the identity.
//
// M1 so far: the test curve (test builds) at a fixed drive. Glide, volume cancellation and local oversampling follow.

#include <vector>

#include "daphne/dsp/block.h"

namespace daphne::color {

struct StageParams {
    double drive = 1.0;   // 0 .. kColorDriveMax
};

class TubeColorStage final : public dsp::Block {
public:
    static constexpr double kDcBlockHz = 2.0;

    explicit TubeColorStage(StageParams p = {}) : p_(p) {}
    void prepare(const dsp::Format& in) override;
    dsp::Format output_format() const override { return fmt_; }
    int process(const double* in, int frames, double* out) override;
    int latency_frames() const override { return 0; }
    void reset() override;
    bool is_identity() const override { return p_.drive == 0.0; }

private:
    StageParams p_;
    dsp::Format fmt_;
    double dc_r_ = 0.0;                          // the DC blocker's pole
    std::vector<double> dc_x1_, dc_y1_;          // its state per channel (last input, last output)
};

}  // namespace daphne::color
