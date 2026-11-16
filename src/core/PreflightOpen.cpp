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
#  include <sys/resource.h>
#  include <unistd.h>
#  include <signal.h>
#  include <fcntl.h>
#  include <poll.h>
#  include <cerrno>
#  include <cstdint>
#  include <cstdlib>
#  include <cstring>
#  include <ctime>
#  if defined(__linux__)
#    include <sys/prctl.h>
#    include <sys/syscall.h>
#    include <stddef.h>             // offsetof
#    if __has_include(<linux/seccomp.h>) && __has_include(<linux/filter.h>) \
        && __has_include(<linux/audit.h>)
#      include <linux/seccomp.h>
#      include <linux/filter.h>
#      include <linux/audit.h>
#      define PDFE_HAVE_SECCOMP_HEADERS 1
#    endif
#    if __has_include(<linux/landlock.h>)
#      include <linux/landlock.h>
#      define PDFE_HAVE_LANDLOCK_HEADERS 1
#    endif
#  endif
#  if defined(__APPLE__)
#    include <mach-o/dyld.h>  // _NSGetExecutablePath
#    include <climits>        // PATH_MAX
#    include <cstdint>        // uint32_t
#    include <cstdlib>        // realpath
#  endif
#endif

#include <algorithm>
#include <optional>
#include <vector>

#if !defined(_WIN32)
extern char** environ;
#endif

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

namespace {
// Bits written by the sandboxed child (see sandboxChildAndExec). Also the
// storage format of the last report, so the report is one atomic word.
constexpr std::uint32_t kBitNoNewPrivs  = 1u << 0;
constexpr std::uint32_t kBitRlimits     = 1u << 1;
constexpr std::uint32_t kBitLandlockFs  = 1u << 2;
constexpr std::uint32_t kBitLandlockNet = 1u << 3;
constexpr std::uint32_t kBitSeccompNet  = 1u << 4;
constexpr std::uint32_t kAllBits = kBitNoNewPrivs | kBitRlimits | kBitLandlockFs
                                 | kBitLandlockNet | kBitSeccompNet;

std::atomic<std::uint32_t> g_lastPosixMask{0};

PosixSandboxReport reportFromMask(std::uint32_t m)
{
    PosixSandboxReport r;
    r.noNewPrivs  = (m & kBitNoNewPrivs) != 0;
    r.rlimits     = (m & kBitRlimits) != 0;
    r.landlockFs  = (m & kBitLandlockFs) != 0;
    r.landlockNet = (m & kBitLandlockNet) != 0;
    r.seccompNet  = (m & kBitSeccompNet) != 0;
    return r;
}
}  // namespace

PosixSandboxReport lastPosixSandboxReport()
{
    return reportFromMask(g_lastPosixMask.load(std::memory_order_acquire));
}

namespace {
[[maybe_unused]] std::uint32_t maskFromReport(const PosixSandboxReport& r)
{
    std::uint32_t m = 0;
    if (r.noNewPrivs)  m |= kBitNoNewPrivs;
    if (r.rlimits)     m |= kBitRlimits;
    if (r.landlockFs)  m |= kBitLandlockFs;
    if (r.landlockNet) m |= kBitLandlockNet;
    if (r.seccompNet)  m |= kBitSeccompNet;
    return m;
}
}  // namespace

namespace detail {

HelperIsolation isolationForSandboxReport(bool maskReported,
                                          const PosixSandboxReport& report)
{
    // A child that died before reporting proved nothing, whatever the report
    // struct happens to contain: degraded. Otherwise Sandboxed requires EVERY
    // property; one missing bit is a degraded sandbox and must say so.
    if (!maskReported) return HelperIsolation::Unrestricted;
    return maskFromReport(report) == kAllBits ? HelperIsolation::Sandboxed
                                              : HelperIsolation::Unrestricted;
}

}  // namespace detail

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

#if !defined(_WIN32)
namespace {

// =============================================================================
// THE POSIX PREFLIGHT SANDBOX (Linux port, phase 6).
//
// Split in two halves on purpose:
//
//   PARENT, before fork(): everything that allocates or can fail in an
//   interesting way — the seccomp BPF program, the Landlock ruleset fd, the
//   rlimit values (clamped to the parent's own hard limits so the child's
//   setrlimit cannot fail with EPERM), the argv/envp arrays and the report
//   pipe. PDFE is multithreaded (Qt, render workers), and after fork() only
//   the calling thread exists in the child; any lock another thread held at
//   fork time (malloc's arena locks included) stays locked forever. So:
//
//   CHILD, between fork() and execve(): async-signal-safe system calls only —
//   setrlimit, prctl, syscall(landlock_restrict_self / close_range), write,
//   execve, _exit. No malloc, no stdio, no C++ library, no locks. (syscall()
//   is not on the POSIX async-signal-safe list by name, but glibc's syscall()
//   is a register shuffle around the `syscall` instruction plus errno; it
//   takes no lock and touches no shared state.)
//
// Order in the child: rlimits, NO_NEW_PRIVS, Landlock, seccomp LAST — the
// seccomp filter is the one that could in principle deny a later step, and
// both Landlock and seccomp require NO_NEW_PRIVS (absent CAP_SYS_ADMIN).
//
// REPORTING. The child writes a bitmask of the properties whose syscalls
// actually succeeded to a CLOEXEC pipe and then execs. The parent reads it:
//   * 4 bytes then EOF          -> exec succeeded, mask is authoritative;
//   * 4 bytes then 4 more bytes -> exec failed (the second word is errno);
//   * EOF with no mask          -> the child died before reporting; treated
//                                  as "nothing applied" and, since the helper
//                                  never ran, the verdict is "not safe".
//
// FALLBACKS — reported, never silent. A kernel without Landlock (pre-5.13, or
// "landlock" absent from the boot-time LSM list) leaves landlockFs and
// landlockNet false; a Landlock ABI below 4 leaves landlockNet false; a kernel
// without seccomp filter mode leaves seccompNet false; a build without the
// headers leaves the corresponding bits false. In every such case
// lastHelperIsolation() is Unrestricted rather than Sandboxed. The helper is
// still launched — crash isolation holds in every case — which is the same
// trade the Windows arm makes when the low-integrity token cannot be made.
// =============================================================================

constexpr unsigned long long kAddressSpaceLimit = 4ULL << 30;  // 4 GiB
constexpr rlim_t kOpenFileLimit = 256;
constexpr int kNoTimeoutCpuSeconds = 600;   // timeoutMs < 0 ("no timeout")

unsigned long long cpuSecondsFor(int timeoutMs)
{
    if (timeoutMs < 0) return static_cast<unsigned long long>(kNoTimeoutCpuSeconds);
    // Round up, plus slack: the wall-clock timeout is the primary bound; the CPU
    // limit is the backstop if the parent itself stalls or is killed.
    return static_cast<unsigned long long>((timeoutMs + 999) / 1000) + 2ULL;
}

#if defined(__linux__) && defined(PDFE_HAVE_SECCOMP_HEADERS)
#  if defined(__x86_64__)
#    define PDFE_SECCOMP_ARCH AUDIT_ARCH_X86_64
#  elif defined(__aarch64__)
#    define PDFE_SECCOMP_ARCH AUDIT_ARCH_AARCH64
#  endif
#endif

#if defined(PDFE_SECCOMP_ARCH)
/// The deny-list filter: wrong arch -> kill; x32 syscall numbers -> kill;
/// the listed syscalls -> EPERM; everything else allowed.
std::vector<sock_filter> buildSeccompFilter()
{
    std::vector<sock_filter> f;
    auto stmt = [&](unsigned short code, unsigned int k) {
        f.push_back(sock_filter{code, 0, 0, k});
    };
    auto jeq = [&](unsigned int k, unsigned char jt, unsigned char jf) {
        f.push_back(sock_filter{static_cast<unsigned short>(BPF_JMP | BPF_JEQ | BPF_K),
                                jt, jf, k});
    };
#  if defined(SECCOMP_RET_KILL_PROCESS)
    const unsigned int kKill = SECCOMP_RET_KILL_PROCESS;
#  else
    const unsigned int kKill = SECCOMP_RET_KILL;
#  endif
    const unsigned int kDeny = SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA);

    stmt(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch));
    jeq(PDFE_SECCOMP_ARCH, 1, 0);
    stmt(BPF_RET | BPF_K, kKill);
    stmt(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr));
#  if defined(__x86_64__)
    // x32 ABI shares AUDIT_ARCH_X86_64 but sets bit 30 of the syscall number;
    // without this check `socket` via x32 would bypass the deny list.
    f.push_back(sock_filter{static_cast<unsigned short>(BPF_JMP | BPF_JGE | BPF_K),
                            0, 1, 0x40000000u});
    stmt(BPF_RET | BPF_K, kKill);
#  endif
    const long denied[] = {
#  ifdef __NR_socket
        __NR_socket,
#  endif
#  ifdef __NR_socketpair
        __NR_socketpair,
#  endif
#  ifdef __NR_connect
        __NR_connect,
#  endif
#  ifdef __NR_bind
        __NR_bind,
#  endif
#  ifdef __NR_listen
        __NR_listen,
#  endif
#  ifdef __NR_accept
        __NR_accept,
#  endif
#  ifdef __NR_accept4
        __NR_accept4,
#  endif
#  ifdef __NR_sendto
        __NR_sendto,
#  endif
#  ifdef __NR_sendmsg
        __NR_sendmsg,
#  endif
#  ifdef __NR_sendmmsg
        __NR_sendmmsg,
#  endif
#  ifdef __NR_ptrace
        __NR_ptrace,
#  endif
#  ifdef __NR_process_vm_readv
        __NR_process_vm_readv,
#  endif
#  ifdef __NR_process_vm_writev
        __NR_process_vm_writev,
#  endif
        // io_uring: since Linux 5.19 an IORING_OP_SOCKET submission creates a
        // socket WITHOUT the socket(2) syscall, so the socket entry above
        // would not see it, and Landlock's network rules (TCP bind/connect
        // only) would not either. The helper never uses io_uring; deny the
        // whole interface.
#  ifdef __NR_io_uring_setup
        __NR_io_uring_setup,
#  endif
#  ifdef __NR_io_uring_enter
        __NR_io_uring_enter,
#  endif
#  ifdef __NR_io_uring_register
        __NR_io_uring_register,
#  endif
    };
    for (long nr : denied) {
        jeq(static_cast<unsigned int>(nr), 0, 1);
        stmt(BPF_RET | BPF_K, kDeny);
    }
    stmt(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
    return f;
}
#endif

#if defined(__linux__) && defined(PDFE_HAVE_LANDLOCK_HEADERS) \
    && defined(__NR_landlock_create_ruleset) && defined(__NR_landlock_restrict_self)
#  define PDFE_HAVE_LANDLOCK 1
#endif

int queryLandlockAbi()
{
#if defined(PDFE_HAVE_LANDLOCK)
    const long abi = ::syscall(__NR_landlock_create_ruleset, nullptr, 0,
                               LANDLOCK_CREATE_RULESET_VERSION);
    return abi < 0 ? 0 : static_cast<int>(abi);
#else
    return 0;
#endif
}

/// A ruleset handling every write-type FS access the ABI knows, with NO rules
/// added — so all of them are denied everywhere — while read/execute/readdir
/// are not handled and therefore stay allowed. Returns -1 when unavailable.
/// `netHandled` is set when the ruleset also denies TCP bind/connect.
int makeWriteDenyRuleset(bool& netHandled)
{
    netHandled = false;
#if defined(PDFE_HAVE_LANDLOCK)
    const int abi = queryLandlockAbi();
    if (abi < 1) return -1;
    struct landlock_ruleset_attr attr;
    std::memset(&attr, 0, sizeof(attr));
    attr.handled_access_fs =
        LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_REMOVE_DIR
        | LANDLOCK_ACCESS_FS_REMOVE_FILE | LANDLOCK_ACCESS_FS_MAKE_CHAR
        | LANDLOCK_ACCESS_FS_MAKE_DIR | LANDLOCK_ACCESS_FS_MAKE_REG
        | LANDLOCK_ACCESS_FS_MAKE_SOCK | LANDLOCK_ACCESS_FS_MAKE_FIFO
        | LANDLOCK_ACCESS_FS_MAKE_BLOCK | LANDLOCK_ACCESS_FS_MAKE_SYM;
#  ifdef LANDLOCK_ACCESS_FS_REFER
    if (abi >= 2) attr.handled_access_fs |= LANDLOCK_ACCESS_FS_REFER;
#  endif
#  ifdef LANDLOCK_ACCESS_FS_TRUNCATE
    if (abi >= 3) attr.handled_access_fs |= LANDLOCK_ACCESS_FS_TRUNCATE;
#  endif
    size_t attrSize = offsetof(struct landlock_ruleset_attr, handled_access_fs)
                    + sizeof(attr.handled_access_fs);
#  if defined(LANDLOCK_ACCESS_NET_BIND_TCP) && defined(LANDLOCK_ACCESS_NET_CONNECT_TCP)
    bool wantNet = false;
    if (abi >= 4) {
        attr.handled_access_net =
            LANDLOCK_ACCESS_NET_BIND_TCP | LANDLOCK_ACCESS_NET_CONNECT_TCP;
        attrSize = offsetof(struct landlock_ruleset_attr, handled_access_net)
                 + sizeof(attr.handled_access_net);
        wantNet = true;
    }
#  else
    const bool wantNet = false;  // headers predate Landlock ABI 4: reported as unavailable
#  endif
    const long fd = ::syscall(__NR_landlock_create_ruleset, &attr, attrSize, 0);
    if (fd < 0) return -1;
    ::fcntl(static_cast<int>(fd), F_SETFD, FD_CLOEXEC);  // already CLOEXEC; stated
    netHandled = wantNet;
    return static_cast<int>(fd);
#else
    return -1;
#endif
}

/// Everything the child needs, prepared by the parent.
struct ChildPlan {
    const char* path = nullptr;
    char* const* argv = nullptr;
    char* const* envp = nullptr;
    int reportFd = -1;
    int landlockFd = -1;
    bool landlockNet = false;
    struct rlimit core{}, as{}, cpu{}, nofile{};
#if defined(PDFE_SECCOMP_ARCH)
    const struct sock_fprog* seccomp = nullptr;
#endif
};

/// Runs in the forked child. ASYNC-SIGNAL-SAFE CALLS ONLY (see block above).
[[noreturn]] void sandboxChildAndExec(const ChildPlan& plan) noexcept
{
    std::uint32_t mask = 0;

#if defined(__linux__)
    // Nothing the parent has open (Qt's sockets, document files, eventfds)
    // should reach the helper. CLOSE_RANGE_CLOEXEC (Linux 5.11) marks every fd
    // >= 3 close-on-exec; the report pipe is CLOEXEC anyway. Best effort: on an
    // older kernel this returns ENOSYS and only already-CLOEXEC fds are closed.
#  if defined(__NR_close_range)
    ::syscall(__NR_close_range, 3u, ~0u, 4u /* CLOSE_RANGE_CLOEXEC */);
#  endif
    // Do not outlive the parent: if PDFE dies mid-preflight the helper is
    // killed rather than orphaned.
    ::prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0);
#endif

    bool rl = true;
    rl = (::setrlimit(RLIMIT_CORE, &plan.core) == 0) && rl;
    rl = (::setrlimit(RLIMIT_AS, &plan.as) == 0) && rl;
    rl = (::setrlimit(RLIMIT_CPU, &plan.cpu) == 0) && rl;
    rl = (::setrlimit(RLIMIT_NOFILE, &plan.nofile) == 0) && rl;
    if (rl) mask |= kBitRlimits;

#if defined(__linux__)
    if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0) mask |= kBitNoNewPrivs;

#  if defined(PDFE_HAVE_LANDLOCK)
    if (plan.landlockFd >= 0 && (mask & kBitNoNewPrivs)
        && ::syscall(__NR_landlock_restrict_self, plan.landlockFd, 0u) == 0) {
        mask |= kBitLandlockFs;
        if (plan.landlockNet) mask |= kBitLandlockNet;
    }
    if (plan.landlockFd >= 0) ::close(plan.landlockFd);
#  endif

#  if defined(PDFE_SECCOMP_ARCH)
    if (plan.seccomp != nullptr && (mask & kBitNoNewPrivs)
        && ::prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, plan.seccomp, 0, 0) == 0) {
        mask |= kBitSeccompNet;
    }
#  endif
#endif

    (void)!::write(plan.reportFd, &mask, sizeof(mask));
    ::execve(plan.path, plan.argv, plan.envp);
    const int e = errno;
    (void)!::write(plan.reportFd, &e, sizeof(e));
    _exit(127);
}

/// Read exactly `n` bytes unless EOF/error; returns bytes read. Bounded by
/// `deadlineMs` (< 0 = unbounded) via poll so a wedged child cannot hang us.
size_t readReport(int fd, void* buf, size_t n, int deadlineMs)
{
    size_t got = 0;
    auto* p = static_cast<unsigned char*>(buf);
    while (got < n) {
        struct pollfd pfd{fd, POLLIN, 0};
        const int pr = ::poll(&pfd, 1, deadlineMs < 0 ? -1 : deadlineMs);
        if (pr < 0 && errno == EINTR) continue;
        if (pr <= 0) break;
        const ssize_t r = ::read(fd, p + got, n - got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        got += static_cast<size_t>(r);
    }
    return got;
}

std::string resolveOnPath(const std::string& name)
{
    if (name.find('/') != std::string::npos) return name;
    const char* pathEnv = std::getenv("PATH");
    const std::string pathList = pathEnv ? pathEnv : "/usr/bin:/bin";
    size_t start = 0;
    while (start <= pathList.size()) {
        const size_t colon = pathList.find(':', start);
        const std::string dir = pathList.substr(
            start, colon == std::string::npos ? std::string::npos : colon - start);
        if (!dir.empty()) {
            const std::string cand = dir + "/" + name;
            if (::access(cand.c_str(), X_OK) == 0) return cand;
        }
        if (colon == std::string::npos) break;
        start = colon + 1;
    }
    return name;  // execve will fail with ENOENT and say so
}

// The child's report pipe, close-on-exec on both ends. pipe2() is Linux (and
// the BSDs) only; macOS has no pipe2, so it gets pipe() + FD_CLOEXEC
// (GitHub #388: the macOS build failed on pipe2). That leaves a short window
// in which a concurrent fork+exec elsewhere in the process could inherit the
// write end; the parent's read of the report would then run to its timeout
// instead of seeing EOF when the helper exits.
bool makeCloexecPipe(int pfd[2])
{
#if defined(__linux__)
    return ::pipe2(pfd, O_CLOEXEC) == 0;
#else
    if (::pipe(pfd) != 0) return false;
    for (int i = 0; i < 2; ++i) {
        const int flags = ::fcntl(pfd[i], F_GETFD);
        if (flags < 0 || ::fcntl(pfd[i], F_SETFD, flags | FD_CLOEXEC) != 0) {
            ::close(pfd[0]);
            ::close(pfd[1]);
            pfd[0] = pfd[1] = -1;
            return false;
        }
    }
    return true;
#endif
}

long long monotonicMs()
{
    struct timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<long long>(ts.tv_sec) * 1000LL + ts.tv_nsec / 1000000L;
}

struct rlimit clampedLimit(int resource, rlim_t soft, rlim_t hard)
{
    struct rlimit cur{};
    if (::getrlimit(resource, &cur) == 0 && cur.rlim_max != RLIM_INFINITY) {
        // An unprivileged process cannot raise its hard limit; asking for more
        // than the parent has would make the child's setrlimit fail (EPERM)
        // and the rlimits bit go false for a reason that is not a weakness.
        if (hard > cur.rlim_max) hard = cur.rlim_max;
    }
    if (soft > hard) soft = hard;
    struct rlimit out{};
    out.rlim_cur = soft;
    out.rlim_max = hard;
    return out;
}

/// fork + sandbox + execve `exePath` with `argv`, then wait up to `timeoutMs`
/// (< 0 = no timeout). The ONE implementation behind both isDocumentOpenSafe()
/// and the test seam, so the seam cannot drift from the product.
detail::SandboxedRunResult runSandboxed(const std::string& exePath,
                                        const std::vector<std::string>& argv,
                                        int timeoutMs)
{
    detail::SandboxedRunResult out;

    // --- everything that allocates happens here, before fork() -------------
    std::vector<std::string> args = argv;
    if (args.empty()) args.push_back(exePath);
    std::vector<char*> cargv;
    cargv.reserve(args.size() + 1);
    for (auto& a : args) cargv.push_back(a.data());
    cargv.push_back(nullptr);

    ChildPlan plan;
    plan.path = exePath.c_str();
    plan.argv = cargv.data();
    plan.envp = environ;
    plan.core = clampedLimit(RLIMIT_CORE, 0, 0);
    plan.as = clampedLimit(RLIMIT_AS, static_cast<rlim_t>(kAddressSpaceLimit),
                           static_cast<rlim_t>(kAddressSpaceLimit));
    const rlim_t cpu = static_cast<rlim_t>(cpuSecondsFor(timeoutMs));
    // Soft limit delivers SIGXCPU; the hard limit two seconds later SIGKILL.
    plan.cpu = clampedLimit(RLIMIT_CPU, cpu, cpu + 2);
    plan.nofile = clampedLimit(RLIMIT_NOFILE, kOpenFileLimit, kOpenFileLimit);

#if defined(PDFE_SECCOMP_ARCH)
    std::vector<sock_filter> filter = buildSeccompFilter();
    struct sock_fprog prog{};
    prog.len = static_cast<unsigned short>(filter.size());
    prog.filter = filter.data();
    plan.seccomp = &prog;
#endif

    bool netHandled = false;
    plan.landlockFd = makeWriteDenyRuleset(netHandled);
    plan.landlockNet = netHandled;

    int pfd[2] = {-1, -1};
    if (!makeCloexecPipe(pfd)) {
        if (plan.landlockFd >= 0) ::close(plan.landlockFd);
        return out;  // launched == false
    }
    plan.reportFd = pfd[1];

    const long long startMs = monotonicMs();
    const pid_t pid = ::fork();
    if (pid == 0) {
        sandboxChildAndExec(plan);  // never returns
    }
    ::close(pfd[1]);
    if (plan.landlockFd >= 0) ::close(plan.landlockFd);
    if (pid < 0) {
        ::close(pfd[0]);
        return out;  // launched == false
    }
    out.launched = true;

    // --- the child's own account of what it applied ------------------------
    std::uint32_t mask = 0;
    if (readReport(pfd[0], &mask, sizeof(mask), timeoutMs) == sizeof(mask)) {
        out.maskReported = true;
        out.report = reportFromMask(mask & kAllBits);
        int execErrno = 0;
        const long long left =
            timeoutMs < 0 ? -1 : std::max<long long>(0, timeoutMs - (monotonicMs() - startMs));
        if (readReport(pfd[0], &execErrno, sizeof(execErrno),
                       static_cast<int>(left)) == sizeof(execErrno)) {
            out.execFailed = true;
        }
    }
    ::close(pfd[0]);

    // --- wait, bounded ------------------------------------------------------
    int status = 0;
    for (;;) {
        const pid_t r = ::waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            out.waitStatus = status;
            return out;
        }
        if (r < 0 && errno != EINTR) {
            // Lost the child's verdict after a successful fork: fail CLOSED.
            out.waitFailed = true;
            return out;
        }
        if (timeoutMs >= 0 && monotonicMs() - startMs >= timeoutMs) {
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &status, 0);
            out.timedOut = true;
            out.waitStatus = status;
            return out;
        }
        struct timespec ts{0, 25 * 1000000L};
        ::nanosleep(&ts, nullptr);
    }
}

}  // namespace
#endif  // !_WIN32

#if !defined(_WIN32)
namespace detail {

SandboxedRunResult runUnderPreflightSandboxForTesting(
    const std::vector<std::string>& argv, int timeoutMs)
{
    if (argv.empty()) return {};
    return runSandboxed(resolveOnPath(argv[0]), argv, timeoutMs);
}

int landlockAbiVersion() { return queryLandlockAbi(); }

unsigned long long preflightAddressSpaceLimitBytes() { return kAddressSpaceLimit; }

unsigned long long preflightCpuLimitSecondsFor(int timeoutMs)
{
    return cpuSecondsFor(timeoutMs);
}

}  // namespace detail
#endif

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

    const detail::SandboxedRunResult r =
        runSandboxed(helper.string(), {helper.string(), file.string()}, timeoutMs);
    if (!r.launched)
        return false; // pipe/fork FAILED while the helper IS deployed — fail
                      // CLOSED (same rationale as the Windows launch-failed
                      // branch: a present-but-unlaunchable sandbox must not
                      // silently pass the document it was meant to vet).

    // Reported, never silent: Sandboxed only when the child confirmed every
    // property. EOF without a mask leaves the report all-false (conservative).
    const PosixSandboxReport recorded =
        r.maskReported ? r.report : PosixSandboxReport{};
    g_lastPosixMask.store(maskFromReport(recorded), std::memory_order_release);
    g_lastIsolation.store(detail::isolationForSandboxReport(r.maskReported, r.report),
                          std::memory_order_release);

    if (r.execFailed || r.timedOut || r.waitFailed)
        return false; // exec failed, hung (killed at the timeout), or the
                      // verdict was lost — fail CLOSED in every case.
    // Safe only on a normal exit with code 0 (a signal/crash is unsafe).
    return WIFEXITED(r.waitStatus) && WEXITSTATUS(r.waitStatus) == 0;
#endif
}

} // namespace pdfe::core
