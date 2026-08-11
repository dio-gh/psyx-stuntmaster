#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace stuntmaster::app {

// The per-user data location for a self-contained stuntmaster.exe. Everything
// the running game reads or writes -- configuration, saves, logs, and input
// bindings -- lives under one root so nothing is created beside the executable
// and the Windows registry is never touched.
struct UserPaths {
    std::filesystem::path root;          // e.g. <Documents>\Stuntmaster
    std::filesystem::path config;        // root / stuntmaster.ini
    std::filesystem::path saves;         // root / saves
    std::filesystem::path logs;          // root / logs
    std::filesystem::path input_config;  // root / input.ini
};

// Compose the standard subpaths under `root`. Pure: performs no filesystem
// access, so it is fully unit-testable and portable.
[[nodiscard]] UserPaths userPathsForRoot(const std::filesystem::path& root);

// The default per-user data root. On Windows this is
// <Documents>\Stuntmaster via the Known Folder API (no registry, no hardcoded
// path). Falls back to %USERPROFILE%\Documents\Stuntmaster, then a relative
// "Stuntmaster", if the shell lookup fails.
[[nodiscard]] std::filesystem::path defaultUserDataRoot();

// Create root, saves, and logs if missing. Empty members are skipped. Returns
// false and sets `error` on failure.
[[nodiscard]] bool ensureUserDirectories(
    const UserPaths& paths,
    std::string& error);

struct DisplaySize {
    std::uint32_t width{};
    std::uint32_t height{};
};

// The primary display's pixel resolution, used as the native default render
// size. Falls back to 1280x720 if it cannot be queried. Windows-only; other
// platforms return the fallback.
[[nodiscard]] DisplaySize primaryDisplaySize() noexcept;

// The default windowed client size: two-thirds of the primary display in each
// axis, so a native-resolution render target is supersampled down into a
// comfortably sized window instead of covering the whole screen. Both axes
// scale by the same factor, so the display's aspect ratio is preserved. A small
// floor guards against a degenerate result on an unexpectedly tiny metric.
[[nodiscard]] DisplaySize defaultWindowedSize() noexcept;

} // namespace stuntmaster::app
