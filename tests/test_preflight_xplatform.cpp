// -*- coding: utf-8 -*-
// Cross-platform correctness for the out-of-process open sandbox
// (pipeline 243a1203 Phase 8, findings R2-23 + R2-25).
//
// test_preflight_open.cpp proves the isolation BOUNDARY holds on the host.
// This file covers the PLATFORM-RESOLUTION contract that boundary depends on:
// isDocumentOpenSafe() can only sandbox an open if it can first LOCATE the
// pdfe_preflight helper next to the running executable. That location is
// resolved by executableDir(), which has a per-OS implementation:
//
//     Windows  -> GetModuleFileNameW
//     macOS    -> _NSGetExecutablePath + realpath   (added in R2-23)
//     Linux    -> /proc/self/exe
//
// Before R2-23 the macOS path did not exist: the non-Windows branch was
// hard-coded to /proc/self/exe, which does not exist on macOS, so
// executableDir() returned empty and isDocumentOpenSafe() FAILED OPEN — every
// hostile document was reported "safe" on macOS. Two guarantees are asserted
// here, both runnable on any host (they do not depend on the macOS-only code
// path actually executing):
//
//   1. On the host, executableDir() resolves to a non-empty directory, so the
//      sandbox is armed rather than silently disabled. (Windows here; the
//      Linux and macOS CI legs run this same assertion on their OS, which is
//      how the macOS _NSGetExecutablePath branch is exercised — see
//      .github/workflows/macos-validation.yml.)
//
//   2. When the executable location is UNRESOLVABLE, isDocumentOpenSafe()
//      fails CLOSED (reports unsafe) rather than fail-open. This is the
//      behavioural fix R2-23 delivers; the test drives the unresolvable state
//      through the detail::setExecutableDirForTesting seam so it is provable
//      on every OS, not only the one where resolution can naturally fail.
#include <gtest/gtest.h>
#include "core/PreflightOpen.h"

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

namespace {
std::filesystem::path fixtures() { return std::filesystem::path(TEST_FIXTURES_DIR); }

// RAII guard: restores normal OS-based resolution no matter how the test exits
// (assertion failure, exception, early return), so one test's override can
// never leak into another and mask a real regression.
struct ExecDirOverrideGuard {
    explicit ExecDirOverrideGuard(std::optional<std::filesystem::path> dir) {
        pdfe::core::detail::setExecutableDirForTesting(std::move(dir));
    }
    ~ExecDirOverrideGuard() {
        pdfe::core::detail::setExecutableDirForTesting(std::nullopt);
    }
    ExecDirOverrideGuard(const ExecDirOverrideGuard&) = delete;
    ExecDirOverrideGuard& operator=(const ExecDirOverrideGuard&) = delete;
};
} // namespace

// (1) The sandbox is ARMED on the host: a clean document is permitted, which
// can only happen if executableDir() resolved and the helper was found and
// run. If executableDir() returned empty (the pre-fix macOS state), the
// fail-closed branch would REFUSE this good document and this test would fail.
// Runs identically on Windows, Linux, and macOS CI — establishing that each
// platform's executableDir() branch produces a usable directory.
TEST(PreflightXPlatform, ExecutableLocationResolvesOnThisHost) {
    auto clean = fixtures() / "test_single.pdf";
    ASSERT_TRUE(std::filesystem::exists(clean)) << clean.string();
    EXPECT_TRUE(pdfe::core::isDocumentOpenSafe(clean))
        << "a known-good document was refused -- the sandbox could not locate/"
           "run the preflight helper on this host (executableDir() branch for "
           "this OS is broken, e.g. the pre-R2-23 macOS fail-open state)";
}

// (2) THE FAIL-CLOSED CONTRACT. With the executable location forced
// unresolvable, isDocumentOpenSafe() must report UNSAFE (false). Against the
// pre-fix code this returns true (fail-open) -> RED. This is the exact defect
// R2-23 fixes: on macOS the empty executableDir() silently disabled the
// sandbox and every document -- including hostile ones -- was treated as safe.
TEST(PreflightXPlatform, UnresolvableHelperLocationFailsClosed) {
    ExecDirOverrideGuard guard(std::filesystem::path{}); // engaged-but-empty

    auto clean = fixtures() / "test_single.pdf";
    ASSERT_TRUE(std::filesystem::exists(clean)) << clean.string();
    // Even a PERFECTLY GOOD document must be refused when we cannot locate the
    // helper: an unlocatable helper means no sandbox, and "no sandbox" must
    // never resolve to "safe".
    EXPECT_FALSE(pdfe::core::isDocumentOpenSafe(clean))
        << "isDocumentOpenSafe() FAILED OPEN when the executable location was "
           "unresolvable -- a hostile document would be treated as safe. This "
           "is the R2-23 macOS defect.";
}

// (2') Deployment-tolerance is intentionally PRESERVED: when the directory
// resolves but the helper binary is simply absent there (a packaging gap, not
// an unresolvable location), the app still fails OPEN so it can open files at
// all. This pins the boundary between the two failure modes so a future change
// cannot collapse them -- the fail-closed fix must not turn every missing-file
// deployment into an app that opens nothing.
TEST(PreflightXPlatform, ResolvedDirButMissingHelperFailsOpen) {
    // A real, EXISTING directory that certainly does not contain pdfe_preflight.
    auto emptyDir = std::filesystem::temp_directory_path() /
                    "pdfe_preflight_no_helper_here";
    std::error_code ec;
    std::filesystem::create_directories(emptyDir, ec);
    ExecDirOverrideGuard guard(emptyDir);

    auto clean = fixtures() / "test_single.pdf";
    ASSERT_TRUE(std::filesystem::exists(clean)) << clean.string();
    EXPECT_TRUE(pdfe::core::isDocumentOpenSafe(clean))
        << "a resolvable-but-helperless deployment should fail OPEN (open "
           "normally), not refuse every document";

    std::filesystem::remove(emptyDir, ec);
}

// (2'') PRESENT-BUT-UNLAUNCHABLE helper must fail CLOSED (Phase-3 A4 preflight
// hardening). This is the boundary opposite ResolvedDirButMissingHelperFailsOpen:
// the helper binary IS present next to the app, but it cannot be run (here: it is
// not a valid executable). The deployment INTENDED vetting yet it could not run,
// so the document must be REFUSED — treating it as safe would silently defeat the
// sandbox on the exact document it was supposed to vet.
//   * Windows exercises the hardened CreateProcessW-failure branch directly
//     (a non-PE file makes CreateProcessW fail) → return false.
//   * POSIX reaches the same contract via exec-failure: fork() succeeds, execl()
//     fails on the junk file, the child _exit(127)s, and exit-code != 0 is unsafe.
// Either way the observable contract "present-but-unrunnable helper => UNSAFE"
// holds. Mutation: reverting the launch-failed branch to `return true` re-reds
// this on Windows.
TEST(PreflightXPlatform, PresentButUnlaunchableHelperFailsClosed) {
    auto badHelperDir = std::filesystem::temp_directory_path() /
                        "pdfe_preflight_unlaunchable";
    std::error_code ec;
    std::filesystem::create_directories(badHelperDir, ec);
    ASSERT_FALSE(ec) << "could not create temp helper dir: " << ec.message();

#if defined(_WIN32)
    const std::filesystem::path bogus = badHelperDir / "pdfe_preflight.exe";
#else
    const std::filesystem::path bogus = badHelperDir / "pdfe_preflight";
#endif
    {
        std::ofstream f(bogus, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(f.is_open()) << "could not write bogus helper: " << bogus.string();
        f << "this is not a valid executable\n";  // non-PE / non-ELF junk
    }
    ASSERT_TRUE(std::filesystem::exists(bogus));

    ExecDirOverrideGuard guard(badHelperDir);

    auto clean = fixtures() / "test_single.pdf";
    ASSERT_TRUE(std::filesystem::exists(clean)) << clean.string();
    EXPECT_FALSE(pdfe::core::isDocumentOpenSafe(clean))
        << "a present-but-unlaunchable preflight helper must fail CLOSED "
           "(refuse), not silently pass the document it was meant to vet";

    std::filesystem::remove(bogus, ec);
    std::filesystem::remove(badHelperDir, ec);
}

// (2'') The override seam is clean: after it is disengaged, normal OS-based
// resolution is restored and the sandbox works again. Guards against a leaked
// override poisoning later tests in the same process.
TEST(PreflightXPlatform, OverrideIsRestoredAfterUse) {
    {
        ExecDirOverrideGuard guard(std::filesystem::path{});
        auto clean = fixtures() / "test_single.pdf";
        EXPECT_FALSE(pdfe::core::isDocumentOpenSafe(clean)); // forced closed
    }
    // Back to real resolution: the good document is permitted again.
    auto clean = fixtures() / "test_single.pdf";
    ASSERT_TRUE(std::filesystem::exists(clean));
    EXPECT_TRUE(pdfe::core::isDocumentOpenSafe(clean))
        << "normal executable resolution was not restored after the test seam "
           "was disengaged";
}
