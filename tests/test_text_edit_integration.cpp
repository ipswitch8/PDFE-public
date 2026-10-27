#include <gtest/gtest.h>
#include "textedit/TextEditTool.h"
#include "textedit/TextEditCommands.h"
#include "textedit/TextEditModel.h"
#include "textedit/ContentStreamEditor.h"
#include "core/Document.h"
#include "core/Page.h"
#include "core/MuPdfContext.h"
#include "core/UndoManager.h"
#include <QMouseEvent>
#include <QKeyEvent>
#include <filesystem>
#include <memory>
#include <atomic>

using namespace pdfe::textedit;
using namespace pdfe::core;
using namespace pdfe::commands;

class TextEditIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        context = std::make_shared<MuPdfContext>();

        std::filesystem::path fixturesDir = TEST_FIXTURES_DIR;
        testPdf = fixturesDir / "test_3page.pdf";

        if (std::filesystem::exists(testPdf)) {
            document = Document::open(context, testPdf);
            undoManager = std::make_unique<UndoManager>();

            if (document && document->pageCount() > 0) {
                tool = std::make_unique<TextEditTool>(document.get(), context);

                // Build a model to find text positions
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
        tool.reset();
        undoManager.reset();
        model.reset();
        document.reset();
        context.reset();
    }

    // Helper: Get center point of a span's bounding box
    QPointF getSpanCenterPoint(int spanIndex) {
        if (!model || spanIndex >= model->spanCount()) {
            return QPointF(0, 0);
        }
        const auto& span = model->span(spanIndex);
        return span.boundingBox.center();
    }

    std::shared_ptr<MuPdfContext> context;
    std::filesystem::path testPdf;
    std::unique_ptr<Document> document;
    std::unique_ptr<UndoManager> undoManager;
    std::unique_ptr<TextEditTool> tool;
    std::unique_ptr<TextEditModel> model;
};

TEST_F(TextEditIntegrationTest, FullEditWorkflow) {
    if (!tool || !model) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    ASSERT_GT(model->spanCount(), 0) << "Need at least one text span";

    // Track callback
    std::atomic<bool> callbackFired{false};
    int capturedPageIndex = -1;
    int capturedSpanIndex = -1;
    std::u32string capturedOldText;
    std::u32string capturedNewText;

    tool->textEdited = [&](int pageIndex, int spanIndex,
                           std::u32string oldText, std::u32string newText,
                           TextSpan spanInfo) {
        callbackFired = true;
        capturedPageIndex = pageIndex;
        capturedSpanIndex = spanIndex;
        capturedOldText = oldText;
        capturedNewText = newText;
    };

    // Activate tool
    tool->activate();

    // Set page
    tool->setPage(0);
    EXPECT_EQ(tool->currentPageIndex(), 0);

    // Click on text to select span
    QPointF textPoint = getSpanCenterPoint(0);
    QMouseEvent press(
        QEvent::MouseButtonPress,
        QPointF(100, 100),
        Qt::LeftButton,
        Qt::LeftButton,
        Qt::NoModifier
    );
    tool->onMousePress(&press, 0, textPoint);

    EXPECT_TRUE(tool->isEditing());

    // Type replacement text
    tool->onTextInput("REPLACEMENT");

    // Commit edit
    tool->commitEdit();

    // Verify callback fired
    EXPECT_TRUE(callbackFired) << "textEdited callback should fire";
    EXPECT_EQ(capturedPageIndex, 0);
    EXPECT_EQ(capturedSpanIndex, 0);
    EXPECT_NE(capturedOldText, capturedNewText) << "Text should have changed";

    // Should no longer be editing after commit
    EXPECT_FALSE(tool->isEditing());
}

TEST_F(TextEditIntegrationTest, UndoRedoCycle) {
    if (!document || !model) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    ASSERT_GT(model->spanCount(), 0);

    // Get span info for creating command
    const auto& spanInfo = model->span(0);
    std::u32string oldText = spanInfo.text;
    std::u32string newText = U"EditedText";

    // Create and execute command via UndoManager
    auto cmd = std::make_unique<EditTextCommand>(
        document.get(),
        context,
        0,
        0,
        spanInfo,
        oldText,
        newText
    );

    // Initially no undo/redo
    EXPECT_FALSE(undoManager->canUndo());
    EXPECT_FALSE(undoManager->canRedo());

    // Execute
    undoManager->execute(std::move(cmd));

    // Should have undo capability
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

TEST_F(TextEditIntegrationTest, MultiplePageEdits) {
    if (!tool || !document) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    if (document->pageCount() < 2) {
        GTEST_SKIP() << "Need at least 2 pages for multi-page test";
    }

    tool->activate();

    // Edit on page 0
    tool->setPage(0);
    EXPECT_EQ(tool->currentPageIndex(), 0);

    // Build model for page 0
    auto page0 = document->page(0);
    auto blocks0 = page0->extractText();
    if (!blocks0.empty()) {
        auto model0 = std::make_unique<TextEditModel>(blocks0);
        if (model0->spanCount() > 0) {
            QPointF point0 = model0->span(0).boundingBox.center();
            QMouseEvent press0(
                QEvent::MouseButtonPress,
                QPointF(100, 100),
                Qt::LeftButton,
                Qt::LeftButton,
                Qt::NoModifier
            );
            tool->onMousePress(&press0, 0, point0);
            tool->onTextInput("PAGE0");
            tool->commitEdit();
        }
    }

    // Switch to page 1
    tool->setPage(1);
    EXPECT_EQ(tool->currentPageIndex(), 1);

    // Edit on page 1
    auto page1 = document->page(1);
    auto blocks1 = page1->extractText();
    if (!blocks1.empty()) {
        auto model1 = std::make_unique<TextEditModel>(blocks1);
        if (model1->spanCount() > 0) {
            QPointF point1 = model1->span(0).boundingBox.center();
            QMouseEvent press1(
                QEvent::MouseButtonPress,
                QPointF(200, 200),
                Qt::LeftButton,
                Qt::LeftButton,
                Qt::NoModifier
            );
            tool->onMousePress(&press1, 1, point1);
            tool->onTextInput("PAGE1");
            tool->commitEdit();
        }
    }

    // Successfully edited on both pages without crash
}

TEST_F(TextEditIntegrationTest, SaveAfterEdit) {
    if (!document || !model) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    ASSERT_GT(model->spanCount(), 0);

    // NOTE: This test may need adjustment for Phase 6a encoding issues
    // For now, we test the workflow without verifying exact text preservation

    const auto& spanInfo = model->span(0);
    std::u32string oldText = spanInfo.text;
    std::u32string newText = U"SaveTest";

    // Execute edit
    auto cmd = std::make_unique<EditTextCommand>(
        document.get(),
        context,
        0,
        0,
        spanInfo,
        oldText,
        newText
    );
    cmd->execute();

    // Save to temp file
    std::filesystem::path tempDir = std::filesystem::temp_directory_path();
    std::filesystem::path tempPdf = tempDir / "pdfe_test_save_after_edit.pdf";

    // Remove if exists
    if (std::filesystem::exists(tempPdf)) {
        std::filesystem::remove(tempPdf);
    }

    // Save document
    EXPECT_NO_THROW({
        document->save(tempPdf);
    });

    EXPECT_TRUE(std::filesystem::exists(tempPdf)) << "Saved PDF should exist";

    // Reopen the saved document
    std::unique_ptr<Document> reopenedDoc;
    EXPECT_NO_THROW({
        reopenedDoc = Document::open(context, tempPdf);
    });

    ASSERT_NE(reopenedDoc, nullptr);
    EXPECT_GT(reopenedDoc->pageCount(), 0);

    // Extract text from reopened document
    auto reopenedPage = reopenedDoc->page(0);
    auto reopenedBlocks = reopenedPage->extractText();

    // Verify we can extract text (actual content verification may need
    // adjustment for encoding issues in Phase 6a)
    EXPECT_FALSE(reopenedBlocks.empty());

    // Cleanup -- reset documents before removing temp file
    // On Windows, QPDF may hold file handles until fully destroyed
    reopenedDoc.reset();
    try {
        std::filesystem::remove(tempPdf);
    } catch (const std::filesystem::filesystem_error&) {
        // Best-effort cleanup on Windows
    }
}

TEST_F(TextEditIntegrationTest, DirtyFlagAfterEdit) {
    if (!document || !model) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    ASSERT_GT(model->spanCount(), 0);

    // UndoManager starts clean
    undoManager->setClean();
    EXPECT_TRUE(undoManager->isClean());

    const auto& spanInfo = model->span(0);
    std::u32string oldText = spanInfo.text;
    std::u32string newText = U"DirtyTest";

    // Execute edit via UndoManager
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

    // Should be dirty after edit
    EXPECT_FALSE(undoManager->isClean()) << "UndoManager should be dirty after edit";

    // Set clean
    undoManager->setClean();
    EXPECT_TRUE(undoManager->isClean());

    // Undo should make it dirty again (moving away from clean position)
    undoManager->undo();
    EXPECT_FALSE(undoManager->isClean());

    // Redo should return to clean
    undoManager->redo();
    EXPECT_TRUE(undoManager->isClean());
}
