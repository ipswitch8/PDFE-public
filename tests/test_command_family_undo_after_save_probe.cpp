// -*- coding: utf-8 -*-
//
// Round-2 finding A4 (probe) — {op → full-rewrite save → undo/redo} applied to
// the command families NOT covered by test_save_throw_undo_safety.cpp.
//
// WHAT A4 REQUIRES
// ----------------
// The undo-after-save UAF class (a command retaining a raw pdf_obj*/Page* that
// dangles once a full-rewrite save drops+reopens the document) has been proven
// present-and-fixed for the Page-holding families (Image/TextEdit/TRE +
// Annotation/Redaction/Form/Signature). A4 asks that the SAME probe be applied
// to the remaining families to CONFIRM they hold no raw pdf_obj across a save:
//   Metadata, Layer, Bookmark, OCR.
//
// STATIC RECON (why these are expected SAFE, not fixes)
// -----------------------------------------------------
//   EditMetadataCommand   — snapshots DocumentMetadata VALUE copies
//                           (old/new) + a stable Document*; no pdf_obj.
//   SetLayerVisibilityCommand — int layerIndex_ + bool + stable Document*.
//   CreateBookmarkCommand — BookmarkPath (vector<int>) + title + BookmarkDest
//                           VALUES + a stable BookmarkManager*; no pdf_obj.
//   OcrAllPagesCommand    — std::function callables + vector<int>; no doc/obj.
// So this probe is a PROOF, expected GREEN. If any family UNEXPECTEDLY reds
// (crash/UAF, hard under ASan), that is a newly-found defect: the family would
// then need the RetainedObjHolder rebind treatment like the others.
//
// THE ORACLE (real state, not EXPECT_NO_THROW-only)
// -------------------------------------------------
// For each family: open a fixture, apply the command via a real UndoManager
// with a live commandPages holder, perform an in-place FULL-REWRITE save via
// pdfe::core::saveWithCommandPageRebind (markRequiresFullRewrite first), then
// undo() and redo(), asserting NO crash/UAF AND a defined observable result:
//   Metadata — title value restored on undo, reapplied on redo.
//   Layer    — layer visibility restored on undo, reapplied on redo.
//   Bookmark — bookmark absent on undo, present again on redo.
//   OCR      — processed pages invalidated on undo, re-OCR'd on redo.

#include <gtest/gtest.h>

#include "core/SaveWithRebind.h"
#include "core/Document.h"
#include "core/Page.h"
#include "core/MuPdfContext.h"
#include "core/UndoManager.h"
#include "core/BookmarkManager.h"
#include "core/BookmarkNode.h"

#include "metadata/MetadataManager.h"
#include "metadata/MetadataCommands.h"
#include "layers/LayerManager.h"
#include "tre/OcrAllPagesCommand.h"
#include "signatures/SignatureManager.h"
#include "signatures/SignatureCommands.h"

#include <QRectF>

#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#ifndef TEST_FIXTURES_DIR
#define TEST_FIXTURES_DIR "."
#endif

namespace {

using pdfe::core::Document;
using pdfe::core::MuPdfContext;
using pdfe::core::Page;
using pdfe::commands::UndoManager;

enum class Family { Metadata, Layer, Bookmark, Ocr, Signature };

const char* familyName(Family f) {
    switch (f) {
        case Family::Metadata:  return "Metadata";
        case Family::Layer:     return "Layer";
        case Family::Bookmark:  return "Bookmark";
        case Family::Ocr:       return "Ocr";
        case Family::Signature: return "Signature";
    }
    return "?";
}

class CommandFamilyUndoAfterSaveProbe : public ::testing::TestWithParam<Family> {
protected:
    void SetUp() override {
        std::filesystem::path fixture =
            std::filesystem::path(TEST_FIXTURES_DIR) / "test_3page.pdf";
        if (!std::filesystem::exists(fixture)) {
            GTEST_SKIP() << "test_3page.pdf not available";
        }
        tempPdf_ = std::filesystem::temp_directory_path() /
                   (std::string("pdfe_famprobe_") + familyName(GetParam()) + ".pdf");
        std::error_code ec;
        std::filesystem::remove(tempPdf_, ec);
        std::filesystem::copy_file(
            fixture, tempPdf_,
            std::filesystem::copy_options::overwrite_existing, ec);
        ASSERT_FALSE(ec) << "failed to stage temp copy: " << ec.message();

        context_ = std::make_shared<MuPdfContext>();
    }

    void TearDown() override {
        context_.reset();
        std::error_code ec;
        std::filesystem::remove(tempPdf_, ec);
    }

    std::filesystem::path tempPdf_;
    std::shared_ptr<MuPdfContext> context_;
};

// ── Metadata ────────────────────────────────────────────────────────────────
void runMetadata(Document* doc, UndoManager& undo,
                 std::vector<std::unique_ptr<Page>>& commandPages,
                 const std::filesystem::path& savePath) {
    pdfe::metadata::MetadataManager mgr(doc->context());

    auto original = mgr.readMetadata(doc);
    const std::string probeTitle = "A4_PROBE_TITLE";

    pdfe::metadata::DocumentMetadata newMeta = original;
    newMeta.title = probeTitle;

    undo.execute(std::make_unique<pdfe::metadata::EditMetadataCommand>(
        &mgr, doc, newMeta));
    ASSERT_EQ(mgr.readMetadata(doc).title, probeTitle);

    doc->markRequiresFullRewrite();
    ASSERT_TRUE(pdfe::core::saveWithCommandPageRebind(*doc, undo, commandPages, savePath));

    // Save persisted the probe title; the reopened doc reflects it.
    EXPECT_EQ(mgr.readMetadata(doc).title, probeTitle)
        << "full-rewrite save must persist the edited metadata";

    ASSERT_TRUE(undo.canUndo());
    undo.undo();
    EXPECT_EQ(mgr.readMetadata(doc).title, original.title)
        << "undo after save must restore the original title";

    ASSERT_TRUE(undo.canRedo());
    undo.redo();
    EXPECT_EQ(mgr.readMetadata(doc).title, probeTitle)
        << "redo after save must reapply the edited title";
}

// ── Layer ───────────────────────────────────────────────────────────────────
void runLayer(Document* doc, UndoManager& undo,
              std::vector<std::unique_ptr<Page>>& commandPages,
              const std::filesystem::path& savePath) {
    pdfe::layers::LayerManager mgr(doc->context());

    // Setup: create a layer to toggle (direct mutation, not on the undo stack).
    int idx = mgr.createLayer(doc, "A4ProbeLayer");
    ASSERT_GE(idx, 0);
    mgr.setVisible(doc, idx, true);
    ASSERT_TRUE(mgr.isVisible(doc, idx));

    undo.execute(std::make_unique<pdfe::layers::SetLayerVisibilityCommand>(
        &mgr, doc, idx, false));
    ASSERT_FALSE(mgr.isVisible(doc, idx));

    doc->markRequiresFullRewrite();
    ASSERT_TRUE(pdfe::core::saveWithCommandPageRebind(*doc, undo, commandPages, savePath));

    // The OCG survives the full-rewrite save (it is referenced from the catalog
    // /OCProperties). NOTE (discovered by this probe): MuPDF's pdf_enable_layer
    // toggles a RUNTIME layer-config state that pdf_save_document does NOT
    // serialize back into /D/OFF, so the hidden state itself does not persist a
    // save+reopen — a separate OCG save-fidelity limitation, out of scope for
    // this A4 dangling-object probe. What A4 requires is proven below: the
    // command, still bound to the live reopened document, drives undo/redo
    // without a dangling Page/pdf_obj (no UAF).
    ASSERT_TRUE(mgr.hasLayers(doc))
        << "the created OCG layer must survive the full-rewrite save";

    ASSERT_TRUE(undo.canUndo());
    undo.undo();
    const bool visAfterUndo = mgr.isVisible(doc, idx);  // command's oldVisible_ (true)

    ASSERT_TRUE(undo.canRedo());
    undo.redo();
    const bool visAfterRedo = mgr.isVisible(doc, idx);  // command's newVisible_ (false)

    // A real state oracle on the LIVE reopened document: undo and redo drive
    // visibility to opposite, defined values — proving the command operates on
    // the live doc (no dangling Page/pdf_obj) rather than crashing.
    EXPECT_TRUE(visAfterUndo)
        << "undo after save must set visibility to the pre-edit (visible) state";
    EXPECT_FALSE(visAfterRedo)
        << "redo after save must re-apply the hidden state";
    EXPECT_NE(visAfterUndo, visAfterRedo)
        << "undo/redo must toggle live visibility on the reopened document";
}

// ── Bookmark ────────────────────────────────────────────────────────────────
void runBookmark(Document* doc, UndoManager& undo,
                 std::vector<std::unique_ptr<Page>>& commandPages,
                 const std::filesystem::path& savePath) {
    pdfe::core::BookmarkManager mgr(doc->context());
    mgr.load(doc);

    const int before = mgr.totalBookmarkCount();

    pdfe::core::BookmarkDest dest;
    dest.type = pdfe::core::BookmarkDestType::Page;
    dest.pageIndex = 0;

    undo.execute(std::make_unique<pdfe::core::CreateBookmarkCommand>(
        &mgr, pdfe::core::BookmarkPath{}, "A4ProbeBookmark", dest));
    ASSERT_EQ(mgr.totalBookmarkCount(), before + 1);

    doc->markRequiresFullRewrite();
    ASSERT_TRUE(pdfe::core::saveWithCommandPageRebind(*doc, undo, commandPages, savePath));

    // The bookmark lives in the manager's in-memory tree; the save must not
    // perturb it (and, crucially, the command holds no dangling pdf_obj).
    EXPECT_EQ(mgr.totalBookmarkCount(), before + 1)
        << "bookmark tree must be intact after the save";

    ASSERT_TRUE(undo.canUndo());
    undo.undo();
    EXPECT_EQ(mgr.totalBookmarkCount(), before)
        << "undo after save must remove the created bookmark";

    ASSERT_TRUE(undo.canRedo());
    undo.redo();
    EXPECT_EQ(mgr.totalBookmarkCount(), before + 1)
        << "redo after save must re-create the bookmark";
}

// ── OCR ─────────────────────────────────────────────────────────────────────
void runOcr(Document* doc, UndoManager& undo,
            std::vector<std::unique_ptr<Page>>& commandPages,
            const std::filesystem::path& savePath) {
    std::set<int> ocred;
    std::set<int> invalidated;

    auto cmd = std::make_unique<pdfe::tre::OcrAllPagesCommand>(
        std::vector<int>{0, 1, 2},
        [&](int p) { ocred.insert(p); },
        [&](int p) { invalidated.insert(p); },
        pdfe::tre::OcrAllPagesCommand::ProgressFn{});
    auto* cmdPtr = cmd.get();  // non-owning observer for state oracle

    undo.execute(std::move(cmd));
    ASSERT_EQ(ocred.size(), 3u);
    ASSERT_EQ(cmdPtr->processedPages().size(), 3u);

    doc->markRequiresFullRewrite();
    ASSERT_TRUE(pdfe::core::saveWithCommandPageRebind(*doc, undo, commandPages, savePath));

    ASSERT_TRUE(undo.canUndo());
    undo.undo();
    EXPECT_EQ(invalidated.size(), 3u)
        << "undo after save must invalidate every processed OCR page";

    invalidated.clear();
    ocred.clear();
    ASSERT_TRUE(undo.canRedo());
    undo.redo();
    EXPECT_EQ(ocred.size(), 3u)
        << "redo after save must re-run OCR on every page";
    EXPECT_EQ(cmdPtr->processedPages().size(), 3u);
}

// ── Signature ─────────────────────────────────────────────────────────────
// Unlike the four families above (which snapshot VALUE state and hold no
// document object), CreateSignatureFieldCommand is a PAGE-HOLDING command: it
// retains a raw pdf::core::Page* and exposes rebindPage(). It is the A4
// dangling-Page case in its purest form — the command's Page* is exactly what a
// full-rewrite save invalidates, so saveWithCommandPageRebind MUST drain and
// re-resolve it or undo() would deref a freed Page (UAF). Its throw-safety is
// also covered by test_save_throw_undo_safety.cpp; this probe adds the
// {op → save → undo → redo} state oracle so all five A4 families live in one
// parametrised probe. Observable: SignatureManager::countSignatures enumerates
// signature WIDGETS (pdf_first_widget + PDF_WIDGET_TYPE_SIGNATURE), so the field
// count moves +1 / restore / +1 across execute/undo/redo — the same observable
// the dedicated CreateSignatureFieldCommand tests use. (getAnnotations uses
// pdf_first_annot, which does NOT surface signature widgets, so it is not usable
// here.) countSignatures is page-independent, so it is robust across the rebind.
void runSignature(Document* doc, UndoManager& undo,
                  std::vector<std::unique_ptr<Page>>& commandPages,
                  const std::filesystem::path& savePath) {
    pdfe::signatures::SignatureManager sigMgr(doc->context());

    Page* page0 = commandPages.back().get();
    ASSERT_NE(page0, nullptr);
    const int before = sigMgr.countSignatures(doc);

    undo.execute(std::make_unique<pdfe::signatures::CreateSignatureFieldCommand>(
        &sigMgr, page0, "A4ProbeSig", QRectF(60, 60, 160, 40)));
    ASSERT_EQ(sigMgr.countSignatures(doc), before + 1)
        << "creating a signature field must add one signature widget";

    doc->markRequiresFullRewrite();
    ASSERT_TRUE(pdfe::core::saveWithCommandPageRebind(*doc, undo, commandPages, savePath));

    // The command's Page* was invalidated by the full-rewrite save and rebound;
    // countSignatures queries the LIVE reopened document.
    EXPECT_EQ(sigMgr.countSignatures(doc), before + 1)
        << "the created signature field must persist the full-rewrite save";

    ASSERT_TRUE(undo.canUndo());
    undo.undo();  // drives the command's rebound Page* — a UAF here would crash
    EXPECT_EQ(sigMgr.countSignatures(doc), before)
        << "undo after save must remove the signature field (no dangling Page/UAF)";

    ASSERT_TRUE(undo.canRedo());
    undo.redo();
    EXPECT_EQ(sigMgr.countSignatures(doc), before + 1)
        << "redo after save must re-create the signature field";
}

TEST_P(CommandFamilyUndoAfterSaveProbe, HoldsNoRawObjAcrossFullRewriteSave) {
    const Family fam = GetParam();

    auto doc = Document::open(context_, tempPdf_);
    ASSERT_NE(doc, nullptr);

    UndoManager undo;

    // A live commandPages holder mirrors the production save invariant: the
    // drain→write→rebind sequence clears this vector, writes, then re-resolves.
    std::vector<std::unique_ptr<Page>> commandPages;
    commandPages.push_back(doc->page(0));
    ASSERT_NE(commandPages.back().get(), nullptr);

    switch (fam) {
        case Family::Metadata: runMetadata(doc.get(), undo, commandPages, tempPdf_); break;
        case Family::Layer:    runLayer(doc.get(), undo, commandPages, tempPdf_);    break;
        case Family::Bookmark: runBookmark(doc.get(), undo, commandPages, tempPdf_); break;
        case Family::Ocr:      runOcr(doc.get(), undo, commandPages, tempPdf_);      break;
        case Family::Signature: runSignature(doc.get(), undo, commandPages, tempPdf_); break;
    }

    // The document itself survived intact and is still queryable (no UAF).
    EXPECT_GT(doc->pageCount(), 0);
    ASSERT_NO_THROW({ auto p = doc->page(0); EXPECT_NE(p, nullptr); });

    // Tidy teardown: release command holders before the pages/doc.
    undo.clear();
    commandPages.clear();
    doc.reset();
}

INSTANTIATE_TEST_SUITE_P(
    UnprovenSafeFamilies, CommandFamilyUndoAfterSaveProbe,
    ::testing::Values(Family::Metadata, Family::Layer, Family::Bookmark, Family::Ocr,
                      Family::Signature),
    [](const ::testing::TestParamInfo<Family>& info) {
        return std::string(familyName(info.param));
    });

}  // namespace
