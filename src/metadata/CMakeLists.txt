add_library(pdfe_metadata STATIC
    MetadataManager.cpp
    MetadataCommands.cpp
)

target_include_directories(pdfe_metadata PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(pdfe_metadata PUBLIC
    pdfe_core
    Qt6::Widgets
)

# R3-22: MetadataManager syncs an existing XMP packet after an Info edit via
# XmpMetadataWriter::syncFromInfo. Used only in the .cpp, so PRIVATE scope.
target_link_libraries(pdfe_metadata PRIVATE pdfe_compliance)

target_compile_features(pdfe_metadata PUBLIC cxx_std_20)
