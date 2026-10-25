// Core Audio output (coreaudio_output.h).
#include "daphne/engine/coreaudio_output.h"

#include <AudioToolbox/AudioConverter.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <utility>

#include "coreaudio_util.h"
#include "daphne/engine/devices.h"

namespace daphne {

namespace {

using Clock = std::chrono::steady_clock;

void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

std::string khz(double r) {
    const int x = static_cast<int>(r / 100.0 + 0.5);
    return std::to_string(x / 10) + (x % 10 ? "." + std::to_string(x % 10) : "") + " kHz";
}

bool all_digits(const std::string& s) { return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; }); }

// The system output this process last took in exclusive mode, and when it let go of it (resolve).
std::mutex g_took_m;
std::string g_took_uid;
Clock::time_point g_took_at{};
constexpr auto kTookFor = std::chrono::seconds(3);

void note_released_default(const std::string& uid) {
    std::lock_guard<std::mutex> l(g_took_m);
    g_took_uid = uid;
    g_took_at = Clock::now();
}

// The device kept in exclusive mode between tracks (keep_output_devices, devices.h): hog mode, rate and DAC format
// stay and the IO runs, silent; the next open of the same device takes it over. Not destroyed at exit (no HAL calls
// then: the system releases hog mode with the process).
std::mutex g_kept_m;
bool g_keep = false;
CoreAudioOutput::Impl* g_kept = nullptr;

// *taken_default: the device was resolved as "the system output".
AudioDeviceID resolve(const std::string& want, std::string* error, bool* taken_default) {
    *taken_default = false;
    if (want.empty() || want == "default") {
        *taken_default = true;
        // While a device is held in hog mode macOS moves the system output elsewhere (or to none), and moves it back a
        // moment after hog mode is released. So "the system output" is: a device this process holds; the one it took
        // as the system output and released less than kTookFor ago (a player restart closes the output and opens it
        // again at once: meanwhile the system output is missing, or another device); else the system output, waited
        // for up to 1 s when there is none. A device another process holds is why there is none.
        const pid_t me = getpid();
        auto devs = ca::output_devices();
        for (AudioDeviceID d : devs)
            if (ca::hog_owner(d) == me) return d;
        {
            std::lock_guard<std::mutex> l(g_took_m);
            if (!g_took_uid.empty() && Clock::now() - g_took_at < kTookFor)
                for (AudioDeviceID d : devs)
                    if (ca::string_prop(d, kAudioDevicePropertyDeviceUID) == g_took_uid) return d;
        }
        for (int i = 0;; ++i) {
            const AudioDeviceID d = ca::default_output();
            if (d != kAudioObjectUnknown) return d;
            for (AudioDeviceID h : devs)
                if (const pid_t p = ca::hog_owner(h); p != -1) {
                    if (error)
                        *error = ca::string_prop(h, kAudioObjectPropertyName) + " is in exclusive use by another application (pid " +
                                 std::to_string(p) + ")";
                    return kAudioObjectUnknown;
                }
            if (i >= 50) break;
            sleep_ms(20);
            devs = ca::output_devices();
        }
        if (error) *error = "no system output device";
        return kAudioObjectUnknown;
    }
    const auto devs = ca::output_devices();
    if (all_digits(want)) {
        const size_t i = static_cast<size_t>(std::stoul(want));
        if (i < devs.size()) return devs[i];
    }
    for (AudioDeviceID d : devs)
        if (ca::string_prop(d, kAudioDevicePropertyDeviceUID) == want) return d;
    for (AudioDeviceID d : devs)
        if (ca::string_prop(d, kAudioObjectPropertyName) == want) return d;
    if (error) *error = "Core Audio device not found: " + want;
    return kAudioObjectUnknown;
}

// Exclusive: the rate asked for when offered, else the same family's highest offered rate below it, else
// the highest offered rate (0: the device lists none; it keeps its own).
int choose_rate(const std::vector<int>& offered, int want) {
    if (offered.empty()) return 0;
    if (std::find(offered.begin(), offered.end(), want) != offered.end()) return want;
    const int fam = want % 44100 == 0 ? 44100 : 48000;
    int best = 0;
    for (int r : offered)
        if (r % fam == 0 && r <= want) best = std::max(best, r);
    return best ? best : offered.back();
}

// Interleaved 32-bit float, the converter's format on both sides.
AudioStreamBasicDescription float_pcm(double rate, int channels) {
    AudioStreamBasicDescription f{};
    f.mSampleRate = rate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagsNativeEndian;
    f.mFramesPerPacket = 1;
    f.mChannelsPerFrame = static_cast<UInt32>(channels);
    f.mBitsPerChannel = 32;
    f.mBytesPerFrame = f.mBytesPerPacket = 4 * static_cast<UInt32>(channels);
    return f;
}

}  // namespace

struct CoreAudioOutput::Impl {
    std::atomic<CoreAudioOutput*> owner{nullptr};   // none while the device is kept with nothing playing, or opening
    AudioDeviceID dev = kAudioObjectUnknown;
    AudioDeviceIOProcID proc = nullptr;
    bool exclusive = false, own_hog = false, io_running = false;
    bool taken_default = false;   // resolved as "the system output" (resolve)
    double original_rate = 0, rate = 0;
    bool rate_touched = false;
    std::vector<std::pair<AudioStreamID, AudioStreamBasicDescription>> physical_restore;   // formats open() changed
    std::vector<AudioStreamID> streams;
    std::vector<AudioStreamBasicDescription> virt;

    // Where one ring channel goes in the IO buffer list.
    struct Slot {
        UInt32 buffer = 0, offset = 0, stride = 0;
    };
    std::vector<Slot> slots;   // per ring channel
    bool mono = false;         // one ring channel: written to both of the stereo pair
    Slot mono_right;
    int channels = 0;
    std::vector<int32_t> tmp;  // one callback's frames from the ring (int24-in-32)
    int tmp_frames = 0;
    std::vector<float> f32;    // one callback's frames as float32, at the device rate

    // A rate the device does not run at and Daphne's chain cannot reach by a whole factor (48 kHz on a 44.1 kHz
    // device): Core Audio's converter resamples in the callback, as Windows does for WASAPI shared (AUTOCONVERTPCM).
    // Shared mode, or exclusive when asked (CoreAudioOptions::convert). The player sees the rate it asked for.
    AudioConverterRef conv = nullptr;
    std::vector<float> conv_in;   // what the converter last took, float32 at the stream rate

    std::atomic<bool> armed{false};
    std::atomic<int> in_render{0};   // stop() waits for a callback still reading the ring
    ByteRing* ring = nullptr;
    const std::atomic<bool>* eof = nullptr;
    std::atomic<uint64_t> calls{0};
    std::atomic<int64_t> last_ns{0};
    int64_t period_ns = 0;

    bool listening = false;
    std::atomic<bool> removed{false};
    std::mutex notes_m;
    std::vector<std::string> notes;

    void note(std::string s) {
        std::lock_guard<std::mutex> l(notes_m);
        notes.push_back(std::move(s));
    }

    static OSStatus io(AudioObjectID, const AudioTimeStamp*, const AudioBufferList*, const AudioTimeStamp*, AudioBufferList* out,
                       const AudioTimeStamp*, void* ctx) {
        static_cast<Impl*>(ctx)->render(out);
        return noErr;
    }

    // frames from the ring into tmp (int24-in-32)
    void pull(int frames) {
        CoreAudioOutput* o = owner.load(std::memory_order_relaxed);   // set while armed
        if (o->feeder_.fill(reinterpret_cast<uint8_t*>(tmp.data()), frames, ring, eof->load(std::memory_order_acquire), o->stats_))
            o->drained_ = true;
    }

    // The converter asks for input (from inside render): up to tmp_frames from the ring, as float32.
    static OSStatus conv_input(AudioConverterRef, UInt32* packets, AudioBufferList* io, AudioStreamPacketDescription**, void* ctx) {
        auto* m = static_cast<Impl*>(ctx);
        const int frames = std::min(static_cast<int>(*packets), m->tmp_frames);
        m->pull(frames);
        constexpr float k = 1.0f / 2147483648.0f;
        const size_t n = static_cast<size_t>(frames) * static_cast<size_t>(m->channels);
        for (size_t i = 0; i < n; ++i) m->conv_in[i] = static_cast<float>(m->tmp[i]) * k;
        io->mBuffers[0].mData = m->conv_in.data();
        io->mBuffers[0].mDataByteSize = static_cast<UInt32>(n * 4);
        io->mBuffers[0].mNumberChannels = static_cast<UInt32>(m->channels);
        *packets = static_cast<UInt32>(frames);
        return noErr;
    }

    // The HAL's IO thread: no lock, no allocation, no system call beyond the clock.
    void render(AudioBufferList* out) {
        for (UInt32 b = 0; b < out->mNumberBuffers; ++b)
            if (out->mBuffers[b].mData) std::memset(out->mBuffers[b].mData, 0, out->mBuffers[b].mDataByteSize);
        calls.fetch_add(1, std::memory_order_relaxed);
        in_render.fetch_add(1);
        CoreAudioOutput* o = owner.load(std::memory_order_acquire);
        if (!o) {   // kept with nothing playing, or still opening: silence
            in_render.fetch_sub(1, std::memory_order_release);
            return;
        }
        OutputStats& st = o->stats_;
        const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
        const int64_t prev = last_ns.exchange(now, std::memory_order_relaxed);
        if (prev) {
            const int64_t us = (now - prev) / 1000;
            int64_t m = st.max_interval_us.load(std::memory_order_relaxed);
            while (us > m && !st.max_interval_us.compare_exchange_weak(m, us, std::memory_order_relaxed)) {}
            if (period_ns > 0 && (now - prev) * 2 > period_ns * 3) st.late_callbacks.fetch_add(1, std::memory_order_relaxed);
        }
        st.callbacks.fetch_add(1, std::memory_order_relaxed);
        if (armed.load() && !slots.empty() && slots[0].buffer < out->mNumberBuffers) {
            int frames = static_cast<int>(out->mBuffers[slots[0].buffer].mDataByteSize / slots[0].stride);
            frames = std::min(frames, tmp_frames);
            if (conv) {
                AudioBufferList abl;
                abl.mNumberBuffers = 1;
                abl.mBuffers[0].mNumberChannels = static_cast<UInt32>(channels);
                abl.mBuffers[0].mDataByteSize = static_cast<UInt32>(static_cast<size_t>(frames) * static_cast<size_t>(channels) * 4);
                abl.mBuffers[0].mData = f32.data();
                UInt32 n = static_cast<UInt32>(frames);
                if (AudioConverterFillComplexBuffer(conv, &Impl::conv_input, this, &n, &abl, nullptr) != noErr) n = 0;
                frames = static_cast<int>(n);
            } else {
                pull(frames);
                // int24-in-32 → float32: every 24-bit value exactly (x / 2^31)
                constexpr float k = 1.0f / 2147483648.0f;
                const size_t n = static_cast<size_t>(frames) * static_cast<size_t>(channels);
                for (size_t i = 0; i < n; ++i) f32[i] = static_cast<float>(tmp[i]) * k;
            }
            auto put = [&](const Slot& s, int c) {
                if (s.buffer >= out->mNumberBuffers || !out->mBuffers[s.buffer].mData) return;
                auto* base = static_cast<uint8_t*>(out->mBuffers[s.buffer].mData) + s.offset;
                for (int i = 0; i < frames; ++i) std::memcpy(base + static_cast<size_t>(i) * s.stride, &f32[static_cast<size_t>(i) * channels + c], 4);
            };
            for (int c = 0; c < channels; ++c) put(slots[static_cast<size_t>(c)], c);
            if (mono) put(mono_right, 0);
        }
        in_render.fetch_sub(1, std::memory_order_release);
    }

    // HAL notifications (its own thread): flags only.
    static OSStatus on_prop(AudioObjectID, UInt32 n, const AudioObjectPropertyAddress* a, void* ctx) {
        auto* m = static_cast<Impl*>(ctx);
        for (UInt32 i = 0; i < n; ++i) {
            if (a[i].mSelector == kAudioDevicePropertyDeviceIsAlive) {
                UInt32 alive = 1;
                ca::get(m->dev, kAudioDevicePropertyDeviceIsAlive, alive);
                if (!alive) {
                    m->removed = true;
                    if (CoreAudioOutput* o = m->owner.load()) o->failed_ = true;
                }
            } else if (a[i].mSelector == kAudioDevicePropertyNominalSampleRate) {
                const double r = ca::nominal_rate(m->dev);
                if (!ca::same_rate(r, m->rate)) {
                    m->note("the device rate was changed to " + khz(r) + " by another application: reopening");
                    if (CoreAudioOutput* o = m->owner.load()) o->reset_ = true;
                }
            }
        }
        return noErr;
    }

    static constexpr AudioObjectPropertySelector kWatched[] = {kAudioDevicePropertyDeviceIsAlive, kAudioDevicePropertyNominalSampleRate};

    void listen(bool on) {
        if (on == listening || dev == kAudioObjectUnknown) return;
        for (auto s : kWatched) {
            const auto ad = ca::addr(s);
            if (on) AudioObjectAddPropertyListener(dev, &ad, &Impl::on_prop, this);
            else AudioObjectRemovePropertyListener(dev, &ad, &Impl::on_prop, this);
        }
        listening = on;
    }

    // After a change: waits until every output stream reports the device rate and the formats read the same
    // twice in a row (one setting can change another a moment later in some drivers). At most ~1 s.
    void settle(bool changed) {
        streams = ca::output_streams(dev);
        std::vector<AudioStreamBasicDescription> v;
        for (int i = 0; i < 40; ++i) {
            std::vector<AudioStreamBasicDescription> v2;
            for (auto s : streams) {
                AudioStreamBasicDescription f{};
                ca::get(s, kAudioStreamPropertyVirtualFormat, f);
                v2.push_back(f);
            }
            const bool same = v.size() == v2.size() && std::equal(v.begin(), v.end(), v2.begin(), [](const auto& x, const auto& y) {
                                  return std::memcmp(&x, &y, sizeof x) == 0;
                              });
            const bool stable = !changed || (i > 0 && same);
            v = std::move(v2);
            if (stable && std::all_of(v.begin(), v.end(), [&](const auto& f) { return ca::same_rate(f.mSampleRate, rate); })) break;
            sleep_ms(25);
        }
        virt = std::move(v);
    }

    // Exclusive: each stream carrying our channels gets the integer DAC format with the most bits at this rate (at
    // least 24, so the HAL's float → integer conversion is exact); one that already has enough is left alone (no
    // relock). True when something changed.
    bool raise_physical(const std::vector<size_t>& used) {
        bool changed = false;
        for (size_t i : used) {
            if (i >= streams.size()) continue;
            const AudioStreamID s = streams[i];
            AudioStreamBasicDescription cur{};
            if (!ca::get(s, kAudioStreamPropertyPhysicalFormat, cur)) continue;
            const int cur_bits = ca::is_integer_pcm(cur) && ca::same_rate(cur.mSampleRate, rate) ? static_cast<int>(cur.mBitsPerChannel) : 0;
            if (cur_bits >= 24) continue;
            AudioStreamBasicDescription best = cur;
            int best_bits = cur_bits;
            for (const auto& r : ca::list<AudioStreamRangedDescription>(s, kAudioStreamPropertyAvailablePhysicalFormats)) {
                if (rate < r.mSampleRateRange.mMinimum - 0.5 || rate > r.mSampleRateRange.mMaximum + 0.5) continue;
                auto f = r.mFormat;
                f.mSampleRate = rate;
                if (!ca::is_integer_pcm(f) || f.mChannelsPerFrame != cur.mChannelsPerFrame || (f.mFormatFlags & kAudioFormatFlagIsNonMixable))
                    continue;
                if (static_cast<int>(f.mBitsPerChannel) > best_bits) best = f, best_bits = static_cast<int>(f.mBitsPerChannel);
            }
            if (best_bits <= cur_bits) continue;
            if (std::none_of(physical_restore.begin(), physical_restore.end(), [&](const auto& p) { return p.first == s; }))
                physical_restore.push_back({s, cur});
            if (ca::set(s, kAudioStreamPropertyPhysicalFormat, best) != noErr) continue;
            for (int k = 0; k < 100; ++k) {
                AudioStreamBasicDescription now{};
                if (ca::get(s, kAudioStreamPropertyPhysicalFormat, now) && now.mBitsPerChannel == best.mBitsPerChannel &&
                    ca::same_rate(now.mSampleRate, rate))
                    break;
                sleep_ms(10);
            }
            note("DAC format " + ca::format_name(cur) + " -> " + ca::format_name(best));
            changed = true;
        }
        return changed;
    }

    // Sets the nominal rate while the IO runs and waits until the device reports it (up to 1.5 s).
    bool change_rate(double want) {
        if (ca::set(dev, kAudioDevicePropertyNominalSampleRate, static_cast<Float64>(want)) != noErr) return false;
        for (int i = 0; i < 150; ++i) {
            if (ca::same_rate(ca::nominal_rate(dev), want)) return true;
            sleep_ms(10);
        }
        return false;
    }

    // Waits until the IO callback has run twice more (the device runs again after a relock); up to 0.5 s.
    void wait_io() {
        const uint64_t c0 = calls.load();
        for (int i = 0; i < 50 && calls.load() < c0 + 2; ++i) sleep_ms(10);
    }

    // Puts back what open() changed while the IO still runs, then stops it and releases hog mode (header).
    void shut() {
        listen(false);
        if (io_running && exclusive) {
            for (const auto& [s, f] : physical_restore) {
                if (ca::set(s, kAudioStreamPropertyPhysicalFormat, f) != noErr) continue;
                for (int k = 0; k < 100; ++k) {
                    AudioStreamBasicDescription now{};
                    if (ca::get(s, kAudioStreamPropertyPhysicalFormat, now) && now.mBitsPerChannel == f.mBitsPerChannel) break;
                    sleep_ms(10);
                }
            }
            if (rate_touched && original_rate > 0 && !ca::same_rate(ca::nominal_rate(dev), original_rate)) {
                change_rate(original_rate);
                wait_io();
            }
        }
        if (io_running) AudioDeviceStop(dev, proc);
        if (proc) AudioDeviceDestroyIOProcID(dev, proc);
        if (conv) AudioConverterDispose(conv);
        if (own_hog && ca::hog_owner(dev) == getpid()) {
            const pid_t none = -1;
            ca::set(dev, kAudioDevicePropertyHogMode, none);
            if (taken_default) note_released_default(ca::string_prop(dev, kAudioDevicePropertyDeviceUID));
        }
        io_running = own_hog = false;
        proc = nullptr;
        conv = nullptr;
    }
};

namespace {

std::unique_ptr<CoreAudioOutput::Impl> take_kept() {
    std::lock_guard<std::mutex> l(g_kept_m);
    return std::unique_ptr<CoreAudioOutput::Impl>(std::exchange(g_kept, nullptr));
}

}  // namespace

void keep_output_devices(bool on) {
    {
        std::lock_guard<std::mutex> l(g_kept_m);
        g_keep = on;
    }
    if (!on) release_kept_output_device();
}

bool release_kept_output_device() {
    auto k = take_kept();
    if (!k) return false;
    k->shut();
    return true;
}

bool output_device_kept() {
    std::lock_guard<std::mutex> l(g_kept_m);
    return g_kept != nullptr;
}

CoreAudioOutput::CoreAudioOutput() = default;

CoreAudioOutput::~CoreAudioOutput() { close(); }

bool CoreAudioOutput::open(const CoreAudioOptions& opt, int rate, int channels, std::string* error) {
    close();
    auto fail = [&](const std::string& msg) {
        if (error) *error = msg;
        if (impl_) {   // put back, not kept
            stop();
            impl_->shut();
            impl_.reset();
        }
        return false;
    };
    if (channels <= 0 || rate <= 0) return fail("Core Audio: invalid request");
    std::string e;
    bool taken_default = false;
    const AudioDeviceID dev = resolve(opt.device, &e, &taken_default);
    // the device kept from the last track: taken over when it is the one asked for in exclusive mode, else put back
    std::unique_ptr<Impl> kept = take_kept();
    if (kept) {
        UInt32 alive = 1;
        ca::get(kept->dev, kAudioDevicePropertyDeviceIsAlive, alive);
        if (!opt.exclusive || kept->dev != dev || !alive || kept->removed) {
            kept->shut();
            kept.reset();
        }
    }
    const bool reuse = kept != nullptr;
    impl_ = reuse ? std::move(kept) : std::make_unique<Impl>();
    Impl& m = *impl_;
    if (dev == kAudioObjectUnknown) return fail(e);
    m.dev = dev;
    m.taken_default = m.taken_default || taken_default;
    m.slots.clear();
    m.mono = false;
    name_ = ca::string_prop(m.dev, kAudioObjectPropertyName);
    if (ca::output_streams(m.dev).empty()) return fail(name_ + " has no output streams");
    m.exclusive = opt.exclusive;
    rates_ = ca::offered_rates(m.dev);
    if (!reuse) m.original_rate = ca::nominal_rate(m.dev);   // put back by close() (kept: still the first open's)

    // exclusive: hog mode first (nothing else can play on the device, and nothing mixes into it)
    if (m.exclusive) {
        const pid_t me = getpid(), holder = ca::hog_owner(m.dev);
        if (holder != -1 && holder != me) return fail(name_ + " is in exclusive use by another application (pid " + std::to_string(holder) + ")");
        if (holder != me) {
            const auto ha = ca::addr(kAudioDevicePropertyHogMode);
            Boolean settable = false;
            if (AudioObjectIsPropertySettable(m.dev, &ha, &settable) != noErr || !settable) return fail(name_ + " does not offer exclusive (hog) mode");
            ca::set(m.dev, kAudioDevicePropertyHogMode, me);
            for (int i = 0; i < 100 && ca::hog_owner(m.dev) != me; ++i) sleep_ms(10);
            if (ca::hog_owner(m.dev) != me) return fail("could not take " + name_ + " in exclusive (hog) mode");
            m.own_hog = true;
        }
    }

    // the IO buffer (HAL buffer frame size), within the device's range
    const int target = m.exclusive ? std::max(choose_rate(rates_, rate), 0) : 0;
    const double io_rate = target > 0 ? target : m.original_rate;
    AudioValueRange range{0, 0};
    ca::get(m.dev, kAudioDevicePropertyBufferFrameSizeRange, range);
    if (opt.buffer_ms > 0 && io_rate > 0) {
        double want = io_rate * opt.buffer_ms / 1000.0;
        if (range.mMaximum > 0) want = std::clamp(want, range.mMinimum, range.mMaximum);
        ca::set(m.dev, kAudioDevicePropertyBufferFrameSize, static_cast<UInt32>(want + 0.5));
    }

    // the IO runs (silent) from here on: a device in hog mode is only changed while it runs (header). A kept
    // device runs already: a new rate is set on it directly (one relock, nothing put back in between)
    if (!reuse) {
        if (AudioDeviceCreateIOProcID(m.dev, &Impl::io, &m, &m.proc) != noErr || !m.proc) return fail("could not create the IO procedure for " + name_);
        if (AudioDeviceStart(m.dev, m.proc) != noErr) return fail("could not start " + name_);
        m.io_running = true;
    } else {
        m.note("the device was kept from the last track");
    }

    const double was = ca::nominal_rate(m.dev);
    bool changed = false;
    if (m.exclusive && target > 0 && !ca::same_rate(was, target)) {
        if (m.change_rate(target)) m.note("device rate " + khz(was) + " -> " + khz(target));
        else m.note("the device did not take " + khz(target) + ": it runs at " + khz(ca::nominal_rate(m.dev)));
        changed = true;
        m.wait_io();
    } else if (m.exclusive && target > 0 && target != rate) {
        m.note(khz(rate) + " is not offered by " + name_);
    }
    if (m.exclusive) m.rate_touched = !ca::same_rate(ca::nominal_rate(m.dev), m.original_rate);
    m.rate = ca::nominal_rate(m.dev);
    if (m.rate <= 0) return fail(name_ + " reports no sample rate");
    m.settle(changed);

    // the channel layout of the IO buffer list (one buffer per interleaved stream, or per channel)
    struct Ch {
        Impl::Slot slot;
        size_t stream;
    };
    std::vector<Ch> chans;
    UInt32 buffer = 0;
    for (size_t s = 0; s < m.virt.size(); ++s) {
        const auto& f = m.virt[s];
        if (!ca::is_float32_pcm(f)) return fail(name_ + " plays " + ca::format_name(f) + ": only 32-bit float streams are supported");
        if (f.mFormatFlags & kAudioFormatFlagIsNonInterleaved) {
            for (UInt32 c = 0; c < f.mChannelsPerFrame; ++c) chans.push_back({{buffer++, 0, 4}, s});
        } else {
            for (UInt32 c = 0; c < f.mChannelsPerFrame; ++c) chans.push_back({{buffer, c * 4, 4 * f.mChannelsPerFrame}, s});
            ++buffer;
        }
    }
    if (chans.empty()) return fail(name_ + " has no output channels");
    UInt32 pref[2] = {1, 2};
    {
        const auto pa = ca::addr(kAudioDevicePropertyPreferredChannelsForStereo, kAudioObjectPropertyScopeOutput);
        UInt32 n = sizeof pref;
        if (!AudioObjectHasProperty(m.dev, &pa) || AudioObjectGetPropertyData(m.dev, &pa, 0, nullptr, &n, pref) != noErr) pref[0] = 1, pref[1] = 2;
    }
    size_t l = pref[0] >= 1 && pref[0] <= chans.size() ? pref[0] - 1 : 0;
    size_t r = pref[1] >= 1 && pref[1] <= chans.size() ? pref[1] - 1 : (chans.size() > 1 ? 1 : 0);
    std::vector<size_t> used;
    // more source channels than the device has (5.1 on a stereo DAC): open stereo, the chain downmixes
    if (channels > 2 && chans.size() < static_cast<size_t>(channels)) channels = 2;
    m.channels = channels;
    if (channels <= 2) {
        m.slots.push_back(chans[l].slot), used.push_back(chans[l].stream);
        if (channels == 2) m.slots.push_back(chans[r].slot), used.push_back(chans[r].stream);
        else if (r != l) m.mono = true, m.mono_right = chans[r].slot, used.push_back(chans[r].stream);
    } else {
        if (chans.size() < static_cast<size_t>(channels))
            return fail(name_ + " has " + std::to_string(chans.size()) + " output channels, the stream " + std::to_string(channels));
        for (int c = 0; c < channels; ++c) m.slots.push_back(chans[static_cast<size_t>(c)].slot), used.push_back(chans[static_cast<size_t>(c)].stream);
    }
    std::sort(used.begin(), used.end());
    used.erase(std::unique(used.begin(), used.end()), used.end());

    if (m.exclusive && m.raise_physical(used)) {
        m.wait_io();
        m.settle(true);
    }

    UInt32 bf = 0;
    ca::get(m.dev, kAudioDevicePropertyBufferFrameSize, bf);
    buffer_frames_ = static_cast<int>(bf);
    m.tmp_frames = std::max<int>({static_cast<int>(bf), static_cast<int>(range.mMaximum), 4096});
    m.tmp_frames = std::min(m.tmp_frames, 65536);
    m.tmp.assign(static_cast<size_t>(m.tmp_frames) * static_cast<size_t>(channels), 0);
    m.f32.assign(m.tmp.size(), 0.0f);
    // a device rate the chain cannot reach by a whole factor: Core Audio's converter resamples (Impl::conv). Shared:
    // the system's rate. Exclusive only when the player asks for it (opt.convert, its last PCM attempt, after the
    // device's own rates failed: a DAC without the track's rate family, e.g. 48 kHz only); never for DoP, which must
    // reach the DAC untouched. A kept device may still have the last track's converter.
    if (m.conv) {
        AudioConverterDispose(m.conv);   // (no callback uses it: nothing owns the output while it opens)
        m.conv = nullptr;
    }
    const int dev_rate = static_cast<int>(m.rate + 0.5);
    if ((!m.exclusive || opt.convert) && !ca::same_rate(m.rate, rate) && (dev_rate < rate || dev_rate % rate != 0)) {
        const auto in = float_pcm(rate, channels), outf = float_pcm(m.rate, channels);
        if (AudioConverterNew(&in, &outf, &m.conv) != noErr || !m.conv) return fail("could not create a sample rate converter for " + name_);
        const UInt32 q = kAudioConverterQuality_Max;
        AudioConverterSetProperty(m.conv, kAudioConverterSampleRateConverterQuality, sizeof q, &q);
        m.conv_in.assign(m.tmp.size(), 0.0f);
        m.note(std::string(m.exclusive ? "exclusive mode" : "shared mode") + ": Core Audio converts " + khz(rate) + " to the device's " + khz(m.rate));
        // callbacks come at the device's rate: in frames of the stream's rate (format()), each one is this many
        buffer_frames_ = std::max(1, static_cast<int>(bf * rate / m.rate + 0.5));
    }
    system_rate_ = m.conv ? dev_rate : 0;
    m.period_ns = bf ? static_cast<int64_t>(1e9 * bf / m.rate) : 0;

    UInt32 lat = 0, safety = 0, slat = 0;
    ca::get(m.dev, kAudioDevicePropertyLatency, lat, kAudioObjectPropertyScopeOutput);
    ca::get(m.dev, kAudioDevicePropertySafetyOffset, safety, kAudioObjectPropertyScopeOutput);
    if (!m.streams.empty()) ca::get(m.streams[0], kAudioStreamPropertyLatency, slat);
    latency_ms_ = 1000.0 * (lat + safety + slat + bf) / m.rate;

    fmt_ = {};
    fmt_.rate = m.conv ? rate : dev_rate;
    fmt_.channels = channels;
    fmt_.type = SampleType::Int24In32;
    fmt_.container_bits = 32;
    fmt_.valid_bits = 24;
    matched_ = m.conv || ca::same_rate(m.rate, rate);
    m.last_ns.store(0, std::memory_order_relaxed);   // (kept: not the gap since the last track's callbacks)
    m.owner.store(this, std::memory_order_release);
    m.listen(true);
    return true;
}

bool CoreAudioOutput::start(ByteRing* ring, const std::atomic<bool>* eof, std::string* error) {
    if (!impl_ || !impl_->io_running) {
        if (error) *error = "Core Audio: the device is not open";
        return false;
    }
    impl_->ring = ring;
    impl_->eof = eof;
    drained_ = false;
    feeder_.configure(fmt_);
    impl_->armed.store(true);
    return true;
}

void CoreAudioOutput::stop() {
    if (!impl_) return;
    impl_->armed.store(false);
    while (impl_->in_render.load(std::memory_order_acquire) != 0) std::this_thread::yield();
}

void CoreAudioOutput::close() {
    if (!impl_) return;
    stop();
    Impl& m = *impl_;
    m.listen(false);
    bool keep = false;
    {
        std::lock_guard<std::mutex> l(g_kept_m);
        keep = g_keep && m.exclusive && m.io_running && m.own_hog && !m.removed;
    }
    if (keep) {
        // kept for the next track: the IO goes on, silent once no callback holds this output any more
        m.owner.store(nullptr, std::memory_order_release);
        while (m.in_render.load(std::memory_order_acquire) != 0) std::this_thread::yield();
        m.ring = nullptr;
        m.eof = nullptr;
        std::unique_ptr<Impl> other;
        {
            std::lock_guard<std::mutex> l(g_kept_m);
            other.reset(std::exchange(g_kept, impl_.release()));
        }
        if (other) other->shut();   // (one device is kept at a time)
        return;
    }
    m.shut();
    impl_.reset();
}

std::string CoreAudioOutput::failure() const {
    if (impl_ && impl_->removed) return "the output device was removed";
    return "the output device stopped";
}

std::vector<std::string> CoreAudioOutput::take_notes() {
    if (!impl_) return {};
    std::lock_guard<std::mutex> l(impl_->notes_m);
    return std::exchange(impl_->notes, {});
}

}  // namespace daphne
