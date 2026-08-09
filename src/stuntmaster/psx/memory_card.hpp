#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace stuntmaster::psx {

// A raw, interoperable 128 KiB PlayStation memory-card image. The guest still
// authors the directory entries, file headers, checksums, and save payload;
// this class only supplies the persistent block device and the small amount of
// filesystem traversal performed by the original BIOS.
class MemoryCard final {
public:
    static constexpr std::size_t frame_size = 128U;
    static constexpr std::size_t frame_count = 1024U;
    static constexpr std::size_t image_size = frame_size * frame_count;
    static constexpr std::size_t block_size = 8U * 1024U;
    static constexpr std::size_t data_block_count = 15U;

    struct File {
        std::string name;
        std::uint32_t size{};
        std::uint32_t first_block{};
        std::vector<std::uint32_t> blocks;
        std::uint8_t attributes{};
    };

    // An empty path creates an in-memory card, which is useful for deterministic
    // tests. A path loads an existing raw image or creates a formatted one.
    explicit MemoryCard(std::filesystem::path path = {});

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }
    [[nodiscard]] std::span<const std::byte> image() const noexcept {
        return image_;
    }
    [[nodiscard]] bool readFrame(
        std::uint32_t frame, std::span<std::byte, frame_size> destination) const;
    [[nodiscard]] bool writeFrame(
        std::uint32_t frame, std::span<const std::byte, frame_size> source);

    [[nodiscard]] std::vector<File> files() const;
    [[nodiscard]] std::optional<File> findFile(std::string_view name) const;
    [[nodiscard]] bool createFile(std::string_view name, std::uint32_t blocks);
    [[nodiscard]] bool eraseFile(std::string_view name);
    [[nodiscard]] std::size_t readFile(
        const File& file, std::uint32_t offset, std::span<std::byte> destination) const;
    [[nodiscard]] std::size_t writeFile(
        const File& file, std::uint32_t offset, std::span<const std::byte> source);

private:
    void format();
    void flush() const;
    [[nodiscard]] std::span<std::byte, frame_size> frame(std::uint32_t index);
    [[nodiscard]] std::span<const std::byte, frame_size> frame(
        std::uint32_t index) const;
    void updateFrameChecksum(std::uint32_t index);

    std::filesystem::path path_;
    std::array<std::byte, image_size> image_{};
};

} // namespace stuntmaster::psx
