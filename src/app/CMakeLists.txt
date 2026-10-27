# App utility library (for testability)
add_library(pdfe_app STATIC
    AppSettings.cpp
    LogHandler.cpp
    LoggingPreferences.cpp
    CrashHandler.cpp
    ProcessMitigations.cpp
    # GitHub #213 / WIN-71 — partial suspend/resume handling. The decision
    # logic is headless so GoogleTest can drive it without a real suspend; see
    # the scope statement at the top of PowerEvents.h.
    PowerEvents.cpp
    StartupTimer.cpp
    Translations.cpp
    SingleInstanceGuard.cpp
    WindowManager.cpp
    UpdateChecker.cpp
    VersionInfo.cpp
)

target_include_directories(pdfe_app PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(pdfe_app PUBLIC
    pdfe_core
    Qt6::Widgets
    Qt6::Network
)
# MiniDumpWriteDump (CrashHandler.cpp) lives in DbgHelp on Windows.
if(WIN32)
    target_link_libraries(pdfe_app PUBLIC dbghelp)
endif()

# Single source of truth for the app version (project(pdfe VERSION ...)),
# consumed by pdfe::app::VersionInfo and its update/version-check test.
target_compile_definitions(pdfe_app PUBLIC
    PDFE_VERSION_STRING="${PROJECT_VERSION}")

set_target_properties(pdfe_app PROPERTIES AUTOMOC ON)

# Main executable
# pdfe_icons.qrc embeds pdfe.ico as a Qt resource (:/icons/pdfe.ico)
# so QApplication::setWindowIcon() can load it at runtime — Qt 6 on
# Windows does NOT automatically pick up the application's Win32 .rc
# icon for the taskbar / window-frame icon. The .rc icon still drives
# the static Explorer file icon.
if(WIN32)
    add_executable(pdfe
        ../main.cpp
        pdfe.rc
        pdfe_icons.qrc
    )
else()
    add_executable(pdfe
        ../main.cpp
        pdfe_icons.qrc
    )
endif()

target_link_libraries(pdfe PRIVATE
    pdfe_app
    pdfe_ui
    pdfe_rendering
    pdfe_core
    Qt6::Widgets
    Qt6::Network
)

# Windows: set WIN32_EXECUTABLE to prevent console window
if(WIN32)
    set_target_properties(pdfe PROPERTIES WIN32_EXECUTABLE $<CONFIG:Release>)
endif()

# ---------------------------------------------------------------------------
# GitHub #298 -- make PDFE's OWN linked output deterministic, so it can be
# pinned the way the 7,364 vcpkg files already are.
#
# WHY THIS IS NEEDED AT ALL. tools/gate/vcpkg_cache_manifest.py pins the
# CONTENTS of every installed dependency file, which closes cache substitution
# for everything it covers -- except 22 static `.lib` archives, excluded for a
# sound reason (lib.exe has no /Brepro equivalent; COFF archive members carry
# their own timestamps). #298's point is that a static archive is a LINK-TIME
# INPUT to this executable, so for that subset the risk does not disappear, it
# TRANSFERS from a pinned file to an unpinned one -- and MuPDF, the largest
# static dependency here, is exactly that case.
#
# #298 calls the fix tractable because "PDFE's own linked output is already
# demonstrably reproducible on this triplet", citing 24/24 byte-identical
# artifacts. MEASURED 2026-10-14, that is not true of this executable: two links
# of IDENTICAL objects differed in
#
#     6,164 of 70,291,968 bytes, first at offset 288 (the PE header)
#
# The 24/24 result is about artifacts built by VCPKG under
# tools/vcpkg-triplets/x64-windows-repro.cmake, which sets VCPKG_LINKER_FLAGS
# for PORTS. It does not reach this target's link. So the flag is the first
# deliverable, not the manifest.
#
# THE RECIPE IS NOT INVENTED HERE -- it is the one that triplet already
# measured, applied to our own link:
#
#   /Brepro               content-derived COFF timestamp and CodeView GUID,
#                         replacing the wall-clock stamp and the per-link
#                         random GUID (measured there as 19 of 666,624 bytes).
#   /PDBALTPATH:<name>    record only the PDB BASENAME in the image, so the
#                         absolute build path -- which on a CI runner contains
#                         the account name -- is not embedded.
#
#                         GitHub #304: this used the MSBuild macro `%_PDB%`,
#                         which expanded but arrived in the image as
#                         `%%%pdfe.pdb%%%` -- MSBuild percent-escaping of the
#                         macro token not round-tripping through CMake's
#                         target_link_options. Measured by parsing the CodeView
#                         RSDS record of the built image directly. No absolute
#                         path and no account name either way, so it was
#                         cosmetic rather than a disclosure finding, but a
#                         debugger looking for `pdfe.pdb` beside the image may
#                         not resolve that name.
#
#                         `$<TARGET_PDB_FILE_NAME:pdfe>` is resolved by CMake,
#                         so no percent token reaches MSBuild and there is
#                         nothing left to escape. It also names the real PDB
#                         rather than assuming the basename.
#   /INCREMENTAL:NO       REQUIRED and easy to miss: an incrementally linked
#                         image cannot carry a reproducible layout, so it
#                         defeats /Brepro outright. That triplet records the
#                         same discovery -- setting the linker flags alone did
#                         NOT fix its two debug DLLs. MSVC defaults Debug to
#                         /INCREMENTAL, which is why this is config-specific.
#
# The first two are unconditional: they cost nothing and the PDB-path one is a
# disclosure fix in its own right. /INCREMENTAL:NO is gated, because it makes
# every incremental Debug relink a full one and that is a real cost to anyone
# iterating. Release is already /INCREMENTAL:NO by default, so the SHIPPED
# artifact is deterministic without the option; the option exists so the
# property can be MEASURED in Debug, which is what the repro test drives.
#
# NOT FIXED HERE, and stated rather than left to be discovered: __FILE__ still
# embeds the absolute source path, so two builds under DIFFERENT roots differ
# even with all of the above. That is cross-machine reproducibility, a strictly
# larger claim than "repeated builds here agree", and the triplet's
# /d1trimfile treatment is the known remedy if it is ever needed.
if(MSVC)
    target_link_options(pdfe PRIVATE
        /Brepro
        "/PDBALTPATH:$<TARGET_PDB_FILE_NAME:pdfe>")

    option(PDFE_REPRODUCIBLE_LINK
           "Disable incremental linking so a Debug link is byte-reproducible \
(GitHub #298). Costs full relinks; Release does not need it." OFF)
    if(PDFE_REPRODUCIBLE_LINK)
        target_link_options(pdfe PRIVATE $<$<CONFIG:Debug>:/INCREMENTAL:NO>)
    endif()
endif()

# ---------------------------------------------------------------------------
# GitHub #128 -- compile translations/*.ts into .qm and actually ship them.
#
# Before this, translations/app_en.ts and app_fr.ts existed, nothing compiled
# them, no .qm existed anywhere in the repo, and nothing was bundled. The
# runtime half was already written and waiting: pdfe::app::installTranslations
# (src/app/Translations.cpp) looks for app_<locale>.qm in, in order, the Qt
# resource path :/i18n, then <exedir>/translations, then <exedir>. It has been
# finding nothing.
#
# qt_add_lrelease is used rather than qt_add_translations because the latter
# also wires an lupdate target that rewrites the .ts sources; compiling is all
# that is wanted here, and a build step that can rewrite tracked source files
# is a surprise nobody asked for.
set(PDFE_TS_FILES
    ${CMAKE_SOURCE_DIR}/translations/app_en.ts
    ${CMAKE_SOURCE_DIR}/translations/app_fr.ts
)

qt_add_lrelease(pdfe
    TS_FILES ${PDFE_TS_FILES}
    QM_FILES_OUTPUT_VARIABLE PDFE_QM_FILES
)

# Dev / run-from-build-tree: put the .qm where installTranslations' SECOND
# lookup path expects them, so a developer running out of the build tree gets
# the same behaviour as an installed copy.
add_custom_command(TARGET pdfe POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:pdfe>/translations"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${PDFE_QM_FILES} "$<TARGET_FILE_DIR:pdfe>/translations"
    COMMENT "Staging compiled .qm catalogs next to pdfe.exe (GitHub #128)"
    VERBATIM
)

# Installed / packaged: CPack picks these up from the same relative location.
install(FILES ${PDFE_QM_FILES} DESTINATION translations)

# Out-of-process document pre-flight opener — crash isolation for hostile
# PDFs (see pdfe_preflight.cpp and src/core/PreflightOpen). Links only MuPDF
# (no Qt) so it spawns fast enough to gate every File>Open. Deployed/installed
# next to pdfe.exe; preflightDocumentSafe() locates it beside the running exe.
add_executable(pdfe_preflight pdfe_preflight.cpp)
target_link_libraries(pdfe_preflight PRIVATE unofficial::libmupdf::libmupdf)
set_target_properties(pdfe_preflight PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)
install(TARGETS pdfe_preflight RUNTIME DESTINATION bin)
# Under the fuzz build the whole tree is ASan/UBSan/coverage instrumented
# (see PDFE_ENABLE_FUZZING in the top-level CMakeLists). A plain exe then
# needs the static sanitizer + sancov runtimes (the fuzz harnesses pull
# these via tests/security/fuzz). fuzzer_no_main supplies the Sanitizer
# coverage symbols without a libFuzzer main(). Normal app builds skip this.
if(PDFE_ENABLE_FUZZING AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang"
   AND CMAKE_SYSTEM_NAME STREQUAL "Windows")
    target_link_options(pdfe_preflight PRIVATE
        "/defaultlib:clang_rt.asan-x86_64.lib"
        "/defaultlib:clang_rt.asan_cxx-x86_64.lib"
        "/defaultlib:clang_rt.ubsan_standalone-x86_64.lib"
        "/defaultlib:clang_rt.ubsan_standalone_cxx-x86_64.lib"
        "/defaultlib:clang_rt.fuzzer_no_main-x86_64.lib"
    )
endif()

# Deploy tessdata next to the executable for dev builds.
# eng = Latin recognition; ara = Arabic recognition; osd = script/orientation
# detection used by the per-call OCR language auto-detect (defect #3).
add_custom_command(TARGET pdfe POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:pdfe>/tessdata"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/tests/fixtures/tessdata/eng.traineddata"
        "${CMAKE_SOURCE_DIR}/tests/fixtures/tessdata/ara.traineddata"
        "${CMAKE_SOURCE_DIR}/tests/fixtures/tessdata/osd.traineddata"
        "$<TARGET_FILE_DIR:pdfe>/tessdata/"
    COMMENT "Deploying tessdata for OCR"
)

# Deploy Qt ICO image-format plugin for dev builds so QIcon can
# resolve :/icons/pdfe.ico at runtime. Without qico, QIcon would
# fall back to a null icon and the title-bar would show the
# generic Windows IDI_APPLICATION glyph. The release/install path
# already copies the entire imageformats/ directory (root
# CMakeLists.txt install rules). SVG plugin no longer required:
# the SVG theme-aware variants were retired in favor of the
# multi-resolution pdfe-red.ico (16/24/32/48/64/128/256 frames).
#
# Windows-only: Linux qt6-base-dev installs plugins to /usr/lib so
# QApplication finds them via standard plugin paths without staging.
if(WIN32)
    # Prefer VCPKG_INSTALLED_DIR when it is set.
    #
    # This is the SAME defect meta-audit F39 (backlog D-6) fixed in the root
    # CMakeLists.txt at the `_vcpkg_base` assignment -- surviving here, in a
    # second file, because that fix was applied to the one site it was found at.
    # Hardcoding "${CMAKE_BINARY_DIR}/vcpkg_installed/..." assumes the DEFAULT
    # manifest-mode install location, so any build configured with
    # VCPKG_MANIFEST_MODE=OFF + -DVCPKG_INSTALLED_DIR=<external root> looks for
    # Qt plugins in a directory that is never created.
    #
    # MEASURED, GitHub #272: a Release build against a neutral vcpkg root
    # compiled pdfe.exe successfully and then died in this POST_BUILD step --
    #   Error copying file (if different) from
    #   ".../build/neutral-pkg/vcpkg_installed/x64-windows-repro/Qt6/plugins/imageformats/qico.dll"
    # -- while the plugins were sitting in the external root all along. MSBuild
    # surfaces it as a wall of MSB3073, which names the cmake invocation rather
    # than the missing directory, so the cause is not obvious from the error.
    #
    # Behaviour-preserving for ordinary builds: in manifest mode the vcpkg
    # toolchain sets VCPKG_INSTALLED_DIR to exactly the hardcoded value.
    if(VCPKG_INSTALLED_DIR)
        set(_pdfe_qt_plugins_base "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
    else()
        set(_pdfe_qt_plugins_base "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}")
    endif()
    add_custom_command(TARGET pdfe POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:pdfe>/imageformats"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_pdfe_qt_plugins_base}/$<IF:$<CONFIG:Debug>,debug/,>Qt6/plugins/imageformats/$<IF:$<CONFIG:Debug>,qicod.dll,qico.dll>"
            "$<TARGET_FILE_DIR:pdfe>/imageformats/"
        COMMENT "Deploying Qt ICO image-format plugin"
    )

    # Deploy the Qt Windows PLATFORM plugin (qwindows.dll) next to pdfe.exe.
    # WITHOUT it the app aborts at startup with "no Qt platform plugin could be
    # initialized" (the modal dialog titled "pdfe"). This was previously only
    # staged by the pdfe_tests POST_BUILD, so a standalone
    # `cmake --build --target pdfe` (e.g. the CI Release build that the UI batch
    # + render suites launch) produced an UN-STARTABLE pdfe.exe — every UI test
    # then failed because the app never opened a main window. Also stage the JPEG
    # image-format plugin (canvas rendering of JPEG content) and the TLS backends
    # (HTTPS/TSA signing) so a standalone pdfe build is fully functional, mirroring
    # the pdfe_tests deploy (tests/CMakeLists.txt). copy_if_different + idempotent
    # make_directory keep this cheap on incremental builds.
    add_custom_command(TARGET pdfe POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:pdfe>/platforms"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_pdfe_qt_plugins_base}/$<IF:$<CONFIG:Debug>,debug/,>Qt6/plugins/platforms/$<IF:$<CONFIG:Debug>,qwindowsd.dll,qwindows.dll>"
            "$<TARGET_FILE_DIR:pdfe>/platforms/"
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:pdfe>/imageformats"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_pdfe_qt_plugins_base}/$<IF:$<CONFIG:Debug>,debug/,>Qt6/plugins/imageformats/$<IF:$<CONFIG:Debug>,qjpegd.dll,qjpeg.dll>"
            "$<TARGET_FILE_DIR:pdfe>/imageformats/"
        # SVG plugins: the main + annotation toolbars draw from
        # QIcon(":/icons/actions/*.svg") (action_icons.qrc, bugfix-3e91a7c4).
        # Without the qsvg imageformat + qsvgicon iconengine plugins those icons
        # resolve NULL and a fresh in-place pdfe.exe shows blank toolbars. The
        # installed package already ships all plugins via the top-level
        # install(DIRECTORY) rules; this stages them for run-from-build-tree.
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_pdfe_qt_plugins_base}/$<IF:$<CONFIG:Debug>,debug/,>Qt6/plugins/imageformats/$<IF:$<CONFIG:Debug>,qsvgd.dll,qsvg.dll>"
            "$<TARGET_FILE_DIR:pdfe>/imageformats/"
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:pdfe>/iconengines"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_pdfe_qt_plugins_base}/$<IF:$<CONFIG:Debug>,debug/,>Qt6/plugins/iconengines/$<IF:$<CONFIG:Debug>,qsvgicond.dll,qsvgicon.dll>"
            "$<TARGET_FILE_DIR:pdfe>/iconengines/"
        # GitHub #192 -- qsvgd.dll and qsvgicond.dll above both link directly
        # against the Qt6Svg MODULE dll (confirmed by `dumpbin /dependents`).
        # That import lives inside a plugin loaded at runtime via
        # QPluginLoader, not in pdfe.exe's own import table, so vcpkg's
        # applocal deployment (which walks only the target's direct imports)
        # never stages it, and nothing else here copied it either -- a
        # genuinely clean build tree shipped pdfe.exe without it. Stage it
        # explicitly, same pattern as the plugins that need it.
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_pdfe_qt_plugins_base}/$<IF:$<CONFIG:Debug>,debug/bin,bin>/$<IF:$<CONFIG:Debug>,Qt6Svgd.dll,Qt6Svg.dll>"
            "$<TARGET_FILE_DIR:pdfe>/"
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:pdfe>/tls"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_pdfe_qt_plugins_base}/$<IF:$<CONFIG:Debug>,debug/,>Qt6/plugins/tls/$<IF:$<CONFIG:Debug>,qopensslbackendd.dll,qopensslbackend.dll>"
            "$<TARGET_FILE_DIR:pdfe>/tls/"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_pdfe_qt_plugins_base}/$<IF:$<CONFIG:Debug>,debug/,>Qt6/plugins/tls/$<IF:$<CONFIG:Debug>,qschannelbackendd.dll,qschannelbackend.dll>"
            "$<TARGET_FILE_DIR:pdfe>/tls/"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_pdfe_qt_plugins_base}/$<IF:$<CONFIG:Debug>,debug/,>Qt6/plugins/tls/$<IF:$<CONFIG:Debug>,qcertonlybackendd.dll,qcertonlybackend.dll>"
            "$<TARGET_FILE_DIR:pdfe>/tls/"
        COMMENT "Deploying Qt platform + JPEG + TLS plugins for pdfe.exe"
    )

    # Deploy OpenSSL libssl for dev builds. libssl is dlopen'd by libcurl
    # at runtime for HTTPS Timestamp-Authority (TSA) requests, so it is NOT
    # in pdfe's import table — vcpkg's applocal deploy (which only copies
    # statically-linked DLLs) skips it, leaving HTTPS/TSA signing prone to a
    # runtime STATUS_DLL_NOT_FOUND (0xC0000135) and failing
    # DllDependencyTest.OpenSslTlsPresent. libcrypto IS linked, so applocal
    # already stages it; only libssl needs this explicit copy.
    add_custom_command(TARGET pdfe POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_pdfe_qt_plugins_base}/$<IF:$<CONFIG:Debug>,debug/bin,bin>/libssl-3-x64.dll"
            "$<TARGET_FILE_DIR:pdfe>/"
        COMMENT "Deploying OpenSSL libssl (dlopen'd by libcurl)"
    )
endif()

# Deploy ONNX Runtime shared lib for dev builds (Windows DLL only;
# on Linux ONNX Runtime is found via system loader / rpath when
# installed via apt/package manager).
if(PDFE_HAS_ONNXRUNTIME AND WIN32)
    add_custom_command(TARGET pdfe POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${ONNXRUNTIME_ROOT}/lib/onnxruntime.dll"
            "$<TARGET_FILE_DIR:pdfe>/"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${ONNXRUNTIME_ROOT}/lib/onnxruntime_providers_shared.dll"
            "$<TARGET_FILE_DIR:pdfe>/"
        COMMENT "Deploying ONNX Runtime DLLs"
    )
endif()

# Deploy CNN font model for dev builds (optional)
if(EXISTS "${CMAKE_SOURCE_DIR}/models/font_embedder.onnx")
    add_custom_command(TARGET pdfe POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:pdfe>/models"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/models/font_embedder.onnx"
            "$<TARGET_FILE_DIR:pdfe>/models/"
        COMMENT "Deploying CNN font model"
    )
endif()

# Install directives
install(TARGETS pdfe RUNTIME DESTINATION bin)
install(FILES "${CMAKE_SOURCE_DIR}/pdfe-red.ico" DESTINATION bin RENAME pdfe.ico)
