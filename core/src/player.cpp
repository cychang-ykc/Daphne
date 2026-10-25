#include "daphne/engine/player.h"

#include <chrono>
#include <thread>

#include "player/core.h"

// Player: the public API (player.h); the processing thread is Player::Impl (player/core.h).
namespace daphne {

Player::Player() : Player(PlayerDeps{}) {}

Player::Player(PlayerDeps deps) : impl_(std::make_unique<Impl>()) {
    if (!deps.open_output) deps.open_output = platform_output_opener();
    if (!deps.open_decoder) deps.open_decoder = [](const std::filesystem::path& p, std::string* e) { return open_decoder(p, e); };
    if (!deps.output_caps) deps.output_caps = platform_output_caps();
    impl_->deps = std::move(deps);
}

Player::~Player() {
    stop();
    if (impl_->th.joinable()) impl_->th.join();
}

bool Player::start(const std::filesystem::path& path, const Settings& s, const OutputChoice& out, PlayCallbacks cb, std::string* error,
                   double start_seconds) {
    Impl& p = *impl_;
    p.path = path;
    p.start_sec = std::max(0.0, start_seconds);
    p.s = s;
    p.out = out;
    p.cb = std::move(cb);
    p.th = std::thread([&p] { p.run(); });
    std::unique_lock<std::mutex> l(p.m);
    p.cv.wait(l, [&p] { return p.start_done; });
    if (!p.start_ok) {
        l.unlock();
        p.th.join();
        if (error) *error = p.start_err;
        return false;
    }
    return true;
}

void Player::apply(const Settings& s, const OutputChoice& out) {
    std::lock_guard<std::mutex> l(impl_->m);
    impl_->pending = std::make_pair(s, out);
}

void Player::stop() { impl_->cancel = true; }

bool Player::finished() const { return impl_->done; }

int64_t Player::position() const { return impl_->pos; }
double Player::position_seconds() const { return impl_->pos_sec; }
double Player::realtime_factor() const { return impl_->rt_factor.load(std::memory_order_relaxed); }

void Player::pause(bool paused) { impl_->want_pause = paused; }
bool Player::paused() const { return impl_->want_pause; }

void Player::seek(double seconds) {
    std::lock_guard<std::mutex> l(impl_->m);
    impl_->seek_req = seconds;
}

void Player::set_next(const std::filesystem::path& path, std::optional<double> headroom_db) {
    impl_->queue.set(path, headroom_db);
}

bool Player::ended() const { return impl_->ended_flag; }
bool Player::last_failure_was_device() const { return impl_->device_fail; }
bool Player::last_failure_was_internal() const { return impl_->internal_fail; }

std::vector<int> Player::asio_rates() const {
    return impl_->report.asio_rates();
}

std::string Player::debug_json() const {
    return impl_->report.diag_json();
}

void Player::meter(double& l, double& r) {
    l = impl_->meter_l.exchange(0.0);
    r = impl_->meter_r.exchange(0.0);
}

bool Player::wait(PlayReport* report, std::string* error) {
    if (impl_->th.joinable()) impl_->th.join();
    if (report) *report = impl_->rep;
    if (!impl_->ok && error) *error = impl_->err;
    return impl_->ok;
}

bool play_file(const std::filesystem::path& path, const Settings& s, const OutputChoice& out, const std::atomic<bool>& cancel,
               const PlayCallbacks& cb, PlayReport* report, std::string* error) {
    Player p;
    if (!p.start(path, s, out, cb, error)) return false;
    while (!p.finished()) {
        if (cancel) p.stop();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return p.wait(report, error);
}

}  // namespace daphne
