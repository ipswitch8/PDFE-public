#include "app/SingleInstanceGuard.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QLocalServer>
#include <QLocalSocket>
#include <QSharedMemory>

#include <memory>

namespace pdfe::app {

namespace {

// The wire protocol is one UTF-8 line per file path, each terminated by '\n'.
// A newline is a safe terminator because Windows and POSIX path separators are
// never '\n'; an empty path (a bare "raise the window" request) is just
// a lone terminator.
//
// GitHub #241 widened this from exactly one line to N, so a secondary launched
// from an Explorer multi-selection can forward every path in one request. N=1 is
// byte-identical to what shipped before.
constexpr char kTerminator = '\n';

}  // namespace

QString SingleInstanceGuard::defaultServerName()
{
    // Per-user so distinct logged-in users on the same host do not share a
    // single-instance domain (and RDP/multi-session boxes stay isolated).
    // qEnvironmentVariable falls back gracefully when USERNAME is unset.
    QString user = qEnvironmentVariable("USERNAME");
    if (user.isEmpty()) {
        user = qEnvironmentVariable("USER");
    }
    if (user.isEmpty()) {
        user = QStringLiteral("default");
    }
    QString name = QStringLiteral("PDFE-single-instance-") + user;

    // F63 — the domain is additionally partitionable by PDFE_INSTANCE_DOMAIN.
    //
    // Per-user is the right granularity for a HUMAN, and the wrong one for a
    // TEST HOST, where several unrelated processes run concurrently as the same
    // account. Measured, not reasoned about: five concurrent `pdfe_tests.exe`
    // processes each launching their own primary+secondary pair collide, and
    // exactly the two cases that depend on the DEFAULT single-instance
    // behaviour fail — one process's "primary" forwards to a stranger's and
    // never shows a window, which reads as "the primary instance never showed a
    // main window" and sends a maintainer into the product. Serial execution of
    // the same five cases passes every time.
    //
    // This is deliberately a SUFFIX and never a replacement: the per-user part
    // stays, so a partitioned domain can never widen to another account.
    //
    // It grants no capability that was not already there. PDFE_DISABLE_SINGLE_-
    // INSTANCE already lets a caller opt out of single-instance entirely; this
    // is strictly narrower — it keeps the guarantee and moves which processes
    // share it. Unset (the shipping default for every real user), the name is
    // byte-identical to what it was before.
    const QString domain = qEnvironmentVariable("PDFE_INSTANCE_DOMAIN");
    if (!domain.isEmpty()) {
        name += QLatin1Char('-') + domain;
    }
    return name;
}

SingleInstanceGuard::SingleInstanceGuard(const QString& serverName,
                                         int connectTimeoutMs,
                                         QObject* parent)
    : QObject(parent),
      serverName_(serverName.isEmpty() ? defaultServerName() : serverName),
      connectTimeoutMs_(connectTimeoutMs)
{
    // Discriminator: a tiny shared-memory segment keyed off the (per-user)
    // server name. The FIRST instance create()s it and becomes primary; a later
    // instance's create() fails with AlreadyExists and it becomes secondary.
    // Unlike a throwaway connect-probe, this consumes NO pipe instance, so the
    // secondary's subsequent forward is the first-ever connection to the
    // primary's freshly-armed server — reliable in-process (tests) and across
    // processes, with no named-pipe instance-exhaustion race.
    sharedMemory_ = new QSharedMemory(serverName_ + QStringLiteral("-shm"), this);

    bool created = sharedMemory_->create(1);
    if (!created && sharedMemory_->error() == QSharedMemory::AlreadyExists) {
        // A crashed primary can leave the segment behind on POSIX. attach()+
        // detach() lets the OS reap an orphan (the last detach frees it) so the
        // retry can claim it. A LIVE primary still holds the segment, so this
        // can never steal an active instance — the retry create() then still
        // fails with AlreadyExists and we correctly stay secondary. On Windows
        // the segment vanishes with its owner, so this path is a harmless no-op.
        sharedMemory_->attach();
        sharedMemory_->detach();
        created = sharedMemory_->create(1);
    }

    if (created) {
        // We own the discriminator → PRIMARY. Stand up the message server.
        isPrimary_ = true;
        server_ = new QLocalServer(this);
        // Do not auto-close for other users; the name is already per-user.
        server_->setSocketOptions(QLocalServer::UserAccessOption);
        if (!server_->listen(serverName_)) {
            // A stale server endpoint left by a crashed primary can block
            // listen(); removeServer clears it so listen() succeeds.
            QLocalServer::removeServer(serverName_);
            if (!server_->listen(serverName_)) {
                // Could not claim the server name. Degrade safely: still PRIMARY
                // (a window opens) but cross-instance forwarding is unavailable
                // this session — never a data-loss regression beyond the
                // pre-guard status quo for this one edge.
                delete server_;
                server_ = nullptr;
                return;
            }
        }
        connect(server_, &QLocalServer::newConnection,
                this, &SingleInstanceGuard::onNewConnection);
        return;
    }

    if (sharedMemory_->error() == QSharedMemory::AlreadyExists) {
        // A live primary already owns the segment → SECONDARY.
        isPrimary_ = false;
        return;
    }

    // create() failed for another reason (permissions, resource limits, …).
    // Rather than silently spawn a rival editor that could clobber saves, we
    // degrade to PRIMARY WITHOUT a server: a window still opens; forwarding is
    // unavailable. This is the safe degradation, never a data-loss regression.
    isPrimary_ = true;
}

SingleInstanceGuard::~SingleInstanceGuard()
{
    // server_ and sharedMemory_ are parented to this and destroyed
    // automatically; QLocalServer's destructor calls close() and
    // QSharedMemory's destructor detaches (releasing the OS refcount).
}

bool SingleInstanceGuard::sendPathsToPrimary(const QStringList& filePaths)
{
    if (isPrimary_) {
        // The primary never forwards to itself.
        return false;
    }

    // No connect-probe consumed a pipe instance during discovery, so this is the
    // first-ever connection to the primary's freshly-armed server.
    //
    // ONE connection carrying N lines, deliberately. An earlier attempt made one
    // connection PER PATH instead, and that loses paths: the design above assumes
    // a single connection per secondary lifetime, and rapid sequential connects
    // outrun the server's re-arm. Measured: one path of three arrived while all
    // three sends reported success — connected, written, flushed, never accepted.
    QLocalSocket socket;
    socket.connectToServer(serverName_);
    if (!socket.waitForConnected(connectTimeoutMs_)) {
        return false;
    }

    // One terminated line per path. An empty list still writes a single empty
    // line, preserving the bare "raise the window" request.
    QByteArray payload;
    if (filePaths.isEmpty()) {
        payload.append(kTerminator);
    } else {
        for (const QString& filePath : filePaths) {
            payload.append(filePath.toUtf8());
            payload.append(kTerminator);
        }
    }
    socket.write(payload);

    // Drain the write by PUMPING the event loop rather than a blocking
    // waitForBytesWritten. When the primary lives in the SAME thread (unit
    // tests, or an in-process forward), the pipe only flushes once the primary
    // ACCEPTS the connection — and that accept runs on THIS thread's event loop
    // (QLocalServer::newConnection). A blocking wait would starve that server,
    // so the bytes would never leave Qt's write buffer (observed: bytesToWrite
    // stays == payload size until timeout). Across processes the remote
    // primary's own loop accepts promptly, so the drain completes on the first
    // iteration. Bounded by connectTimeoutMs_.
    QElapsedTimer timer;
    timer.start();
    while (socket.bytesToWrite() > 0 && timer.elapsed() < connectTimeoutMs_) {
        socket.flush();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    if (socket.bytesToWrite() > 0) {
        return false;
    }

    // Non-blocking settle so the primary can finish reading before we tear the
    // socket down (again pumping, not blocking, to keep a same-thread primary
    // making progress).
    socket.disconnectFromServer();
    QElapsedTimer settle;
    settle.start();
    while (socket.state() != QLocalSocket::UnconnectedState &&
           settle.elapsed() < connectTimeoutMs_) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return true;
}

bool SingleInstanceGuard::sendPathToPrimary(const QString& filePath)
{
    // The one-path case. Kept as its own entry point because callers and tests
    // that mean "forward this one file" should not have to build a list to say
    // so, and it is byte-identical on the wire to what it has always been.
    return sendPathsToPrimary(QStringList{filePath});
}

void SingleInstanceGuard::onNewConnection()
{
    while (server_ && server_->hasPendingConnections()) {
        QLocalSocket* client = server_->nextPendingConnection();
        if (!client) {
            continue;
        }
        // Read the single terminated line ASYNCHRONOUSLY. A blocking read here
        // deadlocks: newConnection can be delivered reentrantly while a SAME-
        // PROCESS secondary is still pumping inside sendPathToPrimary(), so a
        // blocking waitForReadyRead would stall the whole chain. Per-connection
        // buffer + readyRead/disconnected handlers keep the slot non-blocking
        // and re-entrancy-safe.
        auto buffer = std::make_shared<QByteArray>();

        // GitHub #241 — emit EVERY complete line, not just the first.
        //
        // Each line is removed from the buffer before it is emitted, so draining
        // again can only ever emit lines that are newly complete. That is what
        // makes it safe to drain on BOTH readyRead and the peer's close: a
        // multi-path payload whose tail arrives in a later chunk is still
        // delivered, and nothing is delivered twice.
        //
        // Deliberately no "already emitted" latch and no disconnect from this
        // side. An earlier version latched after the first drain and closed the
        // socket, which is precisely how a tail still in flight would be lost.
        auto drain = [this, client, buffer]() {
            buffer->append(client->readAll());
            int term = buffer->indexOf(kTerminator);
            while (term >= 0) {
                const QString filePath = QString::fromUtf8(buffer->left(term));
                buffer->remove(0, term + 1);
                // A real request — even for an empty ("activate the window")
                // path — always writes the terminator, so a received terminator
                // is the discriminator for a genuine forward request.
                emit fileOpenRequested(filePath);
                term = buffer->indexOf(kTerminator);
            }
        };

        connect(client, &QLocalSocket::readyRead, this, drain);
        connect(client, &QLocalSocket::disconnected, this, [client, drain]() {
            // Peer closed — the end of the request. Drain the tail (data and FIN
            // can coalesce) and reap.
            drain();
            client->deleteLater();
        });

        // Bytes may already be buffered before readyRead was connected.
        if (client->bytesAvailable() > 0) {
            drain();
        }
    }
}

}  // namespace pdfe::app
