# Native-UI Harness Audit — pipeline c4a9e271 phase-3

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


**Scope:** audit the native-UI test harness (`tests/ui_automation/`,
`tests/ui_harness/`, `tests/visual/`, `tests/helpers/`) for gaps that block
broad visual+interaction coverage, classify each gap by root cause, and fix the
gaps that block more than two planned test domains. Native stack only
(pywinauto / WinAppDriver / FlaUI / PyAutoGUI — never Selenium).

**Method:** read the C++ source for every test seam (`Q_INVOKABLE`, the
`testXxxPipe` QLineEdit seams gated by `PDFE_TEST_PIPES`, and the status-file
writer), grep every `menu_select` call site in `tests/`, and spot-verify
`setObjectName` coverage on the actions/dialogs the planned domains depend on.
Findings below cite `file:line`.

---

## 0. Harness topology (two parallel trees)

| Tree | Role | menu_select today |
|---|---|---|
| `tests/ui_automation/` | **Active** suite + harness (`harness/render_drive.py`, `harness/pdfe_app_fixture.py`, `conftest.py`). All menu actions already routed through `testToolActivatePipe`. | Only in **comments** documenting the GAP-A14 migration; no live `menu_select` calls in test bodies. |
| `tests/ui_harness/` | **Scaffold** helper library (`core/`, per-domain `helpers.py`). No `test_*.py` files yet — intended for the phase-5..7 batch authoring. | Live `menu_select` calls in domain helpers (always as a *fallback* after a UIA auto_id attempt). |

This split matters for criterion 5: the live suite is already GAP-A14 clean; the
remaining raw `menu_select` calls are in the not-yet-exercised `ui_harness`
scaffold.

---

## 1. Pipe-seam inventory

### 1a. Generic seam (the keystone)
- **`testToolActivatePipe`** (`src/ui/MainWindow.cpp:314`) → **`MainWindow::activateToolByObjectName()`** (`src/ui/MainWindow.cpp:5856`). Iterates `findChildren<QAction*>()` and triggers the one matching the written objectName — **menu OR toolbar, any domain** — deferred via `QTimer::singleShot(0)` so a modal dialog opened by the action cannot block the pipe write, and honouring `QAction::isEnabled()` (returns false for a disabled or already-checked action). Python driver: `render_drive.activate()` / `pdfe_app_fixture.invoke_action()`.

This single seam is why no per-domain "open the X dialog" seam is required (see §4).

### 1b. Input seams (drive an action/gesture)
`testClickByPdfCoordPipe` (:340), `testDoubleClickByPdfCoordPipe` (:388),
`testTextEditTypePipe` (:423), `testAnnotSelectPipe`/`testAnnotColorPipe`/
`testAnnotAuthorPipe`/`testAnnotContentsPipe` (:674/:715/:759/:792),
`testSetFontFamilyPipe`, `testFormFieldValueSetPipe`, `testMovePagePipe`,
`testSelectPagesPipe`, `testDragPipe`, `testGoToPagePipe`, `testCanvasDragPipe`,
`testTripleClickPipe`, `testScrollPipe`, `testAddRedactionMarkPipe`,
`testInsertBookmarkPipe`, `testSaveAsPipe`, `testFileDialogPathPipe`,
`testDialogActionPipe`.

### 1c. Query/result seam pairs (read state back without UIA)
`testActiveToolQueryPipe`/`ResultPipe`, `testTextEditDiagTriggerPipe`/`ResultPipe`
(carries the phase-2 `;ovsz=` overlay-size readback), `testOcrTextQueryPipe`/
`ResultPipe`, `testCommentInventoryTriggerPipe`/`ResultPipe`,
`testFontFamiliesQueryPipe`/`ResultPipe`, `testContextActionsQueryPipe`/`ResultPipe`,
`testFormFieldValueQueryPipe`/`ResultPipe`, `testRedactionMarkCountQueryPipe`/
`ResultPipe`, `testPrintActionEnabledQueryPipe`/`ResultPipe`,
`testAtlasReadyQueryPipe`/`ResultPipe`, `testZoomComboQueryPipe`/`ResultPipe`,
`testPageCountQueryPipe`/`ResultPipe`, `testLtvStatusQueryPipe`/`ResultPipe`,
`testCanvasRotationQueryPipe`/`ResultPipe`, `testValidationDialogQueryPipe`/
`ResultPipe`, `testTrustAnchorDialogQueryPipe`/`ResultPipe`,
`testDialogIntrospectionQueryPipe`/`ResultPipe`, `testAttachmentQueryPipe`/
`ResultPipe`, `testAltTextReadbackTriggerPipe`/`ResultPipe`.

### 1d. Status-file writer
`MainWindow::writeUiStatus()` → `%TEMP%/pdfe_ui_status.txt`, tokens
`TEXT_EDIT_ACTIVATING`, `SPAN_CLICKED|l,t,r,b` (transient), `CNN_COMPLETE|font`,
`ATLAS_READY|idx|mode`, `TEXT_EDIT_READY|N`. Used for async OCR/atlas readiness
polling (UIA cannot observe these transitions while the tile pump is busy).

### 1e. Q_INVOKABLE methods
- `MainWindow` (`src/ui/MainWindow.h`): `getCurrentAnnotationAltText` (:242), `consumeLastPrintDryRunFired` (:248), `currentSelectedAnnotationIndex` (:254), `activateToolByObjectName` (:266), `isPrintEnabled` (:268), `deleteSelectedAnnotation` (:270), `commentPanelRowCount` (:292), `textEditSelectionDiag` (:302), `activeToolDiag` (:311), `currentPageOcrText` (:324).
- `PdfCanvas` (`src/ui/PdfCanvas.h`): coordinate transforms (`pdfRectFromCanvas`, `canvasRectFromPdf`) + synthetic input (`clickByPdfCoord`, `dragByPdfCoord`, `tripleClickByPdfCoord`, `doubleClickByPdfCoord`, `typeIntoTextEdit`, `sendKeyToTextEdit`, `scrollByPixels`, `addRedactionMarkAtPdfRect`).
- Panels/dialogs: `PagePanel::{movePageProgrammatic,selectPagesProgrammatic,dragItemProgrammatic}`, `BookmarkPanel::insertBookmark`, `SearchPanel::performReplace`, `CalculationOrderDialog::moveProgrammatic`, `TabOrderDialog::moveProgrammatic`, `AnnotationPropertiesWidget::{setAnnotationColor,setAnnotationTextColor}`, `CustomizeToolbarsDialog::{addActionByObjectName,removeActionByObjectName,resetToDefaults}`.

---

## 2. objectName / accessibleName coverage

**Finding: no class-level gap.** Every dialog the planned domains touch sets
`objectName` on its interactive children (spot-verified counts: EncryptionDialog,
SignDialog, PreferencesDialog `preferencesTabs`, ComplianceDialog, BatchDialog,
CompareDialog, etc.). Every menu QAction the "menu-only" domains rely on has an
objectName — verified directly:

| Domain | Action objectName | `MainWindow.cpp` |
|---|---|---|
| preferences | `preferencesAction` | :2490 |
| view/search | `toggleSearchPanelAction` | :2563 |
| page-ops | `rotatePageCWAction` / `rotatePageCCWAction` / `rotatePage180Action` | :2584/:2591/:2598 |
| page-ops | `deletePageAction` / `insertPagesFromFileAction` | :2604/:2618 |
| encryption | `saveWithEncryptionAction` | :2891 |
| signatures | `signDocumentAction` / `verifySignaturesAction` | :2912/:2921 |
| compliance | `complianceAction` | :3010 |

Panels (BookmarkPanel, SearchPanel, CommentPanel, PagePanel, RedactionPanel,
SignaturePanel, LayerPanel, PropertiesPanel) name their primary controls and
expose `Q_INVOKABLE` seams for the operations whose child widgets are awkward to
reach via UIA. **Root cause of residual partials:** custom-painted/list widgets
where naming every item is impractical — mitigated by the programmatic seams, so
no fix needed.

---

## 3. Interaction classes with no reliable UIA automation path

| Interaction class | Root cause | Status |
|---|---|---|
| Menu actions (open dialog / toggle panel) | UIA `menu_select` walks the QMenu popup tree via `FindAll`, which **deadlocks on Server-2019** when the tile pump is busy (GAP-A14). | **Covered** by the generic `testToolActivatePipe` seam (§1a). |
| Modal dialog enumeration | UIA cannot enumerate a Qt app-modal dialog while its `exec()` loop runs. | **PARTIALLY covered** by `testDialogIntrospectionQueryPipe` + dialog-specific query seams + `testDialogActionPipe`. Corrected 2026-08-29: that mechanism is **opt-in per dialog** — only 7 of 25 `QDialog` subclasses call `registerDialogIntrospection()`, so 18 remain unobservable and several tests silently decline to assert on them. `tests/helpers/modal_dialog.py` (Win32 `EnumWindows`) is the general fallback needing no opt-in. See `docs/audit/ui-test-observability-audit.md`. |
| Synthetic mouse gestures on canvas (SelectTool handle hit-test, rubber-band, Ctrl/Shift multi-select) | Qt6 spontaneity barrier — `QMouseEvent::spontaneous()==false` for events sent via `QApplication::sendEvent`; the affected tools filter on spontaneity. (`tests/docs/synthetic_event_research.md`) | **Covered** by Win32 `SendInput` drivers (`test_ui_canvas_functional_sendinput.py` et al.) + `PdfCanvas::*ByPdfCoord` Q_INVOKABLE seams. |
| Rendered PDF raster (no UIA tree on the bitmap) | The canvas page render is a painted bitmap, not a control tree. | **Covered** by PyAutoGUI coord-clicks + screenshot diffing (`tests/visual/`, `render_oracle`). |
| Drag-reorder of page/field lists | Qt `ExtendedSelection` needs OS modifier state baked into the event sequence. | **Covered** by `testDragPipe` / `dragItemProgrammatic` + SendInput. |

**No interaction class remains without an automation path.**

---

## 4. Criterion 2 — missing seams blocking >2 domains

**Conclusion: none.** The domains the `ui_harness` scaffold reaches only via
`menu_select` (compliance, encryption, signatures, preferences, search-panel
visibility, page insert/delete/rotate) are all **QAction triggers**, and the
generic `testToolActivatePipe → activateToolByObjectName` seam (§1a) already
fires any of them by objectName — and §2 confirms each target action *has* an
objectName. Adding a per-domain "open dialog" pipe would be redundant
infrastructure, so **no new C++ seam was added** (avoiding speculative seams the
gates would rightly flag).

What was added instead — a **regression lock** so the keystone seam cannot
silently break for a domain: `tests/test_main_window_q_invokable.cpp` gains
`MenuOnlyDomainActions_ExistWithExpectedObjectNames` (all nine domain actions
exist as child QActions with the exact objectName the harness writes),
`ActivateToolByObjectName_FiresAlwaysEnabledMenuAction` (the seam reaches a
non-tool *menu* action, `preferencesAction`, not just checkable tool actions),
and `ActivateToolByObjectName_ResultMatchesEnabledState` (the seam honours
`isEnabled()` — a document-gated action returns an honest `false` with no
document). These are the "gtest smoke per seam" the criterion requires.

---

## 5. Criterion 3 — height-histogram diff mode

`tests/visual/diff.py` extended with `ink_row_histogram()`,
`glyph_ink_height()`, and `height_histogram_compare()` — a font-size-aware
comparison (glyph-ink vertical extent + ratio) that SSIM/pixel-fraction cannot
express (two captures of *different* text differ in nearly every pixel yet may
share a font size). Unit-locked by `tests/visual/test_diff_height_histogram.py`
(6 tests, incl. the bug scenario: equal sizes → ratio ~1.0 → `pass=False`).

---

## 6. Criterion 4 — teardown self-test

`conftest._cleanup_pdfe_between_tests` (autouse) calls `kill_pdfe_processes()` +
`_wait_process_exit("pdfe.exe", 3.0)`. `_wait_process_exit` is best-effort and
returns `None`, so the source of truth is a live process count.
`tests/ui_automation/test_harness_self_test.py` launches a real pdfe.exe and
asserts the teardown primitives clear it within 5s (process count → 0), plus a
fast no-op path when nothing is running. (Orphan test is `require_binary`-gated;
no-op test runs anywhere.)

---

## 7. Criterion 5 — raw menu_select reconciliation

Live `menu_select` call sites (verified by grep, `tests/**/*.py`):

| File:line | Purpose | Disposition |
|---|---|---|
| `tests/ui_harness/core/locate.py:menu_path` | generic menu helper | **Documented** GAP-A14 + new `invoke_action()` pipe primitive added as the preferred path. |
| `tests/ui_harness/{signatures,dialogs,redaction,panels,page_ops,forms,image_edit,text_edit}/helpers.py` | per-domain action triggers — each already a *fallback* after a UIA auto_id attempt | **Annotated** GAP-A14; callers should prefer `invoke_action(window, "<objectName>")`. menu_select retained as last-resort for interactive/local hosts. |
| `tests/ui_harness/menus/helpers.py:48` | the test target **is** the menu bar | **Documented** — menu_select is intentional (it exercises menu navigation itself; skipped/xfail on Server-2019). |
| `tests/ui_harness/core/launch.py:339` (docstring) | About-dialog example | docstring only. |
| `tests/test_ui_forms.py:1589` | wrapped in `_menu_select_with_timeout` | already mitigated by a wall-clock timeout wrapper; documented. |
| `tests/ui_automation/test_ui_search_replace.py:75` | search-panel open — pipe-first (`replaceAction`), menu_select fallback | **Annotated** GAP-A14 inline (pipe path at :67-71 is the Server-2019 path). |
| `tests/ui_automation/test_ui_file_dialog_flows_wad.py:219` | attachment-panel toggle in the WinAppDriver harness, after a visible-first `find_widget` check | **Annotated** inline — WAD has its own timeout/retry; the pywinauto UIA-FindAll deadlock GAP-A14 guards against does not apply to the WAD path. |
| other `tests/ui_automation/**` | — | already migrated; remaining `menu_select` tokens are explanatory comments, not live calls. |

`tests/ui_harness/core/locate.py` now exports `invoke_action()`, mirroring the
active suite's `pdfe_app_fixture.invoke_action()`, so the scaffold's phase-5..7
tests have a deadlock-free primitive from day one rather than inheriting
`menu_select`.

---

## 8. Residual / out-of-scope (tracked, not fixed here)

- `tests/visual/test_visual_baselines.py` baseline drift (P-GAP-105) — pre-existing render/DPI drift, out of phase-3 scope; addressed in phase-10 baseline relock.
- WinAppDriver (`*_wad.py`) menu paths — alternate harness; WAD has its own retry semantics and is not the Server-2019 deadlock path.

**Net phase-3 changes:** 1 audit doc, 3 new gtest smokes (existing file, no CMake
change), `diff.py` height-histogram mode + 6 unit tests, harness teardown
self-test, `ui_harness` `invoke_action` primitive + menu_select annotations. No
new product C++ seam (none was missing).
