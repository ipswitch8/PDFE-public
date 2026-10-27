# -*- coding: utf-8 -*-
"""Crop logo PNGs to content + small border and replace flood-fill
background with transparency.

Pipeline:
  1. Open the source PNG as RGBA.
  2. Sample 8 reference points around the perimeter (4 corners + 4
     edge midpoints) to estimate the dominant background color.
  3. Build a binary "is-background" mask using Manhattan-distance
     tolerance against the median sample.
  4. Compute the content bounding box from the inverse of that mask
     and pad it by a small border (default 6 px) clipped to image
     bounds.
  5. Crop to that bbox.
  6. Replace any remaining background-equivalent pixels in the
     cropped image with alpha=0.
  7. Write the result back to disk in PNG / RGBA.

Idempotency:
  Re-running on the script's own output is a hard no-op. The
  perimeter_has_transparency() guard at the top of process()
  inspects 8 perimeter sample points and short-circuits if any of
  them already has alpha=0 — printing "already processed" and
  leaving the file untouched. This guarantees byte-identical
  re-runs. The trade-off is that a *new* logo PNG which happens
  to have a transparent perimeter pixel (e.g., anti-aliased edge
  in a from-scratch export) is treated as already-processed and
  skipped silently. If you want to force re-processing of such a
  file, pre-fill the alpha channel with 255 before running, or
  invoke the script's lower-level helpers directly.

Tolerance:
  The default tolerance is 16 (per-channel). The criterion that
  drove the original spec suggested ~10, but a sweep against the
  three repo logos showed:
    tol 10  -> bbox includes a false-positive at (0,386) on
               color_logo because of an outlier (195,195,195) pixel
               sitting 12 above the (207,207,207) bg sample.
    tol 14-22 -> stable plateau; bbox = (487,74,1376,736).
    tol >=26 -> bbox shrinks dramatically (logo gradient pixels
                start being misclassified as bg).
  16 is the smallest tolerance that absorbs the perimeter outlier
  noise without eating into logo content. Override with --tolerance
  if your asset has a different noise profile.

Usage:
    python scripts/branding/crop_and_transparent.py
        # Apply default settings to all three repo-root logos.

    python scripts/branding/crop_and_transparent.py --verify
        # Print stats for each PNG without modifying.

    python scripts/branding/crop_and_transparent.py \\
        --input C:/path/foo.png --output C:/path/foo.png \\
        --tolerance 12 --border 8
        # Single-file mode with custom thresholds.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import Iterable, Tuple

from PIL import Image

# Project layout: scripts/branding/ is two levels under repo root.
REPO_ROOT = Path(__file__).resolve().parent.parent.parent

# Default targets — the three repo-root logo PNGs.
DEFAULT_TARGETS = [
    REPO_ROOT / "color_logo.png",
    REPO_ROOT / "itwerx_logo_small.png",
    REPO_ROOT / "red_logo.png",
]

DEFAULT_TOLERANCE = 16  # per-channel (R,G,B) tolerance, 0..255
                        # Empirically chosen against the three repo
                        # logos: tol 14-22 produces a stable bbox; tol
                        # ≥26 starts cutting into logo content. 16
                        # absorbs JPG-style perimeter noise (diff up to
                        # ~12) without risking the logo edges.
DEFAULT_BORDER = 6      # px padding around content bbox


def sample_perimeter(img: Image.Image) -> Tuple[int, int, int]:
    """Return the median perimeter color (R, G, B) of an RGBA image.

    Samples 8 perimeter points: 4 corners and 4 edge midpoints. The
    median is more robust to a stray dark pixel in one corner than
    a simple mean would be, and tolerates one or two non-background
    samples (e.g., when the logo touches an edge).
    """
    w, h = img.size
    px = img.load()
    points = [
        (0, 0),
        (w - 1, 0),
        (0, h - 1),
        (w - 1, h - 1),
        (w // 2, 0),
        (w // 2, h - 1),
        (0, h // 2),
        (w - 1, h // 2),
    ]
    samples = [px[x, y] for (x, y) in points]
    rs = sorted(s[0] for s in samples)
    gs = sorted(s[1] for s in samples)
    bs = sorted(s[2] for s in samples)
    n = len(samples)
    return (rs[n // 2], gs[n // 2], bs[n // 2])


def perimeter_has_transparency(img: Image.Image) -> bool:
    """True if any of the 8 perimeter sample points already has alpha=0.

    Used as the idempotency guard: if the image has been through this
    script before, the perimeter is alpha=0 and we should treat the
    file as already processed (no-op) rather than re-running the
    bg-detection pipeline against a normalized (0,0,0,0) frame.
    """
    w, h = img.size
    px = img.load()
    points = [
        (0, 0),
        (w - 1, 0),
        (0, h - 1),
        (w - 1, h - 1),
        (w // 2, 0),
        (w // 2, h - 1),
        (0, h // 2),
        (w - 1, h // 2),
    ]
    return any(px[x, y][3] == 0 for (x, y) in points)


def is_bg(rgba: Tuple[int, int, int, int],
          bg: Tuple[int, int, int],
          tol: int) -> bool:
    """Pixel test: is this pixel within `tol` of the background color?

    Pre-existing alpha=0 pixels are always treated as background — this
    keeps the script idempotent on its own output.
    """
    r, g, b, a = rgba
    if a == 0:
        return True
    return (abs(r - bg[0]) <= tol
            and abs(g - bg[1]) <= tol
            and abs(b - bg[2]) <= tol)


def content_bbox(img: Image.Image,
                 bg: Tuple[int, int, int],
                 tol: int) -> Tuple[int, int, int, int]:
    """Return (left, top, right, bottom) bbox of non-background content.

    Returns the full image bounds if nothing qualifies as content, so
    a follow-on crop is still safe (no zero-size result).
    """
    w, h = img.size
    px = img.load()
    minx, miny, maxx, maxy = w, h, -1, -1
    for y in range(h):
        for x in range(w):
            if not is_bg(px[x, y], bg, tol):
                if x < minx:
                    minx = x
                if x > maxx:
                    maxx = x
                if y < miny:
                    miny = y
                if y > maxy:
                    maxy = y
    if maxx < 0:
        # No content found; return whole image as a safe no-op bbox.
        return (0, 0, w, h)
    return (minx, miny, maxx + 1, maxy + 1)


def pad_bbox(bbox: Tuple[int, int, int, int],
             size: Tuple[int, int],
             border: int) -> Tuple[int, int, int, int]:
    """Expand bbox by `border` pixels in each direction, clipped to image."""
    l, t, r, b = bbox
    w, h = size
    return (
        max(0, l - border),
        max(0, t - border),
        min(w, r + border),
        min(h, b + border),
    )


def make_transparent(img: Image.Image,
                     bg: Tuple[int, int, int],
                     tol: int) -> Image.Image:
    """Return a copy of `img` (RGBA) with background-equivalent pixels
    forced to alpha=0. Foreground pixels keep their original alpha.

    Alpha=0 pixels have their RGB normalized to (0, 0, 0) so the result
    is bit-stable across re-runs — otherwise the perimeter sampler can
    drift by ±1 between passes when the source has gradient noise.
    """
    out = img.copy()
    px = out.load()
    w, h = out.size
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            if a == 0:
                px[x, y] = (0, 0, 0, 0)
            elif (abs(r - bg[0]) <= tol
                  and abs(g - bg[1]) <= tol
                  and abs(b - bg[2]) <= tol):
                px[x, y] = (0, 0, 0, 0)
    return out


def alpha_histogram(img: Image.Image) -> Tuple[int, int, int]:
    """(min_alpha, max_alpha, alpha_zero_count) for an RGBA image."""
    a = img.split()[-1]
    extrema = a.getextrema()
    zero_count = sum(1 for v in a.getdata() if v == 0)
    return (extrema[0], extrema[1], zero_count)


def process(src_path: Path,
            dst_path: Path,
            tolerance: int,
            border: int,
            verify_only: bool) -> int:
    """Crop + transparent one PNG. Returns 0 on success, 1 on failure."""
    if not src_path.exists():
        print(f"[crop_and_transparent] MISSING: {src_path}", file=sys.stderr)
        return 1

    img = Image.open(src_path).convert("RGBA")
    w, h = img.size

    # Idempotency guard: a file that already has transparent perimeter
    # pixels was produced by a previous run of this script. Re-running
    # would corrupt it because the bg sampler would pick up the
    # normalized (0,0,0) RGB of the alpha=0 region and flag dark
    # foreground pixels as background.
    if perimeter_has_transparency(img):
        amin, amax, azero = alpha_histogram(img)
        print(f"[crop_and_transparent] {src_path.name}: "
              f"already processed ({w}x{h} alpha=[{amin},{amax}] "
              f"alpha_zero={azero}); no-op.")
        return 0

    bg = sample_perimeter(img)
    bbox = content_bbox(img, bg, tolerance)
    padded = pad_bbox(bbox, (w, h), border)

    print(f"[crop_and_transparent] {src_path.name}: "
          f"orig={w}x{h} bg=({bg[0]},{bg[1]},{bg[2]}) "
          f"content_bbox={bbox} padded_bbox={padded}")

    if verify_only:
        amin, amax, azero = alpha_histogram(img)
        print(f"  current alpha: min={amin} max={amax} zero_count={azero}")
        return 0

    cropped = img.crop(padded)
    transparent = make_transparent(cropped, bg, tolerance)

    amin, amax, azero = alpha_histogram(transparent)
    cw, ch = transparent.size
    print(f"  -> {dst_path.name}: new={cw}x{ch} "
          f"alpha=[{amin},{amax}] alpha_zero={azero}")

    dst_path.parent.mkdir(parents=True, exist_ok=True)
    transparent.save(dst_path, format="PNG", optimize=True)
    return 0


def run_targets(targets: Iterable[Path],
                tolerance: int,
                border: int,
                verify_only: bool) -> int:
    rc = 0
    for path in targets:
        rc |= process(path, path, tolerance, border, verify_only)
    return rc


def main(argv: list[str]) -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--input", type=Path,
                   help="Single input PNG (default: process all 3 logos).")
    p.add_argument("--output", type=Path,
                   help="Single output PNG (default: overwrite input).")
    p.add_argument("--tolerance", type=int, default=DEFAULT_TOLERANCE,
                   help=(f"Per-channel RGB tolerance (default "
                         f"{DEFAULT_TOLERANCE}). The original phase-1 spec "
                         "suggested ~10; raised to 16 after a tolerance "
                         "sweep showed (0,386) on color_logo is a "
                         "perimeter outlier with diff=12 from the bg "
                         "sample. See module docstring for the full "
                         "tolerance vs bbox plateau analysis."))
    p.add_argument("--border", type=int, default=DEFAULT_BORDER,
                   help=f"Px padding around content bbox (default {DEFAULT_BORDER}).")
    p.add_argument("--verify", action="store_true",
                   help="Print stats only; do not modify files.")
    args = p.parse_args(argv)

    if args.input is not None:
        out = args.output if args.output is not None else args.input
        return process(args.input, out, args.tolerance, args.border,
                       args.verify)

    return run_targets(DEFAULT_TARGETS, args.tolerance, args.border,
                       args.verify)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
