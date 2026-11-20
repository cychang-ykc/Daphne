#include "daphne/engine/chain.h"

#if defined(DAPHNE_COLOR)
#include "daphne/color/stage.h"
#endif

#include <algorithm>

#include "daphne/dsp/crossfeed.h"
#include "daphne/dsp/downmix.h"
#include "daphne/dsp/eq.h"
#include "daphne/dsp/gain.h"
#include "daphne/dsp/limiter.h"
#include "daphne/dsp/modulator.h"
#include "daphne/dsp/quantizer.h"
#include "daphne/dsp/resampler.h"
#include "daphne/engine/filters.h"
#include "daphne/engine/stages.h"

namespace daphne {

namespace {

bool eq_on(const EqSettings& e) {
    return e.enabled && std::any_of(e.bands.begin(), e.bands.end(), [](const EqBand& b) { return b.on; });
}

// gain, crossfeed and EQ pass samples through unchanged with these settings
bool level_stages_identity(const Settings& s) { return gain_db(s) == 0.0 && !s.crossfeed.enabled && !eq_on(s.eq); }

// The modulator's input limiter: ceiling 0.85 x the loop's maximum stable input (after the gain);
// near the limit the loop's noise and idle tones rise. Release 500 ms keeps the distortion of a
// low-frequency over low (80 ms: a 50 Hz over at -41 dB, 500 ms: -56 dB). Below the ceiling the
// signal only gets the 2 ms look-ahead delay.
std::unique_ptr<dsp::Limiter> modulator_limiter(const ModulatorSettings& m) {
    const double ceiling = 0.85 * dsp::DsdModulator::max_stable_input(m.osr, m.order) / dsp::DsdModulator::kGain;
    return std::make_unique<dsp::Limiter>(ceiling, 2.0, 500.0);
}

}  // namespace

DspChain::DspChain() = default;
DspChain::~DspChain() = default;

std::string DspChain::structure_key(const Settings& s) {
    // Tube Color: only whether it is in the chain changes the structure (an inactive one is cleared by
    // without_inactive_color); its other fields are applied inside the stage (doc/N28)
    Settings k = without_inactive_color(s);
    k.color = {.enabled = k.color.enabled};
    k.eq = {};
    k.crossfeed = {};
    k.volume = {};
    k.headroom_db = 0.0;
    k.design = {};
    return settings_to_json(k).dump();
}

bool DspChain::needs_quantizer(const Settings& s, const AudioInfo& src, const DeviceFormat& dev, bool pre_transparent) {
    if (s.core == CoreMode::Dsd || is_float(dev.type) || is_dsd(dev.type)) return false;   // the modulator / the device take it
    const bool fits = src.lossless_int && src.bits > 0 && src.bits <= effective_bits(dev);
    return !(pre_transparent && level_stages_identity(s) && fits);
}

bool DspChain::fits(const Shape& sh, const Settings& s, const AudioInfo& next) {
    if (next.rate != sh.src.rate || next.channels != sh.src.channels || next.dsd_pcm != sh.src.dsd_pcm) return false;
    if (sh.src.channels != sh.dev.channels && next.channel_mask != sh.src.channel_mask) return false;   // the downmix matrix
    return needs_quantizer(s, next, sh.dev, sh.pre_transparent) == sh.quantized;
}

void DspChain::add(std::string name, std::unique_ptr<dsp::Block> b, int& frames, int& rate, int& channels) {
    b->prepare({rate, channels, frames});
    const dsp::Format f = b->output_format();
    frames = f.max_frames;
    rate = f.rate;
    channels = f.channels;
    max_buf_ = std::max(max_buf_, frames * channels);
    st_.push_back({std::move(name), std::move(b)});
}

bool DspChain::build(const Settings& s, const AudioInfo& src, const DeviceFormat& dev, int chunk, std::string* error) {
    st_.clear();
    gain_ = nullptr, xf_ = nullptr, peq_ = nullptr, quant_ = nullptr;
    clips_taken_ = 0;
    src_ = src;
    dev_ = dev;
    max_buf_ = chunk * std::max(src.channels, dev.channels);
    auto fail = [&](const std::string& m) {
        if (error) *error = m;
        st_.clear();
        return false;
    };
    int frames = chunk, rate = src.rate, ch = src.channels;

    // 1. channels, right after decoding
    if (ch != dev.channels) {
        if (dev.channels != 2) return fail("cannot map " + std::to_string(ch) + " source channels to " + std::to_string(dev.channels));
        add("downmix", std::make_unique<dsp::Downmix>(src.channel_mask), frames, rate, ch);
    }

    // 2. the core's rate
    if (s.core == CoreMode::Dsd) {
        if (dev.rate != modulator_input_rate(src.rate))
            return fail("internal: the modulator's PCM rate is " + std::to_string(modulator_input_rate(src.rate)) + " Hz, not " +
                        std::to_string(dev.rate));
    } else if (dev.rate < src.rate || dev.rate % src.rate != 0) {
        return fail("the device locked " + std::to_string(dev.rate) + " Hz, not an integer multiple of the source rate " +
                    std::to_string(src.rate) + " Hz");
    }

    // 3. gain, crossfeed, EQ at the source rate (the same on both cores)
    auto gain = std::make_unique<dsp::Gain>(gain_db(s));
    gain_ = gain.get();
    add("volume", std::move(gain), frames, rate, ch);
    auto xf = std::make_unique<dsp::Crossfeed>();
    configure_crossfeed(*xf, s.crossfeed);
    xf_ = xf.get();
    add("crossfeed", std::move(xf), frames, rate, ch);
    auto peq = std::make_unique<dsp::Peq>();
    peq->set_params(make_peq_params(s.eq, rate));
    peq_ = peq.get();
    eq_rate_ = rate;
    add("eq", std::move(peq), frames, rate, ch);

    // 4. upsampling: to the device (pcm) or to the modulator's input rate (dsd), the same filter
    if (dev.rate != rate || s.filter.apodize)
        add(s.core == CoreMode::Dsd ? "modulator pre-filter" : "resampler", make_resampler(s.filter, rate, dev.rate), frames, rate, ch);
#if defined(DAPHNE_COLOR)
    // Tube Color (doc/N28), an effect: after the upsampler, before the limiters and the final quantizer; only while
    // active (enabled, and a build that can run it), so with it off the chain is the one without it
    if (color_active(s)) add("tube color", std::make_unique<color::TubeColorStage>(), frames, rate, ch);
#endif
    if (s.core == CoreMode::Dsd) add("modulator limiter", modulator_limiter(s.modulator), frames, rate, ch);

    // the stages other than gain / crossfeed / EQ pass samples through
    pre_transparent_ = std::all_of(st_.begin(), st_.end(), [&](const Stage& x) {
        const dsp::Block* b = x.block.get();
        return b == gain_ || b == xf_ || b == peq_ || b->is_identity();
    });

    // 5. DSD file converted to PCM (+6 dB): peaks above the SACD reference are limited, not clipped
    if (src.dsd_pcm && s.output.dsd_pcm_limiter) {
        add("dsd limiter", std::make_unique<dsp::Limiter>(), frames, rate, ch);
        pre_transparent_ = false;
    }

    // 6. final quantization
    quantized_ = needs_quantizer(s);
    if (quantized_) {
        auto q = std::make_unique<dsp::Quantizer>(effective_bits(dev));
        quant_ = q.get();
        add("quantizer", std::move(q), frames, rate, ch);
    }
    if (rate != dev.rate || ch != dev.channels) return fail("internal: chain output does not match the device format");

    key_ = structure_key(s);
    out_ch_ = ch;
    max_out_ = frames;
    a_.assign(static_cast<size_t>(max_buf_), 0.0);
    b_.assign(static_cast<size_t>(max_buf_), 0.0);
    flush_.assign(static_cast<size_t>(max_buf_), 0.0);
    return true;
}

bool DspChain::update(const Settings& s) {
    if (st_.empty() || structure_key(s) != key_ || needs_quantizer(s) != quantized_) return false;
    gain_->set_db(gain_db(s));
    configure_crossfeed(*xf_, s.crossfeed);
    peq_->set_params(make_peq_params(s.eq, eq_rate_));
    return true;
}

int DspChain::content_delay() const {
    int d = 0;
    for (const auto& s : st_) d += s.block->content_delay_frames();
    return d;
}

int DspChain::run_from(size_t first, const double* in, int frames, double* out) {
    const double* src = in;
    int n = frames;
    if (first >= st_.size()) {
        if (src != out) std::copy(src, src + static_cast<size_t>(n) * out_ch_, out);
        return n;
    }
    for (size_t i = first; i < st_.size(); ++i) {
        double* dst = i + 1 == st_.size() ? out : (src == a_.data() ? b_.data() : a_.data());
        n = st_[i].block->process(src, n, dst);
        src = dst;
    }
    return n;
}

int DspChain::process(const double* in, int frames, double* out) {
    if (st_.empty()) return 0;
    return run_from(0, in, frames, out);
}

int DspChain::flush(double* out) {
    int total = 0;
    for (size_t i = 0; i < st_.size(); ++i) {
        const int n = st_[i].block->flush(flush_.data());
        if (n <= 0) continue;
        total += run_from(i + 1, flush_.data(), n, out + static_cast<size_t>(total) * out_ch_);
    }
    return total;
}

void DspChain::reset() {
    for (auto& s : st_) s.block->reset();
    clips_taken_ = 0;   // the quantizer's count restarts with it
}

int64_t DspChain::take_clips() {
    if (!quant_) return 0;
    const int64_t n = quant_->clip_count() - clips_taken_;
    clips_taken_ = quant_->clip_count();
    return n;
}

bool DspChain::transparent() const {
    return std::all_of(st_.begin(), st_.end(), [](const Stage& s) { return s.block->is_identity(); });
}

std::vector<std::string> DspChain::all_stages() const {
    std::vector<std::string> v;
    for (const auto& s : st_) v.push_back(s.name);
    return v;
}

std::vector<std::string> DspChain::stages() const {
    std::vector<std::string> v;
    for (const auto& s : st_)
        if (!s.block->is_identity()) v.push_back(s.name);
    return v;
}

}  // namespace daphne
