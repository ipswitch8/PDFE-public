// -*- coding: utf-8 -*-
//
// GitHub #372 — dismissing the signing error dialog must not crash, and a
// failed sign must leave the signature-field count at its pre-flow baseline.
//
// Three stacked defects in MainWindow::signTargetField / onSignDocument:
//   1. (UAF) the success path rebuilds tileCache_/tileRenderer_ and re-binds
//      the canvas after the sign-reopen; the catch path returned with NO
//      rebuild, so any failure after `document_ = Document::open(...)` left
//      tileRenderer_ holding a freed Document* — first repaint after the
//      error dialog faulted (0xC0000005).
//   2. The wrong-passphrase probe returned into an UNGUARDED rollback
//      (undoManager_->undo() can fz_throw out of removeSignatureField and
//      escape the Qt slot to std::terminate).
//   3. If the failure hit after the reopen, the undo stack was already
//      cleared, so the caller's canUndo() rollback silently no-oped and a
//      stray unsigned SIGN field remained.
//
// These tests drive the REAL MainWindow flow through the #360 seam, extended
// with suppressErrorDialogs (the QMessageBox::critical modals blocked any
// headless run of these exact paths — which is why they had zero coverage)
// and an injectable imagePath (a nonexistent appearance image makes signField
// throw AFTER the reopen: precisely the #372/#374 failure shape).
//
// Modeled on test_ui_sign_cancel_rollback.cpp. Only the dummy committed
// test_signer.pfx ("test123") is used; the deliberately WRONG passphrase
// below is an obvious non-secret.

#include <gtest/gtest.h>

#include "ui/MainWindow.h"
#include "ui/PdfCanvas.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "signatures/SignatureManager.h"
#include "signatures/SignatureData.h"

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

class SignErrorRecoveryTest : public ::testing::Test {
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
                   / "sign_error_work.pdf";
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

// (A) Wrong passphrase: the PFX probe fails BEFORE any save/reopen. The flow
// must surface the error (recorded, dialog suppressed), run the now-GUARDED
// rollback so the field count returns to baseline (defect 2), and the app
// must survive a subsequent full repaint/refresh cycle.
TEST_F(SignErrorRecoveryTest, WrongPassphraseRollsBackAndSurvives)
{
    const int baseline = mw_->signatureFieldCountForTesting();
    ASSERT_EQ(0, baseline) << "fixture should start with no signature fields";

    const auto outPdf = std::filesystem::path(tempDir_->path().toStdString())
                        / "sign_error_wrongpass.pdf";

    pdfe::ui::MainWindow::SignFlowTestInputs inputs;
    inputs.dialogAccepted = true;
    inputs.pfxPath = QString::fromStdString(pfx_.string());
    inputs.pfxPassword = QStringLiteral("definitely-not-the-passphrase");
    inputs.savePath = QString::fromStdString(outPdf.string());
    inputs.suppressErrorDialogs = true;   // GitHub #372 seam
    mw_->setSignFlowTestInputsForTesting(inputs);

    mw_->triggerSignDocumentForTesting();
    QApplication::processEvents();

    // PREMISE: the failure really was the PFX probe — not some later stage.
    // The probe's error text is the only signing error that names the PFX
    // credentials; a save/sign-stage failure records the generic text.
    const QString err = mw_->lastSigningErrorForTesting();
    ASSERT_FALSE(err.isEmpty()) << "the failed probe must record an error";
    ASSERT_TRUE(err.contains(QStringLiteral("PFX")))
        << "premise: the recorded error must be the PFX-probe failure, got: "
        << err.toStdString();

    // The probe failed before any save: nothing was ever written.
    EXPECT_FALSE(std::filesystem::exists(outPdf))
        << "a wrong-passphrase attempt must fail before the save";

    // Defect 2's oracle: the guarded rollback ran and restored the baseline.
    EXPECT_EQ(baseline, mw_->signatureFieldCountForTesting())
        << "a failed sign must return the field count to its pre-flow baseline";

    // Survival: a full refresh + event-loop turn after the error path. The
    // document was never replaced on this path, so the canvas binding must
    // still be the live document.
    auto* canvas = mw_->canvasForTesting();
    ASSERT_NE(canvas, nullptr);
    EXPECT_EQ(canvas->document(), mw_->documentForTesting())
        << "the canvas must still be bound to the live document";
    canvas->refreshAllPages();
    QApplication::processEvents();
    EXPECT_NE(mw_->documentForTesting(), nullptr);
}

// (B) Failure AFTER the sign-reopen (nonexistent appearance image makes
// signField's fz_new_image_from_file throw — sibling #374's exact shape).
// Pre-fix: tileRenderer_ kept the freed pre-sign Document* (defect 1, UAF on
// next repaint) and the stray unsigned field survived because the undo stack
// was already cleared (defect 3). Post-fix: the catch path performs the same
// reopen + rebuild + rebind as the success path and removes the stray field.
TEST_F(SignErrorRecoveryTest, PostReopenFailureRebuildsCanvasAndRemovesStrayField)
{
    const int baseline = mw_->signatureFieldCountForTesting();
    ASSERT_EQ(0, baseline);

    const auto outPdf = std::filesystem::path(tempDir_->path().toStdString())
                        / "sign_error_postreopen.pdf";
    const auto missingImage =
        std::filesystem::path(tempDir_->path().toStdString())
        / "no_such_signature_image.png";
    // PREMISE: the injected failure is the image load, and it CAN only fire
    // inside signField — i.e. after the save + reopen replaced document_.
    ASSERT_FALSE(std::filesystem::exists(missingImage));

    pdfe::ui::MainWindow::SignFlowTestInputs inputs;
    inputs.dialogAccepted = true;
    inputs.pfxPath = QString::fromStdString(pfx_.string());
    inputs.pfxPassword = QStringLiteral("test123");   // probe SUCCEEDS
    inputs.savePath = QString::fromStdString(outPdf.string());
    inputs.imagePath = QString::fromStdString(missingImage.string());
    inputs.suppressErrorDialogs = true;
    mw_->setSignFlowTestInputsForTesting(inputs);

    mw_->triggerSignDocumentForTesting();
    QApplication::processEvents();

    // PREMISE: the flow got past the probe (the PFX text would mean it never
    // reached the post-reopen stage) and did fail.
    const QString err = mw_->lastSigningErrorForTesting();
    ASSERT_FALSE(err.isEmpty())
        << "the post-reopen signField failure must record an error";
    ASSERT_FALSE(err.contains(QStringLiteral("PFX")))
        << "premise violated: the probe failed, so the injected post-reopen "
           "failure was never reached";
    // The save DID happen before signField threw — that is what makes this
    // the post-reopen shape.
    ASSERT_TRUE(std::filesystem::exists(outPdf))
        << "premise: the failure must occur AFTER the save/reopen";

    // Defect 1's oracle: the recovery rebuild ran — the canvas is bound to
    // the CURRENT (reopened) document, not the freed pre-sign one, and a
    // full repaint/refresh cycle completes.
    auto* canvas = mw_->canvasForTesting();
    ASSERT_NE(canvas, nullptr);
    ASSERT_NE(mw_->documentForTesting(), nullptr)
        << "recovery must leave a live document bound";
    EXPECT_EQ(canvas->document(), mw_->documentForTesting())
        << "GitHub #372: the catch path must rebind the canvas to the "
           "reopened document exactly as the success path does";
    canvas->refreshAllPages();
    QApplication::processEvents();

    // Defect 3's oracle: no stray unsigned SIGN field remains.
    EXPECT_EQ(baseline, mw_->signatureFieldCountForTesting())
        << "GitHub #372: a post-reopen failure must not leave the stray "
           "unsigned field behind (the undo stack was already cleared)";
}
