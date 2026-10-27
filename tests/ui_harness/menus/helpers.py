# -*- coding: utf-8 -*-
"""Menu / toolbar / status-bar / accessibility helpers (Phase 9).

Provides the cross-cutting actions Phase-9 tests need to drive the menu
bar, toolbar buttons, status bar, and tab-traversal of the main window.
All functions accept a ``PDFESession`` from
``tests.ui_harness.core.launch``.
"""

from __future__ import annotations

import pyautogui

from tests.ui_harness.core.canvas import wait_stable
from tests.ui_harness.core.locate import (
    by_auto_id_or_name,
    by_name,
    invoke,
)

__all__ = [
    "invoke_menu",
    "invoke_toolbar",
    "check_status_bar",
    "tab_through_window",
    "get_accessible_name",
]

_ACTION_SETTLE = 0.3


def invoke_menu(session, path):
    """Invoke a menu action by ``"Top->Sub->Action"`` path.

    Parameters
    ----------
    session : PDFESession
    path    : str  Menu path with ``->`` separators (e.g.
              ``"File->Open"``, ``"Edit->Preferences..."``).

    Raises
    ------
    RuntimeError  If the path cannot be resolved.
    """
    window = session.window
    try:
        window.set_focus()
        # GAP-A14: menu_select drives a UIA menu-bar walk that can deadlock on
        # Server-2019 CI. Here it is INTENTIONAL and not replaced — exercising the
        # menu bar IS the test target for this module, so invoke_action() (the
        # pipe seam) would bypass exactly what these tests mean to cover. These
        # menu-navigation tests are skipped/xfail on Server-2019 CI accordingly.
        window.menu_select(path)
        wait_stable(_ACTION_SETTLE)
    except Exception as exc:
        raise RuntimeError(
            f"Could not invoke menu path {path!r}: {exc}"
        ) from exc


def invoke_toolbar(session, name):
    """Click a toolbar button by objectName or accessibleName.

    Parameters
    ----------
    session : PDFESession
    name    : str  ``objectName`` (preferred) or ``accessibleName`` of
              the toolbar QAction or QToolButton.

    Raises
    ------
    RuntimeError  If the toolbar item cannot be located or invoked.
    """
    window = session.window
    try:
        ctrl = by_auto_id_or_name(window, name, name, timeout=5)
    except Exception as exc:
        raise RuntimeError(
            f"Could not locate toolbar item {name!r}: {exc}"
        ) from exc
    try:
        invoke(ctrl)
        wait_stable(_ACTION_SETTLE)
    except Exception as exc:
        raise RuntimeError(
            f"Could not invoke toolbar item {name!r}: {exc}"
        ) from exc


def check_status_bar(session, text):
    """Return True when the status bar contains *text* substring.

    Reads the status bar text best-effort across pywinauto strategies.
    Returns False when the status bar cannot be located rather than
    raising — Phase-9 tests treat this as a soft assertion.
    """
    window = session.window
    candidates = [
        ("statusBar", "Status"),
        ("pageLabel", "Page"),
        ("zoomLabel", "Zoom"),
        ("fileNameLabel", "File"),
    ]
    for obj_name, acc_name in candidates:
        try:
            ctrl = by_auto_id_or_name(window, obj_name, acc_name, timeout=1)
            content = ctrl.window_text() or ""
            if text in content:
                return True
        except Exception:
            continue
    return False


def tab_through_window(session, presses=20):
    """Press Tab *presses* times and collect the focused-widget names.

    Returns
    -------
    list[str]   Accessible names (or window text) of the focused widget
                after each Tab press. Empty entries are filtered out.
    """
    window = session.window
    window.set_focus()

    names = []
    for _ in range(presses):
        pyautogui.press("tab")
        wait_stable(0.05)
        try:
            focused = window.element_info.element.GetFocus()
            name = (focused.CurrentName or "").strip()
        except Exception:
            try:
                name = window.window_text() or ""
            except Exception:
                name = ""
        if name:
            names.append(name)
    return names


def get_accessible_name(session, object_name):
    """Return the accessible name for the widget identified by *object_name*.

    Returns the empty string when the widget cannot be located. This is
    intentionally non-fatal so that screen-reader audits can iterate
    through expected widgets and surface the gaps.
    """
    window = session.window
    try:
        ctrl = by_auto_id_or_name(window, object_name, object_name, timeout=2)
    except Exception:
        return ""
    try:
        return (ctrl.element_info.name or "").strip()
    except Exception:
        try:
            return (ctrl.window_text() or "").strip()
        except Exception:
            return ""
