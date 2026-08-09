#include "stuntmaster/psx/memory_card.hpp"

#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace stuntmaster::psx {
namespace {

constexpr std::uint8_t free_directory_entry = 0xA0U;
constexpr std::uint8_t first_file_block = 0x51U;
constexpr std::uint8_t middle_file_block = 0x52U;
constexpr std::uint8_t last_file_block = 0x53U;
constexpr std::uint16_t end_of_chain = 0xFFFFU;

std::uint16_t readLe16(std::span<const std::byte> bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[offset])) |
        static_cast<std::uint16_t>(
            std::to_integer<std::uint8_t>(bytes[offset + 1U]) << 8U);
}

std::uint32_t readLe32(std::span<const std::byte> bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(readLe16(bytes, offset)) |
        (static_cast<std::uint32_t>(readLe16(bytes, offset + 2U)) << 16U);
}

void writeLe16(std::span<std::byte> bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::byte>(value);
    bytes[offset + 1U] = static_cast<std::byte>(value >> 8U);
}

void writeLe32(std::span<std::byte> bytes, std::size_t offset, std::uint32_t value) {
    writeLe16(bytes, offset, static_cast<std::uint16_t>(value));
    writeLe16(bytes, offset + 2U, static_cast<std::uint16_t>(value >> 16U));
}

bool activeDirectoryEntry(std::uint8_t state) noexcept {
    return state == first_file_block || state == middle_file_block ||
        state == last_file_block;
}

std::string directoryName(std::span<const std::byte> entry) {
    std::string result;
    for (std::size_t index = 0x0AU; index < 0x1EU; ++index) {
        const auto character = std::to_integer<std::uint8_t>(entry[index]);
        if (character == 0U) {
            break;
        }
        result.push_back(static_cast<char>(character));
    }
    return result;
}

} // namespace

MemoryCard::MemoryCard(std::filesystem::path path) : path_(std::move(path)) {
    if (path_.empty() || !std::filesystem::exists(path_)) {
        format();
        flush();
        return;
    }

    std::ifstream input{path_, std::ios::binary};
    input.read(reinterpret_cast<char*>(image_.data()), image_.size());
    if (!input || input.peek() != std::ifstream::traits_type::eof()) {
        throw std::runtime_error{
            "memory-card image must be exactly 131072 bytes: " + path_.string()};
    }
    if (image_[0] != std::byte{'M'} || image_[1] != std::byte{'C'}) {
        throw std::runtime_error{
            "memory-card image has no MC header: " + path_.string()};
    }
}

std::span<std::byte, MemoryCard::frame_size> MemoryCard::frame(
    std::uint32_t index) {
    return std::span<std::byte, frame_size>{
        image_.data() + index * frame_size, frame_size};
}

std::span<const std::byte, MemoryCard::frame_size> MemoryCard::frame(
    std::uint32_t index) const {
    return std::span<const std::byte, frame_size>{
        image_.data() + index * frame_size, frame_size};
}

void MemoryCard::updateFrameChecksum(std::uint32_t index) {
    auto bytes = frame(index);
    std::uint8_t checksum{};
    for (std::size_t offset = 0U; offset + 1U < frame_size; ++offset) {
        checksum ^= std::to_integer<std::uint8_t>(bytes[offset]);
    }
    bytes.back() = static_cast<std::byte>(checksum);
}

void MemoryCard::format() {
    image_.fill(std::byte{0});
    image_[0] = std::byte{'M'};
    image_[1] = std::byte{'C'};
    updateFrameChecksum(0U);
    for (std::uint32_t index = 1U; index <= data_block_count; ++index) {
        auto entry = frame(index);
        entry[0] = static_cast<std::byte>(free_directory_entry);
        writeLe16(entry, 8U, end_of_chain);
        updateFrameChecksum(index);
    }
    // The twenty broken-sector replacement entries begin erased.
    for (std::uint32_t index = 16U; index < 36U; ++index) {
        auto entry = frame(index);
        std::fill_n(entry.begin(), 4U, std::byte{0xFF});
        updateFrameChecksum(index);
    }
}

void MemoryCard::flush() const {
    if (path_.empty()) {
        return;
    }
    if (!path_.parent_path().empty()) {
        std::filesystem::create_directories(path_.parent_path());
    }
    std::ofstream output{path_, std::ios::binary | std::ios::trunc};
    output.write(reinterpret_cast<const char*>(image_.data()), image_.size());
    if (!output) {
        throw std::runtime_error{
            "unable to write memory-card image: " + path_.string()};
    }
}

bool MemoryCard::readFrame(
    std::uint32_t index, std::span<std::byte, frame_size> destination) const {
    if (index >= frame_count) {
        return false;
    }
    std::ranges::copy(frame(index), destination.begin());
    return true;
}

bool MemoryCard::writeFrame(
    std::uint32_t index, std::span<const std::byte, frame_size> source) {
    if (index >= frame_count) {
        return false;
    }
    std::ranges::copy(source, frame(index).begin());
    flush();
    return true;
}

std::vector<MemoryCard::File> MemoryCard::files() const {
    std::vector<File> result;
    std::array<bool, data_block_count + 1U> consumed{};
    for (std::uint32_t directory_index = 1U;
         directory_index <= data_block_count; ++directory_index) {
        const auto first = frame(directory_index);
        const auto state = std::to_integer<std::uint8_t>(first[0]);
        if (!activeDirectoryEntry(state) || consumed[directory_index] ||
            state == middle_file_block || state == last_file_block) {
            continue;
        }
        File file{
            directoryName(first), readLe32(first, 4U), directory_index, {}, state};
        auto block = directory_index;
        for (std::size_t depth = 0U; depth < data_block_count; ++depth) {
            if (block == 0U || block > data_block_count || consumed[block]) {
                file.blocks.clear();
                break;
            }
            consumed[block] = true;
            file.blocks.push_back(block);
            const auto next = readLe16(frame(block), 8U);
            if (next == end_of_chain) {
                break;
            }
            // Directory links are zero-based data-block numbers.
            block = static_cast<std::uint32_t>(next) + 1U;
        }
        if (!file.name.empty() && !file.blocks.empty()) {
            result.push_back(std::move(file));
        }
    }
    return result;
}

std::optional<MemoryCard::File> MemoryCard::findFile(std::string_view name) const {
    const auto entries = files();
    const auto found = std::ranges::find(entries, name, &File::name);
    return found == entries.end() ? std::nullopt : std::optional{*found};
}

bool MemoryCard::createFile(std::string_view name, std::uint32_t block_count) {
    if (name.empty() || name.size() > 20U || block_count == 0U ||
        block_count > data_block_count || findFile(name)) {
        return false;
    }
    std::vector<std::uint32_t> free_blocks;
    for (std::uint32_t index = 1U; index <= data_block_count; ++index) {
        if (std::to_integer<std::uint8_t>(frame(index)[0]) ==
            free_directory_entry) {
            free_blocks.push_back(index);
            if (free_blocks.size() == block_count) {
                break;
            }
        }
    }
    if (free_blocks.size() != block_count) {
        return false;
    }
    for (std::size_t chain_index = 0U; chain_index < free_blocks.size();
         ++chain_index) {
        const auto block = free_blocks[chain_index];
        auto entry = frame(block);
        std::ranges::fill(entry, std::byte{0});
        entry[0] = static_cast<std::byte>(
            chain_index == 0U ? first_file_block :
            chain_index + 1U == free_blocks.size() ? last_file_block :
                                                     middle_file_block);
        writeLe32(entry, 4U, block_count * block_size);
        writeLe16(
            entry, 8U,
            chain_index + 1U == free_blocks.size()
                ? end_of_chain
                : static_cast<std::uint16_t>(free_blocks[chain_index + 1U] - 1U));
        if (chain_index == 0U) {
            for (std::size_t index = 0U; index < name.size(); ++index) {
                entry[0x0AU + index] = static_cast<std::byte>(name[index]);
            }
        }
        updateFrameChecksum(block);
    }
    flush();
    return true;
}

bool MemoryCard::eraseFile(std::string_view name) {
    const auto file = findFile(name);
    if (!file) {
        return false;
    }
    for (const auto block : file->blocks) {
        auto entry = frame(block);
        std::ranges::fill(entry, std::byte{0});
        entry[0] = static_cast<std::byte>(free_directory_entry);
        writeLe16(entry, 8U, end_of_chain);
        updateFrameChecksum(block);
    }
    flush();
    return true;
}

std::size_t MemoryCard::readFile(
    const File& file, std::uint32_t offset,
    std::span<std::byte> destination) const {
    const auto capacity = file.blocks.size() * block_size;
    const auto logical_size = std::min<std::size_t>(file.size, capacity);
    if (offset >= logical_size) {
        return 0U;
    }
    const auto count = std::min(destination.size(), logical_size - offset);
    for (std::size_t index = 0U; index < count; ++index) {
        const auto position = static_cast<std::size_t>(offset) + index;
        const auto block = file.blocks[position / block_size];
        destination[index] = image_[block * block_size + position % block_size];
    }
    return count;
}

std::size_t MemoryCard::writeFile(
    const File& file, std::uint32_t offset, std::span<const std::byte> source) {
    const auto capacity = file.blocks.size() * block_size;
    if (offset >= capacity) {
        return 0U;
    }
    const auto count = std::min(source.size(), capacity - offset);
    for (std::size_t index = 0U; index < count; ++index) {
        const auto position = static_cast<std::size_t>(offset) + index;
        const auto block = file.blocks[position / block_size];
        image_[block * block_size + position % block_size] = source[index];
    }
    flush();
    return count;
}

} // namespace stuntmaster::psx
