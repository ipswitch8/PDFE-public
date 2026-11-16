// -*- coding: utf-8 -*-
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace pdfe::core {

/// Out-of-process crash-isolation check for opening a document.
///
/// Spawns the `pdfe_preflight` helper (located next to the running
/// executable) as a child process that attempts to open `file` with MuPDF.
/// A hostile document can drive MuPDF's parser/repair into unbounded
/// (effectively cyclic) recursion that exhausts the stack — an
/// unrecoverable crash that cannot be safely trapped in-process. By
/// attempting the open in a disposable child first, such a crash terminates
/// only the helper; this function observes the abnormal exit and returns
/// false, so the caller can refuse the document while staying healthy.
///
/// Returns true ONLY when the helper opened and walked the document cleanly
/// (exit code 0). Any other outcome — an ordinary MuPDF "malformed file"
/// error, an abnormal/crash termination, or a timeout — returns false, on
/// the conservative principle that a document the isolated opener could not
/// cleanly handle should not be opened in-process.
///
/// `timeoutMs` bounds a document that hangs the helper (e.g. a pathological
/// loop) rather than crashing it.
///
/// Failure modes distinguish two cases:
///   * If the running executable's own directory CANNOT be resolved (so we
///     have no idea where the helper would live), this returns false —
///     fail CLOSED. Refusing to open the document is the safe outcome: an
///     unresolvable executable location means the sandbox is effectively
///     disabled, and silently treating every document as safe would defeat
///     the security control. This case previously fail-opened on macOS
///     because executableDir() had no Apple branch (it does now).
///   * If the directory resolves but the helper binary is simply absent
///     there (a deployment/packaging gap), this returns true — fail OPEN —
///     so a missing helper does not make the app unable to open any file;
///     the in-process open then applies its normal error handling.
bool isDocumentOpenSafe(const std::filesystem::path& file, int timeoutMs = 15000);

/// How much the last helper launch was actually restricted (GitHub #59,
/// SEC-61).
///
/// The helper parses attacker-controlled bytes, so it is created with a
/// LOW-INTEGRITY primary token: a low-integrity process cannot write to
/// medium-integrity objects (the user's profile, the registry hives the app
/// uses, or the parent process), so a code-execution bug in MuPDF's parser is
/// contained rather than merely relocated to another process.
///
/// This exists because the degradation would otherwise be INVISIBLE. If token
/// duplication or `CreateProcessAsUser` fails, the code falls back to an
/// ordinary launch — the helper still provides crash isolation, so failing the
/// open would be a worse trade — but that fallback silently removes a security
/// property. An effect that matters and cannot be observed cannot be tested
/// or audited, so it is reported here instead.
///
/// LINUX (phase 6 of the Linux port). There is no integrity-level model, so the
/// helper is restricted with a different set of kernel mechanisms, applied in
/// the forked child between fork() and exec():
///   * resource limits (RLIMIT_CORE=0, RLIMIT_AS, RLIMIT_CPU, RLIMIT_NOFILE);
///   * PR_SET_NO_NEW_PRIVS;
///   * a Landlock ruleset that handles every filesystem WRITE-type access and
///     grants none, so the helper can read (it must read the document) but
///     cannot create, modify, truncate, rename or delete anything;
///   * on Landlock ABI >= 4, the same ruleset also denies TCP bind/connect;
///   * a seccomp filter returning EPERM for socket, socketpair, connect, bind,
///     listen, accept, accept4, sendto, sendmsg, sendmmsg, ptrace,
///     process_vm_readv, process_vm_writev, io_uring_setup, io_uring_enter
///     and io_uring_register (io_uring can create sockets without socket(2)).
/// Which of those were ACTUALLY applied is written back by the child before it
/// execs and reported through lastPosixSandboxReport(). `Sandboxed` means all
/// of them; `Unrestricted` means at least one was not (old kernel, Landlock
/// disabled, seccomp unavailable) and the helper ran with crash isolation
/// only for the missing parts. The degraded case still runs the preflight, the
/// same trade the Windows arm makes for a failed token.
///
/// Values are per-process and reflect the MOST RECENT launch only.
enum class HelperIsolation {
    NotLaunched,    ///< no helper launch has happened in this process
    LowIntegrity,   ///< Windows: created with a low-integrity primary token (intended)
    Unrestricted,   ///< fell back to an ordinary / partially restricted launch (degraded)
    Sandboxed       ///< POSIX: every PosixSandboxReport property applied (intended)
};

/// Isolation actually achieved by the most recent helper launch.
/// Windows reports NotLaunched / LowIntegrity / Unrestricted; Linux reports
/// NotLaunched / Sandboxed / Unrestricted. Never LowIntegrity off Windows and
/// never Sandboxed on Windows.
HelperIsolation lastHelperIsolation();

/// The individual POSIX sandbox properties the child reported as applied
/// (each is true only if the corresponding syscall returned success in the
/// child itself). All false on Windows and before the first POSIX launch.
struct PosixSandboxReport {
    bool noNewPrivs = false;   ///< prctl(PR_SET_NO_NEW_PRIVS, 1) succeeded
    bool rlimits = false;      ///< every setrlimit (CORE, AS, CPU, NOFILE) succeeded
    bool landlockFs = false;   ///< landlock_restrict_self with all FS write rights handled
    bool landlockNet = false;  ///< ...and that ruleset also handled TCP bind/connect (ABI >= 4)
    bool seccompNet = false;   ///< the seccomp network/ptrace deny filter is installed

    bool all() const {
        return noNewPrivs && rlimits && landlockFs && landlockNet && seccompNet;
    }
};

/// Report for the most recent POSIX helper launch (see PosixSandboxReport).
PosixSandboxReport lastPosixSandboxReport();

namespace detail {

/// Test seam. Overrides the directory that isDocumentOpenSafe() treats as the
/// running executable's location (where it looks for the pdfe_preflight
/// helper). Engaging it with an EMPTY path simulates an UNRESOLVABLE
/// executable location, exercising the fail-closed branch on any host —
/// including hosts where the real resolution always succeeds. Passing
/// std::nullopt restores normal OS-based resolution. For tests only; not part
/// of the shipping contract.
void setExecutableDirForTesting(std::optional<std::filesystem::path> dir);

/// Outcome of a launch through the POSIX preflight sandbox.
struct SandboxedRunResult {
    bool launched = false;     ///< fork() succeeded
    bool maskReported = false; ///< the child wrote its applied-properties mask
    bool execFailed = false;   ///< the child reported that execve() failed
    bool timedOut = false;     ///< killed by the parent at timeoutMs
    bool waitFailed = false;   ///< waitpid() errored; verdict unknown
    int waitStatus = 0;        ///< raw waitpid() status (valid if launched && !waitFailed)
    PosixSandboxReport report; ///< all false unless maskReported
};

/// The pure mapping isDocumentOpenSafe() uses to turn the child's report into
/// lastHelperIsolation(): Sandboxed only when the mask was reported AND every
/// PosixSandboxReport property is true; Unrestricted for any missing property
/// and for a child that never reported (EOF without a mask). Pure, so every
/// degraded combination is unit-testable without a degraded kernel.
HelperIsolation isolationForSandboxReport(bool maskReported,
                                          const PosixSandboxReport& report);

#if !defined(_WIN32)
/// Test seam. Runs `argv` (argv[0] is resolved against PATH when it contains
/// no '/') under EXACTLY the sandbox isDocumentOpenSafe() applies to
/// pdfe_preflight, with the same timeout handling, and returns the raw wait
/// status plus the per-property report. Lets a test prove each restriction
/// with ordinary tools (sh, python3) instead of trusting the report bits.
/// Does not touch lastHelperIsolation() / lastPosixSandboxReport().
SandboxedRunResult runUnderPreflightSandboxForTesting(
    const std::vector<std::string>& argv, int timeoutMs);

/// The Landlock ABI version this kernel offers (landlock_create_ruleset with
/// LANDLOCK_CREATE_RULESET_VERSION), or a value <= 0 when Landlock is not
/// available (not compiled in, disabled at boot, or headers missing at build).
int landlockAbiVersion();

/// RLIMIT_AS cap applied to the helper, in bytes.
unsigned long long preflightAddressSpaceLimitBytes();

/// RLIMIT_CPU soft limit (seconds) applied for a given timeout.
unsigned long long preflightCpuLimitSecondsFor(int timeoutMs);
#endif

} // namespace detail

} // namespace pdfe::core
