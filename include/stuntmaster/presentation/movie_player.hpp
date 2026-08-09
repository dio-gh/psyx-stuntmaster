#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>

namespace stuntmaster::presentation {

class PsyCrossPresenter;

enum class MoviePlaybackResult {
    completed,
    skipped,
};

inline constexpr std::uint16_t movie_start_button = 0x0008U;

using MovieGuestAudioAdvance =
    std::function<void(std::chrono::steady_clock::duration elapsed)>;

[[nodiscard]] constexpr bool movieStartPressed(
    std::uint16_t previous,
    std::uint16_t current) noexcept {
    return (previous & movie_start_button) != 0U &&
        (current & movie_start_button) == 0U;
}

[[nodiscard]] MoviePlaybackResult playMovie(
    PsyCrossPresenter& presenter,
    std::string_view path,
    std::vector<std::byte> raw_sectors,
    bool audio_enabled,
    MovieGuestAudioAdvance advance_guest_audio = {});

} // namespace stuntmaster::presentation
