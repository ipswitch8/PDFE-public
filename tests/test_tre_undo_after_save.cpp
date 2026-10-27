// -*- coding: utf-8 -*-
//
// Round-2 finding A4 (same class as R2-01/R2-02) — RED→GREEN class oracle for
// the TRE "undo after a full-rewrite save" heap-use-after-free.
//
// THE DEFECT THIS TEST PINS
// -------------------------
// TRE undo (ReconstructRegionCommand / ImageReplaceCommand) captures, at
// execute() time, the page's image XObject and /Contents object via
// PdfContentWriter::saveImageXObjectRef / saveContentsRef. These were RAW
// pdf_keep_obj references into the LIVE pdf_document. On undo() they were put
// straight back with pdf_dict_put(s).
//
// A full-rewrite save invalidates those raw pointers:
//   * Document::save() (in-place File>Save) DROPS pImpl->doc and REOPENS a
//     fresh pdf_document — the kept objs now dangle into freed memory.
//   * Document::saveAs() GC-renumbers the in-memory xref.
// A post-save undo then dereferences the dangling objs → heap-use-after-free
// (hard crash under ASan; cross-document assert / corruption in Debug).
//
// THE FIX (mirrors R2-01 image family + Document::RetainedPageHolder)
// ------------------------------------------------------------------
// save*Ref deep-copy the object into their OWN standalone pdf_document
// (pdf_graft_object via pdfe::core::makeRetainedObjHolder), so the retained
// object is document-independent and survives any save. restore*Ref grafts a
// COPY back into the live document at undo time (non-consuming); release*Ref
// frees the holder.
//
// THE ORACLES
// -----------
// 1. PrimitiveSurvivesFullRewriteSave — deterministic (no OCR): drives the
//    exact PdfContentWriter save/mutate/full-rewrite-save/restore surface that
//    ImageReplaceCommand::undo walks. RED (UAF/corruption) on the unpatched
//    tree; GREEN after the fix.
// 2. ImageReplaceCommandSurvivesFullRewriteSave — end-to-end command lifecycle
//    (execute → push on UndoManager → in-place full-rewrite save via
//    saveWithCommandPageRebind → undo → dtor). Requires the scanned fixture +
//    tessdata; skips otherwise. Exercises the command's holder ownership
//    (release-before-recapture, non-consuming restore, dtor releases once).

#include <gtest/gtest.h>

#include "tre/TreCommands.h"
#include "tre/PdfContentWriter.h"
#include "tre/RegionReconstructor.h"
#include "tre/ImageReplacer.h"
#include "tre/OcrPipeline.h"
#include "tre/CoordMapper.h"
#include "tre/BgColorDetector.h"
#include "tre/FontMatcher.h"
#include "tre/QualityAssessor.h"
#include "tre/ImageMasker.h"
#include "tre/BitmapCompositor.h"
#include "fonts/EmbeddedFontAnalyzer.h"
#include "fonts/SubstituteFontSelector.h"
#include "fonts/FontAnalysisCache.h"
#include "fonts/PdfFontEmbedder.h"
#include "core/Document.h"
#include "core/Page.h"
#include "core/MuPdfContext.h"
#include "core/UndoManager.h"
#include "core/SaveWithRebind.h"

#include <QImage>

#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#ifndef TEST_FIXTURES_DIR
#define TEST_FIXTURES_DIR "."
#endif

using pdfe::core::Document;
using pdfe::core::MuPdfContext;
using pdfe::core::Page;
using pdfe::commands::UndoManager;

namespace {

std::string fixturePath(const std::string& name) {
    return std::string(TEST_FIXTURES_DIR) + "/" + name;
}

// ---------------------------------------------------------------------------
// Oracle 1 — deterministic PdfContentWriter primitive surface (no OCR).
// ---------------------------------------------------------------------------
class TreUndoAfterSavePrimitiveTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::filesystem::path fixture = fixturePath("test_fullpage_image.pdf");
        if (!std::filesystem::exists(fixture)) {
            GTEST_SKIP() << "test_fullpage_image.pdf not available";
        }
        tempPdf_ = std::filesystem::temp_directory_path() /
                   "pdfe_tre_undo_after_save_primitive.pdf";
        std::error_code ec;
        std::filesystem::remove(tempPdf_, ec);
        std::filesystem::copy_file(
            fixture, tempPdf_,
            std::filesystem::copy_options::overwrite_existing, ec);
        ASSERT_FALSE(ec) << "failed to stage temp copy: " << ec.message();

        context_ = std::make_shared<MuPdfContext>();
        embedder_ = std::make_unique<pdfe::fonts::PdfFontEmbedder>(context_.get());
        writer_ = std::make_unique<pdfe::tre::PdfContentWriter>(
            context_.get(), embedder_.get());
    }

    void TearDown() override {
        writer_.reset();
        embedder_.reset();
        context_.reset();
        std::error_code ec;
        std::filesystem::remove(tempPdf_, ec);
    }

    std::filesystem::path tempPdf_;
    std::shared_ptr<MuPdfContext> context_;
    std::unique_ptr<pdfe::fonts::PdfFontEmbedder> embedder_;
    std::unique_ptr<pdfe::tre::PdfContentWriter> writer_;
};

TEST_F(TreUndoAfterSavePrimitiveTest, PrimitiveSurvivesFullRewriteSave) {
    auto doc = Document::open(context_, tempPdf_);
    ASSERT_NE(doc, nullptr);

    // Baseline image identity.
    std::string xobjectName;
    int origWidth = 0, origHeight = 0;
    {
        auto page = doc->page(0);
        auto info = writer_->findPageImageXObject(page.get(), doc.get());
        ASSERT_TRUE(info.has_value())
            << "fixture must carry an image XObject";
        xobjectName = info->xobjectName;
        origWidth = info->width;
        origHeight = info->height;
    }

    // Capture undo refs and mutate the page image — the ImageReplaceCommand
    // execute() surface. Page holder is transient (released at scope end) so
    // the in-place full-rewrite below is not locked by a live Page.
    void* savedImageRef = nullptr;
    void* savedContentsRef = nullptr;
    {
        auto page = doc->page(0);
        savedImageRef = writer_->saveImageXObjectRef(page.get(), doc.get());
        savedContentsRef = writer_->saveContentsRef(page.get(), doc.get());
        ASSERT_NE(savedImageRef, nullptr);
        ASSERT_NE(savedContentsRef, nullptr);

        // 40x30 is < half the fixture image in both axes → replacePageImage
        // does NOT auto-rescale, guaranteeing a real dimension change.
        QImage replacement(40, 30, QImage::Format_RGB888);
        replacement.fill(qRgb(0, 0, 0));
        ASSERT_TRUE(writer_->replacePageImage(page.get(), doc.get(), replacement));

        auto mutated = writer_->findPageImageXObject(page.get(), doc.get());
        ASSERT_TRUE(mutated.has_value());
        ASSERT_NE(mutated->width, origWidth)
            << "sanity: replacePageImage must actually change the dimensions";
    }

    // In-place FULL-REWRITE save through the production wrapper: drops+reopens
    // the pdf_document, dangling any raw retained obj on the unpatched tree.
    std::vector<std::unique_ptr<Page>> commandPages;
    commandPages.push_back(doc->page(0));
    UndoManager undo;
    doc->markRequiresFullRewrite();
    ASSERT_NO_THROW(
        pdfe::core::saveWithCommandPageRebind(*doc, undo, commandPages, tempPdf_));
    commandPages.clear();

    // Undo = restore both refs (ImageReplaceCommand::undo surface). On the
    // unpatched tree savedImageRef/savedContentsRef dangle → UAF here.
    {
        auto page = doc->page(0);
        ASSERT_NO_THROW(writer_->restoreContentsRef(
            page.get(), doc.get(), savedContentsRef))
            << "restoreContentsRef must not crash after a full-rewrite save";
        ASSERT_NO_THROW(writer_->restoreImageXObjectRef(
            page.get(), doc.get(), savedImageRef, xobjectName))
            << "restoreImageXObjectRef must not crash after a full-rewrite save";

        auto restored = writer_->findPageImageXObject(page.get(), doc.get());
        ASSERT_TRUE(restored.has_value());
        EXPECT_EQ(restored->xobjectName, xobjectName);
        EXPECT_EQ(restored->width, origWidth)
            << "undo after full-rewrite save must restore the ORIGINAL image "
               "dimensions (the retained holder must survive the save)";
        EXPECT_EQ(restored->height, origHeight);
    }

    writer_->releaseImageXObjectRef(savedImageRef);
    writer_->releaseContentsRef(savedContentsRef);
    doc.reset();
}

// ---------------------------------------------------------------------------
// Oracle 2 — end-to-end ImageReplaceCommand lifecycle across a full-rewrite
// save. Needs the scanned fixture + tessdata; skips otherwise.
// ---------------------------------------------------------------------------
class TreUndoAfterSaveCommandTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::string scanned = fixturePath("test_scanned.pdf");
        if (!std::filesystem::exists(scanned)) {
            GTEST_SKIP() << "test_scanned.pdf not available";
        }
        tessdata_ = fixturePath("tessdata");
        if (!std::filesystem::exists(tessdata_ + "/eng.traineddata")) {
            GTEST_SKIP() << "tessdata/eng.traineddata not available";
        }
        tempPdf_ = std::filesystem::temp_directory_path() /
                   "pdfe_tre_undo_after_save_command.pdf";
        std::error_code ec;
        std::filesystem::remove(tempPdf_, ec);
        std::filesystem::copy_file(
            scanned, tempPdf_,
            std::filesystem::copy_options::overwrite_existing, ec);
        ASSERT_FALSE(ec) << "failed to stage temp copy: " << ec.message();

        context_ = std::make_shared<MuPdfContext>();
        document_ = Document::open(context_, tempPdf_);
        ASSERT_NE(document_, nullptr);

        analyzer_ = std::make_unique<pdfe::fonts::EmbeddedFontAnalyzer>(context_.get());
        fontCache_ = std::make_unique<pdfe::fonts::FontAnalysisCache>();
        selector_ = std::make_unique<pdfe::fonts::SubstituteFontSelector>(
            analyzer_.get(), fontCache_.get());
        embedder_ = std::make_unique<pdfe::fonts::PdfFontEmbedder>(context_.get());

        pdfe::tre::OcrConfig ocrConfig;
        ocrConfig.tessdataPath = tessdata_;
        ocrPipeline_ = std::make_unique<pdfe::tre::OcrPipeline>(
            context_.get(), document_.get(), ocrConfig);
        coordMapper_ = std::make_unique<pdfe::tre::CoordMapper>(
            context_.get(), document_.get());
        bgDetector_ = std::make_unique<pdfe::tre::BgColorDetector>();
        fontMatcher_ = std::make_unique<pdfe::tre::FontMatcher>(
            selector_.get(), analyzer_.get());
        imageMasker_ = std::make_unique<pdfe::tre::ImageMasker>();
        bitmapCompositor_ = std::make_unique<pdfe::tre::BitmapCompositor>();
        contentWriter_ = std::make_unique<pdfe::tre::PdfContentWriter>(
            context_.get(), embedder_.get());
        imageReplacer_ = std::make_unique<pdfe::tre::ImageReplacer>(
            ocrPipeline_.get(), coordMapper_.get(), bgDetector_.get(),
            fontMatcher_.get(), imageMasker_.get(), bitmapCompositor_.get(),
            contentWriter_.get());
    }

    void TearDown() override {
        imageReplacer_.reset();
        contentWriter_.reset();
        bitmapCompositor_.reset();
        imageMasker_.reset();
        fontMatcher_.reset();
        bgDetector_.reset();
        coordMapper_.reset();
        ocrPipeline_.reset();
        embedder_.reset();
        selector_.reset();
        fontCache_.reset();
        analyzer_.reset();
        document_.reset();
        context_.reset();
        std::error_code ec;
        std::filesystem::remove(tempPdf_, ec);
    }

    std::string tessdata_;
    std::filesystem::path tempPdf_;
    std::shared_ptr<MuPdfContext> context_;
    std::unique_ptr<Document> document_;
    std::unique_ptr<pdfe::fonts::EmbeddedFontAnalyzer> analyzer_;
    std::unique_ptr<pdfe::fonts::FontAnalysisCache> fontCache_;
    std::unique_ptr<pdfe::fonts::SubstituteFontSelector> selector_;
    std::unique_ptr<pdfe::fonts::PdfFontEmbedder> embedder_;
    std::unique_ptr<pdfe::tre::OcrPipeline> ocrPipeline_;
    std::unique_ptr<pdfe::tre::CoordMapper> coordMapper_;
    std::unique_ptr<pdfe::tre::BgColorDetector> bgDetector_;
    std::unique_ptr<pdfe::tre::FontMatcher> fontMatcher_;
    std::unique_ptr<pdfe::tre::ImageMasker> imageMasker_;
    std::unique_ptr<pdfe::tre::BitmapCompositor> bitmapCompositor_;
    std::unique_ptr<pdfe::tre::PdfContentWriter> contentWriter_;
    std::unique_ptr<pdfe::tre::ImageReplacer> imageReplacer_;
};

TEST_F(TreUndoAfterSaveCommandTest, ImageReplaceCommandSurvivesFullRewriteSave) {
    // Original image data for restoration oracle.
    std::vector<uint8_t> originalData;
    {
        auto page = document_->page(0);
        originalData = contentWriter_->getImageXObjectData(page.get(), document_.get());
        if (originalData.empty()) {
            GTEST_SKIP() << "no image XObject data on page 0";
        }
    }

    UndoManager undo;
    pdfe::tre::PdfRect selection{50, 50, 500, 700};
    undo.execute(std::make_unique<pdfe::tre::ImageReplaceCommand>(
        document_.get(), context_, imageReplacer_.get(), contentWriter_.get(),
        0, selection, "Hello World", pdfe::tre::RegionId{1}));

    // In-place full-rewrite save (drops+reopens the doc). The command holds its
    // undo refs across this; a raw kept ref would dangle on the unpatched tree.
    std::vector<std::unique_ptr<Page>> commandPages;
    commandPages.push_back(document_->page(0));
    document_->markRequiresFullRewrite();
    ASSERT_NO_THROW(pdfe::core::saveWithCommandPageRebind(
        *document_, undo, commandPages, tempPdf_));
    commandPages.clear();

    // Undo after the save — the UAF site on the unpatched tree.
    ASSERT_TRUE(undo.canUndo());
    ASSERT_NO_THROW(undo.undo())
        << "post-save TRE undo must not crash/throw";

    // Observable restoration: page 0 still carries a decodable image.
    {
        auto page = document_->page(0);
        auto restoredData = contentWriter_->getImageXObjectData(page.get(), document_.get());
        EXPECT_FALSE(restoredData.empty())
            << "after undo, page 0 must still have an image XObject";
    }

    // dtor releases the holders exactly once (no double-free / leak) — exercised
    // implicitly as `undo` is destroyed at scope exit under ASan.
    undo.clear();
}

}  // namespace
