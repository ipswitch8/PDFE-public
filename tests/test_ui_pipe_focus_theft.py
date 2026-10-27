# -*- coding: utf-8 -*-
"""GitHub #303 — a UIA pipe write must not steal keyboard focus.

THE DEFECT, measured 2026-10-13 with the application's own key trace
(`docs/audit/p96-phase10/raw-i295-*.txt`). Every hidden test pipe is built with
`setFocusPolicy(Qt::NoFocus)` and a zero size, on the assumption that it cannot
take keyboard focus. It can: UIA's `ValuePattern::SetValue` — which is how
`tests/helpers/qinvoke.py` writes to a pipe — FOCUSES the control it writes to,
and `Qt::NoFocus` does not prevent that. `Qt::NoFocus` governs focus from
clicks and TAB, not from an accessibility client asserting it directly. After
any pipe write, every subsequent key event reported

    focus=testKeyTraceQueryPipe[QLineEdit]

so a keystroke a test sent after a pipe interaction was delivered to a hidden
`QLineEdit` rather than to the widget under test.

WHY THIS TEST IS THE PROOF AND THE UNIT TESTS ARE NOT. The defect only exists
when a real UIA client asserts focus, which no in-process test can produce:
`tests/test_test_pipe_focus_guard.cpp` can pin the DECISION the guard makes
once focus has moved, but it cannot make UIA move it. This test does the thing
that actually broke — writes to a pipe through UIA, then sends a key — and
reads the answer out of the product's own trace.

WHAT IT ASSERTS. Not "the fix is installed" and not "focus is widget X" — the
exact property #303 names: a keystroke sent immediately after a pipe write,
with no intervening click, is NOT delivered to a hidden pipe. The widget it
does reach is allowed to vary (canvas or the previously focused widget, which
depends on what the product last focused), because pinning a specific widget
would make this test fail for reasons unrelated to the defect.
"""
from __future__ import annotations

import os
import re
import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parent.parent
PDFE_EXE = REPO_ROOT / "build" / "vs2022" / "bin" / "Debug" / "PDFE.exe"
FIXTURES_DIR = REPO_ROOT / "tests" / "fixtures"

sys.path.insert(0, str(REPO_ROOT))


def _has_pywinauto() -> bool:
    try:
        import pywinauto  # noqa: F401
        return True
    except Exception:
        return False


def _has_pyautogui() -> bool:
    try:
        import pyautogui  # noqa: F401
        return True
    except Exception:
        return False


def _is_interactive_session() -> bool:
    if os.environ.get("PDFE_UI_FORCE_HEADLESS") == "1":
        return False
    return bool(os.environ.get("SESSIONNAME"))


REQUIRES_LIVE_UI = pytest.mark.skipif(
    not (_has_pywinauto() and _has_pyautogui() and PDFE_EXE.exists()
         and _is_interactive_session()),
    reason=("GitHub #303 needs a live PDFE.exe driven through real UIA on an "
            "interactive desktop: the defect IS a UIA behaviour "
            "(ValuePattern::SetValue focusing its target) and cannot be "
            "reproduced in-process."),
)


@REQUIRES_LIVE_UI
def test_a_key_sent_after_a_pipe_write_does_not_land_on_a_pipe():
    """THE CRITERION. A pipe write, then a key, with no click between them."""
    import pyautogui
    from tests.ui_harness.core.launch import PDFESession  # type: ignore
    from tests.helpers import qinvoke
    from tests.helpers.canvas_geometry import point_on_page

    fixture = FIXTURES_DIR / "test_3page.pdf"
    assert fixture.exists(), fixture

    with PDFESession(pdf_path=str(fixture)) as session:
        # Arm the trace in `all` mode. A Ctrl+Z-only trace cannot distinguish
        # "no Ctrl+Z arrived" from "no key arrived at all", and those are
        # different findings with different owners (#295's second round).
        assert qinvoke.key_trace_set_mode(session, "all"), (
            "could not arm the key trace, so this test cannot measure "
            "anything -- failing rather than reporting a pass it did not earn")

        # Put focus somewhere real, the way a user does. point_on_page is used
        # rather than a canvas-rect fraction because the page does not fill the
        # canvas and a guessed offset lands in the grey margin (CLAUDE.md).
        x, y = point_on_page(session, page_index=0, fx=0.5, fy=0.5)
        pyautogui.click(x, y)

        # THE OFFENDING OPERATION. Any pipe write will do -- the defect is a
        # property of the UIA write, not of which pipe. The zoom pipe is used
        # because it is simple, idempotent and has no modal consequences.
        assert qinvoke._write_to_pipe(session, "testSetZoomPipe", "100"), (
            "the pipe write itself failed, so the focus question never arose")

        # ...and the keystroke, with NO intervening click. This is the exact
        # sequence that produced "1010 Ctrl+Z hotkeys removed 0 of 1000
        # annotations" in #295.
        pyautogui.hotkey("ctrl", "z")

        trace = qinvoke.key_trace_dump(session)
        assert trace is not None, (
            "the key trace did not answer. That is a harness failure, not a "
            "pass: 'the product behaved' and 'we could not find out' must "
            "never collapse into the same result")
        assert trace != "empty", (
            "the application recorded NO key events at all, so the keystroke "
            "never reached it. The test proves nothing about focus in that "
            "state -- it is a harness or desktop problem (see "
            "desktop_outage_contaminates_a_ui_sweep)")

        focuses = qinvoke.key_trace_focus_targets(trace)
        assert focuses, "trace had records but no focus= fields: %r" % trace

        offenders = [f for f in focuses if qinvoke.focus_is_a_test_pipe(f)]
        assert not offenders, (
            "GitHub #303: a key sent after a pipe write was delivered with "
            "focus on a hidden test pipe %r. Every keyboard assertion made "
            "after a pipe write in this suite is measuring the wrong widget. "
            "Full trace: %s" % (offenders, trace))


@REQUIRES_LIVE_UI
def test_the_trace_can_still_see_focus_on_a_pipe_if_it_ever_happens():
    """SENSITIVITY. Does the measurement work at all?

    The test above passes if NO record names a pipe — which is also what a
    broken trace, a mis-parsed field, or a `focus_is_a_test_pipe` that never
    returns True would produce. An all-green assertion with no demonstrated
    ability to go red is the vacuous-oracle shape this project keeps finding.

    So: assert the DETECTOR works, on synthetic field values, in the same run.
    This does not need the application, and that is the point — the detector
    must be known-good independently of the thing it is measuring.
    """
    from tests.helpers import qinvoke
    assert qinvoke.focus_is_a_test_pipe("testKeyTraceQueryPipe[QLineEdit]")
    assert qinvoke.focus_is_a_test_pipe("testSetZoomPipe[QLineEdit]")
    # ...and the negative direction, so it is not simply always-true.
    assert not qinvoke.focus_is_a_test_pipe("pdfCanvas[PdfCanvas]")
    assert not qinvoke.focus_is_a_test_pipe("annotContentsEdit[QTextEdit]")
    assert not qinvoke.focus_is_a_test_pipe("none")
    assert not qinvoke.focus_is_a_test_pipe("")
    # A pipe-shaped NAME on a different class must not count: the guard only
    # covers QLineEdit, so claiming otherwise here would over-report.
    assert not qinvoke.focus_is_a_test_pipe("testMisleadingPipe[QWidget]")


@REQUIRES_LIVE_UI
def test_the_parser_reads_the_focus_field_the_application_actually_writes():
    """The other half of the sensitivity question: the FORMAT.

    `key_trace_focus_targets` greps `focus=(\\S+)` out of each record. If the
    application's record format changed, it would return [] and the assertion
    in the first test would pass for the wrong reason. Pinned here against a
    record in the exact shape `MainWindow::recordCtrlZTrace` emits.
    """
    from tests.helpers import qinvoke
    record = ("event=KeyPress key=0x5a mods=0x4000000 "
              "target=pdfCanvas[PdfCanvas] focus=testSetZoomPipe[QLineEdit] "
              "undoEnabled=1 canUndo=1 undoDepth=3")
    assert qinvoke.key_trace_focus_targets(record) == [
        "testSetZoomPipe[QLineEdit]"]
    # Two records, semicolon-joined, as the application joins them.
    assert len(qinvoke.key_trace_focus_targets(record + ";" + record)) == 2
    # The application's own "nothing recorded" answer is not a focus target.
    assert qinvoke.key_trace_focus_targets("empty") == []


@REQUIRES_LIVE_UI
def test_real_ctrl_z_hotkeys_after_a_pipe_write_undo_an_accurate_count(tmp_path):
    """Phase-3 criterion 3, measured with the KEYBOARD.

    WHY THIS EXISTS. The criterion asks that the #295 measurement "report an
    accurate removal count, no longer fabricating '0 of 1000' once focus-theft
    masking is removed". Re-running
    `test_ui_stress.py::test_stress_1000_annotation_undo_redo` reports 1000/1000
    -- but that test drives undo through the #271 `undo_repeat` PIPE seam and
    no longer sends a single keystroke, so its accuracy comes from not using
    the keyboard rather than from this fix. The phase-3 karen gate failed the
    phase on exactly that: disclosing the gap is not closing it.

    So this is the keyboard-driven measurement the criterion actually asks for:
    real `Ctrl+Z` hotkeys, sent AFTER a pipe write with no intervening click,
    removing real annotations, with the count read back from the saved file by
    PyMuPDF rather than from anything the harness incremented. A counter the
    harness increments is precisely what reported 1000 while the document held
    one (#294).

    N is small on purpose. The property under test is "does a keyboard undo
    reach the document after a pipe write", which does not become more true at
    1000 iterations -- and the 1000-note version costs ~10 minutes, almost all
    of it in creation. Volume is covered by the stress test; correctness of the
    keyboard path is covered here.
    """
    import ctypes
    import time

    import fitz
    import pyautogui
    from tests.helpers import qinvoke
    from tests.helpers.canvas_geometry import point_on_page
    # The SAME session class the stress test uses, because the save-dialog
    # driver imported below is written against it: `tests.ui_harness.core.
    # launch.PDFESession` (used by the focus test above) has no
    # `wait_for_top_level_window`, so pairing them silently fails on the first
    # dialog. Two session classes coexist in this suite; pick the one the
    # helper expects rather than adapting the helper.
    from tests.helpers.pdfe_session import PDFESession  # noqa: E402
    from tests.test_ui_stress import _save_as_via_dialog  # reuse, never copy

    fixture = FIXTURES_DIR / "test_3page.pdf"
    assert fixture.exists(), fixture

    # Well separated on the page. A sticky note is ~12 pt square and centred on
    # the click, so notes closer than that OVERLAP and a click on an existing
    # icon hits THAT note instead of creating one -- silently (#238). A 3x3 grid
    # at these fractions is far more than an icon apart at default zoom.
    grid = [(fx, fy) for fy in (0.25, 0.5, 0.75)
            for fx in (0.25, 0.5, 0.75)]
    n = len(grid)

    user32 = ctypes.WinDLL("user32", use_last_error=True)

    with PDFESession(pdf_path=str(fixture)) as session:
        assert session.wait_for_window_title(fixture.name, timeout=10)

        armed = 0
        for fx, fy in grid:
            # Re-arm through the ACKNOWLEDGED seam every iteration. A canvas
            # click arriving before the tool is armed goes to SelectTool and
            # creates nothing, silently -- clicking with the wrong tool is not
            # an error (#294).
            if not qinvoke.activate_tool_and_wait(
                    session, "stickyNoteToolAction", timeout=3.0):
                # WIN-45 / GitHub #229. This was a bare `continue`, which the
                # reachability-skip checker flagged as a new site -- in a test
                # added earlier in this same pipeline. Mine, and it was right
                # to flag it.
                #
                # The `assert armed == n` below WOULD have caught it, 15 lines
                # later, with the unhelpful message "placed 7 of 9". That is
                # the very reasoning this file's own header records as having
                # been wrong before: "a downstream oracle will catch it" is a
                # claim about REACHABILITY, and the useful place to fail is
                # where the dispatch failed, with the reason attached.
                _reach = ("stickyNoteToolAction did not arm within 3.0s at "
                          "grid point (%.2f, %.2f)" % (fx, fy))
                record_matrix_result(current_test_id(), "blocked", _reach)
                pytest.fail(
                    f"WIN-45 reachability blocked (recorded non-pass, not "
                    f"skipped): {_reach}")
            x, y = point_on_page(session, page_index=0, fx=fx, fy=fy)
            pyautogui.moveTo(x, y)
            user32.mouse_event(0x0002, 0, 0, 0, 0)
            time.sleep(0.005)
            user32.mouse_event(0x0004, 0, 0, 0, 0)
            time.sleep(0.35)
            armed += 1
            # Each creation opens a popup; dismiss it so the next click lands
            # on the canvas.
            pyautogui.press("escape")
            time.sleep(0.15)

        assert armed == n, (
            "only %d of %d tool armings were acknowledged, so this run cannot "
            "measure undo accuracy -- failing rather than asserting against a "
            "document whose contents are unknown" % (armed, n))

        after_create = tmp_path / "after_create.pdf"
        assert _save_as_via_dialog(session, after_create, timeout=15), (
            "Save-As did not produce a file, so there is no oracle")

        def count(p: Path) -> int:
            d = fitz.open(str(p))
            try:
                return sum(sum(1 for _ in pg.annots()) for pg in d)
            finally:
                d.close()

        created = count(after_create)
        assert created == n, (
            "precondition failed: expected %d annotations in the saved file, "
            "found %d. The undo measurement below would be meaningless."
            % (n, created))

        # THE OFFENDING OPERATION, then the keyboard. This is the sequence that
        # produced "1010 Ctrl+Z hotkeys removed 0 of 1000 annotations": a pipe
        # write pulls focus onto a hidden QLineEdit, which claims Ctrl+Z for
        # its own undo and -- having nothing to undo -- does nothing with it.
        assert qinvoke._write_to_pipe(session, "testSetZoomPipe", "100"), (
            "the pipe write failed, so the focus hazard never arose and this "
            "test would pass without exercising the defect")

        # NO intervening click. That is the whole point.
        for _ in range(n):
            pyautogui.hotkey("ctrl", "z")
            time.sleep(0.12)

        after_undo = tmp_path / "after_undo.pdf"
        assert _save_as_via_dialog(session, after_undo, timeout=15), (
            "Save-As after the undos did not produce a file")
        remaining = count(after_undo)

        assert remaining == 0, (
            "GitHub #303 / #295: %d real Ctrl+Z hotkeys sent after a pipe "
            "write removed %d of %d annotations -- %d remain. With the focus "
            "guard working this must reach zero; a non-zero remainder means "
            "the keystrokes are still being claimed by something other than "
            "the document."
            % (n, created - remaining, created, remaining))
