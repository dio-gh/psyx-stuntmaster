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

- Repository: <https://github.com/OpenDriver2/PsyCross>
- Pinned submodule revision: `e56e4cde1c2b8a15e0d4e38b26cdd9202e0d17e6`
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
- Prebuilt package: `FFmpeg.LGPL` `20260520.1.0` from NuGet
- Package SHA-256: `1BBEB9FE962B3CC3782541C3F02BBB491BB03B95D6C124BFAA859CE39FAC83CF`
- Package publisher source: <https://github.com/IOL0ol1/FFmpeg.Publisher>
- License in the selected package: LGPL 3.0 or later

The presentation build dynamically links the package's `avcodec`, `avformat`,
`avutil`, `swresample`, and `swscale` libraries for native PlayStation
STR/MDEC video and XA-ADPCM audio decoding. The pinned package contains
headers, MSVC import libraries, and DLLs, so the local build never compiles
FFmpeg. Its checksum is verified before extraction. Release archives include
the package's LGPL license text.

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
