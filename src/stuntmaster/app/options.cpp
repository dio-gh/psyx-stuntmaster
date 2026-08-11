#include "options.hpp"
#include "launcher_settings.hpp"

#include "stuntmaster/game/guest_schedule.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cwctype>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace stuntmaster::app {

namespace {

bool parseScriptedInput(
    std::string_view value, std::vector<Options::ScriptedInput>& out) {
    // Active-low, in the host's normalized logical order: a pressed button
    // clears its bit. These match the masks the PAD boundary tests lock.
    constexpr std::array<std::pair<std::string_view, std::uint16_t>, 12>
        buttons{{
            {"select", 0xFFFEU},
            {"start", 0xFFF7U},
            {"up", 0xFFEFU},
            {"right", 0xFFDFU},
            {"down", 0xFFBFU},
            {"left", 0xFF7FU},
            {"triangle", 0xEFFFU},
            {"circle", 0xDFFFU},
            {"cross", 0xBFFFU},
            {"square", 0x7FFFU},
            {"l1", 0xFBFFU},
            {"r1", 0xF7FFU},
        }};
    while (!value.empty()) {
        const auto comma = value.find(',');
        const auto entry = value.substr(0U, comma);
        const auto colon = entry.find(':');
        if (colon == std::string_view::npos) {
            return false;
        }
        const auto when = entry.substr(0U, colon);
        const auto name = entry.substr(colon + 1U);
        std::uint64_t vblank{};
        const auto parsed = std::from_chars(
            when.data(), when.data() + when.size(), vblank);
        if (parsed.ec != std::errc{} ||
            parsed.ptr != when.data() + when.size()) {
            return false;
        }
        const auto match = std::ranges::find(
            buttons, name, &std::pair<std::string_view, std::uint16_t>::first);
        if (match == buttons.end()) {
            return false;
        }
        out.push_back({vblank, match->second});
        if (comma == std::string_view::npos) {
            break;
        }
        value.remove_prefix(comma + 1U);
    }
    return !out.empty();
}

bool parseWindowSize(
    std::string_view value,
    std::uint32_t& width,
    std::uint32_t& height) {
    const auto separator = value.find_first_of("xX");
    if (separator == std::string_view::npos || separator == 0U ||
        separator + 1U >= value.size() ||
        value.find_first_of("xX", separator + 1U) != std::string_view::npos) {
        return false;
    }
    const auto width_text = value.substr(0U, separator);
    const auto height_text = value.substr(separator + 1U);
    const auto width_result = std::from_chars(
        width_text.data(), width_text.data() + width_text.size(), width);
    const auto height_result = std::from_chars(
        height_text.data(), height_text.data() + height_text.size(), height);
    constexpr std::uint32_t minimum_width = 320U;
    constexpr std::uint32_t minimum_height = 200U;
    constexpr std::uint32_t maximum_width = 16'384U;
    constexpr std::uint32_t maximum_height = 8'640U;
    return width_result.ec == std::errc{} &&
        width_result.ptr == width_text.data() + width_text.size() &&
        height_result.ec == std::errc{} &&
        height_result.ptr == height_text.data() + height_text.size() &&
        width >= minimum_width && width <= maximum_width &&
        height >= minimum_height && height <= maximum_height;
}

} // namespace

void usage() {
    std::cout
        << "Usage: stuntmaster --game <path-to-cue> [--probe-guest] "
           "[--guest-budget <instructions>] [--show-frame|--capture-frame] "
           "[--run] [--input-config <path>] [--input-trace] [--frame-trace] "
           "[--memory-card <path-to-mcr>] "
           "[--load-quick-save <path-to-stsm>] "
           "[--frame-capture-trace] [--timing-trace] [--motion-trace] "
           "[--debug-overlay] "
           "[--experimental-host-menu|--no-experimental-host-menu] "
           "[--window-size <width>x<height>] "
           "[--render-size <width>x<height>] "
           "[--presentation-rate <hz>] [--guest-cpu-scale <1-4>] "
           "[--guest-update-rate <multiple of 30, 30-240>] [--retime-motion] "
           "[--retime-clock] [--interpreter-cpu|--cached-recompiler-cpu] "
           "[--persistent-framebuffer] "
           "[--cd-read-pacing [drive-speed-multiple]|--no-cd-read-pacing] "
           "[--widescreen-cull|--no-widescreen-cull]\n";
}

namespace {

std::filesystem::path runningExecutablePath(const char* argument_zero) {
#ifdef _WIN32
    std::vector<wchar_t> buffer(512U);
    for (;;) {
        const auto length = GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0U) {
            break;
        }
        if (length + 1U < buffer.size()) {
            return std::filesystem::path{
                std::wstring_view{buffer.data(), length}};
        }
        buffer.resize(buffer.size() * 2U);
    }
#endif
    return std::filesystem::path{argument_zero};
}

std::optional<Options> parseOptionsImpl(
    int argc,
    char** argv,
    const std::optional<std::filesystem::path>& launcher_settings_path) {
    Options result;
    if (launcher_settings_path) {
        result.launcher_settings_path = *launcher_settings_path;
        result.have_launcher_settings_path = true;
    }
    bool have_game = false;
    bool have_explicit_mode = false;
    bool command_line_window_size = false;
    bool command_line_render_size = false;
    bool command_line_presentation_rate = false;
    bool command_line_guest_update_rate = false;
    bool command_line_retime_motion = false;
    bool command_line_retime_clock = false;
    bool command_line_widescreen = false;
    bool command_line_cd_read_pacing = false;
    bool command_line_host_menu = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--game" && index + 1 < argc && !have_game) {
            result.game = std::filesystem::path{argv[++index]};
            have_game = true;
        } else if (argument == "--probe-guest" && !result.probe_guest) {
            result.probe_guest = true;
            have_explicit_mode = true;
        } else if (argument == "--show-frame" && !result.show_frame) {
            result.show_frame = true;
        } else if (argument == "--capture-frame" && !result.capture_frame) {
            result.capture_frame = true;
        } else if (argument == "--run" && !result.run_live) {
            result.run_live = true;
            have_explicit_mode = true;
        } else if (argument == "--input-trace" && !result.input_trace) {
            result.input_trace = true;
        } else if (argument == "--frame-trace" && !result.frame_trace) {
            result.frame_trace = true;
        } else if (
            argument == "--publication-trace" && !result.publication_trace) {
            result.publication_trace = true;
        } else if (
            argument == "--audio-capture" && index + 1 < argc &&
            !result.have_audio_capture) {
            result.audio_capture = argv[++index];
            result.have_audio_capture = true;
        } else if (
            argument == "--cd-read-pacing" &&
            !command_line_cd_read_pacing) {
            // The speed multiple is optional, so the bare flag keeps meaning
            // the console's own double speed. Only consume the next argument
            // when it is entirely a number, or `--cd-read-pacing
            // --widescreen-cull` would swallow the flag after it.
            result.cd_read_speed = 2U;
            command_line_cd_read_pacing = true;
            if (index + 1 < argc) {
                const std::string_view value{argv[index + 1]};
                std::uint32_t speed{};
                const auto parsed = std::from_chars(
                    value.data(), value.data() + value.size(), speed);
                if (parsed.ec == std::errc{} &&
                    parsed.ptr == value.data() + value.size()) {
                    if (speed == 0U || speed > 64U) {
                        return std::nullopt;
                    }
                    result.cd_read_speed = speed;
                    ++index;
                }
            }
        } else if (argument == "--no-cd-read-pacing" &&
                   !command_line_cd_read_pacing) {
            result.cd_read_speed = 0U;
            command_line_cd_read_pacing = true;
        } else if (
            argument == "--publication-dump" && index + 1 < argc &&
            !result.have_publication_dump) {
            const std::string_view value{argv[++index]};
            const auto parsed = std::from_chars(
                value.data(),
                value.data() + value.size(),
                result.publication_dump);
            if (parsed.ec != std::errc{} ||
                parsed.ptr != value.data() + value.size() ||
                result.publication_dump == 0U) {
                return std::nullopt;
            }
            result.have_publication_dump = true;
        } else if (
            argument == "--script-input" && index + 1 < argc &&
            result.scripted_input.empty()) {
            if (!parseScriptedInput(argv[++index], result.scripted_input)) {
                return std::nullopt;
            }
        } else if (
            argument == "--frame-capture-trace" &&
            !result.frame_capture_trace) {
            result.frame_capture_trace = true;
        } else if (argument == "--timing-trace" && !result.timing_trace) {
            result.timing_trace = true;
        } else if (argument == "--motion-trace" && !result.motion_trace) {
            result.motion_trace = true;
        } else if (argument == "--ledge-watch" && !result.ledge_watch) {
            result.ledge_watch = true;
        } else if (argument == "--ledge-trace" && !result.ledge_trace) {
            result.ledge_trace = true;
        } else if (
            argument == "--ledge-trace-inputs" &&
            !result.ledge_trace_inputs) {
            result.ledge_trace = true;
            result.ledge_trace_inputs = true;
        } else if (argument == "--debug-overlay" && !result.debug_overlay) {
            result.debug_overlay = true;
        } else if (
            argument == "--interpreter-cpu" && !result.interpreter_cpu) {
            if (result.cached_recompiler_cpu) {
                return std::nullopt;
            }
            result.interpreter_cpu = true;
        } else if (
            argument == "--cached-recompiler-cpu" &&
            !result.cached_recompiler_cpu) {
            if (result.interpreter_cpu) {
                return std::nullopt;
            }
            result.cached_recompiler_cpu = true;
        } else if (
            argument == "--experimental-host-menu" &&
            !command_line_host_menu) {
            result.experimental_host_menu = true;
            command_line_host_menu = true;
        } else if (argument == "--no-experimental-host-menu" &&
                   !command_line_host_menu) {
            result.experimental_host_menu = false;
            command_line_host_menu = true;
        } else if (argument == "--retime-motion" && !result.retime_motion) {
            result.retime_motion = true;
            command_line_retime_motion = true;
        } else if (argument == "--retime-clock" && !result.retime_clock) {
            result.retime_clock = true;
            command_line_retime_clock = true;
        } else if (
            argument == "--watch-writes" && index + 2 < argc &&
            !result.have_watch_writes) {
            // Ranks the guest instructions that write a chosen address range.
            // Used to find what fills a staging buffer before it is uploaded.
            const std::string_view begin_text{argv[++index]};
            const std::string_view end_text{argv[++index]};
            const auto begin = std::from_chars(
                begin_text.data(), begin_text.data() + begin_text.size(),
                result.watch_begin, 16);
            const auto end = std::from_chars(
                end_text.data(), end_text.data() + end_text.size(),
                result.watch_end, 16);
            if (begin.ec != std::errc{} || end.ec != std::errc{} ||
                begin.ptr != begin_text.data() + begin_text.size() ||
                end.ptr != end_text.data() + end_text.size() ||
                result.watch_end <= result.watch_begin) {
                return std::nullopt;
            }
            result.have_watch_writes = true;
        } else if (
            argument == "--widescreen-cull" && !command_line_widescreen) {
            result.widescreen_cull = true;
            command_line_widescreen = true;
        } else if (argument == "--no-widescreen-cull" &&
                   !command_line_widescreen) {
            result.widescreen_cull = false;
            command_line_widescreen = true;
        } else if (argument == "--persistent-framebuffer") {
            // The persistent framebuffer is the default PsyCross path. The
            // flag remains accepted for compatibility with existing scripts.
            result.persistent_framebuffer = true;
        } else if (
            argument == "--replay-capture" && index + 1 < argc &&
            !result.have_replay_capture) {
            // Render one `--frame-capture-trace` dump back through the
            // presenter, with no disc and no guest. Presentation defects are
            // otherwise only reproducible by a human playing to the right
            // place, which makes every renderer question a round trip.
            result.replay_capture = argv[++index];
            result.have_replay_capture = true;
        } else if (
            argument == "--no-framebuffer-composite" &&
            !result.no_framebuffer_composite) {
            // Restores the older behaviour: VRAM copies refused and no base
            // layer, so a screen retail writes rather than draws is missing.
            result.no_framebuffer_composite = true;
            result.framebuffer_composite = false;
        } else if (
            (argument == "--eager-high-rate" || argument == "--eager-sixty") &&
            !result.eager_sixty) {
            // Diagnostic only: engage the high-frequency schedule immediately,
            // including while retail is loading. That is the state that
            // corrupts textures, so it is the state the corruption has to be
            // measured in. `--eager-sixty` is the original spelling from when
            // the mode was 60 Hz only.
            result.eager_sixty = true;
        } else if (
            argument == "--input-config" && index + 1 < argc &&
            !result.have_input_config) {
            result.input_config = std::filesystem::path{argv[++index]};
            result.have_input_config = true;
        } else if (
            argument == "--memory-card" && index + 1 < argc &&
            !result.have_memory_card) {
            result.memory_card = std::filesystem::path{argv[++index]};
            result.have_memory_card = true;
        } else if (
            argument == "--load-quick-save" && index + 1 < argc &&
            !result.have_load_quick_save) {
            result.load_quick_save = std::filesystem::path{argv[++index]};
            result.have_load_quick_save = true;
        } else if (
            argument == "--window-size" && index + 1 < argc &&
            !result.have_window_size) {
            if (!parseWindowSize(
                    argv[++index],
                    result.window_width,
                    result.window_height)) {
                return std::nullopt;
            }
            result.have_window_size = true;
            command_line_window_size = true;
        } else if (
            argument == "--render-size" && index + 1 < argc &&
            !result.have_render_size) {
            if (!parseWindowSize(
                    argv[++index],
                    result.render_width,
                    result.render_height)) {
                return std::nullopt;
            }
            result.have_render_size = true;
            command_line_render_size = true;
        } else if (
            argument == "--presentation-rate" && index + 1 < argc &&
            !result.have_presentation_rate) {
            const std::string_view value{argv[++index]};
            const auto parsed = std::from_chars(
                value.data(),
                value.data() + value.size(),
                result.presentation_rate);
            constexpr std::uint32_t minimum_rate = 30U;
            constexpr std::uint32_t maximum_rate = 360U;
            if (parsed.ec != std::errc{} ||
                parsed.ptr != value.data() + value.size() ||
                result.presentation_rate < minimum_rate ||
                result.presentation_rate > maximum_rate) {
                return std::nullopt;
            }
            result.have_presentation_rate = true;
            command_line_presentation_rate = true;
        } else if (
            argument == "--guest-cpu-scale" && index + 1 < argc &&
            !result.have_guest_cpu_scale) {
            const std::string_view value{argv[++index]};
            const auto parsed = std::from_chars(
                value.data(),
                value.data() + value.size(),
                result.guest_cpu_scale);
            if (parsed.ec != std::errc{} ||
                parsed.ptr != value.data() + value.size() ||
                result.guest_cpu_scale < 1U ||
                result.guest_cpu_scale > 4U) {
                return std::nullopt;
            }
            result.have_guest_cpu_scale = true;
        } else if (
            argument == "--guest-update-rate" && index + 1 < argc &&
            !result.have_guest_update_rate) {
            const std::string_view value{argv[++index]};
            const auto parsed = std::from_chars(
                value.data(),
                value.data() + value.size(),
                result.guest_update_rate);
            if (parsed.ec != std::errc{} ||
                parsed.ptr != value.data() + value.size() ||
                !stuntmaster::game::isSupportedGuestUpdateRate(
                    result.guest_update_rate)) {
                return std::nullopt;
            }
            result.have_guest_update_rate = true;
            command_line_guest_update_rate = true;
        } else if (argument == "--guest-budget" && index + 1 < argc) {
            const std::string_view value{argv[++index]};
            const auto parsed = std::from_chars(
                value.data(), value.data() + value.size(), result.guest_budget);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
                result.guest_budget == 0U) {
                return std::nullopt;
            }
        } else {
            return std::nullopt;
        }
    }

    const auto launcher_settings = launcher_settings_path
        ? loadLauncherSettings(*launcher_settings_path)
        : std::optional<LauncherSettings>{};
    if (launcher_settings) {
        if (!have_game) {
            std::string cue_error;
            const auto cue = findGameCue(
                launcher_settings->game_directory, cue_error);
            if (cue) {
                result.game = *cue;
                have_game = true;
            }
        }
        if (!have_explicit_mode && !result.have_replay_capture) {
            result.run_live = true;
        }
        // Persisted presentation choices are live-play defaults. Diagnostic
        // probes only inherit the disc path, keeping their command lines and
        // validation deterministic.
        if (result.run_live) {
            if (!command_line_widescreen) {
                result.widescreen_cull = launcher_settings->widescreen;
            }
            const auto width = renderWidthFor(
                launcher_settings->resolution_height,
                result.widescreen_cull);
            if (!command_line_window_size) {
                result.window_width = width;
                result.window_height = launcher_settings->resolution_height;
                result.have_window_size = true;
            }
            if (!command_line_render_size) {
                result.render_width = width;
                result.render_height = launcher_settings->resolution_height;
                result.have_render_size = true;
            }
            // Persisted 30 Hz is an initial selection, not a request to
            // remove the runtime capability. Keep F7 and the Display menu able
            // to switch back to 60 Hz on a later run.
            if (!command_line_retime_motion) {
                result.retime_motion = true;
            }
            if (!command_line_retime_clock) {
                result.retime_clock = true;
            }
            if (launcher_settings->sixty_hz) {
                if (!command_line_presentation_rate) {
                    result.presentation_rate = 60U;
                    result.have_presentation_rate = true;
                }
                if (!command_line_guest_update_rate) {
                    result.guest_update_rate = 60U;
                    result.have_guest_update_rate = true;
                }
            }
        }
    }
    if (result.run_live && !result.have_input_config &&
        launcher_settings_path) {
        result.input_config =
            launcher_settings_path->parent_path() / "input.ini";
        result.have_input_config = true;
    }
    if (!result.have_render_size && result.widescreen_cull) {
        // An explicit initial widescreen request selects the matching default
        // target. Otherwise presentation starts in original 4:3 regardless of
        // the independently sized host window.
        result.render_width = 1280U;
        result.render_height = 720U;
    }
    if (result.have_replay_capture) {
        // A standalone presentation mode: no disc, no guest, no other mode.
        const auto conflicts = have_game || result.probe_guest ||
            result.run_live || result.show_frame || result.capture_frame ||
            // A replay has no guest and therefore no frame boundary to trace.
            result.publication_trace ||
            result.guest_budget != 1'000'000U;
        if (conflicts) {
            return std::nullopt;
        }
        return result;
    }
    if (!have_game ||
        (!result.probe_guest && !result.run_live &&
         result.guest_budget != 1'000'000U) ||
        ((result.show_frame || result.capture_frame) && !result.probe_guest) ||
        (result.show_frame && result.capture_frame) ||
        (result.run_live &&
         (result.probe_guest || result.show_frame || result.capture_frame)) ||
        (result.input_trace && !result.run_live) ||
        (result.frame_trace && !result.run_live && !result.probe_guest) ||
        (result.frame_capture_trace && !result.run_live) ||
        (result.timing_trace && !result.run_live) ||
        (result.motion_trace && !result.run_live && !result.probe_guest) ||
        (result.debug_overlay && !result.run_live) ||
        (result.have_load_quick_save &&
         !result.run_live && !result.probe_guest) ||
        // Both install the runtime's single memory-write sink.
        (result.motion_trace && result.frame_trace) ||
        (result.have_input_config && !result.run_live) ||
        (result.have_presentation_rate && !result.run_live) ||
        // A high guest update rate needs CPU headroom to match, and a probe is
        // how that is measured, so this is not live-only.
        (result.have_guest_cpu_scale &&
         !result.run_live && !result.probe_guest) ||
        (result.have_guest_update_rate &&
         !result.run_live && !result.probe_guest) ||
        (result.retime_motion && !result.run_live && !result.probe_guest) ||
        (result.retime_clock && !result.run_live && !result.probe_guest) ||
        (result.eager_sixty &&
         result.guest_update_rate <= stuntmaster::game::retail_update_rate) ||
        (result.widescreen_cull && !result.run_live && !result.probe_guest) ||
        (result.have_watch_writes && result.motion_trace) ||
        (result.have_watch_writes && result.frame_trace) ||
        (result.have_watch_writes && !result.run_live && !result.probe_guest) ||
        (result.have_window_size &&
         !result.run_live && !result.show_frame && !result.capture_frame) ||
        (result.have_render_size &&
         !result.run_live && !result.show_frame && !result.capture_frame) ||
        // Live play publishes to a window, while a trace needs a probe.
        (result.publication_trace && !result.probe_guest) ||
        // Scripted input drives the probe; live play has a real controller.
        (!result.scripted_input.empty() && !result.probe_guest) ||
        // The PsyCross path dumps PUBLISHED_PSYX.BMP by running the real
        // presenter headlessly, which requires a probe.
        (result.have_publication_dump && !result.probe_guest) ||
        (result.have_audio_capture && !result.probe_guest) ||
        (result.run_live && result.guest_budget != 1'000'000U)) {
        return std::nullopt;
    }
    return result;
}

} // namespace

std::optional<Options> parseOptions(int argc, char** argv) {
    if (argc <= 0 || argv == nullptr || argv[0] == nullptr) {
        return std::nullopt;
    }
    const auto executable = runningExecutablePath(argv[0]);
    auto executable_stem = executable.stem().wstring();
    std::transform(
        executable_stem.begin(), executable_stem.end(),
        executable_stem.begin(),
        [](wchar_t character) {
            return static_cast<wchar_t>(std::towlower(character));
        });
    // Unit-test and helper binaries share the build directory with the game.
    // Only the actual game executable should silently consume live defaults.
    const auto settings_path = executable_stem == L"stuntmaster"
        ? std::optional{launcherSettingsPath(executable)}
        : std::optional<std::filesystem::path>{};
    return parseOptionsImpl(argc, argv, settings_path);
}

std::optional<Options> parseOptions(
    int argc,
    char** argv,
    const std::filesystem::path& launcher_settings_path) {
    return parseOptionsImpl(argc, argv, launcher_settings_path);
}

} // namespace stuntmaster::app
