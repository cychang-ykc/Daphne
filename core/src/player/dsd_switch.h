#pragma once
// Tube Color switching a DSD file between sent directly and remodulated without reopening the device (doc/N28, M1 6b).
//
// The two streams line up on the file's DSD bytes. The modulator emits, while it takes its input sample i, the bits of
// the content between samples i - 2 and i - 1 (its cubic interpolation, modulator.h): its byte b holds the content from
// chain frame q b - 2 on, q being the chain frames per DSD byte (705.6 kHz into DSD64: 2, DSD128: 1). A file byte E
// holds the content from chain frame q E. So a remodulation that takes over at the file's byte P drops the modulator's
// bytes before the one that starts at q P (one silent frame in front of the modulator when the chain's delay leaves
// it half a byte off), and a direct stream that takes over goes on at the file byte the modulator's next byte would
// have held.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "crossfader.h"
#include "daphne/engine/chain.h"
#include "daphne/engine/decoder.h"
#include "daphne/engine/dsd_output.h"
#include "daphne/engine/settings.h"

namespace daphne::playback {

// The remodulation of a DSD file prepared off the processing thread, ready to take over at its file byte p.
struct RemodPrep {
    std::unique_ptr<Decoder> dec;
    std::unique_ptr<DspChain> chain;   // built with Tube Color idle: it fades in once the stream has taken over
    std::unique_ptr<DsdEncoder> enc;
    History hist;                      // the input read, for the chain's later warm-ups (crossfades)
    AudioInfo src{};
    DeviceFormat devf{};               // the chain's output: the modulator's input
    int ratio = 1;                     // chain frames per input frame
    int bytes_per_in = 1;              // file DSD bytes (per channel) per input frame
    int q = 1;                         // chain frames per DSD byte
    int dummy = 0;                     // silent frames in front of the modulator (0 .. q - 1)
    int delay = 0;                     // the chain's content delay (chain frames)
    int64_t f0 = 0;                    // the input frame the pre-roll started at
    int64_t in = 0;                    // input frames read (the decoder's position)
    int64_t out = 0;                   // chain frames made (all into the modulator)
    int64_t dropped = 0;               // the modulator's bytes per channel dropped
    int64_t p = -1;                    // the file byte its next byte holds
    std::vector<double> pcm, buf;

    // The content (chain frames, from the start of the file) of the modulator's input sample 0.
    int64_t mod0() const { return static_cast<int64_t>(ratio) * f0 - delay - dummy; }
    // Runs on until the modulator's next byte holds the file's byte target (even, a whole input frame); false when
    // cancelled or at the end of the file.
    bool advance_to(int64_t target, const std::atomic<bool>* cancel);
};

struct RemodRequest {
    std::filesystem::path path;
    Settings cs;                       // the chain's settings (dsd core at the file's multiple, Tube Color idle)
    int dsd_pcm_rate = 0;              // the DSD -> PCM decoder's rate (0: 176.4 / 192 kHz)
    int dsd_rate = 0;                  // the file's
    DeviceFormat device{};
    bool dop = true;
    int64_t at = 0;                    // the file byte to take over at
};

// Tests only: holds the preparation back this long (a slow machine: the switch point then moves on). 0 normally.
extern std::atomic<int> g_remod_prep_delay_ms;

// Opens, builds and pre-rolls; null when it cannot (the switch then reopens the device). On a worker thread.
std::unique_ptr<RemodPrep> prepare_remod(const RemodRequest& r, const std::atomic<bool>* cancel, std::string* error);

}  // namespace daphne::playback
