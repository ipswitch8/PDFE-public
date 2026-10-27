// -*- coding: utf-8 -*-
#pragma once

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QSslError>
#include <QString>
#include <QUrl>

#include <memory>

class QNetworkAccessManager;
class QNetworkReply;

namespace pdfe::signatures {

/**
 * @brief Asynchronous fetcher for OCSP responses and CRLs.
 *
 * SECURITY MODEL (Pipeline F phase-9):
 *
 *   1. NEVER call ignoreSslErrors(). The default QSslConfiguration uses
 *      QSslSocket::VerifyPeer; sslErrors signal triggers reply->abort()
 *      and emits fetchFailed() with the hostname so the user can see
 *      *which* server failed verification.
 *
 *   2. NO event-loop spinning on the UI thread. All requests are issued
 *      via QNetworkAccessManager::get/post which return immediately;
 *      results arrive on QNetworkReply::finished. UI thread is never
 *      blocked, even on a 30-second timeout.
 *
 *   3. Tests inject a custom QNetworkAccessManager (mock) via the
 *      two-argument constructor. The test mock subclasses
 *      QNetworkAccessManager::createRequest() to short-circuit network
 *      I/O. Production code uses the default constructor which creates
 *      its own NAM.
 *
 *   4. Per-request transfer timeout is 15 s (QNetworkRequest::setTransferTimeout).
 *      No request stays in flight past that window.
 *
 *   5. The OCSP request is sent as POST with
 *      Content-Type: application/ocsp-request per RFC 2560. The CRL is
 *      fetched via GET (CRL DP URLs are static blob refs).
 */
class ValidationFetcher : public QObject {
    Q_OBJECT
public:
    explicit ValidationFetcher(QObject* parent = nullptr);

    /// Test/DI variant: inject a custom QNetworkAccessManager. Ownership
    /// stays with the caller; @p nam must outlive this fetcher.
    ValidationFetcher(QNetworkAccessManager* nam, QObject* parent);

    ~ValidationFetcher() override;

    ValidationFetcher(const ValidationFetcher&) = delete;
    ValidationFetcher& operator=(const ValidationFetcher&) = delete;

    /// POST an OCSP request DER body to @p responderUrl. Async. Emits
    /// ocspReceived(QByteArray) on 200-OK, fetchFailed(reason) otherwise.
    void fetchOcsp(const QUrl& responderUrl,
                   const QByteArray& ocspRequestDer);

    /// GET the CRL at @p crlUrl. Async. Emits crlReceived(QByteArray)
    /// on 200-OK, fetchFailed(reason) otherwise.
    void fetchCrl(const QUrl& crlUrl);

    /// Override default 15-second transfer timeout (ms). Tests use this
    /// to drive deterministic failure paths.
    void setTimeoutMs(int ms);

    /// Number of requests currently in flight (test introspection).
    int inflightCount() const;

signals:
    void ocspReceived(QByteArray der);
    void crlReceived(QByteArray der);

    /// Emitted on TLS failure, HTTP non-2xx, transport error, or timeout.
    /// @p reason is a single-line human-readable description, safe to
    /// embed into a QMessageBox::critical text body.
    void fetchFailed(QString reason);

public:
    /// Internal request kind. Public so size-cap helpers in the .cpp
    /// can refer to it from anonymous-namespace free functions.
    enum class Kind { Ocsp, Crl };

private slots:
    void onReplyFinished();
    void onSslErrors(const QList<QSslError>& errors);

private:
    void wireReply(QNetworkReply* reply, Kind kind);

    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pdfe::signatures
