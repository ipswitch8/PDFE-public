// -*- coding: utf-8 -*-
#include "PreflightOpen.h"

#include <atomic>
#include <string>
#include <system_error>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <sddl.h>      // ConvertStringSidToSidW -- GitHub #59
#else
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <unistd.h>
#  include <signal.h>
#  include <ctime>
#  if defined(__APPLE__)
#    include <mach-o/dyld.h>  // _NSGetExecutablePath
#    include <climits>        // PATH_MAX
#    include <cstdint>        // uint32_t
#    include <cstdlib>        // realpath
#  endif
#endif

#include <optional>

namespace pdfe::core {

namespace {

// Test seam (see detail::setExecutableDirForTesting). When engaged, this
// value is returned verbatim by executableDir() — an ENGAGED-but-EMPTY path
// deliberately simulates an unresolvable executable location so the
// fail-closed branch can be exercised on any host.
std::optional<std::filesystem::path>& executableDirOverride()
{
    static std::optional<std::filesystem::path> o;
    return o;
}

// Directory containing the running executable (helper lives beside it).
std::filesystem::path executableDir()
{
    if (executableDirOverride().has_value())
        return *executableDirOverride();

#if defined(_WIN32)
    wchar_t buf[MAX_PATH];
    DWORD n = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return {};
    return std::filesystem::path(std::wstring(buf, n)).parent_path();
#elif defined(__APPLE__)
    // macOS has no /proc. _NSGetExecutablePath yields the path used to launch
    // the image; it may be relative or contain symlinks, so realpath()
    // canonicalizes it to an absolute, symlink-free path. First call with a
    // zero-sized buffer to learn the required length (it returns -1 and sets
    // `size` to include the terminating NUL), then call again to fill it.
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    if (size == 0)
        return {};
    std::string raw(size, '\0');
    if (_NSGetExecutablePath(raw.data(), &size) != 0)
        return {}; // path grew between the two calls — treat as unresolved
    // Drop the trailing NUL the API writes so the string length is correct.
    if (!raw.empty() && raw.back() == '\0')
        raw.pop_back();
    char resolved[PATH_MAX];
    if (::realpath(raw.c_str(), resolved) != nullptr)
        return std::filesystem::path(resolved).parent_path();
    // realpath failed (e.g. a broken symlink component) — the raw path is
    // still a better answer than empty (empty would fail the open closed).
    return std::filesystem::path(raw).parent_path();
#else
    // Linux (and other /proc-providing systems): the running executable is
    // the /proc/self/exe symlink target.
    std::error_code ec;
    auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec)
        return {};
    return p.parent_path();
#endif
}

} // namespace

namespace detail {

void setExecutableDirForTesting(std::optional<std::filesystem::path> dir)
{
    executableDirOverride() = std::move(dir);
}

} // namespace detail

namespace {
// GitHub #59 (SEC-61). Reported through lastHelperIsolation(); see the header
// for why a silent fallback is unacceptable.
std::atomic<HelperIsolation> g_lastIsolation{HelperIsolation::NotLaunched};
}  // namespace

HelperIsolation lastHelperIsolation()
{
    return g_lastIsolation.load(std::memory_order_acquire);
}

#if defined(_WIN32)
namespace {

/// Duplicate the current process token and drop it to LOW integrity.
///
/// Returns a primary token suitable for CreateProcessAsUserW, or nullptr.
/// The caller owns the handle.
///
/// Deriving from our OWN token rather than building one is what keeps this
/// privilege-free: lowering integrity on a copy of the caller's token needs no
/// SeAssignPrimaryToken / SeIncreaseQuota, whereas constructing a token for a
/// different principal would.
HANDLE makeLowIntegrityToken()
{
    HANDLE self = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(),
                            TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY |
                                TOKEN_QUERY | TOKEN_ADJUST_DEFAULT,
                            &self)) {
        return nullptr;
    }

    HANDLE dup = nullptr;
    const BOOL dupOk = ::DuplicateTokenEx(
        self, TOKEN_ALL_ACCESS, nullptr,
        SecurityImpersonation, TokenPrimary, &dup);
    ::CloseHandle(self);
    if (!dupOk || dup == nullptr) {
        return nullptr;
    }

    // S-1-16-4096 = Low Mandatory Level.
    PSID lowSid = nullptr;
    if (!::ConvertStringSidToSidW(L"S-1-16-4096", &lowSid)) {
        ::CloseHandle(dup);
        return nullptr;
    }

    TOKEN_MANDATORY_LABEL til{};
    til.Label.Attributes = SE_GROUP_INTEGRITY;
    til.Label.Sid        = lowSid;

    const BOOL setOk = ::SetTokenInformation(
        dup, TokenIntegrityLevel, &til,
        static_cast<DWORD>(sizeof(TOKEN_MANDATORY_LABEL) +
                           ::GetLengthSid(lowSid)));
    ::LocalFree(lowSid);

    if (!setOk) {
        ::CloseHandle(dup);
        return nullptr;
    }
    return dup;
}

}  // namespace
#endif  // _WIN32

bool isDocumentOpenSafe(const std::filesystem::path& file, int timeoutMs)
{
    std::filesystem::path dir = executableDir();
    if (dir.empty())
        return false; // can't locate the helper at all — fail CLOSED (refuse).
                      // An unresolvable executable location disables the
                      // sandbox; treating documents as safe here would silently
                      // defeat the control (this is the macOS fail-open the
                      // Apple executableDir() branch closes).

#if defined(_WIN32)
    std::filesystem::path helper = dir / L"pdfe_preflight.exe";
    std::error_code ec;
    if (!std::filesystem::exists(helper, ec))
        return true; // helper not deployed — fail open

    // Command line: "helper" "file"  (quoted; CreateProcessW needs a
    // mutable buffer).
    std::wstring cmd = L"\"" + helper.wstring() + L"\" \"" + file.wstring() + L"\"";
    std::wstring mutableCmd = cmd;

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};

    // GitHub #59 (SEC-61): create the helper at LOW INTEGRITY.
    //
    // The helper exists to parse attacker-controlled bytes. Running it in a
    // separate process already contains a CRASH; it does not contain
    // CODE EXECUTION, because a medium-integrity child can still write to the
    // user's profile and registry and can open handles to the parent. A
    // low-integrity process cannot: the mandatory integrity policy denies
    // write-up, so a MuPDF parser bug is confined to objects the low-integrity
    // token can already reach.
    //
    // Fallback is deliberate and REPORTED rather than silent. If the token
    // work fails, an ordinary launch still gives crash isolation, and refusing
    // to open documents because integrity-lowering failed would be a worse
    // trade for the user. But the security property is genuinely gone in that
    // case, so lastHelperIsolation() says so and a test asserts it -- an
    // effect that cannot be observed cannot be audited.
    BOOL created = FALSE;
    HANDLE lowToken = makeLowIntegrityToken();
    if (lowToken != nullptr) {
        created = ::CreateProcessAsUserW(
            lowToken, helper.wstring().c_str(), mutableCmd.data(),
            nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        ::CloseHandle(lowToken);
        if (created) {
            g_lastIsolation.store(HelperIsolation::LowIntegrity,
                                  std::memory_order_release);
        }
    }

    if (!created) {
        created = ::CreateProcessW(helper.wstring().c_str(), mutableCmd.data(),
                                   nullptr, nullptr, FALSE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        if (created) {
            g_lastIsolation.store(HelperIsolation::Unrestricted,
                                  std::memory_order_release);
        }
    }

    if (!created)
        return false; // launch FAILED while the helper IS deployed — fail CLOSED.
                      // This is deliberately stricter than the helper-not-deployed
                      // branch above: a present-but-unlaunchable sandbox means the
                      // deployment INTENDED vetting but it could not run, so
                      // treating the document as safe would silently defeat the
                      // control on the exact document that was supposed to be
                      // vetted. Refuse instead (the caller surfaces a retry).

    bool safe = false;
    DWORD wait = ::WaitForSingleObject(pi.hProcess,
                                       timeoutMs < 0 ? INFINITE : (DWORD)timeoutMs);
    if (wait == WAIT_OBJECT_0)
    {
        DWORD code = 1;
        if (::GetExitCodeProcess(pi.hProcess, &code))
            safe = (code == 0); // ONLY a clean open (exit 0) is safe
    }
    else
    {
        // Timeout (or wait failure): the document hung the helper. Kill it
        // and treat as unsafe.
        ::TerminateProcess(pi.hProcess, 0xDEAD);
        ::WaitForSingleObject(pi.hProcess, 2000);
    }
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return safe;
#else
    std::filesystem::path helper = dir / "pdfe_preflight";
    std::error_code ec;
    if (!std::filesystem::exists(helper, ec))
        return true;

    pid_t pid = ::fork();
    if (pid < 0)
        return false; // fork FAILED while the helper IS deployed — fail CLOSED
                      // (same rationale as the Windows launch-failed branch: a
                      // present-but-unlaunchable sandbox must not silently pass
                      // the document it was meant to vet).
    if (pid == 0)
    {
        ::execl(helper.c_str(), helper.c_str(), file.c_str(), (char*)nullptr);
        _exit(127); // exec failed
    }

    // Poll for completion up to the timeout.
    const int stepMs = 25;
    int elapsed = 0;
    int status = 0;
    for (;;)
    {
        pid_t r = ::waitpid(pid, &status, WNOHANG);
        if (r == pid)
        {
            // Safe only on a normal exit with code 0 (a signal/crash is unsafe).
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        }
        if (r < 0)
            return false; // waitpid ERROR after a successful fork — fail CLOSED.
                          // The child was launched but we lost its verdict; we
                          // cannot assert the document is safe, so refuse. (The
                          // child is reaped below/on exit; a leaked zombie is far
                          // preferable to passing an unvetted document.)
        if (timeoutMs >= 0 && elapsed >= timeoutMs)
        {
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &status, 0);
            return false;
        }
        struct timespec ts{0, stepMs * 1000000L};
        ::nanosleep(&ts, nullptr);
        elapsed += stepMs;
    }
#endif
}

} // namespace pdfe::core
