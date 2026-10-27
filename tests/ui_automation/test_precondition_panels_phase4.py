# -*- coding: utf-8 -*-
"""Phase-4: Precondition-achievable dialog and panel tests.

Fixes 40 wont-fix rows that were blocked on "requires preconditions."
Four groups, each needing a specific setup sequence:

  formFieldPropertiesPanel (10) — FormTool + Tab to field
  imagePropertiesPanel    (14) — ImageTool + canvas click on image
  atlasComparisonDialog   (12) — TRE edit on scanned PDF (auto-opens dialog)
  annotFontControls        (4) — FreeText annotation selected
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
from typing import Callable, Dict, List, Optional, Tuple

import pytest

# GAP-A14 (Phase 7): the atlasComparisonDialog group asserts that the modal
# AtlasComparisonDialog opened with the expected child widgets. On the
# Windows-Server-2019 CI session pywinauto's UIA cannot enumerate Qt modal
# dialogs (Desktop().windows() / dialog.descendants() block in FindAll), so we
# drive the TestDialogSeam.h introspection seam instead: the dialog records its
# child-widget inventory on construction and PDFE_TEST_DIALOG_AUTODISMISS makes
# the modal exec() return at once, freeing the event loop to answer the
# introspection readback pipe. That env var is set PER-TEST (atlas group only)
# inside test_precondition_panels — NOT module-globally — so the form/image/
# annot dock-panel groups are never affected by incidental modal auto-dismiss.
# Start from a known-clean state in case a prior process leaked it.
os.environ.pop("PDFE_TEST_DIALOG_AUTODISMISS", None)

REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tests.ui_automation.harness.pdfe_app_fixture import (  # noqa: E402
    PDFE_EXE,
    PdfeAppFixture,
    find_widget,
    invoke_action,
    kill_pdfe_processes,
    read_dialog_introspection,
)
from tests.ui_automation.harness.dialog_helpers import find_by_suffix  # noqa: E402
from tests.ui_automation.conftest import record_matrix_result  # noqa: E402

MATRIX_CSV = Path(__file__).parent / "matrix" / "ui_test_matrix.csv"
FIXTURES_DIR = REPO_ROOT / "tests" / "fixtures"


def _launch(fixture: str, extra_wait: float = 4.0) -> Tuple[PdfeAppFixture, Path]:
    # Pipeline f7c2a4e8 phase-6 (D2 / P-GAP-74): default extra_wait bumped
    # from 2.0s to 4.0s. The chronic flake (14/23 phase-8 xpasses flipping
    # to xfail in phase-1 confirmation sweep) was timing-sensitive: at 2.0s
    # the atlas-dialog precondition state had occasionally not finished
    # initializing when the test queried the panel via UIA. The longer
    # default reduces the flake window. Per-test overrides may pass a
    # smaller value if needed.
    return _launch_impl(fixture, extra_wait)


def _launch_impl(fixture: str, extra_wait: float) -> Tuple[PdfeAppFixture, Path]:
    kill_pdfe_processes()
    tmp = Path(tempfile.mkdtemp(prefix="pdfe_phase4p_"))
    dst = tmp / fixture
    shutil.copy2(FIXTURES_DIR / fixture, dst)
    fx = PdfeAppFixture()
    fx.launch_args = [str(dst)]
    fx.setUp()
    try:
        fx.main_window.maximize()
    except Exception:
        pass
    time.sleep(extra_wait)
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


def _is_visible(widget) -> bool:
    if widget is None:
        return False
    try:
        return widget.is_visible()
    except Exception:
        return False


def _coord_click(fx: PdfeAppFixture, coord: str, settle: float = 1.5) -> bool:
    """Synthesize a canvas click at a fitz-space point via the
    ``testClickByPdfCoordPipe`` seam (PdfCanvas::clickByPdfCoord).

    GAP-A14 / phase-4 reliability rewrite: the previous form/image/annot
    handlers selected the target object via ``click_canvas(rel_x, rel_y)``
    (a raw Win32 screen-coord click whose mapping ignores dock margins and
    scroll offset) or ``pyautogui_click``. Both miss the object's hit-test
    box intermittently on the maximized CI window. The coord pipe routes a
    press+release through the real tool pipeline using the canvas's own
    PDF→screen math, so the click lands exactly on the object. ``coord`` is
    ``"pageIndex,fitzX,fitzY"`` (origin top-left, y-down — matching
    PyMuPDF ``page.get_text('words')`` / widget/annot rects).
    """
    pipe = find_by_suffix(fx.main_window, "testClickByPdfCoordPipe")
    if pipe is None:
        return False
    try:
        pipe.set_edit_text(coord)
    except Exception:
        try:
            pipe.set_value(coord)
        except Exception:
            return False
    time.sleep(settle)
    return True


def _find_widget_opt(fx: PdfeAppFixture, leaf_id: str, timeout: float = 4.0):
    """``find_widget`` that returns ``None`` instead of raising.

    Uses the harness suffix matcher (Qt6 reports the full QObject path as
    the UIA AutomationId, so an exact ``auto_id`` match misses the leaf
    ``objectName`` we set in C++).
    """
    try:
        return find_widget(fx.main_window, leaf_id, timeout=timeout)
    except Exception:
        return None


# ---------------------------------------------------------------------------
# Group 1: formFieldPropertiesPanel (10 rows)
#
# test_form.pdf fields in Tab order:
#   FirstName (Text), LastName (Text), Comments (Text/multiline),
#   AgreeTerms (CheckBox), Subscribe (CheckBox),
#   Gender (RadioButton), Gender (RadioButton),
#   Country (ComboBox), Interests (ListBox)
#
# Tab count mapping to show each stacked page:
#   1-2 Tab → Text field → formFieldTextEdit visible
#   3 Tab → multiline Text → formFieldMultilineEdit visible
#   4-5 Tab → CheckBox → formFieldCheckbox visible
#   6-7 Tab → RadioButton → formFieldNoEditor visible
#   8 Tab → ComboBox → formFieldCombo visible
#   9 Tab → ListBox → formFieldList visible
# ---------------------------------------------------------------------------

# Phase-4 reliability rewrite (2026-06-14): the previous version activated
# FormTool via ``formToolActionButton.click_input()`` (a UIA toolbar click)
# then pressed N Tabs to land on a field type, then asserted the matching
# QStackedWidget page via ``is_visible()``. That path was flaky two ways:
# (1) the UIA toolbar click intermittently failed to toggle FormTool on, and
# (2) Tab navigation order is not guaranteed stable across the QWidgetForm
# tab chain, so the wrong stacked page (hence wrong widget) was current.
# Worse, the Tab map asserted ``formFieldNoEditor`` (editorStack index 5,
# the *default* page) for a RadioButton — but FormFieldPropertiesPanel maps
# RadioButton to index 2 (the checkbox page, label "Selected"), NOT index 5.
# Index 5 is reached only by PushButton / Signature / Unknown field types.
# test_form.pdf has none of those, so ``formFieldNoEditor`` could never be
# the current page there. We now (a) activate FormTool via the reliable
# ``invoke_action`` pipe seam, (b) select the *specific* field that exposes
# each widget by clicking its fitz-space center via ``testClickByPdfCoordPipe``
# (FormTool.fieldSelected → onFormFieldSelected sets the field + raises the
# dock), and (c) use the pushbutton fixture for formFieldNoEditor so a
# genuine Button field drives editorStack to index 5.
#
# Per-widget (fixture, "page,fitzX,fitzY") selecting the field whose type
# makes that widget the current QStackedWidget page (field rects from
# PyMuPDF page.widgets()):
_FORM_WIDGET_SELECT = {
    # Always-visible info widgets / the stack itself — any selected field
    # works; use FirstName (Text, idx 0).
    "formFieldTypeLabel":     ("test_form.pdf", "0,280,92"),
    "formFieldNameLabel":     ("test_form.pdf", "0,280,92"),
    "formFieldFlagsLabel":    ("test_form.pdf", "0,280,92"),
    "formFieldEditorStack":   ("test_form.pdf", "0,280,92"),
    "formFieldTextEdit":      ("test_form.pdf", "0,280,92"),    # FirstName Text → idx 0
    "formFieldMultilineEdit": ("test_form.pdf", "0,280,187"),   # Comments multiline → idx 1
    "formFieldCheckbox":      ("test_form.pdf", "0,190,242"),   # AgreeTerms CheckBox → idx 2
    "formFieldCombo":         ("test_form.pdf", "0,280,362"),   # Country ComboBox → idx 3
    "formFieldList":          ("test_form.pdf", "0,280,432"),   # Interests ListBox → idx 4
    # PushButton → editorStack default case → index 5 (formFieldNoEditor).
    "formFieldNoEditor":      ("test_form_with_pushbutton.pdf", "0,122,77"),
}

# Widgets whose visibility depends on the QStackedWidget current page (only
# the current page's child is visible). The four "always present" info
# widgets and the stack itself are NOT page-gated.
_FORM_PAGE_GATED = {
    "formFieldTextEdit", "formFieldMultilineEdit", "formFieldCheckbox",
    "formFieldCombo", "formFieldList", "formFieldNoEditor",
}


def _make_form_field_handler(widget_suffix: str):
    _, coord = _FORM_WIDGET_SELECT[widget_suffix]

    def handler(fx: PdfeAppFixture, tmp: Path) -> Tuple[str, str]:
        if not invoke_action(fx.main_window, "formToolAction", settle_s=0.8):
            return "red", "formToolAction pipe not reachable (PDFE_TEST_PIPES?)"

        if not _coord_click(fx, coord):
            return "red", "testClickByPdfCoordPipe not reachable"

        panel = find_by_suffix(fx.main_window, "formFieldPropertiesPanel")
        if panel is None:
            return "red", "formFieldPropertiesPanel not found in UIA tree"
        if not _is_visible(panel):
            return "red", "formFieldPropertiesPanel not visible after field select"

        # For page-gated widgets, is_visible() is the real assertion (only the
        # current stacked page's child is visible). For always-present info
        # widgets, find + visible suffices.
        widget = _find_widget_opt(fx, widget_suffix)
        if widget is None:
            return "red", (
                f"{widget_suffix} not found after selecting field via coord {coord}"
                + (" (wrong stacked page current?)"
                   if widget_suffix in _FORM_PAGE_GATED else "")
            )
        if not _is_visible(widget):
            return "red", (
                f"{widget_suffix} found but not visible after field select"
            )

        try:
            text = widget.window_text() or ""
        except Exception:
            text = ""

        return "green", (
            f"{widget_suffix} visible after FormTool + field select @ {coord}"
            + (f", text={text!r}" if text and len(text) < 50 else "")
        )
    return handler


# ---------------------------------------------------------------------------
# Group 2: imagePropertiesPanel (14 rows)
# ---------------------------------------------------------------------------

_IMAGE_PROP_WIDGETS = [
    "imageInfoGroup",
    "imageXObjectNameLabel",
    "imagePixelDimensionsLabel",
    "imageDisplayDimensionsLabel",
    "imageDpiLabel",
    "imageColorSpaceLabel",
    "imageCompressionLabel",
    "imageTransformGroup",
    "imageWidthSpin",
    "imageHeightSpin",
    "imageRotationCombo",
    "imageCropButton",
    "imageExtractButton",
    "imageRemoveButton",
]


# Phase-4 reliability rewrite (2026-06-14): the previous version activated
# ImageTool via ``imageToolActionButton.click_input()`` (UIA toolbar click)
# and selected the image via ``click_canvas(rel_x=0.5, rel_y=0.5)`` — a raw
# Win32 screen-coord click whose mapping ignores the dock margins / scroll
# offset of the maximized CI window, so the synthetic point frequently
# landed outside the image's hit-test box. When the image was not selected
# the ImagePropertiesPanel dock stayed hidden (it is shown only on
# ImageTool::imageSelected → setImageInfo + dock->show()), so ALL 14 widgets
# read not-visible; even when the panel did appear, the four transform-section
# widgets at the bottom (rotationCombo / cropButton / extract / remove) could
# fall outside the panel's first-paint viewport. We now activate via the
# ``invoke_action`` pipe seam and select the image by clicking its true
# fitz-space center via ``testClickByPdfCoordPipe`` (PdfCanvas::clickByPdfCoord
# → ImageTool.hitTestImage → imageSelected). test_fullpage_image.pdf's image
# bbox is (50,50,562,742) on a 612x792 page → center (306,396). With reliable
# selection, setImageInfo enables the transform controls and the dock shows;
# all 14 widgets are then found + visible.
_IMAGE_SELECT_COORD = "0,306,396"  # center of test_fullpage_image.pdf's image


def _make_image_prop_handler(widget_suffix: str):
    def handler(fx: PdfeAppFixture, tmp: Path) -> Tuple[str, str]:
        if not invoke_action(fx.main_window, "imageToolAction", settle_s=0.8):
            return "red", "imageToolAction pipe not reachable (PDFE_TEST_PIPES?)"

        if not _coord_click(fx, _IMAGE_SELECT_COORD, settle=2.0):
            return "red", "testClickByPdfCoordPipe not reachable"

        panel = find_by_suffix(fx.main_window, "imagePropertiesPanel")
        if panel is None:
            return "red", "imagePropertiesPanel not found after ImageTool+click"
        if not _is_visible(panel):
            return "red", "imagePropertiesPanel not visible after ImageTool+click"

        widget = _find_widget_opt(fx, widget_suffix)
        if widget is None:
            return "red", f"{widget_suffix} not found after image selection"
        if not _is_visible(widget):
            return "red", f"{widget_suffix} found but not visible"

        try:
            text = widget.window_text() or ""
        except Exception:
            text = ""

        return "green", (
            f"{widget_suffix} visible after ImageTool + coord click @ "
            f"{_IMAGE_SELECT_COORD}"
            + (f", text={text!r}" if text and len(text) < 80 else "")
        )
    return handler


# ---------------------------------------------------------------------------
# Group 3: atlasComparisonDialog (12 rows)
#
# The dialog auto-opens on first span click when TRE builds both atlas
# types on a scanned (image-dominated) PDF. Sequence:
#   1. TextEditTool active
#   2. Click on OCR text region in canvas
#   3. TRE runs OCR, builds atlas
#   4. If atlasChoiceMade_ is false, dialog auto-opens
#
# We try multiple click positions and wait up to 30s total.
# ---------------------------------------------------------------------------

_ATLAS_WIDGETS = [
    "atlasOriginalPreview",
    "atlasFontPreview",
    "atlasFontCombo",
    "atlasAtlasPreview",
    "atlasFontCharsetScroll",
    "atlasFontCharsetLabel",
    "atlasUseFontButton",
    "atlasFillMissingCheck",
    "atlasAtlasCharsetScroll",
    "atlasAtlasCharsetLabel",
    "atlasUseScanButton",
    "atlasCancelButton",
]


def _trigger_atlas_dialog(fx: PdfeAppFixture) -> Tuple[Optional[str], Optional[str]]:
    """Trigger TRE on the scanned PDF and capture the AtlasComparisonDialog's
    recorded child-widget inventory via the TestDialogSeam.h introspection seam.

    GAP-A14 rewrite: the previous version located the live dialog window via
    ``pywinauto.Desktop(backend="uia").windows()`` and walked
    ``dialog.descendants()`` — both block indefinitely in UIA ``FindAll`` on the
    Windows-Server-2019 CI session, wedging the whole test file. Instead we:

    1. Activate TextEditTool via ``testToolActivatePipe`` (no UIA toolbar lookup).
    2. Click a known scanned-text span via ``testClickByPdfCoordPipe``.
    3. The first-span-click path in MainWindow constructs AtlasComparisonDialog,
       which calls ``registerDialogIntrospection(this)`` (recording its widget
       inventory) and, because ``PDFE_TEST_DIALOG_AUTODISMISS=1`` is set, the
       modal ``exec()`` rejects on the next event-loop turn — freeing the event
       loop to answer the introspection readback pipe.
    4. Poll ``read_dialog_introspection`` (a pipe round-trip, never a UIA tree
       walk) until the inventory string appears.

    Returns ``(inventory_string, None)`` on success or ``(None, error)``.
    """
    # Stage 1: activate TextEditTool via pipe (bypasses UIA toolbar).
    activate_pipe = find_by_suffix(fx.main_window, "testToolActivatePipe")
    if activate_pipe is None:
        return None, "testToolActivatePipe not found (PDFE_TEST_PIPES not set?)"
    activate_pipe.set_edit_text("textEditToolAction")
    time.sleep(1.5)

    # Stage 2: click on a known text span via the PDF-coord pipe.
    # Coordinates are fitz-space word centers (origin top-left, y-down) from
    # test_scanned_with_ocr.pdf's hidden text layer (612x792 page). The first
    # click on "Document" is confirmed to open the AtlasComparisonDialog
    # instantly; the rest are fallbacks in case OCR span hit-testing misses.
    coord_pipe = find_by_suffix(fx.main_window, "testClickByPdfCoordPipe")
    if coord_pipe is None:
        return None, "testClickByPdfCoordPipe not found (PDFE_TEST_PIPES not set?)"
    click_coords = [
        "0,352.6,108.8",  # "Document"
        "0,190.2,161.1",  # "test"
        "0,96.0,108.8",   # "Test"
        "0,245.9,165.9",  # "page"
    ]
    for coord_str in click_coords:
        try:
            coord_pipe.set_edit_text(coord_str)
        except Exception:
            pass
        # The dialog opens on the first valid span click (atlas build runs on
        # tool-activate, so it is ready by the time we click). Poll the
        # introspection seam (a pipe round-trip, no UIA enumeration) briefly.
        for _ in range(6):
            time.sleep(1.5)
            rec = read_dialog_introspection(fx.main_window, "AtlasComparisonDialog")
            if rec and rec not in ("__null__", ""):
                return rec, None
        # No dialog recorded yet — try the next candidate text position.

    return None, (
        "AtlasComparisonDialog inventory never recorded after TextEditTool "
        "(via pipe) + coord clicks (waited ~80s total). TRE+OCR may not have "
        "found text spans in test_scanned.pdf, or Tesseract eng.traineddata "
        "may be missing."
    )


def _make_atlas_handler(widget_suffix: str):
    def handler(fx: PdfeAppFixture, tmp: Path) -> Tuple[str, str]:
        rec, err = _trigger_atlas_dialog(fx)
        if err:
            return "red", f"atlas dialog: {err}"

        # rec == "AtlasComparisonDialog|name1,name2,...[||combos=...]"
        body = rec.split("||", 1)[0]
        _, _, names_csv = body.partition("|")
        names = [n.strip() for n in names_csv.split(",") if n.strip()]

        if widget_suffix in names:
            return "green", (
                f"{widget_suffix} present in AtlasComparisonDialog inventory "
                f"({len(names)} widgets recorded)"
            )
        return "red", (
            f"{widget_suffix} not in AtlasComparisonDialog inventory; "
            f"recorded {len(names)} widgets"
        )
    return handler


# ---------------------------------------------------------------------------
# Group 4: annotationPropertiesWidget font controls (4 rows)
# ---------------------------------------------------------------------------

_ANNOT_FONT_WIDGETS = [
    "annotFontGroup",
    "annotFontCombo",
    "annotFontSizeSpinner",
    "annotTextColorButton",
]


# Phase-4 reliability rewrite (2026-06-14): the previous version activated
# SelectTool via ``selectToolActionButton.click_input()`` then selected the
# FreeText annotation with a ``pyautogui_click`` at a screen point derived
# from a naive page_w/page_h ratio (ignoring dock margins + scroll). That
# point missed the annot's SelectTool hit-test box, so
# AnnotationManager::getAnnotationAt returned nothing, no annotationSelected
# fired, and AnnotationPropertiesWidget::setAnnotation was never called with a
# FreeText type. The font controls (annotFontGroup and its children) are
# created ``setVisible(false)`` and only shown when setAnnotation sees a
# FreeText annot — a hidden QWidget is excluded from the UIA tree entirely,
# so all four reads returned NOT FOUND. We now select the annotation through
# the dedicated ``testAnnotSelectPipe`` seam (gated by PDFE_TEST_ANNOT_PIPES,
# which launch_pdfe sets): writing "pageIndex,annotIndex" calls
# getAnnotation(page,idx) → setAnnotation(data) directly (no hit-test, no
# coord round-trip), which shows fontGroup for the FreeText annot and raises
# the dock. test_with_freetext_annot.pdf has its single FreeText annot at
# page 0, index 0.
def _make_annot_font_handler(widget_suffix: str):
    def handler(fx: PdfeAppFixture, tmp: Path) -> Tuple[str, str]:
        sel = find_by_suffix(fx.main_window, "testAnnotSelectPipe")
        if sel is None:
            return "red", (
                "testAnnotSelectPipe not reachable (PDFE_TEST_ANNOT_PIPES?)"
            )
        try:
            sel.set_edit_text("0,0")  # page 0, annot 0 (the FreeText)
        except Exception:
            try:
                sel.set_value("0,0")
            except Exception:
                return "red", "testAnnotSelectPipe write failed"
        time.sleep(1.2)

        widget = _find_widget_opt(fx, widget_suffix)
        if widget is None:
            return "red", (
                f"{widget_suffix} not found after selecting FreeText annotation "
                f"(fontGroup hidden → not a FreeText, or selection failed)"
            )
        if not _is_visible(widget):
            return "red", (
                f"{widget_suffix} found but not visible after FreeText selection"
            )

        return "green", (
            f"{widget_suffix} visible after FreeText annotation selected "
            f"via testAnnotSelectPipe"
        )
    return handler


# ---------------------------------------------------------------------------
# Build dispatch table
# ---------------------------------------------------------------------------

HANDLERS: Dict[str, Tuple[str, Callable]] = {}

for ws, (fixture_for_ws, _coord) in _FORM_WIDGET_SELECT.items():
    tid = f"dlg_formFieldPropertiesPanel_{ws}"
    # formFieldNoEditor needs a PushButton field (editorStack default → idx 5),
    # which only test_form_with_pushbutton.pdf provides; the rest use test_form.pdf.
    HANDLERS[tid] = (fixture_for_ws, _make_form_field_handler(ws))

for ws in _IMAGE_PROP_WIDGETS:
    tid = f"dlg_imagePropertiesPanel_{ws}"
    HANDLERS[tid] = ("test_fullpage_image.pdf", _make_image_prop_handler(ws))

for ws in _ATLAS_WIDGETS:
    tid = f"dlg_atlasComparisonDialog_{ws}"
    # GAP-A14: the AtlasComparisonDialog span-click flow only fires on scans
    # that carry a hidden OCR text layer (hasTextContent() true → TextEditTool
    # span flow). test_scanned.pdf is a PURE raster (0 text chars) and routes
    # to the TRE region-drag flow, which never emits spanActivated, so the
    # dialog could never open. test_scanned_with_ocr.pdf carries a 35-word
    # hidden text layer over the raster — confirmed to open the dialog on the
    # first span click.
    HANDLERS[tid] = ("test_scanned_with_ocr.pdf", _make_atlas_handler(ws))

for ws in _ANNOT_FONT_WIDGETS:
    tid = f"dlg_annotationPropertiesWidget_{ws}"
    HANDLERS[tid] = ("test_with_freetext_annot.pdf", _make_annot_font_handler(ws))


# ---------------------------------------------------------------------------
# Matrix row loader
# ---------------------------------------------------------------------------

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
def test_precondition_panels(row: dict) -> None:
    test_id = row["test_id"]
    entry = HANDLERS.get(test_id)
    if entry is None:
        record_matrix_result(test_id, "blocked", "no handler registered")
        pytest.skip(f"{test_id}: no handler")
    fixture, handler = entry

    extra_wait = 3.0 if "atlas" in test_id else 2.0

    # Scope PDFE_TEST_DIALOG_AUTODISMISS to the atlas group ONLY. It makes
    # modal exec() return immediately (needed so the AtlasComparisonDialog
    # introspection readback pipe can be answered), but the form/image/annot
    # groups drive dock panels via real tool interactions that must NOT have
    # any incidental modal auto-rejected out from under them. launch_pdfe
    # copies os.environ at launch time, so toggling here per-test is honored.
    if "atlas" in test_id:
        os.environ["PDFE_TEST_DIALOG_AUTODISMISS"] = "1"
    else:
        os.environ.pop("PDFE_TEST_DIALOG_AUTODISMISS", None)

    fx, tmp = _launch(fixture, extra_wait=extra_wait)
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
