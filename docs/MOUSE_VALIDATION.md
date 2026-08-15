# Mouse control live validation

Automated tests cover semantic layout translation, context rejection,
fingerprint/revert behavior, direct-pad publication, both movement
trampolines, stock/off execution, stationary and attack facing, and target
suppression. The remaining checks are feel and retail animation behavior in a
real level.

Build and launch the exact supported NTSC-U image:

```powershell
tools\build_windows.cmd
build\windows\RelWithDebInfo\stuntmaster.exe `
  --game "..\gameassets\Jackie Chan Stuntmaster (USA).cue" --run
```

Use this short pass:

1. On the title and front-end screens, confirm the mouse is not captured and
   left/right click do not navigate or activate menu rows.
2. Enter the first playable level. Confirm capture begins without an initial
   facing snap, horizontal movement turns Jackie, left click punches, and
   right click kicks.
3. Hold movement directions while turning. In the default camera-relative
   mode, W/A/S/D should retain the fixed camera's directions while Jackie uses
   plausible forward/side/back strafe animations to face the mouse yaw.
4. Fight beside more than one foe. Movement/strafe must not acquire a target or
   rotate Jackie back toward one automatically. Punch, kick, counter, grab,
   jump, roll, directional attacks, held attacks, and simultaneous keyboard or
   gamepad inputs should still follow retail behavior.
5. Press F10 once for character-relative mode. Movement should rotate with
   Jackie and use the ordinary run path. Compare control feel and troublesome
   animation/geometry situations with camera-relative mode.
6. Press F10 again for off. Capture and mouse attacks should stop and stock
   camera-relative movement/facing should return. A third press returns to
   camera-relative mode.
7. Pause and resume, enter/leave photo mode with F11, Alt+Tab away and back,
   trigger a movie/cutscene if convenient, and quick-save/load with F5/F9.
   Capture must follow ownership, no click may leak across a transition, and
   returning to play must synchronize to the current player yaw without a
   stale turn.
8. Optionally bind Middle/X1/X2 to the other semantic actions in `input.ini`
   and switch the retail controller layout. The semantic action must remain the
   same under every layout.

Useful A/B configuration:

```ini
mouse.movement_mode=camera_relative
mouse.sensitivity=20
mouse.punch=Left
mouse.kick=Right
mouse.grab=Middle
mouse.jump=X1
mouse.dive_roll=X2
```

Report the movement mode, location, input sequence, observed animation/action,
and expected behavior for any mismatch. A short capture is especially useful
for camera-relative animation or facing problems.
