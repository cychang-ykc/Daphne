#pragma once
// A device output and a decoder for the player tests (PlayerDeps): no hardware, no files.
//
// FakeDevice is the "sound card": the formats it accepts and everything the player has sent to it,
// across reopens. Each Player::open of it creates a FakeOutput whose thread consumes the ring through
// the same RingFeeder as WASAPI / ASIO. It waits for a whole buffer before taking one (up to 200 ms),
// so a fast run records the stream without underruns: what the player wrote, sample for sample.

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "daphne/engine/audio_output.h"
#include "daphne/engine/decoder.h"
#include "daphne/engine/player.h"

namespace test {

struct FakeDevice {
    std::mutex m;
    std::vector<uint8_t> bytes;                      // every frame sent while playing (not paused, not drained)
    daphne::DeviceFormat fmt{};                      // the last format opened
    std::vector<daphne::OutputRequest> opens;        // every open, in order
    std::vector<size_t> open_at;                     // bytes.size() at each open
    std::function<bool(const daphne::OutputRequest&)> accept = [](const daphne::OutputRequest&) { return true; };
    daphne::SampleType type = daphne::SampleType::Int32;
    int container_bits = 32, valid_bits = 32;
    int buffer_frames = 512;
    double speed = 0;                                // 0: as fast as the player fills; else x real time
    int lock_rate = 0;                               // nonzero: every PCM open locks this rate, with a note why
    bool can_convert = false;                        // with lock_rate: an open with OutputRequest::convert resamples
                                                     // to it (format() the rate asked for, system_rate() lock_rate)
    int mixer_rate = 0;                              // nonzero: a shared mixer at this rate resamples every stream
    std::atomic<bool> request_reset{false};          // the "driver" asks to be reopened

    size_t frames() {
        std::lock_guard<std::mutex> l(m);
        return fmt.block_align() ? bytes.size() / fmt.block_align() : 0;
    }
    std::vector<uint8_t> take() {
        std::lock_guard<std::mutex> l(m);
        return bytes;
    }
    // the recorded stream as int32 samples (Int32 devices)
    std::vector<int32_t> samples32() {
        std::lock_guard<std::mutex> l(m);
        std::vector<int32_t> s(bytes.size() / 4);
        std::memcpy(s.data(), bytes.data(), s.size() * 4);
        return s;
    }
    std::vector<float> samples_f32() {
        std::lock_guard<std::mutex> l(m);
        std::vector<float> s(bytes.size() / 4);
        std::memcpy(s.data(), bytes.data(), s.size() * 4);
        return s;
    }
};

class FakeOutput final : public daphne::AudioOutput {
public:
    FakeOutput(std::shared_ptr<FakeDevice> d, const daphne::OutputRequest& r) : d_(std::move(d)) {
        fmt_.rate = r.rate;
        fmt_.channels = r.channels;
        if (r.dsd) {
            fmt_.type = daphne::SampleType::Dsd8Msb, fmt_.container_bits = fmt_.valid_bits = 8;
        } else {
            fmt_.type = d_->type, fmt_.container_bits = d_->container_bits, fmt_.valid_bits = d_->valid_bits;
            if (d_->mixer_rate > 0 && d_->mixer_rate != r.rate) {
                system_rate_ = d_->mixer_rate;
            } else if (d_->lock_rate > 0 && d_->lock_rate != r.rate) {
                if (r.convert && d_->can_convert) {
                    system_rate_ = d_->lock_rate;
                    notes_.push_back("fake: converts to " + std::to_string(d_->lock_rate) + " Hz");
                } else {
                    fmt_.rate = d_->lock_rate;
                    notes_.push_back("fake: stays at " + std::to_string(d_->lock_rate) + " Hz");
                }
            }
        }
        std::lock_guard<std::mutex> l(d_->m);
        d_->fmt = fmt_;
        d_->opens.push_back(r);
        d_->open_at.push_back(d_->bytes.size());
        d_->request_reset = false;
    }
    ~FakeOutput() override { stop(); }

    const daphne::DeviceFormat& format() const override { return fmt_; }
    bool matches_request() const override { return true; }
    std::string device_name() const override { return "fake"; }
    double latency_ms() const override { return 1000.0 * d_->buffer_frames / fmt_.rate; }
    int buffer_frames() const override { return d_->buffer_frames; }
    int system_rate() const override { return system_rate_; }

    bool start(daphne::ByteRing* ring, const std::atomic<bool>* eof, std::string*) override {
        ring_ = ring, eof_ = eof;
        drained_ = false, stop_ = false;
        feeder_.configure(fmt_);
        th_ = std::thread([this] { loop(); });
        return true;
    }
    bool drained() const override { return drained_; }
    void rearm() override { drained_ = false; }
    bool failed() const override { return false; }
    void stop() override {
        stop_ = true;
        if (th_.joinable()) th_.join();
    }
    const daphne::OutputStats& stats() const override { return stats_; }
    bool reset_requested() const override { return d_->request_reset; }
    std::vector<std::string> take_notes() override { return std::exchange(notes_, {}); }

private:
    int system_rate_ = 0;
    void loop() {
        const int n = d_->buffer_frames;
        const size_t need = static_cast<size_t>(n) * fmt_.block_align();
        std::vector<uint8_t> buf(need);
        // paced by the clock, not by sleep lengths (sleeps overshoot, more on some systems): buffer k is due at
        // start + k periods
        const auto period = std::chrono::duration<double>(d_->speed > 0 ? static_cast<double>(n) / fmt_.frame_rate() / d_->speed : 0.0);
        auto due = std::chrono::steady_clock::now();
        while (!stop_) {
            // a whole buffer of data (or the end, or a pause) before taking one
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
            while (!stop_ && !eof_->load() && !paused() && ring_->readable() < need && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            if (stop_) break;
            const bool was_paused = paused(), was_drained = drained_;
            if (feeder_.fill(buf.data(), n, ring_, eof_->load(), stats_)) drained_ = true;
            stats_.callbacks++;
            if (!(was_paused && paused()) && !was_drained) {
                std::lock_guard<std::mutex> l(d_->m);
                d_->bytes.insert(d_->bytes.end(), buf.begin(), buf.end());
            }
            if (d_->speed > 0) {
                due += std::chrono::duration_cast<std::chrono::steady_clock::duration>(period);
                std::this_thread::sleep_until(due);
            } else if (was_paused || was_drained) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }

    std::shared_ptr<FakeDevice> d_;
    daphne::DeviceFormat fmt_{};
    daphne::ByteRing* ring_ = nullptr;
    const std::atomic<bool>* eof_ = nullptr;
    std::atomic<bool> drained_{false}, stop_{false};
    std::thread th_;
    daphne::OutputStats stats_;
    std::vector<std::string> notes_;
};

inline daphne::OutputOpener fake_opener(std::shared_ptr<FakeDevice> d) {
    return [d](const daphne::OutputRequest& r, std::string* error) -> std::unique_ptr<daphne::AudioOutput> {
        if (!d->accept(r)) {
            if (error) *error = "fake: rate " + std::to_string(r.rate) + " refused";
            return nullptr;
        }
        return std::make_unique<FakeOutput>(d, r);
    };
}

// A source in memory: interleaved float64 at ±1.0, as a decoder delivers it.
struct Signal {
    daphne::AudioInfo info;
    std::vector<double> x;
};

class MemoryDecoder final : public daphne::Decoder {
public:
    explicit MemoryDecoder(std::shared_ptr<const Signal> s) : s_(std::move(s)) { info_ = s_->info; }
    bool open(const std::filesystem::path&, std::string*) override { return true; }
    void close() override {}
    int read(double* out, int frames) override {
        const int ch = info_.channels;
        const int64_t total = static_cast<int64_t>(s_->x.size()) / ch;
        const int n = static_cast<int>(std::min<int64_t>(frames, total - pos_));
        if (n <= 0) return 0;
        std::memcpy(out, s_->x.data() + pos_ * ch, static_cast<size_t>(n) * ch * sizeof(double));
        pos_ += n;
        return n;
    }
    bool seek(int64_t frame) override {
        pos_ = std::clamp<int64_t>(frame, 0, static_cast<int64_t>(s_->x.size()) / info_.channels);
        return true;
    }

private:
    std::shared_ptr<const Signal> s_;
};

}  // namespace test
