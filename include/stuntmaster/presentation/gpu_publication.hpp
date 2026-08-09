#pragma once

#include "stuntmaster/presentation/psycross_presenter.hpp"

#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace stuntmaster::presentation {

// A stopped-flip upload is a reason to refresh the enhanced presenter only at
// a display-controller boundary. Uploads can complete thousands of times
// inside one VBlank during a level load; publishing each completion exposes
// partially assembled texture state and floods the lossy live mailbox.
[[nodiscard]] constexpr bool shouldPublishStoppedFlipUpload(
    bool vblank_boundary_pending,
    std::uint32_t vblanks_since_display_flip,
    std::uint64_t vram_revision,
    std::uint64_t published_vram_revision) noexcept {
    return vblank_boundary_pending && vblanks_since_display_flip >= 3U &&
        vram_revision != published_vram_revision;
}

// Build a drop-safe replay value without consuming the interval accumulator.
// A fallback publication is not an interval boundary: another upload may land
// before retail flips again, and the next value must still contain every VRAM
// revision that earlier draws sampled. Pending packets after the last draw get
// a publication-only tail snapshot so a completed upload is never replayed
// against the preceding segment's stale VRAM.
[[nodiscard]] inline std::vector<GpuReplaySegment>
buildSelfContainedReplaySegments(
    const std::vector<GpuReplaySegment>& settled_segments,
    const std::vector<std::vector<std::uint32_t>>& pending_packets,
    std::span<const std::uint16_t> vram,
    std::uint64_t vram_revision) {
    auto result = settled_segments;
    if (pending_packets.empty() || result.empty()) {
        return result;
    }
    if (result.back().vram_revision == vram_revision) {
        result.back().packets.insert(
            result.back().packets.end(),
            pending_packets.begin(),
            pending_packets.end());
        return result;
    }

    GpuReplaySegment tail;
    tail.packets = pending_packets;
    tail.vram.assign(vram.begin(), vram.end());
    tail.vram_revision = vram_revision;
    result.push_back(std::move(tail));
    return result;
}

} // namespace stuntmaster::presentation
