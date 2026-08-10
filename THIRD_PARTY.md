# Third-party provenance

## SF-pc-port

- Repository: <https://github.com/Madxbio97/SF-pc-port>
- Pinned reference revision: `d9522cda2f785f6f3cb3243a57b12b371528faab`
- License: MIT
- Local research checkout: `.research/SF-pc-port` (ignored by Git)

The project may adapt generic disc, PS-X EXE, R3000A/GTE, machine-device, and
reverse-engineering infrastructure from this revision. Adapted files must keep
a source-provenance comment and the upstream MIT notice. Syphon Filter game
knowledge and game-specific bridge code must not be presented as generic
Stuntmaster findings.

## PsyCross

- Repository: <https://github.com/neonoxd/PsyCross>
- Pinned submodule revision: `23284b7ef885eb2cfa19da26c45e18446062a61d`
- License: MIT
- Path: `external/PsyCross`

The pinned submodule carries focused renderer backports in
`src/render/PsyX_render.cpp`. `GR_BindVertexBuffer` explicitly binds the VBO
paired with the selected VAO. OpenGL does not restore `GL_ARRAY_BUFFER` when a
VAO is bound; without the explicit bind, PsyCross's two VAOs can both upload
through the last startup VBO and a later host presentation can replace queued
vertex data. The fix was cross-checked against the newer PsyCross copy in the
pinned SF-pc-port research checkout. The renderer also exposes a no-VRAM-store
scene end for upload-authoritative retained replay, and a private framebuffer
cache for stable high-rate repeats without reading compositor-dependent
`GL_FRONT`. Stuntmaster-specific packet conversion, VRAM publication ordering,
and capture policy remain in the host adapter outside PsyCross.

The Windows presentation build resolves SDL2 and OpenAL-soft, plus OpenAL's
fmt dependency, through vcpkg. The registry baseline is pinned in `vcpkg.json`;
downloaded packages and build trees remain under ignored build/user-cache
locations. Release archives are linked against their static-library variants
and carry their license notices.

## FFmpeg

- Project: <https://ffmpeg.org/>
- Authentic source release: `8.1.2`
- Source URL: <https://ffmpeg.org/releases/ffmpeg-8.1.2.tar.xz>
- Source SHA-256: `464BEB5E7BF0C311E68B45AE2F04E9CC2AF88851ABB4082231742A74D97B524C`
- Detached-signature SHA-256: `0A0963FCCD70597838073F3E31B20F4A4D8CC2B5E577472C9A5A1F22624246F8`
- Release-key SHA-256: `397B3BECEDCD5A98769967FF1FF8501DDC89F8368B8F766E4701377D7DBAABE5`
- Required primary fingerprint: `FCF986EA15E6E293A5644F10B4322F04D67658D8`
- License: LGPL 2.1 or later

The build downloads only FFmpeg's official source archive, detached signature,
and release key. It verifies all three hashes, requires the exact key
fingerprint, and verifies the signature in an isolated GnuPG keyring before
compiling. The explicit configuration enables the `str` demuxer, `mdec` and
`adpcm_xa` decoders, and the `avcodec`, `avformat`, `avutil`, `swresample`, and
`swscale` libraries. Programs, encoders, muxers, protocols, networking,
filters, devices, GPL, version-3-only, and nonfree code are disabled and the
resulting feature matrix is checked before compilation.

Those five libraries are built with MSVC `/O2 /Brepro /MT /GL` and NASM, then
statically linked into `stuntmaster.exe`; no FFmpeg DLL is distributed. Release
archives include FFmpeg's LGPL and upstream license notices. Each automated
draft Release also carries an exact corresponding-source/relinking bundle; see
[docs/FFMPEG_RELINKING.md](docs/FFMPEG_RELINKING.md).

## psx_mnd_sym format reference

- Repository: <https://github.com/mefistotelis/psx_mnd_sym>
- Pinned reference revision: `546480d905779c5d40efb0d515a2b5777d616bd4`
- License: Unlicense
- Local research checkout: `.research/psx_mnd_sym` (ignored by Git)

`tools/psyq_sym.py` is an independent Python implementation of the documented
MND/SYM record layout, written so symbol recovery has no Go or external-package
runtime dependency.

## Retail game data

Jackie Chan Stuntmaster, its executable, data, movies, audio, artwork,
characters, and trademarks are not licensed by this project. The port must
load them from a user-supplied supported disc image and must not redistribute
them.
