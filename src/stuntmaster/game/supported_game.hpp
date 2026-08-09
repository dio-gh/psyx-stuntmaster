#pragma once

#include "stuntmaster/core/sha256.hpp"

#include <optional>
#include <string_view>

namespace stuntmaster::game {

struct SupportedGame {
    std::string_view title;
    std::string_view region;
    std::string_view serial;
    std::string_view volume_id;
    std::string_view executable_path;
    core::Sha256Digest executable_sha256;
};

[[nodiscard]] const SupportedGame& ntscU() noexcept;
[[nodiscard]] std::optional<SupportedGame> identify(
    std::string_view volume_id,
    std::string_view executable_path,
    const core::Sha256Digest& executable_sha256) noexcept;

} // namespace stuntmaster::game
