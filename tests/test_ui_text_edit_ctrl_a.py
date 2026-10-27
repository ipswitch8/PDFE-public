# -*- coding: utf-8 -*-
"""UI-TE-CTRLA-01 — Ctrl+A while editing selects the span, not a page-copy.

Pipeline a3f8e21d phase 2 (bug 5). Drives the live PDFE.exe via pywinauto +
PyAutoGUI and asserts, with a deterministic non-pixel oracle, that pressing
Ctrl+A while a text-edit session is active does NOT fall through to the global
"Select All → copy page text to clipboard" action.

Oracle (structural, not a pixel baseline — see the UI-E2E flake lessons):
  1. Open a native-text fixture in text-edit mode.
  2. Click into a body-text span (via the testClickByPdfCoordPipe seam) to
     place the caret — an active edit session.
  3. Put a known SENTINEL string on the Windows clipboard.
  4. Press Ctrl+A.
  5. Read the clipboard back. The fix routes Ctrl+A to
     TextEditTool::selectAll(), which never touches the clipboard, so the
     SENTINEL must survive. The pre-fix bug ran MainWindow::onSelectAll (the
     global QAction shadowing the tool), which copies the whole page's text to
     the clipboard — overwriting the SENTINEL.

Clipboard I/O uses PowerShell Set-Clipboard/Get-Clipboard via subprocess so the
test carries no extra Python dependency beyond the existing pywinauto/pyautogui.

Skip guards mirror test_ui_text_edit_selection.py: runs only when pywinauto +
pyautogui + a real PDFE.exe + an interactive Windows desktop session are all
available (environment guards, not a way to dodge a failing assertion).
"""

from __future__ import annotations

import os
import sys
import subprocess
import time
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
    reason=("UI-TE-CTRLA-01 needs live PDFE.exe + interactive desktop + "
            "pywinauto + pyautogui."),
)


def _set_clipboard(text: str) -> None:
    subprocess.run(
        ["powershell", "-NoProfile", "-Command", "Set-Clipboard", "-Value",
         text],
        check=True, capture_output=True, timeout=15,
    )


def _get_clipboard() -> str:
    out = subprocess.run(
        ["powershell", "-NoProfile", "-Command", "Get-Clipboard", "-Raw"],
        check=True, capture_output=True, text=True, timeout=15,
    )
    # Get-Clipboard -Raw preserves the value but appends a trailing newline.
    return (out.stdout or "").rstrip("\r\n")


@REQUIRES_LIVE_UI
def test_ui_te_ctrla_01_ctrl_a_while_editing_selects_span_not_page_copy():
    from tests.ui_harness.core.launch import PDFESession  # type: ignore
    from tests.ui_harness.core.canvas import wait_stable  # type: ignore
    from tests.ui_harness.text_edit.helpers import (  # type: ignore
        open_in_text_edit_mode,
    )
    from tests.helpers.keys import hotkey_to  # type: ignore

    fixture = FIXTURES_DIR / "test_3page.pdf"
    if not fixture.exists():
        pytest.skip(f"Fixture missing: {fixture}")

    sentinel = "PDFE-CTRLA-SENTINEL-9d05e2a4"

    with PDFESession(pdf_path=str(fixture)) as session:
        open_in_text_edit_mode(session, str(fixture), page_idx=0)

        # Place the caret in the body text via the testClickByPdfCoordPipe seam
        # (fitz coords; same span the selection test uses). This dispatches a
        # synthetic QMouseEvent through PDFE's CoordinateTransform and ends with
        # canvas->setFocus(), so the subsequent Ctrl+A reaches the app.
        pipe_ok = False
        try:
            win = session.app.window(title_re=r".*PDFE.*")
            for c in win.descendants(control_type="Edit"):
                aid = c.element_info.automation_id or ""
                suffix = aid.rsplit(".", 1)[-1] if "." in aid else aid
                if suffix == "testClickByPdfCoordPipe":
                    c.set_edit_text("0,298.0,98.0")
                    time.sleep(0.5)
                    pipe_ok = True
                    break
        except Exception:  # noqa: BLE001
            pass
        if not pipe_ok:
            # WIN-45 / F25: unreachable -> recorded 'blocked', then FAIL.
            _reach = (
                "testClickByPdfCoordPipe not reachable — binary missing pipe."
            )
            record_matrix_result(current_test_id(), "blocked", _reach)
            pytest.fail(
                f"WIN-45 reachability blocked (recorded non-pass, not skipped): {_reach}")
        wait_stable(0.3)

        # Seed the clipboard with a sentinel the page-copy path would clobber.
        _set_clipboard(sentinel)
        assert _get_clipboard() == sentinel, "clipboard seed failed"

        # The keystroke under test.
        hotkey_to(session.window.handle, "ctrl", "a")
        time.sleep(0.6)

        after = _get_clipboard()

    assert after == sentinel, (
        "Ctrl+A while editing overwrote the clipboard with page text "
        f"(got {after!r}, expected the sentinel {sentinel!r}). The global "
        "selectAllAction_ shadowed the text-edit tool instead of routing "
        "Ctrl+A to TextEditTool::selectAll() (UI-TE-CTRLA-01 / bug 5)."
    )


def test_ui_te_ctrla_01_skip_summary():
    """Reports why UI-TE-CTRLA-01 was skipped on this run. Always passes so
    pytest counts a non-zero number of cases on headless hosts."""
    has_pywinauto = _has_pywinauto()
    has_pyautogui = _has_pyautogui()
    has_binary = _has_pdfe_binary()
    has_session = _is_interactive_session()
    if not all([has_pywinauto, has_pyautogui, has_binary, has_session]):
        print(
            f"\n[ui-te-ctrla-01-skip] pywinauto={has_pywinauto} "
            f"pyautogui={has_pyautogui} binary={has_binary} "
            f"interactive={has_session}",
            flush=True,
        )
    assert True
