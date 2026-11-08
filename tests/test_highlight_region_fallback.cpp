// -*- coding: utf-8 -*-
// GitHub #367 — Highlight only worked on text.
//
// THE DEFECT. Highlight is a TextMarkupTool. onMouseRelease() ended a
// TextSelector selection and, when the selection had no quads (no text under
// the drag), returned early and created NOTHING. On a scanned page, an image,
// or any non-text region, dragging the Highlight tool produced no annotation at
// all — the tool "only worked on text".
//
// THE FIX (per the phase-1 disposition). When the selector yields no quads,
// onMouseRelease now falls back to the dragged RECT and creates a markup over
// that region. The text-quad path is unchanged.
//
// THE ORACLE. Drive a real Highlight TextMarkupTool over a BLANK page
// (test_blank.pdf has no text, so TextSelector returns no quads deterministically
// — this is the "drag over a region with no text" route, no coordinate
// guessing) and assert a Highlight annotation IS created whose quad matches the
// dragged region. Pre-fix nothing is created and the callback never fires.
#include <gtest/gtest.h>

#include "annotations/AnnotationManager.h"
#include "annotations/TextMarkupTool.h"
#include "annotations/TextSelector.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"

#include <QColor>
#include <QMouseEvent>
#include <QPoint>
#include <QPointF>
#include <QRectF>

#include <filesystem>
#include <memory>
#include <vector>

namespace {

QMouseEvent* mouseEvent(QEvent::Type type) {
    return new QMouseEvent(type, QPoint(0, 0), Qt::LeftButton, Qt::LeftButton,
                           Qt::NoModifier);
}

} // namespace

class HighlightRegionFallbackTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<pdfe::core::MuPdfContext>();
        const std::filesystem::path pdf =
            std::filesystem::path(TEST_FIXTURES_DIR) / "test_blank.pdf";
        if (!std::filesystem::exists(pdf)) {
            GTEST_SKIP() << "test_blank.pdf fixture not available";
        }
        document_ = pdfe::core::Document::open(context_, pdf);
        manager_ = std::make_unique<pdfe::annotations::AnnotationManager>(context_);
        selector_ = std::make_shared<pdfe::annotations::TextSelector>(context_);
        if (document_ && document_->pageCount() > 0) {
            pagePtr_ = document_->page(0);
            page_ = pagePtr_.get();
        }
    }

    std::shared_ptr<pdfe::core::MuPdfContext> context_;
    std::unique_ptr<pdfe::core::Document> document_;
    std::unique_ptr<pdfe::annotations::AnnotationManager> manager_;
    std::shared_ptr<pdfe::annotations::TextSelector> selector_;
    std::unique_ptr<pdfe::core::Page> pagePtr_;
    pdfe::core::Page* page_ = nullptr;
};

// A drag over a text-free region creates a Highlight over the dragged rect.
TEST_F(HighlightRegionFallbackTest, DragOverNonTextCreatesRegionHighlight) {
    if (!document_ || !page_) {
        GTEST_SKIP() << "blank fixture failed to open";
    }

    pdfe::annotations::TextMarkupTool tool(
        manager_.get(), selector_,
        pdfe::annotations::AnnotationType::Highlight, QColor(255, 255, 0));
    tool.setPage(page_);

    bool created = false;
    pdfe::annotations::AnnotationType createdType =
        pdfe::annotations::AnnotationType::Unknown;
    std::vector<QRectF> createdQuads;
    tool.annotationCreated =
        [&](int, pdfe::annotations::AnnotationData data) {
            created = true;
            createdType = data.type;
            createdQuads = data.quadPoints;
        };

    // Drag a rectangle across the blank page. No text is under it, so
    // TextSelector yields no quads and the #367 fallback must engage.
    const QPointF start(100.0, 100.0);
    const QPointF end(300.0, 200.0);
    const QRectF expected = QRectF(start, end).normalized();

    auto* press = mouseEvent(QEvent::MouseButtonPress);
    auto* move  = mouseEvent(QEvent::MouseMove);
    auto* release = mouseEvent(QEvent::MouseButtonRelease);
    tool.onMousePress(press, 0, start);
    tool.onMouseMove(move, 0, end);
    tool.onMouseRelease(release, 0, end);
    delete press;
    delete move;
    delete release;

    ASSERT_TRUE(created)
        << "GitHub #367: dragging Highlight over a text-free region created "
           "nothing. With no text under the drag the tool must fall back to the "
           "dragged rectangle and still create a region highlight.";
    EXPECT_EQ(createdType, pdfe::annotations::AnnotationType::Highlight);
    ASSERT_EQ(createdQuads.size(), 1u)
        << "the region markup must carry one quad covering the dragged rect";
    const QRectF quad = createdQuads.front();
    EXPECT_NEAR(quad.x(),      expected.x(),      0.5);
    EXPECT_NEAR(quad.y(),      expected.y(),      0.5);
    EXPECT_NEAR(quad.width(),  expected.width(),  0.5);
    EXPECT_NEAR(quad.height(), expected.height(), 0.5);
}

// A pure click (no drag) over non-text content must NOT drop a zero-size
// annotation — the fallback is for a real dragged region, and this control
// prevents a fix that creates a degenerate markup on every stray click.
TEST_F(HighlightRegionFallbackTest, ClickWithoutDragCreatesNothing) {
    if (!document_ || !page_) {
        GTEST_SKIP() << "blank fixture failed to open";
    }

    pdfe::annotations::TextMarkupTool tool(
        manager_.get(), selector_,
        pdfe::annotations::AnnotationType::Highlight, QColor(255, 255, 0));
    tool.setPage(page_);

    bool created = false;
    tool.annotationCreated =
        [&](int, pdfe::annotations::AnnotationData) { created = true; };

    const QPointF at(150.0, 150.0);
    auto* press = mouseEvent(QEvent::MouseButtonPress);
    auto* release = mouseEvent(QEvent::MouseButtonRelease);
    tool.onMousePress(press, 0, at);
    tool.onMouseRelease(release, 0, at);   // same point: zero-size rect
    delete press;
    delete release;

    EXPECT_FALSE(created)
        << "a click with no drag produced a zero-size highlight; the region "
           "fallback must require a real dragged rectangle.";
}
