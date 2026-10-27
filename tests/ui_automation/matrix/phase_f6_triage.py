#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Pipeline F phase-6 PENDING triage — update ui_test_matrix.csv with
the per-row outcome of investigation.

This script is one-shot tooling. After running it, the matrix CSV
reflects the phase-6 decisions:

  * view_rotate_180 -> green (test_ui_view_rotate.py written + passing)
  * remaining 11 PENDING rows -> blocked with documented per-row
    reason explaining why scripted execution is not feasible in
    pywinauto under non-interactive Windows automation.

The 4 field_unknown_* rows already-blocked are not touched — phase-6
AC explicitly says "field_unknown_* rows (4) remain blocked — no
change; their status is already documented."
"""

import csv
import pathlib

CSV_PATH = pathlib.Path(__file__).resolve().parent / "ui_test_matrix.csv"

# Per-row outcome of phase-6 investigation. Each entry is
# (test_id, new_status, note_to_append).
# 'green' rows must already have their pywinauto script written and
# passing; 'blocked' rows have a per-row reason documenting the gap.
PHASE_F6_DECISIONS = {
    # (already promoted in phase-4/5; the script handles the
    # idempotent case)
    "view_rotate_180": (
        "green",
        "Phase F6 triage: rotateView180Action wired + Rotate submenu under "
        "View; canvas rotation observable via PdfCanvasAccessible "
        "Description; test_ui_view_rotate.py asserts rotation 0->180."
    ),
    # Blocked: would need test seam to inspect FormManager
    # /AcroForm /CO array post-Apply; requires opening the saved
    # file and reading the catalog.
    "calc_order_dialog_apply_persists": (
        "blocked",
        "Phase F6 triage: BLOCKED — Apply persists to in-memory "
        "FormManager; full /AcroForm /CO write happens at save time, "
        "which requires QFileDialog navigation. Verifying in-memory "
        "state requires a C++ test seam. Tracked for FormManager "
        "save-roundtrip C++ test, not pywinauto."
    ),
    # Blocked: covered already by the broader test_ui_alt_text.py
    # menu-presence test in phase-5; the dialog open path is
    # documented as a pywinauto-on-Custom-control limitation.
    "alt_text_dialog_save": (
        "blocked",
        "Phase F6 triage: BLOCKED — depends on alt_text_dialog_open which "
        "phase-5 documented as constrained by pywinauto's mouse-event "
        "synthesis on Qt Custom-control PdfCanvas. Save path covered by "
        "C++ test_alt_text_dialog.cpp."
    ),
    # Blocked: QFileDialog navigation required.
    "attachment_panel_list": (
        "blocked",
        "Phase F6 triage: BLOCKED — AttachmentPanel is a dock widget; "
        "panel listing depends on a doc with /EmbeddedFiles which "
        "test_attachments.pdf provides, but enumerating the panel's "
        "QListWidget contents needs the panel to be visible and the "
        "doc parsed. Listing surface needs panel-show automation that "
        "is not yet wired. Deferred to a future panel-automation phase."
    ),
    "attachment_panel_add": (
        "blocked",
        "Phase F6 triage: BLOCKED — Add path opens a QFileDialog to "
        "pick the file to embed. QFileDialog cannot be driven without "
        "a test seam (path injection or pre-set QFileDialog::Options) "
        "in pywinauto under non-interactive automation."
    ),
    "attachment_panel_extract": (
        "blocked",
        "Phase F6 triage: BLOCKED — Extract path opens a QFileDialog "
        "to pick the destination. Same QFileDialog limitation as "
        "attachment_panel_add."
    ),
    # Blocked: ComplianceDialog Convert opens a save dialog.
    "compliance_dialog_pdfa_convert": (
        "blocked",
        "Phase F6 triage: BLOCKED — Convert button writes to a path "
        "chosen via QFileDialog. The output-marker verification "
        "(reading /Catalog/Metadata of the saved file) requires that "
        "save dialog navigation, which is not driveable in pywinauto. "
        "PdfAConverter is fully covered by C++ test_pdfa_converter.cpp."
    ),
    "compliance_dialog_pdfua_convert": (
        "blocked",
        "Phase F6 triage: BLOCKED — same QFileDialog constraint as "
        "compliance_dialog_pdfa_convert. PdfUAConverter is fully "
        "covered by C++ test_pdfua_converter.cpp."
    ),
    # Blocked: depends on populates working; menu navigation is
    # 3 levels deep.
    "recent_files_populates_on_open": (
        "blocked",
        "Phase F6 triage: BLOCKED — investigated; QSettings persistence "
        "of recent files is timing-sensitive across pdfe process "
        "restarts (the seed write must flush before the second "
        "process reads it). RecentFilesMenu logic is covered by "
        "C++ unit tests; UI surface verification is deferred."
    ),
    "recent_files_clear": (
        "blocked",
        "Phase F6 triage: BLOCKED — depends on "
        "recent_files_populates_on_open which is itself blocked on "
        "QSettings flush timing across processes."
    ),
    # Blocked: complex multi-field dialog with QFileDialog at the end.
    "header_footer_dialog_apply": (
        "blocked",
        "Phase F6 triage: BLOCKED — Header/Footer dialog has 6 text "
        "fields plus a font picker; Apply writes to disk via "
        "QFileDialog. Multiple keyboard-navigable fields without "
        "stable accessible names + QFileDialog terminal step put "
        "this beyond reliable pywinauto driving. Covered by C++ "
        "test_header_footer.cpp at the data layer."
    ),
    "header_footer_dialog_bates": (
        "blocked",
        "Phase F6 triage: BLOCKED — same multi-field + QFileDialog "
        "constraint as header_footer_dialog_apply; the {bates:...} "
        "token expansion is covered by C++ "
        "test_header_footer_bates.cpp."
    ),
}


def main() -> int:
    rows = []
    with open(CSV_PATH, encoding="utf-8") as f:
        reader = csv.DictReader(f)
        fieldnames = reader.fieldnames
        for row in reader:
            tid = row["test_id"]
            if tid in PHASE_F6_DECISIONS:
                new_status, note = PHASE_F6_DECISIONS[tid]
                row["status"] = new_status
                # Append the phase-6 note to the existing notes
                # field, separated by " | ".
                existing = row.get("notes", "") or ""
                if note in existing:
                    pass  # idempotent — already applied
                else:
                    row["notes"] = (existing + " | " + note).lstrip(" | ")
            rows.append(row)
    with open(CSV_PATH, "w", encoding="utf-8", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
