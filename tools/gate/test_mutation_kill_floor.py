#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Mutation-floor gating tests (pipeline e5b9c231 Phase 5 — criterion 3).

Proves the kill-rate FLOOR actually gates: ``mutation_kill.main`` returns a
non-zero exit when the kill-rate is below ``--floor`` (a surviving mutant = a
false-green oracle), and zero when the rate meets the floor. The CI job
(.github/workflows/mutation-kill.yml) turns that non-zero exit into a failed
build, so this is the unit-level proof that the floor is a gate, not a report.

The real harness rebuilds pdfe_tests per mutant (minutes each), so these tests
stub the per-mutant build+test step and the find-string validation, exercising
only the floor DECISION logic that main() owns.

Run:
    python -m pytest tools/gate/test_mutation_kill_floor.py -q
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import mutation_kill  # noqa: E402


def _manifest(tmp_path: Path) -> Path:
    """A minimal valid manifest with a build section and two mutants."""
    m = {
        "build": {
            "cmake": "cmake",
            "build_preset": "debug-vs2022",
            "test_exe": "build/vs2022/bin/Debug/pdfe_tests.exe",
        },
        "mutants": [
            {"id": "m_killed", "file": "x.cpp", "find": "a", "replace": "b",
             "target": "pdfe_tests", "killing_test": "T.Killed"},
            {"id": "m_survivor", "file": "y.cpp", "find": "c", "replace": "d",
             "target": "pdfe_tests", "killing_test": "T.Survivor"},
        ],
    }
    p = tmp_path / "mutants.json"
    p.write_text(json.dumps(m), encoding="utf-8")
    return p


def _stub_engine(monkeypatch):
    """Bypass real file/build/test I/O; force one KILLED + one SURVIVOR."""
    monkeypatch.setattr(mutation_kill, "validate_mutant",
                        lambda repo, m: (True, "ok"))

    def fake_apply(repo, build_cfg, m):
        killed = m["id"] == "m_killed"
        return {"id": m["id"], "killed": killed,
                "reason": "stub-killed" if killed else "stub-SURVIVOR"}

    monkeypatch.setattr(mutation_kill, "apply_and_test", fake_apply)


def test_floor_gate_fails_when_survivor_below_floor(tmp_path, monkeypatch):
    """kill-rate 0.5 < floor 1.0 -> exit 1 (the survivor gates the build)."""
    _stub_engine(monkeypatch)
    manifest = _manifest(tmp_path)
    rc = mutation_kill.main(["--manifest", str(manifest), "--floor", "1.0"])
    assert rc == 1, "a surviving mutant below the floor must fail the gate (exit 1)"


def test_floor_gate_passes_when_rate_meets_floor(tmp_path, monkeypatch):
    """kill-rate 0.5 >= floor 0.5 -> exit 0."""
    _stub_engine(monkeypatch)
    manifest = _manifest(tmp_path)
    rc = mutation_kill.main(["--manifest", str(manifest), "--floor", "0.5"])
    assert rc == 0, "a kill-rate at or above the floor must pass (exit 0)"


def test_json_report_records_rate_and_floor(tmp_path, monkeypatch):
    """The report artifact carries the numbers CI publishes."""
    _stub_engine(monkeypatch)
    manifest = _manifest(tmp_path)
    out = tmp_path / "report.json"
    mutation_kill.main(["--manifest", str(manifest), "--floor", "1.0",
                        "--json-out", str(out)])
    report = json.loads(out.read_text(encoding="utf-8"))
    assert report["killed"] == 1
    assert report["total"] == 2
    assert abs(report["kill_rate"] - 0.5) < 1e-9
    assert report["floor"] == 1.0


def test_dry_run_validates_manifest_only(tmp_path, monkeypatch):
    """--dry-run returns 0 after validation without building/testing."""
    monkeypatch.setattr(mutation_kill, "validate_mutant",
                        lambda repo, m: (True, "ok"))
    # apply_and_test must NOT be called in dry-run; make it explode if it is.
    def boom(*a, **k):
        raise AssertionError("apply_and_test called during --dry-run")
    monkeypatch.setattr(mutation_kill, "apply_and_test", boom)
    manifest = _manifest(tmp_path)
    rc = mutation_kill.main(["--manifest", str(manifest), "--dry-run"])
    assert rc == 0


# ---------------------------------------------------------------------------
# Toolchain-absence handling (karen gate, phase 9 of backlog-workable-p82).
#
# `apply_and_test` reads any non-zero build exit as "mutant did not compile
# (trivially killed)". That is right for a mutant that breaks the compile, and
# CATASTROPHIC for a cmake that cannot be launched at all: every mutant would be
# reported killed, the kill-rate would be a vacuous 100%, and the floor gate
# would pass having built and tested nothing -- the exact false-green-oracle
# class this harness exists to detect, reproduced inside the detector.
#
# This was very nearly introduced while FIXING the unrelated "cmake not on PATH"
# failure: the first attempt caught OSError and returned exit 127, which reads as
# a broken build. These cases pin the corrected behaviour.
# ---------------------------------------------------------------------------


def test_unlaunchable_cmake_raises_instead_of_reporting_killed_mutants(monkeypatch):
    """A missing toolchain must NOT masquerade as a 100% kill rate."""
    def _boom(*_a, **_k):
        raise FileNotFoundError(2, "The system cannot find the file specified")

    monkeypatch.setattr(mutation_kill.subprocess, "run", _boom)
    monkeypatch.setattr(mutation_kill, "resolve_cmake", lambda _c: "definitely-not-here")

    with pytest.raises(mutation_kill.ToolchainError) as ei:
        mutation_kill.build({"cmake": "definitely-not-here",
                             "build_preset": "p"}, "pdfe_tests")
    msg = str(ei.value)
    assert "definitely-not-here" in msg, "the error must NAME the executable"
    assert "vacuously" in msg or "SETUP" in msg or "setup" in msg


def test_resolve_cmake_prefers_an_explicit_override(monkeypatch, tmp_path):
    fake = tmp_path / "cmake.exe"
    fake.write_text("", encoding="utf-8")
    monkeypatch.setenv("PDFE_CMAKE", str(fake))
    assert mutation_kill.resolve_cmake("cmake") == str(fake)


def test_resolve_cmake_falls_back_to_the_bare_name_when_nothing_resolves(monkeypatch):
    """The last branch must still surface an error rather than inventing a path.

    If every candidate is absent, resolve_cmake returns the bare name so the
    launch fails loudly (and now raises ToolchainError). Returning something
    plausible-but-wrong would hide a genuinely missing toolchain.
    """
    monkeypatch.delenv("PDFE_CMAKE", raising=False)
    monkeypatch.setattr(mutation_kill.shutil, "which", lambda _n: None)
    monkeypatch.setattr(mutation_kill.os.path, "isfile", lambda _p: False)
    assert mutation_kill.resolve_cmake("cmake") == "cmake"
