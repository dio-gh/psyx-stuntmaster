#pragma once

#include "stuntmaster/game/retail_hle.hpp"

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>

namespace stuntmaster::app {

// The hand-off between the guest worker, which reaches a movie boundary inside
// retail's code, and the host thread, which owns the window and the decoder.
// Each of the three phases (fade-to-black transition, movie preparation, and
// the play request itself) blocks the guest until the host has claimed and
// completed it, so no guest publication can race onto the display mid-movie.
class MovieRequestBridge final {
public:
    void transitionAndWait() {
        std::unique_lock lock{mutex_};
        transition_pending_ = true;
        transition_claimed_ = false;
        transition_completed_ = false;
        condition_.notify_all();
        condition_.wait(lock, [this] { return transition_completed_; });
        transition_pending_ = false;
    }

    [[nodiscard]] bool takeTransition() {
        std::lock_guard lock{mutex_};
        if (!transition_pending_ || transition_claimed_) {
            return false;
        }
        transition_claimed_ = true;
        return true;
    }

    void completeTransition() {
        std::lock_guard lock{mutex_};
        transition_completed_ = true;
        condition_.notify_all();
    }

    void prepareAndWait(
        const stuntmaster::game::RetailMoviePlayRequest& request) {
        std::unique_lock lock{mutex_};
        preparation_ = request;
        preparation_claimed_ = false;
        preparation_completed_ = false;
        condition_.notify_all();
        condition_.wait(lock, [this] { return preparation_completed_; });
        preparation_.reset();
    }

    [[nodiscard]] std::optional<stuntmaster::game::RetailMoviePlayRequest>
    takePreparation() {
        std::lock_guard lock{mutex_};
        if (!preparation_ || preparation_claimed_) {
            return std::nullopt;
        }
        preparation_claimed_ = true;
        return preparation_;
    }

    void completePreparation() {
        std::lock_guard lock{mutex_};
        preparation_completed_ = true;
        condition_.notify_all();
    }

    void requestAndWait(
        const stuntmaster::game::RetailMoviePlayRequest& request) {
        std::unique_lock lock{mutex_};
        request_ = request;
        claimed_ = false;
        completed_ = false;
        condition_.notify_all();
        condition_.wait(lock, [this] { return completed_; });
        request_.reset();
    }

    [[nodiscard]] std::optional<stuntmaster::game::RetailMoviePlayRequest>
    take() {
        std::lock_guard lock{mutex_};
        if (!request_ || claimed_) {
            return std::nullopt;
        }
        claimed_ = true;
        return request_;
    }

    void complete() {
        std::lock_guard lock{mutex_};
        completed_ = true;
        condition_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    std::optional<stuntmaster::game::RetailMoviePlayRequest> preparation_;
    std::optional<stuntmaster::game::RetailMoviePlayRequest> request_;
    bool transition_pending_{};
    bool transition_claimed_{};
    bool transition_completed_{};
    bool preparation_claimed_{};
    bool preparation_completed_{};
    bool claimed_{};
    bool completed_{};
};

inline std::string movieDiscPath(std::string path) {
    std::ranges::replace(path, '\\', '/');
    if (path.find('/') == std::string::npos) {
        path = "FE/MOVIES/" + path;
    }
    return path;
}

} // namespace stuntmaster::app
