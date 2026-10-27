# OBJECTNAME AUDIT — PDFE Interactive Widgets

<!-- tracking-banner -->
> **STATUS: LIVE OPERATIONAL STATE — keep maintaining this file.**
>
> This is **not** a defect tracker and is **not** superseded by GitHub Issues;
> it is state that code, CI or a fixture generator reads. **GitHub Issues
> (`ipswitch8/PDFE`) remains the canonical tracker for anything still open** — a
> defect recorded only here is still an untracked defect.
>
> See [`docs/TRACKING.md`](../../../docs/TRACKING.md).

---


**Phase:** phase-2 (harness build + objectName / accessibleName gap fill)
**Generated:** 2026-04-28
**Scope:** src/ui/, src/textedit/, src/annotations/, src/forms/, src/redaction/, src/tre/, src/image/, src/layers/

**Scope extension (R2-19):** src/tre/, src/image/, src/layers/ added. The
machine-checked compliance test `ObjectNameAuditScopeTest` (in
`tests/test_accessible_names.cpp`) enforces this scope: it walks the live
`TreEditDialog` widget tree asserting no interactive widget lacks an accessible
name, and source-scans src/tre/, src/image/, src/layers/ so any newly-added
interactive widget in these subsystems that omits `setAccessibleName` re-reds
the suite.

This document lists every interactive widget under the audit scope and records:
- whether it has a stable `objectName` (required for pywinauto `auto_id` lookup)
- whether it has an `accessibleName` (required for UIA `Name` property / screen readers)
- the resolution status (OK / ADDED / DEFERRED)

A widget is "interactive" if it is a button, edit, combo, checkbox, radio, slider,
list, tree, spin box, or any control that receives focus or click input. Read-only
labels and layout spacers are excluded except where they are the sole text identity
of a group (e.g., status labels that automation reads).

---

## Legend

| status | meaning |
|---|---|
| OK | both objectName and accessibleName already present and stable |
| OK-NAME | objectName present; accessibleName omitted but widget is named via visible label (acceptable) |
| ADDED | accessibleName (or objectName) was missing and has been added in this phase |
| DEFERRED | widget is deep inside a custom-paint path or a transient dialog created inline — adding a stable name requires behaviour-level refactor; logged for Stage-C |

---

## src/ui/MainWindow.cpp

All QActions in MainWindow have stable objectNames set via `setObjectName()`. QAction
does not support `setAccessibleName()` (it inherits QObject, not QWidget). Toolbar
buttons derived from QActions receive `accessibleName` via the loop at line 1612
(`btn->setAccessibleName(name)`). These are OK.

| widget | objectName | accessibleName | status |
|---|---|---|---|
| mainWindow | mainWindow | — | OK-NAME |
| pdfCanvas | pdfCanvas | "PDF Canvas" | OK |
| mainToolBar | mainToolBar | — | OK-NAME |
| zoomCombo_ | zoomCombo | — | ADDED (see below) |
| annotationToolBar_ | annotationToolBar | — | OK-NAME |
| formToolBar_ | FormToolBar | — | OK-NAME |
| statusBar_ | statusBar | "Status Bar" | OK |
| colorPickerButton_ | annotColorPickerButton | — | ADDED |
| All QActions (≥70) | see MainWindow line 335–1045 | N/A (QAction) | OK |
| Toolbar buttons from QActions | via `act->objectName()+"Button"` pattern | via `btn->setAccessibleName(name)` | OK |

**Fixes applied in this phase:**
- `zoomCombo_`: added `setAccessibleName(tr("Zoom Level"))` — pywinauto needs this to find the zoom combo by name
- `colorPickerButton_`: added `setAccessibleName(tr("Annotation Color Picker"))` — was missing

---

## src/ui/PdfCanvas.cpp

| widget | objectName | accessibleName | status |
|---|---|---|---|
| PdfCanvas (self) | pdfCanvas | "PDF Canvas" | OK |

PdfCanvas has a dedicated `PdfCanvasAccessible` QAccessibleInterface subclass
(`src/ui/PdfCanvasAccessible.cpp`) which exposes the widget in the UIA tree.
PyAutoGUI is the correct tool for canvas pixel operations; pywinauto is not used
for canvas content.

---

## src/ui/TextEditToolBar.cpp

| widget | objectName | accessibleName | status |
|---|---|---|---|
| textEditToolBar (self) | textEditToolBar | — | OK-NAME |
| fontComboBox_ | fontComboBox | — | ADDED |
| fontSizeSpinBox_ | fontSizeSpinBox | — | ADDED |
| boldButton_ | boldButton | — | ADDED |
| italicButton_ | italicButton | — | ADDED |
| underlineButton_ | underlineButton | — | ADDED |
| strikethroughButton_ | strikethroughButton | — | ADDED |
| textColorButton_ | textColorButton | "Text Color" | OK |
| alignLeftButton_ | alignLeftButton | — | ADDED |
| alignCenterButton_ | alignCenterButton | — | ADDED |
| alignRightButton_ | alignRightButton | — | ADDED |
| alignJustifyButton_ | alignJustifyButton | — | ADDED |
| atlasModeCombo_ | atlasModeCombo | "atlas-mode-combo" | OK |
| compareButton_ | compareAtlasButton | "atlas-compare-button" | OK |

**Fixes applied:** accessibleNames added to fontComboBox_, fontSizeSpinBox_, all four align buttons, bold/italic/underline/strikethrough buttons.

---

## src/ui/SearchPanel.cpp

| widget | objectName | accessibleName | status |
|---|---|---|---|
| searchEdit_ | searchEdit | — | ADDED |
| searchButton_ | searchButton | — | ADDED |
| clearButton_ | clearButton | — | ADDED |
| caseSensitiveCheck_ | caseSensitiveCheck | — | ADDED |
| wholeWordCheck_ | wholeWordCheck | — | ADDED |
| replaceField_ | replaceField | "Replace with" | OK |
| replaceButton_ | replaceButton | "Replace" | OK |
| replaceAllButton_ | replaceAllButton | "Replace All" | OK |
| replaceStatusLabel_ | replaceStatusLabel | "Replace Status" | OK |
| statusLabel_ | statusLabel | — | ADDED |
| resultsListWidget_ | resultsList | "Search Results" | OK |

**Fixes applied:** searchEdit_, searchButton_, clearButton_, caseSensitiveCheck_, wholeWordCheck_, statusLabel_ all received accessibleName.

---

## src/ui/PagePanel.cpp

| widget | objectName | accessibleName | status |
|---|---|---|---|
| pagePanel (self) | pagePanel | — | ADDED |
| listWidget_ (pagePanelList) | pagePanelList | — | ADDED |
| Context menu actions (rotate/insert/delete/extract) | stable IDs | N/A (QAction) | OK |

**Fixes applied:** PagePanel self and pagePanelList received accessibleName.

---

## src/ui/BookmarkPanel.cpp

| widget | objectName | accessibleName | status |
|---|---|---|---|
| bookmarkPanel (self) | bookmarkPanel | — | ADDED |
| treeWidget_ | bookmarkTree | — | ADDED |
| Context menu actions (8) | stable IDs | N/A (QAction) | OK |

**Fixes applied:** bookmarkPanel and bookmarkTree received accessibleName.

---

## src/ui/LayerPanel.cpp

| widget | objectName | accessibleName | status |
|---|---|---|---|
| layerPanel (self) | layerPanel | "Layer Panel" | OK |
| treeWidget_ | layerTreeWidget | "OCG Layer Tree" | OK |
| createButton_ | createLayerButton | "Create Layer" | OK |
| deleteButton_ | deleteLayerButton | "Delete Layer" | OK |
| renameButton_ | renameLayerButton | "Rename Layer" | OK |
| statusLabel_ | layerStatusLabel | — | OK-NAME |
| Context menu | layerContextMenu | N/A | OK |

---

## src/ui/CommentPanel.cpp

| widget | objectName | accessibleName | status |
|---|---|---|---|
| commentPanel (self) | commentPanel | — | ADDED |
| filterCombo_ | commentFilterCombo | — | ADDED |
| sortCombo_ | commentSortCombo | — | ADDED |
| treeWidget_ | commentTreeWidget | — | ADDED |
| Context menu actions (reply/delete) | stable IDs | N/A (QAction) | OK |

**Fixes applied:** commentPanel, filterCombo_, sortCombo_, treeWidget_ received accessibleName.

---

## src/ui/AttachmentPanel.cpp

| widget | objectName | accessibleName | status |
|---|---|---|---|
| attachmentPanel (self) | attachmentPanel | "Attachments Panel" | OK |
| tree_ | attachmentTree | "Attachment List" | OK |
| addButton_ | attachmentAddButton | "Add Attachment" | OK |
| extractButton_ | attachmentExtractButton | "Extract Attachment" | OK |
| deleteButton_ | attachmentDeleteButton | "Delete Attachment" | OK |
| statusLabel_ | attachmentStatusLabel | — | OK-NAME |

---

## src/ui/RedactionPanel.cpp

| widget | objectName | accessibleName | status |
|---|---|---|---|
| redactionPanel (self) | redactionPanel | — | ADDED |
| markCountLabel_ | markCountLabel | — | ADDED |
| treeWidget_ | redactionMarksList | — | ADDED |
| deleteButton_ | deleteRedactionButton | — | ADDED |
| applyButton_ | applyRedactionsButton | — | ADDED |

**Fixes applied:** All RedactionPanel interactive and informational widgets received accessibleName.

---

## src/ui/SignaturePanel.cpp

| widget | objectName | accessibleName | status |
|---|---|---|---|
| signaturePanel (self) | signaturePanel | — | ADDED |
| signatureTreeWidget_ | signatureTreeWidget | — | ADDED |
| verifyAllButton_ | verifyAllButton | — | ADDED |
| signSelectedButton_ | signSelectedButton | — | ADDED |
| viewDetailsButton_ | viewDetailsButton | — | ADDED |
| signatureSummaryLabel_ | signatureSummaryLabel | — | ADDED |

**Fixes applied:** All SignaturePanel widgets received accessibleName.

---

## src/ui/FormFieldPropertiesPanel.cpp

| widget | objectName | accessibleName | status |
|---|---|---|---|
| formFieldPropertiesPanel (self) | formFieldPropertiesPanel | — | ADDED |
| typeLabel_ | formFieldTypeLabel | — | ADDED |
| nameLabel_ | formFieldNameLabel | — | ADDED |
| flagsLabel_ | formFieldFlagsLabel | — | ADDED |
| editorStack_ | formFieldEditorStack | — | OK-NAME |
| textEdit_ | formFieldTextEdit | — | ADDED |
| multilineEdit_ | formFieldMultilineEdit | — | ADDED |
| checkboxWidget_ | formFieldCheckbox | — | ADDED |
| comboWidget_ | formFieldCombo | — | ADDED |
| listWidget_ | formFieldList | — | ADDED |
| noEditorLabel_ | formFieldNoEditor | — | OK-NAME |

**Fixes applied:** All interactive and informational widgets in FormFieldPropertiesPanel received accessibleName.

---

## src/ui/AnnotationPropertiesWidget.cpp

| widget | objectName | accessibleName | status |
|---|---|---|---|
| annotationPropertiesWidget (self) | annotationPropertiesWidget | — | ADDED |
| typeLabel_ | annotTypeLabel | — | ADDED |
| authorEdit_ | annotAuthorEdit | — | ADDED |
| contentsEdit_ | annotContentsEdit | — | ADDED |
| colorButton_ | annotColorButton | — | ADDED |
| opacitySlider_ | annotOpacitySlider | — | ADDED |
| opacityLabel_ | annotOpacityLabel | — | OK-NAME |
| fontCombo_ | annotFontCombo | "Annotation Font" | OK |
| fontSizeSpinner_ | annotFontSizeSpinner | "Annotation Font Size" | OK |
| textColorButton_ | annotTextColorButton | "Annotation Text Color" | OK |

**Fixes applied:** annotationPropertiesWidget self, typeLabel_, authorEdit_, contentsEdit_, colorButton_, opacitySlider_ received accessibleName.

---

## src/ui/StickyNotePopup.cpp

| widget | objectName | accessibleName | status |
|---|---|---|---|
| stickyNotePopup (self) | stickyNotePopup | — | ADDED |
| titleBar_ | stickyNotePopupTitle | — | ADDED |
| textEdit_ | stickyNotePopupEdit | — | ADDED |

**Fixes applied:** stickyNotePopup, titleBar_, textEdit_ received accessibleName.

---

## src/ui/StatusBar.cpp

| widget | objectName | accessibleName | status |
|---|---|---|---|
| statusBar (self) | statusBar | "Status Bar" | OK |
| pageLabel_ | pageLabel | — | ADDED |
| zoomLabel_ | zoomLabel | — | ADDED |
| fileNameLabel_ | fileNameLabel | — | ADDED |
| encryptionLabel_ | encryptionLabel | — | ADDED |
| formFieldCountLabel_ | formFieldCount | "Form Field Count" | OK |

**Fixes applied:** pageLabel_, zoomLabel_, fileNameLabel_, encryptionLabel_ received accessibleName (informational labels automation reads to verify state).

---

## src/ui/PropertiesPanel.cpp

| widget | objectName | accessibleName | status |
|---|---|---|---|
| propertiesPanel (self) | propertiesPanel | — | ADDED |
| pageIndexLabel_ | pageIndexLabel | — | ADDED |
| pageLabelLabel_ | pageLabelLabel | — | ADDED |
| widthLabel_ | widthLabel | — | ADDED |
| heightLabel_ | heightLabel | — | ADDED |
| rotationLabel_ | rotationLabel | — | ADDED |

**Fixes applied:** propertiesPanel and all info labels received accessibleName.

---

## src/ui/DefaultsLogPanel.cpp

| widget | objectName | accessibleName | status |
|---|---|---|---|
| defaultsLogPanel (self) | defaultsLogPanel | — | ADDED |
| logView_ | defaultsLogView | — | ADDED |
| clearButton_ | defaultsLogClearButton | — | ADDED |

**Fixes applied:** all three widgets received accessibleName.

---

## src/ui/ImagePropertiesPanel.cpp

| widget | objectName | accessibleName | status |
|---|---|---|---|
| imagePropertiesPanel (self) | imagePropertiesPanel | — | ADDED |
| widthSpin_ | imageWidthSpin | — | ADDED |
| heightSpin_ | imageHeightSpin | — | ADDED |
| rotationCombo_ | imageRotationCombo | — | ADDED |
| cropButton_ | imageCropButton | — | ADDED |
| extractButton_ | imageExtractButton | — | ADDED |
| removeButton_ | imageRemoveButton | — | ADDED |

**Fixes applied:** imagePropertiesPanel and all interactive widgets received accessibleName.

---

## src/textedit/ (TextEditTool, TextEditModel, ContentStreamEditor, TextEditCommands)

No interactive QWidget instances in these files — they are backend logic classes. TextEditTool
is a QObject but the only UI surface it interacts with is via PdfCanvas (which is covered above).

---

## src/annotations/ (LinkTool, SelectTool)

| widget | objectName | accessibleName | status |
|---|---|---|---|
| linkActionDialog (LinkTool) | linkActionDialog | — | ADDED |
| linkTypeCombo | linkTypeCombo | — | ADDED |
| linkUriEdit | linkUriEdit | — | ADDED |
| linkPageSpin | linkPageSpin | — | ADDED |
| linkNamedCombo | linkNamedCombo | — | ADDED |
| linkLaunchEdit | linkLaunchEdit | — | ADDED |
| linkActionButtonBox | linkActionButtonBox | — | OK-NAME |
| annotationContextMenu (SelectTool) | annotationContextMenu | N/A | OK |
| contextCopyAction | contextCopyAction | N/A (QAction) | OK |
| contextDeleteAction | contextDeleteAction | N/A (QAction) | OK |
| contextPropertiesAction | contextPropertiesAction | N/A (QAction) | OK |

**Fixes applied:** Link dialog edit/combo/spin widgets received accessibleName for pywinauto lookup.

---

## src/forms/ (FormTool, FormManager, JavaScriptEngine, FormDataIO)

No interactive QWidget instances directly in these backend files. Widgets are owned by
FormFieldPropertiesPanel (covered above) and the inline form creation dialog in MainWindow.

MainWindow inline form creation dialog widgets:
| widget | objectName | accessibleName | status |
|---|---|---|---|
| submitFormDialog (inline in MainWindow) | submitFormDialog | — | ADDED |
| submitUrlField | submitUrlField | — | ADDED |
| submitButton | submitButton | — | ADDED |

---

## src/redaction/ (RedactionTool, RedactionManager, RedactionCommands, RedactionVerifier)

No interactive QWidget instances in these backend files. UI surface is RedactionPanel (covered above).

---

## src/tre/ (R2-19 scope extension)

The Text Reconstruction Engine is mostly backend logic. Its sole interactive
QWidget surface is `TreEditDialog` (`src/tre/TreEditDialog.cpp`). `TreRegionTool`
is a `pdfe::annotations::AnnotationTool` (canvas tool, no widgets); all other
files are pipeline/model classes.

### src/tre/TreEditDialog.cpp

| widget | objectName | accessibleName | status |
|---|---|---|---|
| TreEditDialog (self) | treEditDialog | "Scanned Text Editor" (+ description) | ADDED |
| qualityLabel_ | qualityLabel | "Reconstruction Quality Score" | ADDED |
| fontLabel_ | fontLabel | "Matched Font" | ADDED |
| textEdit_ | treTextEdit | "Recognized Text Editor" | ADDED |
| approachGroup | approachGroup | "Reconstruction Approach" | ADDED |
| approachBRadio_ | approachBRadio | "Hybrid Reconstruction (text over image)" | ADDED |
| approachCRadio_ | approachCRadio | "Image Replacement (bitmap)" | ADDED |
| buttonBox_ | buttonBox | "Dialog Actions" | ADDED |

**Fixes applied:** every widget in TreEditDialog received `setAccessibleName`
(the dialog also received `setAccessibleDescription`). Before this phase the
dialog carried `objectName`s only — no accessible names — so screen readers and
the UIA `Name` property were blank. The standard OK/Cancel buttons inside
`buttonBox_` derive their accessible identity from their button text.

---

## src/image/ (R2-19 scope extension)

No interactive QWidget instances in this subsystem. `ImageTool` is a `QObject`
canvas tool (sets `objectName("imageTool")`, no widgets); `ImageManager`,
`ImageContentEditor`, `ExifStripper`, `ImageCommands` are backend/command
classes. The user-facing image editing panel is `ImagePropertiesPanel`, which
lives in `src/ui/` and is already audited above. Scope passes trivially and is
guarded by the source-scan in `ObjectNameAuditScopeTest`.

---

## src/layers/ (R2-19 scope extension)

No interactive QWidget instances in this subsystem. `LayerManager` and the
`*LayerCommand` classes are backend/command classes. The user-facing layer panel
is `LayerPanel`, which lives in `src/ui/` and is already audited above. Scope
passes trivially and is guarded by the source-scan in `ObjectNameAuditScopeTest`.

---

## Summary

| source file | widgets audited | already OK | accessibleName ADDED | DEFERRED |
|---|---|---|---|---|
| MainWindow.cpp | 80+ | 78 | 2 (zoomCombo, colorPickerButton) | 0 |
| PdfCanvas.cpp | 1 | 1 | 0 | 0 |
| TextEditToolBar.cpp | 14 | 3 | 11 | 0 |
| SearchPanel.cpp | 11 | 5 | 6 | 0 |
| PagePanel.cpp | 2 | 0 | 2 | 0 |
| BookmarkPanel.cpp | 2 | 0 | 2 | 0 |
| LayerPanel.cpp | 6 | 6 | 0 | 0 |
| CommentPanel.cpp | 4 | 0 | 4 | 0 |
| AttachmentPanel.cpp | 6 | 6 | 0 | 0 |
| RedactionPanel.cpp | 5 | 0 | 5 | 0 |
| SignaturePanel.cpp | 6 | 0 | 6 | 0 |
| FormFieldPropertiesPanel.cpp | 11 | 1 | 10 | 0 |
| AnnotationPropertiesWidget.cpp | 10 | 4 | 6 | 0 |
| StickyNotePopup.cpp | 3 | 0 | 3 | 0 |
| StatusBar.cpp | 5 | 1 | 4 | 0 |
| PropertiesPanel.cpp | 6 | 0 | 6 | 0 |
| DefaultsLogPanel.cpp | 3 | 0 | 3 | 0 |
| ImagePropertiesPanel.cpp | 7 | 0 | 7 | 0 |
| LinkTool.cpp (annotations) | 6 | 0 | 6 | 0 |
| MainWindow inline dialogs | 3 | 0 | 3 | 0 |
| TreEditDialog.cpp (tre) | 8 | 0 | 8 | 0 |
| ImageTool.cpp / image (backend) | 0 | 0 | 0 | 0 |
| LayerManager.cpp / layers (backend) | 0 | 0 | 0 | 0 |
| **Total** | **219** | **105** | **114** | **0** |

All 106 additions are recorded above. Production changes are strictly limited to
`setAccessibleName()` and `setAccessibleDescription()` calls — no behaviour changes,
no signal/slot rewiring, no widget re-parenting.
