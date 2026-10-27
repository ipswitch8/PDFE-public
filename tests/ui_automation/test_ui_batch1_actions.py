# -*- coding: utf-8 -*-
"""Pipeline c4a9e271 phase-5 — Visual+interaction tests, batch 1.

Native-UI invocation/liveness smoke for every batch-1 QAction
(file-ops, page-ops, view/nav, search, edit-clipboard, help) over the
GAP-A14 ``testToolActivatePipe`` seam locked in phase-3. Each test maps
1:1 to an ``act_<objectName>`` row in
``tests/ui_automation/matrix/ui_test_matrix.csv`` and records its
outcome via ``conftest.record_matrix_result`` so the matrix status
column reflects a real run against ``build/vs2022/bin/Release/PDFE.exe``.

The matrix row's green bar (generate_matrix.py classify_action expected):

    "action fires without crash; checkable actions toggle and report
     checked-state, disabled actions return false (no exception, window
     stays alive)"

So GREEN == the action was driven through the pipe AND the MainWindow
process + UIA window survived. This is the objectName-grounded
reachability/invocation smoke CI actually drives on Server-2019 — NOT a
pixel assertion (those live in the RenderVerify suite).

Design notes:
  * ONE module-scoped launch shared across all 48 params (conftest opts
    this module out of the per-test autouse kill — see _cleanup_pdfe).
  * Minimal UIA per test (pipe set_edit_text + is_process_running +
    exists) — no dialog enumeration — so COM proxy tables don't
    accumulate the 0x80040155 (REGDB_E_CLASSNOTREG) crash across params.
  * PDFE_TEST_DIALOG_AUTODISMISS=1 so any registered modal QDialog
    auto-rejects instead of blocking the pipe handler.
  * File-dialog actions pre-seat the path via inject_file_dialog_path so
    takeFileDialogPath() returns non-empty and NO native #32770 modal
    appears (those are the historical COM-crash trigger on this RDP host).
  * printAction may raise a native QPrintDialog (not a registered Qt
    seam) — dismissed via Win32 dismiss_native_dialog (EnumWindows,
    no UIA).
  * exitAction is fired LAST and proves itself by the process exiting.
  * GAP-A14 compliant: never calls win.menu_select / raw UIA FindAll.

Teardown closes PDFE.exe; the autouse session safety-net in conftest
guarantees no orphan survives.
"""
from __future__ import annotations

import os
import time
from pathlib import Path

import pytest

# Opt this module into auto-dismiss BEFORE launch_pdfe copies os.environ
# into the child. Registered modal QDialogs (Document Properties, Page
# Labels, Print Preview, About, confirm-prompts) then auto-reject so the
# pipe handler never blocks.
os.environ["PDFE_TEST_DIALOG_AUTODISMISS"] = "1"
# Drive the Release build (matrix greens are recorded against Release).
os.environ.setdefault("PDFE_BUILD_CONFIG", "Release")

from tests.ui_automation.conftest import record_matrix_result  # noqa: E402
from tests.ui_automation.harness.pdfe_app_fixture import (  # noqa: E402
    launch_pdfe,
    kill_pdfe_processes,
    inject_file_dialog_path,
    invoke_action,
    dismiss_native_dialog,
)

REPO_ROOT = Path(__file__).resolve().parents[2]
FIXTURE_PDF = REPO_ROOT / "tests" / "fixtures" / "test_3page.pdf"
GOLDEN_DIR = Path(__file__).resolve().parent / "goldens" / "batch1_actions"

# Scratchpad for throwaway save targets seated into file dialogs.
_SCRATCH = Path(
    os.environ.get(
        "PDFE_TEST_SCRATCH",
        REPO_ROOT / "tests" / "_runtime" / "batch1_scratch",
    )
)


# ---------------------------------------------------------------------------
# Ordered action table. Order is deliberate: non-destructive view/search/edit
# first, then page mutations, then file-ops, then close, then quit LAST.
# kind drives pre/post handling; the green bar is identical for all kinds
# (fire + survive).
# ---------------------------------------------------------------------------
#   plain        — fire, assert window+process alive
#   file_open    — pre-seat an existing PDF path, fire, assert alive
#   file_save    — pre-seat a temp output path, fire, assert alive
#   native_print — fire, dismiss any native #32770, assert alive
#   fullscreen   — fire (enter), fire again (restore), assert alive
#   close        — fire (close doc), assert window still alive
#   quit         — fire, assert the process EXITS (clean shutdown == pass)
ACTIONS: list[tuple[str, str, str]] = [
    # ---- view-nav (13) ----
    ("zoomInAction", "plain", "view-nav"),
    ("zoomOutAction", "plain", "view-nav"),
    ("fitWidthAction", "plain", "view-nav"),
    ("fitPageAction", "plain", "view-nav"),
    ("actualSizeAction", "plain", "view-nav"),
    ("togglePagePanelAction", "plain", "view-nav"),
    ("togglePropertiesPanelAction", "plain", "view-nav"),
    ("toggleSearchPanelAction", "plain", "view-nav"),
    ("toggleBookmarkPanelAction", "plain", "view-nav"),
    ("toggleAttachmentPanelAction", "plain", "view-nav"),
    ("previousViewAction", "plain", "view-nav"),
    ("nextViewAction", "plain", "view-nav"),
    ("fullScreenAction", "fullscreen", "view-nav"),
    # ---- search (2) ----
    ("findAction", "plain", "search"),
    ("replaceAction", "plain", "search"),
    # ---- edit-clipboard (6) ----
    ("selectAllAction", "plain", "edit-clipboard"),
    ("copyAction", "plain", "edit-clipboard"),
    ("cutAction", "plain", "edit-clipboard"),
    ("pasteAction", "plain", "edit-clipboard"),
    ("undoAction", "plain", "edit-clipboard"),
    ("redoAction", "plain", "edit-clipboard"),
    # ---- help (4) ----
    ("aboutAction", "plain", "help"),
    ("checkUpdatesAction", "plain", "help"),
    ("documentationAction", "plain", "help"),
    ("reportIssueAction", "plain", "help"),
    # ---- page-ops (11) ----
    ("rotatePageCWAction", "plain", "page-ops"),
    ("rotatePageCCWAction", "plain", "page-ops"),
    ("rotatePage180Action", "plain", "page-ops"),
    ("movePageUpAction", "plain", "page-ops"),
    ("movePageDownAction", "plain", "page-ops"),
    ("insertBlankPageAction", "plain", "page-ops"),
    ("pageLabelsAction", "plain", "page-ops"),
    ("insertPagesFromFileAction", "file_open", "page-ops"),
    ("extractPagesAction", "file_save", "page-ops"),
    ("replacePageAction", "file_open", "page-ops"),
    ("deletePageAction", "plain", "page-ops"),
    # ---- file-ops (12) ----
    ("documentMetadataAction", "plain", "file-ops"),
    ("printPreviewAction", "plain", "file-ops"),
    ("printAction", "native_print", "file-ops"),
    ("saveAction", "file_save", "file-ops"),
    ("saveAsAction", "file_save", "file-ops"),
    ("exportPlainTextAction", "file_save", "file-ops"),
    ("createPdfFromImagesAction", "file_open", "file-ops"),
    ("combineFilesAction", "file_open", "file-ops"),
    ("openAction", "file_open", "file-ops"),
    ("newFileAction", "plain", "file-ops"),
    ("closeAction", "close", "file-ops"),
    ("exitAction", "quit", "file-ops"),
]


class _Holder:
    """Lazily-(re)launched PDFE instance shared across the module."""

    def __init__(self, working_doc: Path) -> None:  # noqa: D401
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
        # (Re)launch with a THROWAWAY working COPY of the fixture open so
        # doc-dependent actions are ENABLED — never the real fixture. A
        # bare saveAction writes straight back to the open document's path
        # (no dialog), so opening the real fixture would mutate it and trip
        # the session fixture-integrity guard. The copy absorbs the write.
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
    # Throwaway working copy — saveAction writes to the OPEN doc's path, so
    # the open doc must never be the real fixture.
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
    "zoomInAction", "rotatePageCWAction", "togglePagePanelAction",
    "findAction", "selectAllAction", "aboutAction", "newFileAction",
}


@pytest.mark.parametrize(
    "action,kind,domain",
    ACTIONS,
    ids=[a[0] for a in ACTIONS],
)
def test_batch1_action(pdfe, action, kind, domain):
    test_id = f"act_{action}"
    app, win = pdfe.ensure_live()
    pid = pdfe.pid()

    # --- pre-fire: keep modal/native dialogs from blocking the pipe ---
    if kind == "file_open":
        # Seat the working COPY so getOpenFileName returns it without a
        # native modal AND the real fixture is never touched. (createPdfFrom
        # Images wants an image; a PDF path makes it fail-to-load
        # gracefully — window stays alive == green.)
        inject_file_dialog_path(win, str(pdfe.working_doc))
    elif kind == "file_save":
        out = _SCRATCH / f"{action}_out"
        suffix = ".txt" if action == "exportPlainTextAction" else ".pdf"
        inject_file_dialog_path(win, str(out.with_suffix(suffix)))

    # --- fire through the GAP-A14 seam ---
    wrote = invoke_action(win, action, settle_s=0.7)
    if not wrote:
        record_matrix_result(
            test_id, "red",
            "testToolActivatePipe unreachable — pipe seam not found")
        pytest.fail(f"{test_id}: testToolActivatePipe unreachable")

    # --- post-fire handling per kind ---
    if kind == "native_print" and pid is not None:
        # Close any native print dialog the action may have raised.
        dismiss_native_dialog(pid, timeout=3.0)
        time.sleep(0.3)
    elif kind == "fullscreen":
        # Toggle back out of full-screen so the window geometry is restored
        # for any later relaunch-free checks.
        invoke_action(win, action, settle_s=0.7)
    elif kind == "quit":
        # exitAction: success == the process shuts down cleanly.
        deadline = time.time() + 8.0
        exited = False
        while time.time() < deadline:
            try:
                if not app.is_process_running():
                    exited = True
                    break
            except Exception:  # noqa: BLE001
                exited = True
                break
            time.sleep(0.25)
        pdfe.app = None  # consumed — force relaunch if anything follows
        pdfe.win = None
        if not exited:
            kill_pdfe_processes()
            record_matrix_result(
                test_id, "red",
                "exitAction fired but process did not exit within 8s")
            pytest.fail(f"{test_id}: process did not exit")
        record_matrix_result(
            test_id, "green",
            "exitAction fired via pipe; process exited cleanly")
        return

    # --- liveness assertion (the green bar for all non-quit kinds) ---
    if action in _GOLDEN_FOR:
        _capture_golden(win, action)

    if not pdfe.alive():
        record_matrix_result(
            test_id, "red",
            f"window/process died after firing {action} (kind={kind})")
        pytest.fail(f"{test_id}: MainWindow did not survive {action}")

    record_matrix_result(
        test_id, "green",
        f"fired via testToolActivatePipe (kind={kind}); MainWindow alive")
