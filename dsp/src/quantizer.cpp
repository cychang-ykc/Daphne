#include "daphne/dsp/quantizer.h"

#include <algorithm>
#include <cmath>

namespace daphne::dsp {
namespace {

uint64_t rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

// xoshiro256**
uint64_t xoshiro_next(uint64_t* s) {
    const uint64_t result = rotl(s[1] * 5, 7) * 9;
    const uint64_t t = s[1] << 17;
    s[2] ^= s[0];
    s[3] ^= s[1];
    s[1] ^= s[2];
    s[0] ^= s[3];
    s[2] ^= t;
    s[3] = rotl(s[3], 45);
    return result;
}

// xoshiro256** state from a 64-bit seed through splitmix64
void seed_state(uint64_t* s, uint64_t seed) {
    uint64_t z0 = seed;
    if (z0 == 0) z0 = 0x8a5cd789635d2dffull;
    for (int i = 0; i < 4; ++i) {
        uint64_t z = z0 + 0x9e3779b97f4a7c15ull * static_cast<uint64_t>(i + 1);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        s[i] = z ^ (z >> 31);
    }
}

}  // namespace

void Quantizer::prepare(const Format& in) {
    in_ = in;
    reset();
}

void Quantizer::reset() {
    seed_state(s_, seed_);
    clips_ = 0;
}

double Quantizer::uniform() { return static_cast<double>(xoshiro_next(s_) >> 11) * 1.1102230246251565e-16; }   // [0, 1)

void Quantizer::set_pass_through(bool on) {
    if (pass_ && !on) seed_state(s_, seed_);   // the dither starts afresh
    pass_ = on;
}

int Quantizer::process(const double* in, int frames, double* out) {
    const size_t n = static_cast<size_t>(frames) * std::max(1, in_.channels);
    if (pass_) {
        if (in != out) std::copy(in, in + n, out);
        return frames;
    }
    const double scale = std::ldexp(1.0, bits_ - 1), lsb = 1.0 / scale;
    const double top = (scale - 1.0) * lsb;
    for (size_t i = 0; i < n; ++i) {
        const double u1 = uniform(), u2 = uniform();   // explicit order: reproducible across compilers
        const double d = (u1 - u2) * lsb;              // TPDF, +-1 LSB
        const double q = std::round((in[i] + d) * scale) * lsb;
        const double qc = std::clamp(q, -1.0, top);
        if (qc != q) ++clips_;
        out[i] = qc;
    }
    return frames;
}

}  // namespace daphne::dsp
