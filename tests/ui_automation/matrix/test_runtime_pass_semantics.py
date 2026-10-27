# -*- coding: utf-8 -*-
"""Runtime matrix pass-semantics tests (pipeline e5b9c231 Phase 5 — criterion 1).

Proves that runtime *pass* means RAN-AND-ASSERTED-THIS-RUN: a committed-green
row that does not record a result this session must NOT survive as ``green`` in
the runtime CSV — it becomes ``no-run``. This is the honesty fix for the H4
"runtime CSV rebuilt from committed green" defect.

Run:
    python -m pytest tests/ui_automation/matrix/test_runtime_pass_semantics.py -q
"""
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from runtime_status import compute_runtime_rows, NO_RUN  # noqa: E402


def _committed():
    """Three committed rows: one green, one green, one wont-fix."""
    return [
        {"test_id": "a_ran_green", "status": "green", "notes": "closeout"},
        {"test_id": "b_committed_green_not_run", "status": "green", "notes": "closeout"},
        {"test_id": "c_wontfix", "status": "wont-fix", "notes": "by design"},
    ]


def test_committed_green_not_run_this_session_becomes_no_run():
    """The core defect: a committed-green row that did not run must not stay green."""
    results = {"a_ran_green": {"status": "green", "notes": "asserted now"}}
    out = {r["test_id"]: r for r in compute_runtime_rows(_committed(), results)}

    # The row that actually ran + asserted stays green.
    assert out["a_ran_green"]["status"] == "green"
    assert "asserted now" in out["a_ran_green"]["notes"]

    # The committed-green row that did NOT run this session is 'no-run',
    # NOT the inherited 'green'. This is the assertion the old overlay failed.
    assert out["b_committed_green_not_run"]["status"] == NO_RUN, (
        "stale committed green leaked into the runtime CSV — runtime 'pass' "
        "no longer means 'asserted this run'")


def test_wont_fix_is_preserved_verbatim():
    """Permanent, documented dispositions survive; they are not 'no-run'."""
    out = {r["test_id"]: r for r in compute_runtime_rows(_committed(), {})}
    assert out["c_wontfix"]["status"] == "wont-fix"


def test_recorded_red_and_blocked_pass_through():
    """A genuine red/blocked recorded this session is written as-is."""
    results = {
        "a_ran_green": {"status": "red", "notes": "regressed"},
        "b_committed_green_not_run": {"status": "blocked", "notes": "env"},
    }
    out = {r["test_id"]: r for r in compute_runtime_rows(_committed(), results)}
    assert out["a_ran_green"]["status"] == "red"
    assert out["b_committed_green_not_run"]["status"] == "blocked"


def test_inputs_are_not_mutated():
    """compute_runtime_rows must be pure — the committed rows stay untouched."""
    committed = _committed()
    before = [dict(r) for r in committed]
    compute_runtime_rows(committed, {"a_ran_green": {"status": "red", "notes": "x"}})
    assert committed == before, "compute_runtime_rows mutated its input rows"


def test_green_in_output_iff_recorded_green_this_session():
    """Post-condition sweep: every green output row is backed by a session green."""
    results = {
        "a_ran_green": {"status": "green", "notes": ""},
        # b not recorded, c is wont-fix
    }
    out = compute_runtime_rows(_committed(), results)
    for row in out:
        if row["status"] == "green":
            tid = row["test_id"]
            assert results.get(tid, {}).get("status") == "green", (
                f"row {tid} is green in runtime CSV but did not record green "
                f"this session")


# ---------------------------------------------------------------------------
# GitHub #171 — a live green on a wont-fix row must SURVIVE write-back.
#
# The defect: the preserve branch ran before `results` was consulted, so the
# committed status beat the observed one and an expired wont-fix reason could
# never self-correct. Note the NEGATIVE controls below are what keep the fix
# from being "wont-fix no longer means anything": a wont-fix row that did not
# run, and one that ran and AGREED (red/blocked), are both still preserved
# verbatim. Only a contradiction moves the row.
# ---------------------------------------------------------------------------


def test_wont_fix_row_recording_green_is_promoted_not_discarded():
    """POSITIVE CONTROL: the expired-reason case #171 is about."""
    results = {"c_wontfix": {"status": "green", "notes": "passed on this run"}}
    out = {r["test_id"]: r for r in compute_runtime_rows(_committed(), results)}
    assert out["c_wontfix"]["status"] == "green", (
        "a live green on a wont-fix row was discarded — an expired wont-fix "
        "reason still cannot self-correct (GitHub #171)")
    assert "passed on this run" in out["c_wontfix"]["notes"]
    assert "#171" in out["c_wontfix"]["notes"], (
        "the disagreement must be FLAGGED in the artifact, not merely applied")


def test_wont_fix_row_not_run_is_still_preserved_verbatim():
    """NEGATIVE CONTROL: nothing observed, so there is nothing to correct."""
    committed = _committed()
    before = {r["test_id"]: dict(r) for r in committed}
    out = {r["test_id"]: r
           for r in compute_runtime_rows(committed,
                                         {"a_ran_green": {"status": "green",
                                                          "notes": ""}})}
    assert out["c_wontfix"] == before["c_wontfix"], (
        "a wont-fix row that did not run this session must pass through "
        "completely unchanged")


def test_wont_fix_row_recording_red_or_blocked_is_preserved_verbatim():
    """NEGATIVE CONTROL: the observation AGREES, so the reason is still valid.

    Only a pass contradicts a wont-fix. Writing red/blocked back would inflate
    the run's failure counts with rows nobody expects to pass, and would say
    nothing new.
    """
    committed = _committed()
    before = {r["test_id"]: dict(r) for r in committed}
    for observed in ("red", "blocked"):
        out = {r["test_id"]: r for r in compute_runtime_rows(
            _committed(), {"c_wontfix": {"status": observed, "notes": "n"}})}
        assert out["c_wontfix"] == before["c_wontfix"], (
            f"a wont-fix row observed {observed} must be preserved verbatim")


def test_health_checker_reports_a_promoted_wont_fix_row():
    """The write-back fix is only useful if something READS the disagreement.

    Shares the #142 promotable mechanism rather than growing a second one.
    """
    sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
    from check_matrix_health import promotable_rows  # noqa: E402

    committed = {"c_wontfix": "wont-fix", "p": "planned", "g": "green"}
    runtime = {"c_wontfix": "green", "p": "green", "g": "green"}
    rows = promotable_rows(committed, runtime)
    assert ("c_wontfix", "wont-fix") in rows, (
        "a wont-fix row passing this run is an unexpected pass and must be "
        "reported (GitHub #171)")
    assert ("p", "planned") in rows, "POSITIVE CONTROL: #142's case still works"
    assert ("g", "green") not in rows, (
        "NEGATIVE CONTROL: an already-green row is not 'promotable'")
