# Undo: PagePanel Refresh Gap

**Status:** open production bug
**Discovered:** phase-7 (P1 Page Operations)
**Severity:** Medium — visual staleness, not data corruption

## Symptom

After a page-structure change (Insert Blank Page, Delete Selected
Pages, Move Page Up/Down, Insert Pages from File, Extract Pages) the
document model updates correctly and the PagePanel dock refreshes to
show the new page list. When the user presses Ctrl+Z (Undo), the
document model reverts — the undo stack goes clean, the window title
dirty marker `*` is removed, rendering is refreshed via
`canvas_->refreshAllPages()` — but the **PagePanel thumbnails still
show the post-structural-change page list**.

Concrete reproduction:

1. Open any multi-page PDF.
2. Pages > Insert Blank Page. PagePanel shows 4 items (was 3).
3. Edit > Undo. Window title loses the `*`. PagePanel still shows 4
   items.

The document model is correct — a subsequent save writes 3 pages,
File > Close + reopen shows 3 pages — but the PagePanel is stale
until the next structural change forces a refresh.

## Root Cause

`MainWindow::onUndo()` at `src/ui/MainWindow.cpp:1618`:

```cpp
void MainWindow::onUndo()
{
    if (undoManager_) {
        undoManager_->undo();
        // ... image-edit-stack restoration ...
        if (canvas_) canvas_->refreshAllPages();
        updateWindowTitle();
    }
}
```

It calls `refreshAllPages()` (canvas rendering) and
`updateWindowTitle()` (title dirty marker) but not
`onDocumentStructureChanged()`, which is the method that rebuilds
the PagePanel's QListWidget:

```cpp
void MainWindow::onDocumentStructureChanged()
{
    // clears stale page cache, refreshes page count, invalidates
    // tile cache, REFRESHES THUMBNAILS, clamps current page, ...
}
```

The forward path (execute a page command via the Pages menu) calls
`onDocumentStructureChanged()` explicitly at the end of each handler
— e.g., `onInsertBlankPage()` at line 3578. The undo path doesn't.
`onRedo()` has the same gap.

## Fix

Add the structure-change call to both slots:

```cpp
void MainWindow::onUndo()
{
    if (undoManager_) {
        undoManager_->undo();
        // ... image-edit-stack restoration ...
        if (canvas_) canvas_->refreshAllPages();
        onDocumentStructureChanged();  // ← add
        updateWindowTitle();
    }
}
```

And same for `onRedo()`. `onDocumentStructureChanged()` is idempotent
and cheap, so unconditionally calling it on every undo/redo is safe.

The alternative — having each page-command's `undo()` method emit a
structure-changed signal — is more invasive and would break the
command-pattern encapsulation. The MainWindow-level fix is correct.

## Test Implications

Phase-7's `_op_undo_insert`, `_op_undo_delete`, and `_op_redo_delete`
handlers work around this by using the **window title dirty marker**
as the post-undo oracle instead of the PagePanel ListItem count. The
title is refreshed correctly by `updateWindowTitle()` inside
`onUndo`; only the PagePanel is stale.

Once this bug is fixed:
1. Remove the "uses title oracle because of panel-refresh bug"
   notes from `_op_undo_*` handlers.
2. Restore the PagePanel count oracle for `_op_undo_delete` /
   `_op_undo_insert` — it's a stricter check.
3. Phase-7 will tighten its oracles but the currently-green rows
   should remain green.

## Related

- `src/ui/MainWindow.cpp:1618` — `onUndo`
- `src/ui/MainWindow.cpp:1649` — `onRedo`
- `src/ui/MainWindow.cpp:2059` — `onDocumentStructureChanged`
- `src/core/PageCommands.cpp` — per-command `undo()` implementations
  (none trigger a UI refresh, correctly; UI refresh is the caller's
  responsibility)
