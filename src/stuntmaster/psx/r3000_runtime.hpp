// Adapted from SF-pc-port d9522cd under the MIT License.
#pragma once

#include "stuntmaster/psx/executable.hpp"
#include "stuntmaster/psx/gte_runtime.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace stuntmaster::core {
class StateReader;
class StateWriter;
}

namespace stuntmaster::game {
class RetimeHooks;
struct RetimeHook;
} // namespace stuntmaster::game

namespace stuntmaster::psx {

enum class R3000StopReason {
    running,
    returned,
    instruction_budget,
    unsupported_instruction,
    memory_fault,
    alignment_fault,
    arithmetic_overflow,
    syscall,
    breakpoint,
};

[[nodiscard]] std::string_view toString(R3000StopReason reason) noexcept;

struct R3000RunResult {
    R3000StopReason reason{R3000StopReason::running};
    std::uint64_t instructions{};
    std::uint32_t pc{};
    std::uint32_t instruction{};
};

// Compact set of guest PCs at which a batched executor must return control to
// the host before executing the instruction. RAM is word-addressed through a
// bitmap (including its mirrors and KSEG aliases); the handful of non-RAM
// boundaries, such as the A0/B0/C0 BIOS vectors, stay in a sparse list.
class R3000ExecutionBoundaries final {
public:
    R3000ExecutionBoundaries();

    void add(std::uint32_t address);
    void add(std::span<const std::uint32_t> addresses);
    void addRange(
        std::uint32_t first, std::uint32_t past_last,
        std::uint32_t stride = sizeof(std::uint32_t));
    [[nodiscard]] bool contains(std::uint32_t address) const noexcept;

private:
    static constexpr std::size_t ram_word_count =
        (2U * 1024U * 1024U) / sizeof(std::uint32_t);
    static constexpr std::size_t bitmap_word_count =
        ram_word_count / 64U;

    std::vector<std::uint64_t> ram_bitmap_;
    std::vector<std::uint32_t> sparse_;
};

enum class R3000AccessWidth : std::uint8_t {
    byte = 1U,
    halfword = 2U,
    word = 4U,
};

// Width-aware MMIO boundary. A false return leaves the address to the
// interpreter's passive compatibility shadow.
class R3000MmioBus {
public:
    virtual ~R3000MmioBus() = default;
    [[nodiscard]] virtual bool readMmio(
        std::uint32_t physical_address,
        R3000AccessWidth width,
        std::uint32_t& value) noexcept = 0;
    [[nodiscard]] virtual bool writeMmio(
        std::uint32_t physical_address,
        R3000AccessWidth width,
        std::uint32_t value) noexcept = 0;
};

struct R3000DelayedLoadState {
    std::uint8_t reg{};
    std::uint32_t value{};
    bool valid{};
};

struct R3000State {
    std::array<std::uint32_t, 32> gpr{};
    GteState gte{};
    std::uint32_t cop0_status{};
    std::uint32_t cop0_cause{};
    std::uint32_t cop0_epc{};
    std::uint32_t cop0_bad_vaddr{};
    std::uint32_t hi{};
    std::uint32_t lo{};
    std::uint32_t pc{};
    std::uint32_t next_pc{};
    std::uint32_t branch_pc{};
    bool branch_delay_slot{};
    R3000DelayedLoadState load_delay{};
    R3000DelayedLoadState next_load_delay{};
};

// Deterministic interpreter for the user-code portion of the original R3000A.
// Hardware effects are supplied by an optional width-aware machine bus. Any
// unclaimed MMIO byte remains available through the compatibility shadow.
class R3000Runtime final {
public:
    using MemoryWriteSink = std::function<void(
        std::uint32_t address,
        std::uint32_t size,
        std::uint32_t value,
        std::uint32_t pc)>;

    static constexpr std::size_t ram_size = 2U * 1024U * 1024U;
    static constexpr std::size_t scratchpad_size = 1024U;
    static constexpr std::size_t mmio_size = 4U * 1024U;
    static constexpr std::uint32_t return_sentinel = 0xfffffff0U;

    // Host-injected interrupt callbacks run on their own guest stack instead
    // of whatever stack the interrupted code happened to be using. Retail
    // runs its per-frame game step on the 1 KB hardware scratchpad
    // (`0x1F8003F8`), which leaves no room for a nested callback; on real
    // hardware the BIOS exception handler switches stacks before dispatching
    // callbacks, so borrowing the interrupted stack was never faithful. The
    // region below the overlay load address at `0x80010000` and above the
    // BIOS HLE thunk tables is reserved by the kernel and never allocated by
    // retail.
    static constexpr std::uint32_t interrupt_stack_top = 0x8000fff0U;
    static constexpr std::uint32_t interrupt_stack_size = 8U * 1024U;

    // Host-owned guest code lives here: trampolines that retail patches jump
    // to when a change needs more than the one word available at the patch
    // site. The region sits above the BIOS HLE thunk tables, which end below
    // `0x80003000`, and below both the overlay load address and the interrupt
    // stack. The retail entry point clears BSS from `0x800DD658` to
    // `0x800E2C84`, so it does not reach this arena.
    static constexpr std::uint32_t patch_arena_base = 0x80003000U;
    static constexpr std::uint32_t patch_arena_size = 4U * 1024U;

    static_assert(
        patch_arena_base + patch_arena_size <=
            interrupt_stack_top - interrupt_stack_size,
        "the patch arena must not overlap the interrupt stack");

    R3000Runtime();

    void clearMemory() noexcept;
    void loadExecutable(const Executable& executable);
    [[nodiscard]] bool loadBytes(
        std::uint32_t address,
        std::span<const std::byte> bytes) noexcept;
    [[nodiscard]] bool copyBytes(
        std::uint32_t address,
        std::span<std::byte> destination) const noexcept;
    [[nodiscard]] bool restoreRam(std::span<const std::byte> bytes) noexcept;
    [[nodiscard]] bool restoreScratchpad(std::span<const std::byte> bytes) noexcept;
    [[nodiscard]] bool restoreMmio(std::span<const std::byte> bytes) noexcept;

    void reset(std::uint32_t pc, std::uint32_t gp = 0U, std::uint32_t sp = 0U) noexcept;
    void restoreCpuState(const R3000State& state) noexcept;
    [[nodiscard]] bool beginCall(
        std::uint32_t address,
        std::span<const std::uint32_t> arguments = {}) noexcept;
    // As `beginCall`, but switches to the reserved interrupt stack first.
    // The caller must snapshot the interrupted `R3000State` beforehand and
    // restore it at the return sentinel, which also restores the old stack
    // pointer.
    [[nodiscard]] bool beginInterruptCall(
        std::uint32_t address,
        std::span<const std::uint32_t> arguments = {}) noexcept;
    void completeHostCall() noexcept;
    void settleLoadDelay() noexcept;
    void setRegister(std::uint8_t reg, std::uint32_t value) noexcept;
    void attachMmioBus(R3000MmioBus* bus) noexcept { mmio_bus_ = bus; }
    // The sink's `pc` is the interpreter's `pc` register at write time. `step`
    // advances it to `next_pc` before executing the opcode, so for a guest
    // store it points one instruction past the store. Host-initiated writes
    // such as `loadBytes` report whatever `pc` happened to hold.
    void setMemoryWriteSink(MemoryWriteSink sink) {
        memory_write_sink_ = std::move(sink);
    }
    void setExternalInterrupt(bool active) noexcept;

    // Attaches the host retime-hook table. When `hooks` is non-null and
    // `hooks->active()`, the interpreter consults `hooks->find(pc)` before
    // dispatching an instruction and runs the matched hook's `fn` in place of
    // the site (with the site's delay-slot instruction executed first). The
    // hooks object outlives the runtime; a null pointer detaches.
    void setRetimeHooks(game::RetimeHooks* hooks) noexcept;

    [[nodiscard]] bool interruptPending() const noexcept;

    [[nodiscard]] bool atReturnSentinel() const noexcept {
        return state_.pc == return_sentinel;
    }

    [[nodiscard]] R3000RunResult step() noexcept;
    // Executes ordinary guest instructions internally until the budget is
    // consumed or host attention is required. A boundary is observed before
    // its instruction; a claimed MMIO access is observed immediately after
    // its instruction, preserving the single-step scheduler's device ordering.
    [[nodiscard]] R3000RunResult runBatch(
        std::uint64_t instruction_budget,
        const R3000ExecutionBoundaries& boundaries,
        bool execute_initial_boundary = false) noexcept;
    [[nodiscard]] R3000RunResult call(
        std::uint32_t address,
        std::span<const std::uint32_t> arguments = {},
        std::uint64_t instruction_budget = 1'000'000U) noexcept;

    [[nodiscard]] bool read8(std::uint32_t address, std::uint8_t& value) const noexcept;
    [[nodiscard]] bool read16(std::uint32_t address, std::uint16_t& value) const noexcept;
    [[nodiscard]] bool read32(std::uint32_t address, std::uint32_t& value) const noexcept;
    [[nodiscard]] bool write8(std::uint32_t address, std::uint8_t value) noexcept;
    [[nodiscard]] bool write16(std::uint32_t address, std::uint16_t value) noexcept;
    [[nodiscard]] bool write32(std::uint32_t address, std::uint32_t value) noexcept;

    [[nodiscard]] const R3000State& state() const noexcept { return state_; }
    [[nodiscard]] std::span<const std::byte> ram() const noexcept { return ram_; }
    [[nodiscard]] std::span<const std::byte> scratchpad() const noexcept {
        return scratchpad_;
    }
    [[nodiscard]] std::span<const std::byte> mmio() const noexcept { return mmio_; }
    void writeState(core::StateWriter& writer) const;
    [[nodiscard]] bool readState(core::StateReader& reader);
    void rebindStatePointers() noexcept;

private:
    [[nodiscard]] std::byte* memoryByte(std::uint32_t address) noexcept;
    [[nodiscard]] const std::byte* memoryByte(std::uint32_t address) const noexcept;
    [[nodiscard]] static bool physicalAddress(
        std::uint32_t address,
        std::uint32_t& physical) noexcept;
    [[nodiscard]] bool readMmio(
        std::uint32_t address,
        R3000AccessWidth width,
        std::uint32_t& value) const noexcept;
    [[nodiscard]] bool writeMmio(
        std::uint32_t address,
        R3000AccessWidth width,
        std::uint32_t value) noexcept;
    void writeRegister(std::uint8_t reg, std::uint32_t value) noexcept;
    void scheduleLoad(std::uint8_t reg, std::uint32_t value) noexcept;
    void advanceLoadDelay() noexcept;
    void flushLoadDelay() noexcept;
    void clearLoadDelay() noexcept;
    void takeInterrupt() noexcept;

    std::vector<std::byte> ram_;
    std::array<std::byte, scratchpad_size> scratchpad_{};
    std::array<std::byte, mmio_size> mmio_{};
    R3000State state_{};
    R3000MmioBus* mmio_bus_{};
    mutable bool mmio_accessed_{};
    MemoryWriteSink memory_write_sink_;
    game::RetimeHooks* retime_hooks_{};
    // The two-step hook invocation: at a site the interpreter arms a virtual
    // branch to `pc + 4` (the site's delay slot), and the step that executes
    // the delay slot then runs the hook and resumes at the PC its `fn`
    // returns. If execution leaves the expected delay-slot PC instead — an
    // interrupt, a fault, or a host call boundary — the pending hook is
    // dropped rather than fired at the wrong instruction.
    const game::RetimeHook* pending_retime_hook_{};
    std::uint32_t pending_retime_hook_site_{};
    std::uint32_t pending_retime_hook_pc_{};
};

} // namespace stuntmaster::psx
