// -*- coding: utf-8 -*-
//
// GitHub #360 — a CANCELLED "Sign Document" dialog must not leave a stray
// signature field (the red "SIGN" widget annotation) on the page.
//
// Before the fix, MainWindow::onSignDocument() CREATED and COMMITTED a new
// signature field (via CreateSignatureFieldCommand) and only THEN showed the
// modal Sign dialog in signTargetField(); on Cancel it returned without rolling
// the field back, so the SIGN marker stayed on the page. These tests drive the
// REAL MainWindow sign flow end-to-end — no logic is reimplemented here — via a
// headless seam (setSignFlowTestInputsForTesting) that injects the dialog
// result and the Save-As path in place of the two modal exec()s a headless run
// cannot answer.
//
// Oracle: the signature-field widget count on the document, read through the
// real SignatureManager / AcroForm enumeration (NOT a UI pixel check).
//
//   * CancelLeavesNoSignatureField    — the #360 regression; FAILS pre-fix
//                                        (a stray field remains, count 0 -> 1).
//   * AcceptLeavesExactlyOneSignature  — positive control: a completed sign
//                                        keeps exactly one (now signed) field,
//                                        so the rollback is not over-eager.
//
// Security: the only key material used is the committed, obviously-dummy test
// fixture test_signer.pfx (passphrase "test123"); no private key, passphrase,
// or path is ever logged, and the signed output lands only in a QTemporaryDir.

#include <gtest/gtest.h>

#include "ui/MainWindow.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "signatures/SignatureManager.h"
#include "signatures/SignatureData.h"

#include <QApplication>
#include <QTemporaryDir>

#include <filesystem>
#include <memory>
#include <vector>

namespace {

std::filesystem::path fixturesDir()
{
    return std::filesystem::path(TEST_FIXTURES_DIR);
}

} // namespace

class SignCancelRollbackTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        const auto src = fixturesDir() / "test_single.pdf";
        pfx_ = fixturesDir() / "test_signer.pfx";
        if (!std::filesystem::exists(src)) {
            GTEST_SKIP() << "test_single.pdf fixture not available";
        }

        tempDir_ = std::make_unique<QTemporaryDir>();
        ASSERT_TRUE(tempDir_->isValid());
        workPdf_ = std::filesystem::path(tempDir_->path().toStdString())
                   / "sign_cancel_work.pdf";
        std::filesystem::copy_file(src, workPdf_);

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

    std::unique_ptr<QTemporaryDir> tempDir_;
    std::filesystem::path workPdf_;
    std::filesystem::path pfx_;
    std::unique_ptr<pdfe::ui::MainWindow> mw_;
};

// #360 regression. A fresh document has no signature field. Running the Sign
// Document action and CANCELLING the dialog must return the count to the
// pre-sign baseline — the field created to receive the signature is rolled
// back, so no red "SIGN" marker is left behind. FAILS pre-fix (the field was
// committed before the dialog and never removed on cancel).
TEST_F(SignCancelRollbackTest, CancelLeavesNoSignatureField)
{
    const int baseline = mw_->signatureFieldCountForTesting();
    ASSERT_EQ(0, baseline) << "fixture should start with no signature fields";

    pdfe::ui::MainWindow::SignFlowTestInputs inputs;
    inputs.dialogAccepted = false;   // user cancels the Sign dialog
    mw_->setSignFlowTestInputsForTesting(inputs);

    mw_->triggerSignDocumentForTesting();
    QApplication::processEvents();

    EXPECT_EQ(baseline, mw_->signatureFieldCountForTesting())
        << "a cancelled Sign dialog left a stray signature field on the page";
}

// Belt-and-braces for the save-path abort: even if the Sign dialog is ACCEPTED,
// cancelling the subsequent Save-As dialog (no path chosen) must also roll the
// speculatively-created field back — the document was never saved/signed.
TEST_F(SignCancelRollbackTest, AcceptThenSaveCancelLeavesNoSignatureField)
{
    const int baseline = mw_->signatureFieldCountForTesting();
    ASSERT_EQ(0, baseline);

    pdfe::ui::MainWindow::SignFlowTestInputs inputs;
    inputs.dialogAccepted = true;
    inputs.pfxPath = QString::fromStdString(pfx_.string());
    inputs.pfxPassword = QStringLiteral("test123");
    inputs.savePath = QString();   // Save-As cancelled
    mw_->setSignFlowTestInputsForTesting(inputs);

    mw_->triggerSignDocumentForTesting();
    QApplication::processEvents();

    EXPECT_EQ(baseline, mw_->signatureFieldCountForTesting())
        << "cancelling Save-As after accepting the Sign dialog left a stray field";
}

// Positive control: a COMPLETED sign (dialog accepted, valid PFX, a Save-As
// path chosen) must keep exactly one signature field, and it must be signed.
// This proves the #360 rollback is scoped to aborts and never removes a field
// that was legitimately signed.
TEST_F(SignCancelRollbackTest, AcceptLeavesExactlyOneSignedSignature)
{
    if (!std::filesystem::exists(pfx_)) {
        GTEST_SKIP() << "test_signer.pfx fixture not available";
    }
    ASSERT_EQ(0, mw_->signatureFieldCountForTesting());

    const auto outPdf = std::filesystem::path(tempDir_->path().toStdString())
                        / "sign_cancel_signed.pdf";

    pdfe::ui::MainWindow::SignFlowTestInputs inputs;
    inputs.dialogAccepted = true;
    inputs.pfxPath = QString::fromStdString(pfx_.string());
    inputs.pfxPassword = QStringLiteral("test123");
    inputs.savePath = QString::fromStdString(outPdf.string());
    mw_->setSignFlowTestInputsForTesting(inputs);

    mw_->triggerSignDocumentForTesting();
    QApplication::processEvents();

    // The live document (reopened from the signed output) carries exactly one
    // signature field.
    EXPECT_EQ(1, mw_->signatureFieldCountForTesting())
        << "a completed sign should leave exactly one signature field";

    // Independent oracle: reopen the written file with a fresh manager and
    // confirm the one field is actually signed.
    ASSERT_TRUE(std::filesystem::exists(outPdf))
        << "sign flow did not write the signed output";
    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    auto mgr = std::make_unique<pdfe::signatures::SignatureManager>(ctx);
    auto doc = pdfe::core::Document::open(ctx, outPdf);
    ASSERT_NE(doc, nullptr);
    EXPECT_EQ(1, mgr->countSignatures(doc.get()));
    const auto sigs = mgr->listSignatures(doc.get());
    ASSERT_EQ(1u, sigs.size());
    EXPECT_TRUE(sigs[0].isSigned)
        << "the field left by a completed sign should be signed";
}
