#pragma once
// The linear part of an amplifier for Tube Color (doc/N28): the coupling networks before the curve (pre) and the output
// transformer after it (post), as first- and second-order sections. M2 fits them to the M0 reference responses.
//
// The response fades in with the drive by moving the corners, never by mixing with the dry signal (a dry / filtered mix
// cuts deeper than the amplifier itself half way: doc/N28). m = 0: every section is the identity; m = 1: the reference.
// A low-frequency section's corner scales by c = sqrt(m), a high-frequency section's by 1 / c: of the powers of m
// compared (doc/N28), the square root keeps every 1 % drive step closest to an even fade in dB at every frequency (at
// most 1.31 x), with almost nothing at the first step.
//
// Sections are TPT (zero-delay feedback, Zavalishin) filters, prewarped at their reference corner: they stay stable and
// smooth while the corner moves.

#include <complex>
#include <vector>

namespace daphne::color {

// General1 / General2: a section of the virtual loudspeaker load (doc/N28, verify/load_filters.py): its poles at fc_hz
// (and q), its numerator c1 S + c0 over S + 1, or c2 S^2 + c1 S + c0 over S^2 + S / q + 1 (S = s / (2 pi fc_hz); a
// state-variable filter mixing its outputs). It has no corner to move and does not fade itself: the stage fades the
// whole load, y + m (L(y) - y) (one section's share of the load can be far from unity; the whole is near it).
struct Section {
    enum class Kind { HighPass1, LowPass1, HighPass2, LowPass2, General1, General2 };
    Kind kind = Kind::HighPass2;
    double fc_hz = 20.0;   // the reference corner (general: the poles)
    double q = 0.7071;     // second order
    double c2 = 0.0, c1 = 0.0, c0 = 0.0;   // general: the numerator
    bool high() const { return kind == Kind::LowPass1 || kind == Kind::LowPass2; }   // a high-frequency section
    bool general() const { return kind == Kind::General1 || kind == Kind::General2; }
};

struct LinearModel {
    std::vector<Section> pre, post;
    bool empty() const { return pre.empty() && post.empty(); }
};

// The response fade: m = smoothstep(drive / kResponseRampEnd), 1 from kResponseRampEnd up (doc/N28). Below it the whole
// amplifier fades in; above it the response is the preset's and the drive changes only the distortion.
inline constexpr double kResponseRampEnd = 0.25;
double response_mix(double drive);

// One section at a sample rate, its corner movable.
class SectionFilter {
public:
    void prepare(const Section& s, int rate);
    // m in [0, 1]: the fade (m = 0 is never asked for: the stage is then skipped)
    void set_mix(double m);
    double process(double x, double* st) const;
    bool active() const { return active_; }
    // its response at hz with the current corner (exact: the TPT section is the prewarped bilinear transform)
    std::complex<double> response(double hz) const;

private:
    Section s_;
    int rate_ = 0;
    bool active_ = false;   // false: the reference corner is above the rate's reach (no local oversampling yet)
    double g_ref_ = 0.0, k_ = 0.0;
    double g_ = 0.0, a_ = 0.0;   // the current coefficients (a: 1st order G, 2nd order h)
    double n2_ = 0.0, n1_ = 0.0, n0_ = 0.0;   // general: the numerator at the current fade
};

// A chain of sections, two state words per channel and section.
class SectionChain {
public:
    void prepare(const std::vector<Section>& v, int rate, int channels);
    void set_mix(double m);
    double process(double x, int ch);
    void reset();
    std::complex<double> response(double hz) const;

private:
    std::vector<SectionFilter> f_;
    std::vector<double> st_;   // [channel][section][2]
};

}  // namespace daphne::color
