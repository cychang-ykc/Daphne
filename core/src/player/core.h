#pragma once
// The player's processing thread and its state (N4, N25). The parts with their own state are separate
// components (common.h, report.h, timeline.h, output_session.h, change_plan.h, crossfader.h,
// track_queue.h); what ties them together is defined here and implemented by subject:
//   core.cpp        the thread: start, the playback loop, transport, device upkeep, the end
//   pcm_stream.cpp  PCM: opening the device and the chain, producing chunks, the crossfade to a rebuilt chain
//   dsd_stream.cpp  DSD files sent directly, DSD silence and the stream end
//   gapless.cpp     the next track: preparation, decoder swap, the join through a new chain, its headroom
//   changes.cpp     settings changes: in place, background rebuild, device restart

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <future>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "daphne/dsp/progress.h"
#include "daphne/dsp/quantizer.h"
#include "daphne/engine/chain.h"
#include "daphne/engine/dsd_file.h"
#include "daphne/engine/dsd_output.h"
#include "daphne/engine/filters.h"
#include "daphne/engine/player.h"
#include "daphne/engine/ring_buffer.h"
#include "daphne/platform/platform.h"
#include "change_plan.h"
#include "common.h"
#include "crossfader.h"
#include "join.h"
#include "output_session.h"
#include "report.h"
#include "throughput.h"
#include "timeline.h"
#include "track_queue.h"

namespace daphne {

using namespace playback;

namespace playback {

// What differs between PCM playback and a DSD file sent directly, for the one playback loop
// (Player::Impl::play): PcmStream and DsdDirectStream in core.cpp.
struct Stream {
    virtual ~Stream() = default;
    virtual bool apply(const Settings& ns, const OutputChoice& no) = 0;   // a settings change; false: fatal
    virtual bool poll() { return true; }                                   // background work between chunks
    virtual bool produce() = 0;                                            // the next chunk; false at the end
    virtual bool reopen() = 0;                                             // the driver asked to be reopened
};

}  // namespace playback

struct Player::Impl {
    // ------------------------------------------------------------------ state (declaration order = construction order)

    // setup
    PlayerDeps deps;
    std::filesystem::path path;
    Settings s;
    OutputChoice out;
    Report report;
    PlayReport& rep = report.rep;      // the report's parts, used throughout
    PlayCallbacks& cb = report.cb;
    Timeline tl;
    OutputSession io;                  // the device, its ring and the chunk being pushed
    TrackQueue queue;                  // the next track (set_next) and its preparation

    // threading
    std::thread th;
    std::mutex m;
    std::condition_variable cv;
    bool start_done = false, start_ok = false;
    std::string start_err;
    std::optional<std::pair<Settings, OutputChoice>> pending;
    std::atomic<bool> cancel{false}, done{false};
    std::atomic<int64_t> pos{0};
    bool ok = false;
    Stream* stream = nullptr;          // what is playing (play)
    std::string err;

    // playback (processing thread only)
    std::unique_ptr<Decoder> dec;
    std::unique_ptr<DspChain> cur;
    std::future<std::unique_ptr<DspChain>> building;
    std::future<Warm> warming;         // a built chain's warm-up round in the background (begin_crossfade)
    std::shared_ptr<std::atomic<bool>> warm_cancel;   // stops that round (take_warm)
    int warm_rounds = 0;
    int64_t warm_last = 0;             // input frames of the last round
    std::optional<std::pair<Settings, OutputChoice>> build_target, queued;
    std::vector<double> pcm, dsp, tmp_c, tmp_n;
    int64_t in_total = 0;              // source frames read since the start of the track
    int ratio = 1, ch = 0, dch = 0, want = 0;
    // chain output rate / format (= the device's, except for the dsd core: the modulator's PCM)
    int crate = 0;
    int read_frames = kChunk;          // source frames per decoder read (shorter for a live source)
    DeviceFormat chain_fmt{};
    std::unique_ptr<DsdEncoder> dsdenc;
    std::unique_ptr<DsdFile> dsdf;     // DSD file played directly (no chain)
    std::vector<uint8_t> dsd_in;
    int dsd_pcm_rate = 0;              // DSD -> PCM fallback at this rate (0 = 176.4 / 192 kHz)
    int dop_locked = 0;                // the PCM rate the device locked instead of the DoP rate (0: none)
    double start_sec = 0.0;            // Player::start position
    // transport requests (under m) and state
    std::atomic<bool> want_pause{false}, ended_flag{false};
    std::optional<double> seek_req;
    std::filesystem::path cur_path;    // the track being heard
    std::atomic<double> pos_sec{0.0};
    bool was_paused = false;           // the last pause request seen (service_transport: a live source resumed)
    std::atomic<double> meter_l{0.0}, meter_r{0.0};
    bool end_queued = false;           // the stream end (DSD silence) is queued
    std::atomic<bool> device_fail{false};
    std::atomic<bool> internal_fail{false};   // start() failed by an exception (abort_run)
    int run_osr = 0;                   // dsd core: the DSD multiple the device runs
    std::vector<uint8_t> dsd_bytes;
    bool flushed = false;
    // Only the music is played, not the filter's delay around it: at the start (open, seek) the chain's first
    // content-delay frames are dropped (counted in abs_out as if heard), at the end the zeros bring out the last
    // input frame and no more (its ringing after that is cut).
    int64_t skip_out = 0;              // chain output frames still to drop at the start
    int64_t tail_zeros = -1;           // input frames of zeros still to run through the chain at the end (-1: not begun)
    Crossfader xf;                     // a rebuilt chain fading in; the chain output carried over
    Join cross_join;                   // a gapless join through a new chain being made (cross_switch)
    History hist;                      // recent input, for a rebuilt chain's warm-up
    FadeQuantizer fq;                  // fades on integer devices (N21)
    // device restart: fade the last chunk out, the first frames after the restart in
    bool want_fade_in = false, fade_out_next = false;
    int64_t ramp_in_left = 0, ramp_in_len = 1, fade_out_len = 1;

    // background trim of the filter cache to its limit (filter_cache.h); a newer one supersedes it,
    // finished threads are joined at the end of playback
    struct PreJob {
        std::thread th;
        std::shared_ptr<std::atomic<bool>> cancel;
    };
    std::vector<PreJob> pre_jobs;
    // the background rebuild can be cancelled when a newer change supersedes it
    std::shared_ptr<std::atomic<bool>> build_cancel;
    bool build_designs = false;        // the background build designs a filter (not only loads it from the cache)
    Throughput throughput;             // the chain's speed against real time (PCM)
    std::atomic<double> rt_factor{0};  // its last factor, for other threads (Player::realtime_factor)

    // ------------------------------------------------------------------ small helpers

    std::unique_ptr<AudioOutput> open_output(const OutputRequest& req, std::string* error) { return deps.open_output(req, error); }
    OutputCaps caps() const { return deps.output_caps(out); }   // what the chosen output offers
    void event(const std::string& e) const { report.event(e); }
    // a faded frame on an integer device: TPDF re-quantization (FadeQuantizer)
    void requantize(double* frame) { fq.requantize(frame, rep.device, dch, dsdenc != nullptr); }
    // Stream input frame being heard (Timeline::heard_input).
    int64_t heard_input() const { return tl.heard_input(heard_out(), dsdf ? 0 : cur ? cur->content_delay() : 0, ratio); }
    // (the chain's tail at the end of the stream runs past the last frame: held there)
    int64_t heard_position() const {
        const int64_t p = std::max<int64_t>(0, heard_input() - tl.track_origin_in);
        return !dsdf && rep.source.frames > 0 ? std::min(p, rep.source.frames) : p;
    }
    // The source of the track being decoded (the last switched-to one, or the heard one).
    const AudioInfo& decoding_info() const { return tl.switches.empty() ? rep.source : tl.switches.back().info; }
    // what plays now, for the change decisions (change_plan.h)
    Playing playing() const { return {s, out, caps(), want, rep.source.rate, cur_path}; }

    // ------------------------------------------------------------------ core.cpp: the thread

    // The thread: run_playback, and an exception from it (a damaged file, an allocation) ends the playback with an error
    // (abort_run) instead of the process.
    void run();
    void run_playback();
    void abort_run(const std::string& what);
    // The playback loop until the end, a stop or a fatal error, then finish_playback.
    void play(Stream& st);
    void loop(Stream& st);
    class PcmStream;
    class DsdDirectStream;
    // One pass of the transport requests (processing thread). False: the playback cannot go on (err says why).
    bool service_transport();
    // Seek with the device open: pause it (PCM fades out), drop what is queued, refill, resume. False when the decoder
    // had moved on to a gapless track and this one could not be opened again (err): playing on would play the other.
    bool do_seek(double sec);
    // gapless: report tracks that are now heard
    void check_switch();
    // Chain frame being heard (the device's queue counted back from abs_out).
    int64_t heard_out() const;
    // Drops what is left of skip_out from the front of the n frames in dsp; the frames that remain.
    int drop_start(int n);
    // The device's upkeep (OutputSession::upkeep); a driver that asks to be opened again gets reopened,
    // resuming where it was.
    bool service_device();
    // diagnostics for the host's log (Report::diag_json)
    void update_diag();
    void stop_cache_trim(bool join);
    void kick_cache_trim();
    void finish_playback();

    // ------------------------------------------------------------------ pcm_stream.cpp: PCM

    // The PCM decoder for a file: DSD files reaching this point are converted to PCM.
    std::unique_ptr<Decoder> open_source(const std::filesystem::path& file, const Settings& st, std::string* e);
    // Opens the device for the current settings (pcm: stepping the rate down by powers of two
    // when the device cannot open or start it), builds the chain, prefills and starts.
    bool open_and_start(int64_t from, std::string* error);
    // One chunk into bytes; false at the end of the stream.
    bool produce();
    // Starts the crossfade to nx: warm it up on the recent input (in the background, poll_warm) and
    // align it to the current chain by content delay.
    void begin_crossfade(std::unique_ptr<DspChain> nx);
    void launch_warm(Warm w, std::vector<double> in);
    // A warm-up round done: another round, or the rest here and the crossfade starts.
    void poll_warm();
    // The warm-up stops (a seek, a restart, the end); its chain, or null.
    std::unique_ptr<DspChain> take_warm();
    // A change, a gapless join through a new chain or the next build has to wait for this one.
    bool chain_busy() const { return xf.fading || building.valid() || warming.valid() || tl.cross_pending() || cross_join.active(); }
    void finish_fade();

    // ------------------------------------------------------------------ dsd_stream.cpp: DSD files sent directly, the stream end

    // DSD files: straight to the device, every processing stage bypassed.
    // ASIO with output.dsd none / native: native DSD; otherwise DoP at the DSD rate / 16.
    bool open_dsd(int64_t from, std::string* error);
    bool produce_dsd();
    // DSD direct: only the output can change (no processing applies to DSD sent directly).
    // reset: a driver reset reopens the device as it is (nothing more is queued for it).
    bool handle_dsd(const Settings& ns, const OutputChoice& no, bool reset = false);
    // DSD only: queue about `ms` of DSD silence (keeps the DoP marker sequence going).
    void push_dsd_idle(int ms);
    // DoP only: about `ms` of plain PCM zeros (no DoP markers) so the DAC leaves its DoP-detected DSD
    // mode before the stream stops; otherwise some DACs beep on one channel when native DSD follows.
    void push_pcm_silence(int ms);
    // The end of the stream: DSD silence, then (DoP) PCM silence so the DAC leaves DSD mode;
    // queued without blocking, the loop pushes it.
    void queue_end();

    // ------------------------------------------------------------------ gapless.cpp: the next track

    // Prepares the next track (TrackQueue) for the current playback.
    void kick_prepare();
    // gapless: continue with the next track when it has the same format (called at the end of
    // the decoder's data)
    bool try_gapless();
    // Gapless through a new chain (another sample rate on the same output, or the same rate with a format the old
    // chain does not fit), joined as one filter would: zeros through the old chain
    // bring out A's last samples (its content delay) and their ringing; B's start through the new chain (built
    // ahead) is overlap-added so that B's first sample comes exactly one source period after A's last, its
    // pre-ringing over A's end. The hearing side follows when B is heard (check_switch).
    void cross_switch(std::unique_ptr<Decoder> nd, std::unique_ptr<DspChain> nc, const std::filesystem::path& np);
    // The next chunk of the join into dsp (frames); 0 when it is over (the rest is in the carry).
    int join_produce();
    // The track just switched to (switches.back()) has its own headroom (ReplayGain per track): applied from its
    // first decoded sample on (the gain stage ramps over 20 ms); the old value is kept for an undo.
    void apply_headroom(std::optional<double> h);
    // A seek or restart before a pending switch is heard: the heard track's headroom back.
    void undo_headroom();
    // A seek or restart before a pending cross switch is heard: back to the heard segment's chain.
    void undo_cross();

    // ------------------------------------------------------------------ changes.cpp: settings changes

    // Applies a pending settings change; false on a fatal error.
    bool handle(const Settings& ns, const OutputChoice& no);
    // the background rebuild (a new chain for the new settings)
    void launch_build(const Settings& ns, const OutputChoice& no);
    bool poll_build();
    // drain = false: the device is reopened as it is (a driver reset): what is queued is dropped.
    bool restart(const Settings& ns, const OutputChoice& no, bool drain = true);
};

}  // namespace daphne
