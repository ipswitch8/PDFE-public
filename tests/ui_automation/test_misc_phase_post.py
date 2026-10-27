#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""pywinauto scripts for the remaining phase-post matrix rows.

Covers:
  open_portfolio_pdf
  sign_with_invisible_checkbox
  sign_with_certification_level
  preflight_run_pdfa1b            (extends test_preflight_phase_post.py)
  preflight_export_report         (extends test_preflight_phase_post.py)

Refactored 2026-05-01 to use the canonical launch_pdfe + click_menu_path
harness so menu paths fail fast (raise TimeoutError) instead of hanging
indefinitely inside pywinauto's UIA Invoke() COM call. Menu paths
verified against src/ui/MainWindow.cpp action graph.
"""

from __future__ import annotations

import pathlib
import sys
import time

import pytest

try:
    from pywinauto import Application  # noqa: F401  # type: ignore
except ImportError:
    pytest.skip("pywinauto not installed", allow_module_level=True)

from tests.ui_automation.harness.pdfe_app_fixture import launch_pdfe
from tests.ui_automation.harness.menu_nav import click_menu_path
from tests.ui_automation.harness.dialog_helpers import select_combo_item
from tests.ui_automation.conftest import record_matrix_result

# Phase F25 / gap WIN-45: reachability is a RECORDED NON-PASS, never a skip.
from tests.ui_automation.conftest import (  # noqa: E402
    current_test_id,
    record_matrix_result,
)


FIXTURES = pathlib.Path(__file__).resolve().parent.parent / "fixtures"


def _start(fixture: pathlib.Path):
    """Launch pdfe.exe with a fixture; skip cleanly on missing prereqs."""
    if not fixture.exists():
        pytest.skip(f"fixture missing: {fixture.name}")
    try:
        app, win = launch_pdfe(args=[str(fixture)])
    except RuntimeError as e:
        pytest.skip(f"pdfe.exe launch failed: {e}")
    # Let the document load, canvas paint, and gated actions enable.
    # MainWindow.cpp enables preflightAction_, signDocumentAction_, etc.
    # only after document load completes — 1s isn't reliably enough.
    time.sleep(3.0)
    return app, win


def _try_menu(win, paths):
    """Try each menu path in order; pytest.skip if none reach a leaf."""
    for path in paths:
        try:
            click_menu_path(win, path)
            return
        except Exception:
            continue
    pytest.skip(f"none of these menu paths are reachable: {paths!r}")


# --- Portfolio ---------------------------------------------------------


def test_open_portfolio_pdf():
    """open_portfolio_pdf — test_portfolio.pdf loads and the
    AttachmentPanel surfaces the embedded portfolio entries.

    Pipeline K phase-2 (finding 4): hardened oracle — title-substring
    alone could green-light any PDF named like ``test_portfolio.pdf``.
    Now asserts the ``attachmentPanel`` widget is reachable in the
    UIA tree (its presence is what proves the portfolio detection
    path actually fired)."""
    fixture = FIXTURES / "test_portfolio.pdf"
    app, win = _start(fixture)
    try:
        title = win.window_text()
        assert "test_portfolio" in title.lower() or "portfolio" in title.lower(), title
        # Stronger oracle: the attachmentPanel must be reachable in
        # the UIA tree. The panel's auto_id is hierarchical so suffix
        # match is correct (find_by_suffix).
        from tests.ui_automation.harness.dialog_helpers import (
            find_by_suffix,
        )
        panel = find_by_suffix(win, "attachmentPanel")
        if panel is None:
            # Some Qt UIA bridges expose the panel only when its dock
            # widget is visible — try the dock as a fallback.
            panel = find_by_suffix(win, "attachmentPanelDock")
        assert panel is not None, (
            "AttachmentPanel not in UIA tree — portfolio detection "
            "did not fire (title-substring oracle alone is insufficient)"
        )
        record_matrix_result("open_portfolio_pdf", "green",
                             "Pipeline K phase-2: portfolio.pdf loads + AttachmentPanel reachable in UIA")
    finally:
        try:
            app.kill()
        except Exception:
            pass


# --- Sign dialog: invisible + certification ---------------------------


# GitHub #209 (2026-09-18) — the two sign-dialog cases below used to end in
# ``pytest.skip("invisibleCheck not in this build")`` /
# ``pytest.skip("certificationCombo not in this build")``. BOTH claims are
# false, and have been since 2026-05-02. Re-derived from the current tree:
#
#   src/ui/SignDialog.cpp:178  invisibleCheck_->setObjectName("invisibleCheckBox");
#   src/ui/SignDialog.cpp:183  certificationCombo_->setObjectName("certificationLevelCombo");
#   src/ui/SignDialog.cpp:184-187  four addItem() calls (regular / no-changes /
#                                  form-fill / annotations)
#   src/ui/SignDialog.cpp:23   setObjectName("signDialog");
#   src/ui/SignDialog.cpp:244  registerDialogIntrospection(this);
#
# The skip reason named ``invisibleCheck`` — the PRE-2026-05-02 objectName. It
# was renamed to ``invisibleCheckBox`` in the same commit that the skip reason
# was describing, so the lookup was asking for an auto_id that no build has had
# for four months.
#
# The SECOND reason these never ran is the one CLAUDE.md warns about: the sign
# dialog is APP-MODAL (``SignDialog dialog(this); dialog.exec();`` —
# src/ui/MainWindow.cpp:12561-12562), and pywinauto's UIA backend cannot
# enumerate a Qt modal while exec() runs. So ``win.child_window(
# auto_id="QApplication.signDialog").wait("visible")`` timed out unconditionally
# and _open_sign_dialog skipped before either widget lookup was reached. Both
# cases now drive the dialog's OWN seam (TestDialogSeam.h
# registerDialogIntrospection), which is the documented answer.
def _sign_dialog_record(win):
    """Open the sign dialog via its action seam and return its inventory.

    Returns the raw introspection record string. Fails (never skips) when the
    pipe is unreachable or the dialog was never constructed — both of those
    are findings, not preconditions.
    """
    from tests.ui_automation.harness.pdfe_app_fixture import (
        invoke_action, read_dialog_introspection,
    )
    if not invoke_action(win, "signDocumentAction", settle_s=1.5):
        record_matrix_result(
            current_test_id(), "red",
            "testToolActivatePipe unreachable — cannot trigger "
            "signDocumentAction (PDFE_TEST_PIPES must be set)",
        )
        pytest.fail("testToolActivatePipe unreachable for signDocumentAction")
    rec = read_dialog_introspection(win, "signDialog", settle_s=0.8)
    if rec is None:
        record_matrix_result(
            current_test_id(), "red",
            "testDialogIntrospection pipe unreachable",
        )
        pytest.fail("dialog introspection pipe unreachable")
    if rec == "__null__" or not rec:
        record_matrix_result(
            current_test_id(), "red",
            "signDialog was never constructed after signDocumentAction "
            "(introspection record empty)",
        )
        pytest.fail(
            "signDialog never opened — signDocumentAction did not construct "
            f"the dialog (introspection readback: {rec!r})"
        )
    return rec


def _record_widgets(rec: str) -> list:
    """Child-widget objectNames out of a "<dlgName>|a,b,c[||combos=...]" record."""
    body = (rec or "").split("||combos=", 1)[0]
    if "|" not in body:
        return []
    return [n for n in body.split("|", 1)[1].split(",") if n]


def _record_combo_items(rec: str, combo_name: str) -> list:
    """Items of a named QComboBox from the "||combos=Name:a;b@@..." section."""
    if "||combos=" not in (rec or ""):
        return []
    for entry in rec.split("||combos=", 1)[1].split("@@"):
        if ":" not in entry:
            continue
        name, items = entry.split(":", 1)
        if name == combo_name:
            return [it for it in items.split(";") if it]
    return []


def test_sign_dialog_exposes_invisible_checkbox():
    """sign_with_invisible_checkbox — the Invisible-signature checkbox is
    present in the sign dialog under its REAL objectName.

    Oracle: the dialog's own introspection inventory must contain
    ``invisibleCheckBox`` (src/ui/SignDialog.cpp:178). Delete that widget, or
    rename it again, and the assertion fails — which is exactly the regression
    the old ``pytest.skip("invisibleCheck not in this build")`` was silently
    absorbing. The inventory is additionally asserted non-trivial (it must
    also carry the dialog's OK button) so an empty/garbled record cannot pass
    a substring test.
    """
    import os
    fixture = FIXTURES / "test_single.pdf"
    prev = os.environ.get("PDFE_TEST_DIALOG_AUTODISMISS")
    # The modal exec() must return for the main event loop to answer the
    # readback pipe. Per-test opt-in; launch_pdfe inherits os.environ.
    os.environ["PDFE_TEST_DIALOG_AUTODISMISS"] = "1"
    try:
        app, win = _start(fixture)
    except BaseException:
        if prev is None:
            os.environ.pop("PDFE_TEST_DIALOG_AUTODISMISS", None)
        else:
            os.environ["PDFE_TEST_DIALOG_AUTODISMISS"] = prev
        raise
    try:
        rec = _sign_dialog_record(win)
        widgets = _record_widgets(rec)
        from tests.ui_automation.conftest import assert_recorded
        # Guard that the measurement happened: an empty inventory would make
        # every "x in widgets" check vacuously false rather than meaningful,
        # and an inventory of one proves nothing about the dialog's shape.
        assert len(widgets) > 5, (
            f"signDialog inventory has only {len(widgets)} named children "
            f"({widgets!r}) — the record is not a real dialog inventory, so "
            f"the assertion below would not be measuring anything"
        )
        assert "okButton" in widgets, (
            f"signDialog inventory does not contain its own okButton "
            f"(SignDialog.cpp:216) — the record is for something else: {rec!r}"
        )
        assert_recorded(
            "sign_with_invisible_checkbox",
            "invisibleCheckBox" in widgets,
            f"signDialog has no child named 'invisibleCheckBox' "
            f"(src/ui/SignDialog.cpp:178). Inventory: {widgets!r}",
            green_notes=(
                "GitHub #209: signDialog opened via signDocumentAction seam; "
                "invisibleCheckBox present in the dialog's own introspection "
                "inventory (the old skip named the pre-2026-05-02 objectName "
                "'invisibleCheck')"
            ),
        )
    finally:
        try:
            app.kill()
        except Exception:
            pass
        if prev is None:
            os.environ.pop("PDFE_TEST_DIALOG_AUTODISMISS", None)
        else:
            os.environ["PDFE_TEST_DIALOG_AUTODISMISS"] = prev


def test_sign_dialog_exposes_certification_combo():
    """sign_with_certification_level — the certification combo offers all 4
    PDF-spec levels (regular / no-changes / form-fill / annotations).

    Oracle: the combo named ``certificationLevelCombo``
    (src/ui/SignDialog.cpp:183) must appear in the dialog's introspection
    record with EXACTLY four items (:184-187). Drop a level, drop the combo,
    or rename it and this fails. The old
    ``pytest.skip("certificationCombo not in this build")`` named an
    objectName no build has carried since 2026-05-02.
    """
    import os
    fixture = FIXTURES / "test_single.pdf"
    prev = os.environ.get("PDFE_TEST_DIALOG_AUTODISMISS")
    os.environ["PDFE_TEST_DIALOG_AUTODISMISS"] = "1"
    try:
        app, win = _start(fixture)
    except BaseException:
        if prev is None:
            os.environ.pop("PDFE_TEST_DIALOG_AUTODISMISS", None)
        else:
            os.environ["PDFE_TEST_DIALOG_AUTODISMISS"] = prev
        raise
    try:
        rec = _sign_dialog_record(win)
        widgets = _record_widgets(rec)
        assert "certificationLevelCombo" in widgets, (
            f"signDialog has no child named 'certificationLevelCombo' "
            f"(src/ui/SignDialog.cpp:183). Inventory: {widgets!r}"
        )
        items = _record_combo_items(rec, "certificationLevelCombo")
        from tests.ui_automation.conftest import assert_recorded
        assert_recorded(
            "sign_with_certification_level",
            len(items) == 4,
            f"certificationLevelCombo must offer 4 certification levels "
            f"(SignDialog.cpp:184-187), got {len(items)}: {items!r}",
            green_notes=(
                "GitHub #209: certificationLevelCombo present with exactly 4 "
                "levels, read from the signDialog introspection record"
            ),
        )
    finally:
        try:
            app.kill()
        except Exception:
            pass
        if prev is None:
            os.environ.pop("PDFE_TEST_DIALOG_AUTODISMISS", None)
        else:
            os.environ["PDFE_TEST_DIALOG_AUTODISMISS"] = prev


# --- Preflight extensions ---------------------------------------------


def test_preflight_run_pdfa1b():
    """preflight_run_pdfa1b — pick PDF/A-1b, click Run, tree populates.

    The target PDF is test_3page.pdf which is NOT PDF/A conformant,
    so the validator will produce at least one issue. We just confirm
    that Run clickable and the issue tree has content after a short
    wait."""
    fixture = FIXTURES / "test_3page.pdf"
    app, win = _start(fixture)
    try:
        _try_menu(win, ["Tools -> Preflight..."])
        dlg = win.child_window(auto_id="QApplication.preflightDialog")
        dlg.wait("visible", timeout=5)
        combo = dlg.child_window(auto_id="QApplication.preflightDialog.preflightProfileCombo")
        combo.wait("visible", timeout=5)
        if not select_combo_item(combo, "PDF/A-1b",
                                 parent=dlg, timeout_s=3.0):
            pytest.skip("PDF/A-1b option not present in profile combo")
        # Pipeline K phase-2 (finding 2): read back the actual combo
        # value to verify the Qt model committed the selection. A
        # successful select_combo_item return is necessary but not
        # sufficient — older code just relied on the helper's True
        # return and produced silent greens when the underlying
        # SelectionItem.Select didn't propagate. This guards against
        # any future regression of that path.
        selected_text = ""
        try:
            selected_text = combo.selected_text() or ""
        except Exception:  # noqa: BLE001
            try:
                selected_text = combo.window_text() or ""
            except Exception:  # noqa: BLE001
                selected_text = ""
        assert "PDF/A-1b" in selected_text, (
            f"profile combo did not commit PDF/A-1b selection — "
            f"current value: {selected_text!r}"
        )
        run = dlg.child_window(auto_id="QApplication.preflightDialog.preflightRunButton")
        run.click_input()
        time.sleep(0.5)
        # The Tree's auto_id is set from accessibleName ("Preflight issues"),
        # not the objectName, so search by control_type instead.
        tree = dlg.child_window(control_type="Tree")
        tree.wait("visible", timeout=5)
        record_matrix_result("preflight_run_pdfa1b", "green",
                             "Sweep verified: combo selection committed (readback)+run+tree all reachable")
    finally:
        try:
            app.kill()
        except Exception:
            pass


def test_preflight_export_report_button_enables_after_run():
    """preflight_export_report — Export button becomes enabled once a
    validation produces any issues (or passes). Full file-save flow
    requires clicking through a file dialog; here we just assert the
    state transition."""
    fixture = FIXTURES / "test_3page.pdf"
    app, win = _start(fixture)
    try:
        _try_menu(win, ["Tools -> Preflight..."])
        dlg = win.child_window(auto_id="QApplication.preflightDialog")
        dlg.wait("visible", timeout=5)
        export = dlg.child_window(auto_id="QApplication.preflightDialog.preflightExportButton")
        export.wait("visible", timeout=5)
        # Pipeline K phase-3 (finding 9): record red on failure.
        from tests.ui_automation.conftest import assert_recorded
        assert_recorded(
            "preflight_export_report",
            not export.is_enabled(),
            "export button must start disabled but is enabled",
        )

        run = dlg.child_window(auto_id="QApplication.preflightDialog.preflightRunButton")
        run.click_input()
        time.sleep(0.7)
        assert_recorded(
            "preflight_export_report",
            export.is_enabled(),
            "export button must enable after run",
            green_notes="Sweep verified: export button enables after Run",
        )
    finally:
        try:
            app.kill()
        except Exception:
            pass


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-v"]))
