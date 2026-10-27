// -*- coding: utf-8 -*-
//
// Pipeline f4a82c1e, Phase 3 (Option B) — RED repro for the journey-soak
// "batch4-postundo" deterministic crash (undo AFTER a full-rewrite save).
//
// WHAT THE JOURNEY DID
// --------------------
// The cumulative single-session journey (tests/test_ui_journey_soak.py) crashes
// deterministically at the checkpoint labelled "batch4-postundo": the pdfe.exe
// process VANISHES the moment an UNDO runs after a sequence of edits + a save.
// Among the journey's op vocabulary is a page DELETE (delete_page, op kind 11),
// which pushes a pdfe::commands::DeletePageCommand onto the undo stack.
//
// THE DEFECT THIS TEST PINS
// -------------------------
// The Option-B page-rebind refactor (core/SaveWithRebind.h + Command::
// pageIndex()/rebindPage()) drains and re-resolves the *Page* objects that undo
// commands hold by raw pointer, and every Page*-holding family (Annotation,
// Redaction, Image, Form, Signature) overrides the rebind seam. But the
// PAGE-STRUCTURE family does NOT:
//
//   DeletePageCommand / MovePageCommand / DeletePageRangeCommand /
//   ReplacePageCommand each cache a retained *pdf_obj\** (void* retainedPageObj_,
//   from Document::retainPageObj -> pdf_keep_obj) so undo() can re-insert the
//   removed page. That pdf_obj\* belongs to the CURRENT in-memory pdf_document.
//
// A full-rewrite save invalidates that retained pdf_obj\* and neither
// pageIndex()/rebindPage() (which only re-point a Page*, not a pdf_obj*) nor the
// save orchestration touches it:
//
//   * Document::save() (in-place File>Save) drops pImpl->doc via
//     fz_drop_document and REOPENS a fresh pdf_document (Document.cpp:830-882).
//     The retained pdf_obj\* now points into the FREED old document -> undo's
//     Document::insertPageObj(pdf_insert_page) dereferences freed memory -> UAF.
//
//   * Document::saveAs() GC-renumbers the in-memory xref (do_garbage) and drops
//     the page tree. The deleted page's obj is unreferenced, so GC reclaims its
//     xref slot; the kept C-struct is orphaned with stale indirect refs. undo's
//     re-insert grafts a corrupt/dangling object -> UAF / corruption.
//
// CONTRACT (what the eventual fix must restore): a page-structure command on the
// undo stack must survive a full-rewrite save and undo without a crash.
//
// This test is written to be RED on the current tree. A UAF manifests as a
// process-level access violation (exit 0xC0000005) or heap corruption abort;
// gtest cannot catch it with EXPECT_*, so a nonzero process exit IS the RED
// signal. The control test at the bottom (annotation family, which IS
// rebind-covered) passes, proving the harness itself is sound.

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/Page.h"
#include "core/MuPdfContext.h"
#include "core/UndoManager.h"
#include "core/PageCommands.h"
#include "core/SaveWithRebind.h"
#include "annotations/AnnotationCommands.h"
#include "annotations/AnnotationManager.h"

#include <QRectF>

#include <filesystem>
#include <memory>
#include <system_error>
#include <vector>

namespace {

using pdfe::core::Document;
using pdfe::core::MuPdfContext;
using pdfe::core::Page;
using pdfe::commands::UndoManager;
using pdfe::commands::DeletePageCommand;

class UndoAfterSaveCrashTest : public ::testing::Test {
protected:
    void SetUp() override {
        // A multi-page fixture: DeletePageCommand refuses to delete the last
        // page, and we want several pages so the delete + re-insert is real.
        std::filesystem::path fixture =
            std::filesystem::path(TEST_FIXTURES_DIR) / "test_10page.pdf";
        if (!std::filesystem::exists(fixture)) {
            GTEST_SKIP() << "test_10page.pdf not available";
        }
        tempPdf = std::filesystem::temp_directory_path() /
                  "pdfe_undo_after_save_crash.pdf";
        std::error_code ec;
        std::filesystem::remove(tempPdf, ec);
        std::filesystem::copy_file(
            fixture, tempPdf,
            std::filesystem::copy_options::overwrite_existing, ec);
        ASSERT_FALSE(ec) << "failed to stage temp copy: " << ec.message();

        context = std::make_shared<MuPdfContext>();
        document = Document::open(context, tempPdf);
        ASSERT_NE(document, nullptr);
        ASSERT_GT(document->pageCount(), 2);
        undoManager = std::make_unique<UndoManager>();
    }

    void TearDown() override {
        undoManager.reset();
        document.reset();
        context.reset();
        std::error_code ec;
        std::filesystem::remove(tempPdf, ec);
    }

    std::filesystem::path tempPdf;
    std::shared_ptr<MuPdfContext> context;
    std::unique_ptr<Document> document;
    std::unique_ptr<UndoManager> undoManager;
};

// ─────────────────────────────────────────────────────────────────────
// Primary repro (journey-faithful): the journey saves via Save As
// (testSaveAsPipe -> onSaveAsFile -> saveAsWithCommandPageRebind). A page
// DELETE sits on the undo stack across that save; the post-save undo then
// re-inserts using a retained pdf_obj\* that the save's GC has invalidated.
// ─────────────────────────────────────────────────────────────────────
TEST_F(UndoAfterSaveCrashTest, DeletePageCommand_SaveAs_Undo_DoesNotCrash) {
    const int pagesBefore = document->pageCount();

    // Page delete -> DeletePageCommand retains the removed page's pdf_obj*.
    undoManager->execute(
        std::make_unique<DeletePageCommand>(document.get(), /*pageIndex=*/2));
    ASSERT_EQ(document->pageCount(), pagesBefore - 1);

    // Save As exactly as MainWindow::onSaveAsFile does: drain + rebind around
    // Document::saveAs. commandPages_ is empty here (a page delete holds no
    // Page* — it holds a pdf_obj*), so the rebind is a no-op for this command,
    // which is precisely the gap: nothing re-resolves retainedPageObj_.
    std::vector<std::unique_ptr<Page>> commandPages;
    const std::filesystem::path outPath =
        std::filesystem::temp_directory_path() /
        "pdfe_undo_after_save_crash_saveas.pdf";
    std::error_code ec;
    std::filesystem::remove(outPath, ec);
    ASSERT_NO_THROW(
        pdfe::core::saveAsWithCommandPageRebind(
            *document, *undoManager, commandPages, outPath));

    // The post-save undo: re-insert the deleted page via the retained pdf_obj*.
    // On the current tree this dereferences an object the saveAs GC reclaimed.
    ASSERT_TRUE(undoManager->canUndo());
    undoManager->undo();

    // If we survive, the page must be back.
    EXPECT_EQ(document->pageCount(), pagesBefore)
        << "undo after Save As must restore the deleted page";

    std::filesystem::remove(outPath, ec);
}

// ─────────────────────────────────────────────────────────────────────
// Site 2 regression (in-place File>Save, Document::save): a page-structure
// command's retained page object must survive save()'s drop+REOPEN of the
// pdf_document (Document.cpp) and still restore the page on undo.
//
// Before the fix, Document::retainPageObj() kept a raw pdf_obj* (pdf_keep_obj)
// into the LIVE pdf_document. save() fz_drop_document + reopens a fresh
// pdf_document, so that retained obj dangled into freed memory; a post-save
// undo grafted it → UAF ("container and item belong to different documents" or
// silent corruption, depending on heap reuse). The fix deep-copies the retained
// page into its OWN standalone holder document at retain time, so it is
// document-independent and survives the reopen.
//
// A UAF is heap-nondeterministic, so this drives the whole delete → in-place
// save → undo cycle repeatedly and asserts an OBSERVABLE correct outcome (no
// throw + page restored + pageCount correct) every iteration. With the fix that
// is deterministic; without it the post-save undo throws or corrupts.
// ─────────────────────────────────────────────────────────────────────
TEST_F(UndoAfterSaveCrashTest, DeletePageCommand_InPlaceSave_Undo_RestoresPage_Site2) {
    for (int iter = 0; iter < 3; ++iter) {
        const int pagesBefore = document->pageCount();
        ASSERT_GT(pagesBefore, 2) << "iter " << iter;

        // Page delete → DeletePageCommand retains the removed page (now a
        // standalone deep copy, per the Site-2 fix).
        undoManager->execute(
            std::make_unique<DeletePageCommand>(document.get(), /*pageIndex=*/2));
        ASSERT_EQ(document->pageCount(), pagesBefore - 1) << "iter " << iter;

        // Force the full-rewrite drop+reopen branch of Document::save() (a plain
        // page delete can otherwise route to an incremental append, which would
        // NOT exercise the reopen that used to invalidate the retained obj).
        document->markRequiresFullRewrite();

        // In-place File>Save via the production drain+rebind, writing back over
        // the opened file — this is the path that drops+reopens pImpl->doc.
        std::vector<std::unique_ptr<Page>> commandPages;
        ASSERT_NO_THROW(
            pdfe::core::saveWithCommandPageRebind(
                *document, *undoManager, commandPages, tempPdf))
            << "iter " << iter;

        // Post-save undo: re-insert the deleted page from the retained holder.
        // On the pre-fix tree this grafts a freed-document obj.
        ASSERT_TRUE(undoManager->canUndo()) << "iter " << iter;
        ASSERT_NO_THROW(undoManager->undo()) << "iter " << iter;
        EXPECT_EQ(document->pageCount(), pagesBefore)
            << "undo after in-place Save must restore the deleted page (iter "
            << iter << ")";

        // Reset the undo stack so the next iteration starts from a clean,
        // fully-restored document.
        undoManager->clear();
    }
}

// NOTE — the Save As variant of the same retained-obj hazard is covered
// deterministically by the journey-faithful rebind-to-null case below.

// ─────────────────────────────────────────────────────────────────────
// Control (must PASS): the annotation family IS rebind-covered, so the same
// edit -> Save As -> undo shape is safe. Proves the harness is sound and the
// crash above is specific to the un-covered page-structure family, not a
// test-authoring error.
// ─────────────────────────────────────────────────────────────────────
TEST_F(UndoAfterSaveCrashTest, AnnotationCommand_SaveAs_Undo_IsSafe_Control) {
    pdfe::annotations::AnnotationManager manager(context);

    std::vector<std::unique_ptr<Page>> commandPages;
    commandPages.push_back(document->page(0));
    ASSERT_NE(commandPages.back(), nullptr);
    Page* pagePtr = commandPages.back().get();

    QRectF rect(100, 100, 200, 50);
    undoManager->execute(
        std::make_unique<pdfe::annotations::CreateAnnotationCommand>(
            &manager, pagePtr, pdfe::annotations::AnnotationType::Text, rect));
    const size_t countAfterCreate = manager.getAnnotations(pagePtr).size();
    ASSERT_GT(countAfterCreate, 0u);

    const std::filesystem::path outPath =
        std::filesystem::temp_directory_path() /
        "pdfe_undo_after_save_crash_control.pdf";
    std::error_code ec;
    std::filesystem::remove(outPath, ec);
    document->markRequiresFullRewrite();

    ASSERT_NO_THROW(
        pdfe::core::saveAsWithCommandPageRebind(
            *document, *undoManager, commandPages, outPath));

    ASSERT_TRUE(undoManager->canUndo());
    ASSERT_NO_THROW(undoManager->undo());
    Page* reboundPage = commandPages.empty() ? nullptr : commandPages.back().get();
    ASSERT_NE(reboundPage, nullptr);
    EXPECT_EQ(manager.getAnnotations(reboundPage).size(), countAfterCreate - 1);

    std::filesystem::remove(outPath, ec);
}

// ─────────────────────────────────────────────────────────────────────
// Journey-faithful Save As repro (rebind-to-null on a shifted page index).
//
// The journey interleaves annotation creates and page deletes on ONE document.
// An annotation created on a high page index, followed by a page DELETE, leaves
// the annotation command's cached Page index stale: Page::index() returns a
// snapshot (Page.cpp:128), so after a delete shrinks the document the index now
// points past the end. saveAsWithCommandPageRebind snapshots that stale index,
// and its re-resolve step does doc.page(staleIdx) -> nullptr -> rebindPage(
// nullptr). The command's page_ is now null; a later undo dereferences it.
//
// Unlike the same-document retained-obj case above (which saveAs happens to
// survive), THIS is the mechanism by which the journey's Save-As path reaches a
// post-save undo crash without ever using in-place File>Save.
// ─────────────────────────────────────────────────────────────────────
TEST_F(UndoAfterSaveCrashTest, AnnotationOnDeletedPage_SaveAs_Undo_DoesNotCrash) {
    pdfe::annotations::AnnotationManager manager(context);
    const int pagesBefore = document->pageCount();          // 10
    const int lastIdx = pagesBefore - 1;                    // 9

    // 1. Create an annotation on the LAST page; commandPages_ owns that Page.
    std::vector<std::unique_ptr<Page>> commandPages;
    commandPages.push_back(document->page(lastIdx));
    ASSERT_NE(commandPages.back(), nullptr);
    Page* pagePtr = commandPages.back().get();
    ASSERT_EQ(pagePtr->index(), lastIdx);
    undoManager->execute(
        std::make_unique<pdfe::annotations::CreateAnnotationCommand>(
            &manager, pagePtr, pdfe::annotations::AnnotationType::Text,
            QRectF(100, 100, 120, 30)));

    // 2. Delete an earlier page. The document now has one fewer page, so the
    //    last valid index is pagesBefore-2 — the annotation's cached index
    //    (lastIdx) is now out of range. This DeletePageCommand sits on top.
    undoManager->execute(
        std::make_unique<DeletePageCommand>(document.get(), /*pageIndex=*/0));
    ASSERT_EQ(document->pageCount(), pagesBefore - 1);

    // 3. Save As with the production drain+rebind. The rebind resolves the
    //    annotation command's stale index (lastIdx) against the shrunk document
    //    -> doc.page(lastIdx) is null -> rebindPage(nullptr).
    const std::filesystem::path outPath =
        std::filesystem::temp_directory_path() /
        "pdfe_undo_after_save_crash_nullrebind.pdf";
    std::error_code ec;
    std::filesystem::remove(outPath, ec);
    ASSERT_NO_THROW(
        pdfe::core::saveAsWithCommandPageRebind(
            *document, *undoManager, commandPages, outPath));

    // 4. Undo the delete (re-inserts page 0), then undo the annotation. The
    //    annotation command's page_ was rebound to null in step 3, so this
    //    second undo dereferences a null Page.
    ASSERT_TRUE(undoManager->canUndo());
    ASSERT_NO_THROW(undoManager->undo());   // undo the page delete
    ASSERT_TRUE(undoManager->canUndo());
    undoManager->undo();                    // undo the annotation -> null page_

    std::filesystem::remove(outPath, ec);
    SUCCEED();
}

}  // namespace
