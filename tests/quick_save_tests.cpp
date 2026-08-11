#include "quick_save.hpp"
#include "options.hpp"
#include "launcher_settings.hpp"

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
    const auto fixed = std::chrono::system_clock::time_point{
        std::chrono::milliseconds{1'800'000'123}};
    const auto timestamped =
        stuntmaster::app::timestampedQuickSavePath(fixed);
    assert(timestamped.parent_path() == "saves");
    assert(timestamped != stuntmaster::app::defaultQuickSavePath());
    const auto name = timestamped.filename().string();
    assert(name.starts_with("quick-save-"));
    assert(name.ends_with("-123.stsm"));
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
        static_cast<int>(arguments.size()), arguments.data());
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
        widescreen_arguments.data());
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
        static_cast<int>(arguments.size()), arguments.data());
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
        override_arguments.data());
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
        static_cast<int>(arguments.size()), arguments.data());
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
        static_cast<int>(arguments.size()), arguments.data());
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

    const stuntmaster::app::LauncherSettings saved{
        directory / "game", 1080U, true, true};
    std::string error;
    assert(stuntmaster::app::saveLauncherSettings(
        settings_path, saved, error));
    const auto loaded = stuntmaster::app::loadLauncherSettings(settings_path);
    assert(loaded);
    assert(loaded->game_directory == saved.game_directory);
    assert(loaded->resolution_height == 1080U);
    assert(loaded->sixty_hz);
    assert(loaded->widescreen);

    std::array arguments{const_cast<char*>("stuntmaster")};
    const auto parsed = stuntmaster::app::parseOptions(
        static_cast<int>(arguments.size()), arguments.data(), settings_path);
    assert(parsed);
    assert(parsed->game == cue);
    assert(parsed->run_live);
    assert(parsed->window_width == 1920U);
    assert(parsed->window_height == 1080U);
    assert(parsed->render_width == 1920U);
    assert(parsed->render_height == 1080U);
    assert(parsed->presentation_rate == 60U);
    assert(parsed->guest_update_rate == 60U);
    assert(parsed->retime_motion);
    assert(parsed->retime_clock);
    assert(parsed->widescreen_cull);
    assert(parsed->have_input_config);
    assert(parsed->input_config == directory / "input.ini");

    assert(stuntmaster::app::saveRuntimeLauncherSettings(
        settings_path, directory / "other-game", 1440U, false, false,
        error));
    const auto runtime_saved =
        stuntmaster::app::loadLauncherSettings(settings_path);
    assert(runtime_saved);
    assert(runtime_saved->game_directory == saved.game_directory);
    assert(runtime_saved->resolution_height == 1440U);
    assert(!runtime_saved->sixty_hz);
    assert(!runtime_saved->widescreen);

    // A custom command-line render height must not corrupt the launcher's
    // finite resolution tier when only cadence changes at runtime.
    assert(stuntmaster::app::saveRuntimeLauncherSettings(
        settings_path, directory / "other-game", 600U, true, false,
        error));
    const auto custom_height_saved =
        stuntmaster::app::loadLauncherSettings(settings_path);
    assert(custom_height_saved);
    assert(custom_height_saved->resolution_height == 1440U);
    assert(custom_height_saved->sixty_hz);

    assert(stuntmaster::app::saveRuntimeLauncherSettings(
        settings_path, directory / "other-game", 720U, false, false,
        error));
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
    assert(stuntmaster::app::saveLauncherSettings(
        settings_path,
        {directory / "game", 720U, true, true}, error));

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
    assert(parsed->window_width == 960U);
    assert(parsed->window_height == 720U);
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

    std::array interpreter_arguments{
        const_cast<char*>("stuntmaster-tests"),
        const_cast<char*>("--game"),
        const_cast<char*>("game.cue"),
        const_cast<char*>("--probe-guest"),
        const_cast<char*>("--interpreter-cpu"),
    };
    const auto interpreter = stuntmaster::app::parseOptions(
        static_cast<int>(interpreter_arguments.size()),
        interpreter_arguments.data());
    assert(interpreter);
    assert(interpreter->interpreter_cpu);

    std::array cached_arguments{
        const_cast<char*>("stuntmaster-tests"),
        const_cast<char*>("--game"),
        const_cast<char*>("game.cue"),
        const_cast<char*>("--probe-guest"),
        const_cast<char*>("--cached-recompiler-cpu"),
    };
    const auto cached = stuntmaster::app::parseOptions(
        static_cast<int>(cached_arguments.size()), cached_arguments.data());
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
