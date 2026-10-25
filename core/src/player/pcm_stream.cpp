// Player::Impl, PCM (core.h): opening the device and the chain, producing chunks, the crossfade to a rebuilt chain.
#include "core.h"

#include "daphne/engine/live_source.h"

namespace daphne {

bool Player::Impl::open_and_start(int64_t from, std::string* error) {
    const AudioInfo& src = rep.source;
    // attempts: device rate, and for the dsd core the DSD multiple and transport
    struct Attempt {
        int rate, osr;
        bool native;
        bool convert = false;   // the output may resample to a rate the device has (OutputRequest::convert)
    };
    std::vector<Attempt> rates;
    if (s.core == CoreMode::Dsd) {
        if (caps().shared_mix) {
            if (error) *error = "DSD output (dsd core) needs WASAPI exclusive or ASIO: shared mode mixes everything to PCM";
            return false;
        }
        // ASIO: native DSD first (output.dsd auto), then DoP; a DSD rate the device refuses steps
        // down to DSD64. output.max_dsd caps the start: a driver may accept a rate its DAC cannot play
        int top = s.modulator.osr;
        if (s.output.max_dsd > 0 && top > s.output.max_dsd) {
            top = s.output.max_dsd;
            event("DSD" + std::to_string(s.modulator.osr) + " is above output.max_dsd: modulating to DSD" + std::to_string(top));
        }
        for (int osr = top; osr >= 64; osr /= 2) {
            if (caps().native_dsd && s.output.dsd != DsdMode::Dop) rates.push_back({dsd_clock(osr), osr, true});
            if (s.output.dsd != DsdMode::Native) rates.push_back({dop_rate(dsd_clock(osr)), osr, false});
        }
        if (rates.empty()) {
            if (error) *error = "output.dsd = native needs an ASIO driver with a DSD mode";
            return false;
        }
    } else {
        rates.push_back({requested_rate(s, src.rate), 0, false});
        if (s.core == CoreMode::Pcm)
            for (int r = rates[0].rate / 2; r >= src.rate && r % src.rate == 0; r /= 2) rates.push_back({r, 0, false});
    }
    // the device ran at a rate the chain could not use (a DAC without the track's rate family: 48 kHz only for
    // 44.1 kHz, or 96 kHz at most for 192 kHz): one more attempt at the source rate, resampled by the output to the
    // device's (Core Audio exclusive). Only after the device's own rates failed, so a DAC that steps down within the
    // family is never converted; a device that did not open at all (busy, no hog mode) is not tried again
    bool wrong_rate = false;
    want = requested_rate(s, src.rate);
    auto what = [&](size_t i) {
        return s.core == CoreMode::Dsd ? "DSD" + std::to_string(rates[i].osr) + (rates[i].native ? " (native)" : " (DoP)")
                                       : std::to_string(rates[i].rate) + " Hz" + (rates[i].convert ? " (resampled by the system)" : "");
    };
    for (size_t attempt = 0; attempt < rates.size(); ++attempt) {
        bool last = attempt + 1 == rates.size();
        std::string e;
        if (cancel) {   // stopped while starting (dc_stop during dc_load): no further rate is tried
            io.dev.reset();
            if (error) *error = "cancelled";
            return false;
        }
        // the last failure is the error; one before it goes to the log, so the host sees why the next was tried
        auto fail_attempt = [&]() {
            io.dev.reset();
            if (last && wrong_rate && caps().converts && s.core == CoreMode::Pcm && !rates[attempt].convert) {
                rates.push_back({src.rate, 0, false, true});
                last = false;
            }
            if (last && error) *error = e;
            // cancelled: the next round returns without trying the next rate, so the log does not say it does
            if (!last && !cancel) event("the output at " + what(attempt) + " failed (" + e + "): trying " + what(attempt + 1));
            return last;
        };
        dec->seek(from);
        in_total = from;
        flushed = false;
        tail_zeros = -1;
        io.eof = false;
        io.pend = io.pend_off = 0;
        xf.fading = false;
        xf.next.reset();
        xf.clear();
        take_warm();   // a crossfade warming up: the chain is built anew for the settings below
        cross_join.clear();
        io.dev = open_output({out, s.output.buffer_ms, rates[attempt].rate, src.channels, rates[attempt].native, rates[attempt].convert}, &e);
        if (!io.dev) {
            if (fail_attempt()) return false;
            continue;
        }
        if (io.dev->probe_before_design()) {
            // some drivers accept a rate they cannot stream: try it before designing the filter
            // (DoP: DoP silence, so the DAC does not see a stretch of PCM before the stream)
            ByteRing probe(1 << 12);
            std::atomic<bool> probe_eof{false};
            io.dev->set_dop(s.core == CoreMode::Dsd && !rates[attempt].native);
            if (!io.dev->start(&probe, &probe_eof, &e)) {
                if (fail_attempt()) return false;
                continue;
            }
            io.dev->stop();
        }
        rep.device = io.dev->format();
        rep.device_name = io.dev->device_name();
        rep.system_rate = io.dev->system_rate();
        if (rep.device.rate != rates[attempt].rate) wrong_rate = true;
        report.shared_mix = caps().shared_mix;
        rep.latency_ms = io.dev->latency_ms();
        io.note_buffer_fallback(report, s.output.buffer_ms);
        if (auto rates_ok = io.dev->supported_rates(); !rates_ok.empty()) report.set_asio_rates(std::move(rates_ok));
        chain_fmt = rep.device;
        dsdenc.reset();
        run_osr = 0;
        if (s.core == CoreMode::Dsd) {
            const int osr = rates[attempt].osr;
            run_osr = osr;
            const bool native = rates[attempt].native;
            if (rep.device.rate != (native ? dsd_clock(osr) : dop_rate(dsd_clock(osr))) || rep.system_rate > 0) {
                e = "the device locked " + std::to_string(rep.system_rate > 0 ? rep.system_rate : rep.device.rate) + " Hz, not the DSD" +
                    std::to_string(osr) + " rate";
                if (fail_attempt()) return false;
                continue;
            }
            chain_fmt.rate = modulator_input_rate(src.rate);
            chain_fmt.type = SampleType::Float64;
            chain_fmt.container_bits = chain_fmt.valid_bits = 64;
            dsdenc = std::make_unique<DsdEncoder>();
            if (!dsdenc->init(chain_fmt.rate, osr, s.modulator.order, rep.device, native ? DsdTransport::Native : DsdTransport::Dop, &e)) {
                dsdenc.reset();
                if (fail_attempt()) return false;
                continue;
            }
        }
        crate = chain_fmt.rate;
        if (want_fade_in) ramp_in_left = ramp_in_len = std::max<int64_t>(1, crate / 50);   // 20 ms
        // a filter to design before the first sound: the pcm core's resampler or the modulator's pre-filter
        // (the chain's condition). Only a real design reports its progress: a cached filter can take a moment to
        // load, and the host would show a design popup for it (with the text of the last design).
        bool designing = false;
        if (chain_fmt.rate != src.rate || s.filter.apodize) {
            const int to = chain_fmt.rate;
            designing = !filter_cached(s.filter, src.rate, to);
            if (designing)
                event("designing the filter for " + std::to_string(src.rate) + " -> " + std::to_string(to) +
                      " Hz (long filters can take minutes; the result is cached)");
        }
        cur = std::make_unique<DspChain>();
        bool built = false;
        {
            const Settings cs = chain_settings(s, run_osr);
            DspChain* c = cur.get();
            const DeviceFormat devf = chain_fmt;
            std::string be;
            auto job = std::async(std::launch::async, [&, c, cs, devf] {
                dsp::ProgressScope scope(&report.design_prog);
                dsp::CancelScope cancel_scope(&cancel);
                try {
                    return c->build(cs, src, devf, kChunk, &be);
                } catch (const dsp::DesignCancelled&) {
                    be = "cancelled";
                    return false;
                } catch (const std::exception& ex) {
                    be = std::string("the chain could not be built: ") + ex.what();
                    return false;
                }
            });
            while (job.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready)
                if (designing) report.design();
            built = job.get();
            if (!built) e = be;
            report.design(true);
        }
        if (!built) {   // never play at the wrong rate
            if (fail_attempt()) return false;
            continue;
        }
        ratio = std::max(1, crate / src.rate);
        throughput.reset();   // a new chain, a new speed
        tl.h_base = 0;
        dch = rep.device.channels;
        io.block = rep.device.block_align();
        tl.abs_out = from * ratio;
        skip_out = cur->content_delay();
        xf.set_channels(dch);
        io.ring = std::make_unique<ByteRing>(io.ring_bytes(rep.device.frame_rate()));
        dsp.assign(static_cast<size_t>(cur->max_output_frames()) * dch, 0.0);
        tmp_c = dsp;
        tmp_n = dsp;
        io.bytes.assign(dsdenc ? dsdenc->max_bytes(cur->max_output_frames()) : static_cast<size_t>(cur->max_output_frames()) * io.block, 0);
        // a live source is read 10 ms at a time into a short ring: its delay is the live decoder's jitter
        // buffer, not the player's (N4); a file reads whole chunks into about 0.5 s
        const bool live = is_live_path(cur_path);
        read_frames = live ? std::clamp(src.rate / 100, 64, kChunk) : kChunk;
        io.limit = live ? io.live_limit(rep.device.frame_rate()) : 0;
        // what the refill after a seek (a resume) takes from the live decoder at once: the ring up to its
        // limit plus a read, so the decoder holds its target once the ring is full (the prefill reads the
        // silence the decoder returns until it primes)
        if (live && rep.device.frame_rate() > 0 && io.block > 0)
            set_live_reserve(static_cast<double>(io.limit) / (static_cast<double>(io.block) * rep.device.frame_rate()) +
                             static_cast<double>(read_frames) / src.rate);
        const int read_out = static_cast<int>((static_cast<int64_t>(read_frames) * crate + src.rate - 1) / src.rate);   // chain frames per read
        io.chunk = !live ? 0 : dsdenc ? dsdenc->max_bytes(read_out) : static_cast<size_t>(read_out) * io.block;
        // DSD: silence first, so the DAC has switched to DSD and locked before the music (output.dsd_lead_in_ms)
        if (dsdenc) push_dsd_idle(s.output.dsd_lead_in_ms);
        while (!cancel && io.room() >= io.chunk_bytes()) {   // prefill
            if (!produce()) {
                io.eof = true;
                break;
            }
            io.push();
        }
        if (cancel) {   // stopped during the design or the prefill: the device is not started
            io.dev.reset();
            if (error) *error = "cancelled";
            return false;
        }
        io.dev->set_dop(dsdenc && dsdenc->dop());
        io.dev->set_paused(want_pause.load());
        if (!io.dev->start(io.ring.get(), &io.eof, &e)) {
            if (fail_attempt()) return false;
            continue;
        }
        break;
    }
    rep.chain = cur->stages();
    rep.filter = report.filter(s, dsdenc.get());
    report.bit_perfect(tl.heard_chain(cur.get()));
    if (cb.on_start) cb.on_start(rep);
    kick_cache_trim();
    return true;
}

void Player::Impl::finish_fade() {
    // the next chain becomes current; what it has produced beyond the mixed part plays next
    tl.abs_out += xf.delay_shift;
    rep.clipped += cur->take_clips();   // its last mixed block
    cur = xf.finish();
    throughput.reset();
    rep.chain = cur->stages();
    rep.filter = report.filter(s, dsdenc.get());
    report.bit_perfect(tl.heard_chain(cur.get()));
    event("now playing with filter: " + rep.filter + " | dsp: " + join(rep.chain));
    if (cb.on_changed) cb.on_changed(rep);
}

void Player::Impl::begin_crossfade(std::unique_ptr<DspChain> nx) {
    std::vector<double> in;
    Warm w = Crossfader::prepare(std::move(nx), hist, in_total, tl.abs_out, cur->content_delay(), ratio, dch, &in);
    warm_rounds = 0;   // (poll_warm counts the rounds)
    launch_warm(std::move(w), std::move(in));
}

// Up to 3 s of input through a long filter can take longer than the ring holds: the warm-up runs on a
// background thread while the current chain plays on (N4).
void Player::Impl::launch_warm(Warm w, std::vector<double> in) {
    ++warm_rounds;
    warm_last = static_cast<int64_t>(in.size() / static_cast<size_t>(ch));
    warm_cancel = std::make_shared<std::atomic<bool>>(false);
    warming = std::async(std::launch::async, [w = std::move(w), in = std::move(in), ch = ch, flag = warm_cancel]() mutable {
        PoolThreadScope pool;   // what this task sets on its thread is undone when it ends (common.h)
        platform::set_thread_priority(platform::Priority::BelowNormal);
        platform::set_fp_control(platform::with_flush_to_zero(platform::fp_control()));   // as on the processing thread
        w.cancel = flag.get();
        w.feed(in.data(), static_cast<int64_t>(in.size() / static_cast<size_t>(ch)), ch);
        w.cancel = nullptr;   // the flag lives with this task; a later round sets its own
        return std::move(w);
    });
}

void Player::Impl::poll_warm() {
    constexpr int kRounds = 4;
    if (!warming.valid() || warming.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    Warm w = warming.get();
    const int64_t fade = std::max<int64_t>(1, static_cast<int64_t>(crate * kFadeSeconds));
    std::vector<double> in;
    const int64_t left = in_total - w.in_done;   // decoded during the round
    if (!copy_history(hist, w.in_done, in_total, &in)) {
        // the history no longer holds it (a slow round at a high source rate): from the start, here
        w.chain->reset();
        xf.begin(std::move(w.chain), hist, in_total, tl.abs_out, cur->content_delay(), ratio, fade);
    } else if (left > kChunk && left < warm_last && warm_rounds < kRounds) {
        // another round while they get shorter (the background runs faster than the input comes)
        launch_warm(std::move(w), std::move(in));
        return;
    } else {
        // the rest here: at most a chunk, or (a chain slower than the input) at most what the last round had
        w.feed(in.data(), left, ch);
        xf.begin(std::move(w), tl.abs_out, cur->content_delay(), fade);
    }
    tmp_n.assign(static_cast<size_t>(xf.next->max_output_frames()) * dch, 0.0);
    tmp_c.assign(static_cast<size_t>(cur->max_output_frames()) * dch, 0.0);
    const size_t need = std::max(tmp_n.size(), tmp_c.size());
    if (dsp.size() < need) {
        dsp.resize(need);
        io.bytes.resize(dsdenc ? dsdenc->max_bytes(static_cast<int>(need / dch)) : need / dch * io.block);
    }
}

std::unique_ptr<DspChain> Player::Impl::take_warm() {
    if (!warming.valid()) return nullptr;
    if (warm_cancel) *warm_cancel = true;   // the round stops at its next chunk instead of running to the end
    return warming.get().chain;
}

bool Player::Impl::produce() {
    if (io.pend_off < io.pend) return true;
    // the chain's work on this chunk (Throughput): not the decoder, a gapless switch or the carried output
    Throughput::Clock::duration work{};
    bool carried = false;
    int n = 0;
    for (;;) {
        while (n == 0) {
            if (!xf.fading && xf.carry.frames() > 0) {
                n = std::min(xf.carry.frames(), static_cast<int>(dsp.size() / dch));
                std::copy(xf.carry.data(), xf.carry.data() + static_cast<size_t>(n) * dch, dsp.begin());
                xf.carry.pop(n);
                carried = true;
                break;
            }
            if (cross_join.active()) {
                // both chains' work (Throughput; it also reads the next track, a small overcount)
                const auto tj = Throughput::Clock::now();
                n = join_produce();
                work += Throughput::Clock::now() - tj;
                continue;   // (0: the join is over, its last frames are in the carry)
            }
            const int got = dec->read(pcm.data(), read_frames);
            const auto t0 = Throughput::Clock::now();
            if (got > 0) {
                hist.store(pcm.data(), got, in_total);
                in_total += got;
                if (!xf.fading) {
                    n = cur->process(pcm.data(), got, dsp.data());
                } else {
                    const int a = cur->process(pcm.data(), got, tmp_c.data());
                    xf.fc.push(tmp_c.data(), a);
                    const int b = xf.next->process(pcm.data(), got, tmp_n.data());
                    xf.push_next(tmp_n.data(), b);
                    n = xf.mix(dsp.data(), static_cast<int>(dsp.size() / dch), fq, rep.device, dsdenc != nullptr);
                    if (xf.pos >= xf.len) finish_fade();
                }
                work += Throughput::Clock::now() - t0;
            } else {
                if (!flushed && tail_zeros < 0 && try_gapless()) continue;   // the next track follows without a gap
                if (xf.fading) finish_fade();
                if (xf.carry.frames() > 0) continue;
                if (flushed) return false;
                // the stream ends: zeros until the last input frame is out of the filter (its content delay), or the
                // last delay's worth of music never leaves it
                if (tail_zeros < 0) tail_zeros = (static_cast<int64_t>(cur->content_delay()) + ratio - 1) / std::max(1, ratio);
                if (tail_zeros > 0) {
                    const int k = static_cast<int>(std::min<int64_t>(read_frames, tail_zeros));
                    std::fill(pcm.begin(), pcm.begin() + static_cast<std::ptrdiff_t>(k) * ch, 0.0);
                    const auto tz = Throughput::Clock::now();
                    n = cur->process(pcm.data(), k, dsp.data());
                    work += Throughput::Clock::now() - tz;
                    tail_zeros -= k;
                    continue;
                }
                flushed = true;
                const auto t1 = Throughput::Clock::now();
                n = cur->flush(dsp.data());
                work += Throughput::Clock::now() - t1;
                if (n == 0) return false;
            }
        }
        n = drop_start(n);   // the filter's delay at the start
        if (n > 0) break;
    }
    const auto t2 = Throughput::Clock::now();
    if (ramp_in_left > 0) {
        for (int i = 0; i < n && ramp_in_left > 0; ++i, --ramp_in_left) {
            const double g = static_cast<double>(ramp_in_len - ramp_in_left) / static_cast<double>(ramp_in_len);
            for (int c = 0; c < dch; ++c) dsp[static_cast<size_t>(i) * dch + c] *= g;
            if (g > 0.0) requantize(dsp.data() + static_cast<size_t>(i) * dch);
        }
    }
    if (fade_out_next) {
        fade_out_next = false;
        for (int i = 0; i < n; ++i) {
            const double g = i < fade_out_len ? 1.0 - static_cast<double>(i) / static_cast<double>(fade_out_len) : 0.0;
            for (int c = 0; c < dch; ++c) dsp[static_cast<size_t>(i) * dch + c] *= g;
            if (g > 0.0 && g < 1.0) requantize(dsp.data() + static_cast<size_t>(i) * dch);
        }
    }
    if (dsdenc) {
        dsd_bytes.clear();
        dsdenc->encode(dsp.data(), n, dsd_bytes);
        if (io.bytes.size() < dsd_bytes.size()) io.bytes.resize(dsd_bytes.size());
        std::copy(dsd_bytes.begin(), dsd_bytes.end(), io.bytes.begin());
        io.pend = dsd_bytes.size();
    } else {
        rep.clipped += convert_to_device(dsp.data(), static_cast<size_t>(n) * dch, rep.device, io.bytes.data());
        rep.clipped += cur->take_clips() + (xf.next ? xf.next->take_clips() : 0);   // clamped by the final quantizer
        io.pend = static_cast<size_t>(n) * io.block;
    }
    work += Throughput::Clock::now() - t2;
    throughput.add(work, carried || crate <= 0 ? 0.0 : static_cast<double>(n) / crate);
    rep.realtime_factor = throughput.factor();
    rt_factor.store(rep.realtime_factor, std::memory_order_relaxed);
    io.pend_off = 0;
    tl.abs_out += n;
    rep.frames_played += n;
    return true;
}

int Player::Impl::drop_start(int n) {
    if (skip_out <= 0 || n <= 0) return n;
    const int d = static_cast<int>(std::min<int64_t>(skip_out, n));
    std::copy(dsp.begin() + static_cast<std::ptrdiff_t>(d) * dch, dsp.begin() + static_cast<std::ptrdiff_t>(n) * dch, dsp.begin());
    skip_out -= d;
    tl.abs_out += d;
    return n - d;
}

std::unique_ptr<Decoder> Player::Impl::open_source(const std::filesystem::path& file, const Settings& st, std::string* e) {
    if (!is_dsd_path(file)) return deps.open_decoder(file, e);
    auto d = make_dsd_pcm_decoder(dsd_pcm_rate, st.output.dsd_pcm_lowpass);
    if (!d->open(file, e)) d.reset();
    return d;
}

}  // namespace daphne
