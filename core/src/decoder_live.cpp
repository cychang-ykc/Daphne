// Live input decoder (live_source.h).
#include "daphne/engine/live_source.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

namespace daphne {

namespace {

// Seconds of buffered input aimed for, adapting to the producer (N2). The YouTube Music page (MIKU) delivers
// 2048-frame blocks every 40 or 60 ms, measured at most 109 ms behind its average pace: 0.15 s covers that,
// the 10 ms reads and a margin. A busy page stalls longer now and then (304 ms measured): each starve raises
// the target by 50 ms, up to 0.30 s, and 60 s without one lowers it by 25 ms, then every 30 s, back to 0.15 s.
// What it learns is kept for the process: the next live source (the next YouTube Music session) starts there.
// After a starve it waits until it holds the target again before playing on: one gap, not a run of them.
constexpr double kTargetMin = 0.15;
constexpr double kTargetMax = 0.30;
constexpr double kTargetUp = 0.05;     // per starve (once for a whole stall, not per silence returned)
constexpr double kTargetDown = 0.025;
constexpr double kQuietFirst = 60.0;   // seconds of audio without a starve before the first step down
constexpr double kQuietNext = 30.0;    // and between the next ones
constexpr double kOverflow = 1.2;    // beyond this something stalled: restart near the writer
constexpr int kWaitMs = 20;          // running: how long to wait for the producer before a starve

// The ring's write index, written by a producer outside this program: an atomic load (acquire) of plain memory.
// std::atomic_ref where the library has it; Apple's libc++ does not yet, Clang's builtin is the same operation.
inline uint32_t load_acquire(uint32_t* p) {
#ifdef __cpp_lib_atomic_ref
    return std::atomic_ref<uint32_t>(*p).load(std::memory_order_acquire);
#else
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
#endif
}

std::atomic<uint8_t*> g_base{nullptr};
std::atomic<int> g_cap{0};
std::atomic<int> g_readers{0};   // reads inside the ring right now (set_live_bus waits for them to leave)
std::atomic<uint32_t> g_gen{0};  // one more per set_live_bus: a decoder that sees it change syncs to the new writer
std::atomic<int> g_rate{48000};
std::atomic<double> g_reserve{0};   // set_live_reserve

// stats of the open decoder (one live decoder at a time)
std::atomic<double> g_fill{0}, g_read{0}, g_adjust{0}, g_target{kTargetMin};
std::atomic<int64_t> g_resyncs{0}, g_starves{0};
std::atomic<int> g_open{0};

double hermite(double xm1, double x0, double x1, double x2, double t) {
    const double c1 = 0.5 * (x1 - xm1);
    const double c2 = xm1 - 2.5 * x0 + 2 * x1 - 0.5 * x2;
    const double c3 = 0.5 * (x2 - xm1) + 1.5 * (x0 - x1);
    return ((c3 * t + c2) * t + c1) * t + x0;
}

class LiveDecoder final : public Decoder {
public:
    ~LiveDecoder() override { close(); }

    bool open(const std::filesystem::path&, std::string* error) override {
        if (!g_base.load() || g_cap.load() <= 0) {
            if (error) *error = "live input: the host has not shared its ring";
            return false;
        }
        info_ = {};
        info_.rate = g_rate.load();
        info_.channels = 2;
        info_.codec = "Live";
        info_.frames = -1;
        pos_ = 0;
        synced_ = primed_ = jump_ = starving_ = false;
        target_ = g_target.load(), quiet_ = 0;   // what the last live source learned
        if (!open_) g_open++;
        open_ = true;
        g_resyncs = 0, g_starves = 0, g_read = 0;
        return true;
    }

    void close() override {
        if (open_) g_open--;
        open_ = false;
    }

    // A stream has nowhere to seek to: the position counter moves, and the reader goes back to the writer. The
    // player seeks a live source when it resumes after a pause: what the producer wrote meanwhile (it does not
    // stop) would otherwise be played late, and the rate trim takes minutes to work off half a second.
    bool seek(int64_t frame) override {
        pos_ = std::max<int64_t>(0, frame);
        jump_ = true;
        return true;
    }

    int read(double* out, int frames) override {
        if (!open_ || frames <= 0) return 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kWaitMs);
        for (;;) {
            const int n = produce(out, frames);
            if (n > 0) {
                starving_ = false;
                quiet(n);
                return advance(n);
            }
            if (!primed_ || std::chrono::steady_clock::now() >= deadline) {
                // nothing yet (prefill) or the producer fell behind: silence, so the player does not block
                const int s = std::min(frames, primed_ ? std::max(1, info_.rate / 200) : frames);
                std::memset(out, 0, static_cast<size_t>(s) * 2 * sizeof(double));
                if (primed_) {
                    // ran dry: more buffer from now on, and silence until it holds that much again (produce primes
                    // at the target, the player's refill on top, and skips what came in beyond it)
                    g_starves++;
                    starving_ = true;
                    target_ = std::min(kTargetMax, target_ + kTargetUp);
                    g_target = target_;
                    quiet_ = 0;
                    primed_ = false;
                }
                return advance(s);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

private:
    // audio read without a starve: after long enough, the target steps back down
    void quiet(int n) {
        if (target_ <= kTargetMin) return;
        quiet_ += n;
        if (quiet_ < static_cast<int64_t>(kQuietFirst * info_.rate)) return;
        target_ = std::max(kTargetMin, target_ - kTargetDown);
        g_target = target_;
        quiet_ = static_cast<int64_t>((kQuietFirst - kQuietNext) * info_.rate);   // the next step after kQuietNext
    }

    int advance(int n) {
        pos_ += n;
        g_read = static_cast<double>(pos_) / info_.rate;
        return n;
    }

    // Frames interpolated from the ring, or 0 when there is nothing to read now. The ring is only touched in
    // here, under a lease: set_live_bus clears g_base and then waits for g_readers to reach 0 before the host
    // may free the memory (both sides sequentially consistent: either this sees the cleared pointer, or the
    // host sees this reader and waits).
    int produce(double* out, int frames) {
        g_readers.fetch_add(1);
        uint8_t* base = g_base.load();
        // another ring (set_live_bus): its writer is not where the old one was, and what lies between the old
        // read position and it may never have been written
        const uint32_t gen = g_gen.load();
        if (base && gen != gen_) gen_ = gen, synced_ = false;
        const int n = base ? produce_from(base, out, frames) : 0;
        g_readers.fetch_sub(1, std::memory_order_release);
        return n;
    }

    int produce_from(uint8_t* base, double* out, int frames) {
        const uint32_t cap = static_cast<uint32_t>(g_cap.load());
        if (cap == 0) return 0;
        // the producer writes the frames, then the index (release): read as an atomic, the frames up to it are there
        const uint32_t w = load_acquire(reinterpret_cast<uint32_t*>(base));
        const float* ring = reinterpret_cast<const float*>(base + 16);
        const int rate = g_rate.load();

        // how far the writer is ahead, modulo 2^32 (the index wraps there: about 27 h at 44.1 kHz), and the margin
        // short of a full ring that counts as lapped (signed: a ring under 4096 frames would wrap the unsigned one)
        const int64_t ahead = static_cast<int32_t>(w - read_);
        const int64_t margin = std::min<int64_t>(4096, cap / 2);
        if (!synced_ || ahead < 0 || ahead > static_cast<int64_t>(cap) - margin) {
            // first read, page reloaded (the writer went back), or the writer lapped us: restart close to the writer
            if (synced_) g_resyncs++;
            read_ = w, frac_ = 0, synced_ = true, primed_ = false, adj_ = 0;
            return 0;
        }
        int avail = static_cast<int>(w - read_);
        // Priming (the start, after a starve) leaves the target behind the writer, and a seek (a resume) the
        // target plus what the player's refill takes at once: more (a burst from the producer, or what it
        // wrote while the player was paused) is skipped instead of played late and worked off by the rate trim
        // over minutes. Unprimed, the decoder returned silence at once, so the player's ring is full when it
        // primes and only a seek finds it empty (the reserve on top would stay in the decoder).
        const int keep = static_cast<int>(target_ * rate);
        int behind = 0;   // how far behind the writer to go now (0: stay)
        if (!primed_) {
            if (avail < keep) return 0;
            primed_ = true;
            jump_ = false;   // a seek while unprimed: the player's ring has been refilled with silence
            behind = keep;
        } else if (jump_) {
            jump_ = false;
            behind = static_cast<int>((target_ + g_reserve.load()) * rate);
        }
        if (behind > 0 && avail > behind) read_ = w - static_cast<uint32_t>(behind), frac_ = 0, adj_ = 0, avail = behind;
        const double fill = static_cast<double>(avail) / rate;
        g_fill = fill;
        if (fill > kOverflow) {
            g_resyncs++;
            read_ = w - static_cast<uint32_t>(target_ * rate), frac_ = 0, adj_ = 0;
            avail = static_cast<int>(w - read_);
        }
        if (avail < 4) return 0;

        // proportional control of the buffer level through a tiny rate change
        const double want = std::clamp((fill - target_) * 0.02, -0.005, 0.005);
        adj_ += (want - adj_) * 0.02;
        g_adjust = adj_;
        const double step = static_cast<double>(rate) / info_.rate * (1 + adj_);

        int produced = 0;
        while (produced < frames) {
            const int i0 = static_cast<int>(frac_);
            if (i0 + 2 >= avail) break;
            const double t = frac_ - i0;
            const uint32_t b = read_ + static_cast<uint32_t>(i0);
            const size_t pm = ((b - 1) % cap) * 2, p0 = (b % cap) * 2, p1 = ((b + 1) % cap) * 2, p2 = ((b + 2) % cap) * 2;
            out[2 * produced] = hermite(ring[pm], ring[p0], ring[p1], ring[p2], t);
            out[2 * produced + 1] = hermite(ring[pm + 1], ring[p0 + 1], ring[p1 + 1], ring[p2 + 1], t);
            ++produced;
            frac_ += step;
        }
        const int whole = static_cast<int>(frac_);
        read_ += static_cast<uint32_t>(whole);
        frac_ -= whole;
        return produced;
    }

    bool open_ = false;
    uint32_t read_ = 0;   // integer read position in the ring (frames, wraps like the writer's)
    uint32_t gen_ = 0;    // g_gen of the ring read_ belongs to
    double frac_ = 0;     // between read_ and read_ + 1
    bool synced_ = false, primed_ = false;
    bool jump_ = false;   // seek(): back to the writer at the next read
    bool starving_ = false;   // ran dry, waiting to hold the target again
    double target_ = kTargetMin;
    int64_t quiet_ = 0;   // source frames read since the last starve or step down
    double adj_ = 0;      // smoothed rate correction
};

}  // namespace

void set_live_bus(void* base, int cap_frames, int rate) {
    // off the old ring first, and no read left in it, before the new one (with its capacity) is published
    g_base.store(nullptr);
    // seq_cst, not acquire: with an acquire load Apple clang (-mcpu=apple-m1) emits ldapr, which ARMv8.3 lets
    // pass the stlr above; this could then see no reader while a reader still holds the old pointer
    while (g_readers.load() != 0) std::this_thread::yield();
    g_cap = base ? cap_frames : 0;
    if (rate > 8000) g_rate = rate;
    g_gen.fetch_add(1);   // before the pointer: a reader that gets the new ring sees the new generation
    g_base.store(static_cast<uint8_t*>(base));
}

bool live_connect(const std::string&, int, std::string* error) {
    if (error) *error = "live input over a Unix socket is not implemented yet";
    return false;
}

void live_disconnect() {}

void set_live_rate(int rate) {
    if (rate > 8000) g_rate = rate;
}

void set_live_reserve(double seconds) { g_reserve = std::clamp(seconds, 0.0, 1.0); }

void live_forget() { g_target = kTargetMin; }

LiveStats live_stats() {
    LiveStats s;
    s.fill = g_fill.load();
    s.read = g_read.load();
    s.resyncs = g_resyncs.load();
    s.starves = g_starves.load();
    s.adjust = g_adjust.load();
    s.target = g_target.load();
    s.active = g_open.load() > 0;
    return s;
}

std::unique_ptr<Decoder> make_live_decoder() { return std::make_unique<LiveDecoder>(); }

}  // namespace daphne
