# psyx-stuntmaster

psyx-stuntmaster is an experimental Windows-native host for the NTSC-U release
of **Jackie Chan Stuntmaster** (`SLUS-00684`). It runs the original R3000A game
code while replacing the console boundary with native input, OpenGL/PGXP
presentation, OpenAL audio, FFmpeg movie playback, saves, and a small launcher.

The project does not contain the game. You need a legally obtained dump of the
supported disc in BIN/CUE format.

## Features & Improvements

Preserves the original game logic while adding modern presentation and quality-of-life features:

- **Experimental 60 FPS mode** — gameplay can update at 60 Hz while motion, animation, timers, moving platforms, effects, and other frame-dependent systems are retimed to preserve their original speed. Retail-compatible 30 Hz remains available at any time.
- **Improved widescreen support** — expands the 3D view to the selected aspect ratio instead of stretching the original 4:3 image. Aspect-aware culling keeps additional side geometry visible while HUD and menu elements retain their intended proportions.
- **High-resolution PGXP rendering** — renders the original PlayStation graphics through OpenGL at resolutions up to 1440p, with PGXP-enhanced geometry and a resolution independent of the window size.
- **Instant display switching** — toggle between 30/60 Hz and 4:3/16:9 modes while playing. Display settings can also be changed through an added in-game menu and are remembered between sessions.
- **Remappable input** — configurable keyboard and SDL gamepad controls with conventional PlayStation-style controller mappings.
- **Quick saves** — save or restore the complete running game with dedicated hotkeys, alongside the original memory-card save system.
- **Portable launcher and settings** — select the game image and display options from a lightweight launcher; configuration and saves remain beside the executable for easy portability.

> The 60 Hz mode and widescreen culling are experimental. Retail-compatible
> 30 Hz and original 4:3 presentation remain available as compatibility
> fallbacks, and the complete campaign is still being validated.

<img width="1589" height="896" alt="image" src="https://github.com/user-attachments/assets/abc6d404-f401-4ce0-b6fb-6cf75a6674ff" />


## How to Play

1. Download `stuntmaster-pc-<version>-windows-x64.zip` from Releases.
2. Extract the entire archive to a writable folder.
3. Run `stuntmaster-launcher.exe`.
4. Select the folder containing the game's `.cue` and `.bin` files, choose the
   display options, and press **Play**.

The launcher saves portable settings in `stuntmaster.ini` beside the
executables. The game creates `saves\SLUS-00684.mcr` for normal in-game saves
and stores quick saves in the same `saves` directory. Moving the extracted
folder moves those settings and saves with it.

Only this disc revision is supported:

| Field | Required value |
| --- | --- |
| Region | USA / NTSC-U |
| Serial | `SLUS-00684` |
| Layout | Single-track MODE2/2352 BIN/CUE |
| BIN SHA-256 | `0DFC8FCB055E2EBF22380F5FF7568706376588FDCF8C4086DCFCA67DC8295E14` |

The launcher expects exactly one `.cue` file in the selected folder. The host
validates the retail executable and disc data before running them.

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

Standard SDL gamepads use their conventional PlayStation-style layout. Edit
`input.ini` beside the executable to remap keyboard or controller input; the
complete format is documented in [docs/INPUT.md](docs/INPUT.md).

Host controls:

| Key | Function |
| --- | --- |
| `Alt+Enter` | Toggle borderless fullscreen |
| `0` | Toggle diagnostics |
| `F5` | Quick Save |
| `F6` | Write a timestamped quick save |
| `F7` | Toggle 30Hz/60Hz mode |
| `F8` | Toggle 4:3/widescreen mode |
| `F9` | Quick Load |

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
- MSYS2 installed at `C:\msys64`, with NASM (`pacman -S --needed nasm`);
- an internet connection for the first build.

Clone normally; `--recurse-submodules` is optional because the script
initializes PsyCross when needed:

```bat
git clone https://github.com/neonoxd/psyx-stuntmaster.git
cd stuntmaster
tools\build_windows.cmd
```

The script discovers the installed Visual Studio edition, downloads and
signature-verifies the authentic FFmpeg 8.1.2 source release, builds only the
STR/MDEC/XA decoding surface as static libraries, lets vcpkg provide the
smaller SDL2/OpenAL dependencies, builds `RelWithDebInfo`, and runs the tests.
The first FFmpeg build takes roughly two minutes on a current four-core host;
later builds verify and reuse it. Downloads and build products stay under the
ignored `build` directory.

Other build modes:

```bat
tools\build_windows.cmd debug
tools\build_windows.cmd release
tools\build_windows.cmd -CoreOnly
tools\build_windows.cmd -SkipTests
```

The full build outputs `stuntmaster.exe` and `stuntmaster-launcher.exe` under
`build\windows\<configuration>`. The core-only build, useful for emulator and
disc-boundary work, goes under `build\windows-core\<configuration>`.

You can still configure CMake directly. Set
`STUNTMASTER_ENABLE_PSYCROSS=ON`, use the
`x64-windows-static-release` overlay triplet,
and point `STUNTMASTER_FFMPEG_ROOT` at the source-built install root containing
`include` and `lib`. The build script is the reference configuration.

## Create a release archive

Run:

```bat
tools\package_windows.cmd
```

This performs a cleanly configured Release build, runs the deterministic test
suite, invokes the CMake/CPack install rules, verifies the archive contents,
and prints its SHA-256. The upload-ready artifact is:

```text
dist\stuntmaster-pc-0.0.1-windows-x64.zip
```

The archive contains the launcher, game host, input defaults, project notices,
and third-party licenses. FFmpeg is statically linked, so its five former DLLs
are absent. It intentionally contains no disc image, extracted retail asset,
user configuration, or save file. Automated Releases also publish a separate
`stuntmaster-pc-<version>-corresponding-source.zip` with the exact application,
PsyCross, and signed FFmpeg source needed to modify FFmpeg and relink; see
[FFmpeg source and relinking](docs/FFMPEG_RELINKING.md).

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
