# Phase 3 Cluster #1 fix plan — dialog reachability

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


**Pipeline:** f3c91a2e-7b84-4d06-a5e1-8c3f20b9d741
**Cluster:** #1 — 32 failing UI tests across 3 files
**Date:** 2026-06-08

---

## Failing files

| File | Failures |
|---|---|
| `tests/ui_automation/test_dialog_reachability_p0.py` | 18 |
| `tests/ui_automation/test_ui_dialog_coverage.py` | 11 |
| `tests/ui_automation/test_phase7_new_features.py` | 3 (CI shows 6 — 3 are in this cluster, 3 in batch widget set) |

---

## Root-cause classification

### Category A: P0 test fed pseudo-widget rows from the matrix CSV (18 failures)

The matrix CSV (`tests/ui_automation/matrix/ui_test_matrix.csv`) contains
**meta-rows** added in pipeline a7c4f891 phase-7 C1 with element names like
`batchDialog.opens`, `batchDialog.closes_cleanly`, `batchDialog.widget_audit`.
These are NOT widget objectNames — they are sentinel labels written into
the matrix CSV by `test_ui_dialog_coverage.py` to record open / close /
widget-audit outcomes.

`test_dialog_reachability_p0.py` collects every `priority=P0, category=Dialog`
row and tries to find a widget with the row's suffix. For real widgets that
works; for these three sentinels it always fails because there is no
widget named `opens` etc.

**Affected rows (18):**

| dialog | sentinel suffixes |
|---|---|
| batchDialog | opens, closes_cleanly, widget_audit |
| combineFilesWizard | opens, closes_cleanly, widget_audit |
| compareDialog | opens, closes_cleanly, widget_audit |
| optimizeDialog | opens, closes_cleanly, widget_audit |
| preflightDialog | opens, closes_cleanly, widget_audit |
| customizeToolbarsDialog | opens, closes_cleanly, widget_audit |
| tabOrderDialog | opens, closes_cleanly, widget_audit |
| watermarkDialog | opens, closes_cleanly, widget_audit |
| metadataDialog | opens, closes_cleanly, widget_audit |
| printDialog | opens, closes_cleanly, widget_audit |
| printPreviewDialog | opens, closes_cleanly, widget_audit |

(11 dialogs × 3 sentinels = 33 rows; the failing-count log reports 18 because
6 of the rows hit `combineFilesWizard` / a dialog whose opener errored early
and recorded `blocked` rather than `red`.)

**Classification:** CAT-B (test infra)
**Fix:** In `tests/ui_automation/test_dialog_reachability_p0.py`,
add `opens`, `closes_cleanly`, `widget_audit` to a SENTINEL_SUFFIX set
that the test skips (records `blocked` with reason
"sentinel suffix tracked by test_ui_dialog_coverage.py").
This is a test-SETUP fix only — assertion logic unchanged.

---

### Category B: test_ui_dialog_coverage widget-name + menu-text mismatches (11 failures)

`test_ui_dialog_coverage.py` opens dialogs by menu path, asserts the dialog
window appears, then audits widget objectNames. For most dialogs the widget
list in `DIALOG_SPECS` does NOT match the actual `setObjectName()` calls in
`src/ui/*Dialog.cpp`.

| Dialog | Menu path issue | Widget mismatches (expected → actual) |
|---|---|---|
| batchDialog | menu OK | All widgets OK (no mismatch) — opening succeeds but the test was failing because the matrix sentinel test failed first; the dialog itself works. |
| combineFilesWizard | menu OK (`File -> Combine Files...`) | `addButton` → `addFilesButton`, `removeButton` → `removeFileButton` |
| compareDialog | menu OK | All OK |
| optimizeDialog | menu OK (`Tools -> Optimize PDF...`) | All OK |
| preflightDialog | menu OK (`Tools -> Preflight...`) | `profileCombo` → `preflightProfileCombo`, `runButton` → `preflightRunButton`, `issueTree` → `preflightIssueTree`, `summaryLabel` → `preflightSummaryLabel`, `closeBox` → `preflightCloseBox` |
| customizeToolbarsDialog | menu OK (`View -> Customize Toolbars...`) | `addButton` → `addActionButton`, `removeButton` → `removeActionButton` |
| tabOrderDialog | menu path `Forms -> Tab Order...` (test says); actual menu text is `Tab &Order...` so OK after ampersand strip. | `fieldList` → `tabOrderFieldList` |
| watermarkDialog | menu OK (`Tools -> Add Watermark...`) | `modeCombo` → `watermarkDialogModeCombo`, `textEdit` → `watermarkDialogTextEdit`, `opacitySpin` → `watermarkDialogOpacitySpin`, `applyButton` → `watermarkDialogApplyButton`, `cancelButton` → `watermarkDialogCancelButton` |
| metadataDialog | menu OK | `titleEdit` → `metadataTitleEdit`, `authorEdit` → `metadataAuthorEdit`, `subjectEdit` → `metadataSubjectEdit`, `keywordsEdit` → `metadataKeywordsEdit`, `buttonBox` → `metadataButtonBox` |
| printDialog | menu OK | `fromPageSpin` → `fromPageSpinBox`, `toPageSpin` → `toPageSpinBox`, `copiesSpin` → `copiesSpinBox` |
| printPreviewDialog | menu OK | `previewWidget` → `printPreviewWidget` |

**Classification:** CAT-B (wrong objectName in test spec)
**Fix:** Update `DIALOG_SPECS` widget lists in
`tests/ui_automation/test_ui_dialog_coverage.py` to use the actual
`setObjectName()` values from the source. The `close_button` defaults
also need adjusting (e.g. watermarkDialog uses `watermarkDialogCancelButton`,
not `cancelButton`).

---

### Category C: test_phase7_new_features wrong testToolActivatePipe action name (3 failures)

The `_MENU_TO_ACTION` lookup table in
`tests/ui_automation/test_phase7_new_features.py` maps `Security -> Sign Document...`
to action objectName `signAction`. The actual source has
`signDocumentAction_->setObjectName("signDocumentAction")` in
`src/ui/MainWindow.cpp:2290`. The pipe write succeeds (no error) but Qt's
`activateToolByObjectName("signAction")` finds nothing and silently
no-ops, so the SignDialog never opens. The test then searches `main_window`
for `tsaCheckBox` and fails.

Six tests share this root cause (all SignDialog + some Batch / Compliance
that ALSO have unrelated wiring problems):
- `TestSignDialogTSA.test_tsa_checkbox_exists`
- `TestSignDialogTSA.test_tsa_url_edit_exists`
- `TestComplianceDialogPdfUA.test_progress_bar_exists` — falls back to
  click_menu_path which works; progressBar is only visible during a
  multi-second conversion, NOT at dialog open. Belongs to the same
  BLOCKED-justification category as `printPreviewDialog.zoomInAction` etc.
- `TestBatchDialog.test_action_list_and_buttons_exist`
- `TestBatchDialog.test_error_log_exists`
- `TestBatchDialog.test_progress_bar_exists` (batchProgressBar IS at dialog
  open — but the test reads main_window instead of the open dialog, and
  `actionListWidget` / `batchProgressBar` / `batchErrorLog` are NOT
  enumerable as children of main_window when the dialog is modal because
  the modal dialog is a top-level window. Need to find the dialog
  separately.)

**Classification:** mostly CAT-B (wrong action name), partially CAT-A-like
(modal dialog widgets not enumerable from main_window).

**Fixes:**
1. Fix `_MENU_TO_ACTION["Security -> Sign Document..."]` from `signAction`
   to `signDocumentAction`.
2. For modal-dialog widget lookups (BatchDialog, ComplianceDialog), search
   the dialog window itself instead of main_window — switch to
   `find_dialog_window` + `find_by_suffix(dialog, ...)`.
3. `TestComplianceDialogPdfUA.test_progress_bar_exists`: mark xfail with
   the same justification used for `printPreviewDialog.zoomInAction` — the
   progress bar is only briefly visible during a synchronous conversion
   workflow and isn't reachable in the P0 dialog-open pattern.

---

## Counts

| Class | Count |
|---|---|
| a (missing dialog in src) | 0 — all 12 dialogs exist |
| b (objectName / menu / action name mismatch in tests) | 32 of 32 |
| c (menu path missing in src) | 0 |

---

## Specific changes (file:line)

### Fix 1 — `tests/ui_automation/test_dialog_reachability_p0.py`
Add sentinel-suffix filter after the matrix row parses. Lines ~386-400:
inject a check that records `blocked` and skips when suffix is in
`{"opens", "closes_cleanly", "widget_audit"}`.

### Fix 2 — `tests/ui_automation/test_ui_dialog_coverage.py`
Replace the widget names in `DIALOG_SPECS` (lines 87-198) with the actual
objectNames from the source files. Also fix `close` button defaults where
the dialog uses a prefixed objectName.

### Fix 3 — `tests/ui_automation/test_phase7_new_features.py`
Line 85: change `"Security -> Sign Document...": "signAction"`
to `"signDocumentAction"`.
In BatchDialog tests (lines ~285-310) and ComplianceDialog progressBar test
(line 274-278): search the dialog window via `find_dialog_window`, not
`main_window`. Mark `test_progress_bar_exists` xfail for ComplianceDialog
with the documented BLOCKED-EXTERNAL justification (mirrors
`WIDGET_BLOCKLIST` in `test_dialog_reachability_p0.py`).

---

## Expected outcome

After these three fixes, all 32 failures in Cluster #1 should either pass
or convert to a documented `blocked`/`xfail` outcome consistent with the
existing P0-test conventions. No source `setObjectName()` changes required —
all dialogs already use stable, automation-friendly objectNames; the tests
were just looking for the wrong strings.
