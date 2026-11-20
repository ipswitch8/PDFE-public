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

TEST_F(TextEditToolTest, ClickOutsideCommitsAndStartsNewInsertion) {
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
    int firstSpan = tool->activeSpanIndexForTesting();
    int spansBefore = tool->spanCountForTesting();

    // Bug 9: a left-click on empty page space no longer merely deselects — it
    // commits the previous edit AND begins a brand-new text run at the click,
    // so the user can type into empty space (a blank line / margin). The old
    // behaviour (exit editing, no new run) was exactly the defect.
    QPointF outsidePoint = getPointOutsideText();
    QMouseEvent press2(
        QEvent::MouseButtonPress,
        QPointF(200, 200),
        Qt::LeftButton,
        Qt::LeftButton,
        Qt::NoModifier
    );
    tool->onMousePress(&press2, 0, outsidePoint);

    // Still editing, but now a DIFFERENT (newly-created) span — proving the
    // prior edit committed and a fresh insertion point opened.
    EXPECT_TRUE(tool->isEditing())
        << "Clicking empty space should start a new text run (bug 9).";
    EXPECT_NE(tool->activeSpanIndexForTesting(), firstSpan);
    EXPECT_EQ(tool->spanCountForTesting(), spansBefore + 1)
        << "A new empty span must have been appended to the model.";
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

// GitHub #394 (weak-oracle-hardening, phase-2): formerly this test asserted
// ONLY EXPECT_NO_THROW on onPaintOverlay — it would pass even if the overlay
// painted the caret in the wrong place (or nowhere). It now asserts the painted
// caret GEOMETRY via TextEditTool's existing caretScreenRect() seam (the rect
// the overlay actually recorded in the last paint — not a recomputation):
//   * no caret is recorded before any edit session,
//   * after a click the caret lands inside the clicked span's bounds, and
//   * after typing, the caret RE-SYNCS to the new cursor position (it moves)
//     rather than reporting a stale post-click rect.
// With zoom=1.0 and pageOrigin=(0,0) the overlay's PDF->screen map is identity
// (both Y-down, no flip — see onPaintOverlay::pdfToScreen), so the screen caret
// rect is directly comparable to the span's PDF-space bbox.
//
// Mutation proof (phase-2 closeout): making noteCaretAndDrawPreedit leave
// lastCaretScreenRect_ stale/zero reds the geometry assertions below.
TEST_F(TextEditToolTest, PaintOverlayNoCrash) {
    if (!tool || !model) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }
    ASSERT_GT(model->spanCount(), 0) << "fixture must yield a hit-testable span";

    tool->activate();
    tool->setPage(0);

    // Create a QImage to paint on
    QImage image(800, 600, QImage::Format_ARGB32);
    image.fill(Qt::white);

    QPainter painter(&image);

    // Paint overlay should not crash (even when not editing)...
    EXPECT_NO_THROW({
        tool->onPaintOverlay(painter, 0, 1.0f, QPointF(0, 0), 792.0f);
    });
    // ...and must record NO caret while there is no active edit session.
    QRectF caret;
    EXPECT_FALSE(tool->caretScreenRect(caret))
        << "a caret rect was reported before any span was clicked";

    // Click a span to establish an edit.
    QPointF textPoint = getSpanCenterPoint(0);
    QMouseEvent press(
        QEvent::MouseButtonPress,
        QPointF(100, 100),
        Qt::LeftButton,
        Qt::LeftButton,
        Qt::NoModifier
    );
    tool->onMousePress(&press, 0, textPoint);
    ASSERT_TRUE(tool->isEditing());

    // Paint must not crash AND must record a caret inside the active span.
    EXPECT_NO_THROW({
        tool->onPaintOverlay(painter, 0, 1.0f, QPointF(0, 0), 792.0f);
    });

    const int activeIdx = tool->activeSpanIndexForTesting();
    ASSERT_GE(activeIdx, 0);
    const QRectF spanBbox = tool->spanBboxForTesting(activeIdx);  // zoom=1 => screen==pdf
    ASSERT_FALSE(spanBbox.isNull()) << "active span has no bbox";

    ASSERT_TRUE(tool->caretScreenRect(caret))
        << "onPaintOverlay did not record the painted caret (caretScreenRect seam)";
    const double tol = 2.0;
    EXPECT_TRUE(spanBbox.adjusted(-tol, -tol, tol, tol).contains(caret.center()))
        << "painted caret center (" << caret.center().x() << ',' << caret.center().y()
        << ") is not inside the clicked span bbox [" << spanBbox.left() << ','
        << spanBbox.top() << ' ' << spanBbox.width() << 'x' << spanBbox.height() << ']';
    EXPECT_GT(caret.height(), spanBbox.height() * 0.3)
        << "caret vertical extent collapsed — a stale/zero caret rect";
    const QRectF caretAfterClick = caret;

    // Typing must re-sync the caret to the new cursor position: it must move
    // (not report the stale post-click rect) and stay within the span's row.
    tool->onTextInput("Z");
    EXPECT_NO_THROW({
        tool->onPaintOverlay(painter, 0, 1.0f, QPointF(0, 0), 792.0f);
    });
    ASSERT_TRUE(tool->caretScreenRect(caret))
        << "caret rect lost after typing";
    EXPECT_NE(caret, caretAfterClick)
        << "caret did not move after typing — overlay reported a stale caret rect";
    EXPECT_GE(caret.center().x(), spanBbox.left() - tol)
        << "post-type caret jumped left of the span";
    EXPECT_GE(caret.center().y(), spanBbox.top() - tol);
    EXPECT_LE(caret.center().y(), spanBbox.bottom() + tol)
        << "post-type caret left the span's vertical row — stale/zero rect";

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
