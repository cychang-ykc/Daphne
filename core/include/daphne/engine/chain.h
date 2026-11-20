#pragma once
// The processing chain for one track:
//
//   decoder -> downmix / mono duplication (source channels != device channels)
//           -> gain (volume + headroom) -> crossfeed -> EQ          all at the source rate
//           -> pcm core: resampler to the device rate (a copy at the source rate without apodizing)
//              dsd core: the same filter to 705.6 / 768 kHz, then the modulator's input limiter;
//                        the chain ends in PCM, the player's DsdEncoder modulates
//           -> DSD file converted to PCM: look-ahead limiter (output.dsd_pcm_limiter)
//           -> pcm core, integer device: TPDF quantizer at the device word length, left out when
//              the chain is transparent for an integer source that fits the device (bit-perfect)
//
// The device format (rate, channels, sample type) is decided before the chain is built; for the dsd
// core it is the modulator's PCM input (modulator_input_rate, float64).

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "daphne/dsp/block.h"
#include "daphne/engine/decoder.h"
#include "daphne/engine/sample_format.h"
#include "daphne/engine/settings.h"

namespace daphne::dsp {
class Gain;
class Crossfeed;
class Peq;
class Quantizer;
}  // namespace daphne::dsp

namespace daphne::color {
class TubeColorStage;
}  // namespace daphne::color

namespace daphne {

class DspChain {
public:
    DspChain();
    ~DspChain();

    // chunk: the largest frame count process() will be given.
    bool build(const Settings& s, const AudioInfo& src, const DeviceFormat& dev, int chunk, std::string* error);

    // Gain, crossfeed and EQ changes applied in place (the gain ramps, the EQ glides or crossfades).
    // False when the change needs a new chain (filter, core, output, or a different final stage).
    bool update(const Settings& s);

    // Interleaved source frames in, device frames out (out must hold max_output_frames()).
    int process(const double* in, int frames, double* out);
    // End of stream: frames still held back (resampler partial block, limiter delay).
    int flush(double* out);
    void reset();

    int max_output_frames() const { return max_out_; }
    int output_channels() const { return out_ch_; }
    // Device frames between an input sample and its output content (resampler kernel peak, limiter
    // delay); used to align a crossfade between two chains.
    int content_delay() const;
    // Every stage passes samples through unchanged and nothing quantizes.
    bool transparent() const;
    // Names of the stages that change the signal, in order, for reports.
    std::vector<std::string> stages() const;
    // Names of every stage, in order, including those that currently pass samples through (tests, diagnostics).
    std::vector<std::string> all_stages() const;
    // Samples the final quantizer clamped to full scale since the last call (0 without a quantizer).
    // Its output is already inside the device range, so convert_to_device() never sees these.
    int64_t take_clips();

    // Settings fields that update() cannot change in place.
    static std::string structure_key(const Settings& s);

    // What the chain's stages depend on besides the settings, copied out so that another thread can
    // compare a source against it (fits()).
    struct Shape {
        AudioInfo src{};
        DeviceFormat dev{};
        bool pre_transparent = true;
        bool quantized = false;
    };
    Shape shape() const { return {src_, dev_, pre_transparent_, quantized_}; }
    // A chain built from these settings for `next` would be this one: the same rate and channel count,
    // the same layout when downmixed, DSD-to-PCM or not, and the final quantizer present or left out
    // alike (it depends on the source's word length: a 24-bit track after a 16-bit one on a 16-bit
    // device needs it). False: the next track needs its own chain.
    static bool fits(const Shape& sh, const Settings& s, const AudioInfo& next);
    bool fits(const Settings& s, const AudioInfo& next) const { return fits(shape(), s, next); }

private:
    struct Stage {
        std::string name;
        std::unique_ptr<dsp::Block> block;
    };
    void add(std::string name, std::unique_ptr<dsp::Block> b, int& frames, int& rate, int& channels);
    int run_from(size_t first, const double* in, int frames, double* out);
    bool needs_quantizer(const Settings& s) const { return needs_quantizer(s, src_, dev_, pre_transparent_); }
    static bool needs_quantizer(const Settings& s, const AudioInfo& src, const DeviceFormat& dev, bool pre_transparent);
    static bool quantizer_present(const Settings& s, const AudioInfo& src, const DeviceFormat& dev, bool pre_transparent);

    std::vector<Stage> st_;
    std::vector<double> a_, b_, flush_;
    int max_out_ = 0, max_buf_ = 0, out_ch_ = 0, eq_rate_ = 0;
    // for update()
    AudioInfo src_{};
    DeviceFormat dev_{};
    std::string key_;
    bool quantized_ = false;
    bool quant_follows_color_ = false;   // kept only for Tube Color: passes while the stage is idle (doc/N28)
    bool pre_transparent_ = true;   // the stages other than gain / crossfeed / EQ
    dsp::Gain* gain_ = nullptr;
    dsp::Crossfeed* xf_ = nullptr;
    dsp::Peq* peq_ = nullptr;
    dsp::Quantizer* quant_ = nullptr;
#if defined(DAPHNE_COLOR)
    color::TubeColorStage* color_ = nullptr;   // Tube Color (doc/N28), when in the chain
#endif
    int64_t clips_taken_ = 0;
};

}  // namespace daphne
