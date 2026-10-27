# Canvas Click Harness Gap

<!-- tracking-banner -->
> **STATUS: HISTORICAL EVIDENCE — this is not a work list.**
>
> **GitHub Issues (`ipswitch8/PDFE`) is the canonical tracker for anything still
> open.** This document records what an audit found on the day it ran and what
> was decided about it. Rows here may have been fixed, superseded, or
> deliberately declined since. Where a row and a GitHub issue disagree, **the
> issue wins**; where a row describes something still broken and no issue
> exists, file one.
>
> See [`docs/TRACKING.md`](TRACKING.md) for why, and for the parts of this repo that are
> operationally live rather than historical.

---


**Status:** resolved
**Discovered:** phase-5 (P1 Annotation Tool Coverage)
**Resolved:** phase-10 / phase-4 (canvas-coord investigation)
**Affects:** phases 5, 9, 10 — any UI test requiring a canvas click to commit
                a content change.

> **Downstream promise NOT kept — gap A5-83, restated 2026-08-03 (F19).**
> `docs/archive/text-formatting-feature-gaps.md:76-89` made the *document-state
> verification oracle* ("path 2": apply formatting → commit → Save As →
> reopen → inspect the saved content stream for `Tf`/`Ts`/`Tm` with the
> expected arguments) conditional on **exactly this** harness gap being
> resolved: *"path 2 available once the canvas-click harness gap lands."*
> The gap landed. **Path 2 was never built.**
> `grep -rln "content_stream\|reopen_and_verify\|save_and_reload"
> tests/ui_automation/*.py` returns nothing, and
> `tests/ui_automation/test_text_formatting_p2.py:6,125` still declares
> toolbar/UIA widget state as its oracle. The **8 toggle + 4 font + 5 size
> rows are green under the weaker oracle** the archived document itself called
> "honest but not end-to-end". Nothing anywhere records that those greens are
> weaker than the rows around them, because the matrix CSV has no
> oracle-strength column. **This is a stated, open gap — not a closed one.**

## Resolution

### Root Cause

The gap was **not** in click delivery.  Synthetic clicks via
`canvas.click_input(coords=(x, y))` DO reach Qt's event loop and DO
invoke `PdfCanvas::mousePressEvent` → `currentTool_->onMousePress()`.
The phase-5 investigation proved this with `qDebug` instrumentation: the
log line in `StickyNoteTool::onMousePress` fired every time.

The actual bug was in the **oracle**, not the gesture:

1. **Window title dirty marker** — `onAnnotationCreatedByTool` in
   `MainWindow` was not calling `updateWindowTitle`, so the `*` dirty
   indicator never appeared after a synthetic annotation create.  The
   annotation existed in the document model but the oracle returned a
   false-green "no change".

   Fix (product level, `src/ui/MainWindow.cpp`): `onCreate`, `onUndo`,
   and `onRedo` all call `updateWindowTitle` / `onDocumentStructureChanged`.

2. **`commentTreeWidget` tree-item count** works as an oracle — it counts
   `TreeItem` descendants in the `commentTreeWidget` dock, which increases
   by one for each committed annotation regardless of the dirty-marker
   state.

### Proven Tests

| Test | File | Proof |
|------|------|-------|
| `_op_annot_move_drag` | `test_canvas_coords_p2.py` | StickyNote created via `canvas.click_input`; `commentTreeWidget` tree count goes from N to N+1. |
| `_op_click_away_deselect` | `test_canvas_coords_p2.py` | Same oracle; click-away does not remove the annotation. |
| All 5 annot create rows | `test_annotations_p1.py` | green |

### Remaining Gaps (unrelated to click delivery)

- **Painted handle drag** — ~~resize/rotate handles are painted at canvas
  pixel coordinates not discoverable via UIA. Oracle needed: an
  `annotRectLabel` in `AnnotationPropertiesWidget` or a test-only API.~~
  **STALE, corrected 2026-08-03 (F19, gap A5-15).** A test-only API was in
  fact built: `canvas_annot_handle_drag_corner`, `_edge` and `_rotate` are all
  **green**, recovered "via C++ handle-centroid seam
  (`selectByIndex` + `handleCentroidsDebug`)" per the matrix notes. The
  `annotRectLabel` widget was never built, but it turned out not to be the
  only possible oracle.
- **TRE canvas click** — ~~requires a scanned-PDF fixture with known text
  coordinates, OCR processing, and a bitmap-comparison oracle. None of these
  are available; row permanently closed (see matrix notes).~~
  **STALE, corrected 2026-08-03 (F19, gap A5-15).** `"permanently closed"` was
  wrong on both halves: `tests/ui_automation/test_canvas_click_tre.py` exists,
  and `txt_tre_edit_regular_font`, `_arabic`, `_cjk` and `_size_change` are all
  **green** ("c82a1f37 P5: re-measured GREEN via live run").
  **The residual is narrower but real:** the oracle that was built is a
  *screenshot pixel-diff* (`test_canvas_click_tre.py:16`), not the
  bitmap-comparison-against-known-text oracle described here, and not a
  document-state oracle — see gap A5-83 below. A row can be green under a
  weaker oracle than the one its blocker specified; that is what happened.
- **Form field drag-place** — form field creation requires a
  mousePress → mouseDrag → mouseRelease gesture to define the field
  rectangle.  `pyautogui_drag` + `drag_canvas()` can synthesize the
  gesture but no reliable UIA oracle exists to confirm the field was
  registered.  Rows permanently closed pending a UIA-observable field
  count widget.

---

## Original Report (archived)

### Symptom

Synthetic mouse clicks dispatched to the `pdfCanvas` widget via any of:

- `pywinauto.WindowSpecification.click_input(coords=(x, y))`
- `pywinauto.mouse.click(coords=(x, y))`
- Raw `ctypes.windll.user32.SetCursorPos(x, y)` + `mouse_event(LEFTDOWN|LEFTUP)`
- `pyautogui.click(x, y)`

reach the Qt main window (observed: annotation-toolbar button toggle state
flips `0 → 1`, `Escape` key untoggles, `QAction::triggered` fires for the
toolbar button) but do **not** cause the active annotation tool to commit
an annotation to the document. Specifically, after a click:

- `UndoManager::isClean()` stays `true`
- The window title shows no `*` dirty-marker
- `AnnotationManager::getAnnotations(page)` returns zero new items
- No line appears in `%LOCALAPPDATA%\PDFE\PDFE\pdfe.log`

### What Works

- **Menu clicks** (phase-2, phase-4) — `click_menu_path("File -> Open...")`
- **Toolbar button toggles** (phase-5) — `stickyNoteToolActionButton` flips check state
- **Keyboard** (phase-2, phase-4) — `type_keys("{ESC}")`, `{ENTER}`, `^z`
- **Native dialogs** (phase-4) — Windows `#32770` file-save dialog with `auto_id=1001`/`1`
- **Main window activation** (`SetForegroundWindow`, `fx.main_window.set_focus()`)

### Root Cause Hypotheses (were unverified)

1. **HWND mismatch**: `WindowFromPoint(canvas_center)` returns the main
   window HWND (`Qt6100dQWindowIcon`, handle `52888306`), not the canvas
   widget. The canvas is painted into the main window's single HWND and
   has no native child window — `canvas.handle` is `None` in pywinauto.
   Qt does its own hit-testing on the resulting `WM_LBUTTONDOWN`, so this
   alone should not block dispatch.

2. **Qt event filter / modality**: the QActionGroup-driven tool activation
   path may leave focus on the toolbar button such that subsequent canvas
   clicks are consumed by a hidden event filter before reaching
   `PdfCanvas::mousePressEvent`.

3. **RDP / DPI / scaling**: the test environment is a Windows Server 2019
   RDP session. Synthetic input coordinate transforms may be subtly off
   even though rectangle reads back correctly.

4. **Tool activation via synthetic click doesn't fire
   `QAction::triggered`**: if pywinauto internally invokes a UIA
   `InvokePattern`/`TogglePattern` on the CheckBox wrapper rather than
   synthesizing `WM_LBUTTONDOWN`/`WM_LBUTTONUP`, the check state flips
   via direct property change but `QAction::triggered` never fires, so
   `MainWindow::onAnnotationToolSelected` is never called, so
   `canvas_->setCurrentTool(...)` is never called, so
   `PdfCanvas::currentTool_` is null and all subsequent canvas clicks
   fall through to the pan-drag default path. This is the most likely
   explanation — pywinauto's `click_input` docs state it uses mouse
   events, but the UIA wrapper may short-circuit to an Invoke.

**Verdict on hypothesis #4:** not the root cause.  `click_input` on
`stickyNoteToolActionButton` DOES fire `QAction::triggered` (confirmed
via qDebug).  The oracle (window title dirty marker) was the defect.
