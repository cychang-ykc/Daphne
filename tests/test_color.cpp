// Tube Color (doc/N28), M1: settings and the gate that keeps it out of the chain where it cannot run.
#include <catch_amalgamated.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "daphne/engine/chain.h"
#include "daphne/engine/filters.h"
#include "daphne/platform/platform.h"
#include "daphne/engine/settings.h"
#include "daphne/color/tube_color.h"
#include "../core/src/player/track_queue.h"
#include "test_util.h"

using daphne::ColorBias;
using daphne::ColorModel;
using daphne::ColorPreset;
using daphne::Settings;

namespace {

daphne::LoadResult load(const std::string& color_json) { return daphne::settings_from_string(R"({"color": )" + color_json + "}"); }

bool has_error(const daphne::LoadResult& r, const std::string& path) {
    for (const auto& d : r.diagnostics)
        if (d.level == daphne::Diagnostic::Level::Error && d.path == path) return true;
    return false;
}

}  // namespace

TEST_CASE("color settings: defaults, off", "[color]") {
    const Settings s;
    CHECK_FALSE(s.color.enabled);
    CHECK(s.color.model == ColorModel::FixedPoint);
    CHECK(s.color.preset == ColorPreset::Set300b);
    CHECK(s.color.drive == 1.0);
    CHECK(s.color.feedback_db == 6);
    CHECK(s.color.bias == ColorBias::Standard);
    CHECK_FALSE(s.color.bias_ma.has_value());
    CHECK(s.color.load_ohms == 8);
    CHECK_FALSE(s.color.makeup_gain_db.has_value());
    CHECK(daphne::kColorDriveMax == 2.0);
    CHECK_FALSE(daphne::color_active(s));
}

TEST_CASE("color settings: read and written back", "[color]") {
    const auto r = load(R"({"enabled": true, "model": "fixed-point", "preset": "pp-el34-ul", "drive": 1.5, "feedback_db": 12,
                           "bias": 45.5, "load_ohms": 4, "makeup_gain": -3})");
    REQUIRE(r.ok());
    CHECK(r.diagnostics.empty());
    const auto& c = r.settings.color;
    CHECK(c.enabled);
    CHECK(c.preset == ColorPreset::PpEl34Ul);
    CHECK(c.drive == 1.5);
    CHECK(c.feedback_db == 12);
    REQUIRE(c.bias_ma.has_value());
    CHECK(*c.bias_ma == 45.5);
    CHECK(c.load_ohms == 4);
    REQUIRE(c.makeup_gain_db.has_value());
    CHECK(*c.makeup_gain_db == -3.0);
    // round trip
    const auto j = daphne::settings_to_json(r.settings);
    const auto r2 = daphne::settings_from_json(j);
    REQUIRE(r2.ok());
    CHECK(daphne::settings_to_json(r2.settings) == j);
    CHECK(j["color"]["bias"] == 45.5);
    CHECK(j["color"]["makeup_gain"] == -3.0);
    // a named bias replaces a current; "auto" makeup
    const auto r3 = load(R"({"bias": "hot", "makeup_gain": "auto"})");
    REQUIRE(r3.ok());
    CHECK(r3.settings.color.bias == ColorBias::Hot);
    CHECK_FALSE(r3.settings.color.bias_ma.has_value());
    CHECK_FALSE(r3.settings.color.makeup_gain_db.has_value());
    CHECK(daphne::settings_to_json(r3.settings)["color"]["bias"] == "hot");
}

TEST_CASE("color settings: invalid values are errors and keep the default", "[color]") {
    struct Case {
        const char* json;
        const char* path;
    };
    for (const Case& k : {Case{R"({"drive": 2.01})", "color.drive"}, Case{R"({"drive": -0.1})", "color.drive"},
                          Case{R"({"feedback_db": 10})", "color.feedback_db"}, Case{R"({"preset": "set-6b4g"})", "color.preset"},
                          Case{R"({"model": "spice"})", "color.model"}, Case{R"({"model": "circuit"})", "color.model"},
                          Case{R"({"bias": "warm"})", "color.bias"}, Case{R"({"bias": 0})", "color.bias"},
                          Case{R"({"bias": 34})", "color.bias"}, Case{R"({"bias": 101})", "color.bias"},
                          Case{R"({"load_ohms": 6})", "color.load_ohms"}, Case{R"({"makeup_gain": 6.5})", "color.makeup_gain"},
                          Case{R"({"makeup_gain": -7})", "color.makeup_gain"},
                          Case{R"({"enabled": "yes"})", "color.enabled"}}) {
        INFO(k.json);
        const auto r = load(k.json);
        CHECK(has_error(r, k.path));
        const Settings d;
        CHECK(daphne::settings_to_json(r.settings) == daphne::settings_to_json(d));
    }
    const auto unknown = load(R"({"warmth": 1})");
    CHECK(unknown.ok());
    REQUIRE(unknown.diagnostics.size() == 1);
    CHECK(unknown.diagnostics[0].path == "color.warmth");
    CHECK(load(R"({"drive": 2.0})").ok());   // the limit itself is allowed
    CHECK(load(R"({"drive": 0})").ok());
    CHECK(load(R"({"makeup_gain": 6})").ok());
    CHECK(load(R"({"makeup_gain": -6})").ok());
}

TEST_CASE("color settings: one invalid field falls back alone", "[color]") {
    const auto r = load(R"({"enabled": true, "preset": "set-845", "drive": 1.5, "feedback_db": 20, "load_ohms": 16,
                           "makeup_gain": 9, "model": "circuit", "bias": "hot"})");
    CHECK_FALSE(r.ok());
    CHECK(has_error(r, "color.makeup_gain"));
    CHECK(has_error(r, "color.model"));
    const auto& c = r.settings.color;
    CHECK_FALSE(c.makeup_gain_db.has_value());   // the invalid fields: their defaults ("auto", fixed-point)
    CHECK(c.model == ColorModel::FixedPoint);
    CHECK(c.enabled);                            // every valid field is applied
    CHECK(c.preset == ColorPreset::Set845);
    CHECK(c.drive == 1.5);
    CHECK(c.feedback_db == 20);
    CHECK(c.load_ohms == 16);
    CHECK(c.bias == ColorBias::Hot);
}

TEST_CASE("color settings: a bias current applies per preset and comes back", "[color]") {
    // 80 mA: in 300B's range (36 .. 100), outside 845's (44 .. 79)
    CHECK(daphne::color_bias_range(ColorPreset::Set300b).hi_ma == 100.0);
    CHECK(daphne::color_bias_range(ColorPreset::Set845).hi_ma == 79.0);
    auto r = load(R"({"preset": "set-300b", "bias": 80})");
    REQUIRE(r.ok());
    CHECK(r.diagnostics.empty());
    daphne::ColorSettings c = r.settings.color;
    CHECK(daphne::color_bias_current_ma(c) == 80.0);   // 300B: 80 mA
    CHECK_FALSE(daphne::color_bias_falls_back(c));
    c.preset = ColorPreset::Set845;                     // switched: outside the range, the standard bias, reported
    CHECK(daphne::color_bias_current_ma(c) == 72.5);
    CHECK(daphne::color_bias_falls_back(c));
    REQUIRE(c.bias_ma.has_value());
    CHECK(*c.bias_ma == 80.0);                          // the setting itself is kept
    c.preset = ColorPreset::Set300b;                    // switched back: 80 mA again
    CHECK(daphne::color_bias_current_ma(c) == 80.0);
    CHECK_FALSE(daphne::color_bias_falls_back(c));
    // read with a preset that does not allow it: a warning, not an error; kept, standard applies
    r = load(R"({"preset": "set-845", "bias": 80})");
    CHECK(r.ok());
    REQUIRE(r.diagnostics.size() == 1);
    CHECK(r.diagnostics[0].level == daphne::Diagnostic::Level::Warning);
    CHECK(r.diagnostics[0].path == "color.bias");
    CHECK(daphne::color_bias_falls_back(r.settings.color));
    CHECK(daphne::settings_to_json(r.settings)["color"]["bias"] == 80.0);   // written back as set
}

TEST_CASE("color settings: cool and hot per preset", "[color]") {
    using daphne::color_hot_ma;
    // cool = 0.8 x standard everywhere
    CHECK(daphne::color_cool_ma(ColorPreset::Set300b) == Catch::Approx(0.8 * 59.8));
    CHECK(daphne::color_cool_ma(ColorPreset::Set2a3) == Catch::Approx(0.8 * 58.9));
    // hot only where there is room for it: 300B (1.1 x 59.8 = 65.8 mA, below its 92.5 mA cap)
    REQUIRE(color_hot_ma(ColorPreset::Set300b).has_value());
    CHECK(*color_hot_ma(ColorPreset::Set300b) == Catch::Approx(1.1 * 59.8));
    CHECK_FALSE(color_hot_ma(ColorPreset::Set2a3).has_value());
    CHECK_FALSE(color_hot_ma(ColorPreset::Set845).has_value());
    CHECK_FALSE(color_hot_ma(ColorPreset::PpEl34Ul).has_value());
    // hot on 2A3: a warning, standard applies, "hot" is kept (300B uses it again)
    const auto r = load(R"({"preset": "set-2a3", "bias": "hot"})");
    CHECK(r.ok());
    REQUIRE(r.diagnostics.size() == 1);
    CHECK(r.diagnostics[0].level == daphne::Diagnostic::Level::Warning);
    CHECK(r.diagnostics[0].path == "color.bias");
    daphne::ColorSettings c = r.settings.color;
    CHECK(c.bias == ColorBias::Hot);
    CHECK(daphne::color_bias_falls_back(c));
    CHECK(daphne::color_bias_current_ma(c) == 58.9);
    CHECK(daphne::settings_to_json(r.settings)["color"]["bias"] == "hot");
    c.preset = ColorPreset::Set300b;
    CHECK_FALSE(daphne::color_bias_falls_back(c));
    CHECK(daphne::color_bias_current_ma(c) == Catch::Approx(65.78));
    c.preset = ColorPreset::Set845;
    CHECK(daphne::color_bias_falls_back(c));
    CHECK(daphne::color_bias_current_ma(c) == 72.5);
    // cool and standard apply to every preset
    for (auto p : {ColorPreset::Set2a3, ColorPreset::Set300b, ColorPreset::Set845, ColorPreset::PpEl34Ul}) {
        c.preset = p;
        c.bias = ColorBias::Cool;
        CHECK_FALSE(daphne::color_bias_falls_back(c));
        CHECK(daphne::color_bias_current_ma(c) >= daphne::color_bias_range(p).lo_ma);
        c.bias = ColorBias::Standard;
        CHECK(daphne::color_bias_current_ma(c) == daphne::color_bias_range(p).standard_ma);
    }
}

// Where Tube Color cannot run (every build but the test builds, before M2), enabled true must change nothing:
// the chain keys stay those of the settings without it.
TEST_CASE("color settings: the chain keys ignore Tube Color unless it is active", "[color]") {
    Settings off;
    Settings edited = off;
    edited.color.preset = ColorPreset::Set845;
    edited.color.drive = 0.3;
    edited.color.feedback_db = 0;
    const daphne::DeviceFormat dev{};
    // disabled: editing the fields keeps both keys
    CHECK(daphne::DspChain::structure_key(edited) == daphne::DspChain::structure_key(off));
    CHECK(daphne::playback::TrackQueue::chain_key(edited, dev) == daphne::playback::TrackQueue::chain_key(off, dev));
    Settings on = off;
    on.color.enabled = true;
    Settings on_edited = on;
    on_edited.color.drive = 0.3;
    on_edited.color.preset = ColorPreset::Set2a3;
    if (daphne::color::available()) {
        // a test build: in place, the drive and the makeup gain; a new chain (rebuilt and crossfaded), what the curve
        // and the response are made from
        CHECK(daphne::color_active(on));
        CHECK(daphne::DspChain::structure_key(on) != daphne::DspChain::structure_key(off));
        auto key = [](const Settings& x) { return daphne::DspChain::structure_key(x); };
        Settings v = on;
        v.color.drive = 1.7;
        v.color.makeup_gain_db = -2.0;
        CHECK(key(v) == key(on));
        CHECK(key(on_edited) != key(on));   // another preset
        v = on;
        v.color.feedback_db = 20;   // feedback: only the push-pull preset has it
        CHECK(key(v) == key(on));
        v.color.preset = ColorPreset::PpEl34Ul;
        Settings w = v;
        w.color.feedback_db = 0;
        CHECK(key(w) != key(v));
        v = on;
        v.color.bias = ColorBias::Cool;
        CHECK(key(v) != key(on));
        v = on;
        v.color.bias_ma = 500.0;   // outside set-300b's range: the standard bias applies, as with none
        CHECK(key(v) == key(on));
        v = on;
        v.color.load_ohms = 4;
        CHECK(key(v) != key(on));
    } else {
        // a release build: enabled true is the same as off
        CHECK_FALSE(daphne::color_active(on));
        CHECK(daphne::DspChain::structure_key(on) == daphne::DspChain::structure_key(off));
        CHECK(daphne::playback::TrackQueue::chain_key(on_edited, dev) == daphne::playback::TrackQueue::chain_key(off, dev));
    }
}

namespace {

daphne::AudioInfo src_info(int rate, int bits) {
    daphne::AudioInfo a;
    a.rate = rate;
    a.channels = 2;
    a.bits = bits;
    a.lossless_int = bits > 0;
    a.codec = "test";
    return a;
}

daphne::DeviceFormat dev_fmt(int rate, daphne::SampleType t, int bits) {
    daphne::DeviceFormat f;
    f.rate = rate;
    f.channels = 2;
    f.type = t;
    f.container_bits = t == daphne::SampleType::Float64 ? 64 : 32;
    f.valid_bits = bits;
    return f;
}

std::vector<std::string> all_stages(const Settings& s, const daphne::AudioInfo& a, const daphne::DeviceFormat& d) {
    daphne::DspChain c;
    std::string e;
    REQUIRE(c.build(s, a, d, 4096, &e));
    return c.all_stages();
}

// where `name` is among the stages (-1: absent)
int at(const std::vector<std::string>& v, const std::string& name) {
    const auto it = std::find(v.begin(), v.end(), name);
    return it == v.end() ? -1 : static_cast<int>(it - v.begin());
}

}  // namespace

TEST_CASE("color stage: where it sits in the chain, and only while active", "[color][chain]") {
    using daphne::SampleType;
    Settings off;
    off.volume.mode = daphne::VolumeMode::Digital;
    off.volume.db = -3.0;
    off.eq.enabled = true;
    off.eq.bands = {{daphne::EqType::PK, 1000.0, 1.0, 3.0, true}};
    Settings on = off;
    on.color.enabled = true;
    Settings dsd_core;
    dsd_core.core = daphne::CoreMode::Dsd;
    Settings dsd_core_on = dsd_core;
    dsd_core_on.color.enabled = true;
    daphne::AudioInfo dsd_file = src_info(176400, 0);
    dsd_file.dsd_pcm = true;
    Settings file_on;
    file_on.color.enabled = true;
    const auto pcm = dev_fmt(176400, SampleType::Int32, 32), same_rate = dev_fmt(44100, SampleType::Int32, 32),
               dsd_dev = dev_fmt(705600, SampleType::Float64, 64);
    // off: never in the chain, in any build
    for (const auto& v : {all_stages(off, src_info(44100, 24), pcm), all_stages(dsd_core, src_info(44100, 16), dsd_dev),
                          all_stages(Settings{}, dsd_file, pcm)})
        CHECK(at(v, "tube color") < 0);
    if (!daphne::color_active(on)) {
        // a release build: enabled true builds the chain without it
        CHECK(all_stages(on, src_info(44100, 24), pcm) == all_stages(off, src_info(44100, 24), pcm));
        CHECK(all_stages(dsd_core_on, src_info(44100, 16), dsd_dev) == all_stages(dsd_core, src_info(44100, 16), dsd_dev));
        CHECK(all_stages(file_on, dsd_file, pcm) == all_stages(Settings{}, dsd_file, pcm));
        return;
    }
    // PCM core, upsampled: after the resampler (and volume / EQ), before the final quantizer
    auto v = all_stages(on, src_info(44100, 24), pcm);
    INFO(nlohmann::json(v).dump());
    REQUIRE(at(v, "tube color") >= 0);
    CHECK(at(v, "tube color") > at(v, "resampler"));
    CHECK(at(v, "tube color") > at(v, "eq"));
    CHECK(at(v, "tube color") < at(v, "quantizer"));
    // at the source rate (no resampler)
    v = all_stages(on, src_info(44100, 24), same_rate);
    REQUIRE(at(v, "tube color") >= 0);
    CHECK(at(v, "resampler") < 0);
    CHECK(at(v, "tube color") < at(v, "quantizer"));
    // DSD core: after the modulator pre-filter, before the modulator limiter
    v = all_stages(dsd_core_on, src_info(44100, 16), dsd_dev);
    INFO(nlohmann::json(v).dump());
    REQUIRE(at(v, "tube color") >= 0);
    CHECK(at(v, "tube color") == at(v, "modulator pre-filter") + 1);
    CHECK(at(v, "modulator limiter") == at(v, "tube color") + 1);
    // a DSD file converted to PCM: before the DSD limiter
    v = all_stages(file_on, dsd_file, pcm);
    REQUIRE(at(v, "tube color") >= 0);
    CHECK(at(v, "tube color") < at(v, "dsd limiter"));
    // otherwise the chain is the one without it
    auto without = [](std::vector<std::string> x) {
        x.erase(std::remove(x.begin(), x.end(), "tube color"), x.end());
        return x;
    };
    CHECK(without(all_stages(on, src_info(44100, 24), pcm)) == all_stages(off, src_info(44100, 24), pcm));
    CHECK(without(all_stages(dsd_core_on, src_info(44100, 16), dsd_dev)) == all_stages(dsd_core, src_info(44100, 16), dsd_dev));
}

// The upsampling filter's cache key is the filter's own (taps, phase, apodizing, rates): Tube Color, on or off, at any
// drive or preset, never changes it, so a chain rebuilt for it finds the filter it played with (doc/N28).
TEST_CASE("color settings: the filter cache keys do not depend on Tube Color", "[color]") {
    Settings off;
    off.filter.taps = 65536;
    off.output.rate = "88200";
    for (bool enabled : {true, false})
        for (double drive : {0.0, 0.5, 2.0})
            for (auto preset : {ColorPreset::Set2a3, ColorPreset::PpEl34Ul}) {
                Settings s = off;
                s.color.enabled = enabled;
                s.color.drive = drive;
                s.color.preset = preset;
                s.color.feedback_db = 12;
                CHECK(daphne::filter_cache_keys_in_use(s, 44100) == daphne::filter_cache_keys_in_use(off, 44100));
                CHECK(daphne::filter_cache_key(s.filter, 44100, 88200) == daphne::filter_cache_key(off.filter, 44100, 88200));
            }
    // built once without it (designed, stored), the chain with it finds the filter in the cache
    const auto dir = test::temp_path("daphne_color_filter_cache");
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    const auto before = daphne::platform::get_env("DAPHNE_FILTER_CACHE");   // (the player tests turn it off: put back)
    test::set_env("DAPHNE_FILTER_CACHE", &dir);
    daphne::AudioInfo src;
    src.rate = 44100, src.channels = 2, src.bits = 24, src.lossless_int = true, src.codec = "test";
    daphne::DeviceFormat dev;
    dev.rate = 88200, dev.channels = 2, dev.type = daphne::SampleType::Int32, dev.container_bits = dev.valid_bits = 32;
    Settings small = off;
    small.filter.taps = 1024;
    CHECK_FALSE(daphne::filter_cached(small.filter, 44100, 88200));
    daphne::DspChain a;
    std::string e;
    REQUIRE(a.build(small, src, dev, 4096, &e));
    Settings with = small;
    with.color.enabled = true;
    with.color.drive = 0.7;
    CHECK(daphne::filter_cached(with.filter, 44100, 88200));
    if (before) {
        const auto p = daphne::platform::path_from_utf8(*before);
        test::set_env("DAPHNE_FILTER_CACHE", &p);
    } else {
        test::set_env("DAPHNE_FILTER_CACHE", nullptr);
    }
    std::filesystem::remove_all(dir, ec);
}

// Random color objects (doc/N28): every field is left out, valid, of the wrong type, out of range or extreme, at random.
// Each valid field must come through as given, each invalid one must be an error at its own path and keep its default,
// and nothing else in the settings may change. A fixed seed (the same rounds on every platform: the generator's own
// arithmetic only, no std::uniform_*_distribution); DAPHNE_COLOR_PROPERTY_SEED and DAPHNE_COLOR_PROPERTY_ROUNDS run
// other or more rounds locally. A failure prints the seed, the round and the object.
TEST_CASE("color settings: random objects, every invalid field falls back alone", "[color]") {
    const auto seed_env = daphne::platform::get_env("DAPHNE_COLOR_PROPERTY_SEED");
    const auto rounds_env = daphne::platform::get_env("DAPHNE_COLOR_PROPERTY_ROUNDS");
    const uint64_t seed = seed_env ? std::strtoull(seed_env->c_str(), nullptr, 0) : 20261121u;
    const int rounds = rounds_env ? std::atoi(rounds_env->c_str()) : 3000;
    using nlohmann::json;
    const double nan = std::numeric_limits<double>::quiet_NaN(), inf = std::numeric_limits<double>::infinity();
    // values of every type that no field accepts (each field adds its own wrong ones)
    const std::vector<json> foreign = {nullptr, json::array(), json::object(), json::array({1, 2}), json::object({{"x", 1}}),
                                       "", std::string(5000, 'x'), "\xc3\xa9", nan, inf, -inf, 1e308, -1e308,
                                       std::numeric_limits<int64_t>::min(), std::numeric_limits<uint64_t>::max()};
    struct Field {
        const char* key;
        std::vector<json> valid, invalid;
    };
    std::vector<Field> fields = {
        {"enabled", {true, false}, {0, 1, "true", "false", 5e-324}},
        {"model", {"fixed-point"}, {"circuit", "Fixed-Point", "fixed-point ", 0, true, 5e-324}},
        {"preset", {"set-2a3", "set-300b", "set-845", "pp-el34-ul"}, {"set-300B", "300b", "pp-el34", 1, false, 5e-324}},
        {"drive", {0, 0.0, 1, 0.01, 1.35, daphne::kColorDriveMax, 5e-324},
         {-0.01, -1, daphne::kColorDriveMax + 1e-9, 3, 400, "1.0", true}},
        {"feedback_db", {0, 6, 12, 20, 30}, {6.0, 7, -6, 31, 2147483654LL, "6", false, 5e-324}},
        {"bias", {"cool", "standard", "hot", 35.0, 59.8, 72.5, 100, 36}, {34.99, 100.01, 0, -60, "Hot", "warm", true, 5e-324}},
        {"load_ohms", {4, 8, 16}, {8.0, 0, 2, 32, -8, "8", true, 5e-324}},
        {"makeup_gain", {"auto", 0, -6, 6, 1.5, -0.001, 5e-324}, {6.0001, -7, 12, "AUTO", "3", false}},
    };
    for (auto& f : fields) f.invalid.insert(f.invalid.end(), foreign.begin(), foreign.end());
    const auto defaults = daphne::Settings{};
    const auto default_json = daphne::settings_to_json(defaults);

    for (int round = 0; round < rounds; ++round) {
        // splitmix64 per round: a failing round is reproduced from the seed alone
        uint64_t st = seed + 0x9E3779B97F4A7C15ull * static_cast<uint64_t>(round + 1);
        auto next = [&st]() {
            uint64_t z = (st += 0x9E3779B97F4A7C15ull);
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            return z ^ (z >> 31);
        };
        auto pick = [&next](size_t n) { return static_cast<size_t>(next() % n); };
        json obj = json::object();
        std::vector<std::string> bad;
        for (const auto& f : fields) {
            switch (pick(3)) {
                case 0: break;   // left out
                case 1: obj[f.key] = f.valid[pick(f.valid.size())]; break;
                default:
                    obj[f.key] = f.invalid[pick(f.invalid.size())];
                    bad.push_back(std::string("color.") + f.key);
            }
        }
        if (pick(4) == 0) obj["warmth"] = 3;   // an unknown field: a warning only
        // what must come out: the defaults, with every valid field as given
        daphne::ColorSettings want;
        for (const auto& [k, v] : obj.items()) {
            if (std::find(bad.begin(), bad.end(), "color." + k) != bad.end()) continue;
            if (k == "enabled") want.enabled = v.get<bool>();
            if (k == "preset")
                want.preset = v == "set-2a3" ? ColorPreset::Set2a3 : v == "set-845" ? ColorPreset::Set845
                              : v == "pp-el34-ul" ? ColorPreset::PpEl34Ul : ColorPreset::Set300b;
            if (k == "drive") want.drive = v.get<double>();
            if (k == "feedback_db") want.feedback_db = v.get<int>();
            if (k == "bias") {
                if (v.is_string()) want.bias = v == "cool" ? ColorBias::Cool : v == "hot" ? ColorBias::Hot : ColorBias::Standard;
                else want.bias_ma = v.get<double>();
            }
            if (k == "load_ohms") want.load_ohms = v.get<int>();
            if (k == "makeup_gain" && v.is_number()) want.makeup_gain_db = v.get<double>();
        }
        // the object as given (NaN and infinity included), and as text (where they become null: invalid as well)
        for (int as_text = 0; as_text < 2; ++as_text) {
            const json whole = {{"color", obj}};
            const auto r = as_text ? daphne::settings_from_string(whole.dump(-1, ' ', false, json::error_handler_t::replace))
                                   : daphne::settings_from_json(whole);
            const auto& c = r.settings.color;
            std::vector<std::string> errors;
            for (const auto& d : r.diagnostics)
                if (d.level == daphne::Diagnostic::Level::Error) errors.push_back(d.path);
            std::sort(errors.begin(), errors.end());
            auto expected = bad;
            std::sort(expected.begin(), expected.end());
            auto same = [](const std::optional<double>& a, const std::optional<double>& b) {
                return a.has_value() == b.has_value() && (!a || *a == *b);
            };
            auto rest = daphne::settings_to_json(r.settings);
            rest.erase("color");
            auto rest_default = default_json;
            rest_default.erase("color");
            const bool ok = errors == expected && c.enabled == want.enabled && c.model == want.model &&
                            c.preset == want.preset && c.drive == want.drive && c.feedback_db == want.feedback_db &&
                            c.bias == want.bias && same(c.bias_ma, want.bias_ma) && c.load_ohms == want.load_ohms &&
                            same(c.makeup_gain_db, want.makeup_gain_db) && rest == rest_default;
            if (!ok) {
                std::string got_errors;
                for (const auto& e : errors) got_errors += " " + e;
                FAIL("seed " << seed << " round " << round << (as_text ? " (as text)" : " (as json)") << "\n  color (NaN and infinity print as null): "
                             << obj.dump(-1, ' ', false, json::error_handler_t::replace) << "\n  errors:" << got_errors
                             << "\n  read back: " << daphne::settings_to_json(r.settings)["color"].dump());
            }
        }
    }
    SUCCEED("seed " << seed << ", " << rounds << " rounds");
}
