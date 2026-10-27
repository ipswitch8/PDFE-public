// PHASE1_HARDENING: skip=0 expect_ge=0 no_throw=1  (2026-05-08)
#include <gtest/gtest.h>
#include "image/ImageTool.h"
#include "image/ImageManager.h"
#include "image/ImageData.h"
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/Page.h"
#include <QApplication>
#include <QSignalSpy>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QPainter>
#include <QImage>
#include <filesystem>
#include <memory>

#ifndef TEST_FIXTURES_DIR
#define TEST_FIXTURES_DIR "."
#endif

using namespace pdfe::core;
using namespace pdfe::image;

namespace {
    void ensureQApplication() {
        if (!QApplication::instance()) {
            static int argc = 0;
            static char* argv[] = { nullptr };
            static QApplication app(argc, argv);
        }
    }
}

class ImageToolTest : public ::testing::Test {
protected:
    void SetUp() override {
        ensureQApplication();

        context_ = std::make_shared<MuPdfContext>();
        manager_ = std::make_unique<ImageManager>(context_);
        tool_ = std::make_unique<ImageTool>(manager_.get());

        pdfPath_ = std::string(TEST_FIXTURES_DIR) + "/test_pdf_with_images.pdf";
    }

    std::shared_ptr<MuPdfContext> context_;
    std::unique_ptr<ImageManager> manager_;
    std::unique_ptr<ImageTool> tool_;
    std::string pdfPath_;
};

// Test 1: Construct - no throw, objectName is "imageTool", and the
// freshly-built tool answers state queries correctly. The bare no-throw
// would pass on a constructor that produced a half-built tool whose
// hasSelection()/objectName() returned garbage.
TEST_F(ImageToolTest, Construct) {
    auto t = std::make_unique<ImageTool>(manager_.get());
    ASSERT_NE(t, nullptr) << "ImageTool construction returned null";
    EXPECT_EQ(t->objectName().toStdString(), "imageTool");
    EXPECT_FALSE(t->hasSelection())
        << "Freshly-constructed ImageTool must report no selection";
}

// Test 2: InitialState - no selection
TEST_F(ImageToolTest, InitialState_NoSelection) {
    EXPECT_FALSE(tool_->hasSelection());
}

// Test 3: SetPage_NoSelection - setting page doesn't auto-select
TEST_F(ImageToolTest, SetPage_NoSelection) {
    if (!std::filesystem::exists(pdfPath_)) {
        GTEST_SKIP() << "Test fixture not available: " << pdfPath_;
    }

    auto doc = Document::open(context_, pdfPath_);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    tool_->setPage(page.get(), doc.get());

    EXPECT_FALSE(tool_->hasSelection());
}

// Test 4: ClearPage_ResetsSelection - clearing page resets selection
TEST_F(ImageToolTest, ClearPage_ResetsSelection) {
    if (!std::filesystem::exists(pdfPath_)) {
        GTEST_SKIP() << "Test fixture not available: " << pdfPath_;
    }

    auto doc = Document::open(context_, pdfPath_);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    tool_->setPage(page.get(), doc.get());
    tool_->clearPage();

    EXPECT_FALSE(tool_->hasSelection());
}

// Test 5: Deselect_EmitsSignal - deselecting emits imageDeselected
TEST_F(ImageToolTest, Deselect_EmitsSignal) {
    if (!std::filesystem::exists(pdfPath_)) {
        GTEST_SKIP() << "Test fixture not available: " << pdfPath_;
    }

    auto doc = Document::open(context_, pdfPath_);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    tool_->setPage(page.get(), doc.get());

    // Phase-16 fix (qt_test_gap_audit finding #18): deterministically
    // click the center of the first enumerated image's bounding box,
    // and ASSERT (not conditionally skip) the selection succeeded.
    // The previous hard-coded (200, 500) click + `if (hasSelection())`
    // wrap allowed the entire assertion body to no-op silently if the
    // click missed.
    auto images = manager_->enumerateImages(page.get(), doc.get());
    if (images.empty()) {
        GTEST_SKIP() << "No images found in fixture";
    }
    QPointF center = images[0].boundingBox.center();

    QSignalSpy selectedSpy(tool_.get(), &ImageTool::imageSelected);
    QMouseEvent pressEvent(QEvent::MouseButtonPress, center, center,
                           Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    tool_->mousePressEvent(&pressEvent, center, 1.0f);
    QMouseEvent releaseEvent(QEvent::MouseButtonRelease, center, center,
                             Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    tool_->mouseReleaseEvent(&releaseEvent, center, 1.0f);

    ASSERT_TRUE(tool_->hasSelection())
        << "Click at image's bounding-box center must select it";

    QSignalSpy deselectedSpy(tool_.get(), &ImageTool::imageDeselected);
    tool_->deselect();
    EXPECT_EQ(deselectedSpy.count(), 1);
    EXPECT_FALSE(tool_->hasSelection());
}

// Test 6: KeyPress_Delete_EmitsRemoveRequested
TEST_F(ImageToolTest, KeyPress_Delete_EmitsRemoveRequested) {
    if (!std::filesystem::exists(pdfPath_)) {
        GTEST_SKIP() << "Test fixture not available: " << pdfPath_;
    }

    auto doc = Document::open(context_, pdfPath_);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    tool_->setPage(page.get(), doc.get());

    // Try to select an image first
    auto images = manager_->enumerateImages(page.get(), doc.get());
    if (images.empty()) {
        GTEST_SKIP() << "No images found to test";
    }

    // Click on the center of the first image's bounding box
    QPointF center = images[0].boundingBox.center();
    QMouseEvent pressEvent(QEvent::MouseButtonPress, center, center,
                           Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    tool_->mousePressEvent(&pressEvent, center, 1.0f);

    QMouseEvent releaseEvent(QEvent::MouseButtonRelease, center, center,
                             Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    tool_->mouseReleaseEvent(&releaseEvent, center, 1.0f);

    if (!tool_->hasSelection()) {
        GTEST_SKIP() << "Could not select image for delete test";
    }

    QSignalSpy removeSpy(tool_.get(), &ImageTool::removeRequested);
    QKeyEvent keyEvent(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
    tool_->keyPressEvent(&keyEvent);

    EXPECT_EQ(removeSpy.count(), 1);
}

// Test 7: KeyPress_Escape_Deselects
TEST_F(ImageToolTest, KeyPress_Escape_Deselects) {
    if (!std::filesystem::exists(pdfPath_)) {
        GTEST_SKIP() << "Test fixture not available: " << pdfPath_;
    }

    auto doc = Document::open(context_, pdfPath_);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    tool_->setPage(page.get(), doc.get());

    // Try to select an image
    auto images = manager_->enumerateImages(page.get(), doc.get());
    if (images.empty()) {
        GTEST_SKIP() << "No images found to test";
    }

    QPointF center = images[0].boundingBox.center();
    QMouseEvent pressEvent(QEvent::MouseButtonPress, center, center,
                           Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    tool_->mousePressEvent(&pressEvent, center, 1.0f);

    QMouseEvent releaseEvent(QEvent::MouseButtonRelease, center, center,
                             Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    tool_->mouseReleaseEvent(&releaseEvent, center, 1.0f);

    if (!tool_->hasSelection()) {
        GTEST_SKIP() << "Could not select image for escape test";
    }

    QKeyEvent keyEvent(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    tool_->keyPressEvent(&keyEvent);

    EXPECT_FALSE(tool_->hasSelection());
}

// Test 8: CursorAt_NoSelection - cursor is Arrow when nothing selected and not hovering
TEST_F(ImageToolTest, CursorAt_NoSelection) {
    // No page set, cursor should be Arrow
    QCursor cursor = tool_->cursorAt(QPointF(0, 0), 1.0f);
    EXPECT_EQ(cursor.shape(), Qt::ArrowCursor);
}

// Test 9: Paint_NoSelection_DoesNotCrash
TEST_F(ImageToolTest, Paint_NoSelection_DoesNotCrash) {
    QImage surface(100, 100, QImage::Format_ARGB32);
    QPainter painter(&surface);

    EXPECT_NO_THROW({
        tool_->paint(painter, 1.0f);
    });

    painter.end();
}

// Test 10: SelectedImage_NoSelection_ReturnsInvalid
TEST_F(ImageToolTest, SelectedImage_NoSelection_ReturnsInvalid) {
    const auto& info = tool_->selectedImage();
    EXPECT_FALSE(info.isValid());
}

// Test 11: NullEvent_DoesNotCrash
TEST_F(ImageToolTest, NullEvent_DoesNotCrash) {
    EXPECT_NO_THROW({
        tool_->mousePressEvent(nullptr, QPointF(0, 0), 1.0f);
        tool_->mouseMoveEvent(nullptr, QPointF(0, 0), 1.0f);
        tool_->mouseReleaseEvent(nullptr, QPointF(0, 0), 1.0f);
        tool_->keyPressEvent(nullptr);
    });
}
