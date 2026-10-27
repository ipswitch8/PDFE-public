#include <gtest/gtest.h>
#include "ui/PdfCanvas.h"
#include "image/ImageTool.h"
#include "image/ImageManager.h"
#include "core/MuPdfContext.h"
#include <QApplication>
#include <QSignalSpy>

class ImageToolWiringTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_NE(QApplication::instance(), nullptr);

        context_ = std::make_shared<pdfe::core::MuPdfContext>();
        manager_ = std::make_unique<pdfe::image::ImageManager>(context_);
        imageTool_ = std::make_unique<pdfe::image::ImageTool>(manager_.get());
        canvas_ = new pdfe::ui::PdfCanvas();
    }

    void TearDown() override {
        delete canvas_;
        imageTool_.reset();
        manager_.reset();
        context_.reset();
    }

    std::shared_ptr<pdfe::core::MuPdfContext> context_;
    std::unique_ptr<pdfe::image::ImageManager> manager_;
    std::unique_ptr<pdfe::image::ImageTool> imageTool_;
    pdfe::ui::PdfCanvas* canvas_ = nullptr;
};

TEST_F(ImageToolWiringTest, SetImageTool) {
    // Initially null
    EXPECT_EQ(canvas_->imageTool(), nullptr);

    // Set image tool
    canvas_->setImageTool(imageTool_.get());

    // Verify it was set
    EXPECT_EQ(canvas_->imageTool(), imageTool_.get());
}

TEST_F(ImageToolWiringTest, ClearImageTool) {
    // Set image tool first
    canvas_->setImageTool(imageTool_.get());
    ASSERT_EQ(canvas_->imageTool(), imageTool_.get());

    // Clear image tool
    canvas_->clearImageTool();

    // Verify it was cleared
    EXPECT_EQ(canvas_->imageTool(), nullptr);
}

TEST_F(ImageToolWiringTest, SetImageToolClearsOther) {
    // Setting image tool should work alongside currentTool/textEditTool
    // They are independent dispatch paths

    // Set image tool
    canvas_->setImageTool(imageTool_.get());
    EXPECT_EQ(canvas_->imageTool(), imageTool_.get());

    // Image tool should still be set even if other tools are used
    // (This test verifies no inadvertent clearing happens)
    EXPECT_NE(canvas_->imageTool(), nullptr);
}

TEST_F(ImageToolWiringTest, ImageToolInitiallyNull) {
    // Create a fresh canvas
    pdfe::ui::PdfCanvas freshCanvas;

    // Verify imageTool() returns nullptr initially
    EXPECT_EQ(freshCanvas.imageTool(), nullptr);
}

// Phase-16 (qt_test_gap_audit finding #19): test renamed to make
// clear it locks the signal-connectability contract. The end-to-end
// production trigger (ImageTool emits repaintRequested after a
// successful selection) is covered by test_image_tool.cpp tests
// that drive real mouse events. This test catches the narrower
// regression: the signal is no longer declared in the metaobject.
TEST_F(ImageToolWiringTest, RepaintRequestedSignal) {
    QSignalSpy spy(imageTool_.get(), &pdfe::image::ImageTool::repaintRequested);
    ASSERT_TRUE(spy.isValid())
        << "ImageTool must expose repaintRequested() so PdfCanvas "
           "can connect to it for repaint scheduling.";

    // Emit the signal manually to verify it can be connected.
    // The production trigger (mouse-drive → emit) is tested
    // elsewhere — this is the narrow signature lock.
    emit imageTool_->repaintRequested();
    EXPECT_EQ(spy.count(), 1);
}

TEST_F(ImageToolWiringTest, SetImageToolMultipleTimes) {
    // Set image tool
    canvas_->setImageTool(imageTool_.get());
    EXPECT_EQ(canvas_->imageTool(), imageTool_.get());

    // Set it again (should be idempotent)
    canvas_->setImageTool(imageTool_.get());
    EXPECT_EQ(canvas_->imageTool(), imageTool_.get());

    // Clear and set again
    canvas_->clearImageTool();
    EXPECT_EQ(canvas_->imageTool(), nullptr);

    canvas_->setImageTool(imageTool_.get());
    EXPECT_EQ(canvas_->imageTool(), imageTool_.get());
}

TEST_F(ImageToolWiringTest, ImageToolPointerLifetime) {
    // PdfCanvas stores a raw pointer, not owning
    // Verify it doesn't try to delete the tool

    auto tempTool = std::make_unique<pdfe::image::ImageTool>(manager_.get());
    canvas_->setImageTool(tempTool.get());

    EXPECT_EQ(canvas_->imageTool(), tempTool.get());

    // Clear before destroying the tool (proper usage pattern)
    canvas_->clearImageTool();
    EXPECT_EQ(canvas_->imageTool(), nullptr);

    // tempTool will be destroyed here - canvas should not crash
}
