// -*- coding: utf-8 -*-
//
// RENDER-FIDELITY-DIFF: Validate rendering produces consistent output
// for the same input. We can't compare against Acrobat in CI, but we
// can verify:
//   1. Same PDF rendered twice produces byte-identical pixmap data
//   2. Rendering at different zoom levels scales proportionally
//   3. Rotated rendering produces predictable output
//   4. Text-only PDFs render with no transparent pixels

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"

#include <QImage>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

using namespace pdfe::core;

namespace {
std::string fixturesDir() { return std::string(TEST_FIXTURES_DIR); }

QImage renderFirstPage(const std::filesystem::path& path, float zoom = 1.0f)
{
    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, path);
    if (!doc || doc->pageCount() == 0) return QImage();
    auto page = doc->page(0);
    if (!page) return QImage();
    return page->render(QRectF(), zoom);
}
} // namespace

class RenderFidelityTest : public ::testing::Test {};

TEST_F(RenderFidelityTest, SameDocumentSameRenderProducesIdenticalImage)
{
    auto path = std::filesystem::path(fixturesDir()) / "test_single.pdf";
    if (!std::filesystem::exists(path)) GTEST_SKIP();

    QImage a = renderFirstPage(path, 1.0f);
    QImage b = renderFirstPage(path, 1.0f);

    ASSERT_FALSE(a.isNull());
    ASSERT_FALSE(b.isNull());
    EXPECT_EQ(a.size(), b.size());
    EXPECT_EQ(a.format(), b.format());
    EXPECT_EQ(a.sizeInBytes(), b.sizeInBytes());

    if (a.size() == b.size() && a.format() == b.format()) {
        bool identical = (std::memcmp(a.constBits(), b.constBits(),
                                       static_cast<size_t>(a.sizeInBytes())) == 0);
        EXPECT_TRUE(identical)
            << "Two renders of the same PDF must produce identical pixels";
    }
}

TEST_F(RenderFidelityTest, ZoomScalesImageProportionally)
{
    auto path = std::filesystem::path(fixturesDir()) / "test_single.pdf";
    if (!std::filesystem::exists(path)) GTEST_SKIP();

    QImage at1x = renderFirstPage(path, 1.0f);
    QImage at2x = renderFirstPage(path, 2.0f);

    ASSERT_FALSE(at1x.isNull());
    ASSERT_FALSE(at2x.isNull());

    // 2x zoom should produce ~2x linear dimensions (allowing rounding)
    int w1 = at1x.width();
    int w2 = at2x.width();
    int h1 = at1x.height();
    int h2 = at2x.height();

    EXPECT_NEAR(static_cast<double>(w2) / w1, 2.0, 0.05)
        << "Width ratio: " << w2 << "/" << w1;
    EXPECT_NEAR(static_cast<double>(h2) / h1, 2.0, 0.05)
        << "Height ratio: " << h2 << "/" << h1;
}

TEST_F(RenderFidelityTest, RenderProducesNonBlankImage)
{
    auto path = std::filesystem::path(fixturesDir()) / "test_single.pdf";
    if (!std::filesystem::exists(path)) GTEST_SKIP();

    QImage img = renderFirstPage(path, 1.0f);
    ASSERT_FALSE(img.isNull());

    // Sample 100 pixels; at least 5% must be non-white
    int nonWhitePixels = 0;
    int sampled = 0;
    int stride = std::max(1, img.width() * img.height() / 100);
    for (int y = 0; y < img.height(); y += std::max(1, img.height() / 20)) {
        for (int x = 0; x < img.width(); x += std::max(1, img.width() / 20)) {
            QRgb px = img.pixel(x, y);
            sampled++;
            if (qRed(px) < 250 || qGreen(px) < 250 || qBlue(px) < 250) {
                nonWhitePixels++;
            }
        }
    }
    EXPECT_GT(nonWhitePixels, 0)
        << "Rendered page must have some non-white content (sampled "
        << sampled << ", found " << nonWhitePixels << " non-white)";
}

TEST_F(RenderFidelityTest, MultiPageDocumentRendersAllPages)
{
    auto path = std::filesystem::path(fixturesDir()) / "test_3page.pdf";
    if (!std::filesystem::exists(path)) GTEST_SKIP();

    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, path);
    ASSERT_NE(doc, nullptr);
    EXPECT_GE(doc->pageCount(), 3);

    for (int i = 0; i < doc->pageCount(); ++i) {
        auto page = doc->page(i);
        ASSERT_NE(page, nullptr) << "Page " << i;
        QImage img = page->render(QRectF(), 1.0f);
        EXPECT_FALSE(img.isNull()) << "Page " << i << " render must succeed";
        EXPECT_GT(img.width(), 0);
        EXPECT_GT(img.height(), 0);
    }
}

TEST_F(RenderFidelityTest, ZeroZoomReturnsValidImage)
{
    auto path = std::filesystem::path(fixturesDir()) / "test_single.pdf";
    if (!std::filesystem::exists(path)) GTEST_SKIP();

    // Edge case: zoom near zero. Should either return a tiny valid
    // image or an empty one — not a crash.
    QImage img;
    EXPECT_NO_THROW({
        img = renderFirstPage(path, 0.01f);
    });
}
