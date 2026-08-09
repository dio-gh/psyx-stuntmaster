# Architecture

## Authority boundary

The runtime is a hybrid, BIOS-less PlayStation host.

- **Guest:** R3000A program, RAM, gameplay state, physics, combat, AI,
  animation, collision, scripting, timers, and RNG.
- **Host:** disc access, physical input, window, presentation, audio device,
  configuration, logging, and native movie playback.
- **Bridge in:** sampled PlayStation PAD state derived from remappable SDL
  keyboard/gamepad bindings.
- **Bridge out:** GPU command/state snapshots, scanout images, SPU samples, and
  explicit host presentation commands.

There is one writer for gameplay state: the guest. Presentation interpolation,
frame dropping, audio buffering, and native movie playback cannot feed state
back into the simulation.

## Components

```text
stuntmaster.exe
  application and scheduling
    exact-image validation
    fixed 60 Hz VBlank worker
    input sampling
    immutable presentation mailbox
  guest runtime
    PS-X EXE and overlay execution
    R3000A + GTE
    RAM, scratchpad, MMIO, BIOS HLE
    VBlank, DMA, GPU, SPU, and CD boundaries
  disc
    CUE + raw MODE2/2352 sectors
    ISO9660
    optional drive-rate completion pacing
  GPU
    stateful GP0/GP1 decoder and VRAM
    retained packet/VRAM snapshots -> PsyCross/OpenGL
  audio
    SPU registers and 512 KB sound RAM
    ADPCM voices, ADSR, streaming IRQ, and reverb
    bounded sample ring -> OpenAL or WAV capture
  movies
    caller-gated guest request -> main-thread playback
    raw ISO extents -> FFmpeg STR/MDEC + XA decode
    native RGBA/OpenAL presentation; Start edge skips
```

Generic PS1 infrastructure may be adapted from the pinned SF-pc-port revision
when it fits. Every Stuntmaster HLE boundary, guest address, overlay, asset
format, and host bridge remains game-specific and must be established from the
supported retail image.

## Timing and ownership

The worker owns all guest, HLE, CD, GPU-decoder, and SPU state. It
advances `33,868,800 / vblank_rate` guest instructions per emulated VBlank as
the current bootstrap timing model — 564,480 at the console's 60 Hz refresh, so
the guest CPU keeps console speed at every display rate. A future cycle-aware
scheduler will replace that approximation.

The worker samples the latest atomic PAD value once per VBlank and publishes
complete immutable frames by move through a capacity-two newest-value mailbox.
The main thread owns SDL, OpenGL/PsyCross, and OpenAL servicing. It may discard
obsolete frames or missed presentation slots; it may not skip guest ticks.

Guest execution stays inside `R3000Runtime::runBatch` until a machine boundary:
an HLE/BIOS or diagnostic PC, a claimed MMIO access, a stop/fault, or the exact
instruction budget for the next VBlank/probe deadline. RAM boundaries use a
64 KiB bitmap indexed by normalized word address; non-RAM boundaries are a
small sparse list. The batch path still executes `step()` for every ordinary
instruction, so it changes scheduler crossing cost rather than CPU semantics.

The supported retail image spends most of a heavy frame in
`RenderQueue::WaitForLayer`. A failed poll from `0x8009FDF0` back to itself is
exactly 42 instructions and changes only the loop's private counter and `$v1`.
The retail HLE may charge several whole failed polls at once, but only after it
validates the exact PC/register/object graph, recognizes the live virtual
`CheckLayer` implementation, proves its result is still zero, and sees no
pending GPU completion. It never crosses a VBlank or probe deadline and is
disabled for traces/watch writes. Charged polls advance the same guest counter
and diagnostics, so guest instruction time and scheduler ordering remain
unchanged even though those idle instructions are not individually decoded.

Retail's normal game loop updates at 30 Hz even though VBlank and input sampling
run at 60 Hz. Presentation rate is independent and may be 30-360 Hz. Adjacent
compatible retained frames may receive conservative, screen-space,
presentation-only interpolation.

`--guest-update-rate` is a separate, fingerprint-gated experiment. It selects a
whole `GuestSchedule`: the swap-gate immediate, the emulated display rate, the
CPU budget per refresh, and the guest updates per authored 30 Hz step. Retail's
gate cannot hold a swap for less than one VBlank, so a game loop above the
console's 60 Hz refresh comes from a faster emulated display. Its motion and
time compensation is incomplete, so 30 Hz remains authoritative.

The guest publishes its next `Game::pStateHandler` before the current step
returns. Steady frame-loop handlers engage the requested schedule immediately;
explicit load/transition handlers and unknown values use retail's schedule,
because the loading pipeline assumes two VBlank-driven producer passes per
game-loop consumer pass. GPU upload traffic is not a phase detector: palettes,
RAM texture animations, and pause UI imagery all upload during normal play.
Rates past roughly 100 Hz also need `--guest-cpu-scale` headroom, or the game
loop cannot finish a step per refresh and authored timelines run slow.

Host-injected callbacks execute on a reserved guest interrupt stack and restore
the interrupted CPU state. Guest RAM and device effects remain visible.

GPU completion is counted per linked-list DMA2 ordering-table submission. DMA2
block image uploads are not render-layer submissions. A queued completion is
discarded once retail removes its callback; on the tested route retail polls
DrawSync and does not need an injected completion callback.

## GPU and presentation paths

Both paths consume the same stateful GP0/GP1 decoder and retail command stream,
but they solve presentation at different boundaries.

Both paths finish into one fixed-size host render target. `--render-size`
selects its dimensions; when omitted it defaults to 960x720 original 4:3. The
main thread aspect-fits the completed target into the current SDL window using
nearest-neighbour scaling. Window resize therefore changes only the final blit,
not PGXP projection, guest culling, or guest state.

The debug overlay and notification banner are host presentation layers. The main thread
formats immutable counters and configuration/patch state, rasterizes a small
bitmap-font panel, and composites it into the final window backbuffer after the
internal target has been aspect-fitted. Their scale is therefore independent
of render resolution. Number-row `0` always toggles visibility on a key-down
edge and is removed at the PAD bridge; `--debug-overlay` selects only the
initial state.

### PsyCross path

The default path retains GP0 packets, ordered upload-backed VRAM revisions,
display state, and persistent E1-E5 drawing state. The adapter translates raw
hardware packets into the x64 PsyCross/PGXP primitive layout.

This bridge is intentionally isolated because PsyCross expects high-level
Psy-Q primitive structs rather than a GPU command stream. In particular:

- PS1 XY fields and E5 offsets use signed 11-bit semantics;
- the PGXP build stores positions and variable rectangle sizes as half-floats;
- ordered VRAM revisions must be published between primitive batches;
- screens written into a display page require a framebuffer base composite;
- local opcode normalization must not mutate the retained guest packets.

The path publishes on display flips and, when flips stop during a loading
screen, on a guarded VBlank fallback after the framebuffer changes.

## Audio

Retail drives the SPU through hardware registers and DMA4; it does not call
PsyCross's Psy-Q audio implementation. The portable `Spu` owns register decode,
sound RAM, ADPCM decode, voice state, ADSR, streaming-address interrupts, and
reverb. It produces fixed 44.1 kHz stereo samples on the guest clock.

Samples cross to the main thread through a bounded ring. The consumer either
queues them to OpenAL or a probe writes them to WAV. A full ring drops the
oldest audio so recovery favors current A/V alignment over playing a stale
backlog.

SPU mixing and CD completion are per-second quantities divided across the
emulated VBlank rate, carrying their remainders, so audio stays at 44.1 kHz and
the drive keeps one wall-clock speed at every schedule.

On the title route, native movie presentation starts holding true black at the
post-clamp `FadeEnd` call. The guest fade loop exits without rendering shade
255, and its subsequent front-end teardown can otherwise expose the fully
filled loading page before `Game::PlayMovie`. A second latch at
`Game::PlayMovie` covers every other movie route and keeps suppression active
through setup. At the later verified player boundary, every active foreground
voice continues through the guest OpenAL source until those voices and queued
ring/device samples drain. Only retail's verified stereo streaming-music voices
9 and 10 are excluded; this preserves dry front-end sounds as well as
reverberated gameplay SFX. The guest CPU is blocked at that boundary, so the
main thread may then advance the otherwise worker-owned SPU silently from movie
elapsed time. This models the console's independent audio clock: remaining
voices, envelopes, reverb, and IRQ state age through the FMV instead of freezing
and resuming afterward. Guest register and sound-RAM state remain authoritative.

The preceding title fade remains guest-authored. Its shared helper increments a
private grayscale byte once per render-loop pass rather than reading the master
game clock, so high-frequency mode holds all but one call per authored step
through a second accumulator. Fade timing therefore remains retail-equivalent
before the host takes the movie boundary.

## Movies

The guest remains responsible for deciding which movie plays and for its own
pre/post-movie state. The host captures the requested filename at
`Game::PlayMovie`, then pauses the guest worker only at the verified overlay
player call. The main thread reads the ISO file's original 2352-byte sectors,
decodes MDEC video and XA-ADPCM audio through FFmpeg, and presents them through
the existing window and OpenAL context. A fresh Start press ends playback and
is masked from guest input until released. Headless runs and decoder failures
complete the same caller-gated boundary without playback.

## CD timing

Sector bytes are copied to guest RAM synchronously. Completion uses the
console's 2x pacing by default; `--cd-read-pacing N` selects another drive
multiple and `--no-cd-read-pacing` restores immediate completion. Pacing
exposes a remaining-sector count matching the
loader's polling contract. In requested-60 mode, accelerated rates drain at the
requested speed while uploads are active, switch to console 2x after 30 quiet
publications during a load, and return to the requested speed when the guest
publishes a steady handler or uploads resume. A new read supersedes the
outstanding count. This upload heuristic is local to CD settling and does not
control the guest schedule.

This pacing changes only when retail observes completion; it does not create an
asynchronous host write into guest RAM.

## Widescreen

- Original 4:3 is always available.
- The PsyCross/PGXP path expands the 3D view without stretching authored 2D.
- `--widescreen-cull` separately widens retail's two horizontal visibility
  tests and remains a no-op at 4:3.
- `F8` and the experimental Display menu reversibly toggle the complete patch
  set only at the guest worker's safe boundary and map the selected resolution
  tier
  between 16:9 and 4:3 presets. The worker updates the aspect-dependent patch
  limit first; the main thread then reallocates the presentation target.
- Accepted Display-menu and F7/F8 changes are published to the main thread,
  which persists them to the launcher's INI without blocking a guest boundary.
- The wide cull band follows the internal target's exact aspect.
- The frame renderer's earlier per-block bounding-box clip uses the same band;
  this is what keeps already-resident side geometry from disappearing before
  `Block::Draw`.
- Retail's NCLIP, final polygon outcode, and three-camera-block selection remain
  intact. Earlier bypass experiments admitted unrelated/back-facing wall
  geometry and are now used only as save-normalization fingerprints.
- Lower cull bounds remain near-plane protection and cannot simply be removed.
- HUD/menu anchoring remains authored at 4:3 until each element is understood.

## Quick saves

Quick saves are captured and restored only by the guest worker, between
execution batches. Main-thread F5/F6/F9 edges publish atomic request bits; the
worker consumes them immediately before an owed VBlank, avoiding any request
check in the interpreter loop. A successful load abandons that old boundary so
the restored scheduler is not advanced once more. The versioned payload
contains guest CPU/RAM and every
emulated device plus callback, retiming, pacing, and in-progress GPU publication
state that can affect future execution. The application writes a checksummed
temporary file beside the destination and atomically replaces the final path.

Loading first validates the supported executable fingerprint and all
state-affecting launch settings, then decodes into a separate candidate. The
running machine is mutated only after the complete payload validates. Host
frame and audio queues belong to the abandoned timeline and are discarded.
The persistent memory-card image is an external block device and is not rolled
back by a quick load.

Widescreen culling is presentation state despite its reversible guest-code
patch. It is not a compatibility constraint: the candidate runtime recognizes
either retail words or one coherent older/current cull patch set, normalizes it
to the session's current selection, and only then replaces the running machine.

## Dependency rule

Only the presentation/platform layer may include PsyCross, SDL, OpenGL, or
OpenAL headers. Disc, executable loading, guest runtime, GPU/SPU models, bridge
types, patch definitions, and RE libraries remain portable and testable without
graphics or audio-device dependencies.

The detailed simulation/presentation split is recorded in
`docs/PRESENTATION_TIMING.md`.
