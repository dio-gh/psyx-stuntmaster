# psyx-stuntmaster

psyx-stuntmaster is an experimental Windows-native host for the NTSC-U release
of **Jackie Chan Stuntmaster** (`SLUS-00684`). It runs the original R3000A game
code while replacing the console boundary with native input, OpenGL/PGXP
presentation, OpenAL audio, homegrown Wuffs-generated movie codecs, and saves.

The project does not contain the game. You need a legally obtained dump of the
supported disc in BIN/CUE format.

## Features & Improvements

Preserves the original game logic while adding modern presentation and quality-of-life features:

- **Experimental 60 FPS mode** — gameplay can update at 60 Hz while motion, animation, timers, moving platforms, effects, and other frame-dependent systems are retimed to preserve their original speed. Retail-compatible 30 Hz remains available at any time.
- **Improved widescreen support** — expands the 3D view to the selected aspect ratio instead of stretching the original 4:3 image. Aspect-aware culling keeps additional side geometry visible while HUD and menu elements retain their intended proportions.
- **High-resolution PGXP rendering** — renders the original PlayStation graphics through OpenGL at resolutions up to 1440p, with PGXP-enhanced geometry and a resolution independent of the window size.
- **Instant display switching** — toggle between 30/60 Hz and 4:3/16:9 modes while playing. Display settings can also be changed through an added in-game menu and are remembered between sessions.
- **Remappable input** — configurable keyboard, SDL gamepad, and semantic gameplay mouse controls with conventional PlayStation-style controller mappings.
- **Photo mode** — freeze or resume the simulation while flying a free camera through the original retail renderer, with the gameplay HUD hidden automatically.
- **Quick saves** — save or restore the complete running game with dedicated hotkeys, alongside the original memory-card save system.
- **Portable launcher and settings** — select the game image and display options from a lightweight launcher; configuration and saves remain beside the executable for easy portability.

> The 60 Hz mode and widescreen culling are experimental. Retail-compatible
> 30 Hz and original 4:3 presentation remain available as compatibility
> fallbacks, and the complete campaign is still being validated.

<img width="1589" height="896" alt="image" src="https://github.com/user-attachments/assets/abc6d404-f401-4ce0-b6fb-6cf75a6674ff" />


## How to Play

1. Download `stuntmaster-pc-<version>-windows-x64.exe` from Releases.
2. Run it. On first launch it prompts for the folder containing the game's
   `.cue` and `.bin` files.

That single executable is everything you need: no launcher, no archive to
extract, no side files. It is self-configuring, storing its settings, input
bindings, logs, and saves under `Documents\Stuntmaster` (`saves\SLUS-00684.mcr`
for normal in-game saves, plus quick saves in the same `saves` directory). All
third-party license texts are embedded and viewable in-game or with
`stuntmaster.exe --licenses`. Because nothing is written beside the executable,
you can keep it anywhere and move it freely.

Only this disc revision is supported:

| Field | Required value |
| --- | --- |
| Region | USA / NTSC-U |
| Serial | `SLUS-00684` |
| Layout | Single-track MODE2/2352 BIN/CUE |
| BIN SHA-256 | `0DFC8FCB055E2EBF22380F5FF7568706376588FDCF8C4086DCFCA67DC8295E14` |

The first-launch picker expects exactly one `.cue` file in the selected folder.
The host validates the retail executable and disc data before running them.

## Controls

The default keyboard bindings are:

| Action | Key |
| --- | --- |
| Move | W, A, S, D |
| Punch / Square | J |
| Interact / Circle | E |
| Kick / Triangle | K |
| Jump / Cross | Space |
| Parry / L1 | C |
| Stats / L2 | Tab |
| Roll / R1 | Left Shift |
| Strafe / R2 | Left Ctrl |
| Start | Return |
| Select | Escape |

During gameplay, left click punches, right click kicks, and two-axis mouse
movement requests Jackie's camera-relative facing direction through a bounded
turn controller. Movement remains full-speed camera-relative Run while reusing
retail directional animations without entering Strafe; `F10` toggles mouse
control and stock/off mode. Mouse actions are semantic and remappable, so
retail controller layouts and combat combinations remain authoritative, and
mouse clicks never leak into menus or movies.

Standard SDL gamepads use their conventional PlayStation-style layout. Edit
`input.ini` beside the executable to remap keyboard, controller, or mouse input; the
complete format is documented in [docs/INPUT.md](docs/INPUT.md).

Host controls:

| Key | Function |
| --- | --- |
| `Alt+Enter` | Toggle borderless fullscreen |
| `0` | Toggle diagnostics |
| `F11` | Toggle photo mode / free camera during gameplay |
| `P` | Freeze/resume simulation while staying in photo mode |
| `F5` | Quick Save |
| `F6` | Write a timestamped quick save |
| `F7` | Toggle 30Hz/60Hz mode |
| `F8` | Toggle 4:3/widescreen mode |
| `F9` | Quick Load |
| `F10` | Toggle camera-relative / stock mouse control |

Photo mode starts with the world simulation frozen. Press `P`, or R3 on a
controller, to resume/freeze it without leaving the free camera. Move with `W`,
`A`, `S`, `D`, descend/ascend with `Q`/`E`, look with the mouse, and hold Left
Shift to move faster. On a controller, press Select to toggle photo mode, move
with the left stick, look with the right stick, descend with L2, and ascend with
R2. Guest gameplay input is neutralized until photo mode is disabled. Retail
HUD drawing stays hidden in either simulation state and returns when photo mode
ends. Retail automatically resumes and regains camera ownership when a load or
cutscene begins.

## Current status

The supported image boots without a PlayStation BIOS, reaches the title and
first playable level, accepts keyboard and gamepad input, renders through
PsyCross, plays sound and music, and decodes the verified startup movies.
Memory-card and quick-save flows work. The complete campaign has not yet been
validated, so later levels and transitions may expose missing emulation or
game-specific behavior.

Retail-compatible 30 Hz is the default. The launcher can arm the experimental
retimed 60 Hz mode and widescreen culling; both can also be changed from the
host-owned Display menu while playing. See [project status](docs/STATUS.md) for
the detailed compatibility boundary and current experiments.

## Build from a fresh clone

Requirements:

- 64-bit Windows 10 or newer;
- Git;
- Visual Studio 2022 with **Desktop development with C++**, **C++ CMake tools
  for Windows**, and **vcpkg** components;
- an internet connection for the first build.

Clone normally; `--recurse-submodules` is optional because the script
initializes the pinned Wuffs and PsyCross submodules when needed:

```bat
git clone https://github.com/neonoxd/psyx-stuntmaster.git
cd stuntmaster
tools\build_windows.cmd
```

The script discovers the installed Visual Studio edition, compiles the
checked-in C generated from the pinned Wuffs toolchain, lets vcpkg provide the
SDL2/OpenAL dependencies, builds `RelWithDebInfo`, and runs the tests. Downloads
and build products stay under the ignored `build` directory.

Other build modes:

```bat
tools\build_windows.cmd debug
tools\build_windows.cmd release
tools\build_windows.cmd -CoreOnly
tools\build_windows.cmd -SkipTests
```

The full build outputs the single, self-configuring `stuntmaster.exe` under
`build\windows\<configuration>`. The core-only build, useful for emulator and
disc-boundary work, goes under `build\windows-core\<configuration>`.

You can still configure CMake directly. Set
`STUNTMASTER_ENABLE_PSYCROSS=ON` and use the
`x64-windows-static-release` overlay triplet. The build script is the reference
configuration.

## Create a release build

Run:

```bat
tools\package_windows.cmd
```

This performs a cleanly configured Release build, runs the deterministic test
suite, and stages the single upload-ready executable:

```text
dist\stuntmaster-pc-0.0.1-windows-x64.exe
```

That one self-configuring executable is the entire distribution -- no launcher,
archive, side files, or `licenses/` directory, because it seeds its own input
defaults into `Documents\Stuntmaster` on first run and embeds every third-party
license text. It contains no disc image, extracted retail asset, user
configuration, or save file. Alongside it, automated Releases also publish a
separate `stuntmaster-pc-<version>-corresponding-source.zip` with the exact
application, PsyCross, and Wuffs source needed to modify dependencies and
relink.

## Development commands

Inspect and validate a supported dump without extracting it:

```powershell
python tools/stuntkit.py inspect "path\to\jackie_chan_stuntmaster.cue"
```

Run the built host directly, bypassing the launcher UI:

```bat
build\windows\RelWithDebInfo\stuntmaster.exe --game "path\to\game.cue" --run
```

Explicit command-line options override matching launcher settings. Run the
executable without valid arguments to see the diagnostic option summary.
Reverse-engineering and deterministic probe workflows are documented in
[docs/HANDOFF.md](docs/HANDOFF.md) and [docs/re/README.md](docs/re/README.md).

## Architecture and documentation

- [Architecture](docs/ARCHITECTURE.md)
- [Project status](docs/STATUS.md)
- [Input configuration](docs/INPUT.md)
- [Requirements](docs/REQUIREMENTS.md)
- [Reverse-engineering notes](docs/re/README.md)
- [Third-party provenance](THIRD_PARTY.md)

The original guest remains authoritative for gameplay, physics, AI,
animation, collision, scripting, timers, and RNG. The host supplies emulated
hardware/services and presentation; user disc files are read-only and are
never patched on disk.

## Legal

Jackie Chan Stuntmaster, its code, data, movies, audio, artwork, characters,
and trademarks are not licensed by this project. Do not commit or redistribute
disc images, extracted assets, memory cards, or captured retail audio.

Project code is MIT licensed. PsyCross and other third-party components retain
their own licenses; see [THIRD_PARTY.md](THIRD_PARTY.md).
