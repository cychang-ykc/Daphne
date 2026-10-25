// The C API's UTF-8 path functions (N25): paths with non-ASCII characters reach the engine intact.
#include <catch_amalgamated.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <dr_libs/dr_wav.h>

#include "daphne_core.h"
#include "fake_output.h"
#include "internal.h"
#include "test_util.h"

namespace {

// a short silent stereo WAV
void write_wav(const std::filesystem::path& p) {
    drwav_data_format fmt{};
    fmt.container = drwav_container_riff;
    fmt.format = DR_WAVE_FORMAT_PCM;
    fmt.channels = 2;
    fmt.sampleRate = 44100;
    fmt.bitsPerSample = 16;
    drwav w;
    REQUIRE(test::wav_write_open(&w, p, &fmt));
    const int16_t z[2 * 441] = {};
    drwav_write_pcm_frames(&w, 441, z);
    drwav_uninit(&w);
}

// No sound in a test: an output device that does not exist (WASAPI: no such endpoint ID; elsewhere there is
// no output yet). A decoded file then fails with DC_ERR_DEVICE, a file that cannot be read with DC_ERR_FILE.
constexpr const char* kNoDevice = R"({"output": {"api": "wasapi_exclusive", "device": "daphne-test-no-such-device"}})";

}  // namespace

TEST_CASE("C API: dc_load_utf8 reads a file whose name is not ASCII", "[capi]") {
    CHECK(dc_abi_version() >= 2);
    const std::string name = "daphne_測試_🎵.wav";   // UTF-8 (the source is UTF-8: /utf-8 on MSVC)
    const auto p = std::filesystem::temp_directory_path() / std::filesystem::path(std::u8string(
                                                                reinterpret_cast<const char8_t*>(name.data()), name.size()));
    write_wav(p);
    const std::string utf8 = [&] {
        const std::u8string u = p.u8string();
        return std::string(reinterpret_cast<const char*>(u.data()), u.size());
    }();
    dc_player* pl = dc_create();
    REQUIRE(pl);
    char err[512] = {};
    // the file decodes: the failure is the device's
    CHECK(dc_load_utf8(pl, utf8.c_str(), kNoDevice, 0.0, 0, err, sizeof err) == DC_ERR_DEVICE);
    // the same name with one character changed is not found: the file's
    const std::string missing = utf8.substr(0, utf8.size() - 4) + "x.wav";
    CHECK(dc_load_utf8(pl, missing.c_str(), kNoDevice, 0.0, 0, err, sizeof err) == DC_ERR_FILE);
    CHECK(dc_load_utf8(pl, nullptr, kNoDevice, 0.0, 0, err, sizeof err) == DC_ERR_ARG);
    dc_set_next_utf8(pl, utf8.c_str());   // no player running: nothing happens
    dc_set_next_gain_utf8(pl, nullptr, 0.0);
    dc_destroy(pl);
    std::error_code ec;
    std::filesystem::remove(p, ec);
}

TEST_CASE("C API: dc_live_socket tells a NULL path from the source that is not implemented yet", "[capi]") {
    CHECK(dc_live_socket(nullptr, 48000) == DC_ERR_ARG);
    CHECK(dc_live_socket("/tmp/daphne-test.sock", 48000) == 0);
}

TEST_CASE("C API: an error message cut to the caller's buffer ends on a whole UTF-8 character", "[capi]") {
    // the settings error names the unknown field, here in Chinese: every buffer size gives a prefix of the message
    // that stops before a character, never inside one
    const char* settings = R"({"output": {"api": "x"}, "測試鍵": 1})";
    dc_player* pl = dc_create();
    REQUIRE(pl);
    char full[512] = {};
    REQUIRE(dc_load_utf8(pl, "x.wav", settings, 0.0, 0, full, sizeof full) == DC_ERR_SETTINGS);
    const std::string msg = full;
    REQUIRE(msg.find("測試鍵") != std::string::npos);
    for (int len = 1; len <= static_cast<int>(msg.size()) + 2; ++len) {
        std::vector<char> err(static_cast<size_t>(len) + 8, 'z');
        CHECK(dc_load_utf8(pl, "x.wav", settings, 0.0, 0, err.data(), len) == DC_ERR_SETTINGS);
        const std::string got(err.data());
        INFO("buffer " << len << ": " << got.size() << " bytes");
        CHECK(static_cast<int>(got.size()) <= len - 1);
        CHECK(msg.compare(0, got.size(), got) == 0);
        CHECK((got.size() == msg.size() || (static_cast<unsigned char>(msg[got.size()]) & 0xC0) != 0x80));
        CHECK(static_cast<int>(got.size()) >= std::min(len - 1, static_cast<int>(msg.size())) - 3);   // at most a cut character left out
    }
    dc_destroy(pl);
}

namespace {
// A fake output whose first open takes `open_s` (a driver that hangs a while), and memory sources by name.
struct SlowRig {
    std::shared_ptr<test::FakeDevice> dev = std::make_shared<test::FakeDevice>();
    std::atomic<int> opens{0};
    std::shared_ptr<test::Signal> music;
    daphne::PlayerDeps deps(double open_s) {
        daphne::PlayerDeps d;
        const auto inner = test::fake_opener(dev);
        d.open_output = [this, inner, open_s](const daphne::OutputRequest& r, std::string* e) {
            if (opens++ == 0) std::this_thread::sleep_for(std::chrono::duration<double>(open_s));
            return inner(r, e);
        };
        d.open_decoder = [this](const std::filesystem::path&, std::string*) -> std::unique_ptr<daphne::Decoder> {
            return std::make_unique<test::MemoryDecoder>(music);
        };
        return d;
    }
    SlowRig() {
        dev->speed = 1.0;
        music = std::make_shared<test::Signal>();
        const int64_t frames = 3 * 44100;
        music->info.rate = 44100, music->info.channels = 2, music->info.frames = frames, music->info.codec = "TEST";
        music->x.resize(static_cast<size_t>(frames) * 2);
        for (int64_t i = 0; i < frames; ++i) music->x[2 * i] = music->x[2 * i + 1] = 0.5 * std::sin(2 * 3.141592653589793 * 1000.0 * i / 44100);
    }
};
std::string state_of(dc_player* pl) {
    std::vector<char> b(1 << 16);
    const int n = dc_state(pl, b.data(), static_cast<int>(b.size()));
    if (n <= 0) return {};
    const std::string j(b.data(), static_cast<size_t>(n));
    const auto k = j.find("\"state\":\"");
    return k == std::string::npos ? std::string() : j.substr(k + 9, j.find('"', k + 9) - (k + 9));
}
}  // namespace

TEST_CASE("C API: while a load waits (a slow device, a long design), the queries answer at once", "[capi]") {
    // the load holds the operation lock for its whole length; state, meter, pause, seek and set_next do not wait for it
    SlowRig rig;
    const auto deps = rig.deps(1.5);
    dc_internal_player_deps(&deps);
    dc_player* pl = dc_create();
    REQUIRE(pl);
    std::atomic<int> code{1};
    std::thread loader([&] {
        char err[256] = {};
        code = dc_load_utf8(pl, "a.test", R"({"output": {"rate": "native"}})", 0.0, 1, err, sizeof err);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));   // the load is in the slow open now
    using clock = std::chrono::steady_clock;
    double worst = 0;
    int loading = 0, calls = 0;
    const auto until = clock::now() + std::chrono::milliseconds(800);
    while (clock::now() < until) {
        const auto t0 = clock::now();
        const std::string st = state_of(pl);
        double l = 0, r = 0;
        dc_meter(pl, &l, &r);
        dc_pause(pl, 0);
        dc_seek(pl, 0.0);
        dc_set_next_utf8(pl, nullptr);
        worst = std::max(worst, std::chrono::duration<double>(clock::now() - t0).count());
        loading += st == "loading";
        ++calls;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    INFO("slowest round of queries " << worst * 1000 << " ms over " << calls << " rounds");
    CHECK(worst < 0.1);         // microseconds in fact; the load still waits for its device here
    CHECK(loading == calls);    // "loading" while no player is published
    CHECK(code == 1);           // still loading
    loader.join();
    CHECK(code == DC_OK);
    CHECK(state_of(pl) == "playing");
    dc_stop(pl);
    CHECK(state_of(pl) == "idle");
    dc_destroy(pl);
    dc_internal_player_deps(nullptr);
}

TEST_CASE("C API: dc_stop cancels a load in progress; nothing plays", "[capi]") {
    using clock = std::chrono::steady_clock;
    const std::filesystem::path off("off");
    test::set_env("DAPHNE_FILTER_CACHE", &off);   // the long filter is designed, not read from a cache
    {   // during a long filter design (1M taps, seconds): dc_stop returns at once, dc_load says cancelled
        SlowRig rig;
        const auto deps = rig.deps(0.0);
        dc_internal_player_deps(&deps);
        dc_player* pl = dc_create();
        REQUIRE(pl);
        std::atomic<int> code{1};
        std::thread loader([&] {
            char err[256] = {};
            code = dc_load_utf8(pl, "a.test", R"({"output": {"rate": "176400"}, "filter": {"taps": 1048576}, "design": {"threads": 2}})", 0.0, 1, err, sizeof err);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        REQUIRE(state_of(pl) == "loading");
        const auto t0 = clock::now();
        dc_stop(pl);
        const double took = std::chrono::duration<double>(clock::now() - t0).count();
        loader.join();
        INFO("dc_stop took " << took * 1000 << " ms");
        CHECK(took < 2.0);   // the design stops at its next check (before: the whole design, then the start)
        CHECK(code == DC_ERR_CANCELLED);
        CHECK(rig.dev->take().empty());   // the device was never started
        CHECK(state_of(pl) == "idle");
        dc_destroy(pl);
    }
    {   // while the device takes 1.5 s to open (a driver that hangs): that cannot be interrupted, dc_stop waits for it,
        // but the device is not started after it and dc_load says cancelled
        SlowRig rig;
        const auto deps = rig.deps(1.5);
        dc_internal_player_deps(&deps);
        dc_player* pl = dc_create();
        REQUIRE(pl);
        std::atomic<int> code{1};
        std::thread loader([&] {
            char err[256] = {};
            code = dc_load_utf8(pl, "a.test", R"({"output": {"rate": "native"}})", 0.0, 1, err, sizeof err);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        dc_stop(pl);
        loader.join();
        CHECK(code == DC_ERR_CANCELLED);
        CHECK(rig.dev->take().empty());
        dc_destroy(pl);
    }
    test::set_env("DAPHNE_FILTER_CACHE", nullptr);
    dc_internal_player_deps(nullptr);
}

TEST_CASE("C API: dc_cancel cancels a load at once and leaves what plays alone", "[capi]") {
    using clock = std::chrono::steady_clock;
    const std::filesystem::path off("off");
    test::set_env("DAPHNE_FILTER_CACHE", &off);
    SlowRig rig;
    const auto deps = rig.deps(0.0);
    dc_internal_player_deps(&deps);
    dc_player* pl = dc_create();
    REQUIRE(pl);
    CHECK(dc_abi_version() >= 4);
    dc_cancel(pl);   // nothing loading: no effect
    char err[256] = {};
    REQUIRE(dc_load_utf8(pl, "a.test", R"({"output": {"rate": "native"}})", 0.0, 1, err, sizeof err) == DC_OK);
    dc_cancel(pl);   // playing, nothing loading: it plays on
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(state_of(pl) == "playing");
    std::atomic<int> code{1};
    std::thread loader([&] {
        char e2[256] = {};
        code = dc_load_utf8(pl, "a.test", R"({"output": {"rate": "176400"}, "filter": {"taps": 1048576}, "design": {"threads": 2}})", 0.0, 1, e2, sizeof e2);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    REQUIRE(state_of(pl) == "loading");
    const auto t0 = clock::now();
    dc_cancel(pl);
    const double took = std::chrono::duration<double>(clock::now() - t0).count();
    loader.join();
    const double load_ended = std::chrono::duration<double>(clock::now() - t0).count();
    INFO("dc_cancel took " << took * 1000 << " ms, the load ended " << load_ended * 1000 << " ms after it");
    CHECK(took < 0.05);   // it does not wait for the load
    CHECK(code == DC_ERR_CANCELLED);
    CHECK(load_ended < 2.0);
    dc_destroy(pl);
    test::set_env("DAPHNE_FILTER_CACHE", nullptr);
    dc_internal_player_deps(nullptr);
}

TEST_CASE("C API: loads and stops and queries from several threads at once", "[capi]") {
    // no crash, no deadlock, no race (the tsan job): every load is played or cancelled
    SlowRig rig;
    rig.dev->speed = 4.0;
    const auto deps = rig.deps(0.0);
    dc_internal_player_deps(&deps);
    dc_player* pl = dc_create();
    REQUIRE(pl);
    std::atomic<bool> stop{false};
    std::atomic<int> ok{0}, cancelled{0}, other{0};
    std::thread loader([&] {
        for (int i = 0; i < 20 && !stop; ++i) {
            char err[256] = {};
            const int c = dc_load_utf8(pl, "a.test", R"({"output": {"rate": "native"}})", 0.0, 1, err, sizeof err);
            (c == DC_OK ? ok : c == DC_ERR_CANCELLED ? cancelled : other)++;
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
        stop = true;
    });
    std::thread stopper([&] {
        while (!stop) {
            dc_stop(pl);
            std::this_thread::sleep_for(std::chrono::milliseconds(7));
        }
    });
    std::thread querier([&] {
        while (!stop) {
            state_of(pl);
            double l = 0, r = 0;
            dc_meter(pl, &l, &r);
            dc_pause(pl, 0);
            dc_set_next_utf8(pl, nullptr);
        }
    });
    loader.join();
    stopper.join();
    querier.join();
    INFO("played " << ok << ", cancelled " << cancelled << ", other " << other);
    CHECK(ok + cancelled == 20);
    CHECK(other == 0);
    dc_destroy(pl);
    dc_internal_player_deps(nullptr);
}

TEST_CASE("C API: dc_filter_cache_dir_utf8 returns the folder in UTF-8", "[capi]") {
    const std::string name = "daphne_快取_é";
    const auto dir = std::filesystem::temp_directory_path() /
                     std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(name.data()), name.size()));
    test::set_env("DAPHNE_FILTER_CACHE", &dir);
    char out[1024] = {};
    const int n = dc_filter_cache_dir_utf8(out, sizeof out);
    const std::u8string want = dir.u8string();
    CHECK(n == static_cast<int>(want.size()));
    CHECK(std::string(out) == std::string(reinterpret_cast<const char*>(want.data()), want.size()));
    char small[4] = {};
    CHECK(dc_filter_cache_dir_utf8(small, sizeof small) == DC_ERR_BUFFER);
    const std::filesystem::path off("off");
    test::set_env("DAPHNE_FILTER_CACHE", &off);
    CHECK(dc_filter_cache_dir_utf8(out, sizeof out) == 0);   // the cache is disabled
    test::set_env("DAPHNE_FILTER_CACHE", nullptr);   // the tests after this one run as before
}
