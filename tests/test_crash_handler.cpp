// Unit tests for pdfe::app::CrashHandler
// Tests the API surface without triggering actual crashes

#include <gtest/gtest.h>
#include "app/CrashHandler.h"
#include <QString>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

using namespace pdfe::app;

// Test fixture for CrashHandler tests
class CrashHandlerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Ensure clean state before each test
        CrashHandler::instance().uninstall();
    }

    void TearDown() override {
        // Always uninstall after each test to avoid side effects
        CrashHandler::instance().uninstall();
    }
};

// Test 1: Verify singleton pattern - instance() returns same address twice
TEST_F(CrashHandlerTest, Instance_Singleton) {
    // Arrange & Act
    CrashHandler& instance1 = CrashHandler::instance();
    CrashHandler& instance2 = CrashHandler::instance();

    // Assert - Both references should point to the same object
    EXPECT_EQ(&instance1, &instance2)
        << "instance() should return the same singleton instance";
}

// Test 2: Verify install() and uninstall() operations complete without exceptions
TEST_F(CrashHandlerTest, Install_Uninstall) {
    // Arrange
    CrashHandler& handler = CrashHandler::instance();

    // Act & Assert - install() should not throw
    EXPECT_NO_THROW(handler.install())
        << "install() should complete without throwing exceptions";

    // Act & Assert - uninstall() should not throw
    EXPECT_NO_THROW(handler.uninstall())
        << "uninstall() should complete without throwing exceptions";

    // Additional verification - multiple install/uninstall cycles
    EXPECT_NO_THROW({
        handler.install();
        handler.uninstall();
        handler.install();
        handler.uninstall();
    }) << "Multiple install/uninstall cycles should work correctly";
}

// Test 3: Verify setTestCallback accepts a lambda without crashing/throwing
TEST_F(CrashHandlerTest, TestCallback_Set) {
    // Arrange
    CrashHandler& handler = CrashHandler::instance();
    bool callbackCalled = false;
    QString capturedTitle;
    QString capturedDetails;

    // Create a test callback lambda
    auto testCallback = [&](const QString& title, const QString& details) {
        callbackCalled = true;
        capturedTitle = title;
        capturedDetails = details;
    };

    // Act & Assert - Setting callback should not throw
    EXPECT_NO_THROW(handler.setTestCallback(testCallback))
        << "setTestCallback should accept a lambda without throwing";

    // Reset callback to null
    EXPECT_NO_THROW(handler.setTestCallback(nullptr))
        << "Resetting callback to null should not throw";
}

// Test 4: Verify lastCrashInfo() returns empty strings initially
TEST_F(CrashHandlerTest, LastCrashInfo_InitiallyEmpty) {
    // Arrange
    CrashHandler& handler = CrashHandler::instance();

    // Act
    CrashHandler::CrashInfo info = handler.lastCrashInfo();

    // Assert - Initial crash info should be empty
    EXPECT_TRUE(info.title.isEmpty())
        << "Initial crash info title should be empty";
    EXPECT_TRUE(info.details.isEmpty())
        << "Initial crash info details should be empty";

    // Alternative assertion using QString length
    EXPECT_EQ(0, info.title.length())
        << "Initial crash info title length should be 0";
    EXPECT_EQ(0, info.details.length())
        << "Initial crash info details length should be 0";
}

// Additional edge case test: Verify uninstall without install doesn't crash
TEST_F(CrashHandlerTest, Uninstall_WithoutInstall_DoesNotCrash) {
    // Arrange
    CrashHandler& handler = CrashHandler::instance();
    // Note: SetUp already called uninstall, so handler is in uninstalled state

    // Act & Assert - Calling uninstall when not installed should be safe
    EXPECT_NO_THROW(handler.uninstall())
        << "uninstall() without prior install() should be safe";
}

// Additional edge case test: Verify multiple install calls are safe
TEST_F(CrashHandlerTest, Install_MultipleCalls_DoesNotCrash) {
    // Arrange
    CrashHandler& handler = CrashHandler::instance();

    // Act & Assert - Multiple install calls should be safe
    EXPECT_NO_THROW({
        handler.install();
        handler.install(); // Second install
        handler.install(); // Third install
    }) << "Multiple install() calls should be handled safely";

    // Cleanup
    handler.uninstall();
}

// Additional test: Verify setTestCallback with nullptr doesn't crash
TEST_F(CrashHandlerTest, TestCallback_SetNull_DoesNotCrash) {
    // Arrange
    CrashHandler& handler = CrashHandler::instance();

    // Act & Assert - Setting null callback should not throw
    EXPECT_NO_THROW(handler.setTestCallback(nullptr))
        << "setTestCallback with nullptr should not crash";

    // Setting empty/null callback should also work
    EXPECT_NO_THROW(handler.setTestCallback(CrashHandler::CrashCallback()))
        << "setTestCallback with empty CrashCallback should not crash";
}

// --- Post-mortem observability + automation behavior (teardown-AV work) -------

// The crash path writes a full minidump so an otherwise-invisible teardown AV
// leaves a stack behind. Exercise the exact MiniDumpWriteDump/DbgHelp plumbing
// (no real access violation needed) and assert a genuine .dmp is produced.
TEST_F(CrashHandlerTest, WriteTestMinidump_ProducesRealDumpFile) {
#ifndef _WIN32
    GTEST_SKIP() << "minidump is a Windows-only facility";
#else
    const QString path = CrashHandler::writeTestMinidump();
    ASSERT_FALSE(path.isEmpty())
        << "writeTestMinidump() returned empty — MiniDumpWriteDump/DbgHelp "
           "plumbing failed (is dbghelp linked?)";
    const std::filesystem::path p = path.toStdWString();
    ASSERT_TRUE(std::filesystem::exists(p))
        << "reported dump file does not exist: " << path.toStdString();
    // A real minidump has a non-trivial size and the 'MDMP' signature.
    EXPECT_GT(std::filesystem::file_size(p), 1024u)
        << "dump is implausibly small: " << path.toStdString();
    {
        std::ifstream f(p, std::ios::binary);
        char sig[4] = {0};
        f.read(sig, 4);
        EXPECT_EQ(std::string(sig, 4), std::string("MDMP", 4))
            << "file lacks the MDMP minidump signature";
    }
    std::error_code ec;
    std::filesystem::remove(p, ec);  // clean up
#endif
}

// Under automation (PDFE_UI_TEST / PDFE_HEADLESS / CI) a crash must not pop a
// modal dialog. Verify the detector that gates that behavior.
TEST_F(CrashHandlerTest, IsAutomatedRun_DetectsEnvVars) {
    auto save = [](const char* k) -> std::string {
        const char* v = std::getenv(k); return v ? std::string(v) : std::string();
    };
    auto set = [](const char* k, const char* v) {
#ifdef _WIN32
        _putenv_s(k, v);
#else
        if (v && *v) setenv(k, v, 1); else unsetenv(k);
#endif
    };
    const std::string s1 = save("PDFE_UI_TEST"), s2 = save("PDFE_HEADLESS"), s3 = save("CI");

    set("PDFE_UI_TEST", ""); set("PDFE_HEADLESS", ""); set("CI", "");
    EXPECT_FALSE(CrashHandler::isAutomatedRun())
        << "no automation env var set, but isAutomatedRun() is true";

    set("PDFE_UI_TEST", "1");
    EXPECT_TRUE(CrashHandler::isAutomatedRun())
        << "PDFE_UI_TEST=1 must be detected as an automated run";

    // restore
    set("PDFE_UI_TEST", s1.c_str()); set("PDFE_HEADLESS", s2.c_str()); set("CI", s3.c_str());
}

// ===========================================================================
// PHASE-11: crash-dump directory hardening — ACL, override validation, and
// retention pruning.
//
// From the 2026-08-10 reverification §7. All three findings were CONFIRMED
// against the tree: the directory was created with null SECURITY_ATTRIBUTES,
// PDFE_CRASH_DUMP_DIR was honoured with no validation, and the 150 cap was a
// brake (refuse the 151st crash) rather than a retention policy (prune the
// oldest). A minidump here carries MiniDumpWithDataSegs and
// MiniDumpWithIndirectlyReferencedMemory, so it is potentially secret-bearing —
// which is what makes all three of those matter rather than being tidiness.
// ===========================================================================

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <AclApi.h>
#include <sddl.h>
#include <vector>

namespace {

std::filesystem::path freshCrashDir(const char* tag)
{
    auto d = std::filesystem::temp_directory_path()
             / (std::string("pdfe_p11_") + tag + "_"
                + std::to_string(::GetCurrentProcessId())
                + "_" + std::to_string(::GetTickCount64()));
    std::error_code ec;
    std::filesystem::remove_all(d, ec);
    std::filesystem::create_directories(d, ec);
    return d;
}

// Create `n` empty .dmp files with STRICTLY INCREASING last-write times, and
// return them oldest-first. The pruning policy is oldest-first, so a test that
// cannot order the files cannot tell pruning from arbitrary deletion.
std::vector<std::filesystem::path> makeAgedDumps(const std::filesystem::path& dir, int n)
{
    std::vector<std::filesystem::path> made;
    FILETIME base{};
    ::GetSystemTimeAsFileTime(&base);
    ULARGE_INTEGER start;
    start.LowPart = base.dwLowDateTime;
    start.HighPart = base.dwHighDateTime;
    // Walk BACKWARDS from now so index 0 is the oldest; 10 s apart in 100 ns
    // units, comfortably beyond any filesystem timestamp granularity.
    const unsigned long long stepNs100 = 10ULL * 1000ULL * 1000ULL * 10ULL;

    for (int i = 0; i < n; ++i) {
        auto p = dir / ("aged_" + std::to_string(i) + ".dmp");
        { std::ofstream f(p, std::ios::binary); f << "x"; }
        ULARGE_INTEGER t;
        t.QuadPart = start.QuadPart - static_cast<unsigned long long>(n - i) * stepNs100;
        FILETIME ft;
        ft.dwLowDateTime = t.LowPart;
        ft.dwHighDateTime = t.HighPart;
        HANDLE h = ::CreateFileW(p.wstring().c_str(), FILE_WRITE_ATTRIBUTES,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            ::SetFileTime(h, nullptr, nullptr, &ft);
            ::CloseHandle(h);
        }
        made.push_back(p);
    }
    return made;
}

int countDumps(const std::filesystem::path& dir)
{
    int n = 0;
    std::error_code ec;
    for (auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (e.path().extension() == ".dmp") ++n;
    }
    return n;
}

// RAII for PDFE_CRASH_DUMP_DIR so a failing assertion cannot leak the override
// into every later test in the process.
class ScopedCrashDirEnv {
public:
    explicit ScopedCrashDirEnv(const std::wstring& value)
    {
        wchar_t buf[MAX_PATH];
        DWORD n = ::GetEnvironmentVariableW(L"PDFE_CRASH_DUMP_DIR", buf, MAX_PATH);
        if (n > 0 && n < MAX_PATH) { had_ = true; prev_.assign(buf, n); }
        ::SetEnvironmentVariableW(L"PDFE_CRASH_DUMP_DIR", value.c_str());
    }
    ~ScopedCrashDirEnv()
    {
        ::SetEnvironmentVariableW(L"PDFE_CRASH_DUMP_DIR", had_ ? prev_.c_str() : nullptr);
    }
private:
    bool had_ = false;
    std::wstring prev_;
};

}  // namespace

// ---------------------------------------------------------------------------
// (1) OVERRIDE VALIDATION. Each rejected shape names a way a secret-bearing
// dump lands somewhere we cannot protect. Table-driven so adding a shape is one
// row, and so a validator that accepts everything fails on the first negative.
// ---------------------------------------------------------------------------
TEST_F(CrashHandlerTest, CrashDumpDirOverride_RejectsUnprotectablePaths)
{
    struct Case { const char* path; bool acceptable; const char* why; };
    const Case cases[] = {
        {"C:\\Users\\x\\dumps",        true,  "plain drive-absolute local path"},
        {"c:/users/x/dumps",           true,  "forward slashes are a valid separator on Windows"},
        {"\\\\server\\share\\dumps",   false, "UNC: the dump leaves the machine, under an ACL we cannot verify"},
        {"//server/share/dumps",       false, "UNC with forward slashes is still UNC"},
        {"\\\\?\\C:\\dumps",           false, "device namespace bypasses the path parser these checks assume"},
        {"\\\\.\\PhysicalDrive0",      false, "device namespace"},
        {"dumps",                      false, "relative: resolves against a CWD that varies by launcher"},
        {"..\\dumps",                  false, "relative escape"},
        {"C:\\Users\\..\\Windows",     false, "'..' segment: an escape smuggled through an absolute-looking path"},
        {"C:dumps",                    false, "drive-RELATIVE: resolves against that drive's own current directory"},
        {"",                           false, "empty"},
    };

    int accepted = 0, rejected = 0;
    for (const auto& c : cases) {
        const bool got = CrashHandler::isAcceptableCrashDumpDirOverride(QString::fromUtf8(c.path));
        EXPECT_EQ(got, c.acceptable)
            << "path \"" << c.path << "\" — " << c.why;
        (c.acceptable ? accepted : rejected)++;
    }
    // Non-vacuity: a validator stuck at always-true or always-false would pass
    // half the rows above; this pins that the table exercises BOTH verdicts.
    EXPECT_GT(accepted, 0);
    EXPECT_GT(rejected, 0);
}

// A path containing "..." or a filename merely STARTING with ".." is not a
// parent escape and must not be rejected — an over-broad check that refuses
// legitimate directories pushes users back to the unvalidated default.
TEST_F(CrashHandlerTest, CrashDumpDirOverride_DoesNotRejectInnocentDots)
{
    EXPECT_TRUE(CrashHandler::isAcceptableCrashDumpDirOverride("C:\\dumps\\..a"));
    EXPECT_TRUE(CrashHandler::isAcceptableCrashDumpDirOverride("C:\\dumps\\a..b"));
    EXPECT_TRUE(CrashHandler::isAcceptableCrashDumpDirOverride("C:\\dumps\\..."));
}

// ---------------------------------------------------------------------------
// (2) THE ACL. The directory must end up with a PROTECTED DACL — inheritance
// severed — so a permissive ACL on the parent cannot widen it back out.
// ---------------------------------------------------------------------------
TEST_F(CrashHandlerTest, ResolvedCrashDumpDir_HasProtectedOwnerOnlyDacl)
{
    const auto dir = freshCrashDir("acl");
    ScopedCrashDirEnv env(dir.wstring());

    const QString resolved = CrashHandler::resolvedCrashDumpDir();
    ASSERT_FALSE(resolved.isEmpty())
        << "resolvedCrashDumpDir() refused a perfectly good local directory";

    PSECURITY_DESCRIPTOR sd = nullptr;
    PACL dacl = nullptr;
    const DWORD rc = ::GetNamedSecurityInfoW(
        resolved.toStdWString().c_str(), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr, &sd);
    ASSERT_EQ(rc, static_cast<DWORD>(ERROR_SUCCESS)) << "could not read the DACL back";
    ASSERT_NE(dacl, nullptr);

    SECURITY_DESCRIPTOR_CONTROL ctrl{};
    DWORD rev = 0;
    ASSERT_TRUE(::GetSecurityDescriptorControl(sd, &ctrl, &rev));

    // THE assertion. SE_DACL_PROTECTED is what makes "owner-only" stick: without
    // it the parent's inheritable ACEs flow in and the restriction is cosmetic.
    EXPECT_TRUE((ctrl & SE_DACL_PROTECTED) != 0)
        << "the crash-dump directory's DACL is NOT protected, so inherited ACEs "
           "from its parent still apply — a dump that can carry heap contents is "
           "readable by whoever the parent grants";

    ::LocalFree(sd);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// A rejected override must NOT silently become "write dumps to the rejected
// place anyway" — nor kill capture entirely. It falls back to TEMP.
TEST_F(CrashHandlerTest, RejectedOverrideFallsBackToTempRatherThanObeyingIt)
{
    ScopedCrashDirEnv env(L"\\\\evil-share\\dumps");

    const QString resolved = CrashHandler::resolvedCrashDumpDir();
    ASSERT_FALSE(resolved.isEmpty())
        << "a rejected override killed crash capture entirely; it must fall back "
           "to TEMP so crashes are still recorded";
    EXPECT_FALSE(resolved.startsWith("\\\\"))
        << "the UNC override was obeyed despite being rejected; got "
        << resolved.toStdString();
}

// ---------------------------------------------------------------------------
// (3) RETENTION. This is the behaviour change: at the cap, prune the OLDEST
// rather than refuse the newest. The newest crash is the one being debugged.
// ---------------------------------------------------------------------------
TEST_F(CrashHandlerTest, PruneCrashDumps_DeletesOldestFirstDownToKeep)
{
    const auto dir = freshCrashDir("prune");
    const auto aged = makeAgedDumps(dir, 10);
    ASSERT_EQ(countDumps(dir), 10);

    const int deleted =
        CrashHandler::pruneCrashDumps(QString::fromStdWString(dir.wstring()), 4);

    EXPECT_EQ(deleted, 6);
    EXPECT_EQ(countDumps(dir), 4);

    // Oldest-first, specifically — not "some six files". aged[] is oldest-first.
    for (int i = 0; i < 6; ++i) {
        EXPECT_FALSE(std::filesystem::exists(aged[i]))
            << "aged_" << i << ".dmp survived; pruning is not oldest-first";
    }
    for (int i = 6; i < 10; ++i) {
        EXPECT_TRUE(std::filesystem::exists(aged[i]))
            << "aged_" << i << ".dmp was deleted; pruning took NEWER dumps, which "
               "discards exactly the crash most likely under investigation";
    }

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// Non-vacuity for the pruner: below the keep threshold it must delete NOTHING.
// A pruner that always empties the directory would pass the test above.
TEST_F(CrashHandlerTest, PruneCrashDumps_BelowThresholdDeletesNothing)
{
    const auto dir = freshCrashDir("nokill");
    makeAgedDumps(dir, 3);
    ASSERT_EQ(countDumps(dir), 3);

    EXPECT_EQ(CrashHandler::pruneCrashDumps(QString::fromStdWString(dir.wstring()), 10), 0);
    EXPECT_EQ(countDumps(dir), 3);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// THE REGRESSION THIS PHASE FIXES, end to end and through the real crash path.
// At the cap, a new dump must still be WRITTEN. The old code returned early
// here and the 151st crash vanished with only a line on stderr.
//
// Uses maxCrashDumps() rather than a literal 150, so the test tracks the product
// if the cap changes instead of silently testing a stale number.
TEST_F(CrashHandlerTest, AtTheCap_TheNewestCrashIsStillCapturedAndTheOldestIsPruned)
{
    const auto dir = freshCrashDir("cap");
    ScopedCrashDirEnv env(dir.wstring());

    const int cap = CrashHandler::maxCrashDumps();
    ASSERT_GT(cap, 1) << "cap must be meaningful for this test to mean anything";

    // Fill exactly to the cap with cheap placeholder dumps — writing `cap` real
    // minidumps would be ~7 GB.
    const auto aged = makeAgedDumps(dir, cap);
    ASSERT_EQ(countDumps(dir), cap);

    const QString written = CrashHandler::writeTestMinidump();

    EXPECT_FALSE(written.isEmpty())
        << "at the cap the crash path refused to write, so the NEWEST crash — the "
           "one being investigated — was silently discarded. This is the phase-11 "
           "regression: the cap must prune, not brake.";
    EXPECT_TRUE(std::filesystem::exists(written.toStdWString()))
        << "writeTestMinidump() reported a path that does not exist";
    EXPECT_FALSE(std::filesystem::exists(aged[0]))
        << "the oldest placeholder survived; nothing was pruned to make room";
    EXPECT_LE(countDumps(dir), cap)
        << "the directory grew past the cap — the disk-safety property the cap "
           "exists for has been lost while fixing the retention half";

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}
#endif  // _WIN32
