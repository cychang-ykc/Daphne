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
        // convert (the player's last PCM attempt, OutputCaps::converts): the DAC has no rate the chain can use for this
        // track in exclusive mode (192 kHz on a DAC that stops at 96 kHz, 44.1 kHz on a 48 kHz-only one), so this
        // track goes through the Windows mixer, which resamples to the device (as Core Audio's converter does on
        // macOS). The next track is tried in exclusive mode again.
        const bool exclusive = r.out.api != OutputApi::WasapiShared && !r.convert;
        if (!w->open({r.out.device, exclusive, r.buffer_ms}, r.rate, r.channels, error)) return nullptr;
        return w;
    };
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
        c.converts = o.api == OutputApi::WasapiExclusive;   // through the Windows mixer (platform_output_opener)
        return c;
    };
}

}  // namespace daphne
