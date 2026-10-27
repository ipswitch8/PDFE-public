#include <gtest/gtest.h>
#include "annotations/AnnotationCommands.h"
#include "annotations/AnnotationManager.h"
#include "core/Document.h"
#include "core/Page.h"
#include "core/MuPdfContext.h"
#include "core/UndoManager.h"
#include <filesystem>
#include <memory>

class AnnotationCommandTest : public ::testing::Test {
protected:
    void SetUp() override {
        context = std::make_shared<pdfe::core::MuPdfContext>();
        std::filesystem::path fixturesDir = TEST_FIXTURES_DIR;
        testPdf = fixturesDir / "test_3page.pdf";

        if (std::filesystem::exists(testPdf)) {
            document = pdfe::core::Document::open(context, testPdf);
            manager = std::make_unique<pdfe::annotations::AnnotationManager>(context);
            undoManager = std::make_unique<pdfe::commands::UndoManager>();
        }
    }

    void TearDown() override {
        undoManager.reset();
        manager.reset();
        document.reset();
        context.reset();
    }

    std::shared_ptr<pdfe::core::MuPdfContext> context;
    std::filesystem::path testPdf;
    std::unique_ptr<pdfe::core::Document> document;
    std::unique_ptr<pdfe::annotations::AnnotationManager> manager;
    std::unique_ptr<pdfe::commands::UndoManager> undoManager;
};

TEST_F(AnnotationCommandTest, CreateAndUndo) {
    ASSERT_NE(document, nullptr) << "Failed to open test PDF";
    ASSERT_GT(document->pageCount(), 0);

    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    // Create annotation via command
    QRectF rect(100, 100, 200, 50);
    auto cmd = std::make_unique<pdfe::annotations::CreateAnnotationCommand>(
        manager.get(),
        page.get(),
        pdfe::annotations::AnnotationType::Text,
        rect
    );

    // Execute
    auto* cmdPtr = cmd.get();
    undoManager->execute(std::move(cmd));

    // Verify annotation exists
    auto annots = manager->getAnnotations(page.get());
    ASSERT_GT(annots.size(), 0);
    EXPECT_EQ(annots.back().type, pdfe::annotations::AnnotationType::Text);

    int annotIndexAfterCreate = cmdPtr->createdAnnotation().annotIndex;

    // Undo
    ASSERT_TRUE(undoManager->canUndo());
    undoManager->undo();

    // Verify annotation is gone (we should try to get it and handle the case properly)
    // The annotation should be deleted, so the count should be back to original
    auto annotsAfterUndo = manager->getAnnotations(page.get());
    EXPECT_EQ(annotsAfterUndo.size(), annots.size() - 1);

    // Redo
    ASSERT_TRUE(undoManager->canRedo());
    undoManager->redo();

    // Verify annotation is back
    auto annotsAfterRedo = manager->getAnnotations(page.get());
    EXPECT_EQ(annotsAfterRedo.size(), annots.size());
}

TEST_F(AnnotationCommandTest, DeleteAndUndo) {
    ASSERT_NE(document, nullptr) << "Failed to open test PDF";
    ASSERT_GT(document->pageCount(), 0);

    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    // First create an annotation
    QRectF rect(100, 100, 200, 50);
    auto createdData = manager->createAnnotation(
        page.get(),
        pdfe::annotations::AnnotationType::Highlight,
        rect
    );

    int annotIndex = createdData.annotIndex;
    auto annots = manager->getAnnotations(page.get());
    size_t countBeforeDelete = annots.size();

    // Delete via command
    auto cmd = std::make_unique<pdfe::annotations::DeleteAnnotationCommand>(
        manager.get(),
        page.get(),
        annotIndex
    );
    undoManager->execute(std::move(cmd));

    // Verify annotation is deleted
    auto annotsAfterDelete = manager->getAnnotations(page.get());
    EXPECT_EQ(annotsAfterDelete.size(), countBeforeDelete - 1);

    // Undo deletion
    ASSERT_TRUE(undoManager->canUndo());
    undoManager->undo();

    // Verify annotation is restored
    auto annotsAfterUndo = manager->getAnnotations(page.get());
    EXPECT_EQ(annotsAfterUndo.size(), countBeforeDelete);

    // Verify the restored annotation has the same properties
    auto restoredAnnot = annotsAfterUndo.back();
    EXPECT_EQ(restoredAnnot.type, pdfe::annotations::AnnotationType::Highlight);
}

TEST_F(AnnotationCommandTest, ModifyContentsAndUndo) {
    ASSERT_NE(document, nullptr) << "Failed to open test PDF";
    ASSERT_GT(document->pageCount(), 0);

    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    // Create an annotation
    QRectF rect(100, 100, 200, 50);
    auto createdData = manager->createAnnotation(
        page.get(),
        pdfe::annotations::AnnotationType::Text,
        rect
    );

    int annotIndex = createdData.annotIndex;

    // Get old data
    auto oldData = manager->getAnnotation(page.get(), annotIndex);
    EXPECT_TRUE(oldData.contents.empty());

    // Prepare new data
    auto newData = oldData;
    newData.contents = "Test note content";

    // Modify via command
    auto cmd = std::make_unique<pdfe::annotations::ModifyAnnotationCommand>(
        manager.get(),
        page.get(),
        annotIndex,
        pdfe::annotations::ModifyAnnotationCommand::Property::Contents,
        oldData,
        newData
    );
    undoManager->execute(std::move(cmd));

    // Verify contents changed
    auto modifiedData = manager->getAnnotation(page.get(), annotIndex);
    EXPECT_EQ(modifiedData.contents, "Test note content");

    // Undo
    ASSERT_TRUE(undoManager->canUndo());
    undoManager->undo();

    // Verify contents reverted
    auto revertedData = manager->getAnnotation(page.get(), annotIndex);
    EXPECT_TRUE(revertedData.contents.empty());

    // Redo
    ASSERT_TRUE(undoManager->canRedo());
    undoManager->redo();

    // Verify contents changed again
    auto redoneData = manager->getAnnotation(page.get(), annotIndex);
    EXPECT_EQ(redoneData.contents, "Test note content");
}

TEST_F(AnnotationCommandTest, ModifyColorAndUndo) {
    ASSERT_NE(document, nullptr) << "Failed to open test PDF";
    ASSERT_GT(document->pageCount(), 0);

    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    // Create an annotation
    QRectF rect(100, 100, 200, 50);
    auto createdData = manager->createAnnotation(
        page.get(),
        pdfe::annotations::AnnotationType::Highlight,
        rect
    );

    int annotIndex = createdData.annotIndex;

    // Get old data
    auto oldData = manager->getAnnotation(page.get(), annotIndex);
    QColor oldColor = oldData.color;

    // Prepare new data with different color
    auto newData = oldData;
    newData.color = QColor(0, 255, 0); // Green

    // Modify via command
    auto cmd = std::make_unique<pdfe::annotations::ModifyAnnotationCommand>(
        manager.get(),
        page.get(),
        annotIndex,
        pdfe::annotations::ModifyAnnotationCommand::Property::Color,
        oldData,
        newData
    );
    undoManager->execute(std::move(cmd));

    // Verify color changed
    auto modifiedData = manager->getAnnotation(page.get(), annotIndex);
    EXPECT_EQ(modifiedData.color, QColor(0, 255, 0));

    // Undo
    ASSERT_TRUE(undoManager->canUndo());
    undoManager->undo();

    // Verify color reverted
    auto revertedData = manager->getAnnotation(page.get(), annotIndex);
    EXPECT_EQ(revertedData.color, oldColor);
}

TEST_F(AnnotationCommandTest, MultipleUndoRedo) {
    ASSERT_NE(document, nullptr) << "Failed to open test PDF";
    ASSERT_GT(document->pageCount(), 0);

    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    size_t initialCount = manager->getAnnotations(page.get()).size();

    // Create 3 annotations via commands
    for (int i = 0; i < 3; ++i) {
        QRectF rect(100 + i * 50, 100, 200, 50);
        auto cmd = std::make_unique<pdfe::annotations::CreateAnnotationCommand>(
            manager.get(),
            page.get(),
            pdfe::annotations::AnnotationType::Text,
            rect
        );
        undoManager->execute(std::move(cmd));
    }

    // Verify 3 annotations created
    auto annots = manager->getAnnotations(page.get());
    EXPECT_EQ(annots.size(), initialCount + 3);

    // Undo all 3
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(undoManager->canUndo());
        undoManager->undo();
    }

    // Verify all undone
    auto annotsAfterUndo = manager->getAnnotations(page.get());
    EXPECT_EQ(annotsAfterUndo.size(), initialCount);

    // Redo all 3
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(undoManager->canRedo());
        undoManager->redo();
    }

    // Verify all redone
    auto annotsAfterRedo = manager->getAnnotations(page.get());
    EXPECT_EQ(annotsAfterRedo.size(), initialCount + 3);
}

TEST_F(AnnotationCommandTest, MergeConsecutiveContentEdits) {
    ASSERT_NE(document, nullptr) << "Failed to open test PDF";
    ASSERT_GT(document->pageCount(), 0);

    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    // Create an annotation
    QRectF rect(100, 100, 200, 50);
    auto createdData = manager->createAnnotation(
        page.get(),
        pdfe::annotations::AnnotationType::Text,
        rect
    );

    int annotIndex = createdData.annotIndex;

    // Execute 3 consecutive content modifications
    auto data0 = manager->getAnnotation(page.get(), annotIndex);

    auto data1 = data0;
    data1.contents = "H";
    auto cmd1 = std::make_unique<pdfe::annotations::ModifyAnnotationCommand>(
        manager.get(),
        page.get(),
        annotIndex,
        pdfe::annotations::ModifyAnnotationCommand::Property::Contents,
        data0,
        data1
    );
    undoManager->execute(std::move(cmd1));

    auto data2 = data1;
    data2.contents = "He";
    auto cmd2 = std::make_unique<pdfe::annotations::ModifyAnnotationCommand>(
        manager.get(),
        page.get(),
        annotIndex,
        pdfe::annotations::ModifyAnnotationCommand::Property::Contents,
        data1,
        data2
    );
    undoManager->execute(std::move(cmd2));

    auto data3 = data2;
    data3.contents = "Hello";
    auto cmd3 = std::make_unique<pdfe::annotations::ModifyAnnotationCommand>(
        manager.get(),
        page.get(),
        annotIndex,
        pdfe::annotations::ModifyAnnotationCommand::Property::Contents,
        data2,
        data3
    );
    undoManager->execute(std::move(cmd3));

    // Verify final content
    auto finalData = manager->getAnnotation(page.get(), annotIndex);
    EXPECT_EQ(finalData.contents, "Hello");

    // Should be able to undo once (all three merged into one)
    ASSERT_TRUE(undoManager->canUndo());
    undoManager->undo();

    // Should revert to original empty content
    auto revertedData = manager->getAnnotation(page.get(), annotIndex);
    EXPECT_TRUE(revertedData.contents.empty());

    // Creation was done directly (not via undo manager), so no more undo steps
    EXPECT_FALSE(undoManager->canUndo());
}

// ============================================================================
// Phase 3 Cluster #5 merge-by-key-tuple regression tests
// ----------------------------------------------------------------------------
// These three tests lock in the fix in commit deb46a0 / src/annotations/
// AnnotationCommands.cpp where ModifyAnnotationCommand::mergeId() and
// mergeWith() identify the target annotation by
// (newData_.pageIndex, annotIndex_, property_) instead of the raw Page*
// pointer. The raw pointer differs across consecutive edits because
// MainWindow::onAnnotationPropertiesChanged calls Document::page(idx) on
// every edit and that returns a fresh std::unique_ptr<Page> wrapper each
// time, so the previous pointer-equality check never coalesced typing.
// ============================================================================

TEST_F(AnnotationCommandTest, ModifyAnnotationMergeIdConsistentForSameKey) {
    ASSERT_NE(document, nullptr) << "Failed to open test PDF";
    ASSERT_GT(document->pageCount(), 0);

    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    QRectF rect(100, 100, 200, 50);
    auto createdData = manager->createAnnotation(
        page.get(),
        pdfe::annotations::AnnotationType::Text,
        rect
    );
    int annotIndex = createdData.annotIndex;

    auto data0 = manager->getAnnotation(page.get(), annotIndex);
    ASSERT_GE(data0.pageIndex, 0)
        << "createAnnotation must populate pageIndex; the mergeId hash "
           "relies on it being stable across consecutive edits.";

    auto dataA = data0;
    dataA.contents = "A";
    pdfe::annotations::ModifyAnnotationCommand cmdA(
        manager.get(), page.get(), annotIndex,
        pdfe::annotations::ModifyAnnotationCommand::Property::Contents,
        data0, dataA
    );

    auto dataB = dataA;
    dataB.contents = "AB";
    pdfe::annotations::ModifyAnnotationCommand cmdB(
        manager.get(), page.get(), annotIndex,
        pdfe::annotations::ModifyAnnotationCommand::Property::Contents,
        dataA, dataB
    );

    // Both commands target the same (pageIndex, annotIndex, property) tuple,
    // so mergeId must agree -- otherwise consecutive typing never coalesces.
    EXPECT_EQ(cmdA.mergeId(), cmdB.mergeId());
    // Contents edits get a non-zero mergeId (mergeId == 0 means "do not merge").
    EXPECT_NE(0, cmdA.mergeId());
}

TEST_F(AnnotationCommandTest, ModifyAnnotationMergeIdDiffersAcrossProperties) {
    ASSERT_NE(document, nullptr) << "Failed to open test PDF";
    ASSERT_GT(document->pageCount(), 0);

    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    QRectF rect(100, 100, 200, 50);
    auto createdData = manager->createAnnotation(
        page.get(),
        pdfe::annotations::AnnotationType::Text,
        rect
    );
    int annotIndex = createdData.annotIndex;
    auto data0 = manager->getAnnotation(page.get(), annotIndex);

    auto dataContents = data0;
    dataContents.contents = "Hello";
    pdfe::annotations::ModifyAnnotationCommand contentsCmd(
        manager.get(), page.get(), annotIndex,
        pdfe::annotations::ModifyAnnotationCommand::Property::Contents,
        data0, dataContents
    );

    auto dataColor = data0;
    dataColor.color = QColor(0, 255, 0);
    pdfe::annotations::ModifyAnnotationCommand colorCmd(
        manager.get(), page.get(), annotIndex,
        pdfe::annotations::ModifyAnnotationCommand::Property::Color,
        data0, dataColor
    );

    // Only Contents edits opt into merging (mergeId() returns 0 for any
    // non-Contents property). So Color must NOT share an id with Contents.
    EXPECT_NE(contentsCmd.mergeId(), colorCmd.mergeId());
    EXPECT_EQ(0, colorCmd.mergeId());

    // And mergeWith() must refuse to combine across properties, so an
    // Undo of a Color edit cannot accidentally absorb a Contents edit.
    EXPECT_FALSE(contentsCmd.mergeWith(&colorCmd));
}

TEST_F(AnnotationCommandTest, ModifyAnnotationMergeWithAbsorbsNewData) {
    ASSERT_NE(document, nullptr) << "Failed to open test PDF";
    ASSERT_GT(document->pageCount(), 0);

    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    QRectF rect(100, 100, 200, 50);
    auto createdData = manager->createAnnotation(
        page.get(),
        pdfe::annotations::AnnotationType::Text,
        rect
    );
    int annotIndex = createdData.annotIndex;

    auto data0 = manager->getAnnotation(page.get(), annotIndex);
    auto dataA = data0;
    dataA.contents = "Hel";
    auto dataB = dataA;
    dataB.contents = "Hello";

    pdfe::annotations::ModifyAnnotationCommand cmdA(
        manager.get(), page.get(), annotIndex,
        pdfe::annotations::ModifyAnnotationCommand::Property::Contents,
        data0, dataA
    );

    pdfe::annotations::ModifyAnnotationCommand cmdB(
        manager.get(), page.get(), annotIndex,
        pdfe::annotations::ModifyAnnotationCommand::Property::Contents,
        dataA, dataB
    );

    // Execute first; mergeWith must succeed for same-key Contents edits.
    cmdA.execute();
    cmdB.execute();
    ASSERT_TRUE(cmdA.mergeWith(&cmdB));

    // After merging, undoing cmdA must revert to the original (empty)
    // contents -- proving cmdA still owns the original oldData_, while
    // newData_ has been absorbed from cmdB. This is what lets the undo
    // stack collapse N typed characters into a single Undo step.
    cmdA.undo();
    auto reverted = manager->getAnnotation(page.get(), annotIndex);
    EXPECT_TRUE(reverted.contents.empty());

    // Redo must now apply cmdB's absorbed newData_, not cmdA's original "Hel".
    cmdA.redo();
    auto redone = manager->getAnnotation(page.get(), annotIndex);
    EXPECT_EQ(redone.contents, "Hello");
}

TEST_F(AnnotationCommandTest, CleanStateDirtyAfterEdit) {
    ASSERT_NE(document, nullptr) << "Failed to open test PDF";
    ASSERT_GT(document->pageCount(), 0);

    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    // Set clean state
    undoManager->setClean();
    EXPECT_TRUE(undoManager->isClean());

    // Create annotation
    QRectF rect(100, 100, 200, 50);
    auto cmd = std::make_unique<pdfe::annotations::CreateAnnotationCommand>(
        manager.get(),
        page.get(),
        pdfe::annotations::AnnotationType::Text,
        rect
    );
    undoManager->execute(std::move(cmd));

    // Should be dirty now
    EXPECT_FALSE(undoManager->isClean());

    // Undo should return to clean state
    undoManager->undo();
    EXPECT_TRUE(undoManager->isClean());
}
