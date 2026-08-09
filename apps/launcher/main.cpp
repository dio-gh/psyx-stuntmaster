#include "launcher_settings.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr wchar_t window_class_name[] = L"StuntmasterLauncherWindow";
constexpr int directory_edit_id = 1001;
constexpr int browse_button_id = 1002;
constexpr int resolution_combo_id = 1003;
constexpr int sixty_hz_check_id = 1004;
constexpr int widescreen_check_id = 1005;
constexpr int save_button_id = 1006;
constexpr int play_button_id = 1007;
constexpr int status_label_id = 1008;
constexpr std::array<std::uint32_t, 4> resolution_heights{
    480U, 720U, 1080U, 1440U};

struct LauncherWindow {
    HWND window{};
    HWND directory_edit{};
    HWND resolution_combo{};
    HWND sixty_hz_check{};
    HWND widescreen_check{};
    HWND status_label{};
    std::filesystem::path executable_directory;
    std::filesystem::path settings_path;
};

std::wstring widenAscii(const std::string& value) {
    return {value.begin(), value.end()};
}

std::filesystem::path executablePath() {
    std::vector<wchar_t> buffer(512U);
    for (;;) {
        const auto length = GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0U) {
            return {};
        }
        if (length + 1U < buffer.size()) {
            return std::filesystem::path{
                std::wstring_view{buffer.data(), length}};
        }
        buffer.resize(buffer.size() * 2U);
    }
}

std::wstring windowText(HWND control) {
    const auto length = GetWindowTextLengthW(control);
    std::wstring text(static_cast<std::size_t>(length) + 1U, L'\0');
    if (length != 0) {
        GetWindowTextW(control, text.data(), length + 1);
    }
    text.resize(static_cast<std::size_t>(length));
    return text;
}

void setStatus(LauncherWindow& state, const wchar_t* text) {
    SetWindowTextW(state.status_label, text);
}

bool checked(HWND control) {
    return SendMessageW(control, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

std::uint32_t selectedHeight(const LauncherWindow& state) {
    const auto selection = SendMessageW(
        state.resolution_combo, CB_GETCURSEL, 0, 0);
    if (selection < 0 ||
        static_cast<std::size_t>(selection) >= resolution_heights.size()) {
        return 720U;
    }
    return resolution_heights[static_cast<std::size_t>(selection)];
}

void populateResolutions(LauncherWindow& state, std::uint32_t selected_height) {
    SendMessageW(state.resolution_combo, CB_RESETCONTENT, 0, 0);
    const auto widescreen = checked(state.widescreen_check);
    std::size_t selected_index = 1U;
    for (std::size_t index = 0; index < resolution_heights.size(); ++index) {
        const auto height = resolution_heights[index];
        const auto width = stuntmaster::app::renderWidthFor(height, widescreen);
        const auto label = std::to_wstring(width) + L" x " +
            std::to_wstring(height);
        SendMessageW(
            state.resolution_combo, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(label.c_str()));
        if (height == selected_height) {
            selected_index = index;
        }
    }
    SendMessageW(
        state.resolution_combo, CB_SETCURSEL,
        static_cast<WPARAM>(selected_index), 0);
}

int CALLBACK browseCallback(HWND window, UINT message, LPARAM, LPARAM data) {
    if (message == BFFM_INITIALIZED && data != 0) {
        SendMessageW(window, BFFM_SETSELECTIONW, TRUE, data);
    }
    return 0;
}

void browseForDirectory(LauncherWindow& state) {
    auto initial = windowText(state.directory_edit);
    BROWSEINFOW info{};
    info.hwndOwner = state.window;
    info.lpszTitle = L"Select the folder containing the Stuntmaster BIN/CUE dump";
    info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    info.lpfn = browseCallback;
    info.lParam = reinterpret_cast<LPARAM>(initial.c_str());
    const auto item = SHBrowseForFolderW(&info);
    if (item == nullptr) {
        return;
    }
    std::array<wchar_t, 32768> path{};
    if (SHGetPathFromIDListW(item, path.data())) {
        SetWindowTextW(state.directory_edit, path.data());
        setStatus(state, L"");
    }
    CoTaskMemFree(item);
}

bool saveSettings(LauncherWindow& state) {
    stuntmaster::app::LauncherSettings settings;
    settings.game_directory = windowText(state.directory_edit);
    settings.resolution_height = selectedHeight(state);
    settings.sixty_hz = checked(state.sixty_hz_check);
    settings.widescreen = checked(state.widescreen_check);

    std::string error;
    if (!stuntmaster::app::findGameCue(settings.game_directory, error)) {
        MessageBoxW(
            state.window, widenAscii(error).c_str(), L"Stuntmaster Launcher",
            MB_OK | MB_ICONWARNING);
        return false;
    }
    if (!stuntmaster::app::saveLauncherSettings(
            state.settings_path, settings, error)) {
        MessageBoxW(
            state.window, widenAscii(error).c_str(), L"Stuntmaster Launcher",
            MB_OK | MB_ICONERROR);
        return false;
    }
    setStatus(state, L"Settings saved.");
    return true;
}

void launchGame(LauncherWindow& state) {
    if (!saveSettings(state)) {
        return;
    }
    const auto game_executable =
        state.executable_directory / L"stuntmaster.exe";
    std::error_code file_error;
    if (!std::filesystem::is_regular_file(game_executable, file_error)) {
        MessageBoxW(
            state.window,
            L"stuntmaster.exe was not found beside the launcher.",
            L"Stuntmaster Launcher", MB_OK | MB_ICONERROR);
        return;
    }

    auto command_line = L"\"" + game_executable.wstring() + L"\"";
    std::vector<wchar_t> mutable_command{
        command_line.begin(), command_line.end()};
    mutable_command.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(
            game_executable.c_str(), mutable_command.data(), nullptr, nullptr,
            FALSE, 0, nullptr, state.executable_directory.c_str(), &startup,
            &process)) {
        const auto message = L"Could not start stuntmaster.exe (Windows error " +
            std::to_wstring(GetLastError()) + L").";
        MessageBoxW(
            state.window, message.c_str(), L"Stuntmaster Launcher",
            MB_OK | MB_ICONERROR);
        return;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    DestroyWindow(state.window);
}

HWND addControl(
    HWND parent,
    const wchar_t* class_name,
    const wchar_t* text,
    DWORD style,
    int x,
    int y,
    int width,
    int height,
    int id) {
    const auto control = CreateWindowExW(
        std::wstring_view{class_name} == WC_EDITW ? WS_EX_CLIENTEDGE : 0,
        class_name, text, WS_CHILD | WS_VISIBLE | style,
        x, y, width, height, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleW(nullptr), nullptr);
    SendMessageW(
        control, WM_SETFONT,
        reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    return control;
}

void createControls(LauncherWindow& state) {
    addControl(
        state.window, WC_STATICW, L"Game dump folder", 0,
        20, 20, 200, 20, 0);
    state.directory_edit = addControl(
        state.window, WC_EDITW, L"", ES_AUTOHSCROLL,
        20, 42, 385, 25, directory_edit_id);
    addControl(
        state.window, WC_BUTTONW, L"Browse...", BS_PUSHBUTTON,
        415, 41, 85, 27, browse_button_id);

    addControl(
        state.window, WC_STATICW, L"Resolution", 0,
        20, 84, 140, 20, 0);
    state.resolution_combo = addControl(
        state.window, WC_COMBOBOXW, L"",
        CBS_DROPDOWNLIST | WS_VSCROLL,
        20, 106, 180, 160, resolution_combo_id);
    state.sixty_hz_check = addControl(
        state.window, WC_BUTTONW, L"60 Hz game mode", BS_AUTOCHECKBOX,
        230, 106, 150, 24, sixty_hz_check_id);
    state.widescreen_check = addControl(
        state.window, WC_BUTTONW, L"Widescreen", BS_AUTOCHECKBOX,
        380, 106, 120, 24, widescreen_check_id);

    state.status_label = addControl(
        state.window, WC_STATICW, L"", 0,
        20, 154, 270, 24, status_label_id);
    addControl(
        state.window, WC_BUTTONW, L"Save", BS_PUSHBUTTON,
        326, 149, 80, 30, save_button_id);
    const auto play = addControl(
        state.window, WC_BUTTONW, L"Play", BS_DEFPUSHBUTTON,
        416, 149, 84, 30, play_button_id);
    SendMessageW(state.window, DM_SETDEFID, play_button_id, 0);
    SetFocus(play);

    const auto saved = stuntmaster::app::loadLauncherSettings(
        state.settings_path);
    if (saved) {
        SetWindowTextW(
            state.directory_edit, saved->game_directory.c_str());
        SendMessageW(
            state.sixty_hz_check, BM_SETCHECK,
            saved->sixty_hz ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(
            state.widescreen_check, BM_SETCHECK,
            saved->widescreen ? BST_CHECKED : BST_UNCHECKED, 0);
        populateResolutions(state, saved->resolution_height);
    } else {
        populateResolutions(state, 720U);
    }
}

LRESULT CALLBACK windowProcedure(
    HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<LauncherWindow*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        state = static_cast<LauncherWindow*>(create->lpCreateParams);
        state->window = window;
        SetWindowLongPtrW(
            window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (state == nullptr) {
        return DefWindowProcW(window, message, wparam, lparam);
    }
    switch (message) {
    case WM_CREATE:
        createControls(*state);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case browse_button_id:
            browseForDirectory(*state);
            return 0;
        case widescreen_check_id: {
            const auto height = selectedHeight(*state);
            populateResolutions(*state, height);
            setStatus(*state, L"");
            return 0;
        }
        case save_button_id:
            saveSettings(*state);
            return 0;
        case play_button_id:
            launchGame(*state);
            return 0;
        default:
            return 0;
        }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
    SetProcessDPIAware();
    const auto com_result = CoInitializeEx(
        nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    LauncherWindow state;
    const auto executable = executablePath();
    state.executable_directory = executable.parent_path();
    state.settings_path = stuntmaster::app::launcherSettingsPath(executable);

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = windowProcedure;
    window_class.hInstance = instance;
    window_class.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    window_class.hIconSm = window_class.hIcon;
    window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    window_class.lpszClassName = window_class_name;
    if (RegisterClassExW(&window_class) == 0U) {
        return 1;
    }

    RECT bounds{0, 0, 520, 205};
    AdjustWindowRectEx(
        &bounds, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        FALSE, 0);
    const auto window = CreateWindowExW(
        0, window_class_name, L"Stuntmaster Launcher",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT,
        bounds.right - bounds.left, bounds.bottom - bounds.top,
        nullptr, nullptr, instance, &state);
    if (window == nullptr) {
        if (SUCCEEDED(com_result)) {
            CoUninitialize();
        }
        return 1;
    }
    ShowWindow(window, show_command);
    UpdateWindow(window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (SUCCEEDED(com_result)) {
        CoUninitialize();
    }
    return static_cast<int>(message.wParam);
}
