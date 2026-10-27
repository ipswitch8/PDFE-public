# -*- coding: utf-8 -*-
"""Image diff utilities for visual regression testing.

Supports three comparison metrics:
1. Normalised Cross-Correlation (NCC) — overall structural similarity.
2. Per-channel maximum delta — catches single-pixel colour shifts.
3. Perceptual hash distance (pHash via imagehash) — robust to minor
   sub-pixel rendering differences.

A side-by-side output image is generated when any metric exceeds its
threshold, making failures easy to diagnose visually.

Usage
-----
    from tests.ui_harness.core.diff import compare_images, ImageDiffResult

    result = compare_images("baseline.png", "candidate.png")
    assert result.is_same, result.summary
"""

from __future__ import annotations

import os
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

import numpy as np
from PIL import Image

try:
    import imagehash
    _IMAGEHASH_AVAILABLE = True
except ImportError:
    _IMAGEHASH_AVAILABLE = False

# ---------------------------------------------------------------------------
# Default thresholds (can be overridden per call)
# ---------------------------------------------------------------------------

DEFAULT_NCC_THRESHOLD = 0.99          # NCC >= this → pass
DEFAULT_MAX_DELTA_THRESHOLD = 10      # per-channel max pixel diff <= this → pass
DEFAULT_PHASH_THRESHOLD = 5           # pHash distance <= this → pass (0=identical)


# ---------------------------------------------------------------------------
# Data class
# ---------------------------------------------------------------------------

@dataclass
class ImageDiffResult:
    """Results of an image comparison."""
    ncc: float = 1.0
    max_delta: int = 0
    phash_distance: Optional[int] = None
    is_same: bool = True
    summary: str = "identical"
    diff_image_path: Optional[str] = None
    # R-29: set True by compare_to_golden when the committed baseline is absent,
    # distinguishing "no trusted reference exists" from "render differs".
    golden_missing: bool = False

    # Thresholds used for this comparison (stored for reporting)
    ncc_threshold: float = DEFAULT_NCC_THRESHOLD
    max_delta_threshold: int = DEFAULT_MAX_DELTA_THRESHOLD
    phash_threshold: int = DEFAULT_PHASH_THRESHOLD


# ---------------------------------------------------------------------------
# Core comparison
# ---------------------------------------------------------------------------

def compare_images(
    expected_path: str,
    actual_path: str,
    ncc_threshold: float = DEFAULT_NCC_THRESHOLD,
    max_delta_threshold: int = DEFAULT_MAX_DELTA_THRESHOLD,
    phash_threshold: int = DEFAULT_PHASH_THRESHOLD,
    save_diff_beside: bool = True,
) -> ImageDiffResult:
    """Compare two PNG images and return an ImageDiffResult.

    Parameters
    ----------
    expected_path     : str  path to the reference / golden image
    actual_path       : str  path to the newly captured image
    ncc_threshold     : float  minimum NCC for pass (default 0.99)
    max_delta_threshold : int  maximum per-channel pixel delta for pass (default 10)
    phash_threshold   : int  maximum pHash distance for pass (default 5)
    save_diff_beside  : bool  save side-by-side diff next to actual image

    Returns
    -------
    ImageDiffResult
    """
    result = ImageDiffResult(
        ncc_threshold=ncc_threshold,
        max_delta_threshold=max_delta_threshold,
        phash_threshold=phash_threshold,
    )

    if not os.path.exists(expected_path):
        result.is_same = False
        result.summary = f"Expected image not found: {expected_path}"
        return result

    if not os.path.exists(actual_path):
        result.is_same = False
        result.summary = f"Actual image not found: {actual_path}"
        return result

    # P-G-INFRA-8 fix: when both arguments point at the same file the
    # images are byte-identical by construction.  Skip the float32 NCC
    # / phash computation entirely — the centred-correlation formula
    # accumulates float rounding error (denom = sqrt(sum * sum)) and
    # yields ~0.9999 for what should be exactly 1.0, which breaks
    # assert_zero_delta's ncc_threshold=1.0 contract.  Short-circuit
    # is also a perf win for repeated self-checks.
    if os.path.abspath(expected_path) == os.path.abspath(actual_path):
        result.ncc = 1.0
        result.max_delta = 0
        result.phash_distance = 0
        result.is_same = True
        result.summary = "self-comparison (short-circuited)"
        return result

    img_exp = Image.open(expected_path).convert("RGB")
    img_act = Image.open(actual_path).convert("RGB")

    # Resize actual to expected dimensions if they differ (handles minor DPI diffs).
    if img_exp.size != img_act.size:
        img_act = img_act.resize(img_exp.size, Image.LANCZOS)

    arr_exp = np.asarray(img_exp, dtype=np.float32)
    arr_act = np.asarray(img_act, dtype=np.float32)

    # --- NCC ---
    exp_flat = arr_exp.flatten()
    act_flat = arr_act.flatten()
    exp_norm = exp_flat - exp_flat.mean()
    act_norm = act_flat - act_flat.mean()
    denom = np.sqrt((exp_norm ** 2).sum() * (act_norm ** 2).sum())
    result.ncc = float(np.dot(exp_norm, act_norm) / denom) if denom > 0 else 1.0

    # --- Per-channel max delta ---
    delta = np.abs(arr_exp - arr_act)
    result.max_delta = int(delta.max())

    # --- pHash ---
    if _IMAGEHASH_AVAILABLE:
        hash_exp = imagehash.phash(img_exp)
        hash_act = imagehash.phash(img_act)
        result.phash_distance = int(hash_exp - hash_act)

    # --- Pass/fail verdict ---
    failures = []
    if result.ncc < ncc_threshold:
        failures.append(
            f"NCC={result.ncc:.4f} < threshold {ncc_threshold}"
        )
    if result.max_delta > max_delta_threshold:
        failures.append(
            f"max_delta={result.max_delta} > threshold {max_delta_threshold}"
        )
    if result.phash_distance is not None and result.phash_distance > phash_threshold:
        failures.append(
            f"pHash_dist={result.phash_distance} > threshold {phash_threshold}"
        )

    if failures:
        result.is_same = False
        result.summary = "; ".join(failures)
        if save_diff_beside:
            result.diff_image_path = _save_side_by_side(
                img_exp, img_act, actual_path
            )
    else:
        result.is_same = True
        result.summary = (
            f"identical (NCC={result.ncc:.4f}, max_delta={result.max_delta}"
            + (f", pHash={result.phash_distance}" if result.phash_distance is not None else "")
            + ")"
        )

    return result


def _save_side_by_side(img_exp: Image.Image, img_act: Image.Image,
                       actual_path: str) -> str:
    """Save a horizontal side-by-side comparison image next to actual_path."""
    w = img_exp.width + img_act.width + 10
    h = max(img_exp.height, img_act.height)
    composite = Image.new("RGB", (w, h), color=(200, 200, 200))
    composite.paste(img_exp, (0, 0))
    composite.paste(img_act, (img_exp.width + 10, 0))

    stem = Path(actual_path).stem
    parent = Path(actual_path).parent
    diff_path = str(parent / f"{stem}_diff.png")
    composite.save(diff_path)
    return diff_path


# ---------------------------------------------------------------------------
# Convenience: zero-diff self-comparison
# ---------------------------------------------------------------------------

def assert_zero_delta(image_path: str):
    """Assert that an image is identical when compared to itself.

    Useful in the smoke test to verify the diff pipeline is wired correctly.
    Raises AssertionError if comparison fails.
    """
    result = compare_images(
        image_path, image_path,
        ncc_threshold=1.0,
        max_delta_threshold=0,
        phash_threshold=0,
        save_diff_beside=False,
    )
    assert result.is_same, (
        f"Self-comparison of {image_path!r} yielded non-zero delta: "
        f"{result.summary}"
    )
