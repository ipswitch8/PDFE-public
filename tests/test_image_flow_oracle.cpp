// -*- coding: utf-8 -*-
// Phase-7 C2: image subsystem flow tests with PdfCanvas oracle.
//
// Each test performs an image operation (insert/rotate/crop), then
// verifies the page can still be rendered by creating a display list
// via Document::createDisplayList — the PdfCanvas rendering oracle.

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "image/ImageManager.h"
#include "image/ImageContentEditor.h"
#include "image/ImageCommands.h"
#include "image/ImageData.h"
#include "ui/ImagePropertiesPanel.h"

#include <QApplication>
#include <QRectF>
#include <filesystem>
#include <cmath>
#include <memory>

#ifndef TEST_FIXTURES_DIR
#define TEST_FIXTURES_DIR "."
#endif

class ImageFlowOracleTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<pdfe::core::MuPdfContext>();
        manager_ = std::make_unique<pdfe::image::ImageManager>(context_);
        editor_ = std::make_unique<pdfe::image::ImageContentEditor>(context_);
        pdfFixture_ = std::string(TEST_FIXTURES_DIR) + "/test_pdf_with_images.pdf";
        pngFixture_ = std::string(TEST_FIXTURES_DIR) + "/test_image_rgb.png";
    }

    struct DocPage {
        std::unique_ptr<pdfe::core::Document> doc;
        std::unique_ptr<pdfe::core::Page> page;
        std::filesystem::path tempPath;
    };

    DocPage openTempCopy(const std::string& suffix) {
        DocPage dp;
        dp.tempPath = std::filesystem::temp_directory_path() /
                      ("pdfe_imgflow_" + suffix + ".pdf");
        {
            auto doc = pdfe::core::Document::open(context_, pdfFixture_);
            EXPECT_NE(doc, nullptr);
            doc->saveAs(dp.tempPath);
        }
        dp.doc = pdfe::core::Document::open(context_, dp.tempPath);
        EXPECT_NE(dp.doc, nullptr);
        dp.page = dp.doc->page(0);
        EXPECT_NE(dp.page, nullptr);
        return dp;
    }

    void cleanup(DocPage& dp) {
        dp.page.reset();
        dp.doc.reset();
        std::error_code ec;
        std::filesystem::remove(dp.tempPath, ec);
    }

    void verifyDisplayListOracle(pdfe::core::Document* doc, int pageIndex) {
        void* dl = doc->createDisplayList(pageIndex);
        ASSERT_NE(dl, nullptr) << "PdfCanvas oracle: display list creation failed after operation";
        doc->releaseDisplayList(dl);
    }

    std::shared_ptr<pdfe::core::MuPdfContext> context_;
    std::unique_ptr<pdfe::image::ImageManager> manager_;
    std::unique_ptr<pdfe::image::ImageContentEditor> editor_;
    std::string pdfFixture_;
    std::string pngFixture_;
};

TEST_F(ImageFlowOracleTest, InsertImageAndVerifyViaDisplayList) {
    auto dp = openTempCopy("insert");
    int countBefore = manager_->imageCount(dp.page.get(), dp.doc.get());

    pdfe::image::InsertImageOptions opts;
    opts.filePath = pngFixture_;
    opts.targetRect = QRectF(72.0f, 72.0f, 200.0f, 150.0f);
    opts.jpegQuality = 85;
    opts.stripExif = true;

    pdfe::image::InsertImageCommand cmd(
        manager_.get(), dp.page.get(), dp.doc.get(), opts);
    cmd.execute();
    EXPECT_TRUE(cmd.result().success) << cmd.result().errorMessage;

    int countAfter = manager_->imageCount(dp.page.get(), dp.doc.get());
    EXPECT_GT(countAfter, countBefore) << "Image count should increase after insert";

    verifyDisplayListOracle(dp.doc.get(), 0);
    cleanup(dp);
}

TEST_F(ImageFlowOracleTest, RotateImageAndVerifyViaDisplayList) {
    auto dp = openTempCopy("rotate");
    auto imagesBefore = manager_->enumerateImages(dp.page.get(), dp.doc.get());
    ASSERT_FALSE(imagesBefore.empty()) << "Fixture must contain at least one image";

    auto& img = imagesBefore[0];
    auto ctmBefore = img.ctm;

    pdfe::image::RotateImageCommand cmd(
        manager_.get(), editor_.get(),
        dp.page.get(), dp.doc.get(),
        img.xobjectName,
        90.0f,
        img.boundingBox.center().x(), img.boundingBox.center().y(),
        img.displayWidth, img.displayHeight);
    cmd.execute();

    auto imagesAfter = manager_->enumerateImages(dp.page.get(), dp.doc.get());
    ASSERT_FALSE(imagesAfter.empty());
    bool ctmChanged = false;
    for (size_t i = 0; i < 6; ++i) {
        if (std::abs(imagesAfter[0].ctm[i] - ctmBefore[i]) > 0.01f) {
            ctmChanged = true;
            break;
        }
    }
    EXPECT_TRUE(ctmChanged) << "Image CTM should change after 90-degree rotation";

    verifyDisplayListOracle(dp.doc.get(), 0);
    cleanup(dp);
}

TEST_F(ImageFlowOracleTest, CropImageAndVerifyViaDisplayList) {
    auto dp = openTempCopy("crop");
    auto imagesBefore = manager_->enumerateImages(dp.page.get(), dp.doc.get());
    ASSERT_FALSE(imagesBefore.empty()) << "Fixture must contain at least one image";

    auto& img = imagesBefore[0];
    auto bboxBefore = img.boundingBox;
    float halfW = img.displayWidth * 0.5f;
    float halfH = img.displayHeight * 0.5f;
    QRectF cropRect(
        img.boundingBox.x() + img.displayWidth * 0.25f,
        img.boundingBox.y() + img.displayHeight * 0.25f,
        halfW, halfH);

    pdfe::image::CropImageCommand cmd(
        manager_.get(), editor_.get(),
        dp.page.get(), dp.doc.get(),
        img.xobjectName,
        cropRect, halfW, halfH,
        img.boundingBox.x(), img.boundingBox.y());
    cmd.execute();

    auto imagesAfter = manager_->enumerateImages(dp.page.get(), dp.doc.get());
    ASSERT_FALSE(imagesAfter.empty());
    auto bboxAfter = imagesAfter[0].boundingBox;
    bool bboxChanged = (std::abs(bboxAfter.width() - bboxBefore.width()) > 0.01f) ||
                       (std::abs(bboxAfter.height() - bboxBefore.height()) > 0.01f);
    EXPECT_TRUE(bboxChanged) << "Bounding box should change after crop";

    verifyDisplayListOracle(dp.doc.get(), 0);
    cleanup(dp);
}

TEST_F(ImageFlowOracleTest, InsertRotateCropSequenceWithOracle) {
    auto dp = openTempCopy("sequence");

    pdfe::image::InsertImageOptions opts;
    opts.filePath = pngFixture_;
    opts.targetRect = QRectF(100.0f, 400.0f, 150.0f, 100.0f);
    pdfe::image::InsertImageCommand insertCmd(
        manager_.get(), dp.page.get(), dp.doc.get(), opts);
    insertCmd.execute();
    ASSERT_TRUE(insertCmd.result().success);
    verifyDisplayListOracle(dp.doc.get(), 0);

    auto images = manager_->enumerateImages(dp.page.get(), dp.doc.get());
    ASSERT_FALSE(images.empty());
    auto& img = images.back();

    pdfe::image::RotateImageCommand rotateCmd(
        manager_.get(), editor_.get(),
        dp.page.get(), dp.doc.get(),
        img.xobjectName,
        180.0f,
        img.boundingBox.center().x(), img.boundingBox.center().y(),
        img.displayWidth, img.displayHeight);
    rotateCmd.execute();
    verifyDisplayListOracle(dp.doc.get(), 0);

    auto rotatedImages = manager_->enumerateImages(dp.page.get(), dp.doc.get());
    ASSERT_FALSE(rotatedImages.empty());
    auto& rotImg = rotatedImages.back();

    float halfW = rotImg.displayWidth * 0.5f;
    float halfH = rotImg.displayHeight * 0.5f;
    QRectF cropRect(
        rotImg.boundingBox.x(),
        rotImg.boundingBox.y(),
        halfW, halfH);
    pdfe::image::CropImageCommand cropCmd(
        manager_.get(), editor_.get(),
        dp.page.get(), dp.doc.get(),
        rotImg.xobjectName,
        cropRect, halfW, halfH,
        rotImg.boundingBox.x(), rotImg.boundingBox.y());
    cropCmd.execute();
    verifyDisplayListOracle(dp.doc.get(), 0);

    cleanup(dp);
}

class ImagePropertiesPanelAuditTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!qApp) {
            int argc = 0;
            app_ = new QApplication(argc, nullptr);
        }
    }

    void TearDown() override {
        delete panel_;
        panel_ = nullptr;
    }

    QApplication* app_ = nullptr;
    pdfe::image::ImagePropertiesPanel* panel_ = nullptr;
};

TEST_F(ImagePropertiesPanelAuditTest, PanelObjectNameIsSet) {
    panel_ = new pdfe::image::ImagePropertiesPanel();
    EXPECT_EQ(panel_->objectName(), "imagePropertiesPanel");
}

TEST_F(ImagePropertiesPanelAuditTest, KeyWidgetObjectNamesPresent) {
    panel_ = new pdfe::image::ImagePropertiesPanel();

    auto findChild = [&](const char* name) -> QObject* {
        return panel_->findChild<QObject*>(name);
    };

    EXPECT_NE(findChild("imageInfoGroup"), nullptr) << "imageInfoGroup missing";
    EXPECT_NE(findChild("imageTransformGroup"), nullptr) << "imageTransformGroup missing";
    EXPECT_NE(findChild("imageWidthSpin"), nullptr) << "imageWidthSpin missing";
    EXPECT_NE(findChild("imageHeightSpin"), nullptr) << "imageHeightSpin missing";
    EXPECT_NE(findChild("imageRotationCombo"), nullptr) << "imageRotationCombo missing";
    EXPECT_NE(findChild("imageCropButton"), nullptr) << "imageCropButton missing";
    EXPECT_NE(findChild("imageExtractButton"), nullptr) << "imageExtractButton missing";
    EXPECT_NE(findChild("imageRemoveButton"), nullptr) << "imageRemoveButton missing";
}
