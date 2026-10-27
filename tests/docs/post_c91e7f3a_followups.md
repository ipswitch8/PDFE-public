# Post-Pipeline c91e7f3a Follow-up Backlog

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


**Source**: Audit conducted after pipeline c91e7f3a closure (commit a4f6d2e, master).
**Date generated**: 2026-05-16
**Total gaps**: 17 identified (1 verified non-gap, 16 actionable).

This is the prioritized backlog of issues surfaced by the c91e7f3a audit. They are not blocking but represent real coverage, security, and tracking debt. A follow-up pipeline will address them in optimal sequence.

---

## SECURITY (highest priority)

### GAP-15 [HIGH SEVERITY]: Test pipes ship in production builds unconditionally
- **Files**: `src/ui/MainWindow.cpp:291, 313, 585, 660, 684`
- **Affected pipes** (all unconditionally created, NOT env-gated):
  - `testToolActivatePipe_` — can activate any QAction by objectName (Save, Print, Delete Page).
  - `testClickByPdfCoordPipe_` — can click arbitrary PDF coordinates via UIA.
  - `testFormFieldValueSetPipe_` — can set arbitrary form field values without consent.
  - `testFormFieldValueQueryPipe_` — read-only form field value access.
  - `testFontFamiliesQueryPipe_` — read-only font family enumeration.
- **Precedent**: `testAnnotationPipes` IS env-gated via `PDFE_TEST_ANNOT_PIPES` at `MainWindow.cpp:410`.
- **Risk**: Malicious automation script with UIA access to a shipped PDFE.exe could exploit any of the SET pipes silently.
- **Fix**: Gate all 5 pipes behind `PDFE_TEST_PIPES=1` env var (matching existing pattern). Tests set the env var; production users don't.

### GAP-16 [LOW]: FormManager::resetForm() missing defensive null check
- **File**: `src/forms/FormManager.cpp:530-548`
- **Issue**: `pdf_annot_obj()` return value passed to `pdf_dict_get` without null check. Probably safe (MuPDF guards), but adding `if (!fieldObj) continue;` is cheap.
- **Add**: C++ unit test loading a malformed form (widget with NULL annot_obj).

---

## DOCUMENTATION / TRACKING HYGIENE

### GAP-1: test_xfdf_import_populates_fields flake undocumented
- **File**: `tests/test_ui_forms.py:2708`
- **Symptom**: Passes standalone (41s), fails under suite contention.
- **Fix**: Add P-H-INFRA-11 (or similar new code) entry in `tests/KNOWN_WEAK_TESTS.txt`. Replace silent `pytest.skip` paths with strict xfail OR an explicit "single-test-only" gating decorator.

### GAP-2: altTextAction_ selection-aware follow-up not in parity matrix
- **File**: `src/ui/MainWindow.cpp:1586` (hardcoded `annotIndex=0`)
- **Phase-5 karen claim**: "tracked in the parity matrix" — but no entry exists in `tests/docs/test_strategy.md`.
- **Fix**: Add parity-matrix row for "AltText dialog operates on selected annotation, not annotation 0".

### GAP-3: testFormFieldValueSetPipe_ always-on tech debt not tracked
- **File**: `src/ui/MainWindow.cpp:660`
- **Issue**: KNOWN_WEAK:629 describes the always-on pipe as a feature, not as debt. Inconsistent with env-gated annotation pipes.
- **Fix**: Tracked under GAP-15 remediation (env-gate the pipe).

### GAP-4: P-G-INFRA-7 audit trail missing reversion annotation
- **File**: `tests/KNOWN_WEAK_TESTS.txt:518`
- **Issue**: P-G-INFRA-7 RESOLVED line from pipeline b7e2d04a not updated to note phase-6 reversion of the inversion.
- **Fix**: Add cross-reference: "Note: inversion was actually masking a double-toggle bug; reverted in pipeline c91e7f3a phase-6 (commit e5054c6). See P-H-INFRA-10."

---

## STALE XFAILS — Product Resolved, Test Driver Not Migrated

### GAP-5: test_pagepanel_drag_drop_reorders_pages
- **File**: `tests/test_ui_page_operations.py:1243`
- **xfail ref**: P10-INFRA-1 (RESOLVED a7c2e541 phase-17 via movePageProgrammatic)
- **Fix**: Rewrite test driver to call movePageProgrammatic pipe.

### GAP-6: test_redacted_text_not_present_after_save_reopen
- **File**: `tests/test_ui_redaction.py:1325`
- **xfail ref**: P11-INFRA-1 (RESOLVED a7c2e541 phase-17 via coord translator)
- **Fix**: Switch test to new coord translator pipe.

### GAP-7: test_undo_after_partial_marks_removes_only_last
- **File**: `tests/test_ui_redaction.py:1494`
- **xfail ref**: P11-BUG-2 (RESOLVED a7c2e541 phase-17 via addRedactionMarkAtPdfRect)
- **Fix**: Switch test to addRedactionMarkAtPdfRect pipe.

### GAP-8: 2 encryption-dialog xfails
- **Files**: `tests/test_ui_encryption_signatures.py:1029, 1476`
- **Tests**: test_set_user_password_via_encryption_dialog + test_owner_password_with_copy_disabled
- **xfail ref**: P12-INFRA-2 (RESOLVED a7c2e541 phase-17 product-side)
- **Fix**: Live-UI re-run; may need new dialog-driving pipe.

### GAP-9: 3 stress-test xfails
- **File**: `tests/test_ui_stress.py:1039, 1196, 1755`
- **xfail refs**: P15-INFRA-2 (RESOLVED a7c2e541 phase-17 via addRedactionMarkAtPdfRect), P9-DEBT-2 (RESOLVED b3f19d72 phase-12).
- **Fix**: Migrate stress test drivers to new pipes.

---

## COVERAGE / TEST MATRIX GAPS

### GAP-10: 68 GoogleTest expect_ge SKIPs untracked
- **Files**: `tests/test_form_manager.cpp` (14), `tests/test_form_tool.cpp` (19), `tests/test_annotation_manager.cpp` (14), `tests/test_save.cpp` (9), `tests/test_save_operations.cpp` (9), etc.
- **Issue**: No `coverage_gaps_open` or `P-*-COVERAGE-*` taxonomy exists. KNOWN_WEAK.txt focuses on test-side bugs, not coverage absences.
- **Fix**: Establish P-COV-* code naming convention; add KNOWN_WEAK entries describing each cluster.

### GAP-11: scrollToPdfRect CrossPageNavigation test SKIPs silently
- **File**: `tests/test_pdf_canvas_q_invokable.cpp:418`
- **Issue**: `GTEST_SKIP() << "fixture has only 1 page"` while multi-page fixtures exist (`tests/fixtures/test_3page.pdf`).
- **Fix**: Switch fixture to a multi-page PDF; test should actually exercise cross-page nav.

### GAP-12: No Python E2E for keyboard scroll fix
- **Issue**: Up/Down/Left/Right inversion fix has C++ unit tests only. No Python UI test sends arrow keys with visual confirmation.
- **Fix**: Add `tests/test_ui_view_navigation.py::test_keyboard_scroll_directions` exercising all 4 keys at zoom>1.0 with screenshot diff.

### GAP-13: Customize-toolbar Remove SET pipe missing
- **File**: `src/ui/CustomizeToolbarsDialog.cpp:115` (removeButton_)
- **Issue**: Add pipe exists; Remove pipe doesn't. Asymmetric coverage.
- **Fix**: Add `testCustomizeToolbarsRemoveActionPipe` mirror; add Remove-side test.

### GAP-14: 14 fixture screenshots regenerated without change-log
- **Files**: 10× `tests/fixtures/screenshots/phase5_*_canvas.png` + 4× `tests/fixtures/ui_screenshots/text_edit/ui_te_ui_*.png`
- **Issue**: phase-7 closeout regenerated baselines (some -50% size) with no per-baseline justification documented. Could mask regressions.
- **Fix**: Audit each baseline regen against the rendered output; add `baseline_regen_log.json` artifact for future regenerations.

---

## NON-GAP (Verified Clean)

### GAP-17: _dismiss_pdfe_dialogs() isolation — VERIFIED CLEAN
- Test-only helper at `tests/test_ui_specimen_a_green_subset.py:172`. Not in `src/`. No production exposure.

---

## Total Counts

- **High-severity**: 1 (GAP-15)
- **Medium-severity**: 5 (GAP-1, 2, 4, 11, 12)
- **Low-severity**: 10 (GAP-3, 5, 6, 7, 8, 9, 10, 13, 14, 16)
- **Non-gap**: 1 (GAP-17)
