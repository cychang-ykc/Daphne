#pragma once
// Settings: one JSON document owned by the host (the CLI or an application). Missing fields keep
// their defaults; unknown fields are reported as warnings and ignored.
//
// {
//   "version": 1,
//   "output":  { "api": "wasapi_exclusive" | "wasapi_shared" | "asio" | "coreaudio_exclusive" | "coreaudio_shared",
//                "device": "",
//                "rate": "native" | "auto" | "2x" | "<Hz>", "dsd": "auto" | "native" | "dop" | "pcm",
//                "dsd_pcm_lowpass": 24 | 30 | 50, "dsd_pcm_limiter": true, "max_dsd": 0 | 64 | ... | 512,
//                "dsd_lead_in_ms": 0 .. 2000, "buffer_ms": 0 .. 200 },
//   "core":    "pcm" | "dsd",
//   "filter":  { "taps": 16 .. 1048576, "phase": "linear" | "intermediate" | "minimum" | "custom",
//                "phase_custom": 0 .. 1, "apodize": false, "double_double": false },
//   "eq":      { "enabled": false, "preamp_db": "auto" | -60 .. 24,
//                "bands": [ { "type": "PK" | "LS" | "HS" | "HP" | "HP24" | "LP" | "LP24" | "NOTCH" | "AP" | "BP",
//                             "f": Hz, "q": 0.01 .. 100, "gain_db": -30 .. 30, "on": true } ] },
//   "crossfeed": { "enabled": false, "fc": 100 .. 3000, "amount": 0 .. 1 },
//   "volume":  { "mode": "digital" | "fixed", "db": -120 .. 0 },
//   "headroom_db": -24 .. 24,
//   "dsd_modulator": { "order": 5 | 8, "osr": 64 | 128 | 256 | 512 | 1024 },
//   "design":  { "threads": 0 .. 256, "cache_limit_mb": 0 .. },
//   "color":   { "enabled": false, "model": "fixed-point" | "circuit",
//                "preset": "set-2a3" | "set-300b" | "set-845" | "pp-el34-ul" | "set-300b-x" | "h2-pure" (experimental),
//                "drive": 0 .. kColorDriveMax,
//                "feedback_db": 0 | 6 | 12 | 20 | 30, "bias": "cool" | "standard" | "hot" | <mA, per preset>,
//                "load_ohms": 4 | 8 | 16, "makeup_gain": "auto" | -6 .. 6 }
// }

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace daphne {

// Device outputs: WASAPI and ASIO on Windows, Core Audio on macOS (an API of another platform fails to open).
enum class OutputApi { WasapiExclusive, WasapiShared, Asio, CoreAudioExclusive, CoreAudioShared };
// This platform's exclusive (bit-perfect) output, the default.
#if defined(__APPLE__)
inline constexpr OutputApi kDefaultOutputApi = OutputApi::CoreAudioExclusive;
#else
inline constexpr OutputApi kDefaultOutputApi = OutputApi::WasapiExclusive;
#endif
// pcm: decoded PCM through the filter / resampler to the device; dsd: PCM converted to DSD by the
// modulator (DSD files are sent directly in both modes when the device takes DSD).
enum class CoreMode { Pcm, Dsd };
// DSD output: auto (ASIO native DSD, else DoP), native, dop, pcm (DSD always converted to PCM).
enum class DsdMode { Auto, Native, Dop, Pcm };
// Filter phase: partial minimum phase 0 / 0.5 / 1, or phase_custom.
enum class FilterPhase { Linear, Intermediate, Minimum, Custom };
// EQ band types. HP / LP are 12 dB/oct, HP24 / LP24 two Butterworth sections; BP has a 0 dB peak.
enum class EqType { PK, LS, HS, HP, HP24, LP, LP24, NOTCH, AP, BP };
// digital: the gain stage applies volume.db; fixed: no volume in the core (a host that controls the
// device endpoint volume itself sends fixed).
enum class VolumeMode { Digital, Fixed };
// Tube Color (doc/N28). fixed-point: the static curve of a preset's amplifier at its operating point (M2). v1 has
// only this model (the circuit model with dynamic effects is optional, later).
enum class ColorModel { FixedPoint };
// Set300bX: M2's experiment (doc/N28), set-300b with a constant-current-loaded driver (more H2); on the integration
// branch only, removed or kept after listening
// H2Pure: M2's experiment (doc/N28), second harmonic and nothing odd, on set-300b's linear model and loads
enum class ColorPreset { Set2a3, Set300b, Set845, PpEl34Ul, Set300bX, H2Pure };
enum class ColorBias { Cool, Standard, Hot };
// What the amplifier drives (doc/N28): the 8 ohm resistor of the reference data, or a loudspeaker's impedance (a
// two-way stand-mount, vented or sealed). load_ohms is the output transformer's tap (M3); this is what it drives.
enum class ColorLoad { Resistor8, BookshelfVented, BookshelfSealed };

struct OutputSettings {
    OutputApi api = kDefaultOutputApi;
    std::string device;          // empty = system default
    // PCM output rate: "native" (the source rate), "auto" (cap 705600 for 44.1 kHz-family sources,
    // else 768000), "2x" (twice the source rate, for every track) or "<Hz>" (cap); with a cap the
    // ratio is the largest power of two <= 32 with ratio * fs_in <= cap.
    std::string rate = "native";
    DsdMode dsd = DsdMode::Auto;
    // DSD files converted to PCM: low-pass pass-band edge in kHz, 24 (stop band from 30 kHz) |
    // 30 (stop 60) | 50 (stop 80).
    int dsd_pcm_lowpass = 24;
    // DSD files converted to PCM: look-ahead limiter at the end of the chain, so peaks above the
    // SACD reference level (0 dBFS after the +6 dB conversion) are turned down instead of clipped.
    bool dsd_pcm_limiter = true;
    // Highest DSD multiple the DAC takes (64 | 128 | 256 | 512), 0 = no limit. Some drivers accept
    // DSD rates their DAC cannot play: DSD files above it are converted to PCM, the modulator's
    // osr is capped.
    int max_dsd = 0;
    // DSD silence before the music whenever a DSD stream starts on a device, in ms: a DAC switching
    // from PCM to DSD mutes while it locks, which would cut the start of the track.
    int dsd_lead_in_ms = 100;
    // WASAPI device buffer in ms, 0 = default (exclusive: the device period, at least 25 ms; shared:
    // 100 ms), else 10 .. 200 (1 .. 9 count as 10). A device clocked coarsely (a virtual cable timed by
    // the 15.6 ms system tick) runs short with 25 ms buffers and pads the gap with zeros; 50 ms cured it
    // in every run. Below 10 ms nothing is gained (the player buffers 0.5 s ahead anyway; the device
    // buffer only sets how fast pause and seek respond) and few devices take it; above 200 ms pause and
    // seek wait for a whole buffer. A buffer the device does not accept falls back to the default. ASIO:
    // the driver's buffer size applies.
    int buffer_ms = 0;
};

// The resampling filter (filter_design.h). Taps are per input sample; the filter has taps * ratio
// coefficients.
struct FilterSettings {
    int taps = 2048;
    FilterPhase phase = FilterPhase::Intermediate;
    double phase_custom = 0.5;   // for phase = custom
    double phase_alpha() const {
        switch (phase) {
            case FilterPhase::Linear: return 0.0;
            case FilterPhase::Minimum: return 1.0;
            case FilterPhase::Custom: return phase_custom;
            default: return 0.5;
        }
    }
    bool apodize = false;
    bool double_double = false;  // convolve in double-double with the coefficients' low parts
};

struct EqBand {
    EqType type = EqType::PK;
    double f = 1000.0;
    double q = 0.7071;           // Q; the slope S for LS / HS
    double gain_db = 0.0;
    bool on = true;
};

struct EqSettings {
    bool enabled = false;
    // nullopt = "auto" (the preamp that keeps the EQ's largest boost from clipping); a value can only
    // lower it: min(value, auto)
    std::optional<double> preamp_db;
    std::vector<EqBand> bands;
};

struct CrossfeedSettings {
    bool enabled = false;
    double fc = 700.0;           // Hz, 100 .. 3000
    double amount = 0.22;        // 0 .. 1
};

struct VolumeSettings {
    VolumeMode mode = VolumeMode::Fixed;
    double db = 0.0;
};

struct ModulatorSettings {
    int order = 8;               // 5 (least ultrasonic noise) | 8 (quietest audio band)
    int osr = 128;               // DSD64 .. DSD1024
};

// Filter design runs on several cores, below normal priority while music plays.
struct DesignSettings {
    int threads = 0;             // 0 = automatic (all cores but two)
    int cache_limit_mb = 1024;   // filter cache size limit (filter_cache.h); 0 = no limit
};

// The upper limit of the strength slider (drive 1.0 = 100 %, the preset's calibration). One constant (the panel reads
// it through color_info). 4.0 (400 %) from M2's listening on (2026-11-21): on the tube-color integration branch it is
// open for listening; before the branch is merged into main, 400 % must pass every quality check (doc/N28).
inline constexpr double kColorDriveMax = 4.0;

// Tube Color (doc/N28). enabled false (the default): not in the chain, playback exactly as without it. Before M2,
// only test builds can run it (daphne::color::available()); elsewhere enabled true changes nothing either.
struct ColorSettings {
    bool enabled = false;
    ColorModel model = ColorModel::FixedPoint;
    ColorPreset preset = ColorPreset::Set300b;
    double drive = 1.0;                  // 0 .. kColorDriveMax; 0 = the stage passes samples through unchanged
    int feedback_db = 6;                 // pp-el34-ul's feedback step: 0 | 6 | 12 | 20 | 30 (30 = the Mullard 5-20)
    ColorBias bias = ColorBias::Standard;
    // A cathode current per output tube in mA instead of the named bias. Kept as set; it applies only while it lies
    // in the current preset's range (color_bias_range), else the standard bias does (color_bias_current_ma).
    std::optional<double> bias_ma;
    int load_ohms = 8;                   // the output tap: 4 | 8 | 16
    ColorLoad load = ColorLoad::BookshelfVented;   // "resistor-8" | "bookshelf-vented" | "bookshelf-sealed"
    // nullopt = "auto" (loudness kept within 0.5 dB); else a trim of -6 .. 6 dB on top of auto
    std::optional<double> makeup_gain_db;
};

// The cathode current per output tube a preset allows (doc/N28): lo = 0.6 x the reference circuit's quiescent current
// (still class A), hi = the datasheet's plate dissipation (or maximum current) at the preset's plate voltage.
// hot_cap_ma: the current at 90 % of the rated plate dissipation, the ceiling of the hot bias.
struct ColorBiasRange {
    double lo_ma, standard_ma, hi_ma, hot_cap_ma;
};
ColorBiasRange color_bias_range(ColorPreset p);
// The preset's settings name ("set-300b").
const char* color_preset_name(ColorPreset p);
// The load's settings name ("bookshelf-vented").
const char* color_load_name(ColorLoad l);
// The named biases (doc/N28): cool = 0.8 x standard; hot = min(1.1 x standard, hot_cap_ma), offered only where it is
// at least 5 % above standard (nullopt otherwise: of the four presets only set-300b has it).
inline constexpr double kColorCoolFactor = 0.8, kColorHotFactor = 1.1, kColorHotMinStep = 0.05;
double color_cool_ma(ColorPreset p);
std::optional<double> color_hot_ma(ColorPreset p);
// The cathode current that applies: bias_ma when it lies in the preset's range, else the named bias; standard where
// the setting does not apply to the preset (a current outside its range, hot where it has none).
double color_bias_current_ma(const ColorSettings& c);
// True when the setting does not apply to the current preset and the standard bias is used instead (the setting is
// kept; the playback path reports it once).
bool color_bias_falls_back(const ColorSettings& c);
inline constexpr double kColorMakeupTrimMaxDb = 6.0;
// Tube Color's soft limiter on the PCM path (doc/N28): the ceiling (-0.3 dBFS) and its knee (from -0.5 dBFS)
inline constexpr double kColorLimitCeiling = 0.9660508789898133;   // 10^(-0.3/20)
inline constexpr double kColorLimitKneeDb = 0.4;   // (b2, 2026-11-21: 4 dB from -2.3 dBFS compressed loud masters at every drive)

struct Settings {
    int version = 1;
    OutputSettings output;
    CoreMode core = CoreMode::Pcm;
    FilterSettings filter;
    EqSettings eq;
    CrossfeedSettings crossfeed;
    VolumeSettings volume;
    double headroom_db = 0.0;    // per-track gain (ReplayGain) before the processing chain
    ModulatorSettings modulator;
    DesignSettings design;
    ColorSettings color;
};

// True when Tube Color is in the chain: enabled, and this build can run it (before M2: test builds only).
bool color_active(const Settings& s);

// M2 plays every preset at its reference operating point (standard bias, 8 ohm tap); the bias and the load become
// adjustable in M3 (doc/N28). A host shows those controls only when this is true.
inline constexpr bool kColorOperatingPointAdjustable = false;

// What a host needs to draw Tube Color's controls without copying the core's constants (doc/N28, dc_color_info):
//   {"available": b,            this build can run the stage for some preset (else color.enabled true changes nothing)
//    "curve": "test" | "tubes" | "none",  the M1 test curve (test builds: every preset), the presets' own curves,
//                               or none (a build without Tube Color)
//    "drive_max": x,            the strength slider's limit (kColorDriveMax; 1.0 = 100 %)
//    "makeup_trim_max_db": x,   makeup_gain's trim range is +- this
//    "feedback_db": [...],      pp-el34-ul's feedback steps
//    "load_ohms": [...],
//    "operating_point_adjustable": b,   bias and load (M3)
//    "loads": [{"id", "label"}, ...], "default_load": id,
//    "presets": [{"id", "label", "push_pull": b, "available": b (its curve is in this build), "experimental": b,
//                 "h2_table": [[drive, percent], ...] (h2-pure: the strength's H2 for a 1 kHz sine at -18 dBFS RMS),
//                 "bias_ma": {"lo", "standard", "hi", "cool", "hot" (null where the preset offers none)}}, ...]}
nlohmann::json color_info();
// The settings as far as the chain's structure and the prepared chains go: Tube Color's fields are cleared when it is
// not active, so that editing them (while disabled, or in a build that cannot run it) rebuilds nothing.
Settings without_inactive_color(Settings s);

struct Diagnostic {
    enum class Level { Warning, Error } level;
    std::string path;    // e.g. "eq.bands[2].q"
    std::string message;
};

struct LoadResult {
    Settings settings;               // defaults filled in; invalid fields keep their default
    std::vector<Diagnostic> diagnostics;
    bool ok() const;                 // no errors (warnings allowed)
};

constexpr int kSettingsVersion = 1;

LoadResult settings_from_json(const nlohmann::json& j);
LoadResult settings_from_string(const std::string& text);
nlohmann::json settings_to_json(const Settings& s);

// PCM output rate for a source rate; the source rate when no ratio fits.
int pcm_output_rate(int fs_in, const std::string& rate);
// The modulator's input rate: the source upsampled to 705.6 / 768 kHz by the filter first.
int modulator_input_rate(int fs_in);
// DSD clock of the modulator: 44100 * osr (the DSD standard family, whatever the source).
inline int dsd_clock(int osr) { return 44100 * osr; }

}  // namespace daphne
