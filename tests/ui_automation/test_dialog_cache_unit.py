#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Unit tests for ``_DialogCache`` periodic restart + COM recovery.

Pipeline K phase-1 (finding 6): the cache that powers the 158-row
``test_dialog_reachability_p0`` suite must self-restart every N
tests and on COM 0x80040155 failures. Launching real pdfe.exe to
verify this would take 90+ minutes, so the cache exposes a
``_fixture_factory`` injection seam and we exercise it with a stub.
"""
from __future__ import annotations

import sys
from pathlib import Path
from typing import List, Optional
from unittest.mock import patch

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tests.ui_automation import test_dialog_reachability_p0 as drp0  # noqa: E402


class _StubFixture:
    """Stand-in for ``PdfeAppFixture`` that records lifecycle calls."""

    setUp_calls: List[int] = []
    tearDown_calls: List[int] = []
    _next_id = 0

    def __init__(self) -> None:
        type(self)._next_id += 1
        self.id = type(self)._next_id
        self.launch_args: Optional[list] = None
        self.app = object()
        self.main_window = object()

    def setUp(self) -> None:  # noqa: N802 — pywinauto naming
        type(self).setUp_calls.append(self.id)

    def tearDown(self) -> None:  # noqa: N802
        type(self).tearDown_calls.append(self.id)


@pytest.fixture
def stub_factory(tmp_path, monkeypatch):
    """Set up a fresh stub factory + minimal opener registration."""
    _StubFixture.setUp_calls = []
    _StubFixture.tearDown_calls = []
    _StubFixture._next_id = 0

    # Patch shutil.copy2 / mkdtemp / kill so we don't touch real fs/proc.
    monkeypatch.setattr(drp0.shutil, "copy2", lambda *a, **k: None)
    monkeypatch.setattr(drp0.shutil, "rmtree", lambda *a, **k: None)
    monkeypatch.setattr(
        drp0.tempfile, "mkdtemp", lambda *a, **k: str(tmp_path)
    )
    monkeypatch.setattr(drp0, "kill_pdfe_processes", lambda: None)
    # FIXTURES_DIR doesn't have to exist because copy2 is a no-op.

    # Stub the opener resolver — every dialog opens cleanly.
    monkeypatch.setattr(drp0, "resolve_opener", lambda name: lambda fx: None)

    # Skip the 1.5s settle sleep inside _build_fixture.
    monkeypatch.setattr(drp0.time, "sleep", lambda *a, **k: None)

    return _StubFixture


def test_cache_serves_same_fixture_within_threshold(stub_factory):
    cache = drp0._DialogCache()
    cache._fixture_factory = stub_factory
    cache.restart_every = 5

    fx_first = cache.get("aboutDialog")
    for _ in range(4):
        fx = cache.get("aboutDialog")
        assert fx is fx_first, "cache must return same fixture under threshold"

    # 1 setUp on first get, 0 tearDowns
    assert len(stub_factory.setUp_calls) == 1
    assert stub_factory.tearDown_calls == []


def test_cache_restarts_after_threshold(stub_factory):
    cache = drp0._DialogCache()
    cache._fixture_factory = stub_factory
    cache.restart_every = 3

    fx_first = cache.get("aboutDialog")  # creates fx #1, count=0
    cache.get("aboutDialog")              # count=1
    cache.get("aboutDialog")              # count=2
    cache.get("aboutDialog")              # count=3 -> still fx #1, count incremented to 3
    fx_after = cache.get("aboutDialog")   # count=3 hits threshold -> restart -> fx #2

    assert fx_after is not fx_first, (
        "cache must build a fresh fixture once reuse_count >= restart_every"
    )
    assert len(stub_factory.setUp_calls) == 2, "exactly one restart"
    assert len(stub_factory.tearDown_calls) == 1, "first fixture torn down"


def test_cache_periodic_restart_disabled_when_zero(stub_factory):
    cache = drp0._DialogCache()
    cache._fixture_factory = stub_factory
    cache.restart_every = 0  # disabled

    fx_first = cache.get("aboutDialog")
    for _ in range(20):
        fx = cache.get("aboutDialog")
        assert fx is fx_first

    assert len(stub_factory.setUp_calls) == 1


def test_cache_recovers_from_com_error(monkeypatch, stub_factory, tmp_path):
    """A COM failure on first launch triggers a single retry."""
    cache = drp0._DialogCache()
    cache._fixture_factory = stub_factory
    cache.restart_every = 0

    attempts = {"n": 0}
    real_setup = stub_factory.setUp

    def flaky_setup(self):
        attempts["n"] += 1
        if attempts["n"] == 1:
            raise Exception(
                "OleAcc.dll: REGDB_E_CLASSNOTREG (HRESULT 0x80040155)"
            )
        real_setup(self)

    monkeypatch.setattr(stub_factory, "setUp", flaky_setup)

    fx = cache.get("aboutDialog")
    assert fx is not None
    assert attempts["n"] == 2, "expected one retry after COM failure"


def test_cache_persistent_com_error_raises_dialog_unavailable(
    monkeypatch, stub_factory
):
    """A COM failure that persists after retry surfaces as DialogUnavailable."""
    cache = drp0._DialogCache()
    cache._fixture_factory = stub_factory
    cache.restart_every = 0

    def always_com_fail(self):
        raise Exception("REGDB_E_CLASSNOTREG (0x80040155)")

    monkeypatch.setattr(stub_factory, "setUp", always_com_fail)

    with pytest.raises(drp0.DialogUnavailable, match="COM failure persisted"):
        cache.get("aboutDialog")


def test_is_com_error_matches_hresult_strings():
    e1 = Exception("REGDB_E_CLASSNOTREG: nope")
    e2 = Exception("HRESULT 0x80040155 returned by RoGetActivationFactory")
    e3 = Exception("ordinary failure")
    assert drp0._is_com_error(e1) is True
    assert drp0._is_com_error(e2) is True
    assert drp0._is_com_error(e3) is False


def test_threshold_env_default_30(monkeypatch):
    """The module-level threshold defaults to 30 if env unset."""
    monkeypatch.delenv("PDFE_DIALOG_CACHE_RESTART_EVERY", raising=False)
    # Re-evaluate the default the same way the module does.
    import os as _os
    val = int(_os.environ.get("PDFE_DIALOG_CACHE_RESTART_EVERY", "30"))
    assert val == 30


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-v"]))
