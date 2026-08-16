#include "stuntmaster/game/mouse_control.hpp"

#include "stuntmaster/psx/r3000_runtime.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <limits>
#include <numbers>

namespace stuntmaster::game {
namespace {

constexpr std::uint32_t game_manager_address = 0x800DD668U;
constexpr std::uint32_t game_state_handler_offset = 0x1CU;
constexpr std::uint32_t gameplay_state_handler = 0x80029C6CU;
constexpr std::uint32_t input_manager_address = 0x800DD69CU;
constexpr std::uint32_t input_map_offset = 0x2E0U;
constexpr std::uint32_t player_address = 0x800DD6B4U;
constexpr std::uint32_t player_orientation_y_offset = 0x2CU;
constexpr std::uint32_t player_face_angle_offset = 0x114U;
constexpr std::uint32_t player_action_state_offset = 0x164U;
constexpr std::uint32_t player_behaviour_offset = 0x1B8U;
constexpr std::uint32_t behaviour_handler_offset = 0xE0U;
constexpr std::uint32_t player_user_control_handler = 0x80074F5CU;
constexpr std::uint32_t camera_address = 0x800DD734U;
constexpr std::uint32_t camera_orientation_y_offset = 0x2CU;
constexpr double angle_turn = 65'536.0;
// A target may be anywhere on the shortest arc, including directly behind
// Jackie, but may never bank more than one turn decision ahead of the body.
// Rate and acceleration limits govern how quickly that target is reached.
constexpr double maximum_target_lead = angle_turn / 2.0;

constexpr std::uint32_t mouse_mailbox_yaw = 0x800DFA34U;
constexpr std::uint32_t mouse_mailbox_mode = 0x800DFA38U;
constexpr std::uint32_t extension_last_mode = 0x800057F0U;
constexpr std::uint32_t extension_animation_loop = 0x800057F4U;

enum Register : std::uint32_t {
    zero = 0U, v0 = 2U, v1 = 3U, a0 = 4U, a1 = 5U, a2 = 6U,
    a3 = 7U, t0 = 8U, t1 = 9U, t2 = 10U, t3 = 11U, t4 = 12U,
    t5 = 13U, t6 = 14U, s0 = 16U, s1 = 17U, s4 = 20U, gp = 28U,
    sp = 29U, ra = 31U,
};

constexpr std::uint32_t encodeI(
    std::uint32_t opcode, Register rs, Register rt,
    std::int32_t immediate) noexcept {
    return opcode << 26U | static_cast<std::uint32_t>(rs) << 21U |
        static_cast<std::uint32_t>(rt) << 16U |
        (static_cast<std::uint32_t>(immediate) & 0xFFFFU);
}
constexpr std::uint32_t encodeR(
    Register rs, Register rt, Register rd, std::uint32_t shift,
    std::uint32_t function) noexcept {
    return static_cast<std::uint32_t>(rs) << 21U |
        static_cast<std::uint32_t>(rt) << 16U |
        static_cast<std::uint32_t>(rd) << 11U | shift << 6U | function;
}
constexpr std::uint32_t encodeJump(std::uint32_t target) noexcept {
    return 0x08000000U | ((target >> 2U) & 0x03FFFFFFU);
}
constexpr std::uint32_t encodeJal(std::uint32_t target) noexcept {
    return 0x0C000000U | ((target >> 2U) & 0x03FFFFFFU);
}
constexpr std::uint32_t encodeBranch(
    std::uint32_t opcode, Register rs, Register rt,
    std::uint32_t address, std::uint32_t target) noexcept {
    const auto displacement = static_cast<std::int32_t>(target) -
        static_cast<std::int32_t>(address + 4U);
    return encodeI(opcode, rs, rt, displacement / 4);
}
constexpr std::uint32_t lui(Register rt, std::uint16_t value) noexcept {
    return encodeI(0x0FU, zero, rt, value);
}
constexpr std::uint32_t lw(Register rt, std::int32_t offset, Register base) noexcept {
    return encodeI(0x23U, base, rt, offset);
}
constexpr std::uint32_t lbu(Register rt, std::int32_t offset, Register base) noexcept {
    return encodeI(0x24U, base, rt, offset);
}
constexpr std::uint32_t sw(Register rt, std::int32_t offset, Register base) noexcept {
    return encodeI(0x2BU, base, rt, offset);
}
constexpr std::uint32_t sh(Register rt, std::int32_t offset, Register base) noexcept {
    return encodeI(0x29U, base, rt, offset);
}
constexpr std::uint32_t addiu(Register rt, Register rs, std::int32_t value) noexcept {
    return encodeI(0x09U, rs, rt, value);
}
constexpr std::uint32_t ori(Register rt, Register rs, std::uint16_t value) noexcept {
    return encodeI(0x0DU, rs, rt, value);
}
constexpr std::uint32_t andi(Register rt, Register rs, std::uint16_t value) noexcept {
    return encodeI(0x0CU, rs, rt, value);
}
constexpr std::uint32_t sltiu(Register rt, Register rs, std::uint16_t value) noexcept {
    return encodeI(0x0BU, rs, rt, value);
}
constexpr std::uint32_t addu(Register rd, Register rs, Register rt) noexcept {
    return encodeR(rs, rt, rd, 0U, 0x21U);
}
constexpr std::uint32_t subu(Register rd, Register rs, Register rt) noexcept {
    return encodeR(rs, rt, rd, 0U, 0x23U);
}
constexpr std::uint32_t srlv(Register rd, Register rt, Register rs) noexcept {
    return encodeR(rs, rt, rd, 0U, 0x06U);
}
constexpr std::uint32_t sltu(Register rd, Register rs, Register rt) noexcept {
    return encodeR(rs, rt, rd, 0U, 0x2BU);
}
constexpr std::uint32_t jalr(Register rs) noexcept {
    return encodeR(rs, zero, ra, 0U, 0x09U);
}

constexpr std::uint32_t ownership_site = 0x800303D8U;
constexpr std::uint32_t ownership_arena = 0x80004800U;
constexpr std::uint32_t stand_move_site = 0x80031534U;
constexpr std::uint32_t stand_move_arena = 0x80004A00U;
constexpr std::uint32_t stand_face_site = 0x800319A8U;
constexpr std::uint32_t stand_face_arena = 0x80004B00U;
constexpr std::uint32_t run_stop_site = 0x80032718U;
constexpr std::uint32_t run_stop_arena = 0x80004C00U;
constexpr std::uint32_t run_face_site = 0x8003291CU;
constexpr std::uint32_t run_face_arena = 0x80004D00U;
constexpr std::uint32_t input_yaw_site = 0x80075174U;
constexpr std::uint32_t input_yaw_arena = 0x80005000U;
constexpr std::uint32_t run_jump_force_site = 0x800309B0U;
constexpr std::uint32_t run_jump_force_arena = 0x80004900U;
constexpr std::uint32_t flip_face_first_site = 0x80031B40U;
constexpr std::uint32_t flip_face_first_arena = 0x80004E40U;
constexpr std::uint32_t flip_face_second_site = 0x80031BE4U;
constexpr std::uint32_t flip_face_second_arena = 0x80004EA0U;
constexpr std::uint32_t jump_face_first_site = 0x80032060U;
constexpr std::uint32_t jump_face_first_arena = 0x80004F00U;
constexpr std::uint32_t jump_face_second_site = 0x800321ACU;
constexpr std::uint32_t jump_face_second_arena = 0x80004F60U;
constexpr std::uint32_t fall_face_site = 0x800323D8U;
constexpr std::uint32_t fall_face_arena = 0x80005100U;

// Stand, Run, player running/standing jumps, Fall/HardFall, and launcher
// Flip variants retain an independent visible/combat heading. All are below
// state 31, allowing the guest bodies to use one compact bit mask.
constexpr std::uint32_t free_facing_state_mask = 0x00036542U;
[[nodiscard]] constexpr bool freeFacingState(
    std::uint32_t action_state) noexcept {
    return action_state < 31U &&
        (free_facing_state_mask & (1U << action_state)) != 0U;
}

constexpr std::array<std::uint32_t, 9U> ownership_window{
    0xAFB40038U, 0x00A0A021U, 0xAFB30034U, 0x00C09821U, 0xAFBF0040U,
    0xAFB5003CU, 0xAFB20030U, 0xAFB00028U, 0x8E220058U};
constexpr std::array<std::uint32_t, 9U> stand_move_window{
    0x00822023U, 0x8E03002CU, 0x8E020114U, 0x00000000U, 0x14430024U,
    0x00000000U, 0x8E020268U, 0x00000000U, 0x00401821U};
constexpr std::array<std::uint32_t, 9U> stand_face_window{
    0x00003021U, 0x0800C67DU, 0x00000000U, 0x8F850D6CU, 0x0C0193ACU,
    0x24060001U, 0x8E040050U, 0x00000000U, 0x8C820020U};
constexpr std::array<std::uint32_t, 9U> run_stop_window{
    0x24060001U, 0xAFA00010U, 0x8C420014U, 0x00000000U, 0x0040F809U,
    0x00003821U, 0x0800CA8DU, 0x00000000U, 0x8E020058U};
constexpr std::array<std::uint32_t, 9U> run_face_window{
    0x00003021U, 0x0800CA8DU, 0x00000000U, 0x8E050114U, 0x0C0193ACU,
    0x24060001U, 0x8E020058U, 0x00000000U, 0x00021302U};
constexpr std::array<std::uint32_t, 9U> input_yaw_window{
    0x00002821U, 0x8E620018U, 0x00000000U, 0x8C480028U, 0x8C49002CU,
    0x8C4A0030U, 0xAFA80018U, 0xAFA9001CU, 0xAFAA0020U};
constexpr std::array<std::uint32_t, 9U> run_jump_force_window{
    0x00C03821U, 0x8E2202C8U, 0x02202021U, 0x8C450000U, 0x0C0186D1U,
    0x26260028U, 0x8E2302B8U, 0x240205DCU, 0xA6220156U};
constexpr std::array<std::uint32_t, 9U> flip_face_first_window{
    0xAFA20014U, 0x8E250114U, 0x96300156U, 0x24021388U, 0x0C0193ACU,
    0xA6220156U, 0x02202021U, 0x8E2500D4U, 0x27A60010U};
constexpr std::array<std::uint32_t, 9U> flip_face_second_window{
    0x02202021U, 0x02202021U, 0x8E250114U, 0x24060001U, 0x0C0193ACU,
    0x00000000U, 0x02202021U, 0x02002821U, 0x27A60020U};
constexpr std::array<std::uint32_t, 9U> jump_face_first_window{
    0x00000000U, 0x1040009BU, 0x02002021U, 0x8E050114U, 0x0C0193ACU,
    0x24060001U, 0x860202C0U, 0x00000000U, 0x1040000FU};
constexpr std::array<std::uint32_t, 9U> jump_face_second_window{
    0xAFA80018U, 0xAFA9001CU, 0xAFAA0020U, 0x8E050114U, 0x0C0193ACU,
    0x24060001U, 0xAFA00020U, 0xAFA00018U, 0x8E020114U};
constexpr std::array<std::uint32_t, 9U> fall_face_window{
    0xAFA70010U, 0xAFA80014U, 0xAFA90018U, 0x8E250114U, 0x0C0193ACU,
    0x24060001U, 0x3C10800DU, 0x02202021U, 0x8E056550U};

constexpr std::array<std::uint32_t, 26U> run_jump_force_body{
    lui(t0, 0x800EU), lbu(t0, static_cast<std::int16_t>(mouse_mailbox_mode), t0), 0U,
    addiu(t0, t0, -1),
    encodeBranch(0x05U, t0, zero, run_jump_force_arena + 16U,
                 run_jump_force_arena + 96U), 0U,
    addiu(t0, s4, -6),
    encodeBranch(0x05U, t0, zero, run_jump_force_arena + 28U,
                 run_jump_force_arena + 96U), 0U,
    addiu(sp, sp, -16), sw(ra, 12, sp), lw(t0, 0x28, s1),
    lw(t1, 0x114, s1), lw(t2, 0x30, s1), sw(t0, 0, sp), sw(t1, 4, sp),
    sw(t2, 8, sp), addu(a2, sp, zero), encodeJal(0x80061B44U), 0U,
    lw(ra, 12, sp), addiu(sp, sp, 16),
    encodeJump(run_jump_force_arena + 104U), 0U,
    encodeJal(0x80061B44U), 0U,
};

constexpr std::array<std::uint32_t, 19U> makeFaceLeaseBody(
    std::uint32_t arena, Register player, std::uint32_t state_mask) noexcept {
    return {
        lui(t0, 0x800EU),
        lbu(t0, static_cast<std::int16_t>(mouse_mailbox_mode), t0), 0U,
        addiu(t0, t0, -1),
        encodeBranch(0x05U, t0, zero, arena + 16U, arena + 68U), 0U,
        lw(t0, 0x164, player), 0U, sltiu(t1, t0, 31U),
        encodeBranch(0x04U, t1, zero, arena + 36U, arena + 68U), 0U,
        lui(t1, static_cast<std::uint16_t>(state_mask >> 16U)),
        ori(t1, t1, static_cast<std::uint16_t>(state_mask)),
        srlv(t1, t1, t0), andi(t1, t1, 1U),
        encodeBranch(0x05U, t1, zero, arena + 60U, arena + 76U), 0U,
        encodeJal(0x80064EB0U), 0U,
    };
}

constexpr auto flip_face_first_body = makeFaceLeaseBody(
    flip_face_first_arena, s1, (1U << 16U) | (1U << 17U));
constexpr auto flip_face_second_body = makeFaceLeaseBody(
    flip_face_second_arena, s1, (1U << 16U) | (1U << 17U));
constexpr auto jump_face_first_body = makeFaceLeaseBody(
    jump_face_first_arena, s0, (1U << 6U) | (1U << 8U));
constexpr auto jump_face_second_body = makeFaceLeaseBody(
    jump_face_second_arena, s0, (1U << 6U) | (1U << 8U));
constexpr auto fall_face_body = makeFaceLeaseBody(
    fall_face_arena, s1, (1U << 13U) | (1U << 14U));

// The first prologue word is not hook-safe: its following store would execute
// before the stack allocation in a jump delay slot. At 0x800303D8 the stack is
// established, s1/s4 carry Player/newState, and the delay-slot store is safe.
constexpr std::array<std::uint32_t, 36U> ownership_body{
    lui(t0, 0x800EU), lbu(t0, static_cast<std::int16_t>(mouse_mailbox_mode), t0), 0U,
    addiu(t1, t0, -1),
    encodeBranch(0x05U, t1, zero, ownership_arena + 16U, ownership_arena + 140U), 0U,
    sltiu(t1, s4, 31U),
    encodeBranch(0x04U, t1, zero, ownership_arena + 28U, ownership_arena + 60U), 0U,
    lui(t2, 0x4030U), ori(t2, t2, 0x1000U), srlv(t2, t2, s4), andi(t2, t2, 1U),
    encodeBranch(0x05U, t2, zero, ownership_arena + 52U, ownership_arena + 108U), 0U,
    addiu(t1, s4, -19),
    encodeBranch(0x04U, t1, zero, ownership_arena + 64U, ownership_arena + 128U), 0U,
    addiu(t1, s4, -22),
    encodeBranch(0x04U, t1, zero, ownership_arena + 76U, ownership_arena + 128U), 0U,
    addiu(t1, s4, -32), sltiu(t1, t1, 14U),
    encodeBranch(0x05U, t1, zero, ownership_arena + 92U, ownership_arena + 128U), 0U,
    encodeJump(ownership_arena + 140U), 0U,
    lw(t1, 0x114, s1), 0U, sw(t1, 0x2C, s1), encodeJump(ownership_arena + 140U), 0U,
    lw(t1, 0x2C, s1), 0U, sw(t1, 0x114, s1), sw(ra, 0x40, sp),
};

constexpr std::array<std::uint32_t, 12U> stand_move_body{
    lui(t0, 0x800EU), lbu(t0, static_cast<std::int16_t>(mouse_mailbox_mode), t0), 0U,
    addiu(t0, t0, -1),
    encodeBranch(0x04U, t0, zero, stand_move_arena + 16U, stand_move_arena + 40U), 0U,
    encodeBranch(0x05U, v0, v1, stand_move_arena + 24U, 0x800315C8U), 0U,
    encodeJump(0x8003153CU), 0U, sw(zero, 0x1BC, gp), sh(zero, 0xD70, gp),
};

constexpr std::array<std::uint32_t, 13U> stand_face_body{
    lui(t0, 0x800EU), lbu(t0, static_cast<std::int16_t>(mouse_mailbox_mode), t0), 0U,
    addiu(t0, t0, -1),
    encodeBranch(0x05U, t0, zero, stand_face_arena + 16U, stand_face_arena + 44U), 0U,
    lw(t0, 0x164, a0), 0U, addiu(t0, t0, -1),
    encodeBranch(0x04U, t0, zero, stand_face_arena + 36U, stand_face_arena + 52U), 0U,
    encodeJal(0x80064EB0U), 0U,
};

constexpr std::array<std::uint32_t, 10U> run_stop_body{
    lui(t0, 0x800EU), lbu(t0, static_cast<std::int16_t>(mouse_mailbox_mode), t0), 0U,
    addiu(t0, t0, -1),
    encodeBranch(0x05U, t0, zero, run_stop_arena + 16U, run_stop_arena + 32U), 0U,
    addiu(a1, zero, 22), addu(a2, zero, zero), jalr(v0), 0U,
};

constexpr std::array<std::uint32_t, 72U> run_face_body{
    lui(t0, 0x800EU), lbu(t0, static_cast<std::int16_t>(mouse_mailbox_mode), t0), 0U,
    addiu(t0, t0, -1),
    encodeBranch(0x05U, t0, zero, run_face_arena + 16U, run_face_arena + 280U), 0U,
    lw(t0, 0x164, s0), 0U, addiu(t0, t0, -10),
    encodeBranch(0x05U, t0, zero, run_face_arena + 36U, run_face_arena + 280U), 0U,
    lw(t0, 0x2C, s0), lw(t1, 0x114, s0), 0U, subu(t0, t0, t1),
    andi(t0, t0, 0xFFFFU), addiu(a1, zero, 51), addu(t1, zero, zero),
    sltiu(t2, t0, 8193U),
    encodeBranch(0x05U, t2, zero, run_face_arena + 76U, run_face_arena + 196U), 0U,
    sltiu(t2, t0, 24576U),
    encodeBranch(0x05U, t2, zero, run_face_arena + 88U, run_face_arena + 160U), 0U,
    addiu(t2, t0, -24576),
    encodeBranch(0x04U, t2, zero, run_face_arena + 100U, run_face_arena + 196U), 0U,
    ori(t2, zero, 0xA000U), sltu(t2, t0, t2),
    encodeBranch(0x05U, t2, zero, run_face_arena + 116U, run_face_arena + 176U), 0U,
    addiu(t2, t0, -40960),
    encodeBranch(0x04U, t2, zero, run_face_arena + 128U, run_face_arena + 196U), 0U,
    ori(t2, zero, 0xE000U), sltu(t2, t0, t2),
    encodeBranch(0x05U, t2, zero, run_face_arena + 144U, run_face_arena + 188U), 0U,
    encodeJump(run_face_arena + 196U), 0U,
    addiu(a1, zero, 52), addiu(t1, zero, 1), encodeJump(run_face_arena + 196U), 0U,
    addiu(t1, zero, 1), encodeJump(run_face_arena + 196U), 0U,
    addiu(a1, zero, 52), 0U,
    lui(t3, 0x8000U), sw(t1, static_cast<std::int16_t>(extension_animation_loop), t3),
    lw(a0, 0x50, s0), 0U, lw(v0, 0x08, a0), addu(a2, zero, zero),
    sw(zero, 0x10, sp), lw(v0, 0x14, v0), 0U, jalr(v0), addu(a3, zero, zero),
    lw(a0, 0x50, s0), 0U, lw(a0, 0x20, a0), lui(t3, 0x8000U),
    lw(a1, static_cast<std::int16_t>(extension_animation_loop), t3),
    addu(a2, zero, zero), encodeJal(0x80070C20U), 0U,
    encodeJump(run_face_arena + 288U), 0U, encodeJal(0x80064EB0U), 0U,
};

constexpr std::array<std::uint32_t, 59U> input_yaw_body{
    lui(t3, 0x800EU), lbu(t3, static_cast<std::int16_t>(mouse_mailbox_mode), t3), 0U,
    addiu(t4, t3, -1),
    encodeBranch(0x05U, t4, zero, input_yaw_arena + 16U, input_yaw_arena + 140U), 0U,
    lw(t4, 0x164, v0), 0U, sltiu(t5, t4, 31U),
    encodeBranch(0x04U, t5, zero, input_yaw_arena + 36U, input_yaw_arena + 116U), 0U,
    lui(t5, static_cast<std::uint16_t>(free_facing_state_mask >> 16U)),
    ori(t5, t5, static_cast<std::uint16_t>(free_facing_state_mask)),
    srlv(t5, t5, t4), andi(t5, t5, 1U),
    encodeBranch(0x04U, t5, zero, input_yaw_arena + 60U, input_yaw_arena + 116U), 0U,
    lui(t5, 0x800EU), lw(t6, static_cast<std::int16_t>(mouse_mailbox_yaw), t5), 0U,
    sw(t6, 0x2C, v0), addu(t1, t6, zero), sw(zero, 0x1BC, gp), sh(zero, 0xD70, gp),
    lui(t5, 0x8000U), addiu(t6, zero, 1),
    sw(t6, static_cast<std::int16_t>(extension_last_mode), t5),
    encodeJump(input_yaw_arena + 236U), 0U,
    lui(t5, 0x8000U), addiu(t6, zero, 1),
    sw(t6, static_cast<std::int16_t>(extension_last_mode), t5), lw(t1, 0x2C, v0),
    encodeJump(input_yaw_arena + 236U), 0U,
    lui(t5, 0x8000U), lw(t6, static_cast<std::int16_t>(extension_last_mode), t5), 0U,
    sw(zero, static_cast<std::int16_t>(extension_last_mode), t5), addiu(t6, t6, -1),
    encodeBranch(0x05U, t6, zero, input_yaw_arena + 160U, input_yaw_arena + 232U), 0U,
    lw(t4, 0x164, v0), 0U, sltiu(t5, t4, 31U),
    encodeBranch(0x04U, t5, zero, input_yaw_arena + 180U, input_yaw_arena + 232U), 0U,
    lui(t5, static_cast<std::uint16_t>(free_facing_state_mask >> 16U)),
    ori(t5, t5, static_cast<std::uint16_t>(free_facing_state_mask)),
    srlv(t5, t5, t4), andi(t5, t5, 1U),
    encodeBranch(0x04U, t5, zero, input_yaw_arena + 204U, input_yaw_arena + 232U), 0U,
    lw(t1, 0x114, v0), 0U, sw(t1, 0x2C, v0),
    encodeJump(input_yaw_arena + 236U), 0U,
    lw(t1, 0x2C, v0),
};

struct MousePatchDefinition {
    RetailTrampoline trampoline;
    std::span<const std::uint32_t> window;
    std::uint32_t window_address{};
    std::size_t site_index{};
};

[[nodiscard]] std::span<const MousePatchDefinition> mousePatches() noexcept {
    static const std::array definitions{
        MousePatchDefinition{{"mouse_heading_ownership", ownership_site,
                              ownership_window[4], ownership_arena, 0x800303E0U,
                              ownership_body}, ownership_window, ownership_site - 16U, 4U},
        MousePatchDefinition{{"mouse_run_jump_force", run_jump_force_site,
                              run_jump_force_window[4], run_jump_force_arena,
                              0x800309B8U, run_jump_force_body},
                             run_jump_force_window, run_jump_force_site - 16U, 4U},
        MousePatchDefinition{{"mouse_stand_move", stand_move_site,
                              stand_move_window[4], stand_move_arena, 0x8003153CU,
                              stand_move_body}, stand_move_window, stand_move_site - 16U, 4U},
        MousePatchDefinition{{"mouse_stand_face", stand_face_site,
                              stand_face_window[4], stand_face_arena, 0x800319B0U,
                              stand_face_body}, stand_face_window, stand_face_site - 16U, 4U},
        MousePatchDefinition{{"mouse_flip_face_first", flip_face_first_site,
                              flip_face_first_window[4], flip_face_first_arena,
                              0x80031B48U, flip_face_first_body},
                             flip_face_first_window, flip_face_first_site - 16U, 4U},
        MousePatchDefinition{{"mouse_flip_face_second", flip_face_second_site,
                              flip_face_second_window[4], flip_face_second_arena,
                              0x80031BECU, flip_face_second_body},
                             flip_face_second_window, flip_face_second_site - 16U, 4U},
        MousePatchDefinition{{"mouse_jump_face_first", jump_face_first_site,
                              jump_face_first_window[4], jump_face_first_arena,
                              0x80032068U, jump_face_first_body},
                             jump_face_first_window, jump_face_first_site - 16U, 4U},
        MousePatchDefinition{{"mouse_jump_face_second", jump_face_second_site,
                              jump_face_second_window[4], jump_face_second_arena,
                              0x800321B4U, jump_face_second_body},
                             jump_face_second_window, jump_face_second_site - 16U, 4U},
        MousePatchDefinition{{"mouse_fall_face", fall_face_site,
                              fall_face_window[4], fall_face_arena,
                              0x800323E0U, fall_face_body},
                             fall_face_window, fall_face_site - 16U, 4U},
        MousePatchDefinition{{"mouse_run_stop", run_stop_site,
                              run_stop_window[4], run_stop_arena, 0x80032720U,
                              run_stop_body}, run_stop_window, run_stop_site - 16U, 4U},
        MousePatchDefinition{{"mouse_run_face", run_face_site,
                              run_face_window[4], run_face_arena, 0x80032924U,
                              run_face_body}, run_face_window, run_face_site - 16U, 4U},
        MousePatchDefinition{{"mouse_input_yaw", input_yaw_site,
                              input_yaw_window[4], input_yaw_arena, 0x8007517CU,
                              input_yaw_body}, input_yaw_window, input_yaw_site - 16U, 4U},
    };
    return definitions;
}

[[nodiscard]] bool validGuestPointer(std::uint32_t value) noexcept {
    return value >= 0x80010000U && value <= 0x801FFFFCU;
}

[[nodiscard]] bool windowMatches(
    const psx::R3000Runtime& runtime, std::uint32_t address,
    std::span<const std::uint32_t> words,
    std::size_t skip_index = std::numeric_limits<std::size_t>::max()) noexcept {
    for (std::size_t index = 0U; index < words.size(); ++index) {
        if (index == skip_index) continue;
        std::uint32_t found = 0U;
        if (!runtime.read32(address + static_cast<std::uint32_t>(index * 4U), found) ||
            found != words[index]) return false;
    }
    return true;
}

[[nodiscard]] bool trampolineInstalled(
    const psx::R3000Runtime& runtime, const RetailTrampoline& patch) noexcept {
    std::uint32_t word = 0U;
    if (!runtime.read32(patch.site, word) || word != encodeJump(patch.address)) return false;
    for (std::size_t index = 0U; index < patch.body.size(); ++index) {
        if (!runtime.read32(patch.address + static_cast<std::uint32_t>(index * 4U), word) ||
            word != patch.body[index]) return false;
    }
    return runtime.read32(patch.address + static_cast<std::uint32_t>(patch.body.size() * 4U), word) &&
        word == encodeJump(patch.return_address) &&
        runtime.read32(patch.address + static_cast<std::uint32_t>((patch.body.size() + 1U) * 4U), word) &&
        word == 0U;
}

[[nodiscard]] bool equalIgnoringCase(std::string_view left, std::string_view right) noexcept {
    return left.size() == right.size() &&
        std::equal(left.begin(), left.end(), right.begin(), [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) ==
                std::tolower(static_cast<unsigned char>(b));
        });
}

[[nodiscard]] constexpr std::uint16_t angle16(std::uint32_t value) noexcept {
    return static_cast<std::uint16_t>(value);
}
[[nodiscard]] constexpr std::int16_t signedAngleDelta(
    std::uint32_t body, std::uint32_t travel) noexcept {
    return static_cast<std::int16_t>(angle16(body - travel));
}
[[nodiscard]] double wrapAngle(double value) noexcept {
    value = std::fmod(value, angle_turn);
    return value < 0.0 ? value + angle_turn : value;
}
[[nodiscard]] double shortestAngle(double target, double current) noexcept {
    auto difference = wrapAngle(target) - wrapAngle(current);
    if (difference >= angle_turn / 2.0) difference -= angle_turn;
    else if (difference < -angle_turn / 2.0) difference += angle_turn;
    return difference;
}

} // namespace

std::optional<MouseGameplayContext> readMouseGameplayContext(
    const psx::R3000Runtime& runtime, bool photo_mode_active) noexcept {
    if (photo_mode_active) return std::nullopt;
    std::uint32_t game = 0U, state_handler = 0U, player = 0U;
    std::uint32_t behaviour = 0U, control_handler = 0U, input = 0U, camera = 0U;
    MouseGameplayContext result{};
    if (!runtime.read32(game_manager_address, game) || !validGuestPointer(game) ||
        !runtime.read32(game + game_state_handler_offset, state_handler) ||
        state_handler != gameplay_state_handler ||
        !runtime.read32(player_address, player) || !validGuestPointer(player) ||
        !runtime.read32(player + player_behaviour_offset, behaviour) ||
        !validGuestPointer(behaviour) ||
        !runtime.read32(behaviour + behaviour_handler_offset, control_handler) ||
        control_handler != player_user_control_handler ||
        !runtime.read32(player + player_orientation_y_offset, result.player_yaw) ||
        !runtime.read32(player + player_face_angle_offset, result.travel_yaw) ||
        !runtime.read32(player + player_action_state_offset, result.action_state) ||
        !runtime.read32(camera_address, camera) || !validGuestPointer(camera) ||
        !runtime.read32(camera + camera_orientation_y_offset, result.camera_yaw) ||
        !runtime.read32(input_manager_address, input) || !validGuestPointer(input)) {
        return std::nullopt;
    }
    result.player = player;
    for (std::size_t index = 0U; index < result.physical_to_logical.size(); ++index) {
        if (!runtime.read8(input + input_map_offset + static_cast<std::uint32_t>(index),
                           result.physical_to_logical[index])) return std::nullopt;
    }
    return result;
}

MouseHeadingDiagnostic mouseHeadingDiagnostic(
    const MouseGameplayContext& context) noexcept {
    MouseHeadingDiagnostic result{
        MouseHeadingSplitKind::aligned, context.action_state,
        angle16(context.player_yaw), angle16(context.travel_yaw),
        signedAngleDelta(context.player_yaw, context.travel_yaw)};
    if (result.signed_delta != 0) {
        result.kind = freeFacingState(context.action_state)
            ? MouseHeadingSplitKind::free_lease
            : MouseHeadingSplitKind::context;
    }
    return result;
}

std::uint16_t applySemanticMouseActions(
    std::uint16_t active_low_buttons,
    std::span<const bool, primary_action_count> semantic_actions,
    std::span<const std::uint8_t, primary_action_count> physical_to_logical) noexcept {
    std::array<std::uint8_t, primary_action_count> logical_to_physical{};
    std::uint16_t seen = 0U;
    for (std::size_t physical = 0U; physical < primary_action_count; ++physical) {
        const auto logical = physical_to_logical[physical];
        if (logical >= primary_action_count || (seen & (1U << logical)) != 0U)
            return active_low_buttons;
        seen |= static_cast<std::uint16_t>(1U << logical);
        logical_to_physical[logical] = static_cast<std::uint8_t>(physical);
    }
    if (seen != 0xFFU) return active_low_buttons;
    for (std::size_t logical = 0U; logical < semantic_actions.size(); ++logical) {
        if (semantic_actions[logical]) {
            const auto host_bit = logical_to_physical[logical] ^ 8U;
            active_low_buttons &= static_cast<std::uint16_t>(
                ~(std::uint16_t{1U} << host_bit));
        }
    }
    return active_low_buttons;
}

MouseYawController::MouseYawController(MouseControlConfig config) noexcept
    : mode_{config.initial_mode},
      maximum_turn_rate_{std::clamp(config.maximum_turn_rate, 30U, 1440U)},
      turn_acceleration_{std::clamp(config.turn_acceleration, 60U, 10'000U)} {}

bool MouseYawController::freeFacingLease(std::uint32_t action_state) noexcept {
    return freeFacingState(action_state);
}

void MouseYawController::setMode(MouseMovementMode mode) noexcept {
    if (mode_ != mode) {
        mode_ = mode;
        abandon();
    }
}

MouseMovementMode MouseYawController::cycleMode() noexcept {
    setMode(mode_ == MouseMovementMode::off
                ? MouseMovementMode::camera_relative
                : MouseMovementMode::off);
    return mode_;
}

void MouseYawController::abandon() noexcept {
    synchronized_ = false;
    lease_active_ = false;
    have_target_ = false;
    gesture_active_ = false;
    angular_velocity_ = 0.0;
}

void MouseYawController::update(
    const std::optional<MouseGameplayContext>& context,
    std::int32_t mouse_x,
    std::int32_t mouse_y,
    std::uint32_t vblank_rate) noexcept {
    if (mode_ == MouseMovementMode::off || !context) {
        abandon();
        return;
    }
    if (!synchronized_) {
        yaw_ = context->player_yaw;
        continuous_yaw_ = static_cast<double>(angle16(yaw_));
        target_yaw_ = continuous_yaw_;
        synchronized_ = true;
    }
    if (!freeFacingLease(context->action_state)) {
        lease_active_ = false;
        have_target_ = false;
        gesture_active_ = false;
        angular_velocity_ = 0.0;
        return;
    }
    if (!lease_active_) {
        yaw_ = context->player_yaw;
        continuous_yaw_ = static_cast<double>(angle16(yaw_));
        target_yaw_ = continuous_yaw_;
        angular_velocity_ = 0.0;
        have_target_ = false;
        gesture_active_ = false;
        lease_active_ = true;
        return;
    }
    if (mouse_x != 0 || mouse_y != 0) {
        const auto radians = std::atan2(
            static_cast<double>(mouse_x), -static_cast<double>(mouse_y));
        const auto gesture_yaw = wrapAngle(
            static_cast<double>(angle16(context->camera_yaw)) +
            radians * angle_turn / (2.0 * std::numbers::pi));
        if (gesture_active_) {
            // Unwrap against the previous sample, not against the lagging
            // body. A fast circular gesture can therefore cross zero or lap
            // the turn controller without reversing its requested direction.
            target_yaw_ += shortestAngle(gesture_yaw, last_gesture_yaw_);
        } else {
            target_yaw_ = continuous_yaw_ +
                shortestAngle(gesture_yaw, continuous_yaw_);
        }
        last_gesture_yaw_ = gesture_yaw;
        gesture_active_ = true;
        // Do not queue arbitrarily many rotations when a tight mouse circle
        // outruns the physical turn limit. Retain at most a half-turn of
        // lead. A half-turn still permits every absolute direction while
        // preventing a circular gesture from banking extra revolutions.
        target_yaw_ = continuous_yaw_ + std::clamp(
            target_yaw_ - continuous_yaw_,
            -maximum_target_lead, maximum_target_lead);
        have_target_ = true;
    } else {
        gesture_active_ = false;
    }

    const auto rate = std::max(vblank_rate, 1U);
    const auto dt = 1.0 / static_cast<double>(rate);
    const auto maximum_velocity =
        static_cast<double>(maximum_turn_rate_) * angle_turn / 360.0;
    const auto maximum_acceleration =
        static_cast<double>(turn_acceleration_) * angle_turn / 360.0;
    const auto error = have_target_ ? target_yaw_ - continuous_yaw_ : 0.0;
    const auto wanted_velocity =
        std::clamp(error * 10.0, -maximum_velocity, maximum_velocity);
    const auto velocity_step = maximum_acceleration * dt;
    angular_velocity_ += std::clamp(
        wanted_velocity - angular_velocity_, -velocity_step, velocity_step);
    auto yaw_step = angular_velocity_ * dt;
    if (have_target_ && std::abs(yaw_step) > std::abs(error)) {
        yaw_step = error;
        angular_velocity_ = 0.0;
    }
    continuous_yaw_ += yaw_step;
    yaw_ = static_cast<std::uint16_t>(
        static_cast<std::uint32_t>(
            std::llround(wrapAngle(continuous_yaw_))) & 0xFFFFU);
}

std::optional<MouseButton> parseMouseButton(std::string_view value) noexcept {
    if (equalIgnoringCase(value, "none")) return MouseButton::none;
    if (equalIgnoringCase(value, "left")) return MouseButton::left;
    if (equalIgnoringCase(value, "right")) return MouseButton::right;
    if (equalIgnoringCase(value, "middle")) return MouseButton::middle;
    if (equalIgnoringCase(value, "x1")) return MouseButton::x1;
    if (equalIgnoringCase(value, "x2")) return MouseButton::x2;
    return std::nullopt;
}

std::string_view mouseButtonName(MouseButton button) noexcept {
    switch (button) {
    case MouseButton::none: return "None";
    case MouseButton::left: return "Left";
    case MouseButton::right: return "Right";
    case MouseButton::middle: return "Middle";
    case MouseButton::x1: return "X1";
    case MouseButton::x2: return "X2";
    }
    return "None";
}

std::optional<MouseMovementMode> parseMouseMovementMode(
    std::string_view value) noexcept {
    if (equalIgnoringCase(value, "off")) return MouseMovementMode::off;
    if (equalIgnoringCase(value, "camera_relative"))
        return MouseMovementMode::camera_relative;
    return std::nullopt;
}

std::string_view mouseMovementModeName(MouseMovementMode mode) noexcept {
    switch (mode) {
    case MouseMovementMode::off: return "off";
    case MouseMovementMode::camera_relative: return "camera_relative";
    }
    return "off";
}

bool setMouseControlPatches(psx::R3000Runtime& runtime, bool enabled) noexcept {
    const auto definitions = mousePatches();
    std::array<bool, 12U> installed{};
    std::array<bool, 12U> stock{};
    for (std::size_t index = 0U; index < definitions.size(); ++index) {
        const auto& definition = definitions[index];
        installed[index] = trampolineInstalled(runtime, definition.trampoline) &&
            windowMatches(runtime, definition.window_address,
                          definition.window, definition.site_index);
        std::uint32_t word = 0U;
        stock[index] = runtime.read32(definition.trampoline.site, word) &&
            word == definition.trampoline.original_word &&
            windowMatches(runtime, definition.window_address, definition.window);
    }
    const auto allInstalled = std::ranges::all_of(installed, [](bool value) { return value; });
    const auto allStock = std::ranges::all_of(stock, [](bool value) { return value; });
    if (enabled) {
        if (allInstalled) return true;
        if (!allStock) return false;
        std::size_t applied = 0U;
        for (; applied < definitions.size(); ++applied) {
            if (!applyRetailTrampoline(runtime, definitions[applied].trampoline)) {
                while (applied > 0U) {
                    --applied;
                    (void)revertRetailTrampoline(runtime, definitions[applied].trampoline);
                }
                return false;
            }
        }
        if (runtime.write32(extension_last_mode, 0U) &&
            runtime.write32(extension_animation_loop, 0U)) {
            return true;
        }
        for (std::size_t index = definitions.size(); index > 0U; --index)
            (void)revertRetailTrampoline(runtime, definitions[index - 1U].trampoline);
        return false;
    }
    if (allStock) return true;
    if (!allInstalled) return false;
    for (std::size_t index = definitions.size(); index > 0U; --index) {
        if (!revertRetailTrampoline(runtime, definitions[index - 1U].trampoline)) {
            for (std::size_t restore = index; restore < definitions.size(); ++restore)
                (void)applyRetailTrampoline(runtime, definitions[restore].trampoline);
            return false;
        }
    }
    return runtime.write32(extension_last_mode, 0U) &&
        runtime.write32(extension_animation_loop, 0U);
}

} // namespace stuntmaster::game
