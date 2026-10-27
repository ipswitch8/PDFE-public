# SUBSYSTEM_INVENTORY — PDFE UI subsystems

**Pipeline phase:** phase-1 (audit + gap inventory only — no production / test code changes)
**Generated:** 2026-04-28
**Companion to:** `UI_MATRIX_GAPS.md`
**Source-of-truth pipeline:** `C:/Users/dev/source/PDFE/.claude/pipeline.json`

This is a one-page summary of every UI-bearing subsystem in PDFE. Each entry
lists the primary source files, an approximate line count, the existing GoogleTest /
PyTest files that already cover the subsystem, and the count of audit rows the
subsystem contributes to `UI_MATRIX_GAPS.md`.

Line counts are approximate (`wc -l` of the .cpp file) and intended only for
relative scale comparison. "existing test file count" counts test_*.cpp + test_ui_*.py
files known to exercise the subsystem at any depth.

---

## text-edit
- description: native + image-dominated text editing (selection, key dispatch, formatting toolbar, ContentStreamEditor save path, EditTextCommand undo/redo, TRE Tr=3 invisible-text overlay)
- files: src/textedit/TextEditTool.cpp (~1900), TextEditTool.h, TextEditCommands.cpp (~250), TextEditCommands.h, TextEditModel.cpp (~600), TextEditModel.h, ContentStreamEditor.cpp (~1100), ContentStreamEditor.h, src/ui/TextEditToolBar.cpp (338), src/ui/MainWindow.cpp text-edit wiring (lines ~3430–3700)
- existing test file count: 11 (test_text_edit_clipboard / commands / integration / model / pipeline / tool / toolbar / width_scaling, test_textedit_tm_regression, test_overlay_font_mapping, test_unified_text_edit)
- gap count in this audit: 65 (rolled up verbatim from prior MATRIX_GAPS.md as UI-TE-NN)

## annotations
- description: 7 tool subclasses (StickyNote, FreeText, Ink, Shape, TextMarkup, Stamp, Link) plus SelectTool; AnnotationManager CRUD; AnnotationCommands undo/redo; CommentPanel; AnnotationPropertiesWidget; StickyNotePopup; toggle-stickies action; comment threading
- files: src/annotations/{AnnotationManager,AnnotationCommands,AnnotationTool,FreeTextTool,InkTool,LinkTool,SelectTool,ShapeTool,StampTool,StickyNoteTool,TextMarkupTool,TextSelector}.cpp (~3000 LOC total), src/ui/CommentPanel.cpp, AnnotationPropertiesWidget.cpp, StickyNotePopup.cpp, AltTextDialog.cpp
- existing test file count: 12 (test_annotation_commands, test_annotation_font_controls, test_annotation_manager, test_annotation_tools, test_comment_panel, test_comment_panel_threading, test_comment_threading, test_link_actions, test_link_tool, test_select_tool_enhanced, test_sticky_note_popup, test_text_selector, test_alt_text, test_alt_text_dialog, test_stamp_and_watermark, test_stamp_tokens, test_follow_link, test_text_color_and_replace)
- gap count in this audit: 35

## forms
- description: 7 widget types (text/checkbox/radio/combo/list/pushbutton/signature), FormManager CRUD, FormCommands undo/redo, FormTool dispatch, JavaScriptEngine (MuJS), FormDataIO XFDF, FormFieldPropertiesPanel, TabOrderDialog, CalculationOrderDialog, ImageToPdfWizard form fields
- files: src/forms/{FormManager,FormCommands,FormDataIO,FormFieldType,FormTool,JavaScriptEngine}.cpp (~2200 LOC), src/ui/FormFieldPropertiesPanel.cpp, TabOrderDialog.cpp, CalculationOrderDialog.cpp, src/formsubmit/ (HostAllowlist, FormEncoder, FormSubmitter)
- existing test file count: 13 (test_form_commands, test_form_creation, test_form_data_io, test_form_field_data, test_form_field_properties_panel, test_form_manager, test_form_submit, test_form_submitter, test_form_tool, test_forms_designer, test_javascript_engine, test_calculation_order, test_tsa_and_taborder, test_theme_and_field_count)
- gap count in this audit: 28

## page-ops
- description: rotate/delete/insert blank/insert from file/move up/down/extract pages; PagePanel thumbnails + drag-drop reorder + multi-select + context menu; PageOperations.cpp graft helper; ReplacePageCommand
- files: src/core/PageCommands.{cpp,h} (~400), src/core/PageOperations.{cpp,h}, src/ui/PagePanel.cpp (~300)
- existing test file count: 4 (test_page_commands, test_page_operations, test_replace_page, test_thumbnails)
- gap count in this audit: 24

## image-edit
- description: ImageTool selection + drag handles, ImageManager CRUD, ImageCommands undo, ImageContentEditor (CTM math), insert/replace/remove/rotate/crop/resize, EXIF stripping, ImageToPdfWizard, ImagePropertiesPanel, ImageExporter
- files: src/image/{ImageManager,ImageTool,ImageContentEditor,ImageCommands,ExifStripper}.cpp (~1500), src/exports/ImageExporter.cpp, src/ui/ImagePropertiesPanel.cpp, src/ui/ImageToPdfWizard.cpp
- existing test file count: 10 (test_image_commands, test_image_content_editor, test_image_data, test_image_exporter, test_image_manager, test_image_properties_panel, test_image_to_pdf_wizard, test_image_tool, test_image_tool_wiring, test_exif_stripper)
- gap count in this audit: 22

## redaction
- description: RedactionTool mark-to-redact, RedactionManager apply, RedactionCommands undo, RedactionVerifier residual-text scan, RedactionPanel, search-and-redact pipeline
- files: src/redaction/{RedactionManager,RedactionTool,RedactionCommands,RedactionVerifier}.cpp (~1100), src/ui/RedactionPanel.cpp
- existing test file count: 6 (test_redaction_apply, test_redaction_commands, test_redaction_edge_cases, test_redaction_manager, test_redaction_verifier, included RedactionPanelTest)
- gap count in this audit: 18

## signatures
- description: SignDialog + SignatureManager (PKCS#7 via OpenSSL bridge), TsaClient timestamping, certification levels (none/no-changes/form-fill/annotations), invisible-signature, SignaturePanel verify list, SignatureCommands clear/restore
- files: src/signatures/{SignatureManager,SignatureCommands,Pkcs7OpenSslBridge,TsaClient}.cpp (~1500), src/ui/SignDialog.cpp, src/ui/SignaturePanel.cpp
- existing test file count: 7 (test_signature_commands, test_signature_data, test_signature_manager, test_signature_panel, test_invisible_and_certification_signatures, test_pkcs7_bridge, test_sign_dialog, test_tsa_and_taborder, test_tsa_fixture)
- gap count in this audit: 16

## panels
- description: read-mostly side panels — SearchPanel (find/replace), BookmarkPanel (CRUD + reorder), LayerPanel (OCG toggle), MeasureTool dock, AttachmentPanel, RedactionPanel, SignaturePanel, CommentPanel, AnnotationPropertiesWidget, FormFieldPropertiesPanel, ImagePropertiesPanel, PropertiesPanel, DefaultsLogPanel
- files: src/ui/{SearchPanel,BookmarkPanel,LayerPanel,AttachmentPanel,RedactionPanel,SignaturePanel,CommentPanel,AnnotationPropertiesWidget,FormFieldPropertiesPanel,ImagePropertiesPanel,PropertiesPanel,DefaultsLogPanel}.cpp (~2500 total), src/measure/MeasureTool.cpp
- existing test file count: 9 (test_search, test_search_performance, test_bookmark_manager, test_bookmarks, test_layers, test_measure, test_attachment_panel, test_attachments, test_signature_panel, test_comment_panel, test_deferred_panels)
- gap count in this audit: 28

## dialogs
- description: AboutDialog, PreferencesDialog, EncryptionDialog, SignDialog, ComplianceDialog, PreflightDialog, MetadataDialog, WatermarkDialog, HeaderFooterDialog, BatchDialog, OptimizeDialog, CompareDialog, PrintDialog, PrintPreviewDialog, PasswordDialog, AltTextDialog, TabOrderDialog, CalculationOrderDialog, AtlasComparisonDialog, TreEditDialog
- files: src/ui/*Dialog.cpp (20 files, ~5000 LOC total)
- existing test file count: 14 (test_about_dialog, test_preferences_dialog, test_encryption_dialog, test_sign_dialog, test_compliance_dialog, test_preflight_dialog, test_metadata_dialog, test_watermark_extended (header_footer), test_batch, test_optimize, test_diff (compare), test_password_dialog, test_alt_text_dialog, test_tabordering, test_tre_edit_dialog, test_atlas_comparison)
- gap count in this audit: 32

## menus
- description: 9 top-level menus (File, Edit, Pages, Forms, Security, Image, Tools, View, Help) plus dynamic Recent Files submenu, Measure submenu, Atlas Mode submenu (canvas context), and stamp dynamic submenu
- files: src/ui/MainWindow.cpp createMenus() (~lines 1056-1322), RecentFilesMenu.cpp
- existing test file count: 2 (test_recent_files, test_startup_ui_changes — sparse coverage; no per-action menu test)
- gap count in this audit: 18

## main-window-chrome
- description: main toolbar, annotation tool bar, form tool bar, text edit tool bar, status bar (page label, zoom label, file name, encryption indicator, form-field count), title bar (dirty indicator, document name, app icon), zoom combo, dock widget visibility toggles
- files: src/ui/MainWindow.cpp toolbar/statusbar setup, src/ui/StatusBar.cpp (90), src/ui/TextEditToolBar.cpp (338)
- existing test file count: 5 (test_toolbar_toggles, test_main_window_icon, test_mainwindow_size_sanity, test_branding_resources, test_pdfe_icons, test_recent_files, test_startup_timer, test_startup_ui_changes)
- gap count in this audit: 14

## drag-drop
- description: drag-drop file open into main window (NOT IMPLEMENTED — verified absent), drag-drop image insert onto canvas (NOT IMPLEMENTED), page reorder via internal-move drag in PagePanel (IMPLEMENTED), bookmark reorder in BookmarkPanel (manual via context menu), comment-panel item drag (NOT IMPLEMENTED)
- files: src/ui/PagePanel.cpp::PageListWidget::dropEvent (line 27, the only Qt drag-drop in src/), src/ui/MainWindow.cpp (NO setAcceptDrops at the window level)
- existing test file count: 1 partial (test_page_commands covers move-page commands but not the drag-drop UI path)
- gap count in this audit: 10

## context-menus
- description: right-click menus on canvas (text-edit context menu — atlas mode submenu only, no general edit/copy), CommentPanel (Reply/Delete), BookmarkPanel (8 actions: New / Rename / Delete / Up / Down / Indent / Outdent), PagePanel (rotate / insert / delete / extract), MainWindow (none), LayerPanel (none verified)
- files: src/ui/PagePanel.cpp (line 230 onContextMenuRequested), CommentPanel.cpp (line 146), BookmarkPanel.cpp (line 139), MainWindow.cpp text-edit context menu (line 3605)
- existing test file count: 0 dedicated; coverage incidental in test_comment_panel and test_bookmarks
- gap count in this audit: 12

## keyboard-nav
- description: every shortcut in the menu bar (~40 actions), Tab traversal across dock widgets, focus policy on PdfCanvas, Page-Up/Page-Down/Home/End/Arrow-key navigation, Ctrl+B/I/U formatting shortcuts in TextEditToolBar, Esc to cancel modal tools, accessibility label presence for screen readers
- files: src/ui/PdfCanvas.cpp (focus + keyPressEvent), MainWindow.cpp shortcut bindings (~lines 336-787), AccessibilityHelper.cpp, PdfCanvasAccessible.cpp
- existing test file count: 4 (test_keyboard_navigation, test_accessibility, test_accessible_names, test_high_contrast, test_hidpi)
- gap count in this audit: 24

## visual-fidelity
- description: icon glyph correctness (alignJustify uses ↕ — known P0 bug; alignCenter uses ↔; align tools all use generic arrows not alignment glyphs), tooltip / status-tip / accessible-name consistency, icon presence on toolbar buttons (currently text-only on annotation toolbar — visual regression risk), title-bar icon, app-icon resolution (multi-DPI), theme switch (light/dark) icon swap, high-DPI scaling
- files: src/app/pdfe.ico, src/app/pdfe_icons.qrc, src/ui/TextEditToolBar.cpp glyphs (lines 159–181), src/ui/MainWindow.cpp annotation toolbar, src/theme/ThemeManager.cpp
- existing test file count: 5 (test_pdfe_icons, test_main_window_icon, test_branding_resources, test_app_resources, test_high_contrast, test_hidpi, test_theme_and_field_count)
- gap count in this audit: 20

---

## Roll-up

| subsystem | gap rows |
|---|---|
| text-edit | 65 |
| annotations | 35 |
| forms | 28 |
| page-ops | 24 |
| image-edit | 22 |
| redaction | 18 |
| signatures | 16 |
| panels | 28 |
| dialogs | 32 |
| menus | 18 |
| main-window-chrome | 14 |
| drag-drop | 10 |
| context-menus | 12 |
| keyboard-nav | 24 |
| visual-fidelity | 20 |
| **Total** | **366** |

The 65 text-edit rows are the verbatim roll-up of `tests/fixtures/textedit_repro/MATRIX_GAPS.md`
re-IDed under the `UI-TE-` prefix; their cross-reference table lives at the top of
`UI_MATRIX_GAPS.md`.
