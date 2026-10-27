add_library(pdfe_headerfooter STATIC
    HeaderFooterConfig.h
    HeaderFooterEngine.h
    HeaderFooterEngine.cpp
)

target_include_directories(pdfe_headerfooter PUBLIC
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(pdfe_headerfooter PUBLIC
    pdfe_core
    # F24 remediation: the direct-write DocMDP guard (finding 1).
    pdfe_signatures
    unofficial::libmupdf::libmupdf
)

target_compile_features(pdfe_headerfooter PUBLIC cxx_std_20)
