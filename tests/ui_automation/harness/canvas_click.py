# -*- coding: utf-8 -*-
"""PyAutoGUI canvas click harness for tests that need pixel-level drive.

Pipeline K phase-4: bridges the gap between pywinauto's UIA tree (which
ends at the PdfCanvas widget — its painted contents are invisible to
UIA) and the actual PDF pixels. Used by TRE edit tests, sticky-note
drag tests, and any test where the oracle is a screenshot diff rather
than a UIA property change.

Public API
==========

``canvas_region(app_win)``
    Return the PdfCanvas's ``(x, y, w, h)`` rectangle in absolute
    screen coordinates. Resolved via :func:`find_canvas`, which matches
    the canvas AutomationId by TRAILING SEGMENT (``.pdfCanvas``) rather
    than by the full objectName chain -- see that function's docstring
    and GitHub #138 / #198 for why the fixed chain must not be used.
    Returns ``None`` if the canvas isn't reachable.

``click_pdf_point(app_win, page_x, page_y, button="left", clicks=1)``
    Click a PDF-coordinate point on the canvas. Maps PDF points
    (72-DPI origin top-left) to screen pixels using the canvas
    region + ``read_canvas_zoom_percent`` from
    :mod:`canvas_state`. Falls back to ``zoom=100`` if the
    accessibility description doesn't expose zoom.

``screenshot_canvas(app_win)``
    Return a ``PIL.Image`` of the current canvas region. Uses
    PyAutoGUI's region screenshot.

``pixel_diff_ratio(img_a, img_b)``
    Return the fraction of pixels that differ between two
    same-size PIL images, in [0.0, 1.0]. Threshold-based
    callers should use ``> 0.01`` as the lower bound for
    "render changed" and ``> 0.1`` for "render changed
    significantly".

Coordinate mapping
==================

PDF coordinates use a 72-DPI top-left origin (PDFE's canvas
follows screen-space convention, not the PDF spec's bottom-left
PostScript origin — the canvas widget's
:func:`PdfCanvas::pdfPointToScreen` already does the Y-flip).

Screen coordinates use whatever DPI the host display is at —
typically 96 on Windows, scaled by the QPaintDevice ratio for
HiDPI displays. The canvas reports its rendering zoom via the
PdfCanvasAccessible Description string ("zoom Z%"). The mapping
here is approximate (no scrolling correction) but adequate for
clicking in the visible page area.

Limitations
-----------
- Assumes the page is scrolled so the target pixel is visible.
- Doesn't handle multi-page layouts beyond the current page.
- Pixel-diff oracle compares raw bitmaps — anti-aliasing
  variance between runs is treated as "different". Use a
  threshold like 0.01 to absorb that noise.
"""
from __future__ import annotations

import time
from typing import Optional, Tuple

try:
    import pyautogui  # type: ignore
    _PYAUTOGUI_AVAILABLE = True
    # Disable PyAutoGUI's fail-safe in this harness. It is a human-abort
    # mechanism (FailSafeException when the cursor sits in a screen corner
    # at the start of a call). On headless/RDP CI the session starts with
    # the cursor at (0,0), so the check fired before click_pdf_point()
    # could move the mouse — three canvas tests failed this way on run
    # 27461377313. The autouse fixture in tests/conftest.py also enforces
    # this at runtime; setting it here documents intent and covers any
    # non-pytest importer of the harness. See known_issues.md.
    pyautogui.FAILSAFE = False
except ImportError:
    _PYAUTOGUI_AVAILABLE = False

try:
    from PIL import Image, ImageChops  # type: ignore
    _PIL_AVAILABLE = True
except ImportError:
    _PIL_AVAILABLE = False


def _require_pyautogui() -> None:
    if not _PYAUTOGUI_AVAILABLE:
        raise RuntimeError(
            "pyautogui is required for canvas-click tests "
            "(pip install pyautogui)"
        )


def _require_pil() -> None:
    if not _PIL_AVAILABLE:
        raise RuntimeError("Pillow is required (pip install Pillow)")


def find_canvas(app_win, timeout: float = 6.0):
    """Return the PdfCanvas UIA wrapper, or ``None``.

    MATCHES BY TRAILING SEGMENT, NOT BY FULL PATH -- GitHub #138.
    -----------------------------------------------------------
    This used to ask for the literal AutomationId
    ``"QApplication.mainWindow.pdfCanvas"``. Qt builds that id from the
    objectName CHAIN, so it changes whenever a widget is inserted anywhere
    above the canvas. One was: the canvas now lives under a
    ``pdfCentralArea`` and its id is

        QApplication.mainWindow.pdfCentralArea.pdfCanvas

    The literal stopped matching, and because every caller turns "not found"
    into "PdfCanvas absent from UIA tree", two of GitHub #138's five items
    reported the canvas as UNREACHABLE while it was sitting in the tree the
    whole time. That is a false negative wearing the costume of an
    environment limitation, which is the worst kind: it looks like something
    you cannot fix.

    Suffix matching is what ``tests/ui_harness/core/locate.py`` and
    ``tests/ui_automation/smoke_test.py`` already do, and it is immune to the
    next widget someone puts in the chain.

    Two elements answer to the id: the widget itself
    (``class_name == "pdfe::ui::PdfCanvas"``) and its accessible page child
    (``class_name == ""``, Name "Page 1"). The widget is preferred, because
    the child's rectangle is the PAGE rather than the viewport.
    """
    deadline = time.time() + timeout
    fallback = None
    while time.time() < deadline:
        try:
            for el in app_win.descendants():
                try:
                    info = el.element_info
                    aid = info.automation_id or ""
                    if aid != "pdfCanvas" and not aid.endswith(".pdfCanvas"):
                        continue
                    if (info.class_name or "").endswith("PdfCanvas"):
                        return el
                    if fallback is None:
                        fallback = el
                except Exception:  # noqa: BLE001
                    continue
        except Exception:  # noqa: BLE001
            pass
        if fallback is not None:
            return fallback
        time.sleep(0.25)
    return fallback


def canvas_region(app_win) -> Optional[Tuple[int, int, int, int]]:
    """Return the PdfCanvas screen rect ``(x, y, w, h)`` or ``None``.

    The canvas's UIA wrapper exposes ``rectangle()`` returning a
    pywinauto ``RECT``-like object with left/top/right/bottom
    integer attributes.
    """
    canvas = find_canvas(app_win)
    if canvas is None:
        return None
    try:
        r = canvas.rectangle()
    except Exception:  # noqa: BLE001
        return None
    return (r.left, r.top, r.right - r.left, r.bottom - r.top)


def _read_zoom(app_win) -> int:
    """Return canvas zoom as an integer percent. Falls back to 100."""
    try:
        from tests.ui_automation.harness.canvas_state import (
            read_canvas_zoom_percent,
        )
        z = read_canvas_zoom_percent(app_win)
        return z if z > 0 else 100
    except Exception:  # noqa: BLE001
        return 100


def _calibrate_px_per_point(app_win, page_w_pt: float = 612.0,
                            page_h_pt: float = 792.0) -> float:
    """Compute the canvas's pixels-per-PDF-point ratio.

    Strategy 1: use zoom% from PdfCanvasAccessible.Description (when
    parseable) plus the host's screen DPI. This is the "spec-correct"
    answer.

    Strategy 2 (fallback): calibrate from the canvas's screen
    rectangle vs the page's PDF size (assuming fit-page or fit-width
    layout, which is the default on doc-open). The smaller ratio
    (width vs height fit) is the actual rendering scale.

    On this host the Description string is sometimes unreachable via
    pywinauto's UIA bridge, so calibration is the more reliable
    default. Callers that need to click a specific PDF point on
    a known-size page should pass the page dimensions; the
    fallback assumes Letter (8.5×11 in = 612×792 pt).
    """
    region = canvas_region(app_win)
    if region is None:
        return 96.0 / 72.0  # last-resort default
    _rx, _ry, rw, rh = region

    # Default: calibrate from canvas/page ratio. The Description-string
    # zoom is unreliable on this host (it reports ``canvas->zoom()``,
    # which the user may not have changed from 1.0 even though the
    # canvas internally renders at fit-page). Geometry calibration
    # ALWAYS matches what's actually drawn.
    geom_ratio = min(rw / page_w_pt, rh / page_h_pt)

    # Sanity check against the Description-string zoom. If both
    # agree within ±10%, prefer the explicit zoom (a user who has
    # zoomed in/out wants the click to follow). If they disagree,
    # trust geometry.
    z = _read_zoom(app_win)
    if z > 0:
        import os
        screen_dpi = float(os.environ.get("PDFE_CANVAS_SCREEN_DPI", "96"))
        zoom_ratio = (z / 100.0) * (screen_dpi / 72.0)
        if abs(zoom_ratio - geom_ratio) / max(zoom_ratio, geom_ratio) < 0.10:
            return zoom_ratio
    return geom_ratio


def pdf_point_to_screen(app_win, page_x: float, page_y: float,
                        page_w_pt: float = 612.0,
                        page_h_pt: float = 792.0):
    """Map a PDF-point coordinate to absolute screen pixels.

    Returns ``(sx, sy)`` or ``None`` if the canvas region is not
    resolvable. Accounts for PDFE's page centering: when the canvas
    is wider/taller than the rendered page, the page is drawn
    centered with equal margins on both sides
    (``PdfCanvas`` uses ``int pageLeft = (width() - layout.widthPx) / 2``
    in ``mousePressEvent``). Older versions of this helper assumed
    the page started at the canvas's top-left, which produced
    misses of up to ~120 px when the search/comments panels were
    docked open and the canvas was wider than the page.
    """
    region = canvas_region(app_win)
    if region is None:
        return None
    rx, ry, rw, rh = region
    px_per_point = _calibrate_px_per_point(app_win, page_w_pt, page_h_pt)
    page_w_px = page_w_pt * px_per_point
    page_h_px = page_h_pt * px_per_point
    hmargin = max(0.0, (rw - page_w_px) / 2.0)
    vmargin = max(0.0, (rh - page_h_px) / 2.0)
    sx = int(rx + hmargin + page_x * px_per_point)
    sy = int(ry + vmargin + page_y * px_per_point)
    return sx, sy


class ClickOutOfBoundsError(RuntimeError):
    """Raised when a computed click target falls outside the PdfCanvas rect.

    Clicking such a point would land on the desktop, the Windows taskbar (which
    launches whatever app icon sits under the cursor), or another window —
    exactly the mis-fire class this guard exists to prevent. Refusing (and
    failing the test loudly) is safer than a blind coordinate click.
    """


def _safe_canvas_point(app_win, page_x: float, page_y: float,
                       page_w_pt: float, page_h_pt: float):
    """Map a PDF point to screen pixels AND assert it lands inside the canvas.

    Returns ``(sx, sy)`` or ``None`` when the canvas region is unresolvable
    (caller returns False). Raises ``ClickOutOfBoundsError`` when the mapped
    point is outside the PdfCanvas rectangle — a stale / off-screen / minimised
    / scrolled-away canvas would otherwise send the click to the desktop or
    taskbar.
    """
    region = canvas_region(app_win)
    if region is None:
        return None
    pt = pdf_point_to_screen(app_win, page_x, page_y, page_w_pt, page_h_pt)
    if pt is None:
        return None
    sx, sy = pt
    rx, ry, rw, rh = region
    if not (rx <= sx < rx + rw and ry <= sy < ry + rh):
        raise ClickOutOfBoundsError(
            f"canvas click target ({sx},{sy}) is outside the PdfCanvas rect "
            f"({rx},{ry} {rw}x{rh}) — refusing to click (would hit the "
            f"desktop/taskbar/another window). Canvas likely not visible / "
            f"not foreground / scrolled away."
        )
    return sx, sy


def click_pdf_point(app_win, page_x: float, page_y: float,
                    button: str = "left", clicks: int = 1,
                    page_w_pt: float = 612.0,
                    page_h_pt: float = 792.0) -> bool:
    """Click the screen pixel corresponding to a PDF-point coordinate.

    :param app_win: pywinauto app main window wrapper.
    :param page_x, page_y: PDF point coordinates (72-DPI top-left).
    :param button: ``"left"`` / ``"right"`` / ``"middle"``.
    :param clicks: Number of clicks (use 2 for double-click;
                   triple-click usually needs three single calls
                   inside Qt's double-click interval — see
                   ``triple_click_pdf_point``).
    :param page_w_pt, page_h_pt: PDF page dimensions in points (used
                   by the fallback calibration; default Letter).
    :returns: True on success, False if the canvas region was not
              resolvable.
    """
    _require_pyautogui()
    pt = _safe_canvas_point(app_win, page_x, page_y, page_w_pt, page_h_pt)
    if pt is None:
        return False
    sx, sy = pt
    pyautogui.click(x=sx, y=sy, button=button, clicks=clicks)
    return True


def triple_click_pdf_point(app_win, page_x: float, page_y: float,
                           page_w_pt: float = 612.0,
                           page_h_pt: float = 792.0) -> bool:
    """Three single clicks inside Qt's double-click interval.

    Qt's QApplication::doubleClickInterval defaults to 500ms;
    pyautogui.click(clicks=3) sometimes outpaces that. Use small
    explicit sleeps to land all three within the threshold.
    """
    _require_pyautogui()
    import time as _t
    pt = _safe_canvas_point(app_win, page_x, page_y, page_w_pt, page_h_pt)
    if pt is None:
        return False
    sx, sy = pt
    pyautogui.moveTo(sx, sy)
    for _ in range(3):
        pyautogui.click(x=sx, y=sy)
        _t.sleep(0.08)
    return True


def screenshot_canvas(app_win):
    """Return a ``PIL.Image`` of the canvas region, or ``None``."""
    _require_pyautogui()
    _require_pil()
    region = canvas_region(app_win)
    if region is None:
        return None
    rx, ry, rw, rh = region
    return pyautogui.screenshot(region=(rx, ry, rw, rh))


def pixel_diff_ratio(img_a, img_b) -> float:
    """Return the fraction of differing pixels in [0.0, 1.0].

    Both images must be the same size. Counts pixels where any RGB
    channel differs (post-conversion to RGB). Uses
    ``ImageChops.difference`` + a histogram extraction so the count
    is exact, not a bounding-box approximation.

    Callers commonly use ``> 0.001`` for "any rendering change at all"
    (catches cursor blink + active-span-scoped repaints) and ``> 0.01``
    for "substantial multi-region change".
    """
    _require_pil()
    if img_a.size != img_b.size:
        # Different sizes mean the canvas was resized between
        # captures — that itself is a "render changed" signal.
        return 1.0
    diff = ImageChops.difference(
        img_a.convert("RGB"), img_b.convert("RGB")
    )
    bbox = diff.getbbox()
    if bbox is None:
        return 0.0
    total_px = img_a.size[0] * img_a.size[1]
    if total_px <= 0:
        return 0.0
    # Console-session retry phase-2 (2026-05-06): the original bbox
    # approximation overestimated the changed area when changes were
    # scattered (caught the entire enclosing rect) and underestimated
    # when changes were concentrated in a narrow rect (like a single
    # span repaint). A pixel-accurate count via .getdata() iteration
    # is exact: any pixel where ANY of R/G/B differs is counted.
    # Crops the diff image to its bbox first to keep this fast on
    # large canvases (only iterates the changed region).
    #
    # Iterate RGB tuples directly (not luminance L-mode), because
    # ITU-R BT.601 luminance ``0.299*R + 0.587*G + 0.114*B`` rounds
    # single-channel diffs of 1 to zero (e.g. blue-only diff of 1
    # → 0.114 → quantized to 0). Per-channel "any > 0" is exact.
    cropped = diff.crop(bbox)  # diff is already RGB
    changed = sum(
        1 for px in cropped.getdata()
        if px[0] > 0 or px[1] > 0 or px[2] > 0
    )
    return min(1.0, changed / total_px)
