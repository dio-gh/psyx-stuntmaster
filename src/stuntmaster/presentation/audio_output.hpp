#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace stuntmaster::presentation {

// An OpenAL streaming sink for the SPU's mixed output.
//
// PsyCross opens its own OpenAL device, but only from its `SpuInit`, which
// retail never calls -- it links its own copy of libspu and writes hardware
// registers instead. So the device is unclaimed and this owns it outright
// rather than sharing a context.
//
// Failure to open a device is not fatal. A machine with no sound card should
// still run the game, so every entry point becomes a no-op and `ready()`
// reports what happened.
class AudioOutput final {
public:
    static constexpr std::uint32_t sample_rate = 44100U;

    AudioOutput();
    ~AudioOutput();

    AudioOutput(const AudioOutput&) = delete;
    AudioOutput& operator=(const AudioOutput&) = delete;

    [[nodiscard]] bool ready() const noexcept { return ready_; }

    // Hand over interleaved stereo samples. Buffers the device has finished
    // with are recycled here, so this must be called regularly even when there
    // is nothing new to submit.
    void submit(std::span<const std::int16_t> samples);

    // End the current guest-audio timeline immediately and recycle every
    // queued OpenAL buffer. The next submit starts a fresh timeline.
    void stopAndDiscardQueued() noexcept;

    // Reclaim processed buffers and report how many remain queued. Calling
    // this regularly also services a timeline that is only draining.
    [[nodiscard]] std::size_t queuedBufferCount() noexcept;
    [[nodiscard]] bool canAcceptBuffer() noexcept;

    // How many submissions found no free buffer, and how many times playback
    // ran dry. Both mean the mixer is not keeping up with the device.
    [[nodiscard]] std::uint64_t starvedCount() const noexcept {
        return starved_;
    }
    [[nodiscard]] std::uint64_t droppedSubmissions() const noexcept {
        return dropped_submissions_;
    }

private:
    bool ready_{};
    // Opaque so that no OpenAL header reaches a caller; only this translation
    // unit is allowed to know about them.
    void* device_{};
    void* context_{};
    std::uint32_t source_{};
    std::vector<std::uint32_t> free_buffers_;
    std::uint64_t starved_{};
    std::uint64_t dropped_submissions_{};
};

} // namespace stuntmaster::presentation
