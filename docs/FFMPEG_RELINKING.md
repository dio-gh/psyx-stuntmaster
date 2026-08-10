# FFmpeg source and relinking

The Windows release links a narrowly configured FFmpeg 8.1.2 build statically
into `stuntmaster.exe`. FFmpeg remains licensed under LGPL 2.1 or later; the
project does not enable FFmpeg's GPL, version-3-only, or nonfree modes. This
document is intended to preserve the practical ability to inspect, modify, and
replace that library. It is not legal advice.

## Corresponding-source release asset

Each automated draft Release carries a
`stuntmaster-pc-<version>-corresponding-source.zip` asset beside the executable
ZIP. It contains:

- the exact Stuntmaster source commit used for the executable;
- the complete pinned PsyCross submodule source (which GitHub's automatic
  source archives omit);
- FFmpeg's authentic `ffmpeg-8.1.2.tar.xz`, detached signature, and release
  key from `ffmpeg.org`;
- the build, packaging, and verification scripts; and
- `SOURCE-PROVENANCE.json`, recording all three source identities and FFmpeg's
  source/key/signature hashes.

Keep that corresponding-source asset available wherever the executable ZIP is
redistributed. The binary archive also includes FFmpeg's LGPL text, upstream
license summary, this document, and the complete third-party provenance file.

## Rebuild the unmodified release

Use 64-bit Windows, Git, Visual Studio 2022 with Desktop C++, CMake, and vcpkg,
plus MSYS2 at `C:\msys64`. Install NASM from the signed MSYS2 repository:

```powershell
C:\msys64\usr\bin\pacman.exe -S --needed nasm
tools\build_windows.cmd release
```

`tools/build_ffmpeg_windows.ps1` downloads only the official FFmpeg source
archive, detached signature, and release key. It pins all three SHA-256 values,
requires the exact release-key fingerprint, verifies the signature in an
isolated GnuPG keyring, and rejects any unexpected enabled codec or format.
The reference build enables only the `str` demuxer, `mdec` and `adpcm_xa`
decoders, and the `avcodec`, `avformat`, `avutil`, `swresample`, and `swscale`
libraries. It disables programs, devices, filters, networking, protocols,
encoders, muxers, GPL, and nonfree code.

## Modify FFmpeg and relink

Run the build once to verify and extract the authentic source, then edit the
working tree under:

```text
build/dependencies/ffmpeg-8.1.2-msvc-x64-static/source/ffmpeg-8.1.2
```

Force a fresh library build and relink the application:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/build_windows.ps1 `
  Release -ForceFfmpegRebuild
```

The minimal-component assertions remain active. If a modification deliberately
needs another FFmpeg component, update the explicit configure list and expected
component list in `tools/build_ffmpeg_windows.ps1`, then rebuild. Stuntmaster's
MIT license permits source modification; do not remove the LGPL notices or
misrepresent a modified binary as an official project build.

The final executable is linked with MSVC whole-program optimization. FFmpeg C
objects use `/O2 /Brepro /MT /GL`; the executable's Release target uses CMake
interprocedural optimization and `/Brepro`. NASM supplies FFmpeg's x86 assembly
objects. A slower development-only build can pass `-DisableFfmpegAssembly`,
which disables all assembly rather than producing an incomplete mixed build.
