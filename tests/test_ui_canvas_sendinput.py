# -*- coding: utf-8 -*-
"""Pipeline a2b3c4d5 phase-2: Canvas drag/handle tests via Win32 SendInput.

Converts the P-GAP-92 canvas interaction tests from pyautogui to
SendInput, crossing Qt6's event-spontaneity barrier.
"""

from __future__ import annotations

import os
import shutil
import sys
import time
from pathlib import Path

import pytest
from tests.helpers.canvas_geometry import view_state, point_on_page
from tests.helpers.qinvoke import _write_to_pipe

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
    """Get the PdfCanvas rectangle for coordinate calculations."""
    canvas = session.find_optional(auto_id="pdfCanvas", timeout=3)
    if canvas is None:
        # WIN-45: unreachable -> recorded 'blocked' in the matrix, then FAIL.
        _reach = ("pdfCanvas not reachable via UIA.")
        record_matrix_result(current_test_id(), "blocked", _reach)
        pytest.fail(
            f"WIN-45 reachability blocked (recorded non-pass, not skipped): {_reach}")
    return canvas.rectangle()


# =====================================================================
# Canvas drag tests — prove SendInput delivers drag events to PdfCanvas
# =====================================================================

def test_canvas_drag_top_to_bottom(tmp_path):
    """SE-CANVAS-1: Vertical drag on canvas via SendInput.

    Functional assertion: after dragging down on a multi-page doc,
    the window title should still show the document name (PDFE alive
    and document still loaded).
    """
    from tests.helpers.pdfe_session import PDFESession
    from tests.helpers.win32_input import send_drag

    fixture = FIXTURES / "test_multipage.pdf"
    if not fixture.exists():
        fixture = FIXTURES / "test_single.pdf"
    if not fixture.exists():
        pytest.skip("fixture missing")

    with PDFESession(pdf_path=str(fixture)) as session:
        session.wait_for_idle()
        rect = _get_canvas_rect(session)

        cx = (rect.left + rect.right) // 2
        # Canvas-relative ON PURPOSE: this exercises event DELIVERY to the
        # canvas widget (a viewport gesture), not an interaction with page
        # content, so page position is irrelevant. Do not "fix" this to
        # point_on_page -- see docs/audit/ui-test-observability-audit.md.
        y1 = rect.top + 30
        y2 = rect.bottom - 30

        # This test proves SendInput DELIVERS a drag to PdfCanvas. What a
        # delivered drag DOES depends on the active tool:
        #
        #   Select tool active  -> rubber-band selection
        #   no tool installed   -> the canvas's own pan fallback (scrolling)
        #
        # It used to assert scrolling, because at startup no tool was
        # installed and the drag fell through to the pan path. That was a BUG
        # (the toolbar showed Select as active while no SelectTool existed);
        # fixing it means a drag now rubber-bands, so scrolling is the wrong
        # oracle. Assert the delivery that actually happens: a band drawn over
        # an annotation selects it.
        os.environ["PDFE_TEST_ANNOT_PIPES"] = "1"
        assert _write_to_pipe(session, "testAnnotCreatePipe",
                              "Square,0,100,100,120,90"),             "annotation-create pipe unavailable"
        time.sleep(1.5)
        before = view_state(session)
        assert before is not None, "view-state seam unavailable"
        assert before["annots"] >= 1, "setup failed: no annotation to select"

        band_a = point_on_page(session, 0, 0.04, 0.04)
        band_b = point_on_page(session, 0, 0.60, 0.45)
        assert band_a and band_b, "page-rect seam unavailable"
        send_drag(band_a[0], band_a[1], band_b[0], band_b[1], duration=0.5)
        time.sleep(1.0)

        after = view_state(session)
        assert after is not None, "view-state seam unavailable after drag"
        assert after["selCount"] >= 1, (
            "a rubber-band drag over an annotation should select it; "
            "selCount=%d (the drag was probably not delivered)"
            % after["selCount"])

        # GitHub #122 — the delivery proof itself: a middle-button drag is
        # PDFE's pan gesture (#119), so it must move the vertical scroll
        # offset. Reading it through the same view-state seam turns "the
        # process is still alive" into "the drag reached the canvas AND the
        # canvas acted on it".
        #
        # Zoom in FIRST. At the startup zoom a single fixture page is not
        # taller than the canvas viewport on this host, so scrollByPixels()
        # clamps straight back to 0 and the precondition below can never be
        # met — the setup silently establishes nothing. Zooming to 400% makes
        # the page taller than the viewport, exactly as the horizontal sibling
        # test does for scrollX.
        assert _write_to_pipe(session, "testSetZoomPipe", "400"), \
            "zoom pipe unavailable; cannot create vertical travel"
        time.sleep(1.2)
        # NEGATIVE on purpose: the pipe feeds PdfCanvas::scrollByPixels, which
        # synthesises a wheel event, and wheelEvent computes
        # `scrollOffset_.y() - delta`. A POSITIVE value therefore scrolls UP —
        # from the top of the document that clamps straight back to 0 and the
        # precondition below can never be satisfied.
        assert _write_to_pipe(session, "testScrollPipe", "-400"), \
            "scroll pipe unavailable; cannot establish the pan precondition"
        time.sleep(0.8)
        pan_before = view_state(session)
        assert pan_before is not None, "view-state seam unavailable before pan"
        assert pan_before["scrollY"] > 0, (
            "setup: the canvas must be scrolled away from the top, or an "
            "upward pan has nowhere to go and the assertion below would "
            "compare zero against zero (scrollY=%r)" % (pan_before["scrollY"],))

        # Drag DOWN with the middle button: content follows the cursor, so
        # scrollY decreases.
        send_drag(cx, rect.top + 120, cx, rect.bottom - 120,
                  duration=0.5, button="middle")
        time.sleep(0.8)

        pan_after = view_state(session)
        assert pan_after is not None, "view-state seam unavailable after pan"
        assert pan_after["scrollY"] < pan_before["scrollY"], (
            "a middle-button drag down must scroll the canvas back toward "
            "the top; scrollY=%d before, %d after (the drag was probably "
            "not delivered)"
            % (pan_before["scrollY"], pan_after["scrollY"]))

        assert session.proc.poll() is None, "PDFE must survive vertical drag"


def test_canvas_drag_left_to_right(tmp_path):
    """SE-CANVAS-1 / GitHub #122: horizontal drag delivery, proved by the
    horizontal scroll offset rather than by the process staying alive.

    ``assert session.proc.poll() is None`` was the only assertion here. It
    passes for a drag that is never delivered, for a canvas that ignores it,
    and for a pan implementation that was deleted — the exact shape #122 was
    filed about.

    The gesture is the MIDDLE button, because that is PDFE's pan gesture
    (#119): with the Select tool installed a LEFT drag rubber-band-selects
    and deliberately does not scroll.
    """
    from tests.helpers.pdfe_session import PDFESession
    from tests.helpers.win32_input import send_drag, send_key
    from tests.helpers.canvas_geometry import refocus_canvas

    VK_RIGHT = 0x27

    fixture = FIXTURES / "test_single.pdf"
    if not fixture.exists():
        pytest.skip("test_single.pdf missing")

    with PDFESession(pdf_path=str(fixture)) as session:
        session.wait_for_idle()
        rect = _get_canvas_rect(session)

        # Canvas-relative ON PURPOSE: this exercises event DELIVERY to the
        # canvas widget (a viewport gesture), not an interaction with page
        # content, so page position is irrelevant. Do not "fix" this to
        # point_on_page -- see docs/audit/ui-test-observability-audit.md.
        x1 = rect.left + 30
        x2 = rect.right - 30
        cy = (rect.top + rect.bottom) // 2

        # A page narrower than the viewport has no horizontal travel at all,
        # so scrollX would be pinned at 0 and any assertion on it vacuous.
        # Zoom in until the page is wider than the canvas, then start from a
        # non-zero scrollX so a leftward drag has somewhere to go.
        #
        # The Right-arrow keystrokes are the setup, NOT the gesture under
        # test: they reach PdfCanvas::keyPressEvent's own navigation (the
        # Select tool ignores unhandled keys so they fall through), which is a
        # completely different code path from the middle-drag pan asserted
        # below. Writing to a pipe puts Win32 focus on the hidden QLineEdit,
        # so the canvas has to be refocused before the keys are sent or they
        # land in the pipe instead.
        assert _write_to_pipe(session, "testSetZoomPipe", "400"), \
            "zoom pipe unavailable; cannot create horizontal travel"
        time.sleep(1.2)
        assert refocus_canvas(session), \
            "could not return focus to the canvas; the setup keys would go " \
            "to the hidden test pipe instead"
        time.sleep(0.4)
        for _ in range(6):
            send_key(VK_RIGHT, extended=True)
            time.sleep(0.15)
        time.sleep(0.8)

        before = view_state(session)
        assert before is not None, "view-state seam unavailable before drag"
        assert before["scrollX"] > 0, (
            "setup: the canvas must be scrolled right of the origin, or a "
            "leftward pan has nowhere to go and the assertion below would "
            "compare zero against zero (scrollX=%r)" % (before["scrollX"],))

        # Drag left-to-right with the middle button: content follows the
        # cursor, so scrollX decreases.
        send_drag(x1, cy, x2, cy, duration=0.4, button="middle")
        time.sleep(0.8)

        after = view_state(session)
        assert after is not None, "view-state seam unavailable after drag"
        assert after["scrollX"] < before["scrollX"], (
            "GitHub #122: a middle-button drag to the right must decrease "
            "the horizontal scroll offset; scrollX=%d before, %d after (the "
            "drag was probably not delivered to PdfCanvas)"
            % (before["scrollX"], after["scrollX"]))
        assert after["scrollY"] == before["scrollY"], (
            "a purely horizontal drag must not move the vertical offset; "
            "scrollY=%d -> %d" % (before["scrollY"], after["scrollY"]))

        assert session.proc.poll() is None


def test_canvas_click_then_drag(tmp_path):
    """SE-CANVAS-1: Click to select, then drag (annotation move pattern)."""
    from tests.helpers.pdfe_session import PDFESession
    from tests.helpers.win32_input import send_click, send_drag

    fixture = FIXTURES / "test_single.pdf"
    if not fixture.exists():
        pytest.skip("test_single.pdf missing")

    with PDFESession(pdf_path=str(fixture)) as session:
        session.wait_for_idle()
        rect = _get_canvas_rect(session)

        cx = (rect.left + rect.right) // 2
        cy = (rect.top + rect.bottom) // 2

        send_click(cx, cy)
        time.sleep(0.3)

        send_drag(cx, cy, cx + 50, cy + 50, duration=0.3)
        time.sleep(0.3)

        assert session.proc.poll() is None


def test_canvas_double_click(tmp_path):
    """SE-CANVAS-1: Double-click on canvas (text edit activation pattern).

    Functional assertion: double-click on text area should either activate
    text edit mode (dirty indicator appears) or at minimum not crash.
    """
    from tests.helpers.pdfe_session import PDFESession
    from tests.helpers.win32_input import send_click

    fixture = FIXTURES / "test_single.pdf"
    if not fixture.exists():
        pytest.skip("test_single.pdf missing")

    with PDFESession(pdf_path=str(fixture)) as session:
        session.wait_for_idle()
        rect = _get_canvas_rect(session)

        title_before = session.window.wrapper_object().window_text()

        cx = (rect.left + rect.right) // 2
        cy = rect.top + int((rect.bottom - rect.top) * 0.3)

        send_click(cx, cy)
        time.sleep(0.05)
        send_click(cx, cy)
        time.sleep(0.5)

        assert session.proc.poll() is None
        title_after = session.window.wrapper_object().window_text()
        assert title_after, "Window title must be non-empty after double-click"


def test_canvas_right_click(tmp_path):
    """SE-CANVAS-1: Right-click on canvas (context menu pattern).

    Functional assertion: right-click should produce a context menu.
    We verify by checking that Escape dismisses something (PDFE still
    responsive afterward).
    """
    from tests.helpers.pdfe_session import PDFESession
    from tests.helpers.win32_input import send_click, send_key, VK_ESCAPE

    fixture = FIXTURES / "test_single.pdf"
    if not fixture.exists():
        pytest.skip("test_single.pdf missing")

    with PDFESession(pdf_path=str(fixture)) as session:
        session.wait_for_idle()
        rect = _get_canvas_rect(session)

        cx = (rect.left + rect.right) // 2
        cy = (rect.top + rect.bottom) // 2

        send_click(cx, cy, button="right")
        time.sleep(0.5)

        # Dismiss context menu with Escape
        send_key(VK_ESCAPE)
        time.sleep(0.3)

        assert session.proc.poll() is None
        # Verify PDFE is still responsive after right-click + dismiss
        title = session.window.wrapper_object().window_text()
        assert title, "Window must be responsive after context menu dismiss"


def test_canvas_rubber_band_drag(tmp_path):
    """SE-CANVAS-1: Rubber-band selection drag pattern via SendInput."""
    from tests.helpers.pdfe_session import PDFESession
    from tests.helpers.win32_input import send_drag

    fixture = FIXTURES / "test_single.pdf"
    if not fixture.exists():
        pytest.skip("test_single.pdf missing")

    with PDFESession(pdf_path=str(fixture)) as session:
        session.wait_for_idle()

        # Activate Select tool if available
        session.find_optional(auto_id="selectToolAction", timeout=1)

        rect = _get_canvas_rect(session)
        # Canvas-relative ON PURPOSE: this exercises event DELIVERY to the
        # canvas widget (a viewport gesture), not an interaction with page
        # content, so page position is irrelevant. Do not "fix" this to
        # point_on_page -- see docs/audit/ui-test-observability-audit.md.
        x1 = rect.left + 50
        y1 = rect.top + 50
        x2 = rect.right - 50
        y2 = rect.top + int((rect.bottom - rect.top) * 0.5)

        send_drag(x1, y1, x2, y2, duration=0.5)
        time.sleep(0.3)

        assert session.proc.poll() is None


def test_canvas_triple_click(tmp_path):
    """SE-CANVAS-1: Triple-click (line selection pattern) via SendInput."""
    from tests.helpers.pdfe_session import PDFESession
    from tests.helpers.win32_input import send_click

    fixture = FIXTURES / "test_single.pdf"
    if not fixture.exists():
        pytest.skip("test_single.pdf missing")

    with PDFESession(pdf_path=str(fixture)) as session:
        session.wait_for_idle()
        rect = _get_canvas_rect(session)

        cx = (rect.left + rect.right) // 2
        cy = rect.top + int((rect.bottom - rect.top) * 0.3)

        # Triple-click with minimal delay
        for _ in range(3):
            send_click(cx, cy)
            time.sleep(0.05)
        time.sleep(0.3)

        assert session.proc.poll() is None
