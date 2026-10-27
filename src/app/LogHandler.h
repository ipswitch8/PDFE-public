#pragma once
#include <QString>
#include <QFile>
#include <QMutex>
#include <QtMessageHandler>

namespace pdfe::app {

class LogHandler {
public:
    enum Level { Debug = 0, Info = 1, Warning = 2, Critical = 3, Fatal = 4 };

    static LogHandler& instance();

    void install();
    void uninstall();

    void setMinLevel(Level level);
    Level minLevel() const;

    void setFileLogging(bool enabled, const QString& path = {});
    bool isFileLogging() const;
    QString logFilePath() const;

    // -----------------------------------------------------------------------
    // SIZE CAP AND ROTATION (GitHub #151)
    // -----------------------------------------------------------------------
    // The log was opened Append at every launch and nothing ever bounded it —
    // no cap, no age gate, no pruning. It grew monotonically for the life of
    // the installation, accumulating whatever the product logs (page indices,
    // exception text, and, the moment anyone drops minLevel to Debug to chase
    // a problem, literal document characters).
    //
    // The cap is enforced by ROTATION rather than by refusing to log, because
    // a logger that silently stops at a threshold is worse than no logger: the
    // interesting messages are the last ones.
    //
    // EXACTLY ONE BACKUP GENERATION IS KEPT, and that is the privacy half of
    // the design, not an arbitrary number. Rotation does not make the retained
    // data disappear — it MOVES it into `<log>.1`, which is just as readable.
    // A scheme with N generations therefore retains (N+1) x cap bytes of the
    // user's diagnostics forever. With one generation, total on-disk retention
    // is bounded at 2 x maxFileBytes() and the oldest generation is deleted at
    // every rotation, so nothing accumulates. Both files live in the same
    // per-user directory and inherit its ACL; rotation never widens it, because
    // it renames rather than copies.
    static constexpr qint64 kDefaultMaxFileBytes = 1024 * 1024;  // 1 MiB
    /// Floor for setMaxFileBytes — below this a single long message could
    /// rotate the file on every line, which is a churn bug, not a cap.
    static constexpr qint64 kMinMaxFileBytes = 1024;

    /// Bytes the active log may reach before it is rotated. Values below
    /// kMinMaxFileBytes are clamped up.
    void setMaxFileBytes(qint64 bytes);
    qint64 maxFileBytes() const;

    /// Path of the single retained backup generation (`<logFilePath()>.1`),
    /// or an empty string when file logging is off. Public so a test can assert
    /// on the real file rather than on a return value.
    QString rotatedLogFilePath() const;

    /// Rotations performed since the process started. A counter, not a claim:
    /// it lets a test distinguish "stayed small because it rotated" from
    /// "stayed small because nothing was written".
    int rotationCount() const;

    // For testing: access last logged message
    struct LogEntry {
        QtMsgType type = QtDebugMsg;
        QString category;
        QString message;
        QString file;
        int line = 0;
    };
    LogEntry lastEntry() const;

private:
    LogHandler();
    ~LogHandler();
    LogHandler(const LogHandler&) = delete;
    LogHandler& operator=(const LogHandler&) = delete;

    static void messageHandler(QtMsgType type,
                               const QMessageLogContext& context,
                               const QString& msg);

    void handleMessage(QtMsgType type,
                       const QMessageLogContext& context,
                       const QString& msg);

    bool shouldLog(QtMsgType type) const;

    /// Rotate if the active log has reached the cap. Caller MUST already hold
    /// mutex_ (both call sites do); it never takes the lock itself.
    void rotateIfNeededLocked();
    static QString backupPathFor(const QString& path);
    static QString formatMessage(QtMsgType type, const QMessageLogContext& context, const QString& msg);
    static QString levelString(QtMsgType type);

    mutable QMutex mutex_;
    Level minLevel_ = Warning;
    bool fileLogging_ = false;
    QString logPath_;
    QFile logFile_;
    QtMessageHandler previousHandler_ = nullptr;
    LogEntry lastEntry_;
    bool installed_ = false;
    qint64 maxFileBytes_ = kDefaultMaxFileBytes;
    int rotationCount_ = 0;
};

} // namespace pdfe::app
