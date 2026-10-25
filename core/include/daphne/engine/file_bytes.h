#pragma once
// A track's file read ahead by a background thread into a fixed window of memory, so the decoders read memory and a
// slow disk cannot starve the ring. A library on a USB hard disk that stopped answering for ~3 s made the device play
// gaps: the decoder read the file on the playback thread and the ring holds only 0.5 s without a filter.
//
// The window (kWindow, or the whole file when it is smaller) holds the part of the file just ahead of where the
// decoder reads: the loader keeps it full at the disk's speed, far ahead of playback, so a stall shorter than the
// window's playing time is not heard (64 MiB: about 6 minutes of 16-bit / 44.1 kHz WAV, about 1 minute of 24-bit /
// 192 kHz). Memory stays the same whatever the file's size. A read outside the window (a seek, FFmpeg probing the end
// of an M4A for its index) moves the window there: it waits for one disk read at that place, not for the loader to
// read everything in between.
//
// The decoder waits only for bytes the loader has not reached yet: the first read of a file, the first read after a
// move, or a decoder that caught up with a stalled disk. Every reader has its own window: the next track, opened
// (and read ahead) while the current one plays, does not take the current one's.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace daphne {

class FileBytes {
public:
    static constexpr int64_t kWindow = int64_t{64} << 20;

    // The file, being read ahead. nullptr when the file cannot be opened or is empty (or its window cannot be
    // allocated): the decoder then reads the file directly, as before.
    static std::shared_ptr<FileBytes> open(const std::filesystem::path& path);

    ~FileBytes();   // stops the loader (it is not waited for: a stalled disk must not stall the caller)
    FileBytes(const FileBytes&) = delete;
    FileBytes& operator=(const FileBytes&) = delete;

    int64_t size() const;
    // Up to n bytes at off, waiting for the loader when it has not got there yet: all n of them unless the file ends
    // first or a disk read failed (the decoders take a short read for the end of the file). 0 past the end. One
    // reader at a time (a decoder's).
    size_t read(int64_t off, void* dst, size_t n);

    // Tests: the window of files opened from now on (0: kWindow), and a function called on the loader's thread before
    // each disk read (offset, bytes), to make the disk slow or stall it.
    static void set_window_for_tests(int64_t bytes);
    static void set_read_hook_for_tests(std::function<void(int64_t, size_t)> hook);

    struct State;

private:
    explicit FileBytes(std::shared_ptr<State> s);
    std::shared_ptr<State> s_;
};

// A read position over FileBytes, for the decoders' read / seek / tell callbacks.
class FileCursor {
public:
    explicit operator bool() const { return f_ != nullptr; }
    void reset(std::shared_ptr<FileBytes> f) {
        f_ = std::move(f);
        pos_ = 0;
    }
    int64_t size() const { return f_ ? f_->size() : 0; }
    int64_t tell() const { return pos_; }
    size_t read(void* dst, size_t n) {
        const size_t got = f_ ? f_->read(pos_, dst, n) : 0;
        pos_ += static_cast<int64_t>(got);
        return got;
    }
    // whence: 0 from the start, 1 from the current position, 2 from the end (SEEK_SET / SEEK_CUR / SEEK_END)
    bool seek(int64_t off, int whence) {
        const int64_t base = whence == 1 ? pos_ : whence == 2 ? size() : 0;
        if (!f_ || base + off < 0 || base + off > size()) return false;
        pos_ = base + off;
        return true;
    }

private:
    std::shared_ptr<FileBytes> f_;
    int64_t pos_ = 0;
};

// The last disk read of the read ahead that took 500 ms or more, for the log: count goes up with each one.
// Playback does not wait for these unless the decoder caught up with the loader.
struct SlowRead {
    int64_t count = 0;
    int64_t ms = 0;
    std::string file;   // UTF-8 file name
};
SlowRead last_slow_read();
int64_t slow_read_count();   // last_slow_read().count, without the lock (polled by the playback loop)

}  // namespace daphne
