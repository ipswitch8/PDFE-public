# -*- coding: utf-8 -*-
"""PyAutoGUI-based canvas pixel operations for the PDFE PDF canvas.

The PdfCanvas widget is a custom-painted Qt widget.  Its internal content
(page pixels, annotation overlays, text selection handles) is not exposed in
the UIA control tree and cannot be reached by pywinauto.  Use these helpers
for canvas-level interaction: click at a point, draw a rectangle, drag, and
sample colours.

Coordinate system
-----------------
All (x, y) arguments are **screen pixel coordinates** (absolute screen
position, NOT canvas-relative).  Use canvas_rect() to obtain the canvas
bounding box, then convert page-relative coordinates via page_to_screen().

Determinism note
----------------
These helpers inherit the DPI and scaling constraints documented in
DETERMINISM.md.  Ensure the session has been launched via
tests.ui_harness.core.launch.PDFESession before calling these functions.
"""

import time

import pyautogui
import pygetwindow

# Safety guard — reduce PyAutoGUI speed so movements are reliable on CI.
pyautogui.PAUSE = 0.05
# FAILSAFE off: it is a human-abort feature that misfires on headless/RDP
# CI where the cursor starts at a corner (raises before the call can move
# the mouse). tests/conftest.py also enforces this at runtime. See
# known_issues.md (Phase 7 sweep, run 27461377313).
pyautogui.FAILSAFE = False


def _ensure_pdfe_foreground(hwnd: int, timeout: float = 2.0) -> None:
    """Bring ``hwnd`` to the foreground; raise FocusError if it fails.

    Used by press_key / hotkey / type_text when the caller supplies a
    HWND.  Import is deferred so canvas.py can be imported on
    non-Windows CI without side-effects.
    """
    from tests.helpers.keys import (  # noqa: PLC0415
        ensure_foreground,
        FocusError,
        verify_foreground_is,
    )
    if not ensure_foreground(hwnd, timeout=timeout):
        raise FocusError(
            f"canvas keyboard helper: could not bring HWND 0x{hwnd:x} "
            "to the foreground within "
            f"{timeout:.1f}s — refusing to send keystrokes that would "
            "route to the wrong window."
        )
    if not verify_foreground_is(hwnd):
        raise FocusError(
            f"canvas keyboard helper: foreground verification failed for "
            f"HWND 0x{hwnd:x} immediately after ensure_foreground succeeded."
        )


# ---------------------------------------------------------------------------
# Canvas geometry helpers
# ---------------------------------------------------------------------------

def canvas_rect(app):
    """Return the bounding rect of the pdfCanvas widget.

    Qt's UIA bridge exposes QWidget subclasses as 'Pane' control type and
    uses the full object path as AutomationId ("QApplication.mainWindow.pdfCanvas").
    The accessible name is the currently open file name.

    Parameters
    ----------
    app : pywinauto.Application

    Returns
    -------
    pywinauto.win32structures.RECT with .left .top .right .bottom
    """
    win = app.window(title_re=r".*PDFE.*")
    # Try suffix match on auto_id ("pdfCanvas") — control type is "Pane" in Qt UIA.
    from tests.ui_harness.core.locate import by_auto_id
    canvas = by_auto_id(win, "pdfCanvas", control_type=None, timeout=10)
    return canvas.rectangle()


def canvas_center(app):
    """Return (x, y) screen coordinates of the PdfCanvas centre."""
    r = canvas_rect(app)
    return ((r.left + r.right) // 2, (r.top + r.bottom) // 2)


def page_to_screen(app, canvas_x, canvas_y):
    """Convert canvas-relative (x, y) into absolute screen coordinates.

    Parameters
    ----------
    app       : pywinauto.Application
    canvas_x  : int  pixels from the left edge of the canvas widget
    canvas_y  : int  pixels from the top edge of the canvas widget

    Returns
    -------
    (screen_x, screen_y) tuple of ints
    """
    r = canvas_rect(app)
    return (r.left + canvas_x, r.top + canvas_y)


# ---------------------------------------------------------------------------
# Mouse operations
# ---------------------------------------------------------------------------

def click_at(screen_x, screen_y, button="left", clicks=1, duration=0.1):
    """Move to (screen_x, screen_y) and click.

    Parameters
    ----------
    screen_x, screen_y : int
    button   : "left" | "right" | "middle"
    clicks   : int  number of clicks
    duration : float  move duration in seconds
    """
    pyautogui.moveTo(screen_x, screen_y, duration=duration)
    pyautogui.click(screen_x, screen_y, button=button, clicks=clicks)


def double_click_at(screen_x, screen_y, duration=0.1):
    """Double-click at (screen_x, screen_y)."""
    pyautogui.moveTo(screen_x, screen_y, duration=duration)
    pyautogui.doubleClick(screen_x, screen_y)


def right_click_at(screen_x, screen_y, duration=0.1):
    """Right-click at (screen_x, screen_y) to open a context menu."""
    pyautogui.moveTo(screen_x, screen_y, duration=duration)
    pyautogui.rightClick(screen_x, screen_y)


def drag(start_x, start_y, end_x, end_y, button="left", duration=0.3):
    """Press, drag, and release the mouse.

    Parameters
    ----------
    start_x, start_y : int  screen coordinates of the drag start
    end_x,   end_y   : int  screen coordinates of the drag end
    button           : str
    duration         : float  total drag duration in seconds
    """
    pyautogui.moveTo(start_x, start_y, duration=0.1)
    pyautogui.mouseDown(button=button)
    pyautogui.moveTo(end_x, end_y, duration=duration)
    pyautogui.mouseUp(button=button)


def draw_rectangle(x1, y1, x2, y2, button="left", duration=0.3):
    """Drag from top-left to bottom-right to draw a rectangle selection.

    Used to mark redaction regions, select text blocks, etc.
    """
    drag(x1, y1, x2, y2, button=button, duration=duration)


# ---------------------------------------------------------------------------
# Keyboard helpers
# ---------------------------------------------------------------------------

def press_key(key, hwnd=None):
    """Press a single keyboard key by name (e.g. 'enter', 'escape', 'delete').

    Parameters
    ----------
    key  : str  Key name understood by pyautogui (e.g. 'enter', 'escape').
    hwnd : int | None
        Win32 HWND of the window that should receive the key.  When
        supplied, ``ensure_foreground`` is called before the keystroke and
        ``FocusError`` is raised if the window cannot be brought forward.
        Pass ``session.window.handle`` (or an equivalent) from the
        calling test.  Omitting ``hwnd`` falls back to the legacy
        behaviour (no focus guard) and is deprecated.
    """
    if hwnd is not None:
        _ensure_pdfe_foreground(hwnd)
    pyautogui.press(key)


def hotkey(*keys, hwnd=None):
    """Press a key combination (e.g. hotkey('ctrl', 's')).

    Parameters
    ----------
    *keys : str  Key names forwarded to pyautogui.hotkey.
    hwnd  : int | None
        Win32 HWND of the target window.  When supplied, foreground is
        enforced before the keystroke; ``FocusError`` is raised if the
        window cannot be brought forward.  Deprecated without ``hwnd``.
    """
    if hwnd is not None:
        _ensure_pdfe_foreground(hwnd)
    pyautogui.hotkey(*keys)


def type_text(text, interval=0.02, hwnd=None):
    """Type a string of text character by character.

    Parameters
    ----------
    text     : str
    interval : float  seconds between keystrokes
    hwnd     : int | None
        Win32 HWND of the target window.  When supplied, foreground is
        enforced before typing; ``FocusError`` is raised if the window
        cannot be brought forward.  Deprecated without ``hwnd``.
    """
    if hwnd is not None:
        _ensure_pdfe_foreground(hwnd)
    pyautogui.typewrite(text, interval=interval)


# ---------------------------------------------------------------------------
# Colour sampling
# ---------------------------------------------------------------------------

def pick_color(screen_x, screen_y):
    """Return the RGB colour tuple at the given screen pixel.

    Returns
    -------
    (r, g, b) tuple of ints 0–255
    """
    screenshot = pyautogui.screenshot(region=(screen_x, screen_y, 1, 1))
    return screenshot.getpixel((0, 0))[:3]


def pick_color_canvas(app, canvas_x, canvas_y):
    """Sample the colour at a canvas-relative pixel position.

    Parameters
    ----------
    app      : pywinauto.Application
    canvas_x : int  pixels from canvas left edge
    canvas_y : int  pixels from canvas top edge

    Returns
    -------
    (r, g, b) tuple
    """
    sx, sy = page_to_screen(app, canvas_x, canvas_y)
    return pick_color(sx, sy)


# ---------------------------------------------------------------------------
# Wait helpers
# ---------------------------------------------------------------------------

def wait_stable(delay=0.3):
    """Sleep briefly to let Qt repaint after an interaction."""
    time.sleep(delay)
