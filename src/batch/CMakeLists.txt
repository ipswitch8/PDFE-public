add_library(pdfe_batch STATIC
    BatchEngine.cpp
)

target_include_directories(pdfe_batch PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(pdfe_batch PUBLIC
    pdfe_core
    pdfe_compliance
    pdfe_redaction
    pdfe_encryption
    # F24 remediation: the direct-write DocMDP guard (finding 1).
    pdfe_signatures
    unofficial::libmupdf::libmupdf
)

target_compile_features(pdfe_batch PUBLIC cxx_std_20)
