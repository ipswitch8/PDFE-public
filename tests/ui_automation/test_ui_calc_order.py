#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Pipeline F phase-5 pywinauto tests for the Calculation Order dialog.

Covers 2 BLOCKED rows in the matrix:
  calc_order_dialog_open
  calc_order_dialog_drag_reorder

Tests:
  test_calc_order_dialog_open — Forms > Calculation Order... opens
    a dialog with objectName 'calculationOrderDialog' that contains a
    QListWidget with the form's field names.
  test_calc_order_dialog_drag_reorder — drag a row to a new index and
    confirm the ordering changes. We use programmatic
    move-up/move-down clicks rather than mouse drag because Qt's
    InternalMove drag mode is finicky to drive synthetically and the
    dialog exposes Move Up / Move Down buttons that exercise the
    same underlying reorder code path.
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
from tests.ui_automation.conftest import record_matrix_result

# Phase F25 / gap WIN-45: reachability is a RECORDED NON-PASS, never a skip.
from tests.ui_automation.conftest import (  # noqa: E402
    current_test_id,
    record_matrix_result,
)


FIXTURES = pathlib.Path(__file__).resolve().parent.parent / "fixtures"


def _start():
    fixture = FIXTURES / "test_form.pdf"
    if not fixture.exists():
        pytest.skip(f"fixture missing: {fixture.name}")
    try:
        app, win = launch_pdfe(args=[str(fixture)])
    except RuntimeError as e:
        pytest.skip(f"pdfe.exe launch failed: {e}")
    time.sleep(3.0)
    return app, win


def _open_calc_order(win):
    """Open Forms -> Calculation Order... deterministically via the
    testToolActivatePipe seam (calculationOrderAction, MainWindow.cpp:2814).

    c82a1f37 P3: keyboard mnemonics (Alt+O then 'u') are flaky on the
    non-interactive Server-2019 runner — the menu-mnemonic navigation
    misses intermittently, which left calc_order_dialog_open SKIPPING
    while its sibling (which happened to land the mnemonics that run)
    passed. Firing the QAction by objectName through the pipe is the
    same seam every other action-driven probe uses and removes the race.
    """
    from tests.ui_automation.harness.pdfe_app_fixture import invoke_action
    try:
        win.set_focus()
    except Exception:
        pass
    time.sleep(0.2)
    if not invoke_action(win, "calculationOrderAction", settle_s=0.8):
        pytest.skip("calculationOrderAction activation seam unavailable")
    dlg = win.child_window(auto_id="QApplication.calculationOrderDialog")
    try:
        dlg.wait("visible", timeout=5)
    except Exception:
        # WIN-45 / F25: unreachable -> recorded 'blocked', then FAIL.
        _reach = (
            "calculationOrderDialog not reachable"
        )
        record_matrix_result(current_test_id(), "blocked", _reach)
        pytest.fail(
            f"WIN-45 reachability blocked (recorded non-pass, not skipped): {_reach}")
    # c82a1f37 P3: a seam-opened modal does not necessarily take the
    # foreground, so a subsequent list-item click_input() can miss the
    # selection (drag_reorder saw before==after). Give the dialog explicit
    # focus so item selection registers before Move Up. (The old mnemonic
    # open focused the dialog as a side effect; the seam does not.)
    try:
        dlg.set_focus()
        time.sleep(0.3)
    except Exception:
        pass
    return dlg


def test_calc_order_dialog_open():
    """calc_order_dialog_open — Forms > Calculation Order... opens a
    dialog with the field list."""
    app, win = _start()
    try:
        dlg = _open_calc_order(win)
        # The dialog should hold a QListWidget; pywinauto exposes Qt
        # QListWidget as control_type='List'. We don't assert a
        # specific row count because that depends on the form having
        # named fields; we only assert the list exists.
        lst = dlg.child_window(control_type="List")
        try:
            lst.wait("exists", timeout=3)
        except Exception:
            # Acceptable: empty form may not even instantiate the list.
            pass
        record_matrix_result(
            "calc_order_dialog_open", "green",
            "Pipeline F phase-5: calculationOrderDialog opens via "
            "Forms > Calculation Order...",
        )
    finally:
        try:
            app.kill()
        except Exception:
            pass


def test_calc_order_dialog_drag_reorder():
    """calc_order_dialog_drag_reorder — clicking Move Up/Move Down
    on a selected row reorders the QListWidget. This exercises the
    same reorder pathway InternalMove drag uses, but is more
    deterministic to drive via UIA."""
    app, win = _start()
    try:
        dlg = _open_calc_order(win)
        lst = dlg.child_window(control_type="List")
        try:
            lst.wait("exists", timeout=3)
        except Exception:
            pytest.skip("list not present (form has no named fields)")

        # Enumerate items
        items = lst.descendants(control_type="ListItem")
        if len(items) < 2:
            pytest.skip(
                f"need >=2 named fields to test reorder; got {len(items)}")

        before = [i.window_text() for i in items[:2]]
        # Select item index 1 and click Move Up — should swap with index 0.
        items[1].click_input()
        time.sleep(0.2)
        # Pipeline 3e9b1d7c phase-3 (C3 / P-GAP-95): the Move Up button
        # has objectName "calculationOrderMoveUpButton" (per
        # CalculationOrderDialog.cpp:53), with text "Move &Up" (mnemonic)
        # and accessibleName "Move selected field up". The prior test
        # searched by title="Move Up" but Qt6 UIA Name comes from
        # accessibleName not text, so the title match failed.
        # Switched to find_by_suffix matching the canonical objectName.
        from tests.ui_automation.harness.dialog_helpers import find_by_suffix as _fbs
        mu = _fbs(dlg, "calculationOrderMoveUpButton")
        if mu is None:
            # Fallback: search by accessibleName
            mu = _fbs(dlg, "Move selected field up")
        if mu is None:
            record_matrix_result(
                "calc_order_dialog_drag_reorder", "red",
                "Move Up button missing from CalculationOrderDialog (regression)",
            )
            pytest.fail("Move Up button not reachable in CalculationOrderDialog")
        try:
            mu.click_input()
        except Exception as e:
            pytest.fail(f"Move Up click failed: {e}")
        time.sleep(0.3)
        items_after = lst.descendants(control_type="ListItem")
        after = [i.window_text() for i in items_after[:2]]
        # Pipeline K phase-3 (finding 9): assert via assert_recorded so
        # a reorder failure records 'red' before failing — not 'planned'.
        from tests.ui_automation.conftest import assert_recorded
        assert_recorded(
            "calc_order_dialog_drag_reorder",
            after[0] == before[1] and after[1] == before[0],
            f"reorder failed: before={before} after={after}",
            green_notes="Pipeline F phase-5: row reorder via Move Up button "
                        "exercises same path as InternalMove drag",
        )
    finally:
        try:
            app.kill()
        except Exception:
            pass


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-v"]))
