#pragma once
// What the parts of the player share (N4, N25): the processing chunk, the decisions that only depend on
// the settings, and the interleaved frame FIFO of the crossfade and the gapless join.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "daphne/engine/player.h"
#include "daphne/engine/settings.h"
#include "daphne/platform/platform.h"

namespace daphne::playback {

// For a task started with std::async: MSVC runs those on a thread pool and reuses its threads, so the
// priority and the floating-point control a task sets would stay with the thread for the next task (a
// filter design, say). Restores both when the task ends (the pool's threads run at normal priority).
class PoolThreadScope {
public:
    PoolThreadScope() : fp_(platform::fp_control()) {}
    ~PoolThreadScope() {
        platform::set_fp_control(fp_);
        platform::set_thread_priority(platform::Priority::Normal);
    }
    PoolThreadScope(const PoolThreadScope&) = delete;
    PoolThreadScope& operator=(const PoolThreadScope&) = delete;

private:
    uint64_t fp_;
};

constexpr int kChunk = 4096;           // source frames the processing thread decodes at a time
constexpr double kFadeSeconds = 0.05;  // crossfade to a rebuilt chain
constexpr double kPi = 3.141592653589793;

// The device rate asked for: pcm core, the output rate for the source; dsd core, the DoP rate
// (DSD clock / 16).
int requested_rate(const Settings& s, int fs_in);

// Whether a chain for a source at `rate` plays on the output now running (chain output rate
// chain_rate) without reopening the device: pcm, a device rate the settings give the new source
// (stepped down by powers of two as the output session does); dsd, the same modulator input rate (the
// DSD clock does not depend on the source).
bool same_output(const Settings& cs, int rate, int chain_rate);

// Modulator settings that need a new modulator (a device restart).
std::string modulator_key(const Settings& s);

// Settings as the chain sees them. osr > 0: the DSD multiple the device runs (the dsd core may have
// stepped down; the chain's limiter follows the modulator's stable range). pcm_rate > 0: the dsd core plays PCM
// at that rate for this track (the device takes no DoP rate): the chain is the pcm core's, upsampling to it.
Settings chain_settings(const Settings& s, int osr = 0, int pcm_rate = 0);

// What the player learned about devices in this process (Player instances come and go per track): the DoP rates a
// device did not take, and which notes the host was shown once. Keyed by the output API and the device's name (as
// the probe gives it; else the choice as written). Thread-safe.
namespace output_memo {
std::string key(const OutputChoice& out, const std::string& probed_name);
// A DoP rate the device did not take: it and the higher ones of its family are left out from now on. locked: the rate
// the device ran at instead (0: it did not open).
void dop_failed(const std::string& key, int rate, int locked = 0);
// Whether DoP at `rate` is known not to work (it, or a lower rate of its family, failed).
bool dop_known_bad(const std::string& key, int rate);
// The rate of rate's family the device ran at instead of a DoP rate (0: not known).
int locked_instead(const std::string& key, int rate);
// True the first time for (key, what): a note shown once per device and session.
bool first(const std::string& key, const std::string& what);
void clear();
}  // namespace output_memo

// "4096 taps, minimum phase (alpha 1.00), apodize" for messages.
std::string describe_filter(const Settings& x);
// "a -> b -> c", "none" when empty.
std::string join(const std::vector<std::string>& v);

// Interleaved frame FIFO for the crossfade.
struct Fifo {
    std::vector<double> d;
    size_t off = 0;   // values
    int ch = 1;
    int frames() const { return static_cast<int>((d.size() - off) / ch); }
    void push(const double* x, int n) { d.insert(d.end(), x, x + static_cast<size_t>(n) * ch); }
    void push_zeros(int n) { d.insert(d.end(), static_cast<size_t>(n) * ch, 0.0); }
    const double* data() const { return d.data() + off; }
    void pop(int n) {
        off += static_cast<size_t>(n) * ch;
        if (off >= d.size()) d.clear(), off = 0;
        else if (off > (1u << 20)) d.erase(d.begin(), d.begin() + static_cast<std::ptrdiff_t>(off)), off = 0;
    }
    void clear() { d.clear(), off = 0; }
};

}  // namespace daphne::playback
