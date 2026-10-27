// -*- coding: utf-8 -*-
// ===========================================================================
// BINARY STALENESS GUARD — operator directive, 2026-08-15.
//
// THE DEFECT THIS EXISTS FOR, and it was found by the operator rather than by
// any gate. Several suites launch the REAL application as a child process
// (ShellAndInstanceLaunchTest, DllDependencyTest, ProcessMitigations, the
// preflight cases), resolving pdfe.exe from the build output directory beside
// pdfe_tests.exe. Local runs were building only the `pdfe_tests` target, so
// pdfe.exe sat THREE DAYS STALE while every launch-based assertion reported
// green against code that predated the phase under test. That was true of every
// phase of the 9e8d5137 pipeline, not one of them.
//
// WHY NO EXISTING CONTROL CAUGHT IT. The test-runner gate has a freshness check,
// but it compares the RESULT XML against SOURCE mtimes. The stale artifact here
// is a SECOND BINARY that no acceptance criterion names, so the check was
// looking straight past it. CI was correct all along —
// windows-validation.yml builds @('pdfe_tests','pdfe') — which is the tell: the
// defect lived in the local procedure, where nothing was asserting.
//
// THE GENERAL RULE THE OPERATOR STATED, which is broader than the instance:
// when a test finds a flaw and the code is changed to fix it, every SUBSEQUENT
// test must execute the NEW binary, and every EARLIER test that the change
// could have affected is invalidated and must be re-run. A test process that
// cannot tell whether it is running current code cannot make either guarantee.
//
// WHAT THIS FILE ASSERTS: that the binaries under test are not older than the
// sources they are built from. It is deliberately a TEST rather than only a
// runner preflight, so it holds however the suite is invoked — through
// run_gtest.py, through a bare pdfe_tests.exe, or under a debugger.
//
// WHAT IT DELIBERATELY DOES NOT DO: it does not compare CONTENT, and it does not
// try to prove the binary was built FROM those sources. mtime ordering is a
// necessary condition, not a sufficient one; a stronger check would need build
// provenance the build system does not currently emit. Claiming more than
// ordering here would be the "confident statement about an unverified
// mechanism" this programme keeps recording.
// ===========================================================================

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace {

fs::path repoRoot() { return fs::path(PDFE_REPO_ROOT); }

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

fs::path runningExePath()
{
    wchar_t buf[MAX_PATH];
    const DWORD n = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return fs::path(std::wstring(buf, n));
}
#else
fs::path runningExePath() { return {}; }
#endif

struct Newest {
    fs::file_time_type when{};
    fs::path what;
    bool found = false;
};

// Newest mtime across a given set of source trees.
//
// SCOPE, chosen so a false red is not cheaper than a true one: only files that
// can actually change compiled behaviour. Editing a .md or a fixture must never
// red the suite — an over-broad guard that fires on documentation gets disabled,
// and a disabled guard protects nothing.
//
// EACH BINARY IS COMPARED AGAINST THE SOURCES *IT* IS BUILT FROM, which is not
// the same set for both — and getting this wrong is not hypothetical. The first
// version of this file swept `src` + `tests` + `include` for BOTH binaries, and
// promptly failed itself: adding this very file made `tests/` newer than
// `pdfe.exe`, which is not built from `tests/` at all and was in fact perfectly
// current. The guard caught its own over-breadth, which is the one bug an
// over-broad guard reliably finds.
//
//   pdfe_tests.exe  <- src + include + tests
//   pdfe.exe        <- src + include            (NOT tests)
Newest newestSourceIn(const std::vector<std::string>& dirs)
{
    static const char* kExts[] = {".cpp", ".cxx", ".cc", ".h", ".hpp", ".hxx", ".inl"};

    // SCRATCH IS NOT SOURCE, and this sweep used to walk into it.
    //
    // `tests/_tmp/` is pytest's tmp area for this repo, and it sits INSIDE one of
    // the directories swept here. The Python mirror of this check
    // (run_gtest.py::_newest_source) had the same defect and was fixed first;
    // this is the second instance of the same pattern, found because the full
    // suite was run afterwards rather than only the targeted tests. Fixing one
    // instance of a pattern is not fixing the pattern.
    //
    // Two distinct harms, and the second is worse:
    //   * a throwaway `new.cpp` written by tools/gate/test_rebuild_assert.py is
    //     counted as a compilable input, so a perfectly current binary is
    //     reported STALE;
    //   * walking that tree made this test THROW outright --
    //     "The specified module could not be found." -- so the guard did not
    //     merely misreport, it died. Measured: parking tests/_tmp made the test
    //     pass immediately and restoring it made it fail again.
    static const char* kScratch[] = {"_tmp", "__pycache__", ".pytest_cache"};
    auto isScratch = [&](const fs::path& p) {
        for (const auto& part : p) {
            const std::string s = part.string();
            for (const char* k : kScratch) {
                if (s == k) return true;
            }
        }
        return false;
    };

    Newest best;
    for (const auto& d : dirs) {
        const fs::path root = repoRoot() / d;
        std::error_code ec;
        if (!fs::exists(root, ec)) continue;
        for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end;
             it.increment(ec)) {
            // Do not descend into scratch at all: cheaper than filtering every
            // file under it, and it avoids whatever inside those trees makes the
            // iterator throw.
            std::error_code de;
            if (it->is_directory(de) && !de && isScratch(it->path())) {
                it.disable_recursion_pending();
                continue;
            }
            if (!it->is_regular_file(ec)) continue;
            if (isScratch(it->path())) continue;
            const std::string ext = it->path().extension().string();
            if (std::find_if(std::begin(kExts), std::end(kExts),
                             [&](const char* e) { return ext == e; }) == std::end(kExts)) {
                continue;
            }
            std::error_code te;
            const auto t = fs::last_write_time(it->path(), te);
            if (te) continue;
            if (!best.found || t > best.when) {
                best.when = t;
                best.what = it->path();
                best.found = true;
            }
        }
    }
    return best;
}

std::string describe(const fs::file_time_type& t)
{
    // Portable-enough rendering for a failure message: raw ticks are useless to
    // a reader, so express the DIFFERENCE at the call site instead. Here we only
    // need something stable to print.
    return std::to_string(t.time_since_epoch().count());
}

}  // namespace

// ---------------------------------------------------------------------------
// (1) The test binary itself must not predate the sources it is built from.
//
// If it does, every assertion in this process — including this one — is being
// made by code that is not the code in the working tree, and the whole run is
// unsound. This is the check that makes "re-run after a fix" enforceable rather
// than remembered.
// ---------------------------------------------------------------------------
TEST(BinaryStalenessGuard, TestBinaryIsNotOlderThanItsSources)
{
    const auto exe = runningExePath();
    ASSERT_FALSE(exe.empty()) << "could not resolve the running executable path";

    std::error_code ec;
    const auto exeTime = fs::last_write_time(exe, ec);
    ASSERT_FALSE(ec) << "could not stat the running executable: " << exe.string();

    const Newest src = newestSourceIn({"src", "include", "tests"});
    ASSERT_TRUE(src.found)
        << "no source files found under " << repoRoot().string()
        << " — the guard swept nothing, which would make it pass vacuously";

    EXPECT_GE(exeTime, src.when)
        << "STALE TEST BINARY. " << exe.filename().string() << " is OLDER than\n  "
        << src.what.string() << "\n"
        << "so this process is running code that predates the working tree, and "
           "every result in this run — including the ones that look green — is "
           "about a build that no longer exists.\n"
        << "Rebuild before running:\n"
        << "  cmake --build build/vs2022 --config Debug --target pdfe_tests pdfe "
           "--parallel 4 -- -nodeReuse:false\n"
        << "(exe=" << describe(exeTime) << " src=" << describe(src.when) << ")";
}

#if defined(_WIN32)
// ---------------------------------------------------------------------------
// (2) THE ONE THAT WAS ACTUALLY BROKEN: the APPLICATION binary the launch-based
// suites execute as a child process.
//
// pdfe.exe is a sibling of pdfe_tests.exe by construction. Building only the
// `pdfe_tests` target leaves it at whatever age it was — which is exactly the
// three-day-stale state the operator found, invisible to every gate because the
// freshness controls look at the result XML and the sources, never at this.
//
// Skipped (not failed) when pdfe.exe is absent: a tests-only configuration is a
// legitimate way to build, and failing it would punish a setup that never
// launches the app. Present-but-stale is the defect; absent is a choice.
// ---------------------------------------------------------------------------
TEST(BinaryStalenessGuard, AppBinaryLaunchedByTestsIsNotOlderThanItsSources)
{
    const auto exe = runningExePath();
    ASSERT_FALSE(exe.empty());

    const fs::path app = exe.parent_path() / "pdfe.exe";
    std::error_code ec;
    if (!fs::exists(app, ec)) {
        GTEST_SKIP() << "pdfe.exe is not present beside " << exe.filename().string()
                     << " — no launch-based test can run in this configuration, so "
                        "there is no stale binary to guard against";
    }

    const auto appTime = fs::last_write_time(app, ec);
    ASSERT_FALSE(ec) << "could not stat " << app.string();

    // src + include ONLY. pdfe.exe is not built from tests/, so a test-file
    // edit must not accuse it of being stale — see newestSourceIn().
    const Newest src = newestSourceIn({"src", "include"});
    ASSERT_TRUE(src.found);

    EXPECT_GE(appTime, src.when)
        << "STALE APPLICATION BINARY — the defect this guard was written for.\n"
        << app.string() << "\nis OLDER than\n  " << src.what.string() << "\n"
        << "Every suite that LAUNCHES the app (ShellAndInstanceLaunchTest, "
           "DllDependencyTest, ProcessMitigations, preflight) is therefore testing "
           "an application that predates the working tree, and will report green "
           "for code that is not there.\n"
        << "This is invisible to the test-runner gate's freshness check, which "
           "compares the result XML against SOURCE mtimes and never looks at this "
           "binary.\n"
        << "Build BOTH targets:\n"
        << "  cmake --build build/vs2022 --config Debug --target pdfe_tests pdfe "
           "--parallel 4 -- -nodeReuse:false\n"
        << "(app=" << describe(appTime) << " src=" << describe(src.when) << ")";
}
#endif  // _WIN32

// ---------------------------------------------------------------------------
// (3) NON-VACUITY. The two guards above pass trivially if the sweep finds no
// sources — a wrong PDFE_REPO_ROOT, a renamed directory, or an extension list
// that matches nothing would each silently disarm them while still reporting
// green. This asserts the sweep has real reach, so the guards cannot become
// decoration without something going red.
//
// The floor is deliberately far below the true count (the tree holds hundreds of
// translation units) so ordinary file churn cannot trip it, while a sweep that
// collapsed to a handful of files still fails.
// ---------------------------------------------------------------------------
TEST(BinaryStalenessGuard, TheSourceSweepActuallyReachesTheTree)
{
    static const char* kExts[] = {".cpp", ".cxx", ".cc", ".h", ".hpp", ".hxx", ".inl"};
    int counted = 0;
    for (const char* d : {"src", "tests", "include"}) {
        const fs::path root = repoRoot() / d;
        std::error_code ec;
        if (!fs::exists(root, ec)) continue;
        for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end;
             it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            const std::string ext = it->path().extension().string();
            if (std::find_if(std::begin(kExts), std::end(kExts),
                             [&](const char* e) { return ext == e; }) != std::end(kExts)) {
                ++counted;
            }
        }
    }
    EXPECT_GT(counted, 200)
        << "the staleness sweep found only " << counted
        << " source files under " << repoRoot().string()
        << ". It is not reaching the tree, so both guards above are passing "
           "vacuously — which is worse than not having them, because the run "
           "looks guarded.";
}
