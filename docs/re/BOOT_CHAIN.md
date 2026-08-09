# Boot-chain trace

Last verified: 2026-07-29.

## Reproduction

```bat
tools\build_windows.cmd -CoreOnly
build\windows-core\RelWithDebInfo\stuntmaster.exe --game "path\to\jackie_chan_stuntmaster.cue" --probe-guest
```

The probe loads the retail executable at `0x8002601C`, runs its R3000A code,
dispatches known BIOS/Psy-Q platform boundaries in the host, schedules
callbacks, and stops after a deterministic instruction budget.

## Verified progression

1. Entry, `main`, and `SetupPSXStuff__Fv` execute from the retail executable.
2. BIOS-less startup installs B0/C0 tables, event descriptors, interrupt
   priority nodes, and the `MyVBL__Fe` callback.
3. ResetGraph writes direct GP0/GP1 commands and initializes GPU DMA.
4. The RAD CD layer reads its directory and game data from the validated CUE.
5. GPU DMA6 builds the reverse ordering-table chain and clears busy
   synchronously.
6. The sound-bank loader submits DMA4; audio is deferred, but the installed
   libspu completion callback runs and releases the asynchronous load object.
7. The game loads and executes code in the `0x80010000` overlay region.
8. GPU DMA2 block and linked-list streams are captured for the native renderer.
9. The startup movie boundary is skipped only for its verified
   `Game::PlayMovie` caller.
10. GPU and VSync callbacks keep the retail render queue advancing.
11. A connected neutral digital pad bypasses the controller warning.
12. The title path emits a retained frame that PsyCross/OpenGL renders.
13. Early live keyboard input at the title advances into the player-selected
    gameplay route. This observation predates the corrected host PAD byte
    order, so individual Start semantics still require a fresh live check.
14. Letting the title time out advances into a distinct attract/demo route.
    Its apparent pause-menu and Down behavior proves a non-neutral PAD word
    reached retail code, but predates the corrected button byte order and is
    not evidence of individual button identities or a new game.

The fast one-million-instruction trace remains useful for early bootstrap
regressions. The current rendered-title reproduction is:

```bat
tools\build_windows.cmd
build\windows\RelWithDebInfo\stuntmaster.exe --game "path\to\jackie_chan_stuntmaster.cue" --probe-guest --guest-budget 150000000 --capture-frame
```

Representative result:

```text
guest_stop=instruction budget
guest_instructions=150000000
guest_pc=0x80052604
cd_read_calls=65
gpu_frame_primitives=996
scheduled_vsync_callbacks=137
scheduled_gpu_callbacks=36
frame_capture=SCREENSHOT.BMP
```

The exact retained transition frame changes as callback timing improves. The
acceptance signal is a retail title image and an instruction-budget stop, not
an exact primitive count.

## Timing and callback model

The bootstrap scheduler currently advances one VBlank every 564,480 executed
guest instructions. It increments Psy-Q's library-owned `Vcount`, looks up the
enabled `F2000003/2` event, and calls `MyVBL__Fe`. The full interrupted
R3000 state is restored when the callback returns; RAM and device effects
remain visible.

This is deliberately a bootstrap model. It does not yet account for
per-instruction cycles, exception entry, COP0 interrupt state, nested
interrupts, or precise timer phase.

DMA4 completion uses the callback pointer installed by this retail libspu at
`0x800DBF18`. That address is revision-specific and must remain in the retail
adapter, not generic PSX code.

DMA2 completion uses a monotonic transfer watermark. The callback stored at
`0x800D51A0` is sampled while guest calls are suspended but invoked only at the
retail DrawSync boundary (`0x80026C04`). If deferred delivery has outlived
`RenderQueue::current`, `WaitForLayer`'s live layer index in `s2` and the view
table at `0x800DD780` reconstruct the exact layer pointer needed by the
unmodified callback's final `tLayer::Free`. The host first checks `s2` against
the render queue's layer count; `s2` is not a layer index at every DrawSync
caller, and unchecked wrapped addressing was proven to corrupt the queue.

## GPU boundary

`GpuCommandBridge` currently owns:

- GP0 `0x1F801810` and GP1/GPUSTAT `0x1F801814`;
- DMA2 GPU MADR/BCR/CHCR;
- DMA4 SPU MADR/BCR/CHCR completion;
- DMA6 OTC MADR/BCR/CHCR and guest-RAM link generation.

DMA2 linked lists are cycle-bounded and block transfers are RAM-bounded. The
stateful decoder distinguishes GP0 image payload from commands, populates
1024x512 VRAM, and tracks drawing environments. Complete presentation
snapshots are published at GP1 display-start flips rather than arbitrary
instruction-count VBlank boundaries.

The PsyCross adapter replays only screen-sized render targets. It publishes
the decoded VRAM before creating draw splits. Raw GP0 XY fields and E5 draw
offsets are signed 11-bit values; after applying E5 and subtracting the target
origin, the adapter converts them to the half-float fields expected by
PsyCross's default x64 PGXP layout.
Without that conversion, every vertex collapses near `(-0.5, -0.5)` and the
frame is black. Retention snapshots packet data, VRAM, and display origin as
one frame; otherwise later guest uploads make a valid older packet frame turn
black or checkerboard-corrupted. Persistent GP0 E1-E5 state is carried into
each new interval. These rules reproduce the title and a stable, unobstructed
gameplay scene. Each replay also begins from a black background because
PsyCross otherwise preserves color-buffer history in pixels not touched by
the current retained packet interval.

## Live reproduction

```bat
build\windows\RelWithDebInfo\stuntmaster.exe --game "path\to\jackie_chan_stuntmaster.cue" --run --input-config input.example.ini
```

The live host polls native input and refreshes the retail port-one buffer at
guest VBlank. Press Return/Start while the title is visible for the
player-selected route; waiting enters attract mode. WASD is the default D-pad
mapping. The default 1280x720 window contains a 960x720 original-4:3 render
target; the Display menu or an explicit widescreen render aspect expands the
PsyCross/PGXP projection.

## Scope

This note records the verified boot progression and its original reproduction
boundary. The runtime now proceeds beyond this trace into live first-level
gameplay. Current priorities and validation limits are maintained in
`../STATUS.md`; do not infer the next task from this historical boot note.

## Known HLE limitations

- `printf` logs only the format string.
- Critical-section syscalls return success but do not model complete COP0
  transitions.
- `WaitEvent` is nonblocking; callback events are host-scheduled.
- CD bytes are copied synchronously. Completion is immediate by default;
  `--cd-read-pacing N` exposes remaining-sector polling at a chosen bulk rate.
  In requested-60 mode, rates above 2 fall back to console 2x during the final
  upload-quiet settling window.
- GPUSTAT/DMA timing is not cycle-accurate. Callback delivery still uses the
  bootstrap instruction/VBlank schedule described above.
- Port one is live and remappable; analog-stick-to-digital behavior and
  controller hot-plug need longer validation.
- Live presentation supports both the PsyCross bridge and a direct GP0
  rasterizer. Neither makes the device scheduler cycle-accurate; HUD anchoring
  and widened visibility still need broader validation.
- PsyCross's `SCREENSHOT.BMP` helper writes the OpenGL row order directly, so
  the file is vertically inverted even though the window is not.
- BIOS ROM compatibility reads return zero; no retail PS1 BIOS is required or
  bundled.
