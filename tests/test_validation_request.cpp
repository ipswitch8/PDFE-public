// -*- coding: utf-8 -*-
// Pipeline F phase-9: user-confirmed external network fetch for LTV.
//
// New components under test:
//   - OcspUrlExtractor / CrlUrlExtractor (cert AIA + CRL DP parsing)
//   - ValidationRequestDialog (user consent)
//   - ValidationFetcher (QNetworkAccessManager async fetch)
//   - ValidationCoordinator (full flow)
//
// AC test plan (ACs 6.1-6.4):
//   (1) Cancel dialog → no network call made.
//   (2) Accept → mock returns valid OCSP → /DSS written.
//   (3) Accept → mock returns HTTP 500 → no /DSS write, error path fires.
//   (4) Cert has no AIA extension → URLs shown as "not available", Fetch
//       button disabled.
//
// Plus extractor-level tests for URL parsing (positive + scheme filtering).
//
// SECURITY: tests use a mock QNetworkAccessManager that overrides
// createRequest() to return a synthetic QNetworkReply. No real socket
// I/O happens during the test run.

#include <gtest/gtest.h>

#include "signatures/CrlUrlExtractor.h"
#include "signatures/LtvDssWriter.h"
#include "signatures/OcspUrlExtractor.h"
#include "signatures/SignatureManager.h"
#include "signatures/SignatureData.h"
#include "signatures/ValidationCoordinator.h"
#include "signatures/ValidationFetcher.h"
#include "ui/ValidationRequestDialog.h"

#include "core/Document.h"
#include "core/Error.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"

#include <QBuffer>
#include <QByteArray>
#include <QCoreApplication>
#include <QEventLoop>
#include <QListWidget>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QPushButton>
#include <QSignalSpy>
#include <QTimer>
#include <QUrl>

#include <openssl/asn1.h>
#include <openssl/bn.h>
#include <openssl/conf.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/ocsp.h>
#include <openssl/pem.h>
#include <openssl/pkcs12.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace pdfe::signatures;
using namespace pdfe::core;

namespace {

std::string fixturesDir() { return std::string(TEST_FIXTURES_DIR); }

std::filesystem::path tempWorkDir()
{
    auto d = std::filesystem::temp_directory_path() / "pdfe_validation";
    std::filesystem::create_directories(d);
    return d;
}

// Helper: spin the Qt event loop until predicate returns true or timeout.
template<typename Pred>
bool waitForCondition(Pred pred, int timeoutMs = 2000)
{
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout,
                     &loop, &QEventLoop::quit);
    QTimer pollTimer;
    QObject::connect(&pollTimer, &QTimer::timeout,
                     &loop, [&]() {
        if (pred()) loop.quit();
    });
    pollTimer.start(20);
    timer.start(timeoutMs);
    loop.exec();
    return pred();
}

// ----- OpenSSL helpers ---------------------------------------------------

// Load the test signer cert + key from the bundled PFX. Returns true on
// success. Owned X509/EVP_PKEY must be freed by caller.
bool loadTestSigner(EVP_PKEY*& outKey, X509*& outCert)
{
    outKey = nullptr;
    outCert = nullptr;

    auto pfxPath = std::filesystem::path(fixturesDir()) / "test_signer.pfx";
    if (!std::filesystem::exists(pfxPath)) return false;

    std::ifstream f(pfxPath, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string buf = ss.str();
    if (buf.empty()) return false;

    const unsigned char* p =
        reinterpret_cast<const unsigned char*>(buf.data());
    PKCS12* p12 = d2i_PKCS12(nullptr, &p, static_cast<long>(buf.size()));
    if (!p12) return false;

    STACK_OF(X509)* chain = nullptr;
    int ok = PKCS12_parse(p12, "test123", &outKey, &outCert, &chain);
    PKCS12_free(p12);
    if (chain) sk_X509_pop_free(chain, X509_free);

    if (!ok || !outKey || !outCert) {
        if (outKey) { EVP_PKEY_free(outKey); outKey = nullptr; }
        if (outCert) { X509_free(outCert); outCert = nullptr; }
        return false;
    }
    return true;
}

// Build a self-signed cert WITH AIA (OCSP) and CRL DP extensions.
// Used to test that the URL extractors find the http:// URLs.
//
// The extensions are added via X509V3_EXT_conf_nid using a string
// configuration — simplest portable approach with OpenSSL 3.x.
bool generateCertWithAia(const std::string& ocspUrl,
                          const std::string& crlUrl,
                          EVP_PKEY*& outKey,
                          X509*& outCert)
{
    outKey = nullptr;
    outCert = nullptr;

    // RSA-2048 key.
    EVP_PKEY* pkey = EVP_RSA_gen(2048);
    if (!pkey) return false;

    X509* cert = X509_new();
    if (!cert) { EVP_PKEY_free(pkey); return false; }

    X509_set_version(cert, 2); // v3
    ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
    X509_gmtime_adj(X509_getm_notBefore(cert), 0);
    X509_gmtime_adj(X509_getm_notAfter(cert), 60 * 60 * 24 * 30);
    X509_set_pubkey(cert, pkey);

    X509_NAME* name = X509_get_subject_name(cert);
    X509_NAME_add_entry_by_txt(name, "C", MBSTRING_ASC,
        reinterpret_cast<const unsigned char*>("US"), -1, -1, 0);
    X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC,
        reinterpret_cast<const unsigned char*>("PDFE Test"), -1, -1, 0);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
        reinterpret_cast<const unsigned char*>("Validation Test Cert"),
        -1, -1, 0);
    X509_set_issuer_name(cert, name);

    // AIA: id-ad-ocsp;URI=ocspUrl
    {
        std::string val = "OCSP;URI:" + ocspUrl;
        X509_EXTENSION* ext = X509V3_EXT_conf_nid(
            nullptr, nullptr, NID_info_access, val.c_str());
        if (ext) {
            X509_add_ext(cert, ext, -1);
            X509_EXTENSION_free(ext);
        } else {
            X509_free(cert); EVP_PKEY_free(pkey);
            return false;
        }
    }

    // CRL DP: URI:crlUrl
    {
        std::string val = "URI:" + crlUrl;
        X509_EXTENSION* ext = X509V3_EXT_conf_nid(
            nullptr, nullptr, NID_crl_distribution_points,
            val.c_str());
        if (ext) {
            X509_add_ext(cert, ext, -1);
            X509_EXTENSION_free(ext);
        } else {
            X509_free(cert); EVP_PKEY_free(pkey);
            return false;
        }
    }

    if (X509_sign(cert, pkey, EVP_sha256()) <= 0) {
        X509_free(cert); EVP_PKEY_free(pkey);
        return false;
    }

    outKey = pkey;
    outCert = cert;
    return true;
}

// Build a structurally valid OCSP response signed by the supplied key/cert.
// Mirrors the fixture in test_ocsp_crl_parser.cpp; duplicated locally so
// the validation test file is self-contained.
std::vector<uint8_t> buildOcspResponseDer(EVP_PKEY* key, X509* cert)
{
    std::vector<uint8_t> result;
    if (!key || !cert) return result;

    OCSP_BASICRESP* basic = OCSP_BASICRESP_new();
    if (!basic) return result;

    OCSP_CERTID* certId = OCSP_cert_to_id(EVP_sha1(), cert, cert);
    if (!certId) { OCSP_BASICRESP_free(basic); return result; }

    ASN1_TIME* thisUpd = ASN1_TIME_set(nullptr, time(nullptr));
    ASN1_TIME* nextUpd = ASN1_TIME_set(nullptr, time(nullptr) + 86400);
    if (!thisUpd || !nextUpd) {
        if (thisUpd) ASN1_TIME_free(thisUpd);
        if (nextUpd) ASN1_TIME_free(nextUpd);
        OCSP_CERTID_free(certId); OCSP_BASICRESP_free(basic);
        return result;
    }

    OCSP_SINGLERESP* single = OCSP_basic_add1_status(
        basic, certId, V_OCSP_CERTSTATUS_GOOD, 0, nullptr, thisUpd, nextUpd);
    OCSP_CERTID_free(certId);
    ASN1_TIME_free(thisUpd);
    ASN1_TIME_free(nextUpd);
    if (!single) { OCSP_BASICRESP_free(basic); return result; }

    if (OCSP_basic_sign(basic, cert, key, EVP_sha256(), nullptr, 0) != 1) {
        OCSP_BASICRESP_free(basic);
        return result;
    }

    OCSP_RESPONSE* resp = OCSP_response_create(
        OCSP_RESPONSE_STATUS_SUCCESSFUL, basic);
    OCSP_BASICRESP_free(basic);
    if (!resp) return result;

    unsigned char* der = nullptr;
    int derLen = i2d_OCSP_RESPONSE(resp, &der);
    OCSP_RESPONSE_free(resp);
    if (derLen <= 0 || !der) {
        if (der) OPENSSL_free(der);
        return result;
    }
    result.assign(der, der + derLen);
    OPENSSL_free(der);
    return result;
}

std::vector<uint8_t> buildCrlDer(EVP_PKEY* key, X509* cert)
{
    std::vector<uint8_t> result;
    if (!key || !cert) return result;

    X509_CRL* crl = X509_CRL_new();
    if (!crl) return result;
    X509_CRL_set_version(crl, 1);
    X509_CRL_set_issuer_name(crl, X509_get_subject_name(cert));

    ASN1_TIME* lastUpd = ASN1_TIME_set(nullptr, time(nullptr));
    ASN1_TIME* nextUpd = ASN1_TIME_set(nullptr, time(nullptr) + 7 * 86400);
    X509_CRL_set1_lastUpdate(crl, lastUpd);
    X509_CRL_set1_nextUpdate(crl, nextUpd);
    ASN1_TIME_free(lastUpd);
    ASN1_TIME_free(nextUpd);

    X509_CRL_sort(crl);
    if (X509_CRL_sign(crl, key, EVP_sha256()) <= 0) {
        X509_CRL_free(crl); return result;
    }

    unsigned char* der = nullptr;
    int derLen = i2d_X509_CRL(crl, &der);
    X509_CRL_free(crl);
    if (derLen <= 0 || !der) {
        if (der) OPENSSL_free(der); return result;
    }
    result.assign(der, der + derLen);
    OPENSSL_free(der);
    return result;
}

std::vector<uint8_t> certToDer(X509* cert)
{
    std::vector<uint8_t> out;
    if (!cert) return out;
    unsigned char* der = nullptr;
    int n = i2d_X509(cert, &der);
    if (n <= 0 || !der) { if (der) OPENSSL_free(der); return out; }
    out.assign(der, der + n);
    OPENSSL_free(der);
    return out;
}

// -------------------------------------------------------------------------
// Mock QNetworkReply: yields a controlled body / status / error to the
// ValidationFetcher. Lifecycle matches a real reply: setOpenMode +
// emit metaDataChanged + emit finished asynchronously.
// -------------------------------------------------------------------------
class MockNetworkReply : public QNetworkReply {
    Q_OBJECT
public:
    MockNetworkReply(QObject* parent,
                     const QNetworkRequest& req,
                     QNetworkAccessManager::Operation op,
                     int httpStatus,
                     const QByteArray& body,
                     QNetworkReply::NetworkError err = QNetworkReply::NoError,
                     qint64 fakeContentLength = -1,
                     bool stall = false)
        : QNetworkReply(parent), body_(body),
          httpStatus_(httpStatus), err_(err),
          fakeContentLength_(fakeContentLength)
    {
        setRequest(req);
        setUrl(req.url());
        setOperation(op);
        setOpenMode(QIODevice::ReadOnly);

        // A STALLED reply is the whole point of the lifetime controls below
        // (phase-53 review, finding 5): a real OCSP/CRL responder that takes
        // minutes, or never answers, is the ordinary condition under which the
        // user closes or replaces the document while the fetch is in flight.
        // Nothing is scheduled here; deliver() is called by the test when it
        // has arranged the state it wants the completion to land in.
        if (!stall) {
            QTimer::singleShot(0, this, [this]() { deliver(); });
        }
    }

    /// Complete the reply now. Idempotent — a second call does nothing, so a
    /// test that releases a stall cannot double-finish a reply that already
    /// completed on its own.
    void deliver()
    {
        if (delivered_) return;
        delivered_ = true;

        // Optional: lie about Content-Length to drive the
        // pre-parser size cap in ValidationFetcher.
        if (fakeContentLength_ >= 0) {
            this->setHeader(QNetworkRequest::ContentLengthHeader,
                             QVariant::fromValue<qlonglong>(
                                 static_cast<qlonglong>(fakeContentLength_)));
            emit this->metaDataChanged();
        }
        this->setAttribute(QNetworkRequest::HttpStatusCodeAttribute,
                            httpStatus_);
        // Match real-Qt behaviour: HTTP 4xx/5xx surface as
        // NetworkError so the consumer can treat them uniformly.
        QNetworkReply::NetworkError useErr = err_;
        if (err_ == QNetworkReply::NoError && httpStatus_ >= 400) {
            useErr = (httpStatus_ >= 500)
                ? QNetworkReply::InternalServerError
                : QNetworkReply::ProtocolInvalidOperationError;
        }
        if (useErr != QNetworkReply::NoError) {
            this->setError(useErr, QStringLiteral("mock HTTP error"));
            emit this->errorOccurred(useErr);
        }
        emit this->readyRead();
        this->setFinished(true);
        emit this->finished();
    }

    void abort() override {
        setError(QNetworkReply::OperationCanceledError,
                 QStringLiteral("aborted"));
        setFinished(true);
        emit finished();
    }
    qint64 readData(char* data, qint64 maxSize) override {
        qint64 n = qMin<qint64>(maxSize, body_.size() - readPos_);
        if (n <= 0) return -1;
        std::memcpy(data, body_.constData() + readPos_,
                    static_cast<std::size_t>(n));
        readPos_ += n;
        return n;
    }
    qint64 bytesAvailable() const override {
        return body_.size() - readPos_ + QNetworkReply::bytesAvailable();
    }
    bool isSequential() const override { return true; }

private:
    QByteArray body_;
    qint64 readPos_ = 0;
    int httpStatus_ = 200;
    QNetworkReply::NetworkError err_ = QNetworkReply::NoError;
    qint64 fakeContentLength_ = -1;
    bool delivered_ = false;
};

// -------------------------------------------------------------------------
// Mock QNetworkAccessManager: counts createRequest() calls, returns a
// MockNetworkReply with the configured status + body.
// -------------------------------------------------------------------------
class MockNetworkAccessManager : public QNetworkAccessManager {
public:
    explicit MockNetworkAccessManager(QObject* parent = nullptr)
        : QNetworkAccessManager(parent) {}

    int createCount() const { return createCount_; }
    void setOcspBody(const QByteArray& b) { ocspBody_ = b; }
    void setCrlBody(const QByteArray& b)  { crlBody_  = b; }
    void setHttpStatus(int s) { httpStatus_ = s; }
    void setErrorCode(QNetworkReply::NetworkError e) { err_ = e; }
    void setFakeContentLength(qint64 n) { fakeContentLength_ = n; }

    /// Make every subsequent reply STALL: created, dispatched, but never
    /// completing until releaseStalledReplies() is called. This models the slow
    /// or dead OCSP/CRL responder that opens the window finding 5 is about.
    void setStallReplies(bool stall) { stall_ = stall; }

    /// Complete every stalled reply now, in creation order.
    void releaseStalledReplies()
    {
        for (auto& r : stalled_) {
            if (r) r->deliver();
        }
        stalled_.clear();
    }

    int stalledCount() const { return static_cast<int>(stalled_.size()); }

protected:
    QNetworkReply* createRequest(Operation op,
                                  const QNetworkRequest& req,
                                  QIODevice* outgoingData) override
    {
        Q_UNUSED(outgoingData);
        ++createCount_;

        bool isOcsp = false;
        QByteArray ct = req.rawHeader("Content-Type");
        if (op == PostOperation && ct == "application/ocsp-request") {
            isOcsp = true;
        }
        QByteArray body = isOcsp ? ocspBody_ : crlBody_;

        auto* reply = new MockNetworkReply(this, req, op,
                                            httpStatus_, body, err_,
                                            fakeContentLength_, stall_);
        if (stall_) stalled_.push_back(QPointer<MockNetworkReply>(reply));
        return reply;
    }

private:
    bool stall_ = false;
    std::vector<QPointer<MockNetworkReply>> stalled_;
    int createCount_ = 0;
    QByteArray ocspBody_;
    QByteArray crlBody_;
    int httpStatus_ = 200;
    QNetworkReply::NetworkError err_ = QNetworkReply::NoError;
    qint64 fakeContentLength_ = -1;
};

// -------------------------------------------------------------------------
// Sign a fresh PDF in @p outPath so LtvDssWriter::extractFirstSignatureInputs
// has a real /Contents to walk. Mirrors the helper in test_ltv_dss_writer.cpp.
// -------------------------------------------------------------------------
bool buildSignedFixture(std::shared_ptr<MuPdfContext> ctx,
                        const std::filesystem::path& outPath)
{
    auto fixtures = std::filesystem::path(fixturesDir());
    auto pfx = fixtures / "test_signer.pfx";
    auto srcPdf = fixtures / "test_single.pdf";
    if (!std::filesystem::exists(pfx) || !std::filesystem::exists(srcPdf))
        return false;

    SignatureManager mgr(ctx);
    {
        auto doc = Document::open(ctx, srcPdf);
        if (!doc) return false;
        auto page = doc->page(0);
        mgr.createSignatureField(page.get(), "ValSig",
                                  QRectF(100, 500, 200, 50));
        std::filesystem::remove(outPath);
        doc->saveAs(outPath);
    }
    {
        auto doc = Document::open(ctx, outPath);
        if (!doc) return false;
        auto page = doc->page(0);
        SigningOptions opts;
        opts.pfxPath = pfx.string();
        opts.pfxPassword = "test123";
        opts.reason = "Validation flow test";
        opts.location = "PDFE test suite";
        mgr.signField(doc.get(), page.get(), 0, opts);
    }
    return true;
}

} // anonymous namespace

// =========================================================================
// Extractor unit tests
// =========================================================================

class ValidationExtractorTest : public ::testing::Test {};

TEST_F(ValidationExtractorTest, ExtractsHttpOcspUrl)
{
    EVP_PKEY* key = nullptr; X509* cert = nullptr;
    ASSERT_TRUE(generateCertWithAia(
        "http://ocsp.example.com/test",
        "http://crl.example.com/test.crl",
        key, cert));

    std::vector<std::string> ocsp;
    EXPECT_TRUE(OcspUrlExtractor::parse(cert, ocsp));
    ASSERT_EQ(ocsp.size(), 1u);
    EXPECT_EQ(ocsp[0], "http://ocsp.example.com/test");

    std::vector<std::string> crl;
    EXPECT_TRUE(CrlUrlExtractor::parse(cert, crl));
    ASSERT_EQ(crl.size(), 1u);
    EXPECT_EQ(crl[0], "http://crl.example.com/test.crl");

    X509_free(cert); EVP_PKEY_free(key);
}

TEST_F(ValidationExtractorTest, ReturnsFalseWhenNoAiaExtension)
{
    EVP_PKEY* key = nullptr; X509* cert = nullptr;
    ASSERT_TRUE(loadTestSigner(key, cert));

    std::vector<std::string> ocsp;
    // The bundled test_signer.pfx has no AIA extension.
    EXPECT_FALSE(OcspUrlExtractor::parse(cert, ocsp));
    EXPECT_TRUE(ocsp.empty());

    std::vector<std::string> crl;
    EXPECT_FALSE(CrlUrlExtractor::parse(cert, crl));
    EXPECT_TRUE(crl.empty());

    X509_free(cert); EVP_PKEY_free(key);
}

TEST_F(ValidationExtractorTest, FiltersNonHttpSchemes)
{
    EVP_PKEY* key = nullptr; X509* cert = nullptr;
    ASSERT_TRUE(generateCertWithAia(
        "ldap://ldap.example.com/cn=ocsp",  // NOT allowed
        "ldap://ldap.example.com/cn=crl",   // NOT allowed
        key, cert));

    std::vector<std::string> ocsp;
    // parse() returns true (extension is present), but no http/https URLs
    // survive the scheme filter.
    EXPECT_TRUE(OcspUrlExtractor::parse(cert, ocsp));
    EXPECT_TRUE(ocsp.empty())
        << "ldap:// should be filtered to prevent network egress to "
           "non-HTTP services";

    std::vector<std::string> crl;
    EXPECT_TRUE(CrlUrlExtractor::parse(cert, crl));
    EXPECT_TRUE(crl.empty());

    X509_free(cert); EVP_PKEY_free(key);
}

// =========================================================================
// Dialog tests
// =========================================================================

class ValidationDialogTest : public ::testing::Test {};

TEST_F(ValidationDialogTest, DisablesFetchWhenNoUrls)
{
    pdfe::ui::ValidationRequestDialog dlg(QStringList{}, QStringList{});
    EXPECT_TRUE(dlg.isUrlListEmpty());

    auto* fetchBtn = dlg.findChild<QPushButton*>("fetchButton");
    auto* cancelBtn = dlg.findChild<QPushButton*>("cancelButton");
    auto* list = dlg.findChild<QListWidget*>("validationRequestUrlList");

    ASSERT_NE(fetchBtn, nullptr);
    ASSERT_NE(cancelBtn, nullptr);
    ASSERT_NE(list, nullptr);

    EXPECT_FALSE(fetchBtn->isEnabled())
        << "AC 6.4: Fetch button must be disabled when no URLs available";
    EXPECT_TRUE(cancelBtn->isEnabled());
    EXPECT_TRUE(cancelBtn->isDefault())
        << "Cancel must be the default button so Enter dismisses safely";
    EXPECT_EQ(dlg.objectName(), QString("validationRequestDialog"));
}

TEST_F(ValidationDialogTest, ShowsAllUrlsWithPrefixes)
{
    QStringList ocsp{"http://ocsp.example.com/"};
    QStringList crl{"http://crl.example.com/c.crl"};
    pdfe::ui::ValidationRequestDialog dlg(ocsp, crl);

    EXPECT_FALSE(dlg.isUrlListEmpty());

    auto* fetchBtn = dlg.findChild<QPushButton*>("fetchButton");
    ASSERT_NE(fetchBtn, nullptr);
    EXPECT_TRUE(fetchBtn->isEnabled());

    auto* list = dlg.findChild<QListWidget*>("validationRequestUrlList");
    ASSERT_NE(list, nullptr);
    ASSERT_EQ(list->count(), 2);
    EXPECT_TRUE(list->item(0)->text().startsWith("[OCSP] "));
    EXPECT_TRUE(list->item(1)->text().startsWith("[CRL] "));
}

// =========================================================================
// Coordinator integration tests (the four AC tests)
// =========================================================================

class ValidationRequestTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        // Generate cert+key with AIA so the coordinator's URL extractors
        // will find URLs to surface in the dialog.
        if (!generateCertWithAia(
                "http://ocsp.example.com/v",
                "http://crl.example.com/v.crl",
                aiaKey_, aiaCert_)) {
            return;
        }

        // Synthesize an OCSP response signed by the AIA cert.
        ocspBody_ = buildOcspResponseDer(aiaKey_, aiaCert_);
        crlBody_ = buildCrlDer(aiaKey_, aiaCert_);

        // Build a real signed PDF whose first signature can be inspected.
        // (We won't actually substitute the AIA cert into it — for the
        // mock-network tests we only need *some* verified signature for
        // the coordinator to run, and an override OCSP/CRL DER for
        // LtvDssWriter to embed.)
        sigCtx_ = std::make_shared<MuPdfContext>();
        signedFixture_ = tempWorkDir() / "validation_signed.pdf";
        try {
            if (!buildSignedFixture(sigCtx_, signedFixture_)) {
                signedFixture_.clear();
            }
        } catch (...) {
            signedFixture_.clear();
        }
    }

    static void TearDownTestSuite() {
        if (aiaCert_) { X509_free(aiaCert_); aiaCert_ = nullptr; }
        if (aiaKey_) { EVP_PKEY_free(aiaKey_); aiaKey_ = nullptr; }
        std::error_code ec;
        if (!signedFixture_.empty())
            std::filesystem::remove(signedFixture_, ec);
        sigCtx_.reset();
    }

    static EVP_PKEY* aiaKey_;
    static X509* aiaCert_;
    static std::vector<uint8_t> ocspBody_;
    static std::vector<uint8_t> crlBody_;
    static std::filesystem::path signedFixture_;
    static std::shared_ptr<MuPdfContext> sigCtx_;
};
EVP_PKEY* ValidationRequestTest::aiaKey_ = nullptr;
X509* ValidationRequestTest::aiaCert_ = nullptr;
std::vector<uint8_t> ValidationRequestTest::ocspBody_;
std::vector<uint8_t> ValidationRequestTest::crlBody_;
std::filesystem::path ValidationRequestTest::signedFixture_;
std::shared_ptr<MuPdfContext> ValidationRequestTest::sigCtx_;

// AC 6.6.1: Cancel dialog → no network call made.
TEST_F(ValidationRequestTest, CancelDialogPreventsFetch)
{
    if (signedFixture_.empty())
        GTEST_SKIP() << "Signed fixture unavailable";

    MockNetworkAccessManager mockNam;

    ValidationCoordinator coord(&mockNam, nullptr);
    coord.setSkipDialogForTests(true);
    coord.setTestDialogResult(false); // user clicks Cancel

    QSignalSpy cancelSpy(&coord, &ValidationCoordinator::cancelled);
    QSignalSpy completedSpy(&coord, &ValidationCoordinator::completed);

    auto doc = Document::open(sigCtx_, signedFixture_);
    ASSERT_NE(doc, nullptr);

    // Phase-16 fix (qt_test_gap_audit finding #31): entry-point sanity
    // check, not a cancel-semantics check. The real cancel contract is
    // locked by cancelSpy.count() == 1 + mockNam.createCount() == 0
    // below.
    bool requestInitiated = coord.request(nullptr, doc.get());
    EXPECT_TRUE(requestInitiated)
        << "request() entry-point must return true regardless of "
           "user accept/cancel (cancel-semantics asserted via "
           "cancelSpy + mockNam.createCount below).";

    // Drain pending events so the cancellation signal is delivered.
    QCoreApplication::processEvents();

    EXPECT_EQ(cancelSpy.count(), 1)
        << "Cancel must fire cancelled() exactly once";
    EXPECT_EQ(completedSpy.count(), 0);
    EXPECT_EQ(mockNam.createCount(), 0)
        << "AC 6.6.1: cancelling MUST NOT issue any network request";
}

// AC 6.6.2: Accept → mock returns valid OCSP → /DSS written.
TEST_F(ValidationRequestTest, AcceptValidOcspWritesDss)
{
    if (signedFixture_.empty() || ocspBody_.empty() || crlBody_.empty())
        GTEST_SKIP() << "OCSP/CRL fixtures unavailable";

    // Copy fixture so the LtvDssWriter incremental save targets a temp file.
    auto workCopy = tempWorkDir() / "validation_accept_ok.pdf";
    std::filesystem::remove(workCopy);
    std::filesystem::copy_file(signedFixture_, workCopy);

    auto doc = Document::open(sigCtx_, workCopy);
    ASSERT_NE(doc, nullptr);

    // Pre-condition: NO /DSS yet.
    DssCounts before;
    LtvDssWriter::inspectDss(doc.get(), "", before);
    EXPECT_FALSE(before.hasDss);

    MockNetworkAccessManager mockNam;
    QByteArray ocspBytes(reinterpret_cast<const char*>(ocspBody_.data()),
                          static_cast<int>(ocspBody_.size()));
    QByteArray crlBytes(reinterpret_cast<const char*>(crlBody_.data()),
                         static_cast<int>(crlBody_.size()));
    mockNam.setOcspBody(ocspBytes);
    mockNam.setCrlBody(crlBytes);

    ValidationCoordinator coord(&mockNam, nullptr);
    coord.setSkipDialogForTests(true);
    coord.setTestDialogResult(true);
    // The bundled test_signer.pfx has no AIA/CRL DP. Inject test URLs
    // so the coordinator's full mock-network path is exercised end to
    // end. Override the parsed DER bytes so LtvDssWriter sees the
    // pre-built fixture (mockNam returns the same bytes anyway).
    coord.setTestUrls(QStringList{"http://ocsp.example.com/x"},
                       QStringList{"http://crl.example.com/x.crl"});
    coord.setOverrideOcspDer(ocspBytes);
    coord.setOverrideCrlDer(crlBytes);

    QSignalSpy completedSpy(&coord, &ValidationCoordinator::completed);
    QSignalSpy failedSpy(&coord, &ValidationCoordinator::failed);

    bool started = coord.request(nullptr, doc.get());
    EXPECT_TRUE(started);

    // Drive the Qt event loop to deliver the mock's queued finished signals.
    waitForCondition([&]() {
        return completedSpy.count() > 0 || failedSpy.count() > 0;
    }, 5000);

    EXPECT_EQ(failedSpy.count(), 0)
        << "Coordinator should not have failed; reason: "
        << (failedSpy.count() > 0
              ? failedSpy.takeFirst().at(0).toString().toStdString()
              : std::string("(no failure)"));
    EXPECT_GE(completedSpy.count(), 1)
        << "AC 6.6.2: valid OCSP must produce a completed() signal";

    EXPECT_GE(mockNam.createCount(), 2)
        << "Mock NAM should have seen 1 OCSP POST + 1 CRL GET (>=2)";

    // Drop doc handle so incremental save's flushed file is closed.
    doc.reset();

    // Re-open and verify /DSS exists.
    auto doc2 = Document::open(sigCtx_, workCopy);
    ASSERT_NE(doc2, nullptr);
    DssCounts after;
    ASSERT_TRUE(LtvDssWriter::inspectDss(doc2.get(), "", after));
    EXPECT_TRUE(after.hasDss)
        << "AC 6.6.2: /DSS dictionary must be present after success";
    EXPECT_GT(after.topCerts, 0);
    EXPECT_GE(after.topOcsps, 1)
        << "AC 6.6.2: /OCSPs must contain the fetched response";
    EXPECT_GE(after.topCrls, 1);

    doc2.reset();
    std::error_code ec;
    std::filesystem::remove(workCopy, ec);
}

// AC 6.6.3: Accept → mock returns HTTP 500 → no /DSS write,
// failed() fires.
TEST_F(ValidationRequestTest, AcceptHttpErrorShowsErrorDialog)
{
    if (signedFixture_.empty())
        GTEST_SKIP() << "Signed fixture unavailable";

    auto workCopy = tempWorkDir() / "validation_http_err.pdf";
    std::filesystem::remove(workCopy);
    std::filesystem::copy_file(signedFixture_, workCopy);

    auto doc = Document::open(sigCtx_, workCopy);
    ASSERT_NE(doc, nullptr);

    // Pre-condition: no /DSS.
    DssCounts before;
    LtvDssWriter::inspectDss(doc.get(), "", before);
    EXPECT_FALSE(before.hasDss);

    MockNetworkAccessManager mockNam;
    mockNam.setHttpStatus(500);
    mockNam.setOcspBody(QByteArray("server-error"));
    mockNam.setCrlBody(QByteArray("server-error"));

    ValidationCoordinator coord(&mockNam, nullptr);
    coord.setSkipDialogForTests(true);  // also skips QMessageBox::critical
    coord.setTestDialogResult(true);
    coord.setTestUrls(QStringList{"http://ocsp.example.com/p"},
                       QStringList{"http://crl.example.com/p.crl"});

    QSignalSpy completedSpy(&coord, &ValidationCoordinator::completed);
    QSignalSpy failedSpy(&coord, &ValidationCoordinator::failed);

    bool started = coord.request(nullptr, doc.get());
    EXPECT_TRUE(started);

    waitForCondition([&]() {
        return completedSpy.count() > 0 || failedSpy.count() > 0;
    }, 5000);

    EXPECT_EQ(completedSpy.count(), 0)
        << "AC 6.6.3: HTTP 500 must NOT result in DSS write";
    EXPECT_GE(failedSpy.count(), 1)
        << "AC 6.6.3: HTTP error must emit failed()";

    if (failedSpy.count() > 0) {
        QString reason = failedSpy.takeFirst().at(0).toString();
        // Phase-16 fix (qt_test_gap_audit finding #32): tighten the
        // assertion to require the HTTP-500 status code specifically.
        // The previous OR-fallback (contains("HTTP")) accepted any
        // HTTP-related message, including success responses.
        EXPECT_TRUE(reason.contains("500"))
            << "Failure reason should reference HTTP status 500: "
            << reason.toStdString();
    }

    EXPECT_GE(mockNam.createCount(), 1);

    // Re-open and confirm: NO /DSS was written on the failure path.
    doc.reset();
    auto doc2 = Document::open(sigCtx_, workCopy);
    ASSERT_NE(doc2, nullptr);
    DssCounts after;
    LtvDssWriter::inspectDss(doc2.get(), "", after);
    EXPECT_FALSE(after.hasDss)
        << "AC 6.6.3: /DSS must NOT be present after HTTP failure";

    doc2.reset();
    std::error_code ec;
    std::filesystem::remove(workCopy, ec);
}

// Security: oversize Content-Length must abort BEFORE the body is buffered.
// (Mirror against the security-audit finding in phase 9 review: parsers cap
// at 16 MiB OCSP / 64 MiB CRL but the fetcher itself must reject before
// readAll() balloons the heap.)
TEST_F(ValidationRequestTest, OversizeContentLengthAborts)
{
    if (signedFixture_.empty())
        GTEST_SKIP() << "Signed fixture unavailable";

    auto workCopy = tempWorkDir() / "validation_oversize.pdf";
    std::filesystem::remove(workCopy);
    std::filesystem::copy_file(signedFixture_, workCopy);
    auto doc = Document::open(sigCtx_, workCopy);
    ASSERT_NE(doc, nullptr);

    MockNetworkAccessManager mockNam;
    // Lie: announce 32 MiB even though body is empty. The fetcher's
    // metaDataChanged hook must abort on Content-Length > 16 MiB cap.
    mockNam.setFakeContentLength(32LL * 1024 * 1024);
    mockNam.setOcspBody(QByteArray("ignored"));
    mockNam.setCrlBody(QByteArray("ignored"));

    ValidationCoordinator coord(&mockNam, nullptr);
    coord.setSkipDialogForTests(true);
    coord.setTestDialogResult(true);
    coord.setTestUrls(QStringList{"http://ocsp.example.com/p"},
                       QStringList{"http://crl.example.com/p.crl"});

    QSignalSpy completedSpy(&coord, &ValidationCoordinator::completed);
    QSignalSpy failedSpy(&coord, &ValidationCoordinator::failed);

    EXPECT_TRUE(coord.request(nullptr, doc.get()));

    waitForCondition([&]() {
        return completedSpy.count() > 0 || failedSpy.count() > 0;
    }, 5000);

    EXPECT_EQ(completedSpy.count(), 0)
        << "Oversize Content-Length must NOT result in DSS write";
    EXPECT_GE(failedSpy.count(), 1)
        << "Oversize Content-Length must emit failed()";
    if (failedSpy.count() > 0) {
        QString reason = failedSpy.takeFirst().at(0).toString();
        EXPECT_TRUE(reason.contains("cap") || reason.contains("byte"))
            << "Failure reason should mention size cap: "
            << reason.toStdString();
    }

    doc.reset();
    std::error_code ec;
    std::filesystem::remove(workCopy, ec);
}

// AC 6.6.4: Cert has no AIA extension → URLs shown as "not available",
// fetch button disabled.
TEST_F(ValidationRequestTest, NoAiaExtensionDisablesFetch)
{
    EVP_PKEY* key = nullptr; X509* cert = nullptr;
    ASSERT_TRUE(loadTestSigner(key, cert));

    std::vector<std::string> ocspUrls;
    std::vector<std::string> crlUrls;
    bool aiaPresent = OcspUrlExtractor::parse(cert, ocspUrls);
    bool crlDpPresent = CrlUrlExtractor::parse(cert, crlUrls);

    X509_free(cert); EVP_PKEY_free(key);

    EXPECT_FALSE(aiaPresent);
    EXPECT_FALSE(crlDpPresent);
    EXPECT_TRUE(ocspUrls.empty());
    EXPECT_TRUE(crlUrls.empty());

    // Simulate the dialog construction the coordinator would do.
    QStringList ocspQ;
    QStringList crlQ;
    pdfe::ui::ValidationRequestDialog dlg(ocspQ, crlQ);
    EXPECT_TRUE(dlg.isUrlListEmpty());

    auto* fetchBtn = dlg.findChild<QPushButton*>("fetchButton");
    ASSERT_NE(fetchBtn, nullptr);
    EXPECT_FALSE(fetchBtn->isEnabled())
        << "AC 6.6.4: Fetch button must be disabled when cert has no "
           "AIA extension";
}

// ===========================================================================
// FINDING 5 (phase-53 adversarial review) — THE DOCUMENT CAN DIE, OR BE
// REPLACED, WHILE THE FETCH IS IN FLIGHT.
// ===========================================================================
//
// WHAT WAS WRONG. ValidationCoordinator::request() stores a raw
// pdfe::core::Document* and returns; the OCSP/CRL fetches are asynchronous and
// deliberately do not block the UI thread. tryWriteDss() therefore runs an
// UNBOUNDED time later. MainWindow disables neither File>Close nor File>Open
// while a fetch is in flight, and MainWindow::closeDocument() does
// `document_.reset()`. So:
//
//   request validation -> responder stalls -> user closes the document
//   -> fetch completes -> tryWriteDss() -> writeScope -> *document_
//
// dereferenced a destroyed object, through a null unique_ptr. Replacing the
// document instead of closing it was worse than a crash: the write would have
// succeeded, filing one document's revocation material into another's /DSS and
// reporting success.
//
// WHY THESE ARE NOT DECORATION. The gate under test lives in
// ValidationCoordinator::tryWriteDss() and is asserted through the
// coordinator's own observable output — the failed() reason string, which
// carries kDocumentNoLongerOpenMessage / kDocumentReplacedMessage — plus the
// bytes on disk. Revert the gate and:
//   * ClosedDocument...        : the reason is no longer the "closed" message
//                                (the write scope's own fail-closed throw wins)
//                                and writeScopeEntered flips true;
//   * ReplacedDocument...      : completed() fires instead of failed(), and the
//                                FIRST document gains a /DSS it was never
//                                supposed to receive in this scenario.
//
// AND THE DISCRIMINATING NEGATIVE. A gate that refuses everything is not a
// gate, so AcceptsTheSameDocumentReopenedAtANewAddress proves the check is on
// IDENTITY and not on "did anything change": a Document re-opened on the same
// file, carrying the same signature, at a DIFFERENT heap address, is accepted
// and the /DSS is written. That is also why the production check is on the file
// path and the signature /Contents rather than on the pointer — pointer
// equality is wrong in both directions, and this test is the half that would go
// red if someone "simplified" it to a pointer comparison.
//
// The test-side write scope models MainWindow's: it drops out if the owner has
// no document. It is there so that a revert-and-observe run fails an ASSERTION
// instead of taking the whole suite down with a use-after-free — the
// discrimination itself comes from the coordinator's reason string.

namespace {

/// Copy the signed fixture to `name` under the temp work dir and return it.
std::filesystem::path signedWorkCopy(const std::filesystem::path& fixture,
                                     const char* name)
{
    auto p = tempWorkDir() / name;
    std::error_code ec;
    std::filesystem::remove(p, ec);
    // overwrite_existing, not a bare copy_file: a previous run that was killed
    // (which is exactly what a revert-and-observe run of these controls does,
    // since the un-guarded code corrupts the heap) can leave the target behind
    // and briefly locked, and a "file exists" throw here would mask the real
    // result of the NEXT run.
    std::filesystem::copy_file(fixture, p,
                               std::filesystem::copy_options::overwrite_existing);
    return p;
}

std::string readAllBytesOf(const std::filesystem::path& p)
{
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

TEST_F(ValidationRequestTest, ClosedDocumentMidFetchAbandonsTheDssWrite)
{
    if (signedFixture_.empty() || ocspBody_.empty() || crlBody_.empty())
        GTEST_SKIP() << "OCSP/CRL fixtures unavailable";

    const auto work = signedWorkCopy(signedFixture_, "validation_closed_mid.pdf");
    const std::string before = readAllBytesOf(work);
    ASSERT_FALSE(before.empty());

    // The owner's storage, standing in for MainWindow::document_.
    std::unique_ptr<Document> owner = Document::open(sigCtx_, work);
    ASSERT_NE(owner, nullptr);

    MockNetworkAccessManager mockNam;
    QByteArray ocspBytes(reinterpret_cast<const char*>(ocspBody_.data()),
                          static_cast<int>(ocspBody_.size()));
    QByteArray crlBytes(reinterpret_cast<const char*>(crlBody_.data()),
                         static_cast<int>(crlBody_.size()));
    mockNam.setOcspBody(ocspBytes);
    mockNam.setCrlBody(crlBytes);
    mockNam.setStallReplies(true);   // the responder never answers, yet

    ValidationCoordinator coord(&mockNam, nullptr);
    coord.setSkipDialogForTests(true);
    coord.setTestDialogResult(true);
    coord.setTestUrls(QStringList{"http://ocsp.example.com/stall"},
                       QStringList{"http://crl.example.com/stall.crl"});
    coord.setOverrideOcspDer(ocspBytes);
    coord.setOverrideCrlDer(crlBytes);

    coord.setLiveDocumentResolver([&owner]() -> Document* {
        return owner.get();
    });

    bool writeScopeEntered = false;
    coord.setWriteScope([&](const std::function<void()>& write) {
        writeScopeEntered = true;
        if (!owner) {
            // MainWindow's fail-closed backstop, modelled. See the block above
            // for why this is here and why it is NOT what is under test.
            throw pdfe::core::PdfeException("owner has no document");
        }
        write();
    });

    QSignalSpy completedSpy(&coord, &ValidationCoordinator::completed);
    QSignalSpy failedSpy(&coord, &ValidationCoordinator::failed);

    ASSERT_TRUE(coord.request(nullptr, owner.get()));
    QCoreApplication::processEvents();
    ASSERT_GE(mockNam.createCount(), 2)
        << "the fetches must actually have been issued, or this test proves "
           "nothing about what happens when they complete";
    ASSERT_GT(mockNam.stalledCount(), 0)
        << "the replies must be STALLED — a reply that already completed "
           "cannot model a close that races the fetch";
    ASSERT_EQ(completedSpy.count(), 0);
    ASSERT_EQ(failedSpy.count(), 0);

    // ── The user closes the document while the fetch is outstanding. ──
    owner.reset();

    // ── The responder finally answers. ──
    mockNam.releaseStalledReplies();
    waitForCondition([&]() {
        return completedSpy.count() > 0 || failedSpy.count() > 0;
    }, 5000);

    EXPECT_EQ(completedSpy.count(), 0)
        << "a /DSS write against a closed document must never be reported as "
           "completed";
    ASSERT_EQ(failedSpy.count(), 1)
        << "the coordinator must report the abandoned write";

    const QString reason = failedSpy.takeFirst().at(0).toString();
    EXPECT_TRUE(reason.contains(
                    QString::fromUtf8(
                        ValidationCoordinator::kDocumentNoLongerOpenMessage)))
        << "the failure must name the LIFETIME cause, not a generic write "
           "error — this is the assertion that reverts red. Got: "
        << reason.toStdString();

    EXPECT_FALSE(writeScopeEntered)
        << "nothing downstream of the gate may run once the document is gone: "
           "the write scope dereferences *document_ and *undoManager_";

    EXPECT_EQ(readAllBytesOf(work), before)
        << "the file on disk must be byte-identical — no /DSS revision may be "
           "appended on behalf of a document that is no longer open";

    std::error_code ec;
    std::filesystem::remove(work, ec);
}

TEST_F(ValidationRequestTest, ReplacedDocumentMidFetchAbandonsTheDssWrite)
{
    if (signedFixture_.empty() || ocspBody_.empty() || crlBody_.empty())
        GTEST_SKIP() << "OCSP/CRL fixtures unavailable";

    const auto workA = signedWorkCopy(signedFixture_, "validation_replaced_a.pdf");
    const auto workB = signedWorkCopy(signedFixture_, "validation_replaced_b.pdf");
    const std::string beforeA = readAllBytesOf(workA);
    const std::string beforeB = readAllBytesOf(workB);
    ASSERT_FALSE(beforeA.empty());
    ASSERT_FALSE(beforeB.empty());

    auto docA = Document::open(sigCtx_, workA);
    ASSERT_NE(docA, nullptr);
    // docA is deliberately kept ALIVE. That is what makes the wrong-document
    // outcome observable rather than undefined: it is the same state the
    // allocator produces when a new Document lands on a destroyed one's
    // address, made deterministic.
    std::unique_ptr<Document> ownerCurrent;   // what the owner has open NOW

    MockNetworkAccessManager mockNam;
    QByteArray ocspBytes(reinterpret_cast<const char*>(ocspBody_.data()),
                          static_cast<int>(ocspBody_.size()));
    QByteArray crlBytes(reinterpret_cast<const char*>(crlBody_.data()),
                         static_cast<int>(crlBody_.size()));
    mockNam.setOcspBody(ocspBytes);
    mockNam.setCrlBody(crlBytes);
    mockNam.setStallReplies(true);

    ValidationCoordinator coord(&mockNam, nullptr);
    coord.setSkipDialogForTests(true);
    coord.setTestDialogResult(true);
    coord.setTestUrls(QStringList{"http://ocsp.example.com/stall"},
                       QStringList{"http://crl.example.com/stall.crl"});
    coord.setOverrideOcspDer(ocspBytes);
    coord.setOverrideCrlDer(crlBytes);
    coord.setLiveDocumentResolver([&ownerCurrent]() -> Document* {
        return ownerCurrent.get();
    });

    bool writeScopeEntered = false;
    coord.setWriteScope([&](const std::function<void()>& write) {
        writeScopeEntered = true;
        if (!ownerCurrent) throw pdfe::core::PdfeException("no document");
        write();
    });

    QSignalSpy completedSpy(&coord, &ValidationCoordinator::completed);
    QSignalSpy failedSpy(&coord, &ValidationCoordinator::failed);

    ownerCurrent = std::move(docA);
    Document* const requestedAgainst = ownerCurrent.get();
    ASSERT_TRUE(coord.request(nullptr, requestedAgainst));
    QCoreApplication::processEvents();
    ASSERT_GT(mockNam.stalledCount(), 0);

    // ── The user closes A and opens B while the fetch is outstanding. ──
    ownerCurrent.reset();
    ownerCurrent = Document::open(sigCtx_, workB);
    ASSERT_NE(ownerCurrent, nullptr);

    mockNam.releaseStalledReplies();
    waitForCondition([&]() {
        return completedSpy.count() > 0 || failedSpy.count() > 0;
    }, 5000);

    EXPECT_EQ(completedSpy.count(), 0)
        << "material fetched for one document must never be reported as "
           "embedded when a different document is open";
    ASSERT_EQ(failedSpy.count(), 1);

    const QString reason = failedSpy.takeFirst().at(0).toString();
    EXPECT_TRUE(reason.contains(
                    QString::fromUtf8(
                        ValidationCoordinator::kDocumentReplacedMessage)))
        << "the failure must name the IDENTITY cause. Got: "
        << reason.toStdString();

    EXPECT_FALSE(writeScopeEntered);

    ownerCurrent.reset();
    EXPECT_EQ(readAllBytesOf(workB), beforeB)
        << "the NEWLY OPENED document must not receive the previous "
           "document's revocation material";
    EXPECT_EQ(readAllBytesOf(workA), beforeA)
        << "and the original target must not be written either — it is no "
           "longer the document the owner has open, and writing to a file "
           "behind the user's back is the outcome the gate exists to prevent";

    std::error_code ec;
    std::filesystem::remove(workA, ec);
    std::filesystem::remove(workB, ec);
}

// The SECOND arm of the identity check, on its own.
//
// ReplacedDocument... above is refused on the FILE PATH, so it cannot tell you
// anything about the /Contents comparison behind it — and an untested second
// line is decoration. Here the path matches EXACTLY (it is literally the same
// path, re-opened) and only the signature differs, so nothing but the
// /Contents arm can produce the refusal. This is also the shape that address
// reuse produces in the field: same owner, same path, a document that is not
// the one the material was fetched for.
TEST_F(ValidationRequestTest, SamePathDifferentSignatureIsRefused)
{
    if (signedFixture_.empty() || ocspBody_.empty() || crlBody_.empty())
        GTEST_SKIP() << "OCSP/CRL fixtures unavailable";
    const auto unsignedSrc = std::filesystem::path(fixturesDir()) / "test_single.pdf";
    if (!std::filesystem::exists(unsignedSrc))
        GTEST_SKIP() << "test_single.pdf unavailable";

    const auto work = signedWorkCopy(signedFixture_, "validation_samepath.pdf");

    MockNetworkAccessManager mockNam;
    QByteArray ocspBytes(reinterpret_cast<const char*>(ocspBody_.data()),
                          static_cast<int>(ocspBody_.size()));
    QByteArray crlBytes(reinterpret_cast<const char*>(crlBody_.data()),
                         static_cast<int>(crlBody_.size()));
    mockNam.setOcspBody(ocspBytes);
    mockNam.setCrlBody(crlBytes);
    mockNam.setStallReplies(true);

    ValidationCoordinator coord(&mockNam, nullptr);
    coord.setSkipDialogForTests(true);
    coord.setTestDialogResult(true);
    coord.setTestUrls(QStringList{"http://ocsp.example.com/stall"},
                       QStringList{"http://crl.example.com/stall.crl"});
    coord.setOverrideOcspDer(ocspBytes);
    coord.setOverrideCrlDer(crlBytes);

    std::unique_ptr<Document> ownerCurrent = Document::open(sigCtx_, work);
    ASSERT_NE(ownerCurrent, nullptr);
    coord.setLiveDocumentResolver([&ownerCurrent]() -> Document* {
        return ownerCurrent.get();
    });
    bool writeScopeEntered = false;
    coord.setWriteScope([&](const std::function<void()>& write) {
        writeScopeEntered = true;
        if (!ownerCurrent) throw pdfe::core::PdfeException("no document");
        write();
    });

    QSignalSpy completedSpy(&coord, &ValidationCoordinator::completed);
    QSignalSpy failedSpy(&coord, &ValidationCoordinator::failed);

    ASSERT_TRUE(coord.request(nullptr, ownerCurrent.get()));
    QCoreApplication::processEvents();
    ASSERT_GT(mockNam.stalledCount(), 0);

    // Close, replace the FILE AT THE SAME PATH with a document carrying no
    // signature at all, and re-open it. Path identical; signature gone.
    ownerCurrent.reset();
    std::filesystem::copy_file(unsignedSrc, work,
                               std::filesystem::copy_options::overwrite_existing);
    const std::string beforeUnsigned = readAllBytesOf(work);
    ownerCurrent = Document::open(sigCtx_, work);
    ASSERT_NE(ownerCurrent, nullptr);

    mockNam.releaseStalledReplies();
    waitForCondition([&]() {
        return completedSpy.count() > 0 || failedSpy.count() > 0;
    }, 5000);

    EXPECT_EQ(completedSpy.count(), 0);
    ASSERT_EQ(failedSpy.count(), 1);
    const QString reason = failedSpy.takeFirst().at(0).toString();
    EXPECT_TRUE(reason.contains(
                    QString::fromUtf8(
                        ValidationCoordinator::kDocumentReplacedMessage)))
        << "the path matches, so only the /Contents arm can refuse this — if "
           "the message is anything else, that arm is not running. Got: "
        << reason.toStdString();
    EXPECT_FALSE(writeScopeEntered);

    ownerCurrent.reset();
    EXPECT_EQ(readAllBytesOf(work), beforeUnsigned)
        << "the unrelated document sitting at that path must be untouched";

    std::error_code ec;
    std::filesystem::remove(work, ec);
}

// THE DISCRIMINATING NEGATIVE — the gate must accept a legitimate target.
//
// Same file, same signature, DIFFERENT Document object at a DIFFERENT address.
// This is the case a pointer-equality check would wrongly refuse, and it is
// what keeps the two tests above from being satisfied by a guard that simply
// refuses every deferred write.
TEST_F(ValidationRequestTest, AcceptsTheSameDocumentReopenedAtANewAddress)
{
    if (signedFixture_.empty() || ocspBody_.empty() || crlBody_.empty())
        GTEST_SKIP() << "OCSP/CRL fixtures unavailable";

    const auto work = signedWorkCopy(signedFixture_, "validation_reopened.pdf");

    auto original = Document::open(sigCtx_, work);
    ASSERT_NE(original, nullptr);

    DssCounts before;
    LtvDssWriter::inspectDss(original.get(), "", before);
    ASSERT_FALSE(before.hasDss);

    MockNetworkAccessManager mockNam;
    QByteArray ocspBytes(reinterpret_cast<const char*>(ocspBody_.data()),
                          static_cast<int>(ocspBody_.size()));
    QByteArray crlBytes(reinterpret_cast<const char*>(crlBody_.data()),
                         static_cast<int>(crlBody_.size()));
    mockNam.setOcspBody(ocspBytes);
    mockNam.setCrlBody(crlBytes);
    mockNam.setStallReplies(true);

    ValidationCoordinator coord(&mockNam, nullptr);
    coord.setSkipDialogForTests(true);
    coord.setTestDialogResult(true);
    coord.setTestUrls(QStringList{"http://ocsp.example.com/stall"},
                       QStringList{"http://crl.example.com/stall.crl"});
    coord.setOverrideOcspDer(ocspBytes);
    coord.setOverrideCrlDer(crlBytes);

    std::unique_ptr<Document> ownerCurrent;
    coord.setLiveDocumentResolver([&ownerCurrent]() -> Document* {
        return ownerCurrent.get();
    });
    coord.setWriteScope([&](const std::function<void()>& write) {
        if (!ownerCurrent) throw pdfe::core::PdfeException("no document");
        write();
    });

    QSignalSpy completedSpy(&coord, &ValidationCoordinator::completed);
    QSignalSpy failedSpy(&coord, &ValidationCoordinator::failed);

    ownerCurrent = std::move(original);
    ASSERT_TRUE(coord.request(nullptr, ownerCurrent.get()));
    QCoreApplication::processEvents();
    ASSERT_GT(mockNam.stalledCount(), 0);

    // Close and RE-OPEN the very same file. A different Document object, and in
    // general a different address; same path, same signature.
    ownerCurrent.reset();
    ownerCurrent = Document::open(sigCtx_, work);
    ASSERT_NE(ownerCurrent, nullptr);

    mockNam.releaseStalledReplies();
    waitForCondition([&]() {
        return completedSpy.count() > 0 || failedSpy.count() > 0;
    }, 10000);

    EXPECT_EQ(failedSpy.count(), 0)
        << "a re-opened Document on the SAME file with the SAME signature is "
           "the right target and must be accepted; refusing it would make the "
           "gate a blanket refusal. Reason: "
        << (failedSpy.count() > 0
              ? failedSpy.takeFirst().at(0).toString().toStdString()
              : std::string("(none)"));
    EXPECT_EQ(completedSpy.count(), 1);

    ownerCurrent.reset();

    auto reopened = Document::open(sigCtx_, work);
    ASSERT_NE(reopened, nullptr);
    DssCounts after;
    ASSERT_TRUE(LtvDssWriter::inspectDss(reopened.get(), "", after));
    EXPECT_TRUE(after.hasDss)
        << "the /DSS must actually have been written to the accepted target";
    EXPECT_GT(after.topCerts, 0);
    reopened.reset();

    std::error_code ec;
    std::filesystem::remove(work, ec);
}

// Bonus: prove that creating ValidationCoordinator with no document
// emits failed() synchronously (defensive guard).
TEST_F(ValidationRequestTest, RejectsNullDocument)
{
    MockNetworkAccessManager mockNam;
    ValidationCoordinator coord(&mockNam, nullptr);
    coord.setSkipDialogForTests(true);

    QSignalSpy failSpy(&coord, &ValidationCoordinator::failed);
    EXPECT_FALSE(coord.request(nullptr, nullptr));
    EXPECT_EQ(failSpy.count(), 1);
    EXPECT_EQ(mockNam.createCount(), 0);
}

#include "test_validation_request.moc"
