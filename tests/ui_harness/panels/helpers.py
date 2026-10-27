# -*- coding: utf-8 -*-
"""Side-panel helpers for the PDFE UI test harness (Phase 9).

Covers the View-menu side panels: SearchPanel, BookmarkPanel, LayerPanel,
AttachmentPanel, and (where present) MeasurePanel.

All functions accept a ``PDFESession`` (from
``tests.ui_harness.core.launch``) and delegate widget lookup to the shared
``core.locate`` helpers. Lookups use the ``objectName`` of each widget
where one is set in ``src/ui/`` (these names are listed in
``tests/fixtures/ui_audit/OBJECTNAME_AUDIT.md``).

Phase-9 scope. Helpers raise ``RuntimeError`` (not ``NotImplementedError``)
on missing widgets so that test failures distinguish "panel not present"
from "function not yet implemented".
"""

from __future__ import annotations

import time

import pyautogui

from tests.ui_harness.core.canvas import wait_stable
from tests.ui_harness.core.locate import (
    by_auto_id_or_name,
    by_name,
    invoke,
)

__all__ = [
    "open_panel",
    "search_in_panel",
    "expand_bookmark",
    "toggle_layer",
    "measure_distance",
    "open_attachment",
]

# ---------------------------------------------------------------------------
# View-menu paths and panel objectNames.
# ---------------------------------------------------------------------------

_PANEL_MENU_PATHS = {
    "search":     "View->Search Panel",
    "bookmarks":  "View->Bookmarks",
    "bookmark":   "View->Bookmarks",
    "layers":     "View->Layers",
    "layer":      "View->Layers",
    "attachments":"View->Attachments",
    "attachment": "View->Attachments",
    "comments":   "View->Comments",
    "comment":    "View->Comments",
    "pages":      "View->Pages",
    "page":       "View->Pages",
}

_PANEL_OBJECT_NAMES = {
    "search":     ["searchPanel", "SearchPanel"],
    "bookmarks":  ["bookmarkPanel", "BookmarkPanel"],
    "bookmark":   ["bookmarkPanel", "BookmarkPanel"],
    "layers":     ["layerPanel", "LayerPanel"],
    "layer":      ["layerPanel", "LayerPanel"],
    "attachments":["attachmentPanel", "AttachmentPanel"],
    "attachment": ["attachmentPanel", "AttachmentPanel"],
    "comments":   ["commentPanel", "CommentPanel"],
    "comment":    ["commentPanel", "CommentPanel"],
    "pages":      ["pagePanel", "PagePanel"],
    "page":       ["pagePanel", "PagePanel"],
}

_ACTION_SETTLE = 0.4


# ---------------------------------------------------------------------------
# Public helpers
# ---------------------------------------------------------------------------

def open_panel(session, name):
    """Open a side panel by name via the View menu.

    Parameters
    ----------
    session : PDFESession
    name    : str  One of "search"/"bookmarks"/"layers"/"attachments"/
                  "comments"/"pages" (case-insensitive).

    Returns
    -------
    pywinauto WindowSpecification
        Handle to the panel widget if it can be located via UIA, otherwise
        the main window (the panel is dock-embedded and may not surface as
        a child widget on every Qt build).

    Raises
    ------
    ValueError    If *name* is not recognised.
    RuntimeError  If the View menu action cannot be invoked.
    """
    key = name.lower()
    if key not in _PANEL_MENU_PATHS:
        raise ValueError(
            f"open_panel: name={name!r} not recognised. "
            f"Valid: {sorted(set(_PANEL_MENU_PATHS))}"
        )

    window = session.window
    try:
        window.set_focus()
        # GAP-A14: UIA menu walk fallback, retained only for interactive/local
        # hosts where menu_select is reliable. On Server-2019 CI it can deadlock —
        # prefer invoke_action(window, "<panelToggleAction>") for the panel.
        window.menu_select(_PANEL_MENU_PATHS[key])
        wait_stable(_ACTION_SETTLE)
    except Exception as exc:
        raise RuntimeError(
            f"Could not open panel {name!r} via menu "
            f"({_PANEL_MENU_PATHS[key]!r}): {exc}"
        ) from exc

    # Locate the panel widget (best-effort; panels that aren't named
    # discoverably just return the window).
    for obj_name in _PANEL_OBJECT_NAMES.get(key, []):
        try:
            return by_auto_id_or_name(window, obj_name, obj_name, timeout=2)
        except Exception:
            continue
    return window


def search_in_panel(session, query):
    """Open the SearchPanel, type *query* into the search box, click Search.

    Parameters
    ----------
    session : PDFESession
    query   : str   Text to find.

    Raises
    ------
    RuntimeError  If the search edit or button cannot be located.
    """
    open_panel(session, "search")

    window = session.window
    # The search edit / button objectNames depend on SearchPanel.cpp; fall
    # back to accessible names.
    try:
        edit = by_auto_id_or_name(window, "searchEdit", "Search", timeout=5)
    except Exception as exc:
        raise RuntimeError(f"Could not locate SearchPanel edit: {exc}") from exc

    try:
        edit.set_focus()
        edit.type_keys("^a{DELETE}", with_spaces=False)
    except Exception:
        pyautogui.hotkey("ctrl", "a")
        pyautogui.press("delete")
    pyautogui.typewrite(query, interval=0.01)
    wait_stable(0.2)

    try:
        btn = by_auto_id_or_name(window, "searchButton", "Search", timeout=3)
        invoke(btn)
    except Exception:
        pyautogui.press("enter")
    wait_stable(_ACTION_SETTLE)


def expand_bookmark(session, idx):
    """Expand the *idx*-th top-level bookmark in the BookmarkPanel.

    Parameters
    ----------
    session : PDFESession
    idx     : int   Zero-based index.
    """
    open_panel(session, "bookmarks")

    window = session.window
    try:
        tree = by_auto_id_or_name(window, "bookmarkTree", "Bookmarks", timeout=5)
    except Exception as exc:
        raise RuntimeError(
            f"Could not locate BookmarkPanel tree: {exc}"
        ) from exc

    try:
        # pywinauto QTreeWidget exposes children() as a flat list.
        items = tree.children()
        if not items:
            return
        target = items[min(idx, len(items) - 1)]
        try:
            target.expand()
        except Exception:
            target.click_input(double=True)
    except Exception as exc:
        raise RuntimeError(
            f"Could not expand bookmark at idx={idx}: {exc}"
        ) from exc


def toggle_layer(session, idx):
    """Toggle the visibility checkbox on the *idx*-th layer in LayerPanel.

    Parameters
    ----------
    session : PDFESession
    idx     : int   Zero-based index.
    """
    open_panel(session, "layers")

    window = session.window
    try:
        tree = by_auto_id_or_name(window, "layerTree", "Layers", timeout=5)
    except Exception as exc:
        raise RuntimeError(
            f"Could not locate LayerPanel tree: {exc}"
        ) from exc

    try:
        items = tree.children()
        if not items:
            return
        target = items[min(idx, len(items) - 1)]
        try:
            target.click_input(coords=(8, 8))  # checkbox area
        except Exception:
            try:
                target.toggle()
            except Exception:
                pass
    except Exception as exc:
        raise RuntimeError(
            f"Could not toggle layer at idx={idx}: {exc}"
        ) from exc


def measure_distance(session, p1, p2):
    """Activate the measure tool and click two points to measure distance.

    Parameters
    ----------
    session : PDFESession
    p1, p2  : tuple[int, int]  Screen coordinates.

    Returns
    -------
    str | None
        Status-bar reading after the second click, or None when the
        status bar text cannot be read.
    """
    window = session.window
    try:
        window.set_focus()
        # GAP-A14: UIA menu walk fallback, retained only for interactive/local
        # hosts where menu_select is reliable. On Server-2019 CI it can deadlock —
        # prefer invoke_action(window, "measureToolAction").
        window.menu_select("Tools->Measure")
        wait_stable(_ACTION_SETTLE)
    except Exception:
        # Measure tool may not be exposed on the menu in every build.
        return None

    pyautogui.click(p1[0], p1[1])
    wait_stable(0.1)
    pyautogui.click(p2[0], p2[1])
    wait_stable(_ACTION_SETTLE)

    try:
        sb = by_auto_id_or_name(window, "statusBar", "Status", timeout=2)
        return sb.window_text()
    except Exception:
        return None


def open_attachment(session, idx):
    """Open the *idx*-th attachment in the AttachmentPanel.

    Parameters
    ----------
    session : PDFESession
    idx     : int   Zero-based index.
    """
    open_panel(session, "attachments")

    window = session.window
    try:
        list_widget = by_auto_id_or_name(
            window, "attachmentList", "Attachments", timeout=5
        )
    except Exception as exc:
        raise RuntimeError(
            f"Could not locate AttachmentPanel list: {exc}"
        ) from exc

    try:
        items = list_widget.children()
        if not items:
            return
        target = items[min(idx, len(items) - 1)]
        try:
            target.click_input(double=True)
        except Exception:
            target.invoke()
    except Exception as exc:
        raise RuntimeError(
            f"Could not open attachment at idx={idx}: {exc}"
        ) from exc
