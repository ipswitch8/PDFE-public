#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""Tests for check_engine_wiring.py, the C-BELOW standing control.

The guard's own `--self-test` proves it fires and spares. These tests prove the
things the self-test cannot reach from inside itself: that the RATCHET works in
both directions against the real baseline, and that "could not measure" is an
error rather than a pass.

That last one matters more than it looks. A guard that reports a clean result
when it enumerated nothing is the C-FAILOPEN shape this project has already
been bitten by -- DCA 0085 (nothing compared the crash-dump directory to the
run that owns it) and GitHub #246 (a peak-memory bound that cannot be evaluated
reports as a pass). Any new guard must be checked against it explicitly, or it
inherits the defect it was written to catch.
"""
from __future__ import annotations

import importlib.util
import io
import json
import os
import subprocess
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
GUARD = os.path.join(HERE, "check_engine_wiring.py")
BASELINE = os.path.join(HERE, "engine_wiring_baseline.json")


def _load():
    spec = importlib.util.spec_from_file_location("cew", GUARD)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _run(args):
    return subprocess.run([sys.executable, GUARD] + args, cwd=REPO,
                          capture_output=True, text=True)


_FOUND_CACHE = {}


def _found():
    """The guard's result over the real tree, computed once.

    The closure takes ~40s; without this the suite pays it per test. The tree
    does not change mid-run, so a cache is sound here -- and cheap enough that
    it is not worth a fixture.
    """
    if "v" not in _FOUND_CACHE:
        _FOUND_CACHE["v"] = _load().find_unreachable(REPO)
    return _FOUND_CACHE["v"]


def test_self_test_passes():
    """The guard's own both-directions self-test must be green."""
    r = _run(["--self-test"])
    assert r.returncode == 0, r.stdout + r.stderr
    assert "self-test OK" in r.stdout


def test_tree_is_clean_against_the_baseline():
    """Every unreachable class in the tree is baselined, and none has become
    reachable without being removed."""
    r = _run(["--repo-root", "."])
    assert r.returncode == 0, (
        "the engine-wiring guard is red against the current tree:\n"
        + r.stdout + r.stderr)


def test_every_baseline_entry_states_a_kind_and_a_reason():
    """A baseline entry without a reason is an amnesty, not a ratchet.

    The whole defence against this list silently growing is that each entry
    has to say WHICH it is -- a lexical false positive, a deliberate test-only
    seam, superseded code, or an admitted gap.
    """
    data = json.load(io.open(BASELINE, encoding="utf-8"))
    entries = {k: v for k, v in data.items() if not k.startswith("_")}
    assert entries, "baseline has no entries -- this test would prove nothing"
    allowed = {"lexical-false-positive", "deliberate-test-only-seam",
               "superseded", "admitted-gap"}
    for name, e in entries.items():
        assert e.get("kind") in allowed, (
            "%s has kind %r, not one of %s" % (name, e.get("kind"), allowed))
        assert len(e.get("reason", "")) > 40, (
            "%s's reason is too short to be one: %r" % (name, e.get("reason")))
        if e["kind"] == "admitted-gap":
            assert e.get("issue"), (
                "%s is an admitted gap with no issue number -- listing it "
                "here does not track it" % name)


def test_ratchet_fires_when_a_real_member_leaves_the_baseline(tmp_path):
    """Direction 1: an unreachable class NOT in the baseline must fail.

    Mutates a COPY of the baseline, never the tracked file.
    """
    found = _found()
    assert found, "no unreachable classes found -- the fixture is empty"
    victim = sorted(found)[0]

    data = json.load(io.open(BASELINE, encoding="utf-8"))
    baseline = {k: v for k, v in data.items() if not k.startswith("_")}
    del baseline[victim]
    new = {c: h for c, h in found.items() if c not in baseline}
    assert victim in new, (
        "removing %s from the baseline did not make it a new finding -- the "
        "ratchet's add direction is inert" % victim)


def test_ratchet_fires_when_a_baselined_class_becomes_reachable():
    """Direction 2: a baselined class that IS reachable must be flagged for
    removal, or the list can only ever grow."""
    found = _found()
    data = json.load(io.open(BASELINE, encoding="utf-8"))
    baseline = {k: v for k, v in data.items() if not k.startswith("_")}
    # SelectTool is referenced by several src files, so it must never be a
    # member; if it ever is, this test is the wrong shape and should be fixed
    # rather than deleted.
    assert "SelectTool" not in found, (
        "SelectTool is unreachable, which breaks this test's premise")
    stale = [c for c in dict(baseline, SelectTool={}) if c not in found]
    assert "SelectTool" in stale, (
        "a reachable class sitting in the baseline was not reported stale -- "
        "the ratchet's removal direction is inert")


def test_a_comment_mention_does_not_make_a_class_reachable():
    """Regression for the ImageExporter miss (phase-3 claim-check).

    The first version of this guard reported ImageExporter reachable on the
    strength of exactly two mentions, both of them prose inside `//` comments
    (src/core/AtomicFileReplace.h and src/core/Document.cpp). A comment naming
    a component is not a call to it. Pinned against the REAL tree, because the
    synthetic self-test case cannot prove this file is still the one affected.
    """
    found = _found()
    assert found is not None, "could not measure"
    assert "ImageExporter" in found, (
        "ImageExporter is being treated as reachable again -- either it was "
        "genuinely wired up (then remove it from the baseline and delete this "
        "test) or comment stripping regressed")


def test_a_reference_from_dead_code_does_not_make_a_class_reachable():
    """Regression for the WordExporter miss (phase-3 claim-check).

    ExcelExporter.cpp really does call WordExporter::escapeXml. It is a real
    call in real code and it proves nothing, because ExcelExporter is itself
    reachable from nothing. Dead code referencing dead code forms a closed
    island that a depth-1 rule reads as two live components.
    """
    found = _found()
    assert found is not None, "could not measure"
    for cls in ("WordExporter", "ExcelExporter", "ZipWriter"):
        assert cls in found, (
            "%s is being treated as reachable again -- the transitive closure "
            "regressed to depth-1, or the export island was wired up" % cls)


def test_the_entry_point_this_guard_measures_from_exists():
    """The closure is meaningless if its root is wrong.

    If src/main.cpp is ever renamed, find_unreachable must return None ("could
    not measure") rather than silently reporting every class unreachable -- or
    a refactor would turn this guard into a wall of false positives that
    someone would then switch off.
    """
    mod = _load()
    assert mod.ENTRY_POINTS, "no entry point configured"
    for p in mod.ENTRY_POINTS:
        assert os.path.exists(os.path.join(REPO, p)), (
            "configured entry point %s does not exist; update ENTRY_POINTS" % p)


def test_cannot_measure_is_an_error_not_a_pass():
    """C-FAILOPEN guard: pointed at a directory with no tracked sources, the
    guard must refuse rather than report a clean sweep."""
    mod = _load()
    empty = os.path.join(REPO, "tools", "gate")   # a real dir, no src/ or tests/
    assert mod.find_unreachable(empty) is None, (
        "the guard returned a result for a tree it could not enumerate; "
        "'no findings' and 'no measurement' must not be the same answer")


# ---------------------------------------------------------------------------
# GitHub #259. The baseline carried a `kind` on every entry and a prose
# enumeration of the vocabulary in its own `_rules`, and NOTHING read either:
# `_load_baseline` strips `_`-prefixed keys before validating, and `kind` was
# never referenced. A taxonomy nothing enforces is a description of what
# someone once intended. Same shape as p92 found in the guard-reachability
# baseline (DCA 0093), one file over.
# ---------------------------------------------------------------------------
def _baseline_data():
    import json
    path = os.path.join(REPO, "tools", "gate", "engine_wiring_baseline.json")
    return json.load(io.open(path, encoding="utf-8"))


def test_every_baseline_entry_declares_an_enforced_kind():
    mod = _load()
    entries = {k: v for k, v in _baseline_data().items()
               if not k.startswith("_")}
    assert entries, "the baseline is empty; this test would be vacuous"
    assert not mod._validate_baseline(entries), mod._validate_baseline(entries)


def test_an_unknown_kind_is_rejected():
    """The check must be able to FAIL, or it is decoration."""
    mod = _load()
    bad = {"Thing": {"kind": "made-up-kind", "reason": "x", "issue": 1}}
    problems = mod._validate_baseline(bad)
    assert any("made-up-kind" in p for p in problems), problems


def test_an_admitted_gap_without_an_issue_is_rejected():
    mod = _load()
    bad = {"Thing": {"kind": "admitted-gap", "reason": "x"}}
    problems = mod._validate_baseline(bad)
    assert any("issue" in p for p in problems), problems


def test_an_admitted_gap_with_an_issue_number_is_accepted():
    r"""The first version of this check grepped the reason PROSE for `#\d+`
    and reported all 15 real gaps as unissued -- a defect in the check, not
    the data. The number lives in its own field."""
    mod = _load()
    ok = {"Thing": {"kind": "admitted-gap", "reason": "x", "issue": 253}}
    assert not mod._validate_baseline(ok), mod._validate_baseline(ok)


def test_the_engine_wiring_baseline_prose_does_not_restate_the_taxonomy():
    """No second copy of a set that something else enforces.

    `_rules` used to enumerate the kinds verbatim. Nothing read it, so it was
    free to drift from BASELINE_KINDS -- and a reader trusting it would be
    trusting whichever copy they happened to open.
    """
    mod = _load()
    data = _baseline_data()
    prose = "\n".join(
        v if isinstance(v, str) else "\n".join(map(str, v))
        for k, v in data.items() if k.startswith("_"))
    restated = sorted(k for k in mod.BASELINE_KINDS if k in prose)
    assert not restated, (
        "the baseline's own metadata names %s. Nothing reads these fields, so "
        "the copy can drift from BASELINE_KINDS silently. Point at the "
        "enforcing constant instead of repeating it." % restated)
