#include "stuntmaster/presentation/widescreen_overlay.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <ranges>

namespace stuntmaster::presentation {
namespace {

constexpr std::int32_t retail_width = 512;
constexpr std::int32_t retail_centre = retail_width / 2;
constexpr double retail_pixel_aspect = 3.0 / 4.0;
constexpr std::int32_t edge_tolerance = 2;
constexpr std::uint32_t maximum_dark_channel = 16U;

[[nodiscard]] std::int32_t signed11(std::uint32_t value) noexcept {
    value &= 0x7FFU;
    if ((value & 0x400U) != 0U) {
        value |= 0xFFFFF800U;
    }
    return std::bit_cast<std::int32_t>(value);
}

[[nodiscard]] bool nearBlack(std::uint32_t color) noexcept {
    return (color & 0xFFU) <= maximum_dark_channel &&
        ((color >> 8U) & 0xFFU) <= maximum_dark_channel &&
        ((color >> 16U) & 0xFFU) <= maximum_dark_channel;
}

[[nodiscard]] bool neutralGray(std::uint32_t color) noexcept {
    const auto red = color & 0xFFU;
    const auto green = (color >> 8U) & 0xFFU;
    const auto blue = (color >> 16U) & 0xFFU;
    const auto minimum = std::min({red, green, blue});
    const auto maximum = std::max({red, green, blue});
    // Retail's fade and cinematic bars animate a semi-transparent neutral
    // intensity rather than keeping their primitive RGB near zero.
    return maximum - minimum <= 2U;
}

[[nodiscard]] bool overlayColor(
    std::uint32_t color,
    bool semi_transparent) noexcept {
    return nearBlack(color) || (semi_transparent && neutralGray(color));
}

void replaceX(std::uint32_t& position, std::int32_t x) noexcept {
    position = (position & 0xFFFF0000U) |
        (static_cast<std::uint32_t>(x) & 0xFFFFU);
}

[[nodiscard]] std::int32_t rawX(
    std::int32_t translated_x,
    std::int32_t draw_offset_x,
    std::int32_t origin_x) noexcept {
    return translated_x + origin_x - draw_offset_x;
}

} // namespace

WidescreenOverlayBounds widescreenOverlayBounds(
    std::uint32_t window_width,
    std::uint32_t window_height) noexcept {
    if (window_width == 0U || window_height == 0U) {
        return {};
    }
    const auto aspect = static_cast<double>(window_width) /
        static_cast<double>(window_height);
    const auto half_width = 0.5 * retail_width * aspect *
        retail_pixel_aspect;
    auto right = static_cast<long>(retail_centre + half_width + 0.5);
    right = std::clamp(
        right,
        static_cast<long>(retail_width),
        1023L);
    return {
        retail_width - static_cast<std::int32_t>(right),
        static_cast<std::int32_t>(right)};
}

bool extendDarkOverlayToWidescreen(
    std::vector<std::uint32_t>& packet,
    std::int32_t draw_offset_x,
    std::int32_t origin_x,
    std::uint32_t display_width,
    WidescreenOverlayBounds bounds) noexcept {
    if (packet.empty() || display_width == 0U ||
        bounds.left >= 0 || bounds.right <= retail_width) {
        return false;
    }
    const auto opcode = static_cast<std::uint8_t>(packet.front() >> 24U);
    const auto semi_transparent = (opcode & 0x02U) != 0U;
    const auto translatedX = [&](std::uint32_t position) {
        return signed11(position) + draw_offset_x - origin_x;
    };
    const auto desired_left =
        rawX(bounds.left, draw_offset_x, origin_x);
    const auto desired_right =
        rawX(bounds.right, draw_offset_x, origin_x);

    // Variable-size, untextured TILE (opaque or semi-transparent).
    if (opcode >= 0x60U && opcode <= 0x63U && packet.size() >= 3U &&
        overlayColor(packet.front(), semi_transparent)) {
        const auto left = translatedX(packet[1]);
        const auto width = static_cast<std::int32_t>(packet[2] & 0xFFFFU);
        const auto right = left + width;
        if (left > edge_tolerance ||
            right < static_cast<std::int32_t>(display_width) -
                edge_tolerance) {
            return false;
        }
        const auto extended_width = desired_right - desired_left;
        if (extended_width <= 0 ||
            extended_width > std::numeric_limits<std::uint16_t>::max()) {
            return false;
        }
        replaceX(packet[1], desired_left);
        packet[2] = (packet[2] & 0xFFFF0000U) |
            static_cast<std::uint16_t>(extended_width);
        return true;
    }

    // Untextured flat/Gouraud quads. Textured bit 2 must be clear and quad bit
    // 3 set. Semi-transparency (bit 1) is intentionally accepted for fades.
    if (opcode < 0x20U || opcode > 0x3FU ||
        (opcode & 0x04U) != 0U || (opcode & 0x08U) == 0U) {
        return false;
    }
    const auto gouraud = (opcode & 0x10U) != 0U;
    const std::array<std::size_t, 4> positions =
        gouraud ? std::array<std::size_t, 4>{1U, 3U, 5U, 7U}
                : std::array<std::size_t, 4>{1U, 2U, 3U, 4U};
    const std::array<std::size_t, 4> colors{0U, 2U, 4U, 6U};
    if (positions.back() >= packet.size() ||
        (!gouraud && !overlayColor(packet.front(), semi_transparent))) {
        return false;
    }
    if (gouraud && std::ranges::any_of(
            colors,
            [&](std::size_t index) {
                return index >= packet.size() ||
                    !overlayColor(packet[index], semi_transparent);
            })) {
        return false;
    }

    std::array<std::int32_t, 4> x{};
    std::ranges::transform(
        positions,
        x.begin(),
        [&](std::size_t index) { return translatedX(packet[index]); });
    const auto [minimum, maximum] = std::ranges::minmax_element(x);
    if (*minimum > edge_tolerance ||
        *maximum < static_cast<std::int32_t>(display_width) -
            edge_tolerance) {
        return false;
    }
    const auto centre = (*minimum + *maximum) / 2;
    auto left_vertices = 0U;
    auto right_vertices = 0U;
    for (const auto coordinate : x) {
        left_vertices += coordinate <= centre ? 1U : 0U;
        right_vertices += coordinate > centre ? 1U : 0U;
    }
    if (left_vertices != 2U || right_vertices != 2U) {
        return false;
    }
    for (std::size_t index = 0U; index < positions.size(); ++index) {
        replaceX(
            packet[positions[index]],
            x[index] <= centre ? desired_left : desired_right);
    }
    return true;
}

} // namespace stuntmaster::presentation
