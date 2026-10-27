#include "LoggingPreferences.h"

#include "AppSettings.h"
#include "LogHandler.h"

namespace pdfe::app {
namespace {

/// AppSettings stores the level as the Preferences combo's index
/// (0=Debug, 1=Info, 2=Warning, 3=Critical — AppSettings.h:93). Anything
/// outside that range is a corrupt/hand-edited INI; clamp rather than index
/// past the end of the enum.
LogHandler::Level levelFromSettingIndex(int index)
{
    switch (index) {
        case 0:  return LogHandler::Debug;
        case 1:  return LogHandler::Info;
        case 2:  return LogHandler::Warning;
        case 3:  return LogHandler::Critical;
        default: return LogHandler::Warning;   // AppSettings' own default
    }
}

}  // namespace

QString applyLoggingPreferences(LogHandler& logger, const AppSettings& settings)
{
    logger.setMinLevel(levelFromSettingIndex(settings.logLevel()));

    if (!settings.logToFile()) {
        logger.setFileLogging(false);
        return QString();
    }

    // Trimmed because a path typed into the Preferences line edit can pick up
    // a trailing space, and " C:/logs/pdfe.log" is not a path anyone meant.
    logger.setFileLogging(true, settings.logFilePath().trimmed());

    // Read back rather than return what we passed in: when the configured
    // value is empty LogHandler chooses AppLocalDataLocation/pdfe.log, and the
    // caller (and the tests) want the path that is actually open.
    return logger.logFilePath();
}

}  // namespace pdfe::app
