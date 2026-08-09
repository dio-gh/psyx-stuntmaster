#pragma once

#include "stuntmaster/game/guest_schedule.hpp"
#include "stuntmaster/psx/r3000_runtime.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace stuntmaster::game {

// A single-word, fully reversible modification of already-loaded retail code.
//
// Every patch carries the exact word it expects to replace. Applying a patch
// whose fingerprint does not match refuses instead of writing, so an
// unexpected image, an overlay reusing the address, or a later change to the
// load path can never silently corrupt guest code.
struct RetailPatch {
    std::string_view name;
    std::uint32_t address;
    std::uint32_t original_word;
    std::uint32_t patched_word;
};

// `VSCallback__Fe` (0x800A0140) refuses to complete a queued buffer swap
// until at least two VBlanks have elapsed since the previous swap:
//
//   0x800A0158  subu  $v0, $v0, $v1     ; rFrameCount60 - lastSwapVBlank
//   0x800A015C  sltiu $v0, $v0, 2       ; <- the 30 Hz lock
//   0x800A0160  bnez  $v0, 0x800A0274   ; not yet: leave the swap pending
//
// The render queue only frees a layer once a swap completes, so
// `WaitForLayer__11RenderQueueUl` stalls the whole game loop and the retail
// simulation settles at exactly 30 Hz. Lowering the bound to one VBlank
// releases a swap every VBlank, so the loop runs at the emulated display rate.
//
// That is also why a game rate above the console's 60 Hz refresh needs a
// faster emulated VBlank rather than a shorter gate: the gate is already at
// its minimum of one. See `GuestSchedule` in `guest_schedule.hpp`.
//
// This changes the guest's update cadence, not its per-update time step:
// retail advances animation, physics, and `Time::Step` once per game frame,
// so the unlocked guest also advances wall-clock gameplay proportionally
// faster. It is only correct alongside the host retiming hooks and a
// programmed `RetimeState::divisor`.
[[nodiscard]] const RetailPatch& thirtyHertzSwapGate() noexcept;

// Reads the word currently at `patch.address` without modifying it.
[[nodiscard]] std::optional<std::uint32_t> readPatchSite(
    const psx::R3000Runtime& runtime, const RetailPatch& patch) noexcept;

// Writes `patch.patched_word` only when the site still holds
// `patch.original_word`. Returns false and writes nothing otherwise.
[[nodiscard]] bool applyRetailPatch(
    psx::R3000Runtime& runtime, const RetailPatch& patch) noexcept;

// Writes `patch.original_word` only when the site still holds
// `patch.patched_word`. Returns false and writes nothing otherwise.
[[nodiscard]] bool revertRetailPatch(
    psx::R3000Runtime& runtime, const RetailPatch& patch) noexcept;

// Retail culls level geometry against its own 4:3 screen, so at a wider
// window the geometry that belongs at the left and right edges is never
// submitted. It is culled, not absent: panning the camera brings it in.
//
// `RP_ZCullGClip__FP9tGeometry` (`0x800A0E14`), reached from
// `Draw__5BlockRC10tagLVector`, carries its own Cohen-Sutherland outcode with
// the screen bounds baked into immediates rather than read from the clip box:
//
//   800A0E48  lui  $t8, 0x100       ; t8 = 0x01000200, y limit 256, x limit 512
//   800A0E54  ori  $t8, $t8, 0x200
//   800A0E68  lui  $t5, 0x4000      ; t5 = 0x40004000, bit 14 means coord < 0
//   ...
//   800A0FD4  subu $a1, $t8, $v1    ; limit - coord
//   800A0FD8  and  $a1, $a1, $t7    ; sign bit: coord > limit
//   800A0FDC  and  $v1, $v1, $t5    ; bit 14:   coord < 0
//   800A0FE0  or   $v1, $v1, $a1
//   800A0FE4  and  $v0, $v1, $v0    ; AND across vertices: cull if all outside
//
// Because the bounds are immediates, widening `_5tPort.CB` does nothing here.
// That is why an earlier attempt at `P3DClipCode__FUlUl` barely helped: it
// governs object visibility for models, and measurably widened the submitted
// range, but level blocks come through this routine instead.
//
// `x_limit` raises the right bound to the widescreen edge. The matching left
// bound cannot be expressed as an immediate — see `WidescreenLowerBounds` —
// and must be applied alongside these.
//
// A 4:3 window computes `x_limit` back to 512, making this a no-op there.
//
// All sites must change together, so callers apply them all-or-nothing.
[[nodiscard]] std::array<RetailPatch, 2> widescreenBlockCull(
    std::uint16_t x_limit) noexcept;

// Descriptor for an earlier experimental patch that bypassed the frame
// renderer's three-block fast path. It admitted unrelated resident geometry,
// so current widescreen mode never applies it; save normalization only
// recognizes and removes it.
[[nodiscard]] const RetailPatch& legacyWidescreenBlockVisibility() noexcept;

// Horizontal upper bound used by the earlier per-block bounding-box score.
[[nodiscard]] RetailPatch widescreenBlockVisibilityLimit(
    std::uint16_t x_limit) noexcept;

// Props and characters are culled separately from level blocks, through
// `P3DClipCode__FUlUl` (`0x8009E218`) via `P3DClipCodeSphere__FP7tSphere`,
// whose callers are `Show__6GModel`, `Show__6SModel`, `Show__6EModel`,
// `Display__6tETree`, and `PointInView__9ComEffect`. Unlike the block cull it
// does read the clip box:
//
//   8009E248  and  $v0, $a0, $a2    ; sign bits: x < 0, y < 0
//   8009E260  lh   $v1, 0x60($v1)   ; maxX from the clip box
//   8009E26C  subu $a0, $t0, $a0    ; max - coord
//   8009E270  and  $a0, $a0, $a2    ; sign bits: x > maxX, y > maxY
//
// Widened the same way as the block cull: raise the right bound to the
// widescreen edge. Near and far Z culling and both Y bounds are untouched, and
// the matching left bound comes from `WidescreenLowerBounds`.
//
// Widening the clip box itself would be less invasive than replacing the load,
// but `_5tPort.CB` is live retail state that the game rewrites, so the patch
// has to live in the code.
[[nodiscard]] std::array<RetailPatch, 2> widescreenModelCull(
    std::uint16_t x_limit) noexcept;

// The widescreen cull limit for a render target, in retail screen units.
// Retail projects into a 512-wide screen whose pixels are 4:3; a wider target
// shows `512 * aspect * 3/4` of it, centred. The returned limit is the exact
// right edge of that viewport; the matching left edge is `512 - limit`.
[[nodiscard]] std::uint16_t widescreenXLimit(
    std::uint32_t window_width, std::uint32_t window_height) noexcept;

// Exact horizontal edge used by the early whole-block test. This remains a
// separate value so save normalization can reconcile experimental states in
// which polygon and whole-block limits differed.
[[nodiscard]] std::uint16_t widescreenBlockVisibilityXLimit(
    std::uint32_t window_width, std::uint32_t window_height) noexcept;

// Reconciles the four polygon/model limits, sixteen matching lower-bound
// trampolines, and the whole-block upper/lower bounds as one reversible
// setting. Save normalization also recognizes and removes earlier experimental
// NCLIP, final-outcode, and active-list patches. Mixed or unrecognized sites
// are refused without writing.
[[nodiscard]] bool setWidescreenCull(
    psx::R3000Runtime& runtime,
    std::uint16_t x_limit,
    std::uint16_t block_visibility_x_limit,
    bool enabled) noexcept;

// Verifies every site before writing any of them, so a partial application
// cannot leave retail code half-patched.
[[nodiscard]] bool applyRetailPatches(
    psx::R3000Runtime& runtime,
    std::span<const RetailPatch> patches) noexcept;

[[nodiscard]] bool revertRetailPatches(
    psx::R3000Runtime& runtime,
    std::span<const RetailPatch> patches) noexcept;

// A change that needs more than the single word available at its site.
//
// One retail instruction is replaced by a jump into host-owned code in
// `R3000Runtime`'s patch arena. That code does the extra work, re-executes the
// displaced instruction, and jumps back. The site keeps the same fingerprint
// guarantee as `RetailPatch`, and the arena must be untouched before use so
// two patches cannot silently overlap.
//
// The site's own delay slot still executes before the jump is taken, so the
// instruction after the site must remain correct in that position — check it
// when choosing a site.
struct RetailTrampoline {
    std::string_view name;
    std::uint32_t site;
    std::uint32_t original_word;
    std::uint32_t address;
    std::uint32_t return_address;
    // The complete body, including the displaced instruction wherever it
    // belongs. Some patches must run before it and some after, so its position
    // is the body's business. `applyRetailTrampoline` appends only the return
    // jump and its delay slot.
    std::span<const std::uint32_t> body;
};

// The left half of `--widescreen-cull`.
//
// Both retail culls reject a coordinate on the left with a single bit test:
// `RP_ZCullGClip` reads bit 14 of the packed screen pair and `P3DClipCode`
// reads its sign bit. Screen X is an 11-bit saturated GTE result, so within
// its range that bit means exactly "coord < 0" and nothing else — no choice of
// mask or limit immediate can move the threshold to the widescreen left edge.
// Both culls also compute their right bound as `limit - coord`, whose sign is
// a real comparison, which is why only that side could be widened in place.
//
// Dropping the lower bound instead is not the harmless alternative it looks
// like. It is the only rejection retail has for geometry behind the camera:
// `RP_ZCullGClip` has no near-plane test at all, and a vertex behind the eye
// leaves the GTE with its screen coordinate saturated to the edge and its
// `SZ` clamped to zero. Zero depth is ordering-table index zero, the nearest
// slot, so such a polygon is drawn last, over everything — including the HUD —
// stretched across the view. Retail catches it because every vertex lands
// outside the same edge. Remove the left edge from the test and it no longer
// does.
//
// So each cull site instead biases the coordinate by `x_limit - 512` before the
// existing bit test, which moves the threshold to the widescreen left edge and
// leaves the right bound reading the unbiased coordinate. That needs more than
// one word, hence a trampoline per site: fourteen identical outcode blocks in
// `RP_ZCullGClip`, plus `P3DClipCode`'s point test and the radius-aware left
// test inside `P3DClipCodeSphere`.
//
// The object also describes eight retired NCLIP/final-outcode guards so quick
// saves from experimental builds can be normalized. Current mode leaves those
// sites retail: bypassing NCLIP admitted back-facing wall polygons in
// `pokey-wall.stsm`.
//
// Spans point into this object, so it neither copies nor moves.
class WidescreenLowerBounds {
public:
    WidescreenLowerBounds(
        std::uint16_t x_limit,
        std::uint16_t block_visibility_x_limit) noexcept;

    WidescreenLowerBounds(const WidescreenLowerBounds&) = delete;
    WidescreenLowerBounds& operator=(const WidescreenLowerBounds&) = delete;

    [[nodiscard]] std::span<const RetailTrampoline> patches() const noexcept;

private:
    static constexpr std::size_t block_site_count = 14U;
    static constexpr std::size_t cull_site_count = block_site_count + 2U;
    static constexpr std::size_t guard_site_count = 4U;
    static constexpr std::size_t outcode_guard_site_count = 4U;
    static constexpr std::size_t block_visibility_guard_site_count = 1U;
    static constexpr std::size_t site_count =
        cull_site_count + guard_site_count + outcode_guard_site_count +
        block_visibility_guard_site_count;
    static constexpr std::size_t body_capacity = 44U;

    std::array<std::array<std::uint32_t, body_capacity>, site_count> bodies_{};
    std::array<RetailTrampoline, site_count> patches_{};
};

[[nodiscard]] bool applyRetailTrampoline(
    psx::R3000Runtime& runtime, const RetailTrampoline& patch) noexcept;

[[nodiscard]] bool revertRetailTrampoline(
    psx::R3000Runtime& runtime, const RetailTrampoline& patch) noexcept;

// A ledge hang on an `Obstacle` — a platform, a pushable, a table — is held by
// a ticket the obstacle re-issues every guest update. There is exactly one
// place that issues it: the ledge branch of the obstacle's
// `HandleHumanoidCollision`, reached only when `Obstacle::LedgeCheck` accepts.
// `Obstacle::HandleHumanoidObstacleCollision` (`0x8007C178`) disembarks the
// humanoid at the top of every update and calls `Humanoid::LetGoOfLedge`
// (`0x8006C42C`) when nothing re-issued one. A ledge hang on world geometry
// holds no ticket, so that path cannot fire for it — which is why only
// obstacle ledges can drop.
//
// `Obstacle::LedgeCheck` reduces to four independent conditions, live in
// registers at `0x8007C010` where it packs its result:
//
//   $s2  the obstacle's box is wide enough in X and Z
//   $s1  the contact normal faces into the humanoid
//   $a1  the ledge sits within a band of the humanoid's averaged hand height,
//        which is an animated quantity
//   $v0  there is enough clearance below the contact
//   $a2  the contact Y itself
//
// These three trampolines are diagnostic only: they publish counters and the
// last verdict into the arena and change no guest behaviour. They separate the
// two reasons a hang ends — the obstacle stopped issuing a ticket, or
// `Player::_LedgeLatch` decided the player pressed away from the ledge — and,
// for the first, name which of the four conditions failed.
inline constexpr std::uint32_t ledge_trace_check_calls_address = 0x80003900U;
inline constexpr std::uint32_t ledge_trace_check_flags_address = 0x80003904U;
inline constexpr std::uint32_t ledge_trace_check_contact_address = 0x80003908U;
inline constexpr std::uint32_t ledge_trace_letgo_latch_address = 0x8000390CU;
inline constexpr std::uint32_t ledge_trace_letgo_ticket_address = 0x80003910U;
// The two operands of the facing test and its result, sampled where retail
// computes it. A normal that has rolled to +Y means the game has decided the
// humanoid stands on top of the obstacle rather than hanging beside it, which
// is a different failure from a heading that drifted.
inline constexpr std::uint32_t ledge_trace_facing_dot_address = 0x80003914U;
inline constexpr std::uint32_t ledge_trace_normal_x_address = 0x80003918U;
inline constexpr std::uint32_t ledge_trace_normal_y_address = 0x8000391CU;
inline constexpr std::uint32_t ledge_trace_normal_z_address = 0x80003920U;
inline constexpr std::uint32_t ledge_trace_facing_x_address = 0x80003924U;
inline constexpr std::uint32_t ledge_trace_facing_z_address = 0x80003928U;
// The humanoid's action state and vertical velocity at the top of the check.
// The obstacle's "standing on top" fallback issues its ticket only for an
// upward contact normal and a non-positive vertical velocity, so these say why
// that fallback declined too.
inline constexpr std::uint32_t ledge_trace_state_address = 0x8000392CU;
inline constexpr std::uint32_t ledge_trace_velocity_y_address = 0x80003930U;

// Bits in `ledge_trace_check_flags_address`.
enum class LedgeCheckFlag : std::uint32_t {
    box_wide_enough = 1U << 0U,
    normal_faces_humanoid = 1U << 1U,
    ledge_within_hand_band = 1U << 2U,
    clearance_below = 1U << 3U,
    accepted = 1U << 4U,
};

struct LedgeTraceSample {
    std::uint32_t check_calls{};
    std::uint32_t check_flags{};
    std::int32_t check_contact_y{};
    std::uint32_t letgo_from_latch{};
    std::uint32_t letgo_from_ticket{};
    std::int32_t facing_dot{};
    std::int32_t normal_x{};
    std::int32_t normal_y{};
    std::int32_t normal_z{};
    std::int32_t facing_x{};
    std::int32_t facing_z{};
    std::uint32_t humanoid_state{};
    std::int32_t humanoid_velocity_y{};

    [[nodiscard]] friend bool operator==(
        const LedgeTraceSample&, const LedgeTraceSample&) noexcept = default;
};

[[nodiscard]] std::optional<LedgeTraceSample> readLedgeTrace(
    const psx::R3000Runtime& runtime) noexcept;

// A human-readable rendering of one sample's `check_flags`, naming the failed
// conditions rather than printing a mask the reader has to decode.
[[nodiscard]] std::string describeLedgeCheckFlags(std::uint32_t flags);

// The ledge-trace trampolines, as lists. Callers must apply them through these
// rather than naming them individually: a sample point that is declared but
// not installed reads as a zeroed arena, which is indistinguishable from a
// real zero and silently wastes a play session.
//
// The split is a bisection handle, not a feature. The verdict and the two
// `LetGoOfLedge` counters are the set that has been played on; the two
// operand samples were added later and displace instructions deeper inside
// `Obstacle::LedgeCheck`. `ledgeTraceDoesNotChangeWhatLedgeCheckDecides`
// executes the real function with the inner set applied and requires an
// identical verdict, but a passing equivalence test is not a played session,
// so the extra points stay separately switchable until one says otherwise.
[[nodiscard]] std::span<const RetailTrampoline* const> ledgeTracePatches()
    noexcept;

// The operand samples: the humanoid state and vertical velocity at the
// prologue, and the facing dot with both its operands.
[[nodiscard]] std::span<const RetailTrampoline* const>
ledgeTraceInputPatches() noexcept;

// Displaces `move $v0, $a0` at `0x8007C010`, where all four conditions and the
// contact Y are still live.
[[nodiscard]] const RetailTrampoline& ledgeTraceCheckVerdict() noexcept;

// Displaces `sw $s4, 0x50($sp)` at `0x8007BE90`, in the prologue where `$a3`
// is still the humanoid.
[[nodiscard]] const RetailTrampoline& ledgeTraceHumanoidState() noexcept;

// Displaces `addiu $a1, $zero, 1` at `0x8007BF7C`, immediately after the
// facing dot product and before `$s1` stops being the contact normal.
[[nodiscard]] const RetailTrampoline& ledgeTraceFacingInputs() noexcept;

// The `jal Humanoid::LetGoOfLedge` inside `Player::_LedgeLatch`
// (`0x80033788`): the player pressed a direction the latch read as away from
// the ledge.
[[nodiscard]] const RetailTrampoline& ledgeTraceLetGoFromLatch() noexcept;

// The `jal Humanoid::LetGoOfLedge` inside
// `Obstacle::HandleHumanoidObstacleCollision` (`0x8007C650`): no obstacle
// re-issued a ticket this update.
[[nodiscard]] const RetailTrampoline& ledgeTraceLetGoFromTicket() noexcept;

} // namespace stuntmaster::game
