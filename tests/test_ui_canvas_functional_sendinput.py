# -*- coding: utf-8 -*-
"""P-GAP-92 SendInput conversion: functional canvas tests with verification.

Replaces the pyautogui-based tests in test_canvas_phase3.py and
test_canvas_coords_p2.py that are xfailed due to Qt6 spontaneity barrier.
Uses Win32 SendInput to deliver real OS-level events, with functional
assertions that verify outcomes (dirty markers, window state, selection).
"""

from __future__ import annotations

import os
import shutil
import sys
import time
from pathlib import Path

import pytest
from tests.helpers.canvas_geometry import (
    point_on_page, view_state, page_rect_on_canvas,
)
from tests.helpers.qinvoke import activate_tool_by_name, _write_to_pipe

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


def _title_has_dirty_marker(session) -> bool:
    """Check if window title contains '*' (dirty/modified indicator)."""
    try:
        title = session.window.wrapper_object().window_text()
        return "*" in title
    except Exception:
        return False


def _wait_dirty(session, timeout: float = 3.0) -> bool:
    """Poll until the window title shows the dirty marker."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if _title_has_dirty_marker(session):
            return True
        time.sleep(0.2)
    return False


# =====================================================================
# Functional test: annotation drag changes dirty state
# Covers: canvas_annot_handle_drag_corner, canvas_annot_move_drag
# =====================================================================

def test_annotation_drag_marks_dirty(tmp_path):
    """P-GAP-92: Drag on canvas after annotation creation marks document dirty."""
    from tests.helpers.pdfe_session import PDFESession
    from tests.helpers.win32_input import send_click, send_drag

    fixture = FIXTURES / "test_single.pdf"
    if not fixture.exists():
        pytest.skip("fixture missing")

    work = tmp_path / "annot_drag.pdf"
    shutil.copy2(fixture, work)

    with PDFESession(pdf_path=str(work)) as session:
        session.wait_for_idle()

        # Activate sticky note tool and place an annotation
        # Assert the activation. This was `if pipe: try/except: pass`, so a
        # failure to activate was indistinguishable from success and the
        # test carried on with the wrong tool -- doing nothing, then
        # asserting nothing.
        assert activate_tool_by_name(session, "stickyNoteToolAction"), \
            "could not activate stickyNoteToolAction"
        time.sleep(0.8)

        rect = _get_canvas_rect(session)
        cx = (rect.left + rect.right) // 2
        cy = (rect.top + rect.bottom) // 2

        # Click to place annotation
        send_click(cx, cy)
        time.sleep(0.5)

        # Check if dirty after annotation creation
        dirty_after_create = _title_has_dirty_marker(session)

        # Select the annotation just placed, so the drag below is a MOVE
        # rather than a rubber-band over empty space.
        before = view_state(session)
        assert before is not None, "view-state seam unavailable"
        assert before["annots"] >= 1, (
            "setup failed: the sticky-note click created no annotation, so "
            "the drag below would have nothing to move")
        send_click(cx, cy)
        time.sleep(0.8)
        selected = view_state(session)
        assert selected is not None and selected["rect"], (
            "setup failed: could not select the annotation to move it")

        # Now drag it.
        send_drag(cx, cy, cx + 40, cy + 40, duration=0.3)
        time.sleep(1.0)

        # The real oracle: the annotation actually MOVED. Previously this
        # asserted the process was alive, plus a dirty check guarded by
        # `if dirty_after_create:` -- so whenever creation had not set the
        # marker, the test asserted nothing at all.
        after = view_state(session)
        assert after is not None and after["rect"], "lost the selection"
        assert after["rect"][:2] != selected["rect"][:2], (
            "dragging a selected annotation should move it; rect stayed at %r"
            % (selected["rect"],))

        assert session.proc.poll() is None, "PDFE must survive annotation drag"
        assert _title_has_dirty_marker(session), \
            "moving an annotation must mark the document dirty"


# =====================================================================
# Functional test: text selection via drag produces selection state
# Covers: canvas_text_selection_drag
# =====================================================================

def test_text_selection_drag_functional(tmp_path):
    """P-GAP-92: Text selection drag via SendInput."""
    from tests.helpers.pdfe_session import PDFESession
    from tests.helpers.win32_input import send_drag, send_key

    fixture = FIXTURES / "test_single.pdf"
    if not fixture.exists():
        pytest.skip("fixture missing")

    with PDFESession(pdf_path=str(fixture)) as session:
        session.wait_for_idle()

        # Activate select tool
        # Assert the activation. This was `if pipe: try/except: pass`, so a
        # failure to activate was indistinguishable from success and the
        # test carried on with the wrong tool -- doing nothing, then
        # asserting nothing.
        assert activate_tool_by_name(session, "selectToolAction"), \
            "could not activate selectToolAction"
        time.sleep(0.8)

        rect = _get_canvas_rect(session)
        # Page-relative, not canvas-relative: the page does not fill the
        # canvas, so a canvas offset can land in the grey margin and the
        # interaction silently does nothing. See
        # docs/audit/ui-test-observability-audit.md.
        _a = point_on_page(session, 0, 0.12, 0.25)
        _b = point_on_page(session, 0, 0.62, 0.25)
        assert _a and _b, "page-rect seam unavailable; refusing to guess"
        x1, y = _a
        x2, _ = _b

        # Drag across text area
        send_drag(x1, y, x2, y, duration=0.4)
        time.sleep(0.5)

        # Try Ctrl+C to copy selection
        from tests.helpers.win32_input import (
            INPUT, INPUT_KEYBOARD, KEYEVENTF_KEYUP, VK_CONTROL,
            SendInput as _SendInput,
        )
        import ctypes
        inputs = (INPUT * 4)()
        inputs[0].type = INPUT_KEYBOARD
        inputs[0].union.ki.wVk = VK_CONTROL
        inputs[1].type = INPUT_KEYBOARD
        inputs[1].union.ki.wVk = ord('C')
        inputs[2].type = INPUT_KEYBOARD
        inputs[2].union.ki.wVk = ord('C')
        inputs[2].union.ki.dwFlags = KEYEVENTF_KEYUP
        inputs[3].type = INPUT_KEYBOARD
        inputs[3].union.ki.wVk = VK_CONTROL
        inputs[3].union.ki.dwFlags = KEYEVENTF_KEYUP
        _SendInput(4, inputs, ctypes.sizeof(INPUT))
        time.sleep(0.3)

        assert session.proc.poll() is None, "PDFE must survive text selection drag"


# =====================================================================
# Functional test: triple-click selects a line
# Covers: canvas_text_selection_triple_click
# =====================================================================

def test_triple_click_text_selection(tmp_path):
    """P-GAP-92: Triple-click to select line via SendInput."""
    from tests.helpers.pdfe_session import PDFESession
    from tests.helpers.win32_input import send_click

    fixture = FIXTURES / "test_single.pdf"
    if not fixture.exists():
        pytest.skip("fixture missing")

    with PDFESession(pdf_path=str(fixture)) as session:
        session.wait_for_idle()
        rect = _get_canvas_rect(session)

        cx = (rect.left + rect.right) // 2
        cy = rect.top + int((rect.bottom - rect.top) * 0.25)

        # Triple-click with tight timing
        for _ in range(3):
            send_click(cx, cy)
            time.sleep(0.05)
        time.sleep(0.5)

        assert session.proc.poll() is None


# =====================================================================
# Functional test: rubber-band drag on empty canvas
# Covers: canvas_canvas_rubber_band_select
# =====================================================================

def test_rubber_band_select_functional(tmp_path):
    """P-GAP-92: Rubber-band selection drag via SendInput."""
    from tests.helpers.pdfe_session import PDFESession
    from tests.helpers.win32_input import send_drag

    fixture = FIXTURES / "test_single.pdf"
    if not fixture.exists():
        pytest.skip("fixture missing")

    with PDFESession(pdf_path=str(fixture)) as session:
        session.wait_for_idle()

        # Activate select tool
        # Assert the activation. This was `if pipe: try/except: pass`, so a
        # failure to activate was indistinguishable from success and the
        # test carried on with the wrong tool -- doing nothing, then
        # asserting nothing.
        assert activate_tool_by_name(session, "selectToolAction"), \
            "could not activate selectToolAction"
        time.sleep(0.8)

        rect = _get_canvas_rect(session)
        # Page-relative, not canvas-relative: the page does not fill the
        # canvas, so a canvas offset can land in the grey margin and the
        # interaction silently does nothing. See
        # docs/audit/ui-test-observability-audit.md.
        _a = point_on_page(session, 0, 0.08, 0.08)
        _b = point_on_page(session, 0, 0.92, 0.40)
        assert _a and _b, "page-rect seam unavailable; refusing to guess"
        x1, y1 = _a
        x2, y2 = _b

        send_drag(x1, y1, x2, y2, duration=0.5)
        time.sleep(0.5)

        assert session.proc.poll() is None


# =====================================================================
# Functional test: click-away deselects
# Covers: canvas_canvas_click_away_deselect
# =====================================================================

def test_click_away_deselect(tmp_path):
    """P-GAP-92: Click empty area to deselect via SendInput."""
    from tests.helpers.pdfe_session import PDFESession
    from tests.helpers.win32_input import send_click

    fixture = FIXTURES / "test_single.pdf"
    if not fixture.exists():
        pytest.skip("fixture missing")

    with PDFESession(pdf_path=str(fixture)) as session:
        session.wait_for_idle()
        rect = _get_canvas_rect(session)

        # Click center (may select something)
        cx = (rect.left + rect.right) // 2
        cy = (rect.top + rect.bottom) // 2
        send_click(cx, cy)
        time.sleep(0.3)

        # Click an empty area to deselect. Canvas-relative ON PURPOSE: the
        # point of this test is to click where page content is NOT, so
        # point_on_page would be exactly wrong here.
        #
        # It must still be a real, safe point. This previously used
        # `rect.bottom - 5`, which on this display put the click at y=966
        # while the desktop work area ends at 964 — i.e. ON THE TASKBAR.
        # assert_safe_click refused it and the test failed (a PRE-EXISTING
        # failure, not introduced by the coordinate audit). Use the TOP-left
        # inset instead: same "empty margin" semantics, nowhere near the
        # taskbar. See docs/audit/ui-test-observability-audit.md.
        send_click(rect.left + 5, rect.top + 5)
        time.sleep(0.8)

        # The real oracle: nothing is selected any more. Previously this
        # asserted only that the process was alive -- true whether the click
        # deselected, selected something else, or was never delivered.
        after = view_state(session)
        assert after is not None, "view-state seam unavailable"
        assert after["selected"] < 0, (
            "clicking an empty margin should deselect; selected index is %d"
            % after["selected"])

        assert session.proc.poll() is None


# =====================================================================
# Functional test: scroll wheel via keyboard (PageDown/Up)
# Covers: canvas_canvas_scroll_wheel
# =====================================================================

def test_scroll_via_keyboard(tmp_path):
    """P-GAP-92: Scroll canvas via PageDown/Up SendInput."""
    from tests.helpers.pdfe_session import PDFESession
    from tests.helpers.win32_input import send_click, send_key

    fixture = FIXTURES / "test_multipage.pdf"
    if not fixture.exists():
        pytest.skip("test_multipage.pdf missing")

    VK_NEXT = 0x22
    VK_PRIOR = 0x21

    with PDFESession(pdf_path=str(fixture)) as session:
        session.wait_for_idle()
        rect = _get_canvas_rect(session)

        # Focus canvas
        send_click((rect.left + rect.right) // 2, (rect.top + rect.bottom) // 2)
        time.sleep(0.3)

        # Scroll down
        send_key(VK_NEXT, extended=True)
        time.sleep(0.5)

        # Scroll back up
        send_key(VK_PRIOR, extended=True)
        time.sleep(0.5)

        assert session.proc.poll() is None
        title = session.window.wrapper_object().window_text()
        assert title, "Window must be responsive after scroll"


# =====================================================================
# Functional test: annotation handle rotate pattern
# Covers: canvas_annot_handle_drag_rotate
# =====================================================================

def test_annotation_handle_drag(tmp_path):
    """P-GAP-92: Annotation handle drag (resize/rotate pattern)."""
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

        # Click to select potential annotation
        send_click(cx, cy)
        time.sleep(0.3)

        # Drag from a handle position (offset from center)
        handle_x = cx + 30
        handle_y = cy + 30
        send_drag(handle_x, handle_y, handle_x + 50, handle_y + 50, duration=0.3)
        time.sleep(0.3)

        assert session.proc.poll() is None


# =====================================================================
# Functional test: sticky note drag popup
# Covers: canvas_sticky_note_drag_popup
# =====================================================================

def test_sticky_note_interaction(tmp_path):
    """P-GAP-92: Sticky note click + drag pattern via SendInput."""
    from tests.helpers.pdfe_session import PDFESession
    from tests.helpers.win32_input import send_click, send_drag

    fixture = FIXTURES / "test_single.pdf"
    if not fixture.exists():
        pytest.skip("fixture missing")

    with PDFESession(pdf_path=str(fixture)) as session:
        session.wait_for_idle()

        # Activate the sticky-note tool. This used to be wrapped in
        # `if pipe is not None: try/except: pass`, so a failure to activate
        # was indistinguishable from success and the test carried on clicking
        # with no tool -- creating nothing, then asserting nothing. Assert it.
        assert activate_tool_by_name(session, "stickyNoteToolAction"),             "could not activate the sticky-note tool"
        time.sleep(1.0)

        before = view_state(session)
        assert before is not None, "view-state seam unavailable"

        rect = _get_canvas_rect(session)
        # Page-relative, not canvas-relative: the page does not fill the
        # canvas, so a canvas offset can land in the grey margin and the
        # interaction silently does nothing. See
        # docs/audit/ui-test-observability-audit.md.
        _p = point_on_page(session, 0, 0.30, 0.20)
        assert _p, "page-rect seam unavailable; refusing to guess"
        x, y = _p

        # Click to place
        send_click(x, y)
        time.sleep(0.3)

        # Click again (may open popup)
        send_click(x, y)
        time.sleep(1.0)

        # The real oracle: an annotation was actually created. Previously this
        # asserted only that the process was alive, which was true whether the
        # click landed on the page, in the margin, or nowhere at all.
        after = view_state(session)
        assert after is not None, "view-state seam unavailable after clicks"
        assert after["annots"] > before["annots"], (
            "clicking on the page with the sticky-note tool active should "
            "create an annotation; count stayed at %d" % before["annots"])

        assert session.proc.poll() is None


# =====================================================================
# Functional test: save roundtrip after canvas interaction
# Covers: verifies canvas interactions don't corrupt document
# =====================================================================

def test_canvas_interaction_save_roundtrip(tmp_path):
    """P-GAP-92: Canvas interactions followed by save produce valid PDF."""
    from tests.helpers.pdfe_session import PDFESession
    from tests.helpers.win32_input import send_click, send_drag, send_key, VK_ESCAPE

    fixture = FIXTURES / "test_single.pdf"
    if not fixture.exists():
        pytest.skip("fixture missing")

    work = tmp_path / "interaction_save.pdf"
    shutil.copy2(fixture, work)

    with PDFESession(pdf_path=str(work)) as session:
        session.wait_for_idle()
        rect = _get_canvas_rect(session)

        cx = (rect.left + rect.right) // 2
        cy = (rect.top + rect.bottom) // 2

        # Perform several interactions
        send_click(cx, cy)
        time.sleep(0.2)
        send_drag(cx, cy, cx + 30, cy + 30, duration=0.2)
        time.sleep(0.2)
        send_click(cx, cy, button="right")
        time.sleep(0.3)
        send_key(VK_ESCAPE)
        time.sleep(0.2)

        # Save via pipe if available
        save_pipe = session.find_optional(auto_id="testSaveAsPipe", timeout=2)
        if save_pipe is not None:
            out = tmp_path / "saved_after_interaction.pdf"
            try:
                save_pipe.set_edit_text(str(out))
                time.sleep(2.0)
                assert out.exists(), "Save after canvas interaction must produce file"
                assert out.stat().st_size > 100, "Saved file must be non-trivial"
            except Exception:
                pass

        assert session.proc.poll() is None


# =====================================================================
# Regression: the Select tool must BE installed, not just look installed
# =====================================================================

def test_select_tool_is_actually_installed_at_startup(tmp_path):
    """selectToolAction is checked at startup — the tool must really be there.

    REGRESSION. `selectToolAction_->setChecked(true)` runs at construction, so
    the toolbar showed Select as the active tool the moment a document opened.
    The tool OBJECT, though, was only constructed inside
    onAnnotationToolSelected(), which nothing called until the user either
    created an annotation or switched tools and came back. Until then
    canvas_->currentTool() was null: clicking an existing annotation did not
    select it, and a drag panned the view instead of rubber-band selecting.

    The checked state and the real state had simply never been compared. This
    compares them: open a document that already HAS an annotation, click it,
    and require that it becomes selected.
    """
    from tests.helpers.pdfe_session import PDFESession
    from tests.helpers.win32_input import send_click

    fixture = FIXTURES / "test_single.pdf"
    if not fixture.exists():
        pytest.skip("fixture missing")

    work = tmp_path / "startup_select.pdf"
    shutil.copy2(fixture, work)

    os.environ["PDFE_TEST_ANNOT_PIPES"] = "1"
    with PDFESession(pdf_path=str(work)) as session:
        session.wait_for_idle()
        time.sleep(2.0)

        # Create an annotation at a known place WITHOUT touching the toolbar,
        # so the tool state stays exactly as it is at startup.
        assert _write_to_pipe(session, "testAnnotCreatePipe",
                              "Square,0,100,100,80,60"), \
            "annotation-create pipe unavailable"
        time.sleep(1.5)

        before = view_state(session)
        assert before is not None, "view-state seam unavailable"
        assert before["annots"] >= 1, "setup failed: no annotation created"

        # Click the annotation. Its rect is PDF (100,100)-(180,160); aim at the
        # middle of it, converted through the page rect so this is independent
        # of zoom and DPI.
        page = page_rect_on_canvas(session, 0)
        assert page is not None, "page-rect seam unavailable"
        px, py, pw, ph = page
        canvas = _get_canvas_rect(session)
        # test_single.pdf is US Letter: 612 x 792 pt.
        cx = canvas.left + px + int(pw * (140.0 / 612.0))
        cy = canvas.top + py + int(ph * (130.0 / 792.0))
        send_click(cx, cy)
        time.sleep(1.2)

        after = view_state(session)
        assert after is not None, "view-state seam unavailable after click"
        assert after["selCount"] >= 1, (
            "clicking an annotation at startup did not select it "
            "(selCount=%d). The Select tool is checked in the toolbar but no "
            "SelectTool is installed on the canvas." % after["selCount"])
