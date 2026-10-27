// Tests for A3 — source image DPI detection.
//
// Verifies Page::sourceImageDpi() correctly infers the native DPI of
// the dominant image XObject across the full range of DPIs represented
// in tests/fixtures/image_pdf/. Iterates the matrix rather than testing
// one fixture, so a detector coincidentally tuned to one DPI cannot
// pass.
//
// See docs/image-pdf-risks.md (A3).

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace pdfe::core;

namespace {

struct DpiFixture {
    const char* filename;
    int expectedDpi;
};

constexpr DpiFixture kSinglePageFixtures[] = {
    {"a3_72dpi_web_letter.pdf",      72},
    {"a3_150dpi_a4_photo.pdf",      150},
    {"a3_200dpi_fax_legal.pdf",     200},
    {"a3_300dpi_office_letter.pdf", 300},
    {"a3_600dpi_archival_a4.pdf",   600},
    {"a3_1200dpi_ocr_letter.pdf",  1200},
};

constexpr int kMixedDpis[] = {72, 150, 300, 600, 1200};

std::string fixturePath(const char* name) {
    return std::string(TEST_FIXTURES_DIR) + "/image_pdf/" + name;
}

bool fixtureExists(const char* name) {
    return std::filesystem::exists(fixturePath(name));
}

} // namespace

class SourceImageDpiTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<MuPdfContext>();
    }

    void TearDown() override {
        doc_.reset();
        context_.reset();
    }

    bool loadFixture(const char* name) {
        auto path = fixturePath(name);
        if (!std::filesystem::exists(path)) return false;
        try {
            doc_ = Document::open(context_, path);
            return doc_ != nullptr;
        } catch (...) {
            return false;
        }
    }

    std::shared_ptr<MuPdfContext> context_;
    std::unique_ptr<Document> doc_;
};

// -----------------------------------------------------------------------
// Matrix test: every single-page fixture is opened and the detected DPI
// is checked against the filename's declared DPI. A 5% tolerance allows
// for CropBox vs MediaBox integer quantization (e.g. 595.2 pt A4
// produces a fractional DPI that may differ by ~1 from the ideal).
// -----------------------------------------------------------------------
TEST_F(SourceImageDpiTest, SinglePageMatrix) {
    int tested = 0;
    for (const auto& fx : kSinglePageFixtures) {
        if (!fixtureExists(fx.filename)) {
            // Fixtures are gitignored; skip gracefully with a message.
            GTEST_LOG_(INFO) << "Skipping " << fx.filename
                             << " (not generated — run gen_fixtures.py)";
            continue;
        }
        ASSERT_TRUE(loadFixture(fx.filename)) << fx.filename;
        auto page = doc_->page(0);
        ASSERT_TRUE(page) << fx.filename;

        double dpi = page->sourceImageDpi();
        double expected = static_cast<double>(fx.expectedDpi);
        double tolerance = expected * 0.05;
        EXPECT_NEAR(dpi, expected, tolerance)
            << fx.filename << " detected " << dpi << " expected " << expected;
        ++tested;
    }
    // At least one fixture must exist for the test to be meaningful.
    if (tested == 0) {
        GTEST_SKIP() << "No A3 fixtures found — run "
                        "tests/fixtures/image_pdf/gen_fixtures.py";
    }
}

// -----------------------------------------------------------------------
// Mixed-DPI 5-page fixture — each page has a different native DPI. This
// asserts that detection is per-page, not a single document-level
// assumption. If the detector caches or defaults, this catches it.
// -----------------------------------------------------------------------
TEST_F(SourceImageDpiTest, MixedDpiMultiPage) {
    if (!fixtureExists("a3_mixed_dpi_5page.pdf")) {
        GTEST_SKIP() << "a3_mixed_dpi_5page.pdf not generated";
    }
    ASSERT_TRUE(loadFixture("a3_mixed_dpi_5page.pdf"));
    ASSERT_EQ(doc_->pageCount(), 5);

    for (int i = 0; i < 5; ++i) {
        auto page = doc_->page(i);
        ASSERT_TRUE(page) << "page " << i;
        double dpi = page->sourceImageDpi();
        double expected = static_cast<double>(kMixedDpis[i]);
        double tolerance = expected * 0.05;
        EXPECT_NEAR(dpi, expected, tolerance)
            << "page " << i << " detected " << dpi
            << " expected " << expected;
    }
}

// -----------------------------------------------------------------------
// Vector-only / no-image pages must return 0.0 so the caller can fall
// back to the default ocrDpi.
// -----------------------------------------------------------------------
TEST_F(SourceImageDpiTest, NoImageReturnsZero) {
    // Use an existing vector-only fixture from tests/fixtures/.
    std::string path = std::string(TEST_FIXTURES_DIR) + "/test_blank.pdf";
    if (!std::filesystem::exists(path)) {
        GTEST_SKIP() << "test_blank.pdf not found";
    }
    doc_ = Document::open(context_, path);
    ASSERT_TRUE(doc_);
    auto page = doc_->page(0);
    ASSERT_TRUE(page);
    // A blank PDF has no image XObjects.
    double dpi = page->sourceImageDpi();
    EXPECT_DOUBLE_EQ(dpi, 0.0);
}

// -----------------------------------------------------------------------
// Round-trip invariant: the detected DPI must be within ±5% of
// (image_pixels / page_width_in_inches). This is the math the test
// verifies externally via verify_fixtures.py; we re-assert it through
// the C++ API to catch implementation drift.
// -----------------------------------------------------------------------
TEST_F(SourceImageDpiTest, RoundTripInvariantAllFixtures) {
    int tested = 0;
    for (const auto& fx : kSinglePageFixtures) {
        if (!fixtureExists(fx.filename)) continue;
        ASSERT_TRUE(loadFixture(fx.filename));
        auto page = doc_->page(0);
        ASSERT_TRUE(page);
        double dpi = page->sourceImageDpi();
        // Sanity: must be in plausible range.
        EXPECT_GT(dpi, 30.0) << fx.filename;
        EXPECT_LT(dpi, 3000.0) << fx.filename;
        // Must match declared DPI within 5%.
        EXPECT_LT(std::abs(dpi - fx.expectedDpi) / fx.expectedDpi, 0.05)
            << fx.filename << " dpi=" << dpi;
        ++tested;
    }
    if (tested == 0) GTEST_SKIP() << "No A3 fixtures available";
}
