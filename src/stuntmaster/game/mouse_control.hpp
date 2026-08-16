#pragma once

#include "stuntmaster/game/retail_patch.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace stuntmaster::psx {
class R3000Runtime;
}

namespace stuntmaster::game {

// The values are also published to unused bytes in retail's direct-pad
// buffer. Keep them stable: the guest trampolines read them directly.
enum class MouseMovementMode : std::uint8_t {
    off = 0U,
    camera_relative = 1U,
};

enum class MouseButton : std::uint8_t {
    none,
    left,
    right,
    middle,
    x1,
    x2,
};

// These indices are retail InputManager's logical primary-action bits, not
// physical PlayStation buttons. The live controller layout maps them to the
// physical pad word at the point of use.
enum class PrimaryAction : std::uint8_t {
    status = 0U,
    strafe = 1U,
    counter = 2U,
    dive_roll = 3U,
    kick = 4U,
    grab = 5U,
    jump = 6U,
    punch = 7U,
};

inline constexpr std::size_t primary_action_count = 8U;
struct MouseControlConfig {
    std::array<MouseButton, primary_action_count> actions{
        MouseButton::none,  // status
        MouseButton::none,  // strafe
        MouseButton::none,  // counter
        MouseButton::none,  // dive roll
        MouseButton::right, // kick
        MouseButton::none,  // grab
        MouseButton::none,  // jump
        MouseButton::left,  // punch
    };
    MouseMovementMode initial_mode{MouseMovementMode::camera_relative};
    // Degrees per second. The controller converts these to retail's 16-bit
    // turn and applies them at the current emulated VBlank rate.
    std::uint32_t maximum_turn_rate{720U};
    std::uint32_t turn_acceleration{10'000U};
};

struct MouseGameplayContext {
    std::uint32_t player{};
    std::uint32_t player_yaw{};
    std::uint32_t travel_yaw{};
    std::uint32_t camera_yaw{};
    std::uint32_t action_state{};
    std::array<std::uint8_t, primary_action_count> physical_to_logical{};
};

enum class MouseHeadingSplitKind : std::uint8_t {
    aligned,
    free_lease,
    retail_owned,
    committed_context,
};

struct MouseHeadingDiagnostic {
    MouseHeadingSplitKind kind{MouseHeadingSplitKind::aligned};
    std::uint32_t action_state{};
    std::uint16_t body_yaw{};
    std::uint16_t travel_yaw{};
    std::int16_t signed_delta{};

    [[nodiscard]] constexpr bool split() const noexcept {
        return kind != MouseHeadingSplitKind::aligned;
    }
    friend constexpr bool operator==(
        const MouseHeadingDiagnostic&,
        const MouseHeadingDiagnostic&) = default;
};

[[nodiscard]] MouseHeadingDiagnostic mouseHeadingDiagnostic(
    const MouseGameplayContext& context) noexcept;

[[nodiscard]] std::optional<MouseGameplayContext> readMouseGameplayContext(
    const psx::R3000Runtime& runtime,
    bool photo_mode_active = false) noexcept;

// Combines direct semantic mouse actions with the current active-low physical
// pad. Invalid/non-permutation maps fail closed and leave the pad unchanged.
[[nodiscard]] std::uint16_t applySemanticMouseActions(
    std::uint16_t active_low_buttons,
    std::span<const bool, primary_action_count> semantic_actions,
    std::span<const std::uint8_t, primary_action_count>
        physical_to_logical) noexcept;

class MouseYawController final {
public:
    explicit MouseYawController(
        MouseControlConfig config = MouseControlConfig{}) noexcept;
    void setMode(MouseMovementMode mode) noexcept;
    [[nodiscard]] MouseMovementMode mode() const noexcept { return mode_; }
    [[nodiscard]] MouseMovementMode cycleMode() noexcept;
    void update(
        const std::optional<MouseGameplayContext>& context,
        std::int32_t mouse_x,
        std::int32_t mouse_y,
        std::uint32_t vblank_rate = 60U) noexcept;
    void abandon() noexcept;
    [[nodiscard]] bool accepted() const noexcept {
        return synchronized_ && mode_ != MouseMovementMode::off;
    }
    [[nodiscard]] std::uint32_t desiredYaw() const noexcept { return yaw_; }

private:
    [[nodiscard]] static bool freeFacingLease(
        std::uint32_t action_state) noexcept;

    MouseMovementMode mode_{MouseMovementMode::camera_relative};
    std::uint32_t yaw_{};
    double continuous_yaw_{};
    double target_yaw_{};
    double last_gesture_yaw_{};
    double angular_velocity_{};
    std::uint32_t maximum_turn_rate_{720U};
    std::uint32_t turn_acceleration_{10'000U};
    bool synchronized_{};
    bool lease_active_{};
    bool have_target_{};
    bool gesture_active_{};
};

[[nodiscard]] std::optional<MouseButton> parseMouseButton(
    std::string_view value) noexcept;
[[nodiscard]] std::string_view mouseButtonName(MouseButton button) noexcept;
[[nodiscard]] std::optional<MouseMovementMode> parseMouseMovementMode(
    std::string_view value) noexcept;
[[nodiscard]] std::string_view mouseMovementModeName(
    MouseMovementMode mode) noexcept;

// Installs or removes the complete guest patch set transactionally. In
// addition to each displaced word, installation verifies the surrounding
// retail windows.
// Already-normalized stock and already-installed states are accepted, making
// this suitable for quick-save normalization.
[[nodiscard]] bool setMouseControlPatches(
    psx::R3000Runtime& runtime, bool enabled) noexcept;

} // namespace stuntmaster::game
