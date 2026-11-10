// -*- coding: utf-8 -*-
//
// GitHub #374 — a signature image re-selected from the persisted library is
// materialised to a QTemporaryFile with autoRemove, but the SignDialog is
// destroyed (promptSigningOptions returns the options by value and the dialog
// leaves scope) BEFORE SignatureManager::signField reads
// SigningOptions::imagePath via fz_new_image_from_file. Pre-fix the file was
// deleted with the dialog, so signing with a library image ALWAYS threw
// post-reopen (and then landed in the — now fixed — #372 recovery path).
//
// Fix under test: SigningOptions::imageKeepalive (shared_ptr<QTemporaryFile>)
// carries ownership of the temp file out of the dialog, preserving autoRemove
// semantics (the file dies with the last SigningOptions copy — no litter).
//
// Modeled on test_sign_dialog_image_library.cpp (dialog/library seams) and
// test_ui_sign_error_recovery.cpp (MainWindow sign-flow fixture). Only the
// dummy committed test_signer.pfx ("test123") is used.

#include <gtest/gtest.h>

#include "ui/MainWindow.h"
#include "ui/SignDialog.h"
#include "core/Document.h"
#include "signatures/SignatureData.h"
#include "signatures/SignatureManager.h"
#include "support/SettingsIsolation.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>

#include <filesystem>
#include <memory>

using pdfe::ui::SignDialog;

namespace {

std::filesystem::path fixturesDir()
{
    return std::filesystem::path(TEST_FIXTURES_DIR);
}

// Seed the library with a saved image through the REAL dialog slots, then
// re-select it and return the resulting SigningOptions. Runs entirely inside
// the caller-provided dialog's lifetime.
pdfe::signatures::SigningOptions selectLibraryImage(SignDialog& dlg,
                                                    const QString& srcPng,
                                                    const QString& name)
{
    auto* pathEdit = dlg.findChild<QLineEdit*>(QStringLiteral("imagePathEdit"));
    auto* saveBtn =
        dlg.findChild<QPushButton*>(QStringLiteral("saveSignatureImageButton"));
    EXPECT_NE(pathEdit, nullptr);
    EXPECT_NE(saveBtn, nullptr);
    if (!pathEdit || !saveBtn) return {};

    pathEdit->setText(srcPng);
    dlg.setSaveImageNameForTesting(QString(name));
    saveBtn->click();
    EXPECT_GE(dlg.savedSignatureImageCountForTesting(), 1);

    // Clear the field so the re-select is what populates it.
    pathEdit->setText(QString());
    EXPECT_TRUE(dlg.selectSavedSignatureImageForTesting(QString(name)));
    return dlg.signingOptions();
}

} // namespace

// Mechanism half: the materialised temp PNG must survive the dialog's
// destruction for as long as the SigningOptions live — and no longer
// (autoRemove still prevents temp litter).
TEST(SignLibraryImageLifetime, TempImageOutlivesTheDialogAndDiesWithTheOptions)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";

    pdfe_test::SettingsScope settingsScope(
        QStringLiteral("PDFE-Test"),
        QStringLiteral("SignLibraryImageLifetimeTest"),
        /*purgeOnExit=*/true);
    QSettings::setDefaultFormat(QSettings::IniFormat);
    { QSettings cleanup; cleanup.clear(); }

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString src = QDir(dir.path()).filePath(QStringLiteral("sig.png"));
    QImage orig(16, 8, QImage::Format_ARGB32);
    orig.fill(qRgb(40, 90, 160));
    ASSERT_TRUE(orig.save(src, "PNG"));

    pdfe::signatures::SigningOptions opts;
    {
        SignDialog dlg;
        opts = selectLibraryImage(dlg, src, QStringLiteral("keepalive"));
        ASSERT_FALSE(opts.imagePath.empty());
        // PREMISE: the re-selected path is the MATERIALISED temp file, not
        // the browsed source — the #374 mechanism only exists for the temp.
        ASSERT_NE(opts.imagePath, src.toStdString())
            << "premise: re-select must materialise a temp copy";
        ASSERT_TRUE(
            QFile::exists(QString::fromStdString(opts.imagePath)))
            << "premise: the temp image must exist while the dialog lives";
        ASSERT_NE(opts.imageKeepalive, nullptr)
            << "GitHub #374: signingOptions() must hand the temp file's "
               "ownership along with its path";
    }   // <- SignDialog destroyed here, exactly as in promptSigningOptions()

    const QString tmpPath = QString::fromStdString(opts.imagePath);
    EXPECT_TRUE(QFile::exists(tmpPath))
        << "GitHub #374: the library temp image was deleted with the dialog, "
           "before signField could read it";

    // autoRemove semantics preserved: dropping the last SigningOptions copy
    // (the only keepalive holder) must remove the temp file — no litter.
    opts = pdfe::signatures::SigningOptions{};
    EXPECT_FALSE(QFile::exists(tmpPath))
        << "the temp image must be auto-removed once the last SigningOptions "
           "copy releases it";
}

// End-to-end half: the REAL MainWindow sign flow, with the dialog destroyed
// before signing (as production's promptSigningOptions does), must SUCCEED
// when the appearance image is a library-selected temp file. Pre-fix this
// failed post-reopen with the generic signing error (#372's recovery shape).
TEST(SignLibraryImageLifetime, SigningWithALibraryImageSucceedsAfterTheDialogIsDestroyed)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";

    const auto srcPdf = fixturesDir() / "test_single.pdf";
    const auto pfx = fixturesDir() / "test_signer.pfx";
    if (!std::filesystem::exists(srcPdf)) {
        GTEST_SKIP() << "test_single.pdf fixture not available";
    }
    if (!std::filesystem::exists(pfx)) {
        GTEST_SKIP() << "test_signer.pfx fixture not available";
    }

    pdfe_test::SettingsScope settingsScope(
        QStringLiteral("PDFE-Test"),
        QStringLiteral("SignLibraryImageLifetimeE2E"),
        /*purgeOnExit=*/true);
    QSettings::setDefaultFormat(QSettings::IniFormat);
    { QSettings cleanup; cleanup.clear(); }

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString src = QDir(dir.path()).filePath(QStringLiteral("sig.png"));
    QImage orig(32, 12, QImage::Format_ARGB32);
    orig.fill(qRgb(10, 120, 60));
    ASSERT_TRUE(orig.save(src, "PNG"));

    const auto workPdf =
        std::filesystem::path(dir.path().toStdString()) / "lib_image_work.pdf";
    std::filesystem::copy_file(srcPdf, workPdf);
    const auto outPdf =
        std::filesystem::path(dir.path().toStdString()) / "lib_image_signed.pdf";

    auto mw = std::make_unique<pdfe::ui::MainWindow>();
    QApplication::processEvents();
    mw->openFile(QString::fromStdString(workPdf.string()));
    QApplication::processEvents();
    ASSERT_EQ(0, mw->signatureFieldCountForTesting());

    // Production lifetime, replicated: the dialog lives only long enough to
    // produce the options; the sign itself runs AFTER its destruction.
    pdfe::signatures::SigningOptions opts;
    {
        SignDialog dlg;
        opts = selectLibraryImage(dlg, src, QStringLiteral("e2e"));
        ASSERT_FALSE(opts.imagePath.empty());
        ASSERT_NE(opts.imagePath, src.toStdString());
    }

    // THE #374 ASSERT: the image path must still be readable at signField
    // time, i.e. after the dialog is gone. Pre-fix it is already deleted
    // right here and the sign below failed post-reopen.
    const QString tmpPath = QString::fromStdString(opts.imagePath);
    ASSERT_TRUE(QFile::exists(tmpPath))
        << "GitHub #374: the library image must survive the dialog so "
           "signField can read it";

    pdfe::ui::MainWindow::SignFlowTestInputs inputs;
    inputs.dialogAccepted = true;
    inputs.pfxPath = QString::fromStdString(pfx.string());
    inputs.pfxPassword = QStringLiteral("test123");
    inputs.savePath = QString::fromStdString(outPdf.string());
    inputs.imagePath = tmpPath;     // the library temp image
    inputs.suppressErrorDialogs = true;
    mw->setSignFlowTestInputsForTesting(inputs);

    mw->triggerSignDocumentForTesting();
    QApplication::processEvents();

    // Pre-fix direction: the generic post-reopen signing error fired. The
    // fixed flow records NO error and the signed document exists.
    EXPECT_TRUE(mw->lastSigningErrorForTesting().isEmpty())
        << "signing with a library image must not fail; got: "
        << mw->lastSigningErrorForTesting().toStdString();
    EXPECT_TRUE(std::filesystem::exists(outPdf));

    // Green-direction oracle: the reopened document carries exactly one
    // SIGNED signature (the appearance image was consumed by signField).
    auto* mgr = mw->signatureManagerForTesting();
    ASSERT_NE(mgr, nullptr);
    ASSERT_NE(mw->documentForTesting(), nullptr);
    const auto sigs = mgr->listSignatures(mw->documentForTesting());
    ASSERT_EQ(sigs.size(), 1u);
    EXPECT_TRUE(sigs[0].isSigned)
        << "the library-image sign flow must actually sign the field";

    mw->setSignFlowTestInputsForTesting(std::nullopt);
    mw.reset();
    QApplication::processEvents();
}
