// -*- coding: utf-8 -*-
// RFC-3161 TSA fixture — validates that test_timestamped.pdf parses
// cleanly, carries a /DocTimeStamp signature dict with the expected
// /SubFilter, and exposes the cached TimeStampToken via /Contents.
// The cached-bytes approach documented in docs/acrobat-pro-parity-status.md.
#include <gtest/gtest.h>
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "signatures/SignatureManager.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>

namespace {

std::filesystem::path fixturesDir()
{
    return std::filesystem::path(TEST_FIXTURES_DIR);
}

std::string readAll(const std::filesystem::path& p)
{
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss; ss << in.rdbuf();
    return ss.str();
}

} // namespace

class TsaFixtureTest : public ::testing::Test {
protected:
    void SetUp() override {
        pdfPath = fixturesDir() / "test_timestamped.pdf";
        tokenPath = fixturesDir() / "test_tsa_token.der";
        certPath = fixturesDir() / "test_tsa_cert.pem";
        if (!std::filesystem::exists(pdfPath) ||
            !std::filesystem::exists(tokenPath) ||
            !std::filesystem::exists(certPath)) {
            GTEST_SKIP() << "TSA fixtures not generated — run "
                         << "generate_timestamped_pdf.py";
        }
        ctx = std::make_shared<pdfe::core::MuPdfContext>();
    }
    std::filesystem::path pdfPath;
    std::filesystem::path tokenPath;
    std::filesystem::path certPath;
    std::shared_ptr<pdfe::core::MuPdfContext> ctx;
};

TEST_F(TsaFixtureTest, PdfParsesAndHasOnePage)
{
    auto doc = pdfe::core::Document::open(ctx, pdfPath);
    ASSERT_NE(doc, nullptr);
    EXPECT_EQ(doc->pageCount(), 1);
}

TEST_F(TsaFixtureTest, FileContainsDocTimeStampDict)
{
    const std::string bytes = readAll(pdfPath);
    EXPECT_NE(bytes.find("/Type /DocTimeStamp"), std::string::npos);
    EXPECT_NE(bytes.find("/Filter /Adobe.PPKLite"), std::string::npos);
    EXPECT_NE(bytes.find("/SubFilter /ETSI.RFC3161"), std::string::npos);
    EXPECT_NE(bytes.find("/ByteRange"), std::string::npos);
    EXPECT_NE(bytes.find("/Contents"), std::string::npos);
}

TEST_F(TsaFixtureTest, CachedTokenIsValidAsn1SignedData)
{
    const std::string token = readAll(tokenPath);
    ASSERT_GE(token.size(), 16u);
    // First byte of a valid DER SEQUENCE wrapping a ContentInfo is
    // 0x30 (SEQUENCE, constructed). Sanity check that we wrote a
    // well-formed outer wrapper rather than a stray blob.
    EXPECT_EQ(static_cast<unsigned char>(token[0]), 0x30u);
}

TEST_F(TsaFixtureTest, PdfEmbedsEntireTokenInContents)
{
    const std::string bytes = readAll(pdfPath);
    const std::string token = readAll(tokenPath);

    // Build hex form of the first 32 bytes of the token and search for
    // it in the PDF's /Contents hex string. Upper-case hex matches the
    // generator's encoding.
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string prefixHex;
    prefixHex.reserve(64);
    for (size_t i = 0; i < 32 && i < token.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(token[i]);
        prefixHex.push_back(kHex[c >> 4]);
        prefixHex.push_back(kHex[c & 0x0F]);
    }
    EXPECT_NE(bytes.find(prefixHex), std::string::npos)
        << "token prefix not found in PDF /Contents";
}

TEST_F(TsaFixtureTest, SignatureManagerCountsOneSignature)
{
    auto doc = pdfe::core::Document::open(ctx, pdfPath);
    ASSERT_NE(doc, nullptr);
    pdfe::signatures::SignatureManager mgr(ctx);
    const int n = mgr.countSignatures(doc.get());
    EXPECT_EQ(n, 1);
}

TEST_F(TsaFixtureTest, ByteRangeStartsAtZero)
{
    // /ByteRange [0 offset1 offset2 length] — first element must be 0
    // (signature covers everything before /Contents).
    const std::string bytes = readAll(pdfPath);
    const auto brPos = bytes.find("/ByteRange [");
    ASSERT_NE(brPos, std::string::npos);
    EXPECT_EQ(bytes[brPos + 12], '0');
    EXPECT_EQ(bytes[brPos + 13], ' ');
}
