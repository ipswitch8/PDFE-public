// -*- coding: utf-8 -*-
// Phase 9 (coordinator): AlignmentPipeline tracks pending alignment
// changes and flushes them to disk via AlignmentInjector. This is the
// MainWindow-layer integration surface; tests verify the
// queue-and-flush contract without requiring a real document.
#include <gtest/gtest.h>
#include "reflow/AlignmentPipeline.h"
#include "reflow/AlignmentWriter.h"
#include "reflow/TextReflowEngine.h"
#include "reflow/ReflowTypes.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "core/TextBlock.h"
#include "core/TextLine.h"

#include <cmath>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>

using namespace pdfe::reflow;
using namespace pdfe::core;

namespace {
std::filesystem::path fixturesDir()
{
    return std::filesystem::path(TEST_FIXTURES_DIR);
}
std::filesystem::path tempOut(const char* name)
{
    auto d = std::filesystem::temp_directory_path() / "pdfe_align_pipeline";
    std::filesystem::create_directories(d);
    return d / name;
}

AlignmentWriter::BlockParams makeParams(Alignment align,
                                        float left = 72.0f,
                                        float top = 700.0f,
                                        float width = 400.0f)
{
    AlignmentWriter::BlockParams p;
    p.alignment = align;
    auto measurer = [](const std::string& w) {
        return static_cast<float>(w.size()) * 6.0f;
    };
    auto words = TextReflowEngine::tokenize(
        "alignment pipeline block text wrapping across lines", measurer);
    p.lines = TextReflowEngine::reflow(words, 200.0f, 6.0f);
    p.blockLeftPt = left;
    p.blockTopPt = top;
    p.blockWidthPt = width;
    p.lineHeightPt = 14.0f;
    p.fontSizePt = 12.0f;
    p.fontResourceName = "F1";
    return p;
}

/// Left edge (PDF user-space x) of the first extracted text line on @p page
/// whose text contains @p needle. Returns NaN when no such line exists.
/// Used to read back the ACTUAL horizontal placement of an injected
/// alignment block rather than merely its presence.
double injectedLineLeftX(pdfe::core::Page* page, const std::string& needle)
{
    double best = std::numeric_limits<double>::quiet_NaN();
    for (const auto& block : page->extractText()) {
        for (const auto& line : block.lines) {
            const std::string t = line.text();
            if (t.find(needle) == std::string::npos)
                continue;
            // Prefer the first glyph's quad; fall back to the line bbox.
            double x = line.chars.empty() ? line.boundingBox.left()
                                          : line.chars.front().quad.left();
            if (std::isnan(best) || x < best)
                best = x;
        }
    }
    return best;
}

} // namespace

class AlignmentPipelineTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx = std::make_shared<MuPdfContext>();
    }
    std::shared_ptr<MuPdfContext> ctx;
};

TEST_F(AlignmentPipelineTest, EmptyQueueReportsZeroPendingPages)
{
    AlignmentPipeline pipe(ctx);
    EXPECT_EQ(pipe.pendingPageCount(), 0u);
    EXPECT_TRUE(pipe.snapshot().empty());
}

TEST_F(AlignmentPipelineTest, QueueAndSnapshotMatchesInput)
{
    AlignmentPipeline pipe(ctx);
    pipe.queueAlignment(0, makeParams(Alignment::Center));
    pipe.queueAlignment(2, makeParams(Alignment::Right));
    EXPECT_EQ(pipe.pendingPageCount(), 2u);

    const auto snap = pipe.snapshot();
    EXPECT_EQ(snap.size(), 2u);

    bool sawPage0 = false, sawPage2 = false;
    for (const auto& b : snap) {
        if (b.pageIndex == 0 && b.params.alignment == Alignment::Center) sawPage0 = true;
        if (b.pageIndex == 2 && b.params.alignment == Alignment::Right)  sawPage2 = true;
    }
    EXPECT_TRUE(sawPage0);
    EXPECT_TRUE(sawPage2);
}

TEST_F(AlignmentPipelineTest, QueueSameBoundsTwiceReplacesAlignment)
{
    AlignmentPipeline pipe(ctx);
    pipe.queueAlignment(0, makeParams(Alignment::Left));
    pipe.queueAlignment(0, makeParams(Alignment::Justify));
    // Same bounds → last-write-wins, still one block.
    EXPECT_EQ(pipe.pendingPageCount(), 1u);
    const auto snap = pipe.snapshot();
    ASSERT_EQ(snap.size(), 1u);
    EXPECT_EQ(snap[0].params.alignment, Alignment::Justify);
}

TEST_F(AlignmentPipelineTest, DifferentBoundsSamePageAccumulate)
{
    AlignmentPipeline pipe(ctx);
    pipe.queueAlignment(0, makeParams(Alignment::Left, /*left=*/72.0f,
                                      /*top=*/700.0f));
    // Different top coord → different block.
    pipe.queueAlignment(0, makeParams(Alignment::Right, /*left=*/72.0f,
                                      /*top=*/500.0f));
    EXPECT_EQ(pipe.pendingPageCount(), 1u);
    EXPECT_EQ(pipe.snapshot().size(), 2u);
}

TEST_F(AlignmentPipelineTest, ClearPageDropsOnlyThatPage)
{
    AlignmentPipeline pipe(ctx);
    pipe.queueAlignment(0, makeParams(Alignment::Center));
    pipe.queueAlignment(1, makeParams(Alignment::Right));
    pipe.clearPage(0);
    EXPECT_EQ(pipe.pendingPageCount(), 1u);
    const auto snap = pipe.snapshot();
    ASSERT_EQ(snap.size(), 1u);
    EXPECT_EQ(snap[0].pageIndex, 1);
}

TEST_F(AlignmentPipelineTest, ClearDropsEverything)
{
    AlignmentPipeline pipe(ctx);
    pipe.queueAlignment(0, makeParams(Alignment::Center));
    pipe.queueAlignment(1, makeParams(Alignment::Right));
    pipe.clear();
    EXPECT_EQ(pipe.pendingPageCount(), 0u);
    EXPECT_TRUE(pipe.snapshot().empty());
}

// A1-45 oracle repair: asserting only that the phrase is PRESENT on pages 0
// and 2 passes unchanged if Left and Right are swapped (or if the alignment
// enum is ignored entirely). The block's horizontal placement is the thing
// alignment actually controls, so the oracle now reads the emitted glyph x
// back out of the saved PDF and compares it against the offset
// TextReflowEngine::lineXOffset() is contractually required to produce for
// each mode, using the AUTHORED BlockParams (blockLeftPt / blockWidthPt /
// measured line width) as ground truth.
TEST_F(AlignmentPipelineTest, FlushAppliesPendingBlocksToOutputPdf)
{
    auto src = fixturesDir() / "test_3page.pdf";
    if (!std::filesystem::exists(src)) {
        GTEST_SKIP() << "test_3page.pdf not available";
    }

    const auto leftParams  = makeParams(Alignment::Left);
    const auto rightParams = makeParams(Alignment::Right);
    ASSERT_FALSE(leftParams.lines.empty());
    ASSERT_EQ(leftParams.lines.size(), rightParams.lines.size());

    // Authored ground truth taken straight from the params we queue.
    const std::string probe        = leftParams.lines[0].text;
    const float       firstLineW   = leftParams.lines[0].width;
    const float       blockLeftPt  = leftParams.blockLeftPt;
    const float       blockWidthPt = leftParams.blockWidthPt;
    ASSERT_FALSE(probe.empty());
    ASSERT_GT(firstLineW, 0.0f);
    ASSERT_GT(blockWidthPt, firstLineW)
        << "the fixture params must leave slack, else Left and Right "
           "coincide and the test could not distinguish them";

    AlignmentPipeline pipe(ctx);
    pipe.queueAlignment(0, leftParams);
    pipe.queueAlignment(2, rightParams);
    auto out = tempOut("flushed.pdf");
    std::filesystem::remove(out);
    ASSERT_NO_THROW(pipe.flush(src, out));

    auto doc = Document::open(ctx, out);
    ASSERT_NE(doc, nullptr);
    // Target pages should contain the queued phrase; untouched middle
    // page should not.
    EXPECT_NE(doc->page(0)->extractPlainText().find("alignment pipeline"),
              std::string::npos);
    EXPECT_EQ(doc->page(1)->extractPlainText().find("alignment pipeline"),
              std::string::npos);
    EXPECT_NE(doc->page(2)->extractPlainText().find("alignment pipeline"),
              std::string::npos);

    // --- Placement oracle ---------------------------------------------
    auto pageLeft  = doc->page(0);   // queued Alignment::Left
    auto pageRight = doc->page(2);   // queued Alignment::Right
    ASSERT_NE(pageLeft, nullptr);
    ASSERT_NE(pageRight, nullptr);

    const double xLeft  = injectedLineLeftX(pageLeft.get(), probe);
    const double xRight = injectedLineLeftX(pageRight.get(), probe);
    ASSERT_FALSE(std::isnan(xLeft))
        << "injected first line not found on the Left-aligned page";
    ASSERT_FALSE(std::isnan(xRight))
        << "injected first line not found on the Right-aligned page";

    // (a) The Left block starts at the block's own left edge. Tolerance
    //     covers the glyph's left side bearing only.
    EXPECT_NEAR(xLeft, blockLeftPt, 3.0)
        << "Left-aligned block must start at blockLeftPt=" << blockLeftPt;

    // (b) Right-vs-Left delta is exactly lineXOffset(Right) - lineXOffset(Left)
    //     = (blockWidth - lineWidth) - 0. Both pages render the same string in
    //     the same font, so the side bearing cancels and the tolerance is
    //     tight. Swapping the two Alignment enum values, or dropping the
    //     xOffset term from AlignmentWriter::emitFragment, makes this FAIL.
    const double expectedDelta =
        static_cast<double>(blockWidthPt) - static_cast<double>(firstLineW);
    EXPECT_NEAR(xRight - xLeft, expectedDelta, 1.0)
        << "Right-aligned x=" << xRight << " Left-aligned x=" << xLeft
        << " expected delta " << expectedDelta;

    // (c) Direction check independent of the arithmetic above, expressed
    //     against the REAL page width read from the document (not a literal):
    //     the Left block starts in the left half, the Right block does not.
    const double pageW = pageLeft->boundingBox().width();
    ASSERT_GT(pageW, 0.0);
    EXPECT_LT(xLeft, pageW / 2.0)
        << "Left-aligned block should start in the left half of a "
        << pageW << "pt-wide page";
    EXPECT_GT(xRight, xLeft)
        << "Right-aligned block must start to the right of the "
           "Left-aligned one";
}

TEST_F(AlignmentPipelineTest, FlushWithEmptyQueueStillProducesValidOutput)
{
    auto src = fixturesDir() / "test_3page.pdf";
    if (!std::filesystem::exists(src)) {
        GTEST_SKIP() << "test_3page.pdf not available";
    }
    AlignmentPipeline pipe(ctx);
    auto out = tempOut("flushed_empty.pdf");
    std::filesystem::remove(out);
    ASSERT_NO_THROW(pipe.flush(src, out));
    auto doc = Document::open(ctx, out);
    ASSERT_NE(doc, nullptr);
    EXPECT_EQ(doc->pageCount(), 3);
}

TEST(AlignmentPipelineCtor, NullContextRejected)
{
    EXPECT_THROW(AlignmentPipeline(nullptr), std::invalid_argument);
}
