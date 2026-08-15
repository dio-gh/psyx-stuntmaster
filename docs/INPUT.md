# Input configuration

Live input is enabled only in the PsyCross build. Without an explicit flag,
the game loads `input.ini` beside `stuntmaster.exe`; the build seeds that file
from `input.example.ini` only when it is missing:

```bat
build\windows\RelWithDebInfo\stuntmaster.exe --game "path\to\game.cue" --run
```

Use `--input-config <path>` to override the executable-local configuration.

Add `--input-trace` to print only PAD transitions. For example, pressing and
releasing V with the example bindings prints Circle and then neutral:

```text
pad1_active_low=0xDFFF pressed=circle
pad1_active_low=0xFFFF pressed=none
```

This reports the normalized active-low host word before RetailHle serializes
its low byte and then high byte into the retail direct-pad buffer. It does not
inject or automate input.

The file is line-oriented `key=value`. Blank lines and `#` comments are
ignored. Keyboard values are SDL scancode names; gamepad values are SDL
controller button or axis names. Unknown keys and invalid values stop startup
with a line-numbered error.

Supported keyboard actions:

```text
square circle triangle cross
l1 l2 l3 r1 r2 r3
start select
left right up down
```

Use the `keyboard.` prefix, for example:

```ini
keyboard.up=W
keyboard.down=S
keyboard.start=Return
```

Gamepad supports the same actions with the `gamepad.` prefix, plus:

```text
left_x left_y right_x right_y
```

Mouse bindings use gameplay semantics rather than fixed PlayStation buttons.
This matters when the game's controller layout changes: `mouse.punch=Left`
still requests Punch through the live retail layout instead of always
impersonating Square. The host then leaves retail's own command table to derive
heavy, back, directional, held, and chorded attacks.

Supported semantic mouse actions are:

```text
punch kick grab jump dive_roll strafe counter status
```

Each accepts `Left`, `Right`, `Middle`, `X1`, `X2`, or `None`. Defaults bind
left click to Punch and right click to Kick; the other six are unbound. A mouse
button may be assigned to more than one action deliberately. Mouse gameplay
actions are accepted only during exact player control, so clicks do not become
face-button presses in the title, menus, pause screen, movies, loads,
cutscenes, or photo mode. Keyboard and gamepad bindings remain active and may
be combined with mouse actions normally.

Horizontal movement turns the character. The initial behavior and sensitivity
are configurable:

```ini
mouse.movement_mode=camera_relative
mouse.sensitivity=20
```

Sensitivity is positive guest angle units per horizontal pixel; 65,536 units
make one full turn. `F10` cycles these modes at runtime:

- `camera_relative`: W/A/S/D retain the fixed camera's movement directions
  while mouse yaw independently controls facing. Ordinary Run uses retail's
  authored directional strafe path and its forward/side/back animation table.
- `character_relative`: movement directions rotate with mouse yaw and retain
  the ordinary Run path. This is the fallback/A-B mode when camera-relative
  strafing does not feel appropriate for a situation.
- `off`: stock movement and facing, no mouse actions, and no cursor capture.

Both mouse-facing modes suppress retail strafe's automatic foe acquisition and
release an existing strafe target through retail's own target API. Explicit
attacks, countering, and the game's other combat logic remain unchanged.
Vertical mouse motion is intentionally unused because the gameplay camera is
fixed. Cursor capture begins only after the guest confirms steady
`PlayerUserControl` with window focus, and is released on pause, loss of
control/focus, movies, loads, photo mode, and `off` mode.

`input.example.ini` is the canonical complete example. PsyCross's standard SDL
controller mapping supplies the default gamepad layout. The host reports port
one to the retail pad driver as a connected DualShock, so the game's Options
vibration toggle and shake code run; the retail motor values drive
`SDL_GameControllerRumble` on the first attached game controller. Port two,
analog-mode switching, and a graphical rebinding UI are deferred.

The focused gameplay and transition pass is in
[MOUSE_VALIDATION.md](MOUSE_VALIDATION.md).

Window and render size are independent of bindings. The default 1280x720
window contains a 960x720 original-4:3 target. `--render-size WIDTHxHEIGHT`
fixes another internal resolution independently of the resizable SDL window.

The number-row `0` and `F10` keys are always reserved as host-only controls
(diagnostic-overlay visibility and mouse-mode cycling, respectively). They are
masked at the PAD bridge even if an input configuration assigns either key to
a guest action. `--debug-overlay` selects visible-by-default.

`F11` toggles the experimental photo mode during steady gameplay. It starts
with the simulation frozen; `P` freezes/resumes simulation without leaving the
free camera. While photo mode is active, mouse movement controls the view,
`W`/`A`/`S`/`D` move
horizontally, `Q` descends, `E` ascends, and Left Shift increases speed. The
controller Select button also toggles the mode; the left stick moves, the right
stick looks, L2 descends, and R2 ascends, all proportionally after radial
deadzones. R3 is the controller freeze/resume control; L3 retains its optional
frame-trace diagnostic role. Controller Select is consumed as a host control during steady
gameplay but remains guest-visible in menus; the keyboard Select binding is
always guest-visible. The guest PAD is neutralized while photo mode is active
so held inputs cannot queue gameplay actions. The retail HUD is hidden for the
entire photo-mode session, including while P/R3 lets the simulation run. Loads,
camera animations, and retail camera-mode changes automatically resume the
simulation and return ownership to the game.

`F5`, `F6`, and `F9` are always host-only controls in live play. `F5`
atomically replaces `saves\quick-save.stsm`; `F9` restores that file, or
reports that it is missing or incompatible. `F6` preserves a separate state
with a millisecond timestamp in its filename. All three keys are edge-triggered
and masked at the PAD bridge even if the input configuration assigns them to a
guest action. Quick saves do not replace the game's persistent memory-card
saves.

`--load-quick-save <path>` restores a specific `.stsm` file before the guest
begins running. The executable fingerprint and state-affecting launch settings
must match those stored in the file.
