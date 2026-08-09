#include "stuntmaster/psx/gpu_command_bridge.hpp"

#include "stuntmaster/core/state_archive.hpp"

#include <bitset>
#include <utility>

namespace stuntmaster::psx {
namespace {

constexpr std::uint32_t gp0_register = 0x1F801810U;
constexpr std::uint32_t gp1_register = 0x1F801814U;
constexpr std::uint32_t dma2_address_register = 0x1F8010A0U;
constexpr std::uint32_t dma2_block_control_register = 0x1F8010A4U;
constexpr std::uint32_t dma2_channel_control_register = 0x1F8010A8U;
constexpr std::uint32_t dma4_address_register = 0x1F8010C0U;
constexpr std::uint32_t dma4_block_control_register = 0x1F8010C4U;
constexpr std::uint32_t dma4_channel_control_register = 0x1F8010C8U;
constexpr std::uint32_t dma6_address_register = 0x1F8010E0U;
constexpr std::uint32_t dma6_block_control_register = 0x1F8010E4U;
constexpr std::uint32_t dma6_channel_control_register = 0x1F8010E8U;
constexpr std::uint32_t dma_direction_mask = 3U << 29U;
constexpr std::uint32_t dma_busy = 1U << 24U;
constexpr std::uint32_t dma_from_ram = 1U;
constexpr std::uint32_t dma_sync_shift = 9U;
constexpr std::uint32_t dma_sync_mask = 3U;
constexpr std::uint32_t dma_linked_list_sync = 2U;
constexpr std::uint32_t dma_end_of_list = 0x00FFFFFFU;
constexpr std::uint32_t ram_address_mask = 0x001FFFFCU;

} // namespace

GpuCommandBridge::GpuCommandBridge(
    CommandSink sink,
    R3000Runtime* runtime,
    LinkedListBeginSink linked_list_begin_sink,
    Dma2BeginSink dma2_begin_sink)
    : sink_(std::move(sink)),
      linked_list_begin_sink_(std::move(linked_list_begin_sink)),
      dma2_begin_sink_(std::move(dma2_begin_sink)),
      runtime_(runtime) {}

void GpuCommandBridge::writeState(core::StateWriter& writer) const {
    writer.vectorPod(dma4_payload_);
    writer.pod(status_);
    writer.pod(last_command_);
    writer.pod(dma_address_);
    writer.pod(dma_block_control_);
    writer.pod(dma_channel_control_);
    writer.pod(dma4_address_);
    writer.pod(dma4_block_control_);
    writer.pod(dma4_channel_control_);
    writer.pod(dma6_address_);
    writer.pod(dma6_block_control_);
    writer.pod(dma6_channel_control_);
    writer.pod(display_start_x_);
    writer.pod(display_start_y_);
    writer.pod(display_mode_);
    writer.pod(command_count_);
    writer.pod(dma2_transfer_count_);
    writer.pod(dma2_linked_list_commands_);
    writer.pod(dma2_block_commands_);
    writer.pod(dma2_linked_list_transfers_);
    writer.pod(dma2_block_transfers_);
    writer.pod(last_command_was_gp1_);
    writer.pod(dma2_completion_pending_);
    writer.pod(dma4_completion_pending_);
}

bool GpuCommandBridge::readState(core::StateReader& reader) {
    constexpr std::uint64_t maximum_dma4_halfwords = 512U * 1024U / 2U;
    return reader.vectorPod(dma4_payload_, maximum_dma4_halfwords) &&
        reader.pod(status_) && reader.pod(last_command_) &&
        reader.pod(dma_address_) && reader.pod(dma_block_control_) &&
        reader.pod(dma_channel_control_) && reader.pod(dma4_address_) &&
        reader.pod(dma4_block_control_) &&
        reader.pod(dma4_channel_control_) && reader.pod(dma6_address_) &&
        reader.pod(dma6_block_control_) &&
        reader.pod(dma6_channel_control_) && reader.pod(display_start_x_) &&
        reader.pod(display_start_y_) && reader.pod(display_mode_) &&
        reader.pod(command_count_) && reader.pod(dma2_transfer_count_) &&
        reader.pod(dma2_linked_list_commands_) &&
        reader.pod(dma2_block_commands_) &&
        reader.pod(dma2_linked_list_transfers_) &&
        reader.pod(dma2_block_transfers_) &&
        reader.pod(last_command_was_gp1_) &&
        reader.pod(dma2_completion_pending_) &&
        reader.pod(dma4_completion_pending_);
}

bool GpuCommandBridge::readMmio(
    std::uint32_t physical_address,
    R3000AccessWidth width,
    std::uint32_t& value) noexcept {
    // Offered first, because a device on the chain claims its own range and
    // this bridge answers every remaining word access itself.
    if (fallback_bus_ != nullptr &&
        fallback_bus_->readMmio(physical_address, width, value)) {
        return true;
    }
    if (width != R3000AccessWidth::word) {
        return false;
    }
    if (physical_address == gp1_register) {
        value = status_;
        return true;
    }
    if (physical_address == gp0_register) {
        value = 0U;
        return true;
    }
    if (physical_address == dma2_address_register) {
        value = dma_address_;
        return true;
    }
    if (physical_address == dma2_block_control_register) {
        value = dma_block_control_;
        return true;
    }
    if (physical_address == dma2_channel_control_register) {
        value = dma_channel_control_;
        return true;
    }
    if (physical_address == dma4_address_register) {
        value = dma4_address_;
        return true;
    }
    if (physical_address == dma4_block_control_register) {
        value = dma4_block_control_;
        return true;
    }
    if (physical_address == dma4_channel_control_register) {
        value = dma4_channel_control_;
        return true;
    }
    if (physical_address == dma6_address_register) {
        value = dma6_address_;
        return true;
    }
    if (physical_address == dma6_block_control_register) {
        value = dma6_block_control_;
        return true;
    }
    if (physical_address == dma6_channel_control_register) {
        value = dma6_channel_control_;
        return true;
    }
    return false;
}

bool GpuCommandBridge::writeMmio(
    std::uint32_t physical_address,
    R3000AccessWidth width,
    std::uint32_t value) noexcept {
    if (fallback_bus_ != nullptr &&
        fallback_bus_->writeMmio(physical_address, width, value)) {
        return true;
    }
    if (width != R3000AccessWidth::word) {
        return false;
    }
    if (physical_address == dma2_address_register) {
        dma_address_ = value & 0x00FFFFFFU;
        return true;
    }
    if (physical_address == dma2_block_control_register) {
        dma_block_control_ = value;
        return true;
    }
    if (physical_address == dma2_channel_control_register) {
        // Bootstrap transfers are host-synchronous. Retain all mode bits but
        // expose the channel as no longer busy on the next guest read.
        dma_channel_control_ = value & ~dma_busy;
        if ((value & dma_busy) != 0U && (value & dma_from_ram) != 0U) {
            captureDma2();
            dma2_completion_pending_ = true;
        }
        return true;
    }
    if (physical_address == dma4_address_register) {
        dma4_address_ = value & 0x00FFFFFFU;
        return true;
    }
    if (physical_address == dma4_block_control_register) {
        dma4_block_control_ = value;
        return true;
    }
    if (physical_address == dma4_channel_control_register) {
        // Uploads complete synchronously so the retail sound loader cannot
        // retain DMA busy. The payload is handed to whoever owns sound RAM.
        dma4_channel_control_ = value & ~dma_busy;
        dma4_completion_pending_ = (value & dma_busy) != 0U;
        if (dma4_completion_pending_) {
            captureDma4();
        }
        return true;
    }
    if (physical_address == dma6_address_register) {
        dma6_address_ = value & 0x00FFFFFFU;
        return true;
    }
    if (physical_address == dma6_block_control_register) {
        dma6_block_control_ = value;
        return true;
    }
    if (physical_address == dma6_channel_control_register) {
        dma6_channel_control_ = value & ~dma_busy;
        if ((value & dma_busy) != 0U && runtime_ != nullptr) {
            auto words = dma6_block_control_ & 0xFFFFU;
            if (words == 0U) {
                words = 0x10000U;
            }
            auto address = dma6_address_ & 0x001FFFFCU;
            for (std::uint32_t index = 0; index < words; ++index) {
                const auto last = index + 1U == words;
                const auto link = last
                    ? 0x00FFFFFFU
                    : ((address - 4U) & 0x00FFFFFFU);
                if (!runtime_->write32(0x80000000U | address, link)) {
                    return false;
                }
                address = (address - 4U) & 0x001FFFFCU;
            }
        }
        return true;
    }
    if (physical_address != gp0_register && physical_address != gp1_register) {
        return false;
    }
    const auto gp1 = physical_address == gp1_register;
    capture(gp1, value);
    if (gp1 && (value >> 24U) == 0x04U) {
        status_ = (status_ & ~dma_direction_mask) |
            ((value & 3U) << 29U);
    } else if (gp1 && (value >> 24U) == 0x00U) {
        status_ = 0x1C000000U;
    }
    return true;
}

void GpuCommandBridge::capture(bool gp1, std::uint32_t command) {
    if (gp1 && (command >> 24U) == 0x05U) {
        display_start_x_ = command & 0x3FFU;
        display_start_y_ = (command >> 10U) & 0x1FFU;
    } else if (gp1 && (command >> 24U) == 0x08U) {
        display_mode_ = command & 0xFFU;
    }
    last_command_ = command;
    last_command_was_gp1_ = gp1;
    ++command_count_;
    if (sink_) {
        sink_(gp1, command);
    }
}

void GpuCommandBridge::captureDma4() noexcept {
    // Only a transfer to the SPU carries samples; the opposite direction is the
    // guest reading sound RAM back, which nothing here needs yet.
    constexpr std::uint32_t dma_to_device = 1U;
    if (runtime_ == nullptr || !dma4_sink_ ||
        (dma4_channel_control_ & dma_to_device) == 0U) {
        return;
    }
    auto words_per_block = dma4_block_control_ & 0xFFFFU;
    if (words_per_block == 0U) {
        words_per_block = 0x10000U;
    }
    std::uint64_t words = words_per_block;
    const auto sync = (dma4_channel_control_ >> dma_sync_shift) & dma_sync_mask;
    if (sync == 1U) {
        auto blocks = dma4_block_control_ >> 16U;
        if (blocks == 0U) {
            blocks = 0x10000U;
        }
        words *= blocks;
    }
    constexpr std::uint64_t max_words =
        R3000Runtime::ram_size / sizeof(std::uint32_t);
    if (words > max_words) {
        words = max_words;
    }
    // Each DMA word is two sound-RAM halfwords, low half first.
    dma4_payload_.clear();
    dma4_payload_.reserve(static_cast<std::size_t>(words) * 2U);
    auto address = dma4_address_ & ram_address_mask;
    for (std::uint64_t index = 0; index < words; ++index) {
        std::uint32_t word{};
        if (!runtime_->read32(0x80000000U | address, word)) {
            break;
        }
        dma4_payload_.push_back(static_cast<std::uint16_t>(word & 0xFFFFU));
        dma4_payload_.push_back(static_cast<std::uint16_t>(word >> 16U));
        address = (address + 4U) & ram_address_mask;
    }
    dma4_sink_(dma4_payload_);
}

void GpuCommandBridge::captureDma2() noexcept {
    if (runtime_ == nullptr) {
        return;
    }
    ++dma2_transfer_count_;
    if (dma2_begin_sink_) {
        dma2_begin_sink_(
            dma_address_,
            dma_block_control_,
            dma_channel_control_);
    }
    const auto sync =
        (dma_channel_control_ >> dma_sync_shift) & dma_sync_mask;
    if (sync == dma_linked_list_sync) {
        ++dma2_linked_list_transfers_;
    } else {
        ++dma2_block_transfers_;
    }
    if (sync == dma_linked_list_sync) {
        if (linked_list_begin_sink_) {
            linked_list_begin_sink_();
        }
        auto address = dma_address_ & ram_address_mask;
        std::bitset<R3000Runtime::ram_size / sizeof(std::uint32_t)> visited;
        constexpr std::uint32_t max_packets = 65'536U;
        constexpr std::uint32_t max_commands = 1'048'576U;
        std::uint32_t commands = 0U;
        for (std::uint32_t packet = 0; packet < max_packets; ++packet) {
            const auto word_index = address / sizeof(std::uint32_t);
            if (visited.test(word_index)) {
                return;
            }
            visited.set(word_index);
            std::uint32_t header{};
            if (!runtime_->read32(0x80000000U | address, header)) {
                return;
            }
            const auto count = header >> 24U;
            for (std::uint32_t index = 0; index < count; ++index) {
                if (++commands > max_commands) {
                    return;
                }
                std::uint32_t command{};
                const auto command_address =
                    (address + (index + 1U) * 4U) & ram_address_mask;
                if (!runtime_->read32(
                        0x80000000U | command_address, command)) {
                    return;
                }
                capture(false, command);
                ++dma2_linked_list_commands_;
            }
            const auto next = header & 0x00FFFFFFU;
            if (next == dma_end_of_list) {
                return;
            }
            const auto next_address = next & ram_address_mask;
            if (next_address == address) {
                return;
            }
            address = next_address;
        }
        return;
    }

    auto words_per_block = dma_block_control_ & 0xFFFFU;
    if (words_per_block == 0U) {
        words_per_block = 0x10000U;
    }
    std::uint64_t words = words_per_block;
    if (sync == 1U) {
        auto blocks = dma_block_control_ >> 16U;
        if (blocks == 0U) {
            blocks = 0x10000U;
        }
        words *= blocks;
    }
    constexpr std::uint64_t max_words =
        R3000Runtime::ram_size / sizeof(std::uint32_t);
    if (words > max_words) {
        words = max_words;
    }
    auto address = dma_address_ & ram_address_mask;
    for (std::uint64_t index = 0; index < words; ++index) {
        std::uint32_t command{};
        if (!runtime_->read32(0x80000000U | address, command)) {
            return;
        }
        capture(false, command);
        ++dma2_block_commands_;
        address = (address + 4U) & ram_address_mask;
    }
}

} // namespace stuntmaster::psx
