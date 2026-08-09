#include "stuntmaster/psx/gpu_polygon_limits.hpp"

#include <algorithm>
#include <bit>
#include <limits>

namespace stuntmaster::psx {
namespace {

[[nodiscard]] std::int32_t signed11(std::uint32_t value) noexcept {
    value &= 0x7FFU;
    if ((value & 0x400U) != 0U) {
        value |= 0xFFFFF800U;
    }
    return std::bit_cast<std::int32_t>(value);
}

} // namespace

bool gpuPolygonExceedsDrawingLimits(
    std::span<const std::uint32_t> packet) noexcept {
    if (packet.empty()) {
        return false;
    }
    const auto opcode = static_cast<std::uint8_t>(packet.front() >> 24U);
    if (opcode < 0x20U || opcode > 0x3FU) {
        return false;
    }

    const auto vertex_count = (opcode & 0x08U) != 0U ? 4U : 3U;
    const auto textured = (opcode & 0x04U) != 0U;
    const auto gouraud = (opcode & 0x10U) != 0U;
    auto min_x = std::numeric_limits<std::int32_t>::max();
    auto min_y = std::numeric_limits<std::int32_t>::max();
    auto max_x = std::numeric_limits<std::int32_t>::min();
    auto max_y = std::numeric_limits<std::int32_t>::min();
    std::size_t index = 1U;
    for (std::uint32_t vertex = 0U; vertex < vertex_count; ++vertex) {
        if (vertex != 0U && gouraud) {
            ++index;
        }
        if (index >= packet.size()) {
            return false;
        }
        const auto x = signed11(packet[index]);
        const auto y = signed11(packet[index] >> 16U);
        min_x = std::min(min_x, x);
        min_y = std::min(min_y, y);
        max_x = std::max(max_x, x);
        max_y = std::max(max_y, y);
        ++index;
        if (textured) {
            ++index;
        }
    }

    return max_x - min_x > gpu_maximum_polygon_span_x ||
        max_y - min_y > gpu_maximum_polygon_span_y;
}

} // namespace stuntmaster::psx
