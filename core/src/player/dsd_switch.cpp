#include "dsd_switch.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <thread>

#include "common.h"
#include "daphne/dsp/progress.h"
#include "daphne/engine/dsd_file.h"

namespace daphne::playback {

std::atomic<int> g_remod_prep_delay_ms{0};

bool RemodPrep::advance_to(int64_t target, const std::atomic<bool>* cancel) {
    // the chain frame whose content starts at the file byte target, and the modulator byte that starts with it
    const int64_t t_in = target / bytes_per_in;
    const int64_t jstar = static_cast<int64_t>(ratio) * (t_in - f0) + delay;
    const int64_t bstar = (jstar + dummy + 2) / q;
    while ((out + dummy) / q < bstar) {
        if (cancel && *cancel) return false;
        const int got = dec->read(pcm.data(), kChunk);
        if (got <= 0) return false;   // the end of the file: no switch this close to it
        hist.store(pcm.data(), got, in);
        in += got;
        const int n = chain->process(pcm.data(), got, buf.data());
        if (n > 0) enc->modulate(buf.data(), n);
        out += n;
    }
    enc->drop_front(static_cast<size_t>(bstar - dropped));
    dropped = bstar;
    p = target;
    return true;
}

std::unique_ptr<RemodPrep> prepare_remod(const RemodRequest& r, const std::atomic<bool>* cancel, std::string* error) {
    auto fail = [&](const std::string& e) -> std::unique_ptr<RemodPrep> {
        if (error) *error = e;
        return nullptr;
    };
    if (const int ms = g_remod_prep_delay_ms.load(); ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    auto x = std::make_unique<RemodPrep>();
    x->dec = make_dsd_pcm_decoder(r.dsd_pcm_rate, r.cs.output.dsd_pcm_lowpass);
    std::string e;
    if (!x->dec->open(r.path, &e)) return fail(e);
    x->src = x->dec->info();
    const int ch = x->src.channels;
    x->devf = r.device;
    x->devf.rate = modulator_input_rate(x->src.rate);
    x->devf.type = SampleType::Float64;
    x->devf.container_bits = x->devf.valid_bits = 64;
    x->ratio = x->devf.rate / x->src.rate;
    x->bytes_per_in = r.dsd_rate / 8 / x->src.rate;
    const int64_t clock = dsd_clock(r.cs.modulator.osr);
    if (clock != r.dsd_rate || (static_cast<int64_t>(x->devf.rate) * 8) % clock != 0) return fail("not a whole number of chain frames per DSD byte");
    x->q = static_cast<int>(static_cast<int64_t>(x->devf.rate) * 8 / clock);
    if (x->q < 1 || x->q > 2) return fail("not a whole number of chain frames per DSD byte");
    x->chain = std::make_unique<DspChain>();
    try {
        dsp::CancelScope cancel_scope(cancel);
        if (!x->chain->build(r.cs, x->src, x->devf, kChunk, &e)) return fail(e);
    } catch (const dsp::DesignCancelled&) {
        return fail("cancelled");
    } catch (const std::exception& ex) {
        return fail(ex.what());
    }
    // the chain's delay and the converter's (in chain frames, rounded: what is left is under a DSD bit)
    x->delay = x->chain->content_delay() + static_cast<int>(std::llround(x->ratio * x->dec->content_delay_frames()));
    x->enc = std::make_unique<DsdEncoder>();
    if (!x->enc->init(x->devf.rate, r.cs.modulator.osr, r.cs.modulator.order, r.device, r.dop ? DsdTransport::Dop : DsdTransport::Native, &e))
        return fail(e);
    x->pcm.assign(static_cast<size_t>(kChunk) * static_cast<size_t>(ch), 0.0);
    x->buf.assign(static_cast<size_t>(x->chain->max_output_frames()) * static_cast<size_t>(ch), 0.0);
    x->hist.reset(ch);
    // the pre-roll: from where the chain's output at the switch has the filter fully filled, with a margin
    const int64_t t_in = r.at / x->bytes_per_in;
    const int64_t pre = (x->delay + 16 + x->ratio - 1) / x->ratio + 4096;
    x->f0 = std::max<int64_t>(0, t_in - pre);
    x->in = x->f0;
    x->hist.valid_from = x->f0;
    if (!x->dec->seek(x->f0)) return fail("seek");
    // whole modulator bytes start where whole file bytes do: silent frames in front when the chain's delay is off
    const int64_t jstar = static_cast<int64_t>(x->ratio) * (t_in - x->f0) + x->delay;
    x->dummy = static_cast<int>(((x->q - (jstar + 2) % x->q) % x->q));
    if (x->dummy > 0) {
        std::vector<double> z(static_cast<size_t>(x->dummy) * static_cast<size_t>(ch), 0.0);
        x->enc->modulate(z.data(), x->dummy);
    }
    if (!x->advance_to(r.at, cancel)) return fail(cancel && *cancel ? "cancelled" : "the end of the file");
    return x;
}

}  // namespace daphne::playback
