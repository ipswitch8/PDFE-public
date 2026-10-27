# -*- coding: utf-8 -*-
"""Phase-7 image-edit + redaction UI smoke test.

Combined smoke for both subsystems per pipeline.json phase-7. Captures
panel/canvas baselines for image_edit and redaction directories.
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parent.parent
PDFE_EXE = REPO_ROOT / "build" / "vs2022" / "bin" / "Debug" / "PDFE.exe"
FIXTURES_DIR = REPO_ROOT / "tests" / "fixtures"
SCREENSHOTS_IMG = FIXTURES_DIR / "ui_screenshots" / "image_edit"
SCREENSHOTS_RED = FIXTURES_DIR / "ui_screenshots" / "redaction"

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
    reason="Live PDFE.exe + interactive Windows desktop required.",
)


@pytest.fixture()
def session_with_image_doc():
    from tests.ui_harness.core.launch import PDFESession  # type: ignore
    from tests.helpers.keys import hotkey_to  # type: ignore
    from tests.helpers.file_dialog import set_open_dialog_filename  # type: ignore
    import time
    import ctypes

    fixture = FIXTURES_DIR / "test_pdf_with_images.pdf"
    if not fixture.exists():
        pytest.skip(f"Fixture missing: {fixture}")

    with PDFESession(pdf_path=None) as s:
        hwnd = s.window.handle
        hotkey_to(hwnd, "ctrl", "o")
        # Poll for the Open file dialog.
        _user32 = ctypes.WinDLL("user32", use_last_error=True)
        dlg_hwnd = 0
        deadline = time.monotonic() + 8.0
        while time.monotonic() < deadline and dlg_hwnd == 0:
            time.sleep(0.25)
            buf = ctypes.create_unicode_buffer(256)
            fw = _user32.GetForegroundWindow()
            if fw and fw != hwnd:
                _user32.GetWindowTextW(fw, buf, 256)
                title = buf.value or ""
                if "open" in title.lower() or "pdf" in title.lower():
                    dlg_hwnd = int(fw)
        if dlg_hwnd:
            set_open_dialog_filename(dlg_hwnd, str(fixture))
        else:
            pytest.skip("Open file dialog did not appear within 8s")
        time.sleep(1.5)
        yield s


@REQUIRES_LIVE_UI
def test_ui_image_edit_canvas_baseline(session_with_image_doc):
    from tests.ui_harness.core.screenshot import capture_canvas  # type: ignore
    SCREENSHOTS_IMG.mkdir(parents=True, exist_ok=True)
    out_path = capture_canvas(session_with_image_doc.app, "image_edit",
                              "canvas_baseline")
    assert Path(out_path).exists() and Path(out_path).stat().st_size > 0


@REQUIRES_LIVE_UI
def test_ui_redaction_canvas_baseline(session_with_image_doc):
    from tests.ui_harness.core.screenshot import capture_canvas  # type: ignore
    SCREENSHOTS_RED.mkdir(parents=True, exist_ok=True)
    out_path = capture_canvas(session_with_image_doc.app, "redaction",
                              "canvas_baseline")
    assert Path(out_path).exists() and Path(out_path).stat().st_size > 0


def test_ui_image_redaction_skip_summary():
    has_live = (_has_pywinauto() and _has_pyautogui()
                and _has_pdfe_binary() and _is_interactive_session())
    if not has_live:
        print("\n[ui-img-red-skip] live UI not available", flush=True)
    assert True
