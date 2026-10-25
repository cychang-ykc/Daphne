#pragma once
// WASAPI output (exclusive event-driven, or shared).
//   - the render thread reads a lock-free ring of device-format bytes (no lock in the render path)
//   - the ring already holds device-format integers converted with 2^(N-1) scaling
//   - the caller sees whether the locked format differs from the request

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

#include "daphne/engine/audio_output.h"

namespace daphne {

struct WasapiOptions {
    std::string device;      // endpoint ID, list index ("0", "1", ...) or empty = default
    bool exclusive = true;
    // Device buffer in ms, 0 = default (exclusive: the device period, at least 25 ms; shared: 100 ms),
    // else at least 10 ms and the device's minimum period.
    // Exclusive: a buffer the device does not accept falls back to the default (buffer_fallback()).
    int buffer_ms = 0;
};

class WasapiOutput final : public AudioOutput {
public:
    WasapiOutput();
    ~WasapiOutput() override;
    WasapiOutput(const WasapiOutput&) = delete;
    WasapiOutput& operator=(const WasapiOutput&) = delete;

    // Negotiate and initialize. Exclusive search order: rate requested, the same family halved,
    // then 192k / 96k / 48k / 44.1k; type int32, 24-in-32, float32, packed24, int16.
    // Shared: the mixer format. Does not start playback.
    bool open(const WasapiOptions& opt, int rate, int channels, std::string* error);
    const DeviceFormat& format() const override { return fmt_; }
    bool matches_request() const override { return matched_; }
    std::string device_name() const override { return name_; }
    double latency_ms() const override { return latency_ms_; }
    int buffer_frames() const override { return buffer_frames_; }
    int system_rate() const override { return system_rate_; }
    // The buffer asked for (WasapiOptions::buffer_ms) failed to initialize; the default is used.
    bool buffer_fallback() const override { return buffer_fallback_; }

    // Start the render thread reading from ring. When eof is set and the ring runs dry,
    // drained() becomes true (the device keeps getting silence until stop()).
    bool start(ByteRing* ring, const std::atomic<bool>* eof, std::string* error) override;
    bool drained() const override { return drained_.load(); }
    void rearm() override { drained_ = false; }
    bool failed() const override { return failed_.load(); }
    void stop() override;
    void close();

    const OutputStats& stats() const override { return stats_; }

private:
    void render_exclusive();
    void render_shared();

    struct Impl;
    Impl* impl_;
    DeviceFormat fmt_{};
    bool matched_ = false;
    bool exclusive_ = true;
    std::string name_;
    double latency_ms_ = 0;
    int buffer_frames_ = 0;
    int system_rate_ = 0;   // shared mode at another rate than the mixer's: the mixer's (Windows converts)
    bool buffer_fallback_ = false;
    ByteRing* ring_ = nullptr;
    const std::atomic<bool>* eof_ = nullptr;
    std::atomic<bool> drained_{false};
    std::atomic<bool> failed_{false};
    std::thread thread_;
    OutputStats stats_;
};

}  // namespace daphne
