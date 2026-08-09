#pragma once

#include "stuntmaster/core/sha256.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace stuntmaster::app {

// Bump whenever the payload order or any POD state layout changes.
inline constexpr std::uint32_t quick_save_format_version = 2U;

struct QuickSaveCompatibility {
    std::uint32_t guest_update_rate{};
    std::uint32_t guest_cpu_scale{};
    std::uint32_t render_width{};
    std::uint32_t render_height{};
    std::uint32_t cd_read_speed{};
    std::uint32_t flags{};

    bool operator==(const QuickSaveCompatibility&) const = default;
};
static_assert(sizeof(QuickSaveCompatibility) == 6U * sizeof(std::uint32_t));

// Bit zero belonged to the removed software rasterizer and remains reserved so
// the surviving flag values stay stable in diagnostics and saved headers.
inline constexpr std::uint32_t quick_save_flag_framebuffer_composite = 1U << 1U;
inline constexpr std::uint32_t quick_save_flag_retime_motion = 1U << 2U;
inline constexpr std::uint32_t quick_save_flag_retime_clock = 1U << 3U;
inline constexpr std::uint32_t quick_save_flag_eager_high_rate = 1U << 4U;
inline constexpr std::uint32_t quick_save_flag_widescreen_cull = 1U << 5U;

// Only settings that can change the restored machine's future execution are
// compatibility constraints. Framebuffer compositing, the internal render
// target, and the reversible widescreen-cull patch are
// presentation choices; a state can safely be loaded with different values
// after its candidate runtime is normalized to the current cull selection.
[[nodiscard]] bool quickSaveSettingsCompatible(
    const QuickSaveCompatibility& saved,
    const QuickSaveCompatibility& current) noexcept;

struct QuickSaveFile {
    stuntmaster::core::Sha256Digest executable_hash{};
    QuickSaveCompatibility compatibility{};
    std::vector<std::byte> payload;
};

[[nodiscard]] std::filesystem::path defaultQuickSavePath();
[[nodiscard]] std::filesystem::path timestampedQuickSavePath(
    std::chrono::system_clock::time_point now =
        std::chrono::system_clock::now());

void writeQuickSaveFile(
    const std::filesystem::path& destination,
    const stuntmaster::core::Sha256Digest& executable_hash,
    const QuickSaveCompatibility& compatibility,
    std::span<const std::byte> payload);

[[nodiscard]] QuickSaveFile readQuickSaveFile(
    const std::filesystem::path& source);

} // namespace stuntmaster::app
