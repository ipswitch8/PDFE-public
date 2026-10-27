// tests/test_text_edit_save_roundtrip.cpp
//
// Phase-3 regression locks (failing-test-first) for UI-TE-SAVE-* rows in
// tests/fixtures/ui_audit/UI_MATRIX_GAPS.md.
//
// Locks the contract that text-edit changes (text + font family + size +
// bold + multi-format runs) survive a save→reopen→re-extract cycle.
//
// Coverage (rows from UI_MATRIX_GAPS.md):
//   UI-TE-SAVE-01  Reported bug: toolbar font/size never reaches saved
//                  content stream. Locked via font-name + font-size
//                  round-trip extraction.
//   UI-TE-SAVE-02  Overlay font family persists through save→reopen.
//   UI-TE-SAVE-03  Overlay font size persists within ±1pt through
//                  save→reopen.
//   UI-TE-SAVE-04  Bold toggle persists through save→reopen.
//   UI-TE-SAVE-08  Multi-format-run span (one bold word inside a span)
//                  preserves per-run distinction on save.
//   UI-TE-SAVE-13  Edit on a non-Y-flipping CTM page (proxy for the
//                  Lean Startup p2 reproduction) preserves the embedded
//                  font reference rather than swapping to Arial.
//
// Tests intentionally fail RED on the predecessor commit; baseline failure
// log in tests/fixtures/ui_audit/phase_text_edit_baseline_failures.txt.

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "core/TextExtractor.h"
#include "core/UndoManager.h"
#include "textedit/ContentStreamEditor.h"
#include "textedit/TextEditCommands.h"
#include "textedit/TextEditModel.h"

#include <QRectF>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

using namespace pdfe::textedit;
using namespace pdfe::core;
using namespace pdfe::commands;

namespace {

// Small wrapper that opens a fixture, performs an edit on span 0, saves
// to a temp file, reopens, and returns the post-reopen TextSpan at index 0.
class TextEditSaveRoundtripTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_shared<MuPdfContext>();
        std::filesystem::path fixtures = TEST_FIXTURES_DIR;
        fixtureSrc_ = fixtures / "test_3page.pdf";
        if (!std::filesystem::exists(fixtureSrc_)) {
            GTEST_SKIP() << "Test fixture not available: " << fixtureSrc_;
        }
        // Generate a unique temp output path per test
        const auto& info = *::testing::UnitTest::GetInstance()->current_test_info();
        tempOut_ = std::filesystem::temp_directory_path() /
                   ("pdfe_save_roundtrip_" + std::string(info.name()) + ".pdf");
        std::error_code ec;
        std::filesystem::remove(tempOut_, ec);
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove(tempOut_, ec);
    }

    // Apply an edit (text + optional FormattingRun overlay) and save to tempOut_.
    // Then reopen tempOut_ and return the first span on page 0.
    TextSpan editSaveReopen(
        const std::u32string& newText,
        const std::vector<FormattingRun>& overlayRuns)
    {
        auto doc = Document::open(ctx_, fixtureSrc_);
        EXPECT_NE(doc, nullptr);
        auto page = doc->page(0);
        EXPECT_NE(page, nullptr);

        TextExtractor extractor(ctx_);
        auto blocks = extractor.extract(page.get());
        TextEditModel model(blocks);
        EXPECT_GT(model.spanCount(), 0);

        TextSpan editSpan = model.span(0);
        std::u32string oldText = editSpan.text;
        editSpan.formattingRuns = overlayRuns;
        editSpan.text = newText;  // applyText path uses parameter, not span.text

        UndoManager undo;
        auto cmd = std::make_unique<EditTextCommand>(
            doc.get(), ctx_, /*pageIndex=*/0, /*spanIndex=*/0,
            editSpan, oldText, newText);
        undo.execute(std::move(cmd));

        // Save edited document
        doc->saveAs(tempOut_);
        doc.reset();

        // Reopen, re-extract span 0
        auto reopened = Document::open(ctx_, tempOut_);
        EXPECT_NE(reopened, nullptr);
        auto rePage = reopened->page(0);
        EXPECT_NE(rePage, nullptr);
        TextExtractor extractor2(ctx_);
        auto blocks2 = extractor2.extract(rePage.get());
        TextEditModel model2(blocks2);
        EXPECT_GT(model2.spanCount(), 0);
        return model2.span(0);
    }

    // Helper: capture the original-span baseline once.
    TextSpan originalSpan() {
        auto doc = Document::open(ctx_, fixtureSrc_);
        auto page = doc->page(0);
        TextExtractor extractor(ctx_);
        auto blocks = extractor.extract(page.get());
        TextEditModel model(blocks);
        return model.span(0);
    }

    std::shared_ptr<MuPdfContext> ctx_;
    std::filesystem::path fixtureSrc_;
    std::filesystem::path tempOut_;
};

}  // namespace

// ─────────────────────────────────────────────────────────────────────
// UI-TE-SAVE-13 / UI-TE-SAVE-01 — vector-PDF text edit preserves the
// embedded font reference (i.e., does not swap to Arial). Asserted via
// fontName comparison after reopen.
// ─────────────────────────────────────────────────────────────────────
TEST_F(TextEditSaveRoundtripTest, EditPreservesEmbeddedFontReference) {
    TextSpan orig = originalSpan();
    ASSERT_FALSE(orig.fontName.empty())
        << "Test fixture must have a non-empty embedded font name on span 0.";
    ASSERT_FALSE(orig.text.empty());

    // Ascii-only edit so glyph coverage is not the issue
    std::u32string newText = orig.text;
    if (!newText.empty() && newText[0] >= U'A' && newText[0] <= U'Z') {
        newText[0] = (newText[0] == U'A' ? U'B' : U'A');
    } else if (!newText.empty() && newText[0] >= U'a' && newText[0] <= U'z') {
        newText[0] = (newText[0] == U'a' ? U'b' : U'a');
    }
    if (newText == orig.text) {
        newText.push_back(U'.');
    }

    TextSpan after = editSaveReopen(newText, /*overlayRuns=*/{});

    // Strip any subset prefix ("XXXXXX+" or similar) for comparison
    auto strip = [](const std::string& f) {
        size_t plus = f.find('+');
        return plus == std::string::npos ? f : f.substr(plus + 1);
    };

    EXPECT_EQ(strip(after.fontName), strip(orig.fontName))
        << "Edit must preserve the embedded source font reference. "
           "Saw: " << after.fontName << ", expected: " << orig.fontName;
}

// ─────────────────────────────────────────────────────────────────────
// UI-TE-SAVE-03 — font size round-trip within ±1pt.
// ─────────────────────────────────────────────────────────────────────
TEST_F(TextEditSaveRoundtripTest, FontSizeRoundTripWithinOnePt) {
    TextSpan orig = originalSpan();
    ASSERT_GT(orig.fontSize, 0.0f);

    // Edit text only (no fmt run) — size MUST round-trip unchanged.
    std::u32string newText = orig.text + U".";
    TextSpan after = editSaveReopen(newText, /*overlayRuns=*/{});

    EXPECT_NEAR(after.fontSize, orig.fontSize, 1.0f)
        << "Font size drifted across save/reopen by more than 1pt. "
           "Original: " << orig.fontSize << ", after: " << after.fontSize;
}

// ─────────────────────────────────────────────────────────────────────
// UI-TE-SAVE-02 — overlay font-family change reaches saved content stream.
// Builds a FormattingRun specifying a different family and verifies the
// reopened span carries a corresponding font reference (expected to fail
// RED on UI-TE-SAVE-01-affected code).
// ─────────────────────────────────────────────────────────────────────
TEST_F(TextEditSaveRoundtripTest, OverlayFontFamilyReachesContentStream) {
#ifndef _WIN32
    GTEST_SKIP() << "Font swap embed pipeline depends on "
                    "SystemFontDiscovery Linux impl";
#endif
    TextSpan orig = originalSpan();
    ASSERT_FALSE(orig.text.empty());

    FormattingRun run;
    run.charStart = 0;
    run.charEnd = static_cast<int>(orig.text.size());
    run.fontFamily = "Times New Roman";
    run.fontSize = orig.fontSize;
    // No fontFilePath — this is the documented over-simplification for
    // UI-TE-SAVE-01: applyText skips embedding when fontFilePath is empty
    // and the family change never reaches the content stream. We assert
    // the contract that family change MUST result in a font swap.

    TextSpan after = editSaveReopen(orig.text, {run});

    auto strip = [](const std::string& f) {
        size_t plus = f.find('+');
        return plus == std::string::npos ? f : f.substr(plus + 1);
    };

    // The post-reopen font name MUST differ from the original (since
    // we asked for Times New Roman). On the broken path, fontName is
    // unchanged because no font swap was emitted.
    EXPECT_NE(strip(after.fontName), strip(orig.fontName))
        << "Overlay font-family change did not propagate to the saved "
           "content stream. The toolbar's family selection is being lost "
           "before the embed/substitution step.";

    // D2 phase-2: strengthen the assertion. EXPECT_NE alone passes
    // even when after.fontName is empty (the empty string differs
    // from any non-empty original). Lock the correct-reason-for-
    // passing contract by also requiring a non-empty post-reopen
    // font name — the saved stream must carry SOME font reference,
    // not merely fail to reproduce the original one.
    EXPECT_FALSE(after.fontName.empty())
        << "Reopened span has an empty fontName — the family change "
           "produced a content stream with no usable font reference. "
           "EXPECT_NE above passed only because empty != original; "
           "this assertion locks that the swap actually emitted a font.";
}

// ─────────────────────────────────────────────────────────────────────
// UI-TE-SAVE-04 — bold toggle reaches the saved content stream.
// Asserts the reopened span has a bold-weighted font reference.
// ─────────────────────────────────────────────────────────────────────
TEST_F(TextEditSaveRoundtripTest, BoldToggleReachesContentStream) {
    TextSpan orig = originalSpan();
    ASSERT_FALSE(orig.text.empty());

    FormattingRun run;
    run.charStart = 0;
    run.charEnd = static_cast<int>(orig.text.size());
    run.fontFamily.clear();   // Keep family — only toggle bold
    run.bold = true;
    run.fontSize = orig.fontSize;

    TextSpan after = editSaveReopen(orig.text, {run});

    // We need a heuristic for "bold" survived: the font name must have
    // changed (a Bold variant of the original family) OR contain "Bold".
    auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c){ return std::tolower(c); });
        return s;
    };
    bool nameMentionsBold =
        lower(after.fontName).find("bold") != std::string::npos;
    bool nameDiffers = lower(after.fontName) != lower(orig.fontName);

    EXPECT_TRUE(nameMentionsBold || nameDiffers)
        << "Bold toggle did not reach the saved content stream. "
           "Original font: " << orig.fontName
        << ", after: " << after.fontName;
}

// ─────────────────────────────────────────────────────────────────────
// UI-TE-SAVE-05 (P1) — italic toggle round-trips through save without
// corrupting the span. Symmetric to UI-TE-SAVE-04 (bold), but with a
// looser font-name assertion: the test fixture's source is "Helvetica"
// without an italic variant on every system, so we lock the weaker
// invariant that the italic toggle does not silently corrupt the
// content stream — the fontName, if changed, may pick up an italic
// or oblique marker; if unchanged, the family name must be a sane
// resolution and the text must round-trip.
// ─────────────────────────────────────────────────────────────────────
TEST_F(TextEditSaveRoundtripTest, ItalicToggleRoundTripIsNonDestructive) {
    TextSpan orig = originalSpan();
    ASSERT_FALSE(orig.text.empty());

    FormattingRun run;
    run.charStart = 0;
    run.charEnd = static_cast<int>(orig.text.size());
    run.fontFamily.clear();
    run.italic = true;
    run.fontSize = orig.fontSize;

    TextSpan after = editSaveReopen(orig.text, {run});

    // Text must round-trip unchanged
    EXPECT_EQ(after.text, orig.text)
        << "Italic toggle corrupted the saved text content.";
    // Font size must round-trip within tolerance
    EXPECT_NEAR(after.fontSize, orig.fontSize, 1.0f)
        << "Italic toggle drifted font size.";
    // Font name must be non-empty (no font-resource loss)
    EXPECT_FALSE(after.fontName.empty())
        << "Italic toggle dropped the font reference entirely.";
}

// ─────────────────────────────────────────────────────────────────────
// UI-TE-SAVE-09 (P1) — span text fully replaced by the new value.
// (Audit row covers a similar invariant; lock the basic save → reopen
// text-equality contract independent of font changes.)
// ─────────────────────────────────────────────────────────────────────
TEST_F(TextEditSaveRoundtripTest, EditedTextEqualsInputAfterReopen) {
    TextSpan orig = originalSpan();
    std::u32string newText = U"REGRTOKEN_SAVE_09";

    TextSpan after = editSaveReopen(newText, /*overlayRuns=*/{});

    // The reopened span 0 might be re-ordered by extraction; assert the
    // token appears somewhere in the reopened page's plain text.
    auto reopened = Document::open(ctx_, tempOut_);
    auto rePage = reopened->page(0);
    TextExtractor extractor(ctx_);
    std::string plain = extractor.extractPlainText(rePage.get());
    EXPECT_NE(plain.find("REGRTOKEN_SAVE_09"), std::string::npos)
        << "Edited text not present in reopened page. plain="
        << plain.substr(0, 200);
}

// ─────────────────────────────────────────────────────────────────────
// UI-TE-SAVE-08 — multi-formatting-run span preserves per-run distinction.
// Resolved by SAVE-08 phases 1-4: data model (phase-1), per-run Tf/Tj
// emission (phase-2), caller migration (phase-3), and font-resource
// persistence through pdf_filter_page_contents + saveAs GC (phase-4).
// ─────────────────────────────────────────────────────────────────────
TEST_F(TextEditSaveRoundtripTest, MultiFormatRunsPreservedAcrossSave) {
#ifndef _WIN32
    GTEST_SKIP() << "Multi-format-run embed depends on SystemFontDiscovery "
                    "Linux impl";
#endif
    TextSpan orig = originalSpan();
    ASSERT_GE(orig.text.size(), 5u);

    int len = static_cast<int>(orig.text.size());

    // Two runs with distinct fontFamily values, mirroring the pattern
    // used by OverlayFontFamilyReachesContentStream (UI-TE-SAVE-02): the
    // applyText resolver fills in fontFilePath, the embedder adds a
    // proper ToUnicode CMap, and the reopened content stream carries
    // both fonts as distinct references.
    FormattingRun runA;
    runA.charStart = 0;
    runA.charEnd = len / 2;
    runA.fontSize = orig.fontSize;
    runA.fontFamily = "Arial";
    runA.bold = false;

    FormattingRun runB;
    runB.charStart = len / 2;
    runB.charEnd = len;
    runB.fontSize = orig.fontSize;
    runB.fontFamily = "Times New Roman";
    runB.bold = false;

    TextSpan after = editSaveReopen(orig.text, {runA, runB});

    // The reopened span's text matches input but the fontName for the
    // whole span is the dominant run's font in the broken path. To
    // detect per-run preservation, we'd need multi-span output (one per
    // run). We assert that re-extraction produces MORE THAN ONE span
    // for the originally single span (per-run split persisted).
    auto reopened = Document::open(ctx_, tempOut_);
    auto rePage = reopened->page(0);
    TextExtractor extractor(ctx_);
    auto blocks = extractor.extract(rePage.get());
    TextEditModel model(blocks);

    // Count distinct *non-empty* fontName transitions on the first
    // line of the reopened page. Empty fontName indicates an
    // unresolvable Tf reference (the font is missing from the page
    // Resources dict) — that bug existed pre-phase-4 and trivially
    // produced "different but empty" fontNames; rejecting empties here
    // makes the contract genuine.
    int spanCountOnReopenedLine = 0;
    if (!blocks.empty() && !blocks[0].lines.empty()) {
        std::string prev;
        for (const auto& ch : blocks[0].lines[0].chars) {
            if (ch.fontName != prev && !ch.fontName.empty()) {
                ++spanCountOnReopenedLine;
                prev = ch.fontName;
            }
        }
    }

    EXPECT_GE(spanCountOnReopenedLine, 2)
        << "Multi-formatting-run span collapsed to a single font on save. "
           "Per-run distinction must be preserved (one resolvable font "
           "reference per FormattingRun in the saved content stream).";
}
