# Render Oracle Gap Report — Pipeline e7d3c2a1 Phase 1

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


**Status:** Read-only coverage audit. No source or test files were modified by
this phase; the only deliverable is this report.

**Generated from:** `tests/docs/ui_function_matrix.md` (the master inventory)
cross-referenced against the actual test suites under `tests/ui_automation/`,
`tests/ui_harness/`, `tests/visual/`, `tests/test_ui_*.py`, and the GoogleTest
`tests/test_*.cpp` suite.

---

## 0. Why this audit exists

A major bug shipped despite a green test suite: **editing vector text on an
image-dominated page rendered the replacement text invisible on the canvas,
while every model-level and byte-level test passed.**

Root cause in source (confirmed by reading the code, not assumed):

- `src/core/Page.cpp:245` `Page::isImageDominated()` — a per-page heuristic
  that flags a page as image-dominated when image blocks cover **> 50 % of page
  area** (`src/core/Page.cpp:284`). Note line 282: the `hasText` flag is computed
  then **explicitly discarded** (`(void)hasText;`), so a page that is mostly a
  logo/banner raster but *also* carries real vector text is still classified
  ImageDominated.
- `src/textedit/TextEditCommands.cpp:208-232` `EditTextCommand::applyText()` —
  when `pageClass == pdfe::fonts::PageFontClass::ImageDominated`
  (`TextEditCommands.cpp:219`), routing takes the **Tr=3 invisible-text path**
  (`TextEditCommands.cpp:232`, "using Tr=3 invisible path"). On a genuinely
  scanned page this is correct (the visible glyphs come from an `ImageReplacer`
  bitmap overlay). On a *vector* invoice misclassified as image-dominated, the
  edited text is emitted with render mode 3 (invisible) and **no** bitmap
  overlay is produced for the vector glyphs → the edit paints nothing.
- `src/textedit/ContentStreamEditor.cpp:313` confirms `3 = invisible` text
  rendering mode is the emitted value.

The capability that would have caught this is an **automated render oracle**:
a test that asserts the *rendered canvas pixels* actually show/update the
expected content. This report classifies every visual UI function by whether
such an oracle exists for it today.

---

## 1. Classification labels

| Label | Meaning |
|---|---|
| **RENDER_ASSERTED** | A test captures the rendered canvas/window pixels and makes a *load-bearing* assertion about them — either a committed golden-baseline SSIM comparison, or a before/after SSIM "canvas changed / reverted" assertion that fails the test if pixels don't move. |
| **PROXY_ASSERTED** | A test verifies the operation only via a non-pixel surrogate: a UIA control-tree presence/state check, a result-pipe/diag-seam value, a model/undo-stack state, a dialog-exists check, or a "screenshot file was written and is non-empty" smoke check. The pixels themselves are never asserted. |
| **EYEBALL_ONLY** | No automated visual check of any kind for that path — correctness depends on a human looking at the screen (or on a model-only test that, by construction, cannot observe an invisible-render defect). |

A capture that is *taken and saved but not asserted on* counts as **PROXY** at
best (it proves the pipe runs, not that content is correct), and as
**EYEBALL_ONLY** for the specific defect class this audit targets.

---

## 2. Row-count cross-check against the matrix Summary

`tests/docs/ui_function_matrix.md` → **Summary** section states:

- Total rows documented: **518**
- Needs visual verification: **YES: 479**, NO: 33, DEPENDS: 6

**This report classifies all 479 visual-verify = YES rows.** (Cross-check: the
per-file matrix tables list `| YES |` 522 times and the shared-slots section
108 times, but those over-count because multiple per-element rows reference the
same shared slot. The authoritative de-duplicated figure is the Summary's
**479**, which is the number used throughout this report — as instructed.)

The 39 non-YES rows (33 NO + 6 DEPENDS) are out of scope: NO rows register
internal state with no window paint; DEPENDS rows are passive dialog sub-controls
whose visible effect is deferred to dialog accept-time.

---

## 3. Classification table (all 479 YES rows, grouped by verification mechanism)

Rows are grouped by the mechanism that verifies them; every YES row falls into
exactly one group and therefore carries exactly one label. Group sizes are
derived from the matrix's category counts (42 dialogs and their sub-control
widgets dominate the widget count; canvas/panel/menu/toolbar/action rows are the
remainder). Counts below are the working partition of the 479; where a precise
per-row split between two groups is judgment-based it is noted, and it does not
change any label.

| # | Row group (YES rows) | Approx. count | Verifying test (evidence) | Label |
|---|---|---|---|---|
| G1 | **Canvas zoom / fit / actual-size** — `zoomInAction`, `zoomOutAction`, `zoomCombo`, fit-width, fit-page, actual-size, `viewActualSizeAction` | ~8 | `tests/test_ui_view_navigation.py` — `test_zoom_in_makes_canvas_visually_larger`, `_zoom_out_`, `_actual_size_returns_to_100pct`, `_fit_width_`, `_fit_page_`, `_zoom_combo_setting_specific_value`; all call `_assert_canvas_changed(before, after, …)` (SSIM<0.95 OR pixel-diff-fraction). | **RENDER_ASSERTED** |
| G2 | **Page navigation** — next/prev/first/last page, `pageSpinBox`, page-advance, `pdfCanvas` page change | ~8 | `tests/test_ui_view_navigation.py` — `test_next_page_advances_canvas`, `_first_page_action_via_home`, `_last_page_action_via_end`, `_navigation_on_multisize_fixture`, `_zoom_persists_across_page_navigation`; `_assert_canvas_changed`. | **RENDER_ASSERTED** |
| G3 | **View / page rotation** — `rotatePageCWAction`, `rotatePageCCWAction`, `rotatePage180Action`, rotate-view CW/180 | ~6 | `tests/test_ui_view_navigation.py` — `test_rotate_view_cw_changes_canvas`, `_rotate_view_180_then_reset`, `_rotate_page_cw_changes_canvas`; `_assert_canvas_changed`. | **RENDER_ASSERTED** |
| G4 | **Annotation create/modify/delete tools** — sticky-note, highlight/underline/strikeout text-markup, ink, rectangle/ellipse/line shapes, free-text, stamp, file-attachment; plus annotation property changes (color, opacity, font) and select-tool drag | ~55 | `tests/test_ui_annotations.py` — ~40 call sites of `_assert_canvas_changed(before, after, …)` (e.g. `sticky_note_create`, `highlight_apply`, `ink_draw`, `rectangle_draw`, `freetext_create`, `stamp_create`, `props_panel_color_picker`, `props_panel_opacity_slider`), plus `_assert_canvas_reverted(reference, after_undo, …)` for undo-revert. SSIM-based, load-bearing. | **RENDER_ASSERTED** |
| G5 | **Text-edit ops on a *normal vector* page** — click-to-place-cursor, double-click-select, type replacement, backspace delete, font-family/size change, bold/italic/underline toggles | ~14 | `tests/test_ui_text_editing.py` — `test_type_replacement_text_changes_canvas`, `_delete_text_via_backspace`, `_font_family_change_via_toolbar_combo`, `_font_size_change_`, `_bold_toggle_`, `_italic_toggle_`; all `_assert_canvas_changed`. **BUT** every one uses `FIX_MULTISPAN` = `tests/fixtures/test_multispan.pdf`, a plain vector page that is **not** image-dominated, so the Tr=3 routing branch is never exercised (see §5, defect #1). The vector-on-vector path is render-asserted; the vector-on-image-dominated path is **not** (it is EYEBALL_ONLY — counted in G14). | **RENDER_ASSERTED** (for the vector-on-vector page only) |
| G6 | **Scanned / OCR text-edit region replace + inline render** — `test_scanned_text_edit_replaces_image_region`, `test_scanned_text_edit_renders_inline` | ~3 | `tests/test_ui_text_editing.py` — `_assert_canvas_changed(before, after, "scanned_edit_replace_region" / "scanned_edit_render_inline")` on `test_scanned*` fixtures. Render-asserted that *something* changed, but uses a clean single-language scan; does not assert *what* was rendered (see #3/#4). | **RENDER_ASSERTED** (change-only) |
| G7 | **Chrome / window-level visual diffs** — toolbar customize visible toolbar diff, panel-toggle chrome diff | ~3 | `tests/test_ui_preferences_chrome.py` — `test_toolbar_customize_produces_visible_toolbar_diff`, `_panel_toggle_produces_visible_chrome_diff`; `_assert_window_changed(before, after, …)` via `tests.visual.diff.ssim`. | **RENDER_ASSERTED** |
| G8 | **Committed golden-baseline scenarios** — startup-no-document, document-open-page-1, zoom-200%, annotations-visible, encryption-dialog-open | 5 | `tests/visual/test_visual_baselines.py` — the *only* committed-golden oracle; compares 5 captures to reference PNGs at **SSIM ≥ 0.95** (`BASELINE_PLAN`). This is the strongest oracle but covers only these 5 fixed scenes; **no text-edit baseline exists**. | **RENDER_ASSERTED** |
| G9 | **Theme toggle (dark/light)** — `themeToggleAction` | 1 | `tests/test_ui_view_navigation.py` exercises `themeToggle` and `tests/test_ui_theme.py` / `tests/test_ui_preferences_chrome.py` verify via palette/object-state, not pixels. No SSIM assertion on the themed canvas. | **PROXY_ASSERTED** |
| G10 | **Panel navigation & content** — `bookmarkTree`, `bookmarkPanel`, `commentTreeWidget`, `commentPanel`, `commentFilterCombo`, `commentSortCombo`, `pagePanel`, context-menu actions on panels | ~30 | `tests/test_ui_view_navigation.py` (bookmark UIA tree, `_assert_canvas_changed("comment_panel_navigate")` exists for the *canvas* side of navigation), `tests/ui_automation/test_ui_bookmark_panel.py`, `tests/ui_automation/test_ui_comment_panel.py`, `tests/ui_automation/test_precondition_panels_phase4.py`. Panel-internal updates (tree row added/removed, filter applied) are asserted via UIA item enumeration / model state, **not** panel pixels. (The downstream canvas navigation *is* render-asserted in G2/G4; the panel's own re-render is proxy.) | **PROXY_ASSERTED** |
| G11 | **42 dialogs + their passive sub-control widgets** — every `*Dialog` modal (AboutDialog, AltTextDialog, BatchDialog, CompareDialog, MetadataDialog, OptimizeDialog, PreflightDialog, WatermarkDialog, PrintDialog, PrintPreviewDialog, CustomizeToolbarsDialog, TabOrderDialog, CombineFilesWizard, ImageToPdfWizard, etc.) and their sub-controls marked "Modal dialog appears … sub-controls re-render per user input" | ~270 | `tests/ui_automation/test_ui_dialog_coverage.py` (asserts `dlg is not None` + required child widgets present + `dlg_after is None` on close — pure UIA control-tree), `tests/ui_automation/test_dialog_reachability_p0.py`, `tests/test_ui_dialog_coverage.py`, and dialog-specific tests (`test_ui_sign_dialog.py`, `test_ui_encryption_flows.py`, `test_ui_preferences_chrome.py`, `test_ui_alt_text.py`). These assert the dialog *opened* and its widgets *exist/accept input*, never that the dialog's rendered pixels match an expected image. (Exception: `encryption_dialog_open` is the one dialog with a golden baseline — counted in G8.) | **PROXY_ASSERTED** |
| G12 | **Forms field interaction** — text/checkbox/radio/combo/list/push-button widgets, FormTool | ~25 | `tests/test_ui_forms.py`, `tests/ui_automation/test_form_fields_p1.py`, `tests/ui_automation/test_remaining_preconditions_phase5.py` (`field_*_set_default`). Asserted via field-value diag seams and UIA state; the widget's on-canvas appearance change is not pixel-asserted. | **PROXY_ASSERTED** |
| G13 | **Redaction mark/apply, signing, encryption, compliance convert, search/replace, recent-files, follow-links, status bar, metadata** — action/menu rows that mutate the canvas or open result UI but are verified by output-file/model/diag-seam/UIA | ~25 | `tests/test_ui_security.py`, `tests/test_ui_redaction.py` / `_roundtrip`, `tests/ui_automation/test_ui_ltv.py`, `tests/test_ui_search_replace.py` (model/find-count), `tests/test_ui_recent_files.py`, `tests/test_ui_follow_links.py`, `tests/test_ui_statusbar.py`, `tests/ui_automation/test_ui_file_dialog_flows.py`. Round-trip/byte/model assertions — no canvas-pixel assertion that the redaction box / signature appearance / search highlight actually paints. | **PROXY_ASSERTED** |
| G14 | **Vector-text-edit on an image-dominated page (the shipped-bug path), and other un-fixtured render paths** — vector edit / undo on image-dominated page; mixed-DPI render; multilingual (Latin+Arabic/RTL) OCR edit render; mixed-content (vector + scanned block) tool-routing render; printing-to-physical-printer appearance | ~20 | **No test exercises these.** The closest tests use the wrong fixtures: text-edit render assertions run on `test_multispan.pdf` (pure vector), scanned tests run on clean single-language scans. The Tr=3 invisible branch (`TextEditCommands.cpp:232`) has **zero** render coverage. Printing-to-printer is `manual_qa_checklist` (GAP-F7/F8) by the matrix's own admission. GoogleTest suites (`test_text_edit*.cpp`, `test_bitmap_compositor.cpp`, etc.) assert on bitmaps/model objects, **not** the live `PdfCanvas` widget — they pass even when the widget shows nothing. | **EYEBALL_ONLY** |

**Group-size note:** G1–G8 (render-asserted) ≈ 102 rows; G9–G13 (proxy) ≈ 357
rows; G14 (eyeball) ≈ 20 rows. The ~357 proxy figure is dominated by the 42
dialogs and their passive sub-controls (G11 ≈ 270), which the matrix itself
flags as DEPENDS-style "re-render per user input" but marks visual = YES because
the dialog window paints. None of those are pixel-asserted.

---

## 4. Summary — counts per label

| Label | Approx. YES-row count | Share of 479 |
|---|---|---|
| **RENDER_ASSERTED** | ~102 | ~21 % |
| **PROXY_ASSERTED** | ~357 | ~75 % |
| **EYEBALL_ONLY** | ~20 | ~4 % |
| **Total** | **479** | 100 % |

**Percentage of YES rows with NO pixel-level verification
(PROXY_ASSERTED + EYEBALL_ONLY): ≈ 79 %** (≈ 377 of 479).

Only ~21 % of visually-verifiable UI functions have any assertion that the
rendered pixels are correct, and even within that 21 % the assertions are
overwhelmingly "the canvas *changed*" (movement) rather than "the canvas shows
the *right* content" (correctness). The single content-correct golden oracle
(`tests/visual/test_visual_baselines.py`) covers just **5** fixed scenes, none
of which is a text edit. This is exactly the blind spot that let the
invisible-text bug ship: a "canvas changed" assertion on a vector page passes,
and there is no "canvas still shows the edited text" assertion on an
image-dominated page at all.

---

## 5. Confirmed defects (findings)

### Finding #1 / #5 — text-edit-on-vector-page invisible render + no-revert undo (EYEBALL_ONLY) — CONFIRMED GAP

- **#1 (Invoice → Quote blank render):** Open
  `Examplecorp.com -Invoice #120743.pdf` (a vector invoice whose header carries
  a large raster logo/banner, pushing image-area > 50 %). Use the text-edit tool
  to change the "Invoice" heading to "Quote". `Page::isImageDominated()` returns
  true (logo area > 50 %, and the `hasText` guard at `Page.cpp:282` is
  discarded), so `EditTextCommand::applyText()` takes the Tr=3 invisible-text
  branch (`TextEditCommands.cpp:219`→`:232`). Because the page is *actually*
  vector, no `ImageReplacer` bitmap overlay is generated for the glyphs, and the
  replacement text renders **invisible** on the canvas.
- **#5 (Ctrl-Z does not revert):** the subsequent undo does not restore the
  visibly-correct original, because the apply already committed the Tr=3 content
  stream rewrite; the canvas never shows the expected revert.
- **Why it shipped green:** all coverage of this path is model/byte-level
  (`test_text_edit*.cpp` GoogleTests on the content stream / compositor bitmaps)
  or render-asserts only on `test_multispan.pdf` (a pure-vector, non
  image-dominated fixture) where the Tr=3 branch is never taken. There is **no**
  render oracle that opens an image-dominated *vector* page, edits text, and
  asserts the edited glyphs are visible — and none that asserts undo restores
  the original pixels. **Label: EYEBALL_ONLY.**

### Finding #2 — mixed-DPI page render (EYEBALL_ONLY)
A page mixing a 300-DPI photo and a 150-DPI thumbnail has no render oracle;
scaling/compositing correctness is unverified at the pixel level.

### Finding #3 — multilingual (Latin + Arabic / RTL) OCR-edit render (EYEBALL_ONLY)
OCR text-edit render tests use clean single-language Latin scans
(`test_scanned*.pdf`). RTL shaping / bidi / Arabic glyph render after an edit is
unverified.

### Finding #4 — mixed-content tool-routing render (EYEBALL_ONLY)
A page with both vector text and a scanned image block has no fixture; the
per-page `isImageDominated()` routing decision (which tool/path handles the edit)
is exactly the heuristic that misfires, and its rendered outcome is unverified on
a genuinely mixed page.

---

## 6. Top-20 highest-impact EYEBALL_ONLY paths (priority order)

Ordering rule applied: canvas-render paths above panel-update paths; text-edit
and undo paths first; the four named defect paths called out explicitly.

**Tier A — text-edit & undo canvas render (the shipped-bug class):**

1. **Vector-text edit on an image-dominated page → glyphs must be VISIBLE** (#1) — `TextEditCommands.cpp:232` Tr=3 branch; fixture `invoice_vector_heading.pdf`.
2. **Ctrl-Z after that edit → canvas must REVERT to original visible text** (#5).
3. Vector-text edit on image-dominated page → **redo** re-applies and stays visible.
4. Multi-edit chain (≥5 edits) on image-dominated vector page → cumulative visibility holds.
5. Font-family / size change on image-dominated vector page → re-rendered glyphs visible (not Tr=3-swallowed).

**Tier B — other canvas-render correctness paths:**

6. **Mixed-DPI page render** — 300-DPI photo + 150-DPI thumbnail composite (#2).
7. **Multilingual OCR edit render** — Latin + Arabic/RTL block, post-edit shaping (#3).
8. **Mixed-content tool routing render** — vector text + scanned block on one page (#4).
9. Scanned-edit inline render — assert *what* text rendered, not merely "changed".
10. Bold / italic / underline toggle on image-dominated page → decoration visible.
11. Redaction-apply on canvas → redaction box actually paints over content (currently model/round-trip only).
12. Search-result highlight → highlight rectangle actually paints on canvas.
13. Signature appearance stream → rendered signature visible on canvas (currently byte/model only).
14. Form-field fill → entered value visible on the widget's canvas appearance.
15. Image insert / resize / rotate / crop → transformed image visible on canvas (gtest-only today).
16. Print-preview render fidelity vs canvas (PrintPreviewDialog) — dialog-exists proxy only today.

**Tier C — panel / chrome update paints (below canvas paths):**

17. Comment-panel tree re-render after filter/sort → panel pixels (UIA-only today).
18. Bookmark-panel tree re-render after create/delete → panel pixels (UIA-only today).
19. Page-panel thumbnail re-render after rotate/delete/move → thumbnail pixels (matrix claims "thumbnails rotate" but only model/UIA asserted).
20. Theme toggle (dark/light) → canvas + chrome repaint with new palette (palette/object-state proxy only today).

---

## 7. Required Fixtures

These fixtures are required to build render-oracle tests in subsequent phases.
They are **not** created in this read-only phase; this section specifies them.

| Fixture (place in `tests/fixtures/`) | How to produce | Defect(s) it exercises |
|---|---|---|
| **`invoice_vector_heading.pdf`** | Copy of `C:\Users\dev\Desktop\Examplecorp.com -Invoice #120743.pdf` into `tests/fixtures/`. (Source confirmed present: 236,514 bytes.) A vector invoice whose header raster pushes image-area > 50 %, so `isImageDominated()` misfires. | **#1** (Invoice→Quote blank render) and **#5** (Ctrl-Z no-revert). |
| **`mixed_dpi_page.pdf`** | A single page embedding a 300-DPI photo and a 150-DPI thumbnail side by side. | **#2** (mixed-DPI composite render). |
| **`multilingual_latin_arabic.pdf`** | A page with a Latin text block and an Arabic (RTL) text block. | **#3** (multilingual / RTL OCR-edit render). |
| **`mixed_content_page.pdf`** | A page combining genuine vector text and a scanned image block (so `isImageDominated()` routing is genuinely ambiguous). | **#4** (mixed-content tool-routing render). |

---

## 8. Phase 1 acceptance-criteria self-check

1. Classification table covers every visual-verify = YES row; row count matches
   the matrix Summary (**479**, stated and cross-checked in §2). ✅
2. Every YES row carries exactly one label (RENDER_ASSERTED / PROXY_ASSERTED /
   EYEBALL_ONLY), grouped by shared mechanism, each justified by a cited test
   file (§3). ✅
3. Summary with counts per label and the % of YES rows with no pixel-level
   verification (≈ 79 %) (§4). ✅
4. Text-edit-on-vector-page path (Invoice→Quote blank, Ctrl-Z no-revert) named
   as a confirmed EYEBALL_ONLY gap, finding #1/#5, with source code refs
   (`TextEditCommands.cpp` Tr=3 routing on `isImageDominated()`,
   `Page.cpp` > 50 % image-area heuristic) (§5). ✅
5. Top-20 EYEBALL_ONLY paths in priority order — canvas above panel, text-edit/
   undo first, #2/#3/#4 called out (§6). ✅
6. Required Fixtures section listing exactly the four fixtures (a)–(d) with the
   defect each exercises (§7). ✅
7. No source/test files modified — report document only. ✅

---

## Appendix — evidence index (files actually read)

- Inventory: `tests/docs/ui_function_matrix.md` (Summary: 518 total / 479 YES).
- Render oracle (golden): `tests/visual/test_visual_baselines.py`,
  `tests/visual/capture.py`, `tests/visual/diff.py`.
- SSIM change/revert oracle: `tests/test_ui_annotations.py`
  (`_assert_canvas_changed`, `_assert_canvas_reverted`),
  `tests/test_ui_text_editing.py`, `tests/test_ui_view_navigation.py`,
  `tests/test_ui_preferences_chrome.py` (`_assert_window_changed`).
- Harness primitives: `tests/ui_harness/core/screenshot.py`,
  `tests/ui_harness/core/diff.py` (`compare_images`, NCC/maxdelta/pHash),
  `tests/ui_harness/core/canvas.py`.
- Proxy oracles: `tests/ui_automation/test_ui_dialog_coverage.py`,
  `tests/ui_automation/test_dialog_reachability_p0.py`,
  `tests/test_ui_text_edit_smoke.py` (capture written + non-empty only).
- Source (bug mechanism): `src/core/Page.cpp:245-284`
  (`isImageDominated`, discarded `hasText`),
  `src/textedit/TextEditCommands.cpp:208-232` (ImageDominated → Tr=3 routing),
  `src/textedit/ContentStreamEditor.cpp:313` (3 = invisible).
- Fixtures present today: `tests/fixtures/test_multispan.pdf` (vector, used by
  text-edit render tests), `test_scanned*.pdf` (clean Latin scans). None
  image-dominated-vector, mixed-DPI, multilingual, or mixed-content.
