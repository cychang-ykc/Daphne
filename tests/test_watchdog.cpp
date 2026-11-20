// The hard limit of a test case (test_timing.h, doc/N28): a test still running after kHardLimitSeconds of wall clock,
// whatever DAPHNE_TEST_TIME_SCALE says, ends the run with its name and the state of the player it drives. The
// benchmarks ([.bench], run by hand) are exempt.
#include <catch_amalgamated.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

#include "test_timing.h"

namespace {

struct Watch {
    std::mutex m;
    std::condition_variable cv;
    std::string name;   // empty: no test case running (or an exempt one)
    std::chrono::steady_clock::time_point start;
    std::function<std::string()> state;
    bool thread_started = false;
};

Watch& watch() {
    static Watch* w = new Watch;   // never destroyed: the thread outlives main's statics
    return *w;
}

void run() {
    auto& w = watch();
    std::unique_lock<std::mutex> l(w.m);
    for (;;) {
        w.cv.wait_for(l, std::chrono::milliseconds(500));
        if (w.name.empty()) continue;
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - w.start).count();
        if (s <= test::kHardLimitSeconds) continue;
        std::fprintf(stderr, "\nHARD LIMIT: \"%s\" still running after %.0f s (not scaled: DAPHNE_TEST_TIME_SCALE %g)\n",
                     w.name.c_str(), s, test::time_scale());
        if (w.state) std::fprintf(stderr, "  the player: %s\n", w.state().c_str());
        std::fflush(stderr);
        std::_Exit(124);
    }
}

class Watchdog : public Catch::EventListenerBase {
public:
    using Catch::EventListenerBase::EventListenerBase;
    void testCaseStarting(const Catch::TestCaseInfo& t) override {
        auto& w = watch();
        std::lock_guard<std::mutex> l(w.m);
        const bool bench = t.tagsAsString().find("bench") != std::string::npos;
        w.name = bench ? std::string() : t.name;
        w.start = std::chrono::steady_clock::now();
        if (!w.thread_started) {
            w.thread_started = true;
            std::thread(run).detach();
        }
    }
    void testCaseEnded(const Catch::TestCaseStats&) override {
        auto& w = watch();
        std::lock_guard<std::mutex> l(w.m);
        w.name.clear();
        w.state = nullptr;
    }
};

}  // namespace

CATCH_REGISTER_LISTENER(Watchdog)

void test::watchdog_set_state(std::function<std::string()> f) {
    auto& w = watch();
    std::lock_guard<std::mutex> l(w.m);
    w.state = std::move(f);
}
