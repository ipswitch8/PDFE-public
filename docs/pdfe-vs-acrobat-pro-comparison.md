# PDFE vs Acrobat Pro — Feature & Functionality Comparison

**Feature-comparison snapshot date:** 2026-06-20
**Counts re-derived from the tree:** 2026-08-03 (meta-audit follow-up F19, gap A5-97).

**PDFE test state:** **4961 `TEST*` macros across 472 files in `tests/`**
(static count; the binary registers 5295 cases because `TEST_P` suites expand
at run time), **0 `DISABLED_`-prefixed**. Re-derive with
`grep -chE '^\s*TEST(_F|_P)?\(' tests/*.cpp` and `pdfe_tests.exe --gtest_list_tests`.

**PDFE UI E2E state:** native pywinauto/PyAutoGUI suite — **110 files run,
3369 tests, 11 failures, 0 errors, 427 skipped**, per the summary artifact
`tests/_runtime/discovery_phase8/ui-suite-summary.json`. The 11 failures sit in
8 files (`test_dialog_reachability_p0.py`, `test_render_annotations.py`,
`test_render_coord_rotation.py`, `test_render_oracle_proof.py`,
`test_render_page_ops.py`, `test_render_redaction.py`,
`test_render_text_edit.py`, `test_render_view_nav.py`). The
`windows-validation` job does parse this file and fail on unresolved failures
rather than trusting the exit code — that mechanism claim is true.

> **Correction (F19).** These two lines previously read "3926 GoogleTest
> cases, 0 failures" and "87 test files / 3015 tests / **0 failures** / 0
> hidden timeouts". Every one of those five figures was wrong, and the "0
> failures" UI claim was **contradicted by the very artifact the same sentence
> named**: `ui-suite-summary.json` records `failure_count: 11` and 427 skips,
> and has done since it was written. A ledger that cites its own evidence and
> then states the opposite of it is the failure mode this audit exists to
> find. The 11 failures are **not fixed here** — F19 is a documentation-truth
> phase; they are now visible instead of denied.
> `F19DocClaims.ComparisonDocUiE2eCountsMatchTheSummaryArtifact` re-reads the
> JSON and fails if the prose drifts from it again.
**Verification basis:** this comparison was re-grounded against the source tree (UI surface, engine modules, and `tests/docs/known_issues.md` / `gap_checklist.md`) on the snapshot date, not carried forward from prior snapshots. Earlier pipeline-by-pipeline UI-matrix history (Pipelines G/H/I and the UIA-vs-Qt combo-state remediation) is retained in git history and `tests/ui_automation/matrix/`.

**Legend**

- **✓** full parity
- **~** partial / narrower than Acrobat
- **✗** missing
- **⊘** explicitly out of scope by design

---

## Document lifecycle

| Acrobat Pro | PDFE | Notes |
|---|---|---|
| Open / save / close | ✓ | |
| Incremental save (preserves signed bytes) | ✓ | `Document::saveIncremental` |
| Save As (full rewrite) | ✓ | |
| Linearization (web optimization) | ✓ | via QPDF |
| Merge / combine files | ✓ | File > Combine Files... wizard: 3-step flow (file list → output path → progress); accepts mixed PDF + image inputs; merge uses pdf_graft_mapped_page (same path as ImportPages) so cross-document references stay intact |
| Split to multiple PDFs | ✓ | `extractPagesToFile` |
| Create PDF from Word/Excel/HTML | ✗ | one-way: PDFE exports to DOCX/XLSX but doesn't import them as source |
| Create PDF from scanner | ✗ | no scanner-hardware integration |
| Create PDF from image files | ✓ | File → Create PDF from Images... — multi-select wizard; each image becomes a page sized to its pixel dimensions at 72 DPI |

---

## Viewing & navigation

| Acrobat Pro | PDFE | Notes |
|---|---|---|
| Multi-page view, zoom, pan, fit-width/page | ✓ | |
| View rotation (display-only) | ✓ | |
| Page rotation (persisted) | ✓ | |
| Thumbnails panel | ✓ | |
| Bookmarks / outline panel | ✓ | CRUD + persistence |
| Full-text search | ✓ | Tantivy-backed; Rust bridge |
| Find + Replace | ✓ | |
| Links — URI | ✓ | |
| Links — GoTo (internal page) | ✓ | write + follow |
| Links — Named actions | ✓ | NextPage/PrevPage/Print/etc. |
| Links — Launch (external file) | ~ | write supported; follow always prompts with security warning — intentional policy, not a defect; no auto-execute by design. Stays at `~` because PDFE has no opt-in trusted-source allowlist (Acrobat's "Trust Manager") that lets the user permanently approve a known-safe path; every click prompts afresh. Verified by `test_launch_link_follow.cpp` (5 tests covering prompt-fires, decline-blocks, accept-launches, every-click-reprompts, mixed sequences). |
| Page labels (Roman, etc.) | ✓ | read + write via Pages > Page Labels... dialog; supports Decimal / RomanUpper / RomanLower / AlphaUpper / AlphaLower / None styles, optional prefix, start value |
| Navigation history (back/forward) | ✓ | Alt+Left / Alt+Right; View → Previous View / Next View; browser-style with truncation + dup-coalescing |
| Read Out Loud (screen reader) | ✗ | relies on OS accessibility, not built-in TTS |

---

## Annotations & markup

| Acrobat Pro | PDFE | Notes |
|---|---|---|
| Sticky notes | ✓ | |
| Highlight / underline / strikethrough | ✓ | |
| Ink / freehand drawing | ✓ | |
| Shapes (rectangle, circle, line, polygon) | ✓ | |
| Free text / text box | ✓ | |
| Stamps — static (Draft/Approved/Reviewed) | ✓ | |
| Stamps — dynamic tokens (`{date}`, `{author}`) | ✓ | |
| Custom stamp creation | ✓ | token-based + image-import (PNG/JPG/BMP) stamps; library persists across sessions; placement writes /AP/N Form XObject containing the image |
| Comment panel with threading (`/IRT`) | ✓ | |
| Reply action from comment | ✓ | |
| Measurement tools (distance / perimeter / area) | ✓ | |
| Alt text / ActualText on annotations | ✓ | |
| Movie / sound annotations | ⊘ | deferred — legacy, Adobe itself deprecated `/RichMedia`, no observed demand |
| 3D annotations (`/3D`) | ⊘ | deferred — niche CAD workflows |
| File attachments as annotations (on-page clip icon) | ✓ | `/FileAttachment` annotation with PaperClip / PushPin / Graph / Tag icons; embed any file's bytes per-page; save+reopen round-trip preserves payload |

---

## Forms

| Acrobat Pro | PDFE | Notes |
|---|---|---|
| Field types — Text / Checkbox / Radio / Combo / List / Push / Signature | ✓ | all 7 |
| Interactive field-placement palette | ✓ | exclusive toolbar QActionGroup |
| Drag-to-place field rectangle | ✓ | via `FormTool` |
| Field properties panel (name, value, default) | ✓ | |
| Tab order editor with drag reorder | ✓ | |
| Calculation order (`/CO`) editor | ✓ | |
| JavaScript per-field (keystroke/validate/calc/format/focus/blur) | ✓ | MuJS engine |
| JavaScript debugging / IDE | ✗ | no in-app JS debugger |
| XFDF import/export | ✓ | |
| FDF import | ✓ | file import (`FormDataIO::importFromFdf`), added by meta-audit follow-up F15 / gap PDF-40 |
| FDF export to file | ✗ | FDF is produced only as a *form-submit* payload (`FormEncoder`), never written to a file; use XFDF export |
| URL-encoded form submit | ✓ | |
| Form submit via HTTP POST | ✓ | with host allowlist |
| Flatten form to static content | ✓ | |
| Reset form | ✓ | for fields with `/DV` |
| Distribute/collect via email | ✗ | no email integration |

---

## Security & signatures

| Acrobat Pro | PDFE | Notes |
|---|---|---|
| Password encryption (RC4, AES-128, AES-256) | ✓ | |
| Permissions (print / copy / modify / annotate) | ✓ | |
| Redaction with verifier | ✓ | pattern matching + apply |
| Digital signatures (self-signed + PKCS#7) | ✓ | |
| CA-chain certificate signing | ✓ | fixtures shipped |
| Certification signatures (`/DocMDP`) | ✓ | 3 permission levels, UI control |
| Invisible signatures | ✓ | zero-area + no `/AP`, UI checkbox |
| Timestamp (RFC 3161) | ✓ | cached-bytes fixture + TSA URL config in sign dialog |
| Certificate-based (public key) encryption | ✗ | password-only; no recipient-cert encryption |
| Long-term validation (LTV / DSS/VRI) | ~ partial | LTV **data** embedding: `/DSS` dict with `/Certs` + `/VRI` (keyed by SHA-1 of signature contents hex) embedded incrementally; OCSP/CRL parsed via OpenSSL with 16/64 MiB caps; manual fetch dialog (`Security > Request Validation Data...`) — **no automatic external HTTP**, strict TLS, user-confirmed URLs only; embedded-DSS verifier honors trust anchors (HKCU) and refuses to elevate embedded certs to roots. **This is not an ETSI baseline profile:** PDFE emits ISO 32000-1 `adbe.pkcs7.detached`, never `ETSI.CAdES.detached`, and writes no document-level `/DocTimeStamp` — see [docs/signature-profile-support.md](signature-profile-support.md), which is the single place profile support is stated and is machine-checked against the emitted bytes. |
| Signature field appearance customization | ✓ | show labels/name/date/DN/logo toggles |
| Sign with biometric / smartcard | ✗ | PFX file only |
| Malformed-PDF robustness (sandbox / isolation) | ✓ | analog to Acrobat's Protected Mode: every document is opened first in a disposable `pdfe_preflight` child process; if that child crashes, times out, or reports a parse error the editor refuses the file cleanly. Contains the `PDFE-SEC-MUPDF-RECURSION-DOS` class (deep/cyclic page graphs that stack-overflow MuPDF's parser, un-trappable in-process via setjmp-based `fz_try`). Fail-open if the helper is absent. See `src/core/PreflightOpen.{h,cpp}`, `src/app/pdfe_preflight.cpp`. |

---

## Text editing & reconstruction

| Acrobat Pro | PDFE | Notes |
|---|---|---|
| Edit existing text (existing fonts) | ✓ | Phase 6a |
| Metric-compatible font substitution | ✓ | Phase 6b/c/d with scoring algorithm |
| Rich-text formatting (font family/size/bold/italic/underline/strikethrough/color) | ✓ | FormattingRun model |
| Alignment (Left/Center/Right/Justify) with persistence | ✓ | Phase 9 pipeline |
| Spell check | ✗ | not implemented |
| Dictionary / thesaurus | ✗ | |
| OCR on scanned pages | ✓ | Tesseract |
| Multi-language OCR (`eng+fra` etc.) | ✓ | via preferences dialog |
| Text Reconstruction Engine (edit scanned text) | ✓ | Phase 7 TRE |
| Glyph atlas for scanned-style fonts | ✓ | with harvesting + synthesis |
| Scan style transfer (FreeType → scan matching) | ✓ | |
| PANOSE / visual font identification | ✓ | + optional CNN re-ranker |

---

## Accessibility & compliance

| Acrobat Pro | PDFE | Notes |
|---|---|---|
| PDF/A conversion (1b / 2b / 2u / 2a / 3b) | ✓ | |
| PDF/UA conversion | ✓ | |
| Preflight — profile picker + issue tree + text report | ✓ | matches Acrobat's Preflight dialog shape |
| Preflight — one-click fix-all | ✓ | Pipeline E phase-12: Fix All button applies XMP, OutputIntent, JavaScript-strip fixers in one pass; manual-only issues (encryption, missing fonts, transparency, LZW) surfaced separately |
| Tagged PDF auto-tagging | ✓ | `AutoTagger` |
| Structure tree editor (edit tags by hand) | ✗ | read + auto-build only; no manual tree editing UI |
| Alt text / ActualText editing | ✓ | dialog UI |
| Reading order panel | ✗ | no visual reading-order editor |
| Color-blind simulation / soft proof | ✗ | |
| ICC color profiles (OutputIntent) | ✓ | |
| Font embedding compliance check | ✓ | |
| High-contrast mode | ✓ | theme manager |
| Screen-reader friendly widget names | ✓ | automation-ready `objectName` + accessibleName |

---

## Image handling

| Acrobat Pro | PDFE | Notes |
|---|---|---|
| Insert image | ✓ | |
| Delete / replace image | ✓ | |
| Resize / rotate / crop image | ✓ | via CTM |
| EXIF stripping | ✓ | |
| Export page as PNG/JPG | ✓ | batch |
| Compress images (JPEG / downsample) | ✓ | via Optimize |
| Color conversion to grayscale | ✓ | Tools > Convert to Grayscale recolors every page (images, text, vector ops) to DeviceGray via MuPDF pdf_recolor_page; survives save+reopen |
| Lanczos-3 text-bitmap resampling | ✓ | C1 polish |
| LCD subpixel text rendering | ✓ | C5 polish |
| Perceptual-alpha isolation for glyph extraction | ✓ | C4 polish |

---

## Document organization

| Acrobat Pro | PDFE | Notes |
|---|---|---|
| Page insert (blank) | ✓ | |
| Page insert from another PDF | ✓ | |
| Page delete | ✓ | |
| Page extract to new PDF | ✓ | |
| Page reorder (drag in thumbnails) | ✓ | |
| Page rotate (persisted) | ✓ | |
| Replace page | ✓ | |
| Crop pages (edit CropBox) | ✓ | interactive rubber-band crop tool (Tools > Crop Page); writes /CropBox; undoable via CropPageCommand |
| Headers and footers with tokens | ✓ | idempotent marker-based |
| Bates numbering | ✓ | `{bates:PREFIX:START[:WIDTH]}` token |
| Watermark (text + image) | ✓ | page-range support |
| Optimize PDF (compress / subset / garbage-collect) | ✓ | |
| Metadata (Info dict) editing | ✓ | |
| XMP metadata editing | ✓ | |
| Layers (OCG) view + edit | ✓ | |
| Portfolio (package) detection | ✓ | `isPortfolio` / `portfolioView` |
| Portfolio browser / carousel UI | ✗ | opens wrapper page; no dedicated portfolio navigation |
| Portfolio creation | ✗ | |
| Bookmarks CRUD | ✓ | |
| File attachments panel (list / add / extract / delete) | ✓ | |

---

## Export / interchange

| Acrobat Pro | PDFE | Notes |
|---|---|---|
| Export to Word (DOCX) | ✓ | + RTF variant |
| Export to Excel (XLSX) with table detection | ✓ | |
| Export to HTML | ✗ | |
| Export to plain text | ✓ | File → Export Plain Text... — concatenates extracted text with form-feed page separators; UTF-8 BOM for Windows editor compatibility |
| Export page as image (batch) | ✓ | PNG/JPG |
| Export to PPTX | ✗ | |
| Import from image → PDF | ✓ | File → Create PDF from Images... wizard — multi-file picker; QPdfWriter-backed output with each image becoming a page sized to its pixel dimensions at 72 DPI. Same wizard referenced as "Create PDF from image files" in the Document lifecycle section. |

---

## Automation & integration

| Acrobat Pro | PDFE | Notes |
|---|---|---|
| Batch processing engine | ✓ | |
| PDF comparison (diff) | ✓ | `DiffEngine` + `CompareDialog` split-pane UI (Tools > Compare Documents) with diff overlays, page sync, next/prev change navigation |
| Action wizard (macros / multi-step actions) | ✗ | no user-configurable action sequences |
| Document-level JavaScript | ✓ | form-field JS via MuJS plus document events (willSave/didSave/willPrint/didPrint/willClose) wired into Document::save/saveAs/destructor and MainWindow::onPrint via JavaScriptEngine::fireDocumentEvent |
| JavaScript debugger | ✗ | |
| Adobe Document Cloud integration | ⊘ | not Adobe |
| DDE / OLE server | ✗ | |
| Plugin architecture / SDK | ⊘ | explicit non-goal |
| Command-line interface | ✓ | |

---

## UI / workflow

| Acrobat Pro | PDFE | Notes |
|---|---|---|
| Dark / light / system theme | ✓ | |
| Customizable toolbars | ✓ | View > Customize Toolbars... opens drag/reorder dialog with available-action and current-toolbar lists per toolbar; layout persists via QSettings (`pdfe/toolbarLayout`); Reset to Default restores factory layout |
| Recent files menu | ✓ | |
| Preferences dialog (general / view / OCR / advanced) | ✓ | |
| Undo / redo across operations | ✓ | `UndoManager` |
| Cross-platform (Win / Mac / Linux) | ~ | codebase is cross-platform; current packaging + testing is Windows-focused |

---

## Headline gaps vs Acrobat Pro

Ordered by user-visible impact:

1. **PDF creation from non-PDF sources** — Word / Excel / image / scanner → PDF is a major Acrobat use case that PDFE only handles in the export direction.
2. **Action wizard / macros** — Acrobat's "Action Wizard" lets users chain "OCR → redact pattern → save → send" into one button. PDFE has batch processing but not a user-configurable multi-step action builder.
3. **Adobe Document Cloud** — cloud storage, shared review, web-based signatures. PDFE is local-only by design.
4. **Certificate-based (public-key) encryption** — Acrobat can encrypt a PDF to a list of recipient certificates. PDFE is password-only.
5. **Reading order / tag tree editor** — PDFE auto-tags but doesn't expose a visual tag-tree editor for hand-correcting the structure tree.
6. **Spell check + dictionary** — Acrobat has inline spell check in text edit mode. PDFE has no built-in spell infrastructure.
7. **3D content and multimedia** — explicitly deferred; niche.
8. **Distribute-by-email / collected form data** — PDFE has `/SubmitForm` but no Acrobat-style "distribute for signature" workflow.
9. **Portfolio creation + carousel UI** — detection only.
10. **Plugin SDK** — explicit non-goal per the parity doc.

---

## Where PDFE is genuinely stronger than Acrobat Pro

Worth noting honestly — these are places the PDFE implementation goes further than Acrobat's stock behavior:

- **Text reconstruction engine** — editing text on scanned / image-only PDFs via OCR → glyph atlas → style transfer → re-injection is deeper than Acrobat's "Recognize Text" + "Edit PDF" combo, which struggles with cursive / stylized scans.
- **Font identification from scans** — PANOSE matching + visual NCC re-ranking + optional CNN classifier is a pipeline Acrobat doesn't expose at all.
- **Rust-backed full-text search** — Tantivy is materially faster than Acrobat's linear scan on large corpora.
- **Test-backed parity claims** — 4961 GoogleTest macros plus a 110-file native UI E2E suite. Acrobat's behavior is a black box; PDFE's is verifiable. (The UI suite is not currently all-green — see the header block.)
- **AGPL licensing** — PDFE is open source; Acrobat is proprietary.

---

## Honest summary

**Coverage**: PDFE hits parity or near-parity on **every core editing, annotation, forms, signing, compliance, and organization workflow** that represents the vast majority of Acrobat Pro usage. The gaps are concentrated in three clusters: **PDF creation from other formats** (a different product category, really), **cloud / workflow integrations** (explicit design choice to stay local), and **niche / legacy features** (3D, multimedia, rich media).

**Quality**: Every claimed feature has a test-file reference in
`docs/acrobat-pro-parity-status.md`. **Qualified (F19):** those references were
re-audited on 2026-08-03 — of 112 unique test-file pointers, 111 resolve and
**1 did not** (`test_cert_chain_extractor.cpp`, since corrected), and 20 of the
75 count-annotated pointers carried the wrong number, one of them an over-claim
(`test_stamp_tokens.cpp` cited 16, has 12). "Explicit test coverage" therefore
means *a pointer exists and resolves*, not *the pointer was independently
verified to exercise the feature*. The stronger claim is not made here.

**What's missing for most users**: If you use Acrobat for viewing, annotating, editing, forms, signing, OCR, redaction, compliance, and page manipulation — PDFE covers it. If you use Acrobat to **create** PDFs from Word/scanner/image sources, or to run **Action Wizard macros**, or for **cloud collaboration** — PDFE doesn't cover those use cases today.

**Large dynamic-XFA save performance — RESOLVED** (`PDFE-LIMITATION-SAVEAS-LARGE-XFA`, pipeline b2e47d91, 2026-06-21): large dynamic-XFA forms were *correct* but *slow* — release-profiled at 5.2 min (8 MB SF-86), 12 min (3 MB IRS p17), 17 min (4 MB IRS i1040), because MuPDF's garbage-collection level 3 (full object dedup) is super-linear on deep XFA trees. Two fixes closed this: (1) content-only sessions (form-field fills, annotations) now save via incremental append instead of a full rewrite — the common "open, fill, Save" workflow on the 8 MB SF-86 dropped from **317 s → 0.33 s (~967×)**; (2) full-rewrite saves (e.g. "Save As") default to a cheaper GC level, escalating to full dedup only for ops that need it — "Save As" of the SF-86 dropped from **317 s → 2.3 s (~136×)**. The cheaper level still excludes removed/orphaned objects from the output (verified against MuPDF's write path), so redaction and other content removal stay confidential. Measurements: `tests/docs/xfa_save_benchmark.md`; regression coverage: `tests/test_incremental_save_routing.cpp`, `tests/test_conditional_gc.cpp`.

**Full-rewrite save on ORDINARY pages is near-quadratic, and that is NOT resolved**
(gap `PERF-01`, follow-up phase F40, 2026-08-07): the paragraph above was written
about *deep XFA trees*, and for years that was the only place the cost had been
measured. It is not XFA-specific. On plain, single-text-run pages — no forms, no
XFA — a structural edit (`reorderPages`) followed by a **gc=3** full-rewrite save
scales with a measured log-log exponent of **2.08 over the 500 → 2500 page range**
(Debug, best-of-5; 500 p 0.35 s, 2500 p 9.71 s). Independently reproduced on
2026-08-07 against the F27-era figure of 1.94, and the reproduction came out
*higher*, not lower. Below ~500 pages fixed overhead dominates and the apparent
exponent is far lower (**1.15 over 50 → 500**) — **an exponent quoted without its
page range means nothing here**, which is why both are given.

The cost is attributable: the **de-duplication sweep alone** is the entire
super-linear term. The identical edit and document written at `do_garbage=2`
(collect + renumber, no dedup) is **linear** (exponent 1.01 over the same range)
and **37× faster at 2500 pages, producing a SHA-256-identical file** (verified
at 500 pages, against a duplicate-carrying control document on which the gc=3
output correctly does differ, so the check is not vacuous). What the two
mitigations above changed is *when* gc=3 is selected, not what it costs when it
is; operations that still escalate to it (page reorder/delete/import, redaction,
flatten, font embed, encryption) still pay the quadratic term.

This is **disclosed, not fixed**. No production code was changed by F40, and
neither `do_garbage` selection nor object-stream usage was altered. Skipping
dedup is not free — on a document carrying genuinely duplicated objects it costs
**+31.5 % output bytes** — so the fix is a scoping question, not a switch to
flip. Evidence, all four measured options with their time *and* output-size
trade-offs, and the recommendation:
[`docs/audit/meta-audit-2026-07/followup/phase-40/README.md`](audit/meta-audit-2026-07/followup/phase-40/README.md).
Regression coverage bounds the exponent only over **50 → 500 pages**
(`FidelityR3_24_EditSaveScale.FullRewriteSaveScalingExponentStaysBounded`,
threshold 1.75); nothing guards the larger range, and that is a deliberate,
documented position rather than an oversight.

---

*Companion documents:*

- `docs/acrobat-pro-parity-status.md` — per-category feature matrix with commit + test pointers.
- `docs/image-pdf-risks.md` — scanned-PDF handling deep-dive (A3, B2, B3, C1/C4/C5).
- `docs/text-reconstruction-engine-architecture.md` — TRE internals.
- `docs/font-substitution-architecture.md` — font-match scoring algorithm.
- `tests/ui_automation/matrix/ui_test_matrix.csv` — UI test matrix (feature → UI-test mapping).
- `tests/docs/three_question_gate.md` — the Q1/Q2/Q3 end-user readiness synthesis (pipeline f3c91a2e closeout).
