#include "daphne/engine/dsd_output.h"

#include <algorithm>
#include <array>

#include "daphne/dsp/modulator.h"

namespace daphne {

namespace {

constexpr std::array<uint8_t, 256> make_bitrev() {
    std::array<uint8_t, 256> t{};
    for (int v = 0; v < 256; ++v) {
        int r = 0;
        for (int k = 0; k < 8; ++k) r |= ((v >> k) & 1) << (7 - k);
        t[static_cast<size_t>(v)] = static_cast<uint8_t>(r);
    }
    return t;
}
constexpr auto kBitrev = make_bitrev();

}  // namespace

bool dop_capable(const DeviceFormat& f) {
    switch (f.type) {
        case SampleType::Int32:
        case SampleType::Int24In32:
        case SampleType::Packed24: return true;
        case SampleType::Int32Low: return f.valid_bits >= 24;
        default: return false;
    }
}

DsdEncoder::DsdEncoder() = default;
DsdEncoder::~DsdEncoder() = default;

bool DsdEncoder::check(const DeviceFormat& dev, DsdTransport t, int max_channels, std::string* error) {
    dev_ = dev;
    tr_ = t;
    ch_ = dev.channels;
    lsb_first_ = dev.type == SampleType::Dsd8Lsb;
    bits_.clear();
    bits_off_ = 0;
    marker_ = false;
    if (ch_ < 1 || ch_ > max_channels) {
        if (error) *error = "DSD output supports 1 to " + std::to_string(max_channels) + " channels here";
        return false;
    }
    if (t == DsdTransport::Native && !is_dsd(dev.type)) {
        if (error) *error = "native DSD needs a DSD device format, the device locked " + daphne::describe(dev);
        return false;
    }
    if (t == DsdTransport::Dop && !dop_capable(dev)) {
        if (error) *error = "DoP needs a 24- or 32-bit integer device format, the device locked " + daphne::describe(dev);
        return false;
    }
    return true;
}

bool DsdEncoder::init(int pcm_rate, int osr, int order, const DeviceFormat& dev, DsdTransport t, std::string* error) {
    if (!check(dev, t, 2, error)) return false;
    mod_ = std::make_unique<dsp::DsdModulator>(pcm_rate, osr, order);
    order_ = mod_->order();
    clock_ = mod_->dsd_rate();
    pcm_rate_ = pcm_rate;
    return true;
}

bool DsdEncoder::init_direct(int clock, const DeviceFormat& dev, DsdTransport t, std::string* error) {
    mod_.reset();
    order_ = 0;
    clock_ = clock;
    pcm_rate_ = clock / 8;
    return check(dev, t, 8, error);
}

void DsdEncoder::encode(const double* pcm, int frames, std::vector<uint8_t>& out) {
    mod_->process(pcm, frames, ch_, bits_);
    pack(out);
}

void DsdEncoder::modulate(const double* pcm, int frames) { mod_->process(pcm, frames, ch_, bits_); }

void DsdEncoder::drop_front(size_t bytes_per_channel) {
    const size_t n = std::min(bytes_per_channel * static_cast<size_t>(ch_), bits_.size() - bits_off_);
    bits_off_ += n;
    dropped_ += static_cast<int64_t>(n / static_cast<size_t>(ch_));
}

void DsdEncoder::encode_dsd(const uint8_t* bytes, int frames, std::vector<uint8_t>& out) {
    bits_.insert(bits_.end(), bytes, bytes + static_cast<size_t>(frames) * ch_);
    pack(out);
}

void DsdEncoder::idle(int frames, std::vector<uint8_t>& out) {
    const int per = tr_ == DsdTransport::Dop ? 2 : 1;
    bits_.insert(bits_.end(), static_cast<size_t>(frames) * per * ch_, 0x69);
    pack(out);
}

std::string DsdEncoder::describe() const {
    std::string d = "DSD" + std::to_string(clock_ / 44100) + " (" + std::to_string(clock_) + " Hz), ";
    d += mod_ ? "modulator order " + std::to_string(order_) : std::string("direct (no processing)");
    d += tr_ == DsdTransport::Dop ? ", DoP" : ", native DSD";
    return d;
}

size_t DsdEncoder::max_bytes(int frames) const {
    const double bits = static_cast<double>(frames) * clock_ / pcm_rate_ + 16;
    const int per = tr_ == DsdTransport::Dop ? 16 : 8;
    return (static_cast<size_t>(bits / per) + 2) * static_cast<size_t>(dev_.block_align());
}

uint64_t DsdEncoder::clamped_bits() const { return mod_ ? mod_->clamped_bits() : 0; }
uint64_t DsdEncoder::input_clips() const { return mod_ ? mod_->input_clips() : 0; }

void DsdEncoder::put_word(uint32_t w, std::vector<uint8_t>& out) const {
    switch (dev_.type) {
        case SampleType::Packed24:
            out.push_back(static_cast<uint8_t>(w));
            out.push_back(static_cast<uint8_t>(w >> 8));
            out.push_back(static_cast<uint8_t>(w >> 16));
            break;
        case SampleType::Int32Low: {   // right-aligned: the 24-bit word in the low bits
            const uint32_t v = w << (dev_.valid_bits - 24);
            for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
            break;
        }
        default: {   // Int32 / Int24In32: left-aligned
            const uint32_t v = w << 8;
            for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
            break;
        }
    }
}

void DsdEncoder::pack(std::vector<uint8_t>& out) {
    const size_t per_frame = static_cast<size_t>(ch_) * (tr_ == DsdTransport::Dop ? 2 : 1);
    const size_t frames = (bits_.size() - bits_off_) / per_frame;
    out.reserve(out.size() + frames * static_cast<size_t>(dev_.block_align()));
    for (size_t f = 0; f < frames; ++f) {
        const uint8_t* b = bits_.data() + bits_off_;
        if (tr_ == DsdTransport::Dop) {
            const uint32_t mk = marker_ ? 0xFAu : 0x05u;
            for (int c = 0; c < ch_; ++c) put_word(mk << 16 | static_cast<uint32_t>(b[c]) << 8 | b[ch_ + c], out);
            marker_ = !marker_;
        } else {
            for (int c = 0; c < ch_; ++c) out.push_back(lsb_first_ ? kBitrev[b[c]] : b[c]);
        }
        bits_off_ += per_frame;
        packed_ += tr_ == DsdTransport::Dop ? 2 : 1;
    }
    if (bits_off_ > (1u << 16)) {
        bits_.erase(bits_.begin(), bits_.begin() + static_cast<std::ptrdiff_t>(bits_off_));
        bits_off_ = 0;
    }
}

}  // namespace daphne
