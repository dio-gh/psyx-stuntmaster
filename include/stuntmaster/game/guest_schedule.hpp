#pragma once

#include <cstdint>

namespace stuntmaster::game {

// Retail's authored game-loop rate. Every animation timeline, timeout, and
// per-frame integration step in the executable is written against it.
inline constexpr std::uint32_t retail_update_rate = 30U;

// The console's own display refresh, and the rate retail's VBlank-driven task
// lists, PAD sampling, and render-swap gate were written against.
inline constexpr std::uint32_t console_vblank_rate = 60U;

// NTSC R3000A instructions per second at the host's bootstrap one-per-cycle
// model. Dividing it by the emulated VBlank rate keeps the guest CPU running
// at console speed no matter how often the display refreshes.
inline constexpr std::uint32_t console_cpu_rate = 33'868'800U;

// The SPU's fixed output rate. Mixing follows the guest's VBlank boundary, so
// the frames produced per VBlank change with the VBlank rate while the samples
// produced per wall-clock second do not.
inline constexpr std::uint32_t guest_audio_rate = 44'100U;

// Eight retail updates per authored one. Beyond this the per-update integer
// motion step rounds to zero for ordinary walking speeds, which stalls slow
// movement rather than smoothing it.
inline constexpr std::uint32_t maximum_guest_update_rate = 240U;

// How the host advances the guest for one selected game-loop rate.
//
// Retail's loop rate is the display rate divided by the render-swap gate, so
// a rate above the console's 60 Hz refresh needs a faster emulated VBlank, not
// only a shorter gate. Everything else in this struct follows from that:
// the CPU keeps console speed by executing fewer instructions per VBlank, and
// the authored 30 Hz timelines keep wall-clock duration by advancing once every
// `retime_divisor` updates.
struct GuestSchedule {
    std::uint32_t update_rate{};
    std::uint32_t vblank_rate{};
    // The immediate in `VSCallback__Fe`'s swap gate: how many VBlanks must
    // pass before a queued buffer swap completes.
    std::uint32_t swap_gate_vblanks{};
    // Guest updates per authored 30 Hz step. One means no retiming is needed.
    std::uint32_t retime_divisor{};
    std::uint32_t instructions_per_vblank{};
};

// Retail changes `Game::pStateHandler` before returning from one game step,
// so the host can select the cadence for the *next* step without guessing
// from presentation traffic. These handlers are steady frame loops: none of
// them runs the level/petal streaming pipeline whose VBlank producer requires
// retail's two-to-one cadence. In particular, the pause-menu handler belongs
// here; it legitimately uploads UI imagery and must not disable high-rate
// gameplay.
//
// Every transition/load handler is deliberately absent. Unknown handlers are
// treated as load/transition work by the application, which is the safe side
// of the retail invariant. Addresses are from the fingerprinted NTSC-U boot
// executable's `stateTable` at 0x800D5324.
[[nodiscard]] constexpr bool retailStateHandlerAllowsHighFrequency(
    std::uint32_t handler) noexcept {
    switch (handler) {
    case 0x8002BE0CU: // gsTitleLoopState
    case 0x80029C6CU: // gsPlayState
    case 0x80029EF8U: // gsMenuState (pause)
    case 0x8002A064U: // gsErrorLoopState
    case 0x8002A128U: // gsLocationMenuState
    case 0x8002A174U: // gsDbgMenuState
    case 0x8002B6B0U: // gsEndLevelLoopState
    case 0x8002C22CU: // gsEndGameLoopState
        return true;
    default:
        return false;
    }
}

// A rate is supported when it is a whole number of authored 30 Hz steps, is
// not slower than retail, and leaves the emulated VBlank an exact divisor of
// the CPU rate. Every multiple of 30 through the maximum satisfies the last
// condition; the check keeps that true if the maximum ever moves.
[[nodiscard]] constexpr bool isSupportedGuestUpdateRate(
    std::uint32_t update_rate) noexcept {
    if (update_rate < retail_update_rate ||
        update_rate > maximum_guest_update_rate ||
        update_rate % retail_update_rate != 0U) {
        return false;
    }
    const auto vblank_rate =
        update_rate < console_vblank_rate ? console_vblank_rate : update_rate;
    return console_cpu_rate % vblank_rate == 0U;
}

// The cadence retail was written for: a 60 Hz display with a two-VBlank swap
// gate, so the game loop settles at 30 Hz and nothing needs retiming.
[[nodiscard]] constexpr GuestSchedule retailGuestSchedule() noexcept {
    return {
        retail_update_rate,
        console_vblank_rate,
        console_vblank_rate / retail_update_rate,
        1U,
        console_cpu_rate / console_vblank_rate};
}

// The cadence for a requested game-loop rate.
//
// At or below the console refresh the display stays at 60 Hz and the gate does
// the dividing. Above it the gate is already as short as it can be, so the
// display itself runs faster and the gate stays at one VBlank.
[[nodiscard]] constexpr GuestSchedule guestScheduleFor(
    std::uint32_t update_rate) noexcept {
    if (update_rate <= retail_update_rate) {
        return retailGuestSchedule();
    }
    const auto vblank_rate =
        update_rate < console_vblank_rate ? console_vblank_rate : update_rate;
    return {
        update_rate,
        vblank_rate,
        vblank_rate / update_rate,
        update_rate / retail_update_rate,
        console_cpu_rate / vblank_rate};
}

// What one retail game step costs under the host's bootstrap
// one-instruction-per-cycle model, measured on the title-to-first-level route
// by comparing `theTimeMgr` against wall clock at several update rates. The
// loop sustained about 107 updates a second at console CPU speed.
//
// This is a measurement of one route, not a bound. A heavier scene costs more.
inline constexpr std::uint32_t measured_game_step_instructions = 316'000U;

// Two different ceilings constrain a high update rate, and they are easy to
// confuse:
//
//   1. The guest CPU budget. The console executes `console_cpu_rate`
//      instructions a second, and a game step costs about 316,000 of them, so
//      the loop sustains roughly 107 updates a second. Past that the guest
//      runs in slow motion. `--guest-cpu-scale` raises this ceiling.
//   2. The host's own throughput. The guest is interpreted, not run, so the
//      host must deliver `console_cpu_rate * cpu_scale` guest instructions per
//      wall-clock second. Measured at 37.3M against the console's 33.9M, this
//      host has about 1.10x headroom. `--guest-cpu-scale` *lowers* this
//      ceiling, in direct proportion.
//
// Raising the scale to clear the first ceiling therefore walks into the
// second, and the symptom is the same either way: audio and gameplay slow
// together, because both follow the guest's VBlank boundary. Until the guest
// backend is faster than an interpreter, scale one is the only usable setting
// and `sustainableGuestUpdateRate(1)` is the real ceiling for live play.

// The highest supported update rate the guest CPU can actually keep up with at
// a given `--guest-cpu-scale`.
//
// A high update rate is not free: running the game loop `k` times per authored
// frame is `k` times the CPU work per wall-clock second, and the console's own
// CPU only has headroom for about three and a half retail frames a second more
// than it uses. Past this rate the loop cannot finish a step per emulated
// refresh, `WaitForLayer` absorbs the difference, and every authored timeline
// runs slow — the guest stays internally consistent and deterministic, but the
// game plays in slow motion rather than smoothly.
[[nodiscard]] constexpr std::uint32_t sustainableGuestUpdateRate(
    std::uint32_t cpu_scale) noexcept {
    const auto budget = static_cast<std::uint64_t>(console_cpu_rate) *
        (cpu_scale == 0U ? 1U : cpu_scale);
    const auto sustained = budget / measured_game_step_instructions;
    const auto rate = static_cast<std::uint32_t>(
                          sustained / retail_update_rate) *
        retail_update_rate;
    if (rate < retail_update_rate) {
        return retail_update_rate;
    }
    return rate > maximum_guest_update_rate ? maximum_guest_update_rate : rate;
}

// The smallest `--guest-cpu-scale` whose budget covers `update_rate`.
//
// Raising the scale is not the free fix it looks like. It does not make the
// host faster; it raises how many guest instructions the host must interpret
// per wall-clock second, in direct proportion. The host measured 37.3M guest
// instructions a second against the console's 33.9M — about 1.10x, enough to
// run a PlayStation in real time and very little else. At scale two the guest
// therefore runs at roughly half real time, and audio and gameplay slow
// together because both follow the guest's VBlank boundary.
//
// So this is the budget answer, not the advice. Until the guest backend is
// faster than an interpreter, `sustainableGuestUpdateRate(1)` is the real
// ceiling for live play.
[[nodiscard]] constexpr std::uint32_t guestCpuScaleForUpdateRate(
    std::uint32_t update_rate) noexcept {
    const auto needed = static_cast<std::uint64_t>(update_rate) *
        measured_game_step_instructions;
    const auto scale = (needed + console_cpu_rate - 1U) / console_cpu_rate;
    return scale < 1U ? 1U : static_cast<std::uint32_t>(scale);
}

// Spreads `numerator` units evenly over `denominator` steps without losing the
// remainder. Used wherever a per-second quantity has to be split across a
// VBlank rate that does not divide it — 44100 audio frames across 120 VBlanks,
// or a drive's sector rate across any of them.
class RatePacer final {
public:
    struct State {
        std::uint64_t numerator{};
        std::uint64_t denominator{1U};
        std::uint64_t remainder{};
    };
    constexpr RatePacer() = default;
    constexpr RatePacer(
        std::uint64_t numerator, std::uint64_t denominator) noexcept
        : numerator_{numerator},
          denominator_{denominator == 0U ? 1U : denominator} {}

    // Units owed for one step. The dropped fraction is carried, so any run of
    // `denominator` steps delivers exactly `numerator` units.
    [[nodiscard]] constexpr std::uint64_t take() noexcept {
        remainder_ += numerator_;
        const auto units = remainder_ / denominator_;
        remainder_ %= denominator_;
        return units;
    }

    // The largest value `take` can return, for sizing a fixed buffer.
    [[nodiscard]] constexpr std::uint64_t maximumStep() const noexcept {
        return (numerator_ + denominator_ - 1U) / denominator_;
    }

    [[nodiscard]] constexpr std::uint64_t numerator() const noexcept {
        return numerator_;
    }

    [[nodiscard]] constexpr std::uint64_t denominator() const noexcept {
        return denominator_;
    }
    [[nodiscard]] constexpr State state() const noexcept {
        return {numerator_, denominator_, remainder_};
    }
    [[nodiscard]] constexpr bool restoreState(State state) noexcept {
        if (state.denominator == 0U || state.remainder >= state.denominator) {
            return false;
        }
        numerator_ = state.numerator;
        denominator_ = state.denominator;
        remainder_ = state.remainder;
        return true;
    }

    // Changing the rate drops the carried fraction rather than reinterpreting
    // it against a different denominator. At the rates involved that is under
    // one audio frame or a quarter of a sector, once, at a mode change.
    constexpr void reset(
        std::uint64_t numerator, std::uint64_t denominator) noexcept {
        numerator_ = numerator;
        denominator_ = denominator == 0U ? 1U : denominator;
        remainder_ = 0U;
    }

private:
    std::uint64_t numerator_{};
    std::uint64_t denominator_{1U};
    std::uint64_t remainder_{};
};

// Audio frames owed per VBlank at a given emulated refresh rate.
[[nodiscard]] constexpr RatePacer audioFramePacer(
    std::uint32_t vblank_rate) noexcept {
    return {guest_audio_rate, vblank_rate};
}

} // namespace stuntmaster::game
