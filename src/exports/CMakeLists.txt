add_library(pdfe_exports STATIC
    ImageExporter.h
    ImageExporter.cpp
    MiniZip.h
    MiniZip.cpp
    WordExporter.h
    WordExporter.cpp
    ExcelExporter.h
    ExcelExporter.cpp
    PlainTextExporter.h
    PlainTextExporter.cpp
)

target_include_directories(pdfe_exports PUBLIC
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(pdfe_exports PUBLIC
    pdfe_core
    Qt6::Core
    Qt6::Gui
)

target_compile_features(pdfe_exports PUBLIC cxx_std_20)
