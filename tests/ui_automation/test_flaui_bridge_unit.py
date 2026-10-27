#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Unit tests for tests.ui_automation.harness.flaui_bridge.

Pipeline L phase-2 (Option 3): the bridge is optional — pythonnet
must be installed AND FlaUI.Core/FlaUI.UIA3 must be on the .NET
load path. These tests verify graceful degradation when those
prerequisites are absent and exercise basic API when they are present.
"""
from __future__ import annotations

import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tests.ui_automation.harness import flaui_bridge


def test_is_available_returns_bool_without_raising():
    """is_available() must never raise. Return a bool regardless of
    whether pythonnet/FlaUI is installed."""
    result = flaui_bridge.is_available()
    assert isinstance(result, bool), (
        f"is_available() must return bool, got {type(result).__name__}"
    )


def test_load_error_populated_when_unavailable():
    """When is_available() is False, load_error() must return a
    non-empty diagnostic string explaining what's missing."""
    if flaui_bridge.is_available():
        pytest.skip("FlaUI is available — load_error semantics N/A")
    err = flaui_bridge.load_error()
    assert err is not None and len(err) > 0, (
        "load_error() must return a non-empty diagnostic when "
        "is_available() is False"
    )
    # Common load-failure terms that should appear in the diagnostic.
    expected_substrings = ("pythonnet", "FlaUI", "load path", "nuget")
    assert any(s.lower() in err.lower() for s in expected_substrings), (
        f"load_error() should mention pythonnet/FlaUI/loading hints; "
        f"got {err!r}"
    )


def test_find_window_returns_none_when_unavailable():
    """find_window_by_class returns None (not raises) when bridge is unavailable."""
    if flaui_bridge.is_available():
        pytest.skip("FlaUI available — testing fallback behavior only")
    result = flaui_bridge.find_window_by_class("Shell_TrayWnd")
    assert result is None, (
        "find_window_by_class must return None (not raise) when "
        f"bridge unavailable; got {result!r}"
    )


def test_find_window_finds_taskbar_when_available():
    """When FlaUI is available, find_window_by_class('Shell_TrayWnd')
    must return a non-None Element — Shell_TrayWnd is the Windows
    taskbar and is always present on a logged-in session."""
    if not flaui_bridge.is_available():
        pytest.skip(
            "FlaUI not available on this host — install via nuget and "
            f"set PDFE_FLAUI_DLL_DIR. Diagnostic: {flaui_bridge.load_error()}"
        )
    win = flaui_bridge.find_window_by_class("Shell_TrayWnd")
    assert win is not None, (
        "Shell_TrayWnd is the Windows taskbar — should always be "
        "present on a logged-in session"
    )


def test_unavailable_bridge_swallows_action_calls():
    """click(), get_value(), set_value() on None or unavailable bridge
    must return safely (False/None) — never raise."""
    if flaui_bridge.is_available():
        pytest.skip("FlaUI available — testing graceful-degrade only")
    assert flaui_bridge.click(None) is False
    assert flaui_bridge.get_value(None) is None
    assert flaui_bridge.set_value(None, "x") is False


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-v"]))
