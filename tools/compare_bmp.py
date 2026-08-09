#!/usr/bin/env python3
"""Compare two BMP frame captures for presentation-path validation.

Stage 2 of docs/PRESENTATION_REDESIGN.md validates the persistent-framebuffer
present path against an earlier reference capture.
Captures come out as BMPs at possibly different resolutions and vertical
orientations (PsyCross render-target captures vs the vertically-inverted
software-rendered reference), so this tool:

  * loads both images (PIL handles BMP orientation/bit-depth),
  * if sizes differ, resamples the second to the first (nearest, reported),
  * tries both vertical orientations and keeps the better-aligned one,
  * reports mean luma diff, RMS, max, and % of pixels within a luma tolerance,
  * optionally writes an amplified heatmap of the difference.

Exit code is 0 when mean luma diff <= --fail-mean and within-tolerance
fraction >= --fail-within, else 1, so it can gate a scripted campaign.
"""
from __future__ import annotations

import argparse
import sys

import numpy as np
from PIL import Image


def load_luma_rgb(path: str) -> tuple[np.ndarray, np.ndarray, tuple[int, int]]:
    img = Image.open(path).convert("RGB")
    rgb = np.asarray(img, dtype=np.float64)
    # Rec.601 luma, matching how a viewer weights the channels.
    luma = rgb @ np.array([0.299, 0.587, 0.114])
    return luma, rgb, img.size  # size = (w, h)


def resample_to(rgb: np.ndarray, luma: np.ndarray, size_hw: tuple[int, int]):
    h, w = size_hw
    img = Image.fromarray(rgb.astype(np.uint8), "RGB").resize(
        (w, h), Image.NEAREST
    )
    r = np.asarray(img, dtype=np.float64)
    return (r @ np.array([0.299, 0.587, 0.114])), r


def stats(a_luma: np.ndarray, b_luma: np.ndarray, tol: float) -> dict:
    diff = np.abs(a_luma - b_luma)
    return {
        "mean": float(diff.mean()),
        "rms": float(np.sqrt((diff ** 2).mean())),
        "max": float(diff.max()),
        "within": float((diff <= tol).mean()),
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("a", help="reference BMP")
    ap.add_argument("b", help="candidate BMP (e.g. persistent)")
    ap.add_argument("--tol", type=float, default=8.0,
                    help="per-pixel luma tolerance for the within-%% metric")
    ap.add_argument("--heatmap", help="write an amplified abs-diff heatmap here")
    ap.add_argument("--amp", type=float, default=4.0, help="heatmap amplification")
    ap.add_argument("--no-autoflip", action="store_true",
                    help="do not try the vertically flipped orientation")
    ap.add_argument("--fail-mean", type=float, default=None,
                    help="exit 1 if mean luma diff exceeds this")
    ap.add_argument("--fail-within", type=float, default=None,
                    help="exit 1 if within-tolerance fraction below this")
    args = ap.parse_args()

    a_luma, a_rgb, a_size = load_luma_rgb(args.a)
    b_luma, b_rgb, b_size = load_luma_rgb(args.b)

    note = ""
    if a_size != b_size:
        note = f" (resampled b {b_size}->{a_size} nearest)"
        b_luma, b_rgb = resample_to(b_rgb, b_luma, a_luma.shape)

    best = stats(a_luma, b_luma, args.tol)
    best_flip = False
    if not args.no_autoflip:
        flipped = stats(a_luma, np.flipud(b_luma), args.tol)
        if flipped["mean"] < best["mean"]:
            best, best_flip = flipped, True
            b_luma = np.flipud(b_luma)
            b_rgb = np.flipud(b_rgb)

    print(f"A: {args.a}  {a_size[0]}x{a_size[1]}")
    print(f"B: {args.b}  {b_size[0]}x{b_size[1]}{note}")
    if best_flip:
        print("orientation: B vertically flipped for best alignment")
    print(f"mean luma diff : {best['mean']:.3f} / 255")
    print(f"rms  luma diff : {best['rms']:.3f}")
    print(f"max  luma diff : {best['max']:.0f}")
    print(f"within +/-{args.tol:g}   : {best['within'] * 100:.2f}% of pixels")

    if args.heatmap:
        d = np.abs(a_rgb - b_rgb) * args.amp
        Image.fromarray(np.clip(d, 0, 255).astype(np.uint8), "RGB").save(
            args.heatmap
        )
        print(f"heatmap: {args.heatmap} (x{args.amp:g})")

    ok = True
    if args.fail_mean is not None and best["mean"] > args.fail_mean:
        ok = False
    if args.fail_within is not None and best["within"] < args.fail_within:
        ok = False
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
