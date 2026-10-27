# -*- coding: utf-8 -*-
"""Pytest CI guard: every committed baseline PNG must match its logged pixel_md5_post.

Catches accidental baseline tampering (file replaced without regen log update).
Pipeline a7f3d2c9 phase-3 -- closes GAP-24.

How it works
------------
baseline_regen_log.json records ``pixel_md5_post`` for each regenerated
baseline PNG.  The MD5 is computed from raw RGBA pixel bytes (metadata-
stripped) so that PNG ancillary chunk differences (timestamps, tEXt
chunks) do not cause false positives.

This test iterates every entry with a non-null ``pixel_md5_post``, recomputes
the MD5 from the on-disk file, and fails if any value differs from the log.
"""

import hashlib
import json
from pathlib import Path

import pytest
from PIL import Image

REPO_ROOT = Path(__file__).resolve().parents[1]
LOG_PATH = REPO_ROOT / "tests" / "fixtures" / "baseline_regen_log.json"


def _pixel_md5(png_path: Path) -> str:
    """MD5 of raw RGBA pixel bytes (metadata-stripped)."""
    img = Image.open(png_path).convert("RGBA")
    return hashlib.md5(img.tobytes()).hexdigest()


def test_all_baseline_pixel_md5s_match() -> None:
    """Every entry with non-null pixel_md5_post must match the on-disk file.

    Fails with a list of all mismatched entries so a single run exposes every
    problem rather than stopping at the first mismatch.
    """
    if not LOG_PATH.is_file():
        pytest.fail(f"baseline_regen_log.json not found at {LOG_PATH}")

    data = json.loads(LOG_PATH.read_text(encoding="utf-8"))
    mismatches: list[str] = []

    for entry in data.get("baselines", []):
        expected = entry.get("pixel_md5_post")
        if expected is None:
            # Genuinely new file with no post-MD5 recorded; skip (cannot verify).
            continue

        filename = entry.get("filename", "")
        # Entries store paths relative to tests/fixtures/
        png_path = REPO_ROOT / "tests" / "fixtures" / filename

        if not png_path.is_file():
            mismatches.append(f"{filename}: file missing on disk")
            continue

        actual = _pixel_md5(png_path)
        if actual != expected:
            mismatches.append(
                f"{filename}: expected pixel_md5_post={expected}, got {actual}"
            )

    if mismatches:
        pytest.fail(
            "Baseline pixel MD5 mismatches:\n  " + "\n  ".join(mismatches)
        )
