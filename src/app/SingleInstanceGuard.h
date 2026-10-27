#pragma once

#include <QObject>
#include <QString>

class QLocalServer;
class QSharedMemory;

namespace pdfe::app {

/// R3-02 single-instance / same-file data-loss guard.
///
/// PDFE is a single-window desktop editor. Launching it twice on the SAME
/// file (e.g. double-clicking a PDF while an editor is already open on it)
/// previously spawned a second independent editor holding the same document
/// — two editors that can each write the file, opening a concurrent-save
/// data-loss window (whichever saves last silently clobbers the other's
/// edits). This guard closes that window by making the SECOND launch forward
/// its file-open request to the already-running FIRST instance instead of
/// creating its own MainWindow.
///
/// Mechanism (QSharedMemory discriminator + QLocalServer message channel):
///   * The constructor tries to CREATE a tiny per-user QSharedMemory segment.
///     - create() succeeds → no primary yet → we are PRIMARY (isPrimary() ==
///       true). We also start a QLocalServer to receive forwarded paths. The
///       caller builds the MainWindow and connects fileOpenRequested() to its
///       open method (raising/activating the window on each request).
///     - create() fails with AlreadyExists → a primary is already running → we
///       are SECONDARY (isPrimary() == false). The caller forwards the
///       CLI/dropped path via sendPathToPrimary() and exits without a window.
///   * A shared-memory segment is refcounted by the OS and released when the
///     last handle closes, so a crashed primary never wedges the app into a
///     permanent "secondary" state (on Windows the segment vanishes with its
///     owner). A stale QLocalServer name is reclaimed (QLocalServer::removeServer)
///     before listening for the same reason.
///   * Discrimination is done WITHOUT a throwaway connect-probe, so the
///     secondary's forward is the first-ever connection to the primary's
///     freshly-armed pipe instance — reliable in-process (tests) and across
///     processes, with no instance-exhaustion race.
///
/// Forward-to-running-instance is applied for ANY file path (single-window
/// app behavior), which subsumes — and is strictly stronger than — the
/// same-file data-loss case in the acceptance criterion.
///
/// The server name is injectable (constructor argument) so tests get an
/// isolated name that never collides with a real running PDFE (or with each
/// other). Production passes the default per-user name.
class SingleInstanceGuard : public QObject {
    Q_OBJECT

public:
    /// Construct and immediately attempt to become the primary instance for
    /// @p serverName. Pass an empty string to use defaultServerName().
    /// @p connectTimeoutMs bounds the secondary's probe of an existing
    /// primary; the default is generous enough for a live desktop session yet
    /// short enough to keep test event loops snappy.
    explicit SingleInstanceGuard(const QString& serverName = QString(),
                                 int connectTimeoutMs = 500,
                                 QObject* parent = nullptr);
    ~SingleInstanceGuard() override;

    SingleInstanceGuard(const SingleInstanceGuard&) = delete;
    SingleInstanceGuard& operator=(const SingleInstanceGuard&) = delete;

    /// True iff THIS process owns the single-instance server (i.e. is the
    /// running editor). False when another instance already holds it.
    bool isPrimary() const { return isPrimary_; }

    /// The resolved server name this guard used (default or injected).
    QString serverName() const { return serverName_; }

    /// Secondary path: forward @p filePath to the running primary so it opens
    /// the document in its existing window. Returns true if the request was
    /// written and flushed to the primary. A primary instance never needs
    /// this; calling it on a primary returns false. An empty @p filePath is
    /// still forwarded as an empty request (a bare "raise the window" signal).
    bool sendPathToPrimary(const QString& filePath);

    /// GitHub #241 — forward SEVERAL paths in one request.
    ///
    /// Explorer may hand a multi-selection to a single invocation, in which case
    /// a secondary launch has N paths to pass on, not one. Each path is written
    /// as its own terminated line and the primary emits fileOpenRequested() once
    /// per path, so the wire format is unchanged for a single path.
    ///
    /// An empty list still sends one empty line — the bare "raise the window"
    /// request — which is what sendPathToPrimary(QString()) means.
    bool sendPathsToPrimary(const QStringList& filePaths);

    /// The default per-user server name: the application name plus the current
    /// OS user, so distinct logged-in users on the same machine each get their
    /// own single-instance domain (and a per-user path never collides across
    /// sessions).
    static QString defaultServerName();

signals:
    /// Emitted on the PRIMARY instance when a secondary forwards a file to
    /// open. @p filePath is the absolute/relative path the secondary was
    /// launched with; it may be empty (a pure activate-the-window request).
    void fileOpenRequested(const QString& filePath);

private:
    void onNewConnection();

    QString serverName_;
    int connectTimeoutMs_ = 500;
    bool isPrimary_ = false;
    QLocalServer* server_ = nullptr;         // owned via QObject parent when primary
    QSharedMemory* sharedMemory_ = nullptr;  // discriminator; owned via QObject parent
};

}  // namespace pdfe::app
