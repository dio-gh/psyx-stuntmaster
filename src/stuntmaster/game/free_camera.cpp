#include "stuntmaster/game/free_camera.hpp"

#include "stuntmaster/psx/r3000_runtime.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace stuntmaster::game {
namespace {

constexpr std::uint32_t the_camera_address = 0x800DD734U;
constexpr std::uint32_t camera_vtable = 0x800CCCB8U;

constexpr std::uint32_t position_offset = 0x1CU;
constexpr std::uint32_t previous_position_offset = 0x7CU;
constexpr std::uint32_t current_position_offset = 0xCCU;
constexpr std::uint32_t movement_time_offset = 0x12CU;
constexpr std::uint32_t angle_offset = 0x17CU;
constexpr std::uint32_t order_this_offset = 0x170U;
constexpr std::uint32_t order_mode_offset = 0x172U;
constexpr std::uint32_t order_function_offset = 0x174U;
constexpr std::uint32_t camera_animation_offset = 0x1D4U;
constexpr std::uint32_t flags_offset = 0x1D8U;

constexpr double normal_speed = 3'000.0;
constexpr double fast_speed = 12'000.0;
constexpr std::int32_t mouse_sensitivity = 20;
constexpr double angle_turn = 65'536.0;
constexpr double controller_axis_max = 32'767.0;
constexpr double controller_turn_speed = angle_turn * 0.5; // 180 degrees/sec

[[nodiscard]] bool readVector(
    const psx::R3000Runtime& runtime,
    std::uint32_t address,
    std::array<std::uint32_t, 3U>& value) noexcept {
    return runtime.read32(address, value[0]) &&
        runtime.read32(address + 4U, value[1]) &&
        runtime.read32(address + 8U, value[2]);
}

[[nodiscard]] bool writeVector(
    psx::R3000Runtime& runtime,
    std::uint32_t address,
    const std::array<std::uint32_t, 3U>& value) noexcept {
    return runtime.write32(address, value[0]) &&
        runtime.write32(address + 4U, value[1]) &&
        runtime.write32(address + 8U, value[2]);
}

} // namespace

bool FreeCameraController::enable(psx::R3000Runtime& runtime) noexcept {
    std::uint32_t camera = 0U;
    std::uint32_t vtable = 0U;
    std::uint32_t animation = 0U;
    if (!runtime.read32(the_camera_address, camera) || camera < 0x80010000U ||
        camera > 0x801FFE00U || !runtime.read32(camera + 8U, vtable) ||
        vtable != camera_vtable ||
        !runtime.read32(camera + camera_animation_offset, animation) ||
        animation != 0U ||
        !runtime.read16(
            camera + order_this_offset,
            retail_state_.order_this_offset) ||
        !runtime.read16(
            camera + order_mode_offset,
            retail_state_.order_mode_index) ||
        !runtime.read32(
            camera + order_function_offset,
            retail_state_.order_function) ||
        !runtime.read32(camera + flags_offset, retail_state_.flags) ||
        !readVector(
            runtime,
            camera + movement_time_offset,
            retail_state_.movement_time)) {
        return false;
    }

    std::array<std::uint32_t, 3U> position{};
    if (!readVector(runtime, camera + position_offset, position) ||
        !readVector(runtime, camera + angle_offset, angles_)) {
        return false;
    }
    for (std::size_t index = 0; index < position.size(); ++index) {
        position_[index] = static_cast<double>(
            static_cast<std::int32_t>(position[index]));
    }

    camera_ = camera;
    // OrderHandler mode zero is retail's explicit "do not dispatch" state.
    // Camera::Think still calls Move, and Camera::Update still constructs the
    // original tMatrixCamera, so all guest rendering remains intact.
    const auto free_flags = retail_state_.flags & ~3U;
    if (!runtime.write16(camera_ + order_this_offset, 0U) ||
        !runtime.write16(camera_ + order_mode_offset, 0U) ||
        !runtime.write32(camera_ + flags_offset, free_flags) ||
        !writePose(runtime)) {
        (void)restore(runtime);
        camera_ = 0U;
        return false;
    }
    active_ = true;
    return true;
}

bool FreeCameraController::restore(psx::R3000Runtime& runtime) const noexcept {
    if (camera_ == 0U) {
        return true;
    }
    std::uint32_t current_camera = 0U;
    if (!runtime.read32(the_camera_address, current_camera) ||
        current_camera != camera_) {
        return false;
    }
    return runtime.write16(
               camera_ + order_this_offset,
               retail_state_.order_this_offset) &&
        runtime.write16(
            camera_ + order_mode_offset,
            retail_state_.order_mode_index) &&
        runtime.write32(
            camera_ + order_function_offset,
            retail_state_.order_function) &&
        runtime.write32(camera_ + flags_offset, retail_state_.flags) &&
        writeVector(
            runtime,
            camera_ + movement_time_offset,
            retail_state_.movement_time);
}

bool FreeCameraController::writePose(
    psx::R3000Runtime& runtime) const noexcept {
    std::array<std::uint32_t, 3U> position{};
    for (std::size_t index = 0; index < position.size(); ++index) {
        const auto clamped = std::clamp(
            std::round(position_[index]),
            static_cast<double>(std::numeric_limits<std::int32_t>::min()),
            static_cast<double>(std::numeric_limits<std::int32_t>::max()));
        position[index] = static_cast<std::uint32_t>(
            static_cast<std::int32_t>(clamped));
    }
    return writeVector(runtime, camera_ + position_offset, position) &&
        writeVector(runtime, camera_ + previous_position_offset, position) &&
        writeVector(runtime, camera_ + current_position_offset, position) &&
        writeVector(runtime, camera_ + angle_offset, angles_);
}

FreeCameraResult FreeCameraController::toggle(
    psx::R3000Runtime& runtime,
    bool gameplay_state) noexcept {
    if (active_) {
        const auto restored = restore(runtime);
        abandon();
        return restored ? FreeCameraResult::disabled
                        : FreeCameraResult::unavailable;
    }
    if (!gameplay_state || !enable(runtime)) {
        return FreeCameraResult::unavailable;
    }
    return FreeCameraResult::enabled;
}

FreeCameraResult FreeCameraController::update(
    psx::R3000Runtime& runtime,
    const FreeCameraInput& input,
    std::uint32_t vblank_rate,
    bool gameplay_state) noexcept {
    if (!active_) {
        return FreeCameraResult::unchanged;
    }
    if (!gameplay_state) {
        (void)restore(runtime);
        abandon();
        return FreeCameraResult::unavailable;
    }
    std::uint32_t current_camera = 0U;
    std::uint32_t vtable = 0U;
    std::uint32_t animation = 0U;
    std::uint16_t mode = 0U;
    if (!runtime.read32(the_camera_address, current_camera) ||
        current_camera != camera_ || !runtime.read32(camera_ + 8U, vtable) ||
        vtable != camera_vtable ||
        !runtime.read32(camera_ + camera_animation_offset, animation) ||
        !runtime.read16(camera_ + order_mode_offset, mode)) {
        abandon();
        return FreeCameraResult::unavailable;
    }
    if (animation != 0U) {
        // Camera animation bypasses the OrderHandler. Restore the handler now
        // so retail has its prior mode when the animation ends.
        (void)restore(runtime);
        abandon();
        return FreeCameraResult::unavailable;
    }
    if (mode != 0U) {
        // Retail SetMode has taken ownership since the last boundary. Do not
        // overwrite that newer state with the snapshot from free-camera entry.
        abandon();
        return FreeCameraResult::unavailable;
    }

    angles_[1] += static_cast<std::uint32_t>(
        static_cast<std::int64_t>(input.mouse_x) * mouse_sensitivity);
    angles_[0] -= static_cast<std::uint32_t>(
        static_cast<std::int64_t>(input.mouse_y) * mouse_sensitivity);
    const auto updates_per_second = static_cast<double>(
        vblank_rate == 0U ? 60U : vblank_rate);
    angles_[1] += static_cast<std::uint32_t>(static_cast<std::int32_t>(
        std::round(
            static_cast<double>(input.controller_look_x) /
            controller_axis_max * controller_turn_speed /
            updates_per_second)));
    angles_[0] -= static_cast<std::uint32_t>(static_cast<std::int32_t>(
        std::round(
            static_cast<double>(input.controller_look_y) /
            controller_axis_max * controller_turn_speed /
            updates_per_second)));

    double local_x = static_cast<double>(input.controller_right) /
        controller_axis_max;
    double local_y = static_cast<double>(input.controller_up) /
        controller_axis_max;
    double local_z = static_cast<double>(input.controller_forward) /
        controller_axis_max;
    local_x += (input.movement & free_camera_right) != 0U ? 1.0 : 0.0;
    local_x -= (input.movement & free_camera_left) != 0U ? 1.0 : 0.0;
    local_y += (input.movement & free_camera_up) != 0U ? 1.0 : 0.0;
    local_y -= (input.movement & free_camera_down) != 0U ? 1.0 : 0.0;
    local_z += (input.movement & free_camera_forward) != 0U ? 1.0 : 0.0;
    local_z -= (input.movement & free_camera_backward) != 0U ? 1.0 : 0.0;

    const auto magnitude = std::sqrt(
        local_x * local_x + local_y * local_y + local_z * local_z);
    if (magnitude > 0.0) {
        // Preserve sub-unit analog magnitude; normalize only diagonals or a
        // keyboard/controller combination that exceeds full scale.
        if (magnitude > 1.0) {
            local_x /= magnitude;
            local_y /= magnitude;
            local_z /= magnitude;
        }
        const auto yaw = static_cast<double>(angles_[1] & 0xFFFFU) *
            (2.0 * std::numbers::pi / angle_turn);
        const auto sine = std::sin(yaw);
        const auto cosine = std::cos(yaw);
        const auto speed = (input.movement & free_camera_fast) != 0U
            ? fast_speed
            : normal_speed;
        const auto step = speed / updates_per_second;
        // Camera's stored rotation is the view basis, so translating the eye
        // through it uses the inverse direction of an ordinary world/object
        // transform. Negate all three local axes: W/A/E then move forward,
        // left, and up as labelled instead of their opposites.
        position_[0] -= (local_x * cosine + local_z * sine) * step;
        position_[1] -= local_y * step;
        position_[2] -= (-local_x * sine + local_z * cosine) * step;
    }

    if (!writePose(runtime)) {
        (void)restore(runtime);
        abandon();
        return FreeCameraResult::unavailable;
    }
    return FreeCameraResult::unchanged;
}

bool FreeCameraController::normalizeSavedRuntime(
    psx::R3000Runtime& runtime) const noexcept {
    return !active_ || restore(runtime);
}

void FreeCameraController::abandon() noexcept {
    active_ = false;
    camera_ = 0U;
    position_ = {};
    angles_ = {};
    retail_state_ = {};
}

} // namespace stuntmaster::game
