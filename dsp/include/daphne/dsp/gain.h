#pragma once
// Gain (volume + per-track headroom) with a linear ramp when the gain changes.
//
// set_db() may be called from any thread; it is a lock-free store that process() picks up at the
// start of its next call. The first process() after prepare() / reset() starts at the target (no
// fade-in); later changes ramp linearly in amplitude over ramp_ms, whatever the block size, and end
// exactly on the target (0 dB after a change is bit-exact again).

#include <atomic>

#include "daphne/dsp/block.h"

namespace daphne::dsp {

class Gain final : public Block {
public:
    static constexpr double kRampMs = 20.0;
    explicit Gain(double db = 0.0, double ramp_ms = kRampMs) : db_(db), ramp_ms_(ramp_ms) {}

    void set_db(double db) { db_.store(db, std::memory_order_relaxed); }
    double db() const { return db_.load(std::memory_order_relaxed); }

    void prepare(const Format& in) override;
    Format output_format() const override { return fmt_; }
    int process(const double* in, int frames, double* out) override;
    int latency_frames() const override { return 0; }
    void reset() override;
    bool is_identity() const override;

    static double db_to_linear(double db);

private:
    Format fmt_{};
    std::atomic<double> db_;
    double ramp_ms_;
    double target_db_ = 0.0, target_ = 1.0;   // last target seen by process() (audio thread only)
    double current_ = 1.0;                     // linear gain currently applied (audio thread only)
    double step_ = 0.0;                        // per-frame ramp step towards target_
};

}  // namespace daphne::dsp
