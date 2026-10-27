add_library(pdfe_core STATIC
    Error.cpp
    MuPdfContext.cpp
    QpdfWrapper.cpp
    PathSecurity.cpp
    # GitHub #107 -- Mark-of-the-Web propagation on Save As.
    ZoneIdentifier.cpp
    # Meta-audit F13 (SEC-40/41/42): fail-closed pre-parse guard shared by every
    # attacker-reachable XML entry point (XMP, hOCR, XFDF).
    XmlSafety.cpp
    PreflightOpen.cpp
    # Meta-audit F26 REMEDIATION (security-audit gate): the single shared home
    # for "where are the signatures?". EncryptionManager's rewrite-integrity
    # probe and DocMdpEnforcer both read it from here rather than each keeping
    # a private traversal that can drift narrower than the others.
    PdfSignatureProbe.cpp
    # PDF-05 / GitHub #3: detect (and give the UI something to REPORT about)
    # a PDF 2.0 unencrypted wrapper, whose visible page is a cover sheet.
    UnencryptedWrapperProbe.cpp
    AtomicFileReplace.cpp
    # GitHub #100 / WIN-78: advisory free-space pre-check before a save.
    SaveSpacePrecheck.cpp
    # GitHub #99 / WIN-75: SetThreadExecutionState seam — raised for the
    # duration of a batch, and READABLE so a test can prove it was released.
    SleepInhibitor.cpp
    # WIN-87 wider class (phase-60): the temp files that are not documents and
    # are still attack surface — unpredictable+pinned working files, and
    # hardened writes to the one fixed name that has to stay fixed.
    TempFileSafety.cpp
    Document.cpp
    FormXObjectDepthGuard.cpp
    Page.cpp
    PageOperations.cpp
    PageArtifactGraft.cpp
    PageCommands.cpp
    PageLabels.cpp
    CoordinateTransform.cpp
    UndoManager.cpp
    AutosaveManager.cpp
    TextLine.cpp
    TextBlock.cpp
    TextExtractor.cpp
    FontNameUtil.cpp
    ToUnicodeRecovery.cpp
    SearchEngine.cpp
    PrintEngine.cpp
    BookmarkExtractor.cpp
    BookmarkManager.cpp
    SystemFontDiscovery.cpp
    BundledFonts.cpp
    BasicFontMatcher.cpp
)

target_include_directories(pdfe_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(pdfe_core PUBLIC
    unofficial::libmupdf::libmupdf
    qpdf::libqpdf
    Qt6::Widgets
    Qt6::PrintSupport
)

# Platform-specific font discovery libraries
if(WIN32)
    # dwrite  — system font discovery.
    # advapi32 — Get/SetNamedSecurityInfoW + SetSecurityInfo: DACL preservation
    #            across a stage-and-rename (AtomicFileReplace, F58-b/phase-53).
    # bcrypt   — BCryptGenRandom: the CSPRNG behind the unpredictable staging
    #            file name (AtomicFileReplace::StagingFile, F58-a/phase-53).
    target_link_libraries(pdfe_core PUBLIC dwrite advapi32 bcrypt)
endif()

target_compile_features(pdfe_core PUBLIC cxx_std_20)
