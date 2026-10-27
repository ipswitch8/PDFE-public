# -*- coding: utf-8 -*-
"""F70(b) — a shard that died without XML must not feed the healed ledger.

WHY THIS EXISTS. When a shard exits without emitting JUnit XML, run_gtest
synthesises an `<error>` testcase for every test assigned to it, so the work
does not silently vanish from the aggregate. Those then look like failures, are
re-run individually, and PASS -- because they never ran in the first place --
and used to be written to `healed_flakes.json` as "healed on rerun".

Measured once: one dead shard put **348** healthy, never-executed tests in
there. That file FEEDS A GATE. `check_flaky_quarantine.py` fails when a healed
test is not in `flaky_quarantine.json`, so the contaminated ledger left it with
two bad exits and no good one -- fail with 348 untracked heals, or register 348
healthy tests as known flakes and destroy the registry's discriminating value.

These cases pin the corrected behaviour, including the part that is easy to
over-fix: a never-run test that ALSO fails its rerun is a real failure and must
still be reported.
"""
from __future__ import annotations

import importlib.util
import json
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
_SPEC = importlib.util.spec_from_file_location(
    "run_gtest_under_test", ROOT / "tests" / "scripts" / "run_gtest.py")
assert _SPEC and _SPEC.loader
run_gtest = importlib.util.module_from_spec(_SPEC)
sys.modules["run_gtest_under_test"] = run_gtest
_SPEC.loader.exec_module(run_gtest)


def _ledger(tmp_path: Path) -> dict:
    return json.loads((tmp_path / "healed_flakes.json").read_text(encoding="utf-8"))


def test_never_run_ids_are_kept_out_of_healed(tmp_path):
    """THE DEFECT. A dead shard's partition 'heals' on rerun; it must not count."""
    dead = {"A.One", "A.Two", "B.Three"}
    run_gtest._write_healed_ledger(
        tmp_path,
        healed=["A.One", "A.Two", "B.Three", "C.GenuinelyFlaky"],
        unresolved=[],
        never_ran=dead,
    )
    led = _ledger(tmp_path)
    assert led["healed"] == ["C.GenuinelyFlaky"], (
        "a never-executed test was recorded as healed-on-rerun, which is the "
        "contamination F70(b) is about: " + repr(led["healed"]))
    assert led["not_run_due_to_dead_shard"] == sorted(dead)


def test_a_genuinely_flaky_test_still_reaches_the_ledger(tmp_path):
    """The control. Over-filtering would silently retire the gate entirely."""
    run_gtest._write_healed_ledger(
        tmp_path, healed=["C.GenuinelyFlaky"], unresolved=[], never_ran=set())
    assert _ledger(tmp_path)["healed"] == ["C.GenuinelyFlaky"]


def test_a_never_run_test_that_also_fails_its_rerun_is_still_reported(tmp_path):
    """Do NOT hide real failures while removing the contamination.

    A test from a dead shard that fails its rerun too is a genuine failure. It
    stays in `unresolved`; only the HEALED side is filtered.
    """
    run_gtest._write_healed_ledger(
        tmp_path,
        healed=[],
        unresolved=["A.ReallyBroken"],
        never_ran={"A.ReallyBroken"},
    )
    led = _ledger(tmp_path)
    assert led["unresolved"] == ["A.ReallyBroken"], (
        "a real failure was suppressed because it came from a dead shard")
    assert led["not_run_due_to_dead_shard"] == ["A.ReallyBroken"]


def test_the_old_behaviour_is_what_would_fail_this(tmp_path):
    """Pin the pre-fix behaviour explicitly, so the case cannot quietly stop
    measuring anything if the filter is later removed: with no `never_ran`
    supplied at all, the dead-shard ids DO land in `healed`. That is exactly the
    contaminated state, reproduced deliberately."""
    run_gtest._write_healed_ledger(
        tmp_path, healed=["A.One", "A.Two"], unresolved=[], never_ran=None)
    assert _ledger(tmp_path)["healed"] == ["A.One", "A.Two"]
    assert _ledger(tmp_path)["not_run_due_to_dead_shard"] == []


def test_the_quarantine_gate_accepts_the_corrected_ledger(tmp_path):
    """End to end against the REAL gate, not a re-implementation of it.

    The whole point of the fix is what `check_flaky_quarantine.py` does with the
    file, so this drives that checker: the contaminated ledger must FAIL it and
    the corrected one must pass, with the same inputs.
    """
    spec = importlib.util.spec_from_file_location(
        "check_flaky_quarantine_under_test",
        ROOT / "tools" / "gate" / "check_flaky_quarantine.py")
    assert spec and spec.loader
    gate = importlib.util.module_from_spec(spec)
    sys.modules["check_flaky_quarantine_under_test"] = gate
    spec.loader.exec_module(gate)

    registry = json.loads(
        (ROOT / "tools" / "gate" / "flaky_quarantine.json").read_text(
            encoding="utf-8"))
    import datetime as _dt
    today = _dt.date(2026, 1, 1)  # before every expiry in the registry

    contaminated_ok, _ = gate.evaluate(registry, ["A.One", "A.Two"], today)
    assert not contaminated_ok, (
        "the gate ACCEPTED never-run tests as healed flakes; if this passes, "
        "the contamination would have gone unnoticed and the fix is pointless")

    corrected_ok, _ = gate.evaluate(registry, [], today)
    assert corrected_ok, "the gate rejected a correctly-filtered empty ledger"
