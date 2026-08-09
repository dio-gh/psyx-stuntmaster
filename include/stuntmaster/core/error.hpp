#pragma once

#include <stdexcept>
#include <string>

namespace stuntmaster::core {

class Error final : public std::runtime_error {
public:
    explicit Error(std::string message) : std::runtime_error(std::move(message)) {}
};

} // namespace stuntmaster::core

