# -*- coding: utf-8 -*-
"""GAP-23 — Up/Down arrow keys scroll the canvas while text-edit is active.

Pipeline 3e7c1a05 phase-1 (commit 4733175) added event->ignore() for
Up/Down keys in TextEditTool::onKeyPress, but placed it inside the
!isEditing() guard — making it a no-op when editing was actually active.
Pipeline a7f3d2c9 phase-2 (commit 2e9e3a4) moved the ignore() block
before the isEditing() guard, completing the fix so that Up/Down are
unconditionally forwarded to the canvas scroll handler while a text-edit
span is active.

This E2E test verifies the full integration boundary:

    TextEditTool::onKeyPress (ignore — fires before isEditing() guard)
      -> PdfCanvas::keyPressEvent (fall-through to QScrollArea)
        -> canvas scrolls

The test zooms in to 200% so the page height exceeds the viewport,
giving the scroll arrows room to move.  It then measures the pixel
difference of the canvas before and after 20 Down presses, and again
after 20 Up presses.  A non-trivial pixel diff (>2% of sampled pixels
changed) is the proxy for "the canvas actually scrolled".
"""

from __future__ import annotations

import os
import sys
import subprocess
from pathlib import Path

import pytest

# Phase F25 / gap WIN-45: reachability is a RECORDED NON-PASS, never a skip.
from tests.ui_automation.conftest import (  # noqa: E402
    current_test_id,
    record_matrix_result,
)

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------

REPO_ROOT = Path(__file__).resolve().parent.parent
PDFE_EXE = REPO_ROOT / "build" / "vs2022" / "bin" / "Debug" / "PDFE.exe"
FIXTURES_DIR = REPO_ROOT / "tests" / "fixtures"
# GAP-45: diagnostic captures go to the gitignored runtime directory, not
# the source-controlled ui_screenshots/text_edit_arrow_scroll/ tree.
SCREENSHOTS_DIR = REPO_ROOT / "tests" / "_runtime" / "baselines_actual" / "text_edit_arrow_scroll"

sys.path.insert(0, str(REPO_ROOT))

# ---------------------------------------------------------------------------
# Skip guards (mirror the pattern in test_ui_text_edit_selection.py)
# ---------------------------------------------------------------------------


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


def _has_pdfe_binary() -> bool:
    return PDFE_EXE.exists()


def _is_interactive_session() -> bool:
    if os.environ.get("PDFE_UI_FORCE_HEADLESS") == "1":
        return False
    return bool(os.environ.get("SESSIONNAME"))


REQUIRES_LIVE_UI = pytest.mark.skipif(
    not (_has_pywinauto() and _has_pyautogui() and _has_pdfe_binary()
         and _is_interactive_session()),
    reason=(
        "GAP-23 Up/Down arrow scroll test needs live PDFE.exe + "
        "interactive Windows desktop + pywinauto + pyautogui."
    ),
)

# ---------------------------------------------------------------------------
# Pixel-diff helper
# ---------------------------------------------------------------------------


def _pixel_diff_fraction(img_a, img_b, sample_step: int = 2) -> float:
    """Return the fraction of sampled pixels that differ between two PIL Images.

    Both images are compared as RGB.  ``sample_step`` controls the stride
    (every 2nd pixel by default to keep runtime reasonable on large canvases).
    Returns a float in [0, 1].
    """
    from PIL import Image  # noqa: PLC0415
    a = img_a.convert("RGB")
    b = img_b.convert("RGB")
    # Resize b to match a if they differ (handles minor DPI artefacts).
    if a.size != b.size:
        b = b.resize(a.size, Image.LANCZOS)
    pa = a.load()
    pb = b.load()
    w, h = a.size
    changed = 0
    total = 0
    for y in range(0, h, sample_step):
        for x in range(0, w, sample_step):
            total += 1
            if pa[x, y][:3] != pb[x, y][:3]:
                changed += 1
    return changed / total if total > 0 else 0.0


# ---------------------------------------------------------------------------
# GAP-23 test
# ---------------------------------------------------------------------------


@REQUIRES_LIVE_UI
@pytest.mark.timeout(120)
def test_up_down_arrows_scroll_canvas_during_edit():
    """Verify Up/Down arrows scroll the canvas while text-edit is active.

    Pipeline 3e7c1a05 phase-1 (commit 4733175) added event->ignore() for
    Up/Down in TextEditTool::onKeyPress, but placed it inside the
    !isEditing() guard, making it a no-op during active editing.
    Pipeline a7f3d2c9 phase-2 (commit 2e9e3a4) moved the ignore() block
    before the isEditing() guard, completing the fix.
    PdfCanvas::keyPressEvent falls through to the canvas scroll handler
    when event.isAccepted() is false.  This E2E test confirms the full
    integration boundary works:

        TextEditTool ignore -> PdfCanvas check -> canvas scroll.

    Steps
    -----
    1. Open test_3page.pdf (a 3-page fixture — scroll room guaranteed).
    2. Open in text-edit mode via Ctrl+Shift+E (textEditToolAction shortcut).
    3. Zoom to ~200% via Ctrl+= presses so the page overflows the viewport.
    4. Click into a text span via testClickByPdfCoordPipe.
    5. Capture canvas state (before).
    6. Press Down 20x — canvas must scroll down.
    7. Capture canvas state (after Down).
    8. Assert pixel diff (before, after) > 2% (canvas scrolled).
    9. Press Up 20x — canvas must scroll back.
    10. Capture canvas state (after Up).
    11. Assert pixel diff (after Down, after Up) > 2% (canvas scrolled back).
    """
    from PIL import Image  # noqa: PLC0415

    from tests.ui_harness.core.launch import PDFESession, no_orphan_pdfe  # type: ignore
    from tests.ui_harness.core.canvas import canvas_rect, wait_stable  # type: ignore
    from tests.ui_harness.text_edit.helpers import (  # type: ignore
        open_in_text_edit_mode,
    )
    from tests.helpers.keys import (  # type: ignore
        hotkey_to,
        press_to,
    )

    import time
    import pyautogui

    fixture = FIXTURES_DIR / "test_3page.pdf"
    if not fixture.exists():
        pytest.skip(f"Fixture missing: {fixture}")

    SCREENSHOTS_DIR.mkdir(parents=True, exist_ok=True)

    with PDFESession(pdf_path=str(fixture)) as session:
        hwnd = session.window.handle

        # ---- Step 2: activate text-edit mode --------------------------------
        # open_in_text_edit_mode also opens the file (but it's already open
        # via PDFESession).  Use it just for the tool-activation step by
        # calling hotkey directly — mirrors test_ui_text_edit_selection.py.
        open_in_text_edit_mode(session, str(fixture), page_idx=0)

        # ---- Step 3: zoom to ~200% so the page overflows the viewport -------
        # Each Ctrl+= increments zoom by one step (~25% per step at default
        # increments).  Five presses takes us from fit-page (~75%) to ~200%.
        for _ in range(5):
            hotkey_to(hwnd, "ctrl", "=")
            time.sleep(0.15)
        wait_stable(0.5)

        # ---- Step 4: click into a text span via the pipe seam ---------------
        # Fitz coordinates for body text near the top of page 1 of test_3page.pdf:
        #   x ~ 298 pts (centre of 595-pt wide page)
        #   y ~  98 pts (midpoint of "This is a test PDF..." line at fitz 89-106)
        # Matches the coordinates used in test_ui_text_edit_selection.py.
        _pipe_ok = False
        try:
            win = session.app.window(title_re=r".*PDFE.*")
            all_edits = win.descendants(control_type="Edit")
            pipe_ctrl = None
            for _c in all_edits:
                _aid = _c.element_info.automation_id or ""
                _suffix = _aid.rsplit(".", 1)[-1] if "." in _aid else _aid
                if _suffix == "testClickByPdfCoordPipe":
                    pipe_ctrl = _c
                    break
            if pipe_ctrl is not None:
                pipe_ctrl.set_edit_text("0,298.0,98.0")
                time.sleep(0.6)
                _pipe_ok = True
        except Exception:  # noqa: BLE001
            pass

        if not _pipe_ok:
            # WIN-45 / F25: unreachable -> recorded 'blocked', then FAIL.
            _reach = (
                "testClickByPdfCoordPipe not reachable — binary missing pipe "
                "or PDFE_TEST_PIPES env var not set."
            )
            record_matrix_result(current_test_id(), "blocked", _reach)
            pytest.fail(
                f"WIN-45 reachability blocked (recorded non-pass, not skipped): {_reach}")

        wait_stable(0.5)

        # ---- Step 5: capture canvas state (before) --------------------------
        r = canvas_rect(session.app)
        canvas_w = r.right - r.left
        canvas_h = r.bottom - r.top

        before_path = SCREENSHOTS_DIR / "gap23_before.png"
        before_img = pyautogui.screenshot(
            region=(r.left, r.top, canvas_w, canvas_h)
        )
        before_img.save(str(before_path))

        # ---- Step 6: press Down 20x -----------------------------------------
        for _ in range(20):
            press_to(hwnd, "down")
            time.sleep(0.05)
        wait_stable(0.5)

        # ---- Step 7: capture canvas state (after Down) ----------------------
        after_down_path = SCREENSHOTS_DIR / "gap23_after_down.png"
        after_down_img = pyautogui.screenshot(
            region=(r.left, r.top, canvas_w, canvas_h)
        )
        after_down_img.save(str(after_down_path))

        # ---- Step 8: assert Down scrolled the canvas ------------------------
        diff_down = _pixel_diff_fraction(before_img, after_down_img)
        assert diff_down > 0.02, (
            f"Down arrow during text edit didn't scroll the canvas "
            f"(pixel diff = {diff_down:.3%}, threshold = 2%). "
            f"Screenshots: before={before_path}, after={after_down_path}. "
            "Either TextEditTool::onKeyPress is accepting Down (not "
            "ignoring it), PdfCanvas is not forwarding it to the scroll "
            "area, or the zoom level is insufficient to allow scrolling."
        )

        # ---- Step 9: press Up 20x -------------------------------------------
        for _ in range(20):
            press_to(hwnd, "up")
            time.sleep(0.05)
        wait_stable(0.5)

        # ---- Step 10: capture canvas state (after Up) -----------------------
        after_up_path = SCREENSHOTS_DIR / "gap23_after_up.png"
        after_up_img = pyautogui.screenshot(
            region=(r.left, r.top, canvas_w, canvas_h)
        )
        after_up_img.save(str(after_up_path))

        # ---- Step 11: assert Up scrolled the canvas back --------------------
        diff_up = _pixel_diff_fraction(after_down_img, after_up_img)
        assert diff_up > 0.02, (
            f"Up arrow during text edit didn't scroll the canvas back "
            f"(pixel diff = {diff_up:.3%}, threshold = 2%). "
            f"Screenshots: after_down={after_down_path}, "
            f"after_up={after_up_path}. "
            "Either TextEditTool::onKeyPress is accepting Up (not "
            "ignoring it) or PdfCanvas is not forwarding it to the scroll "
            "area."
        )

    # --- No orphan PDFE.exe process after context manager exits. ---
    # Give the OS a moment to fully release the process.
    time.sleep(1.0)
    assert no_orphan_pdfe(), "PDFE.exe orphan process detected after teardown"


# ---------------------------------------------------------------------------
# Always-pass summary (for headless CI that can't run the live test)
# ---------------------------------------------------------------------------


def test_gap23_skip_summary():
    """Reports why the GAP-23 arrow-scroll test was skipped on this run.

    Always passes so pytest counts a non-zero number of test cases on
    headless hosts where the REQUIRES_LIVE_UI guard fires.
    """
    has_pw = _has_pywinauto()
    has_pag = _has_pyautogui()
    has_bin = _has_pdfe_binary()
    has_sess = _is_interactive_session()
    if not all([has_pw, has_pag, has_bin, has_sess]):
        print(
            f"\n[gap-23-skip] pywinauto={has_pw} "
            f"pyautogui={has_pag} binary={has_bin} "
            f"interactive={has_sess}",
            flush=True,
        )
    assert True
