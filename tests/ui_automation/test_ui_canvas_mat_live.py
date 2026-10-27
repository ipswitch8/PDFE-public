# -*- coding: utf-8 -*-
"""Live-UI coverage for the last seven ``planned`` rows of the UI matrix.

Pipeline p86-followups phase-5 (second half). These rows
(``sendinput_drag``, ``canvas_drag_interaction``, ``canvas_zoom_pinch``,
``canvas_double_click_text``, ``canvas_right_click_context``,
``canvas_keyboard_nav``, ``search_navigate``) were the entire remainder of
the matrix: 739 green, 7 planned, nothing else outstanding.

WHAT THIS FILE REFUSES TO DO
============================

**No did-not-crash oracles.** The ``sendinput_drag`` row shipped with
``expected = "PDFE survives"``. That is the permanently-false-guard shape:
``session.proc.poll() is None`` is true whether the drag landed on the page,
landed in the grey margin, or was never delivered at all. Phase 4 rewrote
``font_substitute`` for exactly this and phase 5a rewrote ``render_zoom``.
Every test here asserts a VALUE THAT CHANGED, and wherever possible asserts
it against the INPUT rather than against another output:

    sendinput_drag            the created annotation's PDF rect matches the
                              rectangle we dragged (not merely "an annotation
                              appeared")
    canvas_drag_interaction   scrollX/scrollY moved by the distance dragged
    canvas_zoom_pinch         the zoom percentage rose, then fell back
    canvas_double_click_text  the selected substring EQUALS the word fitz
                              says is at that coordinate
    canvas_right_click_context a popup window appeared AND driving its first
                              item changed the zoom
    canvas_keyboard_nav       the page index advanced and retreated
    search_navigate           the canvas went to the page each result's own
                              label names

**No UIA modal polling.** The canvas context menu is a ``QMenu::exec()``
nested loop, which is the same thing pywinauto's UIA backend cannot
enumerate (a dialog once sat open for four minutes while a 240 s UIA poll
reported nothing). It is detected here with raw Win32 ``EnumWindows`` via
``tests/helpers/modal_dialog.py`` and driven with SendInput keystrokes. No
UIA call is made while the popup is up -- that is also the GAP-A14 menu
hang.

**No guessed canvas coordinates.** Every on-page point comes from
``canvas_geometry.point_on_page``, and ``_point_on_page_visible`` FAILS
rather than falling back when the seam does not answer. It additionally
fails when the computed point is outside the canvas rect: that is a real
condition, not a theoretical one. A probe run for this phase called
``point_on_page`` while the view was left zoomed to 332% and got back
y = 12 against a canvas whose top edge was y = 86 -- the point was correct
ON THE PAGE and simply scrolled out of view, so the click would have landed
on the toolbar and the test would have reported "the double-click did
nothing".

The two drags are the documented viewport-gesture exception ONLY where they
say so: ``canvas_drag_interaction`` pans from the canvas widget centre
because a pan is about the viewport, not about where the page is.
``sendinput_drag`` is NOT that case and uses ``point_on_page`` at both ends.
"""
from __future__ import annotations

import ctypes
import re
import sys
import time
from contextlib import contextmanager
from pathlib import Path
from typing import List, Optional, Tuple

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tests.helpers.pdfe_session import (  # noqa: E402
    PDFE_EXE,
    PDFESession,
    kill_pdfe_orphans,
    no_orphans,
)
from tests.helpers.canvas_geometry import (  # noqa: E402
    canvas_rect,
    page_rect_on_canvas,
    point_on_page,
    refocus_canvas,
    view_state,
)
from tests.helpers.keys import send_keys_to  # noqa: E402
from tests.helpers.modal_dialog import list_process_windows  # noqa: E402
from tests.helpers import qinvoke  # noqa: E402
from tests.helpers import win32_input as wi  # noqa: E402
from tests.ui_automation.conftest import record_matrix_result  # noqa: E402

FIXTURES = REPO_ROOT / "tests" / "fixtures"
FIX_SINGLE = FIXTURES / "test_single.pdf"
FIX_3PAGE = FIXTURES / "test_3page.pdf"

#: Qt renders popup menus into their own top-level window whose class name
#: carries this marker. Checked rather than the title because the popup
#: inherits the application name ("PDFE"), which would also match the main
#: window.
_QT_POPUP_CLASS_MARKER = "QWindowPopup"

pytestmark = pytest.mark.skipif(
    not Path(PDFE_EXE).exists(),
    reason=f"PDFE build artifact not present at {PDFE_EXE}",
)


# ---------------------------------------------------------------------------
# Session plumbing
# ---------------------------------------------------------------------------

@contextmanager
def _session(fixture: Path, settle: float = 1.5):
    """Launch PDFE on *fixture*, yield the session, and prove clean teardown.

    The orphan assertion is not decoration: a stray ``pdfe.exe`` holds a
    MACHINE-GLOBAL single-instance mutex and wedges the next run.
    """
    # Both fixtures this helper is ever called with (FIX_SINGLE, FIX_3PAGE) are
    # TRACKED files. Their absence therefore means a broken checkout, not an
    # environment PDFE is legitimately unavailable in -- so skipping on it is a
    # WIN-45 false green: the product could not be driven and the run reported
    # not-a-failure. The genuine environment gate (no build artifact) is already
    # expressed as the module-level `pytestmark` skipif on PDFE_EXE above.
    if not fixture.exists():
        _reach = f"tracked fixture missing from the checkout: {fixture}"
        pytest.fail(
            f"WIN-45 reachability blocked (recorded non-pass, not skipped): {_reach}")
    kill_pdfe_orphans()
    with PDFESession(pdf_path=str(fixture)) as s:
        assert s.wait_for_window_title(fixture.name, timeout=25), (
            f"PDFE window title never showed {fixture.name!r}"
        )
        time.sleep(settle)
        yield s
    # PDFESession.__exit__ quits then force-kills; confirm it took.
    deadline = time.monotonic() + 10.0
    while time.monotonic() < deadline and not no_orphans():
        time.sleep(0.3)
    assert no_orphans(), (
        "pdfe.exe still running after teardown — an orphan holds the "
        "single-instance mutex and will wedge the next test."
    )


def _require(value, what: str):
    """Return *value*, or fail loudly when a seam declined to answer.

    Never returns a fallback. The whole point of the geometry and view-state
    seams is that a test asks the application instead of guessing; a guess
    reached by way of ``or default`` is still a guess.
    """
    if value is None:
        pytest.fail(f"{what}: the seam did not answer (got None). "
                    f"Refusing to substitute a guess.")
    return value


def _state(s: PDFESession) -> dict:
    return _require(view_state(s), "view-state seam")


def _zoom_percent(s: PDFESession) -> float:
    """Current zoom as a number, via the zoomCombo readback pipe."""
    raw = qinvoke.zoom_combo_current_text(s)
    if not raw or raw == "__null__":
        pytest.fail(f"zoomCombo readback pipe returned {raw!r}; cannot "
                    f"observe the zoom level.")
    m = re.search(r"(\d+(?:\.\d+)?)", raw)
    if not m:
        pytest.fail(f"zoomCombo text {raw!r} is not a percentage.")
    return float(m.group(1))


def _point_on_page_visible(s: PDFESession, page_index: int,
                           fx: float, fy: float) -> Tuple[int, int]:
    """``point_on_page`` plus a hard check that the point is on the canvas.

    ``point_on_page`` is exact about where the page is, but a page position
    that is scrolled or zoomed out of the viewport is still off-widget, and
    clicking it hits the toolbar. Fail here, where the message says so,
    rather than three assertions later where it reads as "the gesture did
    nothing".
    """
    pt = _require(point_on_page(s, page_index, fx, fy),
                  f"point_on_page(page={page_index}, fx={fx}, fy={fy})")
    r = _require(canvas_rect(s), "canvas rect")
    x, y = pt
    if not (r.left <= x <= r.right and r.top <= y <= r.bottom):
        pytest.fail(
            f"point_on_page(page={page_index}, {fx}, {fy}) = {pt} is OUTSIDE "
            f"the canvas rect ({r.left},{r.top})-({r.right},{r.bottom}). The "
            f"page position is correct but not currently visible; a click "
            f"there would land on other chrome. Reset zoom/scroll first."
        )
    return x, y


def _window_class(hwnd: int) -> str:
    buf = ctypes.create_unicode_buffer(256)
    ctypes.WinDLL("user32").GetClassNameW(ctypes.c_void_p(hwnd), buf, 256)
    return buf.value or ""


def _popup_hwnds(pid: int) -> List[int]:
    """Visible Qt popup windows owned by *pid*, found WITHOUT UIA.

    ``list_process_windows`` is raw ``EnumWindows`` + ``WM_GETTEXT``; a Qt
    nested ``exec()`` loop keeps pumping messages so it answers normally,
    which is exactly the property the UIA provider lacks here.
    """
    return [h for h, _t in list_process_windows(pid)
            if _QT_POPUP_CLASS_MARKER in _window_class(h)]


# ---------------------------------------------------------------------------
# canvas_keyboard_nav  (MAT-2, test_3page.pdf)
# ---------------------------------------------------------------------------

def test_canvas_keyboard_nav_pagedown_and_pageup_change_the_page():
    """PageDown/PageUp move the canvas between pages.

    Matrix row: ``canvas_keyboard_nav``.
    Oracle: ``view_state()["page"]``, the index ``PdfCanvas::goToPage``
    actually moved to — not a screenshot, and not liveness.

    The re-focus before every key is load-bearing rather than superstition.
    Writing to any query pipe moves Win32 focus to the hidden pipe
    QLineEdit, so a key synthesised after a state read lands on the PIPE and
    the canvas appears to ignore PageDown while in fact handling it
    perfectly. ``refocus_canvas`` clicks the canvas to take focus back.
    """
    test_id = "canvas_keyboard_nav"
    with _session(FIX_3PAGE) as s:
        start = _state(s)
        assert start["page"] == 0, (
            f"expected to open on page 0, got {start['page']}")

        seen = [start["page"]]
        for expected in (1, 2):
            assert refocus_canvas(s), "could not return focus to the canvas"
            send_keys_to(s.hwnd, "{PGDN}")
            time.sleep(0.8)
            got = _state(s)["page"]
            seen.append(got)
            if got != expected:
                record_matrix_result(test_id, "red",
                                     f"PageDown -> page {got}, want {expected}")
            assert got == expected, (
                f"PageDown should have moved to page {expected}; "
                f"view state reports page {got}. Sequence so far: {seen}")

        assert refocus_canvas(s), "could not return focus to the canvas"
        send_keys_to(s.hwnd, "{PGUP}")
        time.sleep(0.8)
        back = _state(s)["page"]
        seen.append(back)
        assert back == 1, (
            f"PageUp from page 2 should return to page 1; got {back}. "
            f"Sequence: {seen}")

        # Guard the measurement itself: a seam that answered 0 every time
        # would have failed above, but a seam that answered the SAME value
        # every time is the zero-vs-zero shape. Three distinct pages were
        # observed, so the reading genuinely tracked the navigation.
        assert len(set(seen)) == 3, (
            f"expected three distinct page indices across the walk, saw "
            f"{seen} — the page readback is not tracking navigation.")
        record_matrix_result(test_id, "green",
                             f"page walk {seen} via PGDN/PGDN/PGUP")


# ---------------------------------------------------------------------------
# canvas_zoom_pinch  (MAT-2, test_single.pdf)
# ---------------------------------------------------------------------------

def test_canvas_zoom_pinch_ctrl_wheel_raises_then_lowers_the_zoom():
    """Ctrl+wheel over the canvas zooms in, and back out.

    Matrix row: ``canvas_zoom_pinch``.
    Oracle: the numeric zoom percentage read back from ``zoomCombo_``
    (``testZoomComboResultPipe``). "Zoom level changes" is asserted as a
    VALUE COMPARISON in both directions, not as "the call returned".

    Deliberately the WHEEL and not ``zoomInAction_``'s Ctrl+= shortcut: the
    row's gesture is Ctrl+scroll, which enters through
    ``PdfCanvas::wheelEvent``. Driving the QAction instead would pass
    identically whether or not the canvas wheel handler works at all.
    """
    test_id = "canvas_zoom_pinch"
    with _session(FIX_SINGLE) as s:
        r = _require(canvas_rect(s), "canvas rect")
        # Viewport gesture: the wheel goes to the widget under the cursor, so
        # the canvas CENTRE is the right target and where the page sits is
        # irrelevant. This is the documented exception, used deliberately.
        cx, cy = (r.left + r.right) // 2, (r.top + r.bottom) // 2

        before = _zoom_percent(s)
        wi.send_wheel(cx, cy, +3, ctrl=True)
        time.sleep(1.0)
        zoomed_in = _zoom_percent(s)
        if zoomed_in <= before:
            record_matrix_result(test_id, "red",
                                 f"ctrl+wheel up: {before}% -> {zoomed_in}%")
        assert zoomed_in > before, (
            f"Ctrl+wheel-up should raise the zoom; it went {before}% -> "
            f"{zoomed_in}%.")

        wi.send_wheel(cx, cy, -3, ctrl=True)
        time.sleep(1.0)
        zoomed_out = _zoom_percent(s)
        assert zoomed_out < zoomed_in, (
            f"Ctrl+wheel-down should lower the zoom; it went {zoomed_in}% -> "
            f"{zoomed_out}%.")
        # Symmetric step counts must land back where we started. This is what
        # separates "the number moved" from "the number tracks the gesture".
        assert abs(zoomed_out - before) < 0.5, (
            f"three notches up then three down should return to {before}%, "
            f"but the zoom settled at {zoomed_out}%.")
        record_matrix_result(
            test_id, "green",
            f"ctrl+wheel zoom {before}% -> {zoomed_in}% -> {zoomed_out}%")


# ---------------------------------------------------------------------------
# canvas_drag_interaction  (MAT-2, test_single.pdf)
# ---------------------------------------------------------------------------

def test_canvas_drag_interaction_middle_drag_pans_the_viewport():
    """A middle-button drag pans the canvas on both axes.

    Matrix row: ``canvas_drag_interaction`` (expected: scroll/pan changes
    viewport).
    Oracle: ``view_state()``'s ``scrollX``/``scrollY``, compared against the
    DISTANCE DRAGGED — so a pan that fires but ignores the gesture's
    magnitude still fails.

    Middle-button and not left: per the block comment at the head of
    ``PdfCanvas::mousePressEvent`` (GitHub #119), every tool consumes
    left-drag, so pan is bound to middle-drag and is tool-independent.

    VIEWPORT GESTURE — deliberate exception to the point_on_page rule. A pan
    is a statement about the viewport, not about the page, so the canvas
    widget centre is the correct target and where the page happens to sit is
    irrelevant. The canvas rect is asked for, not guessed.
    """
    test_id = "canvas_drag_interaction"
    drag_px = 300
    with _session(FIX_SINGLE) as s:
        r = _require(canvas_rect(s), "canvas rect")
        cx, cy = (r.left + r.right) // 2, (r.top + r.bottom) // 2

        # Precondition: at fit zoom the page fits the viewport and there is
        # NOTHING to pan, so the gesture would correctly do nothing and the
        # test would report a defect that is not there. Zoom in until the
        # rendered page exceeds the viewport on both axes, and say so if it
        # does not.
        wi.send_wheel(cx, cy, +5, ctrl=True)
        time.sleep(1.5)
        page = _require(page_rect_on_canvas(s, 0), "page rect on canvas")
        _px, _py, pw, ph = page
        canvas_w, canvas_h = r.right - r.left, r.bottom - r.top
        assert pw > canvas_w + drag_px and ph > canvas_h + drag_px, (
            f"precondition not met: after zooming in the page is {pw}x{ph} "
            f"against a {canvas_w}x{canvas_h} canvas, which leaves less than "
            f"{drag_px}px of pan range. The pan assertion below would be "
            f"measuring clamping, not panning.")

        before = _state(s)
        # Drag content up and to the left => the view scrolls down and right.
        wi.send_drag(cx + drag_px // 2, cy + drag_px // 2,
                     cx - drag_px // 2, cy - drag_px // 2,
                     button="middle", duration=0.5, steps=15)
        time.sleep(1.0)
        after = _state(s)

        dy = after["scrollY"] - before["scrollY"]
        dx = after["scrollX"] - before["scrollX"]
        if dy <= 0 or dx <= 0:
            record_matrix_result(
                test_id, "red",
                f"middle drag produced scroll delta dx={dx} dy={dy}")
        assert dy > 0 and dx > 0, (
            f"a middle-button drag of {drag_px}px up-left should scroll the "
            f"view down and right; got dx={dx}, dy={dy} "
            f"(before={before}, after={after}).")
        # Tie the result to the INPUT: the pan must track the drag distance,
        # not merely be non-zero. Clamping can only shorten it.
        lo, hi = drag_px * 0.6, drag_px * 1.4
        assert lo <= dy <= hi and lo <= dx <= hi, (
            f"pan distance should track the {drag_px}px drag; got dx={dx}, "
            f"dy={dy}, expected each within [{lo:.0f}, {hi:.0f}].")
        record_matrix_result(
            test_id, "green",
            f"middle-drag {drag_px}px panned dx={dx} dy={dy}")


# ---------------------------------------------------------------------------
# sendinput_drag  (MAT-1, test_single.pdf)
# ---------------------------------------------------------------------------

def test_sendinput_drag_creates_an_annotation_matching_the_dragged_rect():
    """A Win32 SendInput left-drag reaches the active tool and shapes its output.

    Matrix row: ``sendinput_drag``. The row's committed expectation was
    "PDFE survives" — a did-not-crash oracle, true of a drag that was never
    delivered, and the reason this row is being rewritten rather than simply
    implemented.

    Oracle, in two parts:
      1. ``view_state()["annots"]`` goes 0 -> 1, so the drag was delivered to
         ``RectangleTool`` and committed an annotation; and
      2. the annotation's PDF rect matches the rectangle WE DRAGGED, within
         tolerance. Part 2 is what makes a drag that fires at the wrong place
         fail. Part 1 alone would pass for a drag anywhere on the page.

    The endpoints come from ``point_on_page`` — this is NOT a viewport
    gesture; it must land on the page or no tool sees it.
    """
    test_id = "sendinput_drag"
    # Page fractions of the dragged rectangle; the expected PDF rect is
    # derived from these and the page's own size, so the assertion is
    # against the INPUT.
    fx0, fy0, fx1, fy1 = 0.30, 0.40, 0.65, 0.62
    with _session(FIX_SINGLE) as s:
        assert qinvoke.activate_tool_by_name(s, "rectangleToolAction"), (
            "could not activate rectangleToolAction via testToolActivatePipe")
        time.sleep(1.2)

        p1 = _point_on_page_visible(s, 0, fx0, fy0)
        p2 = _point_on_page_visible(s, 0, fx1, fy1)

        before = _state(s)
        assert before["annots"] == 0, (
            f"fixture should open with no annotations; got {before['annots']}")

        wi.send_drag(p1[0], p1[1], p2[0], p2[1], button="left",
                     duration=0.6, steps=18)
        time.sleep(1.5)
        after = _state(s)
        if after["annots"] != 1:
            record_matrix_result(
                test_id, "red",
                f"SendInput drag left annots at {after['annots']}, want 1")
        assert after["annots"] == 1, (
            f"the SendInput left-drag should have created exactly one "
            f"rectangle annotation; annots went {before['annots']} -> "
            f"{after['annots']}.")

        # Select it so view_state reports its rect, then compare to the drag.
        assert qinvoke.activate_tool_by_name(s, "selectToolAction")
        time.sleep(0.8)
        mid = _point_on_page_visible(s, 0, (fx0 + fx1) / 2, (fy0 + fy1) / 2)
        wi.send_click(mid[0], mid[1])
        time.sleep(1.0)
        selected = _state(s)
        assert selected["selCount"] == 1 and selected["rect"] is not None, (
            f"could not select the new annotation to read its rect: "
            f"{selected}")

        import fitz  # local: only this test needs the fixture's page box
        with fitz.open(str(FIX_SINGLE)) as doc:
            page_w = doc[0].rect.width
            page_h = doc[0].rect.height
        want_w = (fx1 - fx0) * page_w
        want_h = (fy1 - fy0) * page_h
        _rx, _ry, got_w, got_h = selected["rect"]
        tol = 12.0  # px of PDF space: int truncation in the fraction->screen
                    # conversion plus the tool's own rounding
        assert abs(got_w - want_w) <= tol and abs(got_h - want_h) <= tol, (
            f"the annotation's size should match the dragged rectangle: "
            f"dragged {want_w:.1f}x{want_h:.1f} PDF units, annotation is "
            f"{got_w}x{got_h} (rect={selected['rect']}). A drag delivered to "
            f"the wrong place would land here.")
        record_matrix_result(
            test_id, "green",
            f"SendInput left-drag created 1 annot sized {got_w}x{got_h} "
            f"vs dragged {want_w:.1f}x{want_h:.1f}")


# ---------------------------------------------------------------------------
# canvas_right_click_context  (MAT-2, test_single.pdf)
# ---------------------------------------------------------------------------

def test_canvas_right_click_opens_a_context_menu_that_acts():
    """Right-click on the page opens the canvas context menu, and it works.

    Matrix row: ``canvas_right_click_context`` (expected: context menu
    appears).

    "Appears" is asserted twice over, because presence alone is weak:
      1. a NEW visible top-level window of class ``*QWindowPopup*`` owned by
         PDFE exists after the right-click and did not before it; and
      2. driving its first item with the keyboard changes the zoom
         percentage — ``MainWindow``'s handler puts ``zoomInAction_`` first
         (see the ``canvasContextMenuRequested`` lambda), so this proves the
         popup is the canvas menu, is focused, and is live.

    NO UIA CALL IS MADE WHILE THE POPUP IS UP. ``QMenu::exec`` is a nested
    event loop — the same construct pywinauto's UIA backend cannot
    enumerate, and the one that produces the GAP-A14 menu hang. Detection is
    raw Win32 ``EnumWindows``; interaction is SendInput keystrokes. The zoom
    is therefore read BEFORE the right-click and again only after the menu
    has closed.
    """
    test_id = "canvas_right_click_context"
    with _session(FIX_SINGLE) as s:
        pid = s.pid
        assert not _popup_hwnds(pid), (
            "a Qt popup window was already open before the right-click; "
            "the 'appeared' assertion would be vacuous.")
        zoom_before = _zoom_percent(s)
        pt = _point_on_page_visible(s, 0, 0.5, 0.5)

        popped: List[int] = []
        try:
            wi.send_click(pt[0], pt[1], button="right")
            deadline = time.monotonic() + 8.0
            while time.monotonic() < deadline:
                popped = _popup_hwnds(pid)
                if popped:
                    break
                time.sleep(0.2)
            if not popped:
                record_matrix_result(
                    test_id, "red",
                    "no Qt popup window after right-click on the page")
            assert popped, (
                f"right-clicking the page at {pt} should open the canvas "
                f"context menu, but no Qt popup window belongs to pid {pid}. "
                f"Windows seen: {list_process_windows(pid)}")

            # First item is Zoom In. Down+Enter activates it and closes the menu.
            send_keys_to(s.hwnd, "{DOWN}{ENTER}")
            time.sleep(1.2)
        finally:
            # Never leave a nested exec() loop running: it would block the
            # graceful quit in teardown and strand an orphan.
            if _popup_hwnds(pid):
                send_keys_to(s.hwnd, "{ESC}")
                time.sleep(0.5)

        assert not _popup_hwnds(pid), (
            "the context menu is still open after activating an item.")
        zoom_after = _zoom_percent(s)
        assert zoom_after > zoom_before, (
            f"activating the context menu's first item (Zoom In) should "
            f"raise the zoom; it went {zoom_before}% -> {zoom_after}%. The "
            f"popup appeared but did not act.")
        record_matrix_result(
            test_id, "green",
            f"popup hwnd(s) {popped}; first item took zoom "
            f"{zoom_before}% -> {zoom_after}%")


# ---------------------------------------------------------------------------
# canvas_double_click_text  (MAT-2, test_single.pdf)
# ---------------------------------------------------------------------------

def test_canvas_double_click_selects_the_word_under_the_cursor():
    """Double-clicking a word selects THAT word.

    Matrix row: ``canvas_double_click_text`` (expected: text edit mode
    activated or word selected).

    Oracle: ``MainWindow::textEditSelectionDiag`` via
    ``text_edit_selection_diag`` — ``active_span``, ``sel_start``,
    ``sel_end`` and the span's text. The load-bearing assertion compares the
    SELECTED SUBSTRING against the word ``fitz`` reports at the coordinate
    we double-clicked, which is an INPUT. "A selection exists" would pass
    for a double-click that selected the wrong word, or the whole span.

    Both words are checked. One word proves the click reached the tool; two
    different words at two different coordinates prove the selection follows
    the cursor rather than being a constant.
    """
    test_id = "canvas_double_click_text"
    import fitz

    with fitz.open(str(FIX_SINGLE)) as doc:
        page = doc[0]
        page_w, page_h = page.rect.width, page.rect.height
        words = page.get_text("words")
    assert len(words) >= 2, (
        f"{FIX_SINGLE.name} must contain at least two words for this test; "
        f"found {len(words)}")
    targets = [words[0], words[-1]]

    with _session(FIX_SINGLE) as s:
        assert qinvoke.activate_tool_by_name(s, "textEditToolAction"), (
            "could not activate textEditToolAction via testToolActivatePipe")
        time.sleep(1.5)

        observed = []
        for x0, y0, x1, y1, word in (t[:5] for t in targets):
            fx = ((x0 + x1) / 2) / page_w
            fy = ((y0 + y1) / 2) / page_h
            pt = _point_on_page_visible(s, 0, fx, fy)

            wi.send_click(pt[0], pt[1])
            time.sleep(0.12)
            wi.send_click(pt[0], pt[1])
            time.sleep(1.2)

            diag = qinvoke.text_edit_selection_diag(s)
            if diag["active_span"] < 0 or diag["sel_end"] <= diag["sel_start"]:
                record_matrix_result(
                    test_id, "red",
                    f"double-click on {word!r} at {pt} produced no selection: "
                    f"{diag}")
            assert diag["active_span"] >= 0, (
                f"double-clicking {word!r} at {pt} did not activate a text "
                f"span (active_span={diag['active_span']}). diag={diag}")
            assert diag["sel_end"] > diag["sel_start"] >= 0, (
                f"double-clicking {word!r} selected nothing: "
                f"sel_start={diag['sel_start']} sel_end={diag['sel_end']}")

            got = diag["active_text"][diag["sel_start"]:diag["sel_end"]]
            observed.append(got)
            assert got == word, (
                f"double-clicking at the centre of {word!r} should select "
                f"that word; the tool selected {got!r} instead "
                f"(span text {diag['active_text']!r}).")

        assert observed[0] != observed[1], (
            f"both double-clicks selected {observed[0]!r} — the selection is "
            f"not following the cursor.")
        record_matrix_result(
            test_id, "green",
            f"double-click selected {observed[0]!r} then {observed[1]!r}")


# ---------------------------------------------------------------------------
# search_navigate  (MAT-2, test_3page.pdf)
# ---------------------------------------------------------------------------

def _search_result_items(s: PDFESession):
    """The SearchPanel result rows, top to bottom.

    Qt's UIA bridge gives every ``QListWidget`` item the SAME AutomationId as
    the list itself ("...searchPanel.resultsList") and does not surface the
    list container, so ``find(auto_id="resultsList")`` returns an ITEM, not
    the list. Items are collected directly and de-duplicated by screen
    rectangle, which also orders them by row.
    """
    seen = {}
    for d in s.window.descendants():
        try:
            aid = d.element_info.automation_id or ""
            if not aid.endswith("resultsList"):
                continue
            if d.element_info.control_type != "ListItem":
                continue
            r = d.rectangle()
        except Exception:  # noqa: BLE001 — stale UIA element
            continue
        seen.setdefault((r.top, r.left), d)
    return [seen[k] for k in sorted(seen)]


def _page_from_result_label(text: str) -> Optional[int]:
    """0-based page index named by a result row's own label ("Page 3: ...")."""
    m = re.match(r"\s*Page\s+(\d+)\s*:", text or "")
    return int(m.group(1)) - 1 if m else None


def test_search_navigate_moves_the_view_to_each_matched_page():
    """Selecting successive search results navigates the canvas.

    Matrix row: ``search_navigate``.

    TWO CORRECTIONS TO THE ROW, both verified against the product:

      * ``SearchPanel`` has NO Next/Previous buttons. Its widgets are
        searchEdit, searchButton, clearButton, the two modifier checks, the
        replace controls, statusLabel and resultsList. Navigation between
        matches IS selection in ``resultsList``:
        ``onResultItemClicked`` -> ``searchResultSelected`` ->
        ``MainWindow::onSearchResultSelected`` -> ``canvas_->goToPage``.
        The row's action is corrected to say so.
      * the fixture moves from test_single.pdf to test_3page.pdf.
        test_single.pdf is ONE page, so "the selection advanced to the next
        match" could not be observed as anything at all — every match would
        navigate to the same page and the oracle would be one-against-one.

    Oracle: for each result row, the page the canvas lands on must equal the
    page THAT ROW'S OWN LABEL names. That is an assertion against an input
    (the label), not against another output, and a navigation that always
    went to page 0 would fail it. The run additionally requires that at
    least two distinct pages were reached, so a fixture that degenerated to
    a single match could never produce a vacuous pass.
    """
    test_id = "search_navigate"
    with _session(FIX_3PAGE) as s:
        # findAction, not toggleSearchPanelAction. The toggle only flips the
        # dock's visibility; the Search dock is TABIFIED behind Properties
        # (see MainWindow's tabifyDockWidget call), so a dock that is
        # "visible" can still be the unraised tab and its children are then
        # not in the UIA tree. MainWindow::onFind does show() + raise() +
        # focusSearchBox(), which is what a user pressing Ctrl+F gets.
        assert qinvoke.activate_tool_by_name(s, "findAction"), (
            "could not open the Search panel via testToolActivatePipe")
        time.sleep(1.5)

        edit = s.find_optional(auto_id="searchEdit", timeout=8)
        assert edit is not None, "searchEdit not reachable in the UIA tree"
        edit.set_edit_text("Page")
        time.sleep(0.3)
        button = s.find_optional(auto_id="searchButton", timeout=6)
        assert button is not None, "searchButton not reachable"
        button.invoke()
        time.sleep(2.5)

        items = _search_result_items(s)
        assert len(items) >= 2, (
            f"searching 'Page' in {FIX_3PAGE.name} should return several "
            f"matches; the results list has {len(items)} row(s). Without at "
            f"least two, 'advances to the next match' is unobservable.")

        labelled = []
        for it in items:
            try:
                label = it.window_text() or ""
            except Exception:  # noqa: BLE001
                continue
            pg = _page_from_result_label(label)
            if pg is not None:
                labelled.append((it, pg, label))
        assert len(labelled) >= 2, (
            f"result rows do not carry a parseable 'Page N:' label: "
            f"{[l for _i, _p, l in labelled]}")

        visited = []
        for it, want_page, label in labelled:
            try:
                it.click_input()
            except Exception as exc:  # noqa: BLE001
                pytest.fail(f"could not select result row {label!r}: {exc}")
            time.sleep(1.2)
            got = _state(s)["page"]
            visited.append(got)
            if got != want_page:
                record_matrix_result(
                    test_id, "red",
                    f"result {label!r} navigated to page {got}, "
                    f"label says {want_page}")
            assert got == want_page, (
                f"selecting the result labelled {label!r} should take the "
                f"canvas to page {want_page}; it went to page {got}.")

        assert len(set(visited)) >= 2, (
            f"every result selection landed on the same page {visited} — "
            f"navigation cannot be distinguished from doing nothing.")
        record_matrix_result(
            test_id, "green",
            f"{len(labelled)} results; canvas visited pages {visited} "
            f"matching each row's own label")
