#include "stuntmaster/presentation/psycross_presenter.hpp"
#include "stuntmaster/presentation/display_scaling.hpp"
#include "stuntmaster/presentation/widescreen_overlay.hpp"
#include "stuntmaster/psx/gpu_polygon_limits.hpp"

#include "PsyX/PsyX_globals.h"
#include "PsyX/PsyX_public.h"
#include "PsyX/PsyX_render.h"
#include "psx/libgpu.h"
#include "psx/libpad.h"
#include <SDL.h>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

extern GrVertex g_vertexBuffer[];
extern int g_vertexIndex;
extern const unsigned char* g_sdlKeyboardState;
extern SDL_Window* g_window;
// Recreate the GL device against the current window size. PsyCross declares
// this only in its .cpp, so mirror the extern here; used once at construction
// to settle a borderless-desktop launch before the window is revealed.
extern void GR_ResetDevice();

namespace stuntmaster::presentation {
namespace {

std::string_view trim(std::string_view value) noexcept {
    constexpr std::string_view whitespace{" \t\r\n"};
    const auto first = value.find_first_not_of(whitespace);
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(whitespace);
    return value.substr(first, last - first + 1U);
}

void applyInputConfig(
    const std::filesystem::path& path,
    game::MouseControlConfig& mouse_config) {
    if (path.empty()) {
        return;
    }
    std::ifstream input{path};
    if (!input) {
        // Not fatal: the constructor already applied the built-in default
        // bindings. A missing input.ini (e.g. before it has been seeded, or if
        // the user deleted it) simply keeps those defaults.
        std::cerr << "input config not found, using defaults: "
                  << path.string() << '\n';
        return;
    }

    const std::array keyboard_bindings{
        std::pair{"keyboard.square", &g_cfg_keyboardMapping.kc_square},
        std::pair{"keyboard.circle", &g_cfg_keyboardMapping.kc_circle},
        std::pair{"keyboard.triangle", &g_cfg_keyboardMapping.kc_triangle},
        std::pair{"keyboard.cross", &g_cfg_keyboardMapping.kc_cross},
        std::pair{"keyboard.l1", &g_cfg_keyboardMapping.kc_l1},
        std::pair{"keyboard.l2", &g_cfg_keyboardMapping.kc_l2},
        std::pair{"keyboard.l3", &g_cfg_keyboardMapping.kc_l3},
        std::pair{"keyboard.r1", &g_cfg_keyboardMapping.kc_r1},
        std::pair{"keyboard.r2", &g_cfg_keyboardMapping.kc_r2},
        std::pair{"keyboard.r3", &g_cfg_keyboardMapping.kc_r3},
        std::pair{"keyboard.start", &g_cfg_keyboardMapping.kc_start},
        std::pair{"keyboard.select", &g_cfg_keyboardMapping.kc_select},
        std::pair{"keyboard.left", &g_cfg_keyboardMapping.kc_dpad_left},
        std::pair{"keyboard.right", &g_cfg_keyboardMapping.kc_dpad_right},
        std::pair{"keyboard.up", &g_cfg_keyboardMapping.kc_dpad_up},
        std::pair{"keyboard.down", &g_cfg_keyboardMapping.kc_dpad_down},
    };
    const std::array gamepad_bindings{
        std::pair{"gamepad.square", &g_cfg_controllerMapping.gc_square},
        std::pair{"gamepad.circle", &g_cfg_controllerMapping.gc_circle},
        std::pair{"gamepad.triangle", &g_cfg_controllerMapping.gc_triangle},
        std::pair{"gamepad.cross", &g_cfg_controllerMapping.gc_cross},
        std::pair{"gamepad.l1", &g_cfg_controllerMapping.gc_l1},
        std::pair{"gamepad.l2", &g_cfg_controllerMapping.gc_l2},
        std::pair{"gamepad.l3", &g_cfg_controllerMapping.gc_l3},
        std::pair{"gamepad.r1", &g_cfg_controllerMapping.gc_r1},
        std::pair{"gamepad.r2", &g_cfg_controllerMapping.gc_r2},
        std::pair{"gamepad.r3", &g_cfg_controllerMapping.gc_r3},
        std::pair{"gamepad.start", &g_cfg_controllerMapping.gc_start},
        std::pair{"gamepad.select", &g_cfg_controllerMapping.gc_select},
        std::pair{"gamepad.left", &g_cfg_controllerMapping.gc_dpad_left},
        std::pair{"gamepad.right", &g_cfg_controllerMapping.gc_dpad_right},
        std::pair{"gamepad.up", &g_cfg_controllerMapping.gc_dpad_up},
        std::pair{"gamepad.down", &g_cfg_controllerMapping.gc_dpad_down},
        std::pair{"gamepad.left_x", &g_cfg_controllerMapping.gc_axis_left_x},
        std::pair{"gamepad.left_y", &g_cfg_controllerMapping.gc_axis_left_y},
        std::pair{"gamepad.right_x", &g_cfg_controllerMapping.gc_axis_right_x},
        std::pair{"gamepad.right_y", &g_cfg_controllerMapping.gc_axis_right_y},
    };
    const std::array mouse_bindings{
        std::pair{
            "mouse.status",
            &mouse_config.actions[static_cast<std::size_t>(
                game::PrimaryAction::status)]},
        std::pair{
            "mouse.strafe",
            &mouse_config.actions[static_cast<std::size_t>(
                game::PrimaryAction::strafe)]},
        std::pair{
            "mouse.counter",
            &mouse_config.actions[static_cast<std::size_t>(
                game::PrimaryAction::counter)]},
        std::pair{
            "mouse.dive_roll",
            &mouse_config.actions[static_cast<std::size_t>(
                game::PrimaryAction::dive_roll)]},
        std::pair{
            "mouse.kick",
            &mouse_config.actions[static_cast<std::size_t>(
                game::PrimaryAction::kick)]},
        std::pair{
            "mouse.grab",
            &mouse_config.actions[static_cast<std::size_t>(
                game::PrimaryAction::grab)]},
        std::pair{
            "mouse.jump",
            &mouse_config.actions[static_cast<std::size_t>(
                game::PrimaryAction::jump)]},
        std::pair{
            "mouse.punch",
            &mouse_config.actions[static_cast<std::size_t>(
                game::PrimaryAction::punch)]},
    };

    std::string line;
    std::size_t line_number = 0U;
    while (std::getline(input, line)) {
        ++line_number;
        if (const auto comment = line.find('#');
            comment != std::string::npos) {
            line.erase(comment);
        }
        const auto content = trim(line);
        if (content.empty()) {
            continue;
        }
        const auto equals = content.find('=');
        if (equals == std::string_view::npos) {
            throw std::runtime_error{
                "invalid input configuration line " +
                std::to_string(line_number)};
        }
        const auto key = trim(content.substr(0U, equals));
        const auto value = trim(content.substr(equals + 1U));
        bool matched = false;
        constexpr int invalid_mapping = -0x40000000;
        for (const auto& [name, destination] : keyboard_bindings) {
            if (key == name) {
                const auto mapping = PsyX_LookupKeyboardMapping(
                    std::string{value}.c_str(), invalid_mapping);
                if (mapping == invalid_mapping) {
                    throw std::runtime_error{
                        "invalid keyboard binding on line " +
                        std::to_string(line_number)};
                }
                *destination = mapping;
                matched = true;
                break;
            }
        }
        if (!matched) {
            for (const auto& [name, destination] : gamepad_bindings) {
                if (key == name) {
                    const auto mapping = PsyX_LookupGameControllerMapping(
                        std::string{value}.c_str(), invalid_mapping);
                    if (mapping == invalid_mapping) {
                        throw std::runtime_error{
                            "invalid gamepad binding on line " +
                            std::to_string(line_number)};
                    }
                    *destination = mapping;
                    matched = true;
                    break;
                }
            }
        }
        if (!matched) {
            for (const auto& [name, destination] : mouse_bindings) {
                if (key == name) {
                    const auto button = game::parseMouseButton(value);
                    if (!button) {
                        throw std::runtime_error{
                            "invalid mouse binding on line " +
                            std::to_string(line_number)};
                    }
                    *destination = *button;
                    matched = true;
                    break;
                }
            }
        }
        if (!matched && key == "mouse.movement_mode") {
            const auto mode = game::parseMouseMovementMode(value);
            if (!mode) {
                throw std::runtime_error{
                    "invalid mouse movement mode on line " +
                    std::to_string(line_number)};
            }
            mouse_config.initial_mode = *mode;
            matched = true;
        }
        if (!matched && key == "mouse.sensitivity") {
            std::int32_t parsed = 0;
            const auto result = std::from_chars(
                value.data(), value.data() + value.size(), parsed);
            if (result.ec != std::errc{} ||
                result.ptr != value.data() + value.size() || parsed < 1 ||
                parsed > 1'000) {
                throw std::runtime_error{
                    "invalid mouse sensitivity on line " +
                    std::to_string(line_number)};
            }
            mouse_config.yaw_units_per_pixel = parsed;
            matched = true;
        }
        if (!matched) {
            throw std::runtime_error{
                "unknown input binding on line " +
                std::to_string(line_number)};
        }
    }
}

bool replayable(std::uint8_t opcode) noexcept {
    return (opcode >= 0x20U && opcode <= 0x7FU) ||
        (opcode >= 0xE0U && opcode <= 0xE5U);
}

std::uint32_t transferExtent(std::uint32_t raw, std::uint32_t mask) noexcept {
    return ((raw - 1U) & mask) + 1U;
}

constexpr std::uint16_t decodePsyCrossPadButtons(
    std::uint8_t native_low,
    std::uint8_t native_high) noexcept {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(native_low) |
        static_cast<std::uint16_t>(native_high) << 8U);
}

// PsyCross writes PADRAW::buttons through a native-endian u_short. Guard the
// byte ordering explicitly: a Circle press is DFFF and Right is FFDF.
static_assert(decodePsyCrossPadButtons(0xFFU, 0xDFU) == 0xDFFFU);
static_assert(decodePsyCrossPadButtons(0xDFU, 0xFFU) == 0xFFDFU);

[[nodiscard]] int controllerMappingValue(
    SDL_GameController* controller,
    int mapping) noexcept {
    if (controller == nullptr || mapping < 0) {
        return 0;
    }
    if ((mapping & CONTROLLER_MAP_FLAG_AXIS) != 0) {
        const auto axis = mapping &
            ~(CONTROLLER_MAP_FLAG_AXIS | CONTROLLER_MAP_FLAG_INVERSE);
        if (axis < 0 || axis >= SDL_CONTROLLER_AXIS_MAX) {
            return 0;
        }
        auto value = static_cast<int>(SDL_GameControllerGetAxis(
            controller, static_cast<SDL_GameControllerAxis>(axis)));
        if ((mapping & CONTROLLER_MAP_FLAG_INVERSE) != 0) {
            value = -value;
        }
        return value;
    }
    if (mapping >= SDL_CONTROLLER_BUTTON_MAX) {
        return 0;
    }
    return SDL_GameControllerGetButton(
               controller, static_cast<SDL_GameControllerButton>(mapping)) != 0
        ? 32'767
        : 0;
}

[[nodiscard]] std::array<std::int16_t, 2U> shapeControllerStick(
    int raw_x,
    int raw_y) noexcept {
    constexpr double axis_max = 32'767.0;
    constexpr double deadzone = axis_max * 0.18;
    const auto x = static_cast<double>(raw_x);
    const auto y = static_cast<double>(raw_y);
    const auto magnitude = std::hypot(x, y);
    if (magnitude <= deadzone) {
        return {};
    }
    const auto shaped = std::min(
        1.0, (magnitude - deadzone) / (axis_max - deadzone));
    const auto scale = shaped * axis_max / magnitude;
    return {
        static_cast<std::int16_t>(std::clamp(
            std::lround(x * scale), -32'767L, 32'767L)),
        static_cast<std::int16_t>(std::clamp(
            std::lround(y * scale), -32'767L, 32'767L)),
    };
}

[[nodiscard]] std::int16_t shapeControllerTrigger(int raw) noexcept {
    constexpr int deadzone = 2'048;
    raw = std::clamp(raw, 0, 32'767);
    if (raw <= deadzone) {
        return 0;
    }
    return static_cast<std::int16_t>(
        (raw - deadzone) * 32'767 / (32'767 - deadzone));
}

std::int32_t signed11(std::uint32_t value) noexcept {
    value &= 0x7FFU;
    if ((value & 0x400U) != 0U) {
        value |= 0xFFFFF800U;
    }
    return std::bit_cast<std::int32_t>(value);
}

// A packed pair of primitive scalars, in whichever representation this
// PsyCross build uses. Not only positions: `TILE` and `SPRT` declare their `w`
// and `h` as `VERTTYPE` too, so a variable-size rectangle's *size* is
// half-float in a PGXP build. Left as the integers the GP0 stream carries,
// they read back as subnormals, all four corners collapse onto one point, and
// the rectangle rasterizes to nothing.
[[nodiscard]] std::uint32_t packPrimitivePair(float low, float high) noexcept {
#if USE_PGXP
    return static_cast<std::uint16_t>(to_half_float(low)) |
        (static_cast<std::uint32_t>(
             static_cast<std::uint16_t>(to_half_float(high)))
         << 16U);
#else
    return static_cast<std::uint16_t>(static_cast<std::int16_t>(low)) |
        (static_cast<std::uint32_t>(static_cast<std::uint16_t>(
             static_cast<std::int16_t>(high)))
         << 16U);
#endif
}

struct ScreenPoint {
    float x{};
    float y{};
};

[[nodiscard]] ScreenPoint translatedPosition(
    std::uint32_t word,
    std::optional<std::uint32_t> previous_word,
    float interpolation_alpha,
    std::int32_t offset_x,
    std::int32_t offset_y,
    std::int32_t origin_x,
    std::int32_t origin_y) noexcept {
    const auto x = signed11(word);
    const auto y = signed11(word >> 16U);
    const auto current_x =
        static_cast<float>(x + offset_x - origin_x);
    const auto current_y =
        static_cast<float>(y + offset_y - origin_y);
    ScreenPoint result{current_x, current_y};
    if (previous_word.has_value()) {
        const auto previous_x = static_cast<float>(
            signed11(*previous_word) + offset_x - origin_x);
        const auto previous_y = static_cast<float>(
            signed11(*previous_word >> 16U) + offset_y - origin_y);
        result.x =
            previous_x + (current_x - previous_x) * interpolation_alpha;
        result.y =
            previous_y + (current_y - previous_y) * interpolation_alpha;
    }
    return result;
}

void packPosition(std::uint32_t& word, ScreenPoint point) noexcept {
#if USE_PGXP
    word = static_cast<std::uint16_t>(
               to_half_float(point.x)) |
        (static_cast<std::uint32_t>(static_cast<std::uint16_t>(
             to_half_float(point.y)))
         << 16U);
#else
    word = static_cast<std::uint16_t>(
               static_cast<std::int16_t>(point.x)) |
        (static_cast<std::uint32_t>(static_cast<std::uint16_t>(
             static_cast<std::int16_t>(point.y)))
         << 16U);
#endif
}

void translatePosition(
    std::uint32_t& word,
    std::optional<std::uint32_t> previous_word,
    float interpolation_alpha,
    std::int32_t offset_x,
    std::int32_t offset_y,
    std::int32_t origin_x,
    std::int32_t origin_y) noexcept {
    packPosition(
        word,
        translatedPosition(
            word,
            previous_word,
            interpolation_alpha,
            offset_x,
            offset_y,
            origin_x,
            origin_y));
}

void translatePrimitive(
    std::vector<std::uint32_t>& packet,
    std::uint8_t opcode,
    const std::vector<std::uint32_t>* previous_packet,
    std::span<const std::uint64_t> previous_positions,
    float interpolation_alpha,
    std::int32_t offset_x,
    std::int32_t offset_y,
    std::int32_t origin_x,
    std::int32_t origin_y,
    bool convert_rectangle_sizes) {
    const auto previousPosition = [&](std::size_t index)
        -> std::optional<std::uint32_t> {
        if (!previous_positions.empty()) {
            if (index >= previous_positions.size() ||
                previous_positions[index] ==
                    std::numeric_limits<std::uint64_t>::max()) {
                return std::nullopt;
            }
            return static_cast<std::uint32_t>(previous_positions[index]);
        }
        if (previous_packet != nullptr && index < previous_packet->size()) {
            return (*previous_packet)[index];
        }
        return std::nullopt;
    };
    if (opcode >= 0x20U && opcode <= 0x3FU) {
        const auto vertices = (opcode & 0x08U) != 0U ? 4U : 3U;
        const auto textured = (opcode & 0x04U) != 0U;
        const auto gouraud = (opcode & 0x10U) != 0U;
        std::size_t index = 1U;
        for (std::uint32_t vertex = 0U; vertex < vertices; ++vertex) {
            if (vertex != 0U && gouraud) {
                ++index;
            }
            if (index >= packet.size()) {
                return;
            }
            translatePosition(
                packet[index],
                previousPosition(index),
                interpolation_alpha,
                offset_x,
                offset_y,
                origin_x,
                origin_y);
            ++index;
            if (textured) {
                ++index;
            }
        }
        return;
    }
    if (opcode >= 0x40U && opcode <= 0x5FU) {
        const auto gouraud = (opcode & 0x10U) != 0U;
        for (std::size_t index = 1U; index < packet.size();
             index += gouraud ? 2U : 1U) {
            translatePosition(
                packet[index],
                previousPosition(index),
                interpolation_alpha,
                offset_x,
                offset_y,
                origin_x,
                origin_y);
        }
        return;
    }
    if (opcode >= 0x60U && opcode <= 0x7FU && packet.size() >= 2U) {
        translatePosition(
            packet[1],
            previousPosition(1U),
            interpolation_alpha,
            offset_x,
            offset_y,
            origin_x,
            origin_y);
        // A variable-size rectangle carries `w, h` in its last word, and
        // PsyCross declares those `VERTTYPE` exactly like the position. Retail
        // sends plain integers, so without this every one of its rectangles
        // reads back as a subnormal pair, collapses to a point, and never
        // rasterizes — the level backdrop and the clapperboard behind the
        // lives counter among them. The fixed-size opcodes, 1x1 and 8x8 and
        // 16x16, carry no such word.
        if (convert_rectangle_sizes && (opcode & 0x18U) == 0U) {
            const auto size_index =
                (opcode & 0x04U) != 0U ? std::size_t{3U} : std::size_t{2U};
            if (size_index < packet.size()) {
                packet[size_index] = packPrimitivePair(
                    static_cast<float>(packet[size_index] & 0xFFFFU),
                    static_cast<float>(packet[size_index] >> 16U));
            }
        }
    }
}

void submitPrimitive(std::span<const std::uint32_t> words);


// Retail does not always *draw* the visible frame. The level-load screen
// uploads its whole 512x240 background straight into one display page with
// LoadImage and MoveImages it into the other, then each frame draws only the
// tutorial text on top and relies on the framebuffer keeping the rest. The
// host rebuilds every frame from its packet stream over a cleared backbuffer,
// so those direct writes were never composited: text appeared, background did
// not.
//
// Drawing the selected page back as the frame's base layer restores them. It
// is self-limiting rather than gated: a page nothing has written is all zeros,
// and 15-bit texels of zero are transparent, so the pass costs two rectangles
// and changes nothing when retail renders the frame normally. Measured against
// captured gameplay, the authored 4:3 area is fully covered by world geometry,
// so there is nothing for this to show through.
//
// PS1 texture pages start at (X*64, Y*256) and a 15-bit page window addresses
// 256x256 texels from there, so a 512-wide page is two rectangles.
// Composite a VRAM rectangle into the persistent render target at its
// display-local position, as one or more textured rectangles. The
// persistent-framebuffer path uses this for screens retail *writes* into the
// display page -- CPU uploads and page-to-page copies -- whose pixels are in
// VRAM but were never drawn, so they must be lifted into the render target to
// appear, interleaved in the same stream order as the draws around them.
// `src_x, src_y` are VRAM coordinates; the display rectangle's top-left is
// `(frame_x, frame_y)`. Splits across the page/UV seam for an arbitrary
// rectangle drawn at an arbitrary destination.
void compositeDisplayRect(
    std::uint32_t src_x,
    std::uint32_t src_y,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t frame_x,
    std::uint32_t frame_y) {
    constexpr std::uint32_t page_window = 256U;
    if (width == 0U || height == 0U) {
        return;
    }
    // A 15-bit page window addresses one 256x256 tile at (page_x*64,
    // page_y*256). Split the rectangle across page-Y bands and page-X spans so
    // an arbitrary write is composited rather than skipped at a page seam. A
    // page-aligned display buffer (y=0 or y=256) never spans a band, so the
    // common case is a single pass; the split only matters for an unaligned
    // write, but handling it leaves no edge left open.
    for (std::uint32_t drawn_y = 0U; drawn_y < height;) {
        const auto span_y = src_y + drawn_y;
        const auto page_y = span_y / page_window;
        if (page_y > 1U) {
            // Beyond the two VRAM page rows; nothing addressable remains.
            return;
        }
        const auto v = span_y - page_y * page_window;
        const auto height_here =
            std::min(page_window - v, height - drawn_y);
        for (std::uint32_t drawn_x = 0U; drawn_x < width;) {
            const auto span_x = src_x + drawn_x;
            const auto page_x = span_x / 64U;
            const auto u = span_x - page_x * 64U;
            const auto width_here =
                std::min(page_window - u, width - drawn_x);
            // E1 with 15-bit direct-colour texels, dithering off, drawing to
            // the display area. Re-seeded before every rectangle; the caller
            // restores the stream's own E1 afterwards so a following textured
            // draw is not left sampling this page.
            const std::uint32_t texpage = 0xE1000000U | (page_x & 0xFU) |
                ((page_y & 1U) << 4U) | (2U << 7U) | (1U << 10U);
            submitPrimitive(std::span{&texpage, 1U});

            const auto local_x = static_cast<std::int32_t>(src_x + drawn_x) -
                static_cast<std::int32_t>(frame_x);
            const auto local_y = static_cast<std::int32_t>(src_y + drawn_y) -
                static_cast<std::int32_t>(frame_y);
            std::uint32_t position = 0U;
            packPosition(
                position,
                ScreenPoint{
                    static_cast<float>(local_x),
                    static_cast<float>(local_y)});
            const std::array<std::uint32_t, 4> rectangle{
                // Textured, opaque, neutral modulation.
                0x64808080U,
                position,
                (v << 8U) | u,
                packPrimitivePair(
                    static_cast<float>(width_here),
                    static_cast<float>(height_here)),
            };
            submitPrimitive(rectangle);
            drawn_x += width_here;
        }
        drawn_y += height_here;
    }
}

void normalizeTexturePages(int first_vertex) noexcept {
    for (auto index = first_vertex; index < g_vertexIndex; ++index) {
#if USE_PGXP
        g_vertexBuffer[index].page = static_cast<float>(
            static_cast<int>(g_vertexBuffer[index].page) & 0x1F);
#else
        g_vertexBuffer[index].page =
            static_cast<short>(g_vertexBuffer[index].page & 0x1F);
#endif
    }
}

void submitPrimitive(std::span<const std::uint32_t> words) {
    std::vector<std::uint32_t> primitive(P_LEN + words.size(), 0U);
    auto* tag = reinterpret_cast<P_TAG*>(primitive.data());
    setlen(tag, static_cast<unsigned int>(words.size()));
#if USE_PGXP && USE_EXTENDED_PRIM_POINTERS
    setpgxpindex(tag, 0xFFFFU);
#endif
    std::ranges::copy(words, primitive.begin() + P_LEN);
    const auto first_vertex = g_vertexIndex;
    DrawPrim(primitive.data());
    normalizeTexturePages(first_vertex);
}

void writeLittleEndian32(
    std::span<unsigned char> destination,
    std::size_t offset,
    std::uint32_t value) {
    destination[offset] = static_cast<unsigned char>(value);
    destination[offset + 1U] = static_cast<unsigned char>(value >> 8U);
    destination[offset + 2U] = static_cast<unsigned char>(value >> 16U);
    destination[offset + 3U] = static_cast<unsigned char>(value >> 24U);
}

void writeLittleEndian16(
    std::span<unsigned char> destination,
    std::size_t offset,
    std::uint16_t value) {
    destination[offset] = static_cast<unsigned char>(value);
    destination[offset + 1U] = static_cast<unsigned char>(value >> 8U);
}

} // namespace

PsyCrossPresenter::PsyCrossPresenter(
    const std::filesystem::path& input_config,
    std::uint32_t window_width,
    std::uint32_t window_height,
    std::uint32_t render_width,
    std::uint32_t render_height,
    bool capture_frame_trace,
    bool hidden_window,
    bool fullscreen)
    : capture_frame_trace_(capture_frame_trace),
      window_width_(window_width),
      window_height_(window_height),
      render_width_(render_width),
      render_height_(render_height),
      last_windowed_width_(window_width),
      last_windowed_height_(window_height) {
    if (window_width == 0U ||
        window_width > static_cast<std::uint32_t>(
            std::numeric_limits<int>::max()) ||
        window_height == 0U ||
        window_height > static_cast<std::uint32_t>(
            std::numeric_limits<int>::max())) {
        throw std::runtime_error{"invalid PsyCross window dimensions"};
    }
    if (render_width == 0U ||
        render_width > static_cast<std::uint32_t>(
            std::numeric_limits<int>::max()) ||
        render_height == 0U ||
        render_height > static_cast<std::uint32_t>(
            std::numeric_limits<int>::max())) {
        throw std::runtime_error{"invalid PsyCross render dimensions"};
    }
    std::array application_name{
        'S', 't', 'u', 'n', 't', 'm', 'a', 's', 't', 'e', 'r', ' ', 'P', 'C',
        '\0'};
    // Always create the window hidden, then reveal it only after it is in its
    // final geometry. For a fullscreen launch the SDL borderless-desktop mode is
    // applied while hidden, so the user never sees a windowed frame flash before
    // it goes fullscreen. `hidden_window` (the headless capture path) leaves it
    // hidden for good. Fullscreen is SDL's own SDL_WINDOW_FULLSCREEN_DESKTOP
    // (DWM-composited borderless): it keeps vsync engaged and stays screenshot-
    // able, and PsyCross owns the Alt+Enter toggle that switches it at runtime.
    g_psxHiddenWindow = 1;
    PsyX_Initialise(
        application_name.data(),
        static_cast<int>(window_width),
        static_cast<int>(window_height),
        0);
    // The on-screen presenter owns its window swaps, bypassing PsyCross's
    // EndScene swap-interval logic, so it must assert vsync itself. The headless
    // capture path presents uncapped.
    vsync_wanted_ = !hidden_window;
    if (!hidden_window) {
        if (fullscreen && g_window != nullptr &&
            SDL_SetWindowFullscreen(
                g_window, SDL_WINDOW_FULLSCREEN_DESKTOP) == 0) {
            // Match the window-size globals to the borderless-desktop extent so
            // the first present composes at the display resolution, exactly as
            // PsyCross's own Alt+Enter path does after a toggle.
            SDL_GetWindowSize(g_window, &g_windowWidth, &g_windowHeight);
            GR_ResetDevice();
        }
        // Reveal the window now that it is in its final windowed or fullscreen
        // geometry.
        if (g_window != nullptr) {
            SDL_ShowWindow(g_window);
        }
    }
    SDL_DisplayMode display_mode{};
    const auto display_index = g_window != nullptr
        ? SDL_GetWindowDisplayIndex(g_window)
        : -1;
    if (display_index >= 0 &&
        SDL_GetCurrentDisplayMode(display_index, &display_mode) == 0 &&
        display_mode.refresh_rate > 0) {
        display_refresh_rate_ = static_cast<std::uint32_t>(
            display_mode.refresh_rate);
    }
    GR_SetRenderResolution(
        static_cast<int>(render_width), static_cast<int>(render_height));
    g_cfg_keyboardMapping.kc_dpad_up = PsyX_LookupKeyboardMapping(
        "W", g_cfg_keyboardMapping.kc_dpad_up);
    g_cfg_keyboardMapping.kc_dpad_down = PsyX_LookupKeyboardMapping(
        "S", g_cfg_keyboardMapping.kc_dpad_down);
    g_cfg_keyboardMapping.kc_dpad_left = PsyX_LookupKeyboardMapping(
        "A", g_cfg_keyboardMapping.kc_dpad_left);
    g_cfg_keyboardMapping.kc_dpad_right = PsyX_LookupKeyboardMapping(
        "D", g_cfg_keyboardMapping.kc_dpad_right);
    applyInputConfig(input_config, mouse_config_);
    debug_overlay_toggle_key_ = PsyX_LookupKeyboardMapping("0", 0);
    quick_save_key_ = PsyX_LookupKeyboardMapping("F5", 0);
    quick_load_key_ = PsyX_LookupKeyboardMapping("F9", 0);
    timestamped_quick_save_key_ = PsyX_LookupKeyboardMapping("F6", 0);
    retime_toggle_key_ = PsyX_LookupKeyboardMapping("F7", 0);
    widescreen_toggle_key_ = PsyX_LookupKeyboardMapping("F8", 0);
    license_toggle_key_ = PsyX_LookupKeyboardMapping("L", 0);
    free_camera_toggle_key_ = PsyX_LookupKeyboardMapping("F11", 0);
    mouse_mode_toggle_key_ = PsyX_LookupKeyboardMapping("F10", 0);
    photo_simulation_toggle_key_ = PsyX_LookupKeyboardMapping("P", 0);
    free_camera_forward_key_ = PsyX_LookupKeyboardMapping("W", 0);
    free_camera_backward_key_ = PsyX_LookupKeyboardMapping("S", 0);
    free_camera_left_key_ = PsyX_LookupKeyboardMapping("A", 0);
    free_camera_right_key_ = PsyX_LookupKeyboardMapping("D", 0);
    free_camera_up_key_ = PsyX_LookupKeyboardMapping("Q", 0);
    free_camera_down_key_ = PsyX_LookupKeyboardMapping("E", 0);
    free_camera_fast_key_ = PsyX_LookupKeyboardMapping("Left Shift", 0);
    PadInitDirect(pad_one_.data(), pad_two_.data());
    PadStartCom();
    SDL_AddEventWatch(&PsyCrossPresenter::mouseEventWatch, this);
    mouse_event_watch_installed_ = true;
    initialized_ = true;
}

bool PsyCrossPresenter::isFullscreen() const noexcept {
    // SDL_WINDOW_FULLSCREEN_DESKTOP includes the SDL_WINDOW_FULLSCREEN bit, so
    // mask for the full desktop-fullscreen flag set.
    return g_window != nullptr &&
        (SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN_DESKTOP) ==
            SDL_WINDOW_FULLSCREEN_DESKTOP;
}

PsyCrossPresenter::~PsyCrossPresenter() {
    if (mouse_event_watch_installed_) {
        SDL_DelEventWatch(&PsyCrossPresenter::mouseEventWatch, this);
        mouse_event_watch_installed_ = false;
    }
    if (initialized_) {
        PadStopCom();
        if (rumble_controller_ != nullptr) {
            SDL_GameControllerClose(rumble_controller_);
            rumble_controller_ = nullptr;
        }
        PsyX_Shutdown();
    }
}

std::uint16_t PsyCrossPresenter::pollPadOneButtons() {
    PsyX_UpdateInput();
    // Detect every fullscreen-state edge (including toggles made during an FMV,
    // which this poll drives) and persist it immediately via the callback -- a
    // window close hard-exits from PsyCross's event pump, so deferring to the
    // main loop would lose a change made just before closing.
    {
        const bool fs_now = isFullscreen();
        if (fs_now != last_fullscreen_state_) {
            last_fullscreen_state_ = fs_now;
            if (fullscreen_changed_callback_) {
                fullscreen_changed_callback_(fs_now);
            }
        }
    }
    // Alt+Enter borderless-desktop fullscreen is owned by PsyCross's own event
    // handler (SDL_SetWindowFullscreen). The host only observes the resulting
    // state through isFullscreen() for persistence.
    //
    // Track the windowed client size so a user resize is captured for
    // persistence. Skip while fullscreen, where the window spans the monitor;
    // the last windowed size then stays frozen for the app to store.
    if (!isFullscreen() && g_windowWidth > 0 && g_windowHeight > 0) {
        last_windowed_width_ = static_cast<std::uint32_t>(g_windowWidth);
        last_windowed_height_ = static_cast<std::uint32_t>(g_windowHeight);
    }
    auto* controller = ensureGameController();
    const auto keyPressed = [](int key) {
        return g_sdlKeyboardState != nullptr && key > 0 &&
            g_sdlKeyboardState[key] != 0U;
    };
    const auto toggle_pressed = g_sdlKeyboardState != nullptr &&
        debug_overlay_toggle_key_ > 0 &&
        g_sdlKeyboardState[debug_overlay_toggle_key_] != 0U;
    const auto quick_save_pressed = keyPressed(quick_save_key_);
    const auto quick_load_pressed = keyPressed(quick_load_key_);
    const auto timestamped_quick_save_pressed =
        keyPressed(timestamped_quick_save_key_);
    const auto retime_toggle_pressed = keyPressed(retime_toggle_key_);
    const auto widescreen_toggle_pressed =
        keyPressed(widescreen_toggle_key_);
    const auto free_camera_toggle_pressed =
        keyPressed(free_camera_toggle_key_);
    const auto mouse_mode_toggle_pressed = keyPressed(mouse_mode_toggle_key_);
    const auto photo_simulation_toggle_pressed =
        keyPressed(photo_simulation_toggle_key_);
    const auto controller_select_pressed =
        controllerMappingValue(controller, g_cfg_controllerMapping.gc_select) >
        16'384;
    const auto controller_select_consumed = controller_select_pressed &&
        (photo_mode_available_ || free_camera_active_);
    const auto controller_r3_pressed =
        controllerMappingValue(controller, g_cfg_controllerMapping.gc_r3) >
        16'384;
    quick_save_requested_ = quick_save_requested_ ||
        (quick_save_pressed && !quick_save_key_down_);
    quick_load_requested_ = quick_load_requested_ ||
        (quick_load_pressed && !quick_load_key_down_);
    timestamped_quick_save_requested_ =
        timestamped_quick_save_requested_ ||
        (timestamped_quick_save_pressed &&
         !timestamped_quick_save_key_down_);
    retime_toggle_requested_ = retime_toggle_requested_ ||
        (retime_toggle_pressed && !retime_toggle_key_down_);
    widescreen_toggle_requested_ = widescreen_toggle_requested_ ||
        (widescreen_toggle_pressed && !widescreen_toggle_key_down_);
    free_camera_toggle_requested_ = free_camera_toggle_requested_ ||
        (free_camera_toggle_pressed && !free_camera_toggle_key_down_) ||
        (controller_select_consumed &&
         !free_camera_controller_select_down_);
    mouse_mode_cycle_requested_ = mouse_mode_cycle_requested_ ||
        (mouse_mode_toggle_pressed && !mouse_mode_toggle_key_down_);
    photo_simulation_toggle_requested_ =
        photo_simulation_toggle_requested_ ||
        (free_camera_active_ &&
         ((photo_simulation_toggle_pressed &&
           !photo_simulation_toggle_key_down_) ||
          (controller_r3_pressed &&
           !photo_simulation_controller_r3_down_)));
    quick_save_key_down_ = quick_save_pressed;
    quick_load_key_down_ = quick_load_pressed;
    timestamped_quick_save_key_down_ = timestamped_quick_save_pressed;
    retime_toggle_key_down_ = retime_toggle_pressed;
    widescreen_toggle_key_down_ = widescreen_toggle_pressed;
    free_camera_toggle_key_down_ = free_camera_toggle_pressed;
    mouse_mode_toggle_key_down_ = mouse_mode_toggle_pressed;
    free_camera_controller_select_down_ = controller_select_pressed;
    photo_simulation_toggle_key_down_ = photo_simulation_toggle_pressed;
    photo_simulation_controller_r3_down_ = controller_r3_pressed;

    free_camera_movement_ = 0U;
    const auto addMovement = [&](int key, game::FreeCameraMovement movement) {
        if (free_camera_active_ && keyPressed(key)) {
            free_camera_movement_ |= static_cast<std::uint8_t>(movement);
        }
    };
    addMovement(free_camera_forward_key_, game::free_camera_forward);
    addMovement(free_camera_backward_key_, game::free_camera_backward);
    addMovement(free_camera_left_key_, game::free_camera_left);
    addMovement(free_camera_right_key_, game::free_camera_right);
    addMovement(free_camera_up_key_, game::free_camera_up);
    addMovement(free_camera_down_key_, game::free_camera_down);
    addMovement(free_camera_fast_key_, game::free_camera_fast);
    free_camera_controller_right_ = 0;
    free_camera_controller_up_ = 0;
    free_camera_controller_forward_ = 0;
    free_camera_controller_look_x_ = 0;
    free_camera_controller_look_y_ = 0;
    updateRelativeMouseMode();
    if (free_camera_active_) {
        int mouse_x = 0;
        int mouse_y = 0;
        (void)SDL_GetRelativeMouseState(&mouse_x, &mouse_y);
        free_camera_mouse_x_ += mouse_x;
        free_camera_mouse_y_ += mouse_y;
        const auto move = shapeControllerStick(
            controllerMappingValue(
                controller, g_cfg_controllerMapping.gc_axis_left_x),
            controllerMappingValue(
                controller, g_cfg_controllerMapping.gc_axis_left_y));
        const auto look = shapeControllerStick(
            controllerMappingValue(
                controller, g_cfg_controllerMapping.gc_axis_right_x),
            controllerMappingValue(
                controller, g_cfg_controllerMapping.gc_axis_right_y));
        const auto ascend = shapeControllerTrigger(controllerMappingValue(
            controller, g_cfg_controllerMapping.gc_l2));
        const auto descend = shapeControllerTrigger(controllerMappingValue(
            controller, g_cfg_controllerMapping.gc_r2));
        free_camera_controller_right_ = move[0];
        free_camera_controller_forward_ = static_cast<std::int16_t>(-move[1]);
        free_camera_controller_up_ = static_cast<std::int16_t>(
            static_cast<int>(ascend) - static_cast<int>(descend));
        free_camera_controller_look_x_ = look[0];
        free_camera_controller_look_y_ = look[1];
    } else if (mouse_gameplay_accepted_ && relative_mouse_active_) {
        int mouse_x = 0;
        int ignored_y = 0;
        (void)SDL_GetRelativeMouseState(&mouse_x, &ignored_y);
        mouse_x_ += mouse_x;
    }
    const auto held_mouse_buttons = SDL_GetMouseState(nullptr, nullptr);
    const auto pressed_mouse_buttons = mouse_button_press_latch_.exchange(
        0U, std::memory_order_acq_rel);
    mouse_held_actions_ = 0U;
    if (mouse_gameplay_accepted_ && relative_mouse_active_ &&
        !free_camera_active_) {
        const auto buttonMask = [](game::MouseButton button) -> unsigned int {
            switch (button) {
            case game::MouseButton::left: return SDL_BUTTON(SDL_BUTTON_LEFT);
            case game::MouseButton::right: return SDL_BUTTON(SDL_BUTTON_RIGHT);
            case game::MouseButton::middle:
                return SDL_BUTTON(SDL_BUTTON_MIDDLE);
            case game::MouseButton::x1: return SDL_BUTTON(SDL_BUTTON_X1);
            case game::MouseButton::x2: return SDL_BUTTON(SDL_BUTTON_X2);
            case game::MouseButton::none: return 0U;
            }
            return 0U;
        };
        for (std::size_t action = 0U; action < mouse_config_.actions.size();
             ++action) {
            const auto mask = buttonMask(mouse_config_.actions[action]);
            if ((held_mouse_buttons & mask) != 0U) {
                mouse_held_actions_ |= static_cast<std::uint8_t>(1U << action);
            }
            if ((pressed_mouse_buttons & mask) != 0U) {
                mouse_pressed_actions_ |=
                    static_cast<std::uint8_t>(1U << action);
            }
        }
    } else {
        mouse_pressed_actions_ = 0U;
        mouse_x_ = 0;
    }
    debug_overlay_visible_ = debug_overlay_toggle_.update(
        debug_overlay_.enabled,
        debug_overlay_visible_,
        toggle_pressed);
    // PsyCross fills this host-side PADRAW buffer by storing a u_short, so
    // buttons[0] is the native low byte on supported Windows x64 hosts. Return
    // the logical 16-bit active-low word; RetailHle is responsible for
    // serializing it into the retail pad buffer's low-byte-first wire order.
    auto buttons =
        decodePsyCrossPadButtons(pad_one_[2], pad_one_[3]);
    // Host license viewer. 'L' toggles it; while open it consumes navigation and
    // the guest sees a neutral pad so the game underneath is frozen. Opening from
    // the in-game menu goes through openLicenseViewer() on the main thread.
    const auto license_toggle_pressed = keyPressed(license_toggle_key_);
    if (license_toggle_pressed && !license_toggle_key_down_ &&
        !license_overlay_.empty()) {
        license_viewer_active_ = !license_viewer_active_;
    }
    license_toggle_key_down_ = license_toggle_pressed;
    if (license_viewer_active_) {
        updateLicenseViewerInput(buttons);
        mouse_held_actions_ = 0U;
        mouse_pressed_actions_ = 0U;
        mouse_x_ = 0;
        previous_trace_buttons_ = 0xFFFFU;
        return 0xFFFFU;
    }
    if (controller_select_consumed) {
        // Controller Select is photo mode's host toggle. Keyboard Select keeps
        // its ordinary guest binding, and controller Select remains guest-
        // visible outside steady gameplay.
        buttons |= 0x0001U;
    }
    if (free_camera_active_ && controller_r3_pressed) {
        // R3 is host-owned only while photo mode is active. L3 remains the
        // optional frame-trace dump control.
        buttons |= 0x0004U;
    }
    if ((debug_overlay_.enabled && toggle_pressed) || quick_save_pressed ||
        quick_load_pressed || timestamped_quick_save_pressed ||
        retime_toggle_pressed || widescreen_toggle_pressed ||
        mouse_mode_toggle_pressed ||
        free_camera_toggle_pressed ||
        (free_camera_active_ && photo_simulation_toggle_pressed) ||
        free_camera_movement_ != 0U) {
        // Host keys never reach the guest PAD, even if a custom input file
        // also maps one of them to a guest-visible button.
        const auto neutralize = [&](int mapping, std::uint16_t mask) {
            if ((debug_overlay_.enabled && toggle_pressed &&
                 mapping == debug_overlay_toggle_key_) ||
                (quick_save_pressed && mapping == quick_save_key_) ||
                (quick_load_pressed && mapping == quick_load_key_) ||
                (timestamped_quick_save_pressed &&
                 mapping == timestamped_quick_save_key_) ||
                (retime_toggle_pressed && mapping == retime_toggle_key_) ||
                (widescreen_toggle_pressed &&
                 mapping == widescreen_toggle_key_) ||
                (mouse_mode_toggle_pressed &&
                 mapping == mouse_mode_toggle_key_) ||
                (free_camera_toggle_pressed &&
                 mapping == free_camera_toggle_key_) ||
                (free_camera_active_ && photo_simulation_toggle_pressed &&
                 mapping == photo_simulation_toggle_key_) ||
                (free_camera_active_ && keyPressed(mapping) &&
                 (mapping == free_camera_forward_key_ ||
                  mapping == free_camera_backward_key_ ||
                  mapping == free_camera_left_key_ ||
                  mapping == free_camera_right_key_ ||
                  mapping == free_camera_up_key_ ||
                  mapping == free_camera_down_key_ ||
                  mapping == free_camera_fast_key_))) {
                buttons |= mask;
            }
        };
        neutralize(g_cfg_keyboardMapping.kc_square, 0x8000U);
        neutralize(g_cfg_keyboardMapping.kc_circle, 0x2000U);
        neutralize(g_cfg_keyboardMapping.kc_triangle, 0x1000U);
        neutralize(g_cfg_keyboardMapping.kc_cross, 0x4000U);
        neutralize(g_cfg_keyboardMapping.kc_l1, 0x0400U);
        neutralize(g_cfg_keyboardMapping.kc_l2, 0x0100U);
        neutralize(g_cfg_keyboardMapping.kc_l3, 0x0002U);
        neutralize(g_cfg_keyboardMapping.kc_r1, 0x0800U);
        neutralize(g_cfg_keyboardMapping.kc_r2, 0x0200U);
        neutralize(g_cfg_keyboardMapping.kc_r3, 0x0004U);
        neutralize(g_cfg_keyboardMapping.kc_dpad_up, 0x0010U);
        neutralize(g_cfg_keyboardMapping.kc_dpad_down, 0x0040U);
        neutralize(g_cfg_keyboardMapping.kc_dpad_left, 0x0080U);
        neutralize(g_cfg_keyboardMapping.kc_dpad_right, 0x0020U);
        neutralize(g_cfg_keyboardMapping.kc_select, 0x0001U);
        neutralize(g_cfg_keyboardMapping.kc_start, 0x0008U);
    }
    // L3 dumps the diagnostic ring. The retail game presents a digital pad,
    // which has no L3, so this cannot disturb gameplay. Circle used to trigger
    // it, but Circle is grab/interact, so normal play dumped the ring
    // constantly and made capture runs unusable.
    constexpr std::uint16_t l3_bit = 0x0002U;
    if (capture_frame_trace_ &&
        (previous_trace_buttons_ & l3_bit) != 0U &&
        (buttons & l3_bit) == 0U) {
        ++capture_trace_event_;
        dumpDiagnosticFrames();
    }
    previous_trace_buttons_ = buttons;
    return buttons;
}

void PsyCrossPresenter::setFreeCameraActive(bool active) noexcept {
    if (free_camera_active_ == active) {
        return;
    }
    free_camera_active_ = active;
    free_camera_movement_ = 0U;
    free_camera_mouse_x_ = 0;
    free_camera_mouse_y_ = 0;
    free_camera_controller_right_ = 0;
    free_camera_controller_up_ = 0;
    free_camera_controller_forward_ = 0;
    free_camera_controller_look_x_ = 0;
    free_camera_controller_look_y_ = 0;
    updateRelativeMouseMode();
}

int PsyCrossPresenter::mouseEventWatch(
    void* userdata, SDL_Event* event) noexcept {
    if (userdata != nullptr && event != nullptr &&
        event->type == SDL_MOUSEBUTTONDOWN) {
        auto* presenter = static_cast<PsyCrossPresenter*>(userdata);
        presenter->mouse_button_press_latch_.fetch_or(
            SDL_BUTTON(event->button.button), std::memory_order_release);
    }
    return 1;
}

void PsyCrossPresenter::setMouseGameplayAccepted(bool accepted) noexcept {
    if (mouse_gameplay_accepted_ == accepted) {
        updateRelativeMouseMode();
        return;
    }
    mouse_gameplay_accepted_ = accepted;
    mouse_held_actions_ = 0U;
    mouse_pressed_actions_ = 0U;
    mouse_x_ = 0;
    updateRelativeMouseMode();
}

void PsyCrossPresenter::updateRelativeMouseMode() noexcept {
    const auto focused = g_window != nullptr &&
        (SDL_GetWindowFlags(g_window) & SDL_WINDOW_INPUT_FOCUS) != 0U;
    const auto wanted = focused &&
        (free_camera_active_ || mouse_gameplay_accepted_);
    if (relative_mouse_active_ == wanted) {
        return;
    }
    const auto changed =
        SDL_SetRelativeMouseMode(wanted ? SDL_TRUE : SDL_FALSE) == 0;
    relative_mouse_active_ = wanted && changed;
    int ignored_x = 0;
    int ignored_y = 0;
    (void)SDL_GetRelativeMouseState(&ignored_x, &ignored_y);
}

void PsyCrossPresenter::applyRumble(
    std::uint8_t motor1,
    std::uint8_t motor2,
    std::uint32_t duration_ms) {
    const auto active = duration_ms != 0U && (motor1 != 0U || motor2 != 0U);
    if (!active) {
        if (rumble_active_ && rumble_controller_ != nullptr) {
            SDL_GameControllerRumble(rumble_controller_, 0, 0, 0);
        }
        rumble_active_ = false;
        return;
    }
    if (ensureGameController() == nullptr) {
        rumble_active_ = false;
        return;
    }
    // DualShock actuator 1 is the big low-frequency motor and actuator 2 the
    // small high-frequency one; SDL's Rumble arguments are in the same
    // [low, high] order. Scale the retail 0..255 bytes to SDL's 0..65535
    // magnitude. SDL_GameControllerRumble needs no SDL haptics subsystem: on
    // Windows it drives XInput controllers directly.
    constexpr std::uint16_t rumble_scale = 257U;
    SDL_GameControllerRumble(
        rumble_controller_,
        static_cast<std::uint16_t>(motor1) * rumble_scale,
        static_cast<std::uint16_t>(motor2) * rumble_scale,
        duration_ms);
    rumble_active_ = true;
}

SDL_GameController* PsyCrossPresenter::ensureGameController() noexcept {
    if (rumble_controller_ != nullptr &&
        SDL_GameControllerGetAttached(rumble_controller_)) {
        return rumble_controller_;
    }
    if (rumble_controller_ != nullptr) {
        SDL_GameControllerClose(rumble_controller_);
        rumble_controller_ = nullptr;
    }
    for (int index = 0; index < SDL_NumJoysticks(); ++index) {
        if (SDL_IsGameController(index)) {
            rumble_controller_ = SDL_GameControllerOpen(index);
            break;
        }
    }
    return rumble_controller_;
}

void PsyCrossPresenter::setDebugOverlay(DebugOverlayState state) noexcept {
    if (!debug_overlay_.enabled && state.enabled) {
        debug_overlay_visible_ = state.initially_visible;
    } else if (!state.enabled) {
        debug_overlay_visible_ = false;
    }
    debug_overlay_ = state;
}

void PsyCrossPresenter::showNotification(std::string message) {
    notification_message_ = std::move(message);
    notification_expires_ =
        std::chrono::steady_clock::now() + std::chrono::milliseconds{2500};
}

void PsyCrossPresenter::setRenderSize(
    std::uint32_t width,
    std::uint32_t height) {
    if (width == 0U || height == 0U ||
        width > static_cast<std::uint32_t>(
            std::numeric_limits<int>::max()) ||
        height > static_cast<std::uint32_t>(
            std::numeric_limits<int>::max())) {
        throw std::runtime_error{"invalid PsyCross render dimensions"};
    }
    if (width == render_width_ && height == render_height_) {
        return;
    }
    render_width_ = width;
    render_height_ = height;
    GR_SetRenderResolution(
        static_cast<int>(width), static_cast<int>(height));
    // GR_BeginRenderTarget reallocates lazily and invalidates PsyCross's repeat
    // cache. A scanout/movie repeat must also wait for a new full blit.
    has_scanout_ = false;
}

void PsyCrossPresenter::presentBlackFrame() {
    if (!initialized_) {
        return;
    }
    GR_BeginRenderTarget();
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.0F, 0.0F, 0.0F, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT);
    finishWindowPresentation();
}

void PsyCrossPresenter::presentMovieFrame(
    std::span<const std::uint8_t> rgba8888,
    std::uint32_t width,
    std::uint32_t height) {
    const auto pixel_count = static_cast<std::size_t>(width) * height;
    if (!initialized_ || width == 0U || height == 0U ||
        pixel_count > std::numeric_limits<std::size_t>::max() / 4U ||
        rgba8888.size() < pixel_count * 4U) {
        return;
    }
    if (scanout_texture_ == 0U) {
        glGenTextures(1, &scanout_texture_);
        glBindTexture(GL_TEXTURE_2D, scanout_texture_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glGenFramebuffers(1, &scanout_framebuffer_);
    }

    glBindTexture(GL_TEXTURE_2D, scanout_texture_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(
        GL_TEXTURE_2D,
        0,
        GL_RGBA8,
        static_cast<GLsizei>(width),
        static_cast<GLsizei>(height),
        0,
        GL_RGBA,
        GL_UNSIGNED_BYTE,
        rgba8888.data());
    scanout_width_ = width;
    scanout_height_ = height;
    blitScanout();
    finishWindowPresentation();
    has_scanout_ = true;
}

void PsyCrossPresenter::blitScanout() {
    const auto width = scanout_width_;
    const auto height = scanout_height_;
    GR_BeginRenderTarget();
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.0F, 0.0F, 0.0F, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT);

    // Retail movies are authored at 4:3. First place them in the fixed internal
    // target; PsyCross then scales that completed
    // image into the independently resizable window.
    const auto target = fitDisplayViewport(
        4U, 3U, render_width_, render_height_);
    const auto x0 = static_cast<GLint>(target.x);
    const auto y0 = static_cast<GLint>(target.y);
    const auto x1 = x0 + static_cast<GLint>(target.width);
    const auto y1 = y0 + static_cast<GLint>(target.height);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, scanout_framebuffer_);
    glFramebufferTexture2D(
        GL_READ_FRAMEBUFFER,
        GL_COLOR_ATTACHMENT0,
        GL_TEXTURE_2D,
        scanout_texture_,
        0);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    // The source rows run top-down and GL's read origin is bottom-left, so the
    // blit flips by reading the source rectangle upside down.
    glBlitFramebuffer(
        0,
        static_cast<GLint>(height),
        static_cast<GLint>(width),
        0,
        x0,
        y0,
        x1,
        y1,
        GL_COLOR_BUFFER_BIT,
        GL_LINEAR);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glReadBuffer(GL_BACK);

}

void PsyCrossPresenter::repeatScanout() {
    if (!initialized_ || !has_scanout_) {
        return;
    }
    // Re-blit the frame this presenter already holds rather than PsyCross's
    // cached copy. At a 120 Hz presentation rate against a 30 Hz guest most
    // presentations are repeats, so routing them back through the Psy-Q layer
    // would leave it responsible for most of what reaches the screen.
    blitScanout();
    finishWindowPresentation();
}

void PsyCrossPresenter::finishWindowPresentation() {
    GR_PresentRenderTargetToWindow();
    drawDebugOverlay();
    drawLicenseOverlay();
    // Keep the live window swap on vsync. PsyCross's GR_ResetDevice (run at
    // launch, on every Alt+Enter toggle, and on window resize) resets the swap
    // interval to 0, so re-assert 1 whenever it has been cleared. The read is a
    // cheap cached query, so steady-state frames set nothing.
    if (vsync_wanted_ && SDL_GL_GetSwapInterval() != 1) {
        SDL_GL_SetSwapInterval(1);
    }
    GR_SwapWindowBuffers();
}

void PsyCrossPresenter::setLicenseDocuments(
    std::vector<LicenseDocument> documents) {
    license_overlay_.setDocuments(std::move(documents));
}

void PsyCrossPresenter::openLicenseViewer() {
    if (!license_overlay_.empty()) {
        license_viewer_active_ = true;
    }
}

void PsyCrossPresenter::closeLicenseViewer() noexcept {
    license_viewer_active_ = false;
}

void PsyCrossPresenter::updateLicenseViewerInput(std::uint16_t pad_buttons) {
    const auto padPressed = [&](std::uint16_t mask) {
        return (pad_buttons & mask) == 0U;
    };
    const auto keyDown = [&](int scancode) {
        return g_sdlKeyboardState != nullptr &&
            g_sdlKeyboardState[scancode] != 0U;
    };
    // Active-low pad masks: D-pad up/down 0x0010/0x0040, left/right 0x0080/0x0020,
    // L1/R1 0x0400/0x0800, circle (back) 0x2000.
    const bool up = padPressed(0x0010U) || keyDown(SDL_SCANCODE_UP);
    const bool down = padPressed(0x0040U) || keyDown(SDL_SCANCODE_DOWN);
    const bool page_up = padPressed(0x0400U) || keyDown(SDL_SCANCODE_PAGEUP);
    const bool page_down = padPressed(0x0800U) || keyDown(SDL_SCANCODE_PAGEDOWN);
    const bool prev_doc = padPressed(0x0080U) || keyDown(SDL_SCANCODE_LEFT);
    const bool next_doc = padPressed(0x0020U) || keyDown(SDL_SCANCODE_RIGHT);
    const bool close = padPressed(0x2000U) || keyDown(SDL_SCANCODE_ESCAPE);

    // Scroll one notch on the rising edge, then auto-repeat on a wall-clock
    // schedule -- a tap nudges precisely, a hold scrolls smoothly, and the speed
    // does not change with the input-poll rate (which is faster during FMV
    // playback and stalls at movie transitions). Fast, coarse movement is on the
    // page buttons.
    constexpr int scroll_notch = 3;
    constexpr auto scroll_initial_delay = std::chrono::milliseconds{350};
    constexpr auto scroll_repeat_interval = std::chrono::milliseconds{35};
    const auto now = std::chrono::steady_clock::now();
    const int direction = (up && !down) ? -1 : ((down && !up) ? 1 : 0);
    if (direction == 0) {
        license_scroll_direction_ = 0;
    } else if (direction != license_scroll_direction_) {
        license_scroll_direction_ = direction;
        license_scroll_next_repeat_ = now + scroll_initial_delay;
        license_overlay_.scrollLines(direction * scroll_notch);
    } else if (now >= license_scroll_next_repeat_) {
        // Schedule the next notch relative to now, so a stall (e.g. a movie
        // transition) never releases a catch-up burst of scrolling.
        license_scroll_next_repeat_ = now + scroll_repeat_interval;
        license_overlay_.scrollLines(direction * scroll_notch);
    }
    if (page_up && !license_page_up_down_) {
        license_overlay_.scrollPages(-1);
    }
    if (page_down && !license_page_down_down_) {
        license_overlay_.scrollPages(1);
    }
    if (prev_doc && !license_prev_doc_down_) {
        license_overlay_.previousDocument();
    }
    if (next_doc && !license_next_doc_down_) {
        license_overlay_.nextDocument();
    }
    if (close && !license_close_down_) {
        license_viewer_active_ = false;
    }
    license_page_up_down_ = page_up;
    license_page_down_down_ = page_down;
    license_prev_doc_down_ = prev_doc;
    license_next_doc_down_ = next_doc;
    license_close_down_ = close;
}

void PsyCrossPresenter::drawDebugOverlay() {
    if (!debug_overlay_.enabled || !debug_overlay_visible_) {
        drawNotificationOverlay();
        return;
    }
    const auto window_width = g_windowWidth > 0
        ? static_cast<std::uint32_t>(g_windowWidth)
        : window_width_;
    const auto window_height = g_windowHeight > 0
        ? static_cast<std::uint32_t>(g_windowHeight)
        : window_height_;
    const auto scale = window_width >= 640U && window_height >= 360U
        ? 2U
        : 1U;
    const auto bitmap = rasterizeDebugOverlay(debug_overlay_, scale);
    if (bitmap.rgba.empty() || bitmap.width == 0U || bitmap.height == 0U) {
        return;
    }
    if (debug_overlay_texture_ == 0U) {
        glGenTextures(1, &debug_overlay_texture_);
        glGenFramebuffers(1, &debug_overlay_framebuffer_);
    }
    glBindTexture(GL_TEXTURE_2D, debug_overlay_texture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(
        GL_TEXTURE_2D,
        0,
        GL_RGBA8,
        static_cast<GLsizei>(bitmap.width),
        static_cast<GLsizei>(bitmap.height),
        0,
        GL_RGBA,
        GL_UNSIGNED_BYTE,
        bitmap.rgba.data());

    const auto scissor_enabled = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, debug_overlay_framebuffer_);
    glFramebufferTexture2D(
        GL_READ_FRAMEBUFFER,
        GL_COLOR_ATTACHMENT0,
        GL_TEXTURE_2D,
        debug_overlay_texture_,
        0);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glDrawBuffer(GL_BACK);
    constexpr GLint margin = 8;
    const auto x0 = margin;
    const auto y0 = std::max(
        0,
        static_cast<GLint>(window_height) - margin -
            static_cast<GLint>(bitmap.height));
    const auto x1 = std::min(
        static_cast<GLint>(window_width),
        x0 + static_cast<GLint>(bitmap.width));
    const auto y1 = std::min(
        static_cast<GLint>(window_height),
        y0 + static_cast<GLint>(bitmap.height));
    glBlitFramebuffer(
        0,
        static_cast<GLint>(bitmap.height),
        x1 - x0,
        0,
        x0,
        y0,
        x1,
        y1,
        GL_COLOR_BUFFER_BIT,
        GL_NEAREST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glReadBuffer(GL_BACK);
    if (scissor_enabled != GL_FALSE) {
        glEnable(GL_SCISSOR_TEST);
    }
    drawNotificationOverlay();
}

void PsyCrossPresenter::drawNotificationOverlay() {
    if (notification_message_.empty() ||
        std::chrono::steady_clock::now() >= notification_expires_) {
        notification_message_.clear();
        return;
    }
    const auto window_width = g_windowWidth > 0
        ? static_cast<std::uint32_t>(g_windowWidth)
        : window_width_;
    const auto window_height = g_windowHeight > 0
        ? static_cast<std::uint32_t>(g_windowHeight)
        : window_height_;
    const auto scale = window_width >= 640U && window_height >= 360U
        ? 2U
        : 1U;
    const auto bitmap = rasterizeNotificationOverlay(
        notification_message_, scale);
    if (bitmap.rgba.empty() || bitmap.width == 0U || bitmap.height == 0U) {
        return;
    }
    if (notification_overlay_texture_ == 0U) {
        glGenTextures(1, &notification_overlay_texture_);
        glGenFramebuffers(1, &notification_overlay_framebuffer_);
    }
    glBindTexture(GL_TEXTURE_2D, notification_overlay_texture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(
        GL_TEXTURE_2D,
        0,
        GL_RGBA8,
        static_cast<GLsizei>(bitmap.width),
        static_cast<GLsizei>(bitmap.height),
        0,
        GL_RGBA,
        GL_UNSIGNED_BYTE,
        bitmap.rgba.data());

    const auto scissor_enabled = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, notification_overlay_framebuffer_);
    glFramebufferTexture2D(
        GL_READ_FRAMEBUFFER,
        GL_COLOR_ATTACHMENT0,
        GL_TEXTURE_2D,
        notification_overlay_texture_,
        0);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glDrawBuffer(GL_BACK);
    constexpr GLint margin = 8;
    const auto x0 = std::max(
        0,
        static_cast<GLint>(window_width) - margin -
            static_cast<GLint>(bitmap.width));
    const auto y0 = std::max(
        0,
        static_cast<GLint>(window_height) - margin -
            static_cast<GLint>(bitmap.height));
    const auto x1 = std::min(
        static_cast<GLint>(window_width),
        x0 + static_cast<GLint>(bitmap.width));
    const auto y1 = std::min(
        static_cast<GLint>(window_height),
        y0 + static_cast<GLint>(bitmap.height));
    glBlitFramebuffer(
        0,
        static_cast<GLint>(bitmap.height),
        x1 - x0,
        0,
        x0,
        y0,
        x1,
        y1,
        GL_COLOR_BUFFER_BIT,
        GL_NEAREST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glReadBuffer(GL_BACK);
    if (scissor_enabled != GL_FALSE) {
        glEnable(GL_SCISSOR_TEST);
    }
}

void PsyCrossPresenter::drawLicenseOverlay() {
    if (!license_viewer_active_ || license_overlay_.empty()) {
        return;
    }
    const auto window_width = g_windowWidth > 0
        ? static_cast<std::uint32_t>(g_windowWidth)
        : window_width_;
    const auto window_height = g_windowHeight > 0
        ? static_cast<std::uint32_t>(g_windowHeight)
        : window_height_;
    // Fill ~90% of the window with a readable page. The rasterizer sizes a row
    // as margin*2 + cols*6*scale wide and margin*2 + rows*9*scale tall
    // (margin = 4*scale), so invert that to fit the target panel.
    const auto scale = window_height >= 1000U ? 3U
        : (window_height >= 400U ? 2U : 1U);
    const auto target_width = window_width * 9U / 10U;
    const auto target_height = window_height * 9U / 10U;
    // Cap the line length at a readable ~80 columns and word-wrap to it, rather
    // than stretching lines across a wide window. The panel is then a fixed,
    // book-like width centred in the window.
    const auto columns = std::clamp<std::size_t>(
        (target_width / scale - 8U) / 6U, 24U, 80U);
    const auto rows = std::max<std::size_t>(
        6U, (target_height / scale - 8U) / 9U);
    license_overlay_.setViewport(columns, rows);
    const auto bitmap = rasterizeTextRows(
        license_overlay_.visibleRows(), scale, {8U, 12U, 28U},
        {224U, 232U, 255U});
    if (bitmap.rgba.empty() || bitmap.width == 0U || bitmap.height == 0U) {
        return;
    }
    if (license_overlay_texture_ == 0U) {
        glGenTextures(1, &license_overlay_texture_);
        glGenFramebuffers(1, &license_overlay_framebuffer_);
    }
    glBindTexture(GL_TEXTURE_2D, license_overlay_texture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(
        GL_TEXTURE_2D,
        0,
        GL_RGBA8,
        static_cast<GLsizei>(bitmap.width),
        static_cast<GLsizei>(bitmap.height),
        0,
        GL_RGBA,
        GL_UNSIGNED_BYTE,
        bitmap.rgba.data());

    const auto scissor_enabled = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, license_overlay_framebuffer_);
    glFramebufferTexture2D(
        GL_READ_FRAMEBUFFER,
        GL_COLOR_ATTACHMENT0,
        GL_TEXTURE_2D,
        license_overlay_texture_,
        0);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glDrawBuffer(GL_BACK);
    // Centre the panel, clamped to the window.
    const auto panel_width = std::min(
        static_cast<GLint>(bitmap.width), static_cast<GLint>(window_width));
    const auto panel_height = std::min(
        static_cast<GLint>(bitmap.height), static_cast<GLint>(window_height));
    const auto x0 = (static_cast<GLint>(window_width) - panel_width) / 2;
    const auto y0 = (static_cast<GLint>(window_height) - panel_height) / 2;
    glBlitFramebuffer(
        0,
        static_cast<GLint>(bitmap.height),
        panel_width,
        0,
        x0,
        y0,
        x0 + panel_width,
        y0 + panel_height,
        GL_COLOR_BUFFER_BIT,
        GL_NEAREST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glReadBuffer(GL_BACK);
    if (scissor_enabled != GL_FALSE) {
        glEnable(GL_SCISSOR_TEST);
    }
}

void PsyCrossPresenter::repeatFrame() {
    if (!initialized_) {
        return;
    }
    GR_RestoreCachedFrame();
    finishWindowPresentation();
}

void PsyCrossPresenter::captureDiagnosticFrame(
    std::span<const std::uint16_t> vram_snapshot,
    const std::vector<std::vector<std::uint32_t>>& packets,
    std::uint32_t display_x,
    std::uint32_t display_y,
    std::uint32_t display_width,
    std::uint32_t display_height) {
    // Long enough that a human who notices a transient artifact can still
    // reach the dump key: about a second and a half of retail 30 Hz frames.
    // Each entry holds a window-sized BGRA readback plus a 1 MB VRAM snapshot,
    // so this trades a few hundred megabytes for a usable capture window.
    constexpr std::size_t maximum_frames = 48U;
    DiagnosticFrame frame;
    frame.width = g_windowWidth > 0
        ? static_cast<std::uint32_t>(g_windowWidth)
        : window_width_;
    frame.height = g_windowHeight > 0
        ? static_cast<std::uint32_t>(g_windowHeight)
        : window_height_;
    frame.bgra.resize(
        static_cast<std::size_t>(frame.width) * frame.height * 4U);
    // Diagnostics are captured immediately after the normal swap, so read
    // the actual displayed image rather than the next render target.
    glReadBuffer(GL_FRONT);
    glReadPixels(
        0,
        0,
        static_cast<GLsizei>(frame.width),
        static_cast<GLsizei>(frame.height),
        GL_BGRA,
        GL_UNSIGNED_BYTE,
        frame.bgra.data());
    frame.vram.assign(vram_snapshot.begin(), vram_snapshot.end());
    frame.packets = packets;
    frame.display_x = display_x;
    frame.display_y = display_y;
    frame.display_width = display_width;
    frame.display_height = display_height;
    diagnostic_frames_.push_back(std::move(frame));
    while (diagnostic_frames_.size() > maximum_frames) {
        diagnostic_frames_.pop_front();
    }
}

void PsyCrossPresenter::dumpDiagnosticFrames() {
    std::size_t frame_index = 0U;
    for (const auto& frame : diagnostic_frames_) {
        ++frame_index;
        const auto stem =
            "FLICKER_EVENT_" + std::to_string(capture_trace_event_) + "_" +
            std::to_string(frame_index);
        const auto bitmap_destination =
            std::filesystem::path{"build"} / (stem + ".BMP");
        constexpr std::size_t bitmap_header_size = 54U;
        std::array<unsigned char, bitmap_header_size> bitmap_header{};
        bitmap_header[0] = 'B';
        bitmap_header[1] = 'M';
        const auto pixel_bytes =
            static_cast<std::uint32_t>(frame.bgra.size());
        writeLittleEndian32(
            bitmap_header, 2U, bitmap_header_size + pixel_bytes);
        writeLittleEndian32(bitmap_header, 10U, bitmap_header_size);
        writeLittleEndian32(bitmap_header, 14U, 40U);
        writeLittleEndian32(bitmap_header, 18U, frame.width);
        writeLittleEndian32(bitmap_header, 22U, frame.height);
        writeLittleEndian16(bitmap_header, 26U, 1U);
        writeLittleEndian16(bitmap_header, 28U, 32U);
        writeLittleEndian32(bitmap_header, 34U, pixel_bytes);
        std::ofstream bitmap_output{
            bitmap_destination, std::ios::binary | std::ios::trunc};
        bitmap_output.write(
            reinterpret_cast<const char*>(bitmap_header.data()),
            static_cast<std::streamsize>(bitmap_header.size()));
        bitmap_output.write(
            reinterpret_cast<const char*>(frame.bgra.data()),
            static_cast<std::streamsize>(frame.bgra.size()));

        const auto packet_destination =
            std::filesystem::path{"build"} / (stem + ".GP0");
        std::ofstream packet_output{
            packet_destination, std::ios::binary | std::ios::trunc};
        const auto write_word =
            [&packet_output](std::uint32_t word) {
                packet_output.write(
                    reinterpret_cast<const char*>(&word),
                    sizeof(word));
            };
        write_word(0x534D4750U);
        write_word(frame.display_x);
        write_word(frame.display_y);
        write_word(frame.display_width);
        write_word(frame.display_height);
        write_word(static_cast<std::uint32_t>(frame.packets.size()));
        for (const auto& packet : frame.packets) {
            write_word(static_cast<std::uint32_t>(packet.size()));
            for (const auto word : packet) {
                write_word(word);
            }
        }

        const auto vram_destination =
            std::filesystem::path{"build"} / (stem + ".VRAM");
        std::ofstream vram_output{
            vram_destination, std::ios::binary | std::ios::trunc};
        vram_output.write(
            reinterpret_cast<const char*>(frame.vram.data()),
            static_cast<std::streamsize>(
                frame.vram.size() * sizeof(std::uint16_t)));
    }
    std::cout << "frame_capture_ring_event=" << capture_trace_event_
              << " frames=" << diagnostic_frames_.size() << '\n';
}

void PsyCrossPresenter::captureRenderTarget(
    const std::filesystem::path& destination) {
    if (!initialized_) {
        return;
    }
    int width = 0;
    int height = 0;
    GR_GetWindowCaptureSize(&width, &height);
    const auto pixel_count =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    if (width <= 0 || height <= 0 ||
        window_capture_buffer_.size() != pixel_count * 4U) {
        throw std::runtime_error{
            "PsyCross window capture buffer is not ready"};
    }
    const auto& bgra = window_capture_buffer_;

    // glReadPixels returns bottom-up rows (GL origin is bottom-left), which is
    // exactly BMP's own row order, so no flip is needed. Written as 32-bit BGRA,
    // matching the diagnostic-ring dump above.
    constexpr std::size_t bitmap_header_size = 54U;
    std::array<unsigned char, bitmap_header_size> bitmap_header{};
    bitmap_header[0] = 'B';
    bitmap_header[1] = 'M';
    const auto pixel_bytes = static_cast<std::uint32_t>(bgra.size());
    writeLittleEndian32(
        bitmap_header, 2U, bitmap_header_size + pixel_bytes);
    writeLittleEndian32(bitmap_header, 10U, bitmap_header_size);
    writeLittleEndian32(bitmap_header, 14U, 40U);
    writeLittleEndian32(
        bitmap_header, 18U, static_cast<std::uint32_t>(width));
    writeLittleEndian32(
        bitmap_header, 22U, static_cast<std::uint32_t>(height));
    writeLittleEndian16(bitmap_header, 26U, 1U);
    writeLittleEndian16(bitmap_header, 28U, 32U);
    writeLittleEndian32(bitmap_header, 34U, pixel_bytes);
    std::ofstream bitmap_output{
        destination, std::ios::binary | std::ios::trunc};
    bitmap_output.write(
        reinterpret_cast<const char*>(bitmap_header.data()),
        static_cast<std::streamsize>(bitmap_header.size()));
    bitmap_output.write(
        reinterpret_cast<const char*>(bgra.data()),
        static_cast<std::streamsize>(bgra.size()));
}

void PsyCrossPresenter::presentPersistent(
    std::span<const std::uint16_t> vram,
    const std::vector<std::vector<std::uint32_t>>& packets,
    std::uint32_t display_x,
    std::uint32_t display_y,
    std::uint32_t display_width,
    std::uint32_t display_height,
    std::span<const GpuReplaySegment> segments) {
    constexpr std::size_t expected_vram_words = 1024U * 512U;
    if (vram.size() != expected_vram_words) {
        throw std::runtime_error{"PsyCross presenter received invalid VRAM"};
    }
    if (std::ranges::any_of(
            segments,
            [=](const GpuReplaySegment& segment) {
                return segment.vram.size() != expected_vram_words;
            })) {
        throw std::runtime_error{
            "PsyCross presenter received an invalid segmented VRAM snapshot"};
    }
    if (display_width == 0U || display_width > 1024U ||
        display_height == 0U || display_height > 512U) {
        throw std::runtime_error{
            "PsyCross presenter received invalid display dimensions"};
    }
    const auto frame_x = display_x;
    const auto frame_y = display_y;
    const auto frame_x1 = frame_x + display_width - 1U;
    const auto frame_y1 = frame_y + display_height - 1U;
    const auto overlay_bounds =
        widescreenOverlayBounds(render_width_, render_height_);

    DISPENV display{};
    DRAWENV draw{};
    SetDefDispEnv(
        &display,
        static_cast<int>(frame_x),
        static_cast<int>(frame_y),
        static_cast<int>(display_width),
        static_cast<int>(display_height));
    SetDefDrawEnv(
        &draw,
        static_cast<int>(frame_x),
        static_cast<int>(frame_y),
        static_cast<int>(display_width),
        static_cast<int>(display_height));
    draw.dfe = 1;
    // The defining difference from presentFrame: the render target is NOT
    // cleared. It persists across presents, so the guest's own 0x02 fills are
    // the only thing that clears it, and content drawn or written on an earlier
    // present survives until the guest overwrites it. That is the whole model.
    draw.isbg = 0;
    PutDispEnv(&display);
    PutDispEnv(&display);
    PutDrawEnv(&draw);

    const auto publish_vram =
        [](std::span<const std::uint16_t> snapshot) {
            GR_CopyVRAM(
                const_cast<unsigned short*>(snapshot.data()),
                0,
                0,
                1024,
                512,
                0,
                0);
            GR_UpdateVRAM();
        };
    // VRAM is texture memory and the source for the screens retail writes: seed
    // it from the ordered decoder snapshot so texture sampling and page copies
    // are correct. This does not touch the persistent render target, so it is
    // not the reconstruct reseed presentFrame does -- that reseed's damage was
    // clearing and rebuilding the *displayed* image, which is now the render
    // target and is never thrown away here.
    publish_vram(
        segments.empty()
            ? vram
            : std::span<const std::uint16_t>{segments.front().vram});

    static_cast<void>(PsyX_BeginScene());

    // Draw-area/offset state tracked exactly as the reconstruct path tracks it,
    // because the two-page VRAM setup means a world pass drawn to the back page
    // must be mapped into the display-local render target through its own E5
    // offset, while UI drawn on the display page maps through the frame origin.
    std::uint32_t draw_x0 = frame_x;
    std::uint32_t draw_y0 = frame_y;
    std::uint32_t draw_x1 = frame_x1;
    std::uint32_t draw_y1 = frame_y1;
    auto draw_offset_x = static_cast<std::int32_t>(frame_x);
    auto draw_offset_y = static_cast<std::int32_t>(frame_y);
    // The last environment E1, with draw-to-display forced on. Compositing a
    // written rectangle submits its own E1, so the stream's E1 is restored
    // afterwards to keep a following textured draw sampling the right page.
    std::optional<std::uint32_t> current_texpage;
    const auto restore_texpage = [&] {
        if (current_texpage.has_value()) {
            submitPrimitive(std::span{&*current_texpage, 1U});
        }
    };
    const auto intersects_display = [&](
        std::uint32_t x,
        std::uint32_t y,
        std::uint32_t width,
        std::uint32_t height) {
        return width != 0U && height != 0U && x <= frame_x1 &&
            x + width - 1U >= frame_x && y <= frame_y1 &&
            y + height - 1U >= frame_y;
    };
    const auto is_full_frame_target = [&] {
        return draw_y0 == frame_y &&
            draw_x1 >= draw_x0 + display_width - 1U &&
            draw_y1 >= draw_y0 + display_height - 1U;
    };

    const auto replay =
        [&](const std::vector<std::vector<std::uint32_t>>& source_packets) {
            for (const auto& packet : source_packets) {
                if (packet.empty()) {
                    continue;
                }
                const auto opcode =
                    static_cast<std::uint8_t>(packet.front() >> 24U);
                if (opcode == 0x02U && packet.size() >= 3U) {
                    // The guest's own frame clear. Honour it where it meets the
                    // display page, in display-local coordinates, and only
                    // there -- a fill of a texture page must not touch what is
                    // on screen.
                    const auto fill_x = packet[1] & 0x3FFU;
                    const auto fill_y = (packet[1] >> 16U) & 0x1FFU;
                    const auto width =
                        transferExtent(packet[2] & 0xFFFFU, 0x3FFU);
                    const auto height =
                        transferExtent(packet[2] >> 16U, 0x1FFU);
                    if (intersects_display(fill_x, fill_y, width, height)) {
                        const auto ix = std::max(fill_x, frame_x);
                        const auto iy = std::max(fill_y, frame_y);
                        const auto ix1 =
                            std::min(fill_x + width - 1U, frame_x1);
                        const auto iy1 =
                            std::min(fill_y + height - 1U, frame_y1);
                        GR_Clear(
                            static_cast<int>(ix - frame_x),
                            static_cast<int>(iy - frame_y),
                            static_cast<int>(ix1 - ix + 1U),
                            static_cast<int>(iy1 - iy + 1U),
                            static_cast<unsigned char>(packet[0]),
                            static_cast<unsigned char>(packet[0] >> 8U),
                            static_cast<unsigned char>(packet[0] >> 16U));
                    }
                    continue;
                }
                if (opcode == 0xA0U && packet.size() >= 3U) {
                    // A CPU-to-VRAM upload targeting the display page: lift the
                    // written rectangle out of VRAM into the render target.
                    const auto dst_x = packet[1] & 0x3FFU;
                    const auto dst_y = (packet[1] >> 16U) & 0x1FFU;
                    const auto width =
                        transferExtent(packet[2] & 0xFFFFU, 0x3FFU);
                    const auto height =
                        transferExtent(packet[2] >> 16U, 0x1FFU);
                    if (intersects_display(dst_x, dst_y, width, height)) {
                        compositeDisplayRect(
                            dst_x, dst_y, width, height, frame_x, frame_y);
                        restore_texpage();
                    }
                    continue;
                }
                if (opcode >= 0x80U && opcode <= 0x9FU &&
                    packet.size() >= 4U) {
                    // A VRAM-to-VRAM copy whose destination is on the display
                    // page: its result is already in the seeded VRAM, so
                    // compositing the destination rectangle shows it.
                    const auto dst_x = packet[2] & 0x3FFU;
                    const auto dst_y = (packet[2] >> 16U) & 0x1FFU;
                    const auto width =
                        transferExtent(packet[3] & 0xFFFFU, 0x3FFU);
                    const auto height =
                        transferExtent(packet[3] >> 16U, 0x1FFU);
                    if (intersects_display(dst_x, dst_y, width, height)) {
                        compositeDisplayRect(
                            dst_x, dst_y, width, height, frame_x, frame_y);
                        restore_texpage();
                    }
                    continue;
                }
                if (opcode == 0xE3U) {
                    draw_x0 = packet[0] & 0x3FFU;
                    draw_y0 = (packet[0] >> 10U) & 0x1FFU;
                } else if (opcode == 0xE4U) {
                    draw_x1 = packet[0] & 0x3FFU;
                    draw_y1 = (packet[0] >> 10U) & 0x1FFU;
                } else if (opcode == 0xE5U) {
                    draw_offset_x = signed11(packet[0]);
                    draw_offset_y = signed11(packet[0] >> 11U);
                }
                if (!replayable(opcode) || packet.size() > 0xFFFFU) {
                    continue;
                }
                // PsyCross accepts polygons beyond the original GPU's
                // 1023x511 screen-space limits. A near-plane crossing can
                // therefore turn geometry behind the camera into a giant
                // clipped quad. Apply the hardware rejection before PGXP
                // converts the retained GP0 coordinates.
                if (psx::gpuPolygonExceedsDrawingLimits(packet)) {
                    continue;
                }
                auto replay_packet = packet;
                if (opcode >= 0x40U && opcode <= 0x5FU &&
                    (opcode & 0x04U) != 0U) {
                    constexpr std::uint32_t line_anti_aliasing_bit = 1U << 26U;
                    replay_packet[0] &= ~line_anti_aliasing_bit;
                }
                if (opcode >= 0x60U && opcode <= 0x7FU &&
                    (opcode & 0x01U) != 0U) {
                    constexpr std::uint32_t raw_texture_bit = 1U << 24U;
                    replay_packet[0] &= ~raw_texture_bit;
                    if ((opcode & 0x04U) != 0U) {
                        replay_packet[0] =
                            (replay_packet[0] & 0xFF000000U) | 0x00808080U;
                    }
                }
                if (opcode == 0xE1U) {
                    constexpr std::uint32_t draw_to_display_bit = 1U << 10U;
                    replay_packet[0] |= draw_to_display_bit;
                    current_texpage = replay_packet[0];
                } else if (opcode >= 0xE3U && opcode <= 0xE5U) {
                    // Coordinate normalization uses the retail draw area; a
                    // single host draw area covers the render target.
                    continue;
                } else if (opcode >= 0x20U && opcode <= 0x7FU) {
                    // A world pass drawn to the back page maps through its own
                    // E5 offset; UI drawn on the display page maps through the
                    // frame origin. This is coordinate correctness for the two
                    // PS1 pages, not a heuristic about which page to keep --
                    // every drawn primitive is kept and forced to the render
                    // target, because within a flip interval the guest draws
                    // exactly one frame.
                    const auto full_frame_target = is_full_frame_target();
                    const auto origin_x = full_frame_target
                        ? draw_offset_x
                        : static_cast<std::int32_t>(frame_x);
                    const auto origin_y = full_frame_target
                        ? draw_offset_y
                        : static_cast<std::int32_t>(frame_y);
                    // Extend cinematic bars and full-width fades to the host
                    // window edges under expanded PGXP projection, exactly as
                    // the reconstruct path does. The persistent path never
                    // interpolates, so the "do not interpolate extended
                    // positions" caveat is satisfied by construction.
                    static_cast<void>(extendDarkOverlayToWidescreen(
                        replay_packet,
                        draw_offset_x,
                        origin_x,
                        display_width,
                        overlay_bounds));
                    translatePrimitive(
                        replay_packet,
                        opcode,
                        nullptr,
                        {},
                        1.0F,
                        draw_offset_x,
                        draw_offset_y,
                        origin_x,
                        origin_y,
                        /*convert_rectangle_sizes=*/true);
                }
                submitPrimitive(replay_packet);
            }
        };

    if (segments.empty()) {
        replay(packets);
        DrawSync(0);
    } else {
        for (std::size_t index = 0U; index < segments.size(); ++index) {
            if (index != 0U) {
                publish_vram(segments[index].vram);
            }
            replay(segments[index].packets);
            DrawSync(0);
        }
    }

    GR_CacheFrameForRepeat();
    const bool capture_this_frame = !render_target_capture_path_.empty();
    if (capture_this_frame) {
        int capture_width = 0;
        int capture_height = 0;
        GR_GetWindowCaptureSize(&capture_width, &capture_height);
        window_capture_buffer_.assign(
            static_cast<std::size_t>(capture_width) * capture_height * 4U, 0U);
        GR_RequestWindowCapture(window_capture_buffer_.data());
    }
    // Keep store off: the displayed image is the persistent render target, and
    // VRAM stays as the guest's own upload-backed texture memory, reseeded from
    // the decoder each present. Storing the downsampled render target back into
    // VRAM would only cost fidelity for the rare case of a later frame sampling
    // a drawn region as a texture; revisit if such a defect appears.
    PsyX_EndSceneNoVramStoreNoSwap();
    finishWindowPresentation();
    if (capture_this_frame) {
        captureRenderTarget(render_target_capture_path_);
        render_target_capture_path_.clear();
    }
    // Feed the L3 frame-capture-trace ring (--frame-capture-trace) from the same
    // present the window shows, so the diagnostic keeps working now that this is
    // the only present path.
    if (capture_frame_trace_) {
        captureDiagnosticFrame(
            vram,
            packets,
            display_x,
            display_y,
            display_width,
            display_height);
        glReadBuffer(GL_BACK);
    }
}

[[noreturn]] void PsyCrossPresenter::showUntilClosed(
    std::span<const std::uint16_t> vram,
    const std::vector<std::vector<std::uint32_t>>& packets,
    std::uint32_t display_x,
    std::uint32_t display_y,
    std::uint32_t display_width,
    std::uint32_t display_height) {
    while (true) {
        presentPersistent(
            vram,
            packets,
            display_x,
            display_y,
            display_width,
            display_height);
    }
}

void PsyCrossPresenter::captureScreenshot(
    std::span<const std::uint16_t> vram,
    const std::vector<std::vector<std::uint32_t>>& packets,
    std::uint32_t display_x,
    std::uint32_t display_y,
    std::uint32_t display_width,
    std::uint32_t display_height) {
    // Faithful capture through the same present path live uses: arm the
    // render-target readback and present. This replaces the old
    // PsyX_TakeScreenshot stale-framebuffer grab.
    captureNextPresent("SCREENSHOT.BMP");
    presentPersistent(
        vram,
        packets,
        display_x,
        display_y,
        display_width,
        display_height);
}

} // namespace stuntmaster::presentation
