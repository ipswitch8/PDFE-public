#include <gtest/gtest.h>
#include "textedit/TextEditCommands.h"
#include "textedit/TextEditModel.h"
#include "textedit/ContentStreamEditor.h"
#include "core/Document.h"
#include "core/Page.h"
#include "core/MuPdfContext.h"
#include "core/UndoManager.h"
#include <filesystem>
#include <memory>

using namespace pdfe::textedit;
using namespace pdfe::core;
using namespace pdfe::commands;

class TextEditCommandTest : public ::testing::Test {
protected:
    void SetUp() override {
        context = std::make_shared<MuPdfContext>();

        std::filesystem::path fixturesDir = TEST_FIXTURES_DIR;
        testPdf = fixturesDir / "test_3page.pdf";

        if (std::filesystem::exists(testPdf)) {
            document = Document::open(context, testPdf);
            undoManager = std::make_unique<UndoManager>();

            // Build a TextEditModel to get span info
            if (document && document->pageCount() > 0) {
                auto page = document->page(0);
                if (page) {
                    auto blocks = page->extractText();
                    if (!blocks.empty()) {
                        model = std::make_unique<TextEditModel>(blocks);
                    }
                }
            }
        }
    }

    void TearDown() override {
        undoManager.reset();
        model.reset();
        document.reset();
        context.reset();
    }

    std::shared_ptr<MuPdfContext> context;
    std::filesystem::path testPdf;
    std::unique_ptr<Document> document;
    std::unique_ptr<UndoManager> undoManager;
    std::unique_ptr<TextEditModel> model;
};

TEST_F(TextEditCommandTest, ConstructorDestructor) {
    if (!document || !model) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    // Verify we have spans to work with
    ASSERT_GT(model->spanCount(), 0) << "Model should have at least one span";

    const auto& spanInfo = model->span(0);
    std::u32string oldText = spanInfo.text;
    std::u32string newText = U"Modified";

    // Construct command without crash
    EXPECT_NO_THROW({
        EditTextCommand cmd(
            document.get(),
            context,
            0,  // pageIndex
            0,  // spanIndex
            spanInfo,
            oldText,
            newText
        );
    });
}

TEST_F(TextEditCommandTest, Description) {
    if (!document || !model) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    ASSERT_GT(model->spanCount(), 0);

    const auto& spanInfo = model->span(0);
    std::u32string oldText = spanInfo.text;
    std::u32string newText = U"Modified";

    EditTextCommand cmd(
        document.get(),
        context,
        0,
        0,
        spanInfo,
        oldText,
        newText
    );

    // Verify description
    std::string desc = cmd.description();
    EXPECT_EQ(desc, "Edit Text");
}

TEST_F(TextEditCommandTest, MergeIdBySpan) {
    if (!document || !model) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    ASSERT_GE(model->spanCount(), 2) << "Need at least 2 spans for this test";

    const auto& span0 = model->span(0);
    const auto& span1 = model->span(1);

    EditTextCommand cmd0(
        document.get(),
        context,
        0,
        0,
        span0,
        span0.text,
        span0.text + U"X"
    );

    EditTextCommand cmd1(
        document.get(),
        context,
        0,
        1,
        span1,
        span1.text,
        span1.text + U"Y"
    );

    // MergeId should be different for different spans
    int mergeId0 = cmd0.mergeId();
    int mergeId1 = cmd1.mergeId();

    EXPECT_NE(mergeId0, mergeId1) << "Different spans should have different merge IDs";
    EXPECT_NE(mergeId0, 0) << "Merge ID should not be zero (merging is enabled)";
}

TEST_F(TextEditCommandTest, MergeSameSpan) {
    if (!document || !model) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    ASSERT_GT(model->spanCount(), 0);

    const auto& spanInfo = model->span(0);
    std::u32string text0 = spanInfo.text;
    std::u32string text1 = text0 + U"A";
    std::u32string text2 = text1 + U"B";

    // Create first command
    auto cmd1 = std::make_unique<EditTextCommand>(
        document.get(),
        context,
        0,
        0,
        spanInfo,
        text0,
        text1
    );

    // Create second command on same span
    auto cmd2 = std::make_unique<EditTextCommand>(
        document.get(),
        context,
        0,
        0,
        spanInfo,
        text1,
        text2
    );

    // Try to merge cmd2 into cmd1
    bool merged = cmd1->mergeWith(cmd2.get());
    EXPECT_TRUE(merged) << "Commands on same span should merge";

    // After merge, cmd1 should have absorbed the final newText
    // The command should now go from text0 directly to text2
    // (we can't easily verify internal state, but we can test that merge succeeded)
}

TEST_F(TextEditCommandTest, MergeDifferentSpan) {
    if (!document || !model) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    ASSERT_GE(model->spanCount(), 2) << "Need at least 2 spans for this test";

    const auto& span0 = model->span(0);
    const auto& span1 = model->span(1);

    // Create commands on different spans
    auto cmd1 = std::make_unique<EditTextCommand>(
        document.get(),
        context,
        0,
        0,
        span0,
        span0.text,
        span0.text + U"X"
    );

    auto cmd2 = std::make_unique<EditTextCommand>(
        document.get(),
        context,
        0,
        1,
        span1,
        span1.text,
        span1.text + U"Y"
    );

    // Try to merge - should fail because different spans
    bool merged = cmd1->mergeWith(cmd2.get());
    EXPECT_FALSE(merged) << "Commands on different spans should not merge";
}

TEST_F(TextEditCommandTest, ExecuteCallsApplyEdits) {
    if (!document || !model) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    ASSERT_GT(model->spanCount(), 0);

    const auto& spanInfo = model->span(0);
    std::u32string oldText = spanInfo.text;
    std::u32string newText = U"TestModified";

    EditTextCommand cmd(
        document.get(),
        context,
        0,
        0,
        spanInfo,
        oldText,
        newText
    );

    // Execute should not crash
    EXPECT_NO_THROW({
        cmd.execute();
    });
}

TEST_F(TextEditCommandTest, UndoManagerIntegration) {
    if (!document || !model) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    ASSERT_GT(model->spanCount(), 0);

    const auto& spanInfo = model->span(0);
    std::u32string oldText = spanInfo.text;
    std::u32string newText = U"Changed";

    // Initially no undo
    EXPECT_FALSE(undoManager->canUndo());
    EXPECT_FALSE(undoManager->canRedo());

    // Execute command via UndoManager
    auto cmd = std::make_unique<EditTextCommand>(
        document.get(),
        context,
        0,
        0,
        spanInfo,
        oldText,
        newText
    );

    undoManager->execute(std::move(cmd));

    // Should now have undo capability
    EXPECT_TRUE(undoManager->canUndo());
    EXPECT_FALSE(undoManager->canRedo());

    // Undo
    undoManager->undo();
    EXPECT_FALSE(undoManager->canUndo());
    EXPECT_TRUE(undoManager->canRedo());

    // Redo
    undoManager->redo();
    EXPECT_TRUE(undoManager->canUndo());
    EXPECT_FALSE(undoManager->canRedo());
}

TEST_F(TextEditCommandTest, CleanStateDirty) {
    if (!document || !model) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    ASSERT_GT(model->spanCount(), 0);

    // Set clean state
    undoManager->setClean();
    EXPECT_TRUE(undoManager->isClean());

    const auto& spanInfo = model->span(0);
    std::u32string oldText = spanInfo.text;
    std::u32string newText = U"Modified";

    // Execute command
    auto cmd = std::make_unique<EditTextCommand>(
        document.get(),
        context,
        0,
        0,
        spanInfo,
        oldText,
        newText
    );

    undoManager->execute(std::move(cmd));

    // Should be dirty now
    EXPECT_FALSE(undoManager->isClean());

    // Undo should return to clean
    undoManager->undo();
    EXPECT_TRUE(undoManager->isClean());
}
