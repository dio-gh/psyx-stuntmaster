#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <vector>

namespace stuntmaster::core {

// A fixed-capacity FIFO of interleaved stereo samples, written by the guest
// worker and drained by whatever owns the audio device.
//
// A mutex is the right tool at this rate: the producer takes it sixty times a
// second and the consumer a few hundred, so contention is not a consideration
// and the alternative would be a lock-free structure whose correctness costs
// more to establish than it saves.
//
// On overflow the oldest samples are dropped rather than the newest. A full
// ring means the consumer has stalled, and when it resumes the useful thing to
// play is the most recent audio, not a delayed backlog that would then be
// permanently behind the picture.
class AudioRing final {
public:
    explicit AudioRing(std::size_t capacity_samples)
        : data_(capacity_samples) {}

    void push(std::span<const std::int16_t> samples) {
        const std::lock_guard lock{mutex_};
        for (const auto sample : samples) {
            data_[write_] = sample;
            write_ = (write_ + 1U) % data_.size();
            if (count_ == data_.size()) {
                read_ = (read_ + 1U) % data_.size();
                ++dropped_;
            } else {
                ++count_;
            }
        }
    }

    // Fills as much of `out` as is available and returns how many samples were
    // written, which may be zero.
    [[nodiscard]] std::size_t pop(std::span<std::int16_t> out) {
        const std::lock_guard lock{mutex_};
        const auto taken = std::min(out.size(), count_);
        for (std::size_t index = 0U; index < taken; ++index) {
            out[index] = data_[read_];
            read_ = (read_ + 1U) % data_.size();
        }
        count_ -= taken;
        return taken;
    }

    // Drop samples that belong to an audio timeline which has just ended,
    // such as the guest mix when native movie playback takes ownership.
    [[nodiscard]] std::size_t discardAll() {
        const std::lock_guard lock{mutex_};
        const auto discarded = count_;
        read_ = write_;
        count_ = 0U;
        return discarded;
    }

    [[nodiscard]] std::size_t available() const {
        const std::lock_guard lock{mutex_};
        return count_;
    }
    [[nodiscard]] std::uint64_t dropped() const {
        const std::lock_guard lock{mutex_};
        return dropped_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::int16_t> data_;
    std::size_t read_{};
    std::size_t write_{};
    std::size_t count_{};
    std::uint64_t dropped_{};
};

} // namespace stuntmaster::core
