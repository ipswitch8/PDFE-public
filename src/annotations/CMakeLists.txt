add_library(pdfe_annotations STATIC
    AnnotationManager.cpp
    AnnotationCommands.cpp
    TextSelector.cpp
    StickyNoteTool.cpp
    TextMarkupTool.cpp
    InkTool.cpp
    ShapeTool.cpp
    FreeTextTool.cpp
    SelectTool.cpp
    LinkTool.cpp
    StampTool.cpp
    ImageStampLibrary.cpp
    FileAttachmentTool.cpp
    CropTool.cpp
)

target_include_directories(pdfe_annotations PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(pdfe_annotations PUBLIC
    pdfe_core
    pdfe_attachments
    Qt6::Widgets
)

target_compile_features(pdfe_annotations PUBLIC cxx_std_20)
