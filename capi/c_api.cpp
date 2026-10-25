#include "daphne_core.h"
#include "internal.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "daphne/engine/devices.h"
#include "daphne/engine/filters.h"
#include "daphne/engine/live_source.h"
#include "daphne/engine/player.h"
#include "daphne/engine/sample_format.h"
#include "daphne/engine/settings.h"
#include "daphne/engine/text.h"
#include "daphne/platform/platform.h"

using nlohmann::json;

// Calls that change what plays (load, stop, apply, destroy) take `op` and run one at a time; a load holds it until
// playback has started, which can take minutes (a first long filter design) or hang in a driver. Everything else
// (state, meter, pause, seek, set_next) only copies the player pointer under `pm`, never waits for `op`, and uses
// the player through that copy: the Player's own methods are safe from any thread (atomics, its own locks).
struct dc_player {
    std::mutex op;                       // load / stop / apply / destroy
    std::mutex pm;                       // the player pointers: held only to copy or swap them
    std::shared_ptr<daphne::Player> player;
    std::shared_ptr<daphne::Player> starting;   // the player dc_load is starting (dc_stop cancels it without op)
    bool stop_requested = false;                // dc_stop came during this dc_load (also before `starting` was set)
    std::atomic<bool> loading{false};    // dc_load is running (dc_state: "loading" while no player is published)
    std::mutex m;                        // events, report, state
    std::deque<std::string> events;
    daphne::PlayReport report;
    std::string error;
    int track = 0;
    std::atomic<double> design{1.0};
    bool failed = false;
};

namespace {

void put(char* dst, int len, const std::string& s) {
    if (!dst || len <= 0) return;
    size_t n = std::min(static_cast<size_t>(len - 1), s.size());
    // cut short: not in the middle of a UTF-8 character (a path, a device name), back to where one starts
    if (n < s.size())
        while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
    std::memcpy(dst, s.data(), n);
    dst[n] = 0;
}

int put_json(char* dst, int len, const std::string& s) {
    if (!dst || len <= static_cast<int>(s.size())) return DC_ERR_BUFFER;
    std::memcpy(dst, s.c_str(), s.size() + 1);
    return static_cast<int>(s.size());
}

bool parse(const char* text, daphne::Settings& s, std::string& msg) {
    const auto r = daphne::settings_from_string(text ? text : "{}");
    for (const auto& d : r.diagnostics)
        msg += std::string(d.level == daphne::Diagnostic::Level::Error ? "error " : "warning ") + d.path + ": " + d.message + "\n";
    s = r.settings;
    return r.ok();
}

json report_json(const daphne::PlayReport& r) {
    json src = {{"codec", r.source.codec}, {"rate", r.source.rate}, {"channels", r.source.channels}, {"bits", r.source.bits},
                {"dsd_rate", r.source.dsd_rate}, {"frames", r.source.frames}, {"lossless", r.source.lossless_int}};
    json dev = {{"rate", r.device.rate}, {"channels", r.device.channels}, {"type", daphne::sample_type_name(r.device.type)},
                {"bits", r.device.valid_bits}, {"container", r.device.container_bits}, {"text", daphne::describe(r.device)}};
    return {{"source", src},           {"device", dev},       {"device_name", r.device_name}, {"system_rate", r.system_rate}, {"bit_perfect", r.bit_perfect},
            {"reason", r.not_bit_perfect_reason}, {"filter", r.filter}, {"chain", r.chain},  {"latency_ms", r.latency_ms},
            {"underruns", r.underruns}, {"clipped", r.clipped}, {"realtime", r.realtime_factor}};
}

void push_event(dc_player* p, json e) {
    std::lock_guard<std::mutex> l(p->m);
    p->events.push_back(e.dump());
    if (p->events.size() > 256) p->events.pop_front();
}

// the player now (a copy: it stays valid while the caller uses it, whatever load / stop do meanwhile)
std::shared_ptr<daphne::Player> current(dc_player* p) {
    std::lock_guard<std::mutex> l(p->pm);
    return p->player;
}

// under op: the player is unpublished first (queries see none from now on), then stopped here. A query may still
// hold a copy for a moment; it is let go before the player is freed, so the freeing happens on this thread.
void stop_player(dc_player* p) {
    std::shared_ptr<daphne::Player> old;
    {
        std::lock_guard<std::mutex> l(p->pm);
        old.swap(p->player);
    }
    if (!old) return;
    old->stop();
    daphne::PlayReport r;
    std::string e;
    old->wait(&r, &e);
    while (old.use_count() > 1) std::this_thread::yield();
    old.reset();
}

// without op: the player dc_load is starting, if any, is told to stop; dc_load then returns DC_ERR_CANCELLED. A filter
// design and the prefill check for it; a device open that hangs in its driver cannot be interrupted.
void cancel_start(dc_player* p) {
    std::shared_ptr<daphne::Player> s;
    {
        std::lock_guard<std::mutex> l(p->pm);
        if (p->loading) p->stop_requested = true;
        s = p->starting;
    }
    if (s) s->stop();
}

// tests only (capi/internal.h): what the players are built with (empty: the platform's devices and decoders)
std::mutex g_deps_m;
daphne::PlayerDeps g_deps;

// A wchar_t path of the C API: UTF-16 on Windows (the OS's own), UTF-32 elsewhere.
std::filesystem::path path_from_wide(const wchar_t* w) {
    if (!w) return {};
#ifdef _WIN32
    return std::filesystem::path(std::wstring(w));
#else
    return daphne::platform::path_from_utf8(daphne::to_utf8(std::wstring(w)));
#endif
}

std::filesystem::path path_from_utf8(const char* s) { return s ? daphne::platform::path_from_utf8(s) : std::filesystem::path(); }

int load(dc_player* p, const std::filesystem::path& path, const char* settings_json, double seconds, int play, char* err, int err_len);

void set_next(dc_player* p, const std::filesystem::path& path, std::optional<double> headroom_db) {
    if (!p) return;
    if (const auto pl = current(p)) pl->set_next(path, headroom_db);
}

std::optional<double> headroom(double db) { return std::isnan(db) ? std::nullopt : std::optional<double>(db); }

}  // namespace

extern "C" {

// 2: the UTF-8 path functions (dc_load_utf8, dc_set_next_utf8, dc_set_next_gain_utf8, dc_filter_cache_dir_utf8)
// 3: dc_keep_device, dc_release_device, dc_device_kept
// 4: dc_cancel; dc_stop cancels a load in progress (DC_ERR_CANCELLED); dc_state "loading"
DC_API int dc_abi_version(void) { return 4; }
DC_API const char* dc_version(void) { return "daphne 0.1.0"; }

DC_API dc_player* dc_create(void) {
    try {
        return new dc_player();
    } catch (...) {
        return nullptr;
    }
}

DC_API void dc_destroy(dc_player* p) {
    if (!p) return;
    try {
        cancel_start(p);
        std::lock_guard<std::mutex> l(p->op);
        stop_player(p);
    } catch (...) {
    }
    delete p;
}

DC_API int dc_load(dc_player* p, const wchar_t* path, const char* settings_json, double seconds, int play, char* err, int err_len) {
    if (!p || !path) return DC_ERR_ARG;
    return load(p, path_from_wide(path), settings_json, seconds, play, err, err_len);
}

DC_API int dc_load_utf8(dc_player* p, const char* path, const char* settings_json, double seconds, int play, char* err, int err_len) {
    if (!p || !path) return DC_ERR_ARG;
    return load(p, path_from_utf8(path), settings_json, seconds, play, err, err_len);
}

}  // extern "C"

namespace {

int load(dc_player* p, const std::filesystem::path& path, const char* settings_json, double seconds, int play, char* err, int err_len) {
    try {
        std::lock_guard<std::mutex> l(p->op);
        {
            std::lock_guard<std::mutex> g(p->pm);   // a dc_stop from here on cancels this load
            p->loading = true;
            p->stop_requested = false;
        }
        struct Loaded {
            dc_player* p;
            ~Loaded() {
                std::lock_guard<std::mutex> g(p->pm);
                p->loading = false;
                p->starting.reset();
            }
        } loaded{p};
        p->design = 1.0;   // not the last player's: a design stopped halfway would show until the next one ends
        daphne::Settings s;
        std::string msg;
        if (!parse(settings_json, s, msg)) {
            put(err, err_len, msg);
            return DC_ERR_SETTINGS;
        }
        stop_player(p);
        {
            std::lock_guard<std::mutex> g(p->m);
            p->events.clear();
            p->track = 0;
            p->error.clear();
            p->failed = false;
            p->report = {};
        }
        daphne::OutputChoice out;
        out.api = s.output.api;
        out.device = s.output.device;
        daphne::PlayCallbacks cb;
        cb.on_start = [p](const daphne::PlayReport& r) {
            {
                std::lock_guard<std::mutex> g(p->m);
                p->report = r;
            }
            push_event(p, {{"type", "started"}});
        };
        cb.on_track_started = [p](const daphne::PlayReport& r) {
            {
                std::lock_guard<std::mutex> g(p->m);
                p->report = r;
                ++p->track;
            }
            push_event(p, {{"type", "track"}});
        };
        cb.on_event = [p](const std::string& e) { push_event(p, {{"type", "info"}, {"text", e}}); };
        cb.on_design = [p](double f) { p->design = f; };
        cb.on_ended = [p]() { push_event(p, {{"type", "ended"}}); };
        cb.on_changed = [p](const daphne::PlayReport& r) {
            {
                std::lock_guard<std::mutex> g(p->m);
                p->report = r;
            }
            push_event(p, {{"type", "changed"}});
        };
        cb.on_failed = [p](const std::string& e) {
            {
                std::lock_guard<std::mutex> g(p->m);
                p->failed = true;
                p->error = e;
            }
            push_event(p, {{"type", "failed"}, {"error", e}});
        };
        std::shared_ptr<daphne::Player> player;
        {
            std::lock_guard<std::mutex> g(g_deps_m);
            player = std::make_shared<daphne::Player>(g_deps);
        }
        player->pause(!play);
        bool stopped_already = false;
        {
            std::lock_guard<std::mutex> g(p->pm);   // dc_stop can reach it from here on, without op
            stopped_already = p->stop_requested;    // while the last track was stopped (a driver slow to let go)
            if (!stopped_already) p->starting = player;
        }
        if (stopped_already) {
            put(err, err_len, "cancelled (dc_stop or dc_cancel)");
            return DC_ERR_CANCELLED;
        }
        std::string e;
        const bool started = player->start(path, s, out, cb, &e, seconds);
        bool cancelled = false;
        {
            std::lock_guard<std::mutex> g(p->pm);
            p->starting.reset();
            cancelled = p->stop_requested;
        }
        if (cancelled) {   // dc_stop came during the start: not published, not an error of the file or the device
            if (started) {
                player->stop();
                daphne::PlayReport r;
                std::string we;
                player->wait(&r, &we);
            }
            put(err, err_len, "cancelled (dc_stop or dc_cancel)");
            return DC_ERR_CANCELLED;
        }
        if (!started) {
            put(err, err_len, e);
            std::lock_guard<std::mutex> g(p->m);
            p->error = e;
            p->failed = true;
            return player->last_failure_was_device() ? DC_ERR_DEVICE : DC_ERR_FILE;
        }
        {
            std::lock_guard<std::mutex> g(p->pm);   // published: queries see it from now on
            p->player = std::move(player);
        }
        return DC_OK;
    } catch (const std::exception& ex) {
        put(err, err_len, ex.what());
        return DC_ERR_FILE;
    } catch (...) {   // anything else must not cross the C boundary (it would end the host)
        put(err, err_len, "unknown error");
        return DC_ERR_INTERNAL;
    }
}

}  // namespace

extern "C" {

DC_API int dc_apply(dc_player* p, const char* settings_json, char* err, int err_len) {
    if (!p) return DC_ERR_ARG;
    try {
        std::lock_guard<std::mutex> l(p->op);
        daphne::Settings s;
        std::string msg;
        if (!parse(settings_json, s, msg)) {
            put(err, err_len, msg);
            return DC_ERR_SETTINGS;
        }
        if (const auto pl = current(p)) {
            daphne::OutputChoice out;
            out.api = s.output.api;
            out.device = s.output.device;
            pl->apply(s, out);
        }
        return DC_OK;
    } catch (const std::exception& ex) {   // not the caller's argument: the engine failed (memory, ...)
        put(err, err_len, ex.what());
        return DC_ERR_INTERNAL;
    } catch (...) {
        put(err, err_len, "unknown error");
        return DC_ERR_INTERNAL;
    }
}

DC_API void dc_pause(dc_player* p, int paused) {
    if (!p) return;
    if (const auto pl = current(p)) pl->pause(paused != 0);
}

DC_API void dc_seek(dc_player* p, double seconds) {
    if (!p) return;
    if (const auto pl = current(p)) pl->seek(seconds);
}

DC_API void dc_set_next(dc_player* p, const wchar_t* path) { set_next(p, path_from_wide(path), std::nullopt); }
DC_API void dc_set_next_utf8(dc_player* p, const char* path) { set_next(p, path_from_utf8(path), std::nullopt); }

DC_API void dc_set_next_gain(dc_player* p, const wchar_t* path, double headroom_db) {
    set_next(p, path_from_wide(path), headroom(headroom_db));
}
DC_API void dc_set_next_gain_utf8(dc_player* p, const char* path, double headroom_db) {
    set_next(p, path_from_utf8(path), headroom(headroom_db));
}

DC_API void dc_stop(dc_player* p) {
    if (!p) return;
    try {
        cancel_start(p);   // a load in progress returns at its next check (a design, the prefill) instead of playing
        std::lock_guard<std::mutex> l(p->op);
        stop_player(p);
    } catch (...) {
    }
}

DC_API void dc_cancel(dc_player* p) {
    if (!p) return;
    try {
        cancel_start(p);
    } catch (...) {
    }
}

DC_API int dc_state(dc_player* p, char* out, int len) {
    if (!p) return DC_ERR_ARG;
    try {
        const auto pl = current(p);   // never waits for a load in progress
        json j;
        std::string state = "idle";
        double position = 0, duration = 0;
        const bool failed = [&] {
            std::lock_guard<std::mutex> g(p->m);
            return p->failed;
        }();
        if (pl) {
            if (pl->ended()) state = "ended";
            else if (failed) state = "failed";
            else state = pl->paused() ? "paused" : "playing";
            position = pl->position_seconds();
        } else if (p->loading) {
            state = "loading";
        } else if (failed) {
            state = "failed";
        }
        std::lock_guard<std::mutex> g(p->m);
        if (p->report.source.rate > 0 && p->report.source.frames > 0)
            duration = static_cast<double>(p->report.source.frames) / p->report.source.rate;
        j = {{"state", state}, {"position", position}, {"duration", duration}, {"track", p->track},
             {"design", p->design.load()}, {"error", p->error}, {"report", report_json(p->report)}};
        if (const auto ls = daphne::live_stats(); ls.active && pl)
            j["live"] = {{"fill", ls.fill}, {"read", ls.read}, {"resyncs", ls.resyncs}, {"starves", ls.starves}, {"adjust", ls.adjust}, {"target", ls.target}};
        if (pl) {
            // the report is the one of the last start / change; the speed is measured on as it plays
            j["report"]["realtime"] = pl->realtime_factor();
            j["asio_rates"] = pl->asio_rates();
            j["debug"] = json::parse(pl->debug_json(), nullptr, false);
        }
        return put_json(out, len, j.dump());
    } catch (...) {
        return DC_ERR_ARG;
    }
}

DC_API int dc_poll_event(dc_player* p, char* out, int len) {
    if (!p) return DC_ERR_ARG;
    std::unique_lock<std::mutex> l(p->m);
    if (p->events.empty()) return 0;
    const std::string& e = p->events.front();
    const int r = put_json(out, len, e);
    if (r >= 0) p->events.pop_front();
    return r;
}

DC_API double dc_design(dc_player* p) { return p ? p->design.load() : 1.0; }

DC_API void dc_live_bus(void* base, int cap_frames, int rate) {
    daphne::live_disconnect();
    daphne::set_live_bus(base, cap_frames, rate);
}
DC_API void dc_live_rate(int rate) { daphne::set_live_rate(rate); }
DC_API int dc_live_socket(const char* path, int rate) { return path && daphne::live_connect(path, rate) ? 1 : 0; }

DC_API void dc_meter(dc_player* p, double* l, double* r) {
    double a = 0, b = 0;
    if (p)
        if (const auto pl = current(p)) pl->meter(a, b);
    if (l) *l = a;
    if (r) *r = b;
}

DC_API int dc_filter_cache_dir(wchar_t* out, int len) {
    try {
#ifdef _WIN32
        const std::wstring d = daphne::filter_cache_dir().wstring();
#else
        const std::wstring d = daphne::to_wide(daphne::platform::path_to_utf8(daphne::filter_cache_dir()));
#endif
        if (!out || len <= static_cast<int>(d.size())) return d.empty() ? 0 : DC_ERR_BUFFER;
        std::wmemcpy(out, d.c_str(), d.size() + 1);
        return static_cast<int>(d.size());
    } catch (...) {   // not "the cache is off" (0): the folder could not be worked out
        return DC_ERR_INTERNAL;
    }
}

DC_API int dc_filter_cache_dir_utf8(char* out, int len) {
    try {
        const std::string d = daphne::platform::path_to_utf8(daphne::filter_cache_dir());
        if (!out || len <= static_cast<int>(d.size())) return d.empty() ? 0 : DC_ERR_BUFFER;
        std::memcpy(out, d.c_str(), d.size() + 1);
        return static_cast<int>(d.size());
    } catch (...) {   // not "the cache is off" (0): the folder could not be worked out
        return DC_ERR_INTERNAL;
    }
}

DC_API long long dc_filter_cache_prune(const char* settings, int limit_mb) {
    try {
        const json j = json::parse(settings ? settings : "{}", nullptr, false);
        if (j.is_discarded()) return -1;
        const auto res = daphne::settings_from_json(j);
        const auto r = daphne::prune_filter_cache(static_cast<uint64_t>(limit_mb < 0 ? 0 : limit_mb) << 20,
                                                 daphne::filter_cache_keys_in_use(res.settings));
        return static_cast<long long>(r.bytes);
    } catch (...) {
        return -1;
    }
}

DC_API void dc_keep_device(int on) {
    try {
        daphne::keep_output_devices(on != 0);
    } catch (...) {
    }
}

DC_API int dc_release_device(void) {
    try {
        return daphne::release_kept_output_device() ? 1 : 0;
    } catch (...) {
        return 0;
    }
}

DC_API int dc_device_kept(void) { return daphne::output_device_kept() ? 1 : 0; }

// tests only (internal.h): the devices and decoders the next players are built with
DC_API void dc_internal_player_deps(const daphne::PlayerDeps* deps) {
    std::lock_guard<std::mutex> g(g_deps_m);
    g_deps = deps ? *deps : daphne::PlayerDeps{};
}

DC_API int dc_devices(char* out, int len) {
    try {
        json w = json::array(), a = json::array(), c = json::array();
        for (const auto& d : daphne::list_wasapi_devices(false)) w.push_back({{"id", d.id}, {"name", d.name}, {"default", d.is_default}});
        for (const auto& d : daphne::list_asio_drivers()) a.push_back({{"name", d.name}, {"clsid", d.clsid}});
        for (const auto& d : daphne::list_coreaudio_devices())
            c.push_back({{"uid", d.uid}, {"name", d.name}, {"default", d.is_default}, {"rate", d.rate}, {"channels", d.channels},
                         {"rates", d.rates}, {"transport", d.transport}, {"hog_pid", d.hog_pid}});
        return put_json(out, len, json{{"wasapi", w}, {"asio", a}, {"coreaudio", c}}.dump());
    } catch (...) {
        return DC_ERR_ARG;
    }
}

}  // extern "C"
