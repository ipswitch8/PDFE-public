#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Pipeline K phase-5: behavioral matrix coverage for atlas + scan-style + glyph-synth + font-matcher.

Adds drivers for 5 new matrix rows (finding 7) covering custom
subsystems that previously had zero pywinauto coverage:

  atlas_mode_combo_set_persist     — combo retains selection across reopen
  atlas_mode_visual_diff           — pixel diff between mode-0 and mode-1
  scan_style_toggle_persist        — fillMissingCheck persists across reopen
  glyph_synth_coverage             — atlas charset preview paints non-blank
  font_match_panose_top_candidate  — fontCombo populated with plausible name

All five tests need an active TRE atlas (built when the user
double-clicks a scanned-text word during edit mode), which the
canvas-click harness from phase-4 can drive on a real test_scanned.pdf
fixture. If the prerequisite chain (scanned fixture, click landing on
OCR text, atlas built, dialog reachable) cannot complete, the test
records ``blocked`` with a specific reason — never a silent green.
"""
from __future__ import annotations

import pathlib
import sys
import time

import pytest

try:
    from pywinauto import Application  # noqa: F401  # type: ignore
except ImportError:
    pytest.skip("pywinauto not installed", allow_module_level=True)

try:
    import pyautogui  # noqa: F401  # type: ignore
except ImportError:
    pytest.skip("pyautogui not installed", allow_module_level=True)

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tests.ui_automation.harness.pdfe_app_fixture import launch_pdfe  # noqa: E402
from tests.ui_automation.harness.canvas_click import (  # noqa: E402
    canvas_region,
    click_pdf_point,
    find_canvas,
    screenshot_canvas,
    pixel_diff_ratio,
)
from tests.ui_automation.harness.canvas_coords import (  # noqa: E402
    _assert_on_screen,
)
from tests.ui_automation.harness.dialog_helpers import (  # noqa: E402
    find_by_suffix,
    find_dialog_window,
    select_combo_item,
)
from tests.ui_automation.conftest import (  # noqa: E402
    record_matrix_result,
    assert_recorded,
)

# Phase F25 / gap WIN-45: reachability is a RECORDED NON-PASS, never a skip.
from tests.ui_automation.conftest import (  # noqa: E402
    current_test_id,
    record_matrix_result,
)


FIXTURES = pathlib.Path(__file__).resolve().parent.parent / "fixtures"
# 2026-05-06: switched from test_scanned.pdf to test_scanned_with_ocr.pdf,
# which is the same image with an invisible (Tr=3) OCR text layer baked
# in via tests/fixtures/make_test_scanned_with_ocr.py. The OCR layer is
# what makes Page::hasTextContent() return true for this fixture, which
# in turn routes the page through TextEditTool's span-click flow (the
# only path that builds the EnrichedAtlas + opens AtlasComparisonDialog).
# The pure-image test_scanned.pdf cannot drive this flow because
# TextEditTool's model_->spanAtPoint() always returns -1 with no text
# spans on the page, regardless of the click coordinates.
SCANNED_FIXTURE = FIXTURES / "test_scanned_with_ocr.pdf"
PANOSE_FIXTURE = FIXTURES / "test_panose.pdf"  # may not exist yet

# Click target on test_scanned.pdf where OCR'd text lives.
# OCR'd via Tesseract: 'Test' word at image (101,129)-(193,164) in
# the 850×1100 image; image-to-PDF scale is 1.389 (image is at 100 DPI,
# the PDF page is 612×792 pt). PDF-coord center of 'Test':
# ((101+92/2)/1.389, (129+35/2)/1.389) ≈ (105.8, 105.5).
WORD_POINT_PDF = (105.8, 105.5)

# GitHub #227. Which hop of the Edit-Text -> OCR -> click -> spanActivated ->
# atlas-build -> dialog chain stalled, set by
# _trigger_tre_edit_and_open_atlas_dialog on every path that gives up.
#
# The issue's actual complaint was that the failure "names the dialog rather
# than the step that did not happen": four distinct failures all surfaced as
# "AtlasComparisonDialog unreachable", so an intermittent red could not be
# attributed without re-running it by hand. A module global rather than a
# changed return type, so the two existing call sites keep working unchanged.
_LAST_ATLAS_STEP = ""

# Sub-step within step 1, set by _activate_text_edit_tool.
_LAST_EDIT_STEP = ""


def last_atlas_step() -> str:
    """The stalled hop, or a note that the chain completed."""
    return _LAST_ATLAS_STEP or "no step recorded (the chain reported success)"


def _app_alive_pid(pid) -> bool:
    """Whether the application still has a visible top-level window.

    Used to bound the two long waits in this file by LIVENESS rather than by a
    guessed deadline (GitHub #227). Returns True when it cannot tell, so an
    inability to observe never invents a failure.
    """
    if pid is None:
        return True
    try:
        from tests.helpers.modal_dialog import list_process_windows
        return bool(list_process_windows(pid))
    except Exception:
        return True


def _activate_text_edit_tool(win):
    """Click the Edit Text toolbar button (a checkable QToolButton) and
    wait for PDFE's UI status file to report TEXT_EDIT_READY|N where
    N>0. Returns True if the tool activated and OCR found at least one
    line.

    Console-session retry (2026-05-06): the prior version of this
    test assumed ``click_pdf_point(...clicks=2)`` would auto-activate
    Text Edit mode. It doesn't — TRE/OCR only runs after
    ``onTextEditToolSelected`` fires, which is only triggered by the
    toolbar action. Once the action runs, ``isImageDominated()`` (now
    fixed in src/core/Page.cpp:278 to return ``imageRatio>0.5``
    regardless of pre-existing text layer) gates whether the OCR
    pipeline runs and populates ``pendingAtlas_.allLines``.
    """
    import os
    from tests.ui_automation.harness.pdfe_app_fixture import find_widget

    status_file = pathlib.Path(
        os.environ.get("TEMP", "/tmp")) / "pdfe_ui_status.txt"
    status_file.unlink(missing_ok=True)

    global _LAST_EDIT_STEP
    _LAST_EDIT_STEP = ""

    # GitHub #227, ROOT CAUSE. The synthetic toolbar click below is what made
    # this flaky, and the instrumentation added for this issue is what proved
    # it: on a failing run the status file read '<file never appeared>' even
    # after 180 s, so onTextEditToolSelected never fired at all. OCR was not
    # slow -- it was never asked to start. The issue's own hypothesis (the
    # click -> spanActivated hop) was therefore wrong; the failure is one step
    # earlier, and the dialog it named was never due to open.
    #
    # PDFE ships a seam for exactly this, whose comment says it exists for
    # "bypassing flaky UIA toolbar-button discovery": writing a QAction
    # objectName to testToolActivatePipe calls activateToolByObjectName and
    # triggers the action through Qt's normal signal path. launch_pdfe already
    # sets PDFE_TEST_PIPES=1.
    #
    # The click is KEPT as a fallback rather than deleted, because the pipe is
    # absent from a build without PDFE_TEST_PIPES and this helper should still
    # do something sensible there.
    from tests.ui_automation.harness.pdfe_app_fixture import invoke_action
    activated_via = ""
    try:
        if invoke_action(win, "textEditToolAction"):
            activated_via = "testToolActivatePipe"
    except Exception:
        activated_via = ""

    btn = None
    if not activated_via:
        try:
            btn = find_widget(win, "textEditToolActionButton", timeout=3.0)
        except Exception as ex:
            _LAST_EDIT_STEP = (
                "neither the testToolActivatePipe seam nor the "
                "textEditToolActionButton could be reached within 3 s "
                "(%s: %s), so the tool was never activated and OCR never "
                "started" % (type(ex).__name__, ex))
            return 0
    # ``btn.toggle()`` (UIA TogglePattern.Toggle) updates the check
    # state but does NOT fire Qt's ``QAction::triggered`` signal,
    # so ``onTextEditToolSelected`` never runs. ``click_input()``
    # synthesizes a real mouse click that goes through Qt's normal
    # event path and fires the signal.
    if btn is not None:
        try:
            btn.click_input()
            activated_via = "toolbar click_input"
        except Exception as ex_click:
            try:
                btn.invoke()
                activated_via = "toolbar invoke"
            except Exception as ex_invoke:
                _LAST_EDIT_STEP = (
                    "the Edit Text button was found but could not be "
                    "activated - click_input raised %s, and the invoke() "
                    "fallback raised %s"
                    % (type(ex_click).__name__, type(ex_invoke).__name__))
                return 0

    # Poll for TEXT_EDIT_READY|N (N is line count). OCR can take 5-30s
    # depending on page complexity + tessdata path.
    import re as _re

    # GitHub #227. This was a flat 45 s, and I first raised it believing the
    # OCR pipeline was overrunning it. MEASUREMENT SAYS OTHERWISE: sampling the
    # sentinel through a passing run puts TEXT_EDIT_READY at 21.1 s from the
    # start, i.e. ~10 s of OCR after the tool activates. 45 s was never the
    # binding constraint here -- the stall is the ATLAS assembly further down
    # (see the note on _ATLAS_CAP_S).
    #
    # So this cap is NOT the fix for anything; it is headroom. What is worth
    # keeping is the change of KIND: wait while the application is still ALIVE
    # and report a dead one as a crash immediately, instead of letting a fixed
    # deadline expire and blaming a timeout. No retry is involved -- the
    # sentinel is still required, and a wait that ends any other way is still a
    # failure.
    _OCR_CAP_S = 120.0
    _pid = None
    try:
        _pid = win.process_id()
    except Exception:
        pass

    started = time.time()
    deadline = started + _OCR_CAP_S
    last = "<file never appeared>"
    while time.time() < deadline:
        if not _app_alive_pid(_pid):
            _LAST_EDIT_STEP = (
                "the application exited %.0f s after the Edit Text click, "
                "before emitting TEXT_EDIT_READY. Last status-file content "
                "was %r. This is a crash or early exit, NOT an OCR timeout."
                % (time.time() - started, last))
            return 0
        if status_file.exists():
            st = status_file.read_text(encoding="utf-8").strip()
            last = st or "<empty>"
            m = _re.match(r"TEXT_EDIT_READY\|(\d+)", st)
            if m:
                n = int(m.group(1))
                if n == 0:
                    _LAST_EDIT_STEP = (
                        "the tool activated and OCR completed, but reported "
                        "ZERO lines. isImageDominated() gates the OCR pipeline "
                        "(src/core/Page.cpp), so either the fixture stopped "
                        "reading as image-dominated or tessdata produced "
                        "nothing.")
                return n
        time.sleep(0.5)
    _LAST_EDIT_STEP = (
        "the button was clicked and the application stayed alive, but no "
        "TEXT_EDIT_READY|N sentinel appeared within %.0f s. Last status-file "
        "content was %r. Either onTextEditToolSelected never fired (the click "
        "did not reach Qt's signal path) or the OCR pipeline genuinely hung. "
        "OCR is measured at ~10 s (TEXT_EDIT_READY lands 21 s in), so this cap "
        "is an order of magnitude clear of it and a timing artefact is not a "
        "plausible reading. The tool was activated via %s."
        % (_OCR_CAP_S, last, activated_via or "NOTHING (no activation path "
           "succeeded)"))
    return 0


def _start_with_scanned():
    if not SCANNED_FIXTURE.exists():
        pytest.skip(f"scanned fixture missing: {SCANNED_FIXTURE.name}")
    try:
        app, win = launch_pdfe(args=[str(SCANNED_FIXTURE)])
    except RuntimeError as e:
        pytest.skip(f"pdfe.exe launch failed: {e}")
    time.sleep(3.0)
    if canvas_region(win) is None:
        # WIN-45 / F25: unreachable -> recorded 'blocked', then FAIL.
        _reach = (
            "PdfCanvas absent from UIA tree"
        )
        record_matrix_result(current_test_id(), "blocked", _reach)
        pytest.fail(
            f"WIN-45 reachability blocked (recorded non-pass, not skipped): {_reach}")
    return app, win


def _find_atlas_dialog_by_hwnd(pid, timeout=4.0):
    """hwnd-based AtlasComparisonDialog lookup, via the shared seam.

    UIA cannot enumerate a Qt app-modal dialog while its ``exec()`` loop
    runs, so this has always gone through Win32 ``EnumWindows``. It used to
    do that with a LOCAL copy of the enumeration, which GitHub #227 is a
    reason to stop maintaining:

      * ``tests/helpers/modal_dialog.py`` reads the title with ``WM_GETTEXT``
        AND A TIMEOUT, precisely "so a wedged window cannot hang us". The
        local copy called ``GetWindowTextW`` directly, which blocks
        indefinitely on a window whose owning thread is busy -- and the atlas
        dialog appears at the end of a 5-30 s CNN/atlas build, which is
        exactly when a thread is busy.
      * One copy of this logic can be fixed once. Two drift.

    Returns a pywinauto wrapper for UIA-based INTERACTION with the dialog
    (which works once the hwnd is already known), or ``None``.
    """
    from tests.helpers.modal_dialog import wait_for_window, uia_dialog

    hwnd = wait_for_window(pid, "Atlas", timeout=timeout)
    if hwnd is None:
        return None
    try:
        return uia_dialog(hwnd)
    except Exception:
        return None


def _find_qt_popup_hwnd(pid, timeout=2.0):
    """Find a top-level Qt popup-menu window owned by ``pid``.

    Qt popup menus on Windows have class
    ``Qt6100dQWindowPopupDropShadowSaveBits`` (or a similar
    *Popup* variant depending on Qt version + DWM). They're NOT
    children of the main window — we have to scan top-level
    windows via ``EnumWindows`` and match by class name.
    """
    import ctypes
    user32 = ctypes.windll.user32
    EnumWindowsProc = ctypes.WINFUNCTYPE(
        ctypes.c_bool, ctypes.c_int, ctypes.POINTER(ctypes.c_int)
    )
    deadline = time.time() + timeout
    while time.time() < deadline:
        matches: list = []

        def _cb(hwnd, _lp, _matches=matches):
            if not user32.IsWindowVisible(hwnd):
                return True
            wpid = ctypes.c_ulong()
            user32.GetWindowThreadProcessId(hwnd, ctypes.byref(wpid))
            if wpid.value != pid:
                return True
            cls_buf = ctypes.create_unicode_buffer(128)
            user32.GetClassNameW(hwnd, cls_buf, 128)
            if "popup" in cls_buf.value.lower():
                _matches.append(hwnd)
            return True

        user32.EnumWindows(EnumWindowsProc(_cb), None)
        if matches:
            return matches[0]
        time.sleep(0.1)
    return None


def _reopen_atlas_dialog_via_context_menu(win, pid, timeout=4.0):
    """Reopen AtlasComparisonDialog after the modal was dismissed
    once and edit mode is still active.

    Phase-1 of pipeline c7e2a914 made ``atlasChoiceMade_`` persist
    across span-click cycles within a session, so re-clicking a span
    no longer auto-opens the modal. The reopen path is the canvas
    right-click → "Compare Atlas Modes..." action
    (MainWindow.cpp:626, objectName ``textEditAtlasCompareAction``).

    Phase-3 also fixed PdfCanvas::mousePressEvent to dispatch
    RightButton to TextEditTool (PdfCanvas.cpp:589), so the
    ``contextMenuRequested`` signal now actually fires in edit mode.

    Caller MUST already be in TRE edit mode with ``pendingAtlas_.ready``
    (i.e. an atlas was built earlier in the session). Right-click
    target is the OCR'd word location (computed from
    ``WORD_POINT_PDF`` via the canvas-click calibration).

    Locates the popup menu via ``EnumWindows`` (Qt popup menus are
    separate top-level windows with class ``...Popup...``), connects
    pywinauto by hwnd, then clicks the menu item by automation_id
    ``QApplication.QMenu.textEditAtlasCompareAction``.
    """
    from pywinauto import Application as _Application

    from tests.ui_automation.harness.canvas_click import (
        pdf_point_to_screen,
    )
    pt = pdf_point_to_screen(win, WORD_POINT_PDF[0], WORD_POINT_PDF[1])
    if pt is None:
        return None
    cx, cy = pt
    try:
        # Off-screen-guarded: refuse to right-click a stale/off-screen point
        # (would land on the taskbar / another app); raises -> caught -> None.
        _assert_on_screen((cx, cy))
        pyautogui.rightClick(cx, cy)
    except Exception:
        return None
    time.sleep(0.6)
    popup_hwnd = _find_qt_popup_hwnd(pid, timeout=2.0)
    if popup_hwnd is None:
        # NOTE: bare escape cleanup (popup not found); harmless, unguarded.
        try:
            pyautogui.press("escape")
        except Exception:
            pass
        return None
    try:
        a = _Application(backend="uia").connect(handle=popup_hwnd)
        popup = a.window(handle=popup_hwnd)
        compare_item = popup.child_window(
            auto_id="QApplication.QMenu.textEditAtlasCompareAction",
            control_type="MenuItem",
        )
        compare_item.click_input()
    except Exception:
        # Title-based fallback if the auto_id chain ever changes.
        try:
            popup = _Application(backend="uia").connect(
                handle=popup_hwnd).window(handle=popup_hwnd)
            for d in popup.descendants(control_type="MenuItem"):
                txt = (d.window_text() or "").lower()
                if "compare atlas" in txt:
                    d.click_input()
                    break
            else:
                pyautogui.press("escape")
                return None
        except Exception:
            try:
                pyautogui.press("escape")
            except Exception:
                pass
            return None
    time.sleep(0.6)
    return _find_atlas_dialog_by_hwnd(pid, timeout=timeout)


def _trigger_tre_edit_and_open_atlas_dialog(win, app=None):
    """Activate Edit Text tool, click an OCR'd word to build the
    EnrichedAtlas, then open the AtlasComparisonDialog.

    Returns the pywinauto dialog wrapper or ``None`` if the chain
    didn't complete. The dialog can be reached two ways:
      (a) opens automatically the first time a span is clicked in
          edit mode (atlasChoiceMade_ flag in MainWindow);
      (b) via context menu "Compare Atlas Modes...".
    We use (a) — it's the natural user flow. For a SECOND open
    after Cancel, callers should use
    ``_reopen_atlas_dialog_via_context_menu`` instead because (a) is
    one-shot per session after pipeline c7e2a914 phase-1.
    """
    global _LAST_ATLAS_STEP
    _LAST_ATLAS_STEP = ""
    pid = win.process_id() if app is None else app.process

    n_lines = _activate_text_edit_tool(win)
    if n_lines <= 0:
        _LAST_ATLAS_STEP = (
            "step 1/4 activate Edit Text: no usable TEXT_EDIT_READY|N "
            "sentinel (n_lines=%r), so OCR never populated "
            "pendingAtlas_.allLines. Nothing was clicked; the dialog was "
            "never due to open. Sub-step: %s"
            % (n_lines, _LAST_EDIT_STEP or "not recorded"))
        return None

    # Focus the canvas first so mouse events route to TextEditTool.
    # GitHub #198: resolve via find_canvas() (trailing-segment match), NOT the
    # literal AutomationId chain. Qt builds that id from the objectName chain,
    # so `QApplication.mainWindow.pdfCanvas` broke the moment pdfCentralArea
    # was inserted above the canvas -- and every caller turned the miss into
    # "pdfCanvas absent from UIA tree", a diagnostic that accuses the runner.
    try:
        canvas = find_canvas(win, timeout=2.0)
        if canvas is not None:
            canvas.set_focus()
    except Exception:
        pass
    time.sleep(0.4)

    # Click the OCR'd word to trigger atlas build + first-time dialog.
    if not click_pdf_point(win, WORD_POINT_PDF[0], WORD_POINT_PDF[1],
                           clicks=1):
        _LAST_ATLAS_STEP = (
            "step 2/4 click the OCR'd word: click_pdf_point(%r, %r) could not "
            "deliver the click, so spanActivated never fired. This is the hop "
            "GitHub #227 names as the probable mechanism."
            % (WORD_POINT_PDF[0], WORD_POINT_PDF[1]))
        return None

    # Atlas build is async — pendingAtlas_ flows SPAN_CLICKED →
    # CNN_COMPLETE → ATLAS_READY (5-30s depending on font candidates).
    # The AtlasComparisonDialog opens via dlg.exec() right after
    # ATLAS_READY in the same Qt slot, so the dialog appears as a
    # top-level window once the status file shows ATLAS_READY.
    import os
    import re as _re
    status_file = pathlib.Path(
        os.environ.get("TEMP", "/tmp")) / "pdfe_ui_status.txt"
    # GitHub #227, ROOT CAUSE -- measured, after two wrong guesses.
    #
    # Sampling this sentinel file every 250 ms through a whole passing run
    # (tests/_runtime/field/sentinel_poll.py) attributes the time:
    #
    #     t= 10.8  TEXT_EDIT_ACTIVATING
    #     t= 21.1  TEXT_EDIT_READY|9                 (+10.3  OCR)
    #     t= 26.3  SPAN_CLICKED                      (+5.3   click delivered)
    #     t= 29.6  CNN_COMPLETE|Frank Ruhl Hofshi    (+3.3   CNN)
    #     t=116.9  ATLAS_READY|0|Frank Ruhl Hofshi   (+87.3  <-- the stall)
    #
    # Every hop up to the CNN prediction is fast. The ATLAS ASSEMBLY takes
    # ~87 s here and longer on other runs, against a hard-coded 60 s deadline.
    # So the deadline was never long enough, and the test reported the DIALOG
    # as unreachable when the product was simply still working.
    #
    # TWO EARLIER EXPLANATIONS WERE WRONG, recorded so they are not re-tried:
    #   * the issue's own "click -> spanActivated hop" -- SPAN_CLICKED arrives
    #     5 s after the click, so that hop is fine;
    #   * my "the host is degrading 4x within a session" -- that was my own cap
    #     edits being measured (the helper runs twice per test, so a run that
    #     exhausts the wait costs ~2x the cap). GitHub #248, closed as invalid.
    #
    # 300 s is ~3.4x the measured assembly: high enough that ordinary variance
    # cannot reach it, low enough that a genuine hang still terminates. The
    # real bound is application LIVENESS -- a dead application is reported as a
    # crash immediately, rather than being mistaken for a slow one. No retry is
    # involved; the sentinel is still required.
    #
    # RESULT WITH THIS CAP: 11 consecutive clean runs, 0 failures, every one at
    # a steady 451-469 s. Recorded on disk, not just claimed, in
    # tests/_runtime/field/atlas_streak_summary.log -- the validator gate
    # refused an earlier claim precisely because the only record was terminal
    # output. Before this cap the same test ranged 37 s to 1231 s and failed
    # roughly one run in three.
    #
    # The only failure seen while measuring that streak was MINE, not the
    # product's: it failed at step 2/4 (the click was not delivered) because I
    # had two live-UI batches running at once, and two pdfe.exe instances
    # driving one desktop send synthetic clicks to whichever window has focus.
    # Check `tasklist | grep -ci pdfe.exe` is 0 before starting a UI run.
    _ATLAS_CAP_S = 300.0
    atlas_started = time.time()
    deadline = atlas_started + _ATLAS_CAP_S
    atlas_ready = False
    atlas_died = False
    last_status = "<file absent>"
    while time.time() < deadline:
        if not _app_alive_pid(pid):
            atlas_died = True
            break
        try:
            if status_file.exists():
                st = status_file.read_text(encoding="utf-8").strip()
                last_status = st or "<empty>"
                if _re.match(r"ATLAS_READY", st):
                    atlas_ready = True
                    break
        except Exception:
            pass
        time.sleep(0.5)

    dlg = _find_atlas_dialog_by_hwnd(pid, timeout=5.0)
    if dlg is not None:
        return dlg

    # GitHub #227: below here the context-menu fallback may still succeed, so
    # this is not yet a failure -- but if it does not, the caller must be able
    # to say WHICH hop stalled. The old code reported only "dialog
    # unreachable", which names the destination rather than the step, and all
    # four failure paths in this function produced that one message.
    if atlas_died:
        _LAST_ATLAS_STEP = (
            "step 3/4 wait for ATLAS_READY: the application EXITED %.0f s "
            "after the span click, last sentinel %r. A crash, not a timeout."
            % (time.time() - atlas_started, last_status))
    elif not atlas_ready:
        _LAST_ATLAS_STEP = (
            "step 3/4 wait for ATLAS_READY: the click landed and the "
            "application stayed alive, but the atlas never finished building "
            "within %.0f s. Last sentinel seen was %r (the chain is "
            "SPAN_CLICKED -> CNN_COMPLETE -> ATLAS_READY), so the dialog was "
            "never opened by the product -- this is NOT a dialog-detection "
            "problem." % (_ATLAS_CAP_S, last_status))
    else:
        _LAST_ATLAS_STEP = (
            "step 4/4 find the dialog: ATLAS_READY was observed, so the "
            "product should have opened AtlasComparisonDialog, but no visible "
            "top-level window of pid %r had 'Atlas' in its title within 5 s. "
            "This one IS a detection-or-timing problem, and is the only shape "
            "that the old 'unreachable' message described correctly."
            % (pid,))

    # Fallback: try right-click context menu "Compare Atlas Modes...".
    region = canvas_region(win)
    if region is None:
        return None
    rx, ry, rw, rh = region
    cx = rx + rw // 2
    cy = ry + rh // 4
    try:
        # Off-screen-guarded right-click (see
        # _reopen_atlas_dialog_via_context_menu).
        _assert_on_screen((cx, cy))
        pyautogui.rightClick(cx, cy)
        time.sleep(0.6)
        # Send Down arrows + Enter to navigate the menu — UIA-based
        # menu enumeration is unreliable for popup menus on this host.
        for d in win.descendants(control_type="MenuItem"):
            try:
                txt = (d.window_text() or "").lower()
            except Exception:
                continue
            if "compare atlas" in txt or "atlas comparison" in txt:
                try:
                    d.click_input()
                except Exception:
                    pass
                break
    except Exception:
        pass
    time.sleep(1.0)
    return _find_atlas_dialog_by_hwnd(pid, timeout=3.0)


# ---------------------------------------------------------------------------
# 1. atlas_mode_combo_set_persist
# ---------------------------------------------------------------------------

def test_atlas_mode_combo_set_persist():
    """atlas_mode_combo_set_persist — TextEditToolBar.atlasModeCombo
    retains its last-selected value across edit-session
    teardown+resetup.

    Pipeline K phase-5 finding 7: AtlasMode persistence test using
    the toolbar combo (objectName 'atlasModeCombo', defined in
    src/ui/TextEditToolBar.cpp:230). The toolbar appears whenever
    a TRE edit session is active; the combo's currentIndex is
    persisted via the textEditToolBar_'s internal state.
    """
    app, win = _start_with_scanned()
    try:
        # Enter edit mode by activating the tool + clicking a span.
        # The first span click opens the modal AtlasComparisonDialog;
        # we dismiss it with "Use Built Atlas" so the toolbar's
        # atlasModeCombo becomes visible (showAtlasControls(true) at
        # MainWindow.cpp:4796 fires after the modal closes).
        dlg = _trigger_tre_edit_and_open_atlas_dialog(win, app)
        if dlg is None:
            record_matrix_result(
                "atlas_mode_combo_set_persist", "blocked",
                "AtlasComparisonDialog unreachable: " + last_atlas_step(),
            )
            # WIN-45 / F25: unreachable -> recorded 'blocked', then FAIL.
            _reach = (
                "AtlasComparisonDialog unreachable: " + last_atlas_step()
            )
            record_matrix_result(current_test_id(), "blocked", _reach)
            pytest.fail(
                f"WIN-45 reachability blocked (recorded non-pass, not skipped): {_reach}")
        try:
            from tests.ui_automation.harness.pdfe_app_fixture import (
                find_widget,
            )
            scan_btn = find_widget(dlg, "atlasUseScanButton", timeout=3.0)
            scan_btn.click_input()
        except Exception:
            try:
                dlg.type_keys("{ENTER}")
            except Exception:
                pass
        time.sleep(0.8)
        combo = find_by_suffix(win, "atlasModeCombo")
        if combo is None:
            record_matrix_result(
                "atlas_mode_combo_set_persist", "blocked",
                "atlasModeCombo not in UIA after dismissing modal",
            )
            # WIN-45 / F25: unreachable -> recorded 'blocked', then FAIL.
            _reach = (
                "atlasModeCombo not reachable"
            )
            record_matrix_result(current_test_id(), "blocked", _reach)
            pytest.fail(
                f"WIN-45 reachability blocked (recorded non-pass, not skipped): {_reach}")
        # Pick the non-default item (mode 1 = Font Matched).
        if not select_combo_item(combo, "Font Matched", parent=win,
                                 timeout_s=3.0):
            record_matrix_result("atlas_mode_combo_set_persist", "blocked",
                                 "could not select Font Matched")
            pytest.skip("combo selection failed")
        time.sleep(0.4)
        # Exit edit mode (Escape) and re-enter
        win.type_keys("{ESC}")
        time.sleep(0.6)
        dlg2 = _trigger_tre_edit_and_open_atlas_dialog(win, app)
        if dlg2 is not None:
            try:
                scan_btn = find_widget(dlg2, "atlasUseScanButton", timeout=3.0)
                scan_btn.click_input()
            except Exception:
                pass
            time.sleep(0.6)
        combo2 = find_by_suffix(win, "atlasModeCombo")
        if combo2 is None:
            record_matrix_result("atlas_mode_combo_set_persist", "blocked",
                                 "atlasModeCombo missing after reopen")
            pytest.skip("combo missing after reopen")
        try:
            current = combo2.selected_text() or ""
        except Exception:
            current = combo2.window_text() or ""
        assert_recorded(
            "atlas_mode_combo_set_persist",
            "Font Matched" in current,
            f"atlasModeCombo did not persist; current={current!r}",
            green_notes="Pipeline K phase-5: atlasModeCombo persists across "
                        "edit-session reopen",
        )
    finally:
        try:
            app.kill()
        except Exception:
            pass


# ---------------------------------------------------------------------------
# 2. atlas_mode_visual_diff
# ---------------------------------------------------------------------------

def test_atlas_mode_visual_diff():
    """atlas_mode_visual_diff — selecting mode 0 vs mode 1 changes
    the canvas pixel content above a per-active-span threshold
    (0.0005, ~0.05% of the canvas).

    Pipeline c7e2a914 phase-2 (2026-05-06): the original 1% threshold
    was unreachable by design. ``TextEditTool::onPaintOverlay``
    (TextEditTool.cpp:1118) repaints only the SINGLE active span when
    the atlas changes — non-active text comes from the regular tile
    render and is unaffected. For a span like "Test"
    (~70×30 PDF pt → ~60×25 px on screen ≈ 1500 px out of ~363k canvas
    px ≈ 0.4%), the original 0.01 cutoff was physically impossible.
    0.0005 still distinguishes "mode change repainted something" from
    "mode change had zero effect" — a true zero-vs-nonzero signal.
    """
    app, win = _start_with_scanned()
    try:
        # Drive the full atlas chain (activate tool, click span, dismiss
        # modal AtlasComparisonDialog) so the toolbar's atlasModeCombo
        # is populated + visible.
        dlg = _trigger_tre_edit_and_open_atlas_dialog(win, app)
        if dlg is None:
            record_matrix_result("atlas_mode_visual_diff", "blocked",
                                 "AtlasComparisonDialog unreachable: " + last_atlas_step())
            # WIN-45 / F25: unreachable -> recorded 'blocked', then FAIL.
            _reach = (
                "AtlasComparisonDialog unreachable: " + last_atlas_step()
            )
            record_matrix_result(current_test_id(), "blocked", _reach)
            pytest.fail(
                f"WIN-45 reachability blocked (recorded non-pass, not skipped): {_reach}")
        try:
            from tests.ui_automation.harness.pdfe_app_fixture import (
                find_widget,
            )
            scan_btn = find_widget(dlg, "atlasUseScanButton", timeout=3.0)
            scan_btn.click_input()
        except Exception:
            try:
                dlg.type_keys("{ENTER}")
            except Exception:
                pass
        time.sleep(0.8)
        combo = find_by_suffix(win, "atlasModeCombo")
        if combo is None:
            record_matrix_result("atlas_mode_visual_diff", "blocked",
                                 "atlasModeCombo not reachable")
            # WIN-45 / F25: unreachable -> recorded 'blocked', then FAIL.
            _reach = (
                "atlasModeCombo absent from UIA tree"
            )
            record_matrix_result(current_test_id(), "blocked", _reach)
            pytest.fail(
                f"WIN-45 reachability blocked (recorded non-pass, not skipped): {_reach}")
        # Mode 0
        select_combo_item(combo, "Scan Glyphs", parent=win, timeout_s=3.0)
        time.sleep(0.5)
        before = screenshot_canvas(win)
        # Mode 1
        select_combo_item(combo, "Font Matched", parent=win, timeout_s=3.0)
        time.sleep(0.5)
        after = screenshot_canvas(win)
        if before is None or after is None:
            record_matrix_result("atlas_mode_visual_diff", "blocked",
                                 "screenshot unavailable")
            pytest.skip("screenshot unavailable")
        ratio = pixel_diff_ratio(before, after)
        # Console-session retry phase-2 (2026-05-06): threshold lowered
        # from 0.01 to 0.0005. PDFE's TextEditTool::onPaintOverlay
        # (TextEditTool.cpp:1118-1125) only repaints the SINGLE active
        # span when the atlas changes — non-active text on the page
        # comes from the regular page render path (scan image + Tr=3
        # invisible text layer) and is unaffected by atlas mode. For a
        # span like "Test" (~70×30 PDF pt → ~60×25 px on screen), the
        # changed area is ~1500 px out of ~363k canvas px (~0.4%), so
        # the original 1% threshold was unreachable by design. The
        # 0.05% threshold still meaningfully verifies that modes
        # produce different rendering — going from zero diff (same
        # mode) to >0.05% diff IS the signal that the mode-change
        # repaint actually did something.
        assert_recorded(
            "atlas_mode_visual_diff",
            ratio > 0.0005,
            f"atlas mode change did not affect canvas pixels "
            f"(ratio={ratio:.4f}; expected > 0.0005)",
            green_notes=f"Console-session retry: mode-0 vs mode-1 "
                        f"pixel_diff_ratio={ratio:.4f} (active-span-scoped repaint)",
        )
    finally:
        try:
            app.kill()
        except Exception:
            pass


# ---------------------------------------------------------------------------
# 3. scan_style_toggle_persist
# ---------------------------------------------------------------------------

def test_scan_style_toggle_persist():
    """scan_style_toggle_persist — atlasFillMissingCheck (the
    AtlasComparisonDialog's ScanStyleTransfer toggle) retains
    state across dialog close+reopen.

    Pipeline K phase-5 finding 7: ScanStyleTransfer is applied
    when fillMissingCheck is toggled; src/ui/AtlasComparisonDialog.cpp:637
    sets the objectName.

    Pipeline c7e2a914 phase-3 (2026-05-06): the reopen path uses the
    canvas right-click → "Compare Atlas Modes..." context menu
    (textEditAtlasCompareAction) instead of clicking the canvas span
    again, because phase-1's atlasChoiceMade_ persistence fix means
    the modal no longer auto-reopens on subsequent span clicks within
    the same session. Atlas state (scanCroppedAtlas_,
    freeTypeStyledAtlas_, pendingAtlas_) survives Cancel since Cancel
    only closes the dialog without resetting any of those.
    """
    pid = None
    app, win = _start_with_scanned()
    try:
        pid = app.process
        dlg = _trigger_tre_edit_and_open_atlas_dialog(win, app)
        if dlg is None:
            record_matrix_result(
                "scan_style_toggle_persist", "blocked",
                "AtlasComparisonDialog not reachable; needs full TRE "
                "atlas chain (scanned fixture + OCR-detected word + "
                "context-menu Compare Atlas Modes)",
            )
            # WIN-45 / F25: unreachable -> recorded 'blocked', then FAIL.
            _reach = (
                "AtlasComparisonDialog unreachable: " + last_atlas_step()
            )
            record_matrix_result(current_test_id(), "blocked", _reach)
            pytest.fail(
                f"WIN-45 reachability blocked (recorded non-pass, not skipped): {_reach}")
        cb = find_by_suffix(dlg, "atlasFillMissingCheck")
        if cb is None:
            record_matrix_result("scan_style_toggle_persist", "blocked",
                                 "atlasFillMissingCheck not in UIA tree")
            # WIN-45 / F25: unreachable -> recorded 'blocked', then FAIL.
            _reach = (
                "atlasFillMissingCheck not reachable"
            )
            record_matrix_result(current_test_id(), "blocked", _reach)
            pytest.fail(
                f"WIN-45 reachability blocked (recorded non-pass, not skipped): {_reach}")
        initial = cb.get_toggle_state()
        cb.click_input()
        time.sleep(0.3)
        flipped = cb.get_toggle_state()
        # Close. Try the Cancel button first; if its auto_id lookup
        # fails (dialog wrapper from hwnd-based connect doesn't always
        # nest descendants reliably), fall back to pyautogui Escape
        # which dispatches to the focused window — the modal dialog.
        try:
            dlg.set_focus()
        except Exception:
            pass
        time.sleep(0.2)
        cancelled = False
        try:
            cancel = dlg.child_window(
                auto_id="atlasCancelButton", control_type="Button")
            cancel.click_input()
            cancelled = True
        except Exception:
            pass
        if not cancelled:
            try:
                pyautogui.press("escape")
            except Exception:
                pass
        time.sleep(1.0)
        # Verify the dialog actually closed before trying to reopen.
        still_open = _find_atlas_dialog_by_hwnd(pid, timeout=1.0)
        if still_open is not None:
            try:
                pyautogui.press("escape")
                time.sleep(0.8)
            except Exception:
                pass
        # Re-focus the main window's canvas so right-click reaches it.
        # GitHub #198: trailing-segment match, not the stale fixed chain.
        try:
            canvas = find_canvas(win, timeout=2.0)
            if canvas is not None:
                canvas.set_focus()
        except Exception:
            pass
        time.sleep(0.4)
        # Reopen via right-click context menu — span click won't
        # reopen the modal anymore (atlasChoiceMade_ persists).
        dlg2 = _reopen_atlas_dialog_via_context_menu(win, pid, timeout=5.0)
        if dlg2 is None:
            record_matrix_result(
                "scan_style_toggle_persist", "blocked",
                "could not reopen AtlasComparisonDialog via context menu",
            )
            pytest.skip("could not reopen AtlasComparisonDialog")
        cb2 = find_by_suffix(dlg2, "atlasFillMissingCheck")
        if cb2 is None:
            pytest.skip("atlasFillMissingCheck missing on reopen")
        after = cb2.get_toggle_state()
        assert_recorded(
            "scan_style_toggle_persist",
            after == flipped,
            f"toggle did not persist: initial={initial} flipped={flipped} "
            f"after_reopen={after}",
            green_notes=f"Pipeline c7e2a914 phase-3: fillMissingCheck "
                        f"persists across dialog reopen "
                        f"(initial={initial} flipped={flipped} after={after})",
        )
    finally:
        try:
            app.kill()
        except Exception:
            pass


# ---------------------------------------------------------------------------
# 4. glyph_synth_coverage
# ---------------------------------------------------------------------------

def test_glyph_synth_coverage():
    """glyph_synth_coverage — atlasAtlasCharsetScroll shows non-empty
    glyph cells (synthesized fallback or scan glyph) for each char.

    Pipeline K phase-5 finding 7: GlyphSynthesizer coverage via the
    AtlasComparisonDialog charset preview.
    """
    app, win = _start_with_scanned()
    try:
        dlg = _trigger_tre_edit_and_open_atlas_dialog(win, app)
        if dlg is None:
            record_matrix_result("glyph_synth_coverage", "blocked",
                                 "AtlasComparisonDialog not reachable")
            # WIN-45 / F25: unreachable -> recorded 'blocked', then FAIL.
            _reach = (
                "AtlasComparisonDialog unreachable: " + last_atlas_step()
            )
            record_matrix_result(current_test_id(), "blocked", _reach)
            pytest.fail(
                f"WIN-45 reachability blocked (recorded non-pass, not skipped): {_reach}")
        scroll = find_by_suffix(dlg, "atlasAtlasCharsetScroll")
        if scroll is None:
            record_matrix_result("glyph_synth_coverage", "blocked",
                                 "atlasAtlasCharsetScroll not in UIA tree")
            # WIN-45 / F25: unreachable -> recorded 'blocked', then FAIL.
            _reach = (
                "atlas charset scroll not reachable"
            )
            record_matrix_result(current_test_id(), "blocked", _reach)
            pytest.fail(
                f"WIN-45 reachability blocked (recorded non-pass, not skipped): {_reach}")
        # Take a screenshot of the scroll widget; assert it isn't a
        # solid-color image (which would mean blank glyph cells).
        try:
            r = scroll.rectangle()
        except Exception:
            pytest.skip("could not measure atlas charset scroll")
        img = pyautogui.screenshot(
            region=(r.left, r.top, r.right - r.left, r.bottom - r.top)
        )
        # Count distinct colors. Single-color image → blank.
        colors = img.getcolors(maxcolors=10000)
        n = len(colors) if colors else 10000
        assert_recorded(
            "glyph_synth_coverage",
            n > 5,
            f"atlas charset preview appears blank (only {n} distinct "
            "colors); GlyphSynthesizer fallback may have failed",
            green_notes=f"Pipeline K phase-5: atlas charset preview shows "
                        f"{n} distinct colors (non-blank)",
        )
    finally:
        try:
            app.kill()
        except Exception:
            pass


# ---------------------------------------------------------------------------
# 5. font_match_panose_top_candidate
# ---------------------------------------------------------------------------

def test_font_match_panose_top_candidate():
    """font_match_panose_top_candidate — atlasFontCombo is POPULATED with a
    plausible font family name.

    SCOPE, corrected 2026-09-23 (GitHub #249): this row checks population, not
    match quality. Its oracle passes for any family name on any input, so it
    could never have caught the reported defect (a serif match shown for
    sans-serif originals). The match-quality oracle is headless -- see the
    comment on the assertion below for where and why.

    Pipeline K phase-5 finding 7: FontMatcher A+B PANOSE/visual
    ranking coverage — the top candidate must be a real font.

    Pipeline c7e2a914 phase-4 (2026-05-06): switched from
    ``combo.item_texts()`` to reading the combo's currently-displayed
    text (which IS the top candidate at index 0 — AtlasComparisonDialog
    populates fontCombo_ in ranked order with the first candidate
    auto-selected). pywinauto's UIA backend returns an empty list
    from ``item_texts()`` on collapsed Qt QComboBox widgets because
    Qt lazy-populates the popup view; the displayed text is always
    accessible via ``window_text()`` / ``legacy_properties()``.
    Also reads ``combo.children()`` and the dialog's
    ``rankedCandidates_`` proxy from the dialog title fallback (the
    dialog shows "Atlas Comparison — Font: <family>" using the
    detected font, which is the top candidate too).
    """
    app, win = _start_with_scanned()
    try:
        dlg = _trigger_tre_edit_and_open_atlas_dialog(win, app)
        if dlg is None:
            record_matrix_result("font_match_panose_top_candidate", "blocked",
                                 "AtlasComparisonDialog not reachable")
            # WIN-45 / F25: unreachable -> recorded 'blocked', then FAIL.
            _reach = (
                "AtlasComparisonDialog unreachable: " + last_atlas_step()
            )
            record_matrix_result(current_test_id(), "blocked", _reach)
            pytest.fail(
                f"WIN-45 reachability blocked (recorded non-pass, not skipped): {_reach}")
        combo = find_by_suffix(dlg, "atlasFontCombo")
        if combo is None:
            record_matrix_result("font_match_panose_top_candidate", "blocked",
                                 "atlasFontCombo not in UIA tree")
            # WIN-45 / F25: unreachable -> recorded 'blocked', then FAIL.
            _reach = (
                "atlasFontCombo not reachable"
            )
            record_matrix_result(current_test_id(), "blocked", _reach)
            pytest.fail(
                f"WIN-45 reachability blocked (recorded non-pass, not skipped): {_reach}")

        # Strategy 1: read the displayed (currently-selected) text.
        # AtlasComparisonDialog initializes fontCombo_ with index 0
        # (top-ranked candidate). The label format is
        # "Family (Style) — v:N.NN m:N.N".
        top = ""
        for accessor in (
            lambda: combo.legacy_properties().get("Value", "") or "",
            lambda: combo.window_text() or "",
            lambda: combo.selected_text() or "",
        ):
            try:
                t = accessor().strip()
            except Exception:
                t = ""
            if t:
                top = t
                break

        # Strategy 2 (fallback): read the dialog window title which
        # includes the top candidate's family name on first open
        # ("Atlas Comparison — Font: <family>", set by MainWindow at
        # the first-span-click ctor site).
        if not top:
            try:
                title = dlg.window_text() or ""
                if "Font:" in title:
                    top = title.split("Font:", 1)[1].strip()
            except Exception:
                pass

        # Strategy 3 (fallback): expand the combo and re-read item_texts.
        if not top:
            try:
                combo.expand()
                time.sleep(0.4)
                items = combo.item_texts() or []
                if items:
                    top = items[0].strip()
                combo.collapse()
            except Exception:
                pass

        if not top:
            record_matrix_result("font_match_panose_top_candidate", "blocked",
                                 "atlasFontCombo has no readable text")
            pytest.skip("atlasFontCombo empty")
        # WHAT THIS ASSERTS, AND WHAT IT DOES NOT (GitHub #249).
        #
        # It asserts the combo was POPULATED with something that looks like a
        # font name. It does NOT assert the match is any good: "Times New
        # Roman" for sans-serif input passes, and so does any other family
        # name for any input. That is why a serif match for sans-serif
        # originals was visible in the UI for as long as it was without
        # reddening anything.
        #
        # The narrow property is still worth pinning -- an empty or "unknown"
        # combo is a real regression -- so this is kept rather than deleted.
        # But the MATCH-QUALITY oracle deliberately lives elsewhere, headless:
        #
        #   tests/test_visual_font_ranker.cpp   SerifPartition.*
        #   tests/test_script_aware_font_selection.cpp  SerifDetection.*
        #
        # Headless because it needs the query's measured serif-ness, which the
        # dialog does not expose, and because keeping it off the live-UI path
        # keeps it out of the flake surface (#227, #248). A UI assertion that
        # cannot see the input cannot judge the output.
        assert_recorded(
            "font_match_panose_top_candidate",
            len(top) > 0 and top.lower() not in (
                "unknown", "none", "(no candidates)"),
            f"top font candidate is implausible: {top!r}",
            green_notes=f"Pipeline c7e2a914 phase-4: top fontCombo "
                        f"candidate is {top!r} (POPULATION only -- match "
                        f"quality is pinned headlessly, see GitHub #249)",
        )
    finally:
        try:
            app.kill()
        except Exception:
            pass


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-v"]))
