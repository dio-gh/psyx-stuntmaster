#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace stuntmaster::app {

struct LauncherSettings {
    std::filesystem::path game_directory;
    std::uint32_t resolution_height{720U};
    bool sixty_hz{};
    bool widescreen{};
};

[[nodiscard]] std::filesystem::path launcherSettingsPath(
    const std::filesystem::path& executable_path);

[[nodiscard]] std::optional<LauncherSettings> loadLauncherSettings(
    const std::filesystem::path& path);

[[nodiscard]] bool saveLauncherSettings(
    const std::filesystem::path& path,
    const LauncherSettings& settings,
    std::string& error);

// Persists an accepted live setting change while retaining the configured
// game folder. Unsupported custom render heights leave the saved resolution
// tier unchanged instead of making the launcher file unreadable.
[[nodiscard]] bool saveRuntimeLauncherSettings(
    const std::filesystem::path& path,
    const std::filesystem::path& fallback_game_directory,
    std::uint32_t resolution_height,
    bool sixty_hz,
    bool widescreen,
    std::string& error);

[[nodiscard]] std::optional<std::filesystem::path> findGameCue(
    const std::filesystem::path& directory,
    std::string& error);

[[nodiscard]] std::uint32_t renderWidthFor(
    std::uint32_t height,
    bool widescreen) noexcept;

} // namespace stuntmaster::app
