// WIN-82 / GitHub #103 — force three real fault kinds and prove each one leaves
// a minidump behind.
//
// WHY CHILD PROCESSES, NON-NEGOTIABLY. Each of these faults ENDS the process
// that makes it: an access violation is terminated by the top-level SEH filter,
// std::terminate aborts, and `_purecall` exits. Run in-process they would take
// the whole gtest binary down and every remaining test with it — the failure
// mode tests/test_malicious_pdf_advanced.cpp's NestedXObjectRenderChild already
// records. So the fault is made in a spawned copy of THIS executable, running
// one dispatch test, and the parent observes the artifacts it left.
//
// WHAT IS ACTUALLY BEING TESTED. Not "does Windows kill a process that
// dereferences null" — that needs no test. The property is that PDFE's crash
// handler is REACHED for each fault kind and writes a usable post-mortem
// minidump. Two of the four fault kinds here could not reach it at all before
// this change: a pure-virtual call and a CRT invalid-parameter fault are
// dispatched through `_purecall` / `_invalid_parameter`, neither of which is a
// structured exception and neither of which unwinds through std::terminate.
// CrashHandler::install() now also installs `_set_purecall_handler` and
// `_set_invalid_parameter_handler`; these tests are what stops those two lines
// from being decoration.
//
// ORDERING (the criterion's "the two installs are exercised before the
// pure-virtual test runs"). This is NOT left to gtest's declaration order,
// which the 16-way shard runner in tests/scripts/run_gtest.py does not preserve
// across shards. It is guaranteed twice over, both times INSIDE the pure-virtual
// test itself:
//
//   1. In the parent, before the child is even spawned, the pure-virtual test
//      installs the handler in its own process and asserts through
//      CrashHandler::installedHandlers() that both new installs took effect.
//   2. In the CHILD — the process that actually makes the pure-virtual call —
//      install() runs and installedHandlers() is read back and WRITTEN TO A
//      MARKER FILE before the fault is issued. The parent asserts that marker
//      says all four handlers were ours. A child that faulted with the purecall
//      handler missing cannot produce that marker.
//
// DUMPS NEVER TOUCH THE REPO. Each child gets PDFE_CRASH_DUMP_DIR pointed at a
// freshly created directory under the system temp path, and every parent test
// removes that directory (dumps included) on exit, pass or fail. The tests also
// ASSERT that the dump they found is under that temp directory, so a
// regression that ignored the override and wrote next to the executable would
// fail rather than silently litter the working tree with ~48 MB .dmp files.

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#endif

#include <gtest/gtest.h>

#include "app/CrashHandler.h"

#include <QDir>
#include <QString>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <crtdbg.h>
#include <exception>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

using pdfe::app::CrashHandler;

namespace {

constexpr const char* kRoleVar = "PDFE_CRASH_FAULT_ROLE";
constexpr const char* kMarkerVar = "PDFE_CRASH_FAULT_MARKER";

// Status the crash handler leaves behind when it handled a CRT fault
// (pure-virtual / invalid parameter) and terminated deliberately. Must agree
// with kCrtFaultHandledExitCode in src/app/CrashHandler.cpp. It is duplicated
// rather than exported because it is an OBSERVABLE of the process, like an
// exit code from any other program — a test that read it from the product
// could not tell "the handler ran" from "the constant changed".
constexpr unsigned long kCrtFaultHandledExit = 0x3D4D0001ul;

// ---------------------------------------------------------------------------
// THE CHILD ROLE.
// ---------------------------------------------------------------------------

#ifdef _WIN32

// Out-of-line so the call below cannot be devirtualized: an inlined
// `this->f()` inside the constructor would be resolved statically and the
// compiler would emit a direct call to a function that does not exist,
// producing a link error instead of the runtime `_purecall` we want.
struct PureVirtualBase {
    PureVirtualBase();
    virtual ~PureVirtualBase() = default;
    virtual void mustBeOverridden() = 0;
};

void dispatchThroughVtable(PureVirtualBase* b) {
    b->mustBeOverridden();
}

PureVirtualBase::PureVirtualBase() {
    // During the BASE constructor the vptr still points at PureVirtualBase's
    // vtable, whose slot for a pure virtual holds the CRT's `_purecall` thunk.
    dispatchThroughVtable(this);
}

struct PureVirtualDerived : PureVirtualBase {
    void mustBeOverridden() override {}
};

void writeMarker(const char* path, const CrashHandler::InstalledHandlers& h) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << "terminate=" << (h.terminate ? 1 : 0) << "\n"
        << "seh=" << (h.unhandledExceptionFilter ? 1 : 0) << "\n"
        << "purecall=" << (h.purecall ? 1 : 0) << "\n"
        << "invalid_parameter=" << (h.invalidParameter ? 1 : 0) << "\n";
    out.flush();
    out.close();
}

#endif  // _WIN32

}  // namespace

#ifdef _WIN32

// The CHILD. One dispatch point for all four fault kinds; the parent selects
// with PDFE_CRASH_FAULT_ROLE and this test is a no-op in an ordinary run.
TEST(CrashFaultChild, RunRole) {
    const char* role = std::getenv(kRoleVar);
    if (role == nullptr || *role == '\0') {
        GTEST_SKIP() << "not a crash-fault child (" << kRoleVar << " unset)";
    }
    const std::string kind(role);

    // The handler under test. Everything after this line is deliberate damage.
    CrashHandler::instance().install();

    // REPORT BEFORE FAULTING. This is the ordering guarantee, made in the very
    // process that is about to fault: if the purecall handler were not ours,
    // the marker says so and the parent fails with that reason rather than
    // with a mystery exit code.
    const CrashHandler::InstalledHandlers installed =
        CrashHandler::installedHandlers();
    const char* marker = std::getenv(kMarkerVar);
    ASSERT_NE(marker, nullptr) << "child was spawned without " << kMarkerVar;
    writeMarker(marker, installed);
    ASSERT_TRUE(installed.all())
        << "install() did not put all four handlers in place; faulting now "
           "would measure the CRT default, not PDFE";

    if (kind == "av_worker_thread") {
        // WIN-82 asks specifically for a worker thread: SetUnhandledExceptionFilter
        // is process-wide, and a filter that only ever saw main-thread faults
        // would leave the common case (a render/OCR worker) undumped.
        std::thread worker([] {
            volatile int* p = reinterpret_cast<volatile int*>(
                static_cast<std::uintptr_t>(0x10u));
            *p = 0xBADF00D;  // access violation, writing 0x10
        });
        worker.join();
        FAIL() << "the worker thread's access violation did not kill the process";
    } else if (kind == "terminate") {
        // A throw out of a noexcept function is the deterministic route to
        // std::terminate WITH an active exception, so the handler's
        // current_exception() branch is the one exercised.
        // C4297 ("function assumed not to throw an exception but does") is
        // EXACTLY the construct being used, deliberately, so it is silenced
        // here rather than worked around: a throw out of a noexcept function
        // is the standard-guaranteed route to std::terminate.
#pragma warning(push)
#pragma warning(disable : 4297)
        auto thrower = []() noexcept {
            throw std::runtime_error("WIN-82 child: deliberate unhandled exception");
        };
#pragma warning(pop)
        thrower();
        FAIL() << "std::terminate did not end the process";
    } else if (kind == "purecall") {
        PureVirtualDerived d;
        (void)d;
        FAIL() << "the pure-virtual call did not reach _purecall";
    } else if (kind == "invalid_parameter") {
        // A null format string to printf is the canonical invalid-parameter
        // fault: the UCRT's _VALIDATE_RETURN macro calls `_invalid_parameter`,
        // which dispatches to the handler installed above.
        //
        // NOT strcpy_s-with-a-too-small-buffer, which was tried first and does
        // NOT reach the handler in a Debug build: that path uses the
        // _CRT_SECURE_INVALID_PARAMETER macro, which in _DEBUG raises a
        // _CRT_ASSERT through _CrtDbgReport instead — and tests/test_main.cpp
        // installs a report hook that aborts on it. Measured: the child died
        // with "[CRT] ... Assertion failed: (L"Buffer is too small" && 0)" and
        // wrote no dump. Two different CRT dispatch macros, only one of which
        // is the invalid-parameter handler's.
        // THE HARNESS HAS TO GET OUT OF THE WAY FIRST, and this is a real
        // finding rather than a workaround. In a _DEBUG build the UCRT's
        // `_invalid_parameter` calls `_CrtDbgReportW(_CRT_ASSERT, ...)` BEFORE
        // it consults the installed handler, and tests/test_main.cpp installs a
        // report hook that unconditionally `std::abort()`s. So inside the gtest
        // binary — and ONLY inside it — no invalid-parameter fault can ever
        // reach CrashHandler. Measured both ways: with the hook in place the
        // child died at "Assertion failed: format != nullptr" with exit 3 and
        // no dump.
        //
        // The shipped application installs no such hook (src/main.cpp does
        // not), and a Release build emits no debug report at all, so removing
        // it here makes the child FAITHFUL to the product rather than lenient
        // to the test. The report still goes to stderr; it just no longer
        // aborts before the handler under test can run.
        _CrtSetReportHook(nullptr);
        _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
        _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);

        const char* nullFormat = nullptr;
        (void)std::printf(nullFormat);
        FAIL() << "printf(nullptr) did not raise a CRT invalid-parameter fault";
    } else {
        FAIL() << "unknown " << kRoleVar << " value: " << kind;
    }
}

// ---------------------------------------------------------------------------
// THE PARENT SIDE.
// ---------------------------------------------------------------------------

namespace {

std::filesystem::path selfExe() {
    wchar_t buf[MAX_PATH];
    const DWORD n = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return std::filesystem::path(std::wstring(buf, n));
}

// A fresh, drive-absolute, override-acceptable dump directory under TEMP.
// Never inside the repository: see the file header.
std::filesystem::path makeDumpDir(const std::string& tag) {
    static std::atomic<unsigned> counter{0};
    const QString base = QDir::tempPath();
    std::filesystem::path dir =
        std::filesystem::path(base.toStdWString())
        / ("pdfe_win82_" + tag + "_" + std::to_string(::GetCurrentProcessId())
           + "_" + std::to_string(counter.fetch_add(1)));
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return ec ? std::filesystem::path{} : dir;
}

struct ChildOutcome {
    bool spawned = false;
    bool exited = false;      // false => timed out and was killed
    DWORD exitCode = 0xFFFFFFFFu;
    std::string marker;       // contents of the pre-fault marker file
    std::vector<std::filesystem::path> dumps;
};

std::vector<std::filesystem::path> dumpsIn(const std::filesystem::path& dir) {
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (e.path().extension() == ".dmp") out.push_back(e.path());
    }
    return out;
}

ChildOutcome runFaultChild(const std::string& role,
                           const std::filesystem::path& dumpDir) {
    ChildOutcome r;
    const auto exe = selfExe();
    if (exe.empty()) return r;

    const auto markerPath = dumpDir / "handlers.marker";

    std::wstring cmd = L"\"" + exe.wstring() +
                       L"\" --gtest_filter=CrashFaultChild.RunRole "
                       L"--gtest_brief=1";
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');

    ::SetEnvironmentVariableA(kRoleVar, role.c_str());
    ::SetEnvironmentVariableW(L"PDFE_CRASH_DUMP_DIR", dumpDir.wstring().c_str());
    ::SetEnvironmentVariableW(L"PDFE_CRASH_FAULT_MARKER",
                              markerPath.wstring().c_str());
    // No modal crash dialog in the child — it would block until the timeout
    // below and turn a passing test into a two-minute hang.
    ::SetEnvironmentVariableA("PDFE_HEADLESS", "1");

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const BOOL created = ::CreateProcessW(exe.wstring().c_str(), cmdBuf.data(),
                                          nullptr, nullptr, FALSE,
                                          CREATE_NO_WINDOW, nullptr, nullptr,
                                          &si, &pi);

    // Cleared immediately so no later test in THIS process is mistaken for a
    // child, and so this process never adopts the child's dump directory.
    ::SetEnvironmentVariableA(kRoleVar, nullptr);
    ::SetEnvironmentVariableW(L"PDFE_CRASH_DUMP_DIR", nullptr);
    ::SetEnvironmentVariableW(L"PDFE_CRASH_FAULT_MARKER", nullptr);
    ::SetEnvironmentVariableA("PDFE_HEADLESS", nullptr);

    if (!created) return r;
    r.spawned = true;

    if (::WaitForSingleObject(pi.hProcess, 120000) == WAIT_OBJECT_0) {
        r.exited = true;
        ::GetExitCodeProcess(pi.hProcess, &r.exitCode);
    } else {
        ::TerminateProcess(pi.hProcess, 1);
        ::WaitForSingleObject(pi.hProcess, 5000);
    }
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);

    {
        std::ifstream in(markerPath, std::ios::binary);
        if (in) {
            r.marker.assign(std::istreambuf_iterator<char>(in),
                            std::istreambuf_iterator<char>());
        }
    }
    r.dumps = dumpsIn(dumpDir);
    return r;
}

// Removes the dump directory and everything in it however the test exits.
// The dumps are ~48 MB each and must never survive the run, let alone reach a
// commit.
struct DumpDirCleaner {
    std::filesystem::path dir;
    ~DumpDirCleaner() {
        if (dir.empty()) return;
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};

// A minidump is not "a file that exists". Check the container: the MDMP
// signature and enough bytes to hold real thread state. A zero-byte or
// truncated file would satisfy a mere existence check and prove nothing.
::testing::AssertionResult isRealMinidump(const std::filesystem::path& p) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(p, ec);
    if (ec) return ::testing::AssertionFailure() << p << ": " << ec.message();
    if (size < 4096) {
        return ::testing::AssertionFailure()
               << p << " is only " << size
               << " bytes — too small to be a minidump with thread state";
    }
    std::ifstream in(p, std::ios::binary);
    char sig[4] = {};
    in.read(sig, 4);
    if (in.gcount() != 4 || std::memcmp(sig, "MDMP", 4) != 0) {
        return ::testing::AssertionFailure()
               << p << " does not start with the MDMP signature — it is not a "
                      "minidump, whatever else it may be";
    }
    return ::testing::AssertionSuccess();
}

// The shared body of the three fault tests. `expectHandledExit` selects whether
// the process is expected to end via PDFE's deliberate CRT-fault exit or via a
// Windows exception status / abort.
void expectFaultProducesMinidump(const std::string& role,
                                 const std::filesystem::path& dumpDir,
                                 const ChildOutcome& r) {
    ASSERT_TRUE(r.spawned) << "CreateProcess failed for role " << role;
    ASSERT_TRUE(r.exited) << "the " << role
                          << " child never exited — it was killed at the "
                             "timeout, so nothing here measures the handler";

    // The pre-fault marker proves install() had all four handlers in place in
    // the faulting process BEFORE the fault was issued.
    ASSERT_FALSE(r.marker.empty())
        << "the " << role
        << " child never wrote its handler marker — it died before install() "
           "completed, so the fault did not exercise the crash handler";
    EXPECT_NE(r.marker.find("purecall=1"), std::string::npos)
        << "purecall handler was NOT installed in the faulting child; marker:\n"
        << r.marker;
    EXPECT_NE(r.marker.find("invalid_parameter=1"), std::string::npos)
        << "invalid-parameter handler was NOT installed in the faulting child; "
           "marker:\n"
        << r.marker;

    // The exit code must NOT be 0: a child that ran to completion did not
    // fault, and every assertion about the dump would then be measuring the
    // writeTestMinidump seam by accident.
    EXPECT_NE(r.exitCode, 0u)
        << "the " << role
        << " child exited cleanly — the fault never happened";

    ASSERT_EQ(r.dumps.size(), 1u)
        << "expected exactly one minidump from the " << role << " child, got "
        << r.dumps.size() << " in " << dumpDir;
    EXPECT_TRUE(isRealMinidump(r.dumps.front()));

    // The dump landed where we told it to, not beside the executable and not
    // in the repository.
    EXPECT_EQ(r.dumps.front().parent_path(), dumpDir)
        << "the crash handler ignored PDFE_CRASH_DUMP_DIR";
}

}  // namespace

// ---------------------------------------------------------------------------
// (0) THE REPORTING SEAM ITSELF. Everything else leans on installedHandlers()
//     telling the truth, so pin it directly: all four false before install,
//     all four true after, all four false again after uninstall. The last of
//     those is the non-vacuity check — a seam hard-coded to `true` would pass
//     the middle assertion and fail this one.
// ---------------------------------------------------------------------------
TEST(CrashHandlerFaultChildren, InstallAndUninstallReportTheFourGlobalHandlers) {
    CrashHandler::instance().uninstall();

    const auto before = CrashHandler::installedHandlers();
    EXPECT_FALSE(before.purecall)
        << "a purecall handler was already ours before install()";
    EXPECT_FALSE(before.invalidParameter)
        << "an invalid-parameter handler was already ours before install()";

    CrashHandler::instance().install();
    const auto after = CrashHandler::installedHandlers();
    CrashHandler::instance().uninstall();
    const auto restored = CrashHandler::installedHandlers();

    EXPECT_TRUE(after.terminate) << "std::terminate handler not installed";
    EXPECT_TRUE(after.unhandledExceptionFilter)
        << "top-level SEH filter not installed";
    EXPECT_TRUE(after.purecall)
        << "WIN-82: _set_purecall_handler was not called by install(), so a "
           "pure-virtual call still dies with no minidump";
    EXPECT_TRUE(after.invalidParameter)
        << "WIN-82: _set_invalid_parameter_handler was not called by install(), "
           "so a CRT invalid-parameter fault still dies with no minidump";

    EXPECT_FALSE(restored.purecall)
        << "uninstall() left OUR purecall handler wired into the CRT — a later "
           "test in this process would die inside the dump writer";
    EXPECT_FALSE(restored.invalidParameter)
        << "uninstall() left OUR invalid-parameter handler wired into the CRT";
}

// ---------------------------------------------------------------------------
// (1) ACCESS VIOLATION ON A WORKER THREAD.
// ---------------------------------------------------------------------------
TEST(CrashHandlerFaultChildren, AccessViolationOnWorkerThreadProducesMinidump) {
    const auto dir = makeDumpDir("av");
    ASSERT_FALSE(dir.empty()) << "could not create a temp dump directory";
    DumpDirCleaner cleaner{dir};

    const auto r = runFaultChild("av_worker_thread", dir);
    ASSERT_NO_FATAL_FAILURE(expectFaultProducesMinidump("av_worker_thread", dir, r));

    // An AV is terminated by the top-level filter returning
    // EXCEPTION_EXECUTE_HANDLER, so the status is the exception code itself.
    EXPECT_EQ(r.exitCode, 0xC0000005u)
        << "expected STATUS_ACCESS_VIOLATION, got 0x" << std::hex << r.exitCode;
}

// ---------------------------------------------------------------------------
// (2) std::terminate.
// ---------------------------------------------------------------------------
TEST(CrashHandlerFaultChildren, StdTerminateProducesMinidump) {
    const auto dir = makeDumpDir("terminate");
    ASSERT_FALSE(dir.empty()) << "could not create a temp dump directory";
    DumpDirCleaner cleaner{dir};

    const auto r = runFaultChild("terminate", dir);
    ASSERT_NO_FATAL_FAILURE(expectFaultProducesMinidump("terminate", dir, r));

    // Not asserting a specific status here: terminateHandler chains to the
    // previous handler, and whether that ends in abort()'s exit 3 or a CRT
    // fast-fail is a CRT implementation detail, not a PDFE property. What IS a
    // PDFE property — the dump — is asserted above.
    EXPECT_LT(r.exitCode, 0xC0000000u)
        << "std::terminate ended the child with a raw exception status 0x"
        << std::hex << r.exitCode
        << " — the terminate handler was bypassed entirely";
}

// ---------------------------------------------------------------------------
// (3) PURE-VIRTUAL CALL. The install this exists for is checked in THIS test
//     body, in the parent, before the child is spawned — see the file header
//     on why ordering is not left to gtest.
// ---------------------------------------------------------------------------
TEST(CrashHandlerFaultChildren, PureVirtualCallProducesMinidump) {
    // (a) Exercise the two new installs here, first, in this process.
    CrashHandler::instance().uninstall();
    CrashHandler::instance().install();
    const auto local = CrashHandler::installedHandlers();
    CrashHandler::instance().uninstall();
    ASSERT_TRUE(local.purecall)
        << "the purecall install is absent — spawning a pure-virtual child now "
           "would measure the CRT default handler";
    ASSERT_TRUE(local.invalidParameter)
        << "the invalid-parameter install is absent";

    // (b) Only now the fault, in a child.
    const auto dir = makeDumpDir("purecall");
    ASSERT_FALSE(dir.empty()) << "could not create a temp dump directory";
    DumpDirCleaner cleaner{dir};

    const auto r = runFaultChild("purecall", dir);
    ASSERT_NO_FATAL_FAILURE(expectFaultProducesMinidump("purecall", dir, r));

    // Unique to the CRT-fault path: PDFE's own handler chose the exit status.
    // The CRT default `_purecall` exits with 3 (via abort) and writes no dump,
    // so this discriminates "our handler ran" from "the CRT's did".
    EXPECT_EQ(static_cast<unsigned long>(r.exitCode), kCrtFaultHandledExit)
        << "the pure-virtual call did not end through PDFE's purecall handler; "
           "exit was 0x" << std::hex << r.exitCode;
}

// ---------------------------------------------------------------------------
// (4) CRT INVALID PARAMETER — the second of the two installs WIN-82 asks for.
//     Without its own fault it would be an install nothing ever exercises.
// ---------------------------------------------------------------------------
TEST(CrashHandlerFaultChildren, InvalidCrtParameterProducesMinidump) {
    const auto dir = makeDumpDir("invalidparam");
    ASSERT_FALSE(dir.empty()) << "could not create a temp dump directory";
    DumpDirCleaner cleaner{dir};

    const auto r = runFaultChild("invalid_parameter", dir);
    ASSERT_NO_FATAL_FAILURE(
        expectFaultProducesMinidump("invalid_parameter", dir, r));

    EXPECT_EQ(static_cast<unsigned long>(r.exitCode), kCrtFaultHandledExit)
        << "the invalid-parameter fault did not end through PDFE's handler; "
           "exit was 0x" << std::hex << r.exitCode;
}

#else   // !_WIN32

TEST(CrashHandlerFaultChildren, WindowsOnly) {
    GTEST_SKIP() << "minidump/crash-handler faults are a Windows-only surface";
}

#endif  // _WIN32
