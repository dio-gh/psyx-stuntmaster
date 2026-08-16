# Mouse dual-heading design

Status: Phase 3 implemented and emulator-tested, 2026-08-16. This document
records the selected architecture and its executable realization. Campaign
and feel validation remain the Phase 4 gate.

## Decision

The final mouse mode will keep the two retail angle fields and give their split
an explicit, narrow lifetime:

- `Humanoid+0x114` (`faceAngle`) remains camera-relative travel intent.
- `Thing+0x2c` (`orientation.y`) is the official visible, combat, perception,
  pickup, and interaction heading.
- Only ordinary Player Stand and Run hold the **free-facing lease**, in which
  the fields may differ and mouse yaw owns `orientation.y`.
- Every other action owns its authored/context heading. On an ownership
  transition the guest extension either commits aim, commits travel, or leaves
  the transition to the retail surface/target code, then stops applying mouse
  yaw until ordinary Stand or Run resumes.

This is not a render-only rotation and it is not a state denylist. The positive
lease has exactly two retail locomotion handlers. Everything else remains
retail-owned by default.

Retail Move/action 2 remains unchanged. Ordinary movement remains `AS_RUN` at
full speed. `AS_STRAFE` is entered only if the player explicitly requests the
retail Strafe action; its targeting and state semantics remain stock.

## Why this field assignment is coherent

Keeping `orientation.y` as official body heading means the existing central
consumers require no redirection:

| Consumer | Heading received | Reason |
| --- | --- | --- |
| `Humanoid::Draw` and model transforms | mouse/body | visible body agrees with gameplay |
| attack joints, hand/foot wall collision, and root motion | mouse/body at entry, then authored | attacks originate in the displayed direction |
| enemy FOV and relative-facing decisions | mouse/body | AI observes what Jackie appears to face |
| pickup reach, table fields, and pushable engagement | mouse/body | interactions use the official visible front |
| Run force and ordinary camera-relative movement | `faceAngle` | retail already applies force along travel intent |
| jump/fall/launcher air steering | `faceAngle`, committed at action entry | action 2 and retail contextual physics remain intact |
| climb, ledge, ladder, pole, push, throw, counter, and fighting handlers | context-normalized retail fields | the owning action may turn both fields without input interference |

The exact executable already routes the camera edge-look-ahead through
`Player+0x114`: `0x800A8B48` and `0x800A8B60` load `faceAngle` before the
`rmSin16` calls. ReChan currently reconstructs those expressions as
`orientation.y`; the executable proves that this locomotion-only consumer
needs no new patch.

## Input and directional-command semantics

Mouse buttons continue through the live InputManager map and the retail
`Control`/`FindActionRequest` pipeline. Holds, chords, controller layouts, and
contextual command processing remain retail-owned.

The mouse yaw must be applied earlier than the prototype did. At
`PlayerUserControl+0x218` (`0x80075174`), retail snapshots `orientation.y`
before it computes the angular sector passed to `FindActionRequest` at
`0x80075344`. Applying the current mouse yaw at that snapshot has two effects:

1. travel angle is still computed from the camera and movement vector;
2. back attacks and directional grab variants are derived relative to the
   actual mouse-facing body, rather than to a stale or camera-facing body.

The final `RequestAction` call at `0x80075398` returns to its stock instruction.
No Move-to-Strafe translation, duplicate command bit, or post-decoder action
injection remains.

## Ownership transitions

`Player::SetActionState` begins at `0x800303BC` and is the single ownership
boundary. The fingerprinted trampoline is installed at `0x800303D8`, after
the stack and saved-register frame is established, and classifies the requested
destination before continuing the retail prologue. Hooking the first prologue
word is unsafe: the following register store would execute in the jump delay
slot before stack allocation.

| Ownership class | Destination states | Entry normalization |
| --- | --- | --- |
| Free-facing | Stand `1`, Run `10` | preserve `faceAngle`; mouse may own `orientation.y` |
| Travel-commit | player running jump `6`, standing jump `8`, dive roll `12`, Fall/HardFall/HardLand `13`–`15`, Flip `16`–`17`, Slope Slide `20`, Table Roll `21`, Hotfoot `30` | copy `faceAngle` to `orientation.y`, then retail owns both |
| Aim/contact-commit | Push Object `19`, Push `22`, combat/interaction range `32`–`45` | copy `orientation.y` to `faceAngle`, then retail owns both |
| Context-retail | every other state, including explicit Strafe `11`, wall/ledge/ladder/pole states, reactions, death, and NIS `73` | do not alter either field; the existing caller/handler owns alignment |

The table is semantic and fail-closed: an unknown or unused state receives no
mouse write. It does not attempt to enumerate all states where mouse input must
be suppressed.

Concrete consequences:

- push engagement uses visible facing, then synchronizes `faceAngle` to that
  contact frame, so `_Push` does not fail its 4552-angle test and
  `_PushObject` does not turn back toward the old travel vector;
- a running jump, dive roll, fall, launcher flip, slope, or table roll starts
  in camera-relative travel direction;
- punch, kick, grab, pickup, throw, counter, and their derived variants start
  from mouse-facing body yaw, after which fighting/target code may author turns;
- ledges, ladders, poles, wall jumps, hits, scripts, and NIS retain their
  existing alignment code without a mouse-state exception.

While a context owns heading, the host keeps mouse capture and semantic button
input active but discards relative orientation gestures. When Stand or Run
regains the lease, yaw is reseeded from the live body before accepting the next
non-zero gesture. Context motion therefore cannot queue a surprise snap.

## Free locomotion changes

### Stand

Retail Stand assumes body and travel must converge. Two exact changes are
needed while mouse mode holds the lease:

1. At `0x80031534`, ignore the body/travel comparison and enter retail's
   existing three-update stand-to-run ramp at `0x8003153C`. This preserves the
   stock acceleration path but never starts the turn-around subsystem merely
   because aim differs from travel.
2. At `0x800319A8`, suppress only Stand's final
   `FaceAngleY(faceAngle, 1)`. All earlier action-entry calls, including dive
   roll alignment at `0x80031398`, remain retail.

### Run

Run retains its force accumulator and its `AddForce(..., faceAngle)` path. At
`0x8003291C`, the ordinary final `FaceAngleY(faceAngle, 1)` is suppressed only
while the final action state is still Run. If Run transitioned to an owned
context earlier in the handler, the stock call executes against normalized
fields.

The same Run hook selects animation using retail's exact Strafe sector table at
`0x800D91E0`, without entering `AS_STRAFE`:

| Body/travel delta | Animation | Playback |
| --- | ---: | --- |
| forward sector | 51 | forward |
| rear sector | 51 | reverse |
| one side sector | 52 | forward |
| other side sector | 52 | reverse |

The sector boundaries remain the ones in `Humanoid::_Straif`: 8193, 24576,
40960, and 57343. Movement remains full-speed Run; the `0xC000` Strafe
multiplier is never reached.

When Run releases Move, its stock animation call at `0x80032718` is scoped to
that stop transition. Mouse mode substitutes the authored Strafe idle animation
22 instead of choosing forward-roll 27 or turn-roll 46 from the now-deliberate
body/travel mismatch. Explicit dive rolls use different call sites and are not
affected.

## Bounded mouse-heading controller

The two-axis gesture still selects the absolute camera-relative target with
`cameraYaw + atan2(x, -y)`, but target selection no longer rotates Jackie
directly. A damped host controller requests angular velocity proportional to
the shortest wrapped target error, then clamps both velocity and acceleration
before integrating one emulated VBlank. This supplies two physical limits:

- `mouse.turn_rate` caps sustained rotation in degrees per second;
- `mouse.turn_acceleration` caps changes in angular velocity in degrees per
  second squared.

The shortest-arc error treats values immediately below and above the 16-bit
zero point as neighbors, so crossing `atan2`'s representation boundary cannot
command a nearly full-circle reversal. A stopped mouse completes the bounded
turn toward the last target. Consecutive non-zero gesture samples are unwrapped
against each other, rather than against the lagging body, so a fast circle
cannot reverse merely by lapping the controller. Target lead is capped at one
quarter-turn, preventing that circle from banking an arbitrarily long spin.
Context entry discards target and velocity; free-lease re-entry reseeds from
live body yaw before accepting another gesture.

The host also compares official body yaw with travel yaw while mouse mode is
active. A split during Stand/Run is classified as expected `free_lease`; a
split in any other action is classified as suspicious `context`. Transitions
show an on-screen message, and the console logs state plus both angles and the
signed delta, rate-limited to once per second for a persistent split.

## Exact hook proof

All selected sites are in the always-resident boot executable. Their local
windows in the supported image are:

| Site | Retail word | Neighboring proof | Purpose |
| --- | ---: | --- | --- |
| `0x800303D8` | `AFBF0040` | `AFB40038 00A0A021 AFB30034 00C09821 [AFBF0040] AFB5003C AFB20030 AFB00028 8E220058` | action ownership transition |
| `0x80031534` | `14430024` | `8E020114 00000000 [14430024] 00000000 8E020268 00000000` | Stand movement comparison |
| `0x800319A8` | `0C0193AC` | `00000000 8F850D6C [0C0193AC] 24060001 8E040050 00000000` | Stand final facing |
| `0x80032718` | `0040F809` | `8C420014 00000000 [0040F809] 00003821 0800CA8D 00000000` | Run stop animation |
| `0x8003291C` | `0C0193AC` | `00000000 8E050114 [0C0193AC] 24060001 8E020058 00000000` | Run facing and directional animation |
| `0x80075174` | `8C49002C` | `00000000 8C480028 [8C49002C] 8C4A0030 AFA80018 AFA9001C` | current-yaw snapshot before command direction |

Every hook can re-execute or emulate its one displaced instruction and has an
unambiguous rejoin. Stock/off behavior therefore remains byte-for-byte
expressible. Installation must verify all six complete windows before changing
any site and must roll the entire set back if one write fails.

The old `0x80075398` action trampoline and `0x80034010` Strafe-target
trampoline are removed. Their stock words are part of the new installation
precondition, preventing mixed prototype/final states.

## Guest code and data placement

The current 4 KiB patch arena is already partitioned among widescreen,
diagnostic, and legacy mouse bodies. The final design reserves a separate 4 KiB
guest-extension code/data arena at `0x80004800`–`0x800057FF`:

- it begins exactly after the 2 KiB menu-object arena
  (`0x80004000`–`0x800047FF`);
- it ends below the reserved interrupt stack bottom (`0x8000DFF0`);
- it is below the first overlay address (`0x80010000`) and retail BSS clear;
- the boot executable and all four exact retail overlays have zero direct
  J/JAL, GP-relative, or LUI-plus-immediate references into the proposed range.

The arena holds the six trampoline bodies, a shared ownership/animation helper,
and only transient words such as last published mode. No retail object is
grown, no save structure changes, and no unused Player field is guessed.

Mouse yaw and enabled/off mode continue to arrive through the ignored tail of
the direct-pad buffer. The wire mode collapses to `0=off`, `1=mouse`; the
`character_relative` value and configuration mode are removed.

## Mode, quick-save, and failure behavior

- Enabling seeds yaw from current `orientation.y`, clears the retail
  turn-around latch/timer, and does not rotate Jackie until a new gesture.
- Disabling during Stand/Run copies `faceAngle` to `orientation.y` once, then
  every hook executes stock behavior. Disabling in a context leaves the
  context's heading untouched.
- Focus loss, pause, photo mode, invalid object graphs, and non-player control
  discard motion and release capture exactly as before.
- Quick saves normalize all six sites and the extension arena out of the saved
  runtime. Load reapplies the fingerprinted set transactionally and reseeds
  transient yaw/lease state from the live Player.
- Any fingerprint mismatch, invalid pointer, unknown state, or malformed live
  input map fails closed to retail behavior. Partial installation is never
  accepted.

## Phase 3 executable acceptance matrix

The implementation gate was not merely that the hooks ran. The combined
emulator tests, fingerprint checks, and executable-control-flow audit cover:

1. off-mode register, branch, RAM, action-bit, target, speed, and animation
   equivalence at every displaced site;
2. camera-relative Run force with arbitrary mouse yaw and unchanged Move bit;
3. the four directional animation sectors, full speed, and idle transition;
4. current-frame mouse yaw feeding the retail directional-command decoder;
5. aim-commit attacks, grabs, pickups, throws, push entry, and enemy-facing
   reads;
6. travel-commit standing/running jumps, dive roll, fall, launcher, slope,
   table roll, and hotfoot;
7. untouched ownership for wall, ledge, ladder, pole, reaction, death, NIS,
   explicit Strafe, pause, and front-end paths;
8. push-angle retention and force agreement after contact claim;
9. context-to-free reseeding with no queued mouse snap;
10. transactional install/revert, wrong-window refusal, quick-save
    normalization, and load rehydration;
11. interpreter, cached recompiler, and native recompiler agreement through
    the ordinary runtime test coverage;
12. removal/rejection of `character_relative` in config, F10 cycling,
    notifications, documentation, and tests.

## Feasibility conclusion

The architecture is feasible without taking over the input subsystem and
without patching every consumer. It uses retail's existing travel field,
official body field, action decoder, Move state, force paths, context handlers,
and authored directional animation assets. Six resident, reversible seams add
the missing ownership contract at the points where retail previously assumed
body and travel were identical.

The remaining uncertainty is gameplay feel rather than an unproven engine
dependency. Animation transitions and context handoffs now have Phase 3
emulator coverage; Phase 4 campaign play must exercise them across actual
levels and tune the bounded turn controller. No known static-system
inconsistency requires a different architecture.
