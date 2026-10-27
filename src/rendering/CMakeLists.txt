add_library(pdfe_rendering STATIC
    TileCache.cpp
    TileRenderer.cpp
    ThumbnailRenderer.cpp
    TilePrefetcher.cpp
)

target_include_directories(pdfe_rendering PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(pdfe_rendering PUBLIC
    pdfe_core
    Qt6::Widgets
)

target_compile_features(pdfe_rendering PUBLIC cxx_std_20)
