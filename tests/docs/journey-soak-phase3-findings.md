# Journey-soak Phase 3 — B-1 resolution (render-lifetime hazard)

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


Pipeline **f4a82c1e**, Phase 3. Charter: *resolve B-1 at the code level* — the
render-lifetime race carried forward, unreproduced, from the 3e91a7c4 closure
doc: "`TileRenderer::cancelAll()` sets each task's cancelled flag and clears the
queue but never `waitForDone()`/joins in-flight workers → a scroll that drops
the page tree while a worker is inside `fz_run_display_list` → potential UAF,
and (per the redaction-save comment) an in-flight worker's display list keeps
the source file locked across a save."

Phase 2 could not reproduce B-1 across ~48 armed edit→scroll rounds; Phase 3
went to the code and to deterministic Document-level probes.

## TL;DR

**B-1, as framed, is refuted.** Tile-render workers are save-safe and cannot
UAF the page tree. The genuine residual hazard is *different*: a live **Page**
object held across a **full-rewrite `Document::save()`** locks the source file on
Windows and makes the save throw (lost changes). That hazard is **latent** (not
hit by any Phase-2 journey) and its safe remediation on the ordinary File>Save
path is a **design decision**, not a mechanical patch — see §4.

A deterministic regression, `tests/test_document_save_file_lock.cpp`, pins the
whole mechanism (4 arms, all green).

## 1. Ownership analysis — why there is no page-tree UAF

`TileRenderer::requestTile()` builds a **self-contained `fz_display_list`** on
the calling thread (`Document::createDisplayList`, serialized by `docMutex`) and
hands ownership to the `RenderTask`. The worker thread renders **that display
list** — it never dereferences the live `pdf_page` / page tree
(`src/rendering/TileRenderer.cpp`, header comment: "decouples document access
from pixel rendering"). A `fz_display_list` is an immutable, reference-counted
capture; dropping the page tree does not invalidate it. So "scroll drops the
page tree while a worker runs → UAF" cannot occur through this path. The worker
touches only its own display list and a `shared_ptr<MuPdfContext>` (kept alive
for the worker's lifetime). `~TileRenderer()` additionally `cancelAll()` **then**
`m_threadPool->waitForDone()`, so teardown already joins.

## 2. Empirical mechanism — what actually locks a save

`Document::save()` full-rewrite path (`src/core/Document.cpp`): write temp →
`fz_drop_document` (its own handle) → `remove()` the original (10×100 ms retry
on Windows) → `rename()` temp over it → reopen. The `remove()` fails while **any
other holder** keeps the source `fz_document` alive (its OS file handle stays
open); the retries exhaust and `save()` throws, after reopening the *pre-edit*
file — a silent lost-changes outcome, and any still-held Page is now stale.

`test_document_save_file_lock.cpp` establishes the ground truth deterministically:

| Arm | Holder across a full-rewrite `save()` | Result |
|---|---|---|
| `FullRewriteSaveSucceedsWithNoHolder` | none | **OK** (control) |
| `HeldDisplayListDoesNotLockSave` | a display list (== what a render worker owns) | **OK** — *render workers are save-safe* |
| `HeldPageLocksFullRewriteSave` | a `Page` (`fz_load_page`→`fz_keep_document`) | **throws** after retry exhaustion (~1 s) |
| `DroppingPageBeforeSaveUnlocksIt` | Page dropped before save | **OK** — the remedy |

This **refutes** two prior claims:
- the 3e91a7c4 closure doc's B-1 render-lifetime / file-lock framing, and
- the redaction-save comment in `MainWindow.cpp` that "display lists keep the
  document alive via `fz_keep_document`." They do not; the redaction path's
  *tile-renderer drain* is harmless belt-and-suspenders. Its **Page-holder
  drop** (`toolPage_.reset()` + `commandPages_.clear()`) is what actually
  prevents the lock there.

## 3. Reachability of the real hazard

The locker is a live `Page`. In `MainWindow` the persistent Page holders are
`toolPage_` (set when an editing tool is active) and `commandPages_` (which
**accumulates** command-scoped Pages across the whole session — cleared at only
three sites: `closeDocument`, `onDocumentStructureChanged`, and the redaction
path). The ordinary `onSaveFile` / `onSaveAsFile` do **not** drop these holders
before `document_->save()`; they rely on the *post-save* `onDocumentStructureChanged()`
to clear them — too late to prevent the lock.

So the hazard is reachable when a full-rewrite-class edit (text content edit,
page op, image/EXIF edit, …) leaves `toolPage_` or `commandPages_` non-empty at
save time. It is **latent**: every Phase-2 journey save passed I-4
(save→reopen→re-extract) because those sequences either routed through the fast
incremental-append path (additive annotation/form edits — no `remove`+`rename`)
or fired `onDocumentStructureChanged()` (clearing the holders) before the save.

## 4. Why the ordinary-save fix is a design decision, not a patch

The redaction path drops `toolPage_` + `commandPages_` **and** clears
`undoManager_` before saving — it can, because redaction is irreversible.
Ordinary Save must **preserve undo**. But undoable commands hold **raw `Page*`
pointers into `commandPages_`** (e.g. `auto* pagePtr = commandPages_.back().get();`),
so dropping `commandPages_` before an ordinary save would dangle those pointers
and could crash a subsequent undo. (Note: the existing *post-save*
`onDocumentStructureChanged()` already clears `commandPages_` after every
full-rewrite save, so an "undo across a full-rewrite save" question may already
exist independently — worth confirming.)

Fix options, in ascending scope:

- **A — minimal / safe / precedented.** Before an ordinary save, clear the
  active tool + `toolPage_.reset()` only (the redaction path's *safe* subset;
  tools are recreatable and `toolPage_` is already reset post-save). Closes the
  common *edit-with-active-tool → Save* case. Residual: `commandPages_`-held
  Pages.
- **B — comprehensive.** Investigate the command/undo/Page-ownership contract;
  make commands hold page *indices* (or re-resolve) rather than owning `Page`
  refs, so all holders can be released before save with undo intact. Larger;
  needs its own tests.
- **C — defer.** Ship this refutation + regression + this writeup; schedule the
  fix as its own pipeline once scope A vs B is chosen.

## 5. Delivered this phase

- `tests/test_document_save_file_lock.cpp` — deterministic 4-arm regression
  locking in the corrected mechanism (render workers save-safe; live Page is the
  locker; dropping it is the remedy).
- This document — refutes B-1's render-lifetime framing on both counts, records
  the true hazard, its latency, and the fix fork.
- Reverted the Phase-3 first-pass scaffolding (a `TileRenderer` render-entry
  test seam + a `saveWithRenderDrain` helper) built on the *refuted* hypothesis
  that draining render workers protects the save — it does not.

**Recommendation:** the render-lifetime B-1 is closed (refuted, with a
regression proving render workers are save-safe). The live-Page save-lock is the
real residual; pick scope A or B for its remediation.

## 6. Resolution — Option B, implemented (Phase 3a)

User chose **Option B** (comprehensive, undo intact). Implemented:

- **Command base rebind contract** (`src/core/Command.h`): `virtual int
  pageIndex() const` (default -1) + `virtual void rebindPage(core::Page*)`
  (default no-op). `UndoManager::forEachCommand()` exposes both stacks.
- **Save orchestration** (`src/core/SaveWithRebind.h`): `detail::
  drainRebindAroundWrite` — snapshot each command's page index (while live),
  release ALL Page holders (unlock), write, re-resolve the needed pages into
  fresh holders, rebind the surviving commands. Wrapped by
  `saveWithCommandPageRebind` (Document::save) and `saveAsWithCommandPageRebind`
  (Document::saveAs).
- **All 18 Page*-holding undo command classes override the contract**
  (`page_ ? page_->index() : -1` / `page_ = page`): Annotation 3, Image 5,
  Form 6, Signature 1, **Redaction 3**. The redaction MARK commands
  (`CreateRedactionCommand` etc.) ride the ordinary undo stack (mark a
  redaction, then File>Save), so they need the contract like any other family;
  the irreversible **Apply Redactions** flow is separate (`onApplyRedactions`
  clears `commandPages_` + the undo stack before its own save, so it never
  reaches the orchestration). `TabOrderDialog` also holds a raw Page* but is a
  modal dialog, never on the undo stack across a save — correctly excluded.
- **Wiring**: `MainWindow::onSaveFile` and `onSaveAsFile` reset `toolPage_`
  pre-write and route the ordinary path through the orchestration, then
  `onDocumentStructureChanged(preserveCommandPages=true)` refreshes caches
  without dropping the rebound pages. The needFlush/closeDocument paths clear
  `commandPages_` + write directly (closeDocument clears the undo stack).

Result: the file-lock / lost-changes hazard is removed for **every** family
(the drain unlocks the write regardless of which family holds a page), and undo
survives an in-place Save **and** a Save As (the rebind).

**Tests**: `tests/test_command_page_rebind.cpp` (RED→GREEN + multi-page +
generic base-contract + Save As, all at the Document/command layer) and
`tests/test_ui_save_lock_rebind.py` (native pywinauto: onSaveFile in-place save
with a held command page + full-rewrite content edit — no lost change, undo
survives). The original 4-arm `test_document_save_file_lock.cpp` still pins the
underlying mechanism.
