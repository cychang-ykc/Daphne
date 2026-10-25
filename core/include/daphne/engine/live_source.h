#pragma once
// Live input: a float32 stereo ring in memory the host shares with an audio producer it does not
// control (a browser page's audio tap), read as a decoder opened with the path kLivePath.
//
// Ring layout (set_live_bus): uint32 write index in frames | 12 bytes reserved | float32 stereo frames,
// cap_frames of them. The producer writes the frames, then the index. The index wraps at 2^32 (about 27 h at
// 44.1 kHz) and the frame at index i is at i % cap_frames on both sides; the reader compares positions modulo
// 2^32, so the wrap is not taken for a reload. A cap_frames that is not a power of two still works, but for the
// moment of the wrap the ring holds only 2^32 % cap_frames frames (for 96000: 23296, about 0.49 s at 48 kHz).
//
// The producer runs on its own clock and delivers in bursts, so the decoder keeps a jitter buffer
// in the ring (0.15 s, raised by 50 ms per stall up to 0.30 s and lowered again after quiet playback;
// what it learns is kept for the process, N2) and holds it there by changing its read rate by at most
// 0.5 % (smoothed), with 4-point Hermite interpolation. Nothing is dropped or repeated while it runs,
// so there are no clicks. It restarts close to the writer when it was lapped, the page reloaded, or
// more than 1.2 s piled up. Until it holds the target it returns silence at once (the player prefills
// its ring) and then starts that far behind the writer, skipping a burst; once running, it waits up to
// 20 ms for the producer and then returns 5 ms of silence (a starve) and is silent again until it
// holds the (raised) target, so the player never blocks for long.
//
// Output: info().rate = the producer's rate when opened, 2 channels, frames -1 (no end). seek() moves
// the position counter and sends the reader back to the writer (the player seeks when it resumes from
// a pause: what the producer wrote meanwhile is skipped, not played late).

#include <cstdint>
#include <memory>
#include <string>

#include "daphne/engine/decoder.h"

namespace daphne {

inline constexpr char kLivePath[] = "live:";
inline bool is_live_path(const std::filesystem::path& p) { return p == std::filesystem::path(kLivePath); }

// The host's shared ring (base = nullptr detaches it) and the producer's sample rate. Returns once no read
// is left in the previous ring: the host may free that memory then, even while a live track plays (the
// decoder reads silence until a ring is shared again; after 20 ms that counts as a starve, which raises the
// learned target). Call it from one thread only. A new ring is read from the old ring's read position (N2):
// start its write index at 0. The producer writes the frames, then publishes the index with a release store.
void set_live_bus(void* base, int cap_frames, int rate);
// Reserved, not implemented yet (always false, N26): the producer's frames arriving on a Unix socket instead
// (macOS MIKU's live-relay.js, used only when its settings say "ytThroughCore": true; by default the YouTube
// Music page plays through the system output and the core lets go of the device). Meant to work as: a thread
// of the engine connects (again while the socket is missing or after it closed), writes the interleaved
// float32 stereo frames (no header, cut anywhere) into a ring of its own and shares that as above.
bool live_connect(const std::string& socket_path, int rate, std::string* error = nullptr);
// Stops that thread and detaches its ring; nothing when not connected.
void live_disconnect();
void set_live_rate(int rate);
// What the player takes from the live decoder at once when it refills its ring after a seek, in seconds: a
// seek goes back to that much more than the target behind the writer, so the decoder holds the target once
// the player's ring is full again (the player sets it for each live source it opens). Priming (the start,
// after a starve) leaves only the target: the player's ring is already full of the silence returned meanwhile.
void set_live_reserve(double seconds);

struct LiveStats {
    double fill = 0;        // seconds buffered in the ring (written, not yet read)
    double read = 0;        // seconds the live decoder has read (its position)
    int64_t resyncs = 0;    // restarts close to the writer
    int64_t starves = 0;    // silence returned while running (the producer fell behind)
    double adjust = 0;      // the read rate correction (relative)
    double target = 0;      // the buffer it aims for now (seconds; 0.15, more after starves, kept between sources, N2)
    bool active = false;    // a live decoder is open
};
LiveStats live_stats();
// The live decoder keeps the jitter buffer target it has learned for the life of the process (a later live
// source starts from it); this goes back to the minimum (tests).
void live_forget();

std::unique_ptr<Decoder> make_live_decoder();

}  // namespace daphne
