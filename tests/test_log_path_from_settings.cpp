// -*- coding: utf-8 -*-
//
// GitHub #151, half 2 — "the configured log path is never read".
//
// WHY THIS FILE EXISTS AND test_log_handler.cpp DOES NOT COVER IT
// ----------------------------------------------------------------
// tests/test_log_handler.cpp:115 already does:
//
//     handler_->setFileLogging(true, logPath);
//     EXPECT_EQ(handler_->logFilePath(), logPath);
//
// That asserts the ENGINE honours a path it is handed, and it has been green
// for the whole life of the defect. It proves nothing about #151, because the
// defect was never in LogHandler: `src/main.cpp` called
// `logger.setFileLogging(true)` with NO path, so `AppSettings::logFilePath()`
// — written by the Preferences dialog — was read by nothing. A test that hands
// the path in itself supplies exactly the argument the product failed to
// supply. (`docs/audit/dca/0070-the-engine-was-right-and-the-dialog-never-
// asked-it.md` is the same shape.)
//
// So the seam under test here is "a value stored in AppSettings decides where
// the log goes", and it is exercised at TWO levels, because each covers a hole
// in the other:
//
//   1. IN-PROCESS, through pdfe::app::applyLoggingPreferences() — the decision
//      function main() now delegates to. Fast, precise, covers the branches
//      (enabled / disabled / empty-path fallback / level).
//
//   2. PROCESS-LEVEL, against the REAL pdfe.exe. This is the only level that
//      can fail if someone reverts main.cpp to the constant-passing form,
//      because (1) would still pass with main.cpp calling nothing at all. The
//      configured path is a freshly-created temp directory, so no hard-coded
//      location in any build of the product can satisfy it by accident.
//
// The disabled case is the discriminating partner of the enabled case: on its
// own, "the file appeared where I asked" could also be satisfied by a product
// that always writes a file everywhere it is told about. The pair shows the
// SETTING decides, using the same path and the same launch.

#include <gtest/gtest.h>

#include "app/AppSettings.h"
#include "app/LogHandler.h"
#include "app/LoggingPreferences.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QString>
#include <QTemporaryDir>

#include <chrono>
#include <filesystem>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using pdfe::app::AppSettings;
using pdfe::app::LogHandler;

namespace {

// A marker unique to THIS process and THIS call.
//
// It has to be. The negative control below reads LogHandler's built-in default
// location, which is a REAL, PERSISTENT file in the user profile that survives
// between runs — measured: a fixed marker string left behind by one run (here,
// by a deliberate mutant) makes every later run fail the control on history
// rather than on behaviour. Uniqueness is what makes "this message did not go
// there" a statement about this run.
QString uniqueMarker(const char* stem)
{
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    return QString("%1_%2_%3")
        .arg(stem)
        .arg(static_cast<qulonglong>(QCoreApplication::applicationPid()))
        .arg(static_cast<qulonglong>(ns));
}

QString readAll(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return QString();
    return QString::fromUtf8(f.readAll());
}

// Set an environment variable in BOTH the CRT table (which Qt's qgetenv reads
// in this process) and the Win32 block (which CreateProcess hands to children).
// qputenv goes through _putenv_s, which the MSVC CRT keeps synchronised with
// the Win32 block; the explicit SetEnvironmentVariableW makes that guarantee
// local to this file rather than a property of the CRT we happen to link.
class ScopedEnv {
public:
    ScopedEnv(const char* name, const QString& value) : name_(name)
    {
        had_ = qEnvironmentVariableIsSet(name);
        if (had_) previous_ = qgetenv(name);
        apply(value.toLocal8Bit());
    }
    ~ScopedEnv()
    {
        if (had_) {
            apply(previous_);
        } else {
            qunsetenv(name_.c_str());
#ifdef _WIN32
            ::SetEnvironmentVariableA(name_.c_str(), nullptr);
#endif
        }
    }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    void apply(const QByteArray& v)
    {
        qputenv(name_.c_str(), v);
#ifdef _WIN32
        ::SetEnvironmentVariableA(name_.c_str(), v.constData());
#endif
    }
    std::string name_;
    QByteArray previous_;
    bool had_ = false;
};

// ─────────────────────────────────────────────────────────────────────────
// Level 1 — the decision function, in process.
// ─────────────────────────────────────────────────────────────────────────

class LoggingPreferencesTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(dir_.isValid());
        iniPath_ = dir_.path() + "/pdfe-settings.ini";
        env_ = std::make_unique<ScopedEnv>("PDFE_SETTINGS_PATH", iniPath_);

        // LogHandler is a process-wide singleton shared with every other test
        // in this binary. Hand it back exactly as found, or a neighbour's
        // leftover level silently decides someone else's result.
        logger_ = &LogHandler::instance();
        savedLevel_ = logger_->minLevel();
        savedFileLogging_ = logger_->isFileLogging();
        savedPath_ = logger_->logFilePath();
        logger_->setFileLogging(false);
        logger_->install();
    }

    void TearDown() override
    {
        logger_->uninstall();
        logger_->setFileLogging(false);
        if (savedFileLogging_) logger_->setFileLogging(true, savedPath_);
        logger_->setMinLevel(savedLevel_);
        env_.reset();
    }

    QString scratch(const QString& leaf) const { return dir_.path() + "/" + leaf; }

    QTemporaryDir dir_;
    QString iniPath_;
    std::unique_ptr<ScopedEnv> env_;
    LogHandler* logger_ = nullptr;
    LogHandler::Level savedLevel_ = LogHandler::Warning;
    bool savedFileLogging_ = false;
    QString savedPath_;
};

// The core of #151: a path stored in AppSettings is where the bytes land.
TEST_F(LoggingPreferencesTest, ConfiguredPathIsWhereTheLoggerActuallyWrites)
{
    const QString configured = scratch("configured-151.log");
    ASSERT_FALSE(QFile::exists(configured));

    AppSettings settings;
    settings.setLogToFile(true);
    settings.setLogFilePath(configured);
    settings.setLogLevel(2);  // Warning — the qWarning below is AT this level
    settings.sync();

    const QString resolved =
        pdfe::app::applyLoggingPreferences(*logger_, settings);

    // Asserted against the INPUT (the string handed to setLogFilePath), not
    // against another output of the same call.
    EXPECT_EQ(resolved, configured);
    EXPECT_EQ(logger_->logFilePath(), configured);
    EXPECT_TRUE(logger_->isFileLogging());

    const QString marker = uniqueMarker("PDFE_CONFIGURED_PATH_MARKER_151");
    qWarning("%s", qPrintable(marker));

    ASSERT_TRUE(QFile::exists(configured))
        << "no log file exists at the configured path " << configured.toStdString()
        << " — AppSettings::logFilePath() is not reaching LogHandler (#151)";
    EXPECT_TRUE(readAll(configured).contains(marker))
        << "the configured file exists but the message went somewhere else";

    // The control that makes the assertion above mean "HERE" rather than
    // "somewhere, including here": LogHandler's own default location must not
    // have received this message.
    const QString fallback =
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) +
        "/pdfe.log";
    EXPECT_NE(fallback, configured);
    EXPECT_FALSE(readAll(fallback).contains(marker))
        << "the message reached BOTH the configured path and the built-in "
           "default " << fallback.toStdString();
}

// The partner case. Without it, "a file appeared where I asked" is also
// satisfied by a product that writes a file wherever it is told about one.
TEST_F(LoggingPreferencesTest, DisabledPreferenceWritesNothingAtTheConfiguredPath)
{
    const QString configured = scratch("must-not-appear-151.log");
    ASSERT_FALSE(QFile::exists(configured));

    AppSettings settings;
    settings.setLogToFile(false);           // the only difference from the test above
    settings.setLogFilePath(configured);
    settings.setLogLevel(2);
    settings.sync();

    const QString resolved =
        pdfe::app::applyLoggingPreferences(*logger_, settings);

    EXPECT_TRUE(resolved.isEmpty());
    EXPECT_FALSE(logger_->isFileLogging());

    qWarning("%s", qPrintable(uniqueMarker("PDFE_DISABLED_MARKER_151")));

    EXPECT_FALSE(QFile::exists(configured))
        << "logToFile() is false and yet a log file was created at "
        << configured.toStdString()
        << " — the enable preference does not decide anything";
}

// An EMPTY configured path is the documented "use the platform default"
// value (AppSettings.cpp, GitHub #151). Test mode moves
// AppLocalDataLocation somewhere disposable so this case cannot write into
// the real user profile.
TEST_F(LoggingPreferencesTest, EmptyConfiguredPathFallsBackToAppLocalData)
{
    const bool savedTestMode = QStandardPaths::isTestModeEnabled();
    QStandardPaths::setTestModeEnabled(true);

    AppSettings settings;
    settings.setLogToFile(true);
    settings.setLogFilePath(QString());
    settings.sync();
    ASSERT_TRUE(settings.logFilePath().isEmpty())
        << "the stored default must be empty, not a concrete path — a concrete "
           "default here duplicates LogHandler's path expression in a second "
           "place where the two can drift (#151)";

    const QString expected =
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) +
        "/pdfe.log";
    const QString resolved =
        pdfe::app::applyLoggingPreferences(*logger_, settings);

    EXPECT_EQ(resolved, expected);
    EXPECT_EQ(logger_->logFilePath(), expected);

    logger_->setFileLogging(false);
    QStandardPaths::setTestModeEnabled(savedTestMode);
}

// The Preferences "Log level" combo is read by the same call. Asserted
// against the index written, in both directions, so a handler stuck at one
// level cannot pass.
TEST_F(LoggingPreferencesTest, LogLevelPreferenceReachesTheHandler)
{
    AppSettings settings;
    settings.setLogToFile(false);

    settings.setLogLevel(3);  // Critical
    settings.sync();
    pdfe::app::applyLoggingPreferences(*logger_, settings);
    EXPECT_EQ(logger_->minLevel(), LogHandler::Critical);

    settings.setLogLevel(0);  // Debug
    settings.sync();
    pdfe::app::applyLoggingPreferences(*logger_, settings);
    EXPECT_EQ(logger_->minLevel(), LogHandler::Debug);
}

#ifdef _WIN32

// ─────────────────────────────────────────────────────────────────────────
// Level 2 — the REAL pdfe.exe. This is the level that reddens when
// src/main.cpp reverts to `logger.setFileLogging(true);`.
// ─────────────────────────────────────────────────────────────────────────

std::filesystem::path pdfeExe()
{
    wchar_t buf[MAX_PATH * 4] = {0};
    const DWORD n = ::GetModuleFileNameW(nullptr, buf, MAX_PATH * 4);
    if (n == 0) return {};
    return std::filesystem::path(std::wstring(buf, n)).parent_path() / L"pdfe.exe";
}

class ChildApp {
public:
    ~ChildApp() { kill(); }

    bool start(const std::filesystem::path& exe)
    {
        std::wstring cmd = L"\"" + exe.wstring() + L"\"";
        std::vector<wchar_t> buf(cmd.begin(), cmd.end());
        buf.push_back(L'\0');
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (!::CreateProcessW(exe.wstring().c_str(), buf.data(), nullptr, nullptr,
                              FALSE, 0, nullptr,
                              exe.parent_path().wstring().c_str(), &si, &pi)) {
            lastError_ = ::GetLastError();
            return false;
        }
        ::CloseHandle(pi.hThread);
        handle_ = pi.hProcess;
        pid_ = pi.dwProcessId;
        return true;
    }

    bool alive() const
    {
        return handle_ && ::WaitForSingleObject(handle_, 0) == WAIT_TIMEOUT;
    }

    void kill()
    {
        if (!handle_) return;
        if (alive()) ::TerminateProcess(handle_, 0);
        ::WaitForSingleObject(handle_, 5000);
        ::CloseHandle(handle_);
        handle_ = nullptr;
        pid_ = 0;
    }

    DWORD pid() const { return pid_; }
    DWORD lastError() const { return lastError_; }

private:
    HANDLE handle_ = nullptr;
    DWORD pid_ = 0;
    DWORD lastError_ = 0;
};

struct EnumCtx {
    DWORD pid = 0;
    int count = 0;
};

BOOL CALLBACK countMainWindows(HWND hwnd, LPARAM lp)
{
    auto* ctx = reinterpret_cast<EnumCtx*>(lp);
    DWORD pid = 0;
    ::GetWindowThreadProcessId(hwnd, &pid);
    if (pid != ctx->pid) return TRUE;
    if (!::IsWindowVisible(hwnd)) return TRUE;
    if (::GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
    wchar_t title[512] = {0};
    ::GetWindowTextW(hwnd, title, 511);
    if (::wcsncmp(title, L"PDFE", 4) != 0) return TRUE;
    ++ctx->count;
    return TRUE;
}

int mainWindowCount(DWORD pid)
{
    EnumCtx ctx;
    ctx.pid = pid;
    ::EnumWindows(&countMainWindows, reinterpret_cast<LPARAM>(&ctx));
    return ctx.count;
}

template <typename Pred>
bool spinUntil(Pred pred, int budgetMs, int stepMs = 100)
{
    for (int waited = 0; waited <= budgetMs; waited += stepMs) {
        if (pred()) return true;
        ::Sleep(static_cast<DWORD>(stepMs));
    }
    return pred();
}

class LaunchedAppLogPathTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        exe_ = pdfeExe();
        ASSERT_FALSE(exe_.empty());
        ASSERT_TRUE(std::filesystem::exists(exe_))
            << "pdfe.exe must be built alongside pdfe_tests.exe; looked at "
            << exe_.string()
            << ". Build the 'pdfe' target (a build-configuration problem, not a "
               "product verdict).";

        ASSERT_TRUE(dir_.isValid());
        iniPath_ = dir_.path() + "/pdfe-settings.ini";

        const auto stamp =
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count();

        // Inherited by the child: its AppSettings reads THIS ini, it never
        // forwards to (or is forwarded to by) a foreign pdfe.exe, and it does
        // not share a single-instance domain with a concurrent shard.
        settingsEnv_ = std::make_unique<ScopedEnv>("PDFE_SETTINGS_PATH", iniPath_);
        optOutEnv_ =
            std::make_unique<ScopedEnv>("PDFE_DISABLE_SINGLE_INSTANCE", "1");
        domainEnv_ = std::make_unique<ScopedEnv>(
            "PDFE_INSTANCE_DOMAIN",
            QString("log151-%1-%2")
                .arg(static_cast<qulonglong>(::GetCurrentProcessId()))
                .arg(static_cast<qulonglong>(stamp)));
    }

    void TearDown() override
    {
        child_.kill();
        domainEnv_.reset();
        optOutEnv_.reset();
        settingsEnv_.reset();
    }

    // The configured path lives in a directory created moments ago by this
    // test. No constant compiled into pdfe.exe can name it.
    QString configuredLogPath() const { return dir_.path() + "/pdfe-configured.log"; }

    void writeSettings(bool logToFile)
    {
        AppSettings settings;   // picks up PDFE_SETTINGS_PATH
        settings.setLogToFile(logToFile);
        settings.setLogFilePath(configuredLogPath());
        settings.setLogLevel(0);  // Debug — everything the app emits is eligible
        settings.sync();
        ASSERT_TRUE(QFile::exists(iniPath_)) << "the settings INI was not written";
    }

    std::filesystem::path exe_;
    QTemporaryDir dir_;
    QString iniPath_;
    ChildApp child_;
    std::unique_ptr<ScopedEnv> settingsEnv_;
    std::unique_ptr<ScopedEnv> optOutEnv_;
    std::unique_ptr<ScopedEnv> domainEnv_;
};

// RED / mutation (verified, p85 phase-4): restore
// `logger.setFileLogging(true);` in src/main.cpp and this case fails on
// "no log file was created at <temp>/pdfe-configured.log" — the product falls
// back to AppLocalDataLocation and the configured path stays empty.
TEST_F(LaunchedAppLogPathTest, LaunchedAppWritesItsLogWhereTheSettingsSay)
{
    writeSettings(/*logToFile=*/true);
    const QString configured = configuredLogPath();
    ASSERT_FALSE(QFile::exists(configured));

    ASSERT_TRUE(child_.start(exe_))
        << "could not launch pdfe.exe (win32 error " << child_.lastError() << ")";

    const bool appeared =
        spinUntil([&] { return QFile::exists(configured); }, 120000);

    // Recorded, not asserted: if the app never started at all the failure
    // below would be ambiguous, and this is the fact that disambiguates it.
    RecordProperty("child_main_windows", mainWindowCount(child_.pid()));
    RecordProperty("child_alive", child_.alive() ? 1 : 0);

    EXPECT_TRUE(appeared)
        << "pdfe.exe created no log file at the configured path "
        << configured.toStdString()
        << " — AppSettings::logFilePath() is not reaching LogHandler from "
           "main() (GitHub #151). Main windows seen for the child: "
        << mainWindowCount(child_.pid()) << "; still running: "
        << (child_.alive() ? "yes" : "no");
}

// The partner. Same launch, same configured path, one setting flipped.
TEST_F(LaunchedAppLogPathTest, LaunchedAppHonoursTheDisabledFileLoggingSetting)
{
    writeSettings(/*logToFile=*/false);
    const QString configured = configuredLogPath();
    ASSERT_FALSE(QFile::exists(configured));

    ASSERT_TRUE(child_.start(exe_))
        << "could not launch pdfe.exe (win32 error " << child_.lastError() << ")";

    // The app must genuinely have started, otherwise "no file appeared" is
    // vacuous. A visible main window is the evidence of a real startup.
    const bool started =
        spinUntil([&] { return mainWindowCount(child_.pid()) > 0; }, 120000);
    ASSERT_TRUE(started)
        << "pdfe.exe never showed a main window, so the absence of a log file "
           "proves nothing about the setting";

    // Startup is well past the logging configuration by the time a window
    // exists; give it a further moment for anything asynchronous.
    ::Sleep(2000);

    EXPECT_FALSE(QFile::exists(configured))
        << "logToFile()==false and yet pdfe.exe created "
        << configured.toStdString()
        << " — file logging is still unconditional (GitHub #151)";
}

#endif  // _WIN32

}  // namespace
