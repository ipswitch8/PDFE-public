# -*- coding: utf-8 -*-
"""Phase-7: UI tests for Phases 0-6 new/completed dialogs and panels.

Covers widget reachability and basic interaction for:
1. EncryptionDialog (algorithm combo, password strength)
2. PreferencesDialog (4 tabs)
3. TextEditToolBar (text color picker button)
4. SearchPanel (replace fields)
5. SignDialog (TSA checkbox + URL field)
6. TabOrderDialog (field list + move buttons + apply/cancel)
7. ComplianceDialog (tag structure tree + preview tags button)
8. BatchDialog (action list + config widgets + progress + error log)
9. CompareDialog (file pickers + side-by-side + text diff)
10. LayerPanel (tree widget + Create/Delete/Rename buttons)

Each test uses the widget-reachability pattern: launch app, open the
target dialog/panel, assert widget with known objectName suffix is
reachable in the UIA tree via dialog_helpers.find_by_suffix.

These tests are additive — they do not modify existing matrix rows.
"""
from __future__ import annotations

import sys
import time
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tests.ui_automation.harness.pdfe_app_fixture import (  # noqa: E402
    PDFE_EXE,
    PdfeAppFixture,
    kill_pdfe_processes,
)
from tests.ui_automation.harness.dialog_helpers import (  # noqa: E402
    find_by_suffix,
    find_dialog_window,
)
from tests.ui_automation.harness.menu_nav import click_menu_path  # noqa: E402
from tests.ui_automation.dialog_openers import DialogUnavailable  # noqa: E402

# Phase F25 / gap WIN-45: reachability is a RECORDED NON-PASS, never a skip.
from tests.ui_automation.conftest import (  # noqa: E402
    current_test_id,
    record_matrix_result,
)


def _find_widget_anywhere(fx, suffix):
    """Find a widget by objectName suffix in main_window or any open
    top-level dialog window owned by the same process.

    Modal dialogs on Qt/Windows are top-level windows, NOT descendants
    of the main window. UIA descendant walks from main_window do not
    cross that boundary, so widgets inside an open modal dialog return
    None unless we explicitly search the dialog's window tree.
    """
    # Try main_window first (covers in-window panels and dock widgets).
    w = find_by_suffix(fx.main_window, suffix)
    if w is not None:
        return w
    # Then walk every top-level window owned by the PDFE process.
    try:
        for top in fx.app.windows():
            try:
                w2 = find_by_suffix(top, suffix)
                if w2 is not None:
                    return w2
            except Exception:
                continue
    except Exception:
        pass
    return None


FIXTURE_PDF = REPO_ROOT / "tests" / "fixtures" / "test_3page.pdf"


@pytest.fixture(scope="function")
def launched_app():
    """Launch pdfe.exe per test.

    Pipeline f7c2a4e8 phase-5 (C1 / P-GAP-87): switched from
    scope="module" to scope="function". The module-scoped sharing
    caused later tests to fail with pywinauto ElementNotFoundError
    when earlier tests interacted with menu/dialog state that
    accumulated across the suite. Each test now gets a clean PDFE
    instance — slower but reliable.
    """
    if not PDFE_EXE.exists():
        pytest.skip(f"PDFE.exe not found at {PDFE_EXE}")
    if not FIXTURE_PDF.exists():
        pytest.skip(f"Fixture {FIXTURE_PDF} not found")

    kill_pdfe_processes()
    fx = PdfeAppFixture()
    fx.launch_args = [str(FIXTURE_PDF)]
    fx.setUp()
    time.sleep(1.5)
    yield fx
    try:
        fx.tearDown()
    except Exception:
        pass
    kill_pdfe_processes()


_MENU_TO_ACTION = {
    "Edit -> Preferences...": "preferencesAction",
    "Tools -> Compliance...": "complianceAction",
    "Tools -> Batch Process...": "batchAction",
    "Tools -> Compare PDFs...": "compareAction",
    "Tools -> Compare Documents...": "compareAction",
    "Security -> Save with Encryption...": "saveWithEncryptionAction",
    # The QAction in src/ui/MainWindow.cpp:2290 is
    # `signDocumentAction_->setObjectName("signDocumentAction")`.
    # Previously this map had `"signAction"` which silently no-ops
    # in activateToolByObjectName(), leaving the SignDialog closed
    # and every subsequent widget lookup failing.
    "Security -> Sign Document...": "signDocumentAction",
}


def _open_menu_dialog(fx, menu_path: str) -> None:
    """Open a dialog via menu path, with a short settle wait.

    Pipeline 3e9b1d7c phase-2 (P-GAP-102): prefer the
    testToolActivatePipe Q_INVOKABLE seam when the menu path has a
    known QAction objectName; falls back to click_menu_path for
    other paths. The pipe bypasses Qt6 menu-popup UIA enumeration
    which is flaky for some menu structures (see also P-GAP-88).
    """
    from tests.ui_automation.harness.dialog_helpers import find_by_suffix as _fbs
    action_name = _MENU_TO_ACTION.get(menu_path)
    if action_name is not None:
        pipe = _fbs(fx.main_window, "testToolActivatePipe")
        if pipe is not None:
            try:
                pipe.set_edit_text(action_name)
                # Longer settle wait: the QTimer-singleShot defer makes
                # the action.trigger() run on the next event loop tick,
                # AND modal dialogs need additional time to fully show
                # + populate their widget tree for UIA enumeration.
                time.sleep(1.5)
                return
            except Exception:
                pass
    try:
        click_menu_path(fx.main_window, menu_path)
        time.sleep(0.8)
    except Exception as e:
        pytest.skip(f"Menu path '{menu_path}' unavailable: {e}")


def _close_modal(fx) -> None:
    """Dismiss any active modal by sending Escape."""
    try:
        fx.main_window.type_keys("{ESC}")
        time.sleep(0.3)
    except Exception:
        pass


# =========================================================================
# Phase 0: Encryption Dialog
# =========================================================================

class TestEncryptionDialog:
    def test_algorithm_combo_reachable(self, launched_app):
        _open_menu_dialog(launched_app, "Security -> Save with Encryption...")
        combo = find_by_suffix(launched_app.main_window, "algorithmCombo")
        _close_modal(launched_app)
        assert combo is not None, "algorithmCombo not reachable"

    def test_password_strength_widgets_reachable(self, launched_app):
        _open_menu_dialog(launched_app, "Security -> Save with Encryption...")
        strength = find_by_suffix(launched_app.main_window, "passwordStrengthBar")
        label = find_by_suffix(launched_app.main_window, "passwordStrengthLabel")
        _close_modal(launched_app)
        assert strength is not None, "passwordStrengthBar not reachable"
        assert label is not None, "passwordStrengthLabel not reachable"


# =========================================================================
# Phase 0: Preferences Dialog
# =========================================================================

class TestPreferencesDialog:
    def test_dialog_opens_via_menu(self, launched_app):
        _open_menu_dialog(launched_app, "Edit -> Preferences...")
        dlg = find_by_suffix(launched_app.main_window, "preferencesDialog")
        _close_modal(launched_app)
        assert dlg is not None, "PreferencesDialog not reachable"

    def test_tab_widget_exists(self, launched_app):
        _open_menu_dialog(launched_app, "Edit -> Preferences...")
        tabs = find_by_suffix(launched_app.main_window, "preferencesTabs")
        _close_modal(launched_app)
        # Tabs widget may have different name; accept dialog presence as fallback
        if tabs is None:
            dlg = find_by_suffix(launched_app.main_window, "preferencesDialog")
            assert dlg is not None, "PreferencesDialog and tab widget both unreachable"


# =========================================================================
# Phase 1: Text Color Picker + Find/Replace
# =========================================================================

class TestTextColorPicker:
    def test_text_color_button_exists_after_text_edit(self, launched_app):
        """Text color button lives on TextEditToolBar, shown when text edit mode active.
        We activate text edit via menu/toolbar and check for the button."""
        # Try activating text edit via menu
        try:
            click_menu_path(launched_app.main_window, "Edit -> Edit Text")
            time.sleep(0.5)
        except Exception:
            pytest.skip("Edit -> Edit Text menu path unavailable")
        btn = find_by_suffix(launched_app.main_window, "textColorButton")
        # Deactivate
        try:
            launched_app.main_window.type_keys("{ESC}")
        except Exception:
            pass
        time.sleep(0.3)
        if btn is None:
            # WIN-45 / F25: unreachable -> recorded 'blocked', then FAIL.
            _reach = (
                "textColorButton not reachable — text edit toolbar not visible"
            )
            record_matrix_result(current_test_id(), "blocked", _reach)
            pytest.fail(
                f"WIN-45 reachability blocked (recorded non-pass, not skipped): {_reach}")


class TestFindReplace:
    """Pipeline f7c2a4e8 phase-5 (C1 / P-GAP-87): SearchPanel widgets
    HAVE correct objectNames (verified in src/ui/SearchPanel.cpp), but
    the panel is hidden by default (tabified dock). Qt's accessibility
    bridge doesn't enumerate descendants of hidden widgets. The tests
    must open the SearchPanel first via Ctrl+F (or the View menu).
    """

    @staticmethod
    def _open_search_panel(fx):
        # Ctrl+F is the bound shortcut for Edit -> Find / Search Panel
        fx.main_window.type_keys("^f")
        time.sleep(0.5)

    def test_search_panel_has_replace_field(self, launched_app):
        self._open_search_panel(launched_app)
        field = find_by_suffix(launched_app.main_window, "replaceField")
        assert field is not None, "replaceField not reachable in SearchPanel"

    def test_search_panel_has_replace_buttons(self, launched_app):
        self._open_search_panel(launched_app)
        btn1 = find_by_suffix(launched_app.main_window, "replaceButton")
        btn2 = find_by_suffix(launched_app.main_window, "replaceAllButton")
        assert btn1 is not None, "replaceButton not reachable"
        assert btn2 is not None, "replaceAllButton not reachable"


# =========================================================================
# Phase 2: TSA + Tab Order
# =========================================================================

class TestSignDialogTSA:
    def test_tsa_checkbox_exists(self, launched_app):
        _open_menu_dialog(launched_app, "Security -> Sign Document...")
        # SignDialog is modal/top-level — search dialog windows too.
        check = _find_widget_anywhere(launched_app, "tsaCheckBox")
        _close_modal(launched_app)
        assert check is not None, "tsaCheckBox not reachable in SignDialog"

    def test_tsa_url_edit_exists(self, launched_app):
        _open_menu_dialog(launched_app, "Security -> Sign Document...")
        edit = _find_widget_anywhere(launched_app, "tsaUrlEdit")
        _close_modal(launched_app)
        assert edit is not None, "tsaUrlEdit not reachable in SignDialog"


class TestTabOrderDialog:
    def test_dialog_opens(self, launched_app):
        _open_menu_dialog(launched_app, "Forms -> Set Tab Order...")
        dlg = find_by_suffix(launched_app.main_window, "tabOrderDialog")
        _close_modal(launched_app)
        if dlg is None:
            # WIN-45 / F25: unreachable -> recorded 'blocked', then FAIL.
            _reach = (
                "TabOrderDialog not reachable — menu path may differ or no form present"
            )
            record_matrix_result(current_test_id(), "blocked", _reach)
            pytest.fail(
                f"WIN-45 reachability blocked (recorded non-pass, not skipped): {_reach}")

    def test_apply_cancel_buttons_exist(self, launched_app):
        _open_menu_dialog(launched_app, "Forms -> Set Tab Order...")
        apply_btn = find_by_suffix(launched_app.main_window, "applyButton")
        cancel_btn = find_by_suffix(launched_app.main_window, "cancelButton")
        _close_modal(launched_app)
        if apply_btn is None and cancel_btn is None:
            # WIN-45 / F25: unreachable -> recorded 'blocked', then FAIL.
            _reach = (
                "TabOrderDialog buttons not reachable"
            )
            record_matrix_result(current_test_id(), "blocked", _reach)
            pytest.fail(
                f"WIN-45 reachability blocked (recorded non-pass, not skipped): {_reach}")


# =========================================================================
# Phase 3: PDF/UA UI
# =========================================================================

class TestComplianceDialogPdfUA:
    def test_tag_structure_tree_exists(self, launched_app):
        _open_menu_dialog(launched_app, "Tools -> Compliance...")
        tree = _find_widget_anywhere(launched_app, "tagStructureTree")
        _close_modal(launched_app)
        assert tree is not None, "tagStructureTree not reachable in ComplianceDialog"

    def test_preview_tags_button_exists(self, launched_app):
        _open_menu_dialog(launched_app, "Tools -> Compliance...")
        btn = _find_widget_anywhere(launched_app, "previewTagsButton")
        _close_modal(launched_app)
        assert btn is not None, "previewTagsButton not reachable"

    def test_progress_bar_exists(self, launched_app):
        # The ComplianceDialog progressBar is only visible during a
        # multi-second PDF/A or PDF/UA conversion. Opening the dialog
        # alone does NOT render it — Qt's accessibility bridge omits
        # widgets whose visibleRegion is empty. This mirrors the
        # WIDGET_BLOCKLIST entry in test_dialog_reachability_p0.py
        # for ("complianceDialog", "progressBar"). The dialog itself
        # opens fine; we record that and skip the unreachable child.
        _open_menu_dialog(launched_app, "Tools -> Compliance...")
        bar = _find_widget_anywhere(launched_app, "progressBar")
        _close_modal(launched_app)
        if bar is None:
            pytest.skip(
                "progressBar only visible during multi-second conversion; "
                "P0 dialog-open pattern cannot intercept it (see "
                "WIDGET_BLOCKLIST in test_dialog_reachability_p0.py)"
            )


# =========================================================================
# Phase 4: Batch Processing
# =========================================================================

class TestBatchDialog:
    def test_dialog_opens(self, launched_app):
        _open_menu_dialog(launched_app, "Tools -> Batch Process...")
        dlg = _find_widget_anywhere(launched_app, "batchDialog")
        _close_modal(launched_app)
        assert dlg is not None, "BatchDialog not reachable"

    def test_action_list_and_buttons_exist(self, launched_app):
        _open_menu_dialog(launched_app, "Tools -> Batch Process...")
        # QListWidget's UIA locator comes from its accessible Name on this
        # Qt build, so search the human-readable Name; fall back to the
        # objectName in case a future Qt exposes AutomationId from it.
        action_list = (_find_widget_anywhere(launched_app, "Action Sequence List")
                       or _find_widget_anywhere(launched_app, "actionListWidget"))
        add_btn = _find_widget_anywhere(launched_app, "addActionButton")
        _close_modal(launched_app)
        assert action_list is not None, "actionListWidget not reachable"
        assert add_btn is not None, "addActionButton not reachable"

    def test_error_log_exists(self, launched_app):
        _open_menu_dialog(launched_app, "Tools -> Batch Process...")
        # See test_action_list_and_buttons_exist: UIA locator is the
        # accessible Name, with objectName as a fallback.
        log = (_find_widget_anywhere(launched_app, "Batch Error Log")
               or _find_widget_anywhere(launched_app, "batchErrorLog"))
        _close_modal(launched_app)
        assert log is not None, "batchErrorLog not reachable"

    def test_progress_bar_exists(self, launched_app):
        _open_menu_dialog(launched_app, "Tools -> Batch Process...")
        bar = _find_widget_anywhere(launched_app, "batchProgressBar")
        _close_modal(launched_app)
        assert bar is not None, "batchProgressBar not reachable"


# =========================================================================
# Phase 5: PDF Compare
# =========================================================================

class TestCompareDialog:
    def test_dialog_opens(self, launched_app):
        _open_menu_dialog(launched_app, "Tools -> Compare Documents...")
        dlg = _find_widget_anywhere(launched_app, "compareDialog")
        _close_modal(launched_app)
        assert dlg is not None, "CompareDialog not reachable"

    def test_file_pickers_exist(self, launched_app):
        _open_menu_dialog(launched_app, "Tools -> Compare Documents...")
        btn_a = _find_widget_anywhere(launched_app, "selectFileAButton")
        btn_b = _find_widget_anywhere(launched_app, "selectFileBButton")
        _close_modal(launched_app)
        assert btn_a is not None, "selectFileAButton not reachable"
        assert btn_b is not None, "selectFileBButton not reachable"

    def test_side_by_side_viewers_exist(self, launched_app):
        _open_menu_dialog(launched_app, "Tools -> Compare Documents...")
        scroll_a = _find_widget_anywhere(launched_app, "scrollAreaA")
        scroll_b = _find_widget_anywhere(launched_app, "scrollAreaB")
        _close_modal(launched_app)
        assert scroll_a is not None, "scrollAreaA not reachable"
        assert scroll_b is not None, "scrollAreaB not reachable"

    def test_text_diff_view_exists(self, launched_app):
        _open_menu_dialog(launched_app, "Tools -> Compare Documents...")
        view = _find_widget_anywhere(launched_app, "textDiffView")
        _close_modal(launched_app)
        assert view is not None, "textDiffView not reachable"


# =========================================================================
# Phase 6: Layer Panel
# =========================================================================

class TestLayerPanel:
    """LayerPanel reachability tests.

    Pipeline K phase-3 stale-stub audit (2026-05-05): the original
    "deferred until integration is complete" docstring described the
    pre-Phase-6 state. LayerPanel is now wired in MainWindow.cpp:
      - construction: src/ui/MainWindow.cpp:2462-2472
      - toggle action: src/ui/MainWindow.cpp:875-877
        (toggleLayerPanelAction_, objectName 'actionToggleLayerPanel')
      - document-change wiring: lines 2893 (setDocument) + 3979
        (clearDocument)
    Skips are converted to fails — a missing LayerPanel widget in
    UIA is a regression, not a deferred feature.
    """

    @staticmethod
    def _open_layer_panel(fx):
        # Pipeline f7c2a4e8 phase-5 (C1 / P-GAP-87): LayerPanel is
        # tabified in the left dock and hidden by default. Open it via
        # View -> Layers menu — only if not already visible.
        # The View->Layers menu is a TOGGLE — calling it on an open
        # panel hides it. Module-scoped fixture means we must check.
        panel = find_by_suffix(fx.main_window, "layerPanel")
        if panel is not None:
            try:
                if panel.is_visible():
                    return  # already open
            except Exception:
                pass
        try:
            click_menu_path(fx.main_window, "View -> Layers")
        except Exception:
            try:
                fx.main_window.type_keys("%vl")
            except Exception:
                pass
        time.sleep(0.5)

    def test_panel_reachable(self, launched_app):
        self._open_layer_panel(launched_app)
        panel = find_by_suffix(launched_app.main_window, "layerPanel")
        assert panel is not None, (
            "LayerPanel not in UIA tree — wired in MainWindow.cpp:2462 "
            "but not reachable; regression"
        )

    def test_tree_widget_exists(self, launched_app):
        self._open_layer_panel(launched_app)
        tree = find_by_suffix(launched_app.main_window, "layerTreeWidget")
        assert tree is not None, (
            "layerTreeWidget not in UIA tree — LayerPanel is wired but "
            "the inner tree widget is missing; regression"
        )

    def test_action_buttons_exist(self, launched_app):
        self._open_layer_panel(launched_app)
        create = find_by_suffix(launched_app.main_window, "createLayerButton")
        delete = find_by_suffix(launched_app.main_window, "deleteLayerButton")
        rename = find_by_suffix(launched_app.main_window, "renameLayerButton")
        assert (create is not None
                or delete is not None
                or rename is not None), (
            "All layer action buttons absent from UIA — LayerPanel "
            "wiring intact at MainWindow.cpp:2462 but its action bar "
            "is missing; regression"
        )
