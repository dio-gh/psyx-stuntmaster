#include "stuntmaster/game/mouse_control.hpp"

#include "stuntmaster/psx/r3000_runtime.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <string>

namespace stuntmaster::game {
namespace {

constexpr std::uint32_t game_manager_address = 0x800DD668U;
constexpr std::uint32_t game_state_handler_offset = 0x1CU;
constexpr std::uint32_t gameplay_state_handler = 0x80029C6CU;
constexpr std::uint32_t input_manager_address = 0x800DD69CU;
constexpr std::uint32_t input_map_offset = 0x2E0U;
constexpr std::uint32_t player_address = 0x800DD6B4U;
constexpr std::uint32_t player_orientation_y_offset = 0x2CU;
constexpr std::uint32_t player_behaviour_offset = 0x1B8U;
constexpr std::uint32_t behaviour_handler_offset = 0xE0U;
constexpr std::uint32_t player_user_control_handler = 0x80074F5CU;

constexpr std::uint32_t action_site = 0x80075398U;
constexpr std::uint32_t action_arena =
    psx::R3000Runtime::patch_arena_base;
constexpr std::uint32_t target_site = 0x80034010U;
constexpr std::uint32_t target_arena =
    psx::R3000Runtime::patch_arena_base + 0x200U;

constexpr std::uint32_t encodeJump(std::uint32_t target) noexcept {
    return 0x08000000U | ((target >> 2U) & 0x03FFFFFFU);
}

constexpr std::array<std::uint32_t, 8U> action_window{
    0x8E640018U, // lw a0,0x18(s3)
    0x0C0193AAU, // jal SetDesiredMoveDirection
    0x02802821U, // move a1,s4
    0x8E640018U, // lw a0,0x18(s3)
    0x0C01B3FFU, // jal RequestAction (patched site)
    0x02002821U, // move a1,s0 (site delay slot)
    0x8FBF0040U,
    0x8FB5003CU,
};

constexpr std::array<std::uint32_t, 8U> target_window{
    0x8E020100U, // lw v0,0x100(s0)
    0x00000000U,
    0x1440000BU, // bnez v0,0x80034040 (patched site)
    0x00000000U,
    0x8F8501D4U,
    0x8E020008U,
    0x8F8601D8U,
    0x8C4200ECU,
};

// PlayerUserControl has already placed the Player in a0 and the requested
// retail action in a1 (the original delay slot at the patch site). Mode 1
// preserves the camera-relative travel angle but substitutes retail's strafe
// action for Run; mode 2 rotates the travel vector from camera space into the
// new character basis. Non-movement actions and idle movement face the mouse.
constexpr std::array<std::uint32_t, 59U> action_body{
    0x3C08800EU, // lui t0,0x800e
    0x9108FA38U, // lbu t0,-0x5c8(t0): mouse mode
    0x00000000U, // R3000A load delay
    0x250BFFFFU, // addiu t3,t0,-1
    0x11600006U, // beq t3,zero,mode_camera
    0x00000000U,
    0x250BFFFEU, // addiu t3,t0,-2
    0x11600017U, // beq t3,zero,mode_character
    0x00000000U,
    encodeJump(action_arena + 57U * 4U),
    0x00000000U,
    // mode_camera
    0x3C09800EU, // lui t1,0x800e
    0x8D29FA34U, // lw t1,-0x5cc(t1): desired yaw
    0x00000000U, // R3000A load delay
    0xAC89002CU, // sw t1,0x2c(a0): orientation.y
    0x8C8A00D0U, // lw t2,0xd0(a0): moveSpeed
    0x00000000U, // R3000A load delay
    0x11400026U, // beq t2,zero,set_face
    0x00000000U,
    0x3C0B0009U,
    0x356B80DCU, // t3 = movement-action mask
    0x00AB5806U, // srlv t3,t3,a1
    0x316B0001U,
    0x11600020U, // beq t3,zero,set_face
    0x00000000U,
    0x24ABFFFEU, // addiu t3,a1,-2 (Run)
    0x1560001EU, // bne t3,zero,call
    0x00000000U,
    0x24050006U, // addiu a1,zero,6 (Strafe)
    encodeJump(action_arena + 57U * 4U),
    0x00000000U,
    // mode_character
    0x3C09800EU,
    0x8D29FA34U,
    0x00000000U, // R3000A load delay
    0xAC89002CU,
    0x8C8A00D0U,
    0x00000000U, // R3000A load delay
    0x11400012U, // beq t2,zero,set_face
    0x00000000U,
    0x3C0B0009U,
    0x356B80DCU,
    0x00AB5806U,
    0x316B0001U,
    0x1160000CU, // beq t3,zero,set_face
    0x00000000U,
    0x3C0B800EU,
    0x8D6BD734U, // lw t3,-0x28cc(t3): Game/the camera
    0x00000000U, // R3000A load delay
    0x8D6B002CU, // lw t3,0x2c(t3): camera yaw
    0x8C8C0114U, // lw t4,0x114(a0): camera-relative faceAngle
    0x00000000U, // R3000A load delay
    0x018B6023U, // subu t4,t4,t3
    0x01896021U, // addu t4,t4,t1
    0xAC8C0114U, // sw t4,0x114(a0)
    encodeJump(action_arena + 57U * 4U),
    0x00000000U,
    0xAC890114U, // set_face: sw t1,0x114(a0)
    0x0C01B3FFU, // call: jal RequestAction
    0x00000000U,
};

// Player::_Straif's stock target branch is retained in mode zero. Mouse modes
// skip acquisition and release an already-held target before continuing.
constexpr std::array<std::uint32_t, 15U> target_body{
    0x3C01800EU, // lui at,0x800e
    0x9021FA38U, // lbu at,-0x5c8(at): mouse mode
    0x00000000U, // R3000A load delay
    0x10200007U, // beq at,zero,stock
    0x00000000U,
    0x10400009U, // beq v0,zero,appended return
    0x00000000U,
    0x0C019480U, // jal ReleaseTarget
    0x02002021U, // move a0,s0
    encodeJump(0x80034040U),
    0x00000000U,
    0x14400003U, // stock: bne v0,zero,appended return
    0x00000000U,
    encodeJump(0x80034018U),
    0x00000000U,
};

[[nodiscard]] bool validGuestPointer(std::uint32_t value) noexcept {
    return value >= 0x80010000U && value <= 0x801FFFFCU;
}

[[nodiscard]] bool windowMatches(
    const psx::R3000Runtime& runtime,
    std::uint32_t address,
    std::span<const std::uint32_t> words,
    std::size_t skip_index = std::numeric_limits<std::size_t>::max()) noexcept {
    for (std::size_t index = 0U; index < words.size(); ++index) {
        if (index == skip_index) {
            continue;
        }
        std::uint32_t found = 0U;
        if (!runtime.read32(
                address + static_cast<std::uint32_t>(index * 4U), found) ||
            found != words[index]) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool trampolineInstalled(
    const psx::R3000Runtime& runtime,
    const RetailTrampoline& patch) noexcept {
    std::uint32_t word = 0U;
    if (!runtime.read32(patch.site, word) ||
        word != encodeJump(patch.address)) {
        return false;
    }
    for (std::size_t index = 0U; index < patch.body.size(); ++index) {
        if (!runtime.read32(
                patch.address + static_cast<std::uint32_t>(index * 4U), word) ||
            word != patch.body[index]) {
            return false;
        }
    }
    return runtime.read32(
               patch.address +
                   static_cast<std::uint32_t>(patch.body.size() * 4U),
               word) &&
        word == encodeJump(patch.return_address) &&
        runtime.read32(
            patch.address +
                static_cast<std::uint32_t>((patch.body.size() + 1U) * 4U),
            word) &&
        word == 0U;
}

[[nodiscard]] bool equalIgnoringCase(
    std::string_view left, std::string_view right) noexcept {
    return left.size() == right.size() &&
        std::equal(left.begin(), left.end(), right.begin(), [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) ==
                std::tolower(static_cast<unsigned char>(b));
        });
}

} // namespace

std::optional<MouseGameplayContext> readMouseGameplayContext(
    const psx::R3000Runtime& runtime, bool photo_mode_active) noexcept {
    if (photo_mode_active) {
        return std::nullopt;
    }
    std::uint32_t game = 0U;
    std::uint32_t state_handler = 0U;
    std::uint32_t player = 0U;
    std::uint32_t behaviour = 0U;
    std::uint32_t control_handler = 0U;
    std::uint32_t input = 0U;
    MouseGameplayContext result{};
    if (!runtime.read32(game_manager_address, game) ||
        !validGuestPointer(game) ||
        !runtime.read32(game + game_state_handler_offset, state_handler) ||
        state_handler != gameplay_state_handler ||
        !runtime.read32(player_address, player) || !validGuestPointer(player) ||
        !runtime.read32(player + player_behaviour_offset, behaviour) ||
        !validGuestPointer(behaviour) ||
        !runtime.read32(
            behaviour + behaviour_handler_offset, control_handler) ||
        control_handler != player_user_control_handler ||
        !runtime.read32(player + player_orientation_y_offset, result.player_yaw) ||
        !runtime.read32(input_manager_address, input) ||
        !validGuestPointer(input)) {
        return std::nullopt;
    }
    result.player = player;
    for (std::size_t index = 0U; index < result.physical_to_logical.size();
         ++index) {
        if (!runtime.read8(
                input + input_map_offset + static_cast<std::uint32_t>(index),
                result.physical_to_logical[index])) {
            return std::nullopt;
        }
    }
    return result;
}

std::uint16_t applySemanticMouseActions(
    std::uint16_t active_low_buttons,
    std::span<const bool, primary_action_count> semantic_actions,
    std::span<const std::uint8_t, primary_action_count>
        physical_to_logical) noexcept {
    std::array<std::uint8_t, primary_action_count> logical_to_physical{};
    std::uint16_t seen = 0U;
    for (std::size_t physical = 0U; physical < primary_action_count;
         ++physical) {
        const auto logical = physical_to_logical[physical];
        if (logical >= primary_action_count ||
            (seen & (1U << logical)) != 0U) {
            return active_low_buttons;
        }
        seen |= static_cast<std::uint16_t>(1U << logical);
        logical_to_physical[logical] = static_cast<std::uint8_t>(physical);
    }
    if (seen != 0xFFU) {
        return active_low_buttons;
    }
    for (std::size_t logical = 0U; logical < semantic_actions.size();
         ++logical) {
        if (semantic_actions[logical]) {
            // ReadSonyPads byte-swaps the direct-pad word. Thus the physical
            // Control bit p is the host pad bit p^8.
            const auto host_bit = logical_to_physical[logical] ^ 8U;
            active_low_buttons &= static_cast<std::uint16_t>(
                ~(std::uint16_t{1U} << host_bit));
        }
    }
    return active_low_buttons;
}

void MouseYawController::setMode(MouseMovementMode mode) noexcept {
    if (mode_ != mode) {
        mode_ = mode;
        synchronized_ = false;
    }
}

MouseMovementMode MouseYawController::cycleMode() noexcept {
    switch (mode_) {
    case MouseMovementMode::off:
        setMode(MouseMovementMode::camera_relative);
        break;
    case MouseMovementMode::camera_relative:
        setMode(MouseMovementMode::character_relative);
        break;
    case MouseMovementMode::character_relative:
        setMode(MouseMovementMode::off);
        break;
    }
    return mode_;
}

void MouseYawController::update(
    const std::optional<MouseGameplayContext>& context,
    std::int32_t mouse_x,
    std::int32_t sensitivity) noexcept {
    if (mode_ == MouseMovementMode::off || !context) {
        synchronized_ = false;
        return;
    }
    if (!synchronized_) {
        yaw_ = context->player_yaw;
        synchronized_ = true;
    }
    yaw_ += static_cast<std::uint32_t>(
        static_cast<std::int64_t>(mouse_x) * sensitivity);
}

std::optional<MouseButton> parseMouseButton(std::string_view value) noexcept {
    if (equalIgnoringCase(value, "none")) return MouseButton::none;
    if (equalIgnoringCase(value, "left")) return MouseButton::left;
    if (equalIgnoringCase(value, "right")) return MouseButton::right;
    if (equalIgnoringCase(value, "middle")) return MouseButton::middle;
    if (equalIgnoringCase(value, "x1")) return MouseButton::x1;
    if (equalIgnoringCase(value, "x2")) return MouseButton::x2;
    return std::nullopt;
}

std::string_view mouseButtonName(MouseButton button) noexcept {
    switch (button) {
    case MouseButton::none: return "None";
    case MouseButton::left: return "Left";
    case MouseButton::right: return "Right";
    case MouseButton::middle: return "Middle";
    case MouseButton::x1: return "X1";
    case MouseButton::x2: return "X2";
    }
    return "None";
}

std::optional<MouseMovementMode> parseMouseMovementMode(
    std::string_view value) noexcept {
    if (equalIgnoringCase(value, "off")) return MouseMovementMode::off;
    if (equalIgnoringCase(value, "camera_relative")) {
        return MouseMovementMode::camera_relative;
    }
    if (equalIgnoringCase(value, "character_relative")) {
        return MouseMovementMode::character_relative;
    }
    return std::nullopt;
}

std::string_view mouseMovementModeName(MouseMovementMode mode) noexcept {
    switch (mode) {
    case MouseMovementMode::off: return "off";
    case MouseMovementMode::camera_relative: return "camera_relative";
    case MouseMovementMode::character_relative: return "character_relative";
    }
    return "off";
}

const RetailTrampoline& mouseActionTrampoline() noexcept {
    static const RetailTrampoline patch{
        "mouse_player_action",
        action_site,
        action_window[4],
        action_arena,
        0x800753A0U,
        action_body};
    return patch;
}

const RetailTrampoline& mouseTargetTrampoline() noexcept {
    static const RetailTrampoline patch{
        "mouse_strafe_target",
        target_site,
        target_window[2],
        target_arena,
        0x80034040U,
        target_body};
    return patch;
}

bool setMouseControlPatches(
    psx::R3000Runtime& runtime, bool enabled) noexcept {
    const auto& action = mouseActionTrampoline();
    const auto& target = mouseTargetTrampoline();
    const auto action_installed = trampolineInstalled(runtime, action);
    const auto target_installed = trampolineInstalled(runtime, target);

    std::uint32_t action_word = 0U;
    std::uint32_t target_word = 0U;
    const auto action_stock = runtime.read32(action.site, action_word) &&
        action_word == action.original_word &&
        windowMatches(runtime, action_site - 16U, action_window);
    const auto target_stock = runtime.read32(target.site, target_word) &&
        target_word == target.original_word &&
        windowMatches(runtime, target_site - 8U, target_window);

    if (enabled) {
        if (action_installed && target_installed) {
            return windowMatches(
                       runtime, action_site - 16U, action_window, 4U) &&
                windowMatches(
                       runtime, target_site - 8U, target_window, 2U);
        }
        if (!action_stock || !target_stock || action_installed ||
            target_installed) {
            return false;
        }
        if (!applyRetailTrampoline(runtime, action)) {
            return false;
        }
        if (!applyRetailTrampoline(runtime, target)) {
            (void)revertRetailTrampoline(runtime, action);
            return false;
        }
        return true;
    }

    if (action_stock && target_stock) {
        return true;
    }
    if (!action_installed || !target_installed) {
        return false;
    }
    if (!revertRetailTrampoline(runtime, target)) {
        return false;
    }
    if (!revertRetailTrampoline(runtime, action)) {
        // Best-effort rollback to the previously coherent installed state.
        (void)applyRetailTrampoline(runtime, target);
        return false;
    }
    return true;
}

} // namespace stuntmaster::game
