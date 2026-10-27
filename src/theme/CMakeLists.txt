add_library(pdfe_theme STATIC
    ThemeManager.h
    ThemeManager.cpp
)

target_include_directories(pdfe_theme PUBLIC
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(pdfe_theme PUBLIC
    pdfe_app
    Qt6::Widgets
)

target_compile_features(pdfe_theme PUBLIC cxx_std_20)

set_target_properties(pdfe_theme PROPERTIES AUTOMOC ON)
