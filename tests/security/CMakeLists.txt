# PDFE security harness CMake registration.
#
# Reached only when PDFE_ENABLE_SECURITY_TESTS=ON. Every test
# registered here MUST carry the LABELS property "security" so it is
# excluded from the default `ctest` invocation and only runs under
# `ctest -L security`.
#
# Phase 1 registers a single test (verify_optin) that proves the
# opt-in invariant holds. Later phases append their own scripts.

set(_security_dir "${CMAKE_CURRENT_SOURCE_DIR}")

# --- Opt-in verification (phase 1) -----------------------------------
# Runs verify_optin.ps1 against this very build. The script asserts:
#   1. `ctest -N` (no -L) lists ZERO security tests
#   2. `ctest -L security -N` lists the security suite
# This is a self-test of the opt-in posture.

find_program(PWSH_EXE NAMES pwsh powershell)
if(PWSH_EXE)
    add_test(
        NAME security_verify_optin
        COMMAND ${PWSH_EXE} -NoProfile -ExecutionPolicy Bypass -File
                "${_security_dir}/runner/verify_optin.ps1"
                -BuildDir "${CMAKE_BINARY_DIR}"
    )
    set_tests_properties(security_verify_optin PROPERTIES
        LABELS "security"
        TIMEOUT 60
    )
else()
    message(WARNING "PowerShell not found; security_verify_optin not registered")
endif()

# --- Phase 3 runner unit tests ---------------------------------------
# decision.py + listener.py have unit tests that don't need admin,
# don't need PDFE.exe, and don't touch the network beyond loopback.
# They carry the 'security' label so they only run under
# `ctest -L security`, matching the opt-in posture.

find_program(PYTHON_EXE NAMES python python3 py)
if(PYTHON_EXE)
    # WORKING_DIRECTORY is load-bearing for all three pytest entries below.
    #
    # pytest.ini sets `--basetemp=tests/_tmp` as a RELATIVE path, deliberately,
    # so the janitor in tests/conftest.py can sweep stale outputs (the default
    # %TEMP%/pytest-of-<user>/ keeps three sessions and fills the disk). A
    # relative basetemp resolves against the PROCESS working directory, and
    # ctest runs these from the build tree -- so it landed on
    # build/vs2022/tests/security/tests/_tmp, whose parent does not exist, and
    # the tests errored at setup with FileNotFoundError.
    #
    # Two things were broken by the same omission, which is why the fix is the
    # working directory rather than a mkdir: the tests could not run, AND the
    # janitor was sweeping a directory nothing wrote to, so the temp files it
    # exists to reclaim were accumulating elsewhere. Creating the missing
    # directory would have fixed only the first.
    #
    # The convention already exists in this file (see the entry below that sets
    # WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"); these three simply did not use it.
    add_test(
        NAME security_runner_decision_unit
        COMMAND ${PYTHON_EXE} -m pytest
                "${_security_dir}/runner/test_decision.py" -v
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )
    set_tests_properties(security_runner_decision_unit PROPERTIES
        LABELS "security"
        TIMEOUT 60
    )
    add_test(
        NAME security_runner_listener_unit
        COMMAND ${PYTHON_EXE} -m pytest
                "${_security_dir}/runner/test_listener.py" -v
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )
    set_tests_properties(security_runner_listener_unit PROPERTIES
        LABELS "security"
        TIMEOUT 120
    )
    add_test(
        NAME security_runner_firewall_log_unit
        COMMAND ${PYTHON_EXE} -m pytest
                "${_security_dir}/runner/test_firewall_log.py" -v
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )
    set_tests_properties(security_runner_firewall_log_unit PROPERTIES
        LABELS "security"
        TIMEOUT 60
    )
else()
    message(WARNING
        "Python not found; phase-3 runner unit tests not registered")
endif()

# --- Phase 4 fuzzing (gated separately) ------------------------------
# Six libFuzzer / AFL++ harnesses around PDFE's MuPDF integration
# wrappers. See tests/security/fuzz/README.md for toolchain
# prerequisites. The harness binaries are NOT registered as CTest
# entries -- fuzz campaigns are time-bounded, not boolean tests;
# scripts/security/run_fuzz.ps1 drives them and emits the report.
if(PDFE_ENABLE_FUZZING)
    if(EXISTS "${_security_dir}/fuzz/CMakeLists.txt")
        add_subdirectory(fuzz)
    else()
        message(STATUS "tests/security/fuzz/CMakeLists.txt not present yet (phase 4)")
    endif()
endif()

# --- Phase 9 regression check ----------------------------------------
# Compares fresh per-phase outputs to canonical baselines under
# tests/security/regression_baselines/. Phases with no fresh output
# (because the live run wasn't done since checkout) are reported as
# 'skipped' by the script rather than failing the gate, so the
# regression check is honest about what it could compare.

if(PWSH_EXE)
    add_test(
        NAME security_regression_check
        COMMAND ${PWSH_EXE} -NoProfile -ExecutionPolicy Bypass -File
                "${CMAKE_SOURCE_DIR}/scripts/security/check_regressions.ps1"
    )
    set_tests_properties(security_regression_check PROPERTIES
        LABELS "security"
        TIMEOUT 600
    )
endif()

# --- pdfe_security_nightly target ------------------------------------
# Full-intensity invocation of every per-phase dispatcher. Documented
# entry point for nightly CI / on-demand thorough runs. NOT a
# dependency of any default build target -- runs only when explicitly
# requested via `cmake --build . --target pdfe_security_nightly`.
if(PWSH_EXE)
    add_custom_target(pdfe_security_nightly
        COMMAND ${PWSH_EXE} -NoProfile -ExecutionPolicy Bypass -File
                "${CMAKE_SOURCE_DIR}/scripts/security/run_all.ps1" -Nightly
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Running pdfe_security_nightly (full SAST + deps + secrets + fuzz + sanitizers + corpus)"
        VERBATIM
    )
endif()
