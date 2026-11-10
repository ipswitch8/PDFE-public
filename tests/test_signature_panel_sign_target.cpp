// -*- coding: utf-8 -*-
//
// GitHub #375 — the SignaturePanel's Sign button emits
// signRequested(pageIndex, widgetIndex) for the SELECTED row, but the
// MainWindow handler Q_UNUSED both indices and called onSignDocument(), which
// signs whichever unsigned field listSignatures() returns FIRST. With two
// unsigned fields, requesting a sign on the second silently signed the first.
//
// These tests build a document with TWO unsigned signature fields, emit the
// panel's real signRequested signal for the SECOND one (the exact signal
// SignaturePanel::onSignClicked() emits), and assert the SECOND field is the
// one signed while the FIRST remains unsigned.
//
// Modeled on test_ui_sign_error_recovery.cpp (sign-flow fixture; only the
// dummy committed test_signer.pfx, passphrase "test123").

#include <gtest/gtest.h>

#include "ui/MainWindow.h"
#include "ui/SignaturePanel.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "signatures/SignatureData.h"
#include "signatures/SignatureManager.h"

#include <QApplication>
#include <QTemporaryDir>

#include <filesystem>
#include <memory>

namespace {

std::filesystem::path fixturesDir()
{
    return std::filesystem::path(TEST_FIXTURES_DIR);
}

} // namespace

class SignaturePanelSignTargetTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        const auto src = fixturesDir() / "test_single.pdf";
        pfx_ = fixturesDir() / "test_signer.pfx";
        if (!std::filesystem::exists(src)) {
            GTEST_SKIP() << "test_single.pdf fixture not available";
        }
        if (!std::filesystem::exists(pfx_)) {
            GTEST_SKIP() << "test_signer.pfx fixture not available";
        }

        tempDir_ = std::make_unique<QTemporaryDir>();
        ASSERT_TRUE(tempDir_->isValid());
        workPdf_ = std::filesystem::path(tempDir_->path().toStdString())
                   / "panel_two_fields.pdf";

        // Build the two-unsigned-field document through the real
        // SignatureManager, then persist it so MainWindow opens it cold.
        {
            auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
            auto doc = pdfe::core::Document::open(ctx, src);
            pdfe::signatures::SignatureManager mgr(ctx);
            auto page = doc->page(0);
            ASSERT_NE(page, nullptr);
            mgr.createSignatureField(page.get(), "SigOne",
                                     QRectF(100, 560, 180, 50));
            mgr.createSignatureField(page.get(), "SigTwo",
                                     QRectF(320, 560, 180, 50));
            page.reset();
            doc->saveAs(workPdf_);
        }

        mw_ = std::make_unique<pdfe::ui::MainWindow>();
        QApplication::processEvents();
        mw_->openFile(QString::fromStdString(workPdf_.string()));
        QApplication::processEvents();
    }

    void TearDown() override
    {
        if (mw_) {
            mw_->setSignFlowTestInputsForTesting(std::nullopt);
        }
        mw_.reset();
        QApplication::processEvents();
        tempDir_.reset();
    }

    // Find a signature by field name in the CURRENT document's listing.
    // Returns false when absent.
    bool findSig(const std::string& name,
                 pdfe::signatures::SignatureData& out) const
    {
        auto* mgr = mw_->signatureManagerForTesting();
        if (!mgr || !mw_->documentForTesting()) return false;
        for (const auto& s : mgr->listSignatures(mw_->documentForTesting())) {
            if (s.fieldName == name) {
                out = s;
                return true;
            }
        }
        return false;
    }

    std::unique_ptr<QTemporaryDir> tempDir_;
    std::filesystem::path workPdf_;
    std::filesystem::path pfx_;
    std::unique_ptr<pdfe::ui::MainWindow> mw_;
};

TEST_F(SignaturePanelSignTargetTest, PanelSignRequestSignsTheClickedFieldNotTheFirstUnsigned)
{
    // PREMISE: two signature fields, BOTH unsigned, and SigOne enumerates
    // before SigTwo (the bug needs a "first unsigned" that is NOT the one
    // requested).
    ASSERT_EQ(2, mw_->signatureFieldCountForTesting());
    pdfe::signatures::SignatureData one, two;
    ASSERT_TRUE(findSig("SigOne", one));
    ASSERT_TRUE(findSig("SigTwo", two));
    ASSERT_FALSE(one.isSigned) << "premise: SigOne must start unsigned";
    ASSERT_FALSE(two.isSigned) << "premise: SigTwo must start unsigned";
    {
        auto* mgr = mw_->signatureManagerForTesting();
        const auto sigs = mgr->listSignatures(mw_->documentForTesting());
        ASSERT_GE(sigs.size(), 2u);
        ASSERT_EQ(sigs[0].fieldName, "SigOne")
            << "premise: SigOne must be the FIRST unsigned field the old "
               "handler would have signed";
    }

    const auto outPdf = std::filesystem::path(tempDir_->path().toStdString())
                        / "panel_signed_second.pdf";
    pdfe::ui::MainWindow::SignFlowTestInputs inputs;
    inputs.dialogAccepted = true;
    inputs.pfxPath = QString::fromStdString(pfx_.string());
    inputs.pfxPassword = QStringLiteral("test123");
    inputs.savePath = QString::fromStdString(outPdf.string());
    inputs.suppressErrorDialogs = true;
    mw_->setSignFlowTestInputsForTesting(inputs);

    // The panel's Sign button for the SigTwo row emits exactly this signal
    // (SignaturePanel::onSignClicked). Emitting it drives the REAL
    // MainWindow connection — the code under test.
    auto* panel = mw_->signaturePanelForTesting();
    ASSERT_NE(panel, nullptr);
    emit panel->signRequested(two.pageIndex, two.widgetIndex);
    QApplication::processEvents();

    EXPECT_TRUE(mw_->lastSigningErrorForTesting().isEmpty())
        << "signing the requested field must succeed; got: "
        << mw_->lastSigningErrorForTesting().toStdString();
    ASSERT_TRUE(std::filesystem::exists(outPdf));

    // THE #375 ORACLE: the SECOND (requested) field is signed; the FIRST
    // (the one the old handler signed) remains unsigned.
    pdfe::signatures::SignatureData oneAfter, twoAfter;
    ASSERT_TRUE(findSig("SigOne", oneAfter));
    ASSERT_TRUE(findSig("SigTwo", twoAfter));
    EXPECT_TRUE(twoAfter.isSigned)
        << "GitHub #375: the panel-requested field (SigTwo) must be the one "
           "signed";
    EXPECT_FALSE(oneAfter.isSigned)
        << "GitHub #375: the FIRST unsigned field must NOT be signed when "
           "the request named another field";
}

// Guard direction: a sign request for an ALREADY-SIGNED row must be refused
// outright — it must neither re-sign that field nor drift onto some other
// unsigned field (the #375 failure shape through another door).
TEST_F(SignaturePanelSignTargetTest, SignRequestOnASignedRowIsRefused)
{
    pdfe::signatures::SignatureData one, two;
    ASSERT_TRUE(findSig("SigOne", one));
    ASSERT_TRUE(findSig("SigTwo", two));
    ASSERT_FALSE(one.isSigned);
    ASSERT_FALSE(two.isSigned);

    // First, legitimately sign SigTwo through the panel path.
    const auto outPdf = std::filesystem::path(tempDir_->path().toStdString())
                        / "panel_signed_once.pdf";
    pdfe::ui::MainWindow::SignFlowTestInputs inputs;
    inputs.dialogAccepted = true;
    inputs.pfxPath = QString::fromStdString(pfx_.string());
    inputs.pfxPassword = QStringLiteral("test123");
    inputs.savePath = QString::fromStdString(outPdf.string());
    inputs.suppressErrorDialogs = true;
    mw_->setSignFlowTestInputsForTesting(inputs);

    auto* panel = mw_->signaturePanelForTesting();
    ASSERT_NE(panel, nullptr);
    emit panel->signRequested(two.pageIndex, two.widgetIndex);
    QApplication::processEvents();
    pdfe::signatures::SignatureData twoSigned;
    ASSERT_TRUE(findSig("SigTwo", twoSigned));
    ASSERT_TRUE(twoSigned.isSigned) << "premise: the first sign must commit";

    // Now request a sign on the SIGNED row. The refusal must happen BEFORE
    // the flow consumes the seam: a second save target must never be created
    // and SigOne must stay unsigned.
    const auto outPdf2 = std::filesystem::path(tempDir_->path().toStdString())
                         / "panel_resign_refused.pdf";
    inputs.savePath = QString::fromStdString(outPdf2.string());
    mw_->setSignFlowTestInputsForTesting(inputs);

    emit panel->signRequested(twoSigned.pageIndex, twoSigned.widgetIndex);
    QApplication::processEvents();

    EXPECT_FALSE(std::filesystem::exists(outPdf2))
        << "a sign request on an already-signed row must be refused before "
           "any save happens";
    pdfe::signatures::SignatureData oneAfter;
    ASSERT_TRUE(findSig("SigOne", oneAfter));
    EXPECT_FALSE(oneAfter.isSigned)
        << "a refused request must not drift onto the first unsigned field";
}
