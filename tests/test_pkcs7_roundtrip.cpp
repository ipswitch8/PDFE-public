// PHASE4_ROUNDTRIP_PKCS7: symbol-level verification tests for the
// PKCS#7 bridge used by the signature subsystem.
//
// The signature round-trip tests cover the full sign+save+verify
// flow at the document level. This file targets the lower-level
// Pkcs7OpenSslBridge::verifyWithChain primitive directly: it must
// reject empty / wrong / malformed inputs cleanly and accept valid
// ones — without relying on the document layer to filter for it.
//
// We generate a real PKCS#7 SignedData blob by signing test_single
// with the test PFX (the existing infrastructure does this), then
// extract the signature contents bytes from the saved PDF and feed
// them straight into verifyWithChain.

#include <gtest/gtest.h>

#include "signatures/Pkcs7OpenSslBridge.h"
#include "signatures/SignatureManager.h"
#include "signatures/SignatureData.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

using pdfe::signatures::Pkcs7OpenSslBridge;
using pdfe::signatures::SignatureManager;
using pdfe::signatures::SigningOptions;

namespace {

class Pkcs7RoundTripTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_shared<pdfe::core::MuPdfContext>();
        fixture_ = std::filesystem::path(TEST_FIXTURES_DIR) / "test_single.pdf";
        pfxPath_ = std::filesystem::path(TEST_FIXTURES_DIR) / "test_signer.pfx";
        ASSERT_TRUE(std::filesystem::exists(fixture_));
        ASSERT_TRUE(std::filesystem::exists(pfxPath_));

        const ::testing::TestInfo* info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        tempDir_ = std::filesystem::temp_directory_path()
                   / "pdfe_pkcs7_roundtrip" / info->name();
        std::filesystem::create_directories(tempDir_);

        signedPath_ = tempDir_ / "signed.pdf";
        produceSignedDocument();
    }

    void TearDown() override {
        ctx_.reset();
    }

    /// Sign test_single.pdf and write the result to signedPath_ so each
    /// test can extract the signature bytes from a fresh signed PDF.
    void produceSignedDocument() {
        SignatureManager mgr(ctx_);
        auto doc = pdfe::core::Document::open(ctx_, fixture_);
        ASSERT_NE(doc, nullptr);
        auto page = doc->page(0);
        mgr.createSignatureField(page.get(), "PkcsField",
                                  QRectF(100, 500, 200, 50));
        doc->saveAs(signedPath_);

        auto reopen = pdfe::core::Document::open(ctx_, signedPath_);
        ASSERT_NE(reopen, nullptr);
        auto p = reopen->page(0);
        SigningOptions opts;
        opts.pfxPath = pfxPath_.string();
        opts.pfxPassword = "test123";
        opts.reason = "PKCS7 round-trip";
        mgr.signField(reopen.get(), p.get(), 0, opts);
    }

    /// Extract the raw signature contents (the hex-encoded PKCS#7
    /// SignedData blob between < and > in the /Contents entry of the
    /// signature dictionary). Returns the decoded DER bytes, or empty
    /// on failure.
    std::vector<uint8_t> extractSignatureDer() {
        std::ifstream f(signedPath_, std::ios::binary);
        if (!f) return {};
        std::string raw((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());

        // Find the /Contents <...> after a /Type /Sig marker. This is a
        // simple-but-reliable extraction for the test signer's output;
        // we don't try to handle the general case (which would need a
        // full PDF parser).
        size_t sigPos = raw.find("/Type /Sig");
        if (sigPos == std::string::npos) {
            sigPos = raw.find("/Type/Sig");
        }
        if (sigPos == std::string::npos) return {};
        size_t contentsPos = raw.find("/Contents", sigPos);
        if (contentsPos == std::string::npos) return {};
        size_t openAngle = raw.find('<', contentsPos);
        if (openAngle == std::string::npos) return {};
        size_t closeAngle = raw.find('>', openAngle + 1);
        if (closeAngle == std::string::npos) return {};

        // Hex-decode between < and >. PDF signatures use ASCII hex,
        // typically padded with zeros to a fixed length.
        std::vector<uint8_t> out;
        out.reserve((closeAngle - openAngle) / 2);
        auto hexNibble = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        for (size_t i = openAngle + 1; i + 1 < closeAngle; i += 2) {
            int hi = hexNibble(raw[i]);
            int lo = hexNibble(raw[i + 1]);
            if (hi < 0 || lo < 0) {
                // skip whitespace / padding
                while (i < closeAngle &&
                       (raw[i] == ' ' || raw[i] == '\n' || raw[i] == '\r')) {
                    ++i;
                }
                if (i + 1 >= closeAngle) break;
                hi = hexNibble(raw[i]);
                lo = hexNibble(raw[i + 1]);
                if (hi < 0 || lo < 0) break;
            }
            out.push_back(static_cast<uint8_t>((hi << 4) | lo));
        }

        // Trim trailing zero-padding (PDF pads /Contents to a fixed
        // size with 0x00). Find the last non-zero byte.
        while (!out.empty() && out.back() == 0x00) {
            out.pop_back();
        }
        return out;
    }

    std::shared_ptr<pdfe::core::MuPdfContext> ctx_;
    std::filesystem::path fixture_;
    std::filesystem::path pfxPath_;
    std::filesystem::path tempDir_;
    std::filesystem::path signedPath_;
};

// ─── Valid PKCS#7 with no signed content yields parseable result ────

TEST_F(Pkcs7RoundTripTest, ValidPkcs7WithoutContentPopulatesChain) {
    auto der = extractSignatureDer();
    ASSERT_GE(der.size(), 32u)
        << "Failed to extract a non-trivial PKCS#7 blob from the signed PDF";

    auto result = Pkcs7OpenSslBridge::verifyWithChain(
        der.data(), der.size(),
        nullptr, 0);
    // Without signed-content bytes, digestValid is expected to be
    // false (there's nothing to digest against). The contract this
    // test locks is: the cert chain is populated regardless.
    EXPECT_FALSE(result.certChain.empty())
        << "PKCS#7 verify must populate the cert chain even without "
           "signed-content bytes";
    EXPECT_FALSE(result.statusMessage.empty())
        << "PKCS#7 verify must produce a human-readable status message";
}

// ─── Empty input rejected cleanly ───────────────────────────────────

TEST_F(Pkcs7RoundTripTest, EmptyInputDoesNotCrashAndReportsInvalid) {
    auto result = Pkcs7OpenSslBridge::verifyWithChain(nullptr, 0,
                                                       nullptr, 0);
    EXPECT_FALSE(result.digestValid)
        << "Empty PKCS#7 must NOT report digestValid=true";
    EXPECT_FALSE(result.chainTrusted)
        << "Empty PKCS#7 must NOT report chainTrusted=true";
    EXPECT_TRUE(result.certChain.empty())
        << "Empty PKCS#7 must produce an empty cert chain";
}

// ─── Garbage input rejected cleanly ─────────────────────────────────

TEST_F(Pkcs7RoundTripTest, GarbageInputRejectedAsInvalid) {
    // A fixed-size buffer of zeros is structurally invalid PKCS#7
    // (no SEQUENCE tag, no OID). The verifier must reject it without
    // throwing.
    std::vector<uint8_t> garbage(128, 0x00);
    auto result = Pkcs7OpenSslBridge::verifyWithChain(
        garbage.data(), garbage.size(), nullptr, 0);
    EXPECT_FALSE(result.digestValid)
        << "Zero-filled buffer must NOT report digestValid=true";
}

// ─── Round-trip byte length is non-zero and bounded ─────────────────

TEST_F(Pkcs7RoundTripTest, ExtractedPkcs7HasReasonableByteLength) {
    auto der = extractSignatureDer();
    EXPECT_GE(der.size(), 100u)
        << "PKCS#7 SignedData must be at least 100 bytes (signer cert + "
           "signature + minimal overhead); got " << der.size();
    EXPECT_LE(der.size(), 100000u)
        << "PKCS#7 from a single-signer test cert must be well under "
           "100 KB; got " << der.size()
        << ". Larger sizes suggest a regression in PFX -> PKCS#7 "
           "encoding (e.g. duplicate cert chain).";
}

} // namespace
