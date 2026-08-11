# Retiming audit: un-retimed per-frame counters and timers

Audit date: 2026-08-09. Trigger: the Butch `_Stomp` landing event
(`920d8be`) was a boss attack whose event cadence was still 60 Hz because a
private `Think`-local counter (`Butch+0x268`) advanced once per guest update
instead of once per authored 30 Hz step. This document answers "how many more
of those are there?" and tracks the fix.

Method: an exhaustive self-increment/self-decrement scan of the Ghidra dump at
`C:\dev\vibe\openstuntmaster\re\src` (`param + 0xNN = param + 0xNN + 1` and
`+ -1`, int and short forms), cross-checked against the typed reconstruction
at `C:\dev\pub\ReChan\src` (which runs all logic on a fixed 30 Hz step and so
never needed per-counter fixes, but confirms field semantics), then triaged
against the live retime hook table (`src/stuntmaster/game/retime_hooks.cpp`)
and the coverage map in `docs/re/README.md`.

## Bottom line

The systemic gap is the **humanoid/Boss action-state machine, the Behaviour
layer, and the menu/HUD pulse layer**. The Butch fix was the first
state-machine counter; it is not the only one. Every per-frame duration,
cadence, or escape timer inside `ProcessAction__8Humanoid` /
`ProcessAction__6Player` state functions, inside `Behaviour::Process`
handlers, and inside the menu/HUD update path runs at `k` times retail at a
high guest update rate, because all three must run every update and are not
covered by any gate. Verified by the same evidence shape as Butch: the
counter is incremented once per call and a visible transition/sound/damage/
decision/blink fires at a fixed count.

**Status:** the boot tier is landed (`retimeCounterHooks()` in
`src/stuntmaster/game/retime_hooks.cpp`, armed by `--retime-clock`): the
pause-menu colour pulse, the HUD `hdTtlive`/`hdAnimTextOvl` holds, the
Player/Humanoid state timers, and the boot Behaviour timers are installed and
covered by `authoredCounterHooksKeepTheirCadence` in `core_tests.cpp`. The
BOL tier is also landed: Butch's stomp counter is migrated into the generic
Shape-1 table, and `_Collapse__4Boss`, Dante's missile/retarget counters,
`_ButchDMS`, `_ButchDMS_Charge`, `_PaulDMS` (recovery + attack duration),
the henchman engage delay, and `CounterAttack`'s hit frames are installed
with fingerprint windows verified unique across all four `*_REL.BIN` files
(`overlay=47`, including the title-frame decider). Three live-reported
interaction bugs are fixed: the fire-pit
run/burn flicker and doubled burn damage (AS_Hotfoot added to the
collision-pass held exceptions — `Think__8Humanoid` clears the `+0x170`
contact word every update, and the hook now re-issues `Untouchable`'s fire
bit `+0x170:3` directly for every burning humanoid on held updates instead of
running the inner collision, so the per-authored damage tick does not double;
the whole-list scan fixes NPCs after an already-burning player) and the 2x push
speed (OL1 `pushable_collision_hold` gates the displacement + engage counter
in `Pushable::HandleHumanoidCollision`, which the AS_PushObject exception
runs every update), and the 2x conveyor carry while running (NBOL
`conveyor_collision_hold` returns from `Conveyor::HandleHumanoidCollision` on
held AS_Run contact checks, while counted updates retain the retail body). A
fourth report — the horizontal pole swing running 2x
fast at 60 Hz, then flickering between the arc and a sinking position under
the first (whole-body hold) attempt — is fixed by `pole_swing_timeline`, a
timeline-only gate at `0x80033078` (see the README section "The player's
pole swing holds its timeline, not its pose-apply"): the pendulum
accumulation holds on held updates while the idempotent pose-apply runs every
update, the same shape as the `Stack` tumble fix. Remaining: live behavioural
confirmation of the BOL entries on a boss route and the two interaction fixes
on their repro saves.

## The pause-menu selected-item blink — reported, root-caused, fixed

User report: the pause menu's selected item blinks faster in 60 Hz mode.

Root cause: the pause menu runs under `gsMenuState` (`0x80029EF8`), which is
a documented steady handler, so it updates at the requested guest rate. Each
game update `MenuMgr::Invoke` (`0x8005FB00`) calls the top menu's
`Update__6hdMenu` (`0x8005D0E8`), which steps the selected item's colour
pulse once per call:

```text
8005CD10  addiu $sp, $sp, -0x18      ; MenuColorNext__FR12xcColour1555
8005CD14  sw    $s0, 0x10($sp)
8005CD18  sw    $ra, 0x14($sp)
8005CD1C  jal   0x8005cc44           ; CalcNextColor: accumulator step + colour
8005CD20  move  $s0, $a0
```

`CalcNextColor` (`0x8005CC44`) is the only place the colour-chase
accumulator steps; `MenuColorNext` then wraps back to `MenuColorStart` when
the chase passes the end colour. One guest update = one chase step, so at a
divisor of 2 the red-to-yellow cycle runs twice per authored step.

Fix (landed): `menu_colour_pulse`, a call gate at `0x8005CD1C` (callee
`CalcNextColor`, rejoin `0x8005CD24`) in `clock_hooks`, reusing the existing
`callGateHook`. The site's delay slot `move $s0, $a0` runs on both paths; the
`$s0`/`$ra` prologue saves are unaffected. Every menu colour pulse funnels
through this one function — the pause menu, the tally prompt pulse, and the
OL2 front-end menus — so one hook covers all of them. The FE title menus run
under `gsTitleLoopState`, which is itself a high-frequency steady handler.
Because title calls neither `Step__4Time` nor `MenuDraw`, PRESS START read a
stale counted phase and advanced at 60 Hz. The fingerprint-gated
`title_frame_decision` hook at OL2 `TitleScreen::SelfUpdate` `0x80011938`
publishes a fresh decision immediately before `MenuColorNext`, models the
displaced `addiu $a0,$s0,0x34`, and restores the authored 30 Hz pulse cadence.

### The stale pause decision (second root cause, fixed with the gate)

The gate alone was not enough: the master hold decision is published by the
`Step__4Time` hook, which only runs during play. While `gsMenuState` is up,
the decision stays frozen at whatever the last play update decided — so the
pause's own authored-rate animation (the chase) either never advances
(frozen) or advances every guest update (2x fast), depending on the stale
phase. Reported live as: cold 60 Hz launch = no blink at all; F7 to 30 Hz =
normal blink; F7 back to 60 Hz = 2x.

Fix (landed): `menu_frame_decision`, a semantic hook at `MenuDraw`'s
prologue `move $s0, $a0` (`0x80029DC0`) that calls `retime.decide()` once
per guest update and models the displaced move. `MenuDraw` runs once per
update in every menu state (pause, location menu, end-level) and is never
called during play, so the two deciders cannot both run in one update. A
divisor of one always counts, exactly as a retail-cadence menu needs.

---

## Tier 1 — Humanoid / Boss action-state timers (same bug shape as Butch)

All of these are per-frame "stateTimer" quantities: written by the state
function once per guest update, compared against an authored duration or a
hardcoded threshold, and the comparison fires a visible state transition or
event.

| # | Function (entry) | Field | What fires | Image | Status |
| --- | --- | --- | --- | --- | --- |
| 1 | `_Collapse__6Player` `0x80032EB0` | `Player+0x268` | get-up transition when `> +0x1D0` | boot | landed |
| 2 | `_HorizontalPoleSwing__6Player` `0x80032F8C` | `Player+0x268` | gates the flip dismount (turn input while `+0x268 != 0`) | boot | landed |
| 3 | `_SlopeSlide__6Player` `0x8003389C` | `Player+0x268` | turn-out of the slide after 8 frames | boot | landed |
| 4 | `_Collapse__8Humanoid` `0x80068DD4` | `Humanoid+0x134` | enemy get-up when `> +0x1D0`; `SignalEnemyGetUp` | boot | landed |
| 5 | `_GotHitFreeForm__8Humanoid` `0x8006C3EC` | `Humanoid+0x134` | switches to `_FlyingBack` after the free-form hit time global; also holds gravity off | boot | landed |
| 6 | `_BackGrabCharacterReceivePreLatch__8Humanoid` `0x800683C4` | `Humanoid+0x134` | escape window: back to stand after 0x1E frames | boot | landed |
| 7 | `_Pause__8Humanoid` `0x80067288` | `Humanoid+0x144` | guard/pause countdown; stand at zero. The decrement runs in the guard branch's delay slot, so the hook is on the store at `0x800672D8` | boot | landed |
| 8 | `_Collapse__4Boss` `0x8001A90C` | `Boss+0x134` | boss get-up when `> +0x1D0`; damage-move selection, `SignalEnemyGetUp` | BOL | landed |
| 9 | `_MissileAttack__5Dante` `0x8001BDE4` | `Dante+0x268` | one missile per frame until the pattern count; volley end resets and exits | BOL | landed |
| 10 | `_TargetMissileAttack__5Dante` `0x8001C358` | `Dante+0x298` | inter-shot retarget delay (`iGp00000C8C`); snap + fire once per window | BOL | landed |

Tier 1 notes:

- `Humanoid+0x134` is ReChan's `stateTimer` (confirmed in
  `ReChan/src/ai/humanoid.h` "PSX +308: state timer"), and `+0x1D0` is the
  humanoid-data ID used as the per-character get-up duration. It is shared by
  the three Humanoid sites (4/5/6) plus Boss (8).
- `Player+0x268` has three increment sites (1/2/3) plus Dante's `+0x268`
  (9) — a different class at the same offset, so the fingerprint-gated BOL
  hook and the boot hooks are separate.
- Site 2's whole state function is additionally covered by the
  `pole_swing_timeline` timeline gate (see the README section "The player's
  pole swing holds its timeline, not its pose-apply"); with the gate the
  counter site runs every update (the pose-apply section), so the counter
  hook's hold on held updates is what keeps the flip-dismount gate on the
  authored schedule.
- Sites 1-3 and 7 are hit by any first-level play: a knockdown, a pole
  swing, a slope slide, and a guard pause.

### Tier 1b — HUD / menu pulse layer (boot executable)

The HUD updates every frame during play (a steady handler) through
`HUD::SelfUpdate` (`0x8003FC10`).

| # | Function (entry) | Field | What fires | Status |
| --- | --- | --- | --- | --- |
| B1 | `MenuColorNext` `0x8005CD10` | chase accumulators | selected-item colour pulse; call gate at `0x8005CD1C` | landed |
| B2 | `Update__8hdTtlive` `0x8008EF0C` | `+0x8` countdown | hides the overlay at zero; guard at `0x8008EF1C`, epilogue `0x8008EF34` | landed |
| B3 | `Update__13hdAnimTextOvl` `0x8008F664` | `+0x30` slide, `+0x8` ttlive | whole-`Update` held prologue at `0x8008F66C` (its `+0x38` pause clamp makes a counter-level hold unsafe) | landed |
| B4 | `Update__7hdTally` `0x80090AC0` | `+0x68` count-up (`+= 0x1C3` per call), `+0x1D` popup countdowns | end-of-level tally / combat popups; whole-`Update` held prologue at `0x80090AC8` (hosted by the Director under `gsPlayState`, a steady handler) | landed |
| B5 | `Update__9CBVEffect` `0x8008CF94` | `Update__11CBVPrimData` `+0x3C` countdown, `+0x28` UV cursor | colour/UV world-effect animation; third WEffect-family override the held prologues miss, so the `jal` at `0x8008CFAC` is call-gated | landed |

Notes:

- B5 was found by the Tier-3 audit: `WEffect::Update`/`FWEffect::Update` have held prologues, but the vtable dispatches CBV effects to `CBVEffect::Update` (a distinct address), so its `Update__11CBVPrimData` step ran per guest update. The two-arg `Update__11CBVPrimDataiP9tPrimGeom` in `Block::Draw` is draw-time colour application only (no counters) and needs no gate.
- The Tier-3 audits are closed: `IncFrame` callers are the gated ladder climb only; `Path::Move`'s only per-frame users are the covered Platform divide and the held WEffect update; `_AiFollowPath`'s `+0x28` back-walk is event-driven and low value; `Pickup` and `Blast` were closed previously.

## Tier 2 — Behaviour decision/cooldown timers (boot executable)

`Behaviour` handlers run once per game update from `Behaviour::Process`.

| # | Function (entry) | Field(s) | What fires | Status |
| --- | --- | --- | --- | --- |
| 11 | `_ButchDMS__9Behaviour` `0x8001CB20` | `Behaviour+0x40` | attack/decision cadence: re-decides at 0x1A frames | BOL | landed |
| 12 | `_ButchDMS_Charge__9Behaviour` `0x8001D178` | `Behaviour+0x40` | charge duration: gives up at 0x33 frames | BOL | landed |
| 13 | `_PaulDMS__9Behaviour` `0x8001D7CC` | `Behaviour+0x68`, `+0x64` | recovery countdown and spotlight-attack duration countdown | BOL | landed |
| 14 | `_OscarHenchmanDMS__9Behaviour` `0x8001E7DC` | global `0x800DD9B4` | henchman engage delay (0x0F frames) | BOL | landed |
| 15 | `_BackoffAndTaunt__9Behaviour` `0x800757EC` | `Behaviour+0x40` | 0x0F-frame backoff before re-engaging | landed |
| 16 | `_BackOutOfTheFight__9Behaviour` `0x80075A68` | `Behaviour+0x40` | 0x0B-frame backout before re-engaging | landed |
| 17 | `NavigateWorld__9BehaviourRl` `0x80076870` | `Behaviour+0xB8` | frames an evasive heading is held after a wall bump; guard at `0x800768B0`, epilogue `0x80076C6C` | landed |
| 18 | `ComplexAttack__9Behaviour` `0x80074914` | `Behaviour+0x48` | command-script index; advances only when no attack node is active | landed |
| 19 | `NavigateEnemies__9Behaviouri` `0x80076C84` | `Behaviour+0x40` | increments every call; reader needs confirmation | landed |
| 20 | `CounterAttack__9Behaviour` `0x8001EDB4` | `Behaviour+0x94/0x98/0x9C` | per-attack frame counters feeding the counter-attack threshold | BOL | landed |

Notes:

- ReChan's typed port names `+0x40` `navDecisionCounter` and confirms the 15
  and 11 frame thresholds (`ReChan/src/ai/behaviour.cpp`, `BackoffAndTaunt` /
  `BackOutOfTheFight`), independent corroboration of the per-frame semantics.
- 18 is lower confidence: `ComplexAttack` advances `+0x48` only when the
  humanoid's active fighting-node field (`+0x1E4/+0x1E8`) is zero, i.e.
  mostly once per completed script step. Hooking it is still correct (the
  hold only matters if a script step ever completes within one guest
  update).
- 11-14 are in the BOL overlay (`0x8001xxxx` window) and need fingerprint
  windows confirmed unique across all four `*_REL.BIN` files.

## Tier 3 — verify before hooking

| # | Function (entry) | Field | Status |
| --- | --- | --- | --- |
| 21 | `_AiFollowPath__9Behaviour` `0x80074C80` | `Behaviour+0x28` | node back-walk countdown; mostly event-driven |
| 22 | `Path::Move` `LinearPath` `0x800A513C`, `SplinePath` `0x800A59CC` | `Path+0x2C` | the path-time accumulator. Platform divides its speed at `0x800227D0`; WEffect updates are held. Audit remaining callers before deciding a site is needed. |
| 23 | `UVPrimData::Update` `0x80098BE0` | `+0x1C` countdown | Conveyor's four calls are inside the held `Conveyor::Think`. Check for other callers. |
| 24 | Explicit `IncFrame__13AnimStructure` callers | anim frame `+0x54` | Ladder climb is gated; verify no other per-frame caller. |
| 25 | `Pickup::Think` `0x8006D984` / `Move` | none | Closed: motion via the shared integrator, pose from anim joint matrices (clock-driven). |
| 26 | `Blast` (`OL1 0x80016528`) | `stateTimer` | Closed: inside the held `Blast::Think`. |

## Third category (known, not a 60 Hz bug)

`rFrameCount60` consumers scale with the emulated VBlank rate, which stays 60
at every guest update rate up to 60 Hz, so they are correct at 30 and 60 and
short only above 60 Hz. Already tabulated in `docs/re/README.md`; no action
at 60 Hz.

## Closed items — verified covered by existing retiming

- **Master clock** `Step__4Time` (`0x80044A40`) — the decider.
- **Scene animation lists, camera animation, and the whole effects manager**
  — `animLoopDSTACK` (`0x8002B368`) is gated by `scene_animation`; that one
  gate also covers `UpdateAnim__6Camera` (including `CameraShake`'s `+0x190`
  countdown), the four `AnimateLoop` lists, and `UpdateAll__7Effects`
  (GEffect, PWEffect, FPWEffect, Trails, particles, `WEffect::gGlobalFrame`).
- **VRAM flipbooks** — `vram_animation` gate.
- **Obstacle `Think` holds** — 19 classes plus `Stack` gates; inside them:
  `Collectible`, `Explosive`, `KnockDown`, `Launcher`, the generator family.
- **Platform divide conversion** — path speed, teeter, fall partition,
  counters, `Bob`, snapshot, bobbed-Y.
- **WEffect/FWEffect updates** — held prologues.
- **Animation frames** — cursors read the master clock; `IncFrame` only
  where gated (ladder).
- **Pickups** — see item 25.

## Implementation — one table, two generic hook bodies

The boot tier is implemented in `retime_hooks.cpp` as two generic bodies plus
a declarative table, the same design as `retimedObstacleThinks`. Every
descriptor carries only `{name, site, rejoin}`; the bodies decode their
instructions from guest RAM, so there is no per-site arithmetic.

### Shape 1 — `counterStepHook`: `addiu $vR, $vR, ±1` (or a `sw` store-site)

Verified at every count-up/count-down site, e.g. `Player::_Collapse`:

```text
80032ED8: lw       $v0, 0x268($s0)      ; Player::_Collapse
80032EE0: addiu    $v0, $v0, 1          ; <- SITE
80032EE4: sw       $v0, 0x268($s0)      ;    delay slot: stores the OLD value
80032EE8: lh       $v1, 0x268($s0)      ;    later comparisons reload memory
```

The site's delay slot runs first (the hook's delay slot), so by hook time the
old value is already stored and `$vR` is still old. The generic body:

- **held:** writes the identity (`$vR = $vSrc`, a no-op for `rs == rt`) — the
  delay-slot store of the old value stands, and every later comparison reads
  old.
- **counted:** models the addiu, re-issues a delay-slot `sw $vR, off($base)`
  with the new value, and recomputes a delay-slot `slt[u] $vD, $x, $vR` with
  it (the `_GotHitFreeForm` site).
- **store-site form** (`_Pause`): the counter update runs in a preceding
  branch's delay slot, and a site that is itself a branch delay slot would
  lose the branch target (the hook dispatch overwrites `next_pc`). The hook
  therefore sits on the store: held skips it, counted issues it.

### Shape 2 — `countdownGuardHook`: branch whose delay slot decrements

Two sites (`Update__8hdTtlive`, `NavigateWorld`):

```text
8008EF14: lw       $v0, 8($a0)          ; hdTtlive: countdown
8008EF1C: bltz     $v0, 0x8008ef34      ; <- SITE (guard on the OLD value)
8008EF20: addiu    $v0, $v0, -1         ;    delay slot: decrement
8008EF24: bnez     $v0, 0x8008ef34      ;    branch on the NEW value
```

The hook replaces the guard: **held** returns the epilogue (no decrement
takes effect, no side effect on the guarded paths fires); **counted**
recovers `old = $vR + delta` from the delay-slot word and models the branch
on it, so the branch target's delay slot (the store) runs normally. The
descriptor carries only the epilogue; the branch opcode/offset and the
decrement are decoded from the site words.

### Rules that came out of implementation

- A hook site must never be another branch's delay slot: the hook dispatch
  overwrites `next_pc`, so the preceding branch's target is lost. The
  `branchDelaySlotsExecuteBeforeTheTarget` interpreter test pins the
  ordering, and `_Pause` demonstrates the store-site workaround.
- Both bodies fail safe: an unrecognized site word resumes at the rejoin
  (retail flow), and every site needs the whole-register-file
  stock-vs-patched equivalence test at its rejoin.

## Suggested verification order

1. Boot tier live A/B at 60 Hz on the tested route (get-up timers, guard
   pause, backoff pacing, HUD hint lifetimes). The pause blink is the fastest
   check: count the selected item's colour cycles over a wall-clock window at
   30 vs 60 Hz — they must match.
2. BOL tier: migrate the bespoke Butch hook into the Shape-1 table
   (behaviour must be identical — the diff test proves it), then add
   `_Collapse__4Boss`, `_MissileAttack__5Dante`, `_TargetMissileAttack`, and
   the BOL Behaviour timers with fingerprint windows.
3. Tier 3 audits (21-24) and the tally host state (B4).
