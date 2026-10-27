// PHASE1_HARDENING: skip=2 expect_ge=0 no_throw=0  (2026-05-08)
#include <gtest/gtest.h>
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/Error.h"
#include "encryption/EncryptionManager.h"
#include "encryption/EncryptionSettings.h"
#include <filesystem>
#include <memory>

using namespace pdfe::encryption;

// ============================================================================
// Test fixture for unencrypted PDF tests
// ============================================================================

class EncryptionManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<pdfe::core::MuPdfContext>();

        auto pdfPath = std::filesystem::path(TEST_FIXTURES_DIR) / "test_single.pdf";
        ASSERT_TRUE(std::filesystem::exists(pdfPath))
            << "test_single.pdf fixture missing at " << pdfPath;
        document_ = pdfe::core::Document::open(context_, pdfPath);
        ASSERT_NE(document_, nullptr);

        manager_ = std::make_unique<EncryptionManager>(context_);
    }

    std::shared_ptr<pdfe::core::MuPdfContext> context_;
    std::unique_ptr<pdfe::core::Document> document_;
    std::unique_ptr<EncryptionManager> manager_;
};

// ============================================================================
// Test fixture for encrypted PDF tests
// ============================================================================

class EncryptedDocumentTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<pdfe::core::MuPdfContext>();
        manager_ = std::make_unique<EncryptionManager>(context_);

        aes256Path_ = std::filesystem::path(TEST_FIXTURES_DIR) / "test_encrypted_aes256.pdf";
        restrictedPath_ = std::filesystem::path(TEST_FIXTURES_DIR) / "test_encrypted_restricted.pdf";
        ownerOnlyPath_ = std::filesystem::path(TEST_FIXTURES_DIR) / "test_encrypted_owner_only.pdf";

        ASSERT_TRUE(std::filesystem::exists(aes256Path_))
            << "Encrypted fixture missing: " << aes256Path_;
    }

    std::shared_ptr<pdfe::core::MuPdfContext> context_;
    std::unique_ptr<EncryptionManager> manager_;
    std::filesystem::path aes256Path_;
    std::filesystem::path restrictedPath_;
    std::filesystem::path ownerOnlyPath_;
};

// ============================================================================
// Unencrypted document tests
// ============================================================================

TEST_F(EncryptionManagerTest, ConstructDestroy) {
    auto mgr = std::make_unique<EncryptionManager>(context_);
    EXPECT_NE(mgr, nullptr);
}

TEST_F(EncryptionManagerTest, UnencryptedPdf_IsNotEncrypted) {
    EXPECT_FALSE(manager_->isEncrypted(document_.get()));
}

TEST_F(EncryptionManagerTest, UnencryptedPdf_DoesNotNeedPassword) {
    EXPECT_FALSE(manager_->needsPassword(document_.get()));
}

TEST_F(EncryptionManagerTest, UnencryptedPdf_FullPermissions) {
    auto perms = manager_->getPermissions(document_.get());
    // Unencrypted docs should have all permissions
    EXPECT_TRUE(perms.print);
    EXPECT_TRUE(perms.modify);
    EXPECT_TRUE(perms.copy);
    EXPECT_TRUE(perms.annotate);
}

TEST_F(EncryptionManagerTest, UnencryptedPdf_EncryptionInfo) {
    auto info = manager_->getEncryptionInfo(document_.get());
    EXPECT_FALSE(info.isEncrypted);
}

TEST_F(EncryptionManagerTest, NullDocument_Throws) {
    EXPECT_THROW(manager_->isEncrypted(nullptr), std::invalid_argument);
    EXPECT_THROW(manager_->needsPassword(nullptr), std::invalid_argument);
    EXPECT_THROW(manager_->getEncryptionInfo(nullptr), std::invalid_argument);
    EXPECT_THROW(manager_->getPermissions(nullptr), std::invalid_argument);
    EXPECT_THROW(manager_->authenticatePassword(nullptr, "test"), std::invalid_argument);
}

TEST_F(EncryptionManagerTest, SaveEncrypted_NullDocument_Throws) {
    EncryptionSettings settings;
    settings.algorithm = EncryptionAlgorithm::AES_256;
    settings.ownerPassword = "owner";
    auto outPath = std::filesystem::temp_directory_path() / "test_enc_null.pdf";
    EXPECT_THROW(manager_->saveEncrypted(nullptr, outPath, settings), std::invalid_argument);
}

TEST_F(EncryptionManagerTest, SaveEncrypted_NoAlgorithm_Throws) {
    EncryptionSettings settings; // algorithm = None
    auto outPath = std::filesystem::temp_directory_path() / "test_enc_noalg.pdf";
    EXPECT_THROW(manager_->saveEncrypted(document_.get(), outPath, settings), std::invalid_argument);
}

TEST_F(EncryptionManagerTest, SaveEncrypted_NoOwnerPassword_Throws) {
    EncryptionSettings settings;
    settings.algorithm = EncryptionAlgorithm::AES_256;
    // ownerPassword is empty
    auto outPath = std::filesystem::temp_directory_path() / "test_enc_nopwd.pdf";
    EXPECT_THROW(manager_->saveEncrypted(document_.get(), outPath, settings), std::invalid_argument);
}

TEST_F(EncryptionManagerTest, RemoveEncryption_NullDocument_Throws) {
    auto outPath = std::filesystem::temp_directory_path() / "test_dec_null.pdf";
    EXPECT_THROW(manager_->removeEncryption(nullptr, outPath), std::invalid_argument);
}

// ============================================================================
// Save + Roundtrip tests
// ============================================================================

TEST_F(EncryptionManagerTest, SaveEncrypted_Roundtrip) {
    auto outPath = std::filesystem::temp_directory_path() / "test_enc_roundtrip.pdf";

    // Save with encryption
    EncryptionSettings settings;
    settings.algorithm = EncryptionAlgorithm::AES_256;
    settings.userPassword = "user123";
    settings.ownerPassword = "owner456";
    settings.permissions = PermissionFlags::all();

    manager_->saveEncrypted(document_.get(), outPath, settings);
    ASSERT_TRUE(std::filesystem::exists(outPath));

    // Reopen with user password and verify encrypted
    auto encDoc = pdfe::core::Document::open(context_, outPath, "user123");
    ASSERT_NE(encDoc, nullptr);

    EXPECT_TRUE(manager_->isEncrypted(encDoc.get()));

    // Get encryption info
    auto info = manager_->getEncryptionInfo(encDoc.get());
    EXPECT_TRUE(info.isEncrypted);

    encDoc.reset();
    std::filesystem::remove(outPath);
}

TEST_F(EncryptionManagerTest, SaveEncrypted_RestrictedPermissions) {
    auto outPath = std::filesystem::temp_directory_path() / "test_enc_restricted.pdf";

    EncryptionSettings settings;
    settings.algorithm = EncryptionAlgorithm::AES_256;
    settings.userPassword = "user123";
    settings.ownerPassword = "owner456";
    settings.permissions.print = false;
    settings.permissions.copy = false;
    settings.permissions.modify = false;

    manager_->saveEncrypted(document_.get(), outPath, settings);
    ASSERT_TRUE(std::filesystem::exists(outPath));

    // Reopen with user password
    auto encDoc = pdfe::core::Document::open(context_, outPath, "user123");
    ASSERT_NE(encDoc, nullptr);

    // Verify restricted permissions
    auto perms = manager_->getPermissions(encDoc.get());
    EXPECT_FALSE(perms.print);
    EXPECT_FALSE(perms.copy);
    EXPECT_FALSE(perms.modify);
    // These should still be allowed
    EXPECT_TRUE(perms.annotate);
    EXPECT_TRUE(perms.formFill);
    EXPECT_TRUE(perms.accessibility);

    encDoc.reset();
    std::filesystem::remove(outPath);
}

TEST_F(EncryptionManagerTest, SaveEncrypted_OwnerPasswordOnly) {
    auto outPath = std::filesystem::temp_directory_path() / "test_enc_owneronly.pdf";

    EncryptionSettings settings;
    settings.algorithm = EncryptionAlgorithm::AES_256;
    // No user password
    settings.ownerPassword = "owner456";
    settings.permissions = PermissionFlags::all();

    manager_->saveEncrypted(document_.get(), outPath, settings);
    ASSERT_TRUE(std::filesystem::exists(outPath));

    // Should open without password (empty user password)
    auto encDoc = pdfe::core::Document::open(context_, outPath);
    ASSERT_NE(encDoc, nullptr);

    EXPECT_TRUE(manager_->isEncrypted(encDoc.get()));

    encDoc.reset();
    std::filesystem::remove(outPath);
}

TEST_F(EncryptionManagerTest, RemoveEncryption_Roundtrip) {
    // First, create an encrypted file
    auto encPath = std::filesystem::temp_directory_path() / "test_to_decrypt.pdf";
    auto decPath = std::filesystem::temp_directory_path() / "test_decrypted.pdf";

    EncryptionSettings settings;
    settings.algorithm = EncryptionAlgorithm::AES_256;
    settings.userPassword = "user123";
    settings.ownerPassword = "owner456";

    manager_->saveEncrypted(document_.get(), encPath, settings);
    ASSERT_TRUE(std::filesystem::exists(encPath));

    // Open encrypted as OWNER, then remove encryption. R2-11: stripping
    // encryption requires owner authority, so this roundtrip authenticates with
    // the owner password (the user-password case is covered as a refusal in
    // test_permission_enforcement.cpp).
    auto encDoc = pdfe::core::Document::open(context_, encPath, "owner456");
    ASSERT_NE(encDoc, nullptr);

    manager_->removeEncryption(encDoc.get(), decPath);
    ASSERT_TRUE(std::filesystem::exists(decPath));

    // Verify decrypted file opens without password
    auto decDoc = pdfe::core::Document::open(context_, decPath);
    ASSERT_NE(decDoc, nullptr);
    EXPECT_FALSE(manager_->isEncrypted(decDoc.get()));

    encDoc.reset();
    decDoc.reset();
    std::filesystem::remove(encPath);
    std::filesystem::remove(decPath);
}

// ============================================================================
// Encrypted fixture tests
// ============================================================================

TEST_F(EncryptedDocumentTest, OpenWithUserPassword) {
    auto doc = pdfe::core::Document::open(context_, aes256Path_, "test123");
    ASSERT_NE(doc, nullptr);
    EXPECT_TRUE(manager_->isEncrypted(doc.get()));
}

TEST_F(EncryptedDocumentTest, OpenWithOwnerPassword) {
    auto doc = pdfe::core::Document::open(context_, aes256Path_, "owner456");
    ASSERT_NE(doc, nullptr);
    EXPECT_TRUE(manager_->isEncrypted(doc.get()));
}

TEST_F(EncryptedDocumentTest, OpenWithWrongPassword_Throws) {
    EXPECT_THROW(
        pdfe::core::Document::open(context_, aes256Path_, "wrongpassword"),
        pdfe::core::PdfeException
    );
}

TEST_F(EncryptedDocumentTest, OwnerOnlyPdf_OpensWithoutPassword) {
    auto doc = pdfe::core::Document::open(context_, ownerOnlyPath_);
    ASSERT_NE(doc, nullptr);
    EXPECT_TRUE(manager_->isEncrypted(doc.get()));
}

TEST_F(EncryptedDocumentTest, EncryptionInfo_AES256) {
    auto doc = pdfe::core::Document::open(context_, aes256Path_, "test123");
    ASSERT_NE(doc, nullptr);

    auto info = manager_->getEncryptionInfo(doc.get());
    EXPECT_TRUE(info.isEncrypted);
    EXPECT_EQ(info.version, 5);
    EXPECT_EQ(info.revision, 6);
    EXPECT_EQ(info.method, "AES-256");
}

TEST_F(EncryptedDocumentTest, RestrictedPermissions) {
    auto doc = pdfe::core::Document::open(context_, restrictedPath_, "test123");
    ASSERT_NE(doc, nullptr);

    auto perms = manager_->getPermissions(doc.get());
    EXPECT_FALSE(perms.print);
    EXPECT_FALSE(perms.copy);
    EXPECT_FALSE(perms.modify);
}

TEST_F(EncryptedDocumentTest, AllPermissions_AES256) {
    auto doc = pdfe::core::Document::open(context_, aes256Path_, "test123");
    ASSERT_NE(doc, nullptr);

    auto perms = manager_->getPermissions(doc.get());
    EXPECT_TRUE(perms.print);
    EXPECT_TRUE(perms.copy);
    EXPECT_TRUE(perms.annotate);
}

TEST_F(EncryptedDocumentTest, AuthenticatePassword_UserCorrect) {
    auto doc = pdfe::core::Document::open(context_, aes256Path_, "test123");
    ASSERT_NE(doc, nullptr);

    // MuPDF has already authenticated with user pwd; re-authenticating may return cached state
    // The key test is that the method doesn't throw
    int level = manager_->authenticatePassword(doc.get(), "test123");
    EXPECT_GT(level, 0); // Should succeed
}

TEST_F(EncryptedDocumentTest, AuthenticatePassword_Wrong) {
    auto doc = pdfe::core::Document::open(context_, aes256Path_, "test123");
    ASSERT_NE(doc, nullptr);

    int level = manager_->authenticatePassword(doc.get(), "wrongpassword");
    EXPECT_EQ(level, 0); // Should fail
}
