#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""End-to-end: prove the pytest HOOK fires, not just that its helpers work.

WHY SEPARATE FROM THE UNIT TESTS
--------------------------------
test_desktop_outage_reclassification.py exercises the journal reader, the
overlap window and the reclassifier by calling them directly. Every one of those
would still pass if the `pytest_runtest_logreport` hook in tests/conftest.py
were never wired up, misnamed, or swallowed by its own `except`. That is the
"a harness that names a defect class is a CLAIM" failure this repo has hit
repeatedly: parts verified, integration assumed.

So this runs a REAL pytest session in a subprocess, with a real journal on disk
covering the run's own wall-clock window, and asserts on what the session
actually reported.
"""
from __future__ import annotations

import datetime as _dt
import io
import json
import os
import subprocess
import sys
import tempfile

import pytest

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

FAILING_TEST = '''
def test_pretends_the_canvas_was_unreachable():
    # Stands in for a live-UI test whose UIA lookups all miss because the
    # desktop went away mid-run.
    assert False, "pdfCanvas not found"
'''

PASSING_TEST = '''
def test_unrelated_pass():
    assert True
'''


def _write_journal(path, start_offset, end_offset):
    """A journal whose window is relative to NOW, so it covers this run."""
    now = _dt.datetime.now().astimezone()
    records = [{"event": "outage-start",
                "at": (now + _dt.timedelta(seconds=start_offset)).isoformat()}]
    if end_offset is not None:
        records.append({"event": "outage-end",
                        "at": (now + _dt.timedelta(seconds=end_offset)).isoformat()})
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with io.open(path, "w", encoding="utf-8") as fh:
        for rec in records:
            fh.write(json.dumps(rec) + "\n")


def _run_session(tmpdir, journal_path, body=FAILING_TEST):
    # The probe test MUST live inside the repo's tests/ tree. pytest only loads
    # a conftest from the test file's ancestor directories, so a probe in %TEMP%
    # never sees tests/conftest.py -- and the hook under test lives there. The
    # first version of this harness did exactly that: every unit test passed,
    # this one failed, and the "failure" was the harness, not the hook. Worth
    # keeping in mind, because the same mistake in the other direction (probe
    # outside the tree, assertion inverted) would have reported the whole
    # mechanism as working while nothing ran.
    probe_dir = os.path.join(REPO, "tests", "_tmp", "outage_probe")
    os.makedirs(probe_dir, exist_ok=True)
    test_file = os.path.join(probe_dir, "test_outage_probe.py")
    io.open(test_file, "w", encoding="utf-8").write(body)
    env = dict(os.environ)
    if journal_path is None:
        env.pop("PDFE_DESKTOP_OUTAGE_LOG", None)
    else:
        env["PDFE_DESKTOP_OUTAGE_LOG"] = journal_path
    try:
        p = subprocess.run(
            [sys.executable, "-m", "pytest", "-p", "no:cacheprovider", test_file, "-v"],
            cwd=REPO, env=env, capture_output=True, text=True, timeout=600)
        return p.stdout + p.stderr
    finally:
        try:
            os.remove(test_file)
        except OSError:
            pass


def test_a_failure_during_an_outage_is_reported_as_blocked():
    """The whole point. The failure must still be a failure, and must be
    labelled as not-evidence-of-a-defect."""
    with tempfile.TemporaryDirectory() as d:
        journal = os.path.join(d, "PDFE", "desktop-outages.jsonl")
        _write_journal(journal, start_offset=-120, end_offset=None)  # ongoing
        out = _run_session(d, journal)
    assert "FAILURES DURING A DESKTOP OUTAGE" in out, out[-4000:]
    assert "test_pretends_the_canvas_was_unreachable" in out
    assert "do not read them as passes" in out


def test_the_test_still_fails_it_is_not_excused():
    """Reclassification must not turn a red into a green. With no desktop the
    test genuinely could not be driven; passing it would be a lie in the other
    direction."""
    with tempfile.TemporaryDirectory() as d:
        journal = os.path.join(d, "PDFE", "desktop-outages.jsonl")
        _write_journal(journal, start_offset=-120, end_offset=None)
        out = _run_session(d, journal)
    assert "1 failed" in out, out[-3000:]
    assert "1 passed" not in out


def test_no_outage_means_no_reclassification_and_no_noise():
    """The everyday case on every developer machine and CI box. An ordinary
    failure must look exactly as it did before this mechanism existed."""
    with tempfile.TemporaryDirectory() as d:
        journal = os.path.join(d, "PDFE", "desktop-outages.jsonl")
        _write_journal(journal, start_offset=-9000, end_offset=-8000)  # long past
        out = _run_session(d, journal)
    assert "FAILURES DURING A DESKTOP OUTAGE" not in out, out[-3000:]
    assert "1 failed" in out


def test_a_missing_journal_is_silent():
    """No journal at all -- the state of every machine that never runs the
    watchdog. Must not warn, must not crash, must not mention outages."""
    with tempfile.TemporaryDirectory() as d:
        out = _run_session(d, os.path.join(d, "does-not-exist.jsonl"))
    assert "FAILURES DURING A DESKTOP OUTAGE" not in out
    assert "1 failed" in out
    assert "Traceback" not in out, out[-3000:]


def test_a_passing_test_during_an_outage_is_not_touched():
    with tempfile.TemporaryDirectory() as d:
        journal = os.path.join(d, "PDFE", "desktop-outages.jsonl")
        _write_journal(journal, start_offset=-120, end_offset=None)
        out = _run_session(d, journal, body=PASSING_TEST)
    assert "1 passed" in out
    assert "FAILURES DURING A DESKTOP OUTAGE" not in out


def test_a_corrupt_journal_does_not_break_the_run():
    """Bookkeeping that can fail a test run is worse than no bookkeeping."""
    with tempfile.TemporaryDirectory() as d:
        journal = os.path.join(d, "PDFE", "desktop-outages.jsonl")
        os.makedirs(os.path.dirname(journal), exist_ok=True)
        io.open(journal, "w", encoding="utf-8").write("not json at all\n{\"event\": \n")
        out = _run_session(d, journal)
    assert "1 failed" in out
    assert "Traceback" not in out, out[-3000:]


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-q"]))
