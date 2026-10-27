# -*- coding: utf-8 -*-
"""The suite-totals checker must catch skip drift (GitHub #276).

The gap it closes: nothing recorded the suite's size or skip set in a form a
script reads, so "skips at or below the current 15-skip baseline" was satisfiable
by fourteen new skips appearing, and "15" itself traced only to a project note.

These tests pin the behaviour that makes it worth having -- including the
controls, because a checker that passes everything and a checker that works look
identical on a clean tree.
"""
from __future__ import annotations

import io
import json
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
CHECKER = HERE / "check_suite_totals.py"
BASELINE = HERE / "suite_totals_baseline.json"


def run(*args):
    proc = subprocess.run([sys.executable, str(CHECKER), *args],
                          capture_output=True, text=True,
                          encoding="utf-8", errors="replace")
    return proc.returncode, (proc.stdout or "") + (proc.stderr or "")


def write_xml(path: Path, cases, tests=None, skipped=None):
    """A minimal merged-gtest-shaped XML. `cases` is [(classname, name, skip)]."""
    body = []
    for classname, name, skip in cases:
        if skip:
            body.append(
                '<testcase classname="%s" name="%s">'
                '<skipped message="because"/></testcase>' % (classname, name))
        else:
            body.append('<testcase classname="%s" name="%s"/>' % (classname, name))
    n_tests = len(cases) if tests is None else tests
    n_skip = sum(1 for c in cases if c[2]) if skipped is None else skipped
    xml = ('<?xml version="1.0"?>\n'
           '<testsuites tests="%d" failures="0" errors="0" skipped="%d">\n'
           '<testsuite name="s" tests="%d" failures="0" errors="0" skipped="%d">\n'
           '%s\n</testsuite>\n</testsuites>\n'
           % (n_tests, n_skip, n_tests, n_skip, "\n".join(body)))
    path.write_text(xml, encoding="utf-8")


BASE_CASES = [("A", "one", False), ("A", "two", False), ("B", "three", True)]


def seed(tmp_path):
    xml = tmp_path / "run.xml"
    base = tmp_path / "baseline.json"
    write_xml(xml, BASE_CASES)
    code, out = run("--xml", str(xml), "--baseline", str(base), "--update")
    assert code == 0, out
    return xml, base


def test_a_matching_run_passes(tmp_path):
    xml, base = seed(tmp_path)
    code, out = run("--xml", str(xml), "--baseline", str(base))
    assert code == 0, out
    assert "OK" in out


def test_a_new_skip_fails_even_though_the_count_is_still_small(tmp_path):
    """THE #276 CASE. A threshold of 'at or below N' would pass this; the point
    is that a test STARTED skipping and nothing said so."""
    xml, base = seed(tmp_path)
    write_xml(xml, [("A", "one", False), ("A", "two", True), ("B", "three", True)])
    code, out = run("--xml", str(xml), "--baseline", str(base))
    assert code == 1, out
    assert "SKIP APPEARED" in out and "A.two" in out, out


def test_a_vanished_skip_also_fails_so_the_baseline_stays_true(tmp_path):
    """Good news still has to be recorded, or the baseline slowly stops
    describing the suite and its next failure means nothing."""
    xml, base = seed(tmp_path)
    write_xml(xml, [("A", "one", False), ("A", "two", False), ("B", "three", False)])
    code, out = run("--xml", str(xml), "--baseline", str(base))
    assert code == 1, out
    assert "skip vanished" in out and "B.three" in out, out


def test_a_shrinking_suite_fails(tmp_path):
    """A test that stops being registered is invisible to a pass/fail count --
    the PDF-79 shape, where seventeen cases did not exist and the suite was
    green for months."""
    xml, base = seed(tmp_path)
    write_xml(xml, [("A", "one", False), ("B", "three", True)])
    code, out = run("--xml", str(xml), "--baseline", str(base))
    assert code == 1, out
    assert "SUITE SHRANK" in out, out


def test_a_growing_suite_is_not_a_failure(tmp_path):
    """Adding tests is the normal case and must not need a baseline update, or
    the check becomes something people route around."""
    xml, base = seed(tmp_path)
    write_xml(xml, BASE_CASES + [("C", "four", False)])
    code, out = run("--xml", str(xml), "--baseline", str(base))
    assert code == 0, out
    assert "grew by 1" in out, out


def test_env_prefixes_downgrade_a_known_environment_difference(tmp_path):
    """CI reported 15 skipped and a workstation 19 for the same commit, the
    difference being CNN-model tests whose model is absent locally. That must be
    expressible without blinding the check to everything else."""
    xml, base = seed(tmp_path)
    write_xml(xml, [("A", "one", False), ("A", "two", False),
                    ("B", "three", True), ("CNNFontClassifierTest", "x", True)])
    code, out = run("--xml", str(xml), "--baseline", str(base),
                    "--allow-env-skips", "CNNFontClassifierTest")
    assert code == 0, out
    assert "[env]" in out, out

    # ...and the same run WITHOUT the allowance must fail, or the allowance is
    # doing nothing and the test above proves nothing.
    code2, out2 = run("--xml", str(xml), "--baseline", str(base))
    assert code2 == 1, out2


def test_a_root_attribute_disagreeing_with_the_tree_is_refused(tmp_path):
    """Vacuity guard. If `skipped=` and the testcase elements disagree, one of
    them is lying and reporting either as the truth would be a confident wrong
    number -- the same lesson as trusting an exit code over the XML."""
    xml, base = seed(tmp_path)
    write_xml(xml, BASE_CASES, skipped=99)
    code, out = run("--xml", str(xml), "--baseline", str(base))
    assert code == 1, out
    assert "MISMATCH" in out, out


def test_the_committed_baseline_is_well_formed_and_populated():
    """The live source has to actually exist and carry the set, not just a
    number -- that distinction is the whole design."""
    data = json.loads(io.open(BASELINE, encoding="utf-8").read())
    assert isinstance(data.get("skipped"), list) and data["skipped"], data.keys()
    assert data["skipped_count"] == len(data["skipped"])
    assert data["tests"] > 6000, data["tests"]
    assert data["skipped"] == sorted(data["skipped"]), "must be sorted for stable diffs"
