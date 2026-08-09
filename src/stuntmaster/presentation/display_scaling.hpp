#pragma once

#include <cstdint>

namespace stuntmaster::presentation {

struct DisplayViewport {
    std::uint32_t x{};
    std::uint32_t y{};
    std::uint32_t width{};
    std::uint32_t height{};

    [[nodiscard]] constexpr bool operator==(
        const DisplayViewport&) const noexcept = default;
};

// Fit a completed image inside a destination without changing its aspect.
// Odd leftover pixels remain on the right or top edge.
[[nodiscard]] constexpr DisplayViewport fitDisplayViewport(
    std::uint32_t source_width,
    std::uint32_t source_height,
    std::uint32_t destination_width,
    std::uint32_t destination_height) noexcept {
    if (source_width == 0U || source_height == 0U ||
        destination_width == 0U || destination_height == 0U) {
        return {};
    }

    auto width = destination_width;
    auto height = static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(destination_width) * source_height /
        source_width);
    if (height > destination_height) {
        height = destination_height;
        width = static_cast<std::uint32_t>(
            static_cast<std::uint64_t>(destination_height) * source_width /
            source_height);
    }
    return {
        (destination_width - width) / 2U,
        (destination_height - height) / 2U,
        width,
        height,
    };
}

} // namespace stuntmaster::presentation
