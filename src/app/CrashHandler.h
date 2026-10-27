#pragma once
#include <QString>
#include <cstdint>
#include <functional>

namespace pdfe::app {

class CrashHandler {
public:
    static CrashHandler& instance();

    /// Install global exception handlers: std::set_terminate, the Windows
    /// top-level SEH filter, and (WIN-82 / GitHub #103) the two CRT handlers
    /// that own the fault kinds neither of those can see —
    /// `_set_purecall_handler` and `_set_invalid_parameter_handler`.
    ///
    /// A pure-virtual call does NOT raise a structured exception and does NOT
    /// go through std::terminate: the compiler emits a call to `_purecall`,
    /// whose default CRT implementation reports and exits. Likewise an invalid
    /// CRT parameter is dispatched through `_invalid_parameter`, not through
    /// SEH. Before those two installs, both fault kinds killed PDFE leaving no
    /// minidump at all.
    void install();

    /// Uninstall (restore previous handlers).
    void uninstall();

    /// Show a crash dialog with the given error info.
    static void showCrashDialog(const QString& title, const QString& details);

    /// WIN-82 REPORTING SEAM — "if the security-relevant effect is invisible in
    /// observable state, the call must REPORT it" (pipeline_crashhandler_p64).
    ///
    /// install() sets four PROCESS-GLOBAL handlers, and every one of them is
    /// otherwise invisible: `installed_` is a private bool that says only "we
    /// ran install() once", not "the CRT is actually pointing at us". A later
    /// `_set_purecall_handler` anywhere in the process — including a static
    /// initialiser in a third-party library — silently displaces ours and
    /// nothing observable changes. This reads the handlers BACK OUT of the CRT
    /// and reports which of them are genuinely ours right now.
    ///
    /// It is also what makes the WIN-82 ordering requirement checkable rather
    /// than asserted: a child process about to make a pure-virtual call can
    /// confirm the purecall handler is in place FIRST, in the very process that
    /// is going to fault.
    struct InstalledHandlers {
        bool terminate = false;
        bool unhandledExceptionFilter = false;
        bool purecall = false;
        bool invalidParameter = false;

        bool all() const {
            return terminate && unhandledExceptionFilter && purecall
                && invalidParameter;
        }
    };

    /// Which of the four global handlers are, right now, CrashHandler's own.
    /// Reads them back out of the CRT / Win32 rather than trusting install().
    static InstalledHandlers installedHandlers();

    /// For testing: set a callback instead of showing a dialog.
    using CrashCallback = std::function<void(const QString& title, const QString& details)>;
    void setTestCallback(CrashCallback cb);

    /// Get the last crash info (for testing).
    struct CrashInfo {
        QString title;
        QString details;
    };
    CrashInfo lastCrashInfo() const;

    /// True when the process is running under automation (a UI-E2E harness or
    /// CI), in which case a crash must NOT pop a modal dialog (it would block
    /// pywinauto until timeout) — it writes a minidump and exits fast instead.
    /// Detected via env: PDFE_UI_TEST, PDFE_HEADLESS, or CI (GitHub Actions sets
    /// CI=true, so this works in CI with no harness change).
    static bool isAutomatedRun();

    /// Test seam: write a minidump of the CURRENT thread state (no live
    /// exception) to the resolved crash-dump directory and return its path
    /// (empty on failure). Exercises the exact MiniDumpWriteDump/DbgHelp
    /// plumbing the crash path uses, so a test can assert a real .dmp is
    /// produced without triggering an actual access violation.
    static QString writeTestMinidump();

    // -----------------------------------------------------------------------
    // Phase-11 seams. These exist so the retention and validation policy can be
    // TESTED rather than described. Each returns the value the crash path
    // itself uses, so a test cannot drift from the product by hard-coding a
    // constant that later changes (the failure mode DCA 0028 records).
    // -----------------------------------------------------------------------

    /// The retained-dump cap the crash path enforces. Exposed so a test asserts
    /// pruning against the REAL threshold instead of a copy of the number.
    static int maxCrashDumps();

    /// Pure predicate: would this value be accepted as PDFE_CRASH_DUMP_DIR?
    /// Rejects UNC, device-namespace, drive-relative and relative paths, and any
    /// ".." segment — see the rationale block in CrashHandler.cpp. No filesystem
    /// access, so it is safe to call from anywhere and cheap to test.
    static bool isAcceptableCrashDumpDirOverride(const QString& dir);

    /// Resolve (creating and securing if needed) the crash-dump directory the
    /// crash path would use right now, honouring PDFE_CRASH_DUMP_DIR. Empty on
    /// failure — including when an owner-only ACL could not be applied.
    static QString resolvedCrashDumpDir();

    /// Delete oldest-first until at most `keep` dumps remain in `dir`; returns
    /// the number deleted. Exposed so the retention policy is testable without
    /// writing 150 real (~48 MB) minidumps.
    static int pruneCrashDumps(const QString& dir, int keep);

private:
    CrashHandler();
    ~CrashHandler();
    CrashHandler(const CrashHandler&) = delete;
    CrashHandler& operator=(const CrashHandler&) = delete;

    static void terminateHandler();

    CrashCallback testCallback_;
    std::terminate_handler previousTerminateHandler_ = nullptr;
    CrashInfo lastCrashInfo_;
    bool installed_ = false;

#ifdef _WIN32
    static long __stdcall sehHandler(struct _EXCEPTION_POINTERS* exInfo);
    void* previousSehHandler_ = nullptr;

    /// WIN-82. `_purecall` — a virtual call dispatched during construction or
    /// destruction of a base whose override no longer exists.
    static void __cdecl purecallHandler();

    /// WIN-82. The CRT's invalid-parameter dispatch (a bad fd, a null buffer to
    /// a _s function, ...). Signature fixed by `_invalid_parameter_handler`.
    static void __cdecl invalidParameterHandler(const wchar_t* expression,
                                                const wchar_t* function,
                                                const wchar_t* file,
                                                unsigned int line,
                                                std::uintptr_t reserved);

    /// Previous CRT handlers, held as void* so this header need not drag in
    /// <stdlib.h>'s handler typedefs (same convention as previousSehHandler_).
    void* previousPurecallHandler_ = nullptr;
    void* previousInvalidParameterHandler_ = nullptr;

    /// Shared tail for the two CRT faults above: minidump, record, route the
    /// notification exactly as the SEH/terminate paths do, then die. Never
    /// returns — the CRT contract for both handlers is that execution must not
    /// resume, and returning from `_purecall` would re-enter the faulting call.
    [[noreturn]] static void reportCrtFaultAndDie(const QString& title,
                                                  const QString& details);

    /// Write a full-context minidump. `exInfo` may be null (e.g. std::terminate
    /// or the test seam), in which case the current thread context is captured.
    /// Returns the dump path written, or an empty string on failure.
    static QString writeMinidump(struct _EXCEPTION_POINTERS* exInfo);
#endif
};

} // namespace pdfe::app
