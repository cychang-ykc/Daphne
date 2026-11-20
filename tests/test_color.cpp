// Tube Color (doc/N28), M1: settings and the gate that keeps it out of the chain where it cannot run.
#include <catch_amalgamated.hpp>

#include "daphne/engine/chain.h"
#include "daphne/engine/settings.h"
#include "daphne/color/tube_color.h"
#include "../core/src/player/track_queue.h"

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
    REQUIRE(daphne::color_bias_ma(c).has_value());
    CHECK(*daphne::color_bias_ma(c) == 80.0);   // 300B: 80 mA
    CHECK_FALSE(daphne::color_bias_falls_back(c));
    c.preset = ColorPreset::Set845;              // switched: outside the range, the standard bias, reported
    CHECK_FALSE(daphne::color_bias_ma(c).has_value());
    CHECK(c.bias == ColorBias::Standard);
    CHECK(daphne::color_bias_falls_back(c));
    REQUIRE(c.bias_ma.has_value());
    CHECK(*c.bias_ma == 80.0);                   // the setting itself is kept
    c.preset = ColorPreset::Set300b;             // switched back: 80 mA again
    REQUIRE(daphne::color_bias_ma(c).has_value());
    CHECK(*daphne::color_bias_ma(c) == 80.0);
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
        // a test build: enabled is part of the structure, the other fields are not
        CHECK(daphne::color_active(on));
        CHECK(daphne::DspChain::structure_key(on) != daphne::DspChain::structure_key(off));
        CHECK(daphne::DspChain::structure_key(on_edited) == daphne::DspChain::structure_key(on));
    } else {
        // a release build: enabled true is the same as off
        CHECK_FALSE(daphne::color_active(on));
        CHECK(daphne::DspChain::structure_key(on) == daphne::DspChain::structure_key(off));
        CHECK(daphne::playback::TrackQueue::chain_key(on_edited, dev) == daphne::playback::TrackQueue::chain_key(off, dev));
    }
}
