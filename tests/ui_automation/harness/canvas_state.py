# -*- coding: utf-8 -*-
"""Read PdfCanvas state (rotation, page index, zoom) via UIA.

Pipeline F phase-6 added :class:`PdfCanvasAccessible` which exposes
the canvas's view rotation, current page, and zoom in its UIA
Description string. This module pulls those values out so tests can
write real oracles instead of relying on ``win.is_visible()``.

Description format from PdfCanvasAccessible::Description:
    "PDF viewer, page X of Y, zoom Z%, rotation R degrees"
"""
from __future__ import annotations

import re
from typing import Optional


_ROTATION_RE = re.compile(r"rotation (\d+)")
_PAGE_RE = re.compile(r"page (\d+) of (\d+)")
_ZOOM_RE = re.compile(r"zoom (\d+)%")


def _canvas_description(win) -> str:
    """Return the PdfCanvas's UIA Description string, or empty."""
    # Suffix match, not the literal full path -- the id gained a
    # `pdfCentralArea` segment and the literal silently stopped matching.
    # See canvas_click.find_canvas (GitHub #138).
    from tests.ui_automation.harness.canvas_click import find_canvas
    canvas = find_canvas(win)
    if canvas is None:
        return ""

    desc = ""
    for accessor in ("description", "value"):
        try:
            d = getattr(canvas.element_info, accessor, None)
            if isinstance(d, str) and d:
                desc = d
                break
        except Exception:  # noqa: BLE001
            continue
    if not desc:
        # Last resort: legacy IAccessible Description via UIA element
        try:
            desc = canvas.element_info.element.CurrentItemStatus or ""
        except Exception:  # noqa: BLE001
            desc = ""
    return desc


def _read_rotation_via_pipe(win) -> int:
    """Pipeline a7c4f891 phase-2 (D3 / P-GAP-84): read rotation
    via the testCanvasRotationQueryPipe + testCanvasRotationResultPipe
    pair. Write any non-empty token to the query pipe, then read the
    result pipe's text. The C++ handler runs synchronously on the Qt
    event loop. Returns -1 on failure (pipe not reachable — e.g.,
    PDFE_TEST_PIPES not set in this build).
    """
    try:
        from tests.ui_automation.harness.dialog_helpers import find_by_suffix
    except ImportError:  # noqa: BLE001
        return -1
    try:
        query = find_by_suffix(win, "testCanvasRotationQueryPipe")
        result = find_by_suffix(win, "testCanvasRotationResultPipe")
        if query is None or result is None:
            return -1
        query.set_edit_text("q")  # any non-empty token triggers handler
        import time
        time.sleep(0.2)
        text = (result.window_text() or "").strip()
        if not text:
            return -1
        return int(text)
    except Exception:  # noqa: BLE001
        return -1


def read_canvas_rotation(win) -> int:
    """Return the canvas's view rotation in degrees (0/90/180/270),
    or -1 if neither the pipe nor the description string yields a
    parseable value.

    Pipeline a7c4f891 phase-2 (D3): prefer the new query+result pipe
    pair which is a hard guarantee under PDFE_TEST_PIPES. Falls back
    to the PdfCanvasAccessible Description scrape for legacy/non-
    pipe builds.

    The caller MUST treat -1 as a hard test failure when the
    rotation oracle is the assertion target — otherwise the test
    silently turns green on hosts where neither path returns data.
    """
    via_pipe = _read_rotation_via_pipe(win)
    if via_pipe >= 0:
        return via_pipe
    desc = _canvas_description(win)
    if not desc:
        return -1
    m = _ROTATION_RE.search(desc)
    return int(m.group(1)) if m else -1


def read_canvas_page(win) -> Optional[tuple]:
    """Return ``(current_page_1_indexed, total_pages)`` or ``None``."""
    desc = _canvas_description(win)
    if not desc:
        return None
    m = _PAGE_RE.search(desc)
    if not m:
        return None
    return int(m.group(1)), int(m.group(2))


def read_canvas_zoom_percent(win) -> int:
    """Return the canvas zoom as an integer percent, or -1 if missing."""
    desc = _canvas_description(win)
    if not desc:
        return -1
    m = _ZOOM_RE.search(desc)
    return int(m.group(1)) if m else -1
