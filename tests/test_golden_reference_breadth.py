# -*- coding: utf-8 -*-
"""C-08 (coverage-expansion phase-3) — visual-regression BREADTH.

The R-29 remediation fixed the self-seeding golden mechanism but committed
exactly ONE correctness golden (``reference/save_icon.png``). A single guarded
icon leaves the wider headless-render surface unprotected: an icon/theme/paint
regression on any OTHER state is invisible.

This suite expands the committed correctness-golden set to a BREADTH of distinct
headless render states, each produced by the deterministic TRUSTED generator
(``generate_reference_goldens.py`` — NOT a self-captured screenshot) and each
PROVEN to:

  * PASS its own correct render, and
  * REJECT every deliberate defect class (mirror / half-size / wrong-colour).

Because every comparison goes through ``compare_to_golden`` against a COMMITTED
baseline (which never self-seeds — a missing golden is a hard failure), there is
no path by which a broken render could mint itself as the reference.

Headless vs runner-only
-----------------------
Every golden here is a deterministic synthetic render (a fixed drawing spec), so
it is fully reproducible on any headless box and committed as a trusted
reference. FULL live-application window states (real MainWindow chrome, native
menus, GPU-composited canvas) are NOT committed here: they depend on the
self-hosted UI runner and its exact display, and faking them would reintroduce
the self-seeding anti-pattern. Those remain runner-only (see
tests/ui_harness/ live-capture tests) and are deliberately excluded from this
committed set.
"""

from __future__ import annotations

import pytest

from tests.ui_harness.core.golden import compare_to_golden, golden_path
from tests.fixtures.ui_screenshots.generate_reference_goldens import (
    REFERENCES,
    DEFECTS,
    baseline_dir,
)

_SUBSYSTEM = "reference"

# Strict thresholds — these are synthetic, fully deterministic fixtures with a
# byte-identical correct render and clearly different defect renders, so no
# CI-drift relaxation is warranted (there is no real screen capture involved).
_NCC = 0.99
_MAXDELTA = 10
_PHASH = 5

# The breadth set this phase ADDS on top of the original save_icon.
_NEW_GOLDENS = ["open_icon", "page_region", "dialog_control"]


def test_breadth_set_registered():
    """The generator registry must contain the original plus the new breadth
    goldens — guards against a golden being silently dropped from the set."""
    assert "save_icon" in REFERENCES
    for name in _NEW_GOLDENS:
        assert name in REFERENCES, f"breadth golden '{name}' missing from registry"


@pytest.mark.parametrize("name", sorted(REFERENCES.keys()))
def test_committed_golden_present(name):
    """Each registered reference must have a COMMITTED baseline PNG on disk
    (the fixture floor — no self-seed can create it at test time)."""
    gp = golden_path(_SUBSYSTEM, name)
    assert gp.exists(), (
        f"Committed golden missing: {gp}. Regenerate with "
        "`python tests/fixtures/ui_screenshots/generate_reference_goldens.py` "
        "and commit it."
    )


@pytest.mark.parametrize("name", sorted(REFERENCES.keys()))
def test_correct_render_passes(tmp_path, name):
    """The correct render of each golden PASSES against its committed baseline."""
    actual = tmp_path / f"{name}_correct.png"
    REFERENCES[name]().save(str(actual))
    result = compare_to_golden(_SUBSYSTEM, name, str(actual),
                               ncc_threshold=_NCC,
                               max_delta_threshold=_MAXDELTA,
                               phash_threshold=_PHASH)
    assert result.is_same, f"correct '{name}' render must match golden: {result.summary}"


@pytest.mark.parametrize("name", sorted(REFERENCES.keys()))
@pytest.mark.parametrize("defect", sorted(DEFECTS.keys()))
def test_defect_render_is_rejected(tmp_path, name, defect):
    """Every defect class (mirror/half-size/wrong-colour) is REJECTED against the
    committed golden — the per-golden proof the oracle discriminates a good
    render from the exact defects it exists to catch."""
    actual = tmp_path / f"{name}_{defect}.png"
    DEFECTS[defect](REFERENCES[name]()).save(str(actual))
    result = compare_to_golden(_SUBSYSTEM, name, str(actual),
                               ncc_threshold=_NCC,
                               max_delta_threshold=_MAXDELTA,
                               phash_threshold=_PHASH)
    assert result.is_same is False, (
        f"'{defect}' defect of '{name}' must be REJECTED against the golden but "
        f"passed: {result.summary}"
    )


def test_no_selfseed_on_absent_golden(tmp_path):
    """Sanity: a comparison against an ABSENT golden fails and creates no
    baseline — the no-self-seed contract still holds for the breadth set."""
    absent = "reference_breadth_absent"
    gp = golden_path(_SUBSYSTEM, absent)
    assert not gp.exists()
    actual = tmp_path / "actual.png"
    REFERENCES["open_icon"]().save(str(actual))
    result = compare_to_golden(_SUBSYSTEM, absent, str(actual),
                               ncc_threshold=_NCC,
                               max_delta_threshold=_MAXDELTA,
                               phash_threshold=_PHASH)
    assert result.is_same is False
    assert getattr(result, "golden_missing", False) is True
    assert not gp.exists(), "compare_to_golden self-seeded a golden"
