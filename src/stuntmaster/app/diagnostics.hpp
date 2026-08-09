#pragma once

#include "gpu_frame.hpp"

#include "stuntmaster/psx/gpu_command_decoder.hpp"
#include "stuntmaster/psx/r3000_runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <ostream>
#include <span>
#include <utility>
#include <vector>

namespace stuntmaster::app {

void printHex(const char* name, std::uint32_t value);

void printPadOneTransition(std::uint16_t active_low_buttons);

void printMmioSnapshot(const stuntmaster::psx::R3000Runtime& runtime);

void printGuestCodeWindow(
    const stuntmaster::psx::R3000Runtime& runtime, std::uint32_t pc);

std::size_t countGpuPrimitives(
    const std::vector<std::vector<std::uint32_t>>& packets);

// Healthy retail VRAM is roughly half occupied, with large unused regions
// between texture pages. A runaway CPU-to-VRAM transfer floods it with the
// words that follow, taking occupancy close to one, so a sparse sample of it
// is a cheap per-frame detector for textures turning to noise.
double sampledVramOccupancy(std::span<const std::uint16_t> vram) noexcept;

// Counts textured polygons whose sampled VRAM region was never written by a
// CPU-to-VRAM upload. Those cannot render correctly: the decoder holds stale
// bytes there because it never rasterizes primitives or applies VRAM-to-VRAM
// copies. Texture pages are shared, so this works at texel granularity rather
// than per page.
std::size_t countUncoveredTexturedPolygons(
    const std::vector<std::vector<std::uint32_t>>& packets,
    const stuntmaster::psx::GpuCommandDecoder& decoder,
    std::vector<std::uint32_t>& reported_pages);

std::size_t countGpuPolygons(
    const std::vector<std::vector<std::uint32_t>>& packets);

void printGpuFrameSummary(
    const std::vector<std::vector<std::uint32_t>>& packets);

// Finds where retail integrates motion, by watching every guest write into the
// live `thePlayer` object and recording which code did it. Static reading does
// not answer this on its own: motion is integrated inside per-class
// `Move`/`UpdatePosition` methods that mostly live in the disc overlays.
//
// Owned and used entirely by the guest worker thread.
struct MotionWatch {
    struct Site {
        std::uint64_t writes{};
        std::uint32_t size{};
        std::uint32_t last_value{};
        std::uint32_t previous_value{};
        bool changed{};
    };

    static constexpr std::uint32_t the_player_address = 0x800DD6B4U;
    // The retail `Player` class is 764 bytes. Watch all of it.
    static constexpr std::uint32_t watched_bytes = 764U;

    // Animation state is not in `Player`. `Player::CreateModel` allocates a
    // 136-byte `PlayerModel` and stores it at `Player+0x50`, and
    // `PlayerModel::SetAnim` reaches its `AnimStructure` through
    // `PlayerModel+0x20`. Follow that chain so the animation advance can be
    // located the same way the player's gravity was.
    static constexpr std::uint32_t player_model_offset = 0x50U;
    static constexpr std::uint32_t anim_structure_offset = 0x20U;
    static constexpr std::uint32_t anim_watched_bytes = 0x80U;

    // `rFrameCount60`, which MyVBL__Fe increments every VBlank, is the control
    // that proves the write sink is live.
    static constexpr std::uint32_t control_address = 0x800DD678U;

    std::uint32_t object{};
    std::uint32_t anim_object{};
    std::map<std::pair<std::uint32_t, std::uint32_t>, Site> anim_sites;
    std::uint64_t observed_writes{};
    std::uint64_t object_changes{};
    std::uint64_t total_writes{};
    std::uint64_t control_writes{};
    std::map<std::pair<std::uint32_t, std::uint32_t>, Site> sites;

    void refresh(const stuntmaster::psx::R3000Runtime& runtime) noexcept {
        std::uint32_t resolved = 0U;
        if (runtime.read32(the_player_address, resolved) &&
            resolved != object) {
            object = resolved;
            ++object_changes;
        }
        anim_object = 0U;
        std::uint32_t model = 0U;
        std::uint32_t anim = 0U;
        if (object != 0U &&
            runtime.read32(object + player_model_offset, model) &&
            model != 0U &&
            runtime.read32(model + anim_structure_offset, anim)) {
            anim_object = anim;
        }
    }

    // A per-game-frame sample of the fields that decide a trajectory. Write
    // counts locate code; only a time series can say whether a retimed
    // trajectory actually matches the retail one.
    void sample(
        const stuntmaster::psx::R3000Runtime& runtime,
        std::uint64_t guest_vblank,
        std::ostream& out) const {
        if (object == 0U) {
            return;
        }
        const auto read = [&runtime, this](std::uint32_t offset) {
            std::uint32_t value = 0U;
            return runtime.read32(object + offset, value)
                ? static_cast<std::int32_t>(value)
                : 0;
        };
        out << "motion_frame vblank=" << std::dec << guest_vblank
            << " px=" << read(0x7CU) << " py=" << read(0x80U)
            << " pz=" << read(0x84U) << " vx=" << read(0x64U)
            << " vy=" << read(0x68U) << " vz=" << read(0x6CU)
            << " ex=" << read(0x70U) << " ey=" << read(0x74U)
            << " ez=" << read(0x78U) << " grav=" << read(0xC4U)
            << " flags=0x" << std::hex << read(0x58U) << std::dec << '\n';
    }

    static void record(
        std::map<std::pair<std::uint32_t, std::uint32_t>, Site>& sites,
        std::uint32_t offset,
        std::uint32_t size,
        std::uint32_t value,
        std::uint32_t pc) {
        auto& site = sites[{offset, pc}];
        ++site.writes;
        site.size = size;
        if (site.writes > 1U && value != site.last_value) {
            site.changed = true;
        }
        site.previous_value = site.last_value;
        site.last_value = value;
    }

    void observe(
        std::uint32_t address,
        std::uint32_t size,
        std::uint32_t value,
        std::uint32_t pc) {
        ++total_writes;
        if (address == control_address) {
            ++control_writes;
        }
        if (anim_object != 0U && address >= anim_object &&
            address < anim_object + anim_watched_bytes) {
            record(anim_sites, address - anim_object, size, value, pc);
        }
        if (object == 0U || address < object ||
            address >= object + watched_bytes) {
            return;
        }
        ++observed_writes;
        auto& site = sites[{address - object, pc}];
        ++site.writes;
        site.size = size;
        if (site.writes > 1U && value != site.last_value) {
            site.changed = true;
        }
        site.previous_value = site.last_value;
        site.last_value = value;
    }
};

void printMotionWatchSites(
    const char* prefix,
    const std::map<std::pair<std::uint32_t, std::uint32_t>,
                   MotionWatch::Site>& sites,
    std::ostream& out);

void printMotionWatch(const MotionWatch& watch, std::ostream& out);

void printRetainedGpuFrameTrace(const RetainedGpuFrame& frame);

} // namespace stuntmaster::app
