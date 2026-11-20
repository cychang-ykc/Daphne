// Player::Impl, the next track (core.h): its preparation, the decoder swap, the join through a new chain, its headroom.
#include "core.h"

namespace daphne {

void Player::Impl::kick_prepare() {
    queue.kick([this] {
        TrackQueue::Context c;
        c.dsd = dsdf != nullptr;
        c.cs = chain_settings(s, run_osr);
        c.cf = chain_fmt;
        c.chain_rate = crate;
        c.channels = ch;
        c.cur_rate = decoding_info().rate;
        c.shape = cur ? cur->shape() : DspChain::Shape{};
        c.opener = deps.open_decoder;
        return c;
    });
}

void Player::Impl::cross_switch(std::unique_ptr<Decoder> nd, std::unique_ptr<DspChain> nc, const std::filesystem::path& np) {
    const size_t need = static_cast<size_t>(std::max(cur->max_output_frames(), nc->max_output_frames())) * dch;
    if (dsp.size() < need) dsp.resize(need);
    if (tmp_c.size() < need) tmp_c.resize(need);
    if (tmp_n.size() < need) tmp_n.resize(need);
    const size_t nb = dsdenc ? dsdenc->max_bytes(static_cast<int>(need / dch)) : need / dch * io.block;
    if (io.bytes.size() < nb) io.bytes.resize(nb);

    // the join is made by join_produce, a chunk at a time: A's tail (2 x its delay of zeros, then the flush)
    // with B through the new chain on top. New-chain frame j lands on join frame j + off, so that B's first
    // sample (new-chain frame dn) is heard right where A's frame K would have been (a_end)
    const int64_t start = tl.abs_out + xf.carry.frames();   // device frame where the join begins
    const int64_t a_end = tl.latest_base() + in_total * ratio + cur->content_delay();
    const int dn = nc->content_delay();
    const int64_t off = (a_end - start) - dn;
    cross_join.clear();
    cross_join.old = cur.get();
    cross_join.zeros_left = 2 * static_cast<int64_t>(cur->content_delay()) / std::max(1, ratio) + 1;
    cross_join.a.ch = cross_join.b.ch = dch;
    cross_join.b_from = std::max<int64_t>(0, off);
    cross_join.skip_b = std::max<int64_t>(0, -off);
    in_total = 0;
    hist.valid_from = 0;

    Switch w;
    w.info = nd->info();
    w.path = np;
    w.cross = true;
    w.at_out = a_end - dn;   // B's frame 0 is heard at at_out + new_delay = a_end
    w.new_delay = dn;
    w.old_ratio = ratio;
    w.old_delay = cur->content_delay();
    w.old_base = tl.latest_base();
    w.old_chain = std::move(cur);
    tl.switches.push_back(std::move(w));
    cur = std::move(nc);
    dec = std::move(nd);
    ratio = std::max(1, crate / dec->info().rate);
    throughput.reset();
}

int Player::Impl::join_produce() {
    Join& j = cross_join;
    const int room = static_cast<int>(dsp.size() / dch);
    for (;;) {
        if (!j.old_done && j.a.frames() == 0) {   // more of A's tail
            int o = 0;
            if (j.zeros_left > 0) {
                const int k = static_cast<int>(std::min<int64_t>(kChunk, j.zeros_left));
                std::fill(pcm.begin(), pcm.begin() + static_cast<std::ptrdiff_t>(k) * ch, 0.0);
                o = j.old->process(pcm.data(), k, tmp_c.data());
                j.zeros_left -= k;
            } else {
                o = j.old->flush(tmp_c.data());
                j.old_done = true;
            }
            j.a.push(tmp_c.data(), o);
            continue;
        }
        const bool a_over = j.old_done && j.a.frames() == 0;
        if (j.t < j.b_from) {   // before B: the tail alone (zeros if it is shorter)
            const int n = static_cast<int>(std::min<int64_t>({room, j.b_from - j.t, a_over ? INT64_MAX : j.a.frames()}));
            if (a_over) std::fill(dsp.begin(), dsp.begin() + static_cast<std::ptrdiff_t>(n) * dch, 0.0);
            else std::copy(j.a.data(), j.a.data() + static_cast<size_t>(n) * dch, dsp.begin()), j.a.pop(n);
            j.t += n;
            return n;
        }
        if (a_over) {   // the tail is out: what the new chain made beyond it plays next, then the normal path
            if (j.b.frames() > 0) xf.carry.push(j.b.data(), j.b.frames());
            rep.clipped += j.old->take_clips();
            j.clear();
            return 0;
        }
        if (j.b.frames() == 0 && !j.b_ended) {   // more of B through the new chain
            const int got = dec->read(pcm.data(), kChunk);
            if (got <= 0) {
                j.b_ended = true;   // B is shorter than the tail: its flush follows the join (the normal path)
                continue;
            }
            hist.store(pcm.data(), got, in_total);
            in_total += got;
            const int o = cur->process(pcm.data(), got, tmp_n.data());
            const int d = static_cast<int>(std::min<int64_t>(j.skip_b, o));
            j.skip_b -= d;
            j.b.push(tmp_n.data() + static_cast<size_t>(d) * dch, o - d);
            continue;
        }
        if (j.b.frames() == 0) {   // B ended: the rest of the tail alone
            const int n = std::min(room, j.a.frames());
            std::copy(j.a.data(), j.a.data() + static_cast<size_t>(n) * dch, dsp.begin());
            j.a.pop(n);
            j.t += n;
            return n;
        }
        const int n = std::min({room, j.a.frames(), j.b.frames()});
        const double* x = j.a.data();
        const double* y = j.b.data();
        for (size_t i = 0; i < static_cast<size_t>(n) * dch; ++i) dsp[i] = x[i] + y[i];
        j.a.pop(n);
        j.b.pop(n);
        j.t += n;
        return n;
    }
}

void Player::Impl::apply_headroom(std::optional<double> h) {
    if (!h || tl.switches.empty() || *h == s.headroom_db) return;
    tl.switches.back().old_headroom = s.headroom_db;
    Settings ns = s;
    ns.headroom_db = *h;
    if (cur->update(chain_settings(ns, run_osr))) {
        s = ns;
        return;
    }
    // the chain's structure changes (e.g. bit-perfect -> a quantizer): an ordinary settings change, a little late
    std::lock_guard<std::mutex> l(m);
    if (!pending) pending = std::make_pair(ns, out);
}

void Player::Impl::undo_headroom() {
    for (const auto& w : tl.switches)
        if (w.old_headroom) {
            Settings ns = s;
            ns.headroom_db = *w.old_headroom;
            if (cur && cur->update(chain_settings(ns, run_osr))) s = ns;
            break;
        }
}

void Player::Impl::undo_cross() {
    for (auto& w : tl.switches)
        if (w.cross) {
            cur = std::move(w.old_chain);
            ratio = w.old_ratio;
            break;
        }
}

bool Player::Impl::try_gapless() {
    const auto [np, nh] = queue.get();
    if (np.empty() || is_dsd_path(np) != (dsdf != nullptr)) return false;
    if (remodulating() && !is_dsd_path(np)) return false;   // a PCM file after a remodulated DSD file: its own start
    const std::shared_ptr<Prep> p = queue.take(np, cancel);   // opened ahead (kick_prepare)
    std::string e;
    // a next track that cannot be opened (a damaged file, also one that throws) is not gapless: this one ends and the
    // host goes on; why goes to the log
    auto not_opened = [&](const std::string& why) {
        event("the next track could not be opened (" + (why.empty() ? std::string("unknown error") : why) +
              "): this one ends here, not gapless");
        return false;
    };
    if (dsdf) {
        std::unique_ptr<DsdFile> f = p ? std::move(p->dsdf) : nullptr;
        if (!f) {
            f = std::make_unique<DsdFile>();
            try {
                if (!f->open(np, &e)) return not_opened(e);
            } catch (const std::exception& ex) {
                return not_opened(ex.what());
            }
        }
        if (f->info().dsd_rate != dsdf->info().dsd_rate || f->info().channels != dsdf->info().channels) return false;
        AudioInfo ai{};
        ai.rate = f->info().dsd_rate / 8;
        ai.channels = f->info().channels;
        ai.bits = 1;
        ai.lossless_int = true;
        ai.frames = f->info().frames;
        ai.channel_mask = f->info().channel_mask;
        ai.codec = f->info().codec;
        ai.dsd_rate = f->info().dsd_rate;
        Switch w;
        w.at_in = tl.abs_out;
        w.info = ai;
        w.path = np;
        tl.switches.push_back(std::move(w));
        dsdf = std::move(f);
    } else {
        std::unique_ptr<Decoder> nd = p ? std::move(p->dec) : nullptr;
        if (!nd) {
            try {
                nd = deps.open_decoder(np, &e);
            } catch (const std::exception& ex) {
                return not_opened(ex.what());
            }
            if (!nd) return not_opened(e);
        }
        if (nd->info().channels != ch) return false;
        const bool fits = cur->fits(chain_settings(s, run_osr), nd->info());
        if (fits) {
            Switch w;
            w.at_in = in_total;
            w.info = nd->info();
            w.path = np;
            tl.switches.push_back(std::move(w));
            dec = std::move(nd);
            apply_headroom(nh);
        } else {
            // another sample rate, or the same rate with a format the current chain does not fit (a 24-bit
            // track after a 16-bit one on a 16-bit device needs the quantizer): gapless only on the same
            // output, joined through the new chain built ahead for the current settings; otherwise the track
            // ends and the host loads the next one
            if (!p || !p->done || !p->chain || xf.fading || building.valid() || warming.valid() || cross_join.active() || p->key != TrackQueue::chain_key(chain_settings(s, run_osr), chain_fmt))
                return false;
            cross_switch(std::move(nd), std::move(p->chain), np);
            apply_headroom(nh);
        }
    }
    queue.consumed(np);
    return true;
}

}  // namespace daphne
