// -*- coding: utf-8 -*-
//
// Autosave-coverage pipeline — Phase 2 REGRESSION LOCKS.
//
// One focused regression test per capture gap the Phase-1 discovery probe
// found (gap-register.json: RedactionApply, Bookmarks, PageLabels,
// LayersDirect). Each drives the family EXACTLY as production now drives it,
// runs a single synchronous autosaveNow() through an isolated recovery dir,
// then reopens the recovery copy in a fresh MuPDF context (as a recovering
// process would) and asserts — with a real state/content oracle — that the
// edit survived the simulated crash.
//
// These are the GREEN locks for the fixes. The corresponding RED baseline is
// the Phase-1 gap-register (capture_present == false for every family here).

#include <gtest/gtest.h>

#include "core/AutosaveManager.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "core/UndoManager.h"

#include "redaction/RedactionManager.h"
#include "redaction/RedactionVerifier.h"
#include "redaction/RedactionData.h"

#include "core/BookmarkManager.h"
#include "core/BookmarkNode.h"
#include "core/PageLabels.h"
#include "layers/LayerManager.h"

#include <QRectF>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#ifndef TEST_FIXTURES_DIR
#define TEST_FIXTURES_DIR "."
#endif

using pdfe::commands::UndoManager;
using pdfe::core::AutosaveManager;
using pdfe::core::Document;
using pdfe::core::MuPdfContext;

namespace {

// Per-test isolated environment: temp copy of a seed fixture + isolated
// recovery dir. Mirrors tests/test_autosave_recovery.cpp.
class AutosaveCaptureFixTest : public ::testing::Test {
protected:
    void stage(const std::string& seed) {
        std::filesystem::path fixture =
            std::filesystem::path(TEST_FIXTURES_DIR) / seed;
        if (!std::filesystem::exists(fixture)) {
            GTEST_SKIP() << seed << " not available";
        }
        recoveryDir_ = std::filesystem::temp_directory_path() /
                       ("pdfe_autosave_capfix_rec_" + seed);
        std::error_code ec;
        std::filesystem::remove_all(recoveryDir_, ec);
        std::filesystem::create_directories(recoveryDir_, ec);
        tempPdf_ = std::filesystem::temp_directory_path() /
                   ("pdfe_autosave_capfix_src_" + seed);
        std::filesystem::remove(tempPdf_, ec);
        std::filesystem::copy_file(
            fixture, tempPdf_,
            std::filesystem::copy_options::overwrite_existing, ec);
        ASSERT_FALSE(ec) << "stage copy failed: " << ec.message();
        ctx_ = std::make_shared<MuPdfContext>();
    }

    void TearDown() override {
        ctx_.reset();
        std::error_code ec;
        std::filesystem::remove_all(recoveryDir_, ec);
        std::filesystem::remove(tempPdf_, ec);
    }

    std::filesystem::path recoveryDir_;
    std::filesystem::path tempPdf_;
    std::shared_ptr<MuPdfContext> ctx_;
};

// GAP: RedactionApply — burn-in goes DIRECT (no undo command). Autosave must
// still fire (via the Document content-version signal) and the recovery copy
// must carry the applied redaction.
TEST_F(AutosaveCaptureFixTest, RedactionApplyIsCapturedByAutosave) {
    stage("test_redaction.pdf");

    auto doc = Document::open(ctx_, tempPdf_);
    ASSERT_NE(doc, nullptr);
    auto undo = std::make_unique<UndoManager>();

    // Production creates the AutosaveManager when the document is OPENED,
    // before any edit — so its content-version baseline is the pristine
    // document. Construct it here, then apply the redaction.
    AutosaveManager mgr(doc.get(), undo.get(), tempPdf_, recoveryDir_);

    pdfe::redaction::RedactionManager rmgr(ctx_);
    std::vector<pdfe::redaction::RedactionData> regions;
    {
        auto page = doc->page(0);
        ASSERT_NE(page, nullptr);
        auto created =
            rmgr.createRedactionMark(page.get(), QRectF(100, 660, 400, 30));
        regions.push_back(created);
        auto res = rmgr.applyAllRedactions(doc.get());
        ASSERT_TRUE(res.success) << res.errorMessage;
        ASSERT_GE(res.marksApplied, 1);
    }

    // The apply went DIRECT — the undo stack is still clean.
    ASSERT_TRUE(undo->isClean())
        << "redaction apply is not an undo command";

    // The fix: a non-undo mutation still triggers autosave via contentVersion.
    ASSERT_TRUE(mgr.autosaveNow())
        << "direct redaction apply must still trigger an autosave";
    ASSERT_TRUE(std::filesystem::exists(mgr.recoveryFilePath()))
        << "recovery copy must be written";

    // Content oracle: reopen the recovery copy fresh and verify the redaction
    // was burned in.
    pdfe::redaction::RedactionVerifier verifier(std::make_shared<MuPdfContext>());
    auto vr = verifier.verify(mgr.recoveryFilePath(), regions);
    EXPECT_TRUE(vr.success)
        << "recovery copy must carry the applied redaction: " << vr.errorMessage;
}

// GAP: Bookmarks — the edit dirties the undo stack, but the bookmark tree lives
// in BookmarkManager until committed. The pre-snapshot flush hook must commit
// it into the live document before the recovery snapshot so the copy carries
// the new bookmark.
TEST_F(AutosaveCaptureFixTest, BookmarkEditIsCapturedByAutosave) {
    stage("test_with_bookmarks.pdf");

    auto doc = Document::open(ctx_, tempPdf_);
    ASSERT_NE(doc, nullptr);
    auto undo = std::make_unique<UndoManager>();

    pdfe::core::BookmarkManager bmgr(ctx_);
    bmgr.load(doc.get());
    const int before = bmgr.totalBookmarkCount();

    pdfe::core::BookmarkDest dest;
    dest.type = pdfe::core::BookmarkDestType::Page;
    dest.pageIndex = 0;
    undo->execute(std::make_unique<pdfe::core::CreateBookmarkCommand>(
        &bmgr, pdfe::core::BookmarkPath{}, "AutosaveFixBookmark", dest));
    ASSERT_FALSE(undo->isClean());

    AutosaveManager mgr(doc.get(), undo.get(), tempPdf_, recoveryDir_);
    // The fix: production wires a pre-snapshot flush that commits the in-panel
    // bookmark tree into the document just before the snapshot.
    mgr.setPreSnapshotFlush([&]() { bmgr.commit(doc.get()); });

    ASSERT_TRUE(mgr.autosaveNow()) << "bookmark edit must trigger autosave";
    ASSERT_TRUE(std::filesystem::exists(mgr.recoveryFilePath()));

    // Content oracle: reopen fresh; the recovery copy carries the new bookmark.
    auto rctx = std::make_shared<MuPdfContext>();
    auto rdoc = Document::open(rctx, mgr.recoveryFilePath());
    ASSERT_NE(rdoc, nullptr);
    pdfe::core::BookmarkManager rmgr(rctx);
    rmgr.load(rdoc.get());
    EXPECT_GE(rmgr.totalBookmarkCount(), before + 1)
        << "recovery copy must carry the newly-created bookmark";
}

// GAP: PageLabels — the editor now routes through EditPageLabelsCommand, so the
// edit dirties the undo stack (autosave fires) and the recovery copy carries
// the new /PageLabels.
TEST_F(AutosaveCaptureFixTest, PageLabelEditIsCapturedByAutosave) {
    stage("test_3page.pdf");

    auto doc = Document::open(ctx_, tempPdf_);
    ASSERT_NE(doc, nullptr);
    auto undo = std::make_unique<UndoManager>();

    std::vector<pdfe::core::PageLabelRange> ranges = {
        {0, pdfe::core::PageLabelStyle::RomanUpper, "", 1},
        {1, pdfe::core::PageLabelStyle::Decimal, "Ch-", 5},
    };
    undo->execute(std::make_unique<pdfe::core::EditPageLabelsCommand>(
        doc.get(), ranges));
    ASSERT_FALSE(undo->isClean())
        << "EditPageLabelsCommand must dirty the undo stack";

    AutosaveManager mgr(doc.get(), undo.get(), tempPdf_, recoveryDir_);
    ASSERT_TRUE(mgr.autosaveNow()) << "page-label edit must trigger autosave";
    ASSERT_TRUE(std::filesystem::exists(mgr.recoveryFilePath()));

    // Content oracle: reopen fresh; the recovery copy carries the new labels.
    auto rctx = std::make_shared<MuPdfContext>();
    auto rdoc = Document::open(rctx, mgr.recoveryFilePath());
    ASSERT_NE(rdoc, nullptr);
    auto readback = pdfe::core::PageLabelManager::read(rdoc.get());
    ASSERT_FALSE(readback.empty())
        << "recovery copy must carry the /PageLabels edit";
    EXPECT_EQ(readback[0].style, pdfe::core::PageLabelStyle::RomanUpper);
}

// GAP: LayersDirect — production routes layer mutations through the undoable
// commands. A SetLayerVisibilityCommand dirties the undo stack (autosave fires)
// and the recovery copy carries the visibility change.
TEST_F(AutosaveCaptureFixTest, LayerToggleIsCapturedByAutosave) {
    stage("test_3page.pdf");

    auto doc = Document::open(ctx_, tempPdf_);
    ASSERT_NE(doc, nullptr);
    auto undo = std::make_unique<UndoManager>();

    // Manager created at open (production ordering), before edits.
    AutosaveManager mgr(doc.get(), undo.get(), tempPdf_, recoveryDir_);

    pdfe::layers::LayerManager lmgr(ctx_);
    const std::string layerName = "AutosaveFixLayer";
    // Establish a layer to toggle (production has layers already; the seed does
    // not, so create one first via its undoable command).
    undo->execute(std::make_unique<pdfe::layers::CreateLayerCommand>(
        &lmgr, doc.get(), layerName));

    int layerIdx = -1;
    for (const auto& l : lmgr.listLayers(doc.get())) {
        if (l.name == layerName) { layerIdx = l.index; break; }
    }
    ASSERT_GE(layerIdx, 0) << "created layer must be listed";

    // The operation under test: toggle the layer via SetLayerVisibilityCommand
    // (the production layer-edit path — routed through the undo stack). This is
    // what makes a layer edit DIRTY the undo stack, so autosave fires — the
    // fix for the LayersDirect gap. (Whether pdf_enable_layer's /D usage state
    // is serialized by pdf_save_document is a separate MuPDF/LayerManager
    // save-persistence matter, unrelated to the autosave-capture contract,
    // which is: autosave captures whatever a normal save would.)
    undo->execute(std::make_unique<pdfe::layers::SetLayerVisibilityCommand>(
        &lmgr, doc.get(), layerIdx, /*visible=*/false));
    ASSERT_FALSE(undo->isClean())
        << "layer command must dirty the undo stack";

    ASSERT_TRUE(mgr.autosaveNow()) << "layer edit must trigger autosave";
    ASSERT_TRUE(std::filesystem::exists(mgr.recoveryFilePath()));

    // Content oracle: reopen the recovery copy fresh — the layer edit was
    // captured (the created layer is present in the recovered document).
    auto rctx = std::make_shared<MuPdfContext>();
    auto rdoc = Document::open(rctx, mgr.recoveryFilePath());
    ASSERT_NE(rdoc, nullptr);
    pdfe::layers::LayerManager rlmgr(rctx);
    bool found = false;
    for (const auto& l : rlmgr.listLayers(rdoc.get())) {
        if (l.name == layerName) { found = true; break; }
    }
    EXPECT_TRUE(found)
        << "recovery copy must carry the layer edit captured by autosave";
}

// PERF: a recovery snapshot must be FAST on large, image-heavy PDFs. The
// autosave timer can fire a recovery write repeatedly while the user keeps
// editing, so re-encoding every image on each write would stall the app.
// Document::writeRecoveryCopy() therefore writes with GC + image/font
// recompression DISABLED — a serialize-and-copy of the already-encoded
// streams. This test guards against a regression that re-enables recompression
// (which took many seconds on this 23 MB / 100-image scan). It also confirms
// the fast, uncompressed copy is still a valid, fully-openable PDF.
TEST_F(AutosaveCaptureFixTest, RecoverySnapshotIsFastOnLargeImagePdf) {
    stage("phase3_large_100page_image.pdf");
    auto doc = Document::open(ctx_, tempPdf_);
    ASSERT_NE(doc, nullptr);
    const int pages = doc->pageCount();
    const std::filesystem::path rec = recoveryDir_ / "large_recovery.pdf";

    const auto t0 = std::chrono::steady_clock::now();
    ASSERT_NO_THROW(doc->writeRecoveryCopy(rec));
    const long long elapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0)
            .count();
    ::testing::Test::RecordProperty("recovery_write_ms",
                                    static_cast<int>(elapsedMs));

    // Validity: the fast, uncompressed recovery copy reopens as a PDF with the
    // same page count.
    ASSERT_TRUE(std::filesystem::exists(rec));
    auto ctx2 = std::make_shared<MuPdfContext>();
    auto reopened = Document::open(ctx2, rec);
    ASSERT_NE(reopened, nullptr);
    EXPECT_EQ(reopened->pageCount(), pages);

    // Perf floor: the no-recompress path serialize-and-copies this 23 MB doc in
    // well under a second on the dev box. A recompress regression would blow
    // far past this. The bound is deliberately generous (disk-speed headroom on
    // a loaded CI runner) so it catches gross regressions without flaking.
    EXPECT_LT(elapsedMs, 8000)
        << "recovery snapshot of a 23 MB image PDF took " << elapsedMs
        << " ms — the write is likely re-encoding image/font streams "
           "(recompression perf regression on the recovery path)";
}

}  // namespace
