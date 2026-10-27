add_library(pdfe_compliance STATIC
    XmpMetadataWriter.cpp
    IccProfileManager.cpp
    FontComplianceChecker.cpp
    ProhibitedFeatureScanner.cpp
    PdfAValidator.cpp
    PdfAConverter.cpp
    StructureTreeBuilder.cpp
    AutoTagger.cpp
    ContentStreamTagger.cpp
    PageFurnitureDetector.cpp
    PdfUAConverter.cpp
    ComplianceManager.cpp
)

target_include_directories(pdfe_compliance PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(pdfe_compliance PUBLIC
    pdfe_core
    pdfe_fonts
    pugixml::pugixml
    qpdf::libqpdf
    Qt6::Widgets
)

target_compile_features(pdfe_compliance PUBLIC cxx_std_20)
