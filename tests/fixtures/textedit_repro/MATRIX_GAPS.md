# MATRIX_GAPS — Text-Edit Test Coverage Audit

<!-- tracking-banner -->
> **STATUS: HISTORICAL EVIDENCE — this is not a work list.**
>
> **GitHub Issues (`ipswitch8/PDFE`) is the canonical tracker for anything still
> open.** This document records what was found or decided on the day it was
> written. Where this document and a GitHub issue disagree, **the issue wins**.
>
> See [`docs/TRACKING.md`](../../../docs/TRACKING.md).

---


**Pipeline phase:** phase-1 (audit + gap inventory only — no production / test code changes)
**Generated:** 2026-04-28
**Author:** qa-expert (strategic orchestration)
**Source-of-truth pipeline:** `C:/Users/dev/source/PDFE/.claude/pipeline.json`

---

## Methodology

This document is the load-bearing fixture consumed by phases 3 (failing
regression tests for the three reported bugs), 4–6 (targeted fixes),
and 7 (matrix expansion). It must be specific enough that a later phase
can pick a single row and write a concrete test from it.

### Inputs read

1. **Existing 105-test text-edit matrix** — every `TEST_F` / `TEST` entry in:
   - `tests/test_text_edit_clipboard.cpp` (5)
   - `tests/test_text_edit_commands.cpp` (8)
   - `tests/test_text_edit_integration.cpp` (5)
   - `tests/test_text_edit_model.cpp` (14)
   - `tests/test_text_edit_pipeline.cpp` (5)
   - `tests/test_text_edit_tool.cpp` (10)
   - `tests/test_text_edit_toolbar.cpp` (17)
   - `tests/test_text_edit_width_scaling.cpp` (7)
   - `tests/test_textedit_tm_regression.cpp` (2)
   - `tests/test_overlay_font_mapping.cpp` (32)
2. **Production code paths**:
   - `src/textedit/TextEditTool.cpp` (selection state, paint overlay, key/mouse dispatch, formatting API)
   - `src/textedit/TextEditTool.h` (`selectionStart_/selectionEnd_`, callbacks)
   - `src/textedit/TextEditCommands.cpp` (`EditTextCommand::mergeWith`, `undo`, `redo`, `applyText`)
   - `src/textedit/TextEditCommands.h` (`oldText_`, `newText_`, `oldFormattingRuns_`, `newFormattingRuns_`)
   - `src/textedit/ContentStreamEditor.h/cpp` (save path: `applyEdits`, `applyEditsWithSubstitution`)
   - `src/textedit/TextEditModel.cpp` (span/char model)
   - `src/ui/MainWindow.cpp` (textEdited callback at line 3430, undo/redo handlers at 2589/2626, EditTextCommand construction)
3. **UI test inventory**: `tests/test_ui_pdfe_smoke.py`, `tests/test_ui_titlebar_icon.py` — neither exercises text editing. Result: zero UI coverage of the text-edit subsystem.
4. **Reproduction fixture**: `C:\Users\dev\Desktop\The Lean Startup - Erick Ries.pdf`, page index 2 (CharisSILRegular body text, fontSize 12.48, 0.48× scaled CTM block).

### Cross-reference rule

Every row below cites either (a) an existing test it does NOT duplicate,
explaining what that test fails to assert, or (b) the absence of any
test exercising the area. Citations use the `file:test-name` form
mandated by criterion 4.

### Three load-bearing observations from the source read

1. **Selection paint guard (`TextEditTool.cpp:1345`)** — `if (selectionStart_ >= 0 && selectionStart_ != selectionEnd_)`. A drag that lands the second hit on the same character (start==end) leaves the highlight invisible, but `cursorPosition_` still advances to `charIdx`. This is consistent with the reported "selection sometimes doesn't appear at all" — particularly on small spans where a single click+release picks the same char index twice. The path is exercised today only by `test_text_edit_clipboard.cpp:CopyAfterSelectAll` via `Ctrl+A`, which guarantees `selectionStart_ != selectionEnd_` and hides the bug.
2. **Empty `oldFormattingRuns_` baseline (`TextEditCommands.cpp:33`)** — explicit comment: `// oldFormattingRuns_ is empty (original state before any formatting)`. After undo of a command that arrived AT a span with pre-existing formatting (e.g. a bold word in a multi-format paragraph), the span returns to *no formatting at all*, not to its pre-edit formatting. Compounded across a chain of edits where each `EditTextCommand` is constructed with the post-edit `formattingRuns` of the *preceding* command, undo can monotonically erase formatting state. This is consistent with the reported "undo eventually blanks the line."
3. **`mergeWith` does not preserve baseline (`TextEditCommands.cpp:68-83`)** — the merge sets `newText_ = otherEdit->newText_` and `newFormattingRuns_ = otherEdit->newFormattingRuns_` but copies nothing from `otherEdit` for the *old* state. That is correct for keystroke-merge (we keep our `oldText_`), BUT the existing `MergeSameSpan` test (`test_text_edit_commands.cpp:145`) only asserts `merged == true` and never `cmd1->undo()` to verify `oldText_` survived. So if a future bug regressed `mergeWith` to overwrite `oldText_`, no test would catch it.

These three observations directly motivate `GAP-SEL-01`, `GAP-SAVE-01`,
and `GAP-UNDO-01` as the three reported bugs and supply the technical
hypotheses that phase-3 regression tests will turn into executable
assertions.

---

## Gap inventory

| id | category | scenario summary | production code path under test (file:function) | suggested test type | suggested test file name | priority |
|---|---|---|---|---|---|---|
| GAP-SEL-01 | selection | Selection highlight is sometimes invisible during edit on the Lean Startup p2 fixture; after click + drag, `selRect` collapses to zero width or is suppressed by the `selectionStart_ != selectionEnd_` guard. | `src/textedit/TextEditTool.cpp:onPaintOverlay` (lines 1345–1395), `onMouseMove` (912–929) | unit + ui-screenshot | `test_text_edit_selection_render.cpp`, `test_ui_text_edit_selection.py` | P0 |
| GAP-SAVE-01 | save | Overlay font/size changes look correct during edit but save with wrong glyphs; the toolbar's selected family/size never reaches the saved content stream's `Tf` / embedded-font resource. | `src/textedit/ContentStreamEditor.cpp:applyEditsWithSubstitution`, `src/textedit/TextEditCommands.cpp:applyText` (line 117 `fmtRuns` branch), `src/ui/MainWindow.cpp:textEdited` (line 3430) | integration | `test_text_edit_save_roundtrip.cpp` | P0 |
| GAP-UNDO-01 | undo | After multiple edits + undos on the same span, original text is lost; undo eventually blanks the line. Suspected cause: empty `oldFormattingRuns_` baseline + EditTextCommand chains constructed with post-edit `spanInfo`. | `src/textedit/TextEditCommands.cpp:EditTextCommand::EditTextCommand` (line 31), `undo` (line 45), `mergeWith` (line 68); `src/ui/MainWindow.cpp:textEdited` (line 3430) | integration | `test_text_edit_undo_chain.cpp` | P0 |
| GAP-SEL-02 | selection | `selRect` is non-empty when at least one character is in the selection range — currently no test asserts that `selectionStart_` and `selectionEnd_` produce a non-degenerate rectangle. | `src/textedit/TextEditTool.cpp:onPaintOverlay` (line 1386 `selRect`) | unit | `test_text_edit_selection_render.cpp` | P0 |
| GAP-SEL-03 | selection | Verify `painter.drawRect` is invoked exactly once per active selection (highlight + frame). Existing `test_text_edit_tool.cpp:PaintOverlayNoCrash` only checks no-throw; never counts draw calls or asserts highlight occurred. | `src/textedit/TextEditTool.cpp:onPaintOverlay` (line 1387) | unit | `test_text_edit_selection_render.cpp` | P0 |
| GAP-SEL-04 | selection | Selection survives a non-printing keystroke (e.g. Shift held alone, modifier-only key). `Key_Left` / `Key_Right` clear selection unless Shift is held — that contract is currently untested. | `src/textedit/TextEditTool.cpp:onKeyPress` (lines 1043–1081) | unit | `test_text_edit_selection_render.cpp` | P0 |
| GAP-SEL-05 | selection | Selection survives focus/blur cycle when click reactivates the same span. `test_text_edit_tool.cpp:ClickOnTextSelectsSpan` covers click-in but never click-out-and-back-in. | `src/textedit/TextEditTool.cpp:onMousePress` (lines 880–909, `selectSpan`) | unit | `test_text_edit_selection_render.cpp` | P0 |
| GAP-SEL-06 | selection | Selection clears on click outside the active span. `test_text_edit_tool.cpp:ClickOutsideDeselectsSpan` covers commitEdit but never asserts `selectionStart_/End_` are reset to -1 afterward. | `src/textedit/TextEditTool.cpp:onMousePress` (lines 883–888) | unit | `test_text_edit_selection_render.cpp` | P0 |
| GAP-SEL-07 | selection | Selection rect coordinates fall inside the active span's bbox — protects against "highlight drawn off-screen" regressions. | `src/textedit/TextEditTool.cpp:onPaintOverlay` (lines 1352–1387) | unit | `test_text_edit_selection_render.cpp` | P1 |
| GAP-SEL-08 | selection | Double-click word selection sets `selectionStart_/selectionEnd_` to the actual word boundary (Unicode-aware via `iswspace`). `test_text_edit_tool.cpp` has no double-click test at all. | `src/textedit/TextEditTool.cpp:onMouseDoubleClick` (lines 938–981) | unit | `test_text_edit_selection_render.cpp` | P1 |
| GAP-SEL-09 | selection | Drag-extend selection: `onMouseMove` with LeftButton held updates `selectionEnd_` and seeds `selectionStart_` from `cursorPosition_`. Today only Ctrl+A path is exercised — manual drag is untested. | `src/textedit/TextEditTool.cpp:onMouseMove` (lines 912–929) | unit | `test_text_edit_selection_render.cpp` | P1 |
| GAP-SEL-10 | selection | Backspace/Delete with a non-empty selection deletes the selection range, not the cursor neighbor. `test_text_edit_tool.cpp:BackspaceDeletesChar` covers cursor-at-end backspace but NOT range deletion. | `src/textedit/TextEditTool.cpp:onKeyPress` (lines 1016–1041) | unit | `test_text_edit_selection_render.cpp` | P1 |
| GAP-SEL-11 | selection | Typing replaces selection: `onTextInput` while `selectionStart_ != selectionEnd_` replaces the range (line 1106). No test asserts this; `KeyInputInsertsText` only inserts at cursor. | `src/textedit/TextEditTool.cpp:onTextInput` (lines 1095–1116) | unit | `test_text_edit_selection_render.cpp` | P1 |
| GAP-SEL-12 | selection | Selection rect honors RTL/LTR direction for languages where layout is right-to-left. Current code assumes LTR `selLeft < selRight` from `horizontalAdvance`. RTL path may be unimplemented; test should be `xfail`/`SKIP` with the reason recorded. | `src/textedit/TextEditTool.cpp:onPaintOverlay` (lines 1371–1387) | unit | `test_text_edit_selection_render.cpp` | P2 |
| GAP-SAVE-02 | save | Overlay font family persists through save→reopen→re-extract for vector PDF spans (set Arial via toolbar, save, reopen, assert page font reference resolves to Arial). `test_text_edit_integration.cpp:SaveAfterEdit` only asserts non-empty extraction — never inspects font name. | `src/textedit/ContentStreamEditor.cpp:applyEditsWithSubstitution`, `src/fonts/PdfFontEmbedder` | integration | `test_text_edit_save_roundtrip.cpp` | P0 |
| GAP-SAVE-03 | save | Overlay font size persists within ±1pt through save→reopen. Toolbar sets size to 14.0; reopened span's `fontSize` must be 13.0–15.0. | `src/textedit/ContentStreamEditor.cpp:canonicalTextMatrixForReplacement` (Tfs emission) | integration | `test_text_edit_save_roundtrip.cpp` | P0 |
| GAP-SAVE-04 | save | Bold toggle persists through save→reopen. `applyBold(true)` then save; reopened span has bold-weight font reference. No test exercises `TextEditTool::applyBold` past the toolbar widget level (`test_text_edit_toolbar.cpp` only checks button state). | `src/textedit/TextEditTool.cpp:applyBold` (line 1724); `ContentStreamEditor::applyEditsWithSubstitution` | integration | `test_text_edit_save_roundtrip.cpp` | P0 |
| GAP-SAVE-05 | save | Italic toggle persists through save→reopen — symmetric to GAP-SAVE-04. | `src/textedit/TextEditTool.cpp:applyItalic` (line 1735) | integration | `test_text_edit_save_roundtrip.cpp` | P1 |
| GAP-SAVE-06 | save | Underline persists through save→reopen as a content-stream decoration (PDF has no native underline; must be drawn as a line). Currently `applyUnderline` only sets `FormattingRun::underline` — no save-time emission verified. | `src/textedit/TextEditTool.cpp:applyUnderline` (line 1745); ContentStreamEditor underline emission | integration | `test_text_edit_save_roundtrip.cpp` | P1 |
| GAP-SAVE-07 | save | Strikethrough persists through save→reopen — symmetric to GAP-SAVE-06. | `src/textedit/TextEditTool.cpp:applyStrikethrough` (line 1752) | integration | `test_text_edit_save_roundtrip.cpp` | P1 |
| GAP-SAVE-08 | save | Multi-formatting-run span (e.g. "Hello **WORLD** there" with WORLD bold) round-trips correctly. Today `applyText` (TextEditCommands.cpp:135–161) embeds only the *dominant* run's font for the entire span — losing per-run distinctions on save. No test asserts per-run survival. | `src/textedit/TextEditCommands.cpp:applyText` (lines 117–161) | integration | `test_text_edit_save_roundtrip.cpp` | P0 |
| GAP-SAVE-09 | save | Image-dominated TRE save persists `fontMatch.sizePt`. The Tr=3 invisible-text path goes through `applyEdits(textRenderMode=3)` (TextEditCommands.cpp:114), and the visible XObject is updated separately via `refreshImageForScannedPage` (MainWindow.cpp:3481). The two paths' size values must agree on reopen. `test_textedit_tm_regression.cpp:EditSaveUndoCycleOnImageDominatedPage` asserts text appears but not size precision. | `src/ui/MainWindow.cpp:refreshImageForScannedPage`; `src/textedit/TextEditCommands.cpp:applyText` | integration | `test_text_edit_save_roundtrip.cpp` | P0 |
| GAP-SAVE-10 | save | Text color persists through save→reopen. `applyTextColor` (TextEditTool.cpp:1759) only stores RGB on `FormattingRun::textColor`; no test asserts the color reaches saved content stream's `rg` op. | `src/textedit/TextEditTool.cpp:applyTextColor` | integration | `test_text_edit_save_roundtrip.cpp` | P1 |
| GAP-SAVE-11 | save | Two consecutive saves of the same edited document produce byte-identical (or text-equivalent) output. Re-save must not double-encode or compound size errors. | `src/core/Document.cpp:save` + ContentStreamEditor | integration | `test_text_edit_save_roundtrip.cpp` | P2 |
| GAP-SAVE-12 | save | Saving with no edits performed (open → text-edit-mode → click span → cancelEdit → save) produces a content stream byte-identical to the original. Today's pipeline may run sanitize-filter on every save. | `src/textedit/TextEditTool.cpp:cancelEdit` | integration | `test_text_edit_save_roundtrip.cpp` | P2 |
| GAP-SAVE-13 | save | Edit on Lean Startup p2 (the canonical reproduction) preserves CharisSILRegular reference rather than swapping to Arial. The 0.48× CTM block is the documented Tm-sign trap from `memory/textedit_tm_sign_fix.md`. | `src/textedit/ContentStreamEditor.cpp:canonicalTextMatrixForReplacement` | integration | `test_text_edit_save_roundtrip.cpp` | P0 |
| GAP-UNDO-02 | undo | Undo of a single keystroke restores prior char-for-char content. Today `test_text_edit_integration.cpp:UndoRedoCycle` asserts `canUndo()/canRedo()` flags only — never compares text before/after. | `src/textedit/TextEditCommands.cpp:undo` (line 45) | integration | `test_text_edit_undo_chain.cpp` | P0 |
| GAP-UNDO-03 | undo | Undo of a merged keystroke run restores the pre-merge state (the original `oldText_` from cmd1, not an intermediate state from cmd2). `test_text_edit_commands.cpp:MergeSameSpan` asserts `merged == true` only — never invokes `cmd1->undo()` to verify the baseline survived. | `src/textedit/TextEditCommands.cpp:mergeWith` (line 68) + `undo` | integration | `test_text_edit_undo_chain.cpp` | P0 |
| GAP-UNDO-04 | undo | Redo after undo restores the edited state. `UndoRedoCycle` asserts `canRedo() == true` but never asserts the page text contains the redone content. | `src/textedit/TextEditCommands.cpp:redo` (line 51) | integration | `test_text_edit_undo_chain.cpp` | P0 |
| GAP-UNDO-05 | undo | Undo chain of N≥5 edits never blanks the line. This is the explicit reported-bug invariant for GAP-UNDO-01: 5 edits + 5 undos must restore the exact original text — no progressive corruption. | `src/textedit/TextEditCommands.cpp:undo`; constructor (oldFormattingRuns_ baseline) | integration | `test_text_edit_undo_chain.cpp` | P0 |
| GAP-UNDO-06 | undo | Undo after Save still works — the saved file does not invalidate the in-memory undo stack. Currently `test_text_edit_integration.cpp:SaveAfterEdit` saves but never calls `undoManager->undo()` afterward. | `src/core/Document.cpp:save`; `src/commands/UndoManager` | integration | `test_text_edit_undo_chain.cpp` | P1 |
| GAP-UNDO-07 | undo | Undo after a formatting change restores prior formatting. Critically: `applyBold/applyItalic/applyFontFamily/applyFontSize` mutate `model_->applyFormatting` directly (TextEditTool.cpp:1698) and *never construct an EditTextCommand* — so today there is **no undo for formatting changes at all**. Test must either lock the bug or drive the desired contract. | `src/textedit/TextEditTool.cpp:applyFormattingField` (line 1665); MainWindow undo wiring (line 2589) | integration | `test_text_edit_undo_chain.cpp` | P0 |
| GAP-UNDO-08 | undo | `mergeWith` must be safe to invoke on commands at different `pageIndex_`, returning `false` (existing test covers different `spanIndex_` only). The cross-page case is implicitly covered by `cmd0.pageIndex_ == cmd1.pageIndex_` precondition but never explicitly tested. | `src/textedit/TextEditCommands.cpp:mergeWith` (line 75) | unit | `test_text_edit_undo_chain.cpp` | P1 |
| GAP-UNDO-09 | undo | `mergeWith` associativity: `merge(merge(a,b),c).undo() == a.undo()` (the original `oldText_` survives transitively). Pipeline phase-6 acceptance requires this property test. | `src/textedit/TextEditCommands.cpp:mergeWith` | unit | `test_text_edit_undo_chain.cpp` | P1 |
| GAP-UNDO-10 | undo | Undo of an empty-replacement edit (delete-all in span) restores the original text. `test_text_edit_pipeline.cpp:EmptyReplacementRemovesText` covers the forward direction only — never undoes. | `src/textedit/TextEditCommands.cpp:undo` | integration | `test_text_edit_undo_chain.cpp` | P1 |
| GAP-UNDO-11 | undo | Image-dominated page edit: undo restores the *visible* XObject (not just the invisible OCR text layer). `imageEditStack_[].savedXObjectRef` (MainWindow.cpp:2597) is the production path — no test exercises it. | `src/ui/MainWindow.cpp:onUndo` (line 2589); `imageEditStack_` | integration | `test_text_edit_undo_chain.cpp` | P0 |
| GAP-UNDO-12 | undo | Undo after Find/Replace replaces text via `EditTextCommand` (MainWindow.cpp:2700) — verify the same undo invariants hold for non-interactive edits. | `src/ui/MainWindow.cpp:onReplace` (line 2700) | integration | `test_text_edit_undo_chain.cpp` | P2 |
| GAP-UNDO-13 | undo | Undo after Replace All (CompositeCommand at MainWindow.cpp:2730) reverses every constituent edit in one step. | `src/ui/MainWindow.cpp:onReplaceAll`; `commands::CompositeCommand` | integration | `test_text_edit_undo_chain.cpp` | P1 |
| GAP-FMT-01 | formatting | Toolbar font-family change actually invokes `TextEditTool::applyFontFamily` and updates the active span's formattingRuns. Today `test_text_edit_toolbar.cpp` covers signal emission but never end-to-end widget→tool→model. | `src/ui/TextEditToolBar.cpp` → `src/textedit/TextEditTool.cpp:applyFontFamily` wiring in `MainWindow.cpp` | integration | `test_text_edit_formatting_apply.cpp` | P1 |
| GAP-FMT-02 | formatting | Toolbar font-size change updates the active span's run.fontSize within the selected range. | `src/ui/TextEditToolBar.cpp` → `TextEditTool::applyFontSize` (line 1717) | integration | `test_text_edit_formatting_apply.cpp` | P1 |
| GAP-FMT-03 | formatting | Apply bold to entire span (no selection) — verify the run covers `[0, textLen)`. `applyFormattingField` lines 1675–1682 set start/end to span-wide when no selection — untested. | `src/textedit/TextEditTool.cpp:applyFormattingField` (line 1665) | unit | `test_text_edit_formatting_apply.cpp` | P1 |
| GAP-FMT-04 | formatting | Apply bold to a selection range — verify the run covers exactly `[selStart, selEnd)` and adjacent runs are split correctly (`applyFormatting` should preserve untouched neighboring runs). | `src/textedit/TextEditModel.cpp:applyFormatting` | unit | `test_text_edit_formatting_apply.cpp` | P1 |
| GAP-FMT-05 | formatting | Toggle bold off after toggle on returns to non-bold without leaving stale runs. `test_formatting_run.cpp:ClearFormattingTest` covers data-layer clear; no test exercises tool-layer toggle. | `src/textedit/TextEditTool.cpp:applyBold` (line 1724) | unit | `test_text_edit_formatting_apply.cpp` | P1 |
| GAP-FMT-06 | formatting | Bold + italic combined on the same selection produces a single FormattingRun with both flags set. `test_formatting_run.cpp:MergeAdjacentIdentical` covers merge of identical runs but not overlapping flag application. | `src/textedit/TextEditTool.cpp:applyBold` + `applyItalic` | unit | `test_text_edit_formatting_apply.cpp` | P2 |
| GAP-FMT-07 | formatting | Underline + strikethrough do NOT cause font-family re-resolution (look at `applyUnderline`/`applyStrikethrough` lines 1745–1757 — they intentionally don't call `fontResolver_->resolve`). Test locks this contract so future "tidy-up" doesn't regress it. | `src/textedit/TextEditTool.cpp:applyUnderline`, `applyStrikethrough` | unit | `test_text_edit_formatting_apply.cpp` | P2 |
| GAP-FMT-08 | formatting | `cancelEdit` clears formattingRuns and restores `originalText`. `test_formatting_run.cpp:CancelEditTest:ClearFormattingRestoresUnmodified` covers the model layer; no test exercises the tool's `cancelEdit` (`TextEditTool.cpp:1499`) end-to-end. | `src/textedit/TextEditTool.cpp:cancelEdit` (line 1494 area) | unit | `test_text_edit_formatting_apply.cpp` | P2 |
| GAP-FMT-09 | formatting | Alignment change emits repaint and updates `currentAlignment_`. `applyAlignment` (TextEditTool.cpp:1767) sets state but no test asserts the repaint signal fires. | `src/textedit/TextEditTool.cpp:applyAlignment` | unit | `test_text_edit_formatting_apply.cpp` | P2 |
| GAP-MULTI-01 | multi-span | Editing two adjacent spans in sequence (commit between) correctly creates two separate undo entries that both revert in order. `test_text_edit_integration.cpp:MultiplePageEdits` covers different *pages*; same-page-different-span is uncovered. | `src/ui/MainWindow.cpp:textEdited` (line 3430); UndoManager merge logic | integration | `test_text_edit_multi_span.cpp` | P1 |
| GAP-MULTI-02 | multi-span | Edit followed by clicking into a different span on the same page commits the first edit (TextEditTool.cpp:891–896) and records it as one undo entry — verify only one EditTextCommand is pushed. | `src/textedit/TextEditTool.cpp:onMousePress` (commit-on-different-span) | unit | `test_text_edit_multi_span.cpp` | P1 |
| GAP-MULTI-03 | multi-span | Span model rebuilds after an edit do NOT invalidate the cached span-index used by an in-flight undo command. `test_text_edit_model.cpp:ConstructFromMultipleBlocks` builds the model once but never tests rebuild-after-edit. | `src/textedit/TextEditTool.cpp:rebuildModel` | unit | `test_text_edit_multi_span.cpp` | P1 |
| GAP-MULTI-04 | multi-span | Find/Replace across multiple spans in one document creates a CompositeCommand whose undo reverses every constituent edit atomically (no partial revert on second-undo). | `src/ui/MainWindow.cpp:onReplaceAll` (line 2722) | integration | `test_text_edit_multi_span.cpp` | P2 |
| GAP-MULTI-05 | multi-span | Editing a span containing existing FormattingRuns (multi-format paragraph) preserves runs that are wholly outside the edit range. `test_formatting_run.cpp:RunShiftingTest` covers shift math at the data layer but not the EditTextCommand-driven path. | `src/textedit/TextEditCommands.cpp:applyText` + `TextEditModel::applyFormatting` | integration | `test_text_edit_multi_span.cpp` | P1 |
| GAP-TRE-01 | tre-image | On image-dominated pages, edit→save→reopen preserves the visible bitmap matching the new text. `test_textedit_tm_regression.cpp:EditSaveUndoCycleOnImageDominatedPage` asserts text is extractable but never pixel-tests the visible XObject. | `src/ui/MainWindow.cpp:refreshImageForScannedPage` (line 3481) | ui-screenshot | `test_ui_text_edit_tre_save.py` | P1 |
| GAP-TRE-02 | tre-image | TRE edit on image-dominated page does not strip per-word style overlays (`overlayWordStyles_`) for unedited words. `test_image_text_edit_bugs.cpp:ShouldSkipStyledRunsWhenEdited` covers the boolean guard but not multi-word editing where one word is edited and another isn't. | `src/textedit/TextEditTool.cpp:setOverlayWordStyles` + render path (line 1308) | unit | `test_text_edit_tre_styles.cpp` | P1 |
| GAP-TRE-03 | tre-image | Atlas-mode overlay (`overlayEnrichedAtlas_` set) renders selection rect at HOCR-aligned position rather than PDF bbox position. The atlasMode branch at TextEditTool.cpp:1376–1385 is currently exercised only by `test_image_text_edit_bugs.cpp:AtlasSetterWorks` which asserts setter only. | `src/textedit/TextEditTool.cpp:onPaintOverlay` (atlas selection branch, lines 1357–1385) | unit | `test_text_edit_tre_styles.cpp` | P1 |
| GAP-TRE-04 | tre-image | Saving an image-dominated page edit emits Tr=3 (invisible) for the OCR text layer. `TextEditCommands.cpp:114` documents this contract — no test verifies the saved content stream actually contains `3 Tr` in the reopened file. | `src/textedit/ContentStreamEditor.cpp:applyEdits(textRenderMode=3)` | integration | `test_text_edit_tre_styles.cpp` | P1 |
| GAP-TRE-05 | tre-image | `imageEditStack_` lossless-undo path: editing twice → undo twice restores both the content stream AND the original XObject ref (not the intermediate one). MainWindow.cpp:2589 onUndo path — production-only, untested. | `src/ui/MainWindow.cpp:onUndo` (line 2589 imageEditStack handling) | integration | `test_text_edit_tre_styles.cpp` | P0 |
| GAP-TRE-06 | tre-image | Image-dominated page detection (`page->isImageDominated()`, called at TextEditCommands.cpp:106) is stable across edit + save + reopen — i.e. the saved page stays classified as image-dominated. | `src/core/Page.cpp:isImageDominated`; ContentStreamEditor | integration | `test_text_edit_tre_styles.cpp` | P2 |
| GAP-UI-01 | selection (ui-screenshot) | Pixel-level proof: open Lean Startup p2, click into a body-text span, hold-drag to select 5 chars; resulting screenshot shows a non-degenerate blue highlight rectangle within the span bbox. This is the only way to catch zero-width-selRect rendering bugs because `painter.drawRect` with a zero-width rect emits no pixels. | `src/textedit/TextEditTool.cpp:onPaintOverlay` end-to-end via PdfCanvas | ui-screenshot | `test_ui_text_edit_selection.py` | P0 |
| GAP-UI-02 | save (ui-screenshot) | Pixel-level proof: edit a Lean Startup p2 word, change toolbar font to Times New Roman size 18, save, reopen the saved file in PDFE, screenshot of the same page region matches a baseline rendering of the edited content (not the unedited content). | `src/textedit/ContentStreamEditor.cpp` end-to-end | ui-screenshot | `test_ui_text_edit_save.py` | P1 |
| GAP-UI-03 | undo (ui-screenshot) | Pixel-level proof: 5 sequential keystrokes on Lean Startup p2 + 5 Ctrl+Z; final screenshot must match the pristine-page baseline screenshot pixel-for-pixel (within tolerance for font hinting). | `src/textedit/TextEditCommands.cpp` end-to-end via UndoManager | ui-screenshot | `test_ui_text_edit_undo.py` | P0 |
| GAP-UI-04 | formatting (ui-screenshot) | Toolbar font-size spinbox change while a selection is active produces a visible size change in the overlay before save. Tests overlay path only (no save). | `src/textedit/TextEditTool.cpp:applyFontSize` overlay render path | ui-screenshot | `test_ui_text_edit_formatting.py` | P1 |
| GAP-UI-05 | selection (ui-screenshot) | Click outside the canvas widget (e.g. on the page panel) commits the edit and clears the selection rectangle. Screenshot before-and-after click — selection rect must be absent post-blur. | `src/textedit/TextEditTool.cpp:commitEdit` (focus-out path) | ui-screenshot | `test_ui_text_edit_selection.py` | P2 |
| GAP-MISC-01 | save | EditTextCommand stores `spanInfo_` by value at construction time. If the model is rebuilt before `redo()`, the stored bbox becomes stale. No test verifies redo correctness after a model rebuild. | `src/textedit/TextEditCommands.cpp:redo` (line 51) + `applyText` | integration | `test_text_edit_undo_chain.cpp` | P2 |
| GAP-MISC-02 | save | `Document::markPageDirty` (called at TextEditCommands.cpp:184) invalidates the display-list cache so the next render picks up the edit. Test asserts the dirty flag actually flips and re-render happens. | `src/core/Document.cpp:markPageDirty`; ContentStreamEditor save flow | integration | `test_text_edit_save_roundtrip.cpp` | P2 |

---

## Summary

| priority | count |
|---|---|
| **P0** | 23 |
| **P1** | 29 |
| **P2** | 13 |
| **Total** | **65** |

Acceptance check against pipeline.json criterion 5:

- ✓ Total gap count ≥ 25 (delivered 65).
- ✓ P0 rows ≥ 8 (delivered 23).
- ✓ Three reported bugs tagged GAP-SEL-01, GAP-SAVE-01, GAP-UNDO-01 and marked P0.
- ✓ Inventory enumerates the explicit selection / save / undo invariants required by criterion 3.
- ✓ Every row references existing test files / production files; gaps proven against the 105-test baseline.
- ✓ No production source files modified by this document.

### Gap distribution by category (rows in the table only — does not double-count UI rows)

| category | count | id range |
|---|---|---|
| selection | 12 | GAP-SEL-01 .. GAP-SEL-12 |
| save | 13 | GAP-SAVE-01 .. GAP-SAVE-13 |
| undo | 13 | GAP-UNDO-01 .. GAP-UNDO-13 |
| formatting | 9 | GAP-FMT-01 .. GAP-FMT-09 |
| multi-span | 5 | GAP-MULTI-01 .. GAP-MULTI-05 |
| tre-image | 6 | GAP-TRE-01 .. GAP-TRE-06 |
| ui-screenshot | 5 | GAP-UI-01 .. GAP-UI-05 (cross-cuts selection / save / undo / formatting) |
| misc | 2 | GAP-MISC-01 .. GAP-MISC-02 |
| **Total** | **65** | |

### Most surprising gap

**GAP-UNDO-07** — The formatting API on `TextEditTool` (`applyBold`,
`applyItalic`, `applyFontFamily`, `applyFontSize`, `applyUnderline`,
`applyStrikethrough`, `applyTextColor`) mutates the model directly via
`TextEditModel::applyFormatting` and emits a repaint, but **never
constructs an `EditTextCommand` and never reaches `UndoManager`**.
That means today, formatting changes are **silently un-undoable** —
Ctrl+Z after toggling bold does nothing. `EditTextCommand`'s
`oldFormattingRuns_` baseline is empty exactly because formatting
changes never go through the command pipeline that would have
populated it.

This is the structural root of GAP-UNDO-01: when a text-edit *does*
fire (which goes through the command pipeline), it captures whatever
ad-hoc `formattingRuns` the model happens to hold at that moment as
`newFormattingRuns_` — and a baseline of *empty* as `oldFormattingRuns_`.
Undoing therefore wipes formatting state that was never the command's
to wipe. Phase 6 cannot fix this with a tweak to `mergeWith()`; it
requires routing the formatting API through `EditTextCommand` (or a
new `FormattingCommand`) so the undo stack owns formatting transitions
the same way it owns text transitions.
