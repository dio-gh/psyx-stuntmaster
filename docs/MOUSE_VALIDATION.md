# Mouse control live validation

Phase 4 automated coverage executes all twelve fingerprinted guest seams, the
complete action-ownership classification, current-frame yaw publication,
directional Run animations, bounded host turning, context handoff, install and
revert. Phase 4 is the live campaign pass for gameplay feel and unusual retail
contexts.

Build and launch the exact supported NTSC-U image:

```powershell
tools\build_windows.cmd
build\windows\RelWithDebInfo\stuntmaster.exe `
  --game "..\gameassets\Jackie Chan Stuntmaster (USA).cue" --run
```

Use this pass:

1. On the title and front-end screens, confirm the mouse is not captured and
   left/right click do not navigate or activate menu rows.
2. Enter the first playable level. Capture should begin without an initial
   snap. Mouse up/right/down/left should request camera-forward/right/back/left
   facing. A circular gesture should produce one bounded 360-degree turn, not
   an instant flip when the requested angle crosses zero. Left click should
   Punch and right click should Kick.
3. Try very tight, very broad, slow, and fast circles. Tightness may change the
   requested direction but must not exceed `mouse.turn_rate`; starts and stops
   should respect `mouse.turn_acceleration`. If the defaults feel too loose or
   heavy, record the gesture and desired feel before changing the two values.
4. Hold each W/A/S/D direction while turning. Travel must stay fixed-camera
   relative and full-speed. Jackie should use plausible forward, side, and
   backward authored animations while his official body heading follows the
   mouse. He must remain in ordinary Run rather than targeting Strafe.
5. Fight beside one and several foes. Free locomotion must not acquire a foe or
   rotate Jackie toward one. Punch, kick, counter, grabs, pickups, throws,
   directional/back attacks, held attacks, and keyboard/gamepad combinations
   should keep retail context and combo behavior.
6. Exercise a launcher/bouncy surface, standing and running jump, dive roll,
   fall, slope, table roll, pushable object, ladder, ledge, wall, pole, and
   explicit Strafe where available. Context entry must claim the appropriate
   heading and retain its stock movement, targeting, collision, and speed.
   Returning to a mouse-facing lease must not replay mouse motion made during
   the context. Jackie's ambient idle animation must remain mouse-responsive.
   In particular, jumping while facing opposite the held travel direction must
   preserve mouse-facing through takeoff, apex, fall, and landing; jump force
   must still follow the held camera-relative direction.
7. Watch the heading diagnostic. Expected mouse-lease and normal retail-owned
   splits—including ledge latch and combat—are console-only and labelled
   `free_lease` or `retail_owned`. Only a split in a state that explicitly
   committed the headings, such as push, roll, slope, or hotfoot, can show
   `HEADING SPLIT: COMMITTED` after half a second. Console lines include state,
   body yaw, travel yaw, and signed delta, and a continuing split is
   rate-limited to once per second.
8. Press F10 for off. Capture and mouse actions should stop, and ordinary stock
   movement/facing should return without a snap. Press F10 again to restore
   camera-relative mouse control.
9. Pause/resume, enter/leave photo mode with F11, Alt+Tab away and back, trigger
   a movie/cutscene, and quick-save/load with F5/F9. Capture must follow
   ownership, no click may leak across a transition, and returning to play must
   synchronize from the live body without a stale turn or diagnostic.
10. Optionally bind Middle/X1/X2 to other semantic actions in `input.ini` and
    switch the retail controller layout. The semantic action and its retail
    combinations must remain correct under every layout.

Useful configuration:

```ini
mouse.movement_mode=camera_relative
mouse.turn_rate=720
mouse.turn_acceleration=10000
mouse.punch=Left
mouse.kick=Right
mouse.grab=Middle
mouse.jump=X1
mouse.dive_roll=X2
```

For a mismatch, report the level/location, action state from the log, input
sequence, observed animation or interaction, and expected behavior. A short
capture is particularly useful for animation transitions or turning feel.
