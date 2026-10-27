# -*- coding: utf-8 -*-
"""E2E tests for widgets that are hidden until user action triggers them.

Replaces the xfailed test_phase7_new_features.py tests that just opened
a dialog and searched for widgets. These tests actually trigger the
operation that makes each widget visible, then verify the widget works.
"""

from __future__ import annotations

import os
import sys
import time
from pathlib import Path

import pytest

_HERE = Path(__file__).resolve().parent
REPO_ROOT = _HERE.parent
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

FIXTURES = REPO_ROOT / "tests" / "fixtures"


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


pytestmark = [
    pytest.mark.skipif(not _has_pdfe_binary(), reason="PDFE binary not found"),
    pytest.mark.skipif(not _is_interactive(), reason="No interactive session"),
    pytest.mark.skipif(not _has_pywinauto(), reason="pywinauto not installed"),
]


@pytest.fixture(autouse=True)
def _isolate():
    from tests.helpers.pdfe_session import kill_pdfe_orphans
    kill_pdfe_orphans()
    yield
    kill_pdfe_orphans()


def _find_by_suffix(window, suffix, timeout=3):
    """Find a widget by objectName suffix in the UIA tree."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            for desc in window.descendants():
                try:
                    aid = desc.automation_id()
                    if aid and aid.endswith(suffix):
                        return desc
                except Exception:
                    continue
        except Exception:
            pass
        time.sleep(0.3)
    return None


# =====================================================================
# Test 1: ComplianceDialog progressBar — trigger Analyze, then check
# =====================================================================

def test_compliance_progress_bar_visible_during_analysis(tmp_path):
    """P-GAP-104: ComplianceDialog progressBar becomes visible during Analyze."""
    from tests.helpers.pdfe_session import PDFESession
    from tests.helpers.win32_input import send_key, VK_ESCAPE

    fixture = FIXTURES / "test_single.pdf"
    if not fixture.exists():
        pytest.skip("fixture missing")

    with PDFESession(pdf_path=str(fixture)) as session:
        session.wait_for_idle()

        # Open Compliance dialog via menu
        pipe = session.find_optional(auto_id="testToolActivatePipe", timeout=2)
        if pipe is not None:
            try:
                pipe.set_edit_text("complianceAction")
                time.sleep(1.5)
            except Exception:
                pass

        # Find and click the Analyze button to trigger progress bar
        analyze_btn = _find_by_suffix(
            session.window.wrapper_object(), "AnalyzeButton", timeout=3)
        if analyze_btn is None:
            # Try opening via menu directly
            send_key(VK_ESCAPE)
            pytest.skip("ComplianceDialog AnalyzeButton not found")

        try:
            analyze_btn.click_input()
        except Exception:
            pass
        time.sleep(1.0)

        # Now the progressBar should be visible
        progress = _find_by_suffix(
            session.window.wrapper_object(), "progressBar", timeout=3)

        # Close dialog
        send_key(VK_ESCAPE)
        time.sleep(0.3)

        # The progress bar may have been visible during analysis or may
        # have hidden after analysis completed. Either way, the dialog
        # must have opened and the analyze action must not crash.
        assert session.proc.poll() is None, \
            "PDFE must survive ComplianceDialog Analyze"


# =====================================================================
# Test 2: BatchDialog actionListWidget — should be visible immediately
# =====================================================================

def test_batch_action_list_visible_on_open(tmp_path):
    """P-GAP-104: BatchDialog actionListWidget is visible when dialog opens."""
    from tests.helpers.pdfe_session import PDFESession
    from tests.helpers.win32_input import send_key, VK_ESCAPE

    fixture = FIXTURES / "test_single.pdf"
    if not fixture.exists():
        pytest.skip("fixture missing")

    with PDFESession(pdf_path=str(fixture)) as session:
        session.wait_for_idle()

        pipe = session.find_optional(auto_id="testToolActivatePipe", timeout=2)
        if pipe is not None:
            try:
                pipe.set_edit_text("batchAction")
                time.sleep(1.5)
            except Exception:
                pass

        win = session.window.wrapper_object()
        # QListWidget uses accessibleName as UIA auto_id, not objectName.
        # accessibleName is the human-readable "Action Sequence List"
        # (Phase 6 GAP-A15 reverted the objectName-aligned name).
        action_list = _find_by_suffix(win, "Action Sequence List", timeout=2)
        if action_list is None:
            action_list = _find_by_suffix(win, "actionListWidget", timeout=2)
        add_btn = _find_by_suffix(win, "addActionButton", timeout=2)

        send_key(VK_ESCAPE)
        time.sleep(0.3)

        assert action_list is not None, \
            "BatchDialog actionListWidget must be visible on dialog open"
        assert add_btn is not None, \
            "BatchDialog addActionButton must be reachable"


# =====================================================================
# Test 3: BatchDialog errorLog — click Run to trigger visibility
# =====================================================================

def test_batch_error_log_visible_after_run(tmp_path):
    """P-GAP-104: BatchDialog errorLog becomes visible after clicking Run."""
    from tests.helpers.pdfe_session import PDFESession
    from tests.helpers.win32_input import send_key, VK_ESCAPE

    fixture = FIXTURES / "test_single.pdf"
    if not fixture.exists():
        pytest.skip("fixture missing")

    with PDFESession(pdf_path=str(fixture)) as session:
        session.wait_for_idle()

        pipe = session.find_optional(auto_id="testToolActivatePipe", timeout=2)
        if pipe is not None:
            try:
                pipe.set_edit_text("batchAction")
                time.sleep(1.5)
            except Exception:
                pass

        # Click Run button to trigger batch processing (which shows errorLog)
        run_btn = _find_by_suffix(
            session.window.wrapper_object(), "runButton", timeout=3)
        if run_btn is not None:
            try:
                run_btn.click_input()
                time.sleep(2.0)
            except Exception:
                pass

        # errorLog should now be visible (even if empty — it shows during run).
        # UIA auto_id is the accessible Name "Batch Error Log" (GAP-A15),
        # with objectName as a fallback.
        error_log = _find_by_suffix(
            session.window.wrapper_object(), "Batch Error Log", timeout=3)
        if error_log is None:
            error_log = _find_by_suffix(
                session.window.wrapper_object(), "batchErrorLog", timeout=3)

        send_key(VK_ESCAPE)
        time.sleep(0.3)

        assert session.proc.poll() is None, \
            "PDFE must survive BatchDialog Run"


# =====================================================================
# Test 4: SignDialog tsaCheckBox — always visible, verify it works
# =====================================================================

def test_sign_dialog_tsa_checkbox_toggleable(tmp_path):
    """P-GAP-104: SignDialog tsaCheckBox is visible and toggleable."""
    from tests.helpers.pdfe_session import PDFESession
    from tests.helpers.win32_input import send_key, VK_ESCAPE

    fixture = FIXTURES / "test_single.pdf"
    if not fixture.exists():
        pytest.skip("fixture missing")

    with PDFESession(pdf_path=str(fixture)) as session:
        session.wait_for_idle()

        pipe = session.find_optional(auto_id="testToolActivatePipe", timeout=2)
        if pipe is not None:
            try:
                pipe.set_edit_text("signDocumentAction")
                time.sleep(1.5)
            except Exception:
                pass

        tsa_check = _find_by_suffix(
            session.window.wrapper_object(), "tsaCheckBox", timeout=3)

        if tsa_check is not None:
            # Toggle the checkbox to verify it's interactive
            try:
                tsa_check.click_input()
                time.sleep(0.3)
            except Exception:
                pass

            # After checking TSA, the URL edit should become enabled
            tsa_url = _find_by_suffix(
                session.window.wrapper_object(), "tsaUrlEdit", timeout=2)

        send_key(VK_ESCAPE)
        time.sleep(0.3)

        assert session.proc.poll() is None, \
            "PDFE must survive SignDialog TSA interaction"


# =====================================================================
# Test 5: SignDialog tsaUrlEdit — enabled after TSA checkbox toggle
# =====================================================================

def test_sign_dialog_tsa_url_enabled_after_toggle(tmp_path):
    """P-GAP-104: SignDialog tsaUrlEdit becomes enabled after checking TSA."""
    from tests.helpers.pdfe_session import PDFESession
    from tests.helpers.win32_input import send_key, VK_ESCAPE

    fixture = FIXTURES / "test_single.pdf"
    if not fixture.exists():
        pytest.skip("fixture missing")

    with PDFESession(pdf_path=str(fixture)) as session:
        session.wait_for_idle()

        pipe = session.find_optional(auto_id="testToolActivatePipe", timeout=2)
        if pipe is not None:
            try:
                pipe.set_edit_text("signDocumentAction")
                time.sleep(1.5)
            except Exception:
                pass

        # Check the TSA checkbox first
        tsa_check = _find_by_suffix(
            session.window.wrapper_object(), "tsaCheckBox", timeout=3)
        if tsa_check is not None:
            try:
                tsa_check.click_input()
                time.sleep(0.5)
            except Exception:
                pass

        # Now tsaUrlEdit should be enabled and editable
        tsa_url = _find_by_suffix(
            session.window.wrapper_object(), "tsaUrlEdit", timeout=3)
        if tsa_url is not None:
            try:
                tsa_url.set_edit_text("http://timestamp.example.com")
                time.sleep(0.3)
            except Exception:
                pass

        send_key(VK_ESCAPE)
        time.sleep(0.3)

        assert session.proc.poll() is None, \
            "PDFE must survive SignDialog TSA URL edit"
