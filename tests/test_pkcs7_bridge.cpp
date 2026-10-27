/**
 * @file test_pkcs7_bridge.cpp
 * @brief Unit tests for Pkcs7OpenSslBridge.
 */

#include <gtest/gtest.h>
#include "signatures/Pkcs7OpenSslBridge.h"
#include "core/MuPdfContext.h"

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

#include <filesystem>

using namespace pdfe::signatures;

static std::string fixturesDir() {
    return TEST_FIXTURES_DIR;
}

class Pkcs7BridgeTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<pdfe::core::MuPdfContext>();
    }

    void TearDown() override {
        context_.reset();
    }

    std::shared_ptr<pdfe::core::MuPdfContext> context_;
};

// Test 1: Construct and destroy
TEST_F(Pkcs7BridgeTest, ConstructDestroy) {
    Pkcs7OpenSslBridge bridge;
    SUCCEED();
}

// Test 2: isAvailable
TEST_F(Pkcs7BridgeTest, IsAvailable) {
    Pkcs7OpenSslBridge bridge;
    EXPECT_TRUE(bridge.isAvailable());
}

// Test 3: Read valid PFX
TEST_F(Pkcs7BridgeTest, ReadPfx_ValidFile) {
    std::string pfxPath = fixturesDir() + "/test_signer.pfx";
    if (!std::filesystem::exists(pfxPath)) {
        GTEST_SKIP() << "test_signer.pfx not available";
    }

    Pkcs7OpenSslBridge bridge;

    context_->execute([&](fz_context* ctx) {
        pdf_pkcs7_signer* signer = bridge.readPfx(ctx, pfxPath.c_str(), "test123");
        ASSERT_NE(signer, nullptr);

        // Check vtable pointers
        EXPECT_NE(signer->keep, nullptr);
        EXPECT_NE(signer->drop, nullptr);
        EXPECT_NE(signer->get_signing_name, nullptr);
        EXPECT_NE(signer->max_digest_size, nullptr);
        EXPECT_NE(signer->create_digest, nullptr);

        // Check max digest size is reasonable
        size_t maxSize = signer->max_digest_size(ctx, signer);
        EXPECT_GT(maxSize, 0u);
        EXPECT_LE(maxSize, 16384u);

        // Check signing name
        pdf_pkcs7_distinguished_name* dn = signer->get_signing_name(ctx, signer);
        ASSERT_NE(dn, nullptr);
        EXPECT_NE(dn->cn, nullptr);
        EXPECT_STREQ(dn->cn, "PDFE Test Signer");
        EXPECT_STREQ(dn->o, "PDFE Test");
        EXPECT_STREQ(dn->ou, "QA");
        EXPECT_STREQ(dn->c, "US");
        pdf_signature_drop_distinguished_name(ctx, dn);

        Pkcs7OpenSslBridge::dropSigner(ctx, signer);
    });
}

// Test 4: Read PFX with wrong password
TEST_F(Pkcs7BridgeTest, ReadPfx_WrongPassword) {
    std::string pfxPath = fixturesDir() + "/test_signer.pfx";
    if (!std::filesystem::exists(pfxPath)) {
        GTEST_SKIP() << "test_signer.pfx not available";
    }

    Pkcs7OpenSslBridge bridge;

    EXPECT_ANY_THROW({
        context_->execute([&](fz_context* ctx) {
            bridge.readPfx(ctx, pfxPath.c_str(), "wrongpassword");
        });
    });
}

// Test 5: Read PFX with nonexistent file
TEST_F(Pkcs7BridgeTest, ReadPfx_NonexistentFile) {
    Pkcs7OpenSslBridge bridge;

    EXPECT_ANY_THROW({
        context_->execute([&](fz_context* ctx) {
            bridge.readPfx(ctx, "/nonexistent/path.pfx", "test123");
        });
    });
}

// Test 6: Create verifier
TEST_F(Pkcs7BridgeTest, CreateVerifier) {
    Pkcs7OpenSslBridge bridge;

    context_->execute([&](fz_context* ctx) {
        pdf_pkcs7_verifier* verifier = bridge.createVerifier(ctx);
        ASSERT_NE(verifier, nullptr);

        // Check vtable pointers
        EXPECT_NE(verifier->drop, nullptr);
        EXPECT_NE(verifier->check_certificate, nullptr);
        EXPECT_NE(verifier->check_digest, nullptr);
        EXPECT_NE(verifier->get_signatory, nullptr);

        Pkcs7OpenSslBridge::dropVerifier(ctx, verifier);
    });
}

// Test 7: Drop null signer (should not crash)
TEST_F(Pkcs7BridgeTest, DropNullSigner) {
    context_->execute([](fz_context* ctx) {
        Pkcs7OpenSslBridge::dropSigner(ctx, nullptr);
    });
    SUCCEED();
}

// Test 8: Drop null verifier (should not crash)
TEST_F(Pkcs7BridgeTest, DropNullVerifier) {
    context_->execute([](fz_context* ctx) {
        Pkcs7OpenSslBridge::dropVerifier(ctx, nullptr);
    });
    SUCCEED();
}
