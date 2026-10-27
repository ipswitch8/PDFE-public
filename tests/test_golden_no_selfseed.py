# -*- coding: utf-8 -*-
"""R-29 (harness-audit phase-6) — the golden oracle does not self-seed, and it
REJECTS defective renders against a committed reference.

Old weakness
------------
``golden.compare_to_golden`` promoted the current capture as the golden on first
run (``auto_regen_if_missing=True``) and ``regen_golden`` silently created a
baseline from whatever image it was handed. So the FIRST render — even a
mirrored, half-size, blanked, or wrong-icon one — became the reference every
later run compared against, and the visual check passed forever.

New oracle (this file)
----------------------
1.  compare_to_golden with a MISSING golden FAILS (``golden_missing``) and does
    NOT create the baseline — no self-seed on the assert path.
2.  regen_golden REFUSES without the explicit PDFE_ALLOW_GOLDEN_SEED opt-in, so
    a test run cannot mint its own passing baseline.
3.  Against the COMMITTED reference golden
    (``baseline/reference/save_icon.png``, produced by the deterministic trusted
    generator ``generate_reference_goldens.py``):
      * the CORRECT render PASSES,
      * a MIRRORED render FAILS,
      * a HALF-SIZE render FAILS,
      * a WRONG-ICON (colour-swapped) render FAILS.
    This proves the oracle discriminates a good render from the exact defect
    classes it exists to catch — the property the self-seeding version could
    never have, because it treated the first (possibly broken) render as truth.
"""

from __future__ import annotations

import os
from pathlib import Path

import pytest

from tests.ui_harness.core.golden import (
    compare_to_golden,
    golden_path,
    regen_golden,
    GOLDEN_DIR,
)
from tests.fixtures.ui_screenshots.generate_reference_goldens import (
    render_reference,
    render_mirrored,
    render_half_size,
    render_wrong_icon,
)

_SUBSYSTEM = "reference"
_TEST = "save_icon"

# Thresholds mirror the harness defaults (strict) for this synthetic, fully
# deterministic fixture: byte-identical correct render, clearly different bad
# renders. No CI-drift relaxation is needed because there is no real screen
# capture involved.
_NCC = 0.99
_MAXDELTA = 10
_PHASH = 5


def _committed_golden_exists() -> bool:
    return golden_path(_SUBSYSTEM, _TEST).exists()


def test_committed_reference_golden_is_present():
    """The trusted reference golden must be committed (fixture floor for R-29)."""
    gp = golden_path(_SUBSYSTEM, _TEST)
    assert gp.exists(), (
        f"Committed reference golden missing: {gp}. Regenerate with "
        "`python tests/fixtures/ui_screenshots/generate_reference_goldens.py` "
        "and commit it."
    )


def test_missing_golden_fails_and_does_not_selfseed(tmp_path):
    """A comparison against an ABSENT golden fails (golden_missing) and creates
    NO baseline file — the core no-self-seed contract."""
    absent_test = "definitely_absent_" + os.urandom(4).hex()
    gp = golden_path(_SUBSYSTEM, absent_test)
    assert not gp.exists()

    actual = tmp_path / "actual.png"
    render_reference().save(str(actual))

    result = compare_to_golden(_SUBSYSTEM, absent_test, str(actual),
                               ncc_threshold=_NCC,
                               max_delta_threshold=_MAXDELTA,
                               phash_threshold=_PHASH)
    assert result.is_same is False, "missing golden must NOT pass"
    assert getattr(result, "golden_missing", False) is True
    # Crucially: no baseline was minted from the current capture.
    assert not gp.exists(), (
        "compare_to_golden self-seeded a golden — the R-29 defect is back"
    )


def test_regen_golden_refuses_without_optin(tmp_path):
    """regen_golden must refuse to baseline unless the explicit maintenance
    opt-in is set, so the assert path can never create a passing baseline."""
    actual = tmp_path / "actual.png"
    render_reference().save(str(actual))
    seed_env = os.environ.pop("PDFE_ALLOW_GOLDEN_SEED", None)
    try:
        with pytest.raises(RuntimeError):
            regen_golden(_SUBSYSTEM, "seed_attempt_" + os.urandom(3).hex(),
                         str(actual))
    finally:
        if seed_env is not None:
            os.environ["PDFE_ALLOW_GOLDEN_SEED"] = seed_env


def test_correct_render_passes_against_committed_golden(tmp_path):
    if not _committed_golden_exists():
        pytest.fail("committed reference golden missing (see other test)")
    actual = tmp_path / "correct.png"
    render_reference().save(str(actual))
    result = compare_to_golden(_SUBSYSTEM, _TEST, str(actual),
                               ncc_threshold=_NCC,
                               max_delta_threshold=_MAXDELTA,
                               phash_threshold=_PHASH)
    assert result.is_same, f"correct render must match golden: {result.summary}"


@pytest.mark.parametrize("name,render", [
    ("mirrored", render_mirrored),
    ("half_size", render_half_size),
    ("wrong_icon", render_wrong_icon),
])
def test_defective_render_is_rejected(tmp_path, name, render):
    """Each defect class must be REJECTED against the committed golden — the
    proof the oracle actually discriminates (a self-seeded baseline could not)."""
    if not _committed_golden_exists():
        pytest.fail("committed reference golden missing (see other test)")
    actual = tmp_path / f"{name}.png"
    render().save(str(actual))
    result = compare_to_golden(_SUBSYSTEM, _TEST, str(actual),
                               ncc_threshold=_NCC,
                               max_delta_threshold=_MAXDELTA,
                               phash_threshold=_PHASH)
    assert result.is_same is False, (
        f"{name} render must be REJECTED against the golden but passed: "
        f"{result.summary}"
    )
