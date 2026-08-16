#include "stuntmaster/core/bounded_latest_mailbox.hpp"
#include "stuntmaster/core/audio_ring.hpp"
#include "stuntmaster/core/error.hpp"
#include "stuntmaster/core/sha256.hpp"
#include "stuntmaster/core/state_archive.hpp"
#include "stuntmaster/game/free_camera.hpp"
#include "stuntmaster/game/guest_schedule.hpp"
#include "stuntmaster/game/mouse_control.hpp"
#include "stuntmaster/game/retail_hle.hpp"
#include "stuntmaster/game/retail_patch.hpp"
#include "stuntmaster/game/retiming.hpp"
#include "stuntmaster/game/supported_game.hpp"
#include "stuntmaster/presentation/movie_player.hpp"
#include "stuntmaster/presentation/gpu_publication.hpp"
#include "stuntmaster/presentation/debug_overlay.hpp"
#include "stuntmaster/presentation/display_scaling.hpp"
#include "stuntmaster/presentation/license_overlay.hpp"
#include "stuntmaster/presentation/widescreen_overlay.hpp"
#include "stuntmaster/psx/bios_hle.hpp"
#include "stuntmaster/psx/executable.hpp"
#include "stuntmaster/psx/gpu_command_bridge.hpp"
#include "stuntmaster/psx/gpu_command_decoder.hpp"
#include "stuntmaster/psx/gpu_polygon_limits.hpp"
#include "stuntmaster/psx/spu.hpp"
#include "stuntmaster/psx/r3000_runtime.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

void mouseControlDualHeadingIsBoundedAndReversible() {
    using Runtime = stuntmaster::psx::R3000Runtime;
    using stuntmaster::game::MouseButton;
    using stuntmaster::game::MouseHeadingSplitKind;
    using stuntmaster::game::MouseMovementMode;

    const std::array<bool, 8U> punch_and_kick{
        false, false, false, false, true, false, false, true};
    const std::array<std::uint8_t, 8U> identity{0, 1, 2, 3, 4, 5, 6, 7};
    assert(stuntmaster::game::applySemanticMouseActions(
               0xFFFFU, punch_and_kick, identity) == 0x6FFFU);
    assert(stuntmaster::game::parseMouseButton("LEFT") == MouseButton::left);
    assert(stuntmaster::game::parseMouseMovementMode("camera_relative") ==
           MouseMovementMode::camera_relative);
    assert(!stuntmaster::game::parseMouseMovementMode("character_relative"));

    stuntmaster::game::MouseControlConfig config;
    assert(config.maximum_turn_rate == 720U);
    assert(config.turn_acceleration == 10'000U);
    config.maximum_turn_rate = 360U;
    config.turn_acceleration = 3600U;
    stuntmaster::game::MouseYawController yaw{config};
    stuntmaster::game::MouseGameplayContext context{};
    context.player_yaw = 0U;
    context.camera_yaw = 0U;
    context.action_state = 1U;
    yaw.update(context, 0, 0, 60U);
    assert(yaw.accepted() && yaw.desiredYaw() == 0U);
    yaw.update(context, 10, 0, 60U);
    assert(yaw.desiredYaw() > 0U && yaw.desiredYaw() < 0x4000U);
    const auto first_step = yaw.desiredYaw();
    assert(first_step <= 1'100U); // bounded to roughly six degrees
    for (int frame = 0; frame < 120; ++frame) yaw.update(context, 0, 0, 60U);
    assert(std::abs(static_cast<int>(yaw.desiredYaw()) - 0x4000) <= 1);

    // The queue bound must not make a valid absolute target unreachable.
    // Camera-back is the furthest possible shortest-arc request.
    stuntmaster::game::MouseYawController backward_yaw{config};
    context.player_yaw = 0U;
    backward_yaw.update(context, 0, 0, 60U);
    backward_yaw.update(context, 0, 10, 60U);
    for (int frame = 0; frame < 120; ++frame)
        backward_yaw.update(context, 0, 0, 60U);
    assert(backward_yaw.desiredYaw() == 0x8000U);

    // 0xffff and 0x0000 are adjacent headings. The controller must take that
    // short arc instead of reversing almost a complete turn at atan2's wrap.
    stuntmaster::game::MouseYawController wrapped_yaw{config};
    context.player_yaw = 0xFFF0U;
    context.action_state = 1U;
    wrapped_yaw.update(context, 0, 0, 60U); // synchronize/lease entry
    wrapped_yaw.update(context, 0, -10, 60U); // camera-forward is zero
    const auto wrapped_step = static_cast<std::int16_t>(
        wrapped_yaw.desiredYaw() - 0xFFF0U);
    assert(wrapped_step > 0 && wrapped_step <= 16);
    for (int frame = 0; frame < 30; ++frame)
        wrapped_yaw.update(context, 0, 0, 60U);
    assert(wrapped_yaw.desiredYaw() == 0U);

    // A gesture rotating much faster than Jackie can turn must keep its
    // direction after lapping him; it may not take the newly-shorter reverse
    // arc. The half-turn lead cap also keeps the queued rotation finite while
    // preserving every possible absolute target direction.
    config.maximum_turn_rate = 90U;
    config.turn_acceleration = 10'000U;
    stuntmaster::game::MouseYawController lapped_yaw{config};
    context.player_yaw = 0U;
    lapped_yaw.update(context, 0, 0, 60U);
    const std::array<std::pair<int, int>, 9U> fast_clockwise{{
        {0, -10}, {10, 0}, {0, 10}, {-10, 0}, {0, -10},
        {10, 0}, {0, 10}, {-10, 0}, {0, -10},
    }};
    auto previous_lapped_yaw = lapped_yaw.desiredYaw();
    for (const auto [x, y] : fast_clockwise) {
        lapped_yaw.update(context, x, y, 60U);
        const auto step = static_cast<std::int16_t>(
            lapped_yaw.desiredYaw() - previous_lapped_yaw);
        assert(step >= 0 && step <= 384);
        previous_lapped_yaw = lapped_yaw.desiredYaw();
    }

    context.action_state = 6U;
    const auto airborne_entry_yaw = yaw.desiredYaw();
    yaw.update(context, -100, 0, 60U);
    assert(yaw.desiredYaw() != airborne_entry_yaw); // jump retains lease
    context.action_state = 32U;
    const auto context_entry_yaw = yaw.desiredYaw();
    yaw.update(context, -100, 0, 60U);
    assert(yaw.desiredYaw() == context_entry_yaw); // context discards gesture
    context.player_yaw = 0x2222U;
    context.action_state = 10U;
    yaw.update(context, -100, 0, 60U);
    assert(yaw.desiredYaw() == 0x2222U); // re-entry reseeds and discards
    yaw.update(context, -100, 0, 60U);
    assert(yaw.desiredYaw() != 0x2222U);
    assert(yaw.cycleMode() == MouseMovementMode::off);
    assert(!yaw.accepted());
    assert(yaw.cycleMode() == MouseMovementMode::camera_relative);

    context.player_yaw = 0x1000U;
    context.travel_yaw = 0x2000U;
    context.action_state = 10U;
    auto diagnostic = stuntmaster::game::mouseHeadingDiagnostic(context);
    assert(diagnostic.kind == MouseHeadingSplitKind::free_lease &&
           diagnostic.signed_delta == -0x1000);
    context.action_state = 13U;
    assert(stuntmaster::game::mouseHeadingDiagnostic(context).kind ==
           MouseHeadingSplitKind::free_lease);
    context.action_state = 32U;
    diagnostic = stuntmaster::game::mouseHeadingDiagnostic(context);
    assert(diagnostic.kind == MouseHeadingSplitKind::context);
    context.travel_yaw = context.player_yaw;
    assert(!stuntmaster::game::mouseHeadingDiagnostic(context).split());

    Runtime runtime;
    constexpr std::uint32_t game = 0x80110000U;
    constexpr std::uint32_t player = 0x80120000U;
    constexpr std::uint32_t behaviour = 0x80121000U;
    constexpr std::uint32_t input = 0x80130000U;
    constexpr std::uint32_t camera = 0x80140000U;
    assert(runtime.write32(0x800DD668U, game));
    assert(runtime.write32(game + 0x1CU, 0x80029C6CU));
    assert(runtime.write32(0x800DD6B4U, player));
    assert(runtime.write32(player + 0x1B8U, behaviour));
    assert(runtime.write32(behaviour + 0xE0U, 0x80074F5CU));
    assert(runtime.write32(player + 0x2CU, 0x1234U));
    assert(runtime.write32(player + 0x114U, 0x5678U));
    assert(runtime.write32(player + 0x164U, 10U));
    assert(runtime.write32(0x800DD734U, camera));
    assert(runtime.write32(camera + 0x2CU, 0x8765U));
    assert(runtime.write32(0x800DD69CU, input));
    for (std::size_t index = 0U; index < identity.size(); ++index)
        assert(runtime.write8(input + 0x2E0U + static_cast<std::uint32_t>(index),
                              identity[index]));
    const auto live = stuntmaster::game::readMouseGameplayContext(runtime);
    assert(live && live->player_yaw == 0x1234U &&
           live->travel_yaw == 0x5678U && live->action_state == 10U &&
           live->camera_yaw == 0x8765U);

    const std::array<std::pair<std::uint32_t, std::array<std::uint32_t, 9U>>, 12U>
        windows{{
            {0x800303D8U, {0xAFB40038U, 0x00A0A021U, 0xAFB30034U,
                           0x00C09821U, 0xAFBF0040U, 0xAFB5003CU,
                           0xAFB20030U, 0xAFB00028U, 0x8E220058U}},
            {0x800309B0U, {0x00C03821U, 0x8E2202C8U, 0x02202021U,
                           0x8C450000U, 0x0C0186D1U, 0x26260028U,
                           0x8E2302B8U, 0x240205DCU, 0xA6220156U}},
            {0x80031534U, {0x00822023U, 0x8E03002CU, 0x8E020114U,
                           0U, 0x14430024U, 0U, 0x8E020268U, 0U,
                           0x00401821U}},
            {0x800319A8U, {0x00003021U, 0x0800C67DU, 0U,
                           0x8F850D6CU, 0x0C0193ACU, 0x24060001U,
                           0x8E040050U, 0U, 0x8C820020U}},
            {0x80031B40U, {0xAFA20014U, 0x8E250114U, 0x96300156U,
                           0x24021388U, 0x0C0193ACU, 0xA6220156U,
                           0x02202021U, 0x8E2500D4U, 0x27A60010U}},
            {0x80031BE4U, {0x02202021U, 0x02202021U, 0x8E250114U,
                           0x24060001U, 0x0C0193ACU, 0U, 0x02202021U,
                           0x02002821U, 0x27A60020U}},
            {0x80032060U, {0U, 0x1040009BU, 0x02002021U,
                           0x8E050114U, 0x0C0193ACU, 0x24060001U,
                           0x860202C0U, 0U, 0x1040000FU}},
            {0x800321ACU, {0xAFA80018U, 0xAFA9001CU, 0xAFAA0020U,
                           0x8E050114U, 0x0C0193ACU, 0x24060001U,
                           0xAFA00020U, 0xAFA00018U, 0x8E020114U}},
            {0x800323D8U, {0xAFA70010U, 0xAFA80014U, 0xAFA90018U,
                           0x8E250114U, 0x0C0193ACU, 0x24060001U,
                           0x3C10800DU, 0x02202021U, 0x8E056550U}},
            {0x80032718U, {0x24060001U, 0xAFA00010U, 0x8C420014U,
                           0U, 0x0040F809U, 0x00003821U,
                           0x0800CA8DU, 0U, 0x8E020058U}},
            {0x8003291CU, {0x00003021U, 0x0800CA8DU, 0U,
                           0x8E050114U, 0x0C0193ACU, 0x24060001U,
                           0x8E020058U, 0U, 0x00021302U}},
            {0x80075174U, {0x00002821U, 0x8E620018U, 0U,
                           0x8C480028U, 0x8C49002CU, 0x8C4A0030U,
                           0xAFA80018U, 0xAFA9001CU, 0xAFAA0020U}},
        }};
    for (const auto& [site, words] : windows)
        assert(runtime.loadBytes(site - 16U, std::as_bytes(std::span{words})));
    assert(stuntmaster::game::setMouseControlPatches(runtime, true));
    assert(stuntmaster::game::setMouseControlPatches(runtime, true));
    const std::array expected_sites{
        std::pair{0x800303D8U, 0x08001200U},
        std::pair{0x800309B0U, 0x08001240U},
        std::pair{0x80031534U, 0x08001280U},
        std::pair{0x800319A8U, 0x080012C0U},
        std::pair{0x80031B40U, 0x08001390U},
        std::pair{0x80031BE4U, 0x080013A8U},
        std::pair{0x80032060U, 0x080013C0U},
        std::pair{0x800321ACU, 0x080013D8U},
        std::pair{0x800323D8U, 0x08001440U},
        std::pair{0x80032718U, 0x08001300U},
        std::pair{0x8003291CU, 0x08001340U},
        std::pair{0x80075174U, 0x08001400U},
    };
    std::uint32_t value = 0U;
    for (const auto& [site, jump] : expected_sites) {
        assert(runtime.read32(site, value) && value == jump);
    }

    const auto runUntil = [&](std::uint32_t start, std::uint32_t stop,
                              int budget = 512) {
        assert(runtime.state().pc == start);
        for (int executed = 0; executed < budget && runtime.state().pc != stop;
             ++executed) {
            const auto step = runtime.step();
            assert(step.reason == stuntmaster::psx::R3000StopReason::running);
        }
        assert(runtime.state().pc == stop);
        runtime.settleLoadDelay();
    };

    assert(runtime.write8(0x800DFA38U, 1U));
    const auto verifyOwnership = [&](std::uint32_t state, bool travel_commit,
                                     bool aim_commit) {
        assert(runtime.write32(player + 0x114U, 0x3456U));
        assert(runtime.write32(player + 0x2CU, 0x2222U));
        runtime.reset(0x800303D8U, 0U, 0x801F0000U);
        runtime.setRegister(17, player); // s1: Player
        runtime.setRegister(20, state);  // s4: new action state
        runUntil(0x800303D8U, 0x800303E0U);
        assert(runtime.read32(player + 0x2CU, value) &&
               value == (travel_commit ? 0x3456U : 0x2222U));
        assert(runtime.read32(player + 0x114U, value) &&
               value == (aim_commit ? 0x2222U : 0x3456U));
    };
    for (const auto state : {12U, 20U, 21U, 30U}) {
        verifyOwnership(state, true, false);
    }
    for (const auto state : {19U, 22U, 32U, 33U, 34U, 35U, 36U, 37U,
                             38U, 39U, 40U, 41U, 42U, 43U, 44U, 45U}) {
        verifyOwnership(state, false, true);
    }
    for (const auto state : {0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U, 10U,
                             11U, 13U, 14U, 15U, 16U, 17U, 18U, 23U, 31U,
                             46U, 63U}) {
        verifyOwnership(state, false, false);
    }

    // Running-jump launch force uses travel yaw without changing official
    // body yaw. Off mode retains retail's original orientation vector.
    const std::array<std::uint32_t, 4U> capture_force_stub{
        0x8CC30004U, 0U, 0x03E00008U, 0U}; // lw v1,4(a2); nop; jr ra; nop
    assert(runtime.loadBytes(
        0x80061B44U, std::as_bytes(std::span{capture_force_stub})));
    assert(runtime.write32(player + 0x2CU, 0x2222U));
    assert(runtime.write32(player + 0x114U, 0x3456U));
    assert(runtime.write8(0x800DFA38U, 1U));
    runtime.reset(0x800309B0U, 0U, 0x801F0000U);
    runtime.setRegister(17, player); // s1
    runtime.setRegister(20, 6U);     // s4: running jump
    runtime.setRegister(3, 0U);
    runUntil(0x800309B0U, 0x800309B8U);
    assert(runtime.state().gpr[3] == 0x3456U);
    assert(runtime.read32(player + 0x2CU, value) && value == 0x2222U);
    assert(runtime.write8(0x800DFA38U, 0U));
    runtime.reset(0x800309B0U, 0U, 0x801F0000U);
    runtime.setRegister(17, player);
    runtime.setRegister(20, 6U);
    runtime.setRegister(3, 0U);
    runUntil(0x800309B0U, 0x800309B8U);
    assert(runtime.state().gpr[3] == 0x2222U);

    assert(runtime.write8(0x800DFA38U, 1U));
    assert(runtime.write32(player + 0x164U, 1U));
    assert(runtime.write32(0x800DFA34U, 0x4567U));
    runtime.reset(0x80075174U, 0U, 0x801F0000U);
    runtime.setRegister(2, player); // v0
    runtime.setRegister(28, 0x800DC94CU); // gp
    runUntil(0x80075174U, 0x8007517CU);
    assert(runtime.read32(player + 0x2CU, value) && value == 0x4567U);
    assert(runtime.state().gpr[9] == 0x4567U); // t1 snapshot used by decoder
    assert(runtime.write32(player + 0x164U, 6U));
    assert(runtime.write32(0x800DFA34U, 0x5A5AU));
    runtime.reset(0x80075174U, 0U, 0x801F0000U);
    runtime.setRegister(2, player);
    runtime.setRegister(28, 0x800DC94CU);
    runUntil(0x80075174U, 0x8007517CU);
    assert(runtime.read32(player + 0x2CU, value) && value == 0x5A5AU);
    assert(runtime.state().gpr[9] == 0x5A5AU);
    assert(runtime.write32(player + 0x164U, 32U));
    assert(runtime.write32(player + 0x2CU, 0x7777U));
    assert(runtime.write32(0x800DFA34U, 0x9999U));
    runtime.reset(0x80075174U, 0U, 0x801F0000U);
    runtime.setRegister(2, player);
    runtime.setRegister(28, 0x800DC94CU);
    runUntil(0x80075174U, 0x8007517CU);
    assert(runtime.read32(player + 0x2CU, value) && value == 0x7777U);

    // Disabling in a free lease rejoins stock with body normalized to travel;
    // disabling during a context leaves the context-owned body untouched.
    assert(runtime.write32(player + 0x164U, 1U));
    assert(runtime.write32(player + 0x114U, 0x2468U));
    assert(runtime.write8(0x800DFA38U, 0U));
    runtime.reset(0x80075174U, 0U, 0x801F0000U);
    runtime.setRegister(2, player);
    runtime.setRegister(28, 0x800DC94CU);
    runUntil(0x80075174U, 0x8007517CU);
    assert(runtime.read32(player + 0x2CU, value) && value == 0x2468U);

    // Stand uses retail's three-update ramp despite a deliberate split and
    // clears any stale turn-around latch/timer. Off mode preserves the stock
    // equality branch.
    assert(runtime.write8(0x800DFA38U, 1U));
    assert(runtime.write32(0x800DCB08U, 1U)); // gp+0x1bc latch
    assert(runtime.write16(0x800DD6BCU, 7U)); // gp+0xd70 timer
    runtime.reset(0x80031534U, 0U, 0x801F0000U);
    runtime.setRegister(2, 0x1111U);
    runtime.setRegister(3, 0x2222U);
    runtime.setRegister(28, 0x800DC94CU);
    runUntil(0x80031534U, 0x8003153CU);
    assert(runtime.read32(0x800DCB08U, value) && value == 0U);
    std::uint16_t half = 0U;
    assert(runtime.read16(0x800DD6BCU, half) && half == 0U);
    assert(runtime.write8(0x800DFA38U, 0U));
    runtime.reset(0x80031534U, 0U, 0x801F0000U);
    runtime.setRegister(2, 0x1111U);
    runtime.setRegister(3, 0x1111U);
    runUntil(0x80031534U, 0x8003153CU);

    // Stand and Run suppress only their final retail FaceAngleY call. A tiny
    // sentinel stub makes call-vs-suppress directly observable.
    const std::array<std::uint32_t, 3U> face_stub{
        0x24031234U, 0x03E00008U, 0U}; // addiu v1,zero,1234; jr ra; nop
    assert(runtime.loadBytes(
        0x80064EB0U, std::as_bytes(std::span{face_stub})));
    assert(runtime.write8(0x800DFA38U, 1U));
    assert(runtime.write32(player + 0x164U, 1U));
    runtime.reset(0x800319A8U, 0U, 0x801F0000U);
    runtime.setRegister(4, player);
    runtime.setRegister(3, 0U);
    runUntil(0x800319A8U, 0x800319B0U);
    assert(runtime.state().gpr[3] == 0U);
    assert(runtime.write32(player + 0x164U, 32U));
    runtime.reset(0x800319A8U, 0U, 0x801F0000U);
    runtime.setRegister(4, player);
    runtime.setRegister(3, 0U);
    runUntil(0x800319A8U, 0x800319B0U);
    assert(runtime.state().gpr[3] == 0x1234U);
    assert(runtime.write8(0x800DFA38U, 0U));
    assert(runtime.write32(player + 0x164U, 1U));
    runtime.reset(0x800319A8U, 0U, 0x801F0000U);
    runtime.setRegister(4, player);
    runtime.setRegister(3, 0U);
    runUntil(0x800319A8U, 0x800319B0U);
    assert(runtime.state().gpr[3] == 0x1234U);

    // Airborne lease seams suppress retail's forced FaceAngleY calls only in
    // the matching jump/fall/flip state. A transition to combat still runs
    // the exact retail call.
    struct AirFaceCase {
        std::uint32_t site;
        std::uint32_t stop;
        std::uint32_t player_register;
        std::uint32_t leased_state;
    };
    const std::array air_face_cases{
        AirFaceCase{0x80031B40U, 0x80031B48U, 17U, 16U},
        AirFaceCase{0x80031BE4U, 0x80031BECU, 17U, 17U},
        AirFaceCase{0x80032060U, 0x80032068U, 16U, 6U},
        AirFaceCase{0x800321ACU, 0x800321B4U, 16U, 8U},
        AirFaceCase{0x800323D8U, 0x800323E0U, 17U, 13U},
    };
    assert(runtime.write8(0x800DFA38U, 1U));
    for (const auto& air : air_face_cases) {
        assert(runtime.write32(player + 0x164U, air.leased_state));
        runtime.reset(air.site, 0U, 0x801F0000U);
        runtime.setRegister(air.player_register, player);
        runtime.setRegister(4, player);
        runtime.setRegister(3, 0U);
        runUntil(air.site, air.stop);
        assert(runtime.state().gpr[3] == 0U);

        assert(runtime.write32(player + 0x164U, 32U));
        runtime.reset(air.site, 0U, 0x801F0000U);
        runtime.setRegister(air.player_register, player);
        runtime.setRegister(4, player);
        runtime.setRegister(3, 0U);
        runUntil(air.site, air.stop);
        assert(runtime.state().gpr[3] == 0x1234U);
    }

    // Run's stop transition chooses authored directional idle 22 under the
    // lease while retaining the original virtual call and off-mode arguments.
    constexpr std::uint32_t indirect_stub_address = 0x80010000U;
    const std::array<std::uint32_t, 2U> return_stub{0x03E00008U, 0U};
    assert(runtime.loadBytes(
        indirect_stub_address, std::as_bytes(std::span{return_stub})));
    assert(runtime.write8(0x800DFA38U, 1U));
    runtime.reset(0x80032718U, 0U, 0x801F0000U);
    runtime.setRegister(2, indirect_stub_address);
    runtime.setRegister(5, 46U);
    runtime.setRegister(6, 1U);
    runUntil(0x80032718U, 0x80032720U);
    assert(runtime.state().gpr[5] == 22U && runtime.state().gpr[6] == 0U);
    assert(runtime.write8(0x800DFA38U, 0U));
    runtime.reset(0x80032718U, 0U, 0x801F0000U);
    runtime.setRegister(2, indirect_stub_address);
    runtime.setRegister(5, 46U);
    runtime.setRegister(6, 1U);
    runUntil(0x80032718U, 0x80032720U);
    assert(runtime.state().gpr[5] == 46U && runtime.state().gpr[6] == 1U);

    // A context claimed during Run still executes the original final facing
    // call; suppression is leased only while the final state remains Run.
    assert(runtime.write8(0x800DFA38U, 1U));
    assert(runtime.write32(player + 0x164U, 32U));
    runtime.reset(0x8003291CU, 0U, 0x801F0000U);
    runtime.setRegister(16, player);
    runtime.setRegister(4, player);
    runtime.setRegister(3, 0U);
    runUntil(0x8003291CU, 0x80032924U);
    assert(runtime.state().gpr[3] == 0x1234U);

    // The Run hook stays in AS_RUN but selects the exact four retail Strafe
    // animation sectors and playback directions.
    constexpr std::uint32_t model = 0x80150000U;
    constexpr std::uint32_t model_vtable = 0x80151000U;
    constexpr std::uint32_t animation = 0x80152000U;
    assert(runtime.write32(player + 0x50U, model));
    assert(runtime.write32(model + 0x08U, model_vtable));
    assert(runtime.write32(model + 0x20U, animation));
    assert(runtime.write32(model_vtable + 0x14U, indirect_stub_address));
    const std::array<std::uint32_t, 3U> capture_anim_stub{
        0xAD6557F8U, 0x03E00008U, 0U}; // sw a1,57f8(t3); jr ra; nop
    const std::array<std::uint32_t, 3U> capture_loop_stub{
        0xAD6557FCU, 0x03E00008U, 0U}; // sw a1,57fc(t3); jr ra; nop
    assert(runtime.loadBytes(indirect_stub_address,
                             std::as_bytes(std::span{capture_anim_stub})));
    assert(runtime.loadBytes(0x80070C20U,
                             std::as_bytes(std::span{capture_loop_stub})));
    assert(runtime.write8(0x800DFA38U, 1U));
    assert(runtime.write32(player + 0x164U, 10U));
    const auto verifySector = [&](std::uint32_t delta,
                                  std::uint32_t expected_animation,
                                  std::uint32_t expected_loop) {
        assert(runtime.write32(player + 0x114U, 0U));
        assert(runtime.write32(player + 0x2CU, delta));
        assert(runtime.write32(0x800057F8U, 0U));
        assert(runtime.write32(0x800057FCU, 0U));
        runtime.reset(0x8003291CU, 0U, 0x801F0000U);
        runtime.setRegister(16, player);
        runUntil(0x8003291CU, 0x80032924U);
        assert(runtime.read32(0x800057F8U, value));
        if (value != expected_animation) {
            std::cerr << "mouse sector delta=" << delta << " animation="
                      << value << " expected=" << expected_animation << '\n';
        }
        assert(value == expected_animation);
        assert(runtime.read32(0x800057FCU, value) && value == expected_loop);
        assert(runtime.read32(player + 0x164U, value) && value == 10U);
    };
    verifySector(0U, 51U, 0U);
    verifySector(0x4000U, 52U, 1U);
    verifySector(0x8000U, 51U, 1U);
    verifySector(0xC000U, 52U, 0U);

    assert(stuntmaster::game::setMouseControlPatches(runtime, false));
    assert(stuntmaster::game::setMouseControlPatches(runtime, false));
    for (std::size_t index = 0U; index < windows.size(); ++index) {
        assert(runtime.read32(windows[index].first, value) &&
               value == windows[index].second[4]);
    }
    assert(runtime.write32(0x80031524U, 0U));
    assert(!stuntmaster::game::setMouseControlPatches(runtime, true));
}


void retailHlePublishesMouseExtensionWithoutChangingDigitalPadBytes() {
    stuntmaster::psx::R3000Runtime runtime;
    stuntmaster::game::RetailHle hle;
    hle.setPadOneState(true, 0x6FFFU);
    hle.setMouseControlState(0x12345678U, 2U);
    assert(hle.onVBlank(runtime));
    std::uint8_t byte = 0U;
    std::uint32_t yaw = 0U;
    assert(runtime.read8(0x800DFA30U, byte) && byte == 0U);
    assert(runtime.read8(0x800DFA31U, byte) && byte == 0x41U);
    assert(runtime.read8(0x800DFA32U, byte) && byte == 0xFFU);
    assert(runtime.read8(0x800DFA33U, byte) && byte == 0x6FU);
    assert(runtime.read32(0x800DFA34U, yaw) && yaw == 0x12345678U);
    assert(runtime.read8(0x800DFA38U, byte) && byte == 2U);
    hle.setMouseControlState(0U, 99U);
    assert(hle.onVBlank(runtime));
    assert(runtime.read8(0x800DFA38U, byte) && byte == 0U);
}

void stoppedFlipPublicationsAreVBlankBoundedAndSelfContained() {
    using stuntmaster::presentation::GpuReplaySegment;
    using stuntmaster::presentation::buildSelfContainedReplaySegments;
    using stuntmaster::presentation::shouldPublishStoppedFlipUpload;

    assert(!shouldPublishStoppedFlipUpload(false, 20U, 8U, 7U));
    assert(!shouldPublishStoppedFlipUpload(true, 2U, 8U, 7U));
    assert(!shouldPublishStoppedFlipUpload(true, 20U, 7U, 7U));
    assert(shouldPublishStoppedFlipUpload(true, 3U, 8U, 7U));

    GpuReplaySegment first;
    first.packets = {{0xE1000000U}, {0x2C000000U}};
    first.vram = {1U, 2U, 3U, 4U};
    first.vram_revision = 4U;
    const std::vector<GpuReplaySegment> settled{first};
    const std::vector<std::vector<std::uint32_t>> pending{
        {0xA0000000U, 0U, 0x00010002U}};
    const std::array<std::uint16_t, 4U> uploaded{9U, 8U, 7U, 6U};

    const auto publication = buildSelfContainedReplaySegments(
        settled, pending, uploaded, 5U);
    assert(publication.size() == 2U);
    assert(publication[0].vram_revision == 4U);
    assert(publication[0].packets.size() == 2U);
    assert(publication[1].vram_revision == 5U);
    assert(publication[1].packets == pending);
    assert(publication[1].vram ==
           std::vector<std::uint16_t>(uploaded.begin(), uploaded.end()));
    // Building a mailbox value cannot consume or mutate the accumulator;
    // otherwise a dropped fallback makes the next publication incomplete.
    assert(settled.size() == 1U);
    assert(settled.front().packets.size() == 2U);

    const auto same_revision = buildSelfContainedReplaySegments(
        settled, pending, uploaded, 4U);
    assert(same_revision.size() == 1U);
    assert(same_revision.front().packets.size() == 3U);
    assert(same_revision.front().vram == first.vram);
}

void guestScheduleDerivesEveryRateFromRetail() {
    using stuntmaster::game::console_cpu_rate;
    using stuntmaster::game::console_vblank_rate;
    using stuntmaster::game::guestScheduleFor;
    using stuntmaster::game::isSupportedGuestUpdateRate;
    using stuntmaster::game::retail_update_rate;

    // Only whole authored steps are supported, so every guest update lands on
    // a fixed schedule instead of drifting against the 30 Hz timelines.
    assert(isSupportedGuestUpdateRate(30U));
    assert(isSupportedGuestUpdateRate(60U));
    assert(isSupportedGuestUpdateRate(90U));
    assert(isSupportedGuestUpdateRate(240U));
    assert(!isSupportedGuestUpdateRate(0U));
    assert(!isSupportedGuestUpdateRate(15U));
    assert(!isSupportedGuestUpdateRate(45U));
    assert(!isSupportedGuestUpdateRate(75U));
    assert(!isSupportedGuestUpdateRate(270U));

    // Retail's own cadence: a 60 Hz display divided by the two-VBlank swap
    // gate, and nothing to retime.
    const auto retail = stuntmaster::game::retailGuestSchedule();
    assert(retail.update_rate == retail_update_rate);
    assert(retail.vblank_rate == console_vblank_rate);
    assert(retail.swap_gate_vblanks == 2U);
    assert(retail.retime_divisor == 1U);
    assert(retail.instructions_per_vblank == 564'480U);
    assert(guestScheduleFor(retail_update_rate).vblank_rate == retail.vblank_rate);

    for (std::uint32_t rate = retail_update_rate;
         rate <= stuntmaster::game::maximum_guest_update_rate;
         rate += retail_update_rate) {
        assert(isSupportedGuestUpdateRate(rate));
        const auto schedule = guestScheduleFor(rate);
        assert(schedule.update_rate == rate);
        // The gate is one word with a minimum of one VBlank, so anything above
        // the console refresh has to come from the display rate instead.
        assert(schedule.vblank_rate >= console_vblank_rate);
        assert(
            schedule.vblank_rate ==
            schedule.update_rate * schedule.swap_gate_vblanks);
        assert(schedule.swap_gate_vblanks >= 1U);
        // The guest CPU keeps console speed at every rate: a faster display
        // gets fewer instructions per refresh, not a faster machine.
        assert(
            schedule.instructions_per_vblank * schedule.vblank_rate ==
            console_cpu_rate);
        // One authored 30 Hz step per `retime_divisor` guest updates.
        assert(
            schedule.retime_divisor * retail_update_rate ==
            schedule.update_rate);
    }
}

void highFrequencyCadenceUsesRetailGameStateNotGpuTraffic() {
    using stuntmaster::game::retailStateHandlerAllowsHighFrequency;

    // The two user-visible steady gameplay states must never fall back merely
    // because rendering uploads a palette, flipbook frame, or pause UI image.
    assert(retailStateHandlerAllowsHighFrequency(0x80029C6CU)); // play
    assert(retailStateHandlerAllowsHighFrequency(0x80029EF8U)); // pause

    // Level/petal load and pre-play perform the streaming/overlay work that
    // needs retail's two-VBlank gate. They unlock only after publishing PLAY.
    assert(!retailStateHandlerAllowsHighFrequency(0x80029574U));
    assert(!retailStateHandlerAllowsHighFrequency(0x8002977CU));
    assert(!retailStateHandlerAllowsHighFrequency(0x8002986CU));
    assert(!retailStateHandlerAllowsHighFrequency(0x80029AC0U));

    // Boot, one-shot transition states and an unexpected handler all fail
    // closed instead of running an unclassified loader at high cadence.
    assert(!retailStateHandlerAllowsHighFrequency(0x80029460U));
    assert(!retailStateHandlerAllowsHighFrequency(0U));
    assert(!retailStateHandlerAllowsHighFrequency(0xDEADBEEFU));
}

void everyTrampolineFitsThePatchArenaTogether() {
    // The arena is fixed RAM with hand-placed slots, and `installTrampolineBody`
    // refuses to write over an occupied one. Applying the whole set into a
    // single runtime is therefore the check that no body has outgrown its slot.
    // Only the non-retime trampolines remain: the widescreen lower bounds and
    // the ledge trace (retiming is host-side hooks now).
    stuntmaster::psx::R3000Runtime runtime;
    const stuntmaster::game::RetailTrampoline* const patches[]{
        &stuntmaster::game::ledgeTraceCheckVerdict(),
        &stuntmaster::game::ledgeTraceFacingInputs(),
        &stuntmaster::game::ledgeTraceHumanoidState(),
        &stuntmaster::game::ledgeTraceLetGoFromLatch(),
        &stuntmaster::game::ledgeTraceLetGoFromTicket(),
    };
    const stuntmaster::game::WidescreenLowerBounds lower_bounds{
        0x257U, 0x257U};

    // Only the bodies are placed here; the sites belong to retail code that is
    // not loaded, so write the expected word at each one first.
    const auto install = [&](const stuntmaster::game::RetailTrampoline& patch) {
        assert(runtime.write32(patch.site, patch.original_word));
        assert(stuntmaster::game::applyRetailTrampoline(runtime, patch));
    };
    for (const auto* patch : patches) {
        install(*patch);
    }
    for (const auto& patch : lower_bounds.patches()) {
        install(patch);
    }
}

void guestCpuScaleTracksTheUpdateRate() {
    using stuntmaster::game::guestCpuScaleForUpdateRate;
    // The console's CPU sustains the retail rate with headroom to spare, and
    // twice it, but running the game loop k times per authored frame is k
    // times the work per second and eventually needs a faster guest.
    //
    // These thresholds are the measured route: 60 and 90 Hz held 30 authored
    // ticks a second at console speed, 120 Hz needed scale two, and 240 Hz
    // needed scale three.
    assert(guestCpuScaleForUpdateRate(30U) == 1U);
    assert(guestCpuScaleForUpdateRate(60U) == 1U);
    assert(guestCpuScaleForUpdateRate(90U) == 1U);
    assert(guestCpuScaleForUpdateRate(120U) == 2U);
    assert(guestCpuScaleForUpdateRate(240U) == 3U);

    // The inverse, which is the figure worth reporting: raising the scale does
    // not make the host faster, it raises what the host must interpret per
    // wall-clock second. At the measured 1.10x host headroom, scale one is the
    // only usable setting in live play, so 90 Hz is the real ceiling.
    using stuntmaster::game::sustainableGuestUpdateRate;
    assert(sustainableGuestUpdateRate(1U) == 90U);
    assert(sustainableGuestUpdateRate(2U) == 210U);
    assert(sustainableGuestUpdateRate(4U) ==
           stuntmaster::game::maximum_guest_update_rate);
    // Never below retail's own rate, and never a rate the schedule rejects.
    assert(sustainableGuestUpdateRate(0U) >=
           stuntmaster::game::retail_update_rate);
    for (const std::uint32_t scale : {0U, 1U, 2U, 3U, 4U}) {
        assert(stuntmaster::game::isSupportedGuestUpdateRate(
            sustainableGuestUpdateRate(scale)));
    }
    // Monotonic, and never zero: a scale of zero would stop the guest.
    std::uint32_t previous = 0U;
    for (std::uint32_t rate = stuntmaster::game::retail_update_rate;
         rate <= stuntmaster::game::maximum_guest_update_rate;
         rate += stuntmaster::game::retail_update_rate) {
        const auto scale = guestCpuScaleForUpdateRate(rate);
        assert(scale >= 1U);
        assert(scale >= previous);
        previous = scale;
    }
}

void ratePacerCarriesEveryRemainder() {
    using stuntmaster::game::audioFramePacer;
    using stuntmaster::game::guest_audio_rate;

    // The console's refresh divides the audio rate exactly.
    auto console = audioFramePacer(60U);
    for (int vblank = 0; vblank < 60; ++vblank) {
        assert(console.take() == 735U);
    }
    assert(console.maximumStep() == 735U);

    // 120 and 240 do not, so the remainder has to be carried or the mix drifts
    // against the guest clock.
    for (const std::uint32_t vblank_rate : {60U, 90U, 120U, 150U, 240U}) {
        auto pacer = audioFramePacer(vblank_rate);
        std::uint64_t produced = 0U;
        std::uint64_t largest = 0U;
        for (std::uint32_t vblank = 0U; vblank < vblank_rate; ++vblank) {
            const auto frames = pacer.take();
            largest = std::max(largest, frames);
            produced += frames;
        }
        // Exactly one second of audio per second of guest time, at every rate.
        assert(produced == guest_audio_rate);
        assert(largest <= pacer.maximumStep());
        // Never more than the console's own block, which sizes the mix buffer.
        assert(largest <= 735U);
    }

    // A zero denominator cannot divide by zero, and resetting drops only the
    // carried fraction.
    stuntmaster::game::RatePacer degenerate{7U, 0U};
    assert(degenerate.take() == 7U);
    stuntmaster::game::RatePacer pacer{44'100U, 120U};
    assert(pacer.take() == 367U);
    pacer.reset(44'100U, 60U);
    assert(pacer.take() == 735U);
}

void cdCompletionKeepsOneDriveSpeedAtEveryVBlankRate() {
    // Pacing exposes a remaining-sector count to retail's polling loader. The
    // drive's transfer rate is a wall-clock quantity, so a faster emulated
    // display must not make the disc faster with it.
    const auto vblanksToDrain = [](std::uint32_t vblank_rate,
                                   std::uint32_t speed,
                                   std::uint32_t sectors) {
        stuntmaster::game::RetailHle hle;
        hle.setCdReadSpeed(speed);
        hle.setVblankRate(vblank_rate);
        assert(hle.vblankRate() == vblank_rate);
        stuntmaster::psx::R3000Runtime runtime;
        hle.setCdPendingSectors(sectors);
        std::uint32_t vblanks = 0U;
        while (hle.cdSectorsPending() != 0U && vblanks < 100'000U) {
            assert(hle.onVBlank(runtime));
            ++vblanks;
        }
        assert(hle.cdSectorsPending() == 0U);
        return vblanks;
    };

    constexpr std::uint32_t sectors = 750U;
    // At the console's double speed that is 750 / 150 = five seconds, whatever
    // the display is doing.
    for (const std::uint32_t vblank_rate : {60U, 90U, 120U, 240U}) {
        const auto vblanks = vblanksToDrain(vblank_rate, 2U, sectors);
        const auto seconds =
            static_cast<double>(vblanks) / static_cast<double>(vblank_rate);
        assert(seconds > 4.9 && seconds < 5.1);
    }
    // The 60 Hz reference is unchanged: five units a VBlank per speed multiple.
    assert(vblanksToDrain(60U, 2U, sectors) == 300U);
    assert(vblanksToDrain(60U, 1U, sectors) == 600U);
}

void audioRingDiscardsAnEndedTimeline() {
    stuntmaster::core::AudioRing ring{8U};
    const std::array<std::int16_t, 6U> first{1, 2, 3, 4, 5, 6};
    ring.push(first);
    assert(ring.available() == first.size());
    assert(ring.discardAll() == first.size());
    assert(ring.available() == 0U);
    assert(ring.discardAll() == 0U);

    const std::array<std::int16_t, 2U> next{7, 8};
    ring.push(next);
    std::array<std::int16_t, 4U> out{};
    assert(ring.pop(out) == next.size());
    assert(out[0] == 7 && out[1] == 8);
}

void debugOverlayReportsHostAndGuestState() {
    using stuntmaster::presentation::DebugOverlayState;
    using stuntmaster::presentation::DebugOverlayVisibilityToggle;
    using stuntmaster::presentation::debugOverlayRows;
    using stuntmaster::presentation::rasterizeDebugOverlay;
    using stuntmaster::presentation::rasterizeNotificationOverlay;

    DebugOverlayState state;
    state.enabled = true;
    state.guest_update_rate = 120U;
    // While a load holds the guest at retail's cadence the requested rate and
    // the schedule in force disagree, and the panel has to show both.
    state.guest_vblank_rate = 60U;
    state.retime_divisor = 1U;
    state.guest_ticks = 123U;
    state.guest_vblanks = 456U;
    state.guest_frames = 789U;
    state.presentation_rate = 120U;
    state.high_frequency_requested = true;
    state.widescreen_cull_active = true;
    const auto waiting = debugOverlayRows(state);
    assert((waiting == std::vector<std::string>{
        "GUEST 120HZ TICK 123",
        "VBLANK 456 FRAME 789",
        "HOST 120HZ PSYCROSS",
        "PATCH 120HZ WAIT",
        "SCHED VB 60 DIV 1",
        "SPEED MEASURING",
        "RETIME NONE",
        "HOOK 0 OF 0",
        "CULL WIDE",
    }));

    state.high_frequency_active = true;
    state.guest_vblank_rate = 120U;
    state.retime_divisor = 4U;
    // The guest is interpreted, so a schedule the host cannot deliver shows up
    // here rather than as a retiming fault. This is the reading that
    // distinguishes "everything is at half speed" from a wrong divisor.
    state.guest_speed_percent = 55U;
    state.retime_motion_active = true;
    state.retime_clock_active = true;
    const auto active = debugOverlayRows(state);
    assert(active[3] == "PATCH 120HZ ACTIVE");
    assert(active[4] == "SCHED VB 120 DIV 4");
    assert(active[5] == "SPEED 55%");
    assert(active[6] == "RETIME MOTION CLOCK");
    // Retime hooks live in swapped disc overlays, so the panel reports how
    // many of the armed hooks are actually live right now.
    state.retime_hooks_armed = 4U;
    state.retime_hooks_live = 3U;
    assert(debugOverlayRows(state)[7] == "HOOK 3 OF 4");

    // At the retail rate the panel says so rather than reporting a patch rate
    // that was never requested.
    auto stock = state;
    stock.guest_update_rate = 30U;
    stock.high_frequency_requested = false;
    stock.high_frequency_active = false;
    stock.guest_vblank_rate = 60U;
    stock.retime_divisor = 1U;
    assert(debugOverlayRows(stock)[3] == "PATCH OFF");

    const auto bitmap = rasterizeDebugOverlay(state, 2U);
    assert(bitmap.width > 0U);
    assert(bitmap.height > 0U);
    assert(bitmap.rgba.size() ==
           static_cast<std::size_t>(bitmap.width) * bitmap.height * 4U);
    const auto lit_pixels = std::count_if(
        bitmap.rgba.begin(), bitmap.rgba.end(),
        [](unsigned char value) { return value == 255U; });
    assert(lit_pixels > bitmap.width * bitmap.height);

    state.enabled = false;
    assert(rasterizeDebugOverlay(state, 2U).rgba.empty());
    const auto notification =
        rasterizeNotificationOverlay("QUICK SAVE CREATED", 2U);
    assert(notification.width > notification.height);
    assert(notification.rgba.size() ==
           static_cast<std::size_t>(notification.width) *
               notification.height * 4U);
    assert(rasterizeNotificationOverlay({}, 2U).rgba.empty());

    DebugOverlayVisibilityToggle toggle;
    auto visible = true;
    visible = toggle.update(true, visible, false);
    assert(visible);
    visible = toggle.update(true, visible, true);
    assert(!visible);
    visible = toggle.update(true, visible, true);
    assert(!visible);
    visible = toggle.update(true, visible, false);
    assert(!visible);
    visible = toggle.update(true, visible, true);
    assert(visible);
    visible = toggle.update(false, visible, false);
    assert(!visible);
}

void displayScalingPreservesAspectAndCenters() {
    using stuntmaster::presentation::DisplayViewport;
    using stuntmaster::presentation::fitDisplayViewport;

    assert((fitDisplayViewport(16U, 9U, 1280U, 720U) ==
            DisplayViewport{0U, 0U, 1280U, 720U}));
    assert((fitDisplayViewport(16U, 9U, 800U, 600U) ==
            DisplayViewport{0U, 75U, 800U, 450U}));
    assert((fitDisplayViewport(4U, 3U, 1280U, 720U) ==
            DisplayViewport{160U, 0U, 960U, 720U}));
    assert((fitDisplayViewport(4U, 3U, 801U, 601U) ==
            DisplayViewport{0U, 0U, 801U, 600U}));
    assert((fitDisplayViewport(0U, 3U, 800U, 600U) == DisplayViewport{}));
}

void oversizedGpuPolygonsMatchHardwareLimits() {
    using stuntmaster::psx::gpuPolygonExceedsDrawingLimits;

    // Exact GT4 packet isolated from the blocked-grid quick save. Its four
    // projected corners span x=-142..1023 and y=-154..803, so the PS1 GPU
    // rejects it while an unrestricted host renderer fills the display with
    // the near-camera gate texture.
    const std::array<std::uint32_t, 12U> blocked_grid{
        0x3C3B5A5AU, 0x0323FF72U, 0x26BFAFE0U, 0x00476B6BU,
        0x032303FFU, 0x0041AFFFU, 0x00324C4CU, 0xFF66FF72U,
        0x0000A0E0U, 0x003B5A5AU, 0xFF6603FFU, 0x0000A0FFU,
    };
    assert(gpuPolygonExceedsDrawingLimits(blocked_grid));

    const auto packPosition = [](std::int32_t x, std::int32_t y) {
        return static_cast<std::uint32_t>(x) & 0x7FFU |
            (static_cast<std::uint32_t>(y) & 0x7FFU) << 16U;
    };
    auto at_limit = blocked_grid;
    at_limit[1] = packPosition(0, 0);
    at_limit[4] = packPosition(1023, 0);
    at_limit[7] = packPosition(0, 511);
    at_limit[10] = packPosition(1023, 511);
    assert(!gpuPolygonExceedsDrawingLimits(at_limit));

    auto too_wide = at_limit;
    too_wide[1] = packPosition(-1, 0);
    too_wide[7] = packPosition(-1, 511);
    assert(gpuPolygonExceedsDrawingLimits(too_wide));

    auto too_tall = at_limit;
    too_tall[1] = packPosition(0, -1);
    too_tall[4] = packPosition(1023, -1);
    assert(gpuPolygonExceedsDrawingLimits(too_tall));

    const std::array<std::uint32_t, 1U> non_polygon{0xE1000000U};
    const std::array<std::uint32_t, 1U> truncated_polygon{0x3C000000U};
    assert(!gpuPolygonExceedsDrawingLimits(non_polygon));
    assert(!gpuPolygonExceedsDrawingLimits(truncated_polygon));
}

void writeLe32(std::vector<std::byte>& bytes, std::size_t offset,
               std::uint32_t value) {
    for (std::size_t index = 0; index < 4; ++index) {
        bytes[offset + index] = std::byte{static_cast<unsigned char>(
            (value >> (index * 8U)) & 0xFFU)};
    }
}

void executableParsing() {
    std::vector<std::byte> bytes(2048 + 16);
    constexpr char signature[] = "PS-X EXE";
    std::memcpy(bytes.data(), signature, 8);
    writeLe32(bytes, 0x10, 0x80010004);
    writeLe32(bytes, 0x18, 0x80010000);
    writeLe32(bytes, 0x1C, 16);
    writeLe32(bytes, 0x30, 0x801FFFF0);
    const auto executable = stuntmaster::psx::Executable::parse(bytes);
    assert(executable.header().initial_pc == 0x80010004);
    assert(executable.header().text_size == 16);
    assert(executable.text().size() == 16);
}

void bootPathParsing() {
    const auto path = stuntmaster::psx::parseBootPath(
        "BOOT = cdrom:\\\\SLUS_006.84;1\r\nTCB = 4\r\n");
    assert(path == "SLUS_006.84");
}

void invalidExecutableRejected() {
    bool rejected = false;
    try {
        static_cast<void>(
            stuntmaster::psx::Executable::parse(std::vector<std::byte>(2048)));
    } catch (const stuntmaster::core::Error&) {
        rejected = true;
    }
    assert(rejected);
}

void sha256AndGameIdentity() {
    constexpr char abc[] = "abc";
    const auto digest = stuntmaster::core::sha256(
        std::as_bytes(std::span{abc, sizeof(abc) - 1}));
    assert(stuntmaster::core::toHex(digest) ==
           "ba7816bf8f01cfea414140de5dae2223"
           "b00361a396177a9cb410ff61f20015ad");

    const auto& supported = stuntmaster::game::ntscU();
    assert(stuntmaster::core::toHex(supported.executable_sha256) ==
           "5aae79f0d603bf95bdcf9a2c278d1891"
           "dcbd9e835a59d4673d6e6dbd82665c64");
    assert(stuntmaster::game::identify(
        supported.volume_id, supported.executable_path,
        supported.executable_sha256));
    auto wrong_digest = supported.executable_sha256;
    wrong_digest[0] ^= std::byte{1};
    assert(!stuntmaster::game::identify(
        supported.volume_id, supported.executable_path, wrong_digest));
}

constexpr std::uint32_t encodeI(std::uint32_t opcode, std::uint32_t rs,
                                std::uint32_t rt, std::uint16_t immediate) {
    return opcode << 26U | rs << 21U | rt << 16U | immediate;
}

constexpr std::uint32_t encodeR(std::uint32_t rs, std::uint32_t rt,
                                std::uint32_t rd, std::uint32_t shift,
                                std::uint32_t function) {
    return rs << 21U | rt << 16U | rd << 11U | shift << 6U | function;
}

void r3000Execution() {
    stuntmaster::psx::R3000Runtime runtime;
    constexpr std::uint32_t address = 0x80010000U;
    const std::array code{
        encodeI(0x09, 0, 2, 40),       // addiu v0, zero, 40
        encodeI(0x09, 2, 2, 2),        // addiu v0, v0, 2
        encodeR(31, 0, 0, 0, 0x08),    // jr ra
        std::uint32_t{0},               // nop
    };
    const auto bytes = std::as_bytes(std::span{code});
    assert(runtime.loadBytes(address, bytes));
    runtime.reset(address, 0, 0x801FFFF0U);
    const auto result = runtime.call(address, {}, 16);
    assert(result.reason == stuntmaster::psx::R3000StopReason::returned);
    assert(runtime.state().gpr[2] == 42);
}

void r3000BatchStopsOnlyAtMachineBoundaries() {
    using stuntmaster::psx::R3000ExecutionBoundaries;
    using stuntmaster::psx::R3000Runtime;
    using stuntmaster::psx::R3000StopReason;

    constexpr std::uint32_t address = 0x80010000U;
    R3000Runtime runtime;
    const std::array code{
        encodeI(0x09, 0, 2, 40),       // addiu v0, zero, 40
        encodeI(0x09, 2, 2, 2),        // addiu v0, v0, 2
        encodeI(0x09, 2, 2, 1),        // boundary: addiu v0, v0, 1
        encodeI(0x0F, 0, 8, 0x1F80),   // lui   t0, 0x1f80
        encodeI(0x0D, 8, 8, 0x1814),   // ori   t0, t0, 0x1814
        encodeI(0x2B, 8, 0, 0),        // sw    zero, 0(t0) (claimed MMIO)
        encodeI(0x09, 0, 3, 7),        // addiu v1, zero, 7
    };
    assert(runtime.loadBytes(address, std::as_bytes(std::span{code})));
    runtime.reset(address, 0U, 0x801FFFF0U);

    R3000ExecutionBoundaries boundaries;
    boundaries.add(address + 8U);
    auto batch = runtime.runBatch(32U, boundaries);
    assert(batch.reason == R3000StopReason::running);
    assert(batch.instructions == 2U);
    assert(runtime.state().pc == address + 8U);
    assert(runtime.state().gpr[2] == 42U);

    stuntmaster::psx::GpuCommandBridge gpu{{}, &runtime};
    runtime.attachMmioBus(&gpu);
    batch = runtime.runBatch(
        32U, boundaries, /*execute_initial_boundary=*/true);
    assert(batch.reason == R3000StopReason::running);
    // The boundary instruction and two address-building instructions execute,
    // followed by the MMIO store; the batch yields before the next guest op.
    assert(batch.instructions == 4U);
    assert(runtime.state().pc == address + 24U);
    assert(runtime.state().gpr[2] == 43U);
    assert(runtime.state().gpr[3] == 0U);

    batch = runtime.runBatch(1U, boundaries);
    assert(batch.instructions == 1U);
    assert(runtime.state().gpr[3] == 7U);
}

void r3000RecompilerMatchesInterpreterAndInvalidatesCodeWrites() {
    using stuntmaster::psx::R3000ExecutionBackend;
    using stuntmaster::psx::R3000ExecutionBoundaries;
    using stuntmaster::psx::R3000Runtime;
    using stuntmaster::psx::R3000State;
    using stuntmaster::psx::R3000StopReason;

    constexpr std::uint32_t address = 0x80010000U;
    constexpr std::uint32_t data_address = 0x80018000U;
    const std::array code{
        encodeI(0x0F, 0, 8, 0x8001),    // lui   $t0, 0x8001
        encodeI(0x0D, 8, 8, 0x8000),    // ori   $t0, $t0, 0x8000
        encodeI(0x09, 0, 9, 7),         // addiu $t1, $zero, 7
        encodeI(0x2B, 8, 9, 0),         // sw    $t1, 0($t0)
        encodeI(0x23, 8, 10, 0),        // lw    $t2, 0($t0)
        std::uint32_t{0},                // load-delay slot
        encodeI(0x09, 10, 10, 5),       // addiu $t2, $t2, 5
        encodeR(9, 10, 0, 0, 0x18),     // mult  $t1, $t2
        encodeI(0x05, 10, 9, 2),        // bne   $t2, $t1, return
        encodeI(0x09, 0, 2, 1),         // delay slot: $v0 = 1
        encodeI(0x09, 0, 2, 99),        // skipped
        encodeR(31, 0, 16, 0, 0x12),    // mflo  $s0 (rd=16)
        encodeR(31, 0, 0, 0, 0x08),     // jr    $ra
        std::uint32_t{0},
    };

    const auto run = [&](R3000ExecutionBackend backend) {
        R3000Runtime runtime;
        runtime.setExecutionBackend(backend);
        assert(runtime.loadBytes(address, std::as_bytes(std::span{code})));
        runtime.reset(address, 0U, 0x801F0000U);
        runtime.setRegister(31U, R3000Runtime::return_sentinel);
        R3000ExecutionBoundaries boundaries;
        const auto result = runtime.runBatch(64U, boundaries);
        assert(result.reason == R3000StopReason::running);
        assert(runtime.atReturnSentinel());
        runtime.settleLoadDelay();
        std::uint32_t stored{};
        assert(runtime.read32(data_address, stored));
        return std::tuple{runtime.state(), result.instructions, stored,
                          runtime.recompilerStats()};
    };

    const auto [interpreted, interpreted_count, interpreted_store,
                interpreted_stats] = run(R3000ExecutionBackend::interpreter);
    const auto [recompiled, recompiled_count, recompiled_store,
                recompiled_stats] = run(R3000ExecutionBackend::cached_recompiler);
    const auto states_match = [](const R3000State& left, const R3000State& right) {
        return left.gpr == right.gpr &&
            left.gte.data == right.gte.data &&
            left.gte.control == right.gte.control &&
            left.cop0_status == right.cop0_status &&
            left.cop0_cause == right.cop0_cause &&
            left.cop0_epc == right.cop0_epc &&
            left.cop0_bad_vaddr == right.cop0_bad_vaddr &&
            left.hi == right.hi && left.lo == right.lo &&
            left.pc == right.pc && left.next_pc == right.next_pc &&
            left.branch_pc == right.branch_pc &&
            left.branch_delay_slot == right.branch_delay_slot &&
            left.load_delay.reg == right.load_delay.reg &&
            left.load_delay.value == right.load_delay.value &&
            left.load_delay.valid == right.load_delay.valid &&
            left.next_load_delay.reg == right.next_load_delay.reg &&
            left.next_load_delay.value == right.next_load_delay.value &&
            left.next_load_delay.valid == right.next_load_delay.valid;
    };
    assert(states_match(interpreted, recompiled));
    assert(interpreted_count == recompiled_count);
    assert(interpreted_store == 7U && recompiled_store == interpreted_store);
    assert(recompiled.gpr[10] == 12U);
    assert(recompiled.gpr[16] == 84U);
    assert(interpreted_stats.blocks_compiled == 0U);
    assert(recompiled_stats.blocks_compiled != 0U);
    assert(recompiled_stats.instructions_executed == recompiled_count);

    // A hot two-block loop exercises the native tier after its lazy compile
    // threshold. The load followed by dependent ALU operations pins R3000A
    // load-delay semantics while the terminating store exercises the guarded
    // direct-RAM write and host-side invalidation handoff.
    constexpr std::uint32_t native_address = 0x80012000U;
    constexpr std::uint32_t native_data_address = 0x80014000U;
    const std::array native_code{
        encodeI(0x09, 14, 14, 1),        // addiu $t6, $t6, 1
        encodeI(0x23, 8, 9, 0),          // lw    $t1, 0($t0)
        encodeI(0x09, 9, 10, 1),         // delay: $t2 = old $t1 + 1
        encodeR(9, 10, 11, 0, 0x21),     // addu  $t3, $t1, $t2
        encodeI(0x2B, 8, 11, 0),         // sw    $t3, 0($t0)
        encodeI(0x09, 12, 12, 1),        // addiu $t4, $t4, 1
        encodeI(0x0B, 12, 13, 100),      // sltiu $t5, $t4, 100
        encodeI(0x05, 13, 0, 0xfff8),    // bne   $t5, $zero, loop
        std::uint32_t{0},
        encodeR(31, 0, 0, 0, 0x08),
        std::uint32_t{0},
    };
    const auto run_native_loop = [&](
        R3000ExecutionBackend backend, std::uint32_t data_address) {
        R3000Runtime runtime;
        runtime.setExecutionBackend(backend);
        assert(runtime.loadBytes(
            native_address, std::as_bytes(std::span{native_code})));
        assert(runtime.write32(data_address, 5U));
        runtime.reset(native_address, 0U, 0x801F0000U);
        runtime.setRegister(8U, data_address);
        runtime.setRegister(31U, R3000Runtime::return_sentinel);
        R3000ExecutionBoundaries boundaries;
        const auto result = runtime.runBatch(2'000U, boundaries);
        assert(result.reason == R3000StopReason::running);
        assert(runtime.atReturnSentinel());
        runtime.settleLoadDelay();
        std::uint32_t stored{};
        assert(runtime.read32(data_address, stored));
        return std::tuple{runtime.state(), result.instructions, stored,
                          runtime.recompilerStats()};
    };
    const auto [native_interpreted, native_interpreted_count,
                native_interpreted_store, native_interpreted_stats] =
        run_native_loop(
            R3000ExecutionBackend::interpreter, native_data_address);
    const auto [native_recompiled, native_recompiled_count,
                native_recompiled_store, native_stats] =
        run_native_loop(
            R3000ExecutionBackend::native_recompiler, native_data_address);
    assert(states_match(native_interpreted, native_recompiled));
    assert(native_interpreted_count == native_recompiled_count);
    assert(native_interpreted_store == native_recompiled_store);
    assert(native_stats.native_blocks_compiled != 0U);
    assert(native_stats.native_regions_compiled != 0U);
    assert(native_stats.native_instructions_executed != 0U);
    assert(native_stats.native_stores_executed != 0U);
    assert(native_stats.native_control_flows_executed != 0U);
    assert(native_interpreted_stats.native_instructions_executed == 0U);
    assert(native_interpreted_stats.native_stores_executed == 0U);
    assert(native_interpreted_stats.native_control_flows_executed == 0U);

    // Conditional branches execute together with their architectural delay
    // slot. Exercise every first-stage condition in both directions after
    // warming the exact block past the lazy native-compilation threshold.
    constexpr std::uint32_t branch_address = 0x8001B000U;
    const auto run_branch_case = [&] (
        R3000ExecutionBackend backend, std::uint32_t branch,
        std::uint32_t left, std::uint32_t right, bool warm) {
        const std::array branch_code{
            branch,                                 // target is instruction 5
            encodeI(0x09, 10, 10, 1),              // delay: ++$t2
            encodeI(0x09, 0, 2, 11),               // fall-through result
            encodeR(18, 0, 0, 0, 0x08),            // jr $s2
            std::uint32_t{0},
            encodeI(0x09, 0, 2, 22),               // taken result
            encodeR(18, 0, 0, 0, 0x08),            // jr $s2
            std::uint32_t{0},
        };
        R3000Runtime runtime;
        runtime.setExecutionBackend(backend);
        assert(runtime.loadBytes(
            branch_address, std::as_bytes(std::span{branch_code})));
        const auto invoke = [&] {
            runtime.reset(branch_address, 0U, 0x801F0000U);
            runtime.setRegister(8U, left);
            runtime.setRegister(9U, right);
            runtime.setRegister(18U, R3000Runtime::return_sentinel);
            R3000ExecutionBoundaries boundaries;
            const auto result = runtime.runBatch(32U, boundaries);
            assert(result.reason == R3000StopReason::running);
            assert(runtime.atReturnSentinel());
            return result.instructions;
        };
        if (warm) {
            for (auto iteration = 0U; iteration != 20U; ++iteration) {
                static_cast<void>(invoke());
            }
        }
        const auto count = invoke();
        return std::tuple{
            runtime.state(), count, runtime.recompilerStats()};
    };
    const auto compare_branch = [&] (
        std::uint32_t instruction, std::uint32_t left,
        std::uint32_t right, bool taken) {
        const auto [branch_interpreted, branch_interpreted_count,
                    branch_interpreted_stats] =
            run_branch_case(
                R3000ExecutionBackend::interpreter,
                instruction, left, right, false);
        const auto [branch_native, branch_native_count, branch_native_stats] =
            run_branch_case(
                R3000ExecutionBackend::native_recompiler,
                instruction, left, right, true);
        assert(states_match(branch_interpreted, branch_native));
        assert(branch_interpreted_count == branch_native_count);
        assert(branch_native.gpr[2] == (taken ? 22U : 11U));
        assert(branch_native.gpr[10] == 1U);
        const auto kind = (instruction >> 16U) & 31U;
        if ((instruction >> 26U) == 0x01U && (kind & 0x10U) != 0U) {
            assert(branch_native.gpr[31] == branch_address + 8U);
        }
        assert(branch_native_stats.native_control_flows_executed != 0U);
        assert(branch_interpreted_stats.native_control_flows_executed == 0U);
    };
    compare_branch(encodeI(0x04, 8, 9, 4), 5U, 5U, true);   // beq
    compare_branch(encodeI(0x04, 8, 9, 4), 5U, 6U, false);
    compare_branch(encodeI(0x05, 8, 9, 4), 5U, 6U, true);   // bne
    compare_branch(encodeI(0x05, 8, 9, 4), 5U, 5U, false);
    compare_branch(encodeI(0x06, 8, 0, 4), 0U, 0U, true);   // blez
    compare_branch(encodeI(0x06, 8, 0, 4), 1U, 0U, false);
    compare_branch(encodeI(0x07, 8, 0, 4), 1U, 0U, true);   // bgtz
    compare_branch(encodeI(0x07, 8, 0, 4), 0xffffffffU, 0U, false);
    compare_branch(encodeI(0x01, 8, 0x00, 4), 0xffffffffU, 0U, true); // bltz
    compare_branch(encodeI(0x01, 8, 0x00, 4), 0U, 0U, false);
    compare_branch(encodeI(0x01, 8, 0x01, 4), 0U, 0U, true); // bgez
    compare_branch(encodeI(0x01, 8, 0x01, 4), 0xffffffffU, 0U, false);
    compare_branch(encodeI(0x01, 8, 0x10, 4), 0xffffffffU, 0U, true); // bltzal
    compare_branch(encodeI(0x01, 8, 0x10, 4), 0U, 0U, false);
    compare_branch(encodeI(0x01, 8, 0x11, 4), 0U, 0U, true); // bgezal
    compare_branch(encodeI(0x01, 8, 0x11, 4), 0xffffffffU, 0U, false);

    // Direct and indirect jump forms share the same delay-slot completion but
    // have distinct link hazards. JALR captures its target before writing the
    // custom link register; both subroutines return through their generated
    // links, while the final JR uses a host-provided sentinel register.
    constexpr std::uint32_t jump_address = 0x8001F000U;
    constexpr auto encode_jump = [](std::uint32_t opcode,
                                    std::uint32_t target) {
        return (opcode << 26U) | ((target >> 2U) & 0x03ffffffU);
    };
    const std::array jump_code{
        encode_jump(0x03U, jump_address + 32U),     // jal function A
        encodeI(0x09, 8, 8, 1),                    // delay: ++$t0
        encodeR(9, 0, 17, 0, 0x09),                // jalr $s1,$t1
        encodeI(0x09, 8, 8, 1),                    // delay: ++$t0
        encode_jump(0x02U, jump_address + 56U),     // j end
        encodeI(0x09, 8, 8, 1),                    // delay: ++$t0
        encodeI(0x09, 0, 2, 99),                   // skipped
        std::uint32_t{0},
        encodeI(0x09, 2, 2, 10),                   // function A
        encodeR(31, 0, 0, 0, 0x08),                // jr $ra
        encodeI(0x09, 8, 8, 1),                    // delay: ++$t0
        encodeI(0x09, 2, 2, 20),                   // function B
        encodeR(17, 0, 0, 0, 0x08),                // jr $s1
        encodeI(0x09, 8, 8, 1),                    // delay: ++$t0
        encodeR(18, 0, 0, 0, 0x08),                // end: jr $s2
        std::uint32_t{0},
    };
    const auto run_jump_case = [&] (
        R3000ExecutionBackend backend, bool warm) {
        R3000Runtime runtime;
        runtime.setExecutionBackend(backend);
        assert(runtime.loadBytes(
            jump_address, std::as_bytes(std::span{jump_code})));
        const auto invoke = [&] {
            runtime.reset(jump_address, 0U, 0x801F0000U);
            runtime.setRegister(9U, jump_address + 44U);
            runtime.setRegister(18U, R3000Runtime::return_sentinel);
            R3000ExecutionBoundaries boundaries;
            const auto result = runtime.runBatch(64U, boundaries);
            assert(result.reason == R3000StopReason::running);
            assert(runtime.atReturnSentinel());
            return result.instructions;
        };
        if (warm) {
            for (auto iteration = 0U; iteration != 20U; ++iteration) {
                static_cast<void>(invoke());
            }
        }
        const auto count = invoke();
        return std::tuple{
            runtime.state(), count, runtime.recompilerStats()};
    };
    const auto [jump_interpreted, jump_interpreted_count,
                jump_interpreted_stats] =
        run_jump_case(R3000ExecutionBackend::interpreter, false);
    const auto [jump_native, jump_native_count, jump_native_stats] =
        run_jump_case(R3000ExecutionBackend::native_recompiler, true);
    assert(states_match(jump_interpreted, jump_native));
    assert(jump_interpreted_count == jump_native_count);
    assert(jump_native.gpr[2] == 30U);
    assert(jump_native.gpr[8] == 5U);
    assert(jump_native.gpr[17] == jump_address + 16U);
    assert(jump_native.gpr[31] == jump_address + 8U);
    assert(jump_native_stats.native_control_flows_executed != 0U);
    assert(jump_interpreted_stats.native_control_flows_executed == 0U);

    // A load immediately before the branch becomes visible to its delay slot,
    // but the branch condition itself must still observe the old register.
    constexpr std::uint32_t branch_load_address = 0x8001C000U;
    constexpr std::uint32_t branch_load_data = 0x8001D000U;
    const std::array branch_load_code{
        encodeI(0x23, 11, 8, 0),                // lw $t0, 0($t3)
        encodeI(0x04, 8, 9, 4),                 // old $t0 == $t1
        encodeR(8, 0, 10, 0, 0x21),             // delay sees loaded $t0
        encodeI(0x09, 0, 2, 11),
        encodeR(31, 0, 0, 0, 0x08),
        std::uint32_t{0},
        encodeI(0x09, 0, 2, 22),
        encodeR(31, 0, 0, 0, 0x08),
        std::uint32_t{0},
    };
    const auto run_branch_load = [&] (
        R3000ExecutionBackend backend, bool warm) {
        R3000Runtime runtime;
        runtime.setExecutionBackend(backend);
        assert(runtime.loadBytes(
            branch_load_address,
            std::as_bytes(std::span{branch_load_code})));
        assert(runtime.write32(branch_load_data, 9U));
        const auto invoke = [&] {
            runtime.reset(branch_load_address, 0U, 0x801F0000U);
            runtime.setRegister(8U, 7U);
            runtime.setRegister(9U, 7U);
            runtime.setRegister(11U, branch_load_data);
            runtime.setRegister(31U, R3000Runtime::return_sentinel);
            R3000ExecutionBoundaries boundaries;
            const auto result = runtime.runBatch(32U, boundaries);
            assert(result.reason == R3000StopReason::running);
            assert(runtime.atReturnSentinel());
            return result.instructions;
        };
        if (warm) {
            for (auto iteration = 0U; iteration != 20U; ++iteration) {
                static_cast<void>(invoke());
            }
        }
        const auto count = invoke();
        return std::tuple{
            runtime.state(), count, runtime.recompilerStats()};
    };
    const auto [branch_load_interpreted, branch_load_interpreted_count,
                branch_load_interpreted_stats] =
        run_branch_load(R3000ExecutionBackend::interpreter, false);
    const auto [branch_load_native, branch_load_native_count,
                branch_load_native_stats] =
        run_branch_load(R3000ExecutionBackend::native_recompiler, true);
    assert(states_match(branch_load_interpreted, branch_load_native));
    assert(branch_load_interpreted_count == branch_load_native_count);
    assert(branch_load_native.gpr[2] == 22U);
    assert(branch_load_native.gpr[8] == 9U);
    assert(branch_load_native.gpr[10] == 9U);
    assert(branch_load_native_stats.native_control_flows_executed != 0U);
    assert(branch_load_interpreted_stats.native_control_flows_executed == 0U);

    // A non-RAM load in the branch delay slot precisely exits after the
    // branch. The portable retry must retain the selected target and execute
    // the slot with branch_delay_slot set. A host boundary at the slot instead
    // prevents the native pair from starting at all.
    constexpr std::uint32_t branch_exit_address = 0x8001E000U;
    constexpr std::uint32_t branch_exit_data = 0x1F800000U;
    const std::array branch_exit_code{
        encodeI(0x04, 0, 0, 3),                 // beq -> taken result
        encodeI(0x23, 8, 10, 0),                // delay: scratchpad lw
        encodeI(0x09, 0, 2, 11),
        encodeR(31, 0, 0, 0, 0x08),
        encodeI(0x09, 0, 2, 22),
        encodeR(31, 0, 0, 0, 0x08),
        std::uint32_t{0},
    };
    const auto run_branch_exit = [&] (
        R3000ExecutionBackend backend, bool stop_at_delay) {
        R3000Runtime runtime;
        runtime.setExecutionBackend(backend);
        assert(runtime.loadBytes(
            branch_exit_address,
            std::as_bytes(std::span{branch_exit_code})));
        assert(runtime.write32(branch_exit_data, 0x12345678U));
        const auto invoke = [&](bool boundary) {
            runtime.reset(branch_exit_address, 0U, 0x801F0000U);
            runtime.setRegister(8U, branch_exit_data);
            runtime.setRegister(31U, R3000Runtime::return_sentinel);
            R3000ExecutionBoundaries boundaries;
            if (boundary) {
                boundaries.add(branch_exit_address + 4U);
            }
            return runtime.runBatch(32U, boundaries);
        };
        for (auto iteration = 0U; iteration != 20U; ++iteration) {
            const auto warm = invoke(false);
            assert(warm.reason == R3000StopReason::running);
            assert(runtime.atReturnSentinel());
        }
        const auto branches_before =
            runtime.recompilerStats().native_control_flows_executed;
        const auto result = invoke(stop_at_delay);
        return std::tuple{runtime.state(), result, runtime.recompilerStats(),
                          branches_before};
    };
    const auto [branch_exit_interpreted, branch_exit_interpreted_result,
                branch_exit_interpreted_stats, branch_exit_unused] =
        run_branch_exit(R3000ExecutionBackend::interpreter, false);
    const auto [branch_exit_native, branch_exit_native_result,
                branch_exit_native_stats, branch_exit_branches_before] =
        run_branch_exit(R3000ExecutionBackend::native_recompiler, false);
    assert(states_match(branch_exit_interpreted, branch_exit_native));
    assert(branch_exit_interpreted_result.instructions ==
           branch_exit_native_result.instructions);
    assert(branch_exit_native.gpr[2] == 22U);
    assert(branch_exit_native.gpr[10] == 0x12345678U);
    assert(branch_exit_native_stats.native_side_exits != 0U);
    assert(branch_exit_native_stats.native_control_flows_executed >
           branch_exit_branches_before);
    assert(branch_exit_interpreted_stats.native_control_flows_executed == 0U);
    static_cast<void>(branch_exit_unused);

    const auto [branch_boundary_state, branch_boundary_result,
                branch_boundary_stats, branch_boundary_branches_before] =
        run_branch_exit(R3000ExecutionBackend::native_recompiler, true);
    assert(branch_boundary_result.reason == R3000StopReason::running);
    assert(branch_boundary_result.instructions == 1U);
    assert(branch_boundary_state.pc == branch_exit_address + 4U);
    assert(branch_boundary_state.next_pc == branch_exit_address + 16U);
    assert(branch_boundary_state.branch_pc == branch_exit_address);
    assert(branch_boundary_state.branch_delay_slot);
    assert(branch_boundary_stats.native_control_flows_executed ==
           branch_boundary_branches_before);

    const auto run_branch_delay_fault = [&] (
        R3000ExecutionBackend backend) {
        R3000Runtime runtime;
        runtime.setExecutionBackend(backend);
        assert(runtime.loadBytes(
            branch_exit_address,
            std::as_bytes(std::span{branch_exit_code})));
        assert(runtime.write32(branch_exit_data, 0x12345678U));
        const auto invoke = [&](std::uint32_t data_address) {
            runtime.reset(branch_exit_address, 0U, 0x801F0000U);
            runtime.setRegister(8U, data_address);
            runtime.setRegister(31U, R3000Runtime::return_sentinel);
            R3000ExecutionBoundaries boundaries;
            return runtime.runBatch(32U, boundaries);
        };
        if (backend == R3000ExecutionBackend::native_recompiler) {
            for (auto iteration = 0U; iteration != 20U; ++iteration) {
                const auto warm = invoke(branch_exit_data);
                assert(warm.reason == R3000StopReason::running);
                assert(runtime.atReturnSentinel());
            }
        }
        const auto result = invoke(branch_exit_data + 1U);
        return std::tuple{
            runtime.state(), result, runtime.recompilerStats()};
    };
    const auto [branch_fault_interpreted, branch_fault_interpreted_result,
                branch_fault_interpreted_stats] =
        run_branch_delay_fault(R3000ExecutionBackend::interpreter);
    const auto [branch_fault_native, branch_fault_native_result,
                branch_fault_native_stats] =
        run_branch_delay_fault(R3000ExecutionBackend::native_recompiler);
    assert(states_match(branch_fault_interpreted, branch_fault_native));
    assert(branch_fault_interpreted_result.reason ==
           R3000StopReason::alignment_fault);
    assert(branch_fault_native_result.reason ==
           branch_fault_interpreted_result.reason);
    assert(branch_fault_native_result.instructions ==
           branch_fault_interpreted_result.instructions);
    assert(branch_fault_native.branch_pc == branch_exit_address);
    assert(branch_fault_native.pc == branch_exit_address + 16U);
    assert(!branch_fault_native.branch_delay_slot);
    assert(branch_fault_native_stats.native_side_exits != 0U);
    assert(branch_fault_interpreted_stats.native_side_exits == 0U);

    // All three aligned store widths lower directly for ordinary RAM. A
    // scratchpad target side-exits before each store, and an installed write
    // sink keeps stores on the portable path so diagnostics see every value.
    constexpr std::uint32_t store_width_address = 0x80015000U;
    constexpr std::uint32_t store_width_data = 0x80017000U;
    constexpr std::uint32_t store_width_scratchpad = 0x1F800000U;
    const std::array store_width_code{
        encodeI(0x09, 14, 14, 1),        // addiu $t6, $t6, 1
        encodeI(0x28, 8, 9, 0),          // sb    $t1, 0($t0)
        encodeI(0x09, 14, 14, 1),        // addiu $t6, $t6, 1
        encodeI(0x29, 8, 10, 2),         // sh    $t2, 2($t0)
        encodeI(0x09, 14, 14, 1),        // addiu $t6, $t6, 1
        encodeI(0x2B, 8, 11, 4),         // sw    $t3, 4($t0)
        encodeI(0x09, 12, 12, 1),        // addiu $t4, $t4, 1
        encodeI(0x0B, 12, 13, 100),      // sltiu $t5, $t4, 100
        encodeI(0x05, 13, 0, 0xfff7),    // bne   $t5, $zero, loop
        std::uint32_t{0},
        encodeR(31, 0, 0, 0, 0x08),
        std::uint32_t{0},
    };
    const auto run_store_widths = [&] (
        R3000ExecutionBackend backend,
        std::uint32_t data_address,
        bool trace_writes = false) {
        R3000Runtime runtime;
        runtime.setExecutionBackend(backend);
        assert(runtime.loadBytes(
            store_width_address,
            std::as_bytes(std::span{store_width_code})));
        std::uint64_t sink_writes = 0U;
        if (trace_writes) {
            runtime.setMemoryWriteSink(
                [&](std::uint32_t, std::uint32_t, std::uint32_t,
                    std::uint32_t) { ++sink_writes; });
        }
        runtime.reset(store_width_address, 0U, 0x801F0000U);
        runtime.setRegister(8U, data_address);
        runtime.setRegister(9U, 0xA1B2C3D4U);
        runtime.setRegister(10U, 0x55667788U);
        runtime.setRegister(11U, 0x99AABBCCU);
        runtime.setRegister(31U, R3000Runtime::return_sentinel);
        R3000ExecutionBoundaries boundaries;
        const auto result = runtime.runBatch(4'000U, boundaries);
        assert(result.reason == R3000StopReason::running);
        assert(runtime.atReturnSentinel());
        std::array<std::byte, 8U> stored{};
        assert(runtime.copyBytes(data_address, stored));
        return std::tuple{runtime.state(), result.instructions, stored,
                          runtime.recompilerStats(), sink_writes};
    };
    const std::array expected_store_bytes{
        std::byte{0xD4}, std::byte{0x00}, std::byte{0x88}, std::byte{0x77},
        std::byte{0xCC}, std::byte{0xBB}, std::byte{0xAA}, std::byte{0x99}};
    const auto [width_interpreted, width_interpreted_count,
                width_interpreted_bytes, width_interpreted_stats,
                width_interpreted_sink] =
        run_store_widths(
            R3000ExecutionBackend::interpreter, store_width_data);
    const auto [width_native, width_native_count, width_native_bytes,
                width_native_stats, width_native_sink] =
        run_store_widths(
            R3000ExecutionBackend::native_recompiler, store_width_data);
    assert(states_match(width_interpreted, width_native));
    assert(width_interpreted_count == width_native_count);
    assert(width_interpreted_bytes == expected_store_bytes);
    assert(width_native_bytes == width_interpreted_bytes);
    assert(width_native_stats.native_stores_executed != 0U);
    assert(width_interpreted_stats.native_stores_executed == 0U);
    assert(width_interpreted_sink == 0U && width_native_sink == 0U);

    // Consecutive stores have no ALU prefix to satisfy the original native
    // region's two-instruction minimum. Each width can stand alone because a
    // successful store already returns to the host for code-page invalidation.
    constexpr std::uint32_t isolated_store_address = 0x80020000U;
    constexpr std::uint32_t isolated_store_data = 0x80021000U;
    const std::array isolated_store_code{
        encodeI(0x28, 8, 9, 0),          // sb $t1, 0($t0)
        encodeI(0x29, 8, 10, 2),         // sh $t2, 2($t0)
        encodeI(0x2B, 8, 11, 4),         // sw $t3, 4($t0)
        encodeR(31, 0, 0, 0, 0x08),      // jr $ra
        std::uint32_t{0},
    };
    const auto run_isolated_stores = [&] (
        R3000ExecutionBackend backend, bool warm) {
        R3000Runtime runtime;
        runtime.setExecutionBackend(backend);
        assert(runtime.loadBytes(
            isolated_store_address,
            std::as_bytes(std::span{isolated_store_code})));
        const auto invoke = [&] {
            runtime.reset(isolated_store_address, 0U, 0x801F0000U);
            runtime.setRegister(8U, isolated_store_data);
            runtime.setRegister(9U, 0xA1B2C3D4U);
            runtime.setRegister(10U, 0x55667788U);
            runtime.setRegister(11U, 0x99AABBCCU);
            runtime.setRegister(31U, R3000Runtime::return_sentinel);
            R3000ExecutionBoundaries boundaries;
            const auto result = runtime.runBatch(16U, boundaries);
            assert(result.reason == R3000StopReason::running);
            assert(runtime.atReturnSentinel());
            return result.instructions;
        };
        if (warm) {
            for (auto iteration = 0U; iteration != 20U; ++iteration) {
                static_cast<void>(invoke());
            }
        }
        const auto count = invoke();
        std::array<std::byte, 8U> stored{};
        assert(runtime.copyBytes(isolated_store_data, stored));
        return std::tuple{
            runtime.state(), count, stored, runtime.recompilerStats()};
    };
    const auto [isolated_interpreted, isolated_interpreted_count,
                isolated_interpreted_bytes, isolated_interpreted_stats] =
        run_isolated_stores(R3000ExecutionBackend::interpreter, false);
    const auto [isolated_native, isolated_native_count,
                isolated_native_bytes, isolated_native_stats] =
        run_isolated_stores(R3000ExecutionBackend::native_recompiler, true);
    assert(states_match(isolated_interpreted, isolated_native));
    assert(isolated_interpreted_count == isolated_native_count);
    assert(isolated_interpreted_bytes == expected_store_bytes);
    assert(isolated_native_bytes == isolated_interpreted_bytes);
    assert(isolated_native_stats.native_stores_executed >= 3U);
    assert(isolated_interpreted_stats.native_stores_executed == 0U);

    const auto [width_scratch_interpreted, width_scratch_interpreted_count,
                width_scratch_interpreted_bytes,
                width_scratch_interpreted_stats,
                width_scratch_interpreted_sink] =
        run_store_widths(
            R3000ExecutionBackend::interpreter, store_width_scratchpad);
    const auto [width_scratch_native, width_scratch_native_count,
                width_scratch_native_bytes, width_scratch_native_stats,
                width_scratch_native_sink] =
        run_store_widths(
            R3000ExecutionBackend::native_recompiler,
            store_width_scratchpad);
    assert(states_match(width_scratch_interpreted, width_scratch_native));
    assert(width_scratch_interpreted_count == width_scratch_native_count);
    assert(width_scratch_interpreted_bytes == expected_store_bytes);
    assert(width_scratch_native_bytes == width_scratch_interpreted_bytes);
    assert(width_scratch_native_stats.native_stores_executed == 0U);
    assert(width_scratch_native_stats.native_side_exits != 0U);
    assert(width_scratch_interpreted_stats.native_side_exits == 0U);
    assert(width_scratch_interpreted_sink == 0U &&
           width_scratch_native_sink == 0U);

    const auto [width_traced, width_traced_count, width_traced_bytes,
                width_traced_stats, width_traced_sink] =
        run_store_widths(
            R3000ExecutionBackend::native_recompiler,
            store_width_data, true);
    assert(states_match(width_interpreted, width_traced));
    assert(width_interpreted_count == width_traced_count);
    assert(width_traced_bytes == expected_store_bytes);
    assert(width_traced_stats.native_stores_executed == 0U);
    assert(width_traced_sink == 300U);

    const auto run_unaligned_store = [&] (R3000ExecutionBackend backend) {
        R3000Runtime runtime;
        runtime.setExecutionBackend(backend);
        assert(runtime.loadBytes(
            store_width_address,
            std::as_bytes(std::span{store_width_code})));
        if (backend == R3000ExecutionBackend::native_recompiler) {
            // Heat and lower the store regions with an aligned target first.
            runtime.reset(store_width_address, 0U, 0x801F0000U);
            runtime.setRegister(8U, store_width_data);
            runtime.setRegister(9U, 0xA1B2C3D4U);
            runtime.setRegister(10U, 0x55667788U);
            runtime.setRegister(11U, 0x99AABBCCU);
            runtime.setRegister(31U, R3000Runtime::return_sentinel);
            R3000ExecutionBoundaries warm_boundaries;
            const auto warm = runtime.runBatch(4'000U, warm_boundaries);
            assert(warm.reason == R3000StopReason::running);
            assert(runtime.atReturnSentinel());
            assert(runtime.recompilerStats().native_stores_executed != 0U);
        }
        const std::array<std::byte, 8U> zeroes{};
        assert(runtime.loadBytes(store_width_data, zeroes));
        runtime.reset(store_width_address, 0U, 0x801F0000U);
        runtime.setRegister(8U, store_width_data + 1U);
        runtime.setRegister(9U, 0xA1B2C3D4U);
        runtime.setRegister(10U, 0x55667788U);
        runtime.setRegister(11U, 0x99AABBCCU);
        runtime.setRegister(31U, R3000Runtime::return_sentinel);
        R3000ExecutionBoundaries boundaries;
        const auto result = runtime.runBatch(32U, boundaries);
        std::array<std::byte, 8U> stored{};
        assert(runtime.copyBytes(store_width_data, stored));
        return std::tuple{runtime.state(), result, stored,
                          runtime.recompilerStats()};
    };
    const auto [unaligned_interpreted, unaligned_interpreted_result,
                unaligned_interpreted_bytes, unaligned_interpreted_stats] =
        run_unaligned_store(R3000ExecutionBackend::interpreter);
    const auto [unaligned_native, unaligned_native_result,
                unaligned_native_bytes, unaligned_native_stats] =
        run_unaligned_store(R3000ExecutionBackend::native_recompiler);
    assert(states_match(unaligned_interpreted, unaligned_native));
    assert(unaligned_interpreted_result.reason ==
           R3000StopReason::alignment_fault);
    assert(unaligned_native_result.reason ==
           unaligned_interpreted_result.reason);
    assert(unaligned_native_result.instructions ==
           unaligned_interpreted_result.instructions);
    assert(unaligned_native_bytes == unaligned_interpreted_bytes);
    assert(unaligned_native_bytes[1] == std::byte{0xD4});
    assert(unaligned_native_stats.native_side_exits != 0U);
    assert(unaligned_interpreted_stats.native_side_exits == 0U);

    // Scratchpad is intentionally outside the native tier's direct-RAM
    // contract. The generated load must side-exit after its ALU prefix, then
    // let the portable memory helper complete the instruction and its delay.
    constexpr std::uint32_t scratchpad_data = 0x1f800000U;
    const auto [scratch_interpreted, scratch_interpreted_count,
                scratch_interpreted_store, scratch_interpreted_stats] =
        run_native_loop(
            R3000ExecutionBackend::interpreter, scratchpad_data);
    const auto [scratch_recompiled, scratch_recompiled_count,
                scratch_recompiled_store, scratch_stats] =
        run_native_loop(
            R3000ExecutionBackend::native_recompiler, scratchpad_data);
    assert(states_match(scratch_interpreted, scratch_recompiled));
    assert(scratch_interpreted_count == scratch_recompiled_count);
    assert(scratch_interpreted_store == scratch_recompiled_store);
    assert(scratch_stats.native_side_exits != 0U);
    assert(scratch_interpreted_stats.native_side_exits == 0U);

    // The store rewrites an instruction already present in the active block.
    // The recompiler must stop using that block immediately, translate the new
    // bytes, and execute 77 rather than the stale cached immediate 1.
    constexpr std::uint32_t modifying_address = 0x80011000U;
    constexpr auto replacement = encodeI(0x09, 0, 2, 77);
    const std::array self_modifying{
        encodeI(0x0F, 0, 8, 0x8001),
        encodeI(0x0D, 8, 8, 0x1000),
        encodeI(0x0F, 0, 9, static_cast<std::uint16_t>(replacement >> 16U)),
        encodeI(0x0D, 9, 9, static_cast<std::uint16_t>(replacement)),
        encodeI(0x2B, 8, 9, 24),        // rewrite instruction at +24
        std::uint32_t{0},
        encodeI(0x09, 0, 2, 1),         // becomes addiu $v0, $zero, 77
        encodeR(31, 0, 0, 0, 0x08),
        std::uint32_t{0},
    };
    R3000Runtime modifying;
    assert(modifying.loadBytes(
        modifying_address, std::as_bytes(std::span{self_modifying})));
    modifying.reset(modifying_address, 0U, 0x801F0000U);
    modifying.setRegister(31U, R3000Runtime::return_sentinel);
    R3000ExecutionBoundaries no_boundaries;
    const auto modified = modifying.runBatch(64U, no_boundaries);
    assert(modified.reason == R3000StopReason::running);
    assert(modifying.atReturnSentinel());
    assert(modifying.state().gpr[2] == 77U);
    assert(modifying.recompilerStats().cache_invalidations != 0U);
    assert(modifying.recompilerStats().blocks_compiled >= 2U);

    // A hot native writer targeting a different cached code page reports the
    // exact store address to the host. The target's generation changes before
    // it can be dispatched again, so its replacement immediate is observed.
    constexpr std::uint32_t native_target_address = 0x80019000U;
    constexpr std::uint32_t native_writer_address = 0x8001A000U;
    const std::array native_target_code{
        encodeI(0x09, 0, 2, 1),
        encodeR(31, 0, 0, 0, 0x08),
        std::uint32_t{0},
    };
    const std::array native_writer_code{
        encodeI(0x09, 12, 12, 1),       // addiu $t4, $t4, 1
        encodeI(0x2B, 8, 9, 0),         // sw    $t1, 0($t0)
        encodeI(0x0B, 12, 13, 100),     // sltiu $t5, $t4, 100
        encodeI(0x05, 13, 0, 0xfffc),   // bne   $t5, $zero, loop
        std::uint32_t{0},
        encodeR(31, 0, 0, 0, 0x08),
        std::uint32_t{0},
    };
    R3000Runtime native_modifying;
    native_modifying.setExecutionBackend(
        R3000ExecutionBackend::native_recompiler);
    assert(native_modifying.loadBytes(
        native_target_address,
        std::as_bytes(std::span{native_target_code})));
    assert(native_modifying.loadBytes(
        native_writer_address,
        std::as_bytes(std::span{native_writer_code})));

    // Fetching the target once marks its page as translated code.
    native_modifying.reset(native_target_address, 0U, 0x801F0000U);
    native_modifying.setRegister(31U, R3000Runtime::return_sentinel);
    auto native_modified =
        native_modifying.runBatch(16U, no_boundaries);
    assert(native_modified.reason == R3000StopReason::running);
    assert(native_modifying.atReturnSentinel());
    assert(native_modifying.state().gpr[2] == 1U);

    native_modifying.reset(native_writer_address, 0U, 0x801F0000U);
    native_modifying.setRegister(8U, native_target_address);
    native_modifying.setRegister(9U, replacement);
    native_modifying.setRegister(31U, R3000Runtime::return_sentinel);
    native_modified = native_modifying.runBatch(2'000U, no_boundaries);
    assert(native_modified.reason == R3000StopReason::running);
    assert(native_modifying.atReturnSentinel());
    assert(native_modifying.recompilerStats().native_stores_executed != 0U);
    assert(native_modifying.recompilerStats().cache_invalidations != 0U);

    native_modifying.reset(native_target_address, 0U, 0x801F0000U);
    native_modifying.setRegister(31U, R3000Runtime::return_sentinel);
    native_modified = native_modifying.runBatch(16U, no_boundaries);
    assert(native_modified.reason == R3000StopReason::running);
    assert(native_modifying.atReturnSentinel());
    assert(native_modifying.state().gpr[2] == 77U);

    // A quick-save candidate copies architectural machine state, never stale
    // translations or diagnostic counters from the abandoned host timeline.
    const R3000Runtime copied = modifying;
    assert(copied.state().gpr == modifying.state().gpr);
    assert(copied.recompilerStats().blocks_compiled == 0U);
}

void gteNormalColorSingle() {
    stuntmaster::psx::GteState state;
    constexpr std::uint32_t identity_diagonal = 0x00001000U;
    state.control[8] = identity_diagonal;
    state.control[10] = identity_diagonal;
    state.control[12] = identity_diagonal;
    state.control[16] = identity_diagonal;
    state.control[18] = identity_diagonal;
    state.control[20] = identity_diagonal;
    state.data[0] = 0x10001000U;
    state.data[1] = 0x00001000U;
    state.data[6] = 0xAA040302U;
    assert(stuntmaster::psx::GteRuntime::executeCommand(
        state, 0x4B08041BU));
    assert(state.data[22] == 0xAA040302U);
    assert(state.control[31] == 0U);
}

void biosCompatibilityDataIsReadOnly() {
    stuntmaster::psx::R3000Runtime runtime;
    std::uint32_t value = 1;
    assert(runtime.read32(0xBFC01000U, value));
    assert(value == 0);
    assert(!runtime.write32(0xBFC01000U, 0x12345678U));
}

void gpuMmioSeparatesCommandsFromStatus() {
    stuntmaster::psx::R3000Runtime runtime;
    std::uint32_t linked_list_begins{};
    stuntmaster::psx::GpuCommandBridge gpu{
        {},
        &runtime,
        [&linked_list_begins] { ++linked_list_begins; }};
    runtime.attachMmioBus(&gpu);
    assert(runtime.write32(0x1F801814U, 0x10000007U));
    std::uint32_t status{};
    assert(runtime.read32(0x1F801814U, status));
    assert(status != 0x10000007U);
    assert((status & 0x04000000U) != 0U);
    assert(gpu.commandCount() == 1U);
    assert(gpu.lastCommandWasGp1());
    assert(runtime.write32(0x1F801814U, 0x08000002U));
    assert(gpu.displayMode() == 2U);
    assert(gpu.displayWidth() == 512U);
    assert(gpu.displayHeight() == 240U);
    assert(runtime.write32(0x1F8010A8U, 0x01000401U));
    std::uint32_t dma_control{};
    assert(runtime.read32(0x1F8010A8U, dma_control));
    assert((dma_control & 0x01000000U) == 0U);
    assert(gpu.takeDma2Completion());
    assert(!gpu.takeDma2Completion());
    linked_list_begins = 0U;
    assert(runtime.write32(
        0x80002000U, 0x02FFFFFFU));
    assert(runtime.write32(0x80002004U, 0xE1000200U));
    assert(runtime.write32(0x80002008U, 0x020000FFU));
    assert(runtime.write32(0x1F8010A0U, 0x00002000U));
    assert(runtime.write32(0x1F8010A8U, 0x01000401U));
    assert(gpu.commandCount() == 4U);
    assert(gpu.lastCommand() == 0x020000FFU);
    assert(linked_list_begins == 1U);
    assert(gpu.takeDma2Completion());
    assert(!gpu.takeDma2Completion());
    assert(runtime.write32(0x1F8010E0U, 0x00001008U));
    assert(runtime.write32(0x1F8010E4U, 3U));
    assert(runtime.write32(0x1F8010E8U, 0x11000002U));
    assert(runtime.read32(0x1F8010E8U, dma_control));
    assert((dma_control & 0x01000000U) == 0U);
    std::uint32_t otc_link{};
    assert(runtime.read32(0x80001008U, otc_link));
    assert(otc_link == 0x00001004U);
    assert(runtime.read32(0x80001000U, otc_link));
    assert(otc_link == 0x00FFFFFFU);
    assert(runtime.write32(0x1F8010C0U, 0x00123456U));
    assert(runtime.write32(0x1F8010C4U, 0x00020010U));
    assert(runtime.write32(0x1F8010C8U, 0x01000201U));
    assert(runtime.read32(0x1F8010C8U, dma_control));
    assert((dma_control & 0x01000000U) == 0U);
    assert(gpu.takeDma4Completion());
    assert(!gpu.takeDma4Completion());
}

void gpuCommandDecoderPacketsAndVram() {
    std::vector<std::vector<std::uint32_t>> packets;
    stuntmaster::psx::GpuCommandDecoder decoder{
        [&packets](std::span<const std::uint32_t> packet) {
            packets.emplace_back(packet.begin(), packet.end());
        }};

    decoder.pushGp0(0xE1000200U);
    assert(decoder.packetCount() == 1U);
    assert(decoder.environmentCount() == 1U);

    const std::array textured_quad{
        0x2CFFFFFFU,
        0x000A0014U, 0x00010002U,
        0x001E0028U, 0x00030004U,
        0x0032003CU, 0x00050006U,
        0x00460050U, 0x00070008U,
    };
    for (const auto word : textured_quad) {
        decoder.pushGp0(word);
    }
    assert(decoder.packetCount() == 2U);
    assert(decoder.primitiveCount() == 1U);
    assert(packets.back().size() == textured_quad.size());

    decoder.pushGp0(0xA0000000U);
    decoder.pushGp0(0x00050007U);
    decoder.pushGp0(0x00020002U);
    decoder.pushGp0(0x22221111U);
    decoder.pushGp0(0x44443333U);
    assert(decoder.imageUploadCount() == 1U);
    assert(decoder.imagePixelCount() == 4U);
    assert(decoder.vramRevision() == 1U);
    const auto upload = decoder.lastImageUpload();
    assert(upload.x == 7U && upload.y == 5U);
    assert(upload.width == 2U && upload.height == 2U);
    assert(!decoder.awaitingImageData());
    const auto vram = decoder.vram();
    const auto first = 5U * stuntmaster::psx::GpuCommandDecoder::vram_width + 7U;
    assert(vram[first] == 0x1111U);
    assert(vram[first + 1U] == 0x2222U);
    assert(vram[first + stuntmaster::psx::GpuCommandDecoder::vram_width] ==
           0x3333U);
    assert(vram[
        first + stuntmaster::psx::GpuCommandDecoder::vram_width + 1U] ==
           0x4444U);

    // Copies are refused outright unless the framebuffer composite asks for
    // them, so the default build keeps the behaviour every other screen was
    // validated against.
    decoder.pushGp0(0x80000000U);
    decoder.pushGp0(0x00050007U);
    decoder.pushGp0(0x000A000BU);
    decoder.pushGp0(0x00020002U);
    assert(decoder.vramCopyCount() == 1U);
    assert(decoder.vramCopiesApplied() == 0U);
    assert(decoder.vramRevision() == 1U);
    assert(vram[10U * stuntmaster::psx::GpuCommandDecoder::vram_width + 11U] ==
           0U);

    // A copy whose source is entirely upload-backed moves bytes this decoder
    // knows the provenance of, so it is applied. Retail needs it: the loading
    // screen uploads its background into one display page and MoveImages it
    // into the other.
    decoder.setApplyVramCopies(true);
    decoder.pushGp0(0x80000000U);
    decoder.pushGp0(0x00050007U);
    decoder.pushGp0(0x000A000BU);
    decoder.pushGp0(0x00020002U);
    assert(decoder.vramCopyCount() == 2U);
    assert(decoder.vramCopiesApplied() == 1U);
    assert(decoder.vramRevision() == 2U);
    const auto copy = decoder.lastVramCopy();
    assert(copy.source_x == 7U && copy.source_y == 5U);
    assert(copy.destination_x == 11U && copy.destination_y == 10U);
    assert(copy.width == 2U && copy.height == 2U);
    const auto copied =
        10U * stuntmaster::psx::GpuCommandDecoder::vram_width + 11U;
    assert(vram[copied] == 0x1111U);
    assert(vram[copied + 1U] == 0x2222U);
    assert(vram[
        copied + stuntmaster::psx::GpuCommandDecoder::vram_width] ==
           0x3333U);
    assert(vram[
        copied + stuntmaster::psx::GpuCommandDecoder::vram_width + 1U] ==
           0x4444U);
    // The destination is now known too, so it may be a source in turn.
    assert(decoder.uploadCovers(11U, 10U, 2U, 2U));

    // A copy reading a region no upload ever wrote is reading whatever the
    // guest rasterized there, which this decoder does not model. Refusing it
    // is what stopped a retail MoveImage from moving stale bytes over the
    // level-one torii's facade tile.
    decoder.pushGp0(0x80000000U);
    decoder.pushGp0(0x00C800C8U);
    decoder.pushGp0(0x012C012CU);
    decoder.pushGp0(0x00020002U);
    assert(decoder.vramCopyCount() == 3U);
    assert(decoder.vramCopiesApplied() == 1U);
    assert(decoder.vramRevision() == 2U);
    const auto refused =
        300U * stuntmaster::psx::GpuCommandDecoder::vram_width + 300U;
    assert(vram[refused] == 0U);
    assert(!decoder.uploadCovers(300U, 300U, 2U, 2U));

}

void gpuCommandDecoderUploadSinkPayload() {
    // The persistent-framebuffer presenter needs the upload pixel payload,
    // which the packet stream lacks. The upload sink delivers it once per
    // finished transfer, in stream order, row-major, exactly as written.
    struct CapturedUpload {
        stuntmaster::psx::GpuImageUpload rect;
        std::vector<std::uint16_t> pixels;
    };
    std::vector<CapturedUpload> uploads;
    stuntmaster::psx::GpuCommandDecoder decoder;
    decoder.setUploadSink(
        [&uploads](
            const stuntmaster::psx::GpuImageUpload& rect,
            std::span<const std::uint16_t> pixels) {
            uploads.push_back(
                {rect, {pixels.begin(), pixels.end()}});
        });

    // A 2x2 upload at (7,5): two 32-bit words carry four 16-bit pixels, low
    // half first, matching gpuCommandDecoderPacketsAndVram.
    decoder.pushGp0(0xA0000000U);
    decoder.pushGp0(0x00050007U);
    decoder.pushGp0(0x00020002U);
    // No payload has arrived mid-transfer, so the sink has not fired.
    decoder.pushGp0(0x22221111U);
    assert(uploads.empty());
    decoder.pushGp0(0x44443333U);

    assert(uploads.size() == 1U);
    assert(uploads.front().rect.x == 7U && uploads.front().rect.y == 5U);
    assert(uploads.front().rect.width == 2U &&
           uploads.front().rect.height == 2U);
    const std::array<std::uint16_t, 4U> expected{
        0x1111U, 0x2222U, 0x3333U, 0x4444U};
    assert(uploads.front().pixels.size() == expected.size());
    assert(std::equal(
        uploads.front().pixels.begin(),
        uploads.front().pixels.end(),
        expected.begin()));

    // A second upload fires the sink again, in order, and does not disturb the
    // first record.
    decoder.pushGp0(0xA0000000U);
    decoder.pushGp0(0x000A000BU);
    decoder.pushGp0(0x00010001U);
    decoder.pushGp0(0x0000BEEFU);
    assert(uploads.size() == 2U);
    assert(uploads.back().rect.x == 11U && uploads.back().rect.y == 10U);
    assert(uploads.back().pixels.size() == 1U);
    assert(uploads.back().pixels.front() == 0xBEEFU);
}

void machineSubsystemStateArchivesRoundTrip() {
    using stuntmaster::core::StateReader;
    using stuntmaster::core::StateWriter;

    stuntmaster::psx::R3000Runtime runtime;
    runtime.reset(0x80012340U, 0x11223344U, 0x801FF000U);
    assert(runtime.write32(0x80001000U, 0xDEADBEEFU));
    StateWriter runtime_writer;
    runtime.writeState(runtime_writer);
    stuntmaster::psx::R3000Runtime restored_runtime;
    StateReader runtime_reader{runtime_writer.data()};
    assert(restored_runtime.readState(runtime_reader));
    assert(runtime_reader.finished());
    StateWriter runtime_again;
    restored_runtime.writeState(runtime_again);
    assert(runtime_again.data() == runtime_writer.data());

    stuntmaster::psx::GpuCommandDecoder decoder;
    decoder.pushGp0(0xA0000000U);
    decoder.pushGp0(0x00020001U);
    decoder.pushGp0(0x00010002U);
    decoder.pushGp0(0x56781234U);
    StateWriter decoder_writer;
    decoder.writeState(decoder_writer);
    stuntmaster::psx::GpuCommandDecoder restored_decoder;
    StateReader decoder_reader{decoder_writer.data()};
    assert(restored_decoder.readState(decoder_reader));
    assert(decoder_reader.finished());
    StateWriter decoder_again;
    restored_decoder.writeState(decoder_again);
    assert(decoder_again.data() == decoder_writer.data());

    stuntmaster::psx::GpuCommandBridge bridge{{}, &runtime};
    assert(bridge.writeMmio(
        0x1F801814U,
        stuntmaster::psx::R3000AccessWidth::word,
        0x05012345U));
    StateWriter bridge_writer;
    bridge.writeState(bridge_writer);
    stuntmaster::psx::GpuCommandBridge restored_bridge;
    StateReader bridge_reader{bridge_writer.data()};
    assert(restored_bridge.readState(bridge_reader));
    assert(bridge_reader.finished());
    StateWriter bridge_again;
    restored_bridge.writeState(bridge_again);
    assert(bridge_again.data() == bridge_writer.data());

    stuntmaster::psx::Spu spu;
    assert(spu.writeMmio(
        stuntmaster::psx::Spu::register_base + 0x180U,
        stuntmaster::psx::R3000AccessWidth::halfword,
        0x2345U));
    StateWriter spu_writer;
    spu.writeState(spu_writer);
    stuntmaster::psx::Spu restored_spu;
    StateReader spu_reader{spu_writer.data()};
    assert(restored_spu.readState(spu_reader));
    assert(spu_reader.finished());
    StateWriter spu_again;
    restored_spu.writeState(spu_again);
    assert(spu_again.data() == spu_writer.data());

    stuntmaster::game::RetailHle hle;
    hle.setCdReadSpeed(16U);
    hle.setVblankRate(90U);
    hle.setCdPendingSectors(123U);
    StateWriter hle_writer;
    hle.writeState(hle_writer);
    stuntmaster::game::RetailHle restored_hle;
    StateReader hle_reader{hle_writer.data()};
    assert(restored_hle.readState(hle_reader));
    assert(hle_reader.finished());
    StateWriter hle_again;
    restored_hle.writeState(hle_again);
    assert(hle_again.data() == hle_writer.data());

    stuntmaster::psx::BiosHle bios;
    StateWriter bios_writer;
    bios.writeState(bios_writer);
    stuntmaster::psx::BiosHle restored_bios;
    StateReader bios_reader{bios_writer.data()};
    assert(restored_bios.readState(bios_reader));
    assert(bios_reader.finished());
    StateWriter bios_again;
    restored_bios.writeState(bios_again);
    assert(bios_again.data() == bios_writer.data());
}


void spuRegistersAndSoundRam() {
    stuntmaster::psx::Spu spu;

    // Voice registers are halfword ports. Retail writes them individually, so
    // a round trip through the decode is the thing to lock.
    constexpr std::uint32_t voice_two = stuntmaster::psx::Spu::register_base +
        2U * 16U;
    assert(spu.writeMmio(
        voice_two, stuntmaster::psx::R3000AccessWidth::halfword, 0x1234U));
    assert(spu.writeMmio(
        voice_two + 4U,
        stuntmaster::psx::R3000AccessWidth::halfword,
        0x0800U));
    assert(spu.voice(2U).volume_left == 0x1234U);
    assert(spu.voice(2U).sample_rate == 0x0800U);

    // A word write covers the halfword pair, low half first.
    assert(spu.writeMmio(
        voice_two, stuntmaster::psx::R3000AccessWidth::word, 0xAAAA5555U));
    assert(spu.voice(2U).volume_left == 0x5555U);
    assert(spu.voice(2U).volume_right == 0xAAAAU);

    // Key on is an edge: it accumulates until something consumes it, and the
    // consumer clears it. A later write must not drop a voice that has not
    // been serviced yet.
    constexpr std::uint32_t key_on = stuntmaster::psx::Spu::register_base +
        0x188U;
    assert(spu.writeMmio(
        key_on, stuntmaster::psx::R3000AccessWidth::halfword, 0x0004U));
    assert(spu.writeMmio(
        key_on, stuntmaster::psx::R3000AccessWidth::halfword, 0x0001U));
    assert(spu.keyOnCount() == 2U);
    assert(spu.takeKeyOn() == 0x0005U);
    assert(spu.takeKeyOn() == 0U);

    // The transfer address is in eight-byte units and reloads the running
    // destination, so a following DMA payload lands where the guest asked.
    constexpr std::uint32_t transfer_address =
        stuntmaster::psx::Spu::register_base + 0x1A6U;
    assert(spu.writeMmio(
        transfer_address,
        stuntmaster::psx::R3000AccessWidth::halfword,
        0x0100U));
    assert(spu.transferAddress() == 0x0800U);
    const std::array<std::uint16_t, 2U> samples{0x3412U, 0x7856U};
    spu.writeSoundRam(samples);
    const auto ram = spu.soundRam();
    assert(ram[0x0800U] == 0x12U);
    assert(ram[0x0801U] == 0x34U);
    assert(ram[0x0802U] == 0x56U);
    assert(ram[0x0803U] == 0x78U);
    assert(spu.transferAddress() == 0x0804U);
    assert(spu.soundRamBytesWritten() == 4U);

    // Addresses outside the SPU are refused, so the bus chain can offer them
    // elsewhere rather than this swallowing them.
    std::uint32_t value{};
    assert(!spu.writeMmio(
        0x1F801810U, stuntmaster::psx::R3000AccessWidth::word, 0U));
    assert(!spu.readMmio(
        0x1F801810U, stuntmaster::psx::R3000AccessWidth::word, value));
}


void spuAdpcmDecodeAndMix() {
    stuntmaster::psx::Spu spu;

    // One ADPCM block at byte 0x1000, which is unit 0x200. Shift zero scales a
    // nibble by 4096 and filter zero disables the predictor, so the decoded
    // samples are exactly the sign-extended nibbles shifted up -- the one case
    // where the expected output can be written down rather than measured.
    constexpr std::uint32_t transfer_address =
        stuntmaster::psx::Spu::register_base + 0x1A6U;
    assert(spu.writeMmio(
        transfer_address,
        stuntmaster::psx::R3000AccessWidth::halfword,
        0x0200U));
    // Header: shift 0, filter 0. Flags: none. The first three nibbles are 4, so
    // the attack ramp is visible across them; nibbles 3 to 6 are 1, 2, 3 and 4
    // and are read once the envelope has reached its maximum.
    // Every later nibble is 4 as well, so the release is audible over a
    // constant sample rather than over silence.
    const std::array<std::uint16_t, 8U> block{
        0x0000U, 0x1444U, 0x4432U, 0x4444U, 0x4444U, 0x4444U, 0x4444U,
        0x4444U};
    spu.writeSoundRam(block);

    const auto voice_register = [](std::uint32_t offset) {
        return stuntmaster::psx::Spu::register_base + offset;
    };
    // Volume 0x2000 is a level of exactly 0x4000, so each stage halves and the
    // arithmetic stays exact.
    assert(spu.writeMmio(
        voice_register(0x0U),
        stuntmaster::psx::R3000AccessWidth::halfword,
        0x2000U));
    assert(spu.writeMmio(
        voice_register(0x2U),
        stuntmaster::psx::R3000AccessWidth::halfword,
        0x2000U));
    // 0x1000 is one output sample per input sample.
    assert(spu.writeMmio(
        voice_register(0x4U),
        stuntmaster::psx::R3000AccessWidth::halfword,
        0x1000U));
    assert(spu.writeMmio(
        voice_register(0x6U),
        stuntmaster::psx::R3000AccessWidth::halfword,
        0x0200U));
    assert(spu.writeMmio(
        stuntmaster::psx::Spu::register_base + 0x180U,
        stuntmaster::psx::R3000AccessWidth::halfword,
        0x2000U));
    assert(spu.writeMmio(
        stuntmaster::psx::Spu::register_base + 0x182U,
        stuntmaster::psx::R3000AccessWidth::halfword,
        0x2000U));
    // Fastest attack and decay, and a sustain level above the envelope's
    // maximum so decay ends at once and the note holds at full.
    assert(spu.writeMmio(
        voice_register(0x8U),
        stuntmaster::psx::R3000AccessWidth::halfword,
        0x000FU));

    // Nothing plays until the voice is keyed on, however configured it is.
    std::array<std::int16_t, 8U> silent{};
    spu.mix(silent);
    assert(spu.activeVoices() == 0U);
    for (const auto sample : silent) {
        assert(sample == 0);
    }

    assert(spu.writeMmio(
        stuntmaster::psx::Spu::register_base + 0x188U,
        stuntmaster::psx::R3000AccessWidth::halfword,
        0x0001U));
    std::array<std::int16_t, 14U> mixed{};
    spu.mix(mixed);
    assert(spu.activeVoices() == 1U);
    assert(spu.activeVoiceMask() == 0x000001U);
    // The first three frames are the attack ramping from silence over a
    // constant sample, so they rise.
    assert(mixed[0] == 0);
    assert(mixed[2] == 1792);
    assert(mixed[4] == 3584);
    // From the fourth the envelope is at its maximum of 0x7FFF, one short of
    // unity, so 4096, 8192, 12288 and 16384 lose a count before each volume
    // stage halves them.
    assert(mixed[6] == 1023 && mixed[7] == 1023);
    assert(mixed[8] == 2047 && mixed[9] == 2047);
    assert(mixed[10] == 3071 && mixed[11] == 3071);
    assert(mixed[12] == 4095 && mixed[13] == 4095);

    // Silent advancement must evolve exactly the same device state as mixing
    // and throwing the output away. This is the native-movie clock path.
    auto reference = spu;
    spu.advance(3U);
    std::array<std::int16_t, 6U> reference_discarded{};
    reference.mix(reference_discarded);
    std::array<std::int16_t, 4U> advanced_after{};
    std::array<std::int16_t, 4U> reference_after{};
    spu.mix(advanced_after);
    reference.mix(reference_after);
    assert(advanced_after == reference_after);

    // A key-off releases rather than cutting: the note fades and the voice is
    // finished only once the envelope reaches zero.
    assert(spu.writeMmio(
        stuntmaster::psx::Spu::register_base + 0x18CU,
        stuntmaster::psx::R3000AccessWidth::halfword,
        0x0001U));
    std::array<std::int16_t, 8U> after{};
    spu.mix(after);
    assert(after[0] != 0);
    assert(std::abs(after[2]) < std::abs(after[0]));
    assert(spu.activeVoices() == 0U);
    assert(spu.activeVoiceMask() == 0U);
}


void spuRepeatAddressRegisterMovesTheLoopPoint() {
    stuntmaster::psx::Spu spu;
    const auto reg = [](std::uint32_t offset) {
        return stuntmaster::psx::Spu::register_base + offset;
    };
    const auto put = [&](std::uint32_t offset, std::uint16_t value) {
        assert(spu.writeMmio(
            reg(offset),
            stuntmaster::psx::R3000AccessWidth::halfword,
            value));
    };
    const auto upload =
        [&](std::uint16_t unit, std::uint16_t header, std::uint16_t body) {
            put(0x1A6U, unit);
            const std::array<std::uint16_t, 8U> block{
                header, body, body, body, body, body, body, body};
            spu.writeSoundRam(block);
        };

    // Two blocks of constant samples. The first ends with loop-end and repeat
    // set, so playback jumps to the repeat address and carries on; shift zero
    // and filter zero make each nibble decode to itself scaled up.
    upload(0x0200U, 0x0300U, 0x1111U); // byte 0x1000, nibbles of 1
    upload(0x0400U, 0x0000U, 0x5555U); // byte 0x2000, nibbles of 5

    put(0x0U, 0x2000U);   // volume left, a level of exactly 0x4000
    put(0x2U, 0x2000U);   // volume right
    put(0x4U, 0x1000U);   // one output sample per input sample
    put(0x6U, 0x0200U);   // start at the first block
    put(0x180U, 0x2000U); // main volume left
    put(0x182U, 0x2000U); // main volume right
    put(0x8U, 0x000FU);   // fastest attack, sustain at full
    put(0x188U, 0x0001U);
    // Written after the key-on, which is both what hardware requires -- key-on
    // reloads the loop point from the start address -- and what retail does: it
    // keys the stream once and then points the loop at whichever half of the
    // buffer it has just refilled. The loop point here is the second block,
    // which is neither the start address nor anything a block flag names.
    put(0xEU, 0x0400U);

    std::array<std::int16_t, 60U> mixed{};
    spu.mix(mixed);
    // Twenty-eight samples of the first block, then the second -- not the first
    // again, which is what ignoring the repeat register produces. The first
    // three frames are the attack ramp.
    assert(mixed[2U * 3U] == 1023);
    assert(mixed[2U * 27U] == 1023);
    assert(mixed[2U * 28U] == 5119);
    assert(mixed[2U * 29U] == 5119);
}

void widescreenDarkOverlaysReachWindowEdges() {
    using stuntmaster::presentation::extendDarkOverlayToWidescreen;
    using stuntmaster::presentation::widescreenOverlayBounds;

    const auto wide = widescreenOverlayBounds(1280U, 720U);
    assert(wide.left == -85);
    assert(wide.right == 597);
    const auto original = widescreenOverlayBounds(960U, 720U);
    assert(original.left == 0);
    assert(original.right == 512);

    // A semi-transparent black TILE spanning the authored frame is the common
    // fullscreen-fade/bar representation. Preserve y/height and only widen x.
    std::vector<std::uint32_t> tile{
        0x62000000U,
        20U << 16U,
        (40U << 16U) | 512U,
    };
    assert(extendDarkOverlayToWidescreen(tile, 0, 0, 512U, wide));
    assert(static_cast<std::int16_t>(tile[1]) == -85);
    assert((tile[1] >> 16U) == 20U);
    assert((tile[2] & 0xFFFFU) == 682U);
    assert((tile[2] >> 16U) == 40U);

    // An ordinary dark HUD rectangle does not span the screen and is left
    // alone, as is every primitive in a 4:3 host window.
    std::vector<std::uint32_t> hud{
        0x60000000U,
        (10U << 16U) | 100U,
        (30U << 16U) | 200U,
    };
    const auto original_hud = hud;
    assert(!extendDarkOverlayToWidescreen(hud, 0, 0, 512U, wide));
    assert(hud == original_hud);
    assert(!extendDarkOverlayToWidescreen(tile, 0, 0, 512U, original));

    // Flat semi-transparent black quads are the other fade representation.
    std::vector<std::uint32_t> quad{
        0x2A000000U,
        0U,
        512U,
        240U << 16U,
        (240U << 16U) | 512U,
    };
    assert(extendDarkOverlayToWidescreen(quad, 0, 0, 512U, wide));
    // Compare the complete packed XY words. Release once sign-extended the
    // negative left edge into the upper half, replacing y with -1 and turning
    // this fullscreen quad into two visible diagonal triangles.
    assert(quad[1] == 0x0000FFABU);
    assert(quad[2] == 0x00000255U);
    assert(quad[3] == 0x00F0FFABU);
    assert(quad[4] == 0x00F00255U);

    // Retail animates fades and cinematic bars with a neutral grayscale
    // semi-transparent quad, including values far above near-black.
    auto gray_fade = quad;
    gray_fade[0] = 0x2A777777U;
    assert(extendDarkOverlayToWidescreen(
        gray_fade, 0, 0, 512U, wide));

    // Coloured and textured primitives are hard exclusions even when their
    // geometry spans the authored frame: scenery and UI art must never stretch.
    auto colored = quad;
    colored[0] = 0x2A204060U;
    assert(!extendDarkOverlayToWidescreen(colored, 0, 0, 512U, wide));
    auto opaque_gray = quad;
    opaque_gray[0] = 0x28777777U;
    assert(!extendDarkOverlayToWidescreen(
        opaque_gray, 0, 0, 512U, wide));
    std::vector<std::uint32_t> textured{
        0x2C000000U,
        0U,
        0U,
        512U,
        0U,
        240U << 16U,
        0U,
        (240U << 16U) | 512U,
        0U,
    };
    assert(!extendDarkOverlayToWidescreen(
        textured, 0, 0, 512U, wide));
}


void spuReverbRoutesThroughTheSoundRamWorkArea() {
    const auto configure = [](
        stuntmaster::psx::Spu& spu, bool routed, bool enabled) {
        const auto put = [&spu](std::uint32_t offset, std::uint16_t value) {
            assert(spu.writeMmio(
                stuntmaster::psx::Spu::register_base + offset,
                stuntmaster::psx::R3000AccessWidth::halfword,
                value));
        };

        // One non-repeating block of a constant positive sample. Its dry path
        // is finished after 28 frames, so any later output is reverb tail.
        put(0x1A6U, 0x0200U);
        const std::array<std::uint16_t, 8U> block{
            0x0100U, 0x4444U, 0x4444U, 0x4444U, 0x4444U, 0x4444U,
            0x4444U, 0x4444U};
        spu.writeSoundRam(block);
        put(0x000U, 0x2000U);
        put(0x002U, 0x2000U);
        put(0x004U, 0x1000U);
        put(0x006U, 0x0200U);
        put(0x008U, 0x000FU);
        put(0x180U, 0x2000U);
        put(0x182U, 0x2000U);

        // A deliberately simple reverb program: input passes through the IIR
        // and first comb path, with separate work slots for L and R.
        // mBASE 0xF000 reserves the final 32 KB of sound RAM.
        put(0x184U, 0x7FFFU);
        put(0x186U, 0x7FFFU);
        put(0x1A2U, 0xF000U);
        const auto reverb = [&put](std::size_t index, std::uint16_t value) {
            put(0x1C0U + static_cast<std::uint32_t>(index * 2U), value);
        };
        reverb(2U, 0x7FFFU);  // vIIR
        reverb(3U, 0x7FFFU);  // vCOMB1
        reverb(10U, 0x0000U); // mLSAME
        reverb(11U, 0x0001U); // mRSAME
        reverb(12U, 0x0000U); // mLCOMB1
        reverb(13U, 0x0001U); // mRCOMB1
        reverb(18U, 0x0002U); // mLDIFF
        reverb(19U, 0x0003U); // mRDIFF
        reverb(26U, 0x0000U); // mLAPF1
        reverb(27U, 0x0001U); // mRAPF1
        reverb(28U, 0x0000U); // mLAPF2
        reverb(29U, 0x0001U); // mRAPF2
        reverb(30U, 0x7FFFU); // vLIN
        reverb(31U, 0x7FFFU); // vRIN
        put(0x198U, routed ? 0x0001U : 0x0000U);
        put(0x1AAU, enabled ? 0x0080U : 0U);
        put(0x188U, 0x0001U);
    };

    stuntmaster::psx::Spu dry;
    stuntmaster::psx::Spu wet;
    stuntmaster::psx::Spu disabled;
    configure(dry, false, true);
    configure(wet, true, true);
    configure(disabled, true, false);
    std::array<std::int16_t, 512U> dry_mix{};
    std::array<std::int16_t, 512U> wet_mix{};
    std::array<std::int16_t, 512U> disabled_mix{};
    dry.mix(dry_mix);
    wet.mix(wet_mix);
    disabled.mix(disabled_mix);

    // Reverb is a send, not a replacement: the two mixes begin with the same
    // dry note. The fixed down/up-sampling filters make the first wet result the
    // right sample of frame 21. Locking that boundary and value catches a
    // one-frame phase error in either filter.
    const auto first_wet = std::mismatch(
        wet_mix.begin(), wet_mix.end(), dry_mix.begin()).first;
    assert(first_wet != wet_mix.end());
    assert(static_cast<std::size_t>(first_wet - wet_mix.begin()) == 43U);
    assert(*first_wet == 4094 && dry_mix[43U] == 4095);

    // Once the 28-frame source block is over, the dry mix is silent while the
    // FIR/IIR pipeline returns its delayed tail.
    assert(dry_mix[56U] == 0 && dry_mix[57U] == 0);
    assert(wet_mix[56U] == -2 && wet_mix[57U] == -3);
    assert(std::any_of(
        wet_mix.begin() + 28U * 2U,
        wet_mix.end(),
        [](std::int16_t sample) { return sample != 0; }));

    // The enabled unit writes inside its reserved final-32-KB work area. The
    // same locations remain untouched when no voice is routed to the unit.
    const auto dry_ram = dry.soundRam();
    const auto wet_ram = wet.soundRam();
    const auto disabled_ram = disabled.soundRam();
    const auto work_begin = 0x78000U;
    assert(std::all_of(
        dry_ram.begin() + work_begin,
        dry_ram.end(),
        [](std::uint8_t byte) { return byte == 0U; }));
    assert(std::any_of(
        wet_ram.begin() + work_begin,
        wet_ram.end(),
        [](std::uint8_t byte) { return byte != 0U; }));
    assert(disabled_mix == dry_mix);
    assert(std::all_of(
        disabled_ram.begin() + work_begin,
        disabled_ram.end(),
        [](std::uint8_t byte) { return byte == 0U; }));
}

void biosHleSetjmpAndTty() {
    stuntmaster::psx::R3000Runtime runtime;
    std::string tty;
    stuntmaster::psx::BiosHle bios{
        [&tty](std::string_view text) { tty.assign(text); }};

    constexpr std::uint32_t text_address = 0x80010000U;
    constexpr char message[] = "hello";
    assert(runtime.loadBytes(
        text_address, std::as_bytes(std::span{message, sizeof(message)})));
    runtime.reset(0xB0U, 0, 0x801FFFF0U);
    runtime.setRegister(4, text_address);
    runtime.setRegister(9, 0x3FU);
    runtime.setRegister(31, 0x80020000U);
    const auto tty_result = bios.dispatch(runtime);
    assert(tty_result.status == stuntmaster::psx::BiosHleStatus::handled);
    assert(tty == "hello");
    assert(runtime.state().pc == 0x80020000U);

    constexpr std::uint32_t jump_buffer = 0x80011000U;
    runtime.reset(0xA0U, 0x12345678U, 0x801FF000U);
    runtime.setRegister(4, jump_buffer);
    runtime.setRegister(9, 0x13U);
    runtime.setRegister(31, 0x80030000U);
    const auto setjmp_result = bios.dispatch(runtime);
    std::uint32_t saved_ra{};
    assert(setjmp_result.status == stuntmaster::psx::BiosHleStatus::handled);
    assert(runtime.read32(jump_buffer, saved_ra));
    assert(saved_ra == 0x80030000U);
    assert(runtime.state().gpr[2] == 0);

    std::uint32_t captured_command{};
    bool captured_gp1{};
    stuntmaster::psx::BiosHle gpu_bios{
        {}, [&captured_command, &captured_gp1](bool gp1, std::uint32_t command) {
            captured_gp1 = gp1;
            captured_command = command;
        }};
    runtime.reset(0xA0U, 0, 0x801FFFF0U);
    runtime.setRegister(4, 0xE1000400U);
    runtime.setRegister(9, 0x49U);
    runtime.setRegister(31, 0x80040000U);
    const auto gpu_result = gpu_bios.dispatch(runtime);
    std::uint32_t gp0_shadow{};
    assert(gpu_result.status == stuntmaster::psx::BiosHleStatus::handled);
    assert(!captured_gp1);
    assert(captured_command == 0xE1000400U);
    assert(gpu_bios.gpuCommandCount() == 1U);
    assert(runtime.read32(0x1F801810U, gp0_shadow));
    assert(gp0_shadow == captured_command);

    runtime.reset(0xA0U, 0, 0x801FFFF0U);
    runtime.setRegister(4, 0x00000000U);
    runtime.setRegister(9, 0x48U);
    runtime.setRegister(31, 0x80040000U);
    const auto gp1_result = gpu_bios.dispatch(runtime);
    std::uint32_t gpu_status{};
    assert(gp1_result.status == stuntmaster::psx::BiosHleStatus::handled);
    assert(runtime.read32(0x1F801814U, gpu_status));
    assert((gpu_status & 0x04000000U) != 0U);

    runtime.reset(0xA0U, 0, 0x801FFFF0U);
    runtime.setRegister(9, 0x44U);
    runtime.setRegister(31, 0x80050000U);
    const auto flush_result = gpu_bios.dispatch(runtime);
    assert(flush_result.status == stuntmaster::psx::BiosHleStatus::handled);
    assert(runtime.state().pc == 0x80050000U);

    runtime.reset(0xB0U, 0, 0x801FFFF0U);
    runtime.setRegister(9, 0x56U);
    runtime.setRegister(31, 0x80060000U);
    const auto table_result = gpu_bios.dispatch(runtime);
    const auto c0_table = runtime.state().gpr[2];
    std::uint32_t clear_counter_thunk{};
    assert(table_result.status == stuntmaster::psx::BiosHleStatus::handled);
    assert(c0_table == 0x80001400U);
    assert(runtime.read32(
        c0_table + 0x0AU * sizeof(std::uint32_t), clear_counter_thunk));
    runtime.reset(clear_counter_thunk, 0, 0x801FFFF0U);
    runtime.setRegister(4, 1U);
    runtime.setRegister(5, 1U);
    runtime.setRegister(31, 0x80070000U);
    const auto thunk_result = gpu_bios.dispatch(runtime);
    assert(thunk_result.status == stuntmaster::psx::BiosHleStatus::handled);
    assert(runtime.state().pc == 0x80070000U);

    runtime.reset(0xB0U, 0, 0x801FFFF0U);
    runtime.setRegister(4, 0xF2000003U);
    runtime.setRegister(5, 2U);
    runtime.setRegister(6, 0x1000U);
    runtime.setRegister(7, 0x80026130U);
    runtime.setRegister(9, 0x08U);
    runtime.setRegister(31, 0x80080000U);
    const auto open_event_result = gpu_bios.dispatch(runtime);
    const auto event_descriptor = runtime.state().gpr[2];
    assert(open_event_result.status ==
           stuntmaster::psx::BiosHleStatus::handled);
    assert((event_descriptor & 0xFFFF0000U) == 0xF1000000U);
    runtime.reset(0xB0U, 0, 0x801FFFF0U);
    runtime.setRegister(4, event_descriptor);
    runtime.setRegister(9, 0x0CU);
    runtime.setRegister(31, 0x80090000U);
    assert(gpu_bios.dispatch(runtime).status ==
           stuntmaster::psx::BiosHleStatus::handled);
    assert(gpu_bios.eventCallback(0xF2000003U, 2U) ==
           std::optional<std::uint32_t>{0x80026130U});

    constexpr std::uint32_t zero_destination = 0x80003000U;
    assert(runtime.write32(zero_destination, 0xDEADBEEFU));
    runtime.reset(0xA0U, 0, 0x801FFFF0U);
    runtime.setRegister(4, zero_destination + 1U);
    runtime.setRegister(5, 2U);
    runtime.setRegister(9, 0x28U);
    runtime.setRegister(31, 0x800A0000U);
    assert(gpu_bios.dispatch(runtime).status ==
           stuntmaster::psx::BiosHleStatus::handled);
    std::uint32_t partly_zeroed{};
    assert(runtime.read32(zero_destination, partly_zeroed));
    assert(partly_zeroed == 0xDE0000EFU);
    assert(runtime.state().gpr[2] == zero_destination + 1U);

    constexpr std::uint32_t copy_source = 0x80003200U;
    constexpr std::uint32_t copy_destination = 0x80003300U;
    constexpr std::array copy_source_bytes{
        std::byte{'p'}, std::byte{'a'}, std::byte{'d'}, std::byte{0}};
    assert(runtime.loadBytes(copy_source, copy_source_bytes));
    assert(runtime.write32(copy_destination, 0xFFFFFFFFU));
    assert(runtime.write32(copy_destination + 4U, 0xFFFFFFFFU));
    runtime.reset(0xA0U, 0, 0x801FFFF0U);
    runtime.setRegister(4, copy_destination);
    runtime.setRegister(5, copy_source);
    runtime.setRegister(6, 6U);
    runtime.setRegister(9, 0x1AU);
    runtime.setRegister(31, 0x800A1000U);
    assert(gpu_bios.dispatch(runtime).status ==
           stuntmaster::psx::BiosHleStatus::handled);
    std::array<std::uint8_t, 6> copied{};
    for (std::uint32_t index = 0; index < copied.size(); ++index) {
        assert(runtime.read8(copy_destination + index, copied[index]));
    }
    assert((copied == std::array<std::uint8_t, 6>{
        'p', 'a', 'd', 0U, 0U, 0U}));
    assert(runtime.state().gpr[2] == copy_destination);

    runtime.reset(0xA0U, 0, 0x801FFFF0U);
    runtime.setRegister(4, copy_source);
    runtime.setRegister(9, 0x1BU);
    runtime.setRegister(31, 0x800A2000U);
    assert(gpu_bios.dispatch(runtime).status ==
           stuntmaster::psx::BiosHleStatus::handled);
    assert(runtime.state().gpr[2] == 3U);

    constexpr std::uint32_t compare_left = 0x80003400U;
    constexpr std::uint32_t compare_right = 0x80003500U;
    constexpr std::array compare_left_bytes{
        std::byte{'p'}, std::byte{'a'}, std::byte{'d'}, std::byte{0}};
    constexpr std::array compare_right_bytes{
        std::byte{'p'}, std::byte{'a'}, std::byte{'d'}, std::byte{0}};
    assert(runtime.loadBytes(compare_left, compare_left_bytes));
    assert(runtime.loadBytes(compare_right, compare_right_bytes));
    runtime.reset(0xA0U, 0, 0x801FFFF0U);
    runtime.setRegister(4, compare_left);
    runtime.setRegister(5, compare_right);
    runtime.setRegister(9, 0x17U);
    runtime.setRegister(31, 0x800A3000U);
    assert(gpu_bios.dispatch(runtime).status ==
           stuntmaster::psx::BiosHleStatus::handled);
    assert(runtime.state().gpr[2] == 0U);

    assert(runtime.write8(compare_right + 2U, 't'));
    runtime.reset(0xA0U, 0, 0x801FFFF0U);
    runtime.setRegister(4, compare_left);
    runtime.setRegister(5, compare_right);
    runtime.setRegister(9, 0x17U);
    runtime.setRegister(31, 0x800A4000U);
    assert(gpu_bios.dispatch(runtime).status ==
           stuntmaster::psx::BiosHleStatus::handled);
    assert(runtime.state().gpr[2] ==
           static_cast<std::uint32_t>(
               static_cast<std::int32_t>('d' - 't')));

    constexpr std::uint32_t integer_text = 0x80003600U;
    constexpr std::array integer_text_bytes{
        std::byte{' '}, std::byte{'\t'}, std::byte{'-'}, std::byte{'4'},
        std::byte{'2'}, std::byte{'x'}, std::byte{0}};
    assert(runtime.loadBytes(integer_text, integer_text_bytes));
    runtime.reset(0xA0U, 0, 0x801FFFF0U);
    runtime.setRegister(4, integer_text);
    runtime.setRegister(9, 0x10U);
    runtime.setRegister(31, 0x800A5000U);
    assert(gpu_bios.dispatch(runtime).status ==
           stuntmaster::psx::BiosHleStatus::handled);
    assert(runtime.state().gpr[2] ==
           static_cast<std::uint32_t>(static_cast<std::int32_t>(-42)));

    runtime.reset(0xA0U, 0, 0x801FFFF0U);
    runtime.setRegister(4, integer_text);
    runtime.setRegister(9, 0x11U);
    runtime.setRegister(31, 0x800A5800U);
    assert(gpu_bios.dispatch(runtime).status ==
           stuntmaster::psx::BiosHleStatus::handled);
    assert(runtime.state().gpr[2] ==
           static_cast<std::uint32_t>(static_cast<std::int32_t>(-42)));

    constexpr std::uint32_t string_copy_destination = 0x80003700U;
    assert(runtime.write32(string_copy_destination, 0xFFFFFFFFU));
    runtime.reset(0xA0U, 0, 0x801FFFF0U);
    runtime.setRegister(4, string_copy_destination);
    runtime.setRegister(5, copy_source);
    runtime.setRegister(9, 0x19U);
    runtime.setRegister(31, 0x800A6000U);
    assert(gpu_bios.dispatch(runtime).status ==
           stuntmaster::psx::BiosHleStatus::handled);
    std::uint32_t copied_string{};
    assert(runtime.read32(string_copy_destination, copied_string));
    assert(copied_string == 0x00646170U);
    assert(runtime.state().gpr[2] == string_copy_destination);

    runtime.reset(0xA0U, 0, 0x801FFFF0U);
    runtime.setRegister(4, copy_source);
    runtime.setRegister(5, 'a');
    runtime.setRegister(9, 0x1EU);
    runtime.setRegister(31, 0x800A7000U);
    assert(gpu_bios.dispatch(runtime).status ==
           stuntmaster::psx::BiosHleStatus::handled);
    assert(runtime.state().gpr[2] == copy_source + 1U);

    runtime.reset(0xA0U, 0, 0x801FFFF0U);
    runtime.setRegister(4, copy_source);
    runtime.setRegister(5, 'x');
    runtime.setRegister(9, 0x1EU);
    runtime.setRegister(31, 0x800A8000U);
    assert(gpu_bios.dispatch(runtime).status ==
           stuntmaster::psx::BiosHleStatus::handled);
    assert(runtime.state().gpr[2] == 0U);

    constexpr std::uint32_t interrupt_node = 0x80003100U;
    assert(runtime.write32(interrupt_node, 0xAAAAAAAAU));
    runtime.reset(0xC0U, 0, 0x801FFFF0U);
    runtime.setRegister(4, 2U);
    runtime.setRegister(5, interrupt_node);
    runtime.setRegister(9, 0x02U);
    runtime.setRegister(31, 0x800B0000U);
    assert(gpu_bios.dispatch(runtime).status ==
           stuntmaster::psx::BiosHleStatus::handled);
    std::uint32_t interrupt_next{};
    assert(runtime.read32(interrupt_node, interrupt_next));
    assert(interrupt_next == 0U);
    runtime.reset(0xC0U, 0, 0x801FFFF0U);
    runtime.setRegister(4, 2U);
    runtime.setRegister(5, interrupt_node);
    runtime.setRegister(9, 0x03U);
    runtime.setRegister(31, 0x800C0000U);
    assert(gpu_bios.dispatch(runtime).status ==
           stuntmaster::psx::BiosHleStatus::handled);
}

void memoryCardPersistsGuestAuthoredFiles() {
    using stuntmaster::psx::BiosHle;
    using stuntmaster::psx::BiosHleStatus;
    using stuntmaster::psx::MemoryCard;
    using stuntmaster::psx::R3000Runtime;

    const auto path =
        std::filesystem::current_path() / "stuntmaster-memory-card-test.mcr";
    std::filesystem::remove(path);

    constexpr std::string_view filename{"BASLUS-00684STUNT"};
    constexpr std::uint32_t string_address = 0x80004000U;
    constexpr std::uint32_t payload_address = 0x80004200U;
    constexpr std::uint32_t read_address = 0x80004400U;
    constexpr std::uint32_t directory_address = 0x80004600U;
    constexpr std::uint32_t raw_frame_address = 0x80004800U;
    constexpr std::array payload{
        std::byte{'S'}, std::byte{'A'}, std::byte{'V'}, std::byte{'E'},
        std::byte{0x10}, std::byte{0x20}, std::byte{0x30}, std::byte{0x40}};

    {
        MemoryCard card{path};
        assert(card.image().size() == MemoryCard::image_size);
        assert(card.image()[0] == std::byte{'M'});
        assert(card.image()[1] == std::byte{'C'});

        R3000Runtime runtime;
        BiosHle bios{{}, {}, &card};
        const auto write_string = [&](std::uint32_t address, std::string_view text) {
            std::vector<char> bytes(text.begin(), text.end());
            bytes.push_back('\0');
            assert(runtime.loadBytes(address, std::as_bytes(std::span{bytes})));
        };
        const auto invoke = [&](std::uint32_t vector, std::uint32_t function,
                                std::array<std::uint32_t, 3> arguments) {
            runtime.reset(vector, 0U, 0x801FFFF0U);
            runtime.setRegister(4U, arguments[0]);
            runtime.setRegister(5U, arguments[1]);
            runtime.setRegister(6U, arguments[2]);
            runtime.setRegister(9U, function);
            runtime.setRegister(31U, 0x80008000U);
            return bios.dispatch(runtime);
        };
        const auto open_callback_event = [&](std::uint32_t event_class,
                                             std::uint32_t callback) {
            runtime.reset(0xB0U, 0U, 0x801FFFF0U);
            runtime.setRegister(4U, event_class);
            runtime.setRegister(5U, 4U);
            runtime.setRegister(6U, 0x1000U);
            runtime.setRegister(7U, callback);
            runtime.setRegister(9U, 0x08U);
            runtime.setRegister(31U, 0x80008000U);
            assert(bios.dispatch(runtime).status == BiosHleStatus::handled);
            const auto event = runtime.state().gpr[2];
            assert(invoke(0xB0U, 0x0CU, {event, 0U, 0U}).status ==
                   BiosHleStatus::handled);
        };

        // Retail's MemCardExist waits on the software-card event after
        // _card_info. Raw frame transfers use the distinct hardware event.
        constexpr std::uint32_t software_success_callback = 0x80018308U;
        constexpr std::uint32_t hardware_success_callback = 0x80018358U;
        open_callback_event(0xF4000001U, software_success_callback);
        open_callback_event(0xF0000011U, hardware_success_callback);

        assert(invoke(0xA0U, 0xABU, {0U, 0U, 0U}).status ==
               BiosHleStatus::handled);
        assert(runtime.state().gpr[2] == 1U);
        assert(bios.takePendingEventCallback() ==
               std::optional<std::uint32_t>{software_success_callback});
        assert(!bios.takePendingEventCallback());
        assert(invoke(0xA0U, 0xABU, {0x10U, 0U, 0U}).status ==
               BiosHleStatus::handled);
        assert(runtime.state().gpr[2] == 0U);

        // Retail links its own libapi firstfile, which walks the kernel device
        // table before it ever reaches the BIOS boundary and returns a null
        // entry without calling it when the "bu" device is missing. Its caller
        // then blocks forever in _get_card_event_x on a completion no BIOS call
        // raised, so card setup must publish the device control block.
        std::uint32_t device_table{};
        std::uint32_t device_table_size{};
        assert(runtime.read32(0x80000150U, device_table));
        assert(runtime.read32(0x80000154U, device_table_size));
        assert(device_table != 0U);
        assert(device_table_size == 0x50U);
        std::uint32_t device_name_address{};
        assert(runtime.read32(device_table, device_name_address));
        std::array<char, 3U> device_name{};
        assert(runtime.copyBytes(
            device_name_address, std::as_writable_bytes(std::span{device_name})));
        assert(std::string_view{device_name.data()} == "bu");
        // The firstfile slot retail saves and overwrites must be writable.
        assert(runtime.write32(device_table + 0x34U, 0x800BAF54U));

        // A fresh card has no matching directory entry. Retail's firstfile
        // wrapper immediately blocks in _get_card_event_x in this case, so a
        // hardware completion must already be scheduled when BIOS returns 0.
        write_string(string_address, "bu00:BASLUS-*");
        assert(invoke(
                   0xB0U, 0x42U,
                   {string_address, directory_address, 0U})
                   .status == BiosHleStatus::handled);
        assert(runtime.state().gpr[2] == 0U);
        assert(bios.takePendingEventCallback() ==
               std::optional<std::uint32_t>{hardware_success_callback});
        assert(!bios.takePendingEventCallback());

        assert(card.createFile(filename, 1U));
        // Retail assembles every card path with strcat: the device prefix is
        // formatted first and the filename appended. A0:15 is that call, not
        // open, and open is B0:32.
        write_string(string_address, "bu00:");
        write_string(string_address + 0x40U, filename);
        assert(invoke(
                   0xA0U, 0x15U,
                   {string_address, string_address + 0x40U, 0U})
                   .status == BiosHleStatus::handled);
        assert(runtime.state().gpr[2] == string_address);
        std::array<char, 32U> assembled{};
        assert(runtime.copyBytes(
            string_address, std::as_writable_bytes(std::span{assembled})));
        assert(std::string_view{assembled.data()} == "bu00:BASLUS-00684STUNT");

        assert(invoke(0xB0U, 0x32U, {string_address, 0x8001U, 0U}).status ==
               BiosHleStatus::handled);
        const auto descriptor = runtime.state().gpr[2];
        assert(descriptor >= 2U);

        assert(runtime.loadBytes(payload_address, payload));
        assert(invoke(
                   0xB0U, 0x35U,
                   {descriptor, payload_address,
                    static_cast<std::uint32_t>(payload.size())})
                   .status == BiosHleStatus::handled);
        assert(runtime.state().gpr[2] == 0U);
        assert(bios.takePendingEventCallback() ==
               std::optional<std::uint32_t>{software_success_callback});

        assert(invoke(0xB0U, 0x33U, {descriptor, 0U, 0U}).status ==
               BiosHleStatus::handled);
        assert(invoke(
                   0xB0U, 0x34U,
                   {descriptor, read_address,
                    static_cast<std::uint32_t>(payload.size())})
                   .status == BiosHleStatus::handled);
        assert(runtime.state().gpr[2] == 0U);
        std::array<std::byte, payload.size()> restored{};
        assert(runtime.copyBytes(read_address, restored));
        assert(restored == payload);
        assert(bios.takePendingEventCallback() ==
               std::optional<std::uint32_t>{software_success_callback});

        // Close is B0:36. Retail reopens the same file for each transfer, so a
        // descriptor that is never released exhausts the table.
        assert(invoke(0xB0U, 0x36U, {descriptor, 0U, 0U}).status ==
               BiosHleStatus::handled);
        assert(runtime.state().gpr[2] == 0U);

        write_string(string_address, "bu00:BASLUS-*");
        assert(invoke(
                   0xB0U, 0x42U,
                   {string_address, directory_address, 0U})
                   .status == BiosHleStatus::handled);
        assert(runtime.state().gpr[2] == directory_address);
        std::array<char, 20U> directory_name{};
        assert(runtime.copyBytes(
            directory_address, std::as_writable_bytes(std::span{directory_name})));
        assert(std::string_view{directory_name.data()} == filename);
        std::uint32_t directory_size{};
        assert(runtime.read32(directory_address + 24U, directory_size));
        assert(directory_size == MemoryCard::block_size);

        assert(invoke(
                   0xB0U, 0x4FU,
                   {0U, 1U, raw_frame_address})
                   .status == BiosHleStatus::handled);
        std::uint8_t allocation_state{};
        assert(runtime.read8(raw_frame_address, allocation_state));
        assert(allocation_state == 0x51U);
        assert(bios.takePendingEventCallback() ==
               std::optional<std::uint32_t>{hardware_success_callback});
        assert(!bios.takePendingEventCallback());
    }

    // Reopening the raw image must expose the exact payload written through
    // the BIOS file calls, proving the result crossed the process boundary.
    MemoryCard reloaded{path};
    const auto file = reloaded.findFile(filename);
    assert(file);
    std::array<std::byte, payload.size()> restored{};
    assert(reloaded.readFile(*file, 0U, restored) == restored.size());
    assert(restored == payload);
    std::filesystem::remove(path);
}

void retailVSyncHle() {
    stuntmaster::psx::R3000Runtime runtime;
    stuntmaster::game::RetailHle hle;
    constexpr std::uint32_t vsync = 0x800366E8U;
    constexpr std::uint32_t vcount = 0x800D8974U;
    constexpr std::uint32_t return_address = 0x80020000U;

    runtime.reset(vsync, 0, 0x801FFFF0U);
    runtime.setRegister(4, 0U);
    runtime.setRegister(31, return_address);
    const auto wait_result = hle.dispatch(runtime);
    std::uint32_t counter{};
    assert(wait_result.status == stuntmaster::game::RetailHleStatus::handled);
    assert(runtime.state().pc == return_address);
    assert(runtime.read32(vcount, counter));
    assert(counter == 1U);

    runtime.reset(vsync, 0, 0x801FFFF0U);
    runtime.setRegister(4, 0xFFFFFFFFU);
    runtime.setRegister(31, return_address);
    const auto query_result = hle.dispatch(runtime);
    assert(query_result.status == stuntmaster::game::RetailHleStatus::handled);
    assert(runtime.state().gpr[2] == 1U);
    assert(runtime.read32(vcount, counter));
    assert(counter == 1U);
    assert(hle.onVBlank(runtime));
    assert(hle.vblankCount() == 1U);
    assert(runtime.read32(vcount, counter));
    assert(counter == 2U);
    std::uint8_t pad_status{};
    std::uint8_t pad_id{};
    std::uint8_t pad_buttons_first{};
    std::uint8_t pad_buttons_second{};
    assert(runtime.read8(0x800DFA30U, pad_status));
    assert(runtime.read8(0x800DFA31U, pad_id));
    assert(runtime.read8(0x800DFA32U, pad_buttons_first));
    assert(runtime.read8(0x800DFA33U, pad_buttons_second));
    assert(pad_status == 0U);
    assert(pad_id == 0x41U);
    assert(pad_buttons_first == 0xFFU);
    assert(pad_buttons_second == 0xFFU);

    // The host publishes the retail libpad's DualShock state so the game's
    // PadGetState(0) == 6 checks (Options vibration toggle, shake function,
    // countdown pump) see a vibration-capable pad. Port two stays
    // disconnected.
    std::uint8_t pad_state{};
    std::uint8_t pad_mode{};
    std::uint8_t pad_comb_count{};
    std::uint8_t pad_actuator_count{};
    std::uint16_t pad_mode_switch_mask{};
    std::uint8_t pad_current_mode{};
    std::uint8_t pad_comb_entry_count{};
    std::uint8_t pad_actuators_per_comb{};
    assert(runtime.read8(0x800E2945U, pad_state));
    assert(runtime.read8(0x800E2942U, pad_mode));
    assert(runtime.read8(0x800E29DFU, pad_comb_count));
    assert(runtime.read8(0x800E29E0U, pad_actuator_count));
    assert(runtime.read16(0x800E29E2U, pad_mode_switch_mask));
    assert(runtime.read8(0x800E29E4U, pad_current_mode));
    assert(runtime.read8(0x800E29E5U, pad_comb_entry_count));
    assert(runtime.read8(0x800E29E6U, pad_actuators_per_comb));
    assert(pad_state == 6U);
    assert(pad_mode == 0xFEU);
    assert(pad_comb_count == 1U);
    assert(pad_actuator_count == 2U);
    assert(pad_mode_switch_mask == 0x73U);
    assert(pad_current_mode == 0x41U);
    assert(pad_comb_entry_count == 1U);
    assert(pad_actuators_per_comb == 2U);
    std::uint8_t pad_two_state{};
    assert(runtime.read8(0x800E2A35U, pad_two_state));
    assert(pad_two_state == 0U);

    const auto retail_active_high = [&]() {
        return static_cast<std::uint16_t>(~(
            static_cast<std::uint16_t>(pad_buttons_first) << 8U |
            static_cast<std::uint16_t>(pad_buttons_second)));
    };

    hle.setPadOneState(true, 0xDFFFU); // Circle
    assert(hle.onVBlank(runtime));
    assert(runtime.read8(0x800DFA32U, pad_buttons_first));
    assert(runtime.read8(0x800DFA33U, pad_buttons_second));
    assert(pad_buttons_first == 0xFFU);
    assert(pad_buttons_second == 0xDFU);
    assert(retail_active_high() == 0x0020U);

    hle.setPadOneState(true, 0xFFDFU); // D-pad Right
    assert(hle.onVBlank(runtime));
    assert(runtime.read8(0x800DFA32U, pad_buttons_first));
    assert(runtime.read8(0x800DFA33U, pad_buttons_second));
    assert(pad_buttons_first == 0xDFU);
    assert(pad_buttons_second == 0xFFU);
    assert(retail_active_high() == 0x2000U);

    hle.setPadOneState(true, 0xFFF7U); // Start
    assert(hle.onVBlank(runtime));
    assert(runtime.read8(0x800DFA32U, pad_buttons_first));
    assert(runtime.read8(0x800DFA33U, pad_buttons_second));
    assert(pad_buttons_first == 0xF7U);
    assert(pad_buttons_second == 0xFFU);
    assert(retail_active_high() == 0x0800U);
}

void retailCdInitHle() {
    using stuntmaster::game::cdReadSpeedForLoadPhase;
    assert(cdReadSpeedForLoadPhase(0U, 30U, 90U) == 0U);
    assert(cdReadSpeedForLoadPhase(2U, 30U, 90U) == 2U);
    assert(cdReadSpeedForLoadPhase(16U, 29U, 90U) == 16U);
    assert(cdReadSpeedForLoadPhase(16U, 30U, 90U) == 2U);
    assert(cdReadSpeedForLoadPhase(16U, 89U, 90U) == 2U);
    assert(cdReadSpeedForLoadPhase(16U, 90U, 90U) == 16U);

    stuntmaster::psx::R3000Runtime runtime;
    stuntmaster::game::RetailHle hle;
    assert(hle.cdReadSpeed() == 0U);
    hle.setCdReadSpeed(16U);
    assert(hle.cdReadSpeed() == 16U);
    hle.setCdReadSpeed(0U);
    constexpr std::uint32_t cd_init = 0x800C746CU;
    constexpr std::uint32_t return_address = 0x80020000U;
    runtime.reset(cd_init, 0, 0x801FFFF0U);
    runtime.setRegister(31, return_address);
    const auto result = hle.dispatch(runtime);
    assert(result.status == stuntmaster::game::RetailHleStatus::handled);
    assert(runtime.state().pc == return_address);
    assert(runtime.state().gpr[2] == 0U);

    constexpr std::uint32_t cd_sync = 0x800C6908U;
    runtime.reset(cd_sync, 0, 0x801FFFF0U);
    runtime.setRegister(4, 0U);
    runtime.setRegister(5, 0U);
    runtime.setRegister(31, return_address);
    const auto sync_result = hle.dispatch(runtime);
    assert(sync_result.status == stuntmaster::game::RetailHleStatus::handled);
    assert(runtime.state().pc == return_address);
    assert(runtime.state().gpr[2] == 2U);

    constexpr std::uint32_t cd_control = 0x800C6E50U;
    constexpr std::uint32_t result_address = 0x80011000U;
    runtime.reset(cd_control, 0, 0x801FFFF0U);
    runtime.setRegister(4, 1U);
    runtime.setRegister(6, result_address);
    runtime.setRegister(31, return_address);
    const auto control_result = hle.dispatch(runtime);
    std::uint8_t drive_status{};
    assert(control_result.status ==
           stuntmaster::game::RetailHleStatus::handled);
    assert(runtime.state().pc == return_address);
    assert(runtime.state().gpr[2] == 0U);
    assert(runtime.read8(result_address, drive_status));
    assert(drive_status == 2U);

    constexpr std::uint32_t cd_ready = 0x800C6B88U;
    runtime.reset(cd_ready, 0, 0x801FFFF0U);
    runtime.setRegister(4, 0U);
    runtime.setRegister(5, result_address);
    runtime.setRegister(31, return_address);
    const auto ready_result = hle.dispatch(runtime);
    assert(ready_result.status ==
           stuntmaster::game::RetailHleStatus::handled);
    assert(runtime.state().pc == return_address);
    assert(runtime.state().gpr[2] == 2U);

    constexpr std::uint32_t location_address = 0x80012000U;
    const std::array location{
        std::byte{0x00}, std::byte{0x02}, std::byte{0x10}, std::byte{0x00}};
    assert(runtime.loadBytes(location_address, location));
    runtime.reset(cd_control, 0, 0x801FFFF0U);
    runtime.setRegister(4, 2U);
    runtime.setRegister(5, location_address);
    runtime.setRegister(31, return_address);
    const auto location_result = hle.dispatch(runtime);
    assert(location_result.status ==
           stuntmaster::game::RetailHleStatus::handled);
    assert(hle.currentCdLba() == 10U);

    constexpr std::uint32_t cd_status = 0x800B89C4U;
    runtime.reset(cd_status, 0, 0x801FFFF0U);
    runtime.setRegister(31, return_address);
    const auto status_result = hle.dispatch(runtime);
    assert(status_result.status ==
           stuntmaster::game::RetailHleStatus::handled);
    assert(runtime.state().gpr[2] == 2U);
}

void retailMovieSkipIsCallerGated() {
    using stuntmaster::game::RetailMoviePlayRequest;
    using stuntmaster::game::retailMovieCallerChainsAnother;
    using stuntmaster::presentation::movieStartPressed;
    assert(movieStartPressed(0xFFFFU, 0xFFF7U));
    assert(!movieStartPressed(0xFFF7U, 0xFFF7U));
    assert(!movieStartPressed(0xFFFFU, 0xFFFFU));

    stuntmaster::psx::R3000Runtime runtime;
    stuntmaster::game::RetailHle hle;
    constexpr std::uint32_t game_play_movie = 0x8002BBF0U;
    constexpr std::uint32_t movie_play = 0x80014534U;
    constexpr std::uint32_t expected_return = 0x8002BD0CU;

    constexpr std::uint32_t path_address = 0x80010000U;
    constexpr std::array movie_path{
        std::byte{'p'}, std::byte{'r'}, std::byte{'o'}, std::byte{'l'},
        std::byte{'o'}, std::byte{'g'}, std::byte{'.'}, std::byte{'s'},
        std::byte{'t'}, std::byte{'r'}, std::byte{0}};
    assert(runtime.loadBytes(path_address, movie_path));
    RetailMoviePlayRequest prepared_movie;
    RetailMoviePlayRequest requested_movie;
    int movie_transition_notifications = 0;
    hle.setMovieTransitionSink(
        [&movie_transition_notifications] {
            ++movie_transition_notifications;
        });
    hle.setMoviePrepareSink(
        [&prepared_movie](const RetailMoviePlayRequest& request) {
            prepared_movie = request;
        });
    hle.setMoviePlaySink(
        [&requested_movie](const RetailMoviePlayRequest& request) {
            requested_movie = request;
        });
    runtime.reset(game_play_movie, 0, 0x801FFFF0U);
    runtime.setRegister(5, path_address);
    runtime.setRegister(31, 0x800C9A18U);
    assert(hle.dispatch(runtime).status ==
           stuntmaster::game::RetailHleStatus::not_boundary);
    assert(prepared_movie.path == "prolog.str");
    assert(prepared_movie.followed_by_movie);

    runtime.reset(0x8002C178U, 0, 0x801FFFF0U);
    assert(hle.dispatch(runtime).status ==
           stuntmaster::game::RetailHleStatus::not_boundary);
    assert(movie_transition_notifications == 1);

    runtime.reset(movie_play, 0, 0x801FFFF0U);
    runtime.setRegister(31, expected_return);
    const auto skipped = hle.dispatch(runtime);
    assert(skipped.status == stuntmaster::game::RetailHleStatus::handled);
    assert(runtime.state().pc == expected_return);
    assert(runtime.state().gpr[2] == 0U);
    assert(requested_movie.path == "prolog.str");
    assert(requested_movie.followed_by_movie);
    assert(retailMovieCallerChainsAnother(0x800C9A18U));
    assert(retailMovieCallerChainsAnother(0x800C9A34U));
    assert(retailMovieCallerChainsAnother(0x8002B968U));
    assert(!retailMovieCallerChainsAnother(0x800C9A50U));
    assert(!retailMovieCallerChainsAnother(0x8002B984U));

    runtime.reset(movie_play, 0, 0x801FFFF0U);
    runtime.setRegister(31, 0x80030000U);
    assert(hle.dispatch(runtime).status ==
           stuntmaster::game::RetailHleStatus::not_boundary);
}

void retailDrawSyncHle() {
    stuntmaster::psx::R3000Runtime runtime;
    stuntmaster::game::RetailHle hle;
    constexpr std::uint32_t draw_sync = 0x80026C04U;
    constexpr std::uint32_t return_address = 0x80040000U;
    runtime.reset(draw_sync, 0, 0x801FFFF0U);
    runtime.setRegister(4, 1U);
    runtime.setRegister(31, return_address);
    const auto result = hle.dispatch(runtime);
    assert(result.status == stuntmaster::game::RetailHleStatus::handled);
    assert(runtime.state().pc == return_address);
    assert(runtime.state().gpr[2] == 0U);
}

void retailWaitForLayerPollFastForward() {
    stuntmaster::psx::R3000Runtime runtime;
    constexpr std::uint32_t poll_pc = 0x8009FDF0U;
    constexpr std::uint32_t view_global = 0x800DD780U;
    constexpr std::uint32_t view = 0x80010000U;
    constexpr std::uint32_t layers = 0x80010100U;
    constexpr std::uint32_t queue = 0x80010200U;
    constexpr std::uint32_t counters = 0x80010300U;
    constexpr std::uint32_t layer = 0x80010400U;
    constexpr std::uint32_t vtable = 0x80010500U;
    constexpr std::uint32_t layer_index = 1U;
    constexpr std::uint32_t counter_offset = 4U;
    constexpr std::uint32_t layer_slot = layers + layer_index * 4U;
    runtime.reset(poll_pc, 0, 0x801FFFF0U);
    assert(runtime.write32(view_global, view));
    assert(runtime.write32(view + 0x10U, layers));
    assert(runtime.write32(queue + 0x24U, counters));
    assert(runtime.write32(counters + counter_offset, 7U));
    assert(runtime.write32(layer_slot, layer));
    assert(runtime.write32(layer + 0x2CU, vtable));
    assert(runtime.write32(vtable + 0x18U, 0x800A03ACU));
    assert(runtime.write32(layer + 0x0CU, 1U));
    runtime.setRegister(31U, poll_pc);
    runtime.setRegister(2U, 0U);
    runtime.setRegister(3U, 7U);
    runtime.setRegister(4U, 1U);
    runtime.setRegister(5U, layer_slot);
    runtime.setRegister(16U, counter_offset);
    runtime.setRegister(17U, queue);
    runtime.setRegister(18U, layer_index);

    assert(stuntmaster::game::RetailHle::fastForwardWaitForLayerPolls(
               runtime, 10U) == 10U);
    std::uint32_t counter{};
    assert(runtime.read32(counters + counter_offset, counter));
    assert(counter == 17U);
    assert(runtime.state().gpr[3] == 17U);

    // Once CheckLayer would return nonzero, even an otherwise identical poll
    // must execute normally so the loop can exit at the exact instruction.
    assert(runtime.write32(layer + 0x0CU, 2U));
    assert(stuntmaster::game::RetailHle::fastForwardWaitForLayerPolls(
               runtime, 10U) == 0U);
}

void retailVSyncCallbackHle() {
    stuntmaster::psx::R3000Runtime runtime;
    stuntmaster::game::RetailHle hle;
    constexpr std::uint32_t registration = 0x8002CEB4U;
    constexpr std::uint32_t callback = 0x800A0140U;
    constexpr std::uint32_t registrations = 0x8002CEE8U;
    constexpr std::uint32_t card_callback = 0x80016020U;
    constexpr std::uint32_t return_address = 0x80040000U;
    runtime.reset(registration, 0, 0x801FFFF0U);
    runtime.setRegister(4, callback);
    runtime.setRegister(31, return_address);
    const auto installed = hle.dispatch(runtime);
    assert(installed.status == stuntmaster::game::RetailHleStatus::handled);
    assert(runtime.state().gpr[2] == 0U);
    assert(hle.vsyncCallback() == callback);
    assert(hle.vsyncCallbacks()[4U] == callback);

    runtime.reset(registrations, 0, 0x801FFFF0U);
    runtime.setRegister(4, 7U);
    runtime.setRegister(5, card_callback);
    runtime.setRegister(31, return_address);
    const auto installed_card = hle.dispatch(runtime);
    assert(installed_card.status ==
           stuntmaster::game::RetailHleStatus::handled);
    assert(runtime.state().gpr[2] == 0U);
    assert(hle.vsyncCallbacks()[4U] == callback);
    assert(hle.vsyncCallbacks()[7U] == card_callback);

    runtime.reset(registration, 0, 0x801FFFF0U);
    runtime.setRegister(4, 0U);
    runtime.setRegister(31, return_address);
    const auto removed = hle.dispatch(runtime);
    assert(removed.status == stuntmaster::game::RetailHleStatus::handled);
    assert(runtime.state().gpr[2] == callback);
    assert(hle.vsyncCallback() == 0U);
    assert(hle.vsyncCallbacks()[7U] == card_callback);

    runtime.reset(registrations, 0, 0x801FFFF0U);
    runtime.setRegister(4, 7U);
    runtime.setRegister(5, 0U);
    runtime.setRegister(31, return_address);
    const auto removed_card = hle.dispatch(runtime);
    assert(removed_card.status ==
           stuntmaster::game::RetailHleStatus::handled);
    assert(runtime.state().gpr[2] == card_callback);
    assert(hle.vsyncCallbacks()[7U] == 0U);
}

void boundedLatestMailboxDropsOnlyObsoleteValues() {
    struct Snapshot {
        std::uint64_t sequence{};
        std::vector<std::uint32_t> packets;
        std::vector<std::uint16_t> vram;
    };
    stuntmaster::core::BoundedLatestMailbox<Snapshot, 2U> mailbox;
    mailbox.publish({1U, {0x11111111U}, {0x0001U}});
    mailbox.publish({2U, {0x22222222U}, {0x0002U}});
    mailbox.publish({3U, {0x33333333U}, {0x0003U}});

    auto latest = mailbox.takeLatest();
    assert(latest);
    assert(latest->sequence == 3U);
    assert(latest->packets == std::vector<std::uint32_t>{0x33333333U});
    assert(latest->vram == std::vector<std::uint16_t>{0x0003U});
    assert(!mailbox.takeLatest());

    const auto statistics = mailbox.statistics();
    assert(statistics.published == 3U);
    assert(statistics.dropped_on_publish == 1U);
    assert(statistics.skipped_on_consume == 1U);

    mailbox.publish({4U, {0x44444444U}, {0x0004U}});
    mailbox.publish({5U, {0x55555555U}, {0x0005U}});
    assert(mailbox.discardAll() == 2U);
    assert(!mailbox.takeLatest());
    assert(mailbox.discardAll() == 0U);
}

void memoryWriteSinkObservesGuestStores() {
    // RE instruments locate retail code by watching which PC writes a guest
    // address, so a silent sink would make them report a clean "nothing wrote
    // here" instead of failing.
    struct Observed {
        std::uint32_t address{};
        std::uint32_t size{};
        std::uint32_t value{};
        std::uint32_t pc{};
    };
    stuntmaster::psx::R3000Runtime runtime;
    constexpr std::uint32_t address = 0x80010000U;
    constexpr std::uint32_t target = 0x80100000U;
    const std::array code{
        encodeI(0x0F, 0, 4, 0x8010),   // lui   a0, 0x8010
        encodeI(0x09, 0, 2, 0x1234),   // addiu v0, zero, 0x1234
        encodeI(0x2B, 4, 2, 0),        // sw    v0, 0(a0)
        encodeI(0x29, 4, 2, 8),        // sh    v0, 8(a0)
        encodeI(0x28, 4, 2, 12),       // sb    v0, 12(a0)
        encodeR(31, 0, 0, 0, 0x08),    // jr    ra
        std::uint32_t{0},              // nop
    };
    assert(runtime.loadBytes(address, std::as_bytes(std::span{code})));

    // Install after loading so the load itself is not counted.
    std::vector<Observed> observed;
    runtime.setMemoryWriteSink(
        [&observed](
            std::uint32_t written_address,
            std::uint32_t size,
            std::uint32_t value,
            std::uint32_t pc) {
            observed.push_back({written_address, size, value, pc});
        });

    runtime.reset(address, 0, 0x801FFFF0U);
    const auto result = runtime.call(address, {}, 32);
    assert(result.reason == stuntmaster::psx::R3000StopReason::returned);

    assert(observed.size() == 3U);
    assert(observed[0].address == target);
    assert(observed[0].size == 4U);
    assert(observed[0].value == 0x1234U);
    // `step` advances `pc` to `next_pc` before executing the opcode, so the
    // sink reports the instruction after the store. Callers locating retail
    // code must subtract one instruction.
    assert(observed[0].pc == address + 8U + 4U);
    assert(observed[1].pc == address + 12U + 4U);
    assert(observed[2].pc == address + 16U + 4U);
    assert(observed[1].address == target + 8U);
    assert(observed[1].size == 2U);
    assert(observed[2].address == target + 12U);
    assert(observed[2].size == 1U);
}

void interruptCallsDoNotBorrowTheInterruptedStack() {
    // Retail runs its per-frame game step on the 1 KB hardware scratchpad.
    // A callback injected onto that stack has almost no headroom, so the
    // runtime must switch to its reserved interrupt stack and the caller's
    // saved state must bring the original stack pointer back.
    using Runtime = stuntmaster::psx::R3000Runtime;
    Runtime runtime;
    constexpr std::uint32_t address = 0x80010000U;
    const std::array code{
        encodeI(0x09, 29, 2, 0),     // addiu v0, sp, 0  (observe sp)
        encodeR(31, 0, 0, 0, 0x08),  // jr ra
        std::uint32_t{0},            // nop
    };
    assert(runtime.loadBytes(address, std::as_bytes(std::span{code})));

    constexpr std::uint32_t scratchpad_stack = 0x1F8003F8U;
    runtime.reset(address, 0, scratchpad_stack);
    const auto interrupted = runtime.state();

    assert(runtime.beginInterruptCall(address));
    assert(runtime.state().gpr[29] == Runtime::interrupt_stack_top);
    while (!runtime.atReturnSentinel()) {
        const auto result = runtime.step();
        assert(result.reason == stuntmaster::psx::R3000StopReason::running);
    }
    runtime.settleLoadDelay();
    assert(runtime.state().gpr[2] == Runtime::interrupt_stack_top);

    runtime.restoreCpuState(interrupted);
    assert(runtime.state().gpr[29] == scratchpad_stack);

    // The reserved stack must stay in RAM, below the overlay load address and
    // clear of the BIOS HLE thunk tables.
    static_assert(Runtime::interrupt_stack_top < 0x80010000U);
    static_assert(
        Runtime::interrupt_stack_top - Runtime::interrupt_stack_size >
        0x80003000U);
    std::uint32_t probe = 0U;
    assert(runtime.write32(Runtime::interrupt_stack_top, 0x5A5A5A5AU));
    assert(runtime.read32(Runtime::interrupt_stack_top, probe));
    assert(probe == 0x5A5A5A5AU);
    assert(runtime.write32(
        Runtime::interrupt_stack_top - Runtime::interrupt_stack_size,
        0xA5A5A5A5U));
}

void retailSwapGatePatchIsFingerprintedAndReversible() {
    const auto& patch = stuntmaster::game::thirtyHertzSwapGate();

    // Lock the exact instructions rather than two opaque constants: the patch
    // must turn `sltiu $v0, $v0, 2` into `sltiu $v0, $v0, 1` and nothing else.
    constexpr std::uint32_t sltiu = 0x0BU;
    constexpr std::uint32_t v0 = 2U;
    assert(patch.address == 0x800A015CU);
    assert(patch.original_word == encodeI(sltiu, v0, v0, 2));
    assert(patch.patched_word == encodeI(sltiu, v0, v0, 1));

    stuntmaster::psx::R3000Runtime runtime;
    const std::array original{patch.original_word};
    assert(runtime.loadBytes(
        patch.address, std::as_bytes(std::span{original})));

    std::uint32_t word = 0U;
    assert(stuntmaster::game::applyRetailPatch(runtime, patch));
    assert(runtime.read32(patch.address, word));
    assert(word == patch.patched_word);

    // Applying twice must refuse rather than write over an unknown site.
    assert(!stuntmaster::game::applyRetailPatch(runtime, patch));
    assert(runtime.read32(patch.address, word));
    assert(word == patch.patched_word);

    assert(stuntmaster::game::revertRetailPatch(runtime, patch));
    assert(runtime.read32(patch.address, word));
    assert(word == patch.original_word);
    assert(!stuntmaster::game::revertRetailPatch(runtime, patch));

    // A site holding anything else is never patched.
    assert(runtime.write32(patch.address, 0U));
    assert(!stuntmaster::game::applyRetailPatch(runtime, patch));
    assert(runtime.read32(patch.address, word));
    assert(word == 0U);
}

void retailSwapGatePatchReleasesASwapEveryVBlank() {
    // Execute the real compare both ways. VSCallback__Fe leaves the queued
    // swap pending while `$v0` is non-zero, so a one-VBlank delta must stop
    // being treated as "too soon" once the patch is applied.
    const auto& patch = stuntmaster::game::thirtyHertzSwapGate();
    constexpr std::uint32_t address = 0x80010000U;

    const auto pendingForDelta =
        [&patch](std::uint32_t gate_word, std::uint32_t vblank_delta) {
            stuntmaster::psx::R3000Runtime runtime;
            const std::array code{
                encodeI(0x09, 0, 2, static_cast<std::uint16_t>(vblank_delta)),
                gate_word,                   // sltiu v0, v0, N
                encodeR(31, 0, 0, 0, 0x08),  // jr ra
                std::uint32_t{0},            // nop
            };
            assert(runtime.loadBytes(address, std::as_bytes(std::span{code})));
            runtime.reset(address, 0, 0x801FFFF0U);
            const auto result = runtime.call(address, {}, 16);
            assert(result.reason ==
                   stuntmaster::psx::R3000StopReason::returned);
            return runtime.state().gpr[2] != 0U;
        };

    assert(pendingForDelta(patch.original_word, 1U));
    assert(!pendingForDelta(patch.original_word, 2U));
    assert(!pendingForDelta(patch.patched_word, 1U));
    assert(!pendingForDelta(patch.patched_word, 2U));
}


void widescreenBlockCullWidensOnlyTheHorizontalBounds() {
    // Retail's block cull bakes its screen bounds into immediates. Only the
    // horizontal bounds may move: the Y limit and the Y "< 0" bit must survive,
    // or geometry above and below the screen would be submitted too.
    assert(stuntmaster::game::widescreenXLimit(960, 720) == 0x200U);   // 4:3
    assert(stuntmaster::game::widescreenXLimit(1280, 720) == 597U);   // 16:9
    assert(stuntmaster::game::widescreenXLimit(1920, 1080) == 597U);  // 16:9
    assert(stuntmaster::game::widescreenBlockVisibilityXLimit(
               1280, 720) == 597U);
    assert(stuntmaster::game::widescreenBlockVisibilityXLimit(
               960, 720) == 0x200U);
    // A narrower-than-4:3 window must never shrink retail's own field.
    assert(stuntmaster::game::widescreenXLimit(640, 720) == 0x200U);
    assert(stuntmaster::game::widescreenXLimit(0, 0) == 0x200U);
    // And an absurd aspect stays inside signed 11-bit screen coordinates.
    assert(stuntmaster::game::widescreenXLimit(20000, 720) <= 1022U);

    const auto patches = stuntmaster::game::widescreenBlockCull(597U);
    assert(patches.size() == 2U);
    assert(patches[0].address == 0x800A0E54U);
    assert(patches[1].address == 0x800A0E5CU);

    // ori $t8, $t8, 0x200 -> ori $t8, $t8, 597. The Y limit comes from the lui
    // and the "< 0" mask is untouched, so only the right bound moves here.
    constexpr std::uint32_t ori = 0x0DU;
    constexpr std::uint32_t t8 = 24U;
    assert(patches[0].original_word == encodeI(ori, t8, t8, 0x200));
    assert(patches[0].patched_word == encodeI(ori, t8, t8, 597));
    assert(patches[1].original_word == encodeI(ori, t8, t8, 0x200));
    assert(patches[1].patched_word == encodeI(ori, t8, t8, 597));

    stuntmaster::psx::R3000Runtime runtime;
    for (const auto& patch : patches) {
        assert(runtime.write32(patch.address, patch.original_word));
    }
    assert(stuntmaster::game::applyRetailPatches(runtime, patches));
    std::uint32_t word = 0U;
    for (const auto& patch : patches) {
        assert(runtime.read32(patch.address, word));
        assert(word == patch.patched_word);
    }
    assert(stuntmaster::game::revertRetailPatches(runtime, patches));
    for (const auto& patch : patches) {
        assert(runtime.read32(patch.address, word));
        assert(word == patch.original_word);
    }

    // One mismatched site must leave every site untouched.
    assert(runtime.write32(patches[1].address, 0xDEADBEEFU));
    assert(!stuntmaster::game::applyRetailPatches(runtime, patches));
    assert(runtime.read32(patches[0].address, word));
    assert(word == patches[0].original_word);
}

void widescreenModelCullWidensTheHorizontalBounds() {
    // Props and characters cull through a different routine than level blocks.
    // Both must widen or crates vanish at the edges while the level does not.
    const auto patches = stuntmaster::game::widescreenModelCull(597U);
    assert(patches.size() == 2U);
    // The sphere test's own maxX must widen too, or objects entering from the
    // right stay clipped while the left works.
    assert(patches[0].address == 0x8009E2D8U);
    assert(patches[1].address == 0x8009E260U);

    constexpr std::uint32_t ori = 0x0DU;
    constexpr std::uint32_t lh = 0x21U;
    constexpr std::uint32_t zero = 0U;
    constexpr std::uint32_t v0 = 2U;
    constexpr std::uint32_t v1 = 3U;
    constexpr std::uint32_t lhu = 0x25U;
    constexpr std::uint32_t s6 = 22U;
    // lhu $s6, 0x60($v0) -> ori $s6, $zero, 597
    assert(patches[0].original_word == encodeI(lhu, v0, s6, 0x60));
    assert(patches[0].patched_word == encodeI(ori, zero, s6, 597));
    // lh $v1, 0x60($v1) -> ori $v1, $zero, 597
    assert(patches[1].original_word == encodeI(lh, v1, v1, 0x60));
    assert(patches[1].patched_word == encodeI(ori, zero, v1, 597));

    stuntmaster::psx::R3000Runtime runtime;
    for (const auto& patch : patches) {
        assert(runtime.write32(patch.address, patch.original_word));
    }
    assert(stuntmaster::game::applyRetailPatches(runtime, patches));
    std::uint32_t word = 0U;
    for (const auto& patch : patches) {
        assert(runtime.read32(patch.address, word));
        assert(word == patch.patched_word);
    }
    assert(stuntmaster::game::revertRetailPatches(runtime, patches));
    for (const auto& patch : patches) {
        assert(runtime.read32(patch.address, word));
        assert(word == patch.original_word);
    }

    // The block and model culls must not target the same instructions.
    const auto blocks = stuntmaster::game::widescreenBlockCull(597U);
    for (const auto& model : patches) {
        for (const auto& block : blocks) {
            assert(model.address != block.address);
        }
    }
}

void widescreenLowerBoundsMoveTheLeftEdgeInsteadOfRemovingIt() {
    // Retail's left bound is a single bit test, so it can only ever mean
    // "coord < 0". Removing it is not a conservative choice: it is the only
    // rejection retail has for geometry behind the camera, whose vertices
    // arrive saturated to the screen edge with zero depth � ordering-table
    // index zero, drawn last, over the HUD.
    constexpr std::uint16_t x_limit = 597U;
    const stuntmaster::game::WidescreenLowerBounds bounds{x_limit, x_limit};
    const auto patches = bounds.patches();
    assert(patches.size() == 25U);

    for (std::size_t index = 0U; index < patches.size(); ++index) {
        const auto& patch = patches[index];
        for (std::size_t other = index + 1U; other < patches.size(); ++other) {
            assert(patch.address != patches[other].address);
            assert(patch.site != patches[other].site);
        }
    }
    // Fourteen byte-identical outcode blocks and the two model-cull sites are
    // current. The next eight descriptors cover retired NCLIP/final-outcode
    // formats for save normalization; the last is the current whole-block
    // lower bound.
    for (std::size_t index = 0U; index < 14U; ++index) {
        assert(patches[index].original_word == 0x03032823U);
        assert(patches[index].return_address == patches[index].site + 4U);
    }
    assert(patches[14].site == 0x8009E248U);
    assert(patches[15].site == 0x8009E39CU);
    assert(patches[16].site == 0x800A0FC0U);
    assert(patches[17].site == 0x800A113CU);
    assert(patches[18].site == 0x800A12A4U);
    assert(patches[19].site == 0x800A13D8U);
    assert(patches[20].site == 0x800A103CU);
    assert(patches[21].site == 0x800A11B8U);
    assert(patches[22].site == 0x800A1304U);
    assert(patches[23].site == 0x800A1438U);
    assert(patches[24].site == 0x8002A194U);

    // Keep executable coverage for the retired descriptor so normalization
    // continues to identify its exact code shape. A retail-accepted negative
    // NCLIP still advances into the outcode. A
    // rejected winding is also advanced when the whole quad is in front of
    // the eye and reaches a widened side margin, while an in-band backface or
    // any zero-depth vertex retains retail's cull branch.
    constexpr std::uint32_t guard_site = 0x800A0FC0U;
    constexpr std::uint32_t cull_target = 0x800A10BCU;
    constexpr std::uint32_t vertices = 0x80100000U;
    const std::array guard_code{
        0x0441003EU,                 // bgez $v0, cull
        std::uint32_t{0},            // nop
        0x24100001U,                 // addiu $s0, $zero, 1 (submitted)
        encodeR(31, 0, 0, 0, 0x08), // jr $ra
        std::uint32_t{0},
    };
    const std::array cull_code{
        0x24100002U,                 // addiu $s0, $zero, 2 (culled)
        encodeR(31, 0, 0, 0, 0x08), // jr $ra
        std::uint32_t{0},
    };
    const auto runGuard = [&](std::uint32_t outcode,
                              std::size_t zero_depth_vertex,
                              std::int16_t first_x) {
        stuntmaster::psx::R3000Runtime runtime;
        assert(runtime.loadBytes(
            guard_site, std::as_bytes(std::span{guard_code})));
        assert(runtime.loadBytes(
            cull_target, std::as_bytes(std::span{cull_code})));
        for (std::size_t index = 0U; index < 4U; ++index) {
            assert(runtime.write16(
                vertices + static_cast<std::uint32_t>(index * 8U),
                static_cast<std::uint16_t>(index == 0U ? first_x : 0)));
            assert(runtime.write16(
                vertices + static_cast<std::uint32_t>(index * 8U) + 4U,
                index == zero_depth_vertex ? 0U : 10U));
        }
        assert(stuntmaster::game::applyRetailTrampoline(
            runtime, patches[16]));
        runtime.reset(guard_site, 0U, 0x801F0000U);
        runtime.setRegister(2U, outcode);       // $v0
        runtime.setRegister(6U, vertices);      // $a2
        runtime.setRegister(7U, vertices + 8U); // $a3
        runtime.setRegister(8U, vertices + 16U);// $t0
        runtime.setRegister(9U, vertices + 24U);// $t1
        runtime.setRegister(
            31U, stuntmaster::psx::R3000Runtime::return_sentinel);
        for (int executed = 0; executed < 64; ++executed) {
            if (runtime.atReturnSentinel()) {
                break;
            }
            assert(runtime.step().reason ==
                   stuntmaster::psx::R3000StopReason::running);
        }
        assert(runtime.atReturnSentinel());
        runtime.settleLoadDelay();
        return runtime.state().gpr[16U];
    };
    constexpr auto no_zero_depth = std::size_t{4U};
    assert(runGuard(0xFFFFFFFFU, no_zero_depth, 0) == 1U);
    assert(runGuard(0U, no_zero_depth, 0) == 2U);
    assert(runGuard(1234U, no_zero_depth, -1) == 1U);
    assert(runGuard(0U, no_zero_depth, 513) == 1U);
    assert(runGuard(0U, 2U, -1024) == 2U);

    // The final outcode guard advances an ordinary accepted polygon and a
    // shared horizontal rejection with positive depth. It retains shared
    // vertical rejection and the zero-depth near/behind-camera case.
    constexpr std::uint32_t outcode_site = 0x800A103CU;
    constexpr std::uint32_t outcode_cull_target = 0x800A10BCU;
    const auto runOutcodeGuard = [&](std::uint32_t outcode,
                                     std::size_t zero_depth_vertex) {
        stuntmaster::psx::R3000Runtime runtime;
        const std::array site_code{
            0x1440001FU,                 // bnez $v0, cull
            std::uint32_t{0},            // nop
            0x24100001U,                 // submitted
            encodeR(31, 0, 0, 0, 0x08), // jr $ra
            std::uint32_t{0},
        };
        assert(runtime.loadBytes(
            outcode_site, std::as_bytes(std::span{site_code})));
        assert(runtime.loadBytes(
            outcode_cull_target, std::as_bytes(std::span{cull_code})));
        for (std::size_t index = 0U; index < 4U; ++index) {
            assert(runtime.write16(
                vertices + static_cast<std::uint32_t>(index * 8U) + 4U,
                index == zero_depth_vertex ? 0U : 10U));
        }
        assert(stuntmaster::game::applyRetailTrampoline(
            runtime, patches[20]));
        runtime.reset(outcode_site, 0U, 0x801F0000U);
        runtime.setRegister(2U, outcode);
        runtime.setRegister(6U, vertices);
        runtime.setRegister(7U, vertices + 8U);
        runtime.setRegister(8U, vertices + 16U);
        runtime.setRegister(9U, vertices + 24U);
        runtime.setRegister(
            31U, stuntmaster::psx::R3000Runtime::return_sentinel);
        for (int executed = 0; executed < 64; ++executed) {
            if (runtime.atReturnSentinel()) {
                break;
            }
            assert(runtime.step().reason ==
                   stuntmaster::psx::R3000StopReason::running);
        }
        assert(runtime.atReturnSentinel());
        runtime.settleLoadDelay();
        return runtime.state().gpr[16U];
    };
    assert(runOutcodeGuard(0U, no_zero_depth) == 1U);
    assert(runOutcodeGuard(0x00004000U, no_zero_depth) == 1U);
    assert(runOutcodeGuard(0x40000000U, no_zero_depth) == 2U);
    assert(runOutcodeGuard(0x00008000U, 1U) == 2U);

    // The real outcode block, at the real site so the fingerprint applies.
    constexpr std::uint32_t site = 0x800A0FD4U;
    const std::array block{
        0x03032823U,                // subu $a1, $t8, $v1   <- site
        0x00AF2824U,                // and  $a1, $a1, $t7
        0x006D1824U,                // and  $v1, $v1, $t5
        0x00651825U,                // or   $v1, $v1, $a1
        0x00431024U,                // and  $v0, $v0, $v1
        encodeR(31, 0, 0, 0, 0x08), // jr   $ra
        std::uint32_t{0},           // nop
    };
    const auto outcode = [&](const stuntmaster::game::RetailTrampoline* patch,
                             std::int32_t x,
                             std::int32_t y) {
        stuntmaster::psx::R3000Runtime runtime;
        assert(runtime.loadBytes(site, std::as_bytes(std::span{block})));
        if (patch != nullptr) {
            assert(stuntmaster::game::applyRetailTrampoline(runtime, *patch));
        }
        runtime.reset(site, 0, 0x801F0000U);
        runtime.setRegister(24, 0x01000000U | x_limit); // $t8, packed limits
        runtime.setRegister(15, 0x80008000U);           // $t7
        runtime.setRegister(13, 0x40004000U);           // $t5
        runtime.setRegister(2, 0xC000C000U);            // $v0, the seed mask
        runtime.setRegister(
            3,
            static_cast<std::uint32_t>(y) << 16U |
                (static_cast<std::uint32_t>(x) & 0xFFFFU)); // $v1, packed SXY
        runtime.setRegister(
            31, stuntmaster::psx::R3000Runtime::return_sentinel);
        for (int executed = 0; executed < 64; ++executed) {
            if (runtime.atReturnSentinel()) {
                break;
            }
            const auto result = runtime.step();
            assert(result.reason ==
                   stuntmaster::psx::R3000StopReason::running);
        }
        assert(runtime.atReturnSentinel());
        runtime.settleLoadDelay();
        return runtime.state().gpr[2];
    };

    constexpr std::uint32_t left = 0x00004000U;
    constexpr std::uint32_t right = 0x00008000U;
    constexpr std::uint32_t below = 0x40000000U;
    constexpr std::uint32_t above = 0x80000000U;
    const auto* widened = &patches[0];

    // The widening itself: a vertex between the retail edge and the widescreen
    // edge is rejected by retail and accepted once the bound moves.
    assert(outcode(nullptr, -50, 100) == left);
    assert(outcode(widened, -50, 100) == 0U);
    // 512 - 597 is the new edge, so -84 is inside it and -86 is not.
    assert(outcode(widened, -84, 100) == 0U);
    assert(outcode(widened, -86, 100) == left);
    // Geometry behind the camera saturates to the screen edge, and must still
    // be rejected � this is the case that reaches the HUD when it is not.
    assert(outcode(widened, -1024, 100) == left);

    // The right bound reads the unbiased coordinate, so moving the left edge
    // must not shift it. $t8 already carries the widened limit here.
    assert(outcode(widened, 550, 100) == 0U);
    assert(outcode(widened, 700, 100) == right);
    assert(outcode(nullptr, 700, 100) == right);

    // Both Y bounds are untouched at every X.
    assert(outcode(widened, 100, -5) == below);
    assert(outcode(widened, 100, 300) == above);
    // The bias is added to the packed pair, so it can carry out of the X half.
    // Masking it back is what keeps a vertex just left of centre from clearing
    // the Y sign bit of the vertex above it.
    assert(outcode(nullptr, -1, -1) == (left | below));
    assert(outcode(widened, -1, -1) == below);
    assert(outcode(widened, -1, 300) == above);

    // A 4:3 window computes the retail limit back, so the trampolines must
    // reproduce retail's own decision exactly rather than merely nearly.
    const stuntmaster::game::WidescreenLowerBounds neutral{0x200U, 0x200U};
    for (const std::int32_t x : {-1024, -100, -1, 0, 1, 255, 596, 598, 1023}) {
        for (const std::int32_t y : {-1, 0, 100, 300}) {
            assert(outcode(&neutral.patches()[0], x, y) ==
                   outcode(nullptr, x, y));
        }
    }
}



void ledgeTraceDoesNotChangeWhatLedgeCheckDecides() {
    // The trace exists to observe, so the only property that matters is that
    // it observes nothing into existence. This runs the real
    // `Obstacle::LedgeCheck` bytes, patched and unpatched, over the same
    // inputs and requires the verdict and every caller-visible register to
    // match. Sampling trampolines are easy to get subtly wrong � a clobbered
    // live register changes a decision without faulting � and only executing
    // the real function catches that.
    using Runtime = stuntmaster::psx::R3000Runtime;
    constexpr std::uint32_t ledge_check = 0x8007BE84U;
    constexpr std::array<std::uint32_t, 108U> ledge_check_code{
        0x27BDFFA8U, // 8007be84
        0xAFB10044U, // 8007be88
        0x00A08821U, // 8007be8c
        0xAFB40050U, // 8007be90
        0xAFB00040U, // 8007be94
        0x00E08021U, // 8007be98
        0x24020020U, // 8007be9c
        0xAFBF0054U, // 8007bea0
        0xAFB3004CU, // 8007bea4
        0xAFB20048U, // 8007bea8
        0x8E030164U, // 8007beac
        0x8E130050U, // 8007beb0
        0x10620006U, // 8007beb4
        0x00C0A021U, // 8007beb8
        0x24020022U, // 8007bebc
        0x10620003U, // 8007bec0
        0x24020023U, // 8007bec4
        0x14620003U, // 8007bec8
        0x00000000U, // 8007becc
        0x0801F005U, // 8007bed0
        0x00001021U, // 8007bed4
        0x84820006U, // 8007bed8
        0x84830000U, // 8007bedc
        0x8F850858U, // 8007bee0
        0x00431023U, // 8007bee4
        0x0045102AU, // 8007bee8
        0x14400007U, // 8007beec
        0x00009021U, // 8007bef0
        0x8482000AU, // 8007bef4
        0x84830004U, // 8007bef8
        0x00000000U, // 8007befc
        0x00431023U, // 8007bf00
        0x0045102AU, // 8007bf04
        0x38520001U, // 8007bf08
        0x27A40030U, // 8007bf0c
        0x00002821U, // 8007bf10
        0x0C01E0D9U, // 8007bf14
        0x2406000CU, // 8007bf18
        0x26100028U, // 8007bf1c
        0x8E040004U, // 8007bf20
        0x0C01D0FBU, // 8007bf24
        0x00000000U, // 8007bf28
        0xAFA20030U, // 8007bf2c
        0x8E040004U, // 8007bf30
        0x0C01D0FBU, // 8007bf34
        0x24844000U, // 8007bf38
        0x02202021U, // 8007bf3c
        0xAFA20038U, // 8007bf40
        0x8FA80030U, // 8007bf44
        0x8FA90034U, // 8007bf48
        0x8FAA0038U, // 8007bf4c
        0xAFA80020U, // 8007bf50
        0xAFA90024U, // 8007bf54
        0xAFAA0028U, // 8007bf58
        0x8FA80020U, // 8007bf5c
        0x8FA90024U, // 8007bf60
        0x8FAA0028U, // 8007bf64
        0xAFA80010U, // 8007bf68
        0xAFA90014U, // 8007bf6c
        0xAFAA0018U, // 8007bf70
        0x0C024D39U, // 8007bf74
        0x27A50010U, // 8007bf78
        0x24050001U, // 8007bf7c
        0x8E700060U, // 8007bf80
        0x00028FC2U, // 8007bf84
        0x0C01E3A6U, // 8007bf88
        0x02002021U, // 8007bf8c
        0x02002021U, // 8007bf90
        0x24050002U, // 8007bf94
        0x0C01E3A6U, // 8007bf98
        0x00408021U, // 8007bf9c
        0x8E030018U, // 8007bfa0
        0x8C420018U, // 8007bfa4
        0x8F84085CU, // 8007bfa8
        0x8E860004U, // 8007bfac
        0x00621821U, // 8007bfb0
        0x000317C2U, // 8007bfb4
        0x00621821U, // 8007bfb8
        0x00031843U, // 8007bfbc
        0x00641023U, // 8007bfc0
        0x00C2102AU, // 8007bfc4
        0x14400004U, // 8007bfc8
        0x00002821U, // 8007bfcc
        0x00641021U, // 8007bfd0
        0x0046102AU, // 8007bfd4
        0x38450001U, // 8007bfd8
        0x8E620024U, // 8007bfdc
        0x8F830860U, // 8007bfe0
        0x8C420000U, // 8007bfe4
        0x00002021U, // 8007bfe8
        0x00431021U, // 8007bfec
        0x00C2102AU, // 8007bff0
        0x12400006U, // 8007bff4
        0x38420001U, // 8007bff8
        0x12200004U, // 8007bffc
        0x00000000U, // 8007c000
        0x10A00002U, // 8007c004
        0x00000000U, // 8007c008
        0x0082202BU, // 8007c00c
        0x00801021U, // 8007c010
        0x8FBF0054U, // 8007c014
        0x8FB40050U, // 8007c018
        0x8FB3004CU, // 8007c01c
        0x8FB20048U, // 8007c020
        0x8FB10044U, // 8007c024
        0x8FB00040U, // 8007c028
        0x03E00008U, // 8007c02c
        0x27BD0058U, // 8007c030
    };

    constexpr std::uint32_t gp = 0x800DC94CU;
    constexpr std::uint32_t stack = 0x801F0000U;
    constexpr std::uint32_t box = 0x80120000U;
    constexpr std::uint32_t normal = 0x80121000U;
    constexpr std::uint32_t contact = 0x80122000U;
    constexpr std::uint32_t humanoid = 0x80123000U;
    constexpr std::uint32_t model = 0x80124000U;
    constexpr std::uint32_t floor = 0x80125000U;
    constexpr std::uint32_t part = 0x80126000U;
    constexpr std::uint32_t dot_slot = 0x80127000U;

    // Stubs for the four helpers the function calls. Each is `jr $ra` with a
    // chosen result, so the arithmetic under test is the function's own.
    const auto stub = [](Runtime& runtime,
                         std::uint32_t address,
                         std::uint32_t source) {
        const std::array code{
            0x3C020000U | (source >> 16U),        // lui $v0, hi
            0x34420000U | (source & 0xFFFFU),     // ori $v0, $v0, lo
            0x8C420000U,                          // lw  $v0, 0($v0)
            0x00000000U,                          // nop
            0x03E00008U,                          // jr  $ra
            0x00000000U,                          // nop
        };
        assert(runtime.loadBytes(address, std::as_bytes(std::span{code})));
    };

    const auto run = [&](bool traced, std::int32_t dot, std::int32_t state) {
        Runtime runtime;
        assert(runtime.loadBytes(
            ledge_check, std::as_bytes(std::span{ledge_check_code})));
        stub(runtime, 0x80078364U, dot_slot + 0x10U); // memset, result unused
        stub(runtime, 0x800743ECU, dot_slot + 0x10U); // sin
        stub(runtime, 0x800934E4U, dot_slot);         // the facing dot
        stub(runtime, 0x80078E98U, dot_slot + 0x4U);  // model part lookup
        if (traced) {
            // The two LetGoOfLedge counters sit in other functions; the three
            // sample points inside this one are what could disturb it.
            std::size_t applied = 0U;
            for (const auto* patch :
                 stuntmaster::game::ledgeTraceInputPatches()) {
                if (patch->site < ledge_check ||
                    patch->site >= ledge_check +
                            ledge_check_code.size() * sizeof(std::uint32_t)) {
                    continue;
                }
                assert(
                    stuntmaster::game::applyRetailTrampoline(runtime, *patch));
                ++applied;
            }
            assert(applied == 2U);
        }
        assert(runtime.write32(
            dot_slot, static_cast<std::uint32_t>(dot)));
        assert(runtime.write32(dot_slot + 0x4U, part));
        assert(runtime.write32(dot_slot + 0x10U, 0U));
        // A box comfortably wider than the minimum on both axes.
        assert(runtime.write32(box + 0x0U, 0x10000000U));
        assert(runtime.write32(box + 0x4U, 0x10000000U));
        assert(runtime.write32(box + 0x8U, 0x00001000U));
        assert(runtime.write32(gp + 0x858U, 0x40U));  // minimum box extent
        assert(runtime.write32(gp + 0x85CU, 0x200U)); // hand band
        assert(runtime.write32(gp + 0x860U, 0x100U)); // clearance
        assert(runtime.write32(
            humanoid + 0x164U, static_cast<std::uint32_t>(state)));
        assert(runtime.write32(humanoid + 0x68U, 0xFFFFFFF0U)); // velocity.y
        assert(runtime.write32(humanoid + 0x2CU, 0x1000U));     // yaw
        assert(runtime.write32(humanoid + 0x50U, model));
        assert(runtime.write32(model + 0x24U, floor));
        assert(runtime.write32(model + 0x60U, 0U));
        assert(runtime.write32(floor, 0x100U));
        assert(runtime.write32(part + 0x18U, 0x800U)); // hand height
        assert(runtime.write32(contact + 0x4U, 0x820U)); // the contact Y
        assert(runtime.write32(normal + 0x0U, 0xFFFFF000U));
        assert(runtime.write32(normal + 0x4U, 0x10U));
        assert(runtime.write32(normal + 0x8U, 0x800U));

        runtime.reset(ledge_check, gp, stack);
        // Distinctive values in every register the call is free to clobber, so
        // "the patch destroyed something the stock function left alone" is
        // visible rather than inferred.
        for (std::uint32_t index = 1U; index < 32U; ++index) {
            if (index == 28U || index == 29U || index == 31U) {
                continue; // $gp, $sp, and $ra belong to the call itself
            }
            runtime.setRegister(index, 0xC0DE0000U | index);
        }
        const std::array arguments{box, normal, contact, humanoid};
        const auto result = runtime.call(ledge_check, arguments, 4096U);
        assert(result.reason == stuntmaster::psx::R3000StopReason::returned);
        std::array<std::uint32_t, 32U> registers{};
        for (std::uint32_t index = 0U; index < 32U; ++index) {
            registers[index] = runtime.state().gpr[index];
        }
        return registers;
    };

    // A state the function rejects outright, and states it evaluates fully,
    // across both signs of the facing dot.
    for (const std::int32_t state : {0x20, 0x17, 0x13}) {
        for (const std::int32_t dot : {-1, 0, 1, -0x4000, 0x4000}) {
            const auto stock = run(false, dot, state);
            const auto traced = run(true, dot, state);
            // The verdict, and then every register. A sampling trampoline may
            // only use scratch the stock function already destroys: the MIPS
            // ABI says a caller must not rely on $t0-$t9 across a call, but
            // stock `LedgeCheck` never writes $t3 and a caller that happens to
            // survive on that will stop surviving. Comparing the whole file
            // makes the rule mechanical instead of a judgement call.
            assert(stock[2] == traced[2]);
            for (std::uint32_t index = 0U; index < 32U; ++index) {
                // $at is the assembler temporary. No compiled MIPS holds a
                // value in it across an instruction � the assembler itself
                // expands macros through it � so it is the one register a
                // trampoline may use freely, and every trampoline here does.
                if (index == 1U) {
                    continue;
                }
                assert(stock[index] == traced[index]);
            }
        }
    }
}




void ledgeTraceReportsWhichConditionRejectedTheLedge() {
    using Runtime = stuntmaster::psx::R3000Runtime;
    using stuntmaster::game::LedgeCheckFlag;
    const auto& verdict = stuntmaster::game::ledgeTraceCheckVerdict();
    assert(verdict.site == 0x8007C010U);
    assert(verdict.original_word == 0x00801021U); // move $v0, $a0
    assert(verdict.return_address == 0x8007C014U);

    // The tail of Obstacle::LedgeCheck from the last condition onwards, so the
    // registers the trampoline samples are produced by the real instructions.
    constexpr std::uint32_t tail_address = 0x8007BFF4U;
    const std::array tail{
        0x12400006U,                // beq  $s2, $zero, done
        0x38420001U,                // _xori $v0, $v0, 1   ; clearance
        0x12200004U,                // beq  $s1, $zero, done
        0x00000000U,                // _nop
        0x10A00002U,                // beq  $a1, $zero, done
        0x00000000U,                // _nop
        0x0082202BU,                // sltu $a0, $a0, $v0
        // done:
        0x00801021U,                // move $v0, $a0       <- site
        // Retail's epilogue starts here, and this instruction is what runs in
        // the injected jump's delay slot. Reproducing it is the point: the
        // trampoline returns with a jump rather than through $ra precisely
        // because $ra is reloaded underneath it.
        0x8FBF0054U,                // lw   $ra, 0x54($sp) <- return target
        encodeR(31, 0, 0, 0, 0x08), // jr   $ra
        std::uint32_t{0},           // nop
    };

    // `$v0` arrives inverted, exactly as retail's `slt` leaves it.
    const auto run = [&](bool box,
                         bool facing,
                         bool hand_band,
                         bool clearance) {
        constexpr std::uint32_t stack = 0x801F0000U;
        Runtime runtime;
        assert(runtime.loadBytes(
            tail_address, std::as_bytes(std::span{tail})));
        assert(stuntmaster::game::applyRetailTrampoline(runtime, verdict));
        // What retail's epilogue reloads into $ra.
        assert(runtime.write32(stack + 0x54U, Runtime::return_sentinel));
        runtime.reset(tail_address, 0U, stack);
        runtime.setRegister(2, clearance ? 0U : 1U);  // $v0, pre-xori
        runtime.setRegister(4, 0U);                   // $a0
        runtime.setRegister(5, hand_band ? 1U : 0U);  // $a1
        runtime.setRegister(6, 0x1234U);              // $a2, the contact Y
        runtime.setRegister(17, facing ? 1U : 0U);    // $s1
        runtime.setRegister(18, box ? 1U : 0U);       // $s2
        runtime.setRegister(31, Runtime::return_sentinel);
        for (int executed = 0; executed < 128; ++executed) {
            if (runtime.atReturnSentinel()) {
                break;
            }
            const auto result = runtime.step();
            assert(result.reason ==
                   stuntmaster::psx::R3000StopReason::running);
        }
        assert(runtime.atReturnSentinel());
        runtime.settleLoadDelay();
        const auto sample = stuntmaster::game::readLedgeTrace(runtime);
        assert(sample.has_value());
        // The retail return value must survive the trampoline.
        assert(runtime.state().gpr[2] ==
               ((sample->check_flags &
                 static_cast<std::uint32_t>(LedgeCheckFlag::accepted)) != 0U
                    ? 1U
                    : 0U));
        return *sample;
    };

    const auto has = [](const stuntmaster::game::LedgeTraceSample& sample,
                        LedgeCheckFlag flag) {
        return (sample.check_flags & static_cast<std::uint32_t>(flag)) != 0U;
    };

    const auto all = run(true, true, true, true);
    assert(all.check_calls == 1U);
    assert(all.check_contact_y == 0x1234);
    assert(has(all, LedgeCheckFlag::accepted));
    assert(stuntmaster::game::describeLedgeCheckFlags(all.check_flags) ==
           "BOX|FACING|HANDBAND|CLEARANCE|ACCEPTED");

    // Each condition alone must be reported false, and must be the reason the
    // verdict is a rejection. That is the whole point of the trace.
    const auto no_box = run(false, true, true, true);
    assert(!has(no_box, LedgeCheckFlag::box_wide_enough));
    assert(!has(no_box, LedgeCheckFlag::accepted));

    const auto no_facing = run(true, false, true, true);
    assert(!has(no_facing, LedgeCheckFlag::normal_faces_humanoid));
    assert(!has(no_facing, LedgeCheckFlag::accepted));

    const auto no_band = run(true, true, false, true);
    assert(!has(no_band, LedgeCheckFlag::ledge_within_hand_band));
    assert(!has(no_band, LedgeCheckFlag::accepted));
    assert(stuntmaster::game::describeLedgeCheckFlags(no_band.check_flags) ==
           "BOX|FACING|CLEARANCE");

    const auto no_clearance = run(true, true, true, false);
    assert(!has(no_clearance, LedgeCheckFlag::clearance_below));
    assert(!has(no_clearance, LedgeCheckFlag::accepted));

    // The facing sample must publish both operands and the result of the dot,
    // and must leave $a1 as its displaced instruction set it.
    {
        const auto& facing = stuntmaster::game::ledgeTraceFacingInputs();
        assert(facing.site == 0x8007BF7CU);
        assert(facing.original_word == 0x24050001U); // addiu $a1, $zero, 1
        assert(facing.return_address == 0x8007BF84U);
        assert(facing.address != verdict.address);

        constexpr std::uint32_t normal = 0x80130000U;
        constexpr std::uint32_t facing_stack = 0x801E0000U;
        const std::array probe{
            facing.original_word,       // addiu $a1, $zero, 1  <- site
            0x8E700060U,                // _lw   $s0, 0x60($s3) ; delay slot
            0x00028FC2U,                // srl   $s1, $v0, 0x1f ; return target
            encodeR(31, 0, 0, 0, 0x08), // jr    $ra
            std::uint32_t{0},           // nop
        };
        Runtime runtime;
        assert(runtime.loadBytes(
            facing.site, std::as_bytes(std::span{probe})));
        assert(stuntmaster::game::applyRetailTrampoline(runtime, facing));
        assert(runtime.write32(normal + 0x0U, 0xFFFFF000U)); // -4096
        assert(runtime.write32(normal + 0x4U, 0x00000010U));
        assert(runtime.write32(normal + 0x8U, 0x00000800U));
        assert(runtime.write32(facing_stack + 0x10U, 0x00001000U));
        assert(runtime.write32(facing_stack + 0x18U, 0xFFFFFF00U)); // -256
        runtime.reset(facing.site, 0U, facing_stack);
        runtime.setRegister(2, 0xFFFFFFF6U); // $v0, the dot: -10
        runtime.setRegister(5, 0x5A5A5A5AU); // $a1, must be overwritten by 1
        runtime.setRegister(17, normal);     // $s1
        runtime.setRegister(19, 0x80140000U); // $s3, for the delay-slot load
        runtime.setRegister(31, Runtime::return_sentinel);
        for (int executed = 0; executed < 128; ++executed) {
            if (runtime.atReturnSentinel()) {
                break;
            }
            const auto result = runtime.step();
            assert(result.reason ==
                   stuntmaster::psx::R3000StopReason::running);
        }
        assert(runtime.atReturnSentinel());
        runtime.settleLoadDelay();
        assert(runtime.state().gpr[5] == 1U);
        // The sign of the dot is what retail turns into the FACING flag.
        assert(runtime.state().gpr[17] == 1U);
        const auto sample = stuntmaster::game::readLedgeTrace(runtime);
        assert(sample.has_value());
        assert(sample->facing_dot == -10);
        assert(sample->normal_x == -4096);
        assert(sample->normal_y == 16);
        assert(sample->normal_z == 2048);
        assert(sample->facing_x == 4096);
        assert(sample->facing_z == -256);
        assert(stuntmaster::game::revertRetailTrampoline(runtime, facing));
    }

    // Every declared sample point must be in the list callers install from. A
    // point that is declared but never applied reads back as a zeroed arena,
    // which is indistinguishable from a real zero, so this cannot be left to
    // whoever edits the application site.
    {
        std::vector<const stuntmaster::game::RetailTrampoline*> listed;
        for (const auto* patch : stuntmaster::game::ledgeTracePatches()) {
            listed.push_back(patch);
        }
        for (const auto* patch : stuntmaster::game::ledgeTraceInputPatches()) {
            listed.push_back(patch);
        }
        for (const auto* declared : {
                 &stuntmaster::game::ledgeTraceCheckVerdict(),
                 &stuntmaster::game::ledgeTraceHumanoidState(),
                 &stuntmaster::game::ledgeTraceFacingInputs(),
                 &stuntmaster::game::ledgeTraceLetGoFromLatch(),
                 &stuntmaster::game::ledgeTraceLetGoFromTicket(),
             }) {
            assert(
                std::find(listed.begin(), listed.end(), declared) !=
                listed.end());
        }
        assert(listed.size() == 5U);
        // And the whole list must install into one runtime without any two
        // bodies claiming the same arena slot.
        Runtime shared;
        for (const auto* patch : listed) {
            assert(shared.write32(patch->site, patch->original_word));
            assert(stuntmaster::game::applyRetailTrampoline(shared, *patch));
        }
    }

    // Both counters displace a call without changing it, so a hang that ends
    // can be attributed to one of them. The latch counter takes its own
    // `LetGoOfLedge` call; the ticket counter takes the call immediately
    // before, on the same drop-only path, leaving `LetGoOfLedge` itself free.
    assert(
        stuntmaster::game::ledgeTraceLetGoFromLatch().original_word ==
        0x0C01B10BU); // jal 0x8006c42c
    assert(
        stuntmaster::game::ledgeTraceLetGoFromTicket().original_word ==
        0x0C018731U); // jal 0x80061cc4
    for (const auto* patch : {
             &stuntmaster::game::ledgeTraceLetGoFromLatch(),
             &stuntmaster::game::ledgeTraceLetGoFromTicket(),
         }) {
        assert(patch->return_address == patch->site + 8U);
        assert(patch->address != verdict.address);
    }
    assert(stuntmaster::game::ledgeTraceLetGoFromLatch().address !=
           stuntmaster::game::ledgeTraceLetGoFromTicket().address);

    // The counting body must still reach LetGoOfLedge, with the delay slot's
    // argument intact.
    const auto& counted = stuntmaster::game::ledgeTraceLetGoFromTicket();
    // The body makes the call itself, so $ra is clobbered exactly as retail's
    // own `jal` would clobber it. Reloading it from the frame is what every
    // real caller does, and is why displacing the call is safe.
    const std::array call_site{
        counted.original_word,      // jal  0x8006c42c    <- site
        0x03202021U,                // _move $a0, $t9     ; the delay slot
        0x8FBF0010U,                // lw   $ra, 0x10($sp) <- return target
        std::uint32_t{0},           // nop
        encodeR(31, 0, 0, 0, 0x08), // jr   $ra
        std::uint32_t{0},           // nop
    };
    const std::array target{
        encodeR(31, 0, 0, 0, 0x08), // jr $ra
        std::uint32_t{0},           // nop
    };
    Runtime runtime;
    assert(runtime.loadBytes(
        counted.site, std::as_bytes(std::span{call_site})));
    assert(runtime.loadBytes(
        0x80061CC4U, std::as_bytes(std::span{target})));
    assert(stuntmaster::game::applyRetailTrampoline(runtime, counted));
    constexpr std::uint32_t call_stack = 0x801F0000U;
    assert(runtime.write32(call_stack + 0x10U, Runtime::return_sentinel));
    for (std::uint32_t expected = 1U; expected <= 3U; ++expected) {
        runtime.reset(counted.site, 0U, call_stack);
        runtime.setRegister(25, 0xABCD0000U); // $t9, the delay slot's source
        runtime.setRegister(31, Runtime::return_sentinel);
        for (int executed = 0; executed < 128; ++executed) {
            if (runtime.atReturnSentinel()) {
                break;
            }
            const auto result = runtime.step();
            assert(result.reason ==
                   stuntmaster::psx::R3000StopReason::running);
        }
        assert(runtime.atReturnSentinel());
        // The humanoid argument must arrive despite the displaced jal.
        assert(runtime.state().gpr[4] == 0xABCD0000U);
        const auto sample = stuntmaster::game::readLedgeTrace(runtime);
        assert(sample.has_value());
        assert(sample->letgo_from_ticket == expected);
        assert(sample->letgo_from_latch == 0U);
    }
    assert(stuntmaster::game::revertRetailTrampoline(runtime, counted));
}














// The overlay-activation mechanism: each overlay hook's fingerprint decides
// whether its overlay is loaded, and `buildActiveRetimeHooks` assembles the
// live sorted table (boot hooks always, plus fingerprint-matched overlays) that
// `RetimeHooks::find` indexes.
void retimeOverlayHooksActivatePerFingerprint() {
    using Runtime = stuntmaster::psx::R3000Runtime;
    using stuntmaster::game::RetimeHook;
    const auto overlays = stuntmaster::game::retimeOverlayHooks();
    // The seventeen recompute/gate hooks, the seventeen overlay held prologues,
    // and the thirteen Platform divide-based conversion hooks (including the
    // carried-velocity snapshot and bobbed-Y preservation).
    assert(overlays.size() == 47U);
    for (std::size_t index = 1U; index < overlays.size(); ++index) {
        assert(overlays[index - 1U].hook.pc < overlays[index].hook.pc);
    }
    // Every overlay hook must carry a non-empty fingerprint window: an empty one
    // would match unconditionally and fire in the wrong overlay.
    for (const auto& overlay : overlays) {
        assert(!overlay.window.empty());
    }

    // Load only the arrow window: only its fingerprint matches.
    const auto& arrow = [&]() -> const stuntmaster::game::RetimeOverlayHook& {
        for (const auto& overlay : overlays) {
            if (overlay.hook.pc == 0x8001BDACU) {
                return overlay;
            }
        }
        assert(false);
        return overlays.front();
    }();
    Runtime runtime;
    assert(runtime.loadBytes(
        arrow.window_address, std::as_bytes(arrow.window)));
    for (const auto& overlay : overlays) {
        const bool expect = overlay.hook.pc == 0x8001BDACU;
        assert(stuntmaster::game::retimeOverlayHookMatches(runtime, overlay) ==
               expect);
    }

    // buildActiveRetimeHooks merges boot + matched overlays, sorted, no dupes.
    const auto boot = stuntmaster::game::retimeLedgeHooks();
    std::array<RetimeHook, 64U> buffer{};
    const auto count = stuntmaster::game::buildActiveRetimeHooks(
        runtime, boot, overlays, buffer);
    assert(count == boot.size() + 1U); // only arrow matched (its window loaded)
    for (std::size_t index = 1U; index < count; ++index) {
        assert(buffer[index - 1U].pc < buffer[index].pc);
    }
    bool has_arrow = false;
    bool has_platform = false;
    for (std::size_t index = 0U; index < count; ++index) {
        if (buffer[index].pc == 0x8001BDACU) {
            has_arrow = true;
        }
        if (buffer[index].pc == 0x800227D0U) {
            has_platform = true;
        }
    }
    assert(has_arrow && !has_platform);

    // A mismatched window is a different overlay at the same address: excluded.
    Runtime other;
    std::vector<std::uint32_t> foreign(
        arrow.window.begin(), arrow.window.end());
    foreign[4] ^= 0x1U;
    assert(other.loadBytes(
        arrow.window_address, std::as_bytes(std::span{foreign})));
    for (const auto& overlay : overlays) {
        if (overlay.hook.pc == 0x8001BDACU) {
            assert(!stuntmaster::game::retimeOverlayHookMatches(
                other, overlay));
        }
    }

    // The merged table dispatches: arrow is found, an unloaded overlay is not,
    // and boot hooks always are.
    stuntmaster::game::RetimeHooks hooks{
        std::span<const RetimeHook>{buffer.data(), count}};
    assert(hooks.find(0x8001BDACU) != nullptr);
    assert(hooks.find(0x800227D0U) == nullptr);
    assert(hooks.find(boot.front().pc) != nullptr);
}

void butchStompEventCounterUsesTheAuthoredClock() {
    using Runtime = stuntmaster::psx::R3000Runtime;
    using stuntmaster::game::RetimeHook;
    using stuntmaster::game::RetimeHooks;

    // Migrated into the generic Shape-1 table: the site is now the addiu
    // (0x8001ADD4) whose delay slot stores the old value; the counted path
    // models the step and re-issues the store, the held path freezes both.
    constexpr std::uint32_t site = 0x8001ADD4U;
    const auto overlays = stuntmaster::game::retimeOverlayHooks();
    const auto found = std::find_if(
        overlays.begin(), overlays.end(), [](const auto& overlay) {
            return overlay.hook.pc == site;
        });
    assert(found != overlays.end());
    const std::array<RetimeHook, 1U> span{found->hook};

    constexpr std::uint32_t butch = 0x80121000U;
    constexpr std::uint32_t stack = 0x801F0000U;
    constexpr std::uint32_t counter_offset = 0x268U;
    const std::array<std::uint32_t, 4U> window{
        encodeI(0x09, 2, 2, 1),       // addiu $v0,$v0,1, hooked
        encodeI(0x2B, 4, 2, 0x268),   // sw $v0,0x268($a0), delay
        encodeR(31, 0, 0, 0, 0x08),   // jr $ra at the rejoin
        0U,
    };

    const auto run = [&](std::uint32_t divisor, bool held,
                         std::uint32_t old_counter) {
        Runtime runtime;
        RetimeHooks hooks{span};
        assert(runtime.loadBytes(site, std::as_bytes(std::span{window})));
        assert(runtime.write32(butch + counter_offset, old_counter));
        hooks.program(divisor);
        hooks.state().advance_this_step_ = !held;
        runtime.setRetimeHooks(&hooks);
        hooks.setActive(true);
        runtime.reset(site, 0U, stack);
        runtime.setRegister(2, old_counter); // $v0 = old counter
        runtime.setRegister(4, butch);       // $a0 = Butch
        runtime.setRegister(31, Runtime::return_sentinel);
        stuntmaster::psx::R3000ExecutionBoundaries boundaries;
        const auto batch = runtime.runBatch(16U, boundaries);
        assert(batch.reason == stuntmaster::psx::R3000StopReason::running);
        assert(runtime.atReturnSentinel());
        std::uint32_t counter = 0U;
        assert(runtime.read32(butch + counter_offset, counter));
        return std::array<std::uint32_t, 2U>{
            counter, runtime.state().gpr[2]};
    };

    // A held 60 Hz update leaves the private timeline at the previous
    // authored frame (memory and register both).
    assert((run(2U, true, 41U) == std::array<std::uint32_t, 2U>{41U, 41U}));
    // The counted update reaches 42, which is the one-shot landing event.
    assert((run(2U, false, 41U) == std::array<std::uint32_t, 2U>{42U, 42U}));
    // Divisor one remains the original one-increment-per-call behavior.
    assert((run(1U, false, 41U) == std::array<std::uint32_t, 2U>{42U, 42U}));
}

// The authored-rate counter holds keep private per-frame counters on the
// master clock's cadence. Shape 1 (`addiu $vR,$vR,+-1` sites): a held update
// leaves the counter and every later comparison on the old value, a counted
// update is bit-identical to retail, and a divisor of one is retail
// everywhere. Shape 2 (branch-guarded countdowns): a held update jumps to
// the epilogue with both arms' side effects suppressed. The colour-pulse
// call gate and the HUD animated-text overlay hold complete the family.
// Windows use the real retail words the sites were cut from.
void authoredCounterHooksKeepTheirCadence() {
    using Runtime = stuntmaster::psx::R3000Runtime;
    using stuntmaster::game::RetimeHook;
    using stuntmaster::game::RetimeHooks;
    using stuntmaster::game::retimeCounterHooks;

    constexpr std::uint32_t jr_ra = encodeR(31, 0, 0, 0, 0x08);
    constexpr std::uint32_t stack = 0x801F0000U;
    constexpr std::uint32_t result = 0x80130000U;
    const auto marker = [](std::uint32_t tag) {
        return std::array<std::uint32_t, 5U>{
            encodeI(0x0F, 0, 3, 0x8013), // lui   $v1, 0x8013
            encodeI(0x09, 0, 2, tag),    // addiu $v0, $zero, tag
            encodeI(0x2B, 3, 2, 0),      // sw    $v0, 0($v1)
            encodeR(31, 0, 0, 0, 0x08),  // jr    $ra
            0U,
        };
    };

    const auto counters = retimeCounterHooks();
    assert(counters.size() == 13U);
    for (std::size_t index = 1U; index < counters.size(); ++index) {
        assert(counters[index - 1U].pc < counters[index].pc);
    }
    const auto counterByPc = [&](std::uint32_t pc) -> RetimeHook {
        for (const auto& hook : counters) {
            if (hook.pc == pc) {
                return hook;
            }
        }
        assert(false);
        return {};
    };

    // Shape 1, the dominant form: `addiu $v0,$v0,+-1` whose delay slot stores
    // the old value and whose later code reloads the field from memory. The
    // hook must leave register and memory untouched on a held update, model
    // the step and repair the delayed store on a counted one.
    const auto runCounter = [&](std::uint32_t site, std::uint32_t site_word,
                                std::uint32_t delay, std::uint32_t object,
                                std::uint32_t offset, std::uint32_t base_reg,
                                std::uint32_t old_value, std::uint32_t divisor,
                                bool held) {
        Runtime runtime;
        const std::array<RetimeHook, 1U> span{counterByPc(site)};
        RetimeHooks hooks{span};
        const std::array<std::uint32_t, 4U> window{
            site_word, delay, jr_ra, 0U};
        assert(runtime.loadBytes(site, std::as_bytes(std::span{window})));
        assert(runtime.write32(object + offset, old_value));
        hooks.program(divisor);
        hooks.state().advance_this_step_ = !held;
        runtime.setRetimeHooks(&hooks);
        hooks.setActive(true);
        runtime.reset(site, 0U, stack);
        runtime.setRegister(2, old_value);     // $v0 = old
        runtime.setRegister(base_reg, object); // the counter's object
        runtime.setRegister(31, Runtime::return_sentinel);
        for (int executed = 0; executed < 16; ++executed) {
            if (runtime.atReturnSentinel()) {
                break;
            }
            const auto step = runtime.step();
            assert(step.reason == stuntmaster::psx::R3000StopReason::running);
        }
        assert(runtime.atReturnSentinel());
        runtime.settleLoadDelay();
        std::uint32_t stored = 0U;
        assert(runtime.read32(object + offset, stored));
        return std::array<std::uint32_t, 2U>{
            stored, runtime.state().gpr[2]};
    };

    constexpr std::uint32_t addiu_v0_1 = encodeI(0x09, 2, 2, 1);
    constexpr std::uint32_t addiu_v0_m1 = encodeI(0x09, 2, 2, 0xFFFF);
    const auto step_cases =
        [&](std::uint32_t site, std::uint32_t site_word, std::uint32_t delay,
            std::uint32_t object, std::uint32_t offset,
            std::uint32_t base_reg) {
            constexpr std::uint32_t old_value = 41U;
            // Held: memory and $v0 both keep the old value.
            assert((runCounter(site, site_word, delay, object, offset, base_reg,
                               old_value, 2U, true) ==
                    std::array<std::uint32_t, 2U>{old_value, old_value}));
            // Counted: the step lands once, and the delayed store is repaired.
            assert((runCounter(site, site_word, delay, object, offset, base_reg,
                               old_value, 2U, false) ==
                    std::array<std::uint32_t, 2U>{old_value + 1U,
                                                  old_value + 1U}));
            // Divisor one: retail's one-increment-per-call behavior.
            assert((runCounter(site, site_word, delay, object, offset, base_reg,
                               old_value, 1U, false) ==
                    std::array<std::uint32_t, 2U>{old_value + 1U,
                                                  old_value + 1U}));
        };
    // The nine delay-slot-store sites (object register, counter field).
    step_cases(0x80032EE0U, addiu_v0_1, encodeI(0x2B, 16, 2, 0x268),
               0x80121000U, 0x268U, 16U); // Player::_Collapse
    step_cases(0x800331B4U, addiu_v0_1, encodeI(0x2B, 17, 2, 0x268),
               0x80121000U, 0x268U, 17U); // Player::_HorizontalPoleSwing
    step_cases(0x800338B8U, addiu_v0_1, encodeI(0x2B, 16, 2, 0x268),
               0x80121000U, 0x268U, 16U); // Player::_SlopeSlide
    step_cases(0x80068EDCU, addiu_v0_1, encodeI(0x2B, 17, 2, 0x134),
               0x80121000U, 0x134U, 17U); // Humanoid::_Collapse
    step_cases(0x800683D4U, addiu_v0_1, encodeI(0x2B, 4, 2, 0x134),
               0x80121000U, 0x134U, 4U); // _BackGrabCharacterReceivePreLatch
    step_cases(0x80075924U, addiu_v0_1, encodeI(0x2B, 17, 2, 0x40),
               0x80121000U, 0x40U, 17U); // Behaviour::_BackoffAndTaunt
    step_cases(0x80075B2CU, addiu_v0_1, encodeI(0x2B, 16, 2, 0x40),
               0x80121000U, 0x40U, 16U); // Behaviour::_BackOutOfTheFight
    step_cases(0x80074A50U, addiu_v0_1, encodeI(0x2B, 16, 2, 0x48),
               0x80121000U, 0x48U, 16U); // Behaviour::ComplexAttack index
    step_cases(0x80076CB8U, addiu_v0_1, encodeI(0x2B, 19, 2, 0x40),
               0x80121000U, 0x40U, 19U); // Behaviour::NavigateEnemies

    // `_Pause` decrements in the guard branch's delay slot, so the hook sits
    // on the store instead: a held update skips the store (the countdown
    // keeps its old value), a counted one issues it, and a divisor of one is
    // retail's per-call store.
    {
        const auto runStore = [&](std::uint32_t divisor, bool held) {
            Runtime runtime;
            const std::array<RetimeHook, 1U> span{counterByPc(0x800672D8U)};
            RetimeHooks hooks{span};
            const std::array<std::uint32_t, 4U> window{
                encodeI(0x2B, 16, 2, 0x144), // sw $v0, 0x144($s0), hooked
                0U,                          // the real delay slot is lw $ra
                jr_ra,
                0U,
            };
            assert(runtime.loadBytes(
                0x800672D8U, std::as_bytes(std::span{window})));
            assert(runtime.write32(0x80121000U + 0x144U, 41U)); // old value
            hooks.program(divisor);
            hooks.state().advance_this_step_ = !held;
            runtime.setRetimeHooks(&hooks);
            hooks.setActive(true);
            runtime.reset(0x800672D8U, 0U, stack);
            runtime.setRegister(2, 40U);      // $v0, decremented by the guard
            runtime.setRegister(16, 0x80121000U); // $s0 = the humanoid
            runtime.setRegister(31, Runtime::return_sentinel);
            for (int executed = 0; executed < 16; ++executed) {
                if (runtime.atReturnSentinel()) {
                    break;
                }
                const auto step = runtime.step();
                assert(step.reason ==
                       stuntmaster::psx::R3000StopReason::running);
            }
            assert(runtime.atReturnSentinel());
            runtime.settleLoadDelay();
            std::uint32_t stored = 0U;
            assert(runtime.read32(0x80121000U + 0x144U, stored));
            return stored;
        };
        assert(runStore(2U, true) == 41U);  // held: the store is skipped
        assert(runStore(2U, false) == 40U); // counted: the store lands
        assert(runStore(1U, false) == 40U); // divisor one: retail per call
    }

    // `_GotHitFreeForm`'s delay slot is `sltu $v1,$v1,$v0`: the comparison
    // consumed the pre-step value, so a counted update must recompute it with
    // the new counter. Held keeps both the counter and the old comparison.
    // The flip case (threshold == old) discriminates: held stays "not yet",
    // counted flips once the counter passes the threshold.
    {
        constexpr std::uint32_t site = 0x8006C3FCU;
        const auto run = [&](bool held, std::uint32_t old_value) {
            Runtime runtime;
            const std::array<RetimeHook, 1U> span{counterByPc(site)};
            RetimeHooks hooks{span};
            const std::array<std::uint32_t, 4U> window{
                addiu_v0_1,
                encodeR(3, 2, 3, 0, 0x2B), // sltu $v1, $v1, $v0
                jr_ra,
                0U,
            };
            assert(runtime.loadBytes(site, std::as_bytes(std::span{window})));
            hooks.program(2U);
            hooks.state().advance_this_step_ = !held;
            runtime.setRetimeHooks(&hooks);
            hooks.setActive(true);
            runtime.reset(site, 0U, stack);
            runtime.setRegister(2, old_value); // $v0 = old counter
            runtime.setRegister(3, 41U);       // $v1 = free-form time global
            runtime.setRegister(31, Runtime::return_sentinel);
            for (int executed = 0; executed < 16; ++executed) {
                if (runtime.atReturnSentinel()) {
                    break;
                }
                const auto step = runtime.step();
                assert(step.reason ==
                       stuntmaster::psx::R3000StopReason::running);
            }
            assert(runtime.atReturnSentinel());
            runtime.settleLoadDelay();
            return std::array<std::uint32_t, 2U>{
                runtime.state().gpr[2], runtime.state().gpr[3]};
        };
        // Held: counter stays 41, `41 < 41` stays false (not yet).
        assert((run(true, 41U) == std::array<std::uint32_t, 2U>{41U, 0U}));
        // Counted: counter reaches 42, `41 < 42` becomes true.
        assert((run(false, 41U) == std::array<std::uint32_t, 2U>{42U, 1U}));
    }

    // Shape 2: the guard branch's delay slot already decremented `$v0` when
    // the hook fires. Held jumps to the epilogue with no side effects; a
    // counted update models the guard on the pre-decrement value, so the
    // taken and fall-through arms resume exactly as retail's branch would.
    // Each resume target is a three-word `addiu $v0,tag; jr $ra; nop` stub,
    // so the tag observable is `$v0` and stubs never overlap even when the
    // targets are only two instructions apart.
    const auto runGuard = [&](std::uint32_t site, std::uint32_t branch,
                              std::uint32_t rejoin, std::uint32_t taken_target,
                              std::uint32_t epilogue, std::uint32_t old_value,
                              bool held) {
        Runtime runtime;
        const std::array<RetimeHook, 1U> span{counterByPc(site)};
        RetimeHooks hooks{span};
        const std::array<std::uint32_t, 4U> window{
            branch,
            addiu_v0_m1,
            jr_ra, // the window's own rejoin stand-in (unused on held)
            0U,
        };
        const auto stub = [](std::uint32_t tag) {
            return std::array<std::uint32_t, 3U>{
                encodeI(0x09, 0, 2, static_cast<std::uint16_t>(tag)),
                jr_ra,
                0U,
            };
        };
        const auto fall_stub = stub(1U);
        const auto taken_stub = stub(2U);
        const auto end_stub = stub(3U);
        assert(runtime.loadBytes(site, std::as_bytes(std::span{window})));
        assert(runtime.loadBytes(
            rejoin, std::as_bytes(std::span{fall_stub})));
        assert(runtime.loadBytes(
            taken_target, std::as_bytes(std::span{taken_stub})));
        assert(runtime.loadBytes(
            epilogue, std::as_bytes(std::span{end_stub})));
        hooks.program(2U);
        hooks.state().advance_this_step_ = !held;
        runtime.setRetimeHooks(&hooks);
        hooks.setActive(true);
        runtime.reset(site, 0U, stack);
        runtime.setRegister(2, old_value); // $v0 = old countdown
        runtime.setRegister(31, Runtime::return_sentinel);
        for (int executed = 0; executed < 16; ++executed) {
            if (runtime.atReturnSentinel()) {
                break;
            }
            const auto step = runtime.step();
            assert(step.reason == stuntmaster::psx::R3000StopReason::running);
        }
        assert(runtime.atReturnSentinel());
        runtime.settleLoadDelay();
        return runtime.state().gpr[2]; // the resume stub's `addiu $v0, tag`
    };
    // The real retail guard words decode to the expected targets (the hook
    // derives the taken arm from the encoded offset; the interpreter tests
    // below use a wider offset where the real spacing is too tight for two
    // five-word stubs).
    {
        const auto decode = [](std::uint32_t site, std::uint32_t word) {
            const auto imm = static_cast<std::int32_t>(
                static_cast<std::int16_t>(word & 0xFFFFU));
            return site + 4U + static_cast<std::uint32_t>(imm << 2U);
        };
        assert(decode(0x8008EF1CU, 0x04400005U) == 0x8008EF34U);
        assert(decode(0x800768B0U, 0x1C400003U) == 0x800768C0U);
    }
    // hdTtlive: `bltz $v0, 5` (epilogue 0x8008EF34), rejoin 0x8008EF24.
    // Held: the epilogue runs with both guarded arms' side effects skipped.
    assert(runGuard(0x8008EF1CU, 0x04400005U, 0x8008EF24U, 0x8008EF34U,
                    0x8008EF34U, 1U, true) == 3U);
    // Counted with a positive countdown: the guard falls through to the
    // decremented-value branch, exactly as retail's `bnez` would.
    assert(runGuard(0x8008EF1CU, 0x04400005U, 0x8008EF24U, 0x8008EF34U,
                    0x8008EF34U, 1U, false) == 1U);
    // Counted with a negative countdown: retail's `bltz` jumps to the
    // epilogue (the "already expired" skip).
    assert(runGuard(0x8008EF1CU, 0x04400005U, 0x8008EF24U, 0x8008EF34U,
                    0x8008EF34U, static_cast<std::uint32_t>(-1), false) == 3U);
    // NavigateWorld: `bgtz $v0, 3`; the taken arm is pushed to 0x80076974
    // (imm 0x30) so the stubs do not collide; the real imm-3 decode is
    // asserted above.
    assert(runGuard(0x800768B0U, 0x1C400030U, 0x800768B8U, 0x80076974U,
                    0x80076C6CU, 5U, true) == 3U);
    assert(runGuard(0x800768B0U, 0x1C400030U, 0x800768B8U, 0x80076974U,
                    0x80076C6CU, 5U, false) == 2U);
    assert(runGuard(0x800768B0U, 0x1C400030U, 0x800768B8U, 0x80076974U,
                    0x80076C6CU, 0U, false) == 1U);

    // The pause-menu colour pulse gates `jal CalcNextColor` (the only step of
    // the chase accumulator): held skips the call, counted runs it.
    {
        constexpr std::uint32_t site = 0x8005CD1CU;
        constexpr std::uint32_t callee = 0x8005CC44U;
        const auto clock_hooks = stuntmaster::game::retimeClockHooks();
        const auto found = std::find_if(
            clock_hooks.begin(), clock_hooks.end(),
            [](const RetimeHook& hook) { return hook.pc == site; });
        assert(found != clock_hooks.end());
        const auto run = [&](bool held) {
            Runtime runtime;
            const std::array<RetimeHook, 1U> span{*found};
            RetimeHooks hooks{span};
            const std::array<std::uint32_t, 4U> window{
                (0x03U << 26U) | ((callee >> 2U) & 0x03FFFFFFU), // jal
                encodeR(0, 16, 4, 0, 0x21), // move $s0, $a0
                encodeR(8, 0, 0, 0, 0x08),  // jr $t0 (the gate re-writes $ra)
                0U,
            };
            assert(runtime.loadBytes(site, std::as_bytes(std::span{window})));
            const auto calc_marker = marker(1U);
            assert(runtime.loadBytes(
                callee, std::as_bytes(std::span{calc_marker})));
            assert(runtime.write32(result, 0U));
            hooks.program(2U);
            hooks.state().advance_this_step_ = !held;
            runtime.setRetimeHooks(&hooks);
            hooks.setActive(true);
            runtime.reset(site, 0U, stack);
            runtime.setRegister(8, Runtime::return_sentinel); // $t0
            runtime.setRegister(16, 0xABCD0000U); // $s0, delay slot arg
            runtime.setRegister(31, Runtime::return_sentinel);
            for (int executed = 0; executed < 16; ++executed) {
                if (runtime.atReturnSentinel()) {
                    break;
                }
                const auto step = runtime.step();
                assert(step.reason ==
                       stuntmaster::psx::R3000StopReason::running);
            }
            assert(runtime.atReturnSentinel());
            runtime.settleLoadDelay();
            std::uint32_t tag = 0U;
            assert(runtime.read32(result, tag));
            return std::array<std::uint32_t, 2U>{
                tag, runtime.state().gpr[16]};
        };
        assert((run(true) == std::array<std::uint32_t, 2U>{0U, 0xABCD0000U}));
        assert((run(false) == std::array<std::uint32_t, 2U>{1U, 0xABCD0000U}));
    }

    // `CBVEffect::Update` is the third WEffect-family Update override; the
    // vtable dispatches CBV effects to it directly, so the held prologues at
    // `WEffect::Update`/`FWEffect::Update` do not cover its colour/UV
    // animation. The `jal Update__11CBVPrimData` is gated: held skips the
    // animation step, counted runs it.
    {
        constexpr std::uint32_t site = 0x8008CFACU;
        constexpr std::uint32_t callee = 0x80098BE0U;
        const auto clock_hooks = stuntmaster::game::retimeClockHooks();
        const auto found = std::find_if(
            clock_hooks.begin(), clock_hooks.end(),
            [](const RetimeHook& hook) { return hook.pc == site; });
        assert(found != clock_hooks.end());
        const auto run = [&](bool held) {
            Runtime runtime;
            const std::array<RetimeHook, 1U> span{*found};
            RetimeHooks hooks{span};
            const std::array<std::uint32_t, 4U> window{
                (0x03U << 26U) | ((callee >> 2U) & 0x03FFFFFFU), // jal
                0U,                                               // nop
                encodeR(8, 0, 0, 0, 0x08), // jr $t0 (the gate re-writes $ra)
                0U,
            };
            assert(runtime.loadBytes(site, std::as_bytes(std::span{window})));
            const auto anim_marker = marker(1U);
            assert(runtime.loadBytes(
                callee, std::as_bytes(std::span{anim_marker})));
            assert(runtime.write32(result, 0U));
            hooks.program(2U);
            hooks.state().advance_this_step_ = !held;
            runtime.setRetimeHooks(&hooks);
            hooks.setActive(true);
            runtime.reset(site, 0U, stack);
            runtime.setRegister(8, Runtime::return_sentinel); // $t0
            runtime.setRegister(31, Runtime::return_sentinel);
            for (int executed = 0; executed < 16; ++executed) {
                if (runtime.atReturnSentinel()) {
                    break;
                }
                const auto step = runtime.step();
                assert(step.reason ==
                       stuntmaster::psx::R3000StopReason::running);
            }
            assert(runtime.atReturnSentinel());
            runtime.settleLoadDelay();
            std::uint32_t tag = 0U;
            assert(runtime.read32(result, tag));
            return tag;
        };
        assert(run(true) == 0U);  // held: the UV step is skipped
        assert(run(false) == 1U); // counted: the UV step runs
    }

    // The pause-menu decider: `MenuDraw`'s prologue publishes the master
    // decision once per guest update, because `Step__4Time` does not run
    // while a menu state is up and the pause menu's own authored-rate
    // animation (the selected-item colour chase) would otherwise read a stale
    // decision from the last play update — frozen or twice as fast depending
    // on the stale phase. The hook models the displaced `move $s0, $a0` on
    // every call.
    {
        constexpr std::uint32_t site = 0x80029DC0U;
        const auto clock_hooks = stuntmaster::game::retimeClockHooks();
        const auto found = std::find_if(
            clock_hooks.begin(), clock_hooks.end(),
            [](const RetimeHook& hook) { return hook.pc == site; });
        assert(found != clock_hooks.end());
        const auto run = [&](std::uint32_t divisor, std::uint32_t seed_accum) {
            Runtime runtime;
            const std::array<RetimeHook, 1U> span{*found};
            RetimeHooks hooks{span};
            const std::array<std::uint32_t, 4U> window{
                encodeR(0, 16, 4, 0, 0x21), // move $s0, $a0 (hooked)
                encodeI(0x2B, 29, 31, 0x18), // sw $ra, 0x18($sp)
                jr_ra,
                0U,
            };
            assert(runtime.loadBytes(site, std::as_bytes(std::span{window})));
            hooks.program(divisor);
            hooks.state().clock_accum = seed_accum;
            runtime.setRetimeHooks(&hooks);
            hooks.setActive(true);
            runtime.reset(site, 0U, stack);
            runtime.setRegister(4, 0xABCD0000U); // $a0 = the menu manager
            runtime.setRegister(31, Runtime::return_sentinel);
            for (int executed = 0; executed < 16; ++executed) {
                if (runtime.atReturnSentinel()) {
                    break;
                }
                const auto step = runtime.step();
                assert(step.reason ==
                       stuntmaster::psx::R3000StopReason::running);
            }
            assert(runtime.atReturnSentinel());
            runtime.settleLoadDelay();
            return std::array<std::uint32_t, 3U>{
                runtime.state().gpr[16],            // $s0, displaced move
                hooks.state().clock_accum,
                hooks.state().advance_this_step_ ? 1U : 0U};
        };
        // Divisor two: the menu alternates counted/held like play does.
        assert((run(2U, 0U) == std::array<std::uint32_t, 3U>{0xABCD0000U, 1U, 0U}));
        assert((run(2U, 1U) == std::array<std::uint32_t, 3U>{0xABCD0000U, 2U, 1U}));
        // Divisor one: every retail-cadence menu update counts.
        assert((run(1U, 5U) == std::array<std::uint32_t, 3U>{0xABCD0000U, 6U, 1U}));
    }

    // The title loop has its own decider immediately before
    // `TitleScreen::SelfUpdate` calls the shared MenuColorNext gate. It runs
    // neither the play nor menu decider, so this prevents the PRESS START
    // colour pulse from consuming a stale counted phase on every 60 Hz update.
    // The hook also models the displaced colour-field argument calculation.
    {
        constexpr std::uint32_t site = 0x80011938U;
        const auto overlays = stuntmaster::game::retimeOverlayHooks();
        const auto found = std::find_if(
            overlays.begin(), overlays.end(), [](const auto& overlay) {
                return overlay.hook.pc == site;
            });
        assert(found != overlays.end());
        const auto run = [&](std::uint32_t divisor, std::uint32_t seed_accum) {
            Runtime runtime;
            const std::array<RetimeHook, 1U> span{found->hook};
            RetimeHooks hooks{span};
            const std::array<std::uint32_t, 4U> window{
                encodeI(0x09, 16, 4, 0x34),  // addiu $a0,$s0,0x34
                encodeI(0x2B, 29, 31, 0x24), // sw $ra,0x24($sp)
                jr_ra,
                0U,
            };
            assert(runtime.loadBytes(site, std::as_bytes(std::span{window})));
            hooks.program(divisor);
            hooks.state().clock_accum = seed_accum;
            runtime.setRetimeHooks(&hooks);
            hooks.setActive(true);
            runtime.reset(site, 0U, stack);
            runtime.setRegister(16, 0x80120000U); // $s0 = TitleScreen
            runtime.setRegister(31, Runtime::return_sentinel);
            for (int executed = 0; executed < 16; ++executed) {
                if (runtime.atReturnSentinel()) {
                    break;
                }
                const auto step = runtime.step();
                assert(step.reason ==
                       stuntmaster::psx::R3000StopReason::running);
            }
            assert(runtime.atReturnSentinel());
            runtime.settleLoadDelay();
            return std::array<std::uint32_t, 3U>{
                runtime.state().gpr[4],
                hooks.state().clock_accum,
                hooks.state().advance_this_step_ ? 1U : 0U};
        };
        assert((run(2U, 0U) ==
                std::array<std::uint32_t, 3U>{0x80120034U, 1U, 0U}));
        assert((run(2U, 1U) ==
                std::array<std::uint32_t, 3U>{0x80120034U, 2U, 1U}));
        assert((run(1U, 5U) ==
                std::array<std::uint32_t, 3U>{0x80120034U, 6U, 1U}));
    }

    // The HUD animated-text overlay is a whole-`Update` hold (its `+0x38`
    // pause clamp makes a counter-level hold unsafe): a held update returns
    // through the prologue with `$s0` restored; a counted one models the
    // displaced `move $s0, $a0`.
    {
        constexpr std::uint32_t site = 0x8008F66CU;
        const auto objects = stuntmaster::game::retimeObjectHooks();
        const auto found = std::find_if(
            objects.begin(), objects.end(),
            [](const RetimeHook& hook) { return hook.pc == site; });
        assert(found != objects.end());
        const auto run = [&](bool held) {
            Runtime runtime;
            const std::array<RetimeHook, 1U> span{*found};
            RetimeHooks hooks{span};
            const std::array<std::uint32_t, 4U> window{
                encodeR(0, 16, 4, 0, 0x21), // move $s0, $a0 (hooked)
                encodeI(0x2B, 29, 31, 0x14), // sw $ra, 0x14($sp)
                jr_ra,
                0U,
            };
            assert(runtime.loadBytes(site, std::as_bytes(std::span{window})));
            assert(runtime.write32(stack + 0x10U, 0x12345678U)); // saved $s0
            hooks.program(2U);
            hooks.state().advance_this_step_ = !held;
            runtime.setRetimeHooks(&hooks);
            hooks.setActive(true);
            runtime.reset(site, 0U, stack);
            runtime.setRegister(4, 0xABCD0000U); // $a0 = the overlay
            runtime.setRegister(31, Runtime::return_sentinel);
            for (int executed = 0; executed < 16; ++executed) {
                if (runtime.atReturnSentinel()) {
                    break;
                }
                const auto step = runtime.step();
                assert(step.reason ==
                       stuntmaster::psx::R3000StopReason::running);
            }
            assert(runtime.atReturnSentinel());
            runtime.settleLoadDelay();
            return runtime.state().gpr[16];
        };
        assert(run(true) == 0x12345678U);  // restored, frame unwound
        assert(run(false) == 0xABCD0000U); // displaced move modeled
    }

    // The end-of-level tally count-up (`DoScoreTally` adds 0x1C3 per call)
    // runs under the Director-driven play state, a steady handler, so the
    // whole `Update__7hdTally` is a held prologue: held freezes the tally,
    // counted advances it.
    {
        constexpr std::uint32_t site = 0x80090AC8U;
        const auto objects = stuntmaster::game::retimeObjectHooks();
        const auto found = std::find_if(
            objects.begin(), objects.end(),
            [](const RetimeHook& hook) { return hook.pc == site; });
        assert(found != objects.end());
        const auto run = [&](bool held) {
            Runtime runtime;
            const std::array<RetimeHook, 1U> span{*found};
            RetimeHooks hooks{span};
            const std::array<std::uint32_t, 4U> window{
                encodeR(0, 18, 4, 0, 0x21), // move $s2, $a0 (hooked)
                encodeI(0x2B, 29, 31, 0x20), // sw $ra, 0x20($sp)
                jr_ra,
                0U,
            };
            assert(runtime.loadBytes(site, std::as_bytes(std::span{window})));
            assert(runtime.write32(stack + 0x18U, 0x12345678U)); // saved $s2
            hooks.program(2U);
            hooks.state().advance_this_step_ = !held;
            runtime.setRetimeHooks(&hooks);
            hooks.setActive(true);
            runtime.reset(site, 0U, stack);
            runtime.setRegister(4, 0xABCD0000U); // $a0 = the tally
            runtime.setRegister(31, Runtime::return_sentinel);
            for (int executed = 0; executed < 16; ++executed) {
                if (runtime.atReturnSentinel()) {
                    break;
                }
                const auto step = runtime.step();
                assert(step.reason ==
                       stuntmaster::psx::R3000StopReason::running);
            }
            assert(runtime.atReturnSentinel());
            runtime.settleLoadDelay();
            return runtime.state().gpr[18];
        };
        assert(run(true) == 0x12345678U);  // restored, frame unwound
        assert(run(false) == 0xABCD0000U); // displaced move modeled
    }

    // --- BOL tier: boss/behaviour counters in BOL_REL.BIN ---
    constexpr std::array<std::uint32_t, 1U> jr_ra_arr{jr_ra};
    const auto overlayByPc = [](std::uint32_t pc) -> RetimeHook {
        for (const auto& overlay : stuntmaster::game::retimeOverlayHooks()) {
            if (overlay.hook.pc == pc) {
                return overlay.hook;
            }
        }
        assert(false);
        return {};
    };
    // Standard Shape-1 sites: held freezes, counted steps once, divisor one
    // is retail.
    const auto runBolStep = [&](std::uint32_t site, std::uint32_t site_word,
                                std::uint32_t delay, std::uint32_t base_reg,
                                std::uint32_t field, std::uint32_t old_value,
                                std::uint32_t divisor, bool held) {
        Runtime runtime;
        const std::array<RetimeHook, 1U> span{overlayByPc(site)};
        RetimeHooks hooks{span};
        const std::array<std::uint32_t, 4U> window{
            site_word, delay, jr_ra, 0U};
        assert(runtime.loadBytes(site, std::as_bytes(std::span{window})));
        assert(runtime.write32(0x80121000U + field, old_value));
        hooks.program(divisor);
        hooks.state().advance_this_step_ = !held;
        runtime.setRetimeHooks(&hooks);
        hooks.setActive(true);
        runtime.reset(site, 0U, stack);
        runtime.setRegister(2, old_value);
        runtime.setRegister(base_reg, 0x80121000U);
        runtime.setRegister(31, Runtime::return_sentinel);
        for (int executed = 0; executed < 16; ++executed) {
            if (runtime.atReturnSentinel()) {
                break;
            }
            const auto step = runtime.step();
            assert(step.reason == stuntmaster::psx::R3000StopReason::running);
        }
        assert(runtime.atReturnSentinel());
        runtime.settleLoadDelay();
        std::uint32_t stored = 0U;
        assert(runtime.read32(0x80121000U + field, stored));
        return std::array<std::uint32_t, 2U>{
            stored, runtime.state().gpr[2]};
    };
    const auto bolStepCases =
        [&](std::uint32_t site, std::uint32_t site_word, std::uint32_t delay,
            std::uint32_t base_reg, std::uint32_t field, int delta) {
            // held: frozen; counted at divisor two: one step; divisor one:
            // retail per call.
            assert((runBolStep(site, site_word, delay, base_reg, field, 41U,
                               2U, true) ==
                    std::array<std::uint32_t, 2U>{41U, 41U}));
            const auto expected = static_cast<std::uint32_t>(
                static_cast<std::int32_t>(41) + delta);
            assert((runBolStep(site, site_word, delay, base_reg, field, 41U,
                               2U, false) ==
                    std::array<std::uint32_t, 2U>{expected, expected}));
            assert((runBolStep(site, site_word, delay, base_reg, field, 41U,
                               1U, false) ==
                    std::array<std::uint32_t, 2U>{expected, expected}));
        };
    // Boss get-up (+0x134), Butch decision (+0x40), Butch charge (+0x40),
    // Paul recovery (+0x68 countdown).
    bolStepCases(0x8001A9B8U, 0x24420001U, 0xAE020134U, 16U, 0x134U, +1);
    bolStepCases(0x8001CD3CU, 0x24420001U, 0xAE020040U, 16U, 0x40U, +1);
    bolStepCases(0x8001D20CU, 0x24420001U, 0xAE020040U, 16U, 0x40U, +1);
    bolStepCases(0x8001D948U, 0x2442FFFFU, 0xAEC20068U, 22U, 0x68U, -1);

    // Two-deep sites: the addiu's delay slot is a branch and the store sits
    // in the branch's own delay slot. The hook decodes the branch target and
    // performs the store itself; the test stands a `jr $ra` on the resume
    // path. Held resumes at the branch's decision on the old value.
    const auto runBolTwoDeep = [&](std::uint32_t site, std::uint32_t site_word,
                                   std::uint32_t branch, std::uint32_t store,
                                   std::uint32_t base_reg,
                                   std::uint32_t field,
                                   std::uint32_t resume_held_taken,
                                   std::uint32_t resume_held_fall,
                                   std::uint32_t old_value,
                                   std::uint32_t divisor, bool held) {
        Runtime runtime;
        const std::array<RetimeHook, 1U> span{overlayByPc(site)};
        RetimeHooks hooks{span};
        const std::array<std::uint32_t, 5U> window{
            site_word, branch, store, jr_ra, 0U};
        assert(runtime.loadBytes(site, std::as_bytes(std::span{window})));
        assert(runtime.loadBytes(
            resume_held_taken, std::as_bytes(std::span{jr_ra_arr})));
        assert(runtime.loadBytes(
            resume_held_fall, std::as_bytes(std::span{jr_ra_arr})));
        assert(runtime.write32(0x80121000U + field, old_value));
        hooks.program(divisor);
        hooks.state().advance_this_step_ = !held;
        runtime.setRetimeHooks(&hooks);
        hooks.setActive(true);
        runtime.reset(site, 0U, stack);
        runtime.setRegister(2, old_value);
        runtime.setRegister(base_reg, 0x80121000U);
        runtime.setRegister(31, Runtime::return_sentinel);
        for (int executed = 0; executed < 16; ++executed) {
            if (runtime.atReturnSentinel()) {
                break;
            }
            const auto step = runtime.step();
            assert(step.reason == stuntmaster::psx::R3000StopReason::running);
        }
        assert(runtime.atReturnSentinel());
        runtime.settleLoadDelay();
        std::uint32_t stored = 0U;
        assert(runtime.read32(0x80121000U + field, stored));
        return std::array<std::uint32_t, 2U>{
            stored, runtime.state().gpr[2]};
    };
    // `_MissileAttack`'s `addiu; j 0x8001bebc; sw` loop-tail counter (+0x268).
    assert((runBolTwoDeep(0x8001C294U, 0x24420001U, 0x08006FAFU,
                          0xAE620268U, 19U, 0x268U, 0x8001BEBCU,
                          0x8001BEBCU, 41U, 2U, true) ==
            std::array<std::uint32_t, 2U>{41U, 41U}));
    assert((runBolTwoDeep(0x8001C294U, 0x24420001U, 0x08006FAFU,
                          0xAE620268U, 19U, 0x268U, 0x8001BEBCU,
                          0x8001BEBCU, 41U, 2U, false) ==
            std::array<std::uint32_t, 2U>{42U, 42U}));
    // `CounterAttack`'s `addiu; j 0x8001ee90; sw` per-attack frames (+0x94).
    assert((runBolTwoDeep(0x8001EE38U, 0x24420001U, 0x08007BA4U,
                          0xAC620094U, 3U, 0x94U, 0x8001EE90U,
                          0x8001EE90U, 41U, 2U, true) ==
            std::array<std::uint32_t, 2U>{41U, 41U}));
    assert((runBolTwoDeep(0x8001EE38U, 0x24420001U, 0x08007BA4U,
                          0xAC620094U, 3U, 0x94U, 0x8001EE90U,
                          0x8001EE90U, 41U, 2U, false) ==
            std::array<std::uint32_t, 2U>{42U, 42U}));
    // `_PaulDMS`'s `addiu; blez; sw` spotlight-attack countdown (+0x64):
    // counted with old 2 keeps it alive (blez not taken), counted with old 1
    // lets it expire (blez taken), held defers the expiry.
    assert((runBolTwoDeep(0x8001DAD0U, 0x2442FFFFU, 0x1840000FU,
                          0xAEC20064U, 22U, 0x64U, 0x8001DB14U,
                          0x8001DADCU, 2U, 2U, false) ==
            std::array<std::uint32_t, 2U>{1U, 1U}));
    assert((runBolTwoDeep(0x8001DAD0U, 0x2442FFFFU, 0x1840000FU,
                          0xAEC20064U, 22U, 0x64U, 0x8001DB14U,
                          0x8001DADCU, 1U, 2U, false) ==
            std::array<std::uint32_t, 2U>{0U, 0U}));
    assert((runBolTwoDeep(0x8001DAD0U, 0x2442FFFFU, 0x1840000FU,
                          0xAEC20064U, 22U, 0x64U, 0x8001DB14U,
                          0x8001DADCU, 1U, 2U, true) ==
            std::array<std::uint32_t, 2U>{1U, 1U}));

    // `_TargetMissileAttack`'s retarget delay (+0x298): the store sits in an
    // unconditional `j`'s delay slot, so a held update undoes it. The
    // machinery runs the store (memory 42) before the hook; counted leaves it,
    // held restores `$v0 - 1`.
    {
        constexpr std::uint32_t site = 0x8001C3D8U;
        constexpr std::uint32_t rejoin = 0x8001C6E8U;
        constexpr std::uint32_t dante = 0x80121000U;
        const auto run = [&](bool held) {
            Runtime runtime;
            const std::array<RetimeHook, 1U> span{overlayByPc(site)};
            RetimeHooks hooks{span};
            const std::array<std::uint32_t, 4U> window{
                0x080071BAU, // j 0x8001c6e8, hooked
                0xAE820298U, // sw $v0, 0x298($s4), delay slot
                jr_ra,
                0U,
            };
            assert(runtime.loadBytes(site, std::as_bytes(std::span{window})));
            assert(runtime.loadBytes(
                rejoin, std::as_bytes(std::span{jr_ra_arr})));
            assert(runtime.write32(dante + 0x298U, 99U));
            hooks.program(2U);
            hooks.state().advance_this_step_ = !held;
            runtime.setRetimeHooks(&hooks);
            hooks.setActive(true);
            runtime.reset(site, 0U, stack);
            runtime.setRegister(2, 42U); // $v0 = new (increment already ran)
            runtime.setRegister(20, dante); // $s4
            runtime.setRegister(31, Runtime::return_sentinel);
            for (int executed = 0; executed < 16; ++executed) {
                if (runtime.atReturnSentinel()) {
                    break;
                }
                const auto step = runtime.step();
                assert(step.reason ==
                       stuntmaster::psx::R3000StopReason::running);
            }
            assert(runtime.atReturnSentinel());
            runtime.settleLoadDelay();
            std::uint32_t stored = 0U;
            assert(runtime.read32(dante + 0x298U, stored));
            return stored;
        };
        assert(run(false) == 42U); // counted: the delay-slot store stands
        assert(run(true) == 41U);  // held: the store is undone
    }

    // The henchman engage delay (global gp+0xCC8): a store-site hold whose
    // `j` delay slot carries execution to its target.
    {
        constexpr std::uint32_t site = 0x8001EA94U;
        constexpr std::uint32_t resume = 0x8001ED98U;
        constexpr std::uint32_t gp = 0x800DC94CU;
        constexpr std::uint32_t global = gp + 0xCC8U;
        const auto run = [&](std::uint32_t divisor, bool held) {
            Runtime runtime;
            const std::array<RetimeHook, 1U> span{overlayByPc(site)};
            RetimeHooks hooks{span};
            const std::array<std::uint32_t, 4U> window{
                0xAF820CC8U, // sw $v0, 0xcc8($gp), hooked
                0x08007B66U, // j 0x8001ed98, delay slot
                jr_ra,
                0U,
            };
            assert(runtime.loadBytes(site, std::as_bytes(std::span{window})));
            assert(runtime.loadBytes(
                resume, std::as_bytes(std::span{jr_ra_arr})));
            assert(runtime.write32(global, 41U));
            hooks.program(divisor);
            hooks.state().advance_this_step_ = !held;
            runtime.setRetimeHooks(&hooks);
            hooks.setActive(true);
            runtime.reset(site, gp, stack);
            runtime.setRegister(2, 42U); // $v0 = new (addiu already ran)
            runtime.setRegister(31, Runtime::return_sentinel);
            for (int executed = 0; executed < 16; ++executed) {
                if (runtime.atReturnSentinel()) {
                    break;
                }
                const auto step = runtime.step();
                assert(step.reason ==
                       stuntmaster::psx::R3000StopReason::running);
            }
            assert(runtime.atReturnSentinel());
            runtime.settleLoadDelay();
            std::uint32_t stored = 0U;
            assert(runtime.read32(global, stored));
            return stored;
        };
        assert(run(2U, true) == 41U);  // held: the store is skipped
        assert(run(2U, false) == 42U); // counted: the store lands
        assert(run(1U, false) == 42U); // divisor one: retail per call
    }

    // The pushable push gate: `Pushable::HandleHumanoidCollision` runs on
    // every guest update for an active pusher, so the displacement and the
    // engage counter must hold on held updates. Counted models the engage
    // branch (`$v1` = old counter < 5); held undoes the delay-slot counter
    // store, re-issues the contact bit, and skips the displacement.
    {
        constexpr std::uint32_t site = 0x80018EFCU;
        constexpr std::uint32_t rejoin = 0x80018F04U;      // displacement
        constexpr std::uint32_t skip = 0x80019178U;        // +0x9C latch
        constexpr std::uint32_t pushable = 0x80121000U;
        const auto run = [&](std::uint32_t divisor, bool held,
                             std::uint32_t v1, std::uint32_t old_counter) {
            Runtime runtime;
            const std::array<RetimeHook, 1U> span{overlayByPc(site)};
            RetimeHooks hooks{span};
            const std::array<std::uint32_t, 4U> window{
                0x1460009EU, // bnez $v1, 0x80019178, hooked
                0xAE2200A4U, // sw $v0, 0xa4($s1), delay slot
                jr_ra,
                0U,
            };
            assert(runtime.loadBytes(site, std::as_bytes(std::span{window})));
            assert(runtime.loadBytes(
                rejoin, std::as_bytes(std::span{jr_ra_arr})));
            assert(runtime.loadBytes(
                skip, std::as_bytes(std::span{jr_ra_arr})));
            assert(runtime.write32(pushable + 0xA4U, old_counter));
            assert(runtime.write32(pushable + 0x170U, 0U));
            hooks.program(divisor);
            hooks.state().advance_this_step_ = !held;
            runtime.setRetimeHooks(&hooks);
            hooks.setActive(true);
            runtime.reset(site, 0U, stack);
            runtime.setRegister(3, v1);             // $v1 = (old < 5)
            runtime.setRegister(2, old_counter + 1U); // $v0 = old + 1
            runtime.setRegister(17, pushable);      // $s1
            runtime.setRegister(18, 0x80122000U);   // $s2 = humanoid
            runtime.setRegister(31, Runtime::return_sentinel);
            for (int executed = 0; executed < 16; ++executed) {
                if (runtime.atReturnSentinel()) {
                    break;
                }
                const auto step = runtime.step();
                assert(step.reason ==
                       stuntmaster::psx::R3000StopReason::running);
            }
            assert(runtime.atReturnSentinel());
            runtime.settleLoadDelay();
            std::uint32_t counter = 0U;
            std::uint32_t contact = 0U;
            assert(runtime.read32(pushable + 0xA4U, counter));
            assert(runtime.read32(0x80122000U + 0x170U, contact));
            return std::array<std::uint32_t, 3U>{
                counter, contact, runtime.state().gpr[2]};
        };
        // Counted, not yet engaged (old < 5): the counter stores, branch
        // taken to the latch, no contact bit.
        assert((run(2U, false, 1U, 3U) ==
                std::array<std::uint32_t, 3U>{4U, 0U, 4U}));
        // Counted, engaged (old >= 5): falls into the displacement.
        assert((run(2U, false, 0U, 6U) ==
                std::array<std::uint32_t, 3U>{7U, 0U, 7U}));
        // Held: counter frozen, contact bit re-issued, displacement skipped.
        assert((run(2U, true, 0U, 6U) ==
                std::array<std::uint32_t, 3U>{6U, 4U, 7U}));
        // Divisor one: retail per call.
        assert((run(1U, false, 0U, 6U) ==
                std::array<std::uint32_t, 3U>{7U, 0U, 7U}));
    }
}

void obstacleCollisionGateServicesContactsThatCannotWait() {
    using Runtime = stuntmaster::psx::R3000Runtime;
    using stuntmaster::game::RetimeHook;
    using stuntmaster::game::RetimeHooks;

    const auto clock_hooks = stuntmaster::game::retimeClockHooks();
    const auto found = std::find_if(
        clock_hooks.begin(), clock_hooks.end(), [](const RetimeHook& hook) {
            return hook.pc == 0x80055D80U;
        });
    assert(found != clock_hooks.end());
    const std::array<RetimeHook, 1U> span{*found};

    constexpr std::uint32_t site = 0x80055D80U;
    constexpr std::uint32_t list_callee = 0x800A96ECU;
    constexpr std::uint32_t humanoid_callee = 0x8007C178U;
    constexpr std::uint32_t list = 0x80120000U;
    constexpr std::uint32_t humanoid = 0x80121000U;
    constexpr std::uint32_t npc = 0x80122000U;
    constexpr std::uint32_t result = 0x80130000U;
    constexpr std::uint32_t stack = 0x801F0000U;
    constexpr std::uint32_t AS_STAND = 1U;
    constexpr std::uint32_t AS_RUN_JUMP = 0x06U;
    constexpr std::uint32_t AS_JUMP = 0x08U;
    constexpr std::uint32_t AS_RUN = 0x0AU;
    constexpr std::uint32_t AS_FALL = 0x0DU;
    constexpr std::uint32_t AS_HARD_FALL = 0x0EU;
    constexpr std::uint32_t AS_PUSH_OBJECT = 0x13U;
    constexpr std::uint32_t AS_LADDER_LATCH_TOP = 0x19U;
    constexpr std::uint32_t AS_LADDER_LATCH = 0x1AU;
    constexpr std::uint32_t AS_LADDER_DISMOUNT = 0x1BU;
    constexpr std::uint32_t AS_CLIMB_LADDER = 0x1CU;
    constexpr std::uint32_t AS_HOTFOOT = 0x1EU;

    // The real site shape: jal wrapper; move $a0,$s0; then a synthetic return.
    const std::array<std::uint32_t, 4U> window{
        (0x03U << 26U) | ((list_callee >> 2U) & 0x03FFFFFFU),
        0x02002021U, // move $a0, $s0
        0x01000008U, // jr   $t0
        0U,
    };
    const auto marker = [](std::uint32_t tag) {
        return std::array<std::uint32_t, 5U>{
            encodeI(0x0F, 0, 3, 0x8013), // lui   $v1, 0x8013
            encodeI(0x09, 0, 2, tag),    // addiu $v0, $zero, tag
            encodeI(0x2B, 3, 2, 0),      // sw    $v0, 0($v1)
            encodeR(31, 0, 0, 0, 0x08),  // jr    $ra
            0U,
        };
    };

    const auto run = [&](bool held, std::uint32_t action_state,
                         bool has_passenger_ticket) {
        Runtime runtime;
        RetimeHooks hooks{span};
        assert(runtime.loadBytes(site, std::as_bytes(std::span{window})));
        const auto list_marker = marker(1U);
        const auto humanoid_marker = marker(2U);
        assert(runtime.loadBytes(
            list_callee, std::as_bytes(std::span{list_marker})));
        assert(runtime.loadBytes(
            humanoid_callee, std::as_bytes(std::span{humanoid_marker})));
        assert(runtime.write32(list, humanoid));
        assert(runtime.write32(humanoid, 0U)); // list terminator
        assert(runtime.write32(humanoid + 0x58U, 1U << 6U));
        assert(runtime.write32(humanoid + 0x164U, action_state));
        assert(runtime.write32(
            humanoid + 0xBCU, has_passenger_ticket ? 0x80122000U : 0U));
        assert(runtime.write32(result, 0U));

        hooks.program(2U);
        hooks.state().advance_this_step_ = !held;
        runtime.setRetimeHooks(&hooks);
        hooks.setActive(true);
        runtime.reset(site, 0U, stack);
        runtime.setRegister(8, Runtime::return_sentinel); // $t0
        runtime.setRegister(16, list);                    // $s0
        runtime.setRegister(31, Runtime::return_sentinel);
        for (int executed = 0; executed < 64; ++executed) {
            if (runtime.atReturnSentinel()) {
                break;
            }
            const auto step = runtime.step();
            assert(step.reason == stuntmaster::psx::R3000StopReason::running);
        }
        assert(runtime.atReturnSentinel());
        runtime.settleLoadDelay();
        std::uint32_t tag = 0U;
        assert(runtime.read32(result, tag));
        return tag;
    };

    assert(run(false, AS_STAND, false) == 1U); // counted: full list pass
    assert(run(true, AS_STAND, true) == 0U);   // held: standing rider stays gated
    assert(run(true, AS_RUN, true) == 2U); // moving contact cannot wait
    assert(run(true, AS_PUSH_OBJECT, false) == 2U); // refresh active pusher
    // A jump that began after the preceding counted collision still owns its
    // passenger ticket on this held update. Service the inner collision now so
    // Platform::MovePassengers cannot pull it through another sub-step.
    assert(run(true, AS_RUN_JUMP, true) == 2U);
    assert(run(true, AS_JUMP, true) == 2U);
    // An already-disembarked jumper still needs the held collision pass: its
    // sub-step can cross a thin dynamic surface before the next authored tick.
    assert(run(true, AS_JUMP, false) == 2U);
    // A normal jump changes to Fall, then HardFall, before a sufficiently low
    // landing. Keep servicing dynamic geometry throughout that descent; the
    // falling.stsm repro crosses its pushable during one of these held states.
    assert(run(true, AS_FALL, false) == 2U);
    assert(run(true, AS_HARD_FALL, false) == 2U);
    // Ladder::HandleHumanoidCollision publishes contact bit 0x170:1, which
    // each ladder state consumes on the following update. The game clears it
    // every update, so all four states need the inner pass even when held.
    assert(run(true, AS_LADDER_LATCH_TOP, false) == 2U);
    assert(run(true, AS_LADDER_LATCH, false) == 2U);
    assert(run(true, AS_LADDER_DISMOUNT, false) == 2U);
    assert(run(true, AS_CLIMB_LADDER, false) == 2U);
    // Untouchable (fire pit) publishes contact bit 0x170:3, which keeps
    // AS_Hotfoot alive; Think__8Humanoid clears the word every update. A
    // held update must re-issue the bit or the burning humanoid exits to
    // AS_Run and the run exception re-ignites it: the run/burn flicker on
    // fire pits. The bit is set directly (no inner collision) so the burn
    // damage tick (authored via the pit's +0x90 counter) does not double.
    assert(run(true, AS_HOTFOOT, false) == 0U);
    {
        Runtime runtime;
        RetimeHooks hooks{span};
        assert(runtime.loadBytes(site, std::as_bytes(std::span{window})));
        const auto humanoid_marker = marker(2U);
        assert(runtime.loadBytes(
            humanoid_callee, std::as_bytes(std::span{humanoid_marker})));
        assert(runtime.write32(list, humanoid));
        assert(runtime.write32(humanoid, 0U)); // list terminator
        assert(runtime.write32(humanoid + 0x58U, 1U << 6U));
        assert(runtime.write32(humanoid + 0x164U, AS_HOTFOOT));
        assert(runtime.write32(humanoid + 0x170U, 0U));
        hooks.program(2U);
        hooks.state().advance_this_step_ = false; // held
        runtime.setRetimeHooks(&hooks);
        hooks.setActive(true);
        runtime.reset(site, 0U, stack);
        runtime.setRegister(8, Runtime::return_sentinel); // $t0
        runtime.setRegister(16, list);                    // $s0
        runtime.setRegister(31, Runtime::return_sentinel);
        for (int executed = 0; executed < 64; ++executed) {
            if (runtime.atReturnSentinel()) {
                break;
            }
            const auto step = runtime.step();
            assert(step.reason == stuntmaster::psx::R3000StopReason::running);
        }
        assert(runtime.atReturnSentinel());
        runtime.settleLoadDelay();
        std::uint32_t context = 0U;
        assert(runtime.read32(humanoid + 0x170U, context));
        assert((context & 8U) != 0U); // fire contact re-issued on held
    }
    {
        // Regression for debug-npc-onfire.stsm: the burning player precedes a
        // burning NPC in the humanoid list. Refreshing the player's contact
        // must not return early and starve the later NPC's fire contact.
        Runtime runtime;
        RetimeHooks hooks{span};
        assert(runtime.loadBytes(site, std::as_bytes(std::span{window})));
        const auto humanoid_marker = marker(2U);
        assert(runtime.loadBytes(
            humanoid_callee, std::as_bytes(std::span{humanoid_marker})));
        assert(runtime.write32(list, humanoid));
        assert(runtime.write32(humanoid, npc));
        assert(runtime.write32(npc, 0U));
        assert(runtime.write32(humanoid + 0x58U, 1U << 6U));
        assert(runtime.write32(humanoid + 0x164U, AS_HOTFOOT));
        assert(runtime.write32(humanoid + 0x170U, 0U));
        assert(runtime.write32(npc + 0x58U, 1U << 6U));
        assert(runtime.write32(npc + 0x164U, AS_HOTFOOT));
        assert(runtime.write32(npc + 0x170U, 0U));
        assert(runtime.write32(result, 0U));
        hooks.program(2U);
        hooks.state().advance_this_step_ = false; // held
        runtime.setRetimeHooks(&hooks);
        hooks.setActive(true);
        runtime.reset(site, 0U, stack);
        runtime.setRegister(8, Runtime::return_sentinel); // $t0
        runtime.setRegister(16, list);                    // $s0
        runtime.setRegister(31, Runtime::return_sentinel);
        for (int executed = 0; executed < 64; ++executed) {
            if (runtime.atReturnSentinel()) {
                break;
            }
            const auto step = runtime.step();
            assert(step.reason == stuntmaster::psx::R3000StopReason::running);
        }
        assert(runtime.atReturnSentinel());
        runtime.settleLoadDelay();
        std::uint32_t tag = 0U;
        std::uint32_t player_context = 0U;
        std::uint32_t npc_context = 0U;
        assert(runtime.read32(result, tag));
        assert(runtime.read32(humanoid + 0x170U, player_context));
        assert(runtime.read32(npc + 0x170U, npc_context));
        assert(tag == 0U); // fire refreshes do not run the damage collision
        assert((player_context & 8U) != 0U);
        assert((npc_context & 8U) != 0U);
    }
}

void conveyorCarryRunsOnlyOnAuthoredUpdates() {
    using Runtime = stuntmaster::psx::R3000Runtime;
    using stuntmaster::game::RetimeHook;
    using stuntmaster::game::RetimeHooks;

    constexpr std::uint32_t site = 0x8001C2DCU;
    constexpr std::uint32_t rejoin = 0x8001C2E4U;
    constexpr std::uint32_t humanoid = 0x80121000U;
    constexpr std::uint32_t result = 0x80122000U;
    constexpr std::uint32_t stack = 0x801F0000U;

    const auto overlays = stuntmaster::game::retimeOverlayHooks();
    const auto found = std::find_if(
        overlays.begin(), overlays.end(), [](const auto& overlay) {
            return overlay.hook.pc == site;
        });
    assert(found != overlays.end());
    const std::array<RetimeHook, 1U> span{found->hook};

    // The real hook site is Conveyor::HandleHumanoidCollision's first load
    // after its frame setup. The synthetic rejoin publishes the loaded flags,
    // standing in for the direct position-add body that follows in retail.
    const std::array<std::uint32_t, 2U> site_words{
        encodeI(0x23, 4, 2, 0x58), // lw $v0,0x58($a0), hooked
        0U,                        // nop, delay slot
    };
    const std::array<std::uint32_t, 4U> rejoin_words{
        encodeI(0x2B, 8, 2, 0),     // sw $v0,0($t0)
        encodeR(31, 0, 0, 0, 0x08), // jr $ra
        0U,
        0U,
    };

    const auto run = [&](std::uint32_t divisor, bool held) {
        Runtime runtime;
        RetimeHooks hooks{span};
        assert(runtime.loadBytes(
            site, std::as_bytes(std::span{site_words})));
        assert(runtime.loadBytes(
            rejoin, std::as_bytes(std::span{rejoin_words})));
        assert(runtime.write32(humanoid + 0x58U, 0x1187CU));
        assert(runtime.write32(result, 0U));
        hooks.program(divisor);
        hooks.state().advance_this_step_ = !held;
        runtime.setRetimeHooks(&hooks);
        hooks.setActive(true);
        runtime.reset(site, 0U, stack);
        runtime.setRegister(4, humanoid); // $a0 after the handler's two moves
        runtime.setRegister(8, result);   // $t0, synthetic result address
        runtime.setRegister(31, Runtime::return_sentinel);
        for (int executed = 0; executed < 16; ++executed) {
            if (runtime.atReturnSentinel()) {
                break;
            }
            const auto step = runtime.step();
            assert(step.reason == stuntmaster::psx::R3000StopReason::running);
        }
        assert(runtime.atReturnSentinel());
        runtime.settleLoadDelay();
        std::uint32_t stored = 0U;
        assert(runtime.read32(result, stored));
        return std::array<std::uint32_t, 2U>{
            stored, runtime.state().gpr[29]};
    };

    // The AS_Run held-update contact pass reaches the handler but returns
    // before its full authored-frame carry. Counted and retail calls continue.
    assert((run(2U, true) ==
            std::array<std::uint32_t, 2U>{0U, stack + 0x28U}));
    assert((run(2U, false) ==
            std::array<std::uint32_t, 2U>{0x1187CU, stack}));
    assert((run(1U, false) ==
            std::array<std::uint32_t, 2U>{0x1187CU, stack}));
}

void runningDynamicPassengerDoesNotAccumulateGravity() {
    using Runtime = stuntmaster::psx::R3000Runtime;
    using stuntmaster::game::RetimeHook;
    using stuntmaster::game::RetimeHooks;

    const auto motion_hooks = stuntmaster::game::retimeMotionHooks();
    const auto found = std::find_if(
        motion_hooks.begin(), motion_hooks.end(), [](const RetimeHook& hook) {
            return hook.pc == 0x80062340U;
        });
    assert(found != motion_hooks.end());
    const std::array<RetimeHook, 1U> span{*found};

    constexpr std::uint32_t site = 0x80062340U;
    constexpr std::uint32_t stack = 0x801F0000U;
    constexpr std::uint32_t humanoid = 0x80121000U;
    constexpr std::uint32_t AS_RUN = 0x0AU;
    constexpr std::uint32_t AS_FALL = 0x0DU;
    const std::array<std::uint32_t, 4U> window{
        0U,                                     // hooked sw $v0,0x68($s1)
        encodeI(0x23, 17, 2, 0x6C),             // lw $v0,0x6c($s1), delay
        encodeR(31, 0, 0, 0, 0x08),             // jr $ra at the rejoin
        0U,
    };

    const auto run = [&](std::uint32_t divisor, std::uint32_t action_state,
                         bool current_player = true) {
        Runtime runtime;
        RetimeHooks hooks{span};
        assert(runtime.loadBytes(site, std::as_bytes(std::span{window})));
        hooks.program(divisor);
        hooks.state().advance_this_step_ = true;
        runtime.setRetimeHooks(&hooks);
        hooks.setActive(true);
        runtime.reset(site, 0U, stack);
        runtime.setRegister(2, static_cast<std::uint32_t>(-9));
        runtime.setRegister(17, humanoid); // $s1 = DynamicThing
        runtime.setRegister(31, Runtime::return_sentinel);
        assert(runtime.write32(
            0x800DD6B4U, current_player ? humanoid : humanoid + 0x1000U));
        assert(runtime.write32(humanoid + 0x164U, action_state));
        assert(runtime.write32(
            humanoid + 0x68U, static_cast<std::uint32_t>(-18)));
        assert(runtime.write32(humanoid + 0x6CU, 123U));
        for (int executed = 0; executed < 32; ++executed) {
            if (runtime.atReturnSentinel()) {
                break;
            }
            const auto step = runtime.step();
            assert(step.reason == stuntmaster::psx::R3000StopReason::running);
        }
        assert(runtime.atReturnSentinel());
        runtime.settleLoadDelay();
        std::uint32_t velocity = 0U;
        assert(runtime.read32(humanoid + 0x68U, velocity));
        return std::array<std::int32_t, 2U>{
            static_cast<std::int32_t>(velocity),
            static_cast<std::int32_t>(runtime.state().gpr[2])};
    };

    // The gravity displacement has already produced a downward contact probe
    // by this store. Grounded running discards only its persistent velocity;
    // the delay-slot load for Z velocity must still arrive normally.
    assert((run(2U, AS_RUN) == std::array<std::int32_t, 2U>{0, 123}));
    // Airborne motion retains the computed Y velocity.
    assert((run(2U, AS_FALL) ==
            std::array<std::int32_t, 2U>{-9, 123}));
    // A divisor of one remains bit-for-bit retail even if the action is Run.
    assert((run(1U, AS_RUN) ==
            std::array<std::int32_t, 2U>{-9, 123}));
    // The shared DynamicThing hook must not interpret another object's +0x164
    // payload as a Player action state.
    assert((run(2U, AS_RUN, false) ==
            std::array<std::int32_t, 2U>{-9, 123}));
}

// `pole_swing_timeline` holds only the horizontal pole-swing's pendulum
// accumulation, `Stack`-style: the body's tail is an idempotent pose-apply
// that retail runs every step, so a whole-body hold rubberbands the model
// between its applied and unapplied poses (the reported flicker). A counted
// update models the displaced `lw $v1, 0x1a8($s1)` and resumes at the rejoin;
// a held update jumps straight to the pose-apply start `0x80033114` with the
// velocity register untouched, so the accumulation store between never runs
// and the position is rewritten from the held angle on every update.
void poleSwingTimelineGateHoldsOnlyTheAccumulation() {
    using Runtime = stuntmaster::psx::R3000Runtime;
    using stuntmaster::game::RetimeHook;
    using stuntmaster::game::RetimeHooks;

    constexpr std::uint32_t site = 0x80033078U;
    constexpr std::uint32_t apply_start = 0x80033114U;
    const auto clock_hooks = stuntmaster::game::retimeClockHooks();
    const auto found = std::find_if(
        clock_hooks.begin(), clock_hooks.end(),
        [](const RetimeHook& hook) { return hook.pc == site; });
    assert(found != clock_hooks.end());
    const std::array<RetimeHook, 1U> span{*found};

    constexpr std::uint32_t stack = 0x801F0000U;
    constexpr std::uint32_t player = 0x80121000U;
    constexpr std::uint32_t jr_ra = encodeR(31, 0, 0, 0, 0x08);
    // The site, its delay slot, the rejoin marker, then the skipped region
    // (which the held path never executes), then `jr $ra` at the apply start.
    std::array<std::uint32_t, 41U> window{};
    window[0] = 0U;                          // hooked lw $v1,0x1a8($s1)
    window[1] = encodeI(0x0F, 0, 2, 0x800E); // lui $v0,0x800e, delay slot
    window[2] = encodeI(0x09, 0, 8, 0x5555); // addiu $t0,$zero,0x5555, rejoin
    window[39] = jr_ra;                      // at 0x80033114, the apply start
    static_assert(site + 39U * 4U == apply_start);

    const auto run = [&](std::uint32_t divisor, bool held) {
        Runtime runtime;
        RetimeHooks hooks{span};
        assert(runtime.loadBytes(site, std::as_bytes(std::span{window})));
        assert(runtime.write32(player + 0x1A8U, 0x1234U));
        hooks.program(divisor);
        hooks.state().advance_this_step_ = !held;
        runtime.setRetimeHooks(&hooks);
        hooks.setActive(true);
        runtime.reset(site, 0U, stack);
        runtime.setRegister(3, 0xDEADBEEFU); // $v1 = the velocity register
        runtime.setRegister(17, player);     // $s1 = the swinging player
        runtime.setRegister(31, Runtime::return_sentinel);
        for (int executed = 0; executed < 64; ++executed) {
            if (runtime.atReturnSentinel()) {
                break;
            }
            const auto step = runtime.step();
            assert(step.reason ==
                   stuntmaster::psx::R3000StopReason::running);
        }
        assert(runtime.atReturnSentinel());
        runtime.settleLoadDelay();
        std::uint32_t velocity = 0U;
        assert(runtime.read32(player + 0x1A8U, velocity));
        return std::array<std::uint32_t, 3U>{
            runtime.state().gpr[3],      // $v1
            runtime.state().gpr[8],      // $t0, set only if the rejoin ran
            velocity,                    // +0x1A8, never written by the gate
        };
    };
    // Counted: the displaced load is modeled and the rejoin runs.
    assert((run(2U, false) == std::array<std::uint32_t, 3U>{0x1234U, 0x5555U, 0x1234U}));
    // Held: the accumulation is skipped; the pose-apply start is reached
    // directly with the velocity register untouched.
    assert((run(2U, true) == std::array<std::uint32_t, 3U>{0xDEADBEEFU, 0U, 0x1234U}));
    // Divisor one counts every update.
    assert((run(1U, false) == std::array<std::uint32_t, 3U>{0x1234U, 0x5555U, 0x1234U}));
}

void widescreenCullSettingTogglesAndNormalizesOldSaves() {
    using stuntmaster::game::WidescreenLowerBounds;
    using stuntmaster::game::applyRetailPatch;
    using stuntmaster::game::applyRetailPatches;
    using stuntmaster::game::applyRetailTrampoline;
    using stuntmaster::game::setWidescreenCull;
    using stuntmaster::game::widescreenBlockCull;
    using stuntmaster::game::widescreenModelCull;

    stuntmaster::psx::R3000Runtime runtime;
    const auto retail_blocks = widescreenBlockCull(0x200U);
    const auto retail_models = widescreenModelCull(0x200U);
    const WidescreenLowerBounds retail_lower{0x200U, 0x200U};
    for (const auto& patch : retail_blocks) {
        assert(runtime.write32(patch.address, patch.original_word));
    }
    for (const auto& patch : retail_models) {
        assert(runtime.write32(patch.address, patch.original_word));
    }
    for (const auto& patch : retail_lower.patches()) {
        assert(runtime.write32(patch.site, patch.original_word));
    }
    assert(runtime.write32(
        stuntmaster::game::legacyWidescreenBlockVisibility().address,
        stuntmaster::game::legacyWidescreenBlockVisibility().original_word));
    const auto retail_visibility_limit =
        stuntmaster::game::widescreenBlockVisibilityLimit(0x200U);
    assert(runtime.write32(
        retail_visibility_limit.address,
        retail_visibility_limit.original_word));

    // Simulate the pre-guard format: four widened immediate limits and the
    // original sixteen lower-bound trampolines, with every later site retail.
    const auto legacy_blocks = widescreenBlockCull(597U);
    const auto legacy_models = widescreenModelCull(597U);
    const WidescreenLowerBounds legacy_lower{597U, 597U};
    assert(applyRetailPatches(runtime, legacy_blocks));
    assert(applyRetailPatches(runtime, legacy_models));
    for (std::size_t index = 0U; index < 16U; ++index) {
        assert(applyRetailTrampoline(runtime, legacy_lower.patches()[index]));
    }

    // Candidate normalization removes that complete state, then applies the
    // aspect-exact 16:9 band to polygons, models, and whole blocks.
    assert(setWidescreenCull(runtime, 597U, 597U, true));
    std::uint32_t word{};
    assert(runtime.read32(retail_blocks[0].address, word));
    assert(word == widescreenBlockCull(597U)[0].patched_word);
    assert(runtime.read32(retail_models[0].address, word));
    assert(word == widescreenModelCull(597U)[0].patched_word);
    assert(runtime.read32(
        stuntmaster::game::legacyWidescreenBlockVisibility().address, word));
    assert(word ==
           stuntmaster::game::legacyWidescreenBlockVisibility().original_word);
    assert(runtime.read32(retail_visibility_limit.address, word));
    assert(word ==
           stuntmaster::game::widescreenBlockVisibilityLimit(597U).
               patched_word);
    const WidescreenLowerBounds current_lower{597U, 597U};
    for (std::size_t index = 0U; index < 16U; ++index) {
        assert(runtime.read32(current_lower.patches()[index].site, word));
        assert(word == (0x08000000U |
            ((current_lower.patches()[index].address >> 2U) &
             0x03FFFFFFU)));
    }
    for (std::size_t index = 16U; index < 24U; ++index) {
        assert(runtime.read32(current_lower.patches()[index].site, word));
        assert(word == current_lower.patches()[index].original_word);
    }
    const auto& block_lower = current_lower.patches().back();
    assert(runtime.read32(block_lower.site, word));
    assert(word == (0x08000000U |
        ((block_lower.address >> 2U) & 0x03FFFFFFU)));

    assert(setWidescreenCull(runtime, 597U, 597U, false));
    for (const auto& patch : retail_blocks) {
        assert(runtime.read32(patch.address, word));
        assert(word == patch.original_word);
    }
    for (const auto& patch : retail_models) {
        assert(runtime.read32(patch.address, word));
        assert(word == patch.original_word);
    }
    for (const auto& patch : retail_lower.patches()) {
        assert(runtime.read32(patch.site, word));
        assert(word == patch.original_word);
    }
    assert(runtime.read32(
        stuntmaster::game::legacyWidescreenBlockVisibility().address, word));
    assert(word ==
           stuntmaster::game::legacyWidescreenBlockVisibility().original_word);
    assert(runtime.read32(retail_visibility_limit.address, word));
    assert(word == retail_visibility_limit.original_word);

    // Normalize the most aggressive experimental format as well. It widened
    // the whole-block band to 1022, enabled all eight polygon guards, and
    // bypassed the renderer's three-block fast path. Current mode removes all
    // three experiments while retaining the safe widescreen bounds.
    const auto aggressive_blocks = widescreenBlockCull(1022U);
    const auto aggressive_models = widescreenModelCull(1022U);
    const auto aggressive_visibility_limit =
        stuntmaster::game::widescreenBlockVisibilityLimit(1022U);
    const WidescreenLowerBounds aggressive_lower{1022U, 1022U};
    assert(applyRetailPatch(
        runtime, stuntmaster::game::legacyWidescreenBlockVisibility()));
    assert(applyRetailPatch(runtime, aggressive_visibility_limit));
    assert(applyRetailPatches(runtime, aggressive_blocks));
    assert(applyRetailPatches(runtime, aggressive_models));
    for (const auto& trampoline : aggressive_lower.patches()) {
        assert(applyRetailTrampoline(runtime, trampoline));
    }
    assert(setWidescreenCull(runtime, 597U, 597U, true));
    assert(runtime.read32(
        stuntmaster::game::legacyWidescreenBlockVisibility().address, word));
    assert(word ==
           stuntmaster::game::legacyWidescreenBlockVisibility().original_word);
    for (std::size_t index = 16U; index < 24U; ++index) {
        assert(runtime.read32(current_lower.patches()[index].site, word));
        assert(word == current_lower.patches()[index].original_word);
    }
    assert(setWidescreenCull(runtime, 597U, 597U, false));

    // A mixed site set is neither retail nor one coherent widescreen patch.
    assert(runtime.write32(retail_blocks[1].address, 0xDEADBEEFU));
    assert(!setWidescreenCull(runtime, 597U, 597U, true));
    assert(runtime.read32(retail_blocks[0].address, word));
    assert(word == retail_blocks[0].original_word);
}

void retailHostMenuCallbackBridge() {
    using stuntmaster::game::HostMenuCommand;
    using stuntmaster::game::HostMenuEvent;
    using stuntmaster::game::RetailHleStatus;

    constexpr std::uint32_t push = 0x80010D08U;
    constexpr std::uint32_t callback = 0x80002FF0U;
    constexpr std::uint32_t menu = 0x80100000U;
    constexpr std::uint32_t frame_item = 0x80100100U;
    constexpr std::uint32_t size_item = 0x80100200U;
    constexpr std::uint32_t widescreen_item = 0x80101700U;
    constexpr std::uint32_t frame_label = 0x80100300U;
    constexpr std::uint32_t frame_value = 0x80100400U;
    constexpr std::uint32_t size_label = 0x80100500U;
    constexpr std::uint32_t size_value = 0x80100600U;
    constexpr std::uint32_t frame_label_text = 0x80100700U;
    constexpr std::uint32_t size_label_text = 0x80100800U;
    constexpr std::uint32_t manager = 0x80100900U;
    constexpr std::uint32_t sound_menu = 0x80100A00U;
    constexpr std::uint32_t sound_frame_item = 0x80100B00U;
    constexpr std::uint32_t music_item = 0x80100C00U;
    constexpr std::uint32_t sound_size_item = 0x80100D00U;
    constexpr std::uint32_t stereo_item = 0x80100E00U;
    constexpr std::uint32_t music_label = 0x80100F00U;
    constexpr std::uint32_t music_value = 0x80101000U;
    constexpr std::uint32_t stereo_label = 0x80101100U;
    constexpr std::uint32_t stereo_value = 0x80101200U;
    constexpr std::uint32_t music_label_text = 0x80101300U;
    constexpr std::uint32_t stereo_label_text = 0x80101400U;
    constexpr std::uint32_t sound_title_text = stereo_label_text + 7U;
    constexpr std::uint32_t stereo_off_text = 0x80101500U;
    constexpr std::uint32_t stereo_on_text = 0x80101600U;
    constexpr std::uint32_t return_address = 0x80050000U;

    stuntmaster::psx::R3000Runtime runtime;
    runtime.reset(push, 0U, 0x801FFFF0U);
    const std::array fingerprint{
        0x27BDFFE0U, 0xAFB20018U, 0x00809021U, 0xAFB10014U};
    for (std::size_t index = 0U; index < fingerprint.size(); ++index) {
        assert(runtime.write32(
            push + static_cast<std::uint32_t>(index * 4U),
            fingerprint[index]));
    }
    assert(runtime.write32(menu + 0x0CU, 0xB1AF7E45U));
    assert(runtime.write32(menu + 0x18U, frame_item));
    assert(runtime.write32(frame_item, widescreen_item));
    assert(runtime.write32(frame_item + 0x0CU, frame_label));
    assert(runtime.write32(frame_item + 0x18U, 0x1B5DD3F5U));
    assert(runtime.write32(frame_item + 0x20U, frame_value));
    assert(runtime.write32(widescreen_item, size_item));
    assert(runtime.write32(widescreen_item + 0x0CU, music_label));
    assert(runtime.write32(widescreen_item + 0x18U, 0xB3DA1CE9U));
    assert(runtime.write32(widescreen_item + 0x20U, music_value));
    assert(runtime.write32(size_item, 0U));
    assert(runtime.write32(size_item + 0x0CU, size_label));
    assert(runtime.write32(size_item + 0x18U, 0xB47983DEU));
    assert(runtime.write32(size_item + 0x20U, size_value));
    assert(runtime.write8(frame_label + 0x2DU, 0U));
    assert(runtime.write32(frame_label + 0x38U, frame_label_text));
    assert(runtime.write8(frame_value + 0x2DU, 0U));
    assert(runtime.write8(size_label + 0x2DU, 0U));
    assert(runtime.write32(size_label + 0x38U, size_label_text));
    assert(runtime.write8(size_value + 0x2DU, 0U));
    assert(runtime.write32(manager + 0x30U, sound_menu));
    assert(runtime.write32(sound_menu, 0U));
    assert(runtime.write32(sound_menu + 0x0CU, 0x061CD029U));
    assert(runtime.write32(sound_menu + 0x18U, sound_frame_item));
    assert(runtime.write32(sound_frame_item, music_item));
    assert(runtime.write32(sound_frame_item + 0x0CU, frame_label));
    assert(runtime.write32(sound_frame_item + 0x18U, 0x1B5DD3F5U));
    assert(runtime.write32(sound_frame_item + 0x20U, frame_value));
    assert(runtime.write32(music_item, sound_size_item));
    assert(runtime.write32(music_item + 0x0CU, music_label));
    assert(runtime.write32(music_item + 0x18U, 0xB3DA1CE9U));
    assert(runtime.write32(music_item + 0x20U, music_value));
    assert(runtime.write32(sound_size_item, stereo_item));
    assert(runtime.write32(sound_size_item + 0x0CU, size_label));
    assert(runtime.write32(sound_size_item + 0x18U, 0xB47983DEU));
    assert(runtime.write32(sound_size_item + 0x20U, size_value));
    assert(runtime.write32(stereo_item, 0U));
    assert(runtime.write32(stereo_item + 0x0CU, stereo_label));
    assert(runtime.write32(stereo_item + 0x18U, 0x3D030EFAU));
    assert(runtime.write32(stereo_item + 0x20U, stereo_value));
    assert(runtime.write8(music_label + 0x2DU, 0U));
    assert(runtime.write32(music_label + 0x38U, music_label_text));
    assert(runtime.write8(music_value + 0x2DU, 0U));
    assert(runtime.write32(music_value + 0x38U, music_item + 0x38U));
    assert(runtime.write8(stereo_label + 0x2DU, 0U));
    assert(runtime.write32(stereo_label + 0x38U, stereo_label_text));
    assert(runtime.write8(stereo_value + 0x2CU, 2U));
    assert(runtime.write8(stereo_value + 0x2DU, 0U));
    assert(runtime.write32(stereo_value + 0x38U, stereo_off_text));
    assert(runtime.write32(stereo_value + 0x3CU, stereo_on_text));
    static constexpr std::array text_style_offsets{
        0x04U, 0x08U, 0x10U, 0x14U, 0x1CU, 0x20U, 0x24U, 0x30U};
    for (const auto offset : text_style_offsets) {
        const auto volume_style = 0x11000000U + offset;
        const auto compact_style = 0x22000000U + offset;
        assert(runtime.write32(music_value + offset, volume_style));
        assert(runtime.write32(stereo_value + offset, compact_style));
        assert(runtime.write32(frame_value + offset, volume_style));
        assert(runtime.write32(size_value + offset, volume_style));
    }
    assert(runtime.write32(frame_value + 0x0CU, 0x11110000U));
    assert(runtime.write32(frame_value + 0x18U, 0x11110001U));
    assert(runtime.write32(size_value + 0x0CU, 0x22220000U));
    assert(runtime.write32(size_value + 0x18U, 0x22220001U));
    const auto seedText = [&](std::uint32_t address, std::string_view text) {
        for (std::size_t index = 0U; index < text.size(); ++index) {
            assert(runtime.write8(
                address + static_cast<std::uint32_t>(index),
                static_cast<std::uint8_t>(text[index])));
        }
        assert(runtime.write8(
            address + static_cast<std::uint32_t>(text.size()), 0U));
    };
    seedText(music_label_text, "Music");
    seedText(music_item + 0x38U, "5");
    seedText(stereo_label_text, "Stereo");
    seedText(sound_title_text, "SOUND OPTIONS");
    seedText(stereo_off_text, "OFF");
    seedText(stereo_on_text, "ON");
    runtime.setRegister(4U, manager);
    runtime.setRegister(5U, menu);

    std::vector<HostMenuEvent> events;
    stuntmaster::game::RetailHle hle;
    hle.setExperimentalHostMenuEnabled(true);
    hle.setHostMenuState(60U, {1280U, 720U}, true);
    assert((hle.hostMenuRenderSizeForWidescreen(false) ==
            stuntmaster::game::HostRenderSize{960U, 720U}));
    assert((hle.hostMenuRenderSizeForWidescreen(true) ==
            stuntmaster::game::HostRenderSize{1280U, 720U}));
    hle.setHostMenuSink(
        [&](const HostMenuEvent& event) { events.push_back(event); });
    const auto push_result = hle.dispatch(runtime);
    assert(push_result.status == RetailHleStatus::not_boundary);
    std::uint32_t word{};
    std::uint16_t half{};
    assert(runtime.read32(frame_item + 0x10U, word) && word == callback);
    assert(runtime.read16(frame_item + 0x30U, half) && half == 1U);
    assert(runtime.read32(frame_value + 0x38U, word) &&
           word == frame_item + 0x38U);
    assert(runtime.read32(size_item + 0x10U, word) && word == callback);
    assert(runtime.read16(size_item + 0x30U, half) && half == 1U);
    assert(runtime.read32(size_value + 0x38U, word) &&
           word == size_item + 0x38U);
    assert(runtime.read32(widescreen_item + 0x10U, word) && word == callback);
    assert(runtime.read16(widescreen_item + 0x30U, half) && half == 1U);
    assert(runtime.read32(music_value + 0x38U, word) &&
           word == widescreen_item + 0x38U);

    const auto readText = [&](std::uint32_t address) {
        std::string text;
        for (std::size_t index = 0U; index < 32U; ++index) {
            std::uint8_t character{};
            assert(runtime.read8(
                address + static_cast<std::uint32_t>(index), character));
            if (character == 0U) {
                break;
            }
            text.push_back(static_cast<char>(character));
        }
        return text;
    };
    assert(readText(frame_label_text) == "Frame Rate");
    assert(readText(frame_item + 0x38U) == "60 HZ");
    assert(readText(size_label_text) == "Resolution");
    assert(readText(size_item + 0x38U) == "1280x720");
    assert(readText(music_label_text) == "Widescreen");
    assert(readText(widescreen_item + 0x38U) == "ON");
    assert(readText(stereo_label_text).empty());
    assert(readText(stereo_off_text).empty());
    assert(readText(sound_title_text) == "DISPLAY MENU");
    for (const auto offset : text_style_offsets) {
        assert(runtime.read32(frame_value + offset, word) &&
               word == 0x22000000U + offset);
        assert(runtime.read32(size_value + offset, word) &&
               word == 0x22000000U + offset);
        assert(runtime.read32(music_value + offset, word) &&
               word == 0x22000000U + offset);
    }
    assert(runtime.read32(frame_value + 0x0CU, word) &&
           word == 0x11110000U);
    assert(runtime.read32(frame_value + 0x18U, word) &&
           word == 0x11110001U);
    assert(runtime.read32(size_value + 0x0CU, word) &&
           word == 0x22220000U);
    assert(runtime.read32(size_value + 0x18U, word) &&
           word == 0x22220001U);

    // The logical HostDSP ID has no screen in FE.1. Immediately around
    // MenuMgr::PushMenu, alias it to the stock Sound screen and then restore
    // the logical ID so later GOTO lookup still finds HostDSP.
    constexpr std::uint32_t alias_site = 0x80010E0CU;
    constexpr std::uint32_t restore_site = 0x80010E18U;
    assert(runtime.write32(alias_site, 0x02402021U));
    assert(runtime.write32(alias_site + 4U, 0x0C017E0CU));
    assert(runtime.write32(alias_site + 8U, 0x02202821U));
    assert(runtime.write32(restore_site, 0x8FBF001CU));
    assert(runtime.write32(restore_site + 4U, 0x8FB20018U));
    assert(runtime.write32(restore_site + 8U, 0x8FB10014U));
    assert(runtime.write32(restore_site + 12U, 0x8FB00010U));
    runtime.reset(alias_site, 0U, 0x801FFFF0U);
    runtime.setRegister(17U, menu);
    assert(hle.dispatch(runtime).status == RetailHleStatus::not_boundary);
    assert(runtime.read32(menu + 0x0CU, word) && word == 0x061CD029U);
    runtime.reset(restore_site, 0U, 0x801FFFF0U);
    runtime.setRegister(17U, menu);
    assert(hle.dispatch(runtime).status == RetailHleStatus::not_boundary);
    assert(runtime.read32(menu + 0x0CU, word) && word == 0xB1AF7E45U);

    // Both menus share overlay text objects. Pushing the retail Sound menu
    // must restore its labels and rebind each value object to that menu's
    // inline buffer before retail installs the menu normally.
    assert(runtime.write32(frame_value + 0x38U, 0U));
    assert(runtime.write32(size_value + 0x38U, 0U));
    runtime.reset(push, 0U, 0x801FFFF0U);
    runtime.setRegister(4U, manager);
    runtime.setRegister(5U, sound_menu);
    const auto sound_push_result = hle.dispatch(runtime);
    assert(sound_push_result.status == RetailHleStatus::not_boundary);
    assert(readText(frame_label_text) == "Sound Effects");
    assert(readText(size_label_text) == "Voice Over");
    assert(readText(music_label_text) == "Music");
    assert(readText(stereo_label_text) == "Stereo");
    assert(readText(stereo_off_text) == "OFF");
    assert(readText(stereo_on_text) == "ON");
    assert(readText(sound_title_text) == "SOUND OPTIONS");
    for (const auto offset : text_style_offsets) {
        assert(runtime.read32(frame_value + offset, word) &&
               word == 0x11000000U + offset);
        assert(runtime.read32(size_value + offset, word) &&
               word == 0x11000000U + offset);
        assert(runtime.read32(music_value + offset, word) &&
               word == 0x11000000U + offset);
    }
    assert(runtime.read32(frame_value + 0x0CU, word) &&
           word == 0x11110000U);
    assert(runtime.read32(frame_value + 0x18U, word) &&
           word == 0x11110001U);
    assert(runtime.read32(size_value + 0x0CU, word) &&
           word == 0x22220000U);
    assert(runtime.read32(size_value + 0x18U, word) &&
           word == 0x22220001U);
    assert(runtime.read32(frame_value + 0x38U, word) &&
           word == sound_frame_item + 0x38U);
    assert(runtime.read32(size_value + 0x38U, word) &&
           word == sound_size_item + 0x38U);
    assert(runtime.read32(music_value + 0x38U, word) &&
           word == music_item + 0x38U);

    runtime.reset(callback, 0U, 0x801FFFF0U);
    runtime.setRegister(31U, return_address);
    runtime.setRegister(4U, frame_item);
    assert(runtime.write16(frame_item + 0x30U, 0U));
    const auto rate_result = hle.dispatch(runtime);
    assert(rate_result.status == RetailHleStatus::handled);
    assert(runtime.state().pc == return_address);
    assert(events.size() == 1U);
    assert(events.back().command == HostMenuCommand::guest_update_rate);
    assert(events.back().value == 30U);
    assert(readText(frame_item + 0x38U) == "30 HZ");

    runtime.reset(callback, 0U, 0x801FFFF0U);
    runtime.setRegister(31U, return_address);
    runtime.setRegister(4U, widescreen_item);
    assert(runtime.write16(widescreen_item + 0x30U, 0U));
    const auto widescreen_result = hle.dispatch(runtime);
    assert(widescreen_result.status == RetailHleStatus::handled);
    assert(events.size() == 2U);
    assert(events.back().command == HostMenuCommand::widescreen_cull);
    assert(events.back().value == 0U);
    assert(events.back().width == 960U);
    assert(events.back().height == 720U);
    assert(readText(widescreen_item + 0x38U) == "OFF");
    assert(runtime.read16(size_item + 0x30U, half) && half == 1U);
    assert(readText(size_item + 0x38U) == "960x720");

    runtime.reset(callback, 0U, 0x801FFFF0U);
    runtime.setRegister(31U, return_address);
    runtime.setRegister(4U, size_item);
    assert(runtime.write16(size_item + 0x30U, 2U));
    const auto size_result = hle.dispatch(runtime);
    assert(size_result.status == RetailHleStatus::handled);
    assert(events.size() == 3U);
    assert(events.back().command == HostMenuCommand::render_size);
    assert(events.back().width == 1440U);
    assert(events.back().height == 1080U);
    assert(readText(size_item + 0x38U) == "1440x1080");

    runtime.reset(callback, 0U, 0x801FFFF0U);
    runtime.setRegister(31U, return_address);
    runtime.setRegister(4U, widescreen_item);
    assert(runtime.write16(widescreen_item + 0x30U, 1U));
    const auto wide_again_result = hle.dispatch(runtime);
    assert(wide_again_result.status == RetailHleStatus::handled);
    assert(events.size() == 4U);
    assert(events.back().command == HostMenuCommand::widescreen_cull);
    assert(events.back().value == 1U);
    assert(events.back().width == 1920U);
    assert(events.back().height == 1080U);
    assert(readText(widescreen_item + 0x38U) == "ON");
    assert(runtime.read16(size_item + 0x30U, half) && half == 2U);
    assert(readText(size_item + 0x38U) == "1920x1080");

    runtime.reset(callback, 0U, 0x801FFFF0U);
    runtime.setRegister(31U, return_address);
    runtime.setRegister(4U, size_item);
    assert(runtime.write16(size_item + 0x30U, 3U));
    const auto size_1440_result = hle.dispatch(runtime);
    assert(size_1440_result.status == RetailHleStatus::handled);
    assert(events.size() == 5U);
    assert(events.back().command == HostMenuCommand::render_size);
    assert(events.back().width == 2560U);
    assert(events.back().height == 1440U);
    assert(readText(size_item + 0x38U) == "2560x1440");
    assert((hle.hostMenuRenderSizeForWidescreen(false) ==
            stuntmaster::game::HostRenderSize{1920U, 1440U}));
}

void ladderStateHooksKeepAuthoredCadence() {
    using Runtime = stuntmaster::psx::R3000Runtime;
    using stuntmaster::game::RetimeHook;
    using stuntmaster::game::RetimeHooks;

    const auto hookByPc = [](std::uint32_t pc) -> RetimeHook {
        const auto hooks = stuntmaster::game::retimeLedgeHooks();
        const auto found = std::find_if(
            hooks.begin(), hooks.end(), [pc](const RetimeHook& hook) {
                return hook.pc == pc;
            });
        assert(found != hooks.end());
        return *found;
    };

    constexpr std::uint32_t climb_site = 0x80069F08U;
    constexpr std::uint32_t climb_epilogue = 0x80069FD4U;
    constexpr std::uint32_t inc_frame = 0x80071300U;
    constexpr std::uint32_t marker_address = 0x80130000U;
    constexpr std::uint32_t stack = 0x801F0000U;
    const auto runClimb = [&](bool held) {
        Runtime runtime;
        const std::array<RetimeHook, 1U> span{hookByPc(climb_site)};
        RetimeHooks hooks{span};
        const std::array<std::uint32_t, 4U> site_window{
            0U,
            0x02202021U, // move $a0, $s1 -- the real delay slot
            encodeR(8, 0, 0, 0, 0x08), // rejoin: jr $t0
            0U,
        };
        const std::array<std::uint32_t, 2U> held_return{
            encodeR(8, 0, 0, 0, 0x08), // epilogue stand-in: jr $t0
            0U,
        };
        const std::array<std::uint32_t, 5U> marker{
            encodeI(0x0F, 0, 3, 0x8013), // lui   $v1, 0x8013
            encodeI(0x09, 0, 2, 1),      // addiu $v0, $zero, 1
            encodeI(0x2B, 3, 2, 0),      // sw    $v0, 0($v1)
            encodeR(31, 0, 0, 0, 0x08),  // jr    $ra
            0U,
        };
        assert(runtime.loadBytes(
            climb_site, std::as_bytes(std::span{site_window})));
        assert(runtime.loadBytes(
            climb_epilogue, std::as_bytes(std::span{held_return})));
        assert(runtime.loadBytes(
            inc_frame, std::as_bytes(std::span{marker})));
        assert(runtime.write32(marker_address, 0U));
        hooks.program(2U);
        hooks.state().advance_this_step_ = !held;
        runtime.setRetimeHooks(&hooks);
        hooks.setActive(true);
        runtime.reset(climb_site, 0U, stack);
        runtime.setRegister(8, Runtime::return_sentinel); // $t0
        runtime.setRegister(17, 0x80123456U);             // $s1
        runtime.setRegister(31, Runtime::return_sentinel);
        for (int executed = 0; executed < 32; ++executed) {
            if (runtime.atReturnSentinel()) {
                break;
            }
            const auto step = runtime.step();
            assert(step.reason == stuntmaster::psx::R3000StopReason::running);
        }
        assert(runtime.atReturnSentinel());
        runtime.settleLoadDelay();
        std::uint32_t ran = 0U;
        assert(runtime.read32(marker_address, ran));
        assert(runtime.state().gpr[4] == 0x80123456U); // delay slot ran
        return ran;
    };
    assert(runClimb(false) == 1U);
    assert(runClimb(true) == 0U);

    constexpr std::uint32_t slide_site = 0x80069FC8U;
    constexpr std::uint32_t position = 0x80122000U;
    const auto runSlide = [&](std::uint32_t divisor) {
        Runtime runtime;
        const std::array<RetimeHook, 1U> span{hookByPc(slide_site)};
        RetimeHooks hooks{span};
        const std::array<std::uint32_t, 4U> window{
            0U,
            encodeI(0x2B, 18, 2, 4), // sw $v0, 4($s2) -- real delay slot
            encodeR(31, 0, 0, 0, 0x08),
            0U,
        };
        assert(runtime.loadBytes(
            slide_site, std::as_bytes(std::span{window})));
        hooks.program(divisor);
        hooks.state().advance_this_step_ = true;
        runtime.setRetimeHooks(&hooks);
        hooks.setActive(true);
        runtime.reset(slide_site, 0U, stack);
        runtime.setRegister(2, 1000U);       // $v0 = old position Y
        runtime.setRegister(18, position);   // $s2 = humanoid + 0x7c
        runtime.setRegister(31, Runtime::return_sentinel);
        while (!runtime.atReturnSentinel()) {
            const auto step = runtime.step();
            assert(step.reason == stuntmaster::psx::R3000StopReason::running);
        }
        runtime.settleLoadDelay();
        std::uint32_t y = 0U;
        assert(runtime.read32(position + 4U, y));
        assert(runtime.state().gpr[2] == y);
        return y;
    };
    assert(runSlide(1U) == 936U);
    assert(runSlide(2U) == 968U);
    assert(runSlide(4U) == 984U);
}

// The Platform divide-based conversion hooks (`Think` runs every update; these
// sub-step the tilt and fall and hold the frame counters/animation). There is
// no byte-trampoline oracle for this model, so these are arithmetic checks: each
// hook, run in isolation at its real site, must divide/hold/multiply the value
// it claims. The physics feel needs live validation. Every hook is identity at a
// divisor of one.
void platformConversionHooksSubStepTheRateSensitiveQuantities() {
    using Runtime = stuntmaster::psx::R3000Runtime;
    using stuntmaster::game::RetimeHook;
    using stuntmaster::game::RetimeHooks;
    using stuntmaster::game::divideHalfToEven;
    const std::uint32_t jr_ra = encodeR(31, 0, 0, 0, 0x08);
    const auto hookByPc = [](std::uint32_t pc) -> RetimeHook {
        for (const auto& overlay : stuntmaster::game::retimeOverlayHooks()) {
            if (overlay.hook.pc == pc) {
                return overlay.hook;
            }
        }
        assert(false);
        return {};
    };
    // Runs one hook at its real site with a `nop` delay slot and a `jr $ra` at
    // the rejoin, then hands the settled runtime to `check`.
    const auto runAtPhase = [&](std::uint32_t site, std::uint32_t divisor,
                                bool held, std::uint32_t phase,
                                auto setup, auto check) {
        Runtime runtime;
        const std::array<RetimeHook, 1U> span{hookByPc(site)};
        RetimeHooks hooks{span};
        // site, delay slot (nop), rejoin (jr $ra), nop.
        const std::array<std::uint32_t, 4U> window{0U, 0U, jr_ra, 0U};
        assert(runtime.loadBytes(site, std::as_bytes(std::span{window})));
        hooks.program(divisor);
        hooks.state().advance_this_step_ = !held;
        hooks.state().clock_accum = phase;
        runtime.setRetimeHooks(&hooks);
        hooks.setActive(true);
        runtime.reset(site, 0U, 0x801F0000U);
        setup(runtime);
        runtime.setRegister(31, Runtime::return_sentinel);
        for (int executed = 0; executed < 32; ++executed) {
            if (runtime.atReturnSentinel()) {
                break;
            }
            const auto result = runtime.step();
            assert(result.reason ==
                   stuntmaster::psx::R3000StopReason::running);
        }
        assert(runtime.atReturnSentinel());
        runtime.settleLoadDelay();
        check(runtime);
    };
    const auto run = [&](std::uint32_t site, std::uint32_t divisor, bool held,
                         auto setup, auto check) {
        runAtPhase(site, divisor, held, held ? 1U : divisor,
                   setup, check);
    };
    constexpr std::uint32_t platform = 0x80121000U;

    for (const std::uint32_t k : {1U, 2U, 3U, 4U}) {
        // Teeter mass x k (angular accel / k): $v1 and $a0 get mass * k.
        for (const auto& [site, reg] : std::initializer_list<
                 std::pair<std::uint32_t, std::size_t>>{
                 {0x80022DFCU, 3U}, {0x80022E88U, 4U}}) {
            run(site, k, false,
                [&](Runtime& r) {
                    r.setRegister(19, platform); // $s3
                    assert(r.write32(platform + 0x34U, 7U));
                },
                [&](Runtime& r) {
                    assert(r.state().gpr[reg] == 7U * k);
                });
        }
        // Teeter angular-velocity / k into $v0 (base $v0).
        for (const auto& [site, off] : std::initializer_list<
                 std::pair<std::uint32_t, std::uint32_t>>{
                 {0x80022F48U, 0x0CU}, {0x80022FA4U, 0x14U}}) {
            for (const std::int32_t angvel : {100, 101, -101, 4096}) {
                run(site, k, false,
                    [&](Runtime& r) {
                        r.setRegister(2, platform); // base $v0
                        assert(r.write32(
                            platform + off, static_cast<std::uint32_t>(angvel)));
                    },
                    [&](Runtime& r) {
                        assert(static_cast<std::int32_t>(r.state().gpr[2]) ==
                               divideHalfToEven(angvel, k));
                    });
            }
        }
        // Teeter damping constant 0x11EB / k into $v1.
        run(0x80022FFCU, k, false, [](Runtime&) {},
            [&](Runtime& r) {
                assert(static_cast<std::int32_t>(r.state().gpr[3]) ==
                       divideHalfToEven(0x11EB, k));
            });
    }

    // Detached fall: exercise every phase at every supported divisor shape.
    // The sub-steps must be smooth, but after k phases the forward-Euler frame
    // must land exactly on retail: position += frame-start velocity and
    // velocity -= 27 (gravity 18 * 3/2), for either velocity sign.
    for (const std::uint32_t k : {1U, 2U, 3U, 4U, 8U}) {
        for (const std::int32_t start_velocity : {-137, -38, 137}) {
            auto velocity = start_velocity;
            std::int32_t position = 0;
            for (std::uint32_t phase = 1U; phase <= k; ++phase) {
                const auto held = phase != k;
                std::int32_t position_step = 0;
                runAtPhase(0x80021BDCU, k, held, phase,
                    [&](Runtime& r) {
                        r.setRegister(16, platform);          // $s0
                        r.setRegister(5, 0x800E0000U);        // lui $a1
                        assert(r.write32(0x800DD6B4U, 0x80122000U));
                        assert(r.write32(0x80122000U + 0xC4U, 18U));
                        assert(r.write32(
                            platform + 0xA4U,
                            static_cast<std::uint32_t>(velocity)));
                    },
                    [&](Runtime& r) {
                        position_step = static_cast<std::int32_t>(
                            r.state().gpr[3]);
                    });

                std::int32_t acceleration_step = 0;
                runAtPhase(0x80021C08U, k, held, phase,
                    [&](Runtime& r) {
                        r.setRegister(16, platform); // $s0
                        r.setRegister(2, 27U);       // gravity * 3/2
                        assert(r.write32(
                            platform + 0xA4U,
                            static_cast<std::uint32_t>(velocity)));
                    },
                    [&](Runtime& r) {
                        assert(static_cast<std::int32_t>(r.state().gpr[3]) ==
                               velocity);
                        acceleration_step = static_cast<std::int32_t>(
                            r.state().gpr[2]);
                    });
                position += position_step;
                velocity -= acceleration_step;
            }
            assert(position == start_velocity);
            assert(velocity == start_velocity - 27);
        }
    }

    // The carried-velocity snapshot: 0xac/b0/b4 = 0xa0/a4/a8 * k, so the delta
    // the collision hands a jumping rider is the full per-frame velocity (it is
    // divided again on apply). Its rejoin is site+0xC (it writes all three and
    // skips the last store), so it needs its own window.
    for (const std::uint32_t k : {1U, 2U, 3U}) {
        Runtime runtime;
        const std::array<RetimeHook, 1U> span{hookByPc(0x80021D74U)};
        RetimeHooks hooks{span};
        // 0x80021d74 (site), 0x80021d78 (delay), 0x80021d7c (skipped),
        // 0x80021d80 (rejoin -> jr $ra).
        const std::array<std::uint32_t, 4U> window{0U, 0U, 0U, jr_ra};
        assert(runtime.loadBytes(0x80021D74U, std::as_bytes(std::span{window})));
        hooks.program(k);
        runtime.setRetimeHooks(&hooks);
        hooks.setActive(true);
        runtime.reset(0x80021D74U, 0U, 0x801F0000U);
        runtime.setRegister(16, platform); // $s0
        assert(runtime.write32(platform + 0xA0U, 30U));
        assert(runtime.write32(platform + 0xA4U, static_cast<std::uint32_t>(-40)));
        assert(runtime.write32(platform + 0xA8U, 50U));
        runtime.setRegister(31, Runtime::return_sentinel);
        for (int executed = 0; executed < 32; ++executed) {
            if (runtime.atReturnSentinel()) {
                break;
            }
            const auto result = runtime.step();
            assert(result.reason == stuntmaster::psx::R3000StopReason::running);
        }
        assert(runtime.atReturnSentinel());
        runtime.settleLoadDelay();
        std::uint32_t ac = 0U;
        std::uint32_t b0 = 0U;
        std::uint32_t b4 = 0U;
        assert(runtime.read32(platform + 0xACU, ac));
        assert(runtime.read32(platform + 0xB0U, b0));
        assert(runtime.read32(platform + 0xB4U, b4));
        assert(static_cast<std::int32_t>(ac) == 30 * static_cast<std::int32_t>(k));
        assert(static_cast<std::int32_t>(b0) ==
               -40 * static_cast<std::int32_t>(k));
        assert(static_cast<std::int32_t>(b4) == 50 * static_cast<std::int32_t>(k));
    }

    // Frame-counter holds: a skipped update leaves the counter, a counted one
    // decrements it.
    for (const bool held : {false, true}) {
        for (const std::uint32_t site : {0x80021A94U, 0x80021D38U}) {
            run(site, 2U, held,
                [&](Runtime& r) {
                    r.setRegister(16, platform); // $s0
                    assert(r.write32(platform + 0x94U, 5U));
                },
                [&](Runtime& r) {
                    std::uint32_t counter = 0U;
                    assert(r.read32(platform + 0x94U, counter));
                    assert(counter == (held ? 5U : 4U));
                });
        }
        // The 0x98 move-delay counter: held restores $v0 to the live counter
        // (so retail's store writes it back unchanged), counted keeps the
        // decremented $v0. $v1 always gets the displaced 0x7c load.
        run(0x80021AA8U, 2U, held,
            [&](Runtime& r) {
                r.setRegister(16, platform);          // $s0
                r.setRegister(2, 4U);                 // $v0 = counter - 1
                assert(r.write32(platform + 0x7CU, 0xABCDU)); // restart flag
                assert(r.write32(platform + 0x98U, 5U));      // live counter
            },
            [&](Runtime& r) {
                assert(r.state().gpr[3] == 0xABCDU);       // displaced $v1
                assert(r.state().gpr[2] == (held ? 5U : 4U));
            });
    }

    // Bob is a call-gate: it runs on a counted update and is skipped on a held
    // one, so its bob-phase timer stays on the authored schedule.
    for (const bool held : {false, true}) {
        Runtime runtime;
        const std::array<RetimeHook, 1U> span{hookByPc(0x80021DA8U)};
        RetimeHooks hooks{span};
        const std::array<std::uint32_t, 4U> window{0U, 0U, jr_ra, 0U};
        assert(runtime.loadBytes(0x80021DA8U, std::as_bytes(std::span{window})));
        // A marker callee at Bob's address.
        const std::array<std::uint32_t, 5U> callee{
            encodeI(0x0F, 0, 3, 0x8012), // lui   $v1, 0x8012
            encodeI(0x09, 0, 2, 1),      // addiu $v0, $zero, 1
            encodeI(0x2B, 3, 2, 0),      // sw    $v0, 0($v1)
            encodeR(31, 0, 0, 0, 0x08),  // jr    $ra
            0U,
        };
        assert(runtime.loadBytes(0x80023190U, std::as_bytes(std::span{callee})));
        hooks.program(2U);
        hooks.state().advance_this_step_ = !held;
        runtime.setRetimeHooks(&hooks);
        hooks.setActive(true);
        assert(runtime.write32(0x80120000U, 0U));
        runtime.reset(0x80021DA8U, 0U, 0x801F0000U);
        runtime.setRegister(31, Runtime::return_sentinel);
        // On a counted update the gate sets $ra to the rejoin and runs the
        // callee, which returns into the test's `jr $ra` and loops on $ra; the
        // marker (set once by the callee) is the observable, not a clean return.
        for (int executed = 0; executed < 32; ++executed) {
            if (runtime.atReturnSentinel()) {
                break;
            }
            const auto result = runtime.step();
            assert(result.reason == stuntmaster::psx::R3000StopReason::running);
        }
        runtime.settleLoadDelay();
        std::uint32_t ran = 0U;
        assert(runtime.read32(0x80120000U, ran));
        assert(ran == (held ? 0U : 1U));
    }

    // Platform::Move must not overwrite an already bobbed Y with the Path's
    // base Y during a held update. It also replaces the saved Path Y so the
    // following retail delta calculation sees zero vertical movement. A
    // counted update and a non-bobbing Platform retain the retail Path Y.
    for (const auto& [held, has_bob, expected_y] :
         std::initializer_list<std::tuple<bool, bool, std::uint32_t>>{
             {true, true, 0xFFFFFE7DU},
             {false, true, 0xFFFFFD9EU},
             {true, false, 0xFFFFFD9EU}}) {
        Runtime runtime;
        const std::array<RetimeHook, 1U> span{hookByPc(0x80022814U)};
        RetimeHooks hooks{span};
        const std::array<std::uint32_t, 4U> window{0U, 0U, jr_ra, 0U};
        assert(runtime.loadBytes(0x80022814U, std::as_bytes(std::span{window})));
        hooks.program(2U);
        hooks.state().advance_this_step_ = !held;
        runtime.setRetimeHooks(&hooks);
        hooks.setActive(true);
        constexpr std::uint32_t stack = 0x801F0000U;
        constexpr std::uint32_t bob = 0x80122000U;
        runtime.reset(0x80022814U, 0U, stack);
        runtime.setRegister(17, platform); // $s1
        runtime.setRegister(8, 0xFFFFFD9EU); // $t0, real delay-slot Path Y
        runtime.setRegister(31, Runtime::return_sentinel);
        assert(runtime.write32(stack + 0x14U, 0xFFFFFE7DU)); // old bobbed Y
        assert(runtime.write32(stack + 0x20U, 1234U));       // Path X
        assert(runtime.write32(stack + 0x24U, 0xFFFFFD9EU)); // Path Y
        assert(runtime.write32(platform + 0x130U, has_bob ? bob : 0U));
        for (int executed = 0; executed < 32; ++executed) {
            if (runtime.atReturnSentinel()) {
                break;
            }
            const auto result = runtime.step();
            assert(result.reason == stuntmaster::psx::R3000StopReason::running);
        }
        assert(runtime.atReturnSentinel());
        runtime.settleLoadDelay();
        std::uint32_t saved_path_y = 0U;
        assert(runtime.read32(stack + 0x24U, saved_path_y));
        assert(saved_path_y == expected_y);
        assert(runtime.state().gpr[7] == 1234U); // displaced Path X load
        assert(runtime.state().gpr[8] == expected_y);
    }
}

void freeCameraUsesRetailPoseAndRestoresItsOwner() {
    using stuntmaster::game::FreeCameraController;
    using stuntmaster::game::FreeCameraInput;
    using stuntmaster::game::FreeCameraResult;
    using stuntmaster::game::free_camera_fast;
    using stuntmaster::game::free_camera_forward;
    using stuntmaster::psx::R3000Runtime;

    constexpr std::uint32_t camera = 0x80110000U;
    constexpr std::uint32_t follow_path = 0x80048AC0U;
    R3000Runtime runtime;
    assert(runtime.write32(0x800DD734U, camera));
    assert(runtime.write32(camera + 8U, 0x800CCCB8U));
    assert(runtime.write32(camera + 0x1D4U, 0U));
    assert(runtime.write16(camera + 0x170U, 4U));
    assert(runtime.write16(camera + 0x172U, 0xFFFFU));
    assert(runtime.write32(camera + 0x174U, follow_path));
    assert(runtime.write32(camera + 0x1D8U, 3U));
    assert(runtime.write32(camera + 0x12CU, 8'000U));
    assert(runtime.write32(camera + 0x130U, 10'000U));
    assert(runtime.write32(camera + 0x134U, 8'000U));
    assert(runtime.write32(camera + 0x1CU, 100U));
    assert(runtime.write32(camera + 0x20U, 200U));
    assert(runtime.write32(camera + 0x24U, 300U));
    assert(runtime.write32(camera + 0x17CU, 0U));
    assert(runtime.write32(camera + 0x180U, 0U));
    assert(runtime.write32(camera + 0x184U, 0U));

    FreeCameraController controller;
    assert(controller.toggle(runtime, true) == FreeCameraResult::enabled);
    assert(controller.active());
    std::uint16_t half = 0U;
    std::uint32_t word = 0U;
    assert(runtime.read16(camera + 0x172U, half) && half == 0U);
    assert(runtime.read32(camera + 0x1D8U, word) && word == 0U);

    FreeCameraInput input{
        static_cast<std::uint8_t>(free_camera_forward), 0, 0};
    assert(controller.update(runtime, input, 60U, true) ==
           FreeCameraResult::unchanged);
    assert(runtime.read32(camera + 0x24U, word) && word == 250U);
    assert(runtime.read32(camera + 0x84U, word) && word == 250U);
    assert(runtime.read32(camera + 0xD4U, word) && word == 250U);

    input = {
        static_cast<std::uint8_t>(free_camera_forward | free_camera_fast),
        2,
        -3};
    assert(controller.update(runtime, input, 60U, true) ==
           FreeCameraResult::unchanged);
    assert(runtime.read32(camera + 0x180U, word) && word == 40U);
    assert(runtime.read32(camera + 0x17CU, word) && word == 60U);

    // Half-stick movement retains half speed instead of being normalized to
    // the digital full-speed path.
    std::uint32_t before_z = 0U;
    assert(runtime.read32(camera + 0x24U, before_z));
    input = {};
    input.controller_forward = 16'384;
    assert(controller.update(runtime, input, 60U, true) ==
           FreeCameraResult::unchanged);
    assert(runtime.read32(camera + 0x24U, word));
    assert(static_cast<std::int32_t>(before_z) -
               static_cast<std::int32_t>(word) ==
           25);

    // Full right-stick look is 180 degrees/second and therefore advances by
    // the same angular rate independently of the guest VBlank schedule.
    input = {};
    input.controller_look_x = 32'767;
    input.controller_look_y = 16'384;
    assert(controller.update(runtime, input, 60U, true) ==
           FreeCameraResult::unchanged);
    assert(runtime.read32(camera + 0x180U, word) && word == 586U);
    assert(runtime.read32(camera + 0x17CU, word) &&
           word == static_cast<std::uint32_t>(-213));

    // A save copy is normalized without disabling the live controller.
    auto saved = runtime;
    assert(controller.normalizeSavedRuntime(saved));
    assert(saved.read16(camera + 0x172U, half) && half == 0xFFFFU);
    assert(saved.read32(camera + 0x174U, word) && word == follow_path);
    assert(runtime.read16(camera + 0x172U, half) && half == 0U);

    assert(controller.toggle(runtime, true) == FreeCameraResult::disabled);
    assert(!controller.active());
    assert(runtime.read16(camera + 0x170U, half) && half == 4U);
    assert(runtime.read16(camera + 0x172U, half) && half == 0xFFFFU);
    assert(runtime.read32(camera + 0x174U, word) && word == follow_path);
    assert(runtime.read32(camera + 0x1D8U, word) && word == 3U);
    assert(runtime.read32(camera + 0x130U, word) && word == 10'000U);

    // If retail selects a new mode while freecam is active, that newer state
    // wins; the stale entry snapshot must not be restored over it.
    assert(controller.toggle(runtime, true) == FreeCameraResult::enabled);
    assert(runtime.write16(camera + 0x172U, 0xFFFFU));
    assert(runtime.write32(camera + 0x174U, 0x8004897CU));
    assert(controller.update(runtime, {}, 60U, true) ==
           FreeCameraResult::unavailable);
    assert(!controller.active());
    assert(runtime.read32(camera + 0x174U, word) && word == 0x8004897CU);

    // Camera animation bypasses OrderHandler, so the prior handler is restored
    // for the frame on which the animation eventually ends.
    assert(runtime.write16(camera + 0x172U, 0xFFFFU));
    assert(runtime.write32(camera + 0x174U, follow_path));
    assert(controller.toggle(runtime, true) == FreeCameraResult::enabled);
    assert(runtime.write32(camera + 0x1D4U, 0x80120000U));
    assert(controller.update(runtime, {}, 60U, true) ==
           FreeCameraResult::unavailable);
    assert(runtime.read16(camera + 0x172U, half) && half == 0xFFFFU);
    assert(runtime.read32(camera + 0x174U, word) && word == follow_path);
}

void photoModeGatesSimulationButNotGuestExecution() {
    using stuntmaster::game::RetimeHooks;
    using stuntmaster::game::photoModeHooks;
    using stuntmaster::psx::R3000Runtime;

    struct Site {
        std::uint32_t pc;
        std::uint32_t rejoin;
        std::uint32_t callee;
        std::uint32_t delay_slot;
        bool camera_owned_gate;
    };
    constexpr std::array sites{
        Site{0x8002B33CU, 0x8002B344U, 0x80057D18U, 0x00C03821U, false},
        Site{0x8003F664U, 0x8003F66CU, 0x8003F67CU, 0U, true},
        Site{0x80044938U, 0x80044940U, 0x80044A40U, 0U, false},
        Site{0x8004CC44U, 0x8004CC4CU, 0x8004CF84U, 0U, false},
        Site{0x8005412CU, 0x80054134U, 0x80055D10U, 0U, false},
    };
    const auto hooks_span = photoModeHooks();
    assert(hooks_span.size() == sites.size());

    for (std::size_t index = 0U; index < sites.size(); ++index) {
        const auto& site = sites[index];
        assert(hooks_span[index].pc == site.pc);
        assert(hooks_span[index].rejoin == site.rejoin);

        // Frozen path: the retail delay slot still executes, but the jal is
        // skipped and $ra remains the handler's caller return address.
        R3000Runtime frozen;
        assert(frozen.write32(site.pc + 4U, site.delay_slot));
        auto frozen_state = frozen.state();
        frozen_state.pc = site.pc;
        frozen_state.next_pc = site.pc + 4U;
        frozen_state.gpr[6] = 0x12345678U;
        frozen_state.gpr[31] = 0x8000FFF0U;
        frozen.restoreCpuState(frozen_state);
        RetimeHooks frozen_hooks{hooks_span};
        if (site.camera_owned_gate) {
            frozen_hooks.setPhotoCameraActive(true);
        } else {
            frozen_hooks.setPhotoSimulationFrozen(true);
        }
        frozen.setRetimeHooks(&frozen_hooks);
        assert(frozen.step().reason ==
               stuntmaster::psx::R3000StopReason::running);
        assert(frozen.step().reason ==
               stuntmaster::psx::R3000StopReason::running);
        assert(frozen.state().pc == site.rejoin);
        assert(frozen.state().gpr[31] == 0x8000FFF0U);
        if (site.delay_slot != 0U) {
            assert(frozen.state().gpr[7] == 0x12345678U);
        }

        // Opposite-owner path: camera ownership must not freeze simulation,
        // and a simulation freeze must not hide the HUD. The virtual call is
        // indistinguishable from the retail jal in either case.
        R3000Runtime running;
        assert(running.write32(site.pc + 4U, site.delay_slot));
        auto running_state = running.state();
        running_state.pc = site.pc;
        running_state.next_pc = site.pc + 4U;
        running_state.gpr[31] = 0x8000FFF0U;
        running.restoreCpuState(running_state);
        RetimeHooks running_hooks{hooks_span};
        if (site.camera_owned_gate) {
            running_hooks.setPhotoSimulationFrozen(true);
        } else {
            running_hooks.setPhotoCameraActive(true);
        }
        running.setRetimeHooks(&running_hooks);
        assert(running.step().reason ==
               stuntmaster::psx::R3000StopReason::running);
        assert(running.step().reason ==
               stuntmaster::psx::R3000StopReason::running);
        assert(running.state().pc == site.callee);
        assert(running.state().gpr[31] == site.rejoin);
    }
}

} // namespace

void licenseOverlayWrapsFoldsAndPaginates() {
    using stuntmaster::presentation::LicenseDocument;
    using stuntmaster::presentation::LicenseOverlay;
    using stuntmaster::presentation::wrapLicenseText;

    // Word wrap folds to uppercase, collapses spaces, and never exceeds width.
    const auto wrapped = wrapLicenseText(
        "The quick brown fox\njumps over  the lazy dog.", 10U);
    for (const auto& line : wrapped) {
        assert(line.size() <= 10U);
        for (const auto character : line) {
            assert(character < 'a' || character > 'z');
        }
    }
    // A single newline is a hard line break (two non-empty rows, no gap); an
    // empty source line (blank line) is preserved as a blank row.
    const auto paragraphs = wrapLicenseText("ALPHA\n\nBETA", 10U);
    assert(paragraphs.size() == 3U);
    assert(paragraphs[0] == "ALPHA");
    assert(paragraphs[1].empty());
    assert(paragraphs[2] == "BETA");

    // A word longer than the column width is hard-split, not dropped.
    const auto split = wrapLicenseText("SUPERCALIFRAGILISTIC", 5U);
    assert(split.size() == 4U);
    for (const auto& line : split) {
        assert(line.size() <= 5U);
    }
    assert(split.front() == "SUPER");

    // Zero columns yields nothing rather than looping forever.
    assert(wrapLicenseText("anything", 0U).empty());

    // The viewer: a header row plus exactly (rows-1) body rows, scrolling
    // clamped at both ends, and document cycling that resets the scroll.
    std::string body;
    for (int line = 0; line < 100; ++line) {
        body += "LINE " + std::to_string(line) + "\n";
    }
    LicenseOverlay overlay{std::vector<LicenseDocument>{
        {"First", body}, {"Second", "SHORT BODY"}}};
    overlay.setViewport(40U, 10U);
    assert(overlay.documentCount() == 2U);
    assert(overlay.visibleRows().size() == 10U);
    // Header carries the document counter and the visible line range.
    assert(overlay.visibleRows().front().find("DOC 1/2") != std::string::npos);
    // Every row is padded to a constant width so the rendered panel does not
    // resize as different-length lines scroll through it.
    for (const auto& row : overlay.visibleRows()) {
        assert(row.size() == 40U);
    }
    overlay.scrollPages(1);
    for (const auto& row : overlay.visibleRows()) {
        assert(row.size() == 40U);
    }
    overlay.home();

    assert(overlay.topLine() == 0U);
    overlay.scrollLines(-5);
    assert(overlay.topLine() == 0U); // clamped at the top
    overlay.scrollPages(1000);
    const auto max_top = overlay.topLine();
    assert(max_top > 0U);
    assert(max_top + (10U - 1U) >= overlay.lineCount()); // clamped at the end
    overlay.scrollLines(10000);
    assert(overlay.topLine() == max_top); // still clamped

    overlay.nextDocument();
    assert(overlay.documentIndex() == 1U);
    assert(overlay.topLine() == 0U); // scroll resets across documents
    overlay.previousDocument();
    assert(overlay.documentIndex() == 0U);
}

int main() {
    mouseControlDualHeadingIsBoundedAndReversible();
    retailHlePublishesMouseExtensionWithoutChangingDigitalPadBytes();
    // A taken branch must execute its delay slot before the target; the
    // retime hooks rely on this ordering (a hook site that is another
    // branch's delay slot would lose the branch target, so such sites are
    // avoided, and this pins the interpreter's behaviour).
    {
        using Runtime = stuntmaster::psx::R3000Runtime;
        Runtime runtime;
        const std::array<std::uint32_t, 7U> code{
            encodeI(0x09, 0, 2, 1),     // addiu $v0, $zero, 1
            encodeI(0x05, 2, 0, 2),     // bnez $v0, +2
            encodeI(0x09, 0, 3, 0x111), // delay slot: $v1 = 0x111
            encodeI(0x09, 0, 3, 0x222), // fallthrough: dead
            encodeI(0x09, 3, 3, 1),     // target: $v1 += 1
            encodeR(31, 0, 0, 0, 0x08), // jr $ra
            0U,
        };
        assert(runtime.loadBytes(0x80010000U, std::as_bytes(std::span{code})));
        runtime.reset(0x80010000U, 0U, 0x801F0000U);
        runtime.setRegister(31, Runtime::return_sentinel);
        for (int i = 0; i < 16 && !runtime.atReturnSentinel(); ++i) {
            const auto s = runtime.step();
            assert(s.reason == stuntmaster::psx::R3000StopReason::running);
        }
        assert(runtime.atReturnSentinel());
        runtime.settleLoadDelay();
        assert(runtime.state().gpr[3] == 0x112U); // delay slot ran first
    }
    licenseOverlayWrapsFoldsAndPaginates();
    stoppedFlipPublicationsAreVBlankBoundedAndSelfContained();
    guestScheduleDerivesEveryRateFromRetail();
    highFrequencyCadenceUsesRetailGameStateNotGpuTraffic();
    everyTrampolineFitsThePatchArenaTogether();
    guestCpuScaleTracksTheUpdateRate();
    ratePacerCarriesEveryRemainder();
    cdCompletionKeepsOneDriveSpeedAtEveryVBlankRate();
    audioRingDiscardsAnEndedTimeline();
    debugOverlayReportsHostAndGuestState();
    displayScalingPreservesAspectAndCenters();
    oversizedGpuPolygonsMatchHardwareLimits();
    executableParsing();
    bootPathParsing();
    widescreenDarkOverlaysReachWindowEdges();
    invalidExecutableRejected();
    sha256AndGameIdentity();
    r3000Execution();
    r3000BatchStopsOnlyAtMachineBoundaries();
    r3000RecompilerMatchesInterpreterAndInvalidatesCodeWrites();
    gteNormalColorSingle();
    biosCompatibilityDataIsReadOnly();
    gpuMmioSeparatesCommandsFromStatus();
    gpuCommandDecoderPacketsAndVram();
    gpuCommandDecoderUploadSinkPayload();
    machineSubsystemStateArchivesRoundTrip();
    spuRegistersAndSoundRam();
    spuAdpcmDecodeAndMix();
    spuRepeatAddressRegisterMovesTheLoopPoint();
    spuReverbRoutesThroughTheSoundRamWorkArea();
    biosHleSetjmpAndTty();
    memoryCardPersistsGuestAuthoredFiles();
    retailVSyncHle();
    retailCdInitHle();
    retailMovieSkipIsCallerGated();
    retailDrawSyncHle();
    retailWaitForLayerPollFastForward();
    retailVSyncCallbackHle();
    retailHostMenuCallbackBridge();
    boundedLatestMailboxDropsOnlyObsoleteValues();
    memoryWriteSinkObservesGuestStores();
    interruptCallsDoNotBorrowTheInterruptedStack();
    retailSwapGatePatchIsFingerprintedAndReversible();
    retailSwapGatePatchReleasesASwapEveryVBlank();
    widescreenBlockCullWidensOnlyTheHorizontalBounds();
    widescreenModelCullWidensTheHorizontalBounds();
    widescreenLowerBoundsMoveTheLeftEdgeInsteadOfRemovingIt();
    widescreenCullSettingTogglesAndNormalizesOldSaves();
    ledgeTraceReportsWhichConditionRejectedTheLedge();
    ledgeTraceDoesNotChangeWhatLedgeCheckDecides();
    obstacleCollisionGateServicesContactsThatCannotWait();
    conveyorCarryRunsOnlyOnAuthoredUpdates();
    runningDynamicPassengerDoesNotAccumulateGravity();
    poleSwingTimelineGateHoldsOnlyTheAccumulation();
    ladderStateHooksKeepAuthoredCadence();
    retimeOverlayHooksActivatePerFingerprint();
    butchStompEventCounterUsesTheAuthoredClock();
    authoredCounterHooksKeepTheirCadence();
    platformConversionHooksSubStepTheRateSensitiveQuantities();
    freeCameraUsesRetailPoseAndRestoresItsOwner();
    photoModeGatesSimulationButNotGuestExecution();
    std::cout << "stuntmaster_core_tests: passed\n";
}
