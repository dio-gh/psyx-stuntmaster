#include "frame_io.hpp"

#include "stuntmaster/core/error.hpp"

#include <array>
#include <fstream>
#include <ios>
#include <iterator>

namespace stuntmaster::app {

std::string_view asText(std::span<const std::byte> bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

void writeWavFile(
    const std::filesystem::path& path,
    std::span<const std::int16_t> samples) {
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    if (!out.is_open()) {
        throw stuntmaster::core::Error{
            "unable to write audio capture: " + path.string()};
    }
    const auto put32 = [&out](std::uint32_t value) {
        const std::array<char, 4U> bytes{
            static_cast<char>(value & 0xFFU),
            static_cast<char>((value >> 8U) & 0xFFU),
            static_cast<char>((value >> 16U) & 0xFFU),
            static_cast<char>((value >> 24U) & 0xFFU)};
        out.write(bytes.data(), bytes.size());
    };
    const auto put16 = [&out](std::uint16_t value) {
        const std::array<char, 2U> bytes{
            static_cast<char>(value & 0xFFU),
            static_cast<char>((value >> 8U) & 0xFFU)};
        out.write(bytes.data(), bytes.size());
    };
    constexpr std::uint16_t channels = 2U;
    constexpr std::uint32_t rate = 44100U;
    constexpr std::uint16_t bits = 16U;
    const auto data_bytes =
        static_cast<std::uint32_t>(samples.size() * sizeof(std::int16_t));
    out.write("RIFF", 4);
    put32(36U + data_bytes);
    out.write("WAVEfmt ", 8);
    put32(16U);
    put16(1U); // PCM
    put16(channels);
    put32(rate);
    put32(rate * channels * bits / 8U);
    put16(channels * bits / 8U);
    put16(bits);
    out.write("data", 4);
    put32(data_bytes);
    for (const auto sample : samples) {
        put16(static_cast<std::uint16_t>(sample));
    }
}

void writeCapturedGpuFrame(
    const RetainedGpuFrame& frame,
    std::span<const std::uint16_t> vram,
    const std::filesystem::path& packet_destination) {
    std::ofstream packet_output{
        packet_destination, std::ios::binary | std::ios::trunc};
    if (!packet_output.is_open()) {
        throw stuntmaster::core::Error{
            "unable to write " + packet_destination.string()};
    }
    const auto write_word = [&packet_output](std::uint32_t word) {
        packet_output.write(
            reinterpret_cast<const char*>(&word), sizeof(word));
    };
    write_word(0x534D4750U);
    write_word(frame.display_x);
    write_word(frame.display_y);
    write_word(frame.display_width);
    write_word(frame.display_height);
    write_word(static_cast<std::uint32_t>(frame.packets.size()));
    for (const auto& packet : frame.packets) {
        write_word(static_cast<std::uint32_t>(packet.size()));
        for (const auto word : packet) {
            write_word(word);
        }
    }

    auto vram_destination = packet_destination;
    vram_destination.replace_extension(".VRAM");
    std::ofstream vram_output{
        vram_destination, std::ios::binary | std::ios::trunc};
    if (!vram_output.is_open()) {
        throw stuntmaster::core::Error{
            "unable to write " + vram_destination.string()};
    }
    vram_output.write(
        reinterpret_cast<const char*>(vram.data()),
        static_cast<std::streamsize>(
            vram.size() * sizeof(std::uint16_t)));
}

std::vector<std::uint32_t> readWords(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    if (!input.is_open()) {
        throw stuntmaster::core::Error{
            "unable to open " + path.string()};
    }
    std::vector<std::uint32_t> words;
    std::uint32_t word = 0U;
    while (input.read(reinterpret_cast<char*>(&word), sizeof(word))) {
        words.push_back(word);
    }
    return words;
}

CapturedFrame loadCapturedFrame(const std::filesystem::path& packet_path) {
    const auto words = readWords(packet_path);
    constexpr std::uint32_t magic = 0x534D4750U;
    if (words.size() < 6U || words[0] != magic) {
        throw stuntmaster::core::Error{
            "not a captured GP0 dump: " + packet_path.string()};
    }
    CapturedFrame frame;
    frame.display_x = words[1];
    frame.display_y = words[2];
    frame.display_width = words[3];
    frame.display_height = words[4];
    const auto packet_count = words[5];
    std::size_t cursor = 6U;
    for (std::uint32_t index = 0U; index < packet_count; ++index) {
        if (cursor >= words.size()) {
            throw stuntmaster::core::Error{"truncated captured GP0 dump"};
        }
        const auto length = words[cursor++];
        if (length > words.size() - cursor) {
            throw stuntmaster::core::Error{"truncated captured GP0 packet"};
        }
        frame.packets.emplace_back(
            words.begin() + static_cast<std::ptrdiff_t>(cursor),
            words.begin() + static_cast<std::ptrdiff_t>(cursor + length));
        cursor += length;
    }

    auto vram_path = packet_path;
    vram_path.replace_extension(".VRAM");
    std::ifstream vram_input{vram_path, std::ios::binary};
    if (!vram_input.is_open()) {
        throw stuntmaster::core::Error{
            "unable to open the matching VRAM snapshot: " +
            vram_path.string()};
    }
    constexpr std::size_t vram_words = 1024U * 512U;
    frame.vram.resize(vram_words);
    vram_input.read(
        reinterpret_cast<char*>(frame.vram.data()),
        static_cast<std::streamsize>(vram_words * sizeof(std::uint16_t)));
    if (static_cast<std::size_t>(vram_input.gcount()) !=
        vram_words * sizeof(std::uint16_t)) {
        throw stuntmaster::core::Error{
            "short VRAM snapshot: " + vram_path.string()};
    }
    return frame;
}

void writeScanoutBitmap(
    std::span<const std::uint16_t> vram,
    std::uint32_t x,
    std::uint32_t y,
    std::uint32_t width,
    std::uint32_t height,
    const std::filesystem::path& destination,
    std::uint32_t stride) {
    const auto row_bytes = (width * 3U + 3U) & ~3U;
    const auto pixel_bytes = row_bytes * height;
    std::vector<unsigned char> header(54U, 0U);
    const auto put32 = [&header](std::size_t offset, std::uint32_t value) {
        header[offset] = static_cast<unsigned char>(value);
        header[offset + 1U] = static_cast<unsigned char>(value >> 8U);
        header[offset + 2U] = static_cast<unsigned char>(value >> 16U);
        header[offset + 3U] = static_cast<unsigned char>(value >> 24U);
    };
    header[0] = 'B';
    header[1] = 'M';
    put32(2U, 54U + pixel_bytes);
    put32(10U, 54U);
    put32(14U, 40U);
    put32(18U, width);
    put32(22U, height);
    header[26] = 1U;
    header[28] = 24U;
    put32(34U, pixel_bytes);

    std::vector<unsigned char> pixels(pixel_bytes, 0U);
    for (std::uint32_t row = 0U; row < height; ++row) {
        // BMP rows run bottom-up.
        const auto source_row = y + (height - 1U - row);
        auto* out = pixels.data() + static_cast<std::size_t>(row) * row_bytes;
        for (std::uint32_t column = 0U; column < width; ++column) {
            const auto index =
                static_cast<std::size_t>(source_row) * stride + x + column;
            const auto pixel = index < vram.size() ? vram[index] : 0U;
            out[column * 3U] =
                static_cast<unsigned char>(((pixel >> 10U) & 0x1FU) << 3U);
            out[column * 3U + 1U] =
                static_cast<unsigned char>(((pixel >> 5U) & 0x1FU) << 3U);
            out[column * 3U + 2U] =
                static_cast<unsigned char>((pixel & 0x1FU) << 3U);
        }
    }
    std::ofstream output{destination, std::ios::binary | std::ios::trunc};
    output.write(
        reinterpret_cast<const char*>(header.data()),
        static_cast<std::streamsize>(header.size()));
    output.write(
        reinterpret_cast<const char*>(pixels.data()),
        static_cast<std::streamsize>(pixels.size()));
}

} // namespace stuntmaster::app
