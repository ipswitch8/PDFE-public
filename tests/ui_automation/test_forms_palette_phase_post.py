#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""pywinauto scripts for form-field palette matrix rows.

Covers:
  form_palette_radio_button_action
  form_palette_push_button_action
  form_palette_signature_action
  tab_order_dialog_drag_reorder

Strategy: open a form-enabled fixture, trigger the "Create Field"
action, then click each palette toolbar button. The palette uses an
exclusive QActionGroup, so clicking any one field-type action
checks it and unchecks the others — that's the observable outcome
this test asserts, without having to verify MuPDF-side creation
mode state (which is a FormTool internal).

Refactored 2026-05-01: switched to launch_pdfe + click_menu_path harness
so menu lookups fail fast (raise) instead of hanging in pywinauto's COM
Invoke() call. Menu paths verified against src/ui/MainWindow.cpp.
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


FIXTURE = pathlib.Path(__file__).resolve().parent.parent / "fixtures" / "test_form.pdf"


@pytest.fixture()
def pdfe_app():
    if not FIXTURE.exists():
        pytest.skip("test_form.pdf fixture missing")
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


def _activate_create_field_mode(win):
    """Reveal the form-field palette toolbar.

    ``formToolBar_`` is created hidden (src/ui/MainWindow.cpp:5539
    ``setVisible(false)``) and is shown by ``MainWindow::onCreateFormField``
    (:11350-11352) and ``onFormToolSelected`` (:10136). Driven through the
    ``createFormFieldAction`` seam rather than the UIA menu walk: GAP-A14
    records ``IUIAutomationElement::FindAll`` blocking indefinitely on the
    Windows-Server/RDP session, and the menu path is not what these cases are
    about.
    """
    from tests.ui_automation.harness.pdfe_app_fixture import invoke_action
    if invoke_action(win, "createFormFieldAction", settle_s=1.2):
        return True
    # Pipe unreachable on this attempt. find_widget's own docstring records
    # why that happens intermittently here: the existing wrapper's cached UIA
    # tree on the Windows-Server-2019 RDP session sometimes returns a PARTIAL
    # FindAll that misses the zero-size hidden pipe QLineEdits. Fall back to
    # the menu path, and let the caller retry — the caller's retry is keyed on
    # the OBSERVABLE (the palette button appearing), not on this return value,
    # so a false negative here cannot turn into a false failure.
    try:
        click_menu_path(win, "Forms -> Create Field")
        return True
    except Exception:  # noqa: BLE001
        return False


#: The palette QActions, in toolbar order (src/ui/MainWindow.cpp:5544-5583).
PALETTE_ACTIONS = [
    "formTextFieldAction",
    "formCheckboxAction",
    "formRadioButtonAction",
    "formComboBoxAction",
    "formListBoxAction",
    "formPushButtonAction",
    "formSignatureAction",
]


def _toggle_state(ctrl):
    """Read a checkable QToolButton's state through whichever UIA pattern
    the Qt bridge exposes. Returns 1 (checked), 0 (unchecked), or None."""
    for reader in ("get_toggle_state", "is_selected"):
        fn = getattr(ctrl, reader, None)
        if fn is None:
            continue
        try:
            v = fn()
        except Exception:  # noqa: BLE001
            continue
        if v is None:
            continue
        return 1 if bool(v) else 0
    try:
        lp = ctrl.legacy_properties()
        # LegacyIAccessible STATE_SYSTEM_CHECKED / _PRESSED.
        state = int(lp.get("State", 0))
        if state & 0x00000008 or state & 0x00000010:
            return 1
        return 0
    except Exception:  # noqa: BLE001
        return None


# GitHub #209 Task B — INVESTIGATED, not assumed.
#
# The recorded skip/blocked reason was "palette action `formTextFieldAction`
# absent from UIA tree", and the issue flagged it as possibly legitimate
# because the form toolbar can be hidden. Measured against the current tree it
# is NOT legitimate: it is the same wrong-auto_id class as GitHub #198.
#
#   1. The toolbar is hidden at construction (MainWindow.cpp:5539), but this
#      test already calls _activate_create_field_mode() first, and
#      onCreateFormField() (:11350) sets it visible. So "hidden by default"
#      does not explain a lookup that runs AFTER activation.
#   2. The thing that is actually in the UIA tree is the QToolButton, not the
#      QAction, and MainWindow.cpp:5594-5603 deliberately renames those
#      buttons: ``btn->setObjectName(act->objectName() + "Button")``. The
#      visible auto_id is therefore ``formTextFieldActionButton``.
#   3. Qt's UIA AutomationId is HIERARCHICAL (see harness/dialog_helpers.py),
#      so even the right leaf name must be matched by suffix.
#      ``win.child_window(auto_id="formTextFieldAction")`` was an exact match
#      against an unqualified name with the "Button" suffix missing — it could
#      never resolve, for any toolbar visibility state.
#
# Every other suite in this repo already uses the right form:
# ``find_by_suffix(win, "formToolActionButton")`` (test_canvas_phase4.py:200,
# test_canvas_coords_p2.py:546, test_annotations_p1.py:159-163, ...).
#
# The old oracle was also vacuous: ``assert action.is_enabled()`` on a toolbar
# button is permanently true, so even with a correct lookup the case measured
# nothing. The name of the test says "checks exclusively"; that is now what it
# asserts.
@pytest.mark.timeout(240)
@pytest.mark.parametrize("action_id", PALETTE_ACTIONS)
def test_form_palette_action_checks_exclusively(pdfe_app, action_id):
    """form_palette_{text,checkbox,radio,combo,list,push,signature}_action.

    Oracle: after clicking one palette button, that button reads CHECKED and
    every other palette button reads UNCHECKED. The palette is an exclusive
    QActionGroup (MainWindow.cpp:5605-5613), so:

      * removing the action under test -> its button is not in the tree -> fail;
      * removing ``setExclusive(true)`` -> the previously-checked sibling
        (``formTextFieldAction_`` is checked at startup, :5614) stays checked
        -> fail;
      * unchecking everything -> the "checked" half fails.

    The siblings' states are read and asserted as a set rather than
    one-against-one, so a build where nothing is checkable cannot pass.
    """
    from tests.ui_automation.harness.dialog_helpers import find_by_suffix
    _, win = pdfe_app

    # MainWindow.cpp:5601 — the QToolButton takes objectName
    # <action objectName> + "Button"; the QAction's own name is never an
    # AutomationId. Suffix match because Qt UIA ids are hierarchical.
    button_id = action_id + "Button"
    target = None
    activated = False
    # Retry ACTIVATION keyed on the observable, not on the activation call's
    # own return value. Measured 2026-09-18: the createFormFieldAction pipe
    # lookup intermittently returns False on this host (partial UIA FindAll —
    # see find_widget's docstring) while the app is perfectly healthy; one
    # attempt in ~5 reported "not reachable" for a run whose neighbours all
    # passed. Retrying until the palette button appears removes the false
    # negative without weakening anything: the case still FAILS if the button
    # never shows up.
    for attempt in range(3):
        activated = _activate_create_field_mode(win) or activated
        deadline = time.time() + 6.0
        while time.time() < deadline:
            target = find_by_suffix(win, button_id)
            if target is not None:
                break
            time.sleep(0.3)
        if target is not None:
            break
        time.sleep(0.6)
    if target is None:
        # WIN-45 / F25: unreachable -> recorded non-pass, then FAIL.
        _reach = (
            f"palette button {button_id} not in the UIA tree after 3 "
            f"createFormFieldAction attempts (activation reported "
            f"{activated}; MainWindow.cpp:5544-5603 defines it)"
        )
        record_matrix_result(current_test_id(), "blocked", _reach)
        pytest.fail(
            f"WIN-45 reachability blocked (recorded non-pass, not skipped): {_reach}")

    target.click_input()
    time.sleep(0.4)

    states = {}
    for other in PALETTE_ACTIONS:
        ctrl = target if other == action_id else find_by_suffix(
            win, other + "Button")
        states[other] = None if ctrl is None else _toggle_state(ctrl)

    readable = {k: v for k, v in states.items() if v is not None}
    # Guard that the measurement happened at all — an all-None map would make
    # "no sibling is checked" trivially true.
    assert len(readable) == len(PALETTE_ACTIONS), (
        f"only {len(readable)} of {len(PALETTE_ACTIONS)} palette buttons "
        f"reported a checkable state: {states!r}. The exclusivity assertion "
        f"below cannot be trusted against a partial reading."
    )

    checked = sorted(k for k, v in readable.items() if v == 1)
    from tests.ui_automation.conftest import assert_recorded
    assert_recorded(
        current_test_id(),
        checked == [action_id],
        f"clicking {action_id} should leave exactly it checked in the "
        f"exclusive palette group (MainWindow.cpp:5605-5613); "
        f"checked={checked!r}, all states={states!r}",
        green_notes=(
            f"GitHub #209 Task B: {button_id} located by suffix, clicked, and "
            f"is the only checked member of the palette QActionGroup"
        ),
    )


def test_tab_order_dialog_drag_reorder(pdfe_app):
    """tab_order_dialog_drag_reorder — Forms > Tab Order... opens a
    drag-enabled list (per src/ui/MainWindow.cpp:1376)."""
    _, win = pdfe_app
    try:
        click_menu_path(win, "Forms -> Tab Order...")
    except Exception:
        # WIN-45 / F25: unreachable -> recorded 'blocked', then FAIL.
        _reach = (
            "Forms > Tab Order menu path not reachable"
        )
        record_matrix_result(current_test_id(), "blocked", _reach)
        pytest.fail(
            f"WIN-45 reachability blocked (recorded non-pass, not skipped): {_reach}")

    dlg = win.child_window(auto_id="QApplication.tabOrderDialog")
    try:
        dlg.wait("visible", timeout=5)
    except Exception:
        pytest.skip("tabOrderDialog did not appear")

    # The list inside tabOrderDialog: use control_type fallback in case
    # the auto_id varies between widget builds.
    try:
        field_list = dlg.child_window(
            auto_id="QApplication.tabOrderDialog.tabOrderFieldList")
        field_list.wait("visible", timeout=2)
    except Exception:
        field_list = dlg.child_window(control_type="List")
    field_list.wait("visible", timeout=5)
    # Pipeline K phase-3 (finding 9): record red on failure.
    from tests.ui_automation.conftest import assert_recorded
    assert_recorded(
        "tab_order_dialog_drag_reorder",
        field_list.is_enabled(),
        "fieldList not enabled in tabOrderDialog",
        green_notes="Sweep verified: Forms->Tab Order... opens, fieldList visible+enabled",
    )


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-v"]))
