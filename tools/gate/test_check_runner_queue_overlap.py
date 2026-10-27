#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Self-tests for tools/gate/check_runner_queue_overlap.py (meta-audit F38).

NOTE ON WIRING: ``pytest.ini`` sets ``testpaths = tests``, so a self-test
living under ``tools/gate/`` runs NOWHERE unless it is named explicitly. This
file is named in the "Gate self-tests" step of
``.github/workflows/fast-checks.yml``. If that line is ever removed, this file
stops running and the check it guards becomes unverified.

The load-bearing tests are the two discrimination controls:

* :func:`test_prefix_fixture_FAILS` — the committed pre-F38 snapshot
  (governance at 22:00) must FAIL, or the check has stopped discriminating.
* :func:`test_live_configuration_PASSES` — the live tree must PASS, or the
  fix regressed.

Both run against the REAL sibling workflow files, so a future edit to any
lane's cron or timeout is exercised by this suite rather than only by CI.
"""
from __future__ import annotations

import datetime as _dt
import io
import sys
from pathlib import Path

import pytest
import yaml

_HERE = Path(__file__).resolve().parent
_REPO = _HERE.parents[1]
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))

import check_runner_queue_overlap as C  # noqa: E402

WF = _REPO / ".github" / "workflows"
FIXTURES = _HERE / "fixtures"
PREFIX_FIXTURE = FIXTURES / "governance-2200-prefix.yml"

# FROZEN, NOT LIVE — 2026-08-13. These used to be the live workflow files. Every
# `schedule:` trigger was removed that day (docs/audit/ci-manual-only-posture.md)
# and the model went empty, taking SEVENTEEN discrimination controls with it: the
# arithmetic that proves this checker can derive a 120-minute tail was resting on
# a policy that changed under it. It rests on committed snapshots now.
# The SUBDIRECTORY is load-bearing: build_lanes() derives a lane's NAME from the
# file STEM, so a snapshot called "windows-sanitizer-scheduled-pre-....yml" is a
# lane named "windows-sanitizer-scheduled-pre-...", which no assertion here can
# find. Measured: five StopIterations. Keep the stems, move the directory.
FROZEN = FIXTURES / "scheduled-lanes-pre-2026-08-13"

OTHER_LANES = [
    FROZEN / "windows-sanitizer.yml",
    FROZEN / "mutation-kill.yml",
    FROZEN / "windows-fuzz.yml",
    FROZEN / "windows-validation.yml",
]

# The same four plus governance at its post-F38 02:00 slot: the configuration the
# "must PASS" controls assert against, now that there is no live scheduled one.
SCHEDULED_LANES = [FROZEN / "governance.yml"] + OTHER_LANES


def _lane(name, crons, timeout):
    return C.Lane(name, crons if isinstance(crons, list) else [crons], timeout)


def _doc(text):
    return yaml.safe_load(io.StringIO(text))


# ---------------------------------------------------------------------------
# THE DISCRIMINATION CONTROLS — these are the point of the whole file
# ---------------------------------------------------------------------------

def test_prefix_fixture_exists():
    assert PREFIX_FIXTURE.is_file(), (
        "the committed pre-F38 snapshot is missing; without it nothing "
        "demonstrates the check can fail")


def test_prefix_fixture_FAILS():
    """governance at 22:00 sits 120 min inside two lanes' tails -> FAIL."""
    lanes, problems = C.build_lanes([PREFIX_FIXTURE] + OTHER_LANES)
    ok, msgs = C.evaluate(lanes, problems, target="governance-2200-prefix")
    blob = "\n".join(msgs)
    assert not ok, "the pre-F38 configuration must FAIL:\n" + blob
    assert "FAIL(pairwise)" in blob, blob
    assert "120" in blob, "expected the 120-minute tail to be derived:\n" + blob


def test_prefix_fixture_fails_on_BOTH_named_cases():
    """Criterion 2: sanitizer-vs-governance AND mutation-vs-governance."""
    lanes, _ = C.build_lanes([PREFIX_FIXTURE] + OTHER_LANES)
    tails = C.pairwise_worst_tails(lanes, "governance-2200-prefix")
    assert tails["windows-sanitizer"][0] == 120.0, tails
    assert tails["mutation-kill"][0] == 120.0, tails


def test_scheduled_configuration_PASSES():
    """RE-POINTED 2026-08-13. There is no live SCHEDULED configuration any more,
    and asserting a model built from an empty one would be a control that cannot
    fail. This asserts the last scheduled configuration still passes the model."""
    lanes, problems = C.build_lanes(SCHEDULED_LANES)
    ok, msgs = C.evaluate(lanes, problems)
    assert ok, "the live configuration must PASS:\n" + "\n".join(msgs)


def test_scheduled_governance_has_zero_pairwise_exposure():
    lanes, _ = C.build_lanes(SCHEDULED_LANES)
    tails = C.pairwise_worst_tails(lanes, "governance")
    assert all(v[0] == 0.0 for v in tails.values()), tails


def test_live_main_exits_zero_and_fixture_main_exits_one():
    """End-to-end through main(), the way CI invokes it."""
    assert C.main([]) == 0
    argv = ["--workflow", str(PREFIX_FIXTURE), "--target", "governance-2200-prefix"]
    for p in OTHER_LANES:
        argv += ["--workflow", str(p)]
    assert C.main(argv) == 1


# ---------------------------------------------------------------------------
# the live lane values are what the check claims they are
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("name,cron,timeout", [
    ("governance", "0 2 * * *", 90),
    ("windows-sanitizer", "0 18 * * 0,2,4", 360),
    ("mutation-kill", "0 20 * * 6", 240),
    ("windows-fuzz", "30 3 * * *", 600),
    ("windows-validation", "20 4 * * *", 720),
])
def test_frozen_lane_values(name, cron, timeout):
    """Pins the values the pairwise/cascade arithmetic was verified against."""
    lanes, _ = C.build_lanes(SCHEDULED_LANES)
    lane = next(l for l in lanes if l.name == name)
    assert lane.crons == [cron]
    assert lane.timeout_minutes == timeout


def test_governance_snapshot_is_not_2200():
    lanes, _ = C.build_lanes([FROZEN / "governance.yml"])
    assert lanes[0].crons == ["0 2 * * *"], (
        "F38 moved governance off 22:00; if this changed back, the 120-minute "
        "exposure is back with it")


# ---------------------------------------------------------------------------
# pairwise model
# ---------------------------------------------------------------------------

def test_pairwise_tail_basic_overlap():
    lanes = [_lane("blocker", "0 18 * * *", 360), _lane("t", "0 22 * * *", 90)]
    assert C.pairwise_worst_tails(lanes, "t")["blocker"][0] == 120.0


def test_pairwise_tail_zero_when_blocker_finished():
    lanes = [_lane("blocker", "0 18 * * *", 360), _lane("t", "0 2 * * *", 90)]
    assert C.pairwise_worst_tails(lanes, "t")["blocker"][0] == 0.0


def test_pairwise_tail_zero_when_blocker_starts_later():
    lanes = [_lane("blocker", "30 3 * * *", 600), _lane("t", "0 2 * * *", 90)]
    assert C.pairwise_worst_tails(lanes, "t")["blocker"][0] == 0.0


def test_pairwise_tail_respects_day_of_week():
    """A Saturday-only blocker cannot delay a target that never fires then."""
    lanes = [_lane("sat", "0 20 * * 6", 240), _lane("t", "0 22 * * 1", 90)]
    assert C.pairwise_worst_tails(lanes, "t")["sat"][0] == 0.0


def test_pairwise_tail_scales_with_timeout():
    a = [_lane("b", "0 18 * * *", 300), _lane("t", "0 22 * * *", 90)]
    b = [_lane("b", "0 18 * * *", 420), _lane("t", "0 22 * * *", 90)]
    assert C.pairwise_worst_tails(a, "t")["b"][0] == 60.0
    assert C.pairwise_worst_tails(b, "t")["b"][0] == 180.0


def test_pairwise_target_absent_yields_empty():
    assert C.pairwise_worst_tails([_lane("x", "0 1 * * *", 10)], "nope") == {}


# ---------------------------------------------------------------------------
# subscription model
# ---------------------------------------------------------------------------

def test_subscription_single_daily_lane():
    got = C.daily_subscription_minutes([_lane("a", "0 1 * * *", 600)])
    assert got == pytest.approx(600.0)


def test_subscription_weekly_lane_is_amortised():
    got = C.daily_subscription_minutes([_lane("a", "0 1 * * 6", 700)])
    assert got == pytest.approx(100.0, abs=1.0)


def test_live_subscription_is_over_a_day():
    """F38's second finding: the shared runner is over-subscribed."""
    lanes, _ = C.build_lanes(SCHEDULED_LANES)
    sub = C.daily_subscription_minutes(lanes)
    assert sub > 1440.0, sub
    assert sub <= C.DEFAULT_MAX_DAILY_SUBSCRIPTION_MINUTES, sub


def test_subscription_ceiling_is_a_real_gate():
    """Lengthening a timeout past the accepted ceiling must FAIL."""
    lanes, _ = C.build_lanes(SCHEDULED_LANES)
    for l in lanes:
        if l.name == "windows-fuzz":
            l.timeout_minutes = 900          # +300 min/day
    ok, msgs = C.evaluate(lanes, [])
    assert not ok
    assert "FAIL(subscription)" in "\n".join(msgs)


def test_subscription_zero_span_is_not_a_crash():
    assert C.daily_subscription_minutes([_lane("a", "0 1 * * *", 10)],
                                        span_days=0) == 0.0


# ---------------------------------------------------------------------------
# cascade model (diagnostic, but must still be right)
# ---------------------------------------------------------------------------

def test_cascade_no_delay_when_runner_is_free():
    lanes = [_lane("a", "0 1 * * *", 60), _lane("b", "0 5 * * *", 60)]
    d = C.serial_cascade_delays(lanes)
    assert d["a"] == 0.0 and d["b"] == 0.0


def test_cascade_delays_the_second_lane():
    lanes = [_lane("a", "0 1 * * *", 120), _lane("b", "0 2 * * *", 30)]
    assert C.serial_cascade_delays(lanes)["b"] == 60.0


def test_cascade_is_reported_but_never_fails_the_check():
    """A divergent cascade must NOT be a gate — it would be permanently red."""
    lanes, _ = C.build_lanes(SCHEDULED_LANES)
    cascade = C.serial_cascade_delays(lanes)
    assert max(cascade.values()) > C.DEFAULT_QUEUE_CANCEL_MINUTES
    ok, msgs = C.evaluate(lanes, [])
    assert ok, "the cascade must be diagnostic only:\n" + "\n".join(msgs)
    assert "DIAGNOSTIC ONLY" in "\n".join(msgs)


# ---------------------------------------------------------------------------
# anti-cheat: an uninterpretable configuration must FAIL, never pass silently
# ---------------------------------------------------------------------------

def test_no_lanes_is_a_failure_not_a_vacuous_pass():
    ok, msgs = C.evaluate([], [])
    assert not ok and "no lane" in "\n".join(msgs)


def test_missing_target_is_a_failure():
    ok, msgs = C.evaluate([_lane("other", "0 1 * * *", 10)], [], target="governance")
    assert not ok and "not among" in "\n".join(msgs)


def test_upstream_problems_short_circuit_to_failure():
    ok, msgs = C.evaluate([_lane("governance", "0 1 * * *", 10)], ["boom"])
    assert not ok and "boom" in "\n".join(msgs)


def test_shared_runner_job_without_timeout_is_a_problem():
    doc = _doc("""
name: x
on:
  schedule:
    - cron: '0 1 * * *'
jobs:
  j:
    runs-on: [self-hosted, Windows, X64, pdfe-windows]
    steps: []
""")
    lane, problems = C.lane_from_document("x", doc)
    assert lane is None
    assert problems and "timeout-minutes" in problems[0]


def test_non_integer_timeout_is_a_problem():
    doc = _doc("""
name: x
on:
  schedule:
    - cron: '0 1 * * *'
jobs:
  j:
    runs-on: [self-hosted, pdfe-windows]
    timeout-minutes: "soon"
""")
    lane, problems = C.lane_from_document("x", doc)
    assert lane is None and problems


def test_uninterpretable_cron_is_a_failure_not_an_assumed_pass():
    doc = _doc("""
name: x
on:
  schedule:
    - cron: 'every tuesday'
jobs:
  j:
    runs-on: [self-hosted, pdfe-windows]
    timeout-minutes: 30
""")
    lane, problems = C.lane_from_document("x", doc)
    assert lane is None
    assert problems and "uninterpretable" in problems[0]


def test_job_not_on_shared_runner_is_excluded_silently():
    doc = _doc("""
name: x
on:
  schedule:
    - cron: '0 1 * * *'
jobs:
  j:
    runs-on: ubuntu-latest
    timeout-minutes: 30
""")
    lane, problems = C.lane_from_document("x", doc)
    assert lane is None and problems == []


def test_dispatch_only_shared_lane_contributes_no_collision():
    doc = _doc("""
name: x
on:
  workflow_dispatch:
jobs:
  j:
    runs-on: [self-hosted, pdfe-windows]
    timeout-minutes: 30
""")
    lane, problems = C.lane_from_document("x", doc)
    assert lane is None and problems == []


def test_mixed_workflow_uses_only_the_shared_job_timeout():
    """windows-fuzz.yml has an ubuntu job at timeout 10; it must be ignored."""
    doc = _doc("""
name: x
on:
  schedule:
    - cron: '30 3 * * *'
jobs:
  heavy:
    runs-on: [self-hosted, Windows, X64, pdfe-windows]
    timeout-minutes: 600
  light:
    runs-on: ubuntu-latest
    timeout-minutes: 10
""")
    lane, problems = C.lane_from_document("x", doc)
    assert problems == []
    assert lane.timeout_minutes == 600


def test_live_fuzz_lane_ignores_its_ubuntu_job():
    lanes, _ = C.build_lanes([FROZEN / "windows-fuzz.yml"])
    assert lanes[0].timeout_minutes == 600, "the ubuntu-latest job's 10-minute " \
        "timeout must not be mistaken for the shared-runner lane's"


# ---------------------------------------------------------------------------
# the PyYAML `on:` -> True trap, which has bitten this project before
# ---------------------------------------------------------------------------

def test_bare_on_key_parses_as_boolean_true():
    """Guard the premise: if PyYAML ever stopped doing this the code changes."""
    assert True in _doc("on:\n  schedule:\n    - cron: '0 1 * * *'\n")


def test_on_block_found_despite_the_boolean_key():
    doc = _doc("""
name: x
on:
  schedule:
    - cron: '0 1 * * *'
jobs:
  j:
    runs-on: [self-hosted, pdfe-windows]
    timeout-minutes: 30
""")
    assert "on" not in doc            # the trap
    lane, problems = C.lane_from_document("x", doc)
    assert problems == [] and lane.crons == ["0 1 * * *"]


def test_quoted_on_key_also_works():
    doc = _doc("""
name: x
"on":
  schedule:
    - cron: '0 1 * * *'
jobs:
  j:
    runs-on: [self-hosted, pdfe-windows]
    timeout-minutes: 30
""")
    lane, problems = C.lane_from_document("x", doc)
    assert problems == [] and lane.crons == ["0 1 * * *"]


# ---------------------------------------------------------------------------
# runs-on shapes
# ---------------------------------------------------------------------------

def test_runs_on_plain_string():
    assert C._runs_on_labels("ubuntu-latest") == ["ubuntu-latest"]


def test_runs_on_list():
    assert "pdfe-windows" in C._runs_on_labels(["self-hosted", "pdfe-windows"])


def test_runs_on_group_labels_mapping():
    labels = C._runs_on_labels({"group": "g", "labels": ["pdfe-windows"]})
    assert "pdfe-windows" in labels and "g" in labels


def test_runs_on_none():
    assert C._runs_on_labels(None) == []


# ---------------------------------------------------------------------------
# CRLF: core.autocrlf=true must not change the verdict
# ---------------------------------------------------------------------------

def test_crlf_workflow_parses_identically(tmp_path):
    text = ("name: x\non:\n  schedule:\n    - cron: '0 1 * * *'\n"
            "jobs:\n  j:\n    runs-on: [self-hosted, pdfe-windows]\n"
            "    timeout-minutes: 30\n")
    lf = tmp_path / "lf.yml"
    crlf = tmp_path / "crlf.yml"
    with io.open(lf, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)
    with io.open(crlf, "w", encoding="utf-8", newline="\r\n") as fh:
        fh.write(text)
    a, _ = C.build_lanes([lf])
    b, _ = C.build_lanes([crlf])
    assert a[0].crons == b[0].crons
    assert a[0].timeout_minutes == b[0].timeout_minutes


def test_unreadable_workflow_is_a_problem(tmp_path):
    _, problems = C.build_lanes([tmp_path / "nope.yml"])
    assert problems and "cannot read" in problems[0]


# ---------------------------------------------------------------------------
# the check is derived from LIVE values, not from a hard-coded number
# ---------------------------------------------------------------------------

def test_moving_governance_back_to_2200_reintroduces_the_failure():
    """The decisive property: the verdict tracks the cron, not a constant."""
    lanes, _ = C.build_lanes(SCHEDULED_LANES)
    for l in lanes:
        if l.name == "governance":
            l.crons = ["0 22 * * *"]
    ok, msgs = C.evaluate(lanes, [])
    assert not ok
    assert "FAIL(pairwise)" in "\n".join(msgs)


def test_lengthening_a_blocker_tail_into_the_slot_fails():
    lanes, _ = C.build_lanes(SCHEDULED_LANES)
    for l in lanes:
        if l.name == "windows-sanitizer":
            l.timeout_minutes = 600          # 18:00 -> 04:00, over 02:00
    ok, msgs = C.evaluate(lanes, [], max_daily_subscription_minutes=10 ** 6)
    assert not ok
    assert "FAIL(pairwise)" in "\n".join(msgs)


def test_bound_of_zero_would_still_pass_the_live_config():
    lanes, _ = C.build_lanes(SCHEDULED_LANES)
    ok, _ = C.evaluate(lanes, [], max_queue_minutes=0)
    assert ok, "the live exposure is exactly 0, so even a zero bound holds"


def test_span_covers_saturday_and_sun_tue_thu():
    """A span that missed a weekly lane would silently under-report."""
    start = C.DEFAULT_SPAN_START
    assert start.weekday() == 6, "span must start on a Sunday"
    sat = C.CronSpec("0 20 * * 6").firings(start, C.DEFAULT_SPAN_DAYS)
    stt = C.CronSpec("0 18 * * 0,2,4").firings(start, C.DEFAULT_SPAN_DAYS)
    assert len(sat) == 10 and len(stt) == 30


def test_docstring_records_the_verified_timeout_semantics():
    """The premise this phase turned on must stay written down."""
    doc = C.__doc__
    assert "30959077894" in doc, "the run id that measured it must be cited"
    assert "does not run while queued" in doc or "not run while queued" in doc


# ---------------------------------------------------------------------------
# The MANUAL-ONLY posture (2026-08-13) and the honesty of the frozen fixtures.
# ---------------------------------------------------------------------------

def test_the_live_tree_really_has_no_scheduled_lane():
    """The frozen fixtures above are only honest while the LIVE tree is
    genuinely unscheduled. Restore a cron and this fails, which is the signal
    that the snapshots have stopped describing anything current."""
    lanes, problems = C.build_lanes([Path(p) for p in C.DEFAULT_WORKFLOWS])
    assert lanes == [], lanes
    assert not problems, problems


def test_evaluate_still_refuses_an_empty_model():
    """The decision core must NOT know about the posture. This is the control
    that briefly broke when the posture was consulted inside evaluate(): every
    'an empty model is not a pass' assertion started passing, because the
    subject of the question was answering it."""
    ok, msgs = C.evaluate([], [], target="governance")
    assert not ok
    assert any("An empty model is not a pass" in m for m in msgs)


def test_main_passes_on_the_posture_but_only_through_main():
    """main() exits 0 on the live (unscheduled) tree via the posture, while the
    pure core still says no. Both halves are asserted so neither can drift."""
    assert C.main([]) == 0
    lanes, problems = C.build_lanes([Path(p) for p in C.DEFAULT_WORKFLOWS])
    assert not C.evaluate(lanes, problems)[0]


def test_posture_refused_when_a_cron_returns(monkeypatch):
    """If a schedule comes back while the model is still empty, that is a
    derivation failure and must stay a FAIL rather than borrowing the posture."""
    import check_asan_cadence as A
    monkeypatch.setattr(A, "_workflows_with_schedule", lambda: ["governance.yml"])
    assert C.main([]) == 1
