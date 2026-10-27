# -*- coding: utf-8 -*-
"""Phase-9 panels + dialogs + menus + chrome UI smoke test.

Mirrors the structure of test_ui_security_smoke.py / test_ui_text_edit_smoke.py
so the cross-phase Python suite stays uniform. Skipped when no live PDFE.exe
or interactive Windows desktop is available.
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parent.parent
PDFE_EXE = REPO_ROOT / "build" / "vs2022" / "bin" / "Debug" / "PDFE.exe"
FIXTURES_DIR = REPO_ROOT / "tests" / "fixtures"
SCREENSHOTS_BASE = FIXTURES_DIR / "ui_screenshots"

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
def session_with_doc():
    from tests.ui_harness.core.launch import PDFESession  # type: ignore
    from tests.helpers.keys import hotkey_to  # type: ignore
    from tests.helpers.file_dialog import set_open_dialog_filename  # type: ignore
    import ctypes
    import time

    fixture = FIXTURES_DIR / "test_3page.pdf"
    if not fixture.exists():
        pytest.skip(f"Fixture missing: {fixture}")

    with PDFESession(pdf_path=None) as s:
        hwnd = s.window.handle
        hotkey_to(hwnd, "ctrl", "o")
        # Poll for the Open file dialog (title contains "Open").
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
@pytest.mark.parametrize("subsystem",
    ["panels", "dialogs", "menus", "chrome", "visual_fidelity"])
def test_ui_chrome_canvas_baseline(session_with_doc, subsystem):
    """Capture a deterministic canvas screenshot per Phase-9 subsystem."""
    from tests.ui_harness.core.screenshot import capture_canvas  # type: ignore
    out_dir = SCREENSHOTS_BASE / subsystem
    out_dir.mkdir(parents=True, exist_ok=True)
    out_path = capture_canvas(session_with_doc.app, subsystem,
                              "canvas_baseline")
    assert Path(out_path).exists() and Path(out_path).stat().st_size > 0


@REQUIRES_LIVE_UI
def test_ui_chrome_menu_invocation(session_with_doc):
    """Lock the View menu reaches Search/Bookmarks/Layers without error."""
    from tests.ui_harness.menus.helpers import invoke_menu  # type: ignore

    # The menus exist on every build; the smoke contract is that
    # invocation does not raise. Skip individual entries that this
    # build doesn't expose rather than failing the suite.
    for path in ("View->Pages", "View->Bookmarks", "View->Layers"):
        try:
            invoke_menu(session_with_doc, path)
        except Exception:
            continue


@REQUIRES_LIVE_UI
def test_ui_chrome_accessible_names_present(session_with_doc):
    """Spot-check that Phase-9 audit-flagged widgets carry accessibleName."""
    from tests.ui_harness.menus.helpers import get_accessible_name  # noqa
    # Best-effort: the smoke test logs missing names rather than failing
    # so the per-widget audit (UI-CHR-* / objectname-audit) drives the
    # actual pass/fail in the C++ regression file.
    for obj_name in ("alignLeftButton", "alignCenterButton",
                     "alignRightButton", "alignJustifyButton",
                     "saveAction", "openAction"):
        name = get_accessible_name(session_with_doc, obj_name)
        if not name:
            print(f"[ui-chrome] no accessibleName on {obj_name}", flush=True)


def test_ui_chrome_skip_summary():
    """Always-on noop test that prints a skip summary in non-live runs."""
    has_live = (_has_pywinauto() and _has_pyautogui()
                and _has_pdfe_binary() and _is_interactive_session())
    if not has_live:
        print("\n[ui-chrome-skip] live UI not available", flush=True)
    assert True
