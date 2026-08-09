#include "stuntmaster/presentation/debug_overlay.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string_view>

namespace stuntmaster::presentation {
namespace {

using Glyph = std::array<std::uint8_t, 7U>;

constexpr Glyph glyph(char character) noexcept {
    switch (character) {
    case 'A': return {0x0EU, 0x11U, 0x11U, 0x1FU, 0x11U, 0x11U, 0x11U};
    case 'B': return {0x1EU, 0x11U, 0x11U, 0x1EU, 0x11U, 0x11U, 0x1EU};
    case 'C': return {0x0EU, 0x11U, 0x10U, 0x10U, 0x10U, 0x11U, 0x0EU};
    case 'D': return {0x1EU, 0x11U, 0x11U, 0x11U, 0x11U, 0x11U, 0x1EU};
    case 'E': return {0x1FU, 0x10U, 0x10U, 0x1EU, 0x10U, 0x10U, 0x1FU};
    case 'F': return {0x1FU, 0x10U, 0x10U, 0x1EU, 0x10U, 0x10U, 0x10U};
    case 'G': return {0x0EU, 0x11U, 0x10U, 0x17U, 0x11U, 0x11U, 0x0FU};
    case 'H': return {0x11U, 0x11U, 0x11U, 0x1FU, 0x11U, 0x11U, 0x11U};
    case 'I': return {0x0EU, 0x04U, 0x04U, 0x04U, 0x04U, 0x04U, 0x0EU};
    case 'J': return {0x01U, 0x01U, 0x01U, 0x01U, 0x11U, 0x11U, 0x0EU};
    case 'K': return {0x11U, 0x12U, 0x14U, 0x18U, 0x14U, 0x12U, 0x11U};
    case 'L': return {0x10U, 0x10U, 0x10U, 0x10U, 0x10U, 0x10U, 0x1FU};
    case 'M': return {0x11U, 0x1BU, 0x15U, 0x15U, 0x11U, 0x11U, 0x11U};
    case 'N': return {0x11U, 0x19U, 0x15U, 0x13U, 0x11U, 0x11U, 0x11U};
    case 'O': return {0x0EU, 0x11U, 0x11U, 0x11U, 0x11U, 0x11U, 0x0EU};
    case 'P': return {0x1EU, 0x11U, 0x11U, 0x1EU, 0x10U, 0x10U, 0x10U};
    case 'Q': return {0x0EU, 0x11U, 0x11U, 0x11U, 0x15U, 0x12U, 0x0DU};
    case 'R': return {0x1EU, 0x11U, 0x11U, 0x1EU, 0x14U, 0x12U, 0x11U};
    case 'S': return {0x0FU, 0x10U, 0x10U, 0x0EU, 0x01U, 0x01U, 0x1EU};
    case 'T': return {0x1FU, 0x04U, 0x04U, 0x04U, 0x04U, 0x04U, 0x04U};
    case 'U': return {0x11U, 0x11U, 0x11U, 0x11U, 0x11U, 0x11U, 0x0EU};
    case 'V': return {0x11U, 0x11U, 0x11U, 0x11U, 0x11U, 0x0AU, 0x04U};
    case 'W': return {0x11U, 0x11U, 0x11U, 0x15U, 0x15U, 0x15U, 0x0AU};
    case 'X': return {0x11U, 0x11U, 0x0AU, 0x04U, 0x0AU, 0x11U, 0x11U};
    case 'Y': return {0x11U, 0x11U, 0x0AU, 0x04U, 0x04U, 0x04U, 0x04U};
    case 'Z': return {0x1FU, 0x01U, 0x02U, 0x04U, 0x08U, 0x10U, 0x1FU};
    case '0': return {0x0EU, 0x11U, 0x13U, 0x15U, 0x19U, 0x11U, 0x0EU};
    case '1': return {0x04U, 0x0CU, 0x14U, 0x04U, 0x04U, 0x04U, 0x1FU};
    case '2': return {0x0EU, 0x11U, 0x01U, 0x02U, 0x04U, 0x08U, 0x1FU};
    case '3': return {0x1EU, 0x01U, 0x01U, 0x0EU, 0x01U, 0x01U, 0x1EU};
    case '4': return {0x02U, 0x06U, 0x0AU, 0x12U, 0x1FU, 0x02U, 0x02U};
    case '5': return {0x1FU, 0x10U, 0x10U, 0x1EU, 0x01U, 0x01U, 0x1EU};
    case '6': return {0x0EU, 0x10U, 0x10U, 0x1EU, 0x11U, 0x11U, 0x0EU};
    case '7': return {0x1FU, 0x01U, 0x02U, 0x04U, 0x08U, 0x08U, 0x08U};
    case '8': return {0x0EU, 0x11U, 0x11U, 0x0EU, 0x11U, 0x11U, 0x0EU};
    case '9': return {0x0EU, 0x11U, 0x11U, 0x0FU, 0x01U, 0x01U, 0x0EU};
    case '%': return {0x18U, 0x18U, 0x02U, 0x04U, 0x08U, 0x03U, 0x03U};
    default: return {};
    }
}

std::string appendFlag(std::string text, std::string_view flag) {
    if (!text.empty()) {
        text.push_back(' ');
    }
    text.append(flag);
    return text;
}

DebugOverlayBitmap rasterizeRows(
    const std::vector<std::string>& rows,
    std::uint32_t scale,
    std::array<std::uint8_t, 3U> background,
    std::array<std::uint8_t, 3U> foreground) {
    if (rows.empty() || scale == 0U) {
        return {};
    }
    std::size_t longest = 0U;
    for (const auto& row : rows) {
        longest = std::max(longest, row.size());
    }
    constexpr std::uint32_t glyph_width = 5U;
    constexpr std::uint32_t glyph_height = 7U;
    constexpr std::uint32_t character_advance = 6U;
    constexpr std::uint32_t line_advance = 9U;
    const auto margin = 4U * scale;

    DebugOverlayBitmap bitmap;
    bitmap.width = margin * 2U +
        static_cast<std::uint32_t>(longest) * character_advance * scale;
    bitmap.height = margin * 2U +
        static_cast<std::uint32_t>(rows.size()) * line_advance * scale;
    bitmap.rgba.resize(
        static_cast<std::size_t>(bitmap.width) * bitmap.height * 4U);
    for (std::size_t index = 0U; index < bitmap.rgba.size(); index += 4U) {
        bitmap.rgba[index] = background[0];
        bitmap.rgba[index + 1U] = background[1];
        bitmap.rgba[index + 2U] = background[2];
        bitmap.rgba[index + 3U] = 0xFFU;
    }

    const auto light_pixel = [&](std::uint32_t x, std::uint32_t y) {
        for (std::uint32_t dy = 0U; dy < scale; ++dy) {
            for (std::uint32_t dx = 0U; dx < scale; ++dx) {
                const auto pixel = static_cast<std::size_t>(y + dy) *
                    bitmap.width + x + dx;
                auto* out = bitmap.rgba.data() + pixel * 4U;
                out[0] = foreground[0];
                out[1] = foreground[1];
                out[2] = foreground[2];
                out[3] = 0xFFU;
            }
        }
    };
    for (std::size_t row_index = 0U; row_index < rows.size(); ++row_index) {
        const auto origin_y = margin +
            static_cast<std::uint32_t>(row_index) * line_advance * scale;
        for (std::size_t character_index = 0U;
             character_index < rows[row_index].size(); ++character_index) {
            const auto shape = glyph(rows[row_index][character_index]);
            const auto origin_x = margin +
                static_cast<std::uint32_t>(character_index) *
                    character_advance * scale;
            for (std::uint32_t y = 0U; y < glyph_height; ++y) {
                for (std::uint32_t x = 0U; x < glyph_width; ++x) {
                    if ((shape[y] & (1U << (glyph_width - 1U - x))) != 0U) {
                        light_pixel(
                            origin_x + x * scale, origin_y + y * scale);
                    }
                }
            }
        }
    }
    return bitmap;
}

} // namespace

std::vector<std::string> debugOverlayRows(const DebugOverlayState& state) {
    constexpr auto renderer = "PSYCROSS";
    // The requested rate and whether it is currently in force. A high rate
    // waits while retail loads, so "WAIT" is a normal state, not a failure.
    const auto high_frequency_state = !state.high_frequency_requested
        ? std::string{"PATCH OFF"}
        : "PATCH " + std::to_string(state.guest_update_rate) + "HZ " +
            (state.high_frequency_active ? "ACTIVE" : "WAIT");
    auto retiming = std::string{};
    if (state.retime_motion_active) {
        retiming = appendFlag(retiming, "MOTION");
    }
    if (state.retime_clock_active) {
        retiming = appendFlag(retiming, "CLOCK");
    }
    if (retiming.empty()) {
        retiming = "NONE";
    }

    return {
        "GUEST " + std::to_string(state.guest_update_rate) +
            "HZ TICK " + std::to_string(state.guest_ticks),
        "VBLANK " + std::to_string(state.guest_vblanks) +
            " FRAME " + std::to_string(state.guest_frames),
        "HOST " + std::to_string(state.presentation_rate) + "HZ " + renderer,
        high_frequency_state,
        // The schedule in force: emulated display rate, and guest updates per
        // authored 30 Hz step. Then how much of that schedule the host is
        // actually delivering — anything under 100 means the guest is running
        // slow, which is a throughput shortfall and not a retiming fault.
        "SCHED VB " + std::to_string(state.guest_vblank_rate) + " DIV " +
            std::to_string(state.retime_divisor),
        state.guest_speed_percent == 0U
            ? std::string{"SPEED MEASURING"}
            : "SPEED " + std::to_string(state.guest_speed_percent) + "%",
        "RETIME " + retiming,
        // Live over total retime hooks. Fewer live than total is normal while
        // the loaded overlays do not contain every fingerprinted hook.
        "HOOK " + std::to_string(state.retime_hooks_live) + " OF " +
            std::to_string(state.retime_hooks_armed),
        std::string{"CULL "} +
            (state.widescreen_cull_active ? "WIDE" : "STOCK"),
    };
}

DebugOverlayBitmap rasterizeDebugOverlay(
    const DebugOverlayState& state,
    std::uint32_t scale) {
    if (!state.enabled || scale == 0U) {
        return {};
    }
    return rasterizeRows(
        debugOverlayRows(state), scale, {12U, 18U, 12U}, {180U, 255U, 180U});
}

DebugOverlayBitmap rasterizeNotificationOverlay(
    std::string_view message,
    std::uint32_t scale) {
    if (message.empty()) {
        return {};
    }
    return rasterizeRows(
        {std::string{message}}, scale, {18U, 18U, 24U}, {255U, 255U, 255U});
}

} // namespace stuntmaster::presentation
