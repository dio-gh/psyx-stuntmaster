#pragma once

#include <cstddef>
#include <memory>
#include <span>

namespace stuntmaster::platform {

// Owns a write-once executable allocation. Creation copies through writable
// memory and seals the pages read/execute before returning; callers never hold
// a writable and executable mapping at the same time.
class ExecutableMemory final {
public:
    [[nodiscard]] static std::unique_ptr<ExecutableMemory> create(
        std::span<const std::byte> code) noexcept;

    ~ExecutableMemory();
    ExecutableMemory(const ExecutableMemory&) = delete;
    ExecutableMemory& operator=(const ExecutableMemory&) = delete;
    ExecutableMemory(ExecutableMemory&&) = delete;
    ExecutableMemory& operator=(ExecutableMemory&&) = delete;

    [[nodiscard]] const std::byte* data() const noexcept {
        return static_cast<const std::byte*>(address_);
    }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }

private:
    ExecutableMemory(void* address, std::size_t size) noexcept
        : address_(address), size_(size) {}

    void* address_{};
    std::size_t size_{};
};

} // namespace stuntmaster::platform
