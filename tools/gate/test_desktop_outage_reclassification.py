#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Tests for the mid-test desktop-outage reclassification chain.

WHY THIS NEEDS ITS OWN TESTS
----------------------------
This mechanism only ever fires when an RDP session drops WHILE a test is
running. Nothing in the ordinary suite reaches it, on any machine, ever. Left
untested it would be a claim -- and a claim of exactly the shape this repo keeps
catching: code that looks like a safety net, has never run, and is discovered to
be inert at the moment it was supposed to work.

The window boundaries come from a real measurement, not a guess: GitHub #132,
`docs/audit/issue-132/desktop-probe-2026-09-03.log`. On reconnect the session
reports Active ELEVEN SECONDS before OpenInputDesktop recovers, which is why the
overlap check carries a 12 s grace on the end edge.
"""
from __future__ import annotations

import datetime as _dt
import io
import json
import os
import sys
import tempfile

import pytest

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
if REPO not in sys.path:
    sys.path.insert(0, REPO)

from tests.helpers import desktop_outage as outage  # noqa: E402


def _journal(tmpdir, records):
    path = os.path.join(tmpdir, "desktop-outages.jsonl")
    with io.open(path, "w", encoding="utf-8") as fh:
        for rec in records:
            fh.write(json.dumps(rec) + "\n")
    return path


def _at(base, seconds):
    return (base + _dt.timedelta(seconds=seconds)).isoformat()


@pytest.fixture()
def base():
    return _dt.datetime(2026, 9, 3, 19, 0, 0).astimezone()


# ---------------------------------------------------------------------------
# Journal parsing
# ---------------------------------------------------------------------------

def test_a_closed_outage_is_read_as_one_window(base, tmp_path):
    p = _journal(str(tmp_path), [
        {"event": "outage-start", "at": _at(base, 45)},
        {"event": "outage-end", "at": _at(base, 146)},
    ])
    windows = outage.read_outages(p)
    assert len(windows) == 1
    assert (windows[0][1] - windows[0][0]).total_seconds() == 101


def test_an_open_outage_is_ongoing_not_discarded(base, tmp_path):
    """The normal state DURING an outage is a start with no end yet. Treating
    that as unparseable would make the mechanism blind in exactly the window it
    exists for."""
    p = _journal(str(tmp_path), [{"event": "outage-start", "at": _at(base, 45)}])
    windows = outage.read_outages(p)
    assert len(windows) == 1 and windows[0][1] is None


def test_a_half_written_final_line_does_not_discard_earlier_records(base, tmp_path):
    """The watchdog appends while the test session reads. A torn last line is
    expected, and must not cost us the records before it."""
    p = _journal(str(tmp_path), [
        {"event": "outage-start", "at": _at(base, 45)},
        {"event": "outage-end", "at": _at(base, 146)},
    ])
    with io.open(p, "a", encoding="utf-8") as fh:
        fh.write('{"event": "outage-st')
    assert len(outage.read_outages(p)) == 1


def test_two_starts_with_no_end_keep_the_earlier(base, tmp_path):
    """A watchdog restart mid-outage. Widening the window can only make the
    mechanism more conservative, which is the safe direction."""
    p = _journal(str(tmp_path), [
        {"event": "outage-start", "at": _at(base, 45)},
        {"event": "outage-start", "at": _at(base, 90)},
        {"event": "outage-end", "at": _at(base, 146)},
    ])
    windows = outage.read_outages(p)
    assert len(windows) == 1
    assert windows[0][0] == base.replace(microsecond=0) + _dt.timedelta(seconds=45)


def test_a_missing_journal_is_no_outage_not_an_error():
    """Every developer machine and CI box. Must be a silent no-op."""
    assert outage.read_outages(os.path.join(tempfile.gettempdir(), "nope-xyz.jsonl")) == []


def test_powershell_seven_digit_fractional_seconds_parse():
    """PowerShell's "o" format emits 7 fractional digits; datetime takes 6.
    Getting this wrong would make every record unparseable and the whole
    mechanism silently inert -- which looks identical to 'no outage'."""
    stamp = outage._parse_stamp("2026-09-03T19:00:45.7912345+00:00")
    assert stamp is not None and stamp.second == 45


# ---------------------------------------------------------------------------
# Overlap, including the measured 11-second recovery lag
# ---------------------------------------------------------------------------

def test_a_test_running_across_the_outage_overlaps(base, tmp_path):
    p = _journal(str(tmp_path), [
        {"event": "outage-start", "at": _at(base, 45)},
        {"event": "outage-end", "at": _at(base, 146)},
    ])
    assert outage.outage_overlapping(base + _dt.timedelta(seconds=30),
                                     base + _dt.timedelta(seconds=60), p) is not None


def test_a_test_finishing_before_the_outage_does_not_overlap(base, tmp_path):
    p = _journal(str(tmp_path), [
        {"event": "outage-start", "at": _at(base, 45)},
        {"event": "outage-end", "at": _at(base, 146)},
    ])
    assert outage.outage_overlapping(base, base + _dt.timedelta(seconds=20), p) is None


def test_a_test_starting_in_the_eleven_second_recovery_gap_still_overlaps(base, tmp_path):
    """THE MEASURED CASE (#132). The journal closes the outage when the probe
    recovers, but a test starting a few seconds later can still have begun
    while the desktop was unusable. Without the grace this is the hole the
    mechanism would leak through."""
    p = _journal(str(tmp_path), [
        {"event": "outage-start", "at": _at(base, 45)},
        {"event": "outage-end", "at": _at(base, 146)},
    ])
    start = base + _dt.timedelta(seconds=152)   # 6 s after the journal closed
    assert outage.outage_overlapping(start, start + _dt.timedelta(seconds=5), p) is not None


def test_well_after_the_grace_does_not_overlap(base, tmp_path):
    """The grace must not be unbounded, or every later failure gets excused."""
    p = _journal(str(tmp_path), [
        {"event": "outage-start", "at": _at(base, 45)},
        {"event": "outage-end", "at": _at(base, 146)},
    ])
    start = base + _dt.timedelta(seconds=200)
    assert outage.outage_overlapping(start, start + _dt.timedelta(seconds=5), p) is None


def test_an_open_outage_overlaps_everything_after_it(base, tmp_path):
    p = _journal(str(tmp_path), [{"event": "outage-start", "at": _at(base, 45)}])
    start = base + _dt.timedelta(seconds=900)
    assert outage.outage_overlapping(start, start + _dt.timedelta(seconds=5), p) is not None


# ---------------------------------------------------------------------------
# Reclassification
# ---------------------------------------------------------------------------

def _matrix():
    from tests.ui_automation import conftest as ui
    return ui


def test_a_red_during_an_outage_becomes_blocked():
    ui = _matrix()
    ui.MATRIX_RESULTS.clear()
    ui.record_matrix_result("m::t", "red", "canvas not found")
    assert ui.reclassify_blocked_by_outage("m::t", "no desktop 19:00-19:02") is True
    assert ui.MATRIX_RESULTS["m::t"]["status"] == "blocked"
    assert "canvas not found" in ui.MATRIX_RESULTS["m::t"]["notes"]
    assert "reclassified red->blocked" in ui.MATRIX_RESULTS["m::t"]["notes"]


def test_a_green_during_an_outage_is_left_alone():
    """If a test passed with no desktop, that is a fact about the TEST worth
    keeping visible. Rewriting it to blocked would destroy the one signal that
    would expose it."""
    ui = _matrix()
    ui.MATRIX_RESULTS.clear()
    ui.record_matrix_result("m::t", "green", "ok")
    assert ui.reclassify_blocked_by_outage("m::t", "no desktop") is False
    assert ui.MATRIX_RESULTS["m::t"]["status"] == "green"


def test_normal_precedence_still_forbids_a_plain_red_to_blocked():
    """The bypass must be the explicit function ONLY. If ordinary
    record_matrix_result could downgrade a red, the precedence rule that stops a
    late skip burying a real failure would be gone."""
    ui = _matrix()
    ui.MATRIX_RESULTS.clear()
    ui.record_matrix_result("m::t", "red", "real failure")
    ui.record_matrix_result("m::t", "blocked", "late skip")
    assert ui.MATRIX_RESULTS["m::t"]["status"] == "red"


def test_id_derived_from_a_nodeid_matches_the_stack_walk_format():
    """Two derivations of the same id is a drift bug: they agree until someone
    changes one, and then reclassification writes to an id nothing reads --
    invisible, because an unwritten matrix row looks like an unreached one."""
    ui = _matrix()
    assert ui.matrix_id_for_nodeid(
        "tests/test_ui_canvas_sendinput.py::test_canvas_drag_top_to_bottom"
    ) == "test_ui_canvas_sendinput::test_canvas_drag_top_to_bottom"
    # Parametrisation is stripped: current_test_id reads a frame name, which
    # never carries one.
    assert ui.matrix_id_for_nodeid("tests/test_x.py::test_y[case1]") == "test_x::test_y"
    assert ui.matrix_id_for_nodeid("garbage") == "<unknown-test>"


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-q"]))
