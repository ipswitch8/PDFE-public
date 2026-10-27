# Bug-fix pipeline `3e91a7c4` — Phase 1: Root-cause + test-gap analysis

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


**Status:** Phase 1 (investigation only — no product/test code changed).
**Method:** 4 parallel source-investigation agents (crash / render / UI / suite-audit) + main-loop
runtime analysis (the actual user PDF + Windows registry). Every code citation below was
spot-verified by direct read. This document is simultaneously (a) the root-cause map, (b) the
"plan how to fix the tests" artifact (test gap + planned regression test per bug), and (c) the
test-suite blind-spot audit + hypothesized other-missed-classes the user asked for.

---

## 1. Reproduction target

Real PDF: `C:\Users\dev\Desktop\SpecimenA Green A Tale of Two Collards - Original Recipe -
Tuvache Betton.pdf` (spelling "SpecimenA Green", double-a; copies also in Documents/Desktop).

- **51 pages**, not encrypted, no password. All pages 594×738, rotation 0.
- page 0 = title, **24–50 pt** (large). pages 1+ = body, **9–14 pt** (small). Blank/image-only
  pages (0 chars): 12, 16, 23, 35, 44.
- **Key reframing:** the user's "first page works, second page's smaller text doesn't" correlates
  with **font size** (title = large, body = small), *not* page index — see Bug #4. The two happen
  to coincide in this document.

Proxy multi-page fixtures already in-repo (for tests): `tests/fixtures/image_pdf/c2_tiny_20page_72dpi.pdf`,
`c2_mixed_8page.pdf`, `c2_large_12page_600dpi.pdf`, `a3_mixed_dpi_5page.pdf`. A **new small-text
multi-page fixture** is needed for the render/edit regression tests (Phase 3), and this 51-page
doc should become a committed "real user PDF" regression fixture (Phase 5 process plan).

### Reproduction record — live, against the built `PDFE.exe`
Environment: `build/vs2022/bin/{Debug,Release}/pdfe.exe` (built 2026-07-06), the real 51-page PDF
above, driven with pywinauto (UIA) + pyautogui in the interactive desktop session. Screenshots saved
to this session's scratchpad.

- **#1 / #2 — REPRODUCED (screenshot).** On launch the toolbar renders as a run of text labels with
  **no icons**: `Select  Sticky Note  Highlight  Underline  Strike Out  Form Tool  Image Tool  Edit
  Text  Redaction Tool  Ink  Text Box  Rectangle  Circle  Line`. This is exactly the reported "series
  of words." (UIA enumeration also shows every toolbar button's Name is its text label.)
- **#3 — REPRODUCED (screenshot + UIA + registry).** The left dock shows only the **Layers** panel
  ("Layer Name" header, "Document has no layers (OCGs)"); the UIA tree's active left TabControl/TabItem
  is `Layers`, and there is **no Pages panel** tab. Consistent with `HKCU\...\Panels\pagePanel=false`.
- **#4 / #5 — REPRODUCED (definitive).** Search→Replace All of `the`→`THE` reported **"Replaced 627
  match(es)"**, but after the replace the body text on the content pages is **still entirely lowercase
  "the"** ("the best kind of feeling", "the weight of them", "the community", "one of the finest
  District Attorneys in the state" …) — **none** became "THE". Baseline `fitz` extraction of the
  original confirms body pages contain lowercase "the" (page 1: 12, page 2: 4, page 3: 3, page 4: 13)
  and **zero** "THE". So the count is reported from the model while the small-text content-stream
  writes never land — the exact reported signature. (Confirms §2 #4/#5 root cause against the real app.)
- **#6 — REPRODUCED (crash), and the mechanism is different from the static hypothesis.** Plain
  wheel-scrolling (720 events) and *armed* scroll after Replace All (Debug **and** Release, ~2900
  events total) did **not** independently crash — so the render-thread/main-thread race the crash
  agent proposed was NOT the trigger reproduced here. Instead, **Replace All's all-page text
  extraction raised a hard crash**: the Debug build popped `Microsoft Visual C++ Runtime Library —
  Debug Error! … abort() has been called`, from the MuPDF assertion **`z != 0` at
  `stext-device.c:1142`** (the structured-text/extraction path; Replace All extracts every page at
  `MainWindow.cpp:5816`). In a **Release** build the assert is compiled out, so `z==0` proceeds as
  undefined behavior — consistent with the user's `Access Violation: read address 0x0`. This is the
  reproduced, debugger-attributable crash site; the render-lifetime race (§2 #6) remains a plausible
  *secondary* defect in the same subsystem but was not the mechanism reproduced. **This correction is
  itself a data point for the process plan: static code-reading alone mis-identified the crash; only
  running the real app on the real document surfaced the true site.**

---

## 2. Per-bug: root cause → test gap → planned regression test

### Bug #6 — Crash on scroll (highest severity)
`Access Violation: read address 0x0` after scrolling a few pages (usually after an edit/Replace-All).

**REPRODUCED primary mechanism (see Reproduction record) — MuPDF `/ActualText` extraction underflow.**
The crash reproduced against the built app is a hard abort from MuPDF `stext-device.c:1142`
(`assert(z != 0)`) on the **text-extraction** path — reached by Replace All (`page->extractText()` for
every page, `MainWindow.cpp:5816`), and also by the text-edit commit and search.

> **CORRECTION (pinned in Phase 2 by reading the MuPDF source — supersedes the initial guess here).**
> `z` is NOT a text-matrix scale. In `do_extract_within_actualtext`, `z = fz_utflen(actualtext)` is the
> **rune length of the span's `/ActualText` string** (a marked-content accessibility override). When
> `/ActualText` is a proper **prefix** of the displayed span (e.g. the span shows `ABC` with
> `/ActualText (A)`), the prefix-match loop consumes all of ActualText (`z → 0`) while the span still
> has unconsumed items, tripping `assert(z != 0)`; and because `z` is `size_t`, line 1148's
> `rune_index(actualtext, z - 1)` evaluates `z - 1 == SIZE_MAX` and reads out of bounds. Debug →
> `abort()`; Release (assert compiled out) → OOB read → the reported `read address 0x0`. Pages **47 and
> 48** of the SpecimenA PDF carry such `/ActualText`. **Fixed in Phase 2 (commit 5873dd9):** PDFE's text
> pipeline sets `FZ_STEXT_IGNORE_ACTUALTEXT` at all extraction sites (a shared `pdfeStextOptions`
> helper) — correct editor semantics (extract the displayed glyphs the user edits, not the ActualText
> override) and it bypasses the crashing path. Regression: `tests/test_actualtext_extraction_crash.cpp`.

**Secondary / related (static hypothesis, NOT the reproduced trigger) — missing synchronization barrier in the async tile-render pipeline.**
The `PdfCanvas` widget layer is fully bounds-/null-guarded; the fault is a use-after-free between
worker render threads and main-thread document mutation:
- Worker side: `RenderTask::run()` calls `fz_run_display_list(...)` — `src/rendering/TileRenderer.cpp:79`
  (workers created `idealThreadCount-1`, `:148`; render from `:174`).
- Main side: `Document::createDisplayList()` drops the whole page tree for each dirty page —
  `pdf_drop_page_tree(ctx, pImpl->pdfDoc)` at `src/core/Document.cpp:629-636`; and does **not**
  null-check `fz_load_page` at `:639-643`.
- Cancellation does **not join**: `TileRenderer::cancelPage`/`cancelAll` (`:195-219`) only
  `store(true)` an atomic + erase the map — `waitForDone()` is called *only* from `~TileRenderer`
  (`:155`). `PdfCanvas::refreshPage/refreshAllPages` (`:229-251`) cancel but never wait.
- `createDisplayList` holds `Document::docMutex` (`:625`) but **workers never take `docMutex`**, so
  it does not serialize `pdf_drop_page_tree` (main) against `fz_run_display_list` (workers).

**Trigger sequence:** edit / Replace-All → `markPageDirty()` (`MainWindow.cpp:5608/5668`,
`TextEditCommands.cpp:382`) + `refreshAllPages()` (`MainWindow.cpp:5617/5846`) → `cancelAll()` flags
but does **not** join → user scrolls → `requestVisibleTiles()` → `createDisplayList(dirtyPage)` →
`pdf_drop_page_tree()` **while** old workers are inside `fz_run_display_list` → deref freed state.
"After a few pages" = accumulating in-flight workers + 256 MB shared MuPDF store crossing eviction
(`MuPdfContext.h:31`) + each revealed dirty page re-dropping the tree. *(Both the crash and render
agents independently converged on `createDisplayList`/`pdf_drop_page_tree`.)*

**Test gap:** the only concurrency test, `tests/test_tile_renderer.cpp`, uses the trivial
`test_3page.pdf` (`:22`), never mutates the document while renders are in flight, and *tolerates*
the race (`CancelPage`/`CancelAll` `:109-172` only assert "no crash + still usable", `:153` "some
tiles may still arrive"). No test scrolls a real multi-page doc through every page; none drives
edit→refreshAllPages→scroll as one sequence; the suite runs with **no ASan/TSan**
(`tests/CMakeLists.txt` has no `-fsanitize`). **And — the reproduced mechanism — no test extracts text
from a document containing marked-content `/ActualText`:** the extract/search/edit tests use hand-built
trivial fixtures (`test_3page.pdf` etc.) that never carry an `/ActualText` span (let alone one that is a
proper prefix of its displayed text), so the `stext-device.c:1142` assertion / Release UB is entirely
uncovered.

**Planned regression tests (test-first, Phase 2):**
- **(A0 — PRIMARY, matches the reproduced crash) — DONE in Phase 2 as `tests/test_actualtext_extraction_crash.cpp::PrefixActualTextDoesNotAbortAndReturnsDisplayedGlyphs`** (commit 5873dd9), with the minimal
  synthetic fixture `tests/fixtures/actualtext_prefix_crash.pdf` (span shows `ABC` with `/ActualText (A)`).
  Confirmed RED against unmodified code (`Assertion failed: z != 0 … stext-device.c, line 1142`, the exact
  reproduced site) then GREEN after the fix — extraction returns the displayed `ABC`. Runs under the Debug
  MuPDF (asserts enabled), so the assertion is a hard failure if the crashing path is ever re-entered.
- (A) GoogleTest `tests/test_tile_renderer.cpp::RenderDuringDocumentMutationIsSafe` — open a
  resource-heavy ≥10-page fixture, launch high-fan-out tile requests across all pages, then
  concurrently `markPageDirty(i)` all pages + `cancelAll()` + a `createDisplayList` sweep (drives
  `pdf_drop_page_tree`). Oracle: zero crash/ASan-AV, every arriving tile non-null. Run under
  ThreadSanitizer/AddressSanitizer so the data race is flagged deterministically (not luck). (Covers
  the secondary render-lifetime hypothesis.)
- (B) pywinauto `tests/ui_automation/test_ui_scroll_stress.py::test_edit_replaceall_then_scroll_survives`
  — open the fixture, do a text edit + Replace-All, then scroll page 1→last. Oracle: `PDFE.exe`
  still alive (no fatal-error/abort dialog / exit) and `currentPage()` reaches the final page. This is
  the end-to-end form of the exact sequence that reproduced the crash.

> **Status of (A)/(B) — DEFERRED to the Phase 5 backlog, not dropped.** Phase 2 fixed the *reproduced*
> crash (A0). Tests (A) and (B) cover the *secondary, unreproduced* render-lifetime race hypothesis; the
> missing cancel-and-join barrier (`TileRenderer::cancelPage`/`cancelAll` never `waitForDone`) is a real
> latent hazard even though it was not the mechanism that crashed here, so it is tracked as a hardening
> item in the Phase 5 testing-process plan (add a scroll-stress soak test + an ASan/TSan CI lane) rather
> than being silently closed.

---

### Bugs #4 & #5 — page-2 edits / Replace-All not rendered (SHARED root; original hypothesis REFUTED)

The planning hypothesis ("a refresh hard-coded to page 0/currentPage") is **refuted** — every layer
uses the true edited page index and invalidates/repaints the correct page:
`PdfCanvas::mousePressEvent` → `findPageAtY` (`PdfCanvas.cpp:646`); `TextEditTool::commitEdit` emits
`textEdited(currentPageIndex_, …)` (`TextEditTool.cpp:1716`); `refreshPage(pageIndex)` →
`tileCache_->invalidate(pageIndex)` (`PdfCanvas.cpp:229-238`; `TileCache.cpp:77-91` erases exactly
that page). Refresh is page-correct.

**Actual root cause — the content-stream WRITE silently no-ops for small text.**
Both bugs funnel through `EditTextCommand::applyText` → `ContentStreamEditor` → `textFilterCallback`,
which selects glyphs belonging to the edited span with a **strict vertical-center-in-bbox test with
zero vertical tolerance** (`src/textedit/ContentStreamEditor.cpp:586-600`, verified):
```cpp
const float charMidY = (fitzCharBbox.y0 + fitzCharBbox.y1) * 0.5f;
... if (charMidY < repl.bbox.top() || charMidY > repl.bbox.bottom()) return false; // no y-tol
... if (replIt == end) return 0; // KEEP original — nothing removed, nothing injected
```
The comment (`:563-585`) states the vertical tolerance was **deliberately removed** to stop adjacent
lines (2–3 pt apart) from being captured. Side effect: a small body-text span's bbox y-range is only
a few points tall, so any small vertical-convention mismatch (fitz bottom-left vs model coords /
union-of-char-quads tightness / rounding) pushes `charMidY` outside the tiny window → **no glyph
matches → no `PendingInjection` queued (`:607-620`) → `/Contents` unchanged**. `applyText` still runs
`markPageDirty` + refresh, so the page re-renders the **original** content while the in-memory
`TextEditModel`/overlay shows the edit. Exactly the reported signature: *"visible in the edit view but
never on the rendered page,"* and size-dependent ⇒ *"all of the smaller text."* Large title text on
page 0 has a tall bbox, so its center still lands inside ⇒ page 0 works.

**#5 shares this defect AND adds a count/write decoupling (verified `MainWindow.cpp:5812-5846`):**
`totalReplaced++` (`:5830`) counts matches from the **model** string replace (`newText.replace`,
`:5828`), independent of whether the content-stream write later lands. So Replace-All reports a
correct count while the small-text pages never change on screen.

*Confidence:* high on the mechanism (code-verified, exact symptom match). The precise numeric reason
small spans miss the filter will be pinned by the Phase 3 red test that parametrizes font size.
Crash #6 is the same render subsystem and is *armed by* these edits, but is a distinct defect.

**Test gap:** all edit/replace oracles inspect **page index 0 only** and never use small font sizes:
`tests/test_text_color_and_replace.cpp` / `tests/test_replace_page.cpp` load a 3-page fixture but
operate on `document->page(0)` at `:124/157/183/221/430/452/556` — **zero** `page(1)`/`page(2)` calls;
`tests/ui_automation/test_ui_search_replace.py` asserts only button reachability + dirty state
(`:5-13`, `:93-228`) and **never re-renders/reads back** pages 2+. Three blind spots stack here:
page > 0, small font size, and **model/overlay-vs-rendered divergence never reconciled**.

**Planned regression tests (test-first, Phase 3):**
- (a) `tests/test_text_edit_page2_render.cpp` (+ Python render-oracle variant) — edit a small-text
  span on `page(1)`, parametrize sizes {6, 8, 10, 18}; **read back** via `extractPlainText()` +
  render; assert new text present / original absent in the *rendered/content-stream* result (not the
  model). Expect FAIL at 6–8 pt pre-fix.
- (b) `tests/test_replace_all_all_pages.cpp` (+ UI variant) — token on pages 0/1/2 incl. small text;
  `onReplaceAll`; per-page `extractPlainText()`+render assert replacement present/original absent on
  every matched page; **assert reported count == number of spans whose content stream actually
  changed** (couples the count to the write, guarding `MainWindow.cpp:5828-5830`).

---

### Bug #1 — Menu/toolbar sections "just look like a series of words"
**Root cause:** separators mostly **exist** (menus ~40 `addSeparator()`; main toolbar
`MainWindow.cpp:3960/3963`; annotation toolbar `:4086/4120/4138/4156/4320`). The "series of words"
appearance is driven by **Bug #2 (no icons)** + text-only buttons (no `setToolButtonStyle(...)`
anywhere in `src/`) so QToolBar's 1-px separators are lost between wide text labels. The **Form
toolbar** (`:4007-4040`, 7 field actions back-to-back) is the one genuine no-separator gap (hidden by
default → low impact).
**Test gap:** no test asserts default menu/toolbar section separators; the only `isSeparator()`
assertions are the customization round-trip (`test_toolbar_customization.cpp:139/170`) and
context-menu ordering (`test_chrome_phase9.cpp:939`).
**Planned test (Phase 4):** `MainToolBarHasSectionSeparators` (≥2 `isSeparator()` among mainToolBar
actions) + `FormToolBarSeparatesFieldGroups` (≥1 separator among Form actions).

### Bug #2 — Missing button icons
**Root cause:** **124** `new QAction` in `MainWindow.cpp`, **0** call `setIcon`. The only icons in
code are the app/window icon (`:207`), a message-box glyph (`:2420`), and an annotation icon *name*
string (`:4310`). **No toolbar-icon assets exist** — the only `.qrc` are `src/app/pdfe_icons.qrc`
(app `.ico`) and `src/ui/branding.qrc` (About-dialog art); no `*.png`/`*.svg` under `src/` or
`resources/`. ⇒ icons must be **created** and added to a `.qrc`, not merely wired.
**Test gap:** only the app icon is tested (`test_pdfe_icons.cpp:30-73`); **zero** assertions that any
toolbar `QAction` has a non-null `QIcon`.
**Planned test (Phase 4):** `ToolbarActionsHaveIcons` — enumerate the toolbar/tool actions,
`EXPECT_FALSE(a->icon().isNull())`.
**Scope note:** Phase 4 must *create* an icon asset set (~20+ actions) — non-trivial design work.

### Bug #3 — Pages panel missing / not the default left-dock tab
**Root cause (compound):** `createDockPanels()` (`MainWindow.cpp:4372`) *does* add Pages and hide the
tab-siblings for a fresh profile (`:4402` addDockWidget Pages; `:4419-4421` tabify+hide Bookmarks;
`:4440-4441` tabify+hide Layers). But: **(a)** there is **no explicit `pagePanelDock_->raise()`** (the
right dock raises its default via `searchPanelDock_->raise()` at `:4533`; the left dock has no
equivalent), and **(b)** `restoreState(savedState)` at `:2282` (after `createDockPanels()` at `:2110`)
reapplies the *previous session's* dock layout, overriding the default. **Runtime confirmation:** the
registry `HKCU\Software\PDFE\PDFE\Panels` has **`pagePanel=false`** persisted (propertiesPanel=true,
searchPanel=true, bookmarkPanel=false) ⇒ Pages is persisted hidden. So stale persisted state + no
forced default = "only Layers shows." Both `PagePanel` and `LayerPanel` are constructed/added — the
panel is not omitted.
**Test gap:** no test asserts the default/active left-dock panel on fresh launch;
`test_startup_ui_changes.cpp` checks `allowedAreas()` + existence (`:56-94`), never actual
`dockWidgetArea()` / raised tab / Pages-default; the `restoreState()` override path is untested;
`layerPanelDock` appears in **0** `.cpp` tests.
**Planned test (Phase 4):** `PagesPanelIsDefaultLeftTab` — fresh (empty) settings: Pages visible +
raised, Layers/Bookmarks hidden; and after a `restoreState()` carrying a Layers-raised layout, the
app still forces Pages to the default; + a pywinauto check that the left dock's active tab Name ==
"Pages" (`auto_id` `pagePanelDock`).

---

## 3. Shared-root & cross-bug analysis
- **#4 ≡ #5:** same content-stream small-text write no-op; #5 adds the model-count/write decoupling.
- **#6:** distinct defect but same multi-page render subsystem, and **causally armed by #4/#5's**
  edit/Replace-All (`markPageDirty`+`refreshAllPages`→`cancelAll` without join). Fixing #4/#5 reduces,
  but does not remove, #6 — #6 needs the join barrier independently.
- **#1 depends on #2:** icons are what make the buttons read as buttons; #3 is independent.

## 4. Bug-CLASS classification + suite blind-spot audit

| Class | Bugs | Suite can catch today? | Evidence |
|---|---|---|---|
| **A. Committed-output correctness beyond the trivially-visible case** | #4, #5 | **No** | All render oracles `page_index=0` (`render_drive.py:408-506` default 0; zero non-zero call-sites); **delta** oracles (`assert_changed`) not **content** oracles; edit/replace gtests `page(0)` only; Replace-All UI test never reads back. |
| **B. Long-session / scroll stability & object lifetime** | #6 | **No** | Zero `QWheelEvent`/`WM_MOUSEWHEEL` in tests; stress uses `Ctrl+G` jumps + not-blank check (`test_ui_stress.py:1421/623`); no ASan/TSan; lifetime coverage only in the tile-renderer unit on trivial content, tolerating the race. |
| **C. UI structural / affordance completeness** | #1, #2, #3 | **No** | Only app icon tested (`test_pdfe_icons.cpp`); separators only in customization round-trip; docks only `allowedAreas()`+existence — no active-tab / Pages-default / `restoreState` path. |

**Three cross-cutting meta-causes** (these explain *why the whole class* ships, not just the instance):
1. **Delta oracles over content oracles** — the suite asks "did pixels change on the visible page,"
   never "is page N's content correct." A silent no-op leaves the original content in place → a delta
   check (only ever run on page 0) is structurally blind to it.
2. **Reachability over realization** — the UIA-tree suite verifies a widget *exists / is reachable*,
   not that it is *visually complete* (iconed, separated, in the right dock, the default tab).
3. **Trust-the-model over verify-the-output** — success/counts are read from the in-memory
   model/overlay and **never reconciled against the rendered/extracted content stream**. The
   edit-view-vs-page divergence (#4) and the correct-count-with-no-change (#5) are both this.

## 5. Hypothesized OTHER missed bug-classes (ranked, each with suite evidence)
1. **Any edit/op on a non-displayed page** (annotations, redaction, form fills, stamps, image edits on
   page 2+) — same `page_index=0` default; `test_render_annotations.py`/`test_render_redaction.py`
   never navigate. A redaction that fails to apply on page 3 would ship.
2. **Save-then-reopen round-trip fidelity beyond page 0** — round-trip tests verify model/annot
   *counts*, never re-render a reopened page 2 and compare pixels.
3. **Mouse-wheel / trackpad continuous scroll** generally (tile seams, prefetch ring, momentum page
   transitions) — zero `QWheelEvent` anywhere; `TilePrefetcher` unit-tested with synthetic inputs.
4. **Rotated / landscape / mixed-size page rendering correctness** — nav tests assert "canvas
   changed", not correct scaling/centering.
5. **Concurrent multi-document / multi-window state isolation** — every fixture opens exactly one doc.
6. **Encrypted / password-protected document EDIT paths** — encryption tested only as save/reopen
   round-trip; `encrypt_then_sign` is xfail-INFRA; decrypt→edit→render-page-2 is unexercised.
7. **High-DPI / multi-monitor render correctness** — `test_hidpi.cpp` is headless scaling math; the
   live harness is single-DPI and its `PrintWindow` output "varies >15% run-to-run."
8. **Keyboard-only / accessibility end-to-end flows** — accessible *names*/handlers are asserted; no
   full open→navigate→edit→save workflow driven by keyboard/Tab focus only.
9. **Success reported without verifying the write landed** (generalization of #5) — any UI
   count/status derived from the model rather than the persisted/rendered result.

*(Concrete testing-process improvements to close these are specified in Phase 5's closure doc.)*

## 6. Handoff to the fix phases (strict test-first)
- **Phase 2 (crash):** tests A+B **first** (red under ASan / scroll-stress), then the cancel-and-join
  barrier + null-check `fz_load_page` + coalesce the per-page tree drop.
- **Phase 3 (#4/#5):** tests (a)+(b) **first** (red at 6–8 pt / pages 2+), then a bounded,
  font-size-aware vertical tolerance (or baseline y-overlap) in `textFilterCallback`, couple the
  Replace-All count to the actual write, and make a zero-glyph-match edit *observable* (no silent
  success).
- **Phase 4 (#1/#2/#3):** the 3 UI-structure tests **first** (red), then Form-toolbar separators +
  create/wire an icon set + force Pages as the default left tab after `restoreState()`.
- **Phase 5:** the testing-process improvement plan mapped to the three meta-causes and the nine
  hypothesized classes (page-2+ render-oracle policy; content-oracle-not-delta; realization-not-
  reachability UI assertions; model-vs-rendered reconciliation; scroll-stress + sanitizer CI job; a
  committed real-user-PDF multi-page corpus incl. this 51-page document).
