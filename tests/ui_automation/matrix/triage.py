# -*- coding: utf-8 -*-
"""Triage all currently-blocked matrix rows into remediation buckets.

Writes:
  - tests/ui_automation/matrix/blocked_triage.csv

Bucket assignments follow docs/*.md gap references and classify each
row as one of:
  trivial-wiring  — Tier-1 menu/handler wiring (phase-2)
  fixture-gen     — missing fixture (phase-3)
  canvas-click    — depends on canvas-click harness fix (phase-4)
  oracle-hook     — needs product-side test-diagnostic hook (phase-5)
  medium-feature  — color picker, multi-select, popup focus (phase-6)
  wont-fix        — formally closed, not addressed
  already-resolved — matrix stale, row already green-able
"""
from __future__ import annotations
import csv
from collections import Counter
from pathlib import Path

MATRIX = Path("tests/ui_automation/matrix/ui_test_matrix.csv")
OUT = Path("tests/ui_automation/matrix/blocked_triage.csv")


def classify(r):
    tid = r["test_id"]
    ph = r["owner_phase"]
    el = (r.get("element") or "").lower()

    # ---- WONT-FIX: large features, obsolete, matrix debt ----
    wontfix_map = {
        "menu_tools_batch": "large feature (BatchProcess) deferred post-1.0",
        "menu_tools_compare": "large feature (Compare) deferred post-1.0",
        "menu_submit_form": "Forms > SubmitForm deferred — no HTTP submit path",
        "menu_tab_order": "Forms > TabOrder deferred — no tab-order UI",
        "menu_view_theme_toggle": "theme toggle deferred — no stylesheet engine",
        "menu_rotate_view": "RotateView deferred — Pages menu covers the semantic need",
        "menu_preferences_open": "Preferences dialog deferred — AppSettings has no UI yet",
        "sec_security_password_strength_meter": "password strength meter widget does not exist",
        "sec_sign_chain_validation": "CA-signed cert needed — phase-8 scope",
        "sec_sign_timestamp_present": "TSA integration not in pdfe SignDialog",
        "sec_sign_verify_invalid": "tampered-signature fixture not in corpus",
        "sec_encrypt_permissions_print_blocked": "multi-stage relaunch harness not built",
        "sec_encrypt_permissions_copy_blocked": "multi-stage relaunch harness not built",
        "page_large_doc_rotate": "no 100-page fixture in corpus — stress test scope",
        "page_large_doc_delete_range": "no 100-page fixture in corpus",
        "page_insert_from_encrypted": "dialog layering not mapped — phase-8 UI scope",
        "page_extract_to_encrypted_target": "phase-8 encryption UI scope",
        "page_page_resize_ignored": "requires parsing saved PDF byte state",
        "page_cropbox_preserved": "requires parsing saved PDF byte state",
        "page_rotate_respects_annotations": "requires canvas annotation + byte-state inspection",
        "page_insert_blank_before": "pdfe has no insert-before-position menu action",
        "page_insert_blank_end": "pdfe has no insert-at-end menu action",
        "page_insert_from_file_range": "nested page-range dialog not mapped",
        "page_undo_move": "page move op itself is blocked",
        "canvas_text_selection_triple_click": "Qt triple-click timing unreliable via pywinauto",
        "canvas_canvas_rubber_band_select": "marquee not UIA-observable",
        "canvas_annot_handle_drag_rotate": "rotate handles painted, not discoverable via UIA",
        "flow_flow_save_as_new": "near-duplicate of flow_flow_open_save_as (already green)",
        "flow_flow_save_as_overwrite": "near-duplicate of flow_flow_open_save_as",
        "flow_flow_print_to_file": "native print dialog — low leverage",
        "flow_flow_print_cancel": "native print dialog — low leverage",
        "flow_flow_corpus_robustness": "delegates to test_pdf_open_robustness.py",
    }
    if tid in wontfix_map:
        return "wont-fix", "phase-1", wontfix_map[tid]

    # Alignment — text formatting deferred
    if tid.startswith("txt_align_"):
        return "wont-fix", "phase-1", "text alignment requires FormattingBlock model"

    # Push-button, Unknown form field, RC4/AES-128
    if tid.startswith("field_pushbutton_"):
        return "wont-fix", "phase-1", "PushButton has no value to round-trip"
    if tid.startswith("field_signature_") and ph == "phase-6":
        return "wont-fix", "phase-1", "Signature state not in XFDF"
    if tid.startswith("field_unknown_"):
        return "wont-fix", "phase-1", "Unknown is matrix-debt, not a real PDF field type"
    if any(s in tid for s in ("rc4_40", "rc4_128", "aes_128")) and "encrypt" in tid:
        return "wont-fix", "phase-1", "pdfe EncryptionDialog offers AES-256 only"

    # PDF/UA
    if tid.startswith("sec_pdfua_"):
        return "wont-fix", "phase-1", "PDF/UA pipeline not wired in pdfe ComplianceDialog"

    # page_move_* rendered-order weak oracle
    if tid.startswith("page_move_"):
        return "wont-fix", "phase-1", "move success observable only via visual diff"
    if tid.startswith("page_drag_drop_reorder_") or tid.startswith("page_multi_select_"):
        return "wont-fix", "phase-1", "QListWidget drag/multi-select unreliable via pywinauto"

    # PDF/A convert blocks not already convert-able
    if tid.startswith("sec_pdfa_") and tid.endswith("_convert"):
        return "wont-fix", "phase-1", "test_3page.pdf doesn't meet PDF/A preconditions; fixture-gen needed but low leverage"

    # ---- TRIVIAL WIRING — phase-2 ----
    if tid in ("menu_copy_basic", "menu_select_all", "menu_cut_basic", "menu_paste_basic"):
        return "trivial-wiring", "phase-2", "Edit > Copy/Cut/Paste/SelectAll — TextSelector + QClipboard"
    if tid == "menu_open_new_document":
        return "trivial-wiring", "phase-2", "File > New — Document::createEmpty + onNewFile"
    if tid == "menu_replace_open":
        return "trivial-wiring", "phase-2", "Edit > Replace — SearchPanel extension"
    if tid in ("menu_image_remove", "menu_image_rotate", "menu_image_crop", "menu_image_resize"):
        return "trivial-wiring", "phase-2", "Image menu — wire ImageManager/ImageContentEditor"
    if tid in ("menu_tools_ocr", "menu_tools_tre", "menu_tools_accessibility"):
        return "trivial-wiring", "phase-2", "Tools menu — wire OcrPipeline / TRE / AccessibilityHelper"

    # ---- FIXTURE GEN — phase-3 ----
    if tid.endswith("_set_default") and ph == "phase-6":
        return "fixture-gen", "phase-3", "needs test_form_with_dv.pdf"
    if tid.startswith("sec_pdfa_") and tid.endswith("_validate"):
        return "fixture-gen", "phase-3", "needs test_pdfa_*.pdf"
    if tid == "sec_sign_verify_valid":
        return "fixture-gen", "phase-3", "needs persisted signed fixture"
    if tid == "flow_flow_open_pdfa":
        return "fixture-gen", "phase-3", "needs test_pdfa_1b.pdf"

    # ---- CANVAS-CLICK HARNESS — phase-4 ----
    if ph == "phase-5" and (tid.endswith("_create") or tid.endswith("_delete")) and not tid.startswith("annot_select_"):
        return "canvas-click", "phase-4", "annotation create/delete depends on canvas click"
    if ph == "phase-6" and tid.endswith("_create") and not any(
        tid.startswith(p) for p in ("field_pushbutton_", "field_signature_", "field_unknown_")
    ):
        return "canvas-click", "phase-4", "form field create requires canvas drag-placement"
    if ph == "phase-9" and tid.startswith("txt_tre_edit_"):
        return "canvas-click", "phase-4", "TRE edit requires canvas span click"
    if tid in ("canvas_annot_handle_drag_corner", "canvas_annot_handle_drag_edge"):
        return "canvas-click", "phase-4", "handle drag oracle via annotRectLabel (phase-5) + canvas click"
    if tid == "canvas_text_selection_drag":
        return "canvas-click", "phase-4", "drag + clipboard oracle with better fixture coords"
    if tid in ("flow_flow_text_edit_commit", "flow_flow_text_edit_cancel"):
        return "canvas-click", "phase-4", "text edit roundtrip"
    if tid == "flow_flow_annot_create_save":
        return "canvas-click", "phase-4", "annotation create flow"
    if tid in ("flow_flow_undo_redo", "flow_flow_close_dirty"):
        return "canvas-click", "phase-4", "needs prior content edit"
    if tid == "flow_flow_form_fill_save":
        return "canvas-click", "phase-4", "form field fill via canvas click"

    # ---- ORACLE HOOKS — phase-5 ----
    if ph == "phase-3":
        # Stacked-widget page dialogs
        if any(k in el for k in ("formfieldpropertiespanel", "imagepropertiespanel", "annotationpropertieswidget")):
            return "oracle-hook", "phase-5", "QStackedWidget page needs test-diagnostic API"
        if "atlascomparisondialog" in el:
            return "oracle-hook", "phase-5", "atlasComparisonDialog via canvas-click + hook"
        if "redactionpanel" in el:
            return "oracle-hook", "phase-5", "redactionPanel populates after workflow"
        if "signaturepanel" in el:
            return "oracle-hook", "phase-5", "signaturePanel populates after workflow"
        if "passworddialog" in el:
            return "already-resolved", "phase-1", "passwordDialog already reachable via encrypted fixture"
        # QAction / QButtonGroup / hidden-tab widgets — not oracleable
        return "wont-fix", "phase-1", "QAction/QButtonGroup/hidden page not exposed to UIA"

    if ph == "phase-5" and any(tid.endswith(s) for s in ("_set_color", "_set_opacity", "_set_contents")):
        return "oracle-hook", "phase-5", "needs Properties Panel stacked-page selection"

    # annot_select_delete — SelectTool non-create op
    if tid == "annot_select_delete":
        return "wont-fix", "phase-1", "SelectTool is not a configurator; matrix debt"
    # sign_invisible — minor variant of sign_visible
    if tid == "sec_sign_invisible":
        return "wont-fix", "phase-1", "invisible sign appearance is a checkbox variant of sign_visible (already green)"
    # Font names not exact-matched by QFontComboBox
    if tid in ("txt_font_times", "txt_font_courier", "txt_font_helvetica"):
        return "wont-fix", "phase-1", "QFontComboBox exact-match against system font name failed at test time — Arial covers the green case"

    # ---- MEDIUM FEATURE — phase-6 ----
    if tid.startswith("txt_color_"):
        return "medium-feature", "phase-6", "add textColorButton to TextEditToolBar"
    if tid == "canvas_sticky_note_drag_popup":
        return "medium-feature", "phase-6", "StickyNotePopup focus override"
    if tid in ("page_delete_range", "page_extract_range", "page_extract_noncontiguous"):
        return "medium-feature", "phase-6", "multi-select harness helper"
    if tid.startswith("page_rotate_") and "multi" in tid:
        return "medium-feature", "phase-6", "multi-select harness helper"

    return "UNCLASSIFIED", "phase-1", "needs manual review"


def main() -> int:
    rows = list(csv.DictReader(open(MATRIX, encoding="utf-8")))
    blocked = [r for r in rows if r["status"] == "blocked"]

    triage = []
    for r in blocked:
        bucket, rem_phase, reason = classify(r)
        triage.append({
            "test_id": r["test_id"],
            "phase_origin": r["owner_phase"],
            "subsystem": r["subsystem"],
            "element": r["element"],
            "bucket": bucket,
            "remediation_phase": rem_phase,
            "reason": reason,
        })

    OUT.parent.mkdir(parents=True, exist_ok=True)

    bucket_counts = Counter(r["bucket"] for r in triage)
    remphase_counts = Counter(r["remediation_phase"] for r in triage)

    with OUT.open("w", encoding="utf-8", newline="") as f:
        # Header metadata as commented first row (CSV comment via "#" prefix)
        f.write(f"# Blocked row triage — total {len(triage)} rows\n")
        for b, n in sorted(bucket_counts.items(), key=lambda x: -x[1]):
            f.write(f"# {b}: {n}\n")
        w = csv.DictWriter(
            f,
            fieldnames=["test_id", "phase_origin", "subsystem",
                        "element", "bucket", "remediation_phase", "reason"],
        )
        w.writeheader()
        for r in triage:
            w.writerow(r)

    print(f"Total blocked: {len(triage)}")
    print("By bucket:")
    for b, n in sorted(bucket_counts.items(), key=lambda x: -x[1]):
        print(f"  {b:20s} {n:4d}")
    print("By remediation phase:")
    for p, n in sorted(remphase_counts.items()):
        print(f"  {p:10s} {n:4d}")

    unclassified = [r for r in triage if r["bucket"] == "UNCLASSIFIED"]
    if unclassified:
        print(f"\nUNCLASSIFIED ({len(unclassified)}):")
        for r in unclassified:
            print(f"  {r['test_id']} ({r['phase_origin']}) {r['element']}")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
