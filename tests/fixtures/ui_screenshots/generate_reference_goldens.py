# -*- coding: utf-8 -*-
"""Deterministic TRUSTED-reference golden generator (R-29 / C-08).

A golden must be a committed, known-correct baseline produced by a trusted
reference — NOT the current run's own capture promoted on first sight (which
makes every future render, mirrored/half-size/blanked/wrong-icon, pass forever).

This script IS such a trusted reference: every image below is drawn from a
fully-specified, deterministic spec (no screenshot, no RNG, no timestamp), so
any machine regenerates byte-identical output and a reviewer can verify
correctness by reading the spec.

C-08 (coverage-expansion phase-3) — BREADTH
-------------------------------------------
The remediation committed exactly ONE correctness golden (``save_icon``). This
generator now emits a SET of committed goldens covering distinct headless render
states, each drawn deliberately ASYMMETRIC and multi-coloured so the classic
render-defect classes are all detectable against it:
  * horizontal MIRROR  → left/right content swaps, arrows/tabs flip
  * HALF-SIZE render    → upscaling to compare blurs every edge
  * WRONG-COLOUR / icon → the palette changes while geometry is intact

Committed goldens (subsystem ``reference``):
  * save_icon      — the original toolbar-icon reference (unchanged spec)
  * open_icon      — a second, visually distinct toolbar icon (folder + arrow)
  * page_region    — a synthetic rendered-PDF-page region (header + text lines +
                     margin marker + page-number box)
  * dialog_control — a simple dialog control paint (checkbox + primary button +
                     focus ring)

Regenerate ALL committed goldens (deliberate maintenance only):
    python tests/fixtures/ui_screenshots/generate_reference_goldens.py
"""

from __future__ import annotations

from pathlib import Path
from typing import Callable, Dict

from PIL import Image, ImageDraw

_SUBSYSTEM = "reference"

# ---------------------------------------------------------------------------
# save_icon — original reference spec (kept byte-stable; imported by tests)
# ---------------------------------------------------------------------------

_W, _H = 128, 128
_LEFT_FILL = (198, 40, 40)     # deep red  — left half
_RIGHT_FILL = (13, 71, 161)    # deep blue — right half
_ARROW_FILL = (255, 214, 0)    # amber arrow, points RIGHT (asymmetry anchor)
_BAR_FILL = (0, 0, 0)          # black baseline bar
_DISK_FILL = (250, 250, 250)   # near-white disk, upper-left (breaks symmetry)


def render_reference() -> Image.Image:
    """Return the canonical save_icon reference image (the correct render)."""
    img = Image.new("RGB", (_W, _H), color=_LEFT_FILL)
    d = ImageDraw.Draw(img)
    d.rectangle([_W // 2, 0, _W - 1, _H - 1], fill=_RIGHT_FILL)
    d.polygon(
        [(28, 54), (78, 54), (78, 40), (104, 64), (78, 88), (78, 74), (28, 74)],
        fill=_ARROW_FILL,
    )
    d.ellipse([16, 12, 44, 40], fill=_DISK_FILL)
    d.rectangle([0, _H - 16, _W - 1, _H - 1], fill=_BAR_FILL)
    return img


# ---------------------------------------------------------------------------
# open_icon — a second, visually distinct toolbar icon (folder + up arrow)
# ---------------------------------------------------------------------------

def render_open_icon() -> Image.Image:
    """A distinct toolbar icon: a manila folder with a tab in the UPPER-LEFT and
    an amber UP-arrow. Asymmetric horizontally (tab left) and vertically (arrow
    head up), so a mirror or flip is unambiguously detectable."""
    img = Image.new("RGB", (_W, _H), color=(245, 245, 245))
    d = ImageDraw.Draw(img)
    # Folder tab (upper-left) then the folder body — manila / brown palette.
    _TAB = (255, 179, 0)
    _BODY = (255, 202, 40)
    _EDGE = (120, 80, 0)
    d.rectangle([16, 30, 60, 46], fill=_TAB, outline=_EDGE)
    d.rectangle([16, 42, 112, 100], fill=_BODY, outline=_EDGE)
    # Amber up-arrow rising out of the folder (points UP → vertical asymmetry).
    d.polygon(
        [(64, 52), (84, 74), (72, 74), (72, 92), (56, 92), (56, 74), (44, 74)],
        fill=(21, 101, 192),
    )
    # Small near-black notch bottom-right to break both axes of symmetry.
    d.rectangle([98, 90, 110, 100], fill=(20, 20, 20))
    return img


# ---------------------------------------------------------------------------
# page_region — a synthetic rendered-PDF-page region
# ---------------------------------------------------------------------------

_PW, _PH = 160, 200


def render_page_region() -> Image.Image:
    """A synthetic rendered-page region: white page, a coloured header block near
    the top-left, left-aligned black text lines of DECREASING length, a red
    left-margin marker, and a page-number box in the BOTTOM-RIGHT. Strongly
    asymmetric so mirror/flip and colour regressions are all detectable."""
    img = Image.new("RGB", (_PW, _PH), color=(255, 255, 255))
    d = ImageDraw.Draw(img)
    # Page border.
    d.rectangle([0, 0, _PW - 1, _PH - 1], outline=(180, 180, 180))
    # Header block (upper-left), navy.
    d.rectangle([12, 12, 96, 30], fill=(13, 71, 161))
    # Body text lines: left-aligned, decreasing width (ragged-right → asymmetry).
    line_widths = [130, 120, 138, 96, 128, 70]
    y = 44
    for w in line_widths:
        d.rectangle([12, y, 12 + w, y + 6], fill=(30, 30, 30))
        y += 16
    # Red left-margin marker (only on the left edge).
    d.rectangle([4, 44, 8, 44 + 6 * 16], fill=(198, 40, 40))
    # Page-number box, bottom-right.
    d.rectangle([_PW - 34, _PH - 24, _PW - 10, _PH - 8], fill=(0, 128, 0))
    return img


# ---------------------------------------------------------------------------
# dialog_control — a simple dialog control paint
# ---------------------------------------------------------------------------

_DW, _DH = 200, 120


def render_dialog_control() -> Image.Image:
    """A simple dialog control paint: a grey dialog surface, a checked checkbox
    in the UPPER-LEFT, and a blue primary button on the RIGHT with a green focus
    ring and an off-centre white label bar. Asymmetric on both axes."""
    img = Image.new("RGB", (_DW, _DH), color=(236, 236, 236))
    d = ImageDraw.Draw(img)
    # Checkbox (upper-left) with a check mark.
    d.rectangle([16, 16, 36, 36], fill=(255, 255, 255), outline=(60, 60, 60))
    d.line([(19, 26), (25, 32)], fill=(0, 140, 0), width=3)
    d.line([(25, 32), (34, 18)], fill=(0, 140, 0), width=3)
    # A short label line to the right of the checkbox.
    d.rectangle([44, 22, 120, 30], fill=(90, 90, 90))
    # Primary button (right side, lower) with a green focus ring.
    bx0, by0, bx1, by1 = 116, 74, 184, 104
    d.rectangle([bx0 - 3, by0 - 3, bx1 + 3, by1 + 3], outline=(0, 160, 0))
    d.rectangle([bx0, by0, bx1, by1], fill=(21, 101, 192))
    # Off-centre white label bar inside the button (nearer its left edge).
    d.rectangle([bx0 + 8, by0 + 11, bx0 + 40, by0 + 19], fill=(255, 255, 255))
    return img


# ---------------------------------------------------------------------------
# Generic defect transforms (apply to ANY reference image)
# ---------------------------------------------------------------------------

def mirror_of(img: Image.Image) -> Image.Image:
    """DEFECT: horizontal mirror. Any left/right asymmetry flips."""
    return img.transpose(Image.FLIP_LEFT_RIGHT)


def half_size_of(img: Image.Image) -> Image.Image:
    """DEFECT: rendered at half resolution. compare_images upsamples it back to
    golden dims (LANCZOS), blurring every sharp edge."""
    return img.resize((max(1, img.width // 2), max(1, img.height // 2)),
                      Image.BILINEAR)


def wrong_color_of(img: Image.Image) -> Image.Image:
    """DEFECT: correct geometry, WRONG palette — swap the R and B channels so
    every non-grey pixel changes hue while shapes stay put (a theme/icon colour
    regression)."""
    r, g, b = img.convert("RGB").split()
    return Image.merge("RGB", (b, g, r))


# save_icon-specific legacy defect names still imported by test_golden_no_selfseed
def render_mirrored() -> Image.Image:
    return mirror_of(render_reference())


def render_half_size() -> Image.Image:
    return half_size_of(render_reference())


def render_wrong_icon() -> Image.Image:
    """save_icon-specific colour swap (kept for the original oracle test)."""
    img = render_reference()
    px = img.load()
    for y in range(_H):
        for x in range(_W):
            r, g, b = px[x, y]
            if (r, g, b) == _LEFT_FILL:
                px[x, y] = (40, 140, 40)
            elif (r, g, b) == _RIGHT_FILL:
                px[x, y] = (40, 90, 140)
    return img


# ---------------------------------------------------------------------------
# Registry of committed correctness goldens (subsystem "reference")
# ---------------------------------------------------------------------------

REFERENCES: Dict[str, Callable[[], Image.Image]] = {
    "save_icon": render_reference,
    "open_icon": render_open_icon,
    "page_region": render_page_region,
    "dialog_control": render_dialog_control,
}

# The generic defect classes every committed golden must REJECT.
DEFECTS: Dict[str, Callable[[Image.Image], Image.Image]] = {
    "mirrored": mirror_of,
    "half_size": half_size_of,
    "wrong_color": wrong_color_of,
}


def baseline_dir() -> Path:
    return Path(__file__).parent / "baseline" / _SUBSYSTEM


def golden_dest(test: str = "save_icon") -> Path:
    return baseline_dir() / f"{test}.png"


def main() -> None:
    dest_dir = baseline_dir()
    dest_dir.mkdir(parents=True, exist_ok=True)
    for name, render in REFERENCES.items():
        dest = dest_dir / f"{name}.png"
        render().save(str(dest))
        print(f"wrote committed reference golden: {dest}")


if __name__ == "__main__":
    main()
