#include <gtest/gtest.h>
#include "core/Document.h"
#include "core/Page.h"
#include "core/MuPdfContext.h"
#include "core/Error.h"
#include <filesystem>
#include <memory>
#include <QImage>
#include <QRectF>

using namespace pdfe::core;

class PageTest : public ::testing::Test {
protected:
    void SetUp() override {
        context = std::make_shared<MuPdfContext>();

        std::filesystem::path fixturesDir = TEST_FIXTURES_DIR;
        testPdf = fixturesDir / "test_3page.pdf";

        if (std::filesystem::exists(testPdf)) {
            document = Document::open(context, testPdf);
        }
    }

    std::shared_ptr<MuPdfContext> context;
    std::filesystem::path testPdf;
    std::unique_ptr<Document> document;
};

TEST_F(PageTest, GetBoundingBox) {
    if (!document) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    ASSERT_GT(document->pageCount(), 0);

    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    QRectF bbox = page->boundingBox();

    EXPECT_GT(bbox.width(), 0.0);
    EXPECT_GT(bbox.height(), 0.0);
}

TEST_F(PageTest, GetRotation) {
    if (!document) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    ASSERT_GT(document->pageCount(), 0);

    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    int rotation = page->rotation();

    // Rotation should be 0, 90, 180, or 270
    EXPECT_TRUE(rotation == 0 || rotation == 90 || rotation == 180 || rotation == 270);
}

TEST_F(PageTest, GetIndex) {
    if (!document) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    ASSERT_GT(document->pageCount(), 0);

    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    EXPECT_EQ(page->index(), 0);
}

TEST_F(PageTest, RenderPage) {
    if (!document) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    ASSERT_GT(document->pageCount(), 0);

    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    QRectF bbox = page->boundingBox();
    QImage image = page->render(bbox, 1.0f);

    EXPECT_FALSE(image.isNull());
    EXPECT_GT(image.width(), 0);
    EXPECT_GT(image.height(), 0);
    EXPECT_EQ(image.format(), QImage::Format_RGBA8888);
}

TEST_F(PageTest, RenderPageWithScaling) {
    if (!document) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    ASSERT_GT(document->pageCount(), 0);

    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    QRectF bbox = page->boundingBox();

    // Render at different scales
    QImage image1x = page->render(bbox, 1.0f);
    QImage image2x = page->render(bbox, 2.0f);

    EXPECT_FALSE(image1x.isNull());
    EXPECT_FALSE(image2x.isNull());

    // 2x scale should produce approximately 2x dimensions
    EXPECT_GT(image2x.width(), image1x.width());
    EXPECT_GT(image2x.height(), image1x.height());

    // Should be roughly 2x (allow some rounding tolerance)
    float widthRatio = static_cast<float>(image2x.width()) / image1x.width();
    float heightRatio = static_cast<float>(image2x.height()) / image1x.height();

    EXPECT_NEAR(widthRatio, 2.0f, 0.1f);
    EXPECT_NEAR(heightRatio, 2.0f, 0.1f);
}

TEST_F(PageTest, PageIsMovable) {
    if (!document) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    ASSERT_GT(document->pageCount(), 0);

    auto page1 = document->page(0);
    int index = page1->index();

    // Move constructor
    auto page2 = std::move(page1);
    EXPECT_EQ(page2->index(), index);

    // Move assignment
    auto page3 = document->page(0);
    page3 = std::move(page2);
    EXPECT_EQ(page3->index(), index);
}

TEST_F(PageTest, MultiplePages) {
    if (!document) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    int pageCount = document->pageCount();
    if (pageCount < 2) {
        GTEST_SKIP() << "Test PDF has only " << pageCount << " page(s), need at least 2";
    }

    auto page0 = document->page(0);
    auto page1 = document->page(1);

    ASSERT_NE(page0, nullptr);
    ASSERT_NE(page1, nullptr);

    EXPECT_EQ(page0->index(), 0);
    EXPECT_EQ(page1->index(), 1);
}

TEST_F(PageTest, BoundingBoxMatchesPageInfo) {
    if (!document) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    ASSERT_GT(document->pageCount(), 0);

    PageInfo info = document->pageInfo(0);
    auto page = document->page(0);
    QRectF bbox = page->boundingBox();

    // Allow small floating point differences
    EXPECT_NEAR(bbox.width(), info.widthPt, 0.1);
    EXPECT_NEAR(bbox.height(), info.heightPt, 0.1);
}
