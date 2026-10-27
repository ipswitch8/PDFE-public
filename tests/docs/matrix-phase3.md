# Matrix remediation — Phase 3 (pipeline c82a1f37)

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
**Scope:** the 48 `P3-PRECONDITION` rows + 1 `MAPPING` row enumerated in
`tests/docs/matrix-triage-phase2.md` — establish the precondition (image
selected / form-tool active / TRE atlas chain / text-edit toolbar / dialog
open), probe via existing seams, and re-green from a **real local run**. No row
greened without a live pass.

## Headline

The precondition-then-probe harness already existed (`test_precondition_panels_
phase4.py`, `test_features_phase7.py`, `test_canvas_click_subsystems.py`,
`test_ui_calc_order.py`, `test_missing_features_phase6.py`). P3 was therefore
principally a **live re-measure** of rows the 9a4e2f71-P6 blanket reconcile had
demoted to `blocked`, plus targeted fixes where the probe genuinely failed.

### Result (49 rows, all accounted for)

| Disposition | Count | Rows |
|---|---|---|
| **green** (live pass) | 46 | 14 imagePropertiesPanel + 10 formFieldPropertiesPanel + 12 atlasComparisonDialog + 4 subsystem (visual_diff, glyph_synth_coverage, font_match_panose_top_candidate, scan_style_toggle_persist) + 3 txt_align (center/right/justify) + 2 calc_order (open, drag_reorder) + menu_rotate_view |
| **wont-fix** | 1 | txt_align_left |
| **route → P5** | 2 | atlas_mode_combo_set_persist, annot_textmarkup_set_color |

Committed matrix: green **544 → 590** (+46), wont-fix **10 → 11**, blocked
**220 → 173**. `verify_matrix.py` 787/787. `check_matrix_health.py --runtime-csv`
OK (floor 585, 0 green→red). `generate_matrix.py` idempotent (byte-identical).
Net new greens **46** — AC target was ≥20.

## Live runs (Bash tool, real PDFE.exe, orphan-clean)

- **phase4** (`test_precondition_panels_phase4.py`): **40/40 passed** in 11:12 —
  every imagePropertiesPanel / formFieldPropertiesPanel / atlasComparisonDialog
  row + 4 annotationPropertiesWidget rows. The image-select, form-tool+coord-
  click and TRE atlas-chain preconditions all work on this runner.
- **subsystem** (`test_canvas_click_subsystems.py`): 4 passed, 1 skipped.
- **txt_align** (`test_features_phase7.py -k txt_align`): 3 passed, 1 skipped
  (left, see below).
- **calc_order** (`test_ui_calc_order.py`): 2 passed.
- **menu_rotate_view** (`test_missing_features_phase6.py -k menu_rotate_view`): 1
  passed.

## Fixes applied (test-harness only; no C++ changed)

1. **menu_rotate_view — MAPPING (P2 carry-over).** The dispatch fired
   `invoke_action(..., "rotateViewAction")`, but that is the C++ *variable*
   name; the QAction's real `objectName` is **`actionRotateViewCW`**
   (`MainWindow.cpp:3226`). `invoke_action` returns True on pipe-write success
   regardless of whether any action matched, so the old value silently no-op'd
   yet still greened — a **false-green**. Re-pointed to `actionRotateViewCW`;
   the fire now actually triggers the rotate. (`test_missing_features_phase6.py`)

2. **calc_order_dialog_open / _drag_reorder.** `_open_calc_order` navigated by
   keyboard mnemonics (`Alt+O`, `u`) — flaky on the non-interactive Server-2019
   runner (open skipped ~intermittently). Switched to the deterministic
   `invoke_action` seam (`calculationOrderAction`, `MainWindow.cpp:2814`). A
   seam-opened modal does not take the foreground, so the subsequent list-item
   `click_input()` in drag_reorder missed its selection (before==after);
   added `dlg.set_focus()` after the dialog is visible. Both rows now pass 5/5
   across repeated runs. (`test_ui_calc_order.py`)

3. **Stale calc_order duplicates removed.** `test_broader_phase_post.py` held two
   tests that hardcoded `record_matrix_result(..., "blocked", "no menu wiring in
   MainWindow.cpp")` — a claim **disproven** in P2 (calculationOrderAction IS
   menu-wired at 2814/3552, enabled at 4904) and contradicted by the real probe
   above. Removed so the false claim is not re-asserted every run; the rows are
   owned solely by `test_ui_calc_order.py`.

## txt_align_left — wont-fix (UI-layer), gtest-covered

`alignLeftButton` is the only `setChecked(true)` align button (the default).
The Qt6 UIA bridge **deterministically omits the checked/pressed toggle button**
from the tree at `TextEditToolBar` activation, while its unchecked siblings
(center/right/justify) are exposed and pass. Confirmed across 7 live iterations.
Two harness approaches were tried and **rejected** because neither surfaced it:
(a) a reconnect-capable `find_widget` poll; (b) clicking a present sibling to
un-check the target then re-finding it. The widget genuinely exists and is
verified headlessly by gtest `test_chrome_phase9.cpp`
(`findChild<QToolButton>("alignLeftButton")` `ASSERT_NE nullptr`). Dispositioned
**wont-fix** (analogous to the accepted `printDialog.rangeButtonGroup` Qt-internal
closure). The handler returns `blocked` (not `red`) for this one button so it
skips cleanly and the wont-fix status survives green-wins aggregation; the other
three align rows keep the strict red contract.

## Rows routed to P5 (documented reroute, not faked)

- **atlas_mode_combo_set_persist.** Single-open `atlasModeCombo` behaviour IS
  covered green by `atlas_mode_visual_diff`. The *persistence-across-reopen*
  probe deterministically fails at the **second** TRE-edit/atlas-dialog entry
  after ESC (fragile reopen), skipped 2/2. Owner → phase-5.
- **annot_textmarkup_set_color.** Precondition is met, but the QColorDialog
  Z-order env race (`CI-LIMITATION-COLORDIALOG-TIMING`, ~30% miss on the
  non-interactive session) blocks a deterministic colour read — an **env**
  limitation, not a precondition gap. Owner → phase-5-env.

## Aggregation method

Each partial module writes a full runtime CSV under `tests/_runtime/`. The
committed matrix was updated by green-wins merge (`run_ui_suite.aggregate_matrix`,
red-dominates so no failure is masked) over only the **authoritative** CSVs
(`ui_test_matrix_20260703_{221532,222546,222622,230112,230217}.csv`); superseded
pre-fix CSVs carrying a stale `txt_align_left=red` were excluded. The 3 non-green
dispositions were applied first so green-wins/red-dominates could not override
them. Result verified: exactly 49 rows changed (46→green, 1→wont-fix, 2 owner
reroute), 0 whole-matrix green→red, 0 target red.
