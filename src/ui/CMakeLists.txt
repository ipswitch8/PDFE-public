add_library(pdfe_ui STATIC
    branding.qrc
    action_icons.qrc
    MainWindow.cpp
    TestPipeFocusGuard.cpp   # GitHub #303
    PdfCanvas.cpp
    PagePanel.cpp
    PropertiesPanel.cpp
    StatusBar.cpp
    SearchPanel.cpp
    PrintDialog.cpp
    PrintPreviewDialog.cpp
    BookmarkPanel.cpp
    CommentPanel.cpp
    AnnotationPropertiesWidget.cpp
    FormFieldPropertiesPanel.cpp
    TextEditToolBar.cpp
    RedactionPanel.cpp
    PasswordDialog.cpp
    EncryptionDialog.cpp
    SignaturePanel.cpp
    SignDialog.cpp
    ImagePropertiesPanel.cpp
    MetadataDialog.cpp
    RecentFilesMenu.cpp
    AboutDialog.cpp
    AccessibilityHelper.cpp
    PdfCanvasAccessible.cpp
    StickyNotePopup.cpp
    ComplianceDialog.cpp
    PreferencesDialog.cpp
    DefaultsLogPanel.cpp
    AtlasComparisonDialog.cpp
    TabOrderDialog.cpp
    BatchDialog.cpp
    CompareDialog.cpp
    ToolbarLayoutManager.cpp
    CustomizeToolbarsDialog.cpp
    CombineFilesService.cpp
    CombineFilesWizard.cpp
    LayerPanel.cpp
    WatermarkDialog.cpp
    HeaderFooterDialog.cpp
    OptimizeDialog.cpp
    AltTextDialog.cpp
    InsertImageDialog.cpp
    AttachmentPanel.cpp
    PreflightDialog.cpp
    CalculationOrderDialog.cpp
    NavigationHistory.cpp
    ImageToPdfWizard.cpp
    PageLabelsDialog.cpp
    # Pipeline F phase-10: trust anchor management UI.
    TrustAnchorDialog.cpp
)

target_include_directories(pdfe_ui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(pdfe_ui PUBLIC
    pdfe_app
    pdfe_rendering
    pdfe_core
    pdfe_annotations
    pdfe_forms
    pdfe_textedit
    pdfe_tre
    pdfe_redaction
    pdfe_encryption
    pdfe_signatures
    pdfe_image
    pdfe_metadata
    pdfe_compliance
    pdfe_batch
    pdfe_diff
    pdfe_layers
    pdfe_headerfooter
    pdfe_measure
    pdfe_optimize
    pdfe_search
    pdfe_attachments
    pdfe_reflow
    pdfe_exports
    pdfe_theme
    # GitHub #18 (PDF-48). Until this line, pdfe_formsubmit was a static
    # library linked into NOTHING: not pdfe_ui, not the pdfe executable. The
    # required-field enforcement added for #18 was therefore correct, tested,
    # and unreachable — an incomplete form was refused in a unit test and
    # submitted fine in the shipped product. Linking it here makes pdfe.exe
    # inherit it transitively (pdfe -> pdfe_ui -> pdfe_formsubmit).
    #
    # The single UI call site is MainWindow::submitCurrentFormTo(), which is
    # gated on AppSettings::externalActionsAllowed() and re-asserted by
    # tests/test_action_egress_inertness.cpp.
    pdfe_formsubmit
    Qt6::Widgets
    Qt6::PrintSupport
)

target_compile_features(pdfe_ui PUBLIC cxx_std_20)
