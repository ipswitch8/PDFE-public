# -*- coding: utf-8 -*-
"""The bootstrap pin guard's two new rules (GitHub #129).

WHY THESE EXIST. `Bootstrap-PdfeRunner.ps1` is the one file copied to a new
machine by hand; everything that machine then receives is decided by its `$Ref`.
That pin has gone stale **twice, by two unrelated mechanisms**, and the file's
own extensive staleness warning did not help either time, because neither
failure arrived by the route the warning anticipated:

  1. hand-maintenance -- an early pin predated three later fixes;
  2. an automated docs remap (`8cc495be`) rewrote it **backwards**, from
     `fc0af9f` to `5f7680a`, because a bare 7-hex string in a `.ps1` is
     indistinguishable from a documentation citation to a regex.

The pre-existing check caught neither of these. It verified that the pin EXISTS
(`cat-file -e`) and that no package file changed after it. Mechanism 2 passes
both: the new SHA exists, it is an ancestor of HEAD, and the package files
happened not to change in between, so the staleness diff was empty. Nothing was
wrong with the RESULT; what was wrong was the DIRECTION. It survived a month.

  * **Rule A** -- the pin must be an ANCESTOR of HEAD, not merely an existing
    object. This repository has had two history rewrites that re-hashed every
    commit, and abandoned objects survive locally (and were still served by the
    remote afterwards), so existence is a much weaker statement here than it
    looks. Measured from the pin's own history: `fed52797` pinned `fc0af9f` and
    `5356cd78` pinned `029cf0e`; **both are dead SHAs today**.
  * **Rule B** -- the pin must never move BACKWARDS relative to its own previous
    value. This is mechanism 2 exactly.

The control case matters as much as the kills: a check that only ever reddens
gets bypassed (DCA 0047).
"""
from __future__ import annotations

import importlib.util
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
_SPEC = importlib.util.spec_from_file_location(
    "check_runner_deploy_pin_under_test",
    ROOT / "tools" / "gate" / "check_runner_deploy_pin.py")
assert _SPEC and _SPEC.loader
pin = importlib.util.module_from_spec(_SPEC)
sys.modules["check_runner_deploy_pin_under_test"] = pin
_SPEC.loader.exec_module(pin)

BOOTSTRAP = ROOT / "tools" / "runner-deployment" / "Bootstrap-PdfeRunner.ps1"


def _git(*args: str) -> tuple[int, str]:
    p = subprocess.run(["git", *args], cwd=str(ROOT), capture_output=True,
                       text=True, encoding="utf-8", errors="replace")
    return p.returncode, p.stdout


def _set_pin(value: str) -> None:
    """Rewrite the pin in place, preserving line endings."""
    raw = BOOTSTRAP.read_text(encoding="utf-8")
    cur, _ = pin.parse_bootstrap(raw)
    assert cur, "could not read the current pin"
    BOOTSTRAP.write_text(raw.replace("'%s'" % cur, "'%s'" % value, 1),
                         encoding="utf-8", newline="")


def test_the_current_tree_passes():
    """The control. A guard that cannot be satisfied gets bypassed."""
    assert pin.check(str(ROOT)) == []


def _dangling_commit() -> str:
    """A commit object that EXISTS locally and is NOT an ancestor of HEAD.

    Built here rather than borrowed from history. The previous version of the
    test below pinned `fc0af9f` -- a real, now-dead pin from this repository --
    and skipped when that object had been garbage-collected locally, which it
    since has been.

    **The skip reason was wrong**, which is why this exists. It said "the
    ancestry rule is still exercised by the fabricated-SHA case below". It is
    not: `check_runner_deploy_pin.py` does `cat-file -e` and **returns early**
    when the object does not exist (lines 136-141), so a fabricated SHA never
    reaches the ancestry check at line 161. The fabricated case proves the
    existence rule; nothing proved RULE A.

    So the rule added for GitHub #129 -- after TWO real dead pins were found
    live in the tree -- had no live coverage, behind a skip whose justification
    was false. A skipped test is not a passing test, and a skip reason is a
    claim like any other.

    `git commit-tree` writes a real, parentless commit: it exists, so
    `cat-file -e` passes, and it is unreachable from HEAD, so the ancestry
    check must fire. Deterministic, and it depends on no object surviving GC.

    IDENTITY SUPPLIED IN THE ENVIRONMENT, NOT ASSUMED
    -------------------------------------------------
    `git commit-tree` writes a commit object, so it needs an author and a
    committer. The first version of this helper supplied neither and passed
    locally only because this workstation has a configured identity. **On the
    CI runner it exited 128** -- "empty ident name (for <runner@...>)" -- so a
    test written to remove a false skip shipped red for four commits. The
    identity is now passed explicitly, which also makes the probe independent
    of whatever git config the machine happens to carry.

    The dates are pinned too. Without them the object's hash includes the
    current time, so every run writes a NEW unreachable commit into the object
    database. Fixed dates make the probe byte-identical run to run.
    """
    rc, tree = _git("rev-parse", "HEAD^{tree}")
    assert rc == 0 and tree.strip(), "could not read HEAD's tree"
    env = dict(os.environ)
    env.update({
        "GIT_AUTHOR_NAME": "pdfe",
        "GIT_AUTHOR_EMAIL": "pdfe@itwerx.net",
        "GIT_COMMITTER_NAME": "pdfe",
        "GIT_COMMITTER_EMAIL": "pdfe@itwerx.net",
        "GIT_AUTHOR_DATE": "2000-01-01T00:00:00+00:00",
        "GIT_COMMITTER_DATE": "2000-01-01T00:00:00+00:00",
    })
    p = subprocess.run(
        ["git", "commit-tree", tree.strip(), "-m", "pin-rule-a probe"],
        cwd=str(ROOT), capture_output=True, text=True, env=env,
        encoding="utf-8", errors="replace")
    assert p.returncode == 0, (
        "commit-tree failed; the probe cannot be built:\n" + p.stderr)
    sha = p.stdout.strip()
    assert sha, "commit-tree produced no object"
    return sha


def test_rule_a_a_dead_sha_is_caught_even_though_it_exists():
    """RULE A: existence is not validity.

    The pin must be an ANCESTOR of HEAD, not merely an object that exists.
    After this repository's history rewrites, abandoned objects survived
    locally and were still served by the remote, so `cat-file -e` passing says
    much less here than it appears to.
    """
    sha = _dangling_commit()

    # The premise, asserted rather than assumed: the probe must EXIST and must
    # NOT be an ancestor. If either half were untrue the test could pass for
    # the wrong reason.
    rc, _ = _git("cat-file", "-e", "%s^{commit}" % sha)
    assert rc == 0, "probe commit does not exist; the test would prove nothing"
    rc, _ = _git("merge-base", "--is-ancestor", sha, "HEAD")
    assert rc != 0, "probe commit IS an ancestor; it cannot exercise RULE A"

    original = BOOTSTRAP.read_text(encoding="utf-8")
    try:
        _set_pin(sha)
        problems = pin.check(str(ROOT))
        assert any("NOT an ancestor" in p for p in problems), problems
    finally:
        BOOTSTRAP.write_text(original, encoding="utf-8", newline="")
    assert BOOTSTRAP.read_text(encoding="utf-8") == original


def test_rule_a_is_unreachable_for_a_nonexistent_sha():
    """Pins the reason the old skip justification was false.

    A fabricated SHA fails the existence check and RETURNS EARLY, so it cannot
    exercise RULE A. If someone later removes that early return, this test
    fails and the comment above stops being true -- which is the point.
    """
    original = BOOTSTRAP.read_text(encoding="utf-8")
    try:
        _set_pin("0" * 40)
        problems = pin.check(str(ROOT))
        assert any("not a commit in this repository" in p for p in problems), \
            problems
        assert not any("NOT an ancestor" in p for p in problems), (
            "a nonexistent SHA reached the ancestry rule; the old skip reason "
            "would now be correct, and this test documents that it was not")
    finally:
        BOOTSTRAP.write_text(original, encoding="utf-8", newline="")
    assert BOOTSTRAP.read_text(encoding="utf-8") == original


def test_check_is_independent_of_the_process_cwd():
    """`check(repo_root)` must not depend on where the process is standing.

    Found by the `validator` gate of phase-6 while verifying this checker:
    `check()` honoured `repo_root` for the bootstrap's FILE path while every
    `_git()` call ran in the PROCESS cwd. A caller passing a root therefore got
    the bootstrap from one repository and the history of whichever repository
    the shell happened to be in.

    It was invisible because both callers ran with cwd == repo_root — the
    workflow invokes the checker from the repo root, and this suite passes
    `str(ROOT)` while pytest's cwd is also the root. The parameter was honoured
    in one half and ignored in the other, which is the "wired into one of two
    call paths" shape this project has recorded before. A test that passes a
    root while relying on cwd is not testing the root.
    """
    import os
    import tempfile

    from_root = pin.check(str(ROOT))

    original = os.getcwd()
    tmp = tempfile.mkdtemp()
    try:
        os.chdir(tmp)
        from_elsewhere = pin.check(str(ROOT))
    finally:
        # chdir OUT before removing it: on Windows a directory cannot be
        # removed while it is the cwd.
        os.chdir(original)
        try:
            os.rmdir(tmp)
        except OSError:
            pass

    assert from_root == from_elsewhere, (
        "the checker's answer depends on the process cwd: %r from the root vs "
        "%r from elsewhere" % (from_root, from_elsewhere))


def test_every_git_call_threads_the_repo_root():
    """Checked on the AST, because a missed call is the whole defect.

    One multi-line `_git(...)` call escaped a single-line regex during the fix
    — a partial threading would have left exactly the half-wired behaviour
    being repaired, and would have looked fixed.
    """
    import ast

    source = (ROOT / "tools" / "gate" / "check_runner_deploy_pin.py").read_text(
        encoding="utf-8")
    missing = [
        node.lineno
        for node in ast.walk(ast.parse(source))
        if isinstance(node, ast.Call)
        and isinstance(node.func, ast.Name)
        and node.func.id == "_git"
        and not any(kw.arg == "cwd" for kw in node.keywords)
    ]
    assert not missing, (
        "these _git() calls do not pass cwd=, so they run in the process cwd "
        "rather than the repo_root the caller asked for: lines %s" % missing)


def test_rule_b_moving_the_pin_backwards_is_caught():
    """Mechanism 2: the remap rewrote the pin to an EARLIER commit.

    Every existing rule passes on this — the SHA exists, it is an ancestor of
    HEAD, and if no package file changed in between the staleness diff is empty.
    Only the direction is wrong.
    """
    original = BOOTSTRAP.read_text(encoding="utf-8")
    current, _ = pin.parse_bootstrap(original)
    # An ancestor of the current pin, i.e. strictly backwards.
    rc, out = _git("rev-list", "--max-count=8", current)
    assert rc == 0 and out.split(), "could not walk back from the current pin"
    older = out.split()[-1]
    assert older != current

    try:
        _set_pin(older)
        problems = pin.check(str(ROOT))
        assert any("MOVED BACKWARDS" in p or "STALE" in p for p in problems), (
            "moving the pin backwards produced no complaint at all: " + repr(problems))
    finally:
        BOOTSTRAP.write_text(original, encoding="utf-8", newline="")
    assert BOOTSTRAP.read_text(encoding="utf-8") == original


def test_rule_a_a_fabricated_sha_is_caught():
    """A plausible-looking SHA that is not in this history at all."""
    original = BOOTSTRAP.read_text(encoding="utf-8")
    try:
        _set_pin("0123456789abcdef0123456789abcdef01234567")
        problems = pin.check(str(ROOT))
        assert problems, "a fabricated pin produced no complaint"
        assert any("not a commit" in p or "NOT an ancestor" in p
                   for p in problems), problems
    finally:
        BOOTSTRAP.write_text(original, encoding="utf-8", newline="")
    assert BOOTSTRAP.read_text(encoding="utf-8") == original


def test_previous_pin_skips_unchanged_values():
    """Rule B would be inert if it compared against HEAD~'s value.

    The pin usually stays put across many commits that touch the bootstrap for
    other reasons, so `_previous_pin` must return the last value that actually
    DIFFERED.
    """
    original = BOOTSTRAP.read_text(encoding="utf-8")
    current, _ = pin.parse_bootstrap(original)
    prev = pin._previous_pin(current)
    if prev:
        assert prev != current, "previous pin must differ from the current one"
