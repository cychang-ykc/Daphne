#include "report.h"

#include "common.h"
#include "daphne/engine/chain.h"
#include "daphne/engine/dsd_output.h"

namespace daphne::playback {

void Report::design(bool force) {
    if (!cb.on_design) return;
    const auto now = std::chrono::steady_clock::now();
    if (!force && now - design_at_ < std::chrono::milliseconds(250)) return;
    design_at_ = now;
    cb.on_design(design_prog.load());
}

void Report::bit_perfect(const DspChain* c) {
    if (!c) return;
    const AudioInfo& src = rep.source;
    rep.not_bit_perfect_reason.clear();
    if (src.dsd_pcm) rep.not_bit_perfect_reason = "DSD converted to PCM";
    else if (!src.lossless_int) rep.not_bit_perfect_reason = "source is not integer PCM";
    else if (!c->transparent()) rep.not_bit_perfect_reason = "DSP active";
    else if (rep.system_rate > 0) rep.not_bit_perfect_reason = "the system resamples to the device's " + std::to_string(rep.system_rate) + " Hz";
    else if (shared_mix) rep.not_bit_perfect_reason = "shared mode (system mixer)";
    else if (effective_bits(rep.device) < src.bits)
        rep.not_bit_perfect_reason =
            "device resolution " + std::to_string(effective_bits(rep.device)) + "-bit < source " + std::to_string(src.bits) + "-bit";
    rep.bit_perfect = rep.not_bit_perfect_reason.empty();
}

std::string Report::filter(const Settings& s, const DsdEncoder* enc) const {
    // DSD file converted to PCM: the conversion, then the upsampling filter only when it is used
    std::string pre;
    if (rep.source.dsd_pcm) {
        const int dr = rep.source.dsd_rate;
        pre = "DSD" + std::to_string(dr > 0 ? dr / (dr % 44100 == 0 ? 44100 : 48000) : 0) + " -> PCM " +
              std::to_string(rep.source.rate) + " Hz (low-pass " + std::to_string(s.output.dsd_pcm_lowpass) + " kHz" +
              (s.output.dsd_pcm_limiter ? ", limiter" : "") + ")";
        if (s.core == CoreMode::Pcm && rep.device.rate == rep.source.rate && !s.filter.apodize) return pre;
        pre += ", then ";
    }
    std::string d = describe_filter(s);
    if (s.core == CoreMode::Pcm && rep.device.rate == rep.source.rate)
        d += s.filter.apodize ? " (output rate = source rate: 1x, the apodizing filter still applies)"
                              : " (output rate = source rate: no resampling, the filter is not used)";
    else if (s.core == CoreMode::Pcm)
        d += ", " + std::to_string(rep.source.rate) + " -> " + std::to_string(rep.device.rate) + " Hz";
    else if (enc)
        d += " | " + enc->describe();
    return pre + d;
}

bool Report::diag_due() {
    const auto now = std::chrono::steady_clock::now();
    if (now - diag_at_ < std::chrono::milliseconds(500)) return false;
    diag_at_ = now;
    return true;
}

void Report::set_diag(std::string d) {
    std::lock_guard<std::mutex> l(m_);
    diag_ = std::move(d);
}

std::string Report::diag_json() const {
    std::lock_guard<std::mutex> l(m_);
    return diag_.empty() ? "{}" : diag_;
}

void Report::set_asio_rates(std::vector<int> r) {
    std::lock_guard<std::mutex> l(m_);
    asio_rates_ = std::move(r);
}

std::vector<int> Report::asio_rates() const {
    std::lock_guard<std::mutex> l(m_);
    return asio_rates_;
}

}  // namespace daphne::playback
