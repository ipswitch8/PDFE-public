add_library(pdfe_formsubmit STATIC
    FormEncoder.h
    FormEncoder.cpp
    # PDF-48 / GitHub #18: carries forms::FormFieldData::isRequired into the
    # submit path. Header-only; adds no link dependency on pdfe_forms.
    FormFieldBridge.h
    HostAllowlist.h
    HostAllowlist.cpp
    FormSubmitter.h
    FormSubmitter.cpp
)

target_include_directories(pdfe_formsubmit PUBLIC
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(pdfe_formsubmit PUBLIC
    Qt6::Core
    Qt6::Network
)

set_target_properties(pdfe_formsubmit PROPERTIES AUTOMOC ON)

target_compile_features(pdfe_formsubmit PUBLIC cxx_std_20)
