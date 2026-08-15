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
    character_relative = 2U,
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
inline constexpr std::int32_t default_mouse_yaw_units_per_pixel = 20;

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
    std::int32_t yaw_units_per_pixel{default_mouse_yaw_units_per_pixel};
};

struct MouseGameplayContext {
    std::uint32_t player{};
    std::uint32_t player_yaw{};
    std::array<std::uint8_t, primary_action_count> physical_to_logical{};
};

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
    void setMode(MouseMovementMode mode) noexcept;
    [[nodiscard]] MouseMovementMode mode() const noexcept { return mode_; }
    [[nodiscard]] MouseMovementMode cycleMode() noexcept;
    void update(
        const std::optional<MouseGameplayContext>& context,
        std::int32_t mouse_x,
        std::int32_t sensitivity = default_mouse_yaw_units_per_pixel) noexcept;
    void abandon() noexcept { synchronized_ = false; }
    [[nodiscard]] bool accepted() const noexcept {
        return synchronized_ && mode_ != MouseMovementMode::off;
    }
    [[nodiscard]] std::uint32_t desiredYaw() const noexcept { return yaw_; }

private:
    MouseMovementMode mode_{MouseMovementMode::camera_relative};
    std::uint32_t yaw_{};
    bool synchronized_{};
};

[[nodiscard]] std::optional<MouseButton> parseMouseButton(
    std::string_view value) noexcept;
[[nodiscard]] std::string_view mouseButtonName(MouseButton button) noexcept;
[[nodiscard]] std::optional<MouseMovementMode> parseMouseMovementMode(
    std::string_view value) noexcept;
[[nodiscard]] std::string_view mouseMovementModeName(
    MouseMovementMode mode) noexcept;

[[nodiscard]] const RetailTrampoline& mouseActionTrampoline() noexcept;
[[nodiscard]] const RetailTrampoline& mouseTargetTrampoline() noexcept;

// Installs or removes both guest patches transactionally. In addition to the
// displaced word, installation verifies the surrounding retail windows.
// Already-normalized stock and already-installed states are accepted, making
// this suitable for quick-save normalization.
[[nodiscard]] bool setMouseControlPatches(
    psx::R3000Runtime& runtime, bool enabled) noexcept;

} // namespace stuntmaster::game
