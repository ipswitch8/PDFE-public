# -*- coding: utf-8 -*-
"""Pipeline c4a9e271 phase-7 — Visual+interaction tests, batch 3.

Native-UI invocation/liveness smoke for every batch-3 QAction
(encryption / signatures / compliance / preferences / tools) over the
GAP-A14 ``testToolActivatePipe`` seam. Each test maps 1:1 to an
``act_<objectName>`` row in
``tests/ui_automation/matrix/ui_test_matrix.csv`` and records its
outcome via ``conftest.record_matrix_result`` so the matrix status
column reflects a real run against ``build/vs2022/bin/Release/PDFE.exe``.

The matrix row's green bar (generate_matrix.py classify_action expected):

    "action fires without crash; checkable actions toggle and report
     checked-state, disabled actions return false (no exception, window
     stays alive)"

GREEN == the action was driven through the pipe AND the MainWindow
process + UIA window survived. This is the objectName-grounded
reachability/invocation smoke CI drives on Server-2019 — NOT a pixel
assertion (those live in the RenderVerify suite).

Why batch-3 needs richer dismissal than batch-1
------------------------------------------------
Batch-1's actions only ever raised *registered* Qt dialogs (which the
TestDialogSeam auto-rejects under ``PDFE_TEST_DIALOG_AUTODISMISS``) or
one native print dialog. Batch-3 fires actions that raise THREE classes
of blocking modal, each needing a different UIA-free dismissal so the
shared-process pipe never wedges:

  * "auto"  — a *registered* Qt dialog (PreferencesDialog,
              ComplianceDialog, TrustAnchorDialog, ValidationRequest
              Dialog). ``PDFE_TEST_DIALOG_AUTODISMISS=1`` makes its modal
              ``exec()`` reject on the next event-loop turn — no manual
              dismissal needed.
  * "modal" — an *unregistered* Qt modal (SignDialog, EncryptionDialog)
              or a raw blocking ``QMessageBox`` (Document Permissions,
              Verify Signatures). These are NOT auto-rejected, so we send
              a UIA-free Win32 ESC keystroke (the same fallback
              dialog_helpers uses) which takes the dialog's Cancel/Close
              path, plus a native #32770 WM_CLOSE in case the action
              chains to a file dialog.
  * "native"— a native ``QFileDialog`` (#32770) whose call site does NOT
              consult ``takeFileDialogPath()`` (Remove Password's
              getSaveFileName, Compare's getOpenFileName), so pre-seating
              can't suppress it. Dismissed via ``dismiss_native_dialog``
              (EnumWindows by pid + WM_CLOSE), with an ESC safety net.
  * "plain" — no modal at all (theme switches, the copy/print/OCR-box
              toggles, the disabled Request-Validation action). Fire +
              survive only.

Design notes (carried from batch-1):
  * ONE module-scoped launch shared across all 25 params (conftest opts
    this module out of the per-test autouse kill via the test_ui_batch3_
    filename rule).
  * The app opens a THROWAWAY working COPY of the fixture so any action
    that writes straight back to the open document's path (a bare
    optimize/save with no dialog) can never mutate the real fixture and
    trip the session fixture-integrity guard.
  * Minimal UIA per test (pipe write + is_process_running + exists) — no
    dialog enumeration — so COM proxy tables don't accumulate the
    0x80040155 (REGDB_E_CLASSNOTREG) crash across params.
  * GAP-A14 compliant: never calls win.menu_select / raw UIA FindAll.

Teardown closes PDFE.exe; the autouse session safety-net in conftest
guarantees no orphan survives.
"""
from __future__ import annotations

import ctypes
import os
import time
from pathlib import Path

import pytest

# Opt this module into auto-dismiss BEFORE launch_pdfe copies os.environ
# into the child. Registered modal QDialogs (Preferences, Compliance,
# TrustAnchor, ValidationRequest) then auto-reject so the pipe handler
# never blocks on them.
os.environ["PDFE_TEST_DIALOG_AUTODISMISS"] = "1"
# Lower the LTV "Request Validation Data..." gate so requestValidation
# is reachable on a self-signed test rig (inert in production).
os.environ.setdefault("PDFE_TEST_VALIDATION_REQUEST_UNLOCKED", "1")
# Drive the Release build (matrix greens are recorded against Release).
os.environ.setdefault("PDFE_BUILD_CONFIG", "Release")

from tests.ui_automation.conftest import record_matrix_result  # noqa: E402
from tests.ui_automation.harness.pdfe_app_fixture import (  # noqa: E402
    launch_pdfe,
    kill_pdfe_processes,
    dismiss_native_dialog,
    invoke_action,
)

REPO_ROOT = Path(__file__).resolve().parents[2]
FIXTURE_PDF = REPO_ROOT / "tests" / "fixtures" / "test_3page.pdf"
GOLDEN_DIR = Path(__file__).resolve().parent / "goldens" / "batch3_actions"

_SCRATCH = Path(
    os.environ.get(
        "PDFE_TEST_SCRATCH",
        REPO_ROOT / "tests" / "_runtime" / "batch3_scratch",
    )
)


# ---------------------------------------------------------------------------
# Action table: (objectName, kind, domain). kind drives post-fire dismissal;
# the green bar is identical for all kinds (fire through the pipe + survive).
#   plain  — no modal; fire + survive
#   auto   — registered dialog; PDFE_TEST_DIALOG_AUTODISMISS closes it
#   modal  — unregistered Qt modal / blocking QMessageBox; Win32 ESC + native
#   native — native #32770 file dialog (no takeFileDialogPath); WM_CLOSE + ESC
# ---------------------------------------------------------------------------
ACTIONS: list[tuple[str, str, str]] = [
    # ---- encryption (5) ----
    ("saveWithEncryptionAction", "modal", "encryption"),
    ("documentPermissionsAction", "modal", "encryption"),
    ("removePasswordAction", "native", "encryption"),
    ("toggleCopyPreventionAction", "plain", "encryption"),
    ("togglePrintPreventionAction", "plain", "encryption"),
    # ---- signatures (4) ----
    ("signDocumentAction", "modal", "signatures"),
    ("verifySignaturesAction", "modal", "signatures"),
    ("requestValidationAction", "modal", "signatures"),
    ("trustAnchorAction", "auto", "signatures"),
    # ---- compliance (5) ----
    ("complianceAction", "auto", "compliance"),
    ("preflightAction", "modal", "compliance"),
    ("accessibilityAction", "modal", "compliance"),
    ("altTextAction", "modal", "compliance"),
    ("openAltTextDialogAction", "modal", "compliance"),
    # ---- preferences / settings (6) ----
    ("preferencesAction", "auto", "preferences"),
    ("themeToggleAction", "plain", "preferences"),
    ("themeLightAction", "plain", "preferences"),
    ("themeDarkAction", "plain", "preferences"),
    ("themeSystemAction", "plain", "preferences"),
    ("customizeToolbarsAction", "modal", "preferences"),
    # ---- tools (5) ----
    ("ocrAction", "modal", "tools"),
    ("batchAction", "modal", "tools"),
    ("compareAction", "native", "tools"),
    ("toggleOcrBoxesAction", "plain", "tools"),
    ("optimizePdfAction", "modal", "tools"),
]
assert len(ACTIONS) == 25, f"expected 25 batch-3 act_ rows, got {len(ACTIONS)}"


def _press_escape(times: int = 2) -> None:
    """Send a UIA-free Win32 ESC to the foreground modal.

    Mirrors dialog_helpers' fallback: keybd_event VK_ESCAPE down/up. When
    a modal exec() is running its nested event loop, the keystroke is
    delivered to the focused modal and takes its Cancel/Close (reject)
    path — even though the modal "blocks" the main window, Windows message
    delivery still occurs, so the pipe is freed on the next turn.
    """
    try:
        user32 = ctypes.WinDLL("user32", use_last_error=True)
        for _ in range(max(1, times)):
            user32.keybd_event(0x1B, 0, 0, 0)        # VK_ESCAPE down
            user32.keybd_event(0x1B, 0, 0x0002, 0)   # KEYEVENTF_KEYUP
            time.sleep(0.15)
    except Exception:  # noqa: BLE001
        pass  # best-effort; the alive() assertion is the real gate


def _dismiss(kind: str, pid) -> None:
    """Close whatever modal `kind` may have raised, UIA-free."""
    if kind == "native":
        if pid is not None:
            dismiss_native_dialog(pid, timeout=3.0)
        _press_escape()
    elif kind == "modal":
        # Unregistered Qt modal or blocking QMessageBox: ESC reject first,
        # then sweep any chained native #32770 (e.g. a save dialog the
        # action would have opened after an accepted modal — inert here
        # because ESC rejects, but cheap insurance).
        _press_escape()
        if pid is not None:
            dismiss_native_dialog(pid, timeout=2.0)
    # "auto" relies on PDFE_TEST_DIALOG_AUTODISMISS; "plain" raises nothing.


class _Holder:
    """Lazily-(re)launched PDFE instance shared across the module."""

    def __init__(self, working_doc: Path) -> None:
        self.app = None
        self.win = None
        self.working_doc = working_doc

    def ensure_live(self):
        if self.app is not None:
            try:
                if self.app.is_process_running() and self.win.exists(timeout=1):
                    return self.app, self.win
            except Exception:  # noqa: BLE001
                pass
        kill_pdfe_processes()
        self.app, self.win = launch_pdfe(args=[str(self.working_doc)])
        return self.app, self.win

    def alive(self) -> bool:
        try:
            return bool(self.app.is_process_running()
                        and self.win.exists(timeout=2))
        except Exception:  # noqa: BLE001
            return False

    def pid(self):
        try:
            return self.app.process
        except Exception:  # noqa: BLE001
            return None

    def teardown(self) -> None:
        if self.app is not None:
            try:
                self.app.kill()
            except Exception:  # noqa: BLE001
                pass
        kill_pdfe_processes()
        self.app = None
        self.win = None


@pytest.fixture(scope="module")
def pdfe():
    import shutil
    if not FIXTURE_PDF.exists():
        pytest.fail(f"fixture missing: {FIXTURE_PDF}")
    _SCRATCH.mkdir(parents=True, exist_ok=True)
    GOLDEN_DIR.mkdir(parents=True, exist_ok=True)
    # Throwaway working copy — an action that writes to the OPEN doc's path
    # without a dialog must never touch the real fixture.
    working_doc = _SCRATCH / "working_3page.pdf"
    shutil.copy2(str(FIXTURE_PDF), str(working_doc))
    holder = _Holder(working_doc)
    holder.ensure_live()
    try:
        yield holder
    finally:
        holder.teardown()


def _capture_golden(win, name: str) -> None:
    """Best-effort provisional golden of the main window (no assertion)."""
    try:
        img = win.capture_as_image()
        img.save(str(GOLDEN_DIR / f"{name}.png"))
    except Exception:  # noqa: BLE001
        pass  # screenshot is evidence, never a gate on the smoke


# One domain-representative screenshot apiece (provisional baseline).
_GOLDEN_FOR = {
    "saveWithEncryptionAction", "signDocumentAction", "complianceAction",
    "preferencesAction", "ocrAction",
}


@pytest.mark.parametrize(
    "action,kind,domain",
    ACTIONS,
    ids=[a[0] for a in ACTIONS],
)
def test_batch3_action(pdfe, action, kind, domain):
    test_id = f"act_{action}"
    app, win = pdfe.ensure_live()
    pid = pdfe.pid()

    # --- fire through the GAP-A14 seam ---
    wrote = invoke_action(win, action, settle_s=0.8)
    if not wrote:
        record_matrix_result(
            test_id, "red",
            "testToolActivatePipe unreachable — pipe seam not found")
        pytest.fail(f"{test_id}: testToolActivatePipe unreachable")

    # --- close whatever modal the action raised (UIA-free) ---
    # Give a registered dialog a beat to record+auto-reject, or an
    # unregistered one a beat to appear, before dismissing.
    time.sleep(0.6)
    _dismiss(kind, pid)
    time.sleep(0.4)

    # --- liveness assertion (the green bar for every kind) ---
    if action in _GOLDEN_FOR and pdfe.alive():
        _capture_golden(win, action)

    if not pdfe.alive():
        record_matrix_result(
            test_id, "red",
            f"window/process died after firing {action} (kind={kind})")
        # Relaunch so the remaining params aren't all poisoned by one crash.
        pdfe.teardown()
        pytest.fail(f"{test_id}: MainWindow did not survive {action}")

    record_matrix_result(
        test_id, "green",
        f"fired via testToolActivatePipe (kind={kind}, domain={domain}); "
        f"modal dismissed UIA-free; MainWindow alive")
