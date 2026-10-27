# -*- coding: utf-8 -*-
"""Self-tests for the native-UI harness itself (pipeline c4a9e271 phase-3).

These do NOT test the product — they verify the harness invariants the whole
visual+interaction suite depends on, so a regression in the plumbing is caught
here rather than as confusing flake in a hundred downstream tests:

  1. The autouse teardown contract — ``conftest._cleanup_pdfe_between_tests``
     calls ``kill_pdfe_processes()`` + ``_wait_process_exit("pdfe.exe", ...)``.
     This file exercises those exact functions and proves an orphaned pdfe.exe
     is gone within 5 seconds (the CLAUDE.md "no orphaned PDFE.exe" rule — an
     orphan wedges the next test's launch).

  2. The same teardown is a fast no-op when nothing is running.

``_wait_process_exit`` is best-effort and always returns ``None`` (it polls
until the process is gone *or* the timeout elapses, then returns either way), so
the source of truth for "the orphan is gone" is a live process-count check, not
the function's return value.

Per SKIP=FAIL: the only skip path is ``require_binary`` (the Release pdfe.exe
not being built is an environment condition, not a harness defect). Every other
failure is a hard assertion.
"""
from __future__ import annotations

import time

import psutil
import pytest

from tests.ui_automation.harness import render_drive as d
from tests.ui_automation.harness.pdfe_app_fixture import (
    kill_pdfe_processes,
    _wait_process_exit,
)

# A small, always-present fixture is enough — we only need a live pdfe.exe.
_FIXTURE = d.FIXTURES / "test_single.pdf"

TEARDOWN_BUDGET_S = 5.0


def _pdfe_count() -> int:
    """Number of live processes named pdfe.exe (case-insensitive)."""
    n = 0
    for proc in psutil.process_iter(["name"]):
        try:
            if (proc.info["name"] or "").lower() == "pdfe.exe":
                n += 1
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            pass
    return n


def test_autouse_teardown_kills_orphan_within_5s():
    """A launched pdfe.exe is killed by the teardown primitives within 5s."""
    d.require_binary()
    if not _FIXTURE.exists():
        pytest.skip(f"fixture missing: {_FIXTURE} (environment, not a harness bug)")

    app, _win = d.open_doc(_FIXTURE, settle_s=1.5)
    try:
        assert _pdfe_count() >= 1, (
            "expected at least one pdfe.exe after open_doc(); the launch path "
            "itself is broken, so the teardown contract cannot be verified")

        t0 = time.time()
        kill_pdfe_processes()
        _wait_process_exit("pdfe.exe", timeout=TEARDOWN_BUDGET_S)
        elapsed = time.time() - t0

        # Source of truth: the live process count, not the best-effort return.
        assert _pdfe_count() == 0, (
            f"a pdfe.exe orphan survived the teardown kill after {elapsed:.2f}s "
            f"— the autouse teardown would leak it and wedge the next launch")
        assert elapsed < TEARDOWN_BUDGET_S, (
            f"teardown took {elapsed:.2f}s to clear the orphan "
            f"(budget {TEARDOWN_BUDGET_S:.0f}s)")
    finally:
        # Belt-and-suspenders: render_drive teardown + the autouse hook both run.
        d.teardown(app)


def test_teardown_is_fast_noop_when_no_pdfe():
    """With no pdfe.exe running, the teardown primitives return cleanly/fast."""
    # Ensure a clean slate first (does not count against the timed assertion).
    kill_pdfe_processes()
    _wait_process_exit("pdfe.exe", timeout=TEARDOWN_BUDGET_S)
    assert _pdfe_count() == 0, "could not reach a no-pdfe baseline for the no-op test"

    t0 = time.time()
    kill_pdfe_processes()
    _wait_process_exit("pdfe.exe", timeout=TEARDOWN_BUDGET_S)
    elapsed = time.time() - t0

    assert _pdfe_count() == 0, "no-op teardown must leave zero pdfe.exe"
    assert elapsed < TEARDOWN_BUDGET_S, (
        f"no-op teardown took {elapsed:.2f}s (budget {TEARDOWN_BUDGET_S:.0f}s)")
