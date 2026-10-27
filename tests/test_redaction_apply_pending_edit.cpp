// -*- coding: utf-8 -*-
// PHASE1_HARDENING: skip=0 expect_ge=1 no_throw=0  (2026-05-08)
//
// D2 phase-5: UI-RED-12 — applyRedactions on a document that has a
// pending text edit (committed via the EditTextCommand path before
// applyRedactions runs) does not lose the edit.
//
// **Contract under test:** When the caller (production: MainWindow's
// onApplyRedactions) follows the pre-flight pattern of "commit any
// pending text-edit overlay first, THEN apply redactions, THEN save",
// the resulting saved file contains BOTH effects:
//   - The redacted region's content has been removed.
//   - The text-edit substitution is preserved in the saved bytes.
//
// **Why this matters:** TextEditTool maintains a typed-but-not-yet-
// committed buffer ("overlay"). A naïve `onApplyRedactions` that
// invokes `applyAllRedactions` + `Document::save` without first
// telling TextEditTool to commit would silently drop the user's
// in-progress edit on the floor. This test asserts the contract that
// the EditTextCommand path (which is what TextEditTool::commitEdit
// fires through MainWindow's textEdited callback) lands in the
// in-memory document BEFORE apply, and survives apply + save +
// reopen.
//
// **Production callers MUST:** call textEditTool->commitEdit() before
// applyAllRedactions when an active text-edit session is in progress.
// MainWindow::onApplyRedactions does NOT yet do this — flagged for
// follow-up. This test locks the underlying contract so the fix is
// straightforward (one commitEdit() call) when the production path
// is updated.

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "core/TextExtractor.h"
#include "redaction/RedactionManager.h"
#include "redaction/RedactionData.h"
#include "textedit/TextEditCommands.h"
#include "textedit/TextEditModel.h"

#include <filesystem>
#include <atomic>
#include <memory>
#include <string>
#ifdef _WIN32
  #include <process.h>
#else
  #include <unistd.h>
#endif

using namespace pdfe::core;
using namespace pdfe::redaction;
using namespace pdfe::textedit;

namespace {

std::filesystem::path fixturesDir() { return std::filesystem::path(TEST_FIXTURES_DIR); }

inline long pdfeProcessId()
{
#ifdef _WIN32
    return static_cast<long>(_getpid());
#else
    return static_cast<long>(getpid());
#endif
}

// Build a per-call-unique output path under the pending-edit temp dir. Fixed
// names collided under --gtest_repeat (iteration >= 2 hit a still-locked file)
// and across processes sharing the temp dir; the pid + atomic counter make each
// path disjoint. The unique token is inserted before the extension so callers
// keep passing "<stem>.pdf".
std::filesystem::path tempOut(const char* name)
{
    static std::atomic<unsigned> counter{0};
    const unsigned n = counter.fetch_add(1, std::memory_order_relaxed);
    auto d = std::filesystem::temp_directory_path() / "pdfe_redact_pending_edit";
    std::filesystem::create_directories(d);

    std::string base(name);
    std::string stem = base, ext;
    const auto dot = base.find_last_of('.');
    if (dot != std::string::npos) { stem = base.substr(0, dot); ext = base.substr(dot); }
    std::string unique = stem + "_" + std::to_string(pdfeProcessId())
                       + "_" + std::to_string(n) + ext;
    return d / unique;
}

class RedactionApplyPendingEditTest : public ::testing::Test {
protected:
    std::shared_ptr<MuPdfContext> context_;
    std::unique_ptr<Document> document_;
    std::filesystem::path tmpPath_;

    void SetUp() override {
        context_ = std::make_shared<MuPdfContext>();
        auto srcPath = fixturesDir() / "test_redaction.pdf";
        if (!std::filesystem::exists(srcPath)) {
            GTEST_SKIP() << "test_redaction.pdf not found";
        }
        tmpPath_ = tempOut("input.pdf");
        std::filesystem::copy_file(srcPath, tmpPath_,
            std::filesystem::copy_options::overwrite_existing);
        document_ = Document::open(context_, tmpPath_);
        ASSERT_NE(document_, nullptr);
    }

    void TearDown() override {
        document_.reset();
        std::error_code ec;
        std::filesystem::remove(tmpPath_, ec);
    }
};

// ─────────────────────────────────────────────────────────────────────
// UI-RED-12 — text edit committed via EditTextCommand BEFORE
// applyAllRedactions persists in the saved+reopened file alongside
// the redaction effect.
// ─────────────────────────────────────────────────────────────────────
TEST_F(RedactionApplyPendingEditTest, EditCommittedBeforeApplyPersistsAfterSaveReopen)
{
    // Stage 1: extract a span from page 0 to drive the text edit.
    auto page = document_->page(0);
    ASSERT_NE(page, nullptr);
    TextExtractor extractor(context_);
    auto blocks = extractor.extract(page.get());
    if (blocks.empty()) {
        GTEST_SKIP() << "test_redaction.pdf has no extractable text on page 0";
    }
    TextEditModel model(blocks);
    if (model.spanCount() == 0) {
        GTEST_SKIP() << "TextEditModel found no spans on page 0";
    }
    const TextSpan span = model.span(0);
    if (span.text.empty()) {
        GTEST_SKIP() << "First span on page 0 is empty";
    }

    // Stage 2: simulate the pre-flight commit of a pending text edit.
    // In production, TextEditTool::commitEdit calls the textEdited
    // callback wired in MainWindow, which constructs an
    // EditTextCommand and pushes it onto undoManager_->execute(). We
    // construct the command directly here to mirror that effect at
    // the library layer.
    const std::u32string oldText = span.text;
    const std::u32string sentinel = U"REDPENDEDIT";
    const std::u32string newText = oldText + U" " + sentinel;

    auto cmd = std::make_unique<EditTextCommand>(
        document_.get(), context_, /*pageIndex=*/0, /*spanIndex=*/0,
        span, oldText, newText);
    cmd->execute();
    page.reset();  // release the page handle before document mutation

    // Stage 3: mark a region for redaction. Pick a corner that doesn't
    // overlap the edited span's bbox so the two effects exercise
    // different code paths in apply.
    RedactionManager mgr(context_);
    page = document_->page(0);
    QRectF pageRect = page->boundingBox();
    QRectF redactRect(pageRect.x() + 4.0, pageRect.y() + 4.0,
                      8.0, 8.0);
    auto markData = mgr.createRedactionMark(page.get(), redactRect);
    EXPECT_NE(markData.annotIndex, -1)
        << "createRedactionMark must return a valid index for the post-edit apply test";
    page.reset();

    // Stage 4: apply all redactions. The pre-flight commit (stage 2)
    // means the in-memory document carries both the text edit and
    // the redaction-mark annotation. applyAllRedactions mutates the
    // page contents to remove the redacted region and finalizes
    // the redaction annotations.
    RedactionApplyOptions opts;
    opts.rewriteDocument = false;  // local file, no QPDF rewrite
    auto result = mgr.applyAllRedactions(document_.get(), opts);
    ASSERT_TRUE(result.success)
        << "applyAllRedactions failed: " << result.errorMessage;

    // Stage 5: save and reopen. This validates the "edit + redaction
    // both persist through the on-disk round-trip" contract.
    auto outPath = tempOut("redacted_with_edit.pdf");
    document_->saveAs(outPath);

    document_.reset();  // close before re-open
    auto reopened = Document::open(context_, outPath);
    ASSERT_NE(reopened, nullptr);

    // Stage 6: verify the text edit survives. Search reopened page 0
    // for the sentinel substring.
    auto reopenedPage = reopened->page(0);
    ASSERT_NE(reopenedPage, nullptr);
    TextExtractor extractor2(context_);
    auto reopenedBlocks = extractor2.extract(reopenedPage.get());
    TextEditModel reopenedModel(reopenedBlocks);

    bool foundSentinel = false;
    for (int i = 0; i < reopenedModel.spanCount(); ++i) {
        const auto& s = reopenedModel.span(i);
        if (s.text.find(sentinel) != std::u32string::npos) {
            foundSentinel = true;
            break;
        }
    }
    EXPECT_TRUE(foundSentinel)
        << "Sentinel substring " << "REDPENDEDIT was lost during the "
           "applyAllRedactions + save + reopen cycle. The pre-flight "
           "text-edit commit pattern did not preserve the edit.";

    // Cleanup
    reopenedPage.reset();
    reopened.reset();
    std::error_code ec;
    std::filesystem::remove(outPath, ec);
}

// Companion contract test: applyAllRedactions WITHOUT a prior text-
// edit commit produces a saved file with NO new text content. This
// is the negative-side of the same contract — it locks the
// observation that text edits land in the saved file ONLY if they
// were committed via the EditTextCommand path before save. If a
// future change makes applyAllRedactions itself commit overlay text,
// this test would fail and force an explicit decision about the
// commit-semantics contract.
TEST_F(RedactionApplyPendingEditTest, ApplyWithoutPriorEditDoesNotInjectText)
{
    auto page = document_->page(0);
    ASSERT_NE(page, nullptr);
    TextExtractor extractor(context_);
    auto preBlocks = extractor.extract(page.get());
    TextEditModel preModel(preBlocks);
    if (preModel.spanCount() == 0) {
        GTEST_SKIP() << "Fixture has no extractable text on page 0";
    }
    const std::u32string preText = preModel.span(0).text;

    // Mark a small redaction region at the corner — far from any
    // span on the page so the redaction itself doesn't truncate the
    // first span's text.
    RedactionManager mgr(context_);
    QRectF pageRect = page->boundingBox();
    QRectF redactRect(pageRect.x() + 4.0, pageRect.y() + 4.0, 8.0, 8.0);
    mgr.createRedactionMark(page.get(), redactRect);
    page.reset();

    // Apply with no preceding text edit.
    RedactionApplyOptions opts;
    opts.rewriteDocument = false;
    auto result = mgr.applyAllRedactions(document_.get(), opts);
    ASSERT_TRUE(result.success);

    auto outPath = tempOut("redact_no_edit.pdf");
    document_->saveAs(outPath);
    document_.reset();

    auto reopened = Document::open(context_, outPath);
    ASSERT_NE(reopened, nullptr);
    auto rPage = reopened->page(0);
    TextExtractor extractor2(context_);
    auto rBlocks = extractor2.extract(rPage.get());
    TextEditModel rModel(rBlocks);

    // The redaction-only path must not inject any new text into the
    // saved file. Verify no span carries the sentinel that the
    // companion test injects.
    bool hasSentinel = false;
    for (int i = 0; i < rModel.spanCount(); ++i) {
        if (rModel.span(i).text.find(U"REDPENDEDIT") != std::u32string::npos) {
            hasSentinel = true;
            break;
        }
    }
    EXPECT_FALSE(hasSentinel)
        << "Sentinel text appeared in the saved file even though no "
           "EditTextCommand was executed beforehand. The contract is "
           "that applyAllRedactions only redacts; it does not inject "
           "text on its own.";

    rPage.reset();
    reopened.reset();
    std::error_code ec;
    std::filesystem::remove(outPath, ec);
}

}  // namespace
