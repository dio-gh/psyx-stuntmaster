#include "user_paths.hpp"

#include <cstdlib>
#include <system_error>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#endif

namespace stuntmaster::app {

namespace {
constexpr const char* config_file_name = "stuntmaster.ini";
constexpr const char* input_file_name = "input.ini";
constexpr const char* saves_dir_name = "saves";
constexpr const char* logs_dir_name = "logs";
} // namespace

UserPaths userPathsForRoot(const std::filesystem::path& root) {
    UserPaths paths;
    paths.root = root;
    paths.config = root / config_file_name;
    paths.saves = root / saves_dir_name;
    paths.logs = root / logs_dir_name;
    paths.input_config = root / input_file_name;
    return paths;
}

std::filesystem::path defaultUserDataRoot() {
#ifdef _WIN32
    PWSTR raw_documents = nullptr;
    const auto result = SHGetKnownFolderPath(
        FOLDERID_Documents, KF_FLAG_CREATE, nullptr, &raw_documents);
    if (SUCCEEDED(result) && raw_documents != nullptr) {
        std::filesystem::path documents{raw_documents};
        CoTaskMemFree(raw_documents);
        return documents / L"Stuntmaster";
    }
    if (raw_documents != nullptr) {
        CoTaskMemFree(raw_documents);
    }
    if (const wchar_t* profile = _wgetenv(L"USERPROFILE")) {
        return std::filesystem::path{profile} / L"Documents" / L"Stuntmaster";
    }
    return std::filesystem::path{L"Stuntmaster"};
#else
    if (const char* home = std::getenv("HOME")) {
        return std::filesystem::path{home} / "Documents" / "Stuntmaster";
    }
    return std::filesystem::path{"Stuntmaster"};
#endif
}

DisplaySize primaryDisplaySize() noexcept {
    DisplaySize size{1280U, 720U};
#ifdef _WIN32
    const int width = GetSystemMetrics(SM_CXSCREEN);
    const int height = GetSystemMetrics(SM_CYSCREEN);
    if (width > 0 && height > 0) {
        size.width = static_cast<std::uint32_t>(width);
        size.height = static_cast<std::uint32_t>(height);
    }
#endif
    return size;
}

DisplaySize defaultWindowedSize() noexcept {
    const auto native = primaryDisplaySize();
    DisplaySize size{native.width * 2U / 3U, native.height * 2U / 3U};
    if (size.width < 640U) {
        size.width = 640U;
    }
    if (size.height < 480U) {
        size.height = 480U;
    }
    return size;
}

bool ensureUserDirectories(const UserPaths& paths, std::string& error) {
    const std::filesystem::path directories[] = {
        paths.root, paths.saves, paths.logs};
    for (const auto& directory : directories) {
        if (directory.empty()) {
            continue;
        }
        std::error_code code;
        std::filesystem::create_directories(directory, code);
        if (code) {
            error = "could not create directory " + directory.string() +
                ": " + code.message();
            return false;
        }
    }
    return true;
}

} // namespace stuntmaster::app
