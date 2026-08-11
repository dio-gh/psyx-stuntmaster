#include "stuntmaster/platform/executable_memory.hpp"

#include <cstring>
#include <limits>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace stuntmaster::platform {

std::unique_ptr<ExecutableMemory> ExecutableMemory::create(
    std::span<const std::byte> code) noexcept {
    if (code.empty()) {
        return {};
    }

#if defined(_WIN32)
    auto* address = VirtualAlloc(
        nullptr, code.size(), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (address == nullptr) {
        return {};
    }
    std::memcpy(address, code.data(), code.size());
    DWORD previous_protection{};
    if (VirtualProtect(
            address, code.size(), PAGE_EXECUTE_READ,
            &previous_protection) == FALSE) {
        VirtualFree(address, 0U, MEM_RELEASE);
        return {};
    }
    FlushInstructionCache(GetCurrentProcess(), address, code.size());
    return std::unique_ptr<ExecutableMemory>{
        new ExecutableMemory{address, code.size()}};
#elif defined(__unix__) || defined(__APPLE__)
    const auto page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0 ||
        code.size() > std::numeric_limits<std::size_t>::max() -
            static_cast<std::size_t>(page_size - 1)) {
        return {};
    }
    const auto allocation_size =
        (code.size() + static_cast<std::size_t>(page_size - 1)) /
        static_cast<std::size_t>(page_size) *
        static_cast<std::size_t>(page_size);
    auto* address = mmap(
        nullptr, allocation_size, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (address == MAP_FAILED) {
        return {};
    }
    std::memcpy(address, code.data(), code.size());
    if (mprotect(address, allocation_size, PROT_READ | PROT_EXEC) != 0) {
        munmap(address, allocation_size);
        return {};
    }
    __builtin___clear_cache(
        static_cast<char*>(address),
        static_cast<char*>(address) + code.size());
    return std::unique_ptr<ExecutableMemory>{
        new ExecutableMemory{address, allocation_size}};
#else
    static_cast<void>(code);
    return {};
#endif
}

ExecutableMemory::~ExecutableMemory() {
    if (address_ == nullptr) {
        return;
    }
#if defined(_WIN32)
    VirtualFree(address_, 0U, MEM_RELEASE);
#elif defined(__unix__) || defined(__APPLE__)
    munmap(address_, size_);
#endif
}

} // namespace stuntmaster::platform
