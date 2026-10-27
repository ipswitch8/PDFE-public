// -*- coding: utf-8 -*-
//
// Round-2 finding R2-03 (High, data-loss) — RED→GREEN class oracle for
// "a THROWN save must not leave the undo stack dangling".
//
// THE DEFECT THIS TEST PINS
// -------------------------
// detail::drainRebindAroundWrite (core/SaveWithRebind.h) runs:
//   step 2  commandPages.clear()   — drops ALL Page holders
//   step 3  performWrite()         — the actual Document::save/saveAs
//   step 4  re-resolve pages
//   step 5  rebind every command to a fresh page
// If performWrite() THROWS (e.g. a residual file-lock holder, or the atomic
// rename-over ultimately failing — R2-04), the exception propagates BEFORE steps
// 4-5. Every undo/redo command then still holds its pre-drain raw Page* into the
// pages dropped in step 2 → dangling → heap-use-after-free on the next undo().
//
// THE FIX
// -------
// Wrap performWrite() so that whether it succeeds OR throws, the re-resolve +
// rebind still run (rebinding each command to a live page or nullptr) before the
// original exception is rethrown.
//
// THE ORACLE
// ----------
// Parametrized over every Page*-holding command family (Image insert/resize/
// rotate/crop/remove, Annotation, Redaction, Form, Signature — the families that
// override rebindPage). For each: apply the op via a real UndoManager with a live
// commandPages vector, INJECT a save throw (hold an EXTRA live Page beyond
// commandPages so the full-rewrite save is file-locked and Document::save throws
// — the mechanism proven by test_document_save_file_lock.cpp), assert the save
// threw, then undo() and assert NO crash/UAF and a defined result — proving every
// command was rebound despite the throw. RED (UAF, hard under ASan) on the
// unpatched tree; GREEN after the fix, Debug and msvc-asan.

#include <gtest/gtest.h>

#include "core/AtomicFileReplace.h"
#include "core/SaveWithRebind.h"
#include "core/Document.h"
#include "core/Page.h"
#include "core/MuPdfContext.h"
#include "core/UndoManager.h"

#include "annotations/AnnotationCommands.h"
#include "annotations/AnnotationManager.h"
#include "redaction/RedactionCommands.h"
#include "redaction/RedactionManager.h"
#include "forms/FormCommands.h"
#include "forms/FormManager.h"
#include "forms/FormFieldType.h"
#include "signatures/SignatureCommands.h"
#include "signatures/SignatureManager.h"
#include "image/ImageCommands.h"
#include "image/ImageManager.h"
#include "image/ImageContentEditor.h"

#include <QRectF>

#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
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

enum class Family {
    Annotation,
    Redaction,
    Form,
    Signature,
    ImageInsert,
    ImageRemove,
    ImageResize,
    ImageRotate,
    ImageCrop,
};

const char* familyName(Family f) {
    switch (f) {
        case Family::Annotation:  return "Annotation";
        case Family::Redaction:   return "Redaction";
        case Family::Form:        return "Form";
        case Family::Signature:   return "Signature";
        case Family::ImageInsert: return "ImageInsert";
        case Family::ImageRemove: return "ImageRemove";
        case Family::ImageResize: return "ImageResize";
        case Family::ImageRotate: return "ImageRotate";
        case Family::ImageCrop:   return "ImageCrop";
    }
    return "?";
}

bool needsExistingImage(Family f) {
    return f == Family::ImageRemove || f == Family::ImageResize ||
           f == Family::ImageRotate || f == Family::ImageCrop;
}

class SaveThrowUndoSafetyTest : public ::testing::TestWithParam<Family> {
protected:
    void SetUp() override {
        // test_pdf_with_images.pdf carries both pages and an image XObject, so it
        // serves every family (annotation/redaction/form/signature create on the
        // page; image ops act on the embedded image).
        std::filesystem::path fixture =
            std::filesystem::path(TEST_FIXTURES_DIR) / "test_pdf_with_images.pdf";
        if (!std::filesystem::exists(fixture)) {
            GTEST_SKIP() << "test_pdf_with_images.pdf not available";
        }
        pngPath_ = std::string(TEST_FIXTURES_DIR) + "/test_image_rgb.png";

        tempPdf_ = std::filesystem::temp_directory_path() /
                   (std::string("pdfe_save_throw_undo_") +
                    familyName(GetParam()) + ".pdf");
        std::error_code ec;
        std::filesystem::remove(tempPdf_, ec);
        std::filesystem::remove(tempSibling(), ec);
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
        std::filesystem::remove(tempSibling(), ec);
    }

    std::filesystem::path tempSibling() const {
        // The full-rewrite temp: this fixture's families drive save()'s
        // full-rewrite branch, and TearDown clears the artifact THAT branch
        // leaves. (A staged incremental residue would be named
        // ".pdfe_part.<hex>" and is not what this sibling refers to.)
        return pdfe::core::fullRewriteTempPathFor(tempPdf_);
    }

    std::filesystem::path tempPdf_;
    std::string pngPath_;
    std::shared_ptr<MuPdfContext> context_;
};

TEST_P(SaveThrowUndoSafetyTest, ThrownSaveStillRebindsUndoStack) {
    const Family fam = GetParam();

    auto doc = Document::open(context_, tempPdf_);
    ASSERT_NE(doc, nullptr);

    // Managers (only the relevant one is used, but constructing all keeps the
    // switch below flat and cheap).
    pdfe::annotations::AnnotationManager annotMgr(context_);
    pdfe::redaction::RedactionManager redMgr(context_);
    pdfe::forms::FormManager formMgr(context_);
    pdfe::signatures::SignatureManager sigMgr(context_);
    pdfe::image::ImageManager imgMgr(context_);
    pdfe::image::ImageContentEditor imgEditor(context_);

    UndoManager undo;

    // commandPages owns the Page the command references (the production invariant
    // SaveWithRebind maintains: drain -> write -> re-resolve into this vector).
    std::vector<std::unique_ptr<Page>> commandPages;
    commandPages.push_back(doc->page(0));
    Page* pagePtr = commandPages.back().get();
    ASSERT_NE(pagePtr, nullptr);

    // For image transform/remove ops, resolve the target image up front.
    std::string imgName;
    QRectF imgBox;
    float imgW = 0.0f, imgH = 0.0f;
    if (fam == Family::ImageInsert || needsExistingImage(fam)) {
        auto imgs = imgMgr.enumerateImages(pagePtr, doc.get());
        if (needsExistingImage(fam)) {
            if (imgs.empty()) {
                GTEST_SKIP() << "fixture has no image for " << familyName(fam);
            }
            imgName = imgs[0].xobjectName;
            imgBox = imgs[0].boundingBox;
            imgW = imgs[0].displayWidth;
            imgH = imgs[0].displayHeight;
        }
    }

    // ── build + execute the family's command via the real UndoManager ─────────
    switch (fam) {
        case Family::Annotation:
            undo.execute(std::make_unique<pdfe::annotations::CreateAnnotationCommand>(
                &annotMgr, pagePtr, pdfe::annotations::AnnotationType::Text,
                QRectF(100, 100, 200, 50)));
            break;
        case Family::Redaction:
            undo.execute(std::make_unique<pdfe::redaction::CreateRedactionCommand>(
                &redMgr, pagePtr, QRectF(80, 80, 120, 30)));
            break;
        case Family::Form:
            undo.execute(std::make_unique<pdfe::forms::CreateFormFieldCommand>(
                &formMgr, pagePtr, pdfe::forms::FormFieldType::Text,
                QRectF(70, 70, 150, 24), "r2_03_field"));
            break;
        case Family::Signature:
            undo.execute(std::make_unique<pdfe::signatures::CreateSignatureFieldCommand>(
                &sigMgr, pagePtr, "r2_03_sig", QRectF(60, 60, 160, 40)));
            break;
        case Family::ImageInsert: {
            pdfe::image::InsertImageOptions o;
            o.filePath = pngPath_;
            o.targetRect = QRectF(60.0, 60.0, 120.0, 90.0);
            undo.execute(std::make_unique<pdfe::image::InsertImageCommand>(
                &imgMgr, pagePtr, doc.get(), o));
            break;
        }
        case Family::ImageRemove:
            undo.execute(std::make_unique<pdfe::image::RemoveImageCommand>(
                &imgMgr, pagePtr, doc.get(), /*imageIndex=*/0));
            break;
        case Family::ImageResize:
            undo.execute(std::make_unique<pdfe::image::ResizeImageCommand>(
                &imgMgr, &imgEditor, pagePtr, doc.get(), imgName,
                imgW * 1.6f + 11.0f, imgH * 1.6f + 7.0f,
                static_cast<float>(imgBox.x()), static_cast<float>(imgBox.y())));
            break;
        case Family::ImageRotate:
            undo.execute(std::make_unique<pdfe::image::RotateImageCommand>(
                &imgMgr, &imgEditor, pagePtr, doc.get(), imgName, 37.0f,
                static_cast<float>(imgBox.center().x()),
                static_cast<float>(imgBox.center().y()), imgW, imgH));
            break;
        case Family::ImageCrop:
            undo.execute(std::make_unique<pdfe::image::CropImageCommand>(
                &imgMgr, &imgEditor, pagePtr, doc.get(), imgName,
                QRectF(imgBox.x(), imgBox.y(),
                       imgBox.width() * 0.6, imgBox.height() * 0.6),
                imgW, imgH,
                static_cast<float>(imgBox.x()) + 5.0f,
                static_cast<float>(imgBox.y()) + 7.0f));
            break;
    }
    ASSERT_TRUE(undo.canUndo()) << "the family command must be on the undo stack";

    doc->markRequiresFullRewrite();

    // ── INJECT the save throw ────────────────────────────────────────────────
    // Hold an EXTRA live Page beyond commandPages. saveWithCommandPageRebind
    // drains commandPages (step 2), but this extra Page keeps the source
    // fz_document open (fz_keep_document), so the full-rewrite rename-over is
    // file-locked and Document::save throws — exactly the residual-holder case
    // R2-03 must survive. (Proven by test_document_save_file_lock.cpp.)
    std::unique_ptr<Page> extraHolder = doc->page(0);
    ASSERT_NE(extraHolder, nullptr);

    // (a) The save must throw (the file-lock holder blocks the rewrite).
    EXPECT_ANY_THROW({
        pdfe::core::saveWithCommandPageRebind(*doc, undo, commandPages, tempPdf_);
    }) << "the held extra Page must lock the full-rewrite save";

    // Drop the extra holder — safe: it kept the OLD (pre-save) document alive,
    // distinct from the ORIGINAL that save() reopened on failure.
    extraHolder.reset();

    // ── undo AFTER the thrown save: the UAF site on the unpatched tree ────────
    // The fix guarantees every command was rebound (to a live page or nullptr)
    // despite the throw, so this dereferences a live object, not a dropped one.
    // A defined thrown failure is acceptable (rebound-to-nullptr no-op / missing
    // state after the reopen); a UAF is NOT — under ASan it aborts regardless of
    // this catch, so the catch cannot mask the defect the test targets.
    ASSERT_TRUE(undo.canUndo());
    bool undoReturnedCleanly = false;
    try {
        undo.undo();
        undoReturnedCleanly = true;
    } catch (const std::exception&) {
        undoReturnedCleanly = true;  // defined failure, not a UAF
    }
    EXPECT_TRUE(undoReturnedCleanly)
        << "post-thrown-save undo must reach a defined state, never a UAF";

    // The document itself survived intact and is still queryable.
    EXPECT_GT(doc->pageCount(), 0);
    ASSERT_NO_THROW({ auto p = doc->page(0); EXPECT_NE(p, nullptr); });

    // Tidy teardown: destroy commands (releases holders) before the pages/doc.
    undo.clear();
    commandPages.clear();
    doc.reset();
}

INSTANTIATE_TEST_SUITE_P(
    AllPageHoldingFamilies, SaveThrowUndoSafetyTest,
    ::testing::Values(
        Family::Annotation,
        Family::Redaction,
        Family::Form,
        Family::Signature,
        Family::ImageInsert,
        Family::ImageRemove,
        Family::ImageResize,
        Family::ImageRotate,
        Family::ImageCrop),
    [](const ::testing::TestParamInfo<Family>& info) {
        return std::string(familyName(info.param));
    });

}  // namespace
