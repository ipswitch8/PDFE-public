# -*- coding: utf-8 -*-
"""F69(b) — the reusable "did the rebuild actually happen" assertion.

The defect it generalises: a `shutil.copy2`-style restore puts the original
BYTES back and the original MTIME with them, so the build system skips the file
and leaves a half-mutated binary that every later step reports on as clean.

The first fix for that was an `os.utime` stamp inside one throwaway script, and
F69 is explicit that "a fix inside one throwaway script is not a control". These
cases pin the PROPERTY instead -- binary newer than sources -- which holds
however the restore was performed.
"""
from __future__ import annotations

import importlib.util
import os
import sys
import time
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
_SPEC = importlib.util.spec_from_file_location(
    "rebuild_assert_under_test", HERE / "rebuild_assert.py")
assert _SPEC and _SPEC.loader
ra = importlib.util.module_from_spec(_SPEC)
sys.modules["rebuild_assert_under_test"] = ra
_SPEC.loader.exec_module(ra)


def _touch(p: Path, when: float) -> None:
    p.write_text("x", encoding="utf-8")
    os.utime(p, (when, when))


def test_a_binary_newer_than_its_sources_passes(tmp_path):
    src = tmp_path / "a.cpp"
    exe = tmp_path / "a.exe"
    _touch(src, time.time() - 100)
    _touch(exe, time.time())
    ok, msg = ra.binary_postdates_sources(str(exe), [str(src)])
    assert ok, msg


def test_the_copy2_scenario_is_caught(tmp_path):
    """THE DEFECT: restore preserved the source's original mtime, so the source
    ends up NEWER than the binary and the rebuild silently did not happen."""
    src = tmp_path / "a.cpp"
    exe = tmp_path / "a.exe"
    _touch(exe, time.time() - 100)
    _touch(src, time.time())          # restored "later" than the build
    ok, msg = ra.binary_postdates_sources(str(exe), [str(src)])
    assert not ok
    assert "STALE BINARY" in msg
    assert "a.cpp" in msg, "the message must name WHICH source, not just 'stale'"


def test_a_missing_binary_is_a_failure_not_a_pass(tmp_path):
    """An absent artifact cannot be proved fresh. Treating 'nothing to compare'
    as success is how a check that never runs reports green."""
    src = tmp_path / "a.cpp"
    _touch(src, time.time())
    ok, msg = ra.binary_postdates_sources(str(tmp_path / "nope.exe"), [str(src)])
    assert not ok
    assert "does not exist" in msg


def test_an_empty_source_set_fails_rather_than_passing_vacuously(tmp_path):
    exe = tmp_path / "a.exe"
    _touch(exe, time.time())
    ok, msg = ra.binary_postdates_sources(str(exe), [])
    assert not ok
    assert "vacuously" in msg


def test_the_newest_source_is_the_one_that_decides(tmp_path):
    """With several sources, an OLD one must not mask a freshly-restored one."""
    old = tmp_path / "old.cpp"
    new = tmp_path / "new.cpp"
    exe = tmp_path / "a.exe"
    _touch(old, time.time() - 500)
    _touch(exe, time.time() - 100)
    _touch(new, time.time())
    ok, msg = ra.binary_postdates_sources(str(exe), [str(old), str(new)])
    assert not ok
    assert "new.cpp" in msg


def test_the_mutation_harness_actually_calls_it(tmp_path):
    """A control nothing invokes is decoration. Pin the call site by content.

    Deliberately checks the IMPORT and the CALL, not just the module name in a
    comment -- this repo has shipped a guard whose only presence was a mention.
    """
    text = (HERE / "mutation_kill.py").read_text(encoding="utf-8")
    assert "from rebuild_assert import binary_postdates_sources" in text, (
        "mutation_kill.py does not import the rebuild assertion")
    assert "binary_postdates_sources(exe, sorted(set(touched_files)))" in text, (
        "mutation_kill.py imports the rebuild assertion but never calls it")


def test_the_check_runs_only_after_a_rebuild_is_attempted(tmp_path):
    """THE DEFECT THE karen GATE CAUGHT, pinned so it cannot come back.

    The first wiring asserted freshness inside `apply_and_test`'s `finally`,
    immediately after the restore. At that instant the answer is fixed: the
    binary was built BEFORE the restore, and `_write()` stamps a fresh mtime on
    the source, so the source is newer BY CONSTRUCTION. The check fired on every
    run, healthy or not, and appended a false "[F69b] STALE BINARY" to every
    mutant record. **A guard that can only fail is as useless as one that can
    never fail** -- it carries no information either way.

    The property is only a real question once a rebuild has been ATTEMPTED since
    the restore. So the call must live in the post-loop helper, which rebuilds
    first, and must NOT live in the per-mutant restore path.
    """
    text = (HERE / "mutation_kill.py").read_text(encoding="utf-8")

    helper_at = text.index("def _assert_tree_rebuilt_after_restore")
    call_at = text.index("binary_postdates_sources(exe, sorted(set(touched_files)))")
    assert call_at > helper_at, (
        "the freshness call is outside the post-rebuild helper")

    # A rebuild must be attempted BEFORE the assertion inside that helper.
    build_at = text.index("bcode, _ = build(", helper_at)
    assert build_at < call_at, (
        "the helper asserts freshness without first attempting a rebuild, so it "
        "is asking a question with only one possible answer")

    # And it must NOT have crept back into the per-mutant restore path.
    restore_at = text.index("# ALWAYS restore, byte-for-byte")
    finally_region = text[restore_at:helper_at]
    assert "binary_postdates_sources(" not in finally_region, (
        "the freshness assertion is back in apply_and_test's finally, where it "
        "always fires -- see this test's docstring")
