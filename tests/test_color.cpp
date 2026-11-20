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
                          Case{R"({"model": "spice"})", "color.model"}, Case{R"({"bias": "warm"})", "color.bias"},
                          Case{R"({"bias": 0})", "color.bias"}, Case{R"({"bias": 501})", "color.bias"},
                          Case{R"({"load_ohms": 6})", "color.load_ohms"}, Case{R"({"makeup_gain": 30})", "color.makeup_gain"},
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
