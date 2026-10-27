# -*- coding: utf-8 -*-
"""Pins for redact_tracker_text.py's manifest MERGE.

WHY THIS FILE EXISTS. The manifest writer originally rebuilt
tracker_redactions.json from only the current run's records. The first time a
SECOND redaction pass ran (p100 phase-1, 2026-10-24), it would have dropped the
three records from the first pass — converting documented redactions back into
unexplained hash mismatches for verify_tracker.py, which reads this file to
know which issues are EXPECTED to differ from the source export. The merge fix
was proved by that one real run (2 records merged, 3 survived); p100's
claim-check gate correctly flagged that a single real run is evidence, not a
standing check. This is the standing check.
"""
from __future__ import annotations

import importlib.util
import os

_MIG = os.path.join(os.path.dirname(os.path.dirname(
    os.path.dirname(os.path.abspath(__file__)))), "scripts", "migration")


def _load():
    import sys
    if _MIG not in sys.path:
        sys.path.insert(0, _MIG)   # redact_tracker_text imports repo_ids
    spec = importlib.util.spec_from_file_location(
        "rtt", os.path.join(_MIG, "redact_tracker_text.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


rtt = _load()


def _manifest():
    """The real pre-p100 shape: three records from the first pass."""
    return {
        "repo": "ipswitch8/PDFE-migration",
        "_why": "existing why text",
        "redacted": [
            {"issue": 146, "needle_labels": ["build-account"], "reason": "r1"},
            {"issue": 261, "needle_labels": ["customer-b-spaced",
                                             "customer-c"], "reason": "r2"},
            {"issue": 277, "needle_labels": ["customer-a"], "reason": "r3"},
        ],
    }


def test_a_second_pass_does_not_drop_the_first_passes_records():
    out = rtt.merge_manifest(_manifest(), [
        {"issue": 146, "needle_labels": ["build-host"], "reason": "r4"},
        {"issue": 277, "needle_labels": ["build-host"], "reason": "r5"},
    ], "ipswitch8/PDFE")
    issues = {r["issue"] for r in out["redacted"]}
    # 261 was in neither new record and MUST survive — this is the exact loss
    # the overwrite bug would have caused.
    assert issues == {146, 261, 277}


def test_labels_are_unioned_not_replaced():
    out = rtt.merge_manifest(_manifest(), [
        {"issue": 146, "needle_labels": ["build-host"], "reason": "r4"},
    ], "ipswitch8/PDFE")
    rec = {r["issue"]: r for r in out["redacted"]}[146]
    assert rec["needle_labels"] == ["build-account", "build-host"]


def test_a_new_issue_is_appended():
    out = rtt.merge_manifest(_manifest(), [
        {"issue": 999, "needle_labels": ["operator-name"], "reason": "r9"},
    ], "ipswitch8/PDFE")
    issues = {r["issue"] for r in out["redacted"]}
    assert 999 in issues and len(issues) == 4


def test_records_are_ordered_by_issue_number():
    out = rtt.merge_manifest(_manifest(), [
        {"issue": 5, "needle_labels": ["x"], "reason": "r"},
    ], "ipswitch8/PDFE")
    nums = [r["issue"] for r in out["redacted"]]
    assert nums == sorted(nums)


def test_repo_is_updated_and_why_is_preserved():
    out = rtt.merge_manifest(_manifest(), [], "ipswitch8/PDFE")
    assert out["repo"] == "ipswitch8/PDFE"
    assert out["_why"] == "existing why text"


def test_an_empty_manifest_gains_the_why_text():
    out = rtt.merge_manifest({}, [
        {"issue": 1, "needle_labels": ["a"], "reason": "r"},
    ], "o/r")
    assert "_why" in out and out["redacted"][0]["issue"] == 1
