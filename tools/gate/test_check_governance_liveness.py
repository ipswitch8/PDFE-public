#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Self-tests for tools/gate/check_governance_liveness.py (meta-audit F44).

WIRING (load-bearing). ``pytest.ini`` sets ``testpaths = tests``, so a
self-test living under ``tools/gate/`` runs **NOWHERE** unless it is named
explicitly. This file is named in the *"Gate self-tests + matrix
pass-semantics"* step of ``.github/workflows/fast-checks.yml``. If that line is
removed, this file stops running and the alarm it guards becomes unverified --
which is precisely the F38-b failure mode, one level up.

WHY THESE FIXTURES, AND WHERE THEY CAME FROM
============================================
This phase builds an ALARM. An alarm that cannot fire is worse than no alarm,
because it manufactures confidence. So the discrimination controls are driven
by the REAL observed failure, captured verbatim, not by a synthetic case
designed to be easy to detect.

* ``fixtures/governance-runs-real-queued-cancelled-2026-08-06.json``
  -- **REAL.** ``gh run list --workflow=governance.yml --json
  databaseId,name,workflowName,status,conclusion,createdAt,startedAt,updatedAt,event,headBranch``
  captured 2026-08-08, sliced to the records that existed on 2026-08-06 (the
  four nightly runs F38 observed). Not one field is edited. It contains run
  **30959077894** itself: created 2026-08-04T23:10:16Z, updated
  2026-08-05T23:09:27Z -- **1439 minutes** -- ``cancelled``, under
  ``timeout-minutes: 90``. This is the fixture criterion 2 requires.

* ``fixtures/governance-runs-real-2026-08-08.json``
  -- **REAL, verbatim, complete.** All six runs GitHub still lists for this
  workflow. Zero successes in the entire visible history.

* ``fixtures/fast-checks-runs-real-healthy-2026-08-08.json``
  -- **REAL.** The healthy side is real data too: ``Fast Checks`` on
  GitHub-hosted ubuntu, which does succeed. Using a real healthy history for
  the OK case means the OK path is not a shape invented by the same author who
  invented the ALARM path.

* ``fixtures/governance-runs-synthetic-healthy.json``
  -- **SYNTHETIC, and labelled so in its filename.** A healthy *governance*
  history cannot be captured from live data because one has never existed.
  Six consecutive nightly successes on the 02:00 cron, execution ~4 min. It
  exists to prove OK is reachable for the actual target workflow.

* ``fixtures/governance-runs-synthetic-stale-success.json`` -- **SYNTHETIC.**
  The lane worked, then stopped: a success on 2026-08-02 followed by four
  queued-then-cancelled nights. Separates ``stale-success`` from
  ``never-succeeded`` and proves the freshness window is load-bearing.

* ``governance-runs-empty.json`` / ``governance-runs-other-workflow-only.json``
  / ``governance-runs-malformed.json`` -- the fail-open traps. ``[]`` is
  exactly what ``gh run list --workflow=governance.yml --status=success``
  returns today.
"""
from __future__ import annotations

import datetime as dt
import json
import subprocess
import sys
from pathlib import Path

import pytest

_HERE = Path(__file__).resolve().parent
_REPO = _HERE.parents[1]
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))

import check_governance_liveness as C  # noqa: E402

FIXTURES = _HERE / "fixtures"

REAL_ALARM = FIXTURES / "governance-runs-real-queued-cancelled-2026-08-06.json"
REAL_CURRENT = FIXTURES / "governance-runs-real-2026-08-08.json"
REAL_HEALTHY_FC = FIXTURES / "fast-checks-runs-real-healthy-2026-08-08.json"
SYN_HEALTHY = FIXTURES / "governance-runs-synthetic-healthy.json"
SYN_STALE = FIXTURES / "governance-runs-synthetic-stale-success.json"
EMPTY = FIXTURES / "governance-runs-empty.json"
OTHER_ONLY = FIXTURES / "governance-runs-other-workflow-only.json"
MALFORMED = FIXTURES / "governance-runs-malformed.json"

# A fixed "now" makes every verdict below deterministic: 2026-08-08T22:00Z,
# shortly after the newest real record in these captures.
NOW = dt.datetime(2026, 8, 8, 22, 0, 0, tzinfo=dt.timezone.utc)


def _classify(path, workflow="Governance", now=NOW, **kw):
    return C.classify(C.read_runs_file(path), workflow=workflow, now=now, **kw)


# ---------------------------------------------------------------------------
# THE DISCRIMINATION CONTROLS -- the point of the whole file
# ---------------------------------------------------------------------------

def test_real_observed_queued_then_cancelled_history_ALARMS():
    """Criterion 2's required control: the alarm fires on the real failure."""
    v = _classify(REAL_ALARM)
    assert v.state == C.STATE_ALARM, v.detail
    assert v.reason == C.REASON_NEVER_SUCCEEDED, v.reason
    assert C.PATTERN_QUEUED_THEN_CANCELLED in v.patterns, v.patterns


def test_real_alarm_fixture_actually_contains_run_30959077894_with_a_24h_span():
    """The fixture must reproduce the OBSERVED pattern, not merely be red.

    A fixture that alarms for some other reason would let this phase claim a
    control it does not have. Assert the specific record, and re-derive its
    queue span rather than trusting the filename.
    """
    runs = {r.run_id: r for r in C.read_runs_file(REAL_ALARM)}
    assert 30959077894 in runs, sorted(runs)
    r = runs[30959077894]
    assert r.conclusion == "cancelled"
    assert r.status == "completed"
    # governance.yml's timeout-minutes is 90. 1439 minutes of "cancelled"
    # cannot be execution.
    assert 1435 <= r.span_minutes <= 1445, r.span_minutes
    assert r.span_minutes >= C.DEFAULT_QUEUE_CANCEL_MINUTES


def test_real_healthy_history_of_a_real_workflow_is_OK():
    v = _classify(REAL_HEALTHY_FC, workflow="Fast Checks")
    assert v.state == C.STATE_OK, v.detail
    assert v.reason == C.REASON_RECENT_SUCCESS


def test_synthetic_healthy_governance_history_is_OK():
    v = _classify(SYN_HEALTHY)
    assert v.state == C.STATE_OK, v.detail
    assert v.patterns == [], v.patterns


def test_full_real_current_history_still_ALARMS_never_succeeded():
    """As of 2026-08-08 the governance lane has NEVER succeeded, so the live
    tree's own data must alarm. If this ever flips to OK, the lane recovered
    and this test is the record of when."""
    v = _classify(REAL_CURRENT)
    assert v.state == C.STATE_ALARM
    assert v.reason == C.REASON_NEVER_SUCCEEDED
    assert C.PATTERN_QUEUED_THEN_CANCELLED in v.patterns


# ---------------------------------------------------------------------------
# FAIL-CLOSED -- "no data" must never read as healthy
# ---------------------------------------------------------------------------

def test_empty_history_ALARMS_and_is_not_a_vacuous_pass():
    v = _classify(EMPTY)
    assert v.state == C.STATE_ALARM
    assert v.reason == C.REASON_NO_RUNS


def test_history_with_zero_matching_runs_ALARMS():
    """A NON-empty history that contains no run of the target workflow is the
    subtler vacuous-pass trap: plenty of data, none of it evidence."""
    v = _classify(OTHER_ONLY)
    assert v.state == C.STATE_ALARM
    assert v.reason == C.REASON_NO_RUNS
    # ...and the same file IS healthy for the workflow it actually describes,
    # proving the filter is doing the work rather than the file being inert.
    assert _classify(OTHER_ONLY, workflow="Fast Checks").state == C.STATE_OK


def test_malformed_input_is_an_ERROR_not_a_verdict():
    with pytest.raises(C.LivenessError):
        C.read_runs_file(MALFORMED)


def test_non_array_json_is_an_ERROR():
    with pytest.raises(C.LivenessError):
        C.parse_runs('{"conclusion": "success"}')


def test_missing_required_field_is_an_ERROR_not_a_skipped_row():
    with pytest.raises(C.LivenessError):
        C.parse_runs(json.dumps([{"workflowName": "Governance", "status": "completed",
                                  "conclusion": "success",
                                  "createdAt": "2026-08-08T02:00:00Z"}]))


def test_unparseable_timestamp_is_an_ERROR():
    with pytest.raises(C.LivenessError):
        C.parse_runs(json.dumps([{"workflowName": "Governance", "status": "completed",
                                  "conclusion": "success", "createdAt": "yesterday",
                                  "updatedAt": "2026-08-08T02:00:00Z"}]))


def test_missing_file_is_an_ERROR():
    with pytest.raises(C.LivenessError):
        C.read_runs_file(FIXTURES / "no-such-fixture-should-exist.json")


# ---------------------------------------------------------------------------
# THE FRESHNESS WINDOW IS LOAD-BEARING, AND IS A PARAMETER
# ---------------------------------------------------------------------------

def test_stale_success_ALARMS_distinctly_from_never_succeeded():
    v = _classify(SYN_STALE)
    assert v.state == C.STATE_ALARM
    assert v.reason == C.REASON_STALE_SUCCESS, v.reason
    assert C.PATTERN_QUEUED_THEN_CANCELLED in v.patterns


def test_the_same_stale_history_is_OK_under_a_wider_stated_window():
    """The window is a stated parameter, not a magic number: widening it to
    cover the 164 h age flips the verdict, so the threshold is what decides."""
    v = _classify(SYN_STALE, max_success_age_hours=200.0)
    assert v.state == C.STATE_OK, v.detail


def test_healthy_history_ALARMS_once_the_window_is_tightened_below_its_age():
    """The mirror control. The healthy fixture is not unconditionally green --
    it is green because a success is recent, and tightening the window past
    that recency turns it red."""
    v = _classify(SYN_HEALTHY, max_success_age_hours=1.0)
    assert v.state == C.STATE_ALARM
    assert v.reason == C.REASON_STALE_SUCCESS


def test_default_window_is_48_hours_and_tolerates_exactly_one_missed_day():
    assert C.DEFAULT_MAX_SUCCESS_AGE_HOURS == 48.0
    runs = C.parse_runs(json.dumps([{
        "databaseId": 1, "workflowName": "Governance", "status": "completed",
        "conclusion": "success", "createdAt": "2026-08-06T02:00:00Z",
        "updatedAt": "2026-08-06T02:04:00Z"}]))
    # 47 h old -> one missed nightly run plus jitter: still OK.
    assert C.classify(runs, now=dt.datetime(2026, 8, 8, 1, 0, tzinfo=dt.timezone.utc)).state \
        == C.STATE_OK
    # 49 h old -> two missed nightly runs: ALARM.
    assert C.classify(runs, now=dt.datetime(2026, 8, 8, 3, 0, tzinfo=dt.timezone.utc)).state \
        == C.STATE_ALARM


def test_zero_or_negative_window_is_rejected_rather_than_silently_accepted():
    with pytest.raises(C.LivenessError):
        C.classify(C.read_runs_file(SYN_HEALTHY), max_success_age_hours=0.0)


# ---------------------------------------------------------------------------
# A SUCCESS MUST BE A REAL SUCCESS
# ---------------------------------------------------------------------------

def test_a_queued_run_is_not_a_success_even_if_recent():
    runs = C.parse_runs(json.dumps([{
        "databaseId": 7, "workflowName": "Governance", "status": "queued",
        "conclusion": None, "createdAt": "2026-08-08T02:00:00Z",
        "updatedAt": "2026-08-08T02:00:00Z"}]))
    v = C.classify(runs, now=NOW)
    assert v.state == C.STATE_ALARM
    assert v.reason == C.REASON_NEVER_SUCCEEDED
    assert C.PATTERN_CURRENTLY_QUEUED in v.patterns


def test_a_success_conclusion_on_a_non_completed_run_is_not_counted():
    """Defence against a shape gh should never emit but which must not read as
    healthy if it ever does."""
    runs = C.parse_runs(json.dumps([{
        "databaseId": 8, "workflowName": "Governance", "status": "in_progress",
        "conclusion": "success", "createdAt": "2026-08-08T02:00:00Z",
        "updatedAt": "2026-08-08T02:01:00Z"}]))
    assert C.classify(runs, now=NOW).state == C.STATE_ALARM


def test_workflow_match_is_case_insensitive_but_not_substring():
    runs = C.read_runs_file(SYN_HEALTHY)
    assert C.classify(runs, workflow="governance", now=NOW).state == C.STATE_OK
    # "Gov" must NOT match "Governance": a loose match would let an unrelated
    # workflow's success stand in as evidence for this one.
    assert C.classify(runs, workflow="Gov", now=NOW).reason == C.REASON_NO_RUNS


# ---------------------------------------------------------------------------
# CLI CONTRACT -- exit codes are what a workflow step actually consumes
# ---------------------------------------------------------------------------

def _cli(*args):
    return subprocess.run(
        [sys.executable, str(_HERE / "check_governance_liveness.py"), *args],
        capture_output=True, text=True, encoding="utf-8", cwd=str(_REPO))


def test_cli_exit_1_on_the_real_alarm_fixture():
    p = _cli("--runs", str(REAL_ALARM), "--now", "2026-08-08T22:00:00Z")
    assert p.returncode == 1, p.stdout + p.stderr
    assert "ALARM" in p.stdout


def test_cli_exit_0_on_a_healthy_fixture():
    p = _cli("--runs", str(SYN_HEALTHY), "--now", "2026-08-08T22:00:00Z")
    assert p.returncode == 0, p.stdout + p.stderr
    assert "OK" in p.stdout


def test_cli_exit_2_on_malformed_input():
    p = _cli("--runs", str(MALFORMED), "--now", "2026-08-08T22:00:00Z")
    assert p.returncode == 2, p.stdout + p.stderr
    assert "ERROR" in p.stderr


def test_cli_exit_2_when_no_source_is_given():
    p = _cli("--now", "2026-08-08T22:00:00Z")
    assert p.returncode == 2, p.stdout + p.stderr


def test_cli_json_payload_is_machine_readable():
    p = _cli("--runs", str(REAL_ALARM), "--now", "2026-08-08T22:00:00Z", "--json")
    assert p.returncode == 1
    payload = json.loads(p.stdout)
    assert payload["state"] == "ALARM"
    assert payload["reason"] == "never-succeeded"
    assert "queued-then-cancelled" in payload["patterns"]


# ---------------------------------------------------------------------------
# THE FIXTURE-MODE AND LIVE-MODE PATHS MUST BE THE SAME CODE
# ---------------------------------------------------------------------------

def test_live_mode_reuses_the_same_parse_and_classify_functions(monkeypatch):
    """Guards the design guidance: a fixture test that exercised a parallel
    parser would prove nothing about production. Feed the REAL captured gh
    stdout through the live path with the subprocess stubbed, and require the
    identical verdict the fixture path produces."""
    captured = REAL_ALARM.read_text(encoding="utf-8")

    class _Proc:
        returncode = 0
        stdout = captured
        stderr = ""

    seen = {}

    def _fake_run(argv, **kw):
        seen["argv"] = argv
        return _Proc()

    monkeypatch.setattr(C.subprocess, "run", _fake_run)
    live = C.classify(C.fetch_runs_via_gh("governance.yml", 40, "gh"), now=NOW)
    fixture = _classify(REAL_ALARM)
    assert (live.state, live.reason, live.patterns) == \
           (fixture.state, fixture.reason, fixture.patterns)
    assert "--workflow" in seen["argv"] and "governance.yml" in seen["argv"]


def test_live_mode_surfaces_a_gh_failure_as_an_ERROR_not_an_OK(monkeypatch):
    class _Proc:
        returncode = 4
        stdout = ""
        stderr = "gh: could not authenticate"

    monkeypatch.setattr(C.subprocess, "run", lambda argv, **kw: _Proc())
    with pytest.raises(C.LivenessError):
        C.fetch_runs_via_gh("governance.yml", 40, "gh")


# ---------------------------------------------------------------------------
# THE ALARM MUST BE HOSTED OFF THE MACHINE IT PROBES
# ---------------------------------------------------------------------------

def test_the_liveness_workflow_exists_and_does_not_run_on_the_probed_host():
    """A probe hosted on the machine it monitors cannot report that machine
    being down -- the whole reason F38-b went unseen. Assert mechanically that
    the lane invoking this checker is NOT on the self-hosted runner."""
    import yaml
    wf = _REPO / ".github" / "workflows" / "ci-liveness.yml"
    assert wf.exists(), "ci-liveness.yml is missing: the alarm has no host"
    doc = yaml.safe_load(wf.read_text(encoding="utf-8"))
    jobs = doc["jobs"]
    assert jobs, "ci-liveness.yml defines no jobs"
    for name, job in jobs.items():
        runs_on = job.get("runs-on")
        flat = runs_on if isinstance(runs_on, str) else " ".join(runs_on or [])
        assert "self-hosted" not in flat, (
            "ci-liveness job %r runs on %r -- an alarm hosted on the runner it "
            "probes inherits the outage it exists to detect (F38-b)." % (name, runs_on))
        assert "pdfe" not in flat.lower(), (
            "ci-liveness job %r targets the probed host: %r" % (name, runs_on))


def test_the_liveness_workflow_invokes_this_checker_in_live_mode():
    wf = (_REPO / ".github" / "workflows" / "ci-liveness.yml").read_text(encoding="utf-8")
    assert "tools/gate/check_governance_liveness.py" in wf
    assert "--from-gh" in wf, (
        "ci-liveness.yml must run the LIVE mode; a fixture-only lane would "
        "assert nothing about the real runner.")


def test_this_selftest_is_named_in_the_fast_checks_gate_selftests_step():
    """pytest.ini's testpaths=tests means this file runs nowhere unless it is
    named. F35/F37/F41/F43 all had to add themselves; assert it rather than
    remember it."""
    wf = (_REPO / ".github" / "workflows" / "fast-checks.yml").read_text(encoding="utf-8")
    assert "tools/gate/test_check_governance_liveness.py" in wf, (
        "this self-test is not named in fast-checks.yml, so it does not run in "
        "CI (pytest.ini: testpaths = tests)")
