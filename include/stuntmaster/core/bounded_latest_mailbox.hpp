#pragma once

#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

namespace stuntmaster::core {

template <typename Value, std::size_t Capacity>
class BoundedLatestMailbox final {
    static_assert(Capacity != 0U);

public:
    struct Statistics {
        std::size_t published{};
        std::size_t dropped_on_publish{};
        std::size_t skipped_on_consume{};
    };

    void publish(Value value) {
        std::optional<Value> obsolete;
        {
            std::scoped_lock lock{mutex_};
            if (values_.size() == Capacity) {
                obsolete.emplace(std::move(values_.front()));
                values_.pop_front();
                ++statistics_.dropped_on_publish;
            }
            values_.push_back(std::move(value));
            ++statistics_.published;
        }
    }

    [[nodiscard]] std::optional<Value> takeLatest() {
        std::deque<Value> obsolete;
        std::optional<Value> result;
        {
            std::scoped_lock lock{mutex_};
            if (values_.empty()) {
                return std::nullopt;
            }
            statistics_.skipped_on_consume += values_.size() - 1U;
            result.emplace(std::move(values_.back()));
            values_.pop_back();
            obsolete.swap(values_);
        }
        return result;
    }

    [[nodiscard]] std::size_t discardAll() {
        std::deque<Value> obsolete;
        std::size_t discarded{};
        {
            std::scoped_lock lock{mutex_};
            discarded = values_.size();
            obsolete.swap(values_);
        }
        return discarded;
    }

    [[nodiscard]] Statistics statistics() const {
        std::scoped_lock lock{mutex_};
        return statistics_;
    }

private:
    mutable std::mutex mutex_;
    std::deque<Value> values_;
    Statistics statistics_;
};

} // namespace stuntmaster::core
