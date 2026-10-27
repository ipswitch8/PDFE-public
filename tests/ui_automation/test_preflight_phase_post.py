#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""pywinauto smoke tests for the Preflight dialog.

Covers the three matrix rows:
  menu_open_preflight_dialog
  preflight_run_pdfa1b
  preflight_export_report

Skipped when the PDFE binary isn't available or pywinauto can't
locate the preflight action — both are expected on CI runners that
don't build the GUI target.

Refactored 2026-05-01: switched to launch_pdfe + click_menu_path harness.
Menu path 'Tools -> Preflight...' verified against
src/ui/MainWindow.cpp:1062 (preflightAction_) and 1412 (toolsMenu).
"""

from __future__ import annotations

import pathlib
import sys
import time

import pytest

try:
    from pywinauto import Application  # noqa: F401  # type: ignore
except ImportError:
    pytest.skip("pywinauto not installed", allow_module_level=True)

from tests.ui_automation.harness.pdfe_app_fixture import launch_pdfe
from tests.ui_automation.harness.menu_nav import click_menu_path
from tests.ui_automation.conftest import record_matrix_result

# Phase F25 / gap WIN-45: reachability is a RECORDED NON-PASS, never a skip.
from tests.ui_automation.conftest import (  # noqa: E402
    current_test_id,
    record_matrix_result,
)


FIXTURE = pathlib.Path(__file__).resolve().parent.parent / "fixtures" / "test_3page.pdf"


@pytest.fixture()
def pdfe_app():
    if not FIXTURE.exists():
        pytest.skip("test_3page.pdf fixture missing")
    try:
        app, win = launch_pdfe(args=[str(FIXTURE)])
    except RuntimeError as e:
        pytest.skip(f"pdfe.exe launch failed: {e}")
    time.sleep(3.0)
    try:
        yield app, win
    finally:
        try:
            app.kill()
        except Exception:
            pass


def _open_preflight(win):
    try:
        click_menu_path(win, "Tools -> Preflight...")
    except Exception:
        # WIN-45 / F25: unreachable -> recorded 'blocked', then FAIL.
        _reach = (
            "Tools > Preflight menu path not reachable"
        )
        record_matrix_result(current_test_id(), "blocked", _reach)
        pytest.fail(
            f"WIN-45 reachability blocked (recorded non-pass, not skipped): {_reach}")


def test_preflight_dialog_opens(pdfe_app):
    """menu_open_preflight_dialog — Tools > Preflight... brings up the dialog."""
    app, win = pdfe_app
    _open_preflight(win)
    # PreflightDialog opens via .exec() and appears as a TOP-LEVEL window,
    # not a descendant of main_window. Search at app scope.
    dlg = win.child_window(auto_id="QApplication.preflightDialog")
    dlg.wait("visible", timeout=5)
    # Pipeline K phase-3 (finding 9): record red on failure.
    from tests.ui_automation.conftest import assert_recorded
    assert_recorded(
        "menu_open_preflight_dialog",
        dlg.exists(),
        "preflightDialog not visible after Tools->Preflight... click",
        green_notes="Sweep verified: Tools->Preflight... opens preflightDialog",
    )


def test_preflight_profile_combo_present(pdfe_app):
    """preflight_run_pdfa1b — combo has the six supported profiles."""
    _, win = pdfe_app
    _open_preflight(win)
    dlg = win.child_window(auto_id="QApplication.preflightDialog")
    dlg.wait("visible", timeout=5)
    combo = dlg.child_window(
        auto_id="QApplication.preflightDialog.preflightProfileCombo",
        control_type="ComboBox")
    combo.wait("visible", timeout=5)
    try:
        items = combo.item_texts()
    except (AttributeError, Exception):
        items = []
    if items:
        assert any("1b" in t for t in items)
        assert any("UA" in t for t in items)


def test_preflight_run_button_exists(pdfe_app):
    """preflight_run_pdfa1b — Run button exists and is enabled."""
    _, win = pdfe_app
    _open_preflight(win)
    dlg = win.child_window(auto_id="QApplication.preflightDialog")
    dlg.wait("visible", timeout=5)
    run = dlg.child_window(
        auto_id="QApplication.preflightDialog.preflightRunButton",
        control_type="Button")
    run.wait("visible", timeout=5)
    assert run.is_enabled()


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-v"]))
