# -*- coding: utf-8 -*-
"""GitHub #289 — a later run must not overwrite the evidence of an earlier one.

WHAT THE DEFECT WAS
===================
Every visual-regression failure wrote to
``tests/visual/_failure_artifacts/<baseline_name>_diff.png``. The filename came
only from the baseline name, so running the same test twice overwrote the first
run's artifact in place, and the directory is gitignored so nothing was
versioned either.

It bit for real: a diff was measured during p96 phase-2, its numbers quoted on
an issue, and a later unrelated run rewrote the same filenames. The tell was two
scripts reporting **2773** and **3456** differing pixels for nominally the same
file — same path, different file, and nothing saying anything had changed.

The codebase already knew. ``tests/conftest.py``'s janitor carried the comment
"Visual _failure_artifacts have non-per-run names so we apply a second rule
there" and capped them by mtime instead, which bounds growth but does nothing
about one run destroying another's evidence.

WHAT THESE TESTS PIN
====================
C3 asks for the capture to be "demonstrated by running the capture twice and
confirming both artifact sets are retrievable". That is done without launching
the app: the property is about PATH SCOPING, and a UI launch would make a
path-naming test depend on a renderer, a window manager and an interactive
desktop. The run id is the mechanism, so the run id is what gets exercised —
including across a real subprocess, since ``PDFE_TEST_RUN_ID`` exists precisely
so the app's own children agree on it.

Run:  python -m pytest tests/visual/test_failure_artifacts_are_run_scoped.py -q
"""

from __future__ import annotations

import importlib
import io
import os
import subprocess
import sys
from pathlib import Path

import pytest

#: parents[2]: this file is <root>/tests/visual/<name>.py. Asserted, because a
#: misresolved root would make every assertion below pass against a tree that
#: does not exist -- the shape this pipeline has hit four times.
REPO_ROOT = Path(__file__).resolve().parents[2]
assert (REPO_ROOT / ".github" / "workflows").is_dir(), (
    f"repo root misresolved to {REPO_ROOT!r}")

ARTIFACT_ROOT = REPO_ROOT / "tests" / "visual" / "_failure_artifacts"


def _dir_for_run(run_id: str) -> Path:
    """Ask output_paths, in a fresh interpreter, what dir that run would use.

    A subprocess rather than a monkeypatched re-import: RUN_ID is resolved at
    import time from the environment, which is exactly the cross-process
    contract being tested. Re-importing in-process would exercise a different
    mechanism than the one the app uses.
    """
    code = (
        "import os, sys; sys.path.insert(0, r'%s');"
        "from tests.helpers.output_paths import visual_failure_dir, RUN_ID;"
        "print(RUN_ID); print(visual_failure_dir(create=True))"
        % str(REPO_ROOT)
    )
    env = dict(os.environ)
    env["PDFE_TEST_RUN_ID"] = run_id
    p = subprocess.run([sys.executable, "-c", code], cwd=str(REPO_ROOT),
                       capture_output=True, text=True, env=env)
    assert p.returncode == 0, f"probe failed: {p.stdout}\n{p.stderr}"
    lines = [l.strip() for l in p.stdout.splitlines() if l.strip()]
    assert len(lines) >= 2, f"probe output unexpected: {p.stdout!r}"
    return lines[0], Path(lines[1])


def test_the_run_id_is_honoured_from_the_environment():
    """The scoping key must come from PDFE_TEST_RUN_ID, not be re-minted.

    If it were re-minted per process, the app's child processes would write
    into a different directory than the test that launched them, and the
    artifacts of one logical run would scatter.
    """
    seen, _d = _dir_for_run("aaaaaaaa")
    assert seen == "aaaaaaaa", (
        f"output_paths minted its own run id ({seen}) instead of honouring "
        "PDFE_TEST_RUN_ID, so a subprocess would not agree with its parent")


def test_two_runs_get_two_directories_and_both_survive():
    """THE criterion. Two runs, two artifact sets, both retrievable."""
    id_a, dir_a = _dir_for_run("11111111")
    id_b, dir_b = _dir_for_run("22222222")

    assert dir_a != dir_b, (
        "two different runs resolved to the SAME artifact directory, so the "
        "second would overwrite the first -- this is GitHub #289 unfixed")

    # Write a distinguishable artifact into each, as the comparator would.
    a = dir_a / "phase5_example_canvas_diff.png"
    b = dir_b / "phase5_example_canvas_diff.png"
    io.open(a, "w", encoding="utf-8").write("run-a evidence")
    io.open(b, "w", encoding="utf-8").write("run-b evidence")

    # Same FILENAME in both -- that is the point. Under the old scheme these
    # were one path and the second write destroyed the first.
    assert a.name == b.name
    assert io.open(a, encoding="utf-8").read() == "run-a evidence", (
        "run A's artifact was modified by run B writing the same filename")
    assert io.open(b, encoding="utf-8").read() == "run-b evidence"

    # And both are reachable by run id without knowing anything else.
    assert (ARTIFACT_ROOT / id_a / a.name).exists()
    assert (ARTIFACT_ROOT / id_b / b.name).exists()


def test_the_old_unversioned_path_would_have_collided():
    """Encode the pre-fix behaviour, so the comparison is a measurement.

    Without this, the test above is satisfied by any two distinct paths and
    says nothing about what changed. The old scheme is one line, so express it.
    """
    old_a = ARTIFACT_ROOT / "phase5_example_canvas_diff.png"
    old_b = ARTIFACT_ROOT / "phase5_example_canvas_diff.png"
    assert old_a == old_b, (
        "the pre-fix path is not being reproduced, so the contrast below "
        "proves nothing")

    _id_a, dir_a = _dir_for_run("33333333")
    _id_b, dir_b = _dir_for_run("44444444")
    new_a = dir_a / "phase5_example_canvas_diff.png"
    new_b = dir_b / "phase5_example_canvas_diff.png"
    assert new_a != new_b, (
        "the run-scoped paths collide exactly as the old ones did")


def test_every_FAILURE_DIR_definition_goes_through_the_helper():
    """No module may rebuild the unversioned path by hand.

    The definition was COPIED into seven files, each as
    ``REPO_ROOT / "tests" / "visual" / "_failure_artifacts"``. Fixing six of
    seven would leave one module still overwriting evidence while the others
    did not -- the harder bug to find, because the artifacts would be
    inconsistent rather than simply wrong.
    """
    offenders = []
    for path in sorted((REPO_ROOT / "tests").rglob("*.py")):
        if path.name == Path(__file__).name:
            continue
        if "_tmp" in path.parts or "__pycache__" in path.parts:
            continue
        try:
            src = io.open(path, encoding="utf-8", errors="replace").read()
        except OSError:
            continue
        # WIN-45 / GitHub #229. This loop used to be:
        #
        #     for line in src.splitlines():
        #         s = line.strip()
        #         if s.startswith("#"):
        #             continue
        #
        # and `check_reachability_skip_shape.py` flagged that `continue` as a
        # new WIN-45 site. It is a FALSE POSITIVE -- the guard skips COMMENT
        # LINES while scanning source text; there is no product being driven
        # and nothing to be unreachable. But the checker matches a structural
        # shape and cannot tell the difference, and its own advice is to
        # express a non-reachability guard in a form it can see.
        #
        # Filtering rather than `continue`-ing does that, and is clearer here
        # anyway. It is deliberately NOT baselined: adding a false positive to
        # reachability_skip_baseline.json would grow a burn-down list with an
        # entry that has nothing to burn down, which is exactly what #229
        # distinguishes itself from magnitude_guard_baseline.json for.
        code_lines = [ln.strip() for ln in src.splitlines()
                      if not ln.strip().startswith("#")]
        for s in code_lines:
            # A FAILURE_DIR assignment that builds the path itself rather than
            # calling the helper.
            if "FAILURE_DIR" in s and "=" in s and "_failure_artifacts" in s:
                offenders.append(f"{path.relative_to(REPO_ROOT)}: {s[:90]}")
    assert not offenders, (
        "these modules build the unversioned artifact path by hand instead of "
        "calling output_paths.visual_failure_dir(), so their artifacts are "
        "still overwritten by the next run (GitHub #289):\n  "
        + "\n  ".join(offenders))


def test_the_janitor_caps_run_directories_not_only_files():
    """The cap has to follow the artifacts into the subdirectories.

    Run-scoped artifacts fix the overwrite and would ALSO have silently
    disabled the existing cap: it iterates the artifact root taking only
    ``is_file()``, which matches nothing inside a run directory. Without a
    directory cap the artifacts grow without bound -- a fix that trades a
    destroyed-evidence bug for a disk-exhaustion one.
    """
    conftest = io.open(REPO_ROOT / "tests" / "conftest.py",
                       encoding="utf-8").read()
    assert "_VISUAL_ARTIFACT_RUNS_KEEP" in conftest, (
        "conftest has no run-directory cap, so run-scoped artifacts accumulate "
        "without bound")
    assert "is_dir()" in conftest, (
        "the janitor never inspects directories, so the run-directory cap "
        "cannot be doing anything")
