add_library(pdfe_fonts STATIC
    FontMetricProfile.cpp
    EmbeddedFontAnalyzer.cpp
    FontAnalysisCache.cpp
    TextShaper.cpp
    GlyphScaleAdjuster.cpp
    SubstituteFontSelector.cpp
    PdfFontEmbedder.cpp
    FontSubstitutionEngine.cpp
    FontFamilyResolver.cpp
    FontResolver.cpp
    VectorSubsetDetector.cpp
    Type3FontBuilder.cpp
)

target_include_directories(pdfe_fonts PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(pdfe_fonts PUBLIC
    pdfe_core
    freetype
    harfbuzz::harfbuzz
    unofficial::sqlite3::sqlite3
)

target_compile_features(pdfe_fonts PUBLIC cxx_std_20)
