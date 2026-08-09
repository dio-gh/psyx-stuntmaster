#pragma once

#include "stuntmaster/psx/memory_card.hpp"
#include "stuntmaster/psx/r3000_runtime.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace stuntmaster::core {
class StateReader;
class StateWriter;
}

namespace stuntmaster::psx {

enum class BiosHleStatus {
    not_boundary,
    handled,
    unsupported,
    memory_fault,
};

struct BiosHleResult {
    BiosHleStatus status{BiosHleStatus::not_boundary};
    std::uint32_t vector{};
    std::uint32_t function{};
};

class BiosHle final {
public:
    using TtySink = std::function<void(std::string_view)>;
    using GpuCommandSink = std::function<void(bool, std::uint32_t)>;

    explicit BiosHle(
        TtySink tty_sink = {}, GpuCommandSink gpu_sink = {},
        MemoryCard* memory_card = nullptr);
    [[nodiscard]] static std::span<const std::uint32_t>
    executionBoundaries() noexcept;
    [[nodiscard]] BiosHleResult dispatch(R3000Runtime& runtime);
    [[nodiscard]] BiosHleResult dispatchSyscall(R3000Runtime& runtime);
    [[nodiscard]] std::uint32_t entryInterruptHook() const noexcept {
        return entry_interrupt_hook_;
    }
    [[nodiscard]] std::uint64_t gpuCommandCount() const noexcept {
        return gpu_command_count_;
    }
    [[nodiscard]] std::uint32_t lastGpuCommand() const noexcept {
        return last_gpu_command_;
    }
    [[nodiscard]] bool lastGpuCommandWasGp1() const noexcept {
        return last_gpu_command_was_gp1_;
    }
    [[nodiscard]] std::uint32_t gpuStatus() const noexcept {
        return gpu_status_;
    }
    [[nodiscard]] std::optional<std::uint32_t> eventCallback(
        std::uint32_t event_class,
        std::uint32_t event_spec) const noexcept;
    [[nodiscard]] std::optional<std::uint32_t>
    takePendingEventCallback() noexcept;
    void writeState(core::StateWriter& writer) const;
    [[nodiscard]] bool readState(core::StateReader& reader);

private:
    [[nodiscard]] bool writeSetjmpBuffer(R3000Runtime& runtime,
                                         std::uint32_t destination) const;
    [[nodiscard]] bool writeTtyString(const R3000Runtime& runtime,
                                      std::uint32_t address) const;
    [[nodiscard]] bool emitGpuCommand(
        R3000Runtime& runtime, bool gp1, std::uint32_t command);
    [[nodiscard]] bool writeFunctionTable(
        R3000Runtime& runtime,
        std::uint32_t table_address,
        std::uint32_t thunk_base) const;
    void complete(R3000Runtime& runtime, std::uint32_t return_value) const;
    [[nodiscard]] std::optional<BiosHleStatus> dispatchMemoryCard(
        R3000Runtime& runtime, std::uint32_t vector, std::uint32_t function);
    [[nodiscard]] bool readGuestString(
        const R3000Runtime& runtime, std::uint32_t address,
        std::string& text) const;
    [[nodiscard]] bool installMemoryCardDevice(R3000Runtime& runtime);
    void queueEvent(std::uint32_t event_class, std::uint32_t event_spec);

    TtySink tty_sink_;
    GpuCommandSink gpu_sink_;
    MemoryCard* memory_card_{};
    std::uint32_t entry_interrupt_hook_{};
    std::uint32_t clear_pad_{};
    std::uint32_t last_gpu_command_{};
    std::uint32_t gpu_status_{0x1C000000U};
    std::uint64_t gpu_command_count_{};
    bool last_gpu_command_was_gp1_{};
    bool memory_card_device_installed_{};
    std::array<std::uint32_t, 4> clear_root_counter_{};
    std::array<std::uint32_t, 4> interrupt_priority_heads_{};
    struct Event {
        std::uint32_t event_class{};
        std::uint32_t event_spec{};
        std::uint32_t mode{};
        std::uint32_t callback{};
        bool allocated{};
        bool enabled{};
        bool ready{};
    };
    std::array<Event, 32> events_{};
    struct OpenFile {
        std::string name;
        std::uint32_t position{};
        bool allocated{};
    };
    std::array<OpenFile, 16> open_files_{};
    std::vector<MemoryCard::File> directory_search_;
    std::size_t directory_search_index_{};
    std::deque<std::uint32_t> pending_event_callbacks_;
};

} // namespace stuntmaster::psx
