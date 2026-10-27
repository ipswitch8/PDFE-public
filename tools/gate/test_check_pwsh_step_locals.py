#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Companion pytest for tools/gate/check_pwsh_step_locals.py (GitHub #280).

WHY THIS EXISTS ALONGSIDE THE GUARD'S OWN --self-test
-----------------------------------------------------
`check_pwsh_step_locals.py` already carries a thorough `--self-test`, wired
into `fast-checks.yml`. It proves the guard detects the *defect shape* and
spares the *fixed shape*, both synthetically.

What it does not do is run the guard against the **real file that was broken**.
That distinction has cost this project real time: a guard that satisfies
planted samples and misses the actual artefact is decoration, and the reason
this defect survived four full sweeps is that nothing ever pointed a checker at
the file as it stood.

So the single test that matters here reconstructs
`.github/workflows/windows-validation.yml` at `1085c852~1` -- the last commit
carrying the unfixed reconciler step, which read `$outDir` and `$testExe` from
the *gtest* step's shell and died on `Join-Path`'s null bind on its first line
-- and requires the guard to flag it.

HISTORICAL NOTE, recorded because the duplication was the expensive part. This
defect was independently diagnosed twice: once on the line that produced
`check_pwsh_step_locals.py`, and once in a session working from a clone that
could not see it, which produced a second guard
(`check_pwsh_step_variables.py`) for the same class. The second was dropped in
the reconciliation merge; this file carries forward the one idea it had that
the surviving guard lacked. Two guards for one class is waste; a real-artefact
control is not.

WIRING (load-bearing). ``pytest.ini`` sets ``testpaths = tests``, so a test
under ``tools/gate/`` runs NOWHERE unless named explicitly. This file is named
in the *"Gate self-tests + matrix pass-semantics"* step of
``.github/workflows/fast-checks.yml``.
"""
from __future__ import annotations

import importlib.util
import pathlib
import re
import subprocess
import sys

import pytest

_HERE = pathlib.Path(__file__).resolve().parent
_REPO = _HERE.parents[1]
_MOD_PATH = _HERE / "check_pwsh_step_locals.py"

# The last commit carrying the unfixed reconciler step. Its successor,
# 1085c852, is "fix(ci): give the param-id reconciler the three variables it
# reads, and a guard for the class".
BROKEN_REF = "1085c852~1"
WORKFLOW = ".github/workflows/windows-validation.yml"


def _load():
    spec = importlib.util.spec_from_file_location("check_pwsh_step_locals",
                                                  _MOD_PATH)
    mod = importlib.util.module_from_spec(spec)
    assert spec and spec.loader
    spec.loader.exec_module(mod)
    return mod


def _blob(ref: str, path: str):
    r = subprocess.run(["git", "show", f"{ref}:{path}"],
                       cwd=str(_REPO), capture_output=True)
    return r.stdout if r.returncode == 0 and r.stdout else None


def test_the_guard_module_imports():
    """A guard that cannot be imported cannot be run by anything."""
    assert _MOD_PATH.is_file(), f"{_MOD_PATH} is missing"
    assert _load() is not None


def test_it_catches_the_real_defect_in_the_real_file(tmp_path):
    """THE control: the file as it stood while four sweeps went red.

    Runs 37238871672, 37260840565, 37366537472 and 38198875534 all failed this
    step, which had never executed its reconciler. If this ever stops failing,
    the guard has lost the only defect it was built for.
    """
    blob = _blob(BROKEN_REF, WORKFLOW)
    if blob is None:
        pytest.skip(f"{BROKEN_REF} not reachable in this clone")

    d = tmp_path / ".github" / "workflows"
    d.mkdir(parents=True)
    (d / "windows-validation.yml").write_bytes(blob)

    r = subprocess.run([sys.executable, str(_MOD_PATH),
                        "--workflow-dir", str(d)],
                       capture_output=True, text=True)
    out = r.stdout + r.stderr
    # GUARD THAT THE MEASUREMENT HAPPENED. A non-zero exit alone is not
    # evidence: argparse exits 2 on an unrecognised flag, which satisfies
    # `returncode != 0` while the guard never inspected a single step. This
    # test was written with the wrong flag first and would have passed
    # vacuously on exactly that. Key on a banner the guard itself emits, and on
    # its step census, before trusting the verdict.
    assert "check_pwsh_step_locals" in out, (
        "the guard did not run at all (usage error?):\n" + out[-2000:])
    assert "pwsh run: step(s)" in out, (
        "the guard printed no step census, so it inspected nothing:\n"
        + out[-2000:])
    assert r.returncode != 0, (
        "the guard PASSED the file that was red in four full sweeps:\n"
        + out[-2000:])
    # Name the variable, not just the file: attribution is the whole point.
    assert "outDir" in out, out[-2000:]


def test_the_fixed_tree_passes():
    """And the current tree must pass -- a guard that cannot pass is as useless
    as one that cannot fail. This is also the class-level assertion: it covers
    every pwsh step in the repo, not just the step that broke."""
    r = subprocess.run([sys.executable, str(_MOD_PATH)],
                       cwd=str(_REPO), capture_output=True, text=True)
    out = r.stdout + r.stderr
    assert r.returncode == 0, out[-2000:]
    # Non-vacuity: a pass that inspected nothing carries no information.
    assert "pwsh run: step(s)" in out, out[-2000:]


def test_the_guards_own_self_test_passes():
    r = subprocess.run([sys.executable, str(_MOD_PATH), "--self-test"],
                       capture_output=True, text=True)
    assert r.returncode == 0, (r.stdout + r.stderr)[-2000:]


def test_only_one_guard_owns_this_class():
    """Regression: the reconciliation merge dropped a duplicate guard.

    Two guards for one defect class is waste, and the dropped one used a
    hand-typed exclusion list -- the C-ENUM shape #275 and #254 were spent
    removing, and which the surviving guard deliberately derives from the shell
    instead. If a second pwsh-locals guard reappears, the duplication is back.
    """
    names = sorted(p.name for p in _HERE.glob("check_pwsh_step_*.py"))
    assert names == ["check_pwsh_step_locals.py"], names


# ----------------------------------------------------------- population floor
#
# GitHub #305-adjacent, raised as finding F2 by the p97 phase-2 claim-check
# gate. `check_pwsh_step_variables.py` was deleted in that phase as a duplicate
# of this guard, and the justification was a measurement: both inspected the
# same 14 workflow files and 62 pwsh steps, so no coverage was lost.
#
# That measurement was true the day it was taken and nothing pinned it. The
# only existing floor is `total_pwsh == 0`, which catches the guard going
# completely blind but not a PARTIAL regression -- a glob narrowed, a shell
# detector that stops recognising `pwsh`, a job shape it silently skips. Half
# the steps could vanish and every assertion above would still pass, because
# they all key on the banner being present rather than on what it says.
#
# So: a RATCHET on the census the guard prints about itself.

#: Measured 2026-10-14 on the full tree. FLOORS, not equalities -- workflows
#: and steps get ADDED, and a guard that reddens whenever the repo grows is a
#: guard that gets switched off. If these numbers legitimately drop (a workflow
#: is deleted), lower them DELIBERATELY in the same commit that deletes it, so
#: the drop is reviewed rather than absorbed.
_MIN_WORKFLOW_FILES = 14
_MIN_PWSH_STEPS = 62


def _census():
    """(workflow_files, pwsh_steps) parsed from the guard's own output.

    Parsed from the printed census rather than by importing and re-deriving,
    deliberately: the printed line is what a human reads and what the other
    tests in this file key on, so pinning THAT closes the gap they leave.
    """
    r = subprocess.run([sys.executable, str(_MOD_PATH)],
                       cwd=str(_REPO), capture_output=True, text=True)
    out = r.stdout + r.stderr
    m = re.search(r"workflows\s*:\s*(\d+) file\(s\), (\d+) pwsh run: step\(s\)",
                  out)
    assert m, ("the guard's census line could not be parsed, so this ratchet "
               "verifies nothing -- fix the parse, do not delete the test:\n"
               + out[-2000:])
    return int(m.group(1)), int(m.group(2))


def test_the_guard_still_covers_the_population_it_was_credited_with():
    """F2: the surviving guard must keep covering what the deleted one did.

    This is the ONGOING property. Without it, "deleting the duplicate lost no
    coverage" is a statement about one afternoon in October 2026.
    """
    files, steps = _census()
    assert files >= _MIN_WORKFLOW_FILES, (
        "workflow coverage REGRESSED: %d file(s), was >= %d when "
        "check_pwsh_step_variables.py was deleted on the strength of this "
        "guard covering the same population. Either the glob narrowed or "
        "workflows were removed; find out which before adjusting the floor."
        % (files, _MIN_WORKFLOW_FILES))
    assert steps >= _MIN_PWSH_STEPS, (
        "pwsh STEP coverage REGRESSED: %d step(s), was >= %d. A partial drop "
        "is the dangerous shape -- the existing `total_pwsh == 0` floor only "
        "catches the guard going fully blind, and every other test in this "
        "file keys on the banner being PRESENT, not on what it says."
        % (steps, _MIN_PWSH_STEPS))


def test_the_floor_is_not_trivially_satisfiable():
    """A floor of zero would pass against a guard that inspects nothing.

    Pins that the recorded floors are actually above the vacuous value, which
    is the failure mode of every "assert count >= N" written in a hurry.
    """
    assert _MIN_WORKFLOW_FILES > 0 and _MIN_PWSH_STEPS > 0
    # And that the floor is close to reality rather than nominal: a floor of 1
    # would satisfy the assertion above while permitting a 98% regression.
    files, steps = _census()
    assert _MIN_PWSH_STEPS >= steps // 2, (
        "the recorded floor (%d) is less than half the measured population "
        "(%d), so it would absorb a catastrophic regression silently"
        % (_MIN_PWSH_STEPS, steps))
