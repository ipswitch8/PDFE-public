# -*- coding: utf-8 -*-
"""Tests for the oracle-kind quantifier (gap A1-67 / UI-ORACLE-01).

The audit's "~632 rows assert structure, not pixels" was derived from the
``tool`` column, which names the DRIVER, not the oracle. These tests lock in a
measurement taken from the backing tests themselves, and ratchet the resulting
numbers so the pixel lane cannot silently shrink.
"""
from __future__ import annotations

import csv
import pathlib
import sys

import pytest

REPO_ROOT = pathlib.Path(__file__).resolve().parents[3]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tests.ui_automation.matrix import coverage_resolver as cr  # noqa: E402
from tests.ui_automation.matrix import oracle_kinds as ok  # noqa: E402


# ---------------------------------------------------------------------------
# classify_source
# ---------------------------------------------------------------------------

def test_cpp_backing_test_is_model_kind():
    """MUTATION KILLED: counting C++ model tests as UI pixel coverage."""
    assert ok.classify_source("TEST(A, B) {}", "tests/test_x.cpp") == "model"


def test_render_oracle_usage_is_pixel_kind():
    """MUTATION KILLED: emptying PIXEL_MARKERS, which would classify the whole
    render-proof lane as merely structural and hide the real ratio."""
    src = ("from tests.ui_automation.harness import render_drive as d\n"
           "def test_a():\n    d.assert_matches_golden(img, 'x.png')\n")
    assert ok.classify_source(src, "tests/ui_automation/test_a.py") == "pixel"


def test_uia_only_test_is_structural_kind():
    """MUTATION KILLED: classifying anything that merely mentions a canvas as
    pixel coverage."""
    src = ("def test_a(win):\n"
           "    assert win.child_window(auto_id='pdfCanvas').exists()\n")
    assert (ok.classify_source(src, "tests/ui_automation/test_a.py")
            == "structural")


def test_every_pixel_marker_is_recognised():
    """MUTATION KILLED: a marker list that has drifted from the harness API
    (e.g. an oracle renamed without updating the classifier), which would
    understate pixel coverage."""
    for marker in ok.PIXEL_MARKERS:
        src = f"def test_a():\n    {marker}\n"
        assert ok.classify_source(src, "t.py") == "pixel", marker


def test_unresolved_rows_are_counted_not_dropped():
    """MUTATION KILLED: silently dropping rows with no backing test, which
    would make the percentages sum over a shrunken denominator and flatter the
    coverage picture."""
    index = cr.TestIndex(node_ids=[], sources={})
    classified = ok.classify_rows(
        [{"test_id": "ghost", "status": "green", "notes": ""}], index)
    assert classified == [("ghost", "green", "unresolved")]
    assert ok.summarise(classified) == {"unresolved": 1}


# ---------------------------------------------------------------------------
# Ratchet on the real matrix
# ---------------------------------------------------------------------------

@pytest.fixture(scope="module")
def measured():
    csv_path = (REPO_ROOT / "tests" / "ui_automation" / "matrix"
                / "ui_test_matrix.csv")
    with csv_path.open("r", encoding="utf-8", newline="") as f:
        rows = list(csv.DictReader(f))
    return ok.classify_rows(rows, cr.build_index())


def test_measured_distribution_matches_the_recorded_figures(measured):
    """The numbers WP5 reported, taken from the tests rather than the ``tool``
    column. 787 rows: 717 structural / 37 model / 6 pixel / 27 unresolved.

    MUTATION KILLED: deleting or de-wiring a pixel-lane assertion (pixel count
    drops), or adding matrix rows without oracles. A genuine improvement also
    fails here — deliberately, so the figure quoted in reports is updated at
    the same time as the reality.
    """
    counts = ok.summarise(measured)
    assert sum(counts.values()) == 787
    assert counts == {"model": 37, "pixel": 6, "structural": 717,
                      "unresolved": 27}


def test_pixel_lane_does_not_shrink(measured):
    """A floor rather than an exact match, so this fails LOUDLY on regression
    while a genuine expansion of the pixel lane does not block work.
    MUTATION KILLED: removing the last pixel assertions from the matrix-backed
    tests."""
    counts = ok.summarise(measured)
    assert counts.get("pixel", 0) >= 6


def test_render_proof_lane_backs_no_matrix_rows(measured):
    """Documents a real structural finding: the 13 ``test_render_*.py`` pixel
    tests are a PARALLEL lane — not one matrix row resolves to them, so the
    matrix's own oracle mix understates project-wide pixel coverage while the
    matrix itself remains ~91% structural.

    MUTATION KILLED: quietly wiring render_* files into matrix rows (which
    would change what the matrix means) without updating this statement.
    """
    backing = {p for p in (ok._evidence_path(r) for r in [])} or set()
    index = cr.build_index()
    csv_path = (REPO_ROOT / "tests" / "ui_automation" / "matrix"
                / "ui_test_matrix.csv")
    with csv_path.open("r", encoding="utf-8", newline="") as f:
        rows = list(csv.DictReader(f))
    for row in rows:
        path = ok._evidence_path(cr.resolve_row(row, index))
        if path:
            backing.add(path)
    render_files = sorted(
        p.name for p in (REPO_ROOT / "tests" / "ui_automation").glob(
            "test_render_*.py"))
    assert render_files, "the render-proof lane disappeared"
    linked = [f for f in render_files
              if f"tests/ui_automation/{f}" in backing]
    assert linked == [], (
        f"render-proof files now back matrix rows: {linked} — update the "
        f"oracle-kind figures and this statement")
