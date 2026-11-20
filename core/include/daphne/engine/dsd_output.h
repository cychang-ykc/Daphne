#pragma once
// DSD to the device: modulated PCM (dsd core) or DSD file bytes (played directly), packed for the
// transport.
//
//   DoP     DSD over PCM: one 24-bit word per channel and frame = marker (0x05 / 0xFA, alternating
//           per frame) + 16 DSD bits, oldest bit first; device rate = DSD clock / 16. Written
//           straight into the integer container (no scaling: the marker must be exact).
//   Native  ASIO DSD (Dsd8Msb / Dsd8Lsb): one byte per channel and frame, 8 DSD bits.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "daphne/engine/sample_format.h"

namespace daphne::dsp {
class DsdModulator;
}

namespace daphne {

enum class DsdTransport { Dop, Native };

// DoP device rate for a DSD clock (DSD64 = 176400).
inline int dop_rate(int dsd_clock) { return dsd_clock / 16; }
// Whether a device format can carry DoP (24 valid bits or more in an integer container).
bool dop_capable(const DeviceFormat& f);

class DsdEncoder {
public:
    DsdEncoder();
    ~DsdEncoder();

    // Modulated PCM: pcm_rate = the rate the chain delivers (705.6 / 768 kHz), the modulator runs
    // 44100 x osr with `order`. 1 or 2 channels.
    bool init(int pcm_rate, int osr, int order, const DeviceFormat& dev, DsdTransport t, std::string* error);
    // DSD file playback (no modulator): `clock` = the file's DSD rate, 1-8 channels.
    bool init_direct(int clock, const DeviceFormat& dev, DsdTransport t, std::string* error);

    // Interleaved PCM frames (dev.channels channels) in; device bytes appended.
    void encode(const double* pcm, int frames, std::vector<uint8_t>& out);
    // Interleaved DSD bytes (one per channel and frame, MSB first) in; device bytes appended.
    void encode_dsd(const uint8_t* bytes, int frames, std::vector<uint8_t>& out);
    // DSD silence (0x69 pattern) for about `frames` device frames, keeping the DoP marker sequence,
    // so the DAC stays in DSD mode while the stream ends or restarts.
    void idle(int frames, std::vector<uint8_t>& out);

    // A stream that takes over from another on the same device without a gap (Tube Color switching a DSD file between
    // sent directly and remodulated, doc/N28): modulate without packing, drop what lies before the switch point,
    // continue the other's DoP marker sequence, pack what is left.
    void modulate(const double* pcm, int frames);
    void drop_front(size_t bytes_per_channel);
    size_t pending_per_channel() const { return (bits_.size() - bits_off_) / static_cast<size_t>(ch_); }
    void follow(const DsdEncoder& prev) { marker_ = prev.marker_; }
    void pack_pending(std::vector<uint8_t>& out) { pack(out); }
    // DSD bytes per channel since init, packed into device frames or dropped (the modulator's: its content position)
    int64_t emitted_per_channel() const { return packed_ + dropped_; }

    int dsd_rate() const { return clock_; }
    bool dop() const { return tr_ == DsdTransport::Dop; }
    int bytes_per_frame() const { return dev_.block_align(); }
    // Upper bound of the bytes encode() appends for `frames` PCM frames.
    size_t max_bytes(int frames) const;
    std::string describe() const;
    // the modulator's protection counters (0 without a modulator)
    uint64_t clamped_bits() const;
    uint64_t input_clips() const;

private:
    bool check(const DeviceFormat& dev, DsdTransport t, int max_channels, std::string* error);
    void pack(std::vector<uint8_t>& out);
    void put_word(uint32_t w, std::vector<uint8_t>& out) const;

    std::unique_ptr<dsp::DsdModulator> mod_;
    DeviceFormat dev_{};
    DsdTransport tr_ = DsdTransport::Dop;
    int clock_ = 0, pcm_rate_ = 0, ch_ = 2, order_ = 0;
    bool lsb_first_ = false;
    std::vector<uint8_t> bits_;   // modulator output / file bytes, interleaved bytes per channel
    size_t bits_off_ = 0;
    bool marker_ = false;         // false: 0x05 next
    int64_t packed_ = 0, dropped_ = 0;   // DSD bytes per channel packed / dropped since init
};

}  // namespace daphne
