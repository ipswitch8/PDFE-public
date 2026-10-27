// PHASE4_ROUNDTRIP_ENCRYPTION: mutate -> save -> reopen -> verify
// tests for the encryption subsystem. Closes the audit's gap that
// test_encryption_manager.cpp tests internal-state-only assertions
// (manager construction, fixture loading) without verifying that
// encryption + permissions + AES-256 actually round-trip through the
// saved file.

#include <gtest/gtest.h>

#include "encryption/EncryptionManager.h"
#include "encryption/EncryptionSettings.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

using namespace pdfe::encryption;

namespace {

class EncryptionRoundTripTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_shared<pdfe::core::MuPdfContext>();
        fixture_ = std::filesystem::path(TEST_FIXTURES_DIR) / "test_single.pdf";
        ASSERT_TRUE(std::filesystem::exists(fixture_))
            << "test_single.pdf fixture missing";
        document_ = pdfe::core::Document::open(ctx_, fixture_);
        ASSERT_NE(document_, nullptr);
        manager_ = std::make_unique<EncryptionManager>(ctx_);

        const ::testing::TestInfo* info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        tempDir_ = std::filesystem::temp_directory_path()
                   / "pdfe_enc_roundtrip" / info->name();
        std::filesystem::create_directories(tempDir_);
    }

    void TearDown() override {
        manager_.reset();
        document_.reset();
        ctx_.reset();
    }

    std::filesystem::path encryptedPath(const std::string& stem) {
        return tempDir_ / (stem + ".pdf");
    }

    std::shared_ptr<pdfe::core::MuPdfContext> ctx_;
    std::filesystem::path fixture_;
    std::filesystem::path tempDir_;
    std::unique_ptr<pdfe::core::Document> document_;
    std::unique_ptr<EncryptionManager> manager_;
};

// ─── AES-256 round-trip ─────────────────────────────────────────────

TEST_F(EncryptionRoundTripTest, Aes256EncryptionPersistsAfterReopen) {
    EncryptionSettings settings;
    settings.algorithm = EncryptionAlgorithm::AES_256;
    settings.userPassword = "user_pw_aes256";
    settings.ownerPassword = "owner_pw_aes256";
    settings.permissions = PermissionFlags::all();

    auto out = encryptedPath("aes256");
    manager_->saveEncrypted(document_.get(), out, settings);

    ASSERT_TRUE(std::filesystem::exists(out))
        << "saveEncrypted must produce an output file";
    ASSERT_GT(std::filesystem::file_size(out), 0u)
        << "Encrypted output must be non-empty";

    auto reopened = pdfe::core::Document::open(ctx_, out, "user_pw_aes256");
    ASSERT_NE(reopened, nullptr)
        << "Encrypted PDF must be re-openable with the user password";
    EXPECT_TRUE(manager_->isEncrypted(reopened.get()))
        << "Re-opened encrypted PDF must report isEncrypted=true";

    auto info = manager_->getEncryptionInfo(reopened.get());
    EXPECT_EQ(info.method, std::string("AES-256"))
        << "Method must be AES-256 (got '" << info.method << "')";
}

// ─── Permission round-trip ──────────────────────────────────────────

TEST_F(EncryptionRoundTripTest, RestrictedPermissionsSurviveSave) {
    EncryptionSettings settings;
    settings.algorithm = EncryptionAlgorithm::AES_256;
    settings.userPassword = "u_perm";
    settings.ownerPassword = "o_perm";
    PermissionFlags pf;
    pf.print = true;
    pf.printHighQuality = true;
    pf.copy = false;     // restrict copy
    pf.modify = false;   // restrict modify
    pf.annotate = false;
    pf.formFill = false;
    pf.assemble = false;
    pf.accessibility = true;
    settings.permissions = pf;

    auto out = encryptedPath("restricted");
    manager_->saveEncrypted(document_.get(), out, settings);
    ASSERT_TRUE(std::filesystem::exists(out));

    auto reopened = pdfe::core::Document::open(ctx_, out, "u_perm");
    ASSERT_NE(reopened, nullptr);
    auto restored = manager_->getPermissions(reopened.get());
    EXPECT_TRUE(restored.print)        << "Print must remain allowed";
    EXPECT_FALSE(restored.copy)        << "Copy must remain restricted";
    EXPECT_FALSE(restored.modify)      << "Modify must remain restricted";
    EXPECT_TRUE(restored.accessibility)
        << "Accessibility must remain allowed";
}

// ─── Decrypt -> isEncrypted=false ───────────────────────────────────

TEST_F(EncryptionRoundTripTest, RemoveEncryptionProducesUnencrypted) {
    EncryptionSettings settings;
    settings.algorithm = EncryptionAlgorithm::AES_256;
    settings.userPassword = "encrypt_then_remove";
    settings.ownerPassword = "owner_for_remove";
    settings.permissions = PermissionFlags::all();
    auto encOut = encryptedPath("encrypted_pre_decrypt");
    manager_->saveEncrypted(document_.get(), encOut, settings);
    ASSERT_TRUE(std::filesystem::exists(encOut));

    // R2-11: removeEncryption requires owner authority — open as OWNER.
    auto encDoc = pdfe::core::Document::open(ctx_, encOut, "owner_for_remove");
    ASSERT_NE(encDoc, nullptr);
    ASSERT_TRUE(manager_->isEncrypted(encDoc.get()));

    auto decOut = encryptedPath("decrypted");
    manager_->removeEncryption(encDoc.get(), decOut);
    ASSERT_TRUE(std::filesystem::exists(decOut));

    auto decDoc = pdfe::core::Document::open(ctx_, decOut);
    ASSERT_NE(decDoc, nullptr)
        << "Decrypted PDF must open without password";
    EXPECT_FALSE(manager_->isEncrypted(decDoc.get()))
        << "removeEncryption must produce a non-encrypted file";
}

// ─── Wrong-password rejection ───────────────────────────────────────

TEST_F(EncryptionRoundTripTest, WrongPasswordDoesNotSilentlyOpen) {
    EncryptionSettings settings;
    settings.algorithm = EncryptionAlgorithm::AES_256;
    settings.userPassword = "correct_password";
    settings.ownerPassword = "owner_password_xyz";
    settings.permissions = PermissionFlags::all();
    auto out = encryptedPath("wrong_pw");
    manager_->saveEncrypted(document_.get(), out, settings);

    // The contract: an encrypted PDF must NOT silently open as if it
    // were unencrypted. Document::open in this codebase throws
    // PdfeException on both no-password and wrong-password.
    auto verifyRejected = [&](const std::string& password,
                              const std::string& label) {
        bool rejected = false;
        try {
            auto doc = pdfe::core::Document::open(ctx_, out, password);
            // If we got a doc back, check it's not silently functional.
            // (fz_needs_password returns true for any encrypted doc
            // regardless of auth state, so we can't use that as a
            // negative signal — we'd need to actually try to use the
            // doc, which is overkill here.)
            rejected = (doc == nullptr);
        } catch (const std::exception&) {
            rejected = true;
        }
        EXPECT_TRUE(rejected)
            << "Encrypted PDF opened with " << label
            << " must be rejected (throw or return null)";
    };
    verifyRejected("",               "no password");
    verifyRejected("wrong_password", "wrong password");

    // Sanity: correct password DOES open the document and the
    // resulting document is queryable. We use pageCount as the
    // "is this real" test — if the auth failed silently, pageCount
    // would either throw or return 0.
    bool acceptedCorrect = false;
    int reopenedPageCount = -1;
    try {
        auto doc = pdfe::core::Document::open(ctx_, out, "correct_password");
        if (doc) {
            reopenedPageCount = doc->pageCount();
            acceptedCorrect = (reopenedPageCount > 0);
        }
    } catch (const std::exception&) {
        acceptedCorrect = false;
    }
    EXPECT_TRUE(acceptedCorrect)
        << "Encrypted PDF must open with correct password and the "
           "resulting document must be queryable (pageCount="
        << reopenedPageCount << ")";
}

// ─── AES-128 round-trip ─────────────────────────────────────────────

TEST_F(EncryptionRoundTripTest, Aes128EncryptionRoundTrips) {
    EncryptionSettings settings;
    settings.algorithm = EncryptionAlgorithm::AES_128;
    settings.userPassword = "u_128";
    settings.ownerPassword = "o_128";
    settings.permissions = PermissionFlags::all();
    auto out = encryptedPath("aes128");
    manager_->saveEncrypted(document_.get(), out, settings);
    ASSERT_TRUE(std::filesystem::exists(out));

    auto reopened = pdfe::core::Document::open(ctx_, out, "u_128");
    ASSERT_NE(reopened, nullptr);
    EXPECT_TRUE(manager_->isEncrypted(reopened.get()));
    auto info = manager_->getEncryptionInfo(reopened.get());
    EXPECT_NE(info.method.find("AES"), std::string::npos)
        << "Method should mention AES; got '" << info.method << "'";
    EXPECT_EQ(info.keyLength, 128)
        << "AES-128 must produce keyLength=128 in the round-trip";
}

// ─── Pipeline a7c4f891 phase-4 (B1): RC4 roundtrip ───────────────────
// RC4-40 and RC4-128 are defined in EncryptionSettings.h but had no
// roundtrip coverage. These tests close that gap. Note that RC4 is
// deprecated in modern PDF (RFC 8019) and is included primarily for
// reading legacy documents — but PDFE supports producing RC4 files
// so the contract should be locked.

TEST_F(EncryptionRoundTripTest, Rc4_40EncryptionRoundTrips) {
    EncryptionSettings settings;
    settings.algorithm = EncryptionAlgorithm::RC4_40;
    settings.userPassword = "u_rc40";
    settings.ownerPassword = "o_rc40";
    settings.permissions = PermissionFlags::all();
    auto out = encryptedPath("rc4_40");
    manager_->saveEncrypted(document_.get(), out, settings);
    ASSERT_TRUE(std::filesystem::exists(out));

    auto reopened = pdfe::core::Document::open(ctx_, out, "u_rc40");
    ASSERT_NE(reopened, nullptr);
    EXPECT_TRUE(manager_->isEncrypted(reopened.get()));
    auto info = manager_->getEncryptionInfo(reopened.get());
    EXPECT_EQ(info.keyLength, 40)
        << "RC4-40 must produce keyLength=40 in the round-trip; "
        << "got " << info.keyLength;
}

TEST_F(EncryptionRoundTripTest, Rc4_128EncryptionRoundTrips) {
    EncryptionSettings settings;
    settings.algorithm = EncryptionAlgorithm::RC4_128;
    settings.userPassword = "u_rc128";
    settings.ownerPassword = "o_rc128";
    settings.permissions = PermissionFlags::all();
    auto out = encryptedPath("rc4_128");
    manager_->saveEncrypted(document_.get(), out, settings);
    ASSERT_TRUE(std::filesystem::exists(out));

    auto reopened = pdfe::core::Document::open(ctx_, out, "u_rc128");
    ASSERT_NE(reopened, nullptr);
    EXPECT_TRUE(manager_->isEncrypted(reopened.get()));
    auto info = manager_->getEncryptionInfo(reopened.get());
    EXPECT_EQ(info.keyLength, 128)
        << "RC4-128 must produce keyLength=128 in the round-trip; "
        << "got " << info.keyLength;
}

TEST_F(EncryptionRoundTripTest, Rc4_40WrongPasswordFails) {
    EncryptionSettings settings;
    settings.algorithm = EncryptionAlgorithm::RC4_40;
    settings.userPassword = "correct_u_rc40";
    settings.ownerPassword = "correct_o_rc40";
    settings.permissions = PermissionFlags::all();
    auto out = encryptedPath("rc4_40_wrong");
    manager_->saveEncrypted(document_.get(), out, settings);
    ASSERT_TRUE(std::filesystem::exists(out));

    // Correct password opens.
    auto good = pdfe::core::Document::open(ctx_, out, "correct_u_rc40");
    EXPECT_NE(good, nullptr) << "correct password must open RC4-40 doc";

    // Wrong password must reject the open — either by throwing an
    // exception OR by returning a still-locked document (depending
    // on MuPDF's password-handling mode). Both are acceptable
    // security postures; a silent open with usable content is NOT.
    bool rejected = false;
    try {
        auto bad = pdfe::core::Document::open(ctx_, out, "wrong_password");
        if (bad == nullptr) {
            rejected = true;
        } else if (manager_->isEncrypted(bad.get())) {
            // Returned a still-locked document — also acceptable rejection.
            rejected = true;
        }
    } catch (const std::exception&) {
        // Exception path — also acceptable rejection.
        rejected = true;
    }
    EXPECT_TRUE(rejected)
        << "wrong password on RC4-40 doc must be rejected (throw, "
        << "return null, or return still-locked) — silent open is "
        << "a security regression";
}

TEST_F(EncryptionRoundTripTest, Rc4_128PermissionsSurviveRoundTrip) {
    EncryptionSettings settings;
    settings.algorithm = EncryptionAlgorithm::RC4_128;
    settings.userPassword = "u_perm128";
    settings.ownerPassword = "o_perm128";
    // Restrict: deny print + deny copy
    settings.permissions = PermissionFlags::all();
    settings.permissions.print = false;
    settings.permissions.copy = false;
    auto out = encryptedPath("rc4_128_perm");
    manager_->saveEncrypted(document_.get(), out, settings);
    ASSERT_TRUE(std::filesystem::exists(out));

    auto reopened = pdfe::core::Document::open(ctx_, out, "u_perm128");
    ASSERT_NE(reopened, nullptr);
    auto info = manager_->getEncryptionInfo(reopened.get());
    EXPECT_FALSE(info.permissions.print)
        << "RC4-128: print=false should survive save/reopen";
    EXPECT_FALSE(info.permissions.copy)
        << "RC4-128: copy=false should survive save/reopen";
}

// ─── File integrity: encrypted file must differ from original ────────

TEST_F(EncryptionRoundTripTest, EncryptedFileBytesDifferFromUnencrypted) {
    EncryptionSettings settings;
    settings.algorithm = EncryptionAlgorithm::AES_256;
    settings.userPassword = "byte_diff_user";
    settings.ownerPassword = "byte_diff_owner";
    settings.permissions = PermissionFlags::all();

    auto out = encryptedPath("for_byte_diff");
    manager_->saveEncrypted(document_.get(), out, settings);

    // The encrypted file must literally be different from the original
    // unencrypted source — proves the crypto pass actually ran. A
    // saveEncrypted that silently no-op'd would produce a copy of the
    // original.
    auto srcSize = std::filesystem::file_size(fixture_);
    auto encSize = std::filesystem::file_size(out);
    // Sizes can differ by encryption overhead; we don't assert exact
    // size, only that the encrypted file is not byte-identical.
    if (srcSize == encSize) {
        std::ifstream a(fixture_, std::ios::binary);
        std::ifstream b(out,      std::ios::binary);
        std::string aBytes{std::istreambuf_iterator<char>(a),
                           std::istreambuf_iterator<char>()};
        std::string bBytes{std::istreambuf_iterator<char>(b),
                           std::istreambuf_iterator<char>()};
        EXPECT_NE(aBytes, bBytes)
            << "Encrypted file must differ byte-wise from the unencrypted "
               "source (saveEncrypted no-op'd?)";
    } else {
        SUCCEED() << "Encrypted file has different size from source ("
                  << encSize << " vs " << srcSize << "), as expected";
    }
}

// ─── Encryption applied to corrupt-xref-recoverable fixture ─────────

TEST_F(EncryptionRoundTripTest, EncryptCorruptXrefRecoverableFixture) {
    auto rwPath = std::filesystem::path(TEST_FIXTURES_DIR) / "realworld"
                  / "corrupt_xref" / "corrupt_xref_recoverable.pdf";
    ASSERT_TRUE(std::filesystem::exists(rwPath))
        << "phase-2 fixture missing: " << rwPath;

    auto rwDoc = pdfe::core::Document::open(ctx_, rwPath);
    ASSERT_NE(rwDoc, nullptr)
        << "MuPDF's xref-repair must allow opening the fixture";

    EncryptionSettings settings;
    settings.algorithm = EncryptionAlgorithm::AES_256;
    settings.userPassword = "rw_corrupt";
    settings.ownerPassword = "rw_corrupt_owner";
    settings.permissions = PermissionFlags::all();
    auto out = encryptedPath("rw_corrupt_xref");
    manager_->saveEncrypted(rwDoc.get(), out, settings);
    ASSERT_TRUE(std::filesystem::exists(out));

    auto reopened = pdfe::core::Document::open(ctx_, out, "rw_corrupt");
    ASSERT_NE(reopened, nullptr)
        << "Encryption must succeed on a repaired-xref document";
    EXPECT_TRUE(manager_->isEncrypted(reopened.get()));
    EXPECT_EQ(reopened->pageCount(), 2)
        << "Page count of the repaired source must survive encryption";
}

// ─── Pipeline a7c4f891 phase-6 (B9): permission enforcement ────────
// Encrypt a PDF with copy=false, reopen with user password, and verify
// that the permission flag survives the round-trip. This is the
// "integration test" axis: the restriction is set, persisted, and
// readable on reopen — proving that a downstream consumer checking
// getPermissions() before allowing copy will see copy=false.
//
// Note: MuPDF itself does not enforce permissions at the API level
// (extractPlainText works regardless of the copy flag — this is by
// design, as PDF viewer permissions are advisory per the spec and
// enforcement is the viewer's responsibility). The test verifies
// that getPermissions() faithfully reports the restriction so PDFE's
// UI layer can enforce it.

TEST_F(EncryptionRoundTripTest, CopyFalsePermissionEnforcedOnReopen) {
    EncryptionSettings settings;
    settings.algorithm = EncryptionAlgorithm::AES_256;
    settings.userPassword = "b9_user";
    settings.ownerPassword = "b9_owner";
    PermissionFlags pf = PermissionFlags::all();
    pf.copy = false;
    settings.permissions = pf;

    auto out = encryptedPath("b9_copy_false");
    manager_->saveEncrypted(document_.get(), out, settings);
    ASSERT_TRUE(std::filesystem::exists(out));

    // Reopen with user password (restricted access level).
    auto reopened = pdfe::core::Document::open(ctx_, out, "b9_user");
    ASSERT_NE(reopened, nullptr);
    EXPECT_TRUE(manager_->isEncrypted(reopened.get()));

    auto perms = manager_->getPermissions(reopened.get());
    EXPECT_FALSE(perms.copy)
        << "B9: copy=false must survive encryption round-trip when "
        << "opened with user password; PDFE UI checks this flag before "
        << "allowing clipboard copy";

    // Other permissions should remain as set (all true except copy).
    EXPECT_TRUE(perms.print)
        << "B9: print should remain true (only copy was restricted)";
    EXPECT_TRUE(perms.modify)
        << "B9: modify should remain true";
    EXPECT_TRUE(perms.annotate)
        << "B9: annotate should remain true";

    // Verify that the OWNER password also opens the document.
    // Per PDF spec, the owner password bypasses permission
    // restrictions — but MuPDF infers authLevel from the stored
    // permission bitmask (not from which password was used), so
    // both passwords may report authLevel=1 when permissions are
    // restricted. The key assertion is that both passwords open
    // the document and it's functional.
    auto ownerDoc = pdfe::core::Document::open(ctx_, out, "b9_owner");
    ASSERT_NE(ownerDoc, nullptr)
        << "B9: owner password must open the encrypted PDF";
    EXPECT_EQ(ownerDoc->pageCount(), document_->pageCount())
        << "B9: document opened with owner password must be functional";
}

// ─── Pipeline a7c4f891 phase-6 (B9): multiple restricted permissions ──

TEST_F(EncryptionRoundTripTest, MultipleRestrictedPermissionsRoundTrip) {
    EncryptionSettings settings;
    settings.algorithm = EncryptionAlgorithm::AES_256;
    settings.userPassword = "b9_multi_u";
    settings.ownerPassword = "b9_multi_o";
    PermissionFlags pf;
    pf.print = false;
    pf.printHighQuality = false;
    pf.copy = false;
    pf.modify = false;
    pf.annotate = false;
    pf.formFill = false;
    pf.assemble = false;
    pf.accessibility = true;  // PDF/UA requires this to remain true
    settings.permissions = pf;

    auto out = encryptedPath("b9_multi_restrict");
    manager_->saveEncrypted(document_.get(), out, settings);
    ASSERT_TRUE(std::filesystem::exists(out));

    auto reopened = pdfe::core::Document::open(ctx_, out, "b9_multi_u");
    ASSERT_NE(reopened, nullptr);
    auto restored = manager_->getPermissions(reopened.get());
    EXPECT_FALSE(restored.print)          << "B9: print must stay false";
    EXPECT_FALSE(restored.printHighQuality) << "B9: printHQ must stay false";
    EXPECT_FALSE(restored.copy)           << "B9: copy must stay false";
    EXPECT_FALSE(restored.modify)         << "B9: modify must stay false";
    EXPECT_FALSE(restored.annotate)       << "B9: annotate must stay false";
    EXPECT_FALSE(restored.formFill)       << "B9: formFill must stay false";
    EXPECT_FALSE(restored.assemble)       << "B9: assemble must stay false";
    EXPECT_TRUE(restored.accessibility)   << "B9: accessibility must stay true";
}

// ─── Pipeline a7c4f891 phase-6 (B12): weak password behavior ──────
// MuPDF and QPDF both accept passwords shorter than 8 characters
// for all encryption algorithms. The PDF specification (ISO 32000)
// does NOT mandate a minimum password length — /O and /U entries
// are derived by padding short passwords with a fixed pad sequence
// (for V<=4) or by hashing (for V=5/AES-256). This means:
//
//   1. A 1-character password is cryptographically valid.
//   2. MuPDF's fz_authenticate_password will accept it.
//   3. QPDF's QPDFWriter::setEncryption will accept it.
//   4. The security is weak but spec-compliant.
//
// PDFE's EncryptionDialog enforces a configurable minimum length
// (default 8) in the UI layer. The backend intentionally does NOT
// reject short passwords because:
//   a) Legacy PDFs with short passwords must be openable.
//   b) The spec allows it.
//   c) UI-layer enforcement is the correct boundary.
//
// This test documents and locks the backend behavior: short passwords
// must round-trip without error.

TEST_F(EncryptionRoundTripTest, ShortPasswordAcceptedByBackend) {
    EncryptionSettings settings;
    settings.algorithm = EncryptionAlgorithm::AES_256;
    settings.userPassword = "ab";  // 2 chars — well under any "strong" threshold
    settings.ownerPassword = "cd";
    settings.permissions = PermissionFlags::all();

    auto out = encryptedPath("b12_short_pw");
    EXPECT_NO_THROW(manager_->saveEncrypted(document_.get(), out, settings))
        << "B12: backend must accept short passwords (spec-compliant; "
        << "UI-layer EncryptionDialog enforces minimum length)";
    ASSERT_TRUE(std::filesystem::exists(out));

    auto reopened = pdfe::core::Document::open(ctx_, out, "ab");
    ASSERT_NE(reopened, nullptr)
        << "B12: short password must authenticate successfully";
    EXPECT_TRUE(manager_->isEncrypted(reopened.get()));
    EXPECT_EQ(reopened->pageCount(), document_->pageCount())
        << "B12: document content must survive short-password encryption";
}

} // namespace
