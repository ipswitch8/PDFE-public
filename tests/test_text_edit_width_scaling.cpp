#include <gtest/gtest.h>
#include "textedit/TextEditCommands.h"
#include "textedit/TextEditModel.h"
#include "fonts/FontSubstitutionEngine.h"
#include "core/Document.h"
#include "core/Page.h"
#include "core/MuPdfContext.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>

using namespace pdfe::textedit;
using namespace pdfe::core;

class TextEditWidthScalingTest : public ::testing::Test {
protected:
    void SetUp() override {
        context = std::make_shared<MuPdfContext>();

        std::filesystem::path fixturesDir = TEST_FIXTURES_DIR;
        testPdf = fixturesDir / "test_3page.pdf";

        if (std::filesystem::exists(testPdf)) {
            document = Document::open(context, testPdf);

            if (document && document->pageCount() > 0) {
                auto page = document->page(0);
                if (page) {
                    auto blocks = page->extractText();
                    if (!blocks.empty()) {
                        model = std::make_unique<TextEditModel>(blocks);
                    }
                }
            }
        }
    }

    void TearDown() override {
        model.reset();
        document.reset();
        context.reset();
    }

    // A copy of span 0 carrying one whole-text formatting run (no font override),
    // so EditTextCommand::applyText routes through the per-run emission path in
    // ContentStreamEditor (the path that emits the horizontal-scale Tz operator).
    // The run uses the span's ORIGINAL font/size, so the saved glyphs keep their
    // natural advance widths — this is the span state a real toolbar-touched edit
    // produces, and the only one with an observable horizontal-scale knob.
    TextSpan spanWithWholeTextRun(const std::u32string& newText) {
        TextSpan s = model->span(0);
        FormattingRun run;
        run.charStart = 0;
        run.charEnd = static_cast<int>(newText.size());
        run.fontSize = s.fontSize;  // original size, no family/path override
        s.formattingRuns = { run };
        return s;
    }

    // Save the (already-edited) in-memory document to a temp file, reopen it in a
    // FRESH context, and return the per-character advance (bbox width / char
    // count) of the first re-extracted span whose text contains `marker`. This
    // reads the REAL saved content stream back through the normal extraction
    // path, so the measured advance reflects any horizontal text-scale (Tz) the
    // emission applied. Returns -1 when the marker is not found (edit dropped).
    double editedSpanAdvancePerChar(const std::u32string& marker) {
        auto outPath = std::filesystem::temp_directory_path() /
                       "pdfe_width_scale_probe.pdf";
        document->saveAs(outPath);
        auto ctx = std::make_shared<MuPdfContext>();
        auto d = Document::open(ctx, outPath);
        if (!d || d->pageCount() == 0) return -1.0;
        auto pg = d->page(0);
        if (!pg) return -1.0;
        auto blocks = pg->extractText();
        TextEditModel m(blocks);
        for (int i = 0; i < m.spanCount(); ++i) {
            const auto& s = m.span(i);
            if (!s.text.empty() &&
                s.text.find(marker) != std::u32string::npos) {
                const int n = static_cast<int>(s.text.size());
                return (n > 0) ? s.boundingBox.width() / n : -1.0;
            }
        }
        return -1.0;
    }

    // True when `marker` appears in ANY re-extracted span of the saved document
    // (used to assert an edit actually landed, rather than only that it threw no
    // exception).
    bool savedDocumentContains(const std::u32string& marker) {
        auto outPath = std::filesystem::temp_directory_path() /
                       "pdfe_width_scale_landed.pdf";
        document->saveAs(outPath);
        auto ctx = std::make_shared<MuPdfContext>();
        auto d = Document::open(ctx, outPath);
        if (!d || d->pageCount() == 0) return false;
        auto pg = d->page(0);
        if (!pg) return false;
        auto blocks = pg->extractText();
        TextEditModel m(blocks);
        for (int i = 0; i < m.spanCount(); ++i) {
            if (m.span(i).text.find(marker) != std::u32string::npos) return true;
        }
        return false;
    }

    std::shared_ptr<MuPdfContext> context;
    std::filesystem::path testPdf;
    std::unique_ptr<Document> document;
    std::unique_ptr<TextEditModel> model;
};

// GitHub #394 (weak-oracle-hardening, phase-2): these three were EXPECT_NO_THROW
// shells asserting no geometry — they passed whether the edit scaled the text,
// squashed it, or was silently dropped. They now drive the REAL EditTextCommand
// through save + reopen and read back the edited span's per-character advance
// (bbox width / char count). The product's deliberate contract (see
// TextEditCommands.cpp:381-396, where a length-based horizontal squash was
// REMOVED) is NATURAL advance widths: the edited text's per-char advance must
// match the original's REGARDLESS of length change — NOT be horizontally scaled
// to fit the old line width. The ratio is asserted within a tight band that a
// correct (unity) scale passes but a reintroduced Tz squash/stretch fails.
//
// Mutation proof (phase-2 closeout): forcing the injected hScale off 100 in
// ContentStreamEditor (e.g. to 50) reds LongerText/ShorterText/SameText here.

TEST_F(TextEditWidthScalingTest, LongerTextNoException) {
    if (!document || !model) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }
    ASSERT_GT(model->spanCount(), 0) << "Need at least one text span";

    const std::u32string oldText = model->span(0).text;
    ASSERT_GT(oldText.size(), 1u) << "fixture span 0 too short to compare advances";
    const double originalAdvance =
        model->span(0).boundingBox.width() / static_cast<double>(oldText.size());
    ASSERT_GT(originalAdvance, 0.0);

    // Append a distinctive, searchable suffix so the edited span is locatable.
    const std::u32string marker = U"WIDTHPROBE";
    const std::u32string newText = oldText + U" " + marker;

    TextSpan span = spanWithWholeTextRun(newText);
    EditTextCommand cmd(document.get(), context, 0, 0, span, oldText, newText);
    EXPECT_NO_THROW(cmd.execute()) << "longer-text edit threw";

    const double editedAdvance = editedSpanAdvancePerChar(marker);
    ASSERT_GT(editedAdvance, 0.0)
        << "edited (longer) text was not found in the saved document — edit dropped";

    // Natural advance: the longer text renders at the SAME per-char advance as the
    // original (no squash-to-fit). A reintroduced Tz squash would shrink it.
    const double ratio = editedAdvance / originalAdvance;
    EXPECT_GT(ratio, 0.7)
        << "edited text was horizontally SQUASHED (advance ratio " << ratio
        << ") — a length-based Tz scale leaked back in";
    EXPECT_LT(ratio, 1.4)
        << "edited text was horizontally STRETCHED (advance ratio " << ratio << ")";

    EXPECT_NO_THROW(cmd.undo());
    EXPECT_NO_THROW(cmd.redo());
}

TEST_F(TextEditWidthScalingTest, ShorterTextNoException) {
    if (!document || !model) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }
    ASSERT_GT(model->spanCount(), 0);

    const std::u32string oldText = model->span(0).text;
    // Fixture-shape PRECONDITION, asserted not skipped: test_3page.pdf's first
    // span must exceed 3 chars so the half-length prefix below is >=2 chars. A
    // committed fixture's span length is a fact to assert, not a magnitude to
    // gate a GTEST_SKIP on (that shape reintroduces the conditional-assertion
    // defect class the magnitude guard catches, #273) — so a fixture that ever
    // shrank would redden here loudly instead of silently skipping.
    ASSERT_GT(oldText.size(), 3u)
        << "fixture test_3page.pdf span(0) is too short (" << oldText.size()
        << " chars) to form a >=2-char shorter prefix";
    const double originalAdvance =
        model->span(0).boundingBox.width() / static_cast<double>(oldText.size());
    ASSERT_GT(originalAdvance, 0.0);

    // Keep a prefix (same char distribution as the original) so the natural
    // per-char advance is directly comparable, and use that prefix as the marker.
    const std::u32string newText = oldText.substr(0, oldText.size() / 2);
    ASSERT_GE(newText.size(), 2u);

    TextSpan span = spanWithWholeTextRun(newText);
    EditTextCommand cmd(document.get(), context, 0, 0, span, oldText, newText);
    EXPECT_NO_THROW(cmd.execute()) << "shorter-text edit threw";

    const double editedAdvance = editedSpanAdvancePerChar(newText);
    ASSERT_GT(editedAdvance, 0.0)
        << "edited (shorter) text was not found in the saved document — edit dropped";

    // Natural advance: shortening must NOT stretch the glyphs to re-fill the old
    // width (the old charRatio=oldLen/newLen bug stretched them ~2x here).
    const double ratio = editedAdvance / originalAdvance;
    EXPECT_GT(ratio, 0.7)
        << "edited (shorter) text per-char advance collapsed (ratio " << ratio << ")";
    EXPECT_LT(ratio, 1.4)
        << "edited (shorter) text was STRETCHED to re-fill the line (ratio "
        << ratio << ") — the removed oldLen/newLen Tz squash is back";

    EXPECT_NO_THROW(cmd.undo());
    EXPECT_NO_THROW(cmd.redo());
}

TEST_F(TextEditWidthScalingTest, SameTextNoScaling) {
    if (!document || !model) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }
    ASSERT_GT(model->spanCount(), 0);

    const std::u32string sameText = model->span(0).text;
    ASSERT_GT(sameText.size(), 1u);
    const double originalAdvance =
        model->span(0).boundingBox.width() / static_cast<double>(sameText.size());
    ASSERT_GT(originalAdvance, 0.0);

    TextSpan span = spanWithWholeTextRun(sameText);
    EditTextCommand cmd(document.get(), context, 0, 0, span, sameText, sameText);
    EXPECT_NO_THROW(cmd.execute()) << "same-text edit threw";

    const double editedAdvance = editedSpanAdvancePerChar(sameText);
    ASSERT_GT(editedAdvance, 0.0)
        << "re-emitted same text not found in the saved document";

    // Re-emitting identical text must not change the per-char advance at all
    // (no scaling applied) — a tight band around unity.
    const double ratio = editedAdvance / originalAdvance;
    EXPECT_NEAR(ratio, 1.0, 0.25)
        << "re-emitting identical text changed the per-char advance (ratio "
        << ratio << ") — scaling applied where none should be";

    EXPECT_NO_THROW(cmd.undo());
    EXPECT_NO_THROW(cmd.redo());
}

// GitHub #394 (weak-oracle-hardening, phase-2): this was a pure-math tautology —
// it recomputed `oldLen/newLen` in the test body and asserted the test's own
// arithmetic, calling NO product code, so it could never catch a scaling
// regression. It is replaced by a test that drives the REAL product function
// EditTextCommand uses to build its emission substitution,
// buildSubstitutionFromFormattingRuns, and asserts the horizontal-scale factor
// that function actually decides. The product's contract after the length-based
// squash was removed (TextEditCommands.cpp:381-396) is UNITY scale — editing
// never horizontally compresses or stretches text to fit the old line width,
// whatever the length change. The previous test's `originalWidth*oldLen/newLen`
// value is exactly the REMOVED squash; asserting it would pin the old bug.
//
// Mutation proof (phase-2 closeout): setting sub.horizontalScale to any
// length-ratio in buildSubstitutionFromFormattingRuns reds this test.
TEST_F(TextEditWidthScalingTest, CharRatioLogic) {
    // Build a span and a set of whole-text formatting runs the way the edit
    // pipeline does, then call the product's real substitution builder.
    TextSpan span;
    span.fontName = "F0";
    span.fontSize = 12.0f;
    span.text = U"HELLO";          // "old" text
    span.originalText = span.text;

    // Simulate an edit that DOUBLED the length (10 chars) — the historical
    // worst case for a squash-to-fit: a reintroduced oldLen/newLen Tz would be
    // 5/10 = 0.5 here. The run carries no font override, so the builder maps it
    // to the span's original font and keeps the scale at unity.
    std::vector<FormattingRun> runs;
    FormattingRun run;
    run.charStart = 0;
    run.charEnd = 10;              // "new" text length (doubled)
    run.fontSize = 12.0f;
    runs.push_back(run);

    std::unordered_map<std::string, std::string> pathToResName;  // no embedded fonts
    pdfe::fonts::SubstitutionInfo sub =
        buildSubstitutionFromFormattingRuns(runs, pathToResName, span);

    // THE contract: no horizontal squash/stretch from length change — exactly
    // 1.0, read from the product's own decision (not recomputed in the test).
    EXPECT_FLOAT_EQ(sub.horizontalScale, 1.0f)
        << "EditTextCommand's substitution builder applied a non-unity horizontal "
           "scale (" << sub.horizontalScale << ") — the removed length-based "
           "squash-to-fit has returned";

    // Sanity that the builder actually ran over the run (so the scale assertion
    // is about a populated substitution, not an empty one).
    ASSERT_EQ(sub.runSegments.size(), 1u);
    EXPECT_EQ(sub.runSegments[0].charStart, 0);
    EXPECT_EQ(sub.runSegments[0].charEnd, 10);
    EXPECT_FLOAT_EQ(sub.runSegments[0].fontSize, 12.0f);
    // No override on the run => it must fall back to the span's original font.
    EXPECT_EQ(sub.runSegments[0].fontResName, span.fontName);
}

TEST_F(TextEditWidthScalingTest, EmptyTextHandling) {
    if (!document || !model) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    ASSERT_GT(model->spanCount(), 0);

    // Get first span
    const auto& span = model->span(0);
    std::u32string oldText = span.text;

    // Test editing to empty text
    std::u32string emptyText = U"";

    // Strengthened (GitHub #394): assert the empty edit actually REMOVES the
    // text and that undo RESTORES it (round-trip through the real save path),
    // not merely that no exception was thrown.
    EditTextCommand cmd(document.get(), context, 0, 0, span, oldText, emptyText);
    EXPECT_NO_THROW(cmd.execute());
    EXPECT_FALSE(savedDocumentContains(oldText))
        << "empty-text edit did not remove the original span text";
    EXPECT_NO_THROW(cmd.undo());
    EXPECT_TRUE(savedDocumentContains(oldText))
        << "undo of the empty-text edit did not restore the original text";
    EXPECT_NO_THROW(cmd.redo());
}

TEST_F(TextEditWidthScalingTest, VeryLongTextHandling) {
    if (!document || !model) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    ASSERT_GT(model->spanCount(), 0);

    // Get first span
    const auto& span = model->span(0);
    std::u32string oldText = span.text;

    // Create very long text (1000+ characters)
    std::u32string veryLongText = oldText;
    for (int i = 0; i < 100; ++i) {
        veryLongText += U" REPEATED_TEXT_SEGMENT";
    }

    EXPECT_GT(veryLongText.size(), 1000u) << "Test text should be very long";

    // Strengthened (GitHub #394): the very-long edit must actually LAND (the
    // repeated marker appears in the saved output), not merely avoid throwing.
    EditTextCommand cmd(document.get(), context, 0, 0, span, oldText, veryLongText);
    EXPECT_NO_THROW(cmd.execute());
    EXPECT_TRUE(savedDocumentContains(U"REPEATED_TEXT_SEGMENT"))
        << "very-long edit was silently dropped (marker not found in saved doc)";
    EXPECT_NO_THROW(cmd.undo());
    EXPECT_NO_THROW(cmd.redo());
}

TEST_F(TextEditWidthScalingTest, SpecialCharactersHandling) {
    if (!document || !model) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    ASSERT_GT(model->spanCount(), 0);

    // Get first span
    const auto& span = model->span(0);
    std::u32string oldText = span.text;

    // Create text with special Unicode characters
    std::u32string specialText = U"Test™ with © symbols • and € currency 中文字符";

    // Strengthened (GitHub #394): the ASCII skeleton of the edit must survive to
    // the saved output (the substitution path must not drop the whole run to
    // tofu), not merely that no exception was thrown.
    EditTextCommand cmd(document.get(), context, 0, 0, span, oldText, specialText);
    EXPECT_NO_THROW(cmd.execute());
    EXPECT_TRUE(savedDocumentContains(U"Test"))
        << "special-character edit dropped its ASCII skeleton in the saved doc";
    EXPECT_NO_THROW(cmd.undo());
    EXPECT_NO_THROW(cmd.redo());
}
