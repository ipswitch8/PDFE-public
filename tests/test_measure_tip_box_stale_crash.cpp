// tests/test_measure_tip_box_stale_crash.cpp
//
// GitHub #344 — the measure "tip box" (a StickyNotePopup bound to a FreeText)
// crashed PDFE after the text box it edits was deleted from the comment panel.
//
// Mechanism:
//   onCommentPanelDelete() deleted the FreeText but never closed the
//   StickyNotePopup still bound to it. A later popup edit routed through
//   onStickyNotePopupContentsChanged → onAnnotationPropertiesChanged →
//   AnnotationManager::getAnnotation(page, data.annotIndex) with a now-stale
//   index, which fz_throws "Annotation index out of range" and crashes.
//
// The fix has two parts, pinned by the two tests below, driven through the REAL
// MainWindow slots (via thin ForTesting seams), not a reimplementation:
//   1. onCommentPanelDelete() closes the popup bound to the deleted annotation.
//   2. onAnnotationPropertiesChanged() upper-bound-guards the index so a stale
//      index is a safe no-op instead of an out-of-range dereference.

#include <gtest/gtest.h>

#include <QString>
#include <QTemporaryDir>

#include <filesystem>
#include <memory>

#include "annotations/AnnotationData.h"
#include "annotations/AnnotationManager.h"
#include "annotations/AnnotationType.h"
#include "core/Document.h"
#include "core/Page.h"
#include "ui/MainWindow.h"

using pdfe::annotations::AnnotationType;
using pdfe::ui::MainWindow;

namespace {

class MeasureTipBoxStaleCrashTest : public ::testing::Test {
protected:
    void SetUp() override {
        const std::filesystem::path fixture =
            std::filesystem::path(TEST_FIXTURES_DIR) / "test_single.pdf";
        if (!std::filesystem::exists(fixture)) {
            GTEST_SKIP() << "fixture not available: " << fixture;
        }
        win_ = std::make_unique<MainWindow>();
        win_->openFile(QString::fromStdString(fixture.string()));

        auto* doc = win_->documentForTesting();
        if (!doc || doc->pageCount() < 1) {
            GTEST_SKIP() << "document did not open";
        }
        mgr_ = win_->annotationManagerForTesting();
        ASSERT_NE(mgr_, nullptr);

        // Two FreeText boxes on page 0, so deleting index 1 leaves index 0
        // valid and makes index 1 genuinely out of range afterwards.
        auto page = doc->page(0);
        ASSERT_NE(page, nullptr);
        mgr_->createAnnotation(page.get(), AnnotationType::FreeText,
                               QRectF(60, 60, 160, 40));
        mgr_->createAnnotation(page.get(), AnnotationType::FreeText,
                               QRectF(60, 160, 160, 40));
        ASSERT_EQ(annotCount(), 2);
    }

    int annotCount() {
        auto* doc = win_->documentForTesting();
        auto page = doc->page(0);
        return static_cast<int>(mgr_->getAnnotations(page.get()).size());
    }

    std::unique_ptr<MainWindow> win_;
    pdfe::annotations::AnnotationManager* mgr_ = nullptr;
};

// Part 1: deleting the annotation the popup is bound to must close the popup.
TEST_F(MeasureTipBoxStaleCrashTest, DeletingBoundAnnotationClosesThePopup) {
    pdfe::annotations::AnnotationData data;
    data.type = AnnotationType::FreeText;
    data.pageIndex = 0;
    data.annotIndex = 1;
    data.rect = QRectF(60, 160, 160, 40);
    data.contents = "tip";

    win_->openStickyNotePopupForTesting(data);
    ASSERT_TRUE(win_->isStickyNotePopupVisibleForTesting())
        << "precondition: the popup must be open and bound to annotation 1";

    win_->deleteAnnotationForTesting(0, 1);

    EXPECT_FALSE(win_->isStickyNotePopupVisibleForTesting())
        << "GitHub #344: onCommentPanelDelete left the StickyNotePopup open on a "
           "now-deleted annotation; a later edit crashes on the stale index";
}

// Part 2: a property change carrying a stale index must be a guarded no-op, not
// an out-of-range dereference that throws out of the slot.
TEST_F(MeasureTipBoxStaleCrashTest, StalePropertyChangeIsAGuardedNoOp) {
    // Delete index 1 directly (as the comment panel would), leaving only 0.
    win_->deleteAnnotationForTesting(0, 1);
    ASSERT_EQ(annotCount(), 1);

    // Feed the exact payload a popup edit of the now-gone annotation would emit.
    pdfe::annotations::AnnotationData stale;
    stale.type = AnnotationType::FreeText;
    stale.pageIndex = 0;
    stale.annotIndex = 1;  // out of range: only index 0 remains
    stale.rect = QRectF(60, 160, 160, 40);
    stale.contents = "edited after delete";

    EXPECT_NO_THROW(win_->applyAnnotationPropertyChangeForTesting(stale))
        << "GitHub #344: getAnnotation() on the stale index throws and the "
           "exception unwinds out of the slot, crashing the app";

    // The guarded no-op must not have mutated the surviving annotation set.
    EXPECT_EQ(annotCount(), 1);
}

}  // namespace
