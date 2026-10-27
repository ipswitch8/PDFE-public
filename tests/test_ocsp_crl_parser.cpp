// -*- coding: utf-8 -*-
// Pipeline F phase-8: OCSP/CRL parsing from local files (offline LTV).
//
// OcspResponseParser and CrlParser ingest DER blobs from disk, validate
// them with OpenSSL's d2i_OCSP_RESPONSE / d2i_X509_CRL, and feed the
// parsed bytes into LtvDssWriter for /DSS embedding.
//
// Tests cover:
//   (1) parse a valid OCSP response from a fixture synthesized at suite
//       setup using OpenSSL APIs (OCSP_basic_sign on a self-signed test
//       cert),
//   (2) parse a valid CRL from a fixture synthesized at suite setup
//       (X509_CRL_new + X509_CRL_sign, signed with the test PFX),
//   (3) malformed OCSP rejects without crash (50-byte garbage buffer),
//   (4) malformed CRL likewise,
//   (5) LtvDssWriter::writeWithFiles with 1 OCSP + 1 CRL embeds both into
//       /DSS and round-trip reads them back via inspectDss.
//
// All fixtures are generated locally via OpenSSL APIs — no network calls,
// no command-line tooling, no external dependencies beyond the OpenSSL
// already linked into pdfe_signatures.

#include <gtest/gtest.h>

#include "signatures/OcspResponseParser.h"
#include "signatures/CrlParser.h"
#include "signatures/LtvDssWriter.h"
#include "signatures/SignatureManager.h"
#include "signatures/SignatureData.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"

#include <openssl/asn1.h>
#include <openssl/evp.h>
#include <openssl/ocsp.h>
#include <openssl/pkcs12.h>
#include <openssl/x509.h>

#include <QRectF>

#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

using namespace pdfe::signatures;
using namespace pdfe::core;

namespace {

std::string fixturesDir() { return std::string(TEST_FIXTURES_DIR); }

std::filesystem::path tempWorkDir()
{
    auto d = std::filesystem::temp_directory_path() / "pdfe_ocsp_crl";
    std::filesystem::create_directories(d);
    return d;
}

// --- Utilities for synthesizing fixtures via OpenSSL ----------------------

// Read the test PFX file and unwrap (key, cert) — same flow as
// Pkcs7OpenSslBridge::readPfx, but without the MuPDF longjmp gymnastics.
// On failure both pointers stay null. Caller frees with EVP_PKEY_free /
// X509_free.
bool loadTestSigner(EVP_PKEY*& outKey, X509*& outCert)
{
    outKey = nullptr;
    outCert = nullptr;

    auto pfxPath =
        std::filesystem::path(fixturesDir()) / "test_signer.pfx";
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

// Build a minimal valid OCSP response, signed with the test signer cert.
// Self-issued: signer signs a "good" status for itself. This produces a
// structurally valid OCSP_RESPONSE that d2i_OCSP_RESPONSE accepts and
// OCSP_resp_get0 can walk.
//
// Returns the DER bytes on success; empty on failure.
std::vector<uint8_t> buildOcspResponseDer(EVP_PKEY* key, X509* cert)
{
    std::vector<uint8_t> result;
    if (!key || !cert) return result;

    OCSP_BASICRESP* basic = OCSP_BASICRESP_new();
    if (!basic) return result;

    // CertID for "this cert was issued by itself" — fine for a structurally
    // valid response. The verifier on parse() side does not check trust.
    OCSP_CERTID* certId = OCSP_cert_to_id(EVP_sha1(), cert, cert);
    if (!certId) {
        OCSP_BASICRESP_free(basic);
        return result;
    }

    // thisUpdate = now, nextUpdate = now + 1 day.
    ASN1_TIME* thisUpd = ASN1_TIME_set(nullptr, time(nullptr));
    ASN1_TIME* nextUpd = ASN1_TIME_set(nullptr, time(nullptr) + 86400);
    if (!thisUpd || !nextUpd) {
        if (thisUpd) ASN1_TIME_free(thisUpd);
        if (nextUpd) ASN1_TIME_free(nextUpd);
        OCSP_CERTID_free(certId);
        OCSP_BASICRESP_free(basic);
        return result;
    }

    // status = good (0). OCSP_basic_add1_status takes ownership-like refs;
    // we keep them and free below.
    OCSP_SINGLERESP* single = OCSP_basic_add1_status(
        basic, certId, V_OCSP_CERTSTATUS_GOOD,
        0, nullptr, thisUpd, nextUpd);

    OCSP_CERTID_free(certId);
    ASN1_TIME_free(thisUpd);
    ASN1_TIME_free(nextUpd);

    if (!single) {
        OCSP_BASICRESP_free(basic);
        return result;
    }

    // Sign the basic response. flags=0 means "include cert in response".
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

// Build a minimal valid CRL, signed with the test signer cert. Issuer ==
// signer subject (self-issued CRL). Empty revoked list — that's still a
// valid CRL ("nobody is revoked yet").
std::vector<uint8_t> buildCrlDer(EVP_PKEY* key, X509* cert)
{
    std::vector<uint8_t> result;
    if (!key || !cert) return result;

    X509_CRL* crl = X509_CRL_new();
    if (!crl) return result;

    // CRL v2 (value 1, since version field is zero-indexed).
    if (X509_CRL_set_version(crl, 1) != 1) {
        X509_CRL_free(crl);
        return result;
    }

    // Issuer = signer's subject. set_issuer_name copies internally.
    if (X509_CRL_set_issuer_name(crl, X509_get_subject_name(cert)) != 1) {
        X509_CRL_free(crl);
        return result;
    }

    ASN1_TIME* lastUpd = ASN1_TIME_set(nullptr, time(nullptr));
    ASN1_TIME* nextUpd = ASN1_TIME_set(nullptr, time(nullptr) + 7 * 86400);
    if (!lastUpd || !nextUpd) {
        if (lastUpd) ASN1_TIME_free(lastUpd);
        if (nextUpd) ASN1_TIME_free(nextUpd);
        X509_CRL_free(crl);
        return result;
    }

    int ok1 = X509_CRL_set1_lastUpdate(crl, lastUpd);
    int ok2 = X509_CRL_set1_nextUpdate(crl, nextUpd);
    ASN1_TIME_free(lastUpd);
    ASN1_TIME_free(nextUpd);
    if (ok1 != 1 || ok2 != 1) {
        X509_CRL_free(crl);
        return result;
    }

    // Sort (no-op for empty revoked list, but good practice) then sign.
    X509_CRL_sort(crl);
    if (X509_CRL_sign(crl, key, EVP_sha256()) <= 0) {
        X509_CRL_free(crl);
        return result;
    }

    unsigned char* der = nullptr;
    int derLen = i2d_X509_CRL(crl, &der);
    X509_CRL_free(crl);
    if (derLen <= 0 || !der) {
        if (der) OPENSSL_free(der);
        return result;
    }
    result.assign(der, der + derLen);
    OPENSSL_free(der);
    return result;
}

bool writeAllBinary(const std::filesystem::path& path,
                    const std::vector<uint8_t>& bytes)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    return f.good();
}

// Mirrors the helper in test_ltv_dss_writer.cpp: build a freshly-signed
// PDF in @p outPath using the SignatureManager flow. Required because the
// pre-built test_signed.pdf only carries an empty signature field, but
// LtvDssWriter::extractFirstSignatureInputs needs a real /V/Contents.
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
        mgr.createSignatureField(page.get(), "OcspCrlSig",
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
        opts.reason = "OCSP/CRL test";
        opts.location = "PDFE test suite";
        mgr.signField(doc.get(), page.get(), 0, opts);
    }
    return true;
}

} // anonymous namespace

class OcspCrlParserTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        // Generate fixtures once for the whole suite. Synthesizing these
        // is cheap (well under a second) but doing it per-test would be
        // wasteful and would also re-prompt OpenSSL key derivation.
        EVP_PKEY* key = nullptr;
        X509* cert = nullptr;
        if (!loadTestSigner(key, cert)) {
            // Leave fixturesAvailable_ false; SetUp() will GTEST_SKIP.
            return;
        }

        auto ocspBytes = buildOcspResponseDer(key, cert);
        auto crlBytes = buildCrlDer(key, cert);

        EVP_PKEY_free(key);
        X509_free(cert);

        if (ocspBytes.empty() || crlBytes.empty()) {
            return;
        }

        auto dir = tempWorkDir();
        ocspFixturePath_ = dir / "mock_ocsp_response.der";
        crlFixturePath_ = dir / "mock_crl.der";

        if (!writeAllBinary(ocspFixturePath_, ocspBytes)) return;
        if (!writeAllBinary(crlFixturePath_, crlBytes)) return;

        // Sign a PDF for the round-trip test (matches phase-7's pattern).
        sigCtx_ = std::make_shared<MuPdfContext>();
        signedFixture_ = dir / "ocsp_crl_signed_source.pdf";
        try {
            if (!buildSignedFixture(sigCtx_, signedFixture_))
                signedFixture_.clear();
        } catch (const std::exception&) {
            signedFixture_.clear();
        }

        fixturesAvailable_ = true;
    }

    static void TearDownTestSuite() {
        std::error_code ec;
        if (!ocspFixturePath_.empty())
            std::filesystem::remove(ocspFixturePath_, ec);
        if (!crlFixturePath_.empty())
            std::filesystem::remove(crlFixturePath_, ec);
        if (!signedFixture_.empty())
            std::filesystem::remove(signedFixture_, ec);
        sigCtx_.reset();
    }

    static bool fixturesAvailable_;
    static std::filesystem::path ocspFixturePath_;
    static std::filesystem::path crlFixturePath_;
    static std::filesystem::path signedFixture_;
    static std::shared_ptr<MuPdfContext> sigCtx_;
};

bool OcspCrlParserTest::fixturesAvailable_ = false;
std::filesystem::path OcspCrlParserTest::ocspFixturePath_;
std::filesystem::path OcspCrlParserTest::crlFixturePath_;
std::filesystem::path OcspCrlParserTest::signedFixture_;
std::shared_ptr<MuPdfContext> OcspCrlParserTest::sigCtx_;

// (1) Parse the synthesized OCSP fixture: derBytes non-empty, thisUpdate
//     populated, certStatus == good (we set it that way in the fixture).
TEST_F(OcspCrlParserTest, ParsesValidOcspResponse)
{
    if (!fixturesAvailable_) {
        GTEST_SKIP() << "OCSP fixture synthesis failed; check OpenSSL build";
    }

    OcspResponse parsed;
    ASSERT_TRUE(OcspResponseParser::parse(ocspFixturePath_, parsed))
        << "Reason: " << parsed.statusMessage;

    EXPECT_FALSE(parsed.derBytes.empty());
    // OCSP DER always starts with SEQUENCE (0x30).
    ASSERT_GE(parsed.derBytes.size(), 2u);
    EXPECT_EQ(parsed.derBytes[0], 0x30u);

    EXPECT_GT(parsed.thisUpdate, 0)
        << "thisUpdate should have been extracted from the fixture";
    EXPECT_GT(parsed.nextUpdate, parsed.thisUpdate)
        << "nextUpdate should be after thisUpdate";
    EXPECT_EQ(parsed.certStatus, V_OCSP_CERTSTATUS_GOOD)
        << "Fixture was synthesized with status=good";
    EXPECT_EQ(parsed.statusMessage, "good");
}

// (2) Parse the synthesized CRL fixture: derBytes non-empty, issuerDN
//     extracted (RFC 2253 form, non-empty for a real cert).
TEST_F(OcspCrlParserTest, ParsesValidCrl)
{
    if (!fixturesAvailable_) {
        GTEST_SKIP() << "CRL fixture synthesis failed; check OpenSSL build";
    }

    CrlInfo parsed;
    ASSERT_TRUE(CrlParser::parse(crlFixturePath_, parsed))
        << "Reason: " << parsed.statusMessage;

    EXPECT_FALSE(parsed.derBytes.empty());
    ASSERT_GE(parsed.derBytes.size(), 2u);
    EXPECT_EQ(parsed.derBytes[0], 0x30u);  // SEQUENCE

    EXPECT_FALSE(parsed.issuerDN.empty())
        << "Issuer DN should parse from a self-issued CRL";
    EXPECT_GT(parsed.thisUpdate, 0);
    EXPECT_GT(parsed.nextUpdate, parsed.thisUpdate);
}

// (3) Malformed OCSP rejects without crash. We hand the parser a 50-byte
//     buffer that looks vaguely like ASN.1 but isn't a valid OCSP response.
TEST_F(OcspCrlParserTest, RejectsMalformedOcsp)
{
    std::vector<uint8_t> garbage(50);
    // Fill with a bogus SEQUENCE-with-length header followed by junk.
    garbage[0] = 0x30; garbage[1] = 0x82; garbage[2] = 0x00; garbage[3] = 0x2E;
    for (size_t i = 4; i < garbage.size(); ++i) {
        garbage[i] = static_cast<uint8_t>((i * 7) & 0xFF);
    }

    OcspResponse parsed;
    EXPECT_FALSE(OcspResponseParser::parseBytes(
        garbage.data(), garbage.size(), parsed));
    EXPECT_TRUE(parsed.derBytes.empty())
        << "Failed parse must not retain bytes";
    EXPECT_FALSE(parsed.statusMessage.empty())
        << "Failure should set a human-readable statusMessage";

    // Also test the file-path overload with a missing file.
    OcspResponse missing;
    auto noPath = tempWorkDir() / "no_such_ocsp_file.der";
    std::error_code ec;
    std::filesystem::remove(noPath, ec);
    EXPECT_FALSE(OcspResponseParser::parse(noPath, missing));
    EXPECT_FALSE(missing.statusMessage.empty());

    // Empty buffer.
    OcspResponse empty;
    EXPECT_FALSE(OcspResponseParser::parseBytes(nullptr, 0, empty));
    EXPECT_FALSE(empty.statusMessage.empty());
}

// (4) Malformed CRL rejects without crash. Same garbage-buffer pattern.
TEST_F(OcspCrlParserTest, RejectsMalformedCrl)
{
    std::vector<uint8_t> garbage(50);
    garbage[0] = 0x30; garbage[1] = 0x82; garbage[2] = 0x00; garbage[3] = 0x2E;
    for (size_t i = 4; i < garbage.size(); ++i) {
        garbage[i] = static_cast<uint8_t>((i * 13) & 0xFF);
    }

    CrlInfo parsed;
    EXPECT_FALSE(CrlParser::parseBytes(
        garbage.data(), garbage.size(), parsed));
    EXPECT_TRUE(parsed.derBytes.empty());
    EXPECT_FALSE(parsed.statusMessage.empty());

    // Missing file path.
    CrlInfo missing;
    auto noPath = tempWorkDir() / "no_such_crl_file.der";
    std::error_code ec;
    std::filesystem::remove(noPath, ec);
    EXPECT_FALSE(CrlParser::parse(noPath, missing));
    EXPECT_FALSE(missing.statusMessage.empty());

    // Empty buffer.
    CrlInfo empty;
    EXPECT_FALSE(CrlParser::parseBytes(nullptr, 0, empty));
    EXPECT_FALSE(empty.statusMessage.empty());
}

// (5) End-to-end: LtvDssWriter::writeWithFiles loads the fixtures, embeds
//     them in /DSS, and the round-trip read sees both.
TEST_F(OcspCrlParserTest, EmbedsOcspCrlInDssRoundTrip)
{
    if (!fixturesAvailable_) {
        GTEST_SKIP() << "Fixture synthesis failed";
    }
    if (signedFixture_.empty() ||
        !std::filesystem::exists(signedFixture_)) {
        GTEST_SKIP() << "On-the-fly signing failed; check OpenSSL/Pkcs7 bridge";
    }

    // Use the same context that signed the doc — re-opening the freshly-
    // signed file in a fresh context fails with "object out of range"
    // (xref cache is keyed to context). Same constraint as phase-7.
    auto ctx = sigCtx_;

    // Per-test work copy of the signed PDF.
    auto work = tempWorkDir() / "dss_with_ocsp_crl.pdf";
    std::error_code ec;
    std::filesystem::remove(work, ec);
    std::filesystem::copy_file(signedFixture_, work);

    auto doc = Document::open(ctx, work);
    ASSERT_NE(doc, nullptr);

    SignatureInputs inputs;
    ASSERT_TRUE(LtvDssWriter::extractFirstSignatureInputs(doc.get(), inputs));
    ASSERT_FALSE(inputs.certChain.empty());

    // 1 cert + 1 OCSP file + 1 CRL file.
    std::vector<std::vector<uint8_t>> certs = { inputs.certChain.front() };
    std::vector<std::filesystem::path> ocspFiles = { ocspFixturePath_ };
    std::vector<std::filesystem::path> crlFiles = { crlFixturePath_ };

    LtvDssWriter writer;
    ASSERT_TRUE(writer.writeWithFiles(
        doc.get(), inputs.contentsHex, certs, ocspFiles, crlFiles));
    doc.reset();

    // Round-trip: re-open and inspect.
    auto vriKey = LtvDssWriter::sha1HexUpper(inputs.contentsHex);
    auto reopened = Document::open(ctx, work);
    ASSERT_NE(reopened, nullptr);

    DssCounts counts{};
    ASSERT_TRUE(LtvDssWriter::inspectDss(reopened.get(), vriKey, counts));
    EXPECT_TRUE(counts.hasDss);
    EXPECT_GE(counts.topCerts, 1);
    EXPECT_EQ(counts.topOcsps, 1)
        << "Exactly one OCSP response should have been embedded";
    EXPECT_EQ(counts.topCrls, 1)
        << "Exactly one CRL should have been embedded";

    EXPECT_TRUE(counts.hasVriKey);
    EXPECT_TRUE(counts.vriHasOcspKey);
    EXPECT_TRUE(counts.vriHasCrlKey);
    EXPECT_GE(counts.vriOcsps, 1);
    EXPECT_GE(counts.vriCrls, 1);
}

// Bonus: writeWithFiles fails cleanly when an input file is missing
// (no partial /DSS write should happen). This locks in the all-or-nothing
// contract documented in the header.
TEST_F(OcspCrlParserTest, WriteWithFiles_MissingFile_FailsCleanly)
{
    if (!fixturesAvailable_) {
        GTEST_SKIP() << "Fixture synthesis failed";
    }
    if (signedFixture_.empty() ||
        !std::filesystem::exists(signedFixture_)) {
        GTEST_SKIP() << "On-the-fly signing failed";
    }

    auto ctx = sigCtx_;
    auto work = tempWorkDir() / "dss_missing_file.pdf";
    std::error_code ec;
    std::filesystem::remove(work, ec);
    std::filesystem::copy_file(signedFixture_, work);

    auto doc = Document::open(ctx, work);
    ASSERT_NE(doc, nullptr);

    SignatureInputs inputs;
    ASSERT_TRUE(LtvDssWriter::extractFirstSignatureInputs(doc.get(), inputs));

    auto bogusOcsp = tempWorkDir() / "definitely_not_here.der";
    std::filesystem::remove(bogusOcsp, ec);

    LtvDssWriter writer;
    EXPECT_FALSE(writer.writeWithFiles(
        doc.get(), inputs.contentsHex,
        { inputs.certChain.front() },
        { bogusOcsp },        // missing → must fail
        { crlFixturePath_ })); // CRL is fine but doesn't matter; whole call fails
}
