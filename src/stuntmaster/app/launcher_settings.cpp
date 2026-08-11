#include "launcher_settings.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <fstream>
#include <string_view>

namespace stuntmaster::app {

namespace {

std::string trim(std::string value) {
    const auto whitespace = [](unsigned char character) {
        return std::isspace(character) != 0;
    };
    value.erase(
        value.begin(),
        std::find_if_not(value.begin(), value.end(), whitespace));
    value.erase(
        std::find_if_not(value.rbegin(), value.rend(), whitespace).base(),
        value.end());
    return value;
}

std::string pathToUtf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

std::filesystem::path pathFromUtf8(std::string_view value) {
    std::u8string converted;
    converted.resize(value.size());
    std::transform(
        value.begin(), value.end(), converted.begin(),
        [](char character) { return static_cast<char8_t>(character); });
    return std::filesystem::path{converted};
}

bool parseBoolean(std::string_view value, bool& out) noexcept {
    if (value == "1" || value == "true") {
        out = true;
        return true;
    }
    if (value == "0" || value == "false") {
        out = false;
        return true;
    }
    return false;
}

bool parseUnsigned(std::string_view value, std::uint32_t& out) noexcept {
    const auto parsed = std::from_chars(
        value.data(), value.data() + value.size(), out);
    return parsed.ec == std::errc{} &&
        parsed.ptr == value.data() + value.size();
}

} // namespace

std::filesystem::path launcherSettingsPath(
    const std::filesystem::path& executable_path) {
    std::error_code error;
    auto absolute = std::filesystem::absolute(executable_path, error);
    if (error) {
        absolute = executable_path;
    }
    return absolute.parent_path() / "stuntmaster.ini";
}

std::optional<LauncherSettings> loadLauncherSettings(
    const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        return std::nullopt;
    }

    // Start from the defaults so an older or hand-edited file that omits keys
    // still loads; only malformed values or a malformed line are rejected.
    LauncherSettings settings;
    bool in_launcher_section = false;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        line = trim(std::move(line));
        if (line.empty() || line.front() == ';' || line.front() == '#') {
            continue;
        }
        if (line.front() == '[' && line.back() == ']') {
            in_launcher_section = line == "[launcher]";
            continue;
        }
        if (!in_launcher_section) {
            continue;
        }
        const auto separator = line.find('=');
        if (separator == std::string::npos) {
            return std::nullopt;
        }
        const auto key = trim(line.substr(0U, separator));
        const auto value = trim(line.substr(separator + 1U));
        if (key == "game_directory") {
            settings.game_directory = pathFromUtf8(value);
        } else if (key == "sixty_hz") {
            if (!parseBoolean(value, settings.sixty_hz)) {
                return std::nullopt;
            }
        } else if (key == "widescreen") {
            if (!parseBoolean(value, settings.widescreen)) {
                return std::nullopt;
            }
        } else if (key == "fullscreen") {
            if (!parseBoolean(value, settings.fullscreen)) {
                return std::nullopt;
            }
        } else if (key == "render_width") {
            if (!parseUnsigned(value, settings.render_width)) {
                return std::nullopt;
            }
        } else if (key == "render_height") {
            if (!parseUnsigned(value, settings.render_height)) {
                return std::nullopt;
            }
        } else if (key == "window_width") {
            if (!parseUnsigned(value, settings.window_width)) {
                return std::nullopt;
            }
        } else if (key == "window_height") {
            if (!parseUnsigned(value, settings.window_height)) {
                return std::nullopt;
            }
        }
        // Unknown keys are ignored for forward compatibility.
    }
    if (!input.eof()) {
        return std::nullopt;
    }
    return settings;
}

bool saveLauncherSettings(
    const std::filesystem::path& path,
    const LauncherSettings& settings,
    std::string& error) {
    if (settings.game_directory.empty()) {
        error = "The settings are incomplete (no game folder).";
        return false;
    }
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    if (!output) {
        error = "Could not open " + path.string() + " for writing.";
        return false;
    }
    output << "[launcher]\n"
           << "game_directory=" << pathToUtf8(settings.game_directory) << '\n'
           << "sixty_hz=" << (settings.sixty_hz ? 1 : 0) << '\n'
           << "widescreen=" << (settings.widescreen ? 1 : 0) << '\n'
           << "fullscreen=" << (settings.fullscreen ? 1 : 0) << '\n'
           << "render_width=" << settings.render_width << '\n'
           << "render_height=" << settings.render_height << '\n'
           << "window_width=" << settings.window_width << '\n'
           << "window_height=" << settings.window_height << '\n';
    if (!output) {
        error = "Could not write " + path.string() + '.';
        return false;
    }
    return true;
}

bool saveRuntimeLauncherSettings(
    const std::filesystem::path& path,
    const std::filesystem::path& fallback_game_directory,
    std::uint32_t render_width,
    std::uint32_t render_height,
    std::uint32_t window_width,
    std::uint32_t window_height,
    bool sixty_hz,
    bool widescreen,
    bool fullscreen,
    std::string& error) {
    auto settings = loadLauncherSettings(path).value_or(LauncherSettings{});
    if (settings.game_directory.empty()) {
        settings.game_directory = fallback_game_directory;
    }
    settings.render_width = render_width;
    settings.render_height = render_height;
    settings.window_width = window_width;
    settings.window_height = window_height;
    settings.sixty_hz = sixty_hz;
    settings.widescreen = widescreen;
    settings.fullscreen = fullscreen;
    return saveLauncherSettings(path, settings, error);
}

std::optional<std::filesystem::path> findGameCue(
    const std::filesystem::path& directory,
    std::string& error) {
    std::error_code status_error;
    if (!std::filesystem::is_directory(directory, status_error)) {
        error = "The selected game dump folder does not exist.";
        return std::nullopt;
    }

    std::optional<std::filesystem::path> found;
    std::error_code iteration_error;
    for (std::filesystem::directory_iterator iterator{
             directory,
             std::filesystem::directory_options::skip_permission_denied,
             iteration_error},
         end;
         !iteration_error && iterator != end;
         iterator.increment(iteration_error)) {
        if (!iterator->is_regular_file(status_error)) {
            status_error.clear();
            continue;
        }
        auto extension = iterator->path().extension().string();
        std::transform(
            extension.begin(), extension.end(), extension.begin(),
            [](unsigned char character) {
                return static_cast<char>(std::tolower(character));
            });
        if (extension != ".cue") {
            continue;
        }
        if (found) {
            error = "The selected folder contains more than one .cue file.";
            return std::nullopt;
        }
        found = iterator->path();
    }
    if (iteration_error) {
        error = "Could not read the selected game dump folder.";
        return std::nullopt;
    }
    if (!found) {
        error = "The selected folder does not contain a .cue file.";
        return std::nullopt;
    }
    return found;
}

std::uint32_t renderWidthFor(
    std::uint32_t height,
    bool widescreen) noexcept {
    if (widescreen) {
        // 480p has no exact integer 16:9 width. 854 is the conventional mode;
        // the other supported tiers are exact.
        return height == 480U ? 854U : height * 16U / 9U;
    }
    return height * 4U / 3U;
}

} // namespace stuntmaster::app
