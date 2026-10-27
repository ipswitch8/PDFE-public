# PDFE UI-Surface Coverage Audit — Pipeline c4a9e271 Phase 4

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


**Date:** 2026-06-23
**Pipeline:** c4a9e271 (per-line font-size fix + comprehensive native-UI
visual test suite)
**Scope:** Enumerate every user-facing UI interaction point in PDFE against
the test matrix, produce a definitive coverage-gap list with domain
groupings, priority/risk tiers, and a batch-allocation plan for phases 5–7.

This audit is **source-grounded**, not aspirational. Every number below is
computed directly from `src/ui/MainWindow.cpp`, `src/ui/*.cpp`, and the
committed `ui_test_matrix.csv` — the same anti-fabrication discipline the
matrix generator adopted after the C4-phase-0 finding that ~65% of the
original hand-written widget lists were invented.

---

## 1. Method

Three ground-truth extractions:

1. **QAction inventory** — every `setObjectName("…Action")` in
   `MainWindow.cpp`, scraped by the regex now wired into
   `generate_matrix.py::scrape_qactions()`. Result: **131 distinct named
   QActions**.
2. **Dialog/widget objectNames** — `audit_source.py` (static, no launch).
   Result: **53/53 expected objectNames present, 0 missing** across the 7
   audited dialog sources — dialog widget reachability is fully grounded.
3. **Matrix cross-reference** — for each QAction objectName, whether *any*
   matrix row referenced it. Result before this phase: **35/131 grounded,
   96 ungrounded** (a true coverage gap).

### The grounding gap explained

The pre-existing `menu_*` matrix rows are *friendly-path* rows (`"File >
Open"`). They do not name the stable QAction `objectName` that the GAP-A14
`testToolActivatePipe` seam (`MainWindow::activateToolByObjectName`, locked
in phase-3) actually drives on Server-2019 CI — where raw UIA `menu_select`
deadlocks. So a domain could *look* covered by a friendly row while the
invocation path CI really uses had **no** grounded assertion. 96 of 131
actions were in that state.

**Phase-4 resolution:** a new QAction-grounded coverage layer (`act_<object
Name>` rows, subsystem `Actions`) emitted one-per-action straight from
source. After regeneration: **131/131 QActions grounded (0 missing).**

---

## 2. Domain coverage table (QAction layer)

`total` = QActions in the domain · `grounded_before` = had any matrix
reference pre-phase-4 · `gap_before` = the coverage gap this phase closed ·
all domains now **grounded_after = total**. Risk tier is the highest
priority among the domain's actions.

| Domain | total | grounded_before | gap_before | risk tier | batch |
|---|---:|---:|---:|---|---|
| forms | 17 | 9 | 8 | P0 (flatten) | phase-6 |
| annotations | 14 | 2 | 12 | P1 | phase-6 |
| text-edit-tre | 13 | 3 | 10 | P1 | phase-6 |
| view-nav | 13 | 4 | 9 | P1 | phase-5 |
| file-ops | 12 | 1 | 11 | P0 (open/save) | phase-5 |
| page-ops | 11 | 4 | 7 | P0 (delete) | phase-5 |
| image-content | 11 | 0 | 11 | P1 | phase-6 |
| preferences | 9 | 2 | 7 | P1 | phase-7 |
| encryption-signatures | 7 | 2 | 5 | P0 (encrypt/sign/remove-pw) | phase-7 |
| edit-clipboard | 6 | 1 | 5 | P1 | phase-5 |
| compliance | 5 | 3 | 2 | P1 | phase-7 |
| help | 4 | 0 | 4 | P2 | phase-5 |
| tools | 4 | 3 | 1 | P1 | phase-7 |
| redaction | 3 | 0 | 3 | P0 (apply) | phase-6 |
| search | 2 | 1 | 1 | P1 | phase-5 |
| **TOTAL** | **131** | **35** | **96** | — | — |

Highest-risk gaps (P0 actions that were ungrounded before this phase):
`file-ops` (11 of 12 ungrounded, incl. open/save/saveAs path), `image-
content` (11/11 — entirely ungrounded), `annotations` (12/14), `redaction`
(3/3 incl. the irreversible `applyRedactionsAction`).

---

## 3. Full UI-surface coverage (all dimensions)

The matrix covers more than menu actions. Current state of every dimension
after phase-4 regeneration (`ui_test_matrix.csv`, 762 rows, verifier green):

| Dimension | subsystem(s) | rows | status |
|---|---|---:|---|
| Dialog widget reachability | Dialogs, DialogWidget | 236 | green (audit_source 53/53) |
| QAction invocation (new) | Actions | 131 | planned (this phase) |
| Page operations | Pages | 49 | green |
| Forms (field types × subtests) | Forms | 46 | green (4 wont-fix: Unknown/Sig XFDF) |
| Security / encryption / signatures | Security | 46 | green |
| Annotations (6 tools × subtests) | Annotations | 38 | green |
| Text formatting + TRE | TextEdit, TRE | 35 | green |
| Canvas coordinate interactions | Canvas | 25 | green |
| Critical end-to-end flows | Flows | 21 | green |
| View / navigation | View | 14 | green |
| File menu leaves | File | 13 | green (1 wont-fix) |
| Image ops | Image | 10 | green |
| Edit / clipboard | Edit | 9 | green |
| Tools | Tools | 9 | green |
| Redaction | Redaction | 5 | green |
| Layers, Fonts, Print, Properties, Search, AltText, CalcOrder, HeaderFooter, PageLabels, Preferences, Attachments, Exports, Rendering, UndoRedo, DefaultsLog, MenuAction, SendInput, Subsystem | (misc sweeps) | ~70 | green |
| Per-line font-size visual proof (new) | RenderVerify | 1 | planned → links phase-2 test |

**Distinct domains enumerated: 15 in the QAction layer + the broader
subsystem dimensions above — well past the ≥8 requirement.**

### Per-line font-size domain (Goal A linkage)

New row `txt_per_line_font_size_ui` (subsystem `RenderVerify`, owner
`phase-6`, P1) links the matrix to the implemented native-UI visual proof
`tests/ui_automation/test_per_line_font_size_ui.py` (pipeline c4a9e271
phase-2). It asserts heading ≥ 1.5× body on both the deterministic `;ovsz=`
readback and the visual glyph-ink-height axis — the regression guard for the
inert `perLineFontSizePt` bug fixed in phase-1.

---

## 4. Risk-tier definitions

| Tier | Meaning | Cadence | Examples |
|---|---|---|---|
| **P0** | Data-integrity, destructive, or security-critical. A defect here corrupts/loses content or breaks the security model. | every release | open/save/saveAs, deletePage, flattenForm, applyRedactions, saveWithEncryption, signDocument, removePassword |
| **P1** | Standard interactive surface. A defect degrades usability but is recoverable. | weekly | annotations, view/nav, image ops, forms fill, compliance, search |
| **P2** | Cosmetic or informational. | nightly | help (about/docs/report-issue/check-updates) |

Phase-4 matrix priority totals: **P0 363 · P1 311 · P2 88**.

---

## 5. Domain-allocation plan (phases 5–7)

Maps every QAction domain to its authoring batch. Aligns with the pipeline
phase descriptions; image-content and document-tools (which the original
plan did not slot explicitly) are assigned here with rationale.

### Phase 5 — Batch 1 (file-ops, page-ops, view/nav, search) — 48 actions
- **file-ops** (12): new, open, save, saveAs, close, exit, print,
  printPreview, exportPlainText, createPdfFromImages, combineFiles,
  documentMetadata
- **edit-clipboard** (6): undo, redo, cut, copy, paste, selectAll
  *(folded into batch 1 — clipboard is file/edit-level, exercised alongside
  open/save flows)*
- **page-ops** (11): rotate CW/CCW/180, delete, insertBlank,
  insertPagesFromFile, extractPages, moveUp/Down, replacePage, pageLabels
- **view-nav** (13): zoomIn/Out, fitWidth/Page, actualSize, fullScreen,
  previous/nextView, toggle{Page,Properties,Search,Bookmark,Attachment}Panel
- **search** (2): find, replace
- **help** (4): about, documentation, reportIssue, checkUpdates
  *(trivial, batched into phase-5 to keep phase-7 focused on security)*

### Phase 6 — Batch 2 (annotations, forms, text-edit/TRE, redaction, image) — 58 actions + per-line
- **annotations** (14): select/stickyNote/highlight/underline/strikeOut/ink/
  freeText/rectangle/circle/line/stamp/fileAttachment tools, importImageStamp,
  toggleStickyNotes
- **forms** (17): formTool, createFormField, fieldProperties, export/import
  FormData, resetForm, flattenForm, tabOrder, calculationOrder, submitForm,
  form{Text,Checkbox,RadioButton,ComboBox,ListBox,PushButton,Signature}
- **text-edit-tre** (13): textEditTool, treTool, treAction, textEdit{Cut,
  Copy,Paste,SelectAll,Bold,Italic,Find,AtlasScan,AtlasFont,AtlasCompare}
- **image-content** (11): imageTool, insertImage, stripExif, image{Remove,
  Rotate,Crop,Resize}, cropTool, convertToGrayscale, watermark, headerFooter
  *(content-editing batch — naturally pairs with TRE/annotation render work)*
- **redaction** (3): redactTool, markRedaction, applyRedactions
- **RenderVerify** (1): per-line font-size visual proof (already implemented)

### Phase 7 — Batch 3 (encryption/signatures, compliance, preferences, tools) — 25 actions
- **encryption-signatures** (7): saveWithEncryption, documentPermissions,
  removePassword, signDocument, verifySignatures, requestValidation,
  trustAnchor
- **compliance** (5): compliance, preflight, accessibility, altText,
  openAltTextDialog
- **preferences** (9): preferences, customizeToolbars, theme{Toggle,Light,
  Dark,System}, toggle{CopyPrevention,PrintPrevention,OcrBoxes}
- **tools** (4): ocr, batch, compare, optimizePdf

---

## 6. Matrix-tooling fixes landed this phase

The matrix verifier had drifted out of sync with its own committed CSV — it
was failing on the project's *own* data before any phase-4 change:

1. **`verify_matrix.py` `VALID_STATUS`** — omitted `wont-fix`, `xfail`,
   `skip`, the terminal lifecycle states emitted by `apply_wontfix.py`,
   `triage.py`, and pytest markers. Added.
2. **`verify_matrix.py` `VALID_PHASES`** — the rigid `{phase-1..10}` set
   rejected sweep-batch owner_phases (`phase-7-C1`, `phase-2-NEW2`,
   `phase-post`, `MAT-1`, …) that successive pipelines legitimately
   attached. Relaxed to a `phase-*` / `MAT-*` predicate.
3. **`generate_matrix.py`** — added the QAction-grounded coverage layer and
   the per-line RenderVerify row; both emitted from source so the matrix
   self-syncs with `MainWindow.cpp`.

After these fixes `verify_matrix.py` is **green: all 762 rows verified**.

---

## 7. Acceptance-criteria disposition

| # | Criterion | Status |
|---|---|---|
| 1 | Audit lists all domains: total / covered / gap / risk tier | ✅ §2, §3 |
| 2 | Matrix updated with new rows for every gap (status=planned); no rows removed | ✅ 624→762 (+138, **0 removed**): 131 `act_` + 1 RenderVerify + 6 dialog-widget rows the scraper newly picked up from SignDialog/CommentPanel (objectNames added since the last regen); 451 statuses preserved; 173 passthrough intact |
| 3 | ≥8 distinct domains enumerated | ✅ 15 QAction domains + broader dimensions |
| 4 | Per-line font-size row added, links phase-2 test | ✅ `txt_per_line_font_size_ui` → `test_per_line_font_size_ui.py` |
| 5 | audit_source.py re-run; any QAction without a matrix row added | ✅ audit_source 53/53; 131/131 QActions now have `act_*` rows (0 missing) |
| 6 | Domain-allocation plan (phase-5/6/7) | ✅ §5 |

---

## 8. Handoff to phases 5–7

The `act_*` rows are **status=planned**. Phases 5–7 author the actual
pywinauto tests that drive each action through `invoke_action()` (the
phase-3 pipe seam) and flip the row to `green`/`red` via
`conftest.record_matrix_result()`. The discovery run (phase-8) then triages
every non-green row into product-defect / harness-gap / environment, feeding
the phase-9 fix backlog.

Expected defect-yield hotspots (entirely ungrounded before this phase, so
most likely to surface latent bugs): **image-content** (11/11 gap),
**file-ops** (11/12), **annotations** (12/14), **redaction** (3/3).
