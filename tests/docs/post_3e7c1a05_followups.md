# Post-Pipeline 3e7c1a05 Follow-up Backlog

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


**Source**: Audit conducted after pipeline 3e7c1a05 closure (commit 0c64f99, master).
**Date generated**: 2026-05-18
**Total gaps**: 10 identified (1 verified non-gap, 9 actionable).

This backlog continues GAP numbering from the post-c91e7f3a audit (GAP-1..17). The follow-up pipeline will address them in optimal sequence.

---

## SECURITY (highest priority)

### GAP-18 [SECURITY, MEDIUM]: 8 ungated test pipes in CustomizeToolbarsDialog
- **File**: `src/ui/CustomizeToolbarsDialog.cpp`
- **Affected pipes** (all unconditionally created):
  - Line 225: `testAddActionPipe_` (`testCustomizeAddActionPipe`)
  - Line 238: `testResetToDefaultsPipe_` (`testCustomizeResetPipe`)
  - Line 276: `testAvailableActionsResultPipe_`
  - Line 285: `testAvailableActionsQueryPipe_`
  - Line 315: `testToolbarContentsResultPipe_`
  - Line 324: `testToolbarContentsQueryPipe_`
  - Line 389: `testWidgetNamesResultPipe_`
  - Line 398: `testWidgetNamesQueryPipe_`
- **Inconsistency**: `testRemoveActionPipe_` at line 254 IS gated. Same file, same constructor, asymmetric handling.
- **Risk**: When Customize Toolbars dialog is open, the Add/Reset/enumeration pipes are UIA-discoverable. Lower attack surface than MainWindow pipes (only exists when dialog open), but same exposure pattern.
- **Fix**: Apply `if (qEnvironmentVariableIsSet("PDFE_TEST_PIPES"))` guard to all 8 pipes, matching the GAP-15 + ALT-TEXT-PIPES-1 pattern.

---

## TEST COVERAGE

### GAP-23: No Python E2E test for Up/Down arrow fall-through in text-edit mode
- **Symptom**: Pipeline 3e7c1a05 phase-1 fixed Up/Down arrow keys silently no-op during text edit. Added a C++ unit test (`TextEditToolTest.UpDownKeyEventsNotAcceptedByOnKeyPress`) verifying the event isn't accepted, but no Python E2E test verifies the page actually scrolls.
- **Integration boundary not tested**: TextEditTool ignore → PdfCanvas isAccepted check → canvas scroll handler.
- **Fix**: Add `tests/test_ui_text_edit.py::test_up_down_arrows_scroll_canvas_during_edit` or similar. Enter text edit mode, capture canvas state, press Down 20×, capture, assert canvas pixels differ (page scrolled). Repeat for Up.

### GAP-25 [SECURITY TEST]: No test verifies pipes are absent when PDFE_TEST_PIPES unset
- **Symptom**: All pipe-gating relies on `qEnvironmentVariableIsSet("PDFE_TEST_PIPES")`. Widgets are structurally guaranteed not to be created (conditional construction), but no automated test asserts UIA-tree absence in production-simulation mode.
- **Fix**: Add a Python test that launches PDFE.exe WITHOUT setting `PDFE_TEST_PIPES`, then enumerates the UIA tree and asserts no widget with name matching `test*Pipe` is found. Place under `tests/test_ui_security.py` or similar.

---

## PRODUCT WORK (carryover)

### GAP-27: AltText handler hardcoded annotIndex=0
- **File**: `src/ui/MainWindow.cpp:1586` — `altTextAction_` handler operates on `annotIndex=0` regardless of selection.
- **Carryover from**: Post-c91e7f3a GAP-2; tracked OPEN in `tests/docs/test_strategy.md` parity matrix.
- **Fix**: Replace hardcoded `0` with actual selected annotation index from `selectedAnnotationIndex_` member (already set elsewhere). Add C++ unit test verifying the dialog operates on the actually-selected annotation in a multi-annotation page.

---

## DATA QUALITY

### GAP-19: 3 baseline_regen_log entries have logically impossible size/MD5 pairing
- **File**: `tests/fixtures/baseline_regen_log.json`
- **Affected entries**:
  - `annotations/toolbar_baseline.png` — `size_before_bytes:0` + `pixel_md5_pre:"22791dac..."`.
  - `forms/form_tool_baseline.png` — `size_before_bytes:0` + `pixel_md5_pre:"01506c54..."`.
  - `page_ops/panel_baseline.png` — `size_before_bytes:0` + `pixel_md5_pre:"5077b61b..."`.
- **Issue**: If file truly didn't exist (size=0), no pre-state MD5 should exist. Either size_before_bytes was wrong (file existed) or pixel_md5_pre was computed from a different source.
- **Fix**: Investigate via `git log --follow` for each baseline. If file existed pre-regen, correct `size_before_bytes` to its actual pre-size. If file truly didn't exist, set `pixel_md5_pre: null` + add `verified: null` + `note` like P-COV-BASELINE-1.

---

## TRACKING HYGIENE

### GAP-21: Stale citations in test_ui_stress.py xfails
- **File**: `tests/test_ui_stress.py`
- **Affected tests**:
  - Line 1200: `test_stress_1000_annotation_undo_redo` cites "P15-INFRA-2 (related)" — P15-INFRA-2 RESOLVED in a7c2e541 phase-17 + b3d7f942 phase-4.
  - Line 1759: `test_stress_form_fill_500_fields_save` cites "P9-DEBT-2 (manifests at stress scale)" — P9-DEBT-2 RESOLVED in b3f19d72 phase-12.
- **Issue**: Tests cite RESOLVED P-codes without new tracking IDs.
- **Fix**: Assign new P-STRESS-* codes for the stress-specific cases. Update KNOWN_WEAK_TESTS.txt with substantive entries. Update test xfail reason strings.

### GAP-22: ALT-TEXT-PIPES-1 entry missing RESOLVED marker
- **File**: `tests/KNOWN_WEAK_TESTS.txt:687`
- **Issue**: The entry body describes the security finding but doesn't carry an explicit "RESOLVED" marker. Other resolved P-* entries use a consistent marker pattern.
- **Fix**: Add "RESOLVED — pipeline 3e7c1a05 phase-1 (commit 4733175) gated both pipes behind PDFE_TEST_PIPES env var." to the entry body, matching the format used by P-H-INFRA-* RESOLVED entries.

---

## PROCESS / DOCUMENTATION

### GAP-24: verify_baseline_pixel_equivalence.py not CI-integrated
- **Symptom**: One-off audit script with no conftest hook, no pytest wrapper, no scripts/ reference. Future baseline regenerations could re-introduce the same honesty gap phase-3 was meant to detect.
- **Fix**: Add `tests/test_baseline_integrity.py` that imports the script's verify function and runs it as a pytest test. Fails when any MD5 mismatch detected. Add to default test discovery.

### GAP-20: System-wide rendering-change not documented as reproducibility constraint
- **Symptom**: 8 canvas baselines were regenerated due to DPI/theme/Qt-version change between capture sessions. Documented per-entry in baseline_regen_log.json but not in test_strategy.md as a known reproducibility limitation.
- **Fix**: Add a section to test_strategy.md documenting that baseline regenerations are environment-specific (display DPI, system theme, Qt version). Recommend re-baselining on a different rig requires fresh capture, not direct copy.

---

## NON-GAP (Verified Clean)

### GAP-26: No test pipes outside MainWindow + CustomizeToolbarsDialog — VERIFIED CLEAN
- Grep across `src/ui/*.cpp` confirmed RedactionTool, FormManager, AnnotationManager have no test-pipe QLineEdits. No production exposure outside the two known files.

---

## Total Counts

- **Medium-severity**: 1 (GAP-18)
- **Low-severity**: 8 (GAP-19, 20, 21, 22, 23, 24, 25, 27)
- **Non-gap**: 1 (GAP-26)
