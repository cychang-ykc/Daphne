#include "daphne/engine/file_bytes.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <system_error>
#include <thread>

#include "daphne/platform/platform.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace daphne {

namespace {

constexpr int64_t kChunk = int64_t{2} << 20;        // one disk read
constexpr int64_t kFirst = int64_t{256} << 10;      // the first read after a move: the reader waits for it
constexpr int64_t kBehind = int64_t{1} << 20;       // kept behind the reader (decoders seek back a little)
constexpr int64_t kSlowMs = 500;

std::atomic<int64_t> g_window{0};
std::mutex g_hook_m;
std::shared_ptr<const std::function<void(int64_t, size_t)>> g_hook;

std::mutex g_slow_m;
SlowRead g_slow;
std::atomic<int64_t> g_slow_count{0};

// The file for the loader. Windows: shared for delete too, so the file can be renamed or deleted while it is read.
class Reader {
public:
    explicit Reader(const std::filesystem::path& path) {
#ifdef _WIN32
        h_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                         FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
#else
        f_ = platform::open_file(path, "rb");
#endif
    }
    ~Reader() {
#ifdef _WIN32
        if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
#else
        if (f_) std::fclose(f_);
#endif
    }
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;
    explicit operator bool() const {
#ifdef _WIN32
        return h_ != INVALID_HANDLE_VALUE;
#else
        return f_ != nullptr;
#endif
    }
    // n bytes at off (fewer at the end of the file or on an error)
    size_t read_at(int64_t off, uint8_t* dst, size_t n) {
        if (off != pos_) {
#ifdef _WIN32
            LARGE_INTEGER li;
            li.QuadPart = off;
            if (!SetFilePointerEx(h_, li, nullptr, FILE_BEGIN)) return 0;
#else
            if (!platform::seek_file(f_, off)) return 0;
#endif
            pos_ = off;
        }
        size_t got = 0;
        while (got < n) {
#ifdef _WIN32
            DWORD part = 0;
            const DWORD want = static_cast<DWORD>(std::min<size_t>(n - got, size_t{1} << 30));
            if (!ReadFile(h_, dst + got, want, &part, nullptr) || part == 0) break;
#else
            const size_t part = std::fread(dst + got, 1, n - got, f_);
            if (part == 0) break;
#endif
            got += part;
        }
        pos_ += static_cast<int64_t>(got);
        return got;
    }

private:
#ifdef _WIN32
    HANDLE h_ = INVALID_HANDLE_VALUE;
#else
    FILE* f_ = nullptr;
#endif
    int64_t pos_ = 0;
};

}  // namespace

// The window: file bytes [lo, hi) are in buf, the byte at file offset o at buf[o % cap]; hi - lo <= cap. Only the
// loader moves hi; a reader moves the window (lo = hi = the new offset, gen + 1) when it reads outside it.
struct FileBytes::State {
    std::unique_ptr<uint8_t[]> buf;
    int64_t cap = 0;
    int64_t size = 0;
    std::string name;   // UTF-8 file name, for the log
    std::mutex m;
    std::condition_variable data;   // the loader -> readers: hi moved, a read failed, the loader stopped
    std::condition_variable room;   // readers -> the loader: room freed, the window moved, cancelled
    int64_t lo = 0, hi = 0;
    int64_t reader = 0;   // where the reader is: what lies more than kBehind before it may be dropped
    uint64_t gen = 0;     // + 1 per move: a disk read started before it is not used
    bool fresh = true;    // no read since the last move yet: the first one is short
    bool failed = false;  // a disk read at hi failed (or found the file shorter): readers get what is there
    bool cancel = false;
    bool done = false;    // the loader stopped
};

namespace {

void copy_out(const FileBytes::State& s, int64_t off, uint8_t* dst, int64_t n) {
    const int64_t at = off % s.cap, first = std::min(n, s.cap - at);
    std::memcpy(dst, s.buf.get() + at, static_cast<size_t>(first));
    if (first < n) std::memcpy(dst + first, s.buf.get(), static_cast<size_t>(n - first));
}

void load(const std::shared_ptr<FileBytes::State>& s, const std::shared_ptr<Reader>& f) {
    std::unique_lock<std::mutex> l(s->m);
    while (!s->cancel) {
        // drop what the reader has left behind (beyond the little kept for its seeks back); a file that fits in the
        // window stays whole: a seek back reads memory
        if (s->cap < s->size) s->lo = std::clamp(s->reader - std::min(kBehind, s->cap / 4), s->lo, s->hi);
        const int64_t room = s->cap - (s->hi - s->lo);
        if (s->hi >= s->size || s->failed || room <= 0) {
            s->room.wait(l);
            continue;
        }
        int64_t n = std::min({kChunk, room, s->size - s->hi});
        if (s->fresh) n = std::min(n, kFirst);
        s->fresh = false;
        // into the free part of the ring: no reader reads it before hi moves past it, and only this thread moves hi
        const int64_t at = s->hi, slot = at % s->cap, first = std::min(n, s->cap - slot);
        const uint64_t gen = s->gen;
        l.unlock();
        std::shared_ptr<const std::function<void(int64_t, size_t)>> hook;
        {
            std::lock_guard<std::mutex> h(g_hook_m);
            hook = g_hook;
        }
        if (hook) (*hook)(at, static_cast<size_t>(n));
        const auto t0 = std::chrono::steady_clock::now();
        int64_t got = static_cast<int64_t>(f->read_at(at, s->buf.get() + slot, static_cast<size_t>(first)));
        if (got == first && first < n) got += static_cast<int64_t>(f->read_at(at + first, s->buf.get(), static_cast<size_t>(n - first)));
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        if (ms >= kSlowMs) {
            std::lock_guard<std::mutex> g(g_slow_m);
            g_slow.count++;
            g_slow.ms = ms;
            g_slow.file = s->name;
            g_slow_count.store(g_slow.count, std::memory_order_release);
        }
        l.lock();
        if (s->gen != gen) continue;   // the window moved while the disk was read: not these bytes
        s->hi += got;
        if (got < n) s->failed = true;   // a read error, or the file shrank
        s->data.notify_all();
    }
    s->done = true;
    s->data.notify_all();
}

}  // namespace

FileBytes::FileBytes(std::shared_ptr<State> s) : s_(std::move(s)) {}

FileBytes::~FileBytes() {
    {
        std::lock_guard<std::mutex> l(s_->m);
        s_->cancel = true;
    }
    s_->room.notify_all();
}

int64_t FileBytes::size() const { return s_->size; }

size_t FileBytes::read(int64_t off, void* dst, size_t n) {
    State& s = *s_;
    if (off < 0 || off >= s.size || n == 0) return 0;
    const int64_t end = std::min(s.size, off + static_cast<int64_t>(n));
    auto* out = static_cast<uint8_t*>(dst);
    int64_t pos = off;
    std::unique_lock<std::mutex> l(s.m);
    while (pos < end) {
        // outside the window (behind it, or further ahead than the loader gets in a read): move it here
        if (pos < s.lo || pos > s.hi + kChunk) {
            s.lo = s.hi = pos;
            ++s.gen;
            s.fresh = true;
            s.failed = false;
        }
        s.reader = pos;
        s.room.notify_one();
        s.data.wait(l, [&] { return s.hi > pos || s.failed || s.done || s.lo > pos; });
        if (s.lo > pos) continue;   // (not from another reader: one at a time)
        if (s.hi <= pos) break;     // a failed read or the loader stopped: what there is
        const int64_t take = std::min(end, s.hi) - pos;
        copy_out(s, pos, out + (pos - off), take);
        pos += take;
    }
    s.reader = pos;
    s.room.notify_one();
    return static_cast<size_t>(pos - off);
}

std::shared_ptr<FileBytes> FileBytes::open(const std::filesystem::path& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size == 0) return nullptr;
    auto f = std::make_shared<Reader>(path);
    if (!*f) return nullptr;
    auto s = std::make_shared<State>();
    s->size = static_cast<int64_t>(size);
    const int64_t window = g_window.load() > 0 ? g_window.load() : kWindow;
    s->cap = std::min(s->size, window);
    s->buf.reset(new (std::nothrow) uint8_t[static_cast<size_t>(s->cap)]);
    if (!s->buf) return nullptr;
    s->name = platform::path_to_utf8(path.filename());
    std::thread([s, f] { load(s, f); }).detach();
    return std::shared_ptr<FileBytes>(new FileBytes(s));
}

void FileBytes::set_window_for_tests(int64_t bytes) { g_window = bytes; }

void FileBytes::set_read_hook_for_tests(std::function<void(int64_t, size_t)> hook) {
    std::lock_guard<std::mutex> l(g_hook_m);
    g_hook = hook ? std::make_shared<const std::function<void(int64_t, size_t)>>(std::move(hook)) : nullptr;
}

SlowRead last_slow_read() {
    std::lock_guard<std::mutex> l(g_slow_m);
    return g_slow;
}

int64_t slow_read_count() { return g_slow_count.load(std::memory_order_acquire); }

}  // namespace daphne
