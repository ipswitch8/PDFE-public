// -*- coding: utf-8 -*-
//
// GitHub #362 — the SignDialog integration for the persisted signature-image
// library. test_signature_image_library.cpp proves the LIBRARY persists; this
// proves the DIALOG wiring is load-bearing: saving the currently-browsed image
// into the library and re-selecting a saved one actually flows into the image
// path that signingOptions() hands to the signing stack.
//
// It drives the REAL production slots — onSaveImageToLibrary() via the save
// button, onSelectSavedImage() via selectSavedSignatureImageForTesting() — and
// reads the public signingOptions(). The modal "name this image" QInputDialog
// is bypassed with setSaveImageNameForTesting() (the production-inert seam);
// nothing here reimplements the dialog's logic. QSettings is redirected to a
// purge-on-exit test hive so the real user store is never touched.
#include <gtest/gtest.h>

#include "ui/SignDialog.h"
#include "signatures/SignatureData.h"
#include "support/SettingsIsolation.h"

#include <QApplication>
#include <QDir>
#include <QImage>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QString>
#include <QTemporaryDir>

#include <string>

using pdfe::ui::SignDialog;

// Save a browsed image into the library, then re-select it: the re-selected
// image must become the signingOptions() image path (a usable PNG that decodes
// to the saved pixels), and a FRESH dialog in the same store must list it —
// the across-sessions guarantee reaching the dialog, not just the library.
TEST(SignDialogImageLibrary, SaveThenReselectFlowsIntoSigningOptions)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";

    pdfe_test::SettingsScope settingsScope(
        QStringLiteral("PDFE-Test"),
        QStringLiteral("SignDialogImageLibraryTest"),
        /*purgeOnExit=*/true);
    QSettings::setDefaultFormat(QSettings::IniFormat);
    { QSettings cleanup; cleanup.clear(); }

    // A known source image on disk (as a real browse would produce).
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString src = QDir(dir.path()).filePath(QStringLiteral("sig.png"));
    QImage orig(12, 6, QImage::Format_ARGB32);
    orig.fill(qRgb(7, 133, 201));
    ASSERT_TRUE(orig.save(src, "PNG"));

    SignDialog dlg;
    auto* pathEdit = dlg.findChild<QLineEdit*>(QStringLiteral("imagePathEdit"));
    auto* saveBtn =
        dlg.findChild<QPushButton*>(QStringLiteral("saveSignatureImageButton"));
    ASSERT_NE(pathEdit, nullptr);
    ASSERT_NE(saveBtn, nullptr) << "the Save-signature-image control must exist";

    // Simulate a browsed image. imagePathEdit_ is read-only to the USER, but a
    // programmatic setText (what onBrowseImage does) still populates it.
    pathEdit->setText(src);

    // Save into the library under a name (seam skips the modal name prompt),
    // driving the real onSaveImageToLibrary() through the button.
    dlg.setSaveImageNameForTesting(QString(QStringLiteral("my-sig")));
    saveBtn->click();
    EXPECT_EQ(dlg.savedSignatureImageCountForTesting(), 1)
        << "saving a browsed image must add exactly one library entry";

    // Clear the field, then re-select the saved image via the real
    // onSelectSavedImage() path.
    pathEdit->setText(QString());
    ASSERT_TRUE(dlg.selectSavedSignatureImageForTesting(
        QString(QStringLiteral("my-sig"))))
        << "re-select of a saved signature image must succeed";

    // THE WIRING CLAIM: the re-selected image reaches signingOptions().
    const auto opts = dlg.signingOptions();
    ASSERT_FALSE(opts.imagePath.empty())
        << "GitHub #362: re-selecting a saved signature image did not repopulate "
           "the image path signingOptions() uses — the dialog wiring is broken";
    QImage reloaded(QString::fromStdString(opts.imagePath));
    ASSERT_FALSE(reloaded.isNull())
        << "signingOptions().imagePath must point at a decodable image";
    // Pixel round-trip proves it is the SAVED image, not a placeholder/empty.
    const QRgb px = reloaded.pixel(0, 0);
    EXPECT_EQ(qRed(px), 7);
    EXPECT_EQ(qGreen(px), 133);
    EXPECT_EQ(qBlue(px), 201);

    // Across sessions, reaching the DIALOG: a fresh SignDialog constructed
    // against the same settings store loads the library and lists the entry.
    SignDialog dlg2;
    EXPECT_EQ(dlg2.savedSignatureImageCountForTesting(), 1)
        << "a new dialog session must load the previously saved signature image";
}
