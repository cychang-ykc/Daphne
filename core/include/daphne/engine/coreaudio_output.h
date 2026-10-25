#pragma once
// Core Audio output (macOS): one HAL device, shared or exclusive (hog mode), N24 / N25.
//   - the HAL's IO callback reads a lock-free ring of device-format bytes (no lock, no allocation there)
//   - the ring holds 24-bit integers left-aligned in 32 bits (int24-in-32); the callback writes them
//     to the stream's virtual format, 32-bit float, which holds every 24-bit integer exactly. In
//     exclusive mode the DAC's physical format is an integer one of at least 24 bits, so the HAL's
//     float → integer conversion is exact too: PCM up to 24 bits and DoP reach the DAC unchanged
//   - exclusive: the device rate is set to the request when the device offers it; shared: the device
//     runs at its own rate (the system mixer's), as WASAPI shared. A request the player's chain cannot
//     reach by a whole factor from there (48 kHz on a 44.1 kHz device) is converted to the device rate
//     in the callback by Core Audio's converter, as Windows does for WASAPI shared
//
// A device held in hog mode is only changed while its IO runs: a rate change on a stopped device in hog
// mode, followed by releasing hog mode, leaves the process unable to start IO on it (AudioDeviceStart
// hangs for seconds; seen by MIKU's own Core Audio output). So open() starts the IO (silent) before it
// changes the rate or the physical format, and close() puts both back while the IO still runs, then
// stops it and releases hog mode.
//
// keep_output_devices (devices.h): close() in exclusive mode then keeps the device running, silent, in hog mode,
// and the next open() of the same device takes it over (the IO callback has no owner meanwhile).

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "daphne/engine/audio_output.h"

namespace daphne {

struct CoreAudioOptions {
    std::string device;      // device UID, list index ("0", "1", ...), or empty / "default" = the system output
    bool exclusive = true;   // hog mode, device rate = the request, DAC format with enough integer bits
    // IO buffer in ms (the HAL's buffer frame size), 0 = the device's default; clamped to the device's range.
    int buffer_ms = 0;
    // exclusive: when the device has no rate the chain can reach by a whole factor (48 kHz only for a 44.1 kHz track),
    // Core Audio's converter resamples to the device's rate (system_rate()). Shared mode always converts.
    bool convert = false;
};

class CoreAudioOutput final : public AudioOutput {
public:
    CoreAudioOutput();
    ~CoreAudioOutput() override;
    CoreAudioOutput(const CoreAudioOutput&) = delete;
    CoreAudioOutput& operator=(const CoreAudioOutput&) = delete;

    // Opens the device and starts its IO (silent until start()). Exclusive: the rate requested when the
    // device offers it, else the same family's highest rate below it, else the device's highest rate
    // (matches_request() false then), or the request with opt.convert when that rate is not a whole multiple
    // of it (the output converts). Shared: the device's current rate, or the request when the device rate is
    // not a whole multiple of it (the output converts).
    bool open(const CoreAudioOptions& opt, int rate, int channels, std::string* error);
    const DeviceFormat& format() const override { return fmt_; }
    bool matches_request() const override { return matched_; }
    std::string device_name() const override { return name_; }
    double latency_ms() const override { return latency_ms_; }
    int buffer_frames() const override { return buffer_frames_; }
    int system_rate() const override { return system_rate_; }
    std::vector<int> supported_rates() const override { return rates_; }

    bool start(ByteRing* ring, const std::atomic<bool>* eof, std::string* error) override;
    bool drained() const override { return drained_.load(); }
    void rearm() override { drained_ = false; }
    bool failed() const override { return failed_.load(); }
    std::string failure() const override;
    // Another application changed the device rate (exclusive: the player reopens it where it was).
    bool reset_requested() const override { return reset_.load(); }
    // Stops reading the ring (the device plays silence); close() releases the device.
    void stop() override;
    // Puts back what open() changed (rate, DAC format), stops the IO, releases hog mode; or keeps the device
    // (keep_output_devices, exclusive mode).
    void close();

    std::vector<std::string> take_notes() override;
    const OutputStats& stats() const override { return stats_; }

    struct Impl;   // the HAL side (coreaudio_output.cpp)

private:
    std::unique_ptr<Impl> impl_;
    DeviceFormat fmt_{};
    bool matched_ = false;
    std::string name_;
    double latency_ms_ = 0;
    int buffer_frames_ = 0;
    int system_rate_ = 0;
    std::vector<int> rates_;
    std::atomic<bool> drained_{false}, failed_{false}, reset_{false};
    OutputStats stats_;
};

}  // namespace daphne
