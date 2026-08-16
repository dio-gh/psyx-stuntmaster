# Project status

Last updated: 2026-08-11.

## Snapshot

The first executable milestone is working for the supported NTSC-U image. The
host validates and boots the retail game without a PlayStation BIOS, reaches
the title and first playable level, accepts keyboard and gamepad input, renders
through either presentation path, and plays retail SFX and streaming music.

The project is now in compatibility and fidelity work. Native FMV playback is
working for the verified startup sequence. The largest experimental area is a
speed-preserving high-frequency guest update, now tunable to any multiple of
30 Hz through 240; the retail 30 Hz mode remains the default.

## Milestone status

| Area | Status | Notes |
| --- | --- | --- |
| Disc and executable | Working | Exact `SLUS-00684` BIN/CUE and boot executable fingerprints are enforced. |
| CPU, GTE, BIOS HLE | Working on tested route | Retail startup, overlays, title, and early gameplay execute. Unsupported services are added when a trace reaches them. |
| CD and DMA | Working on tested route | Raw MODE2/2352 reads, ISO9660, DMA2/4/6, and optional drive-rate completion pacing are implemented. |
| Input | Working; dual-heading mouse control awaits campaign validation | Remappable SDL keyboard/gamepad input reaches the retail PAD buffer. Semantic mouse actions preserve the live retail layout and contextual combos. Camera-relative travel remains full-speed Run while Stand, ambient idle, Run, Jump, Fall, and launcher Flip lease official body heading to a bounded mouse-turn controller; twelve ownership and consumer seams preserve travel physics plus push, traversal, combat, and explicit Strafe contexts. Expected lease and retail-owned splits are console-only, while a persistent split in an explicitly committed state produces an on-screen diagnostic. Gameplay/focus capture remains exact. The host reports port one as a DualShock so retail vibration gates open, and the game's motor values drive `SDL_GameControllerRumble` on the first attached gamepad. |
| PsyCross presentation | Default, working | Title, first level, textures, HUD, pause overlay, widescreen view, and high-rate presentation have been live-validated. |
| Audio | Working | Register-level SPU, DMA sample upload, ADPCM voices, streaming music interrupts, ADSR, reverb, OpenAL output, WAV capture, and native-movie audio hand-off are implemented. |
| FMV | Working on tested route | Homegrown Wuffs-generated codecs decode the shipped raw-disc STR/MDEC v2 video and stereo 4-bit XA-ADPCM on the main thread. All nine movie assets pass disc-backed decode validation. Start skips the active movie; movie hand-offs suppress stale splash frames and have been live-validated; headless and decode-failure paths retain the caller-gated skip. |
| Saves | Working | Retail slot-one flows use a persistent standard 128 KiB memory-card image. F5/F9 persist a versioned full-machine quick save; F6 writes timestamped states; `--load-quick-save` restores a selected file at launch. Slot two remains disconnected. |
| Full campaign | Not validated | First 2 chapters validated and reported completable. Longer levels and later transitions need coverage. |
| Native launcher | Working | The dependency-free Win32 launcher selects the BIN/CUE folder, resolution tier, 60 Hz mode, and widescreen mode. It persists `stuntmaster.ini`; accepted Display-menu, F7, and F8 changes update the same file. Direct game launches consume those defaults and explicit command-line options take precedence. |

The acceptance criteria in `docs/REQUIREMENTS.md` are met. Audio and native
FMV playback were beyond that milestone and have since been added.

## Current defaults and experiments

| Setting | Default | Current guidance |
| --- | --- | --- |
| Guest update | 30 Hz | Stable retail cadence. Keep this as the compatibility baseline. `--guest-update-rate` takes any multiple of 30 up to 240; 90 is the practical live maximum. |
| Presentation | Host display refresh, 1280x720 window, 960x720 4:3 render target | `--presentation-rate 30..360` overrides the detected host refresh. `--render-size` fixes another internal resolution independently of the resizable window. Explicit initial widescreen defaults to 1280x720. |
| Debug overlay | Available, initially hidden | Number-row `0` always toggles the panel without reaching the guest PAD; `--debug-overlay` makes it visible initially. Debug and notification layers are window-space, so render resolution does not scale them. |
| Input config | `input.ini` beside the executable | The build seeds it from `input.example.ini` only when missing. `--input-config` overrides the path, and later builds preserve local binding edits. |
| Renderer | PsyCross | The release baseline and only presentation path. Headless publication tests use the same presenter. |
| CD completion | Console 2x pacing | `--cd-read-pacing N` selects another drive multiple and `--no-cd-read-pacing` restores immediate completion. In a requested high-frequency mode, rates above 2 fall back to console 2x during the final upload-quiet settling window. The drive rate is wall-clock and does not change with the guest rate. |
| Widescreen culling | Off | `--widescreen-cull` selects the initial PsyCross/PGXP cull state; F8 toggles it and switches to the paired 4:3/16:9 render target, matching the Display menu. The aspect-exact horizontal band is applied to polygon, model, and early whole-block visibility tests. Retail NCLIP, final-outcode, and three-block selection remain intact. It remains a no-op at 4:3. |
| Framebuffer composite | On | Required by the PsyCross path for screens retail writes into the framebuffer. |
| FMV | Enabled in live play | Start skips on a fresh press without leaking the held press into guest input. Headless probes skip at the verified boundary. |
| Memory card | `saves\SLUS-00684.mcr` | Created and formatted on first use. `--memory-card` selects another standard raw image. |
| Quick save | F5 / F6 / F9 | F5 atomically replaces `saves\quick-save.stsm`; F6 writes a timestamped sibling; F9 reloads the default. Files restore guest CPU/RAM, devices, callbacks, and scheduler state; stale host frame/audio queues are discarded. A timed top-right message reports success or failure. |
| Runtime cadence | F7 | With both retiming families armed, toggles between retimed 60 Hz and exact retail 30 Hz even when the launch starts at 30. The hooks remain inactive at 30 Hz. Load/transition handlers remain retail-gated. |
| Photo mode / free camera | F11 / controller Select | Experimental gameplay-only photo mode with mouse/WASD/Q/E or dual-stick flight plus L2/R2 vertical control. It begins frozen; P/R3 resumes or freezes simulation without ending free camera. Host execution gates hold retail time, world/physics, animation/effects, and score updates while the retail camera and draw pipeline continue. Retail HUD drawing is suppressed until photo mode exits, even if the simulation is resumed. Loads, cutscenes, and retail mode changes take ownership automatically. Live validation is pending. |
| Gameplay mouse mode | F10 | Toggles a dual-heading camera-relative mode and stock/off. Full-speed Run and Move remain retail while directional animations follow mouse-facing. A bounded velocity/acceleration controller stabilizes turning. Left Punch and right Kick are defaults; all eight primary actions remain semantically bindable. Six ownership-aware guest seams and their transitions are emulator-tested; live campaign validation is pending. |
| Retail host menu | On | The host adds `Options > Display`, with 30/60 Hz, widescreen on/off, and four render-size choices through 1440p; `--no-experimental-host-menu` disables it. The widescreen row switches both the reversible cull patch and the internal target between paired 16:9/4:3 resolution families while preserving the selected tier. Accepted changes persist to `stuntmaster.ini`. It overlays supported-image CD reads in memory; the source disc is not changed. The hidden logical menu ID is the unique `HostDSP`; no compatibility path exists for experimental saves containing earlier menu IDs. The three-row submenu route is covered headlessly and still needs a live visual retest. |

The following flags are diagnostic or experimental, not recommended defaults:

- `--experimental-host-menu` installs a fingerprint-gated guest/host callback
  bridge in the front-end Options menu. Its frame-rate selector requests the
  existing safe-boundary F7 path, so 60 Hz requires `--retime-motion
  --retime-clock`; `--guest-update-rate 60` is no longer required to enable it
  at runtime. Its resolution
  selector changes only the host render target. Its widescreen selector uses
  the same safe-boundary reversible patch path as F8 and also changes the
  target and repopulates the Resolution row with the paired 16:9 or 4:3
  family. F8 follows the same paired target and patch path.

- Widescreen culling is a runtime presentation setting. `--widescreen-cull`
  selects its initial state and F8 toggles it on the guest worker. Quick saves
  made with either state load in either current state: the decoded candidate's
  coherent old/current cull patch set is normalized before restoration rather
  than treated as a launch-compatibility mismatch.

- `--guest-update-rate N` selects a whole schedule. The host reads retail's
  published `Game::pStateHandler`: steady loops such as play and pause use the
  requested cadence, while explicit load and transition handlers use retail's
  full schedule. `--eager-high-rate` (`--eager-sixty` is the original spelling)
  deliberately enables the requested cadence during loads and reproduces
  texture corruption caused by violating retail's 2:1 loader producer/consumer
  timing.
- `--guest-cpu-scale N` raises the guest CPU budget a high rate needs, but
  demands the same multiple of real-time guest execution. The optimized heavy
  headless routes have scale-two margin, and scale two is now live-confirmed at
  full speed on the tested gameplay route. The runtime prints the sustainable
  update rate and warns when the requested rate exceeds it.
- `--retime-motion` and `--retime-clock` now run through the host-owned
  `RetimeHooks` table (guest RAM stays byte-clean); the in-RAM byte retiming
  trampolines are deleted.
- `--retime-motion` divides shared position integration, gravity, and the
  humanoid turn-rate limit by the schedule's divisor, and spends the
  carried-velocity decay — the momentum a
  thing keeps from something it rides, hangs from, or is pushed by — once per
  authored step rather than once per guest update.
- `--retime-clock` retimes the master game clock, general scene-animation
  lists, VRAM texture flipbooks, base and overriding moving-world-effect
  updates, the overlay-local tutorial arrow counter, path-driven
  `Platform::Move`, the phase-partitioned detached-platform fall, the
  held-step preservation of a bobbing platform's already-offset Y pose, the
  per-update `Think` of every obstacle class (`Stack`
  through two narrower gates rather than a whole-`Think` hold, because its
  `Think` re-poses a display model every call), the ledge-latch velocity reset,
  and the ledge ticket-loss decision.
  It also retimes the boot executable's private fullscreen fade counter, which
  does not read the master clock. It retimes animation, transition fades,
  timeouts, frame-counted state, the first-level traffic path, and the
  obstacle motion the player rides, hangs from, or mantles onto. The obstacle
  pass normally remains authored-rate gated, but services active pushers,
  runners on dynamic obstacles, and airborne jump/fall humanoids on held
  updates, plus every ladder state whose one-update contact bit cannot wait.
  Fire contact is re-issued for every burning humanoid on held updates; the
  complete list scan prevents an already-burning player from starving a later
  NPC while leaving the pit's damage tick at authored cadence. The title
  screen also publishes its own master hold decision immediately before the
  shared menu-colour step, keeping PRESS START at the authored blink rate.
  The Pushable displacement and Conveyor's direct player carry remain gated
  to authored ticks inside those exceptions, preventing 2x movement while
  pushing or running on a belt.
  Jumpers with a live passenger ticket
  disembark before the platform carries them again; ticketless jumpers still
  receive the sub-step collision needed to land on thin dynamic geometry
  instead of crossing it between authored passes. This includes the
  `AS_Fall`/`AS_HardFall` descent after the initial jump state; omitting those
  states caused phase-dependent tunneling through pushables. The ladder's
  explicit upward animation advances only on authored ticks, and its direct
  downward position step is divided for smooth retail-speed descent. The jump
  exception prevents a sinking or teetering platform from carrying a player
  once more after takeoff and shortening the jump.
- Live-confirmed at 60 Hz after the object retiming landed: the level-three
  moving car you ride, and the level-three in-engine cutscene car, whose timing
  now matches retail. Obstacle ledge hangs work on ordinary dynamic obstacles.
  The car-lift `Platform` now sub-steps its teeter motion instead of moving a
  whole authored tilt at once; its jump-off feel and the new smooth/exact
  detached-fall partition still need live comparison against retail.
  The `vibrating.stsm` buoyant-platform probe is also fixed: held updates no
  longer replace the platform's bobbed Y with its path-base Y. The rider's
  former `-387/-412` alternation is reduced to the expected slow bob with at
  most a two-unit standing settle between authored collision passes.
  The follow-up `runleft.stsm` probe no longer alternates `AS_Run`/`AS_Fall` or
  accumulates grounded gravity while crossing the platform: Y follows the bob
  with only the two-unit contact probe, persistent vertical velocity stays
  zero, and walking off the edge begins `AS_Fall` with the ordinary first `-9`
  gravity sub-step. `metro-runforward.stsm` exposed why the contact probe must
  remain: removing it left a coplanar ticketless runner unable to reboard a
  turning metro car. The probe is now spent on position, then cleared before it
  can accumulate as velocity; the early sideways departure is gone.
- `boss-attack1.stsm` exposed a separate BOL-local authored-frame counter in
  `Butch::_Stomp`: animation 0x42 was already gated to 30 authored ticks per
  second, while `Butch+0x268` advanced on every 60 Hz `Think` and fired the
  shake/effect/sounds/damage at counter 42 halfway through the jump. The
  fingerprint-gated hook at `0x8001ADD8` now advances only that counter on the
  master clock's counted update, leaving boss movement and collision live. The
  deterministic damage transition moved from update 78 to 119, exactly 41
  held updates later; live confirmation of the visual landing sync is pending.
- `--ledge-trace` publishes counters and the last `Obstacle::LedgeCheck`
  verdict into the patch arena and prints one line per ended ledge hang. It
  changes no guest behaviour and is independent of the guest update rate, so a
  retail-rate run is the control.
- `--no-framebuffer-composite` exists only for A/B testing the PsyCross path.
- `--cd-read-pacing N` with a rate other than 2 intentionally changes bulk
  load timing; its console-speed upload-quiet tail protects the transition.

## Verified facts that constrain future work

- The guest is the only authority for gameplay, AI, physics, animation,
  collision, timers, scripting, and RNG. Host interpolation never feeds back
  into guest state.
- The game links its own Sony libpad copy whose detection state machine runs
  in the SIO interrupt handler. The host emulates neither the SIO port nor its
  interrupts, so the host publishes the DualShock struct state (state 6,
  `PadGetState(0) == 6`) and capability fields at every VBlank instead. The
  game's motor table (`0x800DD6AC` big, `0x800DD6AD` small, countdown
  `0x800DC9D8`) is read per VBlank and applied with `SDL_GameControllerRumble`
  on the main thread; no SDL haptics subsystem is required. See the vibration
  section of `docs/re/README.md`.
- Retail updates gameplay at 30 Hz by holding a render-queue swap for two
  VBlanks. This is a fixed-step engine; it has no global time-step scalar.
- The game loop is the emulated display rate divided by that swap gate, and the
  gate's minimum is one VBlank. A loop faster than the console's 60 Hz refresh
  therefore requires a faster emulated display, not a shorter gate.
- The guest CPU keeps console speed at every schedule: `33,868,800 /
  vblank_rate` instructions per emulated VBlank. One retail game step measured
  about 316,000 of them on the first-level route, so the console CPU sustains
  roughly 107 updates a second. Past that the loop cannot finish a step per
  refresh and authored timelines run slow at an otherwise correct schedule.
- The host uses a boundary-batched native x64 recompiler plus an exact fast-forward
  for retail's side-effect-free `WaitForLayer` polls. On the supplied heavy save,
  an untouched 100M-instruction probe averaged 2.345 s (42.6M/s), while the
  optimized probe averaged 0.700 s (142.9M guest-equivalent instructions/s),
  a 3.35x gain. About 65.1M instructions were verified idle polls. Ordinary
  RAM code is translated first into cached blocks of predecoded operations;
  hot regions then lower common ALU operations and guarded RAM loads/stores to
  W^X native code with two-register allocation and precise side exits. Three 20M probes
  from distinct gameplay saves retained all legacy diagnostics and measured
  2.41x-2.86x. Scale two
  therefore has headless CPU margin on these routes, but presentation and
  non-idle-heavy routes still require measurement. Scale two is live-confirmed
  at full speed on the tested gameplay route. Audio and gameplay slow together
  on a real throughput shortfall, because both follow the guest VBlank
  instruction boundary; a retiming fault cannot slow audio.
- **90 Hz remains the live-validated maximum**, at `--guest-cpu-scale 1`.
  Higher rates remain accepted and are correct in headless probes, where
  nothing has to keep up with a clock. Measured authored ticks a second in
  probes: 29.9 at 60 Hz and 30.1 at 90 Hz at scale one, 30.1 at 120 Hz at scale
  two, and 29.7 at 240 Hz at scale three.
- The debug overlay reports measured guest speed as a percentage of the active
  schedule, which is the fastest way to tell a throughput shortfall from a
  timing bug.
- Retiming is one published number, held host-side in `RetimeState` (no guest
  arena words). The host programs guest updates per authored 30 Hz step;
  `Step__4Time` accumulates against it and publishes a hold decision that every
  other retimed subsystem reads instead of deciding for itself. A divisor of
  one means retail everywhere. Guest RAM stays byte-clean; only the swap gate
  and the non-retime patches remain byte patches.
- Per-second device quantities follow the emulated VBlank rate and carry their
  remainders: SPU mixing produces exactly 44,100 frames per guest second and CD
  completion drains one wall-clock drive speed at every schedule.
- Guest execution and input sampling run on a worker at the schedule's display
  rate, 60 Hz by default. SDL, OpenGL, OpenAL servicing, and presentation
  remain on the main thread.
- Retained frames cross threads as immutable values through a capacity-two
  newest-value mailbox. Dropping presentation work cannot drop guest ticks.
- GPU completion accounting is per linked-list ordering-table submission, not
  per DMA2 transfer. Image uploads also use DMA2. A completion is discarded if
  retail has already removed its callback.
- Retail polls DrawSync on the tested title/gameplay route; injected asynchronous
  GPU callbacks on that route were spurious and corrupted render-queue state.
- Host-injected callbacks use a reserved guest interrupt stack. The retail game
  step uses the 1 KB hardware scratchpad and cannot safely host nested callbacks.
- Retail writes some screens rather than drawing them. PsyCross therefore needs
  ordered VRAM revisions and a framebuffer base composite.
- When retail stops flipping during a load, the PsyCross upload fallback is
  bounded to one publication per pending VBlank. Each mailbox value copies the
  interval's settled VRAM segments and adds a final-revision tail for uploads
  after the last draw, so a dropped value cannot discard ordering or attach an
  upload to stale texture memory. On the deterministic 900M-instruction 60 Hz
  title route, 3,929 uploads now produce zero segment mismatches (previously
  3,935), with 1,336 publications across 1,594 VBlanks instead of thousands of
  partial publications inside individual load VBlanks.
- Presentation renders into a fixed internal target selected by
  `--render-size`, then aspect-fits that completed image into the live window
  with nearest-neighbour scaling. Window resize does not change PGXP projection
  or guest-side widescreen culling; both use the internal target's aspect.
- Widescreen mode applies the aspect-exact screen-space band to polygon, model,
  and early whole-block bounding-box tests. The missing storefront in
  `retime-ws-missing-tile-left.stsm` was the last of those tests, not streaming
  or polygon winding; its exact continuation retains 470 primitives and fills
  the left wedge before movement. Experimental NCLIP/final-outcode and
  active-list bypasses are recognized only to remove them from saved states:
  they admitted back-facing wall geometry in `pokey-wall.stsm`. The corrected
  continuation retains 896 primitives and renders the wall cleanly.
- The debug overlay is generated from immutable host-side diagnostic values
  and composited with notifications into the final window backbuffer. It reports the
  requested rate, whether it is in force, and the schedule that produced the
  displayed frame. Internal render resolution cannot scale either host layer.
  Its bitmap font and patch labels have no guest-memory or guest-input path.
- CD data is copied synchronously. Optional pacing drains the completion count
  retail polls at the selected bulk rate. In requested-60 mode, accelerated
  pacing falls back to console 2x after 30 upload-quiet publications and
  returns to the selected rate when the guest publishes a steady handler or
  uploads resume. A new `CdRead` supersedes the outstanding count. GPU upload
  traffic affects only this CD settling tail, never the guest cadence.
- Retail audio uses SPU registers and DMA directly rather than PsyCross's Psy-Q
  audio API. Gameplay SFX are reverberated and streaming music is dry; front-end
  sounds may also be dry.
- Native movie hand-off holds black and keeps mixing every active foreground
  voice until those voices and their queued output finish. The verified stereo
  streaming-music voices 9 and 10 are excluded, so a dry Start/menu sound is
  not mistaken for music. Movie XA audio starts afterward. Because the SPU is
  independent hardware on the console,
  remaining voices, ADSR, reverb, and IRQ state then advance silently at 44.1
  kHz while the caller-gated guest CPU is paused; pre-movie sounds cannot resume
  from a frozen point after the FMV.
- The title/start fade is a private render-loop counter, not a `theTimeMgr`
  consumer. Retail adds 17 per 30 Hz loop pass; `--retime-clock` advances it on
  the first call of each authored step and holds the rest, preserving the
  retail fade duration before the movie hand-off at any guest update rate.
- The title Start fade clamps to 255 but exits without rendering that final
  shade. At its `FadeEnd` call (`0x8002C178`) the host substitutes true black
  before retail frees/reloads the front-end screens, suppressing the transient
  fully filled loading page. `Game::PlayMovie` then supplies the bare STR
  filename in `a1`; the host captures it at `0x8002BBF0` and keeps suppression
  active. Retail continues its display/audio setup, then pauses at the caller-gated
  `MoviePlayer::Play` boundary `0x80014534` while the main thread owns the
  Wuffs-generated codecs, OpenGL, OpenAL, and movie input.
- At that later movie boundary, the main thread discards queued pre-movie GPU
  frames before releasing the guest. The last
  decoded movie frame remains visible across verified consecutive movie call
  sites; the intervening retail display-teardown frames are consumed without
  presentation. Normal guest presentation resumes after the final movie.
- All retail patches are fingerprint-gated and reversible. Overlay patches use
  a wider fingerprint because overlays reuse load addresses.
- Memory-card persistence stays on the device side of the authority boundary.
  Retail authors the directory entries, save header, checksum, and payload;
  the host persists the exact 128 KiB card image and schedules the original
  Psy-Q completion callbacks through the interrupt stack. Slot two is reported
  disconnected rather than aliasing slot one.
- Retail reaches `firstfile` through its own linked libapi copy, which requires
  a `"bu"` entry in the kernel device table at `0x150`/`0x154` before it will
  issue the BIOS call. Card device setup publishes one. Without it the retail
  wrapper returns a null entry and its caller blocks forever in
  `_get_card_event_x`.

Detailed addresses, disassembly, measurements, and confidence labels belong in
`docs/re/README.md`. Debugging chronology belongs in Git history, not here.

## Open work


### Immediate priority

1. Validate longer first-level play, later campaign transitions, keyboard, and
   gamepad routes. This includes reaching later calls among the disc's nine
   `.STR` movies; the three startup movies have been live-validated.

### Compatibility and fidelity

2. Replace the instruction-count VBlank approximation with cycle-aware device
   scheduling and explicit IRQ delivery. This is required before claiming
   asynchronous GPU/SPU callback fidelity outside the tested polling route.
3. Support partial GP0 `0x02` fills in the PsyCross presenter. It currently
   handles only full-buffer clears.
4. Investigate ten textured polygons per frame that sample an unwritten zeroed
   8-bit VRAM region around `(368,128)`. This is measured separately from the
   resolved 60 Hz load corruption and has not caused a confirmed visible defect.

### Experimental 60 Hz guest mode

6. Revalidate floor and wall collision after motion retiming.
7. Live-validate the obstacle `Think` holds (host hooks, fingerprint-gated per
   overlay). Every obstacle class is now
   covered, alongside position, gravity, the master clock, general scene
   animation, VRAM texture animation, base and moving world effects, the
   tutorial arrow counter, path-driven Platform motion, and the obstacle
   collision pass. That pass is authored-rate gated except for an active
   pusher's inner collision, which must refresh its contact every update.
   Eighteen further
   classes were added as one table: `Door`, `Conveyor`, `KickNRoll`,
   `KnockDown`, `Crusher`, `Launcher`, `Pendulum` in `NBOL_REL.BIN`;
   `DestructibleThing`, `Generator`, `EnemyGenerator`, `ThrowingGenerator`,
   `Collectible`, `Explosive`, `DynamicObstacle`, `Blast`, `TrapDoor` in
   `OL1_REL.BIN`; and `Ladder` and `Untouchable` in the boot executable.
   `Table` and `Chair` are thunks onto `DynamicObstacle`, and `SlipperyFloor`,
   `HorizontalPole`, `TriggerThing` and `Teleporter` have empty `Think`s.
   `Stack` is the exception: its `Think` re-poses a display model every call, so
   a whole-`Think` hold rubberbands it back to its intact pose on held updates.
   It is retimed by two narrow control-flow gates instead (`retimedStackTimeline`
   and `retimedStackSound`), which skip only `Wobble`/`Fall` and the tail sound
   `Think` on a held update while the pose-apply still runs every update. See the
   `Stack` section in `docs/re/README.md`.
   The seven `NBOL` table entries plus the two `Stack` gates are live on the
   level-one route; the nine `OL1` entries need a level that loads
   `OL1_REL.BIN`. `Door` is live-fixed after a first attempt broke unlocking and
   opening outright, and the `Stack` rubberband is fixed after the whole-`Think`
   hold; the rest have no live behavioural confirmation yet. The first-level
   entrance neon's measured transition intervals match stock at 32/38 VBlanks.
   The Platform traffic patch still needs live speed comparison against stock.
8. Scale the VBlank wall-clock consumers above 60 Hz. `rFrameCount60`
   (`0x800DD678`) advances once per emulated VBlank, and retail measures
   durations in it — dialog timeouts, CD waits, load synchronization, menu and
   title fades. `vblank_rate` stays 60 up to a 60 Hz update rate, so those are
   correct at 30 and 60; above that they are short by `vblank_rate / 60`, which
   is not an integer at 90 Hz. The counter itself cannot be retimed:
   `VSCallback__Fe` is the swap gate and reads it directly, so each consumer
   needs its own window scaled. `docs/re/README.md` lists the sites.
9. Watch for very slow movement stalling. Half-to-even rounding cancels normal
   odd-step bias, but a one-unit step still rounds to zero. A confirmed case
   would require per-object fractional remainders.
   Gravity is a related but distinct case, and is a real defect above 60 Hz.
   It is a constant — the player's is 18 — so its per-update rounding is
   biased in one direction rather than alternating. Half-to-even is exact at a
   divisor of two and understates it by 11% at four, which lowers jumps.
   `docs/re/README.md` records the divisor-independent replacement: a
   telescoping partition over the update's phase within the authored step.
10. Revalidate campaign transitions beyond the scripted cold first-level route.
    The mode now falls back to retail's full schedule only for explicit load
    and transition state handlers, preserving the original 2:1 cadence without
    mistaking gameplay palette or UI uploads for loading.
11. Live-validate 90 Hz, the highest rate this host sustains. Deterministic
    probes reach the first level at 60, 90, 120, and 240 Hz without faulting,
    hold the authored 30 Hz clock against wall clock given the CPU budget, and
    keep audio at 44.1 kHz, but motion smoothness, collision, and load
    transitions above 60 Hz have not been observed.
12. Scale two is now live-confirmed at full speed on the tested gameplay route.
    Boundary batching plus exact `WaitForLayer` poll fast-forward raised the
    heavy 100M probe from 42.6M/s to 142.9M guest-equivalent instructions/s
    (3.35x), with identical legacy diagnostics. RAM basic blocks are decoded
    once and invalidated by code-page generations; the cached tier executes
    through the shared interpreter semantic bodies. Differential tests cover
    branches, delay slots, delayed loads, memory, host boundaries, retime
    hooks, guest self-modifying code, native delayed loads, and native side
    exits. The scripted 900M title-to-level route stops on budget with 1,594
    VBlanks and zero GPU segment mismatches.
    A controlled `--interpreter-cpu` selector now keeps the oracle available
    for backend A/B runs. After warming both paths, four alternating 150M boot
    probes averaged 2.490 s through the cached recompiler and 3.462 s through
    the interpreter: 39.0% more instruction throughput and 28.1% less wall
    time. Three alternating scripted 900M title-to-level probes averaged
    5.808 s versus 7.853 s, a 35.2% effective-throughput gain even with
    588M verified idle instructions fast-forwarded identically on both paths.
    The second x64 stage is now the default. Four alternating 150M probes
    averaged 1.934 s natively and 2.669 s with
    `--cached-recompiler-cpu`: 38.0% more throughput and 27.5% less wall time
    over stage one, with 60.4M instructions executed in native regions. On the
    scripted 900M route, native/cached/interpreter runs took 4.334/5.983/7.807 s
    and ended at the same PC with 1,594 VBlanks and zero segment mismatches.
    Native lowering therefore adds 38.0% effective throughput over the cached
    tier and 80.1% over the interpreter on that route.
    Stage three adds guarded RAM `SB`/`SH`/`SW`, ending each store region at the
    write so the host can invalidate a reported code-page address before the
    next guest operation. Scratchpad/MMIO/alignment guards retry portably, and
    write tracing disables native stores. Three current 150M runs averaged
    1.836/2.703/3.575 s for native/cached/interpreter: 47.2% more throughput
    than the cached tier and 94.7% over the interpreter. Native coverage rose
    from 60.4M to 76.5M instructions, including 9.44M stores; versus the stage
    two native baseline, wall time fell another 5.1%. A current 900M route took
    4.047/6.032/7.814 s, with all backends at the same PC, 1,594 VBlanks, and
    zero GPU segment mismatches.
    Stage four lowers the ordinary conditional branches, REGIMM branch/link
    forms, and direct/indirect jumps together with their delay slots. Taken and
    fall-through paths, link hazards, a load immediately before a branch,
    guarded memory in the slot, and a boundary at the slot are differential-
    tested against the interpreter. Three alternating 150M runs averaged
    1.214/2.641/3.563 s for native/cached/interpreter: 117.5% more throughput
    than the cached tier and 193.5% over the interpreter. Native coverage rose
    to 143.1M instructions (95.4%), including 32.28M control transfers and
    12.17M stores; versus the recorded stage-three native baseline, wall time
    fell 33.9%. A current 900M route took 2.906/5.914/7.890 s, with all
    backends at PC `0x80052640`, 1,594 VBlanks, and zero GPU segment mismatches.
    COP0/GTE operations, unusual memory forms, cross-block native linking, and
    non-x64 lowering remain future expansion points.
    Stage five adds dynamic opcode/SPECIAL fallback histograms and permits an
    isolated guarded `SB`/`SH`/`SW` to form a one-instruction native region.
    The histogram showed `SW` was 6.69M of the remaining 6.91M portable
    instructions on the 150M route. Native coverage now reaches 149.78M
    instructions (99.85%) and 18.85M stores; isolated `SW` fallback falls to
    about 19K warm-up/guarded executions. A direct five-round stage-four versus
    stage-five A/B, excluding the first cold pair, measured 1.257 versus
    1.186 s, 5.6% less wall time. After explicit backend warm-up, three current
    150M runs averaged 1.204/2.709/3.600 s for native/cached/interpreter, 124.9%
    more throughput than the cached tier and 198.9% over the interpreter. The
    900M route lowers 311.37M of 313.55M non-idle instructions (99.31%) and
    remains bit-identical at PC `0x80052640`, 1,594 VBlanks, and zero GPU
    segment mismatches. Variable shifts and HI/LO arithmetic were prototyped
    but rejected after direct A/B runs were neutral to slightly slower; they,
    COP0/GTE, unusual memory forms, native block linking, and non-x64 lowering
    remain portable/future work.

### Presentation polish

13. Characterize HUD anchoring at wider aspects and persist presentation
    settings. Alt+Enter now toggles desktop fullscreen, but the mode is not yet
    persisted.

## Reproduction baseline

Build and run all core tests:

```bat
tools\build_windows.cmd
```

Run the normal live route:

```bat
build\windows\RelWithDebInfo\stuntmaster.exe --game "path\to\game.cue" --run --input-config input.example.ini
```

Run a reproducible headless title-to-level route through PsyCross:

```bat
build\windows\RelWithDebInfo\stuntmaster.exe --game "path\to\game.cue" --probe-guest --guest-budget 900000000 --publication-trace --script-input 200:start
```

Capture the SPU mix without requiring an audio device:

```bat
build\windows\RelWithDebInfo\stuntmaster.exe --game "path\to\game.cue" --probe-guest --guest-budget 400000000 --script-input 900:start --audio-capture out.wav
```

Stable acceptance signals are:

- the build script completes and CTest passes;
- the exact supported volume and executable fingerprints are reported;
- a fixed-budget probe stops on its instruction budget rather than a fault;
- the live route shows the recognizable title and first-level scene;
- PAD input selects the gameplay route;
- the audio capture contains retail SFX/music rather than silence.

Exact PCs, primitive counts, callback counts, and VBlank numbers are useful
diagnostics but may change with scheduler and publication work. Do not treat
them as permanent acceptance values.

## Known development environment

- Windows 10/11 x64
- Visual Studio 2022 Community, MSVC `14.43.34808`
- Visual Studio CMake and vcpkg
- Ninja `1.11.1`
- Python `3.10.10` with Capstone
- ghidra_12.1.2_PUBLIC with MCP and Scripting-through-MCP support enabled

## Documentation map

- `README.md`: project overview and normal usage
- `docs/STATUS.md`: current truth, limitations, and priorities
- `docs/HANDOFF.md`: shortest safe path for a follow-up session
- `docs/ARCHITECTURE.md`: durable authority and component boundaries
- `docs/PRESENTATION_TIMING.md`: simulation/presentation timing design
- `docs/re/README.md`: evidence-backed reverse-engineering notebook
- `docs/re/BOOT_CHAIN.md`: boot trace and low-level reproduction
- `THIRD_PARTY.md`: pinned upstreams and provenance
