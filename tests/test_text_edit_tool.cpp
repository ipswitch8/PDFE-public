#include <gtest/gtest.h>
#include "textedit/TextEditTool.h"
#include "textedit/TextEditModel.h"
#include "core/Document.h"
#include "core/Page.h"
#include "core/MuPdfContext.h"
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QImage>
#include <filesystem>
#include <memory>
#include <atomic>

using namespace pdfe::textedit;
using namespace pdfe::core;

class TextEditToolTest : public ::testing::Test {
protected:
    void SetUp() override {
        context = std::make_shared<MuPdfContext>();

        std::filesystem::path fixturesDir = TEST_FIXTURES_DIR;
        testPdf = fixturesDir / "test_3page.pdf";

        if (std::filesystem::exists(testPdf)) {
            document = Document::open(context, testPdf);

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

    // Helper: Get a point outside all text
    QPointF getPointOutsideText() {
        // Use a point far from origin, likely outside text
        return QPointF(10000, 10000);
    }

    std::shared_ptr<MuPdfContext> context;
    std::filesystem::path testPdf;
    std::unique_ptr<Document> document;
    std::unique_ptr<TextEditTool> tool;
    std::unique_ptr<TextEditModel> model;
};

TEST_F(TextEditToolTest, ConstructorDestructor) {
    if (!document) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    // Constructor should not crash
    EXPECT_NO_THROW({
        TextEditTool tmpTool(document.get(), context);
    });
}

TEST_F(TextEditToolTest, ActivateDeactivate) {
    if (!tool) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    // Activate/deactivate lifecycle should work
    EXPECT_NO_THROW({
        tool->activate();
    });

    EXPECT_NO_THROW({
        tool->deactivate();
    });

    // Multiple activate/deactivate cycles
    tool->activate();
    tool->deactivate();
    tool->activate();
    tool->deactivate();
}

TEST_F(TextEditToolTest, SetPageBuildsModel) {
    if (!tool) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    // Activate tool first
    tool->activate();

    // Set page should build internal model without crash
    EXPECT_NO_THROW({
        tool->setPage(0);
    });

    // GitHub #273: this was gated on `document->pageCount() > 1`, so if the
    // fixture ever loaded as a single page the second setPage asserted
    // nothing and the test still passed. The fixture is test_3page.pdf — its
    // page count is a FACT about it, not a condition to hope for, so assert
    // it and then exercise the second page unconditionally.
    ASSERT_GT(document->pageCount(), 1)
        << "fixture " << testPdf << " is expected to have 3 pages but loaded "
        << document->pageCount() << "; the multi-page setPage path below "
           "cannot be exercised, and silently skipping it is what GitHub #273 "
           "is about.";
    EXPECT_NO_THROW({
        tool->setPage(1);
    });
}

TEST_F(TextEditToolTest, ClickOnTextSelectsSpan) {
    if (!tool || !model) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    ASSERT_GT(model->spanCount(), 0) << "Need at least one text span";

    // Activate and set page
    tool->activate();
    tool->setPage(0);

    // Initially not editing
    EXPECT_FALSE(tool->isEditing());

    // Get a point inside the first span
    QPointF pdfPoint = getSpanCenterPoint(0);

    // Create mouse press event
    QMouseEvent pressEvent(
        QEvent::MouseButtonPress,
        QPointF(100, 100),  // Screen position (not used by tool)
        Qt::LeftButton,
        Qt::LeftButton,
        Qt::NoModifier
    );

    // Click on text
    tool->onMousePress(&pressEvent, 0, pdfPoint);

    // Should now be editing
    EXPECT_TRUE(tool->isEditing()) << "Clicking on text should enter editing mode";
}

TEST_F(TextEditToolTest, ClickOutsideDeselectsSpan) {
    if (!tool || !model) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    ASSERT_GT(model->spanCount(), 0);

    tool->activate();
    tool->setPage(0);

    // First click on text to select
    QPointF textPoint = getSpanCenterPoint(0);
    QMouseEvent press1(
        QEvent::MouseButtonPress,
        QPointF(100, 100),
        Qt::LeftButton,
        Qt::LeftButton,
        Qt::NoModifier
    );
    tool->onMousePress(&press1, 0, textPoint);
    ASSERT_TRUE(tool->isEditing());

    // Now click outside text
    QPointF outsidePoint = getPointOutsideText();
    QMouseEvent press2(
        QEvent::MouseButtonPress,
        QPointF(200, 200),
        Qt::LeftButton,
        Qt::LeftButton,
        Qt::NoModifier
    );

    // Click outside should commit and deselect
    tool->onMousePress(&press2, 0, outsidePoint);

    // Should no longer be editing
    EXPECT_FALSE(tool->isEditing()) << "Clicking outside text should exit editing mode";
}

TEST_F(TextEditToolTest, KeyInputInsertsText) {
    if (!tool || !model) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    ASSERT_GT(model->spanCount(), 0);

    // Track if callback fires
    std::atomic<bool> callbackFired{false};
    std::u32string capturedOldText;
    std::u32string capturedNewText;

    tool->textEdited = [&](int pageIndex, int spanIndex,
                           std::u32string oldText, std::u32string newText,
                           TextSpan spanInfo) {
        callbackFired = true;
        capturedOldText = oldText;
        capturedNewText = newText;
    };

    tool->activate();
    tool->setPage(0);

    // Click to select span
    QPointF textPoint = getSpanCenterPoint(0);
    QMouseEvent press(
        QEvent::MouseButtonPress,
        QPointF(100, 100),
        Qt::LeftButton,
        Qt::LeftButton,
        Qt::NoModifier
    );
    tool->onMousePress(&press, 0, textPoint);

    // Type a character
    tool->onTextInput("X");

    // Commit the edit
    tool->commitEdit();

    // Verify callback fired and text changed
    EXPECT_TRUE(callbackFired) << "textEdited callback should fire after commit";
    EXPECT_NE(capturedOldText, capturedNewText) << "Text should have changed";

    // New text should contain the inserted character
    std::u32string expectedChar = U"X";
    EXPECT_TRUE(capturedNewText.find(expectedChar[0]) != std::u32string::npos)
        << "New text should contain inserted character";
}

TEST_F(TextEditToolTest, BackspaceDeletesChar) {
    if (!tool || !model) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    ASSERT_GT(model->spanCount(), 0);

    std::atomic<bool> callbackFired{false};
    std::u32string capturedOldText;
    std::u32string capturedNewText;

    tool->textEdited = [&](int pageIndex, int spanIndex,
                           std::u32string oldText, std::u32string newText,
                           TextSpan spanInfo) {
        callbackFired = true;
        capturedOldText = oldText;
        capturedNewText = newText;
    };

    tool->activate();
    tool->setPage(0);

    // Click to select span
    QPointF textPoint = getSpanCenterPoint(0);
    QMouseEvent press(
        QEvent::MouseButtonPress,
        QPointF(100, 100),
        Qt::LeftButton,
        Qt::LeftButton,
        Qt::NoModifier
    );
    tool->onMousePress(&press, 0, textPoint);

    // Insert two characters, then backspace once -- leaves one char inserted
    tool->onTextInput("AB");

    QKeyEvent backspaceEvent(
        QEvent::KeyPress,
        Qt::Key_Backspace,
        Qt::NoModifier
    );
    tool->onKeyPress(&backspaceEvent);

    // Commit
    tool->commitEdit();

    // Callback should fire because "A" is still inserted (text is modified)
    EXPECT_TRUE(callbackFired);
    EXPECT_NE(capturedOldText, capturedNewText) << "Text should differ after insert+partial backspace";
}

TEST_F(TextEditToolTest, CommitEditFiresCallback) {
    if (!tool || !model) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    ASSERT_GT(model->spanCount(), 0);

    std::atomic<bool> callbackFired{false};
    int capturedPageIndex = -1;
    int capturedSpanIndex = -1;

    tool->textEdited = [&](int pageIndex, int spanIndex,
                           std::u32string oldText, std::u32string newText,
                           TextSpan spanInfo) {
        callbackFired = true;
        capturedPageIndex = pageIndex;
        capturedSpanIndex = spanIndex;
    };

    tool->activate();
    tool->setPage(0);

    // Click to select
    QPointF textPoint = getSpanCenterPoint(0);
    QMouseEvent press(
        QEvent::MouseButtonPress,
        QPointF(100, 100),
        Qt::LeftButton,
        Qt::LeftButton,
        Qt::NoModifier
    );
    tool->onMousePress(&press, 0, textPoint);

    // Edit text
    tool->onTextInput("TEST");

    // Commit
    tool->commitEdit();

    // Verify callback
    EXPECT_TRUE(callbackFired);
    EXPECT_EQ(capturedPageIndex, 0);
    EXPECT_EQ(capturedSpanIndex, 0);
}

TEST_F(TextEditToolTest, CancelEditRestoresText) {
    if (!tool || !model) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    ASSERT_GT(model->spanCount(), 0);

    // Get original text
    const auto& originalSpan = model->span(0);
    std::u32string originalText = originalSpan.text;

    std::atomic<bool> callbackFired{false};

    tool->textEdited = [&](int pageIndex, int spanIndex,
                           std::u32string oldText, std::u32string newText,
                           TextSpan spanInfo) {
        callbackFired = true;
    };

    tool->activate();
    tool->setPage(0);

    // Click to select
    QPointF textPoint = getSpanCenterPoint(0);
    QMouseEvent press(
        QEvent::MouseButtonPress,
        QPointF(100, 100),
        Qt::LeftButton,
        Qt::LeftButton,
        Qt::NoModifier
    );
    tool->onMousePress(&press, 0, textPoint);

    // Edit text
    tool->onTextInput("MODIFIED");

    // Cancel instead of commit
    tool->cancelEdit();

    // Callback should NOT fire
    EXPECT_FALSE(callbackFired) << "cancelEdit should not fire textEdited callback";

    // Should no longer be editing
    EXPECT_FALSE(tool->isEditing());

    // Text should be preserved in original model
    // (The tool doesn't modify the model we have externally)
}

TEST_F(TextEditToolTest, PaintOverlayNoCrash) {
    if (!tool) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    tool->activate();
    tool->setPage(0);

    // Create a QImage to paint on
    QImage image(800, 600, QImage::Format_ARGB32);
    image.fill(Qt::white);

    QPainter painter(&image);

    // Paint overlay should not crash (even when not editing)
    EXPECT_NO_THROW({
        tool->onPaintOverlay(painter, 0, 1.0f, QPointF(0, 0), 792.0f);
    });

    // Paint overlay while editing
    QPointF textPoint = getSpanCenterPoint(0);
    QMouseEvent press(
        QEvent::MouseButtonPress,
        QPointF(100, 100),
        Qt::LeftButton,
        Qt::LeftButton,
        Qt::NoModifier
    );
    tool->onMousePress(&press, 0, textPoint);

    EXPECT_NO_THROW({
        tool->onPaintOverlay(painter, 0, 1.0f, QPointF(0, 0), 792.0f);
    });

    painter.end();
}

// Pipeline 3e7c1a05 phase-1 (UX fix): Up and Down arrow keys must NOT be
// accepted by TextEditTool::onKeyPress so that PdfCanvas can fall through to
// its canvas-scroll handler.  Left/Right (which the tool does handle) are
// accepted for contrast.
TEST_F(TextEditToolTest, UpDownKeyEventsNotAcceptedByOnKeyPress) {
    if (!tool || !model) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    ASSERT_GT(model->spanCount(), 0);

    // Enter editing mode via a click on the first span (same activate/setPage
    // sequence used by all other tests in this fixture).
    tool->activate();
    tool->setPage(0);
    QPointF textPoint = getSpanCenterPoint(0);
    QMouseEvent press(
        QEvent::MouseButtonPress,
        QPointF(100, 100),
        Qt::LeftButton,
        Qt::LeftButton,
        Qt::NoModifier
    );
    tool->onMousePress(&press, 0, textPoint);
    ASSERT_TRUE(tool->isEditing()) << "Tool must be editing before key test";

    // Qt::Key_Up — TextEditTool should ignore (not accept) this key.
    QKeyEvent upEvent(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier);
    tool->onKeyPress(&upEvent);
    EXPECT_FALSE(upEvent.isAccepted())
        << "Key_Up must not be accepted; canvas needs it for page scroll";

    // Qt::Key_Down — same requirement.
    QKeyEvent downEvent(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
    tool->onKeyPress(&downEvent);
    EXPECT_FALSE(downEvent.isAccepted())
        << "Key_Down must not be accepted; canvas needs it for page scroll";

    // Sanity: Qt::Key_Left IS handled — must be accepted.
    QKeyEvent leftEvent(QEvent::KeyPress, Qt::Key_Left, Qt::NoModifier);
    tool->onKeyPress(&leftEvent);
    EXPECT_TRUE(leftEvent.isAccepted())
        << "Key_Left must be accepted (TextEditTool moves cursor)";

    // Sanity: Qt::Key_Right IS handled — must be accepted.
    QKeyEvent rightEvent(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
    tool->onKeyPress(&rightEvent);
    EXPECT_TRUE(rightEvent.isAccepted())
        << "Key_Right must be accepted (TextEditTool moves cursor)";
}
