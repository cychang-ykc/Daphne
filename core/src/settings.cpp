#include "daphne/engine/settings.h"
#include "daphne/engine/track_levels.h"

#if defined(DAPHNE_COLOR)
#include "daphne/color/curves.h"
#include "daphne/color/tube_color.h"
#endif

#include <algorithm>
#include <cstdlib>
#include <initializer_list>
#include <utility>

namespace daphne {
namespace {

using nlohmann::json;

template <class E>
using EnumTable = std::initializer_list<std::pair<const char*, E>>;

const EnumTable<OutputApi> kOutputApi = {
    {"wasapi_exclusive", OutputApi::WasapiExclusive}, {"wasapi_shared", OutputApi::WasapiShared}, {"asio", OutputApi::Asio},
    {"coreaudio_exclusive", OutputApi::CoreAudioExclusive}, {"coreaudio_shared", OutputApi::CoreAudioShared}};
const EnumTable<CoreMode> kCoreMode = {{"pcm", CoreMode::Pcm}, {"dsd", CoreMode::Dsd}};
const EnumTable<DsdMode> kDsdMode = {{"auto", DsdMode::Auto}, {"native", DsdMode::Native}, {"dop", DsdMode::Dop}, {"pcm", DsdMode::Pcm}};
const EnumTable<FilterPhase> kPhase = {{"linear", FilterPhase::Linear}, {"intermediate", FilterPhase::Intermediate},
                                       {"minimum", FilterPhase::Minimum}, {"custom", FilterPhase::Custom}};
const EnumTable<EqType> kEqType = {{"PK", EqType::PK},       {"LS", EqType::LS},       {"HS", EqType::HS},
                                   {"HP", EqType::HP},       {"HP24", EqType::HP24},   {"LP", EqType::LP},
                                   {"LP24", EqType::LP24},   {"NOTCH", EqType::NOTCH}, {"AP", EqType::AP},
                                   {"BP", EqType::BP}};
const EnumTable<VolumeMode> kVolumeMode = {
    {"digital", VolumeMode::Digital}, {"fixed", VolumeMode::Fixed}};
const EnumTable<ColorModel> kColorModel = {{"fixed-point", ColorModel::FixedPoint}};
const EnumTable<ColorPreset> kColorPreset = {{"set-2a3", ColorPreset::Set2a3}, {"set-300b", ColorPreset::Set300b},
                                             {"set-845", ColorPreset::Set845}, {"pp-el34-ul", ColorPreset::PpEl34Ul},
                                             {"set-300b-x", ColorPreset::Set300bX}, {"h2-pure", ColorPreset::H2Pure}};
const EnumTable<ColorBias> kColorBias = {{"cool", ColorBias::Cool}, {"standard", ColorBias::Standard}, {"hot", ColorBias::Hot}};
const EnumTable<ColorLoad> kColorLoad = {{"resistor-8", ColorLoad::Resistor8}, {"bookshelf-vented", ColorLoad::BookshelfVented},
                                         {"bookshelf-sealed", ColorLoad::BookshelfSealed},
                                         {"bookshelf-vented-smooth", ColorLoad::BookshelfVentedSmooth},
                                         {"bookshelf-sealed-smooth", ColorLoad::BookshelfSealedSmooth}};
const EnumTable<ColorAlign> kColorAlign = {{"off", ColorAlign::Off}, {"loudness", ColorAlign::Loudness}, {"near-peak", ColorAlign::NearPeak},
                                           {"conservative", ColorAlign::Conservative}};

template <class E>
const char* to_name(const EnumTable<E>& t, E v) {
    for (const auto& [name, e] : t)
        if (e == v) return name;
    return "?";
}

std::string join(const std::string& path, const std::string& key) { return path.empty() ? key : path + "." + key; }

class Reader {
public:
    explicit Reader(std::vector<Diagnostic>& d) : d_(d) {}

    void error(const std::string& path, const std::string& msg) { d_.push_back({Diagnostic::Level::Error, path, msg}); }
    void warn(const std::string& path, const std::string& msg) { d_.push_back({Diagnostic::Level::Warning, path, msg}); }

    // Warn about keys this version does not know (they are ignored).
    void check_keys(const json& o, const std::string& path, std::initializer_list<const char*> known) {
        for (const auto& item : o.items()) {
            // a named reference, not a structured binding: lambdas capture bindings only from clang 16 (Apple clang 15, Xcode 15)
            const std::string& k = item.key();
            if (std::none_of(known.begin(), known.end(), [&](const char* n) { return k == n; }))
                warn(join(path, k), "unknown field (ignored)");
        }
    }

    // Child object, or nullptr when missing. Wrong type is an error.
    const json* object(const json& o, const char* key, const std::string& path) {
        auto it = o.find(key);
        if (it == o.end() || it->is_null()) return nullptr;
        if (!it->is_object()) {
            error(join(path, key), "must be an object");
            return nullptr;
        }
        return &*it;
    }

    void number(const json& o, const char* key, const std::string& path, double& out, double lo, double hi) {
        auto it = o.find(key);
        if (it == o.end()) return;
        if (!it->is_number()) return error(join(path, key), "must be a number");
        const double v = it->get<double>();
        if (!(v >= lo && v <= hi)) return error(join(path, key), range_msg(lo, hi));   // NaN too
        out = v;
    }

    void integer(const json& o, const char* key, const std::string& path, int& out, int lo, int hi,
                 std::initializer_list<int> allowed = {}) {
        auto it = o.find(key);
        if (it == o.end()) return;
        if (!it->is_number_integer()) return error(join(path, key), "must be an integer");
        const long long v = it->get<long long>();
        if (allowed.size() > 0) {
            if (std::find(allowed.begin(), allowed.end(), v) == allowed.end()) {
                std::string msg = "must be one of";
                for (int a : allowed) msg += " " + std::to_string(a);
                return error(join(path, key), msg);
            }
        } else if (v < lo || v > hi) {
            return error(join(path, key), range_msg(lo, hi));
        }
        out = static_cast<int>(v);
    }

    void boolean(const json& o, const char* key, const std::string& path, bool& out) {
        auto it = o.find(key);
        if (it == o.end()) return;
        if (!it->is_boolean()) return error(join(path, key), "must be true or false");
        out = it->get<bool>();
    }

    void string(const json& o, const char* key, const std::string& path, std::string& out) {
        auto it = o.find(key);
        if (it == o.end()) return;
        if (!it->is_string()) return error(join(path, key), "must be a string");
        out = it->get<std::string>();
    }

    template <class E>
    void enumeration(const json& o, const char* key, const std::string& path, E& out, const EnumTable<E>& t) {
        auto it = o.find(key);
        if (it == o.end()) return;
        if (it->is_string()) {
            const auto s = it->get<std::string>();
            for (const auto& [name, e] : t)
                if (s == name) {
                    out = e;
                    return;
                }
        }
        std::string msg = "must be one of";
        for (const auto& [name, e] : t) msg += std::string(" \"") + name + "\"";
        error(join(path, key), msg);
    }

    // "auto" or a number in [lo, hi]; "auto" -> nullopt.
    void auto_or_number(const json& o, const char* key, const std::string& path, std::optional<double>& out, double lo,
                        double hi) {
        auto it = o.find(key);
        if (it == o.end()) return;
        if (it->is_string() && it->get<std::string>() == "auto") {
            out.reset();
            return;
        }
        if (!it->is_number()) return error(join(path, key), "must be \"auto\" or a number");
        const double v = it->get<double>();
        if (!(v >= lo && v <= hi)) return error(join(path, key), range_msg(lo, hi));   // NaN too
        out = v;
    }

    static std::string number_text(double x) {
        std::string s = std::to_string(x);
        s.erase(s.find_last_not_of('0') + 1);
        if (!s.empty() && s.back() == '.') s.pop_back();
        return s;
    }

private:
    static std::string range_msg(double lo, double hi) { return "must be between " + number_text(lo) + " and " + number_text(hi); }
    std::vector<Diagnostic>& d_;
};

// "native" -> 0, "auto" -> -1, "2x" -> -3, "<Hz>" -> Hz; -2 = invalid.
long rate_value(const std::string& r) {
    if (r == "native") return 0;
    if (r == "auto") return -1;
    if (r == "2x") return -3;
    if (r.empty() || r.size() > 8 || !std::all_of(r.begin(), r.end(), [](char c) { return c >= '0' && c <= '9'; })) return -2;
    const long v = std::strtol(r.c_str(), nullptr, 10);
    return v >= 8000 && v <= 1536000 ? v : -2;
}

void read_output(Reader& r, const json& o, OutputSettings& s) {
    const std::string p = "output";
    r.check_keys(o, p, {"api", "device", "rate", "dsd", "dsd_pcm_lowpass", "dsd_pcm_limiter", "max_dsd", "dsd_lead_in_ms", "buffer_ms"});
    r.enumeration(o, "api", p, s.api, kOutputApi);
    r.string(o, "device", p, s.device);
    std::string rate = s.rate;
    r.string(o, "rate", p, rate);
    if (rate_value(rate) != -2) s.rate = rate;
    else r.error("output.rate", "must be \"native\", \"auto\", \"2x\" or a rate cap in Hz (\"8000\" .. \"1536000\")");
    r.enumeration(o, "dsd", p, s.dsd, kDsdMode);
    r.integer(o, "dsd_pcm_lowpass", p, s.dsd_pcm_lowpass, 0, 0, {24, 30, 50});
    r.boolean(o, "dsd_pcm_limiter", p, s.dsd_pcm_limiter);
    r.integer(o, "max_dsd", p, s.max_dsd, 0, 0, {0, 64, 128, 256, 512});
    r.integer(o, "dsd_lead_in_ms", p, s.dsd_lead_in_ms, 0, 2000);
    r.integer(o, "buffer_ms", p, s.buffer_ms, 0, 200);
}

void read_filter(Reader& r, const json& o, FilterSettings& s) {
    const std::string p = "filter";
    r.check_keys(o, p, {"taps", "phase", "phase_custom", "apodize", "double_double"});
    r.integer(o, "taps", p, s.taps, 16, 1 << 20);
    r.enumeration(o, "phase", p, s.phase, kPhase);
    r.number(o, "phase_custom", p, s.phase_custom, 0.0, 1.0);
    r.boolean(o, "apodize", p, s.apodize);
    r.boolean(o, "double_double", p, s.double_double);
}

void read_eq(Reader& r, const json& o, EqSettings& s) {
    const std::string p = "eq";
    r.check_keys(o, p, {"enabled", "preamp_db", "bands"});
    r.boolean(o, "enabled", p, s.enabled);
    r.auto_or_number(o, "preamp_db", p, s.preamp_db, -60.0, 24.0);
    auto it = o.find("bands");
    if (it == o.end()) return;
    if (!it->is_array()) return r.error("eq.bands", "must be an array");
    s.bands.clear();
    for (size_t i = 0; i < it->size(); ++i) {
        const std::string bp = "eq.bands[" + std::to_string(i) + "]";
        const json& b = (*it)[i];
        if (!b.is_object()) {
            r.error(bp, "must be an object");
            continue;
        }
        r.check_keys(b, bp, {"type", "f", "q", "gain_db", "on"});
        EqBand band;
        r.enumeration(b, "type", bp, band.type, kEqType);
        r.number(b, "f", bp, band.f, 1.0, 100000.0);
        r.number(b, "q", bp, band.q, 0.01, 100.0);
        r.number(b, "gain_db", bp, band.gain_db, -30.0, 30.0);
        r.boolean(b, "on", bp, band.on);
        s.bands.push_back(band);
    }
}

void read_color(Reader& r, const json& o, ColorSettings& s) {
    const std::string p = "color";
    r.check_keys(o, p, {"enabled", "model", "preset", "drive", "feedback_db", "bias", "load_ohms", "load", "align", "makeup_gain"});
    r.boolean(o, "enabled", p, s.enabled);
    r.enumeration(o, "model", p, s.model, kColorModel);
    r.enumeration(o, "preset", p, s.preset, kColorPreset);
    r.number(o, "drive", p, s.drive, 0.0, kColorDriveMax);
    r.integer(o, "feedback_db", p, s.feedback_db, 0, 0, {0, 6, 12, 20, 30});
    // "cool" | "standard" | "hot", or a cathode current in mA. A current no preset allows is an error; one outside the
    // current preset's range is kept (another preset may use it) with a warning, and the standard bias applies
    if (auto it = o.find("bias"); it != o.end()) {
        double lo = 1e9, hi = 0.0;
        for (const auto& [name, e] : kColorPreset) {
            lo = std::min(lo, color_bias_range(e).lo_ma);
            hi = std::max(hi, color_bias_range(e).hi_ma);
        }
        bool ok = false;
        if (it->is_number()) {
            const double ma = it->get<double>();
            if (ma >= lo && ma <= hi) s.bias_ma = ma, ok = true;
        } else if (it->is_string()) {
            for (const auto& [name, e] : kColorBias)
                if (it->get<std::string>() == name) s.bias = e, s.bias_ma.reset(), ok = true;
        }
        if (!ok) {
            r.error("color.bias", "must be \"cool\", \"standard\", \"hot\" or a cathode current in mA (" + Reader::number_text(lo) +
                                      " .. " + Reader::number_text(hi) + ", per preset)");
        } else if (color_bias_falls_back(s)) {
            const std::string preset = to_name(kColorPreset, s.preset);
            const auto range = color_bias_range(s.preset);
            r.warn("color.bias", (s.bias_ma ? "outside " + preset + "'s range (" + Reader::number_text(range.lo_ma) + " .. " +
                                                  Reader::number_text(range.hi_ma) + " mA)"
                                            : "hot does not apply to " + preset + " (no room above its standard bias)") +
                                     ": the standard bias applies; kept for the other presets");
        }
    }
    r.integer(o, "load_ohms", p, s.load_ohms, 0, 0, {4, 8, 16});
    r.enumeration(o, "load", p, s.load, kColorLoad);
    r.enumeration(o, "align", p, s.align, kColorAlign);
    r.auto_or_number(o, "makeup_gain", p, s.makeup_gain_db, -kColorMakeupTrimMaxDb, kColorMakeupTrimMaxDb);
}

json auto_or(const std::optional<double>& v) { return v ? json(*v) : json("auto"); }

}  // namespace

bool LoadResult::ok() const {
    return std::none_of(diagnostics.begin(), diagnostics.end(),
                        [](const Diagnostic& d) { return d.level == Diagnostic::Level::Error; });
}

LoadResult settings_from_json(const json& j) {
    LoadResult res;
    Reader r(res.diagnostics);
    if (!j.is_object()) {
        r.error("", "settings must be a JSON object");
        return res;
    }
    Settings& s = res.settings;
    r.check_keys(j, "", {"version", "output", "core", "filter", "eq", "crossfeed", "volume", "headroom_db", "dsd_modulator",
                         "design", "color"});
    int version = kSettingsVersion;
    r.integer(j, "version", "", version, 1, 1000);
    if (version > kSettingsVersion)
        r.warn("version", "newer than this build (" + std::to_string(kSettingsVersion) + "); unknown fields are ignored");
    s.version = kSettingsVersion;

    if (auto* o = r.object(j, "output", "")) read_output(r, *o, s.output);
    r.enumeration(j, "core", "", s.core, kCoreMode);
    if (auto* o = r.object(j, "filter", "")) read_filter(r, *o, s.filter);
    if (auto* o = r.object(j, "eq", "")) read_eq(r, *o, s.eq);
    if (auto* o = r.object(j, "crossfeed", "")) {
        r.check_keys(*o, "crossfeed", {"enabled", "fc", "amount"});
        r.boolean(*o, "enabled", "crossfeed", s.crossfeed.enabled);
        r.number(*o, "fc", "crossfeed", s.crossfeed.fc, 100.0, 3000.0);
        r.number(*o, "amount", "crossfeed", s.crossfeed.amount, 0.0, 1.0);
    }
    if (auto* o = r.object(j, "volume", "")) {
        r.check_keys(*o, "volume", {"mode", "db"});
        r.enumeration(*o, "mode", "volume", s.volume.mode, kVolumeMode);
        r.number(*o, "db", "volume", s.volume.db, -120.0, 0.0);
    }
    r.number(j, "headroom_db", "", s.headroom_db, -24.0, 24.0);
    if (auto* o = r.object(j, "dsd_modulator", "")) {
        r.check_keys(*o, "dsd_modulator", {"order", "osr"});
        r.integer(*o, "order", "dsd_modulator", s.modulator.order, 0, 0, {5, 8});
        r.integer(*o, "osr", "dsd_modulator", s.modulator.osr, 0, 0, {64, 128, 256, 512, 1024});
    }
    if (auto* o = r.object(j, "design", "")) {
        r.check_keys(*o, "design", {"threads", "cache_limit_mb"});
        r.integer(*o, "threads", "design", s.design.threads, 0, 256);
        r.integer(*o, "cache_limit_mb", "design", s.design.cache_limit_mb, 0, 1 << 20);
    }
    if (auto* o = r.object(j, "color", "")) read_color(r, *o, s.color);
    return res;
}

LoadResult settings_from_string(const std::string& text) {
    json j;
    try {
        j = json::parse(text);
    } catch (const json::exception& e) {   // a syntax error, or a number past double's range (1e400: out_of_range)
        LoadResult res;
        res.diagnostics.push_back({Diagnostic::Level::Error, "", std::string("JSON parse error: ") + e.what()});
        return res;
    }
    return settings_from_json(j);
}

json settings_to_json(const Settings& s) {
    json bands = json::array();
    for (const auto& b : s.eq.bands)
        bands.push_back({{"type", to_name(kEqType, b.type)}, {"f", b.f}, {"q", b.q}, {"gain_db", b.gain_db}, {"on", b.on}});
    return {
        {"version", kSettingsVersion},
        {"output",
         {{"api", to_name(kOutputApi, s.output.api)},
          {"device", s.output.device},
          {"rate", s.output.rate},
          {"dsd", to_name(kDsdMode, s.output.dsd)},
          {"dsd_pcm_lowpass", s.output.dsd_pcm_lowpass},
          {"dsd_pcm_limiter", s.output.dsd_pcm_limiter},
          {"max_dsd", s.output.max_dsd},
          {"dsd_lead_in_ms", s.output.dsd_lead_in_ms},
          {"buffer_ms", s.output.buffer_ms}}},
        {"core", to_name(kCoreMode, s.core)},
        {"filter",
         {{"taps", s.filter.taps},
          {"phase", to_name(kPhase, s.filter.phase)},
          {"phase_custom", s.filter.phase_custom},
          {"apodize", s.filter.apodize},
          {"double_double", s.filter.double_double}}},
        {"eq", {{"enabled", s.eq.enabled}, {"preamp_db", auto_or(s.eq.preamp_db)}, {"bands", bands}}},
        {"crossfeed", {{"enabled", s.crossfeed.enabled}, {"fc", s.crossfeed.fc}, {"amount", s.crossfeed.amount}}},
        {"volume", {{"mode", to_name(kVolumeMode, s.volume.mode)}, {"db", s.volume.db}}},
        {"headroom_db", s.headroom_db},
        {"dsd_modulator", {{"order", s.modulator.order}, {"osr", s.modulator.osr}}},
        {"design", {{"threads", s.design.threads}, {"cache_limit_mb", s.design.cache_limit_mb}}},
        {"color",
         {{"enabled", s.color.enabled},
          {"model", to_name(kColorModel, s.color.model)},
          {"preset", to_name(kColorPreset, s.color.preset)},
          {"drive", s.color.drive},
          {"feedback_db", s.color.feedback_db},
          {"bias", s.color.bias_ma ? json(*s.color.bias_ma) : json(to_name(kColorBias, s.color.bias))},
          {"load_ohms", s.color.load_ohms},
          {"load", to_name(kColorLoad, s.color.load)},
          {"align", to_name(kColorAlign, s.color.align)},
          {"makeup_gain", auto_or(s.color.makeup_gain_db)}}},
    };
}

const char* color_preset_name(ColorPreset p) { return to_name(kColorPreset, p); }
const char* color_load_name(ColorLoad l) { return to_name(kColorLoad, l); }
const char* color_align_name(ColorAlign a) { return to_name(kColorAlign, a); }

ColorBiasRange color_bias_range(ColorPreset p) {
    // doc/N28: the reference circuits' quiescent current (M0) and the datasheet ratings at their plate voltage
    // lo = 0.6 x standard; hi = the rating; hot_cap = 90 % of the rated plate dissipation (plus the screen current)
    switch (p) {
        case ColorPreset::Set2a3: return {35.0, 58.9, 59.0, 53.8};     // RCA: 15 W at 251 V
        case ColorPreset::Set300b:
        case ColorPreset::Set300bX:
        case ColorPreset::H2Pure: return {36.0, 59.8, 100.0, 92.5};   // WE: 100 mA maximum plate current; 36 W at 350 V (X: the same 300B stage)
        case ColorPreset::Set845: return {44.0, 72.5, 79.0, 71.8};     // RCA: 100 W at 1253 V
        case ColorPreset::PpEl34Ul: return {39.0, 65.1, 69.0, 63.0};   // Philips: 25 W at 399 V, plus 6.7 mA screen current
    }
    return {0.0, 0.0, 0.0, 0.0};
}

double color_cool_ma(ColorPreset p) { return kColorCoolFactor * color_bias_range(p).standard_ma; }

std::optional<double> color_hot_ma(ColorPreset p) {
    const auto r = color_bias_range(p);
    const double hot = std::min(kColorHotFactor * r.standard_ma, r.hot_cap_ma);
    if (hot < (1.0 + kColorHotMinStep) * r.standard_ma) return std::nullopt;
    return hot;
}

double color_bias_current_ma(const ColorSettings& c) {
    const auto r = color_bias_range(c.preset);
    if (c.bias_ma) return *c.bias_ma >= r.lo_ma && *c.bias_ma <= r.hi_ma ? *c.bias_ma : r.standard_ma;
    switch (c.bias) {
        case ColorBias::Cool: return color_cool_ma(c.preset);
        case ColorBias::Hot: return color_hot_ma(c.preset).value_or(r.standard_ma);
        case ColorBias::Standard: break;
    }
    return r.standard_ma;
}

bool color_bias_falls_back(const ColorSettings& c) {
    const auto r = color_bias_range(c.preset);
    if (c.bias_ma) return *c.bias_ma < r.lo_ma || *c.bias_ma > r.hi_ma;
    return c.bias == ColorBias::Hot && !color_hot_ma(c.preset);
}

bool color_active(const Settings& s) {
#if defined(DAPHNE_COLOR)
    return s.color.enabled && color::available(to_name(kColorPreset, s.color.preset));
#else
    (void)s;
    return false;
#endif
}

// the strength's H2 the panel shows (h2-pure), from its curve's data; null for the others
json h2_table(ColorPreset p) {
#if defined(DAPHNE_COLOR)
    if (const auto* c = color::preset_curve(to_name(kColorPreset, p)); c && c->h2) {
        json t = json::array();
        for (int i = 0; i < c->h2_n; ++i) t.push_back({c->h2[2 * i], c->h2[2 * i + 1]});
        return t;
    }
#else
    (void)p;
#endif
    return nullptr;
}

nlohmann::json color_info() {
    // the panel's order: the default first
    struct P {
        ColorPreset p;
        const char* label;
        bool push_pull, experimental;
    };
    const P presets[] = {{ColorPreset::Set300b, "300B single-ended", false, false},
                         {ColorPreset::Set300bX, "300B single-ended, CCS driver (experimental)", false, true},
                         {ColorPreset::H2Pure, "Pure H2 (experimental)", false, true},
                         {ColorPreset::Set2a3, "2A3 single-ended", false, false},
                         {ColorPreset::Set845, "845 single-ended", false, false},
                         {ColorPreset::PpEl34Ul, "EL34 push-pull (ultralinear)", true, false}};
    json list = json::array();
    for (const auto& e : presets) {
        const auto r = color_bias_range(e.p);
        const auto hot = color_hot_ma(e.p);
        list.push_back({{"id", to_name(kColorPreset, e.p)},
                        {"label", e.label},
#if defined(DAPHNE_COLOR)
                        {"available", color::available(to_name(kColorPreset, e.p))},
#else
                        {"available", false},
#endif
                        {"push_pull", e.push_pull},
                        {"experimental", e.experimental},
                        {"h2_table", h2_table(e.p)},
                        {"bias_ma",
                         {{"lo", r.lo_ma}, {"standard", r.standard_ma}, {"hi", r.hi_ma}, {"cool", color_cool_ma(e.p)},
                          {"hot", hot ? json(*hot) : json(nullptr)}}}});
    }
#if defined(DAPHNE_COLOR)
    const bool available = color::available_any();
#else
    const bool available = false;
#endif
#if defined(DAPHNE_COLOR)
    const char* curve = color::test_curve() ? "test" : "tubes";
#else
    const char* curve = "none";
#endif
    return {{"available", available},
            {"curve", available ? curve : "none"},
            {"drive_max", kColorDriveMax},
            {"makeup_trim_max_db", kColorMakeupTrimMaxDb},
            {"feedback_db", {0, 6, 12, 20, 30}},
            {"load_ohms", {4, 8, 16}},
            {"operating_point_adjustable", kColorOperatingPointAdjustable},
            {"loads", json::array({{{"id", "resistor-8"}, {"label", "8 ohm resistor"}},
                                   {{"id", "bookshelf-vented"}, {"label", "Bookshelf speaker, vented"}},
                                   {{"id", "bookshelf-sealed"}, {"label", "Bookshelf speaker, sealed"}},
                                   {{"id", "bookshelf-vented-smooth"}, {"label", "Bookshelf speaker, vented, smooth (its tilt, no resonance)"}},
                                   {{"id", "bookshelf-sealed-smooth"}, {"label", "Bookshelf speaker, sealed, smooth (its tilt, no resonance)"}}})},
            {"default_load", to_name(kColorLoad, ColorSettings{}.load)},
            // the level alignment (track_levels.h; experimental), and the references it aligns to
            {"aligns", json::array({{{"id", "off"}, {"label", "Off (as before)"}},
                                    {{"id", "loudness"}, {"label", "Loudness (integrated LUFS)"}},
                                    {{"id", "near-peak"}, {"label", "Near-peak (99.9th percentile)"}},
                                    {{"id", "conservative"}, {"label", "Conservative (the larger cut)"}}})},
            {"default_align", to_name(kColorAlign, ColorSettings{}.align)},
            {"align_refs", {{"lufs", kAlignRefLufs}, {"p999_db", kAlignRefP999Db}}},
            {"presets", list}};
}

Settings without_inactive_color(Settings s) {
    if (!color_active(s)) s.color = {};
    return s;
}

int pcm_output_rate(int fs_in, const std::string& rate) {
    const long t = rate_value(rate);
    if (t == -3) return 2L * fs_in <= 1536000 ? 2 * fs_in : fs_in;   // 2x: twice the source rate, for every track
    if (t <= 0 && t != -1) return fs_in;                             // native (or invalid)
    const long cap = t == -1 ? (fs_in % 44100 == 0 ? 705600 : 768000) : t;
    if (2L * fs_in > cap) return fs_in;
    int ratio = 1;
    while (ratio < 32 && static_cast<long>(ratio) * 2 * fs_in <= cap) ratio *= 2;
    return ratio * fs_in;
}

int modulator_input_rate(int fs_in) { return pcm_output_rate(fs_in, "auto"); }

}  // namespace daphne
