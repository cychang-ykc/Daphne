// The player end to end on a fake device (fake_output.h): what it sends for each kind of playback.
// These pin down the behavior of player.cpp so it can be restructured (N25): every test here must
// give the same result before and after. With DAPHNE_PLAYER_DUMP=<dir>, the timing-independent ones
// also write what the device received to <dir>/<name>.bin for a byte-for-byte comparison of two builds.
#include <catch_amalgamated.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <random>
#include <future>
#include <thread>

#include <nlohmann/json.hpp>

#include "daphne/engine/file_bytes.h"
#include "daphne/engine/live_source.h"
#include "daphne/engine/player.h"
#include "daphne/engine/settings.h"
#include "../core/src/player/crossfader.h"
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
