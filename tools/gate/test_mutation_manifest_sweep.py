#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Self-test for the mutation-manifest staleness sweep (F31 / R-24).

Why this exists
---------------
`.github/workflows/fast-checks.yml` and `.github/workflows/governance.yml` both
run ``python tools/gate/mutation_kill.py --manifest tools/gate/mutants.json
--dry-run``. That dry-run IS the manifest staleness sweep: it enumerates every
mutant, locates its find-string in the cited source file, and (since F31)
reports a PASS/FAIL row per mutant. Both CI lanes went red on every push when
PDF-68 rewrote ``RedactionVerifier::checkSingleRevision`` out from under M11's
anchor and nothing noticed until a human read the log.

There is deliberately NO second, standalone sweep script: a duplicate checker
can drift from the one CI actually runs, and then the duplicate is green while
the real gate is red -- exactly the failure this phase is closing. Instead the
one real sweep gets a discriminating control, which is this file.

The controls
------------
* ``mutants_sweep_current.json``   -> every anchor present  -> all-PASS, exit 0
* ``mutants_sweep_stale.json``     -> one anchor absent     -> that row FAIL,
                                      the sibling row still PASS, exit 2
* ``mutants_sweep_ambiguous.json`` -> one anchor twice      -> FAIL, exit 2

A sweep that cannot fail proves nothing, so the stale/ambiguous halves are the
load-bearing assertions here; the current half proves it is not simply always
red.

Run:
    python -m pytest tools/gate/test_mutation_manifest_sweep.py -q
"""
from __future__ import annotations

import json
import os
import sys
from pathlib import Path

import pytest

GATE_DIR = Path(__file__).resolve().parent
REPO_ROOT = GATE_DIR.parent.parent
FIXTURES = GATE_DIR / "fixtures"

sys.path.insert(0, str(GATE_DIR))
import mutation_kill  # noqa: E402


@pytest.fixture(autouse=True)
def _at_repo_root(monkeypatch):
    """mutation_kill resolves mutant `file` paths against os.getcwd()."""
    monkeypatch.chdir(REPO_ROOT)


def _run(manifest_name, tmp_path):
    sweep_out = tmp_path / "sweep.json"
    rc = mutation_kill.main([
        "--manifest", str((FIXTURES / manifest_name).relative_to(REPO_ROOT)),
        "--dry-run",
        "--sweep-json", str(sweep_out),
    ])
    report = json.loads(sweep_out.read_text(encoding="utf-8"))
    return rc, report


def _row(report, mutant_id):
    for r in report["mutants"]:
        if r["id"] == mutant_id:
            return r
    raise AssertionError("no sweep row for %s in %s" % (mutant_id, report))


def test_fixture_target_is_present():
    """The controls are worthless if their anchor file has been deleted."""
    assert (FIXTURES / "mutant_sweep_target.cpp").is_file(), (
        "tools/gate/fixtures/mutant_sweep_target.cpp is gone; the sweep's "
        "discriminating control has no anchor text and both halves would "
        "fail for the wrong reason")


def test_current_manifest_sweeps_all_pass(tmp_path):
    rc, report = _run("mutants_sweep_current.json", tmp_path)
    assert rc == 0, "a manifest whose anchors are all present must exit 0"
    assert report["failed"] == 0
    assert report["passed"] == report["total"] == 2
    assert _row(report, "FX01-sweep-current-singleline")["status"] == "PASS"
    # The multi-line anchor is stored with "\n" while the tracked fixture file
    # is CRLF on disk (core.autocrlf=true). This row failing means the sweep
    # stopped normalising newlines -- which would silently declare the real,
    # multi-line M11 anchor stale.
    assert _row(report, "FX02-sweep-current-multiline")["status"] == "PASS"


def test_stale_manifest_is_detected_per_mutant(tmp_path):
    rc, report = _run("mutants_sweep_stale.json", tmp_path)
    assert rc == 2, ("a manifest with a stale anchor must fail the gate "
                     "(exit 2), got %r" % rc)
    stale = _row(report, "FX11-sweep-stale-anchor")
    assert stale["status"] == "FAIL"
    assert "find-string absent" in stale["detail"]
    # Per-mutant, not wholesale: the sibling entry must still be reported PASS,
    # otherwise the sweep cannot tell an operator WHICH mutant rotted.
    assert _row(report, "FX10-sweep-still-current")["status"] == "PASS"
    assert report["failed"] == 1 and report["passed"] == 1


def test_ambiguous_anchor_is_detected(tmp_path):
    rc, report = _run("mutants_sweep_ambiguous.json", tmp_path)
    assert rc == 2, "an anchor matching more than once must fail the gate"
    row = _row(report, "FX20-sweep-ambiguous-anchor")
    assert row["status"] == "FAIL"
    assert "ambiguous" in row["detail"]


def test_real_manifest_is_currently_clean(tmp_path):
    """The live gate itself: tools/gate/mutants.json must sweep all-PASS.

    This is the regression lock for F31. If a future refactor moves a mutant's
    anchor again, this fails locally instead of turning both CI lanes red.
    """
    rc, report = _run_real(tmp_path)
    assert rc == 0, "tools/gate/mutants.json is stale again: %s" % [
        r for r in report["mutants"] if r["status"] == "FAIL"]
    assert report["failed"] == 0

    # Every mutant in the manifest must have been swept, and all must pass.
    # Tied to the manifest rather than to a literal so that GROWING the mutant
    # set -- which the manifest's own _comment asks for -- does not redden this
    # lock; the floor below keeps it non-vacuous, because "0 swept, 0 failed"
    # would otherwise satisfy the equality.
    declared = json.loads(
        (REPO_ROOT / "tools" / "gate" / "mutants.json").read_text(
            encoding="utf-8"))["mutants"]
    assert report["total"] == report["passed"] == len(declared)
    assert len(declared) >= 16, (
        "the mutant manifest shrank below its 16-mutant seed set; mutants are "
        "removed only with a recorded reason, never to make a gate pass")


def test_apply_and_revert_restores_bytes_not_just_content(tmp_path, monkeypatch):
    """apply_and_test must restore the file BYTE-for-byte, CRLF included.

    F31 found this the hard way: `_read` opens in universal-newline mode, and
    the original `_write(fpath, original)` wrote the text back with newline=""
    -- so every CRLF source file the harness touched came out LF-only. Under
    core.autocrlf=true `git diff` normalises that away and reports nothing,
    which is exactly what makes it dangerous: real, unrequested worktree
    changes that a residue check cannot see. This test compares raw bytes.
    """
    target = tmp_path / "crlf_source.cpp"
    original_bytes = b"int f() {\r\n    return 0;\r\n}\r\n"
    target.write_bytes(original_bytes)

    monkeypatch.setattr(mutation_kill, "build", lambda cfg, tgt: (0, ""))
    seen = {}

    def fake_run_test(exe, filt):
        seen["bytes_while_mutated"] = target.read_bytes()
        return 1, "mutant killed"

    monkeypatch.setattr(mutation_kill, "run_test", fake_run_test)
    monkeypatch.setattr(os.path, "isfile", lambda p: True)

    m = {"id": "crlf", "file": target.name, "find": "    return 0;",
         "replace": "    return 1;", "target": "pdfe_tests",
         "killing_test": "T.X"}
    res = mutation_kill.apply_and_test(
        str(tmp_path), {"cmake": "cmake", "build_preset": "p",
                        "test_exe": "pdfe_tests.exe"}, m)

    assert res["killed"] is True
    assert seen["bytes_while_mutated"] == b"int f() {\r\n    return 1;\r\n}\r\n", (
        "the mutant must be applied with the file's own line endings")
    assert target.read_bytes() == original_bytes, (
        "revert restored the content but not the bytes (line endings rewritten)")


def _run_real(tmp_path):
    sweep_out = tmp_path / "sweep_real.json"
    rc = mutation_kill.main([
        "--manifest", os.path.join("tools", "gate", "mutants.json"),
        "--dry-run",
        "--sweep-json", str(sweep_out),
    ])
    return rc, json.loads(sweep_out.read_text(encoding="utf-8"))
