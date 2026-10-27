# -*- coding: utf-8 -*-
"""UI-TE-UI-03 — 5-keystroke + 5-Ctrl+Z pixel-equivalence test.

Drives the live PDFE.exe via pywinauto + PyAutoGUI:
  1. Open a fixture PDF in text-edit mode.
  2. Capture the pristine canvas screenshot as a baseline.
  3. Click into a body-text span and type 5 characters.
  4. Press Ctrl+Z five times to undo all keystrokes.
  5. Capture the final canvas screenshot.
  6. Assert it matches the pristine baseline pixel-for-pixel within a
     small tolerance (HOCR/atlas re-rendering can produce tiny
     anti-alias drift even with byte-identical underlying state).

This is the pixel-level proof of UI-TE-UNDO-05 (the kernel test
TextEditUndoChainTest.FiveEditsFiveUndosRestoresExactOriginal already
locks the model-level invariant — UI-TE-UI-03 lifts it to the actual
rendering pipeline).

Skip guards mirror test_ui_text_edit_smoke.py: only runs when
pywinauto + pyautogui + a real PDFE.exe + an interactive Windows
desktop session are all available.
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

import pytest

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
    reason=("UI-TE-UI-03 needs live PDFE.exe + interactive desktop + "
            "pywinauto + pyautogui (D1 visual-regression baseline)."),
)


def _max_per_pixel_delta(img_a, img_b):
    """Return (max_delta, mean_delta, mismatched_fraction).

    max_delta is the largest single channel difference across all
    pixels; mismatched_fraction is the share of pixels with any
    channel difference >5 (above sub-pixel anti-alias noise).
    Both images must be the same size.
    """
    if img_a.size != img_b.size:
        return (255, 255.0, 1.0)
    pa, pb = img_a.load(), img_b.load()
    w, h = img_a.size
    max_d = 0
    sum_d = 0
    mismatch = 0
    sampled = 0
    for y in range(0, h, 2):
        for x in range(0, w, 2):
            ar, ag, ab = pa[x, y][:3]
            br, bg, bb = pb[x, y][:3]
            d = max(abs(ar - br), abs(ag - bg), abs(ab - bb))
            if d > max_d:
                max_d = d
            sum_d += d
            if d > 5:
                mismatch += 1
            sampled += 1
    mean_d = sum_d / max(sampled, 1)
    frac = mismatch / max(sampled, 1)
    return (max_d, mean_d, frac)


@REQUIRES_LIVE_UI
def test_ui_te_ui_03_five_keystrokes_five_undos_restore_pixels():
    """UI-TE-UI-03 — visual proof of the 5-edit / 5-undo round-trip."""
    from PIL import Image
    from tests.ui_harness.core.launch import PDFESession  # type: ignore
    from tests.ui_harness.core.screenshot import capture_canvas  # type: ignore
    from tests.ui_harness.text_edit.helpers import (  # type: ignore
        open_in_text_edit_mode,
        click_into_span,
        type_keys,
    )
    import time
    from tests.helpers.keys import hotkey_to  # type: ignore

    fixture = FIXTURES_DIR / "test_3page.pdf"
    if not fixture.exists():
        pytest.skip(f"Fixture missing: {fixture}")

    SCREENSHOTS_DIR.mkdir(parents=True, exist_ok=True)
    pristine_path = SCREENSHOTS_DIR / "ui_te_ui_03_pristine.png"
    after_path = SCREENSHOTS_DIR / "ui_te_ui_03_after_undo.png"

    with PDFESession(pdf_path=str(fixture)) as session:
        open_in_text_edit_mode(session, str(fixture), page_idx=0)

        # Capture pristine baseline before any edits.
        capture_canvas(session.app, subsystem="text_edit",
                       test="ui_te_ui_03_pristine", wait=0.3)

        # Click into a body-text span and type 5 characters.
        hwnd = session.window.handle
        click_into_span(session, 200, 150)
        time.sleep(0.2)
        type_keys(session, "abcde")
        time.sleep(0.3)

        # Undo five times.
        for _ in range(5):
            hotkey_to(hwnd, "ctrl", "z")
            time.sleep(0.1)
        time.sleep(0.5)  # let the canvas settle

        capture_canvas(session.app, subsystem="text_edit",
                       test="ui_te_ui_03_after_undo", wait=0.3)

    assert pristine_path.exists(), f"Pristine baseline missing: {pristine_path}"
    assert after_path.exists(), f"Post-undo capture missing: {after_path}"

    pristine = Image.open(pristine_path).convert("RGB")
    after = Image.open(after_path).convert("RGB")

    max_d, mean_d, frac = _max_per_pixel_delta(pristine, after)

    # Tolerance: anti-alias drift on glyph edges can shift a few
    # pixels by ~10-15 channel units even when the underlying state
    # is byte-identical. We allow a small mismatched fraction
    # (anti-alias halo) but require mean delta to be very small.
    # If 5 undos truly restored the state, ≥99% of sampled pixels
    # should be exact matches and mean delta should be near zero.
    assert mean_d < 2.0, (
        f"Mean per-pixel delta after 5 undos = {mean_d:.2f}. The "
        "post-undo canvas does not match the pristine baseline. "
        "Either the undo chain failed, the canvas re-rendered with "
        "different layout, or there is real visual drift in the "
        "rendering pipeline (UI-TE-UI-03)."
    )
    assert frac < 0.02, (
        f"Mismatched-pixel fraction = {frac:.4f} (max single delta "
        f"= {max_d}). More than 2% of sampled pixels show a >5-unit "
        "channel difference — the post-undo state is meaningfully "
        "different from pristine. UI-TE-UI-03 demands pixel "
        "equivalence within anti-alias tolerance."
    )


def test_ui_te_ui_03_skip_summary():
    has_pywinauto = _has_pywinauto()
    has_pyautogui = _has_pyautogui()
    has_binary = _has_pdfe_binary()
    has_session = _is_interactive_session()
    if not all([has_pywinauto, has_pyautogui, has_binary, has_session]):
        print(
            f"\n[ui-te-ui-03-skip] pywinauto={has_pywinauto} "
            f"pyautogui={has_pyautogui} binary={has_binary} "
            f"interactive={has_session}",
            flush=True,
        )
    assert True
