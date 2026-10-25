#include <catch_amalgamated.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <algorithm>
#include <random>
#include <thread>
#include <vector>

#include "daphne/engine/file_bytes.h"
#include "test_util.h"

using daphne::FileBytes;
using daphne::FileCursor;

namespace {

using Clock = std::chrono::steady_clock;

std::vector<uint8_t> write_bytes(const std::filesystem::path& p, size_t n, uint32_t seed = 12345) {
    std::vector<uint8_t> b(n);
    uint32_t x = seed;
    for (auto& v : b) v = static_cast<uint8_t>((x = x * 1664525u + 1013904223u) >> 24);
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    return b;
}

double ms_since(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

// The window and the read hook for one test; both back to the defaults at the end. A stalled hook waits on `open`
// (or 5 s at most), so a loader still stalled when the test ends lets go at once.
struct Disk {
    std::shared_ptr<std::atomic<bool>> open = std::make_shared<std::atomic<bool>>(false);
    explicit Disk(int64_t window = 0) { FileBytes::set_window_for_tests(window); }
    ~Disk() {
        *open = true;
        FileBytes::set_read_hook_for_tests(nullptr);
        FileBytes::set_window_for_tests(0);
    }
    // every disk read takes `ms`
    void slow(int ms) {
        FileBytes::set_read_hook_for_tests([ms](int64_t, size_t) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); });
    }
    // a disk read that would read past `from` (or any, while `all`) stalls until release(); reads before it do not
    void stall_from(int64_t from) {
        FileBytes::set_read_hook_for_tests([from, gate = open](int64_t off, size_t n) {
            if (off + static_cast<int64_t>(n) <= from) return;
            const auto t0 = Clock::now();
            while (!*gate && Clock::now() - t0 < std::chrono::seconds(5)) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        });
    }
    void release() { *open = true; }
};

// reads the whole file through a cursor in pieces of `piece` bytes
std::vector<uint8_t> read_all(const std::shared_ptr<FileBytes>& f, size_t piece) {
    FileCursor c;
    c.reset(f);
    std::vector<uint8_t> all, got(piece);
    for (size_t n; (n = c.read(got.data(), got.size())) > 0;) all.insert(all.end(), got.begin(), got.begin() + static_cast<std::ptrdiff_t>(n));
    return all;
}

// "where the bytes differ" instead of printing megabytes
std::string diff(const std::vector<uint8_t>& got, const std::vector<uint8_t>& want) {
    if (got.size() != want.size()) return "size " + std::to_string(got.size()) + " != " + std::to_string(want.size());
    const auto m = std::mismatch(got.begin(), got.end(), want.begin());
    return m.first == got.end() ? "" : "differ at " + std::to_string(m.first - got.begin());
}

// random reads (offsets forward and back, sizes from 1 byte to more than the window) checked against the file
void random_reads(const std::shared_ptr<FileBytes>& f, const std::vector<uint8_t>& want, int count, size_t max_n) {
    std::mt19937 g(7);
    std::vector<uint8_t> got;
    const auto size = static_cast<int64_t>(want.size());
    for (int i = 0; i < count; ++i) {
        const int64_t off = static_cast<int64_t>(g() % static_cast<uint32_t>(size));
        const size_t n = 1 + g() % max_n;
        got.resize(n);
        const size_t r = f->read(off, got.data(), n);
        const size_t expect = static_cast<size_t>(std::min<int64_t>(static_cast<int64_t>(n), size - off));
        INFO("read " << n << " at " << off);
        REQUIRE(r == expect);
        REQUIRE(std::equal(got.begin(), got.begin() + static_cast<std::ptrdiff_t>(r), want.begin() + off));
    }
}

}  // namespace

TEST_CASE("file bytes: a file that fits in the window, read at any offset while it loads", "[file_bytes]") {
    Disk disk;
    const auto p = test::temp_path("daphne_file_bytes.bin");
    const auto want = write_bytes(p, (size_t{5} << 20) + 123);   // more than two loader chunks
    auto f = FileBytes::open(p);
    REQUIRE(f);
    CHECK(f->size() == static_cast<int64_t>(want.size()));

    std::vector<uint8_t> got(4096);
    // the end first: the window moves there
    const int64_t tail = f->size() - 100;
    REQUIRE(f->read(tail, got.data(), got.size()) == 100);
    CHECK(std::equal(got.begin(), got.begin() + 100, want.begin() + tail));
    CHECK(f->read(f->size(), got.data(), got.size()) == 0);
    CHECK(f->read(-1, got.data(), got.size()) == 0);
    CHECK(f->read(0, got.data(), 0) == 0);

    CHECK(diff(read_all(f, 4096), want) == "");
    CHECK(diff(read_all(f, 3 << 20), want) == "");   // reads larger than a loader chunk
    random_reads(f, want, 2000, 300000);

    FileCursor c;
    c.reset(f);
    REQUIRE(c.seek(-10, 2));
    CHECK(c.tell() == f->size() - 10);
    REQUIRE(c.seek(4, 1));
    CHECK(c.tell() == f->size() - 6);
    CHECK_FALSE(c.seek(7, 1));   // past the end: the position stays
    CHECK(c.tell() == f->size() - 6);
    CHECK(c.read(got.data(), got.size()) == 6);
    CHECK(c.read(got.data(), got.size()) == 0);
    CHECK_FALSE(c.seek(-1, 0));
    REQUIRE(c.seek(1000, 0));
    REQUIRE(c.read(got.data(), 1) == 1);
    CHECK(got[0] == want[1000]);
    f.reset();
    c.reset(nullptr);
    std::filesystem::remove(p);
}

TEST_CASE("file bytes: a file larger than the window slides it, across the ring's wrap", "[file_bytes]") {
    // not a power of two, so the ring's wrap falls inside the loader's reads and the decoder's
    Disk disk(int64_t{256} * 1024 + 7);
    const auto p = test::temp_path("daphne_file_bytes_window.bin");
    const auto want = write_bytes(p, (size_t{5} << 20) + 321, 99);
    auto f = FileBytes::open(p);
    REQUIRE(f);
    for (size_t piece : {size_t{1}, size_t{777}, size_t{4096}, size_t{65536}, size_t{1} << 20}) {
        INFO("pieces of " << piece);
        CHECK(diff(read_all(f, piece), want) == "");
    }
    random_reads(f, want, 3000, 600000);   // reads larger than the window too

    // a decoder's pattern: forward, a little back (within what is kept behind), forward
    std::vector<uint8_t> got(10000);
    for (int64_t off = 0; off + 30000 < f->size(); off += 25000) {
        REQUIRE(f->read(off, got.data(), got.size()) == got.size());
        REQUIRE(std::equal(got.begin(), got.end(), want.begin() + off));
        REQUIRE(f->read(off + 5000, got.data(), got.size()) == got.size());
        REQUIRE(std::equal(got.begin(), got.end(), want.begin() + off + 5000));
    }
    f.reset();
    std::filesystem::remove(p);
}

TEST_CASE("file bytes: a missing or empty file is not read ahead", "[file_bytes]") {
    CHECK_FALSE(FileBytes::open(test::temp_path("daphne_file_bytes_missing.bin")));
    const auto e = test::temp_path("daphne_file_bytes_empty.bin");
    write_bytes(e, 0);
    CHECK_FALSE(FileBytes::open(e));
    std::filesystem::remove(e);
    // a tiny window (a few bytes) still works
    Disk disk(3);
    const auto p = test::temp_path("daphne_file_bytes_tiny.bin");
    const auto want = write_bytes(p, 1000);
    auto f = FileBytes::open(p);
    REQUIRE(f);
    CHECK(diff(read_all(f, 7), want) == "");
    random_reads(f, want, 200, 50);
    f.reset();
    std::filesystem::remove(p);
}

TEST_CASE("file bytes: a read far ahead waits for one disk read there, not for everything before it", "[file_bytes]") {
    Disk disk;
    disk.slow(100);   // 32 MiB in 2 MiB reads: 1.6 s to get to the end in order
    const auto p = test::temp_path("daphne_file_bytes_far.bin");
    const auto want = write_bytes(p, size_t{32} << 20, 5);
    auto f = FileBytes::open(p);
    REQUIRE(f);
    std::vector<uint8_t> got(1000);
    REQUIRE(f->read(0, got.data(), got.size()) == got.size());
    auto t0 = Clock::now();
    const int64_t far = f->size() - 5000;
    REQUIRE(f->read(far, got.data(), got.size()) == got.size());
    const double ahead = ms_since(t0);
    CHECK(std::equal(got.begin(), got.end(), want.begin() + far));
    CHECK(ahead < 800);   // the read in flight (discarded) and the one there: ~200 ms
    // and back to the start (an M4A's index read at the end, then the audio from the start)
    t0 = Clock::now();
    REQUIRE(f->read(10, got.data(), got.size()) == got.size());
    CHECK(ms_since(t0) < 800);
    CHECK(std::equal(got.begin(), got.end(), want.begin() + 10));
    UNSCOPED_INFO("far read " << ahead << " ms");
    f.reset();
    std::filesystem::remove(p);
}

TEST_CASE("file bytes: a disk that stalls is not felt while the window has bytes", "[file_bytes]") {
    Disk disk(int64_t{4} << 20);
    const auto p = test::temp_path("daphne_file_bytes_stall.bin");
    const auto want = write_bytes(p, size_t{16} << 20, 6);
    disk.stall_from(int64_t{4} << 20);   // everything past the first window stalls
    auto f = FileBytes::open(p);
    REQUIRE(f);
    std::vector<uint8_t> got(64 * 1024);
    REQUIRE(f->read(0, got.data(), got.size()) == got.size());
    // the window filled (from the page cache: fast) and the loader stalled on the next read
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const auto t0 = Clock::now();
    for (int64_t off = 0; off + static_cast<int64_t>(got.size()) <= (int64_t{4} << 20); off += static_cast<int64_t>(got.size())) {
        REQUIRE(f->read(off, got.data(), got.size()) == got.size());
        REQUIRE(std::equal(got.begin(), got.end(), want.begin() + off));
    }
    CHECK(ms_since(t0) < 1000);   // the disk stalls for 5 s
    // past the window: waits for the disk, then goes on
    std::thread later([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        disk.release();
    });
    const auto t1 = Clock::now();
    REQUIRE(f->read(int64_t{4} << 20, got.data(), got.size()) == got.size());
    CHECK(ms_since(t1) >= 150);
    later.join();
    CHECK(std::equal(got.begin(), got.end(), want.begin() + (int64_t{4} << 20)));
    CHECK(diff(read_all(f, 100000), want) == "");
    f.reset();
    std::filesystem::remove(p);
}

TEST_CASE("file bytes: closing does not wait for a stalled disk", "[file_bytes]") {
    Disk disk;
    disk.stall_from(0);
    const auto p = test::temp_path("daphne_file_bytes_close.bin");
    write_bytes(p, 100000);
    auto f = FileBytes::open(p);
    REQUIRE(f);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));   // the loader is in the stalled read
    const auto t0 = Clock::now();
    f.reset();
    CHECK(ms_since(t0) < 1000);   // the disk stalls for 5 s
    disk.release();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    std::error_code ec;
    std::filesystem::remove(p, ec);   // Windows: the loader may still hold it (opened shared for delete)
}

TEST_CASE("file bytes: a file cut short while it is read gives what is there, without hanging", "[file_bytes]") {
    Disk disk(int64_t{256} << 10);
    const auto p = test::temp_path("daphne_file_bytes_cut.bin");
    const auto want = write_bytes(p, size_t{5} << 20, 8);
    auto f = FileBytes::open(p);
    REQUIRE(f);
    std::vector<uint8_t> got(100000);
    REQUIRE(f->read(0, got.data(), got.size()) == got.size());
    std::filesystem::resize_file(p, size_t{1} << 20);
    const auto t0 = Clock::now();
    CHECK(f->read(int64_t{3} << 20, got.data(), got.size()) == 0);   // past the new end
    // in order from the start: up to the new end, then short
    FileCursor c;
    c.reset(f);
    std::vector<uint8_t> all;
    for (size_t n; (n = c.read(got.data(), got.size())) > 0;) all.insert(all.end(), got.begin(), got.begin() + static_cast<std::ptrdiff_t>(n));
    CHECK(all.size() == (size_t{1} << 20));
    CHECK(std::equal(all.begin(), all.end(), want.begin()));
    CHECK(ms_since(t0) < 2000);
    f.reset();
    c.reset(nullptr);
    std::filesystem::remove(p);
}

TEST_CASE("file bytes: a file over 1 GiB is read ahead in the same window", "[file_bytes]") {
    Disk disk;
    const auto p = test::temp_path("daphne_file_bytes_big.bin");
    const auto head = write_bytes(p, size_t{1} << 20, 9);
    const int64_t size = (int64_t{3} << 30) + 12345;   // sparse: no disk space, offsets past 2^31 and 2^32
    std::filesystem::resize_file(p, static_cast<uintmax_t>(size));
    auto f = FileBytes::open(p);
    REQUIRE(f);
    CHECK(f->size() == size);
    std::vector<uint8_t> got(head.size());
    REQUIRE(f->read(0, got.data(), got.size()) == got.size());
    CHECK(got == head);
    for (int64_t off : {(int64_t{2} << 30) + 1, (int64_t{3} << 30) - 7, size - 1000}) {
        INFO("at " << off);
        got.assign(4000, 0xEE);
        const size_t r = f->read(off, got.data(), got.size());
        CHECK(r == static_cast<size_t>(std::min<int64_t>(4000, size - off)));
        CHECK(std::all_of(got.begin(), got.begin() + static_cast<std::ptrdiff_t>(r), [](uint8_t b) { return b == 0; }));
    }
    REQUIRE(f->read(5, got.data(), 100) == 100);   // and back
    CHECK(std::equal(got.begin(), got.begin() + 100, head.begin() + 5));
    f.reset();
    std::filesystem::remove(p);
}
