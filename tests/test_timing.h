#pragma once
// Time in the player tests (doc/N28).
//
// DAPHNE_TEST_TIME_SCALE (a number >= 1, default 1; set only in the sanitizer jobs): every time the tests arrange runs
// that many times slower, all together, so that a test takes the same path on a machine (or a sanitizer) that cannot
// keep up with real time: the fake device's pace, sleeps, waits, the live producers' clocks, the stalls and delays a
// test injects. Nothing the tests measure in the audio changes (alignment, levels, responses): only the wall clock.
//
// kHardLimitSeconds: the one limit the scale does not touch. A test case still running after it ends the run, with
// the test's name and what the player it drives was doing (test_watchdog.cpp), so that a real hang does not hold CI.

#include <chrono>
#include <cstdlib>
#include <functional>
#include <string>

#include "daphne/platform/platform.h"

namespace test {

inline double time_scale() {
    static const double s = [] {
        const auto v = daphne::platform::get_env("DAPHNE_TEST_TIME_SCALE");
        const double x = v ? std::atof(v->c_str()) : 1.0;
        return x >= 1.0 ? x : 1.0;
    }();
    return s;
}

// test seconds -> wall-clock seconds
inline double wall_s(double s) { return s * time_scale(); }

// a duration in test time -> the steady clock's duration in wall time
template <class Rep, class Period>
std::chrono::steady_clock::duration wall(std::chrono::duration<Rep, Period> d) {
    return std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(d) * time_scale());
}

// wall-clock time since t0 -> test seconds
inline double test_seconds_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() / time_scale();
}

inline constexpr double kHardLimitSeconds = 120.0;

// What the watchdog prints about the test that overran: the player rig registers its state while it lives.
void watchdog_set_state(std::function<std::string()> f);

}  // namespace test
