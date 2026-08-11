#include "quick_save.hpp"
#include "logging.hpp"
#include "options.hpp"
#include "launcher_settings.hpp"
#include "user_paths.hpp"

#include "stuntmaster/core/error.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

void quickSaveFileRoundTripsAndRejectsCorruption() {
    const auto unique = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto directory = std::filesystem::temp_directory_path() /
        ("stuntmaster-quick-save-test-" + unique);
    const auto path = directory / "state.stsm";

    stuntmaster::core::Sha256Digest executable_hash{};
    executable_hash[0] = std::byte{0x42U};
    const stuntmaster::app::QuickSaveCompatibility compatibility{
        60U,
        1U,
        1280U,
        720U,
        16U,
        stuntmaster::app::quick_save_flag_retime_motion,
    };
    const std::array payload{
        std::byte{0x00U}, std::byte{0x11U}, std::byte{0xFEU}};
    stuntmaster::app::writeQuickSaveFile(
        path, executable_hash, compatibility, payload);
    const auto loaded = stuntmaster::app::readQuickSaveFile(path);
    assert(loaded.executable_hash == executable_hash);
    assert(loaded.compatibility == compatibility);
    assert(std::ranges::equal(loaded.payload, payload));

    {
        std::fstream file{path, std::ios::binary | std::ios::in | std::ios::out};
        assert(file);
        file.seekp(-1, std::ios::end);
        const char corrupt = '\x7f';
        file.write(&corrupt, 1);
        assert(file);
    }
    auto rejected = false;
    try {
        (void)stuntmaster::app::readQuickSaveFile(path);
    } catch (const stuntmaster::core::Error&) {
        rejected = true;
    }
    assert(rejected);

    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

void timestampedQuickSaveNamesDoNotAliasTheDefaultSlot() {
    const std::filesystem::path saves_dir =
        std::filesystem::path{"data"} / "Stuntmaster" / "saves";
    const auto fixed = std::chrono::system_clock::time_point{
        std::chrono::milliseconds{1'800'000'123}};
    const auto timestamped =
        stuntmaster::app::timestampedQuickSavePath(saves_dir, fixed);
    assert(timestamped.parent_path() == saves_dir);
    assert(timestamped !=
        stuntmaster::app::defaultQuickSavePath(saves_dir));
    const auto name = timestamped.filename().string();
    assert(name.starts_with("quick-save-"));
    assert(name.ends_with("-123.stsm"));
}

void userPathsComposeUnderTheRoot() {
    const std::filesystem::path root =
        std::filesystem::path{"data"} / "Stuntmaster";
    const auto paths = stuntmaster::app::userPathsForRoot(root);
    assert(paths.root == root);
    assert(paths.config == root / "stuntmaster.ini");
    assert(paths.saves == root / "saves");
    assert(paths.logs == root / "logs");
    assert(paths.input_config == root / "input.ini");
    // The memory card and quick saves share the composed saves directory.
    assert(stuntmaster::app::defaultQuickSavePath(paths.saves).parent_path()
        == paths.saves);
}

void dataRootOverrideRedirectsConfigAndInput() {
    const auto unique = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto root = std::filesystem::temp_directory_path() /
        ("stuntmaster-data-root-" + unique);
    const auto root_arg = root.string();
    std::array arguments{
        const_cast<char*>("stuntmaster"),
        const_cast<char*>("--game"),
        const_cast<char*>("game.cue"),
        const_cast<char*>("--run"),
        const_cast<char*>("--data-root"),
        const_cast<char*>(root_arg.c_str()),
    };
    const auto parsed = stuntmaster::app::parseOptions(
        static_cast<int>(arguments.size()), arguments.data());
    assert(parsed);
    assert(parsed->have_data_root);
    assert(parsed->data_root == root);
    assert(parsed->have_launcher_settings_path);
    assert(parsed->launcher_settings_path == root / "stuntmaster.ini");
    // A live run with no explicit --input-config resolves input.ini under the
    // same data root, never beside the executable.
    assert(parsed->have_input_config);
    assert(parsed->input_config == root / "input.ini");
}

void interactiveGameSetupGateIsInteractiveOnly() {
    stuntmaster::app::Options options;
    // No data root: helper/probe binaries never prompt.
    assert(!stuntmaster::app::needsInteractiveGameSetup(options));

    // Real executable with a data root and no game selected: prompt.
    options.data_root = std::filesystem::path{"data"} / "Stuntmaster";
    assert(stuntmaster::app::needsInteractiveGameSetup(options));

    // A resolved game means nothing to prompt for.
    options.game = "game.cue";
    assert(!stuntmaster::app::needsInteractiveGameSetup(options));
    options.game.clear();

    // Any explicit headless/diagnostic mode suppresses the prompt.
    options.probe_guest = true;
    assert(!stuntmaster::app::needsInteractiveGameSetup(options));
    options.probe_guest = false;
    options.have_replay_capture = true;
    assert(!stuntmaster::app::needsInteractiveGameSetup(options));
}

void firstLaunchWithoutGameReachesInteractiveSetup() {
    // A real-exe first launch: only --data-root, no game, no mode. parseOptions
    // must SUCCEED (not reject the missing --game) so main() can run the
    // interactive picker. This is the integration guard for the option-
    // validation path that game-setup depends on.
    const auto unique = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto root = std::filesystem::temp_directory_path() /
        ("stuntmaster-first-launch-" + unique);
    const auto root_arg = root.string();
    std::array arguments{
        const_cast<char*>("stuntmaster"),
        const_cast<char*>("--data-root"),
        const_cast<char*>(root_arg.c_str()),
    };
    const auto parsed = stuntmaster::app::parseOptions(
        static_cast<int>(arguments.size()), arguments.data());
    assert(parsed);
    assert(parsed->game.empty());
    assert(stuntmaster::app::needsInteractiveGameSetup(*parsed));

    // A probe with no game is NOT interactive setup: it must still be rejected.
    std::array probe_arguments{
        const_cast<char*>("stuntmaster"),
        const_cast<char*>("--data-root"),
        const_cast<char*>(root_arg.c_str()),
        const_cast<char*>("--probe-guest"),
    };
    const auto probe = stuntmaster::app::parseOptions(
        static_cast<int>(probe_arguments.size()), probe_arguments.data());
    assert(!probe);
}

void firstLaunchDefaultsAreSixtyWidescreenFullscreen() {
    // The out-of-the-box config (all flags on, native resolution) yields the
    // intended defaults for a live run.
    stuntmaster::app::Options options;
    const stuntmaster::app::LauncherSettings defaults;
    assert(defaults.sixty_hz && defaults.widescreen && defaults.fullscreen);
    assert(defaults.render_width == 0U && defaults.render_height == 0U);
    stuntmaster::app::applyLiveDisplaySettings(options, defaults);
    assert(options.run_live);
    assert(options.widescreen_cull);
    assert(options.fullscreen);
    assert(options.retime_motion && options.retime_clock);
    assert(options.guest_update_rate == 60U);
    assert(options.have_render_size && options.have_window_size);
    // Native resolution resolves to a real, non-zero render size, while the
    // window defaults to two-thirds of the display -- decoupled from the render
    // target so a native render is supersampled into a smaller window.
    assert(options.render_width > 0U && options.render_height > 0U);
    const auto default_window = stuntmaster::app::defaultWindowedSize();
    assert(options.window_width == default_window.width);
    assert(options.window_height == default_window.height);

    // An explicitly stored resolution is honoured instead of native, a stored
    // window size is honoured independently of the render target, and a stored
    // 30 Hz still arms retiming for later F7 toggling.
    stuntmaster::app::Options explicit_options;
    stuntmaster::app::LauncherSettings explicit_settings;
    explicit_settings.render_width = 1600U;
    explicit_settings.render_height = 900U;
    explicit_settings.window_width = 1024U;
    explicit_settings.window_height = 576U;
    explicit_settings.sixty_hz = false;
    stuntmaster::app::applyLiveDisplaySettings(
        explicit_options, explicit_settings);
    assert(explicit_options.render_width == 1600U);
    assert(explicit_options.render_height == 900U);
    assert(explicit_options.window_width == 1024U);
    assert(explicit_options.window_height == 576U);
    assert(explicit_options.retime_motion && explicit_options.retime_clock);
    assert(explicit_options.guest_update_rate == 30U);
}

void consoleModeKeepsStdoutForArgsOrRedirection() {
    using stuntmaster::app::chooseConsoleMode;
    using stuntmaster::app::RunConsoleMode;
    // Bare double-click (no args, interactive stdout): log to file.
    assert(chooseConsoleMode(1, false) == RunConsoleMode::log_file);
    // Any argument (probe/CLI) keeps stdout.
    assert(chooseConsoleMode(2, false) == RunConsoleMode::attach_or_redirect);
    // A redirected stdout (CI capturing a probe) keeps stdout even with no args.
    assert(chooseConsoleMode(1, true) == RunConsoleMode::attach_or_redirect);
}

void launchOptionAcceptsOneSpecificQuickSave() {
    std::array arguments{
        const_cast<char*>("stuntmaster"),
        const_cast<char*>("--game"),
        const_cast<char*>("game.cue"),
        const_cast<char*>("--run"),
        const_cast<char*>("--load-quick-save"),
        const_cast<char*>("saves/checkpoint.stsm"),
    };
    const auto parsed = stuntmaster::app::parseOptions(
        static_cast<int>(arguments.size()), arguments.data());
    assert(parsed);
    assert(parsed->have_load_quick_save);
    assert(parsed->load_quick_save == "saves/checkpoint.stsm");
}

void presentationDefaultsAreHostTimedAndFourThree() {
    std::array arguments{
        const_cast<char*>("stuntmaster"),
        const_cast<char*>("--game"),
        const_cast<char*>("game.cue"),
        const_cast<char*>("--run"),
    };
    const auto parsed = stuntmaster::app::parseOptions(
        static_cast<int>(arguments.size()), arguments.data(), std::nullopt);
    assert(parsed);
    assert(parsed->render_width == 960U);
    assert(parsed->render_height == 720U);
    assert(!parsed->have_render_size);
    assert(parsed->presentation_rate == 0U);
    assert(!parsed->have_presentation_rate);
    assert(parsed->cd_read_speed == 2U);
    assert(parsed->experimental_host_menu);
    assert(stuntmaster::app::presentationRateForDisplay(*parsed, 144U) ==
           144U);
    assert(stuntmaster::app::presentationRateForDisplay(*parsed, 0U) == 60U);

    std::array widescreen_arguments{
        const_cast<char*>("stuntmaster"),
        const_cast<char*>("--game"),
        const_cast<char*>("game.cue"),
        const_cast<char*>("--run"),
        const_cast<char*>("--widescreen-cull"),
    };
    const auto widescreen = stuntmaster::app::parseOptions(
        static_cast<int>(widescreen_arguments.size()),
        widescreen_arguments.data(), std::nullopt);
    assert(widescreen);
    assert(widescreen->render_width == 1280U);
    assert(widescreen->render_height == 720U);

    auto overridden = *parsed;
    overridden.have_presentation_rate = true;
    overridden.presentation_rate = 120U;
    assert(stuntmaster::app::presentationRateForDisplay(overridden, 144U) ==
           120U);
}

void defaultFeaturesHaveExplicitOptOuts() {
    std::array arguments{
        const_cast<char*>("stuntmaster"),
        const_cast<char*>("--game"),
        const_cast<char*>("game.cue"),
        const_cast<char*>("--run"),
        const_cast<char*>("--no-cd-read-pacing"),
        const_cast<char*>("--no-experimental-host-menu"),
    };
    const auto parsed = stuntmaster::app::parseOptions(
        static_cast<int>(arguments.size()), arguments.data(), std::nullopt);
    assert(parsed);
    assert(parsed->cd_read_speed == 0U);
    assert(!parsed->experimental_host_menu);

    std::array override_arguments{
        const_cast<char*>("stuntmaster"),
        const_cast<char*>("--game"),
        const_cast<char*>("game.cue"),
        const_cast<char*>("--run"),
        const_cast<char*>("--cd-read-pacing"),
        const_cast<char*>("16"),
        const_cast<char*>("--experimental-host-menu"),
    };
    const auto overridden = stuntmaster::app::parseOptions(
        static_cast<int>(override_arguments.size()),
        override_arguments.data(), std::nullopt);
    assert(overridden);
    assert(overridden->cd_read_speed == 16U);
    assert(overridden->experimental_host_menu);
}

void retiredRasterizerOptionIsRejected() {
    std::array arguments{
        const_cast<char*>("stuntmaster"),
        const_cast<char*>("--game"),
        const_cast<char*>("game.cue"),
        const_cast<char*>("--run"),
        const_cast<char*>("--rasterizer"),
    };
    const auto parsed = stuntmaster::app::parseOptions(
        static_cast<int>(arguments.size()), arguments.data(), std::nullopt);
    assert(!parsed);
}

void retimeFlagsArmRuntimeSixtyFromRetailMode() {
    std::array arguments{
        const_cast<char*>("stuntmaster"),
        const_cast<char*>("--game"),
        const_cast<char*>("game.cue"),
        const_cast<char*>("--run"),
        const_cast<char*>("--retime-motion"),
        const_cast<char*>("--retime-clock"),
        const_cast<char*>("--experimental-host-menu"),
    };
    const auto parsed = stuntmaster::app::parseOptions(
        static_cast<int>(arguments.size()), arguments.data(), std::nullopt);
    assert(parsed);
    assert(parsed->guest_update_rate == 30U);
    assert(stuntmaster::app::highFrequencyRuntimeControlAvailable(*parsed));

    auto incomplete = *parsed;
    incomplete.retime_clock = false;
    assert(!stuntmaster::app::highFrequencyRuntimeControlAvailable(incomplete));
}

void launcherSettingsRoundTripAndSupplyLiveDefaults() {
    const auto unique = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto directory = std::filesystem::temp_directory_path() /
        ("stuntmaster-launcher-test-" + unique);
    std::filesystem::create_directories(directory / "game");
    const auto cue = directory / "game" / "stuntmaster.cue";
    std::ofstream{cue} << "FILE \"stuntmaster.bin\" BINARY\n";
    const auto settings_path = directory / "stuntmaster.ini";

    stuntmaster::app::LauncherSettings saved;
    saved.game_directory = directory / "game";
    saved.sixty_hz = true;
    saved.widescreen = true;
    saved.fullscreen = true;
    saved.render_width = 1920U;
    saved.render_height = 1080U;
    saved.window_width = 1600U;
    saved.window_height = 900U;
    std::string error;
    assert(stuntmaster::app::saveLauncherSettings(
        settings_path, saved, error));
    const auto loaded = stuntmaster::app::loadLauncherSettings(settings_path);
    assert(loaded);
    assert(loaded->game_directory == saved.game_directory);
    assert(loaded->render_width == 1920U);
    assert(loaded->render_height == 1080U);
    assert(loaded->window_width == 1600U);
    assert(loaded->window_height == 900U);
    assert(loaded->sixty_hz);
    assert(loaded->widescreen);
    assert(loaded->fullscreen);

    std::array arguments{const_cast<char*>("stuntmaster")};
    const auto parsed = stuntmaster::app::parseOptions(
        static_cast<int>(arguments.size()), arguments.data(), settings_path);
    assert(parsed);
    assert(parsed->game == cue);
    assert(parsed->run_live);
    // The window size is restored from the config independently of the render
    // target, which stays at the stored native-tier resolution.
    assert(parsed->window_width == 1600U);
    assert(parsed->window_height == 900U);
    assert(parsed->render_width == 1920U);
    assert(parsed->render_height == 1080U);
    assert(parsed->fullscreen);
    assert(parsed->presentation_rate == 60U);
    assert(parsed->guest_update_rate == 60U);
    assert(parsed->retime_motion);
    assert(parsed->retime_clock);
    assert(parsed->widescreen_cull);
    assert(parsed->have_input_config);
    assert(parsed->input_config == directory / "input.ini");

    // A runtime change persists explicit resolution, the windowed size, both
    // flags, and fullscreen.
    assert(stuntmaster::app::saveRuntimeLauncherSettings(
        settings_path, directory / "other-game", 2560U, 1440U, 1280U, 720U,
        false, false, false, error));
    const auto runtime_saved =
        stuntmaster::app::loadLauncherSettings(settings_path);
    assert(runtime_saved);
    assert(runtime_saved->game_directory == saved.game_directory);
    assert(runtime_saved->render_width == 2560U);
    assert(runtime_saved->render_height == 1440U);
    assert(runtime_saved->window_width == 1280U);
    assert(runtime_saved->window_height == 720U);
    assert(!runtime_saved->sixty_hz);
    assert(!runtime_saved->widescreen);
    assert(!runtime_saved->fullscreen);

    // A non-tier resolution is stored verbatim (no tier snapping any more).
    assert(stuntmaster::app::saveRuntimeLauncherSettings(
        settings_path, directory / "other-game", 1600U, 900U, 800U, 600U, true,
        false, true, error));
    const auto custom_saved =
        stuntmaster::app::loadLauncherSettings(settings_path);
    assert(custom_saved);
    assert(custom_saved->render_width == 1600U);
    assert(custom_saved->render_height == 900U);
    assert(custom_saved->window_width == 800U);
    assert(custom_saved->window_height == 600U);
    assert(custom_saved->sixty_hz);
    assert(custom_saved->fullscreen);

    assert(stuntmaster::app::saveRuntimeLauncherSettings(
        settings_path, directory / "other-game", 1920U, 1080U, 0U, 0U, false,
        false, true, error));
    const auto retail_initial = stuntmaster::app::parseOptions(
        static_cast<int>(arguments.size()), arguments.data(), settings_path);
    assert(retail_initial);
    assert(retail_initial->guest_update_rate == 30U);
    assert(stuntmaster::app::highFrequencyRuntimeControlAvailable(
        *retail_initial));

    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

void commandLineOverridesPersistedLauncherSettings() {
    const auto unique = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto directory = std::filesystem::temp_directory_path() /
        ("stuntmaster-launcher-override-test-" + unique);
    std::filesystem::create_directories(directory / "game");
    std::ofstream{directory / "game" / "saved.cue"} << "saved";
    const auto settings_path = directory / "stuntmaster.ini";
    std::string error;
    stuntmaster::app::LauncherSettings persisted;
    persisted.game_directory = directory / "game";
    persisted.sixty_hz = true;
    persisted.widescreen = true;
    persisted.fullscreen = true;
    persisted.render_width = 960U;
    persisted.render_height = 720U;
    persisted.window_width = 1024U;
    persisted.window_height = 576U;
    assert(stuntmaster::app::saveLauncherSettings(
        settings_path, persisted, error));

    std::array arguments{
        const_cast<char*>("stuntmaster"),
        const_cast<char*>("--game"),
        const_cast<char*>("override.cue"),
        const_cast<char*>("--run"),
        const_cast<char*>("--guest-update-rate"),
        const_cast<char*>("30"),
        const_cast<char*>("--presentation-rate"),
        const_cast<char*>("144"),
        const_cast<char*>("--render-size"),
        const_cast<char*>("640x480"),
        const_cast<char*>("--no-widescreen-cull"),
        const_cast<char*>("--input-config"),
        const_cast<char*>("custom-input.ini"),
    };
    const auto parsed = stuntmaster::app::parseOptions(
        static_cast<int>(arguments.size()), arguments.data(), settings_path);
    assert(parsed);
    assert(parsed->game == "override.cue");
    assert(parsed->guest_update_rate == 30U);
    assert(parsed->presentation_rate == 144U);
    assert(parsed->render_width == 640U);
    assert(parsed->render_height == 480U);
    // --render-size overrides only the render target; with no --window-size the
    // persisted window size still applies, decoupled from the render size.
    assert(parsed->window_width == 1024U);
    assert(parsed->window_height == 576U);
    assert(!parsed->widescreen_cull);
    assert(parsed->have_input_config);
    assert(parsed->input_config == "custom-input.ini");

    std::array probe_arguments{
        const_cast<char*>("stuntmaster"),
        const_cast<char*>("--probe-guest"),
    };
    const auto probe = stuntmaster::app::parseOptions(
        static_cast<int>(probe_arguments.size()), probe_arguments.data(),
        settings_path);
    assert(probe);
    assert(probe->game == directory / "game" / "saved.cue");
    assert(probe->probe_guest);
    assert(!probe->run_live);
    assert(probe->guest_update_rate == 30U);
    assert(!probe->retime_motion);
    assert(!probe->widescreen_cull);

    // A CPU-backend probe. std::nullopt injects no per-user data root, so the
    // result is deterministic regardless of the machine's <Documents>\Stuntmaster.
    std::array interpreter_arguments{
        const_cast<char*>("stuntmaster"),
        const_cast<char*>("--game"),
        const_cast<char*>("game.cue"),
        const_cast<char*>("--probe-guest"),
        const_cast<char*>("--interpreter-cpu"),
    };
    const auto interpreter = stuntmaster::app::parseOptions(
        static_cast<int>(interpreter_arguments.size()),
        interpreter_arguments.data(), std::nullopt);
    assert(interpreter);
    assert(interpreter->interpreter_cpu);

    std::array cached_arguments{
        const_cast<char*>("stuntmaster"),
        const_cast<char*>("--game"),
        const_cast<char*>("game.cue"),
        const_cast<char*>("--probe-guest"),
        const_cast<char*>("--cached-recompiler-cpu"),
    };
    const auto cached = stuntmaster::app::parseOptions(
        static_cast<int>(cached_arguments.size()), cached_arguments.data(),
        std::nullopt);
    assert(cached);
    assert(cached->cached_recompiler_cpu);
    assert(!cached->interpreter_cpu);

    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

void presentationSettingsDoNotInvalidateQuickSaves() {
    using namespace stuntmaster::app;
    const QuickSaveCompatibility saved{
        60U,
        1U,
        1280U,
        720U,
        16U,
        quick_save_flag_framebuffer_composite |
            quick_save_flag_retime_motion |
            quick_save_flag_retime_clock,
    };
    auto current = saved;
    current.render_width = 1920U;
    current.render_height = 1080U;
    current.flags ^= quick_save_flag_framebuffer_composite;
    assert(quickSaveSettingsCompatible(saved, current));

    current.guest_update_rate = 30U;
    assert(quickSaveSettingsCompatible(saved, current));
    current = saved;
    current.flags ^= quick_save_flag_retime_clock;
    assert(!quickSaveSettingsCompatible(saved, current));

    current = saved;
    current.flags |= quick_save_flag_widescreen_cull;
    assert(quickSaveSettingsCompatible(saved, current));
    auto wide_saved = saved;
    wide_saved.flags |= quick_save_flag_widescreen_cull;
    current = wide_saved;
    current.render_width = 1920U;
    assert(quickSaveSettingsCompatible(wide_saved, current));
}

} // namespace

int main() {
    quickSaveFileRoundTripsAndRejectsCorruption();
    timestampedQuickSaveNamesDoNotAliasTheDefaultSlot();
    userPathsComposeUnderTheRoot();
    dataRootOverrideRedirectsConfigAndInput();
    interactiveGameSetupGateIsInteractiveOnly();
    firstLaunchWithoutGameReachesInteractiveSetup();
    firstLaunchDefaultsAreSixtyWidescreenFullscreen();
    consoleModeKeepsStdoutForArgsOrRedirection();
    launchOptionAcceptsOneSpecificQuickSave();
    presentationDefaultsAreHostTimedAndFourThree();
    defaultFeaturesHaveExplicitOptOuts();
    retiredRasterizerOptionIsRejected();
    retimeFlagsArmRuntimeSixtyFromRetailMode();
    launcherSettingsRoundTripAndSupplyLiveDefaults();
    commandLineOverridesPersistedLauncherSettings();
    presentationSettingsDoNotInvalidateQuickSaves();
    std::cout << "stuntmaster_quick_save_tests: passed\n";
}
