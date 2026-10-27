add_library(pdfe_tre STATIC
    TreTypes.cpp
    HocrParser.cpp
    CoordMapper.cpp
    OcrPipeline.cpp
    BgColorDetector.cpp
    ImageMasker.cpp
    QualityAssessor.cpp
    BitmapCompositor.cpp
    FontMatcher.cpp
    ImageSimilarity.cpp
    VisualFontRanker.cpp
    GlyphAtlas.cpp
    GlyphTemplateCache.cpp
    GlyphAtlasCompositor.cpp
    GlyphSynthesizer.cpp
    LineFontVerifier.cpp
    VectorFontAtlasBuilder.cpp
    VectorEnrichedAtlasBuilder.cpp
    SynthesizedType3FontBundler.cpp
    AutoMissingGlyphResolver.cpp
    EnrichedAtlas.cpp
    ScanStyleTransfer.cpp
    AtlasBuilder.cpp
    PdfContentWriter.cpp
    RegionReconstructor.cpp
    ImageReplacer.cpp
    TreRegionTool.cpp
    TreEditDialog.cpp
    TreCommands.cpp
    CNNFontClassifier.cpp
    CNNFontRanker.cpp
    OcrAllPagesCommand.cpp
    LanczosResize.cpp
)

target_include_directories(pdfe_tre PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(pdfe_tre PUBLIC
    pdfe_core
    pdfe_fonts
    pdfe_annotations
    Tesseract::libtesseract
    pugixml::pugixml
    Qt6::Widgets
)

if(PDFE_HAS_ONNXRUNTIME)
    target_link_libraries(pdfe_tre PUBLIC onnxruntime::onnxruntime)
endif()

target_compile_features(pdfe_tre PUBLIC cxx_std_20)
