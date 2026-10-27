// -*- coding: utf-8 -*-
//
// Meta-audit 2026-07 follow-up, phase F24 — items 3 and 4, both raised by the
// F3 `security-audit` gate and both about a report that is WRONG or in the
// WRONG PLACE rather than about a missing check.
//
// ITEM 3 — `/V 4 /R 4` was reported as AES-128 unconditionally.
//   /V 4 is the version that decoupled key derivation from cipher: /R says how
//   the key is made, the /CF entry named by /StmF says what drives it.
//   `/CFM /V2` is RC4-128 and is legal at /V 4 /R 4. Calling it AES-128
//   OVERSTATES the protection — the same mis-label class F3 closed for /V 1,
//   in the direction a user acts on. Fixed at
//   src/encryption/EncryptionManager.cpp (getMethodString now takes the crypt
//   filter's method). Fixtures: tests/fixtures/flaw_corpus/encryption/
//   v4r4_cfm_v2.pdf and v4r4_cfm_aesv2.pdf, from gen_v4r4_cfm_corpus.py —
//   identical but for /CFM, so a reader that ignores /CFM cannot tell them
//   apart and cannot pass both assertions.
//
// ITEM 4 — the empty-user-password warning was surfaced where nobody looks.
//   It appeared only in the Document Permissions dialog, which nothing prompts
//   the user to open. The gate's judgement, which this phase accepts: a
//   warning nobody sees is not an effective control. The moment that matters is
//   when the user SETS an empty user password alongside a non-empty owner
//   password, while the intent can still be corrected. Surfaced now at
//   src/ui/EncryptionDialog.cpp (a live warning label) and in
//   EncryptionSaveReport for every non-dialog caller.
//
// Every assertion here has a CONTROL that must come out the other way, so none
// of them can be satisfied by a blanket answer.

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "encryption/EncryptionManager.h"
#include "encryption/EncryptionSettings.h"
#include "ui/EncryptionDialog.h"

#include <QApplication>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>

#include <filesystem>
#include <memory>
#include <string>

using namespace pdfe::encryption;
using pdfe::core::Document;
using pdfe::core::MuPdfContext;

namespace {

std::filesystem::path corpusFixture(const char* rel) {
    return std::filesystem::path(TEST_FIXTURES_DIR) / "flaw_corpus"
           / "encryption" / rel;
}

bool contains(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

} // namespace

// ===========================================================================
// ITEM 3 — /V 4 /R 4 method string must follow the crypt filter.
// ===========================================================================

class V4R4CryptFilterMethodTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_shared<MuPdfContext>();
        manager_ = std::make_unique<EncryptionManager>(ctx_);
    }

    EncryptionInfo infoFor(const char* fixture) {
        const auto path = corpusFixture(fixture);
        EXPECT_TRUE(std::filesystem::exists(path))
            << "missing fixture " << path.string()
            << " (regenerate with tests/fixtures/gen_v4r4_cfm_corpus.py)";
        // Both fixtures use the EMPTY user password, so they open with no
        // prompt — which is also why a user would never be asked anything that
        // might reveal the real cipher.
        doc_ = Document::open(ctx_, path);
        EXPECT_NE(doc_, nullptr);
        return manager_->getEncryptionInfo(doc_.get());
    }

    std::shared_ptr<MuPdfContext> ctx_;
    std::unique_ptr<EncryptionManager> manager_;
    std::unique_ptr<Document> doc_;
};

TEST_F(V4R4CryptFilterMethodTest, CfmV2IsReportedAsRc4128NotAes128) {
    const auto info = infoFor("v4r4_cfm_v2.pdf");

    // Preconditions: this really is the /V 4 /R 4 shape the defect is about.
    ASSERT_TRUE(info.isEncrypted);
    ASSERT_EQ(info.version, 4);
    ASSERT_EQ(info.revision, 4);
    ASSERT_EQ(info.streamCryptMethod, std::string("RC4"))
        << "precondition: the fixture's /StmF crypt filter must be RC4";

    EXPECT_EQ(info.method, std::string("RC4-128"))
        << "a /V 4 /R 4 document whose /CFM is /V2 is RC4-128";
    EXPECT_NE(info.method, std::string("AES-128"))
        << "reporting AES-128 here OVERSTATES the protection — the exact "
           "defect this item closes";
    EXPECT_FALSE(contains(info.method, "AES"))
        << "the summary method string must not say AES for an RC4 document; "
           "got: " << info.method;
}

TEST_F(V4R4CryptFilterMethodTest, ControlGenuineAesV2IsStillReportedAes128) {
    const auto info = infoFor("v4r4_cfm_aesv2.pdf");

    ASSERT_TRUE(info.isEncrypted);
    ASSERT_EQ(info.version, 4);
    ASSERT_EQ(info.revision, 4);
    ASSERT_EQ(info.streamCryptMethod, std::string("AES"))
        << "precondition: the control fixture's /StmF crypt filter must be AES";

    EXPECT_EQ(info.method, std::string("AES-128"))
        << "CONTROL: a genuine AESV2 document must still be reported AES-128. "
           "Without this the fix could be a blanket re-label of every /V 4 /R 4 "
           "document to RC4-128, which understates instead of overstating.";
}

TEST_F(V4R4CryptFilterMethodTest, TheTwoFixturesDifferOnlyInTheReportedMethod) {
    // The discrimination is carried by /CFM alone: version, revision and key
    // length are identical across the pair, so nothing else could explain a
    // different answer.
    const auto rc4 = infoFor("v4r4_cfm_v2.pdf");
    const auto aes = infoFor("v4r4_cfm_aesv2.pdf");

    EXPECT_EQ(rc4.version, aes.version);
    EXPECT_EQ(rc4.revision, aes.revision);
    EXPECT_EQ(rc4.keyLength, aes.keyLength);
    EXPECT_NE(rc4.method, aes.method)
        << "if the method string is the same for both, it is not reading /CFM";
    EXPECT_FALSE(rc4.streamsUnprotected)
        << "neither fixture is an /Identity document — this item is about a "
           "mis-LABELLED cipher, not an absent one";
    EXPECT_FALSE(aes.streamsUnprotected);
}

// ===========================================================================
// ITEM 4 — the empty-user-password warning at the point of setting.
// ===========================================================================

TEST(EmptyUserPasswordPredicate, OwnerOnlyProtectionIsTheOneCondition) {
    EncryptionSettings ownerOnly;
    ownerOnly.algorithm = EncryptionAlgorithm::AES_256;
    ownerOnly.ownerPassword = "Owner-Pass-1!";
    ownerOnly.userPassword = "";
    EXPECT_TRUE(ownerOnly.isOwnerOnlyProtection());

    // CONTROL 1 — a user password present: the content IS protected.
    EncryptionSettings both = ownerOnly;
    both.userPassword = "User-Pass-1!";
    EXPECT_FALSE(both.isOwnerOnlyProtection())
        << "CONTROL: setting a user password must clear the condition";

    // CONTROL 2 — no encryption at all: there is nothing to overstate.
    EncryptionSettings none = ownerOnly;
    none.algorithm = EncryptionAlgorithm::None;
    EXPECT_FALSE(none.isOwnerOnlyProtection())
        << "CONTROL: an unencrypted save is not owner-only protection";

    // CONTROL 3 — user password only: no owner password to mislead anyone.
    EncryptionSettings userOnly;
    userOnly.algorithm = EncryptionAlgorithm::AES_256;
    userOnly.userPassword = "User-Pass-1!";
    EXPECT_FALSE(userOnly.isOwnerOnlyProtection());
}

TEST(EmptyUserPasswordPredicate, SaveReportCarriesTheWarningForApiCallers) {
    EncryptionSettings ownerOnly;
    ownerOnly.algorithm = EncryptionAlgorithm::AES_256;
    ownerOnly.ownerPassword = "Owner-Pass-1!";

    const auto report = EncryptionManager::previewPermissionMapping(ownerOnly);
    EXPECT_TRUE(report.emptyUserPassword);
    EXPECT_EQ(report.emptyUserPasswordWarning,
              std::string(emptyUserPasswordWarningText()));
    EXPECT_TRUE(contains(report.emptyUserPasswordWarning, "EMPTY"))
        << "the warning must say what is wrong, not merely be non-empty";

    EncryptionSettings both = ownerOnly;
    both.userPassword = "User-Pass-1!";
    const auto control = EncryptionManager::previewPermissionMapping(both);
    EXPECT_FALSE(control.emptyUserPassword)
        << "CONTROL: a document with a user password must not be warned about";
    EXPECT_TRUE(control.emptyUserPasswordWarning.empty());
}

// --- The surfacing path itself: the dialog, at the moment of typing. -------

class EncryptionDialogWarningTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_NE(qApp, nullptr) << "requires the QApplication from test_main";
        dialog_ = std::make_unique<pdfe::ui::EncryptionDialog>();
        warning_ = dialog_->findChild<QLabel*>("emptyUserPasswordWarningLabel");
        ASSERT_NE(warning_, nullptr)
            << "the warning label must exist and carry a stable objectName so "
               "pywinauto/UIA can reach it";
        algorithm_ = dialog_->findChild<QComboBox*>("algorithmCombo");
        userPw_ = dialog_->findChild<QLineEdit*>("userPasswordEdit");
        ownerPw_ = dialog_->findChild<QLineEdit*>("ownerPasswordEdit");
        ASSERT_NE(algorithm_, nullptr);
        ASSERT_NE(userPw_, nullptr);
        ASSERT_NE(ownerPw_, nullptr);
    }

    void TearDown() override { dialog_.reset(); }

    std::unique_ptr<pdfe::ui::EncryptionDialog> dialog_;
    QLabel* warning_ = nullptr;
    QComboBox* algorithm_ = nullptr;
    QLineEdit* userPw_ = nullptr;
    QLineEdit* ownerPw_ = nullptr;
};

TEST_F(EncryptionDialogWarningTest, WarningAppearsWhenOwnerSetAndUserLeftEmpty) {
    algorithm_->setCurrentIndex(4);           // AES-256
    ownerPw_->setText("Owner-Pass-1!");       // typing the owner password …
    userPw_->setText("");                     // … and no user password

    EXPECT_TRUE(warning_->isVisibleTo(dialog_.get()))
        << "the warning must be surfaced AT THE POINT OF SETTING, not only in "
           "a permissions dialog nothing prompts the user to open";
    EXPECT_EQ(warning_->text().toStdString(),
              std::string(emptyUserPasswordWarningText()))
        << "the dialog must show the same single wording the save report and "
           "the permissions dialog use";
}

TEST_F(EncryptionDialogWarningTest, ControlWarningClearsAsSoonAsAUserPwIsTyped) {
    algorithm_->setCurrentIndex(4);
    ownerPw_->setText("Owner-Pass-1!");
    ASSERT_FALSE(warning_->text().isEmpty()) << "precondition: warning shown";

    userPw_->setText("User-Pass-1!");

    EXPECT_TRUE(warning_->text().isEmpty())
        << "CONTROL: a warning that never clears is decoration — it must track "
           "the condition, not merely appear once";
    EXPECT_FALSE(warning_->isVisibleTo(dialog_.get()));
}

TEST_F(EncryptionDialogWarningTest, ControlNoWarningWhenEncryptionIsOff) {
    algorithm_->setCurrentIndex(0);           // None
    ownerPw_->setText("Owner-Pass-1!");

    EXPECT_TRUE(warning_->text().isEmpty())
        << "CONTROL: with encryption off there is no protection to overstate";
    EXPECT_FALSE(warning_->isVisibleTo(dialog_.get()));
}
