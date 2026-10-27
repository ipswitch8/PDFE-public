// -*- coding: utf-8 -*-
// Phase 7d: OcrAllPagesCommand tests — verifies the composite-command
// execute/undo flow and progress/cancel semantics without requiring a
// live Tesseract instance. Real OCR integration is exercised in situ
// via MainWindow wiring; this suite focuses on the command's contract.
#include <gtest/gtest.h>
#include "tre/OcrAllPagesCommand.h"

#include <set>
#include <vector>

using namespace pdfe::tre;

TEST(OcrAllPagesCommand, ExecuteRunsOcrForEveryListedPage)
{
    std::vector<int> pages{0, 1, 2, 3};
    std::set<int> ocred;
    OcrAllPagesCommand cmd(
        pages,
        [&](int p) { ocred.insert(p); },
        [](int) {},
        {});
    cmd.execute();
    EXPECT_EQ(ocred.size(), pages.size());
    for (int p : pages) EXPECT_TRUE(ocred.count(p));
    EXPECT_EQ(cmd.processedPages().size(), pages.size());
    EXPECT_FALSE(cmd.wasCancelled());
}

TEST(OcrAllPagesCommand, UndoInvalidatesEveryProcessedPage)
{
    std::set<int> invalidated;
    OcrAllPagesCommand cmd(
        {10, 20, 30},
        [](int) {},
        [&](int p) { invalidated.insert(p); },
        {});
    cmd.execute();
    EXPECT_TRUE(invalidated.empty());
    cmd.undo();
    EXPECT_EQ(invalidated.size(), 3u);
    EXPECT_TRUE(invalidated.count(10));
    EXPECT_TRUE(invalidated.count(20));
    EXPECT_TRUE(invalidated.count(30));
}

TEST(OcrAllPagesCommand, UndoBeforeExecuteIsNoOp)
{
    std::set<int> invalidated;
    OcrAllPagesCommand cmd(
        {1, 2},
        [](int) {},
        [&](int p) { invalidated.insert(p); },
        {});
    cmd.undo();  // must not fire without prior execute
    EXPECT_TRUE(invalidated.empty());
}

TEST(OcrAllPagesCommand, CancelViaProgressCallbackStopsMidRun)
{
    std::set<int> ocred;
    OcrAllPagesCommand cmd(
        {0, 1, 2, 3, 4},
        [&](int p) { ocred.insert(p); },
        [](int) {},
        [](int done, int /*total*/) -> bool {
            // Cancel after the 2nd page completes.
            return done < 2;
        });
    cmd.execute();
    EXPECT_TRUE(cmd.wasCancelled());
    EXPECT_EQ(ocred.size(), 2u);
    EXPECT_EQ(cmd.processedPages().size(), 2u);
}

TEST(OcrAllPagesCommand, CancelMidRunStillUndoableOnProcessedPages)
{
    std::set<int> ocred;
    std::set<int> invalidated;
    OcrAllPagesCommand cmd(
        {0, 1, 2, 3, 4},
        [&](int p) { ocred.insert(p); },
        [&](int p) { invalidated.insert(p); },
        [](int done, int /*total*/) -> bool { return done < 3; });
    cmd.execute();
    ASSERT_TRUE(cmd.wasCancelled());
    ASSERT_EQ(cmd.processedPages().size(), 3u);

    cmd.undo();
    // Undo invalidates exactly the pages that succeeded.
    EXPECT_EQ(invalidated.size(), 3u);
    for (int p : cmd.processedPages()) {
        EXPECT_TRUE(invalidated.count(p)) << "page " << p << " not invalidated";
    }
}

TEST(OcrAllPagesCommand, PerPageOcrFailureDoesNotAbortBatch)
{
    // The command swallows exceptions from the OCR callable so one bad
    // page (empty, corrupt, whatever) doesn't kill the whole batch.
    std::set<int> ocred;
    OcrAllPagesCommand cmd(
        {0, 1, 2},
        [&](int p) {
            if (p == 1) throw std::runtime_error("simulated OCR failure");
            ocred.insert(p);
        },
        [](int) {},
        {});
    cmd.execute();
    EXPECT_EQ(ocred.size(), 2u);   // pages 0 and 2 succeeded
    EXPECT_EQ(cmd.processedPages().size(), 2u);
    EXPECT_TRUE(std::find(cmd.processedPages().begin(),
                          cmd.processedPages().end(), 1)
                == cmd.processedPages().end());
}

TEST(OcrAllPagesCommand, DescriptionIsStable)
{
    OcrAllPagesCommand cmd({}, [](int) {}, [](int) {}, {});
    EXPECT_EQ(cmd.description(), "OCR All Pages");
}

// Pipeline G phase-7 AC #4: explicit "cancel after page 2 of 5 →
// processes exactly 2 pages; undo reverses those 2 pages" scenario.
// Verifies the precise count contract that the MainWindow
// QProgressDialog wiring relies on: when the user clicks Cancel
// after the 2nd page completes, exactly 2 pages were OCR'd, exactly
// those 2 pages get invalidated on undo, and the remaining 3 pages
// were never touched (so no spurious invalidate calls).
TEST(OcrAllPagesCommand, CancelAfterPageTwoOfFiveProcessesExactlyTwoUndoReversesBoth)
{
    std::set<int> ocred;
    std::set<int> invalidated;
    OcrAllPagesCommand cmd(
        {0, 1, 2, 3, 4},
        [&](int p) { ocred.insert(p); },
        [&](int p) { invalidated.insert(p); },
        [](int done, int /*total*/) -> bool {
            // Cancel signals after the 2nd page completes (done==2).
            return done < 2;
        });
    cmd.execute();

    ASSERT_TRUE(cmd.wasCancelled());
    ASSERT_EQ(cmd.processedPages().size(), 2u);
    EXPECT_EQ(ocred.size(), 2u);
    EXPECT_TRUE(ocred.count(0));
    EXPECT_TRUE(ocred.count(1));
    // Pages 2, 3, 4 must remain untouched.
    EXPECT_FALSE(ocred.count(2));
    EXPECT_FALSE(ocred.count(3));
    EXPECT_FALSE(ocred.count(4));

    cmd.undo();
    EXPECT_EQ(invalidated.size(), 2u);
    EXPECT_TRUE(invalidated.count(0));
    EXPECT_TRUE(invalidated.count(1));
    // Unprocessed pages must NOT be invalidated.
    EXPECT_FALSE(invalidated.count(2));
    EXPECT_FALSE(invalidated.count(3));
    EXPECT_FALSE(invalidated.count(4));
}
