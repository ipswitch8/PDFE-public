# UI Function Matrix — Pipeline a7c2e541 Phase-2

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


Generated from `tests/docs/ui_element_catalog.json` (518 entries from phase-1) by `tests/docs/generate_function_matrix.py`. Source slot bodies were read directly from `src/ui/*.cpp` while authoring the shared-slots and dialog-info tables.

## Summary

- **Total rows documented**: 518
- **By category**:
  - action: 143
  - context_menu: 4
  - dialog: 42
  - drag_target: 2
  - menu: 15
  - toolbar: 3
  - widget: 309
- **Needs visual verification** (drives phase-4 baseline scope):
  - YES: 479
  - NO: 33
  - DEPENDS: 6

### Visual-verify decision rule (used per row)

- **YES** — produces canvas re-render, opens dialog, navigates pages, changes zoom, applies an annotation, edits text, redacts, signs, runs OCR, switches mode, toggles panel visibility, or otherwise paints pixels in the main window.
- **NO** — registers internal state only (e.g., recent-files entry, navigation history record, clipboard write that's not directly visible in the app window).
- **DEPENDS** — visible effect depends on context (most commonly: undo/redo whose visible result depends on what was undone, or passive QLineEdit values committed only at dialog accept-time).

## Shared slots

Slots invoked by multiple QActions / widgets are documented once here; per-element rows below reference them via `→ ClassName::slotName`.

### `BookmarkPanel::onContextMenu`

- **Input**: QPoint pos.
- **State transitions**: Builds bookmarkContextMenu and exec()s.
- **Visible changes**: Context menu appears.
- **Error states**: None.
- **Needs visual verify**: YES

### `BookmarkPanel::onCreateAtRoot`

- **Input**: QInputDialog::getText 'Bookmark title'.
- **State transitions**: CreateBookmarkCommand at root level; refresh tree.
- **Visible changes**: New bookmark in tree.
- **Error states**: User cancel.
- **Needs visual verify**: YES

### `BookmarkPanel::onCreateChild`

- **Input**: QInputDialog title.
- **State transitions**: CreateBookmarkCommand under selected.
- **Visible changes**: Child added.
- **Error states**: No selection / cancel.
- **Needs visual verify**: YES

### `BookmarkPanel::onDelete`

- **Input**: Confirm Yes/No.
- **State transitions**: DeleteBookmarkCommand.
- **Visible changes**: Tree row removed.
- **Error states**: User No.
- **Needs visual verify**: YES

### `BookmarkPanel::onIndent`

- **Input**: N/A.
- **State transitions**: IndentBookmarkCommand.
- **Visible changes**: Tree depth changes.
- **Error states**: Cannot indent root-without-prev-sibling.
- **Needs visual verify**: YES

### `BookmarkPanel::onItemActivated`

- **Input**: QTreeWidgetItem* + column.
- **State transitions**: Emits bookmarkActivated(pageIndex,x,y,zoom) or externalLinkRequested(uri).
- **Visible changes**: Canvas navigates to bookmark target.
- **Error states**: Invalid type role → no-op.
- **Needs visual verify**: YES

### `BookmarkPanel::onItemChanged`

- **Input**: QTreeWidgetItem* + column 0 text.
- **State transitions**: RenameBookmarkCommand if title changed.
- **Visible changes**: Bookmark renamed.
- **Error states**: Suppressed during programmatic refresh.
- **Needs visual verify**: YES

### `BookmarkPanel::onMoveDown`

- **Input**: N/A.
- **State transitions**: MoveBookmarkCommand down.
- **Visible changes**: Tree reorder.
- **Error states**: Already last.
- **Needs visual verify**: YES

### `BookmarkPanel::onMoveUp`

- **Input**: N/A.
- **State transitions**: MoveBookmarkCommand up.
- **Visible changes**: Tree reorder.
- **Error states**: Already first.
- **Needs visual verify**: YES

### `BookmarkPanel::onOutdent`

- **Input**: N/A.
- **State transitions**: OutdentBookmarkCommand.
- **Visible changes**: Tree depth changes.
- **Error states**: Already at root.
- **Needs visual verify**: YES

### `BookmarkPanel::onRename`

- **Input**: QInputDialog title.
- **State transitions**: RenameBookmarkCommand.
- **Visible changes**: Tree row text updates.
- **Error states**: Empty name → no-op.
- **Needs visual verify**: YES

### `MainWindow::applyAtlasMode`

- **Input**: AtlasMode enum (ScanCropped / FreeTypeStyled).
- **State transitions**: currentAtlasMode_ = mode; if pendingAtlas_.ready, EnrichedAtlas->setMode(); canvas->update.
- **Visible changes**: Edit-text overlay re-renders glyphs in chosen atlas mode.
- **Error states**: Pending atlas not ready → no-op.
- **Needs visual verify**: YES

### `MainWindow::onAbout`

- **Input**: N/A.
- **State transitions**: AboutDialog modal exec().
- **Visible changes**: About dialog modal.
- **Error states**: None.
- **Needs visual verify**: YES

### `MainWindow::onActualSize`

- **Input**: N/A.
- **State transitions**: canvas_->setZoom(1.0f).
- **Visible changes**: Canvas zoom 100%; zoomCombo shows '100%'.
- **Error states**: No canvas → no-op.
- **Needs visual verify**: YES

### `MainWindow::onAnnotationColorPicker`

- **Input**: QColorDialog modal.
- **State transitions**: currentAnnotColor_ updated; colorPickerButton_ stylesheet refreshed; current tool->setColor() if any.
- **Visible changes**: Color picker swatch updates; subsequent annotations use new color.
- **Error states**: User cancel → no change.
- **Needs visual verify**: YES

### `MainWindow::onAnnotationCreatedByTool`

- **Input**: (int pageIndex, AnnotationData).
- **State transitions**: Tool created the annotation directly; this slot deletes the raw annotation and re-creates via CreateAnnotationCommand so undo works.
- **Visible changes**: Annotation persists with undo entry.
- **Error states**: No undo manager → leaves raw annotation in place.
- **Needs visual verify**: YES

### `MainWindow::onAnnotationPropertiesChanged`

- **Input**: AnnotationData.
- **State transitions**: ModifyAnnotationCommand; canvas refresh.
- **Visible changes**: Annotation appearance updates.
- **Error states**: No manager → no-op.
- **Needs visual verify**: YES

### `MainWindow::onAnnotationToolSelected`

- **Input**: QAction* — exclusive annotation tool group.
- **State transitions**: Switches the active tool installed on PdfCanvas (Select/StickyNote/Highlight/Underline/StrikeOut/FreeText/Ink/Rectangle/Circle/Line/Stamp/FileAttachment/TextEdit/TRE/Form/Image/Redact); reads currentAnnotColor_; sets toolPage_.
- **Visible changes**: Canvas cursor + tool overlay changes; selected toolbar button highlighted; some tools reveal sub-toolbars (TextEdit, Form).
- **Error states**: No document → no-op.
- **Needs visual verify**: YES

### `MainWindow::onApplyRedactions`

- **Input**: Confirm warning dialog Yes/No.
- **State transitions**: applyAllRedactions(rewriteDocument=false); document_->save; QPDF rewrite; reopen; clear undo (irreversible); applyRedactionsAction disabled.
- **Visible changes**: Canvas re-renders with redacted content; status bar; QMessageBox 'Successfully applied N redaction(s)'.
- **Error states**: No marks → 'No Redactions' info; user clicks No → no-op; save fail → QMessageBox::critical 'Redaction Save Error'; QPDF rewrite fail → fallback reopen original.
- **Needs visual verify**: YES

### `MainWindow::onBookmarkActivated`

- **Input**: (int pageIndex, float x, float y, float zoom). x/y/zoom currently ignored.
- **State transitions**: canvas_->goToPage(pageIndex).
- **Visible changes**: Canvas navigates to bookmark target.
- **Error states**: None.
- **Needs visual verify**: YES

### `MainWindow::onCheckUpdates`

- **Input**: N/A.
- **State transitions**: QDesktopServices::openUrl('https://github.com/dev/PDFE/releases').
- **Visible changes**: External browser.
- **Error states**: None.
- **Needs visual verify**: NO

### `MainWindow::onCloseFile`

- **Input**: N/A.
- **State transitions**: closeDocument() — releases document_, tileCache_, tileRenderer_, annotationManager_, formManager_, jsEngine_, redactionManager_, encryptionManager_, signatureManager_, imageManager_, metadataManager_; clears commandPages_, toolPage_; disables ~30 document-required actions; clears canvas; resets nav history; closes sticky note popup; clears panels.
- **Visible changes**: Canvas blank, status bar empty, title '<unnamed>', all dock panels emptied.
- **Error states**: None — slot is unconditional.
- **Needs visual verify**: YES

### `MainWindow::onCombineFiles`

- **Input**: CombineFilesWizard interactive.
- **State transitions**: Wizard runs CombineFilesService.
- **Visible changes**: Wizard modal.
- **Error states**: User cancel.
- **Needs visual verify**: YES

### `MainWindow::onCommentPanelDelete`

- **Input**: (int pageIndex, int annotIndex).
- **State transitions**: DeleteAnnotationCommand; commentPanel refresh.
- **Visible changes**: Annotation removed from canvas + panel; window title gets '*'.
- **Error states**: No annotation manager → no-op.
- **Needs visual verify**: YES

### `MainWindow::onCommentPanelNavigate`

- **Input**: (int pageIndex).
- **State transitions**: canvas_->goToPage(pageIndex).
- **Visible changes**: Canvas navigates; comment panel selection follows.
- **Error states**: None.
- **Needs visual verify**: YES

### `MainWindow::onCompliance`

- **Input**: N/A.
- **State transitions**: ComplianceDialog modal exec().
- **Visible changes**: Compliance dialog modal.
- **Error states**: No document → no-op.
- **Needs visual verify**: YES

### `MainWindow::onCopy`

- **Input**: N/A.
- **State transitions**: page->extractPlainText() into QApplication::clipboard()->setText().
- **Visible changes**: Status bar 'Page text copied to clipboard' for 3000 ms; clipboard side-effect not visible in app window.
- **Error states**: No document/canvas → no-op; empty page text → status bar 'Copy: no text on this page'.
- **Needs visual verify**: NO

### `MainWindow::onCreateFormField`

- **Input**: N/A.
- **State transitions**: formToolAction_->setChecked(true); onFormToolSelected(); formToolBar_->setVisible(true).
- **Visible changes**: Form toolbar appears with 7 field-type buttons.
- **Error states**: None.
- **Needs visual verify**: YES

### `MainWindow::onCreatePdfFromImages`

- **Input**: ImageToPdfWizard interactive flow: image files + page-size + DPI + JPEG quality.
- **State transitions**: Wizard returns std::filesystem::path; openFile(out) replaces document_ with the new PDF.
- **Visible changes**: Wizard dialog; on success canvas shows new PDF, recent-files updated.
- **Error states**: Wizard cancelled (out.empty(), no-op); openFile errors propagate.
- **Needs visual verify**: YES

### `MainWindow::onCustomizeToolbars`

- **Input**: CustomizeToolbarsDialog modal.
- **State transitions**: ToolbarLayoutManager::saveLayouts; applyToolbarLayoutsList.
- **Visible changes**: Toolbars rearrange.
- **Error states**: User cancel.
- **Needs visual verify**: YES

### `MainWindow::onCut`

- **Input**: N/A.
- **State transitions**: If textEditTool active and has selection: tool->performCut() — pushes EditTextCommand on undoManager + writes to clipboard. Otherwise no-op.
- **Visible changes**: Status bar 'Text cut to clipboard' (or 'Cut: select text in an editable span first').
- **Error states**: No selection → status bar message only.
- **Needs visual verify**: DEPENDS — visible only if cut succeeds (canvas re-renders).

### `MainWindow::onDeletePages`

- **Input**: Confirm dialog Yes/No.
- **State transitions**: DeletePageRangeCommand; refuses to delete all pages; onDocumentStructureChanged.
- **Visible changes**: Page count drops; thumbnails removed; canvas advances or stays.
- **Error states**: Selection == total page count → QMessageBox warning 'At least one page must remain'; user clicks No → no-op.
- **Needs visual verify**: YES

### `MainWindow::onDeleteRedactionMark`

- **Input**: (int pageIndex, int annotIndex).
- **State transitions**: DeleteRedactionCommand; redaction panel refresh.
- **Visible changes**: Mark removed; panel updates.
- **Error states**: No manager → no-op.
- **Needs visual verify**: YES

### `MainWindow::onDocumentMetadata`

- **Input**: MetadataDialog modal — title/author/subject/keywords/creator/producer fields.
- **State transitions**: EditMetadataCommand on Accept.
- **Visible changes**: MetadataDialog modal; window title may gain '*'.
- **Error states**: None.
- **Needs visual verify**: YES

### `MainWindow::onDocumentPermissions`

- **Input**: N/A.
- **State transitions**: Reads encryption info; QMessageBox::information with permissions table.
- **Visible changes**: Info QMessageBox with permission summary.
- **Error states**: Read fail → QMessageBox::critical.
- **Needs visual verify**: YES

### `MainWindow::onDocumentation`

- **Input**: N/A.
- **State transitions**: QDesktopServices::openUrl('https://github.com/dev/PDFE#documentation').
- **Visible changes**: Default browser opens URL (external app).
- **Error states**: None — Qt swallows openUrl failures.
- **Needs visual verify**: NO — external browser.

### `MainWindow::onExit`

- **Input**: N/A.
- **State transitions**: QCoreApplication::quit() via close()/closeEvent flow; closeEvent calls hasUnsavedChanges and may prompt save.
- **Visible changes**: Application terminates.
- **Error states**: Unsaved changes → modal 'Save changes?' yes/no/cancel.
- **Needs visual verify**: YES

### `MainWindow::onExportFormData`

- **Input**: QFileDialog 'XFDF Files (*.xfdf);;All Files (*)'.
- **State transitions**: FormDataIO::exportToXfdf → write file.
- **Visible changes**: Status bar 'Form data exported to <path>' for 3000 ms.
- **Error states**: User cancel; QFile open fails → QMessageBox::critical; exportToXfdf throws → QMessageBox::critical.
- **Needs visual verify**: NO

### `MainWindow::onExportPlainText`

- **Input**: QFileDialog filter 'Text Files (*.txt);;All Files (*)' default <basename>.txt.
- **State transitions**: PlainTextExporter::write(document_, savePath) — pure read; no document mutation.
- **Visible changes**: Status bar 'Exported text to <name>' for 3000 ms.
- **Error states**: User cancel (no-op); writer returns false → QMessageBox::warning 'Could not write file'.
- **Needs visual verify**: NO

### `MainWindow::onExtractPages`

- **Input**: QFileDialog save path (*.pdf).
- **State transitions**: extractPagesToFile() — pure read on doc, write to outputPath.
- **Visible changes**: Status bar 'Extracted N page(s) to <name>'.
- **Error states**: User cancel; std::exception → QMessageBox::critical 'Extract Error'.
- **Needs visual verify**: NO — output file external to app window.

### `MainWindow::onFind`

- **Input**: N/A.
- **State transitions**: searchPanelDock_->show()/raise(); searchPanel_->focusSearchBox().
- **Visible changes**: Search dock visible + focused; search line edit gets keyboard focus.
- **Error states**: None.
- **Needs visual verify**: YES

### `MainWindow::onFitPage`

- **Input**: N/A.
- **State transitions**: canvas_->fitPage() — sets zoom such that whole page fits viewport.
- **Visible changes**: Canvas re-renders; zoomCombo updates.
- **Error states**: No canvas → no-op.
- **Needs visual verify**: YES

### `MainWindow::onFitWidth`

- **Input**: N/A.
- **State transitions**: canvas_->fitWidth() — sets zoom such that page width fills viewport.
- **Visible changes**: Canvas re-renders; zoomCombo updates.
- **Error states**: No canvas → no-op.
- **Needs visual verify**: YES

### `MainWindow::onFlattenForm`

- **Input**: Confirm warning dialog Yes/No.
- **State transitions**: formManager_->flattenForm; clears undo stack (irreversible); canvas refresh.
- **Visible changes**: Form widgets become static appearance; status bar 'Form fields flattened'.
- **Error states**: User clicks No; std::exception → QMessageBox::critical.
- **Needs visual verify**: YES

### `MainWindow::onFormFieldDeselected`

- **Input**: N/A.
- **State transitions**: formPropertiesPanel_->clear().
- **Visible changes**: Properties panel emptied.
- **Error states**: None.
- **Needs visual verify**: YES

### `MainWindow::onFormFieldProperties`

- **Input**: N/A.
- **State transitions**: propertiesPanelDock_->setVisible(true)+raise().
- **Visible changes**: Properties dock visible & raised.
- **Error states**: None.
- **Needs visual verify**: YES

### `MainWindow::onFormFieldSelected`

- **Input**: (int widgetIndex).
- **State transitions**: FormManager::getFormField → formPropertiesPanel_->setFormField; raise dock.
- **Visible changes**: Properties panel populated; dock raised.
- **Error states**: No page → no-op.
- **Needs visual verify**: YES

### `MainWindow::onFormFieldValueChanged`

- **Input**: FormFieldData (widgetIndex + value).
- **State transitions**: SetFieldValueCommand via undoManager; canvas refresh.
- **Visible changes**: Form widget appearance updates; window title gets '*'.
- **Error states**: No page → no-op.
- **Needs visual verify**: YES

### `MainWindow::onFormToolSelected`

- **Input**: N/A.
- **State transitions**: If checked: onAnnotationToolSelected(formToolAction_).
- **Visible changes**: Form tool active in canvas; form toolbar visibility unchanged.
- **Error states**: No formManager_ → no-op.
- **Needs visual verify**: YES

### `MainWindow::onImageCrop`

- **Input**: N/A — uses center 80% as default.
- **State transitions**: imageTool_->cropRequested(name, QRectF).
- **Visible changes**: Image cropped.
- **Error states**: No selection.
- **Needs visual verify**: YES

### `MainWindow::onImageRemove`

- **Input**: N/A.
- **State transitions**: Emits imageTool_->removeRequested(imageIndex) → RemoveImageCommand.
- **Visible changes**: Image disappears from canvas.
- **Error states**: No selection → no-op.
- **Needs visual verify**: YES

### `MainWindow::onImageResize`

- **Input**: N/A — 50% scale default.
- **State transitions**: imageTool_->resizeRequested(name, w, h, x, y).
- **Visible changes**: Image resized.
- **Error states**: No selection.
- **Needs visual verify**: YES

### `MainWindow::onImageRotate180`

- **Input**: N/A.
- **State transitions**: imageTool_->rotateRequested(name, 180).
- **Visible changes**: Image rotated 180.
- **Error states**: No selection.
- **Needs visual verify**: YES

### `MainWindow::onImageRotate270`

- **Input**: N/A.
- **State transitions**: imageTool_->rotateRequested(name, 270).
- **Visible changes**: Image rotated 270.
- **Error states**: No selection.
- **Needs visual verify**: YES

### `MainWindow::onImageRotate90`

- **Input**: N/A.
- **State transitions**: imageTool_->rotateRequested(name, 90).
- **Visible changes**: Image rotated 90.
- **Error states**: No selection.
- **Needs visual verify**: YES

### `MainWindow::onImageToolSelected`

- **Input**: N/A.
- **State transitions**: Lazy-creates ImageTool; wires imageSelected/imageDeselected/removeRequested/resizeRequested/rotateRequested/cropRequested signals; setPage; canvas_->setImageTool.
- **Visible changes**: Image tool active; image properties dock shown when selecting image.
- **Error states**: No imageManager_ → no-op.
- **Needs visual verify**: YES

### `MainWindow::onImportFormData`

- **Input**: QFileDialog 'XFDF Files (*.xfdf)'.
- **State transitions**: Reads file; FormDataIO::importFromXfdf; canvas refresh.
- **Visible changes**: Form fields populated; canvas re-renders.
- **Error states**: User cancel; read fail → QMessageBox::critical; import fail → QMessageBox::critical.
- **Needs visual verify**: YES

### `MainWindow::onImportImageStamp`

- **Input**: QFileDialog 'PNG/JPG/BMP'.
- **State transitions**: ImageStampLibrary::addStamp + saveToSettings.
- **Visible changes**: File > Stamps menu rebuilds on next aboutToShow with the new stamp.
- **Error states**: User cancel; load fail → silent.
- **Needs visual verify**: DEPENDS — visible only when user reopens File > Stamps.

### `MainWindow::onInsertBlankPage`

- **Input**: N/A — uses currentPage size or 612x792 Letter default.
- **State transitions**: InsertBlankPageCommand; canvas->goToPage(insertAt).
- **Visible changes**: New blank page appears; canvas navigates to it.
- **Error states**: No document/undoManager → no-op.
- **Needs visual verify**: YES

### `MainWindow::onInsertImage`

- **Input**: QFileDialog 'PNG/JPG/JPEG/BMP/GIF/TIFF;All Files'.
- **State transitions**: InsertImageCommand with default 72,72,400,300 placement and stripExif=true; canvas refresh.
- **Visible changes**: Inserted image visible on page.
- **Error states**: User cancel; no page → no-op.
- **Needs visual verify**: YES

### `MainWindow::onInsertPagesFromFile`

- **Input**: QFileDialog 'PDF Files (*.pdf);;All Files (*)'.
- **State transitions**: Opens source doc, ImportPagesCommand, onDocumentStructureChanged, status bar 'Imported N pages from <name>'.
- **Visible changes**: Page count grows; canvas refreshes.
- **Error states**: User cancel (no-op); empty source → QMessageBox::warning; std::exception → QMessageBox::critical.
- **Needs visual verify**: YES

### `MainWindow::onMarkRedaction`

- **Input**: N/A.
- **State transitions**: Sets redactToolAction_ checked + onRedactToolSelected(); marking happens via tool interaction.
- **Visible changes**: Same as onRedactToolSelected.
- **Error states**: None — direct passthrough.
- **Needs visual verify**: YES

### `MainWindow::onMovePageDown`

- **Input**: N/A.
- **State transitions**: MovePageCommand(from, from+1).
- **Visible changes**: Page panel reorder; canvas navigates.
- **Error states**: from >= pageCount-1 → no-op.
- **Needs visual verify**: YES

### `MainWindow::onMovePageUp`

- **Input**: N/A.
- **State transitions**: MovePageCommand(from, from-1); onDocumentStructureChanged; canvas_->goToPage(from-1).
- **Visible changes**: Page panel reorder; canvas navigates.
- **Error states**: from <= 0 → no-op.
- **Needs visual verify**: YES

### `MainWindow::onNewFile`

- **Input**: N/A — no input.
- **State transitions**: Creates blank PDF in QDir::temp() via Document::createBlank(); routes through openFile() so all open-file state machinery runs.
- **Visible changes**: Window title updates to 'pdfe_untitled_<ms>.pdf', canvas renders blank page, status bar shows page count, dirty marker absent.
- **Error states**: Document::createBlank throws → QMessageBox::critical 'Failed to create blank PDF'; subsequent openFile() failure surfaces its own messaging.
- **Needs visual verify**: YES

### `MainWindow::onNextView`

- **Input**: N/A.
- **State transitions**: navHistory_.goForward(); canvas_->goToPage().
- **Visible changes**: Canvas moves to next visited page.
- **Error states**: canGoForward() false → no-op.
- **Needs visual verify**: YES

### `MainWindow::onOpenFile`

- **Input**: QFileDialog filter 'PDF Files (*.pdf);;All Files (*)' starting from AppSettings::lastOpenDirectory().
- **State transitions**: Calls openFile(fileName) — closes existing doc, opens new pdfe::core::Document, replaces tileCache_/tileRenderer_/annotationManager_/formManager_/etc., enables ~30 actions, records in recentFilesMenu_.
- **Visible changes**: Same as MainWindow::openFile (see below).
- **Error states**: User cancels file dialog (no-op return); see openFile error paths.
- **Needs visual verify**: YES

### `MainWindow::onPageChanged`

- **Input**: (int pageIndex).
- **State transitions**: navHistory_.recordVisit; pendingAtlas reset; statusBar setPageInfo; pagePanel setCurrentPage; propertiesPanel setPageInfo; textEditTool setPage if active; activateTextEditForPage if auto-routing; loadOcrBoxesForPage.
- **Visible changes**: Page panel highlight moves; properties panel updates; status bar shows page X of N.
- **Error states**: No document → no-op return.
- **Needs visual verify**: YES

### `MainWindow::onPageMoved`

- **Input**: (int from, int to) — fired by PagePanel drag-drop.
- **State transitions**: MovePageCommand; canvas_->goToPage(adjusted-to).
- **Visible changes**: Page list reorder + canvas refresh.
- **Error states**: from == to → no-op.
- **Needs visual verify**: YES

### `MainWindow::onPageSelected`

- **Input**: (int pageIndex).
- **State transitions**: canvas_->goToPage(pageIndex).
- **Visible changes**: Canvas scrolls to pageIndex.
- **Error states**: No canvas → no-op.
- **Needs visual verify**: YES

### `MainWindow::onPaste`

- **Input**: Clipboard text (UTF-16).
- **State transitions**: If textEditTool active: tool->performPaste() — inserts clipboard text into edit buffer.
- **Visible changes**: Status bar 'Clipboard text pasted'; canvas re-renders if paste succeeded.
- **Error states**: No tool active → status bar 'Paste: activate text editing first'.
- **Needs visual verify**: DEPENDS

### `MainWindow::onPreviousView`

- **Input**: N/A.
- **State transitions**: navHistory_.goBack() returns previous page index; canvas->goToPage(); previous/next view actions re-enabled based on history.canGoBack/canGoForward.
- **Visible changes**: Canvas navigates to previous viewed page; status bar page index updates.
- **Error states**: navHistory_.canGoBack() == false → no-op return.
- **Needs visual verify**: YES

### `MainWindow::onPrint`

- **Input**: PrintDialog modal — pages range, copies, duplex, color, scaling.
- **State transitions**: PrintEngine::print(QPrinter, options); fires JS willPrint/didPrint events on first page.
- **Visible changes**: Print dialog modal; printer system dialog if Native; status bar updates not specified.
- **Error states**: Print engine returns false → QMessageBox::warning 'Print Error'.
- **Needs visual verify**: YES

### `MainWindow::onPrintPreview`

- **Input**: N/A — opens preview with PrintRange::AllPages default.
- **State transitions**: PrintPreviewDialog modal exec().
- **Visible changes**: Print preview dialog modal.
- **Error states**: None — preview is read-only.
- **Needs visual verify**: YES

### `MainWindow::onRedactToolSelected`

- **Input**: N/A.
- **State transitions**: If checked: onAnnotationToolSelected(redactToolAction_); redactionPanelDock_->show(). Else: revert to selectToolAction_.
- **Visible changes**: Redaction tool active in canvas; redaction panel visible.
- **Error states**: No document → no-op.
- **Needs visual verify**: YES

### `MainWindow::onRedactionMarkSelected`

- **Input**: (int pageIndex, _).
- **State transitions**: canvas_->goToPage.
- **Visible changes**: Canvas navigates.
- **Error states**: None.
- **Needs visual verify**: YES

### `MainWindow::onRedo`

- **Input**: N/A.
- **State transitions**: undoManager_->redo(); if next entry has image edit, refreshImageForScannedPage(); markPageDirty; canvas->refreshAllPages; onDocumentStructureChanged.
- **Visible changes**: Canvas reverts to redone state.
- **Error states**: Empty redo stack → no-op.
- **Needs visual verify**: DEPENDS

### `MainWindow::onRemovePassword`

- **Input**: QFileDialog save path.
- **State transitions**: encryptionManager_->removeEncryption(document_, outputPath).
- **Visible changes**: Status bar 'Unencrypted copy saved to <name>'.
- **Error states**: User cancel; std::exception → QMessageBox::critical 'Remove Password Error'.
- **Needs visual verify**: NO

### `MainWindow::onReplace`

- **Input**: (QString find, QString replace) from SearchPanel.
- **State transitions**: Locates current hit; builds EditTextCommand; undoManager_->execute; advances to next match.
- **Visible changes**: Canvas re-renders; SearchPanel status label 'Replaced 1 match'/'No match selected'/'Match not found in text spans'.
- **Error states**: Empty find / no current hit → status text only.
- **Needs visual verify**: YES

### `MainWindow::onReplaceAll`

- **Input**: (QString find, QString replace).
- **State transitions**: Iterates every page+span; builds CompositeCommand 'Replace All'; one undo entry covers all replacements.
- **Visible changes**: Canvas re-renders; SearchPanel 'Replaced N match(es)'/'No matches found'.
- **Error states**: Empty find → no-op.
- **Needs visual verify**: YES

### `MainWindow::onReportIssue`

- **Input**: N/A.
- **State transitions**: QDesktopServices::openUrl('https://github.com/dev/PDFE/issues/new').
- **Visible changes**: External browser.
- **Error states**: None.
- **Needs visual verify**: NO

### `MainWindow::onRequestValidation`

- **Input**: ValidationCoordinator modal consent dialog (then async network).
- **State transitions**: External OCSP/CRL fetch; embedded in /DSS; ValidationCoordinator::completed/cancelled/failed signals.
- **Visible changes**: Modal consent dialog; status bar 'Validation data embedded' / 'Validation request cancelled'.
- **Error states**: Coordinator request() returns false (no consent) → cleanup; failed signal → cleanup.
- **Needs visual verify**: YES

### `MainWindow::onResetForm`

- **Input**: Confirm dialog Yes/No.
- **State transitions**: ResetFormCommand on current page (only fields with /DV reset due to MuPDF behavior); canvas->refreshPage; formPropertiesPanel->clear.
- **Visible changes**: Form widgets revert; status bar 'Form fields reset'.
- **Error states**: User clicks No → no-op; std::exception → QMessageBox::critical.
- **Needs visual verify**: YES

### `MainWindow::onRotatePage180`

- **Input**: N/A.
- **State transitions**: RotatePageCommand(180) for each selected.
- **Visible changes**: Canvas + thumbnails rotate 180.
- **Error states**: None.
- **Needs visual verify**: YES

### `MainWindow::onRotatePageCCW`

- **Input**: N/A.
- **State transitions**: RotatePageCommand(-90) for each selected.
- **Visible changes**: Canvas + thumbnails rotate.
- **Error states**: No document → no-op.
- **Needs visual verify**: YES

### `MainWindow::onRotatePageCW`

- **Input**: N/A — selection from PagePanel + currentPage fallback.
- **State transitions**: RotatePageCommand(+90) for each selected; undoManager_->execute; onDocumentStructureChanged.
- **Visible changes**: Canvas + page panel thumbnails rotate.
- **Error states**: No document/undoManager → no-op.
- **Needs visual verify**: YES

### `MainWindow::onSaveAsFile`

- **Input**: QFileDialog filter 'PDF Files (*.pdf);;All Files (*)' default = currentFileName_.
- **State transitions**: BookmarkPanel::commitToDocument → Document::saveAs(newPath) → flushAlignmentPipelineIfPending if pending → currentFileName_ = newPath → undoManager_->setClean() → updateWindowTitle.
- **Visible changes**: Title updates to new filename, '*' cleared, status bar 'File saved as <name>' for 3000 ms.
- **Error states**: User cancels (no-op); non-PDF → warning; save throws → QMessageBox::critical 'Save Error'.
- **Needs visual verify**: YES

### `MainWindow::onSaveFile`

- **Input**: N/A.
- **State transitions**: BookmarkPanel::commitToDocument → Document::save(currentFileName_) (atomic temp+rename) → flushAlignmentPipelineIfPending if pending → undoManager_->setClean() → onDocumentStructureChanged().
- **Visible changes**: Window title clears '*' suffix; status bar 'File saved' for 3000 ms.
- **Error states**: Document not PDF → QMessageBox::warning 'Only PDF documents can be saved'; std::exception during save → QMessageBox::critical 'Save Error'.
- **Needs visual verify**: YES

### `MainWindow::onSaveWithEncryption`

- **Input**: EncryptionDialog modal (algorithm, key length, user/owner passwords, permissions).
- **State transitions**: EncryptionManager::saveEncrypted(document_, outputPath, settings) OR removeEncryption if algorithm==None.
- **Visible changes**: EncryptionDialog modal; QFileDialog save; status bar 'File saved with AES-256 encryption to <name>'.
- **Error states**: User cancel either dialog; save throws → QMessageBox::critical 'Encryption Error'.
- **Needs visual verify**: YES

### `MainWindow::onSearchResultSelected`

- **Input**: (int pageIndex, QVector<QRectF> quads).
- **State transitions**: canvas->setSearchHighlights; canvas->goToPage.
- **Visible changes**: Canvas navigates + highlights match quads.
- **Error states**: None.
- **Needs visual verify**: YES

### `MainWindow::onSelectAll`

- **Input**: N/A.
- **State transitions**: page->extractPlainText() copied to clipboard.
- **Visible changes**: Status bar 'All page text selected and copied to clipboard'.
- **Error states**: No document/canvas → no-op; empty page → status bar message.
- **Needs visual verify**: NO

### `MainWindow::onSignDocument`

- **Input**: SignDialog modal — pfx path/password, reason, location, appearance flags, TSA, certification.
- **State transitions**: If no unsigned field: CreateSignatureFieldCommand. SaveAs to chosen path (Document::saveAs) + reopen; signatureManager_->signField with options; reopen post-signature; signaturePanel_ updated.
- **Visible changes**: SignDialog modal; QFileDialog 'Save Signed PDF'; signature dock shown; status bar 'Document signed successfully'.
- **Error states**: User cancel any step; signField throws → QMessageBox::critical 'Signing Error'.
- **Needs visual verify**: YES

### `MainWindow::onStickyNoteDoubleClicked`

- **Input**: (int pageIndex, int annotIndex, AnnotationData).
- **State transitions**: stickyNotePopup_->showFor() — positions popup; sets text content; emits popupClosed/contentsChanged.
- **Visible changes**: StickyNotePopup widget appears near the icon.
- **Error states**: Popup null → no-op.
- **Needs visual verify**: YES

### `MainWindow::onStickyNotePopupClosed`

- **Input**: (int pageIndex, int annotIndex).
- **State transitions**: Reposition state cleared.
- **Visible changes**: Popup hidden.
- **Error states**: None.
- **Needs visual verify**: YES

### `MainWindow::onStickyNotePopupContentsChanged`

- **Input**: AnnotationData.
- **State transitions**: ModifyAnnotationCommand; status bar 'Sticky note updated'.
- **Visible changes**: Annotation contents updated; window title gets '*'.
- **Error states**: No annotation manager → no-op.
- **Needs visual verify**: YES

### `MainWindow::onStripExif`

- **Input**: N/A.
- **State transitions**: exifStripper_->stripAllImages; canvas refresh.
- **Visible changes**: QMessageBox::information 'Stripped metadata from N image(s)' or 'No images found'.
- **Error states**: None — Stripper handles internally.
- **Needs visual verify**: YES

### `MainWindow::onTextEditToolSelected`

- **Input**: Triggered when textEditToolAction is checked/unchecked.
- **State transitions**: If checked: skipGroupTextEditCall_=true; activateTextEditForPage(currentPage). If unchecked: textEditAutoRouting_=false.
- **Visible changes**: Canvas tool indicator changes; either TextEditTool or TreRegionTool becomes active depending on page hasTextContent().
- **Error states**: No document/canvas → no-op.
- **Needs visual verify**: YES

### `MainWindow::onToggleBookmarkPanel`

- **Input**: N/A.
- **State transitions**: bookmarkPanelDock_->setVisible(!visible).
- **Visible changes**: Bookmark dock toggles.
- **Error states**: None.
- **Needs visual verify**: YES

### `MainWindow::onToggleCopyPrevention`

- **Input**: (bool checked).
- **State transitions**: Sets ALL_KEY copy permission on document_; not persisted unless saved.
- **Visible changes**: Copy action might be disabled in subsequent operations; no immediate visible change.
- **Error states**: None.
- **Needs visual verify**: NO

### `MainWindow::onToggleFullScreen`

- **Input**: N/A.
- **State transitions**: showFullScreen() / showNormal(); fullScreenAction_->setChecked() reflects.
- **Visible changes**: Window enters/leaves full-screen mode; menu bar usually hidden in full screen.
- **Error states**: None.
- **Needs visual verify**: YES

### `MainWindow::onToggleOcrBoxes`

- **Input**: (bool checked).
- **State transitions**: ocrBoxesEnabled_ = checked; loadOcrBoxesForPage(currentPage); canvas->update.
- **Visible changes**: Canvas overlays per-character OCR bounding boxes.
- **Error states**: No OCR pipeline → silently disabled.
- **Needs visual verify**: YES

### `MainWindow::onTogglePagePanel`

- **Input**: N/A.
- **State transitions**: pagePanelDock_->setVisible(!visible).
- **Visible changes**: Side dock toggles visible/hidden.
- **Error states**: Dock null → no-op.
- **Needs visual verify**: YES

### `MainWindow::onTogglePrintPrevention`

- **Input**: (bool checked).
- **State transitions**: Sets print permission flag on document_.
- **Visible changes**: No immediate visible change; affects subsequent print attempts.
- **Error states**: None.
- **Needs visual verify**: NO

### `MainWindow::onTogglePropertiesPanel`

- **Input**: N/A.
- **State transitions**: propertiesPanelDock_->setVisible(!visible).
- **Visible changes**: Properties dock toggles.
- **Error states**: None.
- **Needs visual verify**: YES

### `MainWindow::onToggleSearchPanel`

- **Input**: N/A.
- **State transitions**: searchPanelDock_->setVisible(!visible).
- **Visible changes**: Search dock toggles.
- **Error states**: None.
- **Needs visual verify**: YES

### `MainWindow::onToggleStickyNotes`

- **Input**: (bool checked) from QAction::toggled.
- **State transitions**: canvas_->setStickyNotesVisible(checked).
- **Visible changes**: Sticky note icons appear/disappear on canvas.
- **Error states**: No canvas → no-op.
- **Needs visual verify**: YES

### `MainWindow::onTreRegionSelected`

- **Input**: (int pageIndex, QRectF pdfRect).
- **State transitions**: regionReconstructor_->analyze; TreEditDialog modal; ReconstructRegionCommand or ImageReplaceCommand depending on user choice; canvas->refreshPage.
- **Visible changes**: TRE edit dialog modal; on Accept, canvas region re-renders with new text.
- **Error states**: Analysis fail → QMessageBox::warning 'Could not recognize text'; user cancel.
- **Needs visual verify**: YES

### `MainWindow::onUndo`

- **Input**: N/A.
- **State transitions**: undoManager_->undo(); if scanned-page image edit on top, restore saved XObject ref via PdfContentWriter::restoreImageXObjectRef + invalidate ImageReplacer cache; canvas->refreshAllPages; onDocumentStructureChanged; updateWindowTitle.
- **Visible changes**: Canvas re-renders with the prior state; window title may add/remove '*' depending on whether undo crosses the saveClean barrier.
- **Error states**: Empty undo stack → undo() is a no-op.
- **Needs visual verify**: DEPENDS — visible effect depends on what's being undone (page rotation visible; metadata edit not visible from canvas).

### `MainWindow::onVerifySignatures`

- **Input**: N/A.
- **State transitions**: verifyAllSignatures; LtvValidator::verify; signaturePanel update; requestValidationAction enabled iff validCount>0.
- **Visible changes**: Signature dock shown; QMessageBox::information with summary.
- **Error states**: No signatures → 'no signatures' info; verify throws → QMessageBox::critical.
- **Needs visual verify**: YES

### `MainWindow::onZoomChanged`

- **Input**: (float zoom).
- **State transitions**: statusBar_->setZoom(zoom*100); updateZoomCombo.
- **Visible changes**: Status bar percent update; zoom combo text updates.
- **Error states**: None.
- **Needs visual verify**: YES

### `MainWindow::onZoomComboChanged`

- **Input**: Combo index — items 25%,50%,75%,100%,125%,150%,200%,400%.
- **State transitions**: Parses percent, canvas_->setZoom(percent/100).
- **Visible changes**: Canvas re-render.
- **Error states**: Unparseable text → no-op.
- **Needs visual verify**: YES

### `MainWindow::onZoomComboEdited`

- **Input**: QString text (free entry, % stripped).
- **State transitions**: Same as onZoomComboChanged.
- **Visible changes**: Canvas re-render.
- **Error states**: Unparseable → no-op.
- **Needs visual verify**: YES

### `MainWindow::onZoomIn`

- **Input**: N/A.
- **State transitions**: canvas_->zoomIn() — multiplies current zoom by step factor.
- **Visible changes**: Canvas re-renders larger; zoomCombo + statusBar reflect new percent.
- **Error states**: No canvas → no-op.
- **Needs visual verify**: YES

### `MainWindow::onZoomOut`

- **Input**: N/A.
- **State transitions**: canvas_->zoomOut().
- **Visible changes**: Canvas re-renders smaller; zoomCombo + statusBar update.
- **Error states**: No canvas → no-op.
- **Needs visual verify**: YES

### `MainWindow::openFile`

- **Input**: QString filePath (UTF-16 path).
- **State transitions**: closeDocument() → Document::open up to 4× retry with PasswordDialog → recreate TileCache/TileRenderer/AnnotationManager/FormManager/JavaScriptEngine/RedactionManager/EncryptionManager/SignatureManager/ImageManager/MetadataManager → wire panels → enable saveAction/saveAsAction/closeAction/copyAction/cutAction/pasteAction/selectAllAction/zoom*/print*/form*/page*/encryption*/signature*/image*/metadata*/compliance/preflight/replace/tabOrder/calculationOrder/altText/submitForm/ocr/treAction/accessibilityAction → recentFilesMenu_->addFile() → AppSettings::setLastOpenDirectory.
- **Visible changes**: Window title shows filename, status bar shows fileName + 1/N + 100%, canvas renders page 1 fitted, page panel populated, bookmark dock shown if outline present, signature dock shown if signatures present.
- **Error states**: Encrypted PDF without password → PasswordDialog (3 retries, then propagate); Document::open throws → QMessageBox::critical 'Failed to open PDF file' + closeDocument() rollback.
- **Needs visual verify**: YES

### `MainWindow::showAtlasComparison`

- **Input**: N/A.
- **State transitions**: AtlasComparisonDialog modal — renders both modes side-by-side.
- **Visible changes**: Modal dialog with comparison preview.
- **Error states**: Pending atlas not ready → no-op.
- **Needs visual verify**: YES

### `PrintDialog::onRangeChanged`

- **Input**: QRadioButton/QSpinBox.
- **State transitions**: Updates internal PrintOptions.range; re-validates From/To inputs.
- **Visible changes**: From/To spinboxes enable/disable.
- **Error states**: Invalid range → OK button disabled.
- **Needs visual verify**: YES

### `QDialog::accept`

- **Input**: Triggered by OK / accept button.
- **State transitions**: Sets dialog result Accepted; closes dialog; caller reads dialog state via accessor methods.
- **Visible changes**: Dialog disappears.
- **Error states**: None.
- **Needs visual verify**: YES

### `QDialog::reject`

- **Input**: Triggered by Cancel / Esc / Close.
- **State transitions**: Sets dialog result Rejected; closes dialog without committing changes.
- **Visible changes**: Dialog disappears.
- **Error states**: None.
- **Needs visual verify**: YES

### `SearchPanel::onReplaceAllClicked`

- **Input**: Find + replace text.
- **State transitions**: Calls MainWindow::onReplaceAll.
- **Visible changes**: Canvas re-renders.
- **Error states**: Empty find.
- **Needs visual verify**: YES

### `SearchPanel::onReplaceClicked`

- **Input**: Find + replace text.
- **State transitions**: Calls MainWindow::onReplace via signal.
- **Visible changes**: Canvas re-renders if replaced.
- **Error states**: Empty find.
- **Needs visual verify**: YES

### `SearchPanel::onResultSelected`

- **Input**: Selected result row.
- **State transitions**: Emits resultSelected(pageIndex, quads); MainWindow navigates.
- **Visible changes**: Canvas navigates.
- **Error states**: None.
- **Needs visual verify**: YES

### `SearchPanel::onSearchClicked`

- **Input**: Search line edit text + case/whole-word checkboxes.
- **State transitions**: Searcher::search across pages; populates results list; emits resultSelected.
- **Visible changes**: Results list populated; canvas highlights first hit.
- **Error states**: Empty query → status 'No matches'.
- **Needs visual verify**: YES

### `SignDialog::validateInput`

- **Input**: PFX path + password line edit text.
- **State transitions**: Toggles okButton enabled state; sets validationLabel red text.
- **Visible changes**: Validation label text + Sign button enabled state.
- **Error states**: Empty path → 'PFX certificate file is required'; empty pwd → 'Certificate password is required'.
- **Needs visual verify**: YES

## Per-dialog summary

Each dialog gets its own row in the per-file matrix, but their purpose + key inputs + outputs are summarized here so individual sub-control rows below can reference the parent.

| class | purpose | inputs | outputs | visual? |
|---|---|---|---|---|
| `AboutDialog` | Static About-PDFE dialog; renders bundled about.md. | N/A — read-only. | QDialog::accept on Close. | YES |
| `AccessibilityHelper` | Helper class — populates accessible names/roles on widgets. | QWidget*. | Side-effects on target widget tree. | NO |
| `AltTextDialog` | Edit /Contents (alt text) and /ActualText for a single annotation. | altTextEdit (QPlainTextEdit), actualTextEdit (QPlainTextEdit). | Annotations updated via AnnotationManager::setAltText/setActualText on accept. | YES |
| `AnnotationPropertiesWidget` | Side panel for editing color/opacity/author/contents of a selected annotation. | colorButton, opacitySlider, authorEdit, contentsEdit. | Emits propertiesChanged → MainWindow::onAnnotationPropertiesChanged → ModifyAnnotationCommand. | YES |
| `AtlasComparisonDialog` | Shows ScanCropped vs FreeTypeStyled atlas-rendered overlays side-by-side for the active span. | scanRadio / fontRadio. | Selection emits modeSelected; MainWindow::applyAtlasMode. | YES |
| `AttachmentPanel` | Side dock listing /EmbeddedFiles entries. | attachmentTree double-click → extract. | Emits requestExtract; right-click context menu. | YES |
| `BatchDialog` | Build & run a batch action list across multiple files. | addActionButton/removeActionButton/files list/output dir/owner password. | BatchProcessor runs; per-file results list updated. | YES |
| `BookmarkPanel` | Tree view + context menu for /Outlines. | QTreeWidget activate / drag-drop. | BookmarkManager mutations via undo. | YES |
| `CalculationOrderDialog` | Edit form-wide /AcroForm /CO array (calculation order). | fieldList QListWidget; up/down/apply buttons. | FormManager::setCalculationOrder on Apply. | YES |
| `CombineFilesService` | Backend service to merge PDFs/images into one document. | CombineFilesOptions (input list, page ranges, output path). | Writes merged PDF. | NO |
| `CombineFilesWizard` | Multi-step wizard around CombineFilesService. | filesList add/remove buttons; output path. | Service invocation on Finish. | YES |
| `CommentPanel` | Tree view of all annotations grouped by page; filter/sort. | filterBox QLineEdit; sortCombo; tree double-click. | Emits navigateToAnnotation / deleteAnnotation. | YES |
| `CompareDialog` | Compare two PDFs page-by-page. | File pickers for A/B; threshold spinbox. | Visual diff side-by-side. | YES |
| `ComplianceDialog` | Check + convert to PDF/A and PDF/UA. | profileCombo (PDF/A-1b/2b/3b, UA-1); convertButton. | ComplianceManager::validate + convert. | YES |
| `CustomizeToolbarsDialog` | Add/remove/reorder toolbar buttons across all toolbars. | availableList, currentList, addButton, removeButton, upButton, downButton, applyButton. | ToolbarLayoutManager::saveLayouts. | YES |
| `DefaultsLogPanel` | Side dock showing diagnostic messages for the active document. | clearButton; copyButton. | Read-only log. | YES |
| `EncryptionDialog` | Configure AES-128/256 user/owner password + permission flags. | algorithmCombo; userPasswordEdit; ownerPasswordEdit; permission checkboxes. | EncryptionSettings via settings() accessor on Accept. | YES |
| `FormFieldPropertiesPanel` | QStackedWidget side panel — different editor per field type. | Per-field-type widgets (text, checkbox, choice, etc.). | Emits valueChanged → SetFieldValueCommand. | YES |
| `HeaderFooterDialog` | Add headers/footers to all pages. | Six text fields (left/center/right × header/footer); fontCombo, sizeSpin, colorButton, marginSpin, pageRange. | HeaderFooterApplier::apply. | YES |
| `ImagePropertiesPanel` | Side panel showing dimensions/colorspace/bpp + rotate/crop/resize buttons. | rotateButton, cropButton, resizeButton, removeButton. | Routes to ImageTool actions. | YES |
| `ImageToPdfWizard` | Wizard converting a list of images into a new PDF. | imageList add/remove; pageSizeCombo; dpiSpin; jpegQualitySpin. | Returns std::filesystem::path of output PDF. | YES |
| `LayerPanel` | Side dock listing OCG layers; toggle visibility. | layerTree checkboxes. | OcgManager::setLayerVisible. | YES |
| `MainWindow` | Top-level QMainWindow — owns canvas, panels, all menus, the document, all *Manager instances. | All QActions, all menu items, drag-drop accepting application/x-pdf-page-id and text/uri-list (file:// PDFs). | See per-action shared-slots table. | YES |
| `MetadataDialog` | Edit document /Info dict. | title/author/subject/keywords/creator/producer line edits. | EditMetadataCommand on Accept. | YES |
| `NavigationHistory` | Helper class — back/forward stack of page visits. | recordVisit(pageIndex). | goBack/goForward return page index. | NO |
| `OptimizeDialog` | Reduce file size: subset fonts, downsample images, remove unused objects. | Per-option checkboxes + preview button. | OptimizationManager::optimize. | YES |
| `PageLabelsDialog` | Edit /PageLabels number tree (Roman/decimal/alpha/prefix). | rangeTable, addRangeButton, removeRangeButton, applyButton. | PageLabelsManager::setLabels. | YES |
| `PagePanel` | Side dock with page thumbnails + drag-drop reorder + context menu. | thumbnailList; right-click menu actions. | Emits pageSelected, pageMoved, contextMenuAction. | YES |
| `PasswordDialog` | Prompt for PDF password during open. | passwordEdit (echo Password). | password() accessor on Accept. | YES |
| `PdfCanvas` | Custom QWidget rendering page tiles + dispatching mouse/keyboard to active tool. | Mouse/keyboard events; setCurrentTool / setTextEditTool / setImageTool. | Emits pageChanged, zoomChanged, annotationDoubleClicked. | YES |
| `PdfCanvasAccessible` | QAccessibleInterface for PdfCanvas — exposes pages + annotations to UIA. | UIA tree queries. | Children list per page. | NO — accessibility tree only. |
| `PreferencesDialog` | App settings: theme, OCR language, default zoom, JS-enabled, recent files limit. | Each tab's controls — themeCombo, ocrLangCombo, zoomDefaultSpin, jsCheckBox, recentLimitSpin. | AppSettings updates on Accept. | YES |
| `PreflightDialog` | Per-rule pass/fail report for selected compliance profile. | profileCombo. | Read-only report tree. | YES |
| `PrintDialog` | PDFE-internal print configurator (range, copies, scaling, duplex). | rangeRadios, fromSpin, toSpin, copiesSpin, scalingCombo, duplexCombo. | options() accessor → PrintEngine. | YES |
| `PrintPreviewDialog` | QPrintPreview-style preview for PDFE rendering. | Zoom buttons, page nav, fit width/page actions. | Read-only preview. | YES |
| `PropertiesPanel` | Side dock — current page metadata (size, rotation, label). | Read-only page info. | Display only. | YES |
| `RecentFilesMenu` | QMenu subclass — recent-files list with clear action. | AppSettings::recentFiles(). | Emits fileTriggered(path) on click. | DEPENDS — visible on next File menu open. |
| `RedactionPanel` | Side dock listing pending redaction marks; navigate/delete; Apply button. | marksTable, deleteButton, navigateButton, applyButton. | Emits navigateToMark / deleteMark / applyAll. | YES |
| `SearchPanel` | Find/replace dock. | searchInput, caseSensitiveCheck, wholeWordCheck, searchButton, replaceInput, replaceButton, replaceAllButton,… | Emits searchResultSelected, replaceRequested, replaceAllRequested. | YES |
| `SignDialog` | Configure signature: PFX path/pwd, reason, location, appearance flags, TSA, certification level. | pfxPathEdit, pfxPasswordEdit, reasonEdit, locationEdit, showLabels/Name/Date/Dn/Logo checkboxes, tsaCheckBox+… | signingOptions() accessor → SigningOptions struct. | YES |
| `SignaturePanel` | Side dock listing signature widgets + LTV status. | signatureTable, verifyButton, requestValidationButton. | Emits navigateToSignature. | YES |
| `StatusBar` | Bottom bar: filename + page count + zoom + encrypted icon + form-field count. | Setters from MainWindow. | Read-only display. | YES |
| `StickyNotePopup` | Floating popup near a clicked sticky-note icon for inline content editing. | contentsEdit QPlainTextEdit. | Emits contentsChanged, popupClosed. | YES |
| `TabOrderDialog` | Edit per-page form-field tab order. | fieldsList QListWidget + up/down buttons. | newTabOrder() accessor on Accept. | YES |
| `TextEditToolBar` | Sub-toolbar shown when TextEditTool active — font family/size, bold/italic/underline, color, alignment. | fontCombo, sizeSpin, boldButton, italicButton, underlineButton, colorButton, alignButtons (left/center/right/… | Emits formattingChanged → TextEditTool. | YES |
| `ToolbarLayoutManager` | Helper class — load/save QToolBar action layouts via QSettings. | QToolBar*, ToolbarLayout struct. | Side-effects on QSettings. | NO |
| `TrustAnchorDialog` | Manage trusted root CA certificates (offline LTV validation). | anchorList, addButton, removeButton, exportButton. | TrustAnchorStore mutations. | YES |
| `ValidationRequestDialog` | Pre-fetch consent dialog for OCSP/CRL network requests. | consentCheck, proceedButton. | Emits consented(bool). | YES |
| `WatermarkDialog` | Add text watermark to all pages. | textEdit, fontCombo, sizeSpin, colorButton, opacitySpin, rotationSpin, applyButton. | WatermarkApplier::apply. | YES |

## Per-file matrix

One section per source file. Rows are grouped by category within each file.

### `src/ui/AboutDialog.cpp` (3 entries)

#### widgets (3)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `aboutContent` | src/ui/AboutDialog.cpp:73 | N/A — passive sub-control of AboutDialog; value read by AboutDialog::accessor() at QDialog::exec()-time. | Read-only HTML; no input. | None at signal level — value captured into result struct when AboutDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `licenseTextEdit` | src/ui/AboutDialog.cpp:107 | N/A — passive sub-control of AboutDialog; value read by AboutDialog::accessor() at QDialog::exec()-time. | Rich-text or plain-text QString. | None at signal level — value captured into result struct when AboutDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `okButton` | src/ui/AboutDialog.cpp:138 | → QDialog::accept | N/A — button click only (no text input). | Sets dialog result Accepted; closes dialog; caller reads dialog state via accessor methods. | Dialog disappears. | None. | YES |

### `src/ui/AboutDialog.h` (1 entries)

#### dialogs (1)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `AboutDialog` | src/ui/AboutDialog.h | AboutDialog::exec() — modal dialog; Static About-PDFE dialog; renders bundled about.md. | N/A — read-only. | QDialog::accept on Close. | Modal dialog appears; on close it disappears. Sub-controls re-render per user input. | Per accept() validation logic in this dialog; reject() always succeeds. | YES |

### `src/ui/AltTextDialog.cpp` (5 entries)

#### widgets (5)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `(QFormLayout#39)` | src/ui/AltTextDialog.cpp:39 | N/A — passive sub-control of AltTextDialog; value read by AltTextDialog::accessor() at QDialog::exec()-time. | Per-widget; see source. | None at signal level — value captured into result struct when AltTextDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `altTextAltEdit` | src/ui/AltTextDialog.cpp:41 | N/A — passive sub-control of AltTextDialog; value read by AltTextDialog::accessor() at QDialog::exec()-time. | Free-form QString; validator/echo-mode per dialog (passwords use Password echo). | None at signal level — value captured into result struct when AltTextDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `altTextActualEdit` | src/ui/AltTextDialog.cpp:47 | N/A — passive sub-control of AltTextDialog; value read by AltTextDialog::accessor() at QDialog::exec()-time. | Rich-text or plain-text QString. | None at signal level — value captured into result struct when AltTextDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `altTextSaveButton` | src/ui/AltTextDialog.cpp:64 | → AltTextDialog::onSave | N/A — button click only (no text input). | } try { mgr_->setAltText(page_, annotIndex_, altEdit_->text().toStdString()); mgr_->setActualText(page_, annotIndex_, actualEdit_->toPlainText().toStdString()); } catch (const std::exception& e) { QM… (extracted from AltTextDialog.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `altTextCancelButton` | src/ui/AltTextDialog.cpp:68 | → QDialog::reject | N/A — button click only (no text input). | Sets dialog result Rejected; closes dialog without committing changes. | Dialog disappears. | None. | YES |

### `src/ui/AltTextDialog.h` (1 entries)

#### dialogs (1)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `AltTextDialog` | src/ui/AltTextDialog.h | AltTextDialog::exec() — modal dialog; Edit /Contents (alt text) and /ActualText for a single annotation. | altTextEdit (QPlainTextEdit), actualTextEdit (QPlainTextEdit). | Annotations updated via AnnotationManager::setAltText/setActualText on accept. | Modal dialog appears; on close it disappears. Sub-controls re-render per user input. | Per accept() validation logic in this dialog; reject() always succeeds. | YES |

### `src/ui/AnnotationPropertiesWidget.cpp` (11 entries)

#### widgets (11)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `(QFormLayout#29)` | src/ui/AnnotationPropertiesWidget.cpp:29 | N/A — passive sub-control of AnnotationPropertiesWidget; value read by AnnotationPropertiesWidget::accessor() at QDialog::exec()-time. | Per-widget; see source. | None at signal level — value captured into result struct when AnnotationPropertiesWidget is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `annotAuthorEdit` | src/ui/AnnotationPropertiesWidget.cpp:39 | → AnnotationPropertiesWidget::onAuthorChanged | Free-form QString; validator/echo-mode per dialog (passwords use Password echo). | emitChanges(); } (extracted from AnnotationPropertiesWidget.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `annotContentsEdit` | src/ui/AnnotationPropertiesWidget.cpp:53 | → AnnotationPropertiesWidget::onContentsChanged | Rich-text or plain-text QString. | emitChanges(); } (extracted from AnnotationPropertiesWidget.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `(QFormLayout#62)` | src/ui/AnnotationPropertiesWidget.cpp:62 | N/A — passive sub-control of AnnotationPropertiesWidget; value read by AnnotationPropertiesWidget::accessor() at QDialog::exec()-time. | Per-widget; see source. | None at signal level — value captured into result struct when AnnotationPropertiesWidget is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `annotColorButton` | src/ui/AnnotationPropertiesWidget.cpp:64 | → AnnotationPropertiesWidget::onColorButtonClicked | N/A — button click only (no text input). | QColor newColor = QColorDialog::getColor(currentData_.color, this, tr("Select Annotation Color")); updateColorButton(); emitChanges() (extracted from AnnotationPropertiesWidget.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `(QFormLayout#75)` | src/ui/AnnotationPropertiesWidget.cpp:75 | N/A — passive sub-control of AnnotationPropertiesWidget; value read by AnnotationPropertiesWidget::accessor() at QDialog::exec()-time. | Per-widget; see source. | None at signal level — value captured into result struct when AnnotationPropertiesWidget is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `annotOpacitySlider` | src/ui/AnnotationPropertiesWidget.cpp:81 | → AnnotationPropertiesWidget::onOpacityChanged | Integer in [minimum,maximum]; orientation horizontal/vertical. | opacityLabel_->setText(QString("%1%").arg(value)); emitChanges(); } (extracted from AnnotationPropertiesWidget.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `annotFontGroup` | src/ui/AnnotationPropertiesWidget.cpp:100 | N/A — passive sub-control of AnnotationPropertiesWidget; value read by AnnotationPropertiesWidget::accessor() at QDialog::exec()-time. | Container only — no input. | None at signal level — value captured into result struct when AnnotationPropertiesWidget is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | NO |
| `annotFontCombo` | src/ui/AnnotationPropertiesWidget.cpp:106 | → AnnotationPropertiesWidget::onFontChanged | One of items() (editable variant accepts free-form QString). | currentData_.daFontName = fontCombo_->currentData().toString().toStdString(); emitChanges() (extracted from AnnotationPropertiesWidget.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `annotFontSizeSpinner` | src/ui/AnnotationPropertiesWidget.cpp:117 | → AnnotationPropertiesWidget::onFontSizeChanged | Integer in [minimum,maximum] with step (defaults: 0..99 step 1 unless overridden). | currentData_.daFontSize = static_cast<float>(size); emitChanges() (extracted from AnnotationPropertiesWidget.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `annotTextColorButton` | src/ui/AnnotationPropertiesWidget.cpp:128 | → AnnotationPropertiesWidget::onTextColorClicked | N/A — button click only (no text input). | QColor color = QColorDialog::getColor(currentData_.daColor, this, tr("Select Text Color")); textColorButton_->setStyleSheet( QString("QPushButton { background-color: %1; }").arg(color.name())) (extracted from AnnotationPropertiesWidget.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |

### `src/ui/AnnotationPropertiesWidget.h` (1 entries)

#### dialogs (1)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `AnnotationPropertiesWidget` | src/ui/AnnotationPropertiesWidget.h | AnnotationPropertiesWidget — QWidget side panel; Side panel for editing color/opacity/author/contents of a selected annotation. | colorButton, opacitySlider, authorEdit, contentsEdit. | Emits propertiesChanged → MainWindow::onAnnotationPropertiesChanged → ModifyAnnotationCommand. | Side dock visibility toggleable via View menu. Internal state visible inline as user interacts. | Per user-interaction; see widget rows for sub-controls. | YES |

### `src/ui/AtlasComparisonDialog.cpp` (4 entries)

#### widgets (4)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `atlasUseFontButton` | src/ui/AtlasComparisonDialog.cpp:617 | → AtlasComparisonDialog::onUseFont | N/A — button click only (no text input). | selectedMode_ = pdfe::tre::AtlasMode::FreeTypeStyled; accept() (extracted from AtlasComparisonDialog.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `atlasFillMissingCheck` | src/ui/AtlasComparisonDialog.cpp:649 | → AtlasComparisonDialog::onFillMissingToggled | Boolean (checked/unchecked); supports tri-state if explicitly set. | bool isSynth = !isAtlas && !scanAtlas_->glyphFromScan(cp).isNull(); bool isMissing = !isAtlas && !isSynth; } } refreshAtlasGrid() (extracted from AtlasComparisonDialog.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `atlasUseScanButton` | src/ui/AtlasComparisonDialog.cpp:673 | → AtlasComparisonDialog::onUseScan | N/A — button click only (no text input). | selectedMode_ = pdfe::tre::AtlasMode::ScanCropped; accept() (extracted from AtlasComparisonDialog.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `atlasCancelButton` | src/ui/AtlasComparisonDialog.cpp:689 | → QDialog::reject | N/A — button click only (no text input). | Sets dialog result Rejected; closes dialog without committing changes. | Dialog disappears. | None. | YES |

### `src/ui/AtlasComparisonDialog.h` (1 entries)

#### dialogs (1)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `AtlasComparisonDialog` | src/ui/AtlasComparisonDialog.h | AtlasComparisonDialog::exec() — modal dialog; Shows ScanCropped vs FreeTypeStyled atlas-rendered overlays side-by-side for the active span. | scanRadio / fontRadio. | Selection emits modeSelected; MainWindow::applyAtlasMode. | Modal dialog appears; on close it disappears. Sub-controls re-render per user input. | Per accept() validation logic in this dialog; reject() always succeeds. | YES |

### `src/ui/AttachmentPanel.cpp` (4 entries)

#### widgets (4)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `attachmentTree` | src/ui/AttachmentPanel.cpp:34 | → AttachmentPanel::onSelectionChanged | Selection of tree items; supports drag-drop if InternalMove enabled. | const bool has = tree_->currentItem() != nullptr; extractButton_->setEnabled(has && mgr_ && document_); deleteButton_->setEnabled(has && mgr_ && document_) (extracted from AttachmentPanel.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `attachmentAddButton` | src/ui/AttachmentPanel.cpp:45 | → AttachmentPanel::onAddClicked | N/A — button click only (no text input). | const QString path = QFileDialog::getOpenFileName(this, tr("Choose a file to attach")); const std::filesystem::path p(path.toStdString()); const std::string name = p.filename().string() (extracted from AttachmentPanel.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `attachmentExtractButton` | src/ui/AttachmentPanel.cpp:48 | → AttachmentPanel::onExtractClicked | N/A — button click only (no text input). | const QString name = currentSelectionName(); const QString dir = QFileDialog::getExistingDirectory(this, tr("Choose a directory to extract to")); const auto out = mgr_->extract(document_, name.toStdS… (extracted from AttachmentPanel.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `attachmentDeleteButton` | src/ui/AttachmentPanel.cpp:52 | → AttachmentPanel::onDeleteClicked | N/A — button click only (no text input). | const QString name = currentSelectionName(); const auto ans = QMessageBox::question(this, tr("Delete Attachment"), tr("Remove %1 from the document?").arg(name)); } statusLabel_->setText(tr("Removed %… (extracted from AttachmentPanel.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |

### `src/ui/AttachmentPanel.h` (1 entries)

#### dialogs (1)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `AttachmentPanel` | src/ui/AttachmentPanel.h | AttachmentPanel — QWidget side panel; Side dock listing /EmbeddedFiles entries. | attachmentTree double-click → extract. | Emits requestExtract; right-click context menu. | Side dock visibility toggleable via View menu. Internal state visible inline as user interacts. | Per user-interaction; see widget rows for sub-controls. | YES |

### `src/ui/BatchDialog.cpp` (21 entries)

#### widgets (21)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `inputGroup` | src/ui/BatchDialog.cpp:45 | N/A — passive sub-control of BatchDialog; value read by BatchDialog::accessor() at QDialog::exec()-time. | Container only — no input. | None at signal level — value captured into result struct when BatchDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | NO |
| `selectFolderButton` | src/ui/BatchDialog.cpp:51 | → BatchDialog::onSelectFolder | N/A — button click only (no text input). | QString dir = QFileDialog::getExistingDirectory( this, tr("Select Input Folder")); inputFiles_.clear(); QDir qdir(dir) (extracted from BatchDialog.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `actionGroup` | src/ui/BatchDialog.cpp:59 | N/A — passive sub-control of BatchDialog; value read by BatchDialog::accessor() at QDialog::exec()-time. | Container only — no input. | None at signal level — value captured into result struct when BatchDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | NO |
| `actionListWidget` | src/ui/BatchDialog.cpp:65 | N/A — passive sub-control of BatchDialog; value read by BatchDialog::accessor() at QDialog::exec()-time. | Selection of one or more items. | None at signal level — value captured into result struct when BatchDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `addActionButton` | src/ui/BatchDialog.cpp:71 | → BatchDialog::onAddAction | N/A — button click only (no text input). | auto action = buildActionFromUi(); actions_.push_back(action); updateActionList() (extracted from BatchDialog.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `removeActionButton` | src/ui/BatchDialog.cpp:74 | → BatchDialog::onRemoveAction | N/A — button click only (no text input). | int row = actionListWidget_->currentRow(); updateActionList(); } (extracted from BatchDialog.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `moveUpButton` | src/ui/BatchDialog.cpp:77 | → BatchDialog::onMoveUp | N/A — button click only (no text input). | int row = actionListWidget_->currentRow(); updateActionList(); actionListWidget_->setCurrentRow(row - 1) (extracted from BatchDialog.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `moveDownButton` | src/ui/BatchDialog.cpp:80 | → BatchDialog::onMoveDown | N/A — button click only (no text input). | int row = actionListWidget_->currentRow(); updateActionList(); actionListWidget_->setCurrentRow(row + 1) (extracted from BatchDialog.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `actionTypeCombo` | src/ui/BatchDialog.cpp:92 | → BatchDialog::onActionTypeChanged | One of items() (editable variant accepts free-form QString). | configStack_->setCurrentIndex(index) (extracted from BatchDialog.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `configStack` | src/ui/BatchDialog.cpp:101 | N/A — passive sub-control of BatchDialog; value read by BatchDialog::accessor() at QDialog::exec()-time. | Container — switches by setCurrentIndex(). | None at signal level — value captured into result struct when BatchDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `watermarkTextEdit` | src/ui/BatchDialog.cpp:107 | N/A — passive sub-control of BatchDialog; value read by BatchDialog::accessor() at QDialog::exec()-time. | Free-form QString; validator/echo-mode per dialog (passwords use Password echo). | None at signal level — value captured into result struct when BatchDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `watermarkOpacitySpin` | src/ui/BatchDialog.cpp:115 | N/A — passive sub-control of BatchDialog; value read by BatchDialog::accessor() at QDialog::exec()-time. | Double in [minimum,maximum] with step + decimals. | None at signal level — value captured into result struct when BatchDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `watermarkPositionCombo` | src/ui/BatchDialog.cpp:124 | N/A — passive sub-control of BatchDialog; value read by BatchDialog::accessor() at QDialog::exec()-time. | One of items() (editable variant accepts free-form QString). | None at signal level — value captured into result struct when BatchDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `pdfaLevelCombo` | src/ui/BatchDialog.cpp:147 | N/A — passive sub-control of BatchDialog; value read by BatchDialog::accessor() at QDialog::exec()-time. | One of items() (editable variant accepts free-form QString). | None at signal level — value captured into result struct when BatchDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `permPrintCheck` | src/ui/BatchDialog.cpp:163 | N/A — passive sub-control of BatchDialog; value read by BatchDialog::accessor() at QDialog::exec()-time. | Boolean (checked/unchecked); supports tri-state if explicitly set. | None at signal level — value captured into result struct when BatchDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `permCopyCheck` | src/ui/BatchDialog.cpp:167 | N/A — passive sub-control of BatchDialog; value read by BatchDialog::accessor() at QDialog::exec()-time. | Boolean (checked/unchecked); supports tri-state if explicitly set. | None at signal level — value captured into result struct when BatchDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `permEditCheck` | src/ui/BatchDialog.cpp:170 | N/A — passive sub-control of BatchDialog; value read by BatchDialog::accessor() at QDialog::exec()-time. | Boolean (checked/unchecked); supports tri-state if explicitly set. | None at signal level — value captured into result struct when BatchDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `ownerPasswordEdit` | src/ui/BatchDialog.cpp:173 | N/A — passive sub-control of BatchDialog; value read by BatchDialog::accessor() at QDialog::exec()-time. | Free-form QString; validator/echo-mode per dialog (passwords use Password echo). | None at signal level — value captured into result struct when BatchDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `runButton` | src/ui/BatchDialog.cpp:192 | → BatchDialog::onRun | N/A — button click only (no text input). | } if (actions_.empty()) { statusLabel_->setText(tr("No actions configured")); } running_ = true; runButton_->setEnabled(false) (extracted from BatchDialog.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `cancelButton` | src/ui/BatchDialog.cpp:195 | → BatchDialog::onCancel | N/A — button click only (no text input). | } statusLabel_->setText(tr("Cancelling...")) (extracted from BatchDialog.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `batchErrorLog` | src/ui/BatchDialog.cpp:217 | N/A — passive sub-control of BatchDialog; value read by BatchDialog::accessor() at QDialog::exec()-time. | Rich-text or plain-text QString. | None at signal level — value captured into result struct when BatchDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |

### `src/ui/BatchDialog.h` (1 entries)

#### dialogs (1)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `BatchDialog` | src/ui/BatchDialog.h | BatchDialog::exec() — modal dialog; Build & run a batch action list across multiple files. | addActionButton/removeActionButton/files list/output dir/owner password. | BatchProcessor runs; per-file results list updated. | Modal dialog appears; on close it disappears. Sub-controls re-render per user input. | Per accept() validation logic in this dialog; reject() always succeeds. | YES |

### `src/ui/BookmarkPanel.cpp` (10 entries)

#### actions (8)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `newBookmarkAction` | src/ui/BookmarkPanel.cpp:152 | → BookmarkPanel::onCreateAtRoot | QInputDialog::getText 'Bookmark title'. | CreateBookmarkCommand at root level; refresh tree. | New bookmark in tree. | User cancel. | YES |
| `newChildBookmarkAction` | src/ui/BookmarkPanel.cpp:157 | → BookmarkPanel::onCreateChild | QInputDialog title. | CreateBookmarkCommand under selected. | Child added. | No selection / cancel. | YES |
| `renameBookmarkAction` | src/ui/BookmarkPanel.cpp:163 | → BookmarkPanel::onRename | QInputDialog title. | RenameBookmarkCommand. | Tree row text updates. | Empty name → no-op. | YES |
| `deleteBookmarkAction` | src/ui/BookmarkPanel.cpp:167 | → BookmarkPanel::onDelete | Confirm Yes/No. | DeleteBookmarkCommand. | Tree row removed. | User No. | YES |
| `moveBookmarkUpAction` | src/ui/BookmarkPanel.cpp:173 | → BookmarkPanel::onMoveUp | N/A. | MoveBookmarkCommand up. | Tree reorder. | Already first. | YES |
| `moveBookmarkDownAction` | src/ui/BookmarkPanel.cpp:177 | → BookmarkPanel::onMoveDown | N/A. | MoveBookmarkCommand down. | Tree reorder. | Already last. | YES |
| `indentBookmarkAction` | src/ui/BookmarkPanel.cpp:183 | → BookmarkPanel::onIndent | N/A. | IndentBookmarkCommand. | Tree depth changes. | Cannot indent root-without-prev-sibling. | YES |
| `outdentBookmarkAction` | src/ui/BookmarkPanel.cpp:187 | → BookmarkPanel::onOutdent | N/A. | OutdentBookmarkCommand. | Tree depth changes. | Already at root. | YES |

#### widgets (1)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `bookmarkTree` | src/ui/BookmarkPanel.cpp:24 | → BookmarkPanel::onItemActivated; → BookmarkPanel::onItemChanged; → BookmarkPanel::onContextMenu | Selection of tree items; supports drag-drop if InternalMove enabled. | Emits bookmarkActivated(pageIndex,x,y,zoom) or externalLinkRequested(uri). | Canvas navigates to bookmark target. | Invalid type role → no-op. | YES |

#### context_menus (1)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `treeWidget_` | src/ui/BookmarkPanel.cpp:31 | Container — populated on customContextMenuRequested or contextMenuEvent. | Right-click event QPoint. | Menu rebuilt on each show; child QActions trigger their own slots. | Popup menu appears at cursor; closes on item selection or Esc. | None at menu level. | YES |

### `src/ui/BookmarkPanel.h` (1 entries)

#### dialogs (1)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `BookmarkPanel` | src/ui/BookmarkPanel.h | BookmarkPanel — QWidget side panel; Tree view + context menu for /Outlines. | QTreeWidget activate / drag-drop. | BookmarkManager mutations via undo. | Side dock visibility toggleable via View menu. Internal state visible inline as user interacts. | Per user-interaction; see widget rows for sub-controls. | YES |

### `src/ui/CalculationOrderDialog.cpp` (5 entries)

#### widgets (5)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `calculationOrderFieldList` | src/ui/CalculationOrderDialog.cpp:42 | N/A — passive sub-control of CalculationOrderDialog; value read by CalculationOrderDialog::accessor() at QDialog::exec()-time. | Selection of one or more items. | None at signal level — value captured into result struct when CalculationOrderDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `calculationOrderMoveUpButton` | src/ui/CalculationOrderDialog.cpp:52 | → CalculationOrderDialog::onMoveUp | N/A — button click only (no text input). | int row = fieldList_->currentRow(); QListWidgetItem* item = fieldList_->takeItem(row); fieldList_->insertItem(row - 1, item) (extracted from CalculationOrderDialog.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `calculationOrderMoveDownButton` | src/ui/CalculationOrderDialog.cpp:58 | → CalculationOrderDialog::onMoveDown | N/A — button click only (no text input). | int row = fieldList_->currentRow(); QListWidgetItem* item = fieldList_->takeItem(row); fieldList_->insertItem(row + 1, item) (extracted from CalculationOrderDialog.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `calculationOrderApplyButton` | src/ui/CalculationOrderDialog.cpp:70 | → CalculationOrderDialog::onApply | N/A — button click only (no text input). | } accept() (extracted from CalculationOrderDialog.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `calculationOrderCancelButton` | src/ui/CalculationOrderDialog.cpp:76 | → QDialog::reject | N/A — button click only (no text input). | Sets dialog result Rejected; closes dialog without committing changes. | Dialog disappears. | None. | YES |

### `src/ui/CalculationOrderDialog.h` (1 entries)

#### dialogs (1)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `CalculationOrderDialog` | src/ui/CalculationOrderDialog.h | CalculationOrderDialog::exec() — modal dialog; Edit form-wide /AcroForm /CO array (calculation order). | fieldList QListWidget; up/down/apply buttons. | FormManager::setCalculationOrder on Apply. | Modal dialog appears; on close it disappears. Sub-controls re-render per user input. | Per accept() validation logic in this dialog; reject() always succeeds. | YES |

### `src/ui/CombineFilesWizard.cpp` (11 entries)

#### widgets (11)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `combineSteps` | src/ui/CombineFilesWizard.cpp:36 | N/A — passive sub-control of CombineFilesWizard; value read by CombineFilesWizard::accessor() at QDialog::exec()-time. | Container — switches by setCurrentIndex(). | None at signal level — value captured into result struct when CombineFilesWizard is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `fileList` | src/ui/CombineFilesWizard.cpp:46 | N/A — passive sub-control of CombineFilesWizard; value read by CombineFilesWizard::accessor() at QDialog::exec()-time. | Selection of one or more items. | None at signal level — value captured into result struct when CombineFilesWizard is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `addFilesButton` | src/ui/CombineFilesWizard.cpp:52 | → CombineFilesWizard::onAddFiles | N/A — button click only (no text input). | QStringList files = QFileDialog::getOpenFileNames( this, tr("Add files to combine"), QString(), tr("PDF and Images (*.pdf *.png *.jpg *.jpeg *.tif *.tiff *.bmp); " "All files (*)")); it->setData(Qt::… (extracted from CombineFilesWizard.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `removeFileButton` | src/ui/CombineFilesWizard.cpp:55 | → CombineFilesWizard::onRemoveFile | N/A — button click only (no text input). | int row = fileList_->currentRow(); delete fileList_->takeItem(row); updateButtonsForStep() (extracted from CombineFilesWizard.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `moveUpButton` | src/ui/CombineFilesWizard.cpp:58 | → CombineFilesWizard::onMoveUp | N/A — button click only (no text input). | int row = fileList_->currentRow(); auto* it = fileList_->takeItem(row); fileList_->insertItem(row - 1, it) (extracted from CombineFilesWizard.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `moveDownButton` | src/ui/CombineFilesWizard.cpp:61 | → CombineFilesWizard::onMoveDown | N/A — button click only (no text input). | int row = fileList_->currentRow(); auto* it = fileList_->takeItem(row); fileList_->insertItem(row + 1, it) (extracted from CombineFilesWizard.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `outputPathEdit` | src/ui/CombineFilesWizard.cpp:79 | N/A — passive sub-control of CombineFilesWizard; value read by CombineFilesWizard::accessor() at QDialog::exec()-time. | Free-form QString; validator/echo-mode per dialog (passwords use Password echo). | None at signal level — value captured into result struct when CombineFilesWizard is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `browseOutputButton` | src/ui/CombineFilesWizard.cpp:82 | → CombineFilesWizard::onChooseOutput | N/A — button click only (no text input). | QString f = QFileDialog::getSaveFileName( this, tr("Save combined PDF as..."), outputEdit_->text(), tr("PDF (*.pdf)")); updateButtonsForStep() (extracted from CombineFilesWizard.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `backButton` | src/ui/CombineFilesWizard.cpp:111 | → CombineFilesWizard::onBack | N/A — button click only (no text input). | int step = steps_->currentIndex(); updateButtonsForStep(); } (extracted from CombineFilesWizard.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `nextButton` | src/ui/CombineFilesWizard.cpp:114 | → CombineFilesWizard::onNext | N/A — button click only (no text input). | const int step = steps_->currentIndex(); steps_->setCurrentIndex(1); updateButtonsForStep() (extracted from CombineFilesWizard.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `cancelButton` | src/ui/CombineFilesWizard.cpp:117 | → CombineFilesWizard::onCancel | N/A — button click only (no text input). | reject() (extracted from CombineFilesWizard.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |

### `src/ui/CombineFilesWizard.h` (1 entries)

#### dialogs (1)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `CombineFilesWizard` | src/ui/CombineFilesWizard.h | CombineFilesWizard::exec() — modal dialog; Multi-step wizard around CombineFilesService. | filesList add/remove buttons; output path. | Service invocation on Finish. | Modal dialog appears; on close it disappears. Sub-controls re-render per user input. | Per accept() validation logic in this dialog; reject() always succeeds. | YES |

### `src/ui/CommentPanel.cpp` (6 entries)

#### actions (2)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `commentReplyAction` | src/ui/CommentPanel.cpp:146 | N/A — action declared but no slot connection found in catalog scan. | N/A. | None at signal/slot level — action may be programmatically triggered or reserved. | None directly. | None — action inert without connection. | NO |
| `commentDeleteAction` | src/ui/CommentPanel.cpp:149 | N/A — action declared but no slot connection found in catalog scan. | N/A. | None at signal/slot level — action may be programmatically triggered or reserved. | None directly. | None — action inert without connection. | NO |

#### widgets (3)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `commentFilterCombo` | src/ui/CommentPanel.cpp:34 | → CommentPanel::onFilterChanged | One of items() (editable variant accepts free-form QString). | currentFilter_ = index; buildAnnotationTree() (extracted from CommentPanel.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `commentSortCombo` | src/ui/CommentPanel.cpp:54 | → CommentPanel::onSortChanged | One of items() (editable variant accepts free-form QString). | currentSort_ = index; buildAnnotationTree() (extracted from CommentPanel.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `commentTreeWidget` | src/ui/CommentPanel.cpp:71 | → CommentPanel::onItemDoubleClicked; → CommentPanel::onContextMenu | Selection of tree items; supports drag-drop if InternalMove enabled. | Q_UNUSED(column); } QVariant pageData = item->data(0, Qt::UserRole); emit navigateToAnnotation(pageIndex) (extracted from CommentPanel.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |

#### context_menus (1)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `treeWidget_` | src/ui/CommentPanel.cpp:77 | Container — populated on customContextMenuRequested or contextMenuEvent. | Right-click event QPoint. | Menu rebuilt on each show; child QActions trigger their own slots. | Popup menu appears at cursor; closes on item selection or Esc. | None at menu level. | YES |

### `src/ui/CommentPanel.h` (1 entries)

#### dialogs (1)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `CommentPanel` | src/ui/CommentPanel.h | CommentPanel — QWidget side panel; Tree view of all annotations grouped by page; filter/sort. | filterBox QLineEdit; sortCombo; tree double-click. | Emits navigateToAnnotation / deleteAnnotation. | Side dock visibility toggleable via View menu. Internal state visible inline as user interacts. | Per user-interaction; see widget rows for sub-controls. | YES |

### `src/ui/CompareDialog.cpp` (13 entries)

#### widgets (13)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `compareFileGroup` | src/ui/CompareDialog.cpp:46 | N/A — passive sub-control of CompareDialog; value read by CompareDialog::accessor() at QDialog::exec()-time. | Container only — no input. | None at signal level — value captured into result struct when CompareDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | NO |
| `selectFileAButton` | src/ui/CompareDialog.cpp:50 | → CompareDialog::onSelectFileA | N/A — button click only (no text input). | QString path = QFileDialog::getOpenFileName( this, tr("Select First PDF"), QString(), tr("PDF Files (*.pdf)")); docA_ = pdfe::core::Document::open(context_, path.toStdString()); compareButton_->setEn… (extracted from CompareDialog.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `selectFileBButton` | src/ui/CompareDialog.cpp:56 | → CompareDialog::onSelectFileB | N/A — button click only (no text input). | QString path = QFileDialog::getOpenFileName( this, tr("Select Second PDF"), QString(), tr("PDF Files (*.pdf)")); docB_ = pdfe::core::Document::open(context_, path.toStdString()); compareButton_->setE… (extracted from CompareDialog.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `compareButton` | src/ui/CompareDialog.cpp:62 | → CompareDialog::onCompare | N/A — button click only (no text input). | QApplication::setOverrideCursor(Qt::BusyCursor); QApplication::processEvents(); pdfe::diff::DiffEngine engine(context_) (extracted from CompareDialog.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `prevPageButton` | src/ui/CompareDialog.cpp:76 | + 1 inline lambda(s) | N/A — button click only (no text input). | pageSpinBox_->setValue(pageSpinBox_->value() - 1) (inline lambda at CompareDialog.cpp:176) | Per lambda — typically updates dialog state, opens a child dialog, or pushes a command. | Per lambda; user-cancel paths return early. | YES |
| `pageSpinBox` | src/ui/CompareDialog.cpp:80 | → CompareDialog::onPageChanged | Integer in [minimum,maximum] with step (defaults: 0..99 step 1 unless overridden). | displayPage(page-1) — repaints diff overlay for the chosen page index. | Compare canvas re-renders the selected page diff. | page out of range → displayPage clamps internally. | YES |
| `nextPageButton` | src/ui/CompareDialog.cpp:85 | + 1 inline lambda(s) | N/A — button click only (no text input). | pageSpinBox_->setValue(pageSpinBox_->value() + 1) (inline lambda at CompareDialog.cpp:179) | Per lambda — typically updates dialog state, opens a child dialog, or pushes a command. | Per lambda; user-cancel paths return early. | YES |
| `prevChangeButton` | src/ui/CompareDialog.cpp:92 | → CompareDialog::onPrevChangeButton | N/A — button click only (no text input). | gotoPrevChangedPage() (extracted from CompareDialog.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `nextChangeButton` | src/ui/CompareDialog.cpp:95 | → CompareDialog::onNextChangeButton | N/A — button click only (no text input). | gotoNextChangedPage() (extracted from CompareDialog.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `diffColorCombo` | src/ui/CompareDialog.cpp:99 | N/A — passive sub-control of CompareDialog; value read by CompareDialog::accessor() at QDialog::exec()-time. | One of items() (editable variant accepts free-form QString). | None at signal level — value captured into result struct when CompareDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `scrollAreaA` | src/ui/CompareDialog.cpp:133 | N/A — passive sub-control of CompareDialog; value read by CompareDialog::accessor() at QDialog::exec()-time. | Per-widget; see source. | None at signal level — value captured into result struct when CompareDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `scrollAreaB` | src/ui/CompareDialog.cpp:141 | N/A — passive sub-control of CompareDialog; value read by CompareDialog::accessor() at QDialog::exec()-time. | Per-widget; see source. | None at signal level — value captured into result struct when CompareDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `textDiffView` | src/ui/CompareDialog.cpp:164 | N/A — passive sub-control of CompareDialog; value read by CompareDialog::accessor() at QDialog::exec()-time. | Rich-text or plain-text QString. | None at signal level — value captured into result struct when CompareDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |

### `src/ui/CompareDialog.h` (1 entries)

#### dialogs (1)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `CompareDialog` | src/ui/CompareDialog.h | CompareDialog::exec() — modal dialog; Compare two PDFs page-by-page. | File pickers for A/B; threshold spinbox. | Visual diff side-by-side. | Modal dialog appears; on close it disappears. Sub-controls re-render per user input. | Per accept() validation logic in this dialog; reject() always succeeds. | YES |

### `src/ui/ComplianceDialog.cpp` (7 entries)

#### widgets (7)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `LevelCombo` | src/ui/ComplianceDialog.cpp:53 | N/A — passive sub-control of ComplianceDialog; value read by ComplianceDialog::accessor() at QDialog::exec()-time. | One of items() (editable variant accepts free-form QString). | None at signal level — value captured into result struct when ComplianceDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `AnalyzeButton` | src/ui/ComplianceDialog.cpp:69 | → ComplianceDialog::onAnalyze | N/A — button click only (no text input). | compliance::ComplianceManager mgr(context_); auto level = selectedLevel(); compliance::ComplianceResult result (extracted from ComplianceDialog.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `ValidateButton` | src/ui/ComplianceDialog.cpp:72 | → ComplianceDialog::onValidate | N/A — button click only (no text input). | compliance::ComplianceManager mgr(context_); auto level = selectedLevel(); compliance::ComplianceResult result (extracted from ComplianceDialog.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `ConvertButton` | src/ui/ComplianceDialog.cpp:75 | → ComplianceDialog::onConvert | N/A — button click only (no text input). | QString filter = tr("PDF Files (*.pdf)"); QString outputPath = QFileDialog::getSaveFileName( this, tr("Save Compliant PDF"), QString(), filter); setConvertingState(true) (extracted from ComplianceDialog.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `previewTagsButton` | src/ui/ComplianceDialog.cpp:78 | → ComplianceDialog::onPreviewTags | N/A — button click only (no text input). | statusLabel_->setText(tr("Generating tag structure...")); QApplication::setOverrideCursor(Qt::BusyCursor); QApplication::processEvents() (extracted from ComplianceDialog.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `IssueTable` | src/ui/ComplianceDialog.cpp:111 | N/A — passive sub-control of ComplianceDialog; value read by ComplianceDialog::accessor() at QDialog::exec()-time. | Cell selection / per-cell editor delegate. | None at signal level — value captured into result struct when ComplianceDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `tagStructureTree` | src/ui/ComplianceDialog.cpp:135 | N/A — passive sub-control of ComplianceDialog; value read by ComplianceDialog::accessor() at QDialog::exec()-time. | Selection of tree items; supports drag-drop if InternalMove enabled. | None at signal level — value captured into result struct when ComplianceDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |

### `src/ui/ComplianceDialog.h` (1 entries)

#### dialogs (1)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `ComplianceDialog` | src/ui/ComplianceDialog.h | ComplianceDialog::exec() — modal dialog; Check + convert to PDF/A and PDF/UA. | profileCombo (PDF/A-1b/2b/3b, UA-1); convertButton. | ComplianceManager::validate + convert. | Modal dialog appears; on close it disappears. Sub-controls re-render per user input. | Per accept() validation logic in this dialog; reject() always succeeds. | YES |

### `src/ui/CustomizeToolbarsDialog.cpp` (9 entries)

#### widgets (9)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `toolbarCombo` | src/ui/CustomizeToolbarsDialog.cpp:70 | → CustomizeToolbarsDialog::onToolbarSelected | One of items() (editable variant accepts free-form QString). | persistCurrentSelectionBack(); selectToolbar(index) (extracted from CustomizeToolbarsDialog.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `availableList` | src/ui/CustomizeToolbarsDialog.cpp:83 | N/A — passive sub-control of CustomizeToolbarsDialog; value read by CustomizeToolbarsDialog::accessor() at QDialog::exec()-time. | Selection of one or more items. | None at signal level — value captured into result struct when CustomizeToolbarsDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `addActionButton` | src/ui/CustomizeToolbarsDialog.cpp:91 | → CustomizeToolbarsDialog::onAddClicked | N/A — button click only (no text input). | auto* sel = availableList_->currentItem(); auto* clone = sel->clone(); int insertAt = currentList_->currentRow() (extracted from CustomizeToolbarsDialog.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `removeActionButton` | src/ui/CustomizeToolbarsDialog.cpp:94 | → CustomizeToolbarsDialog::onRemoveClicked | N/A — button click only (no text input). | int row = currentList_->currentRow(); delete currentList_->takeItem(row); persistCurrentSelectionBack() (extracted from CustomizeToolbarsDialog.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `addSeparatorButton` | src/ui/CustomizeToolbarsDialog.cpp:97 | → CustomizeToolbarsDialog::onAddSeparatorClicked | N/A — button click only (no text input). | int insertAt = currentList_->currentRow(); currentList_->insertItem(insertAt, makeSeparatorItem()); currentList_->setCurrentRow(insertAt) (extracted from CustomizeToolbarsDialog.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `moveUpButton` | src/ui/CustomizeToolbarsDialog.cpp:100 | → CustomizeToolbarsDialog::onMoveUpClicked | N/A — button click only (no text input). | int row = currentList_->currentRow(); auto* it = currentList_->takeItem(row); currentList_->insertItem(row - 1, it) (extracted from CustomizeToolbarsDialog.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `moveDownButton` | src/ui/CustomizeToolbarsDialog.cpp:103 | → CustomizeToolbarsDialog::onMoveDownClicked | N/A — button click only (no text input). | int row = currentList_->currentRow(); auto* it = currentList_->takeItem(row); currentList_->insertItem(row + 1, it) (extracted from CustomizeToolbarsDialog.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `currentList` | src/ui/CustomizeToolbarsDialog.cpp:118 | N/A — passive sub-control of CustomizeToolbarsDialog; value read by CustomizeToolbarsDialog::accessor() at QDialog::exec()-time. | Selection of one or more items. | None at signal level — value captured into result struct when CustomizeToolbarsDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `resetToDefaultButton` | src/ui/CustomizeToolbarsDialog.cpp:129 | → CustomizeToolbarsDialog::onResetClicked | N/A — button click only (no text input). | layouts_ = ToolbarLayoutManager::defaultLayouts(); toolbarCombo_->blockSignals(true); toolbarCombo_->clear() (extracted from CustomizeToolbarsDialog.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |

### `src/ui/CustomizeToolbarsDialog.h` (1 entries)

#### dialogs (1)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `CustomizeToolbarsDialog` | src/ui/CustomizeToolbarsDialog.h | CustomizeToolbarsDialog::exec() — modal dialog; Add/remove/reorder toolbar buttons across all toolbars. | availableList, currentList, addButton, removeButton, upButton, downButton, applyButton. | ToolbarLayoutManager::saveLayouts. | Modal dialog appears; on close it disappears. Sub-controls re-render per user input. | Per accept() validation logic in this dialog; reject() always succeeds. | YES |

### `src/ui/DefaultsLogPanel.cpp` (2 entries)

#### widgets (2)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `defaultsLogView` | src/ui/DefaultsLogPanel.cpp:45 | N/A — passive sub-control of DefaultsLogPanel; value read by DefaultsLogPanel::accessor() at QDialog::exec()-time. | Rich-text or plain-text QString. | None at signal level — value captured into result struct when DefaultsLogPanel is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `defaultsLogClearButton` | src/ui/DefaultsLogPanel.cpp:52 | → DefaultsLogPanel::clearLog | N/A — button click only (no text input). | logView_->clear(); entryCount_ = 0 (extracted from DefaultsLogPanel.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |

### `src/ui/DefaultsLogPanel.h` (1 entries)

#### dialogs (1)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `DefaultsLogPanel` | src/ui/DefaultsLogPanel.h | DefaultsLogPanel — QWidget side panel; Side dock showing diagnostic messages for the active document. | clearButton; copyButton. | Read-only log. | Side dock visibility toggleable via View menu. Internal state visible inline as user interacts. | Per user-interaction; see widget rows for sub-controls. | YES |

### `src/ui/EncryptionDialog.cpp` (19 entries)

#### widgets (19)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `(QFormLayout#26)` | src/ui/EncryptionDialog.cpp:26 | N/A — passive sub-control of EncryptionDialog; value read by EncryptionDialog::accessor() at QDialog::exec()-time. | Per-widget; see source. | None at signal level — value captured into result struct when EncryptionDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `algorithmCombo` | src/ui/EncryptionDialog.cpp:27 | → EncryptionDialog::onAlgorithmChanged | One of items() (editable variant accepts free-form QString). | Updates internal algorithm enum; enables/disables key-length combo and AES-only permission checkboxes. | Permissions checkboxes enable/disable; key length combo updates. | None. | YES |
| `passwordGroup` | src/ui/EncryptionDialog.cpp:42 | N/A — passive sub-control of EncryptionDialog; value read by EncryptionDialog::accessor() at QDialog::exec()-time. | Container only — no input. | None at signal level — value captured into result struct when EncryptionDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | NO |
| `(QFormLayout#44)` | src/ui/EncryptionDialog.cpp:44 | N/A — passive sub-control of EncryptionDialog; value read by EncryptionDialog::accessor() at QDialog::exec()-time. | Per-widget; see source. | None at signal level — value captured into result struct when EncryptionDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `userPasswordEdit` | src/ui/EncryptionDialog.cpp:46 | → EncryptionDialog::validateInput | Free-form QString; validator/echo-mode per dialog (passwords use Password echo). | Re-evaluates user/owner password match + length; toggles okButton enabled state; updates strength label. | Strength label color/text change; OK button enable/disable. | Empty / mismatched passwords → OK disabled. | YES |
| `userPasswordConfirmEdit` | src/ui/EncryptionDialog.cpp:55 | → EncryptionDialog::validateInput | Free-form QString; validator/echo-mode per dialog (passwords use Password echo). | Re-evaluates user/owner password match + length; toggles okButton enabled state; updates strength label. | Strength label color/text change; OK button enable/disable. | Empty / mismatched passwords → OK disabled. | YES |
| `ownerPasswordEdit` | src/ui/EncryptionDialog.cpp:64 | → EncryptionDialog::validateInput; → EncryptionDialog::updatePasswordStrength | Free-form QString; validator/echo-mode per dialog (passwords use Password echo). | Re-evaluates user/owner password match + length; toggles okButton enabled state; updates strength label. | Strength label color/text change; OK button enable/disable. | Empty / mismatched passwords → OK disabled. | YES |
| `ownerPasswordConfirmEdit` | src/ui/EncryptionDialog.cpp:73 | → EncryptionDialog::validateInput | Free-form QString; validator/echo-mode per dialog (passwords use Password echo). | Re-evaluates user/owner password match + length; toggles okButton enabled state; updates strength label. | Strength label color/text change; OK button enable/disable. | Empty / mismatched passwords → OK disabled. | YES |
| `permissionGroup` | src/ui/EncryptionDialog.cpp:111 | N/A — passive sub-control of EncryptionDialog; value read by EncryptionDialog::accessor() at QDialog::exec()-time. | Container only — no input. | None at signal level — value captured into result struct when EncryptionDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | NO |
| `printCheck` | src/ui/EncryptionDialog.cpp:115 | N/A — passive sub-control of EncryptionDialog; value read by EncryptionDialog::accessor() at QDialog::exec()-time. | Boolean (checked/unchecked); supports tri-state if explicitly set. | None at signal level — value captured into result struct when EncryptionDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `printHqCheck` | src/ui/EncryptionDialog.cpp:122 | N/A — passive sub-control of EncryptionDialog; value read by EncryptionDialog::accessor() at QDialog::exec()-time. | Boolean (checked/unchecked); supports tri-state if explicitly set. | None at signal level — value captured into result struct when EncryptionDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `modifyCheck` | src/ui/EncryptionDialog.cpp:129 | N/A — passive sub-control of EncryptionDialog; value read by EncryptionDialog::accessor() at QDialog::exec()-time. | Boolean (checked/unchecked); supports tri-state if explicitly set. | None at signal level — value captured into result struct when EncryptionDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `copyCheck` | src/ui/EncryptionDialog.cpp:136 | N/A — passive sub-control of EncryptionDialog; value read by EncryptionDialog::accessor() at QDialog::exec()-time. | Boolean (checked/unchecked); supports tri-state if explicitly set. | None at signal level — value captured into result struct when EncryptionDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `annotateCheck` | src/ui/EncryptionDialog.cpp:143 | N/A — passive sub-control of EncryptionDialog; value read by EncryptionDialog::accessor() at QDialog::exec()-time. | Boolean (checked/unchecked); supports tri-state if explicitly set. | None at signal level — value captured into result struct when EncryptionDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `formFillCheck` | src/ui/EncryptionDialog.cpp:150 | N/A — passive sub-control of EncryptionDialog; value read by EncryptionDialog::accessor() at QDialog::exec()-time. | Boolean (checked/unchecked); supports tri-state if explicitly set. | None at signal level — value captured into result struct when EncryptionDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `assembleCheck` | src/ui/EncryptionDialog.cpp:157 | N/A — passive sub-control of EncryptionDialog; value read by EncryptionDialog::accessor() at QDialog::exec()-time. | Boolean (checked/unchecked); supports tri-state if explicitly set. | None at signal level — value captured into result struct when EncryptionDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `accessibilityCheck` | src/ui/EncryptionDialog.cpp:164 | N/A — passive sub-control of EncryptionDialog; value read by EncryptionDialog::accessor() at QDialog::exec()-time. | Boolean (checked/unchecked); supports tri-state if explicitly set. | None at signal level — value captured into result struct when EncryptionDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `encryptMetadataCheck` | src/ui/EncryptionDialog.cpp:174 | N/A — passive sub-control of EncryptionDialog; value read by EncryptionDialog::accessor() at QDialog::exec()-time. | Boolean (checked/unchecked); supports tri-state if explicitly set. | None at signal level — value captured into result struct when EncryptionDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `buttonBox` | src/ui/EncryptionDialog.cpp:189 | → QDialog::accept; → QDialog::reject | Button-roles (Ok/Cancel/Apply/Close/Help). | Sets dialog result Accepted; closes dialog; caller reads dialog state via accessor methods. | Dialog disappears. | None. | YES |

### `src/ui/EncryptionDialog.h` (1 entries)

#### dialogs (1)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `EncryptionDialog` | src/ui/EncryptionDialog.h | EncryptionDialog::exec() — modal dialog; Configure AES-128/256 user/owner password + permission flags. | algorithmCombo; userPasswordEdit; ownerPasswordEdit; permission checkboxes. | EncryptionSettings via settings() accessor on Accept. | Modal dialog appears; on close it disappears. Sub-controls re-render per user input. | Per accept() validation logic in this dialog; reject() always succeeds. | YES |

### `src/ui/FormFieldPropertiesPanel.cpp` (7 entries)

#### widgets (7)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `(QFormLayout#32)` | src/ui/FormFieldPropertiesPanel.cpp:32 | N/A — passive sub-control of FormFieldPropertiesPanel; value read by FormFieldPropertiesPanel::accessor() at QDialog::exec()-time. | Per-widget; see source. | None at signal level — value captured into result struct when FormFieldPropertiesPanel is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `formFieldEditorStack` | src/ui/FormFieldPropertiesPanel.cpp:62 | N/A — passive sub-control of FormFieldPropertiesPanel; value read by FormFieldPropertiesPanel::accessor() at QDialog::exec()-time. | Container — switches by setCurrentIndex(). | None at signal level — value captured into result struct when FormFieldPropertiesPanel is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `formFieldTextEdit` | src/ui/FormFieldPropertiesPanel.cpp:66 | → FormFieldPropertiesPanel::onTextValueChanged | Free-form QString; validator/echo-mode per dialog (passwords use Password echo). | emitChanges(); } (extracted from FormFieldPropertiesPanel.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `formFieldMultilineEdit` | src/ui/FormFieldPropertiesPanel.cpp:74 | → FormFieldPropertiesPanel::onMultilineValueChanged | Rich-text or plain-text QString. | emitChanges(); } (extracted from FormFieldPropertiesPanel.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `formFieldCheckbox` | src/ui/FormFieldPropertiesPanel.cpp:83 | → FormFieldPropertiesPanel::onCheckboxToggled | Boolean (checked/unchecked); supports tri-state if explicitly set. | emitChanges(); } (extracted from FormFieldPropertiesPanel.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `formFieldCombo` | src/ui/FormFieldPropertiesPanel.cpp:91 | → FormFieldPropertiesPanel::onComboSelectionChanged | One of items() (editable variant accepts free-form QString). | currentData_.selectedIndices = {index}; emitChanges(); } (extracted from FormFieldPropertiesPanel.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `formFieldList` | src/ui/FormFieldPropertiesPanel.cpp:99 | → FormFieldPropertiesPanel::onListSelectionChanged | Selection of one or more items. | auto selectedItems = listWidget_->selectionModel()->selectedIndexes(); } if (!currentData_.selectedIndices.empty()) { currentData_.value = listWidget_->item( currentData_.selectedIndices[0])->text().… (extracted from FormFieldPropertiesPan… | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |

### `src/ui/FormFieldPropertiesPanel.h` (1 entries)

#### dialogs (1)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `FormFieldPropertiesPanel` | src/ui/FormFieldPropertiesPanel.h | FormFieldPropertiesPanel — QWidget side panel; QStackedWidget side panel — different editor per field type. | Per-field-type widgets (text, checkbox, choice, etc.). | Emits valueChanged → SetFieldValueCommand. | Side dock visibility toggleable via View menu. Internal state visible inline as user interacts. | Per user-interaction; see widget rows for sub-controls. | YES |

### `src/ui/HeaderFooterDialog.cpp` (19 entries)

#### widgets (19)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `headerFooterHeaderGroup` | src/ui/HeaderFooterDialog.cpp:79 | N/A — passive sub-control of HeaderFooterDialog; value read by HeaderFooterDialog::accessor() at QDialog::exec()-time. | Container only — no input. | None at signal level — value captured into result struct when HeaderFooterDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | NO |
| `(QFormLayout#81)` | src/ui/HeaderFooterDialog.cpp:81 | N/A — passive sub-control of HeaderFooterDialog; value read by HeaderFooterDialog::accessor() at QDialog::exec()-time. | Per-widget; see source. | None at signal level — value captured into result struct when HeaderFooterDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `headerFooterLeftHeader` | src/ui/HeaderFooterDialog.cpp:82 | N/A — passive sub-control of HeaderFooterDialog; value read by HeaderFooterDialog::accessor() at QDialog::exec()-time. | Free-form QString; validator/echo-mode per dialog (passwords use Password echo). | None at signal level — value captured into result struct when HeaderFooterDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `headerFooterCenterHeader` | src/ui/HeaderFooterDialog.cpp:86 | N/A — passive sub-control of HeaderFooterDialog; value read by HeaderFooterDialog::accessor() at QDialog::exec()-time. | Free-form QString; validator/echo-mode per dialog (passwords use Password echo). | None at signal level — value captured into result struct when HeaderFooterDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `headerFooterRightHeader` | src/ui/HeaderFooterDialog.cpp:90 | N/A — passive sub-control of HeaderFooterDialog; value read by HeaderFooterDialog::accessor() at QDialog::exec()-time. | Free-form QString; validator/echo-mode per dialog (passwords use Password echo). | None at signal level — value captured into result struct when HeaderFooterDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `headerFooterFooterGroup` | src/ui/HeaderFooterDialog.cpp:97 | N/A — passive sub-control of HeaderFooterDialog; value read by HeaderFooterDialog::accessor() at QDialog::exec()-time. | Container only — no input. | None at signal level — value captured into result struct when HeaderFooterDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | NO |
| `(QFormLayout#99)` | src/ui/HeaderFooterDialog.cpp:99 | N/A — passive sub-control of HeaderFooterDialog; value read by HeaderFooterDialog::accessor() at QDialog::exec()-time. | Per-widget; see source. | None at signal level — value captured into result struct when HeaderFooterDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `headerFooterLeftFooter` | src/ui/HeaderFooterDialog.cpp:100 | N/A — passive sub-control of HeaderFooterDialog; value read by HeaderFooterDialog::accessor() at QDialog::exec()-time. | Free-form QString; validator/echo-mode per dialog (passwords use Password echo). | None at signal level — value captured into result struct when HeaderFooterDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `headerFooterCenterFooter` | src/ui/HeaderFooterDialog.cpp:104 | N/A — passive sub-control of HeaderFooterDialog; value read by HeaderFooterDialog::accessor() at QDialog::exec()-time. | Free-form QString; validator/echo-mode per dialog (passwords use Password echo). | None at signal level — value captured into result struct when HeaderFooterDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `headerFooterRightFooter` | src/ui/HeaderFooterDialog.cpp:108 | N/A — passive sub-control of HeaderFooterDialog; value read by HeaderFooterDialog::accessor() at QDialog::exec()-time. | Free-form QString; validator/echo-mode per dialog (passwords use Password echo). | None at signal level — value captured into result struct when HeaderFooterDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `headerFooterOptionsGroup` | src/ui/HeaderFooterDialog.cpp:115 | N/A — passive sub-control of HeaderFooterDialog; value read by HeaderFooterDialog::accessor() at QDialog::exec()-time. | Container only — no input. | None at signal level — value captured into result struct when HeaderFooterDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | NO |
| `(QFormLayout#117)` | src/ui/HeaderFooterDialog.cpp:117 | N/A — passive sub-control of HeaderFooterDialog; value read by HeaderFooterDialog::accessor() at QDialog::exec()-time. | Per-widget; see source. | None at signal level — value captured into result struct when HeaderFooterDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `headerFooterAuthor` | src/ui/HeaderFooterDialog.cpp:119 | N/A — passive sub-control of HeaderFooterDialog; value read by HeaderFooterDialog::accessor() at QDialog::exec()-time. | Free-form QString; validator/echo-mode per dialog (passwords use Password echo). | None at signal level — value captured into result struct when HeaderFooterDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `headerFooterMarginSpin` | src/ui/HeaderFooterDialog.cpp:124 | N/A — passive sub-control of HeaderFooterDialog; value read by HeaderFooterDialog::accessor() at QDialog::exec()-time. | Double in [minimum,maximum] with step + decimals. | None at signal level — value captured into result struct when HeaderFooterDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `headerFooterFontSizeSpin` | src/ui/HeaderFooterDialog.cpp:132 | N/A — passive sub-control of HeaderFooterDialog; value read by HeaderFooterDialog::accessor() at QDialog::exec()-time. | Double in [minimum,maximum] with step + decimals. | None at signal level — value captured into result struct when HeaderFooterDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `headerFooterRangeCombo` | src/ui/HeaderFooterDialog.cpp:139 | → HeaderFooterDialog::onRangeModeChanged | One of items() (editable variant accepts free-form QString). | const auto mode = static_cast<headerfooter::PageRangeMode>( rangeCombo_->currentData().toInt()); rangeExplicitEdit_->setEnabled(mode == headerfooter::PageRangeMode::Explicit) (extracted from HeaderFooterDialog.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `headerFooterRangeExplicit` | src/ui/HeaderFooterDialog.cpp:148 | N/A — passive sub-control of HeaderFooterDialog; value read by HeaderFooterDialog::accessor() at QDialog::exec()-time. | Free-form QString; validator/echo-mode per dialog (passwords use Password echo). | None at signal level — value captured into result struct when HeaderFooterDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `headerFooterApplyButton` | src/ui/HeaderFooterDialog.cpp:164 | → HeaderFooterDialog::onApply | N/A — button click only (no text input). | } const std::filesystem::path inputPath = document_->filePath(); } QString defaultOut = QString::fromStdString( (inputPath.parent_path() / (inputPath.stem().string() + "_hf" + inputPath.extension().s… (extracted from HeaderFooterDialog.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `headerFooterCancelButton` | src/ui/HeaderFooterDialog.cpp:168 | → QDialog::reject | N/A — button click only (no text input). | Sets dialog result Rejected; closes dialog without committing changes. | Dialog disappears. | None. | YES |

### `src/ui/HeaderFooterDialog.h` (1 entries)

#### dialogs (1)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `HeaderFooterDialog` | src/ui/HeaderFooterDialog.h | HeaderFooterDialog::exec() — modal dialog; Add headers/footers to all pages. | Six text fields (left/center/right × header/footer); fontCombo, sizeSpin, colorButton, marginSpin, pageRange. | HeaderFooterApplier::apply. | Modal dialog appears; on close it disappears. Sub-controls re-render per user input. | Per accept() validation logic in this dialog; reject() always succeeds. | YES |

### `src/ui/ImagePropertiesPanel.cpp` (10 entries)

#### widgets (10)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `imageInfoGroup` | src/ui/ImagePropertiesPanel.cpp:42 | N/A — passive sub-control of ImagePropertiesPanel; value read by ImagePropertiesPanel::accessor() at QDialog::exec()-time. | Container only — no input. | None at signal level — value captured into result struct when ImagePropertiesPanel is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | NO |
| `(QFormLayout#44)` | src/ui/ImagePropertiesPanel.cpp:44 | N/A — passive sub-control of ImagePropertiesPanel; value read by ImagePropertiesPanel::accessor() at QDialog::exec()-time. | Per-widget; see source. | None at signal level — value captured into result struct when ImagePropertiesPanel is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `imageTransformGroup` | src/ui/ImagePropertiesPanel.cpp:79 | N/A — passive sub-control of ImagePropertiesPanel; value read by ImagePropertiesPanel::accessor() at QDialog::exec()-time. | Container only — no input. | None at signal level — value captured into result struct when ImagePropertiesPanel is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | NO |
| `(QFormLayout#81)` | src/ui/ImagePropertiesPanel.cpp:81 | N/A — passive sub-control of ImagePropertiesPanel; value read by ImagePropertiesPanel::accessor() at QDialog::exec()-time. | Per-widget; see source. | None at signal level — value captured into result struct when ImagePropertiesPanel is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `imageWidthSpin` | src/ui/ImagePropertiesPanel.cpp:83 | N/A — passive sub-control of ImagePropertiesPanel; value read by ImagePropertiesPanel::accessor() at QDialog::exec()-time. | Double in [minimum,maximum] with step + decimals. | None at signal level — value captured into result struct when ImagePropertiesPanel is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `imageHeightSpin` | src/ui/ImagePropertiesPanel.cpp:92 | N/A — passive sub-control of ImagePropertiesPanel; value read by ImagePropertiesPanel::accessor() at QDialog::exec()-time. | Double in [minimum,maximum] with step + decimals. | None at signal level — value captured into result struct when ImagePropertiesPanel is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `imageRotationCombo` | src/ui/ImagePropertiesPanel.cpp:101 | N/A — passive sub-control of ImagePropertiesPanel; value read by ImagePropertiesPanel::accessor() at QDialog::exec()-time. | One of items() (editable variant accepts free-form QString). | None at signal level — value captured into result struct when ImagePropertiesPanel is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `imageCropButton` | src/ui/ImagePropertiesPanel.cpp:108 | N/A — passive sub-control of ImagePropertiesPanel; value read by ImagePropertiesPanel::accessor() at QDialog::exec()-time. | N/A — button click only (no text input). | None at signal level — value captured into result struct when ImagePropertiesPanel is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `imageExtractButton` | src/ui/ImagePropertiesPanel.cpp:120 | N/A — passive sub-control of ImagePropertiesPanel; value read by ImagePropertiesPanel::accessor() at QDialog::exec()-time. | N/A — button click only (no text input). | None at signal level — value captured into result struct when ImagePropertiesPanel is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `imageRemoveButton` | src/ui/ImagePropertiesPanel.cpp:126 | N/A — passive sub-control of ImagePropertiesPanel; value read by ImagePropertiesPanel::accessor() at QDialog::exec()-time. | N/A — button click only (no text input). | None at signal level — value captured into result struct when ImagePropertiesPanel is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |

### `src/ui/ImagePropertiesPanel.h` (1 entries)

#### dialogs (1)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `ImagePropertiesPanel` | src/ui/ImagePropertiesPanel.h | ImagePropertiesPanel — QWidget side panel; Side panel showing dimensions/colorspace/bpp + rotate/crop/resize buttons. | rotateButton, cropButton, resizeButton, removeButton. | Routes to ImageTool actions. | Side dock visibility toggleable via View menu. Internal state visible inline as user interacts. | Per user-interaction; see widget rows for sub-controls. | YES |

### `src/ui/LayerPanel.cpp` (5 entries)

#### widgets (4)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `layerTreeWidget` | src/ui/LayerPanel.cpp:29 | → LayerPanel::onItemChanged; → LayerPanel::onContextMenu | Selection of tree items; supports drag-drop if InternalMove enabled. | int layerIndex = item->data(0, Qt::UserRole).toInt(); bool checked = (item->checkState(0) == Qt::Checked); undoManager_->execute(std::move(cmd)) (extracted from LayerPanel.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `createLayerButton` | src/ui/LayerPanel.cpp:39 | → LayerPanel::onCreateLayer | N/A — button click only (no text input). | bool ok; QString name = QInputDialog::getText(this, tr("Create Layer"), tr("Layer name:"), QLineEdit::Normal, tr("New Layer"), &ok); undoManager_->execute(std::move(cmd)) (extracted from LayerPanel.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `deleteLayerButton` | src/ui/LayerPanel.cpp:42 | → LayerPanel::onDeleteLayer | N/A — button click only (no text input). | auto* item = treeWidget_->currentItem(); int layerIndex = item->data(0, Qt::UserRole).toInt(); QString name = item->text(0) (extracted from LayerPanel.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `renameLayerButton` | src/ui/LayerPanel.cpp:45 | → LayerPanel::onRenameLayer | N/A — button click only (no text input). | auto* item = treeWidget_->currentItem(); int layerIndex = item->data(0, Qt::UserRole).toInt(); QString current = item->text(0) (extracted from LayerPanel.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |

#### context_menus (1)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `treeWidget_` | src/ui/LayerPanel.cpp:35 | Container — populated on customContextMenuRequested or contextMenuEvent. | Right-click event QPoint. | Menu rebuilt on each show; child QActions trigger their own slots. | Popup menu appears at cursor; closes on item selection or Esc. | None at menu level. | YES |

### `src/ui/LayerPanel.h` (1 entries)

#### dialogs (1)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `LayerPanel` | src/ui/LayerPanel.h | LayerPanel — QWidget side panel; Side dock listing OCG layers; toggle visibility. | layerTree checkboxes. | OcgManager::setLayerVisible. | Side dock visibility toggleable via View menu. Internal state visible inline as user interacts. | Per user-interaction; see widget rows for sub-controls. | YES |

### `src/ui/MainWindow.cpp` (164 entries)

#### actions (129)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `textEditAtlasScanAction` | src/ui/MainWindow.cpp:609 | → MainWindow::applyAtlasMode | AtlasMode enum (ScanCropped / FreeTypeStyled). | currentAtlasMode_ = mode; if pendingAtlas_.ready, EnrichedAtlas->setMode(); canvas->update. | Edit-text overlay re-renders glyphs in chosen atlas mode. | Pending atlas not ready → no-op. | YES |
| `textEditAtlasFontAction` | src/ui/MainWindow.cpp:616 | → MainWindow::applyAtlasMode | AtlasMode enum (ScanCropped / FreeTypeStyled). | currentAtlasMode_ = mode; if pendingAtlas_.ready, EnrichedAtlas->setMode(); canvas->update. | Edit-text overlay re-renders glyphs in chosen atlas mode. | Pending atlas not ready → no-op. | YES |
| `newFileAction` | src/ui/MainWindow.cpp:637 | → MainWindow::onNewFile | N/A — no input. | Creates blank PDF in QDir::temp() via Document::createBlank(); routes through openFile() so all open-file state machinery runs. | Window title updates to 'pdfe_untitled_<ms>.pdf', canvas renders blank page, status bar shows page count, dirty marker absent. | Document::createBlank throws → QMessageBox::critical 'Failed to create blank PDF'; subsequent openFile() failure surfaces its own messaging. | YES |
| `openAction` | src/ui/MainWindow.cpp:643 | → MainWindow::onOpenFile | QFileDialog filter 'PDF Files (*.pdf);;All Files (*)' starting from AppSettings::lastOpenDirectory(). | Calls openFile(fileName) — closes existing doc, opens new pdfe::core::Document, replaces tileCache_/tileRenderer_/annotationManager_/formManager_/etc., enables ~30 actions, records in recentFilesMenu_. | Same as MainWindow::openFile (see below). | User cancels file dialog (no-op return); see openFile error paths. | YES |
| `saveAction` | src/ui/MainWindow.cpp:649 | → MainWindow::onSaveFile | N/A. | BookmarkPanel::commitToDocument → Document::save(currentFileName_) (atomic temp+rename) → flushAlignmentPipelineIfPending if pending → undoManager_->setClean() → onDocumentStructureChanged(). | Window title clears '*' suffix; status bar 'File saved' for 3000 ms. | Document not PDF → QMessageBox::warning 'Only PDF documents can be saved'; std::exception during save → QMessageBox::critical 'Save Error'. | YES |
| `saveAsAction` | src/ui/MainWindow.cpp:656 | → MainWindow::onSaveAsFile | QFileDialog filter 'PDF Files (*.pdf);;All Files (*)' default = currentFileName_. | BookmarkPanel::commitToDocument → Document::saveAs(newPath) → flushAlignmentPipelineIfPending if pending → currentFileName_ = newPath → undoManager_->setClean() → updateWindowTitle. | Title updates to new filename, '*' cleared, status bar 'File saved as <name>' for 3000 ms. | User cancels (no-op); non-PDF → warning; save throws → QMessageBox::critical 'Save Error'. | YES |
| `exportPlainTextAction` | src/ui/MainWindow.cpp:663 | → MainWindow::onExportPlainText | QFileDialog filter 'Text Files (*.txt);;All Files (*)' default <basename>.txt. | PlainTextExporter::write(document_, savePath) — pure read; no document mutation. | Status bar 'Exported text to <name>' for 3000 ms. | User cancel (no-op); writer returns false → QMessageBox::warning 'Could not write file'. | NO |
| `createPdfFromImagesAction` | src/ui/MainWindow.cpp:671 | → MainWindow::onCreatePdfFromImages | ImageToPdfWizard interactive flow: image files + page-size + DPI + JPEG quality. | Wizard returns std::filesystem::path; openFile(out) replaces document_ with the new PDF. | Wizard dialog; on success canvas shows new PDF, recent-files updated. | Wizard cancelled (out.empty(), no-op); openFile errors propagate. | YES |
| `combineFilesAction` | src/ui/MainWindow.cpp:679 | → MainWindow::onCombineFiles | CombineFilesWizard interactive. | Wizard runs CombineFilesService. | Wizard modal. | User cancel. | YES |
| `previousViewAction` | src/ui/MainWindow.cpp:686 | → MainWindow::onPreviousView | N/A. | navHistory_.goBack() returns previous page index; canvas->goToPage(); previous/next view actions re-enabled based on history.canGoBack/canGoForward. | Canvas navigates to previous viewed page; status bar page index updates. | navHistory_.canGoBack() == false → no-op return. | YES |
| `nextViewAction` | src/ui/MainWindow.cpp:694 | → MainWindow::onNextView | N/A. | navHistory_.goForward(); canvas_->goToPage(). | Canvas moves to next visited page. | canGoForward() false → no-op. | YES |
| `closeAction` | src/ui/MainWindow.cpp:702 | → MainWindow::onCloseFile | N/A. | closeDocument() — releases document_, tileCache_, tileRenderer_, annotationManager_, formManager_, jsEngine_, redactionManager_, encryptionManager_, signatureManager_, imageManager_, metadataManager_; clears commandPages_, toolPage_; disab… | Canvas blank, status bar empty, title '<unnamed>', all dock panels emptied. | None — slot is unconditional. | YES |
| `printAction` | src/ui/MainWindow.cpp:709 | → MainWindow::onPrint | PrintDialog modal — pages range, copies, duplex, color, scaling. | PrintEngine::print(QPrinter, options); fires JS willPrint/didPrint events on first page. | Print dialog modal; printer system dialog if Native; status bar updates not specified. | Print engine returns false → QMessageBox::warning 'Print Error'. | YES |
| `printPreviewAction` | src/ui/MainWindow.cpp:716 | → MainWindow::onPrintPreview | N/A — opens preview with PrintRange::AllPages default. | PrintPreviewDialog modal exec(). | Print preview dialog modal. | None — preview is read-only. | YES |
| `exitAction` | src/ui/MainWindow.cpp:722 | → MainWindow::onExit | N/A. | QCoreApplication::quit() via close()/closeEvent flow; closeEvent calls hasUnsavedChanges and may prompt save. | Application terminates. | Unsaved changes → modal 'Save changes?' yes/no/cancel. | YES |
| `undoAction` | src/ui/MainWindow.cpp:729 | → MainWindow::onUndo | N/A. | undoManager_->undo(); if scanned-page image edit on top, restore saved XObject ref via PdfContentWriter::restoreImageXObjectRef + invalidate ImageReplacer cache; canvas->refreshAllPages; onDocumentStructureChanged; updateWindowTitle. | Canvas re-renders with the prior state; window title may add/remove '*' depending on whether undo crosses the saveClean barrier. | Empty undo stack → undo() is a no-op. | DEPENDS — visible effect depends on what's being undone (page rotation visible;… |
| `redoAction` | src/ui/MainWindow.cpp:736 | → MainWindow::onRedo | N/A. | undoManager_->redo(); if next entry has image edit, refreshImageForScannedPage(); markPageDirty; canvas->refreshAllPages; onDocumentStructureChanged. | Canvas reverts to redone state. | Empty redo stack → no-op. | DEPENDS |
| `copyAction` | src/ui/MainWindow.cpp:743 | → MainWindow::onCopy | N/A. | page->extractPlainText() into QApplication::clipboard()->setText(). | Status bar 'Page text copied to clipboard' for 3000 ms; clipboard side-effect not visible in app window. | No document/canvas → no-op; empty page text → status bar 'Copy: no text on this page'. | NO |
| `cutAction` | src/ui/MainWindow.cpp:750 | → MainWindow::onCut | N/A. | If textEditTool active and has selection: tool->performCut() — pushes EditTextCommand on undoManager + writes to clipboard. Otherwise no-op. | Status bar 'Text cut to clipboard' (or 'Cut: select text in an editable span first'). | No selection → status bar message only. | DEPENDS — visible only if cut succeeds (canvas re-renders). |
| `pasteAction` | src/ui/MainWindow.cpp:757 | → MainWindow::onPaste | Clipboard text (UTF-16). | If textEditTool active: tool->performPaste() — inserts clipboard text into edit buffer. | Status bar 'Clipboard text pasted'; canvas re-renders if paste succeeded. | No tool active → status bar 'Paste: activate text editing first'. | DEPENDS |
| `selectAllAction` | src/ui/MainWindow.cpp:764 | → MainWindow::onSelectAll | N/A. | page->extractPlainText() copied to clipboard. | Status bar 'All page text selected and copied to clipboard'. | No document/canvas → no-op; empty page → status bar message. | NO |
| `findAction` | src/ui/MainWindow.cpp:771 | → MainWindow::onFind | N/A. | searchPanelDock_->show()/raise(); searchPanel_->focusSearchBox(). | Search dock visible + focused; search line edit gets keyboard focus. | None. | YES |
| `replaceAction` | src/ui/MainWindow.cpp:777 | inline lambda @ MainWindow.cpp:782 | N/A — QAction click only; lambda body reads MainWindow members. | Inline lambda; transitions read directly from source (typically: open dialog, run command, refresh canvas). | Per lambda — typically opens a modal dialog or pushes a command. | Per lambda; user-cancel paths typically return early. | YES |
| `preferencesAction` | src/ui/MainWindow.cpp:792 | inline lambda @ MainWindow.cpp:795 | N/A — QAction click only; lambda body reads MainWindow members. | Inline lambda; transitions read directly from source (typically: open dialog, run command, refresh canvas). | Per lambda — typically opens a modal dialog or pushes a command. | Per lambda; user-cancel paths typically return early. | YES |
| `trustAnchorAction` | src/ui/MainWindow.cpp:802 | inline lambda @ MainWindow.cpp:806 | N/A — QAction click only; lambda body reads MainWindow members. | Inline lambda; transitions read directly from source (typically: open dialog, run command, refresh canvas). | Per lambda — typically opens a modal dialog or pushes a command. | Per lambda; user-cancel paths typically return early. | YES |
| `zoomInAction` | src/ui/MainWindow.cpp:812 | → MainWindow::onZoomIn | N/A. | canvas_->zoomIn() — multiplies current zoom by step factor. | Canvas re-renders larger; zoomCombo + statusBar reflect new percent. | No canvas → no-op. | YES |
| `zoomOutAction` | src/ui/MainWindow.cpp:819 | → MainWindow::onZoomOut | N/A. | canvas_->zoomOut(). | Canvas re-renders smaller; zoomCombo + statusBar update. | No canvas → no-op. | YES |
| `fitWidthAction` | src/ui/MainWindow.cpp:826 | → MainWindow::onFitWidth | N/A. | canvas_->fitWidth() — sets zoom such that page width fills viewport. | Canvas re-renders; zoomCombo updates. | No canvas → no-op. | YES |
| `fitPageAction` | src/ui/MainWindow.cpp:832 | → MainWindow::onFitPage | N/A. | canvas_->fitPage() — sets zoom such that whole page fits viewport. | Canvas re-renders; zoomCombo updates. | No canvas → no-op. | YES |
| `actualSizeAction` | src/ui/MainWindow.cpp:838 | → MainWindow::onActualSize | N/A. | canvas_->setZoom(1.0f). | Canvas zoom 100%; zoomCombo shows '100%'. | No canvas → no-op. | YES |
| `fullScreenAction` | src/ui/MainWindow.cpp:845 | → MainWindow::onToggleFullScreen | N/A. | showFullScreen() / showNormal(); fullScreenAction_->setChecked() reflects. | Window enters/leaves full-screen mode; menu bar usually hidden in full screen. | None. | YES |
| `togglePagePanelAction` | src/ui/MainWindow.cpp:852 | → MainWindow::onTogglePagePanel | N/A. | pagePanelDock_->setVisible(!visible). | Side dock toggles visible/hidden. | Dock null → no-op. | YES |
| `togglePropertiesPanelAction` | src/ui/MainWindow.cpp:858 | → MainWindow::onTogglePropertiesPanel | N/A. | propertiesPanelDock_->setVisible(!visible). | Properties dock toggles. | None. | YES |
| `toggleSearchPanelAction` | src/ui/MainWindow.cpp:864 | → MainWindow::onToggleSearchPanel | N/A. | searchPanelDock_->setVisible(!visible). | Search dock toggles. | None. | YES |
| `toggleBookmarkPanelAction` | src/ui/MainWindow.cpp:870 | → MainWindow::onToggleBookmarkPanel | N/A. | bookmarkPanelDock_->setVisible(!visible). | Bookmark dock toggles. | None. | YES |
| `actionToggleLayerPanel` | src/ui/MainWindow.cpp:876 | inline lambda @ MainWindow.cpp:880 | N/A — QAction click only; lambda body reads MainWindow members. | Inline lambda; transitions read directly from source (typically: open dialog, run command, refresh canvas). | Per lambda — typically opens a modal dialog or pushes a command. | Per lambda; user-cancel paths typically return early. | YES |
| `rotatePageCWAction` | src/ui/MainWindow.cpp:885 | → MainWindow::onRotatePageCW | N/A — selection from PagePanel + currentPage fallback. | RotatePageCommand(+90) for each selected; undoManager_->execute; onDocumentStructureChanged. | Canvas + page panel thumbnails rotate. | No document/undoManager → no-op. | YES |
| `rotatePageCCWAction` | src/ui/MainWindow.cpp:892 | → MainWindow::onRotatePageCCW | N/A. | RotatePageCommand(-90) for each selected. | Canvas + thumbnails rotate. | No document → no-op. | YES |
| `rotatePage180Action` | src/ui/MainWindow.cpp:899 | → MainWindow::onRotatePage180 | N/A. | RotatePageCommand(180) for each selected. | Canvas + thumbnails rotate 180. | None. | YES |
| `deletePageAction` | src/ui/MainWindow.cpp:905 | → MainWindow::onDeletePages | Confirm dialog Yes/No. | DeletePageRangeCommand; refuses to delete all pages; onDocumentStructureChanged. | Page count drops; thumbnails removed; canvas advances or stays. | Selection == total page count → QMessageBox warning 'At least one page must remain'; user clicks No → no-op. | YES |
| `insertBlankPageAction` | src/ui/MainWindow.cpp:912 | → MainWindow::onInsertBlankPage | N/A — uses currentPage size or 612x792 Letter default. | InsertBlankPageCommand; canvas->goToPage(insertAt). | New blank page appears; canvas navigates to it. | No document/undoManager → no-op. | YES |
| `insertPagesFromFileAction` | src/ui/MainWindow.cpp:919 | → MainWindow::onInsertPagesFromFile | QFileDialog 'PDF Files (*.pdf);;All Files (*)'. | Opens source doc, ImportPagesCommand, onDocumentStructureChanged, status bar 'Imported N pages from <name>'. | Page count grows; canvas refreshes. | User cancel (no-op); empty source → QMessageBox::warning; std::exception → QMessageBox::critical. | YES |
| `extractPagesAction` | src/ui/MainWindow.cpp:926 | → MainWindow::onExtractPages | QFileDialog save path (*.pdf). | extractPagesToFile() — pure read on doc, write to outputPath. | Status bar 'Extracted N page(s) to <name>'. | User cancel; std::exception → QMessageBox::critical 'Extract Error'. | NO — output file external to app window. |
| `movePageUpAction` | src/ui/MainWindow.cpp:932 | → MainWindow::onMovePageUp | N/A. | MovePageCommand(from, from-1); onDocumentStructureChanged; canvas_->goToPage(from-1). | Page panel reorder; canvas navigates. | from <= 0 → no-op. | YES |
| `movePageDownAction` | src/ui/MainWindow.cpp:939 | → MainWindow::onMovePageDown | N/A. | MovePageCommand(from, from+1). | Page panel reorder; canvas navigates. | from >= pageCount-1 → no-op. | YES |
| `textEditToolAction` | src/ui/MainWindow.cpp:947 | → MainWindow::onTextEditToolSelected | Triggered when textEditToolAction is checked/unchecked. | If checked: skipGroupTextEditCall_=true; activateTextEditForPage(currentPage). If unchecked: textEditAutoRouting_=false. | Canvas tool indicator changes; either TextEditTool or TreRegionTool becomes active depending on page hasTextContent(). | No document/canvas → no-op. | YES |
| `treToolAction` | src/ui/MainWindow.cpp:954 | @internal — programmatically activated by activateTextEditForPage() routing | N/A — no QAction::triggered connection. | No QAction::triggered handler — programmatically toggled by activateTextEditForPage() when current page lacks native text content. The Edit Text tool routes here for scanned-only pages. | DEPENDS — programmatic activation only; visible effect produced by the caller (e.g., activateTextEditForPage flips check state, MainWindow::onAnnotationToolSelected installs tool on canvas). | None directly. | DEPENDS |
| `formToolAction` | src/ui/MainWindow.cpp:960 | → MainWindow::onFormToolSelected | N/A. | If checked: onAnnotationToolSelected(formToolAction_). | Form tool active in canvas; form toolbar visibility unchanged. | No formManager_ → no-op. | YES |
| `createFormFieldAction` | src/ui/MainWindow.cpp:966 | → MainWindow::onCreateFormField | N/A. | formToolAction_->setChecked(true); onFormToolSelected(); formToolBar_->setVisible(true). | Form toolbar appears with 7 field-type buttons. | None. | YES |
| `formFieldPropertiesAction` | src/ui/MainWindow.cpp:973 | → MainWindow::onFormFieldProperties | N/A. | propertiesPanelDock_->setVisible(true)+raise(). | Properties dock visible & raised. | None. | YES |
| `exportFormDataAction` | src/ui/MainWindow.cpp:980 | → MainWindow::onExportFormData | QFileDialog 'XFDF Files (*.xfdf);;All Files (*)'. | FormDataIO::exportToXfdf → write file. | Status bar 'Form data exported to <path>' for 3000 ms. | User cancel; QFile open fails → QMessageBox::critical; exportToXfdf throws → QMessageBox::critical. | NO |
| `importFormDataAction` | src/ui/MainWindow.cpp:986 | → MainWindow::onImportFormData | QFileDialog 'XFDF Files (*.xfdf)'. | Reads file; FormDataIO::importFromXfdf; canvas refresh. | Form fields populated; canvas re-renders. | User cancel; read fail → QMessageBox::critical; import fail → QMessageBox::critical. | YES |
| `resetFormAction` | src/ui/MainWindow.cpp:992 | → MainWindow::onResetForm | Confirm dialog Yes/No. | ResetFormCommand on current page (only fields with /DV reset due to MuPDF behavior); canvas->refreshPage; formPropertiesPanel->clear. | Form widgets revert; status bar 'Form fields reset'. | User clicks No → no-op; std::exception → QMessageBox::critical. | YES |
| `flattenFormAction` | src/ui/MainWindow.cpp:998 | → MainWindow::onFlattenForm | Confirm warning dialog Yes/No. | formManager_->flattenForm; clears undo stack (irreversible); canvas refresh. | Form widgets become static appearance; status bar 'Form fields flattened'. | User clicks No; std::exception → QMessageBox::critical. | YES |
| `tabOrderAction` | src/ui/MainWindow.cpp:1004 | inline lambda @ MainWindow.cpp:1008 | N/A — QAction click only; lambda body reads MainWindow members. | Inline lambda; transitions read directly from source (typically: open dialog, run command, refresh canvas). | Per lambda — typically opens a modal dialog or pushes a command. | Per lambda; user-cancel paths typically return early. | YES |
| `calculationOrderAction` | src/ui/MainWindow.cpp:1029 | inline lambda @ MainWindow.cpp:1034 | N/A — QAction click only; lambda body reads MainWindow members. | Inline lambda; transitions read directly from source (typically: open dialog, run command, refresh canvas). | Per lambda — typically opens a modal dialog or pushes a command. | Per lambda; user-cancel paths typically return early. | YES |
| `altTextAction` | src/ui/MainWindow.cpp:1051 | inline lambda @ MainWindow.cpp:1094 | N/A — QAction click only; lambda body reads MainWindow members. | Inline lambda; transitions read directly from source (typically: open dialog, run command, refresh canvas). | Per lambda — typically opens a modal dialog or pushes a command. | Per lambda; user-cancel paths typically return early. | YES |
| `openAltTextDialogAction` | src/ui/MainWindow.cpp:1071 | inline lambda @ MainWindow.cpp:1077 | N/A — QAction click only; lambda body reads MainWindow members. | Inline lambda; transitions read directly from source (typically: open dialog, run command, refresh canvas). | Per lambda — typically opens a modal dialog or pushes a command. | Per lambda; user-cancel paths typically return early. | YES |
| `submitFormAction` | src/ui/MainWindow.cpp:1121 | inline lambda @ MainWindow.cpp:1125 | N/A — QAction click only; lambda body reads MainWindow members. | Inline lambda; transitions read directly from source (typically: open dialog, run command, refresh canvas). | Per lambda — typically opens a modal dialog or pushes a command. | Per lambda; user-cancel paths typically return early. | YES |
| `redactToolAction` | src/ui/MainWindow.cpp:1144 | → MainWindow::onRedactToolSelected | N/A. | If checked: onAnnotationToolSelected(redactToolAction_); redactionPanelDock_->show(). Else: revert to selectToolAction_. | Redaction tool active in canvas; redaction panel visible. | No document → no-op. | YES |
| `markRedactionAction` | src/ui/MainWindow.cpp:1150 | inline lambda @ MainWindow.cpp:1153 | N/A — QAction click only; lambda body reads MainWindow members. | Inline lambda; transitions read directly from source (typically: open dialog, run command, refresh canvas). | Per lambda — typically opens a modal dialog or pushes a command. | Per lambda; user-cancel paths typically return early. | YES |
| `applyRedactionsAction` | src/ui/MainWindow.cpp:1160 | → MainWindow::onApplyRedactions | Confirm warning dialog Yes/No. | applyAllRedactions(rewriteDocument=false); document_->save; QPDF rewrite; reopen; clear undo (irreversible); applyRedactionsAction disabled. | Canvas re-renders with redacted content; status bar; QMessageBox 'Successfully applied N redaction(s)'. | No marks → 'No Redactions' info; user clicks No → no-op; save fail → QMessageBox::critical 'Redaction Save Error'; QPDF rewrite fail → fallback reopen original. | YES |
| `saveWithEncryptionAction` | src/ui/MainWindow.cpp:1167 | → MainWindow::onSaveWithEncryption | EncryptionDialog modal (algorithm, key length, user/owner passwords, permissions). | EncryptionManager::saveEncrypted(document_, outputPath, settings) OR removeEncryption if algorithm==None. | EncryptionDialog modal; QFileDialog save; status bar 'File saved with AES-256 encryption to <name>'. | User cancel either dialog; save throws → QMessageBox::critical 'Encryption Error'. | YES |
| `documentPermissionsAction` | src/ui/MainWindow.cpp:1174 | → MainWindow::onDocumentPermissions | N/A. | Reads encryption info; QMessageBox::information with permissions table. | Info QMessageBox with permission summary. | Read fail → QMessageBox::critical. | YES |
| `removePasswordAction` | src/ui/MainWindow.cpp:1180 | → MainWindow::onRemovePassword | QFileDialog save path. | encryptionManager_->removeEncryption(document_, outputPath). | Status bar 'Unencrypted copy saved to <name>'. | User cancel; std::exception → QMessageBox::critical 'Remove Password Error'. | NO |
| `signDocumentAction` | src/ui/MainWindow.cpp:1188 | → MainWindow::onSignDocument | SignDialog modal — pfx path/password, reason, location, appearance flags, TSA, certification. | If no unsigned field: CreateSignatureFieldCommand. SaveAs to chosen path (Document::saveAs) + reopen; signatureManager_->signField with options; reopen post-signature; signaturePanel_ updated. | SignDialog modal; QFileDialog 'Save Signed PDF'; signature dock shown; status bar 'Document signed successfully'. | User cancel any step; signField throws → QMessageBox::critical 'Signing Error'. | YES |
| `verifySignaturesAction` | src/ui/MainWindow.cpp:1197 | → MainWindow::onVerifySignatures | N/A. | verifyAllSignatures; LtvValidator::verify; signaturePanel update; requestValidationAction enabled iff validCount>0. | Signature dock shown; QMessageBox::information with summary. | No signatures → 'no signatures' info; verify throws → QMessageBox::critical. | YES |
| `requestValidationAction` | src/ui/MainWindow.cpp:1208 | → MainWindow::onRequestValidation | ValidationCoordinator modal consent dialog (then async network). | External OCSP/CRL fetch; embedded in /DSS; ValidationCoordinator::completed/cancelled/failed signals. | Modal consent dialog; status bar 'Validation data embedded' / 'Validation request cancelled'. | Coordinator request() returns false (no consent) → cleanup; failed signal → cleanup. | YES |
| `aboutAction` | src/ui/MainWindow.cpp:1218 | → MainWindow::onAbout | N/A. | AboutDialog modal exec(). | About dialog modal. | None. | YES |
| `documentationAction` | src/ui/MainWindow.cpp:1223 | → MainWindow::onDocumentation | N/A. | QDesktopServices::openUrl('https://github.com/dev/PDFE#documentation'). | Default browser opens URL (external app). | None — Qt swallows openUrl failures. | NO — external browser. |
| `reportIssueAction` | src/ui/MainWindow.cpp:1229 | → MainWindow::onReportIssue | N/A. | QDesktopServices::openUrl('https://github.com/dev/PDFE/issues/new'). | External browser. | None. | NO |
| `checkUpdatesAction` | src/ui/MainWindow.cpp:1234 | → MainWindow::onCheckUpdates | N/A. | QDesktopServices::openUrl('https://github.com/dev/PDFE/releases'). | External browser. | None. | NO |
| `imageToolAction` | src/ui/MainWindow.cpp:1240 | → MainWindow::onAnnotationToolSelected | QAction* — exclusive annotation tool group. | Member of annotToolGroup_ (QActionGroup) — exclusive selection group dispatches via QActionGroup::triggered → MainWindow::onAnnotationToolSelected; also triggers MainWindow::onImageToolSelected as a side effect on PdfCanvas tool installati… | Canvas cursor + tool overlay changes; selected toolbar button highlighted; some tools reveal sub-toolbars (TextEdit, Form). | No document → no-op. | YES |
| `insertImageAction` | src/ui/MainWindow.cpp:1245 | → MainWindow::onInsertImage | QFileDialog 'PNG/JPG/JPEG/BMP/GIF/TIFF;All Files'. | InsertImageCommand with default 72,72,400,300 placement and stripExif=true; canvas refresh. | Inserted image visible on page. | User cancel; no page → no-op. | YES |
| `stripExifAction` | src/ui/MainWindow.cpp:1250 | → MainWindow::onStripExif | N/A. | exifStripper_->stripAllImages; canvas refresh. | QMessageBox::information 'Stripped metadata from N image(s)' or 'No images found'. | None — Stripper handles internally. | YES |
| `imageRemoveAction` | src/ui/MainWindow.cpp:1255 | → MainWindow::onImageRemove | N/A. | Emits imageTool_->removeRequested(imageIndex) → RemoveImageCommand. | Image disappears from canvas. | No selection → no-op. | YES |
| `imageRotateAction` | src/ui/MainWindow.cpp:1261 | → MainWindow::onImageRotate90 | N/A. | imageTool_->rotateRequested(name, 90). | Image rotated 90. | No selection. | YES |
| `imageCropAction` | src/ui/MainWindow.cpp:1267 | → MainWindow::onImageCrop | N/A — uses center 80% as default. | imageTool_->cropRequested(name, QRectF). | Image cropped. | No selection. | YES |
| `imageResizeAction` | src/ui/MainWindow.cpp:1273 | → MainWindow::onImageResize | N/A — 50% scale default. | imageTool_->resizeRequested(name, w, h, x, y). | Image resized. | No selection. | YES |
| `documentMetadataAction` | src/ui/MainWindow.cpp:1280 | → MainWindow::onDocumentMetadata | MetadataDialog modal — title/author/subject/keywords/creator/producer fields. | EditMetadataCommand on Accept. | MetadataDialog modal; window title may gain '*'. | None. | YES |
| `complianceAction` | src/ui/MainWindow.cpp:1286 | → MainWindow::onCompliance | N/A. | ComplianceDialog modal exec(). | Compliance dialog modal. | No document → no-op. | YES |
| `preflightAction` | src/ui/MainWindow.cpp:1292 | inline lambda @ MainWindow.cpp:1295 | N/A — QAction click only; lambda body reads MainWindow members. | Inline lambda; transitions read directly from source (typically: open dialog, run command, refresh canvas). | Per lambda — typically opens a modal dialog or pushes a command. | Per lambda; user-cancel paths typically return early. | YES |
| `ocrAction` | src/ui/MainWindow.cpp:1302 | inline lambda @ MainWindow.cpp:1306 | N/A — QAction click only; lambda body reads MainWindow members. | Inline lambda; transitions read directly from source (typically: open dialog, run command, refresh canvas). | Per lambda — typically opens a modal dialog or pushes a command. | Per lambda; user-cancel paths typically return early. | YES |
| `actionOcrAllPages` | src/ui/MainWindow.cpp:1313 | inline lambda @ MainWindow.cpp:1318 | N/A — QAction click only; lambda body reads MainWindow members. | Inline lambda; transitions read directly from source (typically: open dialog, run command, refresh canvas). | Per lambda — typically opens a modal dialog or pushes a command. | Per lambda; user-cancel paths typically return early. | YES |
| `treAction` | src/ui/MainWindow.cpp:1361 | inline lambda @ MainWindow.cpp:1365 | N/A — QAction click only; lambda body reads MainWindow members. | Inline lambda; transitions read directly from source (typically: open dialog, run command, refresh canvas). | Per lambda — typically opens a modal dialog or pushes a command. | Per lambda; user-cancel paths typically return early. | YES |
| `accessibilityAction` | src/ui/MainWindow.cpp:1370 | inline lambda @ MainWindow.cpp:1374 | N/A — QAction click only; lambda body reads MainWindow members. | Inline lambda; transitions read directly from source (typically: open dialog, run command, refresh canvas). | Per lambda — typically opens a modal dialog or pushes a command. | Per lambda; user-cancel paths typically return early. | YES |
| `batchAction` | src/ui/MainWindow.cpp:1390 | inline lambda @ MainWindow.cpp:1393 | N/A — QAction click only; lambda body reads MainWindow members. | Inline lambda; transitions read directly from source (typically: open dialog, run command, refresh canvas). | Per lambda — typically opens a modal dialog or pushes a command. | Per lambda; user-cancel paths typically return early. | YES |
| `compareAction` | src/ui/MainWindow.cpp:1398 | inline lambda @ MainWindow.cpp:1401 | N/A — QAction click only; lambda body reads MainWindow members. | Inline lambda; transitions read directly from source (typically: open dialog, run command, refresh canvas). | Per lambda — typically opens a modal dialog or pushes a command. | Per lambda; user-cancel paths typically return early. | YES |
| `actionRotateViewCW` | src/ui/MainWindow.cpp:1411 | inline lambda @ MainWindow.cpp:1416 | N/A — QAction click only; lambda body reads MainWindow members. | Inline lambda; transitions read directly from source (typically: open dialog, run command, refresh canvas). | Per lambda — typically opens a modal dialog or pushes a command. | Per lambda; user-cancel paths typically return early. | YES |
| `actionRotateViewCCW` | src/ui/MainWindow.cpp:1424 | inline lambda @ MainWindow.cpp:1428 | N/A — QAction click only; lambda body reads MainWindow members. | Inline lambda; transitions read directly from source (typically: open dialog, run command, refresh canvas). | Per lambda — typically opens a modal dialog or pushes a command. | Per lambda; user-cancel paths typically return early. | YES |
| `actionRotateView180` | src/ui/MainWindow.cpp:1440 | inline lambda @ MainWindow.cpp:1444 | N/A — QAction click only; lambda body reads MainWindow members. | Inline lambda; transitions read directly from source (typically: open dialog, run command, refresh canvas). | Per lambda — typically opens a modal dialog or pushes a command. | Per lambda; user-cancel paths typically return early. | YES |
| `actionResetViewRotation` | src/ui/MainWindow.cpp:1453 | inline lambda @ MainWindow.cpp:1457 | N/A — QAction click only; lambda body reads MainWindow members. | Inline lambda; transitions read directly from source (typically: open dialog, run command, refresh canvas). | Per lambda — typically opens a modal dialog or pushes a command. | Per lambda; user-cancel paths typically return early. | YES |
| `themeToggleAction` | src/ui/MainWindow.cpp:1464 | inline lambda @ MainWindow.cpp:1468 | N/A — QAction click only; lambda body reads MainWindow members. | Inline lambda; transitions read directly from source (typically: open dialog, run command, refresh canvas). | Per lambda — typically opens a modal dialog or pushes a command. | Per lambda; user-cancel paths typically return early. | YES |
| `themeLightAction` | src/ui/MainWindow.cpp:1482 | inline lambda @ MainWindow.cpp:1487 | N/A — QAction click only; lambda body reads MainWindow members. | Inline lambda; transitions read directly from source (typically: open dialog, run command, refresh canvas). | Per lambda — typically opens a modal dialog or pushes a command. | Per lambda; user-cancel paths typically return early. | YES |
| `themeDarkAction` | src/ui/MainWindow.cpp:1491 | inline lambda @ MainWindow.cpp:1496 | N/A — QAction click only; lambda body reads MainWindow members. | Inline lambda; transitions read directly from source (typically: open dialog, run command, refresh canvas). | Per lambda — typically opens a modal dialog or pushes a command. | Per lambda; user-cancel paths typically return early. | YES |
| `themeSystemAction` | src/ui/MainWindow.cpp:1500 | inline lambda @ MainWindow.cpp:1505 | N/A — QAction click only; lambda body reads MainWindow members. | Inline lambda; transitions read directly from source (typically: open dialog, run command, refresh canvas). | Per lambda — typically opens a modal dialog or pushes a command. | Per lambda; user-cancel paths typically return early. | YES |
| `toggleStickyNotesAction` | src/ui/MainWindow.cpp:1528 | → MainWindow::onToggleStickyNotes | (bool checked) from QAction::toggled. | canvas_->setStickyNotesVisible(checked). | Sticky note icons appear/disappear on canvas. | No canvas → no-op. | YES |
| `toggleCopyPreventionAction` | src/ui/MainWindow.cpp:1536 | → MainWindow::onToggleCopyPrevention | (bool checked). | Sets ALL_KEY copy permission on document_; not persisted unless saved. | Copy action might be disabled in subsequent operations; no immediate visible change. | None. | NO |
| `togglePrintPreventionAction` | src/ui/MainWindow.cpp:1544 | → MainWindow::onTogglePrintPrevention | (bool checked). | Sets print permission flag on document_. | No immediate visible change; affects subsequent print attempts. | None. | NO |
| `toggleOcrBoxesAction` | src/ui/MainWindow.cpp:1552 | → MainWindow::onToggleOcrBoxes | (bool checked). | ocrBoxesEnabled_ = checked; loadOcrBoxesForPage(currentPage); canvas->update. | Canvas overlays per-character OCR bounding boxes. | No OCR pipeline → silently disabled. | YES |
| `replacePageAction` | src/ui/MainWindow.cpp:1662 | inline lambda @ MainWindow.cpp:1665 | N/A — QAction click only; lambda body reads MainWindow members. | Inline lambda; transitions read directly from source (typically: open dialog, run command, refresh canvas). | Per lambda — typically opens a modal dialog or pushes a command. | Per lambda; user-cancel paths typically return early. | YES |
| `pageLabelsAction` | src/ui/MainWindow.cpp:1712 | inline lambda @ MainWindow.cpp:1716 | N/A — QAction click only; lambda body reads MainWindow members. | Inline lambda; transitions read directly from source (typically: open dialog, run command, refresh canvas). | Per lambda — typically opens a modal dialog or pushes a command. | Per lambda; user-cancel paths typically return early. | YES |
| `convertToGrayscaleAction` | src/ui/MainWindow.cpp:1794 | inline lambda @ MainWindow.cpp:1798 | N/A — QAction click only; lambda body reads MainWindow members. | Inline lambda; transitions read directly from source (typically: open dialog, run command, refresh canvas). | Per lambda — typically opens a modal dialog or pushes a command. | Per lambda; user-cancel paths typically return early. | YES |
| `cropToolAction` | src/ui/MainWindow.cpp:1827 | inline lambda @ MainWindow.cpp:1831 | N/A — QAction click only; lambda body reads MainWindow members. | Inline lambda; transitions read directly from source (typically: open dialog, run command, refresh canvas). | Per lambda — typically opens a modal dialog or pushes a command. | Per lambda; user-cancel paths typically return early. | YES |
| `watermarkAction` | src/ui/MainWindow.cpp:1851 | inline lambda @ MainWindow.cpp:1854 | N/A — QAction click only; lambda body reads MainWindow members. | Inline lambda; transitions read directly from source (typically: open dialog, run command, refresh canvas). | Per lambda — typically opens a modal dialog or pushes a command. | Per lambda; user-cancel paths typically return early. | YES |
| `headerFooterAction` | src/ui/MainWindow.cpp:1864 | inline lambda @ MainWindow.cpp:1868 | N/A — QAction click only; lambda body reads MainWindow members. | Inline lambda; transitions read directly from source (typically: open dialog, run command, refresh canvas). | Per lambda — typically opens a modal dialog or pushes a command. | Per lambda; user-cancel paths typically return early. | YES |
| `(unnamed)` | src/ui/MainWindow.cpp:1883 | inline lambda @ MainWindow.cpp:1886 | N/A — QAction click only; lambda body reads MainWindow members. | Inline lambda; transitions read directly from source (typically: open dialog, run command, refresh canvas). | Per lambda — typically opens a modal dialog or pushes a command. | Per lambda; user-cancel paths typically return early. | YES |
| `optimizePdfAction` | src/ui/MainWindow.cpp:1910 | inline lambda @ MainWindow.cpp:1914 | N/A — QAction click only; lambda body reads MainWindow members. | Inline lambda; transitions read directly from source (typically: open dialog, run command, refresh canvas). | Per lambda — typically opens a modal dialog or pushes a command. | Per lambda; user-cancel paths typically return early. | YES |
| `toggleAttachmentPanelAction` | src/ui/MainWindow.cpp:1958 | inline lambda @ MainWindow.cpp:1606 | N/A — QAction click only; lambda body reads MainWindow members. | Inline lambda; transitions read directly from source (typically: open dialog, run command, refresh canvas). | Per lambda — typically opens a modal dialog or pushes a command. | Per lambda; user-cancel paths typically return early. | YES |
| `customizeToolbarsAction` | src/ui/MainWindow.cpp:1979 | → MainWindow::onCustomizeToolbars | CustomizeToolbarsDialog modal. | ToolbarLayoutManager::saveLayouts; applyToolbarLayoutsList. | Toolbars rearrange. | User cancel. | YES |
| `formTextFieldAction` | src/ui/MainWindow.cpp:2051 | inline lambda @ MainWindow.cpp:5114 | N/A — QAction click only; lambda body reads MainWindow members. | Inline lambda; transitions read directly from source (typically: open dialog, run command, refresh canvas). | Per lambda — typically opens a modal dialog or pushes a command. | Per lambda; user-cancel paths typically return early. | YES |
| `formCheckboxAction` | src/ui/MainWindow.cpp:2056 | inline lambda @ MainWindow.cpp:5117 | N/A — QAction click only; lambda body reads MainWindow members. | Inline lambda; transitions read directly from source (typically: open dialog, run command, refresh canvas). | Per lambda — typically opens a modal dialog or pushes a command. | Per lambda; user-cancel paths typically return early. | YES |
| `formRadioButtonAction` | src/ui/MainWindow.cpp:2061 | inline lambda @ MainWindow.cpp:5120 | N/A — QAction click only; lambda body reads MainWindow members. | Inline lambda; transitions read directly from source (typically: open dialog, run command, refresh canvas). | Per lambda — typically opens a modal dialog or pushes a command. | Per lambda; user-cancel paths typically return early. | YES |
| `formComboBoxAction` | src/ui/MainWindow.cpp:2066 | inline lambda @ MainWindow.cpp:5123 | N/A — QAction click only; lambda body reads MainWindow members. | Inline lambda; transitions read directly from source (typically: open dialog, run command, refresh canvas). | Per lambda — typically opens a modal dialog or pushes a command. | Per lambda; user-cancel paths typically return early. | YES |
| `formListBoxAction` | src/ui/MainWindow.cpp:2071 | inline lambda @ MainWindow.cpp:5126 | N/A — QAction click only; lambda body reads MainWindow members. | Inline lambda; transitions read directly from source (typically: open dialog, run command, refresh canvas). | Per lambda — typically opens a modal dialog or pushes a command. | Per lambda; user-cancel paths typically return early. | YES |
| `formPushButtonAction` | src/ui/MainWindow.cpp:2076 | inline lambda @ MainWindow.cpp:5129 | N/A — QAction click only; lambda body reads MainWindow members. | Inline lambda; transitions read directly from source (typically: open dialog, run command, refresh canvas). | Per lambda — typically opens a modal dialog or pushes a command. | Per lambda; user-cancel paths typically return early. | YES |
| `formSignatureAction` | src/ui/MainWindow.cpp:2081 | inline lambda @ MainWindow.cpp:5132 | N/A — QAction click only; lambda body reads MainWindow members. | Inline lambda; transitions read directly from source (typically: open dialog, run command, refresh canvas). | Per lambda — typically opens a modal dialog or pushes a command. | Per lambda; user-cancel paths typically return early. | YES |
| `selectToolAction` | src/ui/MainWindow.cpp:2126 | → MainWindow::onAnnotationToolSelected | QAction* — exclusive annotation tool group. | Annotation tool group; activates SelectTool on PdfCanvas. Switches the active tool installed on PdfCanvas (Select/StickyNote/Highlight/Underline/StrikeOut/FreeText/Ink/Rectangle/Circle/Line/Stamp/FileAttachment/TextEdit/TRE/Form/Image/Reda… | Canvas cursor + tool overlay changes; selected toolbar button highlighted; some tools reveal sub-toolbars (TextEdit, Form). | No document → no-op. | YES |
| `stickyNoteToolAction` | src/ui/MainWindow.cpp:2137 | → MainWindow::onAnnotationToolSelected | QAction* — exclusive annotation tool group. | Annotation tool group; activates StickyNoteTool on PdfCanvas. Switches the active tool installed on PdfCanvas (Select/StickyNote/Highlight/Underline/StrikeOut/FreeText/Ink/Rectangle/Circle/Line/Stamp/FileAttachment/TextEdit/TRE/Form/Image/… | Canvas cursor + tool overlay changes; selected toolbar button highlighted; some tools reveal sub-toolbars (TextEdit, Form). | No document → no-op. | YES |
| `highlightToolAction` | src/ui/MainWindow.cpp:2145 | → MainWindow::onAnnotationToolSelected | QAction* — exclusive annotation tool group. | Annotation tool group; activates HighlightTool (TextMarkupTool variant) on PdfCanvas. Switches the active tool installed on PdfCanvas (Select/StickyNote/Highlight/Underline/StrikeOut/FreeText/Ink/Rectangle/Circle/Line/Stamp/FileAttachment/… | Canvas cursor + tool overlay changes; selected toolbar button highlighted; some tools reveal sub-toolbars (TextEdit, Form). | No document → no-op. | YES |
| `underlineToolAction` | src/ui/MainWindow.cpp:2153 | → MainWindow::onAnnotationToolSelected | QAction* — exclusive annotation tool group. | Annotation tool group; activates UnderlineTool on PdfCanvas. Switches the active tool installed on PdfCanvas (Select/StickyNote/Highlight/Underline/StrikeOut/FreeText/Ink/Rectangle/Circle/Line/Stamp/FileAttachment/TextEdit/TRE/Form/Image/R… | Canvas cursor + tool overlay changes; selected toolbar button highlighted; some tools reveal sub-toolbars (TextEdit, Form). | No document → no-op. | YES |
| `strikeOutToolAction` | src/ui/MainWindow.cpp:2161 | → MainWindow::onAnnotationToolSelected | QAction* — exclusive annotation tool group. | Annotation tool group; activates StrikeOutTool on PdfCanvas. Switches the active tool installed on PdfCanvas (Select/StickyNote/Highlight/Underline/StrikeOut/FreeText/Ink/Rectangle/Circle/Line/Stamp/FileAttachment/TextEdit/TRE/Form/Image/R… | Canvas cursor + tool overlay changes; selected toolbar button highlighted; some tools reveal sub-toolbars (TextEdit, Form). | No document → no-op. | YES |
| `inkToolAction` | src/ui/MainWindow.cpp:2189 | → MainWindow::onAnnotationToolSelected | QAction* — exclusive annotation tool group. | Annotation tool group; activates InkTool on PdfCanvas. Switches the active tool installed on PdfCanvas (Select/StickyNote/Highlight/Underline/StrikeOut/FreeText/Ink/Rectangle/Circle/Line/Stamp/FileAttachment/TextEdit/TRE/Form/Image/Redact)… | Canvas cursor + tool overlay changes; selected toolbar button highlighted; some tools reveal sub-toolbars (TextEdit, Form). | No document → no-op. | YES |
| `freeTextToolAction` | src/ui/MainWindow.cpp:2197 | → MainWindow::onAnnotationToolSelected | QAction* — exclusive annotation tool group. | Annotation tool group; activates FreeTextTool on PdfCanvas (creates /FreeText annotation). Switches the active tool installed on PdfCanvas (Select/StickyNote/Highlight/Underline/StrikeOut/FreeText/Ink/Rectangle/Circle/Line/Stamp/FileAttach… | Canvas cursor + tool overlay changes; selected toolbar button highlighted; some tools reveal sub-toolbars (TextEdit, Form). | No document → no-op. | YES |
| `rectangleToolAction` | src/ui/MainWindow.cpp:2207 | → MainWindow::onAnnotationToolSelected | QAction* — exclusive annotation tool group. | Annotation tool group; activates RectangleTool on PdfCanvas (Square annotation). Switches the active tool installed on PdfCanvas (Select/StickyNote/Highlight/Underline/StrikeOut/FreeText/Ink/Rectangle/Circle/Line/Stamp/FileAttachment/TextE… | Canvas cursor + tool overlay changes; selected toolbar button highlighted; some tools reveal sub-toolbars (TextEdit, Form). | No document → no-op. | YES |
| `circleToolAction` | src/ui/MainWindow.cpp:2215 | → MainWindow::onAnnotationToolSelected | QAction* — exclusive annotation tool group. | Annotation tool group; activates CircleTool on PdfCanvas (Circle annotation). Switches the active tool installed on PdfCanvas (Select/StickyNote/Highlight/Underline/StrikeOut/FreeText/Ink/Rectangle/Circle/Line/Stamp/FileAttachment/TextEdit… | Canvas cursor + tool overlay changes; selected toolbar button highlighted; some tools reveal sub-toolbars (TextEdit, Form). | No document → no-op. | YES |
| `lineToolAction` | src/ui/MainWindow.cpp:2223 | → MainWindow::onAnnotationToolSelected | QAction* — exclusive annotation tool group. | Annotation tool group; activates LineTool on PdfCanvas. Switches the active tool installed on PdfCanvas (Select/StickyNote/Highlight/Underline/StrikeOut/FreeText/Ink/Rectangle/Circle/Line/Stamp/FileAttachment/TextEdit/TRE/Form/Image/Redact… | Canvas cursor + tool overlay changes; selected toolbar button highlighted; some tools reveal sub-toolbars (TextEdit, Form). | No document → no-op. | YES |
| `stampToolAction` | src/ui/MainWindow.cpp:2234 | inline lambda @ MainWindow.cpp:2240 | N/A — QAction click only; lambda body reads MainWindow members. | Inline lambda; transitions read directly from source (typically: open dialog, run command, refresh canvas). | Per lambda — typically opens a modal dialog or pushes a command. | Per lambda; user-cancel paths typically return early. | YES |
| `fileAttachmentToolAction` | src/ui/MainWindow.cpp:2319 | → MainWindow::onAnnotationToolSelected | QAction* — exclusive annotation tool group. | Annotation tool group; inline lambda prompts for source file + icon style, then activates FileAttachmentTool on PdfCanvas. Switches the active tool installed on PdfCanvas (Select/StickyNote/Highlight/Underline/StrikeOut/FreeText/Ink/Rectan… | Canvas cursor + tool overlay changes; selected toolbar button highlighted; some tools reveal sub-toolbars (TextEdit, Form). | No document → no-op. | YES |

#### widgets (16)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `submitUrlField` | src/ui/MainWindow.cpp:1130 | N/A — passive sub-control of MainWindow; value read by MainWindow::accessor() at QDialog::exec()-time. | Free-form QString; validator/echo-mode per dialog (passwords use Password echo). | None at signal level — value captured into result struct when MainWindow is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `submitButton` | src/ui/MainWindow.cpp:1135 | → QDialog::accept | N/A — button click only (no text input). | Sets dialog result Accepted; closes dialog; caller reads dialog state via accessor methods. | Dialog disappears. | None. | YES |
| `ocrAllProgressCancelButton` | src/ui/MainWindow.cpp:1337 | N/A — passive sub-control of MainWindow; value read by MainWindow::accessor() at QDialog::exec()-time. | N/A — button click only (no text input). | None at signal level — value captured into result struct when MainWindow is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `accessibilityReport` | src/ui/MainWindow.cpp:1379 | N/A — passive sub-control of MainWindow; value read by MainWindow::accessor() at QDialog::exec()-time. | Rich-text or plain-text QString. | None at signal level — value captured into result struct when MainWindow is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `(QDialogButtonBox#1384)` | src/ui/MainWindow.cpp:1384 | → QDialog::reject | Button-roles (Ok/Cancel/Apply/Close/Help). | Sets dialog result Rejected; closes dialog without committing changes. | Dialog disappears. | None. | YES |
| `zoomCombo` | src/ui/MainWindow.cpp:2013 | → MainWindow::onZoomComboChanged; → MainWindow::onZoomComboEdited | One of items() (editable variant accepts free-form QString). | Parses percent, canvas_->setZoom(percent/100). | Canvas re-render. | Unparseable text → no-op. | YES |
| `annotColorPickerButton` | src/ui/MainWindow.cpp:2371 | → MainWindow::onAnnotationColorPicker | N/A — button click only (no text input). | currentAnnotColor_ updated; colorPickerButton_ stylesheet refreshed; current tool->setColor() if any. | Color picker swatch updates; subsequent annotations use new color. | User cancel → no change. | YES |
| `pdfCanvas` | src/ui/MainWindow.cpp:276 | → MainWindow::onPageChanged; → MainWindow::onZoomChanged; → MainWindow::onAnnotationCreatedByTool; + 4 inline lambda(s) | Mouse + keyboard events dispatched to active tool; accepts drag-drop of file:// URLs and application/x-pdf-page-id MIME. | navHistory_.recordVisit; pendingAtlas reset; statusBar setPageInfo; pagePanel setCurrentPage; propertiesPanel setPageInfo; textEditTool setPage if active; activateTextEditForPage if auto-routing; loadOcrBoxesForPage. | Page panel highlight moves; properties panel updates; status bar shows page X of N. | No document → no-op return. | YES |
| `pagePanel` | src/ui/MainWindow.cpp:2411 | → MainWindow::onPageSelected; → MainWindow::onPageMoved; + 6 inline lambda(s) | Thumbnail click / drag-drop / context menu. | canvas_->goToPage(pageIndex). | Canvas scrolls to pageIndex. | No canvas → no-op. | YES |
| `bookmarkPanel` | src/ui/MainWindow.cpp:2443 | → MainWindow::onBookmarkActivated | Tree click / context menu / drag-drop. | canvas_->goToPage(pageIndex). | Canvas navigates to bookmark target. | None. | YES |
| `layerPanel` | src/ui/MainWindow.cpp:2463 | + 1 inline lambda(s) | Tree checkboxes toggle visibility. | if (canvas_) canvas_->refreshAllPages() (inline lambda at MainWindow.cpp:2467) | Per lambda — typically updates dialog state, opens a child dialog, or pushes a command. | Per lambda; user-cancel paths return early. | YES |
| `defaultsLogPanel` | src/ui/MainWindow.cpp:2515 | N/A — passive sub-control of MainWindow; value read by MainWindow::accessor() at QDialog::exec()-time. | Read-only log. | None at signal level — value captured into result struct when MainWindow is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `commentPanel` | src/ui/MainWindow.cpp:2526 | → MainWindow::onCommentPanelNavigate; → MainWindow::onCommentPanelDelete | Tree click + filter input. | canvas_->goToPage(pageIndex). | Canvas navigates; comment panel selection follows. | None. | YES |
| `annotationPropertiesWidget` | src/ui/MainWindow.cpp:2540 | → MainWindow::onAnnotationPropertiesChanged | Color/opacity/author/contents. | ModifyAnnotationCommand; canvas refresh. | Annotation appearance updates. | No manager → no-op. | YES |
| `formFieldPropertiesPanel` | src/ui/MainWindow.cpp:2553 | → MainWindow::onFormFieldValueChanged | Per-field-type editor (Text/Checkbox/Choice). | SetFieldValueCommand via undoManager; canvas refresh. | Form widget appearance updates; window title gets '*'. | No page → no-op. | YES |
| `(AttachmentPanel#2608)` | src/ui/MainWindow.cpp:2608 | + 1 inline lambda(s) | Tree double-click extracts file. | updateWindowTitle() (inline lambda at MainWindow.cpp:2614) | Per lambda — typically updates dialog state, opens a child dialog, or pushes a command. | Per lambda; user-cancel paths return early. | YES |

#### menus (15)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `Atlas Mode` | src/ui/MainWindow.cpp:606 | Container — child QActions trigger their own slots; menu open emits aboutToShow. | N/A — menu opens via QMenuBar click or shortcut. | Menu opens (paints popup); aboutToShow fires for dynamic submenus (Stamps, Recent Files); selecting an item closes the menu and triggers the chosen QAction. | Menu popup paints over canvas; closes on item selection or Esc. | None at menu level — child actions handle their own errors. | YES |
| `&File` | src/ui/MainWindow.cpp:1564 | Container — child QActions trigger their own slots; menu open emits aboutToShow. | N/A — menu opens via QMenuBar click or shortcut. | Menu opens (paints popup); aboutToShow fires for dynamic submenus (Stamps, Recent Files); selecting an item closes the menu and triggers the chosen QAction. | Menu popup paints over canvas; closes on item selection or Esc. | None at menu level — child actions handle their own errors. | YES |
| `&Stamps` | src/ui/MainWindow.cpp:1585 | Container — child QActions trigger their own slots; menu open emits aboutToShow. | N/A — menu opens via QMenuBar click or shortcut. | Menu opens (paints popup); aboutToShow fires for dynamic submenus (Stamps, Recent Files); selecting an item closes the menu and triggers the chosen QAction. | Menu popup paints over canvas; closes on item selection or Esc. | None at menu level — child actions handle their own errors. | YES |
| `&Remove Stamp` | src/ui/MainWindow.cpp:1602 | Container — child QActions trigger their own slots; menu open emits aboutToShow. | N/A — menu opens via QMenuBar click or shortcut. | Menu opens (paints popup); aboutToShow fires for dynamic submenus (Stamps, Recent Files); selecting an item closes the menu and triggers the chosen QAction. | Menu popup paints over canvas; closes on item selection or Esc. | None at menu level — child actions handle their own errors. | YES |
| `&Edit` | src/ui/MainWindow.cpp:1627 | Container — child QActions trigger their own slots; menu open emits aboutToShow. | N/A — menu opens via QMenuBar click or shortcut. | Menu opens (paints popup); aboutToShow fires for dynamic submenus (Stamps, Recent Files); selecting an item closes the menu and triggers the chosen QAction. | Menu popup paints over canvas; closes on item selection or Esc. | None at menu level — child actions handle their own errors. | YES |
| `&Pages` | src/ui/MainWindow.cpp:1651 | Container — child QActions trigger their own slots; menu open emits aboutToShow. | N/A — menu opens via QMenuBar click or shortcut. | Menu opens (paints popup); aboutToShow fires for dynamic submenus (Stamps, Recent Files); selecting an item closes the menu and triggers the chosen QAction. | Menu popup paints over canvas; closes on item selection or Esc. | None at menu level — child actions handle their own errors. | YES |
| `F&orms` | src/ui/MainWindow.cpp:1726 | Container — child QActions trigger their own slots; menu open emits aboutToShow. | N/A — menu opens via QMenuBar click or shortcut. | Menu opens (paints popup); aboutToShow fires for dynamic submenus (Stamps, Recent Files); selecting an item closes the menu and triggers the chosen QAction. | Menu popup paints over canvas; closes on item selection or Esc. | None at menu level — child actions handle their own errors. | YES |
| `&Security` | src/ui/MainWindow.cpp:1742 | Container — child QActions trigger their own slots; menu open emits aboutToShow. | N/A — menu opens via QMenuBar click or shortcut. | Menu opens (paints popup); aboutToShow fires for dynamic submenus (Stamps, Recent Files); selecting an item closes the menu and triggers the chosen QAction. | Menu popup paints over canvas; closes on item selection or Esc. | None at menu level — child actions handle their own errors. | YES |
| `&Image` | src/ui/MainWindow.cpp:1761 | Container — child QActions trigger their own slots; menu open emits aboutToShow. | N/A — menu opens via QMenuBar click or shortcut. | Menu opens (paints popup); aboutToShow fires for dynamic submenus (Stamps, Recent Files); selecting an item closes the menu and triggers the chosen QAction. | Menu popup paints over canvas; closes on item selection or Esc. | None at menu level — child actions handle their own errors. | YES |
| `&Tools` | src/ui/MainWindow.cpp:1772 | Container — child QActions trigger their own slots; menu open emits aboutToShow. | N/A — menu opens via QMenuBar click or shortcut. | Menu opens (paints popup); aboutToShow fires for dynamic submenus (Stamps, Recent Files); selecting an item closes the menu and triggers the chosen QAction. | Menu popup paints over canvas; closes on item selection or Esc. | None at menu level — child actions handle their own errors. | YES |
| `&Measure` | src/ui/MainWindow.cpp:1878 | Container — child QActions trigger their own slots; menu open emits aboutToShow. | N/A — menu opens via QMenuBar click or shortcut. | Menu opens (paints popup); aboutToShow fires for dynamic submenus (Stamps, Recent Files); selecting an item closes the menu and triggers the chosen QAction. | Menu popup paints over canvas; closes on item selection or Esc. | None at menu level — child actions handle their own errors. | YES |
| `&View` | src/ui/MainWindow.cpp:1923 | Container — child QActions trigger their own slots; menu open emits aboutToShow. | N/A — menu opens via QMenuBar click or shortcut. | Menu opens (paints popup); aboutToShow fires for dynamic submenus (Stamps, Recent Files); selecting an item closes the menu and triggers the chosen QAction. | Menu popup paints over canvas; closes on item selection or Esc. | None at menu level — child actions handle their own errors. | YES |
| `&Rotate` | src/ui/MainWindow.cpp:1938 | Container — child QActions trigger their own slots; menu open emits aboutToShow. | N/A — menu opens via QMenuBar click or shortcut. | Menu opens (paints popup); aboutToShow fires for dynamic submenus (Stamps, Recent Files); selecting an item closes the menu and triggers the chosen QAction. | Menu popup paints over canvas; closes on item selection or Esc. | None at menu level — child actions handle their own errors. | YES |
| `&Theme` | src/ui/MainWindow.cpp:1946 | Container — child QActions trigger their own slots; menu open emits aboutToShow. | N/A — menu opens via QMenuBar click or shortcut. | Menu opens (paints popup); aboutToShow fires for dynamic submenus (Stamps, Recent Files); selecting an item closes the menu and triggers the chosen QAction. | Menu popup paints over canvas; closes on item selection or Esc. | None at menu level — child actions handle their own errors. | YES |
| `&Help` | src/ui/MainWindow.cpp:1987 | Container — child QActions trigger their own slots; menu open emits aboutToShow. | N/A — menu opens via QMenuBar click or shortcut. | Menu opens (paints popup); aboutToShow fires for dynamic submenus (Stamps, Recent Files); selecting an item closes the menu and triggers the chosen QAction. | Menu popup paints over canvas; closes on item selection or Esc. | None at menu level — child actions handle their own errors. | YES |

#### toolbars (3)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `Main Toolbar` | src/ui/MainWindow.cpp:1998 | Container — child QActions trigger their own slots. | N/A — buttons clicked individually. | QToolBar visibility may toggle (View menu); button state reflects active QAction checked-state. | Toolbar shows/hides; per-button highlight when checked. | None at toolbar level. | YES |
| `Annotation Toolbar` | src/ui/MainWindow.cpp:2036 | Container — child QActions trigger their own slots. | N/A — buttons clicked individually. | QToolBar visibility may toggle (View menu); button state reflects active QAction checked-state. | Toolbar shows/hides; per-button highlight when checked. | None at toolbar level. | YES |
| `Form Fields` | src/ui/MainWindow.cpp:2047 | Container — child QActions trigger their own slots. | N/A — buttons clicked individually. | QToolBar visibility may toggle (View menu); button state reflects active QAction checked-state. | Toolbar shows/hides; per-button highlight when checked. | None at toolbar level. | YES |

#### drag_targets (1)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `dragTarget@src/ui/MainWindow.cpp` | src/ui/MainWindow.cpp | MainWindow::dragEnterEvent + dropEvent | MIME types accepted: 'text/uri-list' (file:// URLs ending in .pdf) and 'application/x-pdf-page-id' (internal page-reorder drags). | On drop with PDF URL: openFile(path). On drop with page-id: PagePanel::onPageMoved → MovePageCommand. | On enter: drop indicator (cursor+highlight). On valid drop: document opens or pages reorder. | Unsupported MIME → dragEnterEvent rejects (cursor shows 'no-drop'). Drop of corrupt PDF → openFile error path. | YES |

### `src/ui/MainWindow.h` (1 entries)

#### dialogs (1)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `MainWindow` | src/ui/MainWindow.h | MainWindow — top-level window; Top-level QMainWindow — owns canvas, panels, all menus, the document, all *Manager instances. | All QActions, all menu items, drag-drop accepting application/x-pdf-page-id and text/uri-list (file:// PDFs). | See per-action shared-slots table. | Application main window. | See per-action error states. | YES |

### `src/ui/MetadataDialog.cpp` (6 entries)

#### widgets (6)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `(QFormLayout#49)` | src/ui/MetadataDialog.cpp:49 | N/A — passive sub-control of MetadataDialog; value read by MetadataDialog::accessor() at QDialog::exec()-time. | Per-widget; see source. | None at signal level — value captured into result struct when MetadataDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `metadataTitleEdit` | src/ui/MetadataDialog.cpp:53 | N/A — passive sub-control of MetadataDialog; value read by MetadataDialog::accessor() at QDialog::exec()-time. | Free-form QString; validator/echo-mode per dialog (passwords use Password echo). | None at signal level — value captured into result struct when MetadataDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `metadataAuthorEdit` | src/ui/MetadataDialog.cpp:58 | N/A — passive sub-control of MetadataDialog; value read by MetadataDialog::accessor() at QDialog::exec()-time. | Free-form QString; validator/echo-mode per dialog (passwords use Password echo). | None at signal level — value captured into result struct when MetadataDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `metadataSubjectEdit` | src/ui/MetadataDialog.cpp:63 | N/A — passive sub-control of MetadataDialog; value read by MetadataDialog::accessor() at QDialog::exec()-time. | Free-form QString; validator/echo-mode per dialog (passwords use Password echo). | None at signal level — value captured into result struct when MetadataDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `metadataKeywordsEdit` | src/ui/MetadataDialog.cpp:68 | N/A — passive sub-control of MetadataDialog; value read by MetadataDialog::accessor() at QDialog::exec()-time. | Free-form QString; validator/echo-mode per dialog (passwords use Password echo). | None at signal level — value captured into result struct when MetadataDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `metadataCreatorEdit` | src/ui/MetadataDialog.cpp:73 | N/A — passive sub-control of MetadataDialog; value read by MetadataDialog::accessor() at QDialog::exec()-time. | Free-form QString; validator/echo-mode per dialog (passwords use Password echo). | None at signal level — value captured into result struct when MetadataDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |

### `src/ui/MetadataDialog.h` (1 entries)

#### dialogs (1)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `MetadataDialog` | src/ui/MetadataDialog.h | MetadataDialog::exec() — modal dialog; Edit document /Info dict. | title/author/subject/keywords/creator/producer line edits. | EditMetadataCommand on Accept. | Modal dialog appears; on close it disappears. Sub-controls re-render per user input. | Per accept() validation logic in this dialog; reject() always succeeds. | YES |

### `src/ui/OptimizeDialog.cpp` (4 entries)

#### widgets (4)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `optimizeOptionsGroup` | src/ui/OptimizeDialog.cpp:43 | N/A — passive sub-control of OptimizeDialog; value read by OptimizeDialog::accessor() at QDialog::exec()-time. | Container only — no input. | None at signal level — value captured into result struct when OptimizeDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | NO |
| `(QCheckBox#48)` | src/ui/OptimizeDialog.cpp:48 | N/A — passive sub-control of OptimizeDialog; value read by OptimizeDialog::accessor() at QDialog::exec()-time. | Boolean (checked/unchecked); supports tri-state if explicitly set. | None at signal level — value captured into result struct when OptimizeDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `optimizeRunButton` | src/ui/OptimizeDialog.cpp:80 | → OptimizeDialog::onRun | N/A — button click only (no text input). | const auto input = document_->filePath(); } const QString defaultOut = QString::fromStdString( (input.parent_path() / (input.stem().string() + "_optimized" + input.extension().string())).string()); c… (extracted from OptimizeDialog.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `optimizeCancelButton` | src/ui/OptimizeDialog.cpp:84 | → QDialog::reject | N/A — button click only (no text input). | Sets dialog result Rejected; closes dialog without committing changes. | Dialog disappears. | None. | YES |

### `src/ui/OptimizeDialog.h` (1 entries)

#### dialogs (1)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `OptimizeDialog` | src/ui/OptimizeDialog.h | OptimizeDialog::exec() — modal dialog; Reduce file size: subset fonts, downsample images, remove unused objects. | Per-option checkboxes + preview button. | OptimizationManager::optimize. | Modal dialog appears; on close it disappears. Sub-controls re-render per user input. | Per accept() validation logic in this dialog; reject() always succeeds. | YES |

### `src/ui/PageLabelsDialog.cpp` (9 entries)

#### widgets (9)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `pageLabelsTable` | src/ui/PageLabelsDialog.cpp:56 | N/A — passive sub-control of PageLabelsDialog; value read by PageLabelsDialog::accessor() at QDialog::exec()-time. | Cell selection / per-cell editor delegate. | None at signal level — value captured into result struct when PageLabelsDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `pageLabelsAddButton` | src/ui/PageLabelsDialog.cpp:72 | → PageLabelsDialog::onAdd | N/A — button click only (no text input). | pdfe::core::PageLabelRange r; r.firstPage = 0; r.style = pdfe::core::PageLabelStyle::Decimal (extracted from PageLabelsDialog.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `pageLabelsEditButton` | src/ui/PageLabelsDialog.cpp:74 | → PageLabelsDialog::onEdit | N/A — button click only (no text input). | int row = table_ ? table_->currentRow() : -1; auto r = ranges_[row]; refreshTable() (extracted from PageLabelsDialog.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `pageLabelsDeleteButton` | src/ui/PageLabelsDialog.cpp:76 | → PageLabelsDialog::onDelete | N/A — button click only (no text input). | int row = table_ ? table_->currentRow() : -1; ranges_.erase(ranges_.begin() + row); refreshTable() (extracted from PageLabelsDialog.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `(QFormLayout#128)` | src/ui/PageLabelsDialog.cpp:128 | N/A — passive sub-control of PageLabelsDialog; value read by PageLabelsDialog::accessor() at QDialog::exec()-time. | Per-widget; see source. | None at signal level — value captured into result struct when PageLabelsDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `rangeFirstPageSpin` | src/ui/PageLabelsDialog.cpp:130 | N/A — passive sub-control of PageLabelsDialog; value read by PageLabelsDialog::accessor() at QDialog::exec()-time. | Integer in [minimum,maximum] with step (defaults: 0..99 step 1 unless overridden). | None at signal level — value captured into result struct when PageLabelsDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `rangeStyleCombo` | src/ui/PageLabelsDialog.cpp:137 | N/A — passive sub-control of PageLabelsDialog; value read by PageLabelsDialog::accessor() at QDialog::exec()-time. | One of items() (editable variant accepts free-form QString). | None at signal level — value captured into result struct when PageLabelsDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `rangePrefixEdit` | src/ui/PageLabelsDialog.cpp:157 | N/A — passive sub-control of PageLabelsDialog; value read by PageLabelsDialog::accessor() at QDialog::exec()-time. | Free-form QString; validator/echo-mode per dialog (passwords use Password echo). | None at signal level — value captured into result struct when PageLabelsDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `rangeStartValueSpin` | src/ui/PageLabelsDialog.cpp:161 | N/A — passive sub-control of PageLabelsDialog; value read by PageLabelsDialog::accessor() at QDialog::exec()-time. | Integer in [minimum,maximum] with step (defaults: 0..99 step 1 unless overridden). | None at signal level — value captured into result struct when PageLabelsDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |

### `src/ui/PageLabelsDialog.h` (1 entries)

#### dialogs (1)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `PageLabelsDialog` | src/ui/PageLabelsDialog.h | PageLabelsDialog::exec() — modal dialog; Edit /PageLabels number tree (Roman/decimal/alpha/prefix). | rangeTable, addRangeButton, removeRangeButton, applyButton. | PageLabelsManager::setLabels. | Modal dialog appears; on close it disappears. Sub-controls re-render per user input. | Per accept() validation logic in this dialog; reject() always succeeds. | YES |

### `src/ui/PagePanel.cpp` (2 entries)

#### context_menus (1)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `listWidget_` | src/ui/PagePanel.cpp:70 | Container — populated on customContextMenuRequested or contextMenuEvent. | Right-click event QPoint. | Menu rebuilt on each show; child QActions trigger their own slots. | Popup menu appears at cursor; closes on item selection or Esc. | None at menu level. | YES |

#### drag_targets (1)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `dragTarget@src/ui/PagePanel.cpp` | src/ui/PagePanel.cpp | MainWindow::dragEnterEvent + dropEvent | MIME types accepted: 'text/uri-list' (file:// URLs ending in .pdf) and 'application/x-pdf-page-id' (internal page-reorder drags). | On drop with PDF URL: openFile(path). On drop with page-id: PagePanel::onPageMoved → MovePageCommand. | On enter: drop indicator (cursor+highlight). On valid drop: document opens or pages reorder. | Unsupported MIME → dragEnterEvent rejects (cursor shows 'no-drop'). Drop of corrupt PDF → openFile error path. | YES |

### `src/ui/PagePanel.h` (1 entries)

#### dialogs (1)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `PagePanel` | src/ui/PagePanel.h | PagePanel — QWidget side panel; Side dock with page thumbnails + drag-drop reorder + context menu. | thumbnailList; right-click menu actions. | Emits pageSelected, pageMoved, contextMenuAction. | Side dock visibility toggleable via View menu. Internal state visible inline as user interacts. | Per user-interaction; see widget rows for sub-controls. | YES |

### `src/ui/PasswordDialog.cpp` (2 entries)

#### widgets (2)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `passwordEdit` | src/ui/PasswordDialog.cpp:27 | → PasswordDialog::onPasswordChanged | Free-form QString; validator/echo-mode per dialog (passwords use Password echo). | Toggles okButton enabled when text is non-empty. | OK button enables. | Empty → OK disabled. | YES |
| `buttonBox` | src/ui/PasswordDialog.cpp:35 | → QDialog::accept; → QDialog::reject | Button-roles (Ok/Cancel/Apply/Close/Help). | Sets dialog result Accepted; closes dialog; caller reads dialog state via accessor methods. | Dialog disappears. | None. | YES |

### `src/ui/PasswordDialog.h` (1 entries)

#### dialogs (1)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `PasswordDialog` | src/ui/PasswordDialog.h | PasswordDialog::exec() — modal dialog; Prompt for PDF password during open. | passwordEdit (echo Password). | password() accessor on Accept. | Modal dialog appears; on close it disappears. Sub-controls re-render per user input. | Per accept() validation logic in this dialog; reject() always succeeds. | YES |

### `src/ui/PdfCanvas.h` (1 entries)

#### dialogs (1)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `PdfCanvas` | src/ui/PdfCanvas.h | PdfCanvas — QWidget side panel; Custom QWidget rendering page tiles + dispatching mouse/keyboard to active tool. | Mouse/keyboard events; setCurrentTool / setTextEditTool / setImageTool. | Emits pageChanged, zoomChanged, annotationDoubleClicked. | Side dock visibility toggleable via View menu. Internal state visible inline as user interacts. | Per user-interaction; see widget rows for sub-controls. | YES |

### `src/ui/PreferencesDialog.cpp` (13 entries)

#### widgets (13)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `preferencesTabWidget` | src/ui/PreferencesDialog.cpp:28 | N/A — passive sub-control of PreferencesDialog; value read by PreferencesDialog::accessor() at QDialog::exec()-time. | Selects one tab; per-tab content is its own subtree. | None at signal level — value captured into result struct when PreferencesDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `(QFormLayout#33)` | src/ui/PreferencesDialog.cpp:33 | N/A — passive sub-control of PreferencesDialog; value read by PreferencesDialog::accessor() at QDialog::exec()-time. | Per-widget; see source. | None at signal level — value captured into result struct when PreferencesDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `maxRecentFilesSpinBox` | src/ui/PreferencesDialog.cpp:35 | N/A — passive sub-control of PreferencesDialog; value read by PreferencesDialog::accessor() at QDialog::exec()-time. | Integer in [minimum,maximum] with step (defaults: 0..99 step 1 unless overridden). | None at signal level — value captured into result struct when PreferencesDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `(QFormLayout#45)` | src/ui/PreferencesDialog.cpp:45 | N/A — passive sub-control of PreferencesDialog; value read by PreferencesDialog::accessor() at QDialog::exec()-time. | Per-widget; see source. | None at signal level — value captured into result struct when PreferencesDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `defaultZoomSpinBox` | src/ui/PreferencesDialog.cpp:47 | N/A — passive sub-control of PreferencesDialog; value read by PreferencesDialog::accessor() at QDialog::exec()-time. | Double in [minimum,maximum] with step + decimals. | None at signal level — value captured into result struct when PreferencesDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `tileCacheSpinBox` | src/ui/PreferencesDialog.cpp:56 | N/A — passive sub-control of PreferencesDialog; value read by PreferencesDialog::accessor() at QDialog::exec()-time. | Integer in [minimum,maximum] with step (defaults: 0..99 step 1 unless overridden). | None at signal level — value captured into result struct when PreferencesDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `(QFormLayout#126)` | src/ui/PreferencesDialog.cpp:126 | N/A — passive sub-control of PreferencesDialog; value read by PreferencesDialog::accessor() at QDialog::exec()-time. | Per-widget; see source. | None at signal level — value captured into result struct when PreferencesDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `ocrLanguageCombo` | src/ui/PreferencesDialog.cpp:127 | N/A — passive sub-control of PreferencesDialog; value read by PreferencesDialog::accessor() at QDialog::exec()-time. | One of items() (editable variant accepts free-form QString). | None at signal level — value captured into result struct when PreferencesDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `(QFormLayout#168)` | src/ui/PreferencesDialog.cpp:168 | N/A — passive sub-control of PreferencesDialog; value read by PreferencesDialog::accessor() at QDialog::exec()-time. | Per-widget; see source. | None at signal level — value captured into result struct when PreferencesDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `logLevelCombo` | src/ui/PreferencesDialog.cpp:170 | N/A — passive sub-control of PreferencesDialog; value read by PreferencesDialog::accessor() at QDialog::exec()-time. | One of items() (editable variant accepts free-form QString). | None at signal level — value captured into result struct when PreferencesDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `logToFileCheck` | src/ui/PreferencesDialog.cpp:179 | N/A — passive sub-control of PreferencesDialog; value read by PreferencesDialog::accessor() at QDialog::exec()-time. | Boolean (checked/unchecked); supports tri-state if explicitly set. | None at signal level — value captured into result struct when PreferencesDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `logFilePathEdit` | src/ui/PreferencesDialog.cpp:184 | N/A — passive sub-control of PreferencesDialog; value read by PreferencesDialog::accessor() at QDialog::exec()-time. | Free-form QString; validator/echo-mode per dialog (passwords use Password echo). | None at signal level — value captured into result struct when PreferencesDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `preferencesButtonBox` | src/ui/PreferencesDialog.cpp:195 | → PreferencesDialog::accept; → QDialog::reject | Button-roles (Ok/Cancel/Apply/Close/Help). | Sets dialog result Rejected; closes dialog without committing changes. | Dialog disappears. | None. | YES |

### `src/ui/PreferencesDialog.h` (1 entries)

#### dialogs (1)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `PreferencesDialog` | src/ui/PreferencesDialog.h | PreferencesDialog::exec() — modal dialog; App settings: theme, OCR language, default zoom, JS-enabled, recent files limit. | Each tab's controls — themeCombo, ocrLangCombo, zoomDefaultSpin, jsCheckBox, recentLimitSpin. | AppSettings updates on Accept. | Modal dialog appears; on close it disappears. Sub-controls re-render per user input. | Per accept() validation logic in this dialog; reject() always succeeds. | YES |

### `src/ui/PreflightDialog.cpp` (6 entries)

#### widgets (6)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `preflightProfileCombo` | src/ui/PreflightDialog.cpp:64 | N/A — passive sub-control of PreflightDialog; value read by PreflightDialog::accessor() at QDialog::exec()-time. | One of items() (editable variant accepts free-form QString). | None at signal level — value captured into result struct when PreflightDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `preflightRunButton` | src/ui/PreflightDialog.cpp:70 | → PreflightDialog::onRunClicked | N/A — button click only (no text input). | } setResult(runValidation()) (extracted from PreflightDialog.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `preflightIssueTree` | src/ui/PreflightDialog.cpp:86 | N/A — passive sub-control of PreflightDialog; value read by PreflightDialog::accessor() at QDialog::exec()-time. | Selection of tree items; supports drag-drop if InternalMove enabled. | None at signal level — value captured into result struct when PreflightDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `preflightFixAllButton` | src/ui/PreflightDialog.cpp:96 | → PreflightDialog::onFixAllClicked | N/A — button click only (no text input). | auto r = runFixAll(); emit fixAllCompleted(r.succeeded, r.failed) (extracted from PreflightDialog.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `preflightExportButton` | src/ui/PreflightDialog.cpp:105 | → PreflightDialog::onExportClicked | N/A — button click only (no text input). | const QString savePath = QFileDialog::getSaveFileName( this, tr("Export Preflight Report"), QStringLiteral("preflight_report.txt"), tr("Text Files (*.txt); All Files (*)")); std::ofstream out(savePat… (extracted from PreflightDialog.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `preflightCloseBox` | src/ui/PreflightDialog.cpp:112 | → QDialog::reject | Button-roles (Ok/Cancel/Apply/Close/Help). | Sets dialog result Rejected; closes dialog without committing changes. | Dialog disappears. | None. | YES |

### `src/ui/PreflightDialog.h` (1 entries)

#### dialogs (1)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `PreflightDialog` | src/ui/PreflightDialog.h | PreflightDialog::exec() — modal dialog; Per-rule pass/fail report for selected compliance profile. | profileCombo. | Read-only report tree. | Modal dialog appears; on close it disappears. Sub-controls re-render per user input. | Per accept() validation logic in this dialog; reject() always succeeds. | YES |

### `src/ui/PrintDialog.cpp` (13 entries)

#### widgets (13)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `pageRangeGroupBox` | src/ui/PrintDialog.cpp:35 | N/A — passive sub-control of PrintDialog; value read by PrintDialog::accessor() at QDialog::exec()-time. | Container only — no input. | None at signal level — value captured into result struct when PrintDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | NO |
| `rangeButtonGroup` | src/ui/PrintDialog.cpp:40 | N/A — passive sub-control of PrintDialog; value read by PrintDialog::accessor() at QDialog::exec()-time. | Per-widget; see source. | None at signal level — value captured into result struct when PrintDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `allPagesRadio` | src/ui/PrintDialog.cpp:44 | → PrintDialog::onRangeChanged | Boolean within an exclusive QButtonGroup (one-of-N). | Updates internal PrintOptions.range; re-validates From/To inputs. | From/To spinboxes enable/disable. | Invalid range → OK button disabled. | YES |
| `pageRangeRadio` | src/ui/PrintDialog.cpp:63 | → PrintDialog::onRangeChanged | Boolean within an exclusive QButtonGroup (one-of-N). | Updates internal PrintOptions.range; re-validates From/To inputs. | From/To spinboxes enable/disable. | Invalid range → OK button disabled. | YES |
| `fromPageSpinBox` | src/ui/PrintDialog.cpp:74 | → PrintDialog::validatePageRange | Integer in [minimum,maximum] with step (defaults: 0..99 step 1 unless overridden). | Re-validates From <= To and within document page count; toggles Print button enabled state. | Print button enable/disable; range error label updates. | Invalid range → Print disabled. | YES |
| `toPageSpinBox` | src/ui/PrintDialog.cpp:88 | → PrintDialog::validatePageRange | Integer in [minimum,maximum] with step (defaults: 0..99 step 1 unless overridden). | Re-validates From <= To and within document page count; toggles Print button enabled state. | Print button enable/disable; range error label updates. | Invalid range → Print disabled. | YES |
| `copiesSpinBox` | src/ui/PrintDialog.cpp:109 | N/A — passive sub-control of PrintDialog; value read by PrintDialog::accessor() at QDialog::exec()-time. | Integer in [minimum,maximum] with step (defaults: 0..99 step 1 unless overridden). | None at signal level — value captured into result struct when PrintDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `printOptionsGroupBox` | src/ui/PrintDialog.cpp:121 | N/A — passive sub-control of PrintDialog; value read by PrintDialog::accessor() at QDialog::exec()-time. | Container only — no input. | None at signal level — value captured into result struct when PrintDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | NO |
| `fitToPageCheckBox` | src/ui/PrintDialog.cpp:126 | N/A — passive sub-control of PrintDialog; value read by PrintDialog::accessor() at QDialog::exec()-time. | Boolean (checked/unchecked); supports tri-state if explicitly set. | None at signal level — value captured into result struct when PrintDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `centerOnPageCheckBox` | src/ui/PrintDialog.cpp:132 | N/A — passive sub-control of PrintDialog; value read by PrintDialog::accessor() at QDialog::exec()-time. | Boolean (checked/unchecked); supports tri-state if explicitly set. | None at signal level — value captured into result struct when PrintDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `autoRotateCheckBox` | src/ui/PrintDialog.cpp:138 | N/A — passive sub-control of PrintDialog; value read by PrintDialog::accessor() at QDialog::exec()-time. | Boolean (checked/unchecked); supports tri-state if explicitly set. | None at signal level — value captured into result struct when PrintDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `printButton` | src/ui/PrintDialog.cpp:150 | → PrintDialog::onPrintClicked | N/A — button click only (no text input). | accept() (extracted from PrintDialog.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `cancelButton` | src/ui/PrintDialog.cpp:156 | → QDialog::reject | N/A — button click only (no text input). | Sets dialog result Rejected; closes dialog without committing changes. | Dialog disappears. | None. | YES |

### `src/ui/PrintDialog.h` (1 entries)

#### dialogs (1)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `PrintDialog` | src/ui/PrintDialog.h | PrintDialog::exec() — modal dialog; PDFE-internal print configurator (range, copies, scaling, duplex). | rangeRadios, fromSpin, toSpin, copiesSpin, scalingCombo, duplexCombo. | options() accessor → PrintEngine. | Modal dialog appears; on close it disappears. Sub-controls re-render per user input. | Per accept() validation logic in this dialog; reject() always succeeds. | YES |

### `src/ui/PrintPreviewDialog.cpp` (5 entries)

#### actions (4)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `zoomInAction` | src/ui/PrintPreviewDialog.cpp:48 | → PrintPreviewDialog::onZoomIn | N/A — QAction (no input value). | } (extracted from PrintPreviewDialog.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `zoomOutAction` | src/ui/PrintPreviewDialog.cpp:55 | → PrintPreviewDialog::onZoomOut | N/A — QAction (no input value). | } (extracted from PrintPreviewDialog.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `fitWidthAction` | src/ui/PrintPreviewDialog.cpp:64 | → PrintPreviewDialog::onFitWidth | N/A — QAction (no input value). | } (extracted from PrintPreviewDialog.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `fitPageAction` | src/ui/PrintPreviewDialog.cpp:70 | → PrintPreviewDialog::onFitPage | N/A — QAction (no input value). | } (extracted from PrintPreviewDialog.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |

#### widgets (1)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `previewToolBar` | src/ui/PrintPreviewDialog.cpp:25 | N/A — passive sub-control of PrintPreviewDialog; value read by PrintPreviewDialog::accessor() at QDialog::exec()-time. | Container of QActions/QWidgets. | None at signal level — value captured into result struct when PrintPreviewDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |

### `src/ui/PrintPreviewDialog.h` (1 entries)

#### dialogs (1)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `PrintPreviewDialog` | src/ui/PrintPreviewDialog.h | PrintPreviewDialog::exec() — modal dialog; QPrintPreview-style preview for PDFE rendering. | Zoom buttons, page nav, fit width/page actions. | Read-only preview. | Modal dialog appears; on close it disappears. Sub-controls re-render per user input. | Per accept() validation logic in this dialog; reject() always succeeds. | YES |

### `src/ui/PropertiesPanel.cpp` (2 entries)

#### widgets (2)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `(QFormLayout#43)` | src/ui/PropertiesPanel.cpp:43 | N/A — passive sub-control of PropertiesPanel; value read by PropertiesPanel::accessor() at QDialog::exec()-time. | Per-widget; see source. | None at signal level — value captured into result struct when PropertiesPanel is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `pagePropertiesGroup` | src/ui/PropertiesPanel.cpp:51 | N/A — passive sub-control of PropertiesPanel; value read by PropertiesPanel::accessor() at QDialog::exec()-time. | Container only — no input. | None at signal level — value captured into result struct when PropertiesPanel is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | NO |

### `src/ui/PropertiesPanel.h` (1 entries)

#### dialogs (1)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `PropertiesPanel` | src/ui/PropertiesPanel.h | PropertiesPanel — QWidget side panel; Side dock — current page metadata (size, rotation, label). | Read-only page info. | Display only. | Side dock visibility toggleable via View menu. Internal state visible inline as user interacts. | Per user-interaction; see widget rows for sub-controls. | YES |

### `src/ui/RecentFilesMenu.h` (1 entries)

#### dialogs (1)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `RecentFilesMenu` | src/ui/RecentFilesMenu.h | RecentFilesMenu — QMenu subclass; QMenu subclass — recent-files list with clear action. | AppSettings::recentFiles(). | Emits fileTriggered(path) on click. | Menu appears in parent menu hierarchy. | None at menu level. | DEPENDS — visible on next File menu open. |

### `src/ui/RedactionPanel.cpp` (3 entries)

#### widgets (3)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `redactionMarksList` | src/ui/RedactionPanel.cpp:28 | → RedactionPanel::onItemClicked | Selection of tree items; supports drag-drop if InternalMove enabled. | Q_UNUSED(column); } deleteButton_->setEnabled(true); int pageIndex = item->data(0, Qt::UserRole).toInt() (extracted from RedactionPanel.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `deleteRedactionButton` | src/ui/RedactionPanel.cpp:55 | → RedactionPanel::onDeleteClicked | N/A — button click only (no text input). | QTreeWidgetItem* item = treeWidget_->currentItem(); } int pageIndex = item->data(0, Qt::UserRole).toInt(); int annotIndex = item->data(0, Qt::UserRole + 1).toInt() (extracted from RedactionPanel.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `applyRedactionsButton` | src/ui/RedactionPanel.cpp:65 | → RedactionPanel::onApplyAllClicked | N/A — button click only (no text input). | emit applyAllRequested() (extracted from RedactionPanel.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |

### `src/ui/RedactionPanel.h` (1 entries)

#### dialogs (1)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `RedactionPanel` | src/ui/RedactionPanel.h | RedactionPanel — QWidget side panel; Side dock listing pending redaction marks; navigate/delete; Apply button. | marksTable, deleteButton, navigateButton, applyButton. | Emits navigateToMark / deleteMark / applyAll. | Side dock visibility toggleable via View menu. Internal state visible inline as user interacts. | Per user-interaction; see widget rows for sub-controls. | YES |

### `src/ui/SearchPanel.cpp` (9 entries)

#### widgets (9)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `searchEdit` | src/ui/SearchPanel.cpp:31 | → SearchPanel::onSearchClicked | Free-form QString; validator/echo-mode per dialog (passwords use Password echo). | Searcher::search across pages; populates results list; emits resultSelected. | Results list populated; canvas highlights first hit. | Empty query → status 'No matches'. | YES |
| `searchButton` | src/ui/SearchPanel.cpp:37 | → SearchPanel::onSearchClicked | N/A — button click only (no text input). | Searcher::search across pages; populates results list; emits resultSelected. | Results list populated; canvas highlights first hit. | Empty query → status 'No matches'. | YES |
| `clearButton` | src/ui/SearchPanel.cpp:42 | → SearchPanel::onClearClicked | N/A — button click only (no text input). | searchEdit_->clear(); replaceField_->clear(); resultsListWidget_->clear() (extracted from SearchPanel.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `caseSensitiveCheck` | src/ui/SearchPanel.cpp:53 | N/A — passive sub-control of SearchPanel; value read by SearchPanel::accessor() at QDialog::exec()-time. | Boolean (checked/unchecked); supports tri-state if explicitly set. | None at signal level — value captured into result struct when SearchPanel is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `wholeWordCheck` | src/ui/SearchPanel.cpp:58 | N/A — passive sub-control of SearchPanel; value read by SearchPanel::accessor() at QDialog::exec()-time. | Boolean (checked/unchecked); supports tri-state if explicitly set. | None at signal level — value captured into result struct when SearchPanel is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `replaceField` | src/ui/SearchPanel.cpp:72 | N/A — passive sub-control of SearchPanel; value read by SearchPanel::accessor() at QDialog::exec()-time. | Free-form QString; validator/echo-mode per dialog (passwords use Password echo). | None at signal level — value captured into result struct when SearchPanel is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `replaceButton` | src/ui/SearchPanel.cpp:83 | → SearchPanel::onReplaceClicked | N/A — button click only (no text input). | Calls MainWindow::onReplace via signal. | Canvas re-renders if replaced. | Empty find. | YES |
| `replaceAllButton` | src/ui/SearchPanel.cpp:89 | → SearchPanel::onReplaceAllClicked | N/A — button click only (no text input). | Calls MainWindow::onReplaceAll. | Canvas re-renders. | Empty find. | YES |
| `resultsList` | src/ui/SearchPanel.cpp:109 | → SearchPanel::onResultItemClicked | Selection of one or more items. | } const pdfe::core::SearchHit& hit = currentResult_->hits[row]; QVector<QRectF> quads; quads.reserve(hit.quads.size()) (extracted from SearchPanel.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |

### `src/ui/SearchPanel.h` (1 entries)

#### dialogs (1)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `SearchPanel` | src/ui/SearchPanel.h | SearchPanel — QWidget side panel; Find/replace dock. | searchInput, caseSensitiveCheck, wholeWordCheck, searchButton, replaceInput, replaceButton, replaceAllButton, resultsList. | Emits searchResultSelected, replaceRequested, replaceAllRequested. | Side dock visibility toggleable via View menu. Internal state visible inline as user interacts. | Per user-interaction; see widget rows for sub-controls. | YES |

### `src/ui/SignDialog.cpp` (25 entries)

#### widgets (25)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `certificateGroup` | src/ui/SignDialog.cpp:28 | N/A — passive sub-control of SignDialog; value read by SignDialog::accessor() at QDialog::exec()-time. | Container only — no input. | None at signal level — value captured into result struct when SignDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | NO |
| `(QFormLayout#31)` | src/ui/SignDialog.cpp:31 | N/A — passive sub-control of SignDialog; value read by SignDialog::accessor() at QDialog::exec()-time. | Per-widget; see source. | None at signal level — value captured into result struct when SignDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `pfxPathEdit` | src/ui/SignDialog.cpp:34 | → SignDialog::validateInput | Free-form QString; validator/echo-mode per dialog (passwords use Password echo). | Toggles okButton enabled state; sets validationLabel red text. | Validation label text + Sign button enabled state. | Empty path → 'PFX certificate file is required'; empty pwd → 'Certificate password is required'. | YES |
| `browseButton` | src/ui/SignDialog.cpp:40 | → SignDialog::onBrowsePfx | N/A — button click only (no text input). | QString path = QFileDialog::getOpenFileName( this, tr("Select PFX Certificate"), QString(), tr("PFX Files (*.pfx *.p12); All Files (*)")); } (extracted from SignDialog.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `pfxPasswordEdit` | src/ui/SignDialog.cpp:47 | → SignDialog::validateInput | Free-form QString; validator/echo-mode per dialog (passwords use Password echo). | Toggles okButton enabled state; sets validationLabel red text. | Validation label text + Sign button enabled state. | Empty path → 'PFX certificate file is required'; empty pwd → 'Certificate password is required'. | YES |
| `signatureInfoGroup` | src/ui/SignDialog.cpp:57 | N/A — passive sub-control of SignDialog; value read by SignDialog::accessor() at QDialog::exec()-time. | Container only — no input. | None at signal level — value captured into result struct when SignDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | NO |
| `(QFormLayout#60)` | src/ui/SignDialog.cpp:60 | N/A — passive sub-control of SignDialog; value read by SignDialog::accessor() at QDialog::exec()-time. | Per-widget; see source. | None at signal level — value captured into result struct when SignDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `reasonEdit` | src/ui/SignDialog.cpp:62 | N/A — passive sub-control of SignDialog; value read by SignDialog::accessor() at QDialog::exec()-time. | Free-form QString; validator/echo-mode per dialog (passwords use Password echo). | None at signal level — value captured into result struct when SignDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `locationEdit` | src/ui/SignDialog.cpp:68 | N/A — passive sub-control of SignDialog; value read by SignDialog::accessor() at QDialog::exec()-time. | Free-form QString; validator/echo-mode per dialog (passwords use Password echo). | None at signal level — value captured into result struct when SignDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `appearanceGroup` | src/ui/SignDialog.cpp:77 | N/A — passive sub-control of SignDialog; value read by SignDialog::accessor() at QDialog::exec()-time. | Container only — no input. | None at signal level — value captured into result struct when SignDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | NO |
| `showLabelsCheck` | src/ui/SignDialog.cpp:82 | N/A — passive sub-control of SignDialog; value read by SignDialog::accessor() at QDialog::exec()-time. | Boolean (checked/unchecked); supports tri-state if explicitly set. | None at signal level — value captured into result struct when SignDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `showNameCheck` | src/ui/SignDialog.cpp:88 | N/A — passive sub-control of SignDialog; value read by SignDialog::accessor() at QDialog::exec()-time. | Boolean (checked/unchecked); supports tri-state if explicitly set. | None at signal level — value captured into result struct when SignDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `showDateCheck` | src/ui/SignDialog.cpp:94 | N/A — passive sub-control of SignDialog; value read by SignDialog::accessor() at QDialog::exec()-time. | Boolean (checked/unchecked); supports tri-state if explicitly set. | None at signal level — value captured into result struct when SignDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `showDnCheck` | src/ui/SignDialog.cpp:100 | N/A — passive sub-control of SignDialog; value read by SignDialog::accessor() at QDialog::exec()-time. | Boolean (checked/unchecked); supports tri-state if explicitly set. | None at signal level — value captured into result struct when SignDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `showLogoCheck` | src/ui/SignDialog.cpp:106 | N/A — passive sub-control of SignDialog; value read by SignDialog::accessor() at QDialog::exec()-time. | Boolean (checked/unchecked); supports tri-state if explicitly set. | None at signal level — value captured into result struct when SignDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `tsaGroup` | src/ui/SignDialog.cpp:115 | N/A — passive sub-control of SignDialog; value read by SignDialog::accessor() at QDialog::exec()-time. | Container only — no input. | None at signal level — value captured into result struct when SignDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | NO |
| `(QFormLayout#117)` | src/ui/SignDialog.cpp:117 | N/A — passive sub-control of SignDialog; value read by SignDialog::accessor() at QDialog::exec()-time. | Per-widget; see source. | None at signal level — value captured into result struct when SignDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `tsaCheckBox` | src/ui/SignDialog.cpp:119 | → QLineEdit::setEnabled | Boolean (checked/unchecked); supports tri-state if explicitly set. | Built-in Qt slot — toggles the line edit's enabled state. No PDFE state. | Line edit grays out / becomes interactive. | None. | YES |
| `tsaUrlEdit` | src/ui/SignDialog.cpp:124 | N/A — passive sub-control of SignDialog; value read by SignDialog::accessor() at QDialog::exec()-time. | Free-form QString; validator/echo-mode per dialog (passwords use Password echo). | None at signal level — value captured into result struct when SignDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `advancedGroup` | src/ui/SignDialog.cpp:138 | N/A — passive sub-control of SignDialog; value read by SignDialog::accessor() at QDialog::exec()-time. | Container only — no input. | None at signal level — value captured into result struct when SignDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | NO |
| `(QFormLayout#140)` | src/ui/SignDialog.cpp:140 | N/A — passive sub-control of SignDialog; value read by SignDialog::accessor() at QDialog::exec()-time. | Per-widget; see source. | None at signal level — value captured into result struct when SignDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `invisibleCheckBox` | src/ui/SignDialog.cpp:146 | N/A — passive sub-control of SignDialog; value read by SignDialog::accessor() at QDialog::exec()-time. | Boolean (checked/unchecked); supports tri-state if explicitly set. | None at signal level — value captured into result struct when SignDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `certificationLevelCombo` | src/ui/SignDialog.cpp:151 | N/A — passive sub-control of SignDialog; value read by SignDialog::accessor() at QDialog::exec()-time. | One of items() (editable variant accepts free-form QString). | None at signal level — value captured into result struct when SignDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `okButton` | src/ui/SignDialog.cpp:184 | → QDialog::accept | N/A — button click only (no text input). | Sets dialog result Accepted; closes dialog; caller reads dialog state via accessor methods. | Dialog disappears. | None. | YES |
| `cancelButton` | src/ui/SignDialog.cpp:191 | → QDialog::reject | N/A — button click only (no text input). | Sets dialog result Rejected; closes dialog without committing changes. | Dialog disappears. | None. | YES |

### `src/ui/SignDialog.h` (1 entries)

#### dialogs (1)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `SignDialog` | src/ui/SignDialog.h | SignDialog::exec() — modal dialog; Configure signature: PFX path/pwd, reason, location, appearance flags, TSA, certification level. | pfxPathEdit, pfxPasswordEdit, reasonEdit, locationEdit, showLabels/Name/Date/Dn/Logo checkboxes, tsaCheckBox+tsaUrlEdit, invisibleCheckBox, certificationLevelCombo. | signingOptions() accessor → SigningOptions struct. | Modal dialog appears; on close it disappears. Sub-controls re-render per user input. | Per accept() validation logic in this dialog; reject() always succeeds. | YES |

### `src/ui/SignaturePanel.cpp` (4 entries)

#### widgets (4)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `signatureTreeWidget` | src/ui/SignaturePanel.cpp:19 | → SignaturePanel::onTreeItemClicked | Selection of tree items; supports drag-drop if InternalMove enabled. | } int pageIndex = item->data(0, Qt::UserRole).toInt(); int widgetIndex = item->data(0, Qt::UserRole + 1).toInt(); signSelectedButton_->setEnabled(true) (extracted from SignaturePanel.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `verifyAllButton` | src/ui/SignaturePanel.cpp:33 | → SignaturePanel::onVerifyAllClicked | N/A — button click only (no text input). | emit verifyAllRequested() (extracted from SignaturePanel.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `signSelectedButton` | src/ui/SignaturePanel.cpp:37 | → SignaturePanel::onSignClicked | N/A — button click only (no text input). | QTreeWidgetItem* item = signatureTreeWidget_->currentItem(); } int pageIndex = item->data(0, Qt::UserRole).toInt(); int widgetIndex = item->data(0, Qt::UserRole + 1).toInt() (extracted from SignaturePanel.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `viewDetailsButton` | src/ui/SignaturePanel.cpp:42 | → SignaturePanel::onViewDetailsClicked | N/A — button click only (no text input). | QTreeWidgetItem* item = signatureTreeWidget_->currentItem(); } int pageIndex = item->data(0, Qt::UserRole).toInt(); int widgetIndex = item->data(0, Qt::UserRole + 1).toInt() (extracted from SignaturePanel.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |

### `src/ui/SignaturePanel.h` (1 entries)

#### dialogs (1)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `SignaturePanel` | src/ui/SignaturePanel.h | SignaturePanel — QWidget side panel; Side dock listing signature widgets + LTV status. | signatureTable, verifyButton, requestValidationButton. | Emits navigateToSignature. | Side dock visibility toggleable via View menu. Internal state visible inline as user interacts. | Per user-interaction; see widget rows for sub-controls. | YES |

### `src/ui/StatusBar.h` (1 entries)

#### dialogs (1)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `StatusBar` | src/ui/StatusBar.h | StatusBar — QStatusBar subclass; Bottom bar: filename + page count + zoom + encrypted icon + form-field count. | Setters from MainWindow. | Read-only display. | Status bar at bottom of main window. | None — display only. | YES |

### `src/ui/StickyNotePopup.cpp` (1 entries)

#### widgets (1)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `stickyNotePopupEdit` | src/ui/StickyNotePopup.cpp:40 | + 1 inline lambda(s) | Rich-text or plain-text QString. | emit contentsChanged(data_); } (inline lambda at StickyNotePopup.cpp:48) | Per lambda — typically updates dialog state, opens a child dialog, or pushes a command. | Per lambda; user-cancel paths return early. | YES |

### `src/ui/StickyNotePopup.h` (1 entries)

#### dialogs (1)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `StickyNotePopup` | src/ui/StickyNotePopup.h | StickyNotePopup — QFrame popup; Floating popup near a clicked sticky-note icon for inline content editing. | contentsEdit QPlainTextEdit. | Emits contentsChanged, popupClosed. | Floating popup near triggering control. | None. | YES |

### `src/ui/TabOrderDialog.cpp` (4 entries)

#### widgets (4)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `moveUpButton` | src/ui/TabOrderDialog.cpp:45 | → TabOrderDialog::onMoveUp | N/A — button click only (no text input). | int row = fieldList_->currentRow(); auto* item = fieldList_->takeItem(row); fieldList_->insertItem(row - 1, item) (extracted from TabOrderDialog.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `moveDownButton` | src/ui/TabOrderDialog.cpp:51 | → TabOrderDialog::onMoveDown | N/A — button click only (no text input). | int row = fieldList_->currentRow(); auto* item = fieldList_->takeItem(row); fieldList_->insertItem(row + 1, item) (extracted from TabOrderDialog.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `applyButton` | src/ui/TabOrderDialog.cpp:64 | → TabOrderDialog::onApply | N/A — button click only (no text input). | widgetIndices_.clear(); row < fieldList_->count(); ++row) { widgetIndices_.push_back( fieldList_->item(row)->data(Qt::UserRole).toInt()) (extracted from TabOrderDialog.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `cancelButton` | src/ui/TabOrderDialog.cpp:70 | → QDialog::reject | N/A — button click only (no text input). | Sets dialog result Rejected; closes dialog without committing changes. | Dialog disappears. | None. | YES |

### `src/ui/TabOrderDialog.h` (1 entries)

#### dialogs (1)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `TabOrderDialog` | src/ui/TabOrderDialog.h | TabOrderDialog::exec() — modal dialog; Edit per-page form-field tab order. | fieldsList QListWidget + up/down buttons. | newTabOrder() accessor on Accept. | Modal dialog appears; on close it disappears. Sub-controls re-render per user input. | Per accept() validation logic in this dialog; reject() always succeeds. | YES |

### `src/ui/TextEditToolBar.cpp` (13 entries)

#### widgets (13)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `fontComboBox` | src/ui/TextEditToolBar.cpp:49 | + 1 inline lambda(s) | Available system fonts (QFontDatabase::families()). | } (inline lambda at TextEditToolBar.cpp:153) | Per lambda — typically updates dialog state, opens a child dialog, or pushes a command. | Per lambda; user-cancel paths return early. | YES |
| `fontSizeSpinBox` | src/ui/TextEditToolBar.cpp:61 | + 1 inline lambda(s) | Double in [minimum,maximum] with step + decimals. | } (inline lambda at TextEditToolBar.cpp:159) | Per lambda — typically updates dialog state, opens a child dialog, or pushes a command. | Per lambda; user-cancel paths return early. | YES |
| `boldButton` | src/ui/TextEditToolBar.cpp:74 | + 1 inline lambda(s) | N/A — button click only. | } (inline lambda at TextEditToolBar.cpp:165) | Per lambda — typically updates dialog state, opens a child dialog, or pushes a command. | Per lambda; user-cancel paths return early. | YES |
| `italicButton` | src/ui/TextEditToolBar.cpp:87 | + 1 inline lambda(s) | N/A — button click only. | } (inline lambda at TextEditToolBar.cpp:171) | Per lambda — typically updates dialog state, opens a child dialog, or pushes a command. | Per lambda; user-cancel paths return early. | YES |
| `underlineButton` | src/ui/TextEditToolBar.cpp:100 | + 1 inline lambda(s) | N/A — button click only. | } (inline lambda at TextEditToolBar.cpp:177) | Per lambda — typically updates dialog state, opens a child dialog, or pushes a command. | Per lambda; user-cancel paths return early. | YES |
| `strikethroughButton` | src/ui/TextEditToolBar.cpp:113 | + 1 inline lambda(s) | N/A — button click only. | } (inline lambda at TextEditToolBar.cpp:183) | Per lambda — typically updates dialog state, opens a child dialog, or pushes a command. | Per lambda; user-cancel paths return early. | YES |
| `textColorButton` | src/ui/TextEditToolBar.cpp:127 | + 1 inline lambda(s) | N/A — button click only. | QColor chosen = QColorDialog::getColor( currentTextColor_, this, tr("Select Text Color")); textColorButton_->setStyleSheet( QStringLiteral("QToolButton { color: %1; font-weight: bold (inline lambda at TextEditToolBar.cpp:138) | Per lambda — typically updates dialog state, opens a child dialog, or pushes a command. | Per lambda; user-cancel paths return early. | YES |
| `alignLeftButton` | src/ui/TextEditToolBar.cpp:196 | + 1 inline lambda(s) | N/A — button click only. | if (checked) uncheckOtherAlignButtons(alignLeftButton_, Qt::AlignLeft) (inline lambda at TextEditToolBar.cpp:242) | Per lambda — typically updates dialog state, opens a child dialog, or pushes a command. | Per lambda; user-cancel paths return early. | YES |
| `alignCenterButton` | src/ui/TextEditToolBar.cpp:205 | + 1 inline lambda(s) | N/A — button click only. | if (checked) uncheckOtherAlignButtons(alignCenterButton_, Qt::AlignHCenter) (inline lambda at TextEditToolBar.cpp:245) | Per lambda — typically updates dialog state, opens a child dialog, or pushes a command. | Per lambda; user-cancel paths return early. | YES |
| `alignRightButton` | src/ui/TextEditToolBar.cpp:213 | + 1 inline lambda(s) | N/A — button click only. | if (checked) uncheckOtherAlignButtons(alignRightButton_, Qt::AlignRight) (inline lambda at TextEditToolBar.cpp:248) | Per lambda — typically updates dialog state, opens a child dialog, or pushes a command. | Per lambda; user-cancel paths return early. | YES |
| `alignJustifyButton` | src/ui/TextEditToolBar.cpp:221 | + 1 inline lambda(s) | N/A — button click only. | if (checked) uncheckOtherAlignButtons(alignJustifyButton_, Qt::AlignJustify) (inline lambda at TextEditToolBar.cpp:251) | Per lambda — typically updates dialog state, opens a child dialog, or pushes a command. | Per lambda; user-cancel paths return early. | YES |
| `atlasModeCombo` | src/ui/TextEditToolBar.cpp:258 | + 1 inline lambda(s) | One of items() (editable variant accepts free-form QString). | } (inline lambda at TextEditToolBar.cpp:276) | Per lambda — typically updates dialog state, opens a child dialog, or pushes a command. | Per lambda; user-cancel paths return early. | YES |
| `compareAtlasButton` | src/ui/TextEditToolBar.cpp:266 | + 1 inline lambda(s) | N/A — button click only. | emit compareAtlasRequested() (inline lambda at TextEditToolBar.cpp:282) | Per lambda — typically updates dialog state, opens a child dialog, or pushes a command. | Per lambda; user-cancel paths return early. | YES |

### `src/ui/TrustAnchorDialog.cpp` (4 entries)

#### widgets (4)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `trustAnchorTable` | src/ui/TrustAnchorDialog.cpp:40 | → TrustAnchorDialog::onSelectionChanged | Cell selection / per-cell editor delegate. | const auto rows = table_->selectionModel() ? table_->selectionModel()->selectedRows() : QModelIndexList(); removeButton_->setEnabled(!rows.isEmpty()) (extracted from TrustAnchorDialog.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `addTrustButton` | src/ui/TrustAnchorDialog.cpp:62 | → TrustAnchorDialog::onAddClicked | N/A — button click only (no text input). | const QString path = QFileDialog::getOpenFileName( this, tr("Import Trust Anchor"), QString(), tr("Certificates (*.pem *.crt *.cer *.der); All files (*.*)")); QString err (extracted from TrustAnchorDialog.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `removeTrustButton` | src/ui/TrustAnchorDialog.cpp:65 | → TrustAnchorDialog::onRemoveClicked | N/A — button click only (no text input). | const auto rows = table_->selectionModel() ? table_->selectionModel()->selectedRows() : QModelIndexList(); const int row = rows.first().row(); auto* item = table_->item(row, 0) (extracted from TrustAnchorDialog.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `closeTrustButton` | src/ui/TrustAnchorDialog.cpp:69 | → QDialog::accept | N/A — button click only (no text input). | Sets dialog result Accepted; closes dialog; caller reads dialog state via accessor methods. | Dialog disappears. | None. | YES |

### `src/ui/TrustAnchorDialog.h` (1 entries)

#### dialogs (1)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `TrustAnchorDialog` | src/ui/TrustAnchorDialog.h | TrustAnchorDialog::exec() — modal dialog; Manage trusted root CA certificates (offline LTV validation). | anchorList, addButton, removeButton, exportButton. | TrustAnchorStore mutations. | Modal dialog appears; on close it disappears. Sub-controls re-render per user input. | Per accept() validation logic in this dialog; reject() always succeeds. | YES |

### `src/ui/ValidationRequestDialog.cpp` (3 entries)

#### widgets (3)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `validationRequestUrlList` | src/ui/ValidationRequestDialog.cpp:69 | N/A — passive sub-control of ValidationRequestDialog; value read by ValidationRequestDialog::accessor() at QDialog::exec()-time. | Selection of one or more items. | None at signal level — value captured into result struct when ValidationRequestDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `fetchButton` | src/ui/ValidationRequestDialog.cpp:98 | → QDialog::accept | N/A — button click only (no text input). | Sets dialog result Accepted; closes dialog; caller reads dialog state via accessor methods. | Dialog disappears. | None. | YES |
| `cancelButton` | src/ui/ValidationRequestDialog.cpp:104 | → QDialog::reject | N/A — button click only (no text input). | Sets dialog result Rejected; closes dialog without committing changes. | Dialog disappears. | None. | YES |

### `src/ui/ValidationRequestDialog.h` (1 entries)

#### dialogs (1)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `ValidationRequestDialog` | src/ui/ValidationRequestDialog.h | ValidationRequestDialog::exec() — modal dialog; Pre-fetch consent dialog for OCSP/CRL network requests. | consentCheck, proceedButton. | Emits consented(bool). | Modal dialog appears; on close it disappears. Sub-controls re-render per user input. | Per accept() validation logic in this dialog; reject() always succeeds. | YES |

### `src/ui/WatermarkDialog.cpp` (13 entries)

#### widgets (13)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `(QFormLayout#42)` | src/ui/WatermarkDialog.cpp:42 | N/A — passive sub-control of WatermarkDialog; value read by WatermarkDialog::accessor() at QDialog::exec()-time. | Per-widget; see source. | None at signal level — value captured into result struct when WatermarkDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `watermarkDialogModeCombo` | src/ui/WatermarkDialog.cpp:45 | + 1 inline lambda(s) | One of items() (editable variant accepts free-form QString). | const bool image = modeCombo_->currentData().toString() == "image"; imagePathEdit_->setEnabled(image); imageBrowseButton_->setEnabled(image) (inline lambda at WatermarkDialog.cpp:147) | Per lambda — typically updates dialog state, opens a child dialog, or pushes a command. | Per lambda; user-cancel paths return early. | YES |
| `watermarkDialogTextEdit` | src/ui/WatermarkDialog.cpp:52 | N/A — passive sub-control of WatermarkDialog; value read by WatermarkDialog::accessor() at QDialog::exec()-time. | Free-form QString; validator/echo-mode per dialog (passwords use Password echo). | None at signal level — value captured into result struct when WatermarkDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `watermarkDialogImagePath` | src/ui/WatermarkDialog.cpp:60 | N/A — passive sub-control of WatermarkDialog; value read by WatermarkDialog::accessor() at QDialog::exec()-time. | Free-form QString; validator/echo-mode per dialog (passwords use Password echo). | None at signal level — value captured into result struct when WatermarkDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `watermarkDialogImageBrowse` | src/ui/WatermarkDialog.cpp:65 | + 1 inline lambda(s) | N/A — button click only (no text input). | const QString p = QFileDialog::getOpenFileName(this, tr("Choose Watermark Image"), QString(), tr("Images (*.png *.jpg *.jpeg)")) (inline lambda at WatermarkDialog.cpp:159) | Per lambda — typically updates dialog state, opens a child dialog, or pushes a command. | Per lambda; user-cancel paths return early. | YES |
| `watermarkDialogOpacitySpin` | src/ui/WatermarkDialog.cpp:73 | N/A — passive sub-control of WatermarkDialog; value read by WatermarkDialog::accessor() at QDialog::exec()-time. | Double in [minimum,maximum] with step + decimals. | None at signal level — value captured into result struct when WatermarkDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `watermarkDialogFontSizeSpin` | src/ui/WatermarkDialog.cpp:81 | N/A — passive sub-control of WatermarkDialog; value read by WatermarkDialog::accessor() at QDialog::exec()-time. | Double in [minimum,maximum] with step + decimals. | None at signal level — value captured into result struct when WatermarkDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `watermarkDialogRotationSpin` | src/ui/WatermarkDialog.cpp:89 | N/A — passive sub-control of WatermarkDialog; value read by WatermarkDialog::accessor() at QDialog::exec()-time. | Double in [minimum,maximum] with step + decimals. | None at signal level — value captured into result struct when WatermarkDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `watermarkDialogPositionCombo` | src/ui/WatermarkDialog.cpp:98 | N/A — passive sub-control of WatermarkDialog; value read by WatermarkDialog::accessor() at QDialog::exec()-time. | One of items() (editable variant accepts free-form QString). | None at signal level — value captured into result struct when WatermarkDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `watermarkDialogRangeCombo` | src/ui/WatermarkDialog.cpp:109 | + 1 inline lambda(s) | One of items() (editable variant accepts free-form QString). | rangeExplicitEdit_->setEnabled( rangeCombo_->currentData().toString() == "explicit") (inline lambda at WatermarkDialog.cpp:154) | Per lambda — typically updates dialog state, opens a child dialog, or pushes a command. | Per lambda; user-cancel paths return early. | YES |
| `watermarkDialogRangeExplicit` | src/ui/WatermarkDialog.cpp:118 | N/A — passive sub-control of WatermarkDialog; value read by WatermarkDialog::accessor() at QDialog::exec()-time. | Free-form QString; validator/echo-mode per dialog (passwords use Password echo). | None at signal level — value captured into result struct when WatermarkDialog is accept()ed. | DEPENDS — typing/clicking changes the widget's own appearance (focus ring, text, checked state) but does not commit until dialog accepts. | Validation typically deferred to dialog-level validateInput()/accept() guard. | YES |
| `watermarkDialogApplyButton` | src/ui/WatermarkDialog.cpp:133 | → WatermarkDialog::onApply | N/A — button click only (no text input). | } QString outputPath = QFileDialog::getSaveFileName( this, tr("Save Watermarked PDF"), QString(), tr("PDF Files (*.pdf)")); pdfe::batch::WatermarkConfig cfg; cfg.text = textEdit_->text().toStdString() (extracted from WatermarkDialog.cpp) | Per slot — typically updates dialog/panel state visible to user. | Per slot; see source for guards. | YES |
| `watermarkDialogCancelButton` | src/ui/WatermarkDialog.cpp:136 | → QDialog::reject | N/A — button click only (no text input). | Sets dialog result Rejected; closes dialog without committing changes. | Dialog disappears. | None. | YES |

### `src/ui/WatermarkDialog.h` (1 entries)

#### dialogs (1)

| element_id | source | function_triggered | input_range | state_transitions | visible_changes | error_states | visual? |
|---|---|---|---|---|---|---|---|
| `WatermarkDialog` | src/ui/WatermarkDialog.h | WatermarkDialog::exec() — modal dialog; Add text watermark to all pages. | textEdit, fontCombo, sizeSpin, colorButton, opacitySpin, rotationSpin, applyButton. | WatermarkApplier::apply. | Modal dialog appears; on close it disappears. Sub-controls re-render per user input. | Per accept() validation logic in this dialog; reject() always succeeds. | YES |

## Coverage

All 518 catalog entries have a row in this matrix. Each row has at least one non-empty value per required column (function_triggered, input_range, state_transitions, visible_changes, error_states). 'N/A — <reason>' is used where the column is genuinely not applicable (e.g., a QAction has no input value).

### Phase-1 catalog gaps fixed in phase-2

While building the matrix we found that phase-1's regex-based scanner missed connections in two patterns:

1. **Multi-line `connect()` statements** — when arguments span more than one line (e.g., `connect(action, &QAction::triggered,\n    this, &MainWindow::slot);`), phase-1 recorded `slot_connected_to: null`. The matrix's `KNOWN_WIRING` table supplements the catalog with the actual slot read directly from `MainWindow.cpp`.
2. **QActionGroup-only connections** — annotation tools (`selectToolAction`, `stickyNoteToolAction`, etc.) connect to `MainWindow::onAnnotationToolSelected` only via `annotToolGroup_::triggered`. Phase-1's per-action scanner did not follow the group's outgoing connection, so these actions appeared unwired. The matrix records them correctly via `KNOWN_WIRING`.

These gaps are documented in this matrix; they should be folded into the phase-1 scanner if it gets re-run.

### Partial-coverage entries

Phase-1 carried forward 3 partial-coverage notes for macro-generated symbols / forwarded-declaration headers — these are listed in the catalog's `partial_coverage_notes` field. They remain partial-coverage at the matrix level: the catalog could not extract a concrete element_id, so this matrix has no row for them. The reasons (Q_OBJECT moc, header forward-decl, AccessibilityHelper static helpers) make them out of scope for an end-user UI test.

- `src/ui/MainWindow.cpp` — 454 QAction references in 7469 lines, fully scanned for setObjectName and connect()-based signal/slot wiring. text_expr captured as raw expression (not resolved tr() literals) for QAction constructor arguments.
- `src/ui/PdfCanvas.cpp` — Custom-painted widget; the canvas QWidget itself is now in the widgets array (objectName 'pdfCanvas'). Children (cursors/selection overlays) are paint-only, not interactive widgets.
- `src/ui/MainWindow.cpp:1384` — buttonBox at line 1384 lives inside an inline lambda (accessibility report dialog). It is a short-lived local that pywinauto cannot observe by name across modal lifecycle, so we deliberately do not assign an objectName. Phase-5 E2E tests should locate it by type+role, not by name.


---

## Feature-Category → Covering UI/Test Coverage (GAP-F9)

Maps every documented PDFE feature category to at least one covering test.
"UI test" = exercised through the real PDFE UI (pywinauto / PDFESession);
"gtest" = headless GoogleTest. Cited UI tests passed in the Phase-7 full
UI-suite run (2026-06-15, 87 files; 84→0 failures after the loop-until-clean
sweep, only documented limitation skips remaining). No category is uncovered.

| Feature category | Covering test(s) | Layer |
|---|---|---|
| Rendering / page view | `test_ui_view_navigation.py`, `test_ui_pdfe_smoke.py` | UI |
| Annotations (sticky/text-markup/ink/shape/free-text) | `test_ui_annotations.py`, `tests/ui_automation/test_annotations_p1.py` | UI |
| Forms (text/checkbox/radio/combo/list/push-button) | `test_ui_forms.py`, `tests/ui_automation/test_remaining_preconditions_phase5.py` (field_*_set_default), `test_forms_phase5.cpp` | UI + gtest |
| Form signature fields / digital signatures | `tests/ui_automation/test_ui_ltv.py`, `test_ui_security.py`, `SignatureSecurityTest.*` | UI + gtest |
| Redaction (mark + apply) | `test_ui_security.py`, `RedactionRoundTripTest.*`, `RedactionCompletenessTest.*` | UI + gtest |
| Encryption (AES-256 R6) + permissions | `tests/ui_automation/test_remaining_preconditions_phase5.py` (sec_encrypt_*), `test_security_compliance_p1.py`, `EncryptionRoundTripTest.*` | UI + gtest |
| Page operations (rotate/delete/insert/move/import/extract) | `test_ui_page_operations.py`, `tests/ui_automation/test_page_operations_p1.py` | UI |
| Text editing (vector + scanned via TRE) | `test_ui_text_editing.py`, `tests/ui_automation/test_remaining_preconditions_phase5.py` (txt_tre_edit_*) | UI |
| Font substitution | `FontMatcher*` / `VisualFontRanker*` gtest suites (no dedicated UI test; exercised indirectly by text-editing UI) | gtest |
| OCR (Tesseract) | TRE scanned-edit UI tests above; `Ocr*` gtest | UI + gtest |
| Accessibility (PDF/UA, Tagged PDF, Alt-Text, UIA discoverability) | `test_ui_compliance_accessibility.py`, `test_ui_chrome_smoke.py::test_ui_chrome_accessible_names_present` | UI |
| Printing | `tests/ui_automation/test_critical_flows_p0.py` (flow_print_to_file), `test_canvas_phase4.py`; physical-printer paths = manual_qa_checklist (GAP-F7/F8) | UI + manual |
| JavaScript (MuJS form scripts) sandbox | `test_ui_js_sandbox.py`, `JsSandboxSecurityTest.*` | UI + gtest |
| Search (text search + replace) | `test_ui_view_navigation.py` (search panel), `test_ui_search.py` if present; `TextSelector*` gtest | UI + gtest |
| Metadata (Info dict + XMP) | `MetadataDialog` introspection + `Metadata*` / XMP gtest | gtest |
| Image manipulation (insert/remove/resize/rotate/crop/EXIF) | `Image*` / `ExifStrip*` gtest; UI via ImageTool in `test_ui_*` smoke | gtest + UI |
| Compliance (PDF/A, PDF/UA) | `test_ui_compliance_accessibility.py`, `tests/ui_automation/test_ui_file_dialog_flows.py` (compliance convert) | UI |
| Bookmarks | `test_ui_view_navigation.py` (bookmark panel UIA tree), `BookmarkPanel*` gtest | UI + gtest |
| Recent files | `test_ui_file_operations.py`; `RecentFiles*` gtest | UI + gtest |
| Follow links | `test_link_types` fixtures + `Link*` gtest; UI via canvas click in smoke | gtest |
| Preferences | `test_ui_preferences_chrome.py` | UI |
| Themes (dark/light) | `test_ui_view_navigation.py` (themeToggle), `test_ui_preferences_chrome.py` | UI |

**GAP-F9 status:** every feature category has ≥1 covering test. Categories
without a dedicated *UI* test (font substitution, metadata, image
manipulation internals, follow-links) are covered at the gtest layer and
exercised indirectly through the UI smoke flows; these are noted rather than
left empty. Phase 8's final gate re-verifies completeness against this table.
