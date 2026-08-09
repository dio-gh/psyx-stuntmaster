#include "diagnostics.hpp"
#include "frame_io.hpp"
#include "gpu_frame.hpp"
#include "launcher_settings.hpp"
#include "movie_request_bridge.hpp"
#include "options.hpp"
#include "quick_save.hpp"

#include "stuntmaster/core/bounded_latest_mailbox.hpp"
#include "stuntmaster/core/audio_ring.hpp"
#include "stuntmaster/core/error.hpp"
#include "stuntmaster/core/sha256.hpp"
#include "stuntmaster/core/state_archive.hpp"
#include "stuntmaster/disc/iso9660.hpp"
#include "stuntmaster/game/guest_schedule.hpp"
#include "stuntmaster/game/retail_hle.hpp"
#include "stuntmaster/game/retail_patch.hpp"
#include "stuntmaster/game/retiming.hpp"
#include "stuntmaster/game/supported_game.hpp"
#include "stuntmaster/presentation/movie_player.hpp"
#include "stuntmaster/presentation/gpu_publication.hpp"
#include "stuntmaster/psx/bios_hle.hpp"
#include "stuntmaster/psx/executable.hpp"
#include "stuntmaster/psx/gpu_command_bridge.hpp"
#include "stuntmaster/psx/gpu_command_decoder.hpp"
#include "stuntmaster/psx/r3000_runtime.hpp"
#include "stuntmaster/psx/spu.hpp"
#include "stuntmaster/presentation/audio_output.hpp"
#include "stuntmaster/presentation/psycross_presenter.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <ratio>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace stuntmaster::app;

namespace {

void replayCapturedFrameWithPresenter(const Options& options) {
#ifdef STUNTMASTER_HAS_PSYCROSS
    const auto frame = loadCapturedFrame(options.replay_capture);
    std::cout << "replay_capture=" << options.replay_capture.string()
              << "\nreplay_packets=" << std::dec
              << frame.packets.size() << "\nreplay_display="
              << frame.display_x << ',' << frame.display_y << ','
              << frame.display_width << 'x' << frame.display_height
              << '\n';
    stuntmaster::presentation::PsyCrossPresenter presenter{
        {},
        options.window_width,
        options.window_height,
        options.render_width,
        options.render_height};
    presenter.setCompositeDisplayPage(options.framebuffer_composite);
    presenter.captureScreenshot(
        frame.vram,
        frame.packets,
        frame.display_x,
        frame.display_y,
        frame.display_width,
        frame.display_height);
    std::cout << "frame_capture=SCREENSHOT.BMP\n";
#else
    (void)options;
    throw stuntmaster::core::Error{
        "--replay-capture requires STUNTMASTER_ENABLE_PSYCROSS=ON"};
#endif
}

struct LoadedGame {
    stuntmaster::disc::Iso9660Image image;
    stuntmaster::core::Sha256Digest executable_hash;
    stuntmaster::psx::Executable executable;
};

[[nodiscard]] LoadedGame loadGame(const Options& options) {
    auto image = stuntmaster::disc::Iso9660Image::open(
        options.game);
    const auto system_cnf = image.readFile("SYSTEM.CNF");
    const auto boot_path = stuntmaster::psx::parseBootPath(asText(system_cnf));
    const auto executable_bytes = image.readFile(boot_path);
    const auto executable_hash = stuntmaster::core::sha256(executable_bytes);
    const auto game = stuntmaster::game::identify(
        image.volumeId(), boot_path, executable_hash);
    if (!game) {
        throw stuntmaster::core::Error{
            "Unsupported game revision (volume=" + image.volumeId() +
            ", boot=" + boot_path + ", executable_sha256=" +
            stuntmaster::core::toHex(executable_hash) + ")"};
    }
    auto executable =
        stuntmaster::psx::Executable::parse(executable_bytes);
    const auto& header = executable.header();

    std::cout << "Stuntmaster PC bootstrap\n";
    std::cout << "volume=" << image.volumeId() << '\n';
    std::cout << "boot=" << boot_path << '\n';
    std::cout << "executable_sha256="
              << stuntmaster::core::toHex(executable_hash) << '\n';
    printHex("initial_pc", header.initial_pc);
    printHex("text_address", header.text_address);
    printHex("text_size", header.text_size);
    printHex("stack_address", header.stack_address);
    return {
        std::move(image),
        executable_hash,
        std::move(executable),
    };
}

void runGuestSession(const Options& option_values, LoadedGame& loaded_game) {
            const auto* options = &option_values;
            auto& image = loaded_game.image;
            const auto& executable_hash = loaded_game.executable_hash;
            const auto& executable = loaded_game.executable;
            stuntmaster::psx::R3000Runtime runtime;
            std::vector<std::vector<std::uint32_t>> current_gpu_packets;
            std::vector<std::vector<std::uint32_t>> completed_gpu_packets;
            std::vector<stuntmaster::presentation::GpuReplaySegment>
                current_gpu_segments;
            std::vector<std::vector<std::uint32_t>>
                pending_gpu_segment_packets;
            RetainedGpuFrame renderable_gpu_frame;
            std::array<std::vector<std::uint32_t>, 5U>
                persistent_gpu_environment;
            bool pending_gpu_display_flip = false;
            bool pending_vblank_boundary = false;
            // GP1(05) writes, and how many of them actually moved the scanout
            // origin. Retail double-buffers by moving it, so a large first
            // count with a tiny second one means the page flip has stopped.
            // How long retail has gone without flipping. Through a level load it
            // stops entirely, which is what the Psy-Q path has to notice.
            std::vector<std::int16_t> captured_audio;
            // Half a second of stereo, which is far more slack than the device
            // needs and cheap enough not to matter.
            stuntmaster::core::AudioRing audio_ring{
                stuntmaster::presentation::AudioOutput::sample_rate};
            std::uint32_t vblanks_since_display_flip = 0U;
            std::uint64_t published_vram_revision = 0U;
            std::uint64_t gpu_display_starts = 0U;
            std::uint64_t gpu_display_origin_changes = 0U;
            std::uint32_t last_display_start = 0xFFFFFFFFU;
            std::uint64_t gpu_segment_mismatches = 0U;
            std::uint32_t published_display_x = 0U;
            std::uint32_t published_display_y = 0U;
            std::uint32_t published_display_width = 0U;
            std::uint32_t published_display_height = 0U;
            bool published_display_valid = false;
            std::ofstream gpu_transfer_trace;
            if (options->frame_trace) {
                gpu_transfer_trace.open(
                    std::filesystem::path{"build"} /
                        "LIVE_GPU_TRANSFERS.log",
                    std::ios::trunc);
            }
            std::uint64_t traced_image_uploads = 0U;
            std::uint64_t traced_vram_copies = 0U;
            std::uint64_t traced_dma2_transfers = 0U;
            stuntmaster::psx::GpuCommandDecoder* gpu_decoder_view = nullptr;
            stuntmaster::psx::GpuCommandDecoder gpu_decoder{
                [&current_gpu_packets,
                 &current_gpu_segments,
                 &pending_gpu_segment_packets,
                 &persistent_gpu_environment,
                 &gpu_decoder_view](
                    std::span<const std::uint32_t> packet) {
                    current_gpu_packets.emplace_back(packet.begin(), packet.end());
                    pending_gpu_segment_packets.emplace_back(
                        packet.begin(), packet.end());
                    const auto opcode =
                        static_cast<std::uint8_t>(packet.front() >> 24U);
                    if (opcode >= 0xE1U && opcode <= 0xE5U) {
                        persistent_gpu_environment[opcode - 0xE1U].assign(
                            packet.begin(), packet.end());
                    }
                    if (opcode < 0x20U || opcode > 0x7FU ||
                        gpu_decoder_view == nullptr) {
                        return;
                    }
                    const auto revision =
                        gpu_decoder_view->vramRevision();
                    if (current_gpu_segments.empty() ||
                        current_gpu_segments.back().vram_revision != revision) {
                        stuntmaster::presentation::GpuReplaySegment segment;
                        segment.packets =
                            std::move(pending_gpu_segment_packets);
                        pending_gpu_segment_packets.clear();
                        const auto decoded_vram = gpu_decoder_view->vram();
                        segment.vram.assign(
                            decoded_vram.begin(), decoded_vram.end());
                        segment.vram_revision = revision;
                        current_gpu_segments.push_back(std::move(segment));
                    } else {
                        auto& segment_packets =
                            current_gpu_segments.back().packets;
                        std::ranges::move(
                            pending_gpu_segment_packets,
                            std::back_inserter(segment_packets));
                        pending_gpu_segment_packets.clear();
                    }
                }};
            gpu_decoder_view = &gpu_decoder;
            std::cout << "renderer=psycross\n";
            gpu_decoder.setApplyVramCopies(options->framebuffer_composite);
            const auto trace_gpu_transfer =
                [&gpu_decoder,
                 &gpu_transfer_trace,
                 &traced_image_uploads,
                 &traced_vram_copies] {
                    if (!gpu_transfer_trace.is_open()) {
                        return;
                    }
                    if (gpu_decoder.imageUploadCount() !=
                        traced_image_uploads) {
                        traced_image_uploads =
                            gpu_decoder.imageUploadCount();
                        const auto& upload =
                            gpu_decoder.lastImageUpload();
                        gpu_transfer_trace
                            << "upload " << traced_image_uploads << ' '
                            << upload.x << ' ' << upload.y << ' '
                            << upload.width << ' ' << upload.height << '\n';
                    }
                    if (gpu_decoder.vramCopyCount() !=
                        traced_vram_copies) {
                        traced_vram_copies = gpu_decoder.vramCopyCount();
                        const auto& copy = gpu_decoder.lastVramCopy();
                        gpu_transfer_trace
                            << "copy " << traced_vram_copies << ' '
                            << copy.source_x << ' ' << copy.source_y << ' '
                            << copy.destination_x << ' '
                            << copy.destination_y << ' '
                            << copy.width << ' ' << copy.height << '\n';
                    }
                };
            stuntmaster::psx::GpuCommandBridge gpu{
                [&gpu_decoder,
                 &pending_gpu_display_flip,
                 &vblanks_since_display_flip,
                 &gpu_display_starts,
                 &gpu_display_origin_changes,
                 &last_display_start,
                 &trace_gpu_transfer](
                    bool gp1, std::uint32_t command) {
                    if (!gp1) {
                        gpu_decoder.pushGp0(command);
                        trace_gpu_transfer();
                    } else if ((command >> 24U) == 0x05U) {
                        pending_gpu_display_flip = true;
                        vblanks_since_display_flip = 0U;
                        ++gpu_display_starts;
                        if ((command & 0x7FFFFU) != last_display_start) {
                            ++gpu_display_origin_changes;
                            last_display_start = command & 0x7FFFFU;
                        }
                    }
                },
                &runtime,
                {},
                [&gpu_transfer_trace,
                 &traced_dma2_transfers,
                 &runtime](
                    std::uint32_t address,
                    std::uint32_t block_control,
                    std::uint32_t channel_control) {
                    if (!gpu_transfer_trace.is_open()) {
                        return;
                    }
                    gpu_transfer_trace
                        << "dma " << ++traced_dma2_transfers << ' '
                        << address << ' ' << block_control << ' '
                        << channel_control << ' '
                        << runtime.state().pc << '\n';
                }};
            // Sound RAM and the SPU register file. Retail writes those
            // registers directly, so the bridge forwards what it does not own
            // and hands over each DMA4 payload.
            stuntmaster::psx::Spu spu;
            gpu.setDma4Sink(
                [&spu](std::span<const std::uint16_t> words) {
                    spu.writeSoundRam(words);
                });
            gpu.setFallbackBus(&spu);
            runtime.attachMmioBus(&gpu);
            runtime.loadExecutable(executable);
            // Declared here rather than beside the other guest services because
            // the schedule owns its CD completion rate.
            stuntmaster::game::RetailHle retail_hle{image};
            retail_hle.setHostMenuRenderSizeFamilies(
                std::array<
                    stuntmaster::game::HostRenderSize,
                    stuntmaster::game::host_render_size_count>{{
                    {640U, 480U},
                    {960U, 720U},
                    {1440U, 1080U},
                    {1920U, 1440U}}},
                std::array<
                    stuntmaster::game::HostRenderSize,
                    stuntmaster::game::host_render_size_count>{{
                    {854U, 480U},
                    {1280U, 720U},
                    {1920U, 1080U},
                    {2560U, 1440U}}});
            retail_hle.setHostMenuState(
                options->guest_update_rate,
                {options->render_width, options->render_height},
                options->widescreen_cull);
            if (options->experimental_host_menu) {
                retail_hle.enableExperimentalHostMenu();
                std::cout << "experimental_host_menu=enabled\n";
            }
            // Two schedules, switched as a set: retail's own, and the
            // requested high-frequency one.
            //
            // Loading at a high rate delivers different bytes to VRAM: the
            // same texture tile at the same address decodes to a clean floor
            // with 57 distinct halfwords at 30 Hz and to static with 199 at
            // 60 Hz. Retail's loader has a frame-rate-dependent sequencing
            // assumption, so run its explicit load/transition states at the
            // cadence they were written for and take the high rate as soon as
            // the guest publishes a steady state handler. GPU uploads cannot
            // select the cadence: palette animation, pause UI and RAM texture
            // flipbooks all upload during legitimate gameplay. The
            // compensating patches must go with the gate: leaving them applied
            // at the retail rate would run the loader slowly, which is a
            // different wrong sequencing rather than none.
            //
            // The locked schedule is always retail's — a 60 Hz display with the
            // two-VBlank gate — no matter which rate was requested, because
            // that ratio between the loader's VBlank-driven producer and its
            // game-loop consumer is the thing being preserved.
            const auto retail_schedule =
                stuntmaster::game::retailGuestSchedule();
            auto high_frequency_schedule =
                stuntmaster::game::guestScheduleFor(
                    options->guest_update_rate);
            auto high_frequency_requested =
                high_frequency_schedule.update_rate >
                retail_schedule.update_rate;
            bool high_frequency_active = false;
            // Everything `applySchedule` owns. All of it is worker-owned once
            // the guest thread starts, and only a mode switch changes it.
            std::uint64_t guest_instructions_per_vblank = 0U;
            std::chrono::steady_clock::duration host_vblank_step{};
            stuntmaster::game::RatePacer audio_frame_pacer;
            const auto applySchedule =
                [&](const stuntmaster::game::GuestSchedule& schedule) {
                    guest_instructions_per_vblank =
                        static_cast<std::uint64_t>(
                            schedule.instructions_per_vblank) *
                        options->guest_cpu_scale;
                    host_vblank_step = std::chrono::duration_cast<
                        std::chrono::steady_clock::duration>(
                        std::chrono::duration<double>{
                            1.0 /
                            static_cast<double>(schedule.vblank_rate)});
                    audio_frame_pacer =
                        stuntmaster::game::audioFramePacer(schedule.vblank_rate);
                    retail_hle.setVblankRate(schedule.vblank_rate);
                };

            // Retiming is host-owned: attach the RetimeHooks table for whatever
            // `--retime-motion`/`--retime-clock` select. The boot hooks (always
            // live) are merged once from the selected spans; the overlay subset
            // is rebuilt each frame by `refreshRetimeHookTable` (an overlay load
            // can swap which fingerprint matches). Guest RAM stays byte-clean;
            // only the swap gate and the non-retime patches remain byte patches.
            stuntmaster::game::RetimeHooks retime_hook_table;
            std::vector<stuntmaster::game::RetimeHook> retime_boot_hooks;
            std::vector<stuntmaster::game::RetimeHook> retime_active_hooks;
            if (options->retime_motion || options->retime_clock) {
                const auto append =
                    [&](std::span<const stuntmaster::game::RetimeHook> span) {
                        retime_boot_hooks.insert(
                            retime_boot_hooks.end(), span.begin(), span.end());
                    };
                if (options->retime_motion) {
                    append(stuntmaster::game::retimeMotionHooks());
                }
                if (options->retime_clock) {
                    append(stuntmaster::game::retimeClockHooks());
                    append(stuntmaster::game::retimeLedgeHooks());
                    append(stuntmaster::game::retimeObjectHooks());
                }
                std::sort(
                    retime_boot_hooks.begin(), retime_boot_hooks.end(),
                    [](const stuntmaster::game::RetimeHook& a,
                       const stuntmaster::game::RetimeHook& b) {
                        return a.pc < b.pc;
                    });
                retime_active_hooks.resize(
                    retime_boot_hooks.size() +
                    stuntmaster::game::retimeOverlayHooks().size());
                runtime.setRetimeHooks(&retime_hook_table);
                std::cerr << "retime_hooks=on boot=" << std::dec
                          << retime_boot_hooks.size() << " overlay="
                          << stuntmaster::game::retimeOverlayHooks().size()
                          << '\n';
            }
            std::uint32_t retime_hooks_live = 0U;
            // Rebuilds the live hook table for the current frame: the boot hooks
            // plus the overlay hooks whose fingerprint currently matches. Only
            // the overlay subset changes (an overlay load/unload).
            const auto refreshRetimeHookTable = [&] {
                if (!retime_hook_table.active()) {
                    return;
                }
                const auto overlay = options->retime_clock
                    ? stuntmaster::game::retimeOverlayHooks()
                    : std::span<const stuntmaster::game::RetimeOverlayHook>{};
                const auto count = stuntmaster::game::buildActiveRetimeHooks(
                    runtime, retime_boot_hooks, overlay, retime_active_hooks);
                retime_hook_table.setHooks(
                    std::span<const stuntmaster::game::RetimeHook>{
                        retime_active_hooks.data(), count});
                // The live count changes only when the game swaps an overlay,
                // so reporting the transition names the level boundary without a
                // line per frame.
                if (count != retime_hooks_live) {
                    retime_hooks_live = static_cast<std::uint32_t>(count);
                    std::cerr << "retime_hooks_live=" << std::dec << count
                              << " of "
                              << (retime_boot_hooks.size() +
                                  (options->retime_clock
                                       ? stuntmaster::game::retimeOverlayHooks()
                                             .size()
                                       : 0U))
                              << '\n';
                }
            };
            const auto setHighFrequencyMode = [&](bool enable) {
                const auto& gate = stuntmaster::game::thirtyHertzSwapGate();
                auto applied = true;
                if (enable) {
                    // The gate is still a byte patch (a bound change, not
                    // retiming); the divisor lives host-side and the hook table
                    // replaces every retime trampoline. The divisor is published
                    // before the gate releases, so no guest update can run
                    // unlocked with a stale one.
                    applied =
                        stuntmaster::game::applyRetailPatch(runtime, gate);
                    if (applied) {
                        retime_hook_table.program(
                            high_frequency_schedule.retime_divisor);
                        retime_hook_table.setActive(true);
                        refreshRetimeHookTable();
                    }
                } else {
                    retime_hook_table.setActive(false);
                    applied =
                        stuntmaster::game::revertRetailPatch(runtime, gate);
                }
                if (applied) {
                    high_frequency_active = enable;
                    applySchedule(
                        enable ? high_frequency_schedule : retail_schedule);
                }
                return applied;
            };
            // The overlay-resident retiming is host-side now: rebuild the live
            // hook table (boot + fingerprint-matched overlays) each frame. An
            // overlay load can swap which overlay is resident at a reused
            // address, and an absent overlay is expected and costs only its
            // fingerprint-window reads.
            const auto refreshOverlayRetiming = [&] {
                refreshRetimeHookTable();
            };
            // A hang only ends once, so report on the transition rather than
            // per frame. The last `Obstacle::LedgeCheck` verdict before the
            // drop is the interesting part: which of its four conditions was
            // false names the cause directly.
            // Everything the ledge hang is decided from, read straight out of
            // the player each frame. `thePlayer` is the same pointer
            // `--motion-trace` resolves.
            std::uint32_t previous_ledge_state = 0xFFFFFFFFU;
            std::uint64_t ledge_watch_frame = 0U;
            const auto reportLedgeWatch = [&] {
                if (!options->ledge_watch) {
                    return;
                }
                ++ledge_watch_frame;
                std::uint32_t player = 0U;
                if (!runtime.read32(0x800DD6B4U, player) || player == 0U) {
                    return;
                }
                const auto field = [&](std::uint32_t offset) {
                    std::uint32_t word = 0U;
                    static_cast<void>(runtime.read32(player + offset, word));
                    return static_cast<std::int32_t>(word);
                };
                const auto state =
                    static_cast<std::uint32_t>(field(0x164U));
                // 0x17 is the ledge hang and 0x18 the pull-up. Report those
                // every frame, and every state change, and nothing else.
                const auto interesting = state == 0x17U || state == 0x18U;
                if (!interesting && state == previous_ledge_state) {
                    return;
                }
                std::cerr << "ledge_watch f=" << std::dec << ledge_watch_frame
                          << " state=0x" << std::hex << state << std::dec
                          << " pos=" << field(0x1CU) << ',' << field(0x20U)
                          << ',' << field(0x24U)
                          << " next=" << field(0x7CU) << ',' << field(0x80U)
                          << ',' << field(0x84U)
                          << " vel=" << field(0x64U) << ',' << field(0x68U)
                          << ',' << field(0x6CU)
                          << " carried=" << field(0x70U) << ',' << field(0x74U)
                          << ',' << field(0x78U)
                          << " mass=" << field(0x34U)
                          << " grav=" << field(0xC4U)
                          << " yaw=" << field(0x2CU)
                          << " flags=0x" << std::hex << field(0x170U)
                          << std::dec << '\n';
                previous_ledge_state = state;
            };
            stuntmaster::game::LedgeTraceSample previous_ledge_trace;
            const auto reportLedgeTrace = [&] {
                if (!options->ledge_trace) {
                    return;
                }
                const auto sample =
                    stuntmaster::game::readLedgeTrace(runtime);
                if (!sample.has_value()) {
                    return;
                }
                if (sample->letgo_from_latch ==
                        previous_ledge_trace.letgo_from_latch &&
                    sample->letgo_from_ticket ==
                        previous_ledge_trace.letgo_from_ticket) {
                    previous_ledge_trace = *sample;
                    return;
                }
                const auto source = sample->letgo_from_ticket !=
                        previous_ledge_trace.letgo_from_ticket
                    ? "no_ticket_reissued"
                    : "latch_read_input_as_away";
                std::cerr << "ledge_letgo=" << source << std::dec
                          << " from_latch=" << sample->letgo_from_latch
                          << " from_ticket=" << sample->letgo_from_ticket
                          << " ledge_checks=" << sample->check_calls
                          << " (+"
                          << (sample->check_calls -
                              previous_ledge_trace.check_calls)
                          << ") last_verdict="
                          << stuntmaster::game::describeLedgeCheckFlags(
                                 sample->check_flags)
                          << " contact_y=" << sample->check_contact_y
                          << " facing_dot=" << sample->facing_dot
                          << " normal=" << sample->normal_x << ','
                          << sample->normal_y << ',' << sample->normal_z
                          << " facing=" << sample->facing_x << ','
                          << sample->facing_z << '\n';
                previous_ledge_trace = *sample;
            };
            applySchedule(retail_schedule);
            // The ledge trace observes boot-executable sites, which are
            // resident for the whole run, so it is applied once rather than
            // refreshed. It is independent of the guest rate on purpose: the
            // 30 Hz run is the control the 60 Hz reading is compared against.
            if (options->ledge_trace) {
                const auto install =
                    [&](std::span<
                        const stuntmaster::game::RetailTrampoline* const>
                            patches) {
                        for (const auto* patch : patches) {
                            if (!stuntmaster::game::applyRetailTrampoline(
                                    runtime, *patch)) {
                                throw stuntmaster::core::Error{
                                    std::string{"--ledge-trace refused: "} +
                                    std::string{patch->name} +
                                    " did not match its fingerprint"};
                            }
                        }
                    };
                install(stuntmaster::game::ledgeTracePatches());
                if (options->ledge_trace_inputs) {
                    install(stuntmaster::game::ledgeTraceInputPatches());
                }
                std::cout << "ledge_trace=on inputs="
                          << (options->ledge_trace_inputs ? "on" : "off")
                          << '\n';
            }
            if (high_frequency_requested) {
                const auto& gate = stuntmaster::game::thirtyHertzSwapGate();
                const auto site =
                    stuntmaster::game::readPatchSite(runtime, gate);
                if (!site.has_value() || *site != gate.original_word) {
                    throw stuntmaster::core::Error{
                        "--guest-update-rate refused: the retail swap gate "
                        "fingerprint did not match"};
                }
                std::cout << std::dec << "guest_update_rate="
                          << high_frequency_schedule.update_rate
                          << " vblank_rate="
                          << high_frequency_schedule.vblank_rate
                          << " retime_divisor="
                          << high_frequency_schedule.retime_divisor
                          << " instructions_per_vblank="
                          << high_frequency_schedule.instructions_per_vblank
                          << '\n';
                // Running the game loop k times per authored frame is k times
                // the CPU work a second. Past what the guest CPU delivers the
                // loop cannot finish a step per emulated refresh and every
                // authored timeline runs slow, which looks like a retiming bug
                // and is not one.
                //
                // Raising --guest-cpu-scale clears that ceiling and walks into
                // the host's own: the guest is interpreted, so the scale
                // multiplies what the host must deliver per wall-clock second.
                // Report the sustainable rate rather than recommending a
                // scale, because overshooting either ceiling looks identical.
                const auto sustainable =
                    stuntmaster::game::sustainableGuestUpdateRate(
                        options->guest_cpu_scale);
                std::cout << "guest_cpu_scale=" << options->guest_cpu_scale
                          << " sustainable_update_rate=" << sustainable
                          << '\n';
                if (high_frequency_schedule.update_rate > sustainable) {
                    std::cout
                        << "warning: " << high_frequency_schedule.update_rate
                        << " Hz needs more guest CPU than --guest-cpu-scale "
                        << options->guest_cpu_scale << " provides, so "
                        << sustainable
                        << " Hz is the highest rate that keeps full speed. A "
                           "higher scale raises the guest budget but demands "
                           "the same multiple of real-time host "
                           "interpretation, which can slow audio and gameplay "
                           "together instead. --debug-overlay reports the "
                           "measured guest speed.\n";
                }
                // Whether the host can deliver a raised scale is a property of
                // the machine, not of the schedule, so state the requirement
                // rather than guess at a threshold. Live play is where it
                // bites; a probe has no clock to keep up with.
                if (options->guest_cpu_scale > 1U && options->run_live) {
                    std::cout
                        << "note: --guest-cpu-scale "
                        << options->guest_cpu_scale
                        << " requires the host to interpret "
                        << options->guest_cpu_scale << "x real time ("
                        << (static_cast<std::uint64_t>(
                                stuntmaster::game::console_cpu_rate) *
                            options->guest_cpu_scale) /
                            1'000'000U
                        << "M guest instructions a second). If it cannot, the "
                           "guest runs slow, and audio and gameplay slow "
                           "together; check SPEED in --debug-overlay.\n";
                }
                if (options->eager_sixty) {
                    if (!setHighFrequencyMode(true)) {
                        throw stuntmaster::core::Error{
                            "--eager-high-rate refused: a retail fingerprint "
                            "did not match"};
                    }
                    std::cout << "retail_patch=" << gate.name
                              << " (eager, including during loads)\n";
                } else {
                    std::cout
                        << "retail_patch=" << gate.name
                        << " (high-frequency mode follows the retail Game "
                           "state; load and transition handlers stay at "
                           "retail cadence)\n";
                }
            }
            auto current_render_width = options->render_width;
            auto current_render_height = options->render_height;
            auto widescreen_x_limit =
                stuntmaster::game::widescreenXLimit(
                    current_render_width, current_render_height);
            auto widescreen_block_x_limit =
                stuntmaster::game::widescreenBlockVisibilityXLimit(
                    current_render_width, current_render_height);
            bool widescreen_cull_active = options->widescreen_cull &&
                widescreen_x_limit > 0x200U;
            if (!stuntmaster::game::setWidescreenCull(
                    runtime,
                    widescreen_x_limit,
                    widescreen_block_x_limit,
                    widescreen_cull_active)) {
                throw stuntmaster::core::Error{
                    "--widescreen-cull refused: a retail screen-cull "
                    "fingerprint did not match"};
            }
            if (widescreen_cull_active) {
                std::cout << "retail_patch="
                          << stuntmaster::game::widescreenBlockVisibilityLimit(
                                 widescreen_block_x_limit).name
                          << '\n';
                for (const auto& patch :
                     stuntmaster::game::widescreenBlockCull(
                         widescreen_x_limit)) {
                    std::cout << "retail_patch=" << patch.name << '\n';
                }
                for (const auto& patch :
                     stuntmaster::game::widescreenModelCull(
                         widescreen_x_limit)) {
                    std::cout << "retail_patch=" << patch.name << '\n';
                }
                std::cout << "retail_patch=widescreen_lower_bounds x17\n";
            }
            if (options->widescreen_cull) {
                std::cout << "widescreen_x_limit=" << std::dec
                          << widescreen_x_limit << '\n';
            }
            if (options->experimental_host_menu) {
                retail_hle.setHostMenuWidescreenCull(
                    widescreen_cull_active);
            }
            // Ranks the guest instructions that write a chosen address range,
            // so a staging buffer's producers can be compared between update
            // rates.
            std::map<std::uint32_t, std::uint64_t> watched_writers;
            std::uint64_t watched_writes = 0U;
            std::uint64_t watched_uploads_seen = 0U;
            if (options->have_watch_writes) {
                const auto begin = options->watch_begin;
                const auto end = options->watch_end;
                runtime.setMemoryWriteSink(
                    [&watched_writers,
                     &watched_writes,
                     &watched_uploads_seen,
                     &gpu_decoder,
                     begin,
                     end](
                        std::uint32_t address,
                        std::uint32_t size,
                        std::uint32_t,
                        std::uint32_t pc) {
                        if (address + size <= begin || address >= end) {
                            return;
                        }
                        // Interleaving is the point: report how far the
                        // producer had got each time the GPU consumed an
                        // upload. A producer that is still writing after the
                        // upload of the region it fills is the race.
                        const auto uploads = gpu_decoder.imageUploadCount();
                        if (uploads != watched_uploads_seen) {
                            watched_uploads_seen = uploads;
                            std::cerr << "watch_interleave uploads=" << uploads
                                      << " producer_writes=" << watched_writes
                                      << " next_write=0x" << std::hex
                                      << address << std::dec << std::endl;
                        }
                        ++watched_writes;
                        // The sink reports the instruction after the store.
                        ++watched_writers[pc - 4U];
                    });
            }
            MotionWatch motion_watch;
            if (options->motion_trace) {
                runtime.setMemoryWriteSink(
                    [&motion_watch](
                        std::uint32_t address,
                        std::uint32_t size,
                        std::uint32_t value,
                        std::uint32_t pc) {
                        motion_watch.observe(address, size, value, pc);
                    });
            }
            if (gpu_transfer_trace.is_open()) {
                runtime.setMemoryWriteSink(
                    [&gpu_transfer_trace](
                        std::uint32_t address,
                        std::uint32_t size,
                        std::uint32_t value,
                        std::uint32_t pc) {
                        const auto physical = address & 0x1FFFFFFFU;
                        const auto begin = physical & 0x001FFFFFU;
                        const auto end =
                            static_cast<std::uint64_t>(begin) + size;
                        constexpr std::uint32_t watched_begin = 0x001F5800U;
                        constexpr std::uint32_t watched_end = 0x001F5B00U;
                        if (begin < watched_end && end > watched_begin) {
                            gpu_transfer_trace
                                << "write " << address << ' ' << size << ' '
                                << value << ' ' << pc << '\n';
                        }
                    });
            }
            const auto memory_card_path = options->have_memory_card
                ? options->memory_card
                : std::filesystem::path{"saves"} / "SLUS-00684.mcr";
            stuntmaster::psx::MemoryCard memory_card{memory_card_path};
            std::cout << "memory_card=" << memory_card_path.string() << '\n';
            stuntmaster::psx::BiosHle bios{
                [](std::string_view text) {
                    std::cout << "guest_tty=" << text << '\n';
                },
                {},
                &memory_card};
            stuntmaster::psx::R3000ExecutionBoundaries execution_boundaries;
            execution_boundaries.add(
                stuntmaster::game::RetailHle::executionBoundaries());
            execution_boundaries.add(
                stuntmaster::psx::BiosHle::executionBoundaries());
            // These are diagnostics rather than host services. Keeping them as
            // batch boundaries preserves the existing counters without forcing
            // every other guest instruction back through the machine loop.
            execution_boundaries.add(std::array{
                0x8009FDECU, // WaitForLayer DrawSync delay slot
                0x8009FEE4U, // QueueLayer
                0x8009FF7CU, // QueueSwap
            });
            MovieRequestBridge movie_requests;
            retail_hle.setCdReadSpeed(options->cd_read_speed);
            auto presentation_rate = presentationRateForDisplay(*options, 60U);
            if (gpu_transfer_trace.is_open()) {
                retail_hle.setCdReadSink(
                    [&gpu_transfer_trace](
                        std::uint32_t lba,
                        std::uint32_t sectors,
                        std::uint32_t destination,
                        std::uint32_t return_address) {
                        gpu_transfer_trace
                            << "cdread " << lba << ' ' << sectors << ' '
                            << destination << ' ' << return_address << '\n';
                    });
            }
#ifdef STUNTMASTER_HAS_PSYCROSS
            std::optional<stuntmaster::presentation::PsyCrossPresenter>
                live_presenter;
            std::optional<stuntmaster::presentation::AudioOutput> audio_output;
            if (options->run_live) {
                live_presenter.emplace(
                    options->input_config,
                    options->window_width,
                    options->window_height,
                    options->render_width,
                    options->render_height,
                    options->frame_capture_trace);
                if (!options->have_presentation_rate) {
                    presentation_rate = presentationRateForDisplay(
                        *options, live_presenter->displayRefreshRate());
                }
                std::cout << "presentation_rate=" << presentation_rate
                          << (options->have_presentation_rate
                                  ? " (override)\n"
                                  : " (host display)\n");
                live_presenter->setCompositeDisplayPage(
                    options->framebuffer_composite);
                // Opened after the presenter, so a machine with no sound device
                // still reaches a window and reports the reason once.
                audio_output.emplace();
                retail_hle.setMovieTransitionSink(
                    [&movie_requests] {
                        movie_requests.transitionAndWait();
                    });
                retail_hle.setMoviePrepareSink(
                    [&movie_requests](
                        const stuntmaster::game::RetailMoviePlayRequest&
                            request) {
                        movie_requests.prepareAndWait(request);
                    });
                retail_hle.setMoviePlaySink(
                    [&movie_requests](
                        const stuntmaster::game::RetailMoviePlayRequest&
                            request) {
                        movie_requests.requestAndWait(request);
                    });
            }
            // Stage 0 of the presentation redesign: a faithful, headlessly
            // scriptable capture of the enhanced (PsyCross) present path. A
            // scripted probe reaches the target publication, runs the real
            // present() into a hidden GL context, and reads the composed render
            // target back to PUBLISHED_PSYX.BMP -- the same live present state
            // the on-screen frame uses, not the throwaway end-of-run presenter
            // `--capture-frame` builds. This is the precondition for changing
            // the renderer: verify every change against this before a play-test.
            std::optional<stuntmaster::presentation::PsyCrossPresenter>
                headless_presenter;
            if (!options->run_live && options->have_publication_dump) {
                headless_presenter.emplace(
                    options->input_config,
                    options->window_width,
                    options->window_height,
                    options->render_width,
                    options->render_height,
                    /*capture_frame_trace=*/false,
                    /*hidden_window=*/true);
                headless_presenter->setCompositeDisplayPage(
                    options->framebuffer_composite);
            }
#else
            if (options->run_live) {
                throw stuntmaster::core::Error{
                    "live execution requires "
                    "STUNTMASTER_ENABLE_PSYCROSS=ON"};
            }
#endif
            stuntmaster::psx::R3000RunResult result;
            std::uint64_t guest_batch_calls = 0U;
            std::uint64_t guest_batched_instructions = 0U;
            std::uint64_t guest_idle_instructions = 0U;
            std::optional<stuntmaster::psx::R3000State> suspended_state;
            // `applySchedule` owns `guest_instructions_per_vblank`: the CPU runs
            // at console speed at every rate, so a faster emulated display gets
            // proportionally fewer instructions between refreshes rather than a
            // faster guest.
            std::uint64_t next_vblank = guest_instructions_per_vblank;
            std::uint64_t scheduled_vblanks = 0U;
            std::uint64_t scheduled_gpu_callbacks = 0U;
            // How often the guest reaches the DrawSync boundary at all, and how
            // often a completion was waiting there. GPU completions can only be
            // delivered at that point in this host, so these bound how many
            // completions retail can ever receive, no matter how many transfers
            // it submitted.
            std::uint64_t gpu_draw_sync_boundaries = 0U;
            std::uint64_t gpu_completions_waiting_at_boundary = 0U;
            // Retail's own render bookkeeping. One layer is one submitted
            // ordering table; one swap is one end-of-frame. They should track
            // each other, and a swap without a layer displays a page that
            // PutDrawEnv has cleared and nothing has drawn into.
            std::uint64_t retail_queue_layer_calls = 0U;
            std::uint64_t retail_queue_swap_calls = 0U;
            std::uint64_t retail_wait_for_layer_calls = 0U;
            std::uint64_t scheduled_vsync_callbacks = 0U;
            // Callbacks delivered while a CPU-to-VRAM transfer was still
            // consuming words. Retail's VSync callback writes GP0 through
            // PutDrawEnv/PutDispEnv, so any of these corrupts the texture that
            // was uploading.
            std::uint64_t callbacks_during_image_transfer = 0U;
            std::vector<std::uint32_t> uncovered_texture_pages;
            std::uint64_t previous_image_pixels = 0U;
            // Frames of quiet required before trusting that a load has
            // finished. Retail streams a level in bursts, so a single idle
            // frame is not enough.
            constexpr std::uint32_t upload_quiet_frames = 90U;
            std::uint32_t frames_since_upload = 0U;
            // The guest publishes the handler for its next Game state before
            // the current step returns. Cadence follows that explicit state,
            // never GPU uploads: palette animation, pause UI and RAM texture
            // flipbooks all use ordinary CPU-to-VRAM transfers in gameplay.
            constexpr std::uint32_t retail_game_manager_address =
                0x800DD668U;
            constexpr std::uint32_t retail_game_state_handler_offset = 0x1CU;
            const auto readRetailGameStateHandler = [&]()
                -> std::optional<std::uint32_t> {
                std::uint32_t game = 0U;
                std::uint32_t handler = 0U;
                if (!runtime.read32(retail_game_manager_address, game) ||
                    game == 0U ||
                    !runtime.read32(
                        game + retail_game_state_handler_offset, handler)) {
                    return std::nullopt;
                }
                return handler;
            };
            std::optional<std::uint32_t> last_retail_state_handler{
                0xFFFFFFFFU};
            std::uint64_t swap_gate_unlocks = 0U;
            std::uint64_t swap_gate_relocks = 0U;
            bool cd_load_tail_active = false;
            std::uint64_t cd_load_tail_entries = 0U;
            std::uint64_t cd_load_tail_publications = 0U;
            std::uint64_t scheduled_spu_callbacks = 0U;
            std::uint64_t scheduled_spu_irq_callbacks = 0U;
            std::uint64_t scheduled_host_presentations = 0U;
            std::uint64_t full_host_presentations = 0U;
            std::uint64_t repeated_host_presentations = 0U;
            std::uint64_t dropped_host_presentations = 0U;
            std::uint64_t retained_gpu_frame_sequence = 0U;
            std::optional<std::uint64_t> previous_world_frame_vblank;
            std::optional<std::uint64_t> previous_world_frame_instructions;
            std::optional<std::uint64_t> presented_gpu_frame_sequence;
            std::uint16_t previous_pad_one_buttons = 0xFFFFU;
            std::deque<std::uint32_t> pending_vsync_callbacks;
            // Retail's DrawSync callback is persistent, not one-shot:
            // `QueueLayer` installs it when the render queue goes idle-to-busy
            // and only `DSCallback` removes it, when the queue drains. So one
            // completion is owed per ordering table submitted while it is
            // installed, and they have to queue.
            //
            // The single pending slot this replaces could hold one, and
            // delivering it snapped a watermark to the current DMA2 transfer
            // count, retiring every submission that had arrived meanwhile.
            // Half of retail's layers were then never told they finished,
            // `WaitForLayer` spun, and the render queue advanced the display
            // environment while the draw environment stayed on one page.
            std::uint64_t observed_draw_transfers = 0U;
            std::uint64_t outstanding_gpu_completions = 0U;
            std::uint32_t outstanding_gpu_callback = 0U;
            std::uint64_t discarded_gpu_completions = 0U;
            bool bios_boundary = false;
            using HostClock = std::chrono::steady_clock;
            const auto host_epoch = HostClock::now();
            auto next_host_vblank = host_epoch;
            auto next_host_presentation = host_epoch;
            // `host_vblank_step` is owned by `applySchedule`, because the
            // emulated display rate is part of the schedule.
            const auto host_presentation_period =
                std::chrono::duration<double>{
                    1.0 /
                    static_cast<double>(presentation_rate)};
            const auto host_presentation_step =
                std::chrono::duration_cast<HostClock::duration>(
                    host_presentation_period);
            HostClock::duration total_host_presentation_time{};
            HostClock::duration maximum_host_presentation_time{};
            stuntmaster::core::BoundedLatestMailbox<RetainedGpuFrame, 2U>
                frame_mailbox;
            std::atomic<std::uint16_t> latest_pad_one_buttons{0xFFFFU};
            std::atomic<bool> guest_finished{};
            constexpr std::uint8_t quick_save_requested = 1U << 0U;
            constexpr std::uint8_t quick_load_requested = 1U << 1U;
            constexpr std::uint8_t timestamped_quick_save_requested = 1U << 2U;
            std::atomic<std::uint8_t> quick_save_requests{};
            std::atomic<bool> retime_toggle_requested{};
            std::atomic<bool> widescreen_toggle_requested{};
            std::atomic<std::uint32_t> host_menu_update_rate_requested{};
            // Guest-menu callbacks publish render changes back to the worker
            // first. It updates the aspect-dependent reversible patches at a
            // VBlank boundary before the main thread reallocates the target.
            std::uint64_t host_menu_render_configuration_requested{};
            std::atomic<std::uint64_t> host_menu_render_size_requested{};
            // 0 means no request; 1 means retail/narrow culling and 2 means
            // guarded widescreen culling.
            std::atomic<std::uint8_t> host_menu_widescreen_requested{};
            std::atomic<bool> published_widescreen_cull_active{
                widescreen_cull_active};
            std::atomic<std::uint32_t> published_requested_update_rate{
                high_frequency_requested
                    ? high_frequency_schedule.update_rate
                    : retail_schedule.update_rate};
            // High dword: selected resolution height. Low bits: 60 Hz and
            // widescreen. The main thread owns filesystem persistence.
            std::atomic<std::uint64_t> runtime_launcher_settings_requested{};
            const auto requestLauncherSettingsPersistence = [&]() {
                if (!options->have_launcher_settings_path) {
                    return;
                }
                const auto packed =
                    (static_cast<std::uint64_t>(current_render_height) << 32U) |
                    (high_frequency_requested ? 1U : 0U) |
                    (widescreen_cull_active ? 2U : 0U);
                runtime_launcher_settings_requested.store(
                    packed, std::memory_order_release);
            };
            enum class QuickSaveNotification : std::uint8_t {
                none,
                save_created,
                timestamped_save_created,
                load_restored,
                save_failed,
                load_failed,
                thirty_hz_mode,
                sixty_hz_mode,
                retime_toggle_unavailable,
                widescreen_mode,
                narrow_cull_mode,
                widescreen_toggle_unavailable,
            };
            std::atomic<QuickSaveNotification> quick_save_notification{
                QuickSaveNotification::none};
            std::atomic<std::uint64_t> quick_load_generation{};
            std::exception_ptr guest_exception;

            if (options->experimental_host_menu) {
                retail_hle.setHostMenuSink(
                    [&](const stuntmaster::game::HostMenuEvent& event) {
                        if (event.command == stuntmaster::game::
                                HostMenuCommand::guest_update_rate) {
                            host_menu_update_rate_requested.store(
                                event.value, std::memory_order_release);
                            return;
                        }
                        if (event.command == stuntmaster::game::
                                HostMenuCommand::widescreen_cull) {
                            host_menu_widescreen_requested.store(
                                event.value != 0U ? 2U : 1U,
                                std::memory_order_release);
                            return;
                        }
                        const auto packed =
                            (static_cast<std::uint64_t>(event.width) << 32U) |
                            event.height;
                        host_menu_render_configuration_requested = packed;
                    });
            }

            std::uint32_t quick_save_flags = 0U;
            quick_save_flags |= options->framebuffer_composite
                ? quick_save_flag_framebuffer_composite : 0U;
            quick_save_flags |= options->retime_motion
                ? quick_save_flag_retime_motion : 0U;
            quick_save_flags |= options->retime_clock
                ? quick_save_flag_retime_clock : 0U;
            quick_save_flags |= options->eager_sixty
                ? quick_save_flag_eager_high_rate : 0U;
            const auto currentQuickSaveCompatibility = [&] {
                const auto flags = quick_save_flags |
                    (widescreen_cull_active
                        ? quick_save_flag_widescreen_cull
                        : 0U);
                return QuickSaveCompatibility{
                    high_frequency_requested
                        ? high_frequency_schedule.update_rate
                        : retail_schedule.update_rate,
                    options->guest_cpu_scale,
                    current_render_width,
                    current_render_height,
                    options->cd_read_speed,
                    flags,
                };
            };
            const auto default_quick_save_path = defaultQuickSavePath();
            std::uint64_t probe_instruction_origin = 0U;

            // A save file is built and applied only on the guest worker. The
            // object copy also makes file loading transactional: parsing and
            // validation mutate a candidate, never the running machine.
            struct MachineQuickSave {
                stuntmaster::psx::R3000Runtime runtime;
                stuntmaster::psx::GpuCommandDecoder gpu_decoder;
                stuntmaster::psx::GpuCommandBridge gpu;
                stuntmaster::psx::Spu spu;
                stuntmaster::game::RetailHle retail_hle;
                stuntmaster::psx::BiosHle bios;
                stuntmaster::game::RetimeState retime_state;
                std::vector<stuntmaster::game::RetimeHook>
                    retime_active_hooks;
                stuntmaster::game::RatePacer audio_frame_pacer;
                stuntmaster::psx::R3000RunResult result;
                std::optional<stuntmaster::psx::R3000State> suspended_state;
                std::vector<std::vector<std::uint32_t>> current_gpu_packets;
                std::vector<std::vector<std::uint32_t>> completed_gpu_packets;
                std::vector<stuntmaster::presentation::GpuReplaySegment>
                    current_gpu_segments;
                std::vector<std::vector<std::uint32_t>>
                    pending_gpu_segment_packets;
                RetainedGpuFrame renderable_gpu_frame;
                std::array<std::vector<std::uint32_t>, 5U>
                    persistent_gpu_environment;
                std::deque<std::uint32_t> pending_vsync_callbacks;
                HostClock::duration host_vblank_step{};
                std::uint64_t guest_instructions_per_vblank{};
                std::uint64_t next_vblank{};
                std::uint64_t scheduled_vblanks{};
                std::uint64_t observed_draw_transfers{};
                std::uint64_t outstanding_gpu_completions{};
                std::uint32_t outstanding_gpu_callback{};
                std::uint64_t previous_image_pixels{};
                std::uint64_t published_vram_revision{};
                std::uint32_t frames_since_upload{};
                std::uint32_t vblanks_since_display_flip{};
                std::uint32_t last_display_start{};
                std::uint32_t retime_hooks_live{};
                bool high_frequency_active{};
                bool cd_load_tail_active{};
                bool pending_gpu_display_flip{};
                bool pending_vblank_boundary{};
                bool retime_active{};
            };

            const auto makeMachineQuickSave = [&] {
                return MachineQuickSave{
                    .runtime = runtime,
                    .gpu_decoder = gpu_decoder,
                    .gpu = gpu,
                    .spu = spu,
                    .retail_hle = retail_hle,
                    .bios = bios,
                    .retime_state = retime_hook_table.state(),
                    .retime_active_hooks = retime_active_hooks,
                    .audio_frame_pacer = audio_frame_pacer,
                    .result = result,
                    .suspended_state = suspended_state,
                    .current_gpu_packets = current_gpu_packets,
                    .completed_gpu_packets = completed_gpu_packets,
                    .current_gpu_segments = current_gpu_segments,
                    .pending_gpu_segment_packets =
                        pending_gpu_segment_packets,
                    .renderable_gpu_frame = renderable_gpu_frame,
                    .persistent_gpu_environment = persistent_gpu_environment,
                    .pending_vsync_callbacks = pending_vsync_callbacks,
                    .host_vblank_step = host_vblank_step,
                    .guest_instructions_per_vblank =
                        guest_instructions_per_vblank,
                    .next_vblank = next_vblank,
                    .scheduled_vblanks = scheduled_vblanks,
                    .observed_draw_transfers = observed_draw_transfers,
                    .outstanding_gpu_completions =
                        outstanding_gpu_completions,
                    .outstanding_gpu_callback = outstanding_gpu_callback,
                    .previous_image_pixels = previous_image_pixels,
                    .published_vram_revision = published_vram_revision,
                    .frames_since_upload = frames_since_upload,
                    .vblanks_since_display_flip = vblanks_since_display_flip,
                    .last_display_start = last_display_start,
                    .retime_hooks_live = retime_hooks_live,
                    .high_frequency_active = high_frequency_active,
                    .cd_load_tail_active = cd_load_tail_active,
                    .pending_gpu_display_flip = pending_gpu_display_flip,
                    .pending_vblank_boundary = pending_vblank_boundary,
                    .retime_active = retime_hook_table.active(),
                };
            };

            const auto applyMachineQuickSave = [&](const MachineQuickSave& saved) {
                runtime = saved.runtime;
                gpu_decoder = saved.gpu_decoder;
                gpu = saved.gpu;
                spu = saved.spu;
                retail_hle = saved.retail_hle;
                bios = saved.bios;
                retime_active_hooks = saved.retime_active_hooks;
                retime_hook_table.state() = saved.retime_state;
                retime_hook_table.setActive(saved.retime_active);
                runtime.attachMmioBus(&gpu);
                gpu.setFallbackBus(&spu);
                audio_frame_pacer = saved.audio_frame_pacer;
                result = saved.result;
                suspended_state = saved.suspended_state;
                current_gpu_packets = saved.current_gpu_packets;
                completed_gpu_packets = saved.completed_gpu_packets;
                current_gpu_segments = saved.current_gpu_segments;
                pending_gpu_segment_packets =
                    saved.pending_gpu_segment_packets;
                renderable_gpu_frame = saved.renderable_gpu_frame;
                persistent_gpu_environment = saved.persistent_gpu_environment;
                pending_vsync_callbacks = saved.pending_vsync_callbacks;
                host_vblank_step = saved.host_vblank_step;
                guest_instructions_per_vblank =
                    saved.guest_instructions_per_vblank;
                next_vblank = saved.next_vblank;
                scheduled_vblanks = saved.scheduled_vblanks;
                observed_draw_transfers = saved.observed_draw_transfers;
                outstanding_gpu_completions =
                    saved.outstanding_gpu_completions;
                outstanding_gpu_callback = saved.outstanding_gpu_callback;
                previous_image_pixels = saved.previous_image_pixels;
                published_vram_revision = saved.published_vram_revision;
                frames_since_upload = saved.frames_since_upload;
                vblanks_since_display_flip =
                    saved.vblanks_since_display_flip;
                last_display_start = saved.last_display_start;
                retime_hooks_live = saved.retime_hooks_live;
                high_frequency_active = saved.high_frequency_active;
                cd_load_tail_active = saved.cd_load_tail_active;
                pending_gpu_display_flip = saved.pending_gpu_display_flip;
                pending_vblank_boundary = saved.pending_vblank_boundary;
                bios_boundary = false;
                refreshRetimeHookTable();
                runtime.rebindStatePointers();
                // A debugger pause or a large snapshot copy must never create
                // wall-clock debt. Resume the restored guest one tick from now.
                next_host_vblank = HostClock::now();
                quick_load_generation.fetch_add(1U, std::memory_order_release);
            };

            const auto writePackets = [](
                stuntmaster::core::StateWriter& writer,
                const std::vector<std::vector<std::uint32_t>>& packets) {
                writer.pod(static_cast<std::uint64_t>(packets.size()));
                for (const auto& packet : packets) {
                    writer.vectorPod(packet);
                }
            };
            const auto readPackets = [](
                stuntmaster::core::StateReader& reader,
                std::vector<std::vector<std::uint32_t>>& packets) {
                std::uint64_t count{};
                if (!reader.pod(count) || count > 1'000'000U) {
                    return false;
                }
                packets.clear();
                packets.resize(static_cast<std::size_t>(count));
                for (auto& packet : packets) {
                    if (!reader.vectorPod(packet, 1U << 20U)) {
                        return false;
                    }
                }
                return true;
            };
            const auto writeSegments = [&writePackets](
                stuntmaster::core::StateWriter& writer,
                const std::vector<stuntmaster::presentation::GpuReplaySegment>&
                    segments) {
                writer.pod(static_cast<std::uint64_t>(segments.size()));
                for (const auto& segment : segments) {
                    writePackets(writer, segment.packets);
                    writer.vectorPod(segment.vram);
                    writer.pod(segment.vram_revision);
                }
            };
            const auto readSegments = [&readPackets](
                stuntmaster::core::StateReader& reader,
                std::vector<stuntmaster::presentation::GpuReplaySegment>&
                    segments) {
                std::uint64_t count{};
                if (!reader.pod(count) || count > 4096U) {
                    return false;
                }
                segments.clear();
                segments.resize(static_cast<std::size_t>(count));
                for (auto& segment : segments) {
                    if (!readPackets(reader, segment.packets) ||
                        !reader.vectorPod(
                            segment.vram,
                            static_cast<std::uint64_t>(
                                stuntmaster::psx::GpuCommandDecoder::vram_width) *
                                stuntmaster::psx::GpuCommandDecoder::vram_height) ||
                        !reader.pod(segment.vram_revision)) {
                        return false;
                    }
                }
                return true;
            };
            const auto writeFrame = [&writePackets, &writeSegments](
                stuntmaster::core::StateWriter& writer,
                const RetainedGpuFrame& frame) {
                writePackets(writer, frame.packets);
                writer.vectorPod(frame.vram);
                writeSegments(writer, frame.segments);
                writer.pod(frame.display_x);
                writer.pod(frame.display_y);
                writer.pod(frame.display_width);
                writer.pod(frame.display_height);
                writer.pod(frame.sequence);
                writer.pod(frame.guest_vblank);
                writer.pod(frame.guest_instructions);
                writer.pod(frame.retail_frame_rate);
                writer.pod(frame.retail_game_ticks);
                writer.pod(frame.retail_measured_frame_rate);
                writer.pod(frame.retime_hooks_armed);
                writer.pod(frame.retime_hooks_live);
                writer.pod(frame.high_frequency_active);
            };
            const auto readFrame = [&readPackets, &readSegments](
                stuntmaster::core::StateReader& reader,
                RetainedGpuFrame& frame) {
                constexpr std::uint64_t maximum_pixels = 1024U * 512U;
                return readPackets(reader, frame.packets) &&
                    reader.vectorPod(frame.vram, maximum_pixels) &&
                    readSegments(reader, frame.segments) &&
                    reader.pod(frame.display_x) &&
                    reader.pod(frame.display_y) &&
                    reader.pod(frame.display_width) &&
                    reader.pod(frame.display_height) &&
                    reader.pod(frame.sequence) &&
                    reader.pod(frame.guest_vblank) &&
                    reader.pod(frame.guest_instructions) &&
                    reader.pod(frame.retail_frame_rate) &&
                    reader.pod(frame.retail_game_ticks) &&
                    reader.pod(frame.retail_measured_frame_rate) &&
                    reader.pod(frame.retime_hooks_armed) &&
                    reader.pod(frame.retime_hooks_live) &&
                    reader.pod(frame.high_frequency_active);
            };

            const auto encodeQuickSave = [&](const MachineQuickSave& saved) {
                stuntmaster::core::StateWriter writer;
                saved.runtime.writeState(writer);
                saved.gpu_decoder.writeState(writer);
                saved.gpu.writeState(writer);
                saved.spu.writeState(writer);
                saved.retail_hle.writeState(writer);
                saved.bios.writeState(writer);
                writer.pod(saved.retime_state);
                writer.pod(saved.retime_active);
                writer.pod(saved.audio_frame_pacer.state());
                writer.pod(saved.result);
                writer.pod(saved.suspended_state.has_value());
                if (saved.suspended_state) {
                    writer.pod(*saved.suspended_state);
                }
                writePackets(writer, saved.current_gpu_packets);
                writePackets(writer, saved.completed_gpu_packets);
                writeSegments(writer, saved.current_gpu_segments);
                writePackets(writer, saved.pending_gpu_segment_packets);
                writeFrame(writer, saved.renderable_gpu_frame);
                for (const auto& environment :
                     saved.persistent_gpu_environment) {
                    writer.vectorPod(environment);
                }
                writer.pod(static_cast<std::uint64_t>(
                    saved.pending_vsync_callbacks.size()));
                for (const auto callback : saved.pending_vsync_callbacks) {
                    writer.pod(callback);
                }
                const auto host_vblank_nanoseconds =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        saved.host_vblank_step).count();
                writer.pod(host_vblank_nanoseconds);
                writer.pod(saved.guest_instructions_per_vblank);
                writer.pod(saved.next_vblank);
                writer.pod(saved.scheduled_vblanks);
                writer.pod(saved.observed_draw_transfers);
                writer.pod(saved.outstanding_gpu_completions);
                writer.pod(saved.outstanding_gpu_callback);
                writer.pod(saved.previous_image_pixels);
                writer.pod(saved.published_vram_revision);
                writer.pod(saved.frames_since_upload);
                writer.pod(saved.vblanks_since_display_flip);
                writer.pod(saved.last_display_start);
                writer.pod(saved.retime_hooks_live);
                writer.pod(saved.high_frequency_active);
                writer.pod(saved.cd_load_tail_active);
                writer.pod(saved.pending_gpu_display_flip);
                writer.pod(saved.pending_vblank_boundary);
                return writer.data();
            };

            const auto decodeQuickSave = [&](std::span<const std::byte> bytes) {
                auto loaded = makeMachineQuickSave();
                stuntmaster::core::StateReader reader{bytes};
                stuntmaster::game::RatePacer::State audio_pacer_state;
                bool has_suspended_state{};
                std::uint64_t pending_vsync_count{};
                std::int64_t host_vblank_nanoseconds{};
                if (!loaded.runtime.readState(reader) ||
                    !loaded.gpu_decoder.readState(reader) ||
                    !loaded.gpu.readState(reader) ||
                    !loaded.spu.readState(reader) ||
                    !loaded.retail_hle.readState(reader) ||
                    !loaded.bios.readState(reader) ||
                    !reader.pod(loaded.retime_state) ||
                    !reader.pod(loaded.retime_active) ||
                    !reader.pod(audio_pacer_state) ||
                    !loaded.audio_frame_pacer.restoreState(audio_pacer_state) ||
                    !reader.pod(loaded.result) ||
                    !reader.pod(has_suspended_state)) {
                    throw stuntmaster::core::Error{"truncated machine state"};
                }
                loaded.suspended_state.reset();
                if (has_suspended_state) {
                    stuntmaster::psx::R3000State state;
                    if (!reader.pod(state)) {
                        throw stuntmaster::core::Error{
                            "truncated suspended CPU state"};
                    }
                    loaded.suspended_state = state;
                }
                if (!readPackets(reader, loaded.current_gpu_packets) ||
                    !readPackets(reader, loaded.completed_gpu_packets) ||
                    !readSegments(reader, loaded.current_gpu_segments) ||
                    !readPackets(reader, loaded.pending_gpu_segment_packets) ||
                    !readFrame(reader, loaded.renderable_gpu_frame)) {
                    throw stuntmaster::core::Error{
                        "invalid GPU publication state"};
                }
                for (auto& environment : loaded.persistent_gpu_environment) {
                    if (!reader.vectorPod(environment, 16U)) {
                        throw stuntmaster::core::Error{
                            "invalid persistent GPU environment"};
                    }
                }
                if (!reader.pod(pending_vsync_count) ||
                    pending_vsync_count > 4096U) {
                    throw stuntmaster::core::Error{
                        "invalid pending VSync callback queue"};
                }
                loaded.pending_vsync_callbacks.clear();
                for (std::uint64_t index = 0U;
                     index < pending_vsync_count; ++index) {
                    std::uint32_t callback{};
                    if (!reader.pod(callback)) {
                        throw stuntmaster::core::Error{
                            "truncated VSync callback queue"};
                    }
                    loaded.pending_vsync_callbacks.push_back(callback);
                }
                if (!reader.pod(host_vblank_nanoseconds) ||
                    host_vblank_nanoseconds <= 0 ||
                    !reader.pod(loaded.guest_instructions_per_vblank) ||
                    !reader.pod(loaded.next_vblank) ||
                    !reader.pod(loaded.scheduled_vblanks) ||
                    !reader.pod(loaded.observed_draw_transfers) ||
                    !reader.pod(loaded.outstanding_gpu_completions) ||
                    !reader.pod(loaded.outstanding_gpu_callback) ||
                    !reader.pod(loaded.previous_image_pixels) ||
                    !reader.pod(loaded.published_vram_revision) ||
                    !reader.pod(loaded.frames_since_upload) ||
                    !reader.pod(loaded.vblanks_since_display_flip) ||
                    !reader.pod(loaded.last_display_start) ||
                    !reader.pod(loaded.retime_hooks_live) ||
                    !reader.pod(loaded.high_frequency_active) ||
                    !reader.pod(loaded.cd_load_tail_active) ||
                    !reader.pod(loaded.pending_gpu_display_flip) ||
                    !reader.pod(loaded.pending_vblank_boundary) ||
                    !reader.finished()) {
                    throw stuntmaster::core::Error{
                        "trailing or truncated scheduler state"};
                }
                loaded.host_vblank_step =
                    std::chrono::duration_cast<HostClock::duration>(
                        std::chrono::nanoseconds{host_vblank_nanoseconds});
                return loaded;
            };

            const auto saveQuickSave = [&](const std::filesystem::path& path) {
                const auto saved = makeMachineQuickSave();
                const auto payload = encodeQuickSave(saved);
                writeQuickSaveFile(
                    path,
                    executable_hash,
                    currentQuickSaveCompatibility(),
                    payload);
                std::cerr << "quick_save=written path=" << path.string()
                          << " bytes=" << payload.size() << " vblank="
                          << std::dec << scheduled_vblanks << " pc=0x"
                          << std::hex << runtime.state().pc << std::dec << '\n';
            };

            const auto loadQuickSave = [&](const std::filesystem::path& path) {
                const auto file = readQuickSaveFile(path);
                if (file.executable_hash != executable_hash) {
                    throw stuntmaster::core::Error{
                        "quick save belongs to a different game executable"};
                }
                if (!quickSaveSettingsCompatible(
                        file.compatibility,
                        currentQuickSaveCompatibility())) {
                    throw stuntmaster::core::Error{
                        "quick save uses incompatible launch settings"};
                }
                if (!stuntmaster::game::isSupportedGuestUpdateRate(
                        file.compatibility.guest_update_rate)) {
                    throw stuntmaster::core::Error{
                        "quick save uses an unsupported guest update rate"};
                }
                auto loaded = decodeQuickSave(file.payload);
                // Widescreen culling is a reversible presentation setting,
                // not a compatibility boundary. Normalize the candidate to
                // the current selection before it can replace the running
                // machine; this also accepts saves made with the older
                // exact-edge limit or at another render aspect.
                if (!stuntmaster::game::setWidescreenCull(
                        loaded.runtime,
                        widescreen_x_limit,
                        widescreen_block_x_limit,
                        widescreen_cull_active)) {
                    throw stuntmaster::core::Error{
                        "quick save contains an inconsistent widescreen "
                        "cull patch"};
                }
                // Do not change even host-owned runtime settings until the
                // complete payload has decoded successfully. The file's
                // requested rate is state metadata, so a save made after an
                // F7 toggle resumes in that mode regardless of the launch
                // command's initial rate.
                high_frequency_schedule =
                    stuntmaster::game::guestScheduleFor(
                        file.compatibility.guest_update_rate);
                high_frequency_requested =
                    high_frequency_schedule.update_rate >
                    retail_schedule.update_rate;
                published_requested_update_rate.store(
                    high_frequency_schedule.update_rate,
                    std::memory_order_release);
                applyMachineQuickSave(loaded);
                if (options->experimental_host_menu) {
                    retail_hle.setHostMenuUpdateRate(
                        high_frequency_requested
                            ? high_frequency_schedule.update_rate
                            : retail_schedule.update_rate);
                    retail_hle.setHostMenuWidescreenCull(
                        widescreen_cull_active);
                }
                std::cerr << "quick_load=restored path=" << path.string()
                          << " vblank=" << std::dec << scheduled_vblanks
                          << " pc=0x" << std::hex << runtime.state().pc
                          << std::dec << '\n';
            };

            if (options->have_load_quick_save) {
                loadQuickSave(options->load_quick_save);
                quick_save_notification.store(
                    QuickSaveNotification::load_restored,
                    std::memory_order_release);
                // A probe budget is work to execute from the loaded point,
                // not an absolute counter inherited from the original run.
                probe_instruction_origin = result.instructions;
            }

            // The main thread only publishes request bits. The worker consumes
            // them at an owed VBlank, where the whole machine is already at a
            // deterministic safe point. A successful load abandons the old
            // boundary: the caller must restart its loop so the restored
            // scheduler does not receive an extra VBlank.
            const auto serviceQuickSaveRequests = [&]() {
                const auto requested_host_menu_rate =
                    host_menu_update_rate_requested.exchange(
                        0U, std::memory_order_acq_rel);
                const auto host_menu_rate_changed =
                    requested_host_menu_rate != 0U &&
                    (requested_host_menu_rate >= 60U) !=
                        high_frequency_requested;
                if (retime_toggle_requested.exchange(
                        false, std::memory_order_acq_rel) ||
                    host_menu_rate_changed) {
                    bool rate_change_accepted = false;
                    const auto toggle_available =
                        highFrequencyRuntimeControlAvailable(*options);
                    if (!toggle_available) {
                        quick_save_notification.store(
                            QuickSaveNotification::retime_toggle_unavailable,
                            std::memory_order_release);
                    } else if (high_frequency_requested) {
                        if (setHighFrequencyMode(false)) {
                            high_frequency_requested = false;
                            high_frequency_schedule = retail_schedule;
                            published_requested_update_rate.store(
                                retail_schedule.update_rate,
                                std::memory_order_release);
                            quick_save_notification.store(
                                QuickSaveNotification::thirty_hz_mode,
                                std::memory_order_release);
                            rate_change_accepted = true;
                        } else {
                            quick_save_notification.store(
                                QuickSaveNotification::
                                    retime_toggle_unavailable,
                                std::memory_order_release);
                        }
                    } else {
                        high_frequency_schedule =
                            stuntmaster::game::guestScheduleFor(
                                stuntmaster::game::console_vblank_rate);
                        high_frequency_requested = true;
                        published_requested_update_rate.store(
                            high_frequency_schedule.update_rate,
                            std::memory_order_release);
                        const auto state_handler =
                            readRetailGameStateHandler();
                        const auto can_enable_now = options->eager_sixty ||
                            (state_handler.has_value() &&
                             stuntmaster::game::
                                 retailStateHandlerAllowsHighFrequency(
                                     *state_handler));
                        if (!can_enable_now ||
                            setHighFrequencyMode(true)) {
                            quick_save_notification.store(
                                QuickSaveNotification::sixty_hz_mode,
                                std::memory_order_release);
                            rate_change_accepted = true;
                        } else {
                            high_frequency_requested = false;
                            high_frequency_schedule = retail_schedule;
                            published_requested_update_rate.store(
                                retail_schedule.update_rate,
                                std::memory_order_release);
                            quick_save_notification.store(
                                QuickSaveNotification::
                                    retime_toggle_unavailable,
                                std::memory_order_release);
                        }
                    }
                    if (options->experimental_host_menu) {
                        retail_hle.setHostMenuUpdateRate(
                            high_frequency_requested
                                ? high_frequency_schedule.update_rate
                                : retail_schedule.update_rate);
                    }
                    if (rate_change_accepted) {
                        requestLauncherSettingsPersistence();
                    }
                }
                const auto requested_host_menu_widescreen =
                    host_menu_widescreen_requested.exchange(
                        0U, std::memory_order_acq_rel);
                const auto keyboard_widescreen_toggle =
                    widescreen_toggle_requested.exchange(
                        false, std::memory_order_acq_rel);
                if (requested_host_menu_widescreen != 0U ||
                    keyboard_widescreen_toggle) {
                    const auto requested = requested_host_menu_widescreen != 0U
                        ? requested_host_menu_widescreen == 2U
                        : !widescreen_cull_active;
                    const auto paired_render_size =
                        retail_hle.hostMenuRenderSizeForWidescreen(requested);
                    const auto requested_render_size =
                        (static_cast<std::uint64_t>(paired_render_size.width)
                             << 32U) |
                        paired_render_size.height;
                    const auto requested_width = static_cast<std::uint32_t>(
                        requested_render_size >> 32U);
                    const auto requested_height = static_cast<std::uint32_t>(
                        requested_render_size);
                    const auto requested_x_limit =
                        stuntmaster::game::widescreenXLimit(
                            requested_width, requested_height);
                    const auto requested_block_x_limit =
                        stuntmaster::game::widescreenBlockVisibilityXLimit(
                            requested_width, requested_height);
                    const auto available = requested_x_limit > 0x200U;
                    bool widescreen_change_accepted = false;
                    if ((requested && !available) ||
                        !stuntmaster::game::setWidescreenCull(
                            runtime,
                            requested_x_limit,
                            requested_block_x_limit,
                            requested && available)) {
                        quick_save_notification.store(
                            QuickSaveNotification::
                                widescreen_toggle_unavailable,
                            std::memory_order_release);
                    } else {
                        current_render_width = requested_width;
                        current_render_height = requested_height;
                        widescreen_x_limit = requested_x_limit;
                        widescreen_block_x_limit = requested_block_x_limit;
                        widescreen_cull_active = requested && available;
                        published_widescreen_cull_active.store(
                            widescreen_cull_active,
                            std::memory_order_release);
                        host_menu_render_size_requested.store(
                            requested_render_size,
                            std::memory_order_release);
                        widescreen_change_accepted = true;
                        quick_save_notification.store(
                            widescreen_cull_active
                                ? QuickSaveNotification::widescreen_mode
                                : QuickSaveNotification::narrow_cull_mode,
                            std::memory_order_release);
                    }
                    if (options->experimental_host_menu) {
                        retail_hle.setHostMenuState(
                            high_frequency_requested
                                ? high_frequency_schedule.update_rate
                                : retail_schedule.update_rate,
                            {current_render_width, current_render_height},
                            widescreen_cull_active);
                    }
                    if (widescreen_change_accepted) {
                        requestLauncherSettingsPersistence();
                    }
                }
                const auto requested_render_configuration = std::exchange(
                    host_menu_render_configuration_requested,
                    std::uint64_t{});
                if (requested_render_configuration != 0U) {
                    const auto requested_width = static_cast<std::uint32_t>(
                        requested_render_configuration >> 32U);
                    const auto requested_height = static_cast<std::uint32_t>(
                        requested_render_configuration);
                    const auto requested_x_limit =
                        stuntmaster::game::widescreenXLimit(
                            requested_width, requested_height);
                    const auto requested_block_x_limit =
                        stuntmaster::game::widescreenBlockVisibilityXLimit(
                            requested_width, requested_height);
                    bool render_change_accepted = false;
                    if (!stuntmaster::game::setWidescreenCull(
                            runtime,
                            requested_x_limit,
                            requested_block_x_limit,
                            widescreen_cull_active)) {
                        quick_save_notification.store(
                            QuickSaveNotification::
                                widescreen_toggle_unavailable,
                            std::memory_order_release);
                    } else {
                        current_render_width = requested_width;
                        current_render_height = requested_height;
                        widescreen_x_limit = requested_x_limit;
                        widescreen_block_x_limit = requested_block_x_limit;
                        host_menu_render_size_requested.store(
                            requested_render_configuration,
                            std::memory_order_release);
                        render_change_accepted = true;
                    }
                    retail_hle.setHostMenuState(
                        high_frequency_requested
                            ? high_frequency_schedule.update_rate
                            : retail_schedule.update_rate,
                        {current_render_width, current_render_height},
                        widescreen_cull_active);
                    if (render_change_accepted) {
                        requestLauncherSettingsPersistence();
                    }
                }
                const auto requests = quick_save_requests.exchange(
                    0U, std::memory_order_acq_rel);
                // Loading wins if both edges reached the worker in the same
                // tick; otherwise a simultaneous F5/F9 would replace the old
                // slot before it could be loaded.
                if ((requests & quick_load_requested) != 0U) {
                    try {
                        loadQuickSave(default_quick_save_path);
                        quick_save_notification.store(
                            QuickSaveNotification::load_restored,
                            std::memory_order_release);
                        return true;
                    } catch (const std::exception& error) {
                        std::cerr << "quick_load=failed: " << error.what()
                                  << '\n';
                        quick_save_notification.store(
                            QuickSaveNotification::load_failed,
                            std::memory_order_release);
                    }
                } else if (
                    (requests & timestamped_quick_save_requested) != 0U) {
                    try {
                        saveQuickSave(timestampedQuickSavePath());
                        quick_save_notification.store(
                            QuickSaveNotification::timestamped_save_created,
                            std::memory_order_release);
                    } catch (const std::exception& error) {
                        std::cerr << "quick_save=failed: " << error.what()
                                  << '\n';
                        quick_save_notification.store(
                            QuickSaveNotification::save_failed,
                            std::memory_order_release);
                    }
                } else if ((requests & quick_save_requested) != 0U) {
                    try {
                        saveQuickSave(default_quick_save_path);
                        quick_save_notification.store(
                            QuickSaveNotification::save_created,
                            std::memory_order_release);
                    } catch (const std::exception& error) {
                        std::cerr << "quick_save=failed: " << error.what()
                                  << '\n';
                        quick_save_notification.store(
                            QuickSaveNotification::save_failed,
                            std::memory_order_release);
                    }
                }
                return false;
            };

            const auto run_guest_loop = [&] {
                while (options->run_live ||
                       result.instructions - probe_instruction_origin <
                           options->guest_budget) {
                    // Publishing and ending a packet interval are two different
                    // things. A display flip ends the interval; when retail
                    // parks the display origin during a load, publish completed
                    // VRAM changes on a VBlank instead so written screens do not
                    // freeze. Avoid republishing unchanged state because the
                    // packet list grows throughout the load.
                    const auto flips_have_stopped =
                        stuntmaster::presentation::
                            shouldPublishStoppedFlipUpload(
                                pending_vblank_boundary,
                                vblanks_since_display_flip,
                                gpu_decoder.vramRevision(),
                                published_vram_revision);
                    const auto frame_boundary =
                        pending_gpu_display_flip || flips_have_stopped;
                    const auto interval_complete = pending_gpu_display_flip;
                    if (frame_boundary) {
                        pending_gpu_display_flip = false;
                        pending_vblank_boundary = false;
                        published_vram_revision = gpu_decoder.vramRevision();
                        {
                            {
                                // A display flip does not fall on a frame
                                // boundary. Retail queues its swap once a
                                // frame's drawing is complete, and
                                // VSCallback__Fe completes it a VBlank later,
                                // by which time retail has already drawn the
                                // next frame's backdrop. Every gameplay
                                // interval therefore reads as world (DFE=0),
                                // UI (DFE=1), then that backdrop (DFE=0), and
                                // compositing all three as one image puts the
                                // backdrop over the world it belongs behind
                                // and over the HUD.
                                //
                                // The ordered VRAM segments must partition the
                                // flat packet list exactly: the presenter
                                // replays the segments while the capture ring
                                // dumps the flat list, so a divergence shows as
                                // content missing on screen but present in the
                                // dump. Count mismatches rather than assume it.
                                if (options->run_live &&
                                    !options->persistent_framebuffer) {
                                    renderable_gpu_frame.packets =
                                        std::move(current_gpu_packets);
                                } else {
                                    // The persistent path needs every
                                    // publication to be self-contained: the
                                    // mailbox may drop a frame, but a persistent
                                    // framebuffer must not miss the commands it
                                    // carried. Copying leaves the accumulating
                                    // list intact through a flips-stopped load
                                    // so the next publication still reconstructs
                                    // the whole screen. It is cleared on the
                                    // display flip like the headless path.
                                    renderable_gpu_frame.packets =
                                        current_gpu_packets;
                                }
                                const auto decoded_vram = gpu_decoder.vram();
                                if (options->persistent_framebuffer) {
                                    // A stopped-flip publication is not an
                                    // interval boundary. Keep the ordered
                                    // segments for the next fallback and make
                                    // this mailbox value self-contained. The
                                    // helper also gives trailing uploads their
                                    // completed VRAM snapshot instead of
                                    // attaching them to the preceding draw.
                                    renderable_gpu_frame.segments =
                                        stuntmaster::presentation::
                                            buildSelfContainedReplaySegments(
                                                current_gpu_segments,
                                                pending_gpu_segment_packets,
                                                decoded_vram,
                                                gpu_decoder.vramRevision());
                                } else {
                                    if (!pending_gpu_segment_packets.empty() &&
                                        !current_gpu_segments.empty()) {
                                        auto& segment_packets =
                                            current_gpu_segments.back().packets;
                                        std::ranges::move(
                                            pending_gpu_segment_packets,
                                            std::back_inserter(segment_packets));
                                        pending_gpu_segment_packets.clear();
                                    }
                                    renderable_gpu_frame.segments =
                                        std::move(current_gpu_segments);
                                }
                                std::size_t segment_packets = 0U;
                                for (const auto& segment :
                                     renderable_gpu_frame.segments) {
                                    segment_packets += segment.packets.size();
                                }
                                // An empty segment list deliberately selects
                                // the flat final-snapshot path. Otherwise the
                                // segments must partition the packet stream.
                                if (!renderable_gpu_frame.segments.empty() &&
                                    segment_packets !=
                                        current_gpu_packets.size()) {
                                    ++gpu_segment_mismatches;
                                }
                                renderable_gpu_frame.vram.assign(
                                    decoded_vram.begin(), decoded_vram.end());
                                renderable_gpu_frame.display_x =
                                    gpu.displayStartX();
                                renderable_gpu_frame.display_y =
                                    gpu.displayStartY();
                                renderable_gpu_frame.display_width =
                                    gpu.displayWidth();
                                renderable_gpu_frame.display_height =
                                    gpu.displayHeight();
                                renderable_gpu_frame.sequence =
                                    ++retained_gpu_frame_sequence;
                                // A per-publication trace lets a scripted probe
                                // find the index of a gameplay frame (high
                                // `drawable`) to
                                // --publication-dump. Near-empty flips-stopped
                                // micro-frames have `drawable` close to zero.
                                if (options->publication_trace &&
                                    !options->run_live) {
                                    std::size_t drawable = 0U;
                                    for (const auto& p :
                                         renderable_gpu_frame.packets) {
                                        if (!p.empty()) {
                                            const auto op =
                                                static_cast<std::uint8_t>(
                                                    p.front() >> 24U);
                                            if (op >= 0x20U && op <= 0x7FU) {
                                                ++drawable;
                                            }
                                        }
                                    }
                                    std::cerr
                                        << "psycross_publication seq="
                                        << renderable_gpu_frame.sequence
                                        << " packets="
                                        << renderable_gpu_frame.packets.size()
                                        << " segments="
                                        << renderable_gpu_frame.segments.size()
                                        << " drawable=" << drawable
                                        << " display="
                                        << renderable_gpu_frame.display_width
                                        << "x"
                                        << renderable_gpu_frame.display_height
                                        << '\n';
                                }
#ifdef STUNTMASTER_HAS_PSYCROSS
                                if (headless_presenter) {
                                    // Present every publication in order, the
                                    // same as live play, so PsyX's texture cache
                                    // and GL state are warmed exactly as they are
                                    // on screen. Arm the render-target capture
                                    // before the target present so present()
                                    // reads it back before the swap -- the same
                                    // present() state the on-screen frame uses,
                                    // read back headlessly.
                                    if (renderable_gpu_frame.sequence ==
                                        options->publication_dump) {
                                        headless_presenter->captureNextPresent(
                                            "PUBLISHED_PSYX.BMP");
                                        writeCapturedGpuFrame(
                                            renderable_gpu_frame,
                                            renderable_gpu_frame.vram,
                                            "PUBLISHED_PSYX.GP0");
                                    }
                                    // A headless probe uses the same persistent
                                    // framebuffer path as live presentation.
                                    headless_presenter->presentPersistent(
                                        renderable_gpu_frame.vram,
                                        renderable_gpu_frame.packets,
                                        renderable_gpu_frame.display_x,
                                        renderable_gpu_frame.display_y,
                                        renderable_gpu_frame.display_width,
                                        renderable_gpu_frame.display_height,
                                        renderable_gpu_frame.segments);
                                    if (renderable_gpu_frame.sequence ==
                                        options->publication_dump) {
                                        std::cout
                                            << "psycross_publication_dump="
                                            << std::dec
                                            << renderable_gpu_frame.sequence
                                            << '\n';
                                    }
                                }
#endif
                                published_display_x = gpu.displayStartX();
                                published_display_y = gpu.displayStartY();
                                published_display_width = gpu.displayWidth();
                                published_display_height = gpu.displayHeight();
                                published_display_valid = true;
                                renderable_gpu_frame.guest_vblank =
                                    scheduled_vblanks;
                                renderable_gpu_frame.guest_instructions =
                                    result.instructions;
                                // `frameRate` is recomputed as 60/VBlank-delta
                                // by EndFrame__7Display; `__MyFrameRate` is
                                // the frame count MyVBL__Fe latches once per
                                // second. Both are retail's own view of its
                                // update cadence.
                                constexpr std::uint32_t retail_frame_rate_address =
                                    0x800DD808U;
                                constexpr std::uint32_t
                                    retail_measured_frame_rate_address =
                                        0x800DD65CU;
                                if (!runtime.read32(
                                        retail_frame_rate_address,
                                        renderable_gpu_frame.
                                            retail_frame_rate) ||
                                    !runtime.read16(
                                        retail_measured_frame_rate_address,
                                        renderable_gpu_frame.
                                            retail_measured_frame_rate)) {
                                    result.reason =
                                        stuntmaster::psx::R3000StopReason::
                                            memory_fault;
                                    result.pc =
                                        retail_frame_rate_address;
                                    break;
                                }
                                // Step__4Game advances Time::Step once per
                                // game update, so `theTimeMgr`'s tick counter
                                // measures gameplay time in game frames. Its
                                // rate against guest VBlanks is the direct
                                // wall-clock speed check for any update-rate
                                // change.
                                constexpr std::uint32_t the_time_mgr_address =
                                    0x800DD714U;
                                constexpr std::uint32_t time_tick_offset =
                                    0x1CU;
                                std::uint32_t time_manager = 0U;
                                if (runtime.read32(
                                        the_time_mgr_address, time_manager) &&
                                    time_manager != 0U) {
                                    (void)runtime.read32(
                                        time_manager + time_tick_offset,
                                        renderable_gpu_frame.
                                            retail_game_ticks);
                                }
                                if (high_frequency_requested &&
                                    !options->eager_sixty) {
                                    const auto pixels =
                                        gpu_decoder.imagePixelCount();
                                    if (pixels != previous_image_pixels) {
                                        previous_image_pixels = pixels;
                                        frames_since_upload = 0U;
                                    } else if (
                                        frames_since_upload <
                                        upload_quiet_frames) {
                                        ++frames_since_upload;
                                    }
                                    // Loading cadence is selected from the
                                    // guest's next state handler. The queue and
                                    // pre-play handlers stay at retail cadence;
                                    // the first frame that publishes PLAY
                                    // unlocks immediately. GPU uploads are not
                                    // evidence of a load: world-effect palettes,
                                    // RAM texture animations and pause UI all
                                    // upload during normal gameplay.
                                    const auto retail_state_handler =
                                        readRetailGameStateHandler();
                                    const auto want_high_frequency =
                                        retail_state_handler.has_value() &&
                                        stuntmaster::game::
                                            retailStateHandlerAllowsHighFrequency(
                                                *retail_state_handler);
                                    if (retail_state_handler !=
                                        last_retail_state_handler) {
                                        last_retail_state_handler =
                                            retail_state_handler;
                                        std::cerr
                                            << "retail_state_handler=";
                                        if (retail_state_handler.has_value()) {
                                            std::cerr << "0x" << std::hex
                                                      << *retail_state_handler
                                                      << std::dec;
                                        } else {
                                            std::cerr << "unavailable";
                                        }
                                        std::cerr << " cadence="
                                                  << (want_high_frequency
                                                          ? "high"
                                                          : "retail")
                                                  << '\n';
                                    }
                                    const auto cd_speed =
                                        want_high_frequency
                                        ? options->cd_read_speed
                                        : stuntmaster::game::
                                              cdReadSpeedForLoadPhase(
                                                  options->cd_read_speed,
                                                  frames_since_upload,
                                                  upload_quiet_frames);
                                    const auto want_cd_load_tail =
                                        cd_speed != options->cd_read_speed;
                                    if (want_cd_load_tail) {
                                        ++cd_load_tail_publications;
                                    }
                                    if (want_cd_load_tail !=
                                        cd_load_tail_active) {
                                        cd_load_tail_active =
                                            want_cd_load_tail;
                                        retail_hle.setCdReadSpeed(cd_speed);
                                        if (want_cd_load_tail) {
                                            ++cd_load_tail_entries;
                                        }
                                    }
                                    if (want_high_frequency != high_frequency_active &&
                                        setHighFrequencyMode(want_high_frequency)) {
                                        if (want_high_frequency) {
                                            ++swap_gate_unlocks;
                                        } else {
                                            ++swap_gate_relocks;
                                        }
                                    }
                                }
                                renderable_gpu_frame.high_frequency_active =
                                    high_frequency_active;
                                refreshOverlayRetiming();
                                reportLedgeTrace();
                                reportLedgeWatch();
                                renderable_gpu_frame.retime_hooks_armed =
                                    static_cast<std::uint32_t>(
                                        retime_boot_hooks.size() +
                                        (options->retime_clock
                                             ? stuntmaster::game::
                                                   retimeOverlayHooks().size()
                                             : 0U));
                                renderable_gpu_frame.retime_hooks_live =
                                    retime_hooks_live;
                                if (options->motion_trace) {
                                    motion_watch.refresh(runtime);
                                    motion_watch.sample(
                                        runtime,
                                        renderable_gpu_frame.guest_vblank,
                                        std::cerr);
                                    // The live host is normally ended by
                                    // closing its window, so report
                                    // periodically rather than only at exit.
                                    constexpr std::uint64_t
                                        motion_report_interval = 256U;
                                    if (renderable_gpu_frame.sequence %
                                            motion_report_interval ==
                                        0U) {
                                        printMotionWatch(
                                            motion_watch, std::cerr);
                                    }
                                }
                                if (options->frame_trace) {
                                    printRetainedGpuFrameTrace(
                                        renderable_gpu_frame);
                                }
                                constexpr std::size_t
                                    minimum_world_polygon_count = 40U;
                                if (options->timing_trace &&
                                    countGpuPolygons(
                                        renderable_gpu_frame.packets) >=
                                        minimum_world_polygon_count) {
                                    std::cerr
                                        << "world_frame_sequence=" << std::dec
                                        << renderable_gpu_frame.sequence
                                        << " guest_vblank="
                                        << renderable_gpu_frame.guest_vblank
                                        << " vblank_delta="
                                        << (previous_world_frame_vblank
                                                ? renderable_gpu_frame.
                                                          guest_vblank -
                                                      *previous_world_frame_vblank
                                                : 0U)
                                        << " instruction_delta="
                                        << (previous_world_frame_instructions
                                                ? renderable_gpu_frame.
                                                          guest_instructions -
                                                      *previous_world_frame_instructions
                                                : 0U)
                                        << " retail_frame_rate="
                                        << renderable_gpu_frame.
                                               retail_frame_rate
                                        << " retail_measured_frame_rate="
                                        << renderable_gpu_frame.
                                               retail_measured_frame_rate
                                        << " retail_game_ticks="
                                        << renderable_gpu_frame.
                                               retail_game_ticks
                                        << " vram_fill="
                                        << sampledVramOccupancy(
                                               renderable_gpu_frame.vram)
                                        << " cb_in_transfer="
                                        << callbacks_during_image_transfer
                                        << " uncovered_textured="
                                        << countUncoveredTexturedPolygons(
                                               renderable_gpu_frame.packets,
                                               gpu_decoder,
                                               uncovered_texture_pages)
                                        << " image_pixels="
                                        << gpu_decoder.imagePixelCount()
                                        << " vram_copies="
                                        << gpu_decoder.vramCopyCount()
                                        << " max_upload="
                                        << gpu_decoder.largestImageUpload()
                                               .width
                                        << 'x'
                                        << gpu_decoder.largestImageUpload()
                                               .height
                                        << " polygons="
                                        << countGpuPolygons(
                                               renderable_gpu_frame.packets)
                                        << std::endl;
                                    previous_world_frame_vblank =
                                        renderable_gpu_frame.guest_vblank;
                                    previous_world_frame_instructions =
                                        renderable_gpu_frame.
                                            guest_instructions;
                                }
                                if (options->run_live) {
                                    frame_mailbox.publish(
                                        std::move(renderable_gpu_frame));
                                    renderable_gpu_frame = {};
                                }
                            }
                            // Only a display flip ends the interval. A VBlank
                            // that merely published keeps the accumulated
                            // packets, so the next publication still carries the
                            // whole frame rather than the remainder of it.
                            if (interval_complete) {
                                completed_gpu_packets =
                                    std::move(current_gpu_packets);
                                current_gpu_packets.clear();
                                current_gpu_segments.clear();
                                pending_gpu_segment_packets.clear();
                                for (const auto& environment :
                                     persistent_gpu_environment) {
                                    if (!environment.empty()) {
                                        current_gpu_packets.push_back(
                                            environment);
                                        pending_gpu_segment_packets.push_back(
                                            environment);
                                    }
                                }
                            }
                        }
                    }
                    if (suspended_state && runtime.atReturnSentinel()) {
                        runtime.settleLoadDelay();
                        runtime.restoreCpuState(*suspended_state);
                        suspended_state.reset();
                        continue;
                    }
                    if (!suspended_state) {
                        const auto callback = bios.takePendingEventCallback();
                        if (callback) {
                            suspended_state = runtime.state();
                            if (!runtime.beginInterruptCall(*callback)) {
                                result.reason =
                                    stuntmaster::psx::R3000StopReason::memory_fault;
                                result.pc = *callback;
                                break;
                            }
                            continue;
                        }
                    }
                    if (!suspended_state && !pending_vsync_callbacks.empty()) {
                        const auto callback = pending_vsync_callbacks.front();
                        pending_vsync_callbacks.pop_front();
                        suspended_state = runtime.state();
                        if (!runtime.beginInterruptCall(callback)) {
                            result.reason =
                                stuntmaster::psx::R3000StopReason::memory_fault;
                            result.pc = callback;
                            break;
                        }
                        ++scheduled_vsync_callbacks;
                        if (gpu_decoder.imageTransferInProgress()) {
                            ++callbacks_during_image_transfer;
                        }
                        continue;
                    }
                    constexpr std::uint32_t gpu_callback_address = 0x800D51A0U;
                    // One drawing completion per ordering table. Image uploads
                    // ride DMA2 as well and outnumber ordering tables heavily,
                    // so the undivided transfer count is the wrong unit; only
                    // linked-list transfers are drawing.
                    if (gpu.dma2LinkedListTransfers() > observed_draw_transfers) {
                        std::uint32_t callback{};
                        if (!runtime.read32(gpu_callback_address, callback)) {
                            result.reason =
                                stuntmaster::psx::R3000StopReason::memory_fault;
                            result.pc = gpu_callback_address;
                            break;
                        }
                        const auto raised =
                            gpu.dma2LinkedListTransfers() -
                            observed_draw_transfers;
                        observed_draw_transfers =
                            gpu.dma2LinkedListTransfers();
                        // A completion raised with no callback installed is
                        // lost on hardware too, so it is not queued.
                        if (callback != 0U) {
                            outstanding_gpu_completions += raised;
                            outstanding_gpu_callback = callback;
                        }
                    }
                    // A zero-latency interrupt can race QueueLayer before it has
                    // published the next entry, so completions are delivered at
                    // an explicit DrawSync boundary rather than the instant the
                    // transfer finishes.
                    constexpr std::uint32_t draw_sync_boundary = 0x80026C04U;
                    const auto at_gpu_safe_point =
                        runtime.state().pc == draw_sync_boundary;
                    if (at_gpu_safe_point) {
                        ++gpu_draw_sync_boundaries;
                        gpu_completions_waiting_at_boundary +=
                            outstanding_gpu_completions != 0U ? 1U : 0U;
                    }
                    if (!suspended_state && outstanding_gpu_completions != 0U &&
                        at_gpu_safe_point) {
                        // DSCallback removes itself when the queue drains, and
                        // a removed callback cannot be invoked. Anything still
                        // queued at that point is owed to nobody.
                        std::uint32_t installed{};
                        if (!runtime.read32(gpu_callback_address, installed)) {
                            result.reason =
                                stuntmaster::psx::R3000StopReason::memory_fault;
                            result.pc = gpu_callback_address;
                            break;
                        }
                        if (installed == 0U) {
                            discarded_gpu_completions +=
                                outstanding_gpu_completions;
                            outstanding_gpu_completions = 0U;
                        } else {
                            outstanding_gpu_callback = installed;
                        }
                    }
                    if (!suspended_state && outstanding_gpu_completions != 0U &&
                        at_gpu_safe_point) {
                        constexpr std::uint32_t render_queue_address =
                            0x800DD7A0U;
                        std::uint32_t render_queue{};
                        std::uint32_t current_layer{};
                        if (!runtime.read32(render_queue_address, render_queue) ||
                            !runtime.read32(
                                render_queue + 0x1CU, current_layer)) {
                            result.reason =
                                stuntmaster::psx::R3000StopReason::memory_fault;
                            result.pc = render_queue_address;
                            break;
                        }
                        bool callback_ready = true;
                        if (current_layer == 0U ||
                            current_layer == 0xFFFFFFFFU) {
                            // Deferred delivery has outlived RenderQueue::current.
                            // WaitForLayer keeps the exact layer index in s2, so
                            // reconstruct the old-layer pointer that the retail
                            // callback needs for its final tLayer::Free call.
                            constexpr std::uint32_t view_address = 0x800DD780U;
                            std::uint32_t view{};
                            std::uint32_t layers{};
                            std::uint32_t layer_count{};
                            std::uint32_t waiting_layer{};
                            const auto waiting_layer_index =
                                runtime.state().gpr[18];
                            if (!runtime.read32(
                                    render_queue, layer_count) ||
                                !runtime.read32(view_address, view) ||
                                !runtime.read32(view + 0x10U, layers) ||
                                (waiting_layer_index < layer_count &&
                                 !runtime.read32(
                                     layers + waiting_layer_index *
                                         sizeof(std::uint32_t),
                                     waiting_layer))) {
                                result.reason =
                                    stuntmaster::psx::R3000StopReason::memory_fault;
                                result.pc = view_address;
                                break;
                            }
                            // s2 is only a layer index while the guest is in
                            // WaitForLayer. At other DrawSync call sites it can
                            // contain an unrelated pointer; defer instead of
                            // allowing wrapped guest-address arithmetic.
                            callback_ready =
                                waiting_layer_index < layer_count &&
                                waiting_layer != 0U &&
                                runtime.write32(
                                    render_queue + 0x1CU, waiting_layer);
                        }
                        if (callback_ready) {
                            const auto callback = outstanding_gpu_callback;
                            // Retire exactly the one being delivered. Retiring
                            // the whole backlog is what lost half of retail's
                            // layer completions.
                            --outstanding_gpu_completions;
                            suspended_state = runtime.state();
                            if (!runtime.beginInterruptCall(callback)) {
                                result.reason =
                                    stuntmaster::psx::R3000StopReason::memory_fault;
                                result.pc = callback;
                                break;
                            }
                            ++scheduled_gpu_callbacks;
                            if (gpu_decoder.imageTransferInProgress()) {
                                ++callbacks_during_image_transfer;
                            }
                            continue;
                        }
                    }
                    if (!suspended_state && gpu.takeDma4Completion()) {
                        // Retail libspu stores the callback installed by
                        // SpuSetTransferCallback in this revision-owned global.
                        constexpr std::uint32_t spu_callback_address =
                            0x800DBF18U;
                        std::uint32_t callback{};
                        if (!runtime.read32(spu_callback_address, callback)) {
                            result.reason =
                                stuntmaster::psx::R3000StopReason::memory_fault;
                            result.pc = spu_callback_address;
                            break;
                        }
                        if (callback == 0U) {
                            constexpr std::uint32_t spu_event_class = 0xF0000009U;
                            constexpr std::uint32_t dma_complete_spec = 0x20U;
                            callback = bios.eventCallback(
                                spu_event_class, dma_complete_spec).value_or(0U);
                        }
                        if (callback != 0U) {
                            suspended_state = runtime.state();
                            if (!runtime.beginInterruptCall(callback)) {
                                result.reason =
                                    stuntmaster::psx::R3000StopReason::memory_fault;
                                result.pc = callback;
                                break;
                            }
                            ++scheduled_spu_callbacks;
                            continue;
                        }
                    }
                    if (!suspended_state && spu.takeIrq()) {
                        // Retail installs its streaming refill handler here
                        // through SpuSetIRQCallback at 0x800C79EC, which stores
                        // the argument at this global.
                        constexpr std::uint32_t spu_irq_callback_address =
                            0x800DBF1CU;
                        std::uint32_t callback{};
                        if (!runtime.read32(
                                spu_irq_callback_address, callback)) {
                            result.reason =
                                stuntmaster::psx::R3000StopReason::memory_fault;
                            result.pc = spu_irq_callback_address;
                            break;
                        }
                        if (callback != 0U) {
                            suspended_state = runtime.state();
                            if (!runtime.beginInterruptCall(callback)) {
                                result.reason =
                                    stuntmaster::psx::R3000StopReason::memory_fault;
                                result.pc = callback;
                                break;
                            }
                            ++scheduled_spu_irq_callbacks;
                            continue;
                        }
                    }
                    if (!suspended_state && result.instructions >= next_vblank) {
                        if (options->run_live && serviceQuickSaveRequests()) {
                            continue;
                        }
                        next_vblank += guest_instructions_per_vblank;
                        if (options->run_live) {
                            retail_hle.setPadOneState(
                                true,
                                latest_pad_one_buttons.load(
                                    std::memory_order_acquire));
                        } else if (!options->scripted_input.empty()) {
                            // Presses combine, so overlapping entries in the
                            // script behave like holding both.
                            std::uint16_t buttons = 0xFFFFU;
                            for (const auto& press : options->scripted_input) {
                                if (scheduled_vblanks >= press.vblank &&
                                    scheduled_vblanks <
                                        press.vblank +
                                            scripted_input_hold_vblanks) {
                                    buttons &= press.active_low_buttons;
                                }
                            }
                            retail_hle.setPadOneState(true, buttons);
                        }
                        // Mixing here keeps audio on the guest's clock rather
                        // than a host one. 44100 Hz against the console's 60
                        // VBlanks is exactly 735 stereo frames; a faster
                        // emulated display does not divide it evenly, so the
                        // pacer carries the remainder and the output rate stays
                        // 44.1 kHz at every schedule.
                        if (options->have_audio_capture || options->run_live) {
                            constexpr std::size_t maximum_frames_per_vblank =
                                stuntmaster::game::guest_audio_rate /
                                stuntmaster::game::console_vblank_rate;
                            const auto frames = static_cast<std::size_t>(
                                audio_frame_pacer.take());
                            std::array<
                                std::int16_t,
                                maximum_frames_per_vblank * 2U>
                                storage{};
                            const auto block =
                                std::span{storage}.first(frames * 2U);
                            spu.mix(block);
                            if (options->run_live) {
                                audio_ring.push(block);
                            }
                            if (options->have_audio_capture) {
                                captured_audio.insert(
                                    captured_audio.end(),
                                    block.begin(),
                                    block.end());
                            }
                        }
                        // Scanout: the beam has returned, so whatever the
                        // display rectangle holds is a frame.
                        pending_vblank_boundary = true;
                        if (vblanks_since_display_flip <
                            std::numeric_limits<std::uint32_t>::max()) {
                            ++vblanks_since_display_flip;
                        }
                        if (!retail_hle.onVBlank(runtime)) {
                            result.reason =
                                stuntmaster::psx::R3000StopReason::memory_fault;
                            result.pc = runtime.state().pc;
                            break;
                        }
                        ++scheduled_vblanks;
                        if (options->run_live) {
                            next_host_vblank += host_vblank_step;
                            auto now = HostClock::now();
                            if (now - next_host_vblank >
                                std::chrono::seconds{1}) {
                                // A debugger pause must not create guest timing
                                // debt. Resume from one fixed tick at the current
                                // wall-clock position.
                                next_host_vblank = now;
                            }
                            if (next_host_vblank > now) {
                                std::this_thread::sleep_until(next_host_vblank);
                            }
                        }
                        for (const auto callback : retail_hle.vsyncCallbacks()) {
                            if (callback != 0U) {
                                pending_vsync_callbacks.push_back(callback);
                            }
                        }
                        constexpr std::uint32_t timer3_event_class = 0xF2000003U;
                        constexpr std::uint32_t interrupted_event_spec = 2U;
                        const auto callback = bios.eventCallback(
                            timer3_event_class, interrupted_event_spec);
                        if (callback) {
                            suspended_state = runtime.state();
                            if (!runtime.beginInterruptCall(*callback)) {
                                result.reason =
                                    stuntmaster::psx::R3000StopReason::memory_fault;
                                result.pc = *callback;
                                break;
                            }
                            continue;
                        }
                    }
                    // `WaitForLayer` spends the idle part of a frame polling
                    // the same layer until VBlank's swap callback changes it.
                    // From the second poll onward, one failed poll is exactly
                    // 42 guest instructions and returns to this same FDF0
                    // state; only its private iteration counter and $v1 advance.
                    // Charge whole invariant polls without interpreting them,
                    // but always leave at least one instruction for the normal
                    // path. That preserves the exact PC at a VBlank/probe
                    // deadline and therefore the existing callback ordering.
                    constexpr std::uint32_t wait_for_layer_poll_pc =
                        0x8009FDF0U;
                    constexpr std::uint64_t wait_for_layer_poll_instructions =
                        42U;
                    const auto can_fast_forward_idle =
                        !suspended_state && !options->frame_trace &&
                        !options->motion_trace && !options->have_watch_writes &&
                        outstanding_gpu_completions == 0U &&
                        runtime.state().pc == wait_for_layer_poll_pc &&
                        runtime.state().gpr[31] == wait_for_layer_poll_pc &&
                        runtime.state().gpr[2] == 0U &&
                        runtime.state().gpr[4] == 1U;
                    if (can_fast_forward_idle) {
                        auto available = next_vblank - result.instructions;
                        if (!options->run_live) {
                            available = std::min(
                                available,
                                options->guest_budget -
                                    (result.instructions -
                                     probe_instruction_origin));
                        }
                        const auto maximum_polls = available > 0U
                            ? (available - 1U) /
                                wait_for_layer_poll_instructions
                            : 0U;
                        const auto polls = stuntmaster::game::RetailHle::
                            fastForwardWaitForLayerPolls(
                                runtime, maximum_polls);
                        if (polls != 0U) {
                            const auto charged =
                                polls * wait_for_layer_poll_instructions;
                            result.instructions += charged;
                            guest_idle_instructions += charged;
                            retail_wait_for_layer_calls += polls;
                            gpu_draw_sync_boundaries += polls;
                            continue;
                        }
                    }
                    const auto retail_result = retail_hle.dispatch(runtime);
                    if (retail_result.status ==
                        stuntmaster::game::RetailHleStatus::handled) {
                        continue;
                    }
                    if (retail_result.status ==
                        stuntmaster::game::RetailHleStatus::memory_fault) {
                        result.reason =
                            stuntmaster::psx::R3000StopReason::memory_fault;
                        result.pc = retail_result.address;
                        break;
                    }
                    const auto bios_result = bios.dispatch(runtime);
                    if (bios_result.status == stuntmaster::psx::BiosHleStatus::handled) {
                        continue;
                    }
                    if (bios_result.status !=
                        stuntmaster::psx::BiosHleStatus::not_boundary) {
                        bios_boundary = true;
                        result.pc = bios_result.vector;
                        break;
                    }
                    constexpr std::uint32_t queue_layer_entry = 0x8009FEE4U;
                    constexpr std::uint32_t queue_swap_entry = 0x8009FF7CU;
                    constexpr std::uint32_t wait_for_layer_entry = 0x8009FDECU;
                    const auto guest_pc = runtime.state().pc;
                    retail_queue_layer_calls +=
                        guest_pc == queue_layer_entry ? 1U : 0U;
                    retail_queue_swap_calls +=
                        guest_pc == queue_swap_entry ? 1U : 0U;
                    retail_wait_for_layer_calls +=
                        guest_pc == wait_for_layer_entry ? 1U : 0U;
                    // Stay inside the CPU core until a boundary that actually
                    // needs the machine scheduler. VBlank remains exact because
                    // it is the batch budget; HLE/BIOS/diagnostic PCs are in the
                    // bitmap above, and runBatch also yields after claimed MMIO.
                    constexpr std::uint64_t maximum_batch = 65'536U;
                    auto batch_budget = maximum_batch;
                    if (!suspended_state) {
                        batch_budget = std::min(
                            batch_budget,
                            next_vblank - result.instructions);
                    }
                    if (!options->run_live) {
                        batch_budget = std::min(
                            batch_budget,
                            options->guest_budget -
                                (result.instructions -
                                 probe_instruction_origin));
                    }
                    const auto batch = runtime.runBatch(
                        batch_budget,
                        execution_boundaries,
                        /*execute_initial_boundary=*/true);
                    ++guest_batch_calls;
                    guest_batched_instructions += batch.instructions;
                    result.instructions += batch.instructions;
                    result.reason = batch.reason;
                    result.pc = batch.pc;
                    result.instruction = batch.instruction;
                    if (batch.reason ==
                        stuntmaster::psx::R3000StopReason::syscall) {
                        const auto syscall_result = bios.dispatchSyscall(runtime);
                        if (syscall_result.status ==
                            stuntmaster::psx::BiosHleStatus::handled) {
                            result.reason = stuntmaster::psx::R3000StopReason::running;
                            continue;
                        }
                    }
                    if (batch.reason !=
                        stuntmaster::psx::R3000StopReason::running) {
                        break;
                    }
                }
            };
            const auto run_guest_guarded = [&] {
                try {
                    run_guest_loop();
                } catch (...) {
                    guest_exception = std::current_exception();
                }
                guest_finished.store(true, std::memory_order_release);
            };
#ifdef STUNTMASTER_HAS_PSYCROSS
            if (live_presenter) {
                std::optional<RetainedGpuFrame> current_gpu_frame;
                bool presented_exact_current = false;
                bool suppress_movie_start = false;
                bool suppress_guest_frames_until_movie = false;
                auto next_host_input = host_epoch;
                // A rolling comparison of guest VBlanks produced against wall
                // clock. The guest is interpreted, so a schedule that asks for
                // more instructions a second than the host can deliver simply
                // runs slow, and audio and gameplay slow together because both
                // follow this boundary. Measuring it turns "everything is at
                // half speed" into a number instead of a guess.
                std::uint64_t speed_sample_vblank = 0U;
                auto speed_sample_time = host_epoch;
                std::uint32_t guest_speed_percent = 0U;
                bool speed_sample_high_frequency = false;
                std::uint64_t observed_quick_load_generation = 0U;
                const auto updateDebugOverlay =
                    [&](const RetainedGpuFrame* frame,
                        bool frame_high_frequency_active) {
                        const auto requested_update_rate =
                            published_requested_update_rate.load(
                                std::memory_order_acquire);
                        const auto runtime_high_frequency_requested =
                            requested_update_rate >
                            retail_schedule.update_rate;
                        const auto stock_retime_control =
                            options->guest_update_rate ==
                                retail_schedule.update_rate &&
                            (options->retime_motion || options->retime_clock);
                        const auto compensated_mode =
                            frame_high_frequency_active ||
                            (!runtime_high_frequency_requested &&
                             stock_retime_control);
                        // The schedule that produced this frame, which is
                        // retail's while a load holds the guest there.
                        const auto schedule = frame_high_frequency_active
                            ? stuntmaster::game::guestScheduleFor(
                                  requested_update_rate)
                            : retail_schedule;
                        if (frame != nullptr) {
                            const auto now = HostClock::now();
                            const auto elapsed = now - speed_sample_time;
                            // A mode switch changes the VBlank rate, so restart
                            // rather than average across two schedules.
                            if (frame_high_frequency_active !=
                                speed_sample_high_frequency) {
                                speed_sample_high_frequency =
                                    frame_high_frequency_active;
                                speed_sample_vblank = frame->guest_vblank;
                                speed_sample_time = now;
                                guest_speed_percent = 0U;
                            } else if (
                                elapsed >= std::chrono::milliseconds{500} &&
                                frame->guest_vblank > speed_sample_vblank) {
                                const auto seconds =
                                    std::chrono::duration<double>{elapsed}
                                        .count();
                                const auto produced = static_cast<double>(
                                    frame->guest_vblank - speed_sample_vblank);
                                const auto expected = seconds *
                                    static_cast<double>(schedule.vblank_rate);
                                guest_speed_percent =
                                    static_cast<std::uint32_t>(
                                        produced / expected * 100.0 + 0.5);
                                speed_sample_vblank = frame->guest_vblank;
                                speed_sample_time = now;
                            }
                        }
                        stuntmaster::presentation::DebugOverlayState overlay;
                        overlay.enabled = true;
                        overlay.initially_visible = options->debug_overlay;
                        overlay.guest_update_rate = requested_update_rate;
                        overlay.guest_vblank_rate = schedule.vblank_rate;
                        overlay.retime_divisor = schedule.retime_divisor;
                        overlay.guest_speed_percent = guest_speed_percent;
                        overlay.guest_ticks =
                            frame != nullptr ? frame->retail_game_ticks : 0U;
                        overlay.guest_vblanks =
                            frame != nullptr ? frame->guest_vblank : 0U;
                        overlay.guest_frames =
                            frame != nullptr ? frame->sequence : 0U;
                        overlay.presentation_rate =
                            presentation_rate;
                        overlay.high_frequency_requested =
                            runtime_high_frequency_requested;
                        overlay.high_frequency_active =
                            frame_high_frequency_active;
                        overlay.retime_motion_active =
                            options->retime_motion && compensated_mode;
                        overlay.retime_clock_active =
                            options->retime_clock && compensated_mode;
                        overlay.widescreen_cull_active =
                            published_widescreen_cull_active.load(
                                std::memory_order_acquire);
                        overlay.retime_hooks_armed =
                            frame != nullptr ? frame->retime_hooks_armed : 0U;
                        overlay.retime_hooks_live =
                            frame != nullptr ? frame->retime_hooks_live : 0U;
                        live_presenter->setDebugOverlay(overlay);
                    };
                updateDebugOverlay(nullptr, high_frequency_active);
                std::jthread guest_worker{run_guest_guarded};
                while (!guest_finished.load(std::memory_order_acquire)) {
                    auto now = HostClock::now();
                    const auto requested_render_size =
                        host_menu_render_size_requested.exchange(
                            0U, std::memory_order_acq_rel);
                    if (requested_render_size != 0U) {
                        const auto width = static_cast<std::uint32_t>(
                            requested_render_size >> 32U);
                        const auto height = static_cast<std::uint32_t>(
                            requested_render_size);
                        live_presenter->setRenderSize(width, height);
                        // Force the current immutable guest publication through
                        // a full present into the newly allocated target.
                        presented_gpu_frame_sequence.reset();
                        presented_exact_current = false;
                        live_presenter->showNotification(
                            "RENDER " + std::to_string(width) + "x" +
                            std::to_string(height));
                    }
                    const auto requested_launcher_settings =
                        runtime_launcher_settings_requested.exchange(
                            0U, std::memory_order_acq_rel);
                    if (requested_launcher_settings != 0U) {
                        const auto height = static_cast<std::uint32_t>(
                            requested_launcher_settings >> 32U);
                        std::error_code path_error;
                        auto game_path = std::filesystem::absolute(
                            options->game, path_error);
                        if (path_error) {
                            game_path = options->game;
                        }
                        std::string settings_error;
                        if (!saveRuntimeLauncherSettings(
                                options->launcher_settings_path,
                                game_path.parent_path(),
                                height,
                                (requested_launcher_settings & 1U) != 0U,
                                (requested_launcher_settings & 2U) != 0U,
                                settings_error)) {
                            std::cerr << "launcher_settings=save_failed: "
                                      << settings_error << '\n';
                        } else {
                            std::cout << "launcher_settings=saved\n";
                        }
                    }
                    switch (quick_save_notification.exchange(
                        QuickSaveNotification::none,
                        std::memory_order_acq_rel)) {
                    case QuickSaveNotification::save_created:
                        live_presenter->showNotification(
                            "QUICK SAVE CREATED");
                        break;
                    case QuickSaveNotification::timestamped_save_created:
                        live_presenter->showNotification(
                            "TIMESTAMPED SAVE CREATED");
                        break;
                    case QuickSaveNotification::load_restored:
                        live_presenter->showNotification(
                            "QUICK SAVE LOADED");
                        break;
                    case QuickSaveNotification::save_failed:
                        live_presenter->showNotification(
                            "QUICK SAVE FAILED");
                        break;
                    case QuickSaveNotification::load_failed:
                        live_presenter->showNotification(
                            "QUICK LOAD FAILED");
                        break;
                    case QuickSaveNotification::thirty_hz_mode:
                        live_presenter->showNotification("30 HZ MODE");
                        break;
                    case QuickSaveNotification::sixty_hz_mode:
                        live_presenter->showNotification(
                            "60 HZ RETIMING ON");
                        break;
                    case QuickSaveNotification::retime_toggle_unavailable:
                        live_presenter->showNotification(
                            "60 HZ NEEDS RETIME FLAGS");
                        break;
                    case QuickSaveNotification::widescreen_mode:
                        live_presenter->showNotification("WIDESCREEN CULL ON");
                        break;
                    case QuickSaveNotification::narrow_cull_mode:
                        live_presenter->showNotification("WIDESCREEN CULL OFF");
                        break;
                    case QuickSaveNotification::widescreen_toggle_unavailable:
                        live_presenter->showNotification(
                            "F8 NEEDS WIDESCREEN PSYCROSS");
                        break;
                    case QuickSaveNotification::none:
                        break;
                    }
                    const auto loaded_generation =
                        quick_load_generation.load(std::memory_order_acquire);
                    if (loaded_generation != observed_quick_load_generation) {
                        observed_quick_load_generation = loaded_generation;
                        (void)frame_mailbox.discardAll();
                        (void)audio_ring.discardAll();
                        if (audio_output && audio_output->ready()) {
                            audio_output->stopAndDiscardQueued();
                        }
                        current_gpu_frame.reset();
                        presented_gpu_frame_sequence.reset();
                        presented_exact_current = false;
                        now = HostClock::now();
                        next_host_input = now;
                        next_host_presentation = now;
                    }
                    if (movie_requests.takeTransition()) {
                        // FadeUpdate has reached its clamp and returned false,
                        // but retail does not render that final 255 shade. Hold
                        // true black before the title route frees/reloads its
                        // front-end screens, otherwise the fully filled loading
                        // page can appear between the last fade shade and the
                        // later Game::PlayMovie notification.
                        live_presenter->presentBlackFrame();
                        const auto discarded = frame_mailbox.discardAll();
                        current_gpu_frame.reset();
                        presented_gpu_frame_sequence.reset();
                        presented_exact_current = false;
                        suppress_guest_frames_until_movie = true;
                        if (options->frame_trace) {
                            std::cerr
                                << "movie_fade_black queued_frames_discarded="
                                << discarded << '\n';
                        }
                        movie_requests.completeTransition();
                    }
                    if (auto preparing_movie =
                            movie_requests.takePreparation()) {
                        // Game::PlayMovie has just been entered, immediately
                        // after the guest-authored fade reaches black. Retail's
                        // setup writes a fully filled loading screen before
                        // reaching MoviePlayer::Play. That is useful during a
                        // level load but is transitional here: keep the black
                        // fade visible and consume every guest publication
                        // until the native movie boundary arrives.
                        live_presenter->presentBlackFrame();
                        const auto discarded = frame_mailbox.discardAll();
                        current_gpu_frame.reset();
                        presented_gpu_frame_sequence.reset();
                        presented_exact_current = false;
                        suppress_guest_frames_until_movie = true;
                        if (options->frame_trace) {
                            std::cerr
                                << "movie_prepare_black="
                                << preparing_movie->path
                                << " queued_frames_discarded=" << discarded
                                << '\n';
                        }
                        // Only now may retail perform its movie-specific setup;
                        // the black hold and frame suppression are already
                        // installed, so none of those writes can race onto the
                        // host display.
                        movie_requests.completePreparation();
                    }
                    if (auto requested_movie = movie_requests.take()) {
                        const auto disc_path =
                            movieDiscPath(requested_movie->path);
                        // The frame in the window and any queued mailbox
                        // values predate the caller-gated movie hand-off. Hide
                        // that transitional splash while the first frame is
                        // decoded. The guest is blocked in requestAndWait, so
                        // nothing newer can be published until complete().
                        live_presenter->presentBlackFrame();

                        // Retail lets the Start/menu SFX finish against black
                        // before movie XA audio takes over. Front-end sounds
                        // may be dry, so reverb routing is not a reliable SFX
                        // classifier. Voices 9 and 10 are the verified stereo
                        // music stream; drain every other active foreground
                        // voice plus the already-queued output. The bound
                        // still protects against malformed looping samples.
                        constexpr std::uint32_t streaming_music_voices =
                            (1U << 9U) | (1U << 10U);
                        const auto pre_movie_sfx_voices =
                            spu.activeVoiceMask() & ~streaming_music_voices;
                        constexpr std::size_t audio_frames_per_vblank = 735U;
                        std::array<std::int16_t,
                                   audio_frames_per_vblank * 2U>
                            guest_audio_block{};
                        const auto pre_movie_audio_started = HostClock::now();
                        const auto pre_movie_audio_deadline =
                            pre_movie_audio_started + std::chrono::seconds{3};
                        std::uint64_t pre_movie_audio_frames = 0U;
                        bool pre_movie_audio_timed_out = false;
                        for (;;) {
                            const auto device_ready =
                                audio_output && audio_output->ready();
                            if (device_ready &&
                                audio_output->canAcceptBuffer()) {
                                const auto taken =
                                    audio_ring.pop(guest_audio_block);
                                if (taken != 0U) {
                                    audio_output->submit(
                                        std::span{guest_audio_block}.first(
                                            taken));
                                }
                            } else if (!device_ready) {
                                (void)audio_ring.discardAll();
                            }

                            const auto elapsed =
                                HostClock::now() - pre_movie_audio_started;
                            using GuestAudioFrame = std::chrono::duration<
                                std::uint64_t,
                                std::ratio<
                                    1,
                                    stuntmaster::presentation::AudioOutput::
                                        sample_rate>>;
                            const auto target_frames =
                                std::chrono::duration_cast<GuestAudioFrame>(
                                    elapsed).count();
                            const auto sfx_active =
                                (spu.activeVoiceMask() &
                                 pre_movie_sfx_voices) != 0U;
                            if (sfx_active &&
                                target_frames >=
                                    pre_movie_audio_frames +
                                        audio_frames_per_vblank &&
                                (!device_ready ||
                                 audio_output->canAcceptBuffer())) {
                                spu.mix(guest_audio_block);
                                pre_movie_audio_frames +=
                                    audio_frames_per_vblank;
                                if (device_ready) {
                                    audio_output->submit(guest_audio_block);
                                }
                            }

                            const auto ring_empty =
                                audio_ring.available() == 0U;
                            const auto device_empty = !device_ready ||
                                audio_output->queuedBufferCount() == 0U;
                            const auto finished =
                                (spu.activeVoiceMask() &
                                 pre_movie_sfx_voices) == 0U;
                            if (finished && ring_empty && device_empty) {
                                break;
                            }
                            if (HostClock::now() >=
                                pre_movie_audio_deadline) {
                                pre_movie_audio_timed_out = true;
                                break;
                            }
                            (void)live_presenter->pollPadOneButtons();
                            std::this_thread::sleep_for(
                                std::chrono::milliseconds{1});
                        }

                        const auto discarded_guest_audio_samples =
                            audio_ring.discardAll();
                        if (audio_output && audio_output->ready()) {
                            audio_output->stopAndDiscardQueued();
                        }
                        std::uint64_t advanced_guest_audio_frames = 0U;
                        const auto advanceGuestAudio =
                            [&spu, &advanced_guest_audio_frames](
                                HostClock::duration elapsed) {
                                using GuestAudioFrame =
                                    std::chrono::duration<
                                        std::uint64_t,
                                        std::ratio<
                                            1,
                                            stuntmaster::presentation::
                                                AudioOutput::sample_rate>>;
                                const auto target =
                                    std::chrono::duration_cast<GuestAudioFrame>(
                                        elapsed).count();
                                if (target > advanced_guest_audio_frames) {
                                    spu.advance(
                                        target - advanced_guest_audio_frames);
                                    advanced_guest_audio_frames = target;
                                }
                            };
                        try {
                            auto raw_movie = image.readRawSectorFile(disc_path);
                            if (raw_movie.sector_size != 2352U) {
                                throw stuntmaster::core::Error{
                                    "STR movie is not stored as raw 2352-byte "
                                    "sectors: " + disc_path};
                            }
                            const auto playback =
                                stuntmaster::presentation::playMovie(
                                    *live_presenter,
                                    disc_path,
                                    std::move(raw_movie.bytes),
                                    audio_output && audio_output->ready(),
                                    advanceGuestAudio);
                            suppress_movie_start =
                                playback == stuntmaster::presentation::
                                    MoviePlaybackResult::skipped;
                        } catch (const std::exception& error) {
                            // Native playback is an enhancement at a caller
                            // that historically skipped. A corrupt/unsupported
                            // stream must not strand the guest worker.
                            std::cerr << "movie: " << disc_path << ": "
                                      << error.what()
                                      << "; continuing without playback\n";
                        }
                        const auto discarded_movie_transition_frames =
                            frame_mailbox.discardAll();
                        if (options->frame_trace &&
                            discarded_movie_transition_frames != 0U) {
                            std::cerr
                                << "movie_transition_frames_discarded="
                                << discarded_movie_transition_frames << '\n';
                        }
                        if (options->frame_trace) {
                            std::cerr
                                << "movie_pre_audio_voice_mask=0x"
                                << std::hex << pre_movie_sfx_voices << std::dec
                                << " frames=" << pre_movie_audio_frames
                                << " timed_out="
                                << pre_movie_audio_timed_out << '\n'
                                << "movie_guest_audio_samples_discarded="
                                << discarded_guest_audio_samples << '\n'
                                << "movie_guest_audio_frames_advanced="
                                << advanced_guest_audio_frames << '\n';
                        }
                        current_gpu_frame.reset();
                        presented_gpu_frame_sequence.reset();
                        presented_exact_current = false;
                        suppress_guest_frames_until_movie =
                            requested_movie->followed_by_movie;
                        now = HostClock::now();
                        next_host_input = now;
                        next_host_presentation = now;
                        // Release the guest only after all pre-movie
                        // presentation state is gone. A verified chained
                        // caller keeps its teardown frames suppressed below.
                        movie_requests.complete();
                    }
                    if (now >= next_host_input) {
                        const auto physical_buttons =
                            live_presenter->pollPadOneButtons();
                        if (live_presenter->takeQuickSaveRequest()) {
                            quick_save_requests.fetch_or(
                                quick_save_requested,
                                std::memory_order_release);
                        }
                        if (live_presenter->takeQuickLoadRequest()) {
                            quick_save_requests.fetch_or(
                                quick_load_requested,
                                std::memory_order_release);
                        }
                        if (live_presenter->takeTimestampedQuickSaveRequest()) {
                            quick_save_requests.fetch_or(
                                timestamped_quick_save_requested,
                                std::memory_order_release);
                        }
                        if (live_presenter->takeRetimeToggleRequest()) {
                            retime_toggle_requested.store(
                                true, std::memory_order_release);
                        }
                        if (live_presenter->takeWidescreenToggleRequest()) {
                            widescreen_toggle_requested.store(
                                true, std::memory_order_release);
                        }
                        if (suppress_movie_start &&
                            (physical_buttons &
                             stuntmaster::presentation::movie_start_button) !=
                                0U) {
                            suppress_movie_start = false;
                        }
                        const auto pad_one_buttons =
                            suppress_movie_start
                            ? static_cast<std::uint16_t>(
                                  physical_buttons |
                                  stuntmaster::presentation::
                                      movie_start_button)
                            : physical_buttons;
                        latest_pad_one_buttons.store(
                            pad_one_buttons, std::memory_order_release);
                        if (options->input_trace &&
                            pad_one_buttons != previous_pad_one_buttons) {
                            printPadOneTransition(pad_one_buttons);
                        }
                        previous_pad_one_buttons = pad_one_buttons;
                        next_host_input += host_vblank_step;
                        while (next_host_input <= now) {
                            next_host_input += host_vblank_step;
                        }
                    }

                    if (now >= next_host_presentation) {
                        if (auto newest = frame_mailbox.takeLatest()) {
                            if (suppress_guest_frames_until_movie) {
                                // This verified retail caller invokes another
                                // movie unconditionally. Keep the native movie
                                // or black frame visible while consuming its
                                // intervening display teardown frames.
                                if (options->frame_trace) {
                                    std::cerr
                                        << "movie_chain_frame_suppressed="
                                        << newest->sequence << '\n';
                                }
                            } else {
                                current_gpu_frame = std::move(*newest);
                            }
                            if (!suppress_guest_frames_until_movie) {
                                presented_exact_current = false;
                            }
                        }
                        if (current_gpu_frame &&
                            current_gpu_frame->ready()) {
                            updateDebugOverlay(
                                &*current_gpu_frame,
                                current_gpu_frame->high_frequency_active);
                            const auto presentation_started =
                                HostClock::now();
                            if (options->persistent_framebuffer) {
                                // Stage 1 persistent framebuffer: no
                                // interpolation, no reconstruct. Apply this
                                // publication's ordered commands to the
                                // persistent render target, or repeat the
                                // cached frame when the guest has not advanced.
                                // Every publication is self-contained, so a
                                // dropped mailbox frame costs a repeat, never a
                                // corrupt framebuffer.
                                if (!presented_gpu_frame_sequence ||
                                    *presented_gpu_frame_sequence !=
                                        current_gpu_frame->sequence) {
                                    live_presenter->presentPersistent(
                                        current_gpu_frame->vram,
                                        current_gpu_frame->packets,
                                        current_gpu_frame->display_x,
                                        current_gpu_frame->display_y,
                                        current_gpu_frame->display_width,
                                        current_gpu_frame->display_height,
                                        current_gpu_frame->segments);
                                    presented_gpu_frame_sequence =
                                        current_gpu_frame->sequence;
                                    presented_exact_current = true;
                                    ++full_host_presentations;
                                } else {
                                    live_presenter->repeatFrame();
                                    ++repeated_host_presentations;
                                }
                            } else {
                                live_presenter->repeatFrame();
                                ++repeated_host_presentations;
                            }
                            const auto presentation_elapsed =
                                HostClock::now() - presentation_started;
                            total_host_presentation_time +=
                                presentation_elapsed;
                            maximum_host_presentation_time = std::max(
                                maximum_host_presentation_time,
                                presentation_elapsed);
                            ++scheduled_host_presentations;
                        }
                        // Drain whatever the guest has mixed. This runs every
                        // host presentation slot rather than only when a frame
                        // is published, because the device also has to have its
                        // finished buffers reclaimed on a regular beat.
                        if (audio_output && audio_output->ready()) {
                            std::array<std::int16_t, 4096U> block{};
                            const auto taken = audio_ring.pop(block);
                            audio_output->submit(
                                std::span{block}.first(taken));
                        }
                        next_host_presentation += host_presentation_step;
                        now = HostClock::now();
                        while (next_host_presentation <= now) {
                            next_host_presentation += host_presentation_step;
                            ++dropped_host_presentations;
                        }
                    }

                    const auto wake_time =
                        std::min(next_host_input, next_host_presentation);
                    now = HostClock::now();
                    if (wake_time > now) {
                        std::this_thread::sleep_until(wake_time);
                    }
                }
                guest_worker.join();
            } else {
                run_guest_guarded();
            }
#else
            run_guest_guarded();
#endif
            if (guest_exception) {
                std::rethrow_exception(guest_exception);
            }
            if (bios_boundary) {
                std::cout << "guest_stop=bios_hle_boundary\n";
                printHex("bios_vector", result.pc);
                printHex("bios_function", runtime.state().gpr[9]);
            } else {
                if (result.reason == stuntmaster::psx::R3000StopReason::running) {
                    result.reason =
                        stuntmaster::psx::R3000StopReason::instruction_budget;
                    result.pc = runtime.state().pc;
                }
                std::cout << "guest_stop="
                          << stuntmaster::psx::toString(result.reason) << '\n';
            }
            std::cout << "guest_instructions=" << std::dec << result.instructions << '\n';
            std::cout << "guest_batch_calls=" << guest_batch_calls
                      << " guest_batched_instructions="
                      << guest_batched_instructions
                      << " guest_idle_instructions="
                      << guest_idle_instructions << '\n';
            if (options->motion_trace) {
                printMotionWatch(motion_watch, std::cout);
            }
            if (!bios_boundary) {
                printHex("guest_pc", result.pc);
                printHex("guest_instruction", result.instruction);
                printGuestCodeWindow(runtime, result.pc);
                printHex("guest_ra", runtime.state().gpr[31]);
                printHex("guest_sp", runtime.state().gpr[29]);
                std::uint32_t caller_ra{};
                if (runtime.read32(runtime.state().gpr[29] + 0x18U, caller_ra)) {
                    printHex("guest_stack_ra_18", caller_ra);
                }
                printHex("guest_a0", runtime.state().gpr[4]);
                printHex("guest_a1", runtime.state().gpr[5]);
                printHex("guest_a2", runtime.state().gpr[6]);
                printHex("guest_a3", runtime.state().gpr[7]);
                printHex("guest_s0", runtime.state().gpr[16]);
                printHex("guest_s1", runtime.state().gpr[17]);
                printHex("guest_s2", runtime.state().gpr[18]);
                printHex("guest_s6", runtime.state().gpr[22]);
                printHex("cd_current_lba", retail_hle.currentCdLba());
                printHex("cd_status", retail_hle.cdStatus());
                std::cout << "cd_read_speed=" << std::dec
                          << retail_hle.cdReadSpeed() << '\n';
                std::cout << "cd_configured_read_speed=" << std::dec
                          << options->cd_read_speed << '\n';
                std::cout << "cd_read_calls=" << std::dec
                          << retail_hle.cdReadCalls() << '\n';
                std::cout << "cd_sectors_read=" << std::dec
                          << retail_hle.cdSectorsRead() << '\n';
                std::cout << "cd_load_tail_entries=" << std::dec
                          << cd_load_tail_entries << '\n';
                std::cout << "cd_load_tail_publications=" << std::dec
                          << cd_load_tail_publications << '\n';
                std::cout << "gpu_command_count=" << std::dec
                          << gpu.commandCount() << '\n';
                std::cout << "gpu_dma2_transfers=" << std::dec
                          << gpu.dma2TransferCount() << '\n';
                std::cout << "gpu_dma2_linked_commands=" << std::dec
                          << gpu.dma2LinkedListCommands() << '\n';
                std::cout << "gpu_dma2_block_commands=" << std::dec
                          << gpu.dma2BlockCommands() << '\n';
                printHex("gpu_dma2_bcr", gpu.dma2BlockControl());
                printHex("gpu_dma2_chcr", gpu.dma2ChannelControl());
                std::cout << "gpu_decoded_packets=" << std::dec
                          << gpu_decoder.packetCount() << '\n';
                std::cout << "gpu_decoded_primitives=" << std::dec
                          << gpu_decoder.primitiveCount() << '\n';
                std::cout << "gpu_decoded_environment=" << std::dec
                          << gpu_decoder.environmentCount() << '\n';
                const auto current_primitives =
                    countGpuPrimitives(current_gpu_packets);
                const auto& gpu_frame_packets =
                    !renderable_gpu_frame.packets.empty()
                    ? renderable_gpu_frame.packets
                    : (current_primitives != 0U
                        ? current_gpu_packets
                        : completed_gpu_packets);
                std::cout << "gpu_frame_packets=" << std::dec
                          << gpu_frame_packets.size() << '\n';
                std::cout << "gpu_frame_primitives=" << std::dec
                          << countGpuPrimitives(gpu_frame_packets) << '\n';
                printGpuFrameSummary(gpu_frame_packets);
                std::cout << "gpu_image_uploads=" << std::dec
                          << gpu_decoder.imageUploadCount() << '\n';
                std::cout << "gpu_image_pixels=" << std::dec
                          << gpu_decoder.imagePixelCount() << '\n';
                std::cout << "gpu_vram_copies=" << std::dec
                          << gpu_decoder.vramCopyCount() << '\n';
                // The refused remainder is copies reading pixels the guest
                // rasterized, which this decoder does not model.
                std::cout << "gpu_vram_copies_applied=" << std::dec
                          << gpu_decoder.vramCopiesApplied() << '\n';
                std::cout << "retail_queue_layer_calls=" << std::dec
                          << retail_queue_layer_calls
                          << " retail_queue_swap_calls=" << std::dec
                          << retail_queue_swap_calls
                          << " retail_wait_for_layer_calls=" << std::dec
                          << retail_wait_for_layer_calls << '\n';
                if (options->have_audio_capture) {
                    writeWavFile(options->audio_capture, captured_audio);
                    std::cout << "audio_capture=" << options->audio_capture.string()
                              << " frames=" << std::dec
                              << captured_audio.size() / 2U << '\n';
                }
                for (std::uint32_t v = 0U; v < 24U; ++v) {
                    const auto& voice = spu.voice(v);
                    // A voice retail never gave a pitch is one it never used.
                    if (voice.sample_rate == 0U) {
                        continue;
                    }
                    std::cout << "spu_voice" << std::dec << v
                              << " start=0x" << std::hex
                              << voice.start_address * 8U
                              << " repeat=0x" << voice.repeat_address * 8U
                              << std::dec << " rate=0x" << std::hex
                              << voice.sample_rate << std::dec
                              << " vol=" << voice.volume_left << ','
                              << voice.volume_right
                              << " adsr_lo=0x" << std::hex << voice.adsr_lo
                              << " adsr_hi=0x" << voice.adsr_hi
                              << " cur_vol=0x" << voice.current_volume
                              << std::dec
                              << '\n';
                }
                std::cout << "spu_register_writes=" << std::dec
                          << spu.registerWrites()
                          << " spu_key_on=" << spu.keyOnCount()
                          << " spu_key_off=" << spu.keyOffCount()
                          << " spu_sound_ram_bytes=" << spu.soundRamBytesWritten()
                          << " spu_main_vol=0x" << std::hex
                          << spu.mainVolumeLeft() << ',' << spu.mainVolumeRight()
                          << std::dec
                          << " spu_irq_raised=" << std::dec << spu.irqCount()
                          << " spu_irq_delivered=" << scheduled_spu_irq_callbacks
                          << " spu_irq=0x" << std::hex << spu.irqAddress()
                          << std::dec << " spu_irq_enabled=" << spu.irqEnabled()
                          << " spu_control=0x" << std::hex << spu.control()
                          << std::dec << '\n';
                std::cout << "spu_reverb_voices=0x" << std::hex
                          << spu.reverbVoices()
                          << " spu_reverb_vol=0x" << spu.reverbVolumeLeft()
                          << ',' << spu.reverbVolumeRight()
                          << " spu_reverb_work=0x" << spu.reverbWorkAddress()
                          << " spu_reverb_regs=";
                for (const auto value : spu.reverbRegisters()) {
                    std::cout << std::hex << value << ',';
                }
                std::cout << std::dec << '\n';
                std::cout << "gpu_draw_sync_boundaries=" << std::dec
                          << gpu_draw_sync_boundaries
                          << " gpu_completions_waiting_at_boundary=" << std::dec
                          << gpu_completions_waiting_at_boundary << '\n';
                std::cout << "gpu_dma2_linked_list_transfers=" << std::dec
                          << gpu.dma2LinkedListTransfers()
                          << " gpu_dma2_block_transfers=" << std::dec
                          << gpu.dma2BlockTransfers()
                          << " gpu_completions_outstanding=" << std::dec
                          << outstanding_gpu_completions
                          << " gpu_completions_discarded=" << std::dec
                          << discarded_gpu_completions << '\n';
                std::cout << "gpu_display_starts=" << std::dec
                          << gpu_display_starts
                          << " gpu_display_origin_changes=" << std::dec
                          << gpu_display_origin_changes << '\n';
                std::cout << "gpu_segment_mismatches=" << std::dec
                          << gpu_segment_mismatches << '\n';
                const auto decoded_vram = gpu_decoder.vram();
                const auto nonzero_vram = std::ranges::count_if(
                    decoded_vram, [](std::uint16_t pixel) {
                        return pixel != 0U;
                    });
                std::uint64_t nonzero_display = 0U;
                for (std::uint32_t y = 0; y < 240U; ++y) {
                    for (std::uint32_t x = 0; x < 320U; ++x) {
                        const auto vram_x =
                            (gpu.displayStartX() + x) & 1023U;
                        const auto vram_y =
                            (gpu.displayStartY() + y) & 511U;
                        if (decoded_vram[vram_y * 1024U + vram_x] != 0U) {
                            ++nonzero_display;
                        }
                    }
                }
                std::cout << "gpu_vram_nonzero=" << std::dec
                          << nonzero_vram << '\n';
                for (std::uint32_t page_y = 0U; page_y < 512U;
                     page_y += 256U) {
                    for (std::uint32_t page_x = 0U; page_x < 1024U;
                         page_x += 256U) {
                        std::uint32_t page_nonzero = 0U;
                        for (std::uint32_t y = 0U; y < 256U; ++y) {
                            for (std::uint32_t x = 0U; x < 256U; ++x) {
                                if (decoded_vram[
                                        (page_y + y) * 1024U + page_x + x] !=
                                    0U) {
                                    ++page_nonzero;
                                }
                            }
                        }
                        std::cout << "gpu_vram_page_" << page_x << '_'
                                  << page_y << "_nonzero=" << std::dec
                                  << page_nonzero << '\n';
                    }
                }
                std::cout << "gpu_display_nonzero=" << std::dec
                          << nonzero_display << '\n';
                std::cout << "scheduled_vblanks=" << std::dec
                          << scheduled_vblanks << '\n';
                std::cout << "scheduled_gpu_callbacks=" << std::dec
                          << scheduled_gpu_callbacks << '\n';
                std::cout << "scheduled_vsync_callbacks=" << std::dec
                          << scheduled_vsync_callbacks << '\n';
                std::cout << "callbacks_during_image_transfer=" << std::dec
                          << callbacks_during_image_transfer << '\n';
                const auto& largest = gpu_decoder.largestImageUpload();
                std::cout << "gpu_max_image_upload=" << std::dec
                          << largest.width << 'x' << largest.height
                          << " at " << largest.x << ',' << largest.y << '\n';
                std::cout << "gpu_image_pixels=" << std::dec
                          << gpu_decoder.imagePixelCount() << '\n';
                std::cout << "gpu_vram_fill=" << std::dec
                          << sampledVramOccupancy(gpu_decoder.vram()) << '\n';
                if (options->have_watch_writes) {
                    std::cout << "watched_writes=" << std::dec
                              << watched_writes << " watched_writers="
                              << watched_writers.size() << '\n';
                    std::vector<std::pair<std::uint32_t, std::uint64_t>> ranked{
                        watched_writers.begin(), watched_writers.end()};
                    std::ranges::sort(
                        ranked, [](const auto& left, const auto& right) {
                            return left.second > right.second;
                        });
                    if (ranked.size() > 24U) {
                        ranked.resize(24U);
                    }
                    for (const auto& [pc, count] : ranked) {
                        std::cout << "watched_writer pc=0x" << std::hex
                                  << std::setw(8) << std::setfill('0') << pc
                                  << std::dec << std::setfill(' ')
                                  << " writes=" << count << '\n';
                    }
                }
                std::cout << "swap_gate_unlocks=" << std::dec
                          << swap_gate_unlocks << " swap_gate_relocks="
                          << swap_gate_relocks << '\n';
                std::cout << "gpu_uncovered_texture_pages="
                          << uncovered_texture_pages.size() << '\n';
                for (const auto page : uncovered_texture_pages) {
                    printHex("gpu_uncovered_tpage", page);
                }
                std::cout << "scheduled_spu_callbacks=" << std::dec
                          << scheduled_spu_callbacks << '\n';
#ifdef STUNTMASTER_HAS_PSYCROSS
                // The device only exists in a build that has a presentation
                // backend, which is the same build that can open a window.
                if (audio_output) {
                    std::cout << "audio_device=" << std::dec
                              << (audio_output->ready() ? "open" : "none")
                              << " audio_starved=" << audio_output->starvedCount()
                              << " audio_dropped_submissions="
                              << audio_output->droppedSubmissions()
                              << " audio_ring_dropped=" << audio_ring.dropped()
                              << '\n';
                }
#endif
                std::cout << "scheduled_host_presentations=" << std::dec
                          << scheduled_host_presentations << '\n';
                std::cout << "full_host_presentations=" << std::dec
                          << full_host_presentations << '\n';
                std::cout << "repeated_host_presentations=" << std::dec
                          << repeated_host_presentations << '\n';
                std::cout << "dropped_host_presentations=" << std::dec
                          << dropped_host_presentations << '\n';
                const auto frame_mailbox_statistics =
                    frame_mailbox.statistics();
                std::cout << "frame_mailbox_published=" << std::dec
                          << frame_mailbox_statistics.published << '\n';
                std::cout << "frame_mailbox_dropped_on_publish=" << std::dec
                          << frame_mailbox_statistics.dropped_on_publish
                          << '\n';
                std::cout << "frame_mailbox_skipped_on_consume=" << std::dec
                          << frame_mailbox_statistics.skipped_on_consume
                          << '\n';
                const auto total_host_presentation_ms =
                    std::chrono::duration<double, std::milli>{
                        total_host_presentation_time}.count();
                const auto maximum_host_presentation_ms =
                    std::chrono::duration<double, std::milli>{
                        maximum_host_presentation_time}.count();
                std::cout << "average_host_presentation_ms=" << std::dec
                          << (scheduled_host_presentations != 0U
                                  ? total_host_presentation_ms /
                                      static_cast<double>(
                                          scheduled_host_presentations)
                                  : 0.0)
                          << '\n';
                std::cout << "maximum_host_presentation_ms=" << std::dec
                          << maximum_host_presentation_ms << '\n';
                printHex("gpu_last_command", gpu.lastCommand());
                printHex("gpu_display_x", gpu.displayStartX());
                printHex("gpu_display_y", gpu.displayStartY());
                printHex("gpu_display_mode", gpu.displayMode());
                std::cout << "gpu_display_width=" << std::dec
                          << gpu.displayWidth() << '\n';
                std::cout << "gpu_display_height=" << std::dec
                          << gpu.displayHeight() << '\n';
                std::uint32_t view{};
                std::uint32_t layers{};
                std::uint32_t layer6{};
                std::uint32_t layer6_state{};
                if (runtime.read32(0x800DD780U, view) &&
                    runtime.read32(view + 0x10U, layers) &&
                    runtime.read32(layers + 6U * sizeof(std::uint32_t), layer6) &&
                    runtime.read32(layer6 + 0x0CU, layer6_state)) {
                    printHex("render_view", view);
                    printHex("render_layer6", layer6);
                    printHex("render_layer6_state", layer6_state);
                }
                std::uint32_t render_queue{};
                if (runtime.read32(0x800DD7A0U, render_queue)) {
                    printHex("render_queue", render_queue);
                    for (std::uint32_t offset = 0U; offset <= 0x1CU;
                         offset += 4U) {
                        std::uint32_t value{};
                        if (runtime.read32(render_queue + offset, value)) {
                            std::cout << "render_queue_" << std::hex
                                      << std::uppercase << std::setw(2)
                                      << std::setfill('0') << offset << "=0x"
                                      << std::setw(8) << value << '\n';
                        }
                    }
                }
                std::uint32_t draw_sync_callback{};
                if (runtime.read32(0x800D51A0U, draw_sync_callback)) {
                    printHex("draw_sync_callback", draw_sync_callback);
                }
                printMmioSnapshot(runtime);
            }
            if (options->show_frame || options->capture_frame) {
#ifdef STUNTMASTER_HAS_PSYCROSS
                const auto current_primitives =
                    countGpuPrimitives(current_gpu_packets);
                const auto& gpu_frame_packets =
                    !renderable_gpu_frame.packets.empty()
                    ? renderable_gpu_frame.packets
                    : (current_primitives != 0U
                        ? current_gpu_packets
                        : completed_gpu_packets);
                const auto gpu_frame_vram =
                    !renderable_gpu_frame.vram.empty()
                    ? std::span<const std::uint16_t>{
                        renderable_gpu_frame.vram}
                    : gpu_decoder.vram();
                const auto gpu_frame_display_x =
                    !renderable_gpu_frame.vram.empty()
                    ? renderable_gpu_frame.display_x
                    : gpu.displayStartX();
                const auto gpu_frame_display_y =
                    !renderable_gpu_frame.vram.empty()
                    ? renderable_gpu_frame.display_y
                    : gpu.displayStartY();
                const auto gpu_frame_display_width =
                    !renderable_gpu_frame.vram.empty()
                    ? renderable_gpu_frame.display_width
                    : gpu.displayWidth();
                const auto gpu_frame_display_height =
                    !renderable_gpu_frame.vram.empty()
                    ? renderable_gpu_frame.display_height
                    : gpu.displayHeight();
                stuntmaster::presentation::PsyCrossPresenter presenter{
                    {},
                    options->window_width,
                    options->window_height,
                    options->render_width,
                    options->render_height};
                presenter.setCompositeDisplayPage(
                    options->framebuffer_composite);
                if (options->capture_frame) {
                    presenter.captureScreenshot(
                        gpu_frame_vram,
                        gpu_frame_packets,
                        gpu_frame_display_x,
                        gpu_frame_display_y,
                        gpu_frame_display_width,
                        gpu_frame_display_height);
                    std::cout << "frame_capture=SCREENSHOT.BMP\n";
                } else {
                    presenter.showUntilClosed(
                        gpu_frame_vram,
                        gpu_frame_packets,
                        gpu_frame_display_x,
                        gpu_frame_display_y,
                        gpu_frame_display_width,
                        gpu_frame_display_height);
                }
#else
                throw stuntmaster::core::Error{
                    "frame presentation requires "
                    "STUNTMASTER_ENABLE_PSYCROSS=ON"};
#endif
            }
}

int runApplication(const Options& options) {
    if (options.have_replay_capture) {
        replayCapturedFrameWithPresenter(options);
        return 0;
    }

    auto loaded_game = loadGame(options);
    if (options.probe_guest || options.run_live) {
        runGuestSession(options, loaded_game);
    } else {
        std::cout << "status=retail executable loaded; use --probe-guest for "
                     "the first execution boundary\n";
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view{argv[1]} == "--help") {
        usage();
        return 0;
    }
    const auto options = parseOptions(argc, argv);
    if (!options) {
        usage();
        return 2;
    }
    try {
        return runApplication(*options);
    } catch (const std::exception& error) {
        std::cerr << "stuntmaster: error: " << error.what() << '\n';
        return 1;
    }
}
