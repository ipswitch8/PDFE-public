add_library(pdfe_textedit STATIC
    TextEditModel.cpp
    ContentStreamEditor.cpp
    TextEditCommands.cpp
    TextEditTool.cpp
)

target_include_directories(pdfe_textedit PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(pdfe_textedit PUBLIC
    pdfe_core
    pdfe_fonts
    Qt6::Widgets
)

target_compile_features(pdfe_textedit PUBLIC cxx_std_20)
