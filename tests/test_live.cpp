#include <catch_amalgamated.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <thread>
#include <vector>

#include "daphne/engine/decoder.h"
#include "daphne/engine/live_source.h"
#include "test_util.h"

using namespace daphne;

namespace {

constexpr double kPi = 3.141592653589793;

// The host's shared ring: uint32 write index | 12 bytes | float32 stereo.
struct Bus {
    explicit Bus(int cap) : cap(cap), mem(16 + static_cast<size_t>(cap) * 8, 0) {
        set_live_bus(mem.data(), cap, 48000);
        set_live_reserve(0);   // no player here (a player test may have set it)
        live_forget();         // the target learned by an earlier test
    }
    ~Bus() { set_live_bus(nullptr, 0, 0); }
    uint32_t& index() { return *reinterpret_cast<uint32_t*>(mem.data()); }
    float* ring() { return reinterpret_cast<float*>(mem.data() + 16); }
    // writes n frames of a 1 kHz sine (continuing its phase), then the index
    void write(int n) {
        for (int i = 0; i < n; ++i, ++t) {
            const float v = static_cast<float>(0.5 * std::sin(2 * kPi * 1000.0 * t / 48000.0));
            const size_t k = (index() + static_cast<uint32_t>(i)) % static_cast<uint32_t>(cap);
            ring()[2 * k] = v, ring()[2 * k + 1] = -v;
        }
        test::store_release(&index(), index() + static_cast<uint32_t>(n));   // published as a producer does
    }
    int cap;
    std::vector<uint8_t> mem;
    int64_t t = 0;
};

}  // namespace

TEST_CASE("Live decoder: silence before the producer starts, then the stream without breaks", "[live]") {
    Bus bus(96000);
    std::string e;
    auto d = open_decoder(kLivePath, &e);
    REQUIRE(d);
    CHECK(d->info().rate == 48000);
    CHECK(d->info().channels == 2);
    CHECK(d->info().frames == -1);

    std::vector<double> out(4096 * 2);
    // nothing written: silence at once (the player prefills)
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(d->read(out.data(), 4096) == 4096);
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(10));
    for (double v : out) CHECK(v == 0.0);

    // the producer runs ahead (0.5 s); then reads and writes alternate at about the same rate
    bus.write(24000);
    std::vector<double> y;
    for (int round = 0; round < 60; ++round) {
        const int n = d->read(out.data(), 1024);
        REQUIRE(n > 0);
        y.insert(y.end(), out.begin(), out.begin() + 2 * n);
        bus.write(1024);
    }
    // skip the silence before the stream started (the first read after priming may begin mid-call)
    size_t s = 0;
    while (s < y.size() && y[s] == 0.0) s += 2;
    REQUIRE(y.size() - s > 2 * 40000);
    // a 1 kHz sine at 0.5: consecutive samples differ by at most 2 pi f / fs * 0.5 (+ the rate correction)
    const double step_max = 2 * kPi * 1000.0 / 48000.0 * 0.5 * 1.01;
    double worst = 0;
    for (size_t i = s + 2; i < y.size(); i += 2) {
        worst = std::max(worst, std::abs(y[i] - y[i - 2]));
        CHECK(y[i + 1] == Catch::Approx(-y[i]).margin(1e-12));   // channels kept apart
    }
    CHECK(worst < step_max);
    const auto st = live_stats();
    CHECK(st.active);
    CHECK(st.resyncs == 0);
    CHECK(st.starves == 0);
    CHECK(std::abs(st.adjust) <= 0.005);
}

TEST_CASE("Live decoder: the write index wrapping at 2^32 is read through, not taken for a reload", "[live]") {
    Bus bus(131072);
    bus.index() = 0xFFFFFFFFu - 30000;   // wraps about 0.6 s into the stream
    std::string e;
    auto d = open_decoder(kLivePath, &e);
    REQUIRE(d);
    std::vector<double> out(1024 * 2), y;
    bus.write(24000);
    d->read(out.data(), 1024);   // syncs to the writer (silence)
    for (int round = 0; round < 80; ++round) {
        bus.write(1024);
        const int n = d->read(out.data(), 1024);
        y.insert(y.end(), out.begin(), out.begin() + 2 * n);
    }
    REQUIRE(bus.index() < 0x10000000u);   // the writer did wrap
    size_t s = 0;
    while (s < y.size() && y[s] == 0.0) s += 2;
    REQUIRE(y.size() - s > 2 * 40000);
    const double step_max = 2 * kPi * 1000.0 / 48000.0 * 0.5 * 1.01;
    double worst = 0;
    for (size_t i = s + 2; i < y.size(); i += 2) worst = std::max(worst, std::abs(y[i] - y[i - 2]));
    CHECK(worst < step_max);   // no break at the wrap
    CHECK(live_stats().resyncs == 0);
}

TEST_CASE("Live decoder: across the 2^32 wrap of a ring that does not divide it, frames the writer overwrote are not read", "[live]") {
    // index i lives in slot i % cap: at the wrap the writer goes back to slot 0, so with MIKU's 96000 frames only
    // 2^32 % 96000 = 23296 of them lie between the reader before the wrap and the writer after it
    Bus bus(96000);
    bus.index() = 0u - 30000u;
    std::string e;
    auto d = open_decoder(kLivePath, &e);
    REQUIRE(d);
    std::vector<double> out(1024 * 2), y;
    bus.write(24000);
    d->read(out.data(), 1024);   // syncs 6000 frames before the wrap
    bus.write(8000);             // the writer wraps; the reader primes 0.15 s behind it, before the wrap
    d->read(out.data(), 1024);
    y.insert(y.end(), out.begin(), out.end());
    bus.write(20000);            // a backlog of 0.57 s: within the ring, but over the slots the reader reads next
    for (int round = 0; round < 60; ++round) {
        const int n = d->read(out.data(), 1024);
        y.insert(y.end(), out.begin(), out.begin() + 2 * n);
        bus.write(1024);
    }
    // the sine is continuous wherever two neighbours are both audio (a resync puts silence in between)
    const double step_max = 2 * kPi * 1000.0 / 48000.0 * 0.5 * 1.01;
    double worst = 0;
    size_t audio = 0;
    for (size_t i = 2; i < y.size(); i += 2) {
        if (y[i] == 0.0 || y[i - 2] == 0.0) continue;
        worst = std::max(worst, std::abs(y[i] - y[i - 2]));
        ++audio;
    }
    CHECK(audio > 20000);
    CHECK(worst < step_max);
}

TEST_CASE("Live decoder: a ring passed with no capacity while it plays is not read", "[live]") {
    Bus bus(48000);
    std::string e;
    auto d = open_decoder(kLivePath, &e);
    REQUIRE(d);
    std::vector<double> out(1024 * 2);
    bus.write(20000);
    d->read(out.data(), 1024);
    bus.write(20000);
    REQUIRE(d->read(out.data(), 1024) > 0);
    set_live_bus(bus.mem.data(), -5, 48000);   // a host's mistake: taken as no ring, not as 0xFFFFFFFB frames
    size_t audio = 0;
    for (int round = 0; round < 10; ++round) {
        bus.write(1024);
        const int n = d->read(out.data(), 1024);
        for (int i = 0; i < 2 * n; ++i) audio += out[static_cast<size_t>(i)] != 0.0;
    }
    CHECK(audio == 0);
    CHECK(live_stats().fill == 0.0);   // and dc_state's "fill" does not keep the last value meanwhile
}

TEST_CASE("Live decoder: a new ring is read from its writer, not from the old ring's read position", "[live]") {
    Bus a(96000);
    std::string e;
    auto d = open_decoder(kLivePath, &e);
    REQUIRE(d);
    std::vector<double> out(1024 * 2);
    a.write(20000);
    d->read(out.data(), 1024);   // syncs to the writer (silence)
    for (int round = 0; round < 20; ++round) {
        a.write(1024);
        d->read(out.data(), 1024);
    }
    // the host shares another ring whose writer goes on from the same index: what lies behind it was never written
    Bus b(96000);
    std::fill(b.ring(), b.ring() + 2 * b.cap, std::numeric_limits<float>::quiet_NaN());
    b.index() = a.index();
    b.t = a.t;
    size_t nan = 0, got = 0;
    for (int round = 0; round < 40; ++round) {
        b.write(1024);
        const int n = d->read(out.data(), 1024);
        for (int i = 0; i < 2 * n; ++i) nan += std::isnan(out[static_cast<size_t>(i)]) ? 1 : 0;
        got += static_cast<size_t>(n);
    }
    CHECK(got > 0);
    CHECK(nan == 0);
}

TEST_CASE("Live decoder: lapped by the writer it restarts close to it; a starve returns 5 ms of silence", "[live]") {
    Bus bus(48000);
    std::string e;
    auto d = open_decoder(kLivePath, &e);
    REQUIRE(d);
    std::vector<double> out(4096 * 2);
    bus.write(20000);
    d->read(out.data(), 1024);   // syncs to the writer (silence)
    bus.write(20000);
    REQUIRE(d->read(out.data(), 1024) > 0);   // primed (more than the 0.15 s target ahead), reading
    bus.write(47000);                          // laps the reader
    d->read(out.data(), 1024);
    CHECK(live_stats().resyncs == 1);

    // drain what is there, then nothing more is written: the read that runs dry returns, after a short wait,
    // 5 ms (240 frames) of silence
    bus.write(20000);
    int guard = 0, n = 0;
    std::chrono::steady_clock::duration waited{};
    for (;;) {
        const auto t0 = std::chrono::steady_clock::now();
        n = d->read(out.data(), 4096);
        waited = std::chrono::steady_clock::now() - t0;
        if (n <= 240 || ++guard >= 100) break;
    }
    CHECK(n == 240);
    for (int i = 0; i < 2 * n; ++i) CHECK(out[static_cast<size_t>(i)] == 0.0);
    CHECK(waited >= std::chrono::milliseconds(15));
    CHECK(waited < std::chrono::milliseconds(200));
    CHECK(live_stats().starves >= 1);
}

TEST_CASE("Live decoder: a seek (the player resuming) skips the backlog beyond the target", "[live]") {
    Bus bus(96000);
    std::string e;
    auto d = open_decoder(kLivePath, &e);
    REQUIRE(d);
    std::vector<double> out(4096 * 2);
    bus.write(20000);
    d->read(out.data(), 1024);                 // syncs to the writer (silence)
    bus.write(20000);
    REQUIRE(d->read(out.data(), 1024) > 0);   // primed, reading
    bus.write(24000);                          // half a second written while the player was paused (below kOverflow)
    d->read(out.data(), 1024);
    CHECK(live_stats().fill > 0.6);            // the target it started at (0.15 s) plus the half second
    REQUIRE(d->seek(12345));
    REQUIRE(d->read(out.data(), 1024) > 0);
    CHECK(live_stats().fill == Catch::Approx(0.15).margin(0.001));   // back at the target behind the writer (no reserve set)
    CHECK(live_stats().resyncs == 0);                                 // not counted as a resync
    // without a backlog a seek changes nothing
    const double before = live_stats().fill;
    REQUIRE(d->seek(20000));
    d->read(out.data(), 256);
    CHECK(live_stats().fill <= before);
}

TEST_CASE("Live decoder: the start and a starve prime at the target; only a seek leaves the reserve too", "[live]") {
    // Unprimed, the decoder returns silence at once, so the player's ring is already full when it primes: it
    // takes a read at a time from then on, and the reserve would stay in the decoder on top of the target.
    // After a seek the player's ring is empty and its refill takes the reserve at once.
    Bus bus(96000);
    set_live_reserve(0.1);
    std::string e;
    auto d = open_decoder(kLivePath, &e);
    REQUIRE(d);
    std::vector<double> out(4096 * 2);
    d->read(out.data(), 256);   // syncs to the writer (silence)
    bus.write(24000);           // a burst at the start: 0.5 s
    REQUIRE(d->read(out.data(), 256) > 0);
    CHECK(live_stats().fill == Catch::Approx(0.15).margin(0.001));   // the target, not the target plus the reserve
    // a seek (the player resuming, its ring cleared): the target plus what the refill takes at once
    bus.write(24000);
    REQUIRE(d->seek(0));
    REQUIRE(d->read(out.data(), 256) > 0);
    CHECK(live_stats().fill == Catch::Approx(0.25).margin(0.001));
    // a starve: silence (the player's ring stays full of it) until the new target is there, then from that far behind
    int guard = 0;
    while (live_stats().starves == 0 && ++guard < 400) d->read(out.data(), 4096);
    REQUIRE(live_stats().starves == 1);
    REQUIRE(live_stats().target == Catch::Approx(0.20));
    bus.write(24000);
    REQUIRE(d->read(out.data(), 256) > 0);
    CHECK(live_stats().fill == Catch::Approx(0.20).margin(0.001));
    set_live_reserve(0);
}

TEST_CASE("Live decoder: the target grows by a step per stall and comes back down after quiet minutes", "[live]") {
    Bus bus(96000);
    std::string e;
    auto d = open_decoder(kLivePath, &e);
    REQUIRE(d);
    std::vector<double> out(4096 * 2);
    CHECK(live_stats().target == Catch::Approx(0.15));
    bus.write(20000);
    d->read(out.data(), 1024);   // syncs
    bus.write(20000);
    REQUIRE(d->read(out.data(), 1024) > 0);
    // a stall: read until it runs dry, then on for a while; one starve and one step for all of it (it waits,
    // silent, until it holds the target again)
    auto stall = [&] {
        const int64_t before = live_stats().starves;
        int guard = 0;
        while (live_stats().starves == before && ++guard < 400) d->read(out.data(), 4096);
        for (int i = 0; i < 3; ++i) {
            CHECK(d->read(out.data(), 4096) == 4096);   // silence at once, not 5 ms after a wait
            for (int k = 0; k < 2 * 4096; ++k) REQUIRE(out[static_cast<size_t>(k)] == 0.0);
        }
        CHECK(live_stats().starves == before + 1);
    };
    stall();
    CHECK(live_stats().target == Catch::Approx(0.20));
    bus.write(20000);
    REQUIRE(d->read(out.data(), 1024) > 0);   // data again: the stall is over
    stall();
    CHECK(live_stats().target == Catch::Approx(0.25));
    for (int k = 0; k < 4; ++k) {   // capped at 0.30
        bus.write(20000);
        REQUIRE(d->read(out.data(), 1024) > 0);
        stall();
    }
    CHECK(live_stats().target == Catch::Approx(0.30));
    // quiet: the producer keeps up (it writes what is read, the buffer stays put)
    auto play = [&](double seconds) {
        for (int64_t got = 0; got < static_cast<int64_t>(seconds * 48000);) {
            bus.write(4096);
            const int n = d->read(out.data(), 4096);
            REQUIRE(n > 240);   // no starve
            got += n;
        }
    };
    bus.write(24000);   // a cushion, so the reads never wait
    // (a few seconds' margin around the steps: what is counted is the audio read, interpolated)
    play(57.0);
    CHECK(live_stats().target == Catch::Approx(0.30));    // not yet
    play(5.0);
    CHECK(live_stats().target == Catch::Approx(0.275));   // about 60 s quiet: one step down
    play(25.0);
    CHECK(live_stats().target == Catch::Approx(0.275));
    play(5.0);
    CHECK(live_stats().target == Catch::Approx(0.25));    // about 30 s later: the next
    play(150.0);
    CHECK(live_stats().target == Catch::Approx(0.15));    // and no lower than 0.15
}

TEST_CASE("Live decoder: the learned target is kept for the next live source; after a starve it refills first", "[live]") {
    Bus bus(96000);
    std::string e;
    std::vector<double> out(4096 * 2);
    {
        auto d = open_decoder(kLivePath, &e);
        REQUIRE(d);
        bus.write(20000);
        d->read(out.data(), 1024);
        bus.write(20000);
        REQUIRE(d->read(out.data(), 1024) > 0);
        int guard = 0;
        while (live_stats().starves == 0 && ++guard < 400) d->read(out.data(), 4096);
        CHECK(live_stats().target == Catch::Approx(0.20));
        // not enough yet: silence, no second starve
        bus.write(4800);   // 0.1 s, below the 0.20 s target
        CHECK(d->read(out.data(), 1024) == 1024);
        CHECK(out[0] == 0.0);
        CHECK(live_stats().starves == 1);
        bus.write(9600);   // now 0.3 s: it plays on, from 0.2 s behind the writer
        REQUIRE(d->read(out.data(), 1024) > 0);
        CHECK(live_stats().fill == Catch::Approx(0.20).margin(0.001));
    }
    auto d2 = open_decoder(kLivePath, &e);   // the next YouTube Music session
    REQUIRE(d2);
    CHECK(live_stats().target == Catch::Approx(0.20));
    live_forget();
    CHECK(live_stats().target == Catch::Approx(0.15));
}

TEST_CASE("Live decoder: without the host's ring it does not open", "[live]") {
    set_live_bus(nullptr, 0, 0);
    std::string e;
    CHECK_FALSE(open_decoder(kLivePath, &e));
    CHECK_FALSE(e.empty());
}

TEST_CASE("Live decoder: a ring detached or replaced while it reads can be freed at once", "[live]") {
    // The host frees (here: poisons with NaN, then frees) each ring as soon as set_live_bus returns. The ring
    // holds 0.25 everywhere, so the decoder's output is 0.25 or silence; NaN or anything else means it read a
    // ring after it was detached.
    constexpr int cap = 48000;
    auto make = [] {
        auto m = std::make_unique<std::vector<float>>(4 + static_cast<size_t>(cap) * 2, 0.25f);
        (*m)[0] = 0.0f;   // the write index (all bits zero = 0)
        return m;
    };
    auto index = [](std::vector<float>& m) -> uint32_t& { return *reinterpret_cast<uint32_t*>(m.data()); };
    auto poison = [](std::vector<float>& m) { std::fill(m.begin() + 4, m.end(), std::numeric_limits<float>::quiet_NaN()); };

    auto cur = make();
    set_live_bus(cur->data(), cap, 48000);
    set_live_reserve(0);
    live_forget();
    std::string e;
    auto d = open_decoder(kLivePath, &e);
    REQUIRE(d);
    std::atomic<bool> stop{false};
    std::atomic<int64_t> good{0}, bad{0};
    std::thread reader([&] {
        std::vector<double> out(4096 * 2);
        while (!stop) {
            const int n = d->read(out.data(), 4096);
            for (int i = 0; i < 2 * n; ++i) {
                const double v = out[static_cast<size_t>(i)];
                if (v == 0.0) continue;
                if (std::abs(v - 0.25) < 1e-9) good++;
                else bad++;
            }
        }
    });
    for (int it = 0; it < 200; ++it) {
        // 8 ms of a steady trickle (48 frames every 10 us): the reader always finds a little more and spends its
        // time inside the ring, where a detach can land. Spun, not slept (sleep_for(1 ms) is 15.6 ms on Windows
        // without a raised timer resolution)
        using clock = std::chrono::steady_clock;
        const auto end = clock::now() + std::chrono::milliseconds(8);
        while (clock::now() < end) {
            test::store_release(&index(*cur), index(*cur) + 48);
            const auto until = clock::now() + std::chrono::microseconds(10);
            while (clock::now() < until) std::this_thread::yield();
        }
        auto old = std::move(cur);
        if (it % 2) {
            set_live_bus(nullptr, 0, 0);   // detached: silence until the next ring
            cur = make();
            poison(*old);
            old.reset();
            set_live_bus(cur->data(), cap, 48000);
        } else {
            cur = make();
            set_live_bus(cur->data(), cap, 48000);   // replaced directly
            poison(*old);
            old.reset();
        }
    }
    stop = true;
    reader.join();
    set_live_bus(nullptr, 0, 0);
    live_forget();
    CHECK(bad == 0);
    CHECK(good > 0);
}

TEST_CASE("Live input: the Unix socket source is reserved, not implemented yet", "[live]") {
    std::string e;
    CHECK_FALSE(live_connect("/tmp/daphne-live-test.sock", 48000, &e));
    CHECK_FALSE(e.empty());
    live_disconnect();   // nothing to stop
}
