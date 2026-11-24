#pragma once
// The audio callback's side of the ring (WASAPI render thread, ASIO bufferSwitch): fills device
// buffers from the ByteRing and adds what has to happen at the device's pace:
//
//   pause / resume  the device keeps running and the queued audio stays in the ring, so pausing
//                   is immediate and the position exact. PCM fades out / in over 20 ms; DSD has no
//                   fade: native DSD sends DSD silence (0x69), DoP sends DoP silence that continues
//                   the marker sequence (and keeps its parity, so the next real frame still
//                   alternates), so the DAC stays locked in DSD mode.
//   underrun        zeros (PCM), 0x69 (native DSD), DoP silence (DoP, before the end of the
//                   stream; the stream itself ends with PCM silence so the DAC leaves DSD mode)
//   level meter     peaks of what is being heard (PCM formats), reset when read
//   fade samples    integer formats are rounded with TPDF dither (the fade changes the level after
//                   the final quantizer, N21)
//
// Everything here runs on the audio thread except the setters / meter(), which are atomic.

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "daphne/engine/ring_buffer.h"
#include "daphne/engine/sample_format.h"

namespace daphne {

struct OutputStats;

class RingFeeder {
public:
    void configure(const DeviceFormat& f);
    void reset();   // a new stream (ring and stats restart)

    // Fills `frames` device frames at dst (little-endian device layout). Returns true when the
    // ring ran dry with eof set (the stream has drained).
    bool fill(uint8_t* dst, int frames, ByteRing* ring, bool eof, OutputStats& stats);

    void set_paused(bool p) { want_pause_.store(p, std::memory_order_release); }
    bool paused() const { return state_.load(std::memory_order_acquire) == kPaused; }
    // The stream carries DoP (the player sets it): pausing sends DoP silence.
    void set_dop(bool d) { dop_.store(d, std::memory_order_release); }
    bool dop() const { return dop_.load(std::memory_order_acquire); }

    // Peak (0..1, first two channels) since the last call.
    void meter(double& l, double& r);

private:
    enum { kPlaying, kFadingOut, kPaused, kFadingIn };
    bool fill_from(uint8_t* dst, int frames, ByteRing* ring, bool eof, OutputStats& stats);
    void silence(uint8_t* dst, int frames, bool dop);
    double read_sample(const uint8_t* p) const;
    void write_sample(uint8_t* p, double v);
    double tpdf();
    void put_dop_idle(uint8_t* frame);
    uint8_t marker_of(const uint8_t* frame) const;

    DeviceFormat f_{};
    int ss_ = 4, block_ = 8, ramp_ = 1, pos_ = 0;
    bool pcm_ = true;
    std::atomic<bool> want_pause_{false}, dop_{false};
    std::atomic<int> state_{kPlaying};
    uint8_t last_marker_ = 0;      // DoP marker of the last frame sent
    uint64_t idle_frames_ = 0;     // DoP silence frames since the pause began
    uint64_t rng_ = 0x9E3779B97F4A7C15ull;   // dither for the pause / resume fades
    std::atomic<uint64_t> peak_l_{0}, peak_r_{0};   // bit patterns of doubles
};

}  // namespace daphne
