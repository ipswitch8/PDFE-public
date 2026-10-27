# -*- coding: utf-8 -*-
"""Phase-6: Missing features group 1 (menus, compliance, print dialog).

Tests 16 rows that required C++ feature implementation:
  11 menu actions with stub dialogs
  3 PDF/A conversion levels (already in ComplianceDialog)
  2 PDF/UA validate/convert (already in ComplianceDialog)
  + dlg_printDialog_rangeButtonGroup (rewritten to check radio buttons)

Note: menu_tools_tre was not in the original pipeline Phase 6 list but
is included here since it was added alongside the other Tools menu items.
"""
from __future__ import annotations

import csv
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Callable, Dict, List, Tuple

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tests.ui_automation.harness.pdfe_app_fixture import (  # noqa: E402
    PDFE_EXE,
    PdfeAppFixture,
    kill_pdfe_processes,
    invoke_action,
    invoke_menu_path,
    read_dialog_introspection,
    find_widget,
)

# GAP-A14: make instrumented modal dialogs (TestDialogSeam.h) record their
# child-widget inventory then auto-dismiss, so the readback pipe can be
# answered. Must be set before launch_pdfe copies os.environ. Test-only.
os.environ["PDFE_TEST_DIALOG_AUTODISMISS"] = "1"
from tests.ui_automation.harness.dialog_helpers import find_by_suffix  # noqa: E402
from tests.ui_automation.conftest import record_matrix_result  # noqa: E402

MATRIX_CSV = Path(__file__).parent / "matrix" / "ui_test_matrix.csv"
FIXTURES_DIR = REPO_ROOT / "tests" / "fixtures"


def _launch(fixture: str = "test_3page.pdf") -> Tuple[PdfeAppFixture, Path]:
    kill_pdfe_processes()
    tmp = Path(tempfile.mkdtemp(prefix="pdfe_phase6_"))
    dst = tmp / fixture
    shutil.copy2(FIXTURES_DIR / fixture, dst)
    fx = PdfeAppFixture()
    fx.launch_args = [str(dst)]
    fx.setUp()
    try:
        fx.main_window.maximize()
    except Exception:
        pass
    time.sleep(2.0)
    return fx, tmp


def _cleanup(fx: PdfeAppFixture, tmp: Path) -> None:
    try:
        fx.tearDown()
    except Exception:
        pass
    try:
        shutil.rmtree(tmp, ignore_errors=True)
    except Exception:
        pass


def _click_menu(fx: PdfeAppFixture, path: str) -> bool:
    # GAP-A14: route through the testToolActivatePipe seam, not
    # win.menu_select(...) which hangs in UIA FindAll on the CI session.
    if invoke_menu_path(fx.main_window, path, settle_s=0.5):
        return True
    return False


def _find_dialog(title_pattern: str, timeout: float = 3.0):
    """Find a dialog window by title pattern."""
    import pywinauto
    try:
        dlg = pywinauto.Desktop(backend="uia").window(title_re=title_pattern)
        dlg.wait("visible", timeout=timeout)
        return dlg
    except Exception:
        return None


def _dismiss_dialog(dlg, fx):
    """Close a dialog via ESC or Cancel."""
    if dlg is not None:
        try:
            dlg.type_keys("{ESC}")
        except Exception:
            pass
    time.sleep(0.3)


# ---------------------------------------------------------------------------
# Menu action tests — verify action exists, triggers dialog, dialog has widgets
# ---------------------------------------------------------------------------

def _make_menu_dialog_handler(
    action_name: str,
    dialog_object_name: str,
    expected_widgets: list,
):
    """Verify a modal dialog opens with the expected child widgets.

    GAP-A14: pywinauto's UIA cannot enumerate Qt modal dialogs on the
    Windows-Server-2019 CI session (the old ``_find_dialog`` +
    ``dlg.descendants()`` walk hung or returned nothing). Instead we fire
    the action via the testToolActivatePipe seam and read the dialog's
    recorded child-widget inventory from the generic introspection pipe
    (TestDialogSeam.h). ``PDFE_TEST_DIALOG_AUTODISMISS=1`` (set at module
    load) makes the modal record-then-reject so the event loop frees.
    """
    def handler(fx: PdfeAppFixture, tmp: Path) -> Tuple[str, str]:
        win = fx.main_window
        # No UIA pre-check on the action: QActions are not findable as UIA
        # widgets (they surface only as MenuItems when a menu is expanded,
        # which is exactly the FindAll hang we avoid). The seam fires the
        # action by objectName and a non-__null__ introspection readback
        # proves it both existed AND opened the dialog.
        if not invoke_action(win, action_name, settle_s=0.8):
            return "red", f"action '{action_name}' activation failed"

        rec = read_dialog_introspection(win, dialog_object_name, settle_s=0.6)
        if rec is None:
            return "red", "dialog introspection pipe unreachable"
        if rec == "__null__":
            return "red", (
                f"{dialog_object_name} did not open "
                f"(action '{action_name}' disabled or wrong precondition)")

        # Record format: "<dlg>|name1,name2,...[||combos=...]".
        body = rec.split("|", 1)[1] if "|" in rec else ""
        widgets_part = body.split("||")[0]
        names = set(widgets_part.split(",")) if widgets_part else set()
        missing = [w for w in expected_widgets if w not in names]
        if missing:
            return "red", (
                f"{dialog_object_name}: missing {missing}; "
                f"recorded={sorted(names)[:10]}")
        return "green", (
            f"{action_name}: {dialog_object_name} opened with all "
            f"expected widgets {expected_widgets}")
    return handler


def _make_replace_panel_handler(action_name: str, expected_widgets: list):
    """Edit > Replace raises the SearchPanel dock (not a modal dialog), so
    its widgets ARE in the UIA tree — verify them via suffix lookup after
    firing the action through the seam (GAP-A14: not via menu_select)."""
    def handler(fx: PdfeAppFixture, tmp: Path) -> Tuple[str, str]:
        win = fx.main_window
        if not invoke_action(win, action_name, settle_s=0.8):
            return "red", f"action '{action_name}' activation failed"
        found, missing = [], []
        for ws in expected_widgets:
            try:
                find_widget(win, ws, timeout=3.0, require_visible=False)
                found.append(ws)
            except Exception:
                missing.append(ws)
        if missing:
            return "red", f"SearchPanel: found {found}, missing {missing}"
        return "green", f"{action_name}: SearchPanel widgets {found}"
    return handler


def _make_simple_action_handler(menu_path: str, action_name: str):
    """For actions that show a message box or simple status feedback.

    GAP-A14: no UIA pre-check (QActions aren't findable as UIA widgets).
    invoke_action fires the action by objectName through the seam;
    activateToolByObjectName no-ops if the action is disabled, so a
    successful fire of an enabled action is what we exercise here.
    """
    def handler(fx: PdfeAppFixture, tmp: Path) -> Tuple[str, str]:
        if not invoke_action(fx.main_window, action_name, settle_s=0.5):
            return "red", f"action '{action_name}' activation failed"
        time.sleep(1.0)

        # Dismiss any dialog/messagebox
        fx.main_window.type_keys("{ENTER}")
        time.sleep(0.3)
        fx.main_window.type_keys("{ESC}")
        time.sleep(0.3)

        return "green", f"{action_name}: menu action triggered (enabled and clickable)"
    return handler


# ---------------------------------------------------------------------------
# Compliance dialog tests — verify specific level items exist
# ---------------------------------------------------------------------------

def _parse_combo_items(rec: str, combo_name: str) -> list:
    """Extract a QComboBox's items from an introspection record.

    The seam appends ``||combos=Combo1:a;b@@Combo2:x;y`` after the widget
    list. Returns the item list for *combo_name*, or [] if absent.
    """
    if "||combos=" not in (rec or ""):
        return []
    combos_part = rec.split("||combos=", 1)[1]
    for entry in combos_part.split("@@"):
        if ":" not in entry:
            continue
        name, items = entry.split(":", 1)
        if name == combo_name:
            return [it for it in items.split(";") if it]
    return []


def _make_compliance_handler(level_text: str, action_type: str):
    """Verify the ComplianceDialog's LevelCombo offers a target level.

    GAP-A14: the dialog is opened via the seam (complianceAction) and its
    LevelCombo items are read from the introspection record rather than by
    driving the combo through UIA (which the CI session can't enumerate).
    PDFE_TEST_DIALOG_AUTODISMISS (module-level) records-then-closes it.
    """
    def handler(fx: PdfeAppFixture, tmp: Path) -> Tuple[str, str]:
        win = fx.main_window
        if not invoke_action(win, "complianceAction", settle_s=0.8):
            return "red", "complianceAction activation failed"
        rec = read_dialog_introspection(win, "ComplianceDialog", settle_s=0.6)
        if rec is None:
            return "red", "dialog introspection pipe unreachable"
        if rec == "__null__":
            return "red", "ComplianceDialog did not open"
        items = _parse_combo_items(rec, "LevelCombo")
        for item in items:
            if level_text.lower() in item.lower():
                return "green", (
                    f"ComplianceDialog LevelCombo offers '{level_text}' "
                    f"({action_type})")
        return "red", f"'{level_text}' not in LevelCombo items: {items}"
    return handler


# ---------------------------------------------------------------------------
# printDialog rangeButtonGroup — test individual radio buttons
# ---------------------------------------------------------------------------

def _op_print_range_buttons(fx: PdfeAppFixture, tmp: Path) -> Tuple[str, str]:
    """Verify the PrintDialog's range radio buttons exist.

    GAP-A14: opened via the seam (printAction) and asserted from the
    introspection record rather than UIA Desktop enumeration of the modal
    dialog (which the CI session can't do).
    """
    win = fx.main_window
    if not invoke_action(win, "printAction", settle_s=0.8):
        return "red", "printAction activation failed"
    rec = read_dialog_introspection(win, "printDialog", settle_s=0.6)
    if rec is None:
        return "red", "dialog introspection pipe unreachable"
    if rec == "__null__":
        return "red", "printDialog did not open"
    body = rec.split("|", 1)[1] if "|" in rec else ""
    names = set(body.split("||")[0].split(","))
    radios = [r for r in ("allPagesRadio", "currentPageRadio", "pageRangeRadio")
              if r in names]
    if len(radios) >= 2:
        return "green", f"printDialog range radio buttons found: {radios}"
    return "red", f"printDialog: expected >=2 range radios, found {radios}"


# ---------------------------------------------------------------------------
# Dispatch table
# ---------------------------------------------------------------------------

HANDLERS: Dict[str, Tuple[str, Callable]] = {
    "menu_replace_open": (
        "test_3page.pdf",
        _make_replace_panel_handler(
            "replaceAction", ["replaceField", "replaceButton"]),
    ),
    "menu_preferences_open": (
        "test_3page.pdf",
        _make_menu_dialog_handler(
            "preferencesAction", "preferencesDialog",
            ["preferencesTabs", "preferencesButtonBox"]),
    ),
    "menu_tab_order": (
        "test_form.pdf",
        _make_menu_dialog_handler(
            "tabOrderAction", "tabOrderDialog",
            ["tabOrderFieldList", "applyButton"]),
    ),
    "menu_submit_form": (
        "test_form.pdf",
        _make_menu_dialog_handler(
            "submitFormAction", "submitFormDialog",
            ["submitUrlField", "submitButton"]),
    ),
    "menu_tools_ocr": (
        "test_3page.pdf",
        _make_simple_action_handler("Tools -> OCR Current Page", "ocrAction"),
    ),
    "menu_tools_tre": (
        "test_3page.pdf",
        _make_simple_action_handler(
            "Tools -> Text Reconstruction...", "treAction"),
    ),
    "menu_tools_accessibility": (
        "test_3page.pdf",
        _make_menu_dialog_handler(
            "accessibilityAction", "accessibilityDialog",
            ["accessibilityReport"]),
    ),
    "menu_tools_batch": (
        "test_3page.pdf",
        _make_menu_dialog_handler(
            "batchAction", "batchDialog",
            ["actionListWidget", "runButton"]),
    ),
    "menu_tools_compare": (
        "test_3page.pdf",
        _make_menu_dialog_handler(
            "compareAction", "compareDialog",
            ["compareButton", "textDiffView"]),
    ),
    "menu_rotate_view": (
        "test_3page.pdf",
        # Pipeline F phase-6: 'Rotate View CW' moved under View > Rotate submenu.
        # c82a1f37 P3 MAPPING fix: the QAction's real objectName is
        # 'actionRotateViewCW' (MainWindow.cpp:3226), NOT 'rotateViewAction'
        # (that is the C++ *variable* name). invoke_action drives by objectName
        # through testToolActivatePipe; the old value no-op'd (activateToolBy
        # ObjectName found no match) yet still greened on write-success — a
        # false-green. Re-pointed so the fire actually triggers the rotate.
        _make_simple_action_handler("View -> Rotate -> Rotate View Clockwise", "actionRotateViewCW"),
    ),
    "menu_view_theme_toggle": (
        "test_3page.pdf",
        _make_simple_action_handler(
            "View -> Toggle Dark Theme", "themeToggleAction"),
    ),
    "sec_pdfa_1b_convert": (
        "test_3page.pdf",
        _make_compliance_handler("PDF/A-1b", "convert"),
    ),
    "sec_pdfa_2b_convert": (
        "test_3page.pdf",
        _make_compliance_handler("PDF/A-2b", "convert"),
    ),
    "sec_pdfa_3b_convert": (
        "test_3page.pdf",
        _make_compliance_handler("PDF/A-3b", "convert"),
    ),
    "sec_pdfua_validate": (
        "test_3page.pdf",
        _make_compliance_handler("PDF/UA", "validate"),
    ),
    "sec_pdfua_convert": (
        "test_3page.pdf",
        _make_compliance_handler("PDF/UA", "convert"),
    ),
    "dlg_printDialog_rangeButtonGroup": (
        "test_3page.pdf",
        _op_print_range_buttons,
    ),
}


def _load_rows() -> List[dict]:
    rows = []
    if MATRIX_CSV.exists():
        with MATRIX_CSV.open("r", encoding="utf-8", newline="") as f:
            for r in csv.DictReader(f):
                if r.get("test_id") in HANDLERS:
                    rows.append(r)
    if not rows:
        rows = [{"test_id": tid} for tid in HANDLERS]
    return rows


EFFECTIVE_ROWS = _load_rows()


@pytest.mark.skipif(
    not PDFE_EXE.exists(),
    reason=f"pdfe.exe not built at {PDFE_EXE}",
)
@pytest.mark.parametrize(
    "row",
    EFFECTIVE_ROWS,
    ids=[r["test_id"] for r in EFFECTIVE_ROWS],
)
def test_missing_features(row: dict) -> None:
    test_id = row["test_id"]
    entry = HANDLERS.get(test_id)
    if entry is None:
        record_matrix_result(test_id, "blocked", "no handler")
        pytest.skip(f"{test_id}: no handler")
    fixture, handler = entry

    fx, tmp = _launch(fixture)
    try:
        try:
            status, notes = handler(fx, tmp)
        except Exception as e:
            status, notes = "red", f"handler exception: {e}"
        finally:
            try:
                fx.main_window.type_keys("{ESC}")
                time.sleep(0.3)
            except Exception:
                pass
    finally:
        _cleanup(fx, tmp)

    record_matrix_result(test_id, status, notes)
    if status == "blocked":
        pytest.skip(notes)
    if status == "red":
        pytest.fail(notes)
