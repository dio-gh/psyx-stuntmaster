#include "logging.hpp"

#include "user_paths.hpp"

#include <cstdio>
#include <ctime>
#include <system_error>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <io.h>
#endif

namespace stuntmaster::app {

RunConsoleMode chooseConsoleMode(
    int argc, bool stdout_redirected) noexcept {
    if (argc > 1 || stdout_redirected) {
        return RunConsoleMode::attach_or_redirect;
    }
    return RunConsoleMode::log_file;
}

#ifdef _WIN32

namespace {

[[nodiscard]] bool stdoutIsRedirected() noexcept {
    const HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    const DWORD type = GetFileType(handle);
    return type == FILE_TYPE_DISK || type == FILE_TYPE_PIPE;
}

} // namespace

void configureConsoleAndLogging(int argc) {
    const bool redirected = stdoutIsRedirected();
    if (chooseConsoleMode(argc, redirected) ==
        RunConsoleMode::attach_or_redirect) {
        // A redirected stdout already flows to the caller's pipe/file; leave it
        // untouched so probe output stays byte-clean. Otherwise attach the
        // parent console (if any) so a terminal launch is visible.
        if (!redirected && AttachConsole(ATTACH_PARENT_PROCESS)) {
            FILE* stream = nullptr;
            (void)freopen_s(&stream, "CONOUT$", "w", stdout);
            (void)freopen_s(&stream, "CONOUT$", "w", stderr);
            (void)freopen_s(&stream, "CONIN$", "r", stdin);
        }
        return;
    }

    // Bare double-click: send diagnostics to the per-user log file and keep the
    // process window-free. The data root is resolved directly because option
    // parsing has not run yet (and a bare launch has no --data-root anyway).
    const auto paths = userPathsForRoot(defaultUserDataRoot());
    std::error_code ignored;
    std::filesystem::create_directories(paths.logs, ignored);
    const auto log_path = paths.logs / "stuntmaster.log";
    FILE* stream = nullptr;
    if (_wfreopen_s(&stream, log_path.wstring().c_str(), L"w", stdout) == 0) {
        // Fold stderr into the same log so ordering is preserved.
        (void)_dup2(_fileno(stdout), _fileno(stderr));
        // Stamp the log with the local start time so a bare-launch session can
        // be dated.
        const std::time_t now = std::time(nullptr);
        std::tm local{};
        if (localtime_s(&local, &now) == 0) {
            char stamp[32];
            if (std::strftime(
                    stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &local) != 0) {
                std::fprintf(stdout, "=== stuntmaster log %s ===\n", stamp);
                std::fflush(stdout);
            }
        }
    }
}

#else

void configureConsoleAndLogging(int) {}

#endif

} // namespace stuntmaster::app
