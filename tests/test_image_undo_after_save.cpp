// -*- coding: utf-8 -*-
//
// Round-2 finding R2-01 (Critical) — RED→GREEN class oracle for the image-
// command "undo after a full-rewrite save" heap-use-after-free.
//
// THE DEFECT THIS TEST PINS
// -------------------------
// Every image command (Insert / Remove / Resize / Rotate / Crop) captures, at
// execute() time, a raw pdf_obj* into the LIVE pdf_document via
// ImageManager::saveContentsRef / saveImageRef (pdf_keep_obj). It stashes that
// raw pointer in void* savedContentsRef_ / savedImageRef_ and, on undo(), grafts
// it back into the page via restoreImage.
//
// A full-rewrite save invalidates that raw pdf_obj*:
//   * Document::save() (in-place File>Save) DROPS pImpl->doc and REOPENS a fresh
//     pdf_document — the kept obj now points into freed memory.
//   * Document::saveAs() GC-renumbers the in-memory xref; the saved obj (which
//     the edit already made unreferenced in the page tree) is reclaimed.
// A post-save undo() then dereferences that dangling obj → heap-use-after-free
// (a hard crash under ASan, a cross-document assert / corruption in Debug).
//
// The command families DO override pageIndex()/rebindPage(), and the save
// orchestration (saveWithCommandPageRebind / saveAsWithCommandPageRebind) drains
// and re-resolves the held Page*. But that only re-points page_ — it does NOT
// touch the retained content/image object refs. Those are the dangling pointers.
//
// THE FIX (mirrors Document::retainPageObj's standalone-holder pattern)
// ---------------------------------------------------------------------
// saveContentsRef/saveImageRef DEEP-COPY the object into their OWN standalone
// pdf_document (pdf_graft_object), so the retained object is document-
// independent and survives any save. restoreImage grafts a COPY back into the
// live document at undo time. releaseRef drops the whole holder.
//
// THE ORACLE
// ----------
// Parametrized over {Insert, Remove, Resize, Rotate, Crop} × {InPlaceSave,
// SaveAs}. Each case: open a real image fixture, push the command on a real
// UndoManager (execute), perform the full-rewrite save through the production
// SaveWithRebind wrapper (with a live commandPages vector), then undo() and
// redo() — asserting an OBSERVABLE state oracle at each step (image count for
// insert/remove; the enumerated CTM of the target image for the transforms),
// never EXPECT_NO_THROW-only. RED (crash / throw / wrong state) on the unpatched
// tree; GREEN after the fix, Debug and msvc-asan.

#include <gtest/gtest.h>

#include "image/ImageCommands.h"
#include "image/ImageManager.h"
#include "image/ImageContentEditor.h"
#include "image/ImageData.h"
#include "core/Document.h"
#include "core/Page.h"
#include "core/MuPdfContext.h"
#include "core/UndoManager.h"
#include "core/SaveWithRebind.h"

#include <QRectF>

#include <array>
#include <cmath>
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
using pdfe::image::CropImageCommand;
using pdfe::image::ImageContentEditor;
using pdfe::image::ImageManager;
using pdfe::image::InsertImageCommand;
using pdfe::image::InsertImageOptions;
using pdfe::image::PageImageInfo;
using pdfe::image::RemoveImageCommand;
using pdfe::image::ResizeImageCommand;
using pdfe::image::RotateImageCommand;

enum class ImgOp { Insert, Remove, Resize, Rotate, Crop };
enum class SaveMode { InPlace, SaveAs };

struct Param {
    ImgOp op;
    SaveMode mode;
};

const char* opName(ImgOp op) {
    switch (op) {
        case ImgOp::Insert: return "Insert";
        case ImgOp::Remove: return "Remove";
        case ImgOp::Resize: return "Resize";
        case ImgOp::Rotate: return "Rotate";
        case ImgOp::Crop:   return "Crop";
    }
    return "?";
}
const char* modeName(SaveMode m) {
    return m == SaveMode::InPlace ? "InPlaceSave" : "SaveAs";
}
bool isTransform(ImgOp op) {
    return op == ImgOp::Resize || op == ImgOp::Rotate || op == ImgOp::Crop;
}

// Locate an image by its XObject key in an enumeration result.
std::optional<PageImageInfo> findByName(const std::vector<PageImageInfo>& v,
                                        const std::string& name) {
    for (const auto& i : v) {
        if (i.xobjectName == name) return i;
    }
    return std::nullopt;
}

// Largest absolute per-component difference between two CTMs.
float ctmMaxDelta(const std::array<float, 6>& a, const std::array<float, 6>& b) {
    float m = 0.0f;
    for (int i = 0; i < 6; ++i) m = std::max(m, std::fabs(a[i] - b[i]));
    return m;
}

class ImageUndoAfterSaveTest : public ::testing::TestWithParam<Param> {
protected:
    void SetUp() override {
        std::filesystem::path fixture =
            std::filesystem::path(TEST_FIXTURES_DIR) / "test_pdf_with_images.pdf";
        if (!std::filesystem::exists(fixture)) {
            GTEST_SKIP() << "test_pdf_with_images.pdf not available";
        }
        pngPath_ = std::string(TEST_FIXTURES_DIR) + "/test_image_rgb.png";

        const Param p = GetParam();
        std::string tag = std::string(opName(p.op)) + "_" + modeName(p.mode);
        tempPdf_ = std::filesystem::temp_directory_path() /
                   ("pdfe_img_undo_after_save_" + tag + ".pdf");
        saveAsPath_ = std::filesystem::temp_directory_path() /
                      ("pdfe_img_undo_after_save_" + tag + "_saveas.pdf");

        std::error_code ec;
        std::filesystem::remove(tempPdf_, ec);
        std::filesystem::remove(saveAsPath_, ec);
        std::filesystem::copy_file(
            fixture, tempPdf_,
            std::filesystem::copy_options::overwrite_existing, ec);
        ASSERT_FALSE(ec) << "failed to stage temp copy: " << ec.message();

        context_ = std::make_shared<MuPdfContext>();
        manager_ = std::make_unique<ImageManager>(context_);
        editor_ = std::make_unique<ImageContentEditor>(context_);
        undo_ = std::make_unique<UndoManager>();
    }

    void TearDown() override {
        undo_.reset();
        editor_.reset();
        manager_.reset();
        context_.reset();
        std::error_code ec;
        std::filesystem::remove(tempPdf_, ec);
        std::filesystem::remove(saveAsPath_, ec);
    }

    // Enumerate the images on page 0 through a throwaway Page handle. Used only
    // for oracles; the command holds its own (rebound) Page independently.
    std::vector<PageImageInfo> enumPage0(Document& doc) {
        auto page = doc.page(0);
        auto v = manager_->enumerateImages(page.get(), &doc);
        page.reset();
        return v;
    }

    void doSave(Document& doc, std::vector<std::unique_ptr<Page>>& commandPages) {
        // Force the full-rewrite path so save() actually drops+reopens (in-place)
        // and saveAs() GC-renumbers — the two branches that dangle a raw obj.
        doc.markRequiresFullRewrite();
        if (GetParam().mode == SaveMode::InPlace) {
            pdfe::core::saveWithCommandPageRebind(doc, *undo_, commandPages, tempPdf_);
        } else {
            pdfe::core::saveAsWithCommandPageRebind(doc, *undo_, commandPages, saveAsPath_);
        }
    }

    std::filesystem::path tempPdf_;
    std::filesystem::path saveAsPath_;
    std::string pngPath_;
    std::shared_ptr<MuPdfContext> context_;
    std::unique_ptr<ImageManager> manager_;
    std::unique_ptr<ImageContentEditor> editor_;
    std::unique_ptr<UndoManager> undo_;
};

TEST_P(ImageUndoAfterSaveTest, UndoRedoCycleSurvivesSaveWithCorrectState) {
    const Param p = GetParam();

    auto doc = Document::open(context_, tempPdf_);
    ASSERT_NE(doc, nullptr);

    // Baseline: the target image, its name and CTM, and the image count.
    std::string targetName;
    std::array<float, 6> originalCtm{};
    float origW = 0.0f, origH = 0.0f;
    QRectF origBox;
    int initialCount = 0;
    {
        auto imgs = enumPage0(*doc);
        initialCount = static_cast<int>(imgs.size());
        ASSERT_GE(initialCount, 1) << "fixture must carry at least one image";
        targetName = imgs[0].xobjectName;
        originalCtm = imgs[0].ctm;
        origW = imgs[0].displayWidth;
        origH = imgs[0].displayHeight;
        origBox = imgs[0].boundingBox;
    }

    // commandPages owns the Page the command references (the production invariant
    // that SaveWithRebind maintains). After doSave() this vector is cleared and
    // repopulated with the rebound page, so pagePtr must NOT be used post-save.
    std::vector<std::unique_ptr<Page>> commandPages;
    commandPages.push_back(doc->page(0));
    Page* pagePtr = commandPages.back().get();
    ASSERT_NE(pagePtr, nullptr);

    // Distinct transform targets so the enumerated CTM changes detectably.
    const float newW = origW * 1.75f + 23.0f;
    const float newH = origH * 1.75f + 17.0f;

    // ── execute via the real UndoManager ──────────────────────────────────
    switch (p.op) {
        case ImgOp::Insert: {
            InsertImageOptions o;
            o.filePath = pngPath_;
            o.targetRect = QRectF(60.0, 60.0, 120.0, 90.0);
            undo_->execute(std::make_unique<InsertImageCommand>(
                manager_.get(), pagePtr, doc.get(), o));
            break;
        }
        case ImgOp::Remove:
            undo_->execute(std::make_unique<RemoveImageCommand>(
                manager_.get(), pagePtr, doc.get(), /*imageIndex=*/0));
            break;
        case ImgOp::Resize:
            undo_->execute(std::make_unique<ResizeImageCommand>(
                manager_.get(), editor_.get(), pagePtr, doc.get(),
                targetName, newW, newH,
                static_cast<float>(origBox.x()), static_cast<float>(origBox.y())));
            break;
        case ImgOp::Rotate:
            undo_->execute(std::make_unique<RotateImageCommand>(
                manager_.get(), editor_.get(), pagePtr, doc.get(),
                targetName, /*angleDegrees=*/37.0f,
                static_cast<float>(origBox.center().x()),
                static_cast<float>(origBox.center().y()),
                origW, origH));
            break;
        case ImgOp::Crop:
            undo_->execute(std::make_unique<CropImageCommand>(
                manager_.get(), editor_.get(), pagePtr, doc.get(),
                targetName,
                QRectF(origBox.x(), origBox.y(),
                       origBox.width() * 0.6, origBox.height() * 0.6),
                newW, newH,
                static_cast<float>(origBox.x()) + 5.0f,
                static_cast<float>(origBox.y()) + 7.0f));
            break;
    }

    // Observable state right after execute (pagePtr still valid — pre-save).
    std::array<float, 6> executedCtm{};
    int countAfterExecute = 0;
    {
        auto imgs = manager_->enumerateImages(pagePtr, doc.get());
        countAfterExecute = static_cast<int>(imgs.size());
        if (p.op == ImgOp::Insert) {
            EXPECT_EQ(countAfterExecute, initialCount + 1);
        } else if (p.op == ImgOp::Remove) {
            EXPECT_EQ(countAfterExecute, initialCount - 1);
        } else {
            EXPECT_EQ(countAfterExecute, initialCount);
            auto t = findByName(imgs, targetName);
            ASSERT_TRUE(t.has_value()) << "target image must survive the transform";
            executedCtm = t->ctm;
            EXPECT_GT(ctmMaxDelta(executedCtm, originalCtm), 1.0f)
                << "transform must visibly change the placement CTM";
        }
    }

    // ── full-rewrite save (drops the raw retained obj on the unpatched tree) ─
    ASSERT_NO_THROW(doSave(*doc, commandPages));
    pagePtr = nullptr;  // dangling after the drain step of doSave

    // ── undo AFTER the save: this is the UAF site on the unpatched tree ─────
    ASSERT_TRUE(undo_->canUndo());
    ASSERT_NO_THROW(undo_->undo())
        << "post-save undo must not crash/throw (retained obj must survive save)";

    {
        auto imgs = enumPage0(*doc);
        if (p.op == ImgOp::Insert) {
            EXPECT_EQ(static_cast<int>(imgs.size()), initialCount)
                << "undo of insert must remove the inserted image";
        } else if (p.op == ImgOp::Remove) {
            EXPECT_EQ(static_cast<int>(imgs.size()), initialCount)
                << "undo of remove must restore the removed image";
        } else {
            EXPECT_EQ(static_cast<int>(imgs.size()), initialCount);
            auto t = findByName(imgs, targetName);
            ASSERT_TRUE(t.has_value()) << "target must be back after undo";
            EXPECT_LT(ctmMaxDelta(t->ctm, originalCtm), 0.5f)
                << "undo must restore the original placement CTM";
        }
    }

    // ── redo AFTER the save: re-applies the op cleanly ──────────────────────
    ASSERT_TRUE(undo_->canRedo());
    ASSERT_NO_THROW(undo_->redo())
        << "post-save redo must not crash/throw";

    {
        auto imgs = enumPage0(*doc);
        EXPECT_EQ(static_cast<int>(imgs.size()), countAfterExecute)
            << "redo must reproduce the executed image count";
        if (isTransform(p.op)) {
            auto t = findByName(imgs, targetName);
            ASSERT_TRUE(t.has_value()) << "target must be present after redo";
            EXPECT_LT(ctmMaxDelta(t->ctm, executedCtm), 0.5f)
                << "redo must reproduce the executed placement CTM";
        }
    }

    // Tidy teardown: destroy commands (releases holders) before the pages.
    undo_->clear();
    commandPages.clear();
    doc.reset();
}

INSTANTIATE_TEST_SUITE_P(
    AllOpsBothSaveModes, ImageUndoAfterSaveTest,
    ::testing::Values(
        Param{ImgOp::Insert, SaveMode::InPlace},
        Param{ImgOp::Insert, SaveMode::SaveAs},
        Param{ImgOp::Remove, SaveMode::InPlace},
        Param{ImgOp::Remove, SaveMode::SaveAs},
        Param{ImgOp::Resize, SaveMode::InPlace},
        Param{ImgOp::Resize, SaveMode::SaveAs},
        Param{ImgOp::Rotate, SaveMode::InPlace},
        Param{ImgOp::Rotate, SaveMode::SaveAs},
        Param{ImgOp::Crop,   SaveMode::InPlace},
        Param{ImgOp::Crop,   SaveMode::SaveAs}),
    [](const ::testing::TestParamInfo<Param>& info) {
        return std::string(opName(info.param.op)) + "_" +
               modeName(info.param.mode);
    });

}  // namespace
