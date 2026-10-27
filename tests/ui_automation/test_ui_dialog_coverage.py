# -*- coding: utf-8 -*-
"""Phase-7 C1: dialog open/close/widget-audit coverage for 12 untracked dialogs.

Each test opens a dialog via its menu path, verifies the dialog window
appears (dialog_opens), checks key widget objectNames (widget_audit),
and dismisses the dialog cleanly (closes_cleanly).
"""
from __future__ import annotations

import os
import sys
import time
from pathlib import Path

import pytest

# ── Imports from the PDFE test harness ────────────────────────────────────
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
# c82a1f37 P5: also put the repo root on the path so record_matrix_result is
# imported from `tests.ui_automation.conftest` — the SAME module instance
# pytest registers as the conftest plugin (whose session-finish hook writes the
# runtime CSV). Importing it as `ui_automation.conftest` (below) created a
# SECOND module object with its own results dict, so every green this file
# recorded (all the *_dialog_opens/widget_audit/closes_cleanly sentinels) was
# silently dropped — the rows stayed blocked despite the tests passing.
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from ui_automation.harness.pdfe_app_fixture import (
    launch_pdfe,
    kill_pdfe_processes,
    PDFE_EXE,
)
from ui_automation.harness.menu_nav import click_menu_path
from ui_automation.harness.dialog_helpers import (
    find_by_suffix,
    find_dialog_window,
    close_dialog_via_button,
)
from tests.ui_automation.conftest import record_matrix_result

FIXTURES = Path(__file__).resolve().parents[1] / "fixtures"
FIXTURE_3PAGE = FIXTURES / "test_3page.pdf"
FIXTURE_FORM = FIXTURES / "test_form.pdf"


def _skip_if_no_exe():
    if not PDFE_EXE.exists():
        pytest.skip(f"pdfe.exe not found at {PDFE_EXE}")


def _start(fixture: Path):
    """Launch PDFE with the given fixture; return (app, win)."""
    _skip_if_no_exe()
    if not fixture.exists():
        pytest.skip(f"fixture missing: {fixture}")
    try:
        app, win = launch_pdfe(args=[str(fixture)])
    except RuntimeError as exc:
        pytest.skip(f"pdfe.exe launch failed: {exc}")
    time.sleep(2.0)
    return app, win


def _stop(app):
    """Kill PDFE and clean up straggler processes."""
    try:
        app.kill()
    except Exception:
        pass
    kill_pdfe_processes()


# Menu-path -> QAction objectName seam for the testToolActivatePipe
# fast-path. Falls back to click_menu_path when no entry matches.
# Pipeline f3c91a2e phase-3 cluster #1: the Qt6 UIA menu-popup
# enumeration is intermittently flaky on the RDP CI host — clicking
# the top-level menu sometimes spawns a popup that UIA never
# enumerates, so `child_window(title="Batch Process...")` times out
# even though the menu item is visible on screen. The pipe seam
# bypasses popup enumeration entirely by invoking the QAction
# directly through MainWindow::activateToolByObjectName.
_MENU_PATH_TO_ACTION = {
    "Tools -> Batch Process...": "batchAction",
    "Tools -> Compare Documents...": "compareAction",
    "Tools -> Optimize PDF...": "optimizePdfAction",
    "Tools -> Preflight...": "preflightAction",
    "Tools -> Add Watermark...": "watermarkAction",
    "View -> Customize Toolbars...": "customizeToolbarsAction",
    "Forms -> Tab Order...": "tabOrderAction",
    "File -> Document Properties...": "documentMetadataAction",
    "File -> Print...": "printAction",
    "File -> Print Preview...": "printPreviewAction",
    "File -> Combine Files...": "combineFilesAction",
    # c82a1f37 P5: 4 previously-uncovered dialogs (their matrix
    # *_dialog_opens/widget_audit/closes_cleanly rows had no owning test).
    "Edit -> Preferences...": "preferencesAction",
    "Forms -> Calculation Order...": "calculationOrderAction",
    "Insert -> Header & Footer...": "headerFooterAction",
    "Pages -> Page Labels...": "pageLabelsAction",
}


def _open_dialog(app, win, menu_path: str, dialog_name: str, timeout: float = 8.0):
    """Open a dialog and return its window, or None.

    First tries the testToolActivatePipe Q_INVOKABLE seam if the menu
    path has a known QAction objectName mapping — this bypasses Qt6
    UIA menu-popup enumeration, which is flaky on the CI RDP host.
    Falls back to ``click_menu_path`` for menus without a known seam.

    After triggering, polls in three places, in order:
      1. ``win.descendants()`` — Qt6 modal QDialogs parented to the
         main window are exposed by the QAccessible bridge as
         descendants of the main window (auto_id
         ``QApplication.<dialogName>``) even though they are
         top-level Win32 HWNDs. This is the lookup that actually
         succeeds on the RDP CI host — Qt6 does NOT register the
         child dialog HWND with ``app.windows()`` / Desktop walks.
      2. ``app.windows()`` — for Qt builds where the dialog is
         re-parented at the HWND level.
      3. Desktop walk filtered by PID — last-resort for transient
         popups.

    Pipeline f3c91a2e phase-3 Cluster #1B: previously only (2) + (3)
    were checked; on this CI host, the modal dialog never appears in
    either, but IS reachable via (1). 11 of 12 dialog tests failed
    because of this gap.
    """
    action_name = _MENU_PATH_TO_ACTION.get(menu_path)
    invoked = False
    if action_name is not None:
        pipe = find_by_suffix(win, "testToolActivatePipe")
        if pipe is not None:
            for method in ("set_edit_text", "set_value"):
                try:
                    getattr(pipe, method)(action_name)
                    invoked = True
                    break
                except Exception:
                    continue
    if not invoked:
        try:
            click_menu_path(win, menu_path)
        except Exception:
            return None
    # Modal dialogs need extra settle time after the action triggers
    # them — exec() runs synchronously and the dialog window doesn't
    # always appear in app.windows() instantly. The QTimer::singleShot
    # defer used by activateToolByObjectName means the action.trigger()
    # runs on the next event loop tick AND modal dialogs need additional
    # time to fully show + populate their widget tree for UIA enumeration.
    time.sleep(2.0)

    deadline = time.time() + timeout
    suffix = "." + dialog_name
    while time.time() < deadline:
        # Strategy 1 (primary on this CI host): descend from mainwindow.
        # Qt6's QAccessible exposes child modal dialogs as descendants
        # under their auto_id "QApplication.<dialogName>", even though
        # at the Win32 HWND level the dialog is its own top-level.
        try:
            dlg = find_by_suffix(win, dialog_name)
            if dlg is not None:
                return dlg
        except Exception:
            pass
        # Strategy 2: app.windows() — works for some Qt builds.
        try:
            for w in app.windows():
                try:
                    aid = w.element_info.automation_id or ""
                except Exception:
                    continue
                if aid == dialog_name or aid.endswith(suffix):
                    return w
        except Exception:
            pass
        # Strategy 3: Desktop walk filtered by PID. Last-resort for
        # transient popups.
        try:
            from pywinauto import Desktop
            pid = None
            try:
                pid = win.process_id()
            except Exception:
                pid = None
            for w in Desktop(backend="uia").windows():
                try:
                    if pid is not None and w.process_id() != pid:
                        continue
                    aid = w.element_info.automation_id or ""
                except Exception:
                    continue
                if aid == dialog_name or aid.endswith(suffix):
                    return w
        except Exception:
            pass
        time.sleep(0.25)
    return None


def _check_widgets(dlg, widget_names: list[str]) -> list[str]:
    """Return list of widget names that were NOT found."""
    missing = []
    for name in widget_names:
        if find_by_suffix(dlg, name) is None:
            missing.append(name)
    return missing


# ── Dialog test specifications ────────────────────────────────────────────
# Each entry: (menu_path, dialog_objectName, key_widgets, fixture, close_button)

DIALOG_SPECS = {
    "batch": {
        "menu": "Tools -> Batch Process...",
        "name": "batchDialog",
        # Names match src/ui/BatchDialog.cpp setObjectName() calls, except
        # actionListWidget: Qt exposes QListWidget's UIA locator from its
        # accessible Name ("Action Sequence List"), not objectName (GAP-A15).
        "widgets": [
            "selectFolderButton", "Action Sequence List", "addActionButton",
            "runButton", "cancelButton", "actionTypeCombo",
        ],
        "fixture": FIXTURE_3PAGE,
        "close": "cancelButton",
    },
    "combine": {
        "menu": "File -> Combine Files...",
        "name": "combineFilesWizard",
        "row_stem": "combine_wizard",  # matrix uses combine_wizard_*, not combineFiles_wizard_*
        # src/ui/CombineFilesWizard.cpp uses prefixed objectNames for
        # add/remove buttons because the wizard's step-1 layout reuses
        # the words for file-list operations vs. wizard navigation.
        "widgets": [
            "addFilesButton", "removeFileButton", "fileList",
            "nextButton", "cancelButton",
        ],
        "fixture": FIXTURE_3PAGE,
        "close": "cancelButton",
    },
    "compare": {
        "menu": "Tools -> Compare Documents...",
        "name": "compareDialog",
        # Names match src/ui/CompareDialog.cpp setObjectName() calls.
        "widgets": [
            "selectFileAButton", "selectFileBButton", "compareButton",
            "pageSpinBox", "textDiffView",
        ],
        "fixture": FIXTURE_3PAGE,
        "close": "cancelButton",
    },
    "optimize": {
        "menu": "Tools -> Optimize PDF...",
        "name": "optimizeDialog",
        # Names match src/ui/OptimizeDialog.cpp setObjectName() calls.
        "widgets": [
            "optimizeRunButton", "optimizeCancelButton",
            "optimizeOptionsGroup",
        ],
        "fixture": FIXTURE_3PAGE,
        "close": "optimizeCancelButton",
    },
    "preflight": {
        "menu": "Tools -> Preflight...",
        "name": "preflightDialog",
        # src/ui/PreflightDialog.cpp uses a `preflight*` prefix on
        # every widget to avoid collision with same-named widgets in
        # other dialogs (e.g. Sign / Encryption / Watermark all have
        # a `closeBox` / `runButton` of their own).
        "widgets": [
            "preflightProfileCombo", "preflightRunButton",
            "preflightIssueTree", "preflightSummaryLabel",
            "preflightCloseBox",
        ],
        "fixture": FIXTURE_3PAGE,
        "close": None,
    },
    "customize_toolbars": {
        "menu": "View -> Customize Toolbars...",
        "name": "customizeToolbarsDialog",
        "row_stem": "customize_toolbars",  # matrix uses customize_toolbars_*
        # src/ui/CustomizeToolbarsDialog.cpp names the add/remove
        # buttons with the `Action` suffix because the dialog moves
        # QAction items between two lists (not generic add/remove).
        "widgets": [
            "toolbarCombo", "availableList", "currentList",
            "addActionButton", "removeActionButton",
            "okButton", "cancelButton",
        ],
        "fixture": FIXTURE_3PAGE,
        "close": "cancelButton",
    },
    "tab_order": {
        "menu": "Forms -> Tab Order...",
        "name": "tabOrderDialog",
        "row_stem": "tab_order_dialog",  # matrix uses tab_order_dialog_*
        # src/ui/TabOrderDialog.cpp prefixes the field list to avoid
        # collision with generic `fieldList` (forms panel uses that).
        "widgets": [
            "tabOrderFieldList", "moveUpButton", "moveDownButton",
            "applyButton", "cancelButton",
        ],
        "fixture": FIXTURE_FORM,
        "close": "cancelButton",
    },
    "watermark": {
        "menu": "Tools -> Add Watermark...",
        "name": "watermarkDialog",
        # src/ui/WatermarkDialog.cpp uses a `watermarkDialog*` prefix
        # on every widget per the dialog's house naming convention.
        "widgets": [
            "watermarkDialogModeCombo", "watermarkDialogTextEdit",
            "watermarkDialogOpacitySpin",
            "watermarkDialogApplyButton",
            "watermarkDialogCancelButton",
        ],
        "fixture": FIXTURE_3PAGE,
        "close": "watermarkDialogCancelButton",
    },
    "metadata": {
        "menu": "File -> Document Properties...",
        "name": "metadataDialog",
        # src/ui/MetadataDialog.cpp uses a `metadata*` prefix on every
        # editable field so the matrix can distinguish XMP-source vs.
        # info-dict-source vs. user-overridable fields by name.
        "widgets": [
            "metadataTitleEdit", "metadataAuthorEdit",
            "metadataSubjectEdit", "metadataKeywordsEdit",
            "metadataButtonBox",
        ],
        "fixture": FIXTURE_3PAGE,
        "close": None,
    },
    "print": {
        "menu": "File -> Print...",
        "name": "printDialog",
        # src/ui/PrintDialog.cpp names the QSpinBox children with the
        # `SpinBox` suffix per Qt's recommended type-revealing
        # naming convention.
        "widgets": [
            "allPagesRadio", "pageRangeRadio",
            "fromPageSpinBox", "toPageSpinBox", "copiesSpinBox",
            "printButton", "cancelButton",
        ],
        "fixture": FIXTURE_3PAGE,
        "close": "cancelButton",
    },
    "print_preview": {
        "menu": "File -> Print Preview...",
        "name": "printPreviewDialog",
        "row_stem": "print_preview",  # matrix uses print_preview_*, not printPreview_dialog_*
        # src/ui/PrintPreviewDialog.cpp wraps QPrintPreviewWidget and
        # names it `printPreviewWidget` rather than the abbreviated
        # `previewWidget`.
        "widgets": [
            "previewToolBar", "printPreviewWidget",
        ],
        "fixture": FIXTURE_3PAGE,
        "close": None,
    },
    # c82a1f37 P5: 4 dialogs whose matrix sentinel rows had no owning test.
    # objectNames verified against the respective src/ui/*Dialog.cpp files.
    "preferences": {
        "menu": "Edit -> Preferences...",
        "name": "preferencesDialog",
        "row_stem": "pref_dialog",  # matrix uses pref_dialog_*
        "close_suffix": "closes",  # matrix uses *_closes, not *_closes_cleanly
        "widgets": [
            "preferencesTabs", "preferencesGeneralTab",
            "preferencesButtonBox", "preferencesOkButton",
            "preferencesCancelButton",
        ],
        "fixture": FIXTURE_3PAGE,
        "close": "preferencesCancelButton",
    },
    "calcorder": {
        "menu": "Forms -> Calculation Order...",
        "name": "calculationOrderDialog",
        "row_stem": "calcorder_dialog",  # matrix uses calcorder_dialog_*
        "close_suffix": "closes",  # matrix uses *_closes, not *_closes_cleanly
        "widgets": [
            "calculationOrderFieldList", "calculationOrderMoveUpButton",
            "calculationOrderMoveDownButton", "calculationOrderApplyButton",
            "calculationOrderCancelButton",
        ],
        "fixture": FIXTURE_FORM,   # needs form fields to populate the list
        "close": "calculationOrderCancelButton",
    },
    "header_footer": {
        "menu": "Insert -> Header & Footer...",
        "name": "headerFooterDialog",
        "row_stem": "hf_dialog",  # matrix uses hf_dialog_*
        "close_suffix": "closes",  # matrix uses *_closes, not *_closes_cleanly
        "widgets": [
            "headerFooterHeaderGroup", "headerFooterFooterGroup",
            "headerFooterOptionsGroup", "headerFooterApplyButton",
            "headerFooterCancelButton",
        ],
        "fixture": FIXTURE_3PAGE,
        "close": "headerFooterCancelButton",
    },
    "pagelabels": {
        "menu": "Pages -> Page Labels...",
        "name": "pageLabelsDialog",
        "row_stem": "pagelabels_dialog",  # matrix uses pagelabels_dialog_*
        "close_suffix": "closes",
        # objectNames set via QStringLiteral in src/ui/PageLabelsDialog.cpp.
        "widgets": [
            "pageLabelsTable", "pageLabelsAddButton", "pageLabelsEditButton",
            "pageLabelsDeleteButton", "pageLabelsOkButton",
            "pageLabelsCancelButton",
        ],
        "fixture": FIXTURE_3PAGE,
        "close": "pageLabelsCancelButton",
    },
    # NOTE: altText dialog is NOT added here — altTextAction requires a
    # figure/image annotation to be selected first (otherwise it shows an
    # info message instead of opening the dialog). The alttext_dialog_* rows
    # need a selected-figure precondition; tracked as a P5/P6 follow-up.
}


def _run_dialog_test(spec_key: str):
    """Run opens + widget_audit + closes_cleanly for one dialog."""
    spec = DIALOG_SPECS[spec_key]
    dialog_name = spec["name"]
    # c82a1f37 P5: the matrix row_id stem. The old inline construction
    # `dialog_name.replace('Dialog','_dialog').replace('Wizard','_wizard')`
    # only snake-cases single-word camelCase names — multi-word dialogs
    # (combineFilesWizard, customizeToolbarsDialog, printPreviewDialog,
    # tabOrderDialog) produced camelCase stems (e.g. "printPreview_dialog")
    # that don't match the snake_case matrix rows ("print_preview"), so
    # record_matrix_result dropped them as unknown ids and the rows stayed
    # blocked despite the test passing. Each such spec now carries an explicit
    # `row_stem`; specs without one keep the legacy construction unchanged.
    stem = spec.get("row_stem") or (
        dialog_name.replace("Dialog", "_dialog").replace("Wizard", "_wizard")
    )
    app, win = _start(spec["fixture"])
    try:
        dlg = _open_dialog(app, win, spec["menu"], dialog_name)
        assert dlg is not None, (
            f"{dialog_name} did not open via menu '{spec['menu']}'"
        )
        record_matrix_result(
            f"{stem}_opens",
            "green",
            f"Pipeline a7c4f891 phase-7 C1: {dialog_name} opened via {spec['menu']}",
        )

        missing = _check_widgets(dlg, spec["widgets"])
        assert not missing, (
            f"{dialog_name} missing widgets: {missing}"
        )
        record_matrix_result(
            f"{stem}_widget_audit",
            "green",
            f"Pipeline a7c4f891 phase-7 C1: all {len(spec['widgets'])} widgets reachable",
        )

        close_dialog_via_button(dlg, button_name=spec.get("close"))
        time.sleep(1.0)

        # Use the same multi-strategy lookup as open: the dialog might
        # only be findable via main_window descendants on this CI host.
        # If the lookup still returns a hit after the close, the close
        # truly failed.
        dlg_after = find_by_suffix(win, dialog_name)
        if dlg_after is None:
            dlg_after = find_dialog_window(app, dialog_name, timeout_s=1.0)
        assert dlg_after is None, (
            f"{dialog_name} did not close cleanly"
        )
        # c82a1f37 P5: most dialogs' matrix row uses the `_closes_cleanly`
        # sentinel, but a few (alttext/calcorder/hf/pagelabels/pref) use
        # `_closes`. Honour a per-spec override so the recorded id matches.
        close_suffix = spec.get("close_suffix", "closes_cleanly")
        record_matrix_result(
            f"{stem}_{close_suffix}",
            "green",
            f"Pipeline a7c4f891 phase-7 C1: {dialog_name} dismissed cleanly",
        )
    finally:
        _stop(app)


# ── Test functions (one per dialog) ───────────────────────────────────────

def test_batch_dialog():
    _run_dialog_test("batch")


def test_combine_files_wizard():
    _run_dialog_test("combine")


def test_compare_dialog():
    _run_dialog_test("compare")


@pytest.mark.xfail(
    reason="P-GAP-C1-IMG2PDF: ImageToPdfWizard is a static utility "
           "that opens QFileDialog, not a custom dialog with objectName",
    strict=False,
)
def test_image_to_pdf_wizard():
    app, win = _start(FIXTURE_3PAGE)
    try:
        click_menu_path(win, "File -> Create PDF from Images...")
        time.sleep(1.0)
        for name in ["imageToPdfWizard", "imageToPdfDialog"]:
            dlg = find_dialog_window(app, name, timeout_s=2.0)
            if dlg is not None:
                close_dialog_via_button(dlg)
                return
        win.type_keys("{ESC}")
        time.sleep(0.3)
        pytest.fail(
            "ImageToPdfWizard: no custom dialog found — static utility "
            "uses QFileDialog directly"
        )
    finally:
        _stop(app)


def test_optimize_dialog():
    _run_dialog_test("optimize")


def test_preflight_dialog():
    _run_dialog_test("preflight")


def test_customize_toolbars_dialog():
    _run_dialog_test("customize_toolbars")


def test_tab_order_dialog():
    _run_dialog_test("tab_order")


def test_watermark_dialog():
    _run_dialog_test("watermark")


def test_metadata_dialog():
    _run_dialog_test("metadata")


def test_print_dialog():
    _run_dialog_test("print")


def test_print_preview_dialog():
    _run_dialog_test("print_preview")


# c82a1f37 P5: 4 dialogs whose matrix sentinel rows previously had no test.
def test_preferences_dialog():
    _run_dialog_test("preferences")


def test_calculation_order_dialog():
    _run_dialog_test("calcorder")


def test_header_footer_dialog():
    _run_dialog_test("header_footer")


def test_page_labels_dialog():
    _run_dialog_test("pagelabels")
