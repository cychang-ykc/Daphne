// The outputs of the macOS build: Core Audio exclusive (hog mode) / shared (N24, N25).
#include "daphne/engine/coreaudio_output.h"
#include "daphne/engine/devices.h"
#include "daphne/engine/player.h"

namespace daphne {

namespace {

bool is_coreaudio(OutputApi a) { return a == OutputApi::CoreAudioExclusive || a == OutputApi::CoreAudioShared; }

}  // namespace

OutputOpener platform_output_opener() {
    return [](const OutputRequest& r, std::string* error) -> std::unique_ptr<AudioOutput> {
        if (!is_coreaudio(r.out.api)) {
            if (error) *error = "WASAPI and ASIO are Windows only: use output.api coreaudio_exclusive or coreaudio_shared";
            return nullptr;
        }
        auto c = std::make_unique<CoreAudioOutput>();
        if (!c->open({r.out.device, r.out.api == OutputApi::CoreAudioExclusive, r.buffer_ms, r.convert}, r.rate, r.channels, error)) return nullptr;
        return c;
    };
}

// Core Audio has no native DSD (DoP through an integer DAC format, exclusive only); shared mode is the
// system mixer at the device's own rate.
OutputCapsQuery platform_output_caps() {
    return [](const OutputChoice& o) {
        OutputCaps c;
        c.native_dsd = false;
        c.shared_mix = o.api == OutputApi::CoreAudioShared;
        c.takes_buffer_ms = true;
        c.converts = o.api == OutputApi::CoreAudioExclusive;
        return c;
    };
}

std::vector<WasapiDevice> list_wasapi_devices(bool, std::string* error) {
    if (error) *error = "WASAPI is Windows only";
    return {};
}

std::vector<AsioDriver> list_asio_drivers(std::string* error) {
    if (error) *error = "ASIO is Windows only";
    return {};
}

}  // namespace daphne
