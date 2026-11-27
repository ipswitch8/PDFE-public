// -*- coding: utf-8 -*-
//
// "Sign" without a certificate: a VISUAL signature (user report, 2026-11-26:
// "no option to sign without a PFX file (i.e. add the image and timestamp
// without certificate)").
//
// WHAT A VISUAL SIGNATURE IS, AND IS NOT
// --------------------------------------
// It places the user's signature image and the date and time on the page, as
// an image /Stamp annotation. It is NOT a digital signature: nothing is signed
// cryptographically, no /Sig value or /ByteRange is written, nothing proves
// who placed it, and later changes are not detected. The Sign dialog says so
// next to the option, and the stamp's /Contents says so too, so a reader who
// inspects the annotation is not misled either.
//
// HOW THE TESTS DRIVE IT
// ----------------------
// The end-to-end test opens the REAL Sign dialog from MainWindow's Sign
// Document action and answers it with the dialog-introspection action script
// (tick the visual option, set the image, press OK). No PFX is supplied. It
// then saves through the real Save action and reopens the file with a fresh
// Document to read what was actually written.
//
// MUTATIONS KILLED (each applied, pdfe_tests rebuilt, VisualSignature* and
// ImageStampThroughMainWindow.* run; 2026-11-26, Linux Release):
//   * onSignDocument's visual branch removed -> PlacesTheImageAndDate...
//   * signTargetField's visual branch removed -> OverAnExistingUnsignedField...
//   * the image not re-applied after MainWindow's re-create -> both
//     end-to-end tests and ImageStampThroughMainWindow (DCA 0130)
//   * StampTool not handing its image on -> ImageStampThroughMainWindow
//   * SignDialog::accept() ignoring visual mode -> VisualModeNeedsAnImage...
//     and PlacesTheImageAndDate...
//   * the dialog notice no longer saying "not a digital signature" ->
//     TheOptionIsLabelledAsNotADigitalSignature
//   * the stamp /Contents without the disclaimer -> PlacesTheImageAndDate...
//   * CreateAnnotationCommand::undo() without its snapshot, redo() without
//     re-applying the image, or MainWindow not handing the command the image
//     -> UndoThenRedoKeepsTheImageAndTheDisclaimer (each)
//   * the appearance composed for the requested rect instead of the stamp's
//     own -> PlacesTheImageAndDate... (aspect check)
//   * the date line or the disclaimer line not drawn ->
//     TheCaptionIsDrawnBelowTheImageAndTheSizeFollowsTheStamp (each; a first
//     single-band version of that test let the date-line removal survive)
//   * the caption without "not a digital signature" ->
//     TheCaptionSaysWhenAndThatItIsNotADigitalSignature

#include <gtest/gtest.h>

#include "annotations/AnnotationManager.h"
#include "annotations/AnnotationType.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "signatures/SignatureData.h"
#include "signatures/SignatureManager.h"
#include "ui/MainWindow.h"
#include "ui/SignDialog.h"
#include "ui/TestDialogSeam.h"
#include "ui/VisualSignature.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDate>
#include <QDialog>
#include <QElapsedTimer>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

namespace testseam = pdfe::ui::testseam;

namespace {

std::filesystem::path fixture(const char* name)
{
    return std::filesystem::path(TEST_FIXTURES_DIR) / name;
}

void settle()
{
    for (int i = 0; i < 5; ++i) QApplication::processEvents();
}

class PipesOn {
public:
    PipesOn() : had_(qEnvironmentVariableIsSet("PDFE_TEST_PIPES"))
    {
        qputenv("PDFE_TEST_PIPES", "1");
        testseam::dialogActionQueue().clear();
    }
    ~PipesOn()
    {
        testseam::dialogActionQueue().clear();
        if (!had_) qunsetenv("PDFE_TEST_PIPES");
    }

private:
    bool had_;
};

// Closes a modal Sign dialog still open after @p ms, so a regression fails
// rather than hanging in exec(). Records whether it had to.
class ModalDeadline {
public:
    explicit ModalDeadline(int ms)
    {
        timer_.setInterval(50);
        QObject::connect(&timer_, &QTimer::timeout, [this, ms]() {
            QWidget* modal = QApplication::activeModalWidget();
            if (!modal) {
                since_.invalidate();
                return;
            }
            if (!since_.isValid()) since_.start();
            if (since_.elapsed() > ms) {
                forced_ = true;
                if (auto* d = qobject_cast<QDialog*>(modal)) d->reject(); else modal->close();
            }
        });
        timer_.start();
    }
    bool forced() const { return forced_; }

private:
    QTimer timer_;
    QElapsedTimer since_;
    bool forced_ = false;
};

std::string fileBytes(const std::filesystem::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

}  // namespace

// ---------------------------------------------------------------------------
// The dialog offers the mode, and says what it is not.
// ---------------------------------------------------------------------------

TEST(VisualSignatureDialog, TheOptionIsLabelledAsNotADigitalSignature)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    PipesOn pipes;
    pdfe::ui::SignDialog dlg(nullptr);

    // The dialog's own introspection record lists the option and its notice,
    // so automation can find them without UIA.
    const QString inventory = testseam::dialogIntrospection(QStringLiteral("signDialog"));
    EXPECT_TRUE(inventory.contains(QStringLiteral("visualSignatureCheck"))) << inventory.toStdString();
    EXPECT_TRUE(inventory.contains(QStringLiteral("visualSignatureNotice"))) << inventory.toStdString();

    auto* check = dlg.findChild<QCheckBox*>(QStringLiteral("visualSignatureCheck"));
    auto* notice = dlg.findChild<QLabel*>(QStringLiteral("visualSignatureNotice"));
    ASSERT_NE(check, nullptr) << "no visual-signature option in the Sign dialog";
    ASSERT_NE(notice, nullptr);
    EXPECT_TRUE(notice->text().contains(QStringLiteral("not a digital signature"), Qt::CaseInsensitive))
        << "notice: '" << notice->text().toStdString() << "'";
    EXPECT_TRUE(check->text().contains(QStringLiteral("visual"), Qt::CaseInsensitive));
}

TEST(VisualSignatureDialog, VisualModeNeedsAnImageButNoCertificate)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    pdfe::ui::SignDialog dlg(nullptr);
    auto* check = dlg.findChild<QCheckBox*>(QStringLiteral("visualSignatureCheck"));
    ASSERT_NE(check, nullptr);
    check->setChecked(true);

    // The certificate, timestamp and certification controls do not apply.
    for (const char* name : {"pfxPathEdit", "browseButton", "pfxPasswordEdit", "createCertButton",
                             "tsaCheckBox", "tsaUrlEdit", "invisibleCheckBox",
                             "certificationLevelCombo"}) {
        auto* w = dlg.findChild<QWidget*>(QLatin1String(name));
        ASSERT_NE(w, nullptr) << name;
        EXPECT_FALSE(w->isEnabled()) << name << " is still enabled in visual mode";
    }

    auto* ok = dlg.findChild<QPushButton*>(QStringLiteral("okButton"));
    ASSERT_NE(ok, nullptr);
    EXPECT_EQ(ok->text(), QStringLiteral("Place")) << "the OK button still says it signs";

    // No image: refused, with the reason in the dialog.
    dlg.accept();
    EXPECT_NE(dlg.result(), QDialog::Accepted) << "accepted a visual signature with no image";
    auto* validation = dlg.findChild<QLabel*>(QStringLiteral("validationLabel"));
    ASSERT_NE(validation, nullptr);
    EXPECT_TRUE(validation->text().contains(QStringLiteral("image"), Qt::CaseInsensitive))
        << validation->text().toStdString();

    // An image and no PFX: accepted, and the options say "visual only".
    auto* image = dlg.findChild<QLineEdit*>(QStringLiteral("imagePathEdit"));
    ASSERT_NE(image, nullptr);
    image->setText(QString::fromStdString(fixture("test_image_rgb.png").string()));
    dlg.accept();
    EXPECT_EQ(dlg.result(), QDialog::Accepted);
    const auto opts = dlg.signingOptions();
    EXPECT_TRUE(opts.visualOnly);
    EXPECT_TRUE(opts.pfxPath.empty());
    EXPECT_TRUE(opts.pfxPassword.empty());
}

TEST(VisualSignatureDialog, UncheckingRestoresTheCertificateRequirement)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    pdfe::ui::SignDialog dlg(nullptr);
    auto* check = dlg.findChild<QCheckBox*>(QStringLiteral("visualSignatureCheck"));
    ASSERT_NE(check, nullptr);
    check->setChecked(true);
    check->setChecked(false);
    auto* image = dlg.findChild<QLineEdit*>(QStringLiteral("imagePathEdit"));
    ASSERT_NE(image, nullptr);
    image->setText(QString::fromStdString(fixture("test_image_rgb.png").string()));
    EXPECT_TRUE(dlg.findChild<QWidget*>(QStringLiteral("pfxPathEdit"))->isEnabled());
    dlg.accept();
    EXPECT_NE(dlg.result(), QDialog::Accepted)
        << "the cryptographic path accepted without a PFX";
    EXPECT_FALSE(dlg.signingOptions().visualOnly);
}

// ---------------------------------------------------------------------------
// End to end: Sign Document, visual option, save, reopen.
// ---------------------------------------------------------------------------

TEST(VisualSignatureEndToEnd, PlacesTheImageAndDateAsAStampWithoutAPfxAndSignsNothing)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const auto work = std::filesystem::path(tmp.path().toStdString()) / "visual.pdf";
    std::filesystem::copy_file(fixture("test_single.pdf"), work);
    const auto png = fixture("test_image_rgb.png");
    ASSERT_TRUE(std::filesystem::exists(png));

    QSize appearanceSize;
    {
        PipesOn pipes;
        pdfe::ui::MainWindow mw;
        mw.openFile(QString::fromStdString(work.string()));
        settle();
        ASSERT_NE(mw.documentForTesting(), nullptr);
        ASSERT_EQ(mw.signatureFieldCountForTesting(), 0) << "precondition: no signature field";

        testseam::enqueueDialogActions(QStringLiteral("signDialog"), {
            QStringLiteral("check:visualSignatureCheck=1"),
            QStringLiteral("edit:imagePathEdit=%1").arg(QString::fromStdString(png.string())),
            QStringLiteral("click:okButton"),
        });
        ModalDeadline deadline(6000);
        mw.triggerSignDocumentForTesting();
        settle();
        EXPECT_FALSE(deadline.forced()) << "the Sign dialog did not accept the visual signature";
        EXPECT_EQ(mw.signatureFieldCountForTesting(), 0)
            << "a visual signature created a signature field";
        EXPECT_TRUE(mw.lastSigningErrorForTesting().isEmpty())
            << mw.lastSigningErrorForTesting().toStdString();
        appearanceSize = mw.lastVisualSignatureAppearanceSizeForTesting();
        ASSERT_TRUE(appearanceSize.isValid()) << "no appearance was composed";

        auto* save = mw.findChild<QAction*>(QStringLiteral("saveAction"));
        ASSERT_NE(save, nullptr);
        save->trigger();
        settle();
    }

    // Reopen what was written.
    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    auto doc = pdfe::core::Document::open(ctx, work);
    ASSERT_NE(doc, nullptr);
    pdfe::annotations::AnnotationManager annots(ctx);
    auto page = doc->page(0);
    int stampIndex = -1;
    pdfe::annotations::AnnotationData stamp;
    for (const auto& a : annots.getAnnotations(page.get())) {
        if (a.type == pdfe::annotations::AnnotationType::Stamp) {
            stamp = a;
            stampIndex = a.annotIndex;
        }
    }
    ASSERT_GE(stampIndex, 0) << "no stamp annotation was saved";

    // The appearance was composed for the stamp's OWN rectangle, not the
    // requested 200 x 60 box, so it is shown unstretched.
    EXPECT_NEAR(double(appearanceSize.width()) / appearanceSize.height(),
                stamp.rect.width() / stamp.rect.height(), 0.02)
        << "appearance " << appearanceSize.width() << "x" << appearanceSize.height()
        << " vs stamp " << stamp.rect.width() << "x" << stamp.rect.height();

    // Inside the Sign dialog's default placement box (50, 50, 200 x 60, fitz
    // space). MuPDF gives every /Stamp its own proportions -- the requested
    // width, a fixed height, centred vertically -- so the height is not 60.
    EXPECT_NEAR(stamp.rect.left(), 50.0, 1.0);
    EXPECT_NEAR(stamp.rect.width(), 200.0, 1.0);
    EXPECT_GE(stamp.rect.top(), 50.0 - 1.0);
    EXPECT_LE(stamp.rect.bottom(), 110.0 + 1.0);
    EXPECT_GT(stamp.rect.height(), 20.0);

    EXPECT_TRUE(annots.stampAppearanceHasImageXObjectForTesting(page.get(), stampIndex))
        << "the stamp carries no image";
    const QString contents = QString::fromStdString(stamp.contents);
    EXPECT_TRUE(contents.contains(QDate::currentDate().toString(Qt::ISODate)))
        << "no date in the stamp: '" << stamp.contents << "'";
    EXPECT_TRUE(contents.contains(QStringLiteral("not a digital signature"), Qt::CaseInsensitive))
        << "the stamp does not say it is not a digital signature: '" << stamp.contents << "'";

    // Nothing was signed.
    pdfe::signatures::SignatureManager sigs(ctx);
    for (const auto& s : sigs.listSignatures(doc.get())) {
        EXPECT_FALSE(s.isSigned) << "a signature value was written";
    }
    EXPECT_EQ(fileBytes(work).find("/ByteRange"), std::string::npos)
        << "the saved file contains a /ByteRange";
}

TEST(VisualSignatureEndToEnd, OverAnExistingUnsignedFieldStampsItsRectAndLeavesItUnsigned)
{
    // Sign Document with an unsigned signature field present signs THAT field.
    // In visual mode the stamp goes on the field's rectangle and the field
    // stays unsigned: no PFX is read, no signature value is written. Driven
    // through MainWindow's sign-flow seam (visualOnly), which stands in for the
    // dialog the other end-to-end test drives for real.
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const auto work = std::filesystem::path(tmp.path().toStdString()) / "field.pdf";
    std::filesystem::copy_file(fixture("test_single.pdf"), work);
    const QRectF fieldRect(100, 300, 240, 80);
    {
        auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
        auto doc = pdfe::core::Document::open(ctx, work);
        ASSERT_NE(doc, nullptr);
        pdfe::signatures::SignatureManager sm(ctx);
        auto page = doc->page(0);
        sm.createSignatureField(page.get(), "Sig1", fieldRect);
        ASSERT_NO_THROW(doc->save(work));
    }

    {
        pdfe::ui::MainWindow mw;
        mw.openFile(QString::fromStdString(work.string()));
        settle();
        ASSERT_NE(mw.documentForTesting(), nullptr);
        ASSERT_EQ(mw.signatureFieldCountForTesting(), 1);

        pdfe::ui::MainWindow::SignFlowTestInputs in;
        in.dialogAccepted = true;
        in.visualOnly = true;
        in.imagePath = QString::fromStdString(fixture("test_image_rgb.png").string());
        in.pfxPath = QStringLiteral("/nonexistent/must-not-be-read.pfx");
        in.suppressErrorDialogs = true;
        mw.setSignFlowTestInputsForTesting(in);
        mw.triggerSignDocumentForTesting();
        settle();
        EXPECT_TRUE(mw.lastSigningErrorForTesting().isEmpty())
            << "the visual path reported a signing error (did it try the PFX?): "
            << mw.lastSigningErrorForTesting().toStdString();
        EXPECT_EQ(mw.signatureFieldCountForTesting(), 1);

        auto* save = mw.findChild<QAction*>(QStringLiteral("saveAction"));
        ASSERT_NE(save, nullptr);
        save->trigger();
        settle();
    }

    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    auto doc = pdfe::core::Document::open(ctx, work);
    ASSERT_NE(doc, nullptr);
    pdfe::annotations::AnnotationManager annots(ctx);
    auto page = doc->page(0);
    int stampIndex = -1;
    pdfe::annotations::AnnotationData stamp;
    for (const auto& a : annots.getAnnotations(page.get())) {
        if (a.type == pdfe::annotations::AnnotationType::Stamp) { stamp = a; stampIndex = a.annotIndex; }
    }
    ASSERT_GE(stampIndex, 0) << "no stamp over the field";
    EXPECT_NEAR(stamp.rect.left(), fieldRect.left(), 1.0);
    EXPECT_NEAR(stamp.rect.width(), fieldRect.width(), 1.0);
    EXPECT_GE(stamp.rect.top(), fieldRect.top() - 1.0);
    EXPECT_LE(stamp.rect.bottom(), fieldRect.bottom() + 1.0);
    EXPECT_TRUE(annots.stampAppearanceHasImageXObjectForTesting(page.get(), stampIndex));

    pdfe::signatures::SignatureManager sigs(ctx);
    const auto list = sigs.listSignatures(doc.get());
    ASSERT_EQ(list.size(), 1u) << "the field disappeared or another was added";
    EXPECT_FALSE(list.front().isSigned) << "the field was signed";
    EXPECT_EQ(fileBytes(work).find("/ByteRange"), std::string::npos);
}

TEST(VisualSignatureEndToEnd, UndoThenRedoKeepsTheImageAndTheDisclaimer)
{
    // The placement goes through the annotation command system so it can be
    // undone. A redo must bring back the SAME stamp: its image and its
    // "not a digital signature" contents, not a bare text stamp. (Redo used to
    // restore the data captured when the command first ran, before the
    // contents and the image were applied.)
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const auto work = std::filesystem::path(tmp.path().toStdString()) / "redo.pdf";
    std::filesystem::copy_file(fixture("test_single.pdf"), work);

    {
        pdfe::ui::MainWindow mw;
        mw.openFile(QString::fromStdString(work.string()));
        settle();
        ASSERT_NE(mw.documentForTesting(), nullptr);

        pdfe::ui::MainWindow::SignFlowTestInputs in;
        in.dialogAccepted = true;
        in.visualOnly = true;
        in.imagePath = QString::fromStdString(fixture("test_image_rgb.png").string());
        in.suppressErrorDialogs = true;
        mw.setSignFlowTestInputsForTesting(in);
        mw.triggerSignDocumentForTesting();
        settle();
        ASSERT_TRUE(mw.lastSigningErrorForTesting().isEmpty())
            << mw.lastSigningErrorForTesting().toStdString();

        auto* undo = mw.findChild<QAction*>(QStringLiteral("undoAction"));
        auto* redo = mw.findChild<QAction*>(QStringLiteral("redoAction"));
        ASSERT_NE(undo, nullptr);
        ASSERT_NE(redo, nullptr);
        undo->trigger();
        settle();
        {
            pdfe::annotations::AnnotationManager live(mw.documentForTesting()->context());
            auto page = mw.documentForTesting()->page(0);
            int stamps = 0;
            for (const auto& a : live.getAnnotations(page.get()))
                if (a.type == pdfe::annotations::AnnotationType::Stamp) ++stamps;
            ASSERT_EQ(stamps, 0) << "undo did not remove the visual signature";
        }
        redo->trigger();
        settle();

        auto* save = mw.findChild<QAction*>(QStringLiteral("saveAction"));
        ASSERT_NE(save, nullptr);
        save->trigger();
        settle();
    }

    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    auto doc = pdfe::core::Document::open(ctx, work);
    ASSERT_NE(doc, nullptr);
    pdfe::annotations::AnnotationManager annots(ctx);
    auto page = doc->page(0);
    int stampIndex = -1;
    pdfe::annotations::AnnotationData stamp;
    for (const auto& a : annots.getAnnotations(page.get())) {
        if (a.type == pdfe::annotations::AnnotationType::Stamp) { stamp = a; stampIndex = a.annotIndex; }
    }
    ASSERT_GE(stampIndex, 0) << "redo did not bring the visual signature back";
    EXPECT_TRUE(annots.stampAppearanceHasImageXObjectForTesting(page.get(), stampIndex))
        << "after undo and redo the stamp lost its image";
    EXPECT_TRUE(QString::fromStdString(stamp.contents)
                    .contains(QStringLiteral("not a digital signature"), Qt::CaseInsensitive))
        << "after undo and redo the stamp lost its disclaimer: '" << stamp.contents << "'";
}

// ---------------------------------------------------------------------------
// The appearance itself: what a viewer sees on the page.
// ---------------------------------------------------------------------------

TEST(VisualSignatureAppearance, TheCaptionSaysWhenAndThatItIsNotADigitalSignature)
{
    const QStringList caption = pdfe::ui::visualSignatureCaption(QStringLiteral("2026-11-26 14:05"));
    ASSERT_EQ(caption.size(), 2);
    EXPECT_TRUE(caption[0].contains(QStringLiteral("2026-11-26 14:05"))) << caption[0].toStdString();
    EXPECT_TRUE(caption[1].contains(QStringLiteral("not a digital signature"), Qt::CaseInsensitive))
        << caption[1].toStdString();
}

TEST(VisualSignatureAppearance, TheCaptionIsDrawnBelowTheImageAndTheSizeFollowsTheStamp)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    QImage signature(300, 100, QImage::Format_ARGB32);
    signature.fill(Qt::blue);
    const QSizeF stampPt(200.0, 52.63);
    const QImage out = pdfe::ui::composeVisualSignature(signature, QStringLiteral("2026-11-26 14:05"),
                                                        stampPt);
    ASSERT_FALSE(out.isNull());
    // Same aspect as the stamp it is composed for.
    EXPECT_NEAR(double(out.width()) / out.height(), stampPt.width() / stampPt.height(), 0.02);

    // The caption band (bottom kVisualSignatureCaptionPercent) holds two
    // lines: the date in its upper part, the disclaimer in its lower part.
    // Each must have dark, non-blue ink of its own -- one line drawn is not
    // enough. The image band above has the signature's blue.
    const int h = out.height();
    const int textH = h * pdfe::ui::kVisualSignatureCaptionPercent / 100;
    const int captionTop = h - textH;
    const int noteTop = h - textH * 45 / 100;  // where the disclaimer line starts
    int dateInk = 0, noteInk = 0, imageBlue = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < out.width(); ++x) {
            const QColor c = QColor::fromRgba(out.pixel(x, y));
            if (c.alpha() < 128) continue;
            const bool ink = c.blue() < 150 && c.lightness() < 160;
            if (y >= captionTop && y < noteTop && ink) ++dateInk;
            if (y >= noteTop && ink) ++noteInk;
            if (y < captionTop && c.blue() > 200 && c.red() < 60) ++imageBlue;
        }
    }
    EXPECT_GT(dateInk, 30) << "the date line was not drawn";
    EXPECT_GT(noteInk, 30) << "the 'not a digital signature' line was not drawn";
    EXPECT_GT(imageBlue, 500) << "the signature image was not drawn in the top band";
}
