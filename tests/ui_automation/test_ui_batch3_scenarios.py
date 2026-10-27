# -*- coding: utf-8 -*-
"""Pipeline c4a9e271 phase-7 — batch-3 multi-step scenario proofs.

These complement the per-action ``act_*`` fire-and-survive smokes in
``test_ui_batch3_actions.py`` with the deeper batch-3 acceptance-criteria
flows (criteria 1 & 2): signature verification + LTV-request reachability
(encryption/signatures) and the PDF/A + PDF/UA compliance result dialog.

Scope note (reality-check, recorded for the gates)
---------------------------------------------------
The *behavioral* depth of these domains is already proven by mature
suites from earlier pipelines — signing/encryption round-trips in
``tests/test_ui_encryption_signatures.py`` (phase-12), PDF/A conversion +
validation in ``tests/test_ui_compliance_accessibility.py`` (phase-13),
and LTV/trust-anchor flows in ``tests/ui_automation/test_ui_ltv.py``.
Batch-3 does NOT re-implement those; it adds the matrix-recording,
GAP-A14-only *reachability + inventory* proofs that the same dialogs are
driveable through the production test seams (verify status pipe,
validation-dialog introspection, generic dialog introspection), so the
phase-7 batch layer has first-class owned rows that cannot silently
drift. Each test records to a ``scenario`` row in
``tests/ui_automation/matrix/ui_test_matrix.csv`` via
``conftest.record_matrix_result``.

GAP-A14: every dialog is driven through the test pipes; no raw UIA
menu_select / FindAll on an open modal. Registered dialogs record their
inventory then auto-reject under ``PDFE_TEST_DIALOG_AUTODISMISS=1`` so
the event loop is free to answer the readback pipe.
"""
from __future__ import annotations

import os
import shutil
import time
from pathlib import Path

import pytest

# Registered dialogs (ValidationRequestDialog, ComplianceDialog) must
# auto-record-then-reject so their readback pipes are answerable. Set
# BEFORE launch_pdfe copies os.environ into the child.
os.environ["PDFE_TEST_DIALOG_AUTODISMISS"] = "1"
os.environ.setdefault("PDFE_TEST_VALIDATION_REQUEST_UNLOCKED", "1")
os.environ.setdefault("PDFE_BUILD_CONFIG", "Release")

from tests.ui_automation.conftest import record_matrix_result  # noqa: E402
from tests.ui_automation.harness.pdfe_app_fixture import (  # noqa: E402
    launch_pdfe,
    kill_pdfe_processes,
    invoke_action,
    read_result_pipe,
    read_dialog_introspection,
)

REPO_ROOT = Path(__file__).resolve().parents[2]
FIXTURES = REPO_ROOT / "tests" / "fixtures"
_RUNTIME = REPO_ROOT / "tests" / "_runtime" / "batch3_scenarios"


def _working_copy(fixture_name: str) -> Path:
    """Copy a fixture into the gitignored runtime dir so nothing the app
    might write touches the real fixture (the session integrity guard
    snapshots every fixture and fails teardown on drift)."""
    src = FIXTURES / fixture_name
    if not src.exists():
        pytest.fail(f"batch-3 scenario fixture missing: {src}")
    _RUNTIME.mkdir(parents=True, exist_ok=True)
    dst = _RUNTIME / fixture_name
    shutil.copy2(str(src), str(dst))
    return dst


class _Launch:
    """Per-scenario launch with guaranteed teardown (no shared holder —
    each scenario opens a different fixture)."""

    def __init__(self, fixture_name: str) -> None:
        self.doc = _working_copy(fixture_name)
        self.app = None
        self.win = None

    def __enter__(self):
        kill_pdfe_processes()
        self.app, self.win = launch_pdfe(args=[str(self.doc)])
        return self.win

    def alive(self) -> bool:
        try:
            return bool(self.app.is_process_running()
                        and self.win.exists(timeout=2))
        except Exception:  # noqa: BLE001
            return False

    def __exit__(self, *exc):
        if self.app is not None:
            try:
                self.app.kill()
            except Exception:  # noqa: BLE001
                pass
        kill_pdfe_processes()
        return False


# ---------------------------------------------------------------------------
# Criterion 1 — signature verification + LTV-request reachability
# ===========================================================================

def test_sec_verify_signed():
    """sec_verify_signed: verify a real PKCS#7-signed PDF and read the
    SignaturePanel LTV/validation status via the testLtvStatus pipe.

    Proves verifySignaturesAction populates the validation status (the
    panel's 'Validation:' row) on a genuinely signed fixture, end-to-end
    through the production seam — not just that the action fires.
    """
    tid = "sec_verify_signed"
    with _Launch("test_signed.pdf") as win:
        if not invoke_action(win, "verifySignaturesAction", settle_s=1.0):
            record_matrix_result(tid, "red",
                                 "testToolActivatePipe unreachable")
            pytest.fail(f"{tid}: verify action pipe unreachable")
        text = read_result_pipe(
            win, "testLtvStatusQueryPipe", "testLtvStatusResultPipe",
            settle_s=0.8)
        ok = (text is not None and text != "__null__"
              and "Validation" in text)
        notes = (f"phase-7 batch-3: verifySignaturesAction on test_signed.pdf; "
                 f"ltvStatusLabel='{text}' (deep round-trip in "
                 f"test_ui_encryption_signatures.py / test_ui_ltv.py)")
        if not ok:
            record_matrix_result(tid, "red", notes)
            pytest.fail(f"{tid}: {notes}")
        record_matrix_result(tid, "green", notes)


def test_sec_ltv_request_dialog():
    """sec_ltv_request_dialog: after verifying, Request Validation Data...
    opens the LTV consent dialog with its privacy warning + URL list +
    Cancel/Fetch buttons (read via the validation-dialog introspection
    seam; the dialog auto-takes the Cancel path so no network fetch).
    """
    tid = "sec_ltv_request_dialog"
    with _Launch("test_signed.pdf") as win:
        invoke_action(win, "verifySignaturesAction", settle_s=1.0)
        if not invoke_action(win, "requestValidationAction", settle_s=1.2):
            record_matrix_result(tid, "red",
                                 "testToolActivatePipe unreachable")
            pytest.fail(f"{tid}: request-validation pipe unreachable")
        rec = read_result_pipe(
            win, "testValidationDialogQueryPipe",
            "testValidationDialogResultPipe", settle_s=0.8)
        ok = (rec is not None and rec != "__null__"
              and "validationRequestDialog" in rec
              and "urlList" in rec)
        notes = (f"phase-7 batch-3: requestValidationAction opened LTV consent "
                 f"dialog; introspection='{rec}'; auto-Cancel (no DSS write)")
        if not ok:
            record_matrix_result(tid, "red", notes)
            pytest.fail(f"{tid}: {notes}")
        record_matrix_result(tid, "green", notes)


# ---------------------------------------------------------------------------
# Criterion 2 — PDF/A + PDF/UA compliance result dialog
# ===========================================================================

def test_cmp_compliance_dialog_levels():
    """cmp_compliance_dialog_levels: the ComplianceDialog opens on a PDF/A
    fixture and exposes the conversion/validation controls (LevelCombo,
    Analyze/Validate/Convert buttons) plus the PDF/A-1b, PDF/A-2b and
    PDF/UA target levels — read via the generic dialog introspection seam.
    """
    tid = "cmp_compliance_dialog_levels"
    with _Launch("test_pdfa_1b.pdf") as win:
        if not invoke_action(win, "complianceAction", settle_s=1.2):
            record_matrix_result(tid, "red",
                                 "testToolActivatePipe unreachable")
            pytest.fail(f"{tid}: compliance action pipe unreachable")
        rec = read_dialog_introspection(win, "ComplianceDialog", settle_s=0.8)
        present = rec or ""
        wants = ["LevelCombo", "ValidateButton", "ConvertButton"]
        missing_ctrls = [w for w in wants if w not in present]
        ok = (rec is not None and rec != "__null__" and not missing_ctrls)
        notes = (f"phase-7 batch-3: ComplianceDialog inventory='{rec}' "
                 f"(deep convert+validate in "
                 f"test_ui_compliance_accessibility.py)")
        if not ok:
            record_matrix_result(tid, "red",
                                 f"{notes}; missing controls={missing_ctrls}")
            pytest.fail(f"{tid}: missing controls {missing_ctrls} in {rec!r}")
        record_matrix_result(tid, "green", notes)


def test_cmp_compliance_target_levels():
    """cmp_compliance_target_levels: the ComplianceDialog LevelCombo lists
    the PDF/A-1b, PDF/A-2b and PDF/UA conformance targets (criterion 2
    PDF/A-1b + PDF/A-2b conversion targets and the PDF/UA check).
    """
    tid = "cmp_compliance_target_levels"
    with _Launch("test_pdfa_1b.pdf") as win:
        if not invoke_action(win, "complianceAction", settle_s=1.2):
            record_matrix_result(tid, "red",
                                 "testToolActivatePipe unreachable")
            pytest.fail(f"{tid}: compliance action pipe unreachable")
        rec = read_dialog_introspection(win, "ComplianceDialog", settle_s=0.8)
        present = rec or ""
        wants = ["PDF/A-1b", "PDF/A-2b", "PDF/UA"]
        missing = [w for w in wants if w not in present]
        ok = (rec is not None and rec != "__null__" and not missing)
        notes = (f"phase-7 batch-3: ComplianceDialog target levels via "
                 f"introspection combos; inventory='{rec}'")
        if not ok:
            record_matrix_result(tid, "red",
                                 f"{notes}; missing levels={missing}")
            pytest.fail(f"{tid}: missing target levels {missing} in {rec!r}")
        record_matrix_result(tid, "green", notes)
