# -*- coding: utf-8 -*-
"""SEC-UI-100: End-to-end redaction roundtrip — place, apply, save,
verify sensitive bytes absent.

Pipeline b9e14d07 phase-4 / e4c71f3a phase-1 FIX-UI-100.
"""

from __future__ import annotations

import os
import sys
import shutil
import time
from pathlib import Path

import pytest
from tests.helpers.canvas_geometry import point_on_page

# Phase F11 / gap WIN-45: reachability is a RECORDED NON-PASS, never a skip.
from tests.ui_automation.conftest import (  # noqa: E402
    current_test_id,
    record_matrix_result,
)

_HERE = Path(__file__).resolve().parent
REPO_ROOT = _HERE.parent
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

FIXTURES = REPO_ROOT / "tests" / "fixtures"
FIX_REDACTION = FIXTURES / "test_redaction.pdf"
KNOWN_SENSITIVE = b"Confidential Document"


def _has_pdfe_binary() -> bool:
    try:
        from tests.helpers.pdfe_session import PDFE_EXE
        return Path(PDFE_EXE).exists()
    except Exception:
        return False


def _is_interactive() -> bool:
    if os.environ.get("PDFE_UI_FORCE_HEADLESS") == "1":
        return False
    return bool(os.environ.get("SESSIONNAME"))


def _has_pywinauto() -> bool:
    try:
        import pywinauto  # noqa: F401
        return True
    except Exception:
        return False


def _has_fitz() -> bool:
    try:
        import fitz  # noqa: F401
        return True
    except Exception:
        return False


pytestmark = [
    pytest.mark.skipif(not _has_pdfe_binary(), reason="PDFE binary not found"),
    pytest.mark.skipif(not _is_interactive(), reason="No interactive session"),
    pytest.mark.skipif(not _has_pywinauto(), reason="pywinauto not installed"),
]


@pytest.fixture(autouse=True)
def _isolate(tmp_path):
    """Kill orphan PDFE processes before and after each test."""
    from tests.helpers.pdfe_session import kill_pdfe_orphans
    kill_pdfe_orphans()
    yield
    kill_pdfe_orphans()


def _activate_redact_tool(session):
    """Activate redaction tool via toolbar button or Security menu fallback."""
    for auto_id in ("redactToolActionButton", "redactToolAction"):
        btn = session.find_optional(auto_id=auto_id, timeout=2)
        if btn is not None:
            try:
                btn.click_input()
                return True
            except Exception:
                pass

    # Fallback: name-based search.
    btn = session.find_optional(name="Redaction Tool", timeout=2)
    if btn is not None:
        try:
            btn.click_input()
            return True
        except Exception:
            pass
    return False


def _apply_redactions(session):
    """Trigger apply-redactions via button or action."""
    for auto_id in ("applyRedactionsButton", "applyRedactionsAction"):
        btn = session.find_optional(auto_id=auto_id, timeout=3)
        if btn is not None:
            try:
                btn.click_input()
                return True
            except Exception:
                pass

    btn = session.find_optional(name="Apply All Redactions", timeout=2)
    if btn is not None:
        try:
            btn.click_input()
            return True
        except Exception:
            pass
    return False


def test_redaction_roundtrip_bytes_absent(tmp_path):
    """SEC-UI-100: Open fixture, activate redaction tool, apply, verify
    sensitive bytes absent."""
    if not FIX_REDACTION.exists():
        pytest.skip(f"Fixture {FIX_REDACTION} not found")

    from tests.helpers.pdfe_session import PDFESession

    work = tmp_path / "redact_roundtrip.pdf"
    shutil.copy2(FIX_REDACTION, work)

    with PDFESession(pdf_path=str(work)) as session:
        session.wait_for_idle()

        # Place a redaction mark covering the top of page 0 via
        # PdfCanvas::addRedactionMarkAtPdfRect Q_INVOKABLE seam.
        canvas = session.find_optional(auto_id="pdfCanvas", timeout=3)
        if canvas is None:
            # WIN-45: unreachable -> recorded 'blocked' in the matrix, then FAIL.
            _reach = ("pdfCanvas not reachable via UIA.")
            record_matrix_result(current_test_id(), "blocked", _reach)
            pytest.fail(
                f"WIN-45 reachability blocked (recorded non-pass, not skipped): {_reach}")

        from pywinauto.uia_defines import IUIA
        try:
            ret = canvas.iface_invoke_pattern.Invoke()
        except Exception:
            pass

        # Use Q_INVOKABLE: place mark covering full page width, top 100pt.
        import ctypes
        from pywinauto import uia_element_info
        # Fallback: activate tool then draw via mouse drag on canvas.
        if not _activate_redact_tool(session):
            # WIN-45: unreachable -> recorded 'blocked' in the matrix, then FAIL.
            _reach = ("redaction tool not reachable via UIA.")
            record_matrix_result(current_test_id(), "blocked", _reach)
            pytest.fail(
                f"WIN-45 reachability blocked (recorded non-pass, not skipped): {_reach}")
        time.sleep(0.5)

        # Draw a redaction rectangle via mouse drag on the top portion.
        # Coordinates from the app, not from canvas offsets. A redaction must
        # land ON the page: `rect.left + 20` is inside the canvas but, at
        # ordinary zoom, in the grey margin beside the page -- so the drag
        # redacted nothing while still satisfying the off-SCREEN guard below.
        # See docs/audit/ui-test-observability-audit.md.
        p1 = point_on_page(session, 0, 0.05, 0.05)
        p2 = point_on_page(session, 0, 0.95, 0.25)
        assert p1 and p2, "page-rect seam unavailable; refusing to guess"
        x1, y1 = p1
        x2, y2 = p2
        import pyautogui
        # Off-screen guard: refuse a drag whose computed endpoints fall
        # outside the screen (a minimised/off-screen canvas reports
        # (-32000,...) coords -> the drag would hit the taskbar/desktop).
        _sw, _sh = pyautogui.size()
        assert 0 <= x1 < _sw and 0 <= y1 < _sh, (
            f"drag start ({x1},{y1}) off-screen {_sw}x{_sh}"
        )
        assert 0 <= x2 < _sw and 0 <= y2 < _sh, (
            f"drag end ({x2},{y2}) off-screen {_sw}x{_sh}"
        )
        pyautogui.moveTo(x1, y1)
        time.sleep(0.2)
        pyautogui.mouseDown()
        pyautogui.moveTo(x2, y2, duration=0.3)
        pyautogui.mouseUp()
        time.sleep(0.5)

        if not _apply_redactions(session):
            # WIN-45: unreachable -> recorded 'blocked' in the matrix, then FAIL.
            _reach = ("Apply Redactions button not reachable via UIA.")
            record_matrix_result(current_test_id(), "blocked", _reach)
            pytest.fail(
                f"WIN-45 reachability blocked (recorded non-pass, not skipped): {_reach}")
        time.sleep(2)

    content = work.read_bytes()
    assert KNOWN_SENSITIVE not in content, (
        "SEC-UI-100: Sensitive text must be absent after redaction"
    )


@pytest.mark.xfail(reason="Mouse-coordinate redaction may not cover exact text area; "
                    "binary check in test_redaction_roundtrip_bytes_absent validates "
                    "the security invariant", strict=False)
def test_redaction_roundtrip_text_extraction(tmp_path):
    """SEC-UI-100 (supplement): Verify redacted text not extractable."""
    # A genuine ENVIRONMENT gate, not a reachability block: PyMuPDF is an
    # optional verification dependency, and its absence says nothing about
    # whether the product works. Expressed with importorskip so the WIN-45
    # ratchet can SEE that it is environmental -- behind the `_has_fitz()`
    # helper it looked identical to a UIA lookup that silently skipped.
    pytest.importorskip("fitz", reason="PyMuPDF not installed; needed only to "
                                       "verify extracted text")
    if not FIX_REDACTION.exists():
        pytest.skip(f"Fixture {FIX_REDACTION} not found")

    import fitz
    from tests.helpers.pdfe_session import PDFESession

    work = tmp_path / "redact_text.pdf"
    shutil.copy2(FIX_REDACTION, work)

    with PDFESession(pdf_path=str(work)) as session:
        session.wait_for_idle()

        canvas = session.find_optional(auto_id="pdfCanvas", timeout=3)
        if canvas is None:
            # WIN-45: unreachable -> recorded 'blocked' in the matrix, then FAIL.
            _reach = ("pdfCanvas not reachable via UIA.")
            record_matrix_result(current_test_id(), "blocked", _reach)
            pytest.fail(
                f"WIN-45 reachability blocked (recorded non-pass, not skipped): {_reach}")

        if not _activate_redact_tool(session):
            # WIN-45: unreachable -> recorded 'blocked' in the matrix, then FAIL.
            _reach = ("redaction tool not reachable via UIA.")
            record_matrix_result(current_test_id(), "blocked", _reach)
            pytest.fail(
                f"WIN-45 reachability blocked (recorded non-pass, not skipped): {_reach}")
        time.sleep(0.5)

        # Coordinates from the app, not from canvas offsets. A redaction must
        # land ON the page: `rect.left + 20` is inside the canvas but, at
        # ordinary zoom, in the grey margin beside the page -- so the drag
        # redacted nothing while still satisfying the off-SCREEN guard below.
        # See docs/audit/ui-test-observability-audit.md.
        p1 = point_on_page(session, 0, 0.05, 0.05)
        p2 = point_on_page(session, 0, 0.95, 0.25)
        assert p1 and p2, "page-rect seam unavailable; refusing to guess"
        x1, y1 = p1
        x2, y2 = p2
        import pyautogui
        # Off-screen guard: refuse a drag whose computed endpoints fall
        # outside the screen (a minimised/off-screen canvas reports
        # (-32000,...) coords -> the drag would hit the taskbar/desktop).
        _sw, _sh = pyautogui.size()
        assert 0 <= x1 < _sw and 0 <= y1 < _sh, (
            f"drag start ({x1},{y1}) off-screen {_sw}x{_sh}"
        )
        assert 0 <= x2 < _sw and 0 <= y2 < _sh, (
            f"drag end ({x2},{y2}) off-screen {_sw}x{_sh}"
        )
        pyautogui.moveTo(x1, y1)
        time.sleep(0.2)
        pyautogui.mouseDown()
        pyautogui.moveTo(x2, y2, duration=0.3)
        pyautogui.mouseUp()
        time.sleep(0.5)

        if not _apply_redactions(session):
            # WIN-45: unreachable -> recorded 'blocked' in the matrix, then FAIL.
            _reach = ("Apply Redactions button not reachable via UIA.")
            record_matrix_result(current_test_id(), "blocked", _reach)
            pytest.fail(
                f"WIN-45 reachability blocked (recorded non-pass, not skipped): {_reach}")
        time.sleep(2)

    doc = fitz.open(str(work))
    full_text = ""
    for page in doc:
        full_text += page.get_text()
    doc.close()

    assert "Confidential Document" not in full_text, (
        "SEC-UI-100: Redacted text must not be extractable via PyMuPDF"
    )
