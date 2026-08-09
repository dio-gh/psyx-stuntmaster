# Presentation timing

## Current split

The live runtime has two timing axes and two host threads:

- A worker advances one guest VBlank at the schedule's display rate, 60 Hz by
  default, executing `33,868,800 / vblank_rate` guest instructions between
  refreshes at the current bootstrap rate. It owns guest, HLE, CD, GPU, and
  SPU state and samples one latest-value PAD state per tick.
- The main thread owns SDL, PsyCross/OpenGL, and OpenAL servicing. It presents
  at a selected 30-360 Hz rate.

Complete immutable frames cross a capacity-two `BoundedLatestMailbox`. The main
thread takes the newest value and discards obsolete presentation work. It never
observes packets and VRAM from different guest frames, and the worker never
waits for presentation.

## Invariants

- Guest state has one writer.
- Input is sampled once per guest VBlank from the latest host value.
- Guest timers, callbacks, AI, physics, animation, collision, and RNG are not
  affected by host presentation rate.
- SDL events and the OpenGL context remain on the main thread.
- OpenAL queue servicing remains on the main thread; sample production follows
  the guest clock.
- Presentation consumes immutable values and never mutates guest state.
- Falling behind drops old frames or audio samples, never guest ticks.

## Presentation rate

`--presentation-rate HZ` changes only the host deadline. If no newer guest frame
exists, presentation repeats a cached complete image. Overdue host slots are
dropped rather than accumulated as timing debt.

The PsyCross path may interpolate screen-space vertex positions between
adjacent retained frames. Correspondence requires compatible packet topology,
draw state, texture identity, color, display geometry, and bounded motion. Only
substantial near-rigid world polygons qualify; small, ambiguous, or articulated
geometry stays on the exact current guest frame.

## Guest update rate

Guest update rate is independent from presentation rate. Retail normally runs
its game loop at 30 Hz by holding a queued render swap until two VBlanks have
passed. The gate is `sltiu $v0, $v0, 2` at `0x800A015C` in `VSCallback__Fe`.

`--guest-update-rate HZ` accepts any multiple of 30 up to 240 and selects a
whole `GuestSchedule` (`include/stuntmaster/game/guest_schedule.hpp`):

| Field | Meaning |
| --- | --- |
| `vblank_rate` | The emulated display refresh. |
| `swap_gate_vblanks` | The gate immediate; the game loop is the display rate divided by it. |
| `instructions_per_vblank` | `33,868,800 / vblank_rate`, so the CPU keeps console speed. |
| `retime_divisor` | Guest updates per authored 30 Hz step. |

The gate cannot go below one VBlank, so 60 Hz is the fastest loop a 60 Hz
display can produce. Above that the display rate itself increases and the gate
stays at one. Retail's own cadence is the 30 Hz row: a 60 Hz display with a
two-VBlank gate and nothing to retime.

Two schedules exist at run time. The locked one is always retail's, whatever
was requested, because retail's loader assumes two VBlank-driven producer
passes per game-loop consumer pass. Cadence follows the next
`Game::pStateHandler` published by retail: steady play, pause, title, and end
loops use the requested schedule, while load/transition handlers and unknown
values use the locked schedule. `--eager-high-rate` (`--eager-sixty` is the
original spelling) bypasses that selector, which reproduces the load
corruption.

Retiming is one host-side number in `RetimeState` (the `RetimeHooks` table),
guest RAM stays byte-clean, and every retiming site is a host hook:

- `Step__4Time` advances a private accumulator by one and counts the update
  only when it reaches the divisor. It publishes that decision in the host
  state rather than a guest word.
- The scene-animation, VRAM-flipbook, world-effect, flying-world-effect,
  tutorial-arrow, obstacle-collision-pass, and platform-motion hooks read that
  decision rather than deciding for themselves, so every authored timeline
  stays on one schedule at any divisor.
- The boot executable's fullscreen fades run outside `Game::Step` and keep a
  second accumulator, seeded one short of the divisor so a fade's first call
  always advances.
- `--retime-motion` divides the shared position step and the single gravity
  read by the divisor, rounding half to even. At a divisor of two that is
  bit-identical to the shift-and-carry halving it replaced.

A divisor of one means retail everywhere, so activating the hook table without
a rate cannot fault or change behaviour.

Everything else measured per second follows the schedule rather than the
console constant: SPU mixing takes `44,100 / vblank_rate` frames per VBlank
through a remainder-carrying `RatePacer`, and CD completion drains
`300 * speed / vblank_rate` quarter-sectors the same way.

### Two ceilings

Running the loop `k` times per authored frame is `k` times the CPU work per
wall-clock second. Two separate limits follow, and they pull opposite ways.

**The guest CPU.** One retail game step measured about 316,000 instructions on
the title-to-first-level route, so the console's CPU sustains roughly 107
updates a second. Above that the loop cannot finish a step per refresh,
`WaitForLayer` absorbs the difference, and every authored timeline runs slow —
the guest stays internally consistent and deterministic, but the game plays in
slow motion. `--guest-cpu-scale` raises this ceiling.

**The host.** The guest is interpreted, so the host must deliver
`33,868,800 * cpu_scale` guest instructions every wall-clock second. Measured
at 37.3M against the console's 33.9M — about 1.10x headroom, enough to run a
PlayStation in real time and very little else. `--guest-cpu-scale` *lowers*
this ceiling, in direct proportion: scale 2 asks for 67.7M a second and the
guest runs at roughly half real time.

Overshooting either looks identical from the outside — audio and gameplay slow
together, because both follow the guest's VBlank boundary, which follows the
instruction counter. That is also how to tell the symptom from a retiming
fault: a wrong divisor cannot slow audio, because audio production is per
VBlank and never passes through a retiming patch.

`sustainableGuestUpdateRate(cpu_scale)` reports the first ceiling; the runtime
prints it and warns when the requested rate exceeds it. `--debug-overlay`
reports the measured guest speed as a percentage of the active schedule, which
surfaces both ceilings at once.

**90 Hz is therefore the practical maximum for live play**, at scale 1, until
the guest backend is faster than an interpreter. Higher rates stay accepted and
are correct in headless probes, where nothing has to keep up with a clock.

Measured authored ticks per second on the first-level route, in probes:

| Update rate | CPU scale | Authored ticks/s |
| --- | --- | --- |
| 60 | 1 | 29.9 |
| 90 | 1 | 30.1 |
| 120 | 1 | 26.9 |
| 120 | 2 | 30.1 |
| 150 | 2 | 30.1 |
| 180 | 2 | 29.5 |
| 210 | 2 | 29.5 |
| 240 | 1 | 13.7 |
| 240 | 3 | 29.7 |

The scale column is what the guest CPU budget needs. It is not advice for live
play: any scale above one exceeds the host's throughput on the measured
machine.

The mode remains experimental. Collision response, remaining per-frame
accelerations and counters, slow integer steps, and later campaign transitions
need validation. The default 30 Hz path remains the compatibility reference.

Each retiming flag can also run at 30 Hz as a diagnostic control, where the
host publishes a divisor of two and the flag should halve only the subsystem it
covers.

## Acceptance checks

- `--input-trace` reports the corrected logical PAD masks; presentation rate
  does not change the guest input-sampling rate.
- Guest VBlank count remains approximately 60 per wall-clock second at every
  presentation rate.
- Title and gameplay probes remain deterministic at a fixed guest instruction
  budget.
- A slow presenter discards snapshots without accelerating, stalling, or
  corrupting guest execution.
- Mailbox tests preserve packet/VRAM pairing and newest-value behavior across
  overflow.
- The normal title, lobby, first-level, control, texture, pacing, and pause
  route has been live-validated at 120 Hz.
- Conservative world interpolation has been live-validated at 360 Hz without
  the articulated-model corruption produced by broad interpolation.
- A high-frequency guest claim must compare motion trajectories and
  `theTimeMgr` against the stock 30 Hz wall-clock result, not merely report one
  world frame per VBlank. `theTimeMgr` must advance 30 times a wall-clock
  second at every rate; a lower figure means the loop is CPU-starved, not that
  retiming is wrong.
- Audio must produce 44,100 frames per guest second at every rate. Equal
  `--audio-capture` lengths for equal instruction budgets is the check.

## Remaining timing work

1. Finish and validate the speed-preserving high-frequency guest experiment
   described in `docs/STATUS.md`.
2. Replace the fixed instruction budget per VBlank with cycle-aware CPU/device
   scheduling and explicit IRQ delivery.
3. Keep device scheduling independent from presentation; neither change should
   require loosening the one-writer or immutable-snapshot boundaries.
