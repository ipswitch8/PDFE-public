// Defect #2 — per-region source image DPI.
//
// ImageReplacer historically asked Page::sourceImageDpi() for ONE page-maximum
// DPI and re-rendered every edited region at it, so a 150-DPI thumbnail drawn
// over a 300-DPI photo got upsampled to 300 DPI (blurred). The fix is
// Page::sourceImageDpiAt(region), which returns the native DPI of the image
// actually under the region. ImageReplacer::replace() / replaceWithFontMatch()
// now call it with the edited selection rect.
//
// These tests exercise the per-region DPI core against mixed_dpi_page.pdf, whose
// layout (from generate_render_oracle_fixtures.py::make_mixed_dpi_page) is:
//   * 300-DPI photo  : fitz Rect(51, 51, 561, 651)  (2125 px / (510 pt/72) = 300)
//   * 150-DPI thumb  : fitz Rect(80, 470, 224, 614) (300 px / (144 pt/72) = 150),
//                      drawn ON TOP of the photo, lower-left.

#include <gtest/gtest.h>

#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/Page.h"

#include <QRectF>

#include <filesystem>
#include <memory>

using namespace pdfe::core;

namespace {

constexpr double kDpiTolerance = 0.05;  // 5% — same tolerance as test_source_image_dpi

bool nearDpi(double got, double expected) {
    return std::abs(got - expected) / expected < kDpiTolerance;
}

}  // namespace

class ImageReplacerDpiTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<MuPdfContext>();
    }

    std::unique_ptr<Page> openPage0(const char* fixture) {
        std::filesystem::path p =
            std::filesystem::path(TEST_FIXTURES_DIR) / fixture;
        if (!std::filesystem::exists(p)) {
            return nullptr;
        }
        document_ = Document::open(context_, p);
        if (!document_ || document_->pageCount() == 0) {
            return nullptr;
        }
        return document_->page(0);
    }

    std::shared_ptr<MuPdfContext> context_;
    std::shared_ptr<Document> document_;
};

// Case 1: a region over the 300-DPI photo (and clear of the thumbnail) reports
// ~300 DPI.
TEST_F(ImageReplacerDpiTest, RegionOver300DpiPhotoReports300) {
    auto page = openPage0("mixed_dpi_page.pdf");
    ASSERT_NE(page, nullptr) << "mixed_dpi_page.pdf fixture missing";

    // Upper-left of the photo, well above the lower-left thumbnail (y < 470).
    const QRectF photoRegion(120.0, 120.0, 200.0, 120.0);  // x:120..320 y:120..240
    const double dpi = page->sourceImageDpiAt(photoRegion);
    EXPECT_TRUE(nearDpi(dpi, 300.0))
        << "expected ~300 DPI over the photo region, got " << dpi;
}

// Case 2: a region over the 150-DPI thumbnail reports ~150 DPI — NOT the
// page-maximum 300. This is the defect: the thumbnail overlaps the underlying
// 300-DPI photo, but the topmost/tightest image (the thumbnail) wins.
TEST_F(ImageReplacerDpiTest, RegionOver150DpiThumbnailReports150) {
    auto page = openPage0("mixed_dpi_page.pdf");
    ASSERT_NE(page, nullptr) << "mixed_dpi_page.pdf fixture missing";

    // Fully inside the thumbnail rect (80..224, 470..614).
    const QRectF thumbRegion(120.0, 500.0, 80.0, 80.0);  // x:120..200 y:500..580
    const double dpi = page->sourceImageDpiAt(thumbRegion);
    EXPECT_TRUE(nearDpi(dpi, 150.0))
        << "expected ~150 DPI over the thumbnail region, got " << dpi
        << " (page-maximum heuristic would wrongly return ~300)";
}

// The per-region result genuinely differs across regions of the same page, and
// is strictly better than the page-wide heuristic. sourceImageDpi() divides
// image pixels by the WHOLE-PAGE size, so for this fixture it yields ~250 (the
// 2125px photo spans only 510 of the 612pt page width: 2125/(612/72) ~= 250) —
// already wrong for the photo, whose true native DPI is 300. The per-region
// accessor reports the photo region at its real 300 DPI and the thumbnail at
// 150 — proving the routing is per-region, not one page value.
TEST_F(ImageReplacerDpiTest, PerRegionDiffersFromPageMaximum) {
    auto page = openPage0("mixed_dpi_page.pdf");
    ASSERT_NE(page, nullptr) << "mixed_dpi_page.pdf fixture missing";

    const double pageMax = page->sourceImageDpi();
    EXPECT_GT(pageMax, 0.0) << "page should report some image DPI";

    const QRectF photoRegion(120.0, 120.0, 200.0, 120.0);
    const double photoDpi = page->sourceImageDpiAt(photoRegion);
    const QRectF thumbRegion(120.0, 500.0, 80.0, 80.0);
    const double thumbDpi = page->sourceImageDpiAt(thumbRegion);

    EXPECT_TRUE(nearDpi(photoDpi, 300.0))
        << "photo region per-region DPI " << photoDpi;
    EXPECT_TRUE(nearDpi(thumbDpi, 150.0))
        << "thumbnail region per-region DPI " << thumbDpi;
    EXPECT_LT(thumbDpi, photoDpi * 0.75)
        << "thumbnail region (" << thumbDpi << ") must be well below the photo "
        << "region (" << photoDpi << ") — proving per-region DPI";
    EXPECT_LT(thumbDpi, pageMax)
        << "thumbnail region DPI (" << thumbDpi
        << ") should be below the page-max heuristic (" << pageMax << ")";
}

// Case 3: a page with a single image returns that image's DPI for any region
// over it, and matches the page-maximum (there is only one image).
TEST_F(ImageReplacerDpiTest, SingleImagePageRegionMatchesPageDpi) {
    auto page = openPage0("test_scanned.pdf");
    ASSERT_NE(page, nullptr) << "test_scanned.pdf fixture missing";

    const double pageMax = page->sourceImageDpi();
    ASSERT_GT(pageMax, 0.0) << "test_scanned.pdf should contain an image";

    // A region somewhere over the (full-page) scanned image.
    const QRectF region(100.0, 100.0, 200.0, 120.0);
    const double regionDpi = page->sourceImageDpiAt(region);
    EXPECT_TRUE(nearDpi(regionDpi, pageMax))
        << "single-image page: region DPI " << regionDpi
        << " should match page DPI " << pageMax;
}

// A region clear of every image falls back to the page-maximum DPI rather than
// returning 0 — callers still get a usable working DPI.
TEST_F(ImageReplacerDpiTest, RegionClearOfImagesFallsBackToPageMax) {
    auto page = openPage0("mixed_dpi_page.pdf");
    ASSERT_NE(page, nullptr) << "mixed_dpi_page.pdf fixture missing";

    // Bottom strip of the page, below both images (photo ends y=651, thumb
    // ends y=614; page height 792).
    const QRectF emptyRegion(300.0, 700.0, 120.0, 60.0);
    const double dpi = page->sourceImageDpiAt(emptyRegion);
    EXPECT_TRUE(nearDpi(dpi, page->sourceImageDpi()))
        << "region clear of images should fall back to page-max DPI, got " << dpi;
}

// A vector-only page (no image XObject) returns 0.0 for any region.
TEST_F(ImageReplacerDpiTest, VectorOnlyPageReturnsZero) {
    auto page = openPage0("test_blank.pdf");
    if (!page) {
        GTEST_SKIP() << "test_blank.pdf fixture missing";
    }
    const QRectF region(100.0, 100.0, 200.0, 200.0);
    EXPECT_DOUBLE_EQ(page->sourceImageDpiAt(region), 0.0);
}
