#include <gtest/gtest.h>
#include "textedit/TextEditTool.h"
#include "textedit/TextEditModel.h"
#include "core/Document.h"
#include "core/Page.h"
#include "core/MuPdfContext.h"
#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QEventLoop>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QThread>
#include <filesystem>
#include <memory>
#include <atomic>

namespace {

/// Write to the Qt clipboard with a verify-then-retry loop. Under
/// Windows RDP sessions the clipboard ownership flips transiently to
/// the session manager, so a single setText() can appear to drop the
/// value. Retrying a few times with processEvents() absorbs the race.
bool rdpSafeSetClipboardText(QClipboard* cb, const QString& text,
                             int maxAttempts = 5, int delayMs = 20) {
    if (!cb) return false;
    for (int i = 0; i < maxAttempts; ++i) {
        cb->setText(text);
        QCoreApplication::processEvents(
            QEventLoop::ExcludeUserInputEvents, delayMs);
        if (cb->text() == text) return true;
        if (i + 1 < maxAttempts) QThread::msleep(delayMs);
    }
    return false;
}

/// Read from the Qt clipboard with retries. Returns the first
/// non-empty value within the retry budget, or empty.
QString rdpSafeClipboardText(QClipboard* cb, int maxAttempts = 5,
                             int delayMs = 20) {
    if (!cb) return {};
    for (int i = 0; i < maxAttempts; ++i) {
        QString s = cb->text();
        if (!s.isEmpty()) return s;
        if (i + 1 < maxAttempts) {
            QCoreApplication::processEvents(
                QEventLoop::ExcludeUserInputEvents, delayMs);
            QThread::msleep(delayMs);
        }
    }
    return {};
}

} // namespace

using namespace pdfe::textedit;
using namespace pdfe::core;

class TextEditClipboardTest : public ::testing::Test {
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

        // Get clipboard instance
        clipboard = QApplication::clipboard();
    }

    void TearDown() override {
        // Clear clipboard after tests
        if (clipboard) {
            clipboard->clear();
        }

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

    // Helper: Click on a text span to select it
    void clickOnSpan(int spanIndex) {
        QPointF textPoint = getSpanCenterPoint(spanIndex);
        QMouseEvent press(
            QEvent::MouseButtonPress,
            QPointF(100, 100),
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier
        );
        tool->onMousePress(&press, 0, textPoint);
    }

    // Helper: Send a key event
    void sendKey(Qt::Key key, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QKeyEvent event(QEvent::KeyPress, key, modifiers);
        tool->onKeyPress(&event);
    }

    std::shared_ptr<MuPdfContext> context;
    std::filesystem::path testPdf;
    std::unique_ptr<Document> document;
    std::unique_ptr<TextEditTool> tool;
    std::unique_ptr<TextEditModel> model;
    QClipboard* clipboard = nullptr;
};

TEST_F(TextEditClipboardTest, CopyAfterSelectAll) {
    if (!tool || !model || !clipboard) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    ASSERT_GT(model->spanCount(), 0) << "Need at least one text span";

    // Activate and set page
    tool->activate();
    tool->setPage(0);

    // Click on first span to select it
    clickOnSpan(0);
    ASSERT_TRUE(tool->isEditing()) << "Should be in editing mode";

    // Clear clipboard first
    clipboard->clear();

    // Send Ctrl+A (Select All)
    sendKey(Qt::Key_A, Qt::ControlModifier);

    // Send Ctrl+C (Copy)
    sendKey(Qt::Key_C, Qt::ControlModifier);

    // Verify clipboard has text — retry under RDP races.
    QString clipboardText = rdpSafeClipboardText(clipboard);
    EXPECT_FALSE(clipboardText.isEmpty()) << "Clipboard should contain text after copy";

    // Clipboard should contain some of the span's text
    const auto& span = model->span(0);
    QString spanText = QString::fromStdU32String(span.text);

    // After select all + copy, clipboard should match the span text
    EXPECT_EQ(clipboardText, spanText) << "Clipboard should match selected span text";
}

TEST_F(TextEditClipboardTest, CutRemovesText) {
    if (!tool || !model || !clipboard) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    ASSERT_GT(model->spanCount(), 0);

    // Track callback
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

    // Get original text
    const auto& originalSpan = model->span(0);
    std::u32string originalText = originalSpan.text;

    // Click on span
    clickOnSpan(0);
    ASSERT_TRUE(tool->isEditing());

    // Clear clipboard
    clipboard->clear();

    // Send Ctrl+A (Select All)
    sendKey(Qt::Key_A, Qt::ControlModifier);

    // Send Ctrl+X (Cut)
    sendKey(Qt::Key_X, Qt::ControlModifier);

    // Clipboard should have text — retry under RDP races.
    QString clipboardText = rdpSafeClipboardText(clipboard);
    EXPECT_FALSE(clipboardText.isEmpty()) << "Clipboard should contain text after cut";

    // Commit the edit
    tool->commitEdit();

    // Callback should fire with modified text
    EXPECT_TRUE(callbackFired) << "Cut should modify text and trigger callback";
    EXPECT_EQ(capturedOldText, originalText) << "Old text should match original";
    EXPECT_NE(capturedNewText, capturedOldText) << "New text should differ (text was cut)";

    // New text should be empty or much shorter (cut removes selected text)
    EXPECT_LT(capturedNewText.size(), capturedOldText.size())
        << "Cut should remove text";
}

TEST_F(TextEditClipboardTest, PasteInsertsText) {
    if (!tool || !model || !clipboard) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    ASSERT_GT(model->spanCount(), 0);

    // Track callback
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

    // Get original text
    const auto& originalSpan = model->span(0);
    std::u32string originalText = originalSpan.text;

    // Set clipboard to known text — use RDP-safe verified write.
    QString pasteText = "PASTED_TEXT";
    ASSERT_TRUE(rdpSafeSetClipboardText(clipboard, pasteText))
        << "Clipboard setText did not verify — test env issue";

    // Click on span
    clickOnSpan(0);
    ASSERT_TRUE(tool->isEditing());

    // Send Ctrl+V (Paste)
    sendKey(Qt::Key_V, Qt::ControlModifier);

    // Commit the edit
    tool->commitEdit();

    // Callback should fire with modified text
    EXPECT_TRUE(callbackFired) << "Paste should modify text and trigger callback";
    EXPECT_EQ(capturedOldText, originalText) << "Old text should match original";
    EXPECT_NE(capturedNewText, capturedOldText) << "New text should differ (text was pasted)";

    // New text should contain the pasted text
    QString newTextQString = QString::fromStdU32String(capturedNewText);
    EXPECT_TRUE(newTextQString.contains(pasteText))
        << "New text should contain pasted content";
}

TEST_F(TextEditClipboardTest, CopyWithNoSelection) {
    if (!tool || !model || !clipboard) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    ASSERT_GT(model->spanCount(), 0);

    tool->activate();
    tool->setPage(0);

    // Click on span but don't select anything
    clickOnSpan(0);
    ASSERT_TRUE(tool->isEditing());

    // Set clipboard to known text — use RDP-safe write that verifies
    // the value actually landed before we proceed.
    QString initialClipboard = "INITIAL_CONTENT";
    ASSERT_TRUE(rdpSafeSetClipboardText(clipboard, initialClipboard))
        << "Clipboard setText did not verify under RDP race — test env issue";

    // Send Ctrl+C (Copy) without selecting anything
    // (cursor is just positioned, no selection)
    sendKey(Qt::Key_C, Qt::ControlModifier);

    // Clipboard should either remain unchanged or contain the whole span
    // depending on implementation (if no selection, some editors copy line/span)
    QString clipboardText = rdpSafeClipboardText(clipboard);

    // The clipboard should have SOME content (either unchanged or span text)
    EXPECT_FALSE(clipboardText.isEmpty()) << "Clipboard should have content";

    // Most implementations: if no selection, copy does nothing or copies whole span
    // We just verify it doesn't crash and clipboard is not corrupted
    EXPECT_NO_THROW({
        QString test = rdpSafeClipboardText(clipboard);
    }) << "Clipboard should remain valid after copy with no selection";
}

TEST_F(TextEditClipboardTest, PasteWithEmptyClipboard) {
    if (!tool || !model || !clipboard) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    ASSERT_GT(model->spanCount(), 0);

    // Track callback
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

    // Get original text
    const auto& originalSpan = model->span(0);
    std::u32string originalText = originalSpan.text;

    // Clear clipboard
    clipboard->clear();
    ASSERT_TRUE(clipboard->text().isEmpty()) << "Clipboard should be empty";

    // Click on span
    clickOnSpan(0);
    ASSERT_TRUE(tool->isEditing());

    // Send Ctrl+V (Paste) with empty clipboard
    // This should not crash
    EXPECT_NO_THROW({
        sendKey(Qt::Key_V, Qt::ControlModifier);
    }) << "Paste with empty clipboard should not crash";

    // Commit
    tool->commitEdit();

    // If clipboard is empty, text should remain unchanged
    // (callback may or may not fire depending on implementation)
    if (callbackFired) {
        // If callback fired, new text should equal old text (no change)
        EXPECT_EQ(capturedNewText, capturedOldText)
            << "Pasting empty clipboard should not modify text";
    }

    // Main goal: verify no crash occurred
    SUCCEED() << "Paste with empty clipboard completed without crash";
}
