# Wuffs codec replacement scope

## Decision summary

Replacing FFmpeg is tractable for the fingerprinted NTSC-U disc, but Wuffs is
the implementation language and C generator, not a ready-made replacement
library. Wuffs v0.3.4 contains no PlayStation STR demuxer, MDEC decoder, or
CD-XA ADPCM decoder. Those three layers must be implemented in the local Wuffs
package and compiled to checked-in C.

The former FFmpeg dependency was isolated behind `media::StrDecoder`, so its
public C++ contract remained stable during replacement. The homegrown package
now provides five logically separate operations:

1. raw 2352-byte-sector STR validation and demuxing;
2. assembly and decoding of MDEC video frames;
3. decoding of CD-XA ADPCM audio sectors;
4. full-range YCbCr 4:2:0 to RGBA8888 conversion; and
5. planar stereo S16 to interleaved stereo S16 conversion.

There is no sample-rate conversion or image resizing on the supported disc.
The replacement can emit RGBA8888 and interleaved stereo S16 directly, making
the last two operations small format conversions instead of general-purpose
`swscale` and `swresample` substitutes.

## Supported-disc inventory

This inventory was read from the exact supported BIN at the raw-sector level.
It describes all nine `.STR` files, not only the three startup movies that have
been live-validated.

Every file has the same structural baseline:

- concatenated 2352-byte MODE2 sectors with the standard 12-byte sync header;
- one video stream on STR channel 1;
- 320x240 MDEC version 2 video;
- one stereo, 4-bit CD-XA stream on channel 1;
- video payload chunks of `0x7E0` bytes beginning at sector offset `0x38`;
- audio payloads of 2304 bytes beginning at sector offset 24; and
- no RIFF wrapper, mono audio, 8-bit XA, resolution change, or second stream.

| Movie | Size (bytes) | Video frames | XA sectors | Audio | Audio-locked fps | MDEC qscale |
| --- | ---: | ---: | ---: | --- | ---: | --- |
| `CREDITS.STR` | 56,891,392 | 2,775 | 1,735 | stereo 18,900 Hz | 14.994597 | 1-6 |
| `DEMO.STR` | 20,283,392 | 990 | 619 | stereo 18,900 Hz | 14.993942 | 1-4 |
| `DOLBY.STR` | 1,884,160 | 122 | 115 | stereo 37,800 Hz | 19.891304 | 1 |
| `FACTORY.STR` | 32,575,488 | 1,589 | 994 | stereo 18,900 Hz | 14.986796 | 1-5 |
| `MAKING.STR` | 87,261,184 | 4,260 | 2,663 | stereo 18,900 Hz | 14.997184 | 1-7 |
| `MDWY320M.STR` | 3,178,496 | 155 | 97 | stereo 18,900 Hz | 14.980670 | 1-2 |
| `PROLOG.STR` | 54,902,784 | 2,678 | 1,674 | stereo 18,900 Hz | 14.997760 | 1-5 |
| `RADI.STR` | 1,769,472 | 86 | 54 | stereo 18,900 Hz | 14.930556 | 1-2 |
| `VICTORY.STR` | 49,655,808 | 2,422 | 1,514 | stereo 18,900 Hz | 14.997523 | 1-6 |

Video frames span 6-11 sectors. The largest encoded frame observed is 22,072
bytes, below the 22,176-byte capacity of eleven `0x7E0` chunks. Each XA sector
contains eighteen 128-byte sound groups and decodes to 2,016 stereo sample
frames. The unusual `DOLBY.STR` rate is why the current decoder performs a
demux-only timing pass instead of trusting STR's conventional 15 fps timebase.

## Required homegrown modules

### STR sector parser and demuxer

The parser must validate the sync bytes, channel and submode fields, duplicated
MODE2 subheaders, sector bounds, frame index/count, encoded frame size, and
width/height before exposing payloads. It must assemble video chunks by frame
and preserve XA predictor history across sectors. Empty/end sectors are legal;
truncated, reordered, duplicated, or inconsistent frame sectors fail closed.

The existing whole-file ownership model can remain for the first replacement.
It supports a deterministic pre-pass that counts completed video frames and XA
sectors, derives the audio-locked frame rate, and then restarts from byte zero
for playback. Streaming I/O is not needed to remove FFmpeg.

### MDEC version 2 video decoder

Only the supported intra-only form is required initially. The implementation
still needs the substantive parts of an MPEG-1 intra decoder:

- 16-bit word byte swapping and the MDEC frame preamble;
- 10-bit DC coefficients used by MDEC versions 1 and 2;
- MPEG-1 run/level VLC decoding, including escape and end-of-block codes;
- zig-zag placement, the MPEG-1 intra quantization matrix, and qscale handling;
- six 8x8 blocks per 16x16 macroblock in Cr, Cb, Y1, Y2, Y3, Y4 order;
- a bounded integer IDCT and clipping; and
- full-range YCbCr 4:2:0 conversion to top-down RGBA8888.

The supported files require version 2 only. Versions 3+, alternate dimensions,
inter frames, encoders, seeking, threading, and generic MPEG containers are out
of scope. The VLC table and transform constants should be derived from public
format specifications, with independent tests; copying FFmpeg implementation
or tables would retain an LGPL-derived-code question after the library itself
is removed.

### Stereo 4-bit CD-XA ADPCM decoder

The supported subset uses the standard five two-tap predictor filters, signed
4-bit residuals, per-channel two-sample history, saturation to signed 16-bit,
and the stereo nibble layout. Both 18,900 Hz (`coding_info == 0x05`) and 37,800
Hz (`coding_info == 0x01`) must be accepted. Mono, 8-bit samples, unsupported
coding-info bits, and invalid filters or shifts should be rejected initially.

The decoder should write interleaved stereo S16 directly. That matches
`MovieAudioChunk` and removes the present general resampler without replacing
it with another dependency.

## Wuffs integration boundary

Wuffs code is hermetic: it performs no file I/O and owns no dynamic memory.
The existing C++ layer will continue to own the raw movie vector, output
vectors, exceptions, and presentation scheduling. Generated C receives bounded
input/output slices and persistent decoder structs.

The dependency is pinned as `external/wuffs` at v0.3.4 commit
`ec71f9c6d829ca763fbbc1f7adecc30a89a8ed0a` under Apache-2.0. Sources live in
`src/stuntmaster/media/wuffs`; generation produces separate checked-in core,
STR, XA, and MDEC C translation units beside the small base-runtime adapter.
Normal builds require only C/C++. Developers with Go can regenerate and verify
all four files with:

```powershell
tools/generate_wuffs_codecs.ps1
tools/generate_wuffs_codecs.ps1 -Check
```

The package comprises `str.wuffs`, `xa.wuffs`, and `mdec.wuffs` alongside its
callable ABI marker. Its CMake library and focused CTests prove that the sources
can be regenerated independently, compiled as C, linked into C++, and called.
The C++
`media::StrDecoder` adapter performs the deterministic timing pre-pass, frame
assembly, event scheduling, and error translation.

## Compatibility contract

For every movie in the fingerprinted supported retail image, the replacement
must reproduce the retired FFmpeg path's observable `media::StrDecoder` output
exactly. The gate covers event type and order, dimensions, sample rates, buffer
sizes and contents, `hasAudio()`, `framesPerSecond()`, and every timestamp's
IEEE-754 bit pattern. Aggregate error statistics and visual tolerances are
diagnostics only; they do not establish drop-in compatibility.

Malformed or out-of-profile streams are the deliberate exception. The Wuffs
path validates the complete input envelope during `open()` and fails closed;
it does not emulate FFmpeg's version-dependent behavior of skipping damaged
sectors, returning partial output, or delaying an error until `next()`. This is
an input-contract and safety distinction, not permission to differ on any byte
shipped by the supported retail image. Reproducing permissive recovery would
broaden the accepted format beyond the inventoried game data and introduce
ambiguous decoder state without improving compatibility for that data.

Tests should therefore require exact retail output and deterministic rejection
of malformed inputs, but should not encode FFmpeg's error text, partial-output
count, or failure timing as a malformed-stream compatibility requirement.

## Verification strategy

- Unit-test sector parsing with synthetic valid and adversarial sectors,
  including every length/count overflow edge.
- Unit-test XA with independently constructed filter/shift/history vectors and
  both supported sample rates.
- Unit-test VLC, dequantization, IDCT, chroma expansion, and clipping as
  separable MDEC stages before testing complete frames.
- Differential-test decoded event counts, timestamps, audio samples, and RGBA
  output against the preserved FFmpeg 8.1.2 black-box reference. FFmpeg is an
  oracle for comparison, not a source to copy.
- Keep retail assets out of Git. Optional integration tests may consume the
  user-supplied fingerprinted disc and verify the inventory above.
- The fingerprinted disc-backed probe decodes all nine movie extents. Normal
  interactive playback is already confirmed for the startup trilogy; remaining
  call-site coverage is part of campaign validation, not codec-format coverage.

## Implementation status

All four implementation phases are complete: STR and XA, MDEC and integer
IDCT, direct RGBA/event integration, and removal of FFmpeg from discovery,
build, link, packaging, licensing, and SBOM paths. The disc-backed validation
tool records the former FFmpeg 8.1.2 aggregate reference without redistributing
retail bytes.

An independent red-team pass exercised malformed inputs and compared every
shipped movie with the retired decoder. The hardened MDEC path verifies the
decoded run-level word count, mandatory version-2 terminal marker, and zero
padding; all 15,077 retail frames satisfy those invariants. XA now validates a
whole sector before changing predictor history or output, so late corruption
cannot poison the next sector.

The strict retail verifier now matches the FFmpeg 8.1.2 reference exactly for
all nine movies: every RGBA and PCM payload, floating-point timestamp bit
pattern, frame-rate bit pattern, event tag and order, dimension, sample rate,
buffer size, event count, and `hasAudio()` result. This covers 15,077 video
frames and 9,465 XA sectors without a numeric or visual tolerance. The MDEC
path reproduces the reference integer IDCT, full-range YUV conversion, and
centered bilinear chroma expansion with edge replication.

In a warmed decode-only all-disc benchmark, the homegrown path took 21.74
seconds versus 6.53 seconds for FFmpeg 8.1.2 (3.33x slower). It averages 1.442
ms per frame, about 46x faster than the movies' authored playback rate. The
factored integer IDCT halves the general transform's multiply count; exact
chroma scaling and color conversion remain the largest optimization target.
