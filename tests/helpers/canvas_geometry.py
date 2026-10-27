# -*- coding: utf-8 -*-
"""Ask the app where a page is on the canvas. Do not guess.

WHY
---
15 UI test files compute canvas coordinates from hardcoded offsets
(``rect.left + 60``) or fractions of the canvas. That works only while the
page happens to sit under the chosen point, and it fails silently when it does
not -- the click lands on the viewport background, the tool sees nothing, and
the test reports a timeout or a missing dialog rather than "I clicked off the
page".

That is not hypothetical. Driving a region drag on a scanned page, a
fraction-of-the-canvas guess put the press at **PDF x = -30.7** -- off the left
edge -- and a screenshot-based measurement disagreed with the UIA canvas
rectangle by ~145px because the two are in different DPI spaces. Both looked
plausible; both were wrong; neither failed loudly.

The application already computes this exactly, in
``PdfCanvas::getPageScreenRect()``. ``testPageRectQueryPipe`` exposes it, so a
test can ask instead of guess.

USE
---
    from tests.helpers.canvas_geometry import canvas_rect, page_rect_on_canvas

    canvas = canvas_rect(session)
    px, py, pw, ph = page_rect_on_canvas(session, 0)
    x = canvas.left + px + int(pw * 0.5)     # centre of the page
    y = canvas.top  + py + int(ph * 0.25)

Coordinates come back in CANVAS-WIDGET space, so add the canvas rect's origin
to get screen coordinates and do no DPI arithmetic yourself.
"""
from __future__ import annotations

import time
from typing import Optional, Tuple

from tests.helpers.qinvoke import _write_to_pipe, _find_pipe


def canvas_rect(session):
    """The PdfCanvas widget rectangle, or None.

    Matches on the AutomationId SUFFIX: Qt reports it with a full object path
    ("QApplication.mainWindow.pdfCanvas"), so a bare ``auto_id="pdfCanvas"``
    lookup silently finds nothing.
    """
    for d in session.window.descendants():
        try:
            if (d.element_info.automation_id or "").endswith("pdfCanvas"):
                return d.rectangle()
        except Exception:
            continue
    return None


def canvas_region_client_px(session, settle: float = 0.0):
    """``(x, y, w, h)`` of the PdfCanvas in MAIN-WINDOW CLIENT DEVICE pixels.

    This is the capture-region answer, and it is deliberately NOT
    :func:`canvas_rect` above. ``canvas_rect`` returns a pywinauto SCREEN
    rectangle; ``capture_hwnd`` works in the window's CLIENT area. Converting
    between them by subtracting ``session.window.rectangle()`` is wrong twice
    over -- that is the FRAME rect, so the result is off by the title-bar
    height and the border width -- and it is in logical rather than device
    pixels. An earlier incident in this repo measured a screenshot and a UIA
    canvas rect disagreeing by ~145 px for exactly this reason.

    The `testCanvasRect` seam does the conversion inside the application,
    where ``canvas_->mapTo(this, {0,0})`` and ``devicePixelRatioF()`` are both
    known exactly, and answers in the space ``GetClientRect`` uses. Hand the
    result straight to ``capture_hwnd(hwnd, region=...)``.

    Returns None if the seam does not answer. Callers must FAIL on None --
    never fall back to a fraction-of-the-window crop, which is the defect
    behind CI-LIMITATION-VISUAL-REGRESSION-SESSION-DRIFT (GitHub #136).
    """
    # READ ONLY -- deliberately no pipe WRITE. MainWindow republishes this
    # value on a 250 ms timer, so the answer is already there. Writing to a
    # query pipe would move Win32 focus to the hidden QLineEdit and the next
    # synthesised keystroke would land on it instead of the canvas; this
    # helper is called by screenshot code that very often runs immediately
    # before a navigation key. See the seam comment in MainWindow.cpp.
    el = _find_pipe(session, "testCanvasRectResultPipe")
    raw = ""
    if el is not None:
        raw = (el.get_value() or "").strip()
    if not raw or raw == "none":
        return None
    try:
        x, y, w, h = (int(float(v)) for v in raw.split(","))
    except ValueError:
        return None
    if w <= 0 or h <= 0:
        return None
    return x, y, w, h


def page_rect_on_canvas(session, page_index: int,
                        settle: float = 1.0
                        ) -> Optional[Tuple[int, int, int, int]]:
    """(x, y, w, h) of `page_index` in canvas-widget coordinates, or None.

    None means the seam did not answer -- treat that as a test failure rather
    than falling back to a guess, which is the whole point of this module.
    """
    if not _write_to_pipe(session, "testPageRectQueryPipe", str(page_index)):
        return None
    time.sleep(settle)
    el = _find_pipe(session, "testPageRectResultPipe")
    if el is None:
        return None
    raw = (el.get_value() or "").strip()
    if not raw or raw == "none":
        return None
    try:
        x, y, w, h = (int(float(v)) for v in raw.split(","))
    except ValueError:
        return None
    if w <= 0 or h <= 0:
        return None
    return x, y, w, h


def point_on_page(session, page_index: int,
                  fx: float, fy: float) -> Optional[Tuple[int, int]]:
    """Screen (x, y) at fractional position (fx, fy) WITHIN the page.

    ``point_on_page(s, 0, 0.5, 0.25)`` is a quarter down the middle of page 0 --
    and is on the page whatever the zoom, scroll position or DPI, which
    ``rect.left + 100`` is not.
    """
    canvas = canvas_rect(session)
    page = page_rect_on_canvas(session, page_index)
    if canvas is None or page is None:
        return None
    px, py, pw, ph = page
    return (canvas.left + px + int(pw * fx),
            canvas.top + py + int(ph * fy))


def view_state(session, settle: float = 0.6):
    """Observable canvas state: dict(page, scrollY, annots, selected).

    The oracle for gestures that previously asserted only
    ``session.proc.poll() is None`` -- i.e. that the app had not crashed. That
    assertion passes whether or not the gesture did anything at all, including
    when the click lands off the page entirely, which is exactly how a family
    of canvas tests stayed green while testing nothing.

    Returns None if the seam does not answer; callers should FAIL on that
    rather than fall back to the crash-only check.

    FOCUS SIDE EFFECT -- read this before using it around keyboard input.
    Writing to a pipe goes through UIA ``set_edit_text``, which moves Win32
    focus to the hidden pipe QLineEdit. Any ``send_key`` issued afterwards
    therefore lands on the PIPE, not on the canvas. This is not theoretical:
    it made test_canvas_scroll_keyboard report "PageDown did nothing" when the
    application was handling PageDown perfectly (instrumentation showed
    currentPage going 0 -> 1). If you query state and then synthesise keys,
    re-focus the canvas in between -- ``refocus_canvas(session)`` does it.
    """
    if not _write_to_pipe(session, "testViewStateQueryPipe", "?"):
        return None
    time.sleep(settle)
    el = _find_pipe(session, "testViewStateResultPipe")
    if el is None:
        return None
    raw = (el.get_value() or "").strip()
    if not raw:
        return None
    out = {}
    for part in raw.split(","):
        if "=" not in part:
            continue
        k, v = part.split("=", 1)
        # Most values are ints; `rect` is "x y w h" (space-separated, so it
        # cannot collide with the comma separator) and is parsed to a tuple.
        # An int-only parser silently returned None here, which would have
        # made every caller's `assert state is not None` fail for the wrong
        # reason.
        if k == "rect":
            out[k] = None if v == "none" else tuple(int(n) for n in v.split())
            continue
        try:
            out[k] = int(v)
        except ValueError:
            return None
    return out or None


def refocus_canvas(session, settle: float = 0.4) -> bool:
    """Return Qt keyboard focus to the canvas after a pipe query.

    Any pipe write moves Win32 focus to the hidden pipe QLineEdit (see
    view_state). Call this before synthesising keyboard input, or the keys go
    to the pipe and the gesture appears to do nothing.
    """
    from tests.helpers.win32_input import send_click
    r = canvas_rect(session)
    if r is None:
        return False
    send_click((r.left + r.right) // 2, (r.top + r.bottom) // 2)
    time.sleep(settle)
    return True
