// Core Audio output on a real Mac (hidden: run with daphne_tests "[.coreaudio]"; CI runners have no
// dependable audio device). Plays at -60 dBFS on the system output, so it is barely audible.
#include <catch_amalgamated.hpp>

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstring>
#include <memory>
#include <random>
#include <thread>
#include <vector>

#include "coreaudio_util.h"
#include "daphne/engine/coreaudio_output.h"
#include "daphne/engine/devices.h"
#include "daphne/engine/player.h"
#include "fake_output.h"

using namespace daphne;

namespace {

constexpr double kPi = 3.141592653589793;

CoreAudioDevice find(const std::string& uid) {
    for (auto& d : list_coreaudio_devices())
        if (uid.empty() ? d.is_default : d.uid == uid) return d;
    return {};
}

int other_rate(const CoreAudioDevice& d) {
    for (int r : {96000, 48000, 88200, 44100})
        if (r != d.rate && std::find(d.rates.begin(), d.rates.end(), r) != d.rates.end()) return r;
    return 0;
}

AudioDeviceID device_of(const std::string& uid) {
    for (auto d : ca::output_devices())
        if (ca::string_prop(d, kAudioDevicePropertyDeviceUID) == uid) return d;
    return kAudioObjectUnknown;
}

// the physical (DAC) format's bits of the device's first output stream, 0 for a float or unknown format
int physical_bits(const std::string& uid) {
    const auto streams = ca::output_streams(device_of(uid));
    AudioStreamBasicDescription f{};
    if (streams.empty() || !ca::get(streams[0], kAudioStreamPropertyPhysicalFormat, f) || !ca::is_integer_pcm(f)) return 0;
    return static_cast<int>(f.mBitsPerChannel);
}

// the most integer bits any physical format offers at this rate
int offered_bits(const std::string& uid, int rate) {
    const auto streams = ca::output_streams(device_of(uid));
    int best = 0;
    if (streams.empty()) return 0;
    for (const auto& r : ca::list<AudioStreamRangedDescription>(streams[0], kAudioStreamPropertyAvailablePhysicalFormats))
        if (ca::is_integer_pcm(r.mFormat) && rate >= r.mSampleRateRange.mMinimum - 0.5 && rate <= r.mSampleRateRange.mMaximum + 0.5)
            best = std::max(best, static_cast<int>(r.mFormat.mBitsPerChannel));
    return best;
}

// 0.3 s of a quiet 1 kHz sine as int24-in-32 stereo, then the end
void fill_ring(ByteRing& ring, int rate) {
    const int n = rate * 3 / 10;
    std::vector<int32_t> buf(static_cast<size_t>(n) * 2);
    for (int i = 0; i < n; ++i) {
        const auto v = static_cast<int32_t>(std::lround(0.001 * std::sin(2 * kPi * 1000.0 * i / rate) * 8388607.0)) << 8;
        buf[2 * static_cast<size_t>(i)] = buf[2 * static_cast<size_t>(i) + 1] = v;
    }
    REQUIRE(ring.write(buf.data(), buf.size() * 4) == buf.size() * 4);
}

bool wait_for(const std::function<bool()>& f, double seconds) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (!f()) {
        if (std::chrono::steady_clock::now() > end) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

}  // namespace

TEST_CASE("Core Audio: exclusive takes hog mode and the rate, and puts both back", "[.coreaudio]") {
    const auto before = find("");
    REQUIRE_FALSE(before.uid.empty());
    REQUIRE(before.hog_pid == -1);
    const int want = other_rate(before);
    if (!want) SKIP("the system output offers one rate only");
    INFO(before.name << ": " << before.rate << " Hz -> " << want << " Hz");
    {
        CoreAudioOutput o;
        std::string e;
        REQUIRE(o.open({before.uid, true, 0}, want, 2, &e));
        for (const auto& n : o.take_notes()) UNSCOPED_INFO(n);
        CHECK(o.matches_request());
        CHECK(o.format().rate == want);
        CHECK(o.format().type == SampleType::Int24In32);
        const auto during = find(before.uid);
        CHECK(during.rate == want);
        CHECK(during.hog_pid == getpid());
        if (offered_bits(before.uid, want) >= 24) CHECK(physical_bits(before.uid) >= 24);

        ByteRing ring(static_cast<size_t>(want) * 8);
        fill_ring(ring, want);
        std::atomic<bool> eof{true};
        REQUIRE(o.start(&ring, &eof, &e));
        CHECK(wait_for([&] { return o.drained(); }, 3.0));
        CHECK(o.stats().callbacks > 0);
        CHECK_FALSE(o.failed());
        o.close();
    }
    const auto after = find(before.uid);
    CHECK(after.rate == before.rate);
    CHECK(after.hog_pid == -1);
}

TEST_CASE("Core Audio: shared keeps the device's rate and takes no hog mode", "[.coreaudio]") {
    const auto before = find("");
    REQUIRE_FALSE(before.uid.empty());
    const int want = other_rate(before);
    CoreAudioOutput o;
    std::string e;
    REQUIRE(o.open({before.uid, false, 0}, want ? want : before.rate, 2, &e));
    // the device rate when it is a whole multiple of the request (the player's chain upsamples), else the request (converted)
    const bool multiple = !want || (before.rate >= want && before.rate % want == 0);
    CHECK(o.format().rate == (multiple ? before.rate : want));
    CHECK(o.matches_request() == (!want || want == before.rate || !multiple));
    CHECK(find(before.uid).rate == before.rate);
    CHECK(find(before.uid).hog_pid == -1);
    ByteRing ring(static_cast<size_t>(o.format().rate) * 8);
    fill_ring(ring, o.format().rate);
    std::atomic<bool> eof{true};
    REQUIRE(o.start(&ring, &eof, &e));
    CHECK(wait_for([&] { return o.drained(); }, 3.0));
    o.close();
    CHECK(find(before.uid).rate == before.rate);
}

TEST_CASE("Core Audio: shared converts a rate the device rate is no whole multiple of", "[.coreaudio]") {
    const auto before = find("");
    REQUIRE_FALSE(before.uid.empty());
    const int want = before.rate % 44100 == 0 ? 48000 : 44100;   // the other family: never a whole multiple
    CoreAudioOutput o;
    std::string e;
    REQUIRE(o.open({before.uid, false, 0}, want, 2, &e));
    bool noted = false;
    for (const auto& n : o.take_notes()) noted |= n.find("converts") != std::string::npos;
    CHECK(noted);
    CHECK(o.format().rate == want);
    CHECK(o.matches_request());
    ByteRing ring(static_cast<size_t>(want) * 8);
    fill_ring(ring, want);
    std::atomic<bool> eof{true};
    REQUIRE(o.start(&ring, &eof, &e));
    CHECK(wait_for([&] { return o.drained(); }, 3.0));
    CHECK(o.stats().underruns == 0);
    o.close();
    CHECK(find(before.uid).rate == before.rate);
}

TEST_CASE("Core Audio: the player plays a track to the end, exclusive", "[.coreaudio]") {
    const auto before = find("");
    REQUIRE_FALSE(before.uid.empty());
    auto sig = std::make_shared<test::Signal>();
    sig->info.rate = 96000;
    sig->info.channels = 2;
    sig->info.frames = 96000 / 2;
    sig->info.codec = "test";
    for (int i = 0; i < 96000 / 2; ++i) {
        const double v = 0.001 * std::sin(2 * kPi * 1000.0 * i / 96000.0);
        sig->x.push_back(v), sig->x.push_back(v);
    }
    PlayerDeps deps;
    deps.open_decoder = [sig](const std::filesystem::path&, std::string*) -> std::unique_ptr<Decoder> {
        return std::make_unique<test::MemoryDecoder>(sig);
    };
    Player p(deps);
    Settings s;
    s.filter.taps = 256;
    std::string e;
    REQUIRE(p.start("sine", s, OutputChoice{OutputApi::CoreAudioExclusive, before.uid}, {}, &e));
    PlayReport rep;
    REQUIRE(p.wait(&rep, &e));
    CHECK(p.ended());
    INFO(rep.device_name << ", " << describe(rep.device));
    CHECK(rep.device.type == SampleType::Int24In32);
    p.stop();
    CHECK(find(before.uid).hog_pid == -1);
    CHECK(find(before.uid).rate == before.rate);
}

TEST_CASE("Core Audio: reopened at another rate right after closing (a player restart)", "[.coreaudio]") {
    const auto before = find("");
    REQUIRE_FALSE(before.uid.empty());
    const int want = other_rate(before);
    if (!want) SKIP("the system output offers one rate only");
    std::string e;
    {
        CoreAudioOutput a;
        REQUIRE(a.open({before.uid, true, 0}, before.rate, 2, &e));
        ByteRing ring(static_cast<size_t>(before.rate) * 8);
        std::atomic<bool> eof{false};
        REQUIRE(a.start(&ring, &eof, &e));
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        a.stop();
    }   // closed here, as Player::Impl::restart does before opening the new output
    CoreAudioOutput b;
    const bool ok = b.open({before.uid, true, 0}, want, 2, &e);
    INFO("error: " << e);
    for (const auto& n : b.take_notes()) UNSCOPED_INFO(n);
    REQUIRE(ok);
    CHECK(b.format().rate == want);
    CHECK(find(before.uid).hog_pid == getpid());
    b.close();
    CHECK(find(before.uid).rate == before.rate);
    CHECK(find(before.uid).hog_pid == -1);
}

TEST_CASE("Core Audio: a kept device is taken over by the next open, and put back on release", "[.coreaudio]") {
    const auto before = find("");
    REQUIRE_FALSE(before.uid.empty());
    REQUIRE(before.hog_pid == -1);
    const int want = other_rate(before);
    if (!want) SKIP("the system output offers one rate only");
    keep_output_devices(true);
    std::string e;
    {
        CoreAudioOutput a;
        REQUIRE(a.open({before.uid, true, 0}, want, 2, &e));
        ByteRing ring(static_cast<size_t>(want) * 8);
        fill_ring(ring, want);
        std::atomic<bool> eof{true};
        REQUIRE(a.start(&ring, &eof, &e));
        CHECK(wait_for([&] { return a.drained(); }, 3.0));
    }   // closed: kept, as at the end of a track
    CHECK(output_device_kept());
    CHECK(find(before.uid).hog_pid == getpid());   // hog mode and the rate stay
    CHECK(find(before.uid).rate == want);
    {
        // the same device at the device's first rate: taken over, the rate set on it directly
        CoreAudioOutput b;
        REQUIRE(b.open({before.uid, true, 0}, before.rate, 2, &e));
        bool kept_note = false;
        for (const auto& n : b.take_notes()) {
            UNSCOPED_INFO(n);
            kept_note = kept_note || n.find("kept") != std::string::npos;
        }
        CHECK(kept_note);
        CHECK_FALSE(output_device_kept());
        CHECK(b.format().rate == before.rate);
        CHECK(find(before.uid).rate == before.rate);
        ByteRing ring(static_cast<size_t>(before.rate) * 8);
        fill_ring(ring, before.rate);
        std::atomic<bool> eof{true};
        REQUIRE(b.start(&ring, &eof, &e));
        CHECK(wait_for([&] { return b.drained(); }, 3.0));
        CHECK(b.stats().callbacks > 0);
    }
    CHECK(output_device_kept());
    CHECK(release_kept_output_device());
    CHECK_FALSE(output_device_kept());
    keep_output_devices(false);
    CHECK(find(before.uid).rate == before.rate);
    CHECK(find(before.uid).hog_pid == -1);
}

// ---- through a loopback device (hidden: daphne_tests "[.blackhole]", needs BlackHole installed) ----
// The player plays BlackHole in hog mode and an input IO procedure of this same process records it (hog mode
// shuts out other processes, and BlackHole's input is the same device): what the device got, sample for sample.
// The recording starts once the player holds the device: taking hog mode stops the IO already running on it, so a
// capture started before would stop the player's too. The tracks begin with silence the recording may miss.

namespace {

// Records a device's input (float32, interleaved or one buffer per channel) as interleaved stereo.
struct Capture {
    AudioDeviceID dev = kAudioObjectUnknown;
    AudioDeviceIOProcID proc = nullptr;
    std::vector<float> buf;
    std::atomic<size_t> n{0};

    static OSStatus io(AudioObjectID, const AudioTimeStamp*, const AudioBufferList* in, const AudioTimeStamp*, AudioBufferList*,
                       const AudioTimeStamp*, void* ctx) {
        auto* c = static_cast<Capture*>(ctx);
        if (!in || in->mNumberBuffers == 0 || !in->mBuffers[0].mData) return noErr;
        size_t at = c->n.load(std::memory_order_relaxed);
        if (in->mNumberBuffers == 1 && in->mBuffers[0].mNumberChannels == 2) {
            const size_t k = std::min<size_t>(in->mBuffers[0].mDataByteSize / 4, c->buf.size() - at);
            std::memcpy(c->buf.data() + at, in->mBuffers[0].mData, k * 4);
            at += k;
        } else if (in->mNumberBuffers >= 2 && in->mBuffers[1].mData) {
            const auto* l = static_cast<const float*>(in->mBuffers[0].mData);
            const auto* r = static_cast<const float*>(in->mBuffers[1].mData);
            for (size_t i = 0; i < in->mBuffers[0].mDataByteSize / 4 && at + 2 <= c->buf.size(); ++i) c->buf[at++] = l[i], c->buf[at++] = r[i];
        }
        c->n.store(at, std::memory_order_release);
        return noErr;
    }
    bool start(AudioDeviceID d, size_t samples) {
        dev = d;
        buf.assign(samples, 0.0f);
        return AudioDeviceCreateIOProcID(dev, &Capture::io, this, &proc) == noErr && AudioDeviceStart(dev, proc) == noErr;
    }
    void stop() {
        if (!proc) return;
        AudioDeviceStop(dev, proc);
        AudioDeviceDestroyIOProcID(dev, proc);
        proc = nullptr;
    }
    ~Capture() { stop(); }
    // the recording as 24-bit integers (-1 where a sample is not one exactly)
    std::vector<int32_t> int24() const {
        std::vector<int32_t> v(n.load());
        for (size_t i = 0; i < v.size(); ++i) {
            const double x = static_cast<double>(buf[i]) * 8388608.0;
            v[i] = x == std::floor(x) && x >= -8388608.0 && x < 8388608.0 ? static_cast<int32_t>(x) : INT32_MIN;
        }
        return v;
    }
};

std::string blackhole_uid() {
    for (const auto& d : list_coreaudio_devices())
        if (d.name.find("BlackHole") != std::string::npos) return d.uid;
    return {};
}

// a short track through the player in exclusive mode while the capture runs
PlayReport play_captured(const std::string& uid, const std::shared_ptr<test::Signal>& sig, const Settings& s, Capture& cap,
                         double seconds, OutputApi api = OutputApi::CoreAudioExclusive) {
    PlayerDeps deps;
    deps.open_decoder = [sig](const std::filesystem::path&, std::string*) -> std::unique_ptr<Decoder> {
        return std::make_unique<test::MemoryDecoder>(sig);
    };
    Player p(deps);
    std::string e;
    REQUIRE(p.start("signal", s, OutputChoice{api, uid}, {}, &e));
    REQUIRE(cap.start(device_of(uid), static_cast<size_t>(seconds * 768000 * 2)));
    PlayReport rep;
    REQUIRE(p.wait(&rep, &e));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    cap.stop();
    p.stop();
    size_t nonzero = 0;
    float peak = 0;
    for (size_t i = 0; i < cap.n.load(); ++i) nonzero += cap.buf[i] != 0.0f, peak = std::max(peak, std::fabs(cap.buf[i]));
    INFO("recorded " << cap.n.load() / 2 << " frames, " << nonzero << " samples not 0, peak " << peak << ", device " << rep.device.rate << " Hz");
    return rep;
}

}  // namespace

TEST_CASE("Core Audio: PCM reaches a loopback device bit-perfectly, exclusive", "[.blackhole]") {
    const std::string uid = blackhole_uid();
    if (uid.empty()) SKIP("BlackHole is not installed");
    for (int rate : {44100, 96000, 192000, 384000}) {
        INFO(rate << " Hz");
        // 0.5 s of silence, then 0.5 s of full-scale 24-bit noise, a different stream on each channel
        auto sig = std::make_shared<test::Signal>();
        sig->info.rate = rate, sig->info.channels = 2, sig->info.bits = 24, sig->info.lossless_int = true, sig->info.codec = "test";
        const int64_t frames = rate / 2;
        sig->info.frames = 2 * frames;
        std::vector<int32_t> src(static_cast<size_t>(frames) * 2);
        std::mt19937 g(static_cast<uint32_t>(rate));
        for (auto& v : src) v = static_cast<int32_t>(g() >> 8) - (1 << 23);
        sig->x.assign(src.size(), 0.0);
        for (int32_t v : src) sig->x.push_back(v / 8388608.0);
        Capture cap;
        const auto rep = play_captured(uid, sig, Settings{}, cap, 3.0);
        CHECK(rep.bit_perfect);
        CHECK(rep.device.rate == rate);
        const auto got = cap.int24();
        // where the track starts in the recording: its first 32 frames
        size_t at = SIZE_MAX;
        for (size_t i = 0; i + 64 <= got.size() && at == SIZE_MAX; i += 2)
            if (std::equal(src.begin(), src.begin() + 64, got.begin() + static_cast<std::ptrdiff_t>(i))) at = i;
        REQUIRE(at != SIZE_MAX);
        REQUIRE(at + src.size() <= got.size());
        size_t wrong = 0;
        for (size_t i = 0; i < src.size(); ++i) wrong += got[at + i] != src[i];
        CHECK(wrong == 0);
    }
}

TEST_CASE("Core Audio: the converter plays a track to its last sample and counts its delay (shared)", "[.blackhole]") {
    // 44.1 kHz on BlackHole at 48 kHz: Core Audio's converter. The track ends in a tone; what the converter still holds
    // when the ring runs dry must be heard before the output is closed (on macOS 11.7 it holds nothing: PrimeInfo is
    // 0 + 0 frames and the tone came back whole before the tail handling too; the test keeps it so)
    const std::string uid = blackhole_uid();
    if (uid.empty()) SKIP("BlackHole is not installed");
    const int dev = find(uid).rate;
    if (dev % 44100 == 0) SKIP("BlackHole runs at a multiple of 44.1 kHz (" << dev << " Hz): no converter");
    auto sig = std::make_shared<test::Signal>();
    sig->info.rate = 44100, sig->info.channels = 2, sig->info.codec = "test";
    const int quiet = 44100 / 2, tone = 44100 * 3 / 10;
    for (int i = 0; i < quiet + tone; ++i) {
        const double v = i < quiet ? 0.0 : 0.5 * std::sin(2 * kPi * 1000.0 * i / 44100.0);
        sig->x.push_back(v), sig->x.push_back(v);
    }
    sig->info.frames = quiet + tone;
    Capture cap;
    const auto rep = play_captured(uid, sig, Settings{}, cap, 3.0, OutputApi::CoreAudioShared);
    REQUIRE(rep.system_rate == dev);
    // the tone from its first to its last sample above a tenth of its level, in device frames
    size_t first = SIZE_MAX, last = 0;
    for (size_t f = 0; 2 * f < cap.n.load(); ++f)
        if (std::fabs(cap.buf[2 * f]) > 0.05f) first = std::min(first, f), last = f;
    REQUIRE(first != SIZE_MAX);
    const double want = static_cast<double>(tone) * dev / 44100.0, got = static_cast<double>(last - first + 1);
    std::fprintf(stderr, "converter: tone %.0f device frames of %.0f, latency_ms %.2f\n", got, want, rep.latency_ms);
    CHECK(got >= want - static_cast<double>(dev) / 1000.0);   // within 1 ms (a cycle's zero crossing at each end)
}

TEST_CASE("Core Audio: DoP reaches a loopback device with its markers intact", "[.blackhole]") {
    const std::string uid = blackhole_uid();
    if (uid.empty()) SKIP("BlackHole is not installed");
    for (int osr : {64, 128}) {   // DSD128 at most: the DAC these tests also run on plays no more
        INFO("DSD" << osr);
        auto sig = std::make_shared<test::Signal>();
        sig->info.rate = 44100, sig->info.channels = 2, sig->info.frames = 44100, sig->info.codec = "test";
        for (int i = 0; i < 44100; ++i) {
            const double v = i < 44100 / 2 ? 0.0 : 0.001 * std::sin(2 * kPi * 1000.0 * i / 44100.0);
            sig->x.push_back(v), sig->x.push_back(v);
        }
        Settings s;
        s.core = CoreMode::Dsd;
        s.modulator.osr = osr;
        s.output.dsd = DsdMode::Dop;
        s.output.max_dsd = 128;
        Capture cap;
        const auto rep = play_captured(uid, sig, s, cap, 3.0);
        CHECK(rep.device.rate == 176400 * osr / 64);
        // every frame from the first marker to the last: both channels carry the same marker, 0x05 and 0xFA in turn
        const auto got = cap.int24();
        auto marker = [&](size_t f) { return got[2 * f] == INT32_MIN ? -1 : (got[2 * f] >> 16) & 0xff; };
        auto is_marker = [&](size_t f) { const int m = marker(f); return m == 0x05 || m == 0xfa; };
        const size_t nf = got.size() / 2;
        size_t first = 0, last = nf;
        while (first < nf && !is_marker(first)) ++first;
        while (last > first && !is_marker(last - 1)) --last;
        REQUIRE(last - first > static_cast<size_t>(rep.device.rate) / 4);
        size_t wrong = 0;
        for (size_t f = first; f < last; ++f) {
            const int m = marker(f);
            const int r = got[2 * f + 1] == INT32_MIN ? -1 : (got[2 * f + 1] >> 16) & 0xff;
            if (!is_marker(f) || r != m || (f > first && m == marker(f - 1))) ++wrong;
        }
        CHECK(wrong == 0);
    }
}
