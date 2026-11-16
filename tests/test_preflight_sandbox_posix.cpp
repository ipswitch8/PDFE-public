// -*- coding: utf-8 -*-
// Linux port, phase 6 — the POSIX preflight sandbox.
//
// core::isDocumentOpenSafe() forks pdfe_preflight and, between fork and exec,
// applies rlimits, PR_SET_NO_NEW_PRIVS, a write-denying Landlock ruleset (plus
// TCP bind/connect denial on Landlock ABI >= 4) and a seccomp filter that
// returns EPERM for socket creation and friends. The child reports which of
// those it actually applied.
//
// These tests do not trust that report on its own. The detail:: seam runs an
// ordinary program (python3) under EXACTLY the same sandbox, and each test
// makes the program try the forbidden thing and exit with a code that says
// what happened. Every restriction test has an UNSANDBOXED CONTROL that runs
// the same probe outside the sandbox and must get the opposite answer, so a
// probe that cannot tell the two states apart fails instead of passing.
//
// Linux-only by #if (the mechanisms are Linux kernel features). There is no
// skip anywhere: on Linux the kernel is REQUIRED to offer Landlock ABI >= 4,
// asserted as an explicit precondition, because a sandbox test that quietly
// passes on a kernel without the sandbox is the failure mode it exists for.
#include <gtest/gtest.h>

#include "core/PreflightOpen.h"

// ---------------------------------------------------------------------------
// The report -> HelperIsolation mapping. Pure, so it runs on every platform,
// and every degraded combination is asserted without needing a degraded
// kernel: "a degraded sandbox is reported Unrestricted, never Sandboxed".
// ---------------------------------------------------------------------------
namespace {
using pdfe::core::HelperIsolation;
using pdfe::core::PosixSandboxReport;
using pdfe::core::detail::isolationForSandboxReport;

PosixSandboxReport fullReport()
{
    PosixSandboxReport r;
    r.noNewPrivs = r.rlimits = r.landlockFs = r.landlockNet = r.seccompNet = true;
    return r;
}
}  // namespace

TEST(PreflightSandboxIsolationMapping, FullReportIsSandboxed)
{
    EXPECT_EQ(isolationForSandboxReport(true, fullReport()), HelperIsolation::Sandboxed);
}

TEST(PreflightSandboxIsolationMapping, EverySingleMissingPropertyIsUnrestricted)
{
    bool PosixSandboxReport::*const fields[] = {
        &PosixSandboxReport::noNewPrivs, &PosixSandboxReport::rlimits,
        &PosixSandboxReport::landlockFs, &PosixSandboxReport::landlockNet,
        &PosixSandboxReport::seccompNet,
    };
    const char* names[] = {"noNewPrivs", "rlimits", "landlockFs", "landlockNet",
                           "seccompNet"};
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        PosixSandboxReport r = fullReport();
        r.*fields[i] = false;
        EXPECT_EQ(isolationForSandboxReport(true, r), HelperIsolation::Unrestricted)
            << "missing " << names[i] << " was reported as Sandboxed";
        // And only that one present must not be enough either.
        PosixSandboxReport only;
        only.*fields[i] = true;
        EXPECT_EQ(isolationForSandboxReport(true, only), HelperIsolation::Unrestricted)
            << "only " << names[i] << " was reported as Sandboxed";
    }
}

TEST(PreflightSandboxIsolationMapping, EmptyMaskIsUnrestricted)
{
    EXPECT_EQ(isolationForSandboxReport(true, PosixSandboxReport{}),
              HelperIsolation::Unrestricted);
}

TEST(PreflightSandboxIsolationMapping, NoMaskReportedIsUnrestrictedWhateverTheStructSays)
{
    // EOF before the child wrote its mask: nothing was proven, even if the
    // report struct were (wrongly) populated.
    EXPECT_EQ(isolationForSandboxReport(false, PosixSandboxReport{}),
              HelperIsolation::Unrestricted);
    EXPECT_EQ(isolationForSandboxReport(false, fullReport()),
              HelperIsolation::Unrestricted);
}

TEST(PreflightSandboxIsolationMapping, NeverReportsTheWindowsOnlyValues)
{
    for (int bits = 0; bits < 32; ++bits) {
        PosixSandboxReport r;
        r.noNewPrivs = bits & 1;
        r.rlimits = bits & 2;
        r.landlockFs = bits & 4;
        r.landlockNet = bits & 8;
        r.seccompNet = bits & 16;
        for (bool reported : {false, true}) {
            const auto iso = isolationForSandboxReport(reported, r);
            EXPECT_NE(iso, HelperIsolation::LowIntegrity);
            EXPECT_NE(iso, HelperIsolation::NotLaunched);
            EXPECT_EQ(iso == HelperIsolation::Sandboxed, reported && bits == 31)
                << "bits=" << bits << " reported=" << reported;
        }
    }
}

#if defined(__linux__)

#include <spawn.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

extern char** environ;

namespace fs = std::filesystem;
using pdfe::core::detail::SandboxedRunResult;
using pdfe::core::detail::runUnderPreflightSandboxForTesting;

namespace {

fs::path fixtures() { return fs::path(TEST_FIXTURES_DIR); }

fs::path scratchDir(const std::string& tag)
{
    std::error_code ec;
    fs::path d = fs::temp_directory_path(ec)
                 / ("pdfe_p6_sandbox_" + tag + "_" + std::to_string(::getpid()));
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

struct DirCleaner {
    fs::path dir;
    ~DirCleaner()
    {
        std::error_code ec;
        if (!dir.empty()) fs::remove_all(dir, ec);
    }
};

std::vector<std::string> py(const std::string& code,
                            const std::vector<std::string>& extra = {})
{
    std::vector<std::string> v{"python3", "-I", "-S", "-c", code};
    v.insert(v.end(), extra.begin(), extra.end());
    return v;
}

/// Exit code of `argv` run WITHOUT the sandbox (the control arm), or -1.
int runUnsandboxed(const std::vector<std::string>& argv)
{
    std::vector<std::string> args = argv;
    std::vector<char*> c;
    for (auto& a : args) c.push_back(a.data());
    c.push_back(nullptr);
    pid_t pid = 0;
    if (::posix_spawnp(&pid, c[0], nullptr, nullptr, c.data(), environ) != 0)
        return -1;
    int st = 0;
    if (::waitpid(pid, &st, 0) != pid) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/// The child ran, exited normally, with `code`.
::testing::AssertionResult exitedWith(const SandboxedRunResult& r, int code)
{
    if (!r.launched) return ::testing::AssertionFailure() << "fork failed";
    if (!r.maskReported)
        return ::testing::AssertionFailure() << "child never reported its mask";
    if (r.execFailed) return ::testing::AssertionFailure() << "execve failed";
    if (r.timedOut) return ::testing::AssertionFailure() << "timed out";
    if (!WIFEXITED(r.waitStatus)) {
        return ::testing::AssertionFailure()
               << "did not exit normally (signal "
               << (WIFSIGNALED(r.waitStatus) ? WTERMSIG(r.waitStatus) : -1) << ")";
    }
    if (WEXITSTATUS(r.waitStatus) != code) {
        return ::testing::AssertionFailure()
               << "exit code " << WEXITSTATUS(r.waitStatus) << ", expected " << code;
    }
    return ::testing::AssertionSuccess();
}

/// Every property must be reported applied on the hosts this suite runs on.
::testing::AssertionResult fullyApplied(const SandboxedRunResult& r)
{
    const auto& p = r.report;
    if (p.all()) return ::testing::AssertionSuccess();
    return ::testing::AssertionFailure()
           << "sandbox degraded: noNewPrivs=" << p.noNewPrivs
           << " rlimits=" << p.rlimits << " landlockFs=" << p.landlockFs
           << " landlockNet=" << p.landlockNet << " seccompNet=" << p.seccompNet;
}

constexpr int kTimeoutMs = 20000;

/// This process's own "NoNewPrivs:" value from /proc/self/status ("" if absent).
std::string selfNoNewPrivs()
{
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.rfind("NoNewPrivs:", 0) == 0) {
            const auto v = line.find_first_not_of(" \t", 11);
            return v == std::string::npos ? std::string() : line.substr(v);
        }
    }
    return {};
}

}  // namespace

// ---------------------------------------------------------------------------
// Precondition, stated as a test of its own so a failure names the cause.
// ---------------------------------------------------------------------------
TEST(PreflightSandboxPosix, KernelOffersLandlockAbiWithNetworkRules)
{
    // Kernel 6.7+ offers ABI 4 (TCP bind/connect). The release host runs 6.11
    // (ABI 5). A kernel below this is a deployment where the sandbox degrades —
    // reported by lastHelperIsolation() == Unrestricted — and the suite must say
    // so loudly rather than pass the assertions below vacuously.
    ASSERT_GE(pdfe::core::detail::landlockAbiVersion(), 4)
        << "this kernel does not offer Landlock ABI >= 4 (disabled at boot or "
           "too old); the preflight sandbox would run DEGRADED here";
}

// ---------------------------------------------------------------------------
// (a) End to end: the real helper on a real fixture.
// ---------------------------------------------------------------------------
TEST(PreflightSandboxPosix, RealHelperOpensFixtureUnderTheFullSandbox)
{
    ASSERT_GE(pdfe::core::detail::landlockAbiVersion(), 4)
        << "precondition: Landlock ABI >= 4";
    const fs::path clean = fixtures() / "test_single.pdf";
    ASSERT_TRUE(fs::exists(clean)) << clean;

    EXPECT_TRUE(pdfe::core::isDocumentOpenSafe(clean))
        << "the sandboxed helper refused a known-good document: a restriction "
           "broke the helper's ability to read and parse it";

    EXPECT_EQ(pdfe::core::lastHelperIsolation(),
              pdfe::core::HelperIsolation::Sandboxed)
        << "the helper ran, but NOT under every sandbox property";
    const auto rep = pdfe::core::lastPosixSandboxReport();
    EXPECT_TRUE(rep.noNewPrivs);
    EXPECT_TRUE(rep.rlimits);
    EXPECT_TRUE(rep.landlockFs);
    EXPECT_TRUE(rep.landlockNet);
    EXPECT_TRUE(rep.seccompNet);
}

TEST(PreflightSandboxPosix, RealHelperStillRefusesGarbageUnderTheSandbox)
{
    // Non-vacuity for the test above: a sandbox that made the helper exit 0
    // regardless would pass it. Garbage must still be refused.
    const fs::path d = scratchDir("garbage");
    DirCleaner clean{d};
    const fs::path junk = d / "junk.pdf";
    {
        std::ofstream f(junk, std::ios::binary);
        f << std::string(4096, '\x7f') << "NOT-A-PDF";
    }
    EXPECT_FALSE(pdfe::core::isDocumentOpenSafe(junk));
    EXPECT_EQ(pdfe::core::lastHelperIsolation(),
              pdfe::core::HelperIsolation::Sandboxed);
}

// ---------------------------------------------------------------------------
// (b) Each restriction, proved with an ordinary program under the same sandbox.
// ---------------------------------------------------------------------------

TEST(PreflightSandboxPosix, NoNewPrivsAndSeccompAreVisibleInTheChildsProcStatus)
{
    // exit 0 iff /proc/self/status says NoNewPrivs 1 AND Seccomp 2 (filter).
    const std::string code =
        "import sys\n"
        "s=open('/proc/self/status').read().split('\\n')\n"
        "d=dict(l.split(':',1) for l in s if ':' in l)\n"
        "ok=d['NoNewPrivs'].strip()=='1' and d['Seccomp'].strip()=='2'\n"
        "sys.exit(0 if ok else 1)\n";
    const auto r = runUnderPreflightSandboxForTesting(py(code), kTimeoutMs);
    EXPECT_TRUE(exitedWith(r, 0));
    EXPECT_TRUE(fullyApplied(r));
    // Control. Only discriminating when the runner itself is not already under
    // no_new_privs (a container or a systemd NoNewPrivileges unit would be);
    // that state is inherited and would make the two arms agree. That case is
    // a FAILURE with the reason, not a skip.
    if (selfNoNewPrivs() == "0") {
        EXPECT_EQ(runUnsandboxed(py(code)), 1)
            << "control: the probe reported NoNewPrivs/seccomp set OUTSIDE the "
               "sandbox, so it cannot discriminate";
    } else {
        ADD_FAILURE() << "the test runner already has NoNewPrivs="
                      << selfNoNewPrivs()
                      << ", so the unsandboxed control cannot discriminate";
    }
}

TEST(PreflightSandboxPosix, WritingANewFileIsDenied)
{
    const fs::path d = scratchDir("write");
    DirCleaner clean{d};
    const fs::path target = d / "written_by_sandboxed_child.txt";
    // 0 = PermissionError (denied), 1 = the write succeeded, 2 = other error.
    const std::string code =
        "import sys\n"
        "try:\n"
        "    open(sys.argv[1],'w').write('x')\n"
        "    sys.exit(1)\n"
        "except PermissionError:\n"
        "    sys.exit(0)\n"
        "except OSError:\n"
        "    sys.exit(2)\n";
    const auto r = runUnderPreflightSandboxForTesting(py(code, {target.string()}),
                                                      kTimeoutMs);
    EXPECT_TRUE(exitedWith(r, 0)) << "the sandboxed child could create " << target;
    EXPECT_FALSE(fs::exists(target));

    const fs::path control = d / "written_by_control.txt";
    EXPECT_EQ(runUnsandboxed(py(code, {control.string()})), 1)
        << "control: the write failed even without the sandbox";
    EXPECT_TRUE(fs::exists(control));
}

TEST(PreflightSandboxPosix, ModifyingOrTruncatingAnExistingFileIsDenied)
{
    const fs::path d = scratchDir("modify");
    DirCleaner clean{d};
    const fs::path victim = d / "existing.txt";
    { std::ofstream f(victim, std::ios::binary); f << "ORIGINAL"; }
    const std::string code =
        "import os,sys\n"
        "denied=0\n"
        "for mode in ('r+','a','w'):\n"
        "    try:\n"
        "        open(sys.argv[1],mode).write('EVIL')\n"
        "    except PermissionError:\n"
        "        denied+=1\n"
        "try:\n"
        "    os.unlink(sys.argv[1])\n"
        "except PermissionError:\n"
        "    denied+=1\n"
        "sys.exit(0 if denied==4 else 1)\n";
    const auto r = runUnderPreflightSandboxForTesting(py(code, {victim.string()}),
                                                      kTimeoutMs);
    EXPECT_TRUE(exitedWith(r, 0));
    std::ifstream in(victim, std::ios::binary);
    const std::string after((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
    EXPECT_EQ(after, "ORIGINAL") << "the sandboxed child changed an existing file";
}

TEST(PreflightSandboxPosix, CreatingAnInetSocketFailsWithEperm)
{
    // 0 = socket() failed with EPERM, 1 = it succeeded, 2 = other errno.
    const std::string code =
        "import errno,socket,sys\n"
        "try:\n"
        "    socket.socket(socket.AF_INET, socket.SOCK_STREAM).close()\n"
        "    sys.exit(1)\n"
        "except OSError as e:\n"
        "    sys.exit(0 if e.errno==errno.EPERM else 2)\n";
    const auto r = runUnderPreflightSandboxForTesting(py(code), kTimeoutMs);
    EXPECT_TRUE(exitedWith(r, 0));
    EXPECT_EQ(runUnsandboxed(py(code)), 1)
        << "control: AF_INET socket creation failed even without the sandbox";
}

TEST(PreflightSandboxPosix, UnixSocketpairIsAlsoDenied)
{
    // Landlock's network rules cover TCP only; AF_UNIX is denied by seccomp.
    const std::string code =
        "import errno,socket,sys\n"
        "try:\n"
        "    a,b=socket.socketpair(); a.close(); b.close()\n"
        "    sys.exit(1)\n"
        "except OSError as e:\n"
        "    sys.exit(0 if e.errno==errno.EPERM else 2)\n";
    const auto r = runUnderPreflightSandboxForTesting(py(code), kTimeoutMs);
    EXPECT_TRUE(exitedWith(r, 0));
    EXPECT_EQ(runUnsandboxed(py(code)), 1);
}

// ---------------------------------------------------------------------------
// Raw-syscall probes (python3 ctypes). The probe exits with the call's errno,
// or 100 if the call SUCCEEDED. EPERM (1) is the seccomp verdict. Every probe
// has an unsandboxed control that must NOT see EPERM, so the EPERM is the
// filter's and not the kernel's own answer to the arguments.
// ---------------------------------------------------------------------------
namespace {
constexpr int kProbeSucceeded = 100;

std::vector<std::string> syscallProbe(const std::string& callExpr)
{
    const std::string code =
        "import ctypes,sys\n"
        "libc=ctypes.CDLL(None,use_errno=True)\n"
        "buf=ctypes.create_string_buffer(256)\n"
        "nr=int(sys.argv[1])\n"
        "r=" + callExpr + "\n"
        "sys.exit(100 if r>=0 else ctypes.get_errno())\n";
    return py(code);
}

std::vector<std::string> withNr(std::vector<std::string> argv, long nr)
{
    argv.push_back(std::to_string(nr));
    return argv;
}
}  // namespace

#if defined(__NR_io_uring_setup) && defined(__NR_io_uring_enter) \
    && defined(__NR_io_uring_register)
TEST(PreflightSandboxPosix, IoUringIsDeniedSoItCannotCreateSockets)
{
    // Since Linux 5.19 IORING_OP_SOCKET creates a socket without socket(2),
    // which neither the socket entry in the filter nor Landlock's TCP rules
    // would see. So the whole io_uring interface is denied.
    const auto setup = syscallProbe("libc.syscall(nr,1,ctypes.byref(buf))");

    // PRECONDITION, asserted not skipped: io_uring must be available to an
    // ordinary process on this host, or the control cannot discriminate. On a
    // host with kernel.io_uring_disabled=2 (or 1 without the io_uring group)
    // the control itself sees EPERM and this fails with that reason.
    const int control = runUnsandboxed(withNr(setup, __NR_io_uring_setup));
    ASSERT_EQ(control, kProbeSucceeded)
        << "control: io_uring_setup failed OUTSIDE the sandbox (errno " << control
        << "); check kernel.io_uring_disabled — this host cannot prove the denial";

    const auto r = runUnderPreflightSandboxForTesting(
        withNr(setup, __NR_io_uring_setup), kTimeoutMs);
    EXPECT_TRUE(exitedWith(r, EPERM)) << "io_uring_setup was not denied with EPERM";

    // enter/register on fd -1: the kernel's own answer is EBADF; the filter's
    // is EPERM, decided before the fd is ever looked at.
    const auto enter = syscallProbe("libc.syscall(nr,-1,0,0,0,None,0)");
    EXPECT_EQ(runUnsandboxed(withNr(enter, __NR_io_uring_enter)), EBADF);
    EXPECT_TRUE(exitedWith(runUnderPreflightSandboxForTesting(
                               withNr(enter, __NR_io_uring_enter), kTimeoutMs),
                           EPERM));
    const auto reg = syscallProbe("libc.syscall(nr,-1,0,None,0)");
    EXPECT_EQ(runUnsandboxed(withNr(reg, __NR_io_uring_register)), EBADF);
    EXPECT_TRUE(exitedWith(runUnderPreflightSandboxForTesting(
                               withNr(reg, __NR_io_uring_register), kTimeoutMs),
                           EPERM));
}
#endif

#if defined(__NR_connect)
TEST(PreflightSandboxPosix, ConnectIsDeniedIndependentlyOfSocket)
{
    // connect(-1, NULL, 0): EBADF from the kernel, EPERM from the filter.
    const auto probe = withNr(syscallProbe("libc.syscall(nr,-1,None,0)"), __NR_connect);
    EXPECT_EQ(runUnsandboxed(probe), EBADF);
    EXPECT_TRUE(exitedWith(runUnderPreflightSandboxForTesting(probe, kTimeoutMs), EPERM));
}
#endif

#if defined(__NR_ptrace)
TEST(PreflightSandboxPosix, PtraceIsDenied)
{
    // PTRACE_PEEKUSER (3) on a pid that does not exist: ESRCH from the kernel,
    // EPERM from the filter.
    const auto probe =
        withNr(syscallProbe("libc.syscall(nr,3,0x7ffffff0,0,0)"), __NR_ptrace);
    EXPECT_EQ(runUnsandboxed(probe), ESRCH);
    EXPECT_TRUE(exitedWith(runUnderPreflightSandboxForTesting(probe, kTimeoutMs), EPERM));
}
#endif

TEST(PreflightSandboxPosix, AddressSpaceIsCappedAndAnAllocationBeyondItFails)
{
    const unsigned long long cap =
        pdfe::core::detail::preflightAddressSpaceLimitBytes();
    ASSERT_GT(cap, 0ull);
    // 0 = limit is exactly the cap AND allocating cap+1 GiB raises MemoryError.
    const std::string code =
        "import resource,sys\n"
        "cap=int(sys.argv[1])\n"
        "if resource.getrlimit(resource.RLIMIT_AS)!=(cap,cap): sys.exit(3)\n"
        "try:\n"
        "    b=bytearray(cap+(1<<30))\n"
        "    sys.exit(1)\n"
        "except MemoryError:\n"
        "    sys.exit(0)\n";
    const auto r = runUnderPreflightSandboxForTesting(
        py(code, {std::to_string(cap)}), kTimeoutMs);
    EXPECT_TRUE(exitedWith(r, 0));

    // Control (limit only — allocating cap+1 GiB unsandboxed would really
    // commit that memory): outside the sandbox RLIMIT_AS is not the cap.
    const std::string limitOnly =
        "import resource,sys\n"
        "cap=int(sys.argv[1])\n"
        "sys.exit(0 if resource.getrlimit(resource.RLIMIT_AS)==(cap,cap) else 1)\n";
    EXPECT_EQ(runUnsandboxed(py(limitOnly, {std::to_string(cap)})), 1);
}

TEST(PreflightSandboxPosix, CoreDumpsAreDisabledAndOpenFilesAreBounded)
{
    const std::string code =
        "import resource,sys\n"
        "c=resource.getrlimit(resource.RLIMIT_CORE)\n"
        "n=resource.getrlimit(resource.RLIMIT_NOFILE)\n"
        "sys.exit(0 if c==(0,0) and n[0]<=256 and n[1]<=256 else 1)\n";
    const auto r = runUnderPreflightSandboxForTesting(py(code), kTimeoutMs);
    EXPECT_TRUE(exitedWith(r, 0));
}

TEST(PreflightSandboxPosix, CpuLimitTracksTheTimeout)
{
    const int timeoutMs = 3000;
    const unsigned long long cpu =
        pdfe::core::detail::preflightCpuLimitSecondsFor(timeoutMs);
    ASSERT_GE(cpu, 3ull);
    ASSERT_LE(cpu, 10ull);
    const std::string code =
        "import resource,sys\n"
        "s,h=resource.getrlimit(resource.RLIMIT_CPU)\n"
        "sys.exit(0 if s==int(sys.argv[1]) and h==int(sys.argv[1])+2 else 1)\n";
    const auto r = runUnderPreflightSandboxForTesting(
        py(code, {std::to_string(cpu)}), timeoutMs);
    EXPECT_TRUE(exitedWith(r, 0));
}

TEST(PreflightSandboxPosix, ACpuSpinIsTerminated)
{
    const auto t0 = std::chrono::steady_clock::now();
    const auto r = runUnderPreflightSandboxForTesting(
        py("while True: pass\n"), /*timeoutMs=*/1500);
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::steady_clock::now() - t0).count();
    ASSERT_TRUE(r.launched);
    ASSERT_TRUE(r.maskReported);
    // Terminated by the wall-clock timeout (SIGKILL from the parent) or by the
    // CPU limit (SIGXCPU, then SIGKILL at the hard limit) — never an exit.
    EXPECT_TRUE(WIFSIGNALED(r.waitStatus))
        << "the spinning child was not killed by a signal";
    if (WIFSIGNALED(r.waitStatus)) {
        const int sig = WTERMSIG(r.waitStatus);
        EXPECT_TRUE(sig == SIGKILL || sig == SIGXCPU) << "signal " << sig;
    }
    EXPECT_TRUE(r.timedOut || (WIFSIGNALED(r.waitStatus)
                               && WTERMSIG(r.waitStatus) == SIGXCPU));
    EXPECT_LT(secs, 15) << "the spin outlived both bounds";
}

TEST(PreflightSandboxPosix, ReadingTheInputFileStillWorks)
{
    // The helper's whole job is to READ the document. Prove a sandboxed child
    // can read a fixture byte-for-byte (size and a prefix match).
    const fs::path clean = fixtures() / "test_single.pdf";
    ASSERT_TRUE(fs::exists(clean));
    const auto size = fs::file_size(clean);
    const std::string code =
        "import sys\n"
        "b=open(sys.argv[1],'rb').read()\n"
        "sys.exit(0 if len(b)==int(sys.argv[2]) and b[:5]==b'%PDF-' else 1)\n";
    const auto r = runUnderPreflightSandboxForTesting(
        py(code, {clean.string(), std::to_string(size)}), kTimeoutMs);
    EXPECT_TRUE(exitedWith(r, 0));
    EXPECT_TRUE(fullyApplied(r));
}

TEST(PreflightSandboxPosix, ExecFailureIsReportedNotMistakenForAVerdict)
{
    const fs::path d = scratchDir("noexec");
    DirCleaner clean{d};
    const auto r = runUnderPreflightSandboxForTesting(
        {(d / "does_not_exist").string()}, kTimeoutMs);
    ASSERT_TRUE(r.launched);
    EXPECT_TRUE(r.maskReported);
    EXPECT_TRUE(r.execFailed);
    EXPECT_TRUE(WIFEXITED(r.waitStatus) && WEXITSTATUS(r.waitStatus) == 127);
}

TEST(PreflightSandboxPosix, TheSandboxDoesNotLeakIntoTheParent)
{
    // The restrictions are applied after fork(), in the child only. If any of
    // them had been applied to THIS process, the gtest binary could not write
    // its own files or open sockets for the remaining tests.
    const std::string nnpBefore = selfNoNewPrivs();
    ASSERT_FALSE(nnpBefore.empty()) << "no NoNewPrivs line in /proc/self/status";
    const auto r = runUnderPreflightSandboxForTesting(py("pass\n"), kTimeoutMs);
    EXPECT_TRUE(exitedWith(r, 0));
    EXPECT_EQ(selfNoNewPrivs(), nnpBefore)
        << "running the sandbox changed THIS process's no_new_privs state";
    const fs::path d = scratchDir("parent");
    DirCleaner clean{d};
    const fs::path p = d / "parent_can_write.txt";
    { std::ofstream f(p); f << "ok"; }
    EXPECT_TRUE(fs::exists(p)) << "the parent lost the ability to create files";
}

#endif  // __linux__
