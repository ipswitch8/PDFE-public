add_library(pdfe_optimize STATIC
    OptimizeTypes.h
    OptimizeEngine.h
    OptimizeEngine.cpp
    GrayscaleConverter.h
    GrayscaleConverter.cpp
)

target_include_directories(pdfe_optimize PUBLIC
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(pdfe_optimize PUBLIC
    pdfe_core
    # F24 remediation: the direct-write DocMDP guard (finding 1).
    pdfe_signatures
    unofficial::libmupdf::libmupdf
)

target_compile_features(pdfe_optimize PUBLIC cxx_std_20)
