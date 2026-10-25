// FFmpeg decoder for every format without a dedicated decoder (AAC, ALAC, APE, WavPack, Vorbis, Opus,
// WMA, TAK, TTA, audio in video containers ...): avformat -> the best audio stream -> avcodec ->
// swresample to interleaved float64 at the source rate and channel layout.
//
// Gapless MP4: FFmpeg drops the encoder delay; the padding at the end is cut here at the length from
// the iTunSMPB tag or, failing that, the stream duration (the edit list). Positions count from the
// stream's start time, so frame 0 is the first audible sample. Ogg (Vorbis / Opus) and MP3 trimming
// is done by FFmpeg itself.

#include "daphne/engine/decoder.h"

#ifdef DAPHNE_HAVE_FFMPEG

#include "daphne/engine/file_bytes.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "daphne/platform/platform.h"

namespace daphne {
namespace {

// iTunSMPB: " 00000000 <delay> <padding> <length> ...", hex; the length counts frames after the delay.
int64_t itunsmpb_length(const AVDictionary* m) {
    const AVDictionaryEntry* e = av_dict_get(m, "iTunSMPB", nullptr, 0);
    if (!e) return -1;
    // the first four hex fields, by strtoull (sscanf_s is MSVC's only; MSVC deprecates plain sscanf)
    unsigned long long f[4] = {};
    const char* p = e->value;
    for (auto& v : f) {
        char* end = nullptr;
        v = std::strtoull(p, &end, 16);
        if (end == p) return -1;
        p = end;
    }
    const unsigned long long len = f[3];
    return len > 0 ? static_cast<int64_t>(len) : -1;
}

class FfmpegDecoder final : public Decoder {
public:
    ~FfmpegDecoder() override { close(); }

    bool open(const std::filesystem::path& path, std::string* error) override {
        close();
        path_ = path;
        auto fail = [&](const std::string& m) {
            if (error) *error = "FFmpeg: " + m;
            close();
            return false;
        };
        if (auto bytes = FileBytes::open(path)) {   // read ahead (file_bytes.h): FFmpeg reads memory
            cur_.reset(std::move(bytes));
            constexpr int kIoBuffer = 1 << 16;
            auto* buf = static_cast<unsigned char*>(av_malloc(kIoBuffer));
            if (buf) io_ = avio_alloc_context(buf, kIoBuffer, 0, &cur_, on_read, nullptr, on_seek);
            if (!io_) {
                av_free(buf);
                return fail("cannot set up reading");
            }
            fmt_ = avformat_alloc_context();
            if (!fmt_) return fail("cannot set up reading");
            fmt_->pb = io_;   // custom IO: avformat_close_input leaves it to close()
        }
        if (avformat_open_input(&fmt_, platform::path_to_utf8(path).c_str(), nullptr, nullptr) < 0) return fail("cannot open file");
        if (avformat_find_stream_info(fmt_, nullptr) < 0) return fail("no stream info");
        const AVCodec* codec = nullptr;
        stream_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
        if (stream_ < 0 || !codec) return fail("no audio stream");
        AVStream* st = fmt_->streams[stream_];
        ctx_ = avcodec_alloc_context3(codec);
        if (!ctx_ || avcodec_parameters_to_context(ctx_, st->codecpar) < 0) return fail("cannot set up decoder");
        if (avcodec_open2(ctx_, codec, nullptr) < 0) return fail("cannot open decoder");

        // keep the source layout; an unspecified one gets the default mask for the channel count
        AVChannelLayout in{};
        av_channel_layout_copy(&in, &ctx_->ch_layout);
        if (in.order == AV_CHANNEL_ORDER_UNSPEC || in.nb_channels <= 0) {
            const int n = ctx_->ch_layout.nb_channels;
            av_channel_layout_uninit(&in);
            const uint64_t mask = n == 4 ? 0xF : n == 6 ? 0x3F : n == 8 ? 0x63F : n == 1 ? AV_CH_LAYOUT_MONO : AV_CH_LAYOUT_STEREO;
            av_channel_layout_from_mask(&in, mask);
        }
        const int rate = ctx_->sample_rate;
        if (swr_alloc_set_opts2(&swr_, &in, AV_SAMPLE_FMT_DBL, rate, &ctx_->ch_layout, ctx_->sample_fmt, rate, 0, nullptr) < 0 ||
            swr_init(swr_) < 0) {
            av_channel_layout_uninit(&in);
            return fail("cannot set up the sample converter");
        }
        info_ = AudioInfo{};
        info_.channels = in.nb_channels;
        if (in.order == AV_CHANNEL_ORDER_NATIVE) info_.channel_mask = static_cast<uint32_t>(in.u.mask & 0x3ffff);
        av_channel_layout_uninit(&in);
        info_.rate = rate;
        info_.codec = avcodec_get_name(ctx_->codec_id);
        for (auto& c : info_.codec) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
        // lossless integer codecs (ALAC, APE, WavPack, TTA, FLAC in other containers, PCM) are exact in float64
        const AVCodecDescriptor* desc = avcodec_descriptor_get(ctx_->codec_id);
        const bool lossless = desc && (desc->props & AV_CODEC_PROP_LOSSLESS);
        const AVSampleFormat packed = av_get_packed_sample_fmt(ctx_->sample_fmt);
        const bool int_fmt = packed == AV_SAMPLE_FMT_S16 || packed == AV_SAMPLE_FMT_S32 || packed == AV_SAMPLE_FMT_U8;
        if (lossless && int_fmt) {
            const int raw = st->codecpar->bits_per_raw_sample > 0 ? st->codecpar->bits_per_raw_sample : ctx_->bits_per_raw_sample;
            info_.bits = raw > 0 ? raw : av_get_bytes_per_sample(packed) * 8;
            info_.lossless_int = true;
        }
        start_ = st->start_time != AV_NOPTS_VALUE ? st->start_time : 0;
        end_ = -1;
        if (std::strstr(fmt_->iformat->name, "mp4")) {   // the "mov,mp4,m4a,3gp,3g2,mj2" demuxer
            end_ = itunsmpb_length(fmt_->metadata);
            if (end_ < 0) end_ = itunsmpb_length(st->metadata);
            if (end_ < 0 && st->duration > 0) end_ = av_rescale_q(st->duration, st->time_base, AVRational{1, rate});
        }
        if (end_ > 0)
            info_.frames = end_;
        else if (st->duration > 0)
            info_.frames = av_rescale_q(st->duration, st->time_base, AVRational{1, rate});
        else if (fmt_->duration > 0)
            info_.frames = av_rescale(fmt_->duration, rate, AV_TIME_BASE);
        pkt_ = av_packet_alloc();
        frame_ = av_frame_alloc();
        pos_ = 0;
        return true;
    }

    void close() override {
        if (swr_) swr_free(&swr_);
        if (ctx_) avcodec_free_context(&ctx_);
        if (fmt_) avformat_close_input(&fmt_);
        if (io_) {
            av_freep(&io_->buffer);
            avio_context_free(&io_);
        }
        cur_.reset(nullptr);
        if (pkt_) av_packet_free(&pkt_);
        if (frame_) av_frame_free(&frame_);
        pending_.clear();
        pending_pos_ = 0;
        eof_ = false;
        target_ = -1;
        next_ = 0;
    }

    int read(double* out, int frames) override {
        if (!ctx_ || frames <= 0) return 0;
        const int ch = info_.channels;
        int done = 0;
        while (done < frames) {
            if (pending_pos_ >= pending_.size()) {
                if (!decode_more()) break;
                continue;
            }
            const size_t avail = (pending_.size() - pending_pos_) / ch;
            const size_t n = std::min<size_t>(avail, static_cast<size_t>(frames - done));
            std::memcpy(out + static_cast<size_t>(done) * ch, pending_.data() + pending_pos_, n * ch * sizeof(double));
            pending_pos_ += n * ch;
            done += static_cast<int>(n);
        }
        pos_ += done;
        return done;
    }

    bool seek(int64_t frame) override {
        if (!ctx_ || frame < 0) return false;
        AVStream* st = fmt_->streams[stream_];
        // lossy codecs rebuild a frame from its predecessor (MDCT overlap): start 4096 frames early so the
        // decoder has settled by the target; the extra frames are dropped in convert_frame
        const int64_t preroll = info_.lossless_int ? 0 : std::max<int64_t>(4096, 4 * int64_t{st->codecpar->seek_preroll});
        if (frame < preroll) {
            // near the start a seek lands after the encoder-delay packet the decoder needs: decode from the top
            if (!open(path_, nullptr)) return false;
            target_ = frame;
            pos_ = frame;
            return true;
        }
        const int64_t ts = start_ + av_rescale_q(frame - preroll, AVRational{1, info_.rate}, st->time_base);
        if (av_seek_frame(fmt_, stream_, ts, AVSEEK_FLAG_BACKWARD) < 0) return false;
        avcodec_flush_buffers(ctx_);
        swr_init(swr_);
        pending_.clear();
        pending_pos_ = 0;
        eof_ = false;
        target_ = frame;   // drop decoded samples before the target (the seek lands on a key frame)
        next_ = frame;
        pos_ = frame;
        return true;
    }

private:
    // Decodes until at least one frame is pending. False at the end of the stream.
    bool decode_more() {
        for (;;) {
            const int r = avcodec_receive_frame(ctx_, frame_);
            if (r == 0) {
                convert_frame();
                av_frame_unref(frame_);
                if (pending_pos_ < pending_.size()) return true;
                continue;
            }
            if (r == AVERROR_EOF) return drain_swr();
            if (r != AVERROR(EAGAIN)) return false;
            if (eof_) return drain_swr();
            if (av_read_frame(fmt_, pkt_) < 0) {
                eof_ = true;
                avcodec_send_packet(ctx_, nullptr);
                continue;
            }
            if (pkt_->stream_index == stream_) avcodec_send_packet(ctx_, pkt_);
            av_packet_unref(pkt_);
        }
    }

    void convert_frame() {
        const int ch = info_.channels;
        const int cap = swr_get_out_samples(swr_, frame_->nb_samples);
        pending_.resize(static_cast<size_t>(std::max(cap, 0)) * ch);
        uint8_t* dst = reinterpret_cast<uint8_t*>(pending_.data());
        const int got = swr_convert(swr_, &dst, cap, const_cast<const uint8_t**>(frame_->extended_data), frame_->nb_samples);
        pending_.resize(static_cast<size_t>(std::max(got, 0)) * ch);
        pending_pos_ = 0;
        const AVStream* st = fmt_->streams[stream_];
        const bool has_pts = frame_->pts != AV_NOPTS_VALUE;
        const int64_t first = has_pts ? av_rescale_q(frame_->pts - start_, st->time_base, AVRational{1, info_.rate}) : next_;
        next_ = first + got;
        if (end_ > 0 && next_ > end_) {   // padding after the last sample
            const int64_t keep = std::max<int64_t>(0, end_ - first);
            pending_.resize(static_cast<size_t>(std::min<int64_t>(keep, got)) * ch);
        }
        if (target_ >= 0) {
            const int64_t drop = target_ - (has_pts ? first : target_);
            if (drop >= static_cast<int64_t>(pending_.size() / ch)) {
                pending_.clear();
                return;   // still before the target
            }
            if (drop > 0) pending_pos_ = static_cast<size_t>(drop) * ch;
            target_ = -1;
        }
    }

    bool drain_swr() {
        if (end_ > 0 && next_ >= end_) return false;
        const int ch = info_.channels;
        const int cap = swr_get_out_samples(swr_, 0);
        if (cap <= 0) return false;
        pending_.resize(static_cast<size_t>(cap) * ch);
        uint8_t* dst = reinterpret_cast<uint8_t*>(pending_.data());
        const int got = swr_convert(swr_, &dst, cap, nullptr, 0);
        pending_.resize(static_cast<size_t>(std::max(got, 0)) * ch);
        pending_pos_ = 0;
        next_ += std::max(got, 0);
        return got > 0;
    }

    static int on_read(void* u, uint8_t* buf, int n) {
        const size_t got = static_cast<FileCursor*>(u)->read(buf, static_cast<size_t>(std::max(n, 0)));
        return got > 0 ? static_cast<int>(got) : AVERROR_EOF;
    }
    static int64_t on_seek(void* u, int64_t off, int whence) {
        auto* c = static_cast<FileCursor*>(u);
        whence &= ~AVSEEK_FORCE;
        if (whence == AVSEEK_SIZE) return c->size();
        return c->seek(off, whence) ? c->tell() : AVERROR(EINVAL);
    }

    FileCursor cur_;
    AVIOContext* io_ = nullptr;
    AVFormatContext* fmt_ = nullptr;
    AVCodecContext* ctx_ = nullptr;
    SwrContext* swr_ = nullptr;
    AVPacket* pkt_ = nullptr;
    AVFrame* frame_ = nullptr;
    int stream_ = -1;
    std::vector<double> pending_;
    size_t pending_pos_ = 0;
    bool eof_ = false;
    int64_t target_ = -1;
    int64_t start_ = 0;    // stream start time (time base): frame 0
    int64_t end_ = -1;     // frames to play (MP4 gapless), -1 = until the decoder stops
    int64_t next_ = 0;     // frame index of the next decoded sample
    std::filesystem::path path_;
};

}  // namespace

std::unique_ptr<Decoder> make_ffmpeg_decoder() { return std::make_unique<FfmpegDecoder>(); }

}  // namespace daphne

#else  // !DAPHNE_HAVE_FFMPEG

namespace daphne {
namespace {

class FfmpegUnavailable final : public Decoder {
public:
    bool open(const std::filesystem::path&, std::string* error) override {
        if (error) *error = "built without FFmpeg (run scripts/fetch_ffmpeg.ps1 and reconfigure)";
        return false;
    }
    void close() override {}
    int read(double*, int) override { return 0; }
    bool seek(int64_t) override { return false; }
};

}  // namespace

std::unique_ptr<Decoder> make_ffmpeg_decoder() { return std::make_unique<FfmpegUnavailable>(); }

}  // namespace daphne

#endif
