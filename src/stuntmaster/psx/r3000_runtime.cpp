// Adapted from SF-pc-port d9522cd under the MIT License.
#include "stuntmaster/psx/r3000_runtime.hpp"

#include "stuntmaster/core/state_archive.hpp"

#include "stuntmaster/core/error.hpp"
#include "stuntmaster/game/retiming.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <optional>
#include <type_traits>

namespace stuntmaster::psx {
namespace {

constexpr std::uint32_t physical_address_mask = 0x1fffffffU;
constexpr std::uint32_t ram_mirror_end = 0x00800000U;
constexpr std::uint32_t scratchpad_address = 0x1f800000U;
constexpr std::uint32_t mmio_address = 0x1f801000U;
constexpr std::uint32_t bios_address = 0x1fc00000U;
constexpr std::uint32_t bios_size = 512U * 1024U;

std::uint32_t signExtend16(std::uint32_t value) noexcept {
    const auto signed_value = static_cast<std::int16_t>(value & 0xffffU);
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(signed_value));
}

std::int32_t asSigned(std::uint32_t value) noexcept {
    return std::bit_cast<std::int32_t>(value);
}

std::uint32_t arithmeticShiftRight(std::uint32_t value, std::uint32_t amount) noexcept {
    amount &= 31U;
    if (amount == 0U) {
        return value;
    }
    const auto fill = (value & 0x80000000U) != 0U
        ? std::numeric_limits<std::uint32_t>::max() << (32U - amount)
        : 0U;
    return (value >> amount) | fill;
}

bool addOverflows(std::uint32_t left, std::uint32_t right, std::uint32_t& result) noexcept {
    const auto wide = static_cast<std::int64_t>(asSigned(left)) +
        static_cast<std::int64_t>(asSigned(right));
    if (wide < std::numeric_limits<std::int32_t>::min() ||
        wide > std::numeric_limits<std::int32_t>::max()) {
        return true;
    }
    result = static_cast<std::uint32_t>(static_cast<std::int32_t>(wide));
    return false;
}

bool subtractOverflows(
    std::uint32_t left,
    std::uint32_t right,
    std::uint32_t& result) noexcept {
    const auto wide = static_cast<std::int64_t>(asSigned(left)) -
        static_cast<std::int64_t>(asSigned(right));
    if (wide < std::numeric_limits<std::int32_t>::min() ||
        wide > std::numeric_limits<std::int32_t>::max()) {
        return true;
    }
    result = static_cast<std::uint32_t>(static_cast<std::int32_t>(wide));
    return false;
}

#if defined(_M_X64) || defined(__x86_64__)
enum class X64Register : std::uint8_t {
    rax = 0U,
    rcx = 1U,
    rdx = 2U,
    rsi = 6U,
    rdi = 7U,
    r8 = 8U,
    r9 = 9U,
    r10 = 10U,
    r11 = 11U,
};

#if defined(_WIN32)
constexpr auto native_state_register = X64Register::rcx;
constexpr auto native_ram_register = X64Register::rdx;
#else
constexpr auto native_state_register = X64Register::rdi;
constexpr auto native_ram_register = X64Register::rsi;
#endif

class X64Emitter final {
public:
    enum class Condition : std::uint8_t {
        below = 0x82U,
        above_equal = 0x83U,
        equal = 0x84U,
        not_equal = 0x85U,
        less_signed = 0x8cU,
        greater_equal_signed = 0x8dU,
        less_equal_signed = 0x8eU,
        greater_signed = 0x8fU,
    };

    [[nodiscard]] std::size_t offset() const noexcept { return code_.size(); }
    [[nodiscard]] const std::vector<std::uint8_t>& code() const noexcept {
        return code_;
    }

    void endbr64() {
        bytes({0xf3U, 0x0fU, 0x1eU, 0xfaU});
    }

    void movRegReg(X64Register destination, X64Register source) {
        rex(false, destination, X64Register::rax, source);
        byte(0x8bU);
        modrm(3U, destination, source);
    }

    void movRegImm(X64Register destination, std::uint32_t value) {
        rex(false, X64Register::rax, X64Register::rax, destination);
        byte(static_cast<std::uint8_t>(
            0xb8U + (number(destination) & 7U)));
        dword(value);
    }

    void movRegMem(
        X64Register destination, X64Register base, std::int32_t displacement) {
        rex(false, destination, X64Register::rax, base);
        byte(0x8bU);
        memoryModrm(destination, base, displacement);
    }

    void movMemReg(
        X64Register base, std::int32_t displacement, X64Register source) {
        rex(false, source, X64Register::rax, base);
        byte(0x89U);
        memoryModrm(source, base, displacement);
    }

    void movMemImm32(
        X64Register base, std::int32_t displacement, std::uint32_t value) {
        rex(false, X64Register::rax, X64Register::rax, base);
        byte(0xc7U);
        memoryModrm(X64Register::rax, base, displacement);
        dword(value);
    }

    void movMemImm8(
        X64Register base, std::int32_t displacement, std::uint8_t value) {
        rex(false, X64Register::rax, X64Register::rax, base);
        byte(0xc6U);
        memoryModrm(X64Register::rax, base, displacement);
        byte(value);
    }

    void xorRegReg(X64Register destination, X64Register source) {
        aluRegReg(0x33U, destination, source);
    }
    void addRegReg(X64Register destination, X64Register source) {
        aluRegReg(0x03U, destination, source);
    }
    void subRegReg(X64Register destination, X64Register source) {
        aluRegReg(0x2bU, destination, source);
    }
    void andRegReg(X64Register destination, X64Register source) {
        aluRegReg(0x23U, destination, source);
    }
    void orRegReg(X64Register destination, X64Register source) {
        aluRegReg(0x0bU, destination, source);
    }
    void orRegReg64(X64Register destination, X64Register source) {
        aluRegReg(0x0bU, destination, source, true);
    }
    void xorValueReg(X64Register destination, X64Register source) {
        aluRegReg(0x33U, destination, source);
    }
    void cmpRegReg(X64Register left, X64Register right) {
        aluRegReg(0x3bU, left, right);
    }

    void addRegImm(X64Register destination, std::uint32_t value) {
        aluRegImm(0U, destination, value);
    }
    void andRegImm(X64Register destination, std::uint32_t value) {
        aluRegImm(4U, destination, value);
    }
    void orRegImm(X64Register destination, std::uint32_t value) {
        aluRegImm(1U, destination, value);
    }
    void xorRegImm(X64Register destination, std::uint32_t value) {
        aluRegImm(6U, destination, value);
    }
    void cmpRegImm(X64Register destination, std::uint32_t value) {
        aluRegImm(7U, destination, value);
    }
    void testRegImm(X64Register destination, std::uint32_t value) {
        rex(false, X64Register::rax, X64Register::rax, destination);
        byte(0xf7U);
        modrm(3U, X64Register::rax, destination);
        dword(value);
    }

    void notReg(X64Register destination) {
        rex(false, X64Register::rax, X64Register::rax, destination);
        byte(0xf7U);
        modrm(3U, static_cast<X64Register>(2U), destination);
    }

    void shiftLeft(X64Register destination, std::uint8_t amount) {
        shiftImmediate(4U, destination, amount);
    }
    void shiftLeft64(X64Register destination, std::uint8_t amount) {
        shiftImmediate(4U, destination, amount, true);
    }
    void shiftRight(X64Register destination, std::uint8_t amount) {
        shiftImmediate(5U, destination, amount);
    }
    void shiftArithmeticRight(X64Register destination, std::uint8_t amount) {
        shiftImmediate(7U, destination, amount);
    }

    void setLessSignedToEax() {
        bytes({0x0fU, 0x9cU, 0xc0U, 0x0fU, 0xb6U, 0xc0U});
    }
    void setLessUnsignedToEax() {
        bytes({0x0fU, 0x92U, 0xc0U, 0x0fU, 0xb6U, 0xc0U});
    }

    void loadRam(
        X64Register destination,
        std::uint8_t width,
        bool sign_extend) {
        rex(false, destination, X64Register::r10, native_ram_register);
        if (width == 4U) {
            byte(0x8bU);
        } else {
            byte(0x0fU);
            byte(width == 1U
                     ? (sign_extend ? 0xbeU : 0xb6U)
                     : (sign_extend ? 0xbfU : 0xb7U));
        }
        modrm(0U, destination, static_cast<X64Register>(4U));
        sib(0U, X64Register::r10, native_ram_register);
    }

    void storeRam(X64Register source, std::uint8_t width) {
        if (width == 2U) {
            byte(0x66U);
        }
        rex(false, source, X64Register::r10, native_ram_register);
        byte(width == 1U ? 0x88U : 0x89U);
        modrm(0U, source, static_cast<X64Register>(4U));
        sib(0U, X64Register::r10, native_ram_register);
    }

    [[nodiscard]] std::size_t jump(Condition condition) {
        byte(0x0fU);
        byte(static_cast<std::uint8_t>(condition));
        const auto displacement = offset();
        dword(0U);
        return displacement;
    }

    [[nodiscard]] std::size_t jump() {
        byte(0xe9U);
        const auto displacement = offset();
        dword(0U);
        return displacement;
    }

    void patch(std::size_t displacement, std::size_t target) {
        const auto relative = static_cast<std::int64_t>(target) -
            static_cast<std::int64_t>(displacement + sizeof(std::int32_t));
        const auto value = static_cast<std::int32_t>(relative);
        std::memcpy(code_.data() + displacement, &value, sizeof(value));
    }

    void ret() { byte(0xc3U); }

private:
    static std::uint8_t number(X64Register reg) noexcept {
        return static_cast<std::uint8_t>(reg);
    }

    void byte(std::uint8_t value) { code_.push_back(value); }
    void bytes(std::initializer_list<std::uint8_t> values) {
        code_.insert(code_.end(), values.begin(), values.end());
    }
    void dword(std::uint32_t value) {
        for (std::uint32_t shift = 0U; shift != 32U; shift += 8U) {
            byte(static_cast<std::uint8_t>(value >> shift));
        }
    }

    void rex(
        bool wide,
        X64Register reg,
        X64Register index,
        X64Register base) {
        const auto value = static_cast<std::uint8_t>(
            0x40U | (wide ? 8U : 0U) |
            ((number(reg) >> 3U) << 2U) |
            ((number(index) >> 3U) << 1U) |
            (number(base) >> 3U));
        if (value != 0x40U) {
            byte(value);
        }
    }

    void modrm(std::uint8_t mode, X64Register reg, X64Register rm) {
        byte(static_cast<std::uint8_t>(
            (mode << 6U) | ((number(reg) & 7U) << 3U) |
            (number(rm) & 7U)));
    }

    void sib(std::uint8_t scale, X64Register index, X64Register base) {
        byte(static_cast<std::uint8_t>(
            (scale << 6U) | ((number(index) & 7U) << 3U) |
            (number(base) & 7U)));
    }

    void memoryModrm(
        X64Register reg, X64Register base, std::int32_t displacement) {
        modrm(2U, reg, base);
        if ((number(base) & 7U) == 4U) {
            sib(0U, static_cast<X64Register>(4U), base);
        }
        dword(static_cast<std::uint32_t>(displacement));
    }

    void aluRegReg(
        std::uint8_t opcode,
        X64Register destination,
        X64Register source,
        bool wide = false) {
        rex(wide, destination, X64Register::rax, source);
        byte(opcode);
        modrm(3U, destination, source);
    }

    void aluRegImm(
        std::uint8_t operation,
        X64Register destination,
        std::uint32_t value) {
        rex(false, static_cast<X64Register>(operation),
            X64Register::rax, destination);
        byte(0x81U);
        modrm(3U, static_cast<X64Register>(operation), destination);
        dword(value);
    }

    void shiftImmediate(
        std::uint8_t operation,
        X64Register destination,
        std::uint8_t amount,
        bool wide = false) {
        rex(wide, static_cast<X64Register>(operation),
            X64Register::rax, destination);
        byte(0xc1U);
        modrm(3U, static_cast<X64Register>(operation), destination);
        byte(amount);
    }

    std::vector<std::uint8_t> code_;
};

bool nativeAluInstruction(std::uint8_t opcode, std::uint8_t function) noexcept {
    if (opcode == 0x00U) {
        switch (function) {
        case 0x00U: case 0x02U: case 0x03U:
        case 0x21U: case 0x23U: case 0x24U: case 0x25U:
        case 0x26U: case 0x27U: case 0x2aU: case 0x2bU:
            return true;
        default:
            return false;
        }
    }
    return opcode >= 0x09U && opcode <= 0x0fU;
}

bool nativeLoadInstruction(std::uint8_t opcode) noexcept {
    return opcode == 0x20U || opcode == 0x21U || opcode == 0x23U ||
        opcode == 0x24U || opcode == 0x25U;
}

bool nativeStoreInstruction(std::uint8_t opcode) noexcept {
    return opcode == 0x28U || opcode == 0x29U || opcode == 0x2bU;
}

bool nativeConditionalBranch(std::uint8_t opcode) noexcept {
    return opcode == 0x01U || (opcode >= 0x04U && opcode <= 0x07U);
}

bool nativeControlFlowInstruction(
    std::uint8_t opcode, std::uint8_t rt, std::uint8_t function) noexcept {
    if (opcode == 0x00U) {
        return function == 0x08U || function == 0x09U;
    }
    if (opcode == 0x01U) {
        return rt == 0x00U || rt == 0x01U ||
            rt == 0x10U || rt == 0x11U;
    }
    return opcode >= 0x02U && opcode <= 0x07U;
}
#endif

} // namespace

std::string_view toString(R3000StopReason reason) noexcept {
    switch (reason) {
    case R3000StopReason::running: return "running";
    case R3000StopReason::returned: return "returned";
    case R3000StopReason::instruction_budget: return "instruction budget";
    case R3000StopReason::unsupported_instruction: return "unsupported instruction";
    case R3000StopReason::memory_fault: return "memory fault";
    case R3000StopReason::alignment_fault: return "alignment fault";
    case R3000StopReason::arithmetic_overflow: return "arithmetic overflow";
    case R3000StopReason::syscall: return "syscall";
    case R3000StopReason::breakpoint: return "breakpoint";
    }
    return "unknown";
}

R3000Runtime::R3000Runtime() : ram_(ram_size) {}

R3000Runtime::R3000Runtime(const R3000Runtime& other)
    : ram_(other.ram_),
      scratchpad_(other.scratchpad_),
      mmio_(other.mmio_),
      state_(other.state_),
      mmio_bus_(other.mmio_bus_),
      mmio_accessed_(other.mmio_accessed_),
      memory_write_sink_(other.memory_write_sink_),
      retime_hooks_(other.retime_hooks_),
      pending_retime_hook_(other.pending_retime_hook_),
      pending_retime_hook_site_(other.pending_retime_hook_site_),
      pending_retime_hook_pc_(other.pending_retime_hook_pc_),
      execution_backend_(other.execution_backend_) {}

R3000Runtime& R3000Runtime::operator=(const R3000Runtime& other) {
    if (this == &other) {
        return *this;
    }
    ram_ = other.ram_;
    scratchpad_ = other.scratchpad_;
    mmio_ = other.mmio_;
    state_ = other.state_;
    mmio_bus_ = other.mmio_bus_;
    mmio_accessed_ = other.mmio_accessed_;
    memory_write_sink_ = other.memory_write_sink_;
    retime_hooks_ = other.retime_hooks_;
    pending_retime_hook_ = other.pending_retime_hook_;
    pending_retime_hook_site_ = other.pending_retime_hook_site_;
    pending_retime_hook_pc_ = other.pending_retime_hook_pc_;
    execution_backend_ = other.execution_backend_;
    recompiler_cache_.clear();
    std::ranges::fill(recompiler_page_generations_, 0U);
    std::ranges::fill(recompiler_code_pages_, false);
    recompiler_stats_ = {};
    return *this;
}

R3000ExecutionBoundaries::R3000ExecutionBoundaries()
    : ram_bitmap_(bitmap_word_count) {}

void R3000ExecutionBoundaries::add(std::uint32_t address) {
    if ((address & 3U) == 0U) {
        auto physical = address;
        if (address >= 0x80000000U && address < 0xc0000000U) {
            physical &= physical_address_mask;
        }
        if (physical < ram_mirror_end) {
            const auto ram_word =
                (physical & static_cast<std::uint32_t>(
                                R3000Runtime::ram_size - 1U)) /
                sizeof(std::uint32_t);
            ram_bitmap_[ram_word / 64U] |=
                std::uint64_t{1U} << (ram_word & 63U);
            return;
        }
    }
    const auto position = std::ranges::lower_bound(sparse_, address);
    if (position == sparse_.end() || *position != address) {
        sparse_.insert(position, address);
    }
}

void R3000ExecutionBoundaries::add(
    std::span<const std::uint32_t> addresses) {
    for (const auto address : addresses) {
        add(address);
    }
}

void R3000ExecutionBoundaries::addRange(
    std::uint32_t first,
    std::uint32_t past_last,
    std::uint32_t stride) {
    if (stride == 0U) {
        return;
    }
    for (auto address = first; address < past_last;) {
        add(address);
        if (past_last - address <= stride) {
            break;
        }
        address += stride;
    }
}

bool R3000ExecutionBoundaries::contains(std::uint32_t address) const noexcept {
    if ((address & 3U) == 0U) {
        auto physical = address;
        if (address >= 0x80000000U && address < 0xc0000000U) {
            physical &= physical_address_mask;
        }
        if (physical < ram_mirror_end) {
            const auto ram_word =
                (physical & static_cast<std::uint32_t>(
                                R3000Runtime::ram_size - 1U)) /
                sizeof(std::uint32_t);
            return (ram_bitmap_[ram_word / 64U] &
                    (std::uint64_t{1U} << (ram_word & 63U))) != 0U;
        }
    }
    return std::ranges::binary_search(sparse_, address);
}

void R3000Runtime::writeState(core::StateWriter& writer) const {
    writer.bytes(ram_);
    writer.pod(scratchpad_);
    writer.pod(mmio_);
    writer.pod(state_);
    const auto pending_site = pending_retime_hook_ != nullptr
        ? pending_retime_hook_->pc
        : pending_retime_hook_site_;
    writer.pod(pending_site);
    writer.pod(pending_retime_hook_pc_);
}

bool R3000Runtime::readState(core::StateReader& reader) {
    pending_retime_hook_ = nullptr;
    const auto read = reader.bytes(ram_) && reader.pod(scratchpad_) &&
        reader.pod(mmio_) && reader.pod(state_) &&
        reader.pod(pending_retime_hook_site_) &&
        reader.pod(pending_retime_hook_pc_);
    if (read) {
        invalidateRecompilerCache();
    }
    return read;
}

void R3000Runtime::rebindStatePointers() noexcept {
    pending_retime_hook_ = pending_retime_hook_site_ != 0U &&
            retime_hooks_ != nullptr
        ? retime_hooks_->find(pending_retime_hook_site_)
        : nullptr;
    if (pending_retime_hook_ == nullptr) {
        pending_retime_hook_site_ = 0U;
    }
}

void R3000Runtime::clearMemory() noexcept {
    std::ranges::fill(ram_, std::byte{0});
    std::ranges::fill(scratchpad_, std::byte{0});
    std::ranges::fill(mmio_, std::byte{0});
    invalidateRecompilerCache();
}

void R3000Runtime::loadExecutable(const Executable& executable) {
    clearMemory();
    const auto& header = executable.header();
    if (!loadBytes(header.text_address, executable.text())) {
        throw core::Error{"PS-X EXE text does not fit emulated RAM"};
    }
    const auto stack_pointer = header.stack_address == 0U
        ? 0U
        : header.stack_address + header.stack_size;
    reset(header.initial_pc, header.initial_gp, stack_pointer);
}

bool R3000Runtime::loadBytes(
    std::uint32_t address,
    std::span<const std::byte> bytes) noexcept {
    const auto first_address = address;
    auto candidate = address;
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (memoryByte(candidate) == nullptr ||
            (candidate == std::numeric_limits<std::uint32_t>::max() && index + 1U < bytes.size())) {
            return false;
        }
        ++candidate;
    }
    for (const auto byte : bytes) {
        *memoryByte(address) = byte;
        ++address;
    }
    if (!bytes.empty()) {
        std::uint32_t physical{};
        if (physicalAddress(first_address, physical) &&
            physical < ram_mirror_end) {
            noteRamWrite(physical, static_cast<std::uint32_t>(bytes.size()));
        }
    }
    if (memory_write_sink_ && !bytes.empty()) {
        memory_write_sink_(
            first_address,
            static_cast<std::uint32_t>(bytes.size()),
            0U,
            state_.pc);
    }
    return true;
}

bool R3000Runtime::copyBytes(
    std::uint32_t address,
    std::span<std::byte> destination) const noexcept {
    auto candidate = address;
    for (std::size_t index = 0; index < destination.size(); ++index) {
        if (memoryByte(candidate) == nullptr ||
            (candidate == std::numeric_limits<std::uint32_t>::max() &&
             index + 1U < destination.size())) {
            return false;
        }
        ++candidate;
    }
    for (auto& byte : destination) {
        byte = *memoryByte(address);
        ++address;
    }
    return true;
}

bool R3000Runtime::restoreRam(std::span<const std::byte> bytes) noexcept {
    if (bytes.size() != ram_.size()) {
        return false;
    }
    std::ranges::copy(bytes, ram_.begin());
    invalidateRecompilerCache();
    return true;
}

bool R3000Runtime::restoreScratchpad(std::span<const std::byte> bytes) noexcept {
    if (bytes.size() != scratchpad_.size()) {
        return false;
    }
    std::ranges::copy(bytes, scratchpad_.begin());
    return true;
}

bool R3000Runtime::restoreMmio(std::span<const std::byte> bytes) noexcept {
    if (bytes.size() != mmio_.size()) {
        return false;
    }
    std::ranges::copy(bytes, mmio_.begin());
    return true;
}

void R3000Runtime::reset(std::uint32_t pc, std::uint32_t gp, std::uint32_t sp) noexcept {
    state_ = {};
    state_.pc = pc;
    state_.next_pc = pc + 4U;
    state_.gpr[28] = gp;
    state_.gpr[29] = sp;
    pending_retime_hook_ = nullptr;
    clearLoadDelay();
}

void R3000Runtime::restoreCpuState(const R3000State& state) noexcept {
    state_ = state;
    state_.gpr[0] = 0U;
    pending_retime_hook_ = nullptr;
}

bool R3000Runtime::beginCall(
    std::uint32_t address,
    std::span<const std::uint32_t> arguments) noexcept {
    if (arguments.size() > 4U) {
        const auto extra_count = arguments.size() - 4U;
        if (extra_count >
            (std::numeric_limits<std::uint32_t>::max() - 16U) / sizeof(std::uint32_t)) {
            return false;
        }
        const auto required_bytes =
            16U + static_cast<std::uint32_t>(extra_count * sizeof(std::uint32_t));
        const auto stack_offset = state_.gpr[29] & static_cast<std::uint32_t>(ram_size - 1U);
        if (required_bytes > ram_size || (state_.gpr[29] & 3U) != 0U ||
            stack_offset > ram_size - required_bytes) {
            return false;
        }
        for (std::size_t index = 4U; index < arguments.size(); ++index) {
            const auto offset = 16U + static_cast<std::uint32_t>((index - 4U) * 4U);
            std::uint32_t existing{};
            if (!read32(state_.gpr[29] + offset, existing)) {
                return false;
            }
        }
    }

    clearLoadDelay();
    for (std::size_t index = 0; index < 4U; ++index) {
        state_.gpr[4U + index] = index < arguments.size() ? arguments[index] : 0U;
    }
    for (std::size_t index = 4U; index < arguments.size(); ++index) {
        const auto stack_offset = 16U + static_cast<std::uint32_t>((index - 4U) * 4U);
        if (!write32(state_.gpr[29] + stack_offset, arguments[index])) {
            return false;
        }
    }
    state_.gpr[31] = return_sentinel;
    state_.pc = address;
    state_.next_pc = address + 4U;
    state_.branch_pc = 0U;
    state_.branch_delay_slot = false;
    pending_retime_hook_ = nullptr;
    return true;
}

bool R3000Runtime::beginInterruptCall(
    std::uint32_t address,
    std::span<const std::uint32_t> arguments) noexcept {
    const auto interrupted_sp = state_.gpr[29];
    state_.gpr[29] = interrupt_stack_top;
    if (!beginCall(address, arguments)) {
        state_.gpr[29] = interrupted_sp;
        return false;
    }
    return true;
}

void R3000Runtime::completeHostCall() noexcept {
    flushLoadDelay();
    state_.pc = state_.gpr[31];
    state_.next_pc = state_.pc + 4U;
    state_.branch_pc = 0U;
    state_.branch_delay_slot = false;
    pending_retime_hook_ = nullptr;
}

void R3000Runtime::settleLoadDelay() noexcept {
    flushLoadDelay();
}

void R3000Runtime::setRegister(std::uint8_t reg, std::uint32_t value) noexcept {
    if (reg < state_.gpr.size()) {
        writeRegister(reg, value);
    }
}

bool R3000Runtime::physicalAddress(
    std::uint32_t address,
    std::uint32_t& physical) noexcept {
    physical = address;
    if (address >= 0x80000000U && address < 0xc0000000U) {
        physical &= physical_address_mask;
        // The scratchpad is cached-only on the R3000A and has no KSEG1 alias.
        if (address >= 0xa0000000U &&
            physical >= scratchpad_address &&
            physical < scratchpad_address + scratchpad_size) {
            return false;
        }
    } else if (address >= 0x20000000U) {
        return false;
    }
    return true;
}

std::byte* R3000Runtime::memoryByte(std::uint32_t address) noexcept {
    std::uint32_t physical{};
    if (!physicalAddress(address, physical)) {
        return nullptr;
    }

    if (physical < ram_mirror_end) {
        return &ram_[physical & static_cast<std::uint32_t>(ram_size - 1U)];
    }
    if (physical >= scratchpad_address &&
        physical < scratchpad_address + scratchpad_size) {
        return &scratchpad_[physical - scratchpad_address];
    }
    if (physical >= mmio_address && physical < mmio_address + mmio_size) {
        return &mmio_[physical - mmio_address];
    }
    return nullptr;
}

const std::byte* R3000Runtime::memoryByte(std::uint32_t address) const noexcept {
    return const_cast<R3000Runtime*>(this)->memoryByte(address);
}

bool R3000Runtime::readMmio(
    std::uint32_t address,
    R3000AccessWidth width,
    std::uint32_t& value) const noexcept {
    std::uint32_t physical{};
    const auto claimed = mmio_bus_ != nullptr &&
        physicalAddress(address, physical) && physical >= mmio_address &&
        physical < mmio_address + mmio_size &&
        mmio_bus_->readMmio(physical, width, value);
    mmio_accessed_ = mmio_accessed_ || claimed;
    return claimed;
}

bool R3000Runtime::writeMmio(
    std::uint32_t address,
    R3000AccessWidth width,
    std::uint32_t value) noexcept {
    std::uint32_t physical{};
    const auto claimed = mmio_bus_ != nullptr &&
        physicalAddress(address, physical) && physical >= mmio_address &&
        physical < mmio_address + mmio_size &&
        mmio_bus_->writeMmio(physical, width, value);
    mmio_accessed_ = mmio_accessed_ || claimed;
    return claimed;
}

bool R3000Runtime::read8(std::uint32_t address, std::uint8_t& value) const noexcept {
    std::uint32_t physical{};
    if (physicalAddress(address, physical) && physical < ram_mirror_end) {
        value = std::to_integer<std::uint8_t>(
            ram_[physical & static_cast<std::uint32_t>(ram_size - 1U)]);
        return true;
    }
    if (physical >= bios_address && physical < bios_address + bios_size) {
        // BIOS-less compatibility data: a zero-filled retail ROM view keeps
        // platform-detection reads deterministic without executing BIOS code.
        value = 0;
        return true;
    }
    std::uint32_t mmio_value{};
    if (readMmio(address, R3000AccessWidth::byte, mmio_value)) {
        value = static_cast<std::uint8_t>(mmio_value);
        return true;
    }
    const auto* source = memoryByte(address);
    if (source == nullptr) {
        return false;
    }
    value = std::to_integer<std::uint8_t>(*source);
    return true;
}

bool R3000Runtime::read16(std::uint32_t address, std::uint16_t& value) const noexcept {
    if ((address & 1U) != 0U) {
        return false;
    }
    std::uint32_t physical{};
    if (physicalAddress(address, physical) && physical < ram_mirror_end) {
        const auto offset =
            physical & static_cast<std::uint32_t>(ram_size - 1U);
        value = static_cast<std::uint16_t>(
            std::to_integer<std::uint8_t>(ram_[offset]) |
            (static_cast<std::uint16_t>(
                 std::to_integer<std::uint8_t>(ram_[offset + 1U]))
             << 8U));
        return true;
    }
    if (physical >= bios_address &&
        physical <= bios_address + bios_size - sizeof(std::uint16_t)) {
        value = 0;
        return true;
    }
    std::uint32_t mmio_value{};
    if (readMmio(address, R3000AccessWidth::halfword, mmio_value)) {
        value = static_cast<std::uint16_t>(mmio_value);
        return true;
    }
    const auto* byte0 = memoryByte(address);
    const auto* byte1 = memoryByte(address + 1U);
    if (byte0 == nullptr || byte1 == nullptr) {
        return false;
    }
    value = std::to_integer<std::uint8_t>(*byte0) |
        static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(*byte1) << 8U);
    return true;
}

bool R3000Runtime::read32(std::uint32_t address, std::uint32_t& value) const noexcept {
    if ((address & 3U) != 0U) {
        return false;
    }
    std::uint32_t physical{};
    if (physicalAddress(address, physical) && physical < ram_mirror_end) {
        const auto offset =
            physical & static_cast<std::uint32_t>(ram_size - 1U);
        value = std::to_integer<std::uint8_t>(ram_[offset]) |
            (static_cast<std::uint32_t>(
                 std::to_integer<std::uint8_t>(ram_[offset + 1U]))
             << 8U) |
            (static_cast<std::uint32_t>(
                 std::to_integer<std::uint8_t>(ram_[offset + 2U]))
             << 16U) |
            (static_cast<std::uint32_t>(
                 std::to_integer<std::uint8_t>(ram_[offset + 3U]))
             << 24U);
        return true;
    }
    if (physical >= bios_address &&
        physical <= bios_address + bios_size - sizeof(std::uint32_t)) {
        value = 0;
        return true;
    }
    std::uint32_t mmio_value{};
    if (readMmio(address, R3000AccessWidth::word, mmio_value)) {
        value = mmio_value;
        return true;
    }
    const auto* byte0 = memoryByte(address);
    const auto* byte1 = memoryByte(address + 1U);
    const auto* byte2 = memoryByte(address + 2U);
    const auto* byte3 = memoryByte(address + 3U);
    if (byte0 == nullptr || byte1 == nullptr || byte2 == nullptr || byte3 == nullptr) {
        return false;
    }
    value = std::to_integer<std::uint8_t>(*byte0) |
        (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(*byte1)) << 8U) |
        (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(*byte2)) << 16U) |
        (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(*byte3)) << 24U);
    return true;
}

bool R3000Runtime::write8(std::uint32_t address, std::uint8_t value) noexcept {
    std::uint32_t physical{};
    if (physicalAddress(address, physical) && physical < ram_mirror_end) {
        ram_[physical & static_cast<std::uint32_t>(ram_size - 1U)] =
            static_cast<std::byte>(value);
        noteRamWrite(physical, 1U);
        if (memory_write_sink_) {
            memory_write_sink_(address, 1U, value, state_.pc);
        }
        return true;
    }
    if (writeMmio(address, R3000AccessWidth::byte, value)) {
        return true;
    }
    auto* destination = memoryByte(address);
    if (destination == nullptr) {
        return false;
    }
    *destination = static_cast<std::byte>(value);
    if (memory_write_sink_) {
        memory_write_sink_(address, 1U, value, state_.pc);
    }
    return true;
}

bool R3000Runtime::write16(std::uint32_t address, std::uint16_t value) noexcept {
    if ((address & 1U) != 0U) {
        return false;
    }
    std::uint32_t physical{};
    if (physicalAddress(address, physical) && physical < ram_mirror_end) {
        const auto offset =
            physical & static_cast<std::uint32_t>(ram_size - 1U);
        ram_[offset] = static_cast<std::byte>(value);
        ram_[offset + 1U] = static_cast<std::byte>(value >> 8U);
        noteRamWrite(physical, 2U);
        if (memory_write_sink_) {
            memory_write_sink_(address, 2U, value, state_.pc);
        }
        return true;
    }
    if (writeMmio(address, R3000AccessWidth::halfword, value)) {
        return true;
    }
    auto* byte0 = memoryByte(address);
    auto* byte1 = memoryByte(address + 1U);
    if (byte0 == nullptr || byte1 == nullptr) {
        return false;
    }
    *byte0 = static_cast<std::byte>(value);
    *byte1 = static_cast<std::byte>(value >> 8U);
    if (memory_write_sink_) {
        memory_write_sink_(address, 2U, value, state_.pc);
    }
    return true;
}

bool R3000Runtime::write32(std::uint32_t address, std::uint32_t value) noexcept {
    if ((address & 3U) != 0U) {
        return false;
    }
    std::uint32_t physical{};
    if (physicalAddress(address, physical) && physical < ram_mirror_end) {
        const auto offset =
            physical & static_cast<std::uint32_t>(ram_size - 1U);
        ram_[offset] = static_cast<std::byte>(value);
        ram_[offset + 1U] = static_cast<std::byte>(value >> 8U);
        ram_[offset + 2U] = static_cast<std::byte>(value >> 16U);
        ram_[offset + 3U] = static_cast<std::byte>(value >> 24U);
        noteRamWrite(physical, 4U);
        if (memory_write_sink_) {
            memory_write_sink_(address, 4U, value, state_.pc);
        }
        return true;
    }
    if (writeMmio(address, R3000AccessWidth::word, value)) {
        return true;
    }
    auto* byte0 = memoryByte(address);
    auto* byte1 = memoryByte(address + 1U);
    auto* byte2 = memoryByte(address + 2U);
    auto* byte3 = memoryByte(address + 3U);
    if (byte0 == nullptr || byte1 == nullptr || byte2 == nullptr || byte3 == nullptr) {
        return false;
    }
    *byte0 = static_cast<std::byte>(value);
    *byte1 = static_cast<std::byte>(value >> 8U);
    *byte2 = static_cast<std::byte>(value >> 16U);
    *byte3 = static_cast<std::byte>(value >> 24U);
    if (memory_write_sink_) {
        memory_write_sink_(address, 4U, value, state_.pc);
    }
    return true;
}

void R3000Runtime::writeRegister(std::uint8_t reg, std::uint32_t value) noexcept {
    if (reg == 0U) {
        return;
    }
    state_.gpr[reg] = value;
    if (state_.load_delay.valid && state_.load_delay.reg == reg) {
        state_.load_delay.valid = false;
    }
}

void R3000Runtime::scheduleLoad(std::uint8_t reg, std::uint32_t value) noexcept {
    if (reg == 0U) {
        return;
    }
    if (state_.load_delay.valid && state_.load_delay.reg == reg) {
        state_.load_delay.valid = false;
    }
    state_.next_load_delay = R3000DelayedLoadState{reg, value, true};
}

void R3000Runtime::advanceLoadDelay() noexcept {
    if (state_.load_delay.valid && state_.load_delay.reg != 0U) {
        state_.gpr[state_.load_delay.reg] = state_.load_delay.value;
    }
    state_.load_delay = state_.next_load_delay;
    state_.next_load_delay = {};
    state_.gpr[0] = 0U;
}

void R3000Runtime::flushLoadDelay() noexcept {
    advanceLoadDelay();
    advanceLoadDelay();
    clearLoadDelay();
}

void R3000Runtime::clearLoadDelay() noexcept {
    state_.load_delay = {};
    state_.next_load_delay = {};
}

void R3000Runtime::setExternalInterrupt(bool active) noexcept {
    constexpr std::uint32_t hardware_interrupt_bit = 1U << 10U;
    if (active) {
        state_.cop0_cause |= hardware_interrupt_bit;
    } else {
        state_.cop0_cause &= ~hardware_interrupt_bit;
    }
}

void R3000Runtime::setRetimeHooks(game::RetimeHooks* hooks) noexcept {
    retime_hooks_ = hooks;
    pending_retime_hook_ = nullptr;
    pending_retime_hook_site_ = 0U;
}

bool R3000Runtime::interruptPending() const noexcept {
    constexpr std::uint32_t interrupt_enable_current = 1U;
    constexpr std::uint32_t interrupt_mask = 0x0000ff00U;
    return (state_.cop0_status & interrupt_enable_current) != 0U &&
        (state_.cop0_status & state_.cop0_cause & interrupt_mask) != 0U;
}

void R3000Runtime::takeInterrupt() noexcept {
    constexpr std::uint32_t bootstrap_exception_vector = 1U << 22U;
    constexpr std::uint32_t branch_delay_bit = 1U << 31U;
    constexpr std::uint32_t exception_code_mask = 0x0000007cU;
    constexpr std::uint32_t mode_stack_mask = 0x0000003fU;

    const auto interrupted_pc = state_.pc;
    state_.cop0_epc = state_.branch_delay_slot ? state_.branch_pc : interrupted_pc;
    state_.cop0_cause &= ~(branch_delay_bit | exception_code_mask);
    if (state_.branch_delay_slot) {
        state_.cop0_cause |= branch_delay_bit;
    }
    state_.cop0_status = (state_.cop0_status & ~mode_stack_mask) |
        ((state_.cop0_status << 2U) & mode_stack_mask);
    const auto vector = (state_.cop0_status & bootstrap_exception_vector) != 0U
        ? 0xbfc00180U
        : 0x80000080U;
    flushLoadDelay();
    state_.pc = vector;
    state_.next_pc = vector + 4U;
    state_.branch_pc = 0U;
    state_.branch_delay_slot = false;
}

bool R3000Runtime::prepareInstruction(R3000RunResult& result) noexcept {
    const auto instruction_pc = state_.pc;
    // A pending hook's delay-slot step was interrupted, faulted, or diverted:
    // drop it rather than fire it at the wrong instruction.
    if (pending_retime_hook_ != nullptr &&
        instruction_pc != pending_retime_hook_pc_) {
        pending_retime_hook_ = nullptr;
        pending_retime_hook_site_ = 0U;
    }
    if (interruptPending()) {
        takeInterrupt();
        result = {R3000StopReason::running, 0U, instruction_pc, 0U};
        return false;
    }
    // A host retime hook at this PC. The site becomes a virtual branch whose
    // delay slot is the retail instruction at `pc + 4`; the next step executes
    // that instruction and then runs the hook, resuming at the PC its `fn`
    // returns (normally the hook's rejoin).
    if (pending_retime_hook_ == nullptr && retime_hooks_ != nullptr &&
        retime_hooks_->active()) {
        if (const auto* hook = retime_hooks_->find(instruction_pc)) {
            // Settle any load delay from the instruction *before* the site, as
            // executing the site instruction would: a load at pc-4 only becomes
            // usable after one more instruction, and here that instruction is
            // the site the hook stands in for. Without this, the site's delay
            // slot (pc+4) reads the still-stale register — e.g. `Think__5Stack`
            // loads `$v0` at pc-4 and dereferences it at pc+4 to set `$s0`, and
            // a stale `$v0` yields a wild `$s0` and a memory fault downstream.
            advanceLoadDelay();
            state_.branch_pc = instruction_pc;
            state_.branch_delay_slot = true;
            state_.pc = instruction_pc + 4U;
            state_.next_pc = instruction_pc + 8U;
            pending_retime_hook_ = hook;
            pending_retime_hook_site_ = hook->pc;
            pending_retime_hook_pc_ = instruction_pc + 4U;
            result = {R3000StopReason::running, 1U, instruction_pc, 0U};
            return false;
        }
    }
    return true;
}

R3000RunResult R3000Runtime::step() noexcept {
    R3000RunResult prepared{};
    if (!prepareInstruction(prepared)) {
        return prepared;
    }
    const auto instruction_pc = state_.pc;
    std::uint32_t instruction{};
    if ((instruction_pc & 3U) != 0U) {
        return {R3000StopReason::alignment_fault, 0U, instruction_pc, 0U};
    }
    const auto fetch_physical = instruction_pc >= 0x80000000U &&
            instruction_pc < 0xc0000000U
        ? instruction_pc & physical_address_mask
        : instruction_pc;
    if (fetch_physical < ram_mirror_end) {
        const auto offset =
            fetch_physical & static_cast<std::uint32_t>(ram_size - 1U);
        instruction = std::to_integer<std::uint8_t>(ram_[offset]) |
            (static_cast<std::uint32_t>(
                 std::to_integer<std::uint8_t>(ram_[offset + 1U]))
             << 8U) |
            (static_cast<std::uint32_t>(
                 std::to_integer<std::uint8_t>(ram_[offset + 2U]))
             << 16U) |
            (static_cast<std::uint32_t>(
                 std::to_integer<std::uint8_t>(ram_[offset + 3U]))
             << 24U);
    } else if (!read32(instruction_pc, instruction)) {
        return {R3000StopReason::memory_fault, 0U, instruction_pc, 0U};
    }

    return executeDecodedInstruction(
        decodeInstruction(instruction_pc, instruction));
}

R3000Runtime::DecodedInstruction R3000Runtime::decodeInstruction(
    std::uint32_t pc,
    std::uint32_t instruction) noexcept {
    const auto opcode = static_cast<std::uint8_t>(instruction >> 26U);
    return DecodedInstruction{
        .pc = pc,
        .instruction = instruction,
        .immediate = instruction & 0xffffU,
        .opcode = opcode,
        .rs = static_cast<std::uint8_t>((instruction >> 21U) & 31U),
        .rt = static_cast<std::uint8_t>((instruction >> 16U) & 31U),
        .rd = static_cast<std::uint8_t>((instruction >> 11U) & 31U),
        .shift = static_cast<std::uint8_t>((instruction >> 6U) & 31U),
        .function = static_cast<std::uint8_t>(instruction & 63U),
        .may_write_memory = opcode == 0x28U || opcode == 0x29U ||
            opcode == 0x2aU || opcode == 0x2bU || opcode == 0x2eU ||
            opcode == 0x3aU,
    };
}

R3000RunResult R3000Runtime::executeDecodedInstruction(
    const DecodedInstruction& decoded) noexcept {
    const auto instruction_pc = decoded.pc;
    const auto instruction = decoded.instruction;

    const auto opcode = decoded.opcode;
    const auto rs = decoded.rs;
    const auto rt = decoded.rt;
    const auto rd = decoded.rd;
    const auto shift = decoded.shift;
    const auto function = decoded.function;
    const auto immediate = decoded.immediate;
    const auto left = state_.gpr[rs];
    const auto right = state_.gpr[rt];

    state_.branch_delay_slot = false;
    state_.pc = state_.next_pc;
    state_.next_pc += 4U;

    auto stop = R3000StopReason::running;
    auto memoryAddress = [&]() noexcept {
        return left + signExtend16(immediate);
    };
    auto branch = [&](bool taken) noexcept {
        state_.branch_pc = instruction_pc;
        state_.branch_delay_slot = true;
        if (taken) {
            state_.next_pc = instruction_pc + 4U + (signExtend16(immediate) << 2U);
        }
    };
    auto loadWord = [&](std::uint32_t address, std::uint32_t& value) noexcept {
        if ((address & 3U) != 0U) {
            stop = R3000StopReason::alignment_fault;
            return false;
        }
        if (!read32(address, value)) {
            stop = R3000StopReason::memory_fault;
            return false;
        }
        return true;
    };
    auto storeWord = [&](std::uint32_t address, std::uint32_t value) noexcept {
        if ((address & 3U) != 0U) {
            stop = R3000StopReason::alignment_fault;
            return false;
        }
        if (!write32(address, value)) {
            stop = R3000StopReason::memory_fault;
            return false;
        }
        return true;
    };

    switch (opcode) {
    case 0x00: {
        switch (function) {
        case 0x00: writeRegister(rd, right << shift); break;
        case 0x02: writeRegister(rd, right >> shift); break;
        case 0x03: writeRegister(rd, arithmeticShiftRight(right, shift)); break;
        case 0x04: writeRegister(rd, right << (left & 31U)); break;
        case 0x06: writeRegister(rd, right >> (left & 31U)); break;
        case 0x07: writeRegister(rd, arithmeticShiftRight(right, left)); break;
        case 0x08:
            state_.branch_pc = instruction_pc;
            state_.branch_delay_slot = true;
            state_.next_pc = left;
            break;
        case 0x09: {
            const auto target = left;
            writeRegister(rd, instruction_pc + 8U);
            state_.branch_pc = instruction_pc;
            state_.branch_delay_slot = true;
            state_.next_pc = target;
            break;
        }
        case 0x0c: stop = R3000StopReason::syscall; break;
        case 0x0d: stop = R3000StopReason::breakpoint; break;
        case 0x10: writeRegister(rd, state_.hi); break;
        case 0x11: state_.hi = left; break;
        case 0x12: writeRegister(rd, state_.lo); break;
        case 0x13: state_.lo = left; break;
        case 0x18: {
            const auto product = static_cast<std::int64_t>(asSigned(left)) *
                static_cast<std::int64_t>(asSigned(right));
            const auto bits = static_cast<std::uint64_t>(product);
            state_.lo = static_cast<std::uint32_t>(bits);
            state_.hi = static_cast<std::uint32_t>(bits >> 32U);
            break;
        }
        case 0x19: {
            const auto product = static_cast<std::uint64_t>(left) * right;
            state_.lo = static_cast<std::uint32_t>(product);
            state_.hi = static_cast<std::uint32_t>(product >> 32U);
            break;
        }
        case 0x1a: {
            const auto numerator = asSigned(left);
            const auto denominator = asSigned(right);
            if (denominator == 0) {
                state_.lo = numerator >= 0 ? 0xffffffffU : 1U;
                state_.hi = left;
            } else if (left == 0x80000000U && denominator == -1) {
                state_.lo = 0x80000000U;
                state_.hi = 0U;
            } else {
                state_.lo = static_cast<std::uint32_t>(numerator / denominator);
                state_.hi = static_cast<std::uint32_t>(numerator % denominator);
            }
            break;
        }
        case 0x1b:
            if (right == 0U) {
                state_.lo = 0xffffffffU;
                state_.hi = left;
            } else {
                state_.lo = left / right;
                state_.hi = left % right;
            }
            break;
        case 0x20: {
            std::uint32_t value{};
            if (addOverflows(left, right, value)) {
                stop = R3000StopReason::arithmetic_overflow;
            } else {
                writeRegister(rd, value);
            }
            break;
        }
        case 0x21: writeRegister(rd, left + right); break;
        case 0x22: {
            std::uint32_t value{};
            if (subtractOverflows(left, right, value)) {
                stop = R3000StopReason::arithmetic_overflow;
            } else {
                writeRegister(rd, value);
            }
            break;
        }
        case 0x23: writeRegister(rd, left - right); break;
        case 0x24: writeRegister(rd, left & right); break;
        case 0x25: writeRegister(rd, left | right); break;
        case 0x26: writeRegister(rd, left ^ right); break;
        case 0x27: writeRegister(rd, ~(left | right)); break;
        case 0x2a: writeRegister(rd, asSigned(left) < asSigned(right) ? 1U : 0U); break;
        case 0x2b: writeRegister(rd, left < right ? 1U : 0U); break;
        default: stop = R3000StopReason::unsupported_instruction; break;
        }
        break;
    }
    case 0x01: {
        const auto kind = rt;
        const auto greater_equal = (kind & 1U) != 0U;
        const auto link = (kind & 0x1eU) == 0x10U;
        if ((kind & 0x0eU) != 0U && !link) {
            stop = R3000StopReason::unsupported_instruction;
            break;
        }
        if (link) {
            writeRegister(31U, instruction_pc + 8U);
        }
        const auto is_negative = asSigned(left) < 0;
        branch(is_negative != greater_equal);
        break;
    }
    case 0x02:
        state_.branch_pc = instruction_pc;
        state_.branch_delay_slot = true;
        state_.next_pc = (state_.pc & 0xf0000000U) | ((instruction & 0x03ffffffU) << 2U);
        break;
    case 0x03:
        writeRegister(31U, instruction_pc + 8U);
        state_.branch_pc = instruction_pc;
        state_.branch_delay_slot = true;
        state_.next_pc = (state_.pc & 0xf0000000U) | ((instruction & 0x03ffffffU) << 2U);
        break;
    case 0x04: branch(left == right); break;
    case 0x05: branch(left != right); break;
    case 0x06: branch(asSigned(left) <= 0); break;
    case 0x07: branch(asSigned(left) > 0); break;
    case 0x08: {
        std::uint32_t value{};
        if (addOverflows(left, signExtend16(immediate), value)) {
            stop = R3000StopReason::arithmetic_overflow;
        } else {
            writeRegister(rt, value);
        }
        break;
    }
    case 0x09: writeRegister(rt, left + signExtend16(immediate)); break;
    case 0x0a:
        writeRegister(rt, asSigned(left) < asSigned(signExtend16(immediate)) ? 1U : 0U);
        break;
    case 0x0b: writeRegister(rt, left < signExtend16(immediate) ? 1U : 0U); break;
    case 0x0c: writeRegister(rt, left & immediate); break;
    case 0x0d: writeRegister(rt, left | immediate); break;
    case 0x0e: writeRegister(rt, left ^ immediate); break;
    case 0x0f: writeRegister(rt, immediate << 16U); break;
    case 0x10: {
        if (instruction == 0x42000010U) {
            state_.cop0_status = (state_.cop0_status & ~0x0fU) |
                ((state_.cop0_status >> 2U) & 0x0fU);
            break;
        }
        if ((instruction & 0x7ffU) != 0U) {
            stop = R3000StopReason::unsupported_instruction;
            break;
        }
        if (rs == 0U) {
            std::uint32_t value{};
            switch (rd) {
            case 8U: value = state_.cop0_bad_vaddr; break;
            case 12U: value = state_.cop0_status; break;
            case 13U: value = state_.cop0_cause; break;
            case 14U: value = state_.cop0_epc; break;
            default: stop = R3000StopReason::unsupported_instruction; break;
            }
            if (stop == R3000StopReason::running) {
                scheduleLoad(rt, value);
            }
        } else if (rs == 4U) {
            switch (rd) {
            case 12U: state_.cop0_status = right; break;
            case 13U:
                state_.cop0_cause = (state_.cop0_cause & ~0x00000300U) |
                    (right & 0x00000300U);
                break;
            case 14U: state_.cop0_epc = right; break;
            default: stop = R3000StopReason::unsupported_instruction; break;
            }
        } else {
            stop = R3000StopReason::unsupported_instruction;
        }
        break;
    }
    case 0x12:
        if (rs == 0U) {
            scheduleLoad(rt, GteRuntime::readData(state_.gte, rd));
        } else if (rs == 2U) {
            scheduleLoad(rt, GteRuntime::readControl(state_.gte, rd));
        } else if (rs == 4U) {
            GteRuntime::writeData(state_.gte, rd, right);
        } else if (rs == 6U) {
            GteRuntime::writeControl(state_.gte, rd, right);
        } else if ((rs & 0x10U) != 0U) {
            if (!GteRuntime::executeCommand(state_.gte, instruction)) {
                stop = R3000StopReason::unsupported_instruction;
            }
        } else {
            stop = R3000StopReason::unsupported_instruction;
        }
        break;
    case 0x20:
    case 0x24: {
        std::uint8_t value{};
        if (!read8(memoryAddress(), value)) {
            stop = R3000StopReason::memory_fault;
        } else if (opcode == 0x20) {
            scheduleLoad(rt, static_cast<std::uint32_t>(
                static_cast<std::int32_t>(static_cast<std::int8_t>(value))));
        } else {
            scheduleLoad(rt, value);
        }
        break;
    }
    case 0x21:
    case 0x25: {
        const auto address = memoryAddress();
        if ((address & 1U) != 0U) {
            stop = R3000StopReason::alignment_fault;
            break;
        }
        std::uint16_t value{};
        if (!read16(address, value)) {
            stop = R3000StopReason::memory_fault;
        } else if (opcode == 0x21) {
            scheduleLoad(rt, static_cast<std::uint32_t>(
                static_cast<std::int32_t>(static_cast<std::int16_t>(value))));
        } else {
            scheduleLoad(rt, value);
        }
        break;
    }
    case 0x22:
    case 0x26: {
        const auto address = memoryAddress();
        std::uint32_t aligned_value{};
        if (!loadWord(address & ~3U, aligned_value)) {
            break;
        }
        const auto existing = state_.load_delay.valid && state_.load_delay.reg == rt
            ? state_.load_delay.value
            : right;
        const auto amount = (address & 3U) * 8U;
        std::uint32_t value{};
        if (opcode == 0x22) {
            const auto mask = 0x00ffffffU >> amount;
            value = (existing & mask) | (aligned_value << (24U - amount));
        } else {
            const auto mask = 0xffffff00U << (24U - amount);
            value = (existing & mask) | (aligned_value >> amount);
        }
        scheduleLoad(rt, value);
        break;
    }
    case 0x23: {
        std::uint32_t value{};
        if (loadWord(memoryAddress(), value)) {
            scheduleLoad(rt, value);
        }
        break;
    }
    case 0x28:
        if (!write8(memoryAddress(), static_cast<std::uint8_t>(right))) {
            stop = R3000StopReason::memory_fault;
        }
        break;
    case 0x29: {
        const auto address = memoryAddress();
        if ((address & 1U) != 0U) {
            stop = R3000StopReason::alignment_fault;
        } else if (!write16(address, static_cast<std::uint16_t>(right))) {
            stop = R3000StopReason::memory_fault;
        }
        break;
    }
    case 0x2a:
    case 0x2e: {
        const auto address = memoryAddress();
        const auto aligned_address = address & ~3U;
        std::uint32_t memory_value{};
        if (!loadWord(aligned_address, memory_value)) {
            break;
        }
        const auto amount = (address & 3U) * 8U;
        std::uint32_t value{};
        if (opcode == 0x2a) {
            const auto mask = 0xffffff00U << amount;
            value = (memory_value & mask) | (right >> (24U - amount));
        } else {
            const auto mask = 0x00ffffffU >> (24U - amount);
            value = (memory_value & mask) | (right << amount);
        }
        static_cast<void>(storeWord(aligned_address, value));
        break;
    }
    case 0x2b: static_cast<void>(storeWord(memoryAddress(), right)); break;
    case 0x32: {
        std::uint32_t value{};
        if (loadWord(memoryAddress(), value)) {
            GteRuntime::writeData(state_.gte, rt, value);
        }
        break;
    }
    case 0x3a:
        static_cast<void>(storeWord(memoryAddress(), GteRuntime::readData(state_.gte, rt)));
        break;
    default: stop = R3000StopReason::unsupported_instruction; break;
    }

    if (stop != R3000StopReason::running) {
        state_.next_load_delay = {};
        pending_retime_hook_ = nullptr;
        pending_retime_hook_site_ = 0U;
        advanceLoadDelay();
        return {stop, 0U, instruction_pc, instruction};
    }
    advanceLoadDelay();
    if (pending_retime_hook_ != nullptr) {
        const auto* hook = pending_retime_hook_;
        pending_retime_hook_ = nullptr;
        pending_retime_hook_site_ = 0U;
        const auto resume = hook->fn(
            state_, retime_hooks_->state(), *this, hook->rejoin, hook->context);
        state_.pc = resume;
        state_.next_pc = resume + 4U;
        state_.branch_pc = 0U;
        state_.branch_delay_slot = false;
    }
    return {R3000StopReason::running, 1U, instruction_pc, instruction};
}

void R3000Runtime::considerNativeCompilation(CompiledBlock& block) noexcept {
#if defined(_M_X64) || defined(__x86_64__)
    if (block.native != nullptr ||
        block.execution_count > native_compilation_threshold) {
        return;
    }
    if (block.execution_count < native_compilation_threshold) {
        ++block.execution_count;
        return;
    }
    ++block.execution_count;
    block.native = compileNativeBlock(block);
    if (block.native == nullptr) {
        return;
    }
    ++recompiler_stats_.native_blocks_compiled;
    for (const auto& region : block.native->regions) {
        recompiler_stats_.native_regions_compiled +=
            region.entry != nullptr ? 1U : 0U;
    }
#else
    static_cast<void>(block);
#endif
}

std::unique_ptr<R3000Runtime::NativeBlock> R3000Runtime::compileNativeBlock(
    const CompiledBlock& block) noexcept {
#if defined(_M_X64) || defined(__x86_64__)
    static_assert(std::is_standard_layout_v<R3000State>);
    static_assert(std::is_standard_layout_v<R3000DelayedLoadState>);
    constexpr auto gpr_offset = offsetof(R3000State, gpr);
    constexpr auto pc_offset = offsetof(R3000State, pc);
    constexpr auto next_pc_offset = offsetof(R3000State, next_pc);
    constexpr auto branch_pc_offset = offsetof(R3000State, branch_pc);
    constexpr auto branch_delay_slot_offset =
        offsetof(R3000State, branch_delay_slot);
    constexpr auto load_delay_offset = offsetof(R3000State, load_delay);
    constexpr auto load_reg_offset = load_delay_offset +
        offsetof(R3000DelayedLoadState, reg);
    constexpr auto load_value_offset = load_delay_offset +
        offsetof(R3000DelayedLoadState, value);
    constexpr auto load_valid_offset = load_delay_offset +
        offsetof(R3000DelayedLoadState, valid);

    X64Emitter emitter;
    std::array<std::size_t, maximum_compiled_block_instructions> offsets{};
    offsets.fill(std::numeric_limits<std::size_t>::max());
    std::array<std::uint8_t, maximum_compiled_block_instructions> lengths{};
    std::array<std::uint8_t, maximum_compiled_block_instructions>
        store_widths{};
    std::array<std::uint8_t, maximum_compiled_block_instructions>
        control_flow_instructions{};
    auto region_count = 0U;

    const auto& instructions = block.instructions;
    for (std::size_t start = 0U; start < instructions.size();) {
        auto past_last = start;
        while (past_last < instructions.size()) {
            const auto& decoded = instructions[past_last];
            if (nativeAluInstruction(decoded.opcode, decoded.function) ||
                nativeLoadInstruction(decoded.opcode)) {
                ++past_last;
                continue;
            }
            if (nativeStoreInstruction(decoded.opcode)) {
                // A direct store ends its native region. The host consumes
                // the returned address immediately, invalidates any touched
                // code page, and only then redispatches the following op.
                ++past_last;
                break;
            }
            if (nativeControlFlowInstruction(
                    decoded.opcode, decoded.rt, decoded.function) &&
                past_last + 1U < instructions.size()) {
                const auto& delay = instructions[past_last + 1U];
                if (nativeAluInstruction(delay.opcode, delay.function) ||
                    nativeLoadInstruction(delay.opcode) ||
                    nativeStoreInstruction(delay.opcode)) {
                    // The transfer and its architectural delay slot form one
                    // terminal native unit. Both outcomes return to the host
                    // at the selected guest PC for redispatch.
                    past_last += 2U;
                }
            }
            break;
        }

        const auto instruction_count = past_last - start;
        const auto isolated_store = instruction_count == 1U &&
            nativeStoreInstruction(instructions[start].opcode);
        if (instruction_count < 2U && !isolated_store) {
            ++start;
            continue;
        }

        std::array<std::uint16_t, 32U> frequency{};
        const auto count_register = [&](std::uint8_t reg) {
            if (reg != 0U) {
                ++frequency[reg];
            }
        };
        for (auto index = start; index < past_last; ++index) {
            const auto& decoded = instructions[index];
            if (decoded.opcode == 0x00U) {
                count_register(decoded.rt);
                if (decoded.function != 0x00U &&
                    decoded.function != 0x02U &&
                    decoded.function != 0x03U) {
                    count_register(decoded.rs);
                }
                count_register(decoded.rd);
            } else if (nativeLoadInstruction(decoded.opcode) ||
                       nativeStoreInstruction(decoded.opcode)) {
                count_register(decoded.rs);
                count_register(decoded.rt);
            } else {
                if (decoded.opcode != 0x0fU) {
                    count_register(decoded.rs);
                }
                count_register(decoded.rt);
            }
        }

        std::array<std::uint8_t, 2U> cached_guest{};
        for (std::size_t slot = 0U; slot < cached_guest.size(); ++slot) {
            for (std::uint8_t reg = 1U; reg < frequency.size(); ++reg) {
                if (frequency[reg] > frequency[cached_guest[slot]]) {
                    cached_guest[slot] = reg;
                }
            }
            frequency[cached_guest[slot]] = 0U;
        }
        const std::array cached_host{X64Register::r8, X64Register::r9};
        const auto cachedRegister = [&](std::uint8_t guest)
            -> std::optional<X64Register> {
            for (std::size_t slot = 0U; slot < cached_guest.size(); ++slot) {
                if (cached_guest[slot] == guest && guest != 0U) {
                    return cached_host[slot];
                }
            }
            return std::nullopt;
        };
        const auto registerDisplacement = [&](std::uint8_t reg) {
            return static_cast<std::int32_t>(
                gpr_offset + static_cast<std::size_t>(reg) *
                    sizeof(std::uint32_t));
        };
        const auto readGuest = [&](std::uint8_t guest, X64Register host) {
            if (guest == 0U) {
                emitter.xorRegReg(host, host);
            } else if (const auto cached = cachedRegister(guest)) {
                if (*cached != host) {
                    emitter.movRegReg(host, *cached);
                }
            } else {
                emitter.movRegMem(
                    host, native_state_register, registerDisplacement(guest));
            }
        };
        const auto writeGuest = [&](std::uint8_t guest, X64Register host) {
            if (guest == 0U) {
                return;
            }
            if (const auto cached = cachedRegister(guest)) {
                if (*cached != host) {
                    emitter.movRegReg(*cached, host);
                }
            } else {
                emitter.movMemReg(
                    native_state_register, registerDisplacement(guest), host);
            }
        };
        const auto spillCache = [&] {
            for (std::size_t slot = 0U; slot < cached_guest.size(); ++slot) {
                if (cached_guest[slot] != 0U) {
                    emitter.movMemReg(
                        native_state_register,
                        registerDisplacement(cached_guest[slot]),
                        cached_host[slot]);
                }
            }
        };
        const auto emitPc = [&](std::size_t executed) {
            emitter.movMemImm32(
                native_state_register, static_cast<std::int32_t>(pc_offset),
                instructions[start].pc +
                    static_cast<std::uint32_t>(executed * 4U));
            emitter.movMemImm32(
                native_state_register,
                static_cast<std::int32_t>(next_pc_offset),
                instructions[start].pc +
                    static_cast<std::uint32_t>((executed + 1U) * 4U));
        };

        offsets[start] = emitter.offset();
        lengths[start] = static_cast<std::uint8_t>(instruction_count);
        auto ends_in_control_flow = false;
        if (instruction_count >= 2U) {
            const auto& terminal_control = instructions[past_last - 2U];
            ends_in_control_flow = nativeControlFlowInstruction(
                terminal_control.opcode, terminal_control.rt,
                terminal_control.function);
        }
        if (ends_in_control_flow) {
            control_flow_instructions[start] =
                static_cast<std::uint8_t>(instruction_count - 1U);
        }
        ++region_count;
        emitter.endbr64();
        for (std::size_t slot = 0U; slot < cached_guest.size(); ++slot) {
            if (cached_guest[slot] != 0U) {
                emitter.movRegMem(
                    cached_host[slot], native_state_register,
                    registerDisplacement(cached_guest[slot]));
            }
        }

        struct MemorySideExit {
            std::vector<std::size_t> jumps;
            std::size_t instructions_executed{};
            std::uint8_t pending_load_reg{};
            bool preserve_branch_state{};
        };
        std::vector<MemorySideExit> memory_side_exits;
        const auto emitRamAddress = [&](const DecodedInstruction& decoded,
                                        std::uint32_t alignment,
                                        MemorySideExit& side_exit) {
            readGuest(decoded.rs, X64Register::r10);
            emitter.addRegImm(
                X64Register::r10, signExtend16(decoded.immediate));
            if (alignment != 0U) {
                emitter.testRegImm(X64Register::r10, alignment);
                side_exit.jumps.push_back(
                    emitter.jump(X64Emitter::Condition::not_equal));
            }

            emitter.movRegReg(X64Register::rax, X64Register::r10);
            emitter.cmpRegImm(X64Register::rax, 0x00800000U);
            const auto low_address =
                emitter.jump(X64Emitter::Condition::below);
            emitter.cmpRegImm(X64Register::rax, 0x80000000U);
            side_exit.jumps.push_back(
                emitter.jump(X64Emitter::Condition::below));
            emitter.cmpRegImm(X64Register::rax, 0xc0000000U);
            side_exit.jumps.push_back(
                emitter.jump(X64Emitter::Condition::above_equal));
            emitter.andRegImm(X64Register::r10, physical_address_mask);
            emitter.cmpRegImm(X64Register::r10, ram_mirror_end);
            side_exit.jumps.push_back(
                emitter.jump(X64Emitter::Condition::above_equal));
            emitter.patch(low_address, emitter.offset());
            emitter.andRegImm(
                X64Register::r10,
                static_cast<std::uint32_t>(ram_size - 1U));
        };
        std::uint8_t pending_load_reg{};
        for (auto index = start; index < past_last; ++index) {
            const auto& decoded = instructions[index];
            if (nativeLoadInstruction(decoded.opcode)) {
                memory_side_exits.push_back(MemorySideExit{
                    {}, index - start, pending_load_reg,
                    ends_in_control_flow && index == past_last - 1U});
                auto& side_exit = memory_side_exits.back();
                const auto alignment = decoded.opcode == 0x23U ? 3U :
                    (decoded.opcode == 0x21U || decoded.opcode == 0x25U
                         ? 1U : 0U);
                emitRamAddress(decoded, alignment, side_exit);

                const auto width = decoded.opcode == 0x20U ||
                        decoded.opcode == 0x24U
                    ? 1U
                    : (decoded.opcode == 0x21U || decoded.opcode == 0x25U
                           ? 2U : 4U);
                const auto sign_extend = decoded.opcode == 0x20U ||
                    decoded.opcode == 0x21U;
                emitter.loadRam(
                    X64Register::rax,
                    static_cast<std::uint8_t>(width), sign_extend);
                if (pending_load_reg != 0U &&
                    pending_load_reg != decoded.rt) {
                    writeGuest(pending_load_reg, X64Register::r11);
                }
                if (decoded.rt != 0U) {
                    emitter.movRegReg(X64Register::r11, X64Register::rax);
                }
                pending_load_reg = decoded.rt;
                continue;
            }

            if (nativeStoreInstruction(decoded.opcode)) {
                memory_side_exits.push_back(MemorySideExit{
                    {}, index - start, pending_load_reg,
                    ends_in_control_flow && index == past_last - 1U});
                auto& side_exit = memory_side_exits.back();
                const auto width = decoded.opcode == 0x28U ? 1U :
                    (decoded.opcode == 0x29U ? 2U : 4U);
                emitRamAddress(decoded, width - 1U, side_exit);
                // A store observes the pre-delay value when it names the load
                // destination, so read its source before committing r11.
                readGuest(decoded.rt, X64Register::rax);
                emitter.storeRam(
                    X64Register::rax, static_cast<std::uint8_t>(width));
                if (pending_load_reg != 0U) {
                    writeGuest(pending_load_reg, X64Register::r11);
                }
                pending_load_reg = 0U;
                store_widths[start] = static_cast<std::uint8_t>(width);
                continue;
            }

            if (nativeControlFlowInstruction(
                    decoded.opcode, decoded.rt, decoded.function)) {
                std::uint8_t link_register{};
                if (nativeConditionalBranch(decoded.opcode)) {
                    readGuest(decoded.rs, X64Register::rax);
                    X64Emitter::Condition condition{};
                    if (decoded.opcode == 0x01U) {
                        emitter.cmpRegImm(X64Register::rax, 0U);
                        condition = (decoded.rt & 1U) != 0U
                            ? X64Emitter::Condition::greater_equal_signed
                            : X64Emitter::Condition::less_signed;
                        if ((decoded.rt & 0x10U) != 0U) {
                            link_register = 31U;
                        }
                    } else if (decoded.opcode == 0x04U ||
                               decoded.opcode == 0x05U) {
                        readGuest(decoded.rt, X64Register::r10);
                        emitter.cmpRegReg(
                            X64Register::rax, X64Register::r10);
                        condition = decoded.opcode == 0x04U
                            ? X64Emitter::Condition::equal
                            : X64Emitter::Condition::not_equal;
                    } else {
                        emitter.cmpRegImm(X64Register::rax, 0U);
                        condition = decoded.opcode == 0x06U
                            ? X64Emitter::Condition::less_equal_signed
                            : X64Emitter::Condition::greater_signed;
                    }
                    const auto taken = emitter.jump(condition);
                    emitter.movRegImm(
                        X64Register::rax, decoded.pc + 8U);
                    const auto selected = emitter.jump();
                    emitter.patch(taken, emitter.offset());
                    emitter.movRegImm(
                        X64Register::rax,
                        decoded.pc + 4U +
                            (signExtend16(decoded.immediate) << 2U));
                    emitter.patch(selected, emitter.offset());
                } else if (decoded.opcode == 0x02U ||
                           decoded.opcode == 0x03U) {
                    emitter.movRegImm(
                        X64Register::rax,
                        ((decoded.pc + 4U) & 0xf0000000U) |
                            ((decoded.instruction & 0x03ffffffU) << 2U));
                    if (decoded.opcode == 0x03U) {
                        link_register = 31U;
                    }
                } else {
                    // JR/JALR capture the target before a link write, which
                    // matters for the legal `jalr $rs,$rs` form.
                    readGuest(decoded.rs, X64Register::rax);
                    if (decoded.function == 0x09U) {
                        link_register = decoded.rd;
                    }
                }

                // Materialize the state at entry to the architectural delay
                // slot. A guarded memory operation in that slot can return
                // here and let the portable executor retry with BD/EPC state
                // indistinguishable from the interpreter.
                emitter.movMemImm32(
                    native_state_register,
                    static_cast<std::int32_t>(pc_offset), decoded.pc + 4U);
                emitter.movMemReg(
                    native_state_register,
                    static_cast<std::int32_t>(next_pc_offset),
                    X64Register::rax);
                emitter.movMemImm32(
                    native_state_register,
                    static_cast<std::int32_t>(branch_pc_offset), decoded.pc);
                emitter.movMemImm8(
                    native_state_register,
                    static_cast<std::int32_t>(branch_delay_slot_offset), 1U);
                if (link_register != 0U) {
                    emitter.movRegImm(
                        X64Register::r10, decoded.pc + 8U);
                    writeGuest(link_register, X64Register::r10);
                }
                if (pending_load_reg != 0U &&
                    pending_load_reg != link_register) {
                    writeGuest(pending_load_reg, X64Register::r11);
                }
                pending_load_reg = 0U;
                continue;
            }

            if (decoded.opcode == 0x00U) {
                readGuest(decoded.rt, X64Register::rax);
                if (decoded.function == 0x00U) {
                    emitter.shiftLeft(X64Register::rax, decoded.shift);
                } else if (decoded.function == 0x02U) {
                    emitter.shiftRight(X64Register::rax, decoded.shift);
                } else if (decoded.function == 0x03U) {
                    emitter.shiftArithmeticRight(
                        X64Register::rax, decoded.shift);
                } else {
                    readGuest(decoded.rs, X64Register::r10);
                    switch (decoded.function) {
                    case 0x21U:
                        emitter.addRegReg(X64Register::r10, X64Register::rax);
                        emitter.movRegReg(X64Register::rax, X64Register::r10);
                        break;
                    case 0x23U:
                        emitter.subRegReg(X64Register::r10, X64Register::rax);
                        emitter.movRegReg(X64Register::rax, X64Register::r10);
                        break;
                    case 0x24U:
                        emitter.andRegReg(X64Register::rax, X64Register::r10);
                        break;
                    case 0x25U:
                        emitter.orRegReg(X64Register::rax, X64Register::r10);
                        break;
                    case 0x26U:
                        emitter.xorValueReg(X64Register::rax, X64Register::r10);
                        break;
                    case 0x27U:
                        emitter.orRegReg(X64Register::rax, X64Register::r10);
                        emitter.notReg(X64Register::rax);
                        break;
                    case 0x2aU:
                        emitter.cmpRegReg(X64Register::r10, X64Register::rax);
                        emitter.setLessSignedToEax();
                        break;
                    case 0x2bU:
                        emitter.cmpRegReg(X64Register::r10, X64Register::rax);
                        emitter.setLessUnsignedToEax();
                        break;
                    default: break;
                    }
                }
                writeGuest(decoded.rd, X64Register::rax);
                if (pending_load_reg != 0U &&
                    pending_load_reg != decoded.rd) {
                    writeGuest(pending_load_reg, X64Register::r11);
                }
                pending_load_reg = 0U;
                continue;
            }

            if (decoded.opcode == 0x0fU) {
                emitter.movRegImm(
                    X64Register::rax, decoded.immediate << 16U);
            } else {
                readGuest(decoded.rs, X64Register::rax);
                switch (decoded.opcode) {
                case 0x09U:
                    emitter.addRegImm(
                        X64Register::rax, signExtend16(decoded.immediate));
                    break;
                case 0x0aU:
                    emitter.cmpRegImm(
                        X64Register::rax, signExtend16(decoded.immediate));
                    emitter.setLessSignedToEax();
                    break;
                case 0x0bU:
                    emitter.cmpRegImm(
                        X64Register::rax, signExtend16(decoded.immediate));
                    emitter.setLessUnsignedToEax();
                    break;
                case 0x0cU:
                    emitter.andRegImm(X64Register::rax, decoded.immediate);
                    break;
                case 0x0dU:
                    emitter.orRegImm(X64Register::rax, decoded.immediate);
                    break;
                case 0x0eU:
                    emitter.xorRegImm(X64Register::rax, decoded.immediate);
                    break;
                default: break;
                }
            }
            writeGuest(decoded.rt, X64Register::rax);
            if (pending_load_reg != 0U &&
                pending_load_reg != decoded.rt) {
                writeGuest(pending_load_reg, X64Register::r11);
            }
            pending_load_reg = 0U;
        }

        spillCache();
        if (ends_in_control_flow) {
            // Completing the delay slot advances to the selected branch arm.
            // Keep branch_pc as the interpreter does, but clear the active
            // delay-slot marker before returning to host dispatch.
            emitter.movRegMem(
                X64Register::rax, native_state_register,
                static_cast<std::int32_t>(next_pc_offset));
            emitter.movMemReg(
                native_state_register, static_cast<std::int32_t>(pc_offset),
                X64Register::rax);
            emitter.addRegImm(X64Register::rax, 4U);
            emitter.movMemReg(
                native_state_register,
                static_cast<std::int32_t>(next_pc_offset),
                X64Register::rax);
            emitter.movMemImm8(
                native_state_register,
                static_cast<std::int32_t>(branch_delay_slot_offset), 0U);
        } else {
            emitPc(instruction_count);
        }
        if (pending_load_reg != 0U) {
            emitter.movMemImm8(
                native_state_register,
                static_cast<std::int32_t>(load_reg_offset), pending_load_reg);
            emitter.movMemReg(
                native_state_register,
                static_cast<std::int32_t>(load_value_offset),
                X64Register::r11);
            emitter.movMemImm8(
                native_state_register,
                static_cast<std::int32_t>(load_valid_offset), 1U);
        }
        if (store_widths[start] != 0U) {
            emitter.shiftLeft64(X64Register::r10, 32U);
            emitter.movRegImm(
                X64Register::rax,
                static_cast<std::uint32_t>(instruction_count));
            emitter.orRegReg64(X64Register::rax, X64Register::r10);
        } else {
            emitter.movRegImm(
                X64Register::rax,
                static_cast<std::uint32_t>(instruction_count));
        }
        emitter.ret();

        for (const auto& exit : memory_side_exits) {
            const auto side_exit_offset = emitter.offset();
            for (const auto jump : exit.jumps) {
                emitter.patch(jump, side_exit_offset);
            }
            spillCache();
            if (!exit.preserve_branch_state) {
                emitPc(exit.instructions_executed);
            }
            if (exit.pending_load_reg != 0U) {
                emitter.movMemImm8(
                    native_state_register,
                    static_cast<std::int32_t>(load_reg_offset),
                    exit.pending_load_reg);
                emitter.movMemReg(
                    native_state_register,
                    static_cast<std::int32_t>(load_value_offset),
                    X64Register::r11);
                emitter.movMemImm8(
                    native_state_register,
                    static_cast<std::int32_t>(load_valid_offset), 1U);
            }
            emitter.movRegImm(
                X64Register::rax,
                static_cast<std::uint32_t>(exit.instructions_executed));
            emitter.ret();
        }
        start = past_last;
    }

    if (region_count == 0U) {
        return {};
    }
    const auto bytes = std::as_bytes(std::span{emitter.code()});
    auto memory = platform::ExecutableMemory::create(bytes);
    if (memory == nullptr) {
        return {};
    }

    auto result = std::make_unique<NativeBlock>();
    result->code = std::move(memory);
    for (std::size_t index = 0U; index < offsets.size(); ++index) {
        if (offsets[index] == std::numeric_limits<std::size_t>::max()) {
            continue;
        }
        const auto* address = result->code->data() + offsets[index];
        NativeEntry entry{};
        static_assert(sizeof(entry) == sizeof(address));
        std::memcpy(&entry, &address, sizeof(entry));
        result->regions[index] =
            NativeRegion{entry, lengths[index], store_widths[index],
                         control_flow_instructions[index]};
    }
    return result;
#else
    static_cast<void>(block);
    return {};
#endif
}

bool R3000Runtime::canRunNativeRegion(
    const CompiledBlock& block,
    std::size_t instruction_index,
    std::uint64_t instruction_budget,
    std::uint64_t instructions_executed,
    const R3000ExecutionBoundaries& boundaries,
    bool execute_initial_boundary) const noexcept {
    if (block.native == nullptr ||
        instruction_index >= block.native->regions.size()) {
        return false;
    }
    const auto& region = block.native->regions[instruction_index];
    if (region.entry == nullptr || region.instruction_count == 0U ||
        region.instruction_count > instruction_budget - instructions_executed ||
        (region.store_width != 0U && memory_write_sink_) ||
        state_.branch_delay_slot || state_.next_pc != state_.pc + 4U ||
        state_.load_delay.valid || state_.next_load_delay.valid ||
        pending_retime_hook_ != nullptr || interruptPending()) {
        return false;
    }
    const auto hooks_active = retime_hooks_ != nullptr && retime_hooks_->active();
    for (std::size_t offset = 0U; offset < region.instruction_count; ++offset) {
        const auto pc = block.instructions[instruction_index + offset].pc;
        if (((!execute_initial_boundary || instructions_executed + offset != 0U) &&
             boundaries.contains(pc)) ||
            (hooks_active && retime_hooks_->find(pc) != nullptr)) {
            return false;
        }
    }
    return true;
}

bool R3000Runtime::compiledBlockValid(const CompiledBlock& block) const noexcept {
    for (std::size_t index = 0U; index < block.page_count; ++index) {
        const auto& page = block.pages[index];
        if (recompiler_page_generations_[page.index] != page.generation) {
            return false;
        }
    }
    return true;
}

void R3000Runtime::invalidateRecompilerCache() noexcept {
    if (!recompiler_cache_.empty()) {
        ++recompiler_stats_.cache_invalidations;
    }
    recompiler_cache_.clear();
    std::ranges::fill(recompiler_code_pages_, false);
    std::ranges::fill(recompiler_page_generations_, 0U);
}

void R3000Runtime::noteRamWrite(
    std::uint32_t physical_address,
    std::uint32_t size) noexcept {
    auto offset = physical_address & static_cast<std::uint32_t>(ram_size - 1U);
    auto remaining = size;
    while (remaining != 0U) {
        const auto page = static_cast<std::size_t>(offset / recompiler_page_size);
        if (recompiler_code_pages_[page]) {
            ++recompiler_page_generations_[page];
            ++recompiler_stats_.cache_invalidations;
        }
        const auto in_page = static_cast<std::uint32_t>(
            offset % recompiler_page_size);
        const auto chunk = std::min<std::uint32_t>(
            remaining,
            static_cast<std::uint32_t>(recompiler_page_size) - in_page);
        remaining -= chunk;
        offset = (offset + chunk) & static_cast<std::uint32_t>(ram_size - 1U);
    }
}

R3000Runtime::CompiledBlock* R3000Runtime::findOrCompileBlock(
    std::uint32_t pc) noexcept {
    if (const auto found = recompiler_cache_.find(pc);
        found != recompiler_cache_.end()) {
        if (compiledBlockValid(found->second)) {
            return &found->second;
        }
        recompiler_cache_.erase(found);
    }

    if ((pc & 3U) != 0U) {
        return nullptr;
    }
    const auto fetch_physical = pc >= 0x80000000U && pc < 0xc0000000U
        ? pc & physical_address_mask
        : pc;
    if (fetch_physical >= ram_mirror_end) {
        return nullptr;
    }

    CompiledBlock block;
    block.instructions.reserve(maximum_compiled_block_instructions);
    auto instruction_pc = pc;
    auto delay_slot_owed = false;
    for (std::size_t index = 0U;
         index < maximum_compiled_block_instructions;
         ++index) {
        const auto physical = instruction_pc >= 0x80000000U &&
                instruction_pc < 0xc0000000U
            ? instruction_pc & physical_address_mask
            : instruction_pc;
        if (physical >= ram_mirror_end) {
            break;
        }
        const auto offset =
            physical & static_cast<std::uint32_t>(ram_size - 1U);
        const auto page_index = static_cast<std::uint16_t>(
            offset / recompiler_page_size);
        if (block.page_count == 0U ||
            block.pages[block.page_count - 1U].index != page_index) {
            if (block.page_count == block.pages.size()) {
                break;
            }
            block.pages[block.page_count++] = CompiledCodePage{
                page_index, recompiler_page_generations_[page_index]};
        }
        const auto instruction =
            std::to_integer<std::uint8_t>(ram_[offset]) |
            (static_cast<std::uint32_t>(
                 std::to_integer<std::uint8_t>(ram_[offset + 1U]))
             << 8U) |
            (static_cast<std::uint32_t>(
                 std::to_integer<std::uint8_t>(ram_[offset + 2U]))
             << 16U) |
            (static_cast<std::uint32_t>(
                 std::to_integer<std::uint8_t>(ram_[offset + 3U]))
             << 24U);
        const auto decoded = decodeInstruction(instruction_pc, instruction);
        block.instructions.push_back(decoded);

        if (delay_slot_owed) {
            break;
        }
        const auto control_transfer =
            (decoded.opcode == 0x00U &&
             (decoded.function == 0x08U || decoded.function == 0x09U)) ||
            decoded.opcode == 0x01U ||
            (decoded.opcode >= 0x02U && decoded.opcode <= 0x07U);
        if (control_transfer) {
            delay_slot_owed = true;
        } else if (decoded.opcode == 0x00U &&
                   (decoded.function == 0x0cU ||
                    decoded.function == 0x0dU)) {
            break;
        }
        if (instruction_pc > std::numeric_limits<std::uint32_t>::max() - 4U) {
            break;
        }
        instruction_pc += 4U;
    }

    if (block.instructions.empty()) {
        return nullptr;
    }
    for (std::size_t index = 0U; index < block.page_count; ++index) {
        recompiler_code_pages_[block.pages[index].index] = true;
    }
    ++recompiler_stats_.blocks_compiled;
    const auto [inserted, unused] =
        recompiler_cache_.insert_or_assign(pc, std::move(block));
    static_cast<void>(unused);
    return &inserted->second;
}

R3000RunResult R3000Runtime::runInterpreterBatch(
    std::uint64_t instruction_budget,
    const R3000ExecutionBoundaries& boundaries,
    bool execute_initial_boundary) noexcept {
    R3000RunResult last{
        R3000StopReason::running, 0U, state_.pc, 0U};
    for (std::uint64_t count = 0U; count < instruction_budget; ++count) {
        if (atReturnSentinel() ||
            ((!execute_initial_boundary || count != 0U) &&
             boundaries.contains(state_.pc))) {
            last.instructions = count;
            last.pc = state_.pc;
            last.instruction = 0U;
            return last;
        }
        mmio_accessed_ = false;
        last = step();
        if (last.reason != R3000StopReason::running) {
            last.instructions = count + 1U;
            return last;
        }
        if (mmio_accessed_) {
            last.instructions = count + 1U;
            return last;
        }
    }
    last.instructions = instruction_budget;
    return last;
}

R3000RunResult R3000Runtime::runRecompiledBatch(
    std::uint64_t instruction_budget,
    const R3000ExecutionBoundaries& boundaries,
    bool execute_initial_boundary) noexcept {
    R3000RunResult last{
        R3000StopReason::running, 0U, state_.pc, 0U};
    std::uint64_t count{};
    while (count < instruction_budget) {
        if (atReturnSentinel() ||
            ((!execute_initial_boundary || count != 0U) &&
             boundaries.contains(state_.pc))) {
            last.instructions = count;
            last.pc = state_.pc;
            last.instruction = 0U;
            return last;
        }

        auto* block = findOrCompileBlock(state_.pc);
        if (block == nullptr) {
            mmio_accessed_ = false;
            last = step();
            ++count;
            if (last.reason != R3000StopReason::running || mmio_accessed_) {
                last.instructions = count;
                return last;
            }
            continue;
        }

        if (execution_backend_ == R3000ExecutionBackend::native_recompiler) {
            considerNativeCompilation(*block);
        }

        auto progressed = false;
        for (std::size_t index = 0U; index < block->instructions.size();) {
            const auto& decoded = block->instructions[index];
            if (count >= instruction_budget || state_.pc != decoded.pc) {
                break;
            }
            if ((!execute_initial_boundary || count != 0U) &&
                boundaries.contains(state_.pc)) {
                last.instructions = count;
                last.pc = state_.pc;
                last.instruction = 0U;
                return last;
            }

            if (execution_backend_ == R3000ExecutionBackend::native_recompiler &&
                canRunNativeRegion(
                    *block, index, instruction_budget, count,
                    boundaries, execute_initial_boundary)) {
                const auto& region = block->native->regions[index];
                mmio_accessed_ = false;
                const auto native_result =
                    region.entry(&state_, ram_.data());
                const auto executed =
                    static_cast<std::uint32_t>(native_result);
                if (executed != 0U && executed <= region.instruction_count) {
                    progressed = true;
                    count += executed;
                    recompiler_stats_.instructions_executed += executed;
                    recompiler_stats_.native_instructions_executed += executed;
                    recompiler_stats_.native_side_exits +=
                        executed != region.instruction_count ? 1U : 0U;
                    if (region.control_flow_instruction != 0U &&
                        executed >= region.control_flow_instruction) {
                        ++recompiler_stats_.native_control_flows_executed;
                    }
                    auto code_invalidated = false;
                    if (region.store_width != 0U &&
                        executed == region.instruction_count) {
                        const auto store_address = static_cast<std::uint32_t>(
                            native_result >> 32U);
                        noteRamWrite(store_address, region.store_width);
                        ++recompiler_stats_.native_stores_executed;
                        code_invalidated = !compiledBlockValid(*block);
                    }
                    const auto& final =
                        block->instructions[index + executed - 1U];
                    last = {R3000StopReason::running, 1U,
                            final.pc, final.instruction};
                    index += executed;
                    if (code_invalidated) {
                        break;
                    }
                    continue;
                }
            }

            progressed = true;
            mmio_accessed_ = false;
            R3000RunResult prepared{};
            if (!prepareInstruction(prepared)) {
                last = prepared;
            } else {
                if (execution_backend_ ==
                    R3000ExecutionBackend::native_recompiler) {
                    ++recompiler_stats_.native_fallback_opcodes[
                        decoded.opcode];
                    if (decoded.opcode == 0x00U) {
                        ++recompiler_stats_.native_fallback_functions[
                            decoded.function];
                    }
                }
                last = executeDecodedInstruction(decoded);
                ++recompiler_stats_.instructions_executed;
            }
            ++count;
            if (last.reason != R3000StopReason::running || mmio_accessed_) {
                last.instructions = count;
                return last;
            }
            if (decoded.may_write_memory && !compiledBlockValid(*block)) {
                break;
            }
            ++index;
        }
        if (!progressed) {
            // A state transition that did not enter this block must be
            // redispatched from its new PC. This guard also prevents a bad
            // cache entry from turning into an infinite host loop.
            recompiler_cache_.erase(state_.pc);
        }
    }
    last.instructions = instruction_budget;
    return last;
}

R3000RunResult R3000Runtime::runBatch(
    std::uint64_t instruction_budget,
    const R3000ExecutionBoundaries& boundaries,
    bool execute_initial_boundary) noexcept {
    if (execution_backend_ == R3000ExecutionBackend::interpreter) {
        return runInterpreterBatch(
            instruction_budget, boundaries, execute_initial_boundary);
    }
    return runRecompiledBatch(
        instruction_budget, boundaries, execute_initial_boundary);
}

R3000RunResult R3000Runtime::call(
    std::uint32_t address,
    std::span<const std::uint32_t> arguments,
    std::uint64_t instruction_budget) noexcept {
    if (!beginCall(address, arguments)) {
        return {R3000StopReason::memory_fault, 0U, address, 0U};
    }

    for (std::uint64_t count = 0; count < instruction_budget; ++count) {
        if (atReturnSentinel()) {
            settleLoadDelay();
            return {R3000StopReason::returned, count, state_.pc, 0U};
        }
        auto result = step();
        if (result.reason != R3000StopReason::running) {
            result.instructions = count;
            return result;
        }
    }
    if (atReturnSentinel()) {
        settleLoadDelay();
        return {
            R3000StopReason::returned,
            instruction_budget,
            state_.pc,
            0U,
        };
    }
    return {R3000StopReason::instruction_budget, instruction_budget, state_.pc, 0U};
}

} // namespace stuntmaster::psx
