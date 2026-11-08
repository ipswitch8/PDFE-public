// -*- coding: utf-8 -*-
//
// GitHub #361 — creating a self-signed PFX when the user has none.
//
// Covers the new Pkcs7OpenSslBridge::createSelfSignedPfx primitive and the
// SignDialog "Create certificate..." headless seam:
//
//   * round-trip: a created .pfx loads back via readPfx with the same
//     passphrase and yields a usable signer, and can actually sign a document;
//   * validation: empty CN and empty passphrase are rejected and write NO
//     .pfx to disk;
//   * no-leak: every artifact lands ONLY in a QTemporaryDir (never the repo)
//     and no error path ever surfaces the passphrase or key material;
//   * the SignDialog seam runs the REAL create + field-population path so
//     signingOptions() returns the freshly created certificate.
//
// SECURITY: the only passphrase in this file is the obviously-dummy "test123"
// literal, used solely against a QTemporaryDir output. No key, passphrase, or
// certificate bytes are written under the repo.

#include <gtest/gtest.h>

#include "signatures/Pkcs7OpenSslBridge.h"
#include "signatures/SignatureManager.h"
#include "signatures/SignatureData.h"
#include "ui/SignDialog.h"
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/Page.h"

#include <QApplication>
#include <QLineEdit>
#include <QPushButton>
#include <QRectF>
#include <QString>
#include <QTemporaryDir>

#include <filesystem>
#include <optional>
#include <string>

using pdfe::signatures::Pkcs7OpenSslBridge;

namespace {

// TEST_FIXTURES_DIR is <repo>/tests/fixtures.
std::filesystem::path repoRoot()
{
    return std::filesystem::path(TEST_FIXTURES_DIR).parent_path().parent_path();
}

// Obviously-dummy test passphrase. Never a real credential; only ever applied
// to a throwaway certificate written into a QTemporaryDir.
constexpr const char* kDummyPass = "test123";

} // namespace

// ── Round-trip: created .pfx loads back via readPfx and yields a signer ──────

TEST(CreateSelfSignedCert, CreatedPfxRoundTripsThroughReadPfx)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::filesystem::path out =
        std::filesystem::path(tmp.path().toStdString()) / "created.pfx";

    Pkcs7OpenSslBridge::SelfSignedCertOptions opts;
    opts.commonName = "PDFE Self-Signed Test";
    opts.organization = "PDFE Test Org";
    opts.validityDays = 30;

    const auto res =
        Pkcs7OpenSslBridge::createSelfSignedPfx(out.string(), kDummyPass, opts);
    ASSERT_TRUE(res.success) << res.errorMessage;
    ASSERT_TRUE(std::filesystem::exists(out));
    EXPECT_GT(std::filesystem::file_size(out), 100u)
        << "a real PKCS#12 with a key + cert is well over 100 bytes";

    // Load it back through the SAME path a user-supplied PFX takes.
    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    Pkcs7OpenSslBridge bridge;
    pdf_pkcs7_signer* signer = nullptr;
    ASSERT_NO_THROW(ctx->execute([&](fz_context* c) {
        signer = bridge.readPfx(c, out.string().c_str(), kDummyPass);
    }));
    ASSERT_NE(signer, nullptr)
        << "the created PFX did not load back via readPfx with its passphrase";

    // Drop the signer on the same context.
    ctx->execute([&](fz_context* c) {
        Pkcs7OpenSslBridge::dropSigner(c, signer);
    });
}

// A wrong passphrase must NOT open the created PFX (the created container is
// genuinely passphrase-protected, not a bare key).
TEST(CreateSelfSignedCert, CreatedPfxRejectsWrongPassphrase)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::filesystem::path out =
        std::filesystem::path(tmp.path().toStdString()) / "created.pfx";

    Pkcs7OpenSslBridge::SelfSignedCertOptions opts;
    opts.commonName = "PDFE Self-Signed Test";
    const auto res =
        Pkcs7OpenSslBridge::createSelfSignedPfx(out.string(), kDummyPass, opts);
    ASSERT_TRUE(res.success) << res.errorMessage;

    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    Pkcs7OpenSslBridge bridge;
    bool threw = false;
    pdf_pkcs7_signer* signer = nullptr;
    try {
        ctx->execute([&](fz_context* c) {
            signer = bridge.readPfx(c, out.string().c_str(), "WRONG-pw-xx");
        });
    } catch (const std::exception&) {
        threw = true;
    }
    EXPECT_TRUE(threw && signer == nullptr)
        << "a wrong passphrase must not open the created PFX";
}

// ── Can be used to sign a real document ──────────────────────────────────────

TEST(CreateSelfSignedCert, CreatedPfxSignsADocument)
{
    const std::string fixture =
        std::string(TEST_FIXTURES_DIR) + "/test_single.pdf";
    if (!std::filesystem::exists(fixture)) {
        GTEST_SKIP() << "test_single.pdf fixture not available";
    }

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::filesystem::path pfx =
        std::filesystem::path(tmp.path().toStdString()) / "signer.pfx";

    Pkcs7OpenSslBridge::SelfSignedCertOptions opts;
    opts.commonName = "PDFE Signer";
    const auto res =
        Pkcs7OpenSslBridge::createSelfSignedPfx(pfx.string(), kDummyPass, opts);
    ASSERT_TRUE(res.success) << res.errorMessage;

    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    pdfe::signatures::SignatureManager mgr(ctx);

    const std::filesystem::path work =
        std::filesystem::path(tmp.path().toStdString()) / "to_sign.pdf";
    {
        auto doc = pdfe::core::Document::open(ctx, fixture);
        ASSERT_NE(doc, nullptr);
        auto page = doc->page(0);
        mgr.createSignatureField(page.get(), "CreatedCertField",
                                 QRectF(100, 500, 200, 50));
        doc->saveAs(work);
    }
    {
        auto doc = pdfe::core::Document::open(ctx, work);
        ASSERT_NE(doc, nullptr);
        auto page = doc->page(0);
        pdfe::signatures::SigningOptions sopts;
        sopts.pfxPath = pfx.string();
        sopts.pfxPassword = kDummyPass;
        sopts.reason = "created-cert sign (#361)";
        ASSERT_NO_THROW(mgr.signField(doc.get(), page.get(), 0, sopts));
    }
    {
        auto doc = pdfe::core::Document::open(ctx, work);
        ASSERT_NE(doc, nullptr);
        auto page = doc->page(0);
        const auto vr = mgr.verifySignature(doc.get(), page.get(), 0);
        EXPECT_TRUE(vr.digestValid)
            << "a signature made with the self-signed cert must validate: "
            << signatureErrorToString(vr.digestStatus);
    }
}

// ── Validation: empty CN / empty passphrase rejected, nothing written ────────

TEST(CreateSelfSignedCert, EmptyCommonNameRejectedNoFileWritten)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::filesystem::path out =
        std::filesystem::path(tmp.path().toStdString()) / "nope.pfx";

    Pkcs7OpenSslBridge::SelfSignedCertOptions opts;
    opts.commonName = "";  // invalid
    const auto res =
        Pkcs7OpenSslBridge::createSelfSignedPfx(out.string(), kDummyPass, opts);

    EXPECT_FALSE(res.success);
    EXPECT_FALSE(res.errorMessage.empty());
    EXPECT_FALSE(std::filesystem::exists(out))
        << "no .pfx must be written when the CN is empty";
}

TEST(CreateSelfSignedCert, EmptyPassphraseRejectedNoFileWritten)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::filesystem::path out =
        std::filesystem::path(tmp.path().toStdString()) / "nope.pfx";

    Pkcs7OpenSslBridge::SelfSignedCertOptions opts;
    opts.commonName = "Valid CN";
    const auto res =
        Pkcs7OpenSslBridge::createSelfSignedPfx(out.string(), /*passphrase*/ "",
                                                opts);

    EXPECT_FALSE(res.success);
    EXPECT_FALSE(res.errorMessage.empty());
    EXPECT_FALSE(std::filesystem::exists(out))
        << "no .pfx must be written when the passphrase is empty";
}

// ── No-leak: artifacts stay in temp, no secret ever surfaces ─────────────────

TEST(CreateSelfSignedCert, ArtifactsStayInTempAndNoSecretLeaks)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::filesystem::path out =
        std::filesystem::path(tmp.path().toStdString()) / "leakcheck.pfx";

    Pkcs7OpenSslBridge::SelfSignedCertOptions opts;
    opts.commonName = "Leak Check CN";
    const std::string pass = kDummyPass;

    const auto res =
        Pkcs7OpenSslBridge::createSelfSignedPfx(out.string(), pass, opts);
    ASSERT_TRUE(res.success) << res.errorMessage;

    // (a) The artifact is under the QTemporaryDir, never under the repo.
    EXPECT_EQ(std::filesystem::path(out).parent_path(),
              std::filesystem::path(tmp.path().toStdString()));
    const std::string outStr =
        std::filesystem::weakly_canonical(out).string();
    const std::string repoStr =
        std::filesystem::weakly_canonical(repoRoot()).string();
    EXPECT_NE(outStr.rfind(repoStr, 0), 0u)
        << "certificate artifact was written under the repo: " << outStr;

    // (b) The success result exposes no secret-bearing field: it carries only
    //     success + a (secret-free) errorMessage, and on success that message
    //     is empty.
    EXPECT_TRUE(res.errorMessage.empty());

    // (c) A FAILURE path must still never surface the passphrase. Force a write
    //     failure by targeting a path whose parent directory does not exist.
    const std::filesystem::path badOut =
        std::filesystem::path(tmp.path().toStdString()) /
        "no_such_subdir" / "out.pfx";
    const auto failRes =
        Pkcs7OpenSslBridge::createSelfSignedPfx(badOut.string(), pass, opts);
    EXPECT_FALSE(failRes.success);
    EXPECT_EQ(failRes.errorMessage.find(pass), std::string::npos)
        << "the error message leaked the passphrase";
    EXPECT_FALSE(std::filesystem::exists(badOut));
}

// ── SignDialog headless seam: real create + field population ─────────────────

TEST(CreateSelfSignedCert, SignDialogSeamCreatesAndPopulatesFields)
{
    if (!qApp) {
        GTEST_SKIP() << "QApplication not available";
    }
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::filesystem::path out =
        std::filesystem::path(tmp.path().toStdString()) / "dialog_created.pfx";

    pdfe::ui::SignDialog dlg(nullptr);

    pdfe::ui::SignDialog::CreateCertTestInputs in;
    in.accepted = true;
    in.commonName = "Dialog Seam CN";
    in.organization = "Dialog Org";
    in.validityDays = 90;
    in.passphrase = QString::fromUtf8(kDummyPass);
    in.outputPath = QString::fromStdString(out.string());
    dlg.setCreateCertTestInputsForTesting(in);

    auto* btn = dlg.findChild<QPushButton*>("createCertButton");
    ASSERT_NE(btn, nullptr) << "the Create-certificate affordance is missing";
    btn->click();  // runs onCreateCert() headlessly via the seam

    EXPECT_TRUE(dlg.lastCreateCertErrorForTesting().isEmpty())
        << dlg.lastCreateCertErrorForTesting().toStdString();
    ASSERT_TRUE(std::filesystem::exists(out))
        << "the seam must run the REAL createSelfSignedPfx";

    auto* pfxPath = dlg.findChild<QLineEdit*>("pfxPathEdit");
    auto* pfxPass = dlg.findChild<QLineEdit*>("pfxPasswordEdit");
    ASSERT_NE(pfxPath, nullptr);
    ASSERT_NE(pfxPass, nullptr);
    EXPECT_EQ(pfxPath->text(), QString::fromStdString(out.string()));
    EXPECT_EQ(pfxPass->text(), QString::fromUtf8(kDummyPass));

    // signingOptions() now returns the freshly created certificate.
    const auto opts = dlg.signingOptions();
    EXPECT_EQ(opts.pfxPath, out.string());
    EXPECT_EQ(opts.pfxPassword, std::string(kDummyPass));
}

// Cancelling the entry dialog (accepted == false) leaves the PFX fields empty
// and writes nothing.
TEST(CreateSelfSignedCert, SignDialogSeamCancelLeavesFieldsEmpty)
{
    if (!qApp) {
        GTEST_SKIP() << "QApplication not available";
    }
    pdfe::ui::SignDialog dlg(nullptr);

    pdfe::ui::SignDialog::CreateCertTestInputs in;
    in.accepted = false;  // simulate the user cancelling the entry dialog
    in.commonName = "Should Not Be Used";
    in.passphrase = QString::fromUtf8(kDummyPass);
    dlg.setCreateCertTestInputsForTesting(in);

    auto* btn = dlg.findChild<QPushButton*>("createCertButton");
    ASSERT_NE(btn, nullptr);
    btn->click();

    auto* pfxPath = dlg.findChild<QLineEdit*>("pfxPathEdit");
    ASSERT_NE(pfxPath, nullptr);
    EXPECT_TRUE(pfxPath->text().isEmpty())
        << "a cancelled create-cert flow must not populate the PFX path";
}

// The dialog validation path: an empty CN injected through the seam is rejected
// (secret-free error recorded, no field populated).
TEST(CreateSelfSignedCert, SignDialogSeamEmptyCnRejected)
{
    if (!qApp) {
        GTEST_SKIP() << "QApplication not available";
    }
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::filesystem::path out =
        std::filesystem::path(tmp.path().toStdString()) / "rejected.pfx";

    pdfe::ui::SignDialog dlg(nullptr);

    pdfe::ui::SignDialog::CreateCertTestInputs in;
    in.accepted = true;
    in.commonName = "";  // invalid
    in.passphrase = QString::fromUtf8(kDummyPass);
    in.outputPath = QString::fromStdString(out.string());
    dlg.setCreateCertTestInputsForTesting(in);

    dlg.findChild<QPushButton*>("createCertButton")->click();

    EXPECT_FALSE(dlg.lastCreateCertErrorForTesting().isEmpty());
    EXPECT_FALSE(std::filesystem::exists(out));
    EXPECT_FALSE(dlg.lastCreateCertErrorForTesting()
                     .contains(QString::fromUtf8(kDummyPass)))
        << "the create-cert error must not contain the passphrase";
    EXPECT_TRUE(dlg.findChild<QLineEdit*>("pfxPathEdit")->text().isEmpty());
}
