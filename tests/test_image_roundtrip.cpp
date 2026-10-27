// PHASE5_ROUNDTRIP_IMAGE: insert / replace / delete -> save -> reopen
// -> verify the image-management subsystem actually persists changes
// in the saved PDF. Closes the audit's gap that test_image_manager.cpp
// asserts internal-state-only (image count, returned ImageOperationResult)
// without verifying the bytes on disk.

#include <gtest/gtest.h>

#include "image/ImageManager.h"
#include "image/ImageData.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"

#include "helpers/RoundTripHelper.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

using pdfe::image::ImageManager;
using pdfe::image::InsertImageOptions;
using pdfe::image::PageImageInfo;

namespace {

class ImageRoundTripTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_shared<pdfe::core::MuPdfContext>();
        // test_pdf_with_images.pdf has 2 images on page 0 — covers
        // the remove / replace flows. test_image_rgb.png + the EXIF
        // JPEG live in fixtures and are used as insert sources.
        fixture_ = std::filesystem::path(TEST_FIXTURES_DIR)
                   / "test_pdf_with_images.pdf";
        rgbPng_ = std::filesystem::path(TEST_FIXTURES_DIR)
                  / "test_image_rgb.png";
        exifJpg_ = std::filesystem::path(TEST_FIXTURES_DIR)
                   / "test_image_with_exif.jpg";
        ASSERT_TRUE(std::filesystem::exists(fixture_));
        ASSERT_TRUE(std::filesystem::exists(rgbPng_));

        document_ = pdfe::core::Document::open(ctx_, fixture_);
        ASSERT_NE(document_, nullptr);
        manager_ = std::make_unique<ImageManager>(ctx_);

        const ::testing::TestInfo* info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        tempDir_ = std::filesystem::temp_directory_path()
                   / "pdfe_image_roundtrip" / info->name();
        std::filesystem::create_directories(tempDir_);
    }

    void TearDown() override {
        manager_.reset();
        document_.reset();
        ctx_.reset();
    }

    std::shared_ptr<pdfe::core::MuPdfContext> ctx_;
    std::filesystem::path fixture_;
    std::filesystem::path rgbPng_;
    std::filesystem::path exifJpg_;
    std::filesystem::path tempDir_;
    std::unique_ptr<pdfe::core::Document> document_;
    std::unique_ptr<ImageManager> manager_;
};

// ─── Insert: count increases after save+reopen ──────────────────────

TEST_F(ImageRoundTripTest, InsertImageCountIncreasesAfterReopen) {
    auto page = document_->page(0);
    int countBefore = manager_->imageCount(page.get(), document_.get());

    InsertImageOptions opts;
    opts.filePath = rgbPng_.string();
    opts.targetRect = QRectF(50, 50, 100, 100);
    opts.maintainAspectRatio = false;
    auto result = manager_->insertImage(page.get(), document_.get(), opts);
    ASSERT_TRUE(result.success) << "insertImage failed: " << result.errorMessage;
    page.reset();

    auto reopened = pdfe::test::SaveAndReopen(*document_, ctx_, tempDir_);
    ASSERT_NE(reopened, nullptr);
    auto rePage = reopened->page(0);
    int countAfter = manager_->imageCount(rePage.get(), reopened.get());
    EXPECT_EQ(countAfter, countBefore + 1)
        << "image count must increase by 1 after insertImage + save + reopen";
}

// ─── Insert: dimensions match insertion target ──────────────────────

TEST_F(ImageRoundTripTest, InsertedImageDimensionsMatchAfterReopen) {
    auto page = document_->page(0);
    InsertImageOptions opts;
    opts.filePath = rgbPng_.string();
    opts.targetRect = QRectF(50, 50, 80, 60);
    opts.maintainAspectRatio = false;
    auto result = manager_->insertImage(page.get(), document_.get(), opts);
    ASSERT_TRUE(result.success);
    page.reset();

    auto reopened = pdfe::test::SaveAndReopen(*document_, ctx_, tempDir_);
    ASSERT_NE(reopened, nullptr);
    auto rePage = reopened->page(0);
    auto images = manager_->enumerateImages(rePage.get(), reopened.get());
    ASSERT_FALSE(images.empty());

    // The newest image should have a bounding box near the requested
    // targetRect. We don't lock exact dimensions because PDF CTMs may
    // produce sub-point precision differences; assert the displayed
    // size is within ±2pt.
    bool found = false;
    for (const auto& img : images) {
        if (std::abs(img.displayWidth - 80.0f) < 2.0f &&
            std::abs(img.displayHeight - 60.0f) < 2.0f) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found)
        << "Saved+reopened doc must contain an image with displayed "
           "size ~80x60 pt (the targetRect we requested)";
}

// ─── Remove: count decreases after save+reopen ───────────────────────

TEST_F(ImageRoundTripTest, RemoveImageCountDecreasesAfterReopen) {
    auto page = document_->page(0);
    int countBefore = manager_->imageCount(page.get(), document_.get());
    ASSERT_GE(countBefore, 1)
        << "Fixture must contain at least one image to remove";

    auto result = manager_->removeImage(page.get(), document_.get(), 0);
    ASSERT_TRUE(result.success) << "removeImage failed: " << result.errorMessage;
    page.reset();

    auto reopened = pdfe::test::SaveAndReopen(*document_, ctx_, tempDir_);
    ASSERT_NE(reopened, nullptr);
    auto rePage = reopened->page(0);
    int countAfter = manager_->imageCount(rePage.get(), reopened.get());
    EXPECT_EQ(countAfter, countBefore - 1)
        << "image count must decrease by 1 after removeImage + save + reopen";
}

// ─── Sequential insert + remove + reopen ────────────────────────────

TEST_F(ImageRoundTripTest, InsertThenRemoveLeavesCountUnchangedAfterReopen) {
    auto page = document_->page(0);
    int countBefore = manager_->imageCount(page.get(), document_.get());

    InsertImageOptions opts;
    opts.filePath = rgbPng_.string();
    opts.targetRect = QRectF(60, 60, 50, 50);
    auto inserted = manager_->insertImage(page.get(), document_.get(), opts);
    ASSERT_TRUE(inserted.success);

    int countMid = manager_->imageCount(page.get(), document_.get());
    EXPECT_EQ(countMid, countBefore + 1);

    // Remove the just-inserted image (last one).
    auto removed = manager_->removeImage(page.get(), document_.get(),
                                          countMid - 1);
    ASSERT_TRUE(removed.success);
    page.reset();

    auto reopened = pdfe::test::SaveAndReopen(*document_, ctx_, tempDir_);
    ASSERT_NE(reopened, nullptr);
    int countAfter = manager_->imageCount(reopened->page(0).get(),
                                           reopened.get());
    EXPECT_EQ(countAfter, countBefore)
        << "insert+remove pair must leave image count unchanged on reopen";
}

// ─── EXIF strip: insert JPEG with EXIF, save, reopen, EXIF gone ─────

TEST_F(ImageRoundTripTest, InsertedJpegStripsExifOnSaveByDefault) {
    if (!std::filesystem::exists(exifJpg_)) {
        GTEST_SKIP() << "test_image_with_exif.jpg fixture missing — "
                        "EXIF strip test cannot run without it";
    }
    auto page = document_->page(0);

    InsertImageOptions opts;
    opts.filePath = exifJpg_.string();
    opts.targetRect = QRectF(70, 70, 100, 100);
    opts.stripExif = true;
    auto result = manager_->insertImage(page.get(), document_.get(), opts);
    ASSERT_TRUE(result.success) << result.errorMessage;
    page.reset();

    auto saveResult = pdfe::test::SaveAndReopenWithPath(
        *document_, ctx_, tempDir_, "exif_strip");
    ASSERT_NE(saveResult.doc, nullptr);

    // Read the saved bytes and confirm no JPEG EXIF marker (FF E1
    // tag with 'Exif\0\0') survives. This is a structural check —
    // the marker is the JFIF/EXIF segment header, present iff EXIF
    // was retained.
    std::ifstream f(saveResult.path, std::ios::binary);
    std::string bytes{std::istreambuf_iterator<char>(f),
                      std::istreambuf_iterator<char>()};
    auto exifSig = std::string("Exif\0\0", 6);
    EXPECT_EQ(bytes.find(exifSig), std::string::npos)
        << "EXIF signature 'Exif\\0\\0' must NOT be present in the "
           "saved PDF when stripExif=true";
}

// ─── Image extract round-trip: extract -> bytes equal? ──────────────

TEST_F(ImageRoundTripTest, ExtractedImageIsNonTrivial) {
    auto page = document_->page(0);
    int count = manager_->imageCount(page.get(), document_.get());
    ASSERT_GE(count, 1);

    QImage extracted = manager_->extractImage(page.get(), document_.get(), 0);
    ASSERT_FALSE(extracted.isNull())
        << "extractImage must return a non-null QImage for image 0";
    EXPECT_GT(extracted.width(),  0);
    EXPECT_GT(extracted.height(), 0);
    EXPECT_GT(extracted.sizeInBytes(), 0);
}

// ─── Page count survives image insert + save ────────────────────────

TEST_F(ImageRoundTripTest, ImageInsertPreservesPageCountOnReopen) {
    int originalPageCount = document_->pageCount();

    auto page = document_->page(0);
    InsertImageOptions opts;
    opts.filePath = rgbPng_.string();
    opts.targetRect = QRectF(80, 80, 30, 30);
    manager_->insertImage(page.get(), document_.get(), opts);
    page.reset();

    auto reopened = pdfe::test::SaveAndReopen(*document_, ctx_, tempDir_);
    ASSERT_NE(reopened, nullptr);
    EXPECT_EQ(reopened->pageCount(), originalPageCount)
        << "image insert must not alter the document's page count";
}

// ─── Real-world fixture: jpeg2000_image.pdf passthrough ─────────────

TEST_F(ImageRoundTripTest, Jpeg2000RealworldFixturePassthrough) {
    auto rwPath = std::filesystem::path(TEST_FIXTURES_DIR) / "realworld"
                  / "jpeg2000" / "jpeg2000_image.pdf";
    ASSERT_TRUE(std::filesystem::exists(rwPath));

    auto rwDoc = pdfe::core::Document::open(ctx_, rwPath);
    ASSERT_NE(rwDoc, nullptr);

    // Open -> save -> reopen passthrough. The contract: a PDF whose
    // image stream uses /Filter /JPXDecode (JPEG2000) must survive
    // an open-save-reopen cycle without crashing the writer.
    auto reopened = pdfe::test::SaveAndReopen(*rwDoc, ctx_, tempDir_,
                                               "jpx_pass");
    ASSERT_NE(reopened, nullptr)
        << "saveAs must succeed on a PDF carrying a /JPXDecode image, "
           "even with a placeholder body the JPX decoder cannot decode";
    EXPECT_EQ(reopened->pageCount(), rwDoc->pageCount());
}

} // namespace
