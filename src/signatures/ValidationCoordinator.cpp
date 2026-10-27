// -*- coding: utf-8 -*-
#include "ValidationCoordinator.h"

#include "CrlParser.h"
#include "CrlUrlExtractor.h"
#include "LtvDssWriter.h"
#include "OcspCrlData.h"
#include "OcspResponseParser.h"
#include "OcspUrlExtractor.h"
#include "ValidationFetcher.h"
#include "core/Document.h"
#include "ui/ValidationRequestDialog.h"

#include <QByteArray>
#include <QDebug>
#include <QLoggingCategory>
#include <QMessageBox>
#include <QPointer>
#include <QStringList>
#include <QUrl>
#include <QWidget>

#include <openssl/asn1.h>
#include <openssl/evp.h>
#include <openssl/ocsp.h>
#include <openssl/x509.h>

#include <cstring>

namespace pdfe::signatures {

namespace {
Q_LOGGING_CATEGORY(lcCoord, "pdfe.signatures.coordinator")
}

class ValidationCoordinator::Impl {
public:
    QPointer<QWidget> dialogParent;
    pdfe::core::Document* doc = nullptr;
    std::unique_ptr<ValidationFetcher> fetcher;
    QNetworkAccessManager* externalNam = nullptr;

    QStringList ocspUrls;
    QStringList crlUrls;

    std::vector<std::vector<uint8_t>> certChain;
    std::string contentsHex;

    std::vector<std::vector<uint8_t>> ocspDers;
    std::vector<std::vector<uint8_t>> crlDers;

    int pendingFetches = 0;
    bool failed = false;
    bool cancelled = false;

    // Test seams.
    bool skipDialog = false;
    bool dialogResult = false;
    QByteArray overrideOcspDer;
    QByteArray overrideCrlDer;
    QStringList testOcspUrls;
    QStringList testCrlUrls;
    bool useTestUrls = false;

    // F58 — see ValidationCoordinator::setWriteScope().
    std::function<void(const std::function<void()>&)> writeScope;

    // F58-g — see ValidationCoordinator::setLiveDocumentResolver().
    std::function<pdfe::core::Document*()> liveDoc;
    // Identity of the document the request was made against, captured at
    // request() time while it was certainly alive. `docPath` and `contentsHex`
    // together are what the resolver's answer is checked against.
    std::string docPath;
};

// F58-g. Deliberately NOT tr()'d: these are matched verbatim by
// ValidationRequestTest's lifetime controls, and the coordinator wraps them in
// a translated sentence at the point of display in exactly the way it already
// wraps LtvDssWriter::lastError().
const char* ValidationCoordinator::kDocumentNoLongerOpenMessage =
    "the document this validation request was made against has been closed";
const char* ValidationCoordinator::kDocumentReplacedMessage =
    "a different document is now open than the one this validation request was "
    "made against";

ValidationCoordinator::ValidationCoordinator(QObject* parent)
    : QObject(parent), impl_(std::make_unique<Impl>())
{
}

ValidationCoordinator::ValidationCoordinator(QNetworkAccessManager* nam,
                                              QObject* parent)
    : QObject(parent), impl_(std::make_unique<Impl>())
{
    impl_->externalNam = nam;
}

ValidationCoordinator::~ValidationCoordinator() = default;

void ValidationCoordinator::setSkipDialogForTests(bool skip)
{
    impl_->skipDialog = skip;
}

void ValidationCoordinator::setTestDialogResult(bool acceptIt)
{
    impl_->dialogResult = acceptIt;
}

void ValidationCoordinator::setOverrideOcspDer(const QByteArray& der)
{
    impl_->overrideOcspDer = der;
}

void ValidationCoordinator::setOverrideCrlDer(const QByteArray& der)
{
    impl_->overrideCrlDer = der;
}

void ValidationCoordinator::setTestUrls(const QStringList& ocsp,
                                         const QStringList& crl)
{
    impl_->testOcspUrls = ocsp;
    impl_->testCrlUrls = crl;
    impl_->useTestUrls = true;
}

void ValidationCoordinator::setWriteScope(
    std::function<void(const std::function<void()>& write)> scope)
{
    impl_->writeScope = std::move(scope);
}

void ValidationCoordinator::setLiveDocumentResolver(
    std::function<pdfe::core::Document*()> resolver)
{
    impl_->liveDoc = std::move(resolver);
}

QStringList ValidationCoordinator::lastOcspUrls() const
{
    return impl_->ocspUrls;
}

QStringList ValidationCoordinator::lastCrlUrls() const
{
    return impl_->crlUrls;
}

bool ValidationCoordinator::extractFirstSignerCertChain(
    pdfe::core::Document* doc,
    std::vector<std::vector<uint8_t>>& chain,
    std::string& contentsHex)
{
    SignatureInputs inputs;
    if (!LtvDssWriter::extractFirstSignatureInputs(doc, inputs))
        return false;

    if (inputs.certChain.empty()) return false;
    chain = inputs.certChain;
    contentsHex = inputs.contentsHex;
    return true;
}

QByteArray ValidationCoordinator::buildOcspRequestDer(
    const std::vector<uint8_t>& signerDer,
    const std::vector<uint8_t>& issuerDer,
    QString& errorOut)
{
    if (signerDer.empty() || issuerDer.empty()) {
        errorOut = tr("OCSP request: missing signer or issuer cert");
        return {};
    }

    const unsigned char* p1 = signerDer.data();
    X509* signer = d2i_X509(nullptr, &p1,
                             static_cast<long>(signerDer.size()));
    if (!signer) {
        errorOut = tr("OCSP request: failed to decode signer cert");
        return {};
    }
    const unsigned char* p2 = issuerDer.data();
    X509* issuer = d2i_X509(nullptr, &p2,
                             static_cast<long>(issuerDer.size()));
    if (!issuer) {
        X509_free(signer);
        errorOut = tr("OCSP request: failed to decode issuer cert");
        return {};
    }

    OCSP_REQUEST* req = OCSP_REQUEST_new();
    if (!req) {
        X509_free(signer); X509_free(issuer);
        errorOut = tr("OCSP request: OCSP_REQUEST_new failed");
        return {};
    }

    OCSP_CERTID* certId = OCSP_cert_to_id(EVP_sha1(), signer, issuer);
    if (!certId) {
        OCSP_REQUEST_free(req); X509_free(signer); X509_free(issuer);
        errorOut = tr("OCSP request: OCSP_cert_to_id failed");
        return {};
    }

    if (!OCSP_request_add0_id(req, certId)) {
        OCSP_CERTID_free(certId);
        OCSP_REQUEST_free(req); X509_free(signer); X509_free(issuer);
        errorOut = tr("OCSP request: OCSP_request_add0_id failed");
        return {};
    }
    // certId is now owned by req.

    unsigned char* der = nullptr;
    int derLen = i2d_OCSP_REQUEST(req, &der);
    OCSP_REQUEST_free(req);
    X509_free(signer); X509_free(issuer);

    if (derLen <= 0 || !der) {
        if (der) OPENSSL_free(der);
        errorOut = tr("OCSP request: i2d_OCSP_REQUEST failed");
        return {};
    }

    QByteArray out(reinterpret_cast<const char*>(der), derLen);
    OPENSSL_free(der);
    return out;
}

bool ValidationCoordinator::request(QWidget* parent,
                                     pdfe::core::Document* doc)
{
    if (!doc) {
        emit failed(tr("No document open"));
        return false;
    }

    impl_->dialogParent = parent;
    impl_->doc = doc;
    // F58-g: capture the identity of the target NOW, while it is certainly
    // alive, so tryWriteDss() has something to check the resolver's answer
    // against an unbounded time later. `contentsHex` (filled in just below) is
    // the other half and is the discriminating one — it identifies the exact
    // signature the fetched revocation material belongs to.
    impl_->docPath = doc->filePath().string();

    // Step 1: get the signer cert chain from the first signature.
    if (!extractFirstSignerCertChain(doc, impl_->certChain,
                                       impl_->contentsHex)) {
        emit failed(tr("No verified signature found in document"));
        return false;
    }

    // Step 2: extract OCSP + CRL URLs from cert[0].
    if (impl_->useTestUrls) {
        impl_->ocspUrls = impl_->testOcspUrls;
        impl_->crlUrls = impl_->testCrlUrls;
    } else {
        std::vector<std::string> ocsp;
        std::vector<std::string> crl;
        OcspUrlExtractor::parseDer(impl_->certChain[0].data(),
                                    impl_->certChain[0].size(), ocsp);
        CrlUrlExtractor::parseDer(impl_->certChain[0].data(),
                                   impl_->certChain[0].size(), crl);

        impl_->ocspUrls.clear();
        impl_->crlUrls.clear();
        for (const auto& s : ocsp) impl_->ocspUrls << QString::fromStdString(s);
        for (const auto& s : crl)  impl_->crlUrls  << QString::fromStdString(s);
    }

    // Step 3: show the consent dialog.
    bool accepted = false;
    if (impl_->skipDialog) {
        accepted = impl_->dialogResult;
    } else {
        pdfe::ui::ValidationRequestDialog dlg(impl_->ocspUrls,
                                               impl_->crlUrls, parent);
        accepted = (dlg.exec() == QDialog::Accepted);
    }

    if (!accepted) {
        qCInfo(lcCoord) << "User cancelled validation fetch";
        emit cancelled();
        return true;
    }

    // Hard guard: a "Fetch" decision with no URLs is a programmer bug
    // (the dialog should have disabled the button), but be defensive.
    if (impl_->ocspUrls.isEmpty() && impl_->crlUrls.isEmpty()) {
        emit failed(tr("No validation URLs available"));
        return true;
    }

    // Step 4: build OCSP request and fire fetches.
    if (impl_->externalNam) {
        impl_->fetcher = std::make_unique<ValidationFetcher>(
            impl_->externalNam, this);
    } else {
        impl_->fetcher = std::make_unique<ValidationFetcher>(this);
    }
    connect(impl_->fetcher.get(), &ValidationFetcher::ocspReceived,
            this, &ValidationCoordinator::onOcspReceived);
    connect(impl_->fetcher.get(), &ValidationFetcher::crlReceived,
            this, &ValidationCoordinator::onCrlReceived);
    connect(impl_->fetcher.get(), &ValidationFetcher::fetchFailed,
            this, &ValidationCoordinator::onFetchFailed);

    impl_->ocspDers.clear();
    impl_->crlDers.clear();
    impl_->pendingFetches = 0;
    impl_->failed = false;
    impl_->cancelled = false;

    if (!impl_->ocspUrls.isEmpty()) {
        // Pick issuer cert. For a real chain it is certChain[1]. For a
        // self-signed leaf (test fixtures sometimes), fall back to the
        // leaf itself — OCSP_cert_to_id will produce a self-issued
        // CertID, which is structurally valid.
        const std::vector<uint8_t>& issuerDer =
            (impl_->certChain.size() >= 2)
                ? impl_->certChain[1] : impl_->certChain[0];
        QString err;
        QByteArray reqDer = buildOcspRequestDer(impl_->certChain[0],
                                                  issuerDer, err);
        if (reqDer.isEmpty()) {
            qCWarning(lcCoord) << "OCSP request build failed:" << err;
        } else {
            for (const QString& url : impl_->ocspUrls) {
                ++impl_->pendingFetches;
                impl_->fetcher->fetchOcsp(QUrl(url), reqDer);
            }
        }
    }

    for (const QString& url : impl_->crlUrls) {
        ++impl_->pendingFetches;
        impl_->fetcher->fetchCrl(QUrl(url));
    }

    if (impl_->pendingFetches == 0) {
        emit failed(tr("No fetchable validation URLs available"));
        return true;
    }

    return true;
}

void ValidationCoordinator::onOcspReceived(QByteArray der)
{
    if (impl_->failed || impl_->cancelled) return;

    QByteArray bytes = !impl_->overrideOcspDer.isEmpty()
        ? impl_->overrideOcspDer : der;

    OcspResponse parsed;
    if (!OcspResponseParser::parseBytes(
            reinterpret_cast<const uint8_t*>(bytes.constData()),
            static_cast<std::size_t>(bytes.size()), parsed)) {
        emitFailed(tr("Received OCSP response is malformed: %1")
                     .arg(QString::fromStdString(parsed.statusMessage)));
        return;
    }

    impl_->ocspDers.push_back(parsed.derBytes);
    --impl_->pendingFetches;
    if (impl_->pendingFetches <= 0) tryWriteDss();
}

void ValidationCoordinator::onCrlReceived(QByteArray der)
{
    if (impl_->failed || impl_->cancelled) return;

    QByteArray bytes = !impl_->overrideCrlDer.isEmpty()
        ? impl_->overrideCrlDer : der;

    CrlInfo parsed;
    if (!CrlParser::parseBytes(
            reinterpret_cast<const uint8_t*>(bytes.constData()),
            static_cast<std::size_t>(bytes.size()), parsed)) {
        emitFailed(tr("Received CRL is malformed: %1")
                     .arg(QString::fromStdString(parsed.statusMessage)));
        return;
    }

    impl_->crlDers.push_back(parsed.derBytes);
    --impl_->pendingFetches;
    if (impl_->pendingFetches <= 0) tryWriteDss();
}

void ValidationCoordinator::onFetchFailed(QString reason)
{
    emitFailed(reason);
}

void ValidationCoordinator::emitFailed(QString reason)
{
    if (impl_->failed) return;
    impl_->failed = true;

    qCWarning(lcCoord) << "Validation fetch failed:" << reason;

    QWidget* p = impl_->dialogParent ? impl_->dialogParent.data() : nullptr;
    if (!impl_->skipDialog) {
        QMessageBox::critical(p, tr("Validation Fetch Failed"), reason);
    }
    emit failed(reason);
    cleanup();
}

void ValidationCoordinator::tryWriteDss()
{
    if (impl_->failed) return;

    // -----------------------------------------------------------------------
    // F58-g (phase-53 review, finding 5) — LIFETIME AND IDENTITY GATE.
    //
    // This runs BEFORE anything — this function, the write scope, or
    // LtvDssWriter — touches the recorded Document*. Everything from here to
    // the write is on the far side of an unbounded asynchronous wait; see
    // setLiveDocumentResolver() for why that wait is unbounded and why nothing
    // else was watching it.
    //
    // The check is on the FILE PATH and the FIRST SIGNATURE'S /Contents, not on
    // the pointer. A pointer comparison is the check that looks right and is
    // wrong in both directions: a Document re-opened at a different address on
    // the same file with the same signature is a legitimate target, and a
    // DIFFERENT document handed the destroyed one's address by the allocator is
    // not, yet compares equal. The /Contents hex is decisive, because it is the
    // identity of the exact signature the fetched OCSP/CRL material was
    // requested for and is about to be filed under in /VRI.
    // -----------------------------------------------------------------------
    pdfe::core::Document* target = impl_->doc;
    if (impl_->liveDoc) {
        target = impl_->liveDoc();
        if (!target) {
            emitFailed(tr("Failed to write /DSS dictionary: %1")
                           .arg(QString::fromUtf8(kDocumentNoLongerOpenMessage)));
            return;
        }
        bool sameDocument = (target->filePath().string() == impl_->docPath);
        if (sameDocument) {
            // Re-extract rather than trust the path: two different documents can
            // legitimately share a path over time (close, edit elsewhere,
            // re-open), and only the signature tells them apart.
            SignatureInputs live;
            try {
                sameDocument =
                    LtvDssWriter::extractFirstSignatureInputs(target, live)
                    && live.contentsHex == impl_->contentsHex;
            } catch (const std::exception&) {
                // A document we cannot even read a signature out of is not the
                // one we fetched material for. Fail closed.
                sameDocument = false;
            }
        }
        if (!sameDocument) {
            emitFailed(tr("Failed to write /DSS dictionary: %1")
                           .arg(QString::fromUtf8(kDocumentReplacedMessage)));
            return;
        }
        impl_->doc = target;
    }

    LtvDssWriter writer;
    bool ok = false;
    // F58: the write now commits by renaming a staged file over the original,
    // which means it must release the fz_document first — so any Page the owner
    // holds has to be dropped and re-resolved around it. The owner installed
    // that sequence via setWriteScope(); without one, run the write directly.
    auto doWrite = [&]() {
        ok = writer.write(impl_->doc, impl_->contentsHex,
                          impl_->certChain,
                          impl_->ocspDers, impl_->crlDers);
    };
    if (impl_->writeScope) {
        try {
            impl_->writeScope(doWrite);
        } catch (const std::exception& e) {
            // A throwing scope is the write failing; report it the same way.
            ok = false;
            emitFailed(tr("Failed to write /DSS dictionary: %1")
                           .arg(QString::fromUtf8(e.what())));
            return;
        }
    } else {
        doWrite();
    }
    if (!ok) {
        // F57 (phase-52): report WHY. The refusal that matters most here is
        // "the existing /DSS could not be snapshotted for rollback, so nothing
        // was written" — telling the user only "failed to write /DSS" makes
        // that indistinguishable from a transient hiccup they should retry.
        const std::string& why = writer.lastError();
        emitFailed(why.empty()
                       ? tr("Failed to write /DSS dictionary")
                       : tr("Failed to write /DSS dictionary: %1")
                             .arg(QString::fromStdString(why)));
        return;
    }

    qCInfo(lcCoord) << "Validation data embedded:"
                     << impl_->ocspDers.size() << "OCSP"
                     << impl_->crlDers.size() << "CRL";

    QWidget* p = impl_->dialogParent ? impl_->dialogParent.data() : nullptr;
    if (!impl_->skipDialog) {
        QMessageBox::information(p, tr("Validation"),
            tr("Validation data embedded."));
    }
    emit completed();
    cleanup();
}

void ValidationCoordinator::cleanup()
{
    // Defer fetcher destruction to the next event-loop iteration: we may
    // be inside a slot connected to a fetcher signal, and synchronous
    // destruction in that case is unsafe.
    if (impl_->fetcher) {
        ValidationFetcher* raw = impl_->fetcher.release();
        raw->deleteLater();
    }
}

} // namespace pdfe::signatures
