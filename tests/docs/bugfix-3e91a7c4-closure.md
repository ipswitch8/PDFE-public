# Bug-fix pipeline `3e91a7c4` — Closure document

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


**Scope:** six user-reported PDFE defects, fixed strictly test-first, with a
mandated test-gap post-mortem and a testing-process improvement plan.
**Status at closure:** Phases 1–5 complete, all gates passed. Full Debug
GoogleTest suite **4049 passed / 0 failed / 6 pre-existing skips**. End-to-end
walkthrough of the real "SpecimenA Green" 51-page document passes (open → scroll
all pages → Replace All → armed re-scroll → save → re-extract) with no crash and
the replacement landing in body content. All work is LOCAL/unpushed; a push
(triggering ~9 h CI) awaits an explicit user go/no-go.

Reproduction target throughout: `SpecimenA Green A Tale of Two Collards …` — 51
pages, page 0 title (24–50 pt), pages 1+ body (9–14 pt), `/ActualText` on pages
47–48. The user's "second page's smaller text doesn't update" tracks **font
size**, not page index (they coincide in this document).

---

## 1. The six reports — root cause, fix, regression test

| # | Report | Root cause | Fix commit | Regression test(s) |
|---|--------|-----------|-----------|--------------------|
| 6 | Crash while scrolling (`Access Violation: read address 0x0`) after an edit | MuPDF `/ActualText` extraction underflow: in `do_extract_within_actualtext` (`stext-device.c:1142`), `z = fz_utflen(actualtext)`; when `/ActualText` is a **prefix** of the displayed span, the prefix loop drives `z→0` while items remain → `assert(z!=0)` (Debug abort) and `rune_index(actualtext, z-1)` with `z-1==SIZE_MAX` OOB (Release → `read 0x0`). Reached by Replace All's all-page extraction. | `5873dd9` (P2) — shared `pdfe::core::pdfeStextOptions()` sets `FZ_STEXT_IGNORE_ACTUALTEXT` at all 8 `fz_new_stext_page_from_page` sites | `test_actualtext_extraction_crash.cpp` (synthetic span `ABC` + `/ActualText(A)`; RED = the exact `z!=0` abort → GREEN returns `ABC`) |
| 4 | Page-2 small-text edits never appear on the rendered page | `ContentStreamEditor::textFilterCallback` built the glyph box via `pageHeight - y`, **discarding the page origin**; span boxes are CropBox-normalized. On a nonzero-origin page (SpecimenA MediaBox `[0 7.9 594 745.9]`) they diverge by `Δ=origin.y0`; short small-font spans can't absorb Δ → zero glyphs match → `/Contents` unchanged while the overlay shows the edit. | `2133e73` (P3) — map the glyph box through the page's real `pdf_page_transform` (`state.pageCtm`) | `test_smalltext_edit_render.cpp::EditLandsAtAllFontSizesOnOffsetOriginPage` (18/11/9/7 pt) + `::EditOnPage2LandsInRenderedContent` |
| 5 | "Replace All" reports a count but only page-1 text changes | Same geometry no-op as #4, **plus** a count/write decoupling: `totalReplaced++` counts the in-memory model replace (`MainWindow.cpp:5830`), independent of whether the content-stream write landed. | `2133e73` (P3) — geometry fix makes every matched span write; `lastReplaceAllCountForTesting()` seam couples the reported count to the actual landed writes | `test_smalltext_edit_render.cpp::ReplaceAllUpdatesEveryMatchedPageAndCountMatchesActual` |
| 1 | Menu/toolbar buttons "just look like a series of words" | Separators mostly existed; the "words" look was driven by #2 (no icons) + text-only buttons (no `setToolButtonStyle`). The Form-fields toolbar was the one genuine no-separator gap. | `c465c85` (P4) — `ToolButtonTextBesideIcon` on main/annotation/form toolbars + 2 Form-toolbar separators | `test_ui_structure_bugs.cpp::FormToolbarGroupsFieldsWithSeparators` |
| 2 | Missing button icons | 124 `QAction`, **0** `setIcon`; **no icon assets existed at all**. And even after wiring, the `.qrc` in the `pdfe_ui` STATIC lib had its RCC init symbol linker-dropped → every `QIcon(":/icons/actions/*.svg")` resolved **null** (the new test caught this live). | `d824277`+`c465c85` (P4) — 25-glyph SVG set → `action_icons.qrc`; `setIcon` on every toolbar action; pinned the resource via a global-scope `::qInitResources_action_icons()` call in `createToolBar()` | `test_ui_structure_bugs.cpp::MainAndAnnotationToolbarActionsHaveIcons` |
| 3 | Pages panel missing / not the default left tab | `restoreState()` reapplied a stale saved windowState (Layers raised / Pages hidden) and a persisted `Panels/pagePanel=false`; there was no forced Pages default and no `raise()`. | `c465c85` (P4) — `ensurePagesDockDefault()` (show+raise Pages, hide Layers/Bookmarks) called **after** `restoreState()` | `test_ui_structure_bugs.cpp::PagesForcedAsDefaultLeftDockTabAfterStaleRestore` + pywinauto `test_ui_pages_default_dock.py` |

**Cross-cutting E2E:** `test_ui_specimen_a_e2e_walkthrough.py` exercises all six
together on the real document (skips when the user PDF is absent; the gtest layer
is the runner-portable coverage).

---

## 2. Test-gap post-mortem — WHY the existing suite missed each (the CLASS of gap)

Not "no test existed" — the point is the *structural* blind spot that let the
whole class ship. Three bug-classes, three cross-cutting meta-causes.

### Class A — committed-output correctness beyond the trivially-visible case (#4, #5)
- **Guarding test now:** `test_smalltext_edit_render.cpp` (edits at 7–18 pt on a
  nonzero-origin page and on page 2, read back via `extractPlainText()`; the
  Replace-All test asserts reported count == landed writes).
- **Why it was missed — the class of gap:**
  1. **Delta oracles over content oracles.** Every render oracle asked "did
     pixels change on the visible page," never "is page N's content correct." A
     silent no-op leaves the *original* content in place, so a delta check is
     structurally blind to it.
  2. **Page-0-only.** All edit/replace gtests operate on `document->page(0)`;
     the render harness defaults `page_index=0` with zero non-zero call-sites.
  3. **Trust-the-model over verify-the-output.** The Replace-All count was read
     from the model string replace and never reconciled against the rendered/
     extracted content — hence "627 replaced" with nothing changed.

### Class B — long-session / scroll stability & object lifetime (#6)
- **Guarding test now:** `test_actualtext_extraction_crash.cpp` (the reproduced
  primary mechanism) + the E2E scroll/Replace-All/armed-scroll walkthrough.
- **Why it was missed:** no test extracts text from a document carrying
  marked-content `/ActualText` (all fixtures are hand-built trivial PDFs); no
  test scrolls a real multi-page document through every page; the suite has **no
  ASan/TSan** lane, and the one concurrency test *tolerates* the race ("no crash
  + still usable"). The crash lived in a fixture-coverage hole, not a logic hole.

### Class C — UI structural / affordance completeness (#1, #2, #3)
- **Guarding test now:** `test_ui_structure_bugs.cpp` (non-null icons, form
  separators, Pages-forced-default) + `test_ui_pages_default_dock.py`.
- **Why it was missed — the class of gap: reachability over realization.** The
  UIA-tree suite verified a widget *exists / is reachable*, never that it is
  *visually complete* — iconed, separated, in the right dock, the default tab.
  Only the app icon was ever asserted; docks were checked for `allowedAreas()`
  and existence, never the active tab or the `restoreState()` override path.

### General lessons (prevent the CLASS, not the 6 instances)
1. **Content oracle, not delta oracle** — assert *what the output is*, on page
   ≥ 1, parametrized by font size; a "something changed" check cannot see a
   silent no-op.
2. **Realization, not reachability** — for UI, assert the affordance is complete
   (icon present, grouped, correct default), not merely that the control exists.
3. **Reconcile reported-vs-rendered** — any user-facing count/status must be
   proven against the rendered/extracted result, never the in-memory model.
4. **Run the real app on the real document** — static code-reading mis-identified
   the #6 crash (guessed a render race; the real site was `/ActualText`
   extraction). Live reproduction on the user's PDF is a required investigation
   step, not optional.

---

## 3. TESTING-PROCESS IMPROVEMENT PLAN

Each item is tied to the bug-class (A/B/C + meta-cause) and/or the Phase-1
hypothesized-other-missed-class it prevents, and prioritized **implement-now**
vs **backlog**. "Delivered" = this pipeline already shipped the seed; the
*process* change is to make it a standing policy, not a one-off.

### Implement-now
| ID | Change | Prevents (class) | Status |
|----|--------|------------------|--------|
| N-1 | **Render/edit oracle policy: every render/edit oracle asserts on page ≥ 1 and is parametrized by font size**, reading back content (extract/render), not deltas. Add a lint/checklist item rejecting new `page_index=0`-only oracles. | A / meta-cause 1 + 2; hyp-1 | Seed delivered (`test_smalltext_edit_render.cpp`); policy TODO |
| N-2 | **UI realization-assertion layer**: toolbar actions have non-null `QIcon`; toolbars/menus have section separators; the default left-dock tab is Pages. Extend to tooltip/`accessibleName` completeness and "no duplicate accessible names." | C / meta-cause 2 | Seed delivered (`test_ui_structure_bugs.cpp`, `test_ui_pages_default_dock.py`) |
| N-3 | **Output-reconciliation policy**: any user-facing count/status is asserted against the rendered/extracted result. Add a helper + checklist rule. | A / meta-cause 3; hyp-9 | Seed delivered (`ReplaceAllUpdatesEveryMatchedPageAndCountMatchesActual`) |
| N-4 | **Adversarial-fixture policy for extraction/search/edit**: a fixture carrying marked-content `/ActualText` (incl. the prefix case) is in the extract/search/edit matrix. | B / #6; hyp-1 | Delivered (`actualtext_prefix_crash.pdf`) |
| N-5 | **Real-user multi-page corpus + committed E2E walkthrough**: an end-to-end open→scroll-all→edit→Replace-All→save→re-extract test on a multi-page document. | A+B / meta-cause 1; hyp-1,2 | Delivered against the user doc (`test_ui_specimen_a_e2e_walkthrough.py`); see B-0 for the CI-portable corpus |

### Backlog (tracked, not dropped)
| ID | Change | Prevents (class) | Note |
|----|--------|------------------|------|
| B-0 | Build a **committed, CI-portable multi-page real-world corpus** (synthetic or licensed — the SpecimenA book is the user's copyrighted file, cannot be committed) incl. a nonzero-origin page, small body text, `/ActualText`, and blank/image-only pages, so N-5's E2E runs on the runner (currently skips when the user PDF is absent). | A+B; hyp-1,2 | Highest-value backlog item |
| B-1 | **TileRenderer cancel-and-join barrier** (`cancelPage`/`cancelAll` never `waitForDone`) + null-check `fz_load_page` + coalesce per-page `pdf_drop_page_tree`; **scroll-stress soak test** (`test_ui_scroll_stress.py`) + an **ASan/TSan CI lane**. | B / #6 secondary | The unreproduced render-lifetime race from Phase 1 — a real latent hazard even though `/ActualText` was the crash that shipped |
| B-2 | Extend edit/annotation/redaction/form/image-op oracles to **page 2+**. | A / hyp-1 | Same page-0 default as #4 |
| B-3 | **Save-then-reopen fidelity beyond page 0** — re-render a reopened page 2 and compare, not just annot/field counts. | A / hyp-2 | |
| B-4 | **Mouse-wheel / trackpad continuous scroll** coverage (tile seams, prefetch ring, momentum transitions) — zero `QWheelEvent` in the suite today. | B / hyp-3 | |
| B-5 | **Rotated / landscape / mixed-size** render correctness (scaling/centering, not "canvas changed"). | — / hyp-4 | |
| B-6 | **Multi-document / multi-window** state isolation. | — / hyp-5 | Every fixture opens one doc |
| B-7 | **Encrypted-document edit→render** paths (decrypt→edit→render page 2). | — / hyp-6 | |
| B-8 | **High-DPI / multi-monitor** render correctness (current harness is single-DPI, PrintWindow varies >15% run-to-run — use structural oracles). | — / hyp-7 | |
| B-9 | **Keyboard-only / accessibility** end-to-end flows (open→navigate→edit→save by keyboard/Tab only). | C / hyp-8 | |
| B-10 | A **CI job** enforcing the N-1/N-2/N-3 policies (page-≥1 oracle lint, realization-assertion presence, reconciliation helper usage) as a coverage checklist gate. | A+C / meta-causes 1–3 | Institutionalizes the three implement-now policies |

---

## 4. Integration verification (Phase 5)

- **GoogleTest, full Debug suite:** 4055 tests, 4049 passed, 0 failed, 6 skipped
  (the 6 skips are the pre-existing XFA-slowness gate in Debug; identical to
  baseline + the 3 new `UiStructureBugs` tests). No interaction regressions from
  combining the P2/P3/P4 fixes.
- **E2E SpecimenA walkthrough:** open → Pages default (#3) → scroll all 51 pages
  → Replace All "the"→"THE" (all-page extraction incl. the `/ActualText` pages,
  #6) → armed re-scroll (#6) → save via the test pipe → re-extract: uppercase
  "THE" present in body pages (#4/#5). Process survives throughout; no orphan.
- **Test-first discipline:** every fix landed after a committed RED (P2/P3 karen
  revert-rebuilt to reproduce RED→GREEN; P4 was restructured into a dedicated
  RED commit `2ba5e0a` → GREEN `c465c85` after karen flagged co-committed seams).

## 5. Push decision
All Phase 1–5 work is committed LOCAL/unpushed. Pushing to origin triggers the
self-hosted windows-validation CI (~9 h). **Awaiting explicit user go/no-go
before pushing** (pipeline AC).
