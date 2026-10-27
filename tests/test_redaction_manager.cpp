// PHASE1_HARDENING: skip=1 expect_ge=2 no_throw=0  (2026-05-08)
#include <gtest/gtest.h>
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/Page.h"
#include "core/Error.h"
#include "redaction/RedactionManager.h"
#include "redaction/RedactionData.h"
#include <filesystem>
#include <memory>

class RedactionManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<pdfe::core::MuPdfContext>();

        auto pdfPath = std::filesystem::path(TEST_FIXTURES_DIR) / "test_redaction.pdf";
        ASSERT_TRUE(std::filesystem::exists(pdfPath))
            << "test_redaction.pdf fixture missing at " << pdfPath;
        document_ = pdfe::core::Document::open(context_, pdfPath);
        ASSERT_NE(document_, nullptr);

        manager_ = std::make_unique<pdfe::redaction::RedactionManager>(context_);
    }

    std::shared_ptr<pdfe::core::MuPdfContext> context_;
    std::unique_ptr<pdfe::core::Document> document_;
    std::unique_ptr<pdfe::redaction::RedactionManager> manager_;
    std::vector<std::unique_ptr<pdfe::core::Page>> pages_;

    pdfe::core::Page* getPage(int index) {
        pages_.push_back(document_->page(index));
        return pages_.back().get();
    }
};

TEST_F(RedactionManagerTest, ConstructDestroy) {
    auto mgr = std::make_unique<pdfe::redaction::RedactionManager>(context_);
    EXPECT_NE(mgr, nullptr);
}

TEST_F(RedactionManagerTest, CreateRedactionMark) {
    auto* page = getPage(0);
    QRectF rect(100.0, 100.0, 100.0, 50.0); // x, y, w, h

    auto data = manager_->createRedactionMark(page, rect);

    EXPECT_EQ(data.pageIndex, 0);
    EXPECT_EQ(data.annotIndex, 0)
        << "first redaction on a fresh page should be index 0";
    // MuPDF may expand the rect slightly, so use tolerance
    EXPECT_NEAR(data.rect.left(), 100.0, 5.0);
    EXPECT_NEAR(data.rect.top(), 100.0, 5.0);
}

TEST_F(RedactionManagerTest, CreateMultipleMarks) {
    auto* page = getPage(0);

    auto d1 = manager_->createRedactionMark(page, QRectF(100, 100, 100, 50));
    auto d2 = manager_->createRedactionMark(page, QRectF(250, 100, 100, 50));
    auto d3 = manager_->createRedactionMark(page, QRectF(100, 200, 100, 50));

    EXPECT_NE(d1.annotIndex, d2.annotIndex);
    EXPECT_NE(d2.annotIndex, d3.annotIndex);

    auto marks = manager_->getRedactionMarks(page);
    EXPECT_EQ(marks.size(), 3u);
}

TEST_F(RedactionManagerTest, CreateMarksOnDifferentPages) {
    auto* page0 = getPage(0);
    auto* page1 = getPage(1);

    auto d0 = manager_->createRedactionMark(page0, QRectF(100, 100, 100, 50));
    auto d1 = manager_->createRedactionMark(page1, QRectF(100, 100, 100, 50));

    EXPECT_EQ(d0.pageIndex, 0);
    EXPECT_EQ(d1.pageIndex, 1);

    EXPECT_EQ(manager_->getRedactionMarks(page0).size(), 1u);
    EXPECT_EQ(manager_->getRedactionMarks(page1).size(), 1u);
}

TEST_F(RedactionManagerTest, DeleteRedactionMark) {
    auto* page = getPage(0);
    auto created = manager_->createRedactionMark(page, QRectF(100, 100, 100, 50));

    auto deleted = manager_->deleteRedactionMark(page, created.annotIndex);

    EXPECT_EQ(deleted.pageIndex, created.pageIndex);
    EXPECT_EQ(deleted.annotIndex, created.annotIndex);
    EXPECT_EQ(manager_->getRedactionMarks(page).size(), 0u);
}

TEST_F(RedactionManagerTest, DeleteNonExistentMark) {
    auto* page = getPage(0);
    EXPECT_THROW(
        manager_->deleteRedactionMark(page, 999),
        pdfe::core::MuPdfException
    );
}

TEST_F(RedactionManagerTest, RestoreRedactionMark) {
    auto* page = getPage(0);
    auto original = manager_->createRedactionMark(page, QRectF(100, 100, 100, 50));
    auto deleted = manager_->deleteRedactionMark(page, original.annotIndex);

    int restoredIdx = manager_->restoreRedactionMark(page, deleted);

    EXPECT_NE(restoredIdx, -1) << "restoreRedactionMark must return a valid index";
    EXPECT_EQ(manager_->getRedactionMarks(page).size(), 1u);
    EXPECT_LT(restoredIdx, static_cast<int>(manager_->getRedactionMarks(page).size()))
        << "restored index must be addressable in the marks list";
}

TEST_F(RedactionManagerTest, SetFillColor) {
    auto* page = getPage(0);
    auto data = manager_->createRedactionMark(page, QRectF(100, 100, 100, 50));

    manager_->setFillColor(page, data.annotIndex, QColor(Qt::red));

    auto retrieved = manager_->getRedactionMark(page, data.annotIndex);
    EXPECT_NEAR(retrieved.fillColor.redF(), 1.0, 0.01);
    EXPECT_NEAR(retrieved.fillColor.greenF(), 0.0, 0.01);
    EXPECT_NEAR(retrieved.fillColor.blueF(), 0.0, 0.01);
}

TEST_F(RedactionManagerTest, SetOverlayText) {
    auto* page = getPage(0);
    auto data = manager_->createRedactionMark(page, QRectF(100, 100, 100, 50));

    manager_->setOverlayText(page, data.annotIndex, "REDACTED");

    auto retrieved = manager_->getRedactionMark(page, data.annotIndex);
    EXPECT_EQ(retrieved.overlayText, "REDACTED");
}

TEST_F(RedactionManagerTest, SetRect) {
    auto* page = getPage(0);
    auto data = manager_->createRedactionMark(page, QRectF(100, 100, 100, 50));

    QRectF newRect(150, 150, 200, 100);
    manager_->setRect(page, data.annotIndex, newRect);

    auto retrieved = manager_->getRedactionMark(page, data.annotIndex);
    EXPECT_NEAR(retrieved.rect.left(), 150.0, 5.0);
    EXPECT_NEAR(retrieved.rect.top(), 150.0, 5.0);
}

TEST_F(RedactionManagerTest, GetRedactionMarks) {
    auto* page = getPage(0);
    auto d1 = manager_->createRedactionMark(page, QRectF(100, 100, 100, 50));
    auto d2 = manager_->createRedactionMark(page, QRectF(250, 100, 100, 50));

    auto marks = manager_->getRedactionMarks(page);

    EXPECT_EQ(marks.size(), 2u);
}

TEST_F(RedactionManagerTest, GetRedactionMarksSinglePage) {
    auto* page0 = getPage(0);
    auto* page1 = getPage(1);

    manager_->createRedactionMark(page0, QRectF(100, 100, 100, 50));
    manager_->createRedactionMark(page0, QRectF(200, 100, 100, 50));
    manager_->createRedactionMark(page1, QRectF(100, 100, 100, 50));

    auto marks0 = manager_->getRedactionMarks(page0);
    auto marks1 = manager_->getRedactionMarks(page1);

    EXPECT_EQ(marks0.size(), 2u);
    EXPECT_EQ(marks1.size(), 1u);

    for (const auto& m : marks0) EXPECT_EQ(m.pageIndex, 0);
    for (const auto& m : marks1) EXPECT_EQ(m.pageIndex, 1);
}

TEST_F(RedactionManagerTest, GetRedactionMark_ByIndex) {
    auto* page = getPage(0);
    auto created = manager_->createRedactionMark(page, QRectF(100, 100, 100, 50));

    auto retrieved = manager_->getRedactionMark(page, created.annotIndex);

    EXPECT_EQ(retrieved.pageIndex, created.pageIndex);
    EXPECT_EQ(retrieved.annotIndex, created.annotIndex);
}

TEST_F(RedactionManagerTest, GetAllRedactionMarks) {
    auto* page0 = getPage(0);
    auto* page1 = getPage(1);
    auto* page2 = getPage(2);

    manager_->createRedactionMark(page0, QRectF(100, 100, 100, 50));
    manager_->createRedactionMark(page0, QRectF(200, 100, 100, 50));
    manager_->createRedactionMark(page1, QRectF(100, 100, 100, 50));
    manager_->createRedactionMark(page2, QRectF(100, 100, 100, 50));
    manager_->createRedactionMark(page2, QRectF(200, 100, 100, 50));

    auto all = manager_->getAllRedactionMarks(document_.get());
    EXPECT_EQ(all.size(), 5u);
}

TEST_F(RedactionManagerTest, PendingMarkCount) {
    auto* page = getPage(0);

    EXPECT_EQ(manager_->pendingMarkCount(document_.get()), 0);

    manager_->createRedactionMark(page, QRectF(100, 100, 100, 50));
    EXPECT_EQ(manager_->pendingMarkCount(document_.get()), 1);

    manager_->createRedactionMark(page, QRectF(200, 100, 100, 50));
    EXPECT_EQ(manager_->pendingMarkCount(document_.get()), 2);
}

TEST_F(RedactionManagerTest, GetRedactionMarkAt_HitTest) {
    auto* page = getPage(0);
    auto created = manager_->createRedactionMark(page, QRectF(100, 100, 100, 50));

    auto hit = manager_->getRedactionMarkAt(page, QPointF(150, 125));

    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(hit.value(), created.annotIndex);
}

TEST_F(RedactionManagerTest, GetRedactionMarkAt_Miss) {
    auto* page = getPage(0);
    manager_->createRedactionMark(page, QRectF(100, 100, 100, 50));

    auto hit = manager_->getRedactionMarkAt(page, QPointF(500, 500));

    EXPECT_FALSE(hit.has_value());
}

TEST_F(RedactionManagerTest, ApplyPageRedactions) {
    auto* page = getPage(0);
    manager_->createRedactionMark(page, QRectF(100, 660, 400, 30));

    EXPECT_NO_THROW(manager_->applyPageRedactions(page));

    auto marks = manager_->getRedactionMarks(page);
    EXPECT_EQ(marks.size(), 0u);
}

TEST_F(RedactionManagerTest, CreateRedactionMark_NullPage) {
    EXPECT_THROW(
        manager_->createRedactionMark(nullptr, QRectF(100, 100, 100, 50)),
        std::invalid_argument
    );
}
