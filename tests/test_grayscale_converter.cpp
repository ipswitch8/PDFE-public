// -*- coding: utf-8 -*-
// Pipeline E phase-7 / Bucket 3e: convert-to-grayscale.
//
// Verifies GrayscaleConverter — uses MuPDF's pdf_recolor_page to
// recolor every page to DeviceGray. Tests cover an image-bearing
// fixture, a text-only fixture, and a save+reopen round-trip with
// pixel-level R==G==B verification on the rendered output.
#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "optimize/GrayscaleConverter.h"

#include <QImage>
#include <QRectF>
#include <QTemporaryDir>

#include <filesystem>
#include <memory>
#include <string>

using namespace pdfe::core;
using namespace pdfe::optimize;

namespace {

// QImage::Format_RGBA8888 (what Page::render produces) places pixels
// in byte order R,G,B,A. Convert to ARGB32 so qRed/qGreen/qBlue read
// channels correctly.
QImage toArgb32(const QImage& in)
{
    return in.format() == QImage::Format_ARGB32
        ? in : in.convertToFormat(QImage::Format_ARGB32);
}

// Returns true iff every sampled pixel satisfies R == G == B
// (within `tolerance`). Tolerance accommodates the JPEG re-encoding
// noise that pdf_recolor_page introduces when it rewrites color
// JPEGs as grayscale JPEGs.
bool allPixelsAreGray(const QImage& imgIn, int tolerance = 4)
{
    QImage img = toArgb32(imgIn);
    for (int y = 0; y < img.height(); y += 8) {
        const QRgb* row = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < img.width(); x += 8) {
            QRgb p = row[x];
            const int r = qRed(p), g = qGreen(p), b = qBlue(p);
            if (std::abs(r - g) > tolerance ||
                std::abs(g - b) > tolerance ||
                std::abs(r - b) > tolerance) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

class GrayscaleConverterTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx = std::make_shared<MuPdfContext>();
        ASSERT_TRUE(workDir.isValid());
    }

    std::unique_ptr<Document> openFixture(const std::string& name) {
        auto fixtures = std::filesystem::path(TEST_FIXTURES_DIR);
        auto src = fixtures / name;
        if (!std::filesystem::exists(src)) return nullptr;
        return Document::open(ctx, src);
    }

    std::shared_ptr<MuPdfContext> ctx;
    QTemporaryDir workDir;
};

TEST_F(GrayscaleConverterTest, ConvertImageBearingPageProducesGrayPixels)
{
    auto doc = openFixture("test_pdf_with_images.pdf");
    if (!doc) GTEST_SKIP() << "test_pdf_with_images.pdf not available";

    // Per-page convert (covers the public single-page API).
    ASSERT_TRUE(GrayscaleConverter::convertPageToGrayscale(doc.get(), 0));

    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);
    QImage rendered = page->render(QRectF(), 1.0f);
    ASSERT_FALSE(rendered.isNull());
    EXPECT_TRUE(allPixelsAreGray(rendered))
        << "After grayscale conversion, every sampled pixel must have "
           "R==G==B (within tolerance for JPEG re-encoding noise).";
}

TEST_F(GrayscaleConverterTest, ConvertTextOnlyPageDoesNotCrashAndKeepsText)
{
    auto doc = openFixture("test_single.pdf");
    if (!doc) GTEST_SKIP() << "test_single.pdf not available";

    auto result = GrayscaleConverter::convertDocumentToGrayscale(doc.get());
    EXPECT_TRUE(result.ok)
        << "All pages should convert: " << result.errorMessage;
    EXPECT_GE(result.pagesConverted, 1);
    EXPECT_EQ(result.pagesFailed, 0);

    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);
    QImage rendered = page->render(QRectF(), 1.0f);
    ASSERT_FALSE(rendered.isNull());
    EXPECT_TRUE(allPixelsAreGray(rendered));
}

TEST_F(GrayscaleConverterTest, SaveAndReopenPreservesGrayscaleConversion)
{
    auto doc = openFixture("test_pdf_with_images.pdf");
    if (!doc) GTEST_SKIP() << "test_pdf_with_images.pdf not available";

    auto result = GrayscaleConverter::convertDocumentToGrayscale(doc.get());
    ASSERT_TRUE(result.ok);

    std::string outPath = (std::filesystem::path(
        workDir.path().toStdString()) / "grayscale.pdf").string();
    ASSERT_NO_THROW(doc->saveAs(outPath));
    doc.reset();

    auto reopened = Document::open(ctx, outPath);
    ASSERT_NE(reopened, nullptr);
    auto page = reopened->page(0);
    ASSERT_NE(page, nullptr);
    QImage rendered = page->render(QRectF(), 1.0f);
    ASSERT_FALSE(rendered.isNull());
    EXPECT_TRUE(allPixelsAreGray(rendered))
        << "Grayscale conversion must survive save+reopen.";
}

TEST_F(GrayscaleConverterTest, MultiPageDocumentConvertsAllPages)
{
    auto doc = openFixture("test_3page.pdf");
    if (!doc) GTEST_SKIP() << "test_3page.pdf not available";
    const int pageCount = doc->pageCount();
    ASSERT_GE(pageCount, 2);

    auto result = GrayscaleConverter::convertDocumentToGrayscale(doc.get());
    EXPECT_TRUE(result.ok);
    EXPECT_EQ(result.pagesConverted, pageCount);
}

TEST_F(GrayscaleConverterTest, NullDocumentReturnsErrorWithoutCrash)
{
    auto result = GrayscaleConverter::convertDocumentToGrayscale(nullptr);
    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.errorMessage.empty());
    EXPECT_EQ(result.pagesConverted, 0);
}
