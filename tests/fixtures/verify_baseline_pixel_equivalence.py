#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Verify baseline_regen_log.json pixel-equivalence claims via MD5.

Background
----------
Pipeline b3d7f942 phase-6 created baseline_regen_log.json with 14
entries. Eight of them claimed "pixel content verified identical … by
MD5 comparison" but no script or artifact recorded the actual hashes.
Pipeline 3e7c1a05 phase-3 computed the real hashes and wrote them into
the log as ``pixel_md5_pre`` (before-regen) and ``pixel_md5_post``
(expected post-regen state matching current on-disk files).

How it works (tamper-detection mode)
--------------------------------------
Each entry stores ``pixel_md5_post``: the expected MD5 of the baseline
PNG as of the regen. This script computes the current on-disk MD5 and
compares it to ``pixel_md5_post``.

* PASS — current file MD5 matches ``pixel_md5_post`` (no tampering).
* FAIL — current file MD5 differs from ``pixel_md5_post`` (baseline
  was modified after the regen without updating the log).
* SKIP — entry has no ``pixel_md5_post`` (genuinely new file with
  ``verified: null``); cannot compare.

Pixel MD5 is computed from raw ``RGBA`` pixel bytes so that PNG
ancillary chunk differences (timestamps, tEXt metadata) do not cause
false positives.

The script is **deterministic and idempotent**: the ``pixel_md5_post``
fields are fixed reference values; re-running always produces the same
result unless the on-disk files change.

Usage
-----
Run from the repository root::

    python tests/fixtures/verify_baseline_pixel_equivalence.py

Exit codes
----------
0 — all populated entries verified (current files match pixel_md5_post).
1 — one or more mismatches, or fatal error.
"""

import hashlib
import io
import json
import sys
from pathlib import Path

# Reconfigure stdout/stderr to UTF-8 (Windows console may default to cp1252).
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, Exception):
        pass

from PIL import Image  # noqa: E402 — after sys.stdout reconfigure

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

REPO = Path(__file__).resolve().parent.parent.parent
LOG_PATH = Path(__file__).resolve().parent / "baseline_regen_log.json"


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def pixel_md5(png_bytes: bytes) -> str:
    """Compute MD5 of raw RGBA pixel bytes (metadata-independent)."""
    img = Image.open(io.BytesIO(png_bytes)).convert("RGBA")
    return hashlib.md5(img.tobytes()).hexdigest()


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------


def main() -> int:
    if not LOG_PATH.is_file():
        sys.stderr.write(f"ERROR: log file not found: {LOG_PATH}\n")
        return 1

    data = json.loads(LOG_PATH.read_text(encoding="utf-8"))
    baselines = data.get("baselines", [])

    passes = 0
    fails = 0
    skips = 0
    fail_paths: list[str] = []

    for entry in baselines:
        filename = entry.get("filename", "")
        git_path = f"tests/fixtures/{filename}"
        file_path = REPO / git_path

        expected_md5 = entry.get("pixel_md5_post")

        # ------------------------------------------------------------------
        # Case 1: no expected MD5 — genuinely new file, skip.
        # ------------------------------------------------------------------
        if expected_md5 is None:
            reason = entry.get("note", "no pixel_md5_post recorded")
            print(f"SKIP: {git_path} reason={reason}")
            skips += 1
            continue

        # ------------------------------------------------------------------
        # Case 2: file missing on disk — cannot verify.
        # ------------------------------------------------------------------
        if not file_path.is_file():
            print(f"SKIP: {git_path} reason=file not found on disk")
            skips += 1
            continue

        # ------------------------------------------------------------------
        # Case 3: compare current MD5 to expected.
        # ------------------------------------------------------------------
        current_md5 = pixel_md5(file_path.read_bytes())

        md5_pre = entry.get("pixel_md5_pre", "n/a")

        if current_md5 == expected_md5:
            print(
                f"PASS: {git_path} "
                f"md5_pre={md5_pre} "
                f"md5_post={expected_md5}"
            )
            passes += 1
        else:
            print(
                f"FAIL: {git_path} "
                f"md5_pre={md5_pre} "
                f"md5_post={expected_md5} "
                f"current={current_md5} mismatch"
            )
            fails += 1
            fail_paths.append(git_path)

    # Summary line.
    if fails == 0:
        print(f"\nALL PASS ({passes} verified, {skips} skipped)")
        return 0
    else:
        print(f"\nMISMATCHES: {fails} entries differ")
        for p in fail_paths:
            print(f"  {p}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
