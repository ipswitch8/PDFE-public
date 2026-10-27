#include <gtest/gtest.h>
#include <QApplication>

#include "security_oracles.h"  // pdfe_test::testProcessJob() — ProcessSpawnGuard scoping

#ifdef _WIN32
  #include <crtdbg.h>
  #include <windows.h>
  #include <cstdlib>
#endif

#ifdef _WIN32
// Route every MSVC debug-runtime error (including "HEAP CORRUPTION DETECTED"
// from the heap validator, _CRT_ASSERT, and _CRT_WARN) to stderr + the
// debugger, and abort on _CRT_ERROR. Without this, the CRT pops a modal
// Abort/Retry/Ignore dialog and blocks the CI host indefinitely. Also
// disable the Windows Error Reporting dialog (SEM_NOGPFAULTERRORBOX) so
// hard crashes terminate the process instead of waiting on user input.
//
// The report hook returns 1 to tell the CRT "I handled it; don't show the
// dialog." We print the message and abort so the test harness reports a
// failed run with a signal that maps to test failure in CI.
static int crt_report_hook(int /*reportType*/, char* message, int* returnValue)
{
    std::fputs("[CRT] ", stderr);
    if (message) std::fputs(message, stderr);
    std::fputc('\n', stderr);
    std::fflush(stderr);
    if (returnValue) *returnValue = 0;
    std::abort();
    return 1;
}

static void installCrashGuards()
{
    // Suppress all OS-level "application has stopped working" dialogs.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX
                 | SEM_NOOPENFILEERRORBOX);

    // Route CRT errors to stderr + debugger instead of modal dialog.
    const int modes[] = { _CRT_WARN, _CRT_ERROR, _CRT_ASSERT };
    for (int m : modes) {
        _CrtSetReportMode(m, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
        _CrtSetReportFile(m, _CRTDBG_FILE_STDERR);
    }

    // Install a hook so heap corruption messages abort cleanly.
    _CrtSetReportHook(crt_report_hook);

    // Keep the default alloc-time heap check — the periodic "every N
    // allocations" flag (_CRTDBG_CHECK_EVERY_1024_DF) is intentionally
    // NOT set because it surfaces pre-existing benign past-writes in
    // third-party code (e.g. Tesseract's language-loading failure path)
    // that predate this harness. Corruption that actually affects the
    // allocator is still caught on the next alloc/free.
    _CrtSetDbgFlag(_CrtSetDbgFlag(_CRTDBG_REPORT_FLAG)
                   | _CRTDBG_ALLOC_MEM_DF);

    // Also ensure abort() doesn't show the "abort has been called" dialog.
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
}
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
    // Install before any allocations to catch corruption in the fixture set.
    installCrashGuards();
    // Create the Job Object that scopes ProcessSpawnGuard (security oracle) to our
    // own process tree, at startup, so every test-spawned child is a member from
    // the beginning — the oracle then ignores unrelated machine-wide cmd.exe/etc.
    (void)pdfe_test::testProcessJob();
    // Bring up the Job completion-port monitor at startup so ProcessSpawnGuard can
    // observe even transient (fast-exiting) in-job process spawns.
    pdfe_test::startSpawnEventMonitor();
#endif

    // The gtest binary is a non-interactive harness: suppress MainWindow's
    // modal prompts (unsaved-changes-on-close AND the R3-04 startup
    // crash-recovery prompt) so a MainWindow constructed in a test that spins
    // the event loop can never block on a QMessageBox::exec(). Same seam the
    // UI-automation harness uses; MainWindow honors it in promptSaveOnClose()
    // and checkForRecoverableSessions().
    qputenv("PDFE_TEST_SKIP_UNSAVED_PROMPT", "1");

    // GitHub #127. The opt-in version check is off by default, but AppSettings
    // with no PDFE_SETTINGS_PATH reads the REAL user store -- so a developer
    // who ticks the Preferences box would make every MainWindow-constructing
    // test issue a live HTTPS request. The suite must not depend on whose
    // machine it runs on, and must not emit network traffic.
    qputenv("PDFE_TEST_NO_UPDATE_CHECK", "1");

    // QApplication must be created before any Qt widgets/printers/screens
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("PDFE");
    QCoreApplication::setApplicationName("PDFE");
    QCoreApplication::setApplicationVersion("0.3.0");

    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
