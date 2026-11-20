// Player::Impl, DSD files sent directly (core.h), DSD silence and the end of the stream.
#include "core.h"

namespace daphne {

void Player::Impl::push_dsd_idle(int ms) {
    if (!dsdenc) return;
    dsd_bytes.clear();
    dsdenc->idle(rep.device.frame_rate() * ms / 1000, dsd_bytes);
    if (io.bytes.size() < dsd_bytes.size()) io.bytes.resize(dsd_bytes.size());
    std::copy(dsd_bytes.begin(), dsd_bytes.end(), io.bytes.begin());
    io.pend = dsd_bytes.size();
    io.pend_off = 0;
    while (!io.push()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
}

void Player::Impl::push_pcm_silence(int ms) {
    if (!dsdenc || !dsdenc->dop()) return;
    const size_t n = static_cast<size_t>(rep.device.frame_rate()) * ms / 1000 * io.block;
    if (io.bytes.size() < n) io.bytes.resize(n);
    std::fill(io.bytes.begin(), io.bytes.begin() + static_cast<std::ptrdiff_t>(n), 0);
    io.pend = n;
    io.pend_off = 0;
    while (!io.push()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
}

void Player::Impl::queue_end() {
    end_queued = true;
    io.eof = true;
    if (!dsdenc) return;
    dsd_bytes.clear();
    dsdenc->idle(rep.device.frame_rate() / 10, dsd_bytes);
    if (dsdenc->dop()) dsd_bytes.resize(dsd_bytes.size() + static_cast<size_t>(rep.device.frame_rate()) / 5 * io.block, 0);
    end_pad = static_cast<int64_t>(dsd_bytes.size()) / std::max(1, io.block);
    if (io.bytes.size() < dsd_bytes.size()) io.bytes.resize(dsd_bytes.size());
    std::copy(dsd_bytes.begin(), dsd_bytes.end(), io.bytes.begin());
    io.pend = dsd_bytes.size();
    io.pend_off = 0;
}

std::optional<Settings> Player::Impl::remodulation(const Settings& ns, const OutputChoice& no) const {
    const int dsd = rep.source.dsd_rate;
    if (!color_active(ns) || dsd <= 0 || !is_dsd_path(cur_path)) return std::nullopt;
    const OutputCaps c = deps.output_caps(no);
    // the DSD multiple; the modulator's clock is 44.1 kHz x osr (a 48 kHz-family file changes rate, doc/N28)
    const int multiple = dsd / (dsd % 44100 == 0 ? 44100 : 48000);
    const bool dsd_out = ns.output.dsd != DsdMode::Pcm && !c.shared_mix && (c.native_dsd || ns.output.dsd != DsdMode::Native) &&
                         !(ns.output.max_dsd > 0 && multiple > ns.output.max_dsd) && multiple >= 64;
    if (!dsd_out) return std::nullopt;
    Settings r = ns;
    r.core = CoreMode::Dsd;
    r.modulator.osr = multiple;
    return r;
}

void Player::Impl::set_dsd_source() {
    const DsdInfo& di = dsdf->info();
    rep.source = AudioInfo{};
    rep.source.rate = di.dsd_rate / 8;
    rep.source.channels = di.channels;
    rep.source.bits = 1;
    rep.source.lossless_int = true;
    rep.source.frames = di.frames;
    rep.source.channel_mask = di.channel_mask;
    rep.source.codec = di.codec;
    rep.source.dsd_rate = di.dsd_rate;
}

// The device's two kinds of stream do not mix: the DSD one ends on silence and the device is opened again for PCM
// (fading in), as for a new output (handle_dsd). The position is carried over in seconds (DSD bytes -> PCM frames).
bool Player::Impl::dsd_direct_to_pcm(const Settings& ns, const OutputChoice& no) {
    check_switch();
    // where the PCM goes on: paused, or with the next file already queued, from what is heard (what is queued is
    // dropped); otherwise what is queued plays out first and it goes on after it (as restart() does for PCM)
    int64_t from = heard_position();
    if (!tl.switches.empty()) {   // the next file was read ahead but is not heard yet: it is the next track again
        queue.restore(tl.switches.front().path);
        tl.switches.clear();
        if (io.ring) io.ring->clear();
        io.pend = io.pend_off = 0;
    } else if (io.dev && !io.dev->paused()) {
        while (!io.push()) std::this_thread::sleep_for(std::chrono::milliseconds(2));   // the chunk made is queued too
        from = std::max<int64_t>(0, tl.abs_out - tl.track_origin_in);
    }
    const double sec = rep.source.rate > 0 ? static_cast<double>(from) / rep.source.rate : 0.0;
    io.drop_if_paused();
    push_dsd_idle(50);
    push_pcm_silence(200);
    io.eof = true;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (io.dev && !io.dev->drained() && !io.dev->failed() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (io.dev) io.dev->stop();
    io.dev.reset();
    dsdf.reset();
    dsdenc.reset();
    s = ns;
    out = no;
    tl.track_origin_in = 0;
    tl.h_base = 0;
    end_queued = false;
    want_fade_in = true;
    event(remodulation(s, out) ? "DSD remodulated (Tube Color)" : "DSD converted to PCM (Tube Color)");
    std::string e;
    dec = open_source(cur_path, s, &e);
    if (!dec) {
        err = e;
        return false;
    }
    rep.source = dec->info();
    ch = rep.source.channels;
    pcm.assign(static_cast<size_t>(kChunk) * ch, 0.0);
    hist.reset(ch);
    dsd_chain_for_color = !caps().shared_mix && s.output.dsd != DsdMode::Pcm;
    stream = pcm_stream;
    if (!open_and_start(static_cast<int64_t>(std::llround(sec * rep.source.rate)), &e)) {   // (it reports the start)
        err = e;
        return false;
    }
    return true;
}

bool Player::Impl::back_to_dsd_direct(const Settings& ns) const {
    return dsd_chain_for_color && !dsdf && color_active(s) && !color_active(ns) && ns.output.dsd != DsdMode::Pcm && is_dsd_path(cur_path);
}

// restart() has faded the PCM out and stopped the device; `from` is in the converted track's PCM frames
bool Player::Impl::pcm_to_dsd_direct(int64_t from) {
    const double sec = rep.source.rate > 0 ? static_cast<double>(from) / rep.source.rate : 0.0;
    std::string e;
    auto f = std::make_unique<DsdFile>();
    if (!f->open(cur_path, &e)) {
        err = e;
        return false;
    }
    dsdf = std::move(f);
    set_dsd_source();
    dsd_chain_for_color = false;
    end_queued = false;
    event("Tube Color off: the DSD file is sent directly again");
    if (open_dsd(static_cast<int64_t>(std::llround(sec * rep.source.rate)), &e)) {
        dec.reset();
        stream = dsd_stream;
        if (cb.on_start) cb.on_start(rep);
        return true;
    }
    // no DSD path after all (the device refused it now): converted as before
    event("DSD cannot be sent directly (" + e + "): converting the DSD file to PCM");
    dsdf.reset();
    dsdenc.reset();
    rep.source = dec->info();
    stream = pcm_stream;
    if (!open_and_start(from, &e)) {
        err = e;
        return false;
    }
    return true;
}

bool Player::Impl::open_dsd(int64_t from, std::string* error) {
    const DsdInfo& di = dsdf->info();
    struct Attempt {
        int rate;
        bool native;
    };
    std::vector<Attempt> at;
    const int multiple = di.dsd_rate / (di.dsd_rate % 44100 == 0 ? 44100 : 48000);
    if (s.output.max_dsd > 0 && multiple > s.output.max_dsd) {
        if (error) *error = "DSD" + std::to_string(multiple) + " is above output.max_dsd (DSD" + std::to_string(s.output.max_dsd) + ")";
        return false;
    }
    if (caps().native_dsd && s.output.dsd != DsdMode::Dop) at.push_back({di.dsd_rate, true});
    // DoP only where it reaches the DAC untouched: a shared output mixes and resamples it into noise
    if (s.output.dsd != DsdMode::Native && !caps().shared_mix) at.push_back({di.dsd_rate / 16, false});
    std::string e = at.empty() ? (caps().shared_mix ? "shared mode mixes everything to PCM"
                                                    : "output.dsd = native needs an ASIO driver with a DSD mode")
                               : "";
    auto what = [&](const Attempt& a) {
        return "DSD" + std::to_string(multiple) + (a.native ? " (native)" : " (DoP, " + std::to_string(a.rate) + " Hz)");
    };
    for (size_t i = 0; i < at.size(); ++i) {
        const Attempt& a = at[i];
        if (cancel) {   // stopped while starting: no further attempt
            io.dev.reset();
            if (error) *error = "cancelled";
            return false;
        }
        // a failure before the last goes to the log, so the host sees why the next was tried
        auto next = [&]() {
            if (io.dev)
                for (const auto& n : io.dev->take_notes()) event(n);   // why the device did not take it (its rate change)
            io.dev.reset();
            if (i + 1 < at.size() && !cancel) event("the output at " + what(a) + " failed (" + e + "): trying " + what(at[i + 1]));
        };
        io.dev = open_output({out, s.output.buffer_ms, a.rate, di.channels, a.native}, &e);
        if (!io.dev) {
            next();
            continue;
        }
        // (an output that resamples reports the rate asked for: the device's own is system_rate)
        const int locked = io.dev->system_rate() > 0 ? io.dev->system_rate() : io.dev->format().rate;
        if (locked != a.rate || io.dev->format().channels != di.channels) {
            if (!a.native && locked < a.rate) dop_locked = locked;
            DeviceFormat f = io.dev->format();
            f.rate = locked;
            e = "the device locked " + describe(f) + " instead of " + std::to_string(a.rate) + " Hz, " + std::to_string(di.channels) + " ch";
            next();
            continue;
        }
        dsdenc = std::make_unique<DsdEncoder>();
        if (!dsdenc->init_direct(di.dsd_rate, io.dev->format(), a.native ? DsdTransport::Native : DsdTransport::Dop, &e)) {
            next();
            continue;
        }
        rep.device = io.dev->format();
        rep.device_name = io.dev->device_name();
        rep.system_rate = io.dev->system_rate();
        rep.latency_ms = io.dev->latency_ms();
        io.note_buffer_fallback(report, s.output.buffer_ms);
        crate = di.dsd_rate / 8;
        ratio = 1;
        dch = rep.device.channels;
        io.block = rep.device.block_align();
        dsdf->seek(from);
        tl.abs_out = from;
        session_in = from;
        end_pad = 0;
        io.eof = false;
        io.pend = io.pend_off = 0;
        io.ring = std::make_unique<ByteRing>(io.ring_bytes(rep.device.frame_rate()));
        dsd_in.assign(static_cast<size_t>(kChunk) * 4 * di.channels, 0);
        io.bytes.assign(static_cast<size_t>(kChunk) * 4 * io.block, 0);
        io.limit = io.chunk = 0;
        // silence first, so the DAC has switched to DSD and locked before the music (output.dsd_lead_in_ms;
        // at least 30 ms after a restart)
        push_dsd_idle(std::max(s.output.dsd_lead_in_ms, want_fade_in ? 30 : 0));
        while (!cancel && io.room() >= io.chunk_bytes()) {
            if (!produce_dsd()) {
                io.eof = true;
                break;
            }
            io.push();
        }
        if (cancel) {
            io.dev.reset();
            if (error) *error = "cancelled";
            return false;
        }
        io.dev->set_dop(dsdenc->dop());
        io.dev->set_paused(want_pause.load());
        if (!io.dev->start(io.ring.get(), &io.eof, &e)) {
            next();
            continue;
        }
        rep.chain.clear();
        rep.filter = "none (DSD file sent directly) | " + dsdenc->describe();
        rep.bit_perfect = true;
        rep.not_bit_perfect_reason.clear();
        return true;
    }
    if (error) *error = e;
    return false;
}

bool Player::Impl::produce_dsd() {
    if (io.pend_off < io.pend) return true;
    const int n = dsdf->read(dsd_in.data(), kChunk * 4);
    if (n <= 0) return false;
    dsd_bytes.clear();
    dsdenc->encode_dsd(dsd_in.data(), n, dsd_bytes);
    if (io.bytes.size() < dsd_bytes.size()) io.bytes.resize(dsd_bytes.size());
    std::copy(dsd_bytes.begin(), dsd_bytes.end(), io.bytes.begin());
    io.pend = dsd_bytes.size();
    io.pend_off = 0;
    tl.abs_out += n;
    rep.frames_played += n;
    return true;
}

bool Player::Impl::handle_dsd(const Settings& ns, const OutputChoice& no, bool reset) {
    // Tube Color switched on: the file goes on as PCM through the chain, from where it is heard
    if (!reset && color_active(ns) && !color_active(s)) return dsd_direct_to_pcm(ns, no);
    const bool reopen = reset || dsd_direct_reopens(playing(), ns, no);
    s = ns;
    if (!reopen) {
        event("DSD file played directly: DSP settings do not apply");
        return true;
    }
    const int64_t from = heard_position();
    io.drop_if_paused();
    if (!reset) {
        push_dsd_idle(50);
        push_pcm_silence(200);
        io.eof = true;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (io.dev && !io.dev->drained() && !io.dev->failed() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (io.dev) io.dev->stop();
    io.dev.reset();
    out = no;
    want_fade_in = true;
    event(reset ? "reopening the device" : "restarting the device for the new output");
    std::string e;
    if (!open_dsd(from, &e)) {
        err = e;
        return false;
    }
    if (cb.on_start) cb.on_start(rep);
    return true;
}

}  // namespace daphne
