// Player::Impl, the processing thread (core.h): start, the playback loop, transport, device upkeep, the end.
#include "core.h"

#include <cstdio>

#include "daphne/engine/file_bytes.h"
#include "daphne/engine/live_source.h"

namespace daphne {

// PCM: settings changes in place / rebuilt / with a restart; the background rebuild and the change
// queued behind it are polled between chunks.
class Player::Impl::PcmStream final : public Stream {
public:
    explicit PcmStream(Impl& p) : p_(p) {}
    bool apply(const Settings& ns, const OutputChoice& no) override { return p_.handle(ns, no); }
    bool poll() override {
        if (!p_.poll_build()) return false;
        p_.poll_warm();
        if (!p_.chain_busy() && p_.queued) {
            auto q = *p_.queued;
            p_.queued.reset();
            return p_.handle(q.first, q.second);
        }
        return true;
    }
    bool produce() override { return p_.produce(); }
    bool reopen() override { return p_.restart(p_.s, p_.out, false); }

private:
    Impl& p_;
};

// A DSD file sent directly: only the output can change; at the end of a file the next one may follow.
class Player::Impl::DsdDirectStream final : public Stream {
public:
    explicit DsdDirectStream(Impl& p) : p_(p) {}
    bool apply(const Settings& ns, const OutputChoice& no) override { return p_.handle_dsd(ns, no); }
    bool produce() override { return p_.produce_dsd() || p_.try_gapless(); }
    bool reopen() override { return p_.handle_dsd(p_.s, p_.out, true); }

private:
    Impl& p_;
};

void Player::Impl::update_diag() {
    if (!report.diag_due()) return;
    char b[512];
    const OutputStats* st = io.dev ? &io.dev->stats() : nullptr;
    std::snprintf(b, sizeof b,
                  "{\"ring\":%zu,\"ring_cap\":%zu,\"pend\":%zu,\"eof\":%d,\"fading\":%d,\"building\":%d,\"paused\":%d,"
                  "\"dev_paused\":%d,\"callbacks\":%lld,\"underruns\":%lld,\"drained\":%d,\"failed\":%d,\"late\":%lld,\"abs_out\":%lld,\"in\":%lld,"
                  "\"rt\":%.2f}",
                  io.ring ? io.ring->readable() : 0, io.ring ? io.ring->capacity() : 0, io.pend - io.pend_off, io.eof.load() ? 1 : 0, xf.fading ? 1 : 0,
                  building.valid() || warming.valid() ? 1 : 0, want_pause.load() ? 1 : 0, io.dev && io.dev->paused() ? 1 : 0,
                  st ? static_cast<long long>(st->callbacks.load()) : -1LL, st ? static_cast<long long>(st->underruns.load()) : -1LL,
                  io.dev && io.dev->drained() ? 1 : 0, io.dev && io.dev->failed() ? 1 : 0,
                  st ? static_cast<long long>(st->late_callbacks.load()) : -1LL, static_cast<long long>(tl.abs_out), static_cast<long long>(in_total),
                  throughput.factor());
    report.set_diag(b);
}

void Player::Impl::stop_cache_trim(bool join) {
    for (auto& j : pre_jobs) *j.cancel = true;
    if (!join) return;
    for (auto& j : pre_jobs)
        if (j.th.joinable()) j.th.join();
    pre_jobs.clear();
}

void Player::Impl::kick_cache_trim() {
    stop_cache_trim(false);
    auto flag = std::make_shared<std::atomic<bool>>(false);
    PreJob j;
    j.cancel = flag;
    // the track playing keeps its filter too when its rate is not one of the usual ones (22.05, 32 kHz ...)
    j.th = std::thread([cfg = s, rate = rep.source.rate, flag] {
        platform::set_thread_priority(platform::Priority::Background);
        if (!*flag) trim_filter_cache(cfg, rate);
    });
    pre_jobs.push_back(std::move(j));
}

int64_t Player::Impl::heard_out() const {
    int64_t q = io.queued_frames();
    if (rep.device.rate > 0 && crate != rep.device.frame_rate()) q = q * crate / rep.device.frame_rate();   // DoP frames -> chain frames
    return tl.abs_out - q;
}

void Player::Impl::check_switch() {
    while (!tl.switches.empty()) {
        Switch& w = tl.switches.front();
        if (w.cross) {
            if (heard_out() < w.at_out + w.new_delay) break;
            tl.h_base = w.at_out;
            tl.track_origin_in = 0;
            want = requested_rate(s, w.info.rate);
        } else {
            if (heard_input() < w.at_in) break;
            tl.track_origin_in = w.at_in;
        }
        const bool cross = w.cross;
        if (cross && cross_join.active() && cross_join.old == w.old_chain.get()) cross_join.owned = std::move(w.old_chain);   // its tail is still being made
        rep.source = w.info;
        cur_path = w.path;
        tl.switches.pop_front();
        if (!dsdf) {   // a DSD file sent directly has no chain and stays bit-perfect
            if (cross) rep.filter = report.filter(s, dsdenc.get());
            rep.chain = tl.heard_chain(cur.get())->stages();   // a cross switch's new chain; the track's own headroom
            report.bit_perfect(tl.heard_chain(cur.get()));     // the track's own format
        }
        event(cross ? "gapless: next track (through its own chain: another sample rate or format)" : "gapless: next track");
        if (cb.on_track_started) cb.on_track_started(rep);
    }
}

bool Player::Impl::do_seek(double sec) {
    if (!io.dev) return true;
    io.dev->set_paused(true);
    // the device fades out at its next callback, one buffer away (output.buffer_ms): wait for two of them
    const int buf_ms = rep.device.frame_rate() > 0 ? static_cast<int>(1000LL * io.dev->buffer_frames() / rep.device.frame_rate()) : 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(300, 2 * buf_ms + 50));
    while (!io.dev->paused() && !io.dev->failed() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    // a track heard just now is the one the seek is in. Before the ring is cleared: the heard position is what was
    // written less what the ring still holds, so with the ring empty everything written would count as heard and a
    // next track only decoded ahead (the last ~0.5 s of this one) would be taken as playing: the seek landed in it
    check_switch();
    io.ring->clear();
    io.pend = io.pend_off = 0;
    cross_join.clear();   // the old chain's tail is not played after a seek
    std::string e;
    if (!tl.switches.empty()) {
        // the decoder already moved on to a gapless track that is not heard yet: back to this one
        queue.restore(tl.switches.front().path);
        undo_cross();
        undo_headroom();
        tl.switches.clear();
        // the decoder holds the next track now: without this one again the seek would play the other at this position
        bool reopened = false;
        if (dsdf) {
            auto f = std::make_unique<DsdFile>();
            if ((reopened = f->open(cur_path, &e))) dsdf = std::move(f);
        } else if (auto d = open_source(cur_path, s, &e)) {
            dec = std::move(d);
            reopened = true;
        }
        if (!reopened) {
            err = "seek: the track could not be opened again: " + e;
            return false;
        }
    }
    int64_t from = static_cast<int64_t>(std::max(0.0, sec) * rep.source.rate);
    if (rep.source.frames > 0) from = std::min(from, rep.source.frames);
    if (dsdf) {
        dsdf->seek(from);
        tl.abs_out = from;
    } else {
        dec->seek(from);
        in_total = from;
        hist.valid_from = from;
        flushed = false;
        tail_zeros = -1;
        xf.clear();
        auto warmed = take_warm();
        if (xf.fading || warmed) {   // a crossfade was running or warming up: the new chain takes over
            cur = warmed ? std::move(warmed) : std::move(xf.next);
            xf.fading = false;
            rep.chain = cur->stages();
            rep.filter = report.filter(s, dsdenc.get());
            report.bit_perfect(tl.heard_chain(cur.get()));
        }
        cur->reset();
        tl.abs_out = from * ratio;
        skip_out = cur->content_delay();
    }
    tl.h_base = 0;
    tl.track_origin_in = 0;
    io.eof = false;
    end_queued = false;
    io.dev->rearm();
    while (!cancel && io.room() >= io.chunk_bytes()) {   // refill
        if (!(dsdf ? produce_dsd() : produce())) {
            if (!try_gapless()) {
                queue_end();
                break;
            }
            continue;
        }
        io.push();
    }
    io.push();
    io.dev->set_paused(want_pause.load());
    pos = heard_position();
    event(is_live_path(cur_path) ? std::string("live input: back to the writer (what it wrote meanwhile is skipped)")
                                 : "seek to " + std::to_string(sec) + " s");
    return true;
}

bool Player::Impl::service_transport() {
    std::optional<double> sk;
    {
        std::lock_guard<std::mutex> l(m);
        sk.swap(seek_req);
    }
    if (sk && !do_seek(*sk)) return false;
    // a live source resumed: the producer kept writing while it was paused, so it goes back to the writer
    // (a seek: the device is already silent, the ring is refilled, it fades in) instead of playing that late
    const bool paused_now = want_pause.load();
    if (io.dev && was_paused && !paused_now && !dsdf && is_live_path(cur_path) && !do_seek(pos_sec.load())) return false;
    was_paused = paused_now;
    if (io.dev) {
        io.dev->set_paused(want_pause.load());
        double l = 0, r = 0;
        io.dev->meter(l, r);
        if (l > meter_l.load()) meter_l = l;
        if (r > meter_r.load()) meter_r = r;
    }
    check_switch();
    if (throughput.warn()) {
        char b[256];
        std::snprintf(b, sizeof b,
                      "processing runs at %.2fx real time (it needs %.1fx): the device will play gaps; a shorter filter, "
                      "no double-double convolution or a lower output rate helps",
                      throughput.factor(), Throughput::kWarn);
        event(b);
    }
    update_diag();
    return true;
}

void Player::Impl::run() {
    try {
        run_playback();
    } catch (const std::exception& ex) {
        abort_run(ex.what());
    } catch (...) {
        abort_run("unknown exception");
    }
}

void Player::Impl::abort_run(const std::string& what) {
    const std::string e = "internal error: " + what;
    try {   // the helper threads end before the state they use goes away
        queue.join_all();
        if (build_cancel) *build_cancel = true;
        if (building.valid()) building.wait();
        take_warm();
        stop_cache_trim(true);
        if (io.dev) io.dev->stop();
    } catch (...) {
    }
    bool starting = false;
    {
        std::lock_guard<std::mutex> l(m);
        if (!start_done) {   // start() is waiting: it returns the error
            start_done = true;
            start_ok = false;
            start_err = e;
            starting = true;
        }
    }
    if (starting) {
        cv.notify_all();
    } else {
        ok = false;
        err = e;
        if (!cancel && cb.on_failed) cb.on_failed(err);
    }
    done = true;
}

void Player::Impl::run_playback() {
    platform::RealtimeScope rt;
    dsp::set_design_threads(s.design.threads);
    std::string e;
    bool started_dsd = false;
    cur_path = path;   // the opening already depends on it (a live source, open_and_start)
    if (is_dsd_path(path)) {
        auto f = std::make_unique<DsdFile>();
        if (!f->open(path, &e)) {
            std::lock_guard<std::mutex> l(m);
            start_done = true;
            start_ok = false;
            start_err = e;
            cv.notify_all();
            done = true;
            return;
        }
        dsdf = std::move(f);
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
        std::string de = "output.dsd = pcm";
        if (s.output.dsd != DsdMode::Pcm && open_dsd(static_cast<int64_t>(start_sec * rep.source.rate), &de)) {
            started_dsd = true;
            if (cb.on_start) cb.on_start(rep);
        } else {
            // no DSD path to the device: the DSD file is converted to PCM
            event(s.output.dsd == DsdMode::Pcm ? "converting the DSD file to PCM (output.dsd = pcm)"
                                                : "DSD cannot be sent directly (" + de + "): converting the DSD file to PCM");
            // shared mode runs at the mixer rate: convert to it when it divides the DSD rate
            if (caps().shared_mix) {
                std::string pe;
                if (auto probe = open_output({out, s.output.buffer_ms, dsdf->info().dsd_rate / 16, dsdf->info().channels, false}, &pe)) {
                    const int mix = probe->system_rate() > 0 ? probe->system_rate() : probe->format().rate;
                    if (mix > 0 && dsdf->info().dsd_rate % (mix * 8) == 0) dsd_pcm_rate = mix;
                }
            } else if (dop_locked > 0) {
                // exclusive, the device stayed below the DoP rate: below the usual 176.4 / 192 kHz, convert to the rate it
                // locked (the chain only goes up, so PCM above it could not play)
                const int dsd = dsdf->info().dsd_rate, usual = (dsd % 44100 == 0 ? 44100 : 48000) * 4;
                if (dop_locked < usual && dsd % (dop_locked * 8) == 0) {
                    dsd_pcm_rate = dop_locked;
                    event("converting at " + std::to_string(dop_locked) + " Hz, the rate the device locked");
                }
            }
            dsdf.reset();
            dsdenc.reset();
        }
    }
    if (started_dsd) {
        {
            std::lock_guard<std::mutex> l(m);
            start_done = true;
            start_ok = true;
        }
        cv.notify_all();
        DsdDirectStream st(*this);
        play(st);
        return;
    }
    dec = open_source(path, s, &e);
    if (dec) {
        rep.source = dec->info();
        ch = rep.source.channels;
        pcm.assign(static_cast<size_t>(kChunk) * ch, 0.0);
        hist.reset(ch);
    }
    const bool started = dec && open_and_start(static_cast<int64_t>(start_sec * rep.source.rate), &e);
    if (dec && !started && !cancel) device_fail = true;   // the file decoded: the device / chain failed
    {
        std::lock_guard<std::mutex> l(m);
        start_done = true;
        start_ok = started;
        start_err = e;
    }
    cv.notify_all();
    if (!started) {
        stop_cache_trim(true);
        done = true;
        return;
    }
    PcmStream st(*this);
    play(st);
}

void Player::Impl::play(Stream& st) {
    stream = &st;
    ok = true;
    try {
        loop(st);
    } catch (const std::exception& ex) {   // the device is stopped and the host told, as for any other failure
        ok = false;
        err = std::string("internal error: ") + ex.what();
    } catch (...) {
        ok = false;
        err = "internal error: unknown exception";
    }
    finish_playback();
    stream = nullptr;
}

void Player::Impl::loop(Stream& st) {
    // A pass that took long is logged with where the time went: the ring holds 0.5 s without a filter, so a longer
    // stall in the decoder (a disk that stops answering) is a gap the device plays. The time of a settings change
    // or a seek is shown but does not count: those pause or refill on purpose. The intended sleeps are left out.
    using clock = std::chrono::steady_clock;
    constexpr int64_t kStallMs = 250;
    auto lap_at = clock::now();
    auto lap = [&lap_at] {
        const auto now = clock::now();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - lap_at).count();
        lap_at = now;
        return static_cast<int64_t>(ms);
    };
    int64_t slow_seen = slow_read_count();
    while (!cancel) {
        lap();
        std::optional<std::pair<Settings, OutputChoice>> p;
        {
            std::lock_guard<std::mutex> l(m);
            p.swap(pending);
        }
        if (p && !st.apply(p->first, p->second)) {
            ok = false;
            break;
        }
        const int64_t t_apply = lap();
        if (!st.poll()) {
            ok = false;
            break;
        }
        const int64_t t_poll = lap();
        kick_prepare();
        if (!service_transport()) {
            ok = false;
            break;
        }
        const int64_t t_transport = lap();
        if (!io.eof && !st.produce()) queue_end();
        const int64_t t_decode = lap();
        const bool pushed = io.push();
        const int64_t t_push = lap();
        if (!pushed) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        else if (io.eof && io.pend_off >= io.pend) {
            if (io.dev->drained()) {
                ended_flag = true;
                if (cb.on_ended) cb.on_ended();
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        lap();
        pos = heard_position();
        pos_sec = rep.source.rate > 0 ? static_cast<double>(pos) / rep.source.rate : 0.0;
        if (cb.on_progress) cb.on_progress(pos, rep.source.frames);
        if (io.dev->failed()) break;
        if (!service_device()) {
            ok = false;
            break;
        }
        const int64_t t_upkeep = lap();
        if (t_poll + t_decode + t_push + t_upkeep >= kStallMs) {
            const int rate = rep.device.frame_rate();
            const double ring_ms = io.ring && rate > 0 ? io.ring->readable() / std::max(1, io.block) * 1000.0 / rate : 0.0;
            char b[256];
            std::snprintf(b, sizeof b,
                          "playback loop: a pass took %lld ms (decode %lld, poll %lld, push %lld, upkeep %lld; settings %lld, transport %lld)"
                          ", ring %.0f ms, underruns %lld",
                          static_cast<long long>(t_apply + t_poll + t_transport + t_decode + t_push + t_upkeep), static_cast<long long>(t_decode),
                          static_cast<long long>(t_poll), static_cast<long long>(t_push), static_cast<long long>(t_upkeep),
                          static_cast<long long>(t_apply), static_cast<long long>(t_transport), ring_ms,
                          static_cast<long long>(io.dev->stats().underruns.load()));
            event(b);
        }
        if (slow_read_count() != slow_seen) {   // the read ahead met a slow disk (file_bytes.h)
            const SlowRead r = last_slow_read();
            slow_seen = r.count;
            event("read ahead: a disk read of " + r.file + " took " + std::to_string(r.ms) + " ms (the decoder reads what was read ahead)");
        }
    }
}

bool Player::Impl::service_device() {
    switch (io.upkeep(report, rep.device.frame_rate(), &err)) {
        case OutputSession::Upkeep::Ok: return true;
        case OutputSession::Upkeep::Fail: return false;
        case OutputSession::Upkeep::Reopen: break;
    }
    return stream->reopen();
}

void Player::Impl::finish_playback() {
    queue.join_all();
    if (io.dev && !io.dev->failed() && dsdenc && !ended_flag) {
        // stopped (or failed) before the end: stop quickly, but on silence. DoP: PCM silence (the DAC
        // leaves DSD mode). Native DSD: DSD silence (0x69, what the feeder writes once the ring is
        // empty) reaches the DAC before the stream stops, not a cut in the middle of the music
        if (io.dev->paused() || cancel) {
            io.ring->clear();
            io.dev->set_paused(false);
        }
        io.pend = io.pend_off = 0;
        push_pcm_silence(200);
        io.eof = true;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!io.dev->drained() && !io.dev->failed() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (!dsdenc->dop() && io.dev->drained() && rep.device.frame_rate() > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(
                static_cast<int>(io.dev->latency_ms()) + 2 * io.dev->buffer_frames() * 1000 / rep.device.frame_rate() + 5));
    }
    if (ok && io.dev && ended_flag) {
        // the device buffer plays out (the ring has drained)
        if (!cancel)
            std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(io.dev->latency_ms()) +
                                                                  2 * io.dev->buffer_frames() * 1000 / rep.device.frame_rate() + 20));
    }
    if (build_cancel) *build_cancel = true;
    if (building.valid()) building.wait();
    take_warm();
    stop_cache_trim(true);
    if (io.dev) {
        io.dev->stop();
        rep.underruns = io.dev->stats().underruns;
        rep.event_timeouts = io.dev->stats().event_timeouts;
        rep.max_interval_us = io.dev->stats().max_interval_us;
        if (io.dev->failed()) {
            ok = false;
            err = io.dev->failure();
        }
        io.dev.reset();
    }
    if (!ok && !cancel && cb.on_failed) cb.on_failed(err);
    done = true;
}

}  // namespace daphne
