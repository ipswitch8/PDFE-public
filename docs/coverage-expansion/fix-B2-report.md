# Fix report — B2: page-struct undo/redo + saveAs teardown UAF (0xC0000005)

**Defect id:** B2 (coverage-expansion generative harness)
**Lock:** `tests/test_generative_invariants.cpp :: GenerativeInvariants.B2_PageStructUndoRedoSaveAsNoCrash`
(was `DISABLED_B2_...`; now ENABLED and passing).
**Class:** use-after-free / access violation on teardown.

---

## 1. Precise root cause

### Symptom
Running the committed deterministic repro (pagestruct alphabet, seed 2000, len 12) — a
page insert/delete/rotate sequence with interleaved undo/redo that leaves a
`DeletePageCommand` on the undo stack in its *executed* (page-deleted, holder-retained)
state — then `doc->saveAs(out)`, reopen, and teardown, faults with:

```
unknown file: error: SEH exception with code 0xc0000005 thrown in the test body.
```

### The faulting object — and why it faults
The fault is **NOT** the retained page object being invalidated by `saveAs`. That hazard was
already closed: `Document::retainPageObj()` deep-copies the page (`pdf_graft_page`) into its
**own standalone `pdf_document`** held by a `RetainedPageHolder`, living in the shared
`MuPdfContext`. That holder is document-independent and provably survives a full-rewrite
`saveAs`/`save` (see `tests/test_undo_after_save_crash.cpp`).

The real fault is a **teardown-ordering use-after-free on the owning `Document`**:

- `DeletePageCommand` (and `MovePageCommand`, `DeletePageRangeCommand`, `ReplacePageCommand`)
  hold a raw `pdfe::core::Document* doc_`.
- Their **destructors** released the retained holder via `doc_->releasePageObj(...)`
  (`src/core/PageCommands.cpp`), and `Document::releasePageObj` dereferences
  `pImpl->context` to drop the holder.
- In the B2 repro the `Document` is destroyed (`doc.reset()`) **before** the `UndoManager`
  and its command stack destruct at end of scope. So when `~DeletePageCommand` runs,
  `doc_` points at freed memory; `doc_->releasePageObj` → `pImpl->context->execute(...)`
  reads the freed `Document::Impl` → **0xC0000005**.

The retained holder itself never needed the `Document` — only an `fz_context` — so routing
its release through a possibly-dead `Document*` was the bug.

### This is a real product crash, not a test-only artifact
`MainWindow` owns `document_` as a `std::unique_ptr` **member** (`MainWindow.h:692`) but
`undoManager_` is a **`QObject` child** (`undoManager_ = new UndoManager(this)`,
`MainWindow.cpp:2127`; raw ptr member at `MainWindow.h:700`). On `MainWindow` destruction,
member destructors run first — freeing `document_` — and only then does `~QObject` delete the
child `UndoManager`. Any page-delete/move/replace command still on the undo stack at close
therefore destructs **after** `document_` is gone → the same dangling-`doc_` UAF. Closing the
app (or otherwise destroying the main window) with an un-cleared page-structure undo command
is enough to hit it.

---

## 2. Fix (minimal, correct — no Option-B refactor needed)

A minimal, safe fix exists and was implemented; the large ~19-class Option-B Page-ownership
refactor is **not** required for B2 (that refactor addressed a *different* hazard —
`Page*`-holding families across a full-rewrite save — and is already landed).

Make the retained-holder release **Document-independent** so command destructors never touch a
possibly-freed `Document`:

- **`src/core/Document.cpp`** — `RetainedPageHolder` gains a
  `std::shared_ptr<MuPdfContext> context;` member, pinned in `retainPageObj()` at retain time.
  The holder now keeps its own context alive for its whole lifetime.
- **`src/core/Document.cpp`** — new `static void Document::releaseRetainedPage(void* obj)`
  drops the holder via the holder's **own** pinned context (`holder->context->execute(...)`),
  never via any `Document`. `Document::releasePageObj()` now delegates to it (single code path).
- **`src/core/Document.h`** — declares the new `static` method (with a comment documenting the
  B2 teardown-order hazard).
- **`src/core/PageCommands.cpp`** — the four destructors
  (`~DeletePageCommand`, `~MovePageCommand`, `~DeletePageRangeCommand`, `~ReplacePageCommand`)
  now call `pdfe::core::Document::releaseRetainedPage(obj)` instead of
  `doc_->releasePageObj(obj)`. `ReplacePageCommand`'s destructor also drops its old
  `&& doc_` guard (no longer relevant).

`undo()`/`redo()` still use `doc_->releasePageObj(...)` — safe, because those only ever run
while the `Document` is alive (they operate on the live document).

### Why it is safe (no dangling handle survives to teardown)
- The holder is a standalone `pdf_document` in the shared `fz_context`; dropping it requires
  only an `fz_context`, which the holder now owns a `shared_ptr` to. Release cannot outlive its
  context, and needs no `Document`.
- Destructors no longer dereference `doc_` at all, so the teardown-order UAF is structurally
  impossible regardless of whether the `Document` or the `UndoManager` is destroyed first.
- No behavior change on the live-edit paths: `retainPageObj`/`insertPageObj`/undo/redo are
  unchanged; only the *release* routing moved off the raw `Document*`.
- `releasePageObj` still works for all existing callers (it delegates).

### Undo-after-save still works
The fix does not touch undo semantics or the retain/re-graft mechanism. `test_undo_after_save_crash.cpp`
(delete → saveAs/in-place-save → undo restores the page) exercises exactly that and remains a
first-class regression. Undo is **not** disabled, cleared, or weakened anywhere.

---

## 3. Lock status

`GenerativeInvariants.DISABLED_B2_PageStructUndoRedoSaveAsNoCrash` →
`GenerativeInvariants.B2_PageStructUndoRedoSaveAsNoCrash` (DISABLED_ prefix removed). The exact
deterministic repro (seed 2000, len 12) and the no-crash / reopen assertions are unchanged.

- Before fix: `--gtest_also_run_disabled_tests --gtest_filter=*DISABLED_B2*` → `SEH exception with code 0xc0000005 thrown in the test body`, FAILED.
- After fix: `--gtest_filter=GenerativeInvariants.B2_PageStructUndoRedoSaveAsNoCrash` → `[ OK ]` / `[ PASSED ] 1 test` (no access violation). In the XML: `<testcase name="B2_PageStructUndoRedoSaveAsNoCrash" ... status="run" result="completed" />` (not disabled).

---

## 4. Tight regression result

**Green.** One bounded run of the B2-central logic suites (page-structure commands,
undo/redo, command-page rebind, undo-after-save, file-lock, annotation commands, and the
full generative harness incl. B2):

```
GenerativeInvariants.* : UndoAfterSaveCrashTest.* : CommandPageRebindTest.* :
PageCommandTest.* : PageOperationsTest.* : PageTest.* : UndoManagerTest.* :
ReplacePageTest.* : ReplaceUndoTest.* : Phase6PageOpsTest.* : CompositeCommandTest.* :
AnnotationCommandTest.* : TreUndoAfterSaveCommandTest.* : TreUndoAfterSavePrimitiveTest.* :
DocumentSaveFileLockTest.*
```

Result: **118 tests / 15 suites, `[ PASSED ] 118 tests`, exit 0**; XML
`testsuites tests="118" failures="0" disabled="0" errors="0"`.

`UndoAfterSaveCrashTest` (4/4 OK) proves undo-after-save is intact; `DocumentSaveFileLockTest`
(4/4 OK, incl. `HeldPageLocksFullRewriteSave` / `DroppingPageBeforeSaveUnlocksIt`) proves the
prior save-lock behavior is unchanged.

### Note on the broad substring filter (pre-existing, unrelated crash)
The broad `*PageOp*:*PageCommand*:*Save*:*Undo*:*Document*:*Redo*:GenerativeInvariants.*`
filter (≈646 tests across 170 suites) pulls in many Qt-widget / MainWindow / canvas tests and
hits the **known, pre-existing, non-deterministic in-process crash** — it died at a *different*
late test every run (`Phase9ChromeTest.KB07_DirtyDocumentDiscardClosesWindow`, then
`PdfCanvasKeyboardScroll`, then `TreCommandsTest`, then `DocumentSaveAtomicTest`), and **each of
those passes in isolation**. That flake is unrelated to this core-library change (it lives in
QApplication/widget teardown and filesystem-permission stunts), and per the task guidance it was
not chased. The curated 118-test logic run above is the reliable, bounded green.

