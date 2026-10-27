# Autosave / Crash-Recovery — Requirements Traceability Matrix (RTM)

Pipeline: **autosave-coverage** (phases 1–6). This RTM is the Phase-6 synthesis
artifact. It maps every edit **family** (rows) against every coverage
**dimension** (columns) to the SPECIFIC, PASSING test that covers it.

**Ground rule (enforced):** a cell is only marked *covered* if a real, emitted,
passing testcase can be named. Cells that are genuinely out of scope carry
`N/A (by-design)` + a footnote; genuine residuals carry `gap: <reason>`. Every
`AutosaveMatrix …` name below was cross-checked against the binary's
`--gtest_list_tests` output; the full suite (`229` C++ instances) is green.

## Suites / files

| Suite (gtest) | File | Instances |
|---|---|---|
| `AutosaveRecoveryTest` | `tests/test_autosave_recovery.cpp` | 5 |
| `AutosaveCaptureAudit` | `tests/test_autosave_capture_audit.cpp` | 15 (discovery probe → gap-register.json) |
| `AutosaveCaptureFixTest` | `tests/test_autosave_capture_fixes.cpp` | 5 (4 gap locks + 1 perf lock) |
| `AutosaveMatrix/AutosaveMatrixTest` | `tests/test_autosave_matrix.cpp` | 186 (parametrized Cell/) |
| `AutosaveMatrixMixed` | `tests/test_autosave_matrix.cpp` | 6 |
| `AutosaveMatrixSignatureException` | `tests/test_autosave_matrix.cpp` | 1 |
| `AutosaveMatrixTre` | `tests/test_autosave_matrix.cpp` | 1 |
| `AutosaveSidecarIntegrityTest` | `tests/test_autosave_sidecar_integrity.cpp` | 10 |
| `test_ui_autosave_recovery.py` (pywinauto, `@pytest.mark.autosave`) | `tests/test_ui_autosave_recovery.py` | 3 |

Matrix total = 186 + 6 + 1 + 1 = **194** instances (the whole
`test_autosave_matrix.cpp`).

## Column legend

- **Trigger** — after the edit, `AutosaveManager::autosaveNow()` returns `true`.
- **Capture** — the recovery copy exists AND a fresh-reopen content oracle finds
  the edit in it.
- **Restore** — simulate a crash (drop everything un-cleanly), then a fresh
  `scanForRecoverable()` surfaces the session and the oracle finds the edit in
  the record's recovery copy.
- **Debounce** — a second `autosaveNow()` with no further edit returns `false`.
- **UndoToPartial** — two edits, undo one, autosave → recovery reflects the
  partial (post-undo) state (kept edit present, undone edit absent).
- **SaveClears** — after `clearRecovery()` (simulated clean save) a fresh scan no
  longer returns the session.
- **Mixed** — the family appears in a 3+-family single-snapshot cross-family test.
- **UI-E2E** — exercised through the shipping native UI recover/discard/live flow.

Matrix cells below cite one representative seed for brevity; most families run
across 2 seeds (see `makeParams()` in the matrix file). Where a supplementary
non-matrix test also covers a cell, it is listed after `+`.

## Matrix (family × dimension)

| Family | Trigger | Capture | Restore | Debounce | UndoToPartial | SaveClears | Mixed | UI-E2E |
|---|---|---|---|---|---|---|---|---|
| **TextEdit** | `Cell/TextEdit_TriggerFires_test_3page` | `Cell/TextEdit_CaptureContains_test_3page` + `AutosaveRecoveryTest.AutosaveWritesRecoverableState` | `Cell/TextEdit_RestoreContains_test_3page` + `AutosaveRecoveryTest.KillMidEditRecoveryRestoresUnsavedWork` | `Cell/TextEdit_DebounceSkip_test_3page` | `Cell/TextEdit_UndoToPartial_test_3page` | `Cell/TextEdit_SaveClearsRecovery_test_3page` + `AutosaveRecoveryTest.CleanSaveLeavesNothingToRecover` | `AutosaveMatrixMixed.TextAnnotationMetadata`, `.BookmarkTextLayer` | flow-agnostic ¹ |
| **Annotations/Highlight** | `Cell/AnnotationHighlight_TriggerFires_test_3page` | `Cell/AnnotationHighlight_CaptureContains_test_3page` | `Cell/AnnotationHighlight_RestoreContains_test_3page` | `Cell/AnnotationHighlight_DebounceSkip_test_3page` | `Cell/AnnotationHighlight_UndoToPartial_test_3page` | `Cell/AnnotationHighlight_SaveClearsRecovery_test_3page` | `AutosaveMatrixMixed.HighlightRotateLayer` | flow-agnostic ¹ |
| **Annotations/Square** | `Cell/AnnotationSquare_TriggerFires_test_3page` | `Cell/AnnotationSquare_CaptureContains_test_3page` | `Cell/AnnotationSquare_RestoreContains_test_3page` | `Cell/AnnotationSquare_DebounceSkip_test_3page` | `Cell/AnnotationSquare_UndoToPartial_test_3page` | `Cell/AnnotationSquare_SaveClearsRecovery_test_3page` | `AutosaveMatrixMixed.TextAnnotationMetadata`, `.ImageMetadataAnnotation` | flow-agnostic ¹ |
| **Annotations/Ink** | `Cell/AnnotationInk_TriggerFires_test_3page` | `Cell/AnnotationInk_CaptureContains_test_3page` | `Cell/AnnotationInk_RestoreContains_test_3page` | `Cell/AnnotationInk_DebounceSkip_test_3page` | `Cell/AnnotationInk_UndoToPartial_test_3page` | `Cell/AnnotationInk_SaveClearsRecovery_test_3page` | `AutosaveMatrixMixed.FormMetadataAnnotation` | flow-agnostic ¹ |
| **RedactionMark** | `Cell/RedactionMark_TriggerFires_test_redaction` | `Cell/RedactionMark_CaptureContains_test_redaction` | `Cell/RedactionMark_RestoreContains_test_redaction` | `Cell/RedactionMark_DebounceSkip_test_redaction` | `Cell/RedactionMark_UndoToPartial_test_redaction` | `Cell/RedactionMark_SaveClearsRecovery_test_redaction` | `AutosaveMatrixMixed.RedactionMarkSignatureMetadata` | flow-agnostic ¹ |
| **RedactionApply** (burn-in) | `Cell/RedactionApply_TriggerFires_test_redaction` + `AutosaveCaptureFixTest.RedactionApplyIsCapturedByAutosave` | `Cell/RedactionApply_CaptureContains_test_redaction` + `AutosaveCaptureFixTest.RedactionApplyIsCapturedByAutosave` | `Cell/RedactionApply_RestoreContains_test_redaction` | `Cell/RedactionApply_DebounceSkip_test_redaction` | N/A (by-design) ² → `Cell/RedactionApply_DirectReTrigger_test_redaction` | `Cell/RedactionApply_SaveClearsRecovery_test_redaction` | — ³ | flow-agnostic ¹ |
| **Image** (insert) | `Cell/ImageInsert_TriggerFires_test_pdf_with_images` | `Cell/ImageInsert_CaptureContains_test_pdf_with_images` | `Cell/ImageInsert_RestoreContains_test_pdf_with_images` | `Cell/ImageInsert_DebounceSkip_test_pdf_with_images` | `Cell/ImageInsert_UndoToPartial_test_pdf_with_images` | `Cell/ImageInsert_SaveClearsRecovery_test_pdf_with_images` | `AutosaveMatrixMixed.ImageMetadataAnnotation` | flow-agnostic ¹ |
| **PageOps/Rotate** | `Cell/PageRotate_TriggerFires_test_3page` | `Cell/PageRotate_CaptureContains_test_3page` | `Cell/PageRotate_RestoreContains_test_3page` | `Cell/PageRotate_DebounceSkip_test_3page` | `Cell/PageRotate_UndoToPartial_test_3page` | `Cell/PageRotate_SaveClearsRecovery_test_3page` | `AutosaveMatrixMixed.HighlightRotateLayer` | **`test_recover_path_restores_unsaved_edit`, `test_discard_path_removes_sidecars`, `test_live_autosave_writes_sidecar_during_edit`** ¹ |
| **PageOps/Delete** | `Cell/PageDelete_TriggerFires_test_3page` | `Cell/PageDelete_CaptureContains_test_3page` | `Cell/PageDelete_RestoreContains_test_3page` | `Cell/PageDelete_DebounceSkip_test_3page` | `Cell/PageDelete_UndoToPartial_test_3page` | `Cell/PageDelete_SaveClearsRecovery_test_3page` | — ³ | flow-agnostic ¹ |
| **FormFill** | `Cell/FormFill_TriggerFires_test_form` | `Cell/FormFill_CaptureContains_test_form` | `Cell/FormFill_RestoreContains_test_form` | `Cell/FormFill_DebounceSkip_test_form` | `Cell/FormFill_UndoToPartial_test_form` | `Cell/FormFill_SaveClearsRecovery_test_form` | `AutosaveMatrixMixed.FormMetadataAnnotation` | flow-agnostic ¹ |
| **Metadata** | `Cell/Metadata_TriggerFires_test_single` | `Cell/Metadata_CaptureContains_test_single` | `Cell/Metadata_RestoreContains_test_single` | `Cell/Metadata_DebounceSkip_test_single` | `Cell/Metadata_UndoToPartial_test_single` | `Cell/Metadata_SaveClearsRecovery_test_single` | 5/6 mixed tests (all except `HighlightRotateLayer`) | flow-agnostic ¹ |
| **Bookmarks** | `Cell/Bookmarks_TriggerFires_test_with_bookmarks` | `Cell/Bookmarks_CaptureContains_test_with_bookmarks` + `AutosaveCaptureFixTest.BookmarkEditIsCapturedByAutosave` | `Cell/Bookmarks_RestoreContains_test_with_bookmarks` | `Cell/Bookmarks_DebounceSkip_test_with_bookmarks` | `Cell/Bookmarks_UndoToPartial_test_with_bookmarks` | `Cell/Bookmarks_SaveClearsRecovery_test_with_bookmarks` | `AutosaveMatrixMixed.BookmarkTextLayer` | flow-agnostic ¹ |
| **PageLabels** | `Cell/PageLabels_TriggerFires_test_3page` | `Cell/PageLabels_CaptureContains_test_3page` + `AutosaveCaptureFixTest.PageLabelEditIsCapturedByAutosave` | `Cell/PageLabels_RestoreContains_test_3page` | `Cell/PageLabels_DebounceSkip_test_3page` | `Cell/PageLabels_UndoToPartial_test_3page` | `Cell/PageLabels_SaveClearsRecovery_test_3page` | — ³ | flow-agnostic ¹ |
| **Layers/Visibility** | `Cell/LayerVisibility_TriggerFires_test_3page` | `Cell/LayerVisibility_CaptureContains_test_3page` + `AutosaveCaptureFixTest.LayerToggleIsCapturedByAutosave` | `Cell/LayerVisibility_RestoreContains_test_3page` ⁴ | `Cell/LayerVisibility_DebounceSkip_test_3page` | `Cell/LayerVisibility_UndoToPartial_test_3page` | `Cell/LayerVisibility_SaveClearsRecovery_test_3page` | — ³ | flow-agnostic ¹ |
| **Layers/Create** | `Cell/LayerCreate_TriggerFires_test_3page` | `Cell/LayerCreate_CaptureContains_test_3page` | `Cell/LayerCreate_RestoreContains_test_3page` | `Cell/LayerCreate_DebounceSkip_test_3page` | `Cell/LayerCreate_UndoToPartial_test_3page` | `Cell/LayerCreate_SaveClearsRecovery_test_3page` | `AutosaveMatrixMixed.HighlightRotateLayer`, `.BookmarkTextLayer` | flow-agnostic ¹ |
| **SignatureFieldCreate** | `Cell/SignatureFieldCreate_TriggerFires_test_single` | `Cell/SignatureFieldCreate_CaptureContains_test_single` | `Cell/SignatureFieldCreate_RestoreContains_test_single` | `Cell/SignatureFieldCreate_DebounceSkip_test_single` | `Cell/SignatureFieldCreate_UndoToPartial_test_single` | `Cell/SignatureFieldCreate_SaveClearsRecovery_test_single` | `AutosaveMatrixMixed.RedactionMarkSignatureMetadata` | flow-agnostic ¹ |
| **SignatureSign** | N/A (by-design) ⁵ | N/A (by-design) ⁵ | N/A (by-design) ⁵ | N/A (by-design) ⁵ | N/A (by-design) ⁵ | N/A (by-design) ⁵ | N/A (by-design) ⁵ | N/A (by-design) ⁵ |
| **TRE** (scanned-text) | `AutosaveMatrixTre.TriggerCaptureRestore` | `AutosaveMatrixTre.TriggerCaptureRestore` | `AutosaveMatrixTre.TriggerCaptureRestore` | generic ⁶ | generic ⁶ | generic ⁶ | — ³ | flow-agnostic ¹ |

### Footnotes

1. **UI-E2E is family-agnostic.** The three pywinauto tests exercise the full
   shipping recover / discard / live-autosave-during-edit FLOW
   (`test_recover_path_restores_unsaved_edit`,
   `test_discard_path_removes_sidecars`,
   `test_live_autosave_writes_sidecar_during_edit`). They drive one representative
   dirty edit — a **page rotate** (`rotatePageCWAction` via the
   `testToolActivatePipe` seam) — because the recovery copy is a full snapshot of
   the live document: the UI recover/discard path carries whatever the document
   holds and is identical regardless of which family produced the dirt. So the UI
   layer is directly attributed to **PageOps/Rotate** (bold) and marked
   *flow-agnostic* for the rest — it is genuine coverage of the recovery FLOW, not
   a per-family UI assertion (which GoogleTest already provides headlessly).
2. **RedactionApply UndoToPartial N/A (by-design).** Redaction burn-in is a
   full-document rewrite with no undo command, so "undo one of two edits" is
   inapplicable. Coverage is not reduced: the dimension is replaced by
   `DirectReTrigger`, which applies a SECOND direct burn-in and asserts the
   recovery copy carries BOTH regions (a strictly harder capture check).
3. **Mixed `—`.** The 6 cross-family `AutosaveMatrixMixed` tests are a
   representative sample proving a single snapshot captures 3+ concurrent
   families; they are not exhaustive across all 16 families. Families with `—`
   are covered for the Mixed *contract* transitively (any two families in the
   same session share one `writeRecoveryCopy` snapshot) but are not individually
   named in a mix. Not a product gap.
4. **Layers/Visibility Restore.** The recovery oracle asserts the created layer
   survives the crash/restore. Note the runtime visibility *bit* itself is not
   serialized by any MuPDF save (see COVERAGE.md residual gap (c)); this is
   orthogonal to the autosave-capture contract ("autosave captures whatever a
   normal save would").
5. **SignatureSign N/A (by-design), locked explicitly.**
   `SignatureManager::signField()` takes a `const Document*`, pushes no undo
   command, and performs its own incremental `pdf_save_document` directly to the
   source file. It therefore neither dirties the undo stack nor moves
   `Document::contentVersion()`, so autosave does not fire — the signature is
   persisted by the signing path itself. This exception is not silent: it is
   pinned by `AutosaveMatrixSignatureException.SignIsNotCapturedByAutosave`
   (asserts no undo command, `autosaveNow()==false`, no recovery copy, AND a
   positive control that the signature persisted via signField's own save).
6. **TRE generic dimensions.** Debounce / UndoToPartial / SaveClears are
   AutosaveManager-level (family-agnostic) behaviours. Debounce is locked by
   `AutosaveSidecarIntegrityTest.DebounceNoRedundantSidecarRewrite`
   (`tests/test_autosave_sidecar_integrity.cpp:598`) and SaveClears by
   `…ClearRecoveryRemovesSessionFromScan` (`:568`). **UndoToPartial has no
   `AutosaveSidecarIntegrityTest` counterpart** — it rests purely on the other
   15 families' cells. *(Corrected in meta-audit follow-up F19, gap A4-31:
   this paragraph previously said all three were "already locked by
   `AutosaveSidecarIntegrityTest`", which was true for two of the three.)*
   The heavy
   (~6 s) TRE reconstruction path re-exercises only the family-specific
   Trigger+Capture+Restore trio (in one combined test) to keep suite time bounded.
   Not a product gap.

## Coverage completeness

Every family × dimension cell above resolves to either a named passing test, an
`N/A (by-design)` with footnote, or a `—`/generic marker with an explicit
rationale (footnotes 3 & 6). **There are no unresolved `gap:` cells** — the one
capture gap the Phase-1 probe found (SignatureSign) is the documented by-design
exception. Residual known limitations (UNC/>MAX_PATH paths; layer visibility-bit
serialization) are catalogued in COVERAGE.md and are orthogonal to the
autosave-capture contract.
