#pragma once
// Decoders: a file in, interleaved float64 frames out (±1.0 full scale).
//
// Integer sources are normalized as sample / 2^31 (left-justified), so x * 2^(N-1) gives back an
// N-bit sample exactly whenever N is at least the source bit depth: the path from file to device is
// bit-perfect when nothing processes the samples.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace daphne {

struct AudioInfo {
    int rate = 0;
    int channels = 0;
    int bits = 0;               // source bit depth for integer PCM; 0 = float or lossy
    bool lossless_int = false;  // samples are exact integers of `bits` bits
    int64_t frames = -1;        // total frames, -1 = unknown
    uint32_t channel_mask = 0;  // WAVEFORMATEXTENSIBLE speaker mask, 0 = unknown (default order)
    std::string codec;          // "FLAC", "WAV", "AIFF", "MP3", "AAC", "ALAC", ...
    int dsd_rate = 0;           // DSD file: its DSD rate. Played directly: rate = dsd_rate / 8, frames = bytes;
                                // converted to PCM (dsd_pcm): rate is the PCM rate
    bool dsd_pcm = false;       // DSD file converted to PCM (make_dsd_pcm_decoder)
};

class Decoder {
public:
    virtual ~Decoder() = default;
    virtual bool open(const std::filesystem::path& path, std::string* error) = 0;
    virtual void close() = 0;
    // Interleaved float64, ±1.0 full scale. Returns frames read; 0 = end of stream.
    virtual int read(double* out, int frames) = 0;
    virtual bool seek(int64_t frame) = 0;
    // Frames between the source's content and the frame it comes out in (a causal filter in the decoder: the DSD -> PCM
    // converter's, a fraction of a frame included); 0 for the PCM decoders.
    virtual double content_delay_frames() const { return 0.0; }
    const AudioInfo& info() const { return info_; }
    int64_t position() const { return pos_; }
    double duration_seconds() const { return info_.rate > 0 && info_.frames > 0 ? double(info_.frames) / info_.rate : 0.0; }

protected:
    AudioInfo info_;
    int64_t pos_ = 0;
};

// Picks the decoder by file extension (kLivePath: the host's live input, live_source.h):
//   wav / wave / bwf / aiff / aif / aifc -> dr_wav
//   flac / fla                           -> dr_flac
//   mp3 / mp2 / mp1                      -> minimp3 (float output)
//   dsf / dff                            -> DSD converted to PCM (the player sends DSD directly when it can)
//   anything else                        -> FFmpeg, which probes the content
// When a dedicated decoder cannot open the file (an unusual WAV codec, a wrong extension), FFmpeg
// gets one more try.
std::unique_ptr<Decoder> open_decoder(const std::filesystem::path& path, std::string* error);
// The file name's extension after the last '.', lower case ASCII ("" without one).
std::string file_extension(const std::filesystem::path& path);

std::unique_ptr<Decoder> make_flac_decoder();
std::unique_ptr<Decoder> make_wav_decoder();
std::unique_ptr<Decoder> make_mp3_decoder();
std::unique_ptr<Decoder> make_ffmpeg_decoder();
// DSF / DFF converted to PCM: make_dsd_pcm_decoder in dsd_file.h.

}  // namespace daphne
