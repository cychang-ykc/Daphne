// FLAC via dr_flac, read as left-justified s32 (exact for every bit depth, 32-bit included).

#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_OGG
#include <dr_libs/dr_flac.h>

#include <vector>

#include "daphne/engine/decoder.h"
#include "daphne/engine/file_bytes.h"

namespace daphne {
namespace {

constexpr double kInv31 = 1.0 / 2147483648.0;

class FlacDecoder final : public Decoder {
public:
    ~FlacDecoder() override { close(); }

    bool open(const std::filesystem::path& path, std::string* error) override {
        close();
        if (auto bytes = FileBytes::open(path)) {   // read ahead (file_bytes.h)
            cur_.reset(std::move(bytes));
            f_ = drflac_open(on_read, on_seek, on_tell, &cur_, nullptr);
        } else {
#ifdef _WIN32
            f_ = drflac_open_file_w(path.c_str(), nullptr);
#else
            f_ = drflac_open_file(path.c_str(), nullptr);
#endif
        }
        if (!f_) {
            if (error) *error = "dr_flac: cannot open FLAC file";
            return false;
        }
        info_ = AudioInfo{};
        info_.rate = static_cast<int>(f_->sampleRate);
        info_.channels = f_->channels;
        info_.bits = f_->bitsPerSample;
        info_.lossless_int = true;
        info_.frames = f_->totalPCMFrameCount > 0 ? static_cast<int64_t>(f_->totalPCMFrameCount) : -1;
        info_.codec = "FLAC";
        pos_ = 0;
        return true;
    }

    void close() override {
        if (f_) drflac_close(f_);
        f_ = nullptr;
        cur_.reset(nullptr);
    }

    int read(double* out, int frames) override {
        if (!f_ || frames <= 0) return 0;
        const size_t n = static_cast<size_t>(frames) * info_.channels;
        if (buf_.size() < n) buf_.resize(n);
        const auto got = static_cast<int>(drflac_read_pcm_frames_s32(f_, frames, buf_.data()));
        for (size_t i = 0; i < static_cast<size_t>(got) * info_.channels; ++i) out[i] = buf_[i] * kInv31;
        pos_ += got;
        return got;
    }

    bool seek(int64_t frame) override {
        if (!f_ || frame < 0) return false;
        if (!drflac_seek_to_pcm_frame(f_, static_cast<drflac_uint64>(frame))) return false;
        pos_ = frame;
        return true;
    }

private:
    static size_t on_read(void* u, void* out, size_t n) { return static_cast<FileCursor*>(u)->read(out, n); }
    static drflac_bool32 on_seek(void* u, int off, drflac_seek_origin o) {
        return static_cast<FileCursor*>(u)->seek(off, o == DRFLAC_SEEK_CUR ? 1 : o == DRFLAC_SEEK_END ? 2 : 0);
    }
    static drflac_bool32 on_tell(void* u, drflac_int64* at) {
        *at = static_cast<FileCursor*>(u)->tell();
        return DRFLAC_TRUE;
    }

    FileCursor cur_;
    drflac* f_ = nullptr;
    std::vector<drflac_int32> buf_;
};

}  // namespace

std::unique_ptr<Decoder> make_flac_decoder() { return std::make_unique<FlacDecoder>(); }

}  // namespace daphne
