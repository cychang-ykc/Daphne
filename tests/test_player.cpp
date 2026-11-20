// The player end to end on a fake device (fake_output.h): what it sends for each kind of playback.
// These pin down the behavior of player.cpp so it can be restructured (N25): every test here must
// give the same result before and after. With DAPHNE_PLAYER_DUMP=<dir>, the timing-independent ones
// also write what the device received to <dir>/<name>.bin for a byte-for-byte comparison of two builds.
#include <catch_amalgamated.hpp>

#include <algorithm>
#include <bit>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <random>
#include <functional>
#include <future>
#include <thread>

#include <nlohmann/json.hpp>

#if defined(DAPHNE_COLOR_TEST_CURVE)
#include "daphne/color/oversampler.h"
#include "daphne/color/stage.h"
#include "daphne/color/test_curve.h"
#endif
#include "daphne/dsp/modulator.h"
#include "daphne/dsp/resampler.h"
#include "daphne/engine/dsd_file.h"
#include "daphne/engine/file_bytes.h"
#include "daphne/engine/filters.h"
#include "daphne/engine/live_source.h"
#include "daphne/engine/player.h"
#include "daphne/engine/settings.h"
#include "../core/src/player/crossfader.h"
#include "../core/src/player/dsd_switch.h"
#include "../core/src/player/throughput.h"
#include "fake_output.h"
#include "test_util.h"

using namespace daphne;
using test::FakeDevice;
using test::Signal;

namespace {

constexpr double kPi = 3.141592653589793;

// no filter cache on the test machine
const bool g_no_cache = [] {
    const std::filesystem::path off("off");
    test::set_env("DAPHNE_FILTER_CACHE", &off);
    return true;
}();

std::shared_ptr<Signal> int24_noise(int rate, int ch, int64_t frames, uint32_t seed) {
    auto s = std::make_shared<Signal>();
    s->info.rate = rate, s->info.channels = ch, s->info.bits = 24, s->info.lossless_int = true;
    s->info.frames = frames, s->info.codec = "TEST";
    // the generator's raw output, which the standard fixes; uniform_int_distribution is not (MSVC's STL, libc++
    // and libstdc++ draw different numbers), so every platform plays the same source (DAPHNE_PLAYER_DUMP compares)
    std::mt19937 g(seed);
    s->x.resize(static_cast<size_t>(frames) * ch);
    for (auto& v : s->x) v = (static_cast<int32_t>(g() >> 8) - (1 << 23)) / 8388608.0;   // 24 bits, -2^23 .. 2^23 - 1
    return s;
}

std::shared_ptr<Signal> sine(int rate, double freq, double seconds, double amp = 0.5) {
    auto s = std::make_shared<Signal>();
    const int64_t frames = static_cast<int64_t>(seconds * rate);
    s->info.rate = rate, s->info.channels = 2, s->info.frames = frames, s->info.codec = "TEST";
    s->x.resize(static_cast<size_t>(frames) * 2);
    for (int64_t i = 0; i < frames; ++i) s->x[2 * i] = s->x[2 * i + 1] = amp * std::sin(2 * kPi * freq * i / rate);
    return s;
}

// The player with the fake device and the sources by name (anything else: the real decoders).
struct Rig {
    std::shared_ptr<FakeDevice> dev = std::make_shared<FakeDevice>();
    std::map<std::string, std::shared_ptr<const Signal>> sources;
    std::mutex m;
    std::vector<std::string> events;
    int started = 0, tracks = 0, changed = 0;
    std::atomic<bool> ended{false};
    std::unique_ptr<Player> player;
    OutputCaps caps;   // what the fake output says it offers

    PlayerDeps deps() {
        PlayerDeps d;
        d.open_output = test::fake_opener(dev);
        d.output_caps = [this](const OutputChoice&) { return caps; };
        d.open_decoder = [this](const std::filesystem::path& p, std::string* e) -> std::unique_ptr<Decoder> {
            const auto it = sources.find(platform::path_to_utf8(p));
            if (it == sources.end()) return open_decoder(p, e);
            return std::make_unique<test::MemoryDecoder>(it->second);
        };
        return d;
    }
    PlayCallbacks callbacks() {
        PlayCallbacks cb;
        cb.on_start = [this](const PlayReport&) {
            std::lock_guard<std::mutex> l(m);
            started++;
        };
        cb.on_track_started = [this](const PlayReport&) {
            std::lock_guard<std::mutex> l(m);
            tracks++;
        };
        cb.on_changed = [this](const PlayReport&) {
            std::lock_guard<std::mutex> l(m);
            changed++;
        };
        cb.on_event = [this](const std::string& e) {
            std::lock_guard<std::mutex> l(m);
            events.push_back(e);
        };
        cb.on_ended = [this] { ended = true; };
        return cb;
    }
    bool start(const std::string& path, const Settings& s, double at = 0.0) {
        player = std::make_unique<Player>(deps());
        std::string e;
        const bool ok = player->start(platform::path_from_utf8(path), s, OutputChoice{}, callbacks(), &e, at);
        if (!ok) UNSCOPED_INFO("start failed: " << e);
        return ok;
    }
    // waits for the end (stops it after `limit`); the report
    PlayReport finish(double limit = 30.0) {
        const auto t0 = std::chrono::steady_clock::now();
        while (!player->finished()) {
            if (std::chrono::steady_clock::now() - t0 > std::chrono::duration<double>(limit)) player->stop();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        PlayReport r;
        std::string e;
        player->wait(&r, &e);
        return r;
    }
    bool saw(const std::string& part) {
        std::lock_guard<std::mutex> l(m);
        return std::any_of(events.begin(), events.end(), [&](const std::string& e) { return e.find(part) != std::string::npos; });
    }
    void dump(const char* name) {
        const auto dir = platform::get_env("DAPHNE_PLAYER_DUMP");
        if (!dir) return;
        const auto b = dev->take();
        std::ofstream(platform::path_from_utf8(*dir) / (std::string(name) + ".bin"), std::ios::binary)
            .write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    }
};

void sleep_s(double s) { std::this_thread::sleep_for(std::chrono::duration<double>(s)); }

// frequency from the zero crossings of channel 0 in [from, to) frames
double frequency(const std::vector<float>& y, int ch, size_t from, size_t to, double rate) {
    int crossings = 0;
    size_t first = 0, last = 0;
    for (size_t i = from + 1; i < to; ++i) {
        if ((y[(i - 1) * ch] < 0) != (y[i * ch] < 0)) {
            if (!crossings) first = i;
            last = i;
            ++crossings;
        }
    }
    return crossings > 1 ? (crossings - 1) / 2.0 / ((last - first) / rate) : 0.0;
}

// longest run of frames with |x| < eps (channel 0) in [from, to)
size_t longest_quiet(const std::vector<float>& y, int ch, size_t from, size_t to, float eps = 1e-4f) {
    size_t best = 0, run = 0;
    for (size_t i = from; i < to; ++i) {
        run = std::abs(y[i * ch]) < eps ? run + 1 : 0;
        best = std::max(best, run);
    }
    return best;
}

// the source's 24-bit samples as an Int32 device carries them
int32_t as_int32(double v) { return static_cast<int32_t>(std::lround(v * 8388608.0)) * 256; }

// the first frame of `y` (from `from`) where `want` (32 frames of `src` from frame k) appears, or -1
int64_t find_frames(const std::vector<int32_t>& y, const Signal& src, int64_t k, size_t from = 0) {
    const int ch = src.info.channels;
    const size_t n = 32 * ch;
    std::vector<int32_t> want(n);
    for (size_t i = 0; i < n; ++i) want[i] = as_int32(src.x[static_cast<size_t>(k) * ch + i]);
    auto it = std::search(y.begin() + static_cast<std::ptrdiff_t>(std::min(from * ch, y.size())), y.end(), want.begin(), want.end());
    return it == y.end() ? -1 : (it - y.begin()) / ch;
}

void use_float_device(FakeDevice& d) {
    d.type = SampleType::Float32;
    d.container_bits = d.valid_bits = 32;
}

}  // namespace

TEST_CASE("Player: a 24-bit source at its own rate is sent bit-perfectly", "[player]") {
    Rig r;
    auto a = int24_noise(44100, 2, 44100, 1);
    r.sources["a"] = a;
    Settings s;
    REQUIRE(r.start("a", s));
    const auto rep = r.finish();
    CHECK(r.ended);
    CHECK(rep.bit_perfect);
    CHECK(rep.chain.empty());
    CHECK(rep.device.rate == 44100);
    const auto y = r.dev->samples32();
    REQUIRE(y.size() >= a->x.size());
    size_t diff = 0;
    for (size_t i = 0; i < a->x.size(); ++i) diff += y[i] != as_int32(a->x[i]);
    CHECK(diff == 0);
    for (size_t i = a->x.size(); i < y.size(); ++i) diff += y[i] != 0;
    CHECK(diff == 0);
    r.dump("bitperfect");
}

TEST_CASE("Player: the debug JSON for the host's log keeps its keys", "[player]") {
    Rig r;
    r.dev->speed = 4.0;
    r.sources["a"] = int24_noise(44100, 2, 4 * 44100, 7);
    REQUIRE(r.start("a", Settings{}));
    sleep_s(0.7);   // updated every 500 ms
    const auto j = nlohmann::json::parse(r.player->debug_json(), nullptr, false);
    r.player->stop();
    r.finish();
    REQUIRE(j.is_object());
    for (const char* k : {"ring", "ring_cap", "pend", "eof", "fading", "building", "paused", "dev_paused", "callbacks", "underruns",
                          "drained", "failed", "late", "abs_out", "in"})
        CHECK(j.contains(k));
    CHECK(j["abs_out"].get<int64_t>() > 0);
}

TEST_CASE("Player: upsampling keeps the speed and the pitch", "[player]") {
    for (const char* rate : {"2x", "auto"}) {
        DYNAMIC_SECTION("output.rate " << rate) {
            Rig r;
            use_float_device(*r.dev);
            r.sources["sine"] = sine(44100, 1000.0, 1.0);
            Settings s;
            s.output.rate = rate;
            s.filter.taps = 256;
            REQUIRE(r.start("sine", s));
            const auto rep = r.finish();
            const int ratio = std::string(rate) == "2x" ? 2 : 16;
            CHECK(rep.device.rate == 44100 * ratio);
            const auto y = r.dev->samples_f32();
            const size_t frames = y.size() / 2;
            REQUIRE(frames >= static_cast<size_t>(44100 * ratio));
            CHECK(frames <= static_cast<size_t>(44100 * ratio + 2 * 256 * ratio + 2 * r.dev->buffer_frames));
            CHECK(frequency(y, 2, frames / 4, frames * 3 / 4, rep.device.rate) == Catch::Approx(1000.0).margin(0.5));
            r.dump(std::string(rate) == "2x" ? "upsample_2x" : "upsample_auto");
        }
    }
}

TEST_CASE("Player: only the music is played, not the filter's delay around it", "[player]") {
    // clicks in the middle and 10 frames before the end. The filter holds the signal back by its delay (half the
    // taps, linear phase): the start drops it, so the middle click is heard right at its own time, and the end runs
    // zeros through the filter until the last click is out, and no further (its ringing after that is cut)
    for (const auto phase : {FilterPhase::Linear, FilterPhase::Minimum}) {
        DYNAMIC_SECTION("phase " << (phase == FilterPhase::Linear ? "linear" : "minimum")) {
            Rig r;
            use_float_device(*r.dev);
            auto a = std::make_shared<Signal>();
            a->info.rate = 44100, a->info.channels = 2, a->info.frames = 44100, a->info.codec = "TEST";
            a->x.assign(2 * 44100, 0.0);
            for (const int at : {22050, 44100 - 10}) a->x[2 * at] = a->x[2 * at + 1] = 0.5;
            r.sources["click"] = a;
            Settings s;
            s.output.rate = "2x";
            s.filter.taps = 8192;
            s.filter.phase = phase;
            REQUIRE(r.start("click", s));
            r.finish();
            CHECK(r.ended);
            const auto y = r.dev->samples_f32();
            const size_t frames = y.size() / 2;
            auto peak_in = [&](size_t from, size_t to) {
                size_t at = from;
                for (size_t i = from; i < std::min(to, frames); ++i)
                    if (std::abs(y[2 * i]) > std::abs(y[2 * at])) at = i;
                return at;
            };
            const size_t mid = peak_in(0, 66150), end = peak_in(66150, frames);
            CHECK(mid == 44100);   // 22050 x 2: no delay before the music
            CHECK(end == 88180);   // (44100 - 10) x 2: the last click is played
            CHECK(std::abs(y[2 * end]) > 0.2);
            CHECK(frames >= 88200u);
            CHECK(frames <= static_cast<size_t>(88200 + 2 + 2 * r.dev->buffer_frames));   // no ringing tail on top
            CHECK(r.player->position() == 44100);   // the end of the track
        }
    }
}

TEST_CASE("Player: the device rate steps down when the device refuses it", "[player]") {
    Rig r;
    use_float_device(*r.dev);
    std::vector<int> tried;
    r.dev->accept = [&](const OutputRequest& q) {
        tried.push_back(q.rate);
        return q.rate <= 176400;
    };
    r.sources["sine"] = sine(44100, 1000.0, 0.5);
    Settings s;
    s.output.rate = "auto";
    s.filter.taps = 256;
    REQUIRE(r.start("sine", s));
    const auto rep = r.finish();
    CHECK(tried == std::vector<int>{705600, 352800, 176400});
    CHECK(rep.device.rate == 176400);
    // the refused attempts are in the log, with the driver's reason
    CHECK(r.saw("the output at 705600 Hz failed (fake: rate 705600 refused): trying 352800 Hz"));
    CHECK(r.saw("the output at 352800 Hz failed (fake: rate 352800 refused): trying 176400 Hz"));
    const auto y = r.dev->samples_f32();
    REQUIRE(y.size() / 2 >= 88200u);
    CHECK(frequency(y, 2, 10000, 80000, 176400) == Catch::Approx(1000.0).margin(0.5));
    r.dump("stepdown");
}

TEST_CASE("Player: gapless at the same rate joins the tracks sample for sample", "[player]") {
    Rig r;
    r.dev->speed = 4.0;   // set_next comes after start, as a host does: before the end of the first track
    auto a = int24_noise(44100, 2, 88200, 2), b = int24_noise(44100, 2, 17640, 3);
    r.sources["a"] = a;
    r.sources["b"] = b;
    REQUIRE(r.start("a", Settings{}));
    r.player->set_next(platform::path_from_utf8("b"));
    r.finish();
    CHECK(r.ended);
    CHECK(r.tracks == 1);
    const auto y = r.dev->samples32();
    REQUIRE(y.size() >= a->x.size() + b->x.size());
    size_t diff = 0;
    for (size_t i = 0; i < a->x.size(); ++i) diff += y[i] != as_int32(a->x[i]);
    for (size_t i = 0; i < b->x.size(); ++i) diff += y[a->x.size() + i] != as_int32(b->x[i]);
    CHECK(diff == 0);
    r.dump("gapless_same");
}

TEST_CASE("Player: gapless to another rate on the same output goes through the new chain", "[player]") {
    Rig r;
    use_float_device(*r.dev);
    r.dev->speed = 4.0;   // long enough for the next chain to be built ahead (kick_prepare)
    r.sources["a"] = sine(44100, 1000.0, 3.0);
    r.sources["b"] = sine(88200, 1000.0, 1.0);
    Settings s;
    s.output.rate = "176400";
    s.filter.taps = 256;
    REQUIRE(r.start("a", s));
    r.player->set_next(platform::path_from_utf8("b"));
    r.finish();
    CHECK(r.ended);
    CHECK(r.tracks == 1);
    CHECK(r.dev->opens.size() == 1);
    CHECK(r.saw("through its own chain"));
    const auto y = r.dev->samples_f32();
    const size_t frames = y.size() / 2;
    REQUIRE(frames >= 4 * 176400u);
    CHECK(frames <= 4 * 176400u + 4 * 256 * 4 + 2 * 512);
    // no gap at the join (3 s; the sine crosses zero every 88 frames at 176.4 kHz)
    CHECK(longest_quiet(y, 2, 1000, 4 * 176400 - 1000) < 10);
    CHECK(frequency(y, 2, 3 * 176400 + 20000, 4 * 176400 - 20000, 176400) == Catch::Approx(1000.0).margin(0.5));
    r.dump("gapless_cross");
}

TEST_CASE("Player: seek resumes at the frame asked for", "[player]") {
    Rig r;
    r.dev->speed = 4.0;
    auto a = int24_noise(44100, 2, 3 * 44100, 4);
    r.sources["a"] = a;
    REQUIRE(r.start("a", Settings{}));
    sleep_s(0.15);
    r.player->seek(2.0);
    r.finish();
    CHECK(r.ended);
    CHECK(r.saw("seek to 2"));
    const auto y = r.dev->samples32();
    // after the 20 ms fade-in the stream is the source from frame 88200 on
    const int64_t at = find_frames(y, *a, 88200 + 2000);
    REQUIRE(at > 0);
    size_t diff = 0;
    const int64_t n = 3 * 44100 - (88200 + 2000);
    for (int64_t i = 0; i < n * 2; ++i) diff += y[static_cast<size_t>(at * 2 + i)] != as_int32(a->x[static_cast<size_t>((88200 + 2000) * 2 + i)]);
    CHECK(diff == 0);
}

namespace {
// A memory decoder that says when it has been read to its end (the read that returns nothing: the player then moves
// on to the next track)
class EndFlagDecoder final : public Decoder {
public:
    EndFlagDecoder(std::shared_ptr<const Signal> s, std::atomic<bool>* at_end) : inner_(std::move(s)), at_end_(at_end) {
        info_ = inner_.info();
    }
    bool open(const std::filesystem::path& p, std::string* e) override { return inner_.open(p, e); }
    void close() override {}
    int read(double* out, int frames) override {
        const int n = inner_.read(out, frames);
        if (n == 0) *at_end_ = true;
        return n;
    }
    bool seek(int64_t frame) override { return inner_.seek(frame); }

private:
    test::MemoryDecoder inner_;
    std::atomic<bool>* at_end_;
};
}  // namespace

TEST_CASE("Player: a seek while the next track is decoded ahead goes back into this one", "[player]") {
    // the decoder moves on to the next track about 0.5 s (the ring) before it is heard. A seek then is in this track:
    // it opens it again and plays it from there (before, the seek landed in the next track). When it cannot be opened
    // again (the file went away), the playback fails instead of playing the next track at that position.
    for (const bool gone : {false, true}) {
        INFO((gone ? "the track cannot be opened again" : "the track opens again"));
        Rig r;
        r.dev->speed = 1.0;
        const auto a = int24_noise(44100, 2, 2 * 44100, 5);
        r.sources["b"] = int24_noise(44100, 2, 44100, 6);
        std::atomic<bool> a_read{false}, a_gone{false};
        PlayerDeps d = r.deps();
        d.open_decoder = [&, inner = d.open_decoder](const std::filesystem::path& p, std::string* e) -> std::unique_ptr<Decoder> {
            if (platform::path_to_utf8(p) != "a") return inner(p, e);
            if (a_gone) {
                if (e) *e = "the file is gone";
                return nullptr;
            }
            return std::make_unique<EndFlagDecoder>(a, &a_read);
        };
        Settings s;
        s.output.rate = "native";
        r.player = std::make_unique<Player>(d);
        std::string e;
        REQUIRE(r.player->start(platform::path_from_utf8("a"), s, OutputChoice{}, r.callbacks(), &e, 0.0));
        r.player->set_next(platform::path_from_utf8("b"));
        const auto t0 = std::chrono::steady_clock::now();
        while (!a_read && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5))
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        REQUIRE(a_read);
        sleep_s(0.02);   // the decoder has moved on to "b"; "a" is still heard for about the ring's 0.5 s
        REQUIRE(r.player->position_seconds() < 1.8);
        a_gone = gone;
        const size_t before = r.dev->samples32().size() / 2;   // frames played before the seek
        r.player->seek(0.5);
        while (!r.player->finished() && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(15))
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        REQUIRE(r.player->finished());
        PlayReport rep;
        std::string err;
        const bool ok = r.player->wait(&rep, &err);
        INFO(err);
        if (gone) {
            CHECK_FALSE(ok);
            CHECK(err.find("could not be opened again") != std::string::npos);
            CHECK_FALSE(r.ended);
            CHECK(r.tracks == 0);   // "b" was never heard
        } else {
            CHECK(ok);
            CHECK(r.ended);
            CHECK(r.tracks == 1);   // "b" after "a" again, once
            // after the seek "a" plays from 0.5 s on (past the 20 ms fade-in), sample for sample
            const auto y = r.dev->samples32();
            const int64_t from = 22050 + 2000;
            const int64_t at = find_frames(y, *a, from, before);   // after the seek, not "a" heard the first time
            REQUIRE(at > 0);
            size_t diff = 0;
            for (int64_t k = 0; k < 2 * 22050; ++k)
                diff += y[static_cast<size_t>(at * 2 + k)] != as_int32(a->x[static_cast<size_t>(from * 2 + k)]);
            CHECK(diff == 0);
        }
    }
}

TEST_CASE("Player: pause holds the position, resume goes on from there", "[player]") {
    Rig r;
    r.dev->speed = 4.0;
    auto a = int24_noise(44100, 2, 3 * 44100, 5);
    r.sources["a"] = a;
    REQUIRE(r.start("a", Settings{}));
    sleep_s(0.1);
    r.player->pause(true);
    sleep_s(0.15);
    const int64_t p1 = r.player->position();
    sleep_s(0.15);
    const int64_t p2 = r.player->position();
    CHECK(p1 == p2);
    CHECK(p1 > 0);
    r.player->pause(false);
    r.finish();
    CHECK(r.ended);
    // nothing lost or repeated: the tail of the source arrives intact
    const auto y = r.dev->samples32();
    const int64_t k = 3 * 44100 - 20000;
    const int64_t at = find_frames(y, *a, k);
    REQUIRE(at > 0);
    size_t diff = 0;
    for (int64_t i = 0; i < 19000 * 2; ++i) diff += y[static_cast<size_t>(at * 2 + i)] != as_int32(a->x[static_cast<size_t>(k * 2 + i)]);
    CHECK(diff == 0);
}

TEST_CASE("Player: a volume change is applied in place", "[player]") {
    Rig r;
    use_float_device(*r.dev);
    r.dev->speed = 4.0;
    // 4 s: the change reaches the processing thread well before it has decoded the end, also on a busy CI machine
    r.sources["sine"] = sine(44100, 1000.0, 4.0);
    Settings s;
    s.volume.mode = VolumeMode::Digital;
    REQUIRE(r.start("sine", s));
    sleep_s(0.1);
    s.volume.db = -20.0 * std::log10(2.0);
    r.player->apply(s, OutputChoice{});
    r.finish();
    CHECK(r.saw("applied in place"));
    CHECK(r.dev->opens.size() == 1);
    const auto y = r.dev->samples_f32();
    float peak = 0;
    for (size_t i = y.size() - 2 * 22050; i < y.size() - 2 * 2000; ++i) peak = std::max(peak, std::abs(y[i]));
    CHECK(peak == Catch::Approx(0.25).margin(0.002));
}

TEST_CASE("Player: a new filter is built in the background and crossfaded in", "[player]") {
    Rig r;
    use_float_device(*r.dev);
    r.dev->speed = 4.0;
    const int secs = 10;   // long enough for the background build and warm-up on a slow machine (a sanitizer build)
    r.sources["sine"] = sine(44100, 1000.0, secs);
    Settings s;
    s.output.rate = "2x";
    s.filter.taps = 256;
    REQUIRE(r.start("sine", s));
    sleep_s(0.1);
    s.filter.taps = 512;
    r.player->apply(s, OutputChoice{});
    r.finish();
    CHECK(r.saw("now playing with filter: 512 taps"));
    CHECK(r.dev->opens.size() == 1);
    CHECK(r.changed >= 1);
    const auto y = r.dev->samples_f32();
    const size_t frames = y.size() / 2;
    REQUIRE(frames >= secs * 88200u - 2 * 512);
    CHECK(frames <= secs * 88200u + 4 * 512 * 2 + 2 * 512);
    CHECK(longest_quiet(y, 2, 1000, secs * 88200 - 2000) < 10);
}

TEST_CASE("Player: an output rate change restarts the device where it was", "[player]") {
    Rig r;
    use_float_device(*r.dev);
    r.dev->speed = 4.0;
    r.sources["sine"] = sine(44100, 1000.0, 2.0);
    Settings s;
    s.filter.taps = 256;
    REQUIRE(r.start("sine", s));
    sleep_s(0.15);
    s.output.rate = "2x";
    r.player->apply(s, OutputChoice{});
    r.finish();
    CHECK(r.ended);
    REQUIRE(r.dev->opens.size() == 2);
    CHECK(r.dev->opens[0].rate == 44100);
    CHECK(r.dev->opens[1].rate == 88200);
    CHECK(r.started == 2);
    // the two parts together cover the track once, plus the restart's last chunk (4096 source frames, 93 ms:
    // 20 ms fade-out, then zeros), which is played again after the restart with a 20 ms fade-in
    const double secs = static_cast<double>(r.dev->open_at[1]) / 8 / 44100 + static_cast<double>(r.dev->take().size() - r.dev->open_at[1]) / 8 / 88200;
    CHECK(secs >= 2.0);
    CHECK(secs <= 2.0 + 4096.0 / 44100 + 0.03);
}

TEST_CASE("Player: a driver reset reopens the device and resumes", "[player]") {
    Rig r;
    r.dev->speed = 4.0;
    auto a = int24_noise(44100, 2, 3 * 44100, 6);
    r.sources["a"] = a;
    REQUIRE(r.start("a", Settings{}));
    sleep_s(0.15);
    r.dev->request_reset = true;
    r.finish();
    CHECK(r.ended);
    CHECK(r.dev->opens.size() == 2);
    CHECK(r.saw("reopening the device"));
    const auto y = r.dev->samples32();
    const int64_t k = 3 * 44100 - 20000;
    CHECK(find_frames(y, *a, k, r.dev->open_at[1] / 8) > 0);
}

TEST_CASE("Player: live input plays the producer's stream", "[player][live]") {
    for (const char* rate : {"native", "192000"}) {
        DYNAMIC_SECTION("output.rate " << rate) {
            Rig r;
            use_float_device(*r.dev);
            r.dev->speed = 1.0;
            const int cap = 96000;
            std::vector<uint8_t> mem(16 + static_cast<size_t>(cap) * 8, 0);
            set_live_bus(mem.data(), cap, 48000);
            live_forget();
            std::atomic<bool> stop{false};
            std::thread producer([&] {
                auto* index = reinterpret_cast<uint32_t*>(mem.data());
                auto* ring = reinterpret_cast<float*>(mem.data() + 16);
                int64_t t = 0;
                const auto t0 = std::chrono::steady_clock::now();
                while (!stop) {
                    const int64_t due =
                        static_cast<int64_t>(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() * 48000);
                    for (; t < due; ++t) {
                        const float v = static_cast<float>(0.5 * std::sin(2 * kPi * 1000.0 * t / 48000.0));
                        const size_t k = static_cast<size_t>(t % cap);
                        ring[2 * k] = v, ring[2 * k + 1] = v;
                    }
                    std::atomic_thread_fence(std::memory_order_release);
                    test::store_release(index, static_cast<uint32_t>(t));
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            });
            Settings s;
            s.output.rate = rate;
            s.filter.taps = 256;
            REQUIRE(r.start(kLivePath, s));
            // the player's part of the delay: what the live decoder has read and the device has not played yet
            double held = 0;
            for (int i = 0; i < 25; ++i) {
                sleep_s(0.1);
                if (i >= 10) held = std::max(held, live_stats().read - r.player->position_seconds());
            }
            r.player->stop();
            const auto rep = r.finish();
            stop = true;
            producer.join();
            set_live_bus(nullptr, 0, 0);
            const int dr = rep.device.rate;
            CHECK(dr == (std::string(rate) == "192000" ? 192000 : 48000));
            const auto y = r.dev->samples_f32();
            const size_t frames = y.size() / 2;
            // silence first (the player's prefill and the live decoder's 0.15 s jitter buffer), then the sine
            size_t music = 0;
            while (music < frames && std::abs(y[music * 2]) < 1e-4f) ++music;
            const size_t half = static_cast<size_t>(dr / 2), ms = static_cast<size_t>(dr / 1000);
            REQUIRE(music + half < frames);   // at least half a second of it
            CHECK(frequency(y, 2, music + 20 * ms, music + half - 20 * ms, dr) == Catch::Approx(1000.0).margin(3.0));
            CHECK(longest_quiet(y, 2, music + 20 * ms, music + half - 20 * ms) < static_cast<size_t>(dr / 4800));
            INFO("held by the player: " << held << " s");
            CHECK(held < 0.12);   // a short ring for a live source (N4), not the 0.5 s of a file
        }
    }
}

TEST_CASE("Player: a live source resumed after a pause goes back to the writer", "[player][live]") {
    Rig r;
    use_float_device(*r.dev);
    r.dev->speed = 1.0;
    const int cap = 96000;
    std::vector<uint8_t> mem(16 + static_cast<size_t>(cap) * 8, 0);
    set_live_bus(mem.data(), cap, 48000);
    live_forget();
    std::atomic<bool> stop{false};
    std::thread producer([&] {   // writes on through the pause, as the page does
        auto* index = reinterpret_cast<uint32_t*>(mem.data());
        const auto t0 = std::chrono::steady_clock::now();
        while (!stop) {
            const auto t = static_cast<uint32_t>(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() * 48000);
            std::atomic_thread_fence(std::memory_order_release);
            test::store_release(index, t);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });
    REQUIRE(r.start(kLivePath, Settings{}));
    sleep_s(1.0);
    r.player->pause(true);
    sleep_s(0.6);   // the producer gets 0.6 s ahead (the decoder does not read meanwhile: its fill is not updated)
    r.player->pause(false);
    sleep_s(0.2);
    // averaged over half a second (one sample jitters with the scheduler: 0.09 s seen once on the macOS CI
    // runner). The reserve itself is checked exactly in test_live ("only a seek leaves the reserve too"):
    // with this test's short device buffer it is about 0.06 s, too close to the scheduler's noise here
    double fill = 0;
    for (int i = 0; i < 20; ++i) {
        sleep_s(0.025);
        fill += live_stats().fill / 20;
    }
    r.player->stop();
    r.finish();
    stop = true;
    producer.join();
    set_live_bus(nullptr, 0, 0);
    CHECK(r.saw("live input: back to the writer"));
    INFO("fill after the resume: " << fill << " s");
    CHECK(fill > 0.0);    // not run dry
    CHECK(fill < 0.25);   // about the 0.15 s target again, not 0.75 s
}

TEST_CASE("Player: a live source starts with the target buffered, not the target plus the refill", "[player][live]") {
    // the live decoder returns silence until it primes, so the player's ring is already full by then
    Rig r;
    use_float_device(*r.dev);
    r.dev->speed = 1.0;
    r.dev->buffer_frames = 2400;   // 50 ms (MIKU's default): a ring of 0.1 s, a reserve of 0.11 s
    const int cap = 96000;
    std::vector<uint8_t> mem(16 + static_cast<size_t>(cap) * 8, 0);
    set_live_bus(mem.data(), cap, 48000);
    live_forget();
    std::atomic<bool> stop{false};
    std::thread producer([&] {
        auto* index = reinterpret_cast<uint32_t*>(mem.data());
        const auto t0 = std::chrono::steady_clock::now();
        while (!stop) {
            const auto t = static_cast<uint32_t>(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() * 48000);
            std::atomic_thread_fence(std::memory_order_release);
            test::store_release(index, t);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });
    REQUIRE(r.start(kLivePath, Settings{}));
    sleep_s(1.0);
    // averaged over a second; the fill is the one seen by the last read, and the player reads 5 at a time
    // when a device buffer frees: about the target less those reads (0.11 s measured)
    double fill = 0;
    for (int i = 0; i < 40; ++i) {
        sleep_s(0.025);
        fill += live_stats().fill / 40;
    }
    r.player->stop();
    r.finish();
    stop = true;
    producer.join();
    set_live_bus(nullptr, 0, 0);
    INFO("decoder fill: " << fill << " s");
    CHECK(fill > 0.0);    // not run dry (a busy CI runner's sleeps make the sampled average vary: 0.04 s seen on macOS)
    CHECK(fill < 0.16);   // with the reserve left in the decoder: 0.21 s measured
}

namespace {

// a DSF file: stereo DSD64, `bytes` per channel of a pattern without 0x69 (DSD silence)
std::filesystem::path write_dsf(const std::string& name, int64_t bytes) {
    auto le = [](std::vector<uint8_t>& v, uint64_t x, int n) {
        for (int k = 0; k < n; ++k) v.push_back(static_cast<uint8_t>(x >> (8 * k)));
    };
    const int ch = 2, block = 4096;
    const int64_t blocks = (bytes + block - 1) / block;
    const uint64_t data = static_cast<uint64_t>(blocks) * block * ch;
    std::vector<uint8_t> f;
    f.insert(f.end(), {'D', 'S', 'D', ' '});
    le(f, 28, 8), le(f, 28 + 52 + 12 + data, 8), le(f, 0, 8);
    f.insert(f.end(), {'f', 'm', 't', ' '});
    le(f, 52, 8), le(f, 1, 4), le(f, 0, 4), le(f, 2, 4), le(f, ch, 4), le(f, 2822400, 4), le(f, 1, 4);
    le(f, static_cast<uint64_t>(bytes) * 8, 8), le(f, block, 4), le(f, 0, 4);
    f.insert(f.end(), {'d', 'a', 't', 'a'});
    le(f, 12 + data, 8);
    for (int64_t b = 0; b < blocks; ++b)
        for (int c = 0; c < ch; ++c)
            for (int i = 0; i < block; ++i) {
                const int64_t k = b * block + i;
                uint8_t v = k < bytes ? static_cast<uint8_t>(0x11 + (k * 7 + c * 3) % 0x50) : 0x69;
                f.push_back(v);
            }
    const auto p = test::temp_path(name);
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(f.data()), static_cast<std::streamsize>(f.size()));
    return p;
}

// DoP frames (Int32: 24-bit word << 8): the marker byte of channel 0 of every frame
std::vector<uint8_t> dop_markers(const std::vector<uint8_t>& b, int ch) {
    std::vector<uint8_t> m;
    for (size_t i = 0; i + 4 * ch <= b.size(); i += 4 * ch) m.push_back(b[i + 3]);
    return m;
}

}  // namespace

TEST_CASE("Player: a DSF file is sent to a DoP device untouched", "[player][dsd]") {
    Rig r;
    const int64_t bytes = 2822400 / 8 / 2;   // 0.5 s per channel
    const auto path = write_dsf("daphne_player_test.dsf", bytes);
    Settings s;
    s.output.dsd_lead_in_ms = 20;
    REQUIRE(r.start(platform::path_to_utf8(path), s));
    const auto rep = r.finish();
    CHECK(r.ended);
    CHECK(rep.device.rate == 176400);
    CHECK(rep.bit_perfect);
    const auto b = r.dev->take();
    const auto m = dop_markers(b, 2);
    // the markers alternate 0x05 / 0xFA through the lead-in silence and the music
    size_t music = 0, bad = 0;
    for (size_t i = 0; i < m.size(); ++i) {
        if (m[i] != 0x05 && m[i] != 0xFA) break;
        if (i && m[i] == m[i - 1]) ++bad;
        const uint8_t hi = b[i * 8 + 2], lo = b[i * 8 + 1];
        if (hi != 0x69 || lo != 0x69) ++music;
    }
    CHECK(bad == 0);
    CHECK(music == static_cast<size_t>(bytes / 2));
    r.player.reset();   // it keeps the file open (Windows cannot remove an open file)
    std::error_code ec;
    std::filesystem::remove(path, ec);
    r.dump("dsf_dop");
}

TEST_CASE("Player: a DSF file on a device that stays below the DoP rate is converted to the rate it locked", "[player][dsd]") {
    // as seen on a USB DAC just plugged back in: asked for 176.4 kHz (DoP DSD64), it locked 88.2 kHz. The usual
    // 176.4 kHz PCM could not play on it (the chain only goes up)
    Rig r;
    r.dev->lock_rate = 88200;
    const int64_t bytes = 2822400 / 8 / 2;   // 0.5 s per channel
    const auto path = write_dsf("daphne_player_locked.dsf", bytes);
    Settings s;
    s.output.dsd_lead_in_ms = 20;
    REQUIRE(r.start(platform::path_to_utf8(path), s));
    const auto rep = r.finish();
    CHECK(r.ended);
    CHECK(rep.device.rate == 88200);
    CHECK(r.saw("fake: stays at 88200 Hz"));   // the device's own reason reaches the log
    CHECK(r.saw("converting at 88200 Hz, the rate the device locked"));
    CHECK(r.dev->frames() >= 88200 / 2 - 1024);
    r.player.reset();
    std::error_code ec;
    std::filesystem::remove(path, ec);
    r.dump("dsf_locked");
}

TEST_CASE("Player: the dsd core sends PCM as DoP", "[player][dsd]") {
    Rig r;
    r.sources["sine"] = sine(44100, 1000.0, 0.5);
    Settings s;
    s.core = CoreMode::Dsd;
    s.modulator.osr = 64;
    s.filter.taps = 256;
    s.output.dsd_lead_in_ms = 20;
    REQUIRE(r.start("sine", s));
    const auto rep = r.finish();
    CHECK(r.ended);
    CHECK(rep.device.rate == 176400);
    const auto m = dop_markers(r.dev->take(), 2);
    size_t bad = 0, n = 0;
    for (size_t i = 1; i < m.size() && (m[i] == 0x05 || m[i] == 0xFA); ++i, ++n) bad += m[i] == m[i - 1];
    CHECK(bad == 0);
    CHECK(n >= 176400 / 2);
    r.dump("dsd_core");
}

TEST_CASE("Player: the output's caps decide the DSD path, not its API", "[player][dsd]") {
    SECTION("an output that takes DSD gets the DSF file as is") {
        Rig r;
        r.caps.native_dsd = true;
        const auto path = write_dsf("daphne_player_caps.dsf", 2822400 / 8 / 2 / 5);   // 0.1 s per channel
        Settings s;
        s.output.dsd_lead_in_ms = 20;
        REQUIRE(r.start(platform::path_to_utf8(path), s));
        r.finish();
        REQUIRE(!r.dev->opens.empty());
        CHECK(r.dev->opens.front().dsd);
        CHECK(r.dev->opens.front().rate == 2822400);
        r.player.reset();
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    SECTION("a shared mixer refuses the dsd core") {
        Rig r;
        r.caps.shared_mix = true;
        r.sources["sine"] = sine(44100, 1000.0, 0.2);
        Settings s;
        s.core = CoreMode::Dsd;
        s.modulator.osr = 64;
        CHECK_FALSE(r.start("sine", s));
        CHECK(r.dev->opens.empty());
    }
    SECTION("a shared mixer is never bit-perfect, though the samples it gets are") {
        Rig r;
        r.caps.shared_mix = true;
        r.sources["a"] = int24_noise(44100, 2, 44100 / 5, 1);
        REQUIRE(r.start("a", Settings{}));
        const auto rep = r.finish();
        CHECK(rep.chain.empty());
        CHECK_FALSE(rep.bit_perfect);
        CHECK(rep.not_bit_perfect_reason == "shared mode (system mixer)");
    }
}

// Tube Color (doc/N28): where it cannot run (every build but the test builds, before M2), color.enabled true must
// change nothing: a DSD file still goes out as DSD (not converted to PCM), and every byte is the same as with it off.
TEST_CASE("Player: color.enabled true changes nothing where Tube Color cannot run", "[player][dsd][color]") {
    Settings probe;
    probe.color.enabled = true;
    if (daphne::color_active(probe)) SKIP("a test build: Tube Color runs here (the M1 stage tests cover it)");
    auto play = [](bool native, bool color, const std::filesystem::path& path) {
        Rig r;
        r.caps.native_dsd = native;
        Settings s;
        s.output.dsd_lead_in_ms = 20;
        s.color.enabled = color;
        s.color.drive = 2.0;
        REQUIRE(r.start(platform::path_to_utf8(path), s));
        const auto rep = r.finish();
        CHECK(r.ended);
        REQUIRE(!r.dev->opens.empty());
        struct Out {
            bool dsd;
            int rate;
            bool bit_perfect;
            std::vector<std::string> chain;
            std::vector<uint8_t> bytes;
        } o{r.dev->opens.front().dsd, r.dev->opens.front().rate, rep.bit_perfect, rep.chain, r.dev->take()};
        r.player.reset();
        return o;
    };
    const auto path = write_dsf("daphne_player_color.dsf", 2822400 / 8 / 2 / 5);   // 0.1 s per channel
    for (bool native : {true, false}) {
        INFO((native ? "native DSD device" : "DoP device"));
        const auto off = play(native, false, path);
        const auto on = play(native, true, path);
        CHECK(on.dsd == native);                 // native: DSD; DoP: DSD packed in PCM frames, never converted
        CHECK(on.rate == off.rate);
        CHECK(on.bit_perfect);
        CHECK(on.chain == off.chain);
        CHECK(on.bytes.size() > 0);
        CHECK(on.bytes == off.bytes);
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
    // a PCM source the same way
    auto play_pcm = [](bool color) {
        Rig r;
        r.sources["a"] = int24_noise(44100, 2, 44100 / 5, 1);
        Settings s;
        s.color.enabled = color;
        REQUIRE(r.start("a", s));
        const auto rep = r.finish();
        CHECK(rep.bit_perfect);
        return r.dev->take();
    };
    CHECK(play_pcm(true) == play_pcm(false));
}


namespace {

// A DSF file (DSD64, stereo) whose bytes count its DoP frames: DoP frame f carries bytes 2f and 2f + 1 of each
// channel; channel 0 holds f's low and middle byte, channel 1 its high byte and 0x5A. Sent directly, every frame
// tells which frame of the file it is (DSF stores each byte LSB first; the player sends it MSB first)
uint8_t reverse_bits(uint8_t x) {
    uint8_t r = 0;
    for (int i = 0; i < 8; ++i) r = static_cast<uint8_t>(r | (((x >> i) & 1) << (7 - i)));
    return r;
}

std::filesystem::path write_counting_dsf(const std::string& name, int64_t bytes, int dsd_rate = 2822400) {
    auto le = [](std::vector<uint8_t>& v, uint64_t x, int n) {
        for (int k = 0; k < n; ++k) v.push_back(static_cast<uint8_t>(x >> (8 * k)));
    };
    const int ch = 2, block = 4096;
    const int64_t blocks = (bytes + block - 1) / block;
    const uint64_t data = static_cast<uint64_t>(blocks) * block * ch;
    std::vector<uint8_t> f;
    f.insert(f.end(), {'D', 'S', 'D', ' '});
    le(f, 28, 8), le(f, 28 + 52 + 12 + data, 8), le(f, 0, 8);
    f.insert(f.end(), {'f', 'm', 't', ' '});
    le(f, 52, 8), le(f, 1, 4), le(f, 0, 4), le(f, 2, 4), le(f, ch, 4), le(f, static_cast<uint64_t>(dsd_rate), 4), le(f, 1, 4);
    le(f, static_cast<uint64_t>(bytes) * 8, 8), le(f, block, 4), le(f, 0, 4);
    f.insert(f.end(), {'d', 'a', 't', 'a'});
    le(f, 12 + data, 8);
    for (int64_t b = 0; b < blocks; ++b)
        for (int c = 0; c < ch; ++c)
            for (int i = 0; i < block; ++i) {
                const int64_t k = b * block + i, fr = k / 2;
                uint8_t v = 0x69;
                if (k < bytes) v = static_cast<uint8_t>(c == 0 ? (k % 2 == 0 ? fr : fr >> 8) : (k % 2 == 0 ? fr >> 16 : 0x5A));
                f.push_back(v);
            }
    const auto p = test::temp_path(name);
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(f.data()), static_cast<std::streamsize>(f.size()));
    return p;
}

// A DSF file (stereo) of these DSD bytes (MSB first, as the player sends them; DSF stores each byte LSB first)
std::filesystem::path write_dsf_bytes(const std::string& name, const std::vector<uint8_t>& l, const std::vector<uint8_t>& r, int dsd_rate) {
    auto le = [](std::vector<uint8_t>& v, uint64_t x, int n) {
        for (int k = 0; k < n; ++k) v.push_back(static_cast<uint8_t>(x >> (8 * k)));
    };
    const int ch = 2, block = 4096;
    const auto bytes = static_cast<int64_t>(std::min(l.size(), r.size()));
    const int64_t blocks = (bytes + block - 1) / block;
    const uint64_t data = static_cast<uint64_t>(blocks) * block * ch;
    std::vector<uint8_t> f;
    f.insert(f.end(), {'D', 'S', 'D', ' '});
    le(f, 28, 8), le(f, 28 + 52 + 12 + data, 8), le(f, 0, 8);
    f.insert(f.end(), {'f', 'm', 't', ' '});
    le(f, 52, 8), le(f, 1, 4), le(f, 0, 4), le(f, 2, 4), le(f, ch, 4), le(f, static_cast<uint64_t>(dsd_rate), 4), le(f, 1, 4);
    le(f, static_cast<uint64_t>(bytes) * 8, 8), le(f, block, 4), le(f, 0, 4);
    f.insert(f.end(), {'d', 'a', 't', 'a'});
    le(f, 12 + data, 8);
    for (int64_t b = 0; b < blocks; ++b)
        for (int c = 0; c < ch; ++c)
            for (int i = 0; i < block; ++i) {
                const int64_t k = b * block + i;
                const auto& v = c == 0 ? l : r;
                f.push_back(k < bytes ? reverse_bits(v[static_cast<size_t>(k)]) : 0x69);
            }
    const auto p = test::temp_path(name);
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(f.data()), static_cast<std::streamsize>(f.size()));
    return p;
}


bool wait_for(const std::function<bool()>& f, double limit = 10.0) {
    const auto t0 = std::chrono::steady_clock::now();
    while (!f()) {
        if (std::chrono::steady_clock::now() - t0 > std::chrono::duration<double>(limit)) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

int starts(Rig& r) {
    std::lock_guard<std::mutex> l(r.m);
    return r.started;
}

int count_events(Rig& r, const std::string& part) {
    std::lock_guard<std::mutex> l(r.m);
    return static_cast<int>(
        std::count_if(r.events.begin(), r.events.end(), [&](const std::string& e) { return e.find(part) != std::string::npos; }));
}

bool color_runs() {
    Settings probe;
    probe.color.enabled = true;
    return daphne::color_active(probe);
}

// a block: what the player decodes at a time (kChunk source frames), at the converted PCM rate (176.4 kHz)
constexpr double kBlockSec = 4096.0 / 176400.0;

}  // namespace

namespace {

// A DSF file (DSD64, stereo) of a sine: `amp` x full scale PCM through the modulator (full scale -> 50 % modulation)
std::filesystem::path write_sine_dsf(const std::string& name, double seconds, double hz, double amp, std::vector<uint8_t>* bits0) {
    const int pcm_rate = 352800, frames = static_cast<int>(seconds * pcm_rate);
    std::vector<double> x(static_cast<size_t>(frames) * 2);
    for (int i = 0; i < frames; ++i)
        x[2 * static_cast<size_t>(i)] = x[2 * static_cast<size_t>(i) + 1] = amp * std::sin(2 * 3.14159265358979323846 * hz * i / pcm_rate);
    daphne::dsp::DsdModulator mod(pcm_rate, 64, 8);
    std::vector<uint8_t> out;   // interleaved L R bytes, MSB first
    mod.process(x.data(), frames, 2, out);
    const int64_t bytes = static_cast<int64_t>(out.size() / 2);
    if (bits0) {
        bits0->clear();
        for (int64_t k = 0; k < bytes; ++k) bits0->push_back(out[2 * static_cast<size_t>(k)]);
    }
    auto le = [](std::vector<uint8_t>& v, uint64_t v64, int n) {
        for (int k = 0; k < n; ++k) v.push_back(static_cast<uint8_t>(v64 >> (8 * k)));
    };
    const int ch = 2, block = 4096;
    const int64_t blocks = (bytes + block - 1) / block;
    const uint64_t data = static_cast<uint64_t>(blocks) * block * ch;
    std::vector<uint8_t> f;
    f.insert(f.end(), {'D', 'S', 'D', ' '});
    le(f, 28, 8), le(f, 28 + 52 + 12 + data, 8), le(f, 0, 8);
    f.insert(f.end(), {'f', 'm', 't', ' '});
    le(f, 52, 8), le(f, 1, 4), le(f, 0, 4), le(f, 2, 4), le(f, ch, 4), le(f, 2822400, 4), le(f, 1, 4);
    le(f, static_cast<uint64_t>(bytes) * 8, 8), le(f, block, 4), le(f, 0, 4);
    f.insert(f.end(), {'d', 'a', 't', 'a'});
    le(f, 12 + data, 8);
    for (int64_t b = 0; b < blocks; ++b)
        for (int c = 0; c < ch; ++c)
            for (int i = 0; i < block; ++i) {
                const int64_t k = b * block + i;
                f.push_back(k < bytes ? reverse_bits(out[2 * static_cast<size_t>(k) + static_cast<size_t>(c)]) : 0x69);   // DSF: LSB first
            }
    const auto p = test::temp_path(name);
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(f.data()), static_cast<std::streamsize>(f.size()));
    return p;
}

// Channel 0's DSD bytes (MSB first) carried by DoP frames (stereo, Int32); idle and non-DoP frames give nothing
std::vector<uint8_t> dop_bytes_ch0(const std::vector<uint8_t>& b, size_t* first_music = nullptr) {
    std::vector<uint8_t> v;
    bool music = false;
    for (size_t i = 0; i + 8 <= b.size(); i += 8) {
        if (b[i + 3] != 0x05 && b[i + 3] != 0xFA) continue;
        const uint8_t hi = b[i + 2], lo = b[i + 1];
        if (!music && hi == 0x69 && lo == 0x69) continue;   // the lead-in
        if (!music && first_music) *first_music = i / 8;
        music = true;
        v.push_back(hi);
        v.push_back(lo);
    }
    return v;
}

// The amplitude of `hz` in a DSD bit stream (bytes MSB first, +-1 per bit) over [from, from + n) bits
double dsd_amplitude(const std::vector<uint8_t>& bytes, double hz, size_t from, size_t n) {
    double c = 0.0, s = 0.0;
    for (size_t k = 0; k < n; ++k) {
        const size_t bit = from + k;
        const double v = (bytes[bit / 8] >> (7 - bit % 8)) & 1 ? 1.0 : -1.0;
        const double w = 2 * 3.14159265358979323846 * hz * static_cast<double>(k) / 2822400.0;
        c += v * std::cos(w);
        s += v * std::sin(w);
    }
    return 2.0 * std::hypot(c, s) / static_cast<double>(n);
}

}  // namespace

// Tube Color does not change the output format (doc/N28): a DSD file that goes out as DSD (native or DoP) is
// remodulated at its own DSD multiple while Tube Color is active; only an output that takes no DSD gets PCM.
TEST_CASE("Player: a DSD file with Tube Color is remodulated at its own rate", "[player][dsd][color]") {
    Settings probe;
    probe.color.enabled = true;
    if (!daphne::color_active(probe)) SKIP("a release build: Tube Color cannot run here (the test above covers it)");
    const auto path = write_dsf("daphne_player_color_tc.dsf", 2822400 / 8 / 2 / 5);   // 0.1 s per channel
    SECTION("native and DoP: still DSD, at the file's rate") {
        for (bool native : {true, false}) {
            INFO((native ? "native DSD device" : "DoP device"));
            Rig r;
            r.caps.native_dsd = native;
            Settings s;
            s.output.dsd_lead_in_ms = 20;
            s.color.enabled = true;
            REQUIRE(r.start(platform::path_to_utf8(path), s));
            const auto rep = r.finish();
            CHECK(r.ended);
            REQUIRE(r.dev->opens.size() == 1);
            CHECK(r.dev->opens.front().dsd == native);
            CHECK(r.dev->opens.front().rate == (native ? 2822400 : 176400));
            CHECK(r.saw("DSD remodulated (Tube Color)"));
            CHECK_FALSE(r.saw("DSD converted to PCM"));
            CHECK(rep.source.dsd_rate == 2822400);
            CHECK(std::find(rep.chain.begin(), rep.chain.end(), std::string("tube color")) != rep.chain.end());
            CHECK(rep.filter.find("DSD64") != std::string::npos);
            if (!native) {   // DoP: every frame carries its marker, alternating
                // (up to the PCM silence the stream ends on, which takes the DAC out of DSD mode)
                const auto m = dop_markers(r.dev->take(), 2);
                REQUIRE(m.size() > 1000);
                size_t bad = 0, n = 0;
                for (size_t i = 1; i < m.size() && (m[i] == 0x05 || m[i] == 0xFA); ++i, ++n) bad += m[i] == m[i - 1];
                CHECK(bad == 0);
                CHECK(n >= 176400 / 10);
            }
            r.player.reset();
        }
    }
    SECTION("an output that takes no DSD: PCM") {
        Rig r;
        Settings s;
        s.output.dsd = daphne::DsdMode::Pcm;
        s.color.enabled = true;
        REQUIRE(r.start(platform::path_to_utf8(path), s));
        r.finish();
        CHECK(r.ended);
        REQUIRE(!r.dev->opens.empty());
        CHECK_FALSE(r.dev->opens.front().dsd);
        CHECK(r.saw("DSD converted to PCM (Tube Color)"));
        CHECK_FALSE(r.saw("remodulated"));
        r.player.reset();
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

// DSD -> PCM is x2 (+6.02 dB, 50 % modulation -> 0 dBFS), the modulator x0.5: remodulated, a DSD file keeps its level
TEST_CASE("Player: a remodulated DSD file keeps its level", "[player][dsd][color]") {
    Settings probe;
    probe.color.enabled = true;
    if (!daphne::color_active(probe)) SKIP("a release build: Tube Color cannot run here");
    const double hz = 1102.5;   // 2560 bits per period at DSD64
    std::vector<uint8_t> in_bits;
    const auto path = write_sine_dsf("daphne_player_color_level.dsf", 1.0, hz, 0.5, &in_bits);
    Rig r;
    Settings s;
    s.output.dsd_lead_in_ms = 20;
    s.color.enabled = true;
    s.color.drive = 0.0;   // the stage passes the signal unchanged: only the path is measured
    REQUIRE(r.start(platform::path_to_utf8(path), s));
    r.finish();
    CHECK(r.ended);
    CHECK(r.saw("DSD remodulated (Tube Color)"));
    const auto out_bits = dop_bytes_ch0(r.dev->take());
    const size_t n = 2560 * 441;   // 0.4 s, whole periods
    REQUIRE(out_bits.size() * 8 > 2822400 / 10 * 5 + n);
    const double a_in = dsd_amplitude(in_bits, hz, 2822400 / 10 * 3, n);
    const double a_out = dsd_amplitude(out_bits, hz, 2822400 / 10 * 4, n);   // after the 20 ms fade-in and the lead-in
    const double db = 20.0 * std::log10(a_out / a_in);
    UNSCOPED_INFO("1102.5 Hz in the DSD stream: in " << a_in << ", out " << a_out << " (" << db << " dB)");
    CHECK(std::abs(db) < 0.01);
    r.player.reset();
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

// Tube Color switched on and off while a DSD file plays (doc/N28): the engine goes on at the position heard, as PCM
// through the chain or sent directly again, without the host loading the file again.
TEST_CASE("Player: Tube Color switched while a DSD file plays keeps the position", "[player][dsd][color]") {
    if (!color_runs()) SKIP("a release build: Tube Color cannot run here");
    const auto path = write_counting_dsf("daphne_color_switch_pos.dsf", 2822400 / 8);   // 1 s per channel
    for (bool native : {true, false}) {
        INFO((native ? "native DSD device" : "DoP device"));
        Rig r;
        r.caps.native_dsd = native;
        r.dev->speed = 1.0;
        Settings off;
        off.output.dsd_lead_in_ms = 20;
        Settings on = off;
        on.color.enabled = true;
        REQUIRE(r.start(platform::path_to_utf8(path), off));
        sleep_s(0.4);
        // paused, the position does not move by itself: what the switch does to it is all there is
        r.player->pause(true);
        sleep_s(0.1);
        const double p0 = r.player->position_seconds();
        r.player->apply(on, OutputChoice{});
        REQUIRE(wait_for([&] { return starts(r) >= 2; }));
        sleep_s(0.05);
        const double p1 = r.player->position_seconds();
        r.player->apply(off, OutputChoice{});
        REQUIRE(wait_for([&] { return starts(r) >= 3; }));
        sleep_s(0.05);
        const double p2 = r.player->position_seconds();
        UNSCOPED_INFO("positions " << p0 << " -> " << p1 << " -> " << p2 << " s");
        CHECK(p0 > 0.05);
        CHECK(std::abs(p1 - p0) <= kBlockSec);
        CHECK(std::abs(p2 - p1) <= kBlockSec);
        CHECK(r.saw("DSD remodulated (Tube Color)"));
        CHECK(r.saw("Tube Color off: the DSD file is sent directly again"));
        r.player->pause(false);
        r.finish();
        CHECK(r.ended);
        // the device: sent directly, remodulated, sent directly again, all at the file's rate (native: DSD64, DoP:
        // 176.4 kHz frames)
        REQUIRE(r.dev->opens.size() == 3);
        for (const auto& o : r.dev->opens) {
            CHECK(o.dsd == native);
            CHECK(o.rate == (native ? 2822400 : 176400));
        }
        r.player.reset();
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
}


// A PCM file on the dsd core (DSD upsampling): Tube Color goes in the chain before the modulator, switched on by a
// rebuild and crossfade like any other chain change; the device stays as it is.
TEST_CASE("Player: Tube Color on the dsd core only rebuilds the chain", "[player][dsd][color]") {
    if (!color_runs()) SKIP("a release build: Tube Color cannot run here");
    for (bool native : {true, false}) {
        INFO((native ? "native DSD device" : "DoP device"));
        Rig r;
        r.caps.native_dsd = native;
        r.dev->speed = 1.0;
        r.sources["sine"] = sine(44100, 1000.0, 2.0);
        Settings off;
        off.core = daphne::CoreMode::Dsd;
        off.modulator.osr = 64;
        off.filter.taps = 256;
        off.output.dsd_lead_in_ms = 20;
        Settings on = off;
        on.color.enabled = true;
        REQUIRE(r.start("sine", off));
        sleep_s(0.3);
        r.player->apply(on, OutputChoice{});
        REQUIRE(wait_for([&] { return r.saw("now playing with filter"); }));
        const auto rep = r.finish();
        CHECK(r.ended);
        CHECK(r.dev->opens.size() == 1);   // never reopened
        CHECK_FALSE(r.saw("remodulated"));
        CHECK_FALSE(r.saw("converted to PCM"));
        CHECK(std::find(rep.chain.begin(), rep.chain.end(), std::string("tube color")) != rep.chain.end());
        if (!native) {   // DoP throughout: the markers never break (up to the PCM silence at the end)
            const auto m = dop_markers(r.dev->take(), 2);
            size_t bad = 0, n = 0;
            for (size_t i = 1; i < m.size() && (m[i] == 0x05 || m[i] == 0xFA); ++i, ++n) bad += m[i] == m[i - 1];
            CHECK(bad == 0);
            CHECK(n >= 176400);
        }
        r.player.reset();
    }
}

// The drive crossing 0 takes the stage in or out of the signal (a rebuild and crossfade, doc/N28): it waits 300 ms for
// the slider to settle. Other drive changes glide in place.
// The drive crossing 0 is taken in place by the stage (doc/N28): no rebuild, the chain and the device as they are.
TEST_CASE("Player: Tube Color's drive crossing 0 is taken in place", "[player][color]") {
    if (!color_runs()) SKIP("a release build: Tube Color cannot run here");
    Rig r;
    r.dev->speed = 1.0;
    r.sources["sine"] = sine(44100, 1000.0, 2.0);
    Settings s;
    s.color.enabled = true;
    s.color.drive = 0.5;
    Settings z = s, more = s;
    z.color.drive = 0.0;
    more.color.drive = 1.4;
    REQUIRE(r.start("sine", s));
    sleep_s(0.3);
    r.player->apply(more, OutputChoice{});
    sleep_s(0.1);
    r.player->apply(z, OutputChoice{});
    sleep_s(0.1);
    r.player->apply(s, OutputChoice{});
    sleep_s(0.1);
    r.player->apply(z, OutputChoice{});
    const auto rep = r.finish();
    CHECK(r.ended);
    CHECK(count_events(r, "applied in place") == 4);
    CHECK(count_events(r, "in the background") == 0);
    CHECK(r.dev->opens.size() == 1);
    CHECK(std::find(rep.chain.begin(), rep.chain.end(), std::string("tube color")) == rep.chain.end());   // idle at the end
    r.player.reset();
}

namespace {

// pink noise (Paul Kellet's filter on uniform white noise) at about `rms`, plus a sine; stereo, the same on both sides
std::shared_ptr<Signal> pink_and_sine(int rate, double seconds, double rms, double sine_amp, double hz) {
    auto s = std::make_shared<Signal>();
    const int64_t frames = static_cast<int64_t>(seconds * rate);
    s->info.rate = rate, s->info.channels = 2, s->info.frames = frames, s->info.codec = "TEST";
    s->x.resize(static_cast<size_t>(frames) * 2);
    std::mt19937 g(7);
    std::uniform_real_distribution<double> u(-1.0, 1.0);
    double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
    std::vector<double> p(static_cast<size_t>(frames));
    double acc = 0.0;
    for (int64_t i = 0; i < frames; ++i) {
        const double w = u(g);
        b0 = 0.99886 * b0 + w * 0.0555179, b1 = 0.99332 * b1 + w * 0.0750759, b2 = 0.96900 * b2 + w * 0.1538520;
        b3 = 0.86650 * b3 + w * 0.3104856, b4 = 0.55000 * b4 + w * 0.5329522, b5 = -0.7616 * b5 - w * 0.0168980;
        p[static_cast<size_t>(i)] = b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362;
        b6 = w * 0.115926;
        acc += p[static_cast<size_t>(i)] * p[static_cast<size_t>(i)];
    }
    const double k = rms / std::sqrt(acc / static_cast<double>(frames));
    for (int64_t i = 0; i < frames; ++i)
        s->x[2 * static_cast<size_t>(i)] = s->x[2 * static_cast<size_t>(i) + 1] =
            k * p[static_cast<size_t>(i)] + sine_amp * std::sin(2 * kPi * hz * static_cast<double>(i) / rate);
    return s;
}

// channel 0 of an Int32 stereo device stream
std::vector<double> ch0_int32(const std::vector<uint8_t>& b) {
    std::vector<double> y;
    for (size_t i = 0; i + 8 <= b.size(); i += 8) {
        int32_t v;
        std::memcpy(&v, &b[i], 4);
        y.push_back(v / 2147483648.0);
    }
    return y;
}

}  // namespace

// Tube Color's drive crossing 0 where the stage runs oversampled (44.1 kHz: 8x, 223 frames of content delay): the
// stage crossfades its delay path and its processing path aligned (doc/N28), so the output stays where the input is (the
// cross-correlation peak does not move) and the switches show no comb notch.
TEST_CASE("Player: Tube Color's drive crossing 0 stays aligned where the stage is oversampled", "[player][color]") {
    if (!color_runs()) SKIP("a release build: Tube Color cannot run here");
    const int rate = 44100;
    Rig r;
    r.dev->speed = 1.0;
    r.sources["pink"] = pink_and_sine(rate, 3.5, 0.003, 0.003, 997.0);   // about -50 dBFS: the curve stays linear
    Settings s;
    s.color.enabled = true;
    s.color.drive = 0.5;
    Settings z = s;
    z.color.drive = 0.0;
    REQUIRE(r.start("pink", s));
    sleep_s(0.6);
    r.player->apply(z, OutputChoice{});
    sleep_s(0.4);
    r.player->apply(s, OutputChoice{});
    const auto rep = r.finish();
    CHECK(r.ended);
    const auto y = ch0_int32(r.dev->take());
    const auto& src = r.sources["pink"]->x;
    std::vector<double> x(src.size() / 2);
    for (size_t i = 0; i < x.size(); ++i) x[i] = src[2 * i];
    REQUIRE(y.size() >= x.size() - 4096);
    // the lag of the cross-correlation peak, window by window (20 ms), on the first differences (pink noise whitened: its
    // low end, where the test response turns the phase, does not blur the peak)
    std::vector<double> dx(x.size(), 0.0), dy(y.size(), 0.0);
    for (size_t i = 1; i < x.size(); ++i) dx[i] = x[i] - x[i - 1];
    for (size_t i = 1; i < y.size(); ++i) dy[i] = y[i] - y[i - 1];
    std::map<int, int> lags;
    const int win = rate / 50, span = 400;
    for (size_t at = static_cast<size_t>(rate / 10); at + win + span < std::min(x.size(), y.size()); at += win) {
        double best = -1e300;
        int lag = 0;
        for (int l = -span; l <= span; ++l) {
            double c = 0.0;
            for (int i = 0; i < win; ++i) c += dy[at + static_cast<size_t>(i)] * dx[static_cast<size_t>(static_cast<int64_t>(at) + i - l)];
            if (c > best) best = c, lag = l;
        }
        ++lags[lag];
    }
    std::string seen;
    for (const auto& [l, n] : lags) seen += std::to_string(l) + " (" + std::to_string(n) + " windows) ";
    WARN("cross-correlation peak, by 20 ms window: " << seen);
    CHECK(lags.size() == 1);
    CHECK(lags.begin()->first == 0);
    // the power ratio |Y / X|^2 in third-octave bands from 200 Hz to 15 kHz, 46 ms windows every 12 ms (each 50 ms
    // crossfade falls in several): between the two chains' responses (0 dB without the stage, down to -0.3 dB with it
    // here), never the comb of a misaligned mix (223 frames apart, half and half: -1 to -3.7 dB in these bands)
    const int n = 2048;
    double lowest = 0.0, highest = -1e9;
    std::vector<double> cen;
    for (double c = 200.0; c <= 15000.0; c *= std::pow(2.0, 1.0 / 3.0)) cen.push_back(c);
    std::vector<std::complex<double>> X(static_cast<size_t>(n / 2 + 1)), Y(X.size());
    for (size_t at = static_cast<size_t>(rate / 10); at + n < std::min(x.size(), y.size()); at += n / 4) {
        for (int k = 0; k <= n / 2; ++k) {   // (a plain DFT of the window: the bins the bands need)
            if (k * static_cast<double>(rate) / n < 170.0 || k * static_cast<double>(rate) / n > 17000.0) continue;
            std::complex<double> xs = 0.0, ys = 0.0;
            for (int i = 0; i < n; ++i) {
                const double w = 0.5 - 0.5 * std::cos(2 * kPi * i / n);
                const auto e = std::polar(w, -2 * kPi * k * i / n);
                xs += x[at + static_cast<size_t>(i)] * e;
                ys += y[at + static_cast<size_t>(i)] * e;
            }
            X[static_cast<size_t>(k)] = xs, Y[static_cast<size_t>(k)] = ys;
        }
        for (double c : cen) {
            double px = 0.0, py = 0.0;
            for (int k = 0; k <= n / 2; ++k) {
                const double f = k * static_cast<double>(rate) / n;
                if (f < c / std::pow(2.0, 1.0 / 6.0) || f >= c * std::pow(2.0, 1.0 / 6.0)) continue;
                px += std::norm(X[static_cast<size_t>(k)]), py += std::norm(Y[static_cast<size_t>(k)]);
            }
            const double db = 10 * std::log10(py / px);
            lowest = std::min(lowest, db), highest = std::max(highest, db);
        }
    }
    WARN("third-octave |Y / X|, 200 Hz .. 15 kHz, over every window: " << lowest << " .. " << highest << " dB");
    CHECK(lowest > -0.8);
    CHECK(highest < 0.5);
    CHECK(count_events(r, "in the background") == 0);   // in place
    CHECK(r.dev->opens.size() == 1);
    r.player.reset();
}

// Benchmarks (not run by default): Tube Color's reaction off drive 0 with a 1M-tap filter, and the oversampling's cost.
// DAPHNE_BENCH_FILTER_CACHE=<dir>: the filter cache they use (the first run designs the filter).

// The benchmarks run with a filter cache (the other tests turn it off): DAPHNE_BENCH_FILTER_CACHE=<dir>
struct BenchCache {
    BenchCache() {
        const auto d = platform::get_env("DAPHNE_BENCH_FILTER_CACHE");
        if (!d) SKIP("DAPHNE_BENCH_FILTER_CACHE=<dir> runs it");
        const std::filesystem::path p = platform::path_from_utf8(*d);
        test::set_env("DAPHNE_FILTER_CACHE", &p);
    }
    ~BenchCache() {
        const std::filesystem::path off("off");
        test::set_env("DAPHNE_FILTER_CACHE", &off);
    }
};

TEST_CASE("Bench: a 1M-tap chain build and speed with Tube Color", "[.bench][color]") {
    if (!color_runs()) SKIP("a release build: Tube Color cannot run here");
    BenchCache cache;
    using clock = std::chrono::steady_clock;
    auto secs = [](clock::time_point a) { return std::chrono::duration<double>(clock::now() - a).count(); };
    daphne::AudioInfo src;
    src.rate = 44100, src.channels = 2, src.bits = 0, src.lossless_int = false, src.codec = "test";
    daphne::DeviceFormat dev;
    dev.rate = 88200, dev.channels = 2, dev.type = daphne::SampleType::Int32, dev.container_bits = dev.valid_bits = 32;
    Settings base;
    base.output.rate = "88200";
    base.filter.taps = 1048576;
    {   // the first time: designed and stored
        const auto t0 = clock::now();
        const auto h = daphne::design_filter(base.filter, 44100, 88200);
        WARN("1M taps: first design_filter " << secs(t0) << " s (cached now: " << daphne::filter_cached(base.filter, 44100, 88200) << ")");
    }
    // the parts, from the cache
    auto t0 = clock::now();
    auto h = daphne::design_filter(base.filter, 44100, 88200);
    const double load = secs(t0);
    t0 = clock::now();
    daphne::dsp::Resampler rs(std::move(h), 2, daphne::dsp::resampler_block_size(1048576LL * 2));
    rs.prepare({44100, 2, 4096});
    const double fft = secs(t0);
    WARN("1M taps from the cache: coefficients loaded in " << load << " s, the resampler's FFT partitions set up in " << fft << " s");
    for (int k = 0; k < 3; ++k) {
        Settings s = base;
        s.color.enabled = k > 0;
        s.color.drive = k == 2 ? 0.5 : 0.0;
        t0 = clock::now();
        daphne::DspChain c;
        std::string e;
        REQUIRE(c.build(s, src, dev, 4096, &e));
        const double build = secs(t0);
        std::vector<double> x(4096 * 2), y(static_cast<size_t>(c.max_output_frames()) * 2);
        for (size_t i = 0; i < x.size(); ++i) x[i] = 0.1 * std::sin(0.01 * static_cast<double>(i));
        t0 = clock::now();
        const int blocks = 3 * 44100 / 4096;   // 3 s: what a warm-up runs through at most
        for (int b = 0; b < blocks; ++b) c.process(x.data(), 4096, y.data());
        const double run = secs(t0);
        WARN("1M taps, " << (k == 0 ? "Tube Color off" : k == 1 ? "Tube Color on, drive 0" : "Tube Color on, drive 0.5") << ": chain build " << build
                         << " s, 3 s of input through it " << run << " s (" << 3.0 / run << " x real time)");
    }
}

TEST_CASE("Bench: Tube Color off drive 0 with 1M taps", "[.bench][color]") {
    if (!color_runs()) SKIP("a release build: Tube Color cannot run here");
    BenchCache cache;
    Rig r;
    r.dev->speed = 1.0;
    r.sources["pink"] = pink_and_sine(44100, 30.0, 0.05, 0.05, 997.0);
    Settings z;
    z.output.rate = "88200";   // 2x: the stage at 88.2 kHz, oversampled 4x
    z.filter.taps = 1048576;
    z.color.enabled = true;
    z.color.drive = 0.0;
    Settings s = z;
    s.color.drive = 0.5;
    REQUIRE(r.start("pink", z, 0.0));
    sleep_s(3.0);
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    r.player->apply(s, OutputChoice{});
    auto at = [&](const std::string& what) {
        REQUIRE(wait_for([&] { return r.saw(what); }, 60.0));
        return std::chrono::duration<double>(clock::now() - t0).count();
    };
    const double t_applied = at("applied in place");
    const std::string d = r.player->debug_json();
    const auto ring = std::stod(d.substr(d.find("\"ring\":") + 7));
    const double ring_s = ring / (8.0 * 88200.0);   // Int32 stereo
    // the stage switches paths (20 ms) and glides (20 ms) from the next chunk on, behind what the ring already holds
    WARN("off drive 0 at 1M taps: applied in place after " << t_applied << " s (no rebuild: " << !r.saw("in the background")
                                                           << "); the ring holds " << ring_s << " s: heard after about " << t_applied + ring_s
                                                           << " s, the full drive 40 ms later");
    r.player->stop();
    r.finish();
}

#if defined(DAPHNE_COLOR_TEST_CURVE)
TEST_CASE("Bench: Tube Color cost with and without the oversampling", "[.bench][color]") {
    for (int rate : {44100, 88200, 176400, 352800}) {
        const int frames = rate * 10, block = 4096;
        std::vector<double> x(static_cast<size_t>(frames) * 2), y(x.size());
        for (size_t i = 0; i < x.size(); ++i) x[i] = 0.3 * std::sin(0.001 * static_cast<double>(i));
        auto time = [&](auto&& run) {
            const auto t0 = std::chrono::steady_clock::now();
            run();
            return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() / 10.0;   // per second of audio
        };
        daphne::color::TubeColorStage st(daphne::color::StageParams{.drive = 0.5, .linear = daphne::color::test_linear_model()});
        st.prepare({rate, 2, block});
        const double full = time([&] {
            for (int a = 0; a < frames; a += block)
                st.process(x.data() + 2 * static_cast<size_t>(a), std::min(block, frames - a), y.data() + 2 * static_cast<size_t>(a));
        });
        double os_only = 0.0;
        const int l = daphne::color::Oversampler::factor(rate);
        if (l > 1) {
            daphne::color::Oversampler os;
            os.prepare(rate, l, 2, block);
            std::vector<double> hi(static_cast<size_t>(l) * 2);
            os_only = time([&] {
                for (int i = 0; i < frames; ++i) {
                    os.up(x.data() + 2 * static_cast<size_t>(i), hi.data());
                    os.down(hi.data(), y.data() + 2 * static_cast<size_t>(i));
                }
            });
        }
        WARN(rate << " Hz stereo, L " << l << ": the stage " << 100 * full << " % of one core, the oversampling alone " << 100 * os_only << " %");
    }
}
#endif

// The playback path says when Tube Color is set but does nothing (doc/N28).
TEST_CASE("Player: the path report says when Tube Color does nothing", "[player][color]") {
    Rig r;
    r.sources["sine"] = int24_noise(44100, 2, 44100 / 5, 1);
    Settings s;
    s.color.enabled = true;
    s.color.drive = 0.0;
    REQUIRE(r.start("sine", s));
    const auto rep = r.finish();
    INFO(rep.filter << " | " << rep.not_bit_perfect_reason);
    if (color_runs()) {
        CHECK(rep.filter.find("Tube Color idle (drive 0)") != std::string::npos);
        CHECK(rep.bit_perfect);   // a 24-bit source: nothing changes its samples
    } else {
        CHECK(rep.filter.find("Tube Color unavailable (before M2)") != std::string::npos);
    }
    Rig r2;
    r2.sources["sine"] = sine(44100, 1000.0, 0.3);
    s.color.drive = 0.5;
    REQUIRE(r2.start("sine", s));
    const auto rep2 = r2.finish();
    INFO(rep2.filter);
    CHECK(rep2.filter.find("idle") == std::string::npos);
    CHECK((rep2.filter.find("unavailable") == std::string::npos) == color_runs());
}

// Tube Color on at drive 0 (doc/N28): the stage a pure delay the player compensates, the quantizer it keeps passing the
// samples through: every byte as with Tube Color off, at a rate oversampled 8x and one oversampled 4x.
TEST_CASE("Player: Tube Color at drive 0 is bit-perfect", "[player][color]") {
    if (!color_runs()) SKIP("a release build: Tube Color cannot run here (covered above)");
    for (int rate : {44100, 96000}) {
        INFO(rate << " Hz");
        auto play = [&](bool color) {
            Rig r;
            r.sources["a"] = int24_noise(rate, 2, rate / 2, 9);
            Settings s;
            s.color.enabled = color;
            s.color.drive = 0.0;
            REQUIRE(r.start("a", s));
            const auto rep = r.finish();
            CHECK(r.ended);
            CHECK(rep.bit_perfect);
            CHECK(rep.device.rate == rate);
            auto b = r.dev->take();
            r.player.reset();
            return b;
        };
        const auto off = play(false), on = play(true);
        REQUIRE(off.size() > static_cast<size_t>(rate) / 2 * 8);
        CHECK(on == off);
    }
}

namespace {

// A DSF file of real music: pink noise and a sine (about -20 dBFS) modulated to DSD64. l, r: its bytes, MSB first.
std::filesystem::path write_music_dsf(const std::string& name, double seconds, std::vector<uint8_t>* l, std::vector<uint8_t>* r) {
    const int pcm_rate = 352800;
    const auto sig = pink_and_sine(pcm_rate, seconds, 0.1, 0.1, 441.0);
    const int frames = static_cast<int>(sig->x.size() / 2);
    daphne::dsp::DsdModulator mod(pcm_rate, 64, 8);
    std::vector<uint8_t> out;   // interleaved L R bytes, MSB first
    mod.process(sig->x.data(), frames, 2, out);
    l->clear(), r->clear();
    for (size_t k = 0; k + 1 < out.size(); k += 2) l->push_back(out[k]), r->push_back(out[k + 1]);
    return write_dsf_bytes(name, *l, *r, 2822400);
}

// The DSD bytes (MSB first) the DoP frames of a stereo Int32 stream carry, frame by frame; marker_ok: they alternate
// from the first to the last (before the PCM silence the stream ends on)
struct DopPayload {
    std::vector<uint8_t> l, r;
    size_t frames = 0;   // DoP frames
    bool markers_alternate = true;
};
DopPayload dop_payload(const std::vector<uint8_t>& b) {
    DopPayload p;
    int last = -1;
    for (size_t i = 0; i + 8 <= b.size(); i += 8) {
        const uint8_t mk = b[i + 3];
        if (mk != 0x05 && mk != 0xFA) {
            if (last >= 0) break;   // the end
            continue;
        }
        if (mk == last) p.markers_alternate = false;
        last = mk;
        p.l.push_back(b[i + 2]), p.l.push_back(b[i + 1]);
        p.r.push_back(b[i + 6]), p.r.push_back(b[i + 5]);
        ++p.frames;
    }
    return p;
}

// Channel 0 of a DSD stream (bytes MSB first, both channels) as PCM at 176.4 kHz: Daphne's DSD -> PCM converter
std::vector<double> dsd_to_pcm(const std::vector<uint8_t>& l, const std::vector<uint8_t>& r, const std::string& name) {
    const auto path = write_dsf_bytes(name, l, r, 2822400);
    auto d = daphne::make_dsd_pcm_decoder(0, 24);
    std::string e;
    REQUIRE(d->open(path, &e));
    std::vector<double> buf(8192 * 2), y;
    for (int n; (n = d->read(buf.data(), 8192)) > 0;)
        for (int i = 0; i < n; ++i) y.push_back(buf[2 * static_cast<size_t>(i)]);
    d.reset();
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return y;
}

// The 10 ms windows' cross-correlation peaks of y against x (first differences), over [from, to) of x, lags +-span
std::map<int, int> window_lags(const std::vector<double>& x, const std::vector<double>& y, size_t from, size_t to, int win, int span) {
    std::map<int, int> lags;
    for (size_t at = std::max<size_t>(from, static_cast<size_t>(span) + 1); at + win + span + 1 < std::min({to, x.size(), y.size()}); at += win) {
        double best = -1e300;
        int lag = 0;
        for (int l = -span; l <= span; ++l) {
            double c = 0.0;
            for (int i = 0; i < win; ++i) {
                const size_t a = at + static_cast<size_t>(i), bb = static_cast<size_t>(static_cast<int64_t>(a) - l);
                c += (y[a] - y[a - 1]) * (x[bb] - x[bb - 1]);
            }
            if (c > best) best = c, lag = l;
        }
        ++lags[lag];
    }
    return lags;
}

// The checks of a seamless switch on and back off (DoP): one device open, no break in the DoP marker sequence, no
// silence, the direct stretches the file's own bytes at one alignment (so nothing lost or repeated anywhere), and the
// remodulated stretch where the file is (decoded to PCM: the cross-correlation peak at lag 0 in every 10 ms window).
void check_seamless(Rig& r, const std::vector<uint8_t>& fl, const std::vector<uint8_t>& fr, const std::string& tag) {
    REQUIRE(r.dev->opens.size() == 1);
    const auto p = dop_payload(r.dev->take());
    CHECK(p.markers_alternate);
    // where the file starts in the stream (after the lead-in silence)
    size_t off = SIZE_MAX;
    for (size_t i = 0; i + 64 <= p.l.size() && off == SIZE_MAX; i += 2)
        if (std::equal(fl.begin(), fl.begin() + 64, p.l.begin() + static_cast<std::ptrdiff_t>(i))) off = i;
    REQUIRE(off != SIZE_MAX);
    // the file's bytes at that one alignment: from the start up to the switch on, and from the switch off to the end
    const size_t n = std::min(fl.size(), p.l.size() - off);
    size_t head = 0, tail = 0;
    while (head < n && p.l[off + head] == fl[head] && p.r[off + head] == fr[head]) ++head;
    while (tail < n && p.l[off + n - 1 - tail] == fl[n - 1 - tail] && p.r[off + n - 1 - tail] == fr[n - 1 - tail]) ++tail;
    UNSCOPED_INFO(tag << ": sent directly up to byte " << head << ", remodulated to byte " << n - tail << ", directly again to "
                      << n << " (the file has " << fl.size() << ")");
    CHECK(n == fl.size());                      // the whole file came out, no more and no less
    CHECK(head > 2822400 / 8 / 4);              // a direct stretch before
    CHECK(tail > 2822400 / 8 / 4);              // and after: at the same alignment, so no byte lost or repeated between
    CHECK(head + tail < n - 2822400 / 8 / 10);  // with a remodulated stretch between them
    // no silence anywhere in the music (a run of DSD idle bytes)
    size_t run = 0, longest = 0;
    for (size_t i = off; i < off + n; ++i) {
        run = p.l[i] == 0x69 && p.r[i] == 0x69 ? run + 1 : 0;
        longest = std::max(longest, run);
    }
    CHECK(longest < 8);
    // the content: the stream and the file as PCM, lined up by the same alignment
    const auto got = dsd_to_pcm(std::vector<uint8_t>(p.l.begin() + static_cast<std::ptrdiff_t>(off), p.l.begin() + static_cast<std::ptrdiff_t>(off + n)),
                                std::vector<uint8_t>(p.r.begin() + static_cast<std::ptrdiff_t>(off), p.r.begin() + static_cast<std::ptrdiff_t>(off + n)),
                                "daphne_6b_got_" + tag + ".dsf");
    const auto ref = dsd_to_pcm(fl, fr, "daphne_6b_ref_" + tag + ".dsf");
    const size_t from = head / 2 > 4410 ? head / 2 - 4410 : 0, to = std::min(got.size(), (n - tail) / 2 + 4410);   // the remodulated stretch, 25 ms either side
    const auto lags = window_lags(ref, got, from, to, 1764, 40);
    std::string seen;
    for (const auto& [l, k] : lags) seen += std::to_string(l) + " (" + std::to_string(k) + " windows) ";
    WARN(tag << ": cross-correlation peak, by 10 ms window across the remodulated stretch and both switches: " << seen
             << "| direct up to byte " << head << ", from byte " << n - tail);
    CHECK(lags.size() == 1);
    CHECK(lags.begin()->first == 0);
}

}  // namespace

// Tube Color switched on and off while a DSD file plays (doc/N28, M1 6b): on the open device, the remodulation prepared
// while the file plays on directly takes over at a DSD byte, and the file's own bytes take over again at the byte the
// modulator would have made next.
TEST_CASE("Player: Tube Color switched while a DSD file plays, without reopening the device", "[player][dsd][color]") {
    if (!color_runs()) SKIP("a release build: Tube Color cannot run here");
    std::vector<uint8_t> fl, fr;
    const auto path = write_music_dsf("daphne_color_6b.dsf", 4.0, &fl, &fr);   // (room after the switch off on a busy machine)
    Settings off;
    off.output.dsd_lead_in_ms = 20;
    // the switch's alignment is measured at drive 1 % (the response all but flat, doc/N28): a higher drive adds the
    // amplifier's own phase (its output transformer's low-pass: some 1.5 samples of group delay in the test response)
    Settings on = off;
    on.color.enabled = true;
    on.color.drive = 0.01;
    SECTION("DoP: aligned to the byte") {
        Rig r;
        r.dev->speed = 1.0;
        REQUIRE(r.start(platform::path_to_utf8(path), off));
        sleep_s(0.5);
        r.player->apply(on, OutputChoice{});
        REQUIRE(wait_for([&] { return r.saw("taken over at DSD byte"); }, 5.0));
        sleep_s(0.6);
        r.player->apply(off, OutputChoice{});
        REQUIRE(wait_for([&] { return r.saw("sent directly again from DSD byte"); }, 5.0));
        r.finish();
        CHECK(r.ended);
        check_seamless(r, fl, fr, "dop");   // (one device open among its checks)
        r.player.reset();
    }
    SECTION("the preparation late: the switch moves on, the file plays on") {
        daphne::playback::g_remod_prep_delay_ms = 700;   // more than the 0.4 s lead
        Rig r;
        r.dev->speed = 1.0;
        REQUIRE(r.start(platform::path_to_utf8(path), off));
        sleep_s(0.3);
        r.player->apply(on, OutputChoice{});
        REQUIRE(wait_for([&] { return r.saw("taken over at DSD byte"); }, 5.0));
        daphne::playback::g_remod_prep_delay_ms = 0;
        CHECK(r.saw("not ready at its point; it takes over later"));
        sleep_s(0.5);
        r.player->apply(off, OutputChoice{});
        REQUIRE(wait_for([&] { return r.saw("sent directly again from DSD byte"); }, 5.0));
        r.finish();
        CHECK(r.ended);
        check_seamless(r, fl, fr, "late");
        r.player.reset();
    }
    SECTION("native DSD: the device opened once") {
        Rig r;
        r.caps.native_dsd = true;
        r.dev->speed = 1.0;
        REQUIRE(r.start(platform::path_to_utf8(path), off));
        sleep_s(0.5);
        r.player->apply(on, OutputChoice{});
        REQUIRE(wait_for([&] { return r.saw("taken over at DSD byte"); }, 5.0));
        sleep_s(0.5);
        r.player->apply(off, OutputChoice{});
        REQUIRE(wait_for([&] { return r.saw("sent directly again from DSD byte"); }, 5.0));
        r.finish();
        CHECK(r.ended);
        CHECK(r.dev->opens.size() == 1);
        CHECK(r.dev->opens.front().dsd);
        r.player.reset();
    }
    daphne::playback::g_remod_prep_delay_ms = 0;
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

// A 48 kHz-family DSD file: the modulator runs at 44.1 kHz x osr, so its remodulation changes the rate: the device is
// reopened as before (doc/N28, a known limitation).
TEST_CASE("Player: Tube Color on a 48 kHz-family DSD file reopens the device", "[player][dsd][color]") {
    if (!color_runs()) SKIP("a release build: Tube Color cannot run here");
    const auto path = write_counting_dsf("daphne_color_48k.dsf", 3072000 / 8, 3072000);
    Rig r;
    r.dev->speed = 1.0;
    Settings off;
    off.output.dsd_lead_in_ms = 20;
    Settings on = off;
    on.color.enabled = true;
    REQUIRE(r.start(platform::path_to_utf8(path), off));
    sleep_s(0.3);
    r.player->apply(on, OutputChoice{});
    REQUIRE(wait_for([&] { return starts(r) >= 2; }));
    r.finish();
    CHECK(r.ended);
    CHECK(r.dev->opens.size() == 2);
    CHECK_FALSE(r.saw("taken over at DSD byte"));
    r.player.reset();
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("Player: Tube Color switched three times within 100 ms on a DSD file", "[player][dsd][color]") {
    if (!color_runs()) SKIP("a release build: Tube Color cannot run here");
    const auto path = write_counting_dsf("daphne_color_switch_fast.dsf", 2822400 / 8 * 2);   // 2 s
    auto spin = [](double s) {   // (a sleep can overshoot by tens of ms on a busy machine)
        const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(s);
        while (std::chrono::steady_clock::now() < until) {
        }
    };
    for (bool native : {true, false}) {
        INFO((native ? "native DSD device" : "DoP device"));
        Rig r;
        r.caps.native_dsd = native;
        r.dev->speed = 1.0;
        Settings off;
        off.output.dsd_lead_in_ms = 20;
        Settings on = off;
        on.color.enabled = true;
        REQUIRE(r.start(platform::path_to_utf8(path), off));
        sleep_s(0.2);
        const auto t0 = std::chrono::steady_clock::now();
        r.player->apply(on, OutputChoice{});
        spin(0.03);
        r.player->apply(off, OutputChoice{});
        spin(0.03);
        r.player->apply(on, OutputChoice{});
        const auto took = std::chrono::steady_clock::now() - t0;
        const double took_ms = std::chrono::duration<double>(took).count() * 1000.0;
        UNSCOPED_INFO("three switches in " << took_ms << " ms");
        CHECK(took < std::chrono::milliseconds(100));
        // the last setting (on) takes over, once, on the open device; nothing hangs
        REQUIRE(wait_for([&] { return r.saw("taken over at DSD byte"); }, 5.0));
        r.finish(10.0);
        CHECK(r.ended);
        CHECK(r.dev->opens.size() == 1);
        CHECK(count_events(r, "taken over at DSD byte") == 1);
        r.player.reset();
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("Crossfader: a warm-up in rounds starts the same crossfade as one warm-up", "[player][crossfade]") {
    // 44.1 -> 88.2 kHz on a 16-bit device: a resampler and a TPDF quantizer (the dither depends on every
    // frame the chain has made)
    AudioInfo src{};
    src.rate = 44100, src.channels = 2, src.bits = 16, src.lossless_int = true, src.codec = "test";
    DeviceFormat dev{};
    dev.rate = 88200, dev.channels = 2, dev.type = SampleType::Int16, dev.container_bits = 16, dev.valid_bits = 16;
    Settings s;
    s.filter.taps = 512;
    auto chain = [&] {
        auto c = std::make_unique<DspChain>();
        std::string e;
        REQUIRE(c->build(playback::chain_settings(s), src, dev, playback::kChunk, &e));
        return c;
    };
    playback::History h;
    h.reset(2);
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> u(-0.5, 0.5);
    std::vector<double> x(2 * 30000);
    for (auto& v : x) v = u(rng);
    h.store(x.data(), 30000, 0);
    const int ratio = 2, cur_delay = 300;
    const int64_t fade = 4410;
    // the playing chain plays on between the rounds
    const int64_t abs_first = 2 * 12000, abs_last = 2 * 29000;

    playback::Crossfader one;
    one.set_channels(2);
    one.begin(chain(), h, 30000, abs_last, cur_delay, ratio, fade);

    playback::Crossfader rounds;
    rounds.set_channels(2);
    std::vector<double> in;
    auto w = playback::Crossfader::prepare(chain(), h, 12500, abs_first, cur_delay, ratio, 2, &in);
    w.feed(in.data(), 12500 - w.in_done, 2);
    REQUIRE(playback::copy_history(h, w.in_done, 21000, &in));
    w.feed(in.data(), 21000 - w.in_done, 2);
    REQUIRE(playback::copy_history(h, w.in_done, 30000, &in));
    w.feed(in.data(), 30000 - w.in_done, 2);
    rounds.begin(std::move(w), abs_last, cur_delay, fade);

    CHECK(rounds.drop_next == one.drop_next);
    CHECK(rounds.delay_shift == one.delay_shift);
    REQUIRE(rounds.fn.frames() == one.fn.frames());
    CHECK(std::equal(one.fn.data(), one.fn.data() + 2 * one.fn.frames(), rounds.fn.data()));
    // the history has moved past what a round still needs
    CHECK_FALSE(playback::copy_history(h, 0, h.cap + 1, &in));

    // a cancelled round stops at once (a seek, a restart or the end does not wait for it)
    std::atomic<bool> stop{true};
    auto wc = playback::Crossfader::prepare(chain(), h, 30000, abs_last, cur_delay, ratio, 2, &in);
    const int64_t at = wc.in_done;
    wc.cancel = &stop;
    wc.feed(in.data(), 30000 - wc.in_done, 2);
    CHECK(wc.in_done == at);
}

TEST_CASE("PoolThreadScope: a std::async task leaves its thread as it found it", "[player]") {
    // MSVC runs std::async tasks on a thread pool and reuses the threads: a priority or FTZ set by one task
    // would reach the next (a filter design)
    const uint64_t fp0 = std::async(std::launch::async, [] { return platform::fp_control(); }).get();
    for (int i = 0; i < 5; ++i) {
        std::async(std::launch::async, [] {
            playback::PoolThreadScope pool;
            platform::set_thread_priority(platform::Priority::BelowNormal);
            platform::set_fp_control(platform::with_flush_to_zero(platform::fp_control()));
        }).get();
        const auto [prio, fp] = std::async(std::launch::async, [] { return std::make_pair(platform::thread_priority(), platform::fp_control()); }).get();
        CHECK(prio == platform::Priority::Normal);
        CHECK(fp == fp0);
    }
}

TEST_CASE("Player: gapless to another rate keeps the next track in time with a long filter", "[player]") {
    // the old chain's tail (about twice its delay) is longer than the first chunk through the new chain:
    // the new chain's output has to cover all of it, or the next track starts again after the tail
    Rig r;
    use_float_device(*r.dev);
    r.dev->speed = 4.0;
    const int secs = 10;   // long enough for the 8192-tap chain to be built ahead on a slow machine (CI)
    r.sources["a"] = sine(44100, 1000.0, secs);
    r.sources["b"] = sine(88200, 1000.0, 1.0);   // the same 1 kHz, its phase continuing from a's end
    Settings s;
    s.output.rate = "176400";
    s.filter.taps = 8192;
    REQUIRE(r.start("a", s));
    r.player->set_next(platform::path_from_utf8("b"));
    r.finish();
    REQUIRE(r.tracks == 1);
    const auto y = r.dev->samples_f32();
    const size_t join = static_cast<size_t>(secs) * 176400;
    REQUIRE(y.size() / 2 > join + 100000);
    // the phase of the 1 kHz sine before and after the join (the filter delay is the same on both sides)
    auto phase = [&](size_t from, size_t to) {
        double c = 0, sn = 0;
        for (size_t i = from; i < to; ++i) {
            const double w = 2 * kPi * 1000.0 * static_cast<double>(i) / 176400.0;
            c += y[2 * i] * std::cos(w), sn += y[2 * i] * std::sin(w);
        }
        return std::atan2(c, sn);
    };
    const double d = std::remainder(phase(join + 60000, join + 100000) - phase(join - 60000, join - 20000), 2 * kPi);
    CHECK(std::abs(d) < 0.1);   // 12491 frames late before the fix: 1.13 rad
    CHECK(longest_quiet(y, 2, join - 50000, join + 50000) < 10);
}

TEST_CASE("Throughput: the factor per 2 s window, one warning per slow stretch", "[player][throughput]") {
    using playback::Throughput;
    using namespace std::chrono_literals;
    Throughput t;
    CHECK(t.factor() == 0.0);
    t.add(100ms, 1.0);
    CHECK(t.factor() == 0.0);   // the window is not full yet
    t.add(100ms, 1.0);
    CHECK(t.factor() == Catch::Approx(10.0));
    CHECK_FALSE(t.warn());
    // slow: one window below 1.2x is not enough, two are
    t.add(2000ms, 2.0);
    CHECK(t.factor() == Catch::Approx(1.0));
    CHECK_FALSE(t.warn());
    t.add(2500ms, 2.0);
    CHECK(t.warn());
    CHECK_FALSE(t.warn());   // once
    t.add(2500ms, 2.0);
    t.add(2500ms, 2.0);
    CHECK_FALSE(t.warn());   // still the same stretch
    // recovered above 1.5x, then slow again: warned again
    t.add(1000ms, 2.0);
    t.add(2500ms, 2.0);
    t.add(2500ms, 2.0);
    CHECK(t.warn());
}

TEST_CASE("Player: the report says how fast the chain runs", "[player][throughput]") {
    Rig r;
    use_float_device(*r.dev);
    r.sources["sine"] = sine(44100, 1000.0, 3.0);
    Settings s;
    s.output.rate = "2x";
    s.filter.taps = 256;
    REQUIRE(r.start("sine", s));
    const auto rep = r.finish();
    CHECK(rep.realtime_factor > 1.5);   // a short filter: far faster than real time on any machine
    CHECK_FALSE(r.saw("real time (it needs"));
}


namespace {

// A source that throws after `after` frames (a decoder hitting a bad allocation or a library error mid-file).
class ThrowingDecoder final : public Decoder {
public:
    ThrowingDecoder(std::shared_ptr<const Signal> s, int64_t after) : inner_(std::move(s)), after_(after) { info_ = inner_.info(); }
    bool open(const std::filesystem::path& p, std::string* e) override { return inner_.open(p, e); }
    void close() override {}
    int read(double* out, int frames) override {
        if (pos_ >= after_) throw std::runtime_error("decoder failure");
        const int n = inner_.read(out, frames);
        pos_ += n;
        return n;
    }
    bool seek(int64_t frame) override { return inner_.seek(frame); }

private:
    test::MemoryDecoder inner_;
    int64_t after_, pos_ = 0;
};

}  // namespace

TEST_CASE("Player: an exception while opening, playing or preparing the next track fails the playback, not the process",
          "[player]") {
    Settings s;
    s.output.rate = "native";
    const auto music = sine(44100, 1000, 1.0);
    auto run = [&](Rig& rig, const std::string& path, std::string* failed, std::string* start_error,
                   const std::filesystem::path& next = {}) {
        PlayerDeps d = rig.deps();
        const auto real = d.open_decoder;
        d.open_decoder = [&, real](const std::filesystem::path& p, std::string* e) -> std::unique_ptr<Decoder> {
            const std::string name = platform::path_to_utf8(p);
            if (name == "throws.test") throw std::runtime_error("open failure");
            if (name == "throws-later.test") return std::make_unique<ThrowingDecoder>(music, 36000);   // after the prefill (about 0.5 s)
            return real(p, e);
        };
        PlayCallbacks cb = rig.callbacks();
        cb.on_failed = [failed](const std::string& e) { *failed = e; };
        rig.player = std::make_unique<Player>(d);
        if (!next.empty()) rig.player->set_next(next);
        return rig.player->start(platform::path_from_utf8(path), s, OutputChoice{}, cb, start_error);
    };
    {   // opening: start() returns the error
        Rig rig;
        std::string failed, e;
        CHECK_FALSE(run(rig, "throws.test", &failed, &e));
        CHECK(e.find("internal error: open failure") != std::string::npos);
    }
    {   // mid-file: the playback fails like a lost device (finish_playback, on_failed)
        Rig rig;
        std::string failed, e;
        REQUIRE(run(rig, "throws-later.test", &failed, &e));
        rig.finish();
        CHECK(failed.find("internal error: decoder failure") != std::string::npos);
        CHECK_FALSE(rig.ended);
    }
    {   // the next track: not prepared, so not gapless; this one plays to its end
        Rig rig;
        rig.sources["a"] = music;
        std::string failed, e;
        REQUIRE(run(rig, "a", &failed, &e, platform::path_from_utf8("throws.test")));
        rig.finish();
        INFO(failed);
        CHECK(failed.empty());
        CHECK(rig.ended);
        CHECK(rig.tracks == 0);
        CHECK(rig.saw("the next track could not be opened (open failure)"));   // why, for the host's log
    }
}

TEST_CASE("Player: a disk that stalls during playback is not heard while the read ahead covers it", "[player][file_bytes]") {
    // a 16-bit WAV on disk through the real decoder, played in real time; from 1 s into the playback every disk read
    // waits until 2.5 s (a USB disk spinning up). The 64 MiB window read the whole file before: no gap. With a 64 KiB
    // window (0.37 s) and the ring (0.5 s) the stall is heard: the test can see the gap it guards against.
    const auto path = test::temp_path("daphne_player_stall.wav");
    {
        drwav_data_format f{};
        f.container = drwav_container_riff, f.format = DR_WAVE_FORMAT_PCM, f.channels = 2, f.sampleRate = 44100, f.bitsPerSample = 16;
        drwav w;
        REQUIRE(test::wav_write_open(&w, path, &f));
        std::vector<int16_t> x(44100 * 4 * 2);
        for (size_t i = 0; i < x.size() / 2; ++i) x[2 * i] = x[2 * i + 1] = static_cast<int16_t>(std::lround(16384 * std::sin(2 * kPi * 1000.0 * i / 44100)));
        REQUIRE(drwav_write_pcm_frames(&w, x.size() / 2, x.data()) == x.size() / 2);
        drwav_uninit(&w);
    }
    auto run = [&](int64_t window, size_t* gap) {
        FileBytes::set_window_for_tests(window);
        auto t0 = std::make_shared<std::atomic<std::chrono::steady_clock::rep>>(0);   // 0: not started
        FileBytes::set_read_hook_for_tests([t0](int64_t, size_t) {
            if (!t0->load()) return;
            const auto now = std::chrono::steady_clock::now().time_since_epoch();
            const auto from = std::chrono::steady_clock::duration(t0->load()) + std::chrono::milliseconds(1000);
            const auto to = from + std::chrono::milliseconds(1500);
            if (now >= from && now < to) std::this_thread::sleep_for(to - now);
        });
        Rig r;
        use_float_device(*r.dev);
        r.dev->speed = 1.0;
        t0->store(std::chrono::steady_clock::now().time_since_epoch().count());
        REQUIRE(r.start(platform::path_to_utf8(path), Settings{}));
        const auto rep = r.finish();
        FileBytes::set_read_hook_for_tests(nullptr);
        FileBytes::set_window_for_tests(0);
        CHECK(r.ended);
        const auto y = r.dev->samples_f32();
        REQUIRE(y.size() / 2 >= 44100u * 4);
        *gap = longest_quiet(y, 2, 100, 44100 * 4 - 100);
        UNSCOPED_INFO("window " << window << ": longest gap " << *gap << " frames, " << rep.underruns << " underruns");
        return rep.underruns;
    };
    size_t gap = 0;
    CHECK(run(0, &gap) == 0);
    CHECK(gap < 10);
    CHECK(run(64 * 1024, &gap) > 0);
    CHECK(gap >= 512);   // the fake device records one buffer of silence per underrun, not the time it waited
    std::filesystem::remove(path);
}

TEST_CASE("Player: an output that can resample is asked to only after the device's own rates failed", "[player][convert]") {
    // Core Audio exclusive (OutputCaps::converts): its converter is the last PCM attempt, never the first
    SECTION("a DAC without the track's family (48 kHz only) plays a 44.1 kHz track resampled to 48 kHz") {
        Rig r;
        r.caps.converts = true;
        r.dev->lock_rate = 48000;
        r.dev->can_convert = true;
        r.sources["a"] = int24_noise(44100, 2, 44100 / 5, 1);
        REQUIRE(r.start("a", Settings{}));
        const auto rep = r.finish();
        CHECK(r.ended);
        REQUIRE(r.dev->opens.size() == 2);
        CHECK_FALSE(r.dev->opens[0].convert);   // the device's own rates first
        CHECK(r.dev->opens[1].convert);
        CHECK(r.dev->opens[1].rate == 44100);
        CHECK(rep.device.rate == 44100);        // the stream's rate (the chain plays it unchanged)
        CHECK(rep.system_rate == 48000);        // what the device runs at
        CHECK_FALSE(rep.bit_perfect);
        CHECK(rep.not_bit_perfect_reason == "the system resamples to the device's 48000 Hz");
        CHECK(r.saw("trying 44100 Hz (resampled by the system)"));
        CHECK(r.dev->frames() >= 44100u / 5);   // (the last buffer padded)
    }
    SECTION("a 192 kHz track on a DAC that stops at 96 kHz is resampled down by the output") {
        Rig r;
        r.caps.converts = true;
        r.dev->lock_rate = 96000;
        r.dev->can_convert = true;
        r.sources["a"] = int24_noise(192000, 2, 192000 / 5, 2);
        REQUIRE(r.start("a", Settings{}));
        const auto rep = r.finish();
        CHECK(r.ended);
        CHECK(rep.device.rate == 192000);
        CHECK(rep.system_rate == 96000);
    }
    SECTION("upsampling that steps down within the family is not resampled") {
        Rig r;
        use_float_device(*r.dev);
        r.caps.converts = true;
        r.dev->lock_rate = 176400;
        r.dev->can_convert = true;
        r.sources["sine"] = sine(44100, 1000.0, 0.3);
        Settings s;
        s.output.rate = "auto";   // 705.6 kHz asked, 176.4 kHz locked: the chain upsamples 4x
        s.filter.taps = 256;
        REQUIRE(r.start("sine", s));
        const auto rep = r.finish();
        CHECK(r.ended);
        REQUIRE(r.dev->opens.size() == 1);
        CHECK_FALSE(r.dev->opens[0].convert);
        CHECK(rep.device.rate == 176400);
        CHECK(rep.system_rate == 0);
    }
    SECTION("a device that does not open is not tried again with the converter") {
        Rig r;
        r.caps.converts = true;
        std::vector<OutputRequest> tried;
        r.dev->accept = [&](const OutputRequest& q) {
            tried.push_back(q);
            return false;
        };
        r.sources["a"] = int24_noise(44100, 2, 4410, 3);
        CHECK_FALSE(r.start("a", Settings{}));
        REQUIRE(tried.size() == 1);
        CHECK_FALSE(tried[0].convert);
    }
    SECTION("an output that cannot resample is not asked to") {
        Rig r;
        r.dev->lock_rate = 48000;
        r.dev->can_convert = true;   // (the fake would, but the caps say no: WASAPI exclusive, ASIO)
        r.sources["a"] = int24_noise(44100, 2, 4410, 4);
        CHECK_FALSE(r.start("a", Settings{}));
        REQUIRE(r.dev->opens.size() == 1);
        CHECK_FALSE(r.dev->opens[0].convert);
    }
}

TEST_CASE("Player: a DSF file stopped while its DoP output opens is not converted to PCM", "[player][dsd]") {
    // the stop comes during the DoP open (a slow driver): the start ends there, with no PCM conversion and no
    // further open of the device (in shared mode that would be the probe of the mixer rate)
    Rig r;
    std::atomic<bool> opening{false};
    auto d = r.deps();
    const auto inner = d.open_output;
    d.open_output = [&, inner](const OutputRequest& q, std::string* e) {
        opening = true;
        sleep_s(0.3);
        return inner(q, e);
    };
    r.player = std::make_unique<Player>(d);
    const auto path = write_dsf("daphne_player_cancel.dsf", 2822400 / 8 / 2 / 5);   // 0.1 s per channel
    Settings s;
    s.output.dsd_lead_in_ms = 20;
    std::atomic<bool> ok{true};
    std::thread starter([&] {
        std::string e;
        ok = r.player->start(path, s, OutputChoice{}, r.callbacks(), &e);
    });
    while (!opening) sleep_s(0.001);
    r.player->stop();
    starter.join();
    CHECK_FALSE(ok);
    CHECK_FALSE(r.saw("converting the DSD file to PCM"));
    CHECK(r.dev->opens.size() == 1);
    r.player.reset();
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("Player: DoP never goes through a converter or a mixer", "[player][dsd][convert]") {
    const int64_t bytes = 2822400 / 8 / 2 / 5;   // 0.1 s per channel
    auto no_dop = [](const std::vector<uint8_t>& b) {
        const auto m = dop_markers(b, 2);
        size_t dop = 0;
        for (size_t i = 1; i < m.size(); ++i) dop += (m[i] == 0x05 && m[i - 1] == 0xFA) || (m[i] == 0xFA && m[i - 1] == 0x05);
        return dop < 4;
    };
    SECTION("exclusive with a converter: the DoP open is not converted, the DSF file goes to PCM at the rate locked") {
        Rig r;
        r.caps.converts = true;
        r.dev->lock_rate = 88200;
        r.dev->can_convert = true;
        const auto path = write_dsf("daphne_player_conv_dop.dsf", bytes);
        Settings s;
        s.output.dsd_lead_in_ms = 20;
        REQUIRE(r.start(platform::path_to_utf8(path), s));
        const auto rep = r.finish();
        CHECK(r.ended);
        REQUIRE(!r.dev->opens.empty());
        CHECK(r.dev->opens[0].rate == 176400);   // the DoP attempt, not converted
        CHECK_FALSE(r.dev->opens[0].convert);
        CHECK(rep.device.rate == 88200);
        CHECK(rep.system_rate == 0);
        CHECK(r.saw("converting at 88200 Hz, the rate the device locked"));
        CHECK(no_dop(r.dev->take()));
        r.player.reset();
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    SECTION("a shared mixer gets PCM, not DoP (at its own rate when that divides the DSD rate)") {
        for (int mixer : {48000, 44100}) {
            INFO("mixer " << mixer);
            Rig r;
            r.caps.shared_mix = true;
            r.dev->mixer_rate = mixer;
            const auto path = write_dsf("daphne_player_shared_dop.dsf", bytes);
            Settings s;
            s.output.dsd_lead_in_ms = 20;
            REQUIRE(r.start(platform::path_to_utf8(path), s));
            const auto rep = r.finish();
            CHECK(r.ended);
            CHECK(r.saw("DSD cannot be sent directly (shared mode mixes everything to PCM)"));
            CHECK(rep.source.dsd_pcm);
            CHECK(rep.device.rate == (mixer == 44100 ? 44100 : 176400));
            CHECK(rep.system_rate == (mixer == 44100 ? 0 : 48000));
            CHECK(no_dop(r.dev->take()));
            r.player.reset();
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
    }
    SECTION("the dsd core does not take an output that resamples") {
        Rig r;
        r.dev->mixer_rate = 48000;   // (an output reporting a system rate in exclusive mode)
        r.sources["sine"] = sine(44100, 1000.0, 0.2);
        Settings s;
        s.core = CoreMode::Dsd;
        s.modulator.osr = 64;
        CHECK_FALSE(r.start("sine", s));
    }
}
