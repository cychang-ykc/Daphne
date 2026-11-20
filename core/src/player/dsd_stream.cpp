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

bool Player::Impl::seamless_on_ok(const Settings& ns, const OutputChoice& no) const {
    const auto r = remodulation(ns, no);
    if (!r || !dsdf || !dsdenc || !(no == out) || !tl.switches.empty() || (io.dev && io.dev->paused())) return false;
    const int dsd = rep.source.dsd_rate;
    if (dsd % 44100 != 0) return false;   // 48 kHz family: the modulator runs at 44.1 kHz x osr, the rate changes
    const OutputCaps c = deps.output_caps(no);
    const bool native = c.native_dsd && ns.output.dsd != DsdMode::Dop;
    if (native == dsdenc->dop()) return false;   // the remodulation would take the other transport
    const int64_t chain_rate = modulator_input_rate(dsd_pcm_rate > 0 ? dsd_pcm_rate : 176400);   // the converter's rate
    return chain_rate * 8 % dsd == 0 && chain_rate * 8 / dsd >= 1 && chain_rate * 8 / dsd <= 2;   // DSD64, DSD128
}

bool Player::Impl::seamless_off_ok(const Settings& ns, const OutputChoice& no) const {
    if (dsdf || !dsdenc || !cur || mod_q == 0 || mod_path != cur_path || !(no == out) || !tl.switches.empty()) return false;
    if ((io.dev && io.dev->paused()) || xf.fading || cross_join.active()) return false;
    const OutputCaps c = deps.output_caps(no);
    const bool native = c.native_dsd && ns.output.dsd != DsdMode::Dop && ns.output.dsd != DsdMode::Pcm;
    return native != dsdenc->dop() && rep.source.dsd_rate % 44100 == 0;
}

void Player::Impl::begin_remod_switch(const Settings& ns, const OutputChoice& no) {
    switch_on = std::make_pair(ns, no);
    RemodRequest q;
    q.path = cur_path;
    q.cs = chain_settings(ns, ns.modulator.osr);
    q.cs.color.drive = 0.0;   // idle until it has taken over: then it fades in
    q.dsd_pcm_rate = dsd_pcm_rate;
    q.dsd_rate = rep.source.dsd_rate;
    q.device = rep.device;
    q.dop = dsdenc->dop();
    // the switch point: the lead past what is written, on 8 bytes (whole input frames and DoP frames, DSD64 / DSD128)
    const int64_t align = 8;
    const int64_t lead = static_cast<int64_t>(kSwitchLead * rep.source.dsd_rate / 8);
    q.at = (tl.abs_out + lead + align - 1) / align * align;
    if (rep.source.frames > 0 && q.at >= rep.source.frames - lead) {   // too close to the end: as before, reopened
        switch_on.reset();
        dsd_direct_to_pcm(ns, no);
        return;
    }
    switch_at = q.at;
    remod_cancel = std::make_shared<std::atomic<bool>>(false);
    remod_job = std::async(std::launch::async, [q, flag = remod_cancel]() -> std::unique_ptr<RemodPrep> {
        PoolThreadScope pool;
        platform::set_thread_priority(platform::Priority::BelowNormal);
        std::string e;
        return prepare_remod(q, flag.get(), &e);
    });
    event("Tube Color on: the remodulation is prepared while the DSD file plays on directly (it takes over without "
          "reopening the device)");
}

void Player::Impl::cancel_remod_switch() {
    if (remod_cancel) *remod_cancel = true;
    if (remod_job.valid()) remod_job.wait();   // (it stops at its next chunk or filter design step)
    remod_job = {};
    remod_ready.reset();
    switch_at = -1;
    switch_on.reset();
}

// The direct stream's poll: a prepared remodulation either waits for the direct stream to reach its point, or (the
// direct stream went past it while it was being prepared) runs on to a later one; it never holds the direct stream up.
bool Player::Impl::poll_remod_switch() {
    if (!remod_job.valid() || remod_job.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return true;
    auto r = remod_job.get();
    if (!r) {
        event("Tube Color on: the remodulation could not be prepared; reopening the device");
        const auto to = switch_on;
        cancel_remod_switch();
        if (to) return dsd_direct_to_pcm(to->first, to->second);
        return true;
    }
    if (r->p >= tl.abs_out && switch_at == r->p) {   // ready in time
        remod_ready = std::move(r);
        return true;
    }
    // late: a later point, the same preparation running on to it
    const int64_t lead = static_cast<int64_t>(kSwitchLead * rep.source.dsd_rate / 8), align = 8;
    const int64_t at = (tl.abs_out + lead + align - 1) / align * align;
    if (rep.source.frames > 0 && at >= rep.source.frames - lead) {
        const auto to = switch_on;
        cancel_remod_switch();
        event("Tube Color on: the remodulation was not ready before the end of the file; reopening the device");
        if (to) return dsd_direct_to_pcm(to->first, to->second);
        return true;
    }
    event("Tube Color on: the remodulation was not ready at its point; it takes over later (the DSD file plays on)");
    switch_at = at;
    std::shared_ptr<RemodPrep> sp(std::move(r));
    remod_job = std::async(std::launch::async, [sp, at, flag = remod_cancel]() -> std::unique_ptr<RemodPrep> {
        PoolThreadScope pool;
        platform::set_thread_priority(platform::Priority::BelowNormal);
        if (!sp->advance_to(at, flag.get())) return nullptr;
        auto out = std::make_unique<RemodPrep>();
        *out = std::move(*sp);
        return out;
    });
    return true;
}

// At the switch point: what is queued is the file up to byte p, the remodulation goes on from p.
bool Player::Impl::take_over_remod() {
    auto r = std::move(remod_ready);
    const auto to = *switch_on;
    remod_job = {};
    remod_cancel.reset();
    switch_at = -1;
    switch_on.reset();
    const int64_t session_bytes = session_in;
    r->enc->follow(*dsdenc);   // the DoP marker sequence goes on
    dsdf.reset();
    dsdenc = std::move(r->enc);
    dec = std::move(r->dec);
    cur = std::move(r->chain);
    hist = std::move(r->hist);
    s = to.first;
    rep.source = dec->info();
    ch = rep.source.channels;
    pcm.assign(static_cast<size_t>(kChunk) * static_cast<size_t>(ch), 0.0);
    chain_fmt = r->devf;
    crate = chain_fmt.rate;
    ratio = r->ratio;
    run_osr = s.modulator.osr;
    want = requested_rate(s, rep.source.rate);
    dch = rep.device.channels;
    in_total = r->in;
    tl.abs_out = static_cast<int64_t>(r->ratio) * r->f0 + r->out;   // the chain frames made, as if from the session's start
    tl.h_base = 0;
    tl.track_origin_in = 0;
    session_in = session_bytes / r->bytes_per_in;
    skip_out = 0;
    flushed = false;
    tail_zeros = -1;
    end_queued = false;
    xf.clear();
    xf.set_channels(dch);
    cross_join.clear();
    throughput.reset();
    read_frames = kChunk;
    io.limit = 0;
    io.chunk = 0;
    dsp.assign(static_cast<size_t>(cur->max_output_frames()) * static_cast<size_t>(dch), 0.0);
    tmp_c = dsp;
    tmp_n = dsp;
    const size_t need = dsdenc->max_bytes(cur->max_output_frames()) + dsdenc->pending_per_channel() * 4 * static_cast<size_t>(io.block);
    if (io.bytes.size() < need) io.bytes.resize(need);
    mod0 = r->mod0();
    mod_q = r->q;
    mod_path = cur_path;
    dsd_chain_for_color = true;
    stream = pcm_stream;
    // what the modulator made past p goes out first
    dsd_bytes.clear();
    dsdenc->pack_pending(dsd_bytes);
    std::copy(dsd_bytes.begin(), dsd_bytes.end(), io.bytes.begin());
    io.pend = dsd_bytes.size();
    io.pend_off = 0;
    // Tube Color fades in from its idle stage (the chain was built at drive 0)
    if (!cur->update(chain_settings(s, run_osr))) launch_build(s, out);
    rep.chain = cur->stages();
    rep.filter = report.filter(s, dsdenc.get());
    report.bit_perfect(tl.heard_chain(cur.get()));
    event("DSD remodulated (Tube Color): taken over at DSD byte " + std::to_string(r->p) + " without reopening the device");
    if (cb.on_changed) cb.on_changed(rep);
    return true;
}

// The stage is idle: the remodulation carries the file as it is, and the file's own bytes go on where the modulator's
// next byte would have been.
bool Player::Impl::take_over_direct() {
    const auto to = *switch_off;
    switch_off.reset();
    const int64_t k = dsdenc->emitted_per_channel();
    const int64_t e_byte = (mod0 + static_cast<int64_t>(mod_q) * k - 2) / mod_q;
    std::string e;
    auto f = std::make_unique<DsdFile>();
    if (!f->open(cur_path, &e) || !f->seek(e_byte)) {
        event("Tube Color off: the DSD file could not be opened again here (" + e + "); reopening the device");
        return false;
    }
    auto enc = std::make_unique<DsdEncoder>();
    if (!enc->init_direct(rep.source.dsd_rate, rep.device, dsdenc->dop() ? DsdTransport::Dop : DsdTransport::Native, &e)) {
        event("Tube Color off: " + e + "; reopening the device");
        return false;
    }
    enc->follow(*dsdenc);
    const int bytes_per_in = rep.source.dsd_rate / 8 / std::max(1, rep.source.rate);
    const int64_t session_frames = session_in;
    s = to.first;
    dsdf = std::move(f);
    dsdenc = std::move(enc);
    set_dsd_source();
    crate = rep.source.rate;   // DSD bytes per channel per second
    ratio = 1;
    dch = rep.device.channels;
    tl.abs_out = e_byte;
    tl.h_base = 0;
    tl.track_origin_in = 0;
    session_in = session_frames * bytes_per_in;
    end_queued = false;
    dsd_in.assign(static_cast<size_t>(kChunk) * 4 * static_cast<size_t>(rep.source.channels), 0);
    if (io.bytes.size() < static_cast<size_t>(kChunk) * 4 * static_cast<size_t>(io.block)) io.bytes.resize(static_cast<size_t>(kChunk) * 4 * static_cast<size_t>(io.block));
    io.limit = io.chunk = 0;
    dsd_chain_for_color = false;
    mod_q = 0;
    stream = dsd_stream;
    rep.chain.clear();
    rep.filter = "none (DSD file sent directly) | " + dsdenc->describe();
    rep.bit_perfect = true;
    rep.not_bit_perfect_reason.clear();
    event("Tube Color off: the DSD file is sent directly again from DSD byte " + std::to_string(e_byte) + " without reopening the device");
    if (cb.on_changed) cb.on_changed(rep);
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
    int read_max = kChunk * 4;
    if (switch_at >= 0) {
        if (tl.abs_out >= switch_at) {
            if (remod_ready && remod_ready->p == tl.abs_out && take_over_remod()) return produce();   // here it takes over
            switch_at = -1;   // not ready here: the direct stream plays on (poll_remod_switch puts it later)
        } else {
            read_max = static_cast<int>(std::min<int64_t>(read_max, switch_at - tl.abs_out));
        }
    }
    const int n = dsdf->read(dsd_in.data(), read_max);
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
    // Tube Color switched on: the file goes on remodulated (or as PCM), from where it is heard. On the open device when
    // the output stays as it is (6b: prepared off this thread, the direct stream plays on until it takes over), else the
    // device is reopened
    if (switch_on || remod_job.valid()) {
        if (!reset && color_active(ns) && no == out) {
            switch_on = std::make_pair(ns, no);   // the latest, applied when it takes over
            return true;
        }
        cancel_remod_switch();
        if (!color_active(ns) && !reset) {
            event("Tube Color off before the remodulation took over: the DSD file is still sent directly");
            if (!dsd_direct_reopens(playing(), ns, no)) {
                s = ns;
                return true;
            }
        }
    }
    if (!reset && color_active(ns) && !color_active(s)) {
        if (seamless_on_ok(ns, no)) {
            begin_remod_switch(ns, no);
            return true;
        }
        return dsd_direct_to_pcm(ns, no);
    }
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
