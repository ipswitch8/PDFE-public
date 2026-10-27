# -*- coding: utf-8 -*-
"""Unit tests for the matrix row -> test resolver (gap A1-70 / UI-MATRIX-02).

Pure and headless: every test builds a synthetic :class:`TestIndex` rather than
scanning the repo, so the resolution RULES are exercised directly and the tests
cannot pass/fail for environmental reasons.

The hole being closed: ``verify_matrix.py`` validated only structure — unique
non-empty test_id, enum-valid priority/status/owner_phase — so a row marked
``green`` did not have to map to any test that exists, let alone one that
asserts. Every test below names the mutation it kills.
"""
from __future__ import annotations

import pathlib
import sys

import pytest

REPO_ROOT = pathlib.Path(__file__).resolve().parents[3]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tests.ui_automation.matrix import coverage_resolver as cr  # noqa: E402


CPP = """
TEST_F(UndoRedoFlowTest, UndoRedoAnnotationRoundtrip) { EXPECT_TRUE(true); }
TEST(MenuActionAccessibility, FileOpenActionHasObjectName) {}
TEST_F(SomeSuite, DISABLED_NotYetWorking) {}
"""

PY_OK = """
def test_plain_thing():
    assert record_matrix_result("row_by_literal", "green")
"""

PY_SKIPPED = """
import pytest

@pytest.mark.skip(reason="not implemented")
def test_blanket_skipped():
    record_matrix_result("row_blanket_skipped", "green")
"""

NODES = [
    "tests/ui_automation/test_menu_navigation_p0.py::test_p0_menu_item_no_crash[menu_close_doc]",
    "tests/ui_automation/test_x.py::test_multi[alpha-row_second_param]",
    "tests/ui_automation/test_x.py::test_named",
]

SOURCES = {
    "tests/test_gap102_residuals.cpp": CPP,
    "tests/ui_automation/test_literal.py": PY_OK,
    "tests/ui_automation/test_skipped.py": PY_SKIPPED,
    "tests/ui_automation/test_x.py": "def test_named():\n    pass\n",
}


@pytest.fixture
def index() -> cr.TestIndex:
    return cr.TestIndex(node_ids=list(NODES), sources=dict(SOURCES))


def row(test_id: str, status: str = "green", notes: str = ""):
    return {"test_id": test_id, "status": status, "notes": notes}


# ---------------------------------------------------------------------------
# A row pointing at a nonexistent test must FAIL the checker
# ---------------------------------------------------------------------------

def test_green_row_with_no_test_anywhere_is_unresolved(index):
    """MUTATION KILLED: the pre-fix behaviour — a checker that validates only
    structure and therefore reports this row as fine."""
    res = cr.resolve_row(row("ghost_row_never_written"), index)
    assert res.kind is None
    assert res.resolved is False
    assert cr.unresolved_claiming_rows([res]) == [res]


def test_covered_by_pointing_at_missing_file_is_unresolved(index):
    """MUTATION KILLED: trusting a ``covered-by:`` claim without verifying it.
    An unverifiable claim is an assertion of coverage with nothing behind it
    and must not resolve the row."""
    res = cr.resolve_row(
        row("claims_missing_file",
            notes="covered-by: tests/test_does_not_exist.cpp::Suite.Name"),
        index)
    assert res.resolved is False
    assert "not present" in res.disabled_reason
    assert cr.unresolved_claiming_rows([res]) == [res]


def test_covered_by_pointing_at_missing_test_in_real_file_is_unresolved(index):
    """MUTATION KILLED: verifying only that the FILE exists (the cheap check)
    instead of that the named test is declared in it — which is exactly how a
    renamed or deleted gtest keeps its matrix row green."""
    res = cr.resolve_row(
        row("claims_missing_test",
            notes=("covered-by: tests/test_gap102_residuals.cpp::"
                   "UndoRedoFlowTest.NoSuchTestName")),
        index)
    assert res.resolved is False
    assert "not declared" in res.disabled_reason


def test_covered_by_pointing_at_uncollected_pytest_test_is_unresolved(index):
    """MUTATION KILLED: accepting any pytest claim without checking the node
    was actually collected (renamed/deleted python test)."""
    res = cr.resolve_row(
        row("claims_dead_py",
            notes="covered-by: tests/ui_automation/test_x.py::test_deleted"),
        index)
    assert res.resolved is False
    assert "not collected" in res.disabled_reason


def test_malformed_covered_by_claim_is_unresolved(index):
    """MUTATION KILLED: treating the mere presence of the string
    ``covered-by:`` as evidence."""
    res = cr.resolve_row(row("bad_claim", notes="covered-by: some prose"),
                         index)
    assert res.resolved is False
    assert "malformed" in res.disabled_reason


def test_disabled_gtest_claim_does_not_resolve(index):
    """MUTATION KILLED: letting a DISABLED_ gtest substantiate a green row —
    a permanently-skipped test asserts nothing."""
    res = cr.resolve_row(
        row("claims_disabled",
            notes=("covered-by: tests/test_gap102_residuals.cpp::"
                   "SomeSuite.DISABLED_NotYetWorking")),
        index)
    assert res.resolved is False
    assert "DISABLED_" in res.disabled_reason


def test_blanket_skipped_python_test_does_not_resolve(index):
    """MUTATION KILLED: counting an unconditionally ``@pytest.mark.skip``
    test as coverage."""
    res = cr.resolve_row(row("row_blanket_skipped"), index)
    assert res.kind == "source"
    assert res.resolved is False
    assert "unconditionally skipped" in res.disabled_reason


# ---------------------------------------------------------------------------
# A valid row must pass
# ---------------------------------------------------------------------------

def test_verified_covered_by_claim_resolves(index):
    """MUTATION KILLED: a checker that rejects everything (which would 'pass'
    the failing-row tests above while being useless)."""
    res = cr.resolve_row(
        row("good_claim",
            notes=("Pipeline notes here; covered-by: "
                   "tests/test_gap102_residuals.cpp::"
                   "UndoRedoFlowTest.UndoRedoAnnotationRoundtrip")),
        index)
    assert res.kind == "covered-by"
    assert res.resolved is True
    assert cr.unresolved_claiming_rows([res]) == []


def test_parametrised_node_id_resolves(index):
    """MUTATION KILLED: dropping the parametrisation strategy — most matrix
    suites parametrise straight off the CSV, so the id never appears as a
    literal in any source."""
    res = cr.resolve_row(row("menu_close_doc"), index)
    assert res.kind == "param"
    assert res.resolved is True
    assert "menu_close_doc" in res.evidence


def test_parametrised_id_matches_only_whole_components(index):
    """MUTATION KILLED: substring matching on node ids (``tid in blob``), which
    resolves ``row_second`` against ``[alpha-row_second_param]`` and would
    launder near-miss ids into coverage."""
    assert cr.resolve_row(row("row_second"), index).resolved is False
    assert cr.resolve_row(row("row_second_param"), index).kind == "param"


def test_literal_source_reference_resolves(index):
    """MUTATION KILLED: dropping the literal-source strategy, which backs the
    ``record_matrix_result("<id>")`` rows."""
    res = cr.resolve_row(row("row_by_literal"), index)
    assert res.kind == "source"
    assert res.resolved is True


def test_notes_gtest_reference_resolves(index):
    """MUTATION KILLED: dropping the legacy notes-name-a-cpp strategy, which
    would turn ~18 genuinely-covered rows into false failures."""
    res = cr.resolve_row(
        row("legacy_notes",
            notes=("Pipeline x: test_gap102_residuals.cpp "
                   "FileOpenActionHasObjectName")),
        index)
    assert res.kind == "notes-gtest"
    assert res.resolved is True


def test_non_claiming_statuses_are_exempt(index):
    """A wont-fix / xfail / skip row is a documented NON-result, so it is not
    required to resolve — but it also never counts as coverage.
    MUTATION KILLED: adding 'green' to the exempt set (which would disable the
    whole check), or failing on wont-fix rows (noise that gets the check
    switched off)."""
    ghosts = [cr.resolve_row(row("ghost", status=s), index)
              for s in ("wont-fix", "xfail", "skip", "planned", "red")]
    assert cr.unresolved_claiming_rows(ghosts) == []
    assert "green" in cr.CLAIMING_STATUSES
    assert "green" not in cr.NON_RESULT_STATUSES


# ---------------------------------------------------------------------------
# End-to-end shape of the report
# ---------------------------------------------------------------------------

def test_format_report_counts_by_evidence_kind(index):
    rows = [row("menu_close_doc"), row("row_by_literal"), row("ghost_x")]
    out = cr.format_report(cr.resolve_rows(rows, index))
    assert "param" in out and "source" in out and "UNRESOLVED" in out


def test_real_matrix_csv_has_no_regression_in_unresolved_rows():
    """Ratchet against the committed matrix: the set of green rows that cannot
    be tied to a test must not GROW.

    This is deliberately an exact-set assertion, not a count threshold: adding
    a new unsubstantiated green row fails, and fixing one of the listed rows
    also fails (with a message telling you to shrink the list). Both directions
    are what keeps the ledger honest.

    MUTATION KILLED: adding a new green row with no test, or silently
    re-introducing auto-resolution for these ids.
    """
    import csv
    csv_path = (REPO_ROOT / "tests" / "ui_automation" / "matrix"
                / "ui_test_matrix.csv")
    with csv_path.open("r", encoding="utf-8", newline="") as f:
        rows = list(csv.DictReader(f))
    idx = cr.build_index()
    unresolved = {r.test_id
                  for r in cr.unresolved_claiming_rows(cr.resolve_rows(rows,
                                                                       idx))}
    assert unresolved == KNOWN_UNSUBSTANTIATED_GREEN_ROWS, (
        "matrix coverage ledger drifted.\n"
        f"newly unsubstantiated: "
        f"{sorted(unresolved - KNOWN_UNSUBSTANTIATED_GREEN_ROWS)}\n"
        f"now substantiated (remove from the list): "
        f"{sorted(KNOWN_UNSUBSTANTIATED_GREEN_ROWS - unresolved)}")


#: Green rows in the committed matrix that claim a passing result with no
#: identifiable asserting test anywhere in the tree.
#:
#: F17 RESOLUTION: gap A1-70's cross-check found 26 such rows (8 "MATRIX-EXPAND"
#: panel flows, 18 "MAT-2" prose rows). None of them had a test, so none of them
#: could honestly stay green: their status in ui_test_matrix.csv was corrected
#: from `green` to `planned`, each with a note recording why. That is the
#: remediation -- the coverage was never there, and the matrix now says so
#: instead of claiming it. The set is therefore EMPTY, and this ratchet's job
#: from here is to keep it empty: a new green row with no test fails
#: immediately, and so does a row silently restored to green without a verified
#: `covered-by:` claim.
KNOWN_UNSUBSTANTIATED_GREEN_ROWS: set = set()
