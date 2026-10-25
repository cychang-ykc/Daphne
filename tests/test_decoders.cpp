#include <catch_amalgamated.hpp>

#include <cmath>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include <dr_libs/dr_wav.h>

#include "daphne/engine/decoder.h"
#include "test_util.h"

namespace {

std::filesystem::path temp_path(const std::string& name) { return test::temp_path(name); }

// writes `frames` frames of a stereo integer ramp at `bits`, returns the samples written
std::vector<int32_t> write_int_wav(const std::filesystem::path& path, int bits, int frames) {
    drwav_data_format fmt{};
    fmt.container = drwav_container_riff;
    fmt.format = DR_WAVE_FORMAT_PCM;
    fmt.channels = 2;
    fmt.sampleRate = 44100;
    fmt.bitsPerSample = static_cast<drwav_uint32>(bits);
    drwav w;
    REQUIRE(test::wav_write_open(&w, path, &fmt));
    const int bytes = bits / 8;
    const int64_t full = (int64_t(1) << (bits - 1)) - 1;
    std::vector<int32_t> s(static_cast<size_t>(frames) * 2);
    std::vector<unsigned char> raw(s.size() * bytes);
    for (size_t i = 0; i < s.size(); ++i) {
        s[i] = static_cast<int32_t>(std::llround(full * std::sin(0.001 * double(i)) * (i & 1 ? -1 : 1)));
        for (int b = 0; b < bytes; ++b) raw[i * bytes + b] = static_cast<unsigned char>(s[i] >> (8 * b));
    }
    drwav_write_pcm_frames(&w, frames, raw.data());
    drwav_uninit(&w);
    return s;
}

void check_exact(const std::filesystem::path& path, int bits, const std::vector<int32_t>& want) {
    std::string err;
    auto d = daphne::open_decoder(path, &err);
    REQUIRE(d);
    CHECK(d->info().bits == bits);
    CHECK(d->info().lossless_int);
    CHECK(d->info().frames == int64_t(want.size() / 2));
    std::vector<double> got(want.size());
    size_t at = 0;
    for (int n; (n = d->read(got.data() + at, 1000)) > 0;) at += size_t(n) * 2;
    REQUIRE(at == want.size());
    const double scale = std::ldexp(1.0, bits - 1);
    size_t bad = 0;
    for (size_t i = 0; i < want.size(); ++i)
        if (got[i] * scale != double(want[i])) ++bad;
    CHECK(bad == 0);
}

}  // namespace

TEST_CASE("WAV integer PCM decodes bit-exactly", "[decoder]") {
    for (int bits : {16, 24, 32}) {
        const auto p = temp_path("daphne_test_" + std::to_string(bits) + ".wav");
        const auto s = write_int_wav(p, bits, 10000);
        check_exact(p, bits, s);
        std::filesystem::remove(p);
    }
}

TEST_CASE("WAV seek lands on the requested frame", "[decoder]") {
    const auto p = temp_path("daphne_test_seek.wav");
    const auto s = write_int_wav(p, 24, 10000);
    auto d = daphne::open_decoder(p, nullptr);
    REQUIRE(d);
    REQUIRE(d->seek(4321));
    double f[2];
    REQUIRE(d->read(f, 1) == 1);
    CHECK(f[0] * 8388608.0 == double(s[4321 * 2]));
    CHECK(f[1] * 8388608.0 == double(s[4321 * 2 + 1]));
    d.reset();
    std::filesystem::remove(p);
}

#ifdef _WIN32
TEST_CASE("a file name with an unpaired surrogate: the extension is read, nothing throws", "[decoder]") {
    std::wstring w = L"C:\\music\\a";
    w += static_cast<wchar_t>(0xD800);
    w += L"b.FLAC";
    const std::filesystem::path p(w);
    CHECK(daphne::file_extension(p) == "flac");
    std::string e;
    std::unique_ptr<daphne::Decoder> d;
    CHECK_NOTHROW(d = daphne::open_decoder(p, &e));   // it does not exist: an error, not an exception
    CHECK_FALSE(d);
    CHECK_FALSE(e.empty());
}
#endif

TEST_CASE("unreadable file reports an error", "[decoder]") {
    const auto p = temp_path("daphne_test_junk.wav");
    FILE* h = daphne::platform::open_file(p, "wb");
    std::fwrite("not audio at all", 1, 16, h);
    std::fclose(h);
    std::string err;
    CHECK_FALSE(daphne::open_decoder(p, &err));
    CHECK_FALSE(err.empty());
    std::filesystem::remove(p);
}

TEST_CASE("a file that is not an MP3 fails to open without a leak", "[decoder]") {
    // minimp3's open from callbacks (the read ahead) does not free its buffer when it fails; the ASan job's leak
    // check catches it
    const auto p = temp_path("daphne_test_bad.mp3");
    {
        std::vector<char> b(5000);
        uint32_t x = 31;
        for (auto& v : b) v = static_cast<char>((x = x * 1664525u + 1013904223u) >> 24);
        std::ofstream(p, std::ios::binary).write(b.data(), static_cast<std::streamsize>(b.size()));
    }
    std::string e;
    CHECK_FALSE(daphne::open_decoder(p, &e));
    std::filesystem::remove(p);
}

TEST_CASE("WAV float64 decodes bit-exactly", "[decoder]") {
    const auto p = temp_path("daphne_test_f64.wav");
    drwav_data_format fmt{};
    fmt.container = drwav_container_riff;
    fmt.format = DR_WAVE_FORMAT_IEEE_FLOAT;
    fmt.channels = 2;
    fmt.sampleRate = 96000;
    fmt.bitsPerSample = 64;
    drwav w;
    REQUIRE(test::wav_write_open(&w, p, &fmt));
    std::vector<double> x(20000);
    for (size_t i = 0; i < x.size(); ++i) x[i] = 0.7 * std::sin(0.0123456789 * double(i)) + 1e-12 * double(i % 7);
    drwav_write_pcm_frames(&w, x.size() / 2, x.data());
    drwav_uninit(&w);
    auto d = daphne::open_decoder(p, nullptr);
    REQUIRE(d);
    CHECK_FALSE(d->info().lossless_int);
    std::vector<double> got(x.size());
    size_t at = 0;
    for (int n; (n = d->read(got.data() + at, 777)) > 0;) at += size_t(n) * 2;
    REQUIRE(at == x.size());
    CHECK(got == x);
    d.reset();
    std::filesystem::remove(p);
}

TEST_CASE("FFmpeg decodes what dr_wav decodes, sample for sample", "[decoder][ffmpeg]") {
    const auto p = temp_path("daphne_test_ffmpeg.wav");
    const auto want = write_int_wav(p, 16, 30000);
    auto f = daphne::make_ffmpeg_decoder();
    std::string err;
    if (!f->open(p, &err)) {
        std::filesystem::remove(p);
        SKIP("built without FFmpeg: " << err);
    }
    CHECK(f->info().rate == 44100);
    CHECK(f->info().channels == 2);
    std::vector<double> got(want.size() + 2000);
    size_t at = 0;
    for (int n; at < want.size() + 1000 && (n = f->read(got.data() + at, 1000)) > 0;) at += size_t(n) * 2;
    REQUIRE(at == want.size());
    size_t bad = 0;
    for (size_t i = 0; i < want.size(); ++i)
        if (got[i] * 32768.0 != double(want[i])) ++bad;
    CHECK(bad == 0);
    // seek: the first sample read is the requested frame's
    REQUIRE(f->seek(12345));
    double fr[2];
    REQUIRE(f->read(fr, 1) == 1);
    CHECK(fr[0] * 32768.0 == double(want[12345 * 2]));
    f.reset();
    std::filesystem::remove(p);
}
