# -*- coding: utf-8 -*-
"""Image-difference utilities.

Provides:
- ``ssim(a, b)`` Structural Similarity Index over the full image.
  Uses scikit-image when available; otherwise a hand-rolled
  implementation of the Wang & Bovik 2004 algorithm.
- ``pixel_diff(a, b, threshold_per_pixel)`` per-pixel L1-channel
  comparison; returns the fraction of pixels exceeding the threshold
  and a visualization image highlighting the differences.
- ``compare(actual, baseline, ssim_threshold, pixel_threshold)``
  high-level pass/fail compare with both metrics.
- ``save_diff_visualization(actual, baseline, out_path)`` writes a
  three-up actual | baseline | diff PNG for failure triage.

All inputs are PIL.Image.Image; both inputs are converted to common
dimensions (smaller one upscaled with bilinear) before comparison so
windows with mild non-determinism in non-client area sizing still
compare cleanly.
"""

from __future__ import annotations

from pathlib import Path
from typing import Tuple

import numpy as np
from PIL import Image, ImageDraw, ImageFont

# Try scikit-image first; fall back to a hand-rolled SSIM implementation.
_HAVE_SKIMAGE = False
try:
    from skimage.metrics import structural_similarity as _sk_ssim  # type: ignore
    _HAVE_SKIMAGE = True
except Exception:  # noqa: BLE001
    _HAVE_SKIMAGE = False


# ---------------------------------------------------------------------------
# SSIM
# ---------------------------------------------------------------------------

def ssim(a: Image.Image, b: Image.Image) -> float:
    """Compute SSIM on the luminance channel of two images.

    If sizes differ, the smaller image is bilinearly resized to match.
    Returns a scalar in [-1, 1] where 1 is identity.
    """
    a_arr, b_arr = _coerce_pair_to_gray(a, b)
    if _HAVE_SKIMAGE:
        # data_range is 255 because we keep uint8.
        return float(_sk_ssim(a_arr, b_arr, data_range=255))
    return float(_ssim_wang_bovik(a_arr, b_arr))


def pixel_diff(
    a: Image.Image,
    b: Image.Image,
    threshold_per_pixel: int = 5,
) -> Tuple[float, Image.Image]:
    """Per-pixel difference (max channel L1) above ``threshold_per_pixel``.

    Returns
    -------
    fraction : float  in [0, 1]
    diff_img : PIL.Image  red highlights where pixels differ
    """
    a_rgb, b_rgb = _coerce_pair_to_rgb(a, b)
    aa = np.asarray(a_rgb, dtype=np.int16)
    bb = np.asarray(b_rgb, dtype=np.int16)
    diff = np.abs(aa - bb).max(axis=-1).astype(np.uint8)
    mask = diff > threshold_per_pixel
    fraction = float(mask.mean()) if mask.size else 0.0

    # Visualization: red overlay on the actual where pixels differ.
    vis = aa.astype(np.uint8).copy()
    vis[mask] = (255, 0, 0)
    diff_img = Image.fromarray(vis, mode="RGB")
    return fraction, diff_img


def compare(
    actual: Image.Image,
    baseline: Image.Image,
    ssim_threshold: float = 0.95,
    pixel_threshold: float = 0.02,
) -> dict:
    """Run both SSIM and pixel-fraction checks.

    A result is a "pass" iff SSIM >= ``ssim_threshold`` *and*
    pixel-difference fraction <= ``pixel_threshold``.

    Returns a dict with keys: pass, ssim, pixel_fraction, diff_image.
    """
    s = ssim(actual, baseline)
    frac, diff_img = pixel_diff(actual, baseline)
    ok = (s >= ssim_threshold) and (frac <= pixel_threshold)
    return {
        "pass": bool(ok),
        "ssim": float(s),
        "pixel_fraction": float(frac),
        "diff_image": diff_img,
    }


def save_diff_visualization(
    actual: Image.Image,
    baseline: Image.Image,
    out_path: Path,
) -> Path:
    """Save a side-by-side actual | baseline | diff image at ``out_path``."""
    out_path = Path(out_path)
    out_path.parent.mkdir(parents=True, exist_ok=True)

    actual_rgb, baseline_rgb = _coerce_pair_to_rgb(actual, baseline)
    _frac, diff_img = pixel_diff(actual_rgb, baseline_rgb)

    w, h = actual_rgb.size
    label_h = 24
    panel_w = w
    panel_h = h + label_h
    canvas = Image.new("RGB", (panel_w * 3, panel_h), color=(40, 40, 40))

    canvas.paste(actual_rgb, (0, label_h))
    canvas.paste(baseline_rgb, (panel_w, label_h))
    canvas.paste(diff_img, (panel_w * 2, label_h))

    draw = ImageDraw.Draw(canvas)
    try:
        font = ImageFont.truetype("arial.ttf", 14)
    except (OSError, IOError):
        font = ImageFont.load_default()

    draw.text((6, 4), "actual",   fill=(255, 255, 255), font=font)
    draw.text((panel_w + 6, 4), "baseline", fill=(255, 255, 255), font=font)
    draw.text((panel_w * 2 + 6, 4), "diff (red = changed)",
              fill=(255, 200, 200), font=font)

    canvas.save(str(out_path))
    return out_path


# ---------------------------------------------------------------------------
# Internals
# ---------------------------------------------------------------------------

def _coerce_pair_to_rgb(
    a: Image.Image, b: Image.Image
) -> Tuple[Image.Image, Image.Image]:
    """Convert both images to RGB and resize the larger one down to the
    smaller's dimensions if they differ.  Resizing-down rather than up
    avoids creating synthetic high-frequency content that would inflate
    SSIM scores.
    """
    a = a.convert("RGB")
    b = b.convert("RGB")
    if a.size != b.size:
        target_w = min(a.size[0], b.size[0])
        target_h = min(a.size[1], b.size[1])
        a = a.resize((target_w, target_h), Image.BILINEAR)
        b = b.resize((target_w, target_h), Image.BILINEAR)
    return a, b


def _coerce_pair_to_gray(
    a: Image.Image, b: Image.Image
) -> Tuple[np.ndarray, np.ndarray]:
    a_rgb, b_rgb = _coerce_pair_to_rgb(a, b)
    a_arr = np.asarray(a_rgb.convert("L"), dtype=np.uint8)
    b_arr = np.asarray(b_rgb.convert("L"), dtype=np.uint8)
    return a_arr, b_arr


def _ssim_wang_bovik(a: np.ndarray, b: np.ndarray) -> float:
    """Hand-rolled SSIM (Wang & Bovik 2004) over the full image.

    Single-scale, 8-bit luminance, default Gaussian-equivalent constants.
    Used when scikit-image is not installed.

    For an 8-bit image with L = 255:
        K1 = 0.01, C1 = (K1 * L) ** 2
        K2 = 0.03, C2 = (K2 * L) ** 2
    """
    a_f = a.astype(np.float64)
    b_f = b.astype(np.float64)

    mu_a = a_f.mean()
    mu_b = b_f.mean()
    sig_a_sq = a_f.var()
    sig_b_sq = b_f.var()
    # Population covariance to match var() above
    sig_ab = ((a_f - mu_a) * (b_f - mu_b)).mean()

    L = 255.0
    C1 = (0.01 * L) ** 2
    C2 = (0.03 * L) ** 2

    num = (2.0 * mu_a * mu_b + C1) * (2.0 * sig_ab + C2)
    den = (mu_a ** 2 + mu_b ** 2 + C1) * (sig_a_sq + sig_b_sq + C2)
    if den == 0.0:
        return 1.0
    return float(num / den)


# ---------------------------------------------------------------------------
# Height-histogram comparison (pipeline c4a9e271 phase-3)
# ---------------------------------------------------------------------------
# SSIM and pixel-fraction answer "did these two renders change?" — the wrong
# question for FONT-SIZE visual verification, where two captures of DIFFERENT
# text legitimately differ in nearly every pixel yet we only care about one
# scalar: how TALL is the rendered glyph ink. These helpers reduce an image to
# a vertical ink profile, extract the glyph-ink height, and compare the height
# RATIO between two captures (e.g. a heading-line overlay vs a body-line
# overlay). The phase-2 per-line-font-size test measured this inline; phase-3
# centralizes it here so every visual font test shares one definition.

def _longest_content_span(flags: np.ndarray, min_seg: int):
    """``(first_start, last_end_exclusive)`` spanning every run of consecutive
    True values in *flags* at least *min_seg* long, or ``None``. Thin isolated
    runs (border artifacts) are dropped; gaps between surviving runs are bridged.
    """
    b = np.asarray(flags, dtype=np.int8)
    if not b.any():
        return None
    edges = np.diff(np.concatenate([[0], b, [0]]))
    starts = np.flatnonzero(edges == 1)
    ends = np.flatnonzero(edges == -1)
    keep = (ends - starts) >= min_seg
    if not keep.any():
        return None
    return int(starts[keep][0]), int(ends[keep][-1])


def crop_to_page_content(img: Image.Image, white_thresh: int = 230,
                         min_run_frac: float = 0.15, pad: int = 2,
                         min_keep: float = 0.05) -> Image.Image:
    """Crop a canvas capture down to the rendered white PAGE block.

    A full-frame PyAutoGUI/PrintWindow capture includes the gray viewport margin
    (and possibly toolbar chrome) around the page; the margin shade, fit-zoom,
    and window geometry differ from host to host and dominate an SSIM/pixel diff,
    so a full-frame baseline drifts to false-reds on a different rig even when the
    page content is identical. The PDF page renders as a large near-white block
    distinct from the gray margin (~#6a) and toolbar (~#d4); keep only rows/cols
    whose near-white run clears *min_run_frac* of the frame dimension (isolating
    that block and rejecting thin border artifacts / the small zoom-combo box),
    then crop to it. Result is rig-independent; ``ssim``/``pixel_diff`` resize the
    two page crops to common dims so residual fit-zoom is a pure scale.

    Returns the original image unchanged when no clear page block is found
    (e.g. a non-white/scanned page) — never worse than no crop.
    """
    rgb = img.convert("RGB")
    w, h = rgb.size
    if w == 0 or h == 0:
        return rgb
    arr = np.asarray(rgb, dtype=np.int16)
    white = (arr >= white_thresh).all(axis=-1)
    if not white.any():
        return rgb
    min_seg = 3
    col_span = _longest_content_span(white.sum(axis=0) >= min_run_frac * h, min_seg)
    row_span = _longest_content_span(white.sum(axis=1) >= min_run_frac * w, min_seg)
    if col_span is None or row_span is None:
        return rgb
    x0 = max(0, col_span[0] - pad); x1 = min(w, col_span[1] + pad)
    y0 = max(0, row_span[0] - pad); y1 = min(h, row_span[1] + pad)
    if (x1 - x0) * (y1 - y0) < min_keep * w * h:
        return rgb
    return rgb.crop((x0, y0, x1, y1))


def ink_row_histogram(img: Image.Image, luma_threshold: int = 80) -> np.ndarray:
    """Per-row count of dark ("ink") pixels (top row first).

    A row's value is the number of pixels whose luminance is below
    ``luma_threshold`` (0-255). This is the raw vertical profile behind
    :func:`glyph_ink_height`.
    """
    gray = np.asarray(img.convert("L"), dtype=np.uint8)
    return (gray < luma_threshold).sum(axis=1).astype(np.int64)


def glyph_ink_height(img: Image.Image, luma_threshold: int = 80,
                     min_px_per_row: int = 3) -> int:
    """Vertical extent (px) of rendered glyph ink in ``img``.

    Rows with at least ``min_px_per_row`` dark pixels are treated as containing
    glyph ink; the height is the span from the first to the last such row
    (inclusive). Returns 0 for a blank image (no qualifying row).

    Measuring the span of *inked rows* (rather than a bounding box over every
    dark pixel) means a stray speck above or below the text line cannot by
    itself dictate the height — it must form a row clearing ``min_px_per_row``.
    The metric tracks the rendered font size: bigger glyphs ink more rows.
    """
    hist = ink_row_histogram(img, luma_threshold=luma_threshold)
    inked = np.flatnonzero(hist >= min_px_per_row)
    if inked.size == 0:
        return 0
    return int(inked[-1] - inked[0] + 1)


def height_histogram_compare(taller: Image.Image, shorter: Image.Image,
                             min_ratio: float = 1.5,
                             luma_threshold: int = 80,
                             min_px_per_row: int = 3) -> dict:
    """Compare the glyph-ink height of two captures.

    ``taller`` is the capture expected to render visibly taller text (e.g. a
    heading-line overlay); ``shorter`` the smaller one (e.g. a body line).

    Returns a dict with keys ``pass``, ``taller_height``, ``shorter_height``,
    ``ratio``. ``pass`` is True iff BOTH images contain ink and
    ``taller_height / shorter_height >= min_ratio``. Under a per-line
    font-size bug that renders both lines at the same region-average size the
    ratio collapses to ~1.0 and ``pass`` is False — so this discriminates a
    real per-line size difference from "every line is the same size".
    """
    th = glyph_ink_height(taller, luma_threshold, min_px_per_row)
    sh = glyph_ink_height(shorter, luma_threshold, min_px_per_row)
    ratio = (th / sh) if sh > 0 else 0.0
    ok = th > 0 and sh > 0 and ratio >= min_ratio
    return {
        "pass": bool(ok),
        "taller_height": int(th),
        "shorter_height": int(sh),
        "ratio": float(ratio),
    }


# ---------------------------------------------------------------------------
# Capture observability  (the SSIM=1.0 signature)
# ---------------------------------------------------------------------------
# A "did it change?" oracle reads
#
#     changed = (ssim < threshold) or (pixel_fraction >= min_fraction)
#
# and reports a FAILURE when nothing changed. That inference is only valid if
# the captures actually observed the screen. When they did not, before and after
# are the SAME BYTES, ssim is exactly 1.0, pixel_fraction is exactly 0.0, and the
# oracle concludes "the product did not repaint" -- which is a false failure
# pointing at the product.
#
# This is not hypothetical and it is not new. tests/docs/known_issues.md records
# it under CI-LIMITATION-WINDOWS-VALIDATION-UI-E2E-FLAKES (2026-06-30):
#
#     test_font_size_change_via_toolbar_spinner   CI runner: FAIL (SSIM=1.0)
#                                                 local RDP: PASS
#     test_canvas_pyautogui_visual_regression     CI runner: FAIL (SSIM=0.81)
#                                                 local RDP: PASS   [desktop-grab]
#
# classified there as "runner-only (session UIA)" and "runner-only
# (desktop-grab)". The distinguishing evidence is SSIM being EXACTLY 1.0 on an
# oracle whose whole purpose is to observe a change: two identical captures.
#
# The screenshot path is `pyautogui.screenshot()` -- a GDI grab of the screen --
# so it depends on there being an attached display. The SendInput canvas tests
# do not capture at all, which is why they pass straight through a disconnect
# while the visual families do not. Measuring the wrong family is how this got
# called "narrow" once already.

#: Two captures this similar cannot distinguish "nothing was drawn" from
#: "nothing was captured". Exactly 1.0 is the clean signal; the epsilon covers
#: a lossy round-trip through PNG.
IDENTICAL_SSIM_EPSILON = 1e-9


def captures_are_indistinguishable(a: Image.Image, b: Image.Image) -> bool:
    """True when two captures carry no information about a change.

    Deliberately strict: byte-identical, or SSIM within an epsilon of 1.0 AND
    no differing pixels at all. A merely SMALL difference is a real observation
    of a small change and must not be swallowed -- that would turn this into a
    blanket excuse for every below-threshold failure, which is the opposite of
    the intent.
    """
    a_rgb, b_rgb = _coerce_pair_to_rgb(a, b)
    if np.array_equal(np.asarray(a_rgb), np.asarray(b_rgb)):
        return True
    fraction, _ = pixel_diff(a_rgb, b_rgb, threshold_per_pixel=0)
    return fraction == 0.0 and abs(ssim(a_rgb, b_rgb) - 1.0) <= IDENTICAL_SSIM_EPSILON


def explain_unchanged(a: Image.Image, b: Image.Image) -> str:
    """Diagnosis text for a 'nothing changed' result, distinguishing the two cases.

    The whole point is that these two look identical in a test report and mean
    opposite things:

      * the product did not repaint          -> a real defect, investigate it
      * the capture did not observe anything -> a harness/session problem, and
                                                the run proves nothing
    """
    if captures_are_indistinguishable(a, b):
        return (
            "the before/after captures are IDENTICAL (SSIM = 1.0, zero differing "
            "pixels). That is not evidence the product failed to repaint -- it is "
            "what a capture that observed nothing looks like. Known cause: a "
            "session with no attached display (RDP disconnected, or a "
            "non-interactive runner session); pyautogui.screenshot() is a GDI "
            "screen grab. See tests/docs/known_issues.md "
            "CI-LIMITATION-WINDOWS-VALIDATION-UI-E2E-FLAKES, where exactly this "
            "produced 'FAIL (SSIM=1.0)' on the runner and PASS locally."
        )
    return (
        "the before/after captures DIFFER but by less than the change threshold, "
        "so the capture pipeline was working and the product genuinely did not "
        "change enough. This is a real result."
    )
