#include "LogHandler.h"
#include "core/AtomicFileReplace.h"
#include <QDateTime>
#include <QStandardPaths>
#include <QTextStream>
#include <QDir>
#include <QFileInfo>
#include <cstdio>
#include <filesystem>
#include <system_error>

namespace pdfe::app {

namespace {

/// QString -> filesystem::path without a narrow-encoding round trip. The same
/// two-arm form as MainWindow.cpp's: on Windows the wide string is the native
/// representation, so a Unicode log directory survives; elsewhere UTF-8 is.
std::filesystem::path qStringToPath(const QString& s)
{
#ifdef _WIN32
    return std::filesystem::path(s.toStdWString());
#else
    return std::filesystem::path(s.toStdString());
#endif
}

}  // namespace

LogHandler::LogHandler() = default;

LogHandler::~LogHandler() {
    if (installed_) {
        uninstall();
    }
    if (logFile_.isOpen()) {
        logFile_.close();
    }
}

LogHandler& LogHandler::instance() {
    static LogHandler instance;
    return instance;
}

void LogHandler::install() {
    QMutexLocker locker(&mutex_);
    if (!installed_) {
        previousHandler_ = qInstallMessageHandler(&LogHandler::messageHandler);
        installed_ = true;
    }
}

void LogHandler::uninstall() {
    QMutexLocker locker(&mutex_);
    if (installed_) {
        qInstallMessageHandler(previousHandler_);
        previousHandler_ = nullptr;
        installed_ = false;
    }
}

void LogHandler::setMinLevel(Level level) {
    QMutexLocker locker(&mutex_);
    minLevel_ = level;
}

LogHandler::Level LogHandler::minLevel() const {
    QMutexLocker locker(&mutex_);
    return minLevel_;
}

void LogHandler::setFileLogging(bool enabled, const QString& path) {
    QMutexLocker locker(&mutex_);

    // Close existing log file if open
    if (logFile_.isOpen()) {
        logFile_.close();
    }

    fileLogging_ = enabled;

    if (enabled) {
        // Determine log file path
        if (path.isEmpty()) {
            QString appDataPath = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
            QDir().mkpath(appDataPath);
            logPath_ = appDataPath + "/pdfe.log";
        } else {
            logPath_ = path;
            // Ensure directory exists
            QFileInfo fileInfo(path);
            QDir().mkpath(fileInfo.absolutePath());
        }

        // Open log file.
        //
        // The result is CHECKED, not discarded. This line is why the compiler
        // emits C4834 for this translation unit, and the warning is right: if the
        // open fails -- a read-only directory, a path the user cannot write, a
        // name already held exclusively -- then setFileLogging(true, path)
        // returns having enabled file logging that will never write a byte, and
        // isFileLogging() reports true. Every later write silently discards.
        //
        // stderr, not qWarning: this object IS the installed message handler and
        // we hold mutex_, so logging here would recurse or deadlock; and the
        // destination being reported as unusable is the log itself.
        //
        // fileLogging_ is deliberately left enabled. Flipping it off here would
        // be a second behaviour change in a path that currently has none, and
        // the in-memory sinks (lastEntry_, the level filter) still work. The
        // defect being fixed is the SILENCE, not the fallback.
        logFile_.setFileName(logPath_);
        if (!logFile_.open(QIODevice::Append | QIODevice::Text)) {
            std::fprintf(stderr,
                         "[pdfe] file logging enabled but '%s' could not be "
                         "opened for append (%s); log messages will not reach "
                         "disk this session\n",
                         logPath_.toUtf8().constData(),
                         logFile_.errorString().toUtf8().constData());
        }

        // GitHub #151 — rotate AT OPEN as well as during writes. Without this,
        // a log left oversized by a previous run (an older build with no cap,
        // or a crash between the last write and the rotation check) would only
        // be brought back under the cap after the current session had written
        // its first message — and never at all in a session that logs nothing.
        rotateIfNeededLocked();
    } else {
        logPath_.clear();
    }
}

void LogHandler::setMaxFileBytes(qint64 bytes) {
    QMutexLocker locker(&mutex_);
    maxFileBytes_ = bytes < kMinMaxFileBytes ? kMinMaxFileBytes : bytes;
    rotateIfNeededLocked();
}

qint64 LogHandler::maxFileBytes() const {
    QMutexLocker locker(&mutex_);
    return maxFileBytes_;
}

QString LogHandler::backupPathFor(const QString& path) {
    return path.isEmpty() ? QString() : path + ".1";
}

QString LogHandler::rotatedLogFilePath() const {
    QMutexLocker locker(&mutex_);
    return backupPathFor(logPath_);
}

int LogHandler::rotationCount() const {
    QMutexLocker locker(&mutex_);
    return rotationCount_;
}

void LogHandler::rotateIfNeededLocked() {
    if (!fileLogging_ || logPath_.isEmpty()) return;

    const qint64 size = logFile_.isOpen() ? logFile_.size()
                                          : QFileInfo(logPath_).size();
    if (size < maxFileBytes_) return;

    if (logFile_.isOpen()) {
        logFile_.close();
    }

    const QString backup = backupPathFor(logPath_);

    // Publish the rotated generation through the sanctioned primitive, NOT
    // `QFile::remove(backup)` + `QFile::rename(logPath_, backup)`.
    //
    // That pair is the WIN-87 shape: the remove is needed only because Qt's
    // rename will not replace an existing name on Windows, and it destroys the
    // previous generation BEFORE the rename that is supposed to supply the new
    // one. If the rename then fails — an indexer or A/V holding a handle, the
    // case the fallback below exists for — retention has already dropped to
    // zero, and the generation that was deleted is gone for the reason we were
    // rotating in the first place.
    //
    // `atomicReplaceFile` is MoveFileExW with MOVEFILE_REPLACE_EXISTING: one
    // directory operation, so `backup` names the old generation until the
    // instant it names the new one, and a failure leaves the old one intact. A
    // destination that does not yet exist (the first rotation) is fine —
    // preserveDestinationDacl treats "nothing to preserve" as success.
    //
    // Found by tools/gate/check_publish_shape_completeness.py on its first run:
    // `QFile::rename` was a publish spelling absent from the WIN-87 sweep's
    // shape list, so this site was invisible to the guard rather than allowed
    // by it. See GitHub #254.
    std::error_code renameEc;
    if (pdfe::core::atomicReplaceFile(qStringToPath(logPath_),
                                      qStringToPath(backup), renameEc)) {
        ++rotationCount_;
    } else {
        // The active log could not be published — an indexer or A/V holding a
        // handle, or a read-only directory. TRUNCATE rather than give up:
        // failing open here would restore precisely the unbounded growth this
        // exists to stop, and the messages being discarded are the OLDEST ones.
        //
        // Report WHY to stderr, directly. `renameEc` is the only account of a
        // failure that is about to destroy log content, and it cannot be reported
        // through qWarning: this function runs inside the installed message
        // handler, under mutex_, so logging here would deadlock or recurse. It
        // cannot be written to the log either — that is the file being rotated.
        // Left unread it would be a diagnosis thrown away at the one moment it
        // matters.
        std::fprintf(stderr,
                     "[pdfe] log rotation could not publish '%s' -> '%s' (%s); "
                     "truncating instead, oldest entries are being discarded\n",
                     logPath_.toUtf8().constData(),
                     backup.toUtf8().constData(),
                     renameEc ? renameEc.message().c_str() : "no error reported");
        QFile stale(logPath_);
        if (stale.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            stale.close();
            ++rotationCount_;
        }
    }

    logFile_.setFileName(logPath_);
    (void)logFile_.open(QIODevice::Append | QIODevice::Text);
}

bool LogHandler::isFileLogging() const {
    QMutexLocker locker(&mutex_);
    return fileLogging_;
}

QString LogHandler::logFilePath() const {
    QMutexLocker locker(&mutex_);
    return logPath_;
}

LogHandler::LogEntry LogHandler::lastEntry() const {
    QMutexLocker locker(&mutex_);
    return lastEntry_;
}

void LogHandler::messageHandler(QtMsgType type,
                                const QMessageLogContext& context,
                                const QString& msg) {
    instance().handleMessage(type, context, msg);
}

void LogHandler::handleMessage(QtMsgType type,
                               const QMessageLogContext& context,
                               const QString& msg) {
    QMutexLocker locker(&mutex_);

    // Update last entry for testing purposes (always, even if below min level)
    lastEntry_.type = type;
    lastEntry_.category = context.category ? QString::fromUtf8(context.category) : QString();
    lastEntry_.message = msg;
    lastEntry_.file = context.file ? QString::fromUtf8(context.file) : QString();
    lastEntry_.line = context.line;

    // Check if we should log this message
    if (!shouldLog(type)) {
        return;
    }

    // Format the message
    QString formatted = formatMessage(type, context, msg);

    // Write to stderr
    fprintf(stderr, "%s\n", formatted.toUtf8().constData());
    fflush(stderr);

    // Write to log file if enabled
    if (fileLogging_ && logFile_.isOpen()) {
        {
            // Scoped so the QTextStream is DESTROYED, and has therefore flushed
            // everything it buffered, before rotateIfNeededLocked() can close
            // and reopen logFile_ underneath it. A stream still bound to the
            // device across a rotation would flush into the wrong generation.
            QTextStream stream(&logFile_);
            stream.setEncoding(QStringConverter::Utf8);
            stream << formatted << Qt::endl;
        }
        logFile_.flush();

        // GitHub #151 — enforce the cap after every accepted write. Checked
        // AFTER rather than before so the message that crosses the boundary is
        // preserved in the generation it belongs to instead of opening the new
        // file with a fragment of the old session's context.
        rotateIfNeededLocked();
    }
}

bool LogHandler::shouldLog(QtMsgType type) const {
    // Map QtMsgType to Level
    Level messageLevel;
    switch (type) {
        case QtDebugMsg:
            messageLevel = Debug;
            break;
        case QtInfoMsg:
            messageLevel = Info;
            break;
        case QtWarningMsg:
            messageLevel = Warning;
            break;
        case QtCriticalMsg:
            messageLevel = Critical;
            break;
        case QtFatalMsg:
            messageLevel = Fatal;
            break;
        default:
            messageLevel = Warning;
            break;
    }

    return messageLevel >= minLevel_;
}

QString LogHandler::formatMessage(QtMsgType type, const QMessageLogContext& context, const QString& msg) {
    QString timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz");
    QString level = levelString(type);
    QString category = context.category ? QString::fromUtf8(context.category) : QString("default");

    return QString("[%1] [%2] [%3] %4")
        .arg(timestamp)
        .arg(level)
        .arg(category)
        .arg(msg);
}

QString LogHandler::levelString(QtMsgType type) {
    switch (type) {
        case QtDebugMsg:
            return "DEBUG";
        case QtInfoMsg:
            return "INFO";
        case QtWarningMsg:
            return "WARNING";
        case QtCriticalMsg:
            return "CRITICAL";
        case QtFatalMsg:
            return "FATAL";
        default:
            return "UNKNOWN";
    }
}

} // namespace pdfe::app
