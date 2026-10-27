add_library(pdfe_reflow STATIC
    ReflowTypes.h
    TextReflowEngine.h
    TextReflowEngine.cpp
    AlignmentWriter.h
    AlignmentWriter.cpp
    AlignmentInjector.h
    AlignmentInjector.cpp
    AlignmentPipeline.h
    AlignmentPipeline.cpp
    BlockParamsBuilder.h
    BlockParamsBuilder.cpp
)

target_include_directories(pdfe_reflow PUBLIC
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(pdfe_reflow PUBLIC
    Qt6::Core
    pdfe_core
    # F24 remediation: the direct-write DocMDP guard (finding 1).
    pdfe_signatures
    unofficial::libmupdf::libmupdf
)

target_compile_features(pdfe_reflow PUBLIC cxx_std_20)
