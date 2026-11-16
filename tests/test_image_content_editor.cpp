#include <gtest/gtest.h>
#include "image/ImageContentEditor.h"
#include "image/ImageManager.h"
#include "image/ImageData.h"
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/Page.h"
#include <QRectF>
#include <QPointF>
#include <filesystem>
#include <memory>
#include <cmath>

#ifndef TEST_FIXTURES_DIR
#define TEST_FIXTURES_DIR "."
#endif

using namespace pdfe::core;
using namespace pdfe::image;

namespace {
// atan2(ctm.b, ctm.a) convention established by
// ImageToolInteractionTest.RealRotation_EnumeratesOrientedBoundingBox:
// a visually-clockwise toolbar rotate of N degrees enumerates to a
// rotationRadians of magnitude N*pi/180 (sign is CW-negative in Y-up user
// space), so oracles below assert on std::abs(rotationRadians).
constexpr float kPi = 3.14159265358979323846f;
inline float degToRad(float deg) { return deg * kPi / 180.0f; }
} // namespace

class ImageContentEditorTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<MuPdfContext>();
        manager_ = std::make_unique<ImageManager>(context_);
        editor_ = std::make_unique<ImageContentEditor>(context_);

        pdfPath_ = std::string(TEST_FIXTURES_DIR) + "/test_pdf_with_images.pdf";
    }

    // Helper to create a temp copy for mutation tests
    std::filesystem::path makeTempCopy(const std::string& suffix) {
        auto tempPath = std::filesystem::temp_directory_path() / ("pdfe_test_editor_" + suffix + ".pdf");
        auto srcDoc = Document::open(context_, pdfPath_);
        srcDoc->saveAs(tempPath);
        return tempPath;
    }

    std::shared_ptr<MuPdfContext> context_;
    std::unique_ptr<ImageManager> manager_;
    std::unique_ptr<ImageContentEditor> editor_;
    std::string pdfPath_;
};

// Test 1: Construct - no throw
TEST_F(ImageContentEditorTest, Construct) {
    EXPECT_NO_THROW({
        auto ed = std::make_unique<ImageContentEditor>(context_);
        EXPECT_NE(ed, nullptr);
    });
}

// Test 2: ResizeImage_Success - the committed placement carries the requested
// (non-round, asymmetric) display dimensions, re-enumerated from the rewritten
// content stream. A boolean-only oracle passed even when the writer discarded
// the requested width/height; this asserts the geometry actually committed.
TEST_F(ImageContentEditorTest, ResizeImage_Success) {
    if (!std::filesystem::exists(pdfPath_)) {
        GTEST_SKIP() << "Test fixture not available: " << pdfPath_;
    }

    auto tempPath = makeTempCopy("resize_ok");
    auto doc = Document::open(context_, tempPath);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    auto before = manager_->enumerateImages(page.get(), doc.get());
    ASSERT_FALSE(before.empty());
    const PageImageInfo img0 = before[0];

    // Arbitrary non-round, asymmetric target so a swap or a hardcoded prior
    // value cannot coincidentally satisfy the oracle.
    const float kNewW = 317.0f;
    const float kNewH = 241.0f;

    bool result = editor_->resizeImage(
        page.get(), doc.get(), img0.xobjectName, kNewW, kNewH,
        static_cast<float>(img0.boundingBox.x()),
        static_cast<float>(img0.boundingBox.y()));
    EXPECT_TRUE(result);

    auto after = manager_->enumerateImages(page.get(), doc.get());
    ASSERT_FALSE(after.empty());
    bool found = false;
    PageImageInfo resized;
    for (const auto& im : after) {
        if (im.xobjectName == img0.xobjectName) { resized = im; found = true; break; }
    }
    ASSERT_TRUE(found) << "Resized image " << img0.xobjectName
                       << " must still be enumerable after resize";

    EXPECT_NEAR(resized.displayWidth, kNewW, 0.5f)
        << "Committed display width must equal the requested " << kNewW;
    EXPECT_NEAR(resized.displayHeight, kNewH, 0.5f)
        << "Committed display height must equal the requested " << kNewH;
    EXPECT_NEAR(static_cast<float>(resized.boundingBox.width()), kNewW, 0.5f);
    EXPECT_NEAR(static_cast<float>(resized.boundingBox.height()), kNewH, 0.5f);

    page.reset();
    doc.reset();
    std::error_code ec;
    std::filesystem::remove(tempPath, ec);
}

// Test 3: ResizeImage_PreservesImageCount - image count unchanged after resize
TEST_F(ImageContentEditorTest, ResizeImage_PreservesImageCount) {
    if (!std::filesystem::exists(pdfPath_)) {
        GTEST_SKIP() << "Test fixture not available: " << pdfPath_;
    }

    auto tempPath = makeTempCopy("resize_count");
    auto doc = Document::open(context_, tempPath);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    int countBefore = manager_->imageCount(page.get(), doc.get());

    editor_->resizeImage(page.get(), doc.get(), "Im0", 300.0f, 250.0f, 50.0f, 50.0f);

    int countAfter = manager_->imageCount(page.get(), doc.get());
    EXPECT_EQ(countBefore, countAfter);

    page.reset();
    doc.reset();
    std::error_code ec;
    std::filesystem::remove(tempPath, ec);
}

// Test 4: ResizeImage_NullPage - returns false
TEST_F(ImageContentEditorTest, ResizeImage_NullPage) {
    if (!std::filesystem::exists(pdfPath_)) {
        GTEST_SKIP() << "Test fixture not available: " << pdfPath_;
    }

    auto doc = Document::open(context_, pdfPath_);
    ASSERT_NE(doc, nullptr);

    bool result = editor_->resizeImage(nullptr, doc.get(), "Im0", 300.0f, 250.0f, 50.0f, 50.0f);
    EXPECT_FALSE(result);
}

// Test 5: ResizeImage_EmptyName - returns false
TEST_F(ImageContentEditorTest, ResizeImage_EmptyName) {
    if (!std::filesystem::exists(pdfPath_)) {
        GTEST_SKIP() << "Test fixture not available: " << pdfPath_;
    }

    auto tempPath = makeTempCopy("resize_empty");
    auto doc = Document::open(context_, tempPath);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    bool result = editor_->resizeImage(page.get(), doc.get(), "", 300.0f, 250.0f, 50.0f, 50.0f);
    EXPECT_FALSE(result);

    page.reset();
    doc.reset();
    std::error_code ec;
    std::filesystem::remove(tempPath, ec);
}

// Test 6: RotateImage_Success - an arbitrary 37-degree rotate commits a CTM
// whose enumerated rotationRadians is ~37deg (NOT 0, NOT a 90deg snap). A
// boolean-only oracle survived a reintroduced 90-degree snap (GitHub #341
// class); this asserts the actual committed orientation.
TEST_F(ImageContentEditorTest, RotateImage_Success) {
    if (!std::filesystem::exists(pdfPath_)) {
        GTEST_SKIP() << "Test fixture not available: " << pdfPath_;
    }

    auto tempPath = makeTempCopy("rotate_ok");
    auto doc = Document::open(context_, tempPath);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    auto before = manager_->enumerateImages(page.get(), doc.get());
    ASSERT_FALSE(before.empty());
    const PageImageInfo img0 = before[0];
    ASSERT_NEAR(img0.rotationRadians, 0.0f, 1e-3f) << "Fixture image starts axis-aligned";
    const QPointF c = img0.boundingBox.center();

    const float kAngleDeg = 37.0f;
    bool result = editor_->rotateImage(
        page.get(), doc.get(), img0.xobjectName, kAngleDeg,
        static_cast<float>(c.x()), static_cast<float>(c.y()),
        img0.displayWidth, img0.displayHeight);
    EXPECT_TRUE(result);

    auto after = manager_->enumerateImages(page.get(), doc.get());
    ASSERT_FALSE(after.empty());
    bool found = false;
    PageImageInfo rotated;
    for (const auto& im : after) {
        if (im.xobjectName == img0.xobjectName) { rotated = im; found = true; break; }
    }
    ASSERT_TRUE(found) << "Rotated image must still be enumerable after rotate";

    const float expectedRad = degToRad(kAngleDeg);         // ~0.6458
    const float ninetyRad   = degToRad(90.0f);             // ~1.5708
    EXPECT_NEAR(std::abs(rotated.rotationRadians), expectedRad, 1e-2f)
        << "A 37-degree rotate must enumerate |rotationRadians| ~= 37*pi/180";
    EXPECT_GT(std::abs(rotated.rotationRadians), 0.1f)
        << "Must NOT collapse to ~0 (rotate ignored)";
    EXPECT_GT(std::abs(std::abs(rotated.rotationRadians) - ninetyRad), 0.1f)
        << "Must NOT snap to ~90 degrees (GitHub #341 regression)";

    page.reset();
    doc.reset();
    std::error_code ec;
    std::filesystem::remove(tempPath, ec);
}

// Test 6b: RotateImage_SecondCallIsAbsolute - rotateImage applies angleDegrees
// as an ABSOLUTE orientation from upright (it rebuilds the CTM from the angle
// alone; additive base+delta composition lives in the UI layer, not here).
// Two consecutive calls (37 then 20) must therefore commit the ABSOLUTE 20deg:
// the second rebuild overwrites the first. This discriminates a regression that
// silently ignores the second call (would leave 37), snaps (0/90), or
// mis-composes additively (would give 57).
TEST_F(ImageContentEditorTest, RotateImage_SecondCallIsAbsolute) {
    if (!std::filesystem::exists(pdfPath_)) {
        GTEST_SKIP() << "Test fixture not available: " << pdfPath_;
    }

    auto tempPath = makeTempCopy("rotate_twice");
    auto doc = Document::open(context_, tempPath);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    auto before = manager_->enumerateImages(page.get(), doc.get());
    ASSERT_FALSE(before.empty());
    const PageImageInfo img0 = before[0];
    const QPointF c = img0.boundingBox.center();

    ASSERT_TRUE(editor_->rotateImage(
        page.get(), doc.get(), img0.xobjectName, 37.0f,
        static_cast<float>(c.x()), static_cast<float>(c.y()),
        img0.displayWidth, img0.displayHeight))
        << "First rotate must succeed";
    ASSERT_TRUE(editor_->rotateImage(
        page.get(), doc.get(), img0.xobjectName, 20.0f,
        static_cast<float>(c.x()), static_cast<float>(c.y()),
        img0.displayWidth, img0.displayHeight))
        << "Second rotate must succeed";

    auto after = manager_->enumerateImages(page.get(), doc.get());
    ASSERT_FALSE(after.empty());
    bool found = false;
    PageImageInfo rotated;
    for (const auto& im : after) {
        if (im.xobjectName == img0.xobjectName) { rotated = im; found = true; break; }
    }
    ASSERT_TRUE(found) << "Twice-rotated image must still be enumerable";

    const float abs20 = std::abs(rotated.rotationRadians);
    EXPECT_NEAR(abs20, degToRad(20.0f), 1e-2f)
        << "Second call is ABSOLUTE: final orientation must be 20deg, not the "
           "first 37deg, not an additive 57deg, not a 0/90 snap";
    // Explicit anti-aliases so a wrong composition cannot sneak through.
    EXPECT_GT(std::abs(abs20 - degToRad(37.0f)), 0.05f) << "Not the stale first 37deg";
    EXPECT_GT(std::abs(abs20 - degToRad(57.0f)), 0.05f) << "Not an additive 57deg";

    page.reset();
    doc.reset();
    std::error_code ec;
    std::filesystem::remove(tempPath, ec);
}

// Test 7: RotateImage_PreservesImageCount - image count unchanged after rotate
TEST_F(ImageContentEditorTest, RotateImage_PreservesImageCount) {
    if (!std::filesystem::exists(pdfPath_)) {
        GTEST_SKIP() << "Test fixture not available: " << pdfPath_;
    }

    auto tempPath = makeTempCopy("rotate_count");
    auto doc = Document::open(context_, tempPath);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    int countBefore = manager_->imageCount(page.get(), doc.get());

    editor_->rotateImage(page.get(), doc.get(), "Im0", 90.0f, 200.0f, 300.0f, 200.0f, 200.0f);

    int countAfter = manager_->imageCount(page.get(), doc.get());
    EXPECT_EQ(countBefore, countAfter);

    page.reset();
    doc.reset();
    std::error_code ec;
    std::filesystem::remove(tempPath, ec);
}

// Test 8: RotateImage_NullDoc - returns false
TEST_F(ImageContentEditorTest, RotateImage_NullDoc) {
    if (!std::filesystem::exists(pdfPath_)) {
        GTEST_SKIP() << "Test fixture not available: " << pdfPath_;
    }

    auto doc = Document::open(context_, pdfPath_);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    bool result = editor_->rotateImage(page.get(), nullptr, "Im0", 90.0f, 200.0f, 300.0f, 200.0f, 200.0f);
    EXPECT_FALSE(result);
}

// Test 9: CropImage_Success - the committed placement carries the requested
// ASYMMETRIC display extent, re-enumerated from the rewritten content stream.
// (enumerateImages derives geometry from the image draw CTM, which the crop
// writer composes from displayWidth/displayHeight; a boolean-only oracle
// survived a writer that discarded those.)
TEST_F(ImageContentEditorTest, CropImage_Success) {
    if (!std::filesystem::exists(pdfPath_)) {
        GTEST_SKIP() << "Test fixture not available: " << pdfPath_;
    }

    auto tempPath = makeTempCopy("crop_ok");
    auto doc = Document::open(context_, tempPath);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    auto before = manager_->enumerateImages(page.get(), doc.get());
    ASSERT_FALSE(before.empty());
    const PageImageInfo img0 = before[0];

    // Asymmetric crop rect AND asymmetric committed display extent, so neither
    // a swap nor a hardcoded prior (square) value can satisfy the oracle.
    QRectF cropRect(37.0, 59.0, 83.0, 61.0);
    const float kDispW = 180.0f;
    const float kDispH = 120.0f;

    bool result = editor_->cropImage(
        page.get(), doc.get(), img0.xobjectName, cropRect, kDispW, kDispH,
        static_cast<float>(img0.boundingBox.x()),
        static_cast<float>(img0.boundingBox.y()));
    EXPECT_TRUE(result);

    auto after = manager_->enumerateImages(page.get(), doc.get());
    ASSERT_FALSE(after.empty());
    bool found = false;
    PageImageInfo cropped;
    for (const auto& im : after) {
        if (im.xobjectName == img0.xobjectName) { cropped = im; found = true; break; }
    }
    ASSERT_TRUE(found) << "Cropped image " << img0.xobjectName
                       << " must still be enumerable after crop";

    EXPECT_NEAR(cropped.displayWidth, kDispW, 0.5f)
        << "Committed display width must equal the requested " << kDispW;
    EXPECT_NEAR(cropped.displayHeight, kDispH, 0.5f)
        << "Committed display height must equal the requested " << kDispH;
    EXPECT_NEAR(static_cast<float>(cropped.boundingBox.width()), kDispW, 0.5f);
    EXPECT_NEAR(static_cast<float>(cropped.boundingBox.height()), kDispH, 0.5f);
    // Guard the asymmetry explicitly (a square-collapsing mutation must fail).
    EXPECT_GT(std::abs(cropped.displayWidth - cropped.displayHeight), 10.0f)
        << "Committed extent must stay asymmetric (180x120)";

    page.reset();
    doc.reset();
    std::error_code ec;
    std::filesystem::remove(tempPath, ec);
}

// Test 10: CropImage_PreservesImageCount - image count unchanged after crop
TEST_F(ImageContentEditorTest, CropImage_PreservesImageCount) {
    if (!std::filesystem::exists(pdfPath_)) {
        GTEST_SKIP() << "Test fixture not available: " << pdfPath_;
    }

    auto tempPath = makeTempCopy("crop_count");
    auto doc = Document::open(context_, tempPath);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    int countBefore = manager_->imageCount(page.get(), doc.get());

    QRectF cropRect(110.0, 110.0, 80.0, 80.0);
    editor_->cropImage(page.get(), doc.get(), "Im0", cropRect, 200.0f, 200.0f, 100.0f, 100.0f);

    int countAfter = manager_->imageCount(page.get(), doc.get());
    EXPECT_EQ(countBefore, countAfter);

    page.reset();
    doc.reset();
    std::error_code ec;
    std::filesystem::remove(tempPath, ec);
}

// Test 11: CropImage_EmptyName - returns false
TEST_F(ImageContentEditorTest, CropImage_EmptyName) {
    if (!std::filesystem::exists(pdfPath_)) {
        GTEST_SKIP() << "Test fixture not available: " << pdfPath_;
    }

    auto tempPath = makeTempCopy("crop_empty");
    auto doc = Document::open(context_, tempPath);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    QRectF cropRect(110.0, 110.0, 80.0, 80.0);
    bool result = editor_->cropImage(page.get(), doc.get(), "", cropRect, 200.0f, 200.0f, 100.0f, 100.0f);
    EXPECT_FALSE(result);

    page.reset();
    doc.reset();
    std::error_code ec;
    std::filesystem::remove(tempPath, ec);
}

// Test 12: NullDoc_AllMethods - all methods return false with null doc
TEST_F(ImageContentEditorTest, NullDoc_AllMethods) {
    if (!std::filesystem::exists(pdfPath_)) {
        GTEST_SKIP() << "Test fixture not available: " << pdfPath_;
    }

    auto tempPath = makeTempCopy("null_doc");
    auto doc = Document::open(context_, tempPath);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    EXPECT_FALSE(editor_->resizeImage(page.get(), nullptr, "Im0", 300.0f, 250.0f, 50.0f, 50.0f));
    EXPECT_FALSE(editor_->rotateImage(page.get(), nullptr, "Im0", 90.0f, 200.0f, 300.0f, 200.0f, 200.0f));

    QRectF cropRect(110.0, 110.0, 80.0, 80.0);
    EXPECT_FALSE(editor_->cropImage(page.get(), nullptr, "Im0", cropRect, 200.0f, 200.0f, 100.0f, 100.0f));

    page.reset();
    doc.reset();
    std::error_code ec;
    std::filesystem::remove(tempPath, ec);
}
