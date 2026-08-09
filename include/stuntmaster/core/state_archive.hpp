#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace stuntmaster::core {

// Small bounded binary archive used by versioned emulator save states. POD
// layout is intentionally part of the file version: a layout change bumps the
// enclosing save-state version rather than attempting schema-free migration.
class StateWriter final {
public:
    template <typename T>
    void pod(const T& value) {
        static_assert(std::is_trivially_copyable_v<T>);
        const auto* begin = reinterpret_cast<const std::byte*>(&value);
        bytes_.insert(bytes_.end(), begin, begin + sizeof(T));
    }

    void bytes(std::span<const std::byte> value) {
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }

    template <typename T>
    void vectorPod(const std::vector<T>& value) {
        static_assert(std::is_trivially_copyable_v<T>);
        pod(static_cast<std::uint64_t>(value.size()));
        bytes(std::as_bytes(std::span{value}));
    }

    void string(const std::string& value) {
        pod(static_cast<std::uint64_t>(value.size()));
        bytes(std::as_bytes(std::span{value}));
    }

    [[nodiscard]] const std::vector<std::byte>& data() const noexcept {
        return bytes_;
    }

private:
    std::vector<std::byte> bytes_;
};

class StateReader final {
public:
    explicit StateReader(std::span<const std::byte> bytes) noexcept
        : bytes_{bytes} {}

    template <typename T>
    bool pod(T& value) noexcept {
        static_assert(std::is_trivially_copyable_v<T>);
        if (!take(sizeof(T), reinterpret_cast<std::byte*>(&value))) {
            return false;
        }
        return true;
    }

    bool bytes(std::span<std::byte> value) noexcept {
        return take(value.size(), value.data());
    }

    template <typename T>
    bool vectorPod(
        std::vector<T>& value,
        std::uint64_t maximum_elements) {
        static_assert(std::is_trivially_copyable_v<T>);
        std::uint64_t count{};
        if (!pod(count) || count > maximum_elements ||
            count > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
            valid_ = false;
            return false;
        }
        value.resize(static_cast<std::size_t>(count));
        return bytes(std::as_writable_bytes(std::span{value}));
    }

    bool string(std::string& value, std::uint64_t maximum_bytes) {
        std::uint64_t count{};
        if (!pod(count) || count > maximum_bytes ||
            count > std::numeric_limits<std::size_t>::max()) {
            valid_ = false;
            return false;
        }
        value.resize(static_cast<std::size_t>(count));
        return bytes(std::as_writable_bytes(std::span{value}));
    }

    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] bool finished() const noexcept {
        return valid_ && offset_ == bytes_.size();
    }
    [[nodiscard]] std::size_t remaining() const noexcept {
        return valid_ ? bytes_.size() - offset_ : 0U;
    }

private:
    bool take(std::size_t count, std::byte* destination) noexcept {
        if (!valid_ || count > bytes_.size() - offset_) {
            valid_ = false;
            return false;
        }
        if (count != 0U) {
            std::memcpy(destination, bytes_.data() + offset_, count);
        }
        offset_ += count;
        return true;
    }

    std::span<const std::byte> bytes_;
    std::size_t offset_{};
    bool valid_{true};
};

} // namespace stuntmaster::core
