#include "launcher_settings.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <fstream>
#include <string_view>

namespace stuntmaster::app {

namespace {

constexpr bool supportedHeight(std::uint32_t height) noexcept {
    return height == 480U || height == 720U || height == 1080U ||
        height == 1440U;
}

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

    LauncherSettings settings;
    bool have_directory = false;
    bool have_resolution = false;
    bool have_sixty_hz = false;
    bool have_widescreen = false;
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
            have_directory = !settings.game_directory.empty();
        } else if (key == "resolution_height") {
            const auto parsed = std::from_chars(
                value.data(), value.data() + value.size(),
                settings.resolution_height);
            have_resolution = parsed.ec == std::errc{} &&
                parsed.ptr == value.data() + value.size() &&
                supportedHeight(settings.resolution_height);
            if (!have_resolution) {
                return std::nullopt;
            }
        } else if (key == "sixty_hz") {
            have_sixty_hz = parseBoolean(value, settings.sixty_hz);
            if (!have_sixty_hz) {
                return std::nullopt;
            }
        } else if (key == "widescreen") {
            have_widescreen = parseBoolean(value, settings.widescreen);
            if (!have_widescreen) {
                return std::nullopt;
            }
        }
    }
    if (!input.eof() || !have_directory || !have_resolution ||
        !have_sixty_hz || !have_widescreen) {
        return std::nullopt;
    }
    return settings;
}

bool saveLauncherSettings(
    const std::filesystem::path& path,
    const LauncherSettings& settings,
    std::string& error) {
    if (settings.game_directory.empty() ||
        !supportedHeight(settings.resolution_height)) {
        error = "The launcher settings are incomplete.";
        return false;
    }
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    if (!output) {
        error = "Could not open " + path.string() + " for writing.";
        return false;
    }
    output << "[launcher]\n"
           << "game_directory=" << pathToUtf8(settings.game_directory) << '\n'
           << "resolution_height=" << settings.resolution_height << '\n'
           << "sixty_hz=" << (settings.sixty_hz ? 1 : 0) << '\n'
           << "widescreen=" << (settings.widescreen ? 1 : 0) << '\n';
    if (!output) {
        error = "Could not write " + path.string() + '.';
        return false;
    }
    return true;
}

bool saveRuntimeLauncherSettings(
    const std::filesystem::path& path,
    const std::filesystem::path& fallback_game_directory,
    std::uint32_t resolution_height,
    bool sixty_hz,
    bool widescreen,
    std::string& error) {
    auto settings = loadLauncherSettings(path).value_or(LauncherSettings{});
    if (settings.game_directory.empty()) {
        settings.game_directory = fallback_game_directory;
    }
    if (supportedHeight(resolution_height)) {
        settings.resolution_height = resolution_height;
    }
    settings.sixty_hz = sixty_hz;
    settings.widescreen = widescreen;
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
