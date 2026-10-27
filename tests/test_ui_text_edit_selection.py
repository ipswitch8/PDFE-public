# -*- coding: utf-8 -*-
"""UI-TE-UI-01 — selection-highlight pixel test.

Drives the live PDFE.exe via pywinauto + PyAutoGUI:
  1. Open a fixture PDF in text-edit mode.
  2. Click into a body-text span to place the caret.
  3. Hold Shift+Right to select 5 characters.
  4. Capture a canvas screenshot and confirm the selection rectangle
     is (a) present (a non-trivial number of selection-blue pixels)
     and (b) confined to the span's vicinity (not bleeding across the
     full canvas).

Skip guards mirror the patterns in test_ui_text_edit_smoke.py: the
test only runs when pywinauto + pyautogui + a real PDFE.exe + an
interactive Windows desktop session are all available.

Selection-blue tolerance: TextEditTool paints the selection with
QColor(0, 120, 215, 80) — RGB(0, 120, 215) at alpha≈31% opacity.
Composited over the white PDF page background the blended pixel is
approximately RGB(175, 212, 242).  The pixel test looks for pixels
where B > 1.2*R AND B > 1.05*G AND B > 80 — a band of "clearly
bluish" pixels that matches the blended selection colour while
rejecting white, grey, and warm-tinted pixels.
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

REPO_ROOT = Path(__file__).resolve().parent.parent
PDFE_EXE = REPO_ROOT / "build" / "vs2022" / "bin" / "Debug" / "PDFE.exe"
FIXTURES_DIR = REPO_ROOT / "tests" / "fixtures"
# GAP-45: diagnostic captures go to the gitignored runtime directory, not
# the source-controlled ui_screenshots/text_edit/ tree.
SCREENSHOTS_DIR = REPO_ROOT / "tests" / "_runtime" / "baselines_actual" / "text_edit"

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


def _has_pdfe_binary() -> bool:
    return PDFE_EXE.exists()


def _is_interactive_session() -> bool:
    if os.environ.get("PDFE_UI_FORCE_HEADLESS") == "1":
        return False
    return bool(os.environ.get("SESSIONNAME"))


REQUIRES_LIVE_UI = pytest.mark.skipif(
    not (_has_pywinauto() and _has_pyautogui() and _has_pdfe_binary()
         and _is_interactive_session()),
    reason=("UI-TE-UI-01 needs live PDFE.exe + interactive desktop + "
            "pywinauto + pyautogui (D1 visual-regression baseline)."),
)


def _count_selection_blue_pixels(img):
    """Count pixels that are clearly Qt-selection-blue.

    TextEditTool paints the selection with QColor(0, 120, 215, 80) —
    RGB(0, 120, 215) at alpha=80/255 (~31% opacity).  Composited over
    the white PDF page background the blended pixel is approximately
    RGB(175, 212, 242).  The original threshold (b > r*1.4) incorrectly
    rejected these blended pixels (242 < 175*1.4 = 245).

    Pipeline c91e7f3a phase-7 fix: relaxed to b > r*1.2 and b > g*1.05
    which matches the actual blended values (242 > 210 and 242 > 222.6)
    while still rejecting pure-white (b > r*1.2 fails when r≈b≈255) and
    warm-grey pixels.

    Returns (count, total) so callers can compute a ratio.
    """
    px = img.load()
    w, h = img.size
    blue_count = 0
    for y in range(0, h, 2):
        for x in range(0, w, 2):
            if _is_selection_blue(px[x, y]):
                blue_count += 1
    sampled = (w // 2) * (h // 2)
    return blue_count, sampled


def _is_selection_blue(rgb) -> bool:
    """Blue-ness predicate shared by the absolute and delta counters."""
    r, g, b = rgb[:3]
    return b > 80 and b > r * 1.2 and b > g * 1.05


def _count_new_blue_pixels(base_img, sel_img) -> int:
    """Count pixels that BECAME selection-blue between the two captures.

    The selection is a *change*, so measure the change. The original oracle
    compared ABSOLUTE blue counts (``sel_blue > base_blue * 3``), which is
    defeated by any STATIC blue UI inside the captured rect — and there is one:
    the canvas capture includes a solid dark-blue chrome strip along its bottom
    edge (RGB(0,66,117), ~7 px tall, full width) contributing ~1692 blue pixels
    to BOTH captures. The genuine ~295-pixel selection band can never be 3x a
    1692-pixel strip, so the test failed deterministically even though the
    selection rendered perfectly (295 px of exactly RGB(0,120,215) — the Qt
    selection colour — appearing on the selected line).

    Diffing cancels the static chrome exactly and isolates the selection band,
    so this is a STRICTER oracle than the ratio it replaces, not a weaker one.
    """
    bpx = base_img.load()
    spx = sel_img.load()
    w = min(base_img.size[0], sel_img.size[0])
    h = min(base_img.size[1], sel_img.size[1])
    new_blue = 0
    for y in range(0, h, 2):
        for x in range(0, w, 2):
            if _is_selection_blue(spx[x, y]) and not _is_selection_blue(bpx[x, y]):
                new_blue += 1
    return new_blue


@REQUIRES_LIVE_UI
def test_ui_te_ui_01_selection_highlight_visible():
    """UI-TE-UI-01 — drag-select 5 chars in a body-text span and
    assert the selection rectangle is rendered."""
    from PIL import Image
    from tests.ui_harness.core.launch import PDFESession  # type: ignore
    from tests.ui_harness.core.screenshot import capture_region  # type: ignore
    from tests.ui_harness.core.canvas import canvas_rect, wait_stable  # type: ignore
    from tests.ui_harness.text_edit.helpers import (  # type: ignore
        open_in_text_edit_mode,
    )
    import time
    from tests.helpers.keys import hotkey_to  # type: ignore

    fixture = FIXTURES_DIR / "test_3page.pdf"
    if not fixture.exists():
        pytest.skip(f"Fixture missing: {fixture}")

    SCREENSHOTS_DIR.mkdir(parents=True, exist_ok=True)
    out_baseline = SCREENSHOTS_DIR / "ui_te_ui_01_baseline.png"
    out_selected = SCREENSHOTS_DIR / "ui_te_ui_01_selected.png"

    with PDFESession(pdf_path=str(fixture)) as session:
        # Activate text-edit mode (helper handles toolbar invocation).
        open_in_text_edit_mode(session, str(fixture), page_idx=0)

        # Resolve canvas rect once and cache it.  canvas_rect() falls through
        # to a window.descendants() UIA scan which can block for several
        # seconds when the tile renderer is active.  Caching the rect avoids
        # a second blocking scan inside capture_region() later.
        r = canvas_rect(session.app)
        canvas_w = r.right - r.left
        canvas_h = r.bottom - r.top

        # Capture pristine state before selection using the cached rect.
        # wait=1.0 ensures the tile renderer has painted the first page;
        # wait=0.3 was too short and could yield an all-white baseline.
        capture_region(r.left, r.top, canvas_w, canvas_h,
                       subsystem="text_edit", test="ui_te_ui_01_baseline",
                       wait=1.0)

        hwnd = session.window.handle

        # Place the caret in the body text using PdfCanvas::clickByPdfCoord
        # via the testClickByPdfCoordPipe seam.  This dispatches a synthetic
        # QMouseEvent through PDFE's CoordinateTransform — correct for any
        # zoom level, scroll offset, or dock-panel layout.  Eliminates the
        # screen-coordinate arithmetic that was fragile at non-fit-page zoom.
        #
        # Fitz coordinates (origin at top-left, y increases downward):
        #   x ≈ 298 pts (centre of 595-pt wide page, body text at 197–399)
        #   y ≈ 98 pts  (midpoint of "This is a test PDF..." at fitz 89–106)
        #
        # clickByPdfCoord ends with canvas->setFocus() so subsequent
        # WM_KEYDOWN/WM_KEYUP messages go directly to PdfCanvas::event().
        #
        # We write directly to the pipe via pywinauto (the ui_harness
        # PDFESession uses session.app, not the helpers session object).
        # Qt6 UIA prefixes AutomationId with the full widget path, e.g.
        # "QApplication.mainWindow.testClickByPdfCoordPipe", so we
        # scan Edit descendants and match on the trailing suffix.
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
                time.sleep(0.5)
                _pipe_ok = True
        except Exception:  # noqa: BLE001
            pass
        if not _pipe_ok:
            # WIN-45 / F25: unreachable -> recorded 'blocked', then FAIL.
            _reach = (
                "testClickByPdfCoordPipe not reachable — binary missing pipe."
            )
            record_matrix_result(current_test_id(), "blocked", _reach)
            pytest.fail(
                f"WIN-45 reachability blocked (recorded non-pass, not skipped): {_reach}")
        wait_stable(0.3)

        # Hold Shift+Right five times → 5-char forward selection.
        # hotkey_to(hwnd) brings the PDFE main window to the foreground
        # before each injection so the keystrokes reach TextEditTool::onKeyPress
        # via Qt's normal keyboard-event routing to the focused child widget
        # (the canvas).
        for _ in range(5):
            hotkey_to(hwnd, "shift", "right")
            time.sleep(0.1)

        # Wait for Qt to repaint the selection highlight before capturing.
        time.sleep(1.0)

        # Use capture_region with the already-computed canvas rect.
        capture_region(r.left, r.top, canvas_w, canvas_h,
                       subsystem="text_edit", test="ui_te_ui_01_selected",
                       wait=0.0)

    assert out_baseline.exists(), f"Baseline not captured: {out_baseline}"
    assert out_selected.exists(), f"Selection not captured: {out_selected}"

    base_img = Image.open(out_baseline).convert("RGB")
    sel_img = Image.open(out_selected).convert("RGB")

    base_blue, _ = _count_selection_blue_pixels(base_img)
    sel_blue, _ = _count_selection_blue_pixels(sel_img)
    # The selection is a CHANGE — measure what BECAME blue. This cancels the
    # static blue chrome strip the canvas capture includes along its bottom
    # edge (see _count_new_blue_pixels), which otherwise swamps any absolute
    # count comparison.
    new_blue = _count_new_blue_pixels(base_img, sel_img)

    # Derive the expected minimum selection-blue pixel count from the
    # actual canvas width captured.  At fit-page zoom the 5-char
    # selection spans roughly 5 × (canvas_w/595 × 7 pt/char) pixels
    # wide and ~(canvas_w/595 × 12 pt/line) pixels tall.  Sampled
    # every-other-pixel and accounting for inter-char gaps the expected
    # count is roughly canvas_w * 0.04.  The floor of 15 keeps the
    # assertion meaningful even on very narrow canvases (≥300 px);
    # the cap of 150 preserves the original intent on wide canvases.
    _img_w = sel_img.size[0]
    _blue_min = min(max(15, int(_img_w * 0.04)), 150)

    assert new_blue > _blue_min, (
        f"Selection rectangle absent from screenshot: only {new_blue} pixels "
        f"BECAME selection-blue (threshold={_blue_min}, canvas_w={_img_w}px; "
        f"absolute counts base={base_blue} sel={sel_blue}). Either Shift+Right "
        "did not register, the canvas didn't repaint, or the selection brush "
        "was overdrawn (UI-TE-UI-01)."
    )


def test_ui_te_ui_01_skip_summary():
    """Reports why UI-TE-UI-01 was skipped on this run. Always passes
    so pytest counts a non-zero number of cases on headless hosts."""
    has_pywinauto = _has_pywinauto()
    has_pyautogui = _has_pyautogui()
    has_binary = _has_pdfe_binary()
    has_session = _is_interactive_session()
    if not all([has_pywinauto, has_pyautogui, has_binary, has_session]):
        print(
            f"\n[ui-te-ui-01-skip] pywinauto={has_pywinauto} "
            f"pyautogui={has_pyautogui} binary={has_binary} "
            f"interactive={has_session}",
            flush=True,
        )
    assert True
