#include "daphne/engine/wasapi_output.h"

// clang-format off
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <mmreg.h>
#include <wrl/client.h>
// clang-format on

#include <algorithm>
#include <cstring>
#include <vector>

#include "daphne/engine/text.h"

using Microsoft::WRL::ComPtr;

namespace daphne {
namespace {

constexpr PROPERTYKEY kFriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
constexpr GUID kSubtypePcm = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
constexpr GUID kSubtypeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
constexpr HRESULT kBufferNotAligned = static_cast<HRESULT>(0x88890019);
constexpr HRESULT kDeviceInvalidated = static_cast<HRESULT>(0x88890004);

std::string hr_text(const char* what, HRESULT hr) {
    char buf[128];
    snprintf(buf, sizeof buf, "%s failed (HRESULT 0x%08lX)", what, static_cast<unsigned long>(hr));
    return buf;
}

void set_type(DeviceFormat& f, SampleType t) {
    f.type = t;
    switch (t) {
        case SampleType::Float32: f.container_bits = 32; f.valid_bits = 32; break;
        case SampleType::Int32: f.container_bits = 32; f.valid_bits = 32; break;
        case SampleType::Int24In32: f.container_bits = 32; f.valid_bits = 24; break;
        case SampleType::Packed24: f.container_bits = 24; f.valid_bits = 24; break;
        case SampleType::Int16: f.container_bits = 16; f.valid_bits = 16; break;
    }
}

WAVEFORMATEXTENSIBLE make_wfx(const DeviceFormat& f, DWORD mask) {
    WAVEFORMATEXTENSIBLE w{};
    w.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    w.Format.nChannels = static_cast<WORD>(f.channels);
    w.Format.nSamplesPerSec = static_cast<DWORD>(f.rate);
    w.Format.wBitsPerSample = static_cast<WORD>(f.container_bits);
    w.Format.nBlockAlign = static_cast<WORD>(f.block_align());
    w.Format.nAvgBytesPerSec = w.Format.nSamplesPerSec * w.Format.nBlockAlign;
    w.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    w.Samples.wValidBitsPerSample = static_cast<WORD>(f.valid_bits);
    w.dwChannelMask = mask;
    w.SubFormat = f.type == SampleType::Float32 ? kSubtypeFloat : kSubtypePcm;
    return w;
}

// Channel masks tried for a channel count.
std::vector<DWORD> channel_masks(int ch) {
    switch (ch) {
        case 1: return {0x4};
        case 2: return {0x3};
        case 4: return {0x33, 0xF};
        case 6: return {0x3F, 0x60F};
        case 8: return {0x63F};
        default: return {0};
    }
}

// Rates tried: requested, the same family halved down to 44.1k / 48k, then 192k, 96k, 48k, 44.1k.
std::vector<int> candidate_rates(int rate) {
    std::vector<int> r{rate};
    auto add = [&](int v) {
        if (v > 0 && std::find(r.begin(), r.end(), v) == r.end()) r.push_back(v);
    };
    for (int base : {44100, 48000}) {
        if (rate % base == 0)
            for (int v = rate / 2; v >= base; v /= 2) add(v);
    }
    for (int v : {192000, 96000, 48000, 44100}) add(v);
    return r;
}

bool classify_mix(const WAVEFORMATEX* mix, DeviceFormat& f) {
    f.rate = static_cast<int>(mix->nSamplesPerSec);
    f.channels = mix->nChannels;
    GUID sub = kSubtypePcm;
    int valid = mix->wBitsPerSample;
    if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        auto* x = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix);
        sub = x->SubFormat;
        valid = x->Samples.wValidBitsPerSample;
    } else if (mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        sub = kSubtypeFloat;
    }
    if (sub == kSubtypeFloat && mix->wBitsPerSample == 32) set_type(f, SampleType::Float32);
    else if (mix->wBitsPerSample == 32) set_type(f, valid == 24 ? SampleType::Int24In32 : SampleType::Int32);
    else if (mix->wBitsPerSample == 24) set_type(f, SampleType::Packed24);
    else if (mix->wBitsPerSample == 16) set_type(f, SampleType::Int16);
    else return false;
    return true;
}

}  // namespace

struct WasapiOutput::Impl {
    bool com = false;
    bool autoconvert = false;   // shared mode: Windows resamples to the mixer rate
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioRenderClient> render;
    HANDLE audio_event = nullptr;
    HANDLE stop_event = nullptr;
    WAVEFORMATEXTENSIBLE wfx{};
    std::vector<uint8_t> silence;
};

WasapiOutput::WasapiOutput() : impl_(new Impl) {}
WasapiOutput::~WasapiOutput() {
    close();
    delete impl_;
}

bool WasapiOutput::open(const WasapiOptions& opt, int rate, int channels, std::string* error) {
    close();
    // the interfaces held here are released before close() leaves COM: Release after CoUninitialize
    // crashes on a thread where this was the only initialization
    ComPtr<IMMDeviceEnumerator> en;
    ComPtr<IPropertyStore> props;
    auto fail = [&](const std::string& msg) {
        if (error) *error = msg;
        props.Reset();
        en.Reset();
        close();
        return false;
    };
    const HRESULT ci = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    impl_->com = SUCCEEDED(ci);
    exclusive_ = opt.exclusive;

    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
    if (FAILED(hr)) return fail(hr_text("MMDeviceEnumerator", hr));

    const bool numeric = !opt.device.empty() && std::all_of(opt.device.begin(), opt.device.end(), ::isdigit);
    if (opt.device.empty()) {
        hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &impl_->device);
    } else if (numeric) {
        ComPtr<IMMDeviceCollection> col;
        hr = en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col);
        if (SUCCEEDED(hr)) hr = col->Item(static_cast<UINT>(std::stoul(opt.device)), &impl_->device);
    } else {
        hr = en->GetDevice(to_wide(opt.device).c_str(), &impl_->device);
    }
    if (FAILED(hr) || !impl_->device) return fail("wasapi: device not found: " + opt.device);

    if (SUCCEEDED(impl_->device->OpenPropertyStore(STGM_READ, &props))) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(props->GetValue(kFriendlyName, &v)) && v.vt == VT_LPWSTR) name_ = to_utf8(v.pwszVal);
        PropVariantClear(&v);
    }

    auto activate = [&]() {
        impl_->client.Reset();
        return impl_->device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                       reinterpret_cast<void**>(impl_->client.GetAddressOf()));
    };
    hr = activate();
    if (FAILED(hr)) return fail(hr_text("Activate IAudioClient", hr));

    // ---- format ----
    if (exclusive_) {
        static const SampleType kOrder[] = {SampleType::Int32, SampleType::Int24In32, SampleType::Float32,
                                            SampleType::Packed24, SampleType::Int16};
        bool found = false;
        for (int r : candidate_rates(rate)) {
            for (DWORD mask : channel_masks(channels)) {
                for (SampleType t : kOrder) {
                    DeviceFormat f;
                    f.rate = r;
                    f.channels = channels;
                    set_type(f, t);
                    auto w = make_wfx(f, mask);
                    if (impl_->client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &w.Format, nullptr) == S_OK) {
                        fmt_ = f;
                        impl_->wfx = w;
                        found = true;
                        break;
                    }
                }
                if (found) break;
            }
            if (found) break;
        }
        if (!found)
            return fail("wasapi: exclusive mode: the device cannot lock " + std::to_string(rate) +
                        " Hz / " + std::to_string(channels) + " Ch");
    } else {
        WAVEFORMATEX* mix = nullptr;
        hr = impl_->client->GetMixFormat(&mix);
        if (FAILED(hr) || !mix) return fail("wasapi: shared mode: cannot query the Windows mixer format");
        const bool ok = classify_mix(mix, fmt_);
        // ask Windows to resample to the mixer when the rates differ (shared mode is never
        // bit-perfect anyway), so the stream plays at the right speed
        system_rate_ = 0;
        if (ok && rate > 0 && fmt_.rate != rate) {
            system_rate_ = fmt_.rate;   // the mixer's rate: what the device plays (the report, MIKU's signal path)
            fmt_.rate = rate;
            impl_->autoconvert = true;
        }
        if (ok) {
            const DWORD mask = mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE
                                   ? reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix)->dwChannelMask
                                   : channel_masks(fmt_.channels).front();
            impl_->wfx = make_wfx(fmt_, mask);
        }
        CoTaskMemFree(mix);
        if (!ok) return fail("shared mode: unsupported mixer format");
    }
    matched_ = fmt_.rate == rate && fmt_.channels == channels;

    // ---- initialize ----
    REFERENCE_TIME def_period = 0, min_period = 0;
    impl_->client->GetDevicePeriod(&def_period, &min_period);
    const AUDCLNT_SHAREMODE mode = exclusive_ ? AUDCLNT_SHAREMODE_EXCLUSIVE : AUDCLNT_SHAREMODE_SHARED;
    const DWORD flags = exclusive_ ? AUDCLNT_STREAMFLAGS_EVENTCALLBACK
                        : impl_->autoconvert ? AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
                                             : 0;
    // Exclusive: one buffer = one device period. A device whose clock is coarse (a virtual cable timed by
    // the 15.6 ms system tick) needs a longer one: output.buffer_ms.
    const REFERENCE_TIME def_duration = exclusive_ ? std::max<REFERENCE_TIME>(def_period, 250000) : 1000000;
    const REFERENCE_TIME want = opt.buffer_ms > 0
                                    ? std::max<REFERENCE_TIME>(static_cast<REFERENCE_TIME>(std::max(opt.buffer_ms, 10)) * 10000,
                                                               exclusive_ ? def_period : 0)
                                    : def_duration;
    auto initialize = [&](REFERENCE_TIME duration) {
        REFERENCE_TIME period = exclusive_ ? duration : 0;
        HRESULT r = impl_->client->Initialize(mode, flags, duration, period, &impl_->wfx.Format, nullptr);
        if (r == kBufferNotAligned) {
            UINT32 frames = 0;
            impl_->client->GetBufferSize(&frames);
            duration = static_cast<REFERENCE_TIME>(10000000.0 * frames / fmt_.rate + 0.5);
            period = exclusive_ ? duration : 0;
            r = activate();
            if (SUCCEEDED(r)) r = impl_->client->Initialize(mode, flags, duration, period, &impl_->wfx.Format, nullptr);
        }
        return r;
    };
    buffer_fallback_ = false;
    hr = initialize(want);
    if (FAILED(hr) && want != def_duration) {   // the device does not take that buffer: the default
        hr = activate();
        if (SUCCEEDED(hr)) hr = initialize(def_duration);
        buffer_fallback_ = SUCCEEDED(hr);
    }
    if (FAILED(hr)) return fail(hr_text("Initialize", hr));

    if (exclusive_) {
        impl_->audio_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        hr = impl_->client->SetEventHandle(impl_->audio_event);
        if (FAILED(hr)) return fail(hr_text("SetEventHandle", hr));
    }
    impl_->stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    hr = impl_->client->GetService(IID_PPV_ARGS(&impl_->render));
    if (FAILED(hr)) return fail(hr_text("GetService RenderClient", hr));
    UINT32 frames = 0;
    impl_->client->GetBufferSize(&frames);
    buffer_frames_ = static_cast<int>(frames);
    REFERENCE_TIME lat = 0;
    impl_->client->GetStreamLatency(&lat);
    latency_ms_ = lat / 10000.0;
    return true;
}

bool WasapiOutput::start(ByteRing* ring, const std::atomic<bool>* eof, std::string* error) {
    if (!impl_->client) {
        if (error) *error = "not open";
        return false;
    }
    ring_ = ring;
    eof_ = eof;
    drained_ = false;
    failed_ = false;
    ResetEvent(impl_->stop_event);
    feeder_.configure(fmt_);
    // pre-roll: one buffer of silence before Start
    BYTE* p = nullptr;
    if (SUCCEEDED(impl_->render->GetBuffer(buffer_frames_, &p)))
        impl_->render->ReleaseBuffer(buffer_frames_, AUDCLNT_BUFFERFLAGS_SILENT);
    const HRESULT hr = impl_->client->Start();
    if (FAILED(hr)) {
        if (error) *error = hr_text("Start", hr);
        return false;
    }
    thread_ = std::thread([this] { exclusive_ ? render_exclusive() : render_shared(); });
    return true;
}

void WasapiOutput::render_exclusive() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD task = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    LARGE_INTEGER freq, last{}, now;
    QueryPerformanceFrequency(&freq);
    const size_t bytes = static_cast<size_t>(buffer_frames_) * fmt_.block_align();
    HANDLE waits[2] = {impl_->stop_event, impl_->audio_event};
    // a stall: no event for two buffers (at least 50 ms)
    const DWORD stall_ms = std::max<DWORD>(50, static_cast<DWORD>(2000.0 * buffer_frames_ / fmt_.rate));
    for (;;) {
        const DWORD w = WaitForMultipleObjects(2, waits, FALSE, stall_ms);
        if (w == WAIT_OBJECT_0) break;
        if (w == WAIT_TIMEOUT) {
            stats_.event_timeouts++;   // the device clock stalled
            continue;
        }
        QueryPerformanceCounter(&now);
        if (last.QuadPart) {
            const int64_t us = (now.QuadPart - last.QuadPart) * 1000000 / freq.QuadPart;
            if (us > stats_.max_interval_us) stats_.max_interval_us = us;
        }
        last = now;
        BYTE* dst = nullptr;
        HRESULT hr = impl_->render->GetBuffer(buffer_frames_, &dst);
        if (FAILED(hr)) {
            if (hr == kDeviceInvalidated) failed_ = true;   // "Exclusive Device Invalidated"
            if (failed_) break;
            continue;
        }
        (void)bytes;
        if (feeder_.fill(dst, buffer_frames_, ring_, eof_->load(), stats_)) drained_ = true;
        impl_->render->ReleaseBuffer(buffer_frames_, 0);
        stats_.callbacks++;
    }
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    CoUninitialize();
}

void WasapiOutput::render_shared() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD task = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    const int block = fmt_.block_align();
    while (WaitForSingleObject(impl_->stop_event, 0) != WAIT_OBJECT_0) {
        UINT32 pad = 0;
        HRESULT hr = impl_->client->GetCurrentPadding(&pad);
        if (hr == kDeviceInvalidated) {
            failed_ = true;   // "Shared Device Invalidated"
            break;
        }
        const UINT32 avail = SUCCEEDED(hr) ? buffer_frames_ - pad : 0;
        if (avail > 0) {
            BYTE* dst = nullptr;
            if (SUCCEEDED(impl_->render->GetBuffer(avail, &dst))) {
                (void)block;
                if (feeder_.fill(dst, static_cast<int>(avail), ring_, eof_->load(), stats_)) drained_ = true;
                impl_->render->ReleaseBuffer(avail, 0);
                stats_.callbacks++;
            }
        }
        Sleep(2);   // shared mode is polled with ~2 ms sleeps
    }
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    CoUninitialize();
}

void WasapiOutput::stop() {
    if (thread_.joinable()) {
        SetEvent(impl_->stop_event);
        thread_.join();
    }
    if (impl_->client) impl_->client->Stop();
}

void WasapiOutput::close() {
    stop();
    impl_->render.Reset();
    impl_->client.Reset();
    impl_->device.Reset();
    if (impl_->audio_event) CloseHandle(impl_->audio_event);
    if (impl_->stop_event) CloseHandle(impl_->stop_event);
    impl_->audio_event = impl_->stop_event = nullptr;
    if (impl_->com) CoUninitialize();
    impl_->com = false;
}

}  // namespace daphne
