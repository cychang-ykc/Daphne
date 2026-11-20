#pragma once
// Final quantization to the device word length: TPDF dither (the difference of two uniform values,
// +-1 LSB, triangular), round, clamp to [-1, 1 - 1 LSB]. One stage at the end of the chain, after
// every level change, so the dither always matches the output grid.
//
// The chain leaves it out when nothing changes the samples and the source's integer samples fit the
// device word length: the path is then bit-perfect. Below 24 bits plain rounding would turn quiet
// signals into harmonic distortion; TPDF turns the error into steady, signal-independent noise.
// No noise shaping.
//
// Pass-through (Tube Color, doc/N28): where the chain would leave the quantizer out but an effect upstream may start
// changing the samples in place, it stays in the chain and copies while the samples are left as they are (they are
// already on the grid). Turned on again, the dither starts at once from a fresh generator (nothing of before is
// carried over: there is no noise-shaping state, only the generator).

#include <cstdint>

#include "daphne/dsp/block.h"

namespace daphne::dsp {

class Quantizer final : public Block {
public:
    // bits: output word length (16 / 24 / 32).
    explicit Quantizer(int bits, uint64_t seed = 0x2545F4914F6CDD1Dull) : bits_(bits), seed_(seed) {}

    void prepare(const Format& in) override;
    Format output_format() const override { return in_; }
    int process(const double* in, int frames, double* out) override;
    int latency_frames() const override { return 0; }
    void reset() override;
    bool is_identity() const override { return pass_; }
    void set_pass_through(bool on);
    bool pass_through() const { return pass_; }

    int bits() const { return bits_; }
    long long clip_count() const { return clips_; }

private:
    double uniform();

    int bits_;
    uint64_t seed_;
    uint64_t s_[4] = {};
    Format in_{};
    long long clips_ = 0;
    bool pass_ = false;
};

}  // namespace daphne::dsp
