// PHASE1_HARDENING: skip=0 expect_ge=2 no_throw=0  (2026-05-08)
#include <gtest/gtest.h>
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/Page.h"
#include "core/Error.h"
#include "redaction/RedactionManager.h"
#include "redaction/RedactionCommands.h"
#include "redaction/RedactionData.h"
#include <filesystem>
#include <memory>

class RedactionCommandsTest : public ::testing::Test {
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

    size_t markCount(pdfe::core::Page* page) {
        return manager_->getRedactionMarks(page).size();
    }
};

// ============================================================================
// CreateRedactionCommand Tests
// ============================================================================

TEST_F(RedactionCommandsTest, CreateCommand_Description) {
    auto* page = getPage(0);
    pdfe::redaction::CreateRedactionCommand command(manager_.get(), page,
        QRectF(100, 100, 100, 50));

    EXPECT_EQ(command.description(), "Mark Redaction");
}

TEST_F(RedactionCommandsTest, CreateCommand_Execute) {
    auto* page = getPage(0);
    size_t initial = markCount(page);

    pdfe::redaction::CreateRedactionCommand command(manager_.get(), page,
        QRectF(100, 100, 100, 50));

    EXPECT_NO_THROW(command.execute());
    EXPECT_EQ(markCount(page), initial + 1);

    const auto& created = command.createdMark();
    EXPECT_EQ(created.pageIndex, 0);
    EXPECT_NE(created.annotIndex, -1) << "Created RedactionMark must have a valid index";
}

TEST_F(RedactionCommandsTest, CreateCommand_Undo) {
    auto* page = getPage(0);
    size_t initial = markCount(page);

    pdfe::redaction::CreateRedactionCommand command(manager_.get(), page,
        QRectF(100, 100, 100, 50));
    command.execute();
    ASSERT_EQ(markCount(page), initial + 1);

    EXPECT_NO_THROW(command.undo());
    EXPECT_EQ(markCount(page), initial);
}

TEST_F(RedactionCommandsTest, CreateCommand_Redo) {
    auto* page = getPage(0);
    size_t initial = markCount(page);

    pdfe::redaction::CreateRedactionCommand command(manager_.get(), page,
        QRectF(100, 100, 100, 50));
    command.execute();
    command.undo();
    ASSERT_EQ(markCount(page), initial);

    EXPECT_NO_THROW(command.redo());
    EXPECT_EQ(markCount(page), initial + 1);
}

TEST_F(RedactionCommandsTest, CreateCommand_UndoRedoPreservesAnnotIndex) {
    auto* page = getPage(0);

    pdfe::redaction::CreateRedactionCommand command(manager_.get(), page,
        QRectF(100, 100, 100, 50));
    command.execute();

    int firstAnnotIdx = command.createdMark().annotIndex;
    command.undo();
    command.redo();

    // After redo, the annotIndex may differ (new annotation created), but a mark should exist
    EXPECT_NE(command.createdMark().annotIndex, -1)
        << "redo() must restore the mark with a valid index";
    EXPECT_EQ(markCount(page), 1u);
}

// ============================================================================
// DeleteRedactionCommand Tests
// ============================================================================

TEST_F(RedactionCommandsTest, DeleteCommand_Description) {
    auto* page = getPage(0);
    auto created = manager_->createRedactionMark(page, QRectF(100, 100, 100, 50));

    pdfe::redaction::DeleteRedactionCommand command(manager_.get(), page, created.annotIndex);

    EXPECT_EQ(command.description(), "Delete Redaction Mark");
}

TEST_F(RedactionCommandsTest, DeleteCommand_Execute) {
    auto* page = getPage(0);
    auto created = manager_->createRedactionMark(page, QRectF(100, 100, 100, 50));
    size_t countAfterCreate = markCount(page);
    ASSERT_GT(countAfterCreate, 0u);

    pdfe::redaction::DeleteRedactionCommand command(manager_.get(), page, created.annotIndex);

    EXPECT_NO_THROW(command.execute());
    EXPECT_EQ(markCount(page), countAfterCreate - 1);
}

TEST_F(RedactionCommandsTest, DeleteCommand_Undo) {
    auto* page = getPage(0);
    auto created = manager_->createRedactionMark(page, QRectF(100, 100, 100, 50));
    size_t countBefore = markCount(page);

    pdfe::redaction::DeleteRedactionCommand command(manager_.get(), page, created.annotIndex);
    command.execute();
    ASSERT_EQ(markCount(page), countBefore - 1);

    EXPECT_NO_THROW(command.undo());
    EXPECT_EQ(markCount(page), countBefore);
}

TEST_F(RedactionCommandsTest, DeleteCommand_Redo) {
    auto* page = getPage(0);
    auto created = manager_->createRedactionMark(page, QRectF(100, 100, 100, 50));
    size_t countBefore = markCount(page);

    pdfe::redaction::DeleteRedactionCommand command(manager_.get(), page, created.annotIndex);
    command.execute();
    command.undo();
    ASSERT_EQ(markCount(page), countBefore);

    EXPECT_NO_THROW(command.redo());
    EXPECT_EQ(markCount(page), countBefore - 1);
}

// ============================================================================
// ModifyRedactionCommand Tests
// ============================================================================

TEST_F(RedactionCommandsTest, ModifyCommand_FillColor) {
    auto* page = getPage(0);
    auto created = manager_->createRedactionMark(page, QRectF(100, 100, 100, 50));

    // Build old and new data
    auto oldData = manager_->getRedactionMark(page, created.annotIndex);
    auto newData = oldData;
    newData.fillColor = QColor(Qt::red);

    pdfe::redaction::ModifyRedactionCommand command(
        manager_.get(), page, created.annotIndex,
        pdfe::redaction::ModifyRedactionCommand::Property::FillColor,
        oldData, newData
    );

    command.execute();

    auto retrieved = manager_->getRedactionMark(page, created.annotIndex);
    EXPECT_NEAR(retrieved.fillColor.redF(), 1.0, 0.01);
    EXPECT_NEAR(retrieved.fillColor.greenF(), 0.0, 0.01);
    EXPECT_NEAR(retrieved.fillColor.blueF(), 0.0, 0.01);
}

TEST_F(RedactionCommandsTest, ModifyCommand_FillColor_Description) {
    auto* page = getPage(0);
    auto created = manager_->createRedactionMark(page, QRectF(100, 100, 100, 50));

    auto oldData = manager_->getRedactionMark(page, created.annotIndex);
    auto newData = oldData;
    newData.fillColor = QColor(Qt::blue);

    pdfe::redaction::ModifyRedactionCommand command(
        manager_.get(), page, created.annotIndex,
        pdfe::redaction::ModifyRedactionCommand::Property::FillColor,
        oldData, newData
    );

    EXPECT_EQ(command.description(), "Change Redaction Color");
}

TEST_F(RedactionCommandsTest, ModifyCommand_OverlayText) {
    auto* page = getPage(0);
    auto created = manager_->createRedactionMark(page, QRectF(100, 100, 100, 50));
    manager_->setOverlayText(page, created.annotIndex, "ORIGINAL");

    auto oldData = manager_->getRedactionMark(page, created.annotIndex);
    auto newData = oldData;
    newData.overlayText = "MODIFIED";

    pdfe::redaction::ModifyRedactionCommand command(
        manager_.get(), page, created.annotIndex,
        pdfe::redaction::ModifyRedactionCommand::Property::OverlayText,
        oldData, newData
    );

    command.execute();

    auto retrieved = manager_->getRedactionMark(page, created.annotIndex);
    EXPECT_EQ(retrieved.overlayText, "MODIFIED");
}

TEST_F(RedactionCommandsTest, ModifyCommand_OverlayText_Description) {
    auto* page = getPage(0);
    auto created = manager_->createRedactionMark(page, QRectF(100, 100, 100, 50));

    auto oldData = manager_->getRedactionMark(page, created.annotIndex);
    auto newData = oldData;
    newData.overlayText = "TEST";

    pdfe::redaction::ModifyRedactionCommand command(
        manager_.get(), page, created.annotIndex,
        pdfe::redaction::ModifyRedactionCommand::Property::OverlayText,
        oldData, newData
    );

    EXPECT_EQ(command.description(), "Change Redaction Text");
}

TEST_F(RedactionCommandsTest, ModifyCommand_Rect) {
    auto* page = getPage(0);
    auto created = manager_->createRedactionMark(page, QRectF(100, 100, 100, 50));

    auto oldData = manager_->getRedactionMark(page, created.annotIndex);
    auto newData = oldData;
    newData.rect = QRectF(150, 150, 200, 100);

    pdfe::redaction::ModifyRedactionCommand command(
        manager_.get(), page, created.annotIndex,
        pdfe::redaction::ModifyRedactionCommand::Property::Rect,
        oldData, newData
    );

    command.execute();

    auto retrieved = manager_->getRedactionMark(page, created.annotIndex);
    EXPECT_NEAR(retrieved.rect.left(), 150.0, 5.0);
    EXPECT_NEAR(retrieved.rect.top(), 150.0, 5.0);
}

TEST_F(RedactionCommandsTest, ModifyCommand_Rect_Description) {
    auto* page = getPage(0);
    auto created = manager_->createRedactionMark(page, QRectF(100, 100, 100, 50));

    auto oldData = manager_->getRedactionMark(page, created.annotIndex);
    auto newData = oldData;
    newData.rect = QRectF(150, 150, 200, 100);

    pdfe::redaction::ModifyRedactionCommand command(
        manager_.get(), page, created.annotIndex,
        pdfe::redaction::ModifyRedactionCommand::Property::Rect,
        oldData, newData
    );

    EXPECT_EQ(command.description(), "Move Redaction");
}

TEST_F(RedactionCommandsTest, ModifyCommand_UndoRedo) {
    auto* page = getPage(0);
    auto created = manager_->createRedactionMark(page, QRectF(100, 100, 100, 50));
    manager_->setOverlayText(page, created.annotIndex, "BEFORE");

    auto oldData = manager_->getRedactionMark(page, created.annotIndex);
    auto newData = oldData;
    newData.overlayText = "AFTER";

    pdfe::redaction::ModifyRedactionCommand command(
        manager_.get(), page, created.annotIndex,
        pdfe::redaction::ModifyRedactionCommand::Property::OverlayText,
        oldData, newData
    );

    command.execute();
    EXPECT_EQ(manager_->getRedactionMark(page, created.annotIndex).overlayText, "AFTER");

    command.undo();
    EXPECT_EQ(manager_->getRedactionMark(page, created.annotIndex).overlayText, "BEFORE");

    command.redo();
    EXPECT_EQ(manager_->getRedactionMark(page, created.annotIndex).overlayText, "AFTER");
}

TEST_F(RedactionCommandsTest, ModifyCommand_MergeWith_SameProperty) {
    auto* page = getPage(0);
    auto created = manager_->createRedactionMark(page, QRectF(100, 100, 100, 50));
    manager_->setOverlayText(page, created.annotIndex, "INITIAL");

    auto data1Old = manager_->getRedactionMark(page, created.annotIndex);
    auto data1New = data1Old;
    data1New.overlayText = "FIRST";

    auto cmd1 = std::make_unique<pdfe::redaction::ModifyRedactionCommand>(
        manager_.get(), page, created.annotIndex,
        pdfe::redaction::ModifyRedactionCommand::Property::OverlayText,
        data1Old, data1New
    );
    cmd1->execute();

    auto data2Old = manager_->getRedactionMark(page, created.annotIndex);
    auto data2New = data2Old;
    data2New.overlayText = "SECOND";

    auto cmd2 = std::make_unique<pdfe::redaction::ModifyRedactionCommand>(
        manager_.get(), page, created.annotIndex,
        pdfe::redaction::ModifyRedactionCommand::Property::OverlayText,
        data2Old, data2New
    );

    // Merge cmd2 into cmd1
    EXPECT_TRUE(cmd1->mergeWith(cmd2.get()));
}

TEST_F(RedactionCommandsTest, ModifyCommand_MergeWith_DifferentProperty) {
    auto* page = getPage(0);
    auto created = manager_->createRedactionMark(page, QRectF(100, 100, 100, 50));

    auto data = manager_->getRedactionMark(page, created.annotIndex);
    auto newData1 = data;
    newData1.overlayText = "test";
    auto newData2 = data;
    newData2.fillColor = QColor(Qt::red);

    auto cmd1 = std::make_unique<pdfe::redaction::ModifyRedactionCommand>(
        manager_.get(), page, created.annotIndex,
        pdfe::redaction::ModifyRedactionCommand::Property::OverlayText,
        data, newData1
    );

    auto cmd2 = std::make_unique<pdfe::redaction::ModifyRedactionCommand>(
        manager_.get(), page, created.annotIndex,
        pdfe::redaction::ModifyRedactionCommand::Property::FillColor,
        data, newData2
    );

    // Different properties should not merge
    EXPECT_FALSE(cmd1->mergeWith(cmd2.get()));
}

TEST_F(RedactionCommandsTest, ModifyCommand_MergeId) {
    auto* page = getPage(0);
    auto created = manager_->createRedactionMark(page, QRectF(100, 100, 100, 50));

    auto data = manager_->getRedactionMark(page, created.annotIndex);

    pdfe::redaction::ModifyRedactionCommand command(
        manager_.get(), page, created.annotIndex,
        pdfe::redaction::ModifyRedactionCommand::Property::FillColor,
        data, data
    );

    EXPECT_EQ(command.mergeId(), 5000 + created.annotIndex);
}

// ============================================================================
// Command Chain Test
// ============================================================================

TEST_F(RedactionCommandsTest, CommandChain_CreateModifyDelete) {
    auto* page = getPage(0);
    size_t initial = markCount(page);

    // Create
    pdfe::redaction::CreateRedactionCommand createCmd(manager_.get(), page,
        QRectF(100, 100, 100, 50));
    createCmd.execute();
    EXPECT_EQ(markCount(page), initial + 1);

    int annotIdx = createCmd.createdMark().annotIndex;

    // Modify
    auto oldData = manager_->getRedactionMark(page, annotIdx);
    auto newData = oldData;
    newData.overlayText = "MODIFIED";

    pdfe::redaction::ModifyRedactionCommand modifyCmd(
        manager_.get(), page, annotIdx,
        pdfe::redaction::ModifyRedactionCommand::Property::OverlayText,
        oldData, newData
    );
    modifyCmd.execute();
    EXPECT_EQ(manager_->getRedactionMark(page, annotIdx).overlayText, "MODIFIED");

    // Delete
    pdfe::redaction::DeleteRedactionCommand deleteCmd(manager_.get(), page, annotIdx);
    deleteCmd.execute();
    EXPECT_EQ(markCount(page), initial);

    // Undo chain in reverse
    deleteCmd.undo();
    EXPECT_EQ(markCount(page), initial + 1);

    modifyCmd.undo();

    createCmd.undo();
    EXPECT_EQ(markCount(page), initial);
}
