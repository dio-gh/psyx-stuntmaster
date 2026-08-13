#pragma once

#include <array>
#include <cstdint>

namespace stuntmaster::psx {
class R3000Runtime;
}

namespace stuntmaster::game {

enum FreeCameraMovement : std::uint8_t {
    free_camera_forward = 1U << 0U,
    free_camera_backward = 1U << 1U,
    free_camera_left = 1U << 2U,
    free_camera_right = 1U << 3U,
    free_camera_up = 1U << 4U,
    free_camera_down = 1U << 5U,
    free_camera_fast = 1U << 6U,
};

struct FreeCameraInput {
    std::uint8_t movement{};
    std::int32_t mouse_x{};
    std::int32_t mouse_y{};
    // Signed controller axes after presentation-layer deadzone shaping.
    // Movement is local right/up/forward; look follows mouse X/Y convention.
    std::int16_t controller_right{};
    std::int16_t controller_up{};
    std::int16_t controller_forward{};
    std::int16_t controller_look_x{};
    std::int16_t controller_look_y{};
};

enum class FreeCameraResult : std::uint8_t {
    unchanged,
    enabled,
    disabled,
    unavailable,
};

// Host-owned controller for retail's guest camera object. It never replaces
// the renderer or constructs a host view matrix: it puts Camera's existing
// OrderHandler into its built-in "skip" state, updates the retail position
// and Euler fields at a VBlank boundary, and lets Camera::Move/Update build the
// ordinary guest matrix. The original OrderHandler, flags, and movement times
// are restored on exit.
class FreeCameraController final {
public:
    [[nodiscard]] bool active() const noexcept { return active_; }

    [[nodiscard]] FreeCameraResult toggle(
        psx::R3000Runtime& runtime,
        bool gameplay_state) noexcept;
    [[nodiscard]] FreeCameraResult update(
        psx::R3000Runtime& runtime,
        const FreeCameraInput& input,
        std::uint32_t vblank_rate,
        bool gameplay_state) noexcept;

    // Free camera is an ephemeral host tool, not quick-save state. Normalize a
    // copied runtime before encoding it so loading a save can never strand the
    // camera in the host-owned skip mode.
    [[nodiscard]] bool normalizeSavedRuntime(
        psx::R3000Runtime& runtime) const noexcept;

    // Used immediately before replacing the whole running machine with a
    // normalized quick-save candidate.
    void abandon() noexcept;

private:
    struct RetailCameraState {
        std::uint16_t order_this_offset{};
        std::uint16_t order_mode_index{};
        std::uint32_t order_function{};
        std::uint32_t flags{};
        std::array<std::uint32_t, 3U> movement_time{};
    };

    [[nodiscard]] bool enable(psx::R3000Runtime& runtime) noexcept;
    [[nodiscard]] bool restore(psx::R3000Runtime& runtime) const noexcept;
    [[nodiscard]] bool writePose(psx::R3000Runtime& runtime) const noexcept;

    RetailCameraState retail_state_{};
    std::uint32_t camera_{};
    std::array<double, 3U> position_{};
    std::array<std::uint32_t, 3U> angles_{};
    bool active_{};
};

} // namespace stuntmaster::game
