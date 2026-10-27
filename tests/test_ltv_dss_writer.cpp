// -*- coding: utf-8 -*-
// Pipeline F phase-7: long-term-validation (LTV) data embedding.
//
// NOTE (meta-audit F20, gap PDF-34): this header used to say "LTV foundation
// for PAdES-B-LT signatures". PDFE reaches no ETSI baseline profile — see
// docs/signature-profile-support.md and tests/test_pades_profiles.cpp. /DSS
// is ISO 32000-2 §12.8.4.3 validation data on its own terms.
//
// LtvDssWriter writes a /DSS dictionary to /Catalog with /Certs +
// /VRI sub-dict, committed via incremental save so the original
// signature ByteRange is preserved.
//
// Tests cover:
//   (a) /DSS exists in /Catalog after write,
//   (b) /VRI keyed by uppercase-hex SHA-1 of Contents hex,
//   (c) Round-trip: reopen and read back cert count,
//   (d) Empty OCSP/CRL produce no /OCSP /CRL in VRI,
//   (e) Multiple certs supported,
//   (f) Signature ByteRange unchanged after DSS insertion.
//
// CertChainExtractor coverage rolls in: chain length 0..N from a real
// PKCS#7 blob (the test_signed.pdf signature).

#include <gtest/gtest.h>

#include "signatures/LtvDssWriter.h"
#include "signatures/CertChainExtractor.h"
#include "signatures/SignatureManager.h"
#include "signatures/SignatureData.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"

#include <QRectF>

#include <chrono>
#include <cstdint>
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
    auto d = std::filesystem::temp_directory_path() / "pdfe_ltv_dss";
    std::filesystem::create_directories(d);
    return d;
}

std::filesystem::path copyFixture(const char* fixture, const char* outName)
{
    auto src = std::filesystem::path(fixturesDir()) / fixture;
    auto dst = tempWorkDir() / outName;
    std::filesystem::remove(dst);
    std::filesystem::copy_file(src, dst);
    return dst;
}

std::string readAllBinary(const std::filesystem::path& p)
{
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

DssCounts inspectDssAt(std::shared_ptr<MuPdfContext> ctx,
                        const std::filesystem::path& path,
                        const std::string& vriKey)
{
    DssCounts c{};
    auto doc = Document::open(ctx, path);
    if (!doc) return c;
    LtvDssWriter::inspectDss(doc.get(), vriKey, c);
    return c;
}

// Build a freshly-signed PDF in @p outPath from a plain fixture using
// the SignatureManager + test_signer.pfx flow. Returns true on success.
//
// The pre-built tests/fixtures/test_signed.pdf only carries an empty
// signature field (the README warns the C++ test suite is responsible
// for actually signing). LtvDssWriter needs a real /V/Contents to
// extract a chain, so we sign on-the-fly here. The flow mirrors
// SignatureManagerTest.SignAndVerify, which is known good.
bool buildSignedFixture(std::shared_ptr<MuPdfContext> ctx,
                        const std::filesystem::path& outPath)
{
    auto fixtures = std::filesystem::path(fixturesDir());
    auto pfx = fixtures / "test_signer.pfx";
    auto srcPdf = fixtures / "test_single.pdf";
    if (!std::filesystem::exists(pfx) || !std::filesystem::exists(srcPdf))
        return false;

    SignatureManager mgr(ctx);

    // Step 1: add a sig field and saveAs to outPath.
    {
        auto doc = Document::open(ctx, srcPdf);
        if (!doc) return false;
        auto page = doc->page(0);
        mgr.createSignatureField(page.get(), "LtvSig",
                                  QRectF(100, 500, 200, 50));
        std::filesystem::remove(outPath);
        doc->saveAs(outPath);
    }

    // Step 2: re-open and sign that field. signField() does an
    // incremental save internally.
    {
        auto doc = Document::open(ctx, outPath);
        if (!doc) return false;
        auto page = doc->page(0);
        SigningOptions opts;
        opts.pfxPath = pfx.string();
        opts.pfxPassword = "test123";
        opts.reason = "LTV test";
        opts.location = "PDFE test suite";
        mgr.signField(doc.get(), page.get(), 0, opts);
    }
    return true;
}

} // anonymous namespace

class LtvDssWriterTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        // Sign once for the whole suite — signing is slow (PKCS#12 +
        // RSA + incremental save). The signed fixture is shared across
        // all tests; tests workCopy() it before mutating.
        sigCtx_ = std::make_shared<MuPdfContext>();
        signedFixture_ = tempWorkDir() / "ltv_signed_source.pdf";
        try {
            if (!buildSignedFixture(sigCtx_, signedFixture_))
                signedFixture_.clear();
        } catch (const std::exception&) {
            signedFixture_.clear();
        }
    }

    static void TearDownTestSuite() {
        std::error_code ec;
        if (!signedFixture_.empty())
            std::filesystem::remove(signedFixture_, ec);
        sigCtx_.reset();
    }

    void SetUp() override {
        if (signedFixture_.empty() ||
            !std::filesystem::exists(signedFixture_)) {
            GTEST_SKIP() << "On-the-fly signing failed; check OpenSSL/Pkcs7 "
                            "bridge and test_signer.pfx";
        }
        // Use the same context that signed the doc — re-opening the
        // freshly-signed file in a fresh context fails with "object out
        // of range" (xref cache is keyed to context). SignatureManager
        // tests use the same pattern (single context_ throughout).
        ctx = sigCtx_;
        auto doc = Document::open(ctx, signedFixture_);
        if (!doc ||
            !LtvDssWriter::extractFirstSignatureInputs(doc.get(), fixture_))
        {
            GTEST_SKIP() << "Could not parse on-the-fly signed fixture";
        }
    }

    // Convenience: copy the signed source fixture to a per-test temp file.
    std::filesystem::path workCopy(const char* outName) {
        auto dst = tempWorkDir() / outName;
        std::filesystem::remove(dst);
        std::filesystem::copy_file(signedFixture_, dst);
        return dst;
    }

    std::shared_ptr<MuPdfContext> ctx;
    SignatureInputs fixture_;

    static std::shared_ptr<MuPdfContext> sigCtx_;
    static std::filesystem::path signedFixture_;
};

std::shared_ptr<MuPdfContext> LtvDssWriterTest::sigCtx_;
std::filesystem::path LtvDssWriterTest::signedFixture_;

// (a) /DSS exists in /Catalog after write, with at least one /Certs entry.
TEST_F(LtvDssWriterTest, WriteOneCert_DssDictExistsInCatalog)
{
    auto work = workCopy("dss_one_cert.pdf");
    auto doc = Document::open(ctx, work);
    ASSERT_NE(doc, nullptr);

    LtvDssWriter writer;
    std::vector<std::vector<uint8_t>> oneCert = { fixture_.certChain.front() };
    ASSERT_TRUE(writer.write(doc.get(), fixture_.contentsHex, oneCert));
    doc.reset();

    auto vriKey = LtvDssWriter::sha1HexUpper(fixture_.contentsHex);
    auto counts = inspectDssAt(ctx, work, vriKey);
    EXPECT_TRUE(counts.hasDss);
    EXPECT_GE(counts.topCerts, 1);
}

// (b) /VRI is keyed by SHA-1(contentsHex), uppercase hex.
TEST_F(LtvDssWriterTest, VriKeyIsSha1OfContentsHexUpper)
{
    auto work = workCopy("dss_vri_key.pdf");
    auto doc = Document::open(ctx, work);
    ASSERT_NE(doc, nullptr);

    LtvDssWriter writer;
    ASSERT_TRUE(writer.write(doc.get(), fixture_.contentsHex,
                             fixture_.certChain));
    doc.reset();

    auto vriKey = LtvDssWriter::sha1HexUpper(fixture_.contentsHex);
    EXPECT_EQ(vriKey.size(), 40u) << "SHA-1 hex must be 40 chars";
    for (char ch : vriKey) {
        EXPECT_TRUE((ch >= '0' && ch <= '9') ||
                    (ch >= 'A' && ch <= 'F'))
            << "VRI key must be uppercase hex; got: " << vriKey;
    }
    auto counts = inspectDssAt(ctx, work, vriKey);
    EXPECT_TRUE(counts.hasVriKey)
        << "VRI must contain entry under SHA-1(ContentsHex)";
    EXPECT_GE(counts.vriCerts, 1);
}

// (c) Round-trip: open signed fixture, write DSS, reopen, count certs.
TEST_F(LtvDssWriterTest, RoundTripCertCount)
{
    auto work = workCopy("dss_roundtrip.pdf");
    auto doc = Document::open(ctx, work);
    ASSERT_NE(doc, nullptr);

    LtvDssWriter writer;
    const size_t expectedCerts = fixture_.certChain.size();
    ASSERT_TRUE(writer.write(doc.get(), fixture_.contentsHex,
                             fixture_.certChain));
    doc.reset();

    auto vriKey = LtvDssWriter::sha1HexUpper(fixture_.contentsHex);
    auto counts = inspectDssAt(ctx, work, vriKey);
    EXPECT_TRUE(counts.hasDss);
    EXPECT_EQ(static_cast<size_t>(counts.topCerts), expectedCerts);
    EXPECT_EQ(static_cast<size_t>(counts.vriCerts), expectedCerts);
}

// (d) Empty OCSP/CRL arrays produce no /OCSP or /CRL keys in VRI.
TEST_F(LtvDssWriterTest, EmptyOcspCrlOmittedFromVri)
{
    auto work = workCopy("dss_no_ocsp.pdf");
    auto doc = Document::open(ctx, work);
    ASSERT_NE(doc, nullptr);

    LtvDssWriter writer;
    ASSERT_TRUE(writer.write(doc.get(), fixture_.contentsHex,
                             fixture_.certChain,
                             /*ocsps=*/{}, /*crls=*/{}));
    doc.reset();

    auto vriKey = LtvDssWriter::sha1HexUpper(fixture_.contentsHex);
    auto counts = inspectDssAt(ctx, work, vriKey);
    EXPECT_TRUE(counts.hasVriKey);
    EXPECT_FALSE(counts.vriHasOcspKey)
        << "Empty ocsps must omit /OCSP";
    EXPECT_FALSE(counts.vriHasCrlKey)
        << "Empty crls must omit /CRL";
    EXPECT_EQ(counts.topOcsps, 0);
    EXPECT_EQ(counts.topCrls, 0);
}

// (e) Multiple certs (we synthesize a duplicate-chain to get N>1 even on a
//     self-signed fixture), all reach /Certs and /VRI/<key>/Cert.
TEST_F(LtvDssWriterTest, WriteMultipleCerts)
{
    auto work = workCopy("dss_multi_certs.pdf");
    auto doc = Document::open(ctx, work);
    ASSERT_NE(doc, nullptr);

    // Build a 3-entry chain by repeating the real signer DER. Real-world
    // chains have leaf+intermediate+root; for the writer's purposes any
    // 3 distinct-by-pointer DER blobs exercise the same code path.
    std::vector<std::vector<uint8_t>> multi;
    multi.push_back(fixture_.certChain.front());
    auto dup = fixture_.certChain.front();
    if (!dup.empty()) dup[0] ^= 0x00; // identity tweak — keep as-is
    multi.push_back(dup);
    multi.push_back(fixture_.certChain.front());

    LtvDssWriter writer;
    ASSERT_TRUE(writer.write(doc.get(), fixture_.contentsHex, multi));
    doc.reset();

    auto vriKey = LtvDssWriter::sha1HexUpper(fixture_.contentsHex);
    auto counts = inspectDssAt(ctx, work, vriKey);
    EXPECT_GE(counts.topCerts, 3);
    EXPECT_GE(counts.vriCerts, 3);
}

// (f) Signature ByteRange invariant: original byte prefix is preserved
//     intact after DSS insertion. /ByteRange continues to point into the
//     same span.
TEST_F(LtvDssWriterTest, IncrementalSavePreservesByteRange)
{
    auto work = workCopy("dss_incremental.pdf");
    const std::string before = readAllBinary(work);

    auto doc = Document::open(ctx, work);
    ASSERT_NE(doc, nullptr);

    LtvDssWriter writer;
    ASSERT_TRUE(writer.write(doc.get(), fixture_.contentsHex,
                             fixture_.certChain));
    doc.reset();

    const std::string after = readAllBinary(work);
    ASSERT_GT(after.size(), before.size())
        << "Incremental save must grow the file";
    ASSERT_EQ(after.compare(0, before.size(), before), 0)
        << "Original byte prefix must be preserved (signature ByteRange "
        << "covers exactly that prefix)";

    // ByteRange smoke-check: the original first range still ends within
    // the preserved prefix.
    const int64_t firstRangeEnd =
        fixture_.firstByteRangeOffset + fixture_.firstByteRangeLength;
    EXPECT_LE(firstRangeEnd, static_cast<int64_t>(before.size()))
        << "Signed first byte range must terminate within preserved prefix";
}

// CertChainExtractor: chain length 0 path (empty input).
TEST_F(LtvDssWriterTest, CertChainExtractor_EmptyInput)
{
    auto chain = CertChainExtractor::extractChain(nullptr, 0);
    EXPECT_TRUE(chain.empty());

    // 5 bytes that look like a SEQUENCE header with bogus length. d2i_PKCS7
    // should reject this cleanly.
    uint8_t bogus[] = {0x30, 0x82, 0x00, 0x05, 0xFF};
    auto chain2 = CertChainExtractor::extractChain(bogus, sizeof(bogus));
    EXPECT_TRUE(chain2.empty()) << "Truncated DER must not parse";
}

// CertChainExtractor: chain length N from the real signed fixture.
TEST_F(LtvDssWriterTest, CertChainExtractor_RealSignature)
{
    auto chain = CertChainExtractor::extractChain(
        fixture_.contentsBin.data(), fixture_.contentsBin.size());
    EXPECT_FALSE(chain.empty());
    for (const auto& der : chain) {
        EXPECT_GT(der.size(), 100u) << "X.509 DER should be >100 bytes";
        // A DER cert always starts with SEQUENCE (0x30).
        EXPECT_EQ(der[0], 0x30) << "X.509 DER must start with SEQUENCE tag";
    }
}

// SHA-1 helper sanity (uppercase hex, 40 chars, deterministic).
TEST_F(LtvDssWriterTest, Sha1HexUpper_KnownVector)
{
    // Known vector: SHA-1("abc") = a9993e364706816aba3e25717850c26c9cd0d89d
    auto h = LtvDssWriter::sha1HexUpper("abc");
    EXPECT_EQ(h, "A9993E364706816ABA3E25717850C26C9CD0D89D");
}

// Performance: 10-cert chain write in < 500 ms (logged, soft-asserted).
TEST_F(LtvDssWriterTest, Perf_TenCertChainUnder500ms)
{
    auto work = workCopy("dss_perf.pdf");
    auto doc = Document::open(ctx, work);
    ASSERT_NE(doc, nullptr);

    std::vector<std::vector<uint8_t>> ten;
    for (int i = 0; i < 10; ++i) {
        ten.push_back(fixture_.certChain.front());
    }

    LtvDssWriter writer;
    auto t0 = std::chrono::steady_clock::now();
    ASSERT_TRUE(writer.write(doc.get(), fixture_.contentsHex, ten));
    auto t1 = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0)
                  .count();

    // Hard 500 ms ceiling per phase-7 perf budget.
    EXPECT_LT(ms, 500) << "10-cert /DSS write took " << ms << " ms (budget 500)";
    std::cerr << "[perf] LtvDssWriter::write 10-cert chain: " << ms
              << " ms\n";
}
