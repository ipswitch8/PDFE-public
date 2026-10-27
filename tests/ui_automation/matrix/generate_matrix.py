# -*- coding: utf-8 -*-
"""
Deterministic generator for the PDFE UI test matrix CSV.

Produces tests/ui_automation/matrix/ui_test_matrix.csv with one row per
planned end-to-end test case, covering the entire application surface:
menus, dialogs, annotation tools, form field types, page operations,
image operations, security, compliance, text formatting, and
canvas-coordinate interactions.

The CSV is the source of truth for the regression suite. Individual
test implementations under tests/ui_automation/test_*.py reference test
IDs from this matrix, and the CI integration phase (phase-11) updates
the status column from 'planned' to 'green'/'red'/'blocked' as tests
land.

Run:
    python tests/ui_automation/matrix/generate_matrix.py

Deterministic: same input produces same output on any machine, same
bytes. No timestamps, no randomness.
"""
from __future__ import annotations

import csv
import re
from dataclasses import dataclass, asdict
from pathlib import Path
from typing import Dict, List


HEADER = [
    "subsystem", "category", "element", "test_id", "preconditions",
    "action", "expected", "tool", "priority", "status", "owner_phase",
    "fixture", "notes",
]


@dataclass
class Row:
    subsystem: str
    category: str
    element: str
    test_id: str
    preconditions: str
    action: str
    expected: str
    tool: str
    priority: str
    status: str
    owner_phase: str
    fixture: str
    notes: str


ROWS: List[Row] = []


def add(**kwargs) -> None:
    row = Row(**kwargs)
    # Enforce allowed values.
    assert row.priority in ("P0", "P1", "P2"), row.priority
    assert row.status in ("planned", "blocked"), row.status
    assert row.owner_phase.startswith("phase-"), row.owner_phase
    ROWS.append(row)


# ---------------------------------------------------------------------------
# Phase 2 — P0 menu navigation (~60 tests)
# ---------------------------------------------------------------------------
# One row per leaf menu item in each of the 9 top-level menus.
# Tests verify clicking the item opens its dialog (or executes its action)
# without crashing. Dialog widget depth is covered separately in phase-3.

MENU_ITEMS = {
    "File": [
        ("New", "open_new_document", "no doc", "click File > New",
         "main window title becomes 'Untitled' or similar"),
        ("Open", "open_valid_pdf", "no doc", "click File > Open and pick test_3page.pdf",
         "3-page doc loaded, page count == 3"),
        ("OpenRecent", "open_recent_file", "recent list non-empty",
         "click File > Open Recent > first entry",
         "that file is loaded"),
        ("Close", "close_doc", "doc open", "click File > Close",
         "main window shows no-document state"),
        ("Save", "save_clean", "doc open and unchanged", "click File > Save",
         "no-op or save without error"),
        ("SaveAs", "save_as", "doc open", "click File > Save As, pick path",
         "file written at new path"),
        ("Print", "print_open_dialog", "doc open", "click File > Print",
         "Print dialog opens"),
        ("PrintPreview", "print_preview_open", "doc open",
         "click File > Print Preview", "PrintPreview dialog opens"),
        ("Properties", "properties_open", "doc open",
         "click File > Properties (or Document Properties)",
         "Metadata dialog opens"),
        ("Exit", "exit_clean", "no dirty state",
         "click File > Exit", "process exits 0"),
    ],
    "Edit": [
        ("Undo", "undo_basic", "recent edit in undo stack", "click Edit > Undo",
         "undo stack pops"),
        ("Redo", "redo_basic", "recent undo", "click Edit > Redo",
         "redo stack pops"),
        ("Cut", "cut_basic", "selection active", "click Edit > Cut",
         "clipboard has selection, selection removed"),
        ("Copy", "copy_basic", "selection active", "click Edit > Copy",
         "clipboard has selection"),
        ("Paste", "paste_basic", "clipboard non-empty", "click Edit > Paste",
         "clipboard text inserted at cursor"),
        ("SelectAll", "select_all", "editing", "click Edit > Select All",
         "selection covers entire span"),
        ("Find", "find_open", "doc open", "click Edit > Find",
         "SearchPanel dock visible"),
        ("Replace", "replace_open", "doc open", "click Edit > Replace",
         "find/replace UI accessible"),
        ("Preferences", "preferences_open", "any",
         "click Edit > Preferences",
         "preferences dialog opens"),
    ],
    "Pages": [
        ("RotateCW", "rotate_cw", "doc open", "click Pages > Rotate CW",
         "current page rotation += 90"),
        ("RotateCCW", "rotate_ccw", "doc open", "click Pages > Rotate CCW",
         "current page rotation -= 90"),
        ("Rotate180", "rotate_180", "doc open", "click Pages > Rotate 180",
         "current page rotation += 180"),
        ("Delete", "delete_page", "multi-page doc", "click Pages > Delete",
         "page count decremented"),
        ("InsertBlank", "insert_blank", "doc open", "click Pages > Insert Blank",
         "page count incremented"),
        ("InsertFromFile", "insert_from_file", "doc open + another PDF file",
         "click Pages > Insert From File, pick file",
         "page count increased by source count"),
        ("Extract", "extract_page", "doc open",
         "click Pages > Extract, pick output path",
         "file written with just selected page"),
        ("Move", "move_page", "multi-page doc",
         "click Pages > Move Up",
         "current page index decreases by 1"),
    ],
    "Forms": [
        ("CreateField", "create_field", "doc open",
         "click Forms > Create Field",
         "field creation mode active"),
        ("ImportData", "import_data_xfdf", "doc with fields",
         "click Forms > Import Data, pick XFDF",
         "field values populated from XFDF"),
        ("ExportData", "export_data_xfdf", "doc with field values",
         "click Forms > Export Data, pick output",
         "XFDF file written"),
        ("ResetForm", "reset_form", "doc with modified fields",
         "click Forms > Reset", "fields returned to /DV defaults"),
        ("FieldProperties", "field_properties_open",
         "field selected", "click Forms > Field Properties",
         "FormFieldPropertiesPanel visible"),
        ("TabOrder", "tab_order", "doc with fields",
         "click Forms > Set Tab Order",
         "tab order editing mode active"),
        ("SubmitForm", "submit_form", "doc with submit action",
         "click Forms > Submit", "submit invoked (dialog or HTTP)"),
    ],
    "Security": [
        ("SetPassword", "set_password_open", "doc open",
         "click Security > Set Password", "EncryptionDialog opens"),
        ("RemovePassword", "remove_password", "encrypted doc",
         "click Security > Remove Password",
         "doc is no longer encrypted after save"),
        ("Sign", "sign_open", "doc open",
         "click Security > Sign", "SignDialog opens"),
        ("VerifySignatures", "verify_signatures", "signed doc",
         "click Security > Verify Signatures",
         "SignaturePanel shows status per sig"),
        ("Redact", "redact_mark", "doc open",
         "click Security > Redact > Mark",
         "redaction mark tool active"),
        ("ApplyRedactions", "apply_redactions",
         "doc with marked redactions",
         "click Security > Apply Redactions",
         "marked regions burned into the doc"),
    ],
    "Image": [
        ("Insert", "image_insert", "doc open",
         "click Image > Insert, pick image",
         "image XObject added to page"),
        ("Remove", "image_remove", "doc with image",
         "click Image > Remove with image selected",
         "image XObject removed"),
        ("Crop", "image_crop", "doc with image",
         "click Image > Crop, drag crop rect",
         "image content clipped"),
        ("Rotate", "image_rotate", "doc with image",
         "click Image > Rotate 90",
         "image appears rotated"),
        ("Resize", "image_resize", "doc with image",
         "click Image > Resize, drag handle",
         "image dimensions change"),
        ("StripExif", "image_strip_exif",
         "doc with JPEG image containing EXIF",
         "click Image > Strip EXIF",
         "EXIF section removed from embedded JPEG"),
    ],
    "Tools": [
        ("OCR", "tools_ocr", "image-based PDF",
         "click Tools > OCR", "Tesseract OCR runs, text layer added"),
        ("TRE", "tools_tre", "image-based PDF",
         "click Tools > Text Reconstruction", "TRE dialog opens"),
        ("Compliance", "tools_compliance", "doc open",
         "click Tools > Compliance", "ComplianceDialog opens"),
        ("Accessibility", "tools_accessibility", "doc open",
         "click Tools > Accessibility Check",
         "accessibility report produced"),
        ("BatchProcess", "tools_batch",
         "any", "click Tools > Batch Process", "batch dialog opens"),
        ("Compare", "tools_compare", "doc open",
         "click Tools > Compare Documents, pick second file",
         "comparison view shown"),
    ],
    "View": [
        ("ZoomIn", "zoom_in", "doc open", "click View > Zoom In",
         "zoom level increases"),
        ("ZoomOut", "zoom_out", "doc open", "click View > Zoom Out",
         "zoom level decreases"),
        ("FitWidth", "fit_width", "doc open", "click View > Fit Width",
         "zoom fits page width"),
        ("FitPage", "fit_page", "doc open", "click View > Fit Page",
         "entire page visible"),
        ("ActualSize", "actual_size", "doc open", "click View > Actual Size",
         "zoom set to 100%"),
        ("RotateView", "rotate_view", "doc open",
         "click View > Rotate View CW",
         "viewport rotates without modifying doc"),
        ("FullScreen", "full_screen", "any",
         "click View > Full Screen", "window enters fullscreen"),
        ("ShowBookmarks", "view_bookmarks_panel",
         "any", "click View > Bookmarks",
         "BookmarkPanel dock visible"),
        ("Theme", "view_theme_toggle", "any",
         "click View > Theme > Dark",
         "palette switches to dark theme"),
    ],
    "Help": [
        ("About", "help_about", "any", "click Help > About PDFE",
         "AboutDialog opens"),
        ("Documentation", "help_documentation", "any",
         "click Help > Documentation",
         "browser opens docs URL or local help"),
        ("ReportIssue", "help_report_issue", "any",
         "click Help > Report Issue",
         "browser opens GitHub issues URL"),
        ("CheckUpdates", "help_check_updates", "any",
         "click Help > Check For Updates",
         "update check runs, result shown"),
    ],
}

for menu, items in MENU_ITEMS.items():
    for element, test_id, pre, action, expected in items:
        add(subsystem=menu, category="Menu",
            element=f"{menu} > {element}", test_id=f"menu_{test_id}",
            preconditions=pre, action=action, expected=expected,
            tool="pywinauto", priority="P0", status="planned",
            owner_phase="phase-2",
            fixture="test_3page.pdf",
            notes="Navigate by title — QMenuBar.QAction auto_id shared")

# ---------------------------------------------------------------------------
# Phase 3 — P0 dialog widget reachability (~150 tests)
# ---------------------------------------------------------------------------
# One row per documented widget in each dialog. Tests assert the widget
# exists (by suffix match on auto_id) and accepts input where applicable.

# ---------------------------------------------------------------------------
# C4 Phase 0 correction: scrape real widget names from src/ui/*.cpp so the
# phase-3 rows reflect what pdfe.exe actually exposes via setObjectName,
# instead of encoding aspirational names that don't match source.
# See qa-expert gate findings: ~65% of the original hand-written lists
# were fabricated.
# ---------------------------------------------------------------------------

SRC_UI_DIR = Path(__file__).resolve().parents[3] / "src" / "ui"

# Map dialog-class source file names to the (objectName root) of the
# top-level dialog/panel. The scraper extracts every setObjectName("...")
# call from the file and returns the set, minus the root itself.
DIALOG_SOURCES = {
    "aboutDialog":                ("AboutDialog.cpp",               "aboutDialog"),
    "passwordDialog":             ("PasswordDialog.cpp",            "passwordDialog"),
    "metadataDialog":             ("MetadataDialog.cpp",            "metadataDialog"),
    "printDialog":                ("PrintDialog.cpp",               "printDialog"),
    "printPreviewDialog":         ("PrintPreviewDialog.cpp",        "printPreviewDialog"),
    "encryptionDialog":           ("EncryptionDialog.cpp",          "encryptionDialog"),
    "signDialog":                 ("SignDialog.cpp",                "signDialog"),
    "complianceDialog":           ("ComplianceDialog.cpp",          "ComplianceDialog"),
    "atlasComparisonDialog":      ("AtlasComparisonDialog.cpp",     "AtlasComparisonDialog"),
    "annotationPropertiesWidget": ("AnnotationPropertiesWidget.cpp","annotationPropertiesWidget"),
    "formFieldPropertiesPanel":   ("FormFieldPropertiesPanel.cpp",  "formFieldPropertiesPanel"),
    "imagePropertiesPanel":       ("ImagePropertiesPanel.cpp",      "imagePropertiesPanel"),
    "bookmarkPanel":              ("BookmarkPanel.cpp",             "bookmarkPanel"),
    "commentPanel":               ("CommentPanel.cpp",              "commentPanel"),
    "searchPanel":                ("SearchPanel.cpp",               "searchPanel"),
    "pagePanel":                  ("PagePanel.cpp",                 "pagePanel"),
    "signaturePanel":             ("SignaturePanel.cpp",            "signaturePanel"),
    "redactionPanel":             ("RedactionPanel.cpp",            "redactionPanel"),
    "defaultsLogPanel":           ("DefaultsLogPanel.cpp",          "defaultsLogPanel"),
}

OBJ_NAME_RE = re.compile(r'setObjectName\(\s*(?:QStringLiteral\()?\s*"([^"]+)"')


def scrape_widget_names(src_file: str, exclude: str) -> List[str]:
    """Return every non-root setObjectName string from the file,
    in source order with duplicates removed."""
    path = SRC_UI_DIR / src_file
    if not path.exists():
        raise SystemExit(
            f"Source file missing for widget scrape: {path}")
    text = path.read_text(encoding="utf-8", errors="replace")
    names: List[str] = []
    seen = set()
    for m in OBJ_NAME_RE.finditer(text):
        name = m.group(1)
        if name == exclude:
            continue  # top-level dialog/panel self
        if name in seen:
            continue
        seen.add(name)
        names.append(name)
    return names


DIALOG_WIDGETS: Dict[str, List[str]] = {
    dlg: scrape_widget_names(src, exclude)
    for dlg, (src, exclude) in DIALOG_SOURCES.items()
}

# Sanity: every dialog must have at least one widget scraped. An empty
# list means the source has no setObjectName calls or the scraper broke.
for dlg, widgets in DIALOG_WIDGETS.items():
    if not widgets:
        raise SystemExit(
            f"No widgets scraped for {dlg} — check src/ui/{DIALOG_SOURCES[dlg][0]}")

for dlg, widgets in DIALOG_WIDGETS.items():
    for w in widgets:
        add(subsystem="Dialogs", category="Dialog",
            element=f"{dlg}.{w}",
            test_id=f"dlg_{dlg}_{w}",
            preconditions=f"{dlg} open",
            action=f"find child widget by auto_id suffix '{w}'",
            expected="widget exists, size > 0, accepts input if editable",
            tool="pywinauto", priority="P0", status="planned",
            owner_phase="phase-3",
            fixture="test_3page.pdf",
            notes="Match by suffix on Qt's hierarchical auto_id")

# ---------------------------------------------------------------------------
# Phase 4 — P0 critical end-to-end flows (~20 tests)
# ---------------------------------------------------------------------------
CRITICAL_FLOWS = [
    ("OpenSaveAsRoundtrip", "flow_open_save_as",
     "test_3page.pdf on disk",
     "Open test_3page.pdf, Save As to temp path, verify file exists and parses",
     "temp file exists and pymupdf opens it with page_count == 3"),
    ("OpenEncryptedWithPassword", "flow_open_encrypted",
     "test_encrypted_aes256.pdf on disk",
     "Open encrypted file, enter password in PasswordDialog, expect success",
     "doc loads, main window shows page content"),
    ("OpenEncryptedWrongPassword", "flow_open_encrypted_wrong_pass",
     "test_encrypted_aes256.pdf on disk",
     "Open, enter wrong password, click OK",
     "error shown, doc not loaded"),
    ("OpenSignedDocument", "flow_open_signed",
     "test_signed.pdf on disk",
     "Open signed doc, check SignaturePanel",
     "SignaturePanel lists signature with valid status"),
    ("OpenTaggedPDFA", "flow_open_pdfa",
     "test_pdfa.pdf on disk",
     "Open tagged PDF/A-2a, verify ComplianceDialog validate",
     "validate reports conformance"),
    ("OpenMultiPage", "flow_open_multi",
     "test_3page.pdf on disk",
     "Open, iterate through pages 0..2",
     "current page advances each click"),
    ("OpenCorruptGracefully", "flow_open_corrupt",
     "corrupted.pdf on disk",
     "Open corrupt file",
     "error dialog, main window stays alive"),
    ("SaveAsNewName", "flow_save_as_new",
     "doc open unchanged",
     "Save As newname.pdf",
     "new file written, original unchanged"),
    ("SaveAsOverwrite", "flow_save_as_overwrite",
     "doc open unchanged + existing target file",
     "Save As existing path, confirm overwrite",
     "existing file replaced with new content"),
    ("PrintToFile", "flow_print_to_file",
     "doc open",
     "Print > Microsoft Print to PDF > pick path",
     "PDF output file created"),
    ("PrintCancel", "flow_print_cancel",
     "doc open",
     "Print > click Cancel",
     "no output produced, no errors"),
    ("TextEditCommit", "flow_text_edit_commit",
     "doc with editable text",
     "Enter text edit mode, change text, commit",
     "in-memory text model changed, undo stack updated"),
    ("TextEditCancel", "flow_text_edit_cancel",
     "doc with editable text",
     "Enter text edit mode, change text, press Escape",
     "original text preserved"),
    ("FormFieldFillAndSave", "flow_form_fill_save",
     "doc with text field",
     "Click text field, type, Save As",
     "saved file has the typed value in the field"),
    ("FormXFDFRoundtrip", "flow_form_xfdf_roundtrip",
     "doc with text field",
     "Export XFDF, reopen doc, Import XFDF",
     "field value restored"),
    ("AnnotationCreateSave", "flow_annot_create_save",
     "doc open",
     "Create sticky note annotation, Save As",
     "saved file contains the annotation"),
    ("UndoRedo", "flow_undo_redo",
     "doc open",
     "Make an edit, Undo, Redo",
     "final state matches post-edit state"),
    ("CloseDirtyConfirm", "flow_close_dirty",
     "doc with unsaved edits",
     "Click Close, confirm save in dialog",
     "dialog appears, save runs on confirm"),
    ("CloseCleanExit", "flow_close_clean_exit",
     "doc unchanged",
     "Click File > Exit",
     "process exits 0 without confirmation dialog"),
    ("RecentFilesRemembered", "flow_recent_files",
     "at least one prior open",
     "Open File > Open Recent",
     "recent file list has >= 1 entry"),
    ("OpenCorpusRobustness", "flow_corpus_robustness",
     "test_PDFs/ corpus directory populated "
     "(set PDFE_TEST_PDFS_DIR to override)",
     "Run tests/test_pdf_open_robustness.py which launches "
     "pdfe.exe against every .pdf in the corpus and checks "
     "each for graceful exit vs crash codes "
     "(ACCESS_VIOLATION, HEAP_CORRUPTION, STACK_BUFFER_OVERRUN)",
     "Zero crashes across the entire corpus; JSON report "
     "written to tests/pdf_open_robustness_results.json"),
]

for element, test_id, pre, action, expected in CRITICAL_FLOWS:
    # Per-row overrides for flows that deviate from the default
    # pywinauto/end-to-end pattern.
    tool = "pywinauto"
    row_notes = "End-to-end, requires on-disk side effects"
    if test_id == "flow_corpus_robustness":
        # Process-level test — just subprocess launch + exit-code
        # check. No UIA inspection needed; the question is
        # "does the process survive startup", not "does the UI
        # look right after load". Implemented as
        # tests/test_pdf_open_robustness.py.
        tool = "subprocess"
        row_notes = (
            "Process-level crash test. Honors "
            "PDFE_BUILD_CONFIG and PDFE_TEST_PDFS_DIR env vars. "
            "Uses kill_pdfe_processes() from the harness as a "
            "post-run sweeper."
        )
    add(subsystem="Flows", category="Flow",
        element=element, test_id=f"flow_{test_id}",
        preconditions=pre, action=action, expected=expected,
        tool=tool, priority="P0", status="planned",
        owner_phase="phase-4",
        fixture="(various)",
        notes=row_notes)

# ---------------------------------------------------------------------------
# Phase 5 — P1 annotation tool coverage (~30 tests)
# ---------------------------------------------------------------------------
ANNOTATION_TOOLS = [
    "StickyNote", "TextMarkup", "Ink", "Shape", "FreeText", "Select",
]
ANNOT_SUBTESTS = [
    ("create", "click canvas at 200,200", "new annotation in AnnotationManager"),
    ("set_color", "open color dialog, pick red", "annotation color updates"),
    ("set_opacity", "drag opacity slider to 50",
     "annotation opacity == 0.5"),
    ("set_contents", "edit contents to 'test'", "contents == 'test'"),
    ("delete", "select annotation, press Delete",
     "annotation count -= 1"),
]

for tool in ANNOTATION_TOOLS:
    for sub, action, expected in ANNOT_SUBTESTS:
        add(subsystem="Annotations", category="Flow",
            element=f"annot_{tool}",
            test_id=f"annot_{tool.lower()}_{sub}",
            preconditions="doc open, annotation toolbar visible",
            action=f"select {tool} tool, {action}",
            expected=expected, tool="mixed", priority="P1",
            status="planned", owner_phase="phase-5",
            fixture="test_3page.pdf",
            notes="Canvas interactions use PyAutoGUI")

# ---------------------------------------------------------------------------
# Phase 6 — P1 form field coverage (~25 tests)
# ---------------------------------------------------------------------------
FORM_FIELD_TYPES = [
    "Text", "Checkbox", "RadioButton", "ComboBox",
    "ListBox", "PushButton", "Signature", "Unknown",
]
FORM_SUBTESTS = [
    ("create", "create field via Forms menu", "field appears in form"),
    ("set_default", "set /DV default value",
     "reset returns to default"),
    ("xfdf_roundtrip", "export XFDF, reimport",
     "value round-trips"),
]
# qa-expert gate finding: Signature and Unknown field types don't
# cleanly fit the create/xfdf-roundtrip/fill pattern. Signature
# fields are created by the signing flow (not Forms > Create Field)
# and don't round-trip through XFDF per the ISO 32000-2 spec. The
# Unknown type is a parse-time fallback label, not a creatable
# type. Mark those rows P2/blocked with explanatory notes so no
# future implementer wastes a cycle chasing them.
for ft in FORM_FIELD_TYPES:
    for sub, action, expected in FORM_SUBTESTS:
        # Per-row overrides for structurally-impossible placeholders
        priority = "P1"
        status_ = "planned"
        row_notes = "Use FormDataIO for XFDF assertions"
        if ft == "Signature" and sub == "xfdf_roundtrip":
            priority = "P2"
            status_ = "blocked"
            row_notes = ("BLOCKED: Signature fields are created via "
                         "signing flow, not XFDF import/export. Out "
                         "of scope for this test.")
        elif ft == "Signature" and sub == "create":
            priority = "P2"
            row_notes = ("Signature fields created via Security > Sign, "
                         "not Forms > Create Field. Covered in phase-8.")
        elif ft == "Unknown" and sub == "create":
            priority = "P2"
            status_ = "blocked"
            row_notes = ("BLOCKED: Unknown is a parse-time fallback, "
                         "not a user-creatable field type.")
        elif ft == "Unknown":
            priority = "P2"
            status_ = "blocked"
            row_notes = ("BLOCKED: Unknown field type cannot be "
                         "instantiated through the UI.")
        add(subsystem="Forms", category="Flow",
            element=f"field_{ft}",
            test_id=f"field_{ft.lower()}_{sub}",
            preconditions="doc open with form field slot",
            action=action, expected=expected, tool="pywinauto",
            priority=priority, status=status_, owner_phase="phase-6",
            fixture="test_form.pdf",
            notes=row_notes)

# Add "fill value" test per type (additional row so target is met).
# Signature and Unknown get blocked rows per above reasoning.
for ft in FORM_FIELD_TYPES:
    priority = "P1"
    status_ = "planned"
    row_notes = ""
    if ft == "Signature":
        priority = "P2"
        row_notes = ("Value-fill for signature = sign operation, "
                     "covered by phase-8 sec_sign_* tests.")
    elif ft == "Unknown":
        priority = "P2"
        status_ = "blocked"
        row_notes = "BLOCKED: Unknown fields cannot be filled via UI."
    add(subsystem="Forms", category="Flow",
        element=f"field_{ft}",
        test_id=f"field_{ft.lower()}_fill_value",
        preconditions="doc open with form field",
        action=f"type/select value into {ft} field",
        expected="form state updated", tool="pywinauto",
        priority=priority, status=status_, owner_phase="phase-6",
        fixture="test_form.pdf", notes=row_notes)

# ---------------------------------------------------------------------------
# Phase 7 — P1 page operations (~40 tests)
# ---------------------------------------------------------------------------
PAGE_OPS = [
    ("rotate_cw_single", "rotate current page clockwise 90",
     "/Rotate key on page == 90"),
    ("rotate_ccw_single", "rotate current page counterclockwise 90",
     "/Rotate key == 270"),
    ("rotate_180_single", "rotate current page 180",
     "/Rotate key == 180"),
    ("rotate_cw_multi", "select 2 pages, rotate CW",
     "both pages have /Rotate == 90"),
    ("rotate_ccw_multi", "select 2 pages, rotate CCW",
     "both pages have /Rotate == 270"),
    ("rotate_180_multi", "select 2 pages, rotate 180",
     "both pages have /Rotate == 180"),
    ("delete_single", "delete current page",
     "page count -= 1"),
    ("delete_range", "select pages 1-2, delete",
     "page count -= 2"),
    ("delete_last_page_prevented", "try delete only page",
     "error or dialog, page count unchanged"),
    ("insert_blank_before", "insert blank before page 0",
     "page count += 1, new blank at index 0"),
    ("insert_blank_middle", "insert blank before middle page",
     "page count += 1"),
    ("insert_blank_end", "insert blank after last page",
     "page count += 1, new blank at last index"),
    ("insert_from_file_all", "insert all pages from other PDF",
     "page count += source count"),
    ("insert_from_file_range", "insert pages 1-2 from other PDF",
     "page count += 2"),
    ("move_up", "move page 2 up one",
     "page indices reordered"),
    ("move_down", "move page 1 down one",
     "page indices reordered"),
    ("move_to_start", "drag page 3 to index 0",
     "page 3 content now at index 0"),
    ("move_to_end", "drag page 0 to end",
     "page 0 content now at last index"),
    ("extract_single", "extract current page to file",
     "output file has 1 page matching the source"),
    ("extract_range", "extract pages 1-2 to file",
     "output file has 2 pages"),
    ("extract_noncontiguous",
     "extract pages 0 and 2 to file", "output has 2 pages"),
    ("multi_select_ctrl_click", "Ctrl+click thumbnails",
     "multi-select highlight"),
    ("multi_select_shift_click", "Shift+click thumbnails",
     "range selection"),
    ("drag_drop_reorder_short", "drag thumb from 0 to 1",
     "order swapped"),
    ("drag_drop_reorder_far", "drag thumb from 0 to last",
     "moved to end"),
    ("undo_rotate", "rotate then Ctrl+Z",
     "rotation reverted"),
    ("undo_delete", "delete then Ctrl+Z",
     "page restored"),
    ("undo_insert", "insert blank then Ctrl+Z",
     "page removed"),
    ("undo_move", "move page then Ctrl+Z",
     "order restored"),
    ("redo_rotate", "undo rotate then Ctrl+Y",
     "rotation reapplied"),
    ("redo_delete", "undo delete then Ctrl+Y",
     "page removed again"),
    ("page_panel_refresh",
     "modify structure, verify PagePanel thumbnails",
     "thumbnails match new structure"),
    ("large_doc_rotate", "open 100-page doc, rotate page 50",
     "rotation applied"),
    ("large_doc_delete_range",
     "open 100-page doc, delete pages 40-60",
     "page count -= 21"),
    ("rotate_cancel_undo", "rotate then undo",
     "no rotation left"),
    ("insert_from_encrypted",
     "try insert from encrypted file",
     "password prompt then success or refusal"),
    ("extract_to_encrypted_target",
     "extract to encrypted output setting",
     "target respects encryption setting"),
    ("page_resize_ignored",
     "verify rotate does not resize MediaBox",
     "MediaBox unchanged"),
    ("cropbox_preserved",
     "rotate and extract, verify CropBox",
     "CropBox intact"),
    ("rotate_respects_annotations",
     "rotate page with annotations",
     "annotations rotate with page"),
]
for test_id, action, expected in PAGE_OPS:
    # Per-row note: populate the Qt gotcha the test will actually
    # hit. qa-expert gate flagged the blank-notes risk.
    note = "Verify via PagePanel thumbnails after op"
    if "drag" in test_id:
        note = ("Drag-drop on Qt uses QDrag; pywinauto click_input "
                "with drag=True often misfires. Use PyAutoGUI "
                "moveTo + mouseDown/mouseUp for reliable drags.")
    elif "multi_select" in test_id:
        note = ("Modifier-key dispatch via pywinauto type_keys is "
                "unreliable for Ctrl+Click; use click_input with "
                "pressed='control' or 'shift'.")
    elif "insert_from_encrypted" in test_id:
        note = ("Modal password prompt blocks main window. Use "
                "Application.window(auto_id_re='.*passwordDialog$') "
                "to grab the child dialog before typing.")
    elif "extract_to_encrypted" in test_id:
        note = ("Target encryption dialog is separate from source. "
                "Verify encryption via saved file re-open.")
    elif "large_doc" in test_id:
        note = ("100-page ops are multi-second. Increase "
                "wait_for_window_visible timeout to 30s.")
    elif "undo_" in test_id or "redo_" in test_id:
        note = ("Use Ctrl+Z / Ctrl+Y via keyboard; menu Edit > Undo "
                "is equivalent but slower.")
    elif "rotate" in test_id:
        note = ("Verify /Rotate value by re-parsing the saved file "
                "with pymupdf; do not trust UI state alone.")
    elif "delete" in test_id:
        note = ("Page count via doc->pageCount() or pymupdf "
                "page_count after refresh.")
    elif "insert" in test_id:
        note = ("Page count after insert reflects doc structure "
                "change; force PagePanel refresh via signals.")
    elif "extract" in test_id:
        note = ("Extract writes a new file. Verify file exists and "
                "contains expected page count via pymupdf.")
    elif "move" in test_id:
        note = ("Move operates on indices, not references. Verify "
                "via page content checksums after move.")
    add(subsystem="Pages", category="Flow",
        element=f"page_{test_id}",
        test_id=f"page_{test_id}",
        preconditions="multi-page doc open",
        action=action, expected=expected, tool="pywinauto",
        priority="P1", status="planned", owner_phase="phase-7",
        fixture="test_3page.pdf or test_multisize.pdf",
        notes=note)

# ---------------------------------------------------------------------------
# Phase 8 — P1 security and compliance (~30 tests)
# ---------------------------------------------------------------------------
SECURITY_TESTS = [
    ("encrypt_rc4_40_set", "set RC4-40 encryption with password",
     "doc encrypted, save succeeds"),
    ("encrypt_rc4_40_reopen", "reopen RC4-40 encrypted doc",
     "password prompt, load success"),
    ("encrypt_rc4_128_set", "set RC4-128 encryption",
     "doc encrypted"),
    ("encrypt_rc4_128_reopen", "reopen RC4-128 doc",
     "password accepted"),
    ("encrypt_aes_128_set", "set AES-128 encryption",
     "doc encrypted"),
    ("encrypt_aes_128_reopen", "reopen AES-128 doc",
     "password accepted"),
    ("encrypt_aes_256_set", "set AES-256 encryption",
     "doc encrypted"),
    ("encrypt_aes_256_reopen", "reopen AES-256 doc",
     "password accepted"),
    ("encrypt_remove_password", "open encrypted, remove password, save",
     "doc no longer encrypted"),
    ("encrypt_permissions_print_blocked",
     "set print=false, verify print action",
     "print disabled"),
    ("encrypt_permissions_copy_blocked",
     "set copy=false, verify Edit > Copy",
     "copy is no-op"),
    ("encrypt_wrong_user_password",
     "provide wrong user password",
     "open fails gracefully"),
    ("encrypt_owner_only",
     "set owner-only password, open",
     "user access allowed but edit blocked"),
    ("sign_visible", "sign with visible appearance",
     "signed doc has appearance stream"),
    ("sign_invisible", "sign with invisible appearance",
     "signed doc has signature field, no appearance"),
    ("sign_verify_valid",
     "verify valid signature",
     "status == Valid"),
    ("sign_verify_invalid",
     "verify tampered signature",
     "status == Invalid"),
    ("sign_chain_validation",
     "verify full cert chain",
     "chain validates"),
    ("sign_timestamp_present",
     "sign with timestamp, verify",
     "timestamp field populated"),
    ("pdfa_1b_validate",
     "validate test_pdfa_1b.pdf",
     "ValidationResult == Valid"),
    ("pdfa_2b_validate",
     "validate test_pdfa_2b.pdf", "Valid"),
    ("pdfa_2u_validate", "validate 2u", "Valid"),
    ("pdfa_2a_validate", "validate 2a", "Valid"),
    ("pdfa_3b_validate", "validate 3b", "Valid"),
    ("pdfa_1b_convert",
     "convert regular PDF to PDF/A-1b",
     "result validates as 1b"),
    ("pdfa_2b_convert", "convert to 2b", "validates as 2b"),
    ("pdfa_3b_convert", "convert to 3b", "validates as 3b"),
    ("pdfua_validate", "validate tagged PDF", "Valid UA"),
    ("pdfua_convert", "convert to UA-1",
     "result validates as UA-1"),
    ("compliance_report_ui",
     "open ComplianceDialog, pick level, analyze",
     "IssueTable populated"),
    ("security_password_strength_meter",
     "type weak password in EncryptionDialog",
     "strength label shows 'Weak'"),
]
for test_id, action, expected in SECURITY_TESTS:
    # Per-row Qt gotcha notes for the high-risk security flows.
    note = "Verify on-disk file state after save, not just UI."
    if "encrypt_" in test_id and "_reopen" in test_id:
        note = ("Modal PasswordDialog intercepts main window; use "
                "Application.windows() to find it by title match, "
                "type password, click okButton. Retry on clipboard "
                "race per commit 8371c81.")
    elif "encrypt_wrong_user" in test_id:
        note = ("Wrong password produces error dialog; assert on "
                "that, not on main window state.")
    elif "encrypt_permissions" in test_id:
        note = ("Permission checks are advisory only — MuPDF "
                "honors them for UI state. Verify action is "
                "disabled, not that it silently no-ops.")
    elif "encrypt_owner_only" in test_id:
        note = ("Owner-only means empty user password; blank "
                "password must be tried before prompting user.")
    elif "sign_visible" in test_id or "sign_invisible" in test_id:
        note = ("Signing requires a test PFX cert. Fixture: "
                "tests/fixtures/signing/test.pfx with known "
                "password.")
    elif "sign_verify" in test_id:
        note = ("Verification uses PKCS#7 byte-range check. Modify "
                "the signed file to simulate tamper for negative "
                "test.")
    elif "sign_chain" in test_id:
        note = ("Chain validation needs OS trust store + test CA. "
                "May require importing a test root cert.")
    elif "sign_timestamp" in test_id:
        note = ("Timestamp needs TSA URL; use a local mock TSA "
                "server or cache a fixture response.")
    elif "pdfa_" in test_id and "_validate" in test_id:
        note = ("ComplianceDialog Analyze button populates "
                "IssueTable; assert table row count == 0 for pass.")
    elif "pdfa_" in test_id and "_convert" in test_id:
        note = ("Convert writes a new file; re-open and validate "
                "via the same dialog to confirm conformance.")
    elif "pdfua_" in test_id:
        note = ("PDF/UA conversion also adds tagged structure. "
                "Verify StructureTreeRoot exists post-convert.")
    elif "compliance_report_ui" in test_id:
        note = ("IssueTable is a QTableWidget; read row count via "
                "child_window.item_count() or walk rows.")
    elif "password_strength" in test_id:
        note = ("passwordStrengthLabel updates on textChanged; "
                "poll after type_keys not during.")
    add(subsystem="Security", category="Flow",
        element=f"sec_{test_id}",
        test_id=f"sec_{test_id}",
        preconditions="doc open (or encrypted fixture loaded)",
        action=action, expected=expected, tool="pywinauto",
        priority="P1", status="planned", owner_phase="phase-8",
        fixture="(various)", notes=note)

# ---------------------------------------------------------------------------
# Phase 9 — P2 text editing rich formatting (~30 tests)
# ---------------------------------------------------------------------------
TEXT_FMT_DIMENSIONS = [
    ("bold_on", "toggle bold ON", "run has bold=true"),
    ("bold_off", "toggle bold OFF", "run has bold=false"),
    ("italic_on", "toggle italic ON", "run has italic=true"),
    ("italic_off", "toggle italic OFF", "italic=false"),
    ("underline_on", "toggle underline ON", "underline=true"),
    ("underline_off", "toggle underline OFF", "underline=false"),
    ("strike_on", "toggle strikethrough ON", "strikethrough=true"),
    ("strike_off", "toggle strikethrough OFF", "strikethrough=false"),
    ("font_arial", "set font Arial", "fontFamily=Arial"),
    ("font_times", "set font Times", "fontFamily=Times"),
    ("font_courier", "set font Courier", "fontFamily=Courier"),
    ("font_helvetica", "set font Helvetica", "fontFamily=Helvetica"),
    ("size_8", "set size 8pt", "fontSize=8"),
    ("size_12", "set size 12pt", "fontSize=12"),
    ("size_18", "set size 18pt", "fontSize=18"),
    ("size_24", "set size 24pt", "fontSize=24"),
    ("size_36", "set size 36pt", "fontSize=36"),
    ("color_red", "set color red", "fontColor=#FF0000"),
    ("color_blue", "set color blue", "fontColor=#0000FF"),
    ("color_green", "set color green", "fontColor=#00FF00"),
    ("color_custom", "set color via dialog", "fontColor matches picked"),
    ("align_left", "set alignment left", "alignment=Left"),
    ("align_center", "set alignment center", "alignment=Center"),
    ("align_right", "set alignment right", "alignment=Right"),
    ("align_justify", "set alignment justify", "alignment=Justify"),
    ("combo_bold_italic",
     "toggle bold+italic", "both flags true"),
    ("combo_italic_underline",
     "toggle italic+underline", "both flags true"),
    ("combo_all_decor",
     "toggle bold+italic+underline+strike",
     "all four flags true"),
    ("tre_edit_regular_font",
     "edit text on image-based PDF with regular font",
     "bitmap regenerated with new text"),
    ("tre_edit_arabic",
     "edit Arabic text on image-based PDF",
     "bitmap regenerated"),
    ("tre_edit_cjk",
     "edit CJK text on image-based PDF",
     "bitmap regenerated"),
    ("tre_edit_size_change",
     "change font size during edit on image-based PDF",
     "bitmap regenerated with new size"),
]
for test_id, action, expected in TEXT_FMT_DIMENSIONS:
    add(subsystem="TextEdit", category="Flow",
        element=f"txt_{test_id}",
        test_id=f"txt_{test_id}",
        preconditions="editable span selected",
        action=action, expected=expected, tool="pywinauto",
        priority="P2", status="planned", owner_phase="phase-9",
        fixture="test_3page.pdf or test_scanned.pdf",
        notes="TRE variants use image-based fixture")

# ---------------------------------------------------------------------------
# Phase 10 — P2 canvas coordinate tests (~15 tests)
# ---------------------------------------------------------------------------
CANVAS_TESTS = [
    ("annot_handle_drag_corner",
     "drag top-left handle of a selected annotation",
     "annotation bbox resized"),
    ("annot_handle_drag_edge",
     "drag edge handle",
     "annotation resized in one dimension"),
    ("annot_move_drag",
     "drag annotation body",
     "annotation position changed"),
    ("text_selection_drag",
     "click-drag across text",
     "selection highlight covers the range"),
    ("text_selection_double_click",
     "double-click word", "word selected"),
    ("text_selection_triple_click",
     "triple-click line", "line selected"),
    ("canvas_pan_drag",
     "middle-click drag", "viewport pans"),
    ("canvas_scroll_wheel",
     "scroll wheel", "viewport scrolls"),
    ("canvas_zoom_wheel",
     "Ctrl+scroll", "zoom level changes"),
    ("canvas_click_away_deselect",
     "click empty canvas area",
     "any selection is cleared"),
    ("canvas_rubber_band_select",
     "drag rectangle over annotations",
     "all enclosed annotations selected"),
    ("annot_handle_drag_rotate",
     "drag rotate handle",
     "annotation rotation changes"),
    ("sticky_note_drag_popup",
     "drag sticky note body with popup open",
     "popup moves with note"),
    ("canvas_double_click_edit",
     "double-click text span",
     "text edit mode activated"),
    ("canvas_tab_navigate_fields",
     "Tab through fields",
     "focus advances across form fields"),
]
for test_id, action, expected in CANVAS_TESTS:
    add(subsystem="Canvas", category="Flow",
        element=f"canvas_{test_id}",
        test_id=f"canvas_{test_id}",
        preconditions="doc open", action=action, expected=expected,
        tool="PyAutoGUI", priority="P2", status="planned",
        owner_phase="phase-10",
        fixture="test_3page.pdf",
        notes="UIA cannot reach canvas interior; use pixel coords")

# ---------------------------------------------------------------------------
# Phase 4 — QAction-grounded coverage layer (objectName source-of-truth)
# ---------------------------------------------------------------------------
# Pipeline c4a9e271 phase-4 finding: the friendly-path menu_* rows above are
# aspirational ("File > Open"), not grounded to the stable QAction objectName
# that the GAP-A14 testToolActivatePipe seam (MainWindow::activateToolBy
# ObjectName, locked in phase-3) actually drives on Server-2019 CI. The
# audit (tests/docs/ui_coverage_audit_phase4.md) found 131 named QActions in
# MainWindow.cpp but only 35 referenced anywhere in the matrix by objectName
# — a 96-action grounding gap. This block scrapes EVERY QAction from source
# (same philosophy as the DIALOG_SOURCES scraper) and emits one objectName-
# grounded invocation-smoke row per action so coverage tracks the real UI
# surface and cannot silently drift as actions are added/renamed.

MAINWINDOW_SRC = Path(__file__).resolve().parents[3] / "src" / "ui" / "MainWindow.cpp"

QACTION_RE = re.compile(
    r'setObjectName\(\s*(?:QStringLiteral\()?\s*"([A-Za-z0-9_]*Action)"')


def scrape_qactions() -> List[str]:
    """Every setObjectName("...Action") in MainWindow.cpp, source order,
    de-duplicated. Mirrors scrape_widget_names() so the matrix stays in
    sync with the actual source instead of a hand-maintained list."""
    if not MAINWINDOW_SRC.exists():
        raise SystemExit(f"MainWindow.cpp missing for QAction scrape: "
                         f"{MAINWINDOW_SRC}")
    text = MAINWINDOW_SRC.read_text(encoding="utf-8", errors="replace")
    names: List[str] = []
    seen = set()
    for m in QACTION_RE.finditer(text):
        nm = m.group(1)
        if nm in seen:
            continue
        seen.add(nm)
        names.append(nm)
    return names


def classify_action(nm: str) -> tuple[str, str, str]:
    """Map a QAction objectName to (domain, owner_phase, priority).

    Domain groupings + batch allocation come straight from the phase-4
    audit's domain-allocation plan. Priority encodes defect-risk tier:
    P0 = data-integrity / destructive / security-critical, P1 = standard
    interactive, P2 = cosmetic / informational.
    Ordered rules; first match wins. The final fallback guarantees total
    coverage so a newly-added action never silently lands without a row.
    """
    if nm.startswith("textEdit") or nm in ("treToolAction", "treAction"):
        return ("text-edit-tre", "phase-6", "P1")
    if nm.startswith("form") or nm in (
            "createFormFieldAction", "exportFormDataAction",
            "importFormDataAction", "resetFormAction", "flattenFormAction",
            "tabOrderAction", "calculationOrderAction", "submitFormAction"):
        # flatten is destructive (fields -> static content)
        pri = "P0" if nm == "flattenFormAction" else "P1"
        return ("forms", "phase-6", pri)
    if "edact" in nm:  # redactToolAction / markRedactionAction / applyRedactionsAction
        pri = "P0" if nm == "applyRedactionsAction" else "P1"  # apply is irreversible
        return ("redaction", "phase-6", pri)
    if nm in ("selectToolAction", "stickyNoteToolAction", "highlightToolAction",
              "underlineToolAction", "strikeOutToolAction", "inkToolAction",
              "freeTextToolAction", "rectangleToolAction", "circleToolAction",
              "lineToolAction", "stampToolAction", "fileAttachmentToolAction",
              "importImageStampAction", "toggleStickyNotesAction"):
        return ("annotations", "phase-6", "P1")
    if nm.startswith("image") or nm in (
            "insertImageAction", "stripExifAction", "imageToolAction",
            "cropToolAction", "convertToGrayscaleAction", "watermarkAction",
            "headerFooterAction"):
        return ("image-content", "phase-6", "P1")
    if nm in ("saveWithEncryptionAction", "documentPermissionsAction",
              "removePasswordAction", "signDocumentAction",
              "verifySignaturesAction", "requestValidationAction",
              "trustAnchorAction"):
        pri = "P0" if nm in ("saveWithEncryptionAction", "signDocumentAction",
                             "removePasswordAction") else "P1"
        return ("encryption-signatures", "phase-7", pri)
    if nm in ("complianceAction", "preflightAction", "accessibilityAction",
              "altTextAction", "openAltTextDialogAction"):
        return ("compliance", "phase-7", "P1")
    if nm in ("preferencesAction", "customizeToolbarsAction", "themeToggleAction",
              "themeLightAction", "themeDarkAction", "themeSystemAction",
              "toggleCopyPreventionAction", "togglePrintPreventionAction",
              "toggleOcrBoxesAction"):
        return ("preferences", "phase-7", "P1")
    if nm in ("ocrAction", "batchAction", "compareAction", "optimizePdfAction"):
        return ("tools", "phase-7", "P1")
    if nm in ("zoomInAction", "zoomOutAction", "fitWidthAction", "fitPageAction",
              "actualSizeAction", "fullScreenAction", "previousViewAction",
              "nextViewAction", "togglePagePanelAction",
              "togglePropertiesPanelAction", "toggleSearchPanelAction",
              "toggleBookmarkPanelAction", "toggleAttachmentPanelAction"):
        return ("view-nav", "phase-5", "P1")
    if nm in ("findAction", "replaceAction"):
        return ("search", "phase-5", "P1")
    if nm in ("rotatePageCWAction", "rotatePageCCWAction", "rotatePage180Action",
              "deletePageAction", "insertBlankPageAction",
              "insertPagesFromFileAction", "extractPagesAction",
              "movePageUpAction", "movePageDownAction", "replacePageAction",
              "pageLabelsAction"):
        pri = "P0" if nm == "deletePageAction" else "P1"  # delete loses content
        return ("page-ops", "phase-5", pri)
    if nm in ("undoAction", "redoAction", "copyAction", "cutAction",
              "pasteAction", "selectAllAction"):
        return ("edit-clipboard", "phase-5", "P1")
    if nm in ("aboutAction", "documentationAction", "reportIssueAction",
              "checkUpdatesAction"):
        return ("help", "phase-5", "P2")
    if nm in ("newFileAction", "openAction", "saveAction", "saveAsAction",
              "closeAction", "exitAction", "printAction", "printPreviewAction",
              "exportPlainTextAction", "createPdfFromImagesAction",
              "combineFilesAction", "documentMetadataAction"):
        pri = "P0" if nm in ("openAction", "saveAction", "saveAsAction") else "P1"
        return ("file-ops", "phase-5", pri)
    # Total-coverage fallback: any future action lands here, not nowhere.
    return ("misc-tools", "phase-7", "P2")


QACTIONS = scrape_qactions()
if len(QACTIONS) < 100:
    raise SystemExit(
        f"Only {len(QACTIONS)} QActions scraped from MainWindow.cpp; "
        f"expected >= 100 — check the scrape regex / source path")

for nm in QACTIONS:
    domain, phase, pri = classify_action(nm)
    add(subsystem="Actions", category="Action",
        element=f"action::{nm}",
        test_id=f"act_{nm}",
        preconditions="doc open (test_3page.pdf), PDFE_TEST_PIPES=1",
        action=(f"invoke QAction '{nm}' through the testToolActivatePipe seam "
                f"(activateToolByObjectName) — GAP-A14, never raw UIA "
                f"menu_select"),
        expected=("action fires without crash; checkable actions toggle and "
                  "report checked-state, disabled actions return false "
                  "(no exception, window stays alive)"),
        tool="pywinauto", priority=pri, status="planned",
        owner_phase=phase,
        fixture="test_3page.pdf",
        notes=(f"domain={domain}. objectName-grounded reachability/invocation "
               f"smoke over the pipe seam locked in phase-3. Complements any "
               f"friendly-path menu_* row for the same action; this row is the "
               f"one CI actually drives on Server-2019."))

# Per-line font-size domain row (phase-4 acceptance criterion): link the
# phase-2 native-UI visual proof into the matrix as a first-class entry.
add(subsystem="RenderVerify", category="Flow",
    element="render_per_line_font_size",
    test_id="txt_per_line_font_size_ui",
    preconditions="mixed_size_scan.pdf fixture, PDFE_TEST_PIPES=1",
    action=("open mixed-size scanned PDF, activate TRE text-edit, click the "
            "heading line then a body line, read overlayFontSizePtOverride via "
            "the ;ovsz= readback in textEditSelectionDiag AND the visual "
            "glyph-ink extent inside the active-edit box"),
    expected=("heading per-line overlay size >= 1.5x body size on BOTH the "
              "deterministic ;ovsz= axis and the visual ink-height axis — "
              "proves perLineFontSizePt drives the live overlay, not the "
              "region average (the inert-bug regression guard)"),
    tool="mixed", priority="P1", status="planned", owner_phase="phase-6",
    fixture="mixed_size_scan.pdf",
    notes=("IMPLEMENTED: tests/ui_automation/test_per_line_font_size_ui.py "
           "(pipeline c4a9e271 phase-2). Goldens under "
           "goldens/per_line_font_size/. This row links Goal-A's fix to the "
           "regression matrix."))

# Pipeline d4e7f921 phase-2: toolbar Bold/Italic indicators must reflect the
# selected span's DESCRIPTOR-sourced style (TextSpan.isItalic/isBold) — not a
# font-name keyword parse. Links the live-UI proof that clicking into a span
# whose italic+bold lives ONLY in /FontDescriptor (neutral name) lights both
# toolbar buttons, to the emitSelectionFormatting wiring + resolveToolbarStyle.
add(subsystem="RenderVerify", category="Flow",
    element="render_toolbar_style_indicators",
    test_id="txt_toolbar_style_indicators",
    preconditions="embedded_descriptor_style.pdf fixture, PDFE_TEST_PIPES=1",
    action=("open the descriptor-italic+bold PDF (style-neutral BaseFont, "
            "/Flags 0x40062), activate text-edit, click into the span, then "
            "read the toolbar boldButton/italicButton UIA toggle_state"),
    expected=("both the Bold and Italic toolbar buttons read checked "
              "(toggle_state=1) — proving emitSelectionFormatting carries the "
              "span's descriptor-sourced isBold/isItalic to the toolbar even "
              "though the font NAME has no style keyword"),
    tool="pywinauto", priority="P1", status="planned", owner_phase="phase-2",
    fixture="embedded_descriptor_style.pdf",
    notes=("IMPLEMENTED: tests/ui_automation/test_ui_toolbar_style_indicators.py "
           "(pipeline d4e7f921 phase-2). Locks TextEditTool::resolveToolbarStyle "
           "+ the selectionFormattingChanged->setSelectionFormatting wiring; "
           "gtest test_textedit_toolbar_indicators.cpp covers the same logic "
           "headless."))

# Phase-9 D8-1 fix: editing a vector heading drawn with a SUBSET embedded font
# (whose unused glyph outlines were stripped) must stay visible after commit —
# the discovery-run regression where "Invoice"->"Quote" collapsed to stray
# glyphs because the stripped Q/u/t rendered .notdef. Links the headless
# render-oracle gtest and the live UI render proof into the matrix.
add(subsystem="RenderVerify", category="Flow",
    element="render_vector_heading_commit",
    test_id="txt_vector_heading_commit_visible",
    preconditions="invoice_vector_heading.pdf fixture, PDFE_TEST_PIPES=1",
    action=("activate the vector text-edit tool, double-click the bold "
            "'Invoice' heading (a real vector span on a subset Arial,Bold "
            "font), type 'Quote', and commit; render the heading region "
            "before/after"),
    expected=("committed heading keeps >=60% of its rendered ink — stripped "
              "subset glyphs (Q/u/t) are substituted with the matching system "
              "font (Arial Bold) so the edited word renders fully, instead of "
              "collapsing to .notdef"),
    tool="mixed", priority="P1", status="planned", owner_phase="phase-9",
    fixture="invoice_vector_heading.pdf",
    notes=("IMPLEMENTED phase-9: gtest tests/test_vector_heading_commit.cpp "
           "(headless render-ink oracle + AutoMissingGlyphResolver coverage) "
           "+ live UI tests/ui_automation/test_render_text_edit.py "
           "(test_vector_text_edit_visible / _redo_stays_visible). Fix: "
           "AutoMissingGlyphResolver flags stripped (empty-outline) subset "
           "glyphs as missing and routes them to the system font; "
           "EditTextCommand::applyText gap-fills run segments to full "
           "coverage so embedded-font chars are not dropped on per-run "
           "emission."))

# Pipeline b91f4e83 phase-6: live-canvas pixel proof of the SpecimenA text-edit
# font-identity defects on the embedded italic-serif subset fixture. The
# AUTHORITATIVE per-glyph face check is the gtest test_textedit_font_identity.cpp
# (FreeType on the resolved font file); these two rows link the live render-verify
# proof (test_render_textedit_embedded_font.py) that the result reaches the
# screen — overlay serif+italic+aligned, and the new-char serif+italic.
add(subsystem="RenderVerify", category="Flow",
    element="render_textedit_embedded_overlay",
    test_id="txt_embedded_italic_serif_overlay",
    preconditions="embedded_italic_serif_subset.pdf fixture, PDFE_TEST_PIPES=1",
    action=("zoom in ~165%, activate the text-edit tool, double-click the "
            "embedded serif-italic heading span ('A Tale of Two Collards', "
            "BaseFont SSTISF+PalatinoLTPro BoldItalic), capture the in-place "
            "editor overlay and the pre-edit page render of the same rect"),
    expected=("overlay glyphs are SERIF not sans (defect 1b family — template "
              "correlation vs serif/sans reference faces), ITALIC not upright "
              "(defect 1b slant — directional assert_slant_matches), and in "
              "register with the underlying page text within 16px (defect 2 "
              "geometry — assert_overlay_alignment)"),
    tool="mixed", priority="P1", status="planned", owner_phase="phase-6",
    fixture="embedded_italic_serif_subset.pdf",
    notes=("IMPLEMENTED phase-6: tests/ui_automation/"
           "test_render_textedit_embedded_font.py::"
           "test_overlay_italic_serif_is_serif_italic_and_aligned + oracle "
           "primitives render_oracle.assert_overlay_font_class / "
           "assert_slant_matches / assert_overlay_alignment + "
           "render_drive.capture_overlay_over_underlay. Per-glyph face identity "
           "locked by gtest test_textedit_font_identity.cpp."))

add(subsystem="RenderVerify", category="Flow",
    element="render_textedit_new_char_font",
    test_id="txt_embedded_new_char_serif_italic",
    preconditions="embedded_italic_serif_subset.pdf fixture, PDFE_TEST_PIPES=1",
    action=("zoom in ~165%, activate text-edit, double-click the heading span "
            "(selects 'Two'), type 'z' (absent from the embedded subset so "
            "routed through AutoMissingGlyphResolver to a system fallback), "
            "capture the overlay and isolate the new 'z' glyph by word segment"),
    expected=("the new 'z' renders SERIF (defect 3 family lock — it used to "
              "fall back to sans) AND ITALIC (the correctly-preserved slant); "
              "asserted on the post-edit line (template correlation + slant) "
              "and on the isolated 'z' glyph (directional serif/sans corr)"),
    tool="mixed", priority="P1", status="planned", owner_phase="phase-6",
    fixture="embedded_italic_serif_subset.pdf",
    notes=("IMPLEMENTED phase-6: tests/ui_automation/"
           "test_render_textedit_embedded_font.py::"
           "test_new_char_absent_from_subset_renders_serif_italic + "
           "render_oracle.word_segments / assert_glyph_class. Italic is CORRECT "
           "for the new char (underlying text is italic); only the wrong family "
           "was the bug. Per-glyph face identity locked by gtest "
           "test_textedit_font_identity.cpp."))

# ---------------------------------------------------------------------------
# Phase 6 — batch-2 multi-step scenario proofs (criteria 2 & 4)
# ---------------------------------------------------------------------------
# The per-action act_* rows above cover tool activation + per-element gesture
# survival. Phase-6 criteria 2 (TRE) and 4 (annotations) additionally call out
# deeper multi-step scenarios driven through the production seams. Each scenario
# test in tests/ui_automation/test_ui_batch2_scenarios.py records to one of these
# IDs via conftest.record_matrix_result — so they are first-class owned rows,
# not passthrough, and cannot silently drift.
BATCH2_SCENARIOS = [
    ("ann_freetext_edit_text", "Annotations",
     "create a FreeText annotation then edit its text via the annotation "
     "contents seam (testAnnotSelectPipe + testAnnotContentsPipe)",
     "annotation created, selected and its contents updated; app survives"),
    ("ann_comment_panel_appears", "Annotations",
     "add an annotation and confirm the comment panel inventory row count "
     "increases (testCommentInventoryTriggerPipe readback)",
     "comment-panel inventory reports >= 1 row and a net increase"),
    ("ann_delete", "Annotations",
     "create, select (testAnnotSelectPipe), then delete an annotation via the "
     "Q_INVOKABLE deleteSelectedAnnotation seam (testAnnotDeletePipe)",
     "comment-panel row count drops after the delete; app survives"),
    ("txt_tre_scanned_flow", "TextEdit",
     "scanned-page TRE flow: activate TRE, switch atlas mode, click heading "
     "span and type an edit, undo/redo, then save-and-reopen the edited file",
     "edit renders, undo/redo survive, saved file re-opens with content"),
]
for tid, subsystem, action, expected in BATCH2_SCENARIOS:
    add(subsystem=subsystem, category="Flow",
        element=f"scenario::{tid}",
        test_id=tid,
        preconditions="batch-2 fixture open from working copy, PDFE_TEST_*_PIPES=1",
        action=action, expected=expected,
        tool="mixed", priority="P1", status="planned", owner_phase="phase-6",
        fixture="invoice_vector_heading.pdf / mixed_size_scan.pdf",
        notes=("IMPLEMENTED: tests/ui_automation/test_ui_batch2_scenarios.py "
               "(pipeline c4a9e271 phase-6). Multi-step scenario proof for the "
               "phase-6 annotation (criterion 4) / TRE (criterion 2) acceptance "
               "items; driven through production pipe seams (GAP-A14)."))

# ---------------------------------------------------------------------------
# Phase 7 — batch-3 multi-step scenario proofs (criteria 1, 2 & 3)
# ---------------------------------------------------------------------------
# The per-action act_* rows for batch-3 (encryption/signatures, compliance,
# preferences, tools) are auto-scraped above and driven fire-and-survive by
# tests/ui_automation/test_ui_batch3_actions.py. Phase-7 criteria 1 (sign/
# verify/LTV), 2 (PDF/A + PDF/UA result dialog) and 3 (preferences per-tab
# control-state) additionally call out deeper, matrix-recording proofs.
# These are GAP-A14 reachability + control-state proofs in the batch layer;
# the full behavioural depth lives in the mature phase-12/13/14 suites cited
# per-row. Each test records to one of these IDs via record_matrix_result,
# so they are first-class owned rows, not passthrough.
BATCH3_SCENARIOS = [
    # (test_id, subsystem, category, action, expected, fixture, impl_note)
    ("sec_verify_signed", "Signatures", "Flow",
     "verify a real PKCS#7-signed PDF via verifySignaturesAction and read "
     "the SignaturePanel validation status (testLtvStatus pipe)",
     "verify fires and the panel reports a 'Validation:' status; app survives",
     "test_signed.pdf",
     "tests/ui_automation/test_ui_batch3_scenarios.py; deep round-trip in "
     "test_ui_encryption_signatures.py (phase-12) / test_ui_ltv.py"),
    ("sec_ltv_request_dialog", "Signatures", "Flow",
     "after verify, requestValidationAction opens the LTV consent dialog "
     "(privacy warning + URL list + Cancel/Fetch) read via the "
     "validation-dialog introspection seam; auto-Cancel (no network fetch)",
     "ValidationRequestDialog reachable with urlList; no DSS write; survives",
     "test_signed.pdf",
     "tests/ui_automation/test_ui_batch3_scenarios.py; deep LTV flow in "
     "test_ui_ltv.py"),
    ("cmp_compliance_dialog_levels", "Compliance", "Flow",
     "complianceAction opens ComplianceDialog on a PDF/A fixture exposing "
     "LevelCombo + Analyze/Validate/Convert controls (introspection seam)",
     "ComplianceDialog reachable with Validate+Convert controls; survives",
     "test_pdfa_1b.pdf",
     "tests/ui_automation/test_ui_batch3_scenarios.py; deep convert+validate "
     "in test_ui_compliance_accessibility.py (phase-13)"),
    ("cmp_compliance_target_levels", "Compliance", "Flow",
     "ComplianceDialog LevelCombo lists the PDF/A-1b, PDF/A-2b and PDF/UA "
     "conformance targets (criterion 2 conversion targets + PDF/UA check)",
     "PDF/A-1b, PDF/A-2b and PDF/UA target levels all present in the dialog",
     "test_pdfa_1b.pdf",
     "tests/ui_automation/test_ui_batch3_scenarios.py; deep conversion in "
     "test_ui_compliance_accessibility.py (phase-13)"),
    ("pref_tabs_present", "Preferences", "Flow",
     "PreferencesDialog (preferencesTabs) exposes all five tab pages via the "
     "widget-inventory ||tabs= seam",
     "General, View, Security, OCR and Advanced tabs all present",
     "test_3page.pdf",
     "tests/ui_automation/test_ui_batch3_preferences.py; deep coverage in "
     "test_ui_preferences_chrome.py (phase-14)"),
    ("pref_state_general", "Preferences", "Flow",
     "General tab control-state readback (maxRecentFilesSpinBox) via "
     "testPrefGetValueResultPipe",
     "live value returned for the General-page control; app survives",
     "test_3page.pdf",
     "tests/ui_automation/test_ui_batch3_preferences.py (criterion 3)"),
    ("pref_state_view", "Preferences", "Flow",
     "View tab control-state readback (defaultZoomSpinBox) via "
     "testPrefGetValueResultPipe",
     "live value returned for the View-page control; app survives",
     "test_3page.pdf",
     "tests/ui_automation/test_ui_batch3_preferences.py (criterion 3)"),
    ("pref_state_security", "Preferences", "Flow",
     "Security tab control-state readback (javaScriptEnabledCheck) via "
     "testPrefGetValueResultPipe",
     "live value returned for the Security-page control; app survives",
     "test_3page.pdf",
     "tests/ui_automation/test_ui_batch3_preferences.py (criterion 3)"),
    ("pref_state_ocr", "Preferences", "Flow",
     "OCR tab control-state readback (ocrLanguageCombo) via "
     "testPrefGetValueResultPipe",
     "live value returned for the OCR-page control; app survives",
     "test_3page.pdf",
     "tests/ui_automation/test_ui_batch3_preferences.py (criterion 3)"),
    ("pref_state_advanced", "Preferences", "Flow",
     "Advanced tab control-state readback (logLevelCombo) via "
     "testPrefGetValueResultPipe",
     "live value returned for the Advanced-page control; app survives",
     "test_3page.pdf",
     "tests/ui_automation/test_ui_batch3_preferences.py (criterion 3)"),
    ("pref_set_roundtrip", "Preferences", "Flow",
     "set maxRecentFilesSpinBox=17 via testPrefSetValuePipe then read it back "
     "(control-state mutation round-trip); Cancel so nothing persists",
     "readback reflects the set value; Cancel leaves QSettings untouched",
     "test_3page.pdf",
     "tests/ui_automation/test_ui_batch3_preferences.py; persistence proven "
     "in test_ui_preferences_chrome.py (phase-14)"),
]
for tid, subsystem, category, action, expected, fixture, impl in BATCH3_SCENARIOS:
    add(subsystem=subsystem, category=category,
        element=f"scenario::{tid}",
        test_id=tid,
        preconditions="batch-3 fixture open from working copy, PDFE_TEST_PIPES=1",
        action=action, expected=expected,
        tool="mixed", priority="P1", status="planned", owner_phase="phase-7",
        fixture=fixture,
        notes=("IMPLEMENTED: " + impl + ". Pipeline c4a9e271 phase-7 batch-3 "
               "scenario proof (GAP-A14 pipe seams; reachability/control-state "
               "layer — deep behavioural coverage in the cited mature suite)."))

# ---------------------------------------------------------------------------
# Batch-3 BEHAVIORAL flow rows (phase-7 criteria 1 & 2 remediation).
# These each LAUNCH PDFE, drive the dialog through the GAP-A14 pipe seam,
# and assert the real product of the flow (decrypted doc opens; /ByteRange;
# pdfaid XMP marker; PDF/UA validate result panel). Implemented in
# tests/ui_automation/test_ui_batch3_flows.py — proven leaf drivers reused
# from the phase-12/13 suites, orchestration is pipe-driven and fresh.
# ---------------------------------------------------------------------------
BATCH3_FLOWS = [
    # (test_id, subsystem, category, action, expected, fixture, impl_note)
    ("sec_open_encrypted", "Encryption", "Flow",
     "open an AES-256 encrypted PDF and supply the user password in the "
     "auto-raised PasswordDialog (passwordEdit)",
     "document decrypts and opens (window title resolves to the fixture)",
     "test_encrypted_aes256.pdf",
     "drives PasswordDialog with the known user password; asserts the "
     "document actually opens"),
    ("sec_sign_real", "Signatures", "Flow",
     "open an unsigned-sigfield PDF, drive SignDialog with the test PFX + "
     "password (opened via testToolActivatePipe), drive the native Save "
     "Signed PDF dialog",
     "saved output carries a real PKCS#7 /ByteRange",
     "test_unsigned_sigfield.pdf",
     "real signing flow end-to-end; hard-asserts /ByteRange in the output"),
    ("sec_ltv_enable", "Signatures", "Flow",
     "after verify, requestValidationAction opens the LTV consent dialog; "
     "assert its Fetch button is reachable AND enabled, then Cancel",
     "LTV Fetch enablement reachable; no network fetch / DSS write",
     "test_signed.pdf",
     "asserts the Fetch button is enabled (LTV-enable reachable); cancels"),
    ("cmp_convert_pdfa_1b", "Compliance", "Flow",
     "open ComplianceDialog (pipe seam), select PDF/A-1b, Convert, drive "
     "the Save Compliant PDF dialog",
     "converted output carries pdfaid:part=1 conformance=B in XMP",
     "phase3_v14_basic.pdf",
     "real PDF/A-1b conversion; asserts the pdfaid XMP marker via pikepdf"),
    ("cmp_convert_pdfa_2b", "Compliance", "Flow",
     "open ComplianceDialog (pipe seam), select PDF/A-2b, Convert, drive "
     "the Save Compliant PDF dialog",
     "converted output carries pdfaid:part=2 conformance=B in XMP",
     "phase3_v14_basic.pdf",
     "real PDF/A-2b conversion; asserts the pdfaid XMP marker via pikepdf"),
    ("cmp_validate_pdfua", "Compliance", "Flow",
     "open ComplianceDialog (pipe seam) on a PDF/UA fixture, select "
     "PDF/UA-1, click Validate",
     "the StatusLabel result panel becomes populated with a PDF/UA verdict",
     "test_pdfua.pdf",
     "real PDF/UA check; asserts a visible StatusLabel result"),
]
for tid, subsystem, category, action, expected, fixture, impl in BATCH3_FLOWS:
    add(subsystem=subsystem, category=category,
        element=f"flow::{tid}",
        test_id=tid,
        preconditions="batch-3 fixture open from working copy, PDFE_TEST_PIPES=1",
        action=action, expected=expected,
        tool="mixed", priority="P1", status="planned", owner_phase="phase-7",
        fixture=fixture,
        notes=("IMPLEMENTED: " + impl + ". Pipeline c4a9e271 phase-7 batch-3 "
               "BEHAVIORAL flow — tests/ui_automation/test_ui_batch3_flows.py "
               "launches PDFE and asserts the real artifact (criteria 1 & 2)."))

# ---------------------------------------------------------------------------
# Output
# ---------------------------------------------------------------------------
OUT = Path(__file__).resolve().parent / "ui_test_matrix.csv"

def main() -> int:
    # Sanity: unique test_ids
    ids = [r.test_id for r in ROWS]
    dup = set([i for i in ids if ids.count(i) > 1])
    if dup:
        raise SystemExit(f"Duplicate test IDs: {sorted(dup)}")
    # Acceptance: >= 400 rows
    if len(ROWS) < 400:
        raise SystemExit(
            f"Matrix has only {len(ROWS)} rows; need >= 400")

    # Preserve hand-edited status + notes from the existing CSV, keyed
    # by test_id. Two preservation paths:
    #
    #   1. Rows the generator OWNS (test_id appears in ROWS): we keep
    #      the structural columns from ROWS but copy any non-'planned'
    #      status + non-default notes from the existing CSV. Sweep
    #      gates write 'green'/'red'/'blocked' with diagnostic notes;
    #      regenerate must not silently revert them to 'planned'.
    #
    #   2. Rows the generator does NOT own (test_id only in existing
    #      CSV — typically rows added by broader-matrix/sweep expansions
    #      that live outside this script): we pass them through
    #      verbatim. Dropping them would lose phase-post matrix rows
    #      like open_portfolio_pdf, theme_switch_*, etc. that are not
    #      in the generator's hard-coded lists.
    existing: Dict[str, Dict[str, str]] = {}
    if OUT.exists():
        with OUT.open("r", encoding="utf-8", newline="") as f:
            reader = csv.DictReader(f)
            for prev in reader:
                tid = prev.get("test_id", "")
                if tid:
                    existing[tid] = prev

    generated_ids = {r.test_id for r in ROWS}
    passthrough = [
        prev for tid, prev in existing.items() if tid not in generated_ids
    ]

    # Priority + status invariants already asserted in add().
    with OUT.open("w", encoding="utf-8", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=HEADER, lineterminator="\n")
        writer.writeheader()
        preserved = 0
        for r in ROWS:
            row = asdict(r)
            prev = existing.get(r.test_id)
            if prev is not None:
                prev_status = prev.get("status", "")
                if prev_status and prev_status != "planned":
                    row["status"] = prev_status
                    preserved += 1
                prev_notes = prev.get("notes", "")
                if prev_notes and prev_notes != row["notes"]:
                    row["notes"] = prev_notes
            writer.writerow(row)
        for prev in passthrough:
            writer.writerow({k: prev.get(k, "") for k in HEADER})

    total = len(ROWS) + len(passthrough)
    print(f"Wrote {total} rows to {OUT} "
          f"({len(ROWS)} generated, {len(passthrough)} passthrough, "
          f"{preserved} statuses preserved from prior CSV)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
