add_library(pdfe_diff STATIC
    DiffEngine.cpp
)

target_include_directories(pdfe_diff PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(pdfe_diff PUBLIC
    pdfe_core
    Qt6::Widgets
)

target_compile_features(pdfe_diff PUBLIC cxx_std_20)
