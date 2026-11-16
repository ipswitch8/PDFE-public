# -*- coding: utf-8 -*-
"""Self-tests for tools/gate/check_defect_ref_autolink.py (GitHub #389).

Each behaviour is pinned in both directions: the checker must FIRE on the
autolinkable form and stay QUIET on the forms the convention allows.
"""
from __future__ import annotations

import contextlib
import io
import json
import subprocess
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_defect_ref_autolink as chk  # noqa: E402

REPO = Path(__file__).resolve().parents[2]


def run_main(args):
    """Call main() and return (rc, stdout). NOT capsys: pytest.ini sets
    `-p no:capture`, so the capsys fixture does not exist in this repo."""
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = chk.main(args)
    return rc, buf.getvalue()


# --- the pattern itself -----------------------------------------------------

@pytest.mark.parametrize("text", [
    "Linux port phase 2 (Defect #42)",
    "Defect  #7 at the start of a line",
    "tab\tDefect\t#123 inside",
    "ends a sentence: Defect #9.",
])
def test_fires_on_autolinkable_internal_reference(text):
    assert chk.find_refs(text, "t"), text


@pytest.mark.parametrize("text", [
    "Linux port (Defect-42)",              # the new convention
    "see `Defect #42` for the old form",   # inline code is not autolinked
    "a ``Defect #42`` double-tick span",
    "GitHub #42 is SEC-104",               # a real GitHub reference
    "the defect #200 describes",           # lower case: a GitHub reference
    "NoDefect #3 is an identifier",        # no word boundary
    "Defect #42a is not a reference",      # digit run must end the token
    "Defect 42 without a hash",
])
def test_quiet_on_allowed_forms(text):
    assert chk.find_refs(text, "t") == [], text


def test_fenced_code_block_is_skipped_and_fence_closes():
    text = "```\nDefect #1 inside a fence\n```\nDefect #2 after the fence\n"
    found = chk.find_refs(text, "doc.md")
    assert [f.line for f in found] == [4]


def test_tilde_fence_is_skipped():
    assert chk.find_refs("~~~\nDefect #1\n~~~\n", "d") == []


def test_finding_reports_location_and_line_number():
    found = chk.find_refs("ok\nfix (Defect #42)\n", "commit abc")
    assert len(found) == 1
    assert str(found[0]) == "commit abc:2: fix (Defect #42)"


# --- --paths ------------------------------------------------------------------

def test_paths_mode_fails_on_violating_file_and_passes_clean_one(tmp_path):
    bad = tmp_path / "bad.md"
    bad.write_text("The port is Defect #42.\n", encoding="utf-8")
    good = tmp_path / "good.md"
    good.write_text("The port is Defect-42; see `Defect #42` for history.\n",
                    encoding="utf-8")
    rc, out = run_main(["--paths", str(bad)])
    assert rc == 1
    assert "bad.md:1" in out
    assert chk.main(["--paths", str(good)]) == 0


def test_paths_mode_reads_utf8(tmp_path):
    f = tmp_path / "u.md"
    f.write_text("Ünïcödé — Defect #5 — ✓\n", encoding="utf-8")
    assert chk.main(["--paths", str(f)]) == 1


def test_missing_path_is_an_input_error_not_a_pass(tmp_path):
    assert chk.main(["--paths", str(tmp_path / "absent.md")]) == 2


def test_no_mode_is_a_usage_error():
    with pytest.raises(SystemExit) as exc:
        chk.main([])
    assert exc.value.code == 2


# --- --event-file -------------------------------------------------------------

def _event(tmp_path, payload):
    p = tmp_path / "event.json"
    p.write_text(json.dumps(payload), encoding="utf-8")
    return str(p)


def test_event_pr_title_and_body_are_checked(tmp_path):
    ev = _event(tmp_path, {"pull_request": {"title": "Port (Defect #42)",
                                            "body": "Also Defect #43."}})
    assert {f.where for f in chk.check_event(ev)} == {"PR title", "PR body"}
    assert chk.main(["--event-file", ev]) == 1


def test_event_clean_pr_passes(tmp_path):
    ev = _event(tmp_path, {"pull_request": {"title": "Port (Defect-42)",
                                            "body": None}})
    assert chk.main(["--event-file", ev]) == 0


def test_non_pr_event_passes(tmp_path):
    assert chk.main(["--event-file", _event(tmp_path, {"ref": "refs/heads/x"})]) == 0


def test_unparseable_event_is_an_input_error(tmp_path):
    p = tmp_path / "e.json"
    p.write_text("{not json", encoding="utf-8")
    assert chk.main(["--event-file", str(p)]) == 2


# --- --base/--head over a real git history -------------------------------------

def _git(cwd, *args):
    subprocess.run(["git", *args], cwd=cwd, check=True, capture_output=True,
                   env={"GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@example.invalid",
                        "GIT_COMMITTER_NAME": "t",
                        "GIT_COMMITTER_EMAIL": "t@example.invalid",
                        "PATH": "/usr/bin:/bin", "HOME": str(cwd)})


@pytest.fixture
def repo(tmp_path, monkeypatch):
    _git(tmp_path, "init", "-q", "-b", "main")
    _git(tmp_path, "commit", "-q", "--allow-empty", "-m", "base: Defect #1 before the range")
    base = subprocess.run(["git", "rev-parse", "HEAD"], cwd=tmp_path,
                          capture_output=True, text=True, check=True).stdout.strip()
    monkeypatch.chdir(tmp_path)
    return tmp_path, base


def test_range_fails_on_commit_message_reference(repo):
    path, base = repo
    _git(path, "commit", "-q", "--allow-empty", "-m", "clean subject\n\nbody (Defect #42)")
    rc, out = run_main(["--base", base, "--head", "HEAD"])
    assert rc == 1
    assert ":3: body (Defect #42)" in out


def test_range_excludes_the_base_commit_itself(repo):
    path, base = repo
    _git(path, "commit", "-q", "--allow-empty", "-m", "fine (Defect-42)")
    assert chk.main(["--base", base, "--head", "HEAD"]) == 0


def test_bad_revision_is_an_input_error(repo):
    assert chk.main(["--base", "no-such-rev", "--head", "HEAD"]) == 2


# --- the live tree ----------------------------------------------------------------

LIVE_DOCS = ["README.md", "CLAUDE.md", "docs/TRACKING.md", "docs/building-linux.md",
             "docs/linux-parity-status.md"]


def test_live_docs_follow_the_convention():
    paths = [str(REPO / p) for p in LIVE_DOCS]
    assert chk.check_paths(paths) == [], "a live doc uses the autolinkable form"


def _step_block(wf: str, marker: str) -> str:
    """The text of the workflow step whose name contains *marker*."""
    start = wf.index(marker)
    nxt = wf.find("\n      - name:", start)
    return wf[start:] if nxt == -1 else wf[start:nxt]


def test_workflow_step_runs_selftests_and_every_mode_over_the_live_docs():
    wf = io.open(REPO / ".github" / "workflows" / "fast-checks.yml",
                 encoding="utf-8").read()
    step = _step_block(wf, "Internal defect references are not autolinked")
    assert "pytest -q tools/gate/test_check_defect_ref_autolink.py" in step
    assert "tools/gate/test_linux_recipe_coupling.py" in step, \
        "the Linux recipe coupling tests are no longer run"
    assert "python tools/gate/check_defect_ref_autolink.py" in step
    for flag in ("--base", "--head", "--event-file", "--paths"):
        assert flag in step, f"the step no longer passes {flag}"
    for doc in LIVE_DOCS:
        assert doc in step, f"the step no longer scans {doc}"
    assert "if: ${{ !cancelled() }}" in step
