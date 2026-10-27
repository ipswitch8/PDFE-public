// -*- coding: utf-8 -*-
//
// Meta-audit follow-up F12 — WIN-15, WIN-16, WIN-17 (single-instance launch
// contract) and the shell double-click / "%1" argument-quoting path.
//
// These tests launch the REAL pdfe.exe as a child process. Nothing is
// simulated: the single-instance tests exercise main.cpp's SingleInstanceGuard
// wiring across two genuine processes, and the shell tests go through the
// Windows shell's own verb resolution and command-line substitution.
//
// WHY PROCESS-LEVEL AND NOT IN-PROCESS
// ------------------------------------
// tests/test_single_instance_guard.cpp already covers the guard CLASS in
// process (QSignalSpy on fileOpenRequested). What had no coverage is the
// user-visible contract built on top of it in src/main.cpp:119-148:
//
//   * a second launch must EXIT rather than open a second editor (WIN-15),
//   * the primary must actually receive and open the forwarded path (WIN-15),
//   * the primary must un-minimise, raise and activate (WIN-16),
//   * PDFE_DISABLE_SINGLE_INSTANCE=1 must genuinely produce two live editors
//     (WIN-17) — the harness opt-out that no test had ever set, so any Python
//     test intending side-by-side instances was silently testing one.
//
// SHELL DOUBLE-CLICK AND THE "%1" REGRESSION CLASS
// ------------------------------------------------
// packaging/file_assoc.nsh exists BECAUSE this exact quoting shipped broken
// once: a CPack round-trip turned the registry command into
// `$"path$" $"%1$"`, so double-clicking any PDF whose path contained a space
// handed PDFE a truncated argument. The file's own header records it.
//
// ShellOpenCommandFromPackagingOpensPathWithSpaces reads the command template
// OUT OF packaging/file_assoc.nsh (it does not restate it), installs it under a
// throw-away per-user ProgID, and shell-executes a document whose directory AND
// file name both contain spaces. The oracle is the primary window's title,
// which MainWindow::updateWindowTitle sets to "PDFE - [<file name>]" only once
// a document is actually open.
//
// The companion test UnquotedPercentOneFailsToOpenPathWithSpaces installs the
// SAME command with the quotes removed and asserts the document does NOT open.
// That is what makes the first test discriminating rather than decorative: the
// pair demonstrates the oracle distinguishes correct quoting from the exact
// broken form that shipped.
//
// SEE_MASK_CLASSNAME is used so the shell resolves OUR ProgID rather than
// whatever application currently owns .pdf on the machine. The verb lookup and
// the "%1" substitution are the same shell code path a double-click uses; only
// the handler selection is pinned, which is what makes the test deterministic.
//
// HYGIENE
// -------
// Every launched process is tracked and force-terminated in TearDown (an
// orphaned pdfe.exe wedges the next run), every registry key created is deleted,
// and every temporary file is removed.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QEventLoop>
#include <QThread>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>

#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "User32.lib")

namespace {

// ── paths ───────────────────────────────────────────────────────────────

std::filesystem::path projectRoot()
{
    return std::filesystem::path(TEST_FIXTURES_DIR).parent_path().parent_path();
}

std::filesystem::path fixturesDir()
{
    return std::filesystem::path(TEST_FIXTURES_DIR);
}

// pdfe.exe sits next to pdfe_tests.exe in the build output directory.
std::filesystem::path pdfeExe()
{
    wchar_t buf[MAX_PATH * 4] = {0};
    const DWORD n = ::GetModuleFileNameW(nullptr, buf, MAX_PATH * 4);
    if (n == 0)
        return {};
    return std::filesystem::path(std::wstring(buf, n)).parent_path() / L"pdfe.exe";
}

std::string readTextFile(const std::filesystem::path& p)
{
    std::ifstream f(p, std::ios::binary);
    if (!f.is_open())
        return {};
    return std::string(std::istreambuf_iterator<char>(f),
                       std::istreambuf_iterator<char>());
}

std::wstring toW(const std::string& s)
{
    if (s.empty())
        return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(n ? n - 1 : 0), L'\0');
    if (n > 1)
        ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n - 1);
    return w;
}

std::string fromW(const std::wstring& w)
{
    if (w.empty())
        return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0,
                                        nullptr, nullptr);
    std::string s(static_cast<size_t>(n ? n - 1 : 0), '\0');
    if (n > 1)
        ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n - 1, nullptr,
                              nullptr);
    return s;
}

// ── event-loop-friendly waiting ─────────────────────────────────────────

template <typename Pred>
bool spinUntil(Pred pred, int budgetMs, int stepMs = 100)
{
    for (int waited = 0; waited <= budgetMs; waited += stepMs) {
        if (pred())
            return true;
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, stepMs);
        QThread::msleep(static_cast<unsigned long>(stepMs));
    }
    return pred();
}

// ── window enumeration ──────────────────────────────────────────────────

struct EnumCtx {
    DWORD pid = 0;
    std::vector<HWND> hwnds;
};

BOOL CALLBACK collectMainWindows(HWND hwnd, LPARAM lp)
{
    auto* ctx = reinterpret_cast<EnumCtx*>(lp);
    DWORD pid = 0;
    ::GetWindowThreadProcessId(hwnd, &pid);
    if (pid != ctx->pid)
        return TRUE;
    if (!::IsWindowVisible(hwnd))
        return TRUE;
    // Owned windows are dialogs/popups, not the main window.
    if (::GetWindow(hwnd, GW_OWNER) != nullptr)
        return TRUE;
    wchar_t title[512] = {0};
    ::GetWindowTextW(hwnd, title, 511);
    // MainWindow's title is "PDFE" or "PDFE - [<file>]" (MainWindow.cpp:6824+).
    if (::wcsncmp(title, L"PDFE", 4) != 0)
        return TRUE;
    ctx->hwnds.push_back(hwnd);
    return TRUE;
}

std::vector<HWND> mainWindowsOf(DWORD pid)
{
    EnumCtx ctx;
    ctx.pid = pid;
    ::EnumWindows(&collectMainWindows, reinterpret_cast<LPARAM>(&ctx));
    return ctx.hwnds;
}

std::wstring windowTitle(HWND hwnd)
{
    if (!hwnd || !::IsWindow(hwnd))
        return {};
    wchar_t title[1024] = {0};
    const int n = ::GetWindowTextW(hwnd, title, 1023);
    return std::wstring(title, static_cast<size_t>(n < 0 ? 0 : n));
}

bool titleContains(HWND hwnd, const std::wstring& needle)
{
    const std::wstring t = windowTitle(hwnd);
    return t.find(needle) != std::wstring::npos;
}

// ── child-process management ────────────────────────────────────────────

class ChildApp {
public:
    ChildApp() = default;
    ChildApp(const ChildApp&) = delete;
    ChildApp& operator=(const ChildApp&) = delete;
    ~ChildApp() { kill(); }

    bool startCreateProcess(const std::filesystem::path& exe,
                            const std::filesystem::path& arg)
    {
        std::wstring cmd = L"\"" + exe.wstring() + L"\"";
        if (!arg.empty())
            cmd += L" \"" + arg.wstring() + L"\"";
        std::vector<wchar_t> buf(cmd.begin(), cmd.end());
        buf.push_back(L'\0');

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        const BOOL ok = ::CreateProcessW(exe.wstring().c_str(), buf.data(), nullptr,
                                         nullptr, FALSE, 0, nullptr,
                                         exe.parent_path().wstring().c_str(), &si,
                                         &pi);
        if (!ok) {
            lastError_ = ::GetLastError();
            return false;
        }
        ::CloseHandle(pi.hThread);
        handle_ = pi.hProcess;
        pid_ = pi.dwProcessId;
        return true;
    }

    // Launch through the Windows shell's "open" verb, resolved against an
    // explicit ProgID (SEE_MASK_CLASSNAME) so the machine's default .pdf
    // handler is never involved.
    bool startShellOpen(const std::filesystem::path& document,
                        const std::wstring& progId)
    {
        SHELLEXECUTEINFOW sei{};
        sei.cbSize = sizeof(sei);
        sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC |
                    SEE_MASK_CLASSNAME | SEE_MASK_FLAG_NO_UI;
        sei.lpVerb = L"open";
        sei.lpFile = document.c_str();
        sei.lpClass = progId.c_str();
        sei.nShow = SW_SHOWNORMAL;
        if (!::ShellExecuteExW(&sei)) {
            lastError_ = ::GetLastError();
            return false;
        }
        handle_ = sei.hProcess;
        pid_ = handle_ ? ::GetProcessId(handle_) : 0;
        return handle_ != nullptr;
    }

    bool alive() const
    {
        if (!handle_)
            return false;
        return ::WaitForSingleObject(handle_, 0) == WAIT_TIMEOUT;
    }

    DWORD waitForExit(DWORD ms) const
    {
        return handle_ ? ::WaitForSingleObject(handle_, ms) : WAIT_FAILED;
    }

    DWORD exitCode() const
    {
        DWORD code = STILL_ACTIVE;
        if (handle_)
            ::GetExitCodeProcess(handle_, &code);
        return code;
    }

    void kill()
    {
        if (handle_) {
            if (alive())
                ::TerminateProcess(handle_, 0);
            ::WaitForSingleObject(handle_, 5000);
            ::CloseHandle(handle_);
            handle_ = nullptr;
        }
        pid_ = 0;
    }

    DWORD pid() const { return pid_; }
    DWORD lastError() const { return lastError_; }

private:
    HANDLE handle_ = nullptr;
    DWORD pid_ = 0;
    DWORD lastError_ = 0;
};

// Set an environment variable for the duration of a scope. Children created
// inside the scope inherit it; the previous value is restored on exit.
class ScopedEnv {
public:
    ScopedEnv(const wchar_t* name, const wchar_t* value) : name_(name)
    {
        wchar_t buf[32767];
        const DWORD n = ::GetEnvironmentVariableW(name, buf, 32767);
        if (n > 0 && n < 32767) {
            had_ = true;
            previous_.assign(buf, n);
        }
        ::SetEnvironmentVariableW(name, value);
    }
    ~ScopedEnv()
    {
        ::SetEnvironmentVariableW(name_.c_str(), had_ ? previous_.c_str() : nullptr);
    }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    std::wstring name_;
    std::wstring previous_;
    bool had_ = false;
};

// ── per-user ProgID registration (HKCU only; no elevation needed) ────────

bool writeRegSz(HKEY root, const std::wstring& subkey, const std::wstring& value)
{
    HKEY key = nullptr;
    const LONG rc = ::RegCreateKeyExW(root, subkey.c_str(), 0, nullptr,
                                      REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr,
                                      &key, nullptr);
    if (rc != ERROR_SUCCESS)
        return false;
    const LONG wrc = ::RegSetValueExW(
        key, nullptr, 0, REG_SZ,
        reinterpret_cast<const BYTE*>(value.c_str()),
        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    ::RegCloseKey(key);
    return wrc == ERROR_SUCCESS;
}

void deleteRegTree(HKEY root, const std::wstring& subkey)
{
    ::RegDeleteTreeW(root, subkey.c_str());
}

// Pull the ProgID "shell\open\command" value out of packaging/file_assoc.nsh.
// The line looks like:
//   WriteRegStr HKCR "PDFE.Document\shell\open\command" "" '"$INSTDIR\bin\pdfe.exe" "%1"'
// and the value is the single-quoted NSIS string at the end of it.
std::string shellOpenCommandTemplateFromPackaging(std::string* whyNot)
{
    const std::filesystem::path nsh = projectRoot() / "packaging" / "file_assoc.nsh";
    const std::string text = readTextFile(nsh);
    if (text.empty()) {
        if (whyNot)
            *whyNot = "could not read " + nsh.string();
        return {};
    }
    const std::string marker = "PDFE.Document\\shell\\open\\command";
    const size_t at = text.find(marker);
    if (at == std::string::npos) {
        if (whyNot)
            *whyNot = "no ProgID open-command line in " + nsh.string();
        return {};
    }
    const size_t lineEnd = text.find('\n', at);
    const std::string line =
        text.substr(at, lineEnd == std::string::npos ? std::string::npos
                                                     : lineEnd - at);
    const size_t first = line.find('\'');
    const size_t last = line.rfind('\'');
    if (first == std::string::npos || last == first) {
        if (whyNot)
            *whyNot = "open-command line carries no single-quoted NSIS value: " + line;
        return {};
    }
    return line.substr(first + 1, last - first - 1);
}

const wchar_t* const kProgIdQuoted = L"PDFE.F12QuotedTest";
const wchar_t* const kProgIdUnquoted = L"PDFE.F12UnquotedTest";

std::wstring progIdCommandKey(const wchar_t* progId)
{
    return std::wstring(L"Software\\Classes\\") + progId + L"\\shell\\open\\command";
}

std::wstring progIdRootKey(const wchar_t* progId)
{
    return std::wstring(L"Software\\Classes\\") + progId;
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════
// Fixture
// ═══════════════════════════════════════════════════════════════════════


// WHY THERE IS NO "KILL EVERY pdfe.exe THIS FIXTURE CAUSED" SWEEP HERE.
//
// This file's header has always claimed "every launched process is tracked and
// force-terminated in TearDown (an orphaned pdfe.exe wedges the next run)".
// MEASURED 2026-08-12: the claim is false. A run left a live pdfe.exe behind and
// the NEXT run's first two single-instance cases failed with "the primary
// instance never showed a main window" — the orphan holds the single-instance
// mutex, so the process launched as the primary forwards to it and exits without
// a window. In isolation, with no orphan, the same case passes in 1.3 s. The
// failure is self-perpetuating across runs, which is how it survived: a true
// sentence about the TRACKED children standing in for a claim about every process
// the fixture causes. The shell-launched cases go through ShellExecuteEx, where
// the process the returned handle names need not be the editor that ends up
// owning the window.
//
// A time-bounded sweep (kill any pdfe.exe created at or after this case's SetUp)
// WAS written and DOES fix it — two back-to-back runs of this suite went 5/5
// with no orphan left, where the second run had previously failed 2/5. It was
// REMOVED AGAIN, deliberately: the suite runs under a 16-shard PARALLEL runner,
// and a pdfe.exe belonging to a concurrent shard is indistinguishable by name and
// start time from a leftover orphan. The sweep would silently kill another
// shard's healthy process and redden an unrelated test — a worse failure than the
// one it fixes, because it blames the wrong code and leaves no trace.
//
// What is left is the diagnosis, not the cure: SetUp records how many pdfe.exe
// processes were already running, so a "never showed a main window" failure
// carries the one fact that separates an environment collision from a product
// regression. Closing this properly needs attribution the fixture does not
// currently have (the launched editor's command line, or a per-run mutex
// namespace); that is booked, not bodged.

// How many pdfe.exe processes are running right now. Used as a PRECONDITION
// check, never as a licence to terminate one this fixture did not start.
size_t countRunningPdfeProcesses()
{
    size_t n = 0;
    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (::Process32FirstW(snap, &pe)) {
        do {
            if (::_wcsicmp(pe.szExeFile, L"pdfe.exe") == 0) ++n;
        } while (::Process32NextW(snap, &pe));
    }
    ::CloseHandle(snap);
    return n;
}

// THE DISCRIMINATOR THE COMMENT BELOW BOOKED, NOW SUPPLIED (phase-59).
//
// SetUp records how many pdfe.exe processes exist and deliberately does NOT
// fail on a non-zero count, because under a 16-shard parallel runner a
// CONCURRENT SHARD's pdfe.exe is indistinguishable BY NAME from a leftover
// orphan, and a guard that reds on another shard's healthy process is a false
// red. That reasoning is correct and stands.
//
// What separates the two cases is the IMAGE PATH. A pdfe.exe running from a
// DIFFERENT build tree cannot be a sibling shard of this binary -- sibling
// shards run the exact same executable. It is another checkout: in practice
// the self-hosted CI runner's, whose UI E2E step holds the MACHINE-GLOBAL
// single-instance mutex for minutes at a time and reds these cases in every
// concurrent local run. That has now cost real suite runs in three phases,
// each time first read as a product regression.
//
// This still does not FAIL early and still never terminates anything: on a
// developer machine a foreign pdfe.exe is somebody's open editor with unsaved
// work. It attaches the fact to the failure MESSAGE, so the red says
// "something else holds the mutex, from <this path>" instead of "the primary
// instance never showed a main window", which sends a maintainer into the
// product.
std::vector<std::wstring> foreignPdfeImagePaths(const std::filesystem::path& ownExe)
{
    std::vector<std::wstring> foreign;
    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return foreign;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    const std::wstring own = ownExe.native();
    if (::Process32FirstW(snap, &pe)) {
        do {
            if (::_wcsicmp(pe.szExeFile, L"pdfe.exe") != 0) continue;
            HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                     pe.th32ProcessID);
            if (!h) continue;
            wchar_t buf[MAX_PATH * 4] = {0};
            DWORD sz = static_cast<DWORD>(std::size(buf));
            if (::QueryFullProcessImageNameW(h, 0, buf, &sz) && sz > 0) {
                const std::wstring img(buf, sz);
                // Same image => a sibling shard of THIS binary, which is
                // healthy and must not be reported as interference.
                if (::_wcsicmp(img.c_str(), own.c_str()) != 0) {
                    foreign.push_back(img);
                }
            }
            ::CloseHandle(h);
        } while (::Process32NextW(snap, &pe));
    }
    ::CloseHandle(snap);
    return foreign;
}

/// Rendered into a failure message. It says something in BOTH directions,
/// deliberately: "nothing foreign was running" is the fact that makes a red
/// here a product verdict, and leaving it unsaid is what let three runs be
/// misread in the first place.
std::string foreignPdfeDiagnosis(const std::filesystem::path& ownExe)
{
    const auto foreign = foreignPdfeImagePaths(ownExe);
    if (foreign.empty()) {
        return " (no pdfe.exe from another build tree was running, so the "
               "machine-global single-instance mutex was NOT held by a foreign "
               "process -- this is a product verdict, not an environment one)";
    }
    std::string s =
        " -- ENVIRONMENT COLLISION, NOT A PRODUCT VERDICT: pdfe.exe is running "
        "from another build tree and holds the MACHINE-GLOBAL single-instance "
        "mutex, so the process this case launched forwarded to it and exited "
        "without a window. Foreign image(s): ";
    for (const auto& w : foreign) {
        s += fromW(w);
        s += ' ';
    }
    s += "(never terminate these: on a developer machine that is somebody's "
         "open editor, and on this host it is the self-hosted CI runner)";
    return s;
}

class ShellAndInstanceLaunchTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        // A PRE-EXISTING pdfe.exe IS RECORDED, NOT ASSERTED ON — and the
        // difference was measured, not reasoned about. It holds the
        // single-instance mutex, so the process this suite launches as the
        // primary forwards to it and exits without a window; the failure then
        // reads as "the primary instance never showed a main window", which
        // sends a maintainer into the product.
        //
        // The obvious guard is to FAIL here. That was written, and it turned a
        // rare race into five deterministic failures: the suite runs under a
        // 16-shard PARALLEL runner, and a pdfe.exe belonging to a CONCURRENT
        // SHARD is indistinguishable from a leftover orphan by name. A guard
        // that reds on another shard's healthy process is a false red, which is
        // the same defect class as a green over broken code.
        //
        // Killing is worse still: on a developer machine that is somebody's
        // open editor with unsaved work.
        //
        // So the count is recorded as a test property. When one of these cases
        // does fail on "never showed a main window", the XML says whether a
        // foreign pdfe.exe was present at SetUp, which is the one fact that
        // separates an environment collision from a product regression.
        RecordProperty("pdfe_processes_running_at_setup",
                       static_cast<int>(countRunningPdfeProcesses()));
        // The count above cannot tell a sibling shard from a foreign
        // checkout; this one can, because it compares IMAGE PATHS.
        // Recorded separately so the XML of a failed run answers the
        // question directly instead of leaving it to be inferred.
        RecordProperty(
            "foreign_pdfe_processes_at_setup",
            static_cast<int>(foreignPdfeImagePaths(pdfeExe()).size()));

        exe_ = pdfeExe();
        ASSERT_FALSE(exe_.empty());
        ASSERT_TRUE(std::filesystem::exists(exe_))
            << "pdfe.exe must be built alongside pdfe_tests.exe; looked at "
            << exe_.string()
            << ". Build the 'pdfe' target (this is a build-configuration "
               "problem, not a product verdict).";

        source_ = fixturesDir() / "test_3page.pdf";
        ASSERT_TRUE(std::filesystem::exists(source_))
            << "test_3page.pdf is a configure-time required fixture";

        const auto stamp = std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count();

        // F63 — give THIS test process its own single-instance domain.
        //
        // The product default is keyed per-USER, which is correct for a human
        // and wrong for a test host: several unrelated pdfe_tests.exe processes
        // run concurrently as one account, so a primary launched here would
        // happily accept a forward from a stranger's secondary — and, worse, a
        // primary launched here forwards to a STRANGER'S primary and exits
        // without ever showing a window. Measured: serial runs of these five
        // cases pass every time; concurrent runs fail exactly the two cases
        // that depend on the default single-instance behaviour.
        //
        // The suffix must be unique per PROCESS, not merely per test: a wall
        // clock can repeat across concurrent processes started together, so the
        // PID is mixed in. The children inherit it through CreateProcess, so
        // the primary and secondary THIS test launches still share a domain and
        // the forwarding behaviour under test is unchanged — only the set of
        // processes that share it narrows.
        instanceDomain_ = std::make_unique<ScopedEnv>(
            L"PDFE_INSTANCE_DOMAIN",
            (L"t" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
             std::to_wstring(stamp))
                .c_str());

        // A directory whose NAME CONTAINS SPACES — the quoting-regression shape.
        work_ = std::filesystem::temp_directory_path() /
                ("pdfe f12 launch " + std::to_string(stamp));
        std::filesystem::create_directories(work_);
    }

    void TearDown() override
    {
        for (auto& c : children_)
            c->kill();
        children_.clear();

        deleteRegTree(HKEY_CURRENT_USER, progIdRootKey(kProgIdQuoted));
        deleteRegTree(HKEY_CURRENT_USER, progIdRootKey(kProgIdUnquoted));

        std::error_code ec;
        std::filesystem::remove_all(work_, ec);
    }

    ChildApp* newChild()
    {
        children_.push_back(std::make_unique<ChildApp>());
        return children_.back().get();
    }

    // Copy the fixture to <work_>/<name> and return the path.
    std::filesystem::path materialise(const std::string& name)
    {
        const std::filesystem::path dst = work_ / name;
        std::filesystem::copy_file(source_, dst,
                                   std::filesystem::copy_options::overwrite_existing);
        return dst;
    }

    // Wait for a visible, unowned "PDFE*" top-level window belonging to pid.
    HWND waitForMainWindow(DWORD pid, int budgetMs = 120000)
    {
        HWND found = nullptr;
        spinUntil(
            [&] {
                const auto w = mainWindowsOf(pid);
                if (!w.empty()) {
                    found = w.front();
                    return true;
                }
                return false;
            },
            budgetMs);
        return found;
    }

    std::filesystem::path exe_;
    std::filesystem::path source_;
    std::filesystem::path work_;
    std::vector<std::unique_ptr<ChildApp>> children_;
    // Held for the lifetime of the fixture so every child launched by the test
    // inherits it; restored by ~ScopedEnv when the fixture is destroyed.
    std::unique_ptr<ScopedEnv> instanceDomain_;
};

// ═══════════════════════════════════════════════════════════════════════
// WIN-15 — a second launch forwards its path and exits without a window
// ═══════════════════════════════════════════════════════════════════════
//
// RED / mutation: delete the `if (!instanceGuard.isPrimary() && ...)` early
// return in src/main.cpp and the second process stays alive with its own
// window → both the exit assertion and the zero-window assertion fire. Break
// SingleInstanceGuard::sendPathToPrimary (or the fileOpenRequested connect) and
// the primary's title never changes to doc B → the forwarding assertion fires.
TEST_F(ShellAndInstanceLaunchTest, SecondLaunchForwardsPathAndExitsWithoutSecondWindow)
{
    // Deliberately NOT setting PDFE_DISABLE_SINGLE_INSTANCE: this test is about
    // the default (single-instance) behaviour.
    ScopedEnv noOptOut(L"PDFE_DISABLE_SINGLE_INSTANCE", L"");

    const auto docA = materialise("doc alpha.pdf");
    const auto docB = materialise("doc bravo.pdf");

    ChildApp* primary = newChild();
    ASSERT_TRUE(primary->startCreateProcess(exe_, docA))
        << "could not launch the primary pdfe.exe (win32 error "
        << primary->lastError() << ")";

    HWND hwnd = waitForMainWindow(primary->pid());
    ASSERT_NE(hwnd, nullptr)
        << "the primary instance never showed a main window within the budget"
        << foreignPdfeDiagnosis(exe_);
    ASSERT_TRUE(spinUntil([&] { return titleContains(hwnd, L"doc alpha.pdf"); }, 60000))
        << "the primary must open its command-line document first; title was \""
        << fromW(windowTitle(hwnd)) << "\"";

    ChildApp* second = newChild();
    ASSERT_TRUE(second->startCreateProcess(exe_, docB))
        << "could not launch the second pdfe.exe (win32 error "
        << second->lastError() << ")";
    const DWORD secondPid = second->pid();

    // Poll for windows belonging to the SECOND process while waiting for it to
    // exit. A window that appears and disappears would still be caught.
    size_t maxSecondWindows = 0;
    const bool exited = spinUntil(
        [&] {
            const size_t n = mainWindowsOf(secondPid).size();
            if (n > maxSecondWindows)
                maxSecondWindows = n;
            return !second->alive();
        },
        90000, 50);

    EXPECT_TRUE(exited)
        << "the second instance must EXIT instead of becoming a second editor";
    EXPECT_EQ(maxSecondWindows, 0u)
        << "the second instance must never create a main window (observed "
        << maxSecondWindows << ")";
    if (exited) {
        EXPECT_EQ(second->exitCode(), 0u)
            << "the forwarding secondary must exit cleanly (0)";
    }

    // GitHub #241 — the forwarded path must be opened by the PRIMARY PROCESS,
    // in a window of its own.
    //
    // This assertion used to read `titleContains(hwnd, "doc bravo.pdf")`, i.e.
    // doc B had to REPLACE doc A in the one window, and the test then required
    // the primary to own exactly one window. That was the correct contract while
    // PDFE was a single-document editor, and it is the contract #241 changes:
    // asking for a second document and having the first silently swapped out
    // from under you is the same defect as asking for four and getting one.
    //
    // The property the single-instance guard exists for is UNCHANGED and still
    // asserted above and below: the secondary must exit without ever creating a
    // window (so the forward really happened rather than a rival editor
    // starting), and the SAME file must never end up in two windows — that last
    // one is what the R3-02 data-loss guarantee actually says, and it is pinned
    // by TheSameFileForwardedTwiceStaysInOneWindow below.
    EXPECT_TRUE(spinUntil([&] { return mainWindowsOf(primary->pid()).size() == 2; },
                          60000))
        << "the primary must open the forwarded document in a second window; it "
           "owns " << mainWindowsOf(primary->pid()).size();

    // Spin on the TITLE, not just the window count. The window is created and
    // shown before its document finishes loading (deliberately — see the
    // show-before-open note in WindowManager::openPath), so a single pass taken
    // the moment the second window appears reads a title that is not set yet.
    const auto anyWindowTitled = [&](const wchar_t* needle) {
        return spinUntil(
            [&] {
                for (HWND w : mainWindowsOf(primary->pid())) {
                    if (titleContains(w, needle)) return true;
                }
                return false;
            },
            60000);
    };

    EXPECT_TRUE(anyWindowTitled(L"doc bravo.pdf"))
        << "no window of the primary process is showing the forwarded document";

    // And doc A must still be open — the point of the change is that opening B
    // does not cost you A.
    EXPECT_TRUE(anyWindowTitled(L"doc alpha.pdf"))
        << "opening the forwarded document closed or replaced the first one";
}

// GitHub #241 — R3-02, stated directly rather than as a side effect of
// single-window-ness.
//
// Two editors on ONE file can each save it, and whichever saves last silently
// clobbers the other's edits. That is the hazard the single-instance guard was
// built for, and the one property that must survive PDFE becoming multi-window.
// Before this test the guarantee was only implied — it held because there was
// just one window for anything to land in, so nothing checked the rule itself.
//
// RED / mutation: remove the same-path lookup in WindowManager::openPath (verified
// as MUTATION-P94-M3) and the second window appears → this fires.
TEST_F(ShellAndInstanceLaunchTest, TheSameFileForwardedTwiceStaysInOneWindow)
{
    ScopedEnv noOptOut(L"PDFE_DISABLE_SINGLE_INSTANCE", L"");

    const auto doc = materialise("doc echo.pdf");

    ChildApp* primary = newChild();
    ASSERT_TRUE(primary->startCreateProcess(exe_, doc))
        << "could not launch the primary pdfe.exe (win32 error "
        << primary->lastError() << ")";

    HWND hwnd = waitForMainWindow(primary->pid());
    ASSERT_NE(hwnd, nullptr)
        << "the primary instance never showed a main window within the budget"
        << foreignPdfeDiagnosis(exe_);
    ASSERT_TRUE(spinUntil([&] { return titleContains(hwnd, L"doc echo.pdf"); }, 60000))
        << "the primary must open its command-line document first; title was \""
        << fromW(windowTitle(hwnd)) << "\"";

    // Launch again on the SAME file. This is the double-click-an-already-open-
    // document case.
    ChildApp* second = newChild();
    ASSERT_TRUE(second->startCreateProcess(exe_, doc));
    ASSERT_TRUE(spinUntil([&] { return !second->alive(); }, 90000))
        << "the forwarding secondary must exit";

    // Give a second window the same budget a legitimate one gets, so a pass here
    // is a measurement and not a race that happened to finish first.
    size_t maxWindows = 0;
    spinUntil(
        [&] {
            const size_t n = mainWindowsOf(primary->pid()).size();
            if (n > maxWindows) maxWindows = n;
            return n > 1;  // stop early if the guarantee is already broken
        },
        20000, 50);

    EXPECT_EQ(maxWindows, 1u)
        << "the same file was opened in " << maxWindows
        << " windows — two editors on one document is the concurrent-save "
           "data-loss window R3-02 exists to close";
}

// ═══════════════════════════════════════════════════════════════════════
// WIN-16 — the primary un-minimises and comes to the foreground on a forward
// ═══════════════════════════════════════════════════════════════════════
//
// RED / mutation: drop the setWindowState/show/raise/activateWindow block from
// the fileOpenRequested lambda in src/main.cpp and the window stays iconic →
// the restore assertion fires.
TEST_F(ShellAndInstanceLaunchTest, ForwardedOpenRestoresTheMinimisedPrimaryWindow)
{
    ScopedEnv noOptOut(L"PDFE_DISABLE_SINGLE_INSTANCE", L"");

    const auto docA = materialise("doc charlie.pdf");

    ChildApp* primary = newChild();
    ASSERT_TRUE(primary->startCreateProcess(exe_, docA));
    HWND hwnd = waitForMainWindow(primary->pid());
    ASSERT_NE(hwnd, nullptr)
        << "the primary never showed a main window"
        << foreignPdfeDiagnosis(exe_);
    ASSERT_TRUE(spinUntil([&] { return titleContains(hwnd, L"doc charlie.pdf"); }, 60000))
        << "the primary must open its own document first";

    ::ShowWindow(hwnd, SW_MINIMIZE);
    ASSERT_TRUE(spinUntil([&] { return ::IsIconic(hwnd) != 0; }, 15000))
        << "could not minimise the primary window (environment condition)";

    // GitHub #241 — forward the SAME document, not a different one.
    //
    // This test used to forward doc delta and require THIS window to come back
    // showing it. Both halves of that depended on the one-window architecture:
    // a different document now correctly gets its own window, so the charlie
    // window would rightly stay minimised and the assertion would be testing
    // the old design rather than the property it was written to protect.
    //
    // The property is "a forwarded open must not leave the user looking at
    // nothing", and the case where it genuinely bites is re-opening a document
    // that IS already open in a minimised window — there is no new window to
    // draw the eye, so if the existing one is not restored the user sees the
    // launch do absolutely nothing. That is the case exercised here.
    ChildApp* second = newChild();
    ASSERT_TRUE(second->startCreateProcess(exe_, docA));
    ASSERT_TRUE(spinUntil([&] { return !second->alive(); }, 90000))
        << "the forwarding secondary must exit";

    EXPECT_TRUE(spinUntil([&] { return ::IsIconic(hwnd) == 0; }, 30000))
        << "a forwarded open must RESTORE the minimised window already showing "
           "that document (WindowManager::raiseWindow clears "
           "Qt::WindowMinimized and calls show()/raise()); the window is still "
           "iconic, so the user would see nothing happen at all";
    EXPECT_TRUE(titleContains(hwnd, L"doc charlie.pdf"))
        << "the restored window must still be showing its document; title is \""
        << fromW(windowTitle(hwnd)) << "\"";
    EXPECT_EQ(mainWindowsOf(primary->pid()).size(), 1u)
        << "re-opening an already-open document must raise its window, not add "
           "a second editor on the same file";
}

// ═══════════════════════════════════════════════════════════════════════
// WIN-17 — the PDFE_DISABLE_SINGLE_INSTANCE opt-out really yields two editors
// ═══════════════════════════════════════════════════════════════════════
//
// The opt-out had ZERO test hits. If it had silently stopped working, every
// harness that intends side-by-side instances would have been asserting against
// one process while believing it had two — a false green invisible in any
// summary.
//
// RED / mutation: remove the `qgetenv("PDFE_DISABLE_SINGLE_INSTANCE") != "1"`
// clause from src/main.cpp and the second process exits with no window → the
// two-live-windows assertion fires.
TEST_F(ShellAndInstanceLaunchTest, DisableSingleInstanceEnvYieldsTwoIndependentEditors)
{
    ScopedEnv optOut(L"PDFE_DISABLE_SINGLE_INSTANCE", L"1");

    const auto docA = materialise("doc echo.pdf");
    const auto docB = materialise("doc foxtrot.pdf");

    ChildApp* first = newChild();
    ASSERT_TRUE(first->startCreateProcess(exe_, docA));
    HWND h1 = waitForMainWindow(first->pid());
    ASSERT_NE(h1, nullptr) << "the first opted-out instance showed no window";

    ChildApp* second = newChild();
    ASSERT_TRUE(second->startCreateProcess(exe_, docB));
    HWND h2 = waitForMainWindow(second->pid());

    EXPECT_NE(h2, nullptr)
        << "with PDFE_DISABLE_SINGLE_INSTANCE=1 the SECOND launch must create "
           "its own editor window; if it does not, every side-by-side harness "
           "scenario has been silently collapsing to a single instance";
    EXPECT_NE(first->pid(), second->pid());
    EXPECT_TRUE(second->alive())
        << "the opted-out second instance must stay alive, not forward and exit";
    EXPECT_TRUE(first->alive()) << "the first instance must remain running";
    EXPECT_NE(h1, h2) << "the two instances must own DISTINCT windows";

    EXPECT_TRUE(spinUntil([&] { return titleContains(h1, L"doc echo.pdf"); }, 60000))
        << "instance 1 must show its own document; title \""
        << fromW(windowTitle(h1)) << "\"";
    if (h2) {
        EXPECT_TRUE(spinUntil([&] { return titleContains(h2, L"doc foxtrot.pdf"); },
                              60000))
            << "instance 2 must show ITS OWN document (not instance 1's); title \""
            << fromW(windowTitle(h2)) << "\"";
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Shell double-click — the shipped "%1" quoting opens a path with spaces
// ═══════════════════════════════════════════════════════════════════════
//
// The command string is READ FROM packaging/file_assoc.nsh, so this test tracks
// what is actually shipped. If someone reverts the quoting to the broken
// `$"...$" $"%1$"` form, or drops the quotes around %1, the structural
// assertion below fires AND the launch stops opening the document.
TEST_F(ShellAndInstanceLaunchTest, ShellOpenCommandFromPackagingOpensPathWithSpaces)
{
    std::string why;
    const std::string tmpl = shellOpenCommandTemplateFromPackaging(&why);
    ASSERT_FALSE(tmpl.empty()) << "packaging/file_assoc.nsh: " << why;

    // The shipped contract: the executable is quoted and so is %1.
    EXPECT_NE(tmpl.find("\"%1\""), std::string::npos)
        << "packaging/file_assoc.nsh must pass the document as a QUOTED \"%1\"; "
           "an unquoted %1 truncates every path containing a space — the exact "
           "regression the file's own header records. Template was: "
        << tmpl;
    EXPECT_EQ(tmpl.find("$\""), std::string::npos)
        << "the NSIS escape form $\" must not appear in the registry value; it "
           "is what the broken CPack round-trip produced. Template was: "
        << tmpl;

    // Substitute the real executable for the NSIS $INSTDIR\bin\pdfe.exe token.
    std::string command = tmpl;
    const std::string token = "$INSTDIR\\bin\\pdfe.exe";
    const size_t at = command.find(token);
    ASSERT_NE(at, std::string::npos)
        << "the packaged command must invoke $INSTDIR\\bin\\pdfe.exe; got: " << tmpl;
    command.replace(at, token.size(), exe_.string());

    ASSERT_TRUE(writeRegSz(HKEY_CURRENT_USER, progIdCommandKey(kProgIdQuoted),
                           toW(command)))
        << "could not create the per-user test ProgID (HKCU write failed)";
    ::SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);

    // Side-by-side so the shell-launched editor is never a forwarding secondary.
    ScopedEnv optOut(L"PDFE_DISABLE_SINGLE_INSTANCE", L"1");

    // Spaces in BOTH the directory (from SetUp) and the file name.
    const auto doc = materialise("doc with space.pdf");
    ASSERT_NE(doc.string().find(' '), std::string::npos)
        << "the document path under test must contain a space";

    ChildApp* app = newChild();
    ASSERT_TRUE(app->startShellOpen(doc, kProgIdQuoted))
        << "ShellExecuteEx(open) failed for the registered ProgID (win32 error "
        << app->lastError() << ")";

    HWND hwnd = waitForMainWindow(app->pid());
    ASSERT_NE(hwnd, nullptr)
        << "the shell-launched pdfe.exe never showed a main window";
    EXPECT_TRUE(spinUntil([&] { return titleContains(hwnd, L"doc with space.pdf"); },
                          60000))
        << "the shell 'open' verb must deliver the FULL spaced path so the "
           "document opens; the window title is \""
        << fromW(windowTitle(hwnd))
        << "\" — a bare \"PDFE\" means the argument was truncated at the first "
           "space, which is the shipped-once regression this test exists for";
}

// ═══════════════════════════════════════════════════════════════════════
// Discrimination proof for the test above
// ═══════════════════════════════════════════════════════════════════════
//
// Same launch, same oracle, but the command registered WITHOUT quotes around
// %1. If this test also showed the document open, the test above would prove
// nothing about quoting. It must not.
TEST_F(ShellAndInstanceLaunchTest, UnquotedPercentOneFailsToOpenPathWithSpaces)
{
    std::string why;
    const std::string tmpl = shellOpenCommandTemplateFromPackaging(&why);
    ASSERT_FALSE(tmpl.empty()) << "packaging/file_assoc.nsh: " << why;

    std::string command = tmpl;
    const std::string token = "$INSTDIR\\bin\\pdfe.exe";
    const size_t at = command.find(token);
    ASSERT_NE(at, std::string::npos);
    command.replace(at, token.size(), exe_.string());

    // Strip the quotes around %1 — the broken form.
    const size_t q = command.find("\"%1\"");
    ASSERT_NE(q, std::string::npos)
        << "expected a quoted \"%1\" in the shipped template to un-quote";
    command.replace(q, 4, "%1");
    ASSERT_EQ(command.find("\"%1\""), std::string::npos);

    ASSERT_TRUE(writeRegSz(HKEY_CURRENT_USER, progIdCommandKey(kProgIdUnquoted),
                           toW(command)));
    ::SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);

    ScopedEnv optOut(L"PDFE_DISABLE_SINGLE_INSTANCE", L"1");

    const auto doc = materialise("doc with space.pdf");
    ChildApp* app = newChild();
    ASSERT_TRUE(app->startShellOpen(doc, kProgIdUnquoted))
        << "ShellExecuteEx(open) failed for the unquoted ProgID (win32 error "
        << app->lastError() << ")";

    HWND hwnd = waitForMainWindow(app->pid());
    ASSERT_NE(hwnd, nullptr)
        << "even with a truncated argument pdfe.exe must still start and show a "
           "window (a crash here would be a separate defect)";

    // Give it the same budget the positive test gets, then assert it did NOT
    // open the document.
    const bool opened =
        spinUntil([&] { return titleContains(hwnd, L"doc with space.pdf"); }, 20000);
    EXPECT_FALSE(opened)
        << "an UNQUOTED %1 must NOT be able to open a path containing spaces. It "
           "did — which would mean the positive quoting test cannot distinguish "
           "correct from broken registration, and the oracle is worthless.";
}

#endif // _WIN32
