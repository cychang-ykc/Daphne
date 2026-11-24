// Linux: no device output yet (ALSA is a later step, N25). The player, the
// chain and the decoders build and run here; tests give the player a fake device (PlayerDeps).
#include "daphne/engine/devices.h"
#include "daphne/engine/player.h"

namespace daphne {

OutputOpener platform_output_opener() {
    return [](const OutputRequest&, std::string* error) -> std::unique_ptr<AudioOutput> {
        if (error) *error = "no audio output on this platform yet (the WASAPI and ASIO outputs are Windows only)";
        return nullptr;
    };
}

// nothing to describe yet: the defaults (an exclusive PCM device, as the tests' fake one)
OutputCapsQuery platform_output_caps() {
    return [](const OutputChoice&) { return OutputCaps{}; };
}

std::vector<WasapiDevice> list_wasapi_devices(bool, std::string* error) {
    if (error) *error = "WASAPI is Windows only";
    return {};
}

std::vector<AsioDriver> list_asio_drivers(std::string* error) {
    if (error) *error = "ASIO is Windows only";
    return {};
}

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

}  // namespace daphne
