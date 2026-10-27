#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""The per-session basetemp must stay per-session (GitHub #257).

WHY THIS FILE EXISTS

The #257 fix — a repo-root ``conftest.py`` that roots each session at
``tests/_tmp/s<pid>`` — was, when first submitted, verified only by concurrency
runs performed BY HAND. The phase-5 claim-check gate said so plainly: nothing
referenced ``_unique_basetemp`` outside ``conftest.py`` itself, so the fix could
be undone with no test going red.

It then demonstrated the undoing, which is the part worth keeping:

    addopts = -p no:capture --basetemp=tests/_tmp

``addopts`` populates ``config.option.basetemp`` BEFORE ``pytest_configure``
runs, so the root hook's ``if getattr(config.option, "basetemp", None): return``
early-returns and every session shares one directory again — exactly the
pre-#257 bug, reintroduced by one line, silently.

So the regression these tests exist to catch is not hypothetical: it is a single
edit to ``pytest.ini`` that looks harmless and restores a defect whose symptom is
fixtures vanishing mid-run.

THE TWO PROPERTIES

  * per-session — two sessions must not share a directory;
  * INSIDE the repo and under ``tests/_tmp/`` — ``check_crl_chain_fixture``
    resolves its corpus under REPO_ROOT, and four guards exclude the
    ``tests/_tmp`` prefix when scanning the tree.
"""
from __future__ import annotations

import importlib.util
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
ROOT_CONFTEST = REPO_ROOT / "conftest.py"
PYTEST_INI = REPO_ROOT / "pytest.ini"


def _root_conftest():
    spec = importlib.util.spec_from_file_location("_root_conftest_under_test",
                                                  ROOT_CONFTEST)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class _Opt:
    def __init__(self, basetemp=None):
        self.basetemp = basetemp


class _Config:
    def __init__(self, basetemp=None):
        self.option = _Opt(basetemp)


# ---------------------------------------------------------------------------
# The regression that motivated this file.
# ---------------------------------------------------------------------------

def test_pytest_ini_does_not_set_basetemp():
    """One line in pytest.ini silently restores the shared directory.

    addopts is applied before pytest_configure, so a --basetemp there makes the
    root hook early-return and every session shares one tree again. This is the
    cheapest possible guard on the whole fix.
    """
    text = PYTEST_INI.read_text(encoding="utf-8")
    addopts = [ln for ln in text.splitlines()
               if ln.strip().startswith("addopts")]
    assert addopts, "pytest.ini has no addopts line at all"
    for line in addopts:
        assert "--basetemp" not in line, (
            "pytest.ini sets --basetemp again (%r). That populates "
            "config.option.basetemp before pytest_configure runs, so the "
            "repo-root conftest early-returns and every session shares one "
            "directory -- the GitHub #257 defect, restored silently." % line)


def test_the_root_conftest_exists():
    """Deleting it is the other one-step undo.

    Without it every session falls back to pytest's default basetemp in %TEMP%,
    outside the repository, which breaks check_crl_chain_fixture's in-repo
    assertion and removes the tests/_tmp prefix four guards exclude on.
    """
    assert ROOT_CONFTEST.is_file(), (
        "the repo-root conftest.py is gone; sessions will fall back to a "
        "basetemp outside the repository (GitHub #257)")


# ---------------------------------------------------------------------------
# The mechanism itself.
# ---------------------------------------------------------------------------

def test_two_sessions_do_not_share_a_directory():
    mod = _root_conftest()
    first = mod._unique_basetemp()
    first.mkdir(parents=True, exist_ok=True)
    try:
        second = mod._unique_basetemp()
        assert first != second, (
            "a second session resolved the same basetemp as the first; pytest "
            "deletes an explicit basetemp at session start, so this is the "
            "shared-directory defect")
    finally:
        try:
            first.rmdir()
        except OSError:
            pass


def test_the_basetemp_is_inside_the_repo_and_under_tests_tmp():
    mod = _root_conftest()
    p = mod._unique_basetemp()
    assert p.is_relative_to(REPO_ROOT), (
        "basetemp resolved outside the repository; check_crl_chain_fixture "
        "would silently skip every mutation")
    assert p.is_relative_to(REPO_ROOT / "tests" / "_tmp"), (
        "basetemp resolved outside tests/_tmp; the janitor sweeps that root "
        "and four guards exclude that prefix when scanning the tree")


def test_an_explicit_basetemp_wins():
    mod = _root_conftest()
    cfg = _Config(basetemp="/somewhere/chosen")
    mod.pytest_configure(cfg)
    assert cfg.option.basetemp == "/somewhere/chosen", (
        "the hook overrode an explicit --basetemp given on the command line")


def test_the_hook_fills_in_a_basetemp_when_none_is_given():
    """The converse: it must actually DO something when unset."""
    mod = _root_conftest()
    cfg = _Config(basetemp=None)
    mod.pytest_configure(cfg)
    assert cfg.option.basetemp, "the hook left basetemp unset"
    assigned = Path(cfg.option.basetemp)
    assert assigned.is_relative_to(REPO_ROOT / "tests" / "_tmp")
    try:
        assigned.rmdir()
    except OSError:
        pass


def test_a_reused_pid_gets_a_distinct_directory(tmp_path, monkeypatch):
    """Windows recycles pids promptly, and a crashed session leaves its
    directory behind, so the bare pid is not unique over time."""
    mod = _root_conftest()
    monkeypatch.setattr(mod, "_TESTS_TMP", tmp_path)
    first = mod._unique_basetemp()
    first.mkdir(parents=True, exist_ok=True)
    second = mod._unique_basetemp()
    assert second != first
    assert second.name.startswith(first.name), (
        "the disambiguated name should extend the pid name, got %r" % second.name)
