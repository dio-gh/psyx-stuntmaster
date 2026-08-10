#include "stuntmaster/game/retiming.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

// The C++ ports of the retiming MIPS bodies, replacing the trampolines at the
// retime sites. Each `fn` is a literal port of the MIPS body it replaces, run
// with the site's delay-slot instruction already executed. The MIPS bodies are
// the spec: the whole-register-file stock-vs-patched tests keep a port
// bit-identical.

namespace stuntmaster::game {
namespace {

// `Move__12DynamicThing`'s gravity read (`0x80062134`).
//
// Entered with `$a0` = the gravity just loaded from field `0xC4`. The site's
// delay slot has loaded `$v1 = MAX_FALL_SPEED` (still in the load delay; the
// code that clamps against it runs after the rejoin, by which time the
// pipeline has delivered it). This divides `$a0` half-to-even and then models
// the displaced `lw $v0, 0x14($sp)`, exactly as the MIPS body did: the divide
// at the single point of use covers every writer of the field by construction.
std::uint32_t gravityHook(
    psx::R3000State& state,
    RetimeState& retime,
    psx::R3000Runtime& runtime,
    std::uint32_t rejoin, const void* context) noexcept {
    const auto gravity = static_cast<std::int32_t>(state.gpr[4]);
    hostWriteRegister(
        state, 4, static_cast<std::uint32_t>(divideHalfToEven(gravity, retime.divisor)));
    std::uint32_t working_velocity = 0U;
    static_cast<void>(runtime.read32(state.gpr[29] + 0x14U, working_velocity));
    hostWriteRegister(state, 2, working_velocity);
    return rejoin;
}

// `Move__12DynamicThing`'s persistent Y-velocity store (`0x80062340`).
//
// A retimed running passenger still needs gravity's tiny downward position
// probe: without it, an exactly coplanar player whose ticket was cleared cannot
// produce an upward collision normal and reboard a moving Platform. But that
// probe must not become persistent velocity or it accumulates into the visible
// -9/-18 correction sawtooth. Store zero for the current player in `AS_Run`
// after the position step has already consumed the probe. Platform carry is in
// the separate force fields, and `AS_Fall` retains the ordinary store.
//
// The hook models the displaced `sw $v0,0x68($s1)`. Its delay slot has already
// issued `lw $v0,0x6c($s1)`; do not write `$v0` here, so that pending load is
// delivered normally after the rejoin.
std::uint32_t runningVelocityYStoreHook(
    psx::R3000State& state,
    RetimeState& retime,
    psx::R3000Runtime& runtime,
    std::uint32_t rejoin, const void* context) noexcept {
    constexpr std::uint32_t the_player_address = 0x800DD6B4U;
    constexpr std::uint32_t velocity_y_offset = 0x68U;
    constexpr std::uint32_t action_state_offset = 0x164U;
    constexpr std::uint32_t action_run = 0x0AU;

    std::uint32_t player = 0U;
    std::uint32_t action_state = 0U;
    const auto running = retime.divisor > 1U &&
        runtime.read32(the_player_address, player) &&
        player == state.gpr[17] &&
        runtime.read32(
            state.gpr[17] + action_state_offset, action_state) &&
        action_state == action_run;
    static_cast<void>(runtime.write32(
        state.gpr[17] + velocity_y_offset,
        running ? 0U : state.gpr[2]));
    return rejoin;
}

// `Move__12DynamicThing`'s shared position step (`0x800622BC`).
//
// Entered with `$a0` = step.x, `$s1` = the DynamicThing, and step.y/step.z in
// `0x24($sp)`/`0x28($sp)`. Divides all three half-to-even (storing y/z back,
// leaving x in `$a0`) and models the displaced `lw $v0, 0x7c($s1)`. The site's
// delay slot has loaded the carried-velocity decay factor into `$a1`; on a
// held update it is overridden to `1.0` in 16.16 so the two `mult` decays
// become the identity and retail's decay is spent exactly once per authored
// step, while a counted update leaves the pipeline to deliver the real factor.
//
// `$v1` is written with the divisor, matching what the MIPS body leaves in it.
std::uint32_t positionStepHook(
    psx::R3000State& state,
    RetimeState& retime,
    psx::R3000Runtime& runtime,
    std::uint32_t rejoin, const void* context) noexcept {
    const auto divisor = retime.divisor == 0U ? 1U : retime.divisor;
    const auto step_x = static_cast<std::int32_t>(state.gpr[4]);
    const auto sp = state.gpr[29];
    std::uint32_t step_y = 0U;
    std::uint32_t step_z = 0U;
    static_cast<void>(runtime.read32(sp + 0x24U, step_y));
    static_cast<void>(runtime.read32(sp + 0x28U, step_z));
    hostWriteRegister(
        state, 4, static_cast<std::uint32_t>(divideHalfToEven(step_x, divisor)));
    static_cast<void>(runtime.write32(
        sp + 0x24U,
        static_cast<std::uint32_t>(divideHalfToEven(
            static_cast<std::int32_t>(step_y), divisor))));
    static_cast<void>(runtime.write32(
        sp + 0x28U,
        static_cast<std::uint32_t>(divideHalfToEven(
            static_cast<std::int32_t>(step_z), divisor))));
    if (retime.hold()) {
        hostWriteRegister(state, 5, 0x00010000U); // $a1 = 1.0 in 16.16
    }
    hostWriteRegister(state, 3, divisor); // $v1, as the MIPS body leaves it
    std::uint32_t position = 0U;
    static_cast<void>(runtime.read32(state.gpr[17] + 0x7CU, position));
    hostWriteRegister(state, 2, position);
    return rejoin;
}

// `FaceAngleY__8Humanoid`'s turn-limit load, shared by both signed branches
// (`0x80064F10` and `0x80064F38`). Each loads the frame's limit into `$a2`
// and reads it twice, so dividing at the load covers the compare and the add
// together. Models the displaced `lh $a2, 0x156($a0)` (sign-extended),
// truncates the divide as the body did, and never collapses a real limit to
// zero. `$t2` is left as the divisor and `$t3` as the retail limit, matching
// the body's register writes. The site's delay slot is a `nop`.
std::uint32_t turnRateHook(
    psx::R3000State& state,
    RetimeState& retime,
    psx::R3000Runtime& runtime,
    std::uint32_t rejoin, const void* context) noexcept {
    const auto divisor = retime.divisor == 0U ? 1U : retime.divisor;
    std::uint16_t raw_limit = 0U;
    static_cast<void>(runtime.read16(state.gpr[4] + 0x156U, raw_limit));
    const auto limit =
        static_cast<std::int32_t>(static_cast<std::int16_t>(raw_limit));
    hostWriteRegister(state, 6, static_cast<std::uint32_t>(limit));  // $a2
    hostWriteRegister(state, 10, divisor);                           // $t2
    hostWriteRegister(state, 11, static_cast<std::uint32_t>(limit)); // $t3
    auto quotient = limit / static_cast<std::int32_t>(divisor);      // truncate
    if (quotient == 0 && limit != 0) {
        quotient = 1; // never stall a real limit
    }
    hostWriteRegister(state, 6, static_cast<std::uint32_t>(quotient));
    return rejoin;
}

// `Obstacle::CorrectThingPosition`'s swept-overlap threshold (`0x8007B7E0`).
// `$gp+0x84C` is retail's per-frame motion threshold (9 = gravity/2); it must
// scale with one update's sink or a ledge hang loses its ticket on the first
// sub-step. Models the displaced `lw $v1, 0x84c($gp)`, divides it, and gives
// back one unit — the most the position step's half-to-even rounding can cost
// — at divisors of two and above, keeping retail's own value at a divisor of
// one and never going negative.
std::uint32_t overlapGateHook(
    psx::R3000State& state,
    RetimeState& retime,
    psx::R3000Runtime& runtime,
    std::uint32_t rejoin, const void* context) noexcept {
    const auto divisor = retime.divisor == 0U ? 1U : retime.divisor;
    std::uint32_t threshold_word = 0U;
    static_cast<void>(runtime.read32(state.gpr[28] + 0x84CU, threshold_word));
    auto threshold = static_cast<std::int32_t>(threshold_word) /
        static_cast<std::int32_t>(divisor);
    if (divisor >= 2U) {
        threshold -= 1;
        if (threshold < 0) {
            threshold = 0;
        }
    }
    hostWriteRegister(state, 3, static_cast<std::uint32_t>(threshold));
    return rejoin;
}

constexpr std::array<RetimeHook, 6U> motion_hooks{{
    {"gravity_at_use",
     0x80062134U,
     0x8006213CU,
     RetimeHookKind::recompute,
     &gravityHook},
    {"dynamic_thing_position_step",
     0x800622BCU,
     0x800622C4U,
     RetimeHookKind::recompute,
     &positionStepHook},
    {"running_player_velocity_y_store",
     0x80062340U,
     0x80062348U,
     RetimeHookKind::semantic,
     &runningVelocityYStoreHook},
    {"humanoid_turn_rate_negative",
     0x80064F10U,
     0x80064F18U,
     RetimeHookKind::recompute,
     &turnRateHook},
    {"humanoid_turn_rate_positive",
     0x80064F38U,
     0x80064F40U,
     RetimeHookKind::recompute,
     &turnRateHook},
    {"obstacle_overlap_gate",
     0x8007B7E0U,
     0x8007B7E8U,
     RetimeHookKind::recompute,
     &overlapGateHook},
}};

// `Step__4Time` (`0x80044A40`) is the master game clock, verified against the
// reconstructed `GEN/TIME.c`: `Time::Step` is exactly `tick += 1` on the field
// at `+0x1C`, once per authored 30 Hz frame, and animation, timeouts, and many
// state counters read that tick. This hook is the single decider for every
// retimed subsystem: it calls `RetimeState::decide()` once per guest update and
// every other hook reads `hold()` instead of deciding for itself.
//
// The displaced `lw $v0, 0x1c($a0)` loads the tick; on a held update the hook
// biases it down by one so the retail `addiu +1` at the rejoin nets zero and
// the store keeps the tick frozen, exactly as the MIPS body did. `$v1` is left
// as the hold decision, matching the body's `sltu`, so the register file at the
// rejoin is identical.
std::uint32_t gameClockHook(
    psx::R3000State& state,
    RetimeState& retime,
    psx::R3000Runtime& runtime,
    std::uint32_t rejoin, const void* context) noexcept {
    retime.decide();
    std::uint32_t tick = 0U;
    static_cast<void>(runtime.read32(state.gpr[4] + 0x1CU, tick));
    if (retime.hold()) {
        --tick;
    }
    hostWriteRegister(state, 2, tick);                       // $v0
    hostWriteRegister(state, 3, retime.hold() ? 1U : 0U);    // $v1
    return rejoin;
}

// A `jal <callee>` gate, shared by every call-gate site via its
// `RetimeCallGate` context: on a held update skip the call and resume at the
// rejoin; on a counted update set `$ra` to the rejoin (exactly as the displaced
// `jal` would) and start the callee, which returns there. The site's delay-slot
// instruction has already run as the hook's delay slot, so its effect is
// present on both paths — the VRAM gate's `clear $s4` in particular.
std::uint32_t callGateHook(
    psx::R3000State& state,
    RetimeState& retime,
    psx::R3000Runtime&,
    std::uint32_t rejoin,
    const void* context) noexcept {
    const auto* gate = static_cast<const RetimeCallGate*>(context);
    if (retime.hold()) {
        return rejoin;
    }
    hostWriteRegister(state, 31, rejoin);
    return gate->callee;
}

// `FadeUpdate__4Game` (`0x8002C9BC`) is the boot executable's shared private
// fullscreen fade: it adds the step at `gp+0xD3C` (17) to the grayscale byte at
// `gp+0xD40` once per render-loop pass, clamping at 255 (verified in Ghidra).
// The four transition loops run outside `Step__4Time`, so this hook keeps its
// own accumulator, seeded one short of the divisor whenever the current
// grayscale is zero so a fade's first call always advances. On a held update
// the step is nulled so the retail add leaves the grayscale unchanged.
//
// `$v0` is restored to the grayscale and `$a0` to the accumulator value,
// matching the registers the MIPS body leaves at the rejoin.
std::uint32_t fullscreenFadeHook(
    psx::R3000State& state,
    RetimeState& retime,
    psx::R3000Runtime& runtime,
    std::uint32_t rejoin, const void* context) noexcept {
    const auto divisor = retime.divisor == 0U ? 1U : retime.divisor;
    const auto gp = state.gpr[28];
    std::uint8_t grayscale = 0U;
    static_cast<void>(runtime.read8(gp + 0xd40U, grayscale));
    auto& accum = retime.fade_accum;
    if (grayscale == 0U) {
        accum = divisor - 1U; // a fade's first call always advances
    }
    accum += 1U;
    if (accum < divisor) {
        hostWriteRegister(state, 3, 0U); // held: the retail add adds nothing
    } else {
        accum = 0U;
        std::uint8_t step = 0U;
        static_cast<void>(runtime.read8(gp + 0xd3cU, step));
        hostWriteRegister(state, 3, step); // counted: the displaced step load
    }
    hostWriteRegister(state, 4, accum);       // $a0, as the body leaves it
    hostWriteRegister(state, 2, grayscale);   // $v0, as the body restores it
    return rejoin;
}

// Models a held prologue's displaced instruction on the counted path. Only the
// forms these prologues use are decoded: `move`/`addu` (SPECIAL funct 0x21)
// and `lw` (opcode 0x23). The value is written directly, which is what the
// rejoin reads; the register is cleared of any pending load delay so the
// pipeline cannot clobber it.
void modelDisplacedInstruction(
    psx::R3000State& state,
    psx::R3000Runtime& runtime,
    std::uint32_t word) noexcept {
    const auto opcode = word >> 26U;
    if (opcode == 0U && (word & 0x3FU) == 0x21U) { // addu / move
        const auto rd = (word >> 11U) & 0x1FU;
        const auto rs = (word >> 21U) & 0x1FU;
        const auto rt = (word >> 16U) & 0x1FU;
        hostWriteRegister(state, rd, state.gpr[rs] + state.gpr[rt]);
        return;
    }
    if (opcode == 0x23U) { // lw
        const auto rs = (word >> 21U) & 0x1FU;
        const auto rt = (word >> 16U) & 0x1FU;
        const auto imm = static_cast<std::int32_t>(
            static_cast<std::int16_t>(word & 0xFFFFU));
        std::uint32_t value = 0U;
        static_cast<void>(runtime.read32(
            state.gpr[rs] + static_cast<std::uint32_t>(imm), value));
        hostWriteRegister(state, rt, value);
        return;
    }
    // Unknown form: leave the registers alone. A site that needs a new form
    // fails its diff gate and is caught, not silently mis-patched.
}

// The shared held-prologue gate for every obstacle `Think` and the two world
// effects. The prologue has already allocated its frame and saved the
// callee-saved registers when the site runs; on a held update the hook
// restores them, unwinds the frame, and returns to the caller, exactly as the
// MIPS body's `lw $sX, off($sp); jr $ra; addiu $sp, frame` does. On a counted
// update it models the displaced instruction (usually `move $sX, $a0`) and
// resumes at the rejoin. The site's delay slot (a saved-`$ra` store) has
// already run as the hook's delay slot, so it is present on both paths.
std::uint32_t heldPrologueHook(
    psx::R3000State& state,
    RetimeState& retime,
    psx::R3000Runtime& runtime,
    std::uint32_t rejoin,
    const void* context) noexcept {
    const auto* prologue =
        static_cast<const RetimeHeldPrologue*>(context);
    if (retime.hold()) {
        for (const auto restore : prologue->restores) {
            if (restore == 0U) {
                continue;
            }
            const auto rs = (restore >> 21U) & 0x1FU;
            const auto rt = (restore >> 16U) & 0x1FU;
            const auto imm = static_cast<std::int32_t>(
                static_cast<std::int16_t>(restore & 0xFFFFU));
            std::uint32_t saved = 0U;
            static_cast<void>(runtime.read32(
                state.gpr[rs] + static_cast<std::uint32_t>(imm), saved));
            hostWriteRegister(state, rt, saved);
        }
        state.gpr[29] += prologue->frame_size; // unwind
        return state.gpr[31];                  // return to the caller
    }
    modelDisplacedInstruction(state, runtime, prologue->displaced);
    return rejoin;
}

// The held-prologue sites, in `pc` order. The overlay sites (OL1/NBOL) reuse
// load addresses across levels, so they must only be live while their overlay
// is loaded — the app decides that subset per frame with
// `buildActiveRetimeHooks`, gated by each site's fingerprint window.
// `RetimeHeldPrologue::restores` lists the `lw $sX, imm($sp)` words for the
// callee-saved registers the prologue has already stored (the second is zero
// except `Conveyor`, which also restores `$s1`).
struct ObjectPrologue {
    std::string_view name;
    std::uint32_t site;
    std::uint32_t rejoin;
    RetimeHeldPrologue prologue;
};

inline constexpr std::array<ObjectPrologue, 23U> object_prologues{{
    {"destructible_think",
     0x80010718U, 0x80010720U, {0x00808021U, 0x18U, {0x8FB00010U, 0U}}},
    {"generator_think",
     0x80011308U, 0x80011310U, {0x00808021U, 0x20U, {0x8FB00010U, 0U}}},
    {"enemy_generator_think",
     0x800117E0U, 0x800117E8U, {0x00808821U, 0x20U, {0x8FB10014U, 0U}}},
    {"throwing_generator_think",
     0x8001210CU, 0x80012114U, {0x00808821U, 0x20U, {0x8FB10014U, 0U}}},
    {"collectible_think",
     0x80012C8CU, 0x80012C94U, {0x00808821U, 0x30U, {0x8FB10024U, 0U}}},
    {"explosive_think",
     0x80013A50U, 0x80013A58U, {0x00808021U, 0x28U, {0x8FB00020U, 0U}}},
    {"dynamic_obstacle_think",
     0x80014160U, 0x80014168U, {0x00808021U, 0x18U, {0x8FB00010U, 0U}}},
    {"blast_think",
     0x80016530U, 0x80016538U, {0x00808021U, 0x40U, {0x8FB00030U, 0U}}},
    {"trapdoor_think",
     0x80017368U, 0x80017370U, {0x00808021U, 0x18U, {0x8FB00010U, 0U}}},
    {"pushable_think",
     0x80017FB4U, 0x80017FBCU, {0x00808021U, 0x20U, {0x8FB00010U, 0U}}},
    {"door_think",
     0x8001AF6CU, 0x8001AF74U, {0x8C84D718U, 0x28U, {0x8FB00020U, 0U}}},
    {"conveyor_think",
     0x8001C270U, 0x8001C278U,
     {0x00808021U, 0x20U, {0x8FB00010U, 0x8FB10014U}}},
    {"kicknroll_think",
     0x8001C7C0U, 0x8001C7C8U, {0x00808021U, 0x18U, {0x8FB00010U, 0U}}},
    {"knockdown_think",
     0x8001DA50U, 0x8001DA58U, {0x00808021U, 0x38U, {0x8FB00030U, 0U}}},
    {"crusher_think",
     0x8001F9F8U, 0x8001FA00U, {0x00808821U, 0x30U, {0x8FB10024U, 0U}}},
    {"launcher_think",
     0x800201ACU, 0x800201B4U, {0x00808821U, 0x30U, {0x8FB10024U, 0U}}},
    {"pendulum_think",
     0x800252BCU, 0x800252C4U, {0x00809821U, 0x70U, {0x8FB30064U, 0U}}},
    {"ladder_think",
     0x80089FDCU, 0x80089FE4U, {0x00808021U, 0x18U, {0x8FB00010U, 0U}}},
    {"hud_anim_text_overlay",
     0x8008F66CU, 0x8008F674U, {0x00808021U, 0x18U, {0x8FB00010U, 0U}}},
    {"hud_tally_update",
     0x80090AC8U, 0x80090AD0U, {0x00809021U, 0x28U, {0x8FB20018U, 0U}}},
    {"world_effect",
     0x8008BA2CU, 0x8008BA34U, {0x00808821U, 0x38U, {0x8FB1002CU, 0U}}},
    {"flying_world_effect",
     0x8008C484U, 0x8008C48CU, {0x00808021U, 0x20U, {0x8FB00018U, 0U}}},
    {"untouchable_think",
     0x800A6548U, 0x800A6550U, {0x00808021U, 0x28U, {0x8FB00020U, 0U}}},
}};

// The boundary between overlay-resident and boot held prologues. Every OL1/NBOL
// obstacle `Think` (and `Pushable`) loads below this; the boot held prologues
// (the two world effects, `Ladder`, `Untouchable`) load well above it and are
// always live. Overlay prologues are gated per loaded overlay via
// `retimeOverlayHooks()`; boot ones stay in `retimeObjectHooks()`.
constexpr std::uint32_t overlay_prologue_limit = 0x80030000U;

// The overlay fingerprint windows, embedded from the extracted overlay bytes
// (formerly carried by the byte overlay trampolines). Each run is unique across
// all four `*_REL.BIN` files, so it decides whether the hook's overlay is the
// one currently loaded at the hook's PC.

// `Think__5Arrow` (`0x8001BBD8`) bob counter window.
inline constexpr std::array<std::uint32_t, 8U> arrow_bob_window{
    0x8E420074U, 0x00000000U, 0x24420001U, 0xAE420074U,
    0x28420008U, 0x14400002U, 0x2402FFF9U, 0xAE420074U};
// `Butch::_Stomp`'s landing-event counter window. This sequence occurs once
// in BOL and not in any of the three overlays that reuse its load range.
inline constexpr std::array<std::uint32_t, 12U> butch_stomp_window{
    0x8C820268U, 0x00000000U, 0x24420001U, 0xAC820268U,
    0x2842002BU, 0x10400005U, 0x27B20030U, 0x8C820058U,
    0x00000000U, 0x34420002U, 0xAC820058U, 0x8C830268U};
// The BOL counter-hold windows (boss/behaviour state timers). Each occurs
// exactly once across BOL/NBOL/OL1/OL2_REL.BIN.
inline constexpr std::array<std::uint32_t, 12U> boss_collapse_window{
    0x8E020134U, 0x00000000U, 0x24420001U, 0xAE020134U,
    0x86030134U, 0x860201D0U, 0x00000000U, 0x0043102AU,
    0x10400043U, 0x00000000U, 0x8E020050U, 0x00000000U};
inline constexpr std::array<std::uint32_t, 12U> dante_missile_window{
    0x241E0001U, 0x8E620268U, 0x26520001U, 0x24420001U,
    0x08006FAFU, 0xAE620268U, 0x13C00011U, 0x24050045U,
    0x3C04800EU, 0x8C84D6B4U, 0x00000000U, 0x8C820008U};
inline constexpr std::array<std::uint32_t, 12U> dante_retarget_window{
    0x00003021U, 0x080071BAU, 0xAE800298U, 0x080071BAU,
    0xAE820298U, 0x8E82029CU, 0x00000000U, 0x1440000EU,
    0x00000000U, 0x3C02800EU, 0x8C42D6B4U, 0x00000000U};
inline constexpr std::array<std::uint32_t, 12U> butch_dms_window{
    0x8E020040U, 0x00000000U, 0x24420001U, 0xAE020040U,
    0x2842001AU, 0x144000AEU, 0x00000000U, 0x8F820C94U,
    0x00000000U, 0x0053102AU, 0x10400031U, 0xAE000040U};
inline constexpr std::array<std::uint32_t, 12U> butch_dms_charge_window{
    0x8E020040U, 0x00000000U, 0x24420001U, 0xAE020040U,
    0x28420033U, 0x1440000AU, 0x27B10028U, 0x24020AAAU,
    0xAFA20010U, 0x02402021U, 0x02202821U, 0x02603021U};
inline constexpr std::array<std::uint32_t, 12U> paul_recovery_window{
    0x8EC20068U, 0x00000000U, 0x10400002U, 0x2442FFFFU,
    0xAEC20068U, 0x00008821U, 0x3C048002U, 0x0C01E0C5U,
    0x248425CCU, 0x0C022A38U, 0x00402021U, 0x00408021U};
inline constexpr std::array<std::uint32_t, 12U> paul_attack_window{
    0x8EC20064U, 0x00000000U, 0x2442FFFFU, 0x1840000FU,
    0xAEC20064U, 0x8EC20018U, 0x00000000U, 0x8C430164U,
    0x00000000U, 0x2462FFBBU, 0x2C420004U, 0x14400005U};
inline constexpr std::array<std::uint32_t, 12U> henchman_window{
    0xAF800CC8U, 0x08007B66U, 0x00000000U, 0x8F830CC8U,
    0x00000000U, 0x2862000FU, 0x10400004U, 0x24620001U,
    0xAF820CC8U, 0x08007B66U, 0x00000000U, 0x10E00034U};
inline constexpr std::array<std::uint32_t, 12U> counter_attack_window{
    0x8C620094U, 0x00000000U, 0x24420001U, 0x08007BA4U,
    0xAC620094U, 0xAC660080U, 0x8C8200A8U, 0x00000000U,
    0x00021080U, 0x00821021U, 0xAC400094U, 0x8C8200A8U};
// `Pushable::HandleHumanoidCollision`'s push-engage gate (`0x80018EFC`,
// bnez $v1, 0x80019178). Unique across all four `*_REL.BIN` files.
inline constexpr std::array<std::uint32_t, 12U> pushable_collision_window{
    0x8E2200A4U, 0x00000000U, 0x00401821U, 0x24420001U, 0x28630005U,
    0x1460009EU, 0xAE2200A4U, 0x8E2B001CU, 0x8E2C0020U, 0x8E2D0024U,
    0xAE2B0074U, 0xAE2C0078U};
// `_TargetMissileAttack` increments `Dante+0x298` by one before the store, so
// a held update restores `new - 1`. The store sits in the `j`'s delay slot at
// `0x8001C3DC`.
inline constexpr RetimeStoreUndo dante_retarget_undo{-1, 0x8001C3DCU};
// `Think__5Stack` timeline gate window.
inline constexpr std::array<std::uint32_t, 12U> stack_timeline_window{
    0x27BDFFE0U, 0xAFB10014U, 0x00808821U, 0x24030001U, 0xAFBF0018U,
    0xAFB00010U, 0x8E22008CU, 0x8E240088U, 0x8C50001CU, 0x10830009U,
    0x28820002U, 0x1440000DU};
// `Think__5Stack` sound gate window.
inline constexpr std::array<std::uint32_t, 12U> stack_sound_window{
    0x8E020008U, 0x00000000U, 0x8C420014U, 0x00000000U, 0x0040F809U,
    0x02002021U, 0x8E24009CU, 0x00000000U, 0x10800003U, 0x00000000U,
    0x0C02B52BU, 0x00000000U};
// `Platform::Move` path-speed window.
inline constexpr std::array<std::uint32_t, 12U> platform_move_speed_window{
    0x00000000U, 0x8C820008U, 0x8E2500B8U, 0x8C42001CU, 0x00000000U,
    0x0040F809U, 0x00000000U, 0x10400003U, 0x00000000U, 0x0C008795U,
    0x02202021U, 0x8E2200D0U};
// `Think__8Platform` (`0x80021918`) window, shared by the divide-based
// conversion hooks.
inline constexpr std::array<std::uint32_t, 12U> platform_think_window{
    0x27BDFFA0U, 0xAFB00050U, 0x00808021U, 0xAFBF0058U, 0xAFB10054U,
    0x8E040140U, 0x00000000U, 0x10800004U, 0x24027FFFU, 0x0C02B2BFU,
    0x00000000U, 0x24027FFFU};
// `Think__8Pushable` (`0x80017FAC`) window.
inline constexpr std::array<std::uint32_t, 12U> pushable_window{
    0x27BDFFE0U, 0xAFB00010U, 0x00808021U, 0xAFBF0018U, 0xAFB10014U,
    0x8E020080U, 0x00000000U, 0x14400009U, 0x00008821U, 0x8E020084U,
    0x00000000U, 0x14400005U};

// One overlay obstacle `Think` held prologue's fingerprint: the entry (which is
// also the window's address) plus the twelve retail words that must be present
// for the hook's overlay to be the one loaded.
struct OverlayObstacleThinkWindow {
    std::string_view name;
    std::uint32_t entry;
    std::array<std::uint32_t, 12U> window;
};

inline constexpr std::array<OverlayObstacleThinkWindow, 16U>
    overlay_obstacle_think_windows{{
        {"door_think", 0x8001AF5CU,
         {0x27BDFFD8U, 0xAFB00020U, 0x00808021U, 0x3C04800EU, 0x8C84D718U,
          0xAFBF0024U, 0x0C0115BDU, 0x00000000U, 0x38420007U, 0x8E030098U,
          0x2C440001U, 0x2C620006U}},
        {"conveyor_think", 0x8001C260U,
         {0x27BDFFE0U, 0xAFB10014U, 0x00008821U, 0xAFB00010U, 0x00808021U,
          0xAFBF0018U, 0x2A220004U, 0x1040000AU, 0x00000000U, 0x8E040080U,
          0x00000000U, 0x10800003U}},
        {"kicknroll_think", 0x8001C7B8U,
         {0x27BDFFE8U, 0xAFB00010U, 0x00808021U, 0xAFBF0014U, 0x92020073U,
          0x00000000U, 0x1040001BU, 0x00000000U, 0x8E040098U, 0x00000000U,
          0x10800003U, 0x00000000U}},
        {"knockdown_think", 0x8001DA48U,
         {0x27BDFFC8U, 0xAFB00030U, 0x00808021U, 0xAFBF0034U, 0x92020073U,
          0x00000000U, 0x10400046U, 0x24020001U, 0x8E030074U, 0x00000000U,
          0x10620007U, 0x28620002U}},
        {"crusher_think", 0x8001F9F0U,
         {0x27BDFFD0U, 0xAFB10024U, 0x00808821U, 0xAFBF0028U, 0xAFB00020U,
          0x92220073U, 0x00000000U, 0x1040001BU, 0x26300060U, 0x8E220074U,
          0x00000000U, 0x10400009U}},
        {"launcher_think", 0x800201A4U,
         {0x27BDFFD0U, 0xAFB10024U, 0x00808821U, 0x8F820C3CU, 0x27A50010U,
          0xAFBF0028U, 0xAFB00020U, 0x8A260063U, 0x9A260060U, 0x8A270067U,
          0x9A270064U, 0x8A28006BU}},
        {"pendulum_think", 0x800252B4U,
         {0x27BDFF90U, 0xAFB30064U, 0x00809821U, 0xAFBF0068U, 0xAFB20060U,
          0xAFB1005CU, 0xAFB00058U, 0x8E6A0028U, 0x8E6B002CU, 0x8E6C0030U,
          0xAFAA0010U, 0xAFAB0014U}},
        {"destructible_think", 0x80010710U,
         {0x27BDFFE8U, 0xAFB00010U, 0x00808021U, 0xAFBF0014U, 0x8E020080U,
          0x00000000U, 0x1440000DU, 0x00000000U, 0x92020073U, 0x00000000U,
          0x10400009U, 0x00000000U}},
        {"generator_think", 0x80011300U,
         {0x27BDFFE0U, 0xAFB00010U, 0x00808021U, 0xAFB20018U, 0x00009021U,
          0xAFB10014U, 0x02408821U, 0xAFBF001CU, 0x8E0200CCU, 0x00000000U,
          0x0242102AU, 0x1040002BU}},
        {"enemy_generator_think", 0x800117D8U,
         {0x27BDFFE0U, 0xAFB10014U, 0x00808821U, 0xAFBF001CU, 0xAFB20018U,
          0xAFB00010U, 0x8E22011CU, 0x00000000U, 0x14400019U, 0x00003021U,
          0x00C09021U, 0x3C04800EU}},
        {"throwing_generator_think", 0x80012104U,
         {0x27BDFFE0U, 0xAFB10014U, 0x00808821U, 0xAFBF0018U, 0xAFB00010U,
          0x8E2200ECU, 0x00000000U, 0x1040000AU, 0x00008021U, 0x3C02800EU,
          0x8C42D6B4U, 0x00000000U}},
        {"collectible_think", 0x80012C84U,
         {0x27BDFFD0U, 0xAFB10024U, 0x00808821U, 0xAFBF002CU, 0xAFB20028U,
          0xAFB00020U, 0x8E2200ACU, 0x00000000U, 0x18400002U, 0x2442FFFFU,
          0xAE2200ACU, 0x8E240074U}},
        {"explosive_think", 0x80013A48U,
         {0x27BDFFD8U, 0xAFB00020U, 0x00808021U, 0xAFBF0024U, 0x8E030074U,
          0x00000000U, 0x2C620005U, 0x1040004DU, 0x3C028002U, 0x24429618U,
          0x00031880U, 0x00621821U}},
        {"dynamic_obstacle_think", 0x80014158U,
         {0x27BDFFE8U, 0xAFB00010U, 0x00808021U, 0xAFBF0014U, 0x8E020008U,
          0x00000000U, 0x8C420010U, 0x00000000U, 0x0040F809U, 0x00000000U,
          0x0C0051CEU, 0x02002021U}},
        {"blast_think", 0x80016528U,
         {0x27BDFFC0U, 0xAFB00030U, 0x00808021U, 0xAFB10034U, 0x24037FFFU,
          0x27A20020U, 0xAFBF0038U, 0xA7A30020U, 0xA4430002U, 0xA4430004U,
          0x24038001U, 0xA4430006U}},
        {"trapdoor_think", 0x80017360U,
         {0x27BDFFE8U, 0xAFB00010U, 0x00808021U, 0xAFBF0014U, 0x8E0200A4U,
          0x00000000U, 0x2442FFFEU, 0x2C420002U, 0x10400009U, 0x00000000U,
          0x8E020008U, 0x00000000U}},
    }};

// The fingerprint an overlay held prologue is gated by, looked up (by name)
// from the embedded window table below. Each window is a run of consecutive
// retail words read out of the extracted overlay, unique across all four
// `*_REL.BIN` files, so it decides whether the hook's overlay is the one
// currently loaded at the hook's PC.
struct OverlayFingerprint {
    std::string_view name;
    std::uint32_t window_address;
    std::span<const std::uint32_t> window;
};

[[nodiscard]] std::pair<std::uint32_t, std::span<const std::uint32_t>>
overlayFingerprintFor(std::string_view name) noexcept {
    static const std::vector<OverlayFingerprint> fingerprints = [] {
        std::vector<OverlayFingerprint> out;
        const auto add = [&](std::string_view entry_name,
                             std::uint32_t address,
                             std::span<const std::uint32_t> words) {
            out.push_back(OverlayFingerprint{entry_name, address, words});
        };
        // The recompute/gate hooks' windows.
        add("arrow_bob", 0x8001BDA4U,
            std::span{arrow_bob_window});
        add("stack_timeline", 0x8001EE28U,
            std::span{stack_timeline_window});
        add("stack_sound", 0x8001EEA4U,
            std::span{stack_sound_window});
        add("platform_move_speed", 0x800227C8U,
            std::span{platform_move_speed_window});
        // The Platform divide-based conversion hooks share `Think`'s window.
        add("platform_think", 0x80021918U,
            std::span{platform_think_window});
        // `Pushable`'s window.
        add("pushable_think", 0x80017FACU,
            std::span{pushable_window});
        // The sixteen overlay obstacle `Think` prologues.
        for (const auto& hold : overlay_obstacle_think_windows) {
            add(hold.name, hold.entry, hold.window);
        }
        return out;
    }();
    for (const auto& entry : fingerprints) {
        if (entry.name == name) {
            return {entry.window_address, entry.window};
        }
    }
    return {0U, {}};
}

// The `jal animLoopDSTACK__Fv` gate at `0x8002B6D0`, reached from
// `gsEndLevelLoopState` immediately after `Step__4Time`, so the clock's
// decision is already published when it reads the hold. `animLoopDSTACK`
// (`0x8002B368`) advances the scene-animation lists once per authored 30 Hz
// frame (verified in `GEN/game.c`).
inline constexpr RetimeCallGate anim_loop_gate{0x8002B368U};
// The `jal updateVramAnims__8Director` gate at `0x8002AD90` inside
// `DrawEverythingHandler`; its delay slot `clear $s4` must run on both paths,
// and does as the hook's delay slot. `updateVramAnims` (`0x8003BB90`) advances
// the Director's two VRAM flipbooks (verified in `GEN/DIRECTOR.c`).
inline constexpr RetimeCallGate vram_anim_gate{0x8003BB90U};
// The whole obstacle collision/carry pass, gated to the authored rate. See the
// `obstacle_collision_pass` clock hook below.
inline constexpr RetimeCallGate obstacle_collision_gate{0x800A96ECU};
// `Obstacle::HandleHumanoidObstacleCollision`, used directly for the active
// pusher on a held update. The list wrapper at `0x800A96EC` does nothing beyond
// walking active humanoids and calling this function for each one.
inline constexpr std::uint32_t humanoid_obstacle_collision = 0x8007C178U;
// The pause/tally/FE selected-item colour pulse (`MenuColorNext__FR12xcColour1555`).
// Its first action is `jal CalcNextColor__FR12xcColour1555`, the only place the
// colour-chase accumulator steps. Gating that call on the master decision holds
// the red-to-yellow pulse to the authored rate at any guest update rate; the
// site's delay slot (`move $s0, $a0`) runs on both paths. The `$s0`/`$ra`
// prologue saves at `0x8005CD14`/`0x8005CD18` are unaffected.
inline constexpr RetimeCallGate menu_colour_calc_gate{0x8005CC44U};
// `CBVEffect::Update` (`0x8008CF94`) is the third WEffect-family Update
// override (beside `WEffect::Update` at `0x8008BA24` and `FWEffect::Update`
// at `0x8008C47C`) and its only call is `Update__11CBVPrimData`
// (`0x80098BE0`), which advances the colour/UV animation's `+0x3C` countdown
// and `+0x28` UV cursor once per call. The held prologues at
// `0x8008BA2C`/`0x8008C484` do not catch it — the vtable dispatches CBV
// effects to this distinct address — so the `jal` is gated directly. The
// site's delay slot is a `nop`.
inline constexpr RetimeCallGate cbv_uv_gate{0x80098BE0U};

// Gating the entire obstacle pass fixed passenger/carry timing, but Pushable's
// collision handler is also the producer for the player's `AS_PushObject`
// contact bit and the push displacement. If it is skipped, `_PushObject` sees
// an unrefreshed contact on the next update, leaves the state, and the counted
// collision immediately enters it again: the visible push/not-push flicker.
//
// Keep the coupled pass at the authored rate normally. Five contact families
// cannot wait for the counted update:
//
// - an active `AS_PushObject` humanoid needs its contact bit refreshed every
//   update (the push displacement itself is held by the OL1
//   `pushable_collision_hold` gate, so the pushable still moves once per
//   authored step);
// - an `AS_Run` humanoid moving across a dynamic obstacle needs its grounded
//   contact and passenger membership refreshed every update. Otherwise one
//   missed authored pass changes it to `AS_Fall`, and the airborne exception
//   lands it again on the next held update: the visible run/fall vibration;
// - an airborne jump/fall humanoid needs obstacle collision on every sub-step.
//   With a passenger ticket, this disembarks it before a sub-stepped
//   Platform::MovePassengers carries it once more after takeoff. Without a
//   ticket, it prevents a descending `AS_Fall`/`AS_HardFall` humanoid from
//   crossing a thin dynamic obstacle entirely during a held update and being
//   below it by the next authored collision pass.
// - the four ladder states consume the contact bit Ladder's collision handler
//   publishes. The bit is cleared at the end of every update, so skipping the
//   held collision makes the latch/climb state reinitialize itself before the
//   next counted update and resets the climb animation forever.
// - `AS_Hotfoot` (on fire): `Untouchable`'s fire contact bit (`+0x170:3`) is
//   what keeps the burning state alive, and `Think__8Humanoid` clears the
//   whole context word at the end of every update. Skipping the held
//   collision lets `_Hotfoot` exit to `AS_Run`, and the run exception then
//   re-ignites the fire on the next update: the visible run/burn flicker on
//   fire pits. Unlike the other exceptions, the burning state needs only the
//   bit — the inner collision would also apply the per-authored burn damage
//   tick on the held update — so the hook re-issues `+0x170:3` directly.
//
// For these cases run that humanoid's inner obstacle collision directly on a
// held update. This is the same call the list wrapper would make, but it does
// not also sub-step every other humanoid's passenger and ledge state.
// Pushable::Think remains authored-rate gated separately, as before the
// whole-pass gate landed.
std::uint32_t obstacleCollisionPassHook(
    psx::R3000State& state,
    RetimeState& retime,
    psx::R3000Runtime& runtime,
    std::uint32_t rejoin,
    const void* context) noexcept {
    constexpr std::uint32_t active_humanoid_flag = 1U << 6U;
    constexpr std::uint32_t action_state_offset = 0x164U;
    constexpr std::uint32_t action_run = 0x0AU;
    constexpr std::uint32_t action_run_jump = 0x06U;
    constexpr std::uint32_t action_jump = 0x08U;
    constexpr std::uint32_t action_fall = 0x0DU;
    constexpr std::uint32_t action_hard_fall = 0x0EU;
    constexpr std::uint32_t action_push_object = 0x13U;
    constexpr std::uint32_t action_hotfoot = 0x1EU;
    constexpr std::uint32_t action_ladder_latch_top = 0x19U;
    constexpr std::uint32_t action_climb_ladder = 0x1CU;
    constexpr std::size_t max_humanoids = 64U;

    if (retime.hold()) {
        // The site's real delay slot has already copied the humanoid-list
        // header from $s0 to $a0. A ccMinList's first word is its head and each
        // humanoid's first word is the next link.
        std::uint32_t humanoid = 0U;
        if (!runtime.read32(state.gpr[4], humanoid)) {
            return rejoin;
        }
        for (std::size_t index = 0U;
             humanoid != 0U && index < max_humanoids;
             ++index) {
            std::uint32_t flags = 0U;
            std::uint32_t action_state = 0U;
            if (!runtime.read32(humanoid + 0x58U, flags) ||
                !runtime.read32(
                    humanoid + action_state_offset, action_state)) {
                return rejoin;
            }
            const auto airborne = action_state == action_run_jump ||
                action_state == action_jump ||
                action_state == action_fall ||
                action_state == action_hard_fall;
            const auto ladder_contact =
                action_state >= action_ladder_latch_top &&
                action_state <= action_climb_ladder;
            if ((flags & active_humanoid_flag) != 0U &&
                (action_state == action_push_object || airborne ||
                 action_state == action_run || ladder_contact ||
                 action_state == action_hotfoot)) {
                if (action_state == action_hotfoot) {
                    // The burning state needs only the fire pit's contact bit
                    // (`+0x170:3`); the full inner collision would also apply
                    // `Untouchable`'s per-authored damage tick (`+0x90 == 0`)
                    // on this held update, doubling the burn rate. Re-issue
                    // the bit directly instead.
                    std::uint32_t context = 0U;
                    if (runtime.read32(humanoid + 0x170U, context)) {
                        static_cast<void>(runtime.write32(
                            humanoid + 0x170U, context | 8U));
                    }
                    return rejoin;
                }
                hostWriteRegister(state, 4, humanoid); // $a0, inner-call arg
                hostWriteRegister(state, 31, rejoin);  // $ra, as the wrapper
                return humanoid_obstacle_collision;
            }
            std::uint32_t next = 0U;
            if (!runtime.read32(humanoid, next) || next == humanoid) {
                return rejoin;
            }
            humanoid = next;
        }
        return rejoin;
    }

    const auto* gate = static_cast<const RetimeCallGate*>(context);
    hostWriteRegister(state, 31, rejoin);
    return gate->callee;
}

// The pause menu's per-frame decider. `Step__4Time` (the play-state decider)
// does not run while `gsMenuState` is up, so the master hold decision would
// otherwise stay frozen at whatever the last play update decided — the pause
// menu's own authored-rate animation (the selected-item colour chase) then
// either never advances or advances every guest update, depending on the
// stale phase. `MenuDraw__FP7MenuMgr` (`0x80029DB8`) runs once per guest
// update in every menu state (pause, location menu, end-level), so this hook
// publishes the decision there. It is never called during play, so the
// `Step__4Time` decider and this one cannot both run in one update. The site
// is the prologue's `move $s0, $a0` (`0x80029DC0`); its delay slot
// (`sw $ra, 0x18($sp)`) runs on both paths and the hook models the displaced
// move. A divisor of one always counts, exactly as a retail-cadence menu
// needs.
std::uint32_t menuFrameDecisionHook(
    psx::R3000State& state,
    RetimeState& retime,
    psx::R3000Runtime&,
    std::uint32_t rejoin, const void*) noexcept {
    retime.decide();
    hostWriteRegister(state, 16, state.gpr[4]); // $s0 = $a0, displaced move
    return rejoin;
}

constexpr std::array<RetimeHook, 8U> clock_hooks{{
    {"menu_frame_decision",
     0x80029DC0U,
     0x80029DC8U,
     RetimeHookKind::semantic,
     &menuFrameDecisionHook},
    {"vram_animation",
     0x8002AD90U,
     0x8002AD98U,
     RetimeHookKind::gate,
     &callGateHook,
     &vram_anim_gate},
    {"scene_animation",
     0x8002B6D0U,
     0x8002B6D8U,
     RetimeHookKind::gate,
     &callGateHook,
     &anim_loop_gate},
    {"fullscreen_fade",
     0x8002C9C0U,
     0x8002C9C8U,
     RetimeHookKind::semantic,
     &fullscreenFadeHook},
    {"game_clock",
     0x80044A40U,
     0x80044A48U,
     RetimeHookKind::semantic,
     &gameClockHook},
    {"obstacle_collision_pass",
     0x80055D80U,
     0x80055D88U,
     RetimeHookKind::gate,
     &obstacleCollisionPassHook,
     &obstacle_collision_gate},
    {"menu_colour_pulse",
     0x8005CD1CU,
     0x8005CD24U,
     RetimeHookKind::gate,
     &callGateHook,
     &menu_colour_calc_gate},
    {"cbv_effect_uv_animation",
     0x8008CFACU,
     0x8008CFB4U,
     RetimeHookKind::gate,
     &callGateHook,
     &cbv_uv_gate},
}};

// ---------------------------------------------------------------------------
// Boot ledge / obstacle-collision-cadence hooks
//
// The boot-executable `--retime-clock` readers beyond the obstacle `Think`
// holds. Each consults the master hold decision `retimedGameClock` publishes
// and never accumulates. Verified against `Player::_LedgeLatch` (`AI/PLAYER.c`)
// and `Obstacle::HandleHumanoidObstacleCollision` (`AI/OBSTACLE.c`).

// `Player::_LedgeLatch`'s velocity reset (`0x80033558`). The site is
// `addiu $v0, $zero, 0x1f`; its delay slot (`sw $zero, 0x6c($s2)`, velocity Z)
// runs before this hook. The rejoin (`0x80033568`) is past retail's own
// velocity-Y and velocity-X stores, so the hook performs them: velocity X
// always, velocity Y only on a counted update. Holding velocity Y on a skipped
// update lets gravity keep accumulating across the authored frame, so the hang
// sinks the full `gravity/2` per authored frame rather than per guest update.
// `$s2` is the humanoid; `$v0` is set by the displaced instruction, matching
// the register the retail tail compares against.
std::uint32_t ledgeLatchVelocityResetHook(
    psx::R3000State& state,
    RetimeState& retime,
    psx::R3000Runtime& runtime,
    std::uint32_t rejoin, const void* context) noexcept {
    hostWriteRegister(state, 2, 0x1FU); // $v0 = displaced addiu $v0, $zero, 0x1f
    const auto humanoid = state.gpr[18]; // $s2
    static_cast<void>(runtime.write32(humanoid + 0x64U, 0U)); // velocity X
    if (!retime.hold()) {
        static_cast<void>(runtime.write32(humanoid + 0x68U, 0U)); // velocity Y
    }
    return rejoin;
}

// Humanoid::_ClimbLadder advances its upward animation explicitly instead of
// through the general scene-animation lists. That animation supplies the
// climb's root motion, and the following frame test emits its footsteps. Run
// both only on an authored update; on a held update jump straight to the
// epilogue so neither the frame nor its sound is repeated.
std::uint32_t ladderClimbUpHook(
    psx::R3000State& state,
    RetimeState& retime,
    psx::R3000Runtime&,
    std::uint32_t rejoin, const void*) noexcept {
    constexpr std::uint32_t inc_frame = 0x80071300U;
    constexpr std::uint32_t epilogue = 0x80069FD4U;
    if (retime.hold()) {
        return epilogue;
    }
    hostWriteRegister(state, 31, rejoin);
    return inc_frame;
}

// The downward branch writes position Y directly rather than using
// DynamicThing::Move, so the shared position integrator cannot sub-step it.
// The intercepted instruction is `addiu $v0,$v0,-0x40`; its real delay-slot
// store has already run when this hook executes, so replace that store with
// the divided delta and leave $v0 matching memory.
std::uint32_t ladderSlideDownHook(
    psx::R3000State& state,
    RetimeState& retime,
    psx::R3000Runtime& runtime,
    std::uint32_t rejoin, const void*) noexcept {
    const auto step = divideHalfToEven(-0x40, retime.divisor);
    const auto retimed = state.gpr[2] + static_cast<std::uint32_t>(step);
    hostWriteRegister(state, 2, retimed);
    static_cast<void>(runtime.write32(state.gpr[18] + 4U, retimed));
    return rejoin;
}

// The coupled obstacle collision/carry/ticket/momentum pass is authored for
// 30 Hz and does not sub-step cleanly. `obstacleCollisionPassHook` gates the
// whole pass — the `jal HandleHumanoidObstacleCollisions__FR6ccList` in
// `MoveThings__2AI` (`0x80055D80`, callee `0x800A96EC`, delay slot
// `move $a0, $s0`) — to the counted update, so it runs once per authored frame.
// By then the humanoid has completed its k sub-steps and meets each obstacle at
// its full-frame position, exactly as a retail frame: passenger membership,
// carry, ledge tickets, and jump-off momentum are all retail-correct, with no
// per-field velocity hacks. On the held updates the humanoid still moves a k-th
// (its world collision and motion are untouched); only its interaction with
// obstacles is evaluated at the authored rate. This replaces the per-site carry
// band-aids (`retimedPassengerHold`, `retimedObstacleDisembark`), which tried to
// patch the same coupling one instruction at a time. The exception is an active
// `AS_PushObject` humanoid: Pushable collision owns both the contact bit that
// keeps that state alive and the push displacement, so the hook calls that
// humanoid's inner collision handler on held updates. The gate is registered in
// `clock_hooks` (`obstacle_collision_pass`).

constexpr std::array<RetimeHook, 3U> ledge_hooks{{
    {"ledge_latch_velocity_reset",
     0x80033558U,
     0x80033568U,
     RetimeHookKind::semantic,
     &ledgeLatchVelocityResetHook},
    {"ladder_climb_up",
     0x80069F08U,
     0x80069F10U,
     RetimeHookKind::gate,
     &ladderClimbUpHook},
    {"ladder_slide_down",
     0x80069FC8U,
     0x80069FD0U,
     RetimeHookKind::recompute,
     &ladderSlideDownHook},
}};

// ---------------------------------------------------------------------------
// Authored-rate counter holds (humanoid/Boss state machine, Behaviour layer,
// HUD/menu layer)
//
// Every retail site in this family is a private per-frame counter that retail
// steps once per guest update — a state-timer, a decision/backoff countdown,
// or a pulse/slide counter — none of which reads the master clock. The two
// generic bodies below hold them on the published decision. Both decode their
// instructions from guest RAM, so the table carries no per-site arithmetic.

// Decodes a branch/jump word at `branch_pc` (normally a counter site's delay
// slot) and returns the resume PC: the branch target when the condition holds
// on `value` (unconditional for `j`/`jal`), otherwise the fallthrough
// `branch_pc + 8` (the instruction after the branch's own delay slot).
std::uint32_t branchResume(
    std::uint32_t branch_pc,
    std::uint32_t word,
    std::uint32_t value,
    std::uint32_t right) noexcept {
    const auto opcode = word >> 26U;
    if (opcode == 0x02U || opcode == 0x03U) { // j / jal
        return (branch_pc & 0xF0000000U) | ((word & 0x03FFFFFFU) << 2U);
    }
    const auto imm = static_cast<std::int32_t>(
        static_cast<std::int16_t>(word & 0xFFFFU));
    const auto target = branch_pc + 4U +
        static_cast<std::uint32_t>(imm << 2U);
    auto taken = false;
    switch (opcode) {
    case 0x07U: taken = static_cast<std::int32_t>(value) > 0; break; // bgtz
    case 0x06U: taken = static_cast<std::int32_t>(value) <= 0; break; // blez
    case 0x05U: taken = value != right; break; // bne
    case 0x04U: taken = value == right; break; // beq
    case 0x01U: // REGIMM: bltz / bgez
        taken = ((word >> 16U) & 0x1FU) == 0U
            ? static_cast<std::int32_t>(value) < 0
            : static_cast<std::int32_t>(value) >= 0;
        break;
    default: break;
    }
    return taken ? target : branch_pc + 8U;
}

[[nodiscard]] constexpr bool isBranchOpcode(std::uint32_t opcode) noexcept {
    return opcode == 0x01U || opcode == 0x02U || opcode == 0x03U ||
        (opcode >= 0x04U && opcode <= 0x07U);
}

// Shape 1: the site is `addiu $vR, $vR, ±1` (or `addiu $vR, $vSrc, ±1`), or a
// plain `sw $vR, off($base)` when the counter update runs in a preceding
// branch's delay slot (`_Pause` decrements in the guard's delay slot, so the
// store is the only safe site — a site that is itself a branch delay slot
// would lose the branch target, because the hook dispatch overwrites
// `next_pc`). The site's delay slot has already run when the hook fires, so
// on a held update the register and memory already hold the old value: the
// hook only writes the identity (`$vR = $vSrc`, a no-op for the common
// `rs == rt` form) so later stores and comparisons keep the counter frozen.
// On a counted update it models the addiu and repairs the two delay-slot
// forms that consumed the pre-step value: a `sw $vR, off($base)` must be
// re-issued with the new value, and a `slt[u] $vD, $x, $vR` must be
// recomputed with it. For a `sw` site a held update skips the store and a
// counted one issues it. Anything unrecognized falls back to retail.
//
// Two-deep form (BOL bosses): the addiu's delay slot is a branch and the
// counter store sits in the branch's own delay slot two words after the site
// (`_MissileAttack`'s `addiu; j; sw`, `_PaulDMS`'s `addiu; blez; sw`,
// `CounterAttack`'s `addiu; j; sw`). The virtual-branch machinery runs the
// branch (on the old value) then fires the hook with the branch state
// overwritten, so the hook decodes the branch target itself, performs the
// store (the branch's own delay-slot store never runs), and re-evaluates the
// branch on the new value. A held update resumes at the branch's decision on
// the old (held) value, so a countdown at its end keeps deferring its
// transition exactly as retail's next authored step would.
std::uint32_t counterStepHook(
    psx::R3000State& state,
    RetimeState& retime,
    psx::R3000Runtime& runtime,
    std::uint32_t rejoin, const void*) noexcept {
    const auto site = rejoin - 8U;
    std::uint32_t word = 0U;
    if (!runtime.read32(site, word)) {
        return rejoin;
    }
    const auto opcode = word >> 26U;
    std::uint32_t delay = 0U;
    const bool delay_is_branch = runtime.read32(site + 4U, delay) &&
        isBranchOpcode(delay >> 26U);
    if (opcode == 0x2BU) { // sw $rt, off($base): a store-site hold
        // A `j` in the delay slot (`henchman_engage_delay`) resumes at its
        // target rather than the plain rejoin.
        const auto resume = delay_is_branch
            ? branchResume(site + 4U, delay, state.gpr[(word >> 21U) & 0x1FU], 0U)
            : rejoin;
        if (!retime.hold()) {
            const auto base = (word >> 21U) & 0x1FU;
            const auto rt = (word >> 16U) & 0x1FU;
            const auto off = static_cast<std::int32_t>(
                static_cast<std::int16_t>(word & 0xFFFFU));
            static_cast<void>(runtime.write32(
                state.gpr[base] + static_cast<std::uint32_t>(off),
                state.gpr[rt]));
        }
        return resume;
    }
    if (opcode != 0x09U) {
        return rejoin; // not the addiu form: leave retail's flow alone
    }
    const auto rs = (word >> 21U) & 0x1FU;
    const auto rt = (word >> 16U) & 0x1FU;
    const auto delta = static_cast<std::int32_t>(
        static_cast<std::int16_t>(word & 0xFFFFU));
    if (delay_is_branch) {
        std::uint32_t store = 0U;
        if (!runtime.read32(site + 8U, store) ||
            (store >> 26U) != 0x2BU || ((store >> 16U) & 0x1FU) != rt) {
            return rejoin; // not the two-deep store form
        }
        const auto base = (store >> 21U) & 0x1FU;
        const auto off = static_cast<std::int32_t>(
            static_cast<std::int16_t>(store & 0xFFFFU));
        const auto rhs = (delay >> 16U) & 0x1FU; // branch's rt operand
        if (retime.hold()) {
            hostWriteRegister(state, rt, state.gpr[rs]);
            return branchResume(
                site + 4U, delay, state.gpr[rs], state.gpr[rhs]);
        }
        const auto new_value = static_cast<std::uint32_t>(
            static_cast<std::int32_t>(state.gpr[rs]) + delta);
        hostWriteRegister(state, rt, new_value);
        static_cast<void>(runtime.write32(
            state.gpr[base] + static_cast<std::uint32_t>(off), new_value));
        return branchResume(site + 4U, delay, new_value, state.gpr[rhs]);
    }
    if (retime.hold()) {
        // Identity: `$vR = $vSrc` (no-op when rs == rt). The delay slot's
        // store of the old value stands, and later comparisons read old.
        hostWriteRegister(state, rt, state.gpr[rs]);
        return rejoin;
    }
    const auto new_value = static_cast<std::uint32_t>(
        static_cast<std::int32_t>(state.gpr[rs]) + delta);
    hostWriteRegister(state, rt, new_value);
    if (runtime.read32(site + 4U, delay)) {
        const auto opcode = delay >> 26U;
        if (opcode == 0x2BU && ((delay >> 16U) & 0x1FU) == rt) {
            // sw $rt, off($base): the delay slot stored the old value.
            const auto base = (delay >> 21U) & 0x1FU;
            const auto off = static_cast<std::int32_t>(
                static_cast<std::int16_t>(delay & 0xFFFFU));
            static_cast<void>(runtime.write32(
                state.gpr[base] + static_cast<std::uint32_t>(off),
                new_value));
        } else if (opcode == 0U) {
            const auto funct = delay & 0x3FU;
            if (funct == 0x2AU || funct == 0x2BU) { // slt / sltu
                const auto rd = (delay >> 11U) & 0x1FU;
                const auto lhs = (delay >> 21U) & 0x1FU;
                const auto rhs = (delay >> 16U) & 0x1FU;
                if (lhs == rt || rhs == rt) {
                    const auto a = state.gpr[lhs];
                    const auto b = state.gpr[rhs];
                    const auto result = funct == 0x2AU
                        ? (static_cast<std::int32_t>(a) <
                                   static_cast<std::int32_t>(b)
                               ? 1U
                               : 0U)
                        : (a < b ? 1U : 0U);
                    hostWriteRegister(state, rd, result);
                }
            }
        }
    }
    return rejoin;
}

// Shape 2: the site is a guard branch (`bgtz`/`bltz`/`blez`/`bgez`) whose
// delay slot decrements the counter. The delay slot has already run when the
// hook fires (`$vR = old + delta`), so on a held update execution jumps to
// the epilogue — the shared return after both arms — leaving the countdown
// and its guarded side effects untouched. On a counted update the branch is
// modelled on the recovered pre-decrement value; the branch target's delay
// slot (the store) then runs normally.
std::uint32_t countdownGuardHook(
    psx::R3000State& state,
    RetimeState& retime,
    psx::R3000Runtime& runtime,
    std::uint32_t rejoin,
    const void* context) noexcept {
    const auto* guard = static_cast<const RetimeCountdownGuard*>(context);
    if (retime.hold()) {
        return guard->epilogue;
    }
    const auto site = rejoin - 8U;
    std::uint32_t word = 0U;
    if (!runtime.read32(site, word)) {
        return rejoin;
    }
    std::uint32_t delay = 0U;
    auto delta = 1;
    if (runtime.read32(site + 4U, delay) && (delay >> 26U) == 0x09U &&
        ((delay >> 16U) & 0x1FU) == ((delay >> 21U) & 0x1FU)) {
        delta = static_cast<std::int32_t>(
            static_cast<std::int16_t>(delay & 0xFFFFU));
    }
    const auto rs = (word >> 21U) & 0x1FU;
    const auto old = static_cast<std::int32_t>(state.gpr[rs]) - delta;
    const auto imm = static_cast<std::int32_t>(
        static_cast<std::int16_t>(word & 0xFFFFU));
    const auto target = site + 4U + static_cast<std::uint32_t>(imm << 2U);
    auto taken = false;
    switch (word >> 26U) {
    case 0x07U: // bgtz
        taken = old > 0;
        break;
    case 0x06U: // blez
        taken = old <= 0;
        break;
    case 0x01U: // REGIMM: bltz / bgez
        taken = ((word >> 16U) & 0x1FU) == 0U ? old < 0 : old >= 0;
        break;
    default:
        return rejoin;
    }
    return taken ? target : rejoin;
}

// A counter store that sits in an unconditional `j`'s delay slot
// (`_TargetMissileAttack`, `Dante+0x298`): the virtual-branch machinery runs
// the store (storing `new` — the increment already ran in an earlier branch's
// delay slot) and then fires the hook. A counted update leaves the store in
// place and resumes at the `j`'s target; a held one restores `old = new +
// delta` (`delta = -1` for the +1 counters) and resumes at the same target.
std::uint32_t jumpStoreUndoHook(
    psx::R3000State& state,
    RetimeState& retime,
    psx::R3000Runtime& runtime,
    std::uint32_t rejoin, const void* context) noexcept {
    const auto* undo = static_cast<const RetimeStoreUndo*>(context);
    std::uint32_t store = 0U;
    if (runtime.read32(undo->store_address, store) &&
        (store >> 26U) == 0x2BU) {
        if (retime.hold()) {
            const auto base = (store >> 21U) & 0x1FU;
            const auto rt = (store >> 16U) & 0x1FU;
            const auto off = static_cast<std::int32_t>(
                static_cast<std::int16_t>(store & 0xFFFFU));
            static_cast<void>(runtime.write32(
                state.gpr[base] + static_cast<std::uint32_t>(off),
                static_cast<std::uint32_t>(
                    static_cast<std::int32_t>(state.gpr[rt]) + undo->delta)));
        }
    }
    return rejoin;
}

// `Pushable::HandleHumanoidCollision` (OL1 `0x80018AF8`) applies the push
// displacement and keeps the push-contact bit alive. The obstacle pass runs
// it on every guest update for an active pusher (the `AS_PushObject`
// exception), so the displacement and the `+0xA4` engage counter would both
// advance twice per authored step at 60 Hz — a shoved vending machine moves
// at 2x. The gate sits on the engage branch (`0x80018EFC`, `bnez $v1,
// 0x80019178`, taken while the engage counter is below 5): the displacement
// block (which also re-issues the contact bit `Humanoid+0x170:2`) runs only
// on counted updates. On a held update the counter store in the branch's
// delay slot is undone, the contact bit is re-issued by the hook (the state
// stays latched, exactly as the pusher exception intends), and execution
// resumes at the skip target `0x80019178` (the `+0x9C` latch store).
// `$s1`/`$s2` are the pushable/humanoid (from the prologue's `move`).
std::uint32_t pushableCollisionHoldHook(
    psx::R3000State& state,
    RetimeState& retime,
    psx::R3000Runtime& runtime,
    std::uint32_t rejoin, const void*) noexcept {
    constexpr std::uint32_t skip_displacement = 0x80019178U;
    if (!retime.hold()) {
        // Model the gate branch: `$v1` = (old engage counter < 5), computed
        // by retail's `slti` at `0x80018EF8`.
        return state.gpr[3] != 0U ? skip_displacement : rejoin;
    }
    // The delay slot (`sw $v0, 0xa4($s1)`) already stored old + 1: undo it.
    std::uint32_t counter = 0U;
    static_cast<void>(runtime.read32(state.gpr[17] + 0xA4U, counter)); // $s1
    static_cast<void>(runtime.write32(state.gpr[17] + 0xA4U, counter - 1U));
    // Re-issue the push-contact bit the skipped block would have set.
    std::uint32_t flags = 0U;
    static_cast<void>(runtime.read32(state.gpr[18] + 0x170U, flags)); // $s2
    static_cast<void>(runtime.write32(state.gpr[18] + 0x170U, flags | 4U));
    return skip_displacement;
}

// Shape 1 sites, read out of the retail boot executable (`rejoin` is the
// instruction after the delay slot). The context is null: both the addiu and
// its delay slot are decoded from guest RAM.
inline constexpr std::array<RetimeHook, 11U> counter_step_hooks{{
    {"player_collapse_timer",
     0x80032EE0U, 0x80032EE8U,
     RetimeHookKind::recompute, &counterStepHook, nullptr},
    {"player_pole_swing_timer",
     0x800331B4U, 0x800331BCU,
     RetimeHookKind::recompute, &counterStepHook, nullptr},
    {"player_slope_slide_timer",
     0x800338B8U, 0x800338C0U,
     RetimeHookKind::recompute, &counterStepHook, nullptr},
    {"humanoid_collapse_timer",
     0x80068EDCU, 0x80068EE4U,
     RetimeHookKind::recompute, &counterStepHook, nullptr},
    {"humanoid_got_hit_free_form_timer",
     0x8006C3FCU, 0x8006C404U,
     RetimeHookKind::recompute, &counterStepHook, nullptr},
    {"humanoid_back_grab_timer",
     0x800683D4U, 0x800683DCU,
     RetimeHookKind::recompute, &counterStepHook, nullptr},
    {"humanoid_pause_timer",
     0x800672D8U, 0x800672E0U,
     RetimeHookKind::recompute, &counterStepHook, nullptr},
    {"behaviour_backoff_timer",
     0x80075924U, 0x8007592CU,
     RetimeHookKind::recompute, &counterStepHook, nullptr},
    {"behaviour_backout_timer",
     0x80075B2CU, 0x80075B34U,
     RetimeHookKind::recompute, &counterStepHook, nullptr},
    {"behaviour_complex_attack_index",
     0x80074A50U, 0x80074A58U,
     RetimeHookKind::recompute, &counterStepHook, nullptr},
    {"behaviour_navigate_enemies_timer",
     0x80076CB8U, 0x80076CC0U,
     RetimeHookKind::recompute, &counterStepHook, nullptr},
}};

// Shape 2 sites: the countdown guard plus its epilogue (the shared return
// after both arms of the guard).
inline constexpr RetimeCountdownGuard hud_ttlive_epilogue{0x8008EF34U};
inline constexpr RetimeCountdownGuard navigate_world_epilogue{0x80076C6CU};
inline constexpr std::array<RetimeHook, 2U> countdown_guard_hooks{{
    {"hud_ttlive_countdown",
     0x8008EF1CU, 0x8008EF24U,
     RetimeHookKind::gate, &countdownGuardHook, &hud_ttlive_epilogue},
    {"behaviour_navigate_world_hold",
     0x800768B0U, 0x800768B8U,
     RetimeHookKind::gate, &countdownGuardHook, &navigate_world_epilogue},
}};

// ---------------------------------------------------------------------------
// Overlay recompute / gate hooks (BOL/NBOL)

// `Think__5Arrow`'s overlay-local bob counter (`0x8001BDAC`). The site is
// `addiu $v0, $v0, 1`; its delay slot (`sw $v0, 0x74($s2)`) runs before the
// hook, storing the pre-increment counter, and the rejoin re-stores the
// retimed one. The counter advances once per authored frame, so the hook adds
// the clock's `!hold` (1 on a counted update, 0 on a held one) — reading the
// master decision rather than accumulating, so it stays exactly in step with
// `retimedGameClock` at any divisor. `$v1` is left as the increment, matching
// the MIPS body's `xori`; `$v0` gets the retimed sum.
std::uint32_t arrowBobHook(
    psx::R3000State& state,
    RetimeState& retime,
    psx::R3000Runtime& runtime,
    std::uint32_t rejoin, const void* context) noexcept {
    const auto increment = retime.hold() ? 0U : 1U;
    hostWriteRegister(state, 3, increment);                // $v1, as the xori
    hostWriteRegister(state, 2, state.gpr[2] + increment); // $v0 += increment
    return rejoin;
}

// `Platform::Move`'s path-speed load (`0x800227D0`). The site is
// `lw $a1, 0xb8($s1)`; its delay slot (`lw $v0, 0x1c($v0)`, the path fn pointer)
// runs before the hook, so the hook re-reads the speed from `0xb8($s1)` itself.
// The speed is divided per authored frame by exact telescoping, not rounding:
// each skipped update advances `floor(speed/k)` and the counted update takes the
// whole remainder, so the `k` advances sum to `speed` for any sign. `$a1` gets
// the per-update advance; `$v1` is left as the hold decision, matching the MIPS
// body's final `lw $v1, 0x3380`. The `±0xB8` delta clamp downstream never binds
// because the advance is now well under it.
std::uint32_t platformMoveSpeedHook(
    psx::R3000State& state,
    RetimeState& retime,
    psx::R3000Runtime& runtime,
    std::uint32_t rejoin, const void* context) noexcept {
    const auto divisor = retime.divisor == 0U ? 1U : retime.divisor;
    std::uint32_t speed_word = 0U;
    static_cast<void>(runtime.read32(state.gpr[17] + 0xB8U, speed_word)); // $s1
    const auto speed = static_cast<std::int32_t>(speed_word);
    const auto k = static_cast<std::int32_t>(divisor);
    auto quotient = speed / k;  // MIPS `div` truncates toward zero
    auto remainder = speed % k; // remainder carries the dividend's sign
    if (remainder < 0) {
        --quotient;     // truncation -> floor
        remainder += k; // remainder -> [0, k)
    }
    auto advance = quotient;
    if (!retime.hold()) {
        advance += remainder; // counted update takes the whole remainder
    }
    hostWriteRegister(state, 5, static_cast<std::uint32_t>(advance)); // $a1
    hostWriteRegister(state, 3, retime.hold() ? 1U : 0U);            // $v1
    return rejoin;
}

// `Think__5Stack`'s timeline dispatch (`0x8001EE44`). The site is
// `lw $a0, 0x88($s1)` (the tumble state); its delay slot (`lw $s0, 0x1c($v0)`)
// runs before the hook. `Stack` re-poses a display model every call, so a whole
// `Think` hold would rubberband it; instead this gate squashes the state to 0
// on a held update — routing the dispatch straight to the idempotent pose-apply
// and skipping `Wobble`/`Fall` (which own the fall counter and transitions) —
// unless the state is already 3 (finished), which still routes to the epilogue.
// A counted update leaves the real state. `$s1` is the stack.
std::uint32_t stackTimelineHook(
    psx::R3000State& state,
    RetimeState& retime,
    psx::R3000Runtime& runtime,
    std::uint32_t rejoin, const void* context) noexcept {
    std::uint32_t tumble_state = 0U;
    static_cast<void>(runtime.read32(state.gpr[17] + 0x88U, tumble_state)); // $s1
    if (retime.hold() && tumble_state != 3U) {
        tumble_state = 0U; // skip Wobble/Fall; pose-apply still runs
    }
    hostWriteRegister(state, 4, tumble_state); // $a0
    return rejoin;
}

// `Think__5Stack`'s tail sound step (`0x8001EEBC`). The site is
// `lw $a0, 0x9c($s1)` (the CKnockDownSound); its delay slot is a `nop`. The
// pose-apply must run every update, but its tail `Think__15CKnockDownSound`
// decrements per-frame countdowns, so on a held update the hook nulls the
// pointer and retail's own `beqz $a0` skips the sound `Think`, keeping the
// countdowns on the authored schedule. A counted update keeps the real pointer.
std::uint32_t stackSoundHook(
    psx::R3000State& state,
    RetimeState& retime,
    psx::R3000Runtime& runtime,
    std::uint32_t rejoin, const void* context) noexcept {
    std::uint32_t sound = 0U;
    static_cast<void>(runtime.read32(state.gpr[17] + 0x9CU, sound)); // $s1
    hostWriteRegister(state, 4, retime.hold() ? 0U : sound);         // $a0
    return rejoin;
}

// ---------------------------------------------------------------------------
// Platform divide-based conversion (NBOL)
//
// The forward path for `Think__8Platform` (see `docs/RECOMP_MIGRATION.md`): run
// `Think` every guest update (no reshape) and sub-step the rate-sensitive
// quantities inside it, so translation, rider carry, and — critically — the
// platform's tilt all advance a k-th per update in step with a sub-stepped
// humanoid. Verified against the Ghidra decompilation of `Think`/`Move`/
// `MovePassengers`/`Teeter`/`GetDeltaVelocity`. The reshape trampoline only
// sub-stepped translation (`Move`+`MovePassengers`) and held `Teeter`, the
// detached-gravity step, and the collision snapshot, which is why the teetering
// car-lift mantle loop was never fixed. These hooks divide the tilt, the
// falling gravity, and hold the frame counters/animation instead.
//
// Every quantity is identity at a divisor of one, so an unretimed table is
// retail. All thirteen sites are in the NBOL overlay alongside `Think`, so they
// share its fingerprint. `Move`'s path-speed divide is the separate
// `retimedPlatformMoveSpeed` recompute hook. `OnXorZRot` needs no hook: it
// recomputes the collision box from the current (sub-stepped) tilt angles every
// call, so it must run every update.

// `Teeter`'s two `torque / mass` divides (`0x80022DFC` -> `$v1`, `0x80022E88` ->
// `$a0`). The mass field `0x34` is read as the divisor of the angular-
// acceleration `div`; multiplying it by k divides the accel a k-th, so the
// persistent angular velocity `(0x12c)->0xc/0x14` gains a full frame's impulse
// over k updates. `context` is the GPR the load targets. `$s3` is the platform.
std::uint32_t platformTeeterMassHook(
    psx::R3000State& state,
    RetimeState& retime,
    psx::R3000Runtime& runtime,
    std::uint32_t rejoin, const void* context) noexcept {
    const auto divisor = retime.divisor == 0U ? 1U : retime.divisor;
    const auto reg = *static_cast<const std::uint32_t*>(context);
    std::uint32_t mass = 0U;
    static_cast<void>(runtime.read32(state.gpr[19] + 0x34U, mass)); // $s3
    hostWriteRegister(
        state, reg,
        static_cast<std::uint32_t>(
            static_cast<std::int32_t>(mass) * static_cast<std::int32_t>(divisor)));
    return rejoin;
}

// `Teeter`'s angle integration `angle += (0x12c)->angvel` (`0x80022F48` reads
// `0xc`, `0x80022FA4` reads `0x14`). The angular velocity is a persistent
// accumulator, so dividing the value added to the angle each update sub-steps
// the tilt. Both load through `$v0` and write `$v0`; `context` is the field
// offset.
std::uint32_t platformTeeterAngVelHook(
    psx::R3000State& state,
    RetimeState& retime,
    psx::R3000Runtime& runtime,
    std::uint32_t rejoin, const void* context) noexcept {
    const auto offset = *static_cast<const std::uint32_t*>(context);
    std::uint32_t angvel = 0U;
    static_cast<void>(runtime.read32(state.gpr[2] + offset, angvel)); // base $v0
    hostWriteRegister(
        state, 2,
        static_cast<std::uint32_t>(
            divideHalfToEven(static_cast<std::int32_t>(angvel), retime.divisor)));
    return rejoin;
}

// `Teeter`'s per-frame damping constant (`0x80022FFC`, `li $v1, 0x11eb`). It
// feeds the two `mult`s that damp the tilt angle; dividing it applies a k-th of
// a frame's damping per update (a first-order approximation of `(1-d)^(1/k)`,
// exact enough for the tilt to settle at the retail rate). The delay slot
// `subu $v0, $zero, $a1` has already run.
std::uint32_t platformTeeterDampingHook(
    psx::R3000State& state,
    RetimeState& retime,
    psx::R3000Runtime&,
    std::uint32_t rejoin, const void* context) noexcept {
    hostWriteRegister(
        state, 3, static_cast<std::uint32_t>(divideHalfToEven(0x11EB, retime.divisor)));
    return rejoin;
}

// The detached-platform fall is retail forward Euler: `pos.y += vel`, then
// `vel -= gravity*3/2`. Naively dividing both terms does not telescope, while
// holding them makes the platform drop a whole authored step at once relative
// to the smoothly sub-stepped player. The two hooks below instead partition
// the retail position delta and acceleration by phase: intermediate motion is
// smooth and every authored-frame endpoint stays bit-identical to retail.
// Signed floor(value * phase / divisor), used to partition one retail delta
// into exact sub-steps. The supported schedules cap the divisor at eight, so
// the signed 32-bit product is comfortably inside int64_t.
std::int32_t partitionFloor(
    std::int32_t value, std::uint32_t phase,
    std::uint32_t divisor) noexcept {
    const auto product = static_cast<std::int64_t>(value) * phase;
    const auto k = static_cast<std::int64_t>(divisor == 0U ? 1U : divisor);
    auto quotient = product / k;
    if (product % k < 0) {
        --quotient;
    }
    return static_cast<std::int32_t>(quotient);
}

// Phase 1..k of the current authored frame. `decide()` leaves clock_accum at
// a multiple of k on the counted update, which is phase k rather than zero.
std::uint32_t retimePhase(const RetimeState& retime) noexcept {
    const auto k = retime.divisor == 0U ? 1U : retime.divisor;
    const auto phase = retime.clock_accum % k;
    return phase == 0U ? k : phase;
}

std::uint32_t platformFallStepHook(
    psx::R3000State& state,
    RetimeState& retime,
    psx::R3000Runtime& runtime,
    std::uint32_t rejoin, const void* context) noexcept {
    const auto k = retime.divisor == 0U ? 1U : retime.divisor;
    const auto phase = retimePhase(retime);
    std::uint32_t velocity_word = 0U;
    static_cast<void>(
        runtime.read32(state.gpr[16] + 0xA4U, velocity_word)); // $s0

    // The site's delay slot has loaded $a1 = 0x800e0000. Retail loads the
    // `thePlayer` pointer from -0x294c($a1) at the rejoin, then reads gravity
    // at +0xc4. Read the same value without perturbing the guest registers.
    std::uint32_t player = 0U;
    std::uint32_t gravity_word = 0U;
    static_cast<void>(runtime.read32(state.gpr[5] - 0x294CU, player));
    static_cast<void>(runtime.read32(player + 0xC4U, gravity_word));
    const auto gravity = static_cast<std::int32_t>(gravity_word);
    const auto acceleration = static_cast<std::int32_t>(
        (static_cast<std::int64_t>(gravity) * 3) / 2);

    // The live velocity has already spent the earlier phases' acceleration.
    // Recover the retail frame's starting velocity, then emit this phase's
    // telescoping share. All k shares sum exactly to the retail position step.
    const auto spent_before = partitionFloor(acceleration, phase - 1U, k);
    const auto frame_velocity = static_cast<std::int32_t>(velocity_word) +
        spent_before;
    const auto step = partitionFloor(frame_velocity, phase, k) -
        partitionFloor(frame_velocity, phase - 1U, k);
    hostWriteRegister(state, 3, static_cast<std::uint32_t>(step));
    return rejoin;
}

// The fall's gravity accel (`0x80021C08`). The delay slot has left the full
// `gravity*3/2` in `$v0`; replace it with this phase's telescoping share. The
// following `subu` applies that share to `$v1`, the displaced live `0xa4` load.
std::uint32_t platformFallAccelHook(
    psx::R3000State& state,
    RetimeState& retime,
    psx::R3000Runtime& runtime,
    std::uint32_t rejoin, const void* context) noexcept {
    std::uint32_t velocity = 0U;
    static_cast<void>(runtime.read32(state.gpr[16] + 0xA4U, velocity)); // $s0
    hostWriteRegister(state, 3, velocity); // displaced lw $v1, 0xa4($s0)
    const auto k = retime.divisor == 0U ? 1U : retime.divisor;
    const auto phase = retimePhase(retime);
    const auto acceleration = static_cast<std::int32_t>(state.gpr[2]);
    const auto step = partitionFloor(acceleration, phase, k) -
        partitionFloor(acceleration, phase - 1U, k);
    hostWriteRegister(state, 2, static_cast<std::uint32_t>(step));
    return rejoin;
}

// A `Think` frame counter (`0x94`/`0x98`) whose `counter - 1` store is a plain
// (non-delay-slot) branch target, held to the counted update. Retail computes
// `counter - 1` in the branch's delay slot and stores it; the hook instead does
// the read-modify-write itself on a counted update and nothing on a held one,
// so the counter decrements once per authored frame. `context` is the field
// offset; `$s0` is the platform.
std::uint32_t platformCounterHoldHook(
    psx::R3000State& state,
    RetimeState& retime,
    psx::R3000Runtime& runtime,
    std::uint32_t rejoin, const void* context) noexcept {
    if (!retime.hold()) {
        const auto offset = *static_cast<const std::uint32_t*>(context);
        std::uint32_t counter = 0U;
        static_cast<void>(runtime.read32(state.gpr[16] + offset, counter));
        static_cast<void>(runtime.write32(state.gpr[16] + offset, counter - 1U));
    }
    return rejoin;
}

// The `0x98` move-delay counter, whose `counter - 1` is stored in a branch
// delay slot (`0x80021AB4`) that cannot be hooked. Instead the hook sits two
// instructions earlier (`0x80021AA8`, `lw $v1, 0x7c($s0)`): the delay-slot
// decrement has already left `$v1... $v0 = counter - 1`, so on a held update it
// restores `$v0` to the live counter (undoing the decrement) and the retail
// store then writes the counter back unchanged. It also models the displaced
// `lw $v1, 0x7c($s0)` the following `bne` reads. `$s0` is the platform.
std::uint32_t platformMoveDelayHoldHook(
    psx::R3000State& state,
    RetimeState& retime,
    psx::R3000Runtime& runtime,
    std::uint32_t rejoin, const void* context) noexcept {
    std::uint32_t restart = 0U;
    static_cast<void>(runtime.read32(state.gpr[16] + 0x7CU, restart)); // $s0
    hostWriteRegister(state, 3, restart); // displaced lw $v1, 0x7c($s0)
    if (retime.hold()) {
        std::uint32_t counter = 0U;
        static_cast<void>(runtime.read32(state.gpr[16] + 0x98U, counter));
        hostWriteRegister(state, 2, counter); // restore $v0, undo the decrement
    }
    return rejoin;
}

// `Platform::Move` copies the current Path position over the Platform each
// update. A bobbing Platform's Y is then offset by `Bob`, but that authored
// phase runs only on counted updates. On a held update, preserve the already
// bobbed Y captured at `0x14($sp)` instead of copying the Path's base Y from
// `0x24($sp)`. Also replace the saved Path Y so Move computes a zero Y delta;
// `MovePassengers` then carries only this sub-step's real path translation.
//
// The site displaces `lw $a3,0x20($sp)` and its real delay slot has already
// loaded the Path Y into `$t0`. The subsequent instructions load Z and store
// `$a3/$t0/$t1` into Platform position, so both registers must be correct.
std::uint32_t platformBobbingPathYHook(
    psx::R3000State& state,
    RetimeState& retime,
    psx::R3000Runtime& runtime,
    std::uint32_t rejoin, const void* context) noexcept {
    constexpr std::uint32_t bob_offset = 0x130U;
    constexpr std::uint32_t old_y_stack_offset = 0x14U;
    constexpr std::uint32_t path_x_stack_offset = 0x20U;
    constexpr std::uint32_t path_y_stack_offset = 0x24U;

    std::uint32_t path_x = 0U;
    static_cast<void>(runtime.read32(
        state.gpr[29] + path_x_stack_offset, path_x)); // $sp
    hostWriteRegister(state, 7, path_x);               // $a3, displaced load

    std::uint32_t bob = 0U;
    if (retime.hold() &&
        runtime.read32(state.gpr[17] + bob_offset, bob) && bob != 0U) { // $s1
        std::uint32_t old_y = 0U;
        if (runtime.read32(state.gpr[29] + old_y_stack_offset, old_y)) {
            static_cast<void>(runtime.write32(
                state.gpr[29] + path_y_stack_offset, old_y));
            hostWriteRegister(state, 8, old_y); // $t0, delay-slot result
        }
    }
    return rejoin;
}

// `Think`'s carried-velocity snapshot (`0x80021D74`, `sw t0, 0xac(s0)`, the
// first of `0xAC/0xB0/0xB4 = 0xA0/0xA4/0xA8`). `0xA0..0xA8` is the per-update
// path delta, already sub-stepped by `retimedPlatformMoveSpeed` so the position
// carry (`MovePassengers`, a direct position add) advances a k-th per update.
// But `0xAC..0xB4` is read by `GetDeltaVelocity` and handed to
// `DisembarkObstacle`, which stores it as the rider's `obstacleVelocity` — and
// `Move__12DynamicThing` applies that through the position step, which
// `retimedPositionStep` divides by k *again*. So a jumper carried by the divided
// snapshot keeps only ~1/k of the platform's speed. This hook writes the
// snapshot as `0xA0..0xA8 * k`, restoring the full per-authored-frame delta the
// carried velocity needs (identity at a divisor of one). `$s0` is the platform;
// the delay slot `sw t1, 0xb0(s0)` runs first (harmlessly overwritten), and the
// following `sw t2, 0xb4(s0)` is skipped because the hook writes all three.
std::uint32_t platformSnapshotHook(
    psx::R3000State& state,
    RetimeState& retime,
    psx::R3000Runtime& runtime,
    std::uint32_t rejoin, const void* context) noexcept {
    const auto divisor = retime.divisor == 0U ? 1U : retime.divisor;
    const auto platform = state.gpr[16]; // $s0
    const std::array<std::pair<std::uint32_t, std::uint32_t>, 3U> fields{{
        {0xA0U, 0xACU}, {0xA4U, 0xB0U}, {0xA8U, 0xB4U}}};
    for (const auto& [src, dst] : fields) {
        std::uint32_t delta = 0U;
        static_cast<void>(runtime.read32(platform + src, delta));
        static_cast<void>(runtime.write32(
            platform + dst,
            static_cast<std::uint32_t>(
                static_cast<std::int32_t>(delta) *
                static_cast<std::int32_t>(divisor))));
    }
    return rejoin;
}

// Per-site context for the shared Platform hooks.
inline constexpr std::uint32_t platform_teeter_mass_x_reg = 3U;  // $v1
inline constexpr std::uint32_t platform_teeter_mass_z_reg = 4U;  // $a0
inline constexpr std::uint32_t platform_teeter_angvel_x_off = 0x0CU;
inline constexpr std::uint32_t platform_teeter_angvel_z_off = 0x14U;
inline constexpr std::uint32_t platform_counter_94_field = 0x94U;
// `Bob` advances a bob-phase timer per call, so it is held to the counted
// update; `OnXorZRot` deliberately has no gate (it must run every update).
inline constexpr RetimeCallGate platform_bob_gate{0x80023190U};

} // namespace

std::span<const RetimeHook> retimeMotionHooks() noexcept {
    return motion_hooks;
}

std::span<const RetimeHook> retimeClockHooks() noexcept {
    return clock_hooks;
}

std::span<const RetimeHook> retimeObjectHooks() noexcept {
    // The always-live boot held prologues (the two world effects, `Ladder`,
    // `Untouchable`). The overlay held prologues live in `retimeOverlayHooks()`,
    // gated per loaded overlay. Both the prologues and this table have static
    // storage, so the context pointers stay valid for the program lifetime.
    static const std::vector<RetimeHook> hooks = [] {
        std::vector<RetimeHook> out;
        for (const auto& prologue : object_prologues) {
            if (prologue.site < overlay_prologue_limit) {
                continue; // overlay: in retimeOverlayHooks()
            }
            out.push_back(RetimeHook{
                prologue.name, prologue.site, prologue.rejoin,
                RetimeHookKind::gate, &heldPrologueHook, &prologue.prologue});
        }
        std::sort(
            out.begin(), out.end(),
            [](const RetimeHook& a, const RetimeHook& b) noexcept {
                return a.pc < b.pc;
            });
        return out;
    }();
    return hooks;
}

std::span<const RetimeHook> retimeLedgeHooks() noexcept {
    return ledge_hooks;
}

std::span<const RetimeHook> retimeCounterHooks() noexcept {
    static const std::vector<RetimeHook> hooks = [] {
        std::vector<RetimeHook> out;
        out.insert(out.end(), counter_step_hooks.begin(), counter_step_hooks.end());
        out.insert(
            out.end(), countdown_guard_hooks.begin(), countdown_guard_hooks.end());
        std::sort(
            out.begin(), out.end(),
            [](const RetimeHook& a, const RetimeHook& b) noexcept {
                return a.pc < b.pc;
            });
        return out;
    }();
    return hooks;
}

// Built once, lazily, so each overlay hook's fingerprint window can borrow the
// one already carried by its byte overlay trampoline — the single source of
// truth for the extracted overlay bytes until the trampolines are deleted. The
// storage is static, so the returned span stays valid for the program lifetime.
// It holds the recompute/gate hooks plus every overlay held prologue; the boot
// held prologues stay always-live in `retimeObjectHooks()`. Sorted by `pc` for
// `RetimeHooks`' indexed table and binary-search fallback;
// `buildActiveRetimeHooks` re-sorts the live subset regardless.
std::span<const RetimeOverlayHook> retimeOverlayHooks() noexcept {
    static const std::vector<RetimeOverlayHook> hooks = [] {
        std::vector<RetimeOverlayHook> out{
            {{"butch_stomp_counter",
              0x8001ADD4U,
              0x8001ADDCU,
              RetimeHookKind::recompute,
              &counterStepHook, nullptr},
             0x8001ADCCU,
             butch_stomp_window},
            {{"boss_collapse_timer",
              0x8001A9B8U,
              0x8001A9C0U,
              RetimeHookKind::recompute,
              &counterStepHook, nullptr},
             0x8001A9B0U,
             boss_collapse_window},
            {{"dante_missile_counter",
              0x8001C294U,
              0x8001C29CU,
              RetimeHookKind::recompute,
              &counterStepHook, nullptr},
             0x8001C288U,
             dante_missile_window},
            {{"dante_retarget_counter",
              0x8001C3D8U,
              0x8001C6E8U,
              RetimeHookKind::recompute,
              &jumpStoreUndoHook, &dante_retarget_undo},
             0x8001C3CCU,
             dante_retarget_window},
            {{"butch_dms_timer",
              0x8001CD3CU,
              0x8001CD44U,
              RetimeHookKind::recompute,
              &counterStepHook, nullptr},
             0x8001CD34U,
             butch_dms_window},
            {{"butch_dms_charge_timer",
              0x8001D20CU,
              0x8001D214U,
              RetimeHookKind::recompute,
              &counterStepHook, nullptr},
             0x8001D204U,
             butch_dms_charge_window},
            {{"paul_dms_recovery_timer",
              0x8001D948U,
              0x8001D950U,
              RetimeHookKind::recompute,
              &counterStepHook, nullptr},
             0x8001D93CU,
             paul_recovery_window},
            {{"paul_dms_attack_timer",
              0x8001DAD0U,
              0x8001DAD8U,
              RetimeHookKind::recompute,
              &counterStepHook, nullptr},
             0x8001DAC8U,
             paul_attack_window},
            {{"henchman_engage_delay",
              0x8001EA94U,
              0x8001EA9CU,
              RetimeHookKind::recompute,
              &counterStepHook, nullptr},
             0x8001EA74U,
             henchman_window},
            {{"counter_attack_hit_frames",
              0x8001EE38U,
              0x8001EE40U,
              RetimeHookKind::recompute,
              &counterStepHook, nullptr},
             0x8001EE30U,
             counter_attack_window},
            {{"pushable_collision_hold",
              0x80018EFCU,
              0x80018F04U,
              RetimeHookKind::semantic,
              &pushableCollisionHoldHook, nullptr},
             0x80018EE8U,
             pushable_collision_window},
            {{"arrow_bob",
              0x8001BDACU,
              0x8001BDB0U,
              RetimeHookKind::recompute,
              &arrowBobHook},
             0x8001BDA4U,
             arrow_bob_window},
            {{"stack_timeline",
              0x8001EE44U,
              0x8001EE4CU,
              RetimeHookKind::gate,
              &stackTimelineHook},
             0x8001EE28U,
             stack_timeline_window},
            {{"stack_sound",
              0x8001EEBCU,
              0x8001EEC4U,
              RetimeHookKind::gate,
              &stackSoundHook},
             0x8001EEA4U,
             stack_sound_window},
            {{"platform_move_speed",
              0x800227D0U,
              0x800227D8U,
              RetimeHookKind::recompute,
              &platformMoveSpeedHook},
             0x800227C8U,
             platform_move_speed_window},
        };
        // The overlay held prologues, gated by the same fingerprint as their
        // byte trampoline. Each carries a non-empty window (an empty one would
        // match unconditionally); the diff-gate test verifies that.
        for (const auto& prologue : object_prologues) {
            if (prologue.site >= overlay_prologue_limit) {
                continue; // boot: always live in retimeObjectHooks()
            }
            const auto [window_address, window] =
                overlayFingerprintFor(prologue.name);
            out.push_back(RetimeOverlayHook{
                {prologue.name, prologue.site, prologue.rejoin,
                 RetimeHookKind::gate, &heldPrologueHook, &prologue.prologue},
                window_address,
                window});
        }
        // The Platform divide-based conversion hooks. `Think` runs every update
        // (no reshape hook); these sub-step the tilt and the fall and hold the
        // frame counters/animation. All are in NBOL alongside `Think`, so they
        // share its fingerprint window.
        const auto think_window_address = 0x80021918U;
        const auto think_window =
            std::span<const std::uint32_t>{platform_think_window};
        const auto add_platform = [&](std::string_view name, std::uint32_t site,
                                      std::uint32_t rejoin, RetimeHookKind kind,
                                      RetimeHookFn fn, const void* context) {
            out.push_back(RetimeOverlayHook{
                {name, site, rejoin, kind, fn, context},
                think_window_address, think_window});
        };
        add_platform("platform_teeter_mass_x", 0x80022DFCU, 0x80022E04U,
                     RetimeHookKind::recompute, &platformTeeterMassHook,
                     &platform_teeter_mass_x_reg);
        add_platform("platform_teeter_mass_z", 0x80022E88U, 0x80022E90U,
                     RetimeHookKind::recompute, &platformTeeterMassHook,
                     &platform_teeter_mass_z_reg);
        add_platform("platform_teeter_angvel_x", 0x80022F48U, 0x80022F50U,
                     RetimeHookKind::recompute, &platformTeeterAngVelHook,
                     &platform_teeter_angvel_x_off);
        add_platform("platform_teeter_angvel_z", 0x80022FA4U, 0x80022FACU,
                     RetimeHookKind::recompute, &platformTeeterAngVelHook,
                     &platform_teeter_angvel_z_off);
        add_platform("platform_teeter_damping", 0x80022FFCU, 0x80023004U,
                     RetimeHookKind::recompute, &platformTeeterDampingHook,
                     nullptr);
        add_platform("platform_fall_step", 0x80021BDCU, 0x80021BE4U,
                     RetimeHookKind::recompute, &platformFallStepHook, nullptr);
        add_platform("platform_fall_accel", 0x80021C08U, 0x80021C10U,
                     RetimeHookKind::recompute, &platformFallAccelHook, nullptr);
        add_platform("platform_counter_94_restart", 0x80021A94U, 0x80021A9CU,
                     RetimeHookKind::gate, &platformCounterHoldHook,
                     &platform_counter_94_field);
        add_platform("platform_counter_94_idle", 0x80021D38U, 0x80021D40U,
                     RetimeHookKind::gate, &platformCounterHoldHook,
                     &platform_counter_94_field);
        add_platform("platform_counter_98", 0x80021AA8U, 0x80021AB0U,
                     RetimeHookKind::gate, &platformMoveDelayHoldHook, nullptr);
        add_platform("platform_bob", 0x80021DA8U, 0x80021DB0U,
                     RetimeHookKind::gate, &callGateHook, &platform_bob_gate);
        add_platform("platform_bob_path_y", 0x80022814U, 0x8002281CU,
                     RetimeHookKind::semantic, &platformBobbingPathYHook,
                     nullptr);
        add_platform("platform_snapshot", 0x80021D74U, 0x80021D80U,
                     RetimeHookKind::recompute, &platformSnapshotHook, nullptr);
        std::sort(
            out.begin(), out.end(),
            [](const RetimeOverlayHook& a, const RetimeOverlayHook& b) noexcept {
                return a.hook.pc < b.hook.pc;
            });
        return out;
    }();
    return hooks;
}

bool retimeOverlayHookMatches(
    psx::R3000Runtime& runtime, const RetimeOverlayHook& overlay) noexcept {
    for (std::size_t index = 0U; index < overlay.window.size(); ++index) {
        const auto address = overlay.window_address +
            static_cast<std::uint32_t>(index * sizeof(std::uint32_t));
        std::uint32_t word = 0U;
        if (!runtime.read32(address, word)) {
            return false; // an absent overlay is expected, not an error
        }
        if (word != overlay.window[index]) {
            return false;
        }
    }
    return true;
}

std::size_t buildActiveRetimeHooks(
    psx::R3000Runtime& runtime,
    std::span<const RetimeHook> boot,
    std::span<const RetimeOverlayHook> overlay,
    std::span<RetimeHook> out) noexcept {
    std::size_t count = 0U;
    for (const auto& hook : boot) {
        if (count < out.size()) {
            out[count] = hook;
        }
        ++count;
    }
    for (const auto& candidate : overlay) {
        if (retimeOverlayHookMatches(runtime, candidate)) {
            if (count < out.size()) {
                out[count] = candidate.hook;
            }
            ++count;
        }
    }
    if (count <= out.size()) {
        std::sort(
            out.begin(),
            out.begin() + static_cast<std::ptrdiff_t>(count),
            [](const RetimeHook& a, const RetimeHook& b) noexcept {
                return a.pc < b.pc;
            });
    }
    return count;
}

} // namespace stuntmaster::game
