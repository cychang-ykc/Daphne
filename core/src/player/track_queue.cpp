#include "track_queue.h"

#include <chrono>

#include "common.h"
#include "daphne/dsp/progress.h"
#include "daphne/engine/file_bytes.h"

namespace daphne::playback {

void TrackQueue::set(const std::filesystem::path& path, std::optional<double> headroom_db) {
    std::lock_guard<std::mutex> l(m_);
    next_path_ = path;
    next_headroom_ = headroom_db;
}

std::pair<std::filesystem::path, std::optional<double>> TrackQueue::get() const {
    std::lock_guard<std::mutex> l(m_);
    return {next_path_, next_headroom_};
}

void TrackQueue::consumed(const std::filesystem::path& np) {
    std::lock_guard<std::mutex> l(m_);
    if (next_path_ == np) next_path_.clear(), next_headroom_.reset();
}

void TrackQueue::restore(const std::filesystem::path& path) {
    std::lock_guard<std::mutex> l(m_);
    if (next_path_.empty()) next_path_ = path;
}

std::string TrackQueue::chain_key(const Settings& cs, const DeviceFormat& cf) { return settings_to_json(cs).dump() + "|" + describe(cf); }

void TrackQueue::kick(const std::function<Context()>& make) {
    for (auto it = threads_.begin(); it != threads_.end();) {   // reap the finished ones
        if (it->first->done && it->first != prep_) {
            it->second.join();
            it = threads_.erase(it);
        } else {
            ++it;
        }
    }
    std::filesystem::path np;
    {
        std::lock_guard<std::mutex> l(m_);
        np = next_path_;
    }
    if (prep_ && prep_->path == np) return;
    drop();
    if (np.empty()) return;
    auto p = std::make_shared<Prep>();
    p->path = np;
    const Context c = make();
    p->key = chain_key(c.cs, c.cf);
    std::thread worker([p, c] {
        FileBytes::set_thread_cancel(&p->cancel);   // dropped: a read waiting for a stalled copy or disk returns
        std::string e;
        // opening may throw on a damaged file: the next track is then not prepared (not gapless), the process goes on
        if (c.dsd) {
            auto f = std::make_unique<DsdFile>();
            try {
                if (f->open(p->path, &e)) p->dsdf = std::move(f);
            } catch (...) {
            }
            p->opened = true;
            p->done = true;
            return;
        }
        std::unique_ptr<Decoder> d;
        try {
            d = c.opener(p->path, &e);
        } catch (...) {
        }
        const bool have = d != nullptr;
        const AudioInfo src = have ? d->info() : AudioInfo{};
        p->dec = std::move(d);   // take() may hand it over as soon as opened is set
        p->opened = true;
        if (have && !p->cancel) {
            const bool other_rate = src.rate != c.cur_rate && same_output(c.cs, src.rate, c.chain_rate);
            const bool other_format = src.rate == c.cur_rate && !DspChain::fits(c.shape, c.cs, src);
            if (src.channels == c.channels && (other_rate || other_format)) {
                try {
                    dsp::CancelScope cancel_scope(&p->cancel);
                    auto chain = std::make_unique<DspChain>();
                    std::string be;
                    if (chain->build(c.cs, src, c.cf, kChunk, &be)) p->chain = std::move(chain);
                } catch (...) {   // cancelled (or failed): no chain, the boundary is not gapless
                }
            }
        }
        p->done = true;
    });
    threads_.emplace_back(p, std::move(worker));
    prep_ = p;
}

void TrackQueue::drop() {
    if (prep_) prep_->cancel = true;
    prep_.reset();
}

std::shared_ptr<Prep> TrackQueue::take(const std::filesystem::path& np, const std::atomic<bool>& cancel) {
    // opened ahead (kick): wait for the open if it is still running
    std::shared_ptr<Prep> p = prep_ && prep_->path == np ? prep_ : nullptr;
    if (p) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!p->opened && !cancel && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        if (!p->opened) p.reset();
    }
    drop();   // consumed (or stale): a new next track starts a new one
    return p;
}

void TrackQueue::join_all() {
    drop();
    for (auto& [pp, worker] : threads_) {
        pp->cancel = true;
        worker.join();
    }
    threads_.clear();
}

}  // namespace daphne::playback
