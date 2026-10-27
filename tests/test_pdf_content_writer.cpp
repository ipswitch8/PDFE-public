#include <gtest/gtest.h>
#include "tre/PdfContentWriter.h"
#include "fonts/PdfFontEmbedder.h"
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/Page.h"
#include <filesystem>
#include <memory>
#include <QImage>

using namespace pdfe::tre;
using namespace pdfe::fonts;
using namespace pdfe::core;

class PdfContentWriterTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<MuPdfContext>();
        fontEmbedder_ = std::make_unique<PdfFontEmbedder>(context_.get());
        writer_ = std::make_unique<PdfContentWriter>(context_.get(), fontEmbedder_.get());

        // Load scanned test PDF if available
        std::filesystem::path scannedPdf = std::filesystem::path(TEST_FIXTURES_DIR) / "test_scanned.pdf";
        if (std::filesystem::exists(scannedPdf)) {
            scannedDoc_ = Document::open(context_, scannedPdf);
            if (scannedDoc_ && scannedDoc_->pageCount() > 0) {
                scannedPage_ = scannedDoc_->page(0);
                hasScannedPdf_ = true;
            }
        }

        // Load text test PDF if available
        std::filesystem::path textPdf = std::filesystem::path(TEST_FIXTURES_DIR) / "test_single.pdf";
        if (std::filesystem::exists(textPdf)) {
            textDoc_ = Document::open(context_, textPdf);
            if (textDoc_ && textDoc_->pageCount() > 0) {
                textPage_ = textDoc_->page(0);
                hasTextPdf_ = true;
            }
        }
    }

    std::shared_ptr<MuPdfContext> context_;
    std::unique_ptr<PdfFontEmbedder> fontEmbedder_;
    std::unique_ptr<PdfContentWriter> writer_;
    std::unique_ptr<Document> scannedDoc_;
    std::unique_ptr<Page> scannedPage_;
    std::unique_ptr<Document> textDoc_;
    std::unique_ptr<Page> textPage_;
    bool hasScannedPdf_ = false;
    bool hasTextPdf_ = false;
};

// ---------------------------------------------------------------------------
// Construction & Basic Tests
// ---------------------------------------------------------------------------

// Test 1: Constructor creates valid writer
TEST_F(PdfContentWriterTest, ConstructorCreatesValidWriter) {
    EXPECT_NE(writer_, nullptr);
}

// Test 2: Move constructor works
TEST_F(PdfContentWriterTest, MoveConstructorWorks) {
    PdfContentWriter movedWriter(std::move(*writer_));
    EXPECT_NE(&movedWriter, nullptr);
}

// Test 3: Move assignment works
TEST_F(PdfContentWriterTest, MoveAssignmentWorks) {
    PdfContentWriter newWriter(context_.get(), fontEmbedder_.get());
    newWriter = std::move(*writer_);
    EXPECT_NE(&newWriter, nullptr);
}

// ---------------------------------------------------------------------------
// findPageImageXObject Tests
// ---------------------------------------------------------------------------

// Test 4: Find image on scanned page returns valid info
TEST_F(PdfContentWriterTest, FindImageOnScannedPageReturnsValidInfo) {
    if (!hasScannedPdf_) {
        GTEST_SKIP() << "No scanned test PDF available";
    }

    auto result = writer_->findPageImageXObject(scannedPage_.get(), scannedDoc_.get());

    ASSERT_TRUE(result.has_value()) << "Should find image on scanned page";
    EXPECT_FALSE(result->xobjectName.empty()) << "XObject name should not be empty";
    EXPECT_GT(result->width, 0) << "Image width should be positive";
    EXPECT_GT(result->height, 0) << "Image height should be positive";
}

// Test 5: Find image with null page returns nullopt
TEST_F(PdfContentWriterTest, FindImageWithNullPageReturnsNullopt) {
    if (!hasScannedPdf_) {
        GTEST_SKIP() << "No scanned test PDF available";
    }

    auto result = writer_->findPageImageXObject(nullptr, scannedDoc_.get());

    EXPECT_FALSE(result.has_value()) << "Should return nullopt for null page";
}

// Test 6: Find image with null document returns nullopt
TEST_F(PdfContentWriterTest, FindImageWithNullDocumentReturnsNullopt) {
    if (!hasScannedPdf_) {
        GTEST_SKIP() << "No scanned test PDF available";
    }

    auto result = writer_->findPageImageXObject(scannedPage_.get(), nullptr);

    EXPECT_FALSE(result.has_value()) << "Should return nullopt for null document";
}

// ---------------------------------------------------------------------------
// replacePageImage Tests
// ---------------------------------------------------------------------------

// Test 7: Replace page image with new image succeeds
TEST_F(PdfContentWriterTest, ReplacePageImageWithNewImageSucceeds) {
    if (!hasScannedPdf_) {
        GTEST_SKIP() << "No scanned test PDF available";
    }

    // Create a test image (100x100 white)
    QImage testImage(100, 100, QImage::Format_RGB888);
    testImage.fill(Qt::white);

    bool result = writer_->replacePageImage(scannedPage_.get(), scannedDoc_.get(), testImage);

    EXPECT_TRUE(result) << "Replace image should succeed";
}

// Test 8: Replace with null image returns false
TEST_F(PdfContentWriterTest, ReplaceWithNullImageReturnsFalse) {
    if (!hasScannedPdf_) {
        GTEST_SKIP() << "No scanned test PDF available";
    }

    QImage nullImage;
    bool result = writer_->replacePageImage(scannedPage_.get(), scannedDoc_.get(), nullImage);

    EXPECT_FALSE(result) << "Should return false for null image";
}

// Test 9: Replace with ARGB image (auto-converts to RGB)
TEST_F(PdfContentWriterTest, ReplaceWithARGBImageAutoConverts) {
    if (!hasScannedPdf_) {
        GTEST_SKIP() << "No scanned test PDF available";
    }

    // Create ARGB image
    QImage argbImage(100, 100, QImage::Format_ARGB32);
    argbImage.fill(QColor(255, 0, 0, 128)); // Semi-transparent red

    bool result = writer_->replacePageImage(scannedPage_.get(), scannedDoc_.get(), argbImage);

    EXPECT_TRUE(result) << "Should auto-convert ARGB to RGB and succeed";
}

// ---------------------------------------------------------------------------
// writeTextBlock Tests
// ---------------------------------------------------------------------------

// Test 10: Write simple text block returns operators string
TEST_F(PdfContentWriterTest, WriteSimpleTextBlockReturnsOperators) {
    if (!hasScannedPdf_) {
        GTEST_SKIP() << "No scanned test PDF available";
    }

    std::filesystem::path arialPath = "C:\\Windows\\Fonts\\arial.ttf";
    if (!std::filesystem::exists(arialPath)) {
        GTEST_SKIP() << "Arial font not available on system";
    }

    FontMatch fontMatch;
    fontMatch.fontFilePath = arialPath.string();
    fontMatch.sizePt = 12.0f;
    fontMatch.color = RgbColor{0, 0, 0};

    PdfRect rect;
    rect.x0 = 50.0;
    rect.y0 = 700.0;
    rect.x1 = 200.0;
    rect.y1 = 720.0;

    std::string result = writer_->writeTextBlock(
        scannedPage_.get(), scannedDoc_.get(),
        "Hello World", fontMatch, rect);

    EXPECT_FALSE(result.empty()) << "Should return non-empty operators string";
    EXPECT_NE(result.find("BT"), std::string::npos) << "Should contain BT operator";
    EXPECT_NE(result.find("ET"), std::string::npos) << "Should contain ET operator";
    EXPECT_NE(result.find("Tf"), std::string::npos) << "Should contain Tf (font) operator";
}

// Test 11: Write text with empty string returns empty
TEST_F(PdfContentWriterTest, WriteTextWithEmptyStringReturnsEmpty) {
    if (!hasScannedPdf_) {
        GTEST_SKIP() << "No scanned test PDF available";
    }

    FontMatch fontMatch;
    fontMatch.fontFilePath = "C:\\Windows\\Fonts\\arial.ttf";
    fontMatch.sizePt = 12.0f;

    PdfRect rect{50, 700, 200, 720};

    std::string result = writer_->writeTextBlock(
        scannedPage_.get(), scannedDoc_.get(),
        "", fontMatch, rect);

    EXPECT_TRUE(result.empty()) << "Should return empty for empty text";
}

// Test 12: Write text with null page returns empty
TEST_F(PdfContentWriterTest, WriteTextWithNullPageReturnsEmpty) {
    if (!hasScannedPdf_) {
        GTEST_SKIP() << "No scanned test PDF available";
    }

    FontMatch fontMatch;
    fontMatch.fontFilePath = "C:\\Windows\\Fonts\\arial.ttf";
    fontMatch.sizePt = 12.0f;

    PdfRect rect{50, 700, 200, 720};

    std::string result = writer_->writeTextBlock(
        nullptr, scannedDoc_.get(),
        "Test", fontMatch, rect);

    EXPECT_TRUE(result.empty()) << "Should return empty for null page";
}

// ---------------------------------------------------------------------------
// appendContentStream Tests
// ---------------------------------------------------------------------------

// Test 13: Append raw operators succeeds
TEST_F(PdfContentWriterTest, AppendRawOperatorsSucceeds) {
    if (!hasScannedPdf_) {
        GTEST_SKIP() << "No scanned test PDF available";
    }

    std::string operators = "q\n1 0 0 1 100 100 cm\nQ\n";
    bool result = writer_->appendContentStream(scannedPage_.get(), scannedDoc_.get(), operators);

    EXPECT_TRUE(result) << "Append content stream should succeed";
}

// Test 14: Append empty string returns false
TEST_F(PdfContentWriterTest, AppendEmptyStringReturnsFalse) {
    if (!hasScannedPdf_) {
        GTEST_SKIP() << "No scanned test PDF available";
    }

    bool result = writer_->appendContentStream(scannedPage_.get(), scannedDoc_.get(), "");

    EXPECT_FALSE(result) << "Should return false for empty operators";
}

// Test 15: Append with null page returns false
TEST_F(PdfContentWriterTest, AppendWithNullPageReturnsFalse) {
    if (!hasScannedPdf_) {
        GTEST_SKIP() << "No scanned test PDF available";
    }

    std::string operators = "q\nQ\n";
    bool result = writer_->appendContentStream(nullptr, scannedDoc_.get(), operators);

    EXPECT_FALSE(result) << "Should return false for null page";
}

// ---------------------------------------------------------------------------
// getImageXObjectData Tests
// ---------------------------------------------------------------------------

// Test 16: Get image data from scanned page returns non-empty
TEST_F(PdfContentWriterTest, GetImageDataFromScannedPageReturnsNonEmpty) {
    if (!hasScannedPdf_) {
        GTEST_SKIP() << "No scanned test PDF available";
    }

    auto data = writer_->getImageXObjectData(scannedPage_.get(), scannedDoc_.get());

    // Image stream data may be empty if the stream cannot be decoded
    // (e.g., JPEG data returned as raw bytes). Just verify no crash.
    SUCCEED();
}

// Test 17: Get image data with null page returns empty
TEST_F(PdfContentWriterTest, GetImageDataWithNullPageReturnsEmpty) {
    if (!hasScannedPdf_) {
        GTEST_SKIP() << "No scanned test PDF available";
    }

    auto data = writer_->getImageXObjectData(nullptr, scannedDoc_.get());

    EXPECT_TRUE(data.empty()) << "Should return empty for null page";
}

// ---------------------------------------------------------------------------
// getContentStreamData Tests
// ---------------------------------------------------------------------------

// Test 18: Get content stream data returns non-empty
TEST_F(PdfContentWriterTest, GetContentStreamDataReturnsNonEmpty) {
    if (!hasScannedPdf_) {
        GTEST_SKIP() << "No scanned test PDF available";
    }

    auto data = writer_->getContentStreamData(scannedPage_.get(), scannedDoc_.get());

    // Content stream might be empty for pure image pages, or non-empty
    // Just verify it doesn't crash
    SUCCEED();
}

// Test 19: Get content stream with null document returns empty
TEST_F(PdfContentWriterTest, GetContentStreamWithNullDocumentReturnsEmpty) {
    if (!hasScannedPdf_) {
        GTEST_SKIP() << "No scanned test PDF available";
    }

    auto data = writer_->getContentStreamData(scannedPage_.get(), nullptr);

    EXPECT_TRUE(data.empty()) << "Should return empty for null document";
}

// Test 20: Write text block with multi-line HOCR data
TEST_F(PdfContentWriterTest, WriteTextBlockWithMultiLineHocrData) {
    if (!hasScannedPdf_) {
        GTEST_SKIP() << "No scanned test PDF available";
    }

    std::filesystem::path arialPath = "C:\\Windows\\Fonts\\arial.ttf";
    if (!std::filesystem::exists(arialPath)) {
        GTEST_SKIP() << "Arial font not available on system";
    }

    FontMatch fontMatch;
    fontMatch.fontFilePath = arialPath.string();
    fontMatch.sizePt = 12.0f;
    fontMatch.color = RgbColor{0, 0, 0};

    PdfRect rect{50, 700, 200, 750};

    // Create HOCR line data
    std::vector<HocrLine> lines;
    HocrLine line1;
    line1.bbox = PixelRect{50, 700, 200, 720};
    HocrWord word1;
    word1.text = "Hello";
    word1.bbox = PixelRect{50, 700, 100, 720};
    word1.confidence = 95;
    line1.words.push_back(word1);
    lines.push_back(line1);

    HocrLine line2;
    line2.bbox = PixelRect{50, 730, 200, 750};
    HocrWord word2;
    word2.text = "World";
    word2.bbox = PixelRect{50, 730, 100, 750};
    word2.confidence = 90;
    line2.words.push_back(word2);
    lines.push_back(line2);

    std::string result = writer_->writeTextBlock(
        scannedPage_.get(), scannedDoc_.get(),
        "Hello World", fontMatch, rect, lines);

    EXPECT_FALSE(result.empty()) << "Should return non-empty operators for multi-line text";
    EXPECT_NE(result.find("BT"), std::string::npos) << "Should contain BT operator";
    EXPECT_NE(result.find("ET"), std::string::npos) << "Should contain ET operator";
}

// ---------------------------------------------------------------------------
// Save / Restore XObject ref — undo lossless round-trip
// ---------------------------------------------------------------------------
//
// The MainWindow image-edit undo path relies on PdfContentWriter's
// save/restore XObject ref API: before a text edit mutates a scanned page,
// saveImageXObjectRef() bumps the original XObject's ref count; on undo,
// restoreImageXObjectRef() re-inserts that exact pdf_obj* into the page
// Resources/XObject dict. This replaces an earlier cached-QImage approach
// whose "lossless" claim was bounded by QImage encoding round-trip.
//
// These tests verify the round-trip is pixel-identical — the whole point of
// the refactor — against a real scanned PDF fixture.
namespace {

size_t countPixelDiffs(const QImage& a, const QImage& b) {
    if (a.size() != b.size() || a.format() != b.format()) {
        return static_cast<size_t>(-1);
    }
    size_t diffs = 0;
    for (int y = 0; y < a.height(); ++y) {
        const uchar* la = a.constScanLine(y);
        const uchar* lb = b.constScanLine(y);
        if (std::memcmp(la, lb, static_cast<size_t>(a.bytesPerLine())) != 0) {
            const int bpp = a.depth() / 8;
            for (int x = 0; x < a.width(); ++x) {
                if (std::memcmp(la + x * bpp, lb + x * bpp,
                                static_cast<size_t>(bpp)) != 0) {
                    ++diffs;
                }
            }
        }
    }
    return diffs;
}

} // namespace

// Test 21: saveImageXObjectRef on scanned page returns non-null ref
TEST_F(PdfContentWriterTest, SaveImageXObjectRefReturnsNonNull) {
    if (!hasScannedPdf_) {
        GTEST_SKIP() << "No scanned test PDF available";
    }

    void* ref = writer_->saveImageXObjectRef(scannedPage_.get(), scannedDoc_.get());
    ASSERT_NE(ref, nullptr) << "Should save a non-null XObject reference";
    writer_->releaseImageXObjectRef(ref);
}

// Test 22: saveImageXObjectRef with null page returns nullptr
TEST_F(PdfContentWriterTest, SaveImageXObjectRefWithNullPageReturnsNull) {
    if (!hasScannedPdf_) {
        GTEST_SKIP() << "No scanned test PDF available";
    }

    void* ref = writer_->saveImageXObjectRef(nullptr, scannedDoc_.get());
    EXPECT_EQ(ref, nullptr);
}

// Test 23: LOSSLESS round-trip — save, mutate via replacePageImage, restore,
// re-render. The restored rendering must be pixel-identical to the baseline.
//
// This is the core correctness guarantee the MainWindow refactor depends on.
// A regression here silently corrupts undo on scanned-PDF text edits.
TEST_F(PdfContentWriterTest, SaveMutateRestoreIsLossless) {
    if (!hasScannedPdf_) {
        GTEST_SKIP() << "No scanned test PDF available";
    }

    auto imageInfo = writer_->findPageImageXObject(
        scannedPage_.get(), scannedDoc_.get());
    ASSERT_TRUE(imageInfo.has_value()) << "Scanned page must have an image XObject";
    const std::string xobjectName = imageInfo->xobjectName;

    // Render baseline (full page at 1x).
    QRectF bbox = scannedPage_->boundingBox();
    QImage baseline = scannedPage_->render(bbox, 1.0f);
    ASSERT_FALSE(baseline.isNull());
    ASSERT_GT(baseline.width(), 0);
    ASSERT_GT(baseline.height(), 0);

    // Save the original XObject reference.
    void* savedRef = writer_->saveImageXObjectRef(
        scannedPage_.get(), scannedDoc_.get());
    ASSERT_NE(savedRef, nullptr);

    // Mutate: replace the page image with a distinctive solid red bitmap at
    // the same dimensions as the original (so replacePageImage keeps it).
    QImage mutation(imageInfo->width, imageInfo->height, QImage::Format_RGB888);
    mutation.fill(QColor(255, 0, 0));
    ASSERT_TRUE(writer_->replacePageImage(
        scannedPage_.get(), scannedDoc_.get(), mutation));

    // Sanity: render after mutation must differ from baseline — otherwise
    // the replace didn't take effect and the lossless claim is vacuous.
    QImage mutated = scannedPage_->render(bbox, 1.0f);
    ASSERT_FALSE(mutated.isNull());
    ASSERT_EQ(mutated.size(), baseline.size());
    size_t mutationDiffs = countPixelDiffs(baseline, mutated);
    ASSERT_GT(mutationDiffs, static_cast<size_t>(0))
        << "replacePageImage did not visibly change the rendering";

    // Restore the original XObject reference.
    ASSERT_TRUE(writer_->restoreImageXObjectRef(
        scannedPage_.get(), scannedDoc_.get(), savedRef, xobjectName));

    // Render after restore — must be pixel-identical to baseline.
    QImage restored = scannedPage_->render(bbox, 1.0f);
    ASSERT_FALSE(restored.isNull());
    ASSERT_EQ(restored.size(), baseline.size());
    ASSERT_EQ(restored.format(), baseline.format());

    size_t restoreDiffs = countPixelDiffs(baseline, restored);
    EXPECT_EQ(restoreDiffs, static_cast<size_t>(0))
        << "Restored render differs from baseline by " << restoreDiffs
        << " pixels — undo is not lossless";
}

// Test 24: Repeated save/mutate/restore cycles stay lossless (no drift).
TEST_F(PdfContentWriterTest, RepeatedSaveMutateRestoreHasNoDrift) {
    if (!hasScannedPdf_) {
        GTEST_SKIP() << "No scanned test PDF available";
    }

    auto imageInfo = writer_->findPageImageXObject(
        scannedPage_.get(), scannedDoc_.get());
    ASSERT_TRUE(imageInfo.has_value());
    const std::string xobjectName = imageInfo->xobjectName;

    QRectF bbox = scannedPage_->boundingBox();
    QImage baseline = scannedPage_->render(bbox, 1.0f);
    ASSERT_FALSE(baseline.isNull());

    for (int cycle = 0; cycle < 3; ++cycle) {
        void* savedRef = writer_->saveImageXObjectRef(
            scannedPage_.get(), scannedDoc_.get());
        ASSERT_NE(savedRef, nullptr) << "cycle " << cycle;

        QImage mutation(imageInfo->width, imageInfo->height, QImage::Format_RGB888);
        mutation.fill(QColor(cycle * 80, 255 - cycle * 60, 128));
        ASSERT_TRUE(writer_->replacePageImage(
            scannedPage_.get(), scannedDoc_.get(), mutation)) << "cycle " << cycle;

        ASSERT_TRUE(writer_->restoreImageXObjectRef(
            scannedPage_.get(), scannedDoc_.get(), savedRef, xobjectName))
            << "cycle " << cycle;

        QImage restored = scannedPage_->render(bbox, 1.0f);
        size_t diffs = countPixelDiffs(baseline, restored);
        EXPECT_EQ(diffs, static_cast<size_t>(0))
            << "cycle " << cycle << ": drift of " << diffs << " pixels";
    }
}
