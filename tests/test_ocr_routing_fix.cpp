// test_ocr_routing_fix.cpp
// Tests for:
// 1. Page::isImageDominated() detection of OCR'd/scanned pages
// 2. ImageCommands markPageDirty integration (all 5 command types, execute/undo/redo)
// 3. TileRenderer stale render rejection via sequence counter
// 4. MainWindow routing integration: OCR pages → TRE, native text → TextEditTool

#include <gtest/gtest.h>
#include "core/CoverageGeometry.h"
#include "core/Document.h"
#include "core/Page.h"
#include "core/MuPdfContext.h"
#include "image/ImageCommands.h"
#include "image/ImageManager.h"
#include "image/ImageContentEditor.h"
#include "image/ImageData.h"
#include "rendering/TileRenderer.h"
#include "rendering/TileId.h"
#include "rendering/Tile.h"
#include "ui/MainWindow.h"
#include "ui/PdfCanvas.h"

#include <QAction>
#include <QApplication>
#include <QSignalSpy>
#include <QImage>
#include <filesystem>
#include <memory>
#include <atomic>

#ifndef TEST_FIXTURES_DIR
#define TEST_FIXTURES_DIR "."
#endif

using namespace pdfe::core;

// ============================================================================
// Page::isImageDominated() Tests
// ============================================================================

class IsImageDominatedTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<MuPdfContext>();
        fixturesDir_ = std::filesystem::path(TEST_FIXTURES_DIR);
    }

    std::shared_ptr<MuPdfContext> context_;
    std::filesystem::path fixturesDir_;
};

TEST_F(IsImageDominatedTest, NativeTextPage_NotImageDominated) {
    auto testPdf = fixturesDir_ / "test_single.pdf";
    if (!std::filesystem::exists(testPdf)) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    auto document = Document::open(context_, testPdf);
    ASSERT_NE(document, nullptr);
    ASSERT_GT(document->pageCount(), 0);

    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    // Native text PDF should NOT be image-dominated
    EXPECT_FALSE(page->isImageDominated())
        << "A native text PDF page should not be image-dominated";
}

TEST_F(IsImageDominatedTest, PureScannedPage_IsImageDominated) {
    auto testPdf = fixturesDir_ / "test_scanned.pdf";
    if (!std::filesystem::exists(testPdf)) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    auto document = Document::open(context_, testPdf);
    ASSERT_NE(document, nullptr);
    ASSERT_GT(document->pageCount(), 0);

    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    // 2026-05-06 isImageDominated semantics fix: pure-scan pages
    // (image-only, no embedded text layer) ARE image-dominated.
    // Without this, the TRE/OCR pipeline never runs on pure scans —
    // which is exactly the case where OCR is most needed.
    EXPECT_TRUE(page->isImageDominated())
        << "A pure scanned page should be image-dominated so the OCR "
        << "pipeline can run on it";
}

TEST_F(IsImageDominatedTest, OcrTextPage_IsImageDominated) {
    auto testPdf = fixturesDir_ / "test_ocr_text.pdf";
    if (!std::filesystem::exists(testPdf)) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    auto document = Document::open(context_, testPdf);
    ASSERT_NE(document, nullptr);
    ASSERT_GT(document->pageCount(), 0);

    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    // OCR'd page has large image covering page + invisible OCR text layer
    EXPECT_TRUE(page->isImageDominated())
        << "An OCR'd PDF page (image + text layer) should be image-dominated";
}

TEST_F(IsImageDominatedTest, OcrTextPage_AlsoHasTextContent) {
    auto testPdf = fixturesDir_ / "test_ocr_text.pdf";
    if (!std::filesystem::exists(testPdf)) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    auto document = Document::open(context_, testPdf);
    ASSERT_NE(document, nullptr);
    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    // This demonstrates the routing bug: hasTextContent() returns true
    // for OCR'd PDFs, but isImageDominated() correctly identifies them
    EXPECT_TRUE(page->hasTextContent())
        << "OCR'd page has text content (the hidden OCR layer)";
    EXPECT_TRUE(page->isImageDominated())
        << "OCR'd page is image-dominated (image covers most of page)";
}

TEST_F(IsImageDominatedTest, BlankPage_NotImageDominated) {
    auto testPdf = fixturesDir_ / "test_blank.pdf";
    if (!std::filesystem::exists(testPdf)) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    auto document = Document::open(context_, testPdf);
    ASSERT_NE(document, nullptr);
    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    EXPECT_FALSE(page->isImageDominated())
        << "A blank page should not be image-dominated";
}

// ============================================================================
// MainWindow Routing Integration Tests (MAJOR finding)
// ============================================================================

class RoutingIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        fixturesDir_ = std::filesystem::path(TEST_FIXTURES_DIR);
        // MainWindow creates its own MuPdfContext internally
        mainWindow_ = std::make_unique<pdfe::ui::MainWindow>();
    }

    void TearDown() override {
        mainWindow_.reset();
    }

    QAction* findAction(const QString& objectName) {
        return mainWindow_->findChild<QAction*>(objectName);
    }

    pdfe::ui::PdfCanvas* findCanvas() {
        return mainWindow_->findChild<pdfe::ui::PdfCanvas*>("pdfCanvas");
    }

    std::filesystem::path fixturesDir_;
    std::unique_ptr<pdfe::ui::MainWindow> mainWindow_;
};

TEST_F(RoutingIntegrationTest, NativeTextPdf_RoutesToTextEditTool) {
    auto testPdf = fixturesDir_ / "test_single.pdf";
    if (!std::filesystem::exists(testPdf)) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    // Open a native text PDF
    mainWindow_->openFile(QString::fromStdString(testPdf.string()));
    QApplication::processEvents();

    auto* canvas = findCanvas();
    ASSERT_NE(canvas, nullptr) << "PdfCanvas should exist";
    ASSERT_NE(canvas->document(), nullptr) << "Document should be loaded";

    // Trigger the text edit tool action
    auto* textEditAction = findAction("textEditToolAction");
    ASSERT_NE(textEditAction, nullptr) << "textEditToolAction should exist";
    textEditAction->trigger();
    QApplication::processEvents();

    // For native text PDF: TextEditTool should be active, not TRE
    EXPECT_NE(canvas->textEditTool(), nullptr)
        << "Native text PDF should route to TextEditTool";
    // currentTool may or may not be null (TRE is set on currentTool)
    // The key assertion is that textEditTool IS set
}

TEST_F(RoutingIntegrationTest, OcrPdf_RoutesToTextEditTool) {
    auto testPdf = fixturesDir_ / "test_ocr_text.pdf";
    if (!std::filesystem::exists(testPdf)) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    // Open an OCR'd PDF (image + OCR text layer)
    mainWindow_->openFile(QString::fromStdString(testPdf.string()));
    QApplication::processEvents();

    auto* canvas = findCanvas();
    ASSERT_NE(canvas, nullptr) << "PdfCanvas should exist";
    ASSERT_NE(canvas->document(), nullptr) << "Document should be loaded";

    // Trigger the text edit tool action
    auto* textEditAction = findAction("textEditToolAction");
    ASSERT_NE(textEditAction, nullptr) << "textEditToolAction should exist";
    textEditAction->trigger();
    QApplication::processEvents();

    // OCR PDF has text content (the hidden OCR layer), so it should route
    // to TextEditTool for direct editing. No need to force re-OCR via TRE.
    EXPECT_NE(canvas->textEditTool(), nullptr)
        << "OCR PDF with text layer should route to TextEditTool";
}

TEST_F(RoutingIntegrationTest, ScannedPdf_RoutesToTreTool) {
    auto testPdf = fixturesDir_ / "test_scanned.pdf";
    if (!std::filesystem::exists(testPdf)) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    // Open a pure scanned PDF (image-only, no OCR text layer)
    mainWindow_->openFile(QString::fromStdString(testPdf.string()));
    QApplication::processEvents();

    auto* canvas = findCanvas();
    ASSERT_NE(canvas, nullptr);
    ASSERT_NE(canvas->document(), nullptr);

    auto* textEditAction = findAction("textEditToolAction");
    ASSERT_NE(textEditAction, nullptr);
    textEditAction->trigger();
    QApplication::processEvents();

    // Pure scanned page has no native text → should route to TRE
    EXPECT_EQ(canvas->textEditTool(), nullptr)
        << "Pure scanned PDF should NOT route to TextEditTool";
    EXPECT_NE(canvas->currentTool(), nullptr)
        << "Pure scanned PDF should route to TRE tool";
}

// ============================================================================
// ImageCommands markPageDirty Tests (all 5 command types)
// ============================================================================

class ImageCommandsDirtyTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<MuPdfContext>();
        manager_ = std::make_unique<pdfe::image::ImageManager>(context_);
        editor_ = std::make_unique<pdfe::image::ImageContentEditor>(context_);
        pngPath_ = std::string(TEST_FIXTURES_DIR) + "/test_image_rgb.png";
        singlePdfPath_ = std::string(TEST_FIXTURES_DIR) + "/test_single.pdf";
        imagePdfPath_ = std::string(TEST_FIXTURES_DIR) + "/test_pdf_with_images.pdf";
    }

    // Helper: copy a PDF to temp, open, return doc+page
    struct DocPage {
        std::unique_ptr<Document> doc;
        std::unique_ptr<Page> page;
        std::filesystem::path tempPath;
    };

    DocPage openTempCopy(const std::string& srcPath, const std::string& suffix) {
        DocPage dp;
        dp.tempPath = std::filesystem::temp_directory_path() / ("pdfe_test_dirty_" + suffix + ".pdf");
        {
            auto doc = Document::open(context_, srcPath);
            doc->saveAs(dp.tempPath);
        }
        dp.doc = Document::open(context_, dp.tempPath);
        dp.page = dp.doc->page(0);
        return dp;
    }

    void verifyDisplayList(Document* doc, int pageIndex = 0) {
        void* displayList = doc->createDisplayList(pageIndex);
        ASSERT_NE(displayList, nullptr) << "createDisplayList should succeed after markPageDirty";
        doc->releaseDisplayList(displayList);
    }

    void cleanup(DocPage& dp) {
        dp.page.reset();
        dp.doc.reset();
        std::error_code ec;
        std::filesystem::remove(dp.tempPath, ec);
    }

    // Helper: get first image XObject name from a PDF with images
    std::string getFirstImageXObjectName(Page* page, Document* doc) {
        auto images = manager_->enumerateImages(page, doc);
        if (images.empty()) return "";
        return images[0].xobjectName;
    }

    std::shared_ptr<MuPdfContext> context_;
    std::unique_ptr<pdfe::image::ImageManager> manager_;
    std::unique_ptr<pdfe::image::ImageContentEditor> editor_;
    std::string pngPath_;
    std::string singlePdfPath_;
    std::string imagePdfPath_;
};

// --- InsertImage ---

TEST_F(ImageCommandsDirtyTest, InsertImage_MarksPageDirty) {
    if (!std::filesystem::exists(pngPath_) || !std::filesystem::exists(singlePdfPath_)) {
        GTEST_SKIP() << "Test fixtures not available";
    }

    auto dp = openTempCopy(singlePdfPath_, "insert_exec");
    pdfe::image::InsertImageOptions options;
    options.filePath = pngPath_;
    options.targetRect = QRectF(100.0f, 100.0f, 200.0f, 150.0f);

    pdfe::image::InsertImageCommand cmd(manager_.get(), dp.page.get(), dp.doc.get(), options);
    cmd.execute();
    verifyDisplayList(dp.doc.get());
    cleanup(dp);
}

TEST_F(ImageCommandsDirtyTest, InsertImage_UndoMarksPageDirty) {
    if (!std::filesystem::exists(pngPath_) || !std::filesystem::exists(singlePdfPath_)) {
        GTEST_SKIP() << "Test fixtures not available";
    }

    auto dp = openTempCopy(singlePdfPath_, "insert_undo");
    pdfe::image::InsertImageOptions options;
    options.filePath = pngPath_;
    options.targetRect = QRectF(100.0f, 100.0f, 200.0f, 150.0f);

    pdfe::image::InsertImageCommand cmd(manager_.get(), dp.page.get(), dp.doc.get(), options);
    cmd.execute();
    cmd.undo();
    verifyDisplayList(dp.doc.get());
    cleanup(dp);
}

TEST_F(ImageCommandsDirtyTest, InsertImage_RedoMarksPageDirty) {
    if (!std::filesystem::exists(pngPath_) || !std::filesystem::exists(singlePdfPath_)) {
        GTEST_SKIP() << "Test fixtures not available";
    }

    auto dp = openTempCopy(singlePdfPath_, "insert_redo");
    pdfe::image::InsertImageOptions options;
    options.filePath = pngPath_;
    options.targetRect = QRectF(100.0f, 100.0f, 200.0f, 150.0f);

    pdfe::image::InsertImageCommand cmd(manager_.get(), dp.page.get(), dp.doc.get(), options);
    cmd.execute();
    cmd.undo();
    cmd.redo();
    verifyDisplayList(dp.doc.get());
    cleanup(dp);
}

// --- RemoveImage ---

TEST_F(ImageCommandsDirtyTest, RemoveImage_MarksPageDirty) {
    if (!std::filesystem::exists(imagePdfPath_)) {
        GTEST_SKIP() << "Test fixture not available: " << imagePdfPath_;
    }

    auto dp = openTempCopy(imagePdfPath_, "remove_exec");
    auto images = manager_->enumerateImages(dp.page.get(), dp.doc.get());
    ASSERT_GT(images.size(), 0) << "Test PDF must have at least one image";

    pdfe::image::RemoveImageCommand cmd(manager_.get(), dp.page.get(), dp.doc.get(), 0);
    cmd.execute();
    verifyDisplayList(dp.doc.get());
    cleanup(dp);
}

TEST_F(ImageCommandsDirtyTest, RemoveImage_UndoMarksPageDirty) {
    if (!std::filesystem::exists(imagePdfPath_)) {
        GTEST_SKIP() << "Test fixture not available: " << imagePdfPath_;
    }

    auto dp = openTempCopy(imagePdfPath_, "remove_undo");
    auto images = manager_->enumerateImages(dp.page.get(), dp.doc.get());
    ASSERT_GT(images.size(), 0);

    pdfe::image::RemoveImageCommand cmd(manager_.get(), dp.page.get(), dp.doc.get(), 0);
    cmd.execute();
    cmd.undo();
    verifyDisplayList(dp.doc.get());
    cleanup(dp);
}

TEST_F(ImageCommandsDirtyTest, RemoveImage_RedoMarksPageDirty) {
    if (!std::filesystem::exists(imagePdfPath_)) {
        GTEST_SKIP() << "Test fixture not available: " << imagePdfPath_;
    }

    auto dp = openTempCopy(imagePdfPath_, "remove_redo");
    auto images = manager_->enumerateImages(dp.page.get(), dp.doc.get());
    ASSERT_GT(images.size(), 0);

    pdfe::image::RemoveImageCommand cmd(manager_.get(), dp.page.get(), dp.doc.get(), 0);
    cmd.execute();
    cmd.undo();
    cmd.redo();
    verifyDisplayList(dp.doc.get());
    cleanup(dp);
}

// --- ResizeImage ---

TEST_F(ImageCommandsDirtyTest, ResizeImage_MarksPageDirty) {
    if (!std::filesystem::exists(imagePdfPath_)) {
        GTEST_SKIP() << "Test fixture not available: " << imagePdfPath_;
    }

    auto dp = openTempCopy(imagePdfPath_, "resize_exec");
    auto xobjName = getFirstImageXObjectName(dp.page.get(), dp.doc.get());
    ASSERT_FALSE(xobjName.empty()) << "Need an image XObject name";

    pdfe::image::ResizeImageCommand cmd(manager_.get(), editor_.get(),
        dp.page.get(), dp.doc.get(), xobjName, 150.0f, 100.0f, 50.0f, 50.0f);
    cmd.execute();
    verifyDisplayList(dp.doc.get());
    cleanup(dp);
}

TEST_F(ImageCommandsDirtyTest, ResizeImage_UndoMarksPageDirty) {
    if (!std::filesystem::exists(imagePdfPath_)) {
        GTEST_SKIP() << "Test fixture not available: " << imagePdfPath_;
    }

    auto dp = openTempCopy(imagePdfPath_, "resize_undo");
    auto xobjName = getFirstImageXObjectName(dp.page.get(), dp.doc.get());
    ASSERT_FALSE(xobjName.empty());

    pdfe::image::ResizeImageCommand cmd(manager_.get(), editor_.get(),
        dp.page.get(), dp.doc.get(), xobjName, 150.0f, 100.0f, 50.0f, 50.0f);
    cmd.execute();
    cmd.undo();
    verifyDisplayList(dp.doc.get());
    cleanup(dp);
}

TEST_F(ImageCommandsDirtyTest, ResizeImage_RedoMarksPageDirty) {
    if (!std::filesystem::exists(imagePdfPath_)) {
        GTEST_SKIP() << "Test fixture not available: " << imagePdfPath_;
    }

    auto dp = openTempCopy(imagePdfPath_, "resize_redo");
    auto xobjName = getFirstImageXObjectName(dp.page.get(), dp.doc.get());
    ASSERT_FALSE(xobjName.empty());

    pdfe::image::ResizeImageCommand cmd(manager_.get(), editor_.get(),
        dp.page.get(), dp.doc.get(), xobjName, 150.0f, 100.0f, 50.0f, 50.0f);
    cmd.execute();
    cmd.undo();
    cmd.redo();
    verifyDisplayList(dp.doc.get());
    cleanup(dp);
}

// --- RotateImage ---

TEST_F(ImageCommandsDirtyTest, RotateImage_MarksPageDirty) {
    if (!std::filesystem::exists(imagePdfPath_)) {
        GTEST_SKIP() << "Test fixture not available: " << imagePdfPath_;
    }

    auto dp = openTempCopy(imagePdfPath_, "rotate_exec");
    auto images = manager_->enumerateImages(dp.page.get(), dp.doc.get());
    ASSERT_GT(images.size(), 0);
    auto& img = images[0];

    pdfe::image::RotateImageCommand cmd(manager_.get(), editor_.get(),
        dp.page.get(), dp.doc.get(), img.xobjectName,
        90.0f,  // 90 degrees clockwise
        img.boundingBox.center().x(), img.boundingBox.center().y(),
        img.displayWidth, img.displayHeight);
    cmd.execute();
    verifyDisplayList(dp.doc.get());
    cleanup(dp);
}

TEST_F(ImageCommandsDirtyTest, RotateImage_UndoMarksPageDirty) {
    if (!std::filesystem::exists(imagePdfPath_)) {
        GTEST_SKIP() << "Test fixture not available: " << imagePdfPath_;
    }

    auto dp = openTempCopy(imagePdfPath_, "rotate_undo");
    auto images = manager_->enumerateImages(dp.page.get(), dp.doc.get());
    ASSERT_GT(images.size(), 0);
    auto& img = images[0];

    pdfe::image::RotateImageCommand cmd(manager_.get(), editor_.get(),
        dp.page.get(), dp.doc.get(), img.xobjectName,
        90.0f, img.boundingBox.center().x(), img.boundingBox.center().y(),
        img.displayWidth, img.displayHeight);
    cmd.execute();
    cmd.undo();
    verifyDisplayList(dp.doc.get());
    cleanup(dp);
}

TEST_F(ImageCommandsDirtyTest, RotateImage_RedoMarksPageDirty) {
    if (!std::filesystem::exists(imagePdfPath_)) {
        GTEST_SKIP() << "Test fixture not available: " << imagePdfPath_;
    }

    auto dp = openTempCopy(imagePdfPath_, "rotate_redo");
    auto images = manager_->enumerateImages(dp.page.get(), dp.doc.get());
    ASSERT_GT(images.size(), 0);
    auto& img = images[0];

    pdfe::image::RotateImageCommand cmd(manager_.get(), editor_.get(),
        dp.page.get(), dp.doc.get(), img.xobjectName,
        90.0f, img.boundingBox.center().x(), img.boundingBox.center().y(),
        img.displayWidth, img.displayHeight);
    cmd.execute();
    cmd.undo();
    cmd.redo();
    verifyDisplayList(dp.doc.get());
    cleanup(dp);
}

// --- CropImage ---

TEST_F(ImageCommandsDirtyTest, CropImage_MarksPageDirty) {
    if (!std::filesystem::exists(imagePdfPath_)) {
        GTEST_SKIP() << "Test fixture not available: " << imagePdfPath_;
    }

    auto dp = openTempCopy(imagePdfPath_, "crop_exec");
    auto images = manager_->enumerateImages(dp.page.get(), dp.doc.get());
    ASSERT_GT(images.size(), 0);
    auto& img = images[0];

    // Crop to inner 50% of the image
    QRectF cropRect(
        img.boundingBox.x() + img.displayWidth * 0.25f,
        img.boundingBox.y() + img.displayHeight * 0.25f,
        img.displayWidth * 0.5f,
        img.displayHeight * 0.5f);

    pdfe::image::CropImageCommand cmd(manager_.get(), editor_.get(),
        dp.page.get(), dp.doc.get(), img.xobjectName,
        cropRect, img.displayWidth * 0.5f, img.displayHeight * 0.5f,
        img.boundingBox.x(), img.boundingBox.y());
    cmd.execute();
    verifyDisplayList(dp.doc.get());
    cleanup(dp);
}

TEST_F(ImageCommandsDirtyTest, CropImage_UndoMarksPageDirty) {
    if (!std::filesystem::exists(imagePdfPath_)) {
        GTEST_SKIP() << "Test fixture not available: " << imagePdfPath_;
    }

    auto dp = openTempCopy(imagePdfPath_, "crop_undo");
    auto images = manager_->enumerateImages(dp.page.get(), dp.doc.get());
    ASSERT_GT(images.size(), 0);
    auto& img = images[0];

    QRectF cropRect(
        img.boundingBox.x() + img.displayWidth * 0.25f,
        img.boundingBox.y() + img.displayHeight * 0.25f,
        img.displayWidth * 0.5f,
        img.displayHeight * 0.5f);

    pdfe::image::CropImageCommand cmd(manager_.get(), editor_.get(),
        dp.page.get(), dp.doc.get(), img.xobjectName,
        cropRect, img.displayWidth * 0.5f, img.displayHeight * 0.5f,
        img.boundingBox.x(), img.boundingBox.y());
    cmd.execute();
    cmd.undo();
    verifyDisplayList(dp.doc.get());
    cleanup(dp);
}

TEST_F(ImageCommandsDirtyTest, CropImage_RedoMarksPageDirty) {
    if (!std::filesystem::exists(imagePdfPath_)) {
        GTEST_SKIP() << "Test fixture not available: " << imagePdfPath_;
    }

    auto dp = openTempCopy(imagePdfPath_, "crop_redo");
    auto images = manager_->enumerateImages(dp.page.get(), dp.doc.get());
    ASSERT_GT(images.size(), 0);
    auto& img = images[0];

    QRectF cropRect(
        img.boundingBox.x() + img.displayWidth * 0.25f,
        img.boundingBox.y() + img.displayHeight * 0.25f,
        img.displayWidth * 0.5f,
        img.displayHeight * 0.5f);

    pdfe::image::CropImageCommand cmd(manager_.get(), editor_.get(),
        dp.page.get(), dp.doc.get(), img.xobjectName,
        cropRect, img.displayWidth * 0.5f, img.displayHeight * 0.5f,
        img.boundingBox.x(), img.boundingBox.y());
    cmd.execute();
    cmd.undo();
    cmd.redo();
    verifyDisplayList(dp.doc.get());
    cleanup(dp);
}

// ============================================================================
// TileRenderer Sequence Counter Tests
// ============================================================================

class TileRendererSequenceTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<MuPdfContext>();
        std::filesystem::path fixturesDir = TEST_FIXTURES_DIR;
        pdfPath_ = fixturesDir / "test_3page.pdf";

        if (!std::filesystem::exists(pdfPath_)) {
            GTEST_SKIP() << "test_3page.pdf not available";
        }

        document_ = Document::open(context_, pdfPath_);
        renderer_ = std::make_unique<pdfe::rendering::TileRenderer>(document_.get());
    }

    void TearDown() override {
        renderer_.reset();
        document_.reset();
        context_.reset();
    }

    pdfe::rendering::TileId createTileId(int pageIndex = 0, int col = 0, int row = 0, float zoom = 1.0f) {
        pdfe::rendering::TileId id;
        id.pageIndex = pageIndex;
        id.col = col;
        id.row = row;
        id.zoom = zoom;
        return id;
    }

    std::shared_ptr<MuPdfContext> context_;
    std::filesystem::path pdfPath_;
    std::unique_ptr<Document> document_;
    std::unique_ptr<pdfe::rendering::TileRenderer> renderer_;
};

TEST_F(TileRendererSequenceTest, CancelAndReRequest_GetsCorrectTile) {
    // This test validates the race condition fix:
    // Request tile, cancel, re-request -- only the second request's callback fires
    QSignalSpy spy(renderer_.get(), &pdfe::rendering::TileRenderer::tileReady);
    ASSERT_TRUE(spy.isValid());

    std::atomic<int> staleCallbackCount{0};
    std::atomic<int> freshCallbackCount{0};

    auto id = createTileId(0, 0, 0, 1.0f);

    // First request (will become stale)
    renderer_->requestTile(id, [&](pdfe::rendering::TileId, QImage) {
        staleCallbackCount++;
    });

    // Cancel immediately
    renderer_->cancelPage(0);

    // Re-request with fresh callback
    renderer_->requestTile(id, [&](pdfe::rendering::TileId, QImage img) {
        freshCallbackCount++;
        EXPECT_FALSE(img.isNull());
    });

    // Wait for the fresh tile
    bool received = false;
    for (int i = 0; i < 20 && !received; ++i) {
        spy.wait(500);
        for (int j = 0; j < spy.count(); ++j) {
            auto tid = spy.at(j).at(0).value<pdfe::rendering::TileId>();
            if (tid == id) {
                received = true;
            }
        }
    }

    EXPECT_TRUE(received) << "Fresh tile should have been received";
    EXPECT_EQ(freshCallbackCount.load(), 1) << "Fresh callback should fire exactly once";
    // Stale callback should NOT have fired (the sequence check prevents it)
    EXPECT_EQ(staleCallbackCount.load(), 0) << "Stale callback should not fire";
}

TEST_F(TileRendererSequenceTest, CancelAndReRequest_MultipleCycles) {
    // Test multiple cancel/re-request cycles -- only the final callback fires
    QSignalSpy spy(renderer_.get(), &pdfe::rendering::TileRenderer::tileReady);

    auto id = createTileId(0, 0, 0, 1.0f);
    std::atomic<int> finalCallbackCount{0};

    // Do 3 cancel/re-request cycles with small gaps to let thread pool drain
    for (int i = 0; i < 3; ++i) {
        renderer_->requestTile(id, nullptr);
        renderer_->cancelPage(0);
        // Let pending tasks process before next cycle
        spy.wait(100);
    }

    // Final request with tracked callback
    renderer_->requestTile(id, [&](pdfe::rendering::TileId, QImage img) {
        finalCallbackCount++;
        EXPECT_FALSE(img.isNull());
    });

    // Wait for the final tile
    bool received = false;
    for (int i = 0; i < 20 && !received; ++i) {
        spy.wait(500);
        for (int j = 0; j < spy.count(); ++j) {
            auto tid = spy.at(j).at(0).value<pdfe::rendering::TileId>();
            if (tid == id) received = true;
        }
    }

    EXPECT_TRUE(received) << "Final tile should arrive";
    EXPECT_EQ(finalCallbackCount.load(), 1) << "Only the final callback should fire";
}

TEST_F(TileRendererSequenceTest, NormalRequest_StillWorks) {
    // Sanity check: normal request/response works after the sequence counter changes
    QSignalSpy spy(renderer_.get(), &pdfe::rendering::TileRenderer::tileReady);
    ASSERT_TRUE(spy.isValid());

    std::atomic<bool> gotCallback{false};
    auto id = createTileId(1, 0, 0, 1.0f);

    renderer_->requestTile(id, [&](pdfe::rendering::TileId tid, QImage img) {
        EXPECT_EQ(tid, id);
        EXPECT_FALSE(img.isNull());
        gotCallback.store(true);
    });

    ASSERT_TRUE(spy.wait(10000)) << "Tile should be delivered";
    EXPECT_TRUE(gotCallback.load()) << "Callback should have fired";
}

// ============================================================================
// Field report 2026-08-28 — a real 16-page scanned manual would not OCR at all.
//
// Page::isImageDominated() gates the ENTIRE OCR path, and it failed twice over:
//
//   1. It summed SIGNED bbox areas. An image placed off the edge of the page
//      gets its far edge clipped to the page boundary while the near edge stays
//      outside, producing an INVERTED bbox (x0 > x1) whose area is NEGATIVE.
//      Measured on that manual, page 2: page x1 = 297.6, block bbox =
//      (336.0, 242.8, 297.6, 356.2). Seven of sixteen pages carried one, one
//      page carried six, and three pages summed to a NEGATIVE total -- at which
//      point no amount of real image content can pass a `> 0.5` test.
//
//      This is NOT a mirrored/negative-determinant CTM; MuPDF normalises those.
//      A synthetic mirror was built first and reproduced nothing. Recorded
//      because the wrong mechanism is the more expensive mistake here.
//
//   2. The `> 0.5` coverage threshold alone refused pure scans. Every page of
//      that manual has NO extractable text, and with the area fix applied 11 of
//      them still fell under 0.5 -- pages that are nothing but pixels, where
//      OCR is the only way to reach the text at all.
//
// Fixtures are generated by tests/fixtures/make_scan_classification_fixtures.py
// and committed. They are NOT skipped when absent: a missing fixture here means
// the check did not run, and a silent skip would report that as success.
// ============================================================================

class ScanClassificationRegressionTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<MuPdfContext>();
        fixturesDir_ = std::filesystem::path(TEST_FIXTURES_DIR);
    }
    std::shared_ptr<MuPdfContext> context_;
    std::filesystem::path fixturesDir_;

    std::shared_ptr<Page> firstPageOf(const char* name) {
        auto pdf = fixturesDir_ / name;
        EXPECT_TRUE(std::filesystem::exists(pdf))
            << "Fixture missing: " << pdf.string()
            << "\nRegenerate with: python "
               "tests/fixtures/make_scan_classification_fixtures.py";
        if (!std::filesystem::exists(pdf)) return nullptr;
        auto doc = Document::open(context_, pdf);
        EXPECT_NE(doc, nullptr);
        if (!doc) return nullptr;
        auto page = doc->page(0);
        docs_.push_back(std::move(doc));
        return page;
    }
    std::vector<std::shared_ptr<Document>> docs_;
};

// GAP, STATED RATHER THAN PAPERED OVER -- now with MEASURED numbers.
//
// The area-SIGN half of this fix still has no isolating test, and a second
// attempt has now failed for a reason worth recording precisely, because the
// first attempt's note ("the PyMuPDF model does not agree with what the C++
// sums") was true but vague.
//
// A text-bearing fixture was built and tuned until PyMuPDF reported the two
// ratios straddling the threshold:
//
//     hasText = true,  signed = 0.444,  clamped = 0.995
//
// The C++ was then instrumented and run on that same file with the clamp
// reverted to the pre-fix signed sum. It reported:
//
//     hasText=1  area=1759310.8  page=484704.0  ratio=3.6297
//
// Not 0.444. POSITIVE 3.63. The synthetic off-page `cm` placements do not
// produce inverted bboxes in the C++ stext path at all -- they add area
// instead of subtracting it -- so the fixture cannot straddle the threshold
// there however it is tuned from Python.
//
// The sign defect is real and measured on the REPORTED manual (three pages
// summed NEGATIVE; page 2 bbox (336.0, 242.8, 297.6, 356.2) against page
// x1 = 297.6). Its inversion comes from how that producer placed real scanned
// images, which a synthetic `cm` has not reproduced. Closing this gap needs a
// fixture built by TUNING AGAINST THE INSTRUMENTED C++, not against PyMuPDF --
// the loop above is now known to work and takes one rebuild per iteration.
//
// RESOLVED 2026-08-31 — but read what was and was NOT closed.
//
// The gap above says "closing this gap needs a fixture built by TUNING AGAINST
// THE INSTRUMENTED C++". That is still the only known route to a DOCUMENT-level
// reproduction, and it remains unachieved. Nobody has made MuPDF emit an
// inverted bbox from a synthetic PDF.
//
// What was wrong was the assumption that a document-level reproduction is the
// only way to test this. The thing under test is not the document pipeline; it
// is eight lines of pure geometry that merely happened to live inside a lambda.
// They now live in pdfe::core::clampedCoverageArea (core/CoverageGeometry.h),
// called from the shipped code path, and the CoverageGeometryTest cases at the
// bottom of this file exercise them directly with the REAL measured bbox from
// the reported manual. An inverted bbox is trivial to construct as DATA and
// (so far) impossible to construct as a PDF.
//
// So: the stated gap — "the area-sign half has no isolating test" — is closed.
// The document-level reproduction is NOT, and this note stays for whoever wants
// it, rather than being deleted to make the file look finished.
//
// The two cases below cover the pure-scan half only, and both are
// mutation-proved RED against the pre-fix code.

// A pure scan whose images cover well under half the page. No text at all, so
// there is nothing on it to edit except pixels.
TEST_F(ScanClassificationRegressionTest,
       APureScanIsImageDominatedEvenBelowHalfCoverage) {
    auto page = firstPageOf("scan_low_coverage.pdf");
    ASSERT_NE(page, nullptr);
    EXPECT_TRUE(page->isImageDominated())
        << "A page with NO extractable text and real image content is a scan "
           "whatever fraction the images cover. Requiring >0.5 coverage kept "
           "OCR off the documents that need it most.";
}

// Both defects on one page: no text AND an inverted bbox.
TEST_F(ScanClassificationRegressionTest,
       APureScanWithAnOffPageImageIsStillImageDominated) {
    auto page = firstPageOf("scan_offpage_image.pdf");
    ASSERT_NE(page, nullptr);
    EXPECT_TRUE(page->isImageDominated())
        << "Signed area 0.088 vs clamped 0.119 -- the off-page block subtracts, "
           "and the page is a pure scan besides.";
}

// The guard must still be able to say NO. A text page with no images at all is
// not image-dominated, and routing it to OCR would be a regression in the other
// direction -- a classifier that answers TRUE unconditionally is not a
// classifier.
TEST_F(ScanClassificationRegressionTest,
       ATextPageWithNoImagesIsNotImageDominated) {
    auto pdf = fixturesDir_ / "test_single.pdf";
    ASSERT_TRUE(std::filesystem::exists(pdf))
        << "Fixture missing: " << pdf.string();
    auto doc = Document::open(context_, pdf);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);
    EXPECT_FALSE(page->isImageDominated())
        << "A native text page must NOT be classified as a scan; the pure-scan "
           "branch is gated on having no text precisely so this stays false.";
}

// ---------------------------------------------------------------------------
// The area-SIGN half, tested at the level the logic actually lives.
//
// Two earlier attempts tried to reproduce an inverted bbox through a whole PDF
// and both failed: synthetic `cm` placements do not produce inverted bboxes in
// MuPDF's stext path (measured: PyMuPDF said signed 0.444, the instrumented C++
// said ratio=3.6297 on the same file). The numbers below instead come from the
// REPORTED MANUAL, where the defect was measured in the field: page 2 carried
// bbox (336.0, 242.8, 297.6, 356.2) against a page whose x1 was 297.6 — x0 > x1,
// and a block that starts beyond the right edge entirely.
//
// This does NOT reproduce the inversion through MuPDF and does not claim to.
// It closes the stated gap — "the area-sign half has no isolating test" — at the
// level where the arithmetic is.
// ---------------------------------------------------------------------------

namespace {
// The real page and block from the reported manual, page 2.
constexpr double kPageX0 = 0.0, kPageY0 = 0.0;
constexpr double kPageX1 = 297.6, kPageY1 = 420.9;
constexpr double kInvX0 = 336.0, kInvY0 = 242.8;
constexpr double kInvX1 = 297.6, kInvY1 = 356.2;
} // namespace

TEST(CoverageGeometryTest, AnInvertedBboxNeverContributesNegativeArea) {
    // The pre-fix signed product is NEGATIVE: (297.6-336.0) * (356.2-242.8)
    // = -38.4 * 113.4 = -4354.6. That is what drove three pages of the
    // reported manual to a negative running total, past which no amount of
    // real image coverage could pass the ratio test.
    const double signedArea = (kInvX1 - kInvX0) * (kInvY1 - kInvY0);
    ASSERT_LT(signedArea, 0.0) << "fixture numbers no longer invert; the test "
                                  "would prove nothing";

    const double area = pdfe::core::clampedCoverageArea(
        kInvX0, kInvY0, kInvX1, kInvY1, kPageX0, kPageY0, kPageX1, kPageY1);
    EXPECT_GE(area, 0.0);
}

TEST(CoverageGeometryTest, ABlockStartingBeyondTheRightEdgeContributesNothing) {
    // THIS is the one that matters. Normalising alone -- abs() of the signed
    // product -- would yield +4354.6 for a block that is entirely off the page,
    // which is wrong in the opposite direction and invisible to any test that
    // only asserts "not negative". Clamping is the stricter, correct rule:
    // coverage a reader cannot see is not coverage.
    const double area = pdfe::core::clampedCoverageArea(
        kInvX0, kInvY0, kInvX1, kInvY1, kPageX0, kPageY0, kPageX1, kPageY1);
    EXPECT_DOUBLE_EQ(area, 0.0);
}

TEST(CoverageGeometryTest, SixInvertedBlocksCannotDriveCoverageNegative) {
    // One page of the reported manual carried SIX inverted blocks. Under the
    // signed rule they compounded; the sum must now be floored at zero.
    double total = 0.0;
    for (int i = 0; i < 6; ++i) {
        total += pdfe::core::clampedCoverageArea(
            kInvX0, kInvY0, kInvX1, kInvY1, kPageX0, kPageY0, kPageX1, kPageY1);
    }
    EXPECT_GE(total, 0.0);
}

TEST(CoverageGeometryTest, AnOrdinaryOnPageBlockIsUnchanged) {
    // Guards the normal case against the clamp mangling it -- the usual way a
    // fix like this regresses. A mutant that breaks inverted input only must
    // leave this GREEN, which is how we know the mutation is behavioural
    // rather than a blanket break.
    const double area = pdfe::core::clampedCoverageArea(
        10.0, 20.0, 110.0, 220.0, kPageX0, kPageY0, kPageX1, kPageY1);
    EXPECT_DOUBLE_EQ(area, 100.0 * 200.0);
}

TEST(CoverageGeometryTest, APartiallyOffPageBlockCountsOnlyTheVisiblePart) {
    // Half on, half off: the visible half counts, the overhang does not.
    const double area = pdfe::core::clampedCoverageArea(
        kPageX1 - 50.0, 0.0, kPageX1 + 50.0, 100.0,
        kPageX0, kPageY0, kPageX1, kPageY1);
    EXPECT_DOUBLE_EQ(area, 50.0 * 100.0);
}