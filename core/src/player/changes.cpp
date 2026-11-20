// Player::Impl, settings changes (core.h, N4): in place, background rebuild, device restart.
#include "core.h"

namespace daphne {

void Player::Impl::launch_build(const Settings& ns, const OutputChoice& no) {
    build_target = std::make_pair(ns, no);
    const Settings cs = chain_settings(ns, run_osr);
    const AudioInfo src = rep.source;
    report.design_prog = 0.0;
    build_designs = (chain_fmt.rate != src.rate || cs.filter.apodize) && !filter_cached(cs.filter, src.rate, chain_fmt.rate);
    build_cancel = std::make_shared<std::atomic<bool>>(false);
    building = std::async(std::launch::async, [cs, src, devf = chain_fmt, this, flag = build_cancel]() -> std::unique_ptr<DspChain> {
        PoolThreadScope pool;   // the priority below is undone when the task ends (common.h)
        // playback is running: stay below it (the design workers take this priority)
        platform::set_thread_priority(platform::Priority::BelowNormal);
        dsp::ProgressScope scope(&report.design_prog);
        dsp::CancelScope cancel_scope(flag.get());
        auto c = std::make_unique<DspChain>();
        std::string e;
        try {
            if (!c->build(cs, src, devf, kChunk, &e)) return nullptr;
        } catch (const dsp::DesignCancelled&) {
            return nullptr;
        } catch (const std::exception&) {   // e.g. memory for a long filter: the current chain stays
            return nullptr;
        }
        return c;
    });
    event((build_designs ? "designing " : "building ") + describe_filter(ns) + " in the background (playback continues)");
}

bool Player::Impl::restart(const Settings& ns, const OutputChoice& no, bool drain) {
    const bool to_dsd = back_to_dsd_direct(ns);
    check_switch();
    if (tl.cross_pending()) drain = false;   // the queue holds the next track at another rate: not played out
    int64_t from = dsdf ? heard_position() : std::max<int64_t>(0, heard_input());   // PCM: stream timeline
    // paused: what is queued is dropped and nothing plays out (draining would skip it and play a faded chunk while
    // paused): it goes on from where it is heard
    if (io.dev && io.dev->paused()) drain = false;
    io.drop_if_paused();
    queue.drop();   // built for the old settings / device
    if (io.dev && !io.eof && !io.dev->failed() && drain) {
        // play what is queued, fade the next chunk out, let the device drain, resume there
        while (!io.push()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        from = std::max<int64_t>(0, (tl.abs_out - (cur ? cur->content_delay() : 0)) / std::max(1, ratio));
        fade_out_len = std::max<int64_t>(1, crate / 50);   // 20 ms
        fade_out_next = true;
        if (produce())
            while (!io.push()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        push_dsd_idle(50);
        push_pcm_silence(200);
        io.eof = true;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!io.dev->drained() && !io.dev->failed() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    want_fade_in = true;
    // `from` is on the stream timeline. With gapless tracks the decoder may have moved on: resume in
    // the track that holds `from`, at its own frame (a later track that was decoded but not reached
    // goes back to being the next track)
    bool reopen_dec = decoder_changes(playing(), ns);
    if (!dsdf) {
        while (!tl.switches.empty() && !tl.switches.front().cross && from >= tl.switches.front().at_in) {
            tl.track_origin_in = tl.switches.front().at_in;
            rep.source = tl.switches.front().info;
            cur_path = tl.switches.front().path;
            tl.switches.pop_front();
            if (cb.on_track_started) cb.on_track_started(rep);
        }
        if (!tl.switches.empty()) {
            queue.restore(tl.switches.front().path);
            undo_cross();
            undo_headroom();
            tl.switches.clear();
            reopen_dec = true;
        }
        from = std::max<int64_t>(0, from - tl.track_origin_in);
        tl.track_origin_in = 0;
    }
    if (io.dev) io.dev->stop();
    io.dev.reset();
    s = ns;
    out = no;
    if (to_dsd) return pcm_to_dsd_direct(from);
    if (reopen_dec) {
        std::string e;
        auto d = open_source(cur_path, s, &e);
        if (!d) {
            err = e;
            return false;
        }
        dec = std::move(d);
    }
    event(drain ? "restarting the device for the new core / rate / output" : "reopening the device");
    std::string e;
    if (!open_and_start(from, &e)) {
        err = e;
        return false;
    }
    return true;
}

bool Player::Impl::zero_cross_only(const Settings& ns) const {
    const Settings a = chain_settings(s, run_osr);
    Settings b = chain_settings(ns, run_osr);
    if (DspChain::structure_key(a) == DspChain::structure_key(b)) return false;
    b.color.drive = a.color.drive;
    return DspChain::structure_key(a) == DspChain::structure_key(b);
}

bool Player::Impl::handle(const Settings& ns, const OutputChoice& no, bool from_hold) {
    // the drive crossing 0 (the stage in or out of the signal): held, the latest settings kept (doc/N28)
    if (!from_hold && cur && !needs_restart(playing(), ns, no) && zero_cross_only(ns)) {
        if (!held) {
            held_until = std::chrono::steady_clock::now() + kZeroCrossHold;
            event("Tube Color drive crosses 0: the chain is rebuilt once the drive settles (300 ms)");
        }
        held = std::make_pair(ns, no);
        return true;
    }
    held.reset();   // newer settings (back across 0, or anything else) replace it
    queue.drop();   // a chain prepared for the next track was built for the old settings
    if (chain_busy()) {   // one change at a time (and not across a rate switch); the latest waits
        queued = std::make_pair(ns, no);
        if (building.valid() && build_cancel) *build_cancel = true;   // superseded: stop designing it
        return true;
    }
    dsp::set_design_threads(ns.design.threads);
    // Tube Color switched off while a DSD file plays as PCM only for it: sent directly again (restart -> pcm_to_dsd_direct)
    if (needs_restart(playing(), ns, no) || back_to_dsd_direct(ns)) return restart(ns, no);
    if (cur->update(chain_settings(ns, run_osr))) {
        s = ns;
        event("applied in place (EQ / crossfeed / volume)");
        // the stages that are active now (EQ switched on / off, volume at 0 dB ...)
        const auto stages = cur->stages();
        const bool changed_stages = stages != rep.chain, was_bit_perfect = rep.bit_perfect;
        rep.chain = stages;
        report.bit_perfect(tl.heard_chain(cur.get()));
        if (changed_stages || rep.bit_perfect != was_bit_perfect) {
            if (cb.on_changed) cb.on_changed(rep);
        }
        return true;
    }
    launch_build(ns, no);
    return true;
}

bool Player::Impl::poll_build() {
    if (!building.valid()) return true;
    if (building.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        if (build_designs) report.design();
        return true;
    }
    report.design(true);
    auto nx = building.get();
    auto target = *build_target;
    build_target.reset();
    if (queued) {   // superseded while designing
        auto q = *queued;
        queued.reset();
        return handle(q.first, q.second);
    }
    if (!nx) {
        event("the new settings could not be built; keeping the current chain");
        return true;
    }
    s = target.first;
    begin_crossfade(std::move(nx));
    kick_cache_trim();
    return true;
}

}  // namespace daphne
