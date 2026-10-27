# Phase 8 — Full Discovery Run (pipeline c4a9e271)

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


**Run date:** 2026-06-24
**Binary under test:** `build/vs2022/bin/Release/pdfe.exe` (built 2026-06-23, marker `c568b30`)
**Engine:** `tests/scripts/run_ui_suite.py` (per-file pytest subprocess isolation)
**Raw artifacts (gitignored):** `tests/_runtime/discovery_phase8/`
 — `ui-junit-merged.xml`, `ui-suite-summary.json`, `per-file/*.xml|*.log`, `runner.console.log`, `evidence/*.png`

---

## 1. Run mechanics & no-hang guarantee (criterion 1)

The suite ran via the project's established per-file isolation runner. Each of the 110 UI
test files executed in its **own** pytest subprocess with:

- a **per-file wall-clock timeout** (default 1200 s; documented overrides up to 2700 s for the
  known all-pass-but-slow files `test_ui_compliance_accessibility.py`, `test_ui_annotations.py`,
  `test_ui_text_editing.py`, `test_ui_forms.py`);
- an inner `pytest-timeout=120 --timeout-method=thread` per-test ceiling;
- a **defensive `taskkill /F /IM pdfe.exe`** between every file (watchdog kill of any orphan
  before the next file starts);
- **up to 2 flaky-file reruns** — a file with rc≠0/timeout is re-run whole; a transient UIA
  flake self-heals on a fresh launch, a genuine failure fails all 3 attempts. (SKIP=FAIL is
  preserved: every test still runs.)

**Pass/fail was read from the merged JUnit XML, never from a process exit code** (GAP-A15).
Hidden problems (files whose pytest crashed with rc≠0 or hit the outer timeout, which can drop
testcases from the XML) are surfaced explicitly by the runner as `files_with_problems`.

### Totals (from `ui-junit-merged.xml`)

| Metric | Value |
|---|---|
| UI test files run | 110 |
| Wall clock | 22 710 s (378.5 min / ~6.3 h) |
| Tests collected/run | **3369** |
| **Passed** | **2931** |
| Failures | 11 |
| Errors | 0 |
| Skipped (xfail/precondition) | 427 |
| Files with rc≠0 after all reruns | 8 |
| No file hung past its timeout | ✔ (0 timeouts; all 8 problem files exited rc=1 with results) |

No file hung; the 8 rc=1 files all produced complete JUnit output (the failures below), so there
are no "silent" lost testcases.

---

## 2. ≥15 distinct product interactions green (criterion 5)

**2931 tests passed**, spanning every UI domain — vastly exceeding the 15-interaction floor.
A non-exhaustive sample of green product interactions verified this run (each is a distinct
interaction in a passing file):

file-open / save / save-as / close · page insert / delete / move / rotate / crop ·
zoom in/out / fit-width / fit-page / view-rotate · search / replace · bookmark panel ·
thumbnail panel · FreeText / StickyNote / Highlight / Underline / Strikethrough / Ink annotate ·
comment panel · form field create / fill / calc-order / submit · redaction mark+apply roundtrip ·
TRE scanned-page OCR · per-line font-size span-click (Goal A) · encryption open / sign / verify /
LTV-reachable · PDF/A-1b + PDF/A-2b convert+validate · PDF/UA check · all 5 Preferences tabs ·
batch-1/2/3 action fire-and-survive (98 act_ rows) · titlebar/statusbar/theme/menu-walk.

---

## 3. Failure triage — every red item (criteria 2, 3, 4, 6)

The 11 JUnit failures collapse to **4 root causes**. Each red item below carries real evidence:
either saved screenshots under `tests/_runtime/discovery_phase8/evidence/` **or** the captured
pipe/diag string from the run (criterion 6 — see §5 for the evidence index).

### Summary

| # | Test | Cluster | Classification | Priority | Evidence |
|---|------|---------|----------------|----------|----------|
| 1 | `test_render_text_edit::test_vector_text_edit_visible` | B | **product-defect (regression)** | **P1** | `clusterB_textedit_before/after.png` + diag |
| 2 | `test_render_text_edit::test_vector_text_edit_redo_stays_visible` | B | **product-defect (regression)** | **P1** | `clusterB_textedit_redo.png` + diag |
| 3 | `test_render_oracle_proof::test_vector_text_edit_visible` | B | **product-defect (regression)** | **P1** | diag `span "Invoice" selected, ovsz=0.00` |
| 4 | `test_render_text_edit::test_vector_text_edit_undo_reverts` | B | env-flake (did **not** reproduce) | P2 | `clusterB_textedit_undo.png` (heading restored) |
| 5 | `test_render_oracle_proof::test_ctrl_z_reverts_canvas` | B | env-flake (did **not** reproduce) | P2 | re-run passed; in-edit undo works |
| 6 | `test_render_page_ops::test_rotate_page_cw_changes_canvas` | A | **stale-golden** (false-red) | P2 | `clusterA_page_ops_rotate_cw_{live,golden,diff}.png` |
| 7 | `test_render_view_nav::test_rotate_page_cw_changes_canvas` | A | **stale-golden** (false-red) | P2 | `clusterA_view_nav_rotate_cw_{live,golden,diff}.png` |
| 8 | `test_render_annotations::test_rectangle_shape_changes_canvas` | A | **stale-golden** (false-red) | P2 | `clusterA_annot_rectangle_{live,golden,diff}.png` |
| 9 | `test_render_redaction::test_redaction_mark_paints_on_canvas` | A | **stale-golden** (false-red) | P2 | `clusterA_redaction_mark_{live,golden,diff}.png` |
| 10 | `test_render_coord_rotation::test_rotated_text_maps_on_screen` | A | **stale-golden** (false-red) | P2 | `clusterA_coord_rotation_{live,golden,diff}.png` |
| 11 | `test_dialog_reachability_p0::…[dlg_commentPanel_commentContextMenu]` | C | **harness-gap** | P2 | pipe msg: widget-not-found |

Plus one self-healed flake during the run itself: `test_features_phase7.py` failed attempt 1,
passed on rerun (rc=0) — environment/UIA contention, no product issue.

Accounting: **11 failures = 3 product-defect + 5 stale-golden + 1 harness-gap + 2 flake.**

---

### Cluster B — vector heading collapses on commit  ▸ PRODUCT DEFECT (P1, regression)

**Symptom (reproduced with screenshots).** Open `tests/fixtures/invoice_vector_heading.pdf`,
activate the vector text-edit tool (`textEditToolAction`), double-click the bold "Invoice"
heading, type "Quote", **commit** (tool switch). The rendered heading **collapses** — the
heading-bbox ink ratio drops from ~0.130 to ~0.038 (only ~29 % kept; the oracle requires ≥60 %).
The saved PNGs show "Invoice" reduced to a couple of stray glyph fragments while the page logo
and all other content remain intact. `redo` re-applies the broken commit and re-collapses the
heading. (`clusterB_textedit_before.png` → full "Invoice"; `clusterB_textedit_after.png` /
`_redo.png` → blank/fragments.)

**Scope refinement from controlled re-capture.** On isolated rerun, the two *in-edit-undo*
assertions (#4 `undo_reverts`, #5 `ctrl_z`) **passed** — in-edit undo correctly restores the
heading (`clusterB_textedit_undo.png` shows full "Invoice" back). So the genuine defect is **the
commit/redo render path only** (3 of the 5 Cluster-B failures); the 2 undo failures in the big
run were UIA-timing flakes, not the defect.

**Root-cause hypothesis (C++).** Two candidate mechanisms, both introduced after the goldens'
commit `4b7db73` and bounded to a small surface. The **runtime diag favors mechanism (b)**:

- **(a) click-steal redirect** — commit `aad6328` added a guard in
  `src/ui/PdfCanvas.cpp:667-676`: when `TextEditTool::spanIndexAtPoint(pdfPoint) < 0`
  **and** `Page::hasImageContentAt(pdfPoint)` is true, the left-click is redirected via
  `emit regionToolRequested(...)` → `MainWindow.cpp:2027` switches to the TRE/region tool, so the
  heading is edited as a *scanned region* (Tr=3 invisible-text + ImageReplacer bitmap) → blanks.
  `Page::hasImageContentAt` (`src/core/Page.cpp:303-322`) tests only image-**block-bbox**
  containment, so the INVOICE logo block (upper-left, same region as the heading) coarsely
  "covers" the heading center.
- **(b) commit/substitution render path** — the oracle diag for failure #3 records
  `span "Invoice" selected, ovsz=0.00`, i.e. the heading span **was** selected (the click was
  *not* stolen). That points the cause at how the **committed** vector edit is re-rendered —
  the per-span `Tr=3`/Type-3 routing (`d618140`, "fix #1+#5") and/or the overlay font-size
  override (`overlayFontSizePtOverride_`, added `aad6328`) being applied to the committed
  visible-vector span and rendering it at a collapsed/zero size. `ovsz=0.00` (no positive
  override) plus a blanked heading is consistent with the substitute glyphs rendering at the
  wrong size on commit.

**Suspect commit:** `aad6328` (this pipeline's Phase-1 — the same commit that wired Goal A's
per-line font size), with `d618140` (per-span Tr=3) as a contributor. The render-verification
suite (`test_render_*.py`, committed in pipeline e7d3c2a1) was **never re-run during c4a9e271
Phases 1–7**, so the regression sat uncaught — the precise gap this discovery phase exists to
close, and a direct confirmation of the project's "missing UI-visual testing lets real defects
ship" thesis.

**Concrete Phase-9 fix direction (not implemented here).**
1. Confirm the path with the discovery evidence: the diag shows the span *was* selected
   (`ovsz=0.00`), so investigate the commit render first (mechanism b). Add a qDebug/test probe
   to see whether `onAnnotationToolSelected(treToolAction_)` fired on the heading double-click
   (would confirm mechanism a) vs the commit re-render blanking a correctly-selected span.
2. If (b): ensure the committed visible-vector span is rendered through the visible path (not the
   `Tr=3` invisible/region path) and at its true font size; ensure `overlayFontSizePtOverride_`
   cannot drive a real-font vector span to a collapsed size.
3. If (a): tighten `PdfCanvas.cpp:667-676` — require *no live text near the point* (nearest-span
   tolerance in `TextEditModel::spanAtPoint`, or make `Page::hasImageContentAt` return false when
   a text block also covers the point) so a heading click never routes to the region tool.
4. **gtest to lock it:** a deterministic case that loads `invoice_vector_heading.pdf`, resolves
   `spanIndexAtPoint(INVOICE_HEADING_CENTER) >= 0`, drives a commit, and asserts the heading
   region is **not blank** after commit/redo (the oracle the current predicate-only
   `test_text_edit_span_provenance.cpp` is missing).

---

### Cluster A — rotate / rectangle / redaction / coord goldens  ▸ STALE GOLDENS (P2, false-red)

**Classification: all 5 are false-reds — no product defect.** For every one, the controlled
re-capture shows the **operation executed correctly** (page rotated 90° CW with the expected
vertical layout; yellow rectangle drawn over the target span; black redaction bar painted above
the title; rotated text rendered upright + centered on-canvas). Side-by-side `*_live.png` vs
`*_golden.png` confirm the same operation result; the `*_diff.png` divergence is dominated by
**environment drift**, not content:

- live captures are **1213×993** with a darker **#666** canvas-viewport margin and a
  right-scrolled toolbar;
- goldens (committed 2026-06-22 at `4b7db73`/`d22c75d`) are **1136×983** with a lighter **#999**
  margin at 81 %/63 % zoom and an unscrolled toolbar.

This drives SSIM down to ~0.35–0.52 and pixel-diff to ~37–72 % — entirely from margin shade,
zoom/fit level, and window geometry. The `assert_matches_golden` gate (SSIM≥0.85 **OR**
pixel-diff≤6 %) correctly fails on raw divergence but the goldens themselves are stale for the
current rig.

**Concrete fix (harness/baseline, no product code):** regenerate these 5 goldens on the current
rig under a **pinned window geometry + zoom + canvas-margin** so future runs are stable. This is
**Phase-10 baseline-lock work** (`update_baselines.py` against Release `pdfe.exe`). Until then
these 5 rows are recorded **blocked (stale-golden)**, not red. *Recommended hardening:* have the
render harness pin window size/zoom (or crop to the page rect before diffing) so goldens are not
geometry-sensitive — otherwise they will re-drift on the next host change.

---

### Cluster C — `commentPanel.commentContextMenu` not reachable  ▸ HARNESS-GAP (P2)

**Classification: harness-gap, no product defect.** `commentContextMenu` is a stack-local
`QMenu` created on-demand inside `CommentPanel::onContextMenu()` (`src/ui/CommentPanel.cpp:236-248`)
and shown via `QMenu::exec()`. Qt6 does not expose a synchronous `exec()` popup in the UIA tree
at rest, so the P0 "open panel → find persistent child" pattern legitimately cannot see it. The
product code is already automation-friendly: the menu has `setObjectName("commentContextMenu")`
(`CommentPanel.cpp:186`) and its action children (`commentReplyAction`/`commentDeleteAction`)
are persistent members reachable via the `testContextActionsQueryPipe` seam. This is identical to
the **already-blocklisted** `bookmarkPanel.bookmarkContextMenu` row.

**Concrete fix (test-only, no C++):** add one BLOCKED-JUSTIFIED entry to `WIDGET_BLOCKLIST` in
`tests/ui_automation/test_dialog_reachability_p0.py` (after the `bookmarkPanel` entry, ~line 156),
mirroring the bookmark wording, so the row records `blocked` not `red`. No `setObjectName` needed.

---

### Flakes (environment / UIA contention — no product issue)

| Test | Evidence it's a flake |
|---|---|
| `test_render_text_edit::test_vector_text_edit_undo_reverts` | passed every isolated rerun; in-edit undo restores heading (`clusterB_textedit_undo.png`) |
| `test_render_oracle_proof::test_ctrl_z_reverts_canvas` | passed isolated rerun; same in-edit-undo path |
| `test_features_phase7.py` (whole file) | failed attempt 1, rc=0 on rerun *inside the discovery run* |

These are consistent with the documented UIA-contention flakiness of a ~6 h pywinauto suite on a
headless Server-2019 rig (the `run_ui_suite.py --reruns` mechanism exists for exactly this). No
action beyond noting; Phase-10 CI should keep the rerun-on-failure mitigation.

---

## 4. Prioritized fix backlog (input to Phase 9 / Phase 10)

| ID | Item | Type | Priority | Target phase |
|---|---|---|---|---|
| **D8-1** | Vector heading collapses on commit/redo (`invoice_vector_heading.pdf`) | product-defect | **P1** | **Phase 9** |
| D8-2 | Regenerate 5 stale render goldens under pinned geometry/zoom; make harness geometry-stable | harness/baseline | P2 | Phase 10 |
| D8-3 | Add `commentPanel.commentContextMenu` BLOCKED-JUSTIFIED blocklist entry | harness | P2 | Phase 9 (trivial) or Phase 10 |
| D8-4 | Keep `--reruns` flake mitigation in CI; consider crop-to-page diffing | harness | P3 | Phase 10 |

**P0 count: 0.** **P1 count: 1 (D8-1).** No data-loss / crash / security defects surfaced — the
one genuine defect is a render regression in vector heading editing.

---

## 5. Evidence index (criterion 6 — ≥5 red items with real screenshots/pipe output)

All under `tests/_runtime/discovery_phase8/evidence/` (29 PNGs) + the per-file JUnit/logs.

**Cluster B (heading collapse) — screenshots:**
`clusterB_textedit_before.png` (full "Invoice"), `clusterB_textedit_inedit.png`,
`clusterB_textedit_after.png` (blanked), `clusterB_textedit_undo.png` (restored),
`clusterB_textedit_redo.png` (re-blanked); full-canvas `clusterB_canvas_{before,inedit,after_commit,after_undo,after_redo}.png`.
Pipe/diag: failure messages embed the selection diag + ink ratios (e.g.
`heading collapsed after edit: ink 0.168 -> 0.032`, `span "Invoice" selected, ovsz=0.00`).

**Cluster A (stale goldens) — 5 sets of live/golden/diff:**
`clusterA_page_ops_rotate_cw_*`, `clusterA_view_nav_rotate_cw_*`, `clusterA_annot_rectangle_*`,
`clusterA_redaction_mark_*`, `clusterA_coord_rotation_*` (each `_live` + `_golden` + `_diff`,
most also `_before`).

**Cluster C — pipe output:** JUnit message
`commentPanel.commentContextMenu: widget with suffix 'commentContextMenu' not found in commentPanel`.

That is **6 red items backed by real screenshots** (5 Cluster-A sets + the Cluster-B heading
sequence) plus pipe/diag output on the rest — exceeding the ≥5 evidence requirement.

**Helper scripts** (reproduce the captures): `discovery_phase8/capture_clusterB.py`,
`discovery_phase8/capture_clusterA.py`.

---

## 6. Conclusion

The discovery run did its job: against a binary that passed every prior phase gate, it surfaced
**one genuine product regression** (D8-1, vector heading collapse on commit) that had been live
since this pipeline's Phase-1 because the render-verification suite was never re-run. Of the 11
raw failures, 1 defect is real and routed to Phase 9; 5 are stale-golden false-reds and 1 is a
known on-demand-menu harness gap (both routed to baseline/harness work); 2 are UIA flakes. 2931
product interactions verified green. Phase 9 fixes D8-1 with a locking gtest; Phase 10 regenerates
the drifted goldens, lands the commentPanel blocklist entry, and wires the suite into CI.
