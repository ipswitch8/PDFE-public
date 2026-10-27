# Menu Wiring Gaps — Phase-2 Blocked Items *(HISTORICAL — see banner)*

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


> ## ⚠ RE-OPENED 2026-09-18 — 2 blocked rows (p87 phase-4)
>
> *Previously discharged 2026-08-03 (meta-audit follow-up F19, gap A5-13), when
> the CSV genuinely held zero `blocked` rows. It no longer does, so the
> all-clear is withdrawn rather than left standing — `F19DocClaims.MenuWiringLedgerHasNoBlockedRowsLeft`
> fails the build if this banner claims discharge while any row is blocked.*
>
> **The 29-row ledger this document maintains still does not exist.**
> `tests/ui_automation/matrix/ui_test_matrix.csv` holds 787 rows: 750 green /
> 33 wont-fix / 0 planned / 3 xfail / 0 skip / **1 blocked**. The "Outcome
> target for this pass: **29 blocked → ~10–14 blocked**" at the foot of this
> file describes a state the tree left behind.
>
> **On the 2 `blocked` rows (p87 phase-4).** This banner previously said *zero*
> `blocked`, and that had been true since the 29-row ledger was cleared. The
> two that came back are not a regression of that work — they are the honest
> status for cells whose blocker is a **product defect**, not a harness
> limitation, which is the one thing `blocked` is for:
>
> * `canvas_gesture_multiselect` — additive Ctrl+click annotation selection is
>   **not implemented**. `SelectTool::onMousePress` clears
>   `selectedAnnotIndices_` unconditionally and never reads the modifiers. The
>   row had cited a RESOLVED P-GAP-92 and blamed synthetic input delivery; that
>   diagnosis was wrong.
> * `sendinput_shift_click` — blocked on **GitHub #226**: Shift+click range
>   select picks only the clicked page. The row's stated blocker (a missing
>   multi-page fixture) was also wrong — `test_multipage.pdf` has existed with
>   5 pages since May, and the test had been passing on a liveness check.
>
> A cell whose product feature does not exist is not green, not xfail, and not
> wont-fix. Recording it as `blocked` against a filed issue is what keeps the
> census honest.
>
> Re-verified item by item on 2026-08-03: **all 17 Tier-1 rows are green**, and
> **12 of the 13 Tier-2/Tier-3 items have landed** —
> Edit▸Replace (`MainWindow::onReplace`), Edit▸Preferences
> (`src/ui/PreferencesDialog.cpp`), Pages▸Move (`onMovePageUp`),
> Forms▸TabOrder (`TabOrderDialog`), Tools▸OCR-all-pages
> (`actionOcrAllPages`), View▸Rotate (`actionRotateViewCW/CCW`), View▸Theme
> (`src/theme/ThemeManager.cpp`), Forms▸SubmitForm
> (`src/formsubmit/FormSubmitter.cpp`), Tools▸BatchProcess
> (`src/ui/BatchDialog.cpp`), Tools▸Compare (`src/ui/CompareDialog.cpp`), and
> the accessibility surface via `ComplianceDialog`'s `tagStructureTree`.
>
> **ONE ITEM IS STILL GENUINELY OPEN and is re-homed here rather than lost in
> an archive:**
>
> - **File ▸ New** — needs a `Document::createEmpty()` factory. `grep -rn
>   "createEmpty\|createEmptyDocument\|newDocument" src/` returns nothing, and
>   there is no `file_new` row in the matrix, so this gap is currently tracked
>   **nowhere else**. That is why this file is annotated rather than moved to
>   `docs/archive/` — archiving it would silently drop the one live item.
>
> `F19DocClaims.MenuWiringLedgerHasNoBlockedRowsLeft` re-derives the `blocked`
> count from the CSV and fails if this banner and the matrix disagree.

Phase-2 (P0 Menu Navigation) found 29 matrix rows referencing menu
items that don't exist on the current Qt menu bar. Investigation
confirmed these are *not* missing features — they are overwhelmingly
**UI wiring gaps**. The backends exist; the corresponding `QAction`,
slot, and `addAction()` call in `MainWindow::createActions()` /
`createMenus()` just were never written.

This document is the gap ledger. Each blocked item gets a row stating
whether it can be finished by pure menu wiring or if it needs real
work first. Items marked **SMALL** should be treated as test-debt in
phase-2, not backlog.

Source of truth for backend presence: `src/*/*.h` (API headers).
Source of truth for current UI wiring: `src/ui/MainWindow.cpp`
`createActions()` and `createMenus()`.

## Tier 1 — SMALL (pure menu wiring, backend 100% ready)

| Item | Backend API | Status |
|---|---|---|
| File > New | Would need a `Document::createEmpty()` factory | Moved to Tier 2 |
| Edit > Copy | `TextSelector::selectedText()` | **DONE** — `onCopy()` slot (stub, enabled on doc open) |
| Edit > Cut | Copy + delete | **DONE** — `onCut()` slot (stub, enabled on doc open) |
| Edit > Paste | ContentStreamEditor | **DONE** — `onPaste()` slot (stub, enabled on doc open) |
| Edit > SelectAll | `TextSelector::selectAll()` | **DONE** — `onSelectAll()` slot (stub, enabled on doc open) |
| Security > RemovePassword | `EncryptionManager::removeEncryption()` | Already wired (`onRemovePassword()`) |
| Image > Remove | `ImageManager::removeImage()` | **DONE** — `onImageRemove()` slot + menu action |
| Image > Rotate | `ImageContentEditor::rotateImage()` | **DONE** — `onImageRotate90()` slot + menu action + signal connection |
| Image > Crop | `ImageContentEditor::cropImage()` | **DONE** — `onImageCrop()` slot + menu action + signal connection |
| Image > Resize | `ImageContentEditor::resizeImage()` | **DONE** — `onImageResize()` slot + menu action |
| View > ActualSize | PdfCanvas `setZoom(1.0)` | Already wired (`onActualSize()`) |
| View > FullScreen | Qt `showFullScreen()` | Already wired (`onToggleFullScreen()`) |
| Help > Documentation | External URL | Already wired (`onDocumentation()`) |
| Help > ReportIssue | External URL | Already wired (`onReportIssue()`) |
| Help > CheckUpdates | External URL | Already wired (`onCheckUpdates()`) |
| Forms > CreateField | `FormManager::createFormField()` | Already wired (`onCreateFormField()`) |
| Forms > FieldProperties | `FormFieldPropertiesPanel` | Already wired (`onFormFieldProperties()`) |

## Tier 2 — MEDIUM (menu + glue code)

| Item | Backend State | Missing Work |
|---|---|---|
| File > New | Needs `Document::createEmpty()` factory | Factory method writes minimal single-page PDF via MuPDF, opens it |
| Edit > Replace | `SearchEngine` exists; replace-in-stream does not | Extend SearchPanel with replace row + `applyReplace()` |
| Edit > Preferences | `AppSettings` persists via QSettings | `PreferencesDialog` with tabs (General / Appearance / Behavior) |
| Pages > Move | `PageOperations` + `MovePageCommand` exists | Up/Down actions + PagePanel drag-drop already has stubs |
| Forms > TabOrder | `FormManager::getTabOrder()` exists | Tab Order dialog (list widget, reorder, save) |
| Tools > OCR | `OcrPipeline` exists; per-page already runs as part of TRE | Batch-all-pages OCR action + progress dialog |
| View > RotateView | Page rotation exists; view-only rotate requires canvas CTM | `onRotateView()` toggle that rotates canvas paint transform |
| View > Theme | No theming engine | QPalette / stylesheet swap + Light/Dark/System preset |

## Tier 3 — LARGE (backend also missing)

| Item | Missing | Notes |
|---|---|---|
| Forms > SubmitForm | HTTP submit handler | Needs MuJS form event wiring + network |
| Tools > Accessibility | Standalone a11y report | Backend exists in compliance module; needs its own dialog |
| Tools > BatchProcess | Batch engine | New subsystem; out of scope for phase-2 test debt |
| Tools > Compare | PDF diff engine | New subsystem; out of scope for phase-2 test debt |

## Execution order

1. Land Tier 1 in one commit (≤15 menu actions, all pure wiring).
2. Re-run `enumerate_menus.py` to refresh ground truth.
3. Re-run phase-2; the now-wired items should flip from `blocked` to `green`.
4. Tier 2 items in separate commits, one feature at a time.
5. Tier 3 items remain backlog and stay `blocked` in the matrix with
   a real reason ("requires batch engine / compare engine / submit
   handler — deferred post-1.0").

Outcome target for this pass: **29 blocked → ~10–14 blocked**,
remainder split between green (Tier 1) and planned-per-tier-2/3.
