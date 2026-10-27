add_library(pdfe_layers STATIC
    LayerManager.cpp
)

target_include_directories(pdfe_layers PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(pdfe_layers PUBLIC
    pdfe_core
    unofficial::libmupdf::libmupdf
)

target_compile_features(pdfe_layers PUBLIC cxx_std_20)
