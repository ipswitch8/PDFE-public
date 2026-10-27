/**
 * @file test_sign_dialog.cpp
 * @brief Unit tests for SignDialog.
 */

#include <gtest/gtest.h>
#include "ui/SignDialog.h"
#include "signatures/SignatureData.h"
#include "signatures/SignatureManager.h"
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/Page.h"

#include <QApplication>
#include <QComboBox>
#include <QLineEdit>
#include <QCheckBox>
#include <QPushButton>
#include <QLabel>
#include <QSignalSpy>
#include <QTest>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>

class SignDialogTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!qApp) {
            GTEST_SKIP() << "QApplication not available";
        }
    }

    void TearDown() override {
        dialog.reset();
    }

    std::unique_ptr<pdfe::ui::SignDialog> dialog;
};

// Test 1: Construct and destroy
TEST_F(SignDialogTest, ConstructDestroy) {
    dialog = std::make_unique<pdfe::ui::SignDialog>(nullptr);
    ASSERT_NE(dialog, nullptr);
    EXPECT_EQ("signDialog", dialog->objectName().toStdString());
}

// Test 2: ObjectNames
TEST_F(SignDialogTest, ObjectNames) {
    dialog = std::make_unique<pdfe::ui::SignDialog>(nullptr);

    EXPECT_NE(nullptr, dialog->findChild<QLineEdit*>("pfxPathEdit"));
    EXPECT_NE(nullptr, dialog->findChild<QPushButton*>("browseButton"));
    EXPECT_NE(nullptr, dialog->findChild<QLineEdit*>("pfxPasswordEdit"));
    EXPECT_NE(nullptr, dialog->findChild<QLineEdit*>("reasonEdit"));
    EXPECT_NE(nullptr, dialog->findChild<QLineEdit*>("locationEdit"));
    EXPECT_NE(nullptr, dialog->findChild<QCheckBox*>("showLabelsCheck"));
    EXPECT_NE(nullptr, dialog->findChild<QCheckBox*>("showNameCheck"));
    EXPECT_NE(nullptr, dialog->findChild<QCheckBox*>("showDateCheck"));
    EXPECT_NE(nullptr, dialog->findChild<QCheckBox*>("showDnCheck"));
    EXPECT_NE(nullptr, dialog->findChild<QCheckBox*>("showLogoCheck"));
    EXPECT_NE(nullptr, dialog->findChild<QLabel*>("previewLabel"));
    EXPECT_NE(nullptr, dialog->findChild<QLabel*>("validationLabel"));
    EXPECT_NE(nullptr, dialog->findChild<QPushButton*>("okButton"));
    EXPECT_NE(nullptr, dialog->findChild<QPushButton*>("cancelButton"));
}

// Test 3: OK button always enabled (validation moved to accept()).
//
// Phase-7 (pipeline b3f19d72 / P12-INFRA-1 sub-issue 3) decoupled
// the OK button's enabled state from text-changed signals so a
// UIA-driven InvokePattern click reliably reaches QDialog::accept,
// even when UIA ValuePattern.SetValue on a password QLineEdit does
// not fire textChanged.  The validation now runs in SignDialog::
// accept() — empty inputs make accept() a no-op (the dialog stays
// open with an inline validation message) rather than disabling
// the OK button.
TEST_F(SignDialogTest, OkAlwaysEnabled) {
    dialog = std::make_unique<pdfe::ui::SignDialog>(nullptr);

    auto* okButton = dialog->findChild<QPushButton*>("okButton");
    ASSERT_NE(okButton, nullptr);
    EXPECT_TRUE(okButton->isEnabled());
}

// Test 4: OK still enabled with valid input (contract unchanged for
// the legitimate happy-path; only the disabled-on-empty side changed).
TEST_F(SignDialogTest, OkEnabled_WithValidInput) {
    dialog = std::make_unique<pdfe::ui::SignDialog>(nullptr);

    auto* pfxPath = dialog->findChild<QLineEdit*>("pfxPathEdit");
    auto* pfxPassword = dialog->findChild<QLineEdit*>("pfxPasswordEdit");
    auto* okButton = dialog->findChild<QPushButton*>("okButton");

    ASSERT_NE(pfxPath, nullptr);
    ASSERT_NE(pfxPassword, nullptr);
    ASSERT_NE(okButton, nullptr);

    pfxPath->setText("C:/test/cert.pfx");
    pfxPassword->setText("password123");

    EXPECT_TRUE(okButton->isEnabled());
}

// Test 5: With password empty, accept() refuses to close the dialog
// (validationLabel_ is populated, OK stays enabled per new contract).
TEST_F(SignDialogTest, AcceptRejected_EmptyPassword) {
    dialog = std::make_unique<pdfe::ui::SignDialog>(nullptr);

    auto* pfxPath = dialog->findChild<QLineEdit*>("pfxPathEdit");
    auto* okButton = dialog->findChild<QPushButton*>("okButton");
    auto* validationLabel = dialog->findChild<QLabel*>("validationLabel");

    ASSERT_NE(pfxPath, nullptr);
    ASSERT_NE(okButton, nullptr);
    ASSERT_NE(validationLabel, nullptr);

    pfxPath->setText("C:/test/cert.pfx");
    // Password still empty.

    // OK is always enabled under the new contract.
    EXPECT_TRUE(okButton->isEnabled());

    // accept() should be a no-op when password is empty.  We can't
    // call dialog->accept() directly without an exec() loop, but we
    // can verify validateInput()'s inline message and confirm the
    // dialog is NOT in the Accepted state.
    QSignalSpy acceptedSpy(dialog.get(), &QDialog::accepted);
    dialog->accept();
    int spyCount = static_cast<int>(acceptedSpy.count());
    EXPECT_EQ(spyCount, 0);
    EXPECT_FALSE(validationLabel->text().isEmpty());
}

// Test 6: SigningOptions returns correct values
TEST_F(SignDialogTest, SigningOptions_ReturnsValues) {
    dialog = std::make_unique<pdfe::ui::SignDialog>(nullptr);

    auto* pfxPath = dialog->findChild<QLineEdit*>("pfxPathEdit");
    auto* pfxPassword = dialog->findChild<QLineEdit*>("pfxPasswordEdit");
    auto* reason = dialog->findChild<QLineEdit*>("reasonEdit");
    auto* location = dialog->findChild<QLineEdit*>("locationEdit");

    pfxPath->setText("C:/certs/signer.pfx");
    pfxPassword->setText("secret");
    reason->setText("Approval");
    location->setText("Office");

    auto opts = dialog->signingOptions();
    EXPECT_EQ("C:/certs/signer.pfx", opts.pfxPath);
    EXPECT_EQ("secret", opts.pfxPassword);
    EXPECT_EQ("Approval", opts.reason);
    EXPECT_EQ("Office", opts.location);
}

// Test 7: Appearance flags default all checked
TEST_F(SignDialogTest, AppearanceFlags_DefaultAllChecked) {
    dialog = std::make_unique<pdfe::ui::SignDialog>(nullptr);

    auto* showLabels = dialog->findChild<QCheckBox*>("showLabelsCheck");
    auto* showName = dialog->findChild<QCheckBox*>("showNameCheck");
    auto* showDate = dialog->findChild<QCheckBox*>("showDateCheck");
    auto* showDn = dialog->findChild<QCheckBox*>("showDnCheck");
    auto* showLogo = dialog->findChild<QCheckBox*>("showLogoCheck");

    EXPECT_TRUE(showLabels->isChecked());
    EXPECT_TRUE(showName->isChecked());
    EXPECT_TRUE(showDate->isChecked());
    EXPECT_TRUE(showDn->isChecked());
    EXPECT_TRUE(showLogo->isChecked());

    // Set required fields
    dialog->findChild<QLineEdit*>("pfxPathEdit")->setText("test.pfx");
    dialog->findChild<QLineEdit*>("pfxPasswordEdit")->setText("pass");

    auto opts = dialog->signingOptions();
    // All checked: 1 + 2 + 4 + 8 + 32 = 47 (0x2F)
    EXPECT_EQ(47, opts.appearanceFlags);
}

// Test 8: Uncheck some appearance options
TEST_F(SignDialogTest, AppearanceFlags_Partial) {
    dialog = std::make_unique<pdfe::ui::SignDialog>(nullptr);

    auto* showLabels = dialog->findChild<QCheckBox*>("showLabelsCheck");
    auto* showName = dialog->findChild<QCheckBox*>("showNameCheck");
    auto* showDate = dialog->findChild<QCheckBox*>("showDateCheck");
    auto* showDn = dialog->findChild<QCheckBox*>("showDnCheck");
    auto* showLogo = dialog->findChild<QCheckBox*>("showLogoCheck");

    // Uncheck everything except labels
    showName->setChecked(false);
    showDate->setChecked(false);
    showDn->setChecked(false);
    showLogo->setChecked(false);

    dialog->findChild<QLineEdit*>("pfxPathEdit")->setText("test.pfx");
    dialog->findChild<QLineEdit*>("pfxPasswordEdit")->setText("pass");

    auto opts = dialog->signingOptions();
    EXPECT_EQ(1, opts.appearanceFlags); // Only labels
}

// =====================================================================
// D1 reconciliation: integration tests for UI-SIG-02 / UI-SIG-03 / UI-SIG-05.
// Drives SignDialog widgets via objectName, calls signingOptions(),
// feeds the result into SignatureManager::signField against the
// committed test_signer.pfx fixture, and verifies the resulting PDF.
// =====================================================================

namespace {

struct SigningFixture {
    std::shared_ptr<pdfe::core::MuPdfContext> context;
    std::unique_ptr<pdfe::signatures::SignatureManager> manager;
    std::filesystem::path tempPdf;
    std::string pfxPath;
};

// Prepare a working copy of test_single.pdf with a signature field
// already created — pdf_sign_signature does an incremental save
// internally, so we need a writable file with the field placed.
SigningFixture prepareFixture(const std::string& tempName,
                               const std::string& fieldName)
{
    SigningFixture f;
    f.context = std::make_shared<pdfe::core::MuPdfContext>();
    f.manager = std::make_unique<pdfe::signatures::SignatureManager>(f.context);
    f.pfxPath = std::string(TEST_FIXTURES_DIR) + "/test_signer.pfx";
    std::string srcPdf = std::string(TEST_FIXTURES_DIR) + "/test_single.pdf";
    f.tempPdf = std::filesystem::temp_directory_path() / tempName;

    auto src = pdfe::core::Document::open(f.context, srcPdf);
    auto page = src->page(0);
    f.manager->createSignatureField(page.get(), fieldName,
                                    QRectF(100, 500, 200, 50));
    src->saveAs(f.tempPdf);
    return f;
}

} // namespace

// UI-SIG-02 — SignDialog with valid .pfx + correct password produces
// a valid PKCS#7 signature embedded in the PDF.
TEST_F(SignDialogTest, SIG02_DialogToValidSignature) {
    if (!std::filesystem::exists(std::string(TEST_FIXTURES_DIR) +
                                 "/test_signer.pfx") ||
        !std::filesystem::exists(std::string(TEST_FIXTURES_DIR) +
                                 "/test_single.pdf")) {
        GTEST_SKIP() << "Required fixtures not available";
    }

    auto fix = prepareFixture("pdfe_sig02_test.pdf", "SIG02_Field");

    dialog = std::make_unique<pdfe::ui::SignDialog>(nullptr);
    dialog->findChild<QLineEdit*>("pfxPathEdit")
        ->setText(QString::fromStdString(fix.pfxPath));
    dialog->findChild<QLineEdit*>("pfxPasswordEdit")->setText("test123");
    dialog->findChild<QLineEdit*>("reasonEdit")->setText("UI-SIG-02");
    dialog->findChild<QLineEdit*>("locationEdit")->setText("D1 cycle");

    auto opts = dialog->signingOptions();
    EXPECT_EQ(opts.pfxPath, fix.pfxPath);
    EXPECT_EQ(opts.pfxPassword, "test123");

    {
        auto doc = pdfe::core::Document::open(fix.context, fix.tempPdf);
        auto page = doc->page(0);
        ASSERT_NO_THROW(
            fix.manager->signField(doc.get(), page.get(), 0, opts));
    }

    // Reopen and verify the embedded PKCS#7 digest validates.
    {
        auto doc = pdfe::core::Document::open(fix.context, fix.tempPdf);
        auto page = doc->page(0);
        auto result =
            fix.manager->verifySignature(doc.get(), page.get(), 0);
        EXPECT_TRUE(result.digestValid)
            << "SignDialog→signField produced an invalid digest: "
            << signatureErrorToString(result.digestStatus);
        EXPECT_EQ(pdfe::signatures::SignatureError::Okay,
                  result.digestStatus);
        EXPECT_FALSE(result.modifiedAfterSigning);
    }

    std::filesystem::remove(fix.tempPdf);
}

// UI-SIG-03 — SignDialog with wrong password surfaces an error and
// does NOT embed a usable signature.
TEST_F(SignDialogTest, SIG03_WrongPasswordRejected) {
    if (!std::filesystem::exists(std::string(TEST_FIXTURES_DIR) +
                                 "/test_signer.pfx") ||
        !std::filesystem::exists(std::string(TEST_FIXTURES_DIR) +
                                 "/test_single.pdf")) {
        GTEST_SKIP() << "Required fixtures not available";
    }

    auto fix = prepareFixture("pdfe_sig03_test.pdf", "SIG03_Field");

    dialog = std::make_unique<pdfe::ui::SignDialog>(nullptr);
    dialog->findChild<QLineEdit*>("pfxPathEdit")
        ->setText(QString::fromStdString(fix.pfxPath));
    // Wrong password — production must surface an error, not crash
    // and not produce a verifiable signature.
    dialog->findChild<QLineEdit*>("pfxPasswordEdit")->setText("WRONG-pw-xx");

    auto opts = dialog->signingOptions();
    ASSERT_EQ(opts.pfxPassword, "WRONG-pw-xx");

    bool threw = false;
    try {
        auto doc = pdfe::core::Document::open(fix.context, fix.tempPdf);
        auto page = doc->page(0);
        fix.manager->signField(doc.get(), page.get(), 0, opts);
    } catch (const std::exception&) {
        threw = true;
    }

    // Independent of which branch fired, the on-disk PDF must NOT
    // contain a verifying signature. Two checks:
    //   (a) byte-grep — a real PKCS#7 envelope is always written into
    //       the file as /Type /Sig with /Contents <hex...>. Wrong-pw
    //       must leave no /Type /Sig in the file.
    //   (b) verify-on-reopen — even if some byte sequence sneaks past
    //       (a), digestValid must be false. Production guarantees one
    //       of these holds.
    auto fileContainsBytes = [](const std::filesystem::path& p,
                                const std::string& needle) {
        std::ifstream in(p, std::ios::binary);
        if (!in) return false;
        std::stringstream ss;
        ss << in.rdbuf();
        return ss.str().find(needle) != std::string::npos;
    };

    const bool hasSigObj = fileContainsBytes(fix.tempPdf, "/Type/Sig")
                        || fileContainsBytes(fix.tempPdf, "/Type /Sig");

    bool digestValid = false;
    if (hasSigObj) {
        // signField produced a /Sig object despite the wrong password.
        // The verify path is the user-facing error surface that must
        // catch this — assert it does.
        try {
            auto doc = pdfe::core::Document::open(fix.context, fix.tempPdf);
            auto page = doc->page(0);
            auto result =
                fix.manager->verifySignature(doc.get(), page.get(), 0);
            digestValid = result.digestValid;
        } catch (const std::exception&) {
            // verify also failed — that's an acceptable error surface.
        }
    }

    EXPECT_FALSE(hasSigObj && digestValid)
        << "Wrong PFX password produced a verifying signature — user "
           "would see a misleading 'success' (UI-SIG-03). Either "
           "signField must throw, or the resulting /Sig must fail "
           "verification. threw=" << threw << " hasSigObj=" << hasSigObj
        << " digestValid=" << digestValid;

    std::filesystem::remove(fix.tempPdf);
}

// UI-SIG-05 — Certification combo "No changes allowed" sets DocMDP
// transform to /P 1 in the resulting PDF /Perms dict.
TEST_F(SignDialogTest, SIG05_CertificationNoChangesSetsDocMDP) {
    if (!std::filesystem::exists(std::string(TEST_FIXTURES_DIR) +
                                 "/test_signer.pfx") ||
        !std::filesystem::exists(std::string(TEST_FIXTURES_DIR) +
                                 "/test_single.pdf")) {
        GTEST_SKIP() << "Required fixtures not available";
    }

    auto fix = prepareFixture("pdfe_sig05_test.pdf", "SIG05_Field");

    dialog = std::make_unique<pdfe::ui::SignDialog>(nullptr);
    dialog->findChild<QLineEdit*>("pfxPathEdit")
        ->setText(QString::fromStdString(fix.pfxPath));
    dialog->findChild<QLineEdit*>("pfxPasswordEdit")->setText("test123");

    // The combo's user-visible text for index 1 is "Certify — no changes
    // allowed"; the userData maps to CertificationLevel::NoChanges (=1).
    auto* certCombo = dialog->findChild<QComboBox*>("certificationLevelCombo");
    ASSERT_NE(certCombo, nullptr);
    certCombo->setCurrentIndex(1);

    auto opts = dialog->signingOptions();
    ASSERT_EQ(opts.certification,
              pdfe::signatures::CertificationLevel::NoChanges);

    {
        auto doc = pdfe::core::Document::open(fix.context, fix.tempPdf);
        auto page = doc->page(0);
        ASSERT_NO_THROW(
            fix.manager->signField(doc.get(), page.get(), 0, opts));
    }

    // Inspect the resulting PDF bytes for the DocMDP markers. This
    // mirrors the byte-grep pattern test_invisible_and_certification_signatures.cpp
    // already uses for certification verification — production code at
    // SignatureManager.cpp:225-258 stamps /Perms /DocMDP /TransformParams
    // /P <level> on the catalog before the incremental save, and that's
    // the wire-level evidence UI-SIG-05 demands.
    auto fileContainsBytes = [](const std::filesystem::path& p,
                                const std::string& needle) {
        std::ifstream in(p, std::ios::binary);
        if (!in) return false;
        std::stringstream ss;
        ss << in.rdbuf();
        return ss.str().find(needle) != std::string::npos;
    };
    EXPECT_TRUE(fileContainsBytes(fix.tempPdf, "/Perms"))
        << "Certified document missing /Perms entry";
    EXPECT_TRUE(fileContainsBytes(fix.tempPdf, "/DocMDP"))
        << "Certified document missing /DocMDP entry";
    EXPECT_TRUE(fileContainsBytes(fix.tempPdf, "/TransformMethod"))
        << "Certified document missing /TransformMethod";
    EXPECT_TRUE(fileContainsBytes(fix.tempPdf, "/P 1"))
        << "Certification level 'No changes' must produce /P 1 "
           "(CertificationLevel::NoChanges); SignDialog combo index 1 "
           "must map to that level (UI-SIG-05).";

    std::filesystem::remove(fix.tempPdf);
}
