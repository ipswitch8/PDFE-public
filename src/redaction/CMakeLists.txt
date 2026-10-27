add_library(pdfe_redaction STATIC
    RedactionManager.cpp
    RedactionCommands.cpp
    RedactionVerifier.cpp
    RedactionScrubber.cpp
    RedactionTool.cpp
)

target_include_directories(pdfe_redaction PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(pdfe_redaction PUBLIC
    pdfe_core
    pdfe_annotations
    Qt6::Widgets
)

target_compile_features(pdfe_redaction PUBLIC cxx_std_20)
