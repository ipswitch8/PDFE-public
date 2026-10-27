add_library(pdfe_encryption STATIC
    EncryptionManager.cpp
)

target_include_directories(pdfe_encryption PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(pdfe_encryption PUBLIC
    pdfe_core
    Qt6::Widgets
)

target_compile_features(pdfe_encryption PUBLIC cxx_std_20)
