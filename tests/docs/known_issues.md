# PDFE Known Issues / Documented Limitations

<!-- tracking-banner -->
> **STATUS: LIVE OPERATIONAL STATE — keep maintaining this file.**
>
> This is **not** a defect tracker and is **not** superseded by GitHub Issues;
> it is state that code or CI reads. **GitHub Issues (`ipswitch8/PDFE`) remains
> the canonical tracker for anything still open** — a defect recorded only here
> is still an untracked defect, so file an issue and cross-reference it.
>
> See [`docs/TRACKING.md`](../../docs/TRACKING.md).

---


**Purpose:** Track product behavior that's intentionally limited or
known not to work, so tests can document the boundary rather than
silently failing.

**Two ID prefixes used in this file** (both equally valid for the
"documented limitation" category in pipeline acceptance criteria):

- **PDFE-LIMITATION-\***: A product-level behavior limitation that
  affects all users (e.g., PDFE-LIMITATION-UNICODE-PATH). Resolution
  belongs in PDFE source code.

- **CI-LIMITATION-\***: A test/CI environment limitation that does
  NOT affect users (e.g., CI-LIMITATION-VISUAL-REGRESSION-SESSION-
  DRIFT). Resolution belongs in test infrastructure or CI workflow.

Where Phase 3 (or any phase) acceptance criteria reference
"PDFE-LIMITATION-* ID," entries using either prefix satisfy the
criterion provided the resolution path is documented.

---

## CI-LIMITATION-CANVAS-SYNTHETIC-DRAG

> **Triage 2026-09-06:** FIX - filed as GitHub #137. This entry stays as the evidence record; the issue tracks the remaining work.

**Discovered:** 2026-06-10 (pipeline f3c91a2e Phase 3 Cluster #7)
**Affected tests:**
- `tests/ui_automation/test_canvas_phase3.py`:
  `canvas_annot_handle_drag_corner`, `canvas_annot_handle_drag_edge`,
  `canvas_annot_handle_drag_rotate`, `canvas_canvas_rubber_band_select`
- `tests/ui_automation/test_canvas_coords_p2.py`:
  `canvas_annot_move_drag`, `canvas_canvas_click_away_deselect`,
  `canvas_canvas_scroll_wheel`

**Symptom:** Synthetic Qt mouse events dispatched via
`PDFE_TEST_PIPES` (testCanvasDragPipe / testClickByPdfCoordPipe) do
not reliably reproduce the annotation handle-drag / rubber-band /
multi-select state machine. Locally and on CI we observe identical
deterministic failures: drag rect after operation matches drag rect
before (dx=dy=0), or pywinauto's UIA proxy throws -2147417851
"server threw an exception" while enumerating descendants under
a partly-populated comment tree.

**Root cause:** The pipes call PdfCanvas::clickByPdfCoord /
PdfCanvas::dragByPdfCoord which use `canvasRectFromPdf` to map a PDF
point to a screen pixel — but a handle is a 7×7 visual rendered AT
the annotation's PDF corner. The pipe's screen coord lands ON the
annotation rect, not on the handle pixel that owns the resize hit
test. Without exposing the handle centroid via a diagnostic
`testHandleScreenPointPipe` (Q_INVOKABLE), the tests cannot land a
drag inside the 7px hit zone.

**Test status:** RESOLVED 2026-09-08 (pipeline backlog-workable-p82
phase-17). All seven named rows run and pass; measured **three consecutive
times with no flake** (7 passed / 0 failed on each run; the four additional
skips in those runs are duplicate parametrizations in
`test_canvas_coords_p2.py` that are deliberately deferred to the canonical
driver in `test_canvas_phase3.py`).

**How it was resolved — and most of it was NOT resolved here.**

* The three handle-drag rows and the three `test_canvas_coords_p2.py` rows
  were recovered in `c82a1f37` P4 (2026-07-04) by the seam this row asked for:
  `testAnnotHandles{Trigger,Result}Pipe` returns the SELECTED annotation's true
  handle centroids, and `testAnnotSelectPipe` drives `SelectTool::selectByIndex`
  so the selection is live. This row was simply never updated, so it went on
  describing six tests as blocked for two months after they were fixed.
* Only `canvas_canvas_rubber_band_select` remained, and its recorded root cause
  was **wrong**. A rubber band is not a handle grab, so "the pipe cannot land
  inside a 7px hit zone" never applied to it. What it lacked was an ORACLE. The
  handler read a status-bar message with a 3-second transient timeout and, when
  it missed it, returned **green** for "pdfe did not crash and the comment tree
  still has two rows" — which is green for a drag that did nothing at all, and
  green for a drag that landed off the page.
* The multi-selection count the resolution path asked for (item 3) **already
  exists**: `PdfCanvas::selectedAnnotationCount()` is reported as `selCount` by
  the view-state pipe. No new pipe was added; a second mechanism for the same
  fact would have been the wrong answer. The handler now reads `selCount`
  before and after the drag and requires `<2 -> >=2`, treats a non-answering
  seam as a failure rather than a pass, and the row was promoted from the
  (now false) `wont-fix` status in `ui_test_matrix.csv`.

**Mutation-verified:** suppressing the drag-pipe write reddens the row —
*"rubber-band drag selected 0 annotation(s), expected at least 2 (before=0,
created=3)"*. The oracle it replaced returns **green** in that exact state,
which is the point.

**Recorded, not fixed:** the committed matrix said `wont-fix` for a reason
("no seam returns the selected-item set") that stopped being true when the
view-state pipe gained `selCount`, and `conftest._write_matrix_updates`
preserves committed `wont-fix` rows — so the live green result was being
DISCARDED on every run. A `wont-fix` row whose stated reason has expired is
invisible in both directions. Same class as GitHub #142.

---

## CI-LIMITATION-COLORDIALOG-TIMING - RESOLVED (GitHub #136, pipeline p84-remaining-open phase-5, 2026-09-12)

> **Triage 2026-09-06:** FIX - filed as GitHub #136.
> **RESOLVED 2026-09-12.** The skip is gone, the row runs, and it passed
> **3/3 consecutive runs**. See **Resolution** at the end of this entry -
> and note the recorded root cause below was WRONG.

**Discovered:** 2026-06-10 (pipeline f3c91a2e Phase 3 Cluster #7)
**Affected test:** `tests/ui_automation/test_annotations_p1.py::
test_p1_annotation_tool[annot_textmarkup_set_color]`

**Symptom:** ~30% of CI runs report "TextMarkup: QColorDialog did
not appear after tab activate + tree select" even though Cluster #5
(commit deb46a0) added the tab-activate + tree-select sequencing.

**Root cause:** QColorDialog opens at a Z-order below the parent
window under non-interactive CI session — the wait-for-dialog poll
calls `WindowSpecification.wait("visible")` against a top-level
that does briefly exist but is obscured, so the visibility check
oscillates and misses the dialog.

**Test status:** Marked skip with `record_matrix_result` status
`blocked`.

**Resolution path (Phase 7):**
1. Replace UIA wait with HWND enumeration via
   `EnumThreadWindows(GetCurrentThreadId(), ...)` filtering by
   class name `"#32770"` (Win32 dialog class).
2. Or expose `Q_INVOKABLE testColorDialogVisible` returning a
   bool from `MainWindow::isColorDialogActive()`.

### Resolution (GitHub #136, 2026-09-12)

**The recorded root cause was wrong.** The dialog was never "obscured" and
its Z-order was never the problem. Neither was `wait("visible")` - the
handler did not call it. What `_op_set_color` actually did was

    for d in fx.main_window.descendants(control_type="Window"): ...

which is a **UIA enumeration**, and UIA cannot see an app-modal Qt dialog at
all (CLAUDE.md; measured elsewhere in this repo as a dialog open for four
minutes while a 240 s UIA poll reported nothing). That is why it missed
~30% of the time - nothing to do with Z-order.

**Fix - detect with Win32, drive with UIA-by-handle.**

* `tests/helpers/modal_dialog.py::wait_for_window` (raw `EnumWindows` +
  `WM_GETTEXT`) locates the dialog by its real title,
  `"Select Annotation Color"`. **Measured: 0.22 s** from the
  `annotColorButton` click, versus the enumeration that could never see it.
* New `tests/helpers/modal_dialog.py::uia_dialog(hwnd)` wraps the *known*
  handle. Detection and interaction are different problems and only
  detection was broken: handed the hwnd, `Desktop(backend="uia").window(
  handle=hwnd).descendants()` listed all 32 children including
  `qt_colorname_lineedit` and the OK button.

**Two real defects the un-skip exposed, both now fixed in the test:**

1. **The hex value never reached the colour.** The handler used
   `set_edit_text()`, i.e. the UIA ValuePattern, which calls
   `QLineEdit::setText()` and emits `textChanged`. Qt's `QColorShower`
   listens on **`textEdited`**, which only fires for real user input - so
   the HTML field showed `#ff0000`, the picked colour stayed put, and OK
   returned the original colour. Now typed with real keystrokes.
2. **The oracle was vacuous.** It asserted the hex field read back the
   string the test had just typed *into that same field*, so it was green
   for defect (1). The oracle is now a **reopen readback**: after OK the
   dialog is reopened and its seed colour - which
   `QColorDialog::getColor(currentData_.color, ...)` takes from the
   annotation-properties model - must equal the colour picked.

**Mutation-verified.** `AnnotationPropertiesWidget::onColorButtonClicked`
line 229, `if (newColor.isValid() && ...)` -> `if (false && ...)` (marker
`PDFE_MUT_F136_M3`, grepped present before the build). Test reddens:
*"TextMarkup: colour did not reach the annotation model - picked #ff0000
but the reopened QColorDialog seeds from '#ffff00' (was '#ffff00'
before)"*. Reverted; marker grep clean; test green again.

**Runs:** `annot_textmarkup_set_color` 3/3 pass. All five `*_set_color`
rows (StickyNote, TextMarkup, Ink, Shape, FreeText) pass under the
strengthened oracle.

---

## CI-LIMITATION-FREETEXT-CREATE-TIMING - RESOLVED (recorded late; fixed in c82a1f37 P4/P5, confirmed GitHub #136 2026-09-12)

> **Triage 2026-09-06:** FIX - filed as GitHub #136.
> **RESOLVED.** This entry was **stale, not open**: `_KNOWN_LIMITATION_IDS`
> in `tests/ui_automation/test_final_phase8.py` has been the empty set since
> pipeline c82a1f37 P4/P5, and the file's own comments say so. All five
> named rows run today and passed **3/3 consecutive runs** (2026-09-12,
> pipeline p84-remaining-open phase-5). Nothing was changed to achieve
> that - only this entry was wrong. Same failure mode as the
> canvas-handle-drag row above: a limitation record left describing tests
> as blocked for months after they were fixed.

**Discovered:** 2026-06-10 (pipeline f3c91a2e Phase 3 Cluster #7)
**Affected tests:** `tests/ui_automation/test_final_phase8.py`:
`annot_select_set_color`, `annot_select_set_opacity`,
`annot_select_set_contents`, `annot_select_delete`,
`page_rotate_respects_annotations`

**Symptom:** All 5 tests share the `_create_and_select_annotation`
helper which creates a FreeText annotation via PyAutoGUI screen-
coord drag + typewrite. CI reports "Failed to create and select
annotation" with no exception detail.

**Root cause:** The PyAutoGUI typewrite is dispatched while the
QInputContext is still attaching to the newly-created FreeText
editor widget — under non-interactive session this can take 200-
800ms. The fixed `time.sleep(0.5)` between drag and typewrite is
intermittently shorter than the IME init.

**Test status:** Marked skip with `record_matrix_result` status
`blocked`.

**Resolution path (Phase 7):**
1. Expose `Q_INVOKABLE testFreeTextEditorReadyPipe` that returns
   true when the inline editor accepts input.
2. Replace PyAutoGUI typewrite with `Q_INVOKABLE
   testTypeIntoFreeTextEditor` that sets the editor text directly
   via the QTextEdit::setPlainText path.

---

## CI-LIMITATION-PAGEPANEL-POPULATE-RACE - RESOLVED (GitHub #136, pipeline p84-remaining-open phase-5, 2026-09-12)

> **Triage 2026-09-06:** FIX - filed as GitHub #136.
> **RESOLVED 2026-09-12** by a real readiness signal (not a longer poll).
> All four named rows run and passed **3/3 consecutive runs**. See
> **Resolution** at the end of this entry.

**Discovered:** 2026-06-10 (pipeline f3c91a2e Phase 3 Cluster #7)
**Affected tests:** `tests/ui_automation/test_page_operations_p1.py`:
`page_multi_select_ctrl_click`, `page_multi_select_shift_click`,
`page_undo_delete`, `page_large_doc_delete_range`

**Symptom:**
- multi_select tests: "rotate after Ctrl+click/Shift+click multi-
  select failed" — the rotate fires but the dirty marker doesn't
  appear, indicating the multi-select state was lost before the
  rotate executed.
- undo_delete + large_doc_delete_range: "fixture has only 0 pages"
  / "expected 100-page fixture, got 0 pages" — `_page_count_from_panel`
  returns 0 immediately after launch on the CI runner; the
  PagePanel hasn't populated its `QListWidget` by the time the
  test queries UIA.

**Root cause:** The PagePanel populates asynchronously after
document open (Document::onDocumentStructureChanged emits
pagesChanged which the panel handles via Qt::QueuedConnection).
Under interactive desktop session this lands in <100ms; under the
GitHub Actions runner subprocess session it can take 1-2 seconds.
`_wait_page_items` already polls but its `min_count` floor (e.g.
50 for 100-page test) is reached BEFORE the population settles,
returning a partial item list that the next operation rejects.

**Test status:** Marked skip with `record_matrix_result` status
`blocked`.

**Resolution path (Phase 7):**
1. Expose `Q_INVOKABLE testPagePanelItemCountPipe` returning
   `pagePanel_->count()` directly (no UIA enumeration).
2. Add a `_wait_for_page_panel_settled` helper that polls the new
   pipe for two consecutive identical reads separated by 500ms
   before returning.

### Resolution (GitHub #136, 2026-09-12)

**Half of it was already fixed and the record had drifted.**
`page_undo_delete` and `page_large_doc_delete_range` were un-blocked for
real by the `testPageCount` pipe in an earlier phase-7. Only the two
modifier-click rows were still skipping - and the `record_matrix_result`
string they wrote still said PAGEPANEL-POPULATE-RACE while the code comment
five lines above it attributed them to
CI-LIMITATION-UIA-MODIFIER-CLICK-FULL-SUITE. Two different causes recorded
for one skip.

**The readiness signal.** New seam, gated by `PDFE_TEST_PIPES` exactly like
its neighbours and statically dead in a `-DPDFE_TESTING=OFF` tree:

* `PagePanel::panelItemCount()` - rows currently in the thumbnail
  `QListWidget`.
* `MainWindow` pipe pair `testPagePanelReadyQueryPipe` /
  `testPagePanelReadyResultPipe`, answering `"<panelItems>,<docPages>"`.

`PagePanel::generateThumbnails()` appends every row **synchronously** (only
the thumbnail *images* arrive later via `onThumbnailReady`), so
`panelItems == docPages > 0` is an **exact** "the panel has finished
populating" predicate. It cannot be true of a half-built panel and it goes
true the instant population ends - it is not a sleep and not a constant.

**What it replaced.** `_wait_page_items(min_count=N)` polled the UIA tree
for a hand-picked FLOOR (3 for the 3-page fixture, 50 for the 100-page
one). A floor is satisfied *before* the panel settles - which is literally
the recorded root cause - and it cannot tell "still populating" from "the
UIA tree is under-reporting". `_wait_page_items` now waits on the signal
for the panel's TRUE terminal count, then polls UIA for **exactly** that
many `ListItem`s, and returns `[]` (caller fails) if the signal never
answers. The failure message names the property:
*"...Readiness signal (panel rows vs document pages): panelItems=2
docPages=3"*.

**Mutation-verified - twice, and the second one is the important one.**

* **M1, under-populate.** `PagePanel::generateThumbnails()` line 249,
  `document_->pageCount()` -> `document_->pageCount() - 1` (marker
  `PDFE_MUT_F136_M1`, grepped present before the build). Both multi-select
  rows redden with the property named: *"PagePanel did not present 3
  selectable page items: have 0. Readiness signal (panel rows vs document
  pages): NOT READY after 20s - panelItems=2 docPages=3"*. Reverted, marker
  grep clean, rows green again.
* **M2, is the signal real or decorative?** `PagePanel::setDocument()`
  delayed population by 20 s (`QTimer::singleShot`, marker
  `PDFE_MUT_F136_M2`). With the honest signal the row **passes** in 29.6 s -
  it waited for the actual event rather than a constant. Then the seam was
  additionally made to lie (`items = pages`, always ready, marker
  `PDFE_MUT_F136_M2B`) with the same delay: the row **fails** in 18.0 s with
  *"...have 0. Readiness signal (panel rows vs document pages):
  panelItems=3 docPages=3"* - the seam claiming ready while UIA sees
  nothing. So the equality predicate is load-bearing; an always-ready seam
  does redden the test. Both reverted, marker grep clean.
  *(Recorded honestly: at a 6 s and a 12 s delay the always-ready mutant
  SURVIVED, because the downstream exact-count UIA poll has an 8 s window
  and absorbed the delay. The mutant only discriminates once the delay
  exceeds that window. That 8 s poll is itself a wait on an exact count
  rather than a sleep, so it is a genuine second line of defence - but the
  seam alone was not the only thing holding those two cases up.)*

**Runs:** all four rows - `page_multi_select_ctrl_click`,
`page_multi_select_shift_click`, `page_undo_delete`,
`page_large_doc_delete_range` - 3/3 pass.

---

## CI-LIMITATION-REDACTION-APPLY-FOCUS

> **Triage 2026-09-06:** FIX - filed as GitHub #134. This entry stays as the evidence record; the issue tracks the remaining work.

**Discovered:** 2026-06-10 (pipeline f3c91a2e Phase 3 Cluster #7)
**Affected test:** `tests/test_ui_redaction.py::
test_unmarked_text_still_present_after_save_reopen`

**Symptom:** Post-Apply, the extracted page-0 text is empty
(`''`) — even the bottom paragraph that was NEVER in the marked
region. Reads as if the Apply truncated the entire content stream.

**Root cause:** Suspected confirmation-dialog focus race —
`_accept_qmessagebox` returns true after the dialog dismisses, but
under non-interactive session the dismiss-event arrives at a
different thread than the main-window thread, and Apply returns
prematurely.

**Test status:** Marked `@pytest.mark.xfail(strict=False)`.

**Resolution path (Phase 7):**
1. Expose `Q_INVOKABLE testRedactionApplyCompletePipe` that the
   `_wait_apply_completes` poll can query instead of relying on
   the completion-dialog appearance.
2. Or instrument `applyRedactionsAction_->trigger()` to set a
   QSignalSpy-compatible signal `redactionApplyCompleted(bool)`.

---

## CI-LIMITATION-NAMED-NEXTPAGE-OVERSHOOT

> **Triage 2026-09-06:** FIX - filed as GitHub #135. This entry stays as the evidence record; the issue tracks the remaining work.

**Discovered:** 2026-06-10 (pipeline f3c91a2e Phase 3 Cluster #7)
**Affected test:** `tests/ui_automation/test_follow_link_phase_post.py::
test_follow_named_nextpage_link_advances_one_page` (also surfaced
via `tests/ui_automation/test_ui_follow_links.py` re-export)

**Symptom:** Clicking the Named NextPage link at PDF coord
`(100, 617)` (fitz Y-down: `(100, 175)`) lands the canvas on
"Page 3 of 3" instead of the expected "Page 2 of 3".

**Root cause (one of):**
- The click is being routed to the wrong link (the fixture has a
  GoTo link to page 3 nearby) — coord math discrepancy between
  generate_link_types_pdf.py and the canvas's PDF-to-screen mapping
  under DPI scaling.
- The Named NextPage action handler advances by 2 instead of 1
  when invoked from a "current page is 1" state under CI.

**Test status:** RESOLVED 2026-09-08 (pipeline backlog-workable-p82
phase-17). The `@pytest.mark.xfail(strict=False)` marker is REMOVED and
`test_follow_named_nextpage_link_advances_one_page` passes live.

**ROOT CAUSE — neither of the two above. It was (c): the link hit-test was
unreachable.**

`PdfCanvas::mousePressEvent` delegates a left-click to the active tool and
`return`s; the link hit-test lives BELOW that branch, on the "no tool is
consuming the click" path. Opening a document installs the Select tool eagerly
(`MainWindow::openFile` — "install the tool that the toolbar already SAYS is
active", itself a fix for a different defect), so a tool is ALWAYS active while
a document is open and the link hit-test had become dead code. Link following
was broken in the PRODUCT, for every link, for every user — not flaky in the
test.

**How the two recorded hypotheses were eliminated, rather than argued away.**
`tests/test_follow_link.cpp::FollowLinkFixtureTest` drives the committed
`test_link_types.pdf` at the exact coordinate the live test uses:
* resolution-only probe: the point resolves to the **Named/NextPage** link,
  not to the nearby GoTo-to-page-3 — so not (a);
* dispatch probe on `followLinkAt`: lands on page index **1** — so not (b).
Both were green while the live test was red, which is what pointed at the
layer between them. Every pre-existing test in that file plants its own link
with `AnnotationManager` and then clicks the rect it just planted, so the
click coordinate and the annotation rect come from the same value; that pair
is self-consistent and could not see this.

By the time it was investigated the live observable was **"Page 1 of 3"** — no
navigation at all — not the "Page 3 of 3" recorded in 2026-06. Measured live
and reproduced in-process before anything was changed.

**Fix:** `PdfCanvas::linkFollowRequested` (a `std::function<bool(int,QPointF)>`,
same shape as the existing `tooltipRequested`), consulted before the Select
tool's press. A signal could not be used: the canvas must know whether the
receiver actually followed a link, so that a followed link is not ALSO handed
to the tool. Deliberately restricted to `SelectTool` — every other tool is an
explicit choice to create or edit at the click point, and
`ACreationToolKeepsItsClickEvenOverALink` is the control that pins it.

**Also fixed here:** `test_follow_goto_link_navigates_to_target_page` asserted
`"3" in text`, which the UNCHANGED status "Page 1 of 3" satisfies. It passed
throughout the period in which clicking a link did nothing. Both assertions in
that file now compare the whole indicator.

**Known limitation, not fixed:** with `TextEditTool` active the press is
consumed even earlier in `mousePressEvent` and links still do not follow.
Out of scope for this row.

---

## CI-LIMITATION-FUZZ-CAMPAIGN-VCPKG-CACHE

> **Triage 2026-09-06:** TOLERATE - superseded in practice; the campaign now runs on the self-hosted runner, not the free tier.

**Discovered:** 2026-06-12 (pipeline f3c91a2e Phase 5)
**Affected:** `.github/workflows/windows-fuzz.yml` — the coverage-guided
clang-cl libFuzzer campaign (7 harnesses).

**Symptom:** The fuzz workflow repeatedly fails during the vcpkg
dependency build, either with "No space left on device" (runs
27397287924, 27400513984) or a transient upstream source-download
failure (gumbo in run 27430972625, qtbase in 27397287924).

**Root cause (environmental, NOT PDFE code):** PDFE's dependency
closure is 46 vcpkg packages including Qt6, which is very large. The
x-gha binary cache backend used by all PDFE CI builds has GitHub's
**10 GB per-repo cache limit**, which cannot hold this closure. So
every build — validation, sanitizer, AND fuzz — is a binary-cache
MISS for the big packages and source-rebuilds them (~90 min). That in
turn requires downloading 46 source tarballs from various upstreams
(download.qt.io, GitHub release assets, etc.), any one of which
failing aborts the whole configure. The disk-constrained self-hosted
host (BUILDHOST, frequently <15 GB free) compounds this: a full Qt6
source build needs ~12-15 GB of transient buildtrees.

**Why the other workflows survive this:** `windows-validation.yml` and
`windows-sanitizer.yml` hit the SAME ~90 min source rebuild every run,
but they tolerate it because (a) they're scheduled when the disk is
otherwise free, and (b) they happened to catch clean upstream-download
windows. The fuzz workflow is more fragile because it ALSO builds a
second clang-cl variant.

**Code status:** All CODE-level fuzz defects were fixed in Phase 5
(GitHub-hosted→self-hosted runner; clang-cl `-dumpversion` null crash
in clang_rt detection→glob; 7-way matrix rebuild→single job;
clang-cl-invalidates-vcpkg-cache→two-stage MSVC/clang-cl build). The
infrastructure is complete and correct.

**Fix applied:** A **persistent local binary cache**
(`VCPKG_BINARY_SOURCES: ...files,C:\vcpkg-bincache,readwrite...`) was
added to windows-fuzz.yml. Unlike x-gha, a files cache has no size
limit and persists across runs on the self-hosted host. Once ONE run
completes the dep closure (e.g. a nightly schedule that catches a
clean download window), `C:\vcpkg-bincache` is populated and every
subsequent run restores from it instantly — no rebuild, no upstream
download — and the campaign goes stably green.

**Verified net in the meantime (Question 2 coverage):** the fuzz
harness entry points are ALREADY exercised under MSVC ASan via
`FontFuzzSmokeTest` (8) + `Pkcs7FuzzSmokeTest` (4) in
`windows-sanitizer.yml`, which passes with 0 findings on every
scheduled run. That lane fires on `schedule: '0 18 * * 0,2,4'`
(Sun/Tue/Thu 18:00 UTC, max 3-day gap) plus `workflow_dispatch` — it
has had **no `push` trigger since 2026-07-20**. This sentence read
"on every push" from that date until F32 corrected it.
The libFuzzer campaign adds coverage-guided mutation depth on top of
that verified baseline; it is additive, not a gap in "known attack
methods verified."

**Resolution path to fully green:**
1. Let the nightly `windows-fuzz.yml` run until it catches a clean
   46-download window and populates `C:\vcpkg-bincache` (the persistent
   cache makes the first success permanent). OR
2. Extend the same `files,C:\vcpkg-bincache` cache to
   windows-validation + windows-sanitizer so the first successful
   build of the closure (which happens regularly) seeds the cache for
   the fuzz workflow too. OR
3. Provision a runner image with the vcpkg closure pre-baked.

---

## CI-LIMITATION-TOOLBAR-OVERFLOW-COUPLES-ORACLES-TO-WINDOW-WIDTH

> **Recorded 2026-10-07, open-issue pipeline phase-2. Tracked on GitHub #284.**
> Supersedes the cause recorded for the SAME oracles in
> CI-LIMITATION-VISUAL-REGRESSION-SESSION-DRIFT below, which is RESOLVED and whose
> own cause was retracted. That makes this the **third** cause recorded for this
> set of tests; read the retraction below before adding a fourth.

**Affected:** 18 of the 42 UI failures in GitHub #277 — 11 canvas/dialog pixel
oracles (chiefly `tests/test_ui_file_operations.py`, plus
`test_ui_view_navigation.py::test_actual_size_returns_to_100pct`) and 7 lookups
that fail with `formToolActionButton not reachable`.

**The limitation.** `tests/ui_harness/core/launch.py::_resize_window` pins the
window to a **lower bound**, not a value: it asks for `WINDOW_WIDTH=1600`, and if
the achieved width is `< 1400` it escalates through 1800/2000/2400 until the bound
is met. The whole block is wrapped in `except Exception: pass`. Its own docstring
records 1158px being observed on this OS when the Python process's DPI-awareness
context differs from PDFE.exe's (GAP-28).

A different achieved width makes the toolbars **overflow differently**, which has
two consequences that look like unrelated bug families:

1. the captured region's top ~38 rows show a different slice of the toolbar, so a
   pixel oracle over it asserts toolbar scroll state as well as rendering;
2. an action pushed into a toolbar's overflow extension has no visible QToolButton,
   and `src/ui/MainWindow.cpp:3240-3246` states that the Qt6 UIA bridge resolves
   `auto_id=<action>` to that proxy only "when the toolbar is visible".

**Measured** (2026-10-07, artifacts re-measured and mtime-anchored on #284 after an
earlier set was overwritten — see CI-LIMITATION-SHARED-FAILURE-ARTIFACT-PATH):
differing pixels confined to `bbox` y=6..44 across panels 566–869 px tall, with
**nothing below**; `best_offset=(0,0)` under re-alignment over dx,dy ∈ [-3,+3], so
it is a content difference and **not** a one-pixel layout shift.

**What this means for writing tests here.** Until #284 lands, a canvas pixel oracle
in this file is **order-dependent**: activating an annotation tool scrolls the
toolbar, so the same test passes or fails depending on what ran before it.
Concretely, and this is the trap that caught the phase-1 fix:

```
11 tests selected with -k, 36 deselected  ->  11 passed
the whole file, 47 tests, no -k           ->  10 failed
```

**So do not validate a change to these oracles with a `-k` selection.** Run the
whole file. A green from a selected subset says nothing about the sweep.

## CI-LIMITATION-SHARED-FAILURE-ARTIFACT-PATH

> **Recorded 2026-10-07, open-issue pipeline phase-2. Tracked on GitHub #289.**

**The limitation.** Visual-regression failures write to
`tests/visual/_failure_artifacts/<baseline_name>_diff.png`. The filename derives
only from the baseline name, so **every run overwrites the previous run's evidence
in place**, and the directory is gitignored (`.gitignore:557`) so nothing is kept.

**Why it matters when investigating.** The normal response to a failing visual
oracle is to re-run it — and that is exactly the action that destroys the diff you
were about to read. Measured consequence: during the #277 triage, two measurement
passes reported **2773** and **3456** differing pixels for nominally the same file,
with nothing to indicate the file had changed between them. A report had already
cited the first number and described the artifact as "still on disk".

**Working rule until #289 lands:** copy a diff artifact somewhere run-specific
*before* re-running anything, and anchor any figure you quote to the file's mtime.

## CI-LIMITATION-KILL-TIMING-ORACLE-CANNOT-SEE-A-LATE-KILL

> **Recorded 2026-10-07, open-issue pipeline phase-4. Tracked on GitHub #291.**

**The limitation.** `DssMidAppendKill.KillMidAppendLeavesTheSignedDocumentByteIdentical`
kills a child process partway through a `/DSS` append and then asserts the signed
document is byte-identical. When the kill lands **after** the append has completed,
the target has legitimately grown by one revision — and the test reports that as

> *"the signature this operation existed to strengthen has been destroyed (F58)"*

There is no signal that separates the two cases. Walking what the parent has:

| signal | proves | cannot tell you |
|---|---|---|
| `childReachedWindow` | bytes of the revision hit disk *somewhere* | TARGET or staging sibling — `startAppendGrowthWatcher` fires on either — and whether the kill then landed before or after the write finished |
| `childKilled` | `TerminateProcess` returned non-zero | **nothing about timing**; equally true for a kill that landed after completion |

**This is already known to be possible, in two places in the source.**
`tests/support/SaveFailureInjection.cpp:511` explains why it polls at 1 ms rather
than 10: *"every millisecond between the child's announcement and TerminateProcess
is a millisecond in which the child can finish the write and leave nothing
injected."* And `tests/test_dss_mid_append_kill.cpp:230` says outright: *"reaching
this line at all means the kill was late."* Neither is **detected**.

**Observed.** Once, in a 16-shard local run of 6,530 tests on 2026-10-07:
`after.size()=201432608` vs `before.size()=97706`. The victim performs **one**
`/DSS` write with an oversized payload (`inflatedCerts`), so a completed append
growing 97 KB to 201 MB is expected arithmetic rather than corruption. The machine
was running a live-UI pytest suite and a 244-translation-unit clang AST sweep
concurrently; `run_gtest.py` re-ran it in a fresh process and it passed, so the run
reported CLEAN with `failures=0` in the merged XML.

**Why it matters when investigating.** A gate citing that merged XML sees clean
evidence. And the failure text names signature destruction, so the next reader
either hunts a data-loss bug that is not there, disbelieves the test and risks
waving through the real thing, or reruns until green — which the healing mechanism
now does automatically.

**Working rule until #291 lands:**

1. Do not run this test (or its `SaveFailureInjection` siblings) alongside another
   heavy job. The kill window is in milliseconds; concurrent load moves it.
2. If it fails, check `after.size()` against `before.size() + one oversized
   revision` **before** believing the message. A size consistent with a *completed*
   append is a late kill, not data loss.
3. Treat a failure that occurred during contention as unattributed until
   re-measured on a quiet machine.

**Not** a product defect on present evidence, and deliberately not recorded as one.

## CI-LIMITATION-VISUAL-REGRESSION-SESSION-DRIFT - RESOLVED (GitHub #136, pipeline p84-remaining-open phase-5, 2026-09-12)

> **Triage 2026-09-06:** FIX - filed as GitHub #136.
> **RESOLVED 2026-09-12.** All 14 rows are un-xfailed and pass, 3/3
> consecutive runs. The root cause recorded below is **WRONG** and was
> refuted by measurement before being replaced - there was never any
> "session drift". See **Resolution** at the end of this entry.

**Discovered:** 2026-06-10 (pipeline f3c91a2e Phase 3 verification)
**Affected tests:** 10 in `tests/test_ui_file_operations.py`:
- test_open_via_file_menu_picks_test_single
- test_open_via_ctrl_o_shortcut
- test_open_at_launch_via_command_line
- test_open_corrupt_xref_pdf_recovers
- test_open_scanned_pdf_loads_canvas
- test_new_file_action_creates_blank_document
- test_status_bar_present_after_open
- test_open_landscape_pdf_renders
- test_open_multisize_pdf_renders
- test_open_then_open_replaces_document

Additional tests added under Phase 3 Cluster #7 (2026-06-10):
- `tests/test_ui_redaction.py::test_apply_partial_page_leaves_unmarked_intact` (SSIM 0.31 vs 0.80)
- `tests/test_ui_redaction.py::test_apply_redactions_across_multiple_pages` (SSIM 0.76 vs 0.80)
- `tests/test_ui_view_navigation.py::test_actual_size_returns_to_100pct` (SSIM 0.13 vs 0.95)
- `tests/test_ui_view_navigation.py::test_previous_page_returns` (SSIM 0.828, threshold lowered to 0.80)

**Symptom:** SSIM 0.18 to 0.91 between baselines (captured by Phase 3
Clusters #2/#6 rebaseline) and CI runner screenshots. Threshold
0.85 catches some runs but not others; even after the threshold was
lowered, scores oscillate as low as -0.27 across runs.

**Root cause:** Screenshots captured under the user's interactive
desktop session do NOT match screenshots captured by the GitHub
Actions runner subprocess. Sub-pixel rendering, font hinting, color
profile, anti-aliasing path, and Qt6 DPI quantization differ between
sessions even on the same physical machine (BUILDHOST). This is a
fundamental Windows + Qt6 + screen-capture-from-different-session
limitation, not a PDFE rendering bug. Documents render correctly —
they just render with slightly different anti-aliasing in different
sessions.

**Workaround for users:** None needed. PDFE renders documents
correctly for actual users. Only the SSIM-based regression tests
are affected.

**Test status:** These 10 tests are marked `@pytest.mark.xfail(
strict=False)` until a session-stable visual-regression approach is
implemented (options: capture from CI runner directly via the
workflow before comparison; or switch to perceptual hashing /
content-based assertions instead of pixel-SSIM).

**Resolution path:** Phase 7 of the next pipeline (when revisiting
visual coverage) — either:
1. Add a workflow step that captures baselines from the CI runner
   on first push to a branch, downloads them, commits as the new
   baseline.
2. Replace SSIM with a content-based assertion (e.g. OCR the canvas
   and assert expected text appears).
3. Use perceptual hashing (pHash with relaxed threshold) which is
   more session-tolerant than SSIM.

### Resolution (GitHub #136, 2026-09-12) - there was no session drift

**Step 1 - the recorded cause was refuted, not argued with.** All ten
`tests/test_ui_file_operations.py` rows were run **in the user's own
interactive desktop session**, the very session this entry claims the
baselines match. **All ten still failed** (10 xfailed, 0 xpassed). So it was
never a GitHub-Actions-vs-interactive difference.

The magnitude ruled the story out by itself. For
`phase5_status_bar_after_open_canvas`:

    SSIM=0.1835 (threshold 0.85), pixel_fraction=0.4519 (threshold 0.05)

**45% of pixels differing.** Anti-aliasing variance is a fraction of one
percent. The saved diff showed the same window with the **dock panels at
different widths**, displacing the whole captured region.

**Step 2 - the actual root cause.** Two faults, both of them the repo's
"never guess canvas coordinates" rule broken for a capture region:

1. `_capture_canvas` grabbed the whole window and cropped a **FRACTION** of
   it - `x = 15% of width`, `y = 20% of height`, `70% x 65%`.
2. Those fractions only land on the canvas for one particular dock layout,
   and the dock layout is **per-user persisted state**: MainWindow's
   constructor ends with
   `restoreState(appSettings_->windowState())`. Different profile (the CI
   runner's service account is one), different machine, or anyone dragging a
   splitter, and the crop box slides onto the docks.

   `test_ui_redaction.py` / `test_ui_view_navigation.py` had a second variant
   of the same fault: they derived the crop as
   `canvas.rectangle() - session.window.rectangle()`, which is the **FRAME**
   rect where `capture_hwnd` returns the **CLIENT** area (so it was off by
   the title-bar height and the border, in the wrong pixel space), with the
   fractional crop as a silent fallback underneath.

**Step 3 - fixed at source, in this order.** Regenerating baselines first
would only have re-recorded the noise.

* **Layout pinned.** `PDFE_TEST_DETERMINISTIC_LAYOUT` (new, in
  `TestDialogSeam.h`, same `if constexpr` compile-time gate as every other
  seam and statically dead under `-DPDFE_TESTING=OFF`) makes MainWindow skip
  the `restoreGeometry`/`restoreState` pair and call
  `applyDeterministicTestLayout()` instead, pinning the left/right dock
  widths. Enabled per-module by an autouse fixture, so no other module's
  baselines are disturbed.
* **Region asked for, not guessed.** New `testCanvasRect` seam answers the
  PdfCanvas rectangle in **main-window client DEVICE pixels** - the space
  `GetClientRect`/`PrintWindow` use - computed inside the app from
  `canvas_->mapTo(this, {0,0})` and `devicePixelRatioF()`. Exposed to Python
  as `tests/helpers/canvas_geometry.canvas_region_client_px`. The fractional
  fallback is **deleted**: if the seam does not answer, the test fails.
* **Published, not queried.** The result pipe is refreshed by the app on a
  250 ms timer and Python only READS it. This is not a detail: a pipe WRITE
  goes through UIA `set_edit_text`, which moves Win32 focus to the hidden
  QLineEdit, and screenshot helpers run immediately before navigation keys
  all over the suite. Routing the capture through a write made
  `test_apply_redactions_across_multiple_pages` skip with *"Did not return to
  page 1 after PGUP"* - the PGUP went to the pipe. UIA `set_focus()` on the
  canvas did NOT undo it; only not stealing focus does.
* **Then** the 10 + 1 baselines were regenerated.

**Result: the captures are now PIXEL-IDENTICAL across separate application
launches.** Measured by running the comparison at `ssim_threshold=0.999999`
and `pixel_threshold=0.0`: they pass. SSIM 1.000000, pixel_fraction 0.000000.

**Step 4 - the thresholds had to come DOWN, and a mutant proved it.** The old
floors (SSIM 0.85, pixel 0.05) were sized for noise that no longer exists, and
a threshold sized for absent noise cannot see a real change. Mutating
`PdfCanvas::PAGE_GAP` 10 -> 11 - a **one pixel** layout shift, marker
`PDFE_MUT_F136_M4`, grepped present before the build - the mutant **SURVIVED**
at the old thresholds:

| | SSIM | pixel_fraction |
|---|---|---|
| clean | 1.0000 | 0.0000 |
| 1-px mutant | 0.8781 - 0.9212 | 0.0051 - 0.0234 |

Thresholds moved to **SSIM >= 0.99, pixel_fraction <= 0.001**, which sit in
the gap. Re-run with the mutant still applied, all three sampled rows redden,
e.g. *"Visual regression 'phase5_status_bar_after_open_canvas': SSIM=0.8781
(>= 0.99), pixel_fraction=0.0136 (<= 0.001)"*. Reverted; marker grep clean;
green again. A capture region so forgiving that nothing can redden it is the
same defect as an xfail, so this step was the point of the exercise, not a
postscript.

**Step 5 - the two things the xfail banner was hiding.**

* `test_new_file_action_creates_blank_document` was not failing on pixels at
  all. It asserted `"pdfe_untitled_" in title` - the temp filename that
  `MainWindow::onNewFile` sets `pendingDisplayName_ = tr("Untitled")`
  specifically to hide, and that
  `CliTest.NewFile_TitleShowsUntitledNotTheGeneratedTempName` asserts must NOT
  leak. The Python assertion was the exact inverse of a passing C++ contract.
  Corrected to assert `"Untitled" in title` AND `"pdfe_untitled_" not in
  title`.
* `test_previous_page_returns` had its SSIM floor lowered 0.85 -> 0.80 under
  this entry's banner, "because CI runs reproducibly land at SSIM=0.828". With
  the region fixed it scores a repeatable **0.917** - so most of what that
  floor was paying for was the bad region. The remaining gap turned out to be
  a **real, exactly reproducible 10 px scroll discrepancy** (3/3 runs), found
  only because a view-state oracle was added alongside the pixels: PGUP
  returns to page 0 at `scrollY=10` rather than `0`. That is
  `PdfCanvas::PAGE_GAP` - the layout starts page 0 at `yOffset = PAGE_GAP`, so
  `goToPage(0)` scrolls to the PAGE top while the initial view sits one gap
  above it, at the top of the SCROLL AREA. Benign, arguably more correct after
  a navigation, and now asserted explicitly with its mechanism named and the
  magnitude pinned to one page gap. The load-bearing oracle for that test is
  the view state, not the pixels; SSIM is kept at 0.90 as a coarse guard,
  above the 0.828 the bad region used to produce.

**A size mismatch is now its own failure.** `_baseline_check` in both modules
refuses to compare images of different dimensions - `compare()` silently
coerces them to a common size, which is how a moved capture region
masqueraded as a mysterious low SSIM for months.

**Runs:** all 14 rows - the 10 `phase5_*_canvas` rows, the 2 in
`test_ui_redaction.py`, and both in `test_ui_view_navigation.py` - pass 3/3
consecutive.

**One residual flake, not hidden:** `test_previous_page_returns` failed **once
in nine** runs, at `_wait_for_page_label_substr(s, "Page 2 of 100")` - the
first PGDN not registering, which is before the capture, the view-state oracle
and the SSIM compare. That is the SelectTool nav-key absorption described in
`_prime_canvas_focus_once`'s own docstring, the same family as
CI-LIMITATION-UIA-MODIFIER-CLICK-FULL-SUITE. Not reproduced in 8 further runs
(3 standalone, 2 in the same 4-test batch, plus the 3 repeat rounds), and the
failing step is unchanged by this work - but the flake is real and is recorded
here rather than rounded off. Full-file regression sweeps found no new failures: 35 rows in
`test_ui_view_navigation.py` (3 pre-existing WIN-45 reachability failures,
each verified to fail identically on the unmodified tree) and 21 in
`test_ui_redaction.py` (19 pass, 1 pre-existing unrelated skip, 1 xfail owned
by GitHub #134).

---

## PDFE-LIMITATION-SAVEAS-LARGE-XFA — RESOLVED (pipeline b2e47d91, 2026-06-21)

**Discovered:** 2026-05-29 (real-world corpus testing)
**Re-evaluated:** 2026-06-06 — empty-output bug no longer reproduces;
limitation downgraded from correctness to performance.
**Release-profiled:** 2026-06-12 (pipeline f3c91a2e Phase 6) — Release is
~2x faster than Debug but still minutes per file; NOT a Debug artifact.
**RESOLVED:** 2026-06-21 (pipeline b2e47d91) — root cause confirmed as
`do_garbage=3` and fixed in two tiers; interactive saves of these forms now
take sub-second to ~2.6 s. See **Resolution** below.

---

### Resolution (pipeline b2e47d91)

**Tier 0 — benchmark (`tests/test_xfa_save_benchmark.cpp`,
`tests/docs/xfa_save_benchmark.md`).** Profiled `pdf_save_document` at every
`do_garbage` level on Release, with a round-trip (re-open, pageCount>0) per cell:

| fixture | gc=0 ms | gc=1 ms | gc=2 ms | gc=3 ms | rt (all levels) |
|---|---:|---:|---:|---:|---|
| ssa_sf86 | 2676 | 2100 | 2278 | 317044 | PASS |
| irs_p17 | 1517 | 1089 | 1086 | 686470 | PASS |
| irs_i1040 | 1223 | 1009 | 1173 | 991916 | PASS |

`do_garbage=3` is the entire cost (150–980× slower than gc=1) and gc=1
round-trips cleanly on all three. **DECISION: (a)** — lower GC is viable.

> **Not only on XFA (gap `PERF-01`, follow-up phase F40, 2026-08-07).** The
> sentence above is right, and it is broader than the fixtures it was measured
> on. The same `do_garbage=3` sweep is the entire super-linear term on
> **ordinary, non-XFA pages** too: a `reorderPages` edit followed by a gc=3
> full-rewrite save measures a log-log exponent of **2.08 over 500 -> 2500
> pages** (Debug, best-of-5), against **1.01 -- linear -- for the identical edit
> at gc=2, which is 37x faster and produces a SHA-256-identical file** (verified
> at 500 pages against a duplicate-carrying control on which gc=3 correctly
> differs). Tier 1 and Tier 2
> below changed *when* gc=3 is selected, not what it costs when it is; the
> operations that still escalate to it still pay the quadratic term. Disclosed,
> not fixed -- no production change was made, because skipping dedup is not free:
> on a document that really does carry duplicate objects it costs **+31.5%
> output bytes**. Full measurements, four measured options with their
> time-vs-output-size trade-offs, and the recommendation:
> `docs/audit/meta-audit-2026-07/followup/phase-40/README.md`.

**Tier 1 — incremental-save routing (commit `95ca259`).** Content-only sessions
(form-field fills, annotations) saving back to the original file now route
through the incremental-append path (no GC) instead of a full rewrite. The
common XFA workflow — open, fill a field, Save — dropped from **317 s → 0.33 s
(~967×)** on ssa_sf86. Destructive/structural ops still full-rewrite so removed
bytes are scrubbed. Test: `tests/test_incremental_save_routing.cpp`.

**Tier 2 — edit-aware conditional GC (commit `768af8a`).** Full-rewrite saves
(`save` to a new file, `saveAs`, conversions, extract) now default to
`do_garbage=1` (which still excludes unreferenced objects — verified against
MuPDF 1.26.10 `pdf-write.c` — so confidentiality is preserved), escalating to
`do_garbage=3` only for dedup-worthy ops (redaction, page delete/import,
flatten, font embed, encryption). **"Save As" of ssa_sf86 with no structural op:
317 s → 2.3 s (~136×).** Test: `tests/test_conditional_gc.cpp`.

> **Tier 3 — a pure page PERMUTATION left that list (GitHub #183, pipeline
> p85-remaining-open phase-5, 2026-09-15).** "page delete/reorder/import" above
> read as one family and is two. A reorder re-orders one `/Kids` array: it
> creates no object and duplicates none, so `removeduplicateobjs` has nothing to
> find and the document paid an O(objects²) scan for it. A **full permutation**
> (same page set, new order, no legacy catalog `/Dests`) now marks
> requires-full-rewrite without needs-full-GC; page **delete** and **import** are
> unchanged and still escalate. Pinned by
> `ConditionalGcTest.PureReorderIsFullRewriteButNotFullGc` against
> `PageDeleteNeedsFullGc` as its control.
>
> The same phase also removed the *other* super-linear term on this path — the
> one the F40 note above correctly says Tier 1 and Tier 2 did not touch — by
> not provoking MuPDF's page-map invalidation once per page. Measured
> reorder+save at 10x the pages: **~50x → 6.3–8.5x**; fitted exponents
> reorder 1.86–1.92 → 0.98–1.11 and save 1.58–1.62 → 0.74–0.88.
> Evidence: `docs/audit/p85-remaining-open/phase-5/benchmarks.json`.
>
> **The trade-off F40 names is still real and is now taken on this path:** a
> reorder+save of a document that *already* carried duplicate objects no longer
> de-duplicates them, so its output can be larger than it was (F40 measured
> +31.5% on a duplicate-carrying control). That is the deliberate position —
> de-duplication is what the Optimize path is for, and a user reordering pages
> did not ask for it. A document that carries no duplicates is byte-identical
> either way.

**Round-trip impact:** the three `ExternalRealWorldTest.SaveRoundTrip` fixtures
now complete at gc=1 in 1.1 s / 1.1 s / 2.6 s (irs_i1040 / irs_p17 / ssa_sf86) —
down from 992 s / 686 s / 317 s. They now run as normal regression coverage on
Release (the `PDFE_RUN_SLOW_SAVE_TESTS` gate is kept for Debug only, where MuPDF
is materially slower). Remaining `PDFE_RUN_SLOW_SAVE_TESTS`-gated tests are the
3 dedicated timing/benchmark tests (`XfaSaveBenchmark.MeasureGcLevels`,
`IncrementalSaveRoutingTest.FormFillSaveIsFastOnXfa`,
`ConditionalGcTest.SaveAsFastOnXfaWithoutStructuralOp`).

The original profiling notes are retained below for the record.

---

**Affected PDFs:**
- `ssa_sf86.pdf` (8 MB, XFA-heavy government form)
- `irs_i1040.pdf` (4 MB, 126-page IRS instructions)
- `irs_p17.pdf` (3 MB, 142-page IRS Publication 17)

**Original symptom (2026-05-29):** `Document::saveAs(outPath)` succeeded
without exception but produced an empty or near-empty output file.
Reopening threw `"Not a PDF file (magic bytes %PDF- missing)"`.

**Current symptom (2026-06-12, Release profiled):** `saveAs` produces
valid output for all three files; the round-trip test (re-open + read
pages) passes. The remaining issue is **runtime**, and Phase 6 of
pipeline f3c91a2e profiled it on a **Release** build with the current
options (`do_garbage = 3, do_compress = 1, do_compress_images = 1,
do_compress_fonts = 1`):

| File | Debug (prior) | **Release (2026-06-12)** | Round-trip |
|------|---------------|--------------------------|------------|
| ssa_sf86.pdf  | ~7.4 min | **5.24 min** (314,536 ms) | PASS |
| irs_p17.pdf   | ~25 min  | **12.2 min** (733,721 ms) | PASS |
| irs_i1040.pdf | ~38 min  | **17.2 min** (1,030,384 ms) | PASS |

Profiled via `PDFE_RUN_SLOW_SAVE_TESTS=1` against
`build/vs2022/bin/Release/pdfe_tests.exe`
(`*SaveRoundTrip/{ssa_sf86,irs_p17,irs_i1040}`), all three PASS.

**Conclusion — NOT downgraded to Debug-only.** Release is roughly ~2x
faster than Debug but is still **minutes, not the <30 sec the PyMuPDF
cross-check predicted**. The "slow path is a Debug artifact" hypothesis
is therefore **refuted**: this is a real product performance limitation
on large/XFA-heavy forms, present in optimized builds too. The skip in
`tests/test_realworld_external_corpus.cpp` stays (the tests would add
~35 min even on Release), gated by `PDFE_RUN_SLOW_SAVE_TESTS=1`.

**Root-cause investigation (started Phase 6):** Since Release is also
slow, the `MuPdfContext::execute` fz_try/fz_catch marshalling-overhead
hypothesis is **ruled out** as the dominant factor — that wrapper is
entered exactly once per save, so it cannot account for minutes. The
time is spent **inside** the single `pdf_save_document` call. The most
likely driver is the **option set**, specifically `do_garbage = 3`
(GC level 3 = full object dedup/merge, the most expensive MuPDF GC
pass; it walks and hash-compares the entire object graph, which is
super-linear on the deep XFA/AcroForm object trees in these files).
PyMuPDF's default `Document.save()` uses `garbage=0` unless explicitly
raised — so the earlier "<30 sec with the same option set" cross-check
almost certainly did **not** actually use `garbage=3`. The compression
flags (`do_compress*`) add a second pass but are linear.

**Resolution path (next pipeline — product perf work, out of Phase 6
scope which was to profile + decide):**
1. Benchmark `pdf_save_document` on these 3 files with `do_garbage`
   stepped 0→1→2→3 (and with `do_compress*` toggled) to attribute the
   cost. Hypothesis: `garbage=3` dominates.
2. If confirmed, expose a save-options policy: default interactive
   "Save"/"Save As" to a cheaper GC level (e.g. `garbage=1` compact, or
   `garbage=2`), reserving `garbage=3` for an explicit
   "Optimize / Reduce File Size" action with a progress dialog.
3. Re-profile to confirm the interactive save of these fixtures drops
   to seconds.

**gtest skip-count reconciliation (Phase 7 sweep, 2026-06-13):** these
three `ExternalCorpus/ExternalRealWorldTest.SaveRoundTrip/{ssa_sf86,
irs_p17,irs_i1040}` skips are the **only** gtest skips remaining on CI
(verified on run 27461377313). They are three parametrized instances of
this single documented limitation. The Phase 7 acceptance criterion's
phrasing "at most the known 2 skips" predates both (a) the SAVEAS-XFA
skip becoming three parametrized instances and (b) the Phase 6 DLL test
`DllDependencyTest.TlsBackendPluginPresent`, which used to add a 4th skip
when `bin/tls/` was absent — now eliminated (the Phase 7 TLS-plugin
deploy fix makes it PASS, see CI-LIMITATION-DLL-DEPENDENCY-CHECKS /
GAP-A14 below). So the accurate known-skip set is exactly these 3, all
one documented limitation, satisfying the criterion's intent.

---

## PDFE-LIMITATION-UNICODE-PATH — RESOLVED (2026-06-12)

**Discovered:** 2026-05-29 (security audit)
**Resolved:** 2026-06-12 (pipeline f3c91a2e Phase 6)

**Original symptom:** Opening (or saving) a PDF whose path contains
non-ASCII characters (CJK, Cyrillic, Arabic, even Latin-1 extended like
`café`) threw `std::system_error` ("No mapping for the Unicode character
exists in the target multi-byte code page") on Windows.

**Root cause:** Two narrow-string conversions on the wide Windows path:
1. `std::filesystem::path::string()` converts the native wide path to the
   system **ANSI code page**, silently dropping any character not in that
   code page. PDFE used `.string()` to feed MuPDF's
   `fz_open_document(ctx, char*)` / `pdf_save_document(ctx, ..., char*)`
   and to build the UNC-check argument + exception messages.
2. MuPDF on Windows internally re-widens its `char*` path argument from
   **UTF-8** (`fz_utf8_to_wchar`) before calling the Win32 file API — so
   it must be handed UTF-8, not ANSI.

**Fix (in `src/core/Document.cpp`):**
- Added `mupdfPathUtf8(path)` = `path.u8string()` reinterpreted to
  `std::string` (lossless UTF-8; byte-identical to `.string()` on POSIX).
- Replaced every narrow path fed to MuPDF with `mupdfPathUtf8(...)`:
  the open constructor, `createBlank`, `save` (temp + reopen paths),
  `saveAs`, and `saveIncremental`.
- In `Document::open`, the UNC/remote pre-check now uses the wide
  overload (`path_security::is_unc_or_remote(filePath.wstring())`) on
  Windows, and exception messages use `mupdfPathUtf8(filePath)`, so a
  CJK path no longer throws from the boundary check before reaching
  MuPDF. (`std::filesystem::exists` and the `_looks_like_pdf` `ifstream`
  already take the wide path overload and were Unicode-safe.)

**Test status:** `tests/test_security_audit.cpp`
`UnicodeFilenameDetectsLimitation` (CJK/Cyrillic) and
`UnicodeFilenameLatin1HandledCleanly` (café/résumé + a saveAs→reopen
round-trip through a Unicode path) now **assert the fix**: the documents
open and round-trip correctly. Both pass on the Debug build (verified
2026-06-12).

**User impact:** Unicode file paths now open and save correctly. No
workaround needed.

---

## PDFE-LIMITATION-WINAPPDRIVER-V0 — DOCUMENTED (2026-06-12)

> **Triage 2026-09-06:** TOLERATE - resolved by documenting a 1.2.1 minimum; this is user-environment guidance working as intended.

**Discovered:** Earlier session
**Documented:** 2026-06-12 (pipeline f3c91a2e Phase 6)

**Symptom:** WinAppDriver v0 (Beta from 2016) cannot create sessions —
returns `/status OK` but `POST /session` times out, so a stale v0 install
looks "reachable" yet fails every WAD test with an opaque timeout.

**Workaround for users:** Install WinAppDriver 1.2.1 or later from
https://github.com/microsoft/WinAppDriver/releases — this is now the
documented minimum supported version.

**Resolution (Phase 6):**
1. **Setup guide:** `tests/docs/winappdriver_setup.md` documents the
   1.2.1 minimum, install steps, Developer-Mode requirement, how to run
   the service + WAD tests, and a troubleshooting table.
2. **Version floor enforced in the harness:**
   `tests/ui_automation/helpers/winappdriver_dialog_driver.py` adds
   `MIN_WINAPPDRIVER_VERSION = (1, 2, 1)`, `get_winappdriver_version()`
   (parses `GET /status`), and `require_winappdriver_version()` which
   `pytest.skip`s with a clear PDFE-LIMITATION-WINAPPDRIVER-V0 message
   when the service is unreachable or below 1.2.1.
3. **WAD tests annotated:** `test_winappdriver_smoke.py` calls
   `require_winappdriver_version()` in both tests;
   `test_ui_file_dialog_flows_wad.py` and `test_ui_encryption_flows_wad.py`
   route their `_skip_if_service_unavailable` / `_skip_if_unavailable`
   helpers through it.
4. **CI verifies version before WAD tests:** `windows-validation.yml`
   has a "Verify WinAppDriver version" step that records the installed
   version (or its absence) in the step summary before the UI suite
   runs, flagging any install below 1.2.1.

**Test status:** WAD tests remain opt-in — they `skip` (not fail) when
the service is absent or below 1.2.1, keeping the suite green on runners
without WinAppDriver.

---

## CI-LIMITATION-CRT-MODAL-DIALOG-HANG — RESOLVED (bandaid verified hardened, 2026-06-12)

**Memory key:** `crt_modal_dialogs_hang_ci`
**Discovered:** earlier session (MSVC CRT heap-corruption dialog blocked
CI test runs indefinitely)
**Verified:** 2026-06-12 (pipeline f3c91a2e Phase 6)

**Symptom:** On a Debug build, MSVC's debug CRT can raise a
heap-corruption / `_CRT_ASSERT` report. By default the CRT presents a
**modal Abort/Retry/Ignore window** (`_CRTDBG_MODE_WNDW`), which blocks a
non-interactive CI host forever (no human to click it).

**Resolution — the `installCrashGuards()` bandaid in
`tests/test_main.cpp` is confirmed robust.** It is installed as the very
first action in `main()` (before any allocation) and does ALL of the
following, which together guarantee a crash aborts cleanly instead of
hanging:

1. `SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX |
   SEM_NOOPENFILEERRORBOX)` — suppresses the OS-level "application has
   stopped working" / WER dialog.
2. For `_CRT_WARN`, `_CRT_ERROR`, `_CRT_ASSERT`:
   `_CrtSetReportMode(m, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG)` +
   `_CrtSetReportFile(m, _CRTDBG_FILE_STDERR)`. Crucially it does **NOT**
   set `_CRTDBG_MODE_WNDW`, so the modal report window is never created —
   the message goes to stderr instead. This is the core fix.
3. `_CrtSetReportHook(crt_report_hook)` — the hook prints `[CRT] <msg>`
   to stderr, flushes, then calls `std::abort()`, converting any CRT
   report into a clean non-zero process exit that CI scores as a test
   failure (not a hang).
4. `_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT)` —
   ensures even the `abort()` itself does not pop the "abort has been
   called" dialog.

**Deliberate non-change:** the periodic alloc-time heap check
(`_CRTDBG_CHECK_EVERY_1024_DF`) is intentionally left unset because it
surfaces pre-existing benign past-writes in third-party code (e.g.
Tesseract's language-loading failure path). `_CRTDBG_ALLOC_MEM_DF`
remains on, so corruption affecting the allocator is still caught on the
next alloc/free — and now aborts via the hook instead of hanging.

**Evidence:** every Debug gtest run in this pipeline (including the
3000+-test windows-validation suite) completes to a final summary with no
operator interaction; the CRT-crash-masks-summary failure mode that
originally hid P-GAP-90 no longer occurs. No code change was required —
the existing guards already implement the no-modal + abort-cleanly
contract; this entry records the verification.

---

## CI-LIMITATION-DLL-DEPENDENCY-CHECKS — RESOLVED (2026-06-12)

**Memory key:** `dll_dependency_checks`
**Discovered:** earlier session (user saw an opaque missing-DLL error
dialog / `0xC0000135 STATUS_DLL_NOT_FOUND`)
**Resolved:** 2026-06-12 (pipeline f3c91a2e Phase 6)

**Goal:** A test must verify all required DLLs are present and loadable so
a missing one fails with a clear gtest error rather than an opaque
`0xC0000135` modal at end-user startup.

**Resolution:** `tests/test_dll_dependencies.cpp` (`DllDependencyTest`)
already covered the Qt6 core/gui/widgets/printsupport DLLs, QPDF,
Tesseract, HarfBuzz, FreeType, SQLite, zlib, ICU (data/i18n/unicode),
ONNX Runtime, OpenSSL crypto, pugixml, and the Qt platform plugin. Phase
6 **closed the runtime-only gaps** — DLLs that PDFE.exe loads at runtime
but the test binary's link did not always pull in (so they were absent
from the original set even though a missing one would hit the end user):

- `OpenSslTlsPresent` (libssl-3 — HTTPS / TSA)
- `LibcurlPresent` (libcurl — RFC-3161 timestamping network round-trip)
- `LeptonicaPresent` (Tesseract image backend — OCR/TRE)
- `JpegPresent` (jpeg62 — MuPDF JPEG decode)
- `OpenJpeg2000Present` (openjp2 — JPEG2000 decode)
- `TlsBackendPluginPresent` (Qt `tls/qopensslbackend` plugin — HTTPS;
  skips gracefully on packaging variants without a `tls/` dir)

All deployed by the same `install()` list in `CMakeLists.txt`
(`_required_dlls` / `_special_release`), so the test binary's `bin/` dir
contains them and the checks are meaningful. The test still `GTEST_SKIP`s
on non-Windows.

---

## P-GAP-90 — RESOLVED (fixed by a real code change in f7c2a4e8 phase-2, 2026-06-12)

<!-- Heading corrected 2026-09-16 (p86-followups phase-6, known_issues.md vs
     KNOWN_WEAK_TESTS.txt consistency cross-check). It previously read
     "pre-existing gtest fail no longer reproduces", which frames a real fix as
     a spontaneous disappearance -- the "it went away on its own" conclusion
     this project has been bitten by before. The body below was already correct
     and names the actual fix (the CommentPanel QAction::triggered wiring), but
     the heading is what a skimmer reads. No status disagreement existed between
     the two registries; only this framing did. -->


**Memory key:** `gtest_baseline_correction`
**Discovered:** a pre-existing C++ failure
(`Phase4AnnotationsTest.CommentPanelDeleteSignalTriggersDeleteCommand`,
`tests/test_annotations_phase4.cpp:467`, `EXPECT_TRUE(undo_->canUndo())`)
was masked in an old baseline because a CRT crash hid the gtest summary.

**Status (2026-06-12, pipeline f3c91a2e Phase 6): RESOLVED — the test
PASSES.** It was fixed in pipeline f7c2a4e8 phase-2: `CommentPanel.cpp`
(~line 97) wires `QAction::triggered` so the CommentPanel delete signal
routes through `UndoManager`, making the delete undoable. Re-ran the
exact test on the current Debug build:
`Phase4AnnotationsTest.CommentPanelDeleteSignalTriggersDeleteCommand
... OK`. The CRT-crash masking is itself closed by
CI-LIMITATION-CRT-MODAL-DIALOG-HANG above, so the summary that hid it now
always prints. No skip is needed; the baseline (3914 PASSED / 0 FAILED in
the latest windows-validation runs) is accurate.

> **Correction (2026-06-13, GAP-A15):** the claim above that "the summary
> always prints" and "the baseline is accurate" was *itself* wrong. There
> is a SECOND masking mechanism independent of the CRT dialog: on run
> 27446597142, `pdfe_tests.exe` exited 0 with 2 `BatchDialogTest` failures
> present in `gtest-results.xml`, and the end-of-run `[ FAILED ] N tests`
> recap was absent (teardown was disrupted *after* the PASSED/SKIPPED
> lines). The CRT fix does not cover this path. See GAP-A15 — the CI step
> now parses the JUnit XML and fails on `failures>0` regardless of exit
> code, which is the real backstop.

---

## GAP-A15 — RESOLVED (BatchDialog a11y-name regression + CI false-green, 2026-06-13)

**Discovered (pipeline f3c91a2e Phase 6 test-runner gate):** two genuine
gtest failures —
`BatchDialogTest.ActionListWidgetExists` (`tests/test_batch.cpp:210`) and
`BatchDialogTest.ErrorLogExists` (`tests/test_batch.cpp:252`) — had been
green on CI since pipeline f3c91a2e Phase 3 (commit `e76c88d`,
Cluster #1B), masked by a CI exit-code bug. Arithmetic gave it away:
the validation run reported `3926 ran`, `3920 PASSED`, `4 SKIPPED`
(= 3924) — the missing 2 were failures the brief-mode summary did not
recap, yet `gtest-results.xml` carried `failures="2"`.

**Two distinct bugs, both fixed:**

1. **Product/test regression (the failures).** Cluster #1B changed two
   `BatchDialog` widgets' `accessibleName` from human-readable
   (`"Action Sequence List"`, `"Batch Error Log"`) to match their
   `objectName` (`"actionListWidget"`, `"batchErrorLog"`) so pywinauto
   could locate them. That broke the `test_batch.cpp` assertions AND
   degraded screen-reader output (CLAUDE.md: `accessibleName` is the
   human-readable UIA Name). Root cause of the *testability* problem:
   Qt's UIA bridge exposes most widgets' AutomationId from `objectName`,
   but `QListWidget`/`QTextEdit` expose it from the accessible Name.
   **Fix:** `src/ui/BatchDialog.cpp` restores the human-readable
   accessible Names; the pywinauto tests locate those two widgets by
   their accessible Name (with `objectName` as a fallback); the shared
   `find_by_suffix` helper now matches the UIA Name in addition to
   AutomationId so either property works.

2. **CI false-green — two layers.**
   - *Layer 1 (step exit code).* The "Run pdfe_tests" step trusted only
     the process exit code, which was 0 despite the 2 failures.
     **Fix:** both `windows-validation.yml` and `windows-sanitizer.yml`
     now parse the JUnit XML (`testsuites/@failures`, `@errors`) after the
     run and fail the step on any failure/error regardless of exit code,
     logging the failing `classname.name`s.
   - *Layer 2 (job-level `continue-on-error`).* Even with Layer 1 fixed,
     the gtest step carried `continue-on-error: true` (added in Phase 2-3
     to tolerate the then-flaky `ZoomComboExists` so the UI suite would
     still run). That swallowed the step's exit-1 at the JOB level — the
     real reason the false-green was so durable. **Fix:** removed the flag
     from the gtest step (`ZoomComboExists` verified stable across runs
     27310588193 / 27335387425 / 27367556829 / 27446597142 — 0 failure
     children each). The diagnostic WinAppDriver-version step and the
     (Phase-7-scoped, GAP-A14) UI-suite step keep their own
     `continue-on-error`; the gtest gate no longer does.

**Status: RESOLVED.** gtest assertions expect human-readable names again;
the XML-parse backstop plus removal of the job-level `continue-on-error`
mean a failing gtest now fails the job, whatever the exit code or teardown
state. Backstop logic verified offline against run 27446597142's actual
`gtest-results.xml` (`failures=2` → would `exit 1`, names both tests).
**End-to-end confirmed on CI run 27461377313 (sha a173a60, full ~9.7h
run):** `tests=3926 failures=0 errors=0`, all 20 BatchDialogTest cases
pass, and the "Run pdfe_tests" step succeeded with `continue-on-error`
removed and the XML backstop active — i.e. the job-level gate is now real
and the suite is genuinely green.

---

## CI-LIMITATION-SHARED-PYTEST-BASETEMP

> **Triage 2026-09-26: RESOLVED** — **GitHub #257** is fixed (pipeline `p93`
> phase-5). The entry is kept because the SIGNATURE is worth recognising if it
> ever recurs, but the limitation itself is gone: each session now gets its own
> basetemp. The workaround below ("do not start a second pytest session") no
> longer applies — see **What changed** at the end.
>
> Everything between here and that section describes the behaviour **as it was**
> and is kept as the measurement of record. One exception, stated because
> "unedited" would otherwise be untrue: the old **Resolution path (not
> implemented)** paragraph was replaced by the **What changed** section, since
> leaving a proposal standing next to its own implementation is the stale-prose
> problem this correction exists to fix.

**Discovered:** 2026-09-25 (pipeline `p92-guard-reachability`, twice during one
run).

**Symptom:** A test fails with a `FileNotFoundError` on a path inside its own
`tmp_path`, or a batched pytest step reports a burst of unrelated failures and
errors together. Re-running passes. The named test is almost never the problem.

Observed instance:

```
FAILED tools/gate/test_mutation_kill_floor.py::test_json_report_records_rate_and_floor
FileNotFoundError: [Errno 2] No such file or directory:
  'tests\_tmp\test_json_report_records_rate_0\report.json'
```

**Root cause — proven, not hypothesised.** `pytest.ini` pins every session to
one shared basetemp:

```
addopts = --basetemp=tests/_tmp -p no:capture
```

When `--basetemp` is set explicitly, pytest **deletes that directory tree at
session start** rather than rotating numbered directories. It is shared, so a
second session destroys the first session's live `tmp_path` directories
mid-test. Demonstrated directly:

```
$ echo sentinel > tests/_tmp/ZZZ_probe_marker.txt
$ python -m pytest tools/gate/test_rebuild_assert.py -q      # unrelated session
7 passed in 0.75s
$ ls tests/_tmp/ZZZ_probe_marker.txt
ls: cannot access ...: No such file or directory      <-- destroyed
```

**Two ways it fires:**

1. **Concurrent sessions in one checkout** — routine when several agents or
   terminals run pytest during one piece of work, and possible in CI if two
   jobs ever share a workspace.
2. **A nested session inside the suite** —
   `tools/gate/test_desktop_outage_hook_end_to_end.py:79` deliberately spawns a
   real pytest subprocess (its own docstring says so). That child inherits
   `pytest.ini`, so it wipes the parent's basetemp.

**Why it matters beyond noise.** The failure presents as an unrelated test
breaking for no stated reason, and a re-run makes it green — which is exactly
the shape that launders a real defect. It also means gate evidence gathered
under contention is untrustworthy in *either* direction: a green from a
contended run says nothing, and a red may name an innocent test.

**How to work around it today:** do not start a second pytest session in this
checkout while one is running. If a multi-failure-plus-errors burst appears in a
batched step, treat it as this signature, re-run that batch alone, and say which
result you are treating as authoritative and why — do not silently relabel it a
flake.

**Not a defect in:** `mutation_kill.py` or `test_mutation_kill_floor.py`. That
test is a correct victim.

### What changed — 2026-09-26 (GitHub #257)

The resolution path this entry used to propose is what was implemented, in its
own gated change (pipeline `p93` phase-5) as that paragraph asked for. #257 is
closed on the commit carrying this change, not before it.

* `pytest.ini` no longer sets `--basetemp` at all. It now reads
  `addopts = -p no:capture`.
* A **repo-root `conftest.py`** roots each session at `tests/_tmp/s<pid>`, with
  a `-<n>` suffix because Windows recycles pids promptly and a crashed session
  leaves its directory behind. An explicit `--basetemp` on the command line
  still wins.
* It stays UNDER `tests/_tmp/` deliberately: the janitor sweeps that root by
  age, and four guards (`check_magnitude_guard`,
  `check_reachability_skip_shape`, `check_packaging_testing_flag`,
  `check_rtm_live_ui_citations`) exclude that prefix when scanning the tree.
  Moving it elsewhere would have silently re-admitted thousands of copied files
  into those scans.

**The hook is at the REPO ROOT, not in `tests/conftest.py`.** That was the first
attempt and it was wrong: `tests/conftest.py` covers `tests/` only, so the gate
suites under `tools/gate/` fell back to pytest's default basetemp in `%TEMP%`,
outside the repository. Ten tests in `tools/gate/test_check_crl_chain_fixture.py`
errored on the next run, because its `in_repo_copy` fixture asserts its scratch
is inside the repo — an assertion whose docstring says it exists for exactly
this. It did its job on the first run.

**Verified:** three concurrent sessions across both trees exit 0 with three
distinct basetemps; the nested-pytest case
(`test_desktop_outage_hook_end_to_end.py`) passes; a 941-test pytest run is
green. The destruction itself was reproduced deterministically before the fix —
an artifact staged in a directory, a second session pointed at the same explicit
basetemp, and the artifact gone.

**What holds this entry to reality.** A doc saying RESOLVED is not a mechanism,
and the phase-5 claim-check gate said so: the fix could be undone by one line in
`pytest.ini` — `addopts` populates `config.option.basetemp` before
`pytest_configure` runs, so the root hook early-returns and the shared directory
returns silently — with nothing going red and this entry still confidently
reading RESOLVED. `tests/scripts/test_basetemp_isolation.py` now fails if
`pytest.ini` regrows a `--basetemp`, if the repo-root `conftest.py` is deleted,
or if two sessions resolve the same directory. Both undo paths are
mutation-verified. If those tests are red, treat this entry as OPEN again
regardless of what the banner says.

**Still true, and worth keeping:** the *signature* described above. A burst of
unrelated failures and errors in one batched step, green on re-run, is the shape
that launders a real defect. If it reappears, suspect something sharing state
between sessions rather than a flake.

---

## CI-LIMITATION-GITHUB-FREE-TIER

> **Triage 2026-09-06:** TOLERATE - environmental ceiling, superseded by the self-hosted Windows runner.

**Discovered:** 2026-05-31 (CI workflow validation)
**Symptom:** Both `.github/workflows/sanitizer.yml` and
`.github/workflows/fuzz.yml` consistently fail at the `cmake
Configure` step at exactly ~2 hours of execution with zero captured
log output. Every matrix job and every sanitizer run hits the same
wall regardless of the parallelism caps applied.

**Configurations tried, all failed:**
- Default ninja/cmake parallelism (4 vCPU on ubuntu-22.04 runner)
- `VCPKG_MAX_CONCURRENCY=2` + `MAKEFLAGS=-j2`
- + `CMAKE_BUILD_PARALLEL_LEVEL=2` + `NINJAFLAGS=-j2`
- + `max-parallel: 1` on the fuzz matrix
- + `VCPKG_BINARY_SOURCES='clear;x-gha,readwrite'` + GHA cache env
- `timeout-minutes` bumped to 180–240 (jobs die before this fires)

**Root cause (hypothesized):**
- Free-tier ubuntu-22.04 runner: 4 vCPU, 7 GB RAM, 14 GB SSD
- vcpkg builds qtbase + mupdf + leptonica + tesseract + onnxruntime
  from source on every cold-cache invocation
- qtbase alone needs ~10 GB buildtree disk + ~4-5 GB peak compile
  memory at default parallelism
- The 2-hour silent death without captured output is consistent
  with the runner being evicted, swap-thrashed to unresponsiveness,
  or the actions agent losing heartbeat — not a `timeout-minutes`
  hit (which would log a clean "timeout exceeded" message)

**Resolution path:** Move sanitizer/fuzz to a self-hosted runner
with adequate resources. The Hetzner workflow
(`.github/workflows/hetzner-sanitizer.yml`) and provisioning guide
(`tests/docs/hetzner_runner_setup.md`) are ready; recommended
CX41 server (4 vCPU, 16 GB RAM, 160 GB SSD, ~€16/month).

**Status of workflow files:** Kept in repo as reference — they
correctly express the build intent. The free-tier failure is
infrastructure-driven, not workflow-config-driven. Once a
self-hosted runner is registered, the same workflows succeed
unchanged. Hetzner workflow's schedule trigger is commented out
until a runner is online.

---

## CI-LIMITATION-PDFE-IS-WINDOWS-ONLY

> **Triage 2026-09-06:** TOLERATE - deliberate platform scope; the Linux/macOS lanes are aspirational by design and their red is expected.

**Discovered:** 2026-06-01 (Hetzner Linux runner end-to-end test)
**Symptom:** PDFE source compiles on Windows (where it's developed
+ tested) but does NOT compile on Linux. Attempting to build on
Ubuntu 22.04 (clang-15 + Qt 6.10 + libstdc++) fails with:

  src/core/Document.cpp:774:29: error: no viable conversion from
  'const basic_string<std::filesystem::path::value_type>' to
  'const basic_string<wchar_t>'
      const std::wstring& w = path.native();

The code assumes `std::filesystem::path::value_type == wchar_t`
which is true only on Windows. On Linux it's `char`. Similar
Windows-specific assumptions are scattered throughout core code
(alternate data stream checks, MAX_PATH constants, etc.).

> **THE CITED LINE ABOVE NO LONGER EXISTS (re-measured 2026-10-09, p95
> phase-13). The disposition is unchanged — only the citation is dead.**
>
> `src/core/Document.cpp` no longer contains any `const std::wstring&` binding
> of `path.native()`. The single remaining call reads
> `const auto& native_path = path.native();`, which deduces correctly on both
> platforms. Whether that was a deliberate fix or an incidental refactor is not
> established here.
>
> **Do not re-cite a line number for it.** `docs/audit/never-run-audits-2026-09/a-tsan.md`
> already caught this on 2026-09-06 and recorded the replacement as `:3146`;
> that had itself drifted to `:3997` within a month. Cite the *form*, not the
> number — this entry is live operational state and a line citation in it decays
> on its own.
>
> **TOLERATE still stands, and the lane is still blocked.** The same search
> (`std::wstring` | `<windows.h>` | `_WIN32` over `src/`) that `a-tsan.md`
> measured at **28 files** now returns **32** — the Windows-only surface has
> *grown*. One of the additions is `src/app/PowerEvents.cpp`, added
> **2026-09-18** (`93231aac`), i.e. after that audit. So the port is further away
> than when this entry was written, not closer. Nothing here should be read as
> progress toward a Linux build.
>
> (First draft of this note credited that file to p95 phase-11. Wrong, and
> checked only afterwards: phase-11 *established that power-event handling
> already existed* and that GitHub #213 was stale — it did not add the file,
> which predates this pipeline by three weeks. The other three additions were
> not individually identified.)

**Status of "cross-platform" claim:** Aspirational. MEMORY.md
describes PDFE as "Cross-platform (Win/Linux/macOS) PDF editor"
but the code has never been ported. CLAUDE.md correctly identifies
PDFE as "Native Windows desktop application: C++20 + Qt 6.10 PDF
editor" — that is the actual state.

**Implication for CI:** Linux-based sanitizer/fuzz CI (the
sanitizer.yml, fuzz.yml, and hetzner-sanitizer.yml workflows in
this repo) cannot build the project as-is. They all configured
correctly with Hetzner CX42 runners (16 GB RAM, Qt6 vcpkg cold
build, all 59 ports, 62 min) but failed at PDFE's own source
compile because of the Windows-only code paths.

**Infrastructure that DOES work (proven across 10 ephemeral
attempts):**
- `tests/scripts/hetzner_ephemeral_runner.ps1` — provisions
  Hetzner cpx42, registers actions-runner, runs workflow,
  destroys server. Cleanup is bulletproof (trap-based, idempotent,
  always-destroy on exit).
- vcpkg manifest mode on Linux: builds Qt6.10 + MuPDF + tesseract
  + ICU + harfbuzz + freetype + tantivy/cargo from source in
  ~60 min on cpx42.
- GitHub Actions runner registration as systemd service.
- Workflow dispatch + polling via gh CLI.

**Resolution paths (user decision required):**

A. **Switch CI to Windows runners.** GitHub Actions has
   `windows-2022` hosted runners, clang-cl supports /fsanitize=address
   for ASan, /fsanitize=undefined is partial on Windows. libFuzzer
   builds on Windows via clang-cl. The existing fuzz harnesses at
   `tests/security/fuzz/` and CMake support both libfuzzer and
   aflpp backends, so this is a matter of adding a windows-2022
   workflow that mirrors hetzner-sanitizer.yml.

B. **Port PDFE to Linux.** Audit all uses of `std::wstring`,
   `path::native()`, `MAX_PATH`, Windows API calls. Replace with
   portable equivalents (e.g., `path::string()`, `PATH_MAX`).
   Significant effort — likely 1-2 weeks of careful work, and
   requires keeping Linux builds in CI to prevent regression.

C. **Accept current state.** Document Windows-only build, focus
   on the gtest suite that DOES work on Windows. The 3911 existing
   tests provide substantial coverage already. Defer ASan/UBSan
   to a Windows clang-cl invocation outside CI for now.

**Workflows pushed to repo for option A:** All four workflow YAML
files remain in place. They are correct for Linux; they fail
because the code is Windows-only, not because the workflows are
wrong. If the project ports to Linux later, the workflows will
work unchanged.

---

## CI-LIMITATION-ASAN-EXCEEDS-FREE-TIER-BUDGET

> **Triage 2026-09-06:** TOLERATE - infra ceiling, bounded and documented. Note its '3911 tests' is a 2026-06 timestamp, not a current fact (baseline is now 5933).

**Discovered:** 2026-06-03 (Windows sanitizer workflow validation)
**Status:** Known and bounded — not a code bug, an infra constraint

**Symptom:** `.github/workflows/windows-sanitizer.yml` successfully
builds PDFE with clang-cl + `-fsanitize=address -fsanitize=undefined`
on the `windows-2022` free-tier runner. Configure ~145 min,
Build ~25 min (24+25 = 170 min). The instrumented `pdfe_tests.exe`
then runs the full GoogleTest suite, but the ASan/UBSan overhead
slows test execution to the point where the full suite (3911
tests) cannot complete within GitHub's 6-hour single-job ceiling.

**Last run timing:**
- Configure: 144.6 min ✓
- Build: 24.9 min ✓
- Tests: cancelled at 180 min (still running when timeout hit)
- Total: 350 min wall-clock (hit `timeout-minutes: 350` cap)

Test output during the 180-min run showed only 1 ASan finding
(the known static-init false positive), suggesting tests were
actually progressing — just slowly.

**Why it's slow:** clang-cl + ASan + UBSan on Windows runs at
~3-5x normal speed for compute-bound C++ test code, much worse
than equivalent Linux clang where ASan is ~2x. Combined with the
single-threaded gtest execution PDFE uses (because the document
context isn't thread-safe between tests), each test takes longer
in serial.

**Resolution options (user decision required):**

A. **Shard tests across matrix jobs.** Split 3911 tests into 4-6
   shards via `--gtest_filter`. Each shard runs in its own
   `windows-2022` matrix job within 6h. Concurrent execution.
   Requires workflow restructuring and shard map maintenance as
   tests are added.

B. **Run a security-focused subset under ASan in CI.** Use
   `--gtest_filter` to run only tests in security-relevant
   suites (TestMaliciousPdf*, TestSecurityAudit*, etc.) under
   ASan. ~200-400 tests, completes within timeout. Full suite
   runs without sanitizers in a separate workflow.

C. **Build-only Windows CI + local sanitizer runs.** Windows CI
   verifies clean ASan/UBSan build (which catches the cross-
   compiler ABI issues we already fixed). Actual sanitizer test
   execution stays as the local-developer workflow via
   `scripts/security/run_sanitizers.ps1`. Pragmatic if developer
   ASan runs are reliable.

D. **Paid runner tier.** GitHub Actions has 4x and 8x larger
   Windows runners on paid tiers; an 8-core runner with the
   same wall-clock budget would likely complete the full suite.
   ~$0.16/min when running, ~$60-100 per full nightly run.

**Recommendation:** Option B if security is a top priority, else
C. A is correct long-term but maintenance cost is non-trivial.

**Proven-working artifacts:** The Windows CI build chain
(.github/workflows/windows-sanitizer.yml + windows-fuzz.yml) is
production-ready up through Build. Whichever option is chosen
above only changes the test-execution step. The dependency
resolution, vcpkg integration, clang-cl + Windows SDK setup, and
clang_rt sanitizer runtime wiring are all validated.

---

## CI-LIMITATION-CLANG-CL-ASAN-RUNTIME-WIN

> **Triage 2026-09-06:** TOLERATE - already RESOLVED in-body (Option B, MSVC ASan) on 2026-06-10; the heading does not say so, which is why it reads as open.

**Discovered:** 2026-06-03 (local Windows ASan attempt)
**Status:** RESOLVED via Option B (MSVC ASan + security subset) on 2026-06-10
in pipeline f3c91a2e Phase 4. The clang-cl runtime bug is left in
place as an upstream issue (clang 19's `asan_dynamic` DLL won't
launch on this Windows host), but the project no longer depends on
the clang-cl path: CI now uses the `windows-vs2022-msvc-asan` preset.

### Resolution chosen (2026-06-10)

**Option B** from CI-LIMITATION-ASAN-EXCEEDS-FREE-TIER-BUDGET below:
run a **security-focused gtest subset under MSVC `/fsanitize=address`**
on every push to master. The full gtest suite continues to run un-
instrumented in `windows-validation.yml`.

> **TRIGGER CORRECTION (F32, 2026-08-07).** The *subset* half of that
> 2026-06-10 decision still stands verbatim. The *trigger* half does
> not: the `push` trigger was removed 2026-07-20 to stop a ~6 h ASan
> build being started by every commit on the single self-hosted
> runner. The lane's live `on:` block is
> `schedule: '0 18 * * 0,2,4'` (Sun/Tue/Thu 18:00 UTC, max 3-day gap)
> **+ `workflow_dispatch`, with no `push`**. The sentence above is
> left as written because it records what was decided on 2026-06-10;
> read the current trigger from this note, or from
> `tests/docs/security_workflows.md`.

**Why Option B + MSVC ASan (not Option A sharding, not clang-cl):**
- Security is Question 2 of the three-question gate this pipeline
  is closing — under the user's own framing it is top priority,
  which the CI-LIMITATION-ASAN-EXCEEDS-FREE-TIER-BUDGET entry
  explicitly identifies as the trigger for picking Option B.
- The `windows-vs2022-msvc-asan` preset already exists in
  `CMakePresets.json` and a clean PASS was demonstrated on
  2026-05-02 (3105/3107 pass, 0 ASan findings — see
  `tests/security/reports/sanitizers/run_20260502T021015Z_msvc_verify2/summary.md`).
  MSVC's asan_dynamic-x86_64.dll ships with VS 2022 and loads
  fine on this Windows install; only the clang-cl variant tripped
  the 0xC0000135 loader bug.
- Self-hosted runner `pdfe-windows` has no GitHub-hosted 6h
  ceiling, but ASan still adds 2-4x runtime so even on the self-
  hosted runner the full 3911-test suite under ASan is wasteful;
  the security subset (`MaliciousPdfTest`, `SecurityAudit`,
  `Encryption*`, `Redaction*`, `Phase7RedactionTest`,
  `Phase8SecurityTest`, `PathSecurity`, `JavaScriptEngineTest`,
  `E2ERedaction`) completes in minutes and exercises every code
  path an attacker can reach.
- Option A sharding stays in reserve in `CMakePresets.json`
  (clang-cl preset retained but unused by CI) so if a future
  finding requires running the full suite under ASan it can be
  re-enabled without re-doing this work.

### Original clang-cl bug (left in place upstream)

`scripts/security/run_sanitizers_local.ps1` still builds PDFE under
clang-cl with `-fsanitize=address -fsanitize=undefined` to completion
(609/610 objects, full link). The resulting `pdfe_tests.exe` exits
with status `0xC0000135 (STATUS_DLL_NOT_FOUND)` during process
startup, before `main()` runs.

**Diagnosed (still applies):**
- All directly-imported DLLs are findable on PATH or in
  `bin/Debug/` (verified via dumpbin /dependents)
- `LoadLibraryW("pdfe_tests.exe")` returns a valid handle —
  static imports resolve
- The DLL load failure is in the clang asan runtime's early
  startup (before user code, before stderr is writable)
- `clang_rt.asan_dynamic-x86_64.dll` only depends on KERNEL32
  and api-ms-win-core-synch-l1-2-0, both findable
- Issue persists regardless of how PATH is configured

**Hypothesis:** clang 19's asan_dynamic DllMain may attempt to
LoadLibrary something not findable on this Windows install (e.g.,
a debugger helper DLL, a UCRT redistributable variant). Without
debugger attach (or gflags loader snaps), the actual failing
load is opaque.

**Future re-investigation path:** Try clang 20+ when available via
VS update. Or capture loader snaps via gflags to identify the
specific DLL that fails to load. Until then, the MSVC ASan path
is the canonical CI route and the clang-cl preset is
build-verification-only.

### Original ctest "exit code 8, 0 findings" — root cause (Phase 5)

The historical sanitizer FAIL recorded in
`tests/security/reports/sanitizers/run_20260603T152054Z/`
("ctest exit 8, 0 findings", clang-cl `vs2022-asan` preset) has the
following root cause, established in pipeline f3c91a2e Phase 5:

- The clang-cl ASan-instrumented `pdfe_tests.exe` **failed to launch**
  with `0xC0000135 (STATUS_DLL_NOT_FOUND)` during process startup,
  before `main()` ran (the same loader bug described above —
  clang 19's `asan_dynamic` DLL won't load on this Windows host).
- `ctest` invoked the binary, the OS refused to start it, and ctest
  surfaced the launch failure as a **non-zero exit (8)**. Exit 8 here
  is ctest/the-test-runner reporting "the test command did not run
  successfully", NOT a gtest assertion failure and NOT a sanitizer
  finding.
- Because the process never reached `main()`, **no test executed**, so
  the finding count was legitimately **0**. Zero findings + non-zero
  exit is the signature of a *launch/loader* failure, not a memory bug.

**Conclusion:** the original exit-8 FAIL was a **loader
misconfiguration** (clang-cl ASan runtime DLL not loadable), *not* a
real memory error and *not* a real test failure. There was nothing to
"fix" in the test suite or the application code.

**Why it is now moot:** Phase 4 switched CI to the MSVC
`/fsanitize=address` preset (`windows-vs2022-msvc-asan`), whose
`asan_dynamic-x86_64.dll` ships with VS 2022 and loads cleanly on this
host. The MSVC ASan run launches, runs the full security subset, and
records `Verdict: PASS` with 0 findings (run 27367556714,
`run_20260611T203905Z_msvc_ci/`). The clang-cl exit-8 path is retired
from CI; the clang-cl preset is build-verification-only.

### What runs in CI now

`.github/workflows/windows-sanitizer.yml`, on `schedule:
'0 18 * * 0,2,4'` (Sun/Tue/Thu 18:00 UTC, max 3-day gap) plus
`workflow_dispatch` — **no `push` trigger** (removed 2026-07-20;
cadence tightened from weekly to Sun/Tue/Thu by follow-up F30):
1. Configures with the `windows-vs2022-msvc-asan` preset.
2. Builds `pdfe_tests` target only.
3. Runs gtest filter
   `MaliciousPdfTest.*:SecurityAudit.*:Encryption*.*:Redaction*.*:Phase8SecurityTest.*:Phase7RedactionTest.*:E2ERedaction.*:PathSecurity.*:JavaScriptEngineTest.*`
   under `ASAN_OPTIONS=halt_on_error=0:print_stacktrace=1:detect_leaks=0:abort_on_error=0`.
4. Writes the run report to
   `tests/security/reports/sanitizers/run_<stamp>/` and updates
   `tests/security/reports/sanitizers/summary.md`.
5. Uploads logs as a workflow artifact.

The runner is `[self-hosted, Windows, X64, pdfe-windows]` —
identical to `windows-validation.yml` — so the runtime ASan DLL
(`clang_rt.asan_dynamic-x86_64.dll` shipped with VS 2022) is
already available on this host.

---

## CI-LIMITATION-WINDOWS-FUZZ-RUNNER

> **Triage 2026-09-06:** TOLERATE - already resolved in-body (Phase 5 repointed the workflow to the self-hosted runner); heading does not say so.

**Discovered:** 2026-06-11 (pipeline f3c91a2e Phase 5)
**Symptom:** The last scheduled `windows-fuzz.yml` run (27326457133,
2026-06-11 05:42 UTC) failed all 7 libFuzzer harness jobs within
2–9 seconds of starting — before any build step ran.

**Root cause:** The workflow was pinned to `runs-on: windows-2022`
(a GitHub-hosted runner) with a cold vcpkg cache. The job died at
environment/setup, consistent with CI-LIMITATION-GITHUB-FREE-TIER
(the free-tier runner cannot complete the vcpkg cold-cache build of
Qt6 + MuPDF + Tesseract). The 2–9s failure with zero recorded steps
is a setup/agent failure, NOT a fuzz crash and NOT a real finding.

**Resolution (Phase 5):** `windows-fuzz.yml` was repointed to the
self-hosted `[self-hosted, Windows, X64, pdfe-windows]` runner — the
same warm-cache host that runs `windows-sanitizer.yml` and
`windows-validation.yml` green. A `push:` trigger filtered to
`src/**` (+ fuzz/CMake/vcpkg paths) was added with a 120s/harness
smoke duration; the nightly schedule keeps the full 1800s/harness
campaign; `concurrency.cancel-in-progress` prevents queue backup on
the single-threaded runner.

**Residual risk (open):** the clang-cl fuzz harnesses link the
clang_rt ASan runtime — the same DLL that trips
CI-LIMITATION-CLANG-CL-ASAN-RUNTIME-WIN (0xC0000135) on this host.
The fuzz step already prepends `$RT_LIB` to PATH, which is the
documented mitigation, but the first self-hosted push run must be
observed: if the harnesses hit 0xC0000135 at launch, the fuzz build
needs the same MSVC-ASan-style runtime wiring the sanitizer workflow
uses. Tracked as GAP-C7/GAP-C8 verification in
`tests/docs/gap_checklist.md`; the orchestrator's post-push CI
observation closes it.

> **F32 (2026-08-07) — this residual risk is CLOSED, and the mechanism
> named above is gone.** The observation did happen: the 2026-06-29
> self-hosted run got past the build (see the next entry,
> CI-LIMITATION-WINDOWS-FUZZ-CXX-EXCEPTION-DEADLY-SIGNAL). Separately,
> `windows-fuzz.yml` has had **no `push` trigger since 2026-07-20** —
> its live `on:` block is `schedule: '30 3 * * *'` (nightly 03:30 UTC)
> `+ workflow_dispatch` — so there is no "post-push CI observation" to
> wait on any more; the nightly is what observes. Text above left as
> written because it is the dated record of the risk as it stood.

---

## CI-LIMITATION-WINDOWS-FUZZ-CXX-EXCEPTION-DEADLY-SIGNAL

> **Triage 2026-09-06:** FIX - filed as GitHub #140. This entry stays as the evidence record; the issue tracks the remaining work.

**Discovered:** 2026-06-29 (pipeline d4e7f921 Phase 6 post-push CI observation —
the first self-hosted `windows-fuzz.yml` run to get PAST the build, closing the
GAP-C7/C8 "observe first run" item from CI-LIMITATION-WINDOWS-FUZZ-RUNNER).

**Symptom:** All 7 libFuzzer harnesses report `rc=77 crashed=True artifacts=1`
within seconds of the campaign starting, and the job fails. This is NOT the older
2–9s setup failure (that was pre-build; here steps 1–10 — configure + build all 7
harnesses — are green and the failure is in the *Run fuzz campaigns* step). The
crash input is trivial (e.g. base64 `ICAg` = three spaces for
`fuzz_encryption_dict`).

**Root cause — benign caught C++ exception mis-reported as a fatal crash, NOT a
real memory-safety finding.** The recorded stack on every harness is:

```
==NNNN== ERROR: libFuzzer: deadly signal
  ... fuzzer::Fuzzer::CrashCallback / ExceptionHandler ...
  ... SentinelOne InProcessClient64.dll  (exception-dispatch hook) ...
  _CxxThrowException
  pdfe::core::MuPdfContext::executeReturn<fz_document*>(...)   MuPdfContext.h:116
  pdfe::core::Document::Document(...)                          Document.cpp:115
  pdfe::core::Document::open(...)                              Document.cpp:245
  pdfe_fuzz_one_input  ->  LLVMFuzzerTestOneInput
```

That is a normal C++ `throw` — `MuPdfContext::executeReturn` translating MuPDF's
`fz_throw` into a `MuPdfException` when the parser rejects a malformed PDF. It is
the **designed** error path, and the harnesses already wrap `Document::open` in
`try { … } catch (...) {}` (e.g. `fuzz_encryption_dict.cpp:73-83`), exactly
because a malformed input is *expected* to throw. It is NOT an ASan
heap-overflow / use-after-free / SEGV / stack-overflow report.

The escalation happens in the Windows exception-dispatch layer: libFuzzer's
crash handler (and/or SentinelOne's `InProcessClient64.dll`, which hooks
`RtlDispatchException` on this host — frames #5–7 of every crash) sees the
SEH-level C++ EH exception (`0xE06D7363`) and fires `CrashCallback` ("deadly
signal") *before/around* the harness's frame-based `catch(...)` can swallow it.
Net: any input that makes MuPDF throw — i.e. most of the corpus — is reported as
a crash, so the campaign is unconditionally red.

**Evidence it is environmental / pre-existing, not a code regression:**
- The scheduled run BEFORE the triggering push (28350927361, 05:42) has a
  byte-identical signature; `windows-fuzz.yml` had failed on every *completed*
  run for days. Pipeline d4e7f921 (font work + test-harness Python) touched no
  fuzz-harness or fuzzing-relevant `src/**` path.
- Trivial 3-byte input trips all 7 harnesses identically — the universal
  "malformed input → throw" path, not 7 distinct deep bugs.
- The signature is a C++ `_CxxThrowException`, not an ASan/UBSan memory error.

**NOT to be confused with** `PDFE-SEC-MUPDF-RECURSION-DOS` (a *real* stack-overflow
finding from `fuzz_document_open` on a pathologically deep page graph). That has a
deep-recursion / access-violation stack, NOT the `_CxxThrowException` →
`Document::open` exception signature, so the triage below classifies it as REAL.

**Resolution (pipeline d4e7f921 follow-up):**
1. **Auto-triggers disabled.** `windows-fuzz.yml` is now `workflow_dispatch`-only
   (push + nightly-schedule triggers removed) so a chronically-red campaign no
   longer fails CI on every master push / nightly. Run it on demand:
   `gh workflow run windows-fuzz.yml` (optional `-f duration_seconds=<n>`).
2. **Campaign triage added.** The *Run fuzz campaigns* step now re-runs each crash
   artifact and classifies it: a "deadly signal" whose stack is the benign
   `_CxxThrowException` + `MuPdfContext::executeReturn`/`Document::open` C++-exception
   pattern (with NO ASan/UBSan/SEGV/stack-overflow/OOM/timeout marker) is recorded
   as a TRIAGED-BENIGN false positive and does NOT fail the job; anything else —
   including any unrecognised crash — is treated as a REAL finding and fails the
   job (conservative default, so genuine findings like the recursion DoS are never
   masked). Both raw and triaged verdicts are written to the JSON summary.

**Residual / proper fix (open):** the benign exception should ideally never reach
libFuzzer's handler at all — options are to stop libFuzzer treating the C++ EH
code as fatal, or to exclude the runner workspace from SentinelOne's
exception-hooking so the harness `catch(...)` runs normally. Until then the triage
keeps the on-demand campaign signal honest. Tracked alongside
CI-LIMITATION-WINDOWS-FUZZ-RUNNER.

> **PROBED 2026-09-07 (pipeline backlog-workable-p82, phase 9), and the result
> is a NEGATIVE one worth recording.**
>
> The residual names two options, both environmental. The first — "stop
> libFuzzer treating the C++ EH code as fatal" — is actually available *in this
> repository*: libFuzzer's `-handle_winexcept` flag ("if 1, try to intercept
> uncaught Windows visual C++ exceptions") defaults to 1, and it is a campaign
> flag, not a host setting. It has been added to the campaign invocation in
> `windows-fuzz.yml`.
>
> **But it is NOT demonstrated to fix anything, and that is stated rather than
> implied.** A minimal libFuzzer harness that throws-and-catches on every input
> (the same shape as the real harnesses) was built locally with clang-cl and run
> over a corpus of malformed inputs:
>
> ```
> RUN A -- default (handle_winexcept = 1):  #20000 DONE, exit 0
> RUN B -- with -handle_winexcept=0:        #20000 DONE, exit 0
> ```
>
> **Both processed 20,000 inputs. Neither aborted.** So libFuzzer's interception
> alone is NOT sufficient cause for the CI symptom, and the other factor this
> entry already names — SentinelOne's `InProcessClient64.dll` hooking
> `RtlDispatchException`, frames #5–7 of every recorded crash — is the
> load-bearing half. That agent is not active on the probe machine, so the
> failure does not reproduce off the affected host and the flag's effect on the
> real symptom is untested.
>
> What the probe DOES establish: the flag is accepted by the clang-cl libFuzzer
> build in use (an unrecognised flag is rejected; this one is not), so adding it
> cannot break the campaign. Evidence:
> `docs/audit/backlog-p82/phase-9/fuzz-winexcept-probe/`.
>
> GitHub #140 stays OPEN. It can only be closed by a dispatched
> `windows-fuzz.yml` run on the affected runner showing a campaign processing
> inputs past the first throw.


---

## CI-LIMITATION-WINDOWS-VALIDATION-UI-E2E-FLAKES

> **Triage 2026-09-06:** FIX - filed as GitHub #138. This entry stays as the evidence record; the issue tracks the remaining work.
>
> **RE-RUN 2026-09-15** (pipeline p85-remaining-open phase 6). All five re-run
> LOCALLY against current HEAD in one pytest invocation: **5 passed, 0 failed,
> 81 s**. Item #3's stale golden was already deleted by 9a4e2f71 phase-4 and
> replaced by the centroid oracle, so "re-baseline the golden" is satisfied by
> there no longer being one — verified: `coord_rotation_page.png` is absent
> from the tree and the test asserts the ink centroid instead.
>
> **Items #3 and #4 first re-ran RED, for a reason nobody had seen.** Both
> failed with `WIN-45 reachability blocked: PdfCanvas absent from UIA tree` —
> a message that reads as an environment limitation and is not one. The canvas
> was in the tree throughout; `canvas_region()` was asking for the literal
> AutomationId `QApplication.mainWindow.pdfCanvas`, and Qt builds that id from
> the objectName CHAIN, which had gained a `pdfCentralArea` segment:
> `QApplication.mainWindow.pdfCentralArea.pdfCanvas`. Six call sites hard-coded
> the stale literal. Fixed by matching the trailing segment
> (`canvas_click.find_canvas`), which is what the other locators in this repo
> already do. **Two of the five items in this entry were therefore blocked by a
> harness false negative dressed as a runner limitation — the worst kind,
> because it looks like something you cannot fix.**

**Discovered:** 2026-06-30 (pipeline d4e7f921 Phase 6 post-push CI observation).
The `windows-validation.yml` UI E2E step (#15) on master HEAD `fb50fd4` reported
**3401 tests, 5 failed, 437 skipped**. The gtest step PASSED (4043) and
`windows-sanitizer.yml` PASSED, so the C++/product side is green — all 5 failures
are test-harness / golden / runner-session issues, **NOT product regressions**.

**Diagnosis method:** pulled the run's `ui-junit.xml` artifact AND re-ran all 5
locally in the interactive RDP session (Bash tool). Local result = **3 failed /
2 passed**; the cross-environment split is the diagnosis. The decisive split:
items that fail on BOTH the runner and locally are flaky/stale; items that fail
ONLY on the runner are specific to the self-hosted runner's NON-INTERACTIVE
session (different desktop/display/UIA behaviour than an interactive login).

| # | Test | CI runner | Local (interactive) | Class |
|---|---|---|---|---|
| 1 | `tests/test_ui_text_editing.py::test_text_edit_tool_deactivates_when_other_tool_selected` | FAIL | FAIL | both-env flaky (UIA timing) |
| 2 | `tests/test_ui_text_editing.py::test_font_size_change_via_toolbar_spinner` | FAIL (SSIM=1.0) | PASS | **runner-only** (session UIA) |
| 3 | `tests/ui_automation/test_render_coord_rotation.py::test_rotated_text_maps_on_screen` | FAIL | FAIL | both-env (stale golden) |
| 4 | `tests/ui_automation/test_render_textedit_embedded_font.py::test_new_char_absent_from_subset_renders_serif_italic` | FAIL | FAIL | both-env flaky (word-seg heuristic) |
| 5 | `tests/visual/test_canvas_pyautogui_regression.py::test_canvas_pyautogui_visual_regression` | FAIL (SSIM=0.81) | PASS | **runner-only** (desktop-grab) |

### Item #1 — toolbar visibility (both environments, flaky)
**Fail mode:** `AssertionError: TextEditToolBar should be visible right after
activating the text-edit tool` — `_text_edit_toolbar_visible()` returns False.
**Root cause:** the assert fires immediately after tool activation; the
TextEditToolBar's UIA children are not reliably reachable that instant (UIA
enumeration timing), so it intermittently reads not-visible. The toolbar-visibility
wiring itself is unchanged and correct (MainWindow.cpp:4034). **Evidence:** fails
on the runner AND locally. **Resolution:** replace the immediate assert with a
bounded poll/wait (>=3 s) for toolbar reachability before asserting; keep the
deactivation-on-other-tool-selection check intact.

### Item #2 — font-size spinner (RUNNER-ONLY)
**Fail mode:** after a toolbar font-size spinner change the canvas is `SSIM=1.0000`
(byte-identical) — i.e. nothing changed. **Root cause:** the spinner's UIA
set-value gesture silently no-ops in the runner's non-interactive session, so the
font size is never applied and the canvas legitimately doesn't change; this is a
harness/session limitation, NOT a font-size-apply bug. **Evidence:** fails on the
runner; **passes locally** (interactive session applies the spinner value).
**Resolution:** drive the font-size change through a deterministic
`PDFE_TEST_PIPES` seam and read back the applied value before asserting the canvas
change; do not weaken the canvas-change assertion.

### Item #3 — coord-rotation pixel golden (both environments, stale)
**Fail mode:** `capture diverged from committed golden coord_rotation_page.png
(> 8% pixels differ)` even WITH the `crop_to_page_content` normalization.
**Root cause:** the committed golden (`tests/ui_automation/goldens/coord_rotation_page.png`,
a c4a9e271 Phase-10 "geometry-stable golden" artifact, commit `69659bf`) does not
match the current render on EITHER environment — an exact-pixel golden is
inherently rig-sensitive and the c4a9e271 P10 crop attempt did not fully stabilize
it. **Evidence:** fails on the runner AND locally; the golden is a prior-pipeline
artifact, not produced by d4e7f921. **Resolution:** replace the pixel-golden
compare with a rig-robust structural/positional oracle (verify rotated text maps to
the geometrically correct screen region) and remove the golden.
**RESOLVED** (pipeline 9a4e2f71 phase-4, commit `7440bc1`): `test_rotated_text_maps_on_screen`
now asserts the rotated text's ink centroid sits in the page's central band
(x[0.32-0.68] y[0.12-0.60]) after `crop_to_page_content`, alongside the existing
not-blank checks; `coord_rotation_page.png` deleted. A double-rotation regression
moves the centroid out of band (or blanks the page) — caught without a pixel golden.

### Item #4 — new-char word segmentation (both environments, flaky)
**Fail mode:** `expected >=4 word segments in 'A Tale of z Collards', got 3`.
**Root cause:** the test's PRIMARY assertions (`assert_overlay_font_class` serif +
`assert_slant_matches` italic — the actual b91f4e83/d4e7f921 font fix) PASS; only
the SECONDARY `word_segments()` isolation of the new 'z' glyph fails because the
inter-word gap fell below `word_segments()`'s hardcoded 28%-of-glyph-height
threshold on some renders. **Evidence:** fails on the runner AND locally
(intermittently — passed in the pipeline d4e7f921 P5 render sweep days earlier);
the serif/italic verification is unaffected. **Resolution:** harden the new-char
isolation (rig-calibrated gap threshold or a positional crop) without dropping the
serif/italic assertions.
**RESOLVED** (pipeline 9a4e2f71 phase-3, commit `7f059fe`): `word_segments()` gained a
`gap_frac` param; the test sweeps it downward until the line splits into exactly its
5 words, then isolates the new 'z' — rig-robust. serif/italic assertions unchanged.

### Item #5 — PyAutoGUI canvas baseline (RUNNER-ONLY)
**Fail mode:** `PyAutoGUI visual regression 'pdf_canvas_pyautogui_baseline':
SSIM=0.8138 ... pixel_fraction=0.1610`. **Root cause:** the test captures the
DESKTOP via a PyAutoGUI screenshot; the runner's non-interactive session desktop
composition/display state differs substantially from the seeded baseline, so the
capture diverges. Our code did NOT change the base canvas (it passes locally and
passed a 06-28 runner subset run). **Evidence:** fails on the runner; **passes
locally**. **Resolution:** switch the capture to a display-state-independent
PrintWindow grab and regenerate the baseline FROM THE RUNNER (the only source of
truth for the non-interactive session render); the baseline must be runner-produced.
**RESOLVED** (pipeline 9a4e2f71 phase-4 `7440bc1` PrintWindow capture; phase-5 structural
oracle, this commit): keeps the Win32 PrintWindow capture (`capture_hwnd`) but REPLACES the
pixel-SSIM-vs-baseline compare with a rig/session-robust STRUCTURAL oracle
(`_assert_canvas_renders_document`): crop to the page block, assert modest dark-ink over a
mostly-light page (blank/solid/wrong-window all fail). **Why not a runner baseline:** the
"regenerate from runner" approach (phase-5 seed run 28465445439 on `37a2063`) was TRIED and
FAILED — the runner's render varies >15% run-to-run (SSIM 0.83 runner-vs-runner in verify run
28495538264, and shifts again across a runner restart / DPI change), so no committed pixel
baseline can pass at 0.95 and a lower threshold would be meaningless. The seed machinery
(seed input, screenshots upload, integrity-guard exclusion) and the committed baseline are
reverted/removed. Measured ink≈0.17 / bright≈0.83 on a text page — deterministic across rigs.

### Matrix-health over-claim exposed + reconciled (phase-6)
The windows-validation "Matrix-health gate" (`check_matrix_health.py`: committed-green
floor + no green->red) had been DORMANT — it only runs when the UI E2E step passes, and
that step ALWAYS failed on these 5 flakes, so the gate was skipped every run. Fixing the
5 flakes made the UI step pass for the first time, so the gate finally ran (run
28554887271) and FAILED: committed closeout green = 768 but a real full runner run
produces only ~439 green + ~330 legitimately-SKIPPED (0 failures, 2971 passed, 430
skipped). The 768 floor was never validated by a real green run — it was an over-claim
(largely `generate_matrix.py` statically marking `act_` rows green). The ~334
over-claimed rows skip on DOCUMENTED preconditions / harness gaps (WinAppDriver not
reachable, "feature not in build", image-selection/atlas prerequisites,
formToolAction-not-in-toolbar, radio-button-needs-canvas-drag, session-drift) — NOT
regressions. **RESOLVED** (phase-6): reconciled `ui_test_matrix.csv` to the measured
runner reality — the 334 over-claimed-green rows demoted to `blocked` (committed green
768->434), so the floor reflects what the runner actually achieves and the gate is
meaningful going forward (catches real green->red). 0 tests failed; nothing was hidden.

### Why this matters / what is NOT affected
These are the same flake/stale-golden/runner-session classes as
[gap20](#) (the theme-toggle strict-xfail demotion) and the c4a9e271 Phase-8
discovery run (11 failures). The font work shipped in b91f4e83 + d4e7f921 is
validated where it counts: CI gtest 4043 pass, sanitizer green, and item #4's
serif+italic assertions (the actual fix) pass on CI. **Resolution: pipeline 9a4e2f71
(the full-fix pipeline) — ALL 5 RESOLVED:** #1/#2 deterministic UIA seams (phase-2,
`9f08f4d`/`a620ab3`); #4 rig-calibrated word-seg (phase-3, `7f059fe`); #3 centroid
positional oracle (phase-4 `7440bc1`); #5 PrintWindow capture + structural ink/bright
oracle (phase-4 `7440bc1` + phase-5 `f1ac48c` — the runner-baseline approach was tried
and proved unviable, SSIM 0.83 runner-to-runner). Final full windows-validation green on all
5 confirms it (phase-6).

---

## How to add a new known issue

1. Add an entry here with: ID, discovered date, symptom, root cause,
   workaround, test status, resolution path.
2. If the test should skip rather than fail, use:
   `GTEST_SKIP() << "PDFE-LIMITATION-<ID>: see tests/docs/known_issues.md";`
3. If the test should xfail (pass when fixed), use pytest's
   `@pytest.mark.xfail(reason="PDFE-LIMITATION-<ID>")`.
4. Reference the issue ID in any commit messages that touch the
   workaround code, so future investigators can find this doc.

---

## CI-LIMITATION-UIA-MODIFIER-CLICK-FULL-SUITE (2026-06-15)

> **Triage 2026-09-06:** TOLERATE - reasoned coverage argument stands: the multi-select rotate FUNCTIONALITY is covered by sibling tests and headless gtests; only the pywinauto gesture mechanic is flaky.

**Symptom:** `tests/ui_automation/test_page_operations_p1.py::test_p1_page_operation[page_multi_select_ctrl_click]` and `[page_multi_select_shift_click]` pass reliably in isolation (4/4) but intermittently fail under the full 87-file, ~5h UI-suite run with "rotate after Ctrl+click/Shift+click multi-select failed".

**Root cause:** The tests drive a real pywinauto `click_input(pressed="control"|"shift")` modifier-click gesture on a page tile. That gesture leaves the PDFE main window non-foreground with lingering Win32 modifier state. The subsequent action (triggering `rotatePageCWAction`) then needs a UIA lookup of the `testToolActivatePipe` widget; under full-suite UIA contention that lookup intermittently fails after the modifier-click, so the rotate doesn't fire and the dirty oracle correctly reports no change. The flakiness is in the **pywinauto modifier-click harness mechanic**, not in PDFE — the multi-select page-rotate FUNCTIONALITY works.

**Mitigation/fix attempts:** Routed the rotate through the `testToolActivatePipe` seam (foreground-independent QTimer-deferred trigger) and switched the dirty oracle to polling. This made the tests 4/4 reliable in isolation, but full-suite UIA contention still trips the post-modifier-click pipe *lookup* itself.

**Coverage (why this is a documented limitation, not a SKIP=FAIL dodge):** The multi-select page-rotate FUNCTIONALITY is covered by the passing `page_rotate_cw_multi` / `page_rotate_ccw_multi` / `page_rotate_180_multi` tests (same file, reliable `select_page_items` selection path) and by the headless `PageCommands` gtests. Only the pywinauto *gesture mechanic* is harness-flaky under load, so the two gesture tests are listed in `_KNOWN_LIMITATION_IDS` (skip with explicit reason).

---

## PDFE-SEC-MUPDF-RECURSION-DOS (2026-06-18)

> **Triage 2026-09-06:** FIX - filed as GitHub #139. This entry stays as the evidence record; the issue tracks the remaining work.

**Symptom:** A crafted PDF whose page graph is pathologically deep or cyclic
(e.g. a chain of thousands of `/Pages` nodes each `/Kids`-referencing the next,
with matching `/Parent` back-links) drives MuPDF's xref-repair / parser /
page-tree walk into effectively-unbounded recursion that exhausts the thread
stack — a hard crash (stack overflow → access violation). Found by the
`fuzz_document_open` libFuzzer harness once the native-Windows `/MT` fuzz build
was made functional.

**Root cause:** Recursion inside the vendored MuPDF (1.26.10) on adversarial
input. It is **not trappable in-process**: `fz_try`/`fz_catch` is setjmp-based
and cannot catch a hardware stack overflow, the overflow corrupts the allocator
state, and a C++/`fz_throw` reject cannot unwind cleanly across the hundreds of
nested `setjmp` frames. The exact per-level walker resists pinpointing because
the overflow corrupts the stack so no debugger (cdb `kc`/`kb`/`dps`) can
reconstruct it; even a 256 MB stack overflows, confirming it is effectively
cyclic, not merely deep. This is the class of issue Artifex/OSS-Fuzz fix
upstream in MuPDF itself; an upstream report is the proper long-term path.

**Mitigation (shipped):** Out-of-process crash isolation. The UI runs the
document open in a disposable `pdfe_preflight` child process
(`src/app/pdfe_preflight.cpp`, `src/core/PreflightOpen.{h,cpp}`,
`src/ui/MainWindow.cpp::openFile`) before opening in-process; any document the
child cannot open cleanly (crash / timeout / parser error) is refused with a
user-facing message. The child crashing never touches the editor. Verified
end-to-end against clean (shipping) MuPDF: the DoS input → child crash →
`isDocumentOpenSafe()` returns false → refused; a normal PDF → opens. This is
the same isolation strategy browsers use for untrusted content, and it
contains ALL open-time crash classes, not just this one.

**Test status:** The `windows-fuzz` campaign WILL still report this crash for
the raw `fuzz_document_open` harness (the harness exercises the parser
directly, by design — that is the fuzzer's job). The *application* is protected
by the preflight isolation. Criterion 4 ("windows-fuzz, no crash") is therefore
"infrastructure operational; one MuPDF recursion DoS found, contained at the
app layer by process isolation, tracked for MuPDF upstream."

**Resolution path:** (1) report the reproducer to Artifex/MuPDF upstream;
(2) on a MuPDF bump, re-run `fuzz_document_open` to confirm; the preflight
isolation remains as defence-in-depth regardless.

> **Step 2 is now ENFORCED, not merely written down (2026-10-09, GitHub #139).**
> Until now nothing read the sentence above: there was no checklist item,
> workflow step or pin tying a version change to the recheck, so a bump could
> silently fix this, silently fail to, or silently change its shape and nobody
> would learn which.
>
> `tools/gate/check_mupdf_bump_recheck.py` (wired into `fast-checks.yml`) fails
> the build when the MuPDF this tree resolves can have moved since the recorded
> recheck in `tools/gate/mupdf_recheck_baseline.json`. It pins **both** doors a
> bump can come through — the declared floor in `vcpkg.json`
> (`"version>=": "1.26.10#1"`) **and** `builtin-baseline`. The second is the
> likely one and the one a naive guard misses: the floor is not the resolved
> version (this triplet has **1.26.10#4** installed), and a baseline bump moves
> the resolved version without altering the libmupdf stanza by one character.
>
> **Two honest limits.** The guard over-triggers — a baseline bump that leaves
> libmupdf untouched still reddens, because resolving the baseline needs a
> configured vcpkg and a network that a fast check lacks. And the recorded
> recheck is still the 2026-06-18 run *that found this defect*; the pins were
> written down on 2026-10-09 without a new harness run, since the gap #139 names
> is that they were never written down at all. Both limits are stated in the
> record file and in the guard's failure message rather than left to be
> discovered.
>
> Step 1 (the upstream report) is a human action outside any repository. No
> guard covers it and none pretends to; it stays open in the issue.

---

## CI-LIMITATION-RENDER-JUNIT-EMPTY-ARTIFACT (2026-07-03)

> **Triage 2026-09-06:** FIX - filed as GitHub #141. This entry stays as the evidence record; the issue tracks the remaining work.

> **RESOLVED 2026-09-18 (pipeline p87-open-issues). The emitter is correct and
> is now REQUIRED to prove it, every run.** Re-measured a fourth time, through
> the workflow's exact chain rather than a re-reading of it — pytest, then
> `scrub_artifacts.py`, then `check_render_junit.py`, the same three commands in
> the same order as the render step:
>
> ```
> $ python -m pytest tests/ui_automation/test_render_oracle_golden_unit.py \
>       --timeout=240 --junitxml=<tmp>/render-junit.xml -v
> pytest exit=0
> root: testsuites {'name': 'pytest tests'}      <-- no counts on the root
> testcases: 12
> $ python tools/gate/scrub_artifacts.py <repo> <tmp>/render-junit.xml <tmp>/render-suite.log
> [scrub-artifacts] render-junit.xml: clean
> after scrub root: {'name': 'pytest tests'} cases: 12
> $ python tools/gate/check_render_junit.py <tmp>/render-junit.xml --backfill-root
> ...: 12 <testcase> element(s)
> NOTE: the root <testsuites> carries no counts ...
> backfilled root <testsuites> with {'tests': 12, 'failures': 0, 'errors': 0, 'skipped': 0}
> after backfill root: {..., 'tests': '12', ...} cases: 12
> ```
>
> **Root cause, now CONFIRMED rather than suspected: the artifact was never
> empty; its ROOT element was.** pytest writes the counts on the child
> `<testsuite>` and leaves `<testsuites>` bare, so `testsuites/@tests` reads as
> nothing — indistinguishable, to a reviewer or an external tool reading the
> root, from a suite that produced no tests. `--junitxml` was correctly wired
> and correctly aggregated the whole time (subset run above: 12 in, 12 out; the
> full 13-module run measures 62). The "per-file JUnit never merged" hypothesis
> in the paragraph below is **refuted**: the render step invokes `python -m
> pytest` ONCE over all `test_render_*.py`, with a single `--junitxml`. There is
> no per-file XML and no merge step — `tests/scripts/run_render_suite.py` does
> merge per-file JUnit into `render-junit-merged.xml`, but the workflow does not
> invoke it and never has.
>
> **What changed, so the symptom cannot recur unobserved.** The render step now
> runs `tools/gate/check_render_junit.py` on the scrubbed artifact, BEFORE the
> exit-code check, with `--tests-dir` (every `test_render_*.py` on disk must have
> contributed a testcase) and `--backfill-root` (the child aggregate is written
> onto the root, so both readings agree). Measured in both directions: a
> zero-testcase artifact exits 1 with "FAIL: ZERO testcases" and the guard's own
> `--self-test` passes 12 cases including the isolated zero-testcase rule.
>
> **Stated limitation.** The original run `28615963046` is HTTP 404 and past
> retention, so the 2026-07-03 observation itself still cannot be re-examined,
> and the runner has been offline throughout, so the guard has not yet fired in
> a real windows-validation run. That is why this is resolved on a MECHANISM
> (the artifact must now assert its own contents) rather than on a fourth clean
> measurement.

> **RE-VERIFIED 2026-09-07 (pipeline backlog-workable-p82, phase 9). The
> suspected root cause is REFUTED, and the symptom does not reproduce.** Each
> link in the chain was measured rather than reasoned about:
>
> 1. **pytest's `--junitxml` populates the file.** The workflow's exact command
>    shape, run locally over all 13 `test_render_*.py` files, produced an XML
>    with **62 testcases** (`tests=62 failures=40 errors=6 skipped=4`).
>    Committed: `docs/audit/backlog-p82/phase-9/render-junit-local.xml`.
>    (The 40 failures are `WIN-45 reachability blocked — PdfCanvas absent from
>    UIA tree`: this session has no interactive desktop, so every app-driven
>    test fails identically at `render_drive.py:172` while the 12 non-UI tests
>    pass. That is an environment condition, not 40 regressions, and it does not
>    affect the property under test — the XML records failures and errors just
>    as it records passes.)
> 2. **`scrub_artifacts.py` does not empty it.** It runs on `$junit` after the
>    suite. Measured on a real pytest artifact: 7 testcases in, 7 out on the
>    no-op path, and 7 in / 7 out on the *rewrite* path (forced by injecting
>    absolute paths), with the account name removed.
> 3. **PDFE's own JUnit consumer reads pytest's shape correctly.**
>    `check_junit_xml.py` sums the child `<testsuite>` elements rather than
>    reading the root, so it reports `tests=12` on a pytest artifact.
>
> Point 3 is the one worth keeping: **pytest writes the counts on the child
> `<testsuite>` and leaves the root `<testsuites>` bare of them.** Any consumer
> that reads `testsuites/@tests` sees nothing — which is exactly what "contains
> 0 testcases" would look like to a reviewer eyeballing the root element. That
> is the most probable explanation of the original observation.
>
> **It could not be closed out against the original evidence:** run
> `28615963046` returns HTTP 404 and its artifacts are long past retention, so
> the 2026-07-03 artifact cannot be re-examined. Per DCA 0046 ("the row was not
> stale, it was unobserved"), this entry is therefore NOT being declared stale
> on reasoning alone — it records three positive measurements and one
> unverifiable original observation. GitHub #141 stays OPEN until a real
> windows-validation run produces a `render-junit.xml` that can be inspected;
> the self-hosted runner is currently OFFLINE, so no such run could be
> obtained here.
>
> **STALE ON BOTH COUNTS — corrected 2026-10-23, audit `p99-issue-audit`
> phase 4.** This is a LIVE-state file, so both halves of that sentence matter:
>
> * **GitHub #141 is CLOSED**, and has been since **2026-09-18** — about a month
>   before this correction. The paragraph above says it "stays OPEN".
> * **The self-hosted runner is ONLINE**, registered as `pdfe-runner-01`
>   (`status: online`, `busy: false`), brought back up on 2026-10-23. The stated
>   reason a run "could not be obtained here" no longer holds.
>
> The analysis above is left intact — the three positive measurements and the
> one unverifiable 2026-07-03 observation are still the evidence, and DCA 0046
> still governs how it was reasoned about. Only the state claims are corrected.
>
> **The closure date is from the RETIRED repository, not this one.** The October
> 2026 migration recreated every issue, so `created_at` and `closed_at` on
> `ipswitch8/PDFE` are **synthetic**: #141 reads `created 2026-10-21T05:11:45Z,
> closed 05:14:57Z` — three minutes apart, which is the migration writing both.
> The real dates (`created 2026-09-06`, `closed 2026-09-18`) survive only on
> `ipswitch8/PDFE_old2`. See
> [`docs/audit/p99-issue-audit/phase-2-tracker-audit.md`](../../docs/audit/p99-issue-audit/phase-2-tracker-audit.md).

**Symptom:** In windows-validation, the "Run render-verification suite (Release
pdfe.exe)" step (`test_render_*.py`) passes (step conclusion = success) and
`render-suite.log` shows real results (e.g. run 28615963046: `46 passed, 2
xfailed, 2 xpassed in 782.94s`), but the uploaded `render-junit.xml` artifact
contains **0 testcases** — the render results are not captured in JUnit XML form
in the artifact bundle. Surfaced by the phase-6 karen gate of pipeline 9a4e2f71
(the render suite only started running in CI once the UI E2E step began passing,
so this reporting gap had not been observable before).

**Root cause (suspected 2026-07-03; REFUTED 2026-09-18 — see the RESOLVED
banner above, which records the confirmed cause):** the render step invokes pytest in
a way that does not point `--junitxml` at the aggregated `render-junit.xml` the
upload step expects (or writes per-file JUnit that is not merged into it), so the
artifact file is created empty/disconnected from the actual run. The render step
is a separate `pwsh` block in `.github/workflows/windows-validation.yml` (the one
after the Matrix-health gate) iterating `test_render_*.py`; the UI E2E step, by
contrast, produces a populated `ui-junit-merged.xml` via `run_ui_suite.py`.

**Impact:** cosmetic/reporting only — `render-suite.log` is authoritative and is
uploaded, the step's exit code correctly gates the job (a real render failure
still reds the build), and the 2 xpassed cases are fixed-bug xfails (better than
expected), not regressions. No test coverage is lost; only the machine-readable
JUnit summary for the render layer is missing.

**Test status:** not skipped/xfailed — the render tests run and gate normally;
this tracks only the empty JUnit artifact.

**Resolution path (DONE 2026-09-18):** the proposed path — "point the render
step's pytest at a single `--junitxml`" — turned out to be already true and not
the defect. What was actually done: the render step now validates the artifact
it uploads (`check_render_junit.py`, run after the scrub and before the
exit-code check) and backfills the root element's counts, so a zero-testcase or
partial artifact reddens the job instead of shipping green. A real
windows-validation run remains the only thing that can exercise that guard in
situ; the runner has been offline, and the guard's `--self-test` runs in the
fast-checks lane meanwhile.

---

## TEST-BACKLOG-SIGN-DIALOG-PLANNED-ROWS (2026-07-03)

> **Triage 2026-09-06:** FIX - filed as GitHub #142. This entry stays as the evidence record; the issue tracks the remaining work.

**Symptom:** Five matrix rows for the signature dialog remain `planned` (never
grounded to `green`/`blocked`) in `tests/ui_automation/matrix/ui_test_matrix.csv`:
`dlg_signDialog_signDialogScrollArea`, `dlg_signDialog_signDialogContent`,
`dlg_signDialog_certificateGroup`, `dlg_signDialog_signatureInfoGroup`,
`dlg_signDialog_appearanceGroup`. Flagged by the phase-6 karen gate of pipeline
9a4e2f71.

**Root cause:** these sub-widget rows were scaffolded (from the sign-dialog UIA
surface) but never given a driving test, so they sit at the default `planned`
status. They are outside the matrix-health green floor and its green->red check
(which only compares committed-`green` rows), so they do not affect the gate —
they are simply un-implemented coverage, not a regression.

**Impact:** none on the gate or on shipped functionality; the signature dialog
itself is exercised by the existing `test_ui_batch3_flows.py` behavioral flows
(/ByteRange, signing) and `test_ui_encryption_signatures.py`. This is a coverage
gap at the individual sub-widget level only.

**Test status:** rows are `planned` (not skip/xfail — no test drives them yet).

**Resolution path (ORIGINAL — WRONG, kept so the mistake is not repeated):**
"open the sign dialog via the existing `activateToolByObjectName`/
`testToolActivatePipe` seam, **assert each sub-widget is present/populated in
the UIA tree**, then promote the rows `planned`->`green`".

That cannot work. pywinauto's UIA backend **cannot enumerate a Qt app-modal
`QDialog::exec()`** — not `Application.windows()`, not `top_window()`, not
`Desktop(backend="uia")`. Measured in this project: a dialog sat open for four
minutes while a 240 s UIA poll reported nothing. A test written to that
instruction times out and reads as "the sign dialog is broken" when it is not.
Use `registerDialogIntrospection()` (SignDialog.cpp:244 already calls it; it
returns the child inventory, which is precisely what these rows need) or
`tests/helpers/modal_dialog.py`'s Win32 `EnumWindows` path. See CLAUDE.md,
"NEVER detect a dialog through UIA".

**STATE AS OF 2026-09-08 (pipeline backlog-workable-p82 phase-17):**

1. **The five named rows are no longer `planned`.** All five are committed
   `green` in `ui_test_matrix.csv`, promoted by a live run in commit `00494e96`
   ("P5d REMEASURE batch 3 — dialogs + sign-dialog backlog"), and the
   objectName contract is additionally pinned at unit-test level by
   `tests/test_sign_dialog_geometry.cpp`. The symptom above is stale; this row
   went on describing work that was already finished.
2. **The gate blindness the root cause describes was real and is now fixed.**
   It was never specific to these five rows: `check_matrix_health.py` filtered
   the committed matrix to `status == "green"` before every check, so ANY
   `planned` row was invisible to it — 26 of them at the time of writing. The
   gate could ratify finished work and could say nothing whatever about work
   not started. It now prints a status census and names every not-yet-done
   row on every run, `--max-planned N` ratchets the committed `planned` count,
   and `--fail-on-promotable` turns a not-yet-done row that records `green`
   into a reported unexpected pass rather than a silent one — which is exactly
   the failure that let point 1 above go unnoticed for two months.
   `ValidationLaneHygiene.MatrixHealthGateRatchetsPlannedRows` re-derives the
   real count from the CSV so the workflow's literal cannot drift from it.

The 26 remaining `planned` rows are un-started coverage, not regressions, and
are now listed by name in the gate's own output.

---

## CI-LIMITATION-WINDOWS-ASAN-NO-LEAK-DETECTION (2026-09-14)

**Status:** ESTABLISHED BY LOCAL MEASUREMENT. Not a defect to fix — a platform
capability that does not exist. GitHub #48 (SEC-20) is UPDATED with this
refutation and stays OPEN; it is **not** closed as fixed, because there is no
"residual leak finding count" to record when the feature is absent.

**Applies to:** both Windows ASan toolchains this repo has presets for —
`windows-vs2022-msvc-asan` (MSVC `cl` with the address sanitizer) and
`windows-vs2022-asan` (clang-cl with `-fsanitize=address`).

### What #48 asked, and why it cannot be satisfied as written

#48 asks to "dispatch `windows-sanitizer.yml` with `detect_leaks=1` and a
suppression file for Qt/MuPDF statics; record the residual finding count".

That LeakSanitizer is absent from the **MSVC** ASan runtime was already recorded
in `docs/security/windows-asan-leak-detection-decision.md`. What was *not*
established — and what the p85 phase-2 criterion narrowed the issue to — is
whether the **clang-cl** runtime behaves differently. It does not. Both Windows
ASan runtimes refuse `detect_leaks=1`.

This matters because the clang-cl preset was the last remaining candidate: it is
the same compiler family whose ASan runtime *does* implement LSan on Linux, so
"it works there" is a reasonable hypothesis to hold until measured. It is now
measured, on this host, and refuted.

### The experiment

Deliberately a **positive-control** experiment: the probe program leaks a block
that LeakSanitizer, if present, is obliged to report. A silent clean exit would
itself be a finding, so the run cannot pass vacuously.

`docs/audit/p85-remaining-open/phase-2/probes/leaky.cpp`:

```cpp
#include <cstdio>
int main() {
    int* p = new int[256];
    p[0] = 1;
    std::printf("alloc %d\n", p[0]);
    return 0;  // p intentionally never deleted
}
```

Commands (from a `vcvars64.bat` shell, with the clang ASan runtime directory
`...\VC\Tools\Llvm\x64\lib\clang\19\lib\windows` prepended to `PATH`, without
which the process fails to load its ASan DLL):

```
clang-cl -fsanitize=address -Od -Zi leaky.cpp -o leaky.exe

set ASAN_OPTIONS=detect_leaks=1
leaky.exe

set ASAN_OPTIONS=detect_leaks=0
leaky.exe
```

Reproduce with `docs/audit/p85-remaining-open/phase-2/probes/probe_leaks.bat`
(re-run from that tracked location on 2026-09-14 and it still reproduces
verbatim), which runs both cases as
separate invocations with stderr redirected per run — the two runs interleave
confusingly otherwise, and an earlier reading of the interleaved output was
briefly ambiguous about which case emitted which line.

### Output (verbatim, 2026-09-14)

```
=== clang-cl version ===
clang version 19.1.5
Target: x86_64-pc-windows-msvc
InstalledDir: C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin
=== compile ===
COMPILE_EXIT=0

=== CASE A: ASAN_OPTIONS=detect_leaks=1 ===
RUN_EXIT=1
==18696==AddressSanitizer: detect_leaks is not supported on this platform.

=== CASE B (control): ASAN_OPTIONS=detect_leaks=0 ===
CONTROL_EXIT=0
==21064==interception_win: unhandled instruction at 0x7ff6cc7427a4: 0f 29 74 24 30 0f 28 f0
alloc 1
```

### Reading it

- **Case A** is decisive: the runtime prints `detect_leaks is not supported on
  this platform` and **exits 1 before `main` runs** — note that `alloc 1` is
  absent. Setting `detect_leaks=1` does not produce a noisier or a quieter run;
  it prevents the process from running at all.
- **Case B** is the control that stops Case A being misread as a broken build:
  the same binary runs to completion, prints `alloc 1` and exits 0 — while
  leaking. So the program does leak, ASan is genuinely loaded and instrumenting,
  and the only variable between the two runs is the `detect_leaks` flag.

Conclusion: **LeakSanitizer is not implemented in the Windows ASan runtime of
either toolchain.** It is not a configuration or suppression-file problem, so a
Qt/MuPDF suppression file would change nothing. A 4–5 hour
`windows-sanitizer.yml` dispatch would return this same message once per test
process and was correctly judged unnecessary to establish it.

`detect_leaks=0` in `windows-sanitizer.yml` therefore stays, and remains
correctly documented at the site itself as a platform fact rather than a
noise-tuning choice.

### Incidental finding (not part of #48)

Case B emits `interception_win: unhandled instruction at ... 0f 29 74 24 30`
(a `movaps` store). That is the clang-cl ASan function-interception hook failing
to patch one function prologue on this host — a known upstream limitation of
ASan's x86-64 interception on Windows. It means a small number of intercepted
functions may go uninstrumented, so Windows ASan coverage is slightly narrower
than the flag implies. Recorded because it bears on how much a clean Windows
ASan run is worth. It is not tracked as a PDFE defect and was not investigated
further.

### Where leak detection does exist

Nowhere on Windows for this project. If leak coverage is ever required, the
options are the Linux/Hetzner sanitizer lanes (`sanitizer.yml`,
`hetzner-sanitizer.yml`), whose clang ASan runtime does implement LSan, or
Application Verifier / UMDH, which are a different instrument answering a
different question. Neither is scheduled, and neither is claimed here to be
equivalent coverage.

---

## PDFE-LIMITATION-TOUNICODE-ENRICHMENT-ON-SAVE — DOCUMENTED (2026-09-15)

**Tracked as:** GitHub #188. **Decision record:**
[`docs/tounicode-recovery-write-side-decision.md`](../../docs/tounicode-recovery-write-side-decision.md).
**Enforced by:** `tests/test_tounicode_recovery_write_side.cpp`
(`ToUnicodeRecoveryWriteSide.*`, 5 cases).

**The behaviour.** Opening a document whose simple fonts have **no authored
`/ToUnicode`**, and whose glyph names PDFE can resolve better than MuPDF can
(in practice: Adobe Expert-set names, which the AGL maps into the Corporate-Use
PUA), causes PDFE to install a synthesized `/ToUnicode` CMap into those font
dictionaries — in the **in-memory object graph**, from the `core::Page`
constructor. A subsequent save therefore writes font entries the input file did
not contain. The user is not told.

**Why it is not a defect.** It is the mechanism that closes the A3-48
redaction-by-search false negative (GitHub #23): without it, an Expert-set run
extracts as `U+F773 U+F765 …` and matches nothing, including nothing a
redaction search looks for. The alternative — an extraction-only side table —
would have to be consulted at all eleven `fz_new_stext_page_from_page` sites,
two of which are the redaction scrubber and the redaction verifier. §2 of the
decision record sets out the full trade-off and §5 the triggers for revisiting.

**The bounds a user can rely on** (each asserted, each mutation-verified):

* A font that carries an authored `/ToUnicode` is left **byte-identical**.
* A font whose glyph names carry no Unicode information gains **nothing** —
  PDFE never invents a mapping.
* Opening does **not** set the modified flag, so PDFE initiates no save of its
  own; the enrichment reaches disk only inside a save the user asked for.

**Not claimed:** that the added object is invisible to an incremental-update
diff. It is not, by design. A workflow that requires a save to change only what
was edited must treat this as a known, intended difference.

---

## PDFE-LIMITATION-NO-CTRL-CLICK-MULTISELECT

> **Recorded 2026-09-17, pipeline p87 phase-4. Cross-referenced on GitHub #221.**
> This replaces a stale `xfail` that blamed the harness for a product gap.

**Discovered:** while resolving GitHub #221, whose three matrix rows cited a
`P-GAP-92` that records RESOLVED.

**Affected rows:**
- `tests/ui_automation/matrix/ui_test_matrix.csv` — `canvas_gesture_multiselect`
  ("Ctrl+click on 2 annotations, both selected").

**Symptom:** the row was `status=xfail`, attributed first to the Qt6 synthetic-
event spontaneity barrier and then to Qt coalescing `SendInput` motion events.
Both explanations describe input delivery; neither is what is happening.

**Root cause — it is not a harness limitation at all.** PDFE does not implement
additive Ctrl+click annotation selection. `SelectTool::onMousePress` clears the
multi-selection unconditionally on a hit:

```
// src/annotations/SelectTool.cpp:292-296
if (hitIndex.has_value()) {
    // Clear multi-select
    selectedAnnotIndices_.clear();
    selectedAnnotIndex_ = hitIndex.value();
```

`event->modifiers()` is never consulted anywhere in the selection path (grep for
`ControlModifier` in `src/annotations/SelectTool.*` returns nothing). The only
producer of a multi-selection is the rubber band, at `SelectTool.cpp:384-396`.

**Why this entry exists rather than a test.** No test seam can make the row pass,
because there is nothing to drive: a test asserting "two annotations selected
after two Ctrl+clicks" would simply be a failing test. Writing one, or leaving
the xfail with an input-delivery rationale, both misreport a missing feature as
a tooling problem — and the second had already cost two investigations.

**Test status:** `canvas_gesture_multiselect` remains a non-result row, with the
CSV note corrected to name this entry. Its two siblings — `canvas_rubber_band`
and `canvas_scroll_wheel` — ARE resolvable and are now real asserting tests in
`tests/ui_automation/test_ui_canvas_gestures_mat2_live.py`.

**Resolution path:** implement additive selection in `SelectTool::onMousePress`
(honour `Qt::ControlModifier` by toggling the hit index in `selectedAnnotIndices_`
instead of clearing it), then the row becomes assertable through the existing
`selCount` view-state seam — the same oracle the rubber-band test already uses.

---

## CI-LIMITATION-SCANNED-FIXTURE-NOT-GLYPHLESS

> **Recorded 2026-09-17, pipeline p87 phase-4. Cross-referenced on GitHub #218.**
> This CORRECTS a previously-recorded cause that was wrong.

**Discovered:** by the recomposite-complete seam that GitHub #218 itself asked
for. The seam was added, and the first thing it measured contradicted the issue.

**Affected tests:**
- `tests/ui_automation/test_render_text_edit.py::test_scanned_page_text_edit_changes_canvas`
- `tests/ui_automation/test_render_images.py::test_mixed_dpi_region_not_smeared`
- `tests/ui_automation/test_render_oracle_proof.py::test_mixed_dpi_region_not_smeared`

**The cause that was recorded, and is wrong:** "the ImageReplacer recomposite
only engages after the OCR -> atlas chain completes, and that completion is not
observable from the test, so the screenshot is sampled at an unpredictable
point." The observability half was true and is now fixed. The implication —
that the recomposite happens and the test merely looks at the wrong moment — is
not true.

**Root cause.** `MainWindow` gates the recomposite on
`spanInfo.isOcrInvisibleLayer()`, which is `isGlyphlessOcrFontName(fontName)` —
it requires the span's font name to contain `glyphless`
(`src/textedit/TextEditModel.h:25-39`). Neither fixture satisfies it:

| fixture | fonts on page 0 |
|---|---|
| `tests/fixtures/test_scanned_with_ocr.pdf` | `Helvetica` (Type1) — **not** glyphless |
| `tests/fixtures/mixed_dpi_page.pdf` | none at all — no text layer |

**Measured 2026-09-17** by driving `testSpanActivatePipe` across the scanned
fixture's spans and reading the new `RECOMPOSITE_*` status: the page has 32
spans; 6 were probed and every one answered
`RECOMPOSITE_SKIPPED|span-not-ocr-invisible`. The recomposite is not late. It
never runs, under any coordinate, at any speed.

**A second, independent defect found at the same time.** The two mixed-DPI
probes compute `sharpness(before)` over `THUMB_REGION_BBOX` and compare
`sharp_after >= 0.85 * sharp_before`. The measured `sharp_before` is exactly
`0.0` — the crop captures nothing — so that comparison was `0 >= 0.0`, true for
any pair of blank images. It would have passed forever without measuring
anything. A `sharp_before > 0.0` guard is now in both tests and is what makes
this visible.

**Test status:** all three are `@pytest.mark.xfail(strict=True)`. Strict is
deliberate: the blocker is structural (a font name), so the tests cannot pass by
luck, and the marker turns red — demanding its own removal — the moment a
fixture is repaired. A `strict=False` marker never does that, which is how the
wrong cause above survived.

**What is NOT affected:** the per-region-DPI product path itself. It is
implemented (`Page::sourceImageDpiAt`) and covered by the gtest
`test_image_replacer_dpi` suite, which remains the authoritative oracle.

**Resolution path:** regenerate `test_scanned_with_ocr.pdf` with a genuine
Tesseract `GlyphLessFont` Tr=3 layer (its generator,
`tests/fixtures/make_test_scanned_with_ocr.py`, is named for a layer the
committed binary does not carry), give `mixed_dpi_page.pdf` a text layer, and
repair `THUMB_REGION_BBOX`. That is a cross-suite fixture change with its own
verification obligations, which is why it is tracked here rather than attempted
as a side effect of this phase.

### RESOLVED IN PART — 2026-09-25 (pipeline p93 phase-3)

Both fixtures now carry a genuinely glyphless font, generated with fontTools
rather than taken from Tesseract (its `pdf.ttf` is not installed by vcpkg —
on this tree it exists only inside a buildtrees scratch directory that no clean
checkout or CI runner has). Shared definition:
`tests/fixtures/glyphless_font.py`.

| test | was | now |
|---|---|---|
| `test_render_text_edit.py::test_scanned_page_text_edit_changes_canvas` | xfail(strict) | **PASSES** — marker removed after it reported XPASS(strict) |
| `test_render_images.py::test_mixed_dpi_region_not_smeared` | xfail(strict) — GitHub #262 | **marker removed; PASSES** (p94 phase-3). #262 and #266 both fixed; the assertion now measures the property the test is named for — see below |
| `test_render_oracle_proof.py::test_mixed_dpi_region_not_smeared` | xfail(strict) — GitHub #262 | **marker removed; PASSES** (p94 phase-3), same as its sibling above |

**Both mixed-DPI probes now run and pass**, after three product fixes and two
oracle corrections. Recorded together because the oracle corrections are the
part most likely to be mistaken for weakening:

* **The crop was measuring the grey margin.** `crop_pdf_bbox` re-derived PDFE's
  page position in Python and returned 196×196 px containing none of the
  thumbnail; the long-cited "sharpness 2.18" was the contrast step between
  margin and paper. It now asks `PdfCanvas::getPageScreenRect()` through
  `testPageRectQueryPipe`. `test_render_oracle_proof.py` held a verbatim COPY of
  the old helper, so fixing the shared one did not fix it — it now delegates,
  and there is one implementation.
* **The `sharp_after >= 0.85 * sharp_before` assertion was replaced, not
  loosened.** It was never once evaluated against this region (see above), and
  it is confounded by text length: measured 2026-09-30 over a fixed crop,
  replacing the 9-character `THUMBNAIL` with a 9-character word gives a ratio of
  0.809 and with a 4-character word 0.605 — a 0.2 swing from word length alone,
  adjudicating a 0.15 blur budget. Both probes now assert the named property
  exactly, on the document the product WRITES: the image under the region keeps
  its native 300×300 px in a 144 pt box, i.e. still 150 DPI, so no upsample to
  the page maximum occurred.

**Residual, honestly stated and not asserted away:** replacement text is still
softer than the original. The FreeType fallback renders glyphs at the SCAN DPI
(300) and the compositor then scales them to the composite DPI (150), so they
are drawn at 2× and downsampled. Rendering at the target DPI needs per-DPI glyph
caching in `EnrichedAtlas`; recorded on GitHub #266 rather than bodged.

**The "second, independent defect" above does not reproduce.** `sharp_before`
measures **2.18**, not `0.0` — and it measures 2.18 with the OLD fixture as
well, so it was never fixture-dependent and nothing in this change addressed
it. It is recorded here as not-reproducing rather than fixed. The
`sharp_before > 0.0` guard stays in both tests and is what will catch it if it
returns.

> **STALE — corrected 2026-10-23, audit `p99-issue-audit` phase 4.**
> **GitHub #262 is CLOSED**, since **2026-09-29** (real date, from the retired
> repository; the live `closed_at` is synthetic — see the #141 correction
> above). The sentence below says it is "what now blocks" these probes, which
> was true when written and has not been for about a month. The measurement and
> the reasoning below are left intact; only the live-state claim is corrected.

**What now blocks the two mixed-DPI probes is new: GitHub #262.** With the OCR
layer present the edit reaches a terminal `RECOMPOSITE_DONE` with a fresh
sequence number — and the 150-DPI region's pixels do not change
(`changed=False`, sharpness `2.18 -> 2.18`). Control, same run with the old
fixture: the test dies earlier at "no `RECOMPOSITE_*` outcome with sequence > N
appeared within 90s", i.e. the edit never reaches `refreshImageForScannedPage`.
So reaching DONE is genuinely new behaviour.

**DETERMINED, and it is NOT #2 (p94 phase-3).** The question left open above —
whether #262 was the per-region-DPI defect or a separate write-back failure —
has an answer, and it is the second. #2 predicts a changed-but-smeared region;
the observable was no change at all, which the smearing oracle cannot separate
from it while nothing changes.

The cause is a single rule. `PdfContentWriter::findPageImageXObject()` chose the
write-back target by pixel area alone — *"Find largest image XObject"* — and
never asked where the edit was. On this page `/fzImg1` (300×300, the 150-DPI
thumbnail) is drawn **after** `/fzImg0` (2125×2500) and sits wholly inside it,
so an edit over the thumbnail was composited into the photo beneath and painted
over. `replacePageImage()` returned true, so `RECOMPOSITE_DONE` was published
for a committed, invisible write.

Per-region DPI was already correct throughout — and pointedly,
`Page::sourceImageDpiAt()` was *already* doing the region-aware image selection
this writer lacked. The two functions disagreed about which image a region
belongs to: one by coverage and topmost-ness for DPI, the other by raw pixel
area for write-back.

Fixed by `findImageXObjectForRegion()`, placement-relative coordinate mapping
(the old arithmetic assumed the target image spans the page; `/fzImg0` covers
only `Rect(51,51,561,651)` of a 612×792 page), and a post-write comparison of
the **rendered** region so an invisible recomposite reports `RECOMPOSITE_FAILED`
rather than `RECOMPOSITE_DONE`.

Headless oracles: `tests/test_recomposite_writes_visible_pixels.cpp`.
Full analysis:
[`docs/audit/dca/0105-a-success-reported-without-checking-the-thing-it-claims.md`](../../docs/audit/dca/0105-a-success-reported-without-checking-the-thing-it-claims.md).

Full gtest suite after the fixture change: **6399 tests, 6377 passed, 0
failures, 0 errors, 22 skipped** — no regression from either regenerated
fixture.

---

## PDFE-LIMITATION-PAGEPANEL-SHIFT-RANGE-SELECT — RESOLVED 2026-09-21

> **Recorded 2026-09-17, pipeline p87 phase-4.** Found while giving GitHub #222's
> two rows a real oracle. Cross-referenced on #222 and filed as GitHub #226.
>
> **RESOLVED 2026-09-21, pipeline p88 phase-3.** Hypothesis 1 below was correct.
> `PagePanel::setCurrentPage()` now moves the current index with
> `QItemSelectionModel::NoUpdate` when a real multi-page selection exists and
> already contains the target, instead of the implicit `ClearAndSelect` that
> `setCurrentRow()` carries. The `xfail(strict=True)` marker is REMOVED, as it
> was written to demand; the test asserts a contiguous three-page run and is
> expected to pass. Headless companion:
> `tests/test_page_panel_range_selection.cpp`, whose mutation reproduces the
> exact collapse (`{0,1,2}` → `{2}`). Analysis: `docs/audit/dca/0086-*.md`.
>
> Kept rather than deleted because the measurement table below is the evidence
> that ruled out the harness, and because the "three independent input paths all
> measured [2]" finding is the thing that pointed away from the gesture and
> toward the round trip.

**Affected test:** `tests/test_ui_sendinput.py::test_sendinput_shift_click_rangeselect`
(marker removed; previously `xfail(strict=True)`).

**Symptom:** Shift+click on a PagePanel thumbnail selects **only the clicked
page**. It does not extend the selection from the anchor, so a user cannot
range-select pages to delete, extract or rotate them as a block.

**This was invisible because the test was not asserting.** Its oracle was
`assert session.proc.poll() is None` — the process is still running — which
passes whether the gesture selected three pages, one page, or none. There was
no way to do better: a pipe existed to SET a page selection
(`testSelectPagesPipe`) and none to READ one. The readback added for GitHub #222
(`testSelectedPagesQueryPipe` / `testSelectedPagesResultPipe`) is what made the
defect observable, and it surfaced on the first run.

**Measured 2026-09-17** on `tests/fixtures/test_multipage.pdf` (5 pages). Each
row is preceded by a plain click on item 0, which correctly yields `[0]`:

| gesture | input path | selection after |
|---|---|---|
| Shift+click item 2 | `send_modified_click`, modifier batched into one `SendInput` | `[2]` |
| Shift+click item 2 | Shift down / click / Shift up as separate `SendInput` calls with elapsed time between | `[2]` |
| Shift+click item 2 | pywinauto `click_input(pressed="shift")` — a different input path entirely | `[2]` |
| Shift+click item 1 (adjacent) | `send_modified_click` | `[1]` |
| **Ctrl+click item 2 (CONTROL)** | `send_modified_click` | **`[0, 2]`** ✅ |

The Ctrl control is what rules out the harness. Modifier delivery works; the
same helper, the same session, the same list — Ctrl extends the selection and
Shift does not.

**Root cause: NOT established.** `PagePanel`'s list is
`QAbstractItemView::ExtendedSelection` (`PagePanel.cpp:98`), under which Qt
implements Shift-select natively, so something is interfering. Two candidate
paths are worth examining first, and both are **hypotheses, not findings**:

1. `PagePanel::setCurrentPage()` (`PagePanel.cpp:186-189`) calls
   `listWidget_->setCurrentRow()` on the `pageSelected` → MainWindow-navigate →
   panel-resync round trip. Shift-select extends from the selection model's
   ANCHOR, and moving the current row between the two clicks would collapse the
   range to the clicked item — which is exactly the observed result.
2. The thumbnail refresh path (`PagePanel.cpp:279-304`) does
   `listWidget_->clear()`, regenerates, and restores the selection with
   `setSelected(true)` — but never restores the current index, so any refresh
   between the two clicks destroys the anchor.

Ctrl+click is consistent with either hypothesis: a toggle does not consult the
anchor.

**Resolution (2026-09-21): hypothesis 1, confirmed by mutation.** It was the
`pageSelected` → MainWindow-navigate → panel-resync round trip. What the
write-up above did not spot is *why* it left no trace: the `QSignalBlocker`
around that `setCurrentRow()` call suppresses the re-emitted signal but not the
selection change, so the line reads as already-defended.

Hypothesis 2 (the thumbnail refresh clearing the anchor) was **not** the cause
here — no refresh occurs between the two clicks — but the observation stands on
its own and is worth keeping: that path still restores the selection without
restoring the current index.

The test's assertion needed no change. It already asserted a *contiguous* run
rather than a count of three, so an additive gesture that happened to pick three
could not have satisfied it.
