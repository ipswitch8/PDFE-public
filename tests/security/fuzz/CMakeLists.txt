# Phase-4 fuzz harnesses for the PDFE -> MuPDF integration code.
#
# Reached only when PDFE_ENABLE_FUZZING=ON (which also forces
# PDFE_ENABLE_SECURITY_TESTS=ON in the top-level CMakeLists.txt).
#
# Six harness executables, each with a single LLVMFuzzerTestOneInput
# entry point (libFuzzer) or AFL++ persistent-mode main(). Backend
# is selected at configure time via PDFE_FUZZ_BACKEND (libfuzzer |
# aflpp). The default is libfuzzer because clang-cl on Windows is
# the realistic toolchain target for this project; aflpp is the CI
# alternative for Linux runners.
#
# Build is *not* registered as a CTest entry. Fuzz runs are time-
# bounded campaigns, not boolean tests; scripts/security/run_fuzz.ps1
# drives them and emits the report. Phase 9 wires that into the
# nightly job.

set(_fuzz_dir "${CMAKE_CURRENT_SOURCE_DIR}")

# ---------------------------------------------------------------
# Backend selection + toolchain probe
# ---------------------------------------------------------------
set(PDFE_FUZZ_BACKEND "libfuzzer" CACHE STRING
    "Fuzz backend: libfuzzer (clang-cl) or aflpp (AFL++ persistent)")
set_property(CACHE PDFE_FUZZ_BACKEND PROPERTY STRINGS libfuzzer aflpp)

if(NOT PDFE_FUZZ_BACKEND STREQUAL "libfuzzer"
   AND NOT PDFE_FUZZ_BACKEND STREQUAL "aflpp")
    message(FATAL_ERROR
        "PDFE_FUZZ_BACKEND must be 'libfuzzer' or 'aflpp', got: ${PDFE_FUZZ_BACKEND}")
endif()

if(PDFE_FUZZ_BACKEND STREQUAL "libfuzzer")
    # libFuzzer needs clang. On Windows that's clang-cl; on Linux
    # it's clang. MSVC cl.exe lacks -fsanitize=fuzzer entirely.
    if(NOT (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR
            CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang"))
        # SEC-15 / GitHub #46 -- FATAL_ERROR, not WARNING + return().
        #
        # This directory is reached ONLY when PDFE_ENABLE_FUZZING=ON, which
        # defaults to OFF. Arriving here therefore means someone explicitly asked
        # for fuzz harnesses and is about to get a configure that succeeds while
        # building none of them. The `return()` made that silent: the build is
        # green, the harness binaries simply do not exist, and the first sign is
        # a campaign step failing to find an .exe -- or worse, an operator
        # concluding the harnesses "passed".
        #
        # Refuse instead. An ordinary MSVC build never reaches this line.
        message(FATAL_ERROR
            "PDFE_FUZZ_BACKEND=libfuzzer requires clang/clang-cl, but \
CMAKE_CXX_COMPILER_ID=${CMAKE_CXX_COMPILER_ID}. Refusing to configure a build \
that would silently produce NO fuzz harnesses while reporting success -- \
PDFE_ENABLE_FUZZING was explicitly requested (it defaults to OFF).
    Reconfigure with `-DCMAKE_CXX_COMPILER=clang-cl`, or drop \
`-DPDFE_ENABLE_FUZZING=ON`.")
    endif()
endif()

# ---------------------------------------------------------------
# Common compile/link flags for fuzz binaries
# ---------------------------------------------------------------
# libFuzzer:  -fsanitize=fuzzer,address,undefined  (clang only)
# aflpp:      relies on the AFL++ wrapper compiler (afl-clang-fast++)
#             being CMAKE_CXX_COMPILER; we add no extra flags here.

set(_fuzz_compile_options)
set(_fuzz_link_options)
set(_fuzz_backend_define)

if(PDFE_FUZZ_BACKEND STREQUAL "libfuzzer")
    list(APPEND _fuzz_compile_options
        -fsanitize=fuzzer,address,undefined
        -fno-omit-frame-pointer
        -O1
        -g
    )
    list(APPEND _fuzz_link_options
        -fsanitize=fuzzer,address,undefined
    )
    # CMake's vs_link_exe wrapper invokes lld-link directly without the
    # clang-cl driver, so -fsanitize=fuzzer doesn't pull in the runtime
    # libs by itself. Force them via /defaultlib so lld-link finds them
    # via LIB env (which scripts/security/run_*.ps1 + the configure-
    # time wrapper set to .../clang/<ver>/lib/windows/).
    #
    # STATIC-CRT (/MT) runtime set: clang_rt.fuzzer-x86_64.lib is built
    # MT_StaticRelease, so the whole fuzz build is /MT (release static
    # CRT) — see the windows-fuzz workflow (RelWithDebInfo +
    # CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded + x64-windows-static
    # deps). That means the *static* ASan runtime (clang_rt.asan +
    # clang_rt.asan_cxx), NOT the asan_dynamic pair — the dynamic ASan
    # import libs carry the MD_DynamicRelease CRT marker and would
    # re-introduce the /failifmismatch the static-CRT switch exists to
    # eliminate.
    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang"
       AND CMAKE_SYSTEM_NAME STREQUAL "Windows")
        list(APPEND _fuzz_link_options
            "/defaultlib:clang_rt.fuzzer-x86_64.lib"
            "/defaultlib:clang_rt.asan-x86_64.lib"
            "/defaultlib:clang_rt.asan_cxx-x86_64.lib"
            "/defaultlib:clang_rt.ubsan_standalone-x86_64.lib"
            "/defaultlib:clang_rt.ubsan_standalone_cxx-x86_64.lib"
        )
    endif()
    set(_fuzz_backend_define PDFE_FUZZ_BACKEND_LIBFUZZER)
else()  # aflpp
    set(_fuzz_backend_define PDFE_FUZZ_BACKEND_AFLPP)
endif()

# ---------------------------------------------------------------
# Helper: declare one fuzz binary
# ---------------------------------------------------------------
function(pdfe_add_fuzz_target target source)
    set(_libs ${ARGN})  # extra pdfe_* libs beyond pdfe_core
    add_executable(${target} ${source})
    target_include_directories(${target} PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${_fuzz_dir}
    )
    target_compile_definitions(${target} PRIVATE
        ${_fuzz_backend_define}
    )
    if(_fuzz_compile_options)
        target_compile_options(${target} PRIVATE ${_fuzz_compile_options})
    endif()
    if(_fuzz_link_options)
        target_link_options(${target} PRIVATE ${_fuzz_link_options})
    endif()
    target_link_libraries(${target} PRIVATE
        pdfe_core
        ${_libs}
    )
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/fuzz"
    )
endfunction()

# ---------------------------------------------------------------
# Six fuzz binaries
# ---------------------------------------------------------------
pdfe_add_fuzz_target(fuzz_document_open
    "${_fuzz_dir}/fuzz_document_open.cpp")

pdfe_add_fuzz_target(fuzz_page_render
    "${_fuzz_dir}/fuzz_page_render.cpp"
    pdfe_rendering)

pdfe_add_fuzz_target(fuzz_text_extract
    "${_fuzz_dir}/fuzz_text_extract.cpp")

pdfe_add_fuzz_target(fuzz_annotation_walk
    "${_fuzz_dir}/fuzz_annotation_walk.cpp"
    pdfe_annotations)

pdfe_add_fuzz_target(fuzz_form_parse
    "${_fuzz_dir}/fuzz_form_parse.cpp"
    pdfe_forms
    pdfe_annotations)

pdfe_add_fuzz_target(fuzz_embedded_files
    "${_fuzz_dir}/fuzz_embedded_files.cpp"
    pdfe_attachments)

# Meta-audit F4 (SEC-06): image-codec decode surface — JPX / DCT / Flate
# image XObjects synthesised around the fuzz input, driven through
# Document::open + Page::render. This is the surface that produced the
# project's only RCE-class defect (#50 / DCA-0011, OpenJPEG heap overflow),
# which had been found by a hand-built fixture because no codec fuzz target
# existed. The harness enables the #50 JPX decode gate itself (and aborts if
# it cannot) — see the file header.
pdfe_add_fuzz_target(fuzz_image_decode
    "${_fuzz_dir}/fuzz_image_decode.cpp"
    pdfe_rendering)

# Meta-audit F13 (SEC-09 / SEC-54 / SEC-58): font-PROGRAM parse surface —
# attacker-controlled sfnt / CFF bytes reaching FreeType via
# EmbeddedFontAnalyzer (FT_New_Face and FT_New_Memory_Face,
# src/fonts/EmbeddedFontAnalyzer.cpp:278). DISTINCT from tests/test_font_fuzz.cpp,
# which is a GoogleTest over font DICTIONARY bytes and never puts fuzz input
# inside a font program. The harness self-checks that both of its arms reach a
# real FreeType parse using a known-good base-14 program, and aborts if not —
# same "read the gate back" discipline as fuzz_image_decode.
pdfe_add_fuzz_target(fuzz_font_program
    "${_fuzz_dir}/fuzz_font_program.cpp"
    pdfe_fonts)

# ---------------------------------------------------------------
# Phase-8 (R2-15/16) harnesses. The four write/mutation harnesses plus the
# fixed encryption harness all #include fuzz_targets.h, whose per-input bodies
# are inline functions. A given harness ODR-uses only its own core, but every
# core-bearing target links the full union of mutation libs so the build is
# robust even if the toolchain emits an unused inline core's references.
# ---------------------------------------------------------------
set(_fuzz_mutation_libs
    pdfe_textedit
    pdfe_fonts
    pdfe_forms
    pdfe_redaction
    pdfe_encryption)

pdfe_add_fuzz_target(fuzz_encryption_dict
    "${_fuzz_dir}/fuzz_encryption_dict.cpp"
    ${_fuzz_mutation_libs})

pdfe_add_fuzz_target(fuzz_content_stream_edit
    "${_fuzz_dir}/fuzz_content_stream_edit.cpp"
    ${_fuzz_mutation_libs})

pdfe_add_fuzz_target(fuzz_save_roundtrip
    "${_fuzz_dir}/fuzz_save_roundtrip.cpp"
    ${_fuzz_mutation_libs})

pdfe_add_fuzz_target(fuzz_mujs_exec
    "${_fuzz_dir}/fuzz_mujs_exec.cpp"
    ${_fuzz_mutation_libs})

pdfe_add_fuzz_target(fuzz_redaction_apply
    "${_fuzz_dir}/fuzz_redaction_apply.cpp"
    ${_fuzz_mutation_libs})

# ---------------------------------------------------------------
# Phase-73 (pipeline phase-20) — THE SIGNATURE-VALIDATION INPUT CLASS.
#
# Every harness above takes PDF bytes. A grep across this directory for
# ocsp|crl|pkcs7|signature returned ZERO before this phase, so the entire
# signature-validation surface — DER from an OCSP responder, from a CRL
# distribution point, or out of a /DSS stream — had no coverage-guided target.
# No amount of PDF mutation reaches it: those bytes are never parsed as ASN.1.
#
# These three link pdfe_signatures, which nothing here had ever done, so they
# also carry the static-CRT risk the link probe below was written to retire.
# ---------------------------------------------------------------
pdfe_add_fuzz_target(fuzz_ocsp_response
    "${_fuzz_dir}/fuzz_ocsp_response.cpp"
    pdfe_signatures)

pdfe_add_fuzz_target(fuzz_crl_parse
    "${_fuzz_dir}/fuzz_crl_parse.cpp"
    pdfe_signatures)

# PROMOTION, not a new harness: tests/test_pkcs7_fuzz.cpp has carried an
# LLVMFuzzerTestOneInput since pipeline e4c71f3a phase-2 that was compiled into
# the gtest binary and linked as a fuzzer by nothing. See DCA 0037. The body is
# shared with that gtest via pkcs7_fuzz_core.h so the campaign and the
# seed-replay oracle cannot drift.
pdfe_add_fuzz_target(fuzz_pkcs7_verify
    "${_fuzz_dir}/fuzz_pkcs7_verify.cpp"
    pdfe_signatures)

# RFC 3161 timestamp tokens. The reverification named TsaClient's libcurl
# and WinHttp paths as remote-bytes-in-scope; parseTimeStampResp and httpPost
# are both PRIVATE, so this drives the public verifyTimestampToken, which
# consumes the same attacker-controlled DER once the transport has handed it
# over. The HTTP layer itself is NOT covered from here -- see the phase-73
# disposition, where that is a named follow-up rather than an implied win.
pdfe_add_fuzz_target(fuzz_tsa_response
    "${_fuzz_dir}/fuzz_tsa_response.cpp"
    pdfe_signatures)

# ---------------------------------------------------------------
# STATIC-CRT LINK PROBE — criterion 1, kept as a standing check.
#
# Before the three targets above, nothing had ever linked pdfe_signatures under
# this configuration: x64-windows-static with CMAKE_MSVC_RUNTIME_LIBRARY=
# MultiThreaded (/MT), while the ordinary build is /MD dynamic. The criterion
# required demonstrating that OpenSSL / WinHTTP / Qt6::Network actually resolve
# there BEFORE any harness was written, because the alternative is writing three
# harnesses and discovering at the end that they cannot be linked at all.
#
# KEPT rather than deleted once it had served: it is the minimal reproduction of
# that question (one call, no corpus, no oracle), so a future toolchain or vcpkg
# bump that breaks the static-CRT link fails HERE, on a target with nothing else
# in it, instead of inside a harness where the failure reads as a harness bug.
# ---------------------------------------------------------------
# DECLARED LIKE repro_stack BELOW, *NOT* via pdfe_add_fuzz_target, and the
# distinction is enforced rather than stylistic. FuzzHarnessCountDocs requires
# the set of pdfe_add_fuzz_target() names to equal run_fuzz.ps1's $AllHarnesses
# and windows-fuzz.yml's arrays exactly — the SEC-13 guard that caught
# fuzz_image_decode being built and campaigned but never dispatched. A probe is
# not a campaign target: it has no oracle and no corpus, and giving it 21 min of
# every nightly would be waste dressed as coverage. So it is built with the same
# flags and deliberately kept OUT of the harness set, exactly as repro_stack is.
if(PDFE_FUZZ_BACKEND STREQUAL "libfuzzer"
   AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang"
   AND CMAKE_SYSTEM_NAME STREQUAL "Windows")
    add_executable(fuzz_signatures_link_probe
        "${_fuzz_dir}/fuzz_signatures_link_probe.cpp")
    target_include_directories(fuzz_signatures_link_probe PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${_fuzz_dir})
    target_compile_definitions(fuzz_signatures_link_probe PRIVATE
        ${_fuzz_backend_define})
    target_compile_options(fuzz_signatures_link_probe PRIVATE
        ${_fuzz_compile_options})
    target_link_options(fuzz_signatures_link_probe PRIVATE
        ${_fuzz_link_options})
    target_link_libraries(fuzz_signatures_link_probe PRIVATE
        pdfe_core
        pdfe_signatures)
    set_target_properties(fuzz_signatures_link_probe PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/fuzz")
endif()


# ---------------------------------------------------------------
# Diagnostic repro exe (own main, opens on a small-stack thread). Not a
# fuzz target and not a CTest entry — used to pinpoint runaway-recursion
# overflows at shallow, ASan-walkable depth. Windows + clang-cl only.
# ---------------------------------------------------------------
if(PDFE_FUZZ_BACKEND STREQUAL "libfuzzer"
   AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang"
   AND CMAKE_SYSTEM_NAME STREQUAL "Windows")
    add_executable(repro_stack "${_fuzz_dir}/repro_stack.cpp")
    target_include_directories(repro_stack PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(repro_stack PRIVATE pdfe_core)
    target_link_options(repro_stack PRIVATE
        "/defaultlib:clang_rt.asan-x86_64.lib"
        "/defaultlib:clang_rt.asan_cxx-x86_64.lib"
        "/defaultlib:clang_rt.ubsan_standalone-x86_64.lib"
        "/defaultlib:clang_rt.ubsan_standalone_cxx-x86_64.lib"
    )
    set_target_properties(repro_stack PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/fuzz")
endif()

# ---------------------------------------------------------------
# Stage seeds + dictionary into the build dir so the run_fuzz.ps1
# wrapper can find them at a stable location regardless of source
# tree layout.
# ---------------------------------------------------------------
file(COPY "${_fuzz_dir}/pdf.dict"
     DESTINATION "${CMAKE_BINARY_DIR}/fuzz/")

# asn1.dict (phase-73) is the DER/ASN.1 counterpart. pdf.dict is not merely
# useless for the signature harnesses, it is harmful: splicing "endobj" and
# "%%EOF" into a length-prefixed binary encoding produces inputs d2i_* rejects
# at the first tag byte, so the mutator spends its budget on tokens that cannot
# occur in a valid input. The campaign steps pick per harness.
file(COPY "${_fuzz_dir}/asn1.dict"
     DESTINATION "${CMAKE_BINARY_DIR}/fuzz/")

# Each harness gets seeds matched to what it exercises. Seeds live
# in the source tree under tests/security/fuzz/seeds/<harness>/ and
# are mirrored into the build dir for use as the seed_corpus arg to
# libFuzzer / AFL++ at run time.
# *.der added in phase-73 for the signature harnesses. Without it the OCSP/CRL
# seeds are silently NOT staged — the campaign starts from an empty corpus and
# still reports healthy exec/s, which is the failure mode this whole phase
# exists to stop being invisible.
file(COPY "${_fuzz_dir}/seeds/"
     DESTINATION "${CMAKE_BINARY_DIR}/fuzz/seeds"
     FILES_MATCHING PATTERN "*.pdf" PATTERN "*.bin" PATTERN "*.der")

# fuzz_pkcs7_verify's seeds are the 5-file corpus that already exists at
# tests/corpus/pkcs7/ and is already driven by the test_pkcs7_fuzz.cpp
# seed-replay oracle. Staged from there rather than copied into seeds/ so there
# is exactly one copy of those bytes and the two consumers cannot diverge.
file(COPY "${CMAKE_SOURCE_DIR}/tests/corpus/pkcs7/"
     DESTINATION "${CMAKE_BINARY_DIR}/fuzz/seeds/fuzz_pkcs7_verify")
