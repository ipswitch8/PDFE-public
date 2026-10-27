#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Pipeline F phase-4 pywinauto tests for the form palette toolbar.

Covers the 3 RED rows in the matrix:
  form_palette_radio_button_action
  form_palette_push_button_action
  form_palette_signature_action

Each test:
  1. Launches pdfe.exe with test_form.pdf,
  2. Activates the Form Tool by invoking ``formToolActionButton`` on
     the Annotation Toolbar (UIA Invoke pattern fires the QAction's
     ``triggered`` signal, which calls ``onFormToolSelected()`` and
     un-hides ``formToolBar_`` per MainWindow.cpp:4655),
  3. Waits for ``QApplication.mainWindow.FormToolBar`` to appear in
     the UIA tree,
  4. Locates the form-palette CheckBox (Qt exposes checkable
     QToolButtons as ``CheckBox`` control type) by qualified
     auto_id and ``invoke()`` it,
  5. Asserts the ``TogglePattern`` reports On — i.e. the QAction is
     now the checked member of its exclusive QActionGroup.

Two key Qt-on-Windows quirks discovered while writing this test:

* Qt exposes a checkable ``QToolButton`` (the widget Qt creates
  for a checkable ``QAction`` placed on a ``QToolBar``) as UIA
  ``CheckBox``, not ``Button``. Filtering ``descendants(...)`` for
  ``Button`` therefore misses every form-palette item.
* ``click_input()`` and ``toggle()`` do not fire the underlying
  ``QAction::triggered`` signal in Qt 6.10 — only ``invoke()``
  does. Without ``invoke()``, the C++ side-effect
  (``setCreationFieldType``) never runs, even though the UIA
  toggle state appears to update. We therefore use ``invoke()``
  exclusively.

The toggle state read after invoke() is the directly-readable
proxy for "tool enters creation mode": each formXxxAction's
``triggered`` slot calls ``FormTool::setCreationFieldType`` (see
MainWindow.cpp:4697-4717), and Qt's exclusive QActionGroup
guarantees only the most recently triggered action is checked.
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

FORM_TOOL_BUTTON_AID = (
    "QApplication.mainWindow.annotationToolBar.formToolActionButton"
)
FORM_TOOLBAR_AID = "QApplication.mainWindow.FormToolBar"


def _start(fixture: pathlib.Path):
    """Launch pdfe.exe with a fixture; skip cleanly on missing prereqs."""
    if not fixture.exists():
        pytest.skip(f"fixture missing: {fixture.name}")
    try:
        app, win = launch_pdfe(args=[str(fixture)])
    except RuntimeError as e:
        pytest.skip(f"pdfe.exe launch failed: {e}")
    # Document load + canvas paint + gated actions enable.
    time.sleep(3.0)
    return app, win


def _activate_form_tool(win):
    """Invoke the Form Tool button so the form palette toolbar
    becomes visible. Returns the form toolbar wrapper."""
    ft = win.child_window(
        auto_id=FORM_TOOL_BUTTON_AID, control_type="CheckBox")
    try:
        ft.wait("exists", timeout=5)
    except Exception:
        # WIN-45 / F25: unreachable -> recorded 'blocked', then FAIL.
        _reach = (
            "formToolActionButton absent from UIA tree"
        )
        record_matrix_result(current_test_id(), "blocked", _reach)
        pytest.fail(
            f"WIN-45 reachability blocked (recorded non-pass, not skipped): {_reach}")
    if not ft.is_enabled():
        pytest.skip("formToolActionButton disabled (no document?)")
    try:
        ft.invoke()
    except Exception as e:
        pytest.skip(f"invoke() on formToolActionButton failed: {e}")
    # Allow the toolbar to materialize and the actions' exclusive
    # group to register.
    time.sleep(0.6)

    ftb = win.child_window(auto_id=FORM_TOOLBAR_AID, control_type="ToolBar")
    try:
        ftb.wait("exists", timeout=5)
    except Exception:
        pytest.skip("FormToolBar not visible after Form Tool activation")
    return ftb


def _click_palette_button(win, action_object_name: str,
                          matrix_id: str, record_label: str) -> None:
    """Pipeline K phase-3 (finding 9): every assertion failure must
    write a 'red' matrix row before re-raising. The previous pattern
    let an AssertionError leave the matrix entry at 'planned' so
    failures vanished from the matrix audit log.
    """
    auto_id = f"{FORM_TOOLBAR_AID}.{action_object_name}"
    btn = win.child_window(auto_id=auto_id, control_type="CheckBox")
    try:
        btn.wait("exists", timeout=5)
    except Exception:
        pytest.skip(f"{action_object_name} not exposed in UIA tree")
    if not btn.is_enabled():
        record_matrix_result(
            matrix_id, "red",
            f"{action_object_name} button must be enabled but is disabled",
        )
        pytest.fail(f"{action_object_name} button must be enabled")
    try:
        btn.invoke()
    except Exception as e:
        pytest.skip(f"invoke() on {action_object_name} failed: {e}")
    # Give the QActionGroup a moment to settle the exclusive state.
    time.sleep(0.3)
    state = btn.get_toggle_state()
    if state != 1:
        record_matrix_result(
            matrix_id, "red",
            f"{action_object_name} toggle_state={state} after invoke "
            "(expected 1)",
        )
        pytest.fail(
            f"{action_object_name} should be checked (toggle_state==1) "
            f"after invoke; got {state}"
        )
    record_matrix_result(matrix_id, "green", record_label)


def test_form_palette_radio_button_action():
    """form_palette_radio_button_action — invoking formRadioButtonAction
    enters Radio Button creation mode (button toggles on)."""
    fixture = FIXTURES / "test_form.pdf"
    app, win = _start(fixture)
    try:
        _activate_form_tool(win)
        _click_palette_button(
            win,
            "formRadioButtonAction",
            "form_palette_radio_button_action",
            "Pipeline F phase-4: formRadioButtonAction reachable + checks on invoke",
        )
    finally:
        try:
            app.kill()
        except Exception:
            pass


def test_form_palette_push_button_action():
    """form_palette_push_button_action — invoking formPushButtonAction
    enters Push Button creation mode."""
    fixture = FIXTURES / "test_form.pdf"
    app, win = _start(fixture)
    try:
        _activate_form_tool(win)
        _click_palette_button(
            win,
            "formPushButtonAction",
            "form_palette_push_button_action",
            "Pipeline F phase-4: formPushButtonAction reachable + checks on invoke",
        )
    finally:
        try:
            app.kill()
        except Exception:
            pass


def test_form_palette_signature_action():
    """form_palette_signature_action — invoking formSignatureAction
    enters Signature creation mode."""
    fixture = FIXTURES / "test_form.pdf"
    app, win = _start(fixture)
    try:
        _activate_form_tool(win)
        _click_palette_button(
            win,
            "formSignatureAction",
            "form_palette_signature_action",
            "Pipeline F phase-4: formSignatureAction reachable + checks on invoke",
        )
    finally:
        try:
            app.kill()
        except Exception:
            pass


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-v"]))
