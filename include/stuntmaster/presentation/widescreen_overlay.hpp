#pragma once

#include <cstdint>
#include <vector>

namespace stuntmaster::presentation {

// Horizontal PS1 screen-space edges that map to the host window edges under
// PsyCross's PGXP widescreen projection. `right` is the far edge, matching a
// variable rectangle's x + width and a quad's right-hand vertex coordinate.
struct WidescreenOverlayBounds {
    std::int32_t left{};
    std::int32_t right{512};
};

[[nodiscard]] WidescreenOverlayBounds widescreenOverlayBounds(
    std::uint32_t window_width,
    std::uint32_t window_height) noexcept;

// Extend only an untextured dark primitive, or neutral grayscale
// semi-transparent primitive, that already spans the complete authored display
// width. This catches cinematic bars and fades while leaving HUD rectangles,
// coloured effects, textures, and ordinary world polygons untouched. The
// packet is raw GP0 data; call this before converting positions to PsyCross's
// PGXP fields.
// A true result also means the caller must not interpolate its positions from
// the previous raw packet: those coordinates are still 4:3 and would make the
// extended edges repeatedly grow from the centre and snap back.
[[nodiscard]] bool extendDarkOverlayToWidescreen(
    std::vector<std::uint32_t>& packet,
    std::int32_t draw_offset_x,
    std::int32_t origin_x,
    std::uint32_t display_width,
    WidescreenOverlayBounds bounds) noexcept;

} // namespace stuntmaster::presentation
