// -*- coding: utf-8 -*-
//
// Round-2 finding R2-02 — RED→GREEN class oracle for the EditTextCommand
// empty-replacement "undo after a full-rewrite save" heap-use-after-free.
//
// THE DEFECT THIS TEST PINS
// -------------------------
// When an edit deletes all text in a span (newText == ""), the find-and-
// replace pipeline can no longer inject the original glyphs back on undo — the
// run it would anchor to is gone. So EditTextCommand::execute() snapshots the
// page's /Contents object and undo() restores it directly.
//
// The snapshot was a RAW pdf_keep_obj into the LIVE pdf_document
// (ContentStreamEditor::captureContentsSnapshot). A full-rewrite save
// invalidates that raw pointer:
//   * Document::save() (in-place File>Save) DROPS pImpl->doc and REOPENS a
//     fresh pdf_document — the kept obj now points into freed memory.
//   * Document::saveAs() GC-renumbers the in-memory xref.
// A post-save undo() then dereferences that dangling obj via pdf_dict_put →
// heap-use-after-free (a hard crash under ASan, a cross-document assert /
// corruption in Debug).
//
// THE FIX (mirrors R2-01 image family + Document::RetainedPageHolder)
// ------------------------------------------------------------------
// captureContentsSnapshot deep-copies /Contents into its OWN standalone
// pdf_document (pdf_graft_object via pdfe::core::makeRetainedObjHolder), so it
// is document-independent and survives any save. restoreContentsSnapshot grafts
// a COPY back into the live document at undo time (non-consuming);
// dropContentsSnapshot frees the holder.
//
// THE ORACLE
// ----------
// Two cases:
//   NormalReplacement  — a non-empty edit (does NOT snapshot /Contents; the
//                        applyText undo path handles it). Guards that the
//                        holder change did not regress the common path.
//   EmptyReplacement   — the empty-replacement edit that DOES capture the
//                        snapshot — this is the UAF site.
// Each: open a real fixture, execute via a real UndoManager, perform an
// in-place FULL-REWRITE save through the production saveWithCommandPageRebind
// wrapper (with a live commandPages vector + markRequiresFullRewrite), then
// undo() and assert the original text is restored SOMEWHERE on page 0
// (content oracle — extraction re-orders spans by Y after an edit, so index
// stability is out of scope; byte-content restoration is the contract).
// RED (crash / corruption / text-not-restored) on the unpatched snapshot code;
// GREEN after the fix, Debug and msvc-asan.

#include <gtest/gtest.h>

#include "textedit/TextEditCommands.h"
#include "textedit/TextEditModel.h"
#include "core/Document.h"
#include "core/Page.h"
#include "core/MuPdfContext.h"
#include "core/UndoManager.h"
#include "core/TextExtractor.h"
#include "core/SaveWithRebind.h"

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
using pdfe::core::TextExtractor;
using pdfe::commands::UndoManager;
using pdfe::textedit::EditTextCommand;
using pdfe::textedit::TextEditModel;
using pdfe::textedit::TextSpan;

namespace {

enum class EditKind { Normal, Empty };

// Does any span on page 0 carry exactly `wanted`?
bool pageContainsSpanText(MuPdfContext* ctx, Document* doc, int pageIdx,
                          const std::u32string& wanted) {
    auto page = doc->page(pageIdx);
    if (!page) return false;
    TextExtractor extractor(
        std::shared_ptr<MuPdfContext>(ctx, [](MuPdfContext*) {}));
    auto blocks = extractor.extract(page.get());
    TextEditModel model(blocks);
    for (int i = 0; i < model.spanCount(); ++i) {
        if (model.span(i).text == wanted) return true;
    }
    return false;
}

class TextEditUndoAfterSaveTest : public ::testing::TestWithParam<EditKind> {
protected:
    void SetUp() override {
        std::filesystem::path fixture =
            std::filesystem::path(TEST_FIXTURES_DIR) / "test_3page.pdf";
        if (!std::filesystem::exists(fixture)) {
            GTEST_SKIP() << "test_3page.pdf not available";
        }
        const char* tag = GetParam() == EditKind::Empty ? "empty" : "normal";
        tempPdf_ = std::filesystem::temp_directory_path() /
                   (std::string("pdfe_text_undo_after_save_") + tag + ".pdf");
        std::error_code ec;
        std::filesystem::remove(tempPdf_, ec);
        std::filesystem::copy_file(
            fixture, tempPdf_,
            std::filesystem::copy_options::overwrite_existing, ec);
        ASSERT_FALSE(ec) << "failed to stage temp copy: " << ec.message();

        context_ = std::make_shared<MuPdfContext>();
        undo_ = std::make_unique<UndoManager>();
    }

    void TearDown() override {
        undo_.reset();
        context_.reset();
        std::error_code ec;
        std::filesystem::remove(tempPdf_, ec);
    }

    std::filesystem::path tempPdf_;
    std::shared_ptr<MuPdfContext> context_;
    std::unique_ptr<UndoManager> undo_;
};

TEST_P(TextEditUndoAfterSaveTest, UndoAfterFullRewriteSaveRestoresOriginal) {
    auto doc = Document::open(context_, tempPdf_);
    ASSERT_NE(doc, nullptr);
    ASSERT_GT(doc->pageCount(), 0);

    // Baseline: capture span 0's original text.
    std::u32string originalText;
    {
        auto page = doc->page(0);
        TextExtractor extractor(context_);
        auto blocks = extractor.extract(page.get());
        ASSERT_FALSE(blocks.empty());
        TextEditModel model(blocks);
        ASSERT_GT(model.spanCount(), 0);
        originalText = model.span(0).text;
        ASSERT_FALSE(originalText.empty());
    }

    // Build the edit from the current on-page span 0.
    TextSpan span;
    {
        auto page = doc->page(0);
        TextExtractor extractor(context_);
        auto blocks = extractor.extract(page.get());
        TextEditModel m(blocks);
        span = m.span(0);
    }
    std::u32string newText = (GetParam() == EditKind::Empty)
                                 ? std::u32string{}
                                 : originalText + U".EDIT";

    undo_->execute(std::make_unique<EditTextCommand>(
        doc.get(), context_, /*pageIndex=*/0, /*spanIndex=*/0,
        span, originalText, newText));

    // commandPages owns the only live Page holder; saveWithCommandPageRebind
    // clears it so the in-place full-rewrite drop+reopen can proceed. The
    // EditTextCommand resolves its page by index each apply, so it does not
    // ride the rebind, but the wrapper still forces the drop+reopen that
    // dangles a raw /Contents snapshot on the unpatched tree.
    std::vector<std::unique_ptr<Page>> commandPages;
    commandPages.push_back(doc->page(0));

    doc->markRequiresFullRewrite();
    ASSERT_NO_THROW(
        pdfe::core::saveWithCommandPageRebind(*doc, *undo_, commandPages, tempPdf_));

    // The UAF site on the unpatched tree: undo restores the dangling snapshot.
    ASSERT_TRUE(undo_->canUndo());
    ASSERT_NO_THROW(undo_->undo())
        << "post-save undo must not crash/throw (snapshot must survive save)";

    EXPECT_TRUE(pageContainsSpanText(context_.get(), doc.get(), 0, originalText))
        << "undo after full-rewrite save failed to restore original text "
           "anywhere on page 0";

    undo_->clear();
    commandPages.clear();
    doc.reset();
}

INSTANTIATE_TEST_SUITE_P(
    BothEditKinds, TextEditUndoAfterSaveTest,
    ::testing::Values(EditKind::Normal, EditKind::Empty),
    [](const ::testing::TestParamInfo<EditKind>& info) {
        return info.param == EditKind::Empty ? "EmptyReplacement"
                                             : "NormalReplacement";
    });

}  // namespace
