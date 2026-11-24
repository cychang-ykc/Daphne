// The outputs of the Windows build: WASAPI exclusive / shared and ASIO (N24).
#include "daphne/engine/asio_output.h"
#include "daphne/engine/devices.h"
#include "daphne/engine/player.h"
#include "daphne/engine/wasapi_output.h"

namespace daphne {

OutputOpener platform_output_opener() {
    return [](const OutputRequest& r, std::string* error) -> std::unique_ptr<AudioOutput> {
        if (r.out.api == OutputApi::CoreAudioExclusive || r.out.api == OutputApi::CoreAudioShared) {
            if (error) *error = "Core Audio is macOS only: use output.api wasapi_exclusive, wasapi_shared or asio";
            return nullptr;
        }
        if (r.out.api == OutputApi::Asio) {
            auto a = std::make_unique<AsioOutput>();
            if (!a->open({r.out.device, r.dsd}, r.rate, r.channels, error)) return nullptr;
            return a;
        }
        auto w = std::make_unique<WasapiOutput>();
        if (!w->open({r.out.device, r.out.api != OutputApi::WasapiShared, r.buffer_ms}, r.rate, r.channels, error)) return nullptr;
        return w;
    };
}

// WASAPI and ASIO tell their rates only by opening: the player finds out that way
OutputProbeQuery platform_output_probe() {
    return [](const OutputChoice&) { return OutputProbe{}; };
}

std::vector<CoreAudioDevice> list_coreaudio_devices(std::string* error) {
    if (error) *error = "Core Audio is macOS only";
    return {};
}

void keep_output_devices(bool) {}
bool release_kept_output_device() { return false; }
bool output_device_kept() { return false; }

OutputCapsQuery platform_output_caps() {
    return [](const OutputChoice& o) {
        OutputCaps c;
        c.native_dsd = o.api == OutputApi::Asio;
        c.shared_mix = o.api == OutputApi::WasapiShared;
        c.takes_buffer_ms = o.api != OutputApi::Asio;
        return c;
    };
}

}  // namespace daphne
