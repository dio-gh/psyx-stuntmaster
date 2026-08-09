#include "stuntmaster/psx/bios_hle.hpp"

#include "stuntmaster/core/state_archive.hpp"

#include <array>
#include <algorithm>
#include <limits>
#include <string>
#include <utility>

namespace stuntmaster::psx {
namespace {

constexpr std::uint32_t a0_vector = 0xA0U;
constexpr std::uint32_t b0_vector = 0xB0U;
constexpr std::uint32_t c0_vector = 0xC0U;
constexpr std::uint32_t b0_table_address = 0x80001000U;
constexpr std::uint32_t c0_table_address = 0x80001400U;
constexpr std::uint32_t b0_thunk_base = 0x80002000U;
constexpr std::uint32_t c0_thunk_base = 0x80002400U;
constexpr std::uint32_t function_count = 256U;
constexpr std::uint32_t table_size = function_count * sizeof(std::uint32_t);
constexpr std::uint32_t event_descriptor_base = 0xF1000000U;
constexpr std::uint32_t card_software_event_class = 0xF4000001U;
constexpr std::uint32_t card_hardware_event_class = 0xF0000011U;
constexpr std::uint32_t card_event_success = 4U;
constexpr std::uint32_t card_event_error = 0x8000U;
// The kernel's device-control-block table. Retail links its own libapi
// `firstfile`, which walks this table for the "bu" device before it reaches the
// BIOS boundary, and returns a null directory entry without issuing the BIOS
// call at all when the device is absent. Its caller then waits forever for the
// hardware-card completion that only a BIOS call raises.
constexpr std::uint32_t device_table_pointer = 0x80000150U;
constexpr std::uint32_t device_table_size_pointer = 0x80000154U;
constexpr std::uint32_t device_table_address = 0x80002800U;
constexpr std::uint32_t device_control_block_size = 0x50U;
constexpr std::uint32_t device_name_address = 0x80002850U;
constexpr std::uint32_t device_description_address = 0x80002858U;

bool isVector(std::uint32_t pc) noexcept {
    return pc == a0_vector || pc == b0_vector || pc == c0_vector;
}

std::optional<std::size_t> eventIndex(std::uint32_t descriptor) noexcept {
    if ((descriptor & 0xFFFF0000U) != event_descriptor_base) {
        return std::nullopt;
    }
    const auto index = static_cast<std::size_t>(descriptor & 0xFFFFU);
    return index < 32U ? std::optional{index} : std::nullopt;
}

void writeLe32(std::span<std::byte> bytes, std::size_t offset, std::uint32_t value) {
    for (std::size_t index = 0U; index < 4U; ++index) {
        bytes[offset + index] = static_cast<std::byte>(value >> (index * 8U));
    }
}

std::optional<std::string> cardFilename(std::string_view path) {
    const auto colon = path.find(':');
    if (colon == std::string_view::npos) {
        return std::string{path};
    }
    const auto device = path.substr(0U, colon);
    if (device != "bu00" && device != "bu10") {
        return std::nullopt;
    }
    // This first persistence milestone exposes one card in slot one. Treat
    // slot two as disconnected instead of aliasing the same save image.
    if (device == "bu10") {
        return std::nullopt;
    }
    return std::string{path.substr(colon + 1U)};
}

bool wildcardMatch(std::string_view pattern, std::string_view value) {
    const auto wildcard = pattern.find('*');
    if (wildcard == std::string_view::npos) {
        return pattern == value;
    }
    const auto prefix = pattern.substr(0U, wildcard);
    const auto suffix = pattern.substr(wildcard + 1U);
    return value.size() >= prefix.size() + suffix.size() &&
        value.starts_with(prefix) && value.ends_with(suffix);
}

} // namespace

std::span<const std::uint32_t> BiosHle::executionBoundaries() noexcept {
    static constexpr auto boundaries = [] {
        std::array<std::uint32_t, function_count * 2U + 3U> result{};
        result[0] = a0_vector;
        result[1] = b0_vector;
        result[2] = c0_vector;
        for (std::uint32_t index = 0U; index < function_count; ++index) {
            result[3U + index] =
                b0_thunk_base + index * sizeof(std::uint32_t);
            result[3U + function_count + index] =
                c0_thunk_base + index * sizeof(std::uint32_t);
        }
        return result;
    }();
    return boundaries;
}

BiosHle::BiosHle(
    TtySink tty_sink, GpuCommandSink gpu_sink, MemoryCard* memory_card)
    : tty_sink_(std::move(tty_sink)), gpu_sink_(std::move(gpu_sink)),
      memory_card_(memory_card) {}

void BiosHle::writeState(core::StateWriter& writer) const {
    writer.pod(entry_interrupt_hook_);
    writer.pod(clear_pad_);
    writer.pod(last_gpu_command_);
    writer.pod(gpu_status_);
    writer.pod(gpu_command_count_);
    writer.pod(last_gpu_command_was_gp1_);
    writer.pod(memory_card_device_installed_);
    writer.pod(clear_root_counter_);
    writer.pod(interrupt_priority_heads_);
    writer.pod(events_);
    for (const auto& file : open_files_) {
        writer.string(file.name);
        writer.pod(file.position);
        writer.pod(file.allocated);
    }
    writer.pod(static_cast<std::uint64_t>(directory_search_.size()));
    for (const auto& file : directory_search_) {
        writer.string(file.name);
        writer.pod(file.size);
        writer.pod(file.first_block);
        writer.vectorPod(file.blocks);
        writer.pod(file.attributes);
    }
    writer.pod(static_cast<std::uint64_t>(directory_search_index_));
    writer.pod(static_cast<std::uint64_t>(pending_event_callbacks_.size()));
    for (const auto callback : pending_event_callbacks_) {
        writer.pod(callback);
    }
}

bool BiosHle::readState(core::StateReader& reader) {
    if (!reader.pod(entry_interrupt_hook_) || !reader.pod(clear_pad_) ||
        !reader.pod(last_gpu_command_) || !reader.pod(gpu_status_) ||
        !reader.pod(gpu_command_count_) ||
        !reader.pod(last_gpu_command_was_gp1_) ||
        !reader.pod(memory_card_device_installed_) ||
        !reader.pod(clear_root_counter_) ||
        !reader.pod(interrupt_priority_heads_) || !reader.pod(events_)) {
        return false;
    }
    for (auto& file : open_files_) {
        if (!reader.string(file.name, 256U) || !reader.pod(file.position) ||
            !reader.pod(file.allocated)) {
            return false;
        }
    }
    std::uint64_t directory_count{};
    if (!reader.pod(directory_count) ||
        directory_count > MemoryCard::data_block_count) {
        return false;
    }
    directory_search_.clear();
    directory_search_.resize(static_cast<std::size_t>(directory_count));
    for (auto& file : directory_search_) {
        if (!reader.string(file.name, 256U) || !reader.pod(file.size) ||
            !reader.pod(file.first_block) ||
            !reader.vectorPod(file.blocks, MemoryCard::data_block_count) ||
            !reader.pod(file.attributes)) {
            return false;
        }
    }
    std::uint64_t directory_index{};
    std::uint64_t pending_count{};
    if (!reader.pod(directory_index) ||
        directory_index > directory_search_.size() ||
        !reader.pod(pending_count) || pending_count > 4096U) {
        return false;
    }
    directory_search_index_ = static_cast<std::size_t>(directory_index);
    pending_event_callbacks_.clear();
    for (std::uint64_t index = 0U; index < pending_count; ++index) {
        std::uint32_t callback{};
        if (!reader.pod(callback)) {
            return false;
        }
        pending_event_callbacks_.push_back(callback);
    }
    return true;
}

void BiosHle::complete(R3000Runtime& runtime, std::uint32_t return_value) const {
    runtime.setRegister(2, return_value);
    runtime.completeHostCall();
}

bool BiosHle::writeTtyString(const R3000Runtime& runtime,
                             std::uint32_t address) const {
    std::string text;
    for (std::size_t index = 0; index < 4096; ++index) {
        std::uint8_t character{};
        if (!runtime.read8(address + static_cast<std::uint32_t>(index), character)) {
            return false;
        }
        if (character == 0) {
            if (tty_sink_) {
                tty_sink_(text);
            }
            return true;
        }
        text.push_back(character >= 0x20 && character < 0x7F
                           ? static_cast<char>(character)
                           : '.');
    }
    return false;
}

bool BiosHle::writeSetjmpBuffer(R3000Runtime& runtime,
                                std::uint32_t destination) const {
    constexpr std::array registers{
        31U, 29U, 30U, 16U, 17U, 18U,
        19U, 20U, 21U, 22U, 23U, 28U,
    };
    for (std::size_t index = 0; index < registers.size(); ++index) {
        if (!runtime.write32(
                destination + static_cast<std::uint32_t>(index * sizeof(std::uint32_t)),
                runtime.state().gpr[registers[index]])) {
            return false;
        }
    }
    return true;
}

bool BiosHle::emitGpuCommand(
    R3000Runtime& runtime, bool gp1, std::uint32_t command) {
    constexpr std::uint32_t gp0_register = 0x1F801810U;
    constexpr std::uint32_t gp1_register = 0x1F801814U;
    if (!runtime.write32(gp1 ? gp1_register : gp0_register, command)) {
        return false;
    }
    if (gp1 && !runtime.write32(gp1_register, gpu_status_)) {
        return false;
    }
    last_gpu_command_ = command;
    last_gpu_command_was_gp1_ = gp1;
    ++gpu_command_count_;
    if (gpu_sink_) {
        gpu_sink_(gp1, command);
    }
    return true;
}

bool BiosHle::writeFunctionTable(
    R3000Runtime& runtime,
    std::uint32_t table_address,
    std::uint32_t thunk_base) const {
    for (std::uint32_t index = 0; index < function_count; ++index) {
        if (!runtime.write32(
                table_address + index * sizeof(std::uint32_t),
                thunk_base + index * sizeof(std::uint32_t))) {
            return false;
        }
    }
    return true;
}

std::optional<std::uint32_t> BiosHle::eventCallback(
    std::uint32_t event_class,
    std::uint32_t event_spec) const noexcept {
    for (const auto& event : events_) {
        if (event.allocated && event.enabled &&
            event.event_class == event_class &&
            event.event_spec == event_spec && event.mode == 0x1000U &&
            event.callback != 0U) {
            return event.callback;
        }
    }
    return std::nullopt;
}

std::optional<std::uint32_t> BiosHle::takePendingEventCallback() noexcept {
    if (pending_event_callbacks_.empty()) {
        return std::nullopt;
    }
    const auto callback = pending_event_callbacks_.front();
    pending_event_callbacks_.pop_front();
    return callback;
}

bool BiosHle::readGuestString(
    const R3000Runtime& runtime, std::uint32_t address, std::string& text) const {
    text.clear();
    for (std::size_t index = 0U; index < 256U; ++index) {
        std::uint8_t character{};
        if (!runtime.read8(address + static_cast<std::uint32_t>(index), character)) {
            return false;
        }
        if (character == 0U) {
            return true;
        }
        text.push_back(static_cast<char>(character));
    }
    return false;
}

bool BiosHle::installMemoryCardDevice(R3000Runtime& runtime) {
    if (memory_card_device_installed_) {
        return true;
    }
    const auto writeString = [&](std::uint32_t address, std::string_view text) {
        for (std::size_t index = 0U; index <= text.size(); ++index) {
            const auto character = index < text.size()
                ? static_cast<std::uint8_t>(text[index])
                : std::uint8_t{0U};
            if (!runtime.write8(
                    address + static_cast<std::uint32_t>(index), character)) {
                return false;
            }
        }
        return true;
    };
    if (!writeString(device_name_address, "bu") ||
        !writeString(device_description_address, "MEMORY CARD")) {
        return false;
    }
    for (std::uint32_t offset = 0U; offset < device_control_block_size;
         offset += sizeof(std::uint32_t)) {
        if (!runtime.write32(device_table_address + offset, 0U)) {
            return false;
        }
    }
    // Only the name, flags, block size, and description are meaningful here.
    // Every BIOS service still enters through the A0/B0 vectors, so the driver
    // entry points stay null; retail's wrapper saves the `firstfile` slot and
    // overwrites it with its own patch, but nothing on this host ever calls
    // through it.
    const bool described =
        runtime.write32(device_table_address + 0x00U, device_name_address) &&
        runtime.write32(device_table_address + 0x04U, 1U) &&
        runtime.write32(
            device_table_address + 0x08U,
            static_cast<std::uint32_t>(MemoryCard::frame_size)) &&
        runtime.write32(
            device_table_address + 0x0CU, device_description_address);
    if (!described) {
        return false;
    }
    if (!runtime.write32(device_table_pointer, device_table_address) ||
        !runtime.write32(device_table_size_pointer, device_control_block_size)) {
        return false;
    }
    memory_card_device_installed_ = true;
    return true;
}

void BiosHle::queueEvent(std::uint32_t event_class, std::uint32_t event_spec) {
    for (auto& event : events_) {
        if (!event.allocated || !event.enabled ||
            event.event_class != event_class || event.event_spec != event_spec) {
            continue;
        }
        if (event.mode == 0x1000U && event.callback != 0U) {
            pending_event_callbacks_.push_back(event.callback);
        } else if (event.mode == 0x2000U) {
            event.ready = true;
        }
    }
}

std::optional<BiosHleStatus> BiosHle::dispatchMemoryCard(
    R3000Runtime& runtime, std::uint32_t vector, std::uint32_t function) {
    constexpr std::uint32_t a0_card_info = 0xABU;
    constexpr std::uint32_t a0_card_load = 0xACU;
    constexpr std::uint32_t b0_open = 0x32U;
    constexpr std::uint32_t b0_lseek = 0x33U;
    constexpr std::uint32_t b0_read = 0x34U;
    constexpr std::uint32_t b0_write = 0x35U;
    constexpr std::uint32_t b0_close = 0x36U;
    constexpr std::uint32_t b0_firstfile = 0x42U;
    constexpr std::uint32_t b0_nextfile = 0x43U;
    constexpr std::uint32_t b0_erase = 0x45U;
    constexpr std::uint32_t b0_card_write = 0x4EU;
    constexpr std::uint32_t b0_card_read = 0x4FU;
    constexpr std::uint32_t b0_new_card = 0x50U;

    const auto is_card_call =
        (vector == a0_vector &&
         (function == a0_card_info || function == a0_card_load)) ||
        (vector == b0_vector &&
         (function == b0_open || function == b0_close ||
          function == b0_lseek ||
          function == b0_read || function == b0_write ||
          function == b0_firstfile || function == b0_nextfile ||
          function == b0_erase || function == b0_card_write ||
          function == b0_card_read || function == b0_new_card));
    if (!is_card_call) {
        return std::nullopt;
    }
    if (!installMemoryCardDevice(runtime)) {
        return BiosHleStatus::memory_fault;
    }

    if (vector == b0_vector && function == b0_open) {
        std::string path;
        if (!readGuestString(runtime, runtime.state().gpr[4], path)) {
            return BiosHleStatus::memory_fault;
        }
        const auto name = cardFilename(path);
        if (!memory_card_ || !name) {
            complete(runtime, std::numeric_limits<std::uint32_t>::max());
            return BiosHleStatus::handled;
        }
        const auto mode = runtime.state().gpr[5];
        if (!memory_card_->findFile(*name) && (mode & 0x200U) != 0U) {
            auto blocks = mode >> 16U;
            if (blocks == 0U) {
                blocks = 1U;
            }
            static_cast<void>(memory_card_->createFile(*name, blocks));
        }
        if (!memory_card_->findFile(*name)) {
            complete(runtime, std::numeric_limits<std::uint32_t>::max());
            return BiosHleStatus::handled;
        }
        for (std::size_t index = 0U; index < open_files_.size(); ++index) {
            if (!open_files_[index].allocated) {
                open_files_[index] = OpenFile{*name, 0U, true};
                complete(runtime, static_cast<std::uint32_t>(index + 2U));
                return BiosHleStatus::handled;
            }
        }
        complete(runtime, std::numeric_limits<std::uint32_t>::max());
        return BiosHleStatus::handled;
    }

    if (vector == b0_vector && function == b0_close) {
        const auto descriptor = runtime.state().gpr[4];
        if (descriptor >= 2U && descriptor - 2U < open_files_.size()) {
            open_files_[descriptor - 2U] = {};
            complete(runtime, 0U);
        } else {
            complete(runtime, std::numeric_limits<std::uint32_t>::max());
        }
        return BiosHleStatus::handled;
    }

    if (vector == b0_vector && function == b0_lseek) {
        const auto descriptor = runtime.state().gpr[4];
        if (descriptor < 2U || descriptor - 2U >= open_files_.size() ||
            !open_files_[descriptor - 2U].allocated || !memory_card_) {
            complete(runtime, std::numeric_limits<std::uint32_t>::max());
            return BiosHleStatus::handled;
        }
        auto& open = open_files_[descriptor - 2U];
        const auto file = memory_card_->findFile(open.name);
        if (!file) {
            complete(runtime, std::numeric_limits<std::uint32_t>::max());
            return BiosHleStatus::handled;
        }
        const auto offset = static_cast<std::int32_t>(runtime.state().gpr[5]);
        const auto whence = runtime.state().gpr[6];
        std::int64_t base{};
        if (whence == 1U) {
            base = open.position;
        } else if (whence == 2U) {
            base = file->size;
        } else if (whence != 0U) {
            complete(runtime, std::numeric_limits<std::uint32_t>::max());
            return BiosHleStatus::handled;
        }
        const auto position = base + offset;
        if (position < 0 || position > std::numeric_limits<std::uint32_t>::max()) {
            complete(runtime, std::numeric_limits<std::uint32_t>::max());
            return BiosHleStatus::handled;
        }
        open.position = static_cast<std::uint32_t>(position);
        complete(runtime, open.position);
        return BiosHleStatus::handled;
    }

    if (vector == b0_vector &&
        (function == b0_read || function == b0_write)) {
        const auto descriptor = runtime.state().gpr[4];
        const auto address = runtime.state().gpr[5];
        const auto requested = runtime.state().gpr[6];
        if (descriptor < 2U || descriptor - 2U >= open_files_.size() ||
            !open_files_[descriptor - 2U].allocated || !memory_card_) {
            queueEvent(card_software_event_class, card_event_error);
            complete(runtime, std::numeric_limits<std::uint32_t>::max());
            return BiosHleStatus::handled;
        }
        auto& open = open_files_[descriptor - 2U];
        const auto file = memory_card_->findFile(open.name);
        if (!file) {
            queueEvent(card_software_event_class, card_event_error);
            complete(runtime, std::numeric_limits<std::uint32_t>::max());
            return BiosHleStatus::handled;
        }
        std::vector<std::byte> bytes(requested);
        std::size_t transferred{};
        if (function == b0_read) {
            transferred = memory_card_->readFile(*file, open.position, bytes);
            if (!runtime.loadBytes(address, std::span{bytes}.first(transferred))) {
                return BiosHleStatus::memory_fault;
            }
        } else {
            if (!runtime.copyBytes(address, bytes)) {
                return BiosHleStatus::memory_fault;
            }
            transferred = memory_card_->writeFile(*file, open.position, bytes);
        }
        open.position += static_cast<std::uint32_t>(transferred);
        queueEvent(card_software_event_class, card_event_success);
        // The retail libmcrd state machines retry read/write until the BIOS
        // returns zero, which means the asynchronous operation was accepted.
        // Completion (and the transfer result) arrives through the software
        // card event; returning the byte count makes retail issue the same
        // operation repeatedly without advancing its state.
        complete(runtime, 0U);
        return BiosHleStatus::handled;
    }

    const auto write_directory_entry = [&](const MemoryCard::File& file,
                                           std::uint32_t destination) {
        std::array<std::byte, 40U> entry{};
        for (std::size_t index = 0U;
             index < std::min<std::size_t>(20U, file.name.size()); ++index) {
            entry[index] = static_cast<std::byte>(file.name[index]);
        }
        writeLe32(entry, 20U, file.attributes);
        writeLe32(entry, 24U, file.size);
        writeLe32(entry, 32U, file.first_block);
        return runtime.loadBytes(destination, entry);
    };

    if (vector == b0_vector && function == b0_firstfile) {
        std::string path;
        if (!readGuestString(runtime, runtime.state().gpr[4], path)) {
            return BiosHleStatus::memory_fault;
        }
        directory_search_.clear();
        directory_search_index_ = 0U;
        const auto pattern = cardFilename(path);
        if (memory_card_ && pattern) {
            for (const auto& file : memory_card_->files()) {
                if (wildcardMatch(*pattern, file.name)) {
                    directory_search_.push_back(file);
                }
            }
        }
        const auto destination = runtime.state().gpr[5];
        if (directory_search_.empty()) {
            // Psy-Q's firstfile implementation waits synchronously on the raw
            // card event when the directory scan finds no match. Without this
            // completion an empty, newly formatted card traps Save/Load in
            // _get_card_event_x forever.
            queueEvent(card_hardware_event_class, card_event_success);
            complete(runtime, 0U);
        } else if (!write_directory_entry(directory_search_[0], destination)) {
            return BiosHleStatus::memory_fault;
        } else {
            directory_search_index_ = 1U;
            complete(runtime, destination);
        }
        return BiosHleStatus::handled;
    }

    if (vector == b0_vector && function == b0_nextfile) {
        const auto destination = runtime.state().gpr[4];
        if (directory_search_index_ >= directory_search_.size()) {
            complete(runtime, 0U);
        } else if (!write_directory_entry(
                       directory_search_[directory_search_index_++], destination)) {
            return BiosHleStatus::memory_fault;
        } else {
            complete(runtime, destination);
        }
        return BiosHleStatus::handled;
    }

    if (vector == b0_vector && function == b0_erase) {
        std::string path;
        if (!readGuestString(runtime, runtime.state().gpr[4], path)) {
            return BiosHleStatus::memory_fault;
        }
        const auto name = cardFilename(path);
        const auto erased = memory_card_ && name && memory_card_->eraseFile(*name);
        queueEvent(
            card_software_event_class,
            erased ? card_event_success : card_event_error);
        complete(
            runtime,
            erased ? 1U : std::numeric_limits<std::uint32_t>::max());
        return BiosHleStatus::handled;
    }

    if (vector == b0_vector &&
        (function == b0_card_read || function == b0_card_write)) {
        const auto slot_two = (runtime.state().gpr[4] & 0x10U) != 0U;
        if (!memory_card_ || slot_two) {
            queueEvent(card_hardware_event_class, card_event_error);
            complete(runtime, 0U);
            return BiosHleStatus::handled;
        }
        const auto card_frame = runtime.state().gpr[5];
        const auto address = runtime.state().gpr[6];
        std::array<std::byte, MemoryCard::frame_size> bytes{};
        bool transferred{};
        if (function == b0_card_read) {
            transferred = memory_card_->readFrame(card_frame, bytes);
            if (transferred && !runtime.loadBytes(address, bytes)) {
                return BiosHleStatus::memory_fault;
            }
        } else {
            if (!runtime.copyBytes(address, bytes)) {
                return BiosHleStatus::memory_fault;
            }
            transferred = memory_card_->writeFrame(card_frame, bytes);
        }
        queueEvent(
            card_hardware_event_class,
            transferred ? card_event_success : card_event_error);
        complete(runtime, transferred ? 1U : 0U);
        return BiosHleStatus::handled;
    }

    if (vector == a0_vector &&
        (function == a0_card_info || function == a0_card_load)) {
        const auto connected = memory_card_ != nullptr &&
            (runtime.state().gpr[4] & 0x10U) == 0U;
        queueEvent(
            card_software_event_class,
            connected ? card_event_success : card_event_error);
        complete(runtime, connected ? 1U : 0U);
        return BiosHleStatus::handled;
    }

    if (vector == b0_vector && function == b0_new_card) {
        // Clears the BIOS "new card" latch; it is a query/reset operation and
        // does not itself raise another completion event.
        const auto connected = memory_card_ != nullptr &&
            (runtime.state().gpr[4] & 0x10U) == 0U;
        complete(runtime, connected ? 1U : 0U);
        return BiosHleStatus::handled;
    }

    return BiosHleStatus::unsupported;
}

BiosHleResult BiosHle::dispatch(R3000Runtime& runtime) {
    auto vector = runtime.state().pc;
    auto function = runtime.state().gpr[9];
    if (vector >= b0_thunk_base && vector < b0_thunk_base + table_size &&
        (vector & 3U) == 0U) {
        function = (vector - b0_thunk_base) / sizeof(std::uint32_t);
        vector = b0_vector;
    } else if (
        vector >= c0_thunk_base && vector < c0_thunk_base + table_size &&
        (vector & 3U) == 0U) {
        function = (vector - c0_thunk_base) / sizeof(std::uint32_t);
        vector = c0_vector;
    }
    if (!isVector(vector)) {
        return {};
    }
    const auto result = [vector, function](BiosHleStatus status) {
        return BiosHleResult{status, vector, function};
    };

    if (const auto card_result = dispatchMemoryCard(runtime, vector, function)) {
        return result(*card_result);
    }

    constexpr std::uint32_t a0_atoi = 0x10U;
    constexpr std::uint32_t a0_atol = 0x11U;
    constexpr std::uint32_t a0_setjmp = 0x13U;
    constexpr std::uint32_t a0_strcat = 0x15U;
    constexpr std::uint32_t a0_strcmp = 0x17U;
    constexpr std::uint32_t a0_strcpy = 0x19U;
    constexpr std::uint32_t a0_strncpy = 0x1AU;
    constexpr std::uint32_t a0_strlen = 0x1BU;
    constexpr std::uint32_t a0_strchr = 0x1EU;
    constexpr std::uint32_t a0_bzero = 0x28U;
    constexpr std::uint32_t a0_puts = 0x3EU;
    constexpr std::uint32_t a0_printf = 0x3FU;
    constexpr std::uint32_t a0_flush_cache = 0x44U;
    constexpr std::uint32_t a0_send_gp1_command = 0x48U;
    constexpr std::uint32_t a0_gpu_cw = 0x49U;
    constexpr std::uint32_t a0_gpu_cwp = 0x4AU;
    constexpr std::uint32_t a0_bu_init_old = 0x55U;
    constexpr std::uint32_t a0_bu_init = 0x70U;
    constexpr std::uint32_t a0_remove_cdrom = 0x72U;
    constexpr std::uint32_t b0_hook_entry_int = 0x19U;
    constexpr std::uint32_t b0_deliver_event = 0x07U;
    constexpr std::uint32_t b0_open_event = 0x08U;
    constexpr std::uint32_t b0_close_event = 0x09U;
    constexpr std::uint32_t b0_wait_event = 0x0AU;
    constexpr std::uint32_t b0_test_event = 0x0BU;
    constexpr std::uint32_t b0_enable_event = 0x0CU;
    constexpr std::uint32_t b0_disable_event = 0x0DU;
    constexpr std::uint32_t b0_puts = 0x3FU;
    constexpr std::uint32_t b0_init_card = 0x4AU;
    constexpr std::uint32_t b0_start_card = 0x4BU;
    constexpr std::uint32_t b0_stop_card = 0x4CU;
    constexpr std::uint32_t b0_get_c0_table = 0x56U;
    constexpr std::uint32_t b0_get_b0_table = 0x57U;
    constexpr std::uint32_t b0_change_clear_pad = 0x5BU;
    constexpr std::uint32_t c0_change_clear_root_counter = 0x0AU;
    constexpr std::uint32_t c0_enqueue_interrupt = 0x02U;
    constexpr std::uint32_t c0_dequeue_interrupt = 0x03U;

    if (vector == a0_vector &&
        (function == a0_atoi || function == a0_atol)) {
        constexpr std::uint32_t maximum_string_length =
            R3000Runtime::ram_size;
        const auto source = runtime.state().gpr[4];
        std::uint32_t index = 0U;
        std::uint8_t character{};
        for (; index < maximum_string_length; ++index) {
            if (!runtime.read8(source + index, character)) {
                return result(BiosHleStatus::memory_fault);
            }
            const bool is_whitespace =
                character == ' ' || character == '\t' ||
                character == '\n' || character == '\v' ||
                character == '\f' || character == '\r';
            if (!is_whitespace) {
                break;
            }
        }
        if (index == maximum_string_length) {
            return result(BiosHleStatus::memory_fault);
        }

        bool negative = false;
        if (character == '-' || character == '+') {
            negative = character == '-';
            ++index;
        }

        std::uint32_t value = 0U;
        for (; index < maximum_string_length; ++index) {
            if (!runtime.read8(source + index, character)) {
                return result(BiosHleStatus::memory_fault);
            }
            if (character < '0' || character > '9') {
                complete(runtime, negative ? 0U - value : value);
                return result(BiosHleStatus::handled);
            }
            value = value * 10U + static_cast<std::uint32_t>(character - '0');
        }
        return result(BiosHleStatus::memory_fault);
    }
    if (vector == a0_vector && function == a0_setjmp) {
        if (!writeSetjmpBuffer(runtime, runtime.state().gpr[4])) {
            return result(BiosHleStatus::memory_fault);
        }
        complete(runtime, 0);
        return result(BiosHleStatus::handled);
    }
    if (vector == a0_vector && function == a0_strcmp) {
        constexpr std::uint32_t maximum_string_length =
            R3000Runtime::ram_size;
        const auto left = runtime.state().gpr[4];
        const auto right = runtime.state().gpr[5];
        for (std::uint32_t index = 0U; index < maximum_string_length;
             ++index) {
            std::uint8_t left_character{};
            std::uint8_t right_character{};
            if (!runtime.read8(left + index, left_character) ||
                !runtime.read8(right + index, right_character)) {
                return result(BiosHleStatus::memory_fault);
            }
            if (left_character != right_character ||
                left_character == 0U) {
                const auto comparison =
                    static_cast<std::int32_t>(left_character) -
                    static_cast<std::int32_t>(right_character);
                complete(runtime, static_cast<std::uint32_t>(comparison));
                return result(BiosHleStatus::handled);
            }
        }
        return result(BiosHleStatus::memory_fault);
    }
    if (vector == a0_vector && function == a0_strcat) {
        // Retail builds every memory-card path with this call: the device
        // prefix is formatted first, then the filename is appended.
        constexpr std::uint32_t maximum_string_length =
            R3000Runtime::ram_size;
        const auto destination = runtime.state().gpr[4];
        const auto source = runtime.state().gpr[5];
        std::uint32_t length = 0U;
        for (; length < maximum_string_length; ++length) {
            std::uint8_t character{};
            if (!runtime.read8(destination + length, character)) {
                return result(BiosHleStatus::memory_fault);
            }
            if (character == 0U) {
                break;
            }
        }
        if (length == maximum_string_length) {
            return result(BiosHleStatus::memory_fault);
        }
        for (std::uint32_t index = 0U; index < maximum_string_length;
             ++index) {
            std::uint8_t character{};
            if (!runtime.read8(source + index, character) ||
                !runtime.write8(destination + length + index, character)) {
                return result(BiosHleStatus::memory_fault);
            }
            if (character == 0U) {
                complete(runtime, destination);
                return result(BiosHleStatus::handled);
            }
        }
        return result(BiosHleStatus::memory_fault);
    }
    if (vector == a0_vector && function == a0_strcpy) {
        constexpr std::uint32_t maximum_string_length =
            R3000Runtime::ram_size;
        const auto destination = runtime.state().gpr[4];
        const auto source = runtime.state().gpr[5];
        for (std::uint32_t index = 0U; index < maximum_string_length;
             ++index) {
            std::uint8_t character{};
            if (!runtime.read8(source + index, character) ||
                !runtime.write8(destination + index, character)) {
                return result(BiosHleStatus::memory_fault);
            }
            if (character == 0U) {
                complete(runtime, destination);
                return result(BiosHleStatus::handled);
            }
        }
        return result(BiosHleStatus::memory_fault);
    }
    if (vector == a0_vector && function == a0_strncpy) {
        const auto destination = runtime.state().gpr[4];
        const auto source = runtime.state().gpr[5];
        const auto length = runtime.state().gpr[6];
        bool reached_terminator = false;
        for (std::uint32_t index = 0; index < length; ++index) {
            std::uint8_t character{};
            if (!reached_terminator &&
                !runtime.read8(source + index, character)) {
                return result(BiosHleStatus::memory_fault);
            }
            if (character == 0U) {
                reached_terminator = true;
            }
            if (!runtime.write8(
                    destination + index,
                    reached_terminator ? 0U : character)) {
                return result(BiosHleStatus::memory_fault);
            }
        }
        complete(runtime, destination);
        return result(BiosHleStatus::handled);
    }
    if (vector == a0_vector && function == a0_strlen) {
        constexpr std::uint32_t maximum_string_length =
            R3000Runtime::ram_size;
        const auto source = runtime.state().gpr[4];
        for (std::uint32_t length = 0U; length < maximum_string_length;
             ++length) {
            std::uint8_t character{};
            if (!runtime.read8(source + length, character)) {
                return result(BiosHleStatus::memory_fault);
            }
            if (character == 0U) {
                complete(runtime, length);
                return result(BiosHleStatus::handled);
            }
        }
        return result(BiosHleStatus::memory_fault);
    }
    if (vector == a0_vector && function == a0_strchr) {
        constexpr std::uint32_t maximum_string_length =
            R3000Runtime::ram_size;
        const auto source = runtime.state().gpr[4];
        const auto target =
            static_cast<std::uint8_t>(runtime.state().gpr[5]);
        for (std::uint32_t index = 0U; index < maximum_string_length;
             ++index) {
            std::uint8_t character{};
            if (!runtime.read8(source + index, character)) {
                return result(BiosHleStatus::memory_fault);
            }
            if (character == target) {
                complete(runtime, source + index);
                return result(BiosHleStatus::handled);
            }
            if (character == 0U) {
                complete(runtime, 0U);
                return result(BiosHleStatus::handled);
            }
        }
        return result(BiosHleStatus::memory_fault);
    }
    if (vector == a0_vector && function == a0_bzero) {
        const auto destination = runtime.state().gpr[4];
        const auto length = runtime.state().gpr[5];
        for (std::uint32_t index = 0; index < length; ++index) {
            if (!runtime.write8(destination + index, 0U)) {
                return result(BiosHleStatus::memory_fault);
            }
        }
        complete(runtime, destination);
        return result(BiosHleStatus::handled);
    }
    if (vector == a0_vector && function == a0_remove_cdrom) {
        complete(runtime, 0);
        return result(BiosHleStatus::handled);
    }
    if (vector == a0_vector && function == a0_flush_cache) {
        // The interpreter fetches directly from coherent guest RAM.
        runtime.completeHostCall();
        return result(BiosHleStatus::handled);
    }
    if (vector == a0_vector &&
        (function == a0_bu_init_old || function == a0_bu_init)) {
        if (!installMemoryCardDevice(runtime)) {
            return result(BiosHleStatus::memory_fault);
        }
        runtime.completeHostCall();
        return result(BiosHleStatus::handled);
    }
    if (vector == a0_vector && function == a0_send_gp1_command) {
        if (!emitGpuCommand(runtime, true, runtime.state().gpr[4])) {
            return result(BiosHleStatus::memory_fault);
        }
        runtime.completeHostCall();
        return result(BiosHleStatus::handled);
    }
    if (vector == a0_vector && function == a0_gpu_cw) {
        if (!emitGpuCommand(runtime, false, runtime.state().gpr[4])) {
            return result(BiosHleStatus::memory_fault);
        }
        complete(runtime, 0U);
        return result(BiosHleStatus::handled);
    }
    if (vector == a0_vector && function == a0_gpu_cwp) {
        const auto source = runtime.state().gpr[4];
        const auto count = runtime.state().gpr[5];
        for (std::uint32_t index = 0; index < count; ++index) {
            std::uint32_t command{};
            if (!runtime.read32(source + index * 4U, command) ||
                !emitGpuCommand(runtime, false, command)) {
                return result(BiosHleStatus::memory_fault);
            }
        }
        complete(runtime, 0U);
        return result(BiosHleStatus::handled);
    }
    if ((vector == a0_vector && (function == a0_puts || function == a0_printf)) ||
        (vector == b0_vector && function == b0_puts)) {
        if (!writeTtyString(runtime, runtime.state().gpr[4])) {
            return result(BiosHleStatus::memory_fault);
        }
        complete(runtime, 0);
        return result(BiosHleStatus::handled);
    }
    if (vector == b0_vector && function == b0_open_event) {
        for (std::size_t index = 0; index < events_.size(); ++index) {
            if (!events_[index].allocated) {
                events_[index] = Event{
                    runtime.state().gpr[4],
                    runtime.state().gpr[5],
                    runtime.state().gpr[6],
                    runtime.state().gpr[7],
                    true,
                    false,
                    false,
                };
                complete(
                    runtime,
                    event_descriptor_base + static_cast<std::uint32_t>(index));
                return result(BiosHleStatus::handled);
            }
        }
        complete(runtime, std::numeric_limits<std::uint32_t>::max());
        return result(BiosHleStatus::handled);
    }
    if (vector == b0_vector &&
        (function == b0_close_event || function == b0_wait_event ||
         function == b0_test_event || function == b0_enable_event ||
         function == b0_disable_event)) {
        const auto index = eventIndex(runtime.state().gpr[4]);
        if (!index || !events_[*index].allocated) {
            complete(runtime, 0U);
            return result(BiosHleStatus::handled);
        }
        auto& event = events_[*index];
        auto return_value = 1U;
        if (function == b0_close_event) {
            event = {};
        } else if (function == b0_enable_event) {
            event.enabled = true;
        } else if (function == b0_disable_event) {
            event.enabled = false;
        } else {
            return_value = event.ready ? 1U : 0U;
            if (function == b0_test_event && event.ready) {
                event.ready = false;
            }
        }
        complete(runtime, return_value);
        return result(BiosHleStatus::handled);
    }
    if (vector == b0_vector && function == b0_deliver_event) {
        for (auto& event : events_) {
            if (event.allocated && event.enabled &&
                event.event_class == runtime.state().gpr[4] &&
                event.event_spec == runtime.state().gpr[5] &&
                event.mode == 0x2000U) {
                event.ready = true;
            }
        }
        complete(runtime, 1U);
        return result(BiosHleStatus::handled);
    }
    if (vector == b0_vector && function == b0_hook_entry_int) {
        entry_interrupt_hook_ = runtime.state().gpr[4];
        complete(runtime, 0);
        return result(BiosHleStatus::handled);
    }
    if (vector == b0_vector &&
        (function == b0_init_card || function == b0_start_card ||
         function == b0_stop_card)) {
        // Device setup has no documented return value. The persistent card is
        // exposed by the file and raw-frame calls above once StartCard runs.
        // Card setup is also where the kernel publishes the "bu" device, which
        // retail's own `firstfile` needs before it will reach the BIOS.
        if (!installMemoryCardDevice(runtime)) {
            return result(BiosHleStatus::memory_fault);
        }
        runtime.completeHostCall();
        return result(BiosHleStatus::handled);
    }
    if (vector == b0_vector &&
        (function == b0_get_c0_table || function == b0_get_b0_table)) {
        const auto requests_c0 = function == b0_get_c0_table;
        const auto table_address =
            requests_c0 ? c0_table_address : b0_table_address;
        const auto thunk_base = requests_c0 ? c0_thunk_base : b0_thunk_base;
        if (!writeFunctionTable(runtime, table_address, thunk_base)) {
            return result(BiosHleStatus::memory_fault);
        }
        complete(runtime, table_address);
        return result(BiosHleStatus::handled);
    }
    if (vector == b0_vector && function == b0_change_clear_pad) {
        const auto previous = clear_pad_;
        clear_pad_ = runtime.state().gpr[4];
        complete(runtime, previous);
        return result(BiosHleStatus::handled);
    }
    if (vector == c0_vector && function == c0_change_clear_root_counter) {
        const auto counter = runtime.state().gpr[4];
        if (counter >= clear_root_counter_.size()) {
            complete(runtime, 0);
            return result(BiosHleStatus::handled);
        }
        const auto previous = clear_root_counter_[counter];
        clear_root_counter_[counter] = runtime.state().gpr[5];
        complete(runtime, previous);
        return result(BiosHleStatus::handled);
    }
    if (vector == c0_vector &&
        (function == c0_enqueue_interrupt ||
         function == c0_dequeue_interrupt)) {
        const auto priority = runtime.state().gpr[4];
        const auto structure = runtime.state().gpr[5];
        if (priority >= interrupt_priority_heads_.size() || structure == 0U) {
            runtime.completeHostCall();
            return result(BiosHleStatus::handled);
        }
        auto& head = interrupt_priority_heads_[priority];
        if (function == c0_enqueue_interrupt) {
            if (!runtime.write32(structure, head)) {
                return result(BiosHleStatus::memory_fault);
            }
            head = structure;
        } else if (head == structure) {
            std::uint32_t next{};
            if (!runtime.read32(structure, next)) {
                return result(BiosHleStatus::memory_fault);
            }
            head = next;
        } else {
            auto current = head;
            for (std::size_t depth = 0; current != 0U && depth < 64U; ++depth) {
                std::uint32_t next{};
                if (!runtime.read32(current, next)) {
                    return result(BiosHleStatus::memory_fault);
                }
                if (next == structure) {
                    std::uint32_t successor{};
                    if (!runtime.read32(structure, successor) ||
                        !runtime.write32(current, successor)) {
                        return result(BiosHleStatus::memory_fault);
                    }
                    break;
                }
                current = next;
            }
        }
        // These BIOS routines do not define a return value.
        runtime.completeHostCall();
        return result(BiosHleStatus::handled);
    }
    return result(BiosHleStatus::unsupported);
}

BiosHleResult BiosHle::dispatchSyscall(R3000Runtime& runtime) {
    const auto function = runtime.state().gpr[4];
    constexpr std::uint32_t enter_critical_section = 1U;
    constexpr std::uint32_t exit_critical_section = 2U;
    if (function == enter_critical_section || function == exit_critical_section) {
        runtime.setRegister(2, 1U);
        return {BiosHleStatus::handled, 0U, function};
    }
    return {BiosHleStatus::unsupported, 0U, function};
}

} // namespace stuntmaster::psx
