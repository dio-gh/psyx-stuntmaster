#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace stuntmaster::app {

struct Options {
    std::filesystem::path game;
    std::filesystem::path input_config;
    std::filesystem::path memory_card;
    std::filesystem::path load_quick_save;
    std::filesystem::path launcher_settings_path;
    std::uint64_t guest_budget{1'000'000U};
    std::uint32_t window_width{1280U};
    std::uint32_t window_height{720U};
    // Original 4:3 presentation is the default. The window remains widescreen
    // so a later in-game widescreen toggle does not resize it.
    std::uint32_t render_width{960U};
    std::uint32_t render_height{720U};
    // Zero means use the refresh rate reported by the window's host display.
    std::uint32_t presentation_rate{};
    std::uint32_t guest_cpu_scale{1U};
    std::uint32_t guest_update_rate{30U};
    bool probe_guest{};
    bool show_frame{};
    bool capture_frame{};
    bool run_live{};
    bool input_trace{};
    bool frame_trace{};
    bool frame_capture_trace{};
    bool timing_trace{};
    bool motion_trace{};
    bool ledge_trace{};
    // A pure-read per-frame readout of the player's ledge state. It patches
    // nothing, so unlike a sampling trampoline it cannot change what it
    // measures.
    bool ledge_watch{};
    // The two operand samples, kept separately switchable while the base set
    // is the one with play time behind it.
    bool ledge_trace_inputs{};
    bool debug_overlay{};
    // Enabled for the supported image by default. The negative command-line
    // flag keeps a clean opt-out for compatibility investigations.
    bool experimental_host_menu{true};
    bool retime_motion{};
    bool retime_clock{};
    // Named for the original 60 Hz-only experiment; it now applies at any
    // requested high-frequency rate.
    bool eager_sixty{};
    bool widescreen_cull{};
    // Retail writes some screens into the display page by DMA instead of
    // drawing them, so the page has to be composited back or those screens are
    // simply absent. On by default; the negative flag is for A/B testing.
    bool framebuffer_composite{true};
    bool no_framebuffer_composite{};
    // PsyCross drives PsyX as a display controller over a persistent
    // framebuffer. See docs/PRESENTATION_REDESIGN.md.
    bool persistent_framebuffer{true};
    // One line per frame handed to presentation. Works headlessly so scripted
    // probes can locate a publication for capture.
    bool publication_trace{};
    // Button presses scheduled against the guest VBlank counter, so a headless
    // probe can reach a screen that needs input to get to. Every presentation
    // question so far has needed a human to play to the right place and then
    // describe what they saw; this makes those screens reproducible.
    struct ScriptedInput {
        std::uint64_t vblank{};
        std::uint16_t active_low_buttons{};
    };
    std::vector<ScriptedInput> scripted_input;
    // Which published frame to write to PUBLISHED.BMP. Without it the file is
    // rewritten every publication and only the last survives, which is useless
    // for a screen that is on display for a moment.
    std::uint64_t publication_dump{};
    bool have_publication_dump{};
    // Drive-speed multiple for CD read pacing: 0 off, 1 single speed, 2 the
    // console's own double speed, higher to shorten the bulk of the wait while
    // retaining a 2x upload-quiet tail before 60 Hz gameplay. Opt-in because
    // it changes reference VBlank numbers.
    std::uint32_t cd_read_speed{2U};
    // Write everything the SPU mixes to a 16-bit stereo 44.1 kHz WAV. This is
    // how audio is checked before any output device exists, the same way
    // PUBLISHED.BMP checks a frame before a window does.
    std::filesystem::path audio_capture;
    bool have_audio_capture{};
    std::filesystem::path replay_capture;
    bool have_replay_capture{};
    bool have_watch_writes{};
    std::uint32_t watch_begin{};
    std::uint32_t watch_end{};
    bool have_input_config{};
    bool have_memory_card{};
    bool have_load_quick_save{};
    bool have_window_size{};
    bool have_render_size{};
    bool have_presentation_rate{};
    bool have_guest_cpu_scale{};
    bool have_guest_update_rate{};
    bool have_launcher_settings_path{};
};

// F7 and the Display menu can request the supported 60 Hz schedule whenever
// both compensation families were armed at launch. Starting at 30 Hz is not a
// restriction; it is the expected retail-speed initial state.
[[nodiscard]] constexpr bool highFrequencyRuntimeControlAvailable(
    const Options& options) noexcept {
    return options.retime_motion && options.retime_clock;
}

[[nodiscard]] constexpr std::uint32_t presentationRateForDisplay(
    const Options& options,
    std::uint32_t detected_refresh_rate) noexcept {
    if (options.have_presentation_rate) {
        return options.presentation_rate;
    }
    return detected_refresh_rate != 0U ? detected_refresh_rate : 60U;
}

// `--script-input 1800:start,2400:cross` — a comma-separated list of
// `vblank:button` presses. The button names are retail's, and each press is
// held for `scripted_input_hold_vblanks` so the guest's edge detection sees it
// the same way it sees a human.
inline constexpr std::uint64_t scripted_input_hold_vblanks = 8U;

void usage();

std::optional<Options> parseOptions(int argc, char** argv);

// Testable entry point for the launcher's persisted defaults. The normal
// overload reads stuntmaster.ini beside the running executable.
std::optional<Options> parseOptions(
    int argc,
    char** argv,
    const std::filesystem::path& launcher_settings_path);

} // namespace stuntmaster::app
