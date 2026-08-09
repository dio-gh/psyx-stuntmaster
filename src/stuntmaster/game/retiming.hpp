#pragma once

#include "stuntmaster/psx/r3000_runtime.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

// Host-owned retime hooks, the replacement for the in-RAM byte trampolines
// described in `docs/RECOMP_MIGRATION.md`.
//
// Every retiming site that used to jump into a MIPS body in the patch arena is
// instead a `RetimeHook` keyed on its guest PC. The interpreter consults the
// table before dispatching an instruction; a recompiler will consult the same
// table at translation time. Guest RAM stays byte-clean: no host write ever
// lands in executable guest memory, so the self-modifying-code invalidations a
// recompiler needs never exist for the retiming itself.
//
// The arithmetic bodies are ported to C++ literally, with the MIPS rounding
// preserved; the whole-register-file oracle tests are the gate that keeps a
// port bit-identical to the retail instruction stream it replaces.

namespace stuntmaster::game {

// Host-owned retiming state, replacing the four patch-arena words at
// `0x80003380`..`0x8000338C`. Nothing is read from or written to guest RAM:
// `divisor == 1` reproduces retail everywhere, which is the old "unprogrammed
// arena" rule made a host default.
struct RetimeState {
    // Guest updates per authored 30 Hz step. One means no retiming.
    std::uint32_t divisor{};
    // The master-clock accumulator (`retime_clock_accumulator_address`).
    std::uint32_t clock_accum{};
    // The private fullscreen-fade accumulator
    // (`retime_fade_accumulator_address`).
    std::uint32_t fade_accum{};
    // The master decision for the current guest update: false means the
    // authored timelines advance, true means they hold. Every hook reads this
    // instead of deciding for itself.
    bool advance_this_step_{true};

    // Called once per guest update by the master hook (the `Step__4Time`
    // decision). Mirrors the MIPS body exactly: the accumulator restarts at
    // zero when programmed, so the first update after a mode change holds for
    // any divisor above one and the advance lands on the update where the
    // accumulator reaches the divisor.
    void decide() noexcept {
        const auto k = divisor == 0U ? 1U : divisor;
        advance_this_step_ = (++clock_accum % k) == 0U;
    }
    [[nodiscard]] bool hold() const noexcept { return !advance_this_step_; }

    void setDivisor(std::uint32_t k) noexcept {
        divisor = k == 0U ? 1U : k;
    }
    // `program` semantics: a mode change begins on a whole
    // authored step with a counted first decision.
    void program(std::uint32_t k) noexcept {
        setDivisor(k);
        clock_accum = 0U;
        fade_accum = 0U;
        advance_this_step_ = true;
    }
};

// What a hook does to the guest, for tooling and future recompiler planning.
// `recompute` rewrites live registers and resumes at the rejoin; `gate` skips
// a timed sub-section on a held update; `semantic` stands in for a whole guest
// call/section with host logic.
enum class RetimeHookKind {
    gate,
    recompute,
    semantic,
};

// A host hook standing in for the retail instruction at `pc`. The interpreter
// executes the retail instruction at `pc + 4` (the site's delay slot, which
// runs before the trampoline body on both paths) and then invokes `fn`, which
// returns the PC to resume at — normally `rejoin`.
//
// `fn` may read/write live guest registers (`R3000State`), the shared
// `RetimeState` (the master hook calls `decide()`, every other hook reads
// `hold()`), and guest memory through the runtime (to model a displaced load
// or store). It must not re-enter the interpreter.
using RetimeHookFn = std::uint32_t (*)(
    psx::R3000State&,
    RetimeState&,
    psx::R3000Runtime&,
    std::uint32_t rejoin,
    const void* context) noexcept;

// Per-site data for a shared `fn`, passed as the hook's `context`. A held
// prologue (an obstacle `Think`, a world-effect `Update`, ...) returns to the
// caller on a held update after restoring the callee-saved registers the
// prologue has already stored and unwinding its frame; on a counted update it
// models `displaced` (the site's own instruction) and resumes at the rejoin.
struct RetimeHeldPrologue {
    std::uint32_t displaced{};
    std::uint32_t frame_size{};
    // `lw $sX, imm($sp)` words, one per callee-saved register the prologue has
    // already stored; the second may be zero where one restore is enough.
    std::array<std::uint32_t, 2U> restores{};
};

// A `jal <callee>` gate: on a held update skip the call and resume at the
// rejoin; on a counted update set `$ra` to the rejoin and start the callee.
struct RetimeCallGate {
    std::uint32_t callee;
};

struct RetimeHook {
    std::string_view name;
    std::uint32_t pc;      // the guest site
    std::uint32_t rejoin;  // resume PC for the run/count path
    RetimeHookKind kind{};
    RetimeHookFn fn{};
    // Passed through to `fn`; per-site data for the shared prologue/gate fns,
    // null for hooks that carry their own constants in their fn.
    const void* context{};
};

// An overlay-resident hook plus the fingerprint that decides whether its
// overlay is the one currently loaded at `hook.pc`. Overlay files reuse load
// addresses across levels, so an overlay hook must fire only while its overlay
// is loaded — the same constraint the byte overlay trampolines honored via their
// fingerprint window. `window` is the expected run of consecutive retail words
// starting at `window_address` (covering the site), read out of the extracted
// overlay; it is unique across all four `*_REL.BIN` files. The app selects the
// live subset per frame with `buildActiveRetimeHooks`; guest RAM stays
// byte-clean, so the check is a read, never a write.
struct RetimeOverlayHook {
    RetimeHook hook;
    std::uint32_t window_address{};
    std::span<const std::uint32_t> window{};
};

// A sorted lookup table of retime hooks plus the `RetimeState` they share.
// `setActive(true)` is the whole "install": a host boolean, fingerprint-gated
// at image load, never a guest memory write.
class RetimeHooks {
public:
    RetimeHooks() = default;
    explicit RetimeHooks(std::span<const RetimeHook> hooks) noexcept {
        setHooks(hooks);
    }

    // The table must stay sorted by `pc`; `find` normally uses the compact
    // open-addressed index built here and retains the sorted search as a
    // fallback if the hook set ever outgrows it. The assertion is the guard
    // against the unsorted-table footgun.
    void setHooks(std::span<const RetimeHook> hooks) noexcept {
        assert([&] {
            for (std::size_t index = 1U; index < hooks.size(); ++index) {
                if (hooks[index - 1U].pc >= hooks[index].pc) {
                    return false;
                }
            }
            return true;
        }());
        hooks_ = hooks;
        index_.fill(nullptr);
        pc_bitmap_.fill(0U);
        indexed_ = hooks.size() <= index_.size() / 2U;
        if (!indexed_) {
            return;
        }
        for (const auto& hook : hooks) {
            if (const auto word = ramWord(hook.pc)) {
                pc_bitmap_[*word / 64U] |=
                    std::uint64_t{1U} << (*word & 63U);
            }
            auto slot = hash(hook.pc);
            while (index_[slot] != nullptr) {
                slot = (slot + 1U) & (index_.size() - 1U);
            }
            index_[slot] = &hook;
        }
    }

    [[nodiscard]] const RetimeHook* find(std::uint32_t pc) const noexcept {
        if (hooks_.empty()) {
            return nullptr;
        }
        if (const auto word = ramWord(pc)) {
            if ((pc_bitmap_[*word / 64U] &
                 (std::uint64_t{1U} << (*word & 63U))) == 0U) {
                return nullptr;
            }
        } else if (pc < hooks_.front().pc || pc > hooks_.back().pc) {
            return nullptr;
        }
        if (indexed_) {
            auto slot = hash(pc);
            while (const auto* hook = index_[slot]) {
                if (hook->pc == pc) {
                    return hook;
                }
                slot = (slot + 1U) & (index_.size() - 1U);
            }
            return nullptr;
        }
        std::size_t low = 0U;
        std::size_t high = hooks_.size();
        while (low < high) {
            const auto middle = low + (high - low) / 2U;
            if (hooks_[middle].pc < pc) {
                low = middle + 1U;
            } else {
                high = middle;
            }
        }
        return low < hooks_.size() && hooks_[low].pc == pc
            ? &hooks_[low]
            : nullptr;
    }

    void setActive(bool active) noexcept { active_ = active; }
    [[nodiscard]] bool active() const noexcept { return active_; }

    [[nodiscard]] RetimeState& state() noexcept { return state_; }
    [[nodiscard]] const RetimeState& state() const noexcept { return state_; }
    void program(std::uint32_t divisor) noexcept { state_.program(divisor); }

private:
    static constexpr std::size_t index_size = 256U;
    static_assert((index_size & (index_size - 1U)) == 0U);
    static constexpr std::size_t ram_word_count =
        (2U * 1024U * 1024U) / sizeof(std::uint32_t);

    [[nodiscard]] static constexpr std::optional<std::size_t> ramWord(
        std::uint32_t pc) noexcept {
        if ((pc & 3U) != 0U) {
            return std::nullopt;
        }
        auto physical = pc;
        if (pc >= 0x80000000U && pc < 0xc0000000U) {
            physical &= 0x1fffffffU;
        } else if (pc >= 0x20000000U) {
            return std::nullopt;
        }
        if (physical >= 0x00800000U) {
            return std::nullopt;
        }
        return static_cast<std::size_t>(
            (physical & (2U * 1024U * 1024U - 1U)) /
            sizeof(std::uint32_t));
    }

    [[nodiscard]] static constexpr std::size_t hash(
        std::uint32_t pc) noexcept {
        return static_cast<std::size_t>(
            ((pc >> 2U) * 2654435761U) & (index_size - 1U));
    }

    std::span<const RetimeHook> hooks_;
    std::array<const RetimeHook*, index_size> index_{};
    std::array<std::uint64_t, ram_word_count / 64U> pc_bitmap_{};
    RetimeState state_{};
    bool indexed_{};
    bool active_{};
};

// Half-to-even divide: the literal port of the MIPS `div` bodies in the
// retiming trampolines (gravity and the position step). The MIPS bodies
// compute `q = floor(x / k)` and then round the quotient half-to-even, so the
// rounding error alternates in sign and cancels instead of accumulating
// downward; at a divisor of two this is bit-identical to the shift-and-carry
// halving it replaced. A divisor of zero is treated as one, as every MIPS body
// treated an unprogrammed arena.
[[nodiscard]] inline std::int32_t divideHalfToEven(    std::int32_t value, std::uint32_t divisor) noexcept {
    const auto k = divisor == 0U ? 1U : divisor;
    // 64-bit so the divide cannot overflow for any divisor the state can hold.
    auto quotient = static_cast<std::int64_t>(value) / k;
    auto remainder = static_cast<std::int64_t>(value) % k;
    if (remainder < 0) {
        --quotient;      // truncation -> floor
        remainder += k;  // remainder -> [0, k)
    }
    const auto twice_remainder = 2 * remainder;
    if (twice_remainder == static_cast<std::int64_t>(k)) {
        quotient += quotient & 1;  // exact half: round to even
    } else if (twice_remainder > static_cast<std::int64_t>(k)) {
        ++quotient;                // round away from zero
    }
    return static_cast<std::int32_t>(quotient);
}

// Mirrors the interpreter's register write for a host hook, including clearing
// a pending load delay on the same register so the pipeline cannot clobber the
// host-written value on the following `advanceLoadDelay`.
[[nodiscard]] inline void hostWriteRegister(
    psx::R3000State& state, std::uint32_t reg, std::uint32_t value) noexcept {
    if (reg == 0U || reg >= state.gpr.size()) {
        return;
    }
    state.gpr[reg] = value;
    if (state.load_delay.valid && state.load_delay.reg == reg) {
        state.load_delay.valid = false;
    }
    if (state.next_load_delay.valid && state.next_load_delay.reg == reg) {
        state.next_load_delay.valid = false;
    }
}

// The recompute hooks for `--retime-motion`: gravity, the shared position
// step, both `FaceAngleY` turn-limit branches, and the obstacle swept-overlap
// gate. These are the first ports from the MIPS bodies, used as the diff gate
// for the migration. Add gate/semantic hooks as separate spans as they land.
[[nodiscard]] std::span<const RetimeHook> retimeMotionHooks() noexcept;

// The `--retime-clock` hooks that decide and consume the master hold decision.
// `retimedGameClock` is the single decider: it calls `RetimeState::decide()`
// once per guest update, and every other hook in this span reads the resulting
// `hold()` rather than deciding for itself. The two call-gates (`animLoop`,
// VRAM flipbooks) skip the gated call on a held update and start it on a
// counted one; the fullscreen fade keeps its own private accumulator because
// its render loops run outside `Step__4Time`.
[[nodiscard]] std::span<const RetimeHook> retimeClockHooks() noexcept;

// The always-live boot held-prologue gates: the two world-effect `Update`s,
// `Ladder`, and `Untouchable`. The OL1/NBOL obstacle `Think` prologues and
// `Pushable` reuse load addresses across levels, so they are gated per loaded
// overlay in `retimeOverlayHooks()` instead.
[[nodiscard]] std::span<const RetimeHook> retimeObjectHooks() noexcept;

// The boot-executable `--retime-clock` readers beyond the obstacle `Think`
// holds. They preserve the ledge-latch velocity reset and Ladder's explicit
// climb/slide timeline. The obstacle collision/carry/ticket pass is gated to
// the authored rate by `obstacle_collision_pass` in `retimeClockHooks()`, with
// narrow held-update exceptions for active pushers, jumping humanoids, and
// the four ladder states whose contact bit cannot wait. The gate replaced the
// per-site carry band-aids
// (`retimedPassengerHold`, `retimedObstacleDisembark`). Boot addresses, always
// live; verified against `AI/PLAYER.c`, `AI/HUMANOID.c`, `AI/LADDER.c`,
// `AI/PUSHABLE.c`, and `Obstacle::HandleHumanoidObstacleCollision` in
// `AI/OBSTACLE.c`.
[[nodiscard]] std::span<const RetimeHook> retimeLedgeHooks() noexcept;

// The overlay-resident hooks, each carrying the fingerprint that decides whether
// its overlay is loaded: the recompute/gate hooks (the tutorial arrow's bob
// counter `retimedArrowBob`, `Platform::Move`'s telescoping path-speed divide
// `retimedPlatformMoveSpeed`, and the two `Stack` timeline/sound gates), every
// OL1/NBOL obstacle `Think` held prologue and `Pushable`, and the eleven
// `Platform` divide-based conversion hooks (the `Teeter` tilt divides, the
// detached-gravity fall's exact phase partitions, and the frame-counter/`Bob`
// holds) that let
// `Think` run every update instead of the old reshape trampoline. See
// `docs/RECOMP_MIGRATION.md`.
[[nodiscard]] std::span<const RetimeOverlayHook> retimeOverlayHooks() noexcept;

// True when `overlay`'s fingerprint window currently matches guest RAM, i.e.
// its overlay is the one loaded at `overlay.hook.pc`. A read-only check.
[[nodiscard]] bool retimeOverlayHookMatches(
    psx::R3000Runtime& runtime, const RetimeOverlayHook& overlay) noexcept;

// Assembles the active hook table for the current frame: every `boot` hook
// (always live) followed by the `overlay` hooks whose fingerprint currently
// matches, written into `out` sorted by `pc` (so `RetimeHooks::find`'s binary
// search stays valid). Returns the number written; `out` must hold at least
// `boot.size() + overlay.size()` entries.
[[nodiscard]] std::size_t buildActiveRetimeHooks(
    psx::R3000Runtime& runtime,
    std::span<const RetimeHook> boot,
    std::span<const RetimeOverlayHook> overlay,
    std::span<RetimeHook> out) noexcept;

} // namespace stuntmaster::game
