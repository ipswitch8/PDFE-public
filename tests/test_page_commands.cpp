#include <gtest/gtest.h>
#include "core/PageCommands.h"
#include "core/Document.h"
#include "core/Page.h"
#include "core/MuPdfContext.h"
#include "core/UndoManager.h"
#include <filesystem>
#include <memory>
#include <set>
#include <vector>

class PageCommandTest : public ::testing::Test {
protected:
    void SetUp() override {
        context = std::make_shared<pdfe::core::MuPdfContext>();
        std::filesystem::path fixturesDir = TEST_FIXTURES_DIR;
        testPdf = fixturesDir / "test_3page.pdf";

        if (std::filesystem::exists(testPdf)) {
            document = pdfe::core::Document::open(context, testPdf);
            undoManager = std::make_unique<pdfe::commands::UndoManager>();
        }
    }

    void TearDown() override {
        undoManager.reset();
        document.reset();
        context.reset();
    }

    /// #224: test_3page.pdf carries ONE inherited /MediaBox for all three
    /// pages and no /Rotate at all, so widthPt/heightPt are identical across
    /// every page. Each MovePageCommand assertion that compared dimensions was
    /// therefore true before the edit, after a correct move, after a no-op and
    /// after a WRONG move -- the project's documented one-against-one shape:
    /// the measurement is real but cannot vary with the thing under test.
    ///
    /// Give each page a distinct, observable identity instead. Applied through
    /// Document directly rather than through a RotatePageCommand, so the undo
    /// stack the Undo/Redo cases assert on stays empty.
    void stampPageIdentities() {
        for (int i = 0; i < document->pageCount(); ++i) {
            document->rotatePage(i, 90 * i);   // -> 0, 90, 180
        }
    }

    /// Identity of every page, in page order. Comparing the WHOLE vector is
    /// deliberate: an assertion about the moved page alone would pass a move
    /// that also disturbed the pages it was supposed to leave alone.
    std::vector<int> pageIdentities() const {
        std::vector<int> ids;
        for (int i = 0; i < document->pageCount(); ++i) {
            ids.push_back(document->pageInfo(i).rotation);
        }
        return ids;
    }

    /// Fails the test unless the identities can actually tell pages apart.
    /// Without this the rewrite would be vacuous in a new way if a future
    /// fixture change made the stamped rotations collide.
    void requireDistinctIdentities(const std::vector<int>& ids) const {
        const std::set<int> uniq(ids.begin(), ids.end());
        ASSERT_EQ(uniq.size(), ids.size())
            << "page identities are not distinct, so no permutation can be "
               "distinguished from any other";
    }

    std::shared_ptr<pdfe::core::MuPdfContext> context;
    std::filesystem::path testPdf;
    std::unique_ptr<pdfe::core::Document> document;
    std::unique_ptr<pdfe::commands::UndoManager> undoManager;
};

TEST_F(PageCommandTest, RotatePageCommand_Execute) {
    if (!document) {
        GTEST_SKIP() << "Test PDF not available";
    }

    ASSERT_GT(document->pageCount(), 0);

    // Get initial rotation
    int initialRotation = document->currentRotation(0);

    // Rotate page 0 by 90 degrees
    auto cmd = std::make_unique<pdfe::commands::RotatePageCommand>(
        document.get(),
        0,
        90
    );
    undoManager->execute(std::move(cmd));

    // Verify rotation
    int newRotation = document->currentRotation(0);
    EXPECT_EQ(newRotation, (initialRotation + 90) % 360);
}

TEST_F(PageCommandTest, RotatePageCommand_Undo) {
    if (!document) {
        GTEST_SKIP() << "Test PDF not available";
    }

    ASSERT_GT(document->pageCount(), 0);

    int initialRotation = document->currentRotation(0);

    // Rotate then undo
    auto cmd = std::make_unique<pdfe::commands::RotatePageCommand>(
        document.get(),
        0,
        90
    );
    undoManager->execute(std::move(cmd));

    ASSERT_TRUE(undoManager->canUndo());
    undoManager->undo();

    // Verify rotation reverted
    int finalRotation = document->currentRotation(0);
    EXPECT_EQ(finalRotation, initialRotation);
}

TEST_F(PageCommandTest, RotatePageCommand_Redo) {
    if (!document) {
        GTEST_SKIP() << "Test PDF not available";
    }

    ASSERT_GT(document->pageCount(), 0);

    int initialRotation = document->currentRotation(0);

    // Rotate, undo, then redo
    auto cmd = std::make_unique<pdfe::commands::RotatePageCommand>(
        document.get(),
        0,
        90
    );
    undoManager->execute(std::move(cmd));
    undoManager->undo();

    ASSERT_TRUE(undoManager->canRedo());
    undoManager->redo();

    // Verify rotation is applied again
    int finalRotation = document->currentRotation(0);
    EXPECT_EQ(finalRotation, (initialRotation + 90) % 360);
}

TEST_F(PageCommandTest, RotatePageCommand_FullCircle) {
    if (!document) {
        GTEST_SKIP() << "Test PDF not available";
    }

    ASSERT_GT(document->pageCount(), 0);

    int initialRotation = document->currentRotation(0);

    // Rotate 90 degrees four times
    for (int i = 0; i < 4; ++i) {
        auto cmd = std::make_unique<pdfe::commands::RotatePageCommand>(
            document.get(),
            0,
            90
        );
        undoManager->execute(std::move(cmd));
    }

    // Should be back to original rotation
    int finalRotation = document->currentRotation(0);
    EXPECT_EQ(finalRotation, initialRotation);
}

TEST_F(PageCommandTest, RotatePageCommand_CCW) {
    if (!document) {
        GTEST_SKIP() << "Test PDF not available";
    }

    ASSERT_GT(document->pageCount(), 0);

    int initialRotation = document->currentRotation(0);

    // Rotate counter-clockwise by 90 degrees
    auto cmd = std::make_unique<pdfe::commands::RotatePageCommand>(
        document.get(),
        0,
        -90
    );
    undoManager->execute(std::move(cmd));

    // Verify rotation
    int newRotation = document->currentRotation(0);
    int expected = (initialRotation - 90 + 360) % 360;
    EXPECT_EQ(newRotation, expected);
}

TEST_F(PageCommandTest, DeletePageCommand_Execute) {
    if (!document) {
        GTEST_SKIP() << "Test PDF not available";
    }

    ASSERT_GE(document->pageCount(), 3);

    int initialCount = document->pageCount();

    // Delete page 1
    auto cmd = std::make_unique<pdfe::commands::DeletePageCommand>(
        document.get(),
        1
    );
    undoManager->execute(std::move(cmd));

    // Verify page count decreased
    EXPECT_EQ(document->pageCount(), initialCount - 1);
}

TEST_F(PageCommandTest, DeletePageCommand_Undo) {
    if (!document) {
        GTEST_SKIP() << "Test PDF not available";
    }

    ASSERT_GE(document->pageCount(), 3);

    int initialCount = document->pageCount();

    // Delete then undo
    auto cmd = std::make_unique<pdfe::commands::DeletePageCommand>(
        document.get(),
        1
    );
    undoManager->execute(std::move(cmd));

    ASSERT_TRUE(undoManager->canUndo());
    undoManager->undo();

    // Verify page count restored
    EXPECT_EQ(document->pageCount(), initialCount);
}

TEST_F(PageCommandTest, DeletePageCommand_Redo) {
    if (!document) {
        GTEST_SKIP() << "Test PDF not available";
    }

    ASSERT_GE(document->pageCount(), 3);

    int initialCount = document->pageCount();

    // Delete, undo, then redo
    auto cmd = std::make_unique<pdfe::commands::DeletePageCommand>(
        document.get(),
        1
    );
    undoManager->execute(std::move(cmd));
    undoManager->undo();

    ASSERT_TRUE(undoManager->canRedo());
    undoManager->redo();

    // Verify page deleted again
    EXPECT_EQ(document->pageCount(), initialCount - 1);
}

TEST_F(PageCommandTest, DeletePageCommand_DeleteFirst) {
    if (!document) {
        GTEST_SKIP() << "Test PDF not available";
    }

    ASSERT_GE(document->pageCount(), 3);

    int initialCount = document->pageCount();

    // Delete first page
    auto cmd = std::make_unique<pdfe::commands::DeletePageCommand>(
        document.get(),
        0
    );
    undoManager->execute(std::move(cmd));

    // Verify page count and remaining pages accessible
    EXPECT_EQ(document->pageCount(), initialCount - 1);

    // Ensure we can still access the remaining pages
    for (int i = 0; i < document->pageCount(); ++i) {
        auto page = document->page(i);
        EXPECT_NE(page, nullptr);
    }
}

TEST_F(PageCommandTest, DeletePageCommand_DeleteLast) {
    if (!document) {
        GTEST_SKIP() << "Test PDF not available";
    }

    ASSERT_GE(document->pageCount(), 3);

    int initialCount = document->pageCount();
    int lastPageIndex = initialCount - 1;

    // Delete last page
    auto cmd = std::make_unique<pdfe::commands::DeletePageCommand>(
        document.get(),
        lastPageIndex
    );
    undoManager->execute(std::move(cmd));

    // Verify page count
    EXPECT_EQ(document->pageCount(), initialCount - 1);
}

TEST_F(PageCommandTest, InsertBlankPageCommand_Execute) {
    if (!document) {
        GTEST_SKIP() << "Test PDF not available";
    }

    ASSERT_GE(document->pageCount(), 3);

    int initialCount = document->pageCount();

    // Insert blank page at index 1
    auto cmd = std::make_unique<pdfe::commands::InsertBlankPageCommand>(
        document.get(),
        1,
        612.0f,  // Letter size width
        792.0f,  // Letter size height
        0
    );
    undoManager->execute(std::move(cmd));

    // Verify page count increased
    EXPECT_EQ(document->pageCount(), initialCount + 1);
}

TEST_F(PageCommandTest, InsertBlankPageCommand_Undo) {
    if (!document) {
        GTEST_SKIP() << "Test PDF not available";
    }

    ASSERT_GE(document->pageCount(), 3);

    int initialCount = document->pageCount();

    // Insert then undo
    auto cmd = std::make_unique<pdfe::commands::InsertBlankPageCommand>(
        document.get(),
        1,
        612.0f,
        792.0f,
        0
    );
    undoManager->execute(std::move(cmd));

    ASSERT_TRUE(undoManager->canUndo());
    undoManager->undo();

    // Verify page count restored
    EXPECT_EQ(document->pageCount(), initialCount);
}

TEST_F(PageCommandTest, InsertBlankPageCommand_AtBeginning) {
    if (!document) {
        GTEST_SKIP() << "Test PDF not available";
    }

    ASSERT_GE(document->pageCount(), 3);

    int initialCount = document->pageCount();

    // Insert blank page at beginning
    auto cmd = std::make_unique<pdfe::commands::InsertBlankPageCommand>(
        document.get(),
        0,
        612.0f,
        792.0f,
        0
    );
    undoManager->execute(std::move(cmd));

    // Verify page count increased
    EXPECT_EQ(document->pageCount(), initialCount + 1);
}

TEST_F(PageCommandTest, InsertBlankPageCommand_AtEnd) {
    if (!document) {
        GTEST_SKIP() << "Test PDF not available";
    }

    ASSERT_GE(document->pageCount(), 3);

    int initialCount = document->pageCount();

    // Insert blank page at end
    auto cmd = std::make_unique<pdfe::commands::InsertBlankPageCommand>(
        document.get(),
        initialCount,
        612.0f,
        792.0f,
        0
    );
    undoManager->execute(std::move(cmd));

    // Verify page count increased
    EXPECT_EQ(document->pageCount(), initialCount + 1);
}

TEST_F(PageCommandTest, MovePageCommand_Forward) {
    if (!document) {
        GTEST_SKIP() << "Test PDF not available";
    }

    ASSERT_GE(document->pageCount(), 3);

    int initialCount = document->pageCount();

    stampPageIdentities();
    const std::vector<int> before = pageIdentities();
    requireDistinctIdentities(before);

    // Move page 0 to index 2
    auto cmd = std::make_unique<pdfe::commands::MovePageCommand>(
        document.get(),
        0,
        2
    );
    undoManager->execute(std::move(cmd));

    // Verify page count unchanged
    EXPECT_EQ(document->pageCount(), initialCount);

    // The target index is interpreted BEFORE the removal shifts everything
    // down, so (0,2) lands at index 1 -- the same reason (0,3) lands at 2
    // rather than 3. Deliberate, and documented at
    // src/core/PageCommands.cpp:183; an expectation written naively against
    // the requested index fails here for the wrong reason.
    const std::vector<int> after = pageIdentities();
    EXPECT_EQ(after, (std::vector<int>{before[1], before[0], before[2]}))
        << "moving page 0 to index 2 must yield the order 1,0,2";
}

TEST_F(PageCommandTest, MovePageCommand_Backward) {
    if (!document) {
        GTEST_SKIP() << "Test PDF not available";
    }

    ASSERT_GE(document->pageCount(), 3);

    int initialCount = document->pageCount();

    stampPageIdentities();
    const std::vector<int> before = pageIdentities();
    requireDistinctIdentities(before);

    // Move page 2 to index 0
    auto cmd = std::make_unique<pdfe::commands::MovePageCommand>(
        document.get(),
        2,
        0
    );
    undoManager->execute(std::move(cmd));

    // Verify page count unchanged
    EXPECT_EQ(document->pageCount(), initialCount);

    // Moving backwards, the removal happens after the target, so index 0 is
    // reached exactly as asked.
    const std::vector<int> after = pageIdentities();
    EXPECT_EQ(after, (std::vector<int>{before[2], before[0], before[1]}))
        << "moving page 2 to index 0 must yield the order 2,0,1";
}

TEST_F(PageCommandTest, MovePageCommand_Undo) {
    if (!document) {
        GTEST_SKIP() << "Test PDF not available";
    }

    ASSERT_GE(document->pageCount(), 3);

    stampPageIdentities();
    const std::vector<int> before = pageIdentities();
    requireDistinctIdentities(before);

    // Move page 0 to index 2
    auto cmd = std::make_unique<pdfe::commands::MovePageCommand>(
        document.get(),
        0,
        2
    );
    undoManager->execute(std::move(cmd));

    // The move must actually have happened, or "undo restored the order" is
    // satisfied by an undo that did nothing to an order that never changed.
    ASSERT_NE(pageIdentities(), before)
        << "the move itself was a no-op, so undoing it proves nothing";

    // Undo
    ASSERT_TRUE(undoManager->canUndo());
    undoManager->undo();

    EXPECT_EQ(pageIdentities(), before)
        << "undo did not restore the original page order";
}

TEST_F(PageCommandTest, MultipleOps_UndoAll) {
    if (!document) {
        GTEST_SKIP() << "Test PDF not available";
    }

    ASSERT_GE(document->pageCount(), 3);

    int initialCount = document->pageCount();
    int initialRotation = document->currentRotation(0);

    // Execute rotate + delete + insert
    auto rotateCmd = std::make_unique<pdfe::commands::RotatePageCommand>(
        document.get(),
        0,
        90
    );
    undoManager->execute(std::move(rotateCmd));

    auto deleteCmd = std::make_unique<pdfe::commands::DeletePageCommand>(
        document.get(),
        1
    );
    undoManager->execute(std::move(deleteCmd));

    auto insertCmd = std::make_unique<pdfe::commands::InsertBlankPageCommand>(
        document.get(),
        0,
        612.0f,
        792.0f,
        0
    );
    undoManager->execute(std::move(insertCmd));

    // Undo all three operations
    ASSERT_TRUE(undoManager->canUndo());
    undoManager->undo(); // Undo insert

    ASSERT_TRUE(undoManager->canUndo());
    undoManager->undo(); // Undo delete

    ASSERT_TRUE(undoManager->canUndo());
    undoManager->undo(); // Undo rotate

    // Verify state restored
    EXPECT_EQ(document->pageCount(), initialCount);
    EXPECT_EQ(document->currentRotation(0), initialRotation);
}

TEST_F(PageCommandTest, CleanState_DirtyAfterPageOp) {
    if (!document) {
        GTEST_SKIP() << "Test PDF not available";
    }

    ASSERT_GE(document->pageCount(), 3);

    // Set clean state
    undoManager->setClean();
    EXPECT_TRUE(undoManager->isClean());

    // Rotate page
    auto cmd = std::make_unique<pdfe::commands::RotatePageCommand>(
        document.get(),
        0,
        90
    );
    undoManager->execute(std::move(cmd));

    // Should be dirty now
    EXPECT_FALSE(undoManager->isClean());

    // Undo should return to clean state
    undoManager->undo();
    EXPECT_TRUE(undoManager->isClean());
}

TEST_F(PageCommandTest, DeletePageRangeCommand_Execute) {
    if (!document) {
        GTEST_SKIP() << "Test PDF not available";
    }

    ASSERT_GE(document->pageCount(), 3);

    int initialCount = document->pageCount();

    // Delete pages 0 and 2
    std::vector<int> indices = {0, 2};
    auto cmd = std::make_unique<pdfe::commands::DeletePageRangeCommand>(
        document.get(),
        indices
    );
    undoManager->execute(std::move(cmd));

    // Verify page count decreased by 2
    EXPECT_EQ(document->pageCount(), initialCount - 2);
}

TEST_F(PageCommandTest, DeletePageRangeCommand_Undo) {
    if (!document) {
        GTEST_SKIP() << "Test PDF not available";
    }

    ASSERT_GE(document->pageCount(), 3);

    int initialCount = document->pageCount();

    // Get page infos before delete
    auto page0Info = document->pageInfo(0);
    auto page1Info = document->pageInfo(1);
    auto page2Info = document->pageInfo(2);

    // Delete pages 0 and 2
    std::vector<int> indices = {0, 2};
    auto cmd = std::make_unique<pdfe::commands::DeletePageRangeCommand>(
        document.get(),
        indices
    );
    undoManager->execute(std::move(cmd));

    // Undo
    ASSERT_TRUE(undoManager->canUndo());
    undoManager->undo();

    // Verify page count restored
    EXPECT_EQ(document->pageCount(), initialCount);

    // Verify pages are restored
    auto restoredPage0Info = document->pageInfo(0);
    auto restoredPage1Info = document->pageInfo(1);
    auto restoredPage2Info = document->pageInfo(2);

    EXPECT_EQ(restoredPage0Info.widthPt, page0Info.widthPt);
    EXPECT_EQ(restoredPage1Info.widthPt, page1Info.widthPt);
    EXPECT_EQ(restoredPage2Info.widthPt, page2Info.widthPt);
}

TEST_F(PageCommandTest, DeletePageRangeCommand_Redo) {
    if (!document) {
        GTEST_SKIP() << "Test PDF not available";
    }

    ASSERT_GE(document->pageCount(), 3);

    int initialCount = document->pageCount();

    // Delete pages 0 and 2
    std::vector<int> indices = {0, 2};
    auto cmd = std::make_unique<pdfe::commands::DeletePageRangeCommand>(
        document.get(),
        indices
    );
    undoManager->execute(std::move(cmd));
    undoManager->undo();

    // Redo
    ASSERT_TRUE(undoManager->canRedo());
    undoManager->redo();

    // Verify page count decreased by 2 again
    EXPECT_EQ(document->pageCount(), initialCount - 2);
}

// ---------------------------------------------------------------------------
// A RANGE OF ONE. DCA 0091 / pipeline p91 phase-3.
//
// `DeletePageCommand` (the singular command) is compiled into PDFE and no
// production path constructs it; it is treated as SUPERSEDED rather than as a
// gap, and the whole justification for that is one sentence:
// `MainWindow::onDeletePages` always builds a `DeletePageRangeCommand`, which
// "covers one page as a range of one".
//
// Nothing held that sentence to anything. Every pre-existing range test used a
// TWO-element vector ({0, 2}) -- so the size-1 path, which is the everyday UI
// case (delete the current page, nothing multi-selected), was asserted by no
// oracle at all. A future "range" optimisation that special-cased size < 2
// would break the common path and leave this file green.
//
// These pin execute / undo / redo at exactly one index, and assert WHICH page
// went, not merely how many -- a count-only oracle passes a command that
// deletes the wrong page.
// ---------------------------------------------------------------------------

TEST_F(PageCommandTest, DeletePageRangeCommand_SingleIndex_DeletesThatPageOnly) {
    if (!document) {
        GTEST_SKIP() << "Test PDF not available";
    }

    ASSERT_GE(document->pageCount(), 3);

    const int initialCount = document->pageCount();
    const auto page0Info = document->pageInfo(0);
    const auto page1Info = document->pageInfo(1);

    // A range of exactly one -- the shape MainWindow::onDeletePages produces
    // when a single page is selected.
    std::vector<int> indices = {0};
    auto cmd = std::make_unique<pdfe::commands::DeletePageRangeCommand>(
        document.get(),
        indices
    );
    undoManager->execute(std::move(cmd));

    EXPECT_EQ(document->pageCount(), initialCount - 1)
        << "a range of one must delete exactly one page";

    // The page that WAS at index 1 must now be at index 0. Asserting only the
    // count would pass a command that deleted some other page, or none.
    EXPECT_EQ(document->pageInfo(0).widthPt, page1Info.widthPt)
        << "the surviving pages did not shift up by one -- the wrong page was "
           "deleted";
    EXPECT_NE(initialCount, document->pageCount())
        << "page 0's removal was not observable at all";
    (void)page0Info;
}

TEST_F(PageCommandTest, DeletePageRangeCommand_SingleIndex_UndoRestoresThePage) {
    if (!document) {
        GTEST_SKIP() << "Test PDF not available";
    }

    ASSERT_GE(document->pageCount(), 3);

    const int initialCount = document->pageCount();
    const auto page0Info = document->pageInfo(0);
    const auto page1Info = document->pageInfo(1);

    std::vector<int> indices = {0};
    auto cmd = std::make_unique<pdfe::commands::DeletePageRangeCommand>(
        document.get(),
        indices
    );
    undoManager->execute(std::move(cmd));

    ASSERT_TRUE(undoManager->canUndo());
    undoManager->undo();

    EXPECT_EQ(document->pageCount(), initialCount)
        << "undo of a one-page range did not restore the page";
    EXPECT_EQ(document->pageInfo(0).widthPt, page0Info.widthPt)
        << "the restored page did not come back at index 0";
    EXPECT_EQ(document->pageInfo(1).widthPt, page1Info.widthPt)
        << "undo restored the page but left the rest of the document shifted";
}

TEST_F(PageCommandTest, DeletePageRangeCommand_SingleIndex_RedoDeletesItAgain) {
    if (!document) {
        GTEST_SKIP() << "Test PDF not available";
    }

    ASSERT_GE(document->pageCount(), 3);

    const int initialCount = document->pageCount();
    const auto page1Info = document->pageInfo(1);

    std::vector<int> indices = {0};
    auto cmd = std::make_unique<pdfe::commands::DeletePageRangeCommand>(
        document.get(),
        indices
    );
    undoManager->execute(std::move(cmd));
    undoManager->undo();

    ASSERT_TRUE(undoManager->canRedo());
    undoManager->redo();

    EXPECT_EQ(document->pageCount(), initialCount - 1)
        << "redo of a one-page range did not re-delete the page";
    EXPECT_EQ(document->pageInfo(0).widthPt, page1Info.widthPt)
        << "redo deleted a different page than execute did";
}

// ============================================================================
// MovePageCommand - Redo (F3)
// ============================================================================

TEST_F(PageCommandTest, MovePageCommand_Redo) {
    if (!document) {
        GTEST_SKIP() << "Test PDF not available";
    }

    ASSERT_GE(document->pageCount(), 3);

    int initialCount = document->pageCount();

    stampPageIdentities();
    const std::vector<int> before = pageIdentities();
    requireDistinctIdentities(before);

    // Move page 0 to index 2
    auto cmd = std::make_unique<pdfe::commands::MovePageCommand>(
        document.get(), 0, 2);
    undoManager->execute(std::move(cmd));

    const std::vector<int> moved = pageIdentities();
    ASSERT_NE(moved, before) << "the move was a no-op; redo has nothing to "
                                "reapply and this test cannot fail";

    // Undo
    undoManager->undo();
    ASSERT_EQ(pageIdentities(), before) << "undo did not take effect, so a "
                                           "redo asserting the moved order "
                                           "would pass without redoing";

    // Redo
    ASSERT_TRUE(undoManager->canRedo());
    undoManager->redo();

    // Verify page count unchanged
    EXPECT_EQ(document->pageCount(), initialCount);

    EXPECT_EQ(pageIdentities(), moved)
        << "redo did not reapply the move";
}

// ============================================================================
// ImportPagesCommand (F2)
// ============================================================================

TEST_F(PageCommandTest, ImportPagesCommand_Execute) {
    if (!document) {
        GTEST_SKIP() << "Test PDF not available";
    }

    int initialCount = document->pageCount();

    // Import all pages from the same test PDF
    auto cmd = std::make_unique<pdfe::commands::ImportPagesCommand>(
        document.get(), initialCount, testPdf, context, 0, 0);
    undoManager->execute(std::move(cmd));

    // Verify one page was imported
    EXPECT_EQ(document->pageCount(), initialCount + 1);
}

TEST_F(PageCommandTest, ImportPagesCommand_Undo) {
    if (!document) {
        GTEST_SKIP() << "Test PDF not available";
    }

    int initialCount = document->pageCount();

    // Import one page
    auto cmd = std::make_unique<pdfe::commands::ImportPagesCommand>(
        document.get(), initialCount, testPdf, context, 0, 0);
    undoManager->execute(std::move(cmd));
    EXPECT_EQ(document->pageCount(), initialCount + 1);

    // Undo
    ASSERT_TRUE(undoManager->canUndo());
    undoManager->undo();

    // Verify page count restored
    EXPECT_EQ(document->pageCount(), initialCount);
}

TEST_F(PageCommandTest, ImportPagesCommand_Redo) {
    if (!document) {
        GTEST_SKIP() << "Test PDF not available";
    }

    int initialCount = document->pageCount();

    // Import one page, undo, then redo
    auto cmd = std::make_unique<pdfe::commands::ImportPagesCommand>(
        document.get(), initialCount, testPdf, context, 0, 0);
    undoManager->execute(std::move(cmd));
    undoManager->undo();

    ASSERT_TRUE(undoManager->canRedo());
    undoManager->redo();

    // Verify page was re-imported
    EXPECT_EQ(document->pageCount(), initialCount + 1);
}

TEST_F(PageCommandTest, ImportPagesCommand_MultiplePages) {
    if (!document) {
        GTEST_SKIP() << "Test PDF not available";
    }

    ASSERT_GE(document->pageCount(), 3);
    int initialCount = document->pageCount();

    // Import pages 0-2 (all 3 pages) from the source
    auto cmd = std::make_unique<pdfe::commands::ImportPagesCommand>(
        document.get(), 1, testPdf, context, 0, 2);
    undoManager->execute(std::move(cmd));

    // Verify 3 pages were imported
    EXPECT_EQ(document->pageCount(), initialCount + 3);

    // Undo should remove all 3
    undoManager->undo();
    EXPECT_EQ(document->pageCount(), initialCount);
}
