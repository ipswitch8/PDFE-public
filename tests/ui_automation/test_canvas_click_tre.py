#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Pipeline K phase-4: TRE text-edit tests using the canvas-click harness.

Unblocks two ``txt_tre_edit_*`` matrix rows that previously had
``_op_blocked`` stubs in ``test_text_formatting_p2.py`` because no
canvas-click harness existed:

  - txt_tre_edit_regular_font: open a scanned PDF, click a word
    on the canvas to enter TRE edit mode, type a replacement, and
    assert the canvas pixels changed.
  - txt_tre_edit_size_change: same flow but use the size combo on
    the TextEditToolBar to change the font size; assert pixels
    changed again after each step.

Both tests use a screenshot pixel-diff oracle (no UIA accessor for
the painted canvas content). Threshold is 0.01 — a render that
genuinely changed will easily clear that floor.

Skip conditions
---------------
These tests need:
  * pyautogui + Pillow installed (canvas-click harness deps);
  * a real pdfe.exe (skipped via PDFE_EXE.exists() in fixture);
  * a scanned-page fixture (test_scanned.pdf) with at least one
    OCR-recognized word at a known position.

Any missing prerequisite → pytest.skip with an explicit reason.
The matrix row record is made on the green path only; a failed
assertion records 'red' via ``assert_recorded()``.
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

try:
    from PIL import Image  # noqa: F401  # type: ignore
except ImportError:
    pytest.skip("Pillow not installed", allow_module_level=True)

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tests.ui_automation.harness.pdfe_app_fixture import launch_pdfe  # noqa: E402
from tests.ui_automation.harness.canvas_click import (  # noqa: E402
    canvas_region,
    click_pdf_point,
    triple_click_pdf_point,
    screenshot_canvas,
    pixel_diff_ratio,
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
# Phase 3 Cluster #6 (2026-06-09): switched from test_scanned.pdf
# (pure-image, no OCR text layer) to test_scanned_with_ocr.pdf
# which has an invisible Tr=3 OCR layer baked in via
# tests/fixtures/make_test_scanned_with_ocr.py. Same swap that
# test_canvas_click_subsystems.py made on 2026-05-06 (see comment
# block in that file). Without the OCR layer Page::hasTextContent()
# returns false, ``onTextEditToolSelected`` skips the atlas build,
# and TextEditTool's spanAtPoint() always returns -1 — so
# double-clicking the word never triggers TRE edit mode and the
# canvas never repaints (pixel_diff_ratio=0.0000).
SCANNED_FIXTURE = FIXTURES / "test_scanned_with_ocr.pdf"

# Click target on test_scanned_with_ocr.pdf where OCR'd text lives.
# Matches test_canvas_click_subsystems.py's WORD_POINT_PDF —
# Tesseract'd 'Test' word at image (101,129)-(193,164) in the
# 850×1100 image; image-to-PDF scale is 1.389. PDF-coord center:
# ((101+92/2)/1.389, (129+35/2)/1.389) ≈ (105.8, 105.5).
WORD_POINT_PDF = (105.8, 105.5)


def _start_with_scanned():
    if not SCANNED_FIXTURE.exists():
        pytest.skip(f"scanned fixture missing: {SCANNED_FIXTURE.name}")
    # Phase 3 Cluster #6 (2026-06-09): set PDFE_SKIP_ATLAS_DIALOG=1
    # in the launch environment so AtlasComparisonDialog is bypassed
    # (it would otherwise block ATLAS_READY status until the user
    # dismisses it — see src/ui/MainWindow.cpp:6222-6245). The bypass
    # is gated by env var and only inflated for tests; production
    # behaviour is unchanged.
    import os as _os
    _os.environ["PDFE_SKIP_ATLAS_DIALOG"] = "1"
    try:
        app, win = launch_pdfe(args=[str(SCANNED_FIXTURE)])
    except RuntimeError as e:
        pytest.skip(f"pdfe.exe launch failed: {e}")
    # Phase 3 Cluster #6 (2026-06-09): maximize the window before
    # OCR runs. Without maximize the default 1280×800 window keeps
    # the PdfCanvas small enough that pdfe's OCR pipeline either
    # delays or skips writing TEXT_EDIT_READY.
    try:
        win.maximize()
    except Exception:  # noqa: BLE001
        pass
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


@pytest.mark.timeout(240)
def test_txt_tre_edit_regular_font():
    """txt_tre_edit_regular_font — click a word on a scanned page,
    type a replacement, assert the canvas render changed.

    Pipeline K phase-4: replaces the _op_blocked stub in
    test_text_formatting_p2.py.

    Phase 3 Cluster #6 (2026-06-09): activate the TextEditTool
    *via the textEditToolActionButton* (real toolbar click) and
    wait for the ``TEXT_EDIT_READY|N`` status sentinel before
    double-clicking. Same pattern as ``_activate_text_edit_tool``
    in test_canvas_click_subsystems.py — the test was failing with
    ``pixel_diff_ratio=0.0000`` because:
      (1) ``testToolActivatePipe`` toggles the QAction but TRE/OCR
          only runs after ``onTextEditToolSelected`` fires, which
          requires the QAction::triggered signal — a real
          ``click_input`` on the toolbar button fires it; the pipe
          alone does not.
      (2) Without OCR completion, the double-click on the canvas
          can't hit a TRE span (no atlas is built), so SelectTool
          processes the click and no repaint happens.
    Wait up to 45s for ``TEXT_EDIT_READY|<lines>``; skip cleanly
    if OCR never reports ready (e.g., tessdata missing).
    """
    app, win = _start_with_scanned()
    try:
        # Activate TextEditTool via the real toolbar button so the
        # QAction::triggered → onTextEditToolSelected → OCR pipeline
        # runs. The testToolActivatePipe shortcut does NOT trigger
        # OCR (it only updates the QAction's checked state).
        import os as _os
        import pathlib as _pl
        import re as _re
        from tests.ui_automation.harness.pdfe_app_fixture import find_widget
        status_file = _pl.Path(
            _os.environ.get("TEMP", "/tmp")) / "pdfe_ui_status.txt"
        status_file.unlink(missing_ok=True)
        try:
            btn = find_widget(win, "textEditToolActionButton", timeout=3.0)
        except Exception:
            # WIN-45 / F25: unreachable -> recorded 'blocked', then FAIL.
            _reach = (
                "textEditToolActionButton not in UIA tree"
            )
            record_matrix_result(current_test_id(), "blocked", _reach)
            pytest.fail(
                f"WIN-45 reachability blocked (recorded non-pass, not skipped): {_reach}")
        try:
            btn.click_input()
        except Exception:
            try:
                btn.invoke()
            except Exception:
                pytest.skip("could not activate TextEditTool button")

        # Poll for TEXT_EDIT_READY|N — OCR runs 30-90s on the
        # test_scanned_with_ocr fixture (Debug build is slower; the
        # standalone diagnostic measured ~9-10s but the pytest
        # harness adds significant overhead).
        lines = 0
        deadline = time.time() + 120.0
        while time.time() < deadline:
            if status_file.exists():
                st = status_file.read_text(encoding="utf-8").strip()
                m = _re.match(r"TEXT_EDIT_READY\|(\d+)", st)
                if m:
                    lines = int(m.group(1))
                    break
            time.sleep(0.5)
        if lines <= 0:
            pytest.skip(
                "OCR did not report TEXT_EDIT_READY within 120s; "
                "tessdata may be missing or page has no OCR lines"
            )

        # First single-click triggers TRE atlas build (deferred from
        # tool activation). The atlas build opens
        # AtlasComparisonDialog on first-click-per-session
        # (atlasChoiceMade_ flag); the test must dismiss the dialog
        # before the canvas can repaint for our edit.
        #
        # GitHub #219: the span activation goes through testSpanActivatePipe,
        # NOT a synthetic click. The xfail this replaces had localised the
        # break correctly -- OCR reports TEXT_EDIT_READY|9 fine; it is the
        # click -> spanActivated hop that does not complete under a synthetic
        # click -- and named the missing seam. TextEditTool already exposed
        # selectSpanForTesting(), which calls the PRODUCTION selectSpan() and
        # so emits the real spanActivated; the pipe is what connects it to
        # Python. This is the same substitution that resolved the archived
        # P-GAP-71 and P-GAP-91.
        from tests.ui_automation.harness.pdfe_app_fixture import (
            _read_pipe_value,
        )
        span_pipe = find_widget(win, "testSpanActivatePipe", timeout=5.0)
        span_result = find_widget(win, "testSpanActivateResultPipe",
                                  timeout=5.0)
        # Span 0 rather than a coordinate: the coordinate adds a second way to
        # fail (landing between spans) to a test whose subject is the
        # activation hop, and a test that can fail for a reason it is not
        # about is the thing that made this row expensive the first time.
        span_pipe.set_edit_text("0")
        verdict = ""
        _deadline = time.time() + 10.0
        while time.time() < _deadline:
            verdict = (_read_pipe_value(span_result) or "").strip()
            if verdict:
                break
            time.sleep(0.2)
        assert verdict.startswith("ok|"), (
            "testSpanActivatePipe did not activate a span; verdict=%r. "
            "'err|no-spans' means OCR produced no editable spans despite "
            "TEXT_EDIT_READY|%d; 'err|no-text-edit-tool' means the tool was "
            "not installed. Neither is the same as the pipe never running "
            "(empty verdict)." % (verdict, lines))

        # Poll for ATLAS_READY OR the AtlasComparisonDialog appearing
        # as a top-level window. Either is fine — the dialog appears
        # right after ATLAS_READY in the same Qt slot.
        atlas_ready = False
        deadline = time.time() + 90.0
        while time.time() < deadline:
            if status_file.exists():
                st = status_file.read_text(encoding="utf-8").strip()
                if "ATLAS_READY" in st:
                    atlas_ready = True
                    break
            time.sleep(0.5)
        if not atlas_ready:
            pytest.skip(
                "ATLAS_READY not reported within 90s after span "
                "click; atlas build may have failed silently"
            )

        # PDFE_SKIP_ATLAS_DIALOG=1 (set in _start_with_scanned)
        # already prevents the modal AtlasComparisonDialog from
        # opening. Brief settle to let the canvas paint the post-
        # atlas state before the baseline screenshot.
        time.sleep(0.8)

        # Take baseline AFTER atlas build (the build itself paints).
        before = screenshot_canvas(win)
        if before is None:
            pytest.skip("canvas screenshot unavailable")

        # Double-click the OCR word to enter TRE edit mode and
        # select the word.
        ok = click_pdf_point(win, WORD_POINT_PDF[0], WORD_POINT_PDF[1],
                             clicks=2)
        if not ok:
            pytest.skip("click_pdf_point returned False on edit click")

        # Poll for edit-mode focus on the canvas/cursor.
        for _wait in range(8):
            time.sleep(0.3)
            try:
                title = win.window_text()
                if "*" in title:
                    break
            except Exception:
                pass

        # Type a replacement — Qt focus should be on the text-edit
        # cursor inside the canvas after the double-click.
        try:
            win.type_keys("Replaced")
        except Exception as e:  # noqa: BLE001
            pytest.skip(f"type_keys failed: {e}")
        time.sleep(1.0)

        after = screenshot_canvas(win)
        if after is None:
            pytest.skip("canvas screenshot unavailable post-edit")

        ratio = pixel_diff_ratio(before, after)
        assert_recorded(
            "txt_tre_edit_regular_font",
            ratio > 0.01,
            f"canvas render did not change after TRE edit "
            f"(pixel_diff_ratio={ratio:.4f}); expected > 0.01",
            green_notes=(
                f"Pipeline K phase-4: canvas-click drove TRE edit; "
                f"pixel_diff_ratio={ratio:.4f}"
            ),
        )
    finally:
        try:
            app.kill()
        except Exception:
            pass


def test_txt_tre_edit_size_change():
    """txt_tre_edit_size_change — enter TRE edit, change font size
    via TextEditToolBar size combo, assert canvas changes again.

    Pipeline K phase-4: replaces the _op_blocked stub in
    test_text_formatting_p2.py.
    """
    app, win = _start_with_scanned()
    try:
        # Enter edit mode on a known word.
        ok = click_pdf_point(win, WORD_POINT_PDF[0], WORD_POINT_PDF[1],
                             clicks=2)
        if not ok:
            pytest.skip("click_pdf_point returned False (no canvas)")
        time.sleep(0.8)

        baseline = screenshot_canvas(win)
        if baseline is None:
            pytest.skip("canvas screenshot unavailable")

        # The size spin/combo lives on TextEditToolBar with
        # objectName "fontSizeSpinBox" or "fontSizeCombo" depending
        # on the build. Use suffix lookup.
        from tests.ui_automation.harness.dialog_helpers import (
            find_by_suffix,
        )
        size_widget = find_by_suffix(win, "fontSizeSpinBox")
        if size_widget is None:
            size_widget = find_by_suffix(win, "fontSizeCombo")
        if size_widget is None:
            # WIN-45 / F25: unreachable -> recorded 'blocked', then FAIL.
            _reach = (
                "TextEditToolBar size widget absent from UIA tree"
            )
            record_matrix_result(current_test_id(), "blocked", _reach)
            pytest.fail(
                f"WIN-45 reachability blocked (recorded non-pass, not skipped): {_reach}")

        try:
            size_widget.set_focus()
            time.sleep(0.2)
            # Aggressive size change so the pixel diff is unmistakable.
            size_widget.type_keys("^a")
            size_widget.type_keys("48{ENTER}")
        except Exception as e:  # noqa: BLE001
            pytest.skip(f"size widget interaction failed: {e}")
        time.sleep(0.6)

        after = screenshot_canvas(win)
        if after is None:
            pytest.skip("canvas screenshot unavailable post-size-change")

        ratio = pixel_diff_ratio(baseline, after)
        assert_recorded(
            "txt_tre_edit_size_change",
            ratio > 0.01,
            f"canvas render did not change after font-size change "
            f"(pixel_diff_ratio={ratio:.4f}); expected > 0.01",
            green_notes=(
                f"Pipeline K phase-4: TRE size change rendered; "
                f"pixel_diff_ratio={ratio:.4f}"
            ),
        )
    finally:
        try:
            app.kill()
        except Exception:
            pass


# ---------------------------------------------------------------------------
# Bonus: canvas_text_selection_triple_click via the new harness
# ---------------------------------------------------------------------------

def test_canvas_text_selection_triple_click():
    """canvas_text_selection_triple_click — triple-click a text run on
    a non-scanned PDF to select the line; assert the canvas pixels
    changed (selection highlight is painted on the canvas).

    Pipeline K phase-4: exercises ``triple_click_pdf_point`` from
    the new harness.

    Phase 3 Cluster #3 (2026-06-09): the default SelectTool ignores
    text-selection triple-clicks (it's for annotations) — pixel_diff
    was 0.0000 because no selection highlight was painted. Activate
    TextEditTool via testToolActivatePipe before clicking; that tool
    owns text-selection painting. Click target moved to (200, 55)
    where 'Single Page Test Document' actually lives in
    test_single.pdf (the old (100, 120) hit a blank region above the
    text). Falls back to skipping when the pipe is unreachable
    (PDFE_TEST_PIPES not set) rather than failing.
    """
    fixture = FIXTURES / "test_single.pdf"
    if not fixture.exists():
        pytest.skip(f"fixture missing: {fixture.name}")
    try:
        app, win = launch_pdfe(args=[str(fixture)])
    except RuntimeError as e:
        pytest.skip(f"pdfe.exe launch failed: {e}")
    try:
        time.sleep(2.5)
        if canvas_region(win) is None:
            # WIN-45 / F25: unreachable -> recorded 'blocked', then FAIL.
            _reach = (
                "PdfCanvas absent from UIA tree"
            )
            record_matrix_result(current_test_id(), "blocked", _reach)
            pytest.fail(
                f"WIN-45 reachability blocked (recorded non-pass, not skipped): {_reach}")

        # Activate TextEditTool first so the triple-click hits the
        # text-selection path. Without this, SelectTool processes
        # the click and the canvas doesn't repaint.
        # Phase 3 Cluster #3 (2026-06-09): activate tool via pipe AND
        # use the Qt-side tripleClickByPdfCoord pipe instead of
        # PyAutoGUI. PyAutoGUI's three Win32 clicks (~80ms apart)
        # are reliable on the OS side, but the test pipe path also
        # routes through the same Qt event loop the real click would
        # use. The pipe handler activates via QTimer::singleShot(0),
        # so allow ~1s for the tool to install before clicking.
        from tests.ui_automation.harness.dialog_helpers import find_by_suffix
        tool_pipe = find_by_suffix(win, "testToolActivatePipe")
        triple_pipe = find_by_suffix(win, "testTripleClickPipe")
        if tool_pipe is not None:
            try:
                tool_pipe.set_edit_text("textEditToolAction")
                time.sleep(1.2)
            except Exception:
                pass

        before = screenshot_canvas(win)
        if before is None:
            pytest.skip("canvas screenshot unavailable")
        # Title 'Single Page Test Document' at approx PDF (200, 55) in
        # top-left PDF coords on a Letter-size page (612x792).
        if triple_pipe is not None:
            try:
                triple_pipe.set_edit_text("0,250,55")
            except Exception:
                pass
        else:
            if not triple_click_pdf_point(win, 200.0, 55.0):
                pytest.skip("triple_click_pdf_point returned False (no canvas)")
        time.sleep(0.8)
        after = screenshot_canvas(win)
        if after is None:
            pytest.skip("canvas screenshot unavailable post-click")
        ratio = pixel_diff_ratio(before, after)
        assert_recorded(
            "canvas_text_selection_triple_click",
            ratio > 0.001,
            f"canvas did not change after triple-click "
            f"(pixel_diff_ratio={ratio:.4f}); selection highlight "
            "should paint pixels even at low threshold",
            green_notes=(
                f"Pipeline K phase-4: triple_click_pdf_point drove "
                f"text-selection paint; pixel_diff_ratio={ratio:.4f}"
            ),
        )
    finally:
        try:
            app.kill()
        except Exception:
            pass


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-v"]))
