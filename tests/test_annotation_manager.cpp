// PHASE1_HARDENING: skip=14 expect_ge=3 no_throw=0  (2026-05-08)
#include <gtest/gtest.h>
#include "annotations/AnnotationManager.h"
#include "annotations/AnnotationType.h"
#include "annotations/AnnotationData.h"
#include "core/Document.h"
#include "core/Page.h"
#include "core/MuPdfContext.h"
#include "core/Error.h"
#include <filesystem>
#include <memory>

using namespace pdfe::annotations;
using namespace pdfe::core;

class AnnotationManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        context = std::make_shared<MuPdfContext>();
        std::filesystem::path fixturesDir = TEST_FIXTURES_DIR;
        testPdf = fixturesDir / "test_3page.pdf";
        if (std::filesystem::exists(testPdf)) {
            document = Document::open(context, testPdf);
            manager = std::make_unique<AnnotationManager>(context);
        }
    }

    std::shared_ptr<MuPdfContext> context;
    std::filesystem::path testPdf;
    std::unique_ptr<Document> document;
    std::unique_ptr<AnnotationManager> manager;
};

TEST_F(AnnotationManagerTest, CreateTextAnnotation) {
    ASSERT_NE(document, nullptr) << "Test fixture not loaded: " << testPdf;

    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    QRectF rect(100, 100, 50, 50);
    AnnotationData data = manager->createAnnotation(
        page.get(),
        AnnotationType::Text,
        rect
    );

    EXPECT_EQ(data.type, AnnotationType::Text);
    EXPECT_EQ(data.pageIndex, 0);
    EXPECT_NE(data.annotIndex, -1) << "createAnnotation must return a valid index, not the sentinel";

    // Verify it exists in the annotations list
    auto annotations = manager->getAnnotations(page.get());
    EXPECT_GT(annotations.size(), 0u);
    EXPECT_LT(data.annotIndex, static_cast<int>(annotations.size()))
        << "annotIndex must be addressable in the page's annotations list";

    bool found = false;
    for (const auto& annot : annotations) {
        if (annot.type == AnnotationType::Text && annot.annotIndex == data.annotIndex) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(AnnotationManagerTest, CreateHighlightAnnotation) {
    ASSERT_NE(document, nullptr) << "Test fixture not loaded: " << testPdf;

    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    QRectF rect(100, 100, 200, 50);
    AnnotationData data = manager->createAnnotation(
        page.get(),
        AnnotationType::Highlight,
        rect
    );

    EXPECT_EQ(data.type, AnnotationType::Highlight);

    // Set quad points
    std::vector<QRectF> quads;
    quads.push_back(QRectF(100, 100, 200, 20));
    quads.push_back(QRectF(100, 120, 200, 20));

    manager->setQuadPoints(page.get(), data.annotIndex, quads);

    // Verify quad points were set
    AnnotationData updated = manager->getAnnotation(page.get(), data.annotIndex);
    EXPECT_EQ(updated.quadPoints.size(), 2);
}

TEST_F(AnnotationManagerTest, DeleteAnnotation) {
    ASSERT_NE(document, nullptr) << "Test fixture not loaded: " << testPdf;

    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    QRectF rect(100, 100, 50, 50);
    AnnotationData data = manager->createAnnotation(
        page.get(),
        AnnotationType::Text,
        rect
    );

    int annotIndex = data.annotIndex;
    size_t countBefore = manager->getAnnotations(page.get()).size();

    // Delete the annotation
    AnnotationData deleted = manager->deleteAnnotation(page.get(), annotIndex);

    EXPECT_EQ(deleted.type, AnnotationType::Text);

    // Verify it's gone
    size_t countAfter = manager->getAnnotations(page.get()).size();
    EXPECT_EQ(countAfter, countBefore - 1);
}

TEST_F(AnnotationManagerTest, ModifyContents) {
    ASSERT_NE(document, nullptr) << "Test fixture not loaded: " << testPdf;

    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    QRectF rect(100, 100, 50, 50);
    AnnotationData data = manager->createAnnotation(
        page.get(),
        AnnotationType::Text,
        rect
    );

    std::string testContents = "This is a test annotation";
    manager->setContents(page.get(), data.annotIndex, testContents);

    // Verify contents were set
    AnnotationData updated = manager->getAnnotation(page.get(), data.annotIndex);
    EXPECT_EQ(updated.contents, testContents);
}

TEST_F(AnnotationManagerTest, ModifyColor) {
    ASSERT_NE(document, nullptr) << "Test fixture not loaded: " << testPdf;

    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    QRectF rect(100, 100, 50, 50);
    AnnotationData data = manager->createAnnotation(
        page.get(),
        AnnotationType::Square,
        rect
    );

    QColor testColor(255, 0, 0); // Red
    manager->setColor(page.get(), data.annotIndex, testColor);

    // Verify color was set
    AnnotationData updated = manager->getAnnotation(page.get(), data.annotIndex);
    EXPECT_EQ(updated.color.red(), 255);
    EXPECT_EQ(updated.color.green(), 0);
    EXPECT_EQ(updated.color.blue(), 0);
}

TEST_F(AnnotationManagerTest, ModifyOpacity) {
    ASSERT_NE(document, nullptr) << "Test fixture not loaded: " << testPdf;

    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    QRectF rect(100, 100, 50, 50);
    AnnotationData data = manager->createAnnotation(
        page.get(),
        AnnotationType::Square,
        rect
    );

    float testOpacity = 0.5f;
    manager->setOpacity(page.get(), data.annotIndex, testOpacity);

    // Verify opacity was set
    AnnotationData updated = manager->getAnnotation(page.get(), data.annotIndex);
    EXPECT_NEAR(updated.opacity, testOpacity, 0.01f);
}

TEST_F(AnnotationManagerTest, GetAnnotations) {
    ASSERT_NE(document, nullptr) << "Test fixture not loaded: " << testPdf;

    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    size_t countBefore = manager->getAnnotations(page.get()).size();

    // Create multiple annotations
    manager->createAnnotation(page.get(), AnnotationType::Text, QRectF(100, 100, 50, 50));
    manager->createAnnotation(page.get(), AnnotationType::Highlight, QRectF(200, 200, 100, 20));
    manager->createAnnotation(page.get(), AnnotationType::Square, QRectF(300, 300, 80, 80));

    auto annotations = manager->getAnnotations(page.get());
    EXPECT_EQ(annotations.size(), countBefore + 3);

    // Verify each annotation has valid data
    for (const auto& annot : annotations) {
        EXPECT_NE(annot.type, AnnotationType::Unknown);
        EXPECT_EQ(annot.pageIndex, 0);
        EXPECT_NE(annot.annotIndex, -1) << "Returned annotation must have a real index, not -1 sentinel";
    }
}

TEST_F(AnnotationManagerTest, HitTestAnnotation) {
    ASSERT_NE(document, nullptr) << "Test fixture not loaded: " << testPdf;

    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    QRectF rect(100, 100, 50, 50);
    AnnotationData data = manager->createAnnotation(
        page.get(),
        AnnotationType::Square,
        rect
    );

    // Hit test at the center of the annotation
    QPointF center = rect.center();
    auto result = manager->getAnnotationAt(page.get(), center);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), data.annotIndex);

    // Hit test outside the annotation
    QPointF outside(500, 500);
    auto missResult = manager->getAnnotationAt(page.get(), outside);

    // Should either be nullopt or a different annotation
    if (missResult.has_value()) {
        EXPECT_NE(missResult.value(), data.annotIndex);
    }
}

TEST_F(AnnotationManagerTest, RestoreDeletedAnnotation) {
    ASSERT_NE(document, nullptr) << "Test fixture not loaded: " << testPdf;

    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    QRectF rect(100, 100, 50, 50);
    AnnotationData data = manager->createAnnotation(
        page.get(),
        AnnotationType::Text,
        rect
    );

    manager->setContents(page.get(), data.annotIndex, "Test content");
    manager->setColor(page.get(), data.annotIndex, QColor(255, 0, 0));

    // Get the complete data before deletion
    AnnotationData fullData = manager->getAnnotation(page.get(), data.annotIndex);

    // Delete the annotation
    manager->deleteAnnotation(page.get(), data.annotIndex);

    // Restore it
    int newIndex = manager->restoreAnnotation(page.get(), fullData);
    EXPECT_NE(newIndex, -1) << "restoreAnnotation must return a valid index, not -1";
    auto annotations = manager->getAnnotations(page.get());
    EXPECT_LT(newIndex, static_cast<int>(annotations.size()))
        << "restored newIndex must address into the annotations list";

    // Verify restoration
    AnnotationData restored = manager->getAnnotation(page.get(), newIndex);
    EXPECT_EQ(restored.type, AnnotationType::Text);
    EXPECT_EQ(restored.contents, "Test content");
    EXPECT_EQ(restored.color.red(), 255);
    EXPECT_EQ(restored.color.green(), 0);
    EXPECT_EQ(restored.color.blue(), 0);
}

TEST_F(AnnotationManagerTest, AnnotationDataSnapshot) {
    ASSERT_NE(document, nullptr) << "Test fixture not loaded: " << testPdf;

    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    QRectF rect(100, 100, 50, 50);
    AnnotationData data = manager->createAnnotation(
        page.get(),
        AnnotationType::Text,
        rect
    );

    // Set various properties
    manager->setContents(page.get(), data.annotIndex, "Test annotation");
    manager->setColor(page.get(), data.annotIndex, QColor(0, 255, 0));
    manager->setOpacity(page.get(), data.annotIndex, 0.75f);

    // Get snapshot
    AnnotationData snapshot = manager->getAnnotation(page.get(), data.annotIndex);

    // Verify all fields are populated
    EXPECT_EQ(snapshot.type, AnnotationType::Text);
    EXPECT_EQ(snapshot.pageIndex, 0);
    EXPECT_EQ(snapshot.annotIndex, data.annotIndex);
    EXPECT_EQ(snapshot.contents, "Test annotation");
    EXPECT_GT(snapshot.rect.width(), 0);
    EXPECT_GT(snapshot.rect.height(), 0);
    EXPECT_EQ(snapshot.color.green(), 255);
    EXPECT_NEAR(snapshot.opacity, 0.75f, 0.01f);
}

TEST_F(AnnotationManagerTest, SetRectModifiesAnnotationBounds) {
    ASSERT_NE(document, nullptr) << "Test fixture not loaded: " << testPdf;

    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    QRectF originalRect(100, 100, 50, 50);
    AnnotationData data = manager->createAnnotation(
        page.get(),
        AnnotationType::Square,
        originalRect
    );

    // Modify the rectangle
    QRectF newRect(200, 200, 100, 100);
    manager->setRect(page.get(), data.annotIndex, newRect);

    // Verify the new bounds (MuPDF may expand rect by border width, so use wider tolerance)
    AnnotationData updated = manager->getAnnotation(page.get(), data.annotIndex);
    EXPECT_NEAR(updated.rect.x(), newRect.x(), 5.0);
    EXPECT_NEAR(updated.rect.y(), newRect.y(), 5.0);
    EXPECT_NEAR(updated.rect.width(), newRect.width(), 5.0);
    EXPECT_NEAR(updated.rect.height(), newRect.height(), 5.0);
}

TEST_F(AnnotationManagerTest, InkAnnotationWithStrokes) {
    ASSERT_NE(document, nullptr) << "Test fixture not loaded: " << testPdf;

    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    QRectF rect(100, 100, 200, 200);
    AnnotationData data = manager->createAnnotation(
        page.get(),
        AnnotationType::Ink,
        rect
    );

    // Create some ink strokes
    std::vector<std::vector<QPointF>> strokes;
    std::vector<QPointF> stroke1;
    stroke1.push_back(QPointF(110, 110));
    stroke1.push_back(QPointF(150, 150));
    stroke1.push_back(QPointF(190, 110));
    strokes.push_back(stroke1);

    std::vector<QPointF> stroke2;
    stroke2.push_back(QPointF(120, 200));
    stroke2.push_back(QPointF(180, 250));
    strokes.push_back(stroke2);

    manager->setInkStrokes(page.get(), data.annotIndex, strokes);

    // Verify strokes were set
    AnnotationData updated = manager->getAnnotation(page.get(), data.annotIndex);
    EXPECT_EQ(updated.inkStrokes.size(), 2);
    EXPECT_EQ(updated.inkStrokes[0].size(), 3);
    EXPECT_EQ(updated.inkStrokes[1].size(), 2);
}

TEST_F(AnnotationManagerTest, InvalidAnnotationIndexThrows) {
    ASSERT_NE(document, nullptr) << "Test fixture not loaded: " << testPdf;

    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    // Try to get an annotation with an invalid index
    EXPECT_THROW(
        manager->getAnnotation(page.get(), 9999),
        std::exception
    );

    // Try to delete an annotation with an invalid index
    EXPECT_THROW(
        manager->deleteAnnotation(page.get(), 9999),
        std::exception
    );
}

TEST_F(AnnotationManagerTest, NullPageThrows) {
    ASSERT_NE(document, nullptr) << "Test fixture not loaded: " << testPdf;

    EXPECT_THROW(
        manager->createAnnotation(nullptr, AnnotationType::Text, QRectF(0, 0, 50, 50)),
        std::invalid_argument
    );

    EXPECT_THROW(
        manager->getAnnotations(nullptr),
        std::invalid_argument
    );
}
