#pragma once

#include "stuntmaster/psx/r3000_runtime.hpp"

#include <cstdint>
#include <functional>
#include <span>
#include <utility>
#include <vector>

namespace stuntmaster::core {
class StateReader;
class StateWriter;
}

namespace stuntmaster::psx {

// Minimal GPU register boundary used during bootstrap. Command words are
// captured for the native renderer; GPUSTAT is host-owned and never aliases
// the last GP1 command.
class GpuCommandBridge final : public R3000MmioBus {
public:
    using CommandSink = std::function<void(bool, std::uint32_t)>;
    using LinkedListBeginSink = std::function<void()>;
    // A DMA4 block transfer's payload, which is retail uploading samples into
    // sound RAM. The bridge reads it out of guest memory and hands it over
    // rather than modelling the SPU itself.
    using Dma4Sink = std::function<void(std::span<const std::uint16_t>)>;
    using Dma2BeginSink = std::function<void(
        std::uint32_t,
        std::uint32_t,
        std::uint32_t)>;

    explicit GpuCommandBridge(
        CommandSink sink = {},
        R3000Runtime* runtime = nullptr,
        LinkedListBeginSink linked_list_begin_sink = {},
        Dma2BeginSink dma2_begin_sink = {});

    void setDma4Sink(Dma4Sink sink) { dma4_sink_ = std::move(sink); }

    // Registers this bridge does not own are offered here before falling
    // through to the runtime's undifferentiated MMIO array, which is where SPU
    // writes used to disappear.
    void setFallbackBus(R3000MmioBus* bus) noexcept { fallback_bus_ = bus; }

    [[nodiscard]] bool readMmio(
        std::uint32_t physical_address,
        R3000AccessWidth width,
        std::uint32_t& value) noexcept override;
    [[nodiscard]] bool writeMmio(
        std::uint32_t physical_address,
        R3000AccessWidth width,
        std::uint32_t value) noexcept override;

    [[nodiscard]] std::uint32_t status() const noexcept { return status_; }
    [[nodiscard]] std::uint64_t commandCount() const noexcept {
        return command_count_;
    }
    [[nodiscard]] std::uint32_t lastCommand() const noexcept {
        return last_command_;
    }
    [[nodiscard]] bool lastCommandWasGp1() const noexcept {
        return last_command_was_gp1_;
    }
    [[nodiscard]] bool takeDma4Completion() noexcept {
        const auto pending = dma4_completion_pending_;
        dma4_completion_pending_ = false;
        return pending;
    }
    [[nodiscard]] bool takeDma2Completion() noexcept {
        const auto pending = dma2_completion_pending_;
        dma2_completion_pending_ = false;
        return pending;
    }
    [[nodiscard]] std::uint64_t dma2TransferCount() const noexcept {
        return dma2_transfer_count_;
    }
    [[nodiscard]] std::uint64_t dma2LinkedListCommands() const noexcept {
        return dma2_linked_list_commands_;
    }
    [[nodiscard]] std::uint64_t dma2BlockCommands() const noexcept {
        return dma2_block_commands_;
    }
    // DMA2 transfers split by sync mode. An ordering table arrives in linked
    // list mode and is what retail's DrawSync callback reports finished; an
    // image upload arrives in block mode and is not a drawing completion. The
    // undivided transfer count cannot tell them apart, and there are far more
    // uploads than ordering tables.
    [[nodiscard]] std::uint64_t dma2LinkedListTransfers() const noexcept {
        return dma2_linked_list_transfers_;
    }
    [[nodiscard]] std::uint64_t dma2BlockTransfers() const noexcept {
        return dma2_block_transfers_;
    }
    [[nodiscard]] std::uint32_t dma2BlockControl() const noexcept {
        return dma_block_control_;
    }
    [[nodiscard]] std::uint32_t dma2ChannelControl() const noexcept {
        return dma_channel_control_;
    }
    [[nodiscard]] std::uint32_t displayStartX() const noexcept {
        return display_start_x_;
    }
    [[nodiscard]] std::uint32_t displayStartY() const noexcept {
        return display_start_y_;
    }
    [[nodiscard]] std::uint32_t displayMode() const noexcept {
        return display_mode_;
    }
    [[nodiscard]] std::uint32_t displayWidth() const noexcept {
        if ((display_mode_ & 0x40U) != 0U) {
            return 368U;
        }
        switch (display_mode_ & 3U) {
        case 0U: return 256U;
        case 1U: return 320U;
        case 2U: return 512U;
        default: return 640U;
        }
    }
    [[nodiscard]] std::uint32_t displayHeight() const noexcept {
        return (display_mode_ & 0x04U) != 0U ? 480U : 240U;
    }
    void writeState(core::StateWriter& writer) const;
    [[nodiscard]] bool readState(core::StateReader& reader);

private:
    void capture(bool gp1, std::uint32_t command);
    void captureDma2() noexcept;
    void captureDma4() noexcept;

    CommandSink sink_;
    LinkedListBeginSink linked_list_begin_sink_;
    Dma2BeginSink dma2_begin_sink_;
    Dma4Sink dma4_sink_;
    std::vector<std::uint16_t> dma4_payload_;
    std::uint32_t status_{0x1C000000U};
    std::uint32_t last_command_{};
    std::uint32_t dma_address_{};
    std::uint32_t dma_block_control_{};
    std::uint32_t dma_channel_control_{};
    std::uint32_t dma4_address_{};
    std::uint32_t dma4_block_control_{};
    std::uint32_t dma4_channel_control_{};
    std::uint32_t dma6_address_{};
    std::uint32_t dma6_block_control_{};
    std::uint32_t dma6_channel_control_{};
    std::uint32_t display_start_x_{};
    std::uint32_t display_start_y_{};
    std::uint32_t display_mode_{};
    std::uint64_t command_count_{};
    std::uint64_t dma2_transfer_count_{};
    std::uint64_t dma2_linked_list_commands_{};
    std::uint64_t dma2_block_commands_{};
    std::uint64_t dma2_linked_list_transfers_{};
    std::uint64_t dma2_block_transfers_{};
    bool last_command_was_gp1_{};
    bool dma2_completion_pending_{};
    bool dma4_completion_pending_{};
    R3000MmioBus* fallback_bus_{};
    R3000Runtime* runtime_{};
};

} // namespace stuntmaster::psx
