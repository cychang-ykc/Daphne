// WAV / AIFF / AIFC via dr_wav. Integer PCM (and A-law, mu-law, ADPCM) is read as left-justified s32,
// exact for every bit depth; 64-bit float is read raw (the f32 path would round it).

#define DR_WAV_IMPLEMENTATION
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4701)
#endif
#include <dr_libs/dr_wav.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <vector>

#include "daphne/engine/decoder.h"
#include "daphne/engine/file_bytes.h"

namespace daphne {
namespace {

constexpr double kInv31 = 1.0 / 2147483648.0;

class WavDecoder final : public Decoder {
public:
    ~WavDecoder() override { close(); }

    bool open(const std::filesystem::path& path, std::string* error) override {
        close();
        bool opened = false;
        if (auto bytes = FileBytes::open(path)) {   // read ahead (file_bytes.h)
            cur_.reset(std::move(bytes));
            opened = drwav_init(&w_, on_read, on_seek, on_tell, &cur_, nullptr);
        } else {
#ifdef _WIN32
            opened = drwav_init_file_w(&w_, path.c_str(), nullptr);
#else
            opened = drwav_init_file(&w_, path.c_str(), nullptr);
#endif
        }
        if (!opened) {
            if (error) *error = "dr_wav: cannot open WAV/AIFF file";
            return false;
        }
        open_ = true;
        info_ = AudioInfo{};
        info_.rate = static_cast<int>(w_.sampleRate);
        info_.channels = w_.channels;
        info_.channel_mask = w_.fmt.channelMask;
        info_.frames = w_.totalPCMFrameCount > 0 ? static_cast<int64_t>(w_.totalPCMFrameCount) : -1;
        info_.codec = w_.container == drwav_container_aiff ? "AIFF" : "WAV";
        const auto tag = w_.translatedFormatTag;
        float64_ = tag == DR_WAVE_FORMAT_IEEE_FLOAT && w_.bitsPerSample == 64;
        float32_ = tag == DR_WAVE_FORMAT_IEEE_FLOAT && w_.bitsPerSample == 32;
        if (tag == DR_WAVE_FORMAT_PCM) {
            info_.bits = w_.bitsPerSample;
            info_.lossless_int = w_.bitsPerSample <= 32;
        }
        pos_ = 0;
        return true;
    }

    void close() override {
        if (open_) drwav_uninit(&w_);
        open_ = false;
        cur_.reset(nullptr);
    }

    int read(double* out, int frames) override {
        if (!open_ || frames <= 0) return 0;
        const size_t n = static_cast<size_t>(frames) * info_.channels;
        int got = 0;
        if (float64_) {
            // native byte order (dr_wav swaps big-endian AIFF): the doubles land straight in the output
            got = static_cast<int>(drwav_read_pcm_frames(&w_, frames, out));
        } else if (float32_) {
            if (f32_.size() < n) f32_.resize(n);
            got = static_cast<int>(drwav_read_pcm_frames_f32(&w_, frames, f32_.data()));
            for (size_t i = 0; i < static_cast<size_t>(got) * info_.channels; ++i) out[i] = f32_[i];
        } else {
            if (s32_.size() < n) s32_.resize(n);
            got = static_cast<int>(drwav_read_pcm_frames_s32(&w_, frames, s32_.data()));
            for (size_t i = 0; i < static_cast<size_t>(got) * info_.channels; ++i) out[i] = s32_[i] * kInv31;
        }
        pos_ += got;
        return got;
    }

    bool seek(int64_t frame) override {
        if (!open_ || frame < 0) return false;
        if (!drwav_seek_to_pcm_frame(&w_, static_cast<drwav_uint64>(frame))) return false;
        pos_ = frame;
        return true;
    }

private:
    static size_t on_read(void* u, void* out, size_t n) { return static_cast<FileCursor*>(u)->read(out, n); }
    static drwav_bool32 on_seek(void* u, int off, drwav_seek_origin o) {
        return static_cast<FileCursor*>(u)->seek(off, o == DRWAV_SEEK_CUR ? 1 : o == DRWAV_SEEK_END ? 2 : 0);
    }
    static drwav_bool32 on_tell(void* u, drwav_int64* at) {
        *at = static_cast<FileCursor*>(u)->tell();
        return DRWAV_TRUE;
    }

    FileCursor cur_;
    drwav w_{};
    bool open_ = false;
    bool float32_ = false, float64_ = false;
    std::vector<drwav_int32> s32_;
    std::vector<float> f32_;
};

}  // namespace

std::unique_ptr<Decoder> make_wav_decoder() { return std::make_unique<WavDecoder>(); }

}  // namespace daphne
