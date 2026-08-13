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

## Wuffs

- Project: <https://github.com/google/wuffs>
- Pinned tag: `v0.3.4`
- Pinned submodule revision: `ec71f9c6d829ca763fbbc1f7adecc30a89a8ed0a`
- License: Apache-2.0
- Path: `external/wuffs`

Wuffs is the source language, proof-checking compiler, and C runtime used for
the homegrown PlayStation STR, MDEC v2, and stereo 4-bit CD-XA codecs under
`src/stuntmaster/media/wuffs`. These codecs target only the formats present on
the fingerprinted retail disc. Generated C is checked in so ordinary builds do
not require Go or execute the Wuffs compiler. The self-contained release
executable embeds Wuffs' Apache-2.0 license. See
[the codec scope](docs/WUFFS_CODEC_REPLACEMENT.md).

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
