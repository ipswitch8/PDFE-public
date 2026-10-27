add_library(pdfe_forms STATIC
    FormFieldType.cpp
    FormManager.cpp
    FormCommands.cpp
    JavaScriptEngine.cpp
    FormDataIO.cpp
    FormTool.cpp
)

target_include_directories(pdfe_forms PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(pdfe_forms PUBLIC
    pdfe_core
    pdfe_annotations
    Qt6::Widgets
)

target_compile_features(pdfe_forms PUBLIC cxx_std_20)
