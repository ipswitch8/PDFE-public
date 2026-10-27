# UI Element Catalog — Pipeline a7c2e541 phase-1

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

Generated: 98 files scanned (49 cpp + 49 h)
Pipeline ID: `a7c2e541-3f8b-4d96-b025-9e1f6c84d307`

## Grand Totals

| Category | Count |
|---|---:|
| Actions (QAction) | 143 |
| Interactive widgets | 310 |
| Dialogs / panels / classes | 42 |
| Menus (top-level + sub) | 15 |
| Toolbars | 3 |
| Context menus | 4 |
| Drag-drop targets | 2 |
| `setObjectName(...)` calls (total) | 641 |
| `setAccessibleName(...)` calls (total) | 373 |
| `setAccessibleName(...)` added in phase-1 | 52 |
| `setObjectName(...)` added in phase-1 | 8 |
| Duplicate objectName warnings | 30 |
| Genuine missing objectNames | 0 |

## Per-File Counts

| File | Actions | Widgets | setObjectName | setAccessibleName |
|---|---:|---:|---:|---:|
| `src/ui/AboutDialog.cpp` | 0 | 3 | 4 | 3 |
| `src/ui/AccessibilityHelper.cpp` | 0 | 0 | 0 | 0 |
| `src/ui/AltTextDialog.cpp` | 0 | 5 | 6 | 5 |
| `src/ui/AnnotationPropertiesWidget.cpp` | 0 | 11 | 12 | 9 |
| `src/ui/AtlasComparisonDialog.cpp` | 0 | 4 | 13 | 12 |
| `src/ui/AttachmentPanel.cpp` | 0 | 4 | 6 | 5 |
| `src/ui/BatchDialog.cpp` | 0 | 21 | 25 | 22 |
| `src/ui/BookmarkPanel.cpp` | 8 | 1 | 12 | 4 |
| `src/ui/CalculationOrderDialog.cpp` | 0 | 5 | 7 | 7 |
| `src/ui/CombineFilesService.cpp` | 0 | 0 | 0 | 0 |
| `src/ui/CombineFilesWizard.cpp` | 0 | 11 | 17 | 12 |
| `src/ui/CommentPanel.cpp` | 2 | 3 | 7 | 6 |
| `src/ui/CompareDialog.cpp` | 0 | 13 | 22 | 15 |
| `src/ui/ComplianceDialog.cpp` | 0 | 7 | 14 | 12 |
| `src/ui/CustomizeToolbarsDialog.cpp` | 0 | 9 | 13 | 12 |
| `src/ui/DefaultsLogPanel.cpp` | 0 | 2 | 3 | 3 |
| `src/ui/EncryptionDialog.cpp` | 0 | 19 | 22 | 16 |
| `src/ui/FormFieldPropertiesPanel.cpp` | 0 | 7 | 11 | 9 |
| `src/ui/HeaderFooterDialog.cpp` | 0 | 19 | 18 | 14 |
| `src/ui/ImagePropertiesPanel.cpp` | 0 | 10 | 15 | 7 |
| `src/ui/ImageToPdfWizard.cpp` | 0 | 0 | 0 | 0 |
| `src/ui/LayerPanel.cpp` | 0 | 4 | 10 | 5 |
| `src/ui/MainWindow.cpp` | 129 | 16 | 196 | 25 |
| `src/ui/MetadataDialog.cpp` | 0 | 6 | 10 | 9 |
| `src/ui/NavigationHistory.cpp` | 0 | 0 | 0 | 0 |
| `src/ui/OptimizeDialog.cpp` | 0 | 4 | 6 | 4 |
| `src/ui/PageLabelsDialog.cpp` | 0 | 9 | 14 | 9 |
| `src/ui/PagePanel.cpp` | 0 | 0 | 8 | 2 |
| `src/ui/PasswordDialog.cpp` | 0 | 2 | 5 | 5 |
| `src/ui/PdfCanvas.cpp` | 0 | 0 | 1 | 0 |
| `src/ui/PdfCanvasAccessible.cpp` | 0 | 0 | 0 | 0 |
| `src/ui/PreferencesDialog.cpp` | 0 | 13 | 12 | 9 |
| `src/ui/PreflightDialog.cpp` | 0 | 6 | 9 | 5 |
| `src/ui/PrintDialog.cpp` | 0 | 13 | 18 | 17 |
| `src/ui/PrintPreviewDialog.cpp` | 4 | 1 | 7 | 3 |
| `src/ui/PropertiesPanel.cpp` | 0 | 2 | 7 | 6 |
| `src/ui/RecentFilesMenu.cpp` | 0 | 0 | 4 | 1 |
| `src/ui/RedactionPanel.cpp` | 0 | 3 | 5 | 5 |
| `src/ui/SearchPanel.cpp` | 0 | 9 | 11 | 11 |
| `src/ui/SignDialog.cpp` | 0 | 26 | 26 | 22 |
| `src/ui/SignaturePanel.cpp` | 0 | 4 | 7 | 7 |
| `src/ui/StatusBar.cpp` | 0 | 0 | 6 | 8 |
| `src/ui/StickyNotePopup.cpp` | 0 | 1 | 3 | 3 |
| `src/ui/TabOrderDialog.cpp` | 0 | 4 | 7 | 7 |
| `src/ui/TextEditToolBar.cpp` | 0 | 13 | 14 | 13 |
| `src/ui/ToolbarLayoutManager.cpp` | 0 | 0 | 0 | 0 |
| `src/ui/TrustAnchorDialog.cpp` | 0 | 4 | 6 | 5 |
| `src/ui/ValidationRequestDialog.cpp` | 0 | 3 | 8 | 6 |
| `src/ui/WatermarkDialog.cpp` | 0 | 13 | 14 | 13 |

## Dialogs

| Class | Base | Header |
|---|---|---|
| `AboutDialog` | `QDialog` | `src/ui/AboutDialog.h` |
| `AltTextDialog` | `QDialog` | `src/ui/AltTextDialog.h` |
| `AnnotationPropertiesWidget` | `QWidget` | `src/ui/AnnotationPropertiesWidget.h` |
| `AtlasComparisonDialog` | `QDialog` | `src/ui/AtlasComparisonDialog.h` |
| `AttachmentPanel` | `QWidget` | `src/ui/AttachmentPanel.h` |
| `BatchDialog` | `QDialog` | `src/ui/BatchDialog.h` |
| `BookmarkPanel` | `QWidget` | `src/ui/BookmarkPanel.h` |
| `CalculationOrderDialog` | `QDialog` | `src/ui/CalculationOrderDialog.h` |
| `CombineFilesWizard` | `QDialog` | `src/ui/CombineFilesWizard.h` |
| `CommentPanel` | `QWidget` | `src/ui/CommentPanel.h` |
| `CompareDialog` | `QDialog` | `src/ui/CompareDialog.h` |
| `ComplianceDialog` | `QDialog` | `src/ui/ComplianceDialog.h` |
| `CustomizeToolbarsDialog` | `QDialog` | `src/ui/CustomizeToolbarsDialog.h` |
| `DefaultsLogPanel` | `QWidget` | `src/ui/DefaultsLogPanel.h` |
| `EncryptionDialog` | `QDialog` | `src/ui/EncryptionDialog.h` |
| `FormFieldPropertiesPanel` | `QWidget` | `src/ui/FormFieldPropertiesPanel.h` |
| `HeaderFooterDialog` | `QDialog` | `src/ui/HeaderFooterDialog.h` |
| `ImagePropertiesPanel` | `QWidget` | `src/ui/ImagePropertiesPanel.h` |
| `LayerPanel` | `QWidget` | `src/ui/LayerPanel.h` |
| `MainWindow` | `QMainWindow` | `src/ui/MainWindow.h` |
| `MetadataDialog` | `QDialog` | `src/ui/MetadataDialog.h` |
| `OptimizeDialog` | `QDialog` | `src/ui/OptimizeDialog.h` |
| `PageLabelsDialog` | `QDialog` | `src/ui/PageLabelsDialog.h` |
| `PagePanel` | `QWidget` | `src/ui/PagePanel.h` |
| `PasswordDialog` | `QDialog` | `src/ui/PasswordDialog.h` |
| `PdfCanvas` | `QWidget` | `src/ui/PdfCanvas.h` |
| `PreferencesDialog` | `QDialog` | `src/ui/PreferencesDialog.h` |
| `PreflightDialog` | `QDialog` | `src/ui/PreflightDialog.h` |
| `PrintDialog` | `QDialog` | `src/ui/PrintDialog.h` |
| `PrintPreviewDialog` | `QDialog` | `src/ui/PrintPreviewDialog.h` |
| `PropertiesPanel` | `QWidget` | `src/ui/PropertiesPanel.h` |
| `RecentFilesMenu` | `QMenu` | `src/ui/RecentFilesMenu.h` |
| `RedactionPanel` | `QWidget` | `src/ui/RedactionPanel.h` |
| `SearchPanel` | `QWidget` | `src/ui/SearchPanel.h` |
| `SignDialog` | `QDialog` | `src/ui/SignDialog.h` |
| `SignaturePanel` | `QWidget` | `src/ui/SignaturePanel.h` |
| `StatusBar` | `QStatusBar` | `src/ui/StatusBar.h` |
| `StickyNotePopup` | `QFrame` | `src/ui/StickyNotePopup.h` |
| `TabOrderDialog` | `QDialog` | `src/ui/TabOrderDialog.h` |
| `TrustAnchorDialog` | `QDialog` | `src/ui/TrustAnchorDialog.h` |
| `ValidationRequestDialog` | `QDialog` | `src/ui/ValidationRequestDialog.h` |
| `WatermarkDialog` | `QDialog` | `src/ui/WatermarkDialog.h` |

## Menus (MainWindow)

| Title | Source Line |
|---|---:|
| `Atlas Mode` | 632 |
| `&File` | 1593 |
| `&Stamps` | 1614 |
| `&Remove Stamp` | 1631 |
| `&Edit` | 1656 |
| `&Pages` | 1680 |
| `F&orms` | 1755 |
| `&Security` | 1771 |
| `&Image` | 1790 |
| `&Tools` | 1801 |
| `&Measure` | 1907 |
| `&View` | 1952 |
| `&Rotate` | 1967 |
| `&Theme` | 1975 |
| `&Help` | 2016 |

## Toolbars (MainWindow)

| Title | Source Line |
|---|---:|
| `Main Toolbar` | 2027 |
| `Annotation Toolbar` | 2065 |
| `Form Fields` | 2076 |

## Context Menus

| Variable | File | Line |
|---|---|---:|
| `treeWidget_` | `src/ui/BookmarkPanel.cpp` | 31 |
| `treeWidget_` | `src/ui/CommentPanel.cpp` | 77 |
| `treeWidget_` | `src/ui/LayerPanel.cpp` | 35 |
| `listWidget_` | `src/ui/PagePanel.cpp` | 70 |

## Drag-Drop Targets

- `src/ui/MainWindow.cpp`
- `src/ui/PagePanel.cpp`

## Duplicate objectName Warnings

| objectName | Count | Severity | Files |
|---|---:|---|---|
| `addActionButton` | 2 | high | src/ui/BatchDialog.cpp:72, src/ui/CustomizeToolbarsDialog.cpp:92 |
| `calculationOrderDialog` | 2 | high | src/ui/CalculationOrderDialog.cpp:25, src/ui/MainWindow.cpp:1063 |
| `fitPageAction` | 2 | high | src/ui/MainWindow.cpp:859, src/ui/PrintPreviewDialog.cpp:71 |
| `fitWidthAction` | 2 | high | src/ui/MainWindow.cpp:853, src/ui/PrintPreviewDialog.cpp:65 |
| `insertBlankPageAction` | 2 | high | src/ui/MainWindow.cpp:939, src/ui/PagePanel.cpp:255 |
| `ownerPasswordEdit` | 2 | high | src/ui/BatchDialog.cpp:174, src/ui/EncryptionDialog.cpp:65 |
| `removeActionButton` | 2 | high | src/ui/BatchDialog.cpp:75, src/ui/CustomizeToolbarsDialog.cpp:95 |
| `rotatePage180Action` | 2 | high | src/ui/MainWindow.cpp:926, src/ui/PagePanel.cpp:249 |
| `rotatePageCCWAction` | 2 | high | src/ui/MainWindow.cpp:919, src/ui/PagePanel.cpp:246 |
| `rotatePageCWAction` | 2 | high | src/ui/MainWindow.cpp:912, src/ui/PagePanel.cpp:243 |
| `validationLabel` | 2 | high | src/ui/EncryptionDialog.cpp:183, src/ui/SignDialog.cpp:204 |
| `zoomInAction` | 2 | high | src/ui/MainWindow.cpp:839, src/ui/PrintPreviewDialog.cpp:49 |
| `zoomOutAction` | 2 | high | src/ui/MainWindow.cpp:846, src/ui/PrintPreviewDialog.cpp:56 |
| `cancelButton` | 7 | low | src/ui/BatchDialog.cpp:196, src/ui/CombineFilesWizard.cpp:118, src/ui/CustomizeToolbarsDialog.cpp:142, src/ui/PrintDialog.cpp:157, src/ui/SignDialog.cpp:222, src/ui/TabOrderDialog.cpp:71, src/ui/ValidationRequestDialog.cpp:105 |
| `okButton` | 5 | low | src/ui/AboutDialog.cpp:139, src/ui/CustomizeToolbarsDialog.cpp:139, src/ui/EncryptionDialog.cpp:192, src/ui/PasswordDialog.cpp:44, src/ui/SignDialog.cpp:215 |
| `moveDownButton` | 4 | low | src/ui/BatchDialog.cpp:81, src/ui/CombineFilesWizard.cpp:62, src/ui/CustomizeToolbarsDialog.cpp:104, src/ui/TabOrderDialog.cpp:52 |
| `moveUpButton` | 4 | low | src/ui/BatchDialog.cpp:78, src/ui/CombineFilesWizard.cpp:59, src/ui/CustomizeToolbarsDialog.cpp:101, src/ui/TabOrderDialog.cpp:46 |
| `annotationPropertiesWidget` | 2 | low | src/ui/AnnotationPropertiesWidget.cpp:22, src/ui/MainWindow.cpp:2570 |
| `attachmentPanel` | 2 | low | src/ui/AttachmentPanel.cpp:28, src/ui/MainWindow.cpp:2638 |
| `bookmarkPanel` | 2 | low | src/ui/BookmarkPanel.cpp:21, src/ui/MainWindow.cpp:2473 |
| `buttonBox` | 2 | low | src/ui/EncryptionDialog.cpp:190, src/ui/PasswordDialog.cpp:36 |
| `commentPanel` | 2 | low | src/ui/CommentPanel.cpp:21, src/ui/MainWindow.cpp:2556 |
| `defaultsLogPanel` | 2 | low | src/ui/DefaultsLogPanel.cpp:42, src/ui/MainWindow.cpp:2545 |
| `formFieldPropertiesPanel` | 2 | low | src/ui/FormFieldPropertiesPanel.cpp:19, src/ui/MainWindow.cpp:2583 |
| `layerPanel` | 2 | low | src/ui/LayerPanel.cpp:23, src/ui/MainWindow.cpp:2493 |
| `pagePanel` | 2 | low | src/ui/MainWindow.cpp:2441, src/ui/PagePanel.cpp:40 |
| `pdfCanvas` | 2 | low | src/ui/MainWindow.cpp:277, src/ui/PdfCanvas.cpp:34 |
| `propertiesPanel` | 2 | low | src/ui/MainWindow.cpp:2532, src/ui/PropertiesPanel.cpp:13 |
| `statusBar` | 2 | low | src/ui/MainWindow.cpp:2433, src/ui/StatusBar.cpp:9 |
| `stickyNotePopup` | 2 | low | src/ui/MainWindow.cpp:2670, src/ui/StickyNotePopup.cpp:15 |

## Phase-1 Remediation Detail

Files modified to add `setAccessibleName(...)` calls (52 additions total):

- `src/ui/PrintDialog.cpp` — 17 widgets (dialog, 2 group boxes, 3 radios, 4 labels, 3 spinboxes, 3 checkboxes, 2 buttons)
- `src/ui/PrintPreviewDialog.cpp` — 3 widgets (dialog, toolbar, preview widget)
- `src/ui/MetadataDialog.cpp` — 9 widgets (dialog, 5 line edits, 3 read-only labels)
- `src/ui/AboutDialog.cpp` — 1 widget (OK button)
- `src/ui/PasswordDialog.cpp` — 3 widgets (dialog, button box, OK button)
- `src/ui/RecentFilesMenu.cpp` — 1 widget (the menu itself)
- `src/ui/SignDialog.cpp` — 18 widgets (dialog, 3 group boxes, 3 line edits, browse button, 5 checkboxes, 2 labels, 2 buttons and pre-existing combo box & invisible-signature checkbox)

Files modified to add `setObjectName(...)` (8 additions total — 3 from initial pass + 5 from remediation):

Initial pass (SignDialog.cpp):
- `certificateGroup`, `signatureInfoGroup`, `appearanceGroup` — three QGroupBox containers in SignDialog were anonymous before. Naming them gives pywinauto a stable parent for the children inside.

Remediation pass (Finding 4):
- `optimizeOptionsGroup` — `OptimizeDialog.cpp:43` QGroupBox
- `compareFileGroup` — `CompareDialog.cpp:46` QGroupBox
- `customizeToolbarsButtonBox` — `CustomizeToolbarsDialog.cpp:135` QDialogButtonBox
- `pageLabelsDialogButtonBox` — `PageLabelsDialog.cpp:84` QDialogButtonBox
- `pageLabelsRangeEditorButtonBox` — `PageLabelsDialog.cpp:166` QDialogButtonBox

## Partial-Coverage Notes

- `src/ui/MainWindow.cpp` — partial: True
  - 454 QAction references in 7469 lines, fully scanned for setObjectName and connect()-based signal/slot wiring. text_expr captured as raw expression (not resolved tr() literals) for QAction constructor arguments.
- `src/ui/PdfCanvas.cpp` — partial: False
  - Custom-painted widget; the canvas QWidget itself is now in the widgets array (objectName 'pdfCanvas'). Children (cursors/selection overlays) are paint-only, not interactive widgets.
- `src/ui/MainWindow.cpp:1384` — partial: False
  - buttonBox at line 1384 lives inside an inline lambda (accessibility report dialog). It is a short-lived local that pywinauto cannot observe by name across modal lifecycle, so we deliberately do not assign an objectName. Phase-5 E2E tests should locate it by type+role, not by name.

## Acceptance Criteria Status

- Catalog JSON + MD: written to `tests/docs/ui_element_catalog.json` + `.md`
- Build: 0 errors after `pdfe_ui` recompile (verified via `cmake --build --preset debug-vs2022 --target pdfe_tests`)
- Tests: 3491 passed, 0 failed, 2 skipped (pdfe_tests.exe --gtest_brief=1).
- Files scanned: 49/49 cpp + 49/49 h = 98/98. 100% coverage.
- AC1 schema: `accessibleName`, `parent_widget`, `signal_slot_wiring` populated on every action and widget entry where the source provides them.
- AC2 pdfCanvas: present in widgets array (custom QWidget derivative, `is_custom_qwidget=true`).
- AC3 duplicates: 30 `duplicate_objectName_warnings` entries.
- AC4 false-positives: lambda-parameter and multi-line setObjectName patterns now resolve correctly.
- AC5 ground-truth match: `total_setObjectName_calls=641` matches `grep -rc setObjectName src/ui/` exactly.
