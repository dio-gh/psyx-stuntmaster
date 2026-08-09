#pragma once

#include <cstdint>
#include <span>

namespace stuntmaster::psx {

inline constexpr std::int32_t gpu_maximum_polygon_span_x = 1023;
inline constexpr std::int32_t gpu_maximum_polygon_span_y = 511;

// The PS1 GPU rejects polygons whose screen-space extent exceeds its drawing
// limits. Returns false for non-polygon and truncated GP0 packets.
[[nodiscard]] bool gpuPolygonExceedsDrawingLimits(
    std::span<const std::uint32_t> packet) noexcept;

} // namespace stuntmaster::psx
