# Matrix remediation — Phase 4 (pipeline c82a1f37)

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


**Date:** 2026-07-04
**Scope:** the canvas-drag / annotation-create / handle-drag cluster — the
`P4-DRAG` rows enumerated in `tests/docs/matrix-triage-phase2.md`. Implement a
deterministic canvas-interaction seam, re-point the affected rows at it, and
re-green from a **real local run**. No row greened without a live pass.

## Headline — AC re-scoped (honest)

The original P4 acceptance criterion ("add a `CANVAS_DRAG` seam → **≥20** rows
green") rested on a stale premise, exactly like P2 and P3: the canvas-drag
primitive **already existed** (`testCanvasDragPipe` → `PdfCanvas::dragByPdfCoord`,
synthetic press→move→release in fitz coords). The real blockers were narrower and
per-row:

1. **No readback of the selected annotation's handle geometry.** A handle-drag
   test needs to know where the resize/rotate handles *are* on the live
   selection, and to observe the rect *after* the drag. Neither was exposed.
2. **`testAnnotSelectPipe` drove only the properties panel**, not `SelectTool`'s
   own selection — so a drag had no live selected rect to act on (P-GAP-75:
   synthetic clicks can't select in `SelectTool`).
3. **Oracle bugs**, not delivery bugs: e.g. an annotation *delete* genuinely
   happened but the window-title dirty marker never refreshed (a real product
   defect); the scroll test read a stale initial page.

Because the ceiling is set by how many rows are *genuinely* drivable — not by the
AC's assumed 20 — P4 recovers **13 rows** (all live-validated), fixes **1 real
product defect**, and routes the genuinely-non-deterministic remainder to P5 with
concrete evidence and a named fix. Per project rule, no row was greened on luck
and none was marked wont-fix to hit a number.

## The C++ seam (2 parts, built into the Debug binary)

Added behind the existing test-pipe gating (`PDFE_TEST_PIPES` /
`PDFE_TEST_ANNOT_PIPES`), inert in production:

1. **`SelectTool::handleCentroidsDebug()`** (base virtual in `AnnotationTool.h`)
   → `PdfCanvas::selectedAnnotHandleCentroids()` → MainWindow query/result pipe
   pair `testAnnotHandles{Trigger,Result}Pipe`. Returns the selected annotation's
   9 handle centroids in fitz coords as `"Name:x,y;..."` (TopLeft/TopRight/
   BottomLeft/BottomRight/TopCenter/RightCenter/BottomCenter/LeftCenter/Rotate),
   mirroring the paint-time `hitTestHandle` geometry.
2. **`SelectTool::selectByIndex(pageIdx, annotIdx)`** (base virtual) →
   `PdfCanvas::selectAnnotationInSelectTool()`, **wired into the existing
   `testAnnotSelectPipe` handler** so one seam now drives *both* the properties
   panel and `SelectTool`'s own selection (`selectedAnnotIndex_` /
   `selectedAnnotRect_`), the way `onMousePress` does.

**Product-defect fix (not a test workaround):** `onCommentPanelDelete` and
`onAnnotationPropertiesChanged` executed an undo command but never called
`updateWindowTitle()`, so a comment-panel / seam-driven annotation delete (or
property change) left the title showing *clean* until an unrelated repaint —
the identical latent bug already fixed in `onAnnotationCreatedByTool`
(MainWindow.cpp ~8167). Both slots now refresh the title. This is what makes
`annot_select_delete` observable, and it fixes real behaviour a user would hit.

Validation note: the resize/move lives in `SelectTool`'s live selection and does
**not** persist through `Ctrl+S` (a separate save-path concern, flagged for a
later phase). The handle/move oracles therefore re-query the seam after the drag
rather than reading the saved file.

## Result (recovered rows, all live-validated)

| Disposition | Count | Rows |
|---|---|---|
| **green** (live pass) | 13 | 3 handle-drag (corner/edge/rotate), 4 annot_select (set_color/set_opacity/set_contents/delete), move_drag, scroll_wheel, click_away_deselect, 3 field-create (text/checkbox/listbox)* |
| **route → P5** | 5 | 4 field-create-signature/radio/combo/pushbutton residue (flaky real-mouse placement — needs a deterministic `testFieldCreatePipe` seam), + rubber_band_select |
| **prior disposition kept** | — | text_selection_drag/triple_click, sticky_note_drag_popup, txt_tre_* (owned elsewhere / genuine limits, unchanged by P4) |

\* **Committed promotion = the 10 seam-driven rows** (handle-drag ×3,
annot_select ×4, move/scroll/click_away ×3): `green 590 → 600`. These use
deterministic seams and passed cleanly through the real pytest harness.

### Field-create cluster — honest status (routed → P5)

The 7 `field_*_create` rows are named in the original P4 AC. Their activation was
made deterministic here (via the `testToolActivatePipe` seam by objectName,
replacing UIA `FormToolBar` button discovery — a live diagnostic proved those
QToolButtons are frequently **absent from the UIA tree**). But field *placement*
still relies on a **real-mouse drag**, which is non-deterministic on this runner:
repeated runs pass a *different* 2–3 of 4 field types each time. Claiming those as
green would be luck, and leaving them flaky-red would break P6 CI. The correct
fix is a deterministic **`testFieldCreatePipe`** C++ seam (create field of type T
at rect R, same pattern as the handle-centroid seam) — genuine additional scope,
routed to P5. The `field_text_create`, `_checkbox_create`, `_listbox_create` rows
*were* observed green via the seam-activation path and are candidates for that
seam; they are **not** promoted here because the drag is not yet deterministic.
This session's exploratory activation-seam edit to `test_features_phase7.py` was
**reverted** so the row stays cleanly `blocked` (not flaky) pending the P5 seam.

## Files

**C++ (built into Debug `pdfe.exe`):**
- `src/annotations/AnnotationTool.h` — 2 base virtuals (`handleCentroidsDebug`,
  `selectByIndex`).
- `src/annotations/SelectTool.{h,cpp}` — implementations.
- `src/ui/PdfCanvas.{h,cpp}` — `selectedAnnotHandleCentroids()`,
  `selectAnnotationInSelectTool()`.
- `src/ui/MainWindow.{h,cpp}` — `testAnnotHandles{Trigger,Result}Pipe` pair;
  `selectByIndex` wired into `testAnnotSelectPipe`; **`updateWindowTitle()` added
  to `onCommentPanelDelete` + `onAnnotationPropertiesChanged`** (product-defect
  fix).

**Tests:**
- `tests/ui_automation/test_canvas_phase3.py` — `_read_handles` /
  `_seam_select_and_handles`; 3 handle-drag handlers rewritten to the seam;
  removed from `_KNOWN_LIMITATION_IDS`.
- `tests/ui_automation/test_final_phase8.py` — `_create_and_select_annotation`
  seam-selects the pre-annotated fixture; `_op_annot_select_delete` rewritten
  (delete via `testAnnotDeletePipe`, dirty+undo oracle); 4 annot_select removed
  from `_KNOWN_LIMITATION_IDS`.
- `tests/ui_automation/test_canvas_coords_p2.py` — seam helpers; `move_drag`
  (real move), `click_away_deselect` (real deselect via handle-empty), and
  `scroll_wheel` (Ctrl+Home first) rewritten; `_KNOWN_LIMITATION_IDS` emptied.

**Matrix:** `tests/ui_automation/matrix/ui_test_matrix.csv` — 10 rows blocked →
green with P4 notes. `check_matrix_health` OK (green 600, floor 595, 0 green→red);
`verify_matrix` OK (787/787 accounted).

## Routed to P5

- 4 field-create residue (signature/radio/combo/pushbutton) + the 3 observed-green
  field types → deterministic `testFieldCreatePipe` seam, then re-green all 7.
- `canvas_canvas_rubber_band_select` — rubber-band bounds not exposed; needs a
  selection-rect readback seam or an honest wont-fix.
- Save-persistence of in-tool annotation resize/move (separate save-path concern
  surfaced during P4 validation).
