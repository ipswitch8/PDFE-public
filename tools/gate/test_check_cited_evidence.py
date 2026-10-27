# -*- coding: utf-8 -*-
"""Discrimination controls for the cited-evidence guard.

WHY THIS FILE EXISTS. `check_cited_evidence.py` was wired into `fast-checks`
with NO tests and NO self-test mode -- a script that gates CI and that nothing
had ever proved could FAIL. An ad-hoc `karen` review caught it. That is the
exact shape this repository keeps getting bitten by: DCA 0041 (a guard broken by
a comment growing), DCA 0045 (a guard that could ONLY fail), and the fuzz entry
point whose only presence was a mention in a comment.

So these cases pin BOTH directions:
  RED   -- a cited-but-untracked file must fail under --fail-on-untracked-cited
  GREEN -- a cited-and-tracked file, and an uncited-but-tracked file, must not

Each test builds a REAL temporary git repository and runs the module's `main()`
inside it, rather than calling `citation_for()` in isolation. The defect being
guarded against is about the interaction of git state and citation text, and a
unit test of the matcher alone would pass while the wiring was broken -- which
is precisely what DCA 0045 records happening to the F69(b) control.
"""
from __future__ import annotations

import contextlib
import importlib.util
import io
import os
import subprocess
import sys
from pathlib import Path

import pytest


def run_main(args):
    """Call the module's main() and return (rc, stdout).

    NOT `capsys`: this project's pytest.ini sets `-p no:capture`, so the capture
    plugin is disabled and the capsys fixture does not exist. Capturing here
    keeps these tests working under the repo's own configuration rather than
    requiring an override of it.
    """
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = ce.main(args)
    return rc, buf.getvalue()

HERE = Path(__file__).resolve().parent
_SPEC = importlib.util.spec_from_file_location(
    "cited_evidence_under_test", HERE / "check_cited_evidence.py")
assert _SPEC and _SPEC.loader
ce = importlib.util.module_from_spec(_SPEC)
sys.modules["cited_evidence_under_test"] = ce
_SPEC.loader.exec_module(ce)


def _run_git(repo, *args):
    subprocess.run(["git"] + list(args), cwd=repo, check=True,
                   capture_output=True, text=True)


@pytest.fixture
def repo(tmp_path, monkeypatch):
    """A real git repo with a docs/ tree, cwd'd into for the duration."""
    _run_git(tmp_path, "init", "-q")
    _run_git(tmp_path, "config", "user.email", "t@example.invalid")
    _run_git(tmp_path, "config", "user.name", "t")
    (tmp_path / "docs").mkdir()
    monkeypatch.chdir(tmp_path)
    return tmp_path


def _write(repo, rel, text):
    path = repo / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    return path


# ── RED: the state the guard exists to catch ────────────────────────────

def test_a_cited_but_untracked_file_fails(repo):
    """THE F36 DEFECT. A document names an artifact; the artifact is not in the
    tree. `git add` on an ignored file exits 0 and stages nothing, so this never
    announces itself -- the guard is the only thing that would."""
    _write(repo, "docs/notes.md", "see docs/audit/run/results.xml for evidence")
    _write(repo, "docs/audit/run/results.xml", "<testsuites/>")
    _run_git(repo, "add", "docs/notes.md")          # the XML stays UNTRACKED
    _run_git(repo, "commit", "-qm", "notes")

    rc, out = run_main(["--fail-on-untracked-cited"])
    assert rc == 1, out
    assert "FAIL" in out
    assert "results.xml" in out


def test_it_only_fails_when_asked_to(repo):
    """Without the flag the same state REPORTS but exits 0, so the script stays
    usable as a survey. A guard that cannot be run non-fatally gets run never."""
    _write(repo, "docs/notes.md", "see docs/audit/run/results.xml")
    _write(repo, "docs/audit/run/results.xml", "<testsuites/>")
    _run_git(repo, "add", "docs/notes.md")
    _run_git(repo, "commit", "-qm", "notes")

    rc, out = run_main([])
    assert rc == 0
    assert "FAIL" in out


def test_a_relative_manifest_citation_also_counts(repo):
    """`phase-34/runs.json` naming `run-01.xml` is a real reference. Requiring
    two path components missed this shape entirely on the first attempt."""
    _write(repo, "docs/audit/run/manifest.json", '{"runs": ["results.xml"]}')
    _write(repo, "docs/audit/run/results.xml", "<testsuites/>")
    _run_git(repo, "add", "docs/audit/run/manifest.json")
    _run_git(repo, "commit", "-qm", "manifest")

    rc, out = run_main(["--fail-on-untracked-cited"])
    assert rc == 1, "a sibling-manifest citation must count as a citation"
    assert "results.xml" in out


# ── GREEN: states the guard must NOT redden on ──────────────────────────

def test_a_cited_and_tracked_file_passes(repo):
    _write(repo, "docs/notes.md", "see docs/audit/run/results.xml")
    _write(repo, "docs/audit/run/results.xml", "<testsuites/>")
    _run_git(repo, "add", "docs/notes.md", "docs/audit/run/results.xml")
    _run_git(repo, "commit", "-qm", "both")

    rc, out = run_main(["--fail-on-untracked-cited"])
    assert rc == 0
    assert "OK: every cited file is tracked" in out


def test_an_uncited_tracked_file_does_not_fail(repo):
    """Untidy is not broken. A check that reds on both teaches people to ignore
    it -- and 50 committed F63 captures sit in exactly this state."""
    _write(repo, "docs/audit/run/orphan.xml", "<testsuites/>")
    _run_git(repo, "add", "docs/audit/run/orphan.xml")
    _run_git(repo, "commit", "-qm", "orphan")

    assert run_main(["--fail-on-untracked-cited"])[0] == 0


def test_an_uncited_untracked_file_does_not_fail(repo):
    """Nothing points at it and it is not committed. That is a housekeeping
    question, not a broken citation."""
    _write(repo, "docs/audit/run/leftover.xml", "<testsuites/>")
    _write(repo, "docs/keep.md", "unrelated")
    _run_git(repo, "add", "docs/keep.md")
    _run_git(repo, "commit", "-qm", "keep")

    assert run_main(["--fail-on-untracked-cited"])[0] == 0


def test_a_bare_basename_from_an_unrelated_tree_is_not_a_citation(repo):
    """`gtest-results.xml` appears in 53 tracked files in the real repo, nearly
    all CI workflows naming their OWN output. Treating that as a citation would
    make the guard demand that every CI artifact be committed."""
    _write(repo, "ci/workflow.yml", "run: pytest --junitxml=results.xml")
    _write(repo, "docs/audit/run/results.xml", "<testsuites/>")
    _run_git(repo, "add", "ci/workflow.yml")
    _run_git(repo, "commit", "-qm", "ci")

    rc, _ = run_main(["--fail-on-untracked-cited"])
    assert rc == 0, "a same-basename mention from an unrelated tree is not a citation"


def test_a_file_citing_itself_proves_nothing(repo):
    """An XML whose own text contains its filename must not count as cited by
    itself, or every artifact self-certifies."""
    _write(repo, "docs/audit/run/results.xml",
           '<testsuites name="results.xml"/>')
    _write(repo, "docs/keep.md", "unrelated")
    _run_git(repo, "add", "docs/keep.md")
    _run_git(repo, "commit", "-qm", "keep")

    assert run_main(["--fail-on-untracked-cited"])[0] == 0


# ── The three-bucket report ─────────────────────────────────────────────

def test_the_report_separates_tracked_from_untracked_uncited(repo):
    """Reporting one UNCITED number reads as a clean split and is not one --
    it produced a wrong count in the commit message that introduced this
    script. The two states must be distinguishable in the output."""
    _write(repo, "docs/audit/run/tracked_orphan.xml", "<testsuites/>")
    _write(repo, "docs/audit/run/untracked_orphan.xml", "<testsuites/>")
    _run_git(repo, "add", "docs/audit/run/tracked_orphan.xml")
    _run_git(repo, "commit", "-qm", "one")

    _, out = run_main([])
    assert "UNCITED, tracked : 1" in out, out
    assert "UNCITED, untracked: 1" in out, out


def test_the_real_repo_passes(repo_not_used=None):
    """Runs against THIS repository, not a fixture: the assertion that would
    have failed while cited evidence sat untracked."""
    cwd = os.getcwd()
    os.chdir(HERE.parent.parent)
    try:
        assert run_main(["--fail-on-untracked-cited"])[0] == 0
    finally:
        os.chdir(cwd)
