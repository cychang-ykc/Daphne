#pragma once
// Tube Color's local oversampling (doc/N28): the curve makes harmonics far above the audio band, so the stage runs at
// 8x the rate family (352.8 / 384 kHz) at least. Below that its input is upsampled by L (the smallest integer reaching
// it), processed, and decimated back by L.
//
// Both filters are the resampler's design (N7, design_lowpass, linear phase, kTapsPerPhase taps per phase: the stop
// band from 0.5 of the input rate at about 150 dB, the pass band flat to 20 kHz at 44.1 kHz; the apodizing design
// needs 512 taps for 140 dB, doc/N28). The decimator takes
// the last of every L high-rate samples, where the round trip (two halves of a kernel of taps x L) lands exactly: the
// content is delayed by a whole number of input frames (kTapsPerPhase - 1), so a rebuilt chain with or without it
// lines up to the sample (the crossfade's content delay).

#include <vector>

namespace daphne::color {

class Oversampler {
public:
    static constexpr int kFamilyMultiple = 8;
    static constexpr int kTapsPerPhase = 224;

    // The factor for a stage at this rate (1: none).
    static int factor(int rate);

    // L > 1
    void prepare(int rate, int factor, int channels, int max_frames);
    int factor() const { return l_; }
    // Input frames between an input sample and its content in the output (the round trip).
    int delay() const { return l_ > 1 ? kTapsPerPhase - 1 : 0; }
    // One input frame in (all channels), L frames at the high rate out (interleaved).
    void up(const double* x, double* hi);
    // L high-rate frames in, one output frame out.
    void down(const double* hi, double* y);
    void reset();

    const std::vector<double>& kernel() const { return h_; }   // for the tests

private:
    int l_ = 1, ch_ = 0, n_ = 0;   // n: kernel length (taps x L)
    std::vector<double> h_;        // the designed low-pass, DC gain 1
    std::vector<double> hup_;      // [phase][k] = L h[k L + phase]
    std::vector<double> hdown_;    // h reversed, against the ring's oldest-first order
    std::vector<double> xin_;      // per channel: the last kTapsPerPhase input frames (ring, doubled)
    std::vector<double> xhi_;      // per channel: the last n high-rate samples (ring, doubled)
    int pin_ = 0, phi_ = 0;
};

}  // namespace daphne::color
