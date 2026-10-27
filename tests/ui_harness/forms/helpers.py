# -*- coding: utf-8 -*-
"""Forms subsystem helpers for the PDFE UI test harness.

Provides high-level operations for the forms tool: activating the Form Tool
QAction, tabbing between fields, filling text fields, toggling checkboxes,
selecting radio buttons and combo/list-box items, firing JS keystroke events,
and importing/exporting form data via XFDF.  All functions accept a
``PDFESession`` instance (from ``tests.ui_harness.core.launch``) as their
first argument and delegate low-level widget location, canvas clicking, and
screenshot capture to the shared ``core`` helpers.

Discovered objectNames (from src/ui/MainWindow.cpp forms-action setup)
-----------------------------------------------------------------------
- formToolAction         — QAction that activates the Form Tool (annotation toolbar)
- exportFormDataAction   — QAction for Forms > Export Form Data...
- importFormDataAction   — QAction for Forms > Import Form Data...
- resetFormAction        — QAction for Forms > Reset Form
- flattenFormAction      — QAction for Forms > Flatten Form
- formsMenu              — QMenu "Forms" in the menu bar

Form field widgets are interacted with primarily via canvas-level clicks and
keyboard input, since PDF form fields are rendered by PDFE into PdfCanvas and
may not surface individual control entries in the UIA tree.  When a field has
an accessibleName set (e.g. from the PDF field's /T entry), ``by_name`` will
locate it; otherwise the helper falls back to Tab-navigation and
pyautogui-driven keyboard input.
"""

import os
import time

import pyautogui
import pywinauto.timings

from tests.ui_harness.core.canvas import (
    canvas_rect,
    click_at,
    page_to_screen,
    wait_stable,
)
from tests.ui_harness.core.locate import (
    by_auto_id,
    by_auto_id_or_name,
    by_name,
    invoke,
    type_into,
)
from tests.ui_harness.core.screenshot import capture_canvas

__all__ = [
    "activate_form_tool",
    "tab_to_field",
    "fill_text_field",
    "toggle_checkbox",
    "select_radio",
    "choose_combo",
    "select_list_items",
    "trigger_js_keystroke",
    "export_xfdf",
    "import_xfdf",
]

# ---------------------------------------------------------------------------
# Internal constants — objectNames discovered from src/ui/MainWindow.cpp
# ---------------------------------------------------------------------------

_FORM_TOOL_ACTION_ID = "formToolAction"
_FORM_TOOL_ACTION_NAME = "Form Tool"

_EXPORT_FORM_DATA_ACTION_ID = "exportFormDataAction"
_EXPORT_FORM_DATA_ACTION_NAME = "Export Form Data"

_IMPORT_FORM_DATA_ACTION_ID = "importFormDataAction"
_IMPORT_FORM_DATA_ACTION_NAME = "Import Form Data"

# Menu path used as a reliable fallback for import/export when the QAction
# UIA node is not directly reachable (Qt may not expose QAction as a UIA node
# outside of the menu tree).
_FORMS_MENU_EXPORT_PATH = "Forms->Export Form Data..."
_FORMS_MENU_IMPORT_PATH = "Forms->Import Form Data..."

_CANVAS_ACCESSIBLE_NAME = "PDF Canvas"

# Seconds to wait for dialogs, canvas readiness, etc.
_DIALOG_TIMEOUT = 15
_CANVAS_TIMEOUT = 20
_FIELD_FOCUS_TIMEOUT = 10

# Maximum number of Tab presses when searching for a named field.
_TAB_SEARCH_MAX = 50

# Pause between Tab presses when cycling to a named field.
_TAB_INTERVAL = 0.08


# ---------------------------------------------------------------------------
# Public helpers
# ---------------------------------------------------------------------------

def activate_form_tool(session):
    """Click the Form Tool QAction to activate the forms interaction mode.

    Locates the action by objectName ``formToolAction`` (UIA AutomationId),
    falling back to accessibleName ``"Form Tool"``.  After invocation the
    Forms toolbar (FormToolBar) becomes visible.

    Parameters
    ----------
    session : PDFESession

    Raises
    ------
    RuntimeError
        If the form tool action cannot be located or invoked.
    """
    window = session.window

    try:
        ctrl = by_auto_id_or_name(
            window, _FORM_TOOL_ACTION_ID, _FORM_TOOL_ACTION_NAME, timeout=10
        )
    except Exception as exc:
        raise RuntimeError(
            f"Could not locate the Form Tool action "
            f"(auto_id={_FORM_TOOL_ACTION_ID!r}, name={_FORM_TOOL_ACTION_NAME!r}): {exc}"
        ) from exc

    invoke(ctrl)
    wait_stable(0.25)


def tab_to_field(session, name=None):
    """Press Tab to advance focus to the next form field.

    If *name* is given, Tab is pressed repeatedly (up to ``_TAB_SEARCH_MAX``
    times) until the focused control's UIA Name matches *name*.  The canvas
    must have keyboard focus before the first Tab is sent; this function
    ensures focus by clicking the canvas centre if needed.

    Parameters
    ----------
    session : PDFESession
    name    : str or None
        If None, a single Tab is sent (move to next field).
        If a string, Tab is pressed until a control with that accessible name
        gains focus, or until ``_TAB_SEARCH_MAX`` attempts are exhausted.

    Raises
    ------
    RuntimeError
        If *name* is given but no matching field gains focus within the
        attempt limit.
    """
    window = session.window

    # Ensure keyboard focus is inside the PDFE window / canvas.
    _ensure_canvas_focus(session)

    if name is None:
        pyautogui.press("tab")
        wait_stable(_TAB_INTERVAL)
        return

    # Tab-cycle until the focused element's name matches.
    for attempt in range(_TAB_SEARCH_MAX):
        pyautogui.press("tab")
        time.sleep(_TAB_INTERVAL)

        # Check which control has focus now.
        try:
            focused = window.get_focus()
            if focused is not None:
                try:
                    focused_name = focused.window_text() or ""
                except Exception:
                    focused_name = ""
                if name.lower() in focused_name.lower() or focused_name.lower() in name.lower():
                    wait_stable(0.1)
                    return
        except Exception:
            pass

        # Also try by accessible name in the UIA tree as a secondary check.
        try:
            ctrl = by_name(window, name, timeout=0.5)
            # If the control exists and reports focus, we're done.
            try:
                if ctrl.has_keyboard_focus():
                    wait_stable(0.1)
                    return
            except Exception:
                pass
        except Exception:
            pass

    raise RuntimeError(
        f"tab_to_field: field with accessible name {name!r} did not receive "
        f"focus after {_TAB_SEARCH_MAX} Tab presses."
    )


def fill_text_field(session, name, value):
    """Navigate to a named text field and type *value* into it.

    Strategy:
    1. Try to locate the field by accessible name via the UIA tree and click
       it directly.
    2. If the UIA lookup fails, use ``tab_to_field`` to navigate by Tab until
       the field is focused.
    3. Clear any existing text (Ctrl+A, Delete) then type *value*.

    Parameters
    ----------
    session : PDFESession
    name    : str   Accessible name of the target text field (PDF /T entry).
    value   : str   Text to type into the field.

    Raises
    ------
    RuntimeError
        If the field cannot be located or focused.
    """
    window = session.window
    field = _locate_field(session, name)

    if field is not None:
        # Direct UIA path: click to focus, clear, type.
        try:
            field.set_focus()
            field.type_keys("^a{DELETE}", with_spaces=False)
            wait_stable(0.05)
            pyautogui.typewrite(value, interval=0.02)
            wait_stable(0.1)
            return
        except Exception:
            pass

    # Fallback: tab to the field and type.
    tab_to_field(session, name)
    pyautogui.hotkey("ctrl", "a")
    pyautogui.press("delete")
    pyautogui.typewrite(value, interval=0.02)
    wait_stable(0.1)


def toggle_checkbox(session, name):
    """Toggle the named checkbox field by clicking it or pressing Space.

    First attempts a direct UIA click on the field located by accessible name.
    Falls back to Tab-navigating to the field and pressing Space.

    Parameters
    ----------
    session : PDFESession
    name    : str  Accessible name of the checkbox field.

    Raises
    ------
    RuntimeError
        If the checkbox field cannot be located.
    """
    field = _locate_field(session, name)

    if field is not None:
        try:
            field.set_focus()
            field.click_input()
            wait_stable(0.15)
            return
        except Exception:
            pass

    # Fallback: tab to the field and toggle with Space.
    tab_to_field(session, name)
    pyautogui.press("space")
    wait_stable(0.15)


def select_radio(session, name):
    """Click the named radio button field.

    Locates the field by accessible name via UIA; falls back to Tab-navigation
    plus Space if the UIA path fails.

    Parameters
    ----------
    session : PDFESession
    name    : str  Accessible name of the radio button field.

    Raises
    ------
    RuntimeError
        If the field cannot be reached.
    """
    field = _locate_field(session, name)

    if field is not None:
        try:
            field.set_focus()
            field.click_input()
            wait_stable(0.15)
            return
        except Exception:
            pass

    # Fallback: tab then space.
    tab_to_field(session, name)
    pyautogui.press("space")
    wait_stable(0.15)


def choose_combo(session, name, value):
    """Open the named combo-box field and select the option matching *value*.

    Strategy:
    1. Locate the field by accessible name via UIA.
    2. Click to open the dropdown (or send Alt+Down if it is a combo-box
       control type).
    3. Type the value and press Enter; this works for both editable and
       non-editable combo boxes in Qt form widgets.

    Parameters
    ----------
    session : PDFESession
    name    : str  Accessible name of the combo-box field.
    value   : str  The option text to select.

    Raises
    ------
    RuntimeError
        If the combo-box field cannot be located.
    """
    window = session.window
    field = _locate_field(session, name)

    if field is not None:
        try:
            field.set_focus()
            # Try UIA SelectionItem / List pattern first.
            try:
                field.select(value)
                wait_stable(0.2)
                return
            except Exception:
                pass
            # Open dropdown via Alt+Down and type the value.
            field.type_keys("%{DOWN}", with_spaces=False)
            wait_stable(0.2)
            pyautogui.typewrite(value, interval=0.02)
            pyautogui.press("enter")
            wait_stable(0.15)
            return
        except Exception:
            pass

    # Fallback: tab to field, open dropdown, type value.
    tab_to_field(session, name)
    pyautogui.hotkey("alt", "down")
    wait_stable(0.2)
    pyautogui.typewrite(value, interval=0.02)
    pyautogui.press("enter")
    wait_stable(0.15)


def select_list_items(session, name, indices):
    """Select items at the given 0-based indices in a list-box field.

    For single-item selection, clicks the item at the specified index.  For
    multi-item selection, holds Ctrl while clicking each subsequent item.

    Because PDF list-box fields rendered inside PdfCanvas may not expose
    individual list items in the UIA tree, this function uses arrow-key
    navigation after focusing the field as the primary strategy, supplemented
    by a UIA list-item click attempt.

    Parameters
    ----------
    session : PDFESession
    name    : str        Accessible name of the list-box field.
    indices : list[int]  Zero-based item indices to select.  Must not be empty.

    Raises
    ------
    ValueError
        If *indices* is empty.
    RuntimeError
        If the list-box field cannot be focused.
    """
    if not indices:
        raise ValueError("select_list_items: indices must not be empty.")

    field = _locate_field(session, name)

    if field is not None:
        try:
            field.set_focus()
            # Try UIA list item selection for each index.
            try:
                items = field.children()
                for i, idx in enumerate(sorted(indices)):
                    if idx < len(items):
                        if i == 0:
                            items[idx].click_input()
                        else:
                            # Ctrl+click for additional selections.
                            pyautogui.keyDown("ctrl")
                            items[idx].click_input()
                            pyautogui.keyUp("ctrl")
                wait_stable(0.15)
                return
            except Exception:
                pass
        except Exception:
            pass

    # Fallback: navigate via Tab then arrow keys.
    tab_to_field(session, name)

    # Move to the top of the list first (Ctrl+Home), then arrow down to each index.
    pyautogui.hotkey("ctrl", "home")
    wait_stable(0.05)

    sorted_indices = sorted(indices)
    for i, target_idx in enumerate(sorted_indices):
        # Navigate from the previous index (or 0) to the current one.
        start = sorted_indices[i - 1] if i > 0 else 0
        steps = target_idx - start
        for _ in range(steps):
            pyautogui.press("down")
            time.sleep(0.03)

        if i > 0:
            # Hold Ctrl for subsequent items to add to selection.
            pyautogui.hotkey("ctrl", "space")

    wait_stable(0.15)


def trigger_js_keystroke(session, name, key):
    """Focus the named field and type one character to fire its Keystroke event.

    PDF form fields can have JavaScript keystroke handlers that validate or
    transform input as each character is typed.  This helper focuses the named
    field and sends a single key press so the JS engine fires the event.

    Parameters
    ----------
    session : PDFESession
    name    : str  Accessible name of the field.
    key     : str  Single character to type (e.g. ``"5"`` or ``"a"``).

    Raises
    ------
    RuntimeError
        If the field cannot be focused.
    """
    field = _locate_field(session, name)

    if field is not None:
        try:
            field.set_focus()
            pyautogui.typewrite(key, interval=0.02)
            wait_stable(0.1)
            return
        except Exception:
            pass

    # Fallback: tab to field, type the key.
    tab_to_field(session, name)
    pyautogui.typewrite(key, interval=0.02)
    wait_stable(0.1)


def export_xfdf(session, path):
    """Invoke Forms > Export Form Data..., type *path* in the save dialog, accept.

    Parameters
    ----------
    session : PDFESession
    path    : str  Absolute path for the output .xfdf file.

    Raises
    ------
    TimeoutError
        If the save dialog does not appear within ``_DIALOG_TIMEOUT`` seconds.
    RuntimeError
        If the export action cannot be located or invoked.
    """
    path = os.path.abspath(path)
    window = session.window

    # Try invoking the QAction via UIA first; fall back to menu navigation.
    _invoke_action_or_menu(
        session,
        action_id=_EXPORT_FORM_DATA_ACTION_ID,
        action_name=_EXPORT_FORM_DATA_ACTION_NAME,
        menu_path=_FORMS_MENU_EXPORT_PATH,
    )

    # Wait for the save-file dialog.
    _wait_for_dialog(session, title_re=r"Export|Save", timeout=_DIALOG_TIMEOUT)

    # Clear the filename field and type the destination path.
    pyautogui.hotkey("ctrl", "a")
    time.sleep(0.05)
    pyautogui.typewrite(path, interval=0.02)
    pyautogui.press("enter")

    wait_stable(0.5)


def import_xfdf(session, path):
    """Invoke Forms > Import Form Data..., type *path* in the open dialog, accept.

    Parameters
    ----------
    session : PDFESession
    path    : str  Absolute path to the .xfdf file to import.

    Raises
    ------
    TimeoutError
        If the open dialog does not appear within ``_DIALOG_TIMEOUT`` seconds.
    RuntimeError
        If the import action cannot be located or invoked.
    """
    path = os.path.abspath(path)

    # Try invoking the QAction via UIA first; fall back to menu navigation.
    _invoke_action_or_menu(
        session,
        action_id=_IMPORT_FORM_DATA_ACTION_ID,
        action_name=_IMPORT_FORM_DATA_ACTION_NAME,
        menu_path=_FORMS_MENU_IMPORT_PATH,
    )

    # Wait for the open-file dialog.
    _wait_for_dialog(session, title_re=r"Import|Open", timeout=_DIALOG_TIMEOUT)

    # Clear the filename field and type the source path.
    pyautogui.hotkey("ctrl", "a")
    time.sleep(0.05)
    pyautogui.typewrite(path, interval=0.02)
    pyautogui.press("enter")

    wait_stable(0.5)


# ---------------------------------------------------------------------------
# Internal helpers
# ---------------------------------------------------------------------------

def _locate_field(session, name):
    """Try to find a form field widget in the UIA tree by accessible name.

    Returns the pywinauto control wrapper if found, or None if the field is
    not reachable via UIA (e.g. it is rendered purely on the canvas).

    Parameters
    ----------
    session : PDFESession
    name    : str  The field's accessible name.

    Returns
    -------
    pywinauto control or None
    """
    window = session.window
    try:
        ctrl = by_name(window, name, timeout=3)
        return ctrl
    except Exception:
        return None


def _ensure_canvas_focus(session):
    """Ensure keyboard focus is inside the PDFE canvas or main window.

    Tries to locate the canvas by accessibleName and set focus on it.
    Falls back to clicking the canvas centre via PyAutoGUI if the UIA
    lookup fails.
    """
    window = session.window
    try:
        canvas_ctrl = by_name(window, _CANVAS_ACCESSIBLE_NAME, timeout=5)
        canvas_ctrl.set_focus()
        return
    except Exception:
        pass

    # Fallback: click the canvas centre.
    try:
        r = canvas_rect(session.app)
        cx = (r.left + r.right) // 2
        cy = (r.top + r.bottom) // 2
        click_at(cx, cy)
    except Exception:
        # Last resort: just focus the main window.
        try:
            window.set_focus()
        except Exception:
            pass


def _invoke_action_or_menu(session, action_id, action_name, menu_path):
    """Invoke a QAction via UIA auto_id lookup, falling back to menu navigation.

    QActions in Qt are not always exposed as standalone UIA nodes outside of
    their menu hierarchy.  This helper tries both strategies.

    Parameters
    ----------
    session     : PDFESession
    action_id   : str  objectName of the QAction.
    action_name : str  accessibleName fallback.
    menu_path   : str  Pywinauto menu_select path (e.g. "Forms->Export Form Data...").

    Raises
    ------
    RuntimeError
        If both strategies fail.
    """
    window = session.window

    # Strategy 1: direct UIA node lookup.
    try:
        ctrl = by_auto_id_or_name(window, action_id, action_name, timeout=5)
        invoke(ctrl)
        return
    except Exception:
        pass

    # Strategy 2: menu navigation.
    try:
        window.set_focus()
        # GAP-A14: UIA menu walk fallback, retained only for interactive/local
        # hosts where menu_select is reliable. On Server-2019 CI it can deadlock —
        # prefer invoke_action(window, action_id) which fires the QAction via the
        # testToolActivatePipe seam instead of enumerating the UIA menu tree.
        window.menu_select(menu_path)
        return
    except Exception as exc:
        raise RuntimeError(
            f"Could not invoke action "
            f"(auto_id={action_id!r}, name={action_name!r}, menu={menu_path!r}): {exc}"
        ) from exc


def _wait_for_dialog(session, title_re, timeout=_DIALOG_TIMEOUT):
    """Block until a top-level dialog whose title matches *title_re* appears.

    Parameters
    ----------
    session  : PDFESession
    title_re : str  Regular expression matched against the dialog title.
    timeout  : int  Seconds to wait.

    Returns
    -------
    pywinauto WindowSpecification for the dialog.

    Raises
    ------
    TimeoutError
        If no matching dialog appears within *timeout* seconds.
    """
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            dlg = session.app.window(title_re=title_re)
            dlg.wait("visible", timeout=1)
            return dlg
        except Exception:
            time.sleep(0.3)

    raise TimeoutError(
        f"Dialog matching title_re={title_re!r} did not appear within {timeout}s."
    )
