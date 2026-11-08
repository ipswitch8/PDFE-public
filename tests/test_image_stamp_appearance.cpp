// -*- coding: utf-8 -*-
// many-bugs-2026-11 phase-7: GitHub #359 — an imported Stamp image must be
// placed AS AN IMAGE, not silently degraded to a text stamp.
//
// The user imports a PNG into the ImageStampLibrary and picks it from the
// Stamp tool. The library entry is looked up by its NAME (the key stamp()
// and setImageStamp() use), the non-null QImage is handed to StampTool, and
// on release StampTool writes a /Stamp annotation whose /AP/N appearance is a
// Form XObject containing an Image XObject. If the lookup had returned a null
// image (the #359 symptom), StampTool would fall through to its text branch
// and the appearance would carry a /DA text run with no image XObject.
//
// This drives the REAL seams end to end — ImageStampLibrary::stamp() for the
// lookup, StampTool::onMousePress/onMouseRelease for placement, and
// AnnotationManager::setStampImageAppearance via the tool — and then inspects
// the produced appearance stream through MuPDF
// (stampAppearanceHasImageXObjectForTesting). PDF points are passed directly
// to the tool's documented API; no canvas pixel coordinates are guessed.
#include <gtest/gtest.h>

#include "annotations/AnnotationData.h"
#include "annotations/AnnotationManager.h"
#include "annotations/ImageStampLibrary.h"
#include "annotations/StampTool.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "ui/MainWindow.h"

#include <QApplication>
#include <QColor>
#include <QImage>
#include <QMouseEvent>
#include <QPointF>
#include <QString>

#include <filesystem>
#include <memory>

using namespace pdfe::annotations;
using namespace pdfe::core;

namespace {

QImage makeRedImage(int w = 40, int h = 20) {
    QImage img(w, h, QImage::Format_RGB32);
    img.fill(qRgb(220, 20, 20));
    return img;
}

QMouseEvent leftButton(QEvent::Type type) {
    return QMouseEvent(type, QPointF(0, 0), QPointF(0, 0),
                       Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
}

} // namespace

class ImageStampAppearanceTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx = std::make_shared<MuPdfContext>();
        auto src = std::filesystem::path(TEST_FIXTURES_DIR) / "test_single.pdf";
        if (!std::filesystem::exists(src)) {
            GTEST_SKIP() << "test_single.pdf not available";
        }
        doc = Document::open(ctx, src);
        ASSERT_NE(doc, nullptr);
        page = doc->page(0);
        ASSERT_NE(page, nullptr);
        mgr = std::make_unique<AnnotationManager>(ctx);
    }

    // Drive StampTool through a press/release drag defining `rect` in PDF
    // points, returning the created annotation index (or -1).
    int placeWithTool(StampTool& tool, QPointF p0, QPointF p1) {
        int capturedIdx = -1;
        tool.setPage(page.get());
        tool.annotationCreated =
            [&](int, const AnnotationData& d) { capturedIdx = d.annotIndex; };
        QMouseEvent press = leftButton(QEvent::MouseButtonPress);
        tool.onMousePress(&press, 0, p0);
        QMouseEvent release = leftButton(QEvent::MouseButtonRelease);
        tool.onMouseRelease(&release, 0, p1);
        return capturedIdx;
    }

    std::shared_ptr<MuPdfContext> ctx;
    std::unique_ptr<Document> doc;
    std::unique_ptr<Page> page;
    std::unique_ptr<AnnotationManager> mgr;
};

// #359 core: a library image, looked up by NAME, placed via StampTool, yields
// an IMAGE appearance (a Form XObject with an Image XObject) — not a text /DA.
TEST_F(ImageStampAppearanceTest, LibraryImagePlacesAsImageNotText) {
    ImageStampLibrary lib;
    ASSERT_TRUE(lib.addStamp(QStringLiteral("logo"), makeRedImage()));

    // The lookup that #359 is about: by name, the key stamp()/setImageStamp()
    // use — not the "Image: <name>" display label shown in the menu.
    QImage img = lib.stamp(QStringLiteral("logo"));
    ASSERT_FALSE(img.isNull())
        << "the library lookup by name must return the imported image";

    StampTool tool(mgr.get());
    tool.setColor(QColor(200, 0, 0));
    tool.setImageStamp(QStringLiteral("logo"), img);
    ASSERT_TRUE(tool.isImageStamp());

    const int idx = placeWithTool(tool, QPointF(72, 72), QPointF(272, 172));
    ASSERT_GE(idx, 0) << "StampTool must have created the annotation";

    EXPECT_TRUE(mgr->stampAppearanceHasImageXObjectForTesting(page.get(), idx))
        << "an imported image stamp must produce an Image XObject appearance, "
           "not degrade to a text stamp";
}

// Negative control that keeps the oracle honest: a text stamp (no image) must
// NOT report an image XObject. If the seam returned true unconditionally the
// test above would be vacuous.
TEST_F(ImageStampAppearanceTest, TextStampHasNoImageXObject) {
    StampTool tool(mgr.get());
    tool.setColor(QColor(0, 0, 200));
    tool.setStampName(QStringLiteral("DRAFT"));
    ASSERT_FALSE(tool.isImageStamp());

    const int idx = placeWithTool(tool, QPointF(72, 72), QPointF(272, 172));
    ASSERT_GE(idx, 0);

    EXPECT_FALSE(mgr->stampAppearanceHasImageXObjectForTesting(page.get(), idx))
        << "a text stamp has no image XObject in its appearance";
}

// #359 "fail loudly" sub-claim, mechanised: the Stamp action's image branch
// resolves a library image via MainWindow::resolveLibraryStampImage(), which the
// production toolbar lambda calls. A valid library image resolves non-null with
// NO warning; a listed-but-null/corrupt (or missing) image resolves to a null
// QImage AND takes the warn path (counter increments) instead of silently
// degrading. Drives the real production method, not a reimplementation.
TEST(StampWarnLoudOnNullImage, ResolveWarnsOnNullAndNotOnValid) {
    if (!qApp) GTEST_SKIP() << "QApplication not available";

    pdfe::ui::MainWindow mw;
    mw.setSuppressStampWarningForTesting(true);  // skip only the modal dialog

    auto* lib = mw.imageStampLibraryForTesting();
    ASSERT_NE(lib, nullptr) << "MainWindow must own an ImageStampLibrary";
    lib->clear();
    ASSERT_TRUE(lib->addStamp(QStringLiteral("logo"), makeRedImage()));

    // Valid entry: resolves non-null, no warning.
    const int warnBefore = mw.stampWarningCountForTesting();
    QImage ok = mw.resolveLibraryStampImageForTesting(QStringLiteral("logo"));
    EXPECT_FALSE(ok.isNull())
        << "a valid library image must resolve to a usable QImage";
    EXPECT_EQ(mw.stampWarningCountForTesting(), warnBefore)
        << "resolving a valid image must NOT take the warn path";

    // Null/corrupt (here: a name not in the library, so stamp() is null — the
    // same branch a listed-but-failed-to-decode PNG hits): resolves null and
    // warns LOUDLY rather than returning a usable image or degrading silently.
    QImage bad = mw.resolveLibraryStampImageForTesting(QStringLiteral("missing"));
    EXPECT_TRUE(bad.isNull())
        << "a missing/corrupt library image must resolve to a null QImage";
    EXPECT_EQ(mw.stampWarningCountForTesting(), warnBefore + 1)
        << "GitHub #359: a null library image must take the loud warn path, not "
           "silently degrade (the pre-fix behaviour). The production Stamp "
           "action calls this same resolver.";
}
