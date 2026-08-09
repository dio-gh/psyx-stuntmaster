#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace stuntmaster::presentation {

struct DebugOverlayState {
    bool enabled{};
    // Applied only when the overlay first becomes available. Runtime key
    // toggles subsequently own visibility.
    bool initially_visible{};
    // The requested game-loop rate, then the schedule actually in force. They
    // differ while a load holds the guest at retail's cadence.
    std::uint32_t guest_update_rate{};
    std::uint32_t guest_vblank_rate{};
    std::uint32_t retime_divisor{};
    // Guest VBlanks produced against wall clock, as a percentage. The guest
    // is interpreted, so a schedule that asks for more instructions a second
    // than the host can deliver simply runs slow — audio and gameplay
    // together. Zero means not yet measured.
    std::uint32_t guest_speed_percent{};
    std::uint64_t guest_ticks{};
    std::uint64_t guest_vblanks{};
    std::uint64_t guest_frames{};
    std::uint32_t presentation_rate{};
    bool high_frequency_requested{};
    bool high_frequency_active{};
    bool retime_motion_active{};
    bool retime_clock_active{};
    bool widescreen_cull_active{};
    // Host-owned retime hooks currently active in the guest. The hook table is
    // rebuilt per frame; overlay-resident hooks are fingerprint-gated, so a
    // hook that is armed can still be absent — either because its overlay is
    // not loaded or because its window did not match. Reporting the live count
    // separates "this object class is not retimed here" from "this object
    // class is retimed and still looks wrong".
    std::uint32_t retime_hooks_armed{};
    std::uint32_t retime_hooks_live{};
};

struct DebugOverlayBitmap {
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<unsigned char> rgba;
};

class DebugOverlayVisibilityToggle final {
public:
    [[nodiscard]] constexpr bool update(
        bool available,
        bool visible,
        bool pressed) noexcept {
        if (available && pressed && !previously_pressed_) {
            visible = !visible;
        }
        previously_pressed_ = pressed;
        return available && visible;
    }

private:
    bool previously_pressed_{};
};

[[nodiscard]] std::vector<std::string> debugOverlayRows(
    const DebugOverlayState& state);

// Rasterize a compact host-owned 5x7 font into a top-down opaque RGBA image.
// This stays in the portable presentation layer; the platform adapter only
// uploads and blits the completed bitmap.
[[nodiscard]] DebugOverlayBitmap rasterizeDebugOverlay(
    const DebugOverlayState& state,
    std::uint32_t scale);

// A one-line host notification uses the same deliberately tiny font as the
// diagnostics panel, but remains available when the debug overlay is off.
[[nodiscard]] DebugOverlayBitmap rasterizeNotificationOverlay(
    std::string_view message,
    std::uint32_t scale);

} // namespace stuntmaster::presentation
