#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace stuntmaster::app {

struct LauncherSettings {
    std::filesystem::path game_directory;
    // Out-of-the-box defaults for a self-contained executable: 60 fps retiming,
    // widescreen, and fullscreen are all on. Resolution defaults to the native
    // desktop, represented here by 0/0 and resolved at startup.
    bool sixty_hz{true};
    bool widescreen{true};
    bool fullscreen{true};
    std::uint32_t render_width{};
    std::uint32_t render_height{};
    // The windowed client size, tracked independently of the render target so a
    // native-resolution render can present into a smaller window. 0/0 means the
    // default (two-thirds of the primary display); a runtime window resize
    // stores the chosen size here so it is restored on the next launch.
    std::uint32_t window_width{};
    std::uint32_t window_height{};
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
// game folder. The render resolution is stored explicitly (0/0 = native) and
// the windowed size independently (0/0 = the two-thirds-of-display default).
[[nodiscard]] bool saveRuntimeLauncherSettings(
    const std::filesystem::path& path,
    const std::filesystem::path& fallback_game_directory,
    std::uint32_t render_width,
    std::uint32_t render_height,
    std::uint32_t window_width,
    std::uint32_t window_height,
    bool sixty_hz,
    bool widescreen,
    bool fullscreen,
    std::string& error);

[[nodiscard]] std::optional<std::filesystem::path> findGameCue(
    const std::filesystem::path& directory,
    std::string& error);

[[nodiscard]] std::uint32_t renderWidthFor(
    std::uint32_t height,
    bool widescreen) noexcept;

} // namespace stuntmaster::app
