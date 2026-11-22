#include "daphne/color/tube_color.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>

#include "daphne/color/stage.h"
#include "daphne/color/curves.h"
#include "daphne/color/test_curve.h"

namespace daphne::color {

bool available(std::string_view preset) { return test_curve() || preset_curve(preset) != nullptr; }

bool available_any() {
    for (const char* p : {"set-300b", "set-2a3", "set-845", "pp-el34-ul"})
        if (available(p)) return true;
    return false;
}

double auto_makeup_db(const std::vector<std::pair<double, double>>& t, double drive) {
    if (t.empty() || drive <= 0.0) return 0.0;
    if (drive <= t.front().first) return t.front().second * drive / t.front().first;   // from 0 dB at drive 0
    for (size_t i = 1; i < t.size(); ++i)
        if (drive <= t[i].first)
            return t[i - 1].second + (t[i].second - t[i - 1].second) * (drive - t[i - 1].first) / (t[i].first - t[i - 1].first);
    return t.back().second;
}

TubeColorStage::TubeColorStage(StageParams p) : p_(std::move(p)), target_(std::max(0.0, p_.drive)) {
    trim_ = trim_target_ = std::pow(10.0, p_.trim_db / 20.0);
    align_ = align_target_ = std::clamp(p_.align, 1e-6, 1.0);
    // built at a drive above 0: running from the start; at 0: idle
    d_ = last_target_ = target_.load();
    w_ = d_ > 0.0 ? 1.0 : 0.0;
    vol_.push_back({0, p_.volume, p_.volume, 0});
}

double TubeColorStage::volume_at(int64_t k) const {
    for (auto it = vol_.rbegin(); it != vol_.rend(); ++it) {
        if (k < it->at) continue;
        if (it->len <= 0 || k >= it->at + it->len) return it->to;
        return it->from + (it->to - it->from) * static_cast<double>(k - it->at + 1) / it->len;
    }
    return vol_.front().from;
}

void TubeColorStage::set_volume(double v, int64_t at, int ramp) {
    if (volume_at(std::max<int64_t>(at, in_count_)) == v && vol_.back().to == v) return;
    // the gain stage ramps from where it is when the change arrives (Gain: linear in amplitude, ending on the target)
    vol_.push_back({at, volume_at(at - 1), v, std::max(1, ramp)});
}

void TubeColorStage::prepare(const dsp::Format& in) {
    fmt_ = in;
    const int ch = std::max(0, in.channels);
    const auto chs = static_cast<size_t>(ch);
    const int l = Oversampler::factor(in.rate);
    if (l > 1) os_.prepare(in.rate, l, ch, in.max_frames);
    else os_ = Oversampler{};
    delay_ = os_.delay();
    hi_.assign(static_cast<size_t>(l) * chs, 0.0);
    xin_.assign(chs, 0.0), dly_.assign(chs, 0.0), path_.assign(chs, 0.0), prime_y_.assign(chs, 0.0);
    hist_frames_ = kHistory + delay_ + 1;
    hist_.assign(static_cast<size_t>(hist_frames_) * chs, 0.0);
    hist_at_ = fed_ = 0;
    const int rate = in.rate * l;   // where the curve and the sections run
    dc_r_ = rate > 0 ? std::exp(-2.0 * std::numbers::pi * kDcBlockHz / rate) : 0.0;
    dc_x1_.assign(chs, 0.0);
    dc_y1_.assign(chs, 0.0);
    glide_frames_ = std::max(1, static_cast<int>(std::lround(kGlideMs * 1e-3 * in.rate)));
    align_frames_ = std::max(1, static_cast<int>(std::lround(kAlignGlideMs * 1e-3 * in.rate)));
    w_step_ = 1.0 / std::max(1.0, std::round(kSwitchMs * 1e-3 * in.rate));
    pre_.prepare(p_.linear.pre, rate, ch);
    post_.prepare(p_.linear.post, rate, ch);
    load_.prepare(p_.load, rate, ch);
    mix_d_ = -1.0;
    update_mix();
    primed_ = w_ > 0.0;   // running from the start: its state builds up with the music
}

void TubeColorStage::reset() {
    std::fill(dc_x1_.begin(), dc_x1_.end(), 0.0);
    std::fill(dc_y1_.begin(), dc_y1_.end(), 0.0);
    std::fill(hist_.begin(), hist_.end(), 0.0);
    hist_at_ = fed_ = 0;
    pre_.reset();
    post_.reset();
    load_.reset();
    os_.reset();
    // a reset (seek, new track) lands where the drive and the volume are going
    const double v = vol_.back().to;
    vol_.assign(1, {0, v, v, 0});
    in_count_ = 0;
    const double t = target_.load();
    d_ = last_target_ = t;
    left_ = 0;
    w_ = t > 0.0 ? 1.0 : 0.0;
    primed_ = w_ > 0.0;
    align_ = align_target_;   // and where the alignment is going
    align_left_ = 0;
    update_mix();
}

void TubeColorStage::set_drive(double d) { target_.store(std::max(0.0, d)); }

void TubeColorStage::set_trim(double db) {
    const double t = std::pow(10.0, db / 20.0);
    if (t == trim_target_) return;
    trim_target_ = t;
    trim_step_ = (t - trim_) / glide_frames_;
}

void TubeColorStage::set_align(double a, bool now) {
    a = std::clamp(a, 1e-6, 1.0);
    align_target_ = a;
    if (now) {
        align_ = a;
        align_left_ = 0;
        return;
    }
    align_left_ = align_frames_;
    align_step_ = (a - align_) / align_frames_;
}

void TubeColorStage::update_mix() {
    if (d_ == mix_d_ || d_ <= 0.0) return;
    mix_d_ = d_;
    const double m = response_mix(d_);
    mix_m_ = m;
    pre_.set_mix(m);
    post_.set_mix(m);
}

void TubeColorStage::glide_to(double v) {
    last_target_ = v;
    left_ = glide_frames_;
    step_ = (v - d_) / glide_frames_;
}

// The processing path picks up where a path that had run all along would be: the oversampler's two filters are filled
// from the history (up to kHistory frames, the newest last); the curve, sections and DC blocker start from rest at next
// to no drive.
void TubeColorStage::prime() {
    const auto ch = static_cast<size_t>(fmt_.channels);
    std::fill(dc_x1_.begin(), dc_x1_.end(), 0.0);
    std::fill(dc_y1_.begin(), dc_y1_.end(), 0.0);
    pre_.reset();
    post_.reset();
    load_.reset();
    d_ = last_target_ = kRiseFrom;
    left_ = 0;
    update_mix();
    if (os_.factor() > 1) {
        os_.reset();
        const int n = std::min(fed_, kHistory);
        for (int k = n; k >= 1; --k) {   // the frame k before the one about to come, divided by its volume as in the path
            const int at = ((hist_at_ - k) % hist_frames_ + hist_frames_) % hist_frames_;
            const double v = volume_at(in_count_ - k);
            const double g = v < kSkipBelow ? 1.0 : 1.0 / (v * p_.level);
            for (size_t c = 0; c < ch; ++c) prime_y_[c] = hist_[static_cast<size_t>(at) * ch + c] * g;
            os_.up(prime_y_.data(), hi_.data());
            os_.down(hi_.data(), prime_y_.data());
        }
    }
    primed_ = true;
}

void TubeColorStage::run_frame(const double* x, double* y) {
    const double d = d_ * align_;   // the effective drive (the level alignment)
#if defined(DAPHNE_COLOR_TEST_CURVE)
    const TestCurve test;
#endif
    const PresetCurve* const f = p_.curve;
    for (int c = 0; c < fmt_.channels; ++c) {
        const auto k = static_cast<size_t>(c);
        const double v = pre_.process(x[c], c);
        double r = 0.0;   // the distortion the curve adds
        if (!skip_curve_) {
#if defined(DAPHNE_COLOR_TEST_CURVE)
            r = (f ? (*f)(d * v) : test(d * v)) / d - v;
#else
            r = f ? (*f)(d * v) / d - v : 0.0;
#endif
        }
        const double w = r - dc_x1_[k] + dc_r_ * dc_y1_[k];      // without its DC
        dc_x1_[k] = r;
        dc_y1_[k] = w;
        const double o = post_.process(v + w, c);
        y[c] = p_.load.empty() ? o : o + mix_m_ * (load_.process(o, c) - o);   // the load, faded with the response
    }
}

void TubeColorStage::process_path(const double* x, double* y) {
    const int l = os_.factor();
    if (l == 1) {
        run_frame(x, y);
        return;
    }
    os_.up(x, hi_.data());
    for (int p = 0; p < l; ++p) {
        double* h = hi_.data() + static_cast<size_t>(p) * static_cast<size_t>(fmt_.channels);
        run_frame(h, h);
    }
    os_.down(hi_.data(), y);
}

int TubeColorStage::process(const double* in, int frames, double* out) {
    const int ch = fmt_.channels;
    const auto chs = static_cast<size_t>(ch);
    const double t = target_.load();
    const bool active = t > 0.0;
    bool untouched = true;
    for (int i = 0; i < frames; ++i) {
        const double* x = in + static_cast<std::ptrdiff_t>(i) * ch;
        double* y = out + static_cast<std::ptrdiff_t>(i) * ch;   // (may be x: everything of x is read first)
        // the input, and the delay path's frame: the input delay_ frames ago (the input itself without oversampling)
        double* slot = hist_.data() + static_cast<size_t>(hist_at_) * chs;
        const int dat = ((hist_at_ - delay_) % hist_frames_ + hist_frames_) % hist_frames_;
        for (size_t c = 0; c < chs; ++c) {
            xin_[c] = x[c];
            dly_[c] = delay_ ? hist_[static_cast<size_t>(dat) * chs + c] : x[c];
        }
        for (size_t c = 0; c < chs; ++c) slot[c] = xin_[c];   // the input as it came (read above when the delay is 0)
        // where it is going
        if (active) {
            if (w_ < 1.0) {   // leaving 0: prime, then cross over at next to no drive
                if (!primed_) prime();
                w_ = std::min(1.0, w_ + w_step_);
            } else if (last_target_ != t) {
                glide_to(t);   // then the drive (a newer one while gliding: from where it is)
            }
        } else if (w_ > 0.0) {   // reaching 0: the drive down first, then cross back
            if (last_target_ != kRiseFrom) glide_to(kRiseFrom);
            else if (left_ == 0) w_ = std::max(0.0, w_ - w_step_);
        }
        if (left_ > 0) {
            d_ = --left_ == 0 ? last_target_ : d_ + step_;
            if (left_ == 0 || left_ % kControl == 0) update_mix();
        }
        // the volume cancelled around the processing path: in by the input's, out by the output content's
        const double v_in = volume_at(in_count_), v_out = volume_at(in_count_ - delay_);
        skip_curve_ = v_in < kSkipBelow;
        if (align_left_ > 0) align_ = --align_left_ == 0 ? align_target_ : align_ + align_step_;
        if (trim_ != trim_target_) {   // the trim glides (linear in amplitude, ending on the target)
            trim_ += trim_step_;
            if ((trim_step_ > 0 && trim_ >= trim_target_) || (trim_step_ < 0 && trim_ <= trim_target_)) trim_ = trim_target_;
        }
        // the makeup gain: the drive's automatic one and the trim, on what the processing path makes
        const double makeup = std::pow(10.0, (auto_makeup_db(p_.makeup, d_) + p_.load_gain_db * mix_m_) / 20.0) * trim_;
        const double g_in = skip_curve_ ? 1.0 : 1.0 / (v_in * p_.level), g_out = (skip_curve_ ? 1.0 : v_out * p_.level) * makeup;
        if (w_ <= 0.0) {   // idle: the delay path alone
            primed_ = false;
            if (!active) d_ = last_target_ = 0.0;
            for (size_t c = 0; c < chs; ++c) y[c] = dly_[c];
        } else {
            for (size_t c = 0; c < chs; ++c) xin_[c] *= g_in;
            process_path(xin_.data(), path_.data());
            for (size_t c = 0; c < chs; ++c) path_[c] *= g_out;
            if (w_ >= 1.0) {
                for (size_t c = 0; c < chs; ++c) y[c] = path_[c];
            } else {   // the crossfade between the two paths (raised cosine), aligned: both are the input delay_ frames ago
                const double g = 0.5 - 0.5 * std::cos(std::numbers::pi * w_);
                for (size_t c = 0; c < chs; ++c) y[c] = (1.0 - g) * dly_[c] + g * path_[c];
            }
            untouched = false;
        }
        hist_at_ = (hist_at_ + 1) % hist_frames_;
        ++in_count_;
        fed_ = std::min(fed_ + 1, hist_frames_);
    }
    untouched_ = untouched;
    // the schedule: what no frame still to come in or out (or the priming) reads goes
    const int64_t oldest = in_count_ - delay_ - kHistory - 1;
    while (vol_.size() > 1 && vol_[1].at + vol_[1].len <= oldest) vol_.erase(vol_.begin());
    return frames;
}

}  // namespace daphne::color
