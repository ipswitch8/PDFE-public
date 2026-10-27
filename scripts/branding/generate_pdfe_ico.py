# -*- coding: utf-8 -*-
"""Regenerate src/app/pdfe.ico from red_logo.png.

Reproducibility: re-running this script on the same red_logo.png input
produces a functionally-equivalent ICO. PIL/Pillow's ICO encoder is
deterministic when given the same input pixels and same size list.

Aspect ratio: red_logo.png is 1408x768 (non-square). Windows shell
icons want square frames, so we letterbox the source onto a square
RGBA canvas with a transparent background — preserving the logo's
proportions and centering it. We do NOT crop (would clip the logo)
or stretch (would deform it).

Sizes embedded:
  Mandatory: 16, 32, 48, 256
  Best-effort: 20, 24, 40, 64, 128

  16  — Windows Explorer small icons + classic title-bar.
  20  — Some HiDPI taskbar paths (125% scaling).
  24  — Tooltip / accessory-bar icon.
  32  — Default Explorer + taskbar at 100%.
  40  — Taskbar at 125% scaling.
  48  — Large icons, default Explorer "large" view.
  64  — XP-era jumbo icons (still hit by some shell paths).
  128 — Vista+ extra-large.
  256 — Vista+ jumbo + thumbnail-quality at 200%+ scaling.

Usage:
    python scripts/branding/generate_pdfe_ico.py
    python scripts/branding/generate_pdfe_ico.py --verify
"""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

from PIL import Image

# Project layout: this script lives at scripts/branding/, repo root is two up.
REPO_ROOT = Path(__file__).resolve().parent.parent.parent
SOURCE_PNG = REPO_ROOT / "red_logo.png"
OUTPUT_ICO = REPO_ROOT / "src" / "app" / "pdfe.ico"

ICON_SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]


def letterbox_to_square(img: Image.Image) -> Image.Image:
    """Fit `img` onto a square RGBA canvas with a transparent background.
    The logo's aspect ratio is preserved; the shorter dimension is
    centered."""
    if img.mode != "RGBA":
        img = img.convert("RGBA")
    w, h = img.size
    side = max(w, h)
    canvas = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    canvas.paste(img, ((side - w) // 2, (side - h) // 2), img)
    return canvas


def generate(source: Path, output: Path) -> None:
    if not source.exists():
        raise FileNotFoundError(f"Source PNG missing: {source}")
    src = Image.open(source)
    print(f"[generate_pdfe_ico] source: {source} ({src.size}, {src.mode})")
    square = letterbox_to_square(src)

    # Pillow's ICO encoder picks the embedded sizes from the `sizes`
    # kwarg when the input is at least as large as the largest target.
    # We feed it the squared canvas (max(width, height) = 1408 px) so
    # all requested sizes are valid downscales. LANCZOS is the default
    # for ICO encoding; pass it explicitly via --resample on Pillow's
    # newer API. To keep the script Pillow-9.x-compatible we let the
    # encoder pick.
    output.parent.mkdir(parents=True, exist_ok=True)
    sizes_tuple = [(s, s) for s in ICON_SIZES]
    square.save(
        output,
        format="ICO",
        sizes=sizes_tuple,
    )
    print(f"[generate_pdfe_ico] wrote: {output}")


def verify(output: Path) -> int:
    """Inspect the ICO directory and confirm it lists the expected sizes.
    Returns 0 on success, non-zero on mismatch."""
    if not output.exists():
        print(f"[generate_pdfe_ico] ERROR: {output} does not exist", file=sys.stderr)
        return 1

    with output.open("rb") as f:
        header = f.read(6)
        if len(header) < 6:
            print(f"[generate_pdfe_ico] ERROR: {output} too short", file=sys.stderr)
            return 1
        reserved, kind, count = struct.unpack("<HHH", header)
        if reserved != 0 or kind != 1:
            print(
                f"[generate_pdfe_ico] ERROR: {output} not an ICO "
                f"(reserved={reserved}, type={kind})",
                file=sys.stderr,
            )
            return 1
        widths = []
        for _ in range(count):
            entry = f.read(16)
            if len(entry) < 16:
                print("[generate_pdfe_ico] ERROR: truncated ICO directory",
                      file=sys.stderr)
                return 1
            w = entry[0]
            h = entry[1]
            # Width/height of 0 means 256.
            widths.append(256 if w == 0 else w)
            del h  # height parallel; we report on width only

    widths.sort()
    print(f"[generate_pdfe_ico] {output.name} contains {count} icons: {widths}")

    mandatory = {16, 32, 48, 256}
    missing = mandatory - set(widths)
    if missing:
        print(f"[generate_pdfe_ico] FAIL: missing mandatory sizes {sorted(missing)}",
              file=sys.stderr)
        return 1
    print("[generate_pdfe_ico] verify OK: all mandatory sizes present")
    return 0


def alpha_verify(output: Path,
                 small_size_threshold: int = 32,
                 max_ring_opaque_pct: float = 25.0) -> int:
    """Inspect every embedded ICO frame and confirm:
      * mode is or is convertible to RGBA
      * alpha range covers at least one transparent pixel (alpha_min == 0)
      * for frames at sizes <= `small_size_threshold`, the outermost
        1-pixel ring has < `max_ring_opaque_pct`% fully-opaque pixels
        (i.e., no gray flood-fill edges from the pre-Phase-1 source)

    Prints a per-frame report. Returns 0 if every frame passes,
    non-zero otherwise.
    """
    if not output.exists():
        print(f"[generate_pdfe_ico] ERROR: {output} does not exist",
              file=sys.stderr)
        return 1

    ico = Image.open(output)
    ico.load()
    sizes = sorted(ico.ico.sizes())
    rc = 0
    for s in sizes:
        frame = ico.ico.getimage(s)
        if frame.mode != "RGBA":
            frame = frame.convert("RGBA")
        alpha = frame.split()[-1]
        amin, amax = alpha.getextrema()
        azero = sum(1 for v in alpha.getdata() if v == 0)
        total = frame.width * frame.height

        # Outer 1-pixel ring opacity ratio.
        px = frame.load()
        ring_total = 2 * frame.width + 2 * max(0, frame.height - 2)
        ring_opaque = 0
        for x in range(frame.width):
            if px[x, 0][3] == 255:
                ring_opaque += 1
            if px[x, frame.height - 1][3] == 255:
                ring_opaque += 1
        for y in range(1, frame.height - 1):
            if px[0, y][3] == 255:
                ring_opaque += 1
            if px[frame.width - 1, y][3] == 255:
                ring_opaque += 1
        ring_pct = ring_opaque * 100.0 / ring_total if ring_total else 0.0

        status = "OK"
        if amin != 0:
            status = "FAIL (no alpha=0 pixel)"
            rc = 1
        elif (s[0] <= small_size_threshold
              and ring_pct >= max_ring_opaque_pct):
            status = (f"FAIL (small-size ring opaque {ring_pct:.1f}% "
                      f">= {max_ring_opaque_pct}%)")
            rc = 1
        print(f"  {s[0]}x{s[1]}: alpha=[{amin},{amax}] "
              f"alpha_zero={azero} ({azero * 100 / total:.1f}%) "
              f"ring_opaque={ring_opaque}/{ring_total} "
              f"({ring_pct:.1f}%) -- {status}")
    if rc == 0:
        print("[generate_pdfe_ico] alpha-verify OK: all frames carry "
              "alpha and small-size rings are clean.")
    return rc


def main(argv: list[str]) -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--verify", action="store_true",
                   help="Inspect the existing ICO without regenerating.")
    p.add_argument("--alpha-verify", action="store_true",
                   help=("Inspect every embedded ICO frame for alpha "
                         "presence and small-size outer-ring opacity. "
                         "Use after regeneration from a transparent "
                         "source PNG to confirm the .ico carries the "
                         "transparency end-to-end."))
    p.add_argument("--source", type=Path, default=SOURCE_PNG)
    p.add_argument("--output", type=Path, default=OUTPUT_ICO)
    args = p.parse_args(argv)

    if not args.verify and not args.alpha_verify:
        generate(args.source, args.output)
    rc = verify(args.output)
    if rc == 0 and args.alpha_verify:
        rc = alpha_verify(args.output)
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
