#include "daphne/engine/dsd_file.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include "daphne/platform/platform.h"

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

uint32_t le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24; }
uint64_t le64(const uint8_t* p) { return le32(p) | static_cast<uint64_t>(le32(p + 4)) << 32; }
uint32_t be32(const uint8_t* p) { return static_cast<uint32_t>(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
uint64_t be64(const uint8_t* p) { return static_cast<uint64_t>(be32(p)) << 32 | be32(p + 4); }
uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>(p[0] << 8 | p[1]); }

bool seek64(FILE* f, int64_t off) { return platform::seek_file(f, off); }

// Header values are checked before they size anything: a damaged or crafted file must fail, not allocate gigabytes or
// loop forever.
constexpr uint64_t kMaxBlock = 1 << 20;        // DSF bytes per channel and block (the format fixes 4096)
constexpr uint64_t kMaxFmtChunk = 1 << 20;     // DSF 'fmt ' chunk (52 bytes in the format)

// DSD64 ... DSD1024 of the 44.1 or the 48 kHz family (what the modulator and DoP handle)
bool valid_dsd_rate(uint64_t rate) {
    for (const uint64_t base : {44100ull * 64, 48000ull * 64})
        for (uint64_t m = 1; m <= 16; m *= 2)
            if (rate == base * m) return true;
    return false;
}

// DFF: where the chunk whose body starts at `body` and holds `size` bytes ends (padded to even), or -1 when it does not
// fit before `end`. The result is always beyond the chunk's 12-byte header, so a walk over chunks moves forward.
int64_t chunk_end(int64_t body, uint64_t size, int64_t end) {
    if (body > end || size > static_cast<uint64_t>(end - body)) return -1;
    return body + static_cast<int64_t>(size + (size & 1));
}

// speaker mask bits (WAVEFORMATEXTENSIBLE)
constexpr uint32_t kFL = 0x1, kFR = 0x2, kFC = 0x4, kLFE = 0x8, kBL = 0x10, kBR = 0x20;

// DSF channel type -> speaker mask
uint32_t dsf_mask(uint32_t type) {
    switch (type) {
        case 1: return kFC;                                  // mono
        case 2: return kFL | kFR;                            // stereo
        case 3: return kFL | kFR | kFC;                      // 3 channels
        case 4: return kFL | kFR | kBL | kBR;                // quad
        case 5: return kFL | kFR | kFC | kLFE;               // 4 channels
        case 6: return kFL | kFR | kFC | kBL | kBR;          // 5 channels
        case 7: return kFL | kFR | kFC | kLFE | kBL | kBR;   // 5.1
        default: return 0;
    }
}

// DFF CHNL channel id -> speaker bit (0 = unknown id)
uint32_t dff_speaker(const uint8_t* id) {
    struct Id {
        const char* name;
        uint32_t bit;
    };
    static constexpr Id ids[] = {{"SLFT", kFL}, {"MLFT", kFL}, {"SRGT", kFR}, {"MRGT", kFR}, {"C   ", kFC},
                                 {"LFE ", kLFE}, {"LS  ", kBL}, {"RS  ", kBR}};
    for (const auto& i : ids)
        if (std::memcmp(id, i.name, 4) == 0) return i.bit;
    return 0;
}

}  // namespace

bool is_dsd_path(const std::filesystem::path& path) {
    const std::string e = file_extension(path);
    return e == "dsf" || e == "dff";
}

DsdFile::DsdFile() = default;
DsdFile::~DsdFile() {
    if (f_) fclose(f_);
}

bool DsdFile::open(const std::filesystem::path& path, std::string* error) {
    if (f_) fclose(f_);
    f_ = nullptr;
    info_ = DsdInfo{};
    f_ = platform::open_file(path, "rb");
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    file_size_ = ec ? -1 : static_cast<int64_t>(size);
    if (!f_) {
        if (error) *error = "cannot open the file";
        return false;
    }
    uint8_t magic[4] = {};
    if (fread(magic, 1, 4, f_) != 4) {
        if (error) *error = "file too short";
        return false;
    }
    seek64(f_, 0);
    if (std::memcmp(magic, "DSD ", 4) == 0) return open_dsf(error);
    if (std::memcmp(magic, "FRM8", 4) == 0) return open_dff(error);
    if (error) *error = "not a DSF or DFF file";
    return false;
}

bool DsdFile::open_dsf(std::string* error) {
    auto fail = [&](const std::string& m) {
        if (error) *error = "DSF: " + m;
        return false;
    };
    uint8_t h[28], fmt[52], data[12];
    if (fread(h, 1, 28, f_) != 28) return fail("short header");
    if (fread(fmt, 1, 52, f_) != 52 || std::memcmp(fmt, "fmt ", 4) != 0) return fail("no 'fmt ' chunk");
    const uint64_t fmt_size = le64(fmt + 4);
    if (fmt_size < 52 || fmt_size > kMaxFmtChunk) return fail("invalid 'fmt ' chunk size " + std::to_string(fmt_size));
    if (le32(fmt + 16) != 0) return fail("unsupported format id (not raw DSD)");
    const uint32_t ch_type = le32(fmt + 20), ch = le32(fmt + 24), rate = le32(fmt + 28), bps = le32(fmt + 32);
    const uint64_t samples = le64(fmt + 36);
    const uint32_t block = le32(fmt + 44);
    if (bps != 1 && bps != 8) return fail("bits per sample must be 1 or 8");
    if (ch < 1 || ch > 8) return fail("invalid channel count " + std::to_string(ch));
    if (block == 0 || block > kMaxBlock) return fail("invalid block size " + std::to_string(block));
    if (!valid_dsd_rate(rate)) return fail("unsupported DSD rate " + std::to_string(rate));
    if (!seek64(f_, 28 + static_cast<int64_t>(fmt_size))) return fail("seek");
    if (fread(data, 1, 12, f_) != 12 || std::memcmp(data, "data", 4) != 0) return fail("no 'data' chunk");
    const uint64_t data_size = le64(data + 4);
    if (data_size < 12) return fail("invalid 'data' chunk size " + std::to_string(data_size));
    data_off_ = 28 + static_cast<int64_t>(fmt_size) + 12;
    uint64_t data_bytes = data_size - 12;
    if (file_size_ >= 0) data_bytes = std::min<uint64_t>(data_bytes, static_cast<uint64_t>(std::max<int64_t>(0, file_size_ - data_off_)));
    dsf_ = true;
    lsb_first_ = bps == 1;
    block_ = static_cast<int>(block);
    info_.codec = "DSF";
    info_.dsd_rate = static_cast<int>(rate);
    info_.channels = static_cast<int>(ch);
    info_.channel_mask = dsf_mask(ch_type);
    // the sample count bounds playback, not the padding of the last block or the chunks after the data. A block holds
    // its channels one after another, so in a block cut short only the bytes the last channel still has are a frame
    // for every channel (beyond them a channel would read the zero fill: full-scale negative in DSD, a click)
    const uint64_t span = static_cast<uint64_t>(block) * ch, rest = data_bytes % span;
    const uint64_t per_ch = data_bytes / span * block +
                            (rest > static_cast<uint64_t>(block) * (ch - 1) ? rest - static_cast<uint64_t>(block) * (ch - 1) : 0);
    info_.frames = static_cast<int64_t>(std::min<uint64_t>(samples / 8, per_ch));
    blk_.assign(static_cast<size_t>(block_) * ch, 0);
    blk_index_ = -1;
    pos_ = 0;
    return true;
}

bool DsdFile::open_dff(std::string* error) {
    auto fail = [&](const std::string& m) {
        if (error) *error = "DFF: " + m;
        return false;
    };
    uint8_t h[16];
    if (fread(h, 1, 16, f_) != 16 || std::memcmp(h + 12, "DSD ", 4) != 0) return fail("not a DSD form");
    const uint64_t form_size = be64(h + 4);
    if (form_size < 4 || form_size > (1ull << 62)) return fail("invalid form size");
    int64_t form_end = 12 + static_cast<int64_t>(form_size);
    if (file_size_ >= 0) form_end = std::min(form_end, file_size_);   // a truncated file: what is there
    int64_t off = 16;
    bool have_data = false;
    uint64_t data_bytes = 0;
    // a speaker mask describes channels in ascending bit order: the CHNL ids must come in that order
    uint32_t mask = 0, last_bit = 0;
    bool mask_ok = true;
    std::string cmpr = "DSD ";
    while (off + 12 <= form_end) {
        uint8_t c[12];
        if (!seek64(f_, off) || fread(c, 1, 12, f_) != 12) break;
        const uint64_t size = be64(c + 4);
        const int64_t body = off + 12;
        const int64_t next = chunk_end(body, size, form_end);
        if (std::memcmp(c, "PROP", 4) == 0) {
            uint8_t t[4];
            if (fread(t, 1, 4, f_) != 4 || std::memcmp(t, "SND ", 4) != 0) return fail("unexpected PROP type");
            const int64_t prop_end = next < 0 ? form_end : body + static_cast<int64_t>(size);
            int64_t p = body + 4;
            while (p + 12 <= prop_end) {
                uint8_t s[12];
                if (!seek64(f_, p) || fread(s, 1, 12, f_) != 12) break;
                const uint64_t ss = be64(s + 4);
                const int64_t p_next = chunk_end(p + 12, ss, prop_end);
                uint8_t v[8] = {};
                if (std::memcmp(s, "FS  ", 4) == 0 && fread(v, 1, 4, f_) == 4) {
                    if (!valid_dsd_rate(be32(v))) return fail("unsupported DSD rate " + std::to_string(be32(v)));
                    info_.dsd_rate = static_cast<int>(be32(v));
                } else if (std::memcmp(s, "CHNL", 4) == 0 && fread(v, 1, 2, f_) == 2) {
                    info_.channels = be16(v);
                    for (int k = 0; k < info_.channels && k < 8; ++k) {
                        uint8_t id[4];
                        if (fread(id, 1, 4, f_) != 4) break;
                        const uint32_t bit = dff_speaker(id);
                        if (bit <= last_bit) mask_ok = false;   // unknown id, duplicate or out of order
                        last_bit = bit;
                        mask |= bit;
                    }
                } else if (std::memcmp(s, "CMPR", 4) == 0 && fread(v, 1, 4, f_) == 4) {
                    cmpr.assign(reinterpret_cast<char*>(v), 4);
                }
                if (p_next < 0) break;
                p = p_next;
            }
        } else if (std::memcmp(c, "DSD ", 4) == 0) {
            data_off_ = body;
            data_bytes = next < 0 ? static_cast<uint64_t>(std::max<int64_t>(0, form_end - body)) : size;   // truncated: what is there
            have_data = true;
        } else if (std::memcmp(c, "DST ", 4) == 0) {
            return fail("DST-compressed DSD is not supported");
        }
        if (next < 0) break;   // the chunk runs past the end: nothing can follow it
        off = next;
    }
    if (cmpr != "DSD ") return fail("compression '" + cmpr + "' is not supported");
    if (!have_data || info_.channels < 1 || info_.channels > 8 || info_.dsd_rate == 0) return fail("missing or invalid sound data");
    dsf_ = false;
    lsb_first_ = false;
    info_.codec = "DFF";
    info_.frames = static_cast<int64_t>(data_bytes) / info_.channels;
    if (mask_ok && mask) info_.channel_mask = mask;
    if (!info_.channel_mask) info_.channel_mask = info_.channels == 2 ? kFL | kFR : info_.channels == 1 ? kFC : 0;
    pos_ = 0;
    return seek64(f_, data_off_);
}

bool DsdFile::fill_block() {
    const int64_t b = pos_ / block_;
    if (b == blk_index_) return true;
    const size_t n = blk_.size();
    if (!seek64(f_, data_off_ + b * static_cast<int64_t>(n))) return false;
    const size_t got = fread(blk_.data(), 1, n, f_);
    std::fill(blk_.begin() + static_cast<std::ptrdiff_t>(got), blk_.end(), 0);
    if (lsb_first_)
        for (auto& v : blk_) v = kBitrev[v];
    blk_index_ = b;
    return got > 0;
}

int DsdFile::read(uint8_t* out, int frames) {
    if (!f_) return 0;
    const int ch = info_.channels;
    const int64_t left = info_.frames - pos_;
    const int n = static_cast<int>(std::min<int64_t>(frames, std::max<int64_t>(0, left)));
    if (n <= 0) return 0;
    if (!dsf_) {   // DFF: already interleaved bytes, MSB first
        const size_t got = fread(out, static_cast<size_t>(ch), static_cast<size_t>(n), f_);
        pos_ += static_cast<int64_t>(got);
        return static_cast<int>(got);
    }
    int done = 0;
    while (done < n) {
        if (!fill_block()) break;
        const int in_blk = static_cast<int>(pos_ % block_);
        const int take = std::min(n - done, block_ - in_blk);
        for (int c = 0; c < ch; ++c) {
            const uint8_t* src = blk_.data() + static_cast<size_t>(c) * block_ + in_blk;
            uint8_t* dst = out + static_cast<size_t>(done) * ch + c;
            for (int i = 0; i < take; ++i) dst[static_cast<size_t>(i) * ch] = src[i];
        }
        done += take;
        pos_ += take;
    }
    return done;
}

bool DsdFile::seek(int64_t frame) {
    if (!f_) return false;
    pos_ = std::clamp<int64_t>(frame, 0, info_.frames);
    if (dsf_) return true;   // the block is loaded on demand
    return seek64(f_, data_off_ + pos_ * info_.channels);
}

// ---------------------------------------------------------------------------------- DSD -> PCM

namespace {

class DsdPcmDecoder final : public Decoder {
public:
    DsdPcmDecoder(int out_rate, int lowpass_khz) : want_rate_(out_rate), lowpass_khz_(lowpass_khz) {}

    bool open(const std::filesystem::path& path, std::string* error) override {
        if (!file_.open(path, error)) return false;
        const DsdInfo& d = file_.info();
        const int base = d.dsd_rate % 44100 == 0 ? 44100 : 48000;
        out_rate_ = want_rate_ > 0 ? want_rate_ : base * 4;   // 176.4 / 192 kHz
        step_ = d.dsd_rate / out_rate_ / 8;                   // bytes per output sample (2 for DSD64)
        if (step_ < 1 || d.dsd_rate % (out_rate_ * 8) != 0) {
            if (error) *error = "unsupported DSD rate " + std::to_string(d.dsd_rate);
            return false;
        }
        ch_ = d.channels;
        // Two linear-phase Kaiser low-passes, 120 dB:
        //   1. at the DSD rate, eight bits per table lookup: passes `pass`, stops from out_rate - stop,
        //      the first frequency that would alias below the final stop-band edge
        //   2. at out_rate: the steep pass -> stop edge (left out when stage 1 already ends at `stop`)
        // so the steep part runs at the low rate (24 kHz setting, DSD64: 23 lookups + 230 taps per
        // output sample instead of 459 lookups).
        const double pass_hz = lowpass_khz_ >= 50 ? 50000.0 : lowpass_khz_ >= 30 ? 30000.0 : 24000.0;
        const double stop_hz = lowpass_khz_ >= 50 ? 80000.0 : lowpass_khz_ >= 30 ? 60000.0 : 30000.0;
        const double pass = std::min(pass_hz, 0.4535 * out_rate_), stop = std::min(stop_hz, 0.5 * out_rate_);
        const double stop1 = std::max(stop, out_rate_ - stop);
        const std::vector<double> h1 = kaiser_lowpass(pass, stop1, d.dsd_rate, 8);
        // x2 (+6.02 dB) folded into the table: 50% modulation (the SACD reference level) -> 0 dBFS
        constexpr double kReferenceGain = 2.0;
        k_ = static_cast<int>(h1.size()) / 8;
        table_.assign(static_cast<size_t>(k_) * 256, 0.0);
        for (int j = 0; j < k_; ++j)
            for (int v = 0; v < 256; ++v) {
                double s = 0.0;
                for (int b = 0; b < 8; ++b) s += h1[static_cast<size_t>(8 * j + b)] * ((v >> (7 - b) & 1) ? 1.0 : -1.0);
                table_[static_cast<size_t>(j) * 256 + v] = s * kReferenceGain;
            }
        h2_.clear();
        if (stop1 > stop) h2_ = kaiser_lowpass(pass, stop, out_rate_, 1);
        n2_ = static_cast<int>(h2_.size());
        reset_state();
        info_ = AudioInfo{};
        info_.rate = out_rate_;
        info_.channels = ch_;
        info_.channel_mask = d.channel_mask;
        info_.dsd_pcm = true;
        info_.dsd_rate = d.dsd_rate;   // the file's DSD rate (rate is the PCM rate it is converted to)
        info_.frames = d.frames / step_;
        info_.codec = d.codec + " (DSD" + std::to_string(d.dsd_rate / base) + " -> PCM)";
        buf_.resize(static_cast<size_t>(kChunk) * step_ * ch_);
        // the filters' delay, the whole frames of it taken out (doc/N28): output frame f is the file's frame f
        lat_ = static_cast<int>(std::floor(filter_delay()));
        return seek(0);
    }

    void close() override {}

    // The filters' delay: the frame the converter makes once the bytes of frame g are in stands for content before g.
    // Stage 1 (linear phase, 8 k bits) is centred (8 k - 1) / 2 bits before the last of them, stage 2 (n2 taps)
    // (n2 - 1) / 2 frames before g; frame g stands for the bits from 8 g step on.
    double filter_delay() const {
        const double bits = 1.0 - 8.0 * step_ + (8.0 * k_ - 1.0) / 2.0 + (n2_ > 0 ? 4.0 * step_ * (n2_ - 1) : 0.0);
        return bits / (8.0 * step_);
    }
    // What is left of it: the whole frames are taken out (lat_), the fraction is not (doc/N28)
    double content_delay_frames() const override { return filter_delay() - lat_; }

    // Output frame f is the converter's frame f + lat_: its first lat_ frames after a seek are made and dropped, and
    // after the last byte of the file DSD silence feeds the last lat_ frames out, so nothing of the music is cut.
    int read(double* out, int frames) override {
        int done = 0;
        while (done < frames) {
            const int want = std::min(frames - done, kChunk);
            int samples = fetch(buf_.data(), want);
            for (int s = 0; s < samples; ++s)
                produce(buf_.data() + static_cast<size_t>(s) * step_ * ch_, out + static_cast<size_t>(done + s) * ch_);
            done += samples;
            if (samples < want) break;
        }
        pos_ += done;
        return done;
    }

    bool seek(int64_t frame) override {
        reset_state();
        eof_ = false;
        tail_left_ = lat_;
        // the converter's frame for output frame `frame`; prime both stages: stage 2 needs the n2 outputs before it,
        // stage 1 the k bytes before those
        const int64_t target = frame + lat_;
        const int64_t first = std::max<int64_t>(0, target - n2_);
        const int64_t start = std::max<int64_t>(0, first * step_ - k_);
        if (!file_.seek(start)) return false;
        std::vector<uint8_t> tmp(static_cast<size_t>(step_) * ch_);
        for (int64_t f = start; f < first * step_; ++f) {
            if (file_.read(tmp.data(), 1) != 1) {
                eof_ = true;
                break;
            }
            push_byte(tmp.data());
        }
        std::vector<double> dummy(static_cast<size_t>(ch_));
        for (int64_t f = first; f < target && !eof_; ++f) {
            if (fetch(tmp.data(), 1) != 1) break;
            produce(tmp.data(), dummy.data());
        }
        pos_ = frame;
        return true;
    }

private:
    static constexpr int kChunk = 4096;

    static double bessel_i0(double x) {
        double s = 1.0, t = 1.0;
        for (int k = 1; k < 60; ++k) {
            t *= (x / (2.0 * k)) * (x / (2.0 * k));
            s += t;
            if (t < 1e-17 * s) break;
        }
        return s;
    }

    // Kaiser-windowed sinc, 120 dB (beta 12.26), length rounded up to a multiple of `multiple`, DC gain 1.
    static std::vector<double> kaiser_lowpass(double pass, double stop, double fs, int multiple) {
        const double pi = 3.14159265358979323846, fc = 0.5 * (pass + stop) / fs, dw = 2.0 * pi * (stop - pass) / fs;
        int n = static_cast<int>(std::ceil((120.0 - 8.0) / (2.285 * dw)));
        n = (n + multiple - 1) / multiple * multiple;
        std::vector<double> h(static_cast<size_t>(n));
        const double beta = 12.26, i0b = bessel_i0(beta), mid = (n - 1) / 2.0;
        double sum = 0.0;
        for (int i = 0; i < n; ++i) {
            const double t = i - mid, r = t / mid;
            const double sinc = t == 0.0 ? 2.0 * fc : std::sin(2.0 * pi * fc * t) / (pi * t);
            h[static_cast<size_t>(i)] = sinc * bessel_i0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / i0b;
            sum += h[static_cast<size_t>(i)];
        }
        for (auto& v : h) v /= sum;
        return h;
    }

    // The bytes of up to n frames (step_ bytes per channel each) into b; past the file's end DSD silence, lat_ frames of
    // it at most (the tail the delay held back). Returns the frames.
    int fetch(uint8_t* b, int n) {
        int got = 0;
        if (!eof_) {
            got = file_.read(b, n * step_) / step_;
            if (got < n) eof_ = true;
        }
        if (got < n && eof_) {
            const int t = std::min(n - got, tail_left_);
            std::fill(b + static_cast<size_t>(got) * step_ * ch_, b + static_cast<size_t>(got + t) * step_ * ch_, uint8_t{0x69});
            tail_left_ -= t;
            got += t;
        }
        return got;
    }

    void reset_state() {
        hist_.assign(static_cast<size_t>(ch_) * 2 * k_, 0x69);   // DSD silence
        head_ = 0;
        x2_.assign(static_cast<size_t>(ch_) * 2 * std::max(n2_, 1), 0.0);
        w2_ = 0;
    }

    // One byte per channel into the stage-1 history. Each channel's history is stored twice in a row
    // (mirror), so the k bytes from head_ are always contiguous, oldest first.
    void push_byte(const uint8_t* frame) {
        for (int c = 0; c < ch_; ++c) {
            uint8_t* h = hist_.data() + static_cast<size_t>(c) * 2 * k_;
            h[head_] = h[head_ + k_] = frame[c];
        }
        head_ = head_ + 1 == k_ ? 0 : head_ + 1;
    }

    // step_ bytes per channel in, one output frame out.
    void produce(const uint8_t* bytes, double* out) {
        for (int b = 0; b < step_; ++b) push_byte(bytes + static_cast<size_t>(b) * ch_);
        const int half = n2_ / 2;
        for (int c = 0; c < ch_; ++c) {
            const uint8_t* h = hist_.data() + static_cast<size_t>(c) * 2 * k_ + head_;
            const double* t = table_.data();
            double y = 0.0;
            for (int j = 0; j < k_; ++j, t += 256) y += t[h[j]];
            if (n2_ > 0) {   // stage 2, mirrored history; the kernel is symmetric, so pairs share a tap
                double* x = x2_.data() + static_cast<size_t>(c) * 2 * n2_;
                x[w2_] = x[w2_ + n2_] = y;
                const double* win = x + w2_ + 1;
                double a0 = 0.0, a1 = 0.0;
                int i = 0;
                for (; i + 1 < half; i += 2) {
                    a0 += h2_[static_cast<size_t>(i)] * (win[i] + win[n2_ - 1 - i]);
                    a1 += h2_[static_cast<size_t>(i + 1)] * (win[i + 1] + win[n2_ - 2 - i]);
                }
                for (; i < half; ++i) a0 += h2_[static_cast<size_t>(i)] * (win[i] + win[n2_ - 1 - i]);
                if (n2_ & 1) a0 += h2_[static_cast<size_t>(half)] * win[half];
                y = a0 + a1;
            }
            out[c] = y;
        }
        if (n2_ > 0) w2_ = w2_ + 1 == n2_ ? 0 : w2_ + 1;
    }

    DsdFile file_;
    int want_rate_ = 0, lowpass_khz_ = 24, out_rate_ = 0, step_ = 1, ch_ = 2, k_ = 1, head_ = 0, n2_ = 0, w2_ = 0;
    int lat_ = 0, tail_left_ = 0;   // the delay's whole frames taken out; the silence frames left for the tail
    bool eof_ = false;
    std::vector<double> table_, h2_, x2_;
    std::vector<uint8_t> hist_, buf_;
};

}  // namespace

std::unique_ptr<Decoder> make_dsd_pcm_decoder(int out_rate, int lowpass_khz) {
    return std::make_unique<DsdPcmDecoder>(out_rate, lowpass_khz);
}

}  // namespace daphne
