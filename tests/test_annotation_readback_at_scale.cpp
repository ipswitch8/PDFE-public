// A crash, reduced: creating sticky notes one after another eventually throws
// out of MainWindow's annotation-created slot and kills the application.
//
// OBSERVED (2026-09-30, Release, tests/test_ui_stress.py::
// test_stress_1000_annotation_undo_redo driving real clicks): PDFE died at
// roughly the 830th sticky note, three runs out of three. The dump's stack is
// unambiguous:
//
//   StickyNoteTool::onMousePress
//     -> PdfCanvas::annotationCreated
//       -> MainWindow::onAnnotationCreatedByTool
//         -> AnnotationManager::getAnnotation
//           -> MuPdfContext::execute
//             -> _CxxThrowException      <-- "Annotation index out of range"
//
// and the exception then crossed a Qt event handler, which Qt itself warns is
// not survivable ("You must not let any exception whatsoever propagate through
// Qt code").
//
// The UI test did not report a crash. It reported ONE SKIPPED: the application
// was gone by the time the next step tried to drive the Save dialog, and the
// helper's "could not save" branch calls pytest.skip(). That is the project's
// own recurring shape -- a crash surfacing as an unreachable control -- and it
// is why this was invisible for as long as the enclosing test was xfail.
//
// This file reproduces it headlessly, where it can be bisected and where a fix
// can be shown to hold.

#include <gtest/gtest.h>

#include "annotations/AnnotationManager.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"

#include <QRectF>

#include <filesystem>
#include <memory>
#include <string>

namespace fs = std::filesystem;

using pdfe::annotations::AnnotationManager;
using pdfe::annotations::AnnotationType;
using pdfe::core::Document;
using pdfe::core::MuPdfContext;

namespace {

fs::path fixture()
{
    return fs::path(TEST_FIXTURES_DIR) / "test_3page.pdf";
}

// How many annotations to drive. The live failure was near 830; 900 clears it
// with margin without making the case slow.
constexpr int kAnnotationCount = 900;

}  // namespace

class AnnotationReadbackAtScaleTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(fs::exists(fixture()))
            << "fixture missing: " << fixture().string();
        ctx_ = std::make_shared<MuPdfContext>();
        doc_ = Document::open(ctx_, fixture());
        ASSERT_NE(doc_, nullptr);
        ASSERT_GE(doc_->pageCount(), 1);
        mgr_ = std::make_unique<AnnotationManager>(ctx_);
    }

    std::shared_ptr<MuPdfContext> ctx_;
    std::unique_ptr<Document> doc_;
    std::unique_ptr<AnnotationManager> mgr_;
};

// THE CONTROL, and it did NOT reproduce -- which is the finding, not a
// disappointment. This is exactly what MainWindow::onAnnotationCreatedByTool
// does: create the annotation against one Page, then read it back through a
// FRESHLY resolved Page for the same index, because the slot re-resolves
// `document_->page(pageIndex)` rather than reusing the page the tool used.
//
// 900 cycles, green. So the divergence that killed the application is not in
// create-then-read-back through AnnotationManager, and anyone hunting it
// should not start there. What the live path has and this does not: an
// UndoManager whose 500-deep stack has begun discarding CreateAnnotationCommand
// objects from the front, a StickyNotePopup opening and closing per creation,
// a per-creation CommentPanel rebuild, and setColor/setContents writes between
// the create and the read.
//
// Keeping a green control that names what it RULED OUT is worth more than
// deleting it for not having reproduced; without it the next investigation
// re-walks this ground.
TEST_F(AnnotationReadbackAtScaleTest, TheManagerPathAloneSurvivesNineHundredCycles)
{
    for (int i = 0; i < kAnnotationCount; ++i) {
        auto page = doc_->page(0);
        ASSERT_NE(page, nullptr) << "page 0 vanished at i=" << i;

        const QRectF rect(10.0 + (i % 40) * 2.0, 10.0 + (i / 40) * 2.0,
                          18.0, 18.0);
        auto created = mgr_->createAnnotation(page.get(), AnnotationType::Text,
                                              rect);
        const int newIndex = created.annotIndex;
        ASSERT_GE(newIndex, 0) << "creation " << i << " reported no index";

        // The read-back, through a page resolved separately -- the slot's
        // exact shape.
        auto freshPage = doc_->page(0);
        ASSERT_NE(freshPage, nullptr);
        ASSERT_NO_THROW({
            const auto readBack = mgr_->getAnnotation(freshPage.get(), newIndex);
            EXPECT_EQ(readBack.annotIndex, newIndex);
        }) << "reading back annotation " << newIndex << " (creation " << i
           << " of " << kAnnotationCount << ") threw. In the application this "
              "throw crosses a Qt event handler and is fatal; the UI test that "
              "hit it reported a SKIP, not a crash.";
    }
}

// THE CLASS. The slot must not let ANY exception reach Qt, whatever the reason
// the read-back failed. That is a separate obligation from the index being
// correct: even with a correct index, a damaged page, a revoked object or a
// MuPDF error would throw here, and the result would be the same fatal exit.
//
// Expressed as a property of the manager the slot calls: an out-of-range read
// must report itself as a C++ exception the caller can catch, and must not
// leave the document mutated. This is the contract MainWindow relies on when
// it guards the call.
TEST_F(AnnotationReadbackAtScaleTest, AnOutOfRangeReadThrowsAndChangesNothing)
{
    auto page = doc_->page(0);
    ASSERT_NE(page, nullptr);

    const QRectF rect(20.0, 20.0, 40.0, 40.0);
    const auto created = mgr_->createAnnotation(page.get(), AnnotationType::Text,
                                                rect);
    ASSERT_GE(created.annotIndex, 0);

    auto beforePage = doc_->page(0);
    const size_t before = mgr_->getAnnotations(beforePage.get()).size();
    ASSERT_GT(before, 0u) << "the annotation just created is not visible, so "
                             "this case would measure nothing";

    EXPECT_ANY_THROW({
        auto p = doc_->page(0);
        (void)mgr_->getAnnotation(p.get(), static_cast<int>(before) + 500);
    }) << "an index far past the end must be reported, not silently accepted";

    auto afterPage = doc_->page(0);
    EXPECT_EQ(mgr_->getAnnotations(afterPage.get()).size(), before)
        << "a failed read changed the annotation count";
}
