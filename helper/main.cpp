// daphne-helper: the player as a child process for a host that is not native code (macOS MIKU, an Electron
// app), N4. One JSON object per line on stdin / stdout, UTF-8; the C API (daphne_core.h) underneath, one call
// per command, so a host behaves as the Windows extension does through P/Invoke.
//
//   request   {"id": n, "cmd": "...", ...}         answered by {"id": n, "ok": true|false, ...}
//   pushed    {"e": "event", "event": {...}}        dc_poll_event (started, track, changed, ended, failed, info)
//             {"e": "design", "progress": x}        filter design progress while it changes (0..1; 1 = done)
//             {"e": "hello", "version": "...", "abi": n}  once, at start
//
//   hello                                  -> version, abi
//   load     path, settings, seek, play    -> code, error   (blocks until playing or failed: run in order)
//   apply    settings                      -> code, error
//   pause    paused                        stop     hold   (the device is released: rate, DAC format, hog mode;
//                                                                  hold: kept for the next load, as MIKU does)
//                                          a stop or a load cancels at once the load that runs (dc_cancel) and the
//                                          loads still queued: they answer code DC_ERR_CANCELLED; nothing of them plays
//   seek     seconds                       next     path, gain (dB or null)
//   state                                  -> state (dc_state's object), kept (a device is kept, nothing loaded)
//   meter                                  -> l, r
//   devices                                -> devices (dc_devices' object)
//   cache_dir                              -> dir      cache_prune  settings, limit_mb -> bytes
//   quit                                   stops playback and exits (also on end of input)
//
// macOS exclusive mode: a device is kept between loads (dc_keep_device): the next load takes it over, a stop without
// hold, quit and the end of input put it back.
// Commands run in order on one thread (load can take minutes while a long filter is designed); state, meter,
// devices and cache_dir are answered at once from the reader, so the host's timer never waits for a load.
// Diagnostics go to stderr.
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <deque>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "daphne_core.h"

using json = nlohmann::json;

namespace {

std::mutex g_out_m;

void emit(const json& j) {
    const std::string s = j.dump(-1, ' ', false, json::error_handler_t::replace);
    std::lock_guard<std::mutex> l(g_out_m);
    std::fwrite(s.data(), 1, s.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

json reply(const json& req, bool ok) {
    json r = {{"ok", ok}};
    if (req.contains("id")) r["id"] = req["id"];
    return r;
}

// a dc_* function that fills a JSON buffer, grown until it fits
template <class F>
json fetch(F f) {
    std::vector<char> b(1 << 14);
    for (int i = 0; i < 8; ++i) {
        const int n = f(b.data(), static_cast<int>(b.size()));
        if (n == DC_ERR_BUFFER) {
            b.resize(b.size() * 4);
            continue;
        }
        if (n <= 0) return json();
        return json::parse(std::string(b.data(), static_cast<size_t>(n)), nullptr, false);
    }
    return json();
}

std::string str(const json& j, const char* k) { return j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : std::string(); }
double num(const json& j, const char* k, double d) { return j.contains(k) && j[k].is_number() ? j[k].get<double>() : d; }
bool flag(const json& j, const char* k, bool d) { return j.contains(k) && j[k].is_boolean() ? j[k].get<bool>() : d; }
// settings may come as an object or as a JSON string
std::string settings_of(const json& j) {
    if (!j.contains("settings")) return "{}";
    return j["settings"].is_string() ? j["settings"].get<std::string>() : j["settings"].dump();
}

class Helper {
public:
    Helper() : p_(dc_create()) { dc_keep_device(1); }
    ~Helper() { dc_destroy(p_); }

    void run() {
        emit({{"e", "hello"}, {"version", dc_version()}, {"abi", dc_abi_version()}});
        std::thread worker([this] { work(); });
        std::thread pump([this] { pump_events(); });
        std::string line;
        json quit_req;
        while (std::getline(std::cin, line)) {
            if (line.empty() || line == "\r") continue;
            json req = json::parse(line, nullptr, false);
            if (req.is_discarded() || !req.is_object()) {
                std::fprintf(stderr, "daphne-helper: not a JSON object: %.200s\n", line.c_str());
                continue;
            }
            if (str(req, "cmd") == "quit") {   // handled here: the reader stops reading, nothing has to interrupt it
                quit_req = std::move(req);
                break;
            }
            // before it is queued, not after the load it replaces: a stop, or a load (MIKU changes tracks with a load
            // alone, no stop in between), cancels the loads before it
            const std::string cmd = str(req, "cmd");
            if (cmd == "stop" || cmd == "load") {
                const uint64_t gen = cancel_loads(cmd == "stop" ? "cancelled (stop)" : "cancelled (a later load)");
                if (cmd == "load") req["_gen"] = gen;
            }
            if (!answer_now(req)) {
                std::lock_guard<std::mutex> l(m_);
                queue_.push_back(std::move(req));
                cv_.notify_one();
            }
        }
        {
            std::lock_guard<std::mutex> l(m_);
            quit_ = true;
            queue_.clear();   // commands not started yet are dropped
            cv_.notify_one();
        }
        dc_stop(p_);
        worker.join();
        dc_stop(p_);   // what the last command may have started
        dc_release_device();
        pump_stop_ = true;
        pump.join();
        if (!quit_req.is_null()) emit(reply(quit_req, true));
    }

private:
    // A stop or a load came: the loads still queued are dropped (each answers cancelled) and the one the worker runs
    // returns at once (dc_cancel), so the command is not held behind a load (a first long design) and nothing of them
    // plays. A load the worker has taken but not yet started sees the new generation and does not start (execute).
    // A cancel in the microseconds between that check and dc_load taking hold is not seen by dc_load: the pump cancels
    // again every 20 ms while the load that runs is not the latest.
    uint64_t cancel_loads(const char* why) {
        std::vector<json> dropped;
        uint64_t gen;
        {
            std::lock_guard<std::mutex> l(m_);
            gen = ++gen_;
            for (auto it = queue_.begin(); it != queue_.end();) {
                if (str(*it, "cmd") == "load") {
                    dropped.push_back(std::move(*it));
                    it = queue_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (const auto& d : dropped) {
            json r = reply(d, false);
            r["code"] = DC_ERR_CANCELLED, r["error"] = why;
            emit(r);
        }
        dc_cancel(p_);
        return gen;
    }

    // queries that never wait for a running command
    bool answer_now(const json& req) {
        const std::string cmd = str(req, "cmd");
        if (cmd == "state") {
            json r = reply(req, true);
            r["state"] = fetch([&](char* b, int n) { return dc_state(p_, b, n); });
            r["kept"] = dc_device_kept() != 0;
            emit(r);
        } else if (cmd == "meter") {
            double l = 0, rr = 0;
            dc_meter(p_, &l, &rr);
            json r = reply(req, true);
            r["l"] = l, r["r"] = rr;
            emit(r);
        } else if (cmd == "devices") {
            json r = reply(req, true);
            r["devices"] = fetch([](char* b, int n) { return dc_devices(b, n); });
            emit(r);
        } else if (cmd == "cache_dir") {
            char b[4096];
            const int n = dc_filter_cache_dir_utf8(b, sizeof b);
            json r = reply(req, true);
            r["dir"] = n > 0 ? std::string(b, static_cast<size_t>(n)) : std::string();
            emit(r);
        } else if (cmd == "hello") {
            json r = reply(req, true);
            r["version"] = dc_version(), r["abi"] = dc_abi_version();
            emit(r);
        } else {
            return false;
        }
        return true;
    }

    void work() {
        for (;;) {
            json req;
            {
                std::unique_lock<std::mutex> l(m_);
                cv_.wait(l, [&] { return quit_ || !queue_.empty(); });
                if (queue_.empty()) return;   // quit_ and nothing left
                req = std::move(queue_.front());
                queue_.pop_front();
            }
            execute(req);
        }
    }

    void execute(const json& req) {
        const std::string cmd = str(req, "cmd");
        char err[2048] = {0};
        if (cmd == "load") {
            if (req.value("_gen", uint64_t{0}) != gen_.load()) {   // a later load or a stop came after it was taken
                json r = reply(req, false);
                r["code"] = DC_ERR_CANCELLED, r["error"] = "cancelled (replaced)";
                emit(r);
                return;
            }
            const std::string path = str(req, "path"), settings = settings_of(req);
            set_running(req.value("_gen", uint64_t{0}));   // the pump cancels it again if a later stop or load came
            const int code = dc_load_utf8(p_, path.c_str(), settings.c_str(), num(req, "seek", 0), flag(req, "play", true) ? 1 : 0, err, sizeof err);
            set_running(0);
            json r = reply(req, code == DC_OK);
            r["code"] = code, r["error"] = err;
            emit(r);
        } else if (cmd == "apply") {
            const std::string settings = settings_of(req);
            const int code = dc_apply(p_, settings.c_str(), err, sizeof err);
            json r = reply(req, code == DC_OK);
            r["code"] = code, r["error"] = err;
            emit(r);
        } else if (cmd == "pause") {
            dc_pause(p_, flag(req, "paused", true) ? 1 : 0);
            emit(reply(req, true));
        } else if (cmd == "seek") {
            dc_seek(p_, num(req, "seconds", 0));
            emit(reply(req, true));
        } else if (cmd == "next") {
            const std::string path = str(req, "path");
            if (req.contains("gain") && req["gain"].is_number()) dc_set_next_gain_utf8(p_, path.c_str(), req["gain"].get<double>());
            else dc_set_next_utf8(p_, path.empty() ? nullptr : path.c_str());
            emit(reply(req, true));
        } else if (cmd == "stop") {
            dc_stop(p_);
            if (!flag(req, "hold", false)) dc_release_device();
            emit(reply(req, true));
        } else if (cmd == "cache_prune") {
            const std::string settings = settings_of(req);
            json r = reply(req, true);
            r["bytes"] = dc_filter_cache_prune(settings.c_str(), static_cast<int>(num(req, "limit_mb", 0)));
            emit(r);
        } else {
            json r = reply(req, false);
            r["error"] = "unknown command: " + cmd;
            emit(r);
        }
    }

    void set_running(uint64_t gen) {
        std::lock_guard<std::mutex> l(run_m_);
        running_gen_ = gen;
    }

    // events and design progress, pushed as they come (every 20 ms)
    void pump_events() {
        double last_design = 1;
        while (!pump_stop_) {
            // a cancel that came between the generation check and dc_load taking hold is not recorded (dc_cancel only
            // reaches a load in progress): the load the worker runs is cancelled again while it is not the latest
            // (under run_m_: the worker cannot have moved on to a later load, which this would cancel)
            {
                std::lock_guard<std::mutex> l(run_m_);
                if (running_gen_ != 0 && running_gen_ != gen_.load()) dc_cancel(p_);
            }
            for (;;) {
                json ev = fetch([&](char* b, int n) { return dc_poll_event(p_, b, n); });
                if (ev.is_null() || ev.is_discarded()) break;
                emit({{"e", "event"}, {"event", ev}});
            }
            const double d = dc_design(p_);
            if (std::fabs(d - last_design) >= 0.01 || (d >= 1 && last_design < 1)) {
                emit({{"e", "design"}, {"progress", d}});
                last_design = d;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    dc_player* p_;
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<json> queue_;
    bool quit_ = false;
    std::atomic<uint64_t> gen_{0};   // bumped by every stop and load (cancel_loads); a load runs only in its own
    std::mutex run_m_;
    uint64_t running_gen_ = 0;   // under run_m_: the generation of the load in dc_load now (0: none)
    std::atomic<bool> pump_stop_{false};
};

}  // namespace

int main() {
#ifdef SIGPIPE
    // a host that went away (MIKU crashed) closed stdout: a write fails instead of ending the process before the
    // cleanup at the end of input (the device's rate and DAC format put back, the hog released)
    std::signal(SIGPIPE, SIG_IGN);
#endif
    std::ios::sync_with_stdio(false);
    Helper h;
    h.run();
    return 0;
}
