# Phase 6: Tantivy full-text search bridge.
#
# The Rust crate at ${CMAKE_SOURCE_DIR}/rust/tantivy_bridge is built via
# a custom command that invokes `cargo build --release`. The resulting
# staticlib (tantivy_bridge.lib on Windows) is linked into pdfe_search
# along with the Windows system libraries Tantivy's transitive deps need.

find_program(CARGO_EXECUTABLE cargo
    HINTS $ENV{USERPROFILE}/.cargo/bin
          $ENV{HOME}/.cargo/bin
)
if(NOT CARGO_EXECUTABLE)
    message(FATAL_ERROR
        "cargo not found. Install the Rust toolchain (https://rustup.rs) "
        "and re-run CMake. The Tantivy bridge needs cargo to build.")
endif()

set(TANTIVY_CRATE_DIR "${CMAKE_SOURCE_DIR}/rust/tantivy_bridge")
set(TANTIVY_CRATE_TARGET_DIR "${TANTIVY_CRATE_DIR}/target")
# Rust's cargo emits MSVC-style 'tantivy_bridge.lib' on Windows but
# Unix-style 'libtantivy_bridge.a' on Linux/macOS.
if(WIN32)
    set(TANTIVY_STATIC_LIB
        "${TANTIVY_CRATE_TARGET_DIR}/release/tantivy_bridge.lib")
else()
    set(TANTIVY_STATIC_LIB
        "${TANTIVY_CRATE_TARGET_DIR}/release/libtantivy_bridge.a")
endif()

file(GLOB_RECURSE TANTIVY_CRATE_SOURCES
    CONFIGURE_DEPENDS
    "${TANTIVY_CRATE_DIR}/Cargo.toml"
    "${TANTIVY_CRATE_DIR}/src/*.rs")

# GitHub #199 (M4). rustc embeds file!() into every panic/unwrap/unreachable
# site, and this crate statically links 39 dependencies, so their source paths
# -- rooted at CARGO_HOME, i.e. the build account's HOME directory -- end up in
# pdfe.exe. Measured before this change: 285 of the 418 account-name
# occurrences in the shipped binary set, the single largest cluster, and the one
# a neutral BUILD root cannot touch because the leak is the home directory.
#
# All 285 shared one prefix, so one --remap-path-prefix covers them. The remap
# target is a fixed neutral string, which also makes the output more
# reproducible -- the same property the x64-windows-repro triplet exists for
# (#177).
#
# Scoped to this command via `cmake -E env` rather than set globally: RUSTFLAGS
# is process-wide and a configure-time set would reach any other cargo
# invocation added later.
#
# Changing RUSTFLAGS changes cargo's fingerprint, so the next build rebuilds the
# crate and all 39 dependencies. That is expected.
if(DEFINED ENV{CARGO_HOME})
    set(_pdfe_cargo_home "$ENV{CARGO_HOME}")
elseif(DEFINED ENV{USERPROFILE})
    set(_pdfe_cargo_home "$ENV{USERPROFILE}/.cargo")
else()
    set(_pdfe_cargo_home "$ENV{HOME}/.cargo")
endif()
file(TO_CMAKE_PATH "${_pdfe_cargo_home}" _pdfe_cargo_home)

add_custom_command(
    OUTPUT "${TANTIVY_STATIC_LIB}"
    COMMAND ${CMAKE_COMMAND} -E env
            "RUSTFLAGS=--remap-path-prefix=${_pdfe_cargo_home}=/cargo"
            ${CARGO_EXECUTABLE} build --release
            --manifest-path "${TANTIVY_CRATE_DIR}/Cargo.toml"
    DEPENDS ${TANTIVY_CRATE_SOURCES}
    COMMENT "Building Rust tantivy_bridge (cargo build --release, paths remapped per GitHub #199)"
    VERBATIM
)

add_custom_target(tantivy_bridge_rs ALL
    DEPENDS "${TANTIVY_STATIC_LIB}")

add_library(pdfe_search STATIC
    TantivyBridge.h
    TantivyIndex.h
    TantivyIndex.cpp
    DocumentSearchIndex.h
    DocumentSearchIndex.cpp
)

add_dependencies(pdfe_search tantivy_bridge_rs)

target_include_directories(pdfe_search PUBLIC
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(pdfe_search PUBLIC
    "${TANTIVY_STATIC_LIB}"
    pdfe_core
    Qt6::Core
)

# Tantivy (via its transitive Rust deps) pulls in Windows APIs for
# filesystem, sockets (for some features we don't use but still linked),
# and crypto. These are required when linking a Rust staticlib on MSVC.
if(WIN32)
    target_link_libraries(pdfe_search PUBLIC
        ws2_32     # Windows sockets
        userenv    # user profile info
        ntdll      # native NT APIs
        bcrypt     # cryptography (random)
        advapi32   # registry, misc
        crypt32    # crypto
        secur32    # security support provider
        ncrypt     # CNG
        pdh        # performance data helper (memory info)
        powrprof   # power management (sys info)
        propsys    # property system
        kernel32
    )
endif()

target_compile_features(pdfe_search PUBLIC cxx_std_20)
