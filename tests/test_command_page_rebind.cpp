// -*- coding: utf-8 -*-
//
// Pipeline f4a82c1e, Phase 3 (Option B) — sub-phase 3a: Annotation family.
//
// RED regression for the residual save/file-lock hazard identified in
// tests/docs/journey-soak-phase3-findings.md §3-4.
//
// THE HAZARD
// ----------
// The ordinary File>Save path (MainWindow::onSaveFile) calls Document::save()
// while the session's accumulated commandPages_ — and the undo stack's
// commands that hold raw Page* refs into them — are still alive. A live Page
// keeps the source fz_document handle open (fz_keep_document), so the
// full-rewrite remove()+rename() of the original fails on Windows, the retry
// budget exhausts, and save() throws after reopening the pre-edit file: a
// silent lost-changes outcome. The irreversible redaction path avoids this by
// dropping both commandPages_ and the undo stack before saving, but ordinary
// Save must PRESERVE undo — and the undo commands hold raw Page* pointers, so
// they cannot simply be dropped without dangling.
//
// THE CONTRACT (what saveWithCommandPageRebind must guarantee)
// ------------------------------------------------------------
//   1. A full-rewrite save succeeds even when an undoable annotation command
//      holds a Page that commandPages owns (the holders are drained first).
//   2. Undo still works afterwards: the surviving command is rebound to a
//      freshly-resolved Page object, not left pointing at the dropped one.
//
// This test drives the contract through the Document/command layer because
// MainWindow is not gtest-instantiable. It FAILS against the pre-refactor
// entry point (which saves without draining) and must pass once the drain +
// rebind implementation lands.

#include <gtest/gtest.h>

#include "core/SaveWithRebind.h"
#include "core/Document.h"
#include "core/Page.h"
#include "core/MuPdfContext.h"
#include "core/UndoManager.h"
#include "annotations/AnnotationCommands.h"
#include "annotations/AnnotationManager.h"
#include "redaction/RedactionCommands.h"
#include "redaction/RedactionManager.h"

#include <QRectF>

#include <filesystem>
#include <memory>
#include <system_error>
#include <vector>

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

namespace {

using pdfe::core::Document;
using pdfe::core::MuPdfContext;
using pdfe::core::Page;
using pdfe::commands::UndoManager;

// Number of objects in a PDF's cross-reference table (== xref length). A gc=3
// full rewrite dedups byte-identical objects and compacts the xref, shrinking
// this count; a gc=1 rewrite (garbage-collect only, no compact/dedup) keeps
// every still-referenced object, so the count stays at the source size.
int countPdfObjects(const std::filesystem::path& p) {
    int n = -1;
    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    if (!ctx) return -1;
    fz_register_document_handlers(ctx);
    pdf_document* doc = nullptr;
    fz_try(ctx) {
        doc = pdf_open_document(ctx, p.string().c_str());
        n = pdf_count_objects(ctx, doc);
    }
    fz_always(ctx) {
        if (doc) pdf_drop_document(ctx, doc);
    }
    fz_catch(ctx) { n = -1; }
    fz_drop_context(ctx);
    return n;
}

class CommandPageRebindTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::filesystem::path fixture =
            std::filesystem::path(TEST_FIXTURES_DIR) / "test_3page.pdf";
        if (!std::filesystem::exists(fixture)) {
            GTEST_SKIP() << "test_3page.pdf not available";
        }
        tempPdf = std::filesystem::temp_directory_path() /
                  "pdfe_cmd_page_rebind_f4a82c1e.pdf";

        std::error_code ec;
        std::filesystem::remove(tempPdf, ec);
        std::filesystem::copy_file(
            fixture, tempPdf,
            std::filesystem::copy_options::overwrite_existing, ec);
        ASSERT_FALSE(ec) << "failed to stage temp copy: " << ec.message();

        context = std::make_shared<MuPdfContext>();
        document = Document::open(context, tempPdf);
        ASSERT_NE(document, nullptr);
        manager = std::make_unique<pdfe::annotations::AnnotationManager>(context);
        undoManager = std::make_unique<UndoManager>();
    }

    void TearDown() override {
        undoManager.reset();
        manager.reset();
        document.reset();
        context.reset();
        std::error_code ec;
        std::filesystem::remove(tempPdf, ec);
    }

    std::filesystem::path tempPdf;
    std::shared_ptr<MuPdfContext> context;
    std::unique_ptr<Document> document;
    std::unique_ptr<pdfe::annotations::AnnotationManager> manager;
    std::unique_ptr<UndoManager> undoManager;
};

TEST_F(CommandPageRebindTest, OrdinarySaveDrainsAndRebindsAnnotationCommand) {
    // Model MainWindow: commandPages_ owns the Page that the undoable command
    // references by raw pointer.
    std::vector<std::unique_ptr<Page>> commandPages;
    commandPages.push_back(document->page(0));
    ASSERT_NE(commandPages.back(), nullptr);
    Page* pagePtr = commandPages.back().get();

    QRectF rect(100, 100, 200, 50);
    auto cmd = std::make_unique<pdfe::annotations::CreateAnnotationCommand>(
        manager.get(), pagePtr, pdfe::annotations::AnnotationType::Text, rect);
    undoManager->execute(std::move(cmd));

    const size_t countAfterCreate = manager->getAnnotations(pagePtr).size();
    ASSERT_GT(countAfterCreate, 0u);

    // Force the full-rewrite temp+rename path (where the held-Page lock bites),
    // as a structural/content edit in a real session would.
    document->markRequiresFullRewrite();

    // Contract 1: the save must not be locked by the held command page.
    ASSERT_NO_THROW({
        const bool ok = pdfe::core::saveWithCommandPageRebind(
            *document, *undoManager, commandPages, tempPdf);
        EXPECT_TRUE(ok);
    }) << "ordinary save must drain command-held pages before the full rewrite "
          "(a live Page locks the source file and makes save() throw)";

    // The rewrite produced a valid, reopenable file.
    {
        auto verifyCtx = std::make_shared<MuPdfContext>();
        auto reopened = Document::open(verifyCtx, tempPdf);
        ASSERT_NE(reopened, nullptr);
        EXPECT_EQ(reopened->pageCount(), 3);
    }

    // Contract 2: undo still works after the save — the command was rebound to
    // a freshly-resolved Page rather than left dangling at the dropped one.
    ASSERT_TRUE(undoManager->canUndo());
    ASSERT_NO_THROW(undoManager->undo());

    Page* reboundPage = commandPages.empty() ? nullptr : commandPages.back().get();
    ASSERT_NE(reboundPage, nullptr)
        << "rebind must repopulate a live command-page holder";
    EXPECT_EQ(manager->getAnnotations(reboundPage).size(), countAfterCreate - 1)
        << "undo must remove the annotation on the rebound page";
}

// Generalization: an undo stack that references TWO distinct pages must have
// every command rebound to the correct freshly-resolved page (exercises the
// multi-index re-resolve + rebind-by-index path in saveWithCommandPageRebind).
TEST_F(CommandPageRebindTest, DrainRebindHandlesMultipleDistinctPages) {
    std::vector<std::unique_ptr<Page>> commandPages;

    auto makeCreateOn = [&](int pageIdx) {
        commandPages.push_back(document->page(pageIdx));
        EXPECT_NE(commandPages.back(), nullptr);
        Page* p = commandPages.back().get();
        QRectF rect(50 + pageIdx * 10, 60, 120, 40);
        undoManager->execute(
            std::make_unique<pdfe::annotations::CreateAnnotationCommand>(
                manager.get(), p, pdfe::annotations::AnnotationType::Text, rect));
    };

    makeCreateOn(0);
    makeCreateOn(1);  // second command references a different page

    const size_t p0Before = manager->getAnnotations(commandPages[0].get()).size();
    const size_t p1Before = manager->getAnnotations(commandPages[1].get()).size();
    ASSERT_GT(p0Before, 0u);
    ASSERT_GT(p1Before, 0u);

    document->markRequiresFullRewrite();

    ASSERT_NO_THROW({
        EXPECT_TRUE(pdfe::core::saveWithCommandPageRebind(
            *document, *undoManager, commandPages, tempPdf));
    });

    // Undo both, most-recent first (page 1, then page 0). Each command must act
    // on the correct rebound page — a mis-rebind would undo on the wrong page
    // or dereference a dangling pointer.
    ASSERT_TRUE(undoManager->canUndo());
    ASSERT_NO_THROW(undoManager->undo());  // undoes the page-1 create
    ASSERT_TRUE(undoManager->canUndo());
    ASSERT_NO_THROW(undoManager->undo());  // undoes the page-0 create

    // Both pages must be resolvable among the rebound holders and back to their
    // pre-create annotation counts.
    Page* p0 = nullptr;
    Page* p1 = nullptr;
    for (auto& holder : commandPages) {
        if (!holder) continue;
        if (holder->index() == 0) p0 = holder.get();
        if (holder->index() == 1) p1 = holder.get();
    }
    ASSERT_NE(p0, nullptr);
    ASSERT_NE(p1, nullptr);
    EXPECT_EQ(manager->getAnnotations(p0).size(), p0Before - 1);
    EXPECT_EQ(manager->getAnnotations(p1).size(), p1Before - 1);
}

// Save As (new path) must also drain + rebind: saveAs writes a new file (no
// in-place lock) but drops+reopens the page tree, so a post-Save-As undo would
// dangle without the rebind.
TEST_F(CommandPageRebindTest, SaveAsAlsoDrainsAndRebinds) {
    std::vector<std::unique_ptr<Page>> commandPages;
    commandPages.push_back(document->page(0));
    ASSERT_NE(commandPages.back(), nullptr);
    Page* pagePtr = commandPages.back().get();

    QRectF rect(90, 90, 140, 44);
    auto cmd = std::make_unique<pdfe::annotations::CreateAnnotationCommand>(
        manager.get(), pagePtr, pdfe::annotations::AnnotationType::Text, rect);
    undoManager->execute(std::move(cmd));
    const size_t countAfterCreate = manager->getAnnotations(pagePtr).size();
    ASSERT_GT(countAfterCreate, 0u);

    const std::filesystem::path outPath =
        std::filesystem::temp_directory_path() /
        "pdfe_cmd_page_rebind_saveas_f4a82c1e.pdf";
    std::error_code ec;
    std::filesystem::remove(outPath, ec);
    document->markRequiresFullRewrite();

    ASSERT_NO_THROW({
        EXPECT_TRUE(pdfe::core::saveAsWithCommandPageRebind(
            *document, *undoManager, commandPages, outPath));
    });

    // The new file exists and reopens cleanly with the annotation.
    {
        auto verifyCtx = std::make_shared<MuPdfContext>();
        auto reopened = Document::open(verifyCtx, outPath);
        ASSERT_NE(reopened, nullptr);
        EXPECT_EQ(reopened->pageCount(), 3);
    }

    // Undo survives the Save As (command rebound, not dangling).
    ASSERT_TRUE(undoManager->canUndo());
    ASSERT_NO_THROW(undoManager->undo());
    Page* reboundPage = commandPages.empty() ? nullptr : commandPages.back().get();
    ASSERT_NE(reboundPage, nullptr);
    EXPECT_EQ(manager->getAnnotations(reboundPage).size(), countAfterCreate - 1);

    std::filesystem::remove(outPath, ec);
}

// Redaction MARK commands ride the ordinary undo stack (mark a redaction ->
// CreateRedactionCommand, then File>Save), so they must survive the save like
// any other family. Without the rebind contract the drain would leave their
// page_ dangling and a post-save undo would UAF.
TEST_F(CommandPageRebindTest, RedactionMarkCommandSurvivesSave) {
    pdfe::redaction::RedactionManager redMgr(context);

    std::vector<std::unique_ptr<Page>> commandPages;
    commandPages.push_back(document->page(0));
    ASSERT_NE(commandPages.back(), nullptr);
    Page* pagePtr = commandPages.back().get();

    QRectF rect(80, 80, 120, 30);
    auto cmd = std::make_unique<pdfe::redaction::CreateRedactionCommand>(
        &redMgr, pagePtr, rect);
    undoManager->execute(std::move(cmd));
    const size_t marksAfterCreate = redMgr.getRedactionMarks(pagePtr).size();
    ASSERT_GT(marksAfterCreate, 0u);

    document->markRequiresFullRewrite();
    ASSERT_NO_THROW({
        EXPECT_TRUE(pdfe::core::saveWithCommandPageRebind(
            *document, *undoManager, commandPages, tempPdf));
    }) << "ordinary save with a redaction mark on the undo stack must not lock";

    // Undo must operate on the rebound page, not a dangling one.
    ASSERT_TRUE(undoManager->canUndo());
    ASSERT_NO_THROW(undoManager->undo());
    Page* reboundPage = commandPages.empty() ? nullptr : commandPages.back().get();
    ASSERT_NE(reboundPage, nullptr);
    EXPECT_EQ(redMgr.getRedactionMarks(reboundPage).size(), marksAfterCreate - 1)
        << "undo must remove the redaction mark on the rebound page";
}

// A minimal command that only holds a Page* and implements the rebind contract
// exactly like the real families (Annotation/Image/Form/Signature all use the
// identical `page_ ? page_->index() : -1` / `page_ = page` bodies). Proves the
// orchestration rebinds ANY page-reporting command generically, which is what
// makes those per-family overrides work.
class FakePageCommand : public pdfe::commands::Command {
public:
    explicit FakePageCommand(Page* page) : page_(page) {}
    void execute() override {}
    void undo() override {}
    [[nodiscard]] std::string description() const override { return "fake"; }
    [[nodiscard]] int pageIndex() const override {
        return page_ ? page_->index() : -1;
    }
    void rebindPage(Page* page) override {
        page_ = page;
        ++rebindCount_;
    }
    Page* page() const { return page_; }
    int rebindCount() const { return rebindCount_; }
private:
    Page* page_;
    int rebindCount_ = 0;
};

TEST_F(CommandPageRebindTest, RebindsAnyPageReportingCommandGenerically) {
    std::vector<std::unique_ptr<Page>> commandPages;
    commandPages.push_back(document->page(1));
    ASSERT_NE(commandPages.back(), nullptr);
    Page* held = commandPages.back().get();
    const int idx = held->index();

    auto* fake = new FakePageCommand(held);
    undoManager->execute(std::unique_ptr<pdfe::commands::Command>(fake));

    document->markRequiresFullRewrite();
    ASSERT_NO_THROW({
        EXPECT_TRUE(pdfe::core::saveWithCommandPageRebind(
            *document, *undoManager, commandPages, tempPdf));
    });

    // The orchestration actually invoked rebindPage() (exactly once), and the
    // command now points at a live page owned by the re-resolved commandPages
    // — not the dropped original. (Asserting rebindCount() rather than pointer
    // identity avoids a false pass when the allocator reuses the freed page's
    // address for the newly-resolved one.)
    EXPECT_EQ(fake->rebindCount(), 1)
        << "orchestration must rebind the page-reporting command exactly once";
    EXPECT_NE(fake->page(), nullptr);
    EXPECT_EQ(fake->pageIndex(), idx) << "rebound page must have the same index";

    bool amongHolders = false;
    for (auto& h : commandPages) {
        if (h && h.get() == fake->page()) amongHolders = true;
    }
    EXPECT_TRUE(amongHolders)
        << "rebound page must be owned by the re-resolved commandPages";
}

// ─── M09 kill (save routing, R-08/R-21/R-23) ───────────────────────────────
// effectiveGarbageLevel() escalates a full-rewrite save to do_garbage=3 (object
// dedup + compact) exactly when a dedup-worthy op ran (needsFullGc_). The
// existing save-path tests validate the written file only by reopen+pageCount,
// so a mutant downgrading the escalated case to gc=1 (skipping dedup, bloating
// the file) survived. This differential oracle proves the dedup actually
// happens: the SAME source, saved with needsFullGc_ set (gc=3), must contain
// STRICTLY FEWER objects than saved without it (gc=1). Under the mutant both
// saves route to gc=1, the counts match, and the strict inequality fails.
//
// The fixture carries three byte-identical, all-referenced font objects: gc=1
// keeps all three (they are used), gc=3 merges them into one and compacts.
TEST_F(CommandPageRebindTest, FullGcEscalationDedupsDuplicateObjects) {
    std::filesystem::path fixture =
        std::filesystem::path(TEST_FIXTURES_DIR) / "dedup_duplicate_objects.pdf";
    ASSERT_TRUE(std::filesystem::exists(fixture))
        << "dedup fixture must be present (CMake fixture floor): " << fixture;

    auto tmpDir = std::filesystem::temp_directory_path();
    std::filesystem::path outGc1 = tmpDir / "pdfe_m09_gc1_f4a82c1e.pdf";
    std::filesystem::path outGc3 = tmpDir / "pdfe_m09_gc3_f4a82c1e.pdf";
    std::error_code ec;
    std::filesystem::remove(outGc1, ec);
    std::filesystem::remove(outGc3, ec);

    // (a) Full-rewrite save WITHOUT needsFullGc_ → effectiveGarbageLevel()==1
    //     (no dedup); all three duplicate-but-referenced fonts are retained.
    {
        auto ctx = std::make_shared<MuPdfContext>();
        auto doc = Document::open(ctx, fixture);
        ASSERT_NE(doc, nullptr);
        ASSERT_EQ(doc->effectiveGarbageLevel(), 1)
            << "guard: an unmarked document must use the cheap default gc level";
        ASSERT_NO_THROW(doc->saveAs(outGc1));
    }

    // (b) Full-rewrite save WITH needsFullGc_ → effectiveGarbageLevel()==3
    //     (dedup): the three identical font objects merge into one.
    {
        auto ctx = std::make_shared<MuPdfContext>();
        auto doc = Document::open(ctx, fixture);
        ASSERT_NE(doc, nullptr);
        doc->markNeedsFullGc();
        ASSERT_EQ(doc->effectiveGarbageLevel(), 3)
            << "guard: markNeedsFullGc() must escalate to gc=3 (the mutant "
               "downgrades this to gc=1, which this oracle then catches)";
        ASSERT_NO_THROW(doc->saveAs(outGc3));
    }

    const int nGc1 = countPdfObjects(outGc1);
    const int nGc3 = countPdfObjects(outGc3);
    ASSERT_GT(nGc1, 0) << "gc=1 output must be a readable PDF";
    ASSERT_GT(nGc3, 0) << "gc=3 output must be a readable PDF";

    EXPECT_LT(nGc3, nGc1)
        << "gc=3 (needsFullGc escalation) must DEDUP identical objects, yielding "
           "strictly fewer objects than gc=1 (" << nGc3 << " vs " << nGc1
           << "). If they match, the full-GC escalation was downgraded to gc=1 "
              "and dedup did not run.";

    std::filesystem::remove(outGc1, ec);
    std::filesystem::remove(outGc3, ec);
}

// ── PDF-99 (meta-audit gap register, issue #35) ──────────────────────────────
//
// Every reopen check above stops at `pageCount() == 3`. A page count is the
// weakest possible oracle for a full rewrite: it survives a save that wrote
// the pre-edit document, a save that dropped every annotation, and a save that
// wrote all of them onto the wrong page. All three are the failure the
// drain-and-rebind path exists to make impossible, and none of them is a page
// count away from correct.
//
// This asserts the reopened file carries the right (type, rect, contents)
// tuples on the right PAGES — including that the page given nothing still has
// nothing, which is what catches a save that piles everything onto one page.
//
// ANCHORED TO THE INPUTS, NOT TO THE SESSION. The obvious way to write this is
// "capture the tuples before the save, compare with the tuples after". That
// comparison is worth less than it looks: both sides run through the same
// creation and read-back code, so a defect that writes an annotation's text to
// the wrong index puts the same wrong answer on both sides and the comparison
// passes. (Measured: this is exactly what MUTANT-4 does, and the
// before-vs-after form of this test did not notice.)
//
// So the expected tuples are the LITERALS handed to the create calls, and BOTH
// the pre-save session and the reopened file are checked against them. The
// pre-save check is not redundant — it is what separates "the save lost it"
// from "it was never there".

struct AnnotTuple {
    int type;
    double x, y, w, h;
    std::string contents;
};

std::vector<AnnotTuple> tuplesFor(pdfe::annotations::AnnotationManager& mgr,
                                  Page* page) {
    std::vector<AnnotTuple> out;
    for (const auto& a : mgr.getAnnotations(page)) {
        out.push_back(AnnotTuple{static_cast<int>(a.type),
                                 a.rect.x(), a.rect.y(),
                                 a.rect.width(), a.rect.height(),
                                 a.contents});
    }
    return out;
}

void expectTuplesMatch(const std::vector<AnnotTuple>& expected,
                       const std::vector<AnnotTuple>& actual,
                       int pageIndex, const char* stage) {
    ASSERT_EQ(actual.size(), expected.size())
        << stage << ": page " << pageIndex << " has " << actual.size()
        << " annotation(s), expected " << expected.size();
    for (size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(actual[i].type, expected[i].type)
            << stage << ": page " << pageIndex << " annotation " << i
            << " has the wrong type";
        EXPECT_EQ(actual[i].contents, expected[i].contents)
            << stage << ": page " << pageIndex << " annotation " << i
            << " carries \"" << actual[i].contents << "\", expected \""
            << expected[i].contents << "\"";
        // 0.5 pt: PDF rects round-trip through an ASCII decimal
        // representation, so exact float equality is not a property the format
        // offers. Half a point is far tighter than any real relocation.
        EXPECT_NEAR(actual[i].x, expected[i].x, 0.5)
            << stage << ": page " << pageIndex << " annotation " << i
            << " is at the wrong x";
        EXPECT_NEAR(actual[i].y, expected[i].y, 0.5)
            << stage << ": page " << pageIndex << " annotation " << i
            << " is at the wrong y";
        EXPECT_NEAR(actual[i].w, expected[i].w, 0.5)
            << stage << ": page " << pageIndex << " annotation " << i
            << " has the wrong width";
        EXPECT_NEAR(actual[i].h, expected[i].h, 0.5)
            << stage << ": page " << pageIndex << " annotation " << i
            << " has the wrong height";
    }
}

TEST_F(CommandPageRebindTest, RebindSavePreservesPerPageAnnotationTuples) {
    std::vector<std::unique_ptr<Page>> commandPages;

    // Two annotations on page 0 and one on page 2 — page 1 deliberately stays
    // empty so "everything landed on one page" is a detectable outcome.
    auto annotateOn = [&](int pageIdx, const QRectF& rect, const char* text) {
        commandPages.push_back(document->page(pageIdx));
        EXPECT_NE(commandPages.back(), nullptr);
        Page* p = commandPages.back().get();
        undoManager->execute(
            std::make_unique<pdfe::annotations::CreateAnnotationCommand>(
                manager.get(), p, pdfe::annotations::AnnotationType::Square,
                rect));
        const auto created = manager->getAnnotations(p);
        EXPECT_FALSE(created.empty());
        if (!created.empty())
            manager->setContents(p, created.back().annotIndex, text);
    };

    annotateOn(0, QRectF(72, 90, 144, 48), "PDF99_PAGE0_FIRST");
    annotateOn(0, QRectF(72, 200, 100, 30), "PDF99_PAGE0_SECOND");
    annotateOn(2, QRectF(120, 310, 180, 60), "PDF99_PAGE2_ONLY");

    // THE ORACLE: restated from the literals above, not read back from the
    // manager. Page 1 is deliberately empty so "everything landed on one page"
    // is a detectable outcome and not merely an unmentioned one.
    const int kSquare =
        static_cast<int>(pdfe::annotations::AnnotationType::Square);
    // A Square annotation's /Rect is the requested rectangle GROWN by its
    // border, which defaults to 1 pt (AnnotationData::borderWidth) — the border
    // is stroked centred on the edge, so half of it falls outside and the box
    // has to contain it. Measured on a clean tree: the requested (72, 90,
    // 144x48) reads back as (71, 89, 146x50). That is a stated, understood
    // transform of the input, not a value copied out of the manager: the
    // oracle is still "what I asked for", expressed in the coordinates the
    // format actually stores.
    const double kBorder = 1.0;
    auto grown = [&](double x, double y, double w, double h,
                     const char* text) {
        return AnnotTuple{kSquare, x - kBorder, y - kBorder,
                          w + 2 * kBorder, h + 2 * kBorder, text};
    };
    const std::vector<std::vector<AnnotTuple>> expected = {
        { grown( 72,  90, 144, 48, "PDF99_PAGE0_FIRST"),
          grown( 72, 200, 100, 30, "PDF99_PAGE0_SECOND") },
        { },
        { grown(120, 310, 180, 60, "PDF99_PAGE2_ONLY") },
    };

    // Pre-save: the session itself must already match the oracle. Without this
    // half, a reopened-file mismatch could not distinguish "the save lost it"
    // from "it was never created correctly".
    for (int i = 0; i < 3; ++i) {
        auto p = document->page(i);
        ASSERT_NE(p, nullptr);
        expectTuplesMatch(expected[i], tuplesFor(*manager, p.get()), i,
                          "pre-save session");
    }

    document->markRequiresFullRewrite();
    ASSERT_NO_THROW({
        EXPECT_TRUE(pdfe::core::saveWithCommandPageRebind(
            *document, *undoManager, commandPages, tempPdf));
    });

    auto verifyCtx = std::make_shared<MuPdfContext>();
    auto reopened = Document::open(verifyCtx, tempPdf);
    ASSERT_NE(reopened, nullptr);
    ASSERT_EQ(reopened->pageCount(), 3);

    pdfe::annotations::AnnotationManager reMgr(verifyCtx);
    for (int i = 0; i < 3; ++i) {
        auto p = reopened->page(i);
        ASSERT_NE(p, nullptr);
        expectTuplesMatch(expected[i], tuplesFor(reMgr, p.get()), i,
                          "after save->reopen");
    }
}

}  // namespace
