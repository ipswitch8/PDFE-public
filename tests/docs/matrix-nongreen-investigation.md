# Matrix non-green investigation (pipeline follow-up to 9a4e2f71)

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


**Date:** 2026-07-03
**Scope:** understand every non-green row in `tests/ui_automation/matrix/ui_test_matrix.csv`
after the phase-6 reconciliation (434 green / 335 blocked / 6 xfail / 5 wont-fix /
5 planned / 2 skip = 787), and define how to test/remediate each properly.

## Data sources
- Committed matrix `ui_test_matrix.csv` (335 `blocked` rows after 9a4e2f71 P6 reconcile).
- Final green validation run **28615963046** `ui-junit-merged.xml` (3401 tests, 2968
  pass, 433 skip, 0 fail) — per-test outcomes + skip reasons.
- Reconcile-source aggregate (run 28554887271) `ui_test_matrix_aggregate.csv` — per-row
  measured status + notes.

## Headline finding
The 335 "blocked" rows are **not** 335 real coverage gaps. They split into a large
**false-block** bucket caused by a matrix-aggregation bug, a set of genuine harness
gaps, and a set of honestly-un-testable rows (absent features / product limitations).

## Root-cause categories (sum = 335)

### 1. Aggregation false-block — ~111 rows — FIX: harness (highest value)
A passing test records the row `green`, but a **different, non-owning** suite
parametrizes over the same `act_`/`dlg_`/`flow_` id, **skips** it (it doesn't own it),
and that skip is auto-recorded `blocked` at `pytest_sessionfinish`
(`tests/ui_automation/conftest.py:575`). `record_matrix_result` is last-write-wins
(`MATRIX_RESULTS[test_id] = {...}`, conftest.py:85) and the cross-file aggregate lets a
`blocked` clobber a `green`. Proven: `test_ui_batch1_actions[zoomInAction]` PASSES and
records `act_zoomInAction=green`, while `test_annotations_p1[act_zoomInAction]` SKIPS the
same row → aggregate = `blocked`. All 131 `act_` rows land here plus several `flow_`/
`field_` rows a batch/p1 suite covers green elsewhere.
**Remediation:** make aggregation **green-wins** — a row is green if ANY suite proves it
green this run (green > red > blocked > skip precedence), OR stop non-owning p1 suites
emitting matrix results for ids they don't own. No new tests, no product change. This
alone recovers ~1/3 of the "blocked" set to green **and** makes the matrix-health gate
strictly more meaningful (green→red still fails).

### 2. Canvas-drag harness gap — ~30 rows — FIX: harness (hard)
Annotation/field CREATE + handle-drag rows (`annot_select_*`, `field_text_create`,
`canvas_annot_handle_drag_*`): creation needs true click-drag rect placement on the
canvas that the current synthetic-click harness can't produce (P-GAP-75). The
functionality is proven headless (gtest `FormCreationTest`, `AnnotationCommands`); only
the UI gesture is un-driveable.
**Remediation:** build a real canvas click-drag primitive (synthetic mouse-down →
move → up with an observable rect delta) behind a `PDFE_TEST_PIPES` seam, then re-point
these rows at it. Highest-effort category.

### 3. Precondition not established — ~31 rows — FIX: harness (medium)
Dialog sub-widget rows that skip because a precondition wasn't set up:
`imagePropertiesPanel` needs an image selected first, `atlasComparisonDialog` needs an
active atlas/CNN compare, `formFieldPropertiesPanel` needs the form tool active.
**Remediation:** the probe test must ESTABLISH the precondition (select an image / open
atlas / activate `formToolAction`) before enumerating the sub-widget, using existing
seams. Medium effort, per-dialog.

### 4. Feature absent from build — ~77 rows — FIX: verify → wont-fix or implement
`menu_*` and some `cmp_*`/`sec_*` rows skip with "X > Y: not present in current pdfe
build" / "no handler" / "EncryptionDialog only exposes AES". These are matrix rows for
actions the app genuinely does not expose (File>New, Edit>Replace, Edit>Preferences,
Forms>TabOrder/SubmitForm, Tools>OCR/TRE/Batch/Compare/Accessibility, View>RotateView/
Theme, RC4 encryption, some PDF/A converters).
**Remediation:** VERIFY each against `src/ui/MainWindow.cpp` — some may exist under a
different objectName (fixable mapping), the rest are honest `wont-fix` (feature not
implemented is not a test failure). Do NOT green these by faking coverage.

### 5. Product / MuPDF semantic gap — ~5 rows — FIX: wont-fix or product
`field_*_set_default`: `pdf_reset_form` does not restore `/DV` even with a `/DV` fixture
— a documented MuPDF semantic gap, a product issue, not a test-layer problem.
**Remediation:** `wont-fix` at the UI layer + (optional) product/MuPDF tracking.

### 6. Qt-internal widget (no objectName) — ~5 rows — FIX: accessible-name or wont-fix
`printDialog.rangeButtonGroup` (a QButtonGroup, not a QWidget), `printPreviewDialog`
zoom/fit actions (Qt-internal, no settable objectName).
**Remediation:** probe by UIA accessible-name fallback where possible; otherwise
`wont-fix` (Qt owns the widget, we can't give it a stable id).

### 7. WinAppDriver unavailable — ~3 rows — FIX: infra
Rows skip with "WinAppDriver service not reachable". Runner infra, not code.
**Remediation:** ensure the WinAppDriver service is up for the run, or re-point these to
the pywinauto path.

### 8. Other / manual triage — ~76 rows — FIX: triage (mixed)
Remaining `dlg_*` panel rows (compliance progressBar, bookmark/comment context menus)
and `annot_textmarkup_*` rows whose recorded reason didn't match a clean bucket. These
need per-row triage and will redistribute across categories 1–7 (many are expected to be
category-1 false-blocks or category-3 preconditions once inspected).

## Proposed remediation phasing (for the planner)
1. **P1 Aggregation fix (cat 1)** — green-wins precedence in conftest/run_ui_suite +
   locking gtest/py unit test; regenerate the matrix from a run → ~111 rows blocked→green.
   Biggest win, lowest risk, gates the rest (re-measures the true remaining set).
2. **P2 Triage sweep (cat 8 + verify cat 4)** — classify the 76 "other" + verify the 77
   "feature absent" against MainWindow.cpp; split into green-able vs honest wont-fix.
3. **P3 Precondition probes (cat 3)** — establish-precondition-then-probe for the
   image/atlas/form-panel dialog rows.
4. **P4 Canvas-drag primitive (cat 2)** — the drag seam + re-point create/handle rows.
5. **P5 wont-fix formalization (cat 4/5/6/7 residue)** — mark genuinely-un-testable rows
   `wont-fix` with reasons (honest, not faked green); infra note for WinAppDriver.
6. **P6 Lock + full validation** — regenerate matrix, run full windows-validation green,
   confirm the matrix-health floor rises to the new (honest) green count.

Each phase re-measures via a real run before the next, so counts are grounded, never
assumed. wont-fix is used ONLY where a feature/product genuinely does not support the
test — never to dodge SKIP=FAIL.
