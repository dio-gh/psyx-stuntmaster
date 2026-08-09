# Follow-up session handoff

This file is the restart checklist. Current product state and priorities live
in `docs/STATUS.md`; evidence and addresses live in `docs/re/README.md`.

## First 15 minutes

1. Read `docs/STATUS.md`.
2. Read `docs/ARCHITECTURE.md`, especially the authority boundary.
3. Run `git status --short` and preserve any unrelated user changes. At the
   time of this handoff, `start.ps1` has a local modification.
4. Check the latest commits with `git log -10 --oneline` rather than relying on
   old exact probe counters.
5. Build the presentation configuration and run its tests:

   ```bat
   tools\build_windows.cmd
   ```

6. Start the normal live baseline:

   ```bat
   build\windows\RelWithDebInfo\stuntmaster.exe --game "path\to\game.cue" --run --input-config input.example.ini
   ```

7. Continue the first item under `Open work` in `docs/STATUS.md` unless the new
   task explicitly changes priority. At this handoff, that item is longer-route
   compatibility, including later campaign movies. Live validation of guest
   update rates above 60 Hz is also open and needs a human at the window.

Do not read the reverse-engineering notebook front to back before starting.
Search it for the subsystem, symbol, or address being changed.

## User interaction constraint

- Never initialize or use Windows Computer Use, desktop UI automation, or an
  equivalent Windows-control facility for this project.
- Do not ask the user for permission to use it again. Live visual validation is
  user-run; rely on deterministic tests, probes, captures, and logs unless the
  user explicitly supplies the observation.

## Project boundary

- Never commit the retail BIN/CUE, extracted assets, captures, or research
  checkouts.
- Support only the fingerprinted NTSC-U `SLUS-00684` image until another build
  is deliberately mapped.
- Gameplay state remains authoritative in the guest. Host input and
  presentation may cross the boundary; host presentation may not mutate game
  state.
- All retail patches must be fingerprint-gated and reversible.
- Preserve provenance for code adapted from SF-pc-port or PsyCross.
- Keep platform dependencies out of the portable core. PsyCross, SDL, OpenGL,
  and OpenAL belong in the presentation/platform layer.

## Supported image

| Field | Value |
| --- | --- |
| Region | USA / NTSC-U |
| Serial | `SLUS-00684` |
| Format | single-track MODE2/2352 BIN/CUE |
| BIN SHA-256 | `0DFC8FCB055E2EBF22380F5FF7568706376588FDCF8C4086DCFCA67DC8295E14` |
| Boot EXE SHA-256 | `5aae79f0d603bf95bdcf9a2c278d1891dcbd9e835a59d4673d6e6dbd82665c64` |

Inspect or enumerate it with:

```powershell
python tools/stuntkit.py inspect "path/to/game.cue"
python tools/stuntkit.py --json inspect "path/to/game.cue"
python tools/stuntkit.py tree "path/to/game.cue"
```

## Build and smoke tests

The canonical script discovers Visual Studio, uses its CMake integration, and
builds `RelWithDebInfo` by default. Pass `debug` for an unoptimized build. It
runs CTest unless `-SkipTests` is given.

Core-only build:

```bat
tools\build_windows.cmd -CoreOnly
build\windows-core\RelWithDebInfo\stuntmaster.exe --game "path\to\game.cue" --probe-guest
```

Presentation build:

```bat
tools\build_windows.cmd
build\windows\RelWithDebInfo\stuntmaster.exe --game "path\to\game.cue" --probe-guest --guest-budget 150000000 --capture-frame
```

The first presentation build may populate the vcpkg cache. The manifest
baseline is pinned in `vcpkg.json`.

Treat these as stable signals:

- CTest passes;
- the volume and executable fingerprints match;
- a deterministic probe stops on its instruction budget, not a fault;
- the capture or live window contains a recognizable retail frame.

The exact stop PC, primitive count, VBlank count, and callback count are
diagnostics. They change as scheduling and frame publication improve.
`SCREENSHOT.BMP` is now the faithful render-target readback (upright, taken
through the same persistent present the window uses), matching the live window;
the old inverted `PsyX_TakeScreenshot` grab is gone.

## Reproducible routes

Normal live play:

```bat
build\windows\RelWithDebInfo\stuntmaster.exe --game "path\to\game.cue" --run
```

Live play defaults to `input.ini` beside the executable. The build seeds it
from `input.example.ini` only when missing; `--input-config` overrides it.

Original 4:3 presentation:

```bat
... --run --window-size 960x720
```

Headless title-to-level route:

```bat
... --probe-guest --guest-budget 900000000 --publication-trace --script-input 200:start
```

`--script-input VBLANK:button` holds each press for eight VBlanks. Give a
screen time to appear before pressing; an input on its first frame can skip the
content being investigated. Multiple presses are comma-separated.

CD completion uses the console's 2x rate by default. `--cd-read-pacing` keeps
that rate explicitly, an optional integer selects another drive-speed
multiple, and `--no-cd-read-pacing` restores immediate completion:

```bat
... --cd-read-pacing 4
```

In a requested high-frequency mode, rates above 2 run at the selected speed
while uploads are active, fall back to 2x from upload-quiet publication 30
onward during a load, then return to the requested speed when the guest
publishes a steady state or uploads resume. This keeps the load fast while
settling its transition at console cadence. GPU upload activity no longer
selects the guest cadence itself. This changes reference VBlank numbers.
Immediate completion remains available for deterministic comparisons.

Capture the mixed SPU output without an audio device:

```bat
... --probe-guest --guest-budget 400000000 --script-input 900:start --audio-capture out.wav
```

## Diagnostic routing

Start with the narrowest tool that observes the failing boundary:

| Problem | First tool |
| --- | --- |
| Guest boot or device fault | `--probe-guest`, then `docs/re/BOOT_CHAIN.md` |
| PAD mapping | `--input-trace` |
| Guest/presentation cadence | `--timing-trace` |
| Player motion writes | `--motion-trace` |
| Ledge hang dropping on an obstacle | `--ledge-trace` |
| Player state while hanging or falling | `--ledge-watch` |
| Arbitrary guest memory writers | `--watch-writes <begin> <end>` |
| PsyCross publication | `--publication-trace` |
| One published PsyCross frame | `--publication-dump N` |
| Transient PsyCross artifact | `--frame-capture-trace`, then `--replay-capture <dump.GP0>` |
| Headless audio | `--audio-capture <path>` |

`--publication-dump N` writes `PUBLISHED_PSYX.BMP` plus the exact retained
packet stream and VRAM snapshot as `PUBLISHED_PSYX.GP0`. `--frame-trace`
is also valid with `--probe-guest`, so a publication number can be selected
without opening the live presenter.

`--ledge-trace` is diagnostic only and independent of the guest rate, so a
30 Hz run is the control for a 60 Hz reading. It prints one line per ended
hang, naming which `LetGoOfLedge` call site fired and which of
`Obstacle::LedgeCheck`'s four conditions was false on the last verdict before
it. `--ledge-trace-inputs` adds two deeper sample points that also publish the
facing dot with both its operands and the humanoid's state and vertical
velocity. See the ledge section of `docs/re/README.md`.

The two levels exist so the trace can be bisected against a behaviour change.
Keep new sample points in the `inputs` set until a played session says the base
set and the extended set behave the same.

A trampoline may only use scratch its host function already destroys. The MIPS
ABI says a caller must not rely on `$t0`-`$t9` across a call, but retail does
in places: stock `Obstacle::LedgeCheck` never writes `$t3` at all, so a `$t3`
value that survives a stock call stops surviving a patched one, and a sample
point built on that broke ledge attachment outright while every executable test
passed. `$at` is the only register that is genuinely free. Pick scratch the
host function overwrites before reading, and cover it with a stock-versus-
patched comparison of the whole register file, as
`ledgeTraceDoesNotChangeWhatLedgeCheckDecides` does.

`--ledge-watch` is host-only: it reads `thePlayer` and prints position,
velocity, carried velocity, mass, gravity, yaw and flags once per frame while
the player is in the ledge hang or pull-up, plus every state change. It patches
no guest code, so unlike a sampling trampoline it cannot change what it
measures. Prefer it to a new trampoline when the question is "what are the
values".

`--frame-capture-trace` retains the latest 48 distinct frames and dumps the
ring on L3 (`F` in `input.example.ini`). It uses a control the digital guest
pad cannot see, so capture does not steal a gameplay button.

`--capture-frame` is not a faithful check of live PsyCross publication. It
creates a fresh presenter at the end of the run without the live ordered
segments. Use live observation or a captured/replayed frame until a capture
inside the PsyCross publication block exists.

## High-value implementation constraints

### Timing and callbacks

- The guest worker advances one VBlank at the schedule's display rate, 60 Hz by
  default, executing `33,868,800 / vblank_rate` instructions between refreshes.
  Retail's game loop normally updates every second VBlank.
- Anything measured per second — SPU frames, CD sector completion — divides
  across the emulated VBlank rate through a remainder-carrying `RatePacer`, not
  a constant per VBlank.
- Host-injected callbacks must use `R3000Runtime::beginInterruptCall`, not
  `beginCall`. The reserved interrupt stack prevents overflow of retail's 1 KB
  scratchpad game-step stack.
- GPU completions are owed per linked-list DMA2 ordering table. Block DMA2
  image uploads do not owe DrawSync completions.
- Retire one completion at a time. Discard it if retail removed the callback.
  On the tested route retail polls DrawSync, so zero injected GPU callbacks is
  expected.
- Validate `s2` against the live render-layer count before using it as a
  `WaitForLayer` index.

### Retail patches and high-frequency mode

- `--guest-update-rate N` is experimental and takes any multiple of 30 through
  240. It selects a whole `GuestSchedule`
  (`include/stuntmaster/game/guest_schedule.hpp`): swap-gate immediate,
  emulated display rate, CPU instructions per VBlank, and guest updates per
  authored 30 Hz step. Read that header before changing anything about guest
  cadence.
- The gate's minimum is one VBlank, so a game loop above the console's 60 Hz
  refresh comes from a faster emulated display, not a shorter gate.
- The host reads `theGameMgr` at `0x800DD668`, then its `Game::pStateHandler`
  field at `+0x1c`. Steady frame-loop handlers (including play and pause) use
  the requested schedule immediately. Explicit load/transition handlers and
  unknown values fail closed to retail's complete schedule, because level
  loading assumes two VBlank task-list passes per game-loop pass.
- Do not restore GPU-upload activity as the cadence signal. Gameplay world
  effects upload animated CLUT rows, RAM textures upload flipbook frames, and
  the pause UI uploads imagery; all are legitimate at high cadence.
- `--eager-high-rate` (`--eager-sixty` is the original spelling) intentionally
  violates that constraint and reproduces load corruption.
- Two ceilings, easy to confuse. A retail game step costs about 316,000
  instructions, so the guest CPU sustains roughly 107 updates a second; past
  that the loop cannot finish a step per refresh and the simulation runs slow
  at a correct schedule. `--guest-cpu-scale` raises that ceiling and demands
  the same multiple of real-time guest execution. Boundary batching plus exact
  fast-forward of verified 42-instruction `WaitForLayer` polls now measures
  3.35x over the untouched heavy probe (142.9M guest-equivalent instructions/s,
  with identical legacy diagnostics). Scale two has headless margin on the
  tested gameplay saves and is now live-confirmed at full speed on the tested
  route. Higher update rates still need their own live validation.
- `R3000Runtime::runBatch` may only cross ordinary guest PCs. It yields before
  HLE/BIOS/diagnostic boundaries, after claimed MMIO, and exactly at the next
  VBlank/probe budget. `RetailHle::fastForwardWaitForLayerPolls` is additionally
  gated by the exact retail PC/register/object graph, a zero virtual
  `CheckLayer` result, no owed GPU completion, and disabled traces/watch writes.
  Preserve every one of those guards if this path changes.
- Before investigating any "retiming is wrong" report, check whether audio is
  slow too. Audio production is per guest VBlank and never passes through a
  retiming patch, so slow audio means a throughput shortfall, not a timing bug.
  `--debug-overlay` reports measured guest speed as a percentage.
- Retiming is one host-side number in `RetimeState` (`RetimeHooks`), not a
  guest arena word. Only `Step__4Time` accumulates against it; every other
  retimed subsystem reads the hold decision it publishes. Add new retimed
  subsystems as readers, never as second accumulators. Guest RAM stays
  byte-clean — no host write ever lands in executable guest memory.
- A divisor of one means retail everywhere. Applying `--retime-motion`/
  `--retime-clock` without a rate arms the runtime 60 Hz capability but leaves
  the hook table inactive at retail 30 Hz. F7 or the Display menu can then
  request 60 Hz without `--guest-update-rate 60` at launch.
- `--retime-motion` covers the shared position integrator and the single
  gravity read; both divide with half-to-even rounding, bit-identical to the
  old halving at a divisor of two. `--retime-clock` covers the master clock,
  general scene-animation lists, VRAM texture flipbooks, both `WEffect::Update`
  and the overriding `FWEffect::Update`, the tutorial arrow's overlay-local
  counter, path-driven `Platform::Move` (including level-one traffic), the
  boot executable's shared private fullscreen-fade counter, and the obstacle
  collision/carry pass (gated to the authored rate, except an active pusher's
  inner collision runs every update to preserve its contact state). The fade
  needs its own accumulator because its four render loops do not call
  `Step__4Time`.
- When retiming a field, search the boot executable and all overlays for every
  reader and writer. Hook the singular side when possible.
- Overlay load addresses are reused. Overlay hooks carry a fingerprint window
  (embedded in `retime_hooks.cpp`) and are gated per loaded overlay by
  `buildActiveRetimeHooks`.
- Obstacle object classes own their whole authored timeline in the `Think`
  dispatched from `AI::privMoveList` — motion, gravity, local frame counters,
  and the delta they carry riders by. Retime one by holding its `Think`
  prologue (a `RetimeHeldPrologue` hook), not by patching individual fields
  inside it. Their `HandleHumanoidCollision` comes from the collision manager
  rather than from `Think`, so holding `Think` does not disturb collision or
  ledge tickets. Every obstacle class is covered by held prologues in
  `retimeObjectHooks()` (always-live boot ones) and `retimeOverlayHooks()`
  (the OL1/NBOL ones, fingerprint-gated). Add a class by appending a
  descriptor, not by writing another body. The exceptions are `Platform`
  (retimed by divide-based sub-step hooks — `Think` runs every update) and
  `Stack`, whose `Think` re-poses a display model every call: a whole-`Think`
  hold rubberbands it back to its rest pose on held updates, so it is retimed
  by two narrow control-flow gates (`retimedStackTimeline`,
  `retimedStackSound`) that skip only its `Wobble`/`Fall` timeline and tail
  sound `Think` on a held update while the pose-apply keeps running. See the
  `Stack` and `Platform` sections in `docs/re/README.md` before touching them.
- A held-prologue hook restores the callee-saved registers the prologue has
  already stored, unwinds the frame, and returns to the caller on a held
  update; on a counted update it models the displaced instruction and resumes
  at the rejoin. The displaced form decoder handles the `move`/`addu` and `lw`
  forms these prologues use; a site that needs a new form fails its test rather
  than being silently mis-patched.
- A hook test that only counts calls proves almost nothing. Compare the whole
  register file between a stock run and a hook run at the instruction where the
  hook rejoins retail, `$at` excluded.
- `GAME_REL.SYM`'s overlay column is not authoritative. It tags the `Pendulum`
  module as overlay 0 although its code is in `NBOL_REL.BIN`. Decide an
  address's image from the extracted bytes and the load-address windows in
  `docs/re/README.md`, not from the symbol table.
- The overlay files behind the SYM overlay ids are tabulated in
  `docs/re/README.md`. Confirm a new fingerprint window is unique across all
  four `*_REL.BIN` files before adding an overlay hook.
- `retime_hooks_live=` on stderr and `HOOK n OF m` in `--debug-overlay`
  report which retime hooks are live right now.

### Rendering

- The PsyCross adapter receives raw GP0 hardware packets, not native Psy-Q
  structs. PS1 XY/E5 values are signed 11-bit; the x64 PGXP primitive fields,
  including variable rectangle sizes, use half-floats.
- GP0 E1-E5 state persists across frame intervals.
- `--render-size` fixes the internal presentation target independently of the
  SDL window. PGXP projection and widescreen culling use the render aspect;
  window resize changes only the final nearest-neighbour, aspect-fitted blit.
- The Display menu has four paired 4:3/16:9 tiers through 1440p. F8 uses the
  same paired resolution switch as its Widescreen row. Accepted Display-menu,
  F7, and F8 changes are persisted on the main thread to `stuntmaster.ini`.
- The debug overlay is presentation-only. It draws a host-owned bitmap after
  the guest frame is aspect-fitted and reports cadence, counters, renderer, and
  active timing/retail patches without mutating guest state. Number-row `0`
  always toggles visibility and is masked from the guest PAD;
  `--debug-overlay` only makes it visible initially. Notifications share this
  final window-space layer, so render resolution cannot scale either one.
- Interpolated polygons must share one previous position at a shared current
  vertex. Moving a matched polygon beside an unmatched one opens a transient
  crack unless that edge motion is propagated to both packets.
- The PsyCross path needs ordered VRAM revisions because retail uploads new
  textures between primitive batches. It composites the selected display page
  for low-polygon screens retail writes rather than draws, but suppresses that
  page under a substantial 3D world pass so upscale cracks reveal black rather
  than bright stale framebuffer contents.
- Its stopped-flip load fallback may publish only with a pending VBlank and
  must copy, not consume, the interval's ordered replay segments. Packets after
  the last draw need a tail segment paired with the final VRAM revision. This
  keeps every lossy-mailbox value self-contained and prevents partially loaded
  texture state from flooding live presentation.
- Keep both local PsyCross fixes in
  `external/PsyCross/src/render/PsyX_render.cpp`: sampler uniforms must update
  when the shader changes, and each alternating VAO must bind its matching VBO.

### CD and audio

- CD bytes arrive synchronously. Pacing delays only the remaining-sector count
  polled by retail. A fresh `CdRead` supersedes the outstanding count; adding
  reads creates an unbounded backlog.
- Retail programs SPU registers directly. The host owns the register file,
  sound RAM, DMA4 transfer, voice mixer, streaming interrupt, reverb, and
  OpenAL output. Do not route it through PsyCross's Psy-Q `Spu*` API.
- On the title Start route, begin holding black at the post-clamp `FadeEnd`
  instruction `0x8002C178`; the fade loop never renders its final 255 shade,
  and front-end teardown can expose the loading page before `Game::PlayMovie`.
  Keep the `Game::PlayMovie` (`0x8002BBF0`) latch as the fallback for all movie
  routes. At the later caller-gated player boundary, audibly drain every active foreground
  voice plus guest OpenAL/sample-ring queues before starting movie XA audio.
  Exclude only verified stereo streaming-music voices 9 and 10; front-end SFX
  can be dry. Then advance remaining SPU state silently from movie elapsed time.
  The guest worker is blocked throughout native playback, so ownership remains
  exclusive and sounds neither get cut nor resume mid-sample after the movie.
- The title-to-intro fade helper at `0x8002C9BC` adds 17 to its grayscale byte
  per private render-loop pass and reaches the movie caller in 15 passes. It is
  independent of `theTimeMgr`; `retimedFullscreenFade()` uses a private
  accumulator at `0x80003384`, reset on grayscale zero and seeded one short of
  the divisor, to preserve its retail wall-clock duration at any guest update
  rate.

## Static reverse engineering

The boot executable and gameplay overlays use different loaders:

```powershell
python tools/disassemble.py .research/SLUS_006.84 0x8002D830 --bytes 0x68
python tools/stuntkit.py extract "path/to/game.cue" OL2_REL.BIN work/overlays/OL2_REL.BIN
python tools/disassemble.py work/overlays/OL2_REL.BIN 0x800184A8 --load-address 0x80010000
python tools/find_mips_xrefs.py work/overlays/OL2_REL.BIN 0x800DD808 --load-address 0x80010000
```

Gameplay object logic usually lives in the four `*_REL.BIN` overlays. They
share the boot executable's `$gp`, so gp-relative global scans remain valid.
Keep extractions under ignored `work/`.

## Upstreams

```text
SF-pc-port  d9522cda2f785f6f3cb3243a57b12b371528faab
PsyCross    e56e4cde1c2b8a15e0d4e38b26cdd9202e0d17e6
```

`.research/SF-pc-port` is an ignored reference checkout.
`external/PsyCross` is the tracked submodule. Project-specific adapter code
stays outside the submodule.

## End-of-session update

Before handing off again:

1. Put durable current facts and priorities in `docs/STATUS.md`.
2. Put addresses, measurements, failed hypotheses worth preserving, and
   confidence labels in `docs/re/README.md` or a focused RE note.
3. Update this file only when the restart procedure or a high-value constraint
   changes.
4. Add or update a deterministic test/probe for material behavior changes.
5. Run `git status --short`, note user-owned changes, and record the exact
   verification performed.
6. Prefer a descriptive Git commit over appending a chronological anecdote to
   a status document.
