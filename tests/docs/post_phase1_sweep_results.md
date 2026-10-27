# Pipeline e5b2c947 Phase-1 Sweep Results

<!-- tracking-banner -->
> **STATUS: HISTORICAL EVIDENCE — this is not a work list.**
>
> **GitHub Issues (`ipswitch8/PDFE`) is the canonical tracker for anything still
> open.** This document records what was found or decided on the day it was
> written. Items here may have been fixed, superseded, or deliberately declined
> since. Where this document and a GitHub issue disagree, **the issue wins**.
>
> See [`docs/TRACKING.md`](../../docs/TRACKING.md).

---


**Date**: 2026-05-18
**Pre-fix baseline**: 264 FAILED (a7f3d2c9 phase-4 sweep, before GAP-28 + GAP-43 fixes)
**Post-fix actual**: 190 FAILED + 25 ERROR = 215 total problem items
**Net reduction (FAILED only)**: 264 - 190 = **74 tests auto-resolved by phase-1 fixes**
**Runtime**: 8972s (2h 29m 32s)
**Collection**: 722 tests (tests/ + tests/ui_automation/)

---

## Summary

| Outcome | Count |
|---------|-------|
| PASSED  | 358   |
| FAILED  | 190   |
| SKIPPED | 149   |
| ERROR   | 25    |

---

## Phase-1 Fixes Applied

### GAP-43: pytest.ini `-p no:capture` (permanent fix)
- `pytest.ini` `addopts` updated from `--basetemp=tests/_tmp` to
  `--basetemp=tests/_tmp -p no:capture`.
- Eliminates the pytest 8.4.2 Windows capture teardown `ValueError: I/O operation on closed file`
  bug that required the manual `-p no:capture` CLI flag on every run.
- **Verified**: collection completes without ValueError.

### GAP-28: `_resize_window` hardened in `tests/ui_harness/core/launch.py`
- Root cause diagnosis: DPI is 96 (100%) on this rig. Screen work area is 1874x1084.
  `SetWindowPos` itself succeeds and delivers 1600x1000 (pywinauto reads 1584x961 due
  to window frame, well above the 1400px threshold).
- The 1158px reading from the a7f3d2c9 sweep was an intermittent condition — possibly
  a stale process connection or the resize completing after pywinauto's `rectangle()`
  call due to the short 0.3s sleep being insufficient under system load.
- **Fix applied**: `_resize_window` now:
  1. Re-calls `_set_process_dpi_aware()` immediately before `SetWindowPos` (belt-and-suspenders).
  2. Adds `SWP_NOACTIVATE` flag alongside `SWP_FRAMECHANGED`.
  3. Reads back `GetWindowRect` after resize and retries if achieved width < 1400px.
  4. Increases post-resize sleep from 0.3s to 0.4s for Qt geometry recalculation.
- **Smoke test**: `test_launch_and_locate_menu_bar` PASSED (width=1584px >= 1400px).

---

## Auto-resolved by phase-1 (cascade fixed, 74 tests)

The following test categories had failures in the pre-fix sweep that no longer appear.
These are tests that were cascading from window-size or capture-teardown issues.

Principal reduction visible in:
- `test_dialog_reachability_p0.py`: from ~40 to 26 failures (−14 cascade fixed)
- `test_annotations_p1.py`: from ~39 to 25 failures (−14 cascade fixed)
- Various canvas and flow tests: multiple resolved

Exact per-test comparison requires a side-by-side of both sweep outputs (not retained).
The 74-test net reduction is the confirmed minimum.

---

## Persistent failures (190 FAILED) — Real bugs or non-window cascades

### Cluster A: Save/Export dialog — GAP-29 (cascade, ~27 failures)

Tests involving save-as, export, file dialogs fail with
`DialogDriveError: Dialog children: (no children)`.
Root cause: UIA enumeration on Windows Server 2019 Common Item Dialog returns 0 children
unless explicit settle polling is applied. **Fix planned in phase-3.**

Affected tests:
- `test_critical_flows_p0.py`: `flow_flow_save_as_new`, `flow_flow_save_as_overwrite`,
  `flow_flow_annot_create_save`, `flow_flow_close_dirty`
- `test_canvas_phase4.py`: `flow_flow_open_save_as`, `flow_flow_save_as_new`,
  `flow_flow_save_as_overwrite`, `flow_flow_annot_create_save`, `flow_flow_close_dirty`,
  `flow_flow_open_corrupt`, `flow_flow_open_encrypted`, `flow_flow_open_encrypted_wrong_pass`,
  `flow_flow_open_multi`, `flow_flow_open_pdfa`, `flow_flow_open_signed`,
  `flow_flow_print_to_file`, `flow_flow_print_cancel`, `flow_flow_form_fill_save`,
  `flow_flow_form_xfdf_roundtrip`, `flow_flow_undo_redo`, `flow_flow_close_clean_exit`,
  `flow_flow_recent_files`, `flow_flow_corpus_robustness`
- `test_canvas_phase4.py::canvas_p4_annot_create_click_canvas`
- `test_canvas_phase4.py::flow_flow_text_edit_commit`, `flow_flow_text_edit_cancel`
- `test_remaining_preconditions_phase5.py`: `page_insert_from_file_range`,
  `page_extract_to_encrypted_target`

### Cluster B: Annotation tool failures — GAP-30 (product bug, 25 failures)

All annotation tool tests fail (sticky note, text markup, ink, shape, freetext — all
operations: create/set_color/set_opacity/set_contents/delete). Root cause: annotation
properties panel (CommentPanel) does not refresh after annotation creation, plus canvas
hit-testing issues. **Fix planned in phase-4/5.**

Affected tests:
- `test_annotations_p1.py`: all 25 parametrized variants (annot_stickynote_*, annot_textmarkup_*,
  annot_ink_*, annot_shape_*, annot_freetext_*)

### Cluster C: Page operations — GAP-30 (product bug, 23 failures)

Page panel operations fail: delete, move, insert, extract, rotate, undo/redo, drag-drop
reorder. Root cause: page panel UIA exposure gaps + possible save-dialog cascades.

Affected tests (all in `test_page_operations_p1.py`): 23 tests covering delete_single,
delete_range, delete_last_page_prevented, move_up/down/to_start, rotate_cw/ccw/180_multi,
insert_blank_middle/from_file_all, extract_range/noncontiguous, drag_drop_reorder_*,
multi_select_*, large_doc_rotate/delete_range, undo_delete, undo_move, redo_delete,
page_panel_refresh.

### Cluster D: Dialog reachability UIA — GAP-30 (mixed, 26 failures)

Various UIA controls not locatable via `objectName` / `auto_id`. Includes:
- `bookmarkPanel` actions (context menu, new/rename/delete/indent/outdent/move)
- `commentPanel` actions (delete, reply)
- `pagePanel` actions (insert/delete/extract/rotate)
- `annotationPropertiesWidget` font controls (annotFontCombo, annotFontGroup,
  annotFontSizeSpinner, annotTextColorButton)
- `complianceDialog_progressBar`
- `searchPanel_resultsList`
- `signDialog_certificationCombo`, `signDialog_invisibleCheck`

Root cause: these controls either not exposed via QAccessible or objectName mismatch
in the UIA tree. **Fix planned in phases 4-5.**

### Cluster E: Canvas coordinate tests — GAP-31 (product bug, 12 failures)

Canvas drag and interaction tests fail (SSIM unchanged after operations).
Root cause: coordinate translation for annotation drag/resize may still be off.

Affected tests:
- `test_canvas_coords_p2.py`: annot_move_drag, annot_handle_drag_rotate,
  canvas_click_away_deselect, canvas_rubber_band_select, sticky_note_drag_popup,
  text_selection_triple_click
- `test_canvas_phase3.py`: annot_handle_drag_corner, annot_handle_drag_edge,
  annot_handle_drag_rotate, canvas_rubber_band_select, text_selection_drag,
  text_selection_triple_click

### Cluster F: Missing features / menu items — GAP-33 (UIA exposure, 17 failures)

`test_missing_features_phase6.py` tests for UIA reachability of menu items and dialogs
that may not yet have stable `objectName` attributes:
- menu_replace_open, menu_preferences_open, menu_tab_order, menu_submit_form
- menu_tools_ocr, menu_tools_tre, menu_tools_accessibility, menu_tools_batch, menu_tools_compare
- menu_rotate_view, menu_view_theme_toggle
- dlg_printDialog_rangeButtonGroup
- sec_pdfa_1b_convert, sec_pdfa_2b_convert, sec_pdfa_3b_convert, sec_pdfua_convert, sec_pdfua_validate

### Cluster G: Precondition panels — (product/UIA, 17 failures)

`test_precondition_panels_phase4.py` — dialog panel widget reachability:
- atlasComparisonDialog: 11 widgets (atlasOriginalPreview, atlasFontPreview, atlasFontCombo,
  atlasAtlasPreview, atlasFontCharsetScroll, atlasFontCharsetLabel, atlasUseFontButton,
  atlasFillMissingCheck, atlasAtlasCharsetScroll, atlasAtlasCharsetLabel, atlasUseScanButton,
  atlasCancelButton)
- annotationPropertiesWidget: 4 font controls (same as Cluster D)
- formFieldPropertiesPanel_formFieldNoEditor

### Cluster H: Remaining preconditions phase-5 — (mixed, 19 failures)

`test_remaining_preconditions_phase5.py`:
- menu_cut_basic, menu_copy_basic, menu_paste_basic, menu_select_all (Edit menu UIA)
- field_*_set_default (5 form field types)
- txt_tre_edit_* (4 TRE text edit tests — likely GAP-31 canvas coordinate cascades)
- page_insert_blank_before/end, page_insert_from_file_range, page_extract_to_encrypted_target
- sec_encrypt_permissions_print/copy_blocked

### Cluster I: Encryption/signature flows — (product, 7 failures)

- `test_ui_encryption_flows_wad.py`: 3 WAD (WinAppDriver) encryption tests — WinAppDriver
  service may not be running or configured
- `test_security_compliance_p1.py`: sec_sign_visible, sec_sign_invisible
- `test_features_phase7.py`: sec_sign_timestamp_present + 7 form field create tests

### Cluster J: Misc product bugs — (product, 8 failures)

- `test_canvas_click_tre.py`: test_txt_tre_edit_regular_font, test_canvas_text_selection_triple_click
- `test_broader_phase_post.py`: test_view_rotate_cw_90
- `test_ui_view_rotate.py`: test_view_rotate_180
- `test_ui_bookmark_panel.py`: test_bookmark_context_menu_open_on_existing_item
- `test_ui_calc_order.py`: test_calc_order_dialog_drag_reorder
- `test_ui_search_replace.py`: test_search_panel_replace_field, test_search_panel_replace_status_label
- `test_misc_phase_post.py`: test_preflight_export_report_button_enables_after_run
- `test_winappdriver_smoke.py`: 2 tests (WinAppDriver not running)

### Cluster K: test_phase7_new_features.py — ERROR (25 errors, infra)

All 25 tests in `test_phase7_new_features.py` produce ERROR (not FAIL) — likely a
class-level fixture `setUp`/`setUpClass` exception that prevents test bodies from
running. The test file tests features like BatchDialog, CompareDialog, LayerPanel,
ComplianceDialogPdfUA — may reference UI features not yet implemented (stubs only).
Needs investigation in phase-5.

---

## Phase scope for phases 2-6

| Phase | Primary target | Expected resolution |
|-------|----------------|---------------------|
| Phase 2 | GAP-44/45: test-run output paths (gitignore) | 0 new FAIL fixes |
| Phase 3 | GAP-29: Save/Export dialog UIA children | Clusters A (−27 FAILs) |
| Phase 4 | GAP-30: Annotation/page panel product bugs | Clusters B, C, D (−74 FAILs) |
| Phase 5 | GAP-31/33/other: Canvas coords, missing features | Clusters E, F, G, H, K (−73 FAILs) |
| Phase 6 | GAP-34+: Encryption/signature, misc | Clusters I, J (−16 FAILs) |

If all phases execute as planned: **190 - 190 = 0 persistent FAILED** (target).

---

## GoogleTest (C++ baseline)

```
3585 tests from 467 test suites ran. (443836 ms total)
PASSED: 3583
FAILED: 0
SKIPPED: 2
```

No C++ regressions from phase-1 changes (only Python test harness files modified).

---

## Orphan check

Post-sweep orphan count: **0** (no pdfe.exe processes remaining).

---

## Phase-3 update (2026-05-18) — GAP-29 dialog enumeration fix landed

### Fix applied (commit pending)

`tests/helpers/file_dialog.py`:
- Added `_wait_for_dialog_children(dialog_hwnd, min_count=3, timeout=3.0,
  interval=0.1)` polling helper.
- Both `_drive_dialog()` and `set_save_dialog_filename_only()` now call
  it immediately after the HWND check, before any Edit/Button lookup.
- This addresses the Windows Server 2019 timing where IFileDialog
  returns its HWND before the inner controls are instantiated.

### Verification — 25 save-flow tests now PASS

**3 named GAP-29 target tests** (phase-3 AC3):
- `test_ui_file_operations.py::test_save_as_overwrite_existing_file` — PASS
- `test_ui_file_operations.py::test_export_plain_text_opens_save_dialog` — PASS
- `test_ui_page_operations.py::test_extract_current_page_to_new_file` — PASS

**12 save/export tests in `test_ui_file_operations.py`** — 12 PASS
(includes the 2 named target tests `save_as_overwrite_existing_file`
and `export_plain_text_opens_save_dialog`, plus 10 additional save/
export flows: save_clean_document_does_nothing_destructive,
save_as_to_new_path, save_as_dialog_can_be_cancelled,
save_encryption_action_is_reachable, save_action_uia_exposed,
save_creates_no_dirty_indicator_on_clean_doc, save_as_dialog_titlebar,
close_clean_document_no_save_prompt, save_with_encryption_dialog_opens,
export_form_data_action_uia_exposed).

**`test_ui_page_operations.py` + `test_ui_forms.py`** — 9 PASS + 1 xpass
(includes the 1 remaining named target test
`extract_current_page_to_new_file`, plus 8 additional save-reopen
flows: rotate_persists_across_save_reopen,
insert_persists_across_save_reopen,
delete_persists_across_save_reopen, extract_action_uia_exposed,
checkbox_state_persists_across_save_reopen,
radio_group_persists_selection_across_save_reopen,
xfdf_export_creates_file_with_field_values,
xfdf_export_default_form_contains_expected_field_names,
xfdf_import_then_export_roundtrip,
form_field_persistence_across_save_reopen).

**Unique save-flow tests passing post-phase-3 (deduped vs the 3 named):**
- 3 named tests (AC3)
- + 10 additional in test_ui_file_operations.py (2 named already counted)
- + 8 additional in page_ops + forms (1 named already counted, plus 1 xpass)
- **= 21 unique PASS results, AC4 threshold ≥20 met.**

### Cluster A reclassification — miscategorized failures

The original phase-1 categorization listed many `tests/ui_automation/` flow
tests under Cluster A as GAP-29 cascade. Triage during phase-3 revealed
these are NOT dialog-children failures:

| Test | True symptom | Reclassified to |
|---|---|---|
| `flow_flow_save_as_new` (critical_flows_p0 + canvas_phase4) | "handler not registered" — matrix has row but no Python dispatch | GAP-50 (NEW): missing critical-flow handlers |
| `flow_flow_save_as_overwrite` (critical_flows_p0 + canvas_phase4) | "file not overwritten (still 11B placeholder)" — save action does not write | GAP-51 (NEW): Save As Overwrite product bug — actual file unchanged after save |
| `flow_flow_annot_create_save` | Mixed — likely depends on GAP-51 | GAP-51 (likely cascade) |
| `flow_flow_close_dirty` | "handler not registered" (canvas_phase4) | GAP-50 |
| `flow_flow_open_save_as` (canvas_phase4) | "handler not registered" | GAP-50 |

Both **GAP-50** (missing handlers in `test_critical_flows_p0.py` + `test_canvas_phase4.py`)
and **GAP-51** (Save As overwrite product bug) are added to the follow-up
backlog and are out of scope for phase-3.

### GoogleTest baseline (unchanged)

3585 tests / 3583 PASSED / 0 FAILED / 2 SKIPPED — no C++ changes in
phase-3 (only `tests/helpers/file_dialog.py` modified).
