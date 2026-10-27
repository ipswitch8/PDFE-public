# -*- coding: utf-8 -*-
"""Pipeline a2b3c4d5 phase-3: Canvas coordinate-level tests via SendInput.

SE-CANVAS-2: Converts test_canvas_coords_p2.py interaction patterns
from pyautogui to Win32 SendInput.
"""

from __future__ import annotations

import os
import sys
import time
from pathlib import Path

import pytest
from tests.helpers.canvas_geometry import (
    point_on_page, view_state, refocus_canvas,
)

# Phase F11 / gap WIN-45: reachability is a RECORDED NON-PASS, never a skip.
from tests.ui_automation.conftest import (  # noqa: E402
    current_test_id,
    record_matrix_result,
)

_HERE = Path(__file__).resolve().parent
REPO_ROOT = _HERE.parent
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

FIXTURES = REPO_ROOT / "tests" / "fixtures"


def _has_pdfe_binary() -> bool:
    try:
        from tests.helpers.pdfe_session import PDFE_EXE
        return Path(PDFE_EXE).exists()
    except Exception:
        return False


def _is_interactive() -> bool:
    if os.environ.get("PDFE_UI_FORCE_HEADLESS") == "1":
        return False
    return bool(os.environ.get("SESSIONNAME"))


def _has_pywinauto() -> bool:
    try:
        import pywinauto  # noqa: F401
        return True
    except Exception:
        return False


pytestmark = [
    pytest.mark.skipif(not _has_pdfe_binary(), reason="PDFE binary not found"),
    pytest.mark.skipif(not _is_interactive(), reason="No interactive session"),
    pytest.mark.skipif(not _has_pywinauto(), reason="pywinauto not installed"),
]


@pytest.fixture(autouse=True)
def _isolate():
    from tests.helpers.pdfe_session import kill_pdfe_orphans
    kill_pdfe_orphans()
    yield
    kill_pdfe_orphans()


def _get_canvas_rect(session):
    canvas = session.find_optional(auto_id="pdfCanvas", timeout=3)
    if canvas is None:
        # WIN-45: unreachable -> recorded 'blocked' in the matrix, then FAIL.
        _reach = ("pdfCanvas not reachable via UIA.")
        record_matrix_result(current_test_id(), "blocked", _reach)
        pytest.fail(
            f"WIN-45 reachability blocked (recorded non-pass, not skipped): {_reach}")
    return canvas.rectangle()


def test_canvas_click_deselect(tmp_path):
    """SE-CANVAS-2: Click on empty canvas area to deselect."""
    from tests.helpers.pdfe_session import PDFESession
    from tests.helpers.win32_input import send_click

    fixture = FIXTURES / "test_single.pdf"
    if not fixture.exists():
        pytest.skip("fixture missing")

    with PDFESession(pdf_path=str(fixture)) as session:
        session.wait_for_idle()
        rect = _get_canvas_rect(session)

        # Click center to potentially select, then click edge to deselect
        cx = (rect.left + rect.right) // 2
        cy = (rect.top + rect.bottom) // 2
        send_click(cx, cy)
        time.sleep(0.2)

        # Click near edge (empty area)
        send_click(rect.left + 5, rect.top + 5)
        time.sleep(0.8)

        # The real oracle: nothing is selected after clicking empty space.
        after = view_state(session)
        assert after is not None, "view-state seam unavailable"
        assert after["selected"] < 0, (
            "clicking empty canvas should leave nothing selected; got %d"
            % after["selected"])

        assert session.proc.poll() is None


def test_canvas_annotation_move_drag(tmp_path):
    """SE-CANVAS-2: Simulate annotation move via click+drag."""
    from tests.helpers.pdfe_session import PDFESession
    from tests.helpers.win32_input import send_click, send_drag

    fixture = FIXTURES / "test_single.pdf"
    if not fixture.exists():
        pytest.skip("fixture missing")

    with PDFESession(pdf_path=str(fixture)) as session:
        session.wait_for_idle()
        rect = _get_canvas_rect(session)

        cx = (rect.left + rect.right) // 2
        cy = (rect.top + rect.bottom) // 2

        # Click to select
        send_click(cx, cy)
        time.sleep(0.2)

        # Drag to "move" (annotation or viewport)
        send_drag(cx, cy, cx + 30, cy + 30, duration=0.3)
        time.sleep(0.2)

        assert session.proc.poll() is None


def test_canvas_scroll_keyboard(tmp_path):
    """SE-CANVAS-2: Page scroll via keyboard (PageDown/PageUp)."""
    from tests.helpers.pdfe_session import PDFESession
    from tests.helpers.win32_input import send_click, send_key

    fixture = FIXTURES / "test_multipage.pdf"
    if not fixture.exists():
        pytest.skip("test_multipage.pdf missing")

    VK_NEXT = 0x22  # PageDown
    VK_PRIOR = 0x21  # PageUp

    with PDFESession(pdf_path=str(fixture)) as session:
        session.wait_for_idle()
        rect = _get_canvas_rect(session)

        # Give the WINDOW foreground focus before synthesising keys.
        # SendInput delivers to whatever window is foreground, so without this
        # the PageDown/PageUp keystrokes can go to another application
        # entirely -- which is why this test saw no view change once it
        # started asserting one.
        session.window.set_focus()
        time.sleep(0.5)

        # Click canvas to give it Qt keyboard focus within the window.
        send_click((rect.left + rect.right) // 2, (rect.top + rect.bottom) // 2)
        time.sleep(0.5)

        start = view_state(session)
        assert start is not None, "view-state seam unavailable"

        # Querying state moved Win32 focus to the hidden pipe QLineEdit, so
        # the keystroke below would land there instead of on the canvas.
        # Without this the test reported "PageDown did nothing" while the app
        # was handling it correctly -- instrumentation showed currentPage
        # going 0 -> 1. See canvas_geometry.view_state's focus note.
        assert refocus_canvas(session), "could not refocus the canvas"

        # PageDown
        send_key(VK_NEXT, extended=True)
        time.sleep(0.8)

        # The real oracle: PageDown moved the view. Previously this asserted
        # only that the process was alive, which is true whether or not the
        # key reached the canvas at all.
        after_down = view_state(session)
        assert after_down is not None, "view-state seam unavailable"
        assert (after_down["page"] > start["page"]
                or after_down["scrollY"] > start["scrollY"]), (
            "PageDown should advance the view; went page=%d scrollY=%d -> "
            "page=%d scrollY=%d"
            % (start["page"], start["scrollY"],
               after_down["page"], after_down["scrollY"]))

        # Same again: the view_state query above stole focus.
        assert refocus_canvas(session), "could not refocus the canvas"

        # PageUp
        send_key(VK_PRIOR, extended=True)
        time.sleep(0.8)

        after_up = view_state(session)
        assert after_up is not None, "view-state seam unavailable"
        assert (after_up["page"] < after_down["page"]
                or after_up["scrollY"] < after_down["scrollY"]), (
            "PageUp should move the view back; stayed at page=%d scrollY=%d"
            % (after_up["page"], after_up["scrollY"]))

        assert session.proc.poll() is None


def test_canvas_sticky_note_click(tmp_path):
    """SE-CANVAS-2: Click where a sticky note might be."""
    from tests.helpers.pdfe_session import PDFESession
    from tests.helpers.win32_input import send_click

    fixture = FIXTURES / "test_single.pdf"
    if not fixture.exists():
        pytest.skip("fixture missing")

    with PDFESession(pdf_path=str(fixture)) as session:
        session.wait_for_idle()
        rect = _get_canvas_rect(session)

        # Click upper-left area of the PAGE where annotations often appear.
        # Page-relative, not canvas-relative: the page does not fill the
        # canvas, so `rect.left + 60` can land in the grey margin and the
        # click hits nothing. See docs/audit/ui-test-observability-audit.md.
        #
        # NOTE: this test asserts only that the process is still alive, so it
        # would pass either way. The coordinates are corrected so the comment
        # above is true and so the test exercises what it claims; the weak
        # oracle is a separate gap, recorded in the same audit doc.
        _p = point_on_page(session, 0, 0.25, 0.15)
        assert _p, "page-rect seam unavailable; refusing to guess"
        send_click(*_p)
        time.sleep(0.3)

        assert session.proc.poll() is None


def test_canvas_corner_drag_resize_pattern(tmp_path):
    """SE-CANVAS-2: Corner-drag resize pattern (annotation handle)."""
    from tests.helpers.pdfe_session import PDFESession
    from tests.helpers.win32_input import send_click, send_drag

    fixture = FIXTURES / "test_single.pdf"
    if not fixture.exists():
        pytest.skip("fixture missing")

    with PDFESession(pdf_path=str(fixture)) as session:
        session.wait_for_idle()
        rect = _get_canvas_rect(session)

        # Click center, then drag from a potential handle position
        cx = (rect.left + rect.right) // 2
        cy = (rect.top + rect.bottom) // 2
        send_click(cx, cy)
        time.sleep(0.2)

        # Drag from corner offset (simulates handle drag)
        send_drag(cx + 20, cy + 20, cx + 60, cy + 60, duration=0.3)
        time.sleep(0.2)

        assert session.proc.poll() is None


def test_canvas_text_selection_drag(tmp_path):
    """SE-CANVAS-2: Text selection drag pattern via SendInput."""
    from tests.helpers.pdfe_session import PDFESession
    from tests.helpers.win32_input import send_drag

    fixture = FIXTURES / "test_single.pdf"
    if not fixture.exists():
        pytest.skip("fixture missing")

    with PDFESession(pdf_path=str(fixture)) as session:
        session.wait_for_idle()
        rect = _get_canvas_rect(session)

        # Drag across a text area in the top third of the PAGE. The old form
        # computed this from the CANVAS, so "top third of page" was only true
        # when the page happened to fill the canvas.
        _a = point_on_page(session, 0, 0.12, 0.25)
        _b = point_on_page(session, 0, 0.88, 0.25)
        assert _a and _b, "page-rect seam unavailable; refusing to guess"
        x1, y = _a
        x2, _ = _b

        send_drag(x1, y, x2, y, duration=0.4)
        time.sleep(0.3)

        assert session.proc.poll() is None


def test_canvas_rapid_clicks(tmp_path):
    """SE-CANVAS-2: Rapid click sequence (stress test for event queue)."""
    from tests.helpers.pdfe_session import PDFESession
    from tests.helpers.win32_input import send_click

    fixture = FIXTURES / "test_single.pdf"
    if not fixture.exists():
        pytest.skip("fixture missing")

    with PDFESession(pdf_path=str(fixture)) as session:
        session.wait_for_idle()
        rect = _get_canvas_rect(session)

        cx = (rect.left + rect.right) // 2
        cy = (rect.top + rect.bottom) // 2

        for i in range(10):
            send_click(cx + (i * 5), cy)
            time.sleep(0.05)

        time.sleep(0.3)
        assert session.proc.poll() is None
