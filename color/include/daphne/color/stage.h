#pragma once
// The Tube Color stage (doc/N28): a dsp::Block after the upsampler (PCM core: before the DSD limiter and the final
// quantizer; DSD core: after the modulator pre-filter, before the modulator limiter).
//
// M1 step 4: the stage takes its place in the chain and passes samples through unchanged; the curve, drive, volume
// cancellation and local oversampling follow in the next steps.

#include "daphne/dsp/block.h"

namespace daphne::color {

class TubeColorStage final : public dsp::Block {
public:
    void prepare(const dsp::Format& in) override { fmt_ = in; }
    dsp::Format output_format() const override { return fmt_; }
    int process(const double* in, int frames, double* out) override;
    int latency_frames() const override { return 0; }
    void reset() override {}
    bool is_identity() const override { return true; }

private:
    dsp::Format fmt_;
};

}  // namespace daphne::color
