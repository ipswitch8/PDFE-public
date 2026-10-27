# Post-f7c2a4e8 Followups

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


**Source pipeline:** f7c2a4e8 closeout (2026-05-22)
**Audit performed:** 2026-05-22 after all phase-8 gates passed

This is the input to the next planning pass. Items are organized
by theme, with effort estimate and dependency notes.

---

## Theme A — Dialog widget UIA exposure (the recurring pattern)

Same root-cause family as P-GAP-87 (resolved in f7c2a4e8 phase-5):
several dialog widgets need `setObjectName` audit so UIA can find them.
The fix pattern is well-proven now (1-line per widget; ~10-30 min per dialog).

### A1. P-GAP-102: PreferencesDialog tab_widget_exists
- **Test:** `test_phase7_new_features.py::TestPreferencesDialog::test_tab_widget_exists`
- **Likely fix:** add `setObjectName("preferencesTabs")` to the QTabWidget in PreferencesDialog constructor.
- **Effort:** S

### A2. P-GAP-102: ComplianceDialog progress_bar_exists
- **Test:** `test_phase7_new_features.py::TestComplianceDialogPdfUA::test_progress_bar_exists`
- **Likely fix:** setObjectName on the QProgressBar in ComplianceDialog.
- **Effort:** S

### A3. P-GAP-102: BatchDialog action_list_and_buttons_exist + error_log_exists + dialog_opens
- **Tests:** 3 tests in `test_phase7_new_features.py::TestBatchDialog`
- **Likely fix:** audit BatchDialog constructor; add setObjectName to action-list widget, error-log widget, and dialog itself.
- **Effort:** S

### A4. P-GAP-102: CompareDialog dialog_opens
- **Tests:** `test_phase7_new_features.py::TestCompareDialog::test_dialog_opens`
- **Likely fix:** setObjectName on CompareDialog widget.
- **Effort:** S

---

## Theme B — Stale xfail marker cleanup (xpass triage)

### B1. 8 XPASSED in f7c2a4e8 final sweep
- 1 in test_annotations_p1.py
- 2 in test_canvas_click_tre.py
- 1 in test_canvas_coords_p2.py
- 1 in test_canvas_phase3.py
- 3 in test_page_operations_p1.py
- **Approach:** confirmation-sweep methodology (run twice, identify stable XPASS, remove marker, verify clean).
- **Effort:** S-M (mostly mechanical, but each marker needs verification)

### B2. P-GAP-96 flaky pair
- annot_ink_set_color + test_txt_tre_edit_regular_font
- **Approach:** investigate test-ordering pollution; fixture-isolation hooks; or accept as permanent strict=False xfail
- **Effort:** S-M

---

## Theme C — Pages workflow tests (the deferred coverage)

### C1. P-GAP-99 (F4): broader Pages-menu workflows
- Pages → Move Up / Move Down / Move To Start (3 tests)
- Pages → Insert From File... (1 test, needs file-open dialog driving)
- **Scaffold:** documented in pipeline_g_phase7_deferred.md
- **Effort:** M (4 new tests with testSelectPagesPipe + menu dispatch)

### C2. P-GAP-93: page_operations_p1 residual 18-test cluster
- 5 sub-clusters: menu/dialog driving, move-page handlers, gesture-target tests, drag-drop, undo/redo deps, large-doc fixtures
- **Approach:** triage by sub-cluster; some need Q_INVOKABLE seams (movePageProgrammatic-style for keyboard moves); some need testInsertFromFilePipe; others need fixture verification.
- **Effort:** L (3-5 hours; this is the largest single backlog item)

### C3. P-GAP-95: calc-order dialog drag-reorder
- **Approach:** add Q_INVOKABLE drag seam on calc-order dialog widget, similar to PagePanel::movePageProgrammatic.
- **Effort:** S

---

## Theme D — Synthetic-event Qt spontaneity (the deep finding)

### D1. P-GAP-98: synthetic events don't trigger tool side-effects
- **Affects:** P-GAP-92 family (canvas handle drag, text-selection, rubber-band, triple-click)
- **Root cause:** QMouseEvent::spontaneous() returns false for events created by QApplication::sendEvent; Qt6 click-timing thresholds; possibly source-flag filtering
- **Approach options** (each ~1-2 hours research):
  - **(a)** Use QTest::mouseClick / QTest::mousePress from QtTest module — sets spontaneity properly
  - **(b)** Use `Qt::MouseEventSynthesizedByQt` source via longer QMouseEvent constructor
  - **(c)** Use `QWindowSystemInterface::handleMouseEvent` (true spontaneous events)
  - **(d)** Use Win32 `SendInput` for OS-truthy events
- **Effort:** M (research + implement winner)

### D2. P-GAP-92: canvas synthetic-interaction tests
- 7 canvas tests in P-GAP-92 family that would PASS once D1 lands
- **Effort:** S after D1

---

## Theme E — Security coverage (deferred from phase-7)

### E1. P-GAP-100 (G1): UI-level redaction roundtrip
- **Fixture needed:** redaction PDFs exist
- **Infrastructure needed:** Python helper using pymupdf to verify redacted bytes are absent from saved file
- **Test scaffold:** open fixture → addRedactionMark pipe → Apply menu → testSaveAsPipe → pymupdf verify
- **Effort:** M (writing the pymupdf verifier is most of the work)

### E2. P-GAP-101 (G3): JS form-action sandbox
- **Fixture needed:** hostile-JS PDF (must generate via pymupdf or qpdf)
- **Test scaffold:** open hostile PDF → trigger form action → assert no external process + no network
- **Effort:** M

### E3. P-GAP-78 residuals: visible/invisible signature workflows
- 2 xfailed tests; need PFX fixture validation + signing workflow
- **Effort:** M

### E4. G4-v2: password-strength ENFORCEMENT (vs just wiring)
- Current G4 verifies the strength label reacts; doesn't verify weak password blocks save
- **Approach:** type weak password → assert OK/Save button disabled
- **Effort:** S

---

## Theme F — Chronic flakes

### F1. P-GAP-74: precondition_panels_phase4 timing
- **Status:** wait bumped 2.0→4.0s (flip count 0 in measurement), but 17 tests still file-level xfailed
- **Approach:** investigate atlas-dialog state timing; might need per-test pipe-based dispatch (testAtlasReadyPipe?)
- **Effort:** L

### F2. P-GAP-79: search/replace workflow harness
- 2 tests xfailed; needs investigation
- **Effort:** M

### F3. P-GAP-82: bookmark panel state-dependent
- 1 test; needs specific bookmarks fixture + sequence
- **Effort:** S-M

### F4. P-GAP-84: view rotate environment-dependent
- 1 test; likely DPI/zoom-state issue specific to test rig
- **Effort:** S-M

---

## Theme G — Test infra improvements

### G1. test_dialog_reachability_p0.py timeout override
- Already applied 1500s override in this pipeline's phase-8 closeout
- Verify in next sweep it completes without crash
- **Effort:** 0 (already done; just needs sweep confirmation)

### G2. Sweep-runner JSON wrapper parse_summary bug (phase-1 sweep had it)
- The custom Python wrapper used in phase-1 sweep had a parse bug that recorded all-zeros into JSON
- Either fix the wrapper (similar to b5e3a9f2 phase-2's sweep_runner.parse_summary fix) or remove the wrapper
- **Effort:** S

---

## Theme H — Documentation hygiene

### H1. Matrix CSV: review STALE entries
- P-GAP-85 + P-GAP-88 are both "STALE matrix row" cases
- Audit the full ui_test_matrix.csv for other stale entries that might pre-emptively fail
- **Effort:** M

### H2. Test count discrepancy: pipeline f7c2a4e8 final sweep showed -124 PASS vs b5e3a9f2 closeout
- The reduction reflects file-level xfails being removed; many tests now run + fail rather than skipping
- Document this in the strategy doc so the regression isn't misread as quality loss
- **Effort:** 0 (already partially documented; needs one more note)

---

## Summary

| Theme | # items | Effort total |
|---|---:|:--|
| A. Dialog UIA exposure | 4 | 4×S |
| B. xpass / flake cleanup | 2 | S, S-M |
| C. Pages workflows | 3 | M, L, S |
| D. Synthetic-event spontaneity | 2 | M, S-after-D1 |
| E. Security coverage | 4 | M, M, M, S |
| F. Chronic flakes | 4 | L, M, S-M, S-M |
| G. Test infra | 2 | 0, S |
| H. Documentation | 2 | M, 0 |
| **Total** | **~23** | mixed |

**Recommended phasing for the next pipeline** (planner refines):

1. **Phase-1 inventory + cheap-win confirmation** (--runxfail of the 8 XPASS files; phase-1 cheap-win list for phase-7 un-marking)
2. **Phase-2 Theme A (dialog UIA exposure)** — 4 dialogs, all S effort, same pattern; one phase
3. **Phase-3 Theme C small (C3 calc-order) + G2 wrapper fix** — quick wins
4. **Phase-4 Theme D (synthetic-event spontaneity)** — research + implement; unblocks P-GAP-92 (D2 followup)
5. **Phase-5 Theme C medium (C1 broader Pages)** — 4 new tests using testSelectPagesPipe
6. **Phase-6 Theme E security coverage** (E1 redaction roundtrip + E2 JS sandbox + E4 password strength v2)
7. **Phase-7 Theme C large (C2 page_operations residual cluster)** — biggest single backlog item
8. **Phase-8 Theme B cleanup + Theme F flake investigation + closeout**

P-GAP-74 (F1) likely deferred again — needs the Q_INVOKABLE atlas-dialog seam which is a phase-of-its-own.
