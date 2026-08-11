#include "game_setup.hpp"

#include "frame_io.hpp"
#include "launcher_settings.hpp"

#include "stuntmaster/core/sha256.hpp"
#include "stuntmaster/disc/iso9660.hpp"
#include "stuntmaster/game/supported_game.hpp"
#include "stuntmaster/psx/executable.hpp"

#include <exception>
#include <optional>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>

#include <array>
#endif

namespace stuntmaster::app {

namespace {

// Quietly confirm a .cue resolves to the one supported release. Mirrors the
// identity check loadGame() performs, without its diagnostic output, so the
// picker can reject an unsupported dump before the machine is built.
[[nodiscard]] bool gameImageIsSupported(
    const std::filesystem::path& cue, std::string& error) {
    try {
        auto image = stuntmaster::disc::Iso9660Image::open(cue);
        const auto system_cnf = image.readFile("SYSTEM.CNF");
        const auto boot_path =
            stuntmaster::psx::parseBootPath(asText(system_cnf));
        const auto executable_bytes = image.readFile(boot_path);
        const auto executable_hash =
            stuntmaster::core::sha256(executable_bytes);
        if (!stuntmaster::game::identify(
                image.volumeId(), boot_path, executable_hash)) {
            error =
                "The selected folder's disc image is not the supported "
                "release (NTSC-U, serial SLUS-00684).";
            return false;
        }
        return true;
    } catch (const std::exception& problem) {
        error = problem.what();
        return false;
    }
}

#ifdef _WIN32

std::wstring widenUtf8(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        wide.data(), length);
    return wide;
}

int CALLBACK browseCallback(
    HWND window, UINT message, LPARAM, LPARAM data) {
    if (message == BFFM_INITIALIZED && data != 0) {
        SendMessageW(window, BFFM_SETSELECTIONW, TRUE, data);
    }
    return 0;
}

[[nodiscard]] std::optional<std::filesystem::path> browseForGameFolder(
    const std::filesystem::path& initial) {
    const std::wstring initial_path = initial.wstring();
    BROWSEINFOW info{};
    info.lpszTitle =
        L"Select the folder containing your Jackie Chan Stuntmaster "
        L"BIN/CUE dump";
    info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    if (!initial_path.empty()) {
        info.lpfn = browseCallback;
        info.lParam = reinterpret_cast<LPARAM>(initial_path.c_str());
    }
    LPITEMIDLIST item = SHBrowseForFolderW(&info);
    if (item == nullptr) {
        return std::nullopt;
    }
    std::array<wchar_t, 32768> path{};
    const bool resolved = SHGetPathFromIDListW(item, path.data());
    CoTaskMemFree(item);
    if (!resolved) {
        return std::nullopt;
    }
    return std::filesystem::path{path.data()};
}

[[nodiscard]] int retryOrQuit(const std::wstring& message) {
    return MessageBoxW(
        nullptr, message.c_str(), L"Stuntmaster",
        MB_RETRYCANCEL | MB_ICONWARNING);
}

#endif // _WIN32

} // namespace

bool resolveGameInteractively(Options& options) {
#ifdef _WIN32
    const HRESULT com_result = CoInitializeEx(
        nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    // Preserve any existing display/patch preferences; only the game folder is
    // being (re)selected here.
    auto settings = loadLauncherSettings(options.launcher_settings_path)
                        .value_or(LauncherSettings{});
    std::filesystem::path initial = settings.game_directory;

    bool proceed = false;
    for (;;) {
        const auto folder = browseForGameFolder(initial);
        if (!folder) {
            if (retryOrQuit(
                    L"No game folder was selected.\n\nStuntmaster needs your "
                    L"Jackie Chan Stuntmaster BIN/CUE dump to run.") ==
                IDRETRY) {
                continue;
            }
            break;
        }
        initial = *folder;

        std::string error;
        const auto cue = findGameCue(*folder, error);
        if (!cue) {
            if (retryOrQuit(widenUtf8(error)) == IDRETRY) {
                continue;
            }
            break;
        }
        if (!gameImageIsSupported(*cue, error)) {
            if (retryOrQuit(widenUtf8(error)) == IDRETRY) {
                continue;
            }
            break;
        }

        settings.game_directory = *folder;
        std::string save_error;
        if (!saveLauncherSettings(
                options.launcher_settings_path, settings, save_error)) {
            // Non-fatal: run this session anyway, but tell the user their
            // choice was not remembered.
            MessageBoxW(
                nullptr, widenUtf8(save_error).c_str(), L"Stuntmaster",
                MB_OK | MB_ICONWARNING);
        }
        options.game = *cue;
        // First launch runs with the out-of-the-box defaults (60 fps,
        // widescreen, fullscreen, native resolution) that were just persisted.
        applyLiveDisplaySettings(options, settings);
        proceed = true;
        break;
    }

    if (SUCCEEDED(com_result)) {
        CoUninitialize();
    }
    return proceed;
#else
    (void)options;
    return false;
#endif
}

} // namespace stuntmaster::app
