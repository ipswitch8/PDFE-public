// PHASE1_HARDENING: skip=0 expect_ge=2 no_throw=0  (2026-05-08)
#include <gtest/gtest.h>
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/Page.h"
#include "core/Error.h"
#include "redaction/RedactionManager.h"
#include "redaction/RedactionCommands.h"
#include "redaction/RedactionVerifier.h"
#include "redaction/RedactionData.h"
#include "ui/RedactionPanel.h"
#include <QApplication>
#include <QTreeWidget>
#include <QPushButton>
#include <QLabel>
#include <QTest>
#include <QSignalSpy>
#include <filesystem>
#include <memory>

// ============================================================================
// Redaction Manager Edge Cases Fixture
// ============================================================================

class RedactionManagerEdgeCaseTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<pdfe::core::MuPdfContext>();
        auto pdfPath = std::filesystem::path(TEST_FIXTURES_DIR) / "test_redaction.pdf";
        if (!std::filesystem::exists(pdfPath)) {
            GTEST_SKIP() << "test_redaction.pdf not found";
        }
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

// ============================================================================
// CRITICAL: Null Page Guards (all manager methods)
// ============================================================================

TEST_F(RedactionManagerEdgeCaseTest, DeleteRedactionMark_NullPage) {
    EXPECT_THROW(
        manager_->deleteRedactionMark(nullptr, 0),
        std::invalid_argument
    );
}

TEST_F(RedactionManagerEdgeCaseTest, RestoreRedactionMark_NullPage) {
    pdfe::redaction::RedactionData data;
    data.rect = QRectF(100, 100, 100, 50);
    EXPECT_THROW(
        manager_->restoreRedactionMark(nullptr, data),
        std::invalid_argument
    );
}

TEST_F(RedactionManagerEdgeCaseTest, SetFillColor_NullPage) {
    EXPECT_THROW(
        manager_->setFillColor(nullptr, 0, QColor(Qt::red)),
        std::invalid_argument
    );
}

TEST_F(RedactionManagerEdgeCaseTest, SetOverlayText_NullPage) {
    EXPECT_THROW(
        manager_->setOverlayText(nullptr, 0, "test"),
        std::invalid_argument
    );
}

TEST_F(RedactionManagerEdgeCaseTest, SetRect_NullPage) {
    EXPECT_THROW(
        manager_->setRect(nullptr, 0, QRectF(0, 0, 100, 50)),
        std::invalid_argument
    );
}

TEST_F(RedactionManagerEdgeCaseTest, GetRedactionMarks_NullPage) {
    EXPECT_THROW(
        manager_->getRedactionMarks(nullptr),
        std::invalid_argument
    );
}

TEST_F(RedactionManagerEdgeCaseTest, GetRedactionMark_NullPage) {
    EXPECT_THROW(
        manager_->getRedactionMark(nullptr, 0),
        std::invalid_argument
    );
}

TEST_F(RedactionManagerEdgeCaseTest, GetRedactionMarkAt_NullPage) {
    EXPECT_THROW(
        manager_->getRedactionMarkAt(nullptr, QPointF(100, 100)),
        std::invalid_argument
    );
}

TEST_F(RedactionManagerEdgeCaseTest, ApplyPageRedactions_NullPage) {
    EXPECT_THROW(
        manager_->applyPageRedactions(nullptr),
        std::invalid_argument
    );
}

TEST_F(RedactionManagerEdgeCaseTest, ApplyAllRedactions_NullDocument) {
    EXPECT_THROW(
        manager_->applyAllRedactions(nullptr),
        std::invalid_argument
    );
}

TEST_F(RedactionManagerEdgeCaseTest, GetAllRedactionMarks_NullDocument) {
    EXPECT_THROW(
        manager_->getAllRedactionMarks(nullptr),
        std::invalid_argument
    );
}

TEST_F(RedactionManagerEdgeCaseTest, PendingMarkCount_NullDocument) {
    EXPECT_THROW(
        manager_->pendingMarkCount(nullptr),
        std::invalid_argument
    );
}

// ============================================================================
// HIGH: Out-of-range annotation index
// ============================================================================

TEST_F(RedactionManagerEdgeCaseTest, GetRedactionMark_OutOfRange) {
    auto* page = getPage(0);
    EXPECT_THROW(
        manager_->getRedactionMark(page, 999),
        pdfe::core::MuPdfException
    );
}

TEST_F(RedactionManagerEdgeCaseTest, SetFillColor_OutOfRange) {
    auto* page = getPage(0);
    EXPECT_THROW(
        manager_->setFillColor(page, 999, QColor(Qt::red)),
        pdfe::core::MuPdfException
    );
}

TEST_F(RedactionManagerEdgeCaseTest, SetOverlayText_OutOfRange) {
    auto* page = getPage(0);
    EXPECT_THROW(
        manager_->setOverlayText(page, 999, "test"),
        pdfe::core::MuPdfException
    );
}

TEST_F(RedactionManagerEdgeCaseTest, SetRect_OutOfRange) {
    auto* page = getPage(0);
    EXPECT_THROW(
        manager_->setRect(page, 999, QRectF(0, 0, 100, 50)),
        pdfe::core::MuPdfException
    );
}

TEST_F(RedactionManagerEdgeCaseTest, DeleteRedactionMark_OutOfRange) {
    auto* page = getPage(0);
    EXPECT_THROW(
        manager_->deleteRedactionMark(page, 999),
        pdfe::core::MuPdfException
    );
}

// ============================================================================
// HIGH: Empty overlay text handling
// ============================================================================

TEST_F(RedactionManagerEdgeCaseTest, SetOverlayText_Empty) {
    auto* page = getPage(0);
    auto created = manager_->createRedactionMark(page, QRectF(100, 100, 100, 50));

    // Set non-empty, then set back to empty
    manager_->setOverlayText(page, created.annotIndex, "REDACTED");
    auto withText = manager_->getRedactionMark(page, created.annotIndex);
    EXPECT_EQ(withText.overlayText, "REDACTED");

    manager_->setOverlayText(page, created.annotIndex, "");
    auto withoutText = manager_->getRedactionMark(page, created.annotIndex);
    EXPECT_TRUE(withoutText.overlayText.empty());
}

TEST_F(RedactionManagerEdgeCaseTest, RestoreRedactionMark_WithOverlayText) {
    auto* page = getPage(0);
    auto created = manager_->createRedactionMark(page, QRectF(100, 100, 100, 50));
    manager_->setOverlayText(page, created.annotIndex, "CONFIDENTIAL");
    manager_->setFillColor(page, created.annotIndex, QColor(Qt::red));

    auto data = manager_->deleteRedactionMark(page, created.annotIndex);
    EXPECT_EQ(data.overlayText, "CONFIDENTIAL");

    int restoredIdx = manager_->restoreRedactionMark(page, data);
    auto restored = manager_->getRedactionMark(page, restoredIdx);

    EXPECT_EQ(restored.overlayText, "CONFIDENTIAL");
    EXPECT_NEAR(restored.fillColor.redF(), 1.0, 0.01);
    EXPECT_NEAR(restored.fillColor.greenF(), 0.0, 0.01);
    EXPECT_NEAR(restored.fillColor.blueF(), 0.0, 0.01);
}

// ============================================================================
// MEDIUM: Overlapping marks at same position
// ============================================================================

TEST_F(RedactionManagerEdgeCaseTest, OverlappingMarks) {
    auto* page = getPage(0);

    auto d1 = manager_->createRedactionMark(page, QRectF(100, 100, 200, 100));
    auto d2 = manager_->createRedactionMark(page, QRectF(150, 120, 200, 100));

    auto marks = manager_->getRedactionMarks(page);
    EXPECT_EQ(marks.size(), 2u);

    // Hit test in overlapping area should return one of them (first hit)
    auto hit = manager_->getRedactionMarkAt(page, QPointF(175, 150));
    ASSERT_TRUE(hit.has_value());
}

TEST_F(RedactionManagerEdgeCaseTest, HitTestExactBoundary) {
    auto* page = getPage(0);
    auto created = manager_->createRedactionMark(page, QRectF(100, 100, 100, 50));

    // Test exact corner point
    auto hitCorner = manager_->getRedactionMarkAt(page, QPointF(100, 100));
    ASSERT_TRUE(hitCorner.has_value());
}

TEST_F(RedactionManagerEdgeCaseTest, HitTestJustOutside) {
    auto* page = getPage(0);
    manager_->createRedactionMark(page, QRectF(100, 100, 100, 50));

    // Test just outside the mark
    auto miss = manager_->getRedactionMarkAt(page, QPointF(99, 99));
    EXPECT_FALSE(miss.has_value());
}

// ============================================================================
// MEDIUM: Page with no redaction marks - various operations
// ============================================================================

TEST_F(RedactionManagerEdgeCaseTest, GetMarksOnCleanPage) {
    auto* page = getPage(0);
    auto marks = manager_->getRedactionMarks(page);
    EXPECT_EQ(marks.size(), 0u);
}

TEST_F(RedactionManagerEdgeCaseTest, ApplyOnCleanPage) {
    auto* page = getPage(0);
    // Applying redactions on a page with no marks should succeed trivially
    EXPECT_NO_THROW(manager_->applyPageRedactions(page));
}

// ============================================================================
// MEDIUM: Mark with various color values
// ============================================================================

TEST_F(RedactionManagerEdgeCaseTest, SetFillColor_White) {
    auto* page = getPage(0);
    auto created = manager_->createRedactionMark(page, QRectF(100, 100, 100, 50));

    manager_->setFillColor(page, created.annotIndex, QColor(Qt::white));
    auto retrieved = manager_->getRedactionMark(page, created.annotIndex);

    EXPECT_NEAR(retrieved.fillColor.redF(), 1.0, 0.01);
    EXPECT_NEAR(retrieved.fillColor.greenF(), 1.0, 0.01);
    EXPECT_NEAR(retrieved.fillColor.blueF(), 1.0, 0.01);
}

TEST_F(RedactionManagerEdgeCaseTest, SetFillColor_CustomRGB) {
    auto* page = getPage(0);
    auto created = manager_->createRedactionMark(page, QRectF(100, 100, 100, 50));

    manager_->setFillColor(page, created.annotIndex, QColor(128, 64, 192));
    auto retrieved = manager_->getRedactionMark(page, created.annotIndex);

    EXPECT_NEAR(retrieved.fillColor.redF(), 128.0 / 255.0, 0.02);
    EXPECT_NEAR(retrieved.fillColor.greenF(), 64.0 / 255.0, 0.02);
    EXPECT_NEAR(retrieved.fillColor.blueF(), 192.0 / 255.0, 0.02);
}

// ============================================================================
// MEDIUM: Default data values
// ============================================================================

TEST_F(RedactionManagerEdgeCaseTest, DefaultRedactionDataValues) {
    pdfe::redaction::RedactionData data;
    EXPECT_EQ(data.pageIndex, -1);
    EXPECT_EQ(data.annotIndex, -1);
    EXPECT_EQ(data.fillColor, QColor(Qt::black));
    EXPECT_TRUE(data.overlayText.empty());
    EXPECT_TRUE(data.enabled);
    EXPECT_EQ(data.imageMethod, pdfe::redaction::RedactionImageMethod::Remove);
    EXPECT_EQ(data.textMethod, pdfe::redaction::RedactionTextMethod::Remove);
    EXPECT_EQ(data.lineArtMethod, pdfe::redaction::RedactionLineArtMethod::RemoveIfCovered);
}

TEST_F(RedactionManagerEdgeCaseTest, DefaultApplyOptionsValues) {
    pdfe::redaction::RedactionApplyOptions opts;
    EXPECT_TRUE(opts.blackBoxes);
    EXPECT_EQ(opts.imageMethod, pdfe::redaction::RedactionImageMethod::Remove);
    EXPECT_EQ(opts.textMethod, pdfe::redaction::RedactionTextMethod::Remove);
    EXPECT_EQ(opts.lineArtMethod, pdfe::redaction::RedactionLineArtMethod::RemoveIfCovered);
    EXPECT_TRUE(opts.rewriteDocument);
}

TEST_F(RedactionManagerEdgeCaseTest, DefaultApplyResultValues) {
    pdfe::redaction::RedactionApplyResult result;
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.marksApplied, 0);
    EXPECT_EQ(result.pagesAffected, 0);
    EXPECT_TRUE(result.errorMessage.empty());
    EXPECT_FALSE(result.documentRewritten);
}

TEST_F(RedactionManagerEdgeCaseTest, DefaultVerifyResultValues) {
    pdfe::redaction::RedactionVerifyResult result;
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.textRemoved);
    EXPECT_TRUE(result.noOrphanStreams);
    EXPECT_TRUE(result.singleRevision);
    EXPECT_TRUE(result.errorMessage.empty());
    EXPECT_TRUE(result.warnings.empty());
}

// ============================================================================
// MEDIUM: RedactionManager move semantics
// ============================================================================

TEST_F(RedactionManagerEdgeCaseTest, MoveConstruct) {
    auto mgr1 = std::make_unique<pdfe::redaction::RedactionManager>(context_);
    pdfe::redaction::RedactionManager mgr2(std::move(*mgr1));
    // mgr2 should be usable
    auto* page = getPage(0);
    auto data = mgr2.createRedactionMark(page, QRectF(100, 100, 100, 50));
    EXPECT_NE(data.annotIndex, -1)
        << "moved-from manager target must still produce valid mark indices";
}

TEST_F(RedactionManagerEdgeCaseTest, MoveAssign) {
    auto mgr1 = std::make_unique<pdfe::redaction::RedactionManager>(context_);
    pdfe::redaction::RedactionManager mgr2(context_);
    mgr2 = std::move(*mgr1);
    // mgr2 should be usable
    auto* page = getPage(0);
    auto data = mgr2.createRedactionMark(page, QRectF(100, 100, 100, 50));
    EXPECT_NE(data.annotIndex, -1)
        << "moved-from manager target must still produce valid mark indices";
}

// ============================================================================
// Command Edge Cases Fixture
// ============================================================================

class RedactionCommandEdgeCaseTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<pdfe::core::MuPdfContext>();
        auto pdfPath = std::filesystem::path(TEST_FIXTURES_DIR) / "test_redaction.pdf";
        if (!std::filesystem::exists(pdfPath)) {
            GTEST_SKIP() << "test_redaction.pdf not found";
        }
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

// ============================================================================
// HIGH: Command undo-before-execute guard
// ============================================================================

TEST_F(RedactionCommandEdgeCaseTest, CreateCommand_UndoBeforeExecute) {
    auto* page = getPage(0);
    pdfe::redaction::CreateRedactionCommand cmd(manager_.get(), page,
        QRectF(100, 100, 100, 50));

    // Undo before execute should be a no-op (guard: if (!executed_) return)
    EXPECT_NO_THROW(cmd.undo());

    // Should still work after undo no-op
    EXPECT_NO_THROW(cmd.execute());
    auto marks = manager_->getRedactionMarks(page);
    EXPECT_EQ(marks.size(), 1u);
}

TEST_F(RedactionCommandEdgeCaseTest, DeleteCommand_UndoBeforeExecute) {
    auto* page = getPage(0);
    auto created = manager_->createRedactionMark(page, QRectF(100, 100, 100, 50));

    pdfe::redaction::DeleteRedactionCommand cmd(manager_.get(), page,
        created.annotIndex);

    // Undo before execute should be a no-op
    EXPECT_NO_THROW(cmd.undo());

    // Mark should still be there
    EXPECT_EQ(manager_->getRedactionMarks(page).size(), 1u);
}

TEST_F(RedactionCommandEdgeCaseTest, ModifyCommand_UndoBeforeExecute) {
    auto* page = getPage(0);
    auto created = manager_->createRedactionMark(page, QRectF(100, 100, 100, 50));
    manager_->setOverlayText(page, created.annotIndex, "ORIGINAL");

    auto oldData = manager_->getRedactionMark(page, created.annotIndex);
    auto newData = oldData;
    newData.overlayText = "MODIFIED";

    pdfe::redaction::ModifyRedactionCommand cmd(
        manager_.get(), page, created.annotIndex,
        pdfe::redaction::ModifyRedactionCommand::Property::OverlayText,
        oldData, newData
    );

    // Undo before execute should be a no-op
    EXPECT_NO_THROW(cmd.undo());

    // Text should still be ORIGINAL
    auto retrieved = manager_->getRedactionMark(page, created.annotIndex);
    EXPECT_EQ(retrieved.overlayText, "ORIGINAL");
}

// ============================================================================
// HIGH: Merge rejection paths
// ============================================================================

TEST_F(RedactionCommandEdgeCaseTest, MergeWith_NonModifyCommand) {
    auto* page = getPage(0);
    auto created = manager_->createRedactionMark(page, QRectF(100, 100, 100, 50));

    auto data = manager_->getRedactionMark(page, created.annotIndex);

    auto modifyCmd = std::make_unique<pdfe::redaction::ModifyRedactionCommand>(
        manager_.get(), page, created.annotIndex,
        pdfe::redaction::ModifyRedactionCommand::Property::FillColor,
        data, data
    );

    // Merging with a CreateCommand should fail
    auto createCmd = std::make_unique<pdfe::redaction::CreateRedactionCommand>(
        manager_.get(), page, QRectF(200, 200, 100, 50)
    );

    EXPECT_FALSE(modifyCmd->mergeWith(createCmd.get()));
}

TEST_F(RedactionCommandEdgeCaseTest, MergeWith_DifferentAnnotIndex) {
    auto* page = getPage(0);
    auto created1 = manager_->createRedactionMark(page, QRectF(100, 100, 100, 50));
    auto created2 = manager_->createRedactionMark(page, QRectF(300, 100, 100, 50));

    auto data1 = manager_->getRedactionMark(page, created1.annotIndex);
    auto data2 = manager_->getRedactionMark(page, created2.annotIndex);

    auto cmd1 = std::make_unique<pdfe::redaction::ModifyRedactionCommand>(
        manager_.get(), page, created1.annotIndex,
        pdfe::redaction::ModifyRedactionCommand::Property::FillColor,
        data1, data1
    );

    auto cmd2 = std::make_unique<pdfe::redaction::ModifyRedactionCommand>(
        manager_.get(), page, created2.annotIndex,
        pdfe::redaction::ModifyRedactionCommand::Property::FillColor,
        data2, data2
    );

    EXPECT_FALSE(cmd1->mergeWith(cmd2.get()));
}

TEST_F(RedactionCommandEdgeCaseTest, MergeWith_DifferentPage) {
    auto* page0 = getPage(0);
    auto* page1 = getPage(1);

    auto created0 = manager_->createRedactionMark(page0, QRectF(100, 100, 100, 50));
    auto created1 = manager_->createRedactionMark(page1, QRectF(100, 100, 100, 50));

    auto data0 = manager_->getRedactionMark(page0, created0.annotIndex);
    auto data1 = manager_->getRedactionMark(page1, created1.annotIndex);

    auto cmd0 = std::make_unique<pdfe::redaction::ModifyRedactionCommand>(
        manager_.get(), page0, created0.annotIndex,
        pdfe::redaction::ModifyRedactionCommand::Property::FillColor,
        data0, data0
    );

    auto cmd1 = std::make_unique<pdfe::redaction::ModifyRedactionCommand>(
        manager_.get(), page1, created1.annotIndex,
        pdfe::redaction::ModifyRedactionCommand::Property::FillColor,
        data1, data1
    );

    EXPECT_FALSE(cmd0->mergeWith(cmd1.get()));
}

// ============================================================================
// HIGH: Create/Delete command base class methods
// ============================================================================

TEST_F(RedactionCommandEdgeCaseTest, CreateCommand_MergeId) {
    auto* page = getPage(0);
    pdfe::redaction::CreateRedactionCommand cmd(manager_.get(), page,
        QRectF(100, 100, 100, 50));

    // CreateRedactionCommand doesn't override mergeId, so it returns 0 (no merge)
    EXPECT_EQ(cmd.mergeId(), 0);
}

TEST_F(RedactionCommandEdgeCaseTest, DeleteCommand_MergeId) {
    auto* page = getPage(0);
    auto created = manager_->createRedactionMark(page, QRectF(100, 100, 100, 50));
    pdfe::redaction::DeleteRedactionCommand cmd(manager_.get(), page,
        created.annotIndex);

    EXPECT_EQ(cmd.mergeId(), 0);
}

TEST_F(RedactionCommandEdgeCaseTest, CreateCommand_MergeWith) {
    auto* page = getPage(0);
    auto cmd1 = std::make_unique<pdfe::redaction::CreateRedactionCommand>(
        manager_.get(), page, QRectF(100, 100, 100, 50));
    auto cmd2 = std::make_unique<pdfe::redaction::CreateRedactionCommand>(
        manager_.get(), page, QRectF(200, 200, 100, 50));

    // CreateRedactionCommand doesn't support merging
    EXPECT_FALSE(cmd1->mergeWith(cmd2.get()));
}

TEST_F(RedactionCommandEdgeCaseTest, DeleteCommand_MergeWith) {
    auto* page = getPage(0);
    auto c1 = manager_->createRedactionMark(page, QRectF(100, 100, 100, 50));
    auto c2 = manager_->createRedactionMark(page, QRectF(200, 200, 100, 50));

    auto cmd1 = std::make_unique<pdfe::redaction::DeleteRedactionCommand>(
        manager_.get(), page, c1.annotIndex);
    auto cmd2 = std::make_unique<pdfe::redaction::DeleteRedactionCommand>(
        manager_.get(), page, c2.annotIndex);

    EXPECT_FALSE(cmd1->mergeWith(cmd2.get()));
}

// ============================================================================
// MEDIUM: Multiple execute/undo/redo cycles
// ============================================================================

TEST_F(RedactionCommandEdgeCaseTest, CreateCommand_MultipleUndoRedoCycles) {
    auto* page = getPage(0);
    pdfe::redaction::CreateRedactionCommand cmd(manager_.get(), page,
        QRectF(100, 100, 100, 50));

    cmd.execute();
    EXPECT_EQ(manager_->getRedactionMarks(page).size(), 1u);

    cmd.undo();
    EXPECT_EQ(manager_->getRedactionMarks(page).size(), 0u);

    cmd.redo();
    EXPECT_EQ(manager_->getRedactionMarks(page).size(), 1u);

    cmd.undo();
    EXPECT_EQ(manager_->getRedactionMarks(page).size(), 0u);

    cmd.redo();
    EXPECT_EQ(manager_->getRedactionMarks(page).size(), 1u);
}

TEST_F(RedactionCommandEdgeCaseTest, DeleteCommand_MultipleUndoRedoCycles) {
    auto* page = getPage(0);
    auto created = manager_->createRedactionMark(page, QRectF(100, 100, 100, 50));

    pdfe::redaction::DeleteRedactionCommand cmd(manager_.get(), page,
        created.annotIndex);

    cmd.execute();
    EXPECT_EQ(manager_->getRedactionMarks(page).size(), 0u);

    cmd.undo();
    EXPECT_EQ(manager_->getRedactionMarks(page).size(), 1u);

    cmd.redo();
    EXPECT_EQ(manager_->getRedactionMarks(page).size(), 0u);

    cmd.undo();
    EXPECT_EQ(manager_->getRedactionMarks(page).size(), 1u);
}

// ============================================================================
// Verifier Edge Cases Fixture
// ============================================================================

class RedactionVerifierEdgeCaseTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<pdfe::core::MuPdfContext>();
        verifier_ = std::make_unique<pdfe::redaction::RedactionVerifier>(context_);

        pdfPath_ = std::filesystem::path(TEST_FIXTURES_DIR) / "test_redaction.pdf";
        if (!std::filesystem::exists(pdfPath_)) {
            GTEST_SKIP() << "test_redaction.pdf not found";
        }
    }

    std::shared_ptr<pdfe::core::MuPdfContext> context_;
    std::unique_ptr<pdfe::redaction::RedactionVerifier> verifier_;
    std::filesystem::path pdfPath_;

    std::filesystem::path createWorkingCopy() {
        auto tmpPath = std::filesystem::temp_directory_path() / "test_redaction_edge.pdf";
        std::filesystem::copy_file(pdfPath_, tmpPath,
            std::filesystem::copy_options::overwrite_existing);
        return tmpPath;
    }
};

// ============================================================================
// CRITICAL: checkTextRemoved negative case (text still present)
// ============================================================================

TEST_F(RedactionVerifierEdgeCaseTest, CheckTextRemoved_TextStillPresent) {
    // Verify against the ORIGINAL file (before redaction) with a region
    // that covers the ENTIRE page. Since we haven't applied redaction,
    // text should still be present in the file.
    auto workingCopy = createWorkingCopy();

    std::vector<pdfe::redaction::RedactionData> redactions;
    pdfe::redaction::RedactionData region;
    region.pageIndex = 0;
    // Cover entire standard letter page (612x792 pts) to guarantee hitting text
    region.rect = QRectF(0, 0, 612, 792);
    redactions.push_back(region);

    bool textRemoved = verifier_->checkTextRemoved(workingCopy, redactions);

    // Text should NOT be removed (we haven't applied redaction)
    EXPECT_FALSE(textRemoved) << "Original file should still contain text in redacted region";

    std::filesystem::remove(workingCopy);
}

// ============================================================================
// HIGH: Verifier with invalid page index in region
// ============================================================================

TEST_F(RedactionVerifierEdgeCaseTest, CheckTextRemoved_InvalidPageIndex) {
    auto workingCopy = createWorkingCopy();

    std::vector<pdfe::redaction::RedactionData> redactions;
    pdfe::redaction::RedactionData region;
    region.pageIndex = 999; // Way out of range
    region.rect = QRectF(100, 100, 100, 50);
    redactions.push_back(region);

    // Should handle gracefully (skip invalid page, return true)
    bool textRemoved = verifier_->checkTextRemoved(workingCopy, redactions);
    EXPECT_TRUE(textRemoved) << "Invalid page index should be skipped";

    std::filesystem::remove(workingCopy);
}

TEST_F(RedactionVerifierEdgeCaseTest, CheckTextRemoved_NegativePageIndex) {
    auto workingCopy = createWorkingCopy();

    std::vector<pdfe::redaction::RedactionData> redactions;
    pdfe::redaction::RedactionData region;
    region.pageIndex = -1;
    region.rect = QRectF(100, 100, 100, 50);
    redactions.push_back(region);

    bool textRemoved = verifier_->checkTextRemoved(workingCopy, redactions);
    EXPECT_TRUE(textRemoved) << "Negative page index should be skipped";

    std::filesystem::remove(workingCopy);
}

// ============================================================================
// HIGH: Verify with failed checks produces warnings
// ============================================================================

TEST_F(RedactionVerifierEdgeCaseTest, VerifyResult_WarningsPopulated) {
    auto workingCopy = createWorkingCopy();

    std::vector<pdfe::redaction::RedactionData> redactions;
    pdfe::redaction::RedactionData region;
    region.pageIndex = 0;
    // Cover entire page to guarantee finding text
    region.rect = QRectF(0, 0, 612, 792);
    redactions.push_back(region);

    auto result = verifier_->verify(workingCopy, redactions);

    // If text was not removed, there should be a warning about it
    if (!result.textRemoved) {
        bool foundTextWarning = false;
        for (const auto& w : result.warnings) {
            if (w.find("text") != std::string::npos || w.find("Text") != std::string::npos) {
                foundTextWarning = true;
                break;
            }
        }
        EXPECT_TRUE(foundTextWarning) << "Should have warning about unredacted text";
    }

    std::filesystem::remove(workingCopy);
}

// ============================================================================
// MEDIUM: Verifier move semantics
// ============================================================================

TEST_F(RedactionVerifierEdgeCaseTest, MoveConstruct) {
    auto v1 = std::make_unique<pdfe::redaction::RedactionVerifier>(context_);
    pdfe::redaction::RedactionVerifier v2(std::move(*v1));

    auto workingCopy = createWorkingCopy();
    std::vector<pdfe::redaction::RedactionData> empty;
    auto result = v2.verify(workingCopy, empty);
    EXPECT_TRUE(result.success);
    std::filesystem::remove(workingCopy);
}

// ============================================================================
// Panel Edge Cases Fixture
// ============================================================================

class RedactionPanelEdgeCaseTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!qApp) {
            GTEST_SKIP() << "QApplication not initialized";
        }
        panel_ = std::make_unique<pdfe::ui::RedactionPanel>();
    }

    void TearDown() override {
        panel_.reset();
    }

    std::unique_ptr<pdfe::ui::RedactionPanel> panel_;

    std::vector<pdfe::redaction::RedactionData> makeMarks(int count, bool allEnabled = true) {
        std::vector<pdfe::redaction::RedactionData> marks;
        for (int i = 0; i < count; ++i) {
            pdfe::redaction::RedactionData mark;
            mark.pageIndex = i % 3;
            mark.annotIndex = i;
            mark.rect = QRectF(100.0 + i * 50.0, 100.0, 200.0, 150.0);
            mark.fillColor = QColor(Qt::black);
            mark.enabled = allEnabled;
            marks.push_back(mark);
        }
        return marks;
    }
};

// ============================================================================
// CRITICAL: deleteMarkRequested signal
// ============================================================================

TEST_F(RedactionPanelEdgeCaseTest, DeleteMarkRequested_Signal) {
    QSignalSpy spy(panel_.get(), &pdfe::ui::RedactionPanel::deleteMarkRequested);
    ASSERT_TRUE(spy.isValid());

    std::vector<pdfe::redaction::RedactionData> marks;
    pdfe::redaction::RedactionData mark;
    mark.pageIndex = 3;
    mark.annotIndex = 7;
    mark.rect = QRectF(100, 100, 200, 150);
    mark.fillColor = QColor(Qt::black);
    marks.push_back(mark);

    panel_->setMarks(marks);
    panel_->show();
    (void)QTest::qWaitForWindowExposed(panel_.get());

    auto* treeWidget = panel_->findChild<QTreeWidget*>("redactionMarksList");
    ASSERT_NE(treeWidget, nullptr);
    ASSERT_EQ(treeWidget->topLevelItemCount(), 1);

    // Phase-16 fix (qt_test_gap_audit finding #23): drive a real
    // click on the tree-widget viewport at the item's rect so the
    // panel's onItemClicked slot fires via the natural Qt event
    // chain — not via a manual itemClicked emit. This catches a
    // regression that switches the slot connection from
    // itemClicked to currentItemChanged.
    auto* item = treeWidget->topLevelItem(0);
    treeWidget->setCurrentItem(item);
    QRect itemRect = treeWidget->visualItemRect(item);
    QTest::mouseClick(treeWidget->viewport(), Qt::LeftButton,
                      Qt::NoModifier, itemRect.center());
    QApplication::processEvents();

    auto* deleteButton = panel_->findChild<QPushButton*>("deleteRedactionButton");
    ASSERT_NE(deleteButton, nullptr);
    ASSERT_TRUE(deleteButton->isEnabled()) << "Delete button should be enabled after item click";

    // Click delete button
    deleteButton->click();
    QApplication::processEvents();

    ASSERT_GE(spy.count(), 1) << "deleteMarkRequested signal should have been emitted";
    auto args = spy.takeFirst();
    EXPECT_EQ(args.at(0).toInt(), 3) << "Signal should carry pageIndex=3";
    EXPECT_EQ(args.at(1).toInt(), 7) << "Signal should carry annotIndex=7";

    panel_->hide();
}

// ============================================================================
// CRITICAL: markSelected signal
// ============================================================================

TEST_F(RedactionPanelEdgeCaseTest, MarkSelected_Signal) {
    QSignalSpy spy(panel_.get(), &pdfe::ui::RedactionPanel::markSelected);
    ASSERT_TRUE(spy.isValid());

    std::vector<pdfe::redaction::RedactionData> marks;
    pdfe::redaction::RedactionData mark;
    mark.pageIndex = 2;
    mark.annotIndex = 5;
    mark.rect = QRectF(100, 100, 200, 150);
    mark.fillColor = QColor(Qt::black);
    marks.push_back(mark);

    panel_->setMarks(marks);

    auto* treeWidget = panel_->findChild<QTreeWidget*>("redactionMarksList");
    ASSERT_NE(treeWidget, nullptr);
    ASSERT_EQ(treeWidget->topLevelItemCount(), 1);

    // Programmatically trigger itemClicked (simulates user clicking an item)
    auto* item = treeWidget->topLevelItem(0);
    treeWidget->setCurrentItem(item);
    emit treeWidget->itemClicked(item, 0);
    QApplication::processEvents();

    ASSERT_GE(spy.count(), 1) << "markSelected signal should have been emitted";
    auto args = spy.takeFirst();
    EXPECT_EQ(args.at(0).toInt(), 2) << "Signal should carry pageIndex=2";
    EXPECT_EQ(args.at(1).toInt(), 5) << "Signal should carry annotIndex=5";
}

// ============================================================================
// CRITICAL: Disabled marks filtering
// ============================================================================

TEST_F(RedactionPanelEdgeCaseTest, DisabledMarks_Filtered) {
    std::vector<pdfe::redaction::RedactionData> marks;

    pdfe::redaction::RedactionData enabledMark;
    enabledMark.pageIndex = 0;
    enabledMark.annotIndex = 0;
    enabledMark.rect = QRectF(100, 100, 200, 150);
    enabledMark.fillColor = QColor(Qt::black);
    enabledMark.enabled = true;
    marks.push_back(enabledMark);

    pdfe::redaction::RedactionData disabledMark;
    disabledMark.pageIndex = 1;
    disabledMark.annotIndex = 1;
    disabledMark.rect = QRectF(200, 100, 200, 150);
    disabledMark.fillColor = QColor(Qt::red);
    disabledMark.enabled = false;
    marks.push_back(disabledMark);

    pdfe::redaction::RedactionData enabledMark2;
    enabledMark2.pageIndex = 2;
    enabledMark2.annotIndex = 2;
    enabledMark2.rect = QRectF(300, 100, 200, 150);
    enabledMark2.fillColor = QColor(Qt::blue);
    enabledMark2.enabled = true;
    marks.push_back(enabledMark2);

    panel_->setMarks(marks);

    auto* treeWidget = panel_->findChild<QTreeWidget*>("redactionMarksList");
    ASSERT_NE(treeWidget, nullptr);

    // Only 2 enabled marks should appear
    EXPECT_EQ(treeWidget->topLevelItemCount(), 2)
        << "Disabled marks should be filtered out from the tree";

    // Verify the mark count label reflects enabled-only count
    auto* countLabel = panel_->findChild<QLabel*>("markCountLabel");
    ASSERT_NE(countLabel, nullptr);
    EXPECT_TRUE(countLabel->text().contains("2"))
        << "Mark count should reflect only enabled marks (actual: "
        << countLabel->text().toStdString() << ")";
}

TEST_F(RedactionPanelEdgeCaseTest, AllDisabledMarks_EmptyPanel) {
    std::vector<pdfe::redaction::RedactionData> marks;

    for (int i = 0; i < 3; ++i) {
        pdfe::redaction::RedactionData mark;
        mark.pageIndex = i;
        mark.annotIndex = i;
        mark.rect = QRectF(100, 100, 200, 150);
        mark.fillColor = QColor(Qt::black);
        mark.enabled = false;
        marks.push_back(mark);
    }

    panel_->setMarks(marks);

    auto* treeWidget = panel_->findChild<QTreeWidget*>("redactionMarksList");
    ASSERT_NE(treeWidget, nullptr);
    EXPECT_EQ(treeWidget->topLevelItemCount(), 0);

    auto* applyButton = panel_->findChild<QPushButton*>("applyRedactionsButton");
    ASSERT_NE(applyButton, nullptr);
    EXPECT_FALSE(applyButton->isEnabled())
        << "Apply button should be disabled when all marks are disabled";
}

// ============================================================================
// MEDIUM: Panel formatting - singular mark count label
// ============================================================================

TEST_F(RedactionPanelEdgeCaseTest, SingularMarkCountLabel) {
    std::vector<pdfe::redaction::RedactionData> marks;
    pdfe::redaction::RedactionData mark;
    mark.pageIndex = 0;
    mark.annotIndex = 0;
    mark.rect = QRectF(100, 100, 200, 150);
    mark.fillColor = QColor(Qt::black);
    marks.push_back(mark);

    panel_->setMarks(marks);

    auto* countLabel = panel_->findChild<QLabel*>("markCountLabel");
    ASSERT_NE(countLabel, nullptr);
    EXPECT_TRUE(countLabel->text().contains("1"))
        << "Label should contain '1' (actual: " << countLabel->text().toStdString() << ")";
    // Verify it says "mark" (singular) not "marks" (plural)
    EXPECT_TRUE(countLabel->text().contains("mark") && !countLabel->text().contains("marks"))
        << "Singular form expected (actual: " << countLabel->text().toStdString() << ")";
}

TEST_F(RedactionPanelEdgeCaseTest, PluralMarkCountLabel) {
    auto marks = makeMarks(3);
    panel_->setMarks(marks);

    auto* countLabel = panel_->findChild<QLabel*>("markCountLabel");
    ASSERT_NE(countLabel, nullptr);
    EXPECT_TRUE(countLabel->text().contains("3"))
        << "Label should contain '3' (actual: " << countLabel->text().toStdString() << ")";
    EXPECT_TRUE(countLabel->text().contains("marks"))
        << "Plural form expected (actual: " << countLabel->text().toStdString() << ")";
}

// ============================================================================
// MEDIUM: Panel color formatting branches
// ============================================================================

TEST_F(RedactionPanelEdgeCaseTest, ColorFormatting_NamedColors) {
    auto* treeWidget = panel_->findChild<QTreeWidget*>("redactionMarksList");
    ASSERT_NE(treeWidget, nullptr);

    // Test various named colors
    struct ColorTest {
        QColor color;
        QString expectedText;
    };
    std::vector<ColorTest> tests = {
        {QColor(Qt::black), "Black"},
        {QColor(Qt::white), "White"},
        {QColor(Qt::red), "Red"},
        {QColor(Qt::blue), "Blue"},
        {QColor(Qt::green), "Green"},
        {QColor(Qt::yellow), "Yellow"},
    };

    for (size_t i = 0; i < tests.size(); ++i) {
        std::vector<pdfe::redaction::RedactionData> marks;
        pdfe::redaction::RedactionData mark;
        mark.pageIndex = 0;
        mark.annotIndex = static_cast<int>(i);
        mark.rect = QRectF(100, 100, 200, 150);
        mark.fillColor = tests[i].color;
        marks.push_back(mark);

        panel_->setMarks(marks);

        ASSERT_EQ(treeWidget->topLevelItemCount(), 1);
        auto* item = treeWidget->topLevelItem(0);
        QString colorText = item->text(2);
        EXPECT_EQ(colorText, tests[i].expectedText)
            << "Color " << i << " expected '"
            << tests[i].expectedText.toStdString()
            << "' but got '" << colorText.toStdString() << "'";
    }
}

TEST_F(RedactionPanelEdgeCaseTest, ColorFormatting_HexFallback) {
    std::vector<pdfe::redaction::RedactionData> marks;
    pdfe::redaction::RedactionData mark;
    mark.pageIndex = 0;
    mark.annotIndex = 0;
    mark.rect = QRectF(100, 100, 200, 150);
    mark.fillColor = QColor(128, 64, 192); // Not a named color
    marks.push_back(mark);

    panel_->setMarks(marks);

    auto* treeWidget = panel_->findChild<QTreeWidget*>("redactionMarksList");
    ASSERT_NE(treeWidget, nullptr);
    ASSERT_EQ(treeWidget->topLevelItemCount(), 1);

    auto* item = treeWidget->topLevelItem(0);
    QString colorText = item->text(2);
    // Should be hex format like #8040C0
    EXPECT_TRUE(colorText.startsWith("#"))
        << "Non-named color should be hex (actual: " << colorText.toStdString() << ")";
    EXPECT_EQ(colorText.length(), 7)
        << "Hex color should be #RRGGBB format (actual: " << colorText.toStdString() << ")";
}

// ============================================================================
// MEDIUM: Panel overlay text truncation
// ============================================================================

TEST_F(RedactionPanelEdgeCaseTest, OverlayText_Truncation) {
    std::vector<pdfe::redaction::RedactionData> marks;
    pdfe::redaction::RedactionData mark;
    mark.pageIndex = 0;
    mark.annotIndex = 0;
    mark.rect = QRectF(100, 100, 200, 150);
    mark.fillColor = QColor(Qt::black);
    mark.overlayText = "This is a very long overlay text that should be truncated in the panel display";
    marks.push_back(mark);

    panel_->setMarks(marks);

    auto* treeWidget = panel_->findChild<QTreeWidget*>("redactionMarksList");
    ASSERT_NE(treeWidget, nullptr);
    ASSERT_EQ(treeWidget->topLevelItemCount(), 1);

    auto* item = treeWidget->topLevelItem(0);
    QString displayText = item->text(3);

    // Text longer than 30 chars should be truncated with "..."
    EXPECT_LE(displayText.length(), 33)
        << "Text should be truncated (actual length: " << displayText.length() << ")";
    EXPECT_TRUE(displayText.endsWith("..."))
        << "Truncated text should end with '...' (actual: " << displayText.toStdString() << ")";
}

TEST_F(RedactionPanelEdgeCaseTest, OverlayText_ShortNotTruncated) {
    std::vector<pdfe::redaction::RedactionData> marks;
    pdfe::redaction::RedactionData mark;
    mark.pageIndex = 0;
    mark.annotIndex = 0;
    mark.rect = QRectF(100, 100, 200, 150);
    mark.fillColor = QColor(Qt::black);
    mark.overlayText = "SHORT";
    marks.push_back(mark);

    panel_->setMarks(marks);

    auto* treeWidget = panel_->findChild<QTreeWidget*>("redactionMarksList");
    ASSERT_NE(treeWidget, nullptr);
    ASSERT_EQ(treeWidget->topLevelItemCount(), 1);

    auto* item = treeWidget->topLevelItem(0);
    QString displayText = item->text(3);
    EXPECT_EQ(displayText, "SHORT")
        << "Short text should not be truncated (actual: " << displayText.toStdString() << ")";
}

TEST_F(RedactionPanelEdgeCaseTest, OverlayText_Empty) {
    std::vector<pdfe::redaction::RedactionData> marks;
    pdfe::redaction::RedactionData mark;
    mark.pageIndex = 0;
    mark.annotIndex = 0;
    mark.rect = QRectF(100, 100, 200, 150);
    mark.fillColor = QColor(Qt::black);
    mark.overlayText = "";
    marks.push_back(mark);

    panel_->setMarks(marks);

    auto* treeWidget = panel_->findChild<QTreeWidget*>("redactionMarksList");
    ASSERT_NE(treeWidget, nullptr);
    ASSERT_EQ(treeWidget->topLevelItemCount(), 1);

    auto* item = treeWidget->topLevelItem(0);
    QString displayText = item->text(3);
    EXPECT_TRUE(displayText.isEmpty())
        << "Empty overlay text should remain empty (actual: " << displayText.toStdString() << ")";
}

// ============================================================================
// MEDIUM: Delete button state management
// ============================================================================

TEST_F(RedactionPanelEdgeCaseTest, DeleteButton_EnabledAfterItemClick) {
    auto* deleteButton = panel_->findChild<QPushButton*>("deleteRedactionButton");
    ASSERT_NE(deleteButton, nullptr);
    EXPECT_FALSE(deleteButton->isEnabled());

    auto marks = makeMarks(2);
    panel_->setMarks(marks);

    auto* treeWidget = panel_->findChild<QTreeWidget*>("redactionMarksList");
    ASSERT_NE(treeWidget, nullptr);

    // Programmatically trigger itemClicked (simulates user clicking an item)
    auto* item = treeWidget->topLevelItem(0);
    treeWidget->setCurrentItem(item);
    emit treeWidget->itemClicked(item, 0);
    QApplication::processEvents();

    EXPECT_TRUE(deleteButton->isEnabled())
        << "Delete button should be enabled after clicking an item";
}

TEST_F(RedactionPanelEdgeCaseTest, DeleteButton_DisabledAfterClear) {
    auto marks = makeMarks(2);
    panel_->setMarks(marks);

    auto* treeWidget = panel_->findChild<QTreeWidget*>("redactionMarksList");
    ASSERT_NE(treeWidget, nullptr);

    // Programmatically trigger itemClicked to enable delete
    auto* item = treeWidget->topLevelItem(0);
    treeWidget->setCurrentItem(item);
    emit treeWidget->itemClicked(item, 0);
    QApplication::processEvents();

    auto* deleteButton = panel_->findChild<QPushButton*>("deleteRedactionButton");
    ASSERT_NE(deleteButton, nullptr);
    EXPECT_TRUE(deleteButton->isEnabled());

    panel_->clear();
    EXPECT_FALSE(deleteButton->isEnabled())
        << "Delete button should be disabled after clear";
}

// ============================================================================
// MEDIUM: Panel page number formatting (1-based display)
// ============================================================================

TEST_F(RedactionPanelEdgeCaseTest, PageNumberDisplay_OneBased) {
    std::vector<pdfe::redaction::RedactionData> marks;
    pdfe::redaction::RedactionData mark;
    mark.pageIndex = 0; // 0-based index
    mark.annotIndex = 0;
    mark.rect = QRectF(100, 100, 200, 150);
    mark.fillColor = QColor(Qt::black);
    marks.push_back(mark);

    panel_->setMarks(marks);

    auto* treeWidget = panel_->findChild<QTreeWidget*>("redactionMarksList");
    ASSERT_NE(treeWidget, nullptr);
    ASSERT_EQ(treeWidget->topLevelItemCount(), 1);

    auto* item = treeWidget->topLevelItem(0);
    QString pageText = item->text(0);
    // pageIndex 0 should display as "1" (1-based)
    EXPECT_EQ(pageText, "1")
        << "Page 0 should display as '1' (actual: " << pageText.toStdString() << ")";
}

TEST_F(RedactionPanelEdgeCaseTest, PageNumberDisplay_MultiplePages) {
    std::vector<pdfe::redaction::RedactionData> marks;
    for (int i = 0; i < 3; ++i) {
        pdfe::redaction::RedactionData mark;
        mark.pageIndex = i;
        mark.annotIndex = i;
        mark.rect = QRectF(100, 100, 200, 150);
        mark.fillColor = QColor(Qt::black);
        marks.push_back(mark);
    }

    panel_->setMarks(marks);

    auto* treeWidget = panel_->findChild<QTreeWidget*>("redactionMarksList");
    ASSERT_NE(treeWidget, nullptr);
    ASSERT_EQ(treeWidget->topLevelItemCount(), 3);

    EXPECT_EQ(treeWidget->topLevelItem(0)->text(0), "1");
    EXPECT_EQ(treeWidget->topLevelItem(1)->text(0), "2");
    EXPECT_EQ(treeWidget->topLevelItem(2)->text(0), "3");
}

// ============================================================================
// MEDIUM: Region formatting
// ============================================================================

TEST_F(RedactionPanelEdgeCaseTest, RegionFormatting) {
    std::vector<pdfe::redaction::RedactionData> marks;
    pdfe::redaction::RedactionData mark;
    mark.pageIndex = 0;
    mark.annotIndex = 0;
    mark.rect = QRectF(150.5, 200.25, 300.75, 100.0);
    mark.fillColor = QColor(Qt::black);
    marks.push_back(mark);

    panel_->setMarks(marks);

    auto* treeWidget = panel_->findChild<QTreeWidget*>("redactionMarksList");
    ASSERT_NE(treeWidget, nullptr);
    ASSERT_EQ(treeWidget->topLevelItemCount(), 1);

    auto* item = treeWidget->topLevelItem(0);
    QString regionText = item->text(1);
    // Should contain formatted coordinates
    EXPECT_FALSE(regionText.isEmpty())
        << "Region should have formatted coordinates";
    EXPECT_TRUE(regionText.contains("150"))
        << "Region should contain x coordinate (actual: " << regionText.toStdString() << ")";
    EXPECT_TRUE(regionText.contains("200"))
        << "Region should contain y coordinate (actual: " << regionText.toStdString() << ")";
}

// ============================================================================
// LOW: UserRole data stored correctly
// ============================================================================

TEST_F(RedactionPanelEdgeCaseTest, ItemDataStored_PageAndAnnotIndex) {
    std::vector<pdfe::redaction::RedactionData> marks;
    pdfe::redaction::RedactionData mark;
    mark.pageIndex = 7;
    mark.annotIndex = 13;
    mark.rect = QRectF(100, 100, 200, 150);
    mark.fillColor = QColor(Qt::black);
    marks.push_back(mark);

    panel_->setMarks(marks);

    auto* treeWidget = panel_->findChild<QTreeWidget*>("redactionMarksList");
    ASSERT_NE(treeWidget, nullptr);
    ASSERT_EQ(treeWidget->topLevelItemCount(), 1);

    auto* item = treeWidget->topLevelItem(0);
    EXPECT_EQ(item->data(0, Qt::UserRole).toInt(), 7);
    EXPECT_EQ(item->data(0, Qt::UserRole + 1).toInt(), 13);
}

// ============================================================================
// MEDIUM: Panel setMarks replaces previous content
// ============================================================================

TEST_F(RedactionPanelEdgeCaseTest, SetMarks_Replaces) {
    auto marks1 = makeMarks(3);
    panel_->setMarks(marks1);

    auto* treeWidget = panel_->findChild<QTreeWidget*>("redactionMarksList");
    ASSERT_NE(treeWidget, nullptr);
    EXPECT_EQ(treeWidget->topLevelItemCount(), 3);

    auto marks2 = makeMarks(1);
    panel_->setMarks(marks2);
    EXPECT_EQ(treeWidget->topLevelItemCount(), 1);
}

TEST_F(RedactionPanelEdgeCaseTest, SetMarks_EmptyVector) {
    auto marks = makeMarks(3);
    panel_->setMarks(marks);

    auto* treeWidget = panel_->findChild<QTreeWidget*>("redactionMarksList");
    ASSERT_NE(treeWidget, nullptr);
    EXPECT_EQ(treeWidget->topLevelItemCount(), 3);

    std::vector<pdfe::redaction::RedactionData> empty;
    panel_->setMarks(empty);
    EXPECT_EQ(treeWidget->topLevelItemCount(), 0);

    auto* applyButton = panel_->findChild<QPushButton*>("applyRedactionsButton");
    EXPECT_FALSE(applyButton->isEnabled());
}

// ============================================================================
// MEDIUM: Apply options combinations
// ============================================================================

class RedactionApplyOptionsTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<pdfe::core::MuPdfContext>();
        auto srcPath = std::filesystem::path(TEST_FIXTURES_DIR) / "test_redaction.pdf";
        if (!std::filesystem::exists(srcPath)) {
            GTEST_SKIP() << "test_redaction.pdf not found";
        }

        tmpPath_ = std::filesystem::temp_directory_path() / "test_redaction_options.pdf";
        std::filesystem::copy_file(srcPath, tmpPath_,
            std::filesystem::copy_options::overwrite_existing);

        document_ = pdfe::core::Document::open(context_, tmpPath_);
        ASSERT_NE(document_, nullptr);
        manager_ = std::make_unique<pdfe::redaction::RedactionManager>(context_);
    }

    void TearDown() override {
        pages_.clear();
        document_.reset();
        manager_.reset();
        if (std::filesystem::exists(tmpPath_)) {
            std::filesystem::remove(tmpPath_);
        }
    }

    std::shared_ptr<pdfe::core::MuPdfContext> context_;
    std::unique_ptr<pdfe::core::Document> document_;
    std::unique_ptr<pdfe::redaction::RedactionManager> manager_;
    std::filesystem::path tmpPath_;
    std::vector<std::unique_ptr<pdfe::core::Page>> pages_;

    pdfe::core::Page* getPage(int index) {
        pages_.push_back(document_->page(index));
        return pages_.back().get();
    }
};

TEST_F(RedactionApplyOptionsTest, TextMethodNone) {
    auto* page = getPage(0);
    manager_->createRedactionMark(page, QRectF(100, 100, 100, 50));

    pdfe::redaction::RedactionApplyOptions options;
    options.textMethod = pdfe::redaction::RedactionTextMethod::None;
    options.rewriteDocument = false;

    auto result = manager_->applyAllRedactions(document_.get(), options);
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.marksApplied, 1);
}

TEST_F(RedactionApplyOptionsTest, ImageMethodPixels) {
    auto* page = getPage(0);
    manager_->createRedactionMark(page, QRectF(100, 100, 100, 50));

    pdfe::redaction::RedactionApplyOptions options;
    options.imageMethod = pdfe::redaction::RedactionImageMethod::Pixels;
    options.rewriteDocument = false;

    auto result = manager_->applyAllRedactions(document_.get(), options);
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.marksApplied, 1);
    // Verify in-memory state: the mark was consumed by apply.
    auto remaining = manager_->getRedactionMarks(page);
    EXPECT_EQ(remaining.size(), 0u);
}

TEST_F(RedactionApplyOptionsTest, ImageMethodNone) {
    auto* page = getPage(0);
    manager_->createRedactionMark(page, QRectF(100, 100, 100, 50));

    pdfe::redaction::RedactionApplyOptions options;
    options.imageMethod = pdfe::redaction::RedactionImageMethod::None;
    options.rewriteDocument = false;

    auto result = manager_->applyAllRedactions(document_.get(), options);
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.marksApplied, 1);
    auto remaining = manager_->getRedactionMarks(page);
    EXPECT_EQ(remaining.size(), 0u);
}

TEST_F(RedactionApplyOptionsTest, LineArtRemoveIfTouched) {
    auto* page = getPage(0);
    manager_->createRedactionMark(page, QRectF(100, 100, 100, 50));

    pdfe::redaction::RedactionApplyOptions options;
    options.lineArtMethod = pdfe::redaction::RedactionLineArtMethod::RemoveIfTouched;
    options.rewriteDocument = false;

    auto result = manager_->applyAllRedactions(document_.get(), options);
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.marksApplied, 1);
    auto remaining = manager_->getRedactionMarks(page);
    EXPECT_EQ(remaining.size(), 0u);
}

TEST_F(RedactionApplyOptionsTest, LineArtNone) {
    auto* page = getPage(0);
    manager_->createRedactionMark(page, QRectF(100, 100, 100, 50));

    pdfe::redaction::RedactionApplyOptions options;
    options.lineArtMethod = pdfe::redaction::RedactionLineArtMethod::None;
    options.rewriteDocument = false;

    auto result = manager_->applyAllRedactions(document_.get(), options);
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.marksApplied, 1);
    auto remaining = manager_->getRedactionMarks(page);
    EXPECT_EQ(remaining.size(), 0u);
}

TEST_F(RedactionApplyOptionsTest, BlackBoxesFalse) {
    auto* page = getPage(0);
    manager_->createRedactionMark(page, QRectF(100, 100, 100, 50));

    pdfe::redaction::RedactionApplyOptions options;
    options.blackBoxes = false;
    options.rewriteDocument = false;

    auto result = manager_->applyAllRedactions(document_.get(), options);
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.marksApplied, 1);
    auto remaining = manager_->getRedactionMarks(page);
    EXPECT_EQ(remaining.size(), 0u);
}

// ============================================================================
// Pipeline a7c4f891 phase-4 — Theme B Critical security tests B4/B5/B6:
//   B4: RedactionOnEncryptedPdf — redaction must work on encrypted PDF
//       (encrypt → reopen → mark+apply → save → reopen → verify text gone)
//   B5: RedactionPreservesAnnotations — sticky-note placed AT a distinct
//       region must survive applyRedactions when the redaction is at a
//       non-overlapping region (locks the "redaction does not nuke
//       unrelated annotations" contract)
//   B6: OCR text layer redaction — apply redaction to a fixture with
//       hidden/OCR text layer; verify the redacted text is absent from
//       the saved file's text extraction
// ============================================================================

#include "encryption/EncryptionManager.h"
#include "encryption/EncryptionSettings.h"
#include "annotations/AnnotationManager.h"
#include "annotations/AnnotationType.h"

class RedactionSecurityTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<pdfe::core::MuPdfContext>();
        srcPath_ = std::filesystem::path(TEST_FIXTURES_DIR) / "test_redaction.pdf";
        if (!std::filesystem::exists(srcPath_)) {
            GTEST_SKIP() << "test_redaction.pdf not found";
        }
        const ::testing::TestInfo* info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        tempDir_ = std::filesystem::temp_directory_path()
                   / "pdfe_redaction_security" / info->name();
        std::filesystem::create_directories(tempDir_);
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(tempDir_, ec);
    }

    std::shared_ptr<pdfe::core::MuPdfContext> context_;
    std::filesystem::path srcPath_;
    std::filesystem::path tempDir_;

    std::filesystem::path tmpFile(const std::string& stem) {
        return tempDir_ / (stem + ".pdf");
    }
};

// ─── B4: RedactionOnEncryptedPdf ────────────────────────────────────
// Lock the contract that redaction works through encryption: a doc
// can be encrypted, reopened, redacted, re-saved with encryption,
// and reopened again — and the redacted text must be absent from
// the final document. This is a security contract because a
// regression that silently bypasses redaction-on-encrypted-doc
// would leave sensitive text in encrypted documents that users
// believed they had redacted.

TEST_F(RedactionSecurityTest, RedactionOnEncryptedPdf) {
    using namespace pdfe::encryption;

    // Stage 1: Encrypt the fixture.
    auto encOut = tmpFile("encrypted_source");
    {
        auto doc = pdfe::core::Document::open(context_, srcPath_);
        ASSERT_NE(doc, nullptr);

        EncryptionManager encMgr(context_);
        EncryptionSettings settings;
        settings.algorithm = EncryptionAlgorithm::AES_128;
        settings.userPassword = "redact_user";
        settings.ownerPassword = "redact_owner";
        settings.permissions = PermissionFlags::all();
        encMgr.saveEncrypted(doc.get(), encOut, settings);
    }
    ASSERT_TRUE(std::filesystem::exists(encOut));

    // Stage 2: Reopen encrypted doc, verify it has the secret text.
    auto encDoc = pdfe::core::Document::open(context_, encOut, "redact_user");
    ASSERT_NE(encDoc, nullptr) << "encrypted doc must reopen with user password";

    auto pageBefore = encDoc->page(0);
    ASSERT_NE(pageBefore, nullptr);
    std::string textBefore = pageBefore->extractPlainText();
    ASSERT_NE(textBefore.find("Social Security"), std::string::npos)
        << "fixture must contain 'Social Security' before redaction; "
        << "if this fails, the encryption path corrupted text extraction";

    // Stage 3: Cover the entire page with a redaction mark to
    // guarantee we hit the target text. Using a full-page redaction
    // sidesteps coordinate-system uncertainty (PDF bottom-left vs.
    // Qt top-left) while still proving the byte-level absence
    // contract.
    QRectF pageRect = pageBefore->boundingBox();
    pdfe::redaction::RedactionManager mgr(context_);
    auto mark = mgr.createRedactionMark(pageBefore.get(), pageRect);
    ASSERT_NE(mark.annotIndex, -1)
        << "createRedactionMark must succeed on encrypted-source doc";
    pageBefore.reset();

    // Stage 4: Apply redactions on the encrypted-source document.
    pdfe::redaction::RedactionApplyOptions opts;
    opts.rewriteDocument = false;
    auto applyResult = mgr.applyAllRedactions(encDoc.get(), opts);
    ASSERT_TRUE(applyResult.success)
        << "applyAllRedactions must succeed on encrypted doc: "
        << applyResult.errorMessage;
    EXPECT_GE(applyResult.marksApplied, 1)
        << "at least one redaction mark must have been applied";

    // Stage 5: Save the redacted doc as encrypted again, reopen,
    // and verify the target text is GONE.
    auto redactedOut = tmpFile("redacted_encrypted");
    {
        EncryptionManager encMgr2(context_);
        EncryptionSettings settings2;
        settings2.algorithm = EncryptionAlgorithm::AES_128;
        settings2.userPassword = "redact_user2";
        settings2.ownerPassword = "redact_owner2";
        settings2.permissions = PermissionFlags::all();
        encMgr2.saveEncrypted(encDoc.get(), redactedOut, settings2);
    }
    encDoc.reset();
    ASSERT_TRUE(std::filesystem::exists(redactedOut));

    auto finalDoc = pdfe::core::Document::open(context_, redactedOut, "redact_user2");
    ASSERT_NE(finalDoc, nullptr)
        << "redacted+re-encrypted doc must reopen with the new user password";

    auto finalPage = finalDoc->page(0);
    ASSERT_NE(finalPage, nullptr);
    std::string textAfter = finalPage->extractPlainText();

    // The redacted text MUST be absent from the saved file.
    EXPECT_EQ(textAfter.find("Social Security"), std::string::npos)
        << "Redacted 'Social Security' text leaked through to the "
        << "re-encrypted output file. Security regression: "
        << "applyRedactions on an encrypted-source document failed "
        << "to strip the target bytes before re-encryption.";
    EXPECT_EQ(textAfter.find("123-45-6789"), std::string::npos)
        << "Redacted SSN digits leaked through to the re-encrypted "
        << "output file";
    EXPECT_EQ(textAfter.find("Account Number"), std::string::npos)
        << "Redacted 'Account Number' text leaked through";
}

// ─── B5: RedactionPreservesAnnotations ──────────────────────────────
// Lock the contract that applyRedactions does NOT destroy
// non-overlapping annotations on the same page. A regression that
// nukes all annotations during apply would silently strip user
// review notes, highlights, and other markup — and a "successful"
// redaction operation would lose the surrounding context.

TEST_F(RedactionSecurityTest, RedactionPreservesAnnotations) {
    auto doc = pdfe::core::Document::open(context_, srcPath_);
    ASSERT_NE(doc, nullptr);

    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    // Stage 1: place a sticky-note annotation at the bottom-left
    // corner of the page (small region: 24x24 pts at coordinates
    // far from the redaction target).
    pdfe::annotations::AnnotationManager annMgr(context_);
    QRectF stickyRect(20.0, 20.0, 24.0, 24.0);
    auto sticky = annMgr.createAnnotation(
        page.get(), pdfe::annotations::AnnotationType::Text, stickyRect);
    ASSERT_NE(sticky.annotIndex, -1)
        << "sticky-note creation must succeed";
    // Tag it with a contents string so we can identify it after reopen.
    annMgr.setContents(page.get(), sticky.annotIndex,
        "REDACTION_TEST_SURVIVES_SENTINEL");

    auto annsBefore = annMgr.getAnnotations(page.get());
    int textAnnotsBefore = 0;
    for (const auto& a : annsBefore) {
        if (a.type == pdfe::annotations::AnnotationType::Text) {
            ++textAnnotsBefore;
        }
    }
    ASSERT_GE(textAnnotsBefore, 1)
        << "sticky-note must be visible in getAnnotations before redaction";

    // Stage 2: place a redaction mark at a non-overlapping region
    // (top-right corner area, well away from the sticky-note).
    pdfe::redaction::RedactionManager mgr(context_);
    QRectF redactRect(400.0, 400.0, 50.0, 50.0);
    // Verify non-overlap: sticky=(20,20,24,24) → intersects=(20-44, 20-44)
    // redact=(400,400,50,50) → (400-450, 400-450) — clearly disjoint.
    ASSERT_FALSE(stickyRect.intersects(redactRect))
        << "test precondition: sticky and redaction rects must be disjoint";

    auto mark = mgr.createRedactionMark(page.get(), redactRect);
    ASSERT_NE(mark.annotIndex, -1);

    page.reset();

    // Stage 3: apply redactions.
    pdfe::redaction::RedactionApplyOptions opts;
    opts.rewriteDocument = false;
    auto applyResult = mgr.applyAllRedactions(doc.get(), opts);
    ASSERT_TRUE(applyResult.success) << applyResult.errorMessage;

    // Stage 4: save and reopen.
    auto out = tmpFile("redaction_preserves_annots");
    doc->saveAs(out);
    doc.reset();

    auto reopened = pdfe::core::Document::open(context_, out);
    ASSERT_NE(reopened, nullptr);
    auto reopenedPage = reopened->page(0);
    ASSERT_NE(reopenedPage, nullptr);

    // Stage 5: verify the sticky-note survived the redaction +
    // save + reopen cycle.
    pdfe::annotations::AnnotationManager annMgr2(context_);
    auto annsAfter = annMgr2.getAnnotations(reopenedPage.get());

    bool foundSurvivingSticky = false;
    for (const auto& a : annsAfter) {
        if (a.type == pdfe::annotations::AnnotationType::Text
            && a.contents.find("REDACTION_TEST_SURVIVES_SENTINEL")
                != std::string::npos) {
            foundSurvivingSticky = true;
            break;
        }
    }
    EXPECT_TRUE(foundSurvivingSticky)
        << "Sticky-note annotation at a non-overlapping region was "
        << "destroyed by applyRedactions. Annotations count after: "
        << annsAfter.size() << ". A successful redaction must NOT "
        << "nuke unrelated annotations on the same page.";
}

// ─── B6: OCR text layer redaction ───────────────────────────────────
// OCR'd scanned PDFs carry an invisible text layer (Tr=3 hidden Tj
// operators) used for search/copy. A redaction over an OCR text
// region MUST strip the hidden text bytes from the saved file —
// otherwise a user who redacts visually but believes the text is
// gone will have a false sense of security when the file ships
// to a recipient who runs pdftotext on it.

TEST_F(RedactionSecurityTest, OcrTextLayerRedaction) {
    auto ocrPath = std::filesystem::path(TEST_FIXTURES_DIR) / "test_ocr_text.pdf";
    if (!std::filesystem::exists(ocrPath)) {
        GTEST_SKIP() << "test_ocr_text.pdf not found";
    }
    auto doc = pdfe::core::Document::open(context_, ocrPath);
    ASSERT_NE(doc, nullptr);

    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    // Stage 1: extract text BEFORE redaction. The fixture must
    // contain extractable text (visible or hidden OCR layer);
    // otherwise B6 cannot prove anything.
    std::string textBefore = page->extractPlainText();
    if (textBefore.empty()) {
        GTEST_SKIP() << "OCR fixture has no extractable text — B6 "
                     << "cannot verify byte-level absence contract";
    }

    // Pick a distinctive word from the text-before that we can
    // search for as the redaction target. We can't assume any
    // specific word, so take the first non-trivial whitespace
    // token (>=4 chars to avoid false positives like "the").
    std::string target;
    {
        std::string cur;
        for (char c : textBefore) {
            if (std::isalnum(static_cast<unsigned char>(c))) {
                cur.push_back(c);
            } else {
                if (cur.size() >= 4) {
                    target = cur;
                    break;
                }
                cur.clear();
            }
        }
        if (target.empty() && cur.size() >= 4) {
            target = cur;
        }
    }
    if (target.empty()) {
        GTEST_SKIP() << "could not find a >=4-char alphanumeric "
                     << "token in OCR text to use as redaction target";
    }
    ASSERT_NE(textBefore.find(target), std::string::npos)
        << "sanity: target token must be in pre-redaction text";

    // Stage 2: cover the entire page with a redaction. This
    // guarantees the OCR text layer (wherever it is in the page
    // coordinate space) is hit. Coordinate-system independence
    // for the test, byte-level absence is the contract under test.
    QRectF pageRect = page->boundingBox();
    pdfe::redaction::RedactionManager mgr(context_);
    auto mark = mgr.createRedactionMark(page.get(), pageRect);
    ASSERT_NE(mark.annotIndex, -1);
    page.reset();

    // Stage 3: apply redactions. textMethod=Remove (default)
    // means hidden text Tj operators in the page content stream
    // are filtered out by pdf_filter_page_contents.
    pdfe::redaction::RedactionApplyOptions opts;
    opts.rewriteDocument = false;
    opts.textMethod = pdfe::redaction::RedactionTextMethod::Remove;
    auto applyResult = mgr.applyAllRedactions(doc.get(), opts);
    ASSERT_TRUE(applyResult.success) << applyResult.errorMessage;

    // Stage 4: save and reopen for byte-level verification.
    auto out = tmpFile("ocr_redacted");
    doc->saveAs(out);
    doc.reset();

    auto reopened = pdfe::core::Document::open(context_, out);
    ASSERT_NE(reopened, nullptr);
    auto reopenedPage = reopened->page(0);
    ASSERT_NE(reopenedPage, nullptr);

    std::string textAfter = reopenedPage->extractPlainText();

    // The target word MUST be absent from the post-redaction text
    // extraction. This is the byte-level absence contract: the OCR
    // text layer's hidden Tj operators were stripped from the
    // page content stream.
    EXPECT_EQ(textAfter.find(target), std::string::npos)
        << "OCR text layer redaction leaked: target token '"
        << target << "' is still present in extractPlainText() "
        << "output AFTER apply+save+reopen. Security regression: "
        << "hidden OCR Tj operators not stripped by "
        << "RedactionTextMethod::Remove.";
}
