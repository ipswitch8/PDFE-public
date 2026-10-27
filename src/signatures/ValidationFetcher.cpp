// -*- coding: utf-8 -*-
#include "ValidationFetcher.h"

#include <QDebug>
#include <QHash>
#include <QLoggingCategory>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QSslConfiguration>
#include <QSslError>
#include <QSslSocket>
#include <QStringList>

namespace pdfe::signatures {

namespace {
Q_LOGGING_CATEGORY(lcVal, "pdfe.signatures.fetch")

constexpr int kDefaultTimeoutMs = 15000;
// Pre-parser size caps — bound peak heap usage if a malicious or
// misconfigured server tries to push huge bodies. Mirrored against the
// downstream parsers (OcspResponseParser kMaxOcspBytes=16 MiB,
// CrlParser kMaxCrlBytes=64 MiB) so we abort *before* allocating the
// full body, not after. Enforced via Content-Length on the request and
// incremental tracking on readyRead.
constexpr qint64 kMaxOcspBytes = 16LL * 1024 * 1024;
constexpr qint64 kMaxCrlBytes  = 64LL * 1024 * 1024;
}

class ValidationFetcher::Impl {
public:
    QNetworkAccessManager* nam = nullptr;     // either owned_ or external
    std::unique_ptr<QNetworkAccessManager> owned_;
    int timeoutMs = kDefaultTimeoutMs;
    QHash<QNetworkReply*, ValidationFetcher::Kind> kinds;
};

static qint64 capForKind(ValidationFetcher::Kind k)
{
    return k == ValidationFetcher::Kind::Ocsp ? kMaxOcspBytes : kMaxCrlBytes;
}

ValidationFetcher::ValidationFetcher(QObject* parent)
    : QObject(parent), impl_(std::make_unique<Impl>())
{
    // No QObject parent on the NAM: ownership is exclusively via the
    // unique_ptr below. Setting parent=this AND owning via unique_ptr
    // would double-delete on ~ValidationFetcher.
    impl_->owned_ = std::make_unique<QNetworkAccessManager>();
    impl_->nam = impl_->owned_.get();
}

ValidationFetcher::ValidationFetcher(QNetworkAccessManager* nam,
                                      QObject* parent)
    : QObject(parent), impl_(std::make_unique<Impl>())
{
    impl_->nam = nam;
    // Caller owns nam; we do not adopt it.
}

ValidationFetcher::~ValidationFetcher() = default;

void ValidationFetcher::setTimeoutMs(int ms)
{
    if (ms > 0) impl_->timeoutMs = ms;
}

int ValidationFetcher::inflightCount() const
{
    return impl_->kinds.size();
}

// -------------------------------------------------------------------------
// Common reply wiring.
//
// We rely on QNetworkReply::finished for the success path AND the
// HTTP-level failure path (Qt fires finished after error()). sslErrors
// is connected with the explicit "no ignore" handler — if it fires we
// abort and surface the error.
// -------------------------------------------------------------------------
void ValidationFetcher::wireReply(QNetworkReply* reply, Kind kind)
{
    if (!reply) return;
    impl_->kinds.insert(reply, kind);

    // SECURITY: connect sslErrors *first*, before finished, so any TLS
    // failure has a chance to abort before payload bytes flow. We do
    // NOT call ignoreSslErrors() — peer verification stays strict.
    connect(reply, &QNetworkReply::sslErrors,
            this, &ValidationFetcher::onSslErrors);

    // SECURITY: bound peak heap by aborting before unbounded buffering.
    // (Pre-parser cap; the downstream parsers also cap, but this prevents
    // the response from ever sitting in our heap above the limit.)
    const qint64 cap = capForKind(kind);
    connect(reply, &QNetworkReply::metaDataChanged, this, [reply, cap]() {
        QVariant v = reply->header(QNetworkRequest::ContentLengthHeader);
        bool ok = false;
        qint64 announced = v.toLongLong(&ok);
        if (ok && announced > cap) {
            reply->setProperty("pdfe_aborted_oversize", true);
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::readyRead, this, [reply, cap]() {
        if (reply->bytesAvailable() > cap) {
            reply->setProperty("pdfe_aborted_oversize", true);
            reply->abort();
        }
    });

    connect(reply, &QNetworkReply::finished,
            this, &ValidationFetcher::onReplyFinished);
}

void ValidationFetcher::fetchOcsp(const QUrl& responderUrl,
                                    const QByteArray& ocspRequestDer)
{
    if (!impl_->nam) {
        emit fetchFailed(tr("No QNetworkAccessManager available"));
        return;
    }
    if (!responderUrl.isValid() || responderUrl.scheme().isEmpty()) {
        emit fetchFailed(tr("Invalid OCSP responder URL"));
        return;
    }
    if (responderUrl.scheme().compare(QStringLiteral("https"),
                                       Qt::CaseInsensitive) == 0
        && !QSslSocket::supportsSsl()) {
        emit fetchFailed(tr("HTTPS OCSP requested but SSL/TLS support is "
                            "not available in this Qt build"));
        return;
    }

    qCInfo(lcVal) << "POST OCSP to" << responderUrl.toString();

    QNetworkRequest req(responderUrl);
    req.setRawHeader("Content-Type", "application/ocsp-request");
    req.setHeader(QNetworkRequest::ContentLengthHeader,
                  ocspRequestDer.size());
    req.setRawHeader("User-Agent", "PDFE-Validation/1.0");

    // Per-request timeout (Qt 5.15+).
    req.setTransferTimeout(impl_->timeoutMs);

    // Strict TLS: VerifyPeer is the QSslConfiguration default. We make it
    // explicit so future Qt-level default changes do not silently relax it.
    QSslConfiguration tls = QSslConfiguration::defaultConfiguration();
    tls.setPeerVerifyMode(QSslSocket::VerifyPeer);
    req.setSslConfiguration(tls);

    QNetworkReply* reply = impl_->nam->post(req, ocspRequestDer);
    wireReply(reply, Kind::Ocsp);
}

void ValidationFetcher::fetchCrl(const QUrl& crlUrl)
{
    if (!impl_->nam) {
        emit fetchFailed(tr("No QNetworkAccessManager available"));
        return;
    }
    if (!crlUrl.isValid() || crlUrl.scheme().isEmpty()) {
        emit fetchFailed(tr("Invalid CRL URL"));
        return;
    }
    if (crlUrl.scheme().compare(QStringLiteral("https"),
                                 Qt::CaseInsensitive) == 0
        && !QSslSocket::supportsSsl()) {
        emit fetchFailed(tr("HTTPS CRL requested but SSL/TLS support is "
                            "not available in this Qt build"));
        return;
    }

    qCInfo(lcVal) << "GET CRL from" << crlUrl.toString();

    QNetworkRequest req(crlUrl);
    req.setRawHeader("User-Agent", "PDFE-Validation/1.0");
    req.setTransferTimeout(impl_->timeoutMs);

    QSslConfiguration tls = QSslConfiguration::defaultConfiguration();
    tls.setPeerVerifyMode(QSslSocket::VerifyPeer);
    req.setSslConfiguration(tls);

    QNetworkReply* reply = impl_->nam->get(req);
    wireReply(reply, Kind::Crl);
}

void ValidationFetcher::onSslErrors(const QList<QSslError>& errors)
{
    QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;

    QStringList parts;
    parts.reserve(errors.size());
    for (const QSslError& err : errors) {
        parts << err.errorString();
    }
    QString hostname = reply->url().host();
    QString joined = parts.isEmpty() ? tr("unknown TLS error")
                                      : parts.join(QStringLiteral("; "));

    qCWarning(lcVal) << "TLS error talking to" << hostname << ":" << joined;

    // Do NOT call ignoreSslErrors. Abort the reply so finished() will
    // fire with a NetworkError, but emit our descriptive failure first
    // so the UI sees the original cause rather than the abort.
    impl_->kinds.remove(reply);
    reply->abort();
    reply->deleteLater();

    emit fetchFailed(tr("TLS error talking to %1: %2")
                       .arg(hostname, joined));
}

void ValidationFetcher::onReplyFinished()
{
    QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;

    auto it = impl_->kinds.find(reply);
    if (it == impl_->kinds.end()) {
        // Already handled by sslErrors path.
        reply->deleteLater();
        return;
    }
    Kind kind = it.value();
    impl_->kinds.erase(it);

    QNetworkReply::NetworkError err = reply->error();
    int httpStatus = reply
        ->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QString errStr = reply->errorString();
    QUrl url = reply->url();

    if (err == QNetworkReply::OperationCanceledError) {
        // Distinguish our own oversize-abort from sslErrors-driven abort.
        if (reply->property("pdfe_aborted_oversize").toBool()) {
            emit fetchFailed(tr("Response body exceeded %1 byte cap; "
                                "rejected before parsing")
                               .arg(capForKind(kind)));
        }
        // Otherwise sslErrors path already emitted fetchFailed.
        reply->deleteLater();
        return;
    }

    if (err != QNetworkReply::NoError) {
        qCWarning(lcVal) << "Fetch failed:" << url.toString()
                          << "code=" << err
                          << "http=" << httpStatus
                          << "msg=" << errStr;
        emit fetchFailed(tr("Network error fetching %1: %2 (HTTP %3)")
                           .arg(url.toString(), errStr)
                           .arg(httpStatus));
        reply->deleteLater();
        return;
    }

    if (httpStatus >= 400) {
        emit fetchFailed(tr("HTTP %1 fetching %2")
                           .arg(httpStatus).arg(url.toString()));
        reply->deleteLater();
        return;
    }

    QByteArray body = reply->readAll();
    qCInfo(lcVal) << "Fetched" << body.size() << "bytes from"
                   << url.toString();
    // PRIVACY: response bytes are NOT logged. Only the byte count.
    reply->deleteLater();

    if (kind == Kind::Ocsp) {
        emit ocspReceived(body);
    } else {
        emit crlReceived(body);
    }
}

} // namespace pdfe::signatures
