#pragma once

#include "stuntmaster/presentation/psycross_presenter.hpp"

#include <cstdint>
#include <vector>

namespace stuntmaster::app {

// One frame the guest worker hands to PsyCross: the packet stream, the VRAM it
// was drawn against, and retail bookkeeping for presentation and diagnostics.
struct RetainedGpuFrame {
    std::vector<std::vector<std::uint32_t>> packets;
    std::vector<std::uint16_t> vram;
    std::vector<stuntmaster::presentation::GpuReplaySegment> segments;
    std::uint32_t display_x{};
    std::uint32_t display_y{};
    std::uint32_t display_width{};
    std::uint32_t display_height{};
    std::uint64_t sequence{};
    std::uint64_t guest_vblank{};
    std::uint64_t guest_instructions{};
    std::uint32_t retail_frame_rate{};
    std::uint32_t retail_game_ticks{};
    std::uint16_t retail_measured_frame_rate{};
    // Host-owned retime hooks active for the table that produced this frame,
    // and how many of the fingerprinted overlay hooks were actually resident.
    // Guest-owned state reaches presentation only as a value here.
    std::uint32_t retime_hooks_armed{};
    std::uint32_t retime_hooks_live{};
    bool high_frequency_active{};
    [[nodiscard]] bool ready() const noexcept {
        return !packets.empty() && !vram.empty();
    }
};

} // namespace stuntmaster::app
