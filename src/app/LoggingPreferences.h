#pragma once

#include <QString>

namespace pdfe::app {

class AppSettings;
class LogHandler;

/// Push the user's persisted logging preferences into the live LogHandler.
///
/// GitHub #151, half 2 — "the configured log path is never read".
///
/// `LogHandler::setFileLogging(bool, const QString&)` has always honoured a
/// supplied path correctly. The defect was never in the engine: `src/main.cpp`
/// called it as `setFileLogging(true)` — enabled unconditionally, path
/// defaulted to `{}` — so `AppSettings::logFilePath()`, `logToFile()` and
/// `logLevel()` were written by the Preferences dialog and then read by
/// nothing. Three controls that silently did nothing. (Same shape as
/// `docs/audit/dca/0070-the-engine-was-right-and-the-dialog-never-asked-it.md`.)
///
/// This function is the caller the engine was missing, factored out of main()
/// so the decision it makes is reachable from a test without launching the app.
/// `src/main.cpp` must contain no other `setFileLogging` call site — the
/// end-to-end launch test in `tests/test_log_path_from_settings.cpp` is what
/// proves the product actually routes through here.
///
/// Semantics, stated so a later reader does not have to infer them:
///   * `logToFile()` DECIDES whether a file is written at all. It defaults to
///     false, so a stock install now writes no `pdfe.log`. That is the
///     deliberate answer to #151's second question ("decide whether file
///     logging should be on by default at all") and it is the only reading
///     under which the "Enable file logging" checkbox — unchecked by default —
///     is telling the truth.
///   * `logFilePath()` names the file. An EMPTY value means "wherever
///     LogHandler puts it by default", which is
///     `AppLocalDataLocation/pdfe.log`.
///   * `logLevel()` sets the minimum severity, for stderr and the file alike.
///
/// Returns the absolute path file logging was pointed at, or an empty string
/// when file logging is disabled. The return value is the observable the tests
/// assert on; it is read back out of LogHandler, not echoed from the input.
QString applyLoggingPreferences(LogHandler& logger, const AppSettings& settings);

}  // namespace pdfe::app
