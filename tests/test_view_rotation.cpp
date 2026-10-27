// -*- coding: utf-8 -*-
// Phase 4c: View rotation tests.
// Verifies per-session view rotation state on PdfCanvas without touching the
// underlying PDF file. Rotation is display-only and clears on clearDocument().
#include <gtest/gtest.h>
#include "ui/PdfCanvas.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "rendering/TileCache.h"
#include "rendering/TileRenderer.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <filesystem>
#include <memory>

using namespace pdfe::ui;
using namespace pdfe::core;
using namespace pdfe::rendering;

namespace {

QByteArray sha256OfFile(const std::filesystem::path& p)
{
    QFile f(QString::fromStdString(p.string()));
    if (!f.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash h(QCryptographicHash::Sha256);
    h.addData(&f);
    return h.result();
}

} // namespace

TEST(ViewRotationTest, DefaultRotationIsZero)
{
    PdfCanvas canvas;
    EXPECT_EQ(canvas.viewRotation(), 0);
}

TEST(ViewRotationTest, RotateCWCyclesThroughQuadrants)
{
    PdfCanvas canvas;
    EXPECT_EQ(canvas.viewRotation(), 0);
    canvas.rotateViewCW();
    EXPECT_EQ(canvas.viewRotation(), 90);
    canvas.rotateViewCW();
    EXPECT_EQ(canvas.viewRotation(), 180);
    canvas.rotateViewCW();
    EXPECT_EQ(canvas.viewRotation(), 270);
    canvas.rotateViewCW();
    EXPECT_EQ(canvas.viewRotation(), 0);
}

TEST(ViewRotationTest, RotateCCWCyclesThroughQuadrants)
{
    PdfCanvas canvas;
    canvas.rotateViewCCW();
    EXPECT_EQ(canvas.viewRotation(), 270);
    canvas.rotateViewCCW();
    EXPECT_EQ(canvas.viewRotation(), 180);
    canvas.rotateViewCCW();
    EXPECT_EQ(canvas.viewRotation(), 90);
    canvas.rotateViewCCW();
    EXPECT_EQ(canvas.viewRotation(), 0);
}

TEST(ViewRotationTest, ResetClearsToZero)
{
    PdfCanvas canvas;
    canvas.rotateViewCW();
    canvas.rotateViewCW();
    ASSERT_EQ(canvas.viewRotation(), 180);
    canvas.resetViewRotation();
    EXPECT_EQ(canvas.viewRotation(), 0);
}

TEST(ViewRotationTest, SetRotationNormalizesNegativeAndOverflow)
{
    PdfCanvas canvas;
    canvas.setViewRotation(450);     // 450 % 360 = 90
    EXPECT_EQ(canvas.viewRotation(), 90);
    canvas.setViewRotation(-90);     // -90 -> 270
    EXPECT_EQ(canvas.viewRotation(), 270);
    canvas.setViewRotation(720);     // 720 -> 0
    EXPECT_EQ(canvas.viewRotation(), 0);
}

TEST(ViewRotationTest, NonMultipleOfNinetyFallsBackToZero)
{
    PdfCanvas canvas;
    canvas.setViewRotation(45);
    EXPECT_EQ(canvas.viewRotation(), 0);
    canvas.setViewRotation(89);
    // 89 / 90 * 90 = 0 (integer truncation), so still 0
    EXPECT_EQ(canvas.viewRotation(), 0);
}

TEST(ViewRotationTest, ClearDocumentResetsRotation)
{
    PdfCanvas canvas;
    canvas.rotateViewCW();
    ASSERT_EQ(canvas.viewRotation(), 90);
    canvas.clearDocument();
    EXPECT_EQ(canvas.viewRotation(), 0)
        << "View rotation must not leak across document open cycles";
}

TEST(ViewRotationTest, NinetyDegreeTransposesPageDimensions)
{
    // CTM math check: at 90° or 270° rotation, getPageScreenRect() must return
    // the pre-rotation width as the post-rotation height (and vice versa).
    // Verifies the updateDocumentLayout() transpose path.
    std::filesystem::path fixtures = TEST_FIXTURES_DIR;
    auto pdf = fixtures / "test_single.pdf";
    if (!std::filesystem::exists(pdf)) {
        GTEST_SKIP() << "test_single.pdf fixture not available";
    }
    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, pdf);
    ASSERT_NE(doc, nullptr);
    ASSERT_GT(doc->pageCount(), 0);

    PdfCanvas canvas;
    canvas.resize(800, 600);  // Give the widget some geometry so layout runs.
    TileCache cache(8 * 1024 * 1024);
    TileRenderer renderer(doc.get());
    canvas.setDocument(doc.get(), &cache, &renderer);

    // Flush any deferred layout/tile requests.
    QCoreApplication::processEvents();

    QPoint origin0;
    int w0 = 0, h0 = 0;
    ASSERT_TRUE(canvas.getPageScreenRect(0, origin0, w0, h0));
    ASSERT_GT(w0, 0);
    ASSERT_GT(h0, 0);

    // 180° should NOT transpose.
    canvas.setViewRotation(180);
    QPoint origin180;
    int w180 = 0, h180 = 0;
    ASSERT_TRUE(canvas.getPageScreenRect(0, origin180, w180, h180));
    EXPECT_EQ(w180, w0) << "180° rotation must preserve width";
    EXPECT_EQ(h180, h0) << "180° rotation must preserve height";

    // 90° must transpose.
    canvas.setViewRotation(90);
    QPoint origin90;
    int w90 = 0, h90 = 0;
    ASSERT_TRUE(canvas.getPageScreenRect(0, origin90, w90, h90));
    EXPECT_EQ(w90, h0) << "90° rotation must transpose: new width == old height";
    EXPECT_EQ(h90, w0) << "90° rotation must transpose: new height == old width";

    // 270° must also transpose (same dims as 90°).
    canvas.setViewRotation(270);
    QPoint origin270;
    int w270 = 0, h270 = 0;
    ASSERT_TRUE(canvas.getPageScreenRect(0, origin270, w270, h270));
    EXPECT_EQ(w270, h0);
    EXPECT_EQ(h270, w0);

    canvas.clearDocument();
}

TEST(ViewRotationTest, RotationDoesNotMutateFileBytes)
{
    // Acceptance: viewRotation affects render output but NOT file bytes.
    std::filesystem::path fixtures = TEST_FIXTURES_DIR;
    auto pdf = fixtures / "test_single.pdf";
    if (!std::filesystem::exists(pdf)) {
        GTEST_SKIP() << "test_single.pdf fixture not available";
    }

    QByteArray hashBefore = sha256OfFile(pdf);
    ASSERT_FALSE(hashBefore.isEmpty());

    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, pdf);
    ASSERT_NE(doc, nullptr);

    PdfCanvas canvas;
    TileCache cache(8 * 1024 * 1024);  // 8 MB cap
    TileRenderer renderer(doc.get());
    canvas.setDocument(doc.get(), &cache, &renderer);

    canvas.rotateViewCW();
    canvas.rotateViewCW();
    canvas.rotateViewCW();
    canvas.resetViewRotation();
    canvas.rotateViewCCW();

    // No save was triggered; file bytes must be byte-equal.
    QByteArray hashAfter = sha256OfFile(pdf);
    EXPECT_EQ(hashBefore, hashAfter)
        << "View rotation must not write to the PDF file";

    canvas.clearDocument();
}
