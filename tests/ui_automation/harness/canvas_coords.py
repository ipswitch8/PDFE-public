# -*- coding: utf-8 -*-
"""PdfCanvas coordinate mapping for PyAutoGUI-driven tests.

The PDFE ``PdfCanvas`` widget is custom-painted and does not expose
its interior (text selection regions, annotation handles, drag zones)
to the UIA tree. Tests that need to click at a specific PDF
coordinate must:

1. Find the canvas rectangle in screen coordinates via pywinauto.
2. Map a PDF point to a screen point using the canvas's current zoom
   and scroll offset.
3. Invoke PyAutoGUI to click/drag at the resolved screen coordinate.

This module encapsulates the mapping math and the PyAutoGUI calls so
individual tests can simply say "click on annotation at PDF (100, 200)
on page 0" and not worry about the transform.

Current scope: the mapping assumes the default 1x zoom, no scroll,
and single-page visibility. Multi-page scroll + zoom support will
land in phase-10 when canvas tests are implemented in anger.
"""
from __future__ import annotations

import time
from dataclasses import dataclass
from typing import Optional, Tuple


@dataclass
class CanvasBounds:
    """Screen-coordinate bounds of a PdfCanvas widget."""
    left: int
    top: int
    width: int
    height: int

    @property
    def center(self) -> Tuple[int, int]:
        return (self.left + self.width // 2,
                self.top + self.height // 2)


def get_canvas_bounds(main_window) -> Optional[CanvasBounds]:
    """Find the ``pdfCanvas`` widget and return its screen bounds.

    Returns ``None`` if the canvas is not reachable via UIA.

    Pipeline b5e3a9f2 phase-5 (P-GAP-71): the previous lookup used
    ``child_window(auto_id_re=r".*\\.pdfCanvas$")`` which required the
    automation_id to end with ``.pdfCanvas`` (dot prefix). On Qt6,
    setObjectName("pdfCanvas") produces automation_id="pdfCanvas"
    with no dot prefix, so the regex never matched and all 6 phase3
    tests failed with "pdfCanvas not found". Aligned with
    ``find_by_suffix`` semantics (exact match OR dot-prefixed
    suffix) — the same lookup ``click_canvas()`` uses successfully.
    """
    try:
        from tests.ui_automation.harness.dialog_helpers import find_by_suffix
        canvas = find_by_suffix(main_window, "pdfCanvas")
        if canvas is None:
            return None
        rect = canvas.rectangle()
        return CanvasBounds(
            left=rect.left,
            top=rect.top,
            width=rect.right - rect.left,
            height=rect.bottom - rect.top,
        )
    except Exception:  # noqa: BLE001
        return None


def pdf_point_to_screen(
    bounds: CanvasBounds,
    pdf_x: float,
    pdf_y: float,
    page_pdf_width: float = 612.0,
    page_pdf_height: float = 792.0,
) -> Tuple[int, int]:
    """Convert a PDF-space point to a screen pixel.

    Simple mapping: assume the canvas shows one page at its
    full-page fit, origin at top-left, PDF y-axis flipped to
    screen y-axis. Doesn't account for zoom or scroll yet.

    :param bounds: Canvas bounds on screen.
    :param pdf_x: PDF x in points.
    :param pdf_y: PDF y in points, measured from the top.
    :param page_pdf_width: PDF page width in points (default Letter).
    :param page_pdf_height: PDF page height in points.
    :returns: ``(screen_x, screen_y)`` tuple.
    """
    fx = pdf_x / page_pdf_width
    fy = pdf_y / page_pdf_height
    sx = bounds.left + int(fx * bounds.width)
    sy = bounds.top + int(fy * bounds.height)
    return sx, sy


class ClickOutOfScreenError(RuntimeError):
    """Raised when an absolute click target is off the screen.

    A minimised or off-screen PDFE window reports a rectangle like
    ``(-32000,-32000)``; coordinates derived from it would send an absolute
    PyAutoGUI click to a screen corner / the taskbar / another window (which
    can launch an unrelated app). Refusing — and failing the test loudly — is
    safer than a blind coordinate mis-fire.
    """


def _assert_on_screen(*points) -> None:
    """Raise if any ``(x, y)`` is not a safe PDFE click target.

    Delegates to tests.helpers.click_safety.assert_safe_click, which checks the
    desktop WORK AREA (excludes the taskbar) and the PDFE window bounds.

    NOTE: an earlier version of this guard only rejected points outside
    ``pyautogui.size()``. That was WRONG for the failure it was meant to stop —
    the Windows taskbar is ON-screen, so taskbar clicks (which launch unrelated
    apps) passed the check. Always validate against the work area / PDFE window,
    never the raw screen size.
    """
    from tests.helpers.click_safety import assert_safe_click, UnsafeClickError
    for (x, y) in points:
        try:
            assert_safe_click(x, y, op="canvas click")
        except UnsafeClickError as exc:
            raise ClickOutOfScreenError(str(exc)) from exc


def pyautogui_click(x: int, y: int) -> None:
    """Click at a screen coordinate using PyAutoGUI (off-screen-guarded)."""
    import pyautogui  # Imported lazily to keep harness importable
    _assert_on_screen((x, y))
    pyautogui.click(x=x, y=y)
    time.sleep(0.1)


def pyautogui_double_click(x: int, y: int) -> None:
    """Double-click at a screen coordinate (off-screen-guarded)."""
    import pyautogui
    _assert_on_screen((x, y))
    pyautogui.doubleClick(x=x, y=y)
    time.sleep(0.1)


def pyautogui_drag(x0: int, y0: int, x1: int, y1: int,
                   duration: float = 0.3) -> None:
    """Mouse-down at (x0, y0), drag to (x1, y1), mouse-up (off-screen-guarded)."""
    import pyautogui
    _assert_on_screen((x0, y0), (x1, y1))
    pyautogui.moveTo(x0, y0)
    pyautogui.mouseDown()
    pyautogui.moveTo(x1, y1, duration=duration)
    pyautogui.mouseUp()
    time.sleep(0.1)


def click_canvas(main_window, rel_x: float = 0.5, rel_y: float = 0.5,
                 pause: float = 0.3) -> bool:
    """Click on the pdfCanvas at relative coordinates (0–1 range).

    Finds the pdfCanvas widget by ``auto_id`` suffix, converts the
    relative position to pixel coordinates within that widget, then
    dispatches a left-click via ``click_input``.  The widget-relative
    coordinate form ensures the click lands inside the canvas regardless
    of its position on screen.

    :param main_window: pywinauto ``WindowSpecification`` for the main window.
    :param rel_x: Horizontal position as a fraction of canvas width (0 = left,
        1 = right).  Default ``0.5`` (horizontal centre).
    :param rel_y: Vertical position as a fraction of canvas height (0 = top,
        1 = bottom).  Default ``0.5`` (vertical centre).
    :param pause: Seconds to sleep after the click so Qt can process the event.
    :returns: ``True`` on success, ``False`` if the canvas could not be found
        or the click raised an exception.
    """
    try:
        from tests.ui_automation.harness.dialog_helpers import find_by_suffix
        canvas = find_by_suffix(main_window, "pdfCanvas")
        if canvas is None:
            return False
        r = canvas.rectangle()
        w = r.right - r.left
        h = r.bottom - r.top
        px = int(rel_x * w)
        py = int(rel_y * h)
        canvas.click_input(coords=(px, py))
        time.sleep(pause)
        return True
    except Exception:  # noqa: BLE001
        return False


def drag_canvas(main_window, start_rel: tuple, end_rel: tuple,
                pause: float = 0.5) -> bool:
    """Drag on pdfCanvas from start to end relative coordinates.

    Uses :func:`pyautogui_drag` (mouse-down → move → mouse-up) because
    pywinauto's ``drag_mouse_input`` with ``absolute=False`` sometimes
    fails to keep the button held across the full motion path on Qt
    custom-painted widgets.

    :param main_window: pywinauto ``WindowSpecification`` for the main window.
    :param start_rel: ``(rel_x, rel_y)`` of drag start as canvas fractions.
    :param end_rel: ``(rel_x, rel_y)`` of drag end as canvas fractions.
    :param pause: Seconds to sleep after the drag completes.
    :returns: ``True`` on success, ``False`` if the canvas was not found.
    """
    try:
        from tests.ui_automation.harness.dialog_helpers import find_by_suffix
        canvas = find_by_suffix(main_window, "pdfCanvas")
        if canvas is None:
            return False
        r = canvas.rectangle()
        w = r.right - r.left
        h = r.bottom - r.top

        x0 = r.left + int(start_rel[0] * w)
        y0 = r.top + int(start_rel[1] * h)
        x1 = r.left + int(end_rel[0] * w)
        y1 = r.top + int(end_rel[1] * h)

        pyautogui_drag(x0, y0, x1, y1, duration=0.4)
        time.sleep(pause)
        return True
    except Exception:  # noqa: BLE001
        return False
