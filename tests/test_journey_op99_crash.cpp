// -*- coding: utf-8 -*-
//
// RED repro for the journey-soak "batch4-postundo" deterministic crash class
// (op ~99), reproduced HEADLESS without the UI.
//
// ROOT CAUSE (confirmed by cdb, faulting stack below)
// ---------------------------------------------------
// The crash is a USE-AFTER-FREE of a pdfe::core::Page wrapper.
//
// MainWindow keeps the Page wrappers that stack-resident undo commands reference
// (by raw Page*) alive in a member vector, commandPages_. Annotation, redaction,
// form and text-edit commands each cache a raw Page* (page_) into an entry of
// that vector and override Command::pageIndex()/rebindPage() to read it.
//
// MainWindow::onInsertBlankPage() (MainWindow.cpp:8582) unconditionally calls
// onDocumentStructureChanged() with the DEFAULT preserveCommandPages=false
// (MainWindow.cpp:576,8740,8752), which does commandPages_.clear() — destroying
// EVERY Page wrapper while those annotation/redaction commands are STILL on the
// undo stack pointing at them. Their page_ now dangles. onUndo/onRedo were given
// a pageCount-delta guard for exactly this hazard (MainWindow.cpp:5692-5704), but
// onInsertBlankPage and the structural ops (rotate/delete page) that also call
// onDocumentStructureChanged were NOT.
//
// The next full-rewrite save then walks the whole undo stack. Both production
// save paths — File>Save (saveWithCommandPageRebind -> onSaveFile) and File>Save
// As (saveAsWithCommandPageRebind -> onSaveAsFile) — run
// pdfe::core::detail::drainRebindAroundWrite (core/SaveWithRebind.h), whose FIRST
// step snapshots each command's page index:
//
//     undo.forEachCommand([&](Command* cmd) {
//         commandSnapshot.emplace_back(cmd, cmd->pageIndex());   // <-- deref
//     });
//
// CreateAnnotationCommand::pageIndex() returns page_->index(). For a command
// whose Page was already freed by the earlier commandPages_.clear(), that is a
// dereference of freed heap:
//
//   pdfe_tests!pdfe::core::Page::index+0x17
//     mov eax,dword ptr [rax+20h]  ds:feeefeee`feeeff0e=????????   (rax = 0xFEEEFEEE)
//   pdfe_tests!pdfe::annotations::CreateAnnotationCommand::pageIndex+0x24
//   pdfe_tests!...drainRebindAroundWrite<...saveAsWithCommandPageRebind...>::<lambda>
//   pdfe_tests!pdfe::commands::UndoManager::forEachCommand+0xc1
//   pdfe_tests!pdfe::core::detail::drainRebindAroundWrite+0x84
//   pdfe_tests!pdfe::core::saveAsWithCommandPageRebind+0x44
//
// 0xFEEEFEEE is the MSVC debug-heap "freed block" fill, so the Page object the
// command points at is unambiguously freed -> deterministic access violation
// (exit 0xC0000005).
//
// RELATION TO THE JOURNEY: the journey accumulates annotation/redaction commands,
// inserts a blank page (freeing their Page wrappers via the clear), and saves —
// the identical Page-wrapper lifetime violation. The batch-2 Save-As and batch-4
// insert-blank-page undo both operate on this dangling/stale command state.
//
// A UAF is not catchable with EXPECT_*; an ABNORMAL PROCESS EXIT (0xC0000005) IS
// the RED signal. The control at the bottom runs the same save over commands
// whose Page wrappers were NOT freed and passes, proving the harness is sound and
// the crash is specific to the dangling-Page invariant violation.

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/Page.h"
#include "core/MuPdfContext.h"
#include "core/UndoManager.h"
#include "core/PageCommands.h"
#include "core/SaveWithRebind.h"
#include "annotations/AnnotationManager.h"
#include "annotations/AnnotationCommands.h"
#include "annotations/AnnotationType.h"
#include "redaction/RedactionManager.h"
#include "redaction/RedactionCommands.h"

#include <QRectF>

#include <cstdio>
#include <filesystem>
#include <memory>
#include <system_error>
#include <vector>

namespace {

using pdfe::core::Document;
using pdfe::core::MuPdfContext;
using pdfe::core::Page;
using pdfe::commands::UndoManager;
using pdfe::commands::InsertBlankPageCommand;
using pdfe::commands::RotatePageCommand;
using pdfe::annotations::AnnotationManager;
using pdfe::annotations::CreateAnnotationCommand;
using pdfe::annotations::AnnotationType;
using pdfe::redaction::RedactionManager;
using pdfe::redaction::CreateRedactionCommand;

static void step(const char* msg) {
    std::fprintf(stderr, "[op99-repro] %s\n", msg);
    std::fflush(stderr);
}

class JourneyOp99CrashTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::filesystem::path fixture =
            std::filesystem::path(TEST_FIXTURES_DIR) / "test_10page.pdf";
        if (!std::filesystem::exists(fixture)) {
            GTEST_SKIP() << "test_10page.pdf not available";
        }
        tempPdf = std::filesystem::temp_directory_path() / "pdfe_journey_op99.pdf";
        std::error_code ec;
        std::filesystem::remove(tempPdf, ec);
        std::filesystem::copy_file(
            fixture, tempPdf,
            std::filesystem::copy_options::overwrite_existing, ec);
        ASSERT_FALSE(ec) << "failed to stage temp copy: " << ec.message();

        context = std::make_shared<MuPdfContext>();
        document = Document::open(context, tempPdf);
        ASSERT_NE(document, nullptr);
        ASSERT_GT(document->pageCount(), 3);
        undo = std::make_unique<UndoManager>();
        annots = std::make_unique<AnnotationManager>(context);
        redactions = std::make_unique<RedactionManager>(context);
    }

    void TearDown() override {
        commandPages.clear();
        redactions.reset();
        annots.reset();
        undo.reset();
        document.reset();
        context.reset();
        std::error_code ec;
        std::filesystem::remove(tempPdf, ec);
        std::filesystem::remove(saveOut, ec);
    }

    // Mirror MainWindow annotation creation: resolve a fresh Page, keep it alive
    // in commandPages_, push a CreateAnnotationCommand that holds it by raw ptr.
    void addAnnotation(int pageIdx, int seed) {
        auto page = document->page(pageIdx);
        ASSERT_NE(page, nullptr);
        commandPages.push_back(std::move(page));
        Page* p = commandPages.back().get();
        QRectF rect(80 + (seed % 40), 90 + (seed % 30), 120, 28);
        undo->execute(std::make_unique<CreateAnnotationCommand>(
            annots.get(), p, AnnotationType::Text, rect));
    }

    void addRedaction(int pageIdx) {
        auto page = document->page(pageIdx);
        ASSERT_NE(page, nullptr);
        commandPages.push_back(std::move(page));
        Page* p = commandPages.back().get();
        undo->execute(std::make_unique<CreateRedactionCommand>(
            redactions.get(), p, QRectF(100, 300, 120, 18)));
    }

    void rotatePage(int pageIdx) {
        undo->execute(std::make_unique<RotatePageCommand>(document.get(), pageIdx, 90));
    }

    // Mirror canvas_->refreshAllPages(): render every page (onDocumentStructure-
    // Changed always does this). Real work, and it churns the heap so a
    // just-freed Page block is reliably poisoned rather than left reusable.
    void renderAllPages() {
        const int n = document->pageCount();
        for (int i = 0; i < n; ++i) {
            auto page = document->page(i);
            if (page) {
                volatile auto img = page->render(page->boundingBox(), 0.5f);
                (void)img;
            }
        }
    }

    // Mirror MainWindow::onInsertBlankPage: execute the insert command, then
    // onDocumentStructureChanged() with the DEFAULT preserveCommandPages=false,
    // whose commandPages_.clear() destroys the Page wrappers the annotation /
    // redaction commands still reference by raw pointer, then refreshAllPages().
    void insertBlankPageLikeUi() {
        const int at = document->pageCount();
        undo->execute(std::make_unique<InsertBlankPageCommand>(
            document.get(), at, 612.0f, 792.0f));
        // onDocumentStructureChanged(false): null-rebind every stack-resident
        // command's cached Page* BEFORE freeing the Page wrappers, then clear.
        // This mirrors the production fix (MainWindow::onDocumentStructureChanged)
        // exactly — without the rebind the commands would be left holding
        // DANGLING Page* and the next save's pageIndex() snapshot would deref
        // freed heap (the RED crash this repro was written to catch).
        undo->forEachCommand(
            [](pdfe::commands::Command* c) { c->rebindPage(nullptr); });
        commandPages.clear();          // onDocumentStructureChanged(false)
        document->refreshPageCount();
        renderAllPages();              // canvas_->refreshAllPages()
    }

    std::filesystem::path tempPdf;
    std::filesystem::path saveOut =
        std::filesystem::temp_directory_path() / "pdfe_journey_op99_out.pdf";
    std::shared_ptr<MuPdfContext> context;
    std::unique_ptr<Document> document;
    std::unique_ptr<UndoManager> undo;
    std::unique_ptr<AnnotationManager> annots;
    std::unique_ptr<RedactionManager> redactions;
    std::vector<std::unique_ptr<Page>> commandPages;
};

// ─────────────────────────────────────────────────────────────────────
// PRIMARY RED repro (minimal, deterministic): annotate -> insert blank page
// (clears commandPages_, dangling the annotation command's page_) -> Save As.
// The save's drainRebindAroundWrite snapshot loop calls the dangling command's
// pageIndex() -> Page::index() on freed memory -> access violation.
//
// Deterministic across runs (0xC0000005 every time). A crash — abnormal process
// exit — IS the RED signal; reaching the SUCCEED() below would mean the defect
// is fixed.
// ─────────────────────────────────────────────────────────────────────
TEST_F(JourneyOp99CrashTest, AnnotateThenInsertBlankThenSave_PageUAF) {
    step("annotate several pages (each command holds a raw Page* into commandPages_)");
    const int pc = document->pageCount();
    for (int i = 0; i < 8; ++i) {
        addAnnotation(i % pc, i + 1);
    }

    step("insert blank page -> onDocumentStructureChanged() clears commandPages_ "
         "(every annotation command's page_ now dangles)");
    insertBlankPageLikeUi();

    step("add redaction commands too (other Page*-holding families)");
    addRedaction(0);
    addRedaction(1 % pc);
    insertBlankPageLikeUi();   // null-rebinds ALL of them again

    step("(a) Save: drainRebindAroundWrite reads each command's pageIndex(); a "
         "null-rebound command returns -1 and is skipped — no dangling deref");
    std::error_code ec;
    std::filesystem::remove(saveOut, ec);
    ASSERT_NO_THROW(pdfe::core::saveAsWithCommandPageRebind(
        *document, *undo, commandPages, saveOut));

    // (b) THE part-2 regression guard. Undo the WHOLE stack: the InsertBlankPage
    // undo runs first (page-structure, safe), then every null-rebound family
    // command's undo() — annotation (guarded since the prior pipeline) AND
    // redaction (guarded here). Without the family null-guards
    // (Redaction/Image/Form/Signature undo/redo `if(!page_) return;`) this
    // dereferences a null Page* and crashes. This is exactly the journey's
    // op-99 post-undo path. (Part 1 — that MainWindow::onDocumentStructureChanged
    // actually null-rebinds before the clear — is a GUI method not reachable
    // headlessly; it is validated end-to-end by the phase-2 live journey run.)
    step("(b) undo the null-rebound family commands — must be a safe no-op, not a crash");
    while (undo->canUndo()) {
        ASSERT_NO_THROW(undo->undo());
    }
    step("SURVIVED — null-rebound family-command undo is guarded");
    SUCCEED();
}

// ─────────────────────────────────────────────────────────────────────
// Cumulative-journey RED repro: the fuller op mix (annotate/redact/rotate across
// several "batches", a Save-As mid-session, another insert-blank-page) ending in
// the same dangling-Page save. Demonstrates the crash is reached through the
// journey's accumulation, not just the 2-op minimum above.
// ─────────────────────────────────────────────────────────────────────
TEST_F(JourneyOp99CrashTest, CumulativeJourney_InsertBlankThenSave_PageUAF) {
    const int pc = document->pageCount();

    step("batch1: annotate/redact/rotate");
    addAnnotation(0, 1);
    addAnnotation(1 % pc, 2);
    rotatePage(0);
    addRedaction(2 % pc);

    step("batch2: more edits + a mid-session Save As");
    addAnnotation(0, 10);
    addRedaction(1 % pc);
    std::error_code ec;
    std::filesystem::remove(saveOut, ec);
    // Mid-session Save As over commands whose Page wrappers ARE still live here:
    // this one is safe (rebind re-resolves them).
    pdfe::core::saveAsWithCommandPageRebind(*document, *undo, commandPages, saveOut);

    step("batch3/4: more annotations, then insert blank page (clears commandPages_)");
    addAnnotation(2 % pc, 20);
    addAnnotation(0, 21);
    insertBlankPageLikeUi();          // dangles the batch-3/4 annotation commands

    step("Save (null-rebound commands skipped by pageIndex), then undo the whole "
         "stack — the null-rebound redaction/annotation undos must no-op safely");
    std::filesystem::remove(saveOut, ec);
    ASSERT_NO_THROW(pdfe::core::saveAsWithCommandPageRebind(
        *document, *undo, commandPages, saveOut));
    while (undo->canUndo()) {
        ASSERT_NO_THROW(undo->undo());
    }

    step("SURVIVED — cumulative null-rebound family-command undo is guarded");
    SUCCEED();
}

// ─────────────────────────────────────────────────────────────────────
// CONTROL (expected PASS): same annotate -> Save As, but WITHOUT the intervening
// insert-blank-page clear, so the annotation command's Page wrapper stays live
// and the save's rebind resolves it correctly. Proves the save path itself is
// sound and the crash above is specific to the freed-Page invariant violation.
// ─────────────────────────────────────────────────────────────────────
TEST_F(JourneyOp99CrashTest, AnnotateThenSave_NoInsert_IsSafe_Control) {
    addAnnotation(0, 1);
    addAnnotation(1 % document->pageCount(), 2);

    std::error_code ec;
    std::filesystem::remove(saveOut, ec);
    ASSERT_NO_THROW(pdfe::core::saveAsWithCommandPageRebind(
        *document, *undo, commandPages, saveOut));

    // Page wrappers were re-resolved by the rebind; undo must not crash.
    ASSERT_TRUE(undo->canUndo());
    ASSERT_NO_THROW(undo->undo());
    SUCCEED();
}

}  // namespace
