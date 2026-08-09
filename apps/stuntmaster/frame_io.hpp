#pragma once

#include "gpu_frame.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

namespace stuntmaster::app {

// Reinterprets raw disc bytes as text, for reading SYSTEM.CNF and friends.
std::string_view asText(std::span<const std::byte> bytes);

// A 16-bit stereo 44.1 kHz WAV, which is the only format this needs to write and
// the one every tool can already open.
void writeWavFile(
    const std::filesystem::path& path,
    std::span<const std::int16_t> samples);

// One `--frame-capture-trace` dump: the packet stream the presenter replayed
// and the VRAM it sampled, in the format `dumpDiagnosticFrames` writes.
struct CapturedFrame {
    std::vector<std::vector<std::uint32_t>> packets;
    std::vector<std::uint16_t> vram;
    std::uint32_t display_x{};
    std::uint32_t display_y{};
    std::uint32_t display_width{};
    std::uint32_t display_height{};
};

[[nodiscard]] std::vector<std::uint32_t> readWords(
    const std::filesystem::path& path);

[[nodiscard]] CapturedFrame loadCapturedFrame(
    const std::filesystem::path& packet_path);

void writeCapturedGpuFrame(
    const RetainedGpuFrame& frame,
    std::span<const std::uint16_t> vram,
    const std::filesystem::path& packet_destination);

// A scanout rectangle of VRAM, written as a bottom-up 24-bit BMP.
void writeScanoutBitmap(
    std::span<const std::uint16_t> vram,
    std::uint32_t x,
    std::uint32_t y,
    std::uint32_t width,
    std::uint32_t height,
    const std::filesystem::path& destination,
    // Source pitch. Full VRAM is 1024 wide; a published frame is packed to its
    // own width, and reading it at the VRAM pitch walks off the end of the
    // buffer -- which read uninitialized memory and then faulted, after being
    // mistaken for evidence about the frame's contents.
    std::uint32_t stride = 1024U);

} // namespace stuntmaster::app
