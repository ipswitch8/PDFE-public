# -*- coding: utf-8 -*-
"""Per-dialog openers for phase-3 widget reachability tests.

Each opener is a callable ``(fx: PdfeAppFixture) -> None`` that
leaves the target dialog (or dock panel) visible on screen. After
the opener returns, the test walks ``fx.main_window`` for the widget
whose auto_id suffix the matrix row specifies.

Dialogs that cannot be opened in the current build raise
``DialogUnavailable`` — the per-row test records ``blocked`` without
counting it as a failure.
"""
from __future__ import annotations

import time
from typing import Callable, Dict

from tests.ui_automation.harness.menu_nav import (
    click_menu_path,
    open_top_menu,
)
from tests.ui_automation.harness.dialog_helpers import find_by_suffix


class DialogUnavailable(Exception):
    """Raise when a dialog cannot be opened in the current build."""


def _menu_opener(path: str) -> Callable:
    def _open(fx) -> None:
        click_menu_path(fx.main_window, path)
        time.sleep(0.6)
    return _open


def _noop_already_visible(fx) -> None:
    """Dock panels whose widgets are reachable from the main window
    tree without any explicit open step (panel exists in the widget
    hierarchy even when the dock is collapsed)."""
    time.sleep(0.1)


def _search_panel_with_sample_opener(fx) -> None:
    """Open searchPanel and dispatch a sample search so resultsList is
    populated.

    Phase-3 of pipeline c94e2f17 (GAP-64): the resultsListWidget at
    src/ui/SearchPanel.cpp:110 has setObjectName("resultsList") but
    Qt6's UIA bridge omits empty QListWidget descendants from the
    enumeration. The opener populates the list by typing a sample
    search term ("the" — common across English-language fixtures) and
    pressing Enter, which triggers SearchPanel::onSearch.
    """
    suffix = "."
    # Find searchEdit and set its text.
    edit = None
    for d in fx.main_window.descendants():
        try:
            aid = d.element_info.automation_id or ""
        except Exception:  # noqa: BLE001
            continue
        if aid.endswith(".searchEdit") or aid == "searchEdit":
            edit = d
            break
    if edit is None:
        return  # let the row record red via downstream search
    try:
        for m in ("set_edit_text", "set_value"):
            try:
                getattr(edit, m)("Page")  # word present on all 3 pages
                break
            except Exception:  # noqa: BLE001
                continue
        time.sleep(0.2)
        # Click the searchButton — more reliable than returnPressed on
        # a QLineEdit that may not have keyboard focus from set_edit_text.
        for d in fx.main_window.descendants():
            try:
                aid = d.element_info.automation_id or ""
            except Exception:  # noqa: BLE001
                continue
            if aid.endswith(".searchButton") or aid == "searchButton":
                try:
                    d.click_input()
                except Exception:  # noqa: BLE001
                    pass
                break
        time.sleep(1.5)  # let SearchPanel populate the list
    except Exception:  # noqa: BLE001
        pass


def _annotation_properties_dock_opener(fx) -> None:
    """Open the Annotation Properties dock by selecting the FreeText
    annotation in the test_with_freetext_annot.pdf fixture.

    Phase-2 of pipeline c94e2f17 (GAP-62): annotationPropertiesDock is
    hidden by default (it's tabified with other docks and only
    shown when an annotation is selected, per the
    annotationPropertiesDock_->show()/raise() calls at
    src/ui/MainWindow.cpp:451-455, 5141, 6661).

    The opener:
      1. Activates SelectTool via testToolActivatePipe.
      2. Sends a click_by_pdf_coord at (300, 350) — the center of the
         FreeText annotation in test_with_freetext_annot.pdf.
      3. Verifies annotationPropertiesDock has on-screen coords.

    Subsequent annotFontCombo/annotFontGroup/annotFontSizeSpinner
    widgets become enumerable in UIA because setAnnotation() with
    a FreeText AnnotationData makes fontGroup_->setVisible(true)
    (src/ui/AnnotationPropertiesWidget.cpp:182).
    """
    def _find_pipe(aid: str):
        for d in fx.main_window.descendants():
            try:
                a = d.element_info.automation_id or ""
            except Exception:  # noqa: BLE001
                continue
            if a.endswith("." + aid) or a == aid:
                return d
        return None

    # Step 1: activate SelectTool. MainWindow::activateToolByObjectName
    # at src/ui/MainWindow.cpp:4613 has a guard that SKIPS trigger() if
    # the action is already checked (to prevent unchecking checked
    # exclusive actions). selectToolAction_ is setChecked(true) at
    # construction — so a direct pipe write of "selectToolAction" is a
    # no-op and currentTool_ stays null. Workaround: fire a different
    # tool first to unchecked-state selectToolAction, then re-fire
    # selectToolAction which now goes through trigger() and properly
    # invokes onAnnotationToolSelected → setCurrentTool(SelectTool).
    pipe_act = _find_pipe("testToolActivatePipe")
    if pipe_act is None:
        raise DialogUnavailable(
            "testToolActivatePipe not reachable; cannot open "
            "annotation properties via SelectTool"
        )
    def _pipe_write(name: str):
        for m in ("set_edit_text", "set_value"):
            try:
                getattr(pipe_act, m)(name)
                return True
            except Exception:  # noqa: BLE001
                continue
        return False
    # Toggle: fire stickyNote first (unchecks selectTool), then fire
    # selectTool which now goes through trigger() path.
    _pipe_write("stickyNoteToolAction")
    time.sleep(0.4)
    _pipe_write("selectToolAction")
    time.sleep(0.4)

    # Step 2: click on the FreeText annotation via the click pipe.
    pipe_click = _find_pipe("testClickByPdfCoordPipe")
    if pipe_click is None:
        raise DialogUnavailable(
            "testClickByPdfCoordPipe not reachable; cannot click "
            "annotation to trigger properties dock show"
        )
    for m in ("set_edit_text", "set_value"):
        try:
            # FreeText annotation in test_with_freetext_annot.pdf is at
            # PDF rect (200,300)-(400,400); click at center (300, 350).
            getattr(pipe_click, m)("0,300.0,350.0")
            time.sleep(0.8)
            break
        except Exception:  # noqa: BLE001
            continue

    # Step 3: verify dock is on-screen.
    dock = _find_pipe("annotationPropertiesDock")
    if dock is not None:
        try:
            r = dock.rectangle()
            if r.left >= 0 and r.right > r.left:
                return  # dock visible
        except Exception:  # noqa: BLE001
            pass
    # Dock didn't appear — let caller record red but don't raise
    # DialogUnavailable (the opener "tried"); the widget search
    # downstream may still find specific widgets via UIA descendants.


def _ensure_dock_visible_via_pipe(action_name: str, panel_widget_name: str) -> Callable:
    """Open the named dock by firing the toggle QAction through the
    testToolActivatePipe (Q_INVOKABLE seam), then verify the dock's
    primary widget has non-negative on-screen coordinates.

    The toggle action is a checkbox-style QAction whose `triggered`
    handler shows the dock. Firing it twice would toggle off; we
    detect the current state by checking the panel widget's rect
    sign — Qt returns negative geometry for hidden widgets.

    Phase-D Pipeline b3e17a04: needed because some docks (notably
    bookmarkPanel) are hidden by default on first launch on this
    rig regardless of the toggle action's checked state — the
    QSettings persistence may have a stale value, or the dock area
    layout may push it off-screen until the toggle is explicitly
    triggered.
    """
    def _open(fx) -> None:
        # Check current visibility via the panel widget's rect.
        panel = find_by_suffix(fx.main_window, panel_widget_name)
        if panel is not None:
            try:
                r = panel.rectangle()
                if r.left >= 0 and r.top >= 0 and r.right > r.left:
                    # Already on-screen.
                    return
            except Exception:  # noqa: BLE001
                pass
        # Fire the toggle via the test pipe. The pipe is a zero-size
        # hidden QLineEdit — find_by_suffix(descendants()) doesn't reach
        # it on this rig. Walk descendants directly with a control_type
        # filter relaxed (Edit) and match by full auto_id.
        pipe = None
        try:
            for d in fx.main_window.descendants():
                try:
                    aid = d.element_info.automation_id or ""
                except Exception:  # noqa: BLE001
                    continue
                if aid.endswith(".testToolActivatePipe") or aid == "testToolActivatePipe":
                    pipe = d
                    break
        except Exception:  # noqa: BLE001
            pass
        if pipe is None:
            raise DialogUnavailable(
                f"testToolActivatePipe not reachable; cannot ensure "
                f"{action_name} dock visibility"
            )
        for method in ("set_edit_text", "set_value"):
            try:
                getattr(pipe, method)(action_name)
                time.sleep(0.6)
                break
            except Exception:  # noqa: BLE001
                continue
        # Re-verify visibility.
        panel = find_by_suffix(fx.main_window, panel_widget_name)
        if panel is None:
            raise DialogUnavailable(
                f"{panel_widget_name} unreachable after firing {action_name}"
            )
        try:
            r = panel.rectangle()
            if r.right <= r.left or r.left < 0:
                raise DialogUnavailable(
                    f"{panel_widget_name} still hidden after firing "
                    f"{action_name} (rect={r})"
                )
        except DialogUnavailable:
            raise
        except Exception:  # noqa: BLE001
            pass
    return _open


def _via_view_menu(leaf: str) -> Callable:
    def _open(fx) -> None:
        click_menu_path(fx.main_window, f"View -> {leaf}")
        time.sleep(0.4)
    return _open


def _blocked(reason: str) -> Callable:
    def _open(fx) -> None:
        raise DialogUnavailable(reason)
    return _open


def _form_field_select(fx) -> None:
    """Activate FormTool and Tab to the first form field to populate
    the formFieldPropertiesPanel stacked page.
    Requires test_form.pdf loaded via DIALOG_FIXTURES override."""
    btn = find_by_suffix(fx.main_window, "formToolAction")
    if btn is None:
        raise DialogUnavailable("formToolAction not found in toolbar")
    try:
        btn.click_input()
    except Exception as e:
        raise DialogUnavailable(f"formToolAction click failed: {e}")
    time.sleep(0.5)
    # Tab to the first form field — FormTool handles Tab key
    try:
        fx.main_window.type_keys("{TAB}")
    except Exception:
        pass
    time.sleep(0.8)


def _image_select(fx) -> None:
    """Activate ImageTool — but image selection requires canvas click
    which is blocked on the canvas-click harness gap (Phase 4)."""
    raise DialogUnavailable(
        "imagePropertiesPanel requires image selection via canvas click; "
        "canvas-click harness gap blocks this (Phase 4 dependency)"
    )


DIALOG_OPENERS: Dict[str, Callable] = {
    # --- modal dialogs reachable via the menu bar ---
    "aboutDialog": _menu_opener("Help -> About PDFE"),
    "metadataDialog": _menu_opener("File -> Document Properties..."),
    "printDialog": _menu_opener("File -> Print..."),
    "printPreviewDialog": _menu_opener("File -> Print Preview..."),
    "encryptionDialog": _menu_opener("Security -> Save with Encryption..."),
    "signDialog": _menu_opener("Security -> Sign Document..."),
    "complianceDialog": _menu_opener("Tools -> Compliance..."),
    "preferencesDialog": _menu_opener("Edit -> Preferences..."),
    "tabOrderDialog": _menu_opener("Forms -> Set Tab Order..."),
    "batchDialog": _menu_opener("Tools -> Batch Process..."),
    "compareDialog": _menu_opener("Tools -> Compare Documents..."),

    # --- dock panels that are visible by default on first launch ---
    "pagePanel": _noop_already_visible,
    # Phase-4.5 c94e2f17 (GAP-61 Q_INVOKABLE seam): bookmarkPanel
    # actions are now discoverable via testContextActionsQueryPipe
    # regardless of dock visibility. The bookmarkTree widget itself
    # is enumerated by UIA even when at negative coords (Qt accessibility
    # still surfaces hidden-dock children for QTreeWidget). So _noop
    # is now the right opener — no need to force-show the dock.
    "bookmarkPanel": _noop_already_visible,
    # Phase-3 of c94e2f17 (GAP-64): dispatch a sample search so
    # resultsList has items (Qt6 UIA omits empty QListWidgets).
    "searchPanel": _search_panel_with_sample_opener,

    # --- Properties Panel pages ---
    # Phase-2 of c94e2f17 (GAP-62): annotationPropertiesDock is shown
    # only when an annotation is selected. Opener does SelectTool +
    # click-by-pdf-coord on the FreeText annotation in the fixture.
    "annotationPropertiesWidget": _annotation_properties_dock_opener,
    "formFieldPropertiesPanel": _form_field_select,
    "imagePropertiesPanel": _image_select,

    # --- panels shown automatically when an annotation is created ---
    "commentPanel": _noop_already_visible,
    "defaultsLogPanel": _noop_already_visible,

    # --- panels shown by activating their feature via the menu ---
    "redactionPanel": _menu_opener("Security -> Mark Redaction"),
    "signaturePanel": _menu_opener("Security -> Verify Signatures"),

    # --- dialogs reached by preloading a special fixture ---
    "passwordDialog": _noop_already_visible,

    "atlasComparisonDialog": _blocked(
        "atlasComparisonDialog requires active image-based text edit "
        "workflow (TRE span click → atlas build → comparison dialog); "
        "permanent-closure: deep workflow not reachable in P0 dialog test"),
}


# Per-dialog fixture override. If a dialog name appears here, the
# _DialogCache launches pdfe with this fixture path (relative to
# tests/fixtures/) instead of the default test_3page.pdf.
DIALOG_FIXTURES: Dict[str, str] = {
    "passwordDialog": "test_encrypted_aes256.pdf",
    "formFieldPropertiesPanel": "test_form.pdf",
    "imagePropertiesPanel": "test_pdf_with_images.pdf",
    # Phase-C Pipeline 7d4b2f91: panel-state fixtures so context-menu
    # tests have actual content to right-click on.
    "bookmarkPanel": "test_with_bookmarks.pdf",
    "commentPanel": "phase3_annotations_all_types.pdf",
    # Phase-2 of c94e2f17 (GAP-62): annotation selected to populate
    # the annotationPropertiesWidget stacked-page font controls.
    "annotationPropertiesWidget": "test_with_freetext_annot.pdf",
}


def resolve(dialog_name: str) -> Callable:
    """Return the opener for a dialog or a blocking callable."""
    return DIALOG_OPENERS.get(
        dialog_name,
        _blocked(f"no opener registered for {dialog_name}"),
    )
