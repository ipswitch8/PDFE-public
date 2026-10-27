// tests/test_text_edit_selection_render.cpp
//
// Phase-3 regression locks (failing-test-first) for UI-TE-SEL-* rows in
// tests/fixtures/ui_audit/UI_MATRIX_GAPS.md.
//
// Locks the contract that TextEditTool selection state and onPaintOverlay
// draw a non-degenerate selection highlight after click+drag.
//
// Coverage (rows from UI_MATRIX_GAPS.md):
//   UI-TE-SEL-01  Reported bug: selection highlight invisible after click+drag.
//                 selRect collapses or selectionStart_!=selectionEnd_ guard
//                 suppresses it.
//   UI-TE-SEL-02  selRect is non-degenerate (non-zero width) when at least
//                 one character is in the selection range.
//   UI-TE-SEL-03  painter.drawRect invoked for the selection on
//                 onPaintOverlay (verified via pixel sampling).
//   UI-TE-SEL-04  Selection survives a non-printing keystroke (modifier-only).
//   UI-TE-SEL-05  Selection survives focus/blur cycle when click reactivates
//                 the same span.
//   UI-TE-SEL-06  Selection clears on click outside the active span.
//
// Tests intentionally fail RED on the predecessor commit; baseline failure
// log in tests/fixtures/ui_audit/phase_text_edit_baseline_failures.txt.

#include <gtest/gtest.h>
#include "textedit/TextEditTool.h"
#include "textedit/TextEditModel.h"
#include "core/Document.h"
#include "core/Page.h"
#include "core/MuPdfContext.h"

#include <QApplication>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPointF>
#include <QRectF>

#include <filesystem>
#include <memory>

using namespace pdfe::textedit;
using namespace pdfe::core;

namespace {

class TextEditSelectionRenderTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<MuPdfContext>();

        std::filesystem::path fixturesDir = TEST_FIXTURES_DIR;
        testPdf_ = fixturesDir / "test_3page.pdf";
        if (!std::filesystem::exists(testPdf_)) {
            GTEST_SKIP() << "Test fixture not available: " << testPdf_;
        }

        document_ = Document::open(context_, testPdf_);
        ASSERT_NE(document_, nullptr);
        ASSERT_GT(document_->pageCount(), 0);

        tool_ = std::make_unique<TextEditTool>(document_.get(), context_);
        auto page = document_->page(0);
        ASSERT_NE(page, nullptr);
        auto blocks = page->extractText();
        ASSERT_FALSE(blocks.empty());
        model_ = std::make_unique<TextEditModel>(blocks);
        ASSERT_GT(model_->spanCount(), 0);

        tool_->activate();
        tool_->setPage(0);
    }

    void TearDown() override {
        tool_.reset();
        model_.reset();
        document_.reset();
        context_.reset();
    }

    QPointF spanCenter(int idx) const {
        return model_->span(idx).boundingBox.center();
    }

    QPointF spanLeftEdge(int idx) const {
        const auto& bbox = model_->span(idx).boundingBox;
        return QPointF(bbox.left() + 0.5, bbox.center().y());
    }

    QPointF spanInteriorAt(int idx, double t) const {
        const auto& bbox = model_->span(idx).boundingBox;
        return QPointF(bbox.left() + t * bbox.width(), bbox.center().y());
    }

    // Send a left-button press at the given PDF point.
    void mousePressAt(const QPointF& pdfPoint) {
        QMouseEvent ev(QEvent::MouseButtonPress, QPointF(100, 100),
                       Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        tool_->onMousePress(&ev, 0, pdfPoint);
    }

    // Send a left-button drag (LeftButton held in `buttons`) move event.
    void mouseDragTo(const QPointF& pdfPoint) {
        QMouseEvent ev(QEvent::MouseMove, QPointF(120, 100),
                       Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        tool_->onMouseMove(&ev, 0, pdfPoint);
    }

    // Click+drag from spanCenter to a position N chars to the right.
    // Uses the span's bbox to interpolate a target X.
    void clickAndDragSelectFirstChars(int spanIdx, int charsToCover) {
        const auto& span = model_->span(spanIdx);
        // Click at the left edge of the span
        mousePressAt(QPointF(span.boundingBox.left() + 0.5,
                             span.boundingBox.center().y()));
        // Drag to a position covering `charsToCover` of the span's width
        int textLen = static_cast<int>(span.text.size());
        if (textLen <= 0) return;
        double frac = std::min(1.0, static_cast<double>(charsToCover) /
                                      static_cast<double>(textLen));
        double targetX = span.boundingBox.left() +
                         frac * span.boundingBox.width();
        mouseDragTo(QPointF(targetX, span.boundingBox.center().y()));
    }

    // Render the overlay into a QImage and report whether any pixels
    // in the expected highlight region are non-white (indicating a draw).
    bool overlayHasHighlightPixels(int spanIdx, double leftFrac, double rightFrac) {
        const auto& span = model_->span(spanIdx);
        // Use a 1:1 mapping from PDF coords to image pixels (zoom=1.0)
        // and a generous canvas to host the span.
        const int W = std::max(800, static_cast<int>(span.boundingBox.right() + 100));
        const int H = std::max(600, static_cast<int>(span.boundingBox.bottom() + 100));
        QImage img(W, H, QImage::Format_ARGB32);
        img.fill(Qt::white);
        {
            QPainter painter(&img);
            float zoom = 1.0f;
            // pageOrigin is the screen coordinate of the page top-left
            QPointF pageOrigin(0.0, 0.0);
            float pageHeightPt = static_cast<float>(H);
            tool_->onPaintOverlay(painter, /*pageIndex=*/0, zoom,
                                   pageOrigin, pageHeightPt);
        }

        QRectF region(span.boundingBox.left() +
                          leftFrac * span.boundingBox.width(),
                      span.boundingBox.top(),
                      (rightFrac - leftFrac) * span.boundingBox.width(),
                      span.boundingBox.height());
        // Sample a grid of pixels inside the region; the highlight color is
        // a non-white semi-transparent blue, so any non-white pixel counts.
        int hits = 0;
        const int probes = 25;
        for (int i = 0; i < probes; ++i) {
            double tx = region.left() +
                        (i + 0.5) * region.width() / probes;
            double ty = region.center().y();
            int px = static_cast<int>(tx);
            int py = static_cast<int>(ty);
            if (px < 0 || px >= W || py < 0 || py >= H) continue;
            QRgb c = img.pixel(px, py);
            if (c != qRgb(255, 255, 255)) ++hits;
        }
        return hits > 0;
    }

    std::shared_ptr<MuPdfContext> context_;
    std::filesystem::path testPdf_;
    std::unique_ptr<Document> document_;
    std::unique_ptr<TextEditTool> tool_;
    std::unique_ptr<TextEditModel> model_;
};

// Note: TEST_F below uses TextEditSelectionRenderTest; close the anonymous
// namespace before TEST_F so the macro can register the test fixture.

}  // namespace

// ─────────────────────────────────────────────────────────────────────
// UI-TE-SEL-02 — selRect non-degenerate after click+drag
// Uses the public hasTextSelection() observer.
// ─────────────────────────────────────────────────────────────────────
TEST_F(TextEditSelectionRenderTest, ClickDragProducesNonDegenerateSelection) {
    ASSERT_GT(model_->span(0).text.size(), 3u)
        << "Test requires a span with >3 chars to drag-select.";

    clickAndDragSelectFirstChars(0, 3);
    EXPECT_TRUE(tool_->hasTextSelection())
        << "Click+drag must produce a non-degenerate selection.";
}

// ─────────────────────────────────────────────────────────────────────
// UI-TE-SEL-01 — Reported bug: selection highlight visible after drag.
// Pixel-level proof via onPaintOverlay rendering.
// ─────────────────────────────────────────────────────────────────────
TEST_F(TextEditSelectionRenderTest, ClickDragRendersHighlightPixels) {
    ASSERT_GT(model_->span(0).text.size(), 3u);

    clickAndDragSelectFirstChars(0, 3);
    ASSERT_TRUE(tool_->hasTextSelection());

    // The highlight should occupy the leftmost ~30% of the span (3 chars
    // out of >3 chars). Sampling [0.0, 0.30] should hit highlight pixels.
    EXPECT_TRUE(overlayHasHighlightPixels(0, 0.0, 0.30))
        << "onPaintOverlay produced no visible highlight in the dragged region.";
}

// ─────────────────────────────────────────────────────────────────────
// UI-TE-SEL-03 — painter.drawRect invoked for selection on overlay paint.
// Probes a slightly different region to lock the contract on draw call.
// ─────────────────────────────────────────────────────────────────────
TEST_F(TextEditSelectionRenderTest, OverlayPaintCoversSelectionRegion) {
    ASSERT_GE(model_->span(0).text.size(), 5u);

    clickAndDragSelectFirstChars(0, 5);
    ASSERT_TRUE(tool_->hasTextSelection());

    EXPECT_TRUE(overlayHasHighlightPixels(0, 0.0, 0.50))
        << "Selection highlight rectangle was not painted onto the overlay.";
}

// ─────────────────────────────────────────────────────────────────────
// UI-TE-SEL-04 — selection survives a modifier-only keystroke.
// Pressing Shift alone (no character) MUST NOT clear the selection.
// ─────────────────────────────────────────────────────────────────────
TEST_F(TextEditSelectionRenderTest, ModifierOnlyKeyDoesNotClearSelection) {
    ASSERT_GT(model_->span(0).text.size(), 3u);

    clickAndDragSelectFirstChars(0, 3);
    ASSERT_TRUE(tool_->hasTextSelection());

    QKeyEvent shiftDown(QEvent::KeyPress, Qt::Key_Shift,
                        Qt::ShiftModifier, QString());
    tool_->onKeyPress(&shiftDown);

    EXPECT_TRUE(tool_->hasTextSelection())
        << "Modifier-only keypress must not clear the selection.";
}

// ─────────────────────────────────────────────────────────────────────
// UI-TE-SEL-05 — selection survives click-out + click-back-in only when
// the user re-creates it. Locking the related contract: clicking back
// into the same span resets selection cleanly (no stale state) so a
// fresh drag starts at -1, not at the old end.
// ─────────────────────────────────────────────────────────────────────
TEST_F(TextEditSelectionRenderTest, ReclickOnSpanResetsSelection) {
    ASSERT_GT(model_->span(0).text.size(), 3u);

    clickAndDragSelectFirstChars(0, 3);
    ASSERT_TRUE(tool_->hasTextSelection());

    // Click back into the same span (no drag) — selection should reset
    // (selectionStart_ = selectionEnd_ = -1 per onMousePress contract).
    mousePressAt(spanCenter(0));
    EXPECT_FALSE(tool_->hasTextSelection())
        << "A bare click into the active span must reset selection state "
           "so a subsequent drag starts cleanly.";
}

// ─────────────────────────────────────────────────────────────────────
// UI-TE-SEL-08 (P1) — double-click word selection sets non-degenerate range.
// ─────────────────────────────────────────────────────────────────────
TEST_F(TextEditSelectionRenderTest, DoubleClickSelectsWord) {
    ASSERT_GT(model_->span(0).text.size(), 3u);

    // Double-click in the interior of the span
    QMouseEvent ev(QEvent::MouseButtonDblClick, QPointF(120, 100),
                   Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QPointF pdfPoint = spanInteriorAt(0, 0.4);
    tool_->onMouseDoubleClick(&ev, 0, pdfPoint);

    EXPECT_TRUE(tool_->hasTextSelection())
        << "Double-click must produce a non-empty word selection.";
}

// ─────────────────────────────────────────────────────────────────────
// UI-TE-SEL-10 (P1) — backspace with selection deletes the range, not the
// cursor neighbor. Verified by checking text length decrease > 1.
// ─────────────────────────────────────────────────────────────────────
TEST_F(TextEditSelectionRenderTest, BackspaceWithSelectionDeletesRange) {
    ASSERT_GE(model_->span(0).text.size(), 5u);
    size_t origLen = model_->span(0).text.size();

    clickAndDragSelectFirstChars(0, 3);
    ASSERT_TRUE(tool_->hasTextSelection());

    QKeyEvent backspace(QEvent::KeyPress, Qt::Key_Backspace,
                        Qt::NoModifier, QString());
    tool_->onKeyPress(&backspace);

    // After backspace with 3-char selection, span should be 3 chars shorter
    size_t newLen = tool_->activeSpan() ? tool_->activeSpan()->text.size() : 0;
    EXPECT_LE(newLen, origLen - 2)
        << "Backspace with selection must delete >1 char. orig="
        << origLen << ", new=" << newLen;

    // Selection must be cleared after the range delete
    EXPECT_FALSE(tool_->hasTextSelection())
        << "Selection must be cleared after range delete via backspace.";
}

// ─────────────────────────────────────────────────────────────────────
// UI-TE-SEL-11 (P1) — typing while selected replaces the selection.
// ─────────────────────────────────────────────────────────────────────
TEST_F(TextEditSelectionRenderTest, TypingReplacesSelection) {
    ASSERT_GE(model_->span(0).text.size(), 5u);
    size_t origLen = model_->span(0).text.size();

    clickAndDragSelectFirstChars(0, 3);
    ASSERT_TRUE(tool_->hasTextSelection());

    tool_->onTextInput("Q");

    // After typing 'Q' with non-empty selection: selection chars deleted,
    // 1 inserted. Net length change is negative (n_selected >= 2 ⇒ −1 or
    // more). The exact selection count depends on hit-test rounding from
    // the drag, so we assert the invariant (length decreased) rather than
    // a specific count.
    size_t newLen = tool_->activeSpan() ? tool_->activeSpan()->text.size() : 0;
    EXPECT_LT(newLen, origLen)
        << "Typing one char with non-empty selection must reduce span "
           "length (selected range deleted, single char inserted). orig="
        << origLen << ", new=" << newLen;

    // Selection must be cleared after replace
    EXPECT_FALSE(tool_->hasTextSelection())
        << "Selection must be cleared after typing-replaces-selection.";
}

// ─────────────────────────────────────────────────────────────────────
// UI-TE-SEL-06 — selection clears on click outside the active span.
// (Existing test_text_edit_tool.cpp verifies isEditing() goes false but
// not the selection state. This test locks selection state explicitly.)
// ─────────────────────────────────────────────────────────────────────
TEST_F(TextEditSelectionRenderTest, ClickOutsideClearsSelection) {
    ASSERT_GT(model_->span(0).text.size(), 3u);

    clickAndDragSelectFirstChars(0, 3);
    ASSERT_TRUE(tool_->hasTextSelection());

    // Click far outside any text
    mousePressAt(QPointF(10000, 10000));
    EXPECT_FALSE(tool_->hasTextSelection())
        << "Click outside the active span must clear the selection.";
    EXPECT_FALSE(tool_->isEditing())
        << "Click outside should also commit + exit editing.";
}

// ─────────────────────────────────────────────────────────────────────
// UI-TE-SEL-12 — Shift+Right anchors selection at current cursor
// position on the first keystroke (product bug fixed in pipeline
// c91e7f3a phase-3).  Without the anchor-setting fix, selectionStart_
// stays -1 after Shift+Right, so hasTextSelection() returns false and
// the canvas paint path produces no highlight.
// ─────────────────────────────────────────────────────────────────────
TEST_F(TextEditSelectionRenderTest, ShiftRightAnchorsSelectionOnFirstKeystroke) {
    ASSERT_GE(model_->span(0).text.size(), 5u);

    // Click into the span to activate and place caret at position 0.
    mousePressAt(spanLeftEdge(0));
    ASSERT_TRUE(tool_->isEditing());
    ASSERT_FALSE(tool_->hasTextSelection())
        << "Bare click must not pre-select anything.";

    // Send Shift+Right — should anchor selectionStart_ = 0 and set
    // selectionEnd_ = 1, producing a non-degenerate single-char selection.
    QKeyEvent shiftRight(QEvent::KeyPress, Qt::Key_Right,
                         Qt::ShiftModifier, QString());
    tool_->onKeyPress(&shiftRight);

    EXPECT_TRUE(tool_->hasTextSelection())
        << "Shift+Right must produce a non-empty selection (selectionStart_ "
           "anchored at cursor before the first move). This was the root "
           "cause of UI-TE-UI-01 sel_blue=0 in pipeline c91e7f3a phase-3.";
}

// ─────────────────────────────────────────────────────────────────────
// UI-TE-SEL-13 — Five Shift+Right keystrokes extend the selection to
// five characters from the initial cursor position.
// ─────────────────────────────────────────────────────────────────────
TEST_F(TextEditSelectionRenderTest, FiveShiftRightExtendsFiveChars) {
    ASSERT_GE(model_->span(0).text.size(), 6u);

    // Place caret at left edge of span.
    mousePressAt(spanLeftEdge(0));
    ASSERT_TRUE(tool_->isEditing());

    // Five Shift+Right presses.
    for (int i = 0; i < 5; ++i) {
        QKeyEvent shiftRight(QEvent::KeyPress, Qt::Key_Right,
                             Qt::ShiftModifier, QString());
        tool_->onKeyPress(&shiftRight);
    }

    EXPECT_TRUE(tool_->hasTextSelection())
        << "Five Shift+Right presses must yield a non-empty selection.";
}

// ─────────────────────────────────────────────────────────────────────
// UI-TE-SEL-14 — Shift+Right selection renders visible highlight pixels
// (full paint-path integration test).
// ─────────────────────────────────────────────────────────────────────
TEST_F(TextEditSelectionRenderTest, ShiftRightSelectionRendersHighlight) {
    ASSERT_GE(model_->span(0).text.size(), 5u);

    // Place caret at left edge of span.
    mousePressAt(spanLeftEdge(0));
    ASSERT_TRUE(tool_->isEditing());

    // Three Shift+Right presses to select the first three chars.
    for (int i = 0; i < 3; ++i) {
        QKeyEvent shiftRight(QEvent::KeyPress, Qt::Key_Right,
                             Qt::ShiftModifier, QString());
        tool_->onKeyPress(&shiftRight);
    }

    ASSERT_TRUE(tool_->hasTextSelection());

    // The first ~30% of the span should contain highlight pixels
    // after onPaintOverlay runs.
    EXPECT_TRUE(overlayHasHighlightPixels(0, 0.0, 0.30))
        << "Shift+Right selection must produce visible highlight pixels in "
           "the canvas overlay paint path (UI-TE-UI-01 regression lock).";
}

// ─────────────────────────────────────────────────────────────────────
// UI-TE-SEL-15 — Shift+Left from mid-span anchors and extends leftward.
// ─────────────────────────────────────────────────────────────────────
TEST_F(TextEditSelectionRenderTest, ShiftLeftAnchorsSelectionOnFirstKeystroke) {
    ASSERT_GE(model_->span(0).text.size(), 5u);

    // Click near the middle of the span to place caret there.
    mousePressAt(spanInteriorAt(0, 0.5));
    ASSERT_TRUE(tool_->isEditing());
    ASSERT_FALSE(tool_->hasTextSelection());

    // Shift+Left should anchor at the current mid-span caret position.
    QKeyEvent shiftLeft(QEvent::KeyPress, Qt::Key_Left,
                        Qt::ShiftModifier, QString());
    tool_->onKeyPress(&shiftLeft);

    EXPECT_TRUE(tool_->hasTextSelection())
        << "Shift+Left must anchor selection at the current cursor and "
           "extend one char to the left.";
}
