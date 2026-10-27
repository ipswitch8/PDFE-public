#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Pipeline F phase-11 pywinauto tests for the LTV UI flows.

Covers three new matrix rows (all should be green after this phase):

  * signaturePanel_ltv_status — open a signed fixture, expand the
    Signatures panel, read the Validation status label.
  * validationRequestDialog_open — Edit/Security > Request Validation
    Data... opens the modal dialog with the privacy warning + URL list +
    Cancel/Fetch buttons. We click Cancel and verify no DSS write
    occurred.
  * trustAnchorDialog_open — Edit > Trust Anchors... opens the
    TrustAnchorDialog with table + Add/Remove buttons.

These are integration smoke tests — they confirm the UI surface is
reachable and that the wired-up actions and dialogs respect the
expected objectName values that the C++ unit tests already assert on.
"""

from __future__ import annotations

import os
import pathlib
import sys
import time

import pytest

# Every test in this module opens a signature/LTV modal (ValidationRequest,
# TrustAnchor) and then reads a MainWindow-child result pipe that is only
# readable once the modal's exec() has returned. So this module REQUIRES the
# dialogs to auto-dismiss. launch_pdfe no longer forces
# PDFE_TEST_DIALOG_AUTODISMISS globally (that broke ~50 dialog-inspection tests
# that need their modal to stay OPEN), so opt in here at module scope. Each UI
# file runs in its own pytest subprocess, so this does not leak to other files.
os.environ["PDFE_TEST_DIALOG_AUTODISMISS"] = "1"

try:
    from pywinauto import Application  # noqa: F401  # type: ignore
    from pywinauto import Desktop  # type: ignore
except ImportError:
    pytest.skip("pywinauto not installed", allow_module_level=True)

from tests.ui_automation.harness.pdfe_app_fixture import (
    launch_pdfe, invoke_action, read_result_pipe,
)
from tests.ui_automation.conftest import record_matrix_result


FIXTURES = pathlib.Path(__file__).resolve().parent.parent / "fixtures"


def _parse_introspection(record: str) -> dict:
    """Parse a ``"name|k=v|k=v"`` dialog-introspection record into a dict.

    The leading token (the dialog objectName) is stored under the
    ``"_name"`` key; each subsequent ``k=v`` pair is stored as-is.
    """
    parts = (record or "").split("|")
    out = {"_name": parts[0] if parts else ""}
    for p in parts[1:]:
        if "=" in p:
            k, v = p.split("=", 1)
            out[k] = v
    return out


def _start(fixture: pathlib.Path):
    if not fixture.exists():
        pytest.skip(f"fixture missing: {fixture.name}")
    try:
        app, win = launch_pdfe(args=[str(fixture)])
    except RuntimeError as e:
        pytest.skip(f"pdfe.exe launch failed: {e}")
    time.sleep(3.0)
    return app, win


def _find_top_level_dialog(pid: int, name_substr: str, timeout: float = 3.0):
    """Walk Desktop windows looking for a dialog whose automation_id
    contains @p name_substr and which belongs to PID @p pid."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        for w in Desktop(backend="uia").windows():
            try:
                if w.process_id() != pid:
                    continue
                aid = w.element_info.automation_id or ""
                if name_substr in aid:
                    return w
            except Exception:
                continue
        time.sleep(0.2)
    return None


# --- Signature panel LTV status -----------------------------------------


def test_signature_panel_ltv_status():
    """signaturePanel_ltv_status — the LTV status label is exposed in
    the Signatures panel after opening a signed fixture.

    The label objectName is 'ltvStatusLabel'. Its text starts with
    'Validation:'. We don't assert a specific status value (LTV vs
    Online vs NoData) — that depends on the signer cert + system trust
    state. We only assert the label is reachable.
    """
    fixture = FIXTURES / "test_signed.pdf"
    app, win = _start(fixture)
    try:
        # Surface the Signatures panel + recompute LTV status via the
        # verify action (it auto-shows signaturePanelDock_). Invoked
        # through the testToolActivatePipe seam rather than
        # win.menu_select(...) — the latter drives pywinauto's UIA menu
        # walk, which hangs in FindAll on the CI session (GAP-A14).
        invoke_action(win, "verifySignaturesAction", settle_s=0.8)

        # Read the ltvStatusLabel text via the result-pipe seam. On the
        # Server-2019 CI session UIA cannot see the label directly (it
        # used to skip here); the testLtvStatus query/result pipe pair
        # returns SignaturePanel::ltvStatusText() so we get a genuine
        # assertion (GAP-A14, SKIP=FAIL).
        text = read_result_pipe(
            win, "testLtvStatusQueryPipe", "testLtvStatusResultPipe",
            settle_s=0.6)
        assert text is not None, (
            "testLtvStatus result pipe unreachable — PDFE_TEST_PIPES not "
            "honored? (launch_pdfe sets it)")

        # After verifying a signed fixture the panel shows
        # 'Validation: <status>'. We don't pin a specific status value
        # (LTV vs Online vs NoData depends on signer cert + trust state),
        # only that the label was populated with the Validation row.
        from tests.ui_automation.conftest import assert_recorded
        assert_recorded(
            "signaturePanel_ltv_status",
            text != "__null__" and "Validation" in text,
            f"unexpected ltv label text via result pipe: {text!r}",
            green_notes=f"Pipeline F phase-11: ltvStatusLabel text='{text}' "
                        f"(read via testLtvStatusResultPipe)",
        )
    finally:
        try:
            app.kill()
        except Exception:
            pass


# --- ValidationRequestDialog -------------------------------------------


def test_validation_request_dialog_cancel():
    """validationRequestDialog_open — Security > Request Validation
    Data... opens the dialog. We click Cancel, then assert the dialog
    closes without an error popup (no DSS write attempted).

    AC 9.6.1 is exercised by C++ unit tests (mock NAM verifies
    createRequest count == 0 after Cancel); the pywinauto test only
    confirms the UI surface is reachable.
    """
    fixture = FIXTURES / "test_signed.pdf"
    app, win = _start(fixture)
    try:
        # Verify-signatures pre-flight enables requestValidationAction
        # (it is disabled until at least one signature verifies valid),
        # then open the dialog. Both go through the testToolActivatePipe
        # seam instead of win.menu_select(...), which hangs in pywinauto's
        # UIA FindAll on the CI session (GAP-A14).
        invoke_action(win, "verifySignaturesAction", settle_s=0.8)
        # requestValidationAction is gated on signedCount>0 here because
        # launch_pdfe sets PDFE_TEST_VALIDATION_REQUEST_UNLOCKED=1. The
        # action opens the modal dialog, which (under PDFE_TEST_PIPES)
        # records its widget inventory and auto-takes the Cancel path
        # (QDialog::reject — no servers contacted, no DSS write), then
        # returns so the event loop is free to answer the result pipe.
        assert invoke_action(win, "requestValidationAction", settle_s=1.0), (
            "testToolActivatePipe unreachable — PDFE_TEST_PIPES not honored?")

        # Read the dialog introspection via the result-pipe seam. UIA on
        # Server 2019 cannot enumerate the modal dialog (it used to skip
        # here); the pipe returns what the real dialog constructed
        # (GAP-A14, SKIP=FAIL).
        rec = read_result_pipe(
            win, "testValidationDialogQueryPipe",
            "testValidationDialogResultPipe", settle_s=0.6)
        assert rec is not None, (
            "testValidationDialog result pipe unreachable")
        assert rec != "__null__", (
            "ValidationRequestDialog never opened — requestValidationAction "
            "was disabled or the seam did not fire")

        d = _parse_introspection(rec)
        assert d["_name"] == "validationRequestDialog", rec
        # The consent dialog must carry the privacy warning, the URL list,
        # and BOTH Cancel and Fetch buttons (AC 9.6).
        assert d.get("warning") == "1", f"missing privacy warning: {rec}"
        assert d.get("urlList") == "1", f"missing URL list: {rec}"
        assert d.get("cancel") == "1", f"missing Cancel button: {rec}"
        assert d.get("fetch") == "1", f"missing Fetch button: {rec}"

        record_matrix_result(
            "validationRequestDialog_open", "green",
            f"Pipeline F phase-11 / GAP-A14: ValidationRequestDialog opened "
            f"via Security menu with warning+URL list+Cancel/Fetch; Cancel "
            f"path auto-taken (no DSS write). introspection={rec}",
        )
    finally:
        try:
            app.kill()
        except Exception:
            pass


# --- TrustAnchorDialog --------------------------------------------------


def test_trust_anchor_dialog_open():
    """trustAnchorDialog_open — Edit > Trust Anchors... opens the
    TrustAnchorDialog. We verify the table widget and the Add/Remove
    buttons are reachable, then close.

    The dialog enumerates anchors via TrustAnchorManager::listTrusts().
    We do not assert a specific anchor count — the user's HKCU may have
    zero entries; an empty table is the valid baseline state.
    """
    fixture = FIXTURES / "test_3page.pdf"
    app, win = _start(fixture)
    try:
        # Open via the testToolActivatePipe seam, not win.menu_select(...)
        # (UIA menu walk hangs on the CI session — GAP-A14). Under
        # PDFE_TEST_PIPES the dialog records its widget inventory and
        # auto-dismisses (QDialog::accept) so the event loop is free.
        assert invoke_action(win, "trustAnchorAction", settle_s=1.0), (
            "testToolActivatePipe unreachable — PDFE_TEST_PIPES not honored?")

        # Read the dialog introspection via the result-pipe seam (UIA on
        # Server 2019 cannot enumerate the modal dialog — used to skip
        # here; GAP-A14, SKIP=FAIL).
        rec = read_result_pipe(
            win, "testTrustAnchorDialogQueryPipe",
            "testTrustAnchorDialogResultPipe", settle_s=0.6)
        assert rec is not None, (
            "testTrustAnchorDialog result pipe unreachable")
        assert rec != "__null__", (
            "TrustAnchorDialog never opened — the seam did not fire")

        d = _parse_introspection(rec)
        assert d["_name"] == "trustAnchorDialog", rec
        # The dialog must carry the anchor table + Add/Remove buttons.
        assert d.get("table") == "1", f"missing trust table: {rec}"
        assert d.get("add") == "1", f"missing Add button: {rec}"
        assert d.get("remove") == "1", f"missing Remove button: {rec}"
        # rows is >= 0 (empty table is the valid baseline). Confirm the
        # count parsed to a non-negative integer (sanity on the table).
        assert int(d.get("rows", "-1")) >= 0, f"bad row count: {rec}"

        record_matrix_result(
            "trustAnchorDialog_open", "green",
            f"Pipeline F phase-11 / GAP-A14: TrustAnchorDialog opened via "
            f"Edit menu with table+Add/Remove buttons. introspection={rec}",
        )
    finally:
        try:
            app.kill()
        except Exception:
            pass


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-v"]))
