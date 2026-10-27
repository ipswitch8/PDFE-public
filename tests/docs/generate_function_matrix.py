# -*- coding: utf-8 -*-
"""
Pipeline a7c2e541 phase-2 — UI Function Matrix generator.

Reads tests/docs/ui_element_catalog.json (518 entries from phase-1) and emits
tests/docs/ui_function_matrix.md plus tests/docs/ui_function_matrix.json so
downstream pipeline phases can query the per-element function spec
programmatically.

Strategy
--------
1. Phase-1 catalog already records ``signal_slot_wiring`` for connected
   widgets/actions. That gives the function_triggered column directly for
   ~170 entries.
2. For unwired widgets (309 - wired = ~226), the widget is a passive sub-
   control of a dialog; function_triggered is "N/A — passive sub-control of
   <parent>" and state_transitions are limited to "captured by <parent
   dialog>::<accessor>() at exec()-time".
3. Shared slots (MainWindow::openFile, onSaveFile, etc.) are documented
   ONCE in a shared-slots table and referenced from every row that
   triggers them. Slot bodies were read directly from MainWindow.cpp at
   generator authoring time.
4. Visual-verify decision rule (per pipeline.json):
     YES = produces canvas re-render, opens dialog, navigates pages,
           changes zoom, applies annotation, edits text, redacts, signs,
           runs OCR, switches mode, toggles panel visibility
     NO  = registers internal state only (e.g., RecentFilesMenu::addFile,
           NavigationHistory::recordVisit)
     DEPENDS = undo/redo (visible effect depends on what was undone)
"""

from __future__ import annotations

import io
import json
import sys
from collections import defaultdict
from pathlib import Path

# --------------------------------------------------------------------------- #
# 1. Shared slot table — slot bodies hand-extracted from src/ui/MainWindow.cpp #
# --------------------------------------------------------------------------- #

SHARED_SLOTS = {
    "MainWindow::onNewFile": {
        "input": "N/A — no input.",
        "state": "Creates blank PDF in QDir::temp() via Document::createBlank(); routes through openFile() so all open-file state machinery runs.",
        "visible": "Window title updates to 'pdfe_untitled_<ms>.pdf', canvas renders blank page, status bar shows page count, dirty marker absent.",
        "errors": "Document::createBlank throws → QMessageBox::critical 'Failed to create blank PDF'; subsequent openFile() failure surfaces its own messaging.",
        "visual": "YES",
    },
    "MainWindow::onOpenFile": {
        "input": "QFileDialog filter 'PDF Files (*.pdf);;All Files (*)' starting from AppSettings::lastOpenDirectory().",
        "state": "Calls openFile(fileName) — closes existing doc, opens new pdfe::core::Document, replaces tileCache_/tileRenderer_/annotationManager_/formManager_/etc., enables ~30 actions, records in recentFilesMenu_.",
        "visible": "Same as MainWindow::openFile (see below).",
        "errors": "User cancels file dialog (no-op return); see openFile error paths.",
        "visual": "YES",
    },
    "MainWindow::openFile": {
        "input": "QString filePath (UTF-16 path).",
        "state": "closeDocument() → Document::open up to 4× retry with PasswordDialog → recreate TileCache/TileRenderer/AnnotationManager/FormManager/JavaScriptEngine/RedactionManager/EncryptionManager/SignatureManager/ImageManager/MetadataManager → wire panels → enable saveAction/saveAsAction/closeAction/copyAction/cutAction/pasteAction/selectAllAction/zoom*/print*/form*/page*/encryption*/signature*/image*/metadata*/compliance/preflight/replace/tabOrder/calculationOrder/altText/submitForm/ocr/treAction/accessibilityAction → recentFilesMenu_->addFile() → AppSettings::setLastOpenDirectory.",
        "visible": "Window title shows filename, status bar shows fileName + 1/N + 100%, canvas renders page 1 fitted, page panel populated, bookmark dock shown if outline present, signature dock shown if signatures present.",
        "errors": "Encrypted PDF without password → PasswordDialog (3 retries, then propagate); Document::open throws → QMessageBox::critical 'Failed to open PDF file' + closeDocument() rollback.",
        "visual": "YES",
    },
    "MainWindow::onSaveFile": {
        "input": "N/A.",
        "state": "BookmarkPanel::commitToDocument → Document::save(currentFileName_) (atomic temp+rename) → flushAlignmentPipelineIfPending if pending → undoManager_->setClean() → onDocumentStructureChanged().",
        "visible": "Window title clears '*' suffix; status bar 'File saved' for 3000 ms.",
        "errors": "Document not PDF → QMessageBox::warning 'Only PDF documents can be saved'; std::exception during save → QMessageBox::critical 'Save Error'.",
        "visual": "YES",
    },
    "MainWindow::onSaveAsFile": {
        "input": "QFileDialog filter 'PDF Files (*.pdf);;All Files (*)' default = currentFileName_.",
        "state": "BookmarkPanel::commitToDocument → Document::saveAs(newPath) → flushAlignmentPipelineIfPending if pending → currentFileName_ = newPath → undoManager_->setClean() → updateWindowTitle.",
        "visible": "Title updates to new filename, '*' cleared, status bar 'File saved as <name>' for 3000 ms.",
        "errors": "User cancels (no-op); non-PDF → warning; save throws → QMessageBox::critical 'Save Error'.",
        "visual": "YES",
    },
    "MainWindow::onCloseFile": {
        "input": "N/A.",
        "state": "closeDocument() — releases document_, tileCache_, tileRenderer_, annotationManager_, formManager_, jsEngine_, redactionManager_, encryptionManager_, signatureManager_, imageManager_, metadataManager_; clears commandPages_, toolPage_; disables ~30 document-required actions; clears canvas; resets nav history; closes sticky note popup; clears panels.",
        "visible": "Canvas blank, status bar empty, title '<unnamed>', all dock panels emptied.",
        "errors": "None — slot is unconditional.",
        "visual": "YES",
    },
    "MainWindow::onCreatePdfFromImages": {
        "input": "ImageToPdfWizard interactive flow: image files + page-size + DPI + JPEG quality.",
        "state": "Wizard returns std::filesystem::path; openFile(out) replaces document_ with the new PDF.",
        "visible": "Wizard dialog; on success canvas shows new PDF, recent-files updated.",
        "errors": "Wizard cancelled (out.empty(), no-op); openFile errors propagate.",
        "visual": "YES",
    },
    "MainWindow::onPreviousView": {
        "input": "N/A.",
        "state": "navHistory_.goBack() returns previous page index; canvas->goToPage(); previous/next view actions re-enabled based on history.canGoBack/canGoForward.",
        "visible": "Canvas navigates to previous viewed page; status bar page index updates.",
        "errors": "navHistory_.canGoBack() == false → no-op return.",
        "visual": "YES",
    },
    "MainWindow::onNextView": {
        "input": "N/A.",
        "state": "navHistory_.goForward(); canvas_->goToPage().",
        "visible": "Canvas moves to next visited page.",
        "errors": "canGoForward() false → no-op.",
        "visual": "YES",
    },
    "MainWindow::onExportPlainText": {
        "input": "QFileDialog filter 'Text Files (*.txt);;All Files (*)' default <basename>.txt.",
        "state": "PlainTextExporter::write(document_, savePath) — pure read; no document mutation.",
        "visible": "Status bar 'Exported text to <name>' for 3000 ms.",
        "errors": "User cancel (no-op); writer returns false → QMessageBox::warning 'Could not write file'.",
        "visual": "NO",
    },
    "MainWindow::onExit": {
        "input": "N/A.",
        "state": "QCoreApplication::quit() via close()/closeEvent flow; closeEvent calls hasUnsavedChanges and may prompt save.",
        "visible": "Application terminates.",
        "errors": "Unsaved changes → modal 'Save changes?' yes/no/cancel.",
        "visual": "YES",
    },
    "MainWindow::onUndo": {
        "input": "N/A.",
        "state": "undoManager_->undo(); if scanned-page image edit on top, restore saved XObject ref via PdfContentWriter::restoreImageXObjectRef + invalidate ImageReplacer cache; canvas->refreshAllPages; onDocumentStructureChanged; updateWindowTitle.",
        "visible": "Canvas re-renders with the prior state; window title may add/remove '*' depending on whether undo crosses the saveClean barrier.",
        "errors": "Empty undo stack → undo() is a no-op.",
        "visual": "DEPENDS — visible effect depends on what's being undone (page rotation visible; metadata edit not visible from canvas).",
    },
    "MainWindow::onRedo": {
        "input": "N/A.",
        "state": "undoManager_->redo(); if next entry has image edit, refreshImageForScannedPage(); markPageDirty; canvas->refreshAllPages; onDocumentStructureChanged.",
        "visible": "Canvas reverts to redone state.",
        "errors": "Empty redo stack → no-op.",
        "visual": "DEPENDS",
    },
    "MainWindow::onCopy": {
        "input": "N/A.",
        "state": "page->extractPlainText() into QApplication::clipboard()->setText().",
        "visible": "Status bar 'Page text copied to clipboard' for 3000 ms; clipboard side-effect not visible in app window.",
        "errors": "No document/canvas → no-op; empty page text → status bar 'Copy: no text on this page'.",
        "visual": "NO",
    },
    "MainWindow::onCut": {
        "input": "N/A.",
        "state": "If textEditTool active and has selection: tool->performCut() — pushes EditTextCommand on undoManager + writes to clipboard. Otherwise no-op.",
        "visible": "Status bar 'Text cut to clipboard' (or 'Cut: select text in an editable span first').",
        "errors": "No selection → status bar message only.",
        "visual": "DEPENDS — visible only if cut succeeds (canvas re-renders).",
    },
    "MainWindow::onPaste": {
        "input": "Clipboard text (UTF-16).",
        "state": "If textEditTool active: tool->performPaste() — inserts clipboard text into edit buffer.",
        "visible": "Status bar 'Clipboard text pasted'; canvas re-renders if paste succeeded.",
        "errors": "No tool active → status bar 'Paste: activate text editing first'.",
        "visual": "DEPENDS",
    },
    "MainWindow::onSelectAll": {
        "input": "N/A.",
        "state": "page->extractPlainText() copied to clipboard.",
        "visible": "Status bar 'All page text selected and copied to clipboard'.",
        "errors": "No document/canvas → no-op; empty page → status bar message.",
        "visual": "NO",
    },
    "MainWindow::onFind": {
        "input": "N/A.",
        "state": "searchPanelDock_->show()/raise(); searchPanel_->focusSearchBox().",
        "visible": "Search dock visible + focused; search line edit gets keyboard focus.",
        "errors": "None.",
        "visual": "YES",
    },
    "MainWindow::onReplace": {
        "input": "(QString find, QString replace) from SearchPanel.",
        "state": "Locates current hit; builds EditTextCommand; undoManager_->execute; advances to next match.",
        "visible": "Canvas re-renders; SearchPanel status label 'Replaced 1 match'/'No match selected'/'Match not found in text spans'.",
        "errors": "Empty find / no current hit → status text only.",
        "visual": "YES",
    },
    "MainWindow::onReplaceAll": {
        "input": "(QString find, QString replace).",
        "state": "Iterates every page+span; builds CompositeCommand 'Replace All'; one undo entry covers all replacements.",
        "visible": "Canvas re-renders; SearchPanel 'Replaced N match(es)'/'No matches found'.",
        "errors": "Empty find → no-op.",
        "visual": "YES",
    },
    "MainWindow::onPrint": {
        "input": "PrintDialog modal — pages range, copies, duplex, color, scaling.",
        "state": "PrintEngine::print(QPrinter, options); fires JS willPrint/didPrint events on first page.",
        "visible": "Print dialog modal; printer system dialog if Native; status bar updates not specified.",
        "errors": "Print engine returns false → QMessageBox::warning 'Print Error'.",
        "visual": "YES",
    },
    "MainWindow::onPrintPreview": {
        "input": "N/A — opens preview with PrintRange::AllPages default.",
        "state": "PrintPreviewDialog modal exec().",
        "visible": "Print preview dialog modal.",
        "errors": "None — preview is read-only.",
        "visual": "YES",
    },
    "MainWindow::onZoomIn": {
        "input": "N/A.",
        "state": "canvas_->zoomIn() — multiplies current zoom by step factor.",
        "visible": "Canvas re-renders larger; zoomCombo + statusBar reflect new percent.",
        "errors": "No canvas → no-op.",
        "visual": "YES",
    },
    "MainWindow::onZoomOut": {
        "input": "N/A.",
        "state": "canvas_->zoomOut().",
        "visible": "Canvas re-renders smaller; zoomCombo + statusBar update.",
        "errors": "No canvas → no-op.",
        "visual": "YES",
    },
    "MainWindow::onFitWidth": {
        "input": "N/A.",
        "state": "canvas_->fitWidth() — sets zoom such that page width fills viewport.",
        "visible": "Canvas re-renders; zoomCombo updates.",
        "errors": "No canvas → no-op.",
        "visual": "YES",
    },
    "MainWindow::onFitPage": {
        "input": "N/A.",
        "state": "canvas_->fitPage() — sets zoom such that whole page fits viewport.",
        "visible": "Canvas re-renders; zoomCombo updates.",
        "errors": "No canvas → no-op.",
        "visual": "YES",
    },
    "MainWindow::onActualSize": {
        "input": "N/A.",
        "state": "canvas_->setZoom(1.0f).",
        "visible": "Canvas zoom 100%; zoomCombo shows '100%'.",
        "errors": "No canvas → no-op.",
        "visual": "YES",
    },
    "MainWindow::onToggleFullScreen": {
        "input": "N/A.",
        "state": "showFullScreen() / showNormal(); fullScreenAction_->setChecked() reflects.",
        "visible": "Window enters/leaves full-screen mode; menu bar usually hidden in full screen.",
        "errors": "None.",
        "visual": "YES",
    },
    "MainWindow::onTogglePagePanel": {
        "input": "N/A.",
        "state": "pagePanelDock_->setVisible(!visible).",
        "visible": "Side dock toggles visible/hidden.",
        "errors": "Dock null → no-op.",
        "visual": "YES",
    },
    "MainWindow::onTogglePropertiesPanel": {
        "input": "N/A.",
        "state": "propertiesPanelDock_->setVisible(!visible).",
        "visible": "Properties dock toggles.",
        "errors": "None.",
        "visual": "YES",
    },
    "MainWindow::onToggleSearchPanel": {
        "input": "N/A.",
        "state": "searchPanelDock_->setVisible(!visible).",
        "visible": "Search dock toggles.",
        "errors": "None.",
        "visual": "YES",
    },
    "MainWindow::onToggleBookmarkPanel": {
        "input": "N/A.",
        "state": "bookmarkPanelDock_->setVisible(!visible).",
        "visible": "Bookmark dock toggles.",
        "errors": "None.",
        "visual": "YES",
    },
    "MainWindow::onAbout": {
        "input": "N/A.",
        "state": "AboutDialog modal exec().",
        "visible": "About dialog modal.",
        "errors": "None.",
        "visual": "YES",
    },
    "MainWindow::onDocumentation": {
        "input": "N/A.",
        "state": "QDesktopServices::openUrl('https://github.com/dev/PDFE#documentation').",
        "visible": "Default browser opens URL (external app).",
        "errors": "None — Qt swallows openUrl failures.",
        "visual": "NO — external browser.",
    },
    "MainWindow::onReportIssue": {
        "input": "N/A.",
        "state": "QDesktopServices::openUrl('https://github.com/dev/PDFE/issues/new').",
        "visible": "External browser.",
        "errors": "None.",
        "visual": "NO",
    },
    "MainWindow::onCheckUpdates": {
        "input": "N/A.",
        "state": "QDesktopServices::openUrl('https://github.com/dev/PDFE/releases').",
        "visible": "External browser.",
        "errors": "None.",
        "visual": "NO",
    },
    "MainWindow::onPageChanged": {
        "input": "(int pageIndex).",
        "state": "navHistory_.recordVisit; pendingAtlas reset; statusBar setPageInfo; pagePanel setCurrentPage; propertiesPanel setPageInfo; textEditTool setPage if active; activateTextEditForPage if auto-routing; loadOcrBoxesForPage.",
        "visible": "Page panel highlight moves; properties panel updates; status bar shows page X of N.",
        "errors": "No document → no-op return.",
        "visual": "YES",
    },
    "MainWindow::onZoomChanged": {
        "input": "(float zoom).",
        "state": "statusBar_->setZoom(zoom*100); updateZoomCombo.",
        "visible": "Status bar percent update; zoom combo text updates.",
        "errors": "None.",
        "visual": "YES",
    },
    "MainWindow::onPageSelected": {
        "input": "(int pageIndex).",
        "state": "canvas_->goToPage(pageIndex).",
        "visible": "Canvas scrolls to pageIndex.",
        "errors": "No canvas → no-op.",
        "visual": "YES",
    },
    "MainWindow::onSearchResultSelected": {
        "input": "(int pageIndex, QVector<QRectF> quads).",
        "state": "canvas->setSearchHighlights; canvas->goToPage.",
        "visible": "Canvas navigates + highlights match quads.",
        "errors": "None.",
        "visual": "YES",
    },
    "MainWindow::onBookmarkActivated": {
        "input": "(int pageIndex, float x, float y, float zoom). x/y/zoom currently ignored.",
        "state": "canvas_->goToPage(pageIndex).",
        "visible": "Canvas navigates to bookmark target.",
        "errors": "None.",
        "visual": "YES",
    },
    "MainWindow::onZoomComboChanged": {
        "input": "Combo index — items 25%,50%,75%,100%,125%,150%,200%,400%.",
        "state": "Parses percent, canvas_->setZoom(percent/100).",
        "visible": "Canvas re-render.",
        "errors": "Unparseable text → no-op.",
        "visual": "YES",
    },
    "MainWindow::onZoomComboEdited": {
        "input": "QString text (free entry, % stripped).",
        "state": "Same as onZoomComboChanged.",
        "visible": "Canvas re-render.",
        "errors": "Unparseable → no-op.",
        "visual": "YES",
    },
    "MainWindow::onRotatePageCW": {
        "input": "N/A — selection from PagePanel + currentPage fallback.",
        "state": "RotatePageCommand(+90) for each selected; undoManager_->execute; onDocumentStructureChanged.",
        "visible": "Canvas + page panel thumbnails rotate.",
        "errors": "No document/undoManager → no-op.",
        "visual": "YES",
    },
    "MainWindow::onRotatePageCCW": {
        "input": "N/A.",
        "state": "RotatePageCommand(-90) for each selected.",
        "visible": "Canvas + thumbnails rotate.",
        "errors": "No document → no-op.",
        "visual": "YES",
    },
    "MainWindow::onRotatePage180": {
        "input": "N/A.",
        "state": "RotatePageCommand(180) for each selected.",
        "visible": "Canvas + thumbnails rotate 180.",
        "errors": "None.",
        "visual": "YES",
    },
    "MainWindow::onDeletePages": {
        "input": "Confirm dialog Yes/No.",
        "state": "DeletePageRangeCommand; refuses to delete all pages; onDocumentStructureChanged.",
        "visible": "Page count drops; thumbnails removed; canvas advances or stays.",
        "errors": "Selection == total page count → QMessageBox warning 'At least one page must remain'; user clicks No → no-op.",
        "visual": "YES",
    },
    "MainWindow::onInsertBlankPage": {
        "input": "N/A — uses currentPage size or 612x792 Letter default.",
        "state": "InsertBlankPageCommand; canvas->goToPage(insertAt).",
        "visible": "New blank page appears; canvas navigates to it.",
        "errors": "No document/undoManager → no-op.",
        "visual": "YES",
    },
    "MainWindow::onInsertPagesFromFile": {
        "input": "QFileDialog 'PDF Files (*.pdf);;All Files (*)'.",
        "state": "Opens source doc, ImportPagesCommand, onDocumentStructureChanged, status bar 'Imported N pages from <name>'.",
        "visible": "Page count grows; canvas refreshes.",
        "errors": "User cancel (no-op); empty source → QMessageBox::warning; std::exception → QMessageBox::critical.",
        "visual": "YES",
    },
    "MainWindow::onExtractPages": {
        "input": "QFileDialog save path (*.pdf).",
        "state": "extractPagesToFile() — pure read on doc, write to outputPath.",
        "visible": "Status bar 'Extracted N page(s) to <name>'.",
        "errors": "User cancel; std::exception → QMessageBox::critical 'Extract Error'.",
        "visual": "NO — output file external to app window.",
    },
    "MainWindow::onMovePageUp": {
        "input": "N/A.",
        "state": "MovePageCommand(from, from-1); onDocumentStructureChanged; canvas_->goToPage(from-1).",
        "visible": "Page panel reorder; canvas navigates.",
        "errors": "from <= 0 → no-op.",
        "visual": "YES",
    },
    "MainWindow::onMovePageDown": {
        "input": "N/A.",
        "state": "MovePageCommand(from, from+1).",
        "visible": "Page panel reorder; canvas navigates.",
        "errors": "from >= pageCount-1 → no-op.",
        "visual": "YES",
    },
    "MainWindow::onPageMoved": {
        "input": "(int from, int to) — fired by PagePanel drag-drop.",
        "state": "MovePageCommand; canvas_->goToPage(adjusted-to).",
        "visible": "Page list reorder + canvas refresh.",
        "errors": "from == to → no-op.",
        "visual": "YES",
    },
    "MainWindow::onTextEditToolSelected": {
        "input": "Triggered when textEditToolAction is checked/unchecked.",
        "state": "If checked: skipGroupTextEditCall_=true; activateTextEditForPage(currentPage). If unchecked: textEditAutoRouting_=false.",
        "visible": "Canvas tool indicator changes; either TextEditTool or TreRegionTool becomes active depending on page hasTextContent().",
        "errors": "No document/canvas → no-op.",
        "visual": "YES",
    },
    "MainWindow::onFormToolSelected": {
        "input": "N/A.",
        "state": "If checked: onAnnotationToolSelected(formToolAction_).",
        "visible": "Form tool active in canvas; form toolbar visibility unchanged.",
        "errors": "No formManager_ → no-op.",
        "visual": "YES",
    },
    "MainWindow::onCreateFormField": {
        "input": "N/A.",
        "state": "formToolAction_->setChecked(true); onFormToolSelected(); formToolBar_->setVisible(true).",
        "visible": "Form toolbar appears with 7 field-type buttons.",
        "errors": "None.",
        "visual": "YES",
    },
    "MainWindow::onFormFieldProperties": {
        "input": "N/A.",
        "state": "propertiesPanelDock_->setVisible(true)+raise().",
        "visible": "Properties dock visible & raised.",
        "errors": "None.",
        "visual": "YES",
    },
    "MainWindow::onExportFormData": {
        "input": "QFileDialog 'XFDF Files (*.xfdf);;All Files (*)'.",
        "state": "FormDataIO::exportToXfdf → write file.",
        "visible": "Status bar 'Form data exported to <path>' for 3000 ms.",
        "errors": "User cancel; QFile open fails → QMessageBox::critical; exportToXfdf throws → QMessageBox::critical.",
        "visual": "NO",
    },
    "MainWindow::onImportFormData": {
        "input": "QFileDialog 'XFDF Files (*.xfdf)'.",
        "state": "Reads file; FormDataIO::importFromXfdf; canvas refresh.",
        "visible": "Form fields populated; canvas re-renders.",
        "errors": "User cancel; read fail → QMessageBox::critical; import fail → QMessageBox::critical.",
        "visual": "YES",
    },
    "MainWindow::onResetForm": {
        "input": "Confirm dialog Yes/No.",
        "state": "ResetFormCommand on current page (only fields with /DV reset due to MuPDF behavior); canvas->refreshPage; formPropertiesPanel->clear.",
        "visible": "Form widgets revert; status bar 'Form fields reset'.",
        "errors": "User clicks No → no-op; std::exception → QMessageBox::critical.",
        "visual": "YES",
    },
    "MainWindow::onFlattenForm": {
        "input": "Confirm warning dialog Yes/No.",
        "state": "formManager_->flattenForm; clears undo stack (irreversible); canvas refresh.",
        "visible": "Form widgets become static appearance; status bar 'Form fields flattened'.",
        "errors": "User clicks No; std::exception → QMessageBox::critical.",
        "visual": "YES",
    },
    "MainWindow::onFormFieldSelected": {
        "input": "(int widgetIndex).",
        "state": "FormManager::getFormField → formPropertiesPanel_->setFormField; raise dock.",
        "visible": "Properties panel populated; dock raised.",
        "errors": "No page → no-op.",
        "visual": "YES",
    },
    "MainWindow::onFormFieldDeselected": {
        "input": "N/A.",
        "state": "formPropertiesPanel_->clear().",
        "visible": "Properties panel emptied.",
        "errors": "None.",
        "visual": "YES",
    },
    "MainWindow::onFormFieldValueChanged": {
        "input": "FormFieldData (widgetIndex + value).",
        "state": "SetFieldValueCommand via undoManager; canvas refresh.",
        "visible": "Form widget appearance updates; window title gets '*'.",
        "errors": "No page → no-op.",
        "visual": "YES",
    },
    "MainWindow::onAnnotationToolSelected": {
        "input": "QAction* — exclusive annotation tool group.",
        "state": "Switches the active tool installed on PdfCanvas (Select/StickyNote/Highlight/Underline/StrikeOut/FreeText/Ink/Rectangle/Circle/Line/Stamp/FileAttachment/TextEdit/TRE/Form/Image/Redact); reads currentAnnotColor_; sets toolPage_.",
        "visible": "Canvas cursor + tool overlay changes; selected toolbar button highlighted; some tools reveal sub-toolbars (TextEdit, Form).",
        "errors": "No document → no-op.",
        "visual": "YES",
    },
    "MainWindow::onImportImageStamp": {
        "input": "QFileDialog 'PNG/JPG/BMP'.",
        "state": "ImageStampLibrary::addStamp + saveToSettings.",
        "visible": "File > Stamps menu rebuilds on next aboutToShow with the new stamp.",
        "errors": "User cancel; load fail → silent.",
        "visual": "DEPENDS — visible only when user reopens File > Stamps.",
    },
    "MainWindow::onAnnotationColorPicker": {
        "input": "QColorDialog modal.",
        "state": "currentAnnotColor_ updated; colorPickerButton_ stylesheet refreshed; current tool->setColor() if any.",
        "visible": "Color picker swatch updates; subsequent annotations use new color.",
        "errors": "User cancel → no change.",
        "visual": "YES",
    },
    "MainWindow::onStickyNoteDoubleClicked": {
        "input": "(int pageIndex, int annotIndex, AnnotationData).",
        "state": "stickyNotePopup_->showFor() — positions popup; sets text content; emits popupClosed/contentsChanged.",
        "visible": "StickyNotePopup widget appears near the icon.",
        "errors": "Popup null → no-op.",
        "visual": "YES",
    },
    "MainWindow::onStickyNotePopupContentsChanged": {
        "input": "AnnotationData.",
        "state": "ModifyAnnotationCommand; status bar 'Sticky note updated'.",
        "visible": "Annotation contents updated; window title gets '*'.",
        "errors": "No annotation manager → no-op.",
        "visual": "YES",
    },
    "MainWindow::onStickyNotePopupClosed": {
        "input": "(int pageIndex, int annotIndex).",
        "state": "Reposition state cleared.",
        "visible": "Popup hidden.",
        "errors": "None.",
        "visual": "YES",
    },
    "MainWindow::applyAtlasMode": {
        "input": "AtlasMode enum (ScanCropped / FreeTypeStyled).",
        "state": "currentAtlasMode_ = mode; if pendingAtlas_.ready, EnrichedAtlas->setMode(); canvas->update.",
        "visible": "Edit-text overlay re-renders glyphs in chosen atlas mode.",
        "errors": "Pending atlas not ready → no-op.",
        "visual": "YES",
    },
    "MainWindow::showAtlasComparison": {
        "input": "N/A.",
        "state": "AtlasComparisonDialog modal — renders both modes side-by-side.",
        "visible": "Modal dialog with comparison preview.",
        "errors": "Pending atlas not ready → no-op.",
        "visual": "YES",
    },
    "MainWindow::onToggleStickyNotes": {
        "input": "(bool checked) from QAction::toggled.",
        "state": "canvas_->setStickyNotesVisible(checked).",
        "visible": "Sticky note icons appear/disappear on canvas.",
        "errors": "No canvas → no-op.",
        "visual": "YES",
    },
    "MainWindow::onToggleCopyPrevention": {
        "input": "(bool checked).",
        "state": "Sets ALL_KEY copy permission on document_; not persisted unless saved.",
        "visible": "Copy action might be disabled in subsequent operations; no immediate visible change.",
        "errors": "None.",
        "visual": "NO",
    },
    "MainWindow::onTogglePrintPrevention": {
        "input": "(bool checked).",
        "state": "Sets print permission flag on document_.",
        "visible": "No immediate visible change; affects subsequent print attempts.",
        "errors": "None.",
        "visual": "NO",
    },
    "MainWindow::onToggleOcrBoxes": {
        "input": "(bool checked).",
        "state": "ocrBoxesEnabled_ = checked; loadOcrBoxesForPage(currentPage); canvas->update.",
        "visible": "Canvas overlays per-character OCR bounding boxes.",
        "errors": "No OCR pipeline → silently disabled.",
        "visual": "YES",
    },
    "MainWindow::onCommentPanelNavigate": {
        "input": "(int pageIndex).",
        "state": "canvas_->goToPage(pageIndex).",
        "visible": "Canvas navigates; comment panel selection follows.",
        "errors": "None.",
        "visual": "YES",
    },
    "MainWindow::onCommentPanelDelete": {
        "input": "(int pageIndex, int annotIndex).",
        "state": "DeleteAnnotationCommand; commentPanel refresh.",
        "visible": "Annotation removed from canvas + panel; window title gets '*'.",
        "errors": "No annotation manager → no-op.",
        "visual": "YES",
    },
    "MainWindow::onAnnotationPropertiesChanged": {
        "input": "AnnotationData.",
        "state": "ModifyAnnotationCommand; canvas refresh.",
        "visible": "Annotation appearance updates.",
        "errors": "No manager → no-op.",
        "visual": "YES",
    },
    "MainWindow::onAnnotationCreatedByTool": {
        "input": "(int pageIndex, AnnotationData).",
        "state": "Tool created the annotation directly; this slot deletes the raw annotation and re-creates via CreateAnnotationCommand so undo works.",
        "visible": "Annotation persists with undo entry.",
        "errors": "No undo manager → leaves raw annotation in place.",
        "visual": "YES",
    },
    "MainWindow::onTreRegionSelected": {
        "input": "(int pageIndex, QRectF pdfRect).",
        "state": "regionReconstructor_->analyze; TreEditDialog modal; ReconstructRegionCommand or ImageReplaceCommand depending on user choice; canvas->refreshPage.",
        "visible": "TRE edit dialog modal; on Accept, canvas region re-renders with new text.",
        "errors": "Analysis fail → QMessageBox::warning 'Could not recognize text'; user cancel.",
        "visual": "YES",
    },
    "MainWindow::onRedactToolSelected": {
        "input": "N/A.",
        "state": "If checked: onAnnotationToolSelected(redactToolAction_); redactionPanelDock_->show(). Else: revert to selectToolAction_.",
        "visible": "Redaction tool active in canvas; redaction panel visible.",
        "errors": "No document → no-op.",
        "visual": "YES",
    },
    "MainWindow::onMarkRedaction": {
        "input": "N/A.",
        "state": "Sets redactToolAction_ checked + onRedactToolSelected(); marking happens via tool interaction.",
        "visible": "Same as onRedactToolSelected.",
        "errors": "None — direct passthrough.",
        "visual": "YES",
    },
    "MainWindow::onApplyRedactions": {
        "input": "Confirm warning dialog Yes/No.",
        "state": "applyAllRedactions(rewriteDocument=false); document_->save; QPDF rewrite; reopen; clear undo (irreversible); applyRedactionsAction disabled.",
        "visible": "Canvas re-renders with redacted content; status bar; QMessageBox 'Successfully applied N redaction(s)'.",
        "errors": "No marks → 'No Redactions' info; user clicks No → no-op; save fail → QMessageBox::critical 'Redaction Save Error'; QPDF rewrite fail → fallback reopen original.",
        "visual": "YES",
    },
    "MainWindow::onDeleteRedactionMark": {
        "input": "(int pageIndex, int annotIndex).",
        "state": "DeleteRedactionCommand; redaction panel refresh.",
        "visible": "Mark removed; panel updates.",
        "errors": "No manager → no-op.",
        "visual": "YES",
    },
    "MainWindow::onRedactionMarkSelected": {
        "input": "(int pageIndex, _).",
        "state": "canvas_->goToPage.",
        "visible": "Canvas navigates.",
        "errors": "None.",
        "visual": "YES",
    },
    "MainWindow::onSaveWithEncryption": {
        "input": "EncryptionDialog modal (algorithm, key length, user/owner passwords, permissions).",
        "state": "EncryptionManager::saveEncrypted(document_, outputPath, settings) OR removeEncryption if algorithm==None.",
        "visible": "EncryptionDialog modal; QFileDialog save; status bar 'File saved with AES-256 encryption to <name>'.",
        "errors": "User cancel either dialog; save throws → QMessageBox::critical 'Encryption Error'.",
        "visual": "YES",
    },
    "MainWindow::onDocumentPermissions": {
        "input": "N/A.",
        "state": "Reads encryption info; QMessageBox::information with permissions table.",
        "visible": "Info QMessageBox with permission summary.",
        "errors": "Read fail → QMessageBox::critical.",
        "visual": "YES",
    },
    "MainWindow::onRemovePassword": {
        "input": "QFileDialog save path.",
        "state": "encryptionManager_->removeEncryption(document_, outputPath).",
        "visible": "Status bar 'Unencrypted copy saved to <name>'.",
        "errors": "User cancel; std::exception → QMessageBox::critical 'Remove Password Error'.",
        "visual": "NO",
    },
    "MainWindow::onSignDocument": {
        "input": "SignDialog modal — pfx path/password, reason, location, appearance flags, TSA, certification.",
        "state": "If no unsigned field: CreateSignatureFieldCommand. SaveAs to chosen path (Document::saveAs) + reopen; signatureManager_->signField with options; reopen post-signature; signaturePanel_ updated.",
        "visible": "SignDialog modal; QFileDialog 'Save Signed PDF'; signature dock shown; status bar 'Document signed successfully'.",
        "errors": "User cancel any step; signField throws → QMessageBox::critical 'Signing Error'.",
        "visual": "YES",
    },
    "MainWindow::onVerifySignatures": {
        "input": "N/A.",
        "state": "verifyAllSignatures; LtvValidator::verify; signaturePanel update; requestValidationAction enabled iff validCount>0.",
        "visible": "Signature dock shown; QMessageBox::information with summary.",
        "errors": "No signatures → 'no signatures' info; verify throws → QMessageBox::critical.",
        "visual": "YES",
    },
    "MainWindow::onRequestValidation": {
        "input": "ValidationCoordinator modal consent dialog (then async network).",
        "state": "External OCSP/CRL fetch; embedded in /DSS; ValidationCoordinator::completed/cancelled/failed signals.",
        "visible": "Modal consent dialog; status bar 'Validation data embedded' / 'Validation request cancelled'.",
        "errors": "Coordinator request() returns false (no consent) → cleanup; failed signal → cleanup.",
        "visual": "YES",
    },
    "MainWindow::onImageToolSelected": {
        "input": "N/A.",
        "state": "Lazy-creates ImageTool; wires imageSelected/imageDeselected/removeRequested/resizeRequested/rotateRequested/cropRequested signals; setPage; canvas_->setImageTool.",
        "visible": "Image tool active; image properties dock shown when selecting image.",
        "errors": "No imageManager_ → no-op.",
        "visual": "YES",
    },
    "MainWindow::onInsertImage": {
        "input": "QFileDialog 'PNG/JPG/JPEG/BMP/GIF/TIFF;All Files'.",
        "state": "InsertImageCommand with default 72,72,400,300 placement and stripExif=true; canvas refresh.",
        "visible": "Inserted image visible on page.",
        "errors": "User cancel; no page → no-op.",
        "visual": "YES",
    },
    "MainWindow::onStripExif": {
        "input": "N/A.",
        "state": "exifStripper_->stripAllImages; canvas refresh.",
        "visible": "QMessageBox::information 'Stripped metadata from N image(s)' or 'No images found'.",
        "errors": "None — Stripper handles internally.",
        "visual": "YES",
    },
    "MainWindow::onDocumentMetadata": {
        "input": "MetadataDialog modal — title/author/subject/keywords/creator/producer fields.",
        "state": "EditMetadataCommand on Accept.",
        "visible": "MetadataDialog modal; window title may gain '*'.",
        "errors": "None.",
        "visual": "YES",
    },
    "MainWindow::onCompliance": {
        "input": "N/A.",
        "state": "ComplianceDialog modal exec().",
        "visible": "Compliance dialog modal.",
        "errors": "No document → no-op.",
        "visual": "YES",
    },
    "MainWindow::onImageRemove": {
        "input": "N/A.",
        "state": "Emits imageTool_->removeRequested(imageIndex) → RemoveImageCommand.",
        "visible": "Image disappears from canvas.",
        "errors": "No selection → no-op.",
        "visual": "YES",
    },
    "MainWindow::onImageRotate90": {"input": "N/A.", "state": "imageTool_->rotateRequested(name, 90).", "visible": "Image rotated 90.", "errors": "No selection.", "visual": "YES"},
    "MainWindow::onImageRotate180": {"input": "N/A.", "state": "imageTool_->rotateRequested(name, 180).", "visible": "Image rotated 180.", "errors": "No selection.", "visual": "YES"},
    "MainWindow::onImageRotate270": {"input": "N/A.", "state": "imageTool_->rotateRequested(name, 270).", "visible": "Image rotated 270.", "errors": "No selection.", "visual": "YES"},
    "MainWindow::onImageCrop": {"input": "N/A — uses center 80% as default.", "state": "imageTool_->cropRequested(name, QRectF).", "visible": "Image cropped.", "errors": "No selection.", "visual": "YES"},
    "MainWindow::onImageResize": {"input": "N/A — 50% scale default.", "state": "imageTool_->resizeRequested(name, w, h, x, y).", "visible": "Image resized.", "errors": "No selection.", "visual": "YES"},
    "MainWindow::onCombineFiles": {"input": "CombineFilesWizard interactive.", "state": "Wizard runs CombineFilesService.", "visible": "Wizard modal.", "errors": "User cancel.", "visual": "YES"},
    "MainWindow::onCustomizeToolbars": {"input": "CustomizeToolbarsDialog modal.", "state": "ToolbarLayoutManager::saveLayouts; applyToolbarLayoutsList.", "visible": "Toolbars rearrange.", "errors": "User cancel.", "visual": "YES"},
    "BookmarkPanel::onCreateAtRoot": {"input": "QInputDialog::getText 'Bookmark title'.", "state": "CreateBookmarkCommand at root level; refresh tree.", "visible": "New bookmark in tree.", "errors": "User cancel.", "visual": "YES"},
    "BookmarkPanel::onCreateChild": {"input": "QInputDialog title.", "state": "CreateBookmarkCommand under selected.", "visible": "Child added.", "errors": "No selection / cancel.", "visual": "YES"},
    "BookmarkPanel::onRename": {"input": "QInputDialog title.", "state": "RenameBookmarkCommand.", "visible": "Tree row text updates.", "errors": "Empty name → no-op.", "visual": "YES"},
    "BookmarkPanel::onDelete": {"input": "Confirm Yes/No.", "state": "DeleteBookmarkCommand.", "visible": "Tree row removed.", "errors": "User No.", "visual": "YES"},
    "BookmarkPanel::onMoveUp": {"input": "N/A.", "state": "MoveBookmarkCommand up.", "visible": "Tree reorder.", "errors": "Already first.", "visual": "YES"},
    "BookmarkPanel::onMoveDown": {"input": "N/A.", "state": "MoveBookmarkCommand down.", "visible": "Tree reorder.", "errors": "Already last.", "visual": "YES"},
    "BookmarkPanel::onIndent": {"input": "N/A.", "state": "IndentBookmarkCommand.", "visible": "Tree depth changes.", "errors": "Cannot indent root-without-prev-sibling.", "visual": "YES"},
    "BookmarkPanel::onOutdent": {"input": "N/A.", "state": "OutdentBookmarkCommand.", "visible": "Tree depth changes.", "errors": "Already at root.", "visual": "YES"},
    "BookmarkPanel::onItemActivated": {"input": "QTreeWidgetItem* + column.", "state": "Emits bookmarkActivated(pageIndex,x,y,zoom) or externalLinkRequested(uri).", "visible": "Canvas navigates to bookmark target.", "errors": "Invalid type role → no-op.", "visual": "YES"},
    "BookmarkPanel::onItemChanged": {"input": "QTreeWidgetItem* + column 0 text.", "state": "RenameBookmarkCommand if title changed.", "visible": "Bookmark renamed.", "errors": "Suppressed during programmatic refresh.", "visual": "YES"},
    "BookmarkPanel::onContextMenu": {"input": "QPoint pos.", "state": "Builds bookmarkContextMenu and exec()s.", "visible": "Context menu appears.", "errors": "None.", "visual": "YES"},
    # SearchPanel slots
    "SearchPanel::onSearchClicked": {"input": "Search line edit text + case/whole-word checkboxes.", "state": "Searcher::search across pages; populates results list; emits resultSelected.", "visible": "Results list populated; canvas highlights first hit.", "errors": "Empty query → status 'No matches'.", "visual": "YES"},
    "SearchPanel::onResultSelected": {"input": "Selected result row.", "state": "Emits resultSelected(pageIndex, quads); MainWindow navigates.", "visible": "Canvas navigates.", "errors": "None.", "visual": "YES"},
    "SearchPanel::onReplaceClicked": {"input": "Find + replace text.", "state": "Calls MainWindow::onReplace via signal.", "visible": "Canvas re-renders if replaced.", "errors": "Empty find.", "visual": "YES"},
    "SearchPanel::onReplaceAllClicked": {"input": "Find + replace text.", "state": "Calls MainWindow::onReplaceAll.", "visible": "Canvas re-renders.", "errors": "Empty find.", "visual": "YES"},
    # Common
    "QDialog::accept": {"input": "Triggered by OK / accept button.", "state": "Sets dialog result Accepted; closes dialog; caller reads dialog state via accessor methods.", "visible": "Dialog disappears.", "errors": "None.", "visual": "YES"},
    "QDialog::reject": {"input": "Triggered by Cancel / Esc / Close.", "state": "Sets dialog result Rejected; closes dialog without committing changes.", "visible": "Dialog disappears.", "errors": "None.", "visual": "YES"},
    "PrintDialog::onRangeChanged": {"input": "QRadioButton/QSpinBox.", "state": "Updates internal PrintOptions.range; re-validates From/To inputs.", "visible": "From/To spinboxes enable/disable.", "errors": "Invalid range → OK button disabled.", "visual": "YES"},
    "SignDialog::validateInput": {"input": "PFX path + password line edit text.", "state": "Toggles okButton enabled state; sets validationLabel red text.", "visible": "Validation label text + Sign button enabled state.", "errors": "Empty path → 'PFX certificate file is required'; empty pwd → 'Certificate password is required'.", "visual": "YES"},
}

# --------------------------------------------------------------------------- #
# 1b. Known wiring missed by phase-1 catalog (multi-line connect() statements,
#     QActionGroup-only connections, etc). Maps objectName → slot/lambda info.
# --------------------------------------------------------------------------- #

KNOWN_WIRING = {
    # MainWindow.cpp — direct connects spanning multiple lines (catalog regex
    # parsed only single-line connect statements).
    "exportPlainTextAction":      "MainWindow::onExportPlainText",
    "createPdfFromImagesAction":  "MainWindow::onCreatePdfFromImages",
    "combineFilesAction":         "MainWindow::onCombineFiles",
    "previousViewAction":         "MainWindow::onPreviousView",
    "nextViewAction":             "MainWindow::onNextView",
    "imageToolAction":            "MainWindow::onAnnotationToolSelected",  # via annotToolGroup_
    "toggleStickyNotesAction":    "MainWindow::onToggleStickyNotes",
    "toggleCopyPreventionAction": "MainWindow::onToggleCopyPrevention",
    "togglePrintPreventionAction": "MainWindow::onTogglePrintPrevention",
    "toggleOcrBoxesAction":       "MainWindow::onToggleOcrBoxes",
    "customizeToolbarsAction":    "MainWindow::onCustomizeToolbars",
    # Annotation tool group — all dispatch through onAnnotationToolSelected
    "selectToolAction":           "MainWindow::onAnnotationToolSelected",
    "stickyNoteToolAction":       "MainWindow::onAnnotationToolSelected",
    "highlightToolAction":        "MainWindow::onAnnotationToolSelected",
    "underlineToolAction":        "MainWindow::onAnnotationToolSelected",
    "strikeOutToolAction":        "MainWindow::onAnnotationToolSelected",
    "inkToolAction":              "MainWindow::onAnnotationToolSelected",
    "freeTextToolAction":         "MainWindow::onAnnotationToolSelected",
    "rectangleToolAction":        "MainWindow::onAnnotationToolSelected",
    "circleToolAction":           "MainWindow::onAnnotationToolSelected",
    "lineToolAction":             "MainWindow::onAnnotationToolSelected",
    "fileAttachmentToolAction":   "MainWindow::onAnnotationToolSelected",  # plus inline lambda for source file picker
    # Internal-only actions (no triggered connection — invoked programmatically)
    "treToolAction":              "@internal — programmatically activated by activateTextEditForPage() routing",
    "requestValidationAction":    "MainWindow::onRequestValidation",  # connected via direct & — phase-1 missed
    # textEditAtlasScanAction / textEditAtlasFontAction connect via inline
    # lambdas → MainWindow::applyAtlasMode (multi-line connect missed)
    "textEditAtlasScanAction":    "MainWindow::applyAtlasMode",
    "textEditAtlasFontAction":    "MainWindow::applyAtlasMode",
}

# Override the visible/state/error annotations for a few of these so we don't
# label them generically as "no slot connection".
KNOWN_WIRING_NOTES = {
    "imageToolAction": "Member of annotToolGroup_ (QActionGroup) — exclusive selection group dispatches via QActionGroup::triggered → MainWindow::onAnnotationToolSelected; also triggers MainWindow::onImageToolSelected as a side effect on PdfCanvas tool installation.",
    "selectToolAction": "Annotation tool group; activates SelectTool on PdfCanvas.",
    "stickyNoteToolAction": "Annotation tool group; activates StickyNoteTool on PdfCanvas.",
    "highlightToolAction": "Annotation tool group; activates HighlightTool (TextMarkupTool variant) on PdfCanvas.",
    "underlineToolAction": "Annotation tool group; activates UnderlineTool on PdfCanvas.",
    "strikeOutToolAction": "Annotation tool group; activates StrikeOutTool on PdfCanvas.",
    "inkToolAction": "Annotation tool group; activates InkTool on PdfCanvas.",
    "freeTextToolAction": "Annotation tool group; activates FreeTextTool on PdfCanvas (creates /FreeText annotation).",
    "rectangleToolAction": "Annotation tool group; activates RectangleTool on PdfCanvas (Square annotation).",
    "circleToolAction": "Annotation tool group; activates CircleTool on PdfCanvas (Circle annotation).",
    "lineToolAction": "Annotation tool group; activates LineTool on PdfCanvas.",
    "fileAttachmentToolAction": "Annotation tool group; inline lambda prompts for source file + icon style, then activates FileAttachmentTool on PdfCanvas.",
    "treToolAction": "No QAction::triggered handler — programmatically toggled by activateTextEditForPage() when current page lacks native text content. The Edit Text tool routes here for scanned-only pages.",
}

# --------------------------------------------------------------------------- #
# 2. Per-dialog metadata — what each dialog does at exec()-time + key inputs  #
# --------------------------------------------------------------------------- #

DIALOG_INFO = {
    "AboutDialog": {
        "purpose": "Static About-PDFE dialog; renders bundled about.md.",
        "inputs": "N/A — read-only.",
        "outputs": "QDialog::accept on Close.",
        "visual": "YES",
    },
    "AccessibilityHelper": {
        "purpose": "Helper class — populates accessible names/roles on widgets.",
        "inputs": "QWidget*.",
        "outputs": "Side-effects on target widget tree.",
        "visual": "NO",
    },
    "AltTextDialog": {
        "purpose": "Edit /Contents (alt text) and /ActualText for a single annotation.",
        "inputs": "altTextEdit (QPlainTextEdit), actualTextEdit (QPlainTextEdit).",
        "outputs": "Annotations updated via AnnotationManager::setAltText/setActualText on accept.",
        "visual": "YES",
    },
    "AnnotationPropertiesWidget": {
        "purpose": "Side panel for editing color/opacity/author/contents of a selected annotation.",
        "inputs": "colorButton, opacitySlider, authorEdit, contentsEdit.",
        "outputs": "Emits propertiesChanged → MainWindow::onAnnotationPropertiesChanged → ModifyAnnotationCommand.",
        "visual": "YES",
    },
    "AtlasComparisonDialog": {
        "purpose": "Shows ScanCropped vs FreeTypeStyled atlas-rendered overlays side-by-side for the active span.",
        "inputs": "scanRadio / fontRadio.",
        "outputs": "Selection emits modeSelected; MainWindow::applyAtlasMode.",
        "visual": "YES",
    },
    "AttachmentPanel": {
        "purpose": "Side dock listing /EmbeddedFiles entries.",
        "inputs": "attachmentTree double-click → extract.",
        "outputs": "Emits requestExtract; right-click context menu.",
        "visual": "YES",
    },
    "BatchDialog": {
        "purpose": "Build & run a batch action list across multiple files.",
        "inputs": "addActionButton/removeActionButton/files list/output dir/owner password.",
        "outputs": "BatchProcessor runs; per-file results list updated.",
        "visual": "YES",
    },
    "BookmarkPanel": {
        "purpose": "Tree view + context menu for /Outlines.",
        "inputs": "QTreeWidget activate / drag-drop.",
        "outputs": "BookmarkManager mutations via undo.",
        "visual": "YES",
    },
    "CalculationOrderDialog": {
        "purpose": "Edit form-wide /AcroForm /CO array (calculation order).",
        "inputs": "fieldList QListWidget; up/down/apply buttons.",
        "outputs": "FormManager::setCalculationOrder on Apply.",
        "visual": "YES",
    },
    "CombineFilesService": {
        "purpose": "Backend service to merge PDFs/images into one document.",
        "inputs": "CombineFilesOptions (input list, page ranges, output path).",
        "outputs": "Writes merged PDF.",
        "visual": "NO",
    },
    "CombineFilesWizard": {
        "purpose": "Multi-step wizard around CombineFilesService.",
        "inputs": "filesList add/remove buttons; output path.",
        "outputs": "Service invocation on Finish.",
        "visual": "YES",
    },
    "CommentPanel": {
        "purpose": "Tree view of all annotations grouped by page; filter/sort.",
        "inputs": "filterBox QLineEdit; sortCombo; tree double-click.",
        "outputs": "Emits navigateToAnnotation / deleteAnnotation.",
        "visual": "YES",
    },
    "CompareDialog": {
        "purpose": "Compare two PDFs page-by-page.",
        "inputs": "File pickers for A/B; threshold spinbox.",
        "outputs": "Visual diff side-by-side.",
        "visual": "YES",
    },
    "ComplianceDialog": {
        "purpose": "Check + convert to PDF/A and PDF/UA.",
        "inputs": "profileCombo (PDF/A-1b/2b/3b, UA-1); convertButton.",
        "outputs": "ComplianceManager::validate + convert.",
        "visual": "YES",
    },
    "CustomizeToolbarsDialog": {
        "purpose": "Add/remove/reorder toolbar buttons across all toolbars.",
        "inputs": "availableList, currentList, addButton, removeButton, upButton, downButton, applyButton.",
        "outputs": "ToolbarLayoutManager::saveLayouts.",
        "visual": "YES",
    },
    "DefaultsLogPanel": {
        "purpose": "Side dock showing diagnostic messages for the active document.",
        "inputs": "clearButton; copyButton.",
        "outputs": "Read-only log.",
        "visual": "YES",
    },
    "EncryptionDialog": {
        "purpose": "Configure AES-128/256 user/owner password + permission flags.",
        "inputs": "algorithmCombo; userPasswordEdit; ownerPasswordEdit; permission checkboxes.",
        "outputs": "EncryptionSettings via settings() accessor on Accept.",
        "visual": "YES",
    },
    "FormFieldPropertiesPanel": {
        "purpose": "QStackedWidget side panel — different editor per field type.",
        "inputs": "Per-field-type widgets (text, checkbox, choice, etc.).",
        "outputs": "Emits valueChanged → SetFieldValueCommand.",
        "visual": "YES",
    },
    "HeaderFooterDialog": {
        "purpose": "Add headers/footers to all pages.",
        "inputs": "Six text fields (left/center/right × header/footer); fontCombo, sizeSpin, colorButton, marginSpin, pageRange.",
        "outputs": "HeaderFooterApplier::apply.",
        "visual": "YES",
    },
    "ImagePropertiesPanel": {
        "purpose": "Side panel showing dimensions/colorspace/bpp + rotate/crop/resize buttons.",
        "inputs": "rotateButton, cropButton, resizeButton, removeButton.",
        "outputs": "Routes to ImageTool actions.",
        "visual": "YES",
    },
    "ImageToPdfWizard": {
        "purpose": "Wizard converting a list of images into a new PDF.",
        "inputs": "imageList add/remove; pageSizeCombo; dpiSpin; jpegQualitySpin.",
        "outputs": "Returns std::filesystem::path of output PDF.",
        "visual": "YES",
    },
    "LayerPanel": {
        "purpose": "Side dock listing OCG layers; toggle visibility.",
        "inputs": "layerTree checkboxes.",
        "outputs": "OcgManager::setLayerVisible.",
        "visual": "YES",
    },
    "MainWindow": {
        "purpose": "Top-level QMainWindow — owns canvas, panels, all menus, the document, all *Manager instances.",
        "inputs": "All QActions, all menu items, drag-drop accepting application/x-pdf-page-id and text/uri-list (file:// PDFs).",
        "outputs": "See per-action shared-slots table.",
        "visual": "YES",
    },
    "MetadataDialog": {
        "purpose": "Edit document /Info dict.",
        "inputs": "title/author/subject/keywords/creator/producer line edits.",
        "outputs": "EditMetadataCommand on Accept.",
        "visual": "YES",
    },
    "NavigationHistory": {
        "purpose": "Helper class — back/forward stack of page visits.",
        "inputs": "recordVisit(pageIndex).",
        "outputs": "goBack/goForward return page index.",
        "visual": "NO",
    },
    "OptimizeDialog": {
        "purpose": "Reduce file size: subset fonts, downsample images, remove unused objects.",
        "inputs": "Per-option checkboxes + preview button.",
        "outputs": "OptimizationManager::optimize.",
        "visual": "YES",
    },
    "PageLabelsDialog": {
        "purpose": "Edit /PageLabels number tree (Roman/decimal/alpha/prefix).",
        "inputs": "rangeTable, addRangeButton, removeRangeButton, applyButton.",
        "outputs": "PageLabelsManager::setLabels.",
        "visual": "YES",
    },
    "PagePanel": {
        "purpose": "Side dock with page thumbnails + drag-drop reorder + context menu.",
        "inputs": "thumbnailList; right-click menu actions.",
        "outputs": "Emits pageSelected, pageMoved, contextMenuAction.",
        "visual": "YES",
    },
    "PasswordDialog": {
        "purpose": "Prompt for PDF password during open.",
        "inputs": "passwordEdit (echo Password).",
        "outputs": "password() accessor on Accept.",
        "visual": "YES",
    },
    "PdfCanvas": {
        "purpose": "Custom QWidget rendering page tiles + dispatching mouse/keyboard to active tool.",
        "inputs": "Mouse/keyboard events; setCurrentTool / setTextEditTool / setImageTool.",
        "outputs": "Emits pageChanged, zoomChanged, annotationDoubleClicked.",
        "visual": "YES",
    },
    "PdfCanvasAccessible": {
        "purpose": "QAccessibleInterface for PdfCanvas — exposes pages + annotations to UIA.",
        "inputs": "UIA tree queries.",
        "outputs": "Children list per page.",
        "visual": "NO — accessibility tree only.",
    },
    "PreferencesDialog": {
        "purpose": "App settings: theme, OCR language, default zoom, JS-enabled, recent files limit.",
        "inputs": "Each tab's controls — themeCombo, ocrLangCombo, zoomDefaultSpin, jsCheckBox, recentLimitSpin.",
        "outputs": "AppSettings updates on Accept.",
        "visual": "YES",
    },
    "PreflightDialog": {
        "purpose": "Per-rule pass/fail report for selected compliance profile.",
        "inputs": "profileCombo.",
        "outputs": "Read-only report tree.",
        "visual": "YES",
    },
    "PrintDialog": {
        "purpose": "PDFE-internal print configurator (range, copies, scaling, duplex).",
        "inputs": "rangeRadios, fromSpin, toSpin, copiesSpin, scalingCombo, duplexCombo.",
        "outputs": "options() accessor → PrintEngine.",
        "visual": "YES",
    },
    "PrintPreviewDialog": {
        "purpose": "QPrintPreview-style preview for PDFE rendering.",
        "inputs": "Zoom buttons, page nav, fit width/page actions.",
        "outputs": "Read-only preview.",
        "visual": "YES",
    },
    "PropertiesPanel": {
        "purpose": "Side dock — current page metadata (size, rotation, label).",
        "inputs": "Read-only page info.",
        "outputs": "Display only.",
        "visual": "YES",
    },
    "RecentFilesMenu": {
        "purpose": "QMenu subclass — recent-files list with clear action.",
        "inputs": "AppSettings::recentFiles().",
        "outputs": "Emits fileTriggered(path) on click.",
        "visual": "DEPENDS — visible on next File menu open.",
    },
    "RedactionPanel": {
        "purpose": "Side dock listing pending redaction marks; navigate/delete; Apply button.",
        "inputs": "marksTable, deleteButton, navigateButton, applyButton.",
        "outputs": "Emits navigateToMark / deleteMark / applyAll.",
        "visual": "YES",
    },
    "SearchPanel": {
        "purpose": "Find/replace dock.",
        "inputs": "searchInput, caseSensitiveCheck, wholeWordCheck, searchButton, replaceInput, replaceButton, replaceAllButton, resultsList.",
        "outputs": "Emits searchResultSelected, replaceRequested, replaceAllRequested.",
        "visual": "YES",
    },
    "SignDialog": {
        "purpose": "Configure signature: PFX path/pwd, reason, location, appearance flags, TSA, certification level.",
        "inputs": "pfxPathEdit, pfxPasswordEdit, reasonEdit, locationEdit, showLabels/Name/Date/Dn/Logo checkboxes, tsaCheckBox+tsaUrlEdit, invisibleCheckBox, certificationLevelCombo.",
        "outputs": "signingOptions() accessor → SigningOptions struct.",
        "visual": "YES",
    },
    "SignaturePanel": {
        "purpose": "Side dock listing signature widgets + LTV status.",
        "inputs": "signatureTable, verifyButton, requestValidationButton.",
        "outputs": "Emits navigateToSignature.",
        "visual": "YES",
    },
    "StatusBar": {
        "purpose": "Bottom bar: filename + page count + zoom + encrypted icon + form-field count.",
        "inputs": "Setters from MainWindow.",
        "outputs": "Read-only display.",
        "visual": "YES",
    },
    "StickyNotePopup": {
        "purpose": "Floating popup near a clicked sticky-note icon for inline content editing.",
        "inputs": "contentsEdit QPlainTextEdit.",
        "outputs": "Emits contentsChanged, popupClosed.",
        "visual": "YES",
    },
    "TabOrderDialog": {
        "purpose": "Edit per-page form-field tab order.",
        "inputs": "fieldsList QListWidget + up/down buttons.",
        "outputs": "newTabOrder() accessor on Accept.",
        "visual": "YES",
    },
    "TextEditToolBar": {
        "purpose": "Sub-toolbar shown when TextEditTool active — font family/size, bold/italic/underline, color, alignment.",
        "inputs": "fontCombo, sizeSpin, boldButton, italicButton, underlineButton, colorButton, alignButtons (left/center/right/justify).",
        "outputs": "Emits formattingChanged → TextEditTool.",
        "visual": "YES",
    },
    "ToolbarLayoutManager": {
        "purpose": "Helper class — load/save QToolBar action layouts via QSettings.",
        "inputs": "QToolBar*, ToolbarLayout struct.",
        "outputs": "Side-effects on QSettings.",
        "visual": "NO",
    },
    "TrustAnchorDialog": {
        "purpose": "Manage trusted root CA certificates (offline LTV validation).",
        "inputs": "anchorList, addButton, removeButton, exportButton.",
        "outputs": "TrustAnchorStore mutations.",
        "visual": "YES",
    },
    "ValidationRequestDialog": {
        "purpose": "Pre-fetch consent dialog for OCSP/CRL network requests.",
        "inputs": "consentCheck, proceedButton.",
        "outputs": "Emits consented(bool).",
        "visual": "YES",
    },
    "WatermarkDialog": {
        "purpose": "Add text watermark to all pages.",
        "inputs": "textEdit, fontCombo, sizeSpin, colorButton, opacitySpin, rotationSpin, applyButton.",
        "outputs": "WatermarkApplier::apply.",
        "visual": "YES",
    },
}

# --------------------------------------------------------------------------- #
# 3. Per-widget heuristics for input_range based on Qt class                  #
# --------------------------------------------------------------------------- #

# input_range default suggestions per Qt widget type. These represent the
# baseline interpretation; specific dialogs may have narrower ranges noted
# inline in the dialog summary above.
WIDGET_TYPE_INPUT = {
    "QPushButton":      "N/A — button click only (no text input).",
    "QToolButton":      "N/A — button click only.",
    "QCheckBox":        "Boolean (checked/unchecked); supports tri-state if explicitly set.",
    "QRadioButton":     "Boolean within an exclusive QButtonGroup (one-of-N).",
    "QLineEdit":        "Free-form QString; validator/echo-mode per dialog (passwords use Password echo).",
    "QPlainTextEdit":   "Multi-line QString; no validator.",
    "QTextEdit":        "Rich-text or plain-text QString.",
    "QTextBrowser":     "Read-only HTML; no input.",
    "QSpinBox":         "Integer in [minimum,maximum] with step (defaults: 0..99 step 1 unless overridden).",
    "QDoubleSpinBox":   "Double in [minimum,maximum] with step + decimals.",
    "QSlider":          "Integer in [minimum,maximum]; orientation horizontal/vertical.",
    "QComboBox":        "One of items() (editable variant accepts free-form QString).",
    "QFontComboBox":    "Available system fonts (QFontDatabase::families()).",
    "QListWidget":      "Selection of one or more items.",
    "QTreeWidget":      "Selection of tree items; supports drag-drop if InternalMove enabled.",
    "QTableWidget":     "Cell selection / per-cell editor delegate.",
    "QDialogButtonBox": "Button-roles (Ok/Cancel/Apply/Close/Help).",
    "QGroupBox":        "Container only — no input.",
    "QLabel":           "Read-only display.",
    "QProgressBar":     "Read-only [0..100] progress.",
    "QStackedWidget":   "Container — switches by setCurrentIndex().",
    "QTabWidget":       "Selects one tab; per-tab content is its own subtree.",
    "QToolBar":         "Container of QActions/QWidgets.",
    "QMenu":            "Container of QActions.",
    "QMenuBar":         "Container of QMenus.",
    "QStatusBar":       "Display-only.",
    "QDockWidget":      "Container — has visibility + dock-area state.",
    "QDateEdit":        "QDate in [minimumDate,maximumDate].",
    "QDateTimeEdit":    "QDateTime in range.",
    "QFileDialog":      "File path(s); filter per call site.",
    "QInputDialog":     "Per-call (text/int/double/item).",
    "QMessageBox":      "Standard buttons; returns chosen role.",
    "QColorDialog":     "QColor selection.",
    "QProgressDialog": "Cancel button.",
    # Custom widgets in PDFE
    "PdfCanvas":        "Mouse + keyboard events dispatched to active tool; accepts drag-drop of file:// URLs and application/x-pdf-page-id MIME.",
    "PagePanel":        "Thumbnail click / drag-drop / context menu.",
    "BookmarkPanel":    "Tree click / context menu / drag-drop.",
    "PropertiesPanel":  "Read-only display.",
    "SearchPanel":      "Form: find QLineEdit, replace QLineEdit, case/wholeword QCheckBox, search QPushButton.",
    "CommentPanel":     "Tree click + filter input.",
    "RedactionPanel":   "Mark list + Apply button.",
    "SignaturePanel":   "Signature list + Verify button.",
    "AttachmentPanel":  "Tree double-click extracts file.",
    "LayerPanel":       "Tree checkboxes toggle visibility.",
    "ImagePropertiesPanel": "Read-only display + buttons.",
    "FormFieldPropertiesPanel": "Per-field-type editor (Text/Checkbox/Choice).",
    "AnnotationPropertiesWidget": "Color/opacity/author/contents.",
    "DefaultsLogPanel": "Read-only log.",
    "StatusBar":        "Read-only display.",
    "StickyNotePopup":  "Inline contents QPlainTextEdit.",
    "TextEditToolBar":  "Font/size/bold/italic/underline/color/align controls.",
}

WIDGET_TYPE_VISUAL = {
    "QLabel": "DEPENDS — visible only when text changes.",
    "QStatusBar": "YES",
    "QProgressBar": "YES",
    "QSlider": "YES",
    "QSpinBox": "YES",
    "QDoubleSpinBox": "YES",
    "QComboBox": "YES",
    "QFontComboBox": "YES",
    "QPushButton": "YES",
    "QToolButton": "YES",
    "QCheckBox": "YES",
    "QRadioButton": "YES",
    "QLineEdit": "YES",
    "QPlainTextEdit": "YES",
    "QTextEdit": "YES",
    "QTextBrowser": "YES",
    "QListWidget": "YES",
    "QTreeWidget": "YES",
    "QTableWidget": "YES",
    "QGroupBox": "NO",
    "QStackedWidget": "YES",
    "QTabWidget": "YES",
    "QDialogButtonBox": "YES",
    "QToolBar": "YES",
    "QMenu": "YES",
    "QMenuBar": "YES",
    "QDockWidget": "YES",
    "QDateEdit": "YES",
    "QDateTimeEdit": "YES",
    "PdfCanvas": "YES",
    "PagePanel": "YES",
    "BookmarkPanel": "YES",
    "PropertiesPanel": "YES",
    "SearchPanel": "YES",
    "CommentPanel": "YES",
    "RedactionPanel": "YES",
    "SignaturePanel": "YES",
    "AttachmentPanel": "YES",
    "LayerPanel": "YES",
    "ImagePropertiesPanel": "YES",
    "FormFieldPropertiesPanel": "YES",
    "AnnotationPropertiesWidget": "YES",
    "DefaultsLogPanel": "YES",
    "StatusBar": "YES",
    "StickyNotePopup": "YES",
    "TextEditToolBar": "YES",
}

# --------------------------------------------------------------------------- #
# 4. Generator                                                                #
# --------------------------------------------------------------------------- #

REPO = Path(__file__).resolve().parents[2]
CATALOG = REPO / "tests" / "docs" / "ui_element_catalog.json"
OUT_MD = REPO / "tests" / "docs" / "ui_function_matrix.md"
OUT_JSON = REPO / "tests" / "docs" / "ui_function_matrix.json"
UI_SRC = REPO / "src" / "ui"


# --------------------------------------------------------------------------- #
# Phase-2 remediation Finding 4 — reduce placeholder rows for non-shared
# slots by reading slot bodies directly from src/ui/*.cpp.
#
# Two layers:
#  A. EXTRA_SLOTS: hand-authored richer descriptions for non-shared slots
#     where the source body is too long, complex, or non-trivial to summarize
#     mechanically. These take precedence.
#  B. _read_slot_body(): falls back to mechanical extraction — finds the
#     definition `void Class::slot(...) { ... }` and returns the body text
#     (whitespace-collapsed) so the matrix can show what the slot actually
#     does instead of "Connected to X slot — body in X.cpp."
#
# The Qt built-in `QLineEdit::setEnabled` (a connect-target Qt slot, not a
# member function PDFE owns) is also handled in EXTRA_SLOTS.
# --------------------------------------------------------------------------- #

EXTRA_SLOTS: dict[str, dict] = {
    # Qt built-in slots used as connect targets — not PDFE members.
    "QLineEdit::setEnabled": {
        "input": "(bool) — passed from sender signal (e.g., QCheckBox::toggled).",
        "state": "Built-in Qt slot — toggles the line edit's enabled state. No PDFE state.",
        "visible": "Line edit grays out / becomes interactive.",
        "errors": "None.",
        "visual": "YES",
    },
    # Hand-authored non-shared slots that benefit from richer prose than
    # mechanical body extraction can give.
    "CompareDialog::onPageChanged": {
        "input": "(int page) from QSpinBox::valueChanged via QOverload<int>::of.",
        "state": "displayPage(page-1) — repaints diff overlay for the chosen page index.",
        "visible": "Compare canvas re-renders the selected page diff.",
        "errors": "page out of range → displayPage clamps internally.",
        "visual": "YES",
    },
    "EncryptionDialog::validateInput": {
        "input": "(QString text) from any password / permission edit.",
        "state": "Re-evaluates user/owner password match + length; toggles okButton enabled state; updates strength label.",
        "visible": "Strength label color/text change; OK button enable/disable.",
        "errors": "Empty / mismatched passwords → OK disabled.",
        "visual": "YES",
    },
    "EncryptionDialog::onAlgorithmChanged": {
        "input": "(int comboIndex) from QComboBox::currentIndexChanged.",
        "state": "Updates internal algorithm enum; enables/disables key-length combo and AES-only permission checkboxes.",
        "visible": "Permissions checkboxes enable/disable; key length combo updates.",
        "errors": "None.",
        "visual": "YES",
    },
    "EncryptionDialog::updatePasswordStrength": {
        "input": "(QString) — current password edit text.",
        "state": "Computes strength score; sets passwordStrengthLabel_ text + color.",
        "visible": "Strength label updates inline.",
        "errors": "None.",
        "visual": "YES",
    },
    "PrintDialog::validatePageRange": {
        "input": "(int) from From/To QSpinBox::valueChanged.",
        "state": "Re-validates From <= To and within document page count; toggles Print button enabled state.",
        "visible": "Print button enable/disable; range error label updates.",
        "errors": "Invalid range → Print disabled.",
        "visual": "YES",
    },
    "PasswordDialog::onPasswordChanged": {
        "input": "(QString) from passwordEdit text edit.",
        "state": "Toggles okButton enabled when text is non-empty.",
        "visible": "OK button enables.",
        "errors": "Empty → OK disabled.",
        "visual": "YES",
    },
}

# Source-body cache so repeated lookups for the same class don't re-read.
_class_source_cache: dict[str, str] = {}


def _read_class_source(cls: str) -> str:
    """Read src/ui/<Cls>.cpp text (cached). Returns '' if the file is
    missing — that triggers the "body in <Cls>.cpp" fallback rather than
    a hard error so the generator stays robust to filename mismatches."""
    if cls in _class_source_cache:
        return _class_source_cache[cls]
    path = UI_SRC / f"{cls}.cpp"
    if not path.exists():
        _class_source_cache[cls] = ""
        return ""
    txt = path.read_text(encoding="utf-8", errors="replace")
    _class_source_cache[cls] = txt
    return txt


def _extract_slot_body(slot: str) -> str | None:
    """Extract `Class::method` body from src/ui/Class.cpp. Returns the
    body text with whitespace collapsed, or None if not found.

    Locates `<ret> Class::method(...) [const] {` then balances braces.
    """
    if "::" not in slot:
        return None
    cls, name = slot.split("::", 1)
    text = _read_class_source(cls)
    if not text:
        return None
    # Match definition start. Allow any return type (void, int, bool, etc.),
    # any args, optional const, then opening brace.
    import re as _re
    pat = _re.compile(
        r'\b\w[\w:<>,\s\*&]*\b'                  # return type
        + _re.escape(cls) + r'\s*::\s*' + _re.escape(name)
        + r'\s*\([^)]*\)\s*(?:const\s*)?\{'
    )
    m = pat.search(text)
    if not m:
        return None
    # Brace-balance from the '{'
    i = m.end() - 1  # at '{'
    depth = 0
    while i < len(text):
        ch = text[i]
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                break
        i += 1
    if i >= len(text):
        return None
    body = text[m.end():i].strip()
    body = _re.sub(r'//[^\n]*', ' ', body)        # strip line comments
    body = _re.sub(r'/\*.*?\*/', ' ', body, flags=_re.S)  # block comments
    body = _re.sub(r'\s+', ' ', body)
    return body


def _summarize_slot_body(body: str, max_len: int = 200) -> str:
    """Reduce a slot body to a concise state_transitions summary.

    Strategy: take the first ~3 statements (split by `;`), prefer
    statements that look load-bearing (assignments, function calls, emits).
    Strip leading 'if/while/for' guards to surface the actual mutation.
    """
    if not body:
        return ""
    # Drop obvious early-return guards: "if (!x) return;"
    parts = [p.strip() for p in body.split(";") if p.strip()]
    chosen = []
    for p in parts:
        if p.startswith("if ") or p.startswith("else if ") or p.startswith("else"):
            continue
        if p.startswith("while ") or p.startswith("for ") or p.startswith("return"):
            continue
        chosen.append(p)
        if len(chosen) >= 3:
            break
    if not chosen:
        chosen = parts[:3]
    out = "; ".join(chosen)
    if len(out) > max_len:
        out = out[: max_len - 1] + "…"
    return out


def lookup_non_shared_slot(slot: str) -> dict | None:
    """Return a dict like SHARED_SLOTS values for a non-shared slot, or
    None if no information could be gathered."""
    if slot in EXTRA_SLOTS:
        return EXTRA_SLOTS[slot]
    body = _extract_slot_body(slot)
    if not body:
        return None
    summary = _summarize_slot_body(body)
    if not summary:
        return None
    cls = slot.split("::", 1)[0]
    return {
        "input": "Per-signal — see source.",
        "state": f"{summary} (extracted from {cls}.cpp)",
        "visible": "Per slot — typically updates dialog/panel state visible to user.",
        "errors": "Per slot; see source for guards.",
        "visual": "YES",
    }


def _extract_lambda_body(parent_class: str, line_no: int) -> str | None:
    """Find the inline lambda starting at the given line in src/ui/<class>.cpp
    and return its body summary. Lambdas in connect() statements look like:

        connect(sender, &Cls::sig, this, [this]() { ... });

    Heuristic: locate the `[` capture-list bracket on or after `line_no`,
    then find its matching `{ ... }` body.
    """
    text = _read_class_source(parent_class)
    if not text:
        return None
    lines = text.splitlines()
    if line_no - 1 >= len(lines):
        return None
    # Search a small window starting at the connect's line.
    window_start_idx = sum(len(l) + 1 for l in lines[: line_no - 1])
    window = text[window_start_idx: window_start_idx + 4000]
    # Find first '{' after the first '[' (capture list) in window.
    bracket = window.find("[")
    if bracket < 0:
        return None
    brace = window.find("{", bracket)
    if brace < 0:
        return None
    depth = 0
    i = brace
    while i < len(window):
        ch = window[i]
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                break
        i += 1
    if i >= len(window):
        return None
    import re as _re
    body = window[brace + 1: i].strip()
    body = _re.sub(r'//[^\n]*', ' ', body)
    body = _re.sub(r'/\*.*?\*/', ' ', body, flags=_re.S)
    body = _re.sub(r'\s+', ' ', body)
    return body


def lookup_lambda_wiring(parent_class: str, line_no: int) -> dict | None:
    """Return a SHARED_SLOTS-shaped dict for an inline lambda, by reading
    the lambda body from source. Returns None if the body cannot be
    located or summarised."""
    body = _extract_lambda_body(parent_class, line_no)
    if not body:
        return None
    summary = _summarize_slot_body(body)
    if not summary:
        return None
    return {
        "input": "Per-signal — lambda captures sender by reference.",
        "state": f"{summary} (inline lambda at {parent_class}.cpp:{line_no})",
        "visible": "Per lambda — typically updates dialog state, opens a child dialog, or pushes a command.",
        "errors": "Per lambda; user-cancel paths return early.",
        "visual": "YES",
    }


def truncate(s: str, n: int = 200) -> str:
    if s is None:
        return ""
    s = str(s).replace("\n", " ").replace("|", "\\|")
    return s if len(s) <= n else s[: n - 1] + "…"


def slot_lookup(slot: str | None) -> dict | None:
    if not slot:
        return None
    return SHARED_SLOTS.get(slot)


def lambda_purpose(parent_class: str, line: int) -> str:
    """Best-effort heuristic for inline lambda slot — described per parent."""
    return f"inline lambda in {parent_class} (line {line})"


def render_action(a: dict, parent_class: str) -> dict:
    """Build matrix row for a QAction entry."""
    objname = a.get("objectName") or "(unnamed)"
    slot = a.get("slot_connected_to")
    wirings = a.get("signal_slot_wiring") or []

    # Phase-1 catalog missed multi-line connect() statements and
    # QActionGroup-only connections. KNOWN_WIRING fills those in.
    if not slot and objname in KNOWN_WIRING:
        wired = KNOWN_WIRING[objname]
        if wired.startswith("@internal"):
            note = KNOWN_WIRING_NOTES.get(objname, wired)
            return {
                "element_id": objname,
                "category": "action",
                "source": f"{a.get('source_file')}:{a.get('source_line')}",
                "function_triggered": wired,
                "input_range": "N/A — no QAction::triggered connection.",
                "state_transitions": note,
                "visible_changes": "DEPENDS — programmatic activation only; visible effect produced by the caller (e.g., activateTextEditForPage flips check state, MainWindow::onAnnotationToolSelected installs tool on canvas).",
                "error_states": "None directly.",
                "needs_visual_verify": "DEPENDS",
                "shortcut": a.get("shortcut") or "",
                "accessible_name": a.get("accessibleName") or "",
                "text": a.get("text_expr") or "",
            }
        slot = wired  # fall through to shared-slot lookup

    has_lambda_wiring = any(
        (w.get("slot") == "<lambda>" or w.get("lambda"))
        for w in wirings
    )

    # Determine function_triggered
    if slot and slot in SHARED_SLOTS:
        info = SHARED_SLOTS[slot]
        function = f"→ {slot}"
        input_range = info["input"]
        state_transitions = info["state"]
        # If we have a known-wiring note, prefix it.
        if objname in KNOWN_WIRING_NOTES:
            state_transitions = (KNOWN_WIRING_NOTES[objname] + " " + state_transitions)
        visible_changes = info["visible"]
        error_states = info["errors"]
        visual = info["visual"]
    elif slot and "::" in slot:
        # Phase-2 remediation Finding 4: enrich non-shared slot rows
        # using EXTRA_SLOTS or source-body extraction.
        extra = lookup_non_shared_slot(slot)
        function = f"→ {slot}"
        input_range = "N/A — QAction (no input value)."
        if extra:
            state_transitions = extra["state"]
            visible_changes = extra["visible"]
            error_states = extra["errors"]
            visual = extra["visual"]
        else:
            state_transitions = f"Implementation in {slot.split('::')[0]}.cpp; consult that file for details."
            visible_changes = "Class-specific; see source."
            error_states = "Class-specific; see source."
            visual = "YES"
    elif has_lambda_wiring:
        # inline lambda
        lam_line = next(
            (w.get("source_line") for w in wirings
             if w.get("slot") == "<lambda>" or w.get("lambda")),
            None,
        )
        function = f"inline lambda @ {parent_class}.cpp:{lam_line}"
        input_range = "N/A — QAction click only; lambda body reads MainWindow members."
        state_transitions = "Inline lambda; transitions read directly from source (typically: open dialog, run command, refresh canvas)."
        visible_changes = "Per lambda — typically opens a modal dialog or pushes a command."
        error_states = "Per lambda; user-cancel paths typically return early."
        visual = "YES"
    else:
        function = "N/A — action declared but no slot connection found in catalog scan."
        input_range = "N/A."
        state_transitions = "None at signal/slot level — action may be programmatically triggered or reserved."
        visible_changes = "None directly."
        error_states = "None — action inert without connection."
        visual = "NO"

    return {
        "element_id": objname,
        "category": "action",
        "source": f"{a.get('source_file')}:{a.get('source_line')}",
        "function_triggered": function,
        "input_range": input_range,
        "state_transitions": state_transitions,
        "visible_changes": visible_changes,
        "error_states": error_states,
        "needs_visual_verify": visual,
        "shortcut": a.get("shortcut") or "",
        "accessible_name": a.get("accessibleName") or "",
        "text": a.get("text_expr") or "",
    }


def render_widget(w: dict, parent_class: str) -> dict:
    objname = w.get("objectName") or f"({w.get('type')}#{w.get('source_line')})"
    wtype = w.get("type") or "QWidget"
    wirings = w.get("signal_slot_wiring") or []

    if wirings:
        connected_slots = []
        lambdas = 0
        first_lambda_line = None
        for wi in wirings:
            slot_val = wi.get("slot")
            if slot_val and slot_val != "<lambda>":
                connected_slots.append(slot_val)
            else:
                lambdas += 1
                if first_lambda_line is None:
                    first_lambda_line = wi.get("source_line")
        parts = []
        if connected_slots:
            parts += [f"→ {s}" for s in connected_slots]
        if lambdas:
            parts.append(f"+ {lambdas} inline lambda(s)")
        function = "; ".join(parts) if parts else f"signal/slot in {parent_class}"

        # If at least one shared slot
        shared_hit = next((SHARED_SLOTS[s] for s in connected_slots if s in SHARED_SLOTS), None)
        if shared_hit:
            input_range = WIDGET_TYPE_INPUT.get(wtype, "Per-widget; see source.")
            state_transitions = shared_hit["state"]
            visible_changes = shared_hit["visible"]
            error_states = shared_hit["errors"]
            visual = shared_hit["visual"]
        else:
            # Phase-2 remediation Finding 4: try to enrich non-shared
            # slots from EXTRA_SLOTS or by reading the source body.
            extra = next(
                (info for s in connected_slots
                 if (info := lookup_non_shared_slot(s)) is not None),
                None,
            )
            # If no named slot info, fall back to lambda-body extraction
            # for the first lambda wiring.
            if extra is None and lambdas and first_lambda_line is not None:
                extra = lookup_lambda_wiring(parent_class, first_lambda_line)
            input_range = WIDGET_TYPE_INPUT.get(wtype, "Per-widget; see source.")
            if extra:
                state_transitions = extra["state"]
                visible_changes = extra["visible"]
                error_states = extra["errors"]
                visual = extra["visual"]
            else:
                state_transitions = f"Connected to {parent_class} slot — body in {parent_class}.cpp."
                visible_changes = "Per slot; typically updates dialog state or emits a signal."
                error_states = "Per slot."
                visual = WIDGET_TYPE_VISUAL.get(wtype, "YES")
    else:
        # Passive sub-control of a dialog — captured via accessor at exec()-time.
        function = (f"N/A — passive sub-control of {parent_class}; value read by "
                    f"{parent_class}::accessor() at QDialog::exec()-time.")
        input_range = WIDGET_TYPE_INPUT.get(wtype, "Per-widget; see source.")
        state_transitions = (f"None at signal level — value captured into result struct when "
                             f"{parent_class} is accept()ed.")
        visible_changes = ("DEPENDS — typing/clicking changes the widget's own appearance "
                           "(focus ring, text, checked state) but does not commit until dialog accepts.")
        error_states = "Validation typically deferred to dialog-level validateInput()/accept() guard."
        visual = WIDGET_TYPE_VISUAL.get(wtype, "YES")

    return {
        "element_id": objname,
        "category": "widget",
        "source": f"{w.get('source_file')}:{w.get('source_line')}",
        "function_triggered": function,
        "input_range": input_range,
        "state_transitions": state_transitions,
        "visible_changes": visible_changes,
        "error_states": error_states,
        "needs_visual_verify": visual,
        "widget_type": wtype,
        "accessible_name": w.get("accessibleName") or "",
        "var": w.get("var") or "",
    }


def render_dialog(d: dict) -> dict:
    cls = d["class_name"]
    base = d.get("base", "")
    info = DIALOG_INFO.get(cls, {})

    if base == "QDialog":
        function = (f"{cls}::exec() — modal dialog; "
                    f"{info.get('purpose', 'see class header for purpose.')}")
        state = info.get(
            "outputs",
            f"On accept(), caller reads dialog result via {cls}::accessor() methods; "
            f"on reject() no state changes.",
        )
        visible = ("Modal dialog appears; on close it disappears. "
                   "Sub-controls re-render per user input.")
        errors = "Per accept() validation logic in this dialog; reject() always succeeds."
    elif base == "QMainWindow":
        function = f"{cls} — top-level window; {info.get('purpose', '')}"
        state = info.get("outputs", "Hosts the entire application window.")
        visible = "Application main window."
        errors = "See per-action error states."
    elif base == "QMenu":
        function = (f"{cls} — QMenu subclass; "
                    f"{info.get('purpose', '')}")
        state = info.get("outputs", "Menu populated dynamically; emits triggered/aboutToShow.")
        visible = "Menu appears in parent menu hierarchy."
        errors = "None at menu level."
    elif base == "QStatusBar":
        function = (f"{cls} — QStatusBar subclass; "
                    f"{info.get('purpose', '')}")
        state = info.get("outputs", "Read-only display; setters from MainWindow.")
        visible = "Status bar at bottom of main window."
        errors = "None — display only."
    elif base == "QFrame":
        function = (f"{cls} — QFrame popup; "
                    f"{info.get('purpose', '')}")
        state = info.get("outputs", "Popup widget; emits content-change/close signals.")
        visible = "Floating popup near triggering control."
        errors = "None."
    else:  # QWidget panel
        function = (f"{cls} — QWidget side panel; "
                    f"{info.get('purpose', '')}")
        state = info.get(
            "outputs",
            f"{cls} emits domain-specific signals consumed by MainWindow.",
        )
        visible = ("Side dock visibility toggleable via View menu. "
                   "Internal state visible inline as user interacts.")
        errors = "Per user-interaction; see widget rows for sub-controls."

    return {
        "element_id": cls,
        "category": "dialog",
        "source": d.get("header") or "?",
        "function_triggered": function,
        "input_range": info.get(
            "inputs",
            "Per-widget — see widget rows for this class's source file.",
        ),
        "state_transitions": state,
        "visible_changes": visible,
        "error_states": errors,
        "needs_visual_verify": info.get("visual", "YES"),
        "base": base,
    }


def render_menu(m: dict) -> dict:
    return {
        "element_id": m.get("title") or "(unnamed menu)",
        "category": "menu",
        "source": f"{m.get('source_file')}:{m.get('source_line')}",
        "function_triggered": "Container — child QActions trigger their own slots; menu open emits aboutToShow.",
        "input_range": "N/A — menu opens via QMenuBar click or shortcut.",
        "state_transitions": "Menu opens (paints popup); aboutToShow fires for dynamic submenus (Stamps, Recent Files); selecting an item closes the menu and triggers the chosen QAction.",
        "visible_changes": "Menu popup paints over canvas; closes on item selection or Esc.",
        "error_states": "None at menu level — child actions handle their own errors.",
        "needs_visual_verify": "YES",
    }


def render_toolbar(t: dict) -> dict:
    return {
        "element_id": t.get("title") or "(unnamed toolbar)",
        "category": "toolbar",
        "source": f"{t.get('source_file')}:{t.get('source_line')}",
        "function_triggered": "Container — child QActions trigger their own slots.",
        "input_range": "N/A — buttons clicked individually.",
        "state_transitions": "QToolBar visibility may toggle (View menu); button state reflects active QAction checked-state.",
        "visible_changes": "Toolbar shows/hides; per-button highlight when checked.",
        "error_states": "None at toolbar level.",
        "needs_visual_verify": "YES",
    }


def render_context_menu(cm: dict) -> dict:
    return {
        "element_id": cm.get("var") or "(unnamed context menu)",
        "category": "context_menu",
        "source": f"{cm.get('source_file')}:{cm.get('source_line')}",
        "function_triggered": "Container — populated on customContextMenuRequested or contextMenuEvent.",
        "input_range": "Right-click event QPoint.",
        "state_transitions": "Menu rebuilt on each show; child QActions trigger their own slots.",
        "visible_changes": "Popup menu appears at cursor; closes on item selection or Esc.",
        "error_states": "None at menu level.",
        "needs_visual_verify": "YES",
    }


def render_drag_target(dt: dict) -> dict:
    return {
        "element_id": f"dragTarget@{dt.get('source_file')}",
        "category": "drag_target",
        "source": dt.get("source_file") or "?",
        "function_triggered": "MainWindow::dragEnterEvent + dropEvent",
        "input_range": "MIME types accepted: 'text/uri-list' (file:// URLs ending in .pdf) and 'application/x-pdf-page-id' (internal page-reorder drags).",
        "state_transitions": "On drop with PDF URL: openFile(path). On drop with page-id: PagePanel::onPageMoved → MovePageCommand.",
        "visible_changes": "On enter: drop indicator (cursor+highlight). On valid drop: document opens or pages reorder.",
        "error_states": "Unsupported MIME → dragEnterEvent rejects (cursor shows 'no-drop'). Drop of corrupt PDF → openFile error path.",
        "needs_visual_verify": "YES",
    }


def main() -> int:
    if not CATALOG.exists():
        print(f"Missing catalog: {CATALOG}", file=sys.stderr)
        return 1

    cat = json.loads(CATALOG.read_text(encoding="utf-8"))

    # parent class lookup from header → class
    header_to_class = {d["header"]: d["class_name"] for d in cat["dialogs"]}

    rows: list[dict] = []
    by_file: dict[str, list[dict]] = defaultdict(list)
    by_category: dict[str, int] = defaultdict(int)
    visual_counts = {"YES": 0, "NO": 0, "DEPENDS": 0}

    # Actions
    for a in cat["actions"]:
        # Try to map source_file → parent class
        src = a.get("source_file") or ""
        cls = "?"
        if src.startswith("src/ui/") and src.endswith(".cpp"):
            cls = src.rsplit("/", 1)[-1].rsplit(".", 1)[0]
        row = render_action(a, cls)
        rows.append(row)
        by_file[src].append(row)
        by_category["action"] += 1

    # Widgets
    for w in cat["widgets"]:
        src = w.get("source_file") or ""
        cls = "?"
        if src.startswith("src/ui/") and src.endswith(".cpp"):
            cls = src.rsplit("/", 1)[-1].rsplit(".", 1)[0]
        row = render_widget(w, cls)
        rows.append(row)
        by_file[src].append(row)
        by_category["widget"] += 1

    # Dialogs
    for d in cat["dialogs"]:
        row = render_dialog(d)
        rows.append(row)
        by_file[d.get("header", "?")].append(row)
        by_category["dialog"] += 1

    # Menus
    for m in cat["menus"]:
        row = render_menu(m)
        rows.append(row)
        by_file[m.get("source_file", "?")].append(row)
        by_category["menu"] += 1

    # Toolbars
    for t in cat["toolbars"]:
        row = render_toolbar(t)
        rows.append(row)
        by_file[t.get("source_file", "?")].append(row)
        by_category["toolbar"] += 1

    # Context menus
    for cm in cat["context_menus"]:
        row = render_context_menu(cm)
        rows.append(row)
        by_file[cm.get("source_file", "?")].append(row)
        by_category["context_menu"] += 1

    # Drag targets
    for dt in cat["drag_targets"]:
        row = render_drag_target(dt)
        rows.append(row)
        by_file[dt.get("source_file", "?")].append(row)
        by_category["drag_target"] += 1

    # Visual counts
    for r in rows:
        v = r["needs_visual_verify"]
        if v.startswith("YES"):
            visual_counts["YES"] += 1
        elif v.startswith("NO"):
            visual_counts["NO"] += 1
        else:
            visual_counts["DEPENDS"] += 1

    # ----------------------------- write JSON ------------------------------ #
    json_doc = {
        "schema_version": "1",
        "pipeline_id": cat.get("pipeline_id"),
        "phase": "phase-2",
        "generated_from": "tests/docs/ui_element_catalog.json",
        "summary": {
            "total_rows": len(rows),
            "by_category": dict(by_category),
            "visual_verify": visual_counts,
        },
        "shared_slots": {k: v for k, v in SHARED_SLOTS.items()},
        "rows": rows,
    }
    OUT_JSON.write_text(json.dumps(json_doc, indent=2), encoding="utf-8")

    # ----------------------------- write MD -------------------------------- #
    out = io.StringIO()
    out.write("# UI Function Matrix — Pipeline a7c2e541 Phase-2\n\n")
    out.write(
        "Generated from `tests/docs/ui_element_catalog.json` (518 entries from "
        "phase-1) by `tests/docs/generate_function_matrix.py`. Source slot "
        "bodies were read directly from `src/ui/*.cpp` while authoring the "
        "shared-slots and dialog-info tables.\n\n"
    )

    out.write("## Summary\n\n")
    out.write(f"- **Total rows documented**: {len(rows)}\n")
    out.write(f"- **By category**:\n")
    for k in sorted(by_category):
        out.write(f"  - {k}: {by_category[k]}\n")
    out.write("- **Needs visual verification** (drives phase-4 baseline scope):\n")
    out.write(f"  - YES: {visual_counts['YES']}\n")
    out.write(f"  - NO: {visual_counts['NO']}\n")
    out.write(f"  - DEPENDS: {visual_counts['DEPENDS']}\n")
    out.write("\n### Visual-verify decision rule (used per row)\n\n")
    out.write(
        "- **YES** — produces canvas re-render, opens dialog, navigates pages, "
        "changes zoom, applies an annotation, edits text, redacts, signs, runs "
        "OCR, switches mode, toggles panel visibility, or otherwise paints "
        "pixels in the main window.\n"
        "- **NO** — registers internal state only (e.g., recent-files entry, "
        "navigation history record, clipboard write that's not directly "
        "visible in the app window).\n"
        "- **DEPENDS** — visible effect depends on context (most commonly: "
        "undo/redo whose visible result depends on what was undone, or "
        "passive QLineEdit values committed only at dialog accept-time).\n\n"
    )

    # ------------------------- shared-slots section ------------------------ #
    out.write("## Shared slots\n\n")
    out.write(
        "Slots invoked by multiple QActions / widgets are documented once "
        "here; per-element rows below reference them via `→ ClassName::slotName`.\n\n"
    )
    for slot, info in sorted(SHARED_SLOTS.items()):
        out.write(f"### `{slot}`\n\n")
        out.write(f"- **Input**: {info['input']}\n")
        out.write(f"- **State transitions**: {info['state']}\n")
        out.write(f"- **Visible changes**: {info['visible']}\n")
        out.write(f"- **Error states**: {info['errors']}\n")
        out.write(f"- **Needs visual verify**: {info['visual']}\n\n")

    # ----------------------- per-dialog summary ---------------------------- #
    out.write("## Per-dialog summary\n\n")
    out.write(
        "Each dialog gets its own row in the per-file matrix, but their "
        "purpose + key inputs + outputs are summarized here so individual "
        "sub-control rows below can reference the parent.\n\n"
    )
    out.write("| class | purpose | inputs | outputs | visual? |\n")
    out.write("|---|---|---|---|---|\n")
    for cls, info in sorted(DIALOG_INFO.items()):
        out.write("| `{cls}` | {p} | {i} | {o} | {v} |\n".format(
            cls=cls,
            p=truncate(info.get("purpose", ""), 110),
            i=truncate(info.get("inputs", ""), 110),
            o=truncate(info.get("outputs", ""), 110),
            v=info.get("visual", "?"),
        ))
    out.write("\n")

    # ------------------------- per-file matrix ----------------------------- #
    out.write("## Per-file matrix\n\n")
    out.write(
        "One section per source file. Rows are grouped by category within "
        "each file.\n\n"
    )

    for src in sorted(by_file):
        rows_in_file = by_file[src]
        out.write(f"### `{src}` ({len(rows_in_file)} entries)\n\n")
        # Group by category
        by_cat_file: dict[str, list[dict]] = defaultdict(list)
        for r in rows_in_file:
            by_cat_file[r["category"]].append(r)

        for cat_name in ("action", "widget", "dialog", "menu", "toolbar",
                         "context_menu", "drag_target"):
            cat_rows = by_cat_file.get(cat_name) or []
            if not cat_rows:
                continue
            out.write(f"#### {cat_name}s ({len(cat_rows)})\n\n")
            out.write("| element_id | source | function_triggered | "
                      "input_range | state_transitions | visible_changes | "
                      "error_states | visual? |\n")
            out.write("|---|---|---|---|---|---|---|---|\n")
            for r in cat_rows:
                out.write("| `{eid}` | {src} | {fn} | {ir} | {st} | {vc} | {er} | {v} |\n".format(
                    eid=truncate(r["element_id"], 80),
                    src=truncate(r["source"], 80),
                    fn=truncate(r["function_triggered"], 200),
                    ir=truncate(r["input_range"], 200),
                    st=truncate(r["state_transitions"], 240),
                    vc=truncate(r["visible_changes"], 200),
                    er=truncate(r["error_states"], 200),
                    v=truncate(r["needs_visual_verify"], 80),
                ))
            out.write("\n")

    # --------------------------- coverage notes ---------------------------- #
    out.write("## Coverage\n\n")
    out.write(
        f"All {len(rows)} catalog entries have a row in this matrix. Each "
        f"row has at least one non-empty value per required column "
        f"(function_triggered, input_range, state_transitions, "
        f"visible_changes, error_states). 'N/A — <reason>' is used where "
        f"the column is genuinely not applicable (e.g., a QAction has no "
        f"input value).\n\n"
    )

    out.write("### Phase-1 catalog gaps fixed in phase-2\n\n")
    out.write(
        "While building the matrix we found that phase-1's regex-based "
        "scanner missed connections in two patterns:\n\n"
        "1. **Multi-line `connect()` statements** — when arguments span "
        "more than one line (e.g., `connect(action, &QAction::triggered,\\n"
        "    this, &MainWindow::slot);`), phase-1 recorded "
        "`slot_connected_to: null`. The matrix's `KNOWN_WIRING` table "
        "supplements the catalog with the actual slot read directly from "
        "`MainWindow.cpp`.\n"
        "2. **QActionGroup-only connections** — annotation tools "
        "(`selectToolAction`, `stickyNoteToolAction`, etc.) connect to "
        "`MainWindow::onAnnotationToolSelected` only via "
        "`annotToolGroup_::triggered`. Phase-1's per-action scanner did "
        "not follow the group's outgoing connection, so these actions "
        "appeared unwired. The matrix records them correctly via "
        "`KNOWN_WIRING`.\n\n"
        "These gaps are documented in this matrix; they should be folded "
        "into the phase-1 scanner if it gets re-run.\n\n"
    )

    out.write("### Partial-coverage entries\n\n")
    out.write(
        "Phase-1 carried forward 3 partial-coverage notes for "
        "macro-generated symbols / forwarded-declaration headers — these "
        "are listed in the catalog's `partial_coverage_notes` field. They "
        "remain partial-coverage at the matrix level: the catalog could not "
        "extract a concrete element_id, so this matrix has no row for them. "
        "The reasons (Q_OBJECT moc, header forward-decl, AccessibilityHelper "
        "static helpers) make them out of scope for an end-user UI test.\n\n"
    )
    for note in cat.get("partial_coverage_notes", []):
        out.write(f"- `{note.get('file')}` — {note.get('reason')}\n")
    out.write("\n")

    OUT_MD.write_text(out.getvalue(), encoding="utf-8")
    print(f"Wrote {OUT_MD} ({len(rows)} rows, "
          f"{visual_counts['YES']} visual / {visual_counts['NO']} non-visual / "
          f"{visual_counts['DEPENDS']} depends).")
    print(f"Wrote {OUT_JSON}.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
