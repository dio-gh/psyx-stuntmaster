# Mouse heading dependency audit

Status: Phase 1 complete, 2026-08-16. Design and implementation are deliberately
deferred to the Phase 2 touchpoint.

## Question

The first camera-relative mouse prototype reused retail action 6, Strafe, to
let camera-relative travel (`Humanoid+0x114`, `faceAngle`) differ from Jackie's
body yaw (`Thing+0x2c`, `orientation.y`). Live play established that this is not
a general locomotion primitive: it is slower, blocks launcher air control when
it replaces Move, and makes push interactions inconsistent.

This audit identifies every relevant meaning carried by those two angles and
by Move/Strafe state before a replacement architecture is selected.

## Sources and coverage

- Exact supported NTSC-U executable: `SLUS_006.84`, SHA-256
  `5aae79f0d603bf95bdcf9a2c278d1891dcbd9e835a59d4673d6e6dbd82665c64`.
- Retail `GAME_REL.SYM` function names and addresses.
- Inferred Ghidra sources for `PLAYER.CPP`, `HUMANOID.CPP`, `AI.CPP`,
  `COLMGR.CPP`, `THING.CPP`, `LADDER.CPP`, and `PUSHABLE.CPP`.
- ReChan's named reconstruction, including all direct references under
  `src/ai` and `src/gen` to `orientation.y`, `faceAngle`, Move/Strafe command
  bits, and `AS_STRAFE`.
- Existing emulator execution of the two mouse trampolines against the exact
  retail instruction windows.

The audit is Player-scoped. Uses of the same inherited fields on unrelated
obstacle or NPC instances are excluded unless they consume the Player's state
or participate in a Player interaction.

## The three retail channels are not interchangeable

| Channel | Exact location | Retail meaning | Important writers |
| --- | ---: | --- | --- |
| Body yaw | `Thing+0x2c` | Authoritative object/model transform and physical/combat facing | `FaceAngleY`, state handlers, ladders, ledges, fighting moves, scripts |
| Desired/travel yaw | `Humanoid+0x114` | Desired facing or direction selected by control/navigation | `SetDesiredMoveDirection`, `FacePointDesired`, `PlayerUserControl` |
| Command/action state | `Humanoid+0x160/+0x164` | Per-tick semantic requests and persistent handler state | `RequestAction`, `SetActionState`, collision/context handlers |

`FaceAngleY` (`0x80064EB0`) is the normal bridge from desired yaw to body yaw.
Ordinary Run uses that bridge and therefore assumes the angles converge.
Strafe is the exceptional retail state that deliberately preserves their
difference.

## Input and command decoding

`Behaviour::PlayerUserControl` (`0x80074F5C`) computes a camera-relative
direction and writes it through `SetDesiredMoveDirection` (`0x80064EA8`). It
then requests the result of the normal command table:

- action 2 / command bit 2 is directional Move;
- action 6 / command bit 6 is the explicit Strafe control (retail R2);
- jumps, directional jumps, grabs, back attacks, heavy attacks, and held
  variants are derived separately from direction, chords, and duration.

Consequences:

1. Replacing action 2 with action 6 changes semantics, not merely animation.
2. Publishing both bits is invalid. `Humanoid::_Straif` treats Move as a request
   to return to Run, while Run/Stand treat Strafe as a request to enter Strafe.
3. Contextual handlers often test bit 2 exactly. The launcher `_Flip`, falls,
   landing transitions, pushing, ladder climbing, ledge handling, and dive-roll
   exits are confirmed consumers.

The final design must leave retail command decoding intact. A second heading
must not be smuggled through another gameplay action.

## Ordinary locomotion and Strafe

| Retail path | Address | Angle/action behavior | Mouse inconsistency |
| --- | ---: | --- | --- |
| `Player::_Stand` | `0x80031350` | Compares body yaw with `faceAngle`; turns around or enters Run; bit 6 enters Strafe | A permanent split changes stand-to-run timing and turn-around logic |
| `Player::_Run` | `0x800325CC` | Calls `FaceAngleY(faceAngle)` and applies full run force along `faceAngle` | Re-couples body and travel; cannot retain independent mouse facing unchanged |
| `Player::_Straif` | `0x80033FF8` | Combat-target wrapper; scales `moveSpeed` by `0xC000` | Exactly 75% movement speed and combat-state side effects |
| `Humanoid::_Straif` | `0x80067610` | Force follows captured `faceAngle`; animation sector follows body/travel delta | Useful authored animations, but coupled to action 6 and state 11 |

The Player strafe table at `0x800D91E0` provides idle animation 22 and
directional animations 51/52, including reverse playback. Those assets are
useful independently of the state that normally selects them.

Strafe state is externally observable. Enemy/boss behavior contains explicit
checks for the Player being in `AS_STRAFE`; for example Grontar changes its
decision to a grab, and boss targeting logic distinguishes a strafing Player
with a matching target. Suppressing retail target acquisition does not remove
these state-level effects.

Conclusion: action 6/state 11 cannot be the final universal locomotion carrier.

## Rendering and animation

`Thing::Draw` (`0x800616EC`) copies `Thing::orientation` directly into the
model transform. The visible body yaw is therefore the same field used by
gameplay.

The coupling continues below rendering:

- `Thing::GetObjectToWorldSpaceVector` (`0x80062874`) transforms local offsets
  with the same orientation.
- Player/Humanoid action-state setup selects Run, Strafe, jump, push, ladder,
  ledge, pickup, throw, and fighting animations.
- Strafe chooses animations from the body/travel angular difference.
- Fighting move root motion in `ProcessGenericFightingMove` (`0x8006AE0C`) and
  `ProcessBodyThrow` (`0x8006B0A0`) advances position along body yaw.
- Fighting-node selection and `ReSyncOrientation` (`0x8006BF04`) can change
  both body yaw and `faceAngle` as authored moves turn Jackie.

A render-only rotation is therefore insufficient by itself: attack joints and
authored root motion would disagree unless combat evaluation receives the same
heading deliberately.

## Combat and targeting

| Consumer | Address | Uses body yaw for |
| --- | ---: | --- |
| `HTW_HandleHandFootCollisions` | `0x800A7FAC` | Hand/foot wall sweep direction |
| Fighting strike processing | `0x8006A714`, `0x8006AE0C` | Attack joint windows and forward root motion |
| `GetImpactRegion` | `0x800680B8` | Front/side/back hit classification |
| Back-grab tests | `0x8006B778` family | Relative attacker/target orientation and attachment |
| Table/pickup throw handlers | `0x80068718`, `0x800685A8` | Release direction |
| Counter pre-latch/latch | `0x80069518`, `0x800695A8` | Target-facing override |

Enemy behavior also reads the Player's body yaw for field-of-view and relative
facing decisions. This is semantically a visible/combat heading, not a travel
heading.

Mouse-facing attacks therefore need body/attack evaluation to agree at attack
entry. Authored fighting moves must then be allowed to own their turn deltas
without the input hook overwriting them each tick.

## Traversal and environmental collision

| System | Retail evidence | Required meaning |
| --- | --- | --- |
| Falling and air steering | `FallingPhysics` `0x80032368`, `_Jump` `0x80031C68`, `_Fall` `0x80032444` use Move and `faceAngle` | Camera-relative travel |
| Launcher/bounce | `_Flip` `0x80031A78` gates force on Move and uses `faceAngle` | Camera-relative travel; never Strafe semantics |
| Wall jump | Player wall checks and `_WallJump` `0x80032D8C` use body/desired angles and then take ownership | State-owned traversal heading |
| Ledge detection | `CheckForLedges` `0x8006A1D8`, `CheckForLedges2` `0x8006A3B0` cast probes along body yaw | Locomotion approach heading |
| Ledge latch/pull-up | `0x8003352C`, `0x800337A8`; obstacle latch aligns body to ledge | State-owned ledge heading |
| Ladder collision/climb | ladder collision tests Move; `PutHumanoidOnLadder` `0x8008A570` writes ladder yaw | State-owned ladder heading |
| Horizontal poles | pole collision and `_HorizontalPoleSwing` preserve/replace body yaw | State-owned pole heading |
| Slopes/table roll | `_SlopeSlide` `0x8003389C`, `_TableRoll` `0x80033DF8` mix body and travel angles | Surface/action-owned heading |
| Dive roll/hotfoot | `_DiveRoll` `0x80066E3C`, `_Hotfoot` `0x80067F54` apply force along body yaw | Action-entry travel heading |

`COLMGR.CPP` also uses body yaw for climb offsets and camera look-ahead edge
tests. `Obstacle::LedgeCheck` tests whether the humanoid is facing the obstacle
front using body yaw. These are why merely preserving action 2 fixed the bounce
blocker but cannot make a permanently mouse-owned `orientation.y` safe.

## Interactions and carried objects

| System | Address/evidence | Conflict created by universal mouse body yaw |
| --- | --- | --- |
| Pickup reach | `AI::GetPickupWithinReach`, `0x80056B04` | Search point is 300 units along body yaw |
| Push contact | `Pushable::HandleHumanoidCollision`, `0x80018AF8` | Impulse projection uses humanoid body yaw |
| Player push state | `_Push`, `0x80032A48` | Exits when body and desired angles differ by about 25 degrees |
| Push-object state | `_PushObject`, `0x80032B80` | Requires Move and applies force along body yaw |
| Table interaction | table field test uses humanoid body yaw | Interaction cone follows body rather than travel/contact |
| Throws | `_TableThrow`/`_Throw` | Released object follows body yaw |

Push and traversal states are not input contexts in which mouse yaw can keep
winning every frame. They establish an interaction frame with an object or
surface and must be able to own the authoritative heading for their lifetime.

## State ownership and overwrite hazards

The current trampoline runs at the end of `PlayerUserControl` whenever that
behavior handler remains installed. Player action handlers run independently;
being in PlayerUserControl does not imply that the current action state is
ordinary locomotion.

Confirmed state-owned yaw writers include:

- ladder and ledge attachment;
- wall jumps, poles, slopes, table rolls, and launch flips;
- target-facing counters and grabs;
- fighting move resynchronization and turn deltas;
- scripted population/checkpoint restoration and non-interactive control
  transitions.

An unconditional per-tick write of mouse yaw can undo those decisions between
updates even when action 2 is preserved. The final channel needs an explicit
ownership protocol, not an expanding denylist of action-state numbers.

## Complete inconsistency matrix

| Domain | Travel heading | Visible/combat heading | Context can take ownership? | Action 6 safe? |
| --- | --- | --- | --- | --- |
| Idle | none | mouse | no | unnecessary |
| Camera-relative ground locomotion | camera/input | mouse | collision may transition | no |
| Jump/fall/launcher | camera/input | normally travel/action | yes | no |
| Attack/combo/counter | authored root motion | mouse at entry, then authored | yes | no |
| Pickup/throw | contact or mouse by semantic action | object/throw heading | yes | no |
| Push | object/contact normal | object/contact normal | yes | no |
| Ledge/wall/ladder/pole | surface frame | surface frame | yes | no |
| Slope/table/dive roll/hotfoot | action/surface frame | action/surface frame | yes | no |
| Cutscene/NIS/non-player control | script | script | yes | no |

This matrix rules out both failed fallbacks:

- universal Strafe corrupts speed, action bits, state observers, and contextual
  movement;
- character-relative Run makes camera-relative navigation rotate with mouse and
  has been rejected in live play as severely nauseating.

## Phase 1 conclusions

1. Camera-relative travel must remain the locomotion basis.
2. Retail Move/action 2 must remain the semantic movement request.
3. Retail Strafe assets may be reused, but action 6/state 11 may not be used as
   the universal carrier.
4. The engine needs a distinct mouse/aim heading rather than overloading
   `orientation.y` or `faceAngle` globally.
5. Each consumer must be routed by meaning:
   locomotion/collision, visible/combat, or context-owned.
6. State-owned traversal, interaction, and authored combat turns need an
   explicit ownership signal so input cannot overwrite them.
7. Model rendering, attack collision, combat root motion, and enemy perception
   must agree on the visible/combat heading at the points where it matters.
8. `character_relative` should be removed in the implementation phase.

## Questions Phase 2 must prove

- Where can a second heading be stored and transported without colliding with
  retail state or quick saves?
- Can model/attack evaluation consume mouse heading while physical traversal
  retains retail orientation, or must `orientation.y` remain the combat heading
  with selected physical consumers redirected to locomotion heading?
- What is the smallest set of exact guest hook points that covers a semantic
  boundary rather than enumerating individual action states?
- How can authored strafe animation selection be reused without entering
  `AS_STRAFE`?
- At which transitions should an interaction/action claim and release heading
  ownership, and can that be inferred from existing handler identity rather
  than a brittle state denylist?
- How are stock/off equivalence, quick-save normalization, target suppression,
  and malformed-fingerprint fail-closed behavior preserved?

Phase 2 must answer these with executable/disassembly proof before gameplay
code changes resume.
