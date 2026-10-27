cmake_minimum_required(VERSION 3.25)
project(pdfe VERSION 0.3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Qt6 requires AUTOMOC/AUTOUIC/AUTORCC
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Find packages
find_package(Qt6 REQUIRED COMPONENTS Widgets Test PrintSupport Network Svg)

# GitHub #128 -- the i18n pipeline used to produce nothing. translations/*.ts
# existed, no .qm was ever built, and no lrelease step existed anywhere, so the
# application was English-only regardless of locale while the repo read as
# though translation were supported.
#
# LinguistTools supplies lrelease (.ts -> .qm). It arrives with the `qttools`
# vcpkg dependency added for this issue. REQUIRED rather than QUIET on purpose:
# a silently-absent tool is exactly how this shipped broken for so long. If the
# component is missing the configure should fail loudly, not quietly resume
# producing nothing.
find_package(Qt6 REQUIRED COMPONENTS LinguistTools)
find_package(unofficial-libmupdf CONFIG REQUIRED)
find_package(unofficial-mujs CONFIG REQUIRED)
find_package(qpdf REQUIRED)
find_package(freetype CONFIG REQUIRED)
find_package(harfbuzz CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(Tesseract CONFIG REQUIRED)
find_package(pugixml CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
# ONNX Runtime -- pre-built binaries (too large to build from source via vcpkg)
#
# HASH-PINNED (GitHub #72). This used to be a bare EXISTS probe: if a file named
# onnxruntime.lib was present, it was linked, and onnxruntime.dll next to it was
# REDISTRIBUTED in the installer. Existence is not identity. The directory is
# .gitignored and populated by a download, so nothing in the repository recorded
# which bytes were being shipped to users under our code signature -- and on the
# self-hosted runner the tree survives between jobs, so anything that once wrote
# there would have been picked up silently by every later build.
#
# These are the SHA-256 digests of the files inside the official
# onnxruntime-win-x64-1.20.1 release archive, cross-checked against the copy
# already in use so pinning changes no behaviour.
set(ONNXRUNTIME_ROOT "${CMAKE_SOURCE_DIR}/external/onnxruntime-win-x64-1.20.1")
set(ONNXRUNTIME_LIB_SHA256 "c5a4f0b1af1e8f1edd11391e1a15a7d52832d94a1a67cf6b8f22ed29f863a06c")
set(ONNXRUNTIME_DLL_SHA256 "4cb41e89b8bf30578e1dd95e9c40292d61974a4bfcd666409302c4f0c5aa8ce0")
set(ONNXRUNTIME_PROVIDERS_DLL_SHA256 "3b164f0019863266971843baca9551f015db7e502e2d9c4575f74c6a2bbbb78a")

if(EXISTS "${ONNXRUNTIME_ROOT}/lib/onnxruntime.lib")
    # Absent is a supported state -- the re-ranker is optional and simply turns
    # off. PRESENT-BUT-WRONG is not: it means we would link and ship bytes we
    # cannot account for, so it is fatal rather than a downgrade to OFF.
    # Downgrading would be worse than useless here: the build would go green
    # having quietly dropped a feature because someone tampered with a DLL.
    foreach(_ort_file "lib/onnxruntime.lib;${ONNXRUNTIME_LIB_SHA256}"
                      "lib/onnxruntime.dll;${ONNXRUNTIME_DLL_SHA256}"
                      "lib/onnxruntime_providers_shared.dll;${ONNXRUNTIME_PROVIDERS_DLL_SHA256}")
        list(GET _ort_file 0 _ort_rel)
        list(GET _ort_file 1 _ort_expected)
        set(_ort_path "${ONNXRUNTIME_ROOT}/${_ort_rel}")
        if(NOT EXISTS "${_ort_path}")
            message(FATAL_ERROR
                "ONNX Runtime is present but incomplete: ${_ort_rel} is missing "
                "from ${ONNXRUNTIME_ROOT}. Re-extract the pinned "
                "onnxruntime-win-x64-1.20.1 release archive.")
        endif()
        file(SHA256 "${_ort_path}" _ort_actual)
        if(NOT _ort_actual STREQUAL _ort_expected)
            message(FATAL_ERROR
                "ONNX Runtime SHA256 mismatch for ${_ort_rel}:\n"
                "  expected ${_ort_expected}\n"
                "  actual   ${_ort_actual}\n"
                "These bytes are linked into PDFE and redistributed in the "
                "installer under our code signature. Refusing to build against "
                "an unverified copy -- delete ${ONNXRUNTIME_ROOT} and re-extract "
                "the pinned onnxruntime-win-x64-1.20.1 release archive.")
        endif()
    endforeach()
    message(STATUS "ONNX Runtime SHA256 pins verified (3 files)")

    set(PDFE_HAS_ONNXRUNTIME ON)
    add_compile_definitions(PDFE_HAS_ONNXRUNTIME=1)

    add_library(onnxruntime::onnxruntime SHARED IMPORTED GLOBAL)
    set_target_properties(onnxruntime::onnxruntime PROPERTIES
        IMPORTED_IMPLIB "${ONNXRUNTIME_ROOT}/lib/onnxruntime.lib"
        IMPORTED_LOCATION "${ONNXRUNTIME_ROOT}/lib/onnxruntime.dll"
        INTERFACE_INCLUDE_DIRECTORIES "${ONNXRUNTIME_ROOT}/include"
    )
    message(STATUS "ONNX Runtime found (pre-built) -- CNN font re-ranker enabled")
else()
    set(PDFE_HAS_ONNXRUNTIME OFF)
    message(STATUS "ONNX Runtime not found -- CNN font re-ranker disabled")
endif()

# --- Security testing harness (OPT-IN) ---
# Defaults OFF. Enabling adds tests under the CTest label `security`
# only — they are NOT part of the default ctest invocation, NOT
# part of pdfe_tests.exe, and NOT a CI green-bar requirement. See
# docs/security-testing.md (created in pipeline phase 9).
#
# These options are declared HERE (before the src/ subdirectories) rather
# than after, because PDFE_ENABLE_FUZZING gates tree-wide instrumentation
# compile options that every PDFE target must inherit — they have to be in
# effect before the first add_subdirectory(src/...) call below.
option(PDFE_ENABLE_SECURITY_TESTS
       "Build the opt-in security test harness under tests/security/. \
Tests are registered under CTest label 'security' and excluded from \
the default ctest run."
       OFF)
option(PDFE_ENABLE_FUZZING
       "Build the libFuzzer + AFL++ harnesses under tests/security/fuzz/. \
Requires clang-cl with -fsanitize=fuzzer support. Implies \
PDFE_ENABLE_SECURITY_TESTS=ON."
       OFF)

# SEC-45 / GitHub #55 -- compile-time gate for the PDFE_TEST_PIPES seam.
#
# The pipes let an external process drive the running application: write a
# QAction objectName into a hidden QLineEdit and the app activates that tool;
# query a dialog's child inventory and read it back. They exist because
# pywinauto's UIA backend cannot enumerate a Qt modal dialog on the CI host, and
# they are genuinely needed there.
#
# What they should NOT be is present in a shipped binary. The seam was gated on
# an ENVIRONMENT VARIABLE alone, and an environment variable is set by whoever
# launches the process -- not by whoever ships it. `TestDialogSeam.h` said the
# seam was "inert in production ... never shipped", which was true of the env var
# and false of the code.
#
# DEFAULT ON so every existing developer and CI configure is unchanged; the
# packaging path is what must turn it OFF. GitHub #182 found that NO build path
# in this repository ever did -- not a preset, not a workflow, not the signing
# script -- so the #55 mitigation had never reached a shipped binary, v0.2.0
# included. The OFF tree now exists and is the ONLY tree an installer may be
# packaged from:
#
#     cmake --preset windows-vs2022-package     # -> build/vs2022-package
#
# CONFIGURE-scoped rather than per-target because pdfe_ui is a static library
# linked into BOTH pdfe.exe and pdfe_tests.exe -- one compilation serves both, so
# a per-target definition could not differ between them -- and the seam is not
# confined to pdfe_ui anyway, since dialogs across several libraries include
# src/ui/TestDialogSeam.h. That is why the fix is a SECOND CONFIGURE TREE and
# not a target property.
#
# The default MUST stay ON: pdfe_tests.exe needs the seams compiled in. Both
# halves -- default ON here, OFF on every packaging path -- are held by
# tools/gate/check_packaging_testing_flag.py so neither can drift alone.
option(PDFE_TESTING
       "Compile the test-only introspection seams (PDFE_TEST_PIPES). Turn OFF \
for release/packaging builds so the seam is statically dead."
       ON)
if(PDFE_ENABLE_FUZZING)
    set(PDFE_ENABLE_SECURITY_TESTS ON CACHE BOOL "" FORCE)
endif()

if(PDFE_TESTING)
    # Tree-wide: the seam appears in pdfe_ui and in dialogs across several libs,
    # and tests query the capability through the same header.
    add_compile_definitions(PDFE_TESTING)
    message(STATUS "PDFE_TESTING=ON: test introspection seams (PDFE_TEST_PIPES) compiled in")
else()
    message(STATUS "PDFE_TESTING=OFF: test introspection seams are statically dead (release posture)")
endif()

# --- Fuzzing instrumentation (applied tree-wide) ---
# When PDFE_ENABLE_FUZZING is on (clang-cl + libFuzzer), the *code under
# test* — pdfe_core and every linked pdfe_* lib — must be compiled with
# SanitizerCoverage + ASan/UBSan, not just the harness translation units.
# Otherwise libFuzzer gets zero coverage feedback from the product code
# (degenerating to blind random input) and ASan never watches it, so a
# "no findings" campaign is a meaningless false-green. The harness exes
# additionally get full `-fsanitize=fuzzer` (the libFuzzer driver/main)
# in tests/security/fuzz/CMakeLists.txt; here we give the rest of the
# tree `fuzzer-no-link` so coverage is collected without pulling a second
# main().
#
# _DISABLE_STRING/VECTOR_ANNOTATION: the vcpkg dependencies (MuPDF, Qt,
# QPDF, …) are prebuilt WITHOUT ASan, so their objects carry the MSVC STL
# marker annotate_string=0 / annotate_vector=0. Instrumented PDFE objects
# would carry =1, and lld-link's /failifmismatch then refuses to combine
# them ("mismatch detected for 'annotate_string'") — which is exactly why
# every prior fuzz build failed at link. Disabling the container
# annotations makes the instrumented PDFE code link cleanly against the
# non-ASan deps. We keep full heap / stack / use-after-free / UB
# detection; only the std::string / std::vector red-zone annotations are
# dropped.
if(PDFE_ENABLE_FUZZING)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        add_compile_options(
            -fsanitize=fuzzer-no-link,address,undefined
            -fno-omit-frame-pointer
            -O1
            -g
        )
        add_compile_definitions(_DISABLE_STRING_ANNOTATION _DISABLE_VECTOR_ANNOTATION)
        message(STATUS
            "PDFE fuzzing: tree-wide coverage+ASan/UBSan instrumentation enabled "
            "(STL container annotations disabled for non-ASan dependency linkage)")
    else()
        # SEC-15 / GitHub #46 -- FATAL_ERROR, not WARNING.
        #
        # This branch's own message says it: without clang the code under test is
        # NOT instrumented, so "fuzz results would be a false-green". A WARNING
        # that produces a false-green is the worst of both worlds -- the build
        # succeeds, the campaign runs, it reports no findings, and the no-findings
        # result means nothing because libFuzzer had zero coverage feedback and
        # ASan was never watching.
        #
        # `PDFE_ENABLE_FUZZING` defaults to OFF, so ON here IS the explicit
        # request the gap row asks us to distinguish: nobody arrives in this
        # branch by accident. An ordinary MSVC build never reaches it.
        #
        # The issue names only tests/security/fuzz/CMakeLists.txt:38. This site
        # is fixed too because it is the same defect -- and it is the more
        # dangerous half, since that one skips the harnesses (visibly: no
        # binaries) while this one silently ships uninstrumented ones.
        message(FATAL_ERROR
            "PDFE_ENABLE_FUZZING=ON but CMAKE_CXX_COMPILER_ID=${CMAKE_CXX_COMPILER_ID} "
            "is not Clang/clang-cl. The code under test would NOT be instrumented, "
            "so libFuzzer would get zero coverage feedback and ASan would never "
            "watch it -- a campaign that reports no findings while establishing "
            "nothing. Refusing to configure.\n"
            "    Reconfigure with -DCMAKE_CXX_COMPILER=clang-cl, or drop "
            "-DPDFE_ENABLE_FUZZING=ON.")
    endif()
endif()

# Subdirectories
add_subdirectory(src/core)
add_subdirectory(src/rendering)
add_subdirectory(src/ui)
add_subdirectory(src/annotations)
add_subdirectory(src/forms)
add_subdirectory(src/textedit)
add_subdirectory(src/fonts)
add_subdirectory(src/tre)
add_subdirectory(src/redaction)
add_subdirectory(src/encryption)
add_subdirectory(src/signatures)
add_subdirectory(src/image)
add_subdirectory(src/metadata)
add_subdirectory(src/compliance)
add_subdirectory(src/batch)
add_subdirectory(src/diff)
add_subdirectory(src/layers)
add_subdirectory(src/headerfooter)
add_subdirectory(src/measure)
add_subdirectory(src/optimize)
add_subdirectory(src/attachments)
add_subdirectory(src/search)
add_subdirectory(src/exports)
add_subdirectory(src/theme)
add_subdirectory(src/reflow)
add_subdirectory(src/formsubmit)
add_subdirectory(src/app)

# Testing
option(PDFE_BUILD_TESTS "Build tests" ON)
if(PDFE_BUILD_TESTS)
    enable_testing()
    find_package(GTest CONFIG REQUIRED)
    add_subdirectory(tests)
endif()

# --- Security testing harness wiring (OPT-IN) ---
# The PDFE_ENABLE_SECURITY_TESTS / PDFE_ENABLE_FUZZING options are declared
# earlier (above the src/ subdirectories) so fuzz instrumentation can be
# applied tree-wide. This block only wires in the test subdirectory, which
# must come after the pdfe_* targets it links against are defined.
if(PDFE_ENABLE_SECURITY_TESTS)
    if(NOT PDFE_BUILD_TESTS)
        message(WARNING
            "PDFE_ENABLE_SECURITY_TESTS=ON requires PDFE_BUILD_TESTS=ON; \
forcing tests on.")
        set(PDFE_BUILD_TESTS ON CACHE BOOL "" FORCE)
        enable_testing()
    endif()
    add_subdirectory(tests/security)
endif()

# --- Installation & Packaging ---
include(InstallRequiredSystemLibraries)

# Runtime DLL deployment for install tree
if(WIN32)
    # Meta-audit follow-up F39 (backlog row D-6). This used to be hardcoded to
    # "${CMAKE_BINARY_DIR}/vcpkg_installed/...", which silently assumed the
    # DEFAULT manifest-mode install location. The two-stage libFuzzer build
    # (.github/workflows/windows-fuzz.yml) does not use it: stage 1 installs the
    # static-CRT deps to a SEPARATE root and stage 2 configures with
    # VCPKG_MANIFEST_MODE=OFF + -DVCPKG_INSTALLED_DIR=<that root>. With the old
    # line, _vcpkg_base pointed at a directory that stage 2 never creates, and
    # the windeployqt find_program below then hard-failed the configure with
    # FATAL_ERROR. Prefer VCPKG_INSTALLED_DIR when it is set; in ordinary
    # manifest-mode builds the vcpkg toolchain sets it to exactly the old value,
    # so this is behaviour-preserving there.
    if(VCPKG_INSTALLED_DIR)
        set(_vcpkg_base "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
    else()
        set(_vcpkg_base "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}")
    endif()

    # Explicit list of required runtime DLLs (no glob to avoid bundling
    # unnecessary PostgreSQL, Qt6Test, D-Bus, pkgconf, etc.)
    # Names here follow the RELEASE naming convention: ${name}.dll
    # Debug convention: ${name}d.dll or ${name}.dll (both tried, OPTIONAL)
    set(_required_dlls
        # NOTE: Qt6 module DLLs *and* every Qt plugin are deployed by windeployqt
        # (see the install(CODE) block near the end of this file) -- authoritative,
        # so a newly linked Qt module can't be silently dropped the way Qt6Network
        # once was. The entries below are Qt's non-Qt third-party dependencies plus
        # the other libraries pdfe.exe links; windeployqt does NOT deploy these
        # under vcpkg, so they remain explicit.
        # Qt internal dependencies
        double-conversion pcre2-16 zstd libpng16 md4c
        # MuPDF dependencies (static lib, but these are its shared deps)
        openjp2 jpeg62
        # QPDF
        qpdf30
        # Font stack (FreeType + HarfBuzz)
        freetype harfbuzz bz2 brotlidec brotlicommon
        # OpenSSL (crypto for signatures, ssl for libcurl)
        libcrypto-3-x64 libssl-3-x64
        # SQLite (font analysis cache)
        sqlite3
        # Tesseract OCR chain
        tesseract55 leptonica-1.87.0 archive liblzma lz4 gif tiff
        # WebP (leptonica dependency)
        libwebp libwebpmux libsharpyuv
        # (ONNX Runtime deployed separately from external/)
        # Other
        libexpat pugixml
    )

    # Install each required DLL — debug from debug/bin, release from bin
    foreach(_dll ${_required_dlls})
        install(FILES "${_vcpkg_base}/debug/bin/${_dll}d.dll"
                DESTINATION bin
                CONFIGURATIONS Debug
                OPTIONAL)
        install(FILES "${_vcpkg_base}/debug/bin/${_dll}.dll"
                DESTINATION bin
                CONFIGURATIONS Debug
                OPTIONAL)
        install(FILES "${_vcpkg_base}/bin/${_dll}.dll"
                DESTINATION bin
                CONFIGURATIONS Release RelWithDebInfo MinSizeRel
                OPTIONAL)
    endforeach()

    # DLLs with non-standard debug naming (suffix embedded differently).
    # These can't use the ${name}d.dll / ${name}.dll pattern above.
    set(_special_release icudt78    icuin78    icuuc78    zlib1  libcurl)
    set(_special_debug   icudtd78   icuind78   icuucd78   zlibd1 libcurl-d)
    list(LENGTH _special_release _special_count)
    math(EXPR _special_last "${_special_count} - 1")
    foreach(_idx RANGE ${_special_last})
        list(GET _special_release ${_idx} _rel)
        list(GET _special_debug ${_idx} _dbg)
        install(FILES "${_vcpkg_base}/debug/bin/${_dbg}.dll"
                DESTINATION bin
                CONFIGURATIONS Debug
                OPTIONAL)
        install(FILES "${_vcpkg_base}/bin/${_rel}.dll"
                DESTINATION bin
                CONFIGURATIONS Release RelWithDebInfo MinSizeRel
                OPTIONAL)
    endforeach()

    # ONNX Runtime DLLs (pre-built, from external/)
    if(PDFE_HAS_ONNXRUNTIME)
        install(FILES "${ONNXRUNTIME_ROOT}/lib/onnxruntime.dll"
                DESTINATION bin OPTIONAL)
        install(FILES "${ONNXRUNTIME_ROOT}/lib/onnxruntime_providers_shared.dll"
                DESTINATION bin OPTIONAL)
    endif()

    # CNN font model. The font embedder loads <exe_dir>/models/font_embedder.onnx
    # at runtime (src/ui/MainWindow.cpp path1 — comment there says "post-build
    # deployed, OR installed"). src/app/CMakeLists.txt POST_BUILD stages it into
    # the BUILD tree for dev/CI runs, but the installer previously had NO rule for
    # it — an installed app therefore silently lost CNN font re-ranking (fell back
    # to the heuristic matcher). The model is gitignored and fetched at build time
    # (CI "Fetch CNN font_embedder.onnx model" step, ~44 MB, SHA256-verified), so
    # OPTIONAL mirrors the deploy's if(EXISTS) guard: bundled when present (CI
    # Release build), skipped cleanly on a local build that didn't fetch it. Must
    # land at bin/models (install exe is bin/pdfe.exe, so <exe_dir>/models).
    install(FILES "${CMAKE_SOURCE_DIR}/models/font_embedder.onnx"
            DESTINATION bin/models
            OPTIONAL)

    # Tessdata for OCR: eng (Latin), ara (Arabic), osd (script/orientation
    # detection for the per-call OCR language auto-detect, defect #3).
    install(FILES "${CMAKE_SOURCE_DIR}/tests/fixtures/tessdata/eng.traineddata"
                  "${CMAKE_SOURCE_DIR}/tests/fixtures/tessdata/ara.traineddata"
                  "${CMAKE_SOURCE_DIR}/tests/fixtures/tessdata/osd.traineddata"
            DESTINATION bin/tessdata)

    # --- Qt runtime: module DLLs + plugins via windeployqt (authoritative) ---
    # Replaces the previous hand-maintained Qt6*.dll list and per-plugin install
    # rules. windeployqt analyses the built pdfe.exe and deploys exactly the Qt
    # module DLLs and the plugins it needs (platforms, imageformats, iconengines,
    # styles, tls, generic, networkinformation, plus ICU / D3Dcompiler). This is
    # what prevents a recurrence of the "Qt6Network.dll was not found" class of
    # bug: a newly linked Qt module is picked up automatically, and a windeployqt
    # failure hard-fails the install (FATAL_ERROR) instead of silently shipping an
    # incomplete bundle. (Non-Qt third-party DLLs are still deployed explicitly
    # above -- windeployqt does not deploy them under vcpkg. A PATH-sanitized
    # launch smoke test -- scripts/verify-installer-deps.ps1 -- guards the whole
    # bundle in CI.)
    #
    # STATIC Qt EXCEPTION (meta-audit follow-up F39, backlog row D-6). A static
    # Qt triplet (x64-windows-static) links every Qt module INTO the exe, so
    # there are no Qt DLLs to deploy and vcpkg ships no windeployqt for it at
    # all — measured: build-fuzz-deps-static/x64-windows-static/tools/Qt6/bin
    # contains no windeployqt.exe, while the dynamic x64-windows tree does. The
    # libFuzzer lane is by construction a static-CRT / static-Qt configure
    # (windows-fuzz.yml, which the CMakeLists.txt:35 toolchain note forces), so
    # the unconditional FATAL_ERROR below made that configure IMPOSSIBLE — the
    # concrete reason the fuzz lane never produced a linked harness and D-6
    # stayed open. Detected from Qt6::Core's own target type rather than by
    # string-matching the triplet name, so a differently-named static triplet is
    # handled too.
    #
    # This is NOT a weakening of the guard: for a shared-Qt build — every build
    # that actually ships an installer — windeployqt remains REQUIRED and its
    # absence is still a hard configure failure, exactly as before.
    set(_qt_is_static FALSE)
    if(TARGET Qt6::Core)
        get_target_property(_qt_core_type Qt6::Core TYPE)
        if(_qt_core_type STREQUAL "STATIC_LIBRARY")
            set(_qt_is_static TRUE)
        endif()
    endif()

    if(_qt_is_static)
        message(STATUS
            "Qt6 is a STATIC build (Qt6::Core TYPE=STATIC_LIBRARY) -- skipping "
            "windeployqt: Qt is linked into the executable and vcpkg ships no "
            "windeployqt for a static triplet. Install-tree Qt deployment is a "
            "no-op for this configuration (F39 / D-6).")
    else()
    find_program(WINDEPLOYQT_EXECUTABLE windeployqt
        HINTS "${_vcpkg_base}/tools/Qt6/bin" "${_vcpkg_base}/bin")
    if(NOT WINDEPLOYQT_EXECUTABLE)
        message(FATAL_ERROR
            "windeployqt not found under ${_vcpkg_base}; cannot deploy the Qt runtime.")
    endif()
    # GitHub #157: a Debug package must use vcpkg's DEBUG windeployqt wrapper.
    #
    # vcpkg splits the Qt runtime -- release DLLs in <base>/bin, the
    # `d`-suffixed debug DLLs in <base>/debug/bin -- and windeployqt resolves
    # the modules it deploys from the Qt installation implied by the `qtpaths`
    # it uses, NOT from PATH. Invoking the release `windeployqt.exe` with
    # `--debug` therefore asked for the right FILENAMES in the wrong DIRECTORY:
    #
    #   Unable to find dependent libraries of .../x64-windows/bin/Qt6Networkd.dll
    #   windeployqt failed (exit 1) -- Qt runtime not deployed
    #
    # vcpkg ships the per-config entry points for exactly this:
    # `windeployqt.debug.bat` (and `qtpaths.debug.bat`) alongside the release
    # executables. Selecting the right one is the supported fix.
    #
    # Consequence of the old behaviour, and why it went unnoticed: packaging
    # only ever worked in Release, where the manual signed-release script runs
    # it. `cpack -C Debug` -- the cheap local check a developer would use after
    # touching install() rules -- always failed, so nobody used it, and a change
    # that broke packaging would stay invisible until release day.
    set(_wdq_debug_wrapper "${_vcpkg_base}/tools/Qt6/bin/windeployqt.debug.bat")
    if(EXISTS "${_wdq_debug_wrapper}")
        set(_WDQ_FOR_DEBUG "${_wdq_debug_wrapper}")
    else()
        # Older/other layouts ship no wrapper; fall back to the release binary
        # with --debug, which is the previous behaviour rather than a new risk.
        set(_WDQ_FOR_DEBUG "${WINDEPLOYQT_EXECUTABLE}")
        message(STATUS
            "windeployqt.debug.bat not present; Debug packaging will use the "
            "release windeployqt with --debug (see GitHub #157)")
    endif()
    install(CODE "
        set(_wdq_exe  \"$<IF:$<CONFIG:Debug>,${_WDQ_FOR_DEBUG},${WINDEPLOYQT_EXECUTABLE}>\")
        set(_wdq_flag \"$<IF:$<CONFIG:Debug>,--debug,--release>\")
        message(STATUS \"Deploying Qt runtime via \${_wdq_exe} \${_wdq_flag}\")
        execute_process(
            COMMAND \"\${_wdq_exe}\" \${_wdq_flag}
                    --no-translations --no-compiler-runtime --no-opengl-sw
                    --dir \"\${CMAKE_INSTALL_PREFIX}/bin\"
                    \"\${CMAKE_INSTALL_PREFIX}/bin/pdfe.exe\"
            RESULT_VARIABLE _wdq_rc)
        if(NOT _wdq_rc EQUAL 0)
            message(FATAL_ERROR \"windeployqt failed (exit \${_wdq_rc}) -- Qt runtime not deployed\")
        endif()
    ")
    endif()  # _qt_is_static
endif()

# License file
install(FILES "${CMAKE_SOURCE_DIR}/LICENSE" DESTINATION .)

# AGPL corresponding-source offer (GitHub #200 item 5). Shipping the source
# URL INSIDE the install tree means a recipient of the binary alone -- copied
# install dir, no release page, no installer -- is still not left to guess
# where the source is. The release-notes half of the same obligation is
# enforced per-release by tools/gate/check_release_asset_hygiene.py, which
# fails a release whose body does not name the corresponding source.
install(FILES "${CMAKE_SOURCE_DIR}/CORRESPONDING-SOURCE.txt" DESTINATION .)

# CPack configuration -- output to a consistent directory under the build tree
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}/installer")
set(CPACK_PACKAGE_NAME "PDFE")
set(CPACK_PACKAGE_VENDOR "Itwerx Corp")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "PDFE - Professional PDF Editor")
set(CPACK_PACKAGE_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(CPACK_PACKAGE_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(CPACK_PACKAGE_VERSION_PATCH ${PROJECT_VERSION_PATCH})
set(CPACK_PACKAGE_INSTALL_DIRECTORY "PDFE")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")

if(WIN32)
    set(CPACK_GENERATOR "NSIS")
    set(CPACK_NSIS_DISPLAY_NAME "PDFE - Professional PDF Editor")
    set(CPACK_NSIS_PACKAGE_NAME "PDFE")
    set(CPACK_NSIS_MUI_ICON "${CMAKE_SOURCE_DIR}/pdfe-red.ico")
    set(CPACK_NSIS_MUI_UNIICON "${CMAKE_SOURCE_DIR}/pdfe-red.ico")
    set(CPACK_NSIS_INSTALLED_ICON_NAME "bin\\\\pdfe.exe")
    set(CPACK_NSIS_HELP_LINK "https://github.com/ipswitch8/PDFE")
    set(CPACK_NSIS_URL_INFO_ABOUT "https://github.com/ipswitch8/PDFE")
    set(CPACK_NSIS_MODIFY_PATH OFF)
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)

    # ------------------------------------------------------------------
    # GitHub #74 (WIN-06) -- elevation is ALREADY REQUESTED. Measured, not
    # assumed, and deliberately NOT re-added here.
    #
    # The issue proposes adding `RequestExecutionLevel admin`. Running
    # `cpack -C Debug -G NSIS` and reading the generated script shows CPack's
    # own NSIS template already emits it, at top level:
    #
    #     project.nsi:39   ;Require administrator access
    #     project.nsi:41   RequestExecutionLevel admin
    #
    # with no Section or Function above it. So the installer already prompts a
    # standard user and aborts cleanly if they decline -- the behaviour #74
    # asks for -- and it does so as a property of the script rather than of
    # Windows' filename-based installer-detection heuristic.
    #
    # ADDING IT AGAIN ACTIVELY BREAKS THE BUILD, which is how this was found.
    # The obvious hook, CPACK_NSIS_EXTRA_PREINSTALL_COMMANDS, emits INSIDE the
    # core install Section, and the directive is only valid at top level:
    #
    #     Error: command RequestExecutionLevel not valid in Section
    #     Error in script "...project.nsi" on line 661 -- aborting
    #
    # A text-oracle test asserting our CMakeLists contained the string would
    # have passed while the installer no longer built at all. Only running
    # cpack caught it -- which is precisely why the acceptance criterion asks
    # for a real invocation rather than a source assertion.
    #
    # `InstallerScopeDocs.InstallerRequestsElevationExplicitly` therefore
    # asserts the PROPERTY (the packaging configuration yields an installer
    # that requests admin) rather than asserting that this file adds a line.

    # Start menu shortcut
    set(CPACK_NSIS_CREATE_ICONS_EXTRA
        "CreateShortCut '$SMPROGRAMS\\\\$STARTMENU_FOLDER\\\\PDFE.lnk' '$INSTDIR\\\\bin\\\\pdfe.exe' '' '$INSTDIR\\\\bin\\\\pdfe.ico'"
    )
    set(CPACK_NSIS_DELETE_ICONS_EXTRA
        "Delete '$SMPROGRAMS\\\\$STARTMENU_FOLDER\\\\PDFE.lnk'"
    )

    # Desktop shortcut + file-association registry entries.
    #
    # NSIS string-quoting note: WriteRegStr arguments are wrapped in
    # single quotes here, so embedded double quotes are LITERAL and
    # need NO ``$\"`` escape. (The previous ``'$\"...$\"'`` form
    # produced a literal ``$"..."$``" in the registry, breaking the
    # shell open command and giving Windows nothing it could resolve
    # to a real executable — that's why "Open with" rendered the
    # raw command string with no icon, and clicking Open failed
    # with "Windows cannot access the specified device, path, or
    # file". Inside CMake's set() the literal ``"`` is escaped as
    # ``\"``; the .nsi sees a plain ``"``.)
    #
    # Modern Windows (10/11) will not let any installer FORCE itself
    # to be the default PDF handler — that's gated by a hash-signed
    # UserChoice key. What we can do, and what these entries
    # accomplish, is:
    #   1. Make PDFE appear in the "Open with" picker with the right
    #      friendly name and icon (HKCR\Applications\pdfe.exe).
    #   2. Make PDFE selectable in Windows Settings -> Apps ->
    #      Default apps via the RegisteredApplications +
    #      Capabilities pattern (HKLM\Software\Clients\... is the
    #      old protocol; modern Windows uses
    #      HKLM\Software\RegisteredApplications +
    #      HKLM\Software\PDFE\Capabilities).
    #   3. Wire the ProgID (PDFE.Document) so when the user does
    #      pick PDFE, the correct command runs.
    # File-association registry block lives in a separate
    # source-controlled .nsh file (packaging/file_assoc*.nsh)
    # and is brought in via NSIS !include. CMake/CPack only sees
    # the include directive — no escape-mangling of the WriteRegStr
    # arguments. Direct inline in CPACK_NSIS_EXTRA_INSTALL_COMMANDS
    # is unreliable: CMake serializes the variable into
    # CPackConfig.cmake with literal ``"`` characters, and on
    # round-trip CPack's NSIS generator truncates / list-splits at
    # those quotes, mangling the registry writes (the resulting
    # ``$"path$" $"%1$"`` is what produced the user-visible
    # "Windows cannot access" error and the raw-path "Open with"
    # entry).
    # Copy the .nsh helpers into the build dir so the !include path
    # is space-free regardless of whether the source dir contains
    # spaces. We must use UNQUOTED !include directives because CMake
    # serializes the EXTRA_*_COMMANDS variables into CPackConfig.cmake
    # without escaping internal ``"`` characters; those chars then
    # terminate the string on round-trip and CPack list-splits the
    # remainder, mangling the .nsi.
    configure_file(
        "${CMAKE_SOURCE_DIR}/packaging/file_assoc.nsh"
        "${CMAKE_BINARY_DIR}/installer/file_assoc.nsh"
        COPYONLY)
    configure_file(
        "${CMAKE_SOURCE_DIR}/packaging/file_assoc_uninstall.nsh"
        "${CMAKE_BINARY_DIR}/installer/file_assoc_uninstall.nsh"
        COPYONLY)
    file(TO_NATIVE_PATH
        "${CMAKE_BINARY_DIR}/installer/file_assoc.nsh" _PDFE_NSH_INSTALL)
    file(TO_NATIVE_PATH
        "${CMAKE_BINARY_DIR}/installer/file_assoc_uninstall.nsh" _PDFE_NSH_UNINSTALL)
    set(CPACK_NSIS_EXTRA_INSTALL_COMMANDS "!include ${_PDFE_NSH_INSTALL}")
    set(CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS "!include ${_PDFE_NSH_UNINSTALL}")
endif()

include(CPack)
