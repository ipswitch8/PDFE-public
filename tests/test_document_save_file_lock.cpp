// -*- coding: utf-8 -*-
//
// Pipeline f4a82c1e Phase 3 — cumulative single-session save/file-lock mechanism.
//
// WHAT THIS PINS DOWN
// -------------------
// A full-rewrite Document::save() on Windows writes a temp file, drops the
// document's own handle, then remove()s the original and rename()s the temp
// over it (src/core/Document.cpp). That remove() fails while *any other holder*
// keeps the source fz_document alive, because the OS file handle stays open —
// so save() exhausts its retry budget and throws, leaving the on-disk file
// byte-identical to the pre-edit original (a silent lost-changes hazard in a
// long-lived session).
//
// The prior 3e91a7c4 closure doc ("B-1") and a comment on the redaction-save
// path both assert that an in-flight *tile render worker* is such a holder,
// because "display lists keep the document alive via fz_keep_document". These
// tests establish the ground truth empirically:
//
//   * a held display list (exactly what an in-flight TileRenderer worker owns)
//     does NOT keep the source locked — the save succeeds; and
//   * a held Page (fz_load_page → fz_keep_document, e.g. MainWindow's toolPage_
//     / commandPages_) DOES keep it locked — the save throws; dropping the Page
//     first lets the save succeed.
//
// So the render-lifetime framing of B-1 is refuted: tile workers are save-safe.
// The real locker is a live Page. Guarding that on the ordinary File>Save path
// is a separate, non-trivial design item (commandPages_ accumulates Page refs
// that undo commands also reference, so they cannot simply be dropped before an
// ordinary save the way the irreversible redaction path drops them) and is
// tracked in tests/docs/journey-soak-phase3-findings.md.

#include <gtest/gtest.h>
#include "core/Document.h"
#include "core/Page.h"
#include "core/MuPdfContext.h"

#include <filesystem>
#include <memory>
#include <system_error>

namespace {

class DocumentSaveFileLockTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::filesystem::path fixture =
            std::filesystem::path(TEST_FIXTURES_DIR) / "test_3page.pdf";
        if (!std::filesystem::exists(fixture)) {
            GTEST_SKIP() << "test_3page.pdf not available";
        }
        tempPdf = std::filesystem::temp_directory_path() /
                  "pdfe_save_file_lock_f4a82c1e.pdf";
        stageFreshCopy(fixture);
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove(tempPdf, ec);
    }

    // Reset the working file to a pristine copy of the fixture. Each arm forces
    // the full-rewrite temp+rename path (not the incremental append, which does
    // not remove the open original and so is not exposed to the lock).
    void stageFreshCopy(const std::filesystem::path& fixture) {
        std::error_code ec;
        std::filesystem::remove(tempPdf, ec);
        std::filesystem::copy_file(
            fixture, tempPdf,
            std::filesystem::copy_options::overwrite_existing, ec);
        ASSERT_FALSE(ec) << "failed to stage temp copy: " << ec.message();
    }

    std::filesystem::path fixture() const {
        return std::filesystem::path(TEST_FIXTURES_DIR) / "test_3page.pdf";
    }

    std::filesystem::path tempPdf;
};

// Control: a full-rewrite save with no external holder succeeds.
TEST_F(DocumentSaveFileLockTest, FullRewriteSaveSucceedsWithNoHolder) {
    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    auto doc = pdfe::core::Document::open(ctx, tempPdf);
    ASSERT_NE(doc, nullptr);
    doc->markRequiresFullRewrite();

    EXPECT_NO_THROW(doc->save(tempPdf));

    // The rewrite produced a valid, reopenable file.
    auto verifyCtx = std::make_shared<pdfe::core::MuPdfContext>();
    auto reopened = pdfe::core::Document::open(verifyCtx, tempPdf);
    ASSERT_NE(reopened, nullptr);
    EXPECT_EQ(reopened->pageCount(), 3);
}

// A held display list — exactly what an in-flight TileRenderer worker owns — is
// NOT a file-lock holder. This is the empirical refutation of the "B-1"
// render-lifetime hazard: rendering a tile while a save fires is safe.
TEST_F(DocumentSaveFileLockTest, HeldDisplayListDoesNotLockSave) {
    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    auto doc = pdfe::core::Document::open(ctx, tempPdf);
    ASSERT_NE(doc, nullptr);
    doc->markRequiresFullRewrite();

    // Own a display list across the save, mirroring a render worker mid-render.
    void* displayList = doc->createDisplayList(0);
    ASSERT_NE(displayList, nullptr);

    EXPECT_NO_THROW(doc->save(tempPdf))
        << "a held display list must not lock a full-rewrite save "
           "(render workers are save-safe)";

    // "Did not throw" is not "wrote a usable document": re-open the target and
    // confirm the full rewrite really produced a readable 3-page PDF, not a
    // truncated or half-staged one (GitHub #163).
    {
        auto verifyCtx = std::make_shared<pdfe::core::MuPdfContext>();
        auto reopened = pdfe::core::Document::open(verifyCtx, tempPdf);
        ASSERT_NE(reopened, nullptr)
            << "the save that 'succeeded' under a held display list produced a "
               "file that will not re-open";
        EXPECT_EQ(reopened->pageCount(), 3);
    }

    // Drop the display list on the document's context to avoid leaking it.
    doc->context()->execute([displayList](fz_context* c) {
        fz_drop_display_list(c, static_cast<fz_display_list*>(displayList));
    });
}

// A held Page (fz_load_page → fz_keep_document) IS a file-lock holder: the
// source handle stays open, remove() fails all its retries, and save() throws.
// This is the true mechanism behind the redaction path's Page-holder drop.
TEST_F(DocumentSaveFileLockTest, HeldPageLocksFullRewriteSave) {
    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    auto doc = pdfe::core::Document::open(ctx, tempPdf);
    ASSERT_NE(doc, nullptr);
    doc->markRequiresFullRewrite();

    // Hold a Page across the save. The Page keeps the *old* fz_document alive
    // via fz_keep_document, so it is safe to drop after the throw (it owns a
    // still-live document, distinct from the one save() reopened).
    std::unique_ptr<pdfe::core::Page> page = doc->page(0);
    ASSERT_NE(page, nullptr);

    EXPECT_ANY_THROW(doc->save(tempPdf))
        << "a live Page must lock a full-rewrite save (fz_keep_document keeps "
           "the source handle open, so remove() of the original fails)";

    page.reset();  // safe: drops the old document the Page kept alive

    // The refusal must be a refusal, not a partial write: the original is still
    // the readable 3-page document it was before the locked save (GitHub #163).
    {
        auto verifyCtx = std::make_shared<pdfe::core::MuPdfContext>();
        auto reopened = pdfe::core::Document::open(verifyCtx, tempPdf);
        ASSERT_NE(reopened, nullptr)
            << "the locked save left the original unreadable — it half-wrote "
               "before throwing";
        EXPECT_EQ(reopened->pageCount(), 3);
    }
}

// Dropping the Page before saving removes the only holder, so the save
// succeeds — the remedy the fix for the ordinary save path must achieve.
TEST_F(DocumentSaveFileLockTest, DroppingPageBeforeSaveUnlocksIt) {
    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    auto doc = pdfe::core::Document::open(ctx, tempPdf);
    ASSERT_NE(doc, nullptr);
    doc->markRequiresFullRewrite();

    std::unique_ptr<pdfe::core::Page> page = doc->page(0);
    ASSERT_NE(page, nullptr);
    page.reset();  // release the holder — the drain the caller must perform

    EXPECT_NO_THROW(doc->save(tempPdf));

    auto verifyCtx = std::make_shared<pdfe::core::MuPdfContext>();
    auto reopened = pdfe::core::Document::open(verifyCtx, tempPdf);
    ASSERT_NE(reopened, nullptr);
    EXPECT_EQ(reopened->pageCount(), 3);
}

}  // namespace
