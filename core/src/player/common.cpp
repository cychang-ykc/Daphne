#include "common.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <mutex>
#include <set>

#include "daphne/engine/dsd_output.h"

namespace daphne::playback {

int requested_rate(const Settings& s, int fs_in) {
    if (s.core == CoreMode::Dsd) return dop_rate(dsd_clock(s.modulator.osr));
    return pcm_output_rate(fs_in, s.output.rate);
}

bool same_output(const Settings& cs, int rate, int chain_rate) {
    if (cs.core == CoreMode::Dsd) return modulator_input_rate(rate) == chain_rate;
    for (int r = pcm_output_rate(rate, cs.output.rate); r >= rate && r % rate == 0; r /= 2)
        if (r == chain_rate) return true;
    return false;
}

std::string modulator_key(const Settings& s) {
    if (s.core != CoreMode::Dsd) return "";
    return settings_to_json(s)["dsd_modulator"].dump() + std::to_string(static_cast<int>(s.output.dsd)) + "m" +
           std::to_string(s.output.max_dsd);
}

Settings chain_settings(const Settings& s, int osr, int pcm_rate) {
    Settings c = s;
    if (osr > 0) c.modulator.osr = osr;
    if (pcm_rate > 0) {
        c.core = CoreMode::Pcm;
        c.output.rate = std::to_string(pcm_rate);
    }
    return c;
}

namespace output_memo {
namespace {
std::mutex g_m;
std::map<std::string, std::map<int, int>> g_dop_bad;   // DoP rate -> the rate locked instead (0: none)
std::set<std::pair<std::string, std::string>> g_shown;
int family(int rate) { return rate % 44100 == 0 ? 44100 : 48000; }
}  // namespace

std::string key(const OutputChoice& out, const std::string& probed_name) {
    return std::to_string(static_cast<int>(out.api)) + "|" + (probed_name.empty() ? out.device : probed_name);
}

void dop_failed(const std::string& key, int rate, int locked) {
    std::lock_guard<std::mutex> l(g_m);
    int& v = g_dop_bad[key][rate];
    if (locked > 0) v = locked;
}

bool dop_known_bad(const std::string& key, int rate) {
    std::lock_guard<std::mutex> l(g_m);
    auto it = g_dop_bad.find(key);
    if (it == g_dop_bad.end()) return false;
    for (const auto& [r, locked] : it->second)
        if (family(r) == family(rate) && r <= rate) return true;
    return false;
}

int locked_instead(const std::string& key, int rate) {
    std::lock_guard<std::mutex> l(g_m);
    auto it = g_dop_bad.find(key);
    if (it == g_dop_bad.end()) return 0;
    int best = 0;
    for (const auto& [r, locked] : it->second)
        if (locked > 0 && family(locked) == family(rate) && locked < rate) best = std::max(best, locked);
    return best;
}

bool first(const std::string& key, const std::string& what) {
    std::lock_guard<std::mutex> l(g_m);
    return g_shown.insert({key, what}).second;
}

void clear() {
    std::lock_guard<std::mutex> l(g_m);
    g_dop_bad.clear();
    g_shown.clear();
}
}  // namespace output_memo

std::string describe_filter(const Settings& x) {
    static const char* phases[] = {"linear", "intermediate", "minimum", "custom"};
    char b[96];
    std::snprintf(b, sizeof b, "%d taps, %s phase (alpha %.2f)", x.filter.taps, phases[static_cast<int>(x.filter.phase)],
                  x.filter.phase_alpha());
    std::string d = b;
    if (x.filter.apodize) d += ", apodize";
    if (x.filter.double_double) d += ", double-double convolution";
    if (x.core == CoreMode::Dsd)
        d = "DSD modulator order " + std::to_string(x.modulator.order) + ", DSD" + std::to_string(x.modulator.osr) + ", pre-filter " + d;
    return d;
}

std::string join(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& x : v) s += (s.empty() ? "" : " -> ") + x;
    return s.empty() ? "none" : s;
}

}  // namespace daphne::playback
