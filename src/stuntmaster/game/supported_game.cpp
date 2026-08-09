#include "stuntmaster/game/supported_game.hpp"

namespace stuntmaster::game {
namespace {

constexpr SupportedGame supported_game{
    "Jackie Chan Stuntmaster",
    "USA / NTSC-U",
    "SLUS-00684",
    "SLUS-00684",
    "SLUS_006.84",
    {
        std::byte{0x5a}, std::byte{0xae}, std::byte{0x79}, std::byte{0xf0},
        std::byte{0xd6}, std::byte{0x03}, std::byte{0xbf}, std::byte{0x95},
        std::byte{0xbd}, std::byte{0xcf}, std::byte{0x9a}, std::byte{0x2c},
        std::byte{0x27}, std::byte{0x8d}, std::byte{0x18}, std::byte{0x91},
        std::byte{0xdc}, std::byte{0xbd}, std::byte{0x9e}, std::byte{0x83},
        std::byte{0x5a}, std::byte{0x59}, std::byte{0xd4}, std::byte{0x67},
        std::byte{0x3d}, std::byte{0x6e}, std::byte{0x6d}, std::byte{0xbd},
        std::byte{0x82}, std::byte{0x66}, std::byte{0x5c}, std::byte{0x64},
    },
};

} // namespace

const SupportedGame& ntscU() noexcept {
    return supported_game;
}

std::optional<SupportedGame> identify(
    std::string_view volume_id,
    std::string_view executable_path,
    const core::Sha256Digest& executable_sha256) noexcept {
    if (volume_id == supported_game.volume_id &&
        executable_path == supported_game.executable_path &&
        executable_sha256 == supported_game.executable_sha256) {
        return supported_game;
    }
    return std::nullopt;
}

} // namespace stuntmaster::game
