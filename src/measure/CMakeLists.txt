add_library(pdfe_measure STATIC
    MeasureTypes.h
    MeasureCalc.h
    MeasureCalc.cpp
    MeasureTool.h
    MeasureTool.cpp
    # GitHub #21 / gap PDF-57: the read side of ISO 32000-1 §12.9 /Measure.
    MeasureDictReader.h
    MeasureDictReader.cpp
)

target_include_directories(pdfe_measure PUBLIC
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(pdfe_measure PUBLIC
    Qt6::Core
    Qt6::Gui
    pdfe_core
    pdfe_annotations
)

target_compile_features(pdfe_measure PUBLIC cxx_std_20)
