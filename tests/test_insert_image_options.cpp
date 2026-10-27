// -*- coding: utf-8 -*-
// GitHub #242 — Insert Image had no size, placement or encoding control.
//
// THE DEFECT
//
// onInsertImage() was a file picker and one literal:
//
//     opts.targetRect = QRectF(72, 72, 400, 300); // Default placement
//
// Every image landed one inch from the top-left at a fixed 400x300 — the same
// box on an A4 portrait page, a landscape slide, or a small custom page, and
// the same box for a tall photograph as for a wide one. The three other options
// InsertImageOptions already accepted (jpegQuality, stripExif,
// maintainAspectRatio) were never surfaced at all.
//
// WHAT THE ISSUE GOT WRONG, AND IT MATTERS FOR SCOPE
//
// The report and the issue both assumed rotation would need an engine change.
// It does not. ImageTool already emits resizeRequested, rotateRequested AND
// cropRequested, and MainWindow::onImageToolSelected() already connects all
// three to real commands. Post-insert manipulation has worked the whole time.
// The gap was that nothing took the user to it after an insert, so the controls
// were unreachable from the path where they were wanted.
//
// WHY THESE TESTS ASSERT ON A RULE RATHER THAN A RECTANGLE
//
// A test that pinned one expected rectangle for one page size would pass
// equally against the old literal if the numbers happened to line up, and would
// say nothing about the defect — which was that the placement ignored its
// INPUTS. So these vary the page and the image and assert the relationships:
// it fits, it is centred, it keeps the image's proportions, and it does not
// enlarge a small image to fill the page.
#include <gtest/gtest.h>

#include "ui/InsertImageDialog.h"

#include <QRectF>
#include <QSize>
#include <QSizeF>

using pdfe::ui::InsertImageDialog;

namespace {

// US Letter and A4 in points, plus a landscape slide and a deliberately tiny
// page — the shapes a single literal cannot serve.
constexpr QSizeF kLetter{612.0, 792.0};
constexpr QSizeF kA4{595.0, 842.0};
constexpr QSizeF kSlide{960.0, 540.0};
constexpr QSizeF kTiny{200.0, 150.0};

bool fitsWithin(const QRectF& r, QSizeF page)
{
    return r.left() >= -0.01 && r.top() >= -0.01 &&
           r.right() <= page.width() + 0.01 &&
           r.bottom() <= page.height() + 0.01;
}

bool isCentred(const QRectF& r, QSizeF page)
{
    const double dx = std::abs((r.left() + r.right()) / 2.0 - page.width() / 2.0);
    const double dy = std::abs((r.top() + r.bottom()) / 2.0 - page.height() / 2.0);
    return dx < 0.5 && dy < 0.5;
}

}  // namespace

// The core property the literal violated: the placement must depend on the page.
TEST(InsertImageDefaultPlacement, DiffersWithThePageItIsPlacedOn)
{
    const QSize image(1200, 900);

    const QRectF onLetter = InsertImageDialog::defaultRectFor(image, kLetter);
    const QRectF onSlide = InsertImageDialog::defaultRectFor(image, kSlide);
    const QRectF onTiny = InsertImageDialog::defaultRectFor(image, kTiny);

    EXPECT_NE(onLetter, onSlide)
        << "the same image placed on a portrait page and a landscape slide got "
           "the identical rectangle — the placement is ignoring the page";
    EXPECT_NE(onLetter, onTiny);

    for (const auto& [rect, page] : {std::pair{onLetter, kLetter},
                                     std::pair{onSlide, kSlide},
                                     std::pair{onTiny, kTiny}}) {
        EXPECT_TRUE(fitsWithin(rect, page))
            << "placement " << rect.x() << "," << rect.y() << " "
            << rect.width() << "x" << rect.height()
            << " does not fit a " << page.width() << "x" << page.height()
            << " page — the old literal did not fit the tiny page either";
        EXPECT_TRUE(isCentred(rect, page)) << "placement is not centred";
    }
}

// And on the image: a tall photograph must not be given a wide box.
TEST(InsertImageDefaultPlacement, KeepsTheImageProportions)
{
    const QSize wide(1600, 400);   // 4:1
    const QSize tall(400, 1600);   // 1:4

    const QRectF w = InsertImageDialog::defaultRectFor(wide, kLetter);
    const QRectF t = InsertImageDialog::defaultRectFor(tall, kLetter);

    EXPECT_GT(w.width(), w.height()) << "a 4:1 image was not placed in a wide box";
    EXPECT_LT(t.width(), t.height()) << "a 1:4 image was not placed in a tall box";

    const double wRatio = w.width() / w.height();
    const double tRatio = t.width() / t.height();
    EXPECT_NEAR(wRatio, 4.0, 0.05)
        << "aspect ratio not preserved for the wide image (got " << wRatio << ")";
    EXPECT_NEAR(tRatio, 0.25, 0.01)
        << "aspect ratio not preserved for the tall image (got " << tRatio << ")";
}

// Scale DOWN to fit, never up. A 32x32 icon blown up to fill the page would be
// a worse default than the literal it replaces.
TEST(InsertImageDefaultPlacement, DoesNotEnlargeASmallImage)
{
    const QSize icon(32, 32);
    const QRectF r = InsertImageDialog::defaultRectFor(icon, kLetter);

    // 32 px at 96 dpi is 24 pt.
    EXPECT_NEAR(r.width(), 24.0, 0.5)
        << "a 32px icon was scaled up to " << r.width() << " pt";
    EXPECT_NEAR(r.height(), 24.0, 0.5);
    EXPECT_TRUE(isCentred(r, kLetter));
}

// A large image must be scaled down far enough to leave the one-inch margin,
// not merely clipped to the page edge.
TEST(InsertImageDefaultPlacement, LeavesAMarginForALargeImage)
{
    const QSize huge(6000, 4000);
    const QRectF r = InsertImageDialog::defaultRectFor(huge, kA4);

    EXPECT_TRUE(fitsWithin(r, kA4));
    EXPECT_GE(r.left(), 71.5) << "no left margin";
    EXPECT_LE(r.right(), kA4.width() - 71.5) << "no right margin";
    EXPECT_NEAR(r.width() / r.height(), 1.5, 0.02)
        << "aspect ratio lost while scaling down";
}

// Degenerate inputs must not produce a rectangle that cannot be drawn. A zero
// page size is the shape a caller hits when a page failed to load.
TEST(InsertImageDefaultPlacement, DegenerateInputsStillYieldAUsableRect)
{
    const QRectF noPage = InsertImageDialog::defaultRectFor(QSize(100, 100), QSizeF(0, 0));
    EXPECT_GT(noPage.width(), 0.0);
    EXPECT_GT(noPage.height(), 0.0);

    // An unreadable image reports an invalid size; the placement should fall
    // back to the available area rather than collapsing to nothing.
    const QRectF noImage = InsertImageDialog::defaultRectFor(QSize(), kLetter);
    EXPECT_GT(noImage.width(), 0.0);
    EXPECT_GT(noImage.height(), 0.0);
    EXPECT_TRUE(fitsWithin(noImage, kLetter));
}

// ---------------------------------------------------------------------------
// A USER'S CHOICE, not just the default formula.
//
// A gate review failed this phase for the gap these close. Everything above
// tests `defaultRectFor()` — the rule that replaced the hardcoded literal — and
// that is necessary but not sufficient: the acceptance criterion asks that the
// resulting placement reflect a NON-DEFAULT user choice, which is the whole
// point of having a dialog rather than a better constant. A dialog whose
// controls were decorative would satisfy every test above.
//
// The controls are reached by objectName, which is also how the UI suite drives
// them, so these double as a check that those names exist and are stable.
// ---------------------------------------------------------------------------
#include "ui/InsertImageDialog.h"

#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QSpinBox>

namespace {

QDoubleSpinBox* spin(InsertImageDialog& dlg, const char* name)
{
    return dlg.findChild<QDoubleSpinBox*>(QString::fromLatin1(name));
}

}  // namespace

TEST(InsertImageUserChoice, TargetRectReflectsValuesTheUserTyped)
{
    InsertImageDialog dlg(QStringLiteral("p94.png"), QSize(1200, 900), kLetter);

    const QRectF def = InsertImageDialog::defaultRectFor(QSize(1200, 900), kLetter);

    auto* x = spin(dlg, "insertImageXSpin");
    auto* y = spin(dlg, "insertImageYSpin");
    auto* w = spin(dlg, "insertImageWidthSpin");
    auto* h = spin(dlg, "insertImageHeightSpin");
    auto* keepAspect = dlg.findChild<QCheckBox*>("insertImageKeepAspectCheck");
    ASSERT_NE(x, nullptr) << "no X control — the UI suite could not drive this either";
    ASSERT_NE(y, nullptr);
    ASSERT_NE(w, nullptr);
    ASSERT_NE(h, nullptr);
    ASSERT_NE(keepAspect, nullptr);

    // Unlock the ratio so width and height can be set independently; the locked
    // behaviour has its own test below.
    keepAspect->setChecked(false);

    x->setValue(123.0);
    y->setValue(45.0);
    w->setValue(200.0);
    h->setValue(100.0);

    const QRectF chosen = dlg.targetRect();
    EXPECT_EQ(chosen, QRectF(123.0, 45.0, 200.0, 100.0))
        << "targetRect() did not return what the user typed — the controls are "
           "decorative and the placement is still effectively fixed";
    EXPECT_NE(chosen, def)
        << "the chosen rect equals the default, so this test cannot tell a live "
           "control from a dead one";
}

TEST(InsertImageUserChoice, TheOtherThreeOptionsRoundTrip)
{
    InsertImageDialog dlg(QStringLiteral("p94.png"), QSize(640, 480), kA4);

    auto* quality = dlg.findChild<QSpinBox*>("insertImageJpegQualitySpin");
    auto* strip = dlg.findChild<QCheckBox*>("insertImageStripExifCheck");
    auto* keepAspect = dlg.findChild<QCheckBox*>("insertImageKeepAspectCheck");
    ASSERT_NE(quality, nullptr);
    ASSERT_NE(strip, nullptr);
    ASSERT_NE(keepAspect, nullptr);

    // These three were accepted by the engine all along and surfaced nowhere.
    EXPECT_EQ(dlg.jpegQuality(), 85) << "default should match the engine's own";
    EXPECT_TRUE(dlg.stripExif()) << "EXIF stripping must stay on by default";
    EXPECT_TRUE(dlg.maintainAspectRatio());

    quality->setValue(40);
    strip->setChecked(false);
    keepAspect->setChecked(false);

    EXPECT_EQ(dlg.jpegQuality(), 40);
    EXPECT_FALSE(dlg.stripExif())
        << "the user cleared EXIF stripping and the dialog ignored them — this "
           "is the privacy-relevant one";
    EXPECT_FALSE(dlg.maintainAspectRatio());
}

// The aspect lock is a behaviour, not a stored flag: with it on, editing one
// dimension must move the other.
TEST(InsertImageUserChoice, TheAspectLockDrivesTheOtherDimension)
{
    InsertImageDialog dlg(QStringLiteral("p94.png"), QSize(1600, 400), kLetter);

    auto* w = spin(dlg, "insertImageWidthSpin");
    auto* h = spin(dlg, "insertImageHeightSpin");
    auto* keepAspect = dlg.findChild<QCheckBox*>("insertImageKeepAspectCheck");
    ASSERT_NE(w, nullptr);
    ASSERT_NE(h, nullptr);
    ASSERT_NE(keepAspect, nullptr);
    ASSERT_TRUE(keepAspect->isChecked()) << "precondition: the lock starts on";

    w->setValue(400.0);
    // 1600x400 is 4:1, so the height must follow to 100.
    EXPECT_NEAR(h->value(), 100.0, 0.5)
        << "width changed but height did not follow while the aspect lock was "
           "on; the checkbox does nothing";
    EXPECT_NEAR(dlg.targetRect().width() / dlg.targetRect().height(), 4.0, 0.05);

    // With the lock off the two are independent.
    keepAspect->setChecked(false);
    w->setValue(300.0);
    EXPECT_NEAR(h->value(), 100.0, 0.5)
        << "height moved with the aspect lock OFF";
}
