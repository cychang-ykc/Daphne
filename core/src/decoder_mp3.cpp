// MPEG audio layer 1 / 2 / 3 via minimp3_ex with float output: full precision, samples above 0 dBFS
// are kept. minimp3_ex reads the LAME / Info tag and trims the encoder delay and padding (gapless).

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_FLOAT_OUTPUT
#include <minimp3/minimp3.h>
#include <minimp3/minimp3_ex.h>

#include <vector>

#include "daphne/engine/decoder.h"
#include "daphne/engine/file_bytes.h"

namespace daphne {
namespace {

class Mp3Decoder final : public Decoder {
public:
    ~Mp3Decoder() override { close(); }

    bool open(const std::filesystem::path& path, std::string* error) override {
        close();
        int r = 0;
        if (auto bytes = FileBytes::open(path)) {   // read ahead (file_bytes.h)
            cur_.reset(std::move(bytes));
            io_ = {on_read, &cur_, on_seek, &cur_};
            r = mp3dec_ex_open_cb(&d_, &io_, MP3D_SEEK_TO_SAMPLE);
        } else {
#ifdef _WIN32
            r = mp3dec_ex_open_w(&d_, path.c_str(), MP3D_SEEK_TO_SAMPLE);
#else
            r = mp3dec_ex_open(&d_, path.c_str(), MP3D_SEEK_TO_SAMPLE);
#endif
        }
        if (r != 0 || d_.info.channels <= 0) {
            // mp3dec_ex_open_cb does not free what it allocated when it fails (the file variant does); a header
            // without channels is freed here too
            mp3dec_ex_close(&d_);
            cur_.reset(nullptr);
            if (error) *error = "minimp3: cannot open MP3 file";
            return false;
        }
        open_ = true;
        info_ = AudioInfo{};
        info_.rate = d_.info.hz;
        info_.channels = d_.info.channels;
        info_.frames = d_.samples > 0 ? static_cast<int64_t>(d_.samples / d_.info.channels) : -1;
        info_.codec = d_.info.layer == 3 ? "MP3" : d_.info.layer == 2 ? "MP2" : "MP1";
        pos_ = 0;
        return true;
    }

    void close() override {
        if (open_) mp3dec_ex_close(&d_);
        open_ = false;
        cur_.reset(nullptr);
    }

    int read(double* out, int frames) override {
        if (!open_ || frames <= 0) return 0;
        const size_t n = static_cast<size_t>(frames) * info_.channels;
        if (buf_.size() < n) buf_.resize(n);
        const size_t got = mp3dec_ex_read(&d_, buf_.data(), n);
        for (size_t i = 0; i < got; ++i) out[i] = buf_[i];
        const int f = static_cast<int>(got / info_.channels);
        pos_ += f;
        return f;
    }

    bool seek(int64_t frame) override {
        if (!open_ || frame < 0) return false;
        if (mp3dec_ex_seek(&d_, static_cast<uint64_t>(frame) * info_.channels) != 0) return false;
        pos_ = frame;
        return true;
    }

private:
    static size_t on_read(void* buf, size_t n, void* u) { return static_cast<FileCursor*>(u)->read(buf, n); }
    static int on_seek(uint64_t at, void* u) { return static_cast<FileCursor*>(u)->seek(static_cast<int64_t>(at), 0) ? 0 : -1; }

    FileCursor cur_;
    mp3dec_io_t io_{};
    mp3dec_ex_t d_{};
    bool open_ = false;
    std::vector<mp3d_sample_t> buf_;
};

}  // namespace

std::unique_ptr<Decoder> make_mp3_decoder() { return std::make_unique<Mp3Decoder>(); }

}  // namespace daphne
