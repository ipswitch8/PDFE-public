# -*- coding: utf-8 -*-
"""Golden-image baseline management.

"Golden" images are the authoritative reference screenshots.  They live at:

    tests/fixtures/ui_screenshots/baseline/<subsystem>/<test>.png

R-29 (harness-audit) — NO SELF-SEEDING
--------------------------------------
A golden MUST be a COMMITTED, known-correct baseline produced by a trusted
reference (a deliberate human-reviewed capture, or a deterministic synthetic
generator — see tests/fixtures/ui_screenshots/generate_reference_goldens.py).
It is NEVER the current run's own capture promoted on first sight: that makes
any render — mirrored, half-size, blanked, wrong-icon — pass forever, because
the very first (possibly broken) capture becomes the reference it is later
compared against.

Consequently ``compare_to_golden`` NEVER creates a baseline. When the committed
golden is absent the comparison FAILS (``is_same=False``, ``golden_missing=True``)
so a missing baseline is a loud test failure, not a silent auto-pass.

Deliberately (re)baselining after a reviewed UI change is a separate, explicit
maintenance action: ``regen_golden(..., force=True)`` guarded by the
``PDFE_ALLOW_GOLDEN_SEED=1`` environment opt-in. It is never reachable from the
normal assert path, so a test run can never mint its own passing baseline.

Usage
-----
    from tests.ui_harness.core.golden import compare_to_golden

    actual_path = capture_canvas(app, "text_edit", "bold_on")
    result = compare_to_golden("text_edit", "bold_on", actual_path)
    assert result.is_same, result.summary   # FAILS if the golden is missing

    # Deliberate rebaseline (maintenance only, opt-in):
    #   PDFE_ALLOW_GOLDEN_SEED=1  →  regen_golden(..., force=True)
"""

import os
import shutil
from pathlib import Path
from typing import Optional

from tests.ui_harness.core.diff import (
    ImageDiffResult,
    compare_images,
    DEFAULT_NCC_THRESHOLD,
    DEFAULT_MAX_DELTA_THRESHOLD,
    DEFAULT_PHASH_THRESHOLD,
)

# ---------------------------------------------------------------------------
# Path constants
# ---------------------------------------------------------------------------

_HARNESS_DIR = Path(__file__).parent.parent
_REPO_ROOT = _HARNESS_DIR.parent.parent
GOLDEN_DIR = _REPO_ROOT / "tests" / "fixtures" / "ui_screenshots" / "baseline"


def golden_path(subsystem: str, test: str) -> Path:
    """Return the expected path for a golden image."""
    return GOLDEN_DIR / subsystem / f"{test}.png"


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

def _seed_allowed() -> bool:
    """True only when the explicit maintenance opt-in is set. Baselining a
    golden is a deliberate, human-reviewed action — never something a normal
    test run may do — so it is gated behind PDFE_ALLOW_GOLDEN_SEED=1."""
    return os.environ.get("PDFE_ALLOW_GOLDEN_SEED", "") == "1"


def regen_golden(subsystem: str, test: str, actual_path: str,
                 force: bool = False) -> str:
    """Deliberately (re)baseline a golden from a TRUSTED reference image.

    R-29: this is a MAINTENANCE tool, not part of any assert path. It refuses
    to run unless the ``PDFE_ALLOW_GOLDEN_SEED=1`` opt-in is set, so a test can
    never self-seed its own passing baseline from the current (possibly broken)
    capture. Callers must pass an image produced by a trusted reference — a
    reviewed capture or the deterministic synthetic generator — NOT an
    unreviewed run-time screenshot.

    Parameters
    ----------
    subsystem   : str
    test        : str
    actual_path : str  path to the trusted reference image
    force       : bool  overwrite an existing golden (required when one exists)

    Returns
    -------
    str  path of the golden file

    Raises
    ------
    RuntimeError  if PDFE_ALLOW_GOLDEN_SEED is not set, or the golden already
                  exists and force is False.
    """
    if not _seed_allowed():
        raise RuntimeError(
            "regen_golden refused: baselining a golden is a deliberate "
            "maintenance action. Set PDFE_ALLOW_GOLDEN_SEED=1 and pass a "
            "trusted reference image (never the current test capture). "
            "See tests/ui_harness/core/golden.py (R-29)."
        )
    dest = golden_path(subsystem, test)
    if dest.exists() and not force:
        raise RuntimeError(
            f"Golden already exists ({dest}); pass force=True to overwrite "
            "after a reviewed UI change."
        )
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(actual_path, str(dest))
    return str(dest)


def compare_to_golden(
    subsystem: str,
    test: str,
    actual_path: str,
    ncc_threshold: float = DEFAULT_NCC_THRESHOLD,
    max_delta_threshold: int = DEFAULT_MAX_DELTA_THRESHOLD,
    phash_threshold: int = DEFAULT_PHASH_THRESHOLD,
) -> ImageDiffResult:
    """Compare actual_path against the COMMITTED golden for (subsystem, test).

    R-29: NEVER seeds. If the committed golden is absent the result is a
    FAILURE (``is_same=False``, ``golden_missing=True``), never an auto-created
    pass. A missing baseline means "this render has no trusted reference yet" —
    a condition that must fail the test, not silently mint the current capture
    as correct.

    Parameters
    ----------
    subsystem, test, actual_path : identify the comparison
    ncc_threshold, max_delta_threshold, phash_threshold : per-call overrides

    Returns
    -------
    ImageDiffResult  (``golden_missing`` set when the baseline is absent)
    """
    gp = golden_path(subsystem, test)

    if not gp.exists():
        result = ImageDiffResult(is_same=False)
        result.golden_missing = True
        result.summary = (
            f"Golden image missing: {gp}. A committed, known-correct baseline "
            "is required — the test does NOT self-seed one from the current "
            "capture (R-29). Produce it from a trusted reference and commit it "
            "(PDFE_ALLOW_GOLDEN_SEED=1 regen_golden), or mark the test pending."
        )
        return result

    return compare_images(
        str(gp),
        actual_path,
        ncc_threshold=ncc_threshold,
        max_delta_threshold=max_delta_threshold,
        phash_threshold=phash_threshold,
    )


def list_golden_images(subsystem: Optional[str] = None):
    """Return paths of all stored golden images.

    Parameters
    ----------
    subsystem : str or None  filter by subsystem; None returns all

    Returns
    -------
    list of Path
    """
    if not GOLDEN_DIR.exists():
        return []

    if subsystem:
        sub_dir = GOLDEN_DIR / subsystem
        if not sub_dir.exists():
            return []
        return list(sub_dir.glob("*.png"))

    return list(GOLDEN_DIR.rglob("*.png"))


def delete_golden(subsystem: str, test: str) -> bool:
    """Delete a stored golden image.

    Returns True if the file existed and was deleted, False otherwise.
    """
    gp = golden_path(subsystem, test)
    if gp.exists():
        gp.unlink()
        return True
    return False
