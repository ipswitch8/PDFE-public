#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include "CrashHandler.h"
#include "core/Error.h"
#include <QMessageBox>
#include <exception>
#include <cstdlib>
#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#include <DbgHelp.h>   // MiniDumpWriteDump
#include <AclApi.h>    // SetNamedSecurityInfoW  (owner-only crash-dir DACL)
#include <sddl.h>      // ConvertSidToStringSidW, SDDL security descriptors
#include <string>
#else
#include "core/PathSecurity.h"
#include "core/TempFileSafety.h"
#include <QStandardPaths>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstring>
#include <ctime>
#include <execinfo.h>   // backtrace, backtrace_symbols_fd
#include <fcntl.h>
#include <filesystem>
#include <signal.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
// Linux only (GitHub #388): macOS's <ucontext.h> is an #error without
// _XOPEN_SOURCE, and XNU's SYS_gettid is a different call (per-thread
// uid/gid), so neither may reach an Apple build.
#include <sys/syscall.h>
#include <ucontext.h>
#endif
#include <utility>
#include <vector>
#endif

namespace pdfe::app {

namespace {

#ifdef _WIN32

// ---------------------------------------------------------------------------
// WHAT A CRASH DUMP CONTAINS, AND WHY THIS FILE CARES ABOUT PERMISSIONS
// (phase-11, from the 2026-08-10 reverification §7).
//
// writeMinidump() below requests MiniDumpWithDataSegs and
// MiniDumpWithIndirectlyReferencedMemory. That is not a full-heap dump — saying
// it "captures all passwords" would overstate it — but it DOES capture writable
// data segments and heap reachable from stack/register pointers. A password
// still held in a QLineEdit's QString at crash time is exactly that shape. So a
// .dmp here is potentially secret-bearing, and it was being written into a
// directory created with `CreateDirectoryW(dir, nullptr)` — null
// SECURITY_ATTRIBUTES, i.e. whatever the parent's inheritable ACL happens to
// be — at a path any environment variable could redirect, with no validation.
//
// Three things follow, all implemented below:
//   1. the directory gets an EXPLICIT, PROTECTED DACL granting only the current
//      user, applied whether we created it or found it already there;
//   2. a PDFE_CRASH_DUMP_DIR override is VALIDATED before use;
//   3. the 150 cap becomes a retention policy that PRUNES, not a brake that
//      silently drops the 151st crash — which is the one you most want.
// ---------------------------------------------------------------------------

// Cap on retained dumps. ~48 MB each measured on the dev host, so ~7.2 GB at the
// cap. Distinct crash SIGNATURES are what matter for triage and there are never
// this many; the cap keeps the set small enough to always upload.
constexpr int kMaxCrashDumps = 150;

// Is `p` acceptable as a PDFE_CRASH_DUMP_DIR override?
//
// This is a PURE PREDICATE on the string — no filesystem access — so it is
// cheap, crash-path safe, and directly testable. The rejections are not
// stylistic; each names a way secret-bearing dumps end up somewhere we cannot
// protect:
//
//   * UNC ("\\server\share") — the dump leaves the machine entirely and lands
//     under a remote ACL we do not control and cannot verify.
//   * relative — resolves against the process CWD, which varies by launcher, so
//     the dump lands somewhere unpredictable (and possibly world-readable).
//   * a ".." segment — a relative escape smuggled through an absolute-looking
//     path; normalising it is more code than refusing it.
//   * device namespace ("\\?\", "\\.\") — bypasses the normal path parser and
//     with it the assumptions the checks above rest on.
//
// A rejected override is NOT fatal: the caller falls back to the default TEMP
// path so crashes are still captured. Refusing to capture at all would trade a
// confidentiality problem for a diagnosability one.
bool crashDumpDirOverrideIsAcceptable(const std::wstring& p) {
    if (p.empty()) return false;
    if (p.size() > MAX_PATH - 64) return false;  // leave room for the filename

    // UNC and the device namespace both begin with two separators.
    const auto isSep = [](wchar_t c) { return c == L'\\' || c == L'/'; };
    if (p.size() >= 2 && isSep(p[0]) && isSep(p[1])) return false;

    // Must be a drive-absolute path: "X:\..." (a drive-RELATIVE "X:foo" is not
    // absolute — it resolves against that drive's own current directory).
    if (p.size() < 3) return false;
    const wchar_t drive = p[0];
    const bool driveOk = (drive >= L'A' && drive <= L'Z') || (drive >= L'a' && drive <= L'z');
    if (!driveOk || p[1] != L':' || !isSep(p[2])) return false;

    // Reject any ".." path segment.
    for (size_t i = 0; i + 1 < p.size(); ++i) {
        const bool atSegStart = (i == 0) || isSep(p[i - 1]);
        if (!atSegStart || p[i] != L'.' || p[i + 1] != L'.') continue;
        const size_t after = i + 2;
        if (after == p.size() || isSep(p[after])) return false;
    }
    return true;
}

// Build a PROTECTED DACL granting FULL access to the current user and nobody
// else, and apply it to `dir`. "Protected" (the SDDL "P" flag) breaks
// inheritance, so a permissive ACL on the parent — %TEMP%'s, or whatever a
// redirected path sits under — cannot widen it back out.
//
// Applied on BOTH paths (created-by-us and already-existed) deliberately. If we
// only secured directories we created, an attacker who pre-creates the
// directory with a permissive ACL keeps it, and we would write secret-bearing
// dumps into their container — the create-vs-adopt gap that makes "we set the
// ACL" untrue exactly when it matters.
//
// Returns false if the DACL could not be applied; the caller then refuses to
// write, because an unprotectable dump directory is worse than no dump.
bool applyOwnerOnlyDacl(const std::wstring& dir) {
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) return false;

    DWORD needed = 0;
    ::GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
    if (needed == 0) { ::CloseHandle(token); return false; }

    // Stack buffer: TOKEN_USER is a SID plus a pointer; 512 bytes covers any
    // real SID. Avoids a heap allocation on a path that can run during a crash.
    unsigned char tokenBuf[512];
    if (needed > sizeof(tokenBuf)) { ::CloseHandle(token); return false; }
    if (!::GetTokenInformation(token, TokenUser, tokenBuf, needed, &needed)) {
        ::CloseHandle(token);
        return false;
    }
    ::CloseHandle(token);

    auto* tu = reinterpret_cast<TOKEN_USER*>(tokenBuf);
    LPWSTR sidStr = nullptr;
    if (!::ConvertSidToStringSidW(tu->User.Sid, &sidStr) || sidStr == nullptr) return false;

    // D:P            -> DACL, protected (inheritance broken)
    // (A;OICI;FA;;;S) -> Allow, Object+Container inherit, Full Access, to S
    std::wstring sddl = L"D:P(A;OICI;FA;;;";
    sddl += sidStr;
    sddl += L")";
    ::LocalFree(sidStr);

    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(), SDDL_REVISION_1, &sd, nullptr)) {
        return false;
    }

    BOOL daclPresent = FALSE, daclDefaulted = FALSE;
    PACL dacl = nullptr;
    bool ok = false;
    if (::GetSecurityDescriptorDacl(sd, &daclPresent, &dacl, &daclDefaulted) && daclPresent) {
        // PROTECTED_DACL_SECURITY_INFORMATION is what actually severs
        // inheritance on an EXISTING directory; the "P" in the SDDL alone
        // describes the descriptor we built, not what the object ends up with.
        const DWORD rc = ::SetNamedSecurityInfoW(
            const_cast<LPWSTR>(dir.c_str()), SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
            nullptr, nullptr, dacl, nullptr);
        ok = (rc == ERROR_SUCCESS);
    }
    ::LocalFree(sd);
    return ok;
}

// Resolve the crash-dump directory: a VALIDATED PDFE_CRASH_DUMP_DIR if set,
// else %TEMP%\pdfe_crashes. Created if missing, and secured either way.
// Returns an empty wstring on failure — including when the directory exists but
// cannot be given an owner-only DACL.
std::wstring resolveCrashDumpDir() {
    wchar_t buf[MAX_PATH];
    std::wstring dir;
    DWORD n = ::GetEnvironmentVariableW(L"PDFE_CRASH_DUMP_DIR", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        std::wstring override_(buf, n);
        if (crashDumpDirOverrideIsAcceptable(override_)) {
            dir = override_;
        } else {
            std::fprintf(stderr,
                         "[CrashHandler] PDFE_CRASH_DUMP_DIR rejected (must be a "
                         "local drive-absolute path with no '..'; UNC and device "
                         "paths are refused) - falling back to TEMP\n");
            std::fflush(stderr);
        }
    }
    if (dir.empty()) {
        wchar_t tmp[MAX_PATH];
        DWORD tn = ::GetTempPathW(MAX_PATH, tmp);
        if (tn == 0 || tn >= MAX_PATH) return {};
        dir.assign(tmp, tn);
        dir += L"pdfe_crashes";
    }

    if (!::CreateDirectoryW(dir.c_str(), nullptr)) {
        if (::GetLastError() != ERROR_ALREADY_EXISTS) return {};
    }

    if (!applyOwnerOnlyDacl(dir)) {
        std::fprintf(stderr,
                     "[CrashHandler] could not apply an owner-only ACL to the crash "
                     "dump directory - refusing to write a potentially "
                     "secret-bearing dump there\n");
        std::fflush(stderr);
        return {};
    }
    return dir;
}

// Count existing *.dmp in `dir`. Stack buffers only — no heap churn on the
// crash path.
int countExistingDumps(const std::wstring& dir) {
    std::wstring pattern = dir + L"\\*.dmp";
    WIN32_FIND_DATAW fd;
    HANDLE h = ::FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int n = 0;
    do { ++n; } while (::FindNextFileW(h, &fd));
    ::FindClose(h);
    return n;
}

// Delete oldest-first until at most `keep` dumps remain. Returns how many were
// deleted.
//
// RETENTION, NOT A BRAKE — this is the behaviour change phase-11 exists for.
// The cap used to be enforced by REFUSING to write once 150 dumps existed, so
// the directory froze with the 150 oldest and every crash after that vanished.
// The newest crash is the one you are usually debugging, so the old policy
// discarded precisely the evidence worth keeping, silently.
//
// Deliberately a repeated single-oldest scan rather than a sort. Sorting 150
// entries needs either a heap allocation or ~78 KB of stack for the names, and
// this can run on an already-crashing thread whose stack is the one thing we
// must not lean on. Each pass is one FindFirstFile walk with a single reusable
// name buffer; in steady state exactly one file is removed per crash.
int pruneOldestDumps(const std::wstring& dir, int keep) {
    if (keep < 0) keep = 0;
    int deleted = 0;

    // Bounded so a delete that keeps failing (file locked by an uploader, say)
    // cannot spin forever on the crash path.
    for (int guard = 0; guard <= kMaxCrashDumps; ++guard) {
        if (countExistingDumps(dir) <= keep) break;

        const std::wstring pattern = dir + L"\\*.dmp";
        WIN32_FIND_DATAW fd;
        HANDLE h = ::FindFirstFileW(pattern.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) break;

        wchar_t oldestName[MAX_PATH];
        oldestName[0] = L'\0';
        ULARGE_INTEGER oldest;
        oldest.QuadPart = 0;
        bool haveOldest = false;

        do {
            ULARGE_INTEGER t;
            t.LowPart = fd.ftLastWriteTime.dwLowDateTime;
            t.HighPart = fd.ftLastWriteTime.dwHighDateTime;
            if (!haveOldest || t.QuadPart < oldest.QuadPart) {
                oldest = t;
                ::wcsncpy_s(oldestName, fd.cFileName, _TRUNCATE);
                haveOldest = true;
            }
        } while (::FindNextFileW(h, &fd));
        ::FindClose(h);

        if (!haveOldest) break;

        const std::wstring victim = dir + L"\\" + oldestName;
        if (!::DeleteFileW(victim.c_str())) break;  // stop rather than spin
        ++deleted;
    }
    return deleted;
}
#else  // POSIX

// ---------------------------------------------------------------------------
// THE POSIX CRASH HANDLER (Linux port, phase 6).
//
// WHAT IT DOES. install() resolves and secures the crash directory, opens an
// O_DIRECTORY descriptor on it, prunes old reports, allocates an alternate
// signal stack, primes backtrace(), pre-formats the version line, and installs
// one SA_SIGINFO|SA_ONSTACK|SA_RESETHAND handler for SIGSEGV, SIGBUS, SIGILL,
// SIGFPE, SIGABRT and SIGTRAP. On a fatal signal the handler writes a TEXT
// report (there is no minidump format here) and re-raises, so the process dies
// with the ORIGINAL signal and a parent's waitpid() sees WIFSIGNALED with it.
//
// ASYNC-SIGNAL SAFETY. The handler runs at an arbitrary instruction — possibly
// inside malloc with its arena lock held, inside stdio, inside Qt. So the
// handler uses ONLY the following, and nothing that allocates or locks:
//   * clock_gettime, getpid, write, openat, close, fsync, sigaction, raise,
//     nanosleep, _exit                        — on the POSIX async-signal-safe
//                                               list (signal-safety(7)).
//   * syscall(SYS_gettid)                     — a raw system call; glibc's
//                                               syscall() takes no lock.
//   * std::atomic<int> exchange/load          — lock-free on every target we
//                                               build (static_assert below).
//   * hand-written decimal/hex formatting into stack/static char buffers — no
//     snprintf (not on the list; glibc's may allocate for some conversions).
//   * backtrace() and backtrace_symbols_fd()  — NOT on the POSIX list. glibc
//     documents backtrace_symbols_fd as not calling malloc (it formats each
//     frame straight to the fd), unlike backtrace_symbols. backtrace()'s caveat
//     is that its FIRST call dlopen()s libgcc_s for the unwinder, which
//     allocates and takes the loader lock — so install() calls it once,
//     outside any signal, and the handler's call then finds the unwinder
//     already loaded. Residual risk, stated rather than hidden: both walk
//     loader data (dladdr / dl_iterate_phdr take the loader's recursive lock),
//     so a fault raised while THIS thread holds the loader lock (i.e. during a
//     dlopen) could deadlock in the backtrace step. Everything before the
//     backtrace — the marker, signal, si_code, fault address, pid, version —
//     is already written and on disk by then.
//
// Not used in the handler: Qt, std::string, malloc/new, stdio, locale,
// QStandardPaths, the CrashHandler singleton's QString members. The
// terminate handler (std::terminate path) is a different context — not a
// signal — and keeps the existing Qt-based behaviour; its std::abort() then
// raises SIGABRT, which this handler reports.
//
// REPORT FILE. openat(dirfd, "pdfe_crash_<sec>.<nsec>_<pid>_<signo>.crash",
// O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC|O_NOFOLLOW, 0600). The directory is 0700
// and owned by us (createOwnerOnlyDirectory), the fd was opened at install
// time so a later rename/symlink of the path cannot redirect the write, O_EXCL
// means we never write into a pre-existing file, and 0600 matches the Windows
// owner-only DACL. A report holds addresses, not memory contents, so it is
// less secret-bearing than a minidump — it is still treated as owner-only.
//
// LIMITS (also in docs/linux-parity-status.md):
//   * sigaltstack is PER THREAD. install() gives the installing (main) thread
//     an alternate stack; a stack overflow on a WORKER thread has no stack to
//     run the handler on, so the kernel kills the process with SIGSEGV and no
//     report is written. Other faults on worker threads (null deref, abort) are
//     reported — the handler itself is process-wide.
//   * SIGKILL and the OOM killer cannot be caught; nothing is written.
//   * The backtrace has symbol names only for exported symbols (the binaries
//     are not linked with -rdynamic); each frame carries module+offset and the
//     absolute address, which addr2line resolves against the build.
//   * No pruning in the handler (directory scanning is not async-signal-safe).
//     install() prunes to maxCrashDumps()-1, leaving room for this process's
//     one report.
// ---------------------------------------------------------------------------

constexpr int kMaxCrashDumps = 150;  // same cap as the Windows arm

constexpr int kHandledSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP};
constexpr int kNumHandledSignals =
    static_cast<int>(sizeof(kHandledSignals) / sizeof(kHandledSignals[0]));

constexpr const char* kReportPrefix = "pdfe_crash_";
constexpr const char* kReportSuffix = ".crash";

// Alternate stack: generous, because the libgcc unwinder behind backtrace()
// uses several KiB per call level and a stack overflow leaves the normal
// stack unusable. mmap'd, not malloc'd, so it is page-aligned and its own
// mapping.
constexpr size_t kAltStackBytes = 256 * 1024;

static_assert(std::atomic<int>::is_always_lock_free,
              "the signal handler relies on lock-free std::atomic<int>");

// ---- state shared with the handler (written only outside signal context) --
int g_crashDirFd = -1;
void* g_altStackMem = nullptr;
std::atomic<int> g_handlerTid{0};    // tid currently inside the handler, 0 = none
char g_versionLine[256];
size_t g_versionLen = 0;
struct sigaction g_previous[kNumHandledSignals];
bool g_signalsInstalled = false;
std::string g_crashDirPath;          // read only outside signal context

// ---- async-signal-safe formatting ------------------------------------------
struct SafeBuf {
    char data[1024];
    size_t len = 0;
    void put(const char* s) {
        while (s && *s && len < sizeof(data)) data[len++] = *s++;
    }
    void putDec(unsigned long long v) {
        char tmp[24];
        int n = 0;
        do { tmp[n++] = static_cast<char>('0' + (v % 10)); v /= 10; } while (v && n < 24);
        while (n > 0 && len < sizeof(data)) data[len++] = tmp[--n];
    }
    void putSigned(long long v) {
        if (v < 0) { put("-"); putDec(static_cast<unsigned long long>(-(v + 1)) + 1); }
        else putDec(static_cast<unsigned long long>(v));
    }
    void putHex(unsigned long long v, int minDigits = 1) {
        static const char* hex = "0123456789abcdef";
        char tmp[16];
        int n = 0;
        do { tmp[n++] = hex[v & 0xF]; v >>= 4; } while (v && n < 16);
        while (n < minDigits && n < 16) tmp[n++] = '0';
        put("0x");
        while (n > 0 && len < sizeof(data)) data[len++] = tmp[--n];
    }
    void putPadded(unsigned long long v, int width) {
        char tmp[24];
        int n = 0;
        do { tmp[n++] = static_cast<char>('0' + (v % 10)); v /= 10; } while (v && n < 24);
        while (n < width && n < 24) tmp[n++] = '0';
        while (n > 0 && len < sizeof(data)) data[len++] = tmp[--n];
    }
    void flushTo(int fd) {
        size_t off = 0;
        while (off < len) {
            const ssize_t w = ::write(fd, data + off, len - off);
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) break;
            off += static_cast<size_t>(w);
        }
        len = 0;
    }
};

const char* signalName(int sig) {
    switch (sig) {
        case 0:       return "TEST";
        case SIGSEGV: return "SIGSEGV";
        case SIGBUS:  return "SIGBUS";
        case SIGILL:  return "SIGILL";
        case SIGFPE:  return "SIGFPE";
        case SIGABRT: return "SIGABRT";
        case SIGTRAP: return "SIGTRAP";
        default:      return "SIG?";
    }
}

const char* siCodeName(int sig, int code) {
    switch (code) {
        case SI_USER:  return "SI_USER";
#if defined(SI_TKILL)  // Linux only; macOS has no tkill (GitHub #388)
        case SI_TKILL: return "SI_TKILL";
#endif
        case SI_QUEUE: return "SI_QUEUE";
        default: break;
    }
    if (sig == SIGSEGV) {
        if (code == SEGV_MAPERR) return "SEGV_MAPERR";
        if (code == SEGV_ACCERR) return "SEGV_ACCERR";
    } else if (sig == SIGBUS) {
        if (code == BUS_ADRALN) return "BUS_ADRALN";
        if (code == BUS_ADRERR) return "BUS_ADRERR";
        if (code == BUS_OBJERR) return "BUS_OBJERR";
    } else if (sig == SIGFPE) {
        if (code == FPE_INTDIV) return "FPE_INTDIV";
        if (code == FPE_INTOVF) return "FPE_INTOVF";
        if (code == FPE_FLTDIV) return "FPE_FLTDIV";
    } else if (sig == SIGILL) {
        if (code == ILL_ILLOPC) return "ILL_ILLOPC";
        if (code == ILL_ILLOPN) return "ILL_ILLOPN";
    } else if (sig == SIGTRAP) {
        if (code == TRAP_BRKPT) return "TRAP_BRKPT";
    }
    return "?";
}

/// Write the report body to `fd`. Async-signal-safe (see the block above).
/// `info`/`uctx` may be null (the writeTestMinidump seam).
void writeReportBody(int fd, int sig, const siginfo_t* info, const void* uctx) {
    SafeBuf b;
    struct timespec now{};
    ::clock_gettime(CLOCK_REALTIME, &now);

    b.put("=== PDFE CRASH REPORT ===\n");
    b.put("signal: "); b.put(signalName(sig)); b.put(" ("); b.putSigned(sig); b.put(")\n");
    if (info != nullptr) {
        b.put("si_code: "); b.putSigned(info->si_code);
        b.put(" ("); b.put(siCodeName(sig, info->si_code)); b.put(")\n");
        b.put("fault address: ");
        b.putHex(reinterpret_cast<unsigned long long>(info->si_addr), 16);
        b.put("\n");
        if (info->si_code <= 0) {   // sent by kill/raise/abort: who sent it
            b.put("sender pid: "); b.putSigned(info->si_pid); b.put("\n");
        }
    } else {
        b.put("si_code: none (no signal; writeTestMinidump seam)\n");
        b.put("fault address: none\n");
    }
#if defined(__linux__) && defined(__x86_64__)
    if (uctx != nullptr) {
        const auto* uc = static_cast<const ucontext_t*>(uctx);
        b.put("instruction pointer: ");
        b.putHex(static_cast<unsigned long long>(uc->uc_mcontext.gregs[REG_RIP]), 16);
        b.put("\nstack pointer: ");
        b.putHex(static_cast<unsigned long long>(uc->uc_mcontext.gregs[REG_RSP]), 16);
        b.put("\n");
    }
#elif defined(__linux__) && defined(__aarch64__)
    if (uctx != nullptr) {
        const auto* uc = static_cast<const ucontext_t*>(uctx);
        b.put("instruction pointer: ");
        b.putHex(static_cast<unsigned long long>(uc->uc_mcontext.pc), 16);
        b.put("\nstack pointer: ");
        b.putHex(static_cast<unsigned long long>(uc->uc_mcontext.sp), 16);
        b.put("\n");
    }
#else
    (void)uctx;
#endif
    b.put("pid: "); b.putSigned(::getpid()); b.put("\n");
#if defined(__linux__) && defined(SYS_gettid)
    b.put("tid: "); b.putSigned(::syscall(SYS_gettid)); b.put("\n");
#endif
    b.put("time (CLOCK_REALTIME): "); b.putDec(static_cast<unsigned long long>(now.tv_sec));
    b.put("."); b.putPadded(static_cast<unsigned long long>(now.tv_nsec), 9); b.put("\n");
    b.flushTo(fd);
    if (g_versionLen > 0) {
        (void)!::write(fd, g_versionLine, g_versionLen);
    }

    b.put("backtrace:\n");
    b.flushTo(fd);
    void* frames[128];
    const int n = ::backtrace(frames, 128);
    if (n > 0) ::backtrace_symbols_fd(frames, n, fd);
    b.put("backtrace frames: "); b.putSigned(n); b.put("\n");
    b.put("=== END OF REPORT ===\n");
    b.flushTo(fd);
}

/// Create a new report file in the held directory and write it. Returns the
/// fd-relative file name in `nameOut` (NUL-terminated) and true on success.
/// Async-signal-safe.
bool writeReportFile(int sig, const siginfo_t* info, const void* uctx,
                     char* nameOut, size_t nameCap) {
    if (g_crashDirFd < 0 || nameCap < 96) return false;
    struct timespec ts{};
    ::clock_gettime(CLOCK_REALTIME, &ts);
    for (int attempt = 0; attempt < 8; ++attempt) {
        SafeBuf nb;
        nb.put(kReportPrefix);
        nb.putDec(static_cast<unsigned long long>(ts.tv_sec));
        nb.put(".");
        nb.putPadded(static_cast<unsigned long long>(ts.tv_nsec), 9);
        nb.put("_"); nb.putSigned(::getpid());
        nb.put("_"); nb.putSigned(sig);
        if (attempt > 0) { nb.put("_"); nb.putSigned(attempt); }
        nb.put(kReportSuffix);
        if (nb.len >= nameCap) return false;
        std::memcpy(nameOut, nb.data, nb.len);
        nameOut[nb.len] = '\0';

        const int fd = ::openat(g_crashDirFd, nameOut,
                                O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW,
                                0600);
        if (fd < 0) {
            if (errno == EEXIST) continue;
            return false;
        }
        writeReportBody(fd, sig, info, uctx);
        ::fsync(fd);
        ::close(fd);
        return true;
    }
    return false;
}

void writeStderrNotice(int sig, const char* name, bool wrote) {
    SafeBuf b;
    b.put("[CrashHandler] fatal signal ");
    b.put(signalName(sig));
    b.put(" (");
    b.putSigned(sig);
    b.put(wrote ? ") - crash report written: " : ") - could not write a crash report file");
    if (wrote) {
        b.put(g_crashDirPath.empty() ? "" : "<crash dir>/");
        b.put(name);
    }
    b.put("\n");
    b.flushTo(STDERR_FILENO);
}

void fatalSignalHandler(int sig, siginfo_t* info, void* uctx) {
    const int savedErrno = errno;
#if defined(__linux__) && defined(SYS_gettid)
    const int self = static_cast<int>(::syscall(SYS_gettid));
#else
    const int self = static_cast<int>(::getpid());
#endif
    int expected = 0;
    if (!g_handlerTid.compare_exchange_strong(expected, self)) {
        if (expected != self) {
            // Another thread is already writing a report. Give it time to
            // finish and kill the process; do not race it into the directory.
            for (int i = 0; i < 100 && g_handlerTid.load() != 0; ++i) {
                struct timespec ts{0, 100 * 1000000L};
                ::nanosleep(&ts, nullptr);
            }
        }
        // Same thread faulted INSIDE the handler (or the other thread never
        // finished): die with the default action for this signal, now.
        struct sigaction dfl{};
        dfl.sa_handler = SIG_DFL;
        sigemptyset(&dfl.sa_mask);
        ::sigaction(sig, &dfl, nullptr);
        ::raise(sig);
        errno = savedErrno;
        return;
    }

    char name[160];
    name[0] = '\0';
    bool wrote = writeReportFile(sig, info, uctx, name, sizeof(name));
    if (!wrote) {
        // No usable directory: the report still goes somewhere a CI log keeps.
        writeReportBody(STDERR_FILENO, sig, info, uctx);
    }
    writeStderrNotice(sig, name, wrote);

    // SA_RESETHAND restored SIG_DFL for `sig` on entry. `sig` is blocked while
    // we run (no SA_NODEFER), so raise() leaves it pending and it is delivered
    // — with the default action, killing the process — the moment we return.
    // For a hardware fault (SIGSEGV/SIGBUS/SIGILL/SIGFPE) returning would also
    // re-execute the faulting instruction; the explicit raise makes kill()- and
    // abort()-originated signals die the same way.
    ::raise(sig);
    errno = savedErrno;
}

// ---- outside-signal helpers (allowed to allocate) --------------------------

bool posixOverrideIsAcceptable(const std::string& p) {
    if (p.empty() || p[0] != '/') return false;              // absolute only
    if (p.find('\0') != std::string::npos) return false;
    if (p.size() > static_cast<size_t>(PATH_MAX) - 128) return false;  // room for the name
    // Reject any ".." segment (same rule and rationale as the Windows arm).
    size_t i = 0;
    while (i <= p.size()) {
        const size_t slash = p.find('/', i);
        const size_t end = (slash == std::string::npos) ? p.size() : slash;
        if (end - i == 2 && p[i] == '.' && p[i + 1] == '.') return false;
        if (slash == std::string::npos) break;
        i = slash + 1;
    }
    // A network mount is the POSIX analogue of UNC: the report leaves the
    // machine and lands under permissions we cannot verify.
    if (pdfe::core::path_security::path_is_on_network_filesystem(p)) return false;
    return true;
}

std::string stripTrailingSlashes(std::string p) {
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    return p;
}

std::string resolvePosixCrashDir() {
    std::string dir;
    const char* env = std::getenv("PDFE_CRASH_DUMP_DIR");
    if (env != nullptr && *env != '\0') {
        const std::string o(env);
        if (posixOverrideIsAcceptable(o)) {
            dir = stripTrailingSlashes(o);
        } else {
            std::fprintf(stderr,
                         "[CrashHandler] PDFE_CRASH_DUMP_DIR rejected (must be an "
                         "absolute local path with no '..'; network filesystems "
                         "are refused) - falling back to the app data directory\n");
            std::fflush(stderr);
        }
    }
    if (dir.empty()) {
        const QString base =
            QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        if (base.isEmpty()) return {};
        dir = stripTrailingSlashes(base.toStdString()) + "/crashes";
    }
    std::error_code ec;
    if (!pdfe::core::createOwnerOnlyDirectory(std::filesystem::path(dir), ec)) {
        std::fprintf(stderr,
                     "[CrashHandler] could not create an owner-only (0700) crash "
                     "directory (%s) - refusing to write crash reports there\n",
                     ec.message().c_str());
        std::fflush(stderr);
        return {};
    }
    return dir;
}

bool isCrashReportName(const std::string& n) {
    const std::string pre(kReportPrefix), suf(kReportSuffix);
    return n.size() > pre.size() + suf.size()
        && n.compare(0, pre.size(), pre) == 0
        && n.compare(n.size() - suf.size(), suf.size(), suf) == 0;
}

int prunePosixReports(const std::string& dir, int keep) {
    if (keep < 0) keep = 0;
    namespace fs = std::filesystem;
    std::vector<std::pair<std::pair<long long, long long>, std::string>> found;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (!isCrashReportName(name)) continue;
        struct stat st{};
        // lstat: a symlink with a report-like name is not a report; never
        // follow it, never count it, never delete through it.
        if (::lstat(it->path().c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
#if defined(__APPLE__)
        const struct timespec& mt = st.st_mtimespec;
#else
        const struct timespec& mt = st.st_mtim;
#endif
        found.push_back({{static_cast<long long>(mt.tv_sec),
                          static_cast<long long>(mt.tv_nsec)},
                         it->path().string()});
    }
    if (static_cast<int>(found.size()) <= keep) return 0;
    std::sort(found.begin(), found.end());   // oldest mtime first, then name
    int deleted = 0;
    const size_t excess = found.size() - static_cast<size_t>(keep);
    for (size_t i = 0; i < excess; ++i) {
        if (::unlink(found[i].second.c_str()) == 0) ++deleted;
    }
    return deleted;
}

bool handlerIsOurs(const struct sigaction& sa) {
    return (sa.sa_flags & SA_SIGINFO) != 0 && (sa.sa_flags & SA_ONSTACK) != 0
        && sa.sa_sigaction == &fatalSignalHandler;
}

void prepareVersionLine() {
#ifndef PDFE_VERSION_STRING
#define PDFE_VERSION_STRING "0.0.0"
#endif
    SafeBuf b;
    b.put("version: PDFE " PDFE_VERSION_STRING);
#if defined(__VERSION__)
    b.put(" (compiler " __VERSION__ ")");
#endif
#if defined(__x86_64__)
    b.put(" x86_64");
#elif defined(__aarch64__)
    b.put(" aarch64");
#endif
    b.put("\n");
    const size_t n = std::min(b.len, sizeof(g_versionLine));
    std::memcpy(g_versionLine, b.data, n);
    g_versionLen = n;
}

/// Install-time half (outside signal context; allowed to allocate).
void installPosixSignalHandling() {
    if (g_signalsInstalled) return;

    // 1. Directory: resolve + secure, prune, and hold an fd for the handler.
    g_crashDirPath = resolvePosixCrashDir();
    if (!g_crashDirPath.empty()) {
        prunePosixReports(g_crashDirPath, kMaxCrashDumps - 1);
        g_crashDirFd = ::open(g_crashDirPath.c_str(),
                              O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    }

    // 2. Version line, formatted now so the handler only copies bytes.
    prepareVersionLine();

    // 3. Prime backtrace(): its first call dlopen()s the unwinder (malloc +
    //    loader lock). Doing that here means the handler's call does not.
    {
        void* frames[4];
        (void)::backtrace(frames, 4);
    }

    // 4. Alternate signal stack for THIS thread (see LIMITS above).
    if (g_altStackMem == nullptr) {
        void* mem = ::mmap(nullptr, kAltStackBytes, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mem != MAP_FAILED) g_altStackMem = mem;
    }
    if (g_altStackMem != nullptr) {
        stack_t ss{};
        ss.ss_sp = g_altStackMem;
        ss.ss_size = kAltStackBytes;
        ss.ss_flags = 0;
        ::sigaltstack(&ss, nullptr);
    }

    // 5. The handlers. Previous actions saved for uninstall().
    for (int i = 0; i < kNumHandledSignals; ++i) {
        struct sigaction sa{};
        sa.sa_sigaction = &fatalSignalHandler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
        ::sigaction(kHandledSignals[i], &sa, &g_previous[i]);
    }
    g_signalsInstalled = true;
}

void uninstallPosixSignalHandling() {
    if (!g_signalsInstalled) return;
    for (int i = 0; i < kNumHandledSignals; ++i) {
        ::sigaction(kHandledSignals[i], &g_previous[i], nullptr);
    }
    // Disable the alternate stack before unmapping it; a stack the kernel
    // still points at must never be freed.
    stack_t cur{};
    if (g_altStackMem != nullptr && ::sigaltstack(nullptr, &cur) == 0
        && cur.ss_sp == g_altStackMem && !(cur.ss_flags & SS_ONSTACK)) {
        stack_t off{};
        off.ss_flags = SS_DISABLE;
        if (::sigaltstack(&off, nullptr) == 0) {
            ::munmap(g_altStackMem, kAltStackBytes);
            g_altStackMem = nullptr;
        }
    }
    if (g_crashDirFd >= 0) {
        ::close(g_crashDirFd);
        g_crashDirFd = -1;
    }
    g_signalsInstalled = false;
}

#endif  // _WIN32 / POSIX

}  // namespace

CrashHandler::CrashHandler() = default;

CrashHandler::~CrashHandler() {
    if (installed_) {
        uninstall();
    }
}

CrashHandler& CrashHandler::instance() {
    static CrashHandler instance;
    return instance;
}

void CrashHandler::install() {
    if (installed_) {
        return;
    }

    // Install C++ terminate handler
    previousTerminateHandler_ = std::set_terminate(terminateHandler);

#ifdef _WIN32
    // Install Windows SEH handler
    previousSehHandler_ = reinterpret_cast<void*>(
        SetUnhandledExceptionFilter(sehHandler)
    );

    // WIN-82 / GitHub #103 — the two fault kinds the handlers above cannot see.
    //
    // A pure-virtual call is not a structured exception and does not unwind
    // through std::terminate: MSVC emits a direct call to `_purecall`, whose
    // default implementation prints "pure virtual function call" and exits. A
    // CRT invalid-parameter fault (`_invalid_parameter`) is dispatched the same
    // way. Both therefore killed PDFE with NO minidump written — the top-level
    // SEH filter is never reached in either case.
    previousPurecallHandler_ =
        reinterpret_cast<void*>(_set_purecall_handler(purecallHandler));
    previousInvalidParameterHandler_ = reinterpret_cast<void*>(
        _set_invalid_parameter_handler(invalidParameterHandler));
#else
    // POSIX: crash directory + fd, pruning, alternate stack, backtrace priming
    // and the fatal-signal handlers. See the block comment above
    // installPosixSignalHandling().
    installPosixSignalHandling();
#endif

    installed_ = true;
}

void CrashHandler::uninstall() {
    if (!installed_) {
        return;
    }

    // Restore previous terminate handler
    if (previousTerminateHandler_) {
        std::set_terminate(previousTerminateHandler_);
        previousTerminateHandler_ = nullptr;
    }

#ifdef _WIN32
    // Restore previous SEH handler
    if (previousSehHandler_) {
        SetUnhandledExceptionFilter(
            reinterpret_cast<LPTOP_LEVEL_EXCEPTION_FILTER>(previousSehHandler_)
        );
        previousSehHandler_ = nullptr;
    }

    // WIN-82. Restore unconditionally, INCLUDING when the previous value was
    // null: null is the CRT's own "use the default" state, not "nothing was
    // installed", so skipping the restore on null would leave our handler
    // wired up after uninstall() and make a later test in the same process
    // die in our dump writer instead of failing normally.
    _set_purecall_handler(
        reinterpret_cast<_purecall_handler>(previousPurecallHandler_));
    previousPurecallHandler_ = nullptr;
    _set_invalid_parameter_handler(reinterpret_cast<_invalid_parameter_handler>(
        previousInvalidParameterHandler_));
    previousInvalidParameterHandler_ = nullptr;
#else
    uninstallPosixSignalHandling();
#endif

    installed_ = false;
}

void CrashHandler::terminateHandler() {
    CrashHandler& handler = instance();

    QString title = "PDFE has encountered an error";
    QString details;

    // Try to get information about the current exception
    std::exception_ptr exPtr = std::current_exception();

    if (exPtr) {
        try {
            std::rethrow_exception(exPtr);
        }
        catch (const pdfe::core::PdfeException& e) {
            details = QString("PDFE Exception: %1")
                .arg(QString::fromUtf8(e.what()));
        }
        catch (const std::exception& e) {
            details = QString("Standard Exception: %1")
                .arg(QString::fromUtf8(e.what()));
        }
        catch (...) {
            details = "Unknown exception type";
        }
    } else {
        details = "std::terminate called without an active exception";
    }

#ifdef _WIN32
    // Leave a minidump for post-mortem (no live SEH record here — current
    // thread context is captured).
    QString dumpPath = writeMinidump(nullptr);
    if (!dumpPath.isEmpty()) {
        details += QString("\n\nMinidump: %1").arg(dumpPath);
        std::fprintf(stderr, "[CrashHandler] minidump written: %s\n",
                     dumpPath.toUtf8().constData());
        std::fflush(stderr);
    }
#endif

    // Store crash info for testing
    handler.lastCrashInfo_.title = title;
    handler.lastCrashInfo_.details = details;

    // Route the notification (see sehHandler): callback > automated (no modal) >
    // interactive dialog. A modal here under automation would block the driver.
    if (handler.testCallback_) {
        handler.testCallback_(title, details);
    } else if (isAutomatedRun()) {
        std::fprintf(stderr, "[CrashHandler] %s | %s\n",
                     title.toUtf8().constData(), details.toUtf8().constData());
        std::fflush(stderr);
    } else {
        showCrashDialog(title, details);
    }

    // Call previous handler if available, otherwise abort
    if (handler.previousTerminateHandler_) {
        handler.previousTerminateHandler_();
    } else {
        std::abort();
    }
}

#ifdef _WIN32
long __stdcall CrashHandler::sehHandler(struct _EXCEPTION_POINTERS* exInfo) {
    CrashHandler& handler = instance();

    QString title = "PDFE has encountered a fatal error";
    QString details;

    // Decode exception code
    DWORD exCode = exInfo->ExceptionRecord->ExceptionCode;

    switch (exCode) {
        case EXCEPTION_ACCESS_VIOLATION: {
            ULONG_PTR* info = exInfo->ExceptionRecord->ExceptionInformation;
            const char* operation = (info[0] == 0) ? "read" :
                                   (info[0] == 1) ? "write" : "execute";
            details = QString("Access Violation: Attempted to %1 address 0x%2")
                .arg(operation)
                .arg(info[1], 0, 16);
            break;
        }
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
            details = "Array Bounds Exceeded";
            break;
        case EXCEPTION_BREAKPOINT:
            details = "Breakpoint Exception";
            break;
        case EXCEPTION_DATATYPE_MISALIGNMENT:
            details = "Datatype Misalignment";
            break;
        case EXCEPTION_FLT_DENORMAL_OPERAND:
            details = "Floating Point: Denormal Operand";
            break;
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:
            details = "Floating Point: Divide by Zero";
            break;
        case EXCEPTION_FLT_INEXACT_RESULT:
            details = "Floating Point: Inexact Result";
            break;
        case EXCEPTION_FLT_INVALID_OPERATION:
            details = "Floating Point: Invalid Operation";
            break;
        case EXCEPTION_FLT_OVERFLOW:
            details = "Floating Point: Overflow";
            break;
        case EXCEPTION_FLT_STACK_CHECK:
            details = "Floating Point: Stack Check";
            break;
        case EXCEPTION_FLT_UNDERFLOW:
            details = "Floating Point: Underflow";
            break;
        case EXCEPTION_ILLEGAL_INSTRUCTION:
            details = "Illegal Instruction";
            break;
        case EXCEPTION_IN_PAGE_ERROR:
            details = "In-Page Error";
            break;
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
            details = "Integer Divide by Zero";
            break;
        case EXCEPTION_INT_OVERFLOW:
            details = "Integer Overflow";
            break;
        case EXCEPTION_INVALID_DISPOSITION:
            details = "Invalid Disposition";
            break;
        case EXCEPTION_NONCONTINUABLE_EXCEPTION:
            details = "Noncontinuable Exception";
            break;
        case EXCEPTION_PRIV_INSTRUCTION:
            details = "Privileged Instruction";
            break;
        case EXCEPTION_SINGLE_STEP:
            details = "Single Step Exception";
            break;
        case EXCEPTION_STACK_OVERFLOW:
            details = "Stack Overflow";
            break;
        default:
            details = QString("Unknown Exception Code: 0x%1")
                .arg(exCode, 8, 16, QChar('0'));
            break;
    }

    // Add address information
    details += QString("\n\nException Address: 0x%1")
        .arg(reinterpret_cast<quintptr>(exInfo->ExceptionRecord->ExceptionAddress), 0, 16);

    // Write a minidump FIRST (before any dialog / heap-heavy work), so a crash
    // always leaves a stack behind for post-mortem — this is what makes an
    // otherwise-invisible teardown AV diagnosable in CI.
    QString dumpPath = writeMinidump(exInfo);
    if (!dumpPath.isEmpty()) {
        details += QString("\n\nMinidump: %1").arg(dumpPath);
        std::fprintf(stderr, "[CrashHandler] minidump written: %s\n",
                     dumpPath.toUtf8().constData());
        std::fflush(stderr);
    }

    // Store crash info for testing
    handler.lastCrashInfo_.title = title;
    handler.lastCrashInfo_.details = details;

    // Route the notification: an explicit test callback wins; otherwise, under
    // automation (UI-E2E / CI) we MUST NOT pop a modal dialog — it would block
    // the driver (pywinauto) until the per-test timeout, wedging the whole
    // suite (the observed ~30-min stalls). Emit to stderr and let the process
    // terminate. Only an interactive user gets the dialog.
    if (handler.testCallback_) {
        handler.testCallback_(title, details);
    } else if (isAutomatedRun()) {
        std::fprintf(stderr, "[CrashHandler] %s | %s\n",
                     title.toUtf8().constData(), details.toUtf8().constData());
        std::fflush(stderr);
    } else {
        showCrashDialog(title, details);
    }

    return EXCEPTION_EXECUTE_HANDLER;
}

// ---------------------------------------------------------------------------
// WIN-82 / GitHub #103 — the two CRT fault kinds.
//
// Shared tail. Deliberately mirrors terminateHandler()/sehHandler() rather than
// inventing a third notification policy: minidump first (it is the only thing
// that survives the process), then record for tests, then route
// callback > automated-stderr > modal dialog.
//
// TERMINATION. This must not return. Returning from `_purecall` resumes the
// call that faulted, and returning from an invalid-parameter handler makes the
// CRT continue into behaviour it has already declared undefined. TerminateProcess
// with a distinctive status is used rather than abort() so that (a) a parent
// test can distinguish "PDFE's handler ran and ended the process" from the CRT
// default's exit 3 or a genuine AV's 0xC0000005, and (b) abort()'s own
// _CALL_REPORTFAULT path cannot re-enter WER after we have already written a
// dump.
// ---------------------------------------------------------------------------

/// Exit status a process leaves behind when CrashHandler handled a CRT fault
/// (pure-virtual call or invalid parameter) and terminated deliberately.
/// Not a Windows exception status (top two bits are not both set), so it can
/// never be confused with a real 0xC0000005-family crash.
static constexpr DWORD kCrtFaultHandledExitCode = 0x3D4D0001u;

void CrashHandler::reportCrtFaultAndDie(const QString& title,
                                        const QString& details) {
    CrashHandler& handler = instance();

    QString full = details;
    const QString dumpPath = writeMinidump(nullptr);
    if (!dumpPath.isEmpty()) {
        full += QString("\n\nMinidump: %1").arg(dumpPath);
        std::fprintf(stderr, "[CrashHandler] minidump written: %s\n",
                     dumpPath.toUtf8().constData());
        std::fflush(stderr);
    }

    handler.lastCrashInfo_.title = title;
    handler.lastCrashInfo_.details = full;

    if (handler.testCallback_) {
        handler.testCallback_(title, full);
    } else if (isAutomatedRun()) {
        std::fprintf(stderr, "[CrashHandler] %s | %s\n",
                     title.toUtf8().constData(), full.toUtf8().constData());
        std::fflush(stderr);
    } else {
        showCrashDialog(title, full);
    }

    std::fflush(stderr);
    std::fflush(stdout);
    ::TerminateProcess(::GetCurrentProcess(), kCrtFaultHandledExitCode);
    // TerminateProcess on the current process does not return; this is only
    // here so the [[noreturn]] contract holds for every compiler.
    std::abort();
}

void __cdecl CrashHandler::purecallHandler() {
    reportCrtFaultAndDie(
        "PDFE has encountered a fatal error",
        "Pure virtual function call (_purecall): a virtual call was dispatched "
        "on an object whose most-derived part has already been destroyed, or "
        "was not yet constructed.");
}

void __cdecl CrashHandler::invalidParameterHandler(const wchar_t* expression,
                                                   const wchar_t* function,
                                                   const wchar_t* file,
                                                   unsigned int line,
                                                   std::uintptr_t /*reserved*/) {
    // The release CRT passes null for all of these; the debug CRT fills them
    // in. Report whatever is available rather than dereferencing blindly.
    const auto text = [](const wchar_t* w) -> QString {
        return (w != nullptr) ? QString::fromWCharArray(w)
                              : QString("<not available>");
    };
    reportCrtFaultAndDie(
        "PDFE has encountered a fatal error",
        QString("Invalid CRT parameter: %1 (in %2, %3:%4)")
            .arg(text(expression), text(function), text(file))
            .arg(line));
}

QString CrashHandler::writeMinidump(struct _EXCEPTION_POINTERS* exInfo) {
    std::wstring dir = resolveCrashDumpDir();
    if (dir.empty()) return {};

    // Retention: make room for THIS dump by removing the oldest, rather than
    // refusing to write. See pruneOldestDumps() for why the old refuse-at-cap
    // behaviour discarded exactly the crash you were about to investigate.
    if (countExistingDumps(dir) >= kMaxCrashDumps) {
        const int removed = pruneOldestDumps(dir, kMaxCrashDumps - 1);
        std::fprintf(stderr,
                     "[CrashHandler] dump cap (%d) reached — pruned %d oldest dump(s)\n",
                     kMaxCrashDumps, removed);
        std::fflush(stderr);
        if (countExistingDumps(dir) >= kMaxCrashDumps) {
            // Pruning could not free a slot (every candidate locked, or the
            // directory is not writable). Refusing here is the old behaviour,
            // kept ONLY as the genuine last resort it was always described as.
            std::fprintf(stderr,
                         "[CrashHandler] pruning freed no slot — skipping dump write\n");
            std::fflush(stderr);
            return {};
        }
    }

    SYSTEMTIME st;
    ::GetLocalTime(&st);
    wchar_t name[160];
    ::swprintf(name, 160,
               L"\\pdfe_crash_%04u%02u%02u_%02u%02u%02u_%lu.dmp",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
               ::GetCurrentProcessId());
    std::wstring path = dir + name;

    HANDLE hFile = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return {};

    MINIDUMP_EXCEPTION_INFORMATION mei{};
    mei.ThreadId = ::GetCurrentThreadId();
    mei.ExceptionPointers = exInfo;
    mei.ClientPointers = FALSE;

    // Stacks + handles + thread info + referenced memory: enough to root-cause a
    // UAF/teardown crash without a full-memory dump's size.
    const MINIDUMP_TYPE type = static_cast<MINIDUMP_TYPE>(
        MiniDumpWithDataSegs | MiniDumpWithHandleData | MiniDumpWithThreadInfo |
        MiniDumpWithUnloadedModules | MiniDumpWithIndirectlyReferencedMemory);

    BOOL ok = ::MiniDumpWriteDump(::GetCurrentProcess(), ::GetCurrentProcessId(),
                                  hFile, type, exInfo ? &mei : nullptr,
                                  nullptr, nullptr);
    ::CloseHandle(hFile);
    if (!ok) {
        ::DeleteFileW(path.c_str());
        return {};
    }
    return QString::fromWCharArray(path.c_str());
}
#endif

CrashHandler::InstalledHandlers CrashHandler::installedHandlers() {
    InstalledHandlers out;

    // std::get_terminate() reads the current handler without disturbing it.
    out.terminate = (std::get_terminate() == &CrashHandler::terminateHandler);

#ifdef _WIN32
    // Win32 and the CRT offer no getters for the remaining three, only
    // set-and-return-previous. Each is therefore a set/restore round trip.
    // Safe here: these are cheap pointer stores, and the window in which the
    // handler is momentarily the previous value is a few instructions wide on
    // one thread. Doing it at all is the point — a bool cached at install time
    // would report our INTENT, and the whole reason this seam exists is that
    // intent and the CRT's actual state can differ.
    {
        LPTOP_LEVEL_EXCEPTION_FILTER prev = ::SetUnhandledExceptionFilter(nullptr);
        ::SetUnhandledExceptionFilter(prev);
        out.unhandledExceptionFilter = (prev == &CrashHandler::sehHandler);
    }
    {
        _purecall_handler prev = _set_purecall_handler(nullptr);
        _set_purecall_handler(prev);
        out.purecall = (prev == &CrashHandler::purecallHandler);
    }
    {
        _invalid_parameter_handler prev = _set_invalid_parameter_handler(nullptr);
        _set_invalid_parameter_handler(prev);
        out.invalidParameter = (prev == &CrashHandler::invalidParameterHandler);
    }
#else
    // POSIX has getters, so no round trip: read each action back and require
    // EVERY handled signal to dispatch to our handler with SA_ONSTACK. A
    // library that later replaced one of them makes this false.
    {
        bool allOurs = true;
        for (int i = 0; i < kNumHandledSignals; ++i) {
            struct sigaction cur{};
            if (::sigaction(kHandledSignals[i], nullptr, &cur) != 0
                || !handlerIsOurs(cur)) {
                allOurs = false;
            }
        }
        out.fatalSignals = allOurs;
        stack_t ss{};
        out.altStack = g_altStackMem != nullptr && ::sigaltstack(nullptr, &ss) == 0
                    && !(ss.ss_flags & SS_DISABLE) && ss.ss_sp == g_altStackMem;
    }
#endif

    return out;
}

bool CrashHandler::isAutomatedRun() {
    // Any of these present => automated context (no interactive user to see a
    // modal). GitHub Actions sets CI=true, so this works in CI unchanged; the
    // UI-E2E harness also sets PDFE_UI_TEST for local automated runs.
    return std::getenv("PDFE_UI_TEST") != nullptr
        || std::getenv("PDFE_HEADLESS") != nullptr
        || std::getenv("CI") != nullptr;
}

QString CrashHandler::writeTestMinidump() {
#ifdef _WIN32
    return writeMinidump(nullptr);  // current-thread context, no live exception
#else
    // Same writer as the signal handler, through a directory fd opened the
    // same way. Not in signal context, so pruning to make room is allowed here
    // (mirrors the Windows arm's prune-then-write).
    const std::string dir = resolvePosixCrashDir();
    if (dir.empty()) return {};
    prunePosixReports(dir, kMaxCrashDumps - 1);
    const int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dfd < 0) return {};
    const int savedFd = g_crashDirFd;
    g_crashDirFd = dfd;
    char name[160];
    const bool ok = writeReportFile(0, nullptr, nullptr, name, sizeof(name));
    g_crashDirFd = savedFd;
    ::close(dfd);
    if (!ok) return {};
    return QString::fromStdString(dir + "/" + name);
#endif
}

// --- phase-11 seams: the retention/validation policy, made testable ---------

int CrashHandler::maxCrashDumps() {
    return kMaxCrashDumps;  // 150 on every platform
}

bool CrashHandler::isAcceptableCrashDumpDirOverride(const QString& dir) {
#ifdef _WIN32
    return crashDumpDirOverrideIsAcceptable(dir.toStdWString());
#else
    return posixOverrideIsAcceptable(dir.toStdString());
#endif
}

QString CrashHandler::resolvedCrashDumpDir() {
#ifdef _WIN32
    const std::wstring d = resolveCrashDumpDir();
    return d.empty() ? QString() : QString::fromWCharArray(d.c_str());
#else
    const std::string d = resolvePosixCrashDir();
    return d.empty() ? QString() : QString::fromStdString(d);
#endif
}

int CrashHandler::pruneCrashDumps(const QString& dir, int keep) {
#ifdef _WIN32
    if (dir.isEmpty()) return 0;
    return pruneOldestDumps(dir.toStdWString(), keep);
#else
    if (dir.isEmpty()) return 0;
    return prunePosixReports(dir.toStdString(), keep);
#endif
}

void CrashHandler::showCrashDialog(const QString& title, const QString& details) {
    QString fullMessage = details + "\n\nThe application will now close. Please report this issue.";

    QMessageBox::critical(
        nullptr,
        title,
        fullMessage,
        QMessageBox::Ok
    );
}

void CrashHandler::setTestCallback(CrashCallback cb) {
    testCallback_ = std::move(cb);
}

CrashHandler::CrashInfo CrashHandler::lastCrashInfo() const {
    return lastCrashInfo_;
}

} // namespace pdfe::app
