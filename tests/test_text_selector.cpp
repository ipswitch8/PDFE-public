#include <gtest/gtest.h>
#include "annotations/TextSelector.h"
#include "core/Document.h"
#include "core/Page.h"
#include "core/MuPdfContext.h"
#include <filesystem>
#include <memory>

using namespace pdfe::annotations;
using namespace pdfe::core;

/// Test fixture for TextSelector tests.
class TextSelectorTest : public ::testing::Test {
protected:
    void SetUp() override {
        context = std::make_shared<MuPdfContext>();
        std::filesystem::path fixturesDir = TEST_FIXTURES_DIR;
        testPdf = fixturesDir / "test_3page.pdf";

        if (std::filesystem::exists(testPdf)) {
            document = Document::open(context, testPdf);
            selector = std::make_unique<TextSelector>(context);
        }
    }

    void TearDown() override {
        selector.reset();
        document.reset();
    }

    std::shared_ptr<MuPdfContext> context;
    std::filesystem::path testPdf;
    std::unique_ptr<Document> document;
    std::unique_ptr<TextSelector> selector;
};

TEST_F(TextSelectorTest, ConstructorCreatesSelector)
{
    ASSERT_NE(selector, nullptr);
    EXPECT_FALSE(selector->isSelecting());
}

TEST_F(TextSelectorTest, BeginSelectionSetsSelectingState)
{
    ASSERT_NE(document, nullptr);
    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    selector->beginSelection(page.get(), QPointF(100, 100));
    EXPECT_TRUE(selector->isSelecting());
}

TEST_F(TextSelectorTest, BeginAndEndSelection)
{
    ASSERT_NE(document, nullptr);
    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    // Begin selection
    selector->beginSelection(page.get(), QPointF(100, 100));
    EXPECT_TRUE(selector->isSelecting());

    // Update to extend selection
    selector->updateSelection(QPointF(300, 100));

    // End selection
    TextSelection result = selector->endSelection();
    EXPECT_FALSE(selector->isSelecting());

    // Result should have valid page index
    EXPECT_EQ(result.pageIndex, 0);

    // If there's text in this region, we should have some quads
    // (This is page-dependent, but typically the first page has text)
    // We can't guarantee specific content, so just verify no crash occurred
}

TEST_F(TextSelectorTest, SelectionReturnsText)
{
    ASSERT_NE(document, nullptr);
    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    // Get page bounds and select a large area to ensure we capture text
    QRectF bounds = page->boundingBox();
    QPointF start(bounds.left() + 50, bounds.top() + 50);
    QPointF end(bounds.right() - 50, bounds.top() + 150);

    selector->beginSelection(page.get(), start);
    selector->updateSelection(end);
    TextSelection result = selector->endSelection();

    // If the page has any text, we should capture some
    // For test_3page.pdf, page 0 should have text
    if (!result.selectedText.isEmpty()) {
        EXPECT_GT(result.selectedText.length(), 0);
        EXPECT_GT(result.quads.size(), 0);
    }
}

TEST_F(TextSelectorTest, SelectionQuadsAreValid)
{
    ASSERT_NE(document, nullptr);
    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    QRectF bounds = page->boundingBox();
    selector->beginSelection(page.get(), QPointF(bounds.left() + 50, bounds.top() + 50));
    selector->updateSelection(QPointF(bounds.right() - 50, bounds.top() + 150));
    TextSelection result = selector->endSelection();

    // All quads should have positive dimensions
    for (const auto& quad : result.quads) {
        EXPECT_GT(quad.width(), 0.0);
        EXPECT_GT(quad.height(), 0.0);
    }
}

TEST_F(TextSelectorTest, EmptySelection)
{
    ASSERT_NE(document, nullptr);
    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    // Select at a single point (start and end at same location)
    QPointF point(100, 100);
    selector->beginSelection(page.get(), point);
    selector->updateSelection(point);
    TextSelection result = selector->endSelection();

    // Should not crash, result may be empty or contain the character at that point
    EXPECT_EQ(result.pageIndex, 0);
}

TEST_F(TextSelectorTest, SelectionOnEmptyRegion)
{
    ASSERT_NE(document, nullptr);
    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    // Select far outside the page bounds where there's definitely no text
    QRectF bounds = page->boundingBox();
    QPointF start(bounds.right() + 1000, bounds.bottom() + 1000);
    QPointF end(bounds.right() + 2000, bounds.bottom() + 2000);

    selector->beginSelection(page.get(), start);
    selector->updateSelection(end);
    TextSelection result = selector->endSelection();

    // Should return empty or minimal selection without crashing
    EXPECT_EQ(result.pageIndex, 0);
}

TEST_F(TextSelectorTest, CancelSelection)
{
    ASSERT_NE(document, nullptr);
    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    selector->beginSelection(page.get(), QPointF(100, 100));
    EXPECT_TRUE(selector->isSelecting());

    selector->updateSelection(QPointF(300, 100));
    EXPECT_TRUE(selector->isSelecting());

    selector->cancel();
    EXPECT_FALSE(selector->isSelecting());

    // After cancel, endSelection should return empty result
    TextSelection result = selector->endSelection();
    EXPECT_EQ(result.pageIndex, -1);
    EXPECT_TRUE(result.quads.empty());
    EXPECT_TRUE(result.selectedText.isEmpty());
}

TEST_F(TextSelectorTest, MultiLineSelection)
{
    ASSERT_NE(document, nullptr);
    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    // Select a large vertical area to span multiple lines
    QRectF bounds = page->boundingBox();
    QPointF start(bounds.left() + 50, bounds.top() + 50);
    QPointF end(bounds.right() - 50, bounds.top() + 300);  // Large vertical span

    selector->beginSelection(page.get(), start);
    selector->updateSelection(end);
    TextSelection result = selector->endSelection();

    // GitHub #273. This was:
    //
    //     if (result.quads.size() > 1) {
    //         EXPECT_GT(result.quads.size(), 1);     // <-- can never fail
    //         EXPECT_TRUE(result.selectedText.contains('\n'));
    //     }
    //
    // The first assertion RESTATED ITS OWN GUARD, so it was incapable of
    // failing — the "one-against-one" vacuous shape. And because the whole
    // body was gated, a selection that captured zero or one quad asserted
    // NOTHING, in a test called MultiLineSelection.
    //
    // The span above is 250 pt tall on purpose, so on this fixture the
    // selection must cross line boundaries. That is the property the test is
    // named for, and it is now asserted unconditionally. If this fails it is
    // real information: either the selector stopped spanning lines, or the
    // fixture's text layout moved — both worth knowing, neither worth hiding.
    ASSERT_GT(static_cast<int>(result.quads.size()), 1)
        << "a 250 pt vertical selection produced "
        << result.quads.size() << " quad(s); it must cross line boundaries on "
           "this fixture. Either TextSelector stopped spanning lines or the "
           "fixture's layout changed.";
    EXPECT_TRUE(result.selectedText.contains('\n'))
        << "a multi-line selection must contain newline characters; got: "
        // .toStdString(): QString has no operator<< for std::ostream, so
        // streaming it straight into a gtest message fails to compile
        // (gtest-message.h instantiates Message::operator<<<QString>).
        << result.selectedText.toStdString();
}

TEST_F(TextSelectorTest, IsSelectingDuringSelection)
{
    ASSERT_NE(document, nullptr);
    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    EXPECT_FALSE(selector->isSelecting());

    selector->beginSelection(page.get(), QPointF(100, 100));
    EXPECT_TRUE(selector->isSelecting());

    selector->updateSelection(QPointF(200, 100));
    EXPECT_TRUE(selector->isSelecting());

    selector->endSelection();
    EXPECT_FALSE(selector->isSelecting());
}

TEST_F(TextSelectorTest, CurrentQuadsDuringSelection)
{
    ASSERT_NE(document, nullptr);
    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    // Before selection
    EXPECT_TRUE(selector->currentQuads().empty());

    // During selection
    QRectF bounds = page->boundingBox();
    selector->beginSelection(page.get(), QPointF(bounds.left() + 50, bounds.top() + 50));
    selector->updateSelection(QPointF(bounds.right() - 50, bounds.top() + 150));

    // Should have some quads if text exists in the region
    auto quads = selector->currentQuads();
    // Can't guarantee specific count, but verify API works

    selector->endSelection();

    // After selection, currentQuads should be empty
    EXPECT_TRUE(selector->currentQuads().empty());
}

TEST_F(TextSelectorTest, UpdateSelectionWithoutBegin)
{
    // Calling updateSelection without beginSelection should be safe
    selector->updateSelection(QPointF(100, 100));
    EXPECT_FALSE(selector->isSelecting());
}

TEST_F(TextSelectorTest, EndSelectionWithoutBegin)
{
    // Calling endSelection without beginSelection should return empty result
    TextSelection result = selector->endSelection();
    EXPECT_EQ(result.pageIndex, -1);
    EXPECT_TRUE(result.quads.empty());
    EXPECT_TRUE(result.selectedText.isEmpty());
}

TEST_F(TextSelectorTest, BeginSelectionOnNullPage)
{
    // Calling beginSelection with null page should be safe
    selector->beginSelection(nullptr, QPointF(100, 100));
    EXPECT_FALSE(selector->isSelecting());
}

TEST_F(TextSelectorTest, ReverseSelection)
{
    ASSERT_NE(document, nullptr);
    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    // Select from right to left (reverse direction)
    QRectF bounds = page->boundingBox();
    QPointF start(bounds.right() - 50, bounds.top() + 50);
    QPointF end(bounds.left() + 50, bounds.top() + 50);

    selector->beginSelection(page.get(), start);
    selector->updateSelection(end);
    TextSelection result = selector->endSelection();

    // Should handle reverse selection the same as forward selection
    EXPECT_EQ(result.pageIndex, 0);
    // Should still produce valid quads if text exists
}

TEST_F(TextSelectorTest, MoveConstructor)
{
    auto selector2 = std::make_unique<TextSelector>(context);

    ASSERT_NE(document, nullptr);
    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    selector2->beginSelection(page.get(), QPointF(100, 100));
    EXPECT_TRUE(selector2->isSelecting());

    // Move construct
    TextSelector selector3(std::move(*selector2));
    EXPECT_TRUE(selector3.isSelecting());
}

TEST_F(TextSelectorTest, MoveAssignment)
{
    auto selector2 = std::make_unique<TextSelector>(context);
    TextSelector selector3(context);

    ASSERT_NE(document, nullptr);
    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    selector2->beginSelection(page.get(), QPointF(100, 100));
    EXPECT_TRUE(selector2->isSelecting());

    // Move assign
    selector3 = std::move(*selector2);
    EXPECT_TRUE(selector3.isSelecting());
}
