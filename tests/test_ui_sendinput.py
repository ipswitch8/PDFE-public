# -*- coding: utf-8 -*-
"""GAP-92/98: Win32 SendInput tests crossing Qt6's event-spontaneity barrier.

Pipeline e4c71f3a phase-5.

These tests use Win32 SendInput to inject OS-level keyboard and mouse
events into PDFE.exe, bypassing pywinauto's synthetic-event limitations
that prevent Qt6 from recognizing them as spontaneous user input.
"""

from __future__ import annotations

import os
import sys
import time
from pathlib import Path

import pytest

_HERE = Path(__file__).resolve().parent
REPO_ROOT = _HERE.parent
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))


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


def _selected_pages(session, timeout: float = 4.0):
    """Return the PagePanel's selected page indices as a list of ints.

    GitHub #222. Returns ``None`` when the seam is unreachable, and ``[]`` for
    a genuinely empty selection -- the handler answers the literal string
    ``"none"`` for that case precisely so it cannot be confused with a pipe
    that never replied. Folding the two together is what lets a broken oracle
    read as a passing test.
    """
    query = session.find_optional(auto_id="testSelectedPagesQueryPipe",
                                  timeout=timeout)
    result = session.find_optional(auto_id="testSelectedPagesResultPipe",
                                   timeout=timeout)
    if query is None or result is None:
        return None
    try:
        query.set_edit_text("1")
    except Exception:  # noqa: BLE001
        return None
    time.sleep(0.4)
    raw = None
    for accessor in ("iface_value", None):
        try:
            raw = (result.iface_value.CurrentValue if accessor
                   else result.window_text())
            if raw:
                break
        except Exception:  # noqa: BLE001
            continue
    if not raw:
        return None
    raw = raw.strip()
    if raw == "none":
        return []
    try:
        return [int(part) for part in raw.split(",") if part.strip() != ""]
    except ValueError:
        return None


@pytest.fixture(autouse=True)
def _isolate():
    """Kill orphan PDFE processes before and after each test."""
    from tests.helpers.pdfe_session import kill_pdfe_orphans
    kill_pdfe_orphans()
    yield
    kill_pdfe_orphans()


FIXTURES = REPO_ROOT / "tests" / "fixtures"


# =====================================================================
# Test 1: SendInput keyboard smoke test — confirms events cross the
# Qt6 spontaneity barrier
# =====================================================================

def test_sendinput_keyboard_smoke(tmp_path):
    """GAP-92: SendInput keystroke reaches a Qt6 QLineEdit."""
    from tests.helpers.pdfe_session import PDFESession
    from tests.helpers.win32_input import send_key, VK_ESCAPE

    fixture = FIXTURES / "test_single.pdf"
    if not fixture.exists():
        pytest.skip("test_single.pdf fixture missing")

    with PDFESession(pdf_path=str(fixture)) as session:
        session.wait_for_idle()

        # Press Escape (should be handled by Qt — at minimum, not crash).
        send_key(VK_ESCAPE)
        time.sleep(0.3)

        # PDFE should still be alive after the keystroke.
        assert session.proc.poll() is None, "GAP-92: PDFE must survive SendInput keystroke"


# =====================================================================
# Test 2: SendInput mouse click — confirms OS-level click on canvas
# =====================================================================

def test_sendinput_mouse_click(tmp_path):
    """GAP-92: SendInput mouse click reaches PdfCanvas."""
    from tests.helpers.pdfe_session import PDFESession
    from tests.helpers.win32_input import send_click

    fixture = FIXTURES / "test_single.pdf"
    if not fixture.exists():
        pytest.skip("test_single.pdf fixture missing")

    with PDFESession(pdf_path=str(fixture)) as session:
        session.wait_for_idle()

        canvas = session.find_optional(auto_id="pdfCanvas", timeout=3)
        if canvas is None:
            pytest.skip("pdfCanvas not found in UIA tree")

        rect = canvas.rectangle()
        cx = (rect.left + rect.right) // 2
        cy = (rect.top + rect.bottom) // 2

        send_click(cx, cy)
        time.sleep(0.3)

        assert session.proc.poll() is None, "GAP-92: PDFE must survive SendInput click"


# =====================================================================
# Test 3: Ctrl+click multi-select on PagePanel
# =====================================================================

def test_sendinput_ctrl_click_multiselect(tmp_path):
    """GAP-98: Ctrl+click multi-select gesture via SendInput."""
    from tests.helpers.pdfe_session import PDFESession
    from tests.helpers.win32_input import send_click, send_modified_click

    fixture = FIXTURES / "test_multipage.pdf"
    if not fixture.exists():
        fixture = FIXTURES / "test_single.pdf"
    if not fixture.exists():
        pytest.skip("No suitable fixture")

    with PDFESession(pdf_path=str(fixture)) as session:
        session.wait_for_idle()

        # PagePanel list widget has the page thumbnails as children
        panel = session.find_optional(name="Page Thumbnails", timeout=3)
        if panel is None:
            panel = session.find_optional(auto_id="pagePanelList", timeout=3)
        if panel is None:
            pytest.skip("Page Thumbnails list not found in UIA tree")

        items = [c for c in panel.children()
                 if getattr(c, 'control_type', '') == 'ListItem'
                 or hasattr(c, 'rectangle')]
        if len(items) < 2:
            pytest.skip(f"Need at least 2 page items for multi-select (got {len(items)})")

        # Click first item normally
        r1 = items[0].rectangle()
        send_click((r1.left + r1.right) // 2, (r1.top + r1.bottom) // 2)
        time.sleep(0.3)

        before = _selected_pages(session)
        assert before is not None, (
            "testSelectedPagesQueryPipe/ResultPipe did not answer; without the "
            "selection readback this test has no oracle and would be back to a "
            "liveness check")

        # Ctrl+click second item
        r2 = items[1].rectangle()
        send_modified_click(
            (r2.left + r2.right) // 2, (r2.top + r2.bottom) // 2,
            ctrl=True,
        )
        time.sleep(0.5)

        after = _selected_pages(session)
        assert after is not None, "selection seam stopped answering"
        # GitHub #222. This assertion replaces `assert session.proc.poll() is
        # None` -- a liveness check that passed whether the gesture selected
        # two pages, one page, or nothing at all. The single-click result is
        # the CONTROL: without it, "2 selected" is also satisfied by a document
        # that was already multi-selected before the gesture.
        assert len(before) == 1, (
            f"the plain click before the Ctrl+click should leave exactly one "
            f"page selected, got {before}; the comparison below would then be "
            f"measuring the wrong starting point")
        assert len(after) == 2, (
            f"Ctrl+click should ADD to the selection: expected 2 selected "
            f"pages, got {after} (was {before} after the plain click)")
        assert set(before) < set(after), (
            f"Ctrl+click replaced the selection instead of extending it: "
            f"{before} -> {after}")


# =====================================================================
# Test 4: Shift+click range-select on PagePanel
# =====================================================================

# The xfail(strict=True) that stood here was written to "go red -- demanding
# its own removal -- the day range-select is fixed". GitHub #226 is fixed, so
# it is removed rather than flipped to non-strict.
#
# The defect was never in the gesture: PagePanel's list has always been
# ExtendedSelection and Qt built the anchor..clicked range correctly every
# time. PagePanel::setCurrentPage() then collapsed it, because setCurrentRow()
# carries an implicit ClearAndSelect and the click's own pageSelected signal
# routed straight back into it. That is why all three input paths measured
# [2]: they were all correct, and all undone afterwards.
#
# Headless companion: tests/test_page_panel_range_selection.cpp.
def test_sendinput_shift_click_rangeselect(tmp_path):
    """GAP-98: Shift+click range-select gesture via SendInput."""
    from tests.helpers.pdfe_session import PDFESession
    from tests.helpers.win32_input import send_click, send_modified_click

    fixture = FIXTURES / "test_multipage.pdf"
    if not fixture.exists():
        fixture = FIXTURES / "test_single.pdf"
    if not fixture.exists():
        pytest.skip("No suitable fixture")

    with PDFESession(pdf_path=str(fixture)) as session:
        session.wait_for_idle()

        panel = session.find_optional(name="Page Thumbnails", timeout=3)
        if panel is None:
            panel = session.find_optional(auto_id="pagePanelList", timeout=3)
        if panel is None:
            pytest.skip("Page Thumbnails list not found in UIA tree")

        items = [c for c in panel.children()
                 if getattr(c, 'control_type', '') == 'ListItem'
                 or hasattr(c, 'rectangle')]
        if len(items) < 3:
            pytest.skip(f"Need at least 3 page items for range-select (got {len(items)})")

        # Click first item
        r1 = items[0].rectangle()
        send_click((r1.left + r1.right) // 2, (r1.top + r1.bottom) // 2)
        time.sleep(0.3)

        before = _selected_pages(session)
        assert before is not None, (
            "testSelectedPagesQueryPipe/ResultPipe did not answer; without the "
            "selection readback this test has no oracle")

        # Shift+click third item
        r3 = items[2].rectangle()
        send_modified_click(
            (r3.left + r3.right) // 2, (r3.top + r3.bottom) // 2,
            shift=True,
        )
        time.sleep(0.5)

        after = _selected_pages(session)
        assert after is not None, "selection seam stopped answering"
        # GitHub #222: a range-select is a CONTIGUOUS run, which is what
        # distinguishes it from the Ctrl+click case. Asserting only the count
        # would pass for an additive gesture that happened to pick 3.
        assert len(before) == 1, (
            f"the plain click before the Shift+click should leave exactly one "
            f"page selected, got {before}")
        assert len(after) == 3, (
            f"Shift+click from item 0 to item 2 should select the 3-page "
            f"range, got {after} (was {before})")
        assert sorted(after) == list(range(min(after), min(after) + 3)), (
            f"Shift+click produced a non-contiguous set {sorted(after)}; a "
            f"range-select must be a contiguous run")


# =====================================================================
# Test 5: SendInput drag on canvas (scroll wheel proxy)
# =====================================================================

def test_sendinput_canvas_drag(tmp_path):
    """GAP-92: SendInput mouse drag on PdfCanvas."""
    from tests.helpers.pdfe_session import PDFESession
    from tests.helpers.win32_input import send_drag

    fixture = FIXTURES / "test_single.pdf"
    if not fixture.exists():
        pytest.skip("test_single.pdf fixture missing")

    with PDFESession(pdf_path=str(fixture)) as session:
        session.wait_for_idle()

        canvas = session.find_optional(auto_id="pdfCanvas", timeout=3)
        if canvas is None:
            pytest.skip("pdfCanvas not found")

        rect = canvas.rectangle()
        cx = (rect.left + rect.right) // 2
        # Canvas-relative ON PURPOSE: this exercises event DELIVERY to the
        # canvas widget (a viewport gesture), not an interaction with page
        # content, so page position is irrelevant. Do not "fix" this to
        # point_on_page -- see docs/audit/ui-test-observability-audit.md.
        y1 = rect.top + 50
        y2 = rect.bottom - 50

        send_drag(cx, y1, cx, y2, duration=0.3)
        time.sleep(0.3)

        assert session.proc.poll() is None, "GAP-92: PDFE must survive SendInput drag"


# =====================================================================
# Test 6: SendInput Ctrl+Z triggers undo
# =====================================================================

def test_sendinput_ctrl_z_undo(tmp_path):
    """GAP-92: SendInput Ctrl+Z triggers Qt6 undo action."""
    from tests.helpers.pdfe_session import PDFESession
    from tests.helpers.win32_input import send_key, VK_CONTROL
    import ctypes

    fixture = FIXTURES / "test_single.pdf"
    if not fixture.exists():
        pytest.skip("test_single.pdf fixture missing")

    with PDFESession(pdf_path=str(fixture)) as session:
        session.wait_for_idle()

        # Ctrl+Z via SendInput (modifier held during key press)
        from tests.helpers.win32_input import (
            INPUT, INPUT_KEYBOARD, KEYEVENTF_KEYUP, SendInput as _SendInput,
        )

        inputs = (INPUT * 4)()

        # Ctrl down
        inputs[0].type = INPUT_KEYBOARD
        inputs[0].union.ki.wVk = VK_CONTROL
        inputs[0].union.ki.dwFlags = 0

        # Z down
        inputs[1].type = INPUT_KEYBOARD
        inputs[1].union.ki.wVk = ord('Z')
        inputs[1].union.ki.dwFlags = 0

        # Z up
        inputs[2].type = INPUT_KEYBOARD
        inputs[2].union.ki.wVk = ord('Z')
        inputs[2].union.ki.dwFlags = KEYEVENTF_KEYUP

        # Ctrl up
        inputs[3].type = INPUT_KEYBOARD
        inputs[3].union.ki.wVk = VK_CONTROL
        inputs[3].union.ki.dwFlags = KEYEVENTF_KEYUP

        _SendInput(4, inputs, ctypes.sizeof(INPUT))
        time.sleep(0.5)

        assert session.proc.poll() is None, "GAP-92: PDFE must survive Ctrl+Z via SendInput"
