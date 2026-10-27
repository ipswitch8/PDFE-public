// tests/test_form_tool_document_closing.cpp
//
// GitHub #235 — PDFE died with an access violation reading 0xffffffffffffffff
// at `pdfe::core::Page::index+0x17`, reached from
// `FormTool::onPaintOverlay -> PdfCanvas::paintEvent`. Reproduced twice with
// identical faulting addresses.
//
// FormTool caches a raw `pdfe::core::Page*`. AnnotationTool exposes
// `onDocumentClosing()` precisely so a tool can release such a pointer before
// the document frees its pages, and PdfCanvas calls it on every close/reload —
// but FormTool never overrode it, so it inherited the base no-op and kept a
// pointer into freed memory. The next repaint ran `page_->index()` on it.
//
// WHY THE EXISTING NULL CHECK WAS NOT THE GUARD. onPaintOverlay already had
//
//     if (!page_ || selectedWidget_ < 0) return;
//     if (page_->index() != pageIndex) return;
//
// and it looks defensive. It is not: a dangling pointer is not null, so it
// passes the first line and is dereferenced on the second. The pointer has to
// be *released*, not tested.
//
// WHAT THESE TESTS CAN AND CANNOT DO. They do not reproduce the use-after-free
// — dereferencing freed memory is undefined behaviour, and a test that depends
// on it is testing the allocator, not the product. They pin the *contract* that
// makes the crash impossible: after onDocumentClosing() the tool holds no page
// and paints nothing. That contract is directly observable, and removing the
// override reddens it.

#include <gtest/gtest.h>

#include <QImage>
#include <QPainter>
#include <QPointF>

#include <filesystem>
#include <memory>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "core/UndoManager.h"
#include "forms/FormManager.h"
#include "forms/FormTool.h"

using pdfe::commands::UndoManager;
using pdfe::core::Document;
using pdfe::core::MuPdfContext;
using pdfe::forms::FormManager;
using pdfe::forms::FormTool;

namespace {

class FormToolDocumentClosingTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_shared<MuPdfContext>();
        const std::filesystem::path fixture =
            std::filesystem::path(TEST_FIXTURES_DIR) / "test_form.pdf";
        if (!std::filesystem::exists(fixture)) {
            GTEST_SKIP() << "fixture missing: " << fixture;
        }
        document_ = Document::open(ctx_, fixture);
        ASSERT_NE(document_, nullptr);
        ASSERT_GT(document_->pageCount(), 0);

        manager_ = std::make_unique<FormManager>(ctx_);
        undo_ = std::make_unique<UndoManager>();
        pageOwner_ = document_->page(0);
        page_ = pageOwner_.get();
        ASSERT_NE(page_, nullptr);

        tool_ = std::make_unique<FormTool>(manager_.get(), nullptr, undo_.get());
        tool_->setPage(page_);
    }

    // True if anything at all was drawn.
    bool paintProducedOutput(int pageIndex = 0) {
        canvas_.fill(Qt::transparent);
        {
            QPainter painter(&canvas_);
            tool_->onPaintOverlay(painter, pageIndex, 1.0f, QPointF(0, 0), 792.0f);
        }
        for (int y = 0; y < canvas_.height(); ++y)
            for (int x = 0; x < canvas_.width(); ++x)
                if (qAlpha(canvas_.pixel(x, y)) != 0) return true;
        return false;
    }

    QImage canvas_{400, 400, QImage::Format_ARGB32};
    std::shared_ptr<MuPdfContext> ctx_;
    std::unique_ptr<Document> document_;
    std::unique_ptr<FormManager> manager_;
    std::unique_ptr<UndoManager> undo_;
    std::shared_ptr<pdfe::core::Page> pageOwner_;
    pdfe::core::Page* page_ = nullptr;
    std::unique_ptr<FormTool> tool_;
};

// THE CONTRACT. Painting after the document has been declared closing must not
// reach the page at all. Deleting the onDocumentClosing() override leaves page_
// set and the selection live, the highlight is drawn, and this reddens.
TEST_F(FormToolDocumentClosingTest, PaintingAfterDocumentClosingTouchesNothing) {
    tool_->tabToNextField();   // public path to a selection
    if (tool_->selectedFieldIndex() < 0) {
        GTEST_SKIP() << "fixture has no selectable widget on page 0";
    }
    ASSERT_TRUE(paintProducedOutput())
        << "precondition: a selected field must paint a highlight, otherwise "
           "the post-close assertion below passes for the wrong reason";

    tool_->onDocumentClosing();

    EXPECT_LT(tool_->selectedFieldIndex(), 0)
        << "the selection must be dropped, or the next paint re-enters the "
           "page-dereferencing branch";
    EXPECT_FALSE(paintProducedOutput())
        << "GitHub #235: onPaintOverlay still drew after onDocumentClosing, so "
           "it still reached page_ -- which by then points at freed memory";
}

// The narrower half, stated separately because it is the actual crash site:
// the guard must not depend on the page POINTER being null-checked, because a
// freed page is not a null page. Painting for a DIFFERENT page index must also
// be inert after close -- that path runs `page_->index() != pageIndex`, which
// is the exact line that faulted.
TEST_F(FormToolDocumentClosingTest, TheIndexComparisonIsNotReachedAfterClosing) {
    tool_->tabToNextField();   // public path to a selection
    if (tool_->selectedFieldIndex() < 0) {
        GTEST_SKIP() << "fixture has no selectable widget on page 0";
    }
    tool_->onDocumentClosing();

    // Page index 7 does not exist in the fixture. Before the fix this would
    // still evaluate page_->index() to find that out.
    EXPECT_FALSE(paintProducedOutput(7));
    EXPECT_FALSE(paintProducedOutput(0));
}

// onAnnotationsChanged is the document-STAYS-OPEN sibling: the field set moved,
// so a cached widget index may name a different field or none, but the page is
// still valid and must be kept.
TEST_F(FormToolDocumentClosingTest, OnAnnotationsChangedDropsTheIndexButKeepsThePage) {
    tool_->tabToNextField();   // public path to a selection
    if (tool_->selectedFieldIndex() < 0) {
        GTEST_SKIP() << "fixture has no selectable widget on page 0";
    }

    tool_->onAnnotationsChanged();
    EXPECT_LT(tool_->selectedFieldIndex(), 0);

    // The page must still be usable -- selecting again has to work. If
    // onAnnotationsChanged had nulled page_ (copying onDocumentClosing without
    // thinking), the rebuilt tab order would be empty, tabToNextField() would
    // return immediately, and the tool would be dead for the rest of the
    // session on a mere field edit.
    tool_->tabToNextField();
    EXPECT_GE(tool_->selectedFieldIndex(), 0)
        << "onAnnotationsChanged must NOT release the page -- the document is "
           "still open";
}

}  // namespace
