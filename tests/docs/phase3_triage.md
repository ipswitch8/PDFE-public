# Phase 3 triage — pipeline f3c91a2e-7b84-4d06-a5e1-8c3f20b9d741

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


**Source:** `ui-suite-summary.json` from CI run **27107152975** (sha `8141921`)
**Counts:** 66 failures + 4 timeouts = **70 work items**
**Triaged:** 2026-06-07

---

## Executive summary

### Category totals

| Cat | Count | Meaning |
|---|---|---|
| **CAT-A** real PDFE bug | **8** | UI didn't behave correctly; fix lives in `src/` |
| **CAT-B** test infra | **49** | Brittle selector / wrong dialog spec / pywinauto helper gap; fix in `tests/` |
| **CAT-C** documented limitation | **0** | None require a new `PDFE-LIMITATION-*` entry; root causes are real bugs or test bugs |
| **CAT-D** Phase-2 env / runner | **3** | Per-file 1200 s budget too small for files with 40-97 collected tests |
| **CAT-T** timeout | **1** | `test_precondition_panels_phase4.py` — every test failing AND budget too small (both A and D) |
| **TOTAL** | **70** | |

### Top 3 root-cause clusters

1. **Cluster #1: Dialog widget reachability — 32 failures (CAT-B)**
   `test_dialog_reachability_p0` (18) + `test_ui_dialog_coverage` (11) + `test_phase7_new_features` (3) all fail with identical "X widget not reachable in YDialog" / "YDialog did not open via menu Z" errors. Common pattern: tests look for dialogs (batchDialog, compareDialog, metadataDialog, preferencesDialog, printDialog, printPreviewDialog, optimizeDialog, preflightDialog, customizeToolbarsDialog, tabOrderDialog, watermarkDialog, combineFilesWizard) by their `objectName` after triggering a menu — but the dialogs either (a) don't exist in current `src/ui/`, (b) have a different `objectName` than the test expects, or (c) the menu path the test uses doesn't exist. **Fixing the dialog discovery/menu-mapping in the test helpers probably unblocks all 32.**

2. **Cluster #2: Visual regression on Phase-5 file-open scenarios — 10 failures (CAT-A, possibly stale baseline → CAT-B)**
   `test_ui_file_operations` has 10 visual-regression failures with very low SSIM (-0.24 to 0.25) and high pixel diff (30-55%). The diff PNGs (e.g. `phase5_open_test_single_canvas_diff.png`) show the canvas is offset roughly one panel-width relative to baseline — i.e. the side panel is/isn't visible in one of the two screenshots. Either (a) a real regression where the page canvas no longer renders the PDF after `File→Open`, or (b) the panels-default-visible state diverged from when the baselines were captured. **Capture a fresh screenshot manually on master; if it matches actual, baselines are stale (CAT-B). Otherwise it's a real regression in MainWindow startup/open path (CAT-A).**

3. **Cluster #3: Canvas mouse-drag actions not registering — 8 failures (CAT-A + CAT-B mixed)**
   `test_canvas_phase3` and `test_canvas_coords_p2` share a pattern: drag operations (corner handle resize, edge handle resize, annotation move, rubber-band select, text-selection drag, triple-click) end with no observable side effect (`dx=0.0, dy=0.0`, `tree 0 -> 0`, clipboard empty, "handler not registered"). Subset: "handler not registered" (4) is purely CAT-B (the synthetic-event runner's dispatch table is missing entries). The other 4 may be CAT-A if real PDFE drag/resize code doesn't react to pywinauto-synthesized events. **Investigate the synthetic-event handler registration first (cheap CAT-B fix); if drag still doesn't work after handlers register, then CAT-A.**

### Recommended Phase-3 fix order (by impact)

| Order | Fix | Unblocks | Effort |
|---|---|---|---|
| 1 | Audit/fix dialog `objectName`s and menu paths in `tests/ui_automation/` helpers (or in `src/ui/*Dialog.cpp` if missing) | ~32 failures (Cluster #1) | M |
| 2 | Capture fresh Phase-5 file-open baselines or fix MainWindow open path | ~10 failures (Cluster #2) | S–L |
| 3 | Register missing synthetic-event handlers in canvas test driver (`canvas_text_selection_triple_click`, `canvas_canvas_rubber_band_select`, `canvas_annot_handle_drag_rotate`, `canvas_sticky_note_drag_popup`) | ~4 failures, possibly more if Cluster #3 root cause is shared | S |
| 4 | Split big test files (`test_ui_annotations.py` 45 items, `test_ui_compliance_accessibility.py` 97 items, `test_ui_preferences_chrome.py` 70 items) into smaller per-file shards, OR raise 1200 s per-file budget in `tests/scripts/run_ui_suite.py` | 3 of 4 timeouts | S |
| 5 | Annotation undo title-cleanup bug (StickyNote/FreeText delete: undo doesn't clean `*` from window title) — fix in `src/ui/MainWindow.cpp` title-dirty-tracking on annotation delete undo path | 2 failures | S |
| 6 | Sign dialog TSA widget objectNames (`tsaCheckBox`, `tsaUrlEdit`, `pfxPathEdit`, etc.) — verify Sign dialog actually exposes these | 5 failures (cross-file) | S |
| 7 | TestHarnessSmoke baseline regen (NCC=0.5990) | 1 failure | S |

---

## CAT-A: Real PDFE application bugs

1. **`tests.test_ui_file_operations::test_open_via_file_menu_picks_test_single`** (and 9 sibling tests in same module)
   - Symptom: Visual regression, SSIM -0.0160 to 0.2524, pixel_fraction 30-55%. Diff PNGs show canvas is blank/offset relative to baseline.
   - Likely fix area: `src/ui/MainWindow.cpp` (panel layout on file open, or PdfCanvas first-paint timing), OR `tests/visual/baselines/phase5_*_canvas.png` (regen).
   - Note: Could also be CAT-B (stale baselines from before a panel-default-visibility change). **Verify with manual screenshot before fixing.**
   - All 10: `test_open_via_file_menu_picks_test_single`, `test_open_via_ctrl_o_shortcut`, `test_open_at_launch_via_command_line`, `test_open_corrupt_xref_pdf_recovers`, `test_open_scanned_pdf_loads_canvas`, `test_new_file_action_creates_blank_document`, `test_status_bar_present_after_open`, `test_open_landscape_pdf_renders`, `test_open_multisize_pdf_renders`, `test_open_then_open_replaces_document`
   - Priority: **P1** (10 tests blocked, possibly the most user-visible)

2. **`tests.ui_automation.test_annotations_p1::test_p1_annotation_tool[annot_stickynote_delete]`**
   - Symptom: `StickyNote: undo didn't clean title after 4 attempts: 'PDFE - [test_3page.pdf]*'`
   - Likely fix area: `src/ui/MainWindow.cpp` — `updateWindowTitle()` / dirty-flag tracking on annotation undo; `src/core/UndoManager` `isClean()` not honored after annotation delete undo.
   - Priority: **P2**

3. **`tests.ui_automation.test_annotations_p1::test_p1_annotation_tool[annot_freetext_delete]`**
   - Symptom: same root cause as #2 — `FreeText: undo didn't clean title after 4 attempts: 'PDFE - [test_3page.pdf]*'`
   - Likely fix area: same as #2 (annotation undo doesn't restore clean state)
   - Priority: **P2** (single fix likely covers both)

4. **`tests.ui_automation.test_canvas_click_tre::test_canvas_text_selection_triple_click`**
   - Symptom: `canvas did not change after triple-click (pixel_diff_ratio=0.0000); selection highlight should paint pixels even at low threshold`
   - Likely fix area: `src/ui/PdfCanvas.cpp::mouseDoubleClickEvent` / triple-click handling — triple-click word/paragraph selection may not be implemented or not wired for the current tool.
   - Priority: **P2**

5. **`tests.ui_automation.test_canvas_phase3::test_canvas_phase3[canvas_annot_handle_drag_corner]`**
   - Symptom: `corner handle drag did not resize: orig=Rect(200, 300, 400, 400), new=Rect(200, 300, 400, 400), dx=0.0, dy=0.0`
   - Likely fix area: `src/annotations/AnnotationTool.cpp` / `src/ui/PdfCanvas.cpp` resize-handle hit testing; or test infra synthetic mouse drag not generating the right modifier/timing sequence (could be CAT-B). **Investigate handler registration first.**
   - Priority: **P2**

6. **`tests.ui_automation.test_canvas_phase3::test_canvas_phase3[canvas_annot_handle_drag_edge]`**
   - Symptom: same pattern as #5, edge handle.
   - Likely fix area: same as #5
   - Priority: **P2**

7. **`tests.ui_automation.test_canvas_phase3::test_canvas_phase3[canvas_text_selection_drag]`**
   - Symptom: `text selection drag: clipboard empty after all attempts`
   - Likely fix area: `src/ui/PdfCanvas.cpp` text selection → clipboard copy on mouse-drag, OR clipboard not flushed before pywinauto reads it.
   - Priority: **P2**

8. **`tests.ui_automation.test_canvas_phase3::test_canvas_phase3[canvas_text_selection_triple_click]`**
   - Symptom: `triple-click: clipboard empty after all attempts`
   - Likely fix area: same as #4 (triple-click word selection) plus clipboard copy.
   - Priority: **P2**

---

## CAT-B: Test infrastructure issues

### Sub-cluster B1 — Dialog widget-reachable probes (`test_dialog_reachability_p0`, 18 failures)

Pattern: `widget with suffix 'opens|closes_cleanly|widget_audit' not found in <Dialog>` for `batchDialog`, `compareDialog`, `metadataDialog`, `preferencesDialog`, `printDialog`, `printPreviewDialog`.
- Likely cause: The probe expects a child widget literally named `opens` / `closes_cleanly` / `widget_audit` — these look like test-fixture sentinel widgets that were never added to the dialogs, OR the dialog doesn't open at all (which the test should handle as a separate failure mode).
- Fix: `tests/ui_automation/test_dialog_reachability_p0.py` — clarify whether these suffixes refer to test cases (re-read fixture) or expected widgets, then either add the widget objectNames in `src/ui/*Dialog.cpp` or drop the false probe.
- All 18 share root cause.
- Priority: **P1**

### Sub-cluster B2 — Dialog open-via-menu probes (`test_ui_dialog_coverage`, 11 failures)

Pattern: `<dialogName> did not open via menu '<Menu -> Item>'` for batchDialog, combineFilesWizard, compareDialog, optimizeDialog, preflightDialog, customizeToolbarsDialog, tabOrderDialog, watermarkDialog, metadataDialog, printDialog, printPreviewDialog.
- Likely cause: Menu paths don't exist in current PDFE menu structure (no "Tools → Batch Process...", "Tools → Compare Documents...", "Tools → Optimize PDF...", "Tools → Preflight...", "Tools → Add Watermark...", "View → Customize Toolbars...", "Forms → Tab Order...", "File → Combine Files...", "File → Document Properties...", "File → Print...", "File → Print Preview..."). Some of these dialogs may not exist as Qt classes either.
- Fix path: (a) confirm which dialogs actually exist in `src/ui/`; (b) for missing dialogs, the tests are speculative coverage — either implement the dialogs (CAT-A escalation) or mark tests xfail with reference to known_issues. (c) for existing dialogs, fix menu strings in test.
- All 11 share root cause.
- Priority: **P1**

### Sub-cluster B3 — Sign / Compliance / Batch widget naming (`test_phase7_new_features`, 6 failures)

- `TestSignDialogTSA::test_tsa_checkbox_exists` — `tsaCheckBox not reachable in SignDialog`
- `TestSignDialogTSA::test_tsa_url_edit_exists` — `tsaUrlEdit not reachable in SignDialog`
- `TestComplianceDialogPdfUA::test_progress_bar_exists` — `progressBar not reachable in ComplianceDialog`
- `TestBatchDialog::test_action_list_and_buttons_exist` — `actionListWidget not reachable`
- `TestBatchDialog::test_error_log_exists` — `batchErrorLog not reachable`
- `TestBatchDialog::test_progress_bar_exists` — `batchProgressBar not reachable`
- `test_features_phase7[sec_sign_timestamp_present]` — `tsaCheckBox not found in Sign dialog`
- `test_final_phase8[field_signature_fill_value]` — `Sign dialog missing fields: ['pfxPathEdit','pfxPasswordEdit','reasonEdit','locationEdit']`
- Likely cause: The Sign/Compliance/Batch dialogs may exist but their child widgets have different `objectName`s than the tests expect, OR the dialogs themselves aren't being opened (the parent dialog may fail before child widget lookup).
- Fix: Cross-reference test expectations vs. actual `src/ui/SignDialog.cpp`, `src/ui/ComplianceDialog.cpp`, `src/ui/BatchDialog.cpp` objectNames. Either rename test or rename widget. Could escalate to CAT-A (missing widgets in production dialog).
- All 8 share root cause.
- Priority: **P1**

### Sub-cluster B4 — Menu paths in `test_menu_navigation_p0` (3 failures)

- `menu_open_valid_pdf`: `'File -> Open...'` — "Open..." menu item not found
- `menu_verify_signatures`: `'Security -> Verify Signatures'` not found
- `menu_tools_compliance`: `'Tools -> Compliance...'` not found
- Likely cause: Menu strings drift (e.g. "Open File..." vs "Open..."), OR an i18n/keyboard-accelerator stripping bug in the helper.
- Fix: `tests/ui_automation/test_menu_navigation_p0.py` — verify against `src/ui/MainWindow.cpp` actual `QAction::text()`.
- Priority: **P2**

### Sub-cluster B5 — Synthetic-event handler registration (`test_canvas_coords_p2`, 4 of 6 failures)

- `canvas_text_selection_triple_click`: `handler not registered: canvas_text_selection_triple_click`
- `canvas_canvas_rubber_band_select`: `handler not registered: canvas_canvas_rubber_band_select`
- `canvas_annot_handle_drag_rotate`: `handler not registered: canvas_annot_handle_drag_rotate`
- `canvas_sticky_note_drag_popup`: `handler not registered: canvas_sticky_note_drag_popup`
- Likely cause: The pytest parametrized table includes scenarios whose handler functions were never implemented in the synthetic-event dispatcher.
- Fix: Locate dispatch table in `tests/ui_automation/` (likely `synthetic_events.py` or similar), add the missing 4 handlers, OR mark them xfail.
- Priority: **P2**

### Sub-cluster B6 — Canvas annot move (`test_canvas_coords_p2`, 2 of 6 failures)

- `canvas_annot_move_drag`: `annotation not committed: tree 0 -> 0`
- `canvas_canvas_click_away_deselect`: `sticky not created`
- Likely cause: The synthetic-event sequence does not produce a complete move/create (no widget commit). Could be that the test driver fires `mousedown→mouseup` too fast for PDFE to register a click vs. drag.
- Fix: Tune timing in synthetic event helpers; OR escalate to CAT-A if real-mouse repro fails too.
- Priority: **P3**

### Sub-cluster B7 — Canvas phase3 rubber-band (`test_canvas_phase3`, 1 of 5 failures)

- `canvas_canvas_rubber_band_select`: `failed to create 2 annotations: tree 0 -> 0`
- Likely cause: Rubber-band select → multi-create flow timing / synthetic events.
- Fix: Same as B6.
- Priority: **P3**

### Sub-cluster B8 — Annotation color dialog ink (`test_annotations_p1`, 1 failure)

- `annot_ink_set_color`: `Ink: QColorDialog did not appear after tab activate + tree select`
- Likely cause: Known from `pipeline_h_triage.md` — `QColorDialog::exec()` is system-modal and pywinauto cannot dismiss it. The test was on a known-flaky list from prior pipelines. The "did not appear" symptom suggests the dialog opened on a different display/desktop or was suppressed.
- Fix: `tests/ui_automation/test_annotations_p1.py` — add color-dialog dismissal helper, OR mark the `annot_*_set_color` family xfail with reference to existing known issue.
- Priority: **P3**

### Sub-cluster B9 — Harness smoke baseline (1 failure)

- `tests.test_ui_harness_smoke.TestUIHarnessSmokeTest::test_launch_and_locate_menu_bar` — `NCC=0.5990 < threshold 0.99`
- Likely cause: Stale `golden_path("smoke", "harness_baseline")` — current screenshot differs significantly. Could be a real UI change (panels visible, theme drift) but the test is a sanity smoke and threshold 0.99 is very strict.
- Fix: Re-capture golden after confirming current UI is correct; lower threshold if appropriate.
- Priority: **P3**

---

## CAT-C: Documented limitations

**None.** All 66 failures and 4 timeouts are categorizable as A, B, or D. No new `PDFE-LIMITATION-*` entries are needed at this triage stage. If post-investigation a Cluster-#1 dialog turns out to be deliberately removed/never-shipped, it may convert to a CAT-C entry then.

---

## CAT-D: Phase-2 environment / runner issues

1. **`tests/test_ui_annotations.py` (1200 s timeout, 45 collected items)**
   - Symptom: Each test ~30-60 s (process launch + UIA tree wait + per-test interaction). 45 × ~35 s ≈ 1575 s. Budget is 1200 s.
   - Cause: per-file budget too small for files with many parametrized tests.
   - Fix: `tests/scripts/run_ui_suite.py` — either (a) bump default `--per-file-timeout` for this file, (b) split the file into `test_ui_annotations_part_a.py` etc., or (c) move the heaviest tests into a slower-tier suite.
   - Priority: **P2**

2. **`tests/test_ui_compliance_accessibility.py` (1200 s timeout, 97 collected items)**
   - Symptom: 97 items × ~12-15 s ≈ 1164-1455 s. Right at the boundary, falls over on slow CI.
   - Cause: same as #1 — file is just too big.
   - Fix: split file in half.
   - Priority: **P2**

3. **`tests/test_ui_preferences_chrome.py` (1200 s timeout, 70 collected items)**
   - Symptom: 70 items × ~17 s ≈ 1190 s. On the edge.
   - Cause: same as #1.
   - Fix: split file or raise budget.
   - Priority: **P2**

---

## CAT-T: Timeouts

1. **`tests/ui_automation/test_precondition_panels_phase4.py` (1200 s timeout, 40 collected items)**
   - Symptom: Log shows `F` (failed) marker for every single test that ran. Got through ~12 tests before timeout. Per-test time ≈ 110 s (long because each test is taking a per-test failure path with retries before giving up).
   - Cause hypothesis: A common precondition (panel reachability) is failing for every test — the test setup looks for a panel widget that no longer exists or no longer has the expected `objectName`. Each test then retries until its own timeout (~100 s) and the file accumulates >1200 s.
   - Investigation approach:
     1. Run the file with `pytest -x` to stop on first failure and see the exact assertion.
     2. If the failure is uniformly "panel <X> not reachable", this is a single CAT-A (panel missing objectName) or CAT-B (selector wrong) — fix the precondition helper and 40 tests recover.
     3. Likely **same root cause as Cluster #1 dialog reachability** — panels and dialogs both use objectName lookup.
   - Priority: **P1** (one fix probably saves 40 tests)

---

## Quick stats / cross-refs

- **Most-failing files:** test_dialog_reachability_p0.py (18), test_ui_dialog_coverage.py (11), test_ui_file_operations.py (10), test_canvas_coords_p2.py (6), test_canvas_phase3.py (5).
- **Dominant failure mode:** widget-not-reachable (~35 of 66 = 53%).
- **Cross-references to prior pipelines:** Cluster #3 canvas-drag failures match the residual set documented in `tests/docs/pipeline_h_triage.md` §3 (P-GAP-92/93/95/96) — those were marked stably failing through Pipeline H. They are still failing here.
- **Likely cross-cutting source change:** None obvious from triage data alone. The 53% widget-not-reachable rate suggests either (a) a recent refactor renamed many widget objectNames, or (b) these tests were added speculatively and never matched real production code.

---

## Suggested Phase-3 sub-tasks (ordered by likely impact)

1. **P3-T1** — Investigate Cluster #1 root cause (single sample dialog, e.g. `metadataDialog`). Likely outcome: ~32 tests recover with one objectName/menu-mapping fix. Effort: M.
2. **P3-T2** — Investigate `test_precondition_panels_phase4` first-failure (probably reveals same root cause as T1). Effort: S.
3. **P3-T3** — Re-capture or fix Phase-5 file-open visual baselines (Cluster #2). Effort: S–L depending on whether it's a real bug.
4. **P3-T4** — Add 4 missing synthetic-event handlers (Sub-cluster B5). Effort: S.
5. **P3-T5** — Annotation undo title-dirty bug (CAT-A items 2, 3). Effort: S.
6. **P3-T6** — Sign/Compliance/Batch dialog widget objectNames (Sub-cluster B3). Effort: S.
7. **P3-T7** — Split or rebudget the 3 oversize timeout files (CAT-D items 1-3). Effort: S.
8. **P3-T8** — Triple-click word selection (CAT-A items 4, 8) and resize-handle drag (CAT-A items 5, 6). Effort: M.
9. **P3-T9** — text-selection drag → clipboard (CAT-A item 7). Effort: S.
10. **P3-T10** — Smoke baseline regen (Sub-cluster B9). Effort: trivial.
