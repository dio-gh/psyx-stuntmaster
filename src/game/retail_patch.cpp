#include "stuntmaster/game/retail_patch.hpp"

#include <algorithm>
#include <utility>

namespace stuntmaster::game {
namespace {

constexpr std::uint32_t jump_opcode = 0x02U;

// Retail's own screen geometry, as baked into RP_ZCullGClip's immediates.
constexpr double retail_screen_width = 512.0;
constexpr double retail_screen_centre_x = 256.0;
// PS1 pixels are 4:3 across a 512-wide screen.
constexpr double retail_pixel_aspect = 3.0 / 4.0;
constexpr std::uint16_t retail_screen_x_limit = 0x200U;

// The widescreen lower-bound trampolines sit above the retiming words. The
// original sixteen compact slots retain their old addresses so quick saves
// made by earlier builds remain recognizable. The four NCLIP guards and four
// final-outcode guards are larger and use separate 0xC0-byte slots.
constexpr std::uint32_t widescreen_arena_offset = 0x400U;
constexpr std::uint32_t widescreen_arena_stride = 0x20U;
constexpr std::uint32_t widescreen_guard_arena_offset = 0x600U;
constexpr std::uint32_t widescreen_guard_arena_stride = 0xC0U;
constexpr std::uint32_t widescreen_outcode_arena_offset = 0xC00U;
constexpr std::uint32_t widescreen_outcode_arena_stride = 0xC0U;
constexpr std::uint32_t widescreen_block_visibility_arena_offset = 0xF00U;
static_assert(widescreen_guard_arena_stride >= (44U + 2U) * 4U);
static_assert(
    widescreen_guard_arena_offset + 4U * widescreen_guard_arena_stride <=
    0x900U); // ledge-trace diagnostics begin here
static_assert(widescreen_outcode_arena_stride >= (23U + 2U) * 4U);
static_assert(
    widescreen_outcode_arena_offset +
        4U * widescreen_outcode_arena_stride <=
    psx::R3000Runtime::patch_arena_size);
static_assert(
    widescreen_block_visibility_arena_offset + 4U * 4U <=
    psx::R3000Runtime::patch_arena_size);

// `RP_ZCullGClip__FP9tGeometry` repeats one outcode block per vertex, four
// times for its two quad loops and three times for its two triangle loops.
// Every one is byte-identical, and the site is the `subu` that begins it.
constexpr std::array<std::uint32_t, 14> widescreen_block_sites{
    0x800A0FD4U,
    0x800A0FF0U,
    0x800A100CU,
    0x800A1028U,
    0x800A1150U,
    0x800A116CU,
    0x800A1188U,
    0x800A11A4U,
    0x800A12B8U,
    0x800A12D4U,
    0x800A12F0U,
    0x800A13ECU,
    0x800A1408U,
    0x800A1424U,
};

constexpr std::size_t widescreen_legacy_trampoline_count = 16U;

constexpr std::array<std::string_view, 25> widescreen_site_names{
    "widescreen_block_left_0",
    "widescreen_block_left_1",
    "widescreen_block_left_2",
    "widescreen_block_left_3",
    "widescreen_block_left_4",
    "widescreen_block_left_5",
    "widescreen_block_left_6",
    "widescreen_block_left_7",
    "widescreen_block_left_8",
    "widescreen_block_left_9",
    "widescreen_block_left_10",
    "widescreen_block_left_11",
    "widescreen_block_left_12",
    "widescreen_block_left_13",
    "widescreen_model_left",
    "widescreen_sphere_left",
    "widescreen_quad_nclip_guard_0",
    "widescreen_quad_nclip_guard_1",
    "widescreen_triangle_nclip_guard_0",
    "widescreen_triangle_nclip_guard_1",
    "widescreen_quad_outcode_guard_0",
    "widescreen_quad_outcode_guard_1",
    "widescreen_triangle_outcode_guard_0",
    "widescreen_triangle_outcode_guard_1",
    "widescreen_block_visibility_left",
};

[[nodiscard]] constexpr std::uint32_t encodeJump(
    std::uint32_t target) noexcept {
    return (jump_opcode << 26U) | ((target >> 2U) & 0x03FFFFFFU);
}

// A `j` only reaches within the current 256 MB region.
[[nodiscard]] constexpr bool jumpReaches(
    std::uint32_t from, std::uint32_t to) noexcept {
    return ((from + 4U) & 0xF0000000U) == (to & 0xF0000000U);
}

// Writes a trampoline's body plus its return jump and delay slot.
//
// The arena has to be empty first, which is how two patches that would overlap
// are caught.
[[nodiscard]] bool installTrampolineBody(
    psx::R3000Runtime& runtime,
    const RetailTrampoline& patch) noexcept {
    const auto word_count = patch.body.size() + 2U;
    const auto byte_count =
        static_cast<std::uint32_t>(word_count * sizeof(std::uint32_t));
    if (byte_count > psx::R3000Runtime::patch_arena_size ||
        patch.address < psx::R3000Runtime::patch_arena_base ||
        patch.address - psx::R3000Runtime::patch_arena_base >
            psx::R3000Runtime::patch_arena_size - byte_count) {
        return false;
    }
    if (!jumpReaches(patch.site, patch.address) ||
        !jumpReaches(
            patch.address + byte_count - 8U, patch.return_address)) {
        return false;
    }

    const auto expected = [&](std::size_t index) {
        if (index < patch.body.size()) {
            return patch.body[index];
        }
        return index == patch.body.size()
            ? encodeJump(patch.return_address)
            : 0U;
    };
    for (std::size_t index = 0U; index < word_count; ++index) {
        std::uint32_t existing = 0U;
        const auto address = patch.address +
            static_cast<std::uint32_t>(index * sizeof(std::uint32_t));
        if (!runtime.read32(address, existing)) {
            return false;
        }
        if (existing != 0U && existing != expected(index)) {
            return false;
        }
    }

    for (std::size_t index = 0U; index < word_count; ++index) {
        if (!runtime.write32(
                patch.address +
                    static_cast<std::uint32_t>(index * sizeof(std::uint32_t)),
                expected(index))) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool writeWhenSiteMatches(
    psx::R3000Runtime& runtime,
    const RetailPatch& patch,
    std::uint32_t expected,
    std::uint32_t replacement) noexcept {
    const auto observed = readPatchSite(runtime, patch);
    if (!observed.has_value() || *observed != expected) {
        return false;
    }
    return runtime.write32(patch.address, replacement);
}

} // namespace

const RetailPatch& thirtyHertzSwapGate() noexcept {
    // sltiu $v0, $v0, 2  ->  sltiu $v0, $v0, 1
    static constexpr RetailPatch patch{
        "vscallback_swap_gate",
        0x800A015CU,
        0x2C420002U,
        0x2C420001U};
    return patch;
}

std::optional<std::uint32_t> readPatchSite(
    const psx::R3000Runtime& runtime, const RetailPatch& patch) noexcept {
    std::uint32_t word = 0U;
    if (!runtime.read32(patch.address, word)) {
        return std::nullopt;
    }
    return word;
}

bool applyRetailPatch(
    psx::R3000Runtime& runtime, const RetailPatch& patch) noexcept {
    return writeWhenSiteMatches(
        runtime, patch, patch.original_word, patch.patched_word);
}

bool revertRetailPatch(
    psx::R3000Runtime& runtime, const RetailPatch& patch) noexcept {
    return writeWhenSiteMatches(
        runtime, patch, patch.patched_word, patch.original_word);
}

std::uint16_t widescreenXLimit(
    std::uint32_t window_width, std::uint32_t window_height) noexcept {
    const auto viewport_limit = widescreenBlockVisibilityXLimit(
        window_width, window_height);
    return viewport_limit;
}

std::uint16_t widescreenBlockVisibilityXLimit(
    std::uint32_t window_width, std::uint32_t window_height) noexcept {
    if (window_width == 0U || window_height == 0U) {
        return retail_screen_x_limit;
    }
    // Retail projects into a 512-wide screen of 4:3 pixels, centred on 256.
    const auto aspect = static_cast<double>(window_width) /
        static_cast<double>(window_height);
    const auto half_width = 0.5 * retail_screen_width * aspect *
        retail_pixel_aspect;
    auto limit = static_cast<long>(
        retail_screen_centre_x + half_width + 0.5);
    return static_cast<std::uint16_t>(
        std::clamp<long>(limit, retail_screen_x_limit, 1022L));
}

std::array<RetailPatch, 2> widescreenBlockCull(
    std::uint16_t x_limit) noexcept {
    // ori $t8, $t8, 0x200 -> ori $t8, $t8, x_limit  (the right bound)
    const auto limit_word = 0x37180000U | x_limit;
    return {{
        {"widescreen_block_limit_a", 0x800A0E54U, 0x37180200U, limit_word},
        {"widescreen_block_limit_b", 0x800A0E5CU, 0x37180200U, limit_word},
    }};
}

std::array<RetailPatch, 2> widescreenModelCull(
    std::uint16_t x_limit) noexcept {
    return {{
        // lhu $s6, 0x60($v0) -> ori $s6, $zero, x_limit. The sphere test keeps
        // its own copy of maxX for the radius-aware path, so widening only the
        // point test leaves objects entering from the right still clipped: they
        // fail the quick test, fall through to this path, and meet the original
        // bound.
        {"widescreen_sphere_limit",
         0x8009E2D8U,
         0x94560060U,
         0x34160000U | x_limit},
        // lh $v1, 0x60($v1) -> ori $v1, $zero, x_limit. Replaces the clip
        // box's right bound with the widescreen edge. This hardcodes what was
        // a lookup: retail keeps 512 there for the one screen this game uses,
        // and the fingerprint refuses if that ever stops being the instruction.
        {"widescreen_model_limit",
         0x8009E260U,
         0x84630060U,
         0x34030000U | x_limit},
    }};
}

const RetailPatch& legacyWidescreenBlockVisibility() noexcept {
    // lw $v0, -0x30f8($v0) -> move $v0, $zero. The following branch selects
    // retail's active-list membership check when this flag is zero, instead of
    // comparing the block against only three camera-local IDs.
    static constexpr RetailPatch patch{
        "widescreen_block_visibility",
        0x8002ADC8U,
        0x8C42CF08U,
        0x00001021U};
    return patch;
}

RetailPatch widescreenBlockVisibilityLimit(
    std::uint16_t x_limit) noexcept {
    return {
        "widescreen_block_visibility_limit",
        0x8002A198U,
        0x84A40060U, // lh $a0, 0x60($a1)
        0x34040000U | x_limit}; // ori $a0, $zero, x_limit
}

WidescreenLowerBounds::WidescreenLowerBounds(
    std::uint16_t x_limit,
    std::uint16_t block_visibility_x_limit) noexcept {
    // Retail's visible band is [0, 512]; the widened one is
    // [512 - x_limit, x_limit], still centred on 256. Adding this bias to a
    // coordinate therefore moves retail's own "< 0" test onto the widescreen
    // left edge without disturbing the right bound, which keeps reading the
    // unbiased coordinate.
    const auto bias = x_limit > retail_screen_x_limit
        ? static_cast<std::uint32_t>(x_limit - retail_screen_x_limit)
        : 0U;

    const auto slot = [](std::size_t index) {
        if (index >= cull_site_count + guard_site_count +
                outcode_guard_site_count) {
            return psx::R3000Runtime::patch_arena_base +
                widescreen_block_visibility_arena_offset;
        }
        if (index >= cull_site_count + guard_site_count) {
            return psx::R3000Runtime::patch_arena_base +
                widescreen_outcode_arena_offset +
                static_cast<std::uint32_t>(
                    index - cull_site_count - guard_site_count) *
                    widescreen_outcode_arena_stride;
        }
        if (index >= widescreen_legacy_trampoline_count) {
            return psx::R3000Runtime::patch_arena_base +
                widescreen_guard_arena_offset +
                static_cast<std::uint32_t>(
                    index - widescreen_legacy_trampoline_count) *
                    widescreen_guard_arena_stride;
        }
        return psx::R3000Runtime::patch_arena_base +
            widescreen_arena_offset +
            static_cast<std::uint32_t>(index) * widescreen_arena_stride;
    };
    const auto body_of = [this](std::size_t index, std::size_t length) {
        return std::span<const std::uint32_t>{
            bodies_[index].data(), length};
    };

    for (std::size_t index = 0U; index < block_site_count; ++index) {
        // The site's own delay slot is `and $a1, $a1, $t7`, which runs on a
        // stale $a1 before the jump and again on the correct one after the
        // return, so the body only has to leave $a1 right. $at is dead across
        // an outcode block, and the mask keeps the biased X out of the packed
        // Y half when the addition carries.
        bodies_[index] = {
            0x03032823U,        // subu  $a1, $t8, $v1   ; displaced
            0x20610000U | bias, // addiu $at, $v1, bias
            0x3021FFFFU,        // andi  $at, $at, 0xffff
            0x00031C02U,        // srl   $v1, $v1, 16
            0x00031C00U,        // sll   $v1, $v1, 16
            0x00611825U,        // or    $v1, $v1, $at
        };
        patches_[index] = RetailTrampoline{
            widescreen_site_names[index],
            widescreen_block_sites[index],
            0x03032823U, // subu $a1, $t8, $v1
            slot(index),
            widescreen_block_sites[index] + 4U,
            body_of(index, 6U)};
    }

    // P3DClipCode's point test. Its lower bound is the packed sign bit, so the
    // biased X contributes bit 15 and the untouched Y keeps bit 31; the retail
    // instruction after the site shifts both down by one as before.
    constexpr std::size_t model_index = block_site_count;
    bodies_[model_index] = {
        0x20830000U, // addiu $v1, $a0, bias   ; patched below
        0x30638000U, // andi  $v1, $v1, 0x8000 ; biased X sign only
        0x3C018000U, // lui   $at, 0x8000
        0x00811024U, // and   $v0, $a0, $at    ; unbiased Y sign only
        0x00431025U, // or    $v0, $v0, $v1
        0x00000000U,
    };
    bodies_[model_index][0] |= bias;
    patches_[model_index] = RetailTrampoline{
        widescreen_site_names[model_index],
        0x8009E248U,
        0x00861024U, // and $v0, $a0, $a2
        slot(model_index),
        0x8009E24CU,
        body_of(model_index, 5U)};

    // P3DClipCodeSphere's radius-aware left test, `x + radius < 0`. Sign
    // extension has already happened here, so this is plain 32-bit arithmetic
    // with no packed half to protect. The site is the mask rather than the
    // addition, because the addition's own delay slot is the branch.
    constexpr std::size_t sphere_index = block_site_count + 1U;
    bodies_[sphere_index] = {
        0x3084FFFFU, // andi  $a0, $a0, 0xffff ; displaced
        0x00641021U, // addu  $v0, $v1, $a0
        0x20420000U, // addiu $v0, $v0, bias   ; patched below
        0x00000000U,
        0x00000000U,
        0x00000000U,
    };
    bodies_[sphere_index][2] |= bias;
    patches_[sphere_index] = RetailTrampoline{
        widescreen_site_names[sphere_index],
        0x8009E39CU,
        0x3084FFFFU, // andi $a0, $a0, 0xffff
        slot(sphere_index),
        0x8009E3A4U,
        body_of(sphere_index, 3U)};

    // GTE NCLIP works on saturated projected coordinates. Outside the retail
    // 4:3 band a front-facing polygon can therefore collapse or reverse its
    // projected winding even though all of its vertices remain in front of
    // the eye. Keep retail's result for any zero-depth polygon (the dangerous
    // behind-camera case), and for ordinary backfaces whose complete vertex
    // set remains inside [0, 512]. An NCLIP rejection advances as soon as a
    // front-of-camera polygon reaches either widescreen margin. The following
    // widened outcode still rejects polygons wholly outside the widened band.
    const auto make_quad_guard = [&](std::size_t index,
                                     std::uint32_t site,
                                     std::uint32_t original,
                                     std::uint32_t cull) {
        bodies_[index] = {
            0x0440002BU,     // bltz  $v0, return/run outcode
            0x00000000U,     // nop
            0x94C30004U,     // lhu   $v1, 4($a2)
            0x00000000U,     // nop   ; load delay
            0x10600025U,     // beqz  $v1, cull
            0x00000000U,     // nop
            0x94E30004U,     // lhu   $v1, 4($a3)
            0x00000000U,     // nop   ; load delay
            0x10600021U,     // beqz  $v1, cull
            0x00000000U,     // nop
            0x95030004U,     // lhu   $v1, 4($t0)
            0x00000000U,     // nop   ; load delay
            0x1060001DU,     // beqz  $v1, cull
            0x00000000U,     // nop
            0x95230004U,     // lhu   $v1, 4($t1)
            0x00000000U,     // nop   ; load delay
            0x10600019U,     // beqz  $v1, cull
            0x00000000U,     // nop
            0x84C30000U,     // lh    $v1, 0($a2) ; projected X
            0x00000000U,     // nop   ; load delay
            0x24610000U,     // addiu $at, $v1, 0
            0x2C210201U,     // sltiu $at, $at, 513 ; inside retail band
            0x10200015U,     // beqz  $at, return/run widened outcode
            0x00000000U,     // nop
            0x84E30000U,     // lh    $v1, 0($a3)
            0x00000000U,     // nop   ; load delay
            0x24610000U,     // addiu $at, $v1, 0
            0x2C210201U,     // sltiu $at, $at, 513
            0x1020000FU,     // beqz  $at, return/run outcode
            0x00000000U,     // nop
            0x85030000U,     // lh    $v1, 0($t0)
            0x00000000U,     // nop   ; load delay
            0x24610000U,     // addiu $at, $v1, 0
            0x2C210201U,     // sltiu $at, $at, 513
            0x10200009U,     // beqz  $at, return/run outcode
            0x00000000U,     // nop
            0x85230000U,     // lh    $v1, 0($t1)
            0x00000000U,     // nop   ; load delay
            0x24610000U,     // addiu $at, $v1, 0
            0x2C210201U,     // sltiu $at, $at, 513
            0x10200003U,     // beqz  $at, return/run outcode
            0x00000000U,     // nop
            encodeJump(cull),
            0x00000000U,
        };
        patches_[index] = RetailTrampoline{
            widescreen_site_names[index], site, original,
            slot(index), site + 8U, body_of(index, 44U)};
    };
    const auto make_triangle_guard = [&](std::size_t index,
                                         std::uint32_t site,
                                         std::uint32_t original,
                                         std::uint32_t cull) {
        bodies_[index] = {
            0x04400021U,     // bltz  $v0, return/run outcode
            0x00000000U,     // nop
            0x94C30004U,     // lhu   $v1, 4($a2)
            0x00000000U,     // nop   ; load delay
            0x1060001BU,     // beqz  $v1, cull
            0x00000000U,     // nop
            0x94E30004U,     // lhu   $v1, 4($a3)
            0x00000000U,     // nop   ; load delay
            0x10600017U,     // beqz  $v1, cull
            0x00000000U,     // nop
            0x95030004U,     // lhu   $v1, 4($t0)
            0x00000000U,     // nop   ; load delay
            0x10600013U,     // beqz  $v1, cull
            0x00000000U,     // nop
            0x84C30000U,     // lh    $v1, 0($a2) ; projected X
            0x00000000U,     // nop   ; load delay
            0x24610000U,     // addiu $at, $v1, 0
            0x2C210201U,     // sltiu $at, $at, 513
            0x1020000FU,     // beqz  $at, return/run widened outcode
            0x00000000U,     // nop
            0x84E30000U,     // lh    $v1, 0($a3)
            0x00000000U,     // nop   ; load delay
            0x24610000U,     // addiu $at, $v1, 0
            0x2C210201U,     // sltiu $at, $at, 513
            0x10200009U,     // beqz  $at, return/run outcode
            0x00000000U,     // nop
            0x85030000U,     // lh    $v1, 0($t0)
            0x00000000U,     // nop   ; load delay
            0x24610000U,     // addiu $at, $v1, 0
            0x2C210201U,     // sltiu $at, $at, 513
            0x10200003U,     // beqz  $at, return/run outcode
            0x00000000U,     // nop
            encodeJump(cull),
            0x00000000U,
        };
        patches_[index] = RetailTrampoline{
            widescreen_site_names[index], site, original,
            slot(index), site + 8U, body_of(index, 34U)};
    };

    make_quad_guard(
        cull_site_count, 0x800A0FC0U, 0x0441003EU, 0x800A10BCU);
    make_quad_guard(
        cull_site_count + 1U, 0x800A113CU, 0x0441003EU, 0x800A1238U);
    make_triangle_guard(
        cull_site_count + 2U, 0x800A12A4U, 0x04410031U, 0x800A136CU);
    make_triangle_guard(
        cull_site_count + 3U, 0x800A13D8U, 0x04410031U, 0x800A14A0U);

    // The ANDed outcode can still reject a very large polygon when every
    // projected vertex saturates beyond the same horizontal edge, even though
    // the polygon's interior reaches the expanded view. Advance only a shared
    // horizontal rejection, and only when every transformed vertex has
    // positive camera depth. Shared top/bottom rejection and the zero-depth
    // near/behind-camera case keep retail's branch.
    const auto make_quad_outcode_guard = [&](std::size_t index,
                                              std::uint32_t site,
                                              std::uint32_t original,
                                              std::uint32_t cull) {
        bodies_[index] = {
            0x10400016U,     // beqz  $v0, return/submit
            0x00000000U,     // nop
            0x3041C000U,     // andi  $at, $v0, horizontal outcode bits
            0x10200011U,     // beqz  $at, cull (vertical-only rejection)
            0x00000000U,     // nop
            0x94C30004U,     // lhu   $v1, 4($a2)
            0x00000000U,     // nop   ; load delay
            0x1060000DU,     // beqz  $v1, cull
            0x00000000U,     // nop
            0x94E30004U,     // lhu   $v1, 4($a3)
            0x00000000U,     // nop   ; load delay
            0x10600009U,     // beqz  $v1, cull
            0x00000000U,     // nop
            0x95030004U,     // lhu   $v1, 4($t0)
            0x00000000U,     // nop   ; load delay
            0x10600005U,     // beqz  $v1, cull
            0x00000000U,     // nop
            0x95230004U,     // lhu   $v1, 4($t1)
            0x00000000U,     // nop   ; load delay
            0x14600003U,     // bnez  $v1, return/submit
            0x00000000U,     // nop
            encodeJump(cull),
            0x00000000U,
        };
        patches_[index] = RetailTrampoline{
            widescreen_site_names[index], site, original,
            slot(index), site + 8U, body_of(index, 23U)};
    };
    const auto make_triangle_outcode_guard = [&](std::size_t index,
                                                  std::uint32_t site,
                                                  std::uint32_t original,
                                                  std::uint32_t cull) {
        bodies_[index] = {
            0x10400012U,     // beqz  $v0, return/submit
            0x00000000U,     // nop
            0x3041C000U,     // andi  $at, $v0, horizontal outcode bits
            0x1020000DU,     // beqz  $at, cull (vertical-only rejection)
            0x00000000U,     // nop
            0x94C30004U,     // lhu   $v1, 4($a2)
            0x00000000U,     // nop   ; load delay
            0x10600009U,     // beqz  $v1, cull
            0x00000000U,     // nop
            0x94E30004U,     // lhu   $v1, 4($a3)
            0x00000000U,     // nop   ; load delay
            0x10600005U,     // beqz  $v1, cull
            0x00000000U,     // nop
            0x95030004U,     // lhu   $v1, 4($t0)
            0x00000000U,     // nop   ; load delay
            0x14600003U,     // bnez  $v1, return/submit
            0x00000000U,     // nop
            encodeJump(cull),
            0x00000000U,
        };
        patches_[index] = RetailTrampoline{
            widescreen_site_names[index], site, original,
            slot(index), site + 8U, body_of(index, 19U)};
    };

    constexpr auto outcode_index = cull_site_count + guard_site_count;
    make_quad_outcode_guard(
        outcode_index, 0x800A103CU, 0x1440001FU, 0x800A10BCU);
    make_quad_outcode_guard(
        outcode_index + 1U, 0x800A11B8U, 0x1440001FU, 0x800A1238U);
    make_triangle_outcode_guard(
        outcode_index + 2U, 0x800A1304U, 0x14400019U, 0x800A136CU);
    make_triangle_outcode_guard(
        outcode_index + 3U, 0x800A1438U, 0x14400019U, 0x800A14A0U);

    // The earlier block-level ClipCode repeats the same hardcoded 0..512 X
    // band before a Block ever reaches Block::Draw. Bias its signed lower test
    // to the widescreen left edge; its upper load is patched separately.
    constexpr auto block_visibility_index =
        cull_site_count + guard_site_count + outcode_guard_site_count;
    const auto block_visibility_bias =
        block_visibility_x_limit > retail_screen_x_limit
        ? static_cast<std::uint32_t>(
              block_visibility_x_limit - retail_screen_x_limit)
        : 0U;
    bodies_[block_visibility_index] = {
        0x24410000U | block_visibility_bias,
        0x00011FC3U,        // sra   $v1, $at, 31
    };
    patches_[block_visibility_index] = RetailTrampoline{
        widescreen_site_names[block_visibility_index],
        0x8002A194U,
        0x00021FC3U, // sra $v1, $v0, 31
        slot(block_visibility_index),
        0x8002A198U,
        body_of(block_visibility_index, 2U)};
}

std::span<const RetailTrampoline> WidescreenLowerBounds::patches()
    const noexcept {
    return patches_;
}

bool applyRetailPatches(
    psx::R3000Runtime& runtime,
    std::span<const RetailPatch> patches) noexcept {
    for (const auto& patch : patches) {
        const auto observed = readPatchSite(runtime, patch);
        if (!observed.has_value() || *observed != patch.original_word) {
            return false;
        }
    }
    for (const auto& patch : patches) {
        if (!applyRetailPatch(runtime, patch)) {
            return false;
        }
    }
    return true;
}

bool revertRetailPatches(
    psx::R3000Runtime& runtime,
    std::span<const RetailPatch> patches) noexcept {
    for (const auto& patch : patches) {
        const auto observed = readPatchSite(runtime, patch);
        if (!observed.has_value() || *observed != patch.patched_word) {
            return false;
        }
    }
    for (const auto& patch : patches) {
        if (!revertRetailPatch(runtime, patch)) {
            return false;
        }
    }
    return true;
}

bool setWidescreenCull(
    psx::R3000Runtime& runtime,
    std::uint16_t x_limit,
    std::uint16_t block_visibility_x_limit,
    bool enabled) noexcept {
    // First identify the complete current state without changing it. A saved
    // machine may contain the old exact-edge limit, a different aspect's
    // limit, the current guarded limit, or the untouched retail words.
    const auto probe_blocks = widescreenBlockCull(retail_screen_x_limit);
    const auto probe_models = widescreenModelCull(retail_screen_x_limit);
    const WidescreenLowerBounds probe_lower{
        retail_screen_x_limit, retail_screen_x_limit};
    bool all_original = true;
    bool all_patched = true;
    bool nclip_guard_original = true;
    bool nclip_guard_patched = true;
    bool outcode_guard_original = true;
    bool outcode_guard_patched = true;
    bool visibility_original = true;
    bool visibility_patched = true;
    bool visibility_limit_original = true;
    bool visibility_limit_patched = true;
    bool visibility_lower_original = true;
    bool visibility_lower_patched = true;
    std::optional<std::uint16_t> observed_limit;
    std::optional<std::uint16_t> observed_visibility_limit;
    const auto inspect_immediate = [&](const RetailPatch& patch,
                                       std::uint32_t opcode_mask,
                                       std::uint32_t opcode) {
        std::uint32_t word{};
        if (!runtime.read32(patch.address, word)) {
            all_original = false;
            all_patched = false;
            return;
        }
        all_original = all_original && word == patch.original_word;
        const auto limit = static_cast<std::uint16_t>(word & 0xFFFFU);
        const auto recognized = (word & opcode_mask) == opcode &&
            limit > retail_screen_x_limit && limit <= 1023U &&
            (!observed_limit.has_value() || *observed_limit == limit);
        all_patched = all_patched && recognized;
        if (recognized) {
            observed_limit = limit;
        }
    };
    inspect_immediate(probe_blocks[0], 0xFFFF0000U, 0x37180000U);
    inspect_immediate(probe_blocks[1], 0xFFFF0000U, 0x37180000U);
    inspect_immediate(probe_models[0], 0xFFFF0000U, 0x34160000U);
    inspect_immediate(probe_models[1], 0xFFFF0000U, 0x34030000U);
    const auto probe_visibility_limit =
        widescreenBlockVisibilityLimit(retail_screen_x_limit);
    if (const auto observed = readPatchSite(
            runtime, probe_visibility_limit); observed.has_value()) {
        visibility_limit_original =
            *observed == probe_visibility_limit.original_word;
        const auto limit = static_cast<std::uint16_t>(*observed & 0xFFFFU);
        visibility_limit_patched =
            (*observed & 0xFFFF0000U) == 0x34040000U &&
            limit > retail_screen_x_limit && limit <= 1023U;
        if (visibility_limit_patched) {
            observed_visibility_limit = limit;
        }
        all_original = all_original && visibility_limit_original;
    } else {
        all_original = false;
        visibility_limit_original = false;
        visibility_limit_patched = false;
    }
    if (const auto observed = readPatchSite(
            runtime, legacyWidescreenBlockVisibility());
        observed.has_value()) {
        visibility_original =
            *observed == legacyWidescreenBlockVisibility().original_word;
        visibility_patched =
            *observed == legacyWidescreenBlockVisibility().patched_word;
        all_original = all_original && visibility_original;
    } else {
        all_original = false;
        visibility_original = false;
        visibility_patched = false;
    }
    std::size_t trampoline_index{};
    for (const auto& patch : probe_lower.patches()) {
        std::uint32_t word{};
        if (!runtime.read32(patch.site, word)) {
            all_original = false;
            all_patched = false;
            continue;
        }
        const auto jump = 0x08000000U |
            ((patch.address >> 2U) & 0x03FFFFFFU);
        all_original = all_original && word == patch.original_word;
        if (trampoline_index < widescreen_legacy_trampoline_count) {
            all_patched = all_patched && word == jump;
        } else if (trampoline_index <
                   widescreen_legacy_trampoline_count + 4U) {
            nclip_guard_original =
                nclip_guard_original && word == patch.original_word;
            nclip_guard_patched = nclip_guard_patched && word == jump;
        } else if (trampoline_index <
                   widescreen_legacy_trampoline_count + 8U) {
            outcode_guard_original =
                outcode_guard_original && word == patch.original_word;
            outcode_guard_patched = outcode_guard_patched && word == jump;
        } else {
            visibility_lower_original = word == patch.original_word;
            visibility_lower_patched = word == jump;
        }
        ++trampoline_index;
    }
    all_patched = all_patched &&
        (nclip_guard_original || nclip_guard_patched) &&
        (outcode_guard_original || outcode_guard_patched) &&
        (visibility_original || visibility_patched) &&
        (visibility_limit_original || visibility_limit_patched) &&
        (visibility_lower_original || visibility_lower_patched);
    if (!all_original && !all_patched) {
        return false;
    }

    if (all_patched) {
        const auto blocks = widescreenBlockCull(*observed_limit);
        const auto models = widescreenModelCull(*observed_limit);
        const auto saved_visibility_limit =
            observed_visibility_limit.value_or(*observed_limit);
        const WidescreenLowerBounds lower{
            *observed_limit, saved_visibility_limit};
        // Every site was validated above. Restore jump sites first so no path
        // can enter an arena body while it is being cleared.
        auto lower_patches = lower.patches();
        if (visibility_lower_patched && !revertRetailTrampoline(
                runtime, lower_patches.back())) {
            return false;
        }
        if (outcode_guard_patched) {
            for (std::size_t index =
                     widescreen_legacy_trampoline_count + 4U;
                 index < widescreen_legacy_trampoline_count + 8U; ++index) {
                if (!revertRetailTrampoline(runtime, lower_patches[index])) {
                    return false;
                }
            }
        }
        if (nclip_guard_patched) {
            for (std::size_t index = widescreen_legacy_trampoline_count;
                 index < widescreen_legacy_trampoline_count + 4U; ++index) {
                if (!revertRetailTrampoline(runtime, lower_patches[index])) {
                    return false;
                }
            }
        }
        for (std::size_t index = 0U;
             index < widescreen_legacy_trampoline_count; ++index) {
            const auto& patch = lower_patches[index];
            if (!revertRetailTrampoline(runtime, patch)) {
                return false;
            }
        }
        if (!revertRetailPatches(runtime, models) ||
            !revertRetailPatches(runtime, blocks)) {
            return false;
        }
        if (visibility_limit_patched && !revertRetailPatch(
                runtime,
                widescreenBlockVisibilityLimit(saved_visibility_limit))) {
            return false;
        }
        if (visibility_patched && !revertRetailPatch(
                runtime, legacyWidescreenBlockVisibility())) {
            return false;
        }
    }

    if (!enabled || x_limit <= retail_screen_x_limit) {
        return true;
    }
    const auto blocks = widescreenBlockCull(x_limit);
    const auto models = widescreenModelCull(x_limit);
    const auto visibility_limit =
        widescreenBlockVisibilityLimit(block_visibility_x_limit);
    const WidescreenLowerBounds lower{
        x_limit, block_visibility_x_limit};
    if (!applyRetailPatch(runtime, visibility_limit) ||
        !applyRetailPatches(runtime, blocks) ||
        !applyRetailPatches(runtime, models)) {
        return false;
    }
    const auto lower_patches = lower.patches();
    for (std::size_t index = 0U;
         index < widescreen_legacy_trampoline_count; ++index) {
        if (!applyRetailTrampoline(runtime, lower_patches[index])) {
            (void)setWidescreenCull(
                runtime, x_limit, block_visibility_x_limit, false);
            return false;
        }
    }
    if (!applyRetailTrampoline(runtime, lower_patches.back())) {
        (void)setWidescreenCull(
            runtime, x_limit, block_visibility_x_limit, false);
        return false;
    }
    return true;
}

namespace {

// The trace words, and the three bodies above them. All of it is outside every
// patch slot, and none of it is read by anything but the host.
constexpr std::uint32_t ledge_trace_calls_offset = 0x900U;
constexpr std::uint32_t ledge_trace_flags_offset = 0x904U;
constexpr std::uint32_t ledge_trace_contact_offset = 0x908U;
constexpr std::uint32_t ledge_trace_latch_offset = 0x90CU;
constexpr std::uint32_t ledge_trace_ticket_offset = 0x910U;
constexpr std::uint32_t ledge_trace_facing_dot_offset = 0x914U;
constexpr std::uint32_t ledge_trace_normal_x_offset = 0x918U;
constexpr std::uint32_t ledge_trace_normal_y_offset = 0x91CU;
constexpr std::uint32_t ledge_trace_normal_z_offset = 0x920U;
constexpr std::uint32_t ledge_trace_facing_x_offset = 0x924U;
constexpr std::uint32_t ledge_trace_facing_z_offset = 0x928U;
constexpr std::uint32_t ledge_trace_state_offset = 0x92CU;
constexpr std::uint32_t ledge_trace_velocity_y_offset = 0x930U;

static_assert(
    psx::R3000Runtime::patch_arena_base + ledge_trace_calls_offset ==
            ledge_trace_check_calls_address &&
        psx::R3000Runtime::patch_arena_base + ledge_trace_flags_offset ==
            ledge_trace_check_flags_address &&
        psx::R3000Runtime::patch_arena_base + ledge_trace_contact_offset ==
            ledge_trace_check_contact_address &&
        psx::R3000Runtime::patch_arena_base + ledge_trace_latch_offset ==
            ledge_trace_letgo_latch_address &&
        psx::R3000Runtime::patch_arena_base + ledge_trace_ticket_offset ==
            ledge_trace_letgo_ticket_address &&
        psx::R3000Runtime::patch_arena_base + ledge_trace_facing_dot_offset ==
            ledge_trace_facing_dot_address &&
        psx::R3000Runtime::patch_arena_base + ledge_trace_normal_x_offset ==
            ledge_trace_normal_x_address &&
        psx::R3000Runtime::patch_arena_base + ledge_trace_facing_z_offset ==
            ledge_trace_facing_z_address &&
        psx::R3000Runtime::patch_arena_base + ledge_trace_state_offset ==
            ledge_trace_state_address &&
        psx::R3000Runtime::patch_arena_base + ledge_trace_velocity_y_offset ==
            ledge_trace_velocity_y_address,
    "the published ledge-trace addresses must match the arena layout");

// Both counted sites have the same shape: a `jal` whose delay slot loads its
// argument. Replacing only the `jal` keeps that delay slot in place, so the
// body counts the call and then makes it with an empty delay slot. `$v1` and
// `$at` are caller-saved and dead across the call.
[[nodiscard]] constexpr std::array<std::uint32_t, 7U> ledgeTraceCountedCall(
    std::uint32_t counter_offset, std::uint32_t jal_word) noexcept {
    // `lui $at, 0x8000` leaves the arena's own base in the displacement, so
    // the slot offset alone would address the wrong page.
    const auto displacement =
        (psx::R3000Runtime::patch_arena_base & 0xFFFFU) + counter_offset;
    return {
        0x3C018000U,                  // lui   $at, 0x8000
        0x8C230000U | displacement,   // lw    $v1, counter($at)
        0x00000000U,                  // nop
        0x24630001U,                  // addiu $v1, $v1, 1
        0xAC230000U | displacement,   // sw    $v1, counter($at)
        jal_word,                     // jal   the displaced call
        0x00000000U,                  // nop
    };
}

} // namespace

std::span<const RetailTrampoline* const> ledgeTracePatches() noexcept {
    static const std::array<const RetailTrampoline*, 3U> patches{
        &ledgeTraceCheckVerdict(),
        &ledgeTraceLetGoFromLatch(),
        &ledgeTraceLetGoFromTicket(),
    };
    return patches;
}

std::span<const RetailTrampoline* const> ledgeTraceInputPatches() noexcept {
    static const std::array<const RetailTrampoline*, 2U> patches{
        &ledgeTraceHumanoidState(),
        &ledgeTraceFacingInputs(),
    };
    return patches;
}

std::optional<LedgeTraceSample> readLedgeTrace(
    const psx::R3000Runtime& runtime) noexcept {
    LedgeTraceSample sample;
    if (!runtime.read32(ledge_trace_check_calls_address, sample.check_calls) ||
        !runtime.read32(
            ledge_trace_check_flags_address, sample.check_flags) ||
        !runtime.read32(
            ledge_trace_letgo_latch_address, sample.letgo_from_latch) ||
        !runtime.read32(
            ledge_trace_letgo_ticket_address, sample.letgo_from_ticket)) {
        return std::nullopt;
    }
    const auto signedWord = [&](std::uint32_t address, std::int32_t& out) {
        std::uint32_t word = 0U;
        if (!runtime.read32(address, word)) {
            return false;
        }
        out = static_cast<std::int32_t>(word);
        return true;
    };
    if (!signedWord(
            ledge_trace_check_contact_address, sample.check_contact_y) ||
        !signedWord(ledge_trace_facing_dot_address, sample.facing_dot) ||
        !signedWord(ledge_trace_normal_x_address, sample.normal_x) ||
        !signedWord(ledge_trace_normal_y_address, sample.normal_y) ||
        !signedWord(ledge_trace_normal_z_address, sample.normal_z) ||
        !signedWord(ledge_trace_facing_x_address, sample.facing_x) ||
        !signedWord(ledge_trace_facing_z_address, sample.facing_z) ||
        !signedWord(
            ledge_trace_velocity_y_address, sample.humanoid_velocity_y) ||
        !runtime.read32(
            ledge_trace_state_address, sample.humanoid_state)) {
        return std::nullopt;
    }
    return sample;
}

std::string describeLedgeCheckFlags(std::uint32_t flags) {
    if (flags == 0U) {
        return "none";
    }
    constexpr std::pair<LedgeCheckFlag, std::string_view> names[]{
        {LedgeCheckFlag::box_wide_enough, "BOX"},
        {LedgeCheckFlag::normal_faces_humanoid, "FACING"},
        {LedgeCheckFlag::ledge_within_hand_band, "HANDBAND"},
        {LedgeCheckFlag::clearance_below, "CLEARANCE"},
        {LedgeCheckFlag::accepted, "ACCEPTED"},
    };
    std::string text;
    for (const auto& [flag, name] : names) {
        if ((flags & static_cast<std::uint32_t>(flag)) == 0U) {
            continue;
        }
        if (!text.empty()) {
            text.push_back('|');
        }
        text.append(name);
    }
    return text;
}

const RetailTrampoline& ledgeTraceCheckVerdict() noexcept {
    // Entered at the instruction that packs the result, where every condition
    // is still a live 0-or-1 register:
    //
    //   8007BFF4  beq  $s2, $zero, done    ; box wide enough
    //   8007BFFC  beq  $s1, $zero, done    ; normal faces the humanoid
    //   8007C004  beq  $a1, $zero, done    ; ledge within the hand band
    //   8007C00C  sltu $a0, $a0, $v0       ; clearance below
    //   8007C010  move $v0, $a0            ; <- site
    //
    // The state-rejected early-out jumps straight to the epilogue and never
    // reaches here, so a call that returns without a sample is itself a
    // signal. `$v1` is dead — the clearance constant was its last use — and
    // the site's delay slot reloads `$ra`, which is safe because the body
    // returns with a jump rather than through `$ra`.
    static constexpr std::uint32_t body[]{
        0x000218C0U, // sll   $v1, $v0, 3     ; clearance   -> bit 3
        0x00051080U, // sll   $v0, $a1, 2     ; hand band   -> bit 2
        0x00621825U, // or    $v1, $v1, $v0
        0x00111040U, // sll   $v0, $s1, 1     ; facing      -> bit 1
        0x00621825U, // or    $v1, $v1, $v0
        0x00721825U, // or    $v1, $v1, $s2   ; box         -> bit 0
        0x00041100U, // sll   $v0, $a0, 4     ; the verdict -> bit 4
        0x00621825U, // or    $v1, $v1, $v0
        0x3C018000U, // lui   $at, 0x8000
        0xAC233904U, // sw    $v1, 0x3904($at)
        0xAC263908U, // sw    $a2, 0x3908($at) ; the contact Y
        0x8C233900U, // lw    $v1, 0x3900($at)
        0x00000000U, // nop
        0x24630001U, // addiu $v1, $v1, 1
        0xAC233900U, // sw    $v1, 0x3900($at)
        0x00801021U, // move  $v0, $a0        ; the displaced instruction
    };
    static const RetailTrampoline patch{
        "ledge_check_verdict",
        0x8007C010U,
        0x00801021U, // move $v0, $a0
        psx::R3000Runtime::patch_arena_base + 0x940U,
        0x8007C014U,
        body};
    return patch;
}

const RetailTrampoline& ledgeTraceHumanoidState() noexcept {
    // `Obstacle::LedgeCheck`'s prologue, where `$a3` is still the humanoid:
    //
    //   8007BE90  sw   $s4, 0x50($sp)   ; <- site
    //   8007BE94  sw   $s0, 0x40($sp)   ; a store, safe in the delay slot
    //   8007BE98  move $s0, $a3
    //
    // The action state and the vertical velocity are what the obstacle's own
    // ticket branches read once `LedgeCheck` has declined: the "standing on
    // top" branch issues a ticket only for a contact normal pointing up and a
    // non-positive vertical velocity. Sampling them here means a rejected
    // ledge and the reason its fallback also declined arrive together.
    // Scratch is `$v0` and `$v1`, which the prologue itself overwrites before
    // reading — `li $v0, 0x20` at 0x8007BE9C and `lw $v1, 0x164($s0)` at
    // 0x8007BEAC. A caller-saved `$t` register would be free by the ABI and
    // is not free in practice: stock `LedgeCheck` never writes `$t3` at all,
    // so anything upstream that happens to hold a value there survives a stock
    // call and does not survive a patched one.
    static constexpr std::uint32_t body[]{
        0x3C018000U, // lui $at, 0x8000
        0x8CE20164U, // lw  $v0, 0x164($a3) ; action state
        0x8CE30068U, // lw  $v1, 0x68($a3)  ; velocity.y
        0xAC22392CU, // sw  $v0, 0x392c($at)
        0xAC233930U, // sw  $v1, 0x3930($at)
        0xAFB40050U, // sw  $s4, 0x50($sp)  ; the displaced instruction
    };
    static const RetailTrampoline patch{
        "ledge_humanoid_state",
        0x8007BE90U,
        0xAFB40050U, // sw $s4, 0x50($sp)
        psx::R3000Runtime::patch_arena_base + 0xB00U,
        0x8007BE98U,
        body};
    return patch;
}

const RetailTrampoline& ledgeTraceFacingInputs() noexcept {
    // `Obstacle::LedgeCheck` computes its facing test as
    // `dot(contact normal, humanoid facing) < 0`:
    //
    //   8007BF3C  move  $a0, $s1        ; the contact normal
    //   8007BF74  jal   0x800934E4      ; the dot product
    //   8007BF78  _addiu $a1, $sp, 0x10 ; sin/cos of the humanoid's yaw
    //   8007BF7C  addiu $a1, $zero, 1   ; <- site
    //   8007BF80  lw    $s0, 0x60($s3)  ; safe in the injected jump's delay
    //   8007BF84  srl   $s1, $v0, 0x1f  ; the sign becomes the FACING flag
    //
    // Sampling here, before `$s1` stops being the normal, publishes both
    // operands and the result. That separates a wrong heading from a normal
    // that is no longer pointing out of the face the humanoid is hanging on —
    // a normal that has rolled to +Y means the game has decided the humanoid
    // is standing on top rather than hanging beside.
    //
    // `$t2` and `$t3` are dead from here to the end of the function, and
    // `$a1` is restored by the displaced instruction.
    // Scratch is `$v0` once its value has been published, and `$v1`, which
    // `lw $v1, 0x18($s0)` at 0x8007BFA0 overwrites before reading. Same reason
    // as the prologue sample: a `$t` register is free by the ABI and not free
    // in practice.
    static constexpr std::uint32_t body[]{
        0x3C018000U, // lui   $at, 0x8000
        0xAC223914U, // sw    $v0, 0x3914($at) ; the dot
        0x8E220000U, // lw    $v0, 0x0($s1)    ; normal.x
        0x8E230004U, // lw    $v1, 0x4($s1)    ; normal.y
        0xAC223918U, // sw    $v0, 0x3918($at)
        0xAC23391CU, // sw    $v1, 0x391c($at)
        0x8E220008U, // lw    $v0, 0x8($s1)    ; normal.z
        0x8FA30010U, // lw    $v1, 0x10($sp)   ; facing.x
        0xAC223920U, // sw    $v0, 0x3920($at)
        0xAC233924U, // sw    $v1, 0x3924($at)
        0x8FA20018U, // lw    $v0, 0x18($sp)   ; facing.z
        0x00000000U, // nop
        0xAC223928U, // sw    $v0, 0x3928($at)
        0x24050001U, // addiu $a1, $zero, 1    ; the displaced instruction
    };
    static const RetailTrampoline patch{
        "ledge_facing_inputs",
        0x8007BF7CU,
        0x24050001U, // addiu $a1, $zero, 1
        psx::R3000Runtime::patch_arena_base + 0xAC0U,
        0x8007BF84U,
        body};
    return patch;
}

const RetailTrampoline& ledgeTraceLetGoFromLatch() noexcept {
    static constexpr auto body =
        ledgeTraceCountedCall(ledge_trace_latch_offset, 0x0C01B10BU);
    static const RetailTrampoline patch{
        "ledge_letgo_from_latch",
        0x80033788U,
        0x0C01B10BU, // jal 0x8006c42c
        psx::R3000Runtime::patch_arena_base + 0x9C0U,
        0x80033790U,
        body};
    return patch;
}

const RetailTrampoline& ledgeTraceLetGoFromTicket() noexcept {
    // The call immediately before `LetGoOfLedge`, on the same drop-only path:
    //
    //   8007C648  jal   0x80061CC4
    //   8007C64C  _move $a1, $v0
    //   8007C650  jal   0x8006C42C
    //
    // Counting here rather than at the `LetGoOfLedge` call itself keeps that
    // site free, and means the counter reports ticket losses the collision
    // manager detected — including any the retiming then suppresses, which is
    // what makes the suppression visible.
    static constexpr auto body =
        ledgeTraceCountedCall(ledge_trace_ticket_offset, 0x0C018731U);
    static const RetailTrampoline patch{
        "ledge_letgo_from_ticket",
        0x8007C648U,
        0x0C018731U, // jal 0x80061cc4
        psx::R3000Runtime::patch_arena_base + 0xA00U,
        0x8007C650U,
        body};
    return patch;
}

bool applyRetailTrampoline(
    psx::R3000Runtime& runtime, const RetailTrampoline& patch) noexcept {
    std::uint32_t site_word = 0U;
    if (!runtime.read32(patch.site, site_word) ||
        site_word != patch.original_word) {
        return false;
    }
    if (!installTrampolineBody(runtime, patch)) {
        return false;
    }
    return runtime.write32(patch.site, encodeJump(patch.address));
}

bool revertRetailTrampoline(
    psx::R3000Runtime& runtime, const RetailTrampoline& patch) noexcept {
    std::uint32_t site_word = 0U;
    if (!runtime.read32(patch.site, site_word) ||
        site_word != encodeJump(patch.address)) {
        return false;
    }
    if (!runtime.write32(patch.site, patch.original_word)) {
        return false;
    }
    const auto word_count = patch.body.size() + 2U;
    for (std::size_t index = 0U; index < word_count; ++index) {
        if (!runtime.write32(
                patch.address +
                    static_cast<std::uint32_t>(index * sizeof(std::uint32_t)),
                0U)) {
            return false;
        }
    }
    return true;
}

} // namespace stuntmaster::game
