#include "diagnostics.hpp"

#include <algorithm>
#include <array>
#include <iomanip>
#include <iostream>

namespace stuntmaster::app {

namespace {

// PS1 screen coordinates are signed 11-bit, in the E5 draw offset as well as in
// vertex fields.
[[nodiscard]] std::int32_t signed11(std::uint32_t value) noexcept {
    value &= 0x7FFU;
    if ((value & 0x400U) != 0U) {
        value |= 0xFFFFF800U;
    }
    return static_cast<std::int32_t>(value);
}

} // namespace

void printHex(const char* name, std::uint32_t value) {
    std::cout << name << "=0x" << std::hex << std::uppercase << std::setw(8)
              << std::setfill('0') << value << '\n';
}

void printPadOneTransition(std::uint16_t active_low_buttons) {
    constexpr std::array button_names{
        std::pair{std::uint16_t{0x0001U}, "select"},
        std::pair{std::uint16_t{0x0002U}, "l3"},
        std::pair{std::uint16_t{0x0004U}, "r3"},
        std::pair{std::uint16_t{0x0008U}, "start"},
        std::pair{std::uint16_t{0x0010U}, "up"},
        std::pair{std::uint16_t{0x0020U}, "right"},
        std::pair{std::uint16_t{0x0040U}, "down"},
        std::pair{std::uint16_t{0x0080U}, "left"},
        std::pair{std::uint16_t{0x0100U}, "l2"},
        std::pair{std::uint16_t{0x0200U}, "r2"},
        std::pair{std::uint16_t{0x0400U}, "l1"},
        std::pair{std::uint16_t{0x0800U}, "r1"},
        std::pair{std::uint16_t{0x1000U}, "triangle"},
        std::pair{std::uint16_t{0x2000U}, "circle"},
        std::pair{std::uint16_t{0x4000U}, "cross"},
        std::pair{std::uint16_t{0x8000U}, "square"},
    };
    const auto pressed_buttons =
        static_cast<std::uint16_t>(~active_low_buttons);
    std::cout << "pad1_active_low=0x" << std::hex << std::uppercase
              << std::setw(4) << std::setfill('0') << active_low_buttons
              << " pressed=";
    bool first = true;
    for (const auto& [mask, name] : button_names) {
        if ((pressed_buttons & mask) == 0U) {
            continue;
        }
        if (!first) {
            std::cout << ',';
        }
        std::cout << name;
        first = false;
    }
    if (first) {
        std::cout << "none";
    }
    std::cout << '\n';
}

void printMmioSnapshot(const stuntmaster::psx::R3000Runtime& runtime) {
    constexpr std::array registers{
        std::pair{"irq_status", 0x1F801070U},
        std::pair{"irq_mask", 0x1F801074U},
        std::pair{"timer0_counter", 0x1F801100U},
        std::pair{"timer0_mode", 0x1F801104U},
        std::pair{"timer0_target", 0x1F801108U},
        std::pair{"timer1_counter", 0x1F801110U},
        std::pair{"timer1_mode", 0x1F801114U},
        std::pair{"timer1_target", 0x1F801118U},
        std::pair{"timer2_counter", 0x1F801120U},
        std::pair{"timer2_mode", 0x1F801124U},
        std::pair{"timer2_target", 0x1F801128U},
        std::pair{"gpu_status", 0x1F801814U},
    };
    for (const auto& [name, address] : registers) {
        std::uint32_t value{};
        if (runtime.read32(address, value)) {
            printHex(name, value);
        }
    }
}

void printGuestCodeWindow(
    const stuntmaster::psx::R3000Runtime& runtime, std::uint32_t pc) {
    constexpr std::uint32_t bytes_before = 16U;
    for (std::uint32_t offset = 0; offset < 9U; ++offset) {
        const auto address = pc - bytes_before + offset * 4U;
        std::uint32_t instruction{};
        if (runtime.read32(address, instruction)) {
            std::cout << "guest_code_" << std::dec
                      << static_cast<std::int32_t>(offset * 4U) -
                             static_cast<std::int32_t>(bytes_before)
                      << "=0x" << std::hex << std::uppercase << std::setw(8)
                      << std::setfill('0') << instruction << '\n';
        }
    }
}

std::size_t countGpuPrimitives(
    const std::vector<std::vector<std::uint32_t>>& packets) {
    return std::ranges::count_if(
        packets, [](const std::vector<std::uint32_t>& packet) {
            if (packet.empty()) {
                return false;
            }
            const auto opcode =
                static_cast<std::uint8_t>(packet.front() >> 24U);
            return opcode >= 0x20U && opcode <= 0x7FU;
        });
}

double sampledVramOccupancy(std::span<const std::uint16_t> vram) noexcept {
    constexpr std::size_t stride = 61U; // coprime with the row pitch
    std::size_t sampled = 0U;
    std::size_t occupied = 0U;
    for (std::size_t index = 0U; index < vram.size(); index += stride) {
        ++sampled;
        if (vram[index] != 0U) {
            ++occupied;
        }
    }
    return sampled == 0U
        ? 0.0
        : static_cast<double>(occupied) / static_cast<double>(sampled);
}

std::size_t countUncoveredTexturedPolygons(
    const std::vector<std::vector<std::uint32_t>>& packets,
    const stuntmaster::psx::GpuCommandDecoder& decoder,
    std::vector<std::uint32_t>& reported_pages) {
    std::size_t uncovered = 0U;
    for (const auto& packet : packets) {
        if (packet.empty()) {
            continue;
        }
        const auto opcode = static_cast<std::uint8_t>(packet.front() >> 24U);
        if (opcode < 0x20U || opcode > 0x3FU || (opcode & 0x04U) == 0U) {
            continue;
        }
        const auto vertices = (opcode & 0x08U) != 0U ? 4U : 3U;
        const auto gouraud = (opcode & 0x10U) != 0U;
        std::uint32_t index = 1U;
        std::uint32_t tpage = 0U;
        std::uint32_t u0 = 256U;
        std::uint32_t u1 = 0U;
        std::uint32_t v0 = 256U;
        std::uint32_t v1 = 0U;
        bool malformed = false;
        for (std::uint32_t vertex = 0U; vertex < vertices; ++vertex) {
            if (gouraud && vertex > 0U) {
                ++index;
            }
            ++index; // position
            if (index >= packet.size()) {
                malformed = true;
                break;
            }
            const auto attribute = packet[index];
            if (vertex == 1U) {
                tpage = (attribute >> 16U) & 0xFFFFU;
            }
            u0 = std::min(u0, attribute & 0xFFU);
            u1 = std::max(u1, attribute & 0xFFU);
            v0 = std::min(v0, (attribute >> 8U) & 0xFFU);
            v1 = std::max(v1, (attribute >> 8U) & 0xFFU);
            ++index;
        }
        if (malformed) {
            continue;
        }
        // Texture-page base is 64 halfwords in X and 256 lines in Y. U is in
        // texels, so scale it into halfwords by the page's colour depth.
        const auto depth = (tpage >> 7U) & 0x3U;
        const auto shift = depth == 0U ? 2U : (depth == 1U ? 1U : 0U);
        const auto base_x = (tpage & 0xFU) * 64U;
        const auto base_y = ((tpage >> 4U) & 0x1U) * 256U;
        const auto x = base_x + (u0 >> shift);
        const auto width = ((u1 - u0) >> shift) + 1U;
        if (!decoder.uploadCovers(x, base_y + v0, width, v1 - v0 + 1U)) {
            ++uncovered;
            if (std::ranges::find(reported_pages, tpage) ==
                reported_pages.end()) {
                reported_pages.push_back(tpage);
                // Report the region once so the missing data can be traced to
                // whatever produced it.
                std::cerr << "uncovered_region tpage=0x" << std::hex << tpage
                          << std::dec << " depth="
                          << (depth == 0U ? "4bpp"
                                          : (depth == 1U ? "8bpp" : "15bpp"))
                          << " vram=" << x << ',' << (base_y + v0) << ' '
                          << width << 'x' << (v1 - v0 + 1U) << std::endl;
            }
        }
    }
    return uncovered;
}

std::size_t countGpuPolygons(
    const std::vector<std::vector<std::uint32_t>>& packets) {
    return std::ranges::count_if(
        packets, [](const std::vector<std::uint32_t>& packet) {
            if (packet.empty()) {
                return false;
            }
            const auto opcode =
                static_cast<std::uint8_t>(packet.front() >> 24U);
            return opcode >= 0x20U && opcode <= 0x3FU;
        });
}

void printGpuFrameSummary(
    const std::vector<std::vector<std::uint32_t>>& packets) {
    std::array<std::uint32_t, 256> opcode_counts{};
    std::array<std::vector<std::uint32_t>, 3> drawing_environment_words;
    for (const auto& packet : packets) {
        if (packet.empty()) {
            continue;
        }
        const auto opcode =
            static_cast<std::uint8_t>(packet.front() >> 24U);
        ++opcode_counts[opcode];
        if (opcode >= 0xE3U && opcode <= 0xE5U) {
            auto& values = drawing_environment_words[opcode - 0xE3U];
            if (std::ranges::find(values, packet.front()) == values.end()) {
                values.push_back(packet.front());
            }
        }
    }
    for (std::size_t opcode = 0; opcode < opcode_counts.size(); ++opcode) {
        if (opcode_counts[opcode] == 0U) {
            continue;
        }
        std::cout << "gpu_frame_opcode_" << std::hex << std::uppercase
                  << std::setw(2) << std::setfill('0') << opcode << '='
                  << std::dec << opcode_counts[opcode] << '\n';
    }
    for (std::size_t environment = 0U;
         environment < drawing_environment_words.size();
         ++environment) {
        const auto opcode = environment + 0xE3U;
        const auto& values = drawing_environment_words[environment];
        for (std::size_t index = 0U; index < values.size(); ++index) {
            std::cout << "gpu_frame_env_" << std::hex << std::uppercase
                      << opcode << '_' << std::dec << index << "=0x"
                      << std::hex << std::uppercase << std::setw(8)
                      << std::setfill('0') << values[index] << '\n';
        }
    }
}

void printMotionWatchSites(
    const char* prefix,
    const std::map<std::pair<std::uint32_t, std::uint32_t>,
                   MotionWatch::Site>& sites,
    std::ostream& out) {
    // Rank by writes so per-frame work surfaces above one-shot initialization.
    std::vector<std::pair<std::pair<std::uint32_t, std::uint32_t>,
                          MotionWatch::Site>>
        ranked{sites.begin(), sites.end()};
    std::ranges::sort(ranked, [](const auto& left, const auto& right) {
        return left.second.writes > right.second.writes;
    });
    constexpr std::size_t reported_sites = 96U;
    if (ranked.size() > reported_sites) {
        ranked.resize(reported_sites);
    }
    for (const auto& [key, site] : ranked) {
        const auto& [offset, pc] = key;
        // The sink reports the instruction after the store; report the store.
        out << prefix << " offset=0x" << std::hex << std::setw(3)
            << std::setfill('0') << offset << " store_pc=0x" << std::setw(8)
            << (pc - 4U) << std::dec << std::setfill(' ')
            << " size=" << site.size << " writes=" << site.writes
            << " varying=" << (site.changed ? 1 : 0) << " last=0x" << std::hex
            << site.last_value << " prev=0x" << site.previous_value
            << std::dec << '\n';
    }
}

void printMotionWatch(const MotionWatch& watch, std::ostream& out) {
    out << "motion_object=0x" << std::hex << std::setw(8)
        << std::setfill('0') << watch.object << " motion_anim_object=0x"
        << std::setw(8) << watch.anim_object << std::dec
        << std::setfill(' ') << '\n';
    out << "motion_sites=" << std::dec << watch.sites.size()
        << " motion_anim_sites=" << watch.anim_sites.size()
        << " motion_observed_writes=" << watch.observed_writes
        << " motion_object_changes=" << watch.object_changes
        << " motion_total_writes=" << watch.total_writes
        << " motion_control_writes=" << watch.control_writes << '\n';
    printMotionWatchSites("motion_write", watch.sites, out);
    printMotionWatchSites("motion_anim_write", watch.anim_sites, out);
    out << std::flush;
}

void printRetainedGpuFrameTrace(const RetainedGpuFrame& frame) {
    std::size_t polygons = 0U;
    std::size_t lines = 0U;
    std::size_t rectangles = 0U;
    std::size_t fills = 0U;
    std::size_t vram_copies = 0U;
    std::size_t offscreen_modes = 0U;
    std::size_t scanout_primitives = 0U;
    std::size_t subclip_primitives = 0U;
    std::size_t other_page_primitives = 0U;
    auto draw_x0 = frame.display_x;
    auto draw_y0 = frame.display_y;
    auto draw_x1 = frame.display_x + frame.display_width - 1U;
    auto draw_y1 = frame.display_y + frame.display_height - 1U;
    std::uint64_t fingerprint = 1'469'598'103'934'665'603ULL;
    for (const auto& packet : frame.packets) {
        for (const auto word : packet) {
            fingerprint ^= word;
            fingerprint *= 1'099'511'628'211ULL;
        }
        if (packet.empty()) {
            continue;
        }
        const auto opcode =
            static_cast<std::uint8_t>(packet.front() >> 24U);
        if (opcode == 0xE3U) {
            draw_x0 = packet.front() & 0x3FFU;
            draw_y0 = (packet.front() >> 10U) & 0x1FFU;
        } else if (opcode == 0xE4U) {
            draw_x1 = packet.front() & 0x3FFU;
            draw_y1 = (packet.front() >> 10U) & 0x1FFU;
        }
        fills += opcode == 0x02U ? 1U : 0U;
        polygons += opcode >= 0x20U && opcode <= 0x3FU ? 1U : 0U;
        lines += opcode >= 0x40U && opcode <= 0x5FU ? 1U : 0U;
        rectangles += opcode >= 0x60U && opcode <= 0x7FU ? 1U : 0U;
        vram_copies += opcode >= 0x80U && opcode <= 0x9FU ? 1U : 0U;
        offscreen_modes +=
            opcode == 0xE1U && (packet.front() & (1U << 10U)) == 0U
            ? 1U
            : 0U;
        if (opcode >= 0x20U && opcode <= 0x7FU) {
            const auto frame_x1 =
                frame.display_x + frame.display_width - 1U;
            const auto frame_y1 =
                frame.display_y + frame.display_height - 1U;
            const auto intersects_scanout =
                draw_x0 <= frame_x1 && draw_x1 >= frame.display_x &&
                draw_y0 <= frame_y1 && draw_y1 >= frame.display_y;
            if (intersects_scanout) {
                ++scanout_primitives;
                const auto is_full_scanout_clip =
                    draw_x0 == frame.display_x &&
                    draw_y0 == frame.display_y &&
                    draw_x1 >= frame_x1 &&
                    draw_y1 >= frame_y1;
                subclip_primitives += is_full_scanout_clip ? 0U : 1U;
            } else {
                ++other_page_primitives;
            }
        }
    }
    std::cout << "gpu_frame_sequence=" << std::dec << frame.sequence
              << " guest_vblank=" << frame.guest_vblank
              << " guest_instructions=" << frame.guest_instructions
              << " retail_frame_rate=" << frame.retail_frame_rate
              << " retail_measured_frame_rate="
              << frame.retail_measured_frame_rate
              << " retail_game_ticks=" << frame.retail_game_ticks
              << " packets=" << frame.packets.size()
              << " segments=" << frame.segments.size()
              << " primitives=" << countGpuPrimitives(frame.packets)
              << " polygons=" << polygons
              << " lines=" << lines
              << " rectangles=" << rectangles
              << " fills=" << fills
              << " vram_copies=" << vram_copies
              << " offscreen_modes=" << offscreen_modes
              << " scanout_primitives=" << scanout_primitives
              << " subclip_primitives=" << subclip_primitives
              << " other_page_primitives=" << other_page_primitives
              << " display=" << frame.display_x << ',' << frame.display_y
              << ',' << frame.display_width << 'x' << frame.display_height
              << " fingerprint=0x" << std::hex << std::uppercase
              << fingerprint << std::dec << '\n';
}

} // namespace stuntmaster::app
