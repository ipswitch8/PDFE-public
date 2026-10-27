// -*- coding: utf-8 -*-
//
// R3-04 — Periodic autosave + crash-recovery, class-level oracle.
//
// WHAT THIS PINS DOWN
// -------------------
// AutosaveManager is a HEADLESS engine (Document + UndoManager only, no UI) so
// the "kill-mid-edit → recovery" contract can be exercised without a real
// process kill: we simulate the crash by DROPPING the Document + AutosaveManager
// WITHOUT a clean save, then run a FRESH static AutosaveManager::scanForRecoverable().
//
// Cases:
//   1. AutosaveWritesRecoverableState  — a real dirty edit + autosaveNow() must
//      produce a recovery PDF copy + a JSON sidecar, and the copy must be a
//      valid, openable PDF carrying the unsaved edit.
//   2. KillMidEditRecoveryRestoresUnsavedWork — after autosave, DROP everything
//      without a clean save (simulated crash); a fresh scanForRecoverable()
//      returns a record for that original path; opening the recovery copy shows
//      the UNSAVED edit (never written back to the original).
//   3. CleanSaveLeavesNothingToRecover — after a real clean Document::save() +
//      AutosaveManager::clearRecovery(), a fresh scan does NOT return the record.
//   4. NoDirtyNoAutosave — opening + autosaveNow() with no edits writes nothing.
//
// The oracles are real state oracles (files exist / don't; content extracted
// from the recovery PDF), never EXPECT_NO_THROW-only.
//
// MUTATION SURVIVOR: disabling the recovery-copy write in
// Document::writeRecoveryCopy (or the scan) re-REDs cases 1 & 2.

#include <gtest/gtest.h>

#include "core/AutosaveManager.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "core/TextExtractor.h"
#include "core/UndoManager.h"
#include "textedit/TextEditCommands.h"
#include "textedit/TextEditModel.h"

#include <filesystem>
#include <fstream>
#include <iterator>
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
using pdfe::core::Page;
using pdfe::core::RecoveryRecord;
using pdfe::core::TextExtractor;
using pdfe::textedit::EditTextCommand;
using pdfe::textedit::TextEditModel;
using pdfe::textedit::TextSpan;

namespace {

// Does any span on page 0 of the document at `path` carry exactly `wanted`?
bool filePageContainsSpanText(const std::filesystem::path& path,
                              const std::u32string& wanted) {
    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, path);
    if (!doc || doc->pageCount() == 0) return false;
    auto page = doc->page(0);
    if (!page) return false;
    TextExtractor extractor(ctx);
    auto blocks = extractor.extract(page.get());
    TextEditModel model(blocks);
    for (int i = 0; i < model.spanCount(); ++i) {
        if (model.span(i).text == wanted) return true;
    }
    return false;
}

class AutosaveRecoveryTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::filesystem::path fixture =
            std::filesystem::path(TEST_FIXTURES_DIR) / "test_3page.pdf";
        if (!std::filesystem::exists(fixture)) {
            GTEST_SKIP() << "test_3page.pdf not available";
        }

        // Test-scoped, isolated recovery directory (never touches real AppData).
        recoveryDir_ = std::filesystem::temp_directory_path() /
                       "pdfe_autosave_recovery_r304_test";
        std::error_code ec;
        std::filesystem::remove_all(recoveryDir_, ec);
        std::filesystem::create_directories(recoveryDir_, ec);

        tempPdf_ = std::filesystem::temp_directory_path() /
                   "pdfe_autosave_source_r304.pdf";
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
        std::filesystem::remove_all(recoveryDir_, ec);
        std::filesystem::remove(tempPdf_, ec);
    }

    // Extract span 0's original text from the source doc.
    std::u32string originalSpan0Text(Document* doc) {
        auto page = doc->page(0);
        EXPECT_NE(page, nullptr);
        TextExtractor extractor(context_);
        auto blocks = extractor.extract(page.get());
        EXPECT_FALSE(blocks.empty());
        TextEditModel model(blocks);
        EXPECT_GT(model.spanCount(), 0);
        return model.span(0).text;
    }

    // Execute a real dirty edit on span 0 → `original + ".EDIT"` via a real
    // UndoManager. Returns the edited text.
    std::u32string makeDirtyEdit(Document* doc, UndoManager* undo,
                                 const std::u32string& originalText) {
        TextSpan span;
        {
            auto page = doc->page(0);
            TextExtractor extractor(context_);
            auto blocks = extractor.extract(page.get());
            TextEditModel m(blocks);
            span = m.span(0);
        }
        std::u32string newText = originalText + U".EDIT";
        undo->execute(std::make_unique<EditTextCommand>(
            doc, context_, /*pageIndex=*/0, /*spanIndex=*/0,
            span, originalText, newText));
        return newText;
    }

    std::filesystem::path recoveryDir_;
    std::filesystem::path tempPdf_;
    std::shared_ptr<MuPdfContext> context_;
};

// Case 1: a dirty edit + autosaveNow() writes a recovery copy + sidecar, and
// the copy is a valid PDF carrying the unsaved edit.
TEST_F(AutosaveRecoveryTest, AutosaveWritesRecoverableState) {
    auto doc = Document::open(context_, tempPdf_);
    ASSERT_NE(doc, nullptr);
    auto undo = std::make_unique<UndoManager>();

    std::u32string originalText = originalSpan0Text(doc.get());
    ASSERT_FALSE(originalText.empty());
    std::u32string editedText = makeDirtyEdit(doc.get(), undo.get(), originalText);

    ASSERT_FALSE(undo->isClean()) << "edit must leave the undo stack dirty";

    AutosaveManager mgr(doc.get(), undo.get(), tempPdf_, recoveryDir_);
    ASSERT_TRUE(mgr.autosaveNow()) << "dirty doc must autosave";

    // State oracle: both the recovery copy and its sidecar now exist.
    EXPECT_TRUE(std::filesystem::exists(mgr.recoveryFilePath()))
        << "recovery PDF copy must be written";
    EXPECT_TRUE(std::filesystem::exists(mgr.sidecarPath()))
        << "recovery JSON sidecar must be written";

    // Content oracle: the recovery copy is a valid PDF carrying the EDIT.
    EXPECT_TRUE(filePageContainsSpanText(mgr.recoveryFilePath(), editedText))
        << "recovery copy must carry the unsaved edit";

    // The original source on disk was NEVER saved — it still has the original.
    EXPECT_TRUE(filePageContainsSpanText(tempPdf_, originalText))
        << "the untouched source must still hold the pre-edit text";
    EXPECT_FALSE(filePageContainsSpanText(tempPdf_, editedText))
        << "the edit must NOT have leaked into the unsaved source";
}

// Case 2: kill-mid-edit → recovery restores unsaved work.
TEST_F(AutosaveRecoveryTest, KillMidEditRecoveryRestoresUnsavedWork) {
    std::u32string editedText;
    std::filesystem::path recoveryCopy;
    {
        auto doc = Document::open(context_, tempPdf_);
        ASSERT_NE(doc, nullptr);
        auto undo = std::make_unique<UndoManager>();

        std::u32string originalText = originalSpan0Text(doc.get());
        editedText = makeDirtyEdit(doc.get(), undo.get(), originalText);

        auto mgr = std::make_unique<AutosaveManager>(
            doc.get(), undo.get(), tempPdf_, recoveryDir_);
        ASSERT_TRUE(mgr->autosaveNow());
        recoveryCopy = mgr->recoveryFilePath();

        // SIMULATED CRASH: drop the manager + undo + document WITHOUT a clean
        // save and WITHOUT clearRecovery(). The sidecar must survive.
        mgr.reset();
        undo.reset();
        doc.reset();
    }

    // A FRESH scan (as a new process would run at launch) finds the session.
    std::vector<RecoveryRecord> found =
        AutosaveManager::scanForRecoverable(recoveryDir_);
    ASSERT_FALSE(found.empty())
        << "scan must surface the un-cleanly-exited session";

    bool matched = false;
    for (const auto& rec : found) {
        std::error_code ec;
        if (std::filesystem::equivalent(rec.originalPath, tempPdf_, ec)) {
            matched = true;
            EXPECT_EQ(rec.recoveryFilePath, recoveryCopy);
            // Content oracle: open the record's recovery file → the UNSAVED
            // edit is present (it was never saved to the original).
            EXPECT_TRUE(filePageContainsSpanText(rec.recoveryFilePath, editedText))
                << "recovered file must carry the unsaved edit";
            break;
        }
    }
    EXPECT_TRUE(matched) << "no recovery record mapped back to the source path";
}

// Case 3: clean save leaves nothing to recover.
TEST_F(AutosaveRecoveryTest, CleanSaveLeavesNothingToRecover) {
    std::string sessionId;
    std::u32string editedText;
    {
        auto doc = Document::open(context_, tempPdf_);
        ASSERT_NE(doc, nullptr);
        auto undo = std::make_unique<UndoManager>();

        std::u32string originalText = originalSpan0Text(doc.get());
        editedText = makeDirtyEdit(doc.get(), undo.get(), originalText);

        AutosaveManager mgr(doc.get(), undo.get(), tempPdf_, recoveryDir_);
        ASSERT_TRUE(mgr.autosaveNow());
        ASSERT_TRUE(mgr.hasRecovery());
        sessionId = mgr.sessionId();

        // A real clean save, then mark the undo clean (as MainWindow does after
        // a successful save) and clear the recovery sidecar.
        doc->markRequiresFullRewrite();
        ASSERT_NO_THROW(doc->save(tempPdf_));
        undo->setClean();
        mgr.clearRecovery();

        EXPECT_FALSE(mgr.hasRecovery())
            << "clearRecovery() must remove this session's sidecar";
    }

    // The premise of this test is that the save was CLEAN — i.e. it really
    // persisted the edit. Nothing checked that: a save() that silently did
    // nothing would leave just as little to recover, and the test would still
    // pass while proving the opposite of its name (GitHub #163).
    ASSERT_FALSE(editedText.empty());
    EXPECT_TRUE(filePageContainsSpanText(tempPdf_, editedText))
        << "the 'clean save' never wrote the edit to disk, so there is nothing "
           "to recover for the wrong reason";

    // A fresh scan must NOT return the cleanly-exited session.
    std::vector<RecoveryRecord> found =
        AutosaveManager::scanForRecoverable(recoveryDir_);
    for (const auto& rec : found) {
        EXPECT_NE(rec.sessionId, sessionId)
            << "a cleanly-saved session must not be recoverable";
    }
}

// Case 4: no dirty state → no autosave, no sidecar.
TEST_F(AutosaveRecoveryTest, NoDirtyNoAutosave) {
    auto doc = Document::open(context_, tempPdf_);
    ASSERT_NE(doc, nullptr);
    auto undo = std::make_unique<UndoManager>();

    ASSERT_TRUE(undo->isClean()) << "a freshly opened doc is clean";

    AutosaveManager mgr(doc.get(), undo.get(), tempPdf_, recoveryDir_);
    EXPECT_FALSE(mgr.autosaveNow()) << "clean doc must not autosave";
    EXPECT_FALSE(mgr.hasRecovery()) << "no sidecar for a clean doc";
    EXPECT_FALSE(std::filesystem::exists(mgr.recoveryFilePath()))
        << "no recovery copy for a clean doc";

    // And a scan finds nothing.
    std::vector<RecoveryRecord> found =
        AutosaveManager::scanForRecoverable(recoveryDir_);
    EXPECT_TRUE(found.empty());
}

// Case 5: discardAllRecoverable() clears stale sidecars a force-killed dirty
// session left behind. This is the seam MainWindow::checkForRecoverableSessions
// uses under PDFE_TEST_SKIP_UNSAVED_PROMPT: a UI test that dirtied a document
// and was then killed by PDFESession leaves a sidecar that would otherwise fire
// the startup recovery prompt on a later launch lacking the seam, wedging its
// teardown. The skip-path must actively DISCARD those sidecars, not just ignore
// them. (Mutation oracle: revert the skip-path to a bare `return;` — i.e. remove
// the discardAllRecoverable() call — and this test reds because the sidecar
// survives the scan.)
TEST_F(AutosaveRecoveryTest, DiscardAllRecoverableClearsStaleSidecars) {
    std::filesystem::path recoveryCopy;
    std::filesystem::path sidecar;
    {
        auto doc = Document::open(context_, tempPdf_);
        ASSERT_NE(doc, nullptr);
        auto undo = std::make_unique<UndoManager>();
        std::u32string originalText = originalSpan0Text(doc.get());
        makeDirtyEdit(doc.get(), undo.get(), originalText);

        auto mgr = std::make_unique<AutosaveManager>(
            doc.get(), undo.get(), tempPdf_, recoveryDir_);
        ASSERT_TRUE(mgr->autosaveNow());
        recoveryCopy = mgr->recoveryFilePath();
        sidecar = mgr->sidecarPath();

        // SIMULATED CRASH: drop everything WITHOUT a clean save / clearRecovery.
        mgr.reset();
        undo.reset();
        doc.reset();
    }

    // Precondition: the stale sidecar + copy are on disk and scannable.
    ASSERT_TRUE(std::filesystem::exists(sidecar));
    ASSERT_TRUE(std::filesystem::exists(recoveryCopy));
    ASSERT_FALSE(AutosaveManager::scanForRecoverable(recoveryDir_).empty());

    // The seam under test: discard every recoverable session in the dir.
    const std::size_t discarded =
        AutosaveManager::discardAllRecoverable(recoveryDir_);
    EXPECT_GE(discarded, 1u) << "at least the stale session must be discarded";

    // Post-condition oracle: nothing recoverable remains; both files are gone.
    EXPECT_TRUE(AutosaveManager::scanForRecoverable(recoveryDir_).empty())
        << "stale sidecars must not survive the discard";
    EXPECT_FALSE(std::filesystem::exists(sidecar))
        << "sidecar file must be removed";
    EXPECT_FALSE(std::filesystem::exists(recoveryCopy))
        << "recovery copy must be removed";

    // Idempotent: a second sweep of an already-clean dir discards nothing and
    // does not throw.
    EXPECT_EQ(AutosaveManager::discardAllRecoverable(recoveryDir_), 0u);
}

// ─── M08 + M16 kill (save routing, R-08/R-21/R-23) ──────────────────────────
// writeRecoveryCopy is a FAST no-recompress snapshot: it sets do_compress=0 and
// do_compress_images=0 so existing streams are serialized VERBATIM (the
// ~440ms/23MB perf property). The suite's other cases verify recovered CONTENT
// only, so two mutants survived: forcing do_compress=1 (M16) re-deflates general
// streams, and forcing do_compress_images=1 (M08) re-encodes image streams.
//
// This oracle pins the no-recompress property directly. The committed fixture
// stores an uncompressed content stream carrying an ASCII marker and an
// uncompressed raw-RGB image made of a 0xAB sentinel run. In a verbatim copy
// both sentinels survive byte-for-byte in the raw recovery file; under either
// mutation the corresponding (large, highly-compressible) stream is deflated
// and its sentinel disappears — failing the assertion and killing the mutant.
TEST_F(AutosaveRecoveryTest, RecoveryCopyPreservesVerbatimStreams) {
    std::filesystem::path fixture =
        std::filesystem::path(TEST_FIXTURES_DIR) / "recovery_verbatim_streams.pdf";
    ASSERT_TRUE(std::filesystem::exists(fixture))
        << "verbatim-streams fixture must be present (CMake fixture floor): "
        << fixture;

    auto doc = Document::open(context_, fixture);
    ASSERT_NE(doc, nullptr);

    std::filesystem::path out = recoveryDir_ / "recovery_verbatim_out.pdf";
    std::error_code ec;
    std::filesystem::remove(out, ec);

    ASSERT_NO_THROW(doc->writeRecoveryCopy(out));
    ASSERT_TRUE(std::filesystem::exists(out))
        << "writeRecoveryCopy must produce a recovery file";

    std::ifstream f(out, std::ios::binary);
    ASSERT_TRUE(f.is_open());
    const std::string raw((std::istreambuf_iterator<char>(f)),
                          std::istreambuf_iterator<char>());
    ASSERT_FALSE(raw.empty()) << "recovery file has no bytes";

    // M16 oracle: the uncompressed content stream must survive verbatim, so its
    // ASCII marker is present in the raw bytes. do_compress=1 deflates it away.
    EXPECT_NE(raw.find("RECOVERYVERBATIMMARKER"), std::string::npos)
        << "content-stream marker missing from the recovery copy: the content "
           "stream was re-deflated (do_compress must stay 0 for a fast, "
           "no-recompress recovery snapshot).";

    // M08 oracle: the uncompressed raw image must survive verbatim, so its 0xAB
    // sentinel run is present. do_compress_images=1 re-encodes it away.
    const std::string imageSentinel(512, static_cast<char>(0xAB));
    EXPECT_NE(raw.find(imageSentinel), std::string::npos)
        << "raw image sentinel missing from the recovery copy: the image stream "
           "was re-encoded (do_compress_images must stay 0 for a fast, "
           "no-recompress recovery snapshot).";
}

}  // namespace
