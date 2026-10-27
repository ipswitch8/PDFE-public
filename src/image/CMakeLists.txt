add_library(pdfe_image STATIC
    ImageManager.cpp
    ImageCommands.cpp
    ImageContentEditor.cpp
    ExifStripper.cpp
    ImageTool.cpp
)

target_include_directories(pdfe_image PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(pdfe_image PUBLIC
    pdfe_core
    Qt6::Widgets
)

target_compile_features(pdfe_image PUBLIC cxx_std_20)
