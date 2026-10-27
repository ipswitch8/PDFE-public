// -*- coding: utf-8 -*-
//
// Round-2 finding R2-04 (High, data-loss) — RED→GREEN oracles for the ATOMIC,
// recoverable in-place Document::save().
//
// THE DEFECT THIS TEST PINS
// -------------------------
// The old in-place full-rewrite save wrote a temp, dropped the doc handle, then
//   step 3: std::filesystem::remove(original)   ← DELETES THE ORIGINAL FIRST
//   step 4: std::filesystem::rename(temp, original)
// The remove-then-rename window means a crash — or a rename failure at step 4 —
// leaves the ORIGINAL deleted with only the temp surviving: silent data loss.
//
// THE FIX
// -------
//   * Never remove-first. Replace the original via an ATOMIC rename-over
//     (MoveFileExW MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH on Windows,
//     std::filesystem::rename on POSIX) — the original is byte-intact until the
//     instant it is replaced, and on ANY failure it is still on disk.
//   * fsync the temp before the rename (durability).
//   * On failure, the Document reopens the untouched original and stays usable.
//   * On open, a sibling "<path>.pdfe_tmp" (a crashed prior save's fully-written
//     new version) is DETECTED and surfaced via pendingRecoveredTemp() — never
//     silently applied over the user's file.
//
// THE ORACLES
// -----------
//   1. RenameFailurePreservesOriginal — inject a rename-over failure; assert the
//      original bytes are unchanged, the Document stays usable, and a save to a
//      different path still succeeds. (Windows-only injection.)
//   2. OrphanTempRecovery — a sibling .pdfe_tmp is detected on open and the
//      user's original is not clobbered.
//   3. AtomicityHappyPath — a normal in-place save leaves exactly the target
//      file (no leftover .pdfe_tmp) with the edit persisted.

#include <gtest/gtest.h>

#include "core/AtomicFileReplace.h"
#include "core/Document.h"
#include "core/Page.h"
#include "core/MuPdfContext.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <system_error>

#ifndef TEST_FIXTURES_DIR
#define TEST_FIXTURES_DIR "."
#endif

namespace {

using pdfe::core::Document;
using pdfe::core::MuPdfContext;

std::string readAll(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f),
                       std::istreambuf_iterator<char>());
}

class DocumentSaveAtomicTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::filesystem::path fixture =
            std::filesystem::path(TEST_FIXTURES_DIR) / "test_3page.pdf";
        if (!std::filesystem::exists(fixture)) {
            GTEST_SKIP() << "test_3page.pdf not available";
        }
        tempPdf = std::filesystem::temp_directory_path() /
                  "pdfe_save_atomic_r2_04.pdf";
        // The R2-04 crash-recovery artifact shape: a COMPLETE full rewrite
        // that never got renamed. OrphanTempRecovery below plants one of these
        // deliberately and asserts open() surfaces it — so this must be the
        // full-rewrite name, not a generic staging-residue match.
        tempSibling = pdfe::core::fullRewriteTempPathFor(tempPdf);

        std::error_code ec;
        std::filesystem::remove(tempPdf, ec);
        std::filesystem::remove(tempSibling, ec);
        std::filesystem::copy_file(
            fixture, tempPdf,
            std::filesystem::copy_options::overwrite_existing, ec);
        ASSERT_FALSE(ec) << "failed to stage temp copy: " << ec.message();
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove(tempPdf, ec);
        std::filesystem::remove(tempSibling, ec);
    }

    std::filesystem::path tempPdf;
    std::filesystem::path tempSibling;
};

#if defined(_WIN32)
// (1) An atomic-replace FAILURE must leave the ORIGINAL byte-intact and the
// Document usable — never a deleted original nor a dead (null-handle) Document.
//
// Injection: hold a live Page across save(). The Page keeps the source
// fz_document open (fz_keep_document), so the atomic rename-over of the original
// cannot delete the still-open destination — MoveFileExW fails every one of its
// 10×100ms retries and save() throws AT THE ATOMIC-REPLACE STEP (the R2-04
// failure branch). This is the exact residual-file-lock holder proven by
// test_document_save_file_lock.cpp (HeldPageLocksFullRewriteSave), and unlike a
// temp-file lock it does NOT interfere with pdf_save_document writing the temp,
// so the throw genuinely originates at the replace, not the temp write.
//
// NOTE on the atomic-vs-remove-first mutation: a lock on the destination that
// fails the rename ALSO fails the old remove-first code's remove() of that same
// destination (both need to delete it), so the old code would likewise preserve
// the original HERE. The remove-first data-loss window (original gone, only temp
// left) is only reachable via a real crash between the remove and the rename, so
// it cannot be reproduced by an in-process lock. This test therefore pins the
// fix's FAILURE-BRANCH GUARANTEES (original intact + Document reopened & usable);
// the deterministic mutation-survivor for R2-04 rides on the orphan-recovery
// scan (see OrphanTempRecoveryDetectedWithoutClobber).
TEST_F(DocumentSaveAtomicTest, AtomicReplaceFailurePreservesOriginalAndKeepsDocUsable) {
    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, tempPdf);
    ASSERT_NE(doc, nullptr);
    ASSERT_EQ(doc->pageCount(), 3);

    // A real structural edit — the change that would be LOST if the save deleted
    // the original — and force the full-rewrite temp+rename path.
    doc->insertBlankPage(1, 300.0f, 300.0f);
    doc->markRequiresFullRewrite();
    ASSERT_EQ(doc->pageCount(), 4);

    // Snapshot the ORIGINAL on-disk bytes (the edit is only in memory so far).
    const std::string originalBytes = readAll(tempPdf);
    ASSERT_FALSE(originalBytes.empty());

    // Inject the atomic-replace failure: a held live Page locks the source file.
    std::unique_ptr<pdfe::core::Page> holder = doc->page(0);
    ASSERT_NE(holder, nullptr);

    // (a) save threw (the held Page blocks the rename-over of the original).
    EXPECT_ANY_THROW(doc->save(tempPdf))
        << "save must throw when the original cannot be atomically replaced";

    // Drop the holder — safe: it kept the OLD (pre-save) document alive, distinct
    // from the ORIGINAL that save() reopened on failure.
    holder.reset();

    // (b) the ORIGINAL bytes are UNCHANGED — never deleted, never truncated.
    const std::string afterBytes = readAll(tempPdf);
    EXPECT_EQ(afterBytes, originalBytes)
        << "a failed atomic save must leave the original byte-identical";

    // (c) the Document is still usable (reopened the untouched original — NOT a
    // dead null handle, which the old rename-failure branch left behind).
    EXPECT_NO_THROW({ (void)doc->pageCount(); });
    EXPECT_GT(doc->pageCount(), 0);

    // A save to a DIFFERENT path still succeeds (the doc handles are live).
    std::filesystem::path otherPath =
        std::filesystem::temp_directory_path() / "pdfe_save_atomic_other.pdf";
    std::error_code ec;
    std::filesystem::remove(otherPath, ec);
    EXPECT_NO_THROW(doc->saveAs(otherPath));
    {
        auto vctx = std::make_shared<MuPdfContext>();
        auto reop = Document::open(vctx, otherPath);
        ASSERT_NE(reop, nullptr);
        EXPECT_GT(reop->pageCount(), 0);
    }
    std::filesystem::remove(otherPath, ec);
    std::filesystem::remove(tempSibling, ec);
}
#endif  // _WIN32

// (2) An orphaned "<path>.pdfe_tmp" (a crashed prior save's fully-written new
// version) is detected on open and reported via pendingRecoveredTemp(); the
// user's original is not clobbered.
TEST_F(DocumentSaveAtomicTest, OrphanTempRecoveryDetectedWithoutClobber) {
    // Simulate a crash after temp-write, before the rename: a complete sibling.
    std::error_code ec;
    std::filesystem::copy_file(
        tempPdf, tempSibling,
        std::filesystem::copy_options::overwrite_existing, ec);
    ASSERT_FALSE(ec) << "failed to stage orphan temp: " << ec.message();

    const std::string originalBytes = readAll(tempPdf);
    ASSERT_FALSE(originalBytes.empty());

    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, tempPdf);
    ASSERT_NE(doc, nullptr);

    // The orphan is detected and surfaced.
    auto pending = doc->pendingRecoveredTemp();
    ASSERT_TRUE(pending.has_value())
        << "open() must detect a sibling .pdfe_tmp left by a crashed save";
    EXPECT_EQ(std::filesystem::path(*pending), tempSibling);

    // The user's original file was NOT overwritten on open.
    EXPECT_EQ(readAll(tempPdf), originalBytes)
        << "open() must never clobber the user's file with the orphan temp";
    // The orphan itself is left on disk for the caller to recover from.
    EXPECT_TRUE(std::filesystem::exists(tempSibling));

    doc.reset();
    std::filesystem::remove(tempSibling, ec);
}

// Control: with no orphan sibling, pendingRecoveredTemp() is empty.
TEST_F(DocumentSaveAtomicTest, NoOrphanTempReportsNothing) {
    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, tempPdf);
    ASSERT_NE(doc, nullptr);
    EXPECT_FALSE(doc->pendingRecoveredTemp().has_value());
}

// (3) A normal in-place full-rewrite save leaves EXACTLY the target file (no
// leftover .pdfe_tmp) with the edit persisted.
TEST_F(DocumentSaveAtomicTest, HappyPathLeavesOnlyTargetWithEditPersisted) {
    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, tempPdf);
    ASSERT_NE(doc, nullptr);
    ASSERT_EQ(doc->pageCount(), 3);

    doc->insertBlankPage(1, 320.0f, 320.0f);
    doc->markRequiresFullRewrite();
    ASSERT_EQ(doc->pageCount(), 4);

    ASSERT_NO_THROW(doc->save(tempPdf));

    // No leftover temp.
    EXPECT_FALSE(std::filesystem::exists(tempSibling))
        << "a successful atomic save must leave no .pdfe_tmp orphan";

    // The edit persisted: reopen and confirm the inserted page is on disk.
    doc.reset();
    auto vctx = std::make_shared<MuPdfContext>();
    auto reopened = Document::open(vctx, tempPdf);
    ASSERT_NE(reopened, nullptr);
    EXPECT_EQ(reopened->pageCount(), 4)
        << "the in-place save must persist the inserted page";
    EXPECT_FALSE(reopened->pendingRecoveredTemp().has_value());
}

}  // namespace
