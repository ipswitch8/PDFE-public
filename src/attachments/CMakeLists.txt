add_library(pdfe_attachments STATIC
    AttachmentData.h
    AttachmentManager.h
    AttachmentManager.cpp
)

target_include_directories(pdfe_attachments PUBLIC
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(pdfe_attachments PUBLIC
    pdfe_core
    unofficial::libmupdf::libmupdf
)

target_compile_features(pdfe_attachments PUBLIC cxx_std_20)
