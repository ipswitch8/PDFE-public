# -*- coding: utf-8 -*-
"""Matrix-health H4 tests (pipeline e5b9c231 Phase 5 — criterion 2).

Proves the health gate flags GREEN -> SKIP / NO-RUN degradations, not just
GREEN -> RED. A committed-green row that silently degrades to skip is a
coverage loss the old check (green->red only) let through.

Run:
    python -m pytest tests/scripts/test_check_matrix_health.py -q
"""
from __future__ import annotations

import csv
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_matrix_health import evaluate  # noqa: E402


def _write_csv(path: Path, rows):
    with path.open("w", encoding="utf-8", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["test_id", "status", "notes"])
        w.writeheader()
        for tid, status in rows:
            w.writerow({"test_id": tid, "status": status, "notes": ""})


def _committed(path: Path):
    _write_csv(path, [
        ("row_a", "green"),
        ("row_b", "green"),
        ("row_c", "green"),
    ])


def test_green_to_skip_is_flagged(tmp_path):
    """The core H4 case: a committed-green row degrading to 'skip' must FAIL."""
    committed = tmp_path / "committed.csv"
    runtime = tmp_path / "runtime.csv"
    _committed(committed)
    _write_csv(runtime, [
        ("row_a", "green"),
        ("row_b", "skip"),     # silent degradation
        ("row_c", "green"),
    ])
    healthy, msgs = evaluate(committed, runtime, floor_delta=5)
    assert not healthy, "green->skip degradation was not flagged (H4 hole open)"
    assert any("row_b" in m for m in msgs)


def test_green_to_blocked_is_flagged(tmp_path):
    committed = tmp_path / "committed.csv"
    runtime = tmp_path / "runtime.csv"
    _committed(committed)
    _write_csv(runtime, [("row_a", "green"), ("row_b", "green"), ("row_c", "blocked")])
    healthy, _ = evaluate(committed, runtime, floor_delta=5)
    assert not healthy


def test_green_to_red_still_flagged(tmp_path):
    """Regression: the original green->red defense must remain."""
    committed = tmp_path / "committed.csv"
    runtime = tmp_path / "runtime.csv"
    _committed(committed)
    _write_csv(runtime, [("row_a", "green"), ("row_b", "red"), ("row_c", "green")])
    healthy, _ = evaluate(committed, runtime, floor_delta=5)
    assert not healthy


def test_green_to_no_run_tolerated_without_require_ran(tmp_path):
    """A partial leg (row absent from runtime CSV) is tolerated by default."""
    committed = tmp_path / "committed.csv"
    runtime = tmp_path / "runtime.csv"
    _committed(committed)
    # row_c absent entirely (a partial leg that didn't run it).
    _write_csv(runtime, [("row_a", "green"), ("row_b", "green")])
    healthy, msgs = evaluate(committed, runtime, floor_delta=5)
    assert healthy, "out-of-scope no-run should be tolerated without --require-ran"
    assert any("not run this leg" in m for m in msgs)


def test_green_to_no_run_flagged_with_require_ran(tmp_path):
    """A full sweep (--require-ran) must FAIL on a committed-green row not run."""
    committed = tmp_path / "committed.csv"
    runtime = tmp_path / "runtime.csv"
    _committed(committed)
    _write_csv(runtime, [
        ("row_a", "green"),
        ("row_b", "green"),
        ("row_c", "no-run"),   # explicit no-run from the honest overlay
    ])
    healthy, msgs = evaluate(committed, runtime, floor_delta=5, require_ran=True)
    assert not healthy, "full-sweep green->no-run was not flagged"
    assert any("row_c" in m for m in msgs)


def test_all_green_is_healthy(tmp_path):
    committed = tmp_path / "committed.csv"
    runtime = tmp_path / "runtime.csv"
    _committed(committed)
    _write_csv(runtime, [("row_a", "green"), ("row_b", "green"), ("row_c", "green")])
    healthy, _ = evaluate(committed, runtime, floor_delta=5, require_ran=True)
    assert healthy


# ---------------------------------------------------------------------------
# GitHub #142 — the gate could only ratify. A committed `planned` row was not
# merely unsatisfied, it was INVISIBLE: every check above filters the
# committed matrix down to `status == "green"` before looking at anything.
#
# Each test below carries a CONTROL, because a check about un-started work has
# an obvious way to be useless: fail on everything, or fail on nothing.
# ---------------------------------------------------------------------------

def _committed_with_planned(path: Path, planned: int = 2):
    rows = [("row_a", "green"), ("row_b", "green"), ("row_c", "green")]
    rows += [(f"row_planned_{i}", "planned") for i in range(planned)]
    _write_csv(path, rows)


def test_planned_rows_are_named_in_the_output(tmp_path):
    """VISIBILITY, which is the actual criterion. Before this the word
    'planned' never appeared in the gate's output at all."""
    committed = tmp_path / "committed.csv"
    runtime = tmp_path / "runtime.csv"
    _committed_with_planned(committed, planned=2)
    _write_csv(runtime, [("row_a", "green"), ("row_b", "green"),
                         ("row_c", "green")])
    healthy, msgs = evaluate(committed, runtime, floor_delta=5)
    blob = "\n".join(msgs)
    assert "row_planned_0" in blob and "row_planned_1" in blob, \
        "planned rows are still invisible in the gate's own report"
    assert "NOT-YET-DONE rows:        2" in blob
    # CONTROL: merely reporting them must NOT fail the gate. Un-started work
    # is not a regression, and a gate that reddened on it would be turned off.
    assert healthy, "reporting planned rows must not by itself fail the gate"


def test_planned_ceiling_fails_when_exceeded(tmp_path):
    committed = tmp_path / "committed.csv"
    runtime = tmp_path / "runtime.csv"
    _committed_with_planned(committed, planned=3)
    _write_csv(runtime, [("row_a", "green"), ("row_b", "green"),
                         ("row_c", "green")])
    healthy, msgs = evaluate(committed, runtime, floor_delta=5, max_planned=2)
    assert not healthy, "the planned ratchet did not fire at 3 > 2"
    assert any("exceed the pinned ceiling" in m for m in msgs)

    # CONTROL 1 — the SAME matrix at the ceiling must pass. Without this, a
    # ratchet that failed unconditionally would satisfy the assertion above.
    healthy_at, _ = evaluate(committed, runtime, floor_delta=5, max_planned=3)
    assert healthy_at, "the ratchet fires at or below the ceiling too"

    # CONTROL 2 — with no ceiling supplied the gate must behave exactly as
    # before, so existing callers are unaffected.
    healthy_off, _ = evaluate(committed, runtime, floor_delta=5)
    assert healthy_off


def test_planned_row_that_ran_green_is_reported_as_promotable(tmp_path):
    """The strict-xfail analogue. A not-yet-done row recording green is an
    UNEXPECTED PASS; swallowing it is how a row sits for months describing
    finished work — which is exactly what happened to the five sign-dialog
    rows GitHub #142 names."""
    committed = tmp_path / "committed.csv"
    runtime = tmp_path / "runtime.csv"
    _committed_with_planned(committed, planned=2)
    _write_csv(runtime, [("row_a", "green"), ("row_b", "green"),
                         ("row_c", "green"),
                         ("row_planned_0", "green")])   # unexpected pass

    healthy, msgs = evaluate(committed, runtime, floor_delta=5)
    blob = "\n".join(msgs)
    assert "NOT-YET-DONE but GREEN this run: 1" in blob
    assert "row_planned_0" in blob
    # CONTROL — reported, but not fatal, unless the caller asks for it.
    assert healthy

    strict, msgs2 = evaluate(committed, runtime, floor_delta=5,
                             fail_on_promotable=True)
    assert not strict, "--fail-on-promotable did not fail on an unexpected pass"
    assert any("recorded green this run" in m for m in msgs2)

    # CONTROL — with no promotable row, the strict flag must not fail. A flag
    # that reddens a clean matrix is worse than no flag.
    clean_runtime = tmp_path / "clean.csv"
    _write_csv(clean_runtime, [("row_a", "green"), ("row_b", "green"),
                               ("row_c", "green"),
                               ("row_planned_0", "no-run")])
    ok, _ = evaluate(committed, clean_runtime, floor_delta=5,
                     fail_on_promotable=True)
    assert ok


def test_wont_fix_and_xfail_are_not_counted_as_not_yet_done(tmp_path):
    """`wont-fix` is a decision and `xfail` has a driving test that RUNS.
    Counting either as backlog would inflate the census until nobody read it
    — the failure mode of the path-hygiene guard that matched 365 files."""
    committed = tmp_path / "committed.csv"
    runtime = tmp_path / "runtime.csv"
    _write_csv(committed, [("row_a", "green"), ("row_w", "wont-fix"),
                           ("row_x", "xfail"), ("row_s", "skip")])
    _write_csv(runtime, [("row_a", "green")])
    healthy, msgs = evaluate(committed, runtime, floor_delta=5, max_planned=0)
    blob = "\n".join(msgs)
    assert "NOT-YET-DONE rows:        0 (planned=0)" in blob
    assert healthy, "a ceiling of 0 fired on rows that are not backlog"
