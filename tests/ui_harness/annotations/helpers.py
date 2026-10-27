# -*- coding: utf-8 -*-
"""Annotations subsystem helpers for the PDFE UI test harness.

Provides high-level operations for annotation tools: activating toolbar
actions, placing sticky notes, drawing ink strokes and shapes, applying text
markup, adjusting annotation color, and toggling the Comment and Annotation
Properties dock panels.  All functions accept a ``PDFESession`` instance (from
``tests.ui_harness.core.launch``) as their first argument and delegate
low-level widget location, canvas operations, and screenshot capture to the
shared ``core`` helpers.

Discovered objectNames (from src/ui/MainWindow.cpp createAnnotationToolBar
and dock-panel setup, and src/ui/AnnotationPropertiesWidget.cpp)
---------------------------------------------------------------------------
Annotation toolbar QActions
  selectToolAction        — Select / interact with annotations
  stickyNoteToolAction    — Sticky note
  highlightToolAction     — Highlight text
  underlineToolAction     — Underline text
  strikeOutToolAction     — Strike-out text
  inkToolAction           — Freehand ink
  freeTextToolAction      — Text-box (Free Text annotation)
  rectangleToolAction     — Rectangle shape
  circleToolAction        — Circle shape
  lineToolAction          — Line shape
  stampToolAction         — Stamp (Draft / Approved / etc.)
  annotColorPickerButton  — Color-picker QPushButton in the annotation toolbar

AnnotationPropertiesWidget (dock objectName: annotationPropertiesDock)
  annotColorButton        — QPushButton inside AnnotationPropertiesWidget

Dock panels
  commentPanelDock              — Comments panel (QDockWidget)
  annotationPropertiesDock      — Annotation Properties panel (QDockWidget)

Note: there is no dedicated toggle menu action for the comment panel or the
annotation properties dock in the View menu.  Both docks are shown/raised
directly by locating them in the UIA tree.

Note: ``linkAction`` / ``selectAction`` in the original brief map to
``selectToolAction``; there is no linkToolAction in the current codebase.
"""

import time

import pyautogui
import pywinauto.timings

from tests.ui_harness.core.canvas import (
    canvas_rect,
    click_at,
    drag,
    draw_rectangle,
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

__all__ = [
    "activate_tool",
    "place_sticky_note",
    "draw_ink_stroke",
    "draw_shape",
    "highlight_range",
    "set_color",
    "open_comment_panel",
    "open_annotation_properties",
]

# ---------------------------------------------------------------------------
# Internal constants
# ---------------------------------------------------------------------------

# Map of caller-friendly tool names → QAction objectName in MainWindow.
_TOOL_ACTION_IDS = {
    # canonical names (match objectName directly)
    "stickyNoteAction":   "stickyNoteToolAction",
    "highlightAction":    "highlightToolAction",
    "underlineAction":    "underlineToolAction",
    "strikeOutAction":    "strikeOutToolAction",
    "freeTextAction":     "freeTextToolAction",
    "inkAction":          "inkToolAction",
    "rectangleAction":    "rectangleToolAction",
    "circleAction":       "circleToolAction",
    "lineAction":         "lineToolAction",
    "stampAction":        "stampToolAction",
    "selectAction":       "selectToolAction",
    # aliases (convenience names a caller might pass)
    "stickyNote":         "stickyNoteToolAction",
    "highlight":          "highlightToolAction",
    "underline":          "underlineToolAction",
    "strikeOut":          "strikeOutToolAction",
    "strikethrough":      "strikeOutToolAction",
    "freeText":           "freeTextToolAction",
    "textBox":            "freeTextToolAction",
    "ink":                "inkToolAction",
    "rectangle":          "rectangleToolAction",
    "circle":             "circleToolAction",
    "ellipse":            "circleToolAction",
    "line":               "lineToolAction",
    "stamp":              "stampToolAction",
    "select":             "selectToolAction",
    # The brief mentions linkAction; no link tool exists — map to select.
    "linkAction":         "selectToolAction",
}

# Accessible names used as fallbacks when UIA AutomationId lookup fails.
_TOOL_ACCESSIBLE_NAMES = {
    "stickyNoteToolAction": "Sticky Note",
    "highlightToolAction":  "Highlight",
    "underlineToolAction":  "Underline",
    "strikeOutToolAction":  "Strike Out",
    "freeTextToolAction":   "Text Box",
    "inkToolAction":        "Ink",
    "rectangleToolAction":  "Rectangle",
    "circleToolAction":     "Circle",
    "lineToolAction":       "Line",
    "stampToolAction":      "Stamp",
    "selectToolAction":     "Select",
}

# objectName of the color-picker button embedded directly in the annotation
# toolbar (set in MainWindow::createAnnotationToolBar).
_TOOLBAR_COLOR_PICKER_ID = "annotColorPickerButton"
_TOOLBAR_COLOR_PICKER_NAME = "Annotation Color Picker"

# objectName of the color button inside AnnotationPropertiesWidget.
_PROP_COLOR_BUTTON_ID = "annotColorButton"
_PROP_COLOR_BUTTON_NAME = "Annotation Color"

# Dock objectNames.
_COMMENT_DOCK_ID = "commentPanelDock"
_ANNOT_PROP_DOCK_ID = "annotationPropertiesDock"

# Seconds to wait for a dock panel to become visible after showing it.
_DOCK_APPEAR_TIMEOUT = 10

# Seconds between polls when waiting for UI state changes.
_POLL_INTERVAL = 0.2


# ---------------------------------------------------------------------------
# Public helpers
# ---------------------------------------------------------------------------

def activate_tool(session, tool_name):
    """Click the named annotation toolbar QAction to activate the tool.

    Parameters
    ----------
    session   : PDFESession
    tool_name : str
        One of the canonical action names or friendly aliases defined in
        ``_TOOL_ACTION_IDS``.  Supported values: ``stickyNoteAction``,
        ``highlightAction``, ``underlineAction``, ``strikeOutAction``,
        ``freeTextAction``, ``inkAction``, ``rectangleAction``,
        ``circleAction``, ``lineAction``, ``stampAction``, ``linkAction``
        (maps to select), ``selectAction``, and lower-case aliases without
        the "Action" suffix.

    Raises
    ------
    ValueError
        If *tool_name* is not recognised.
    RuntimeError
        If the toolbar action cannot be located or invoked.
    """
    action_id = _TOOL_ACTION_IDS.get(tool_name)
    if action_id is None:
        raise ValueError(
            f"Unknown tool name {tool_name!r}. "
            f"Supported names: {sorted(_TOOL_ACTION_IDS)}"
        )

    accessible_name = _TOOL_ACCESSIBLE_NAMES.get(action_id, "")
    window = session.window

    try:
        ctrl = by_auto_id_or_name(
            window, action_id, accessible_name, timeout=10
        )
    except Exception as exc:
        raise RuntimeError(
            f"Could not locate annotation tool action "
            f"(auto_id={action_id!r}, name={accessible_name!r}): {exc}"
        ) from exc

    invoke(ctrl)
    wait_stable(0.15)


def place_sticky_note(session, x, y, text=None):
    """Click the canvas at (x, y) to place a sticky note, then type *text*.

    This function does NOT activate the sticky note tool — call
    ``activate_tool(session, 'stickyNoteAction')`` first.

    After a sticky note is placed PDFE may auto-open a text popup for the
    note contents.  If *text* is provided it is typed into that popup
    (which should already have keyboard focus after placement).

    Parameters
    ----------
    session : PDFESession
    x       : int  Canvas-relative pixels from the left edge.
    y       : int  Canvas-relative pixels from the top edge.
    text    : str or None  Text to type into the sticky note popup.
    """
    sx, sy = page_to_screen(session.app, x, y)
    click_at(sx, sy)
    wait_stable(0.25)  # allow the popup to open

    if text:
        # The StickyNotePopup auto-grabs focus when it opens; type directly.
        pyautogui.typewrite(text, interval=0.02)
        wait_stable(0.1)


def draw_ink_stroke(session, points):
    """Draw a freehand ink stroke through a sequence of canvas-relative points.

    The mouse is pressed at the first point, moved through the intermediate
    points, and released at the last point.  Activate the ink tool first via
    ``activate_tool(session, 'inkAction')``.

    Parameters
    ----------
    session : PDFESession
    points  : sequence of (x, y) tuples
        Canvas-relative pixel coordinates.  Must contain at least two points.

    Raises
    ------
    ValueError
        If fewer than two points are supplied.
    """
    if len(points) < 2:
        raise ValueError(
            f"draw_ink_stroke requires at least 2 points; got {len(points)}."
        )

    screen_pts = [page_to_screen(session.app, px, py) for px, py in points]

    start_x, start_y = screen_pts[0]
    pyautogui.moveTo(start_x, start_y, duration=0.1)
    pyautogui.mouseDown(button="left")

    for sx, sy in screen_pts[1:]:
        pyautogui.moveTo(sx, sy, duration=0.08)

    pyautogui.mouseUp(button="left")
    wait_stable(0.2)


def draw_shape(session, rect, kind="rectangle"):
    """Drag on the canvas to draw a shape annotation.

    Activate the appropriate tool first (e.g. ``activate_tool(session,
    'rectangleAction')``).

    Parameters
    ----------
    session : PDFESession
    rect    : (x1, y1, x2, y2)
        Canvas-relative pixel coordinates of the bounding box corners.
        (x1, y1) is the drag start; (x2, y2) is the drag end.
    kind    : str
        Informational label (``"rectangle"``, ``"circle"``, ``"line"``).
        The actual tool must already be active; this parameter is not used
        to switch tools.
    """
    x1, y1, x2, y2 = rect
    sx1, sy1 = page_to_screen(session.app, x1, y1)
    sx2, sy2 = page_to_screen(session.app, x2, y2)
    draw_rectangle(sx1, sy1, sx2, sy2)
    wait_stable(0.2)


def highlight_range(session, start_xy, end_xy):
    """Drag from *start_xy* to *end_xy* to apply the active text-markup tool.

    The highlight (or underline / strike-out) tool must already be active
    before calling this function.

    Parameters
    ----------
    session   : PDFESession
    start_xy  : (x, y) canvas-relative start of the drag.
    end_xy    : (x, y) canvas-relative end of the drag.
    """
    sx1, sy1 = page_to_screen(session.app, start_xy[0], start_xy[1])
    sx2, sy2 = page_to_screen(session.app, end_xy[0], end_xy[1])
    drag(sx1, sy1, sx2, sy2, button="left", duration=0.3)
    wait_stable(0.2)


def set_color(session, rgb):
    """Set the annotation color via the toolbar color-picker button.

    Opens a QColorDialog by clicking the ``annotColorPickerButton`` in the
    annotation toolbar.  The native color dialog is then driven by typing the
    hex value into its HTML-color line-edit and accepting.

    If the toolbar color-picker cannot be found (e.g. no document is open),
    the function tries the ``annotColorButton`` inside the Annotation
    Properties dock as a fallback.

    Parameters
    ----------
    session : PDFESession
    rgb     : (r, g, b) tuple of ints 0–255.

    Raises
    ------
    RuntimeError
        If neither color button can be located.
    """
    r, g, b = rgb
    hex_color = f"#{r:02x}{g:02x}{b:02x}"

    window = session.window

    # Try toolbar button first.
    color_btn = None
    for auto_id, acc_name in [
        (_TOOLBAR_COLOR_PICKER_ID, _TOOLBAR_COLOR_PICKER_NAME),
        (_PROP_COLOR_BUTTON_ID, _PROP_COLOR_BUTTON_NAME),
    ]:
        try:
            color_btn = by_auto_id_or_name(window, auto_id, acc_name, timeout=5)
            break
        except Exception:
            continue

    if color_btn is None:
        raise RuntimeError(
            f"Could not locate any color-picker button "
            f"(tried auto_ids {_TOOLBAR_COLOR_PICKER_ID!r} and "
            f"{_PROP_COLOR_BUTTON_ID!r}). "
            "Ensure a document is open and an annotation is selected."
        )

    invoke(color_btn)
    wait_stable(0.4)  # allow QColorDialog to open

    # Drive the native Windows color dialog.
    # The dialog has an HTML hex-color edit field; Tab to it and type the value.
    # On Windows the dialog layout is: OK, Cancel, then the hex edit is reachable
    # via the "Add to Custom Colors" area.  The most robust approach is to type
    # the hex string into the dialog's edit control directly.
    try:
        color_dlg = session.app.window(title_re=r"Select Color|Color")
        color_dlg.wait("visible", timeout=8)

        # Look for the HTML-color line-edit inside the dialog (objectName "htmlEdit"
        # in QColorDialog source, UIA AutomationId may vary).
        html_edit = None
        for aid in ("htmlEdit", "html_edit"):
            try:
                html_edit = color_dlg.child_window(auto_id=aid, control_type="Edit")
                html_edit.wait("exists visible", timeout=2)
                break
            except Exception:
                continue

        if html_edit is not None:
            html_edit.set_focus()
            html_edit.type_keys("^a", with_spaces=False)
            html_edit.type_keys(hex_color, with_spaces=False)
            pyautogui.press("enter")
        else:
            # Fallback: type the hex value and press Enter; works when the
            # dialog's hex field has initial focus or is the only edit field.
            pyautogui.typewrite(hex_color, interval=0.03)
            pyautogui.press("enter")

        wait_stable(0.15)
    except Exception:
        # If the dialog was not found (e.g. it closed immediately), dismiss
        # gracefully and continue; the color may or may not have been applied.
        pyautogui.press("escape")
        wait_stable(0.1)


def open_comment_panel(session):
    """Show and raise the Comment Panel dock.

    PDFE does not expose a dedicated toggle action for the comment panel in
    the View menu.  Instead this function locates the dock widget directly by
    its objectName (``commentPanelDock``) and calls show/raise via UIA or
    falls back to the dock's built-in toggleViewAction.

    Parameters
    ----------
    session : PDFESession

    Raises
    ------
    RuntimeError
        If the comment panel dock cannot be located.
    """
    _show_dock(session, _COMMENT_DOCK_ID, "Comments Panel")


def open_annotation_properties(session):
    """Show and raise the Annotation Properties dock.

    Locates the dock widget by objectName ``annotationPropertiesDock`` and
    brings it to the front.  The dock is tabbed with the Properties and Form
    Properties docks; raising it makes the Annotation Properties tab active.

    Parameters
    ----------
    session : PDFESession

    Raises
    ------
    RuntimeError
        If the annotation properties dock cannot be located.
    """
    _show_dock(session, _ANNOT_PROP_DOCK_ID, "Annotation Properties Panel")


# ---------------------------------------------------------------------------
# Internal helpers
# ---------------------------------------------------------------------------

def _show_dock(session, dock_auto_id, dock_accessible_name):
    """Ensure a QDockWidget is visible and raised.

    Strategy
    --------
    1. Locate the dock via auto_id or accessible name.
    2. Attempt ``ctrl.invoke()`` which triggers the dock's toggleViewAction.
    3. If the dock is already visible the invoke is a no-op; verify with
       ``wait_until`` that it is visible.

    Parameters
    ----------
    session              : PDFESession
    dock_auto_id         : str  objectName of the QDockWidget.
    dock_accessible_name : str  accessibleName / UIA Name of the dock.

    Raises
    ------
    RuntimeError
        If the dock cannot be found or made visible.
    """
    window = session.window

    try:
        dock = by_auto_id_or_name(
            window, dock_auto_id, dock_accessible_name, timeout=10
        )
    except Exception as exc:
        raise RuntimeError(
            f"Could not locate dock panel "
            f"(auto_id={dock_auto_id!r}, name={dock_accessible_name!r}): {exc}"
        ) from exc

    # If the dock reports as not visible, invoke (toggles visibility on).
    try:
        if not dock.is_visible():
            invoke(dock)
    except Exception:
        # Some dock controls do not support IsVisible directly; try invoking.
        try:
            invoke(dock)
        except Exception:
            pass

    # Wait until the dock is visible.
    def _is_visible():
        try:
            return dock.is_visible()
        except Exception:
            return False

    try:
        pywinauto.timings.wait_until(
            timeout=_DOCK_APPEAR_TIMEOUT,
            retry_interval=_POLL_INTERVAL,
            func=_is_visible,
        )
    except pywinauto.timings.TimeoutError:
        raise RuntimeError(
            f"Dock panel {dock_auto_id!r} did not become visible "
            f"within {_DOCK_APPEAR_TIMEOUT}s."
        )

    # Raise the dock (bring its tab to front if it is tabbed).
    try:
        dock.set_focus()
    except Exception:
        pass

    wait_stable(0.15)
