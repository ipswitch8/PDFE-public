#include <QApplication>
#include <QAccessible>
#include <QCommandLineParser>
#include <QIcon>
#include <QNetworkProxy>
#include <QNetworkProxyFactory>
#include "ui/MainWindow.h"
// pdfeAppIcon() declared in MainWindow.h
#include "ui/PdfCanvasAccessible.h"
#include "app/AppSettings.h"
#include "app/LogHandler.h"
#include "app/LoggingPreferences.h"
#include "app/CrashHandler.h"
#include "app/ProcessMitigations.h"
#include "app/Translations.h"
#include "app/SingleInstanceGuard.h"
#include "app/WindowManager.h"
#include "app/PowerEvents.h"
#include "core/SaveSpacePrecheck.h"
#include <QMessageBox>
#include <QString>

int main(int argc, char* argv[])
{
    // Install LogHandler BEFORE applyProcessMitigations so the
    // mitigation init's qWarning/qInfo calls route to LogHandler. The
    // install() path only calls qInstallMessageHandler — it does not
    // need QApplication. setFileLogging is deferred until after
    // QApplication construction (it depends on the org/app name to
    // resolve the AppLocalData path).
    auto& logger = pdfe::app::LogHandler::instance();
    logger.setMinLevel(pdfe::app::LogHandler::Debug);
    logger.install();

    // Apply Windows process mitigation policies BEFORE QApplication so
    // ImageLoadPolicy:NoRemoteImages is active before Qt starts probing
    // for plugins. See app/ProcessMitigations.h for the full policy
    // list and intentional-omission rationale. No-op on non-Windows.
    pdfe::app::applyProcessMitigations();

    // Test-only seam: when PDFE_USE_QT_FILE_DIALOG=1 is set, force Qt's
    // own QDialog-based file picker instead of the native Win32 shell
    // IFileOpenDialog. UI regression tests (pywinauto) rely on the
    // resulting dialog being part of the same UIA tree as MainWindow;
    // the native shell dialog is out-of-process (dllhost.exe), pulls
    // its initial directory from a per-exe shell MRU that can point at
    // a missing drive (showing a blocking system error modal first),
    // and requires WinAppDriver-style harnesses to drive. The Qt
    // QFileDialog has no such complications, exposes stable Qt
    // objectNames, and lives inside the test process's UIA reach.
    // Production users get the native dialog (env var unset).
    if (qgetenv("PDFE_USE_QT_FILE_DIALOG") == "1") {
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
    }

    QApplication app(argc, argv);
    QApplication::setApplicationName("PDFE");
    // Kept in step with project(pdfe VERSION ...) in the top-level CMakeLists,
    // src/app/pdfe.rc, and VersionInfo.cpp's fallback. A hard-coded literal here
    // that drifts from the resource makes the app REPORT one version while its
    // file properties claim another -- which is the shape of a support call
    // nobody can answer.
    QApplication::setApplicationVersion("0.3.0");
    QApplication::setOrganizationName("PDFE");

    // Set the runtime window / taskbar icon. Qt 6 on Windows does
    // not auto-pick up the application's Win32 .rc icon for windows
    // it owns; without this call the title bar and taskbar fall
    // back to IDI_APPLICATION (the generic Windows app icon) even
    // though the static Explorer file icon shows the .rc icon
    // correctly. Prefer the SVG variant that matches the OS color
    // scheme (light/dark); fall back to the multi-size .ico if the
    // SVG resources or the QtSvg plugin aren't available at runtime.
    QApplication::setWindowIcon(pdfe::ui::pdfeAppIcon());

    // Install translations for the system locale. PDFE ships English-only, so
    // this normally finds no app_<locale>.qm and returns false, leaving the
    // English source strings in place — the mechanism is in place so a locale
    // can be added purely as a build-time step (lupdate/lrelease, see
    // docs/i18n.md) with no code change. Qt's own base translations
    // (qtbase_<locale>.qm) are still loaded when available so standard Qt
    // dialog buttons localize on a translated system. Must run after
    // QApplication construction (needs applicationDirPath + Qt library paths).
    pdfe::app::installTranslations(app);

    // Suppress Qt's WPAD / system-proxy auto-discovery. PDFE has no
    // built-in feature that needs an outbound HTTP connection (no
    // update check, no telemetry, no remote-PDF fetcher); leaving
    // QNetworkProxyFactory at its default "use system configuration"
    // causes Qt to issue NetBIOS / LLMNR broadcasts at QApplication
    // startup looking for a WPAD server. Phase-3 corpus runs measured
    // those broadcasts (UDP 192.168.0.255:137-138, 224.0.0.252:5355,
    // ff02::1:3:5355) being attributed to PDFE.exe and surfaced as
    // exfiltration-attempt findings. They're link-local only and the
    // firewall sandbox catches them, but disabling at the source is
    // the cleaner defense.
    QNetworkProxyFactory::setUseSystemConfiguration(false);
    QNetworkProxy::setApplicationProxy(QNetworkProxy(QNetworkProxy::NoProxy));

    // Install accessibility factory
    QAccessible::installFactory(pdfe::ui::pdfCanvasAccessibleFactory);

    // Install crash handler
    pdfe::app::CrashHandler::instance().install();

    // LogHandler was installed above; apply the user's logging preferences now
    // that QApplication exists (AppSettings keys off the org/app name, and
    // LogHandler's default path resolves through AppLocalDataLocation).
    //
    // GitHub #151 — this line used to read `logger.setFileLogging(true);`.
    // Enabled unconditionally, with NO path argument, so the overload defaulted
    // to `{}` and LogHandler picked its own location. AppSettings was not
    // referenced anywhere in this file, which meant the Preferences dialog's
    // "Enable file logging", "Log file path" and "Log level" controls were
    // written to disk and read by nothing at all. The engine was never wrong;
    // the caller supplied a constant. See app/LoggingPreferences.h for the
    // semantics and for why logToFile() (default FALSE) now genuinely decides.
    {
        pdfe::app::AppSettings startupSettings;
        pdfe::app::applyLoggingPreferences(logger, startupSettings);
    }

    // GitHub #100 / WIN-78 — surface the free-space pre-check to the user.
    //
    // Installed HERE rather than in MainWindow so it exists only in the real
    // application: a modal dialog wired up by a widget constructor would also
    // exist inside the GoogleTest process, where nothing can dismiss it.
    // Without a handler the core falls back to a log line, which is what the
    // test build gets.
    //
    // Warning, not a question. The save is not blocked and there is nothing to
    // decide — offering Cancel would imply PDFE knew the save would fail, which
    // it does not: free space is a racing figure and the staged-write path
    // already guarantees the original survives a genuine ENOSPC.
    pdfe::core::setSaveSpaceWarningHandler(
        [](const pdfe::core::SaveSpaceEstimate& est,
           const std::filesystem::path& target) {
            QMessageBox::warning(
                nullptr,
                QObject::tr("Low disk space"),
                QObject::tr("There may not be enough free space to save "
                            "\"%1\".\n\n"
                            "Estimated size: %2 MB\n"
                            "Free space: %3 MB\n\n"
                            "The save will be attempted anyway. If it fails, "
                            "your existing file is left unchanged.")
                    .arg(QString::fromStdWString(target.filename().wstring()))
                    .arg(static_cast<double>(est.estimatedBytes) / (1024.0 * 1024.0),
                         0, 'f', 1)
                    .arg(static_cast<double>(est.freeBytes) / (1024.0 * 1024.0),
                         0, 'f', 1));
        });

    // Parse command line
    QCommandLineParser parser;
    parser.setApplicationDescription("PDFE - PDF Editor");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument("file", "PDF file to open", "[file]");
    parser.process(app);

    // GitHub #241 — ALL positional arguments, not just the first.
    //
    // This used to be `positionalArgs.first()` collapsed into a single QString,
    // and everything downstream took that one value. Multi-selecting several
    // PDFs in Explorer and choosing Open passes them to one invocation, so
    // arguments 2..N were dropped here with no message at all — the user asked
    // for four documents and got one.
    const QStringList cliFiles = parser.positionalArguments();

    // R3-02 single-instance / same-file data-loss guard. If an editor is
    // already running for this user, forward the file to it (so it opens in the
    // existing window) instead of spawning a second editor on the same file —
    // which would open a concurrent-save data-loss window. Constructed AFTER
    // QApplication (QLocalSocket needs a live QCoreApplication) and BEFORE any
    // MainWindow so the secondary exits without touching the document model.
    // PDFE_DISABLE_SINGLE_INSTANCE=1 opts out (e.g. deliberate side-by-side
    // sessions in a test harness).
    pdfe::app::SingleInstanceGuard instanceGuard;
    if (!instanceGuard.isPrimary() &&
        qgetenv("PDFE_DISABLE_SINGLE_INSTANCE") != "1") {
        // Secondary: hand our files (if any) to the running primary and exit.
        //
        // GitHub #241: forwards the WHOLE list. Explorer's other multi-open
        // behaviour is to spawn one process per selected file, and each of
        // those secondaries forwards its own path; this covers the batched case
        // where one secondary holds them all. Both routes now end with the
        // primary opening every document instead of the last one to arrive.
        instanceGuard.sendPathsToPrimary(cliFiles);
        return 0;
    }

    // GitHub #213 / WIN-71 — observe WM_POWERBROADCAST.
    //
    // Installed HERE, not in MainWindow, for the same reason the save-space
    // warning handler above is: a native event filter on the QApplication is
    // process-global, and the GoogleTest binary builds MainWindows freely. One
    // filter belongs to the one real application.
    //
    // PARTIAL BY DESIGN. It acts on PBT_APMSUSPEND (take a recovery snapshot)
    // and records PBT_APMRESUMEAUTOMATIC. It does NOT make PDFE
    // suspend/resume-correct — nothing revalidates handles or invalidates
    // render state on resume. See docs/power-event-handling-scope.md for what
    // full support needs; #213 stays open for it.
    //
    // Function-local static so it outlives app.exec() and is destroyed after
    // the QApplication has finished dispatching.
    static pdfe::app::PowerEventFilter powerEventFilter;
    app.installNativeEventFilter(&powerEventFilter);

    // GitHub #241 — one window per document, owned by the manager.
    //
    // The MainWindow used to be a stack object here, which is the deepest
    // reason PDFE could only ever show one document: there was exactly one, and
    // its lifetime was main()'s. WindowManager holds however many the user
    // asked for and enforces the rule the single-instance guard was protecting
    // — the same FILE never gets two editors, because both could save it.
    pdfe::app::WindowManager windowManager;

    // Primary: a forwarded open request from a later launch is routed through
    // the manager, so it raises the window already showing that file, or opens
    // a new one for a file that is not open yet. Either way the user never ends
    // up with two editors on one document.
    QObject::connect(&instanceGuard, &pdfe::app::SingleInstanceGuard::fileOpenRequested,
                     &windowManager, [&windowManager](const QString& path) {
        windowManager.openPath(path);
    });

    // Open every file named on the command line. An empty list still shows the
    // one empty window that launching PDFE with no arguments always has.
    if (cliFiles.isEmpty()) {
        windowManager.openEmptyWindow();
    } else {
        for (const QString& file : cliFiles) {
            windowManager.openPath(file);
        }
    }

    return app.exec();
}
