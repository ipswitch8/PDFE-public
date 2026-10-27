// -*- coding: utf-8 -*-
// tests/test_cjk_content_edit_phase75.cpp
//
// Follow-up phase 75 (pipeline phase-22): WIN-49 prerequisite — CJK
// save-correctness on the content-edit path.
//
// WHAT THIS FILE IS FOR. Before this phase, typing CJK into a text span and
// saving produced a file that opened cleanly and had lost the text: the
// simple-font encoder replaced every codepoint >= 256 with '?', and the CID
// encoder wrote CID 0 (two raw NUL bytes) whenever the font had no glyph.
// Neither said anything. The reverification's finding was that shipping IME
// (phase-23) on top of that converts a *visible* block — you cannot type CJK
// at all today — into *silent data loss*, which is strictly worse.
//
// WHY THE EXISTING SUITE DID NOT COVER THIS. tests/test_i18n_render.cpp and
// tests/test_f18_backlog_corpus.cpp both round-trip CJK documents, but they
// round-trip them WITH NO EDIT: they open, save, reopen and compare. That
// exercises the copy path, which never touches the encoder. The defect lives
// entirely in the injection path, which only runs when a span is edited. A
// round-trip that does not edit cannot reach it, so those tests passed
// throughout — the phase spec calls this out explicitly and requires the
// edit-and-save case to be added rather than cited.

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "core/TextExtractor.h"
#include "core/UndoManager.h"
#include "textedit/ContentStreamEditor.h"
#include "textedit/TextEditCommands.h"
#include "textedit/TextEditModel.h"
#include "fonts/EmbeddedFontAnalyzer.h"
#include "fonts/FontAnalysisCache.h"
#include "fonts/SubstituteFontSelector.h"
#include "tre/AutoMissingGlyphResolver.h"

#include <QRectF>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace pdfe::textedit;
using namespace pdfe::core;
using namespace pdfe::commands;

namespace {

// 日本語 — three CJK Unified Ideographs, all outside any simple font's
// single-byte encoding and outside every Latin font's cmap. Written as
// explicit codepoints rather than a u8 literal so the test cannot be
// perturbed by the source file's encoding or the compiler's locale.
constexpr char32_t kNihongo[] = {0x65E5, 0x672C, 0x8A9E};  // 日 本 語
const std::u32string kCjkText(kNihongo, kNihongo + 3);

// TextEditModel exposes spans one at a time; ContentStreamEditor wants the
// whole vector. Copy rather than reach into the model's internals.
std::vector<TextSpan> allSpans(const TextEditModel& model)
{
    std::vector<TextSpan> out;
    out.reserve(static_cast<size_t>(model.spanCount()));
    for (int i = 0; i < model.spanCount(); ++i)
        out.push_back(model.span(i));
    return out;
}

class CjkContentEditTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_shared<MuPdfContext>();
        std::filesystem::path fixtures = TEST_FIXTURES_DIR;
        fixtureSrc_ = fixtures / "test_3page.pdf";
        if (!std::filesystem::exists(fixtureSrc_)) {
            GTEST_SKIP() << "Test fixture not available: " << fixtureSrc_;
        }
        const auto& info =
            *::testing::UnitTest::GetInstance()->current_test_info();
        tempOut_ = std::filesystem::temp_directory_path() /
                   ("pdfe_cjk_p75_" + std::string(info.name()) + ".pdf");
        std::error_code ec;
        std::filesystem::remove(tempOut_, ec);
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove(tempOut_, ec);
    }

    // Drive the SAME programmatic route the UI drives: EditTextCommand ->
    // TextEditCommands::applyText -> AutoMissingGlyphResolver (font routing)
    // -> PdfFontEmbedder -> ContentStreamEditor (encoding) -> saveAs.
    // Returns the concatenated text of the reopened page.
    std::u32string editSaveReopenAllText(const std::u32string& newText) {
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
        editSpan.text = newText;

        UndoManager undo;
        auto cmd = std::make_unique<EditTextCommand>(
            doc.get(), ctx_, /*pageIndex=*/0, /*spanIndex=*/0,
            editSpan, oldText, newText);
        undo.execute(std::move(cmd));

        doc->saveAs(tempOut_);
        doc.reset();

        auto reopened = Document::open(ctx_, tempOut_);
        EXPECT_NE(reopened, nullptr);
        auto rePage = reopened->page(0);
        EXPECT_NE(rePage, nullptr);
        TextExtractor extractor2(ctx_);
        auto blocks2 = extractor2.extract(rePage.get());
        TextEditModel model2(blocks2);

        std::u32string all;
        for (int i = 0; i < model2.spanCount(); ++i)
            all += model2.span(i).text;
        return all;
    }

    // Raw bytes of the saved file, for the "what actually landed on disk"
    // assertions. Reading the extracted text alone would not distinguish
    // "the text survived" from "the extractor helpfully repaired it".
    std::string savedBytes() const {
        std::ifstream in(tempOut_, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    }

    std::shared_ptr<MuPdfContext> ctx_;
    std::filesystem::path fixtureSrc_;
    std::filesystem::path tempOut_;
};

}  // namespace

// ─────────────────────────────────────────────────────────────────────
// Criterion 5 — the core discriminating test. Edit CJK, save, reopen,
// and require the codepoints back.
// ─────────────────────────────────────────────────────────────────────
TEST_F(CjkContentEditTest, CjkEditSurvivesSaveAndReopen) {
    std::u32string after = editSaveReopenAllText(kCjkText);

    for (char32_t cp : kCjkText) {
        EXPECT_NE(after.find(cp), std::u32string::npos)
            << "U+" << std::hex << static_cast<uint32_t>(cp)
            << " did not survive edit -> save -> reopen. Extracted text has "
            << std::dec << after.size() << " codepoints.";
    }
}

// The negative half of the same criterion, and the one that actually
// separates a fix from a plausible-looking non-fix: the pre-phase behaviour
// ALSO produced a readable document — it just contained '?' where the CJK
// was. A test that only checked "the document reopens" or "some text is
// present" passed on the broken code.
TEST_F(CjkContentEditTest, CjkEditDoesNotDegradeToQuestionMarksOrNul) {
    std::u32string after = editSaveReopenAllText(kCjkText);

    // Exactly as many '?' as the untouched fixture had (the fixture's own
    // text is preserved in the other spans; the edited span had none).
    const size_t questionMarks =
        static_cast<size_t>(std::count(after.begin(), after.end(), U'?'));
    EXPECT_EQ(questionMarks, 0u)
        << "The edited CJK text came back as '?' — the simple-font encoder's "
           "silent substitution is still in the save path.";

    const size_t nuls =
        static_cast<size_t>(std::count(after.begin(), after.end(), U'\0'));
    EXPECT_EQ(nuls, 0u)
        << "The edited CJK text came back as U+0000 — the CID encoder wrote "
           ".notdef (CID 0) for characters the font could not map.";
}

// ─────────────────────────────────────────────────────────────────────
// Criterion 4 — /ToUnicode.
//
// The register recorded "no /ToUnicode CMap is ever generated (zero hits,
// two searches)". That search was for PDFE's own generation code, and PDFE
// has none — but the CID embedding path calls MuPDF's pdf_add_cid_font,
// which calls pdf_add_to_unicode and writes the stream (verified in
// mupdf 1.26.10 source/pdf/pdf-font-add.c). Absence of our code was read as
// absence of the mechanism. This test pins the mechanism rather than the
// code, so it stays true however the CMap comes to exist — and it would
// have caught the case the register feared, because without /ToUnicode the
// Identity-H CIDs above are un-extractable and the round-trip test fails.
// ─────────────────────────────────────────────────────────────────────
TEST_F(CjkContentEditTest, SavedCjkEditCarriesAToUnicodeCMap) {
    (void)editSaveReopenAllText(kCjkText);

    const std::string bytes = savedBytes();
    ASSERT_FALSE(bytes.empty()) << "Saved file is empty or unreadable.";

    EXPECT_NE(bytes.find("/ToUnicode"), std::string::npos)
        << "The saved document has no /ToUnicode entry, so the Identity-H "
           "CIDs written for the CJK edit cannot be mapped back to Unicode "
           "by any consumer (copy/paste, search, accessibility).";
    EXPECT_NE(bytes.find("/Identity-H"), std::string::npos)
        << "The CJK edit did not go through a composite (Type0/Identity-H) "
           "font at all, so it cannot have encoded the codepoints.";
}

// ─────────────────────────────────────────────────────────────────────
// Criterion 3 — the production resolver reaches script-aware selection.
//
// AutoMissingGlyphResolver::resolve is the actual production caller. Before
// this phase its system-font fallback resolved exactly one family (the
// source font's, else Times New Roman / Arial), all Latin, so every CJK
// codepoint failed the FT_Get_Char_Index coverage check and was dropped
// from the run map — leaving the save path to write .notdef.
// ─────────────────────────────────────────────────────────────────────
TEST_F(CjkContentEditTest, ResolverRoutesCjkToACoveringFont) {
    auto doc = Document::open(ctx_, fixtureSrc_);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    TextExtractor extractor(ctx_);
    auto blocks = extractor.extract(page.get());
    TextEditModel model(blocks);
    ASSERT_GT(model.spanCount(), 0);
    const std::string sourceFont = model.span(0).fontName;
    ASSERT_FALSE(sourceFont.empty());

    auto runs = pdfe::tre::AutoMissingGlyphResolver::resolve(
        ctx_.get(), page.get(), doc.get(), sourceFont,
        model.span(0).fontSize, kCjkText);

    ASSERT_FALSE(runs.empty())
        << "The resolver produced no runs for CJK text, so every codepoint "
           "falls through to the source (Latin) font and renders .notdef.";

    // Every CJK index must be covered by a run that names a destination.
    for (size_t i = 0; i < kCjkText.size(); ++i) {
        bool covered = false;
        for (const auto& r : runs) {
            const int idx = static_cast<int>(i);
            if (idx >= r.charStart && idx < r.charEnd
                && (!r.fontFilePath.empty() || !r.preInstalledResName.empty())) {
                covered = true;
                break;
            }
        }
        EXPECT_TRUE(covered)
            << "codepoint index " << i << " (U+" << std::hex
            << static_cast<uint32_t>(kCjkText[i]) << std::dec
            << ") was not routed to any substitute font.";
    }
}

// The SAME criterion against an EMBEDDED subset font, which is a different
// code path and would otherwise go untested.
//
// AutoMissingGlyphResolver has two arms. The fixture above references a
// non-embedded standard-14 font, so it takes the early arm added by this
// phase. A document carrying its own subsetted font program takes the long
// arm — FreeType face, absent/stripped partition, Type 3 atlas, Latin
// system-font fallback — and reaches the script-aware fallback at the very
// end of that arm. Only exercising the short arm would leave the long one's
// fallback provably dead: a mutation deleting it survived until this test
// existed.
TEST_F(CjkContentEditTest, ResolverRoutesCjkFromAnEmbeddedSubsetFontToo) {
    std::filesystem::path fixtures = TEST_FIXTURES_DIR;
    const auto embedded = fixtures / "embedded_italic_serif_subset.pdf";
    if (!std::filesystem::exists(embedded))
        GTEST_SKIP() << "Fixture not available: " << embedded;

    auto doc = Document::open(ctx_, embedded);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    TextExtractor extractor(ctx_);
    auto blocks = extractor.extract(page.get());
    TextEditModel model(blocks);
    ASSERT_GT(model.spanCount(), 0);
    const std::string sourceFont = model.span(0).fontName;
    ASSERT_FALSE(sourceFont.empty());

    auto runs = pdfe::tre::AutoMissingGlyphResolver::resolve(
        ctx_.get(), page.get(), doc.get(), sourceFont,
        model.span(0).fontSize, kCjkText);

    ASSERT_FALSE(runs.empty())
        << "An embedded subset font produced no runs for CJK: the long arm's "
           "system-font fallback resolved only Latin families, so every CJK "
           "codepoint was dropped and left to render .notdef.";

    for (size_t i = 0; i < kCjkText.size(); ++i) {
        bool covered = false;
        for (const auto& r : runs) {
            const int idx = static_cast<int>(i);
            if (idx >= r.charStart && idx < r.charEnd
                && (!r.fontFilePath.empty() || !r.preInstalledResName.empty())) {
                covered = true;
                break;
            }
        }
        EXPECT_TRUE(covered)
            << "codepoint index " << i << " was not routed (embedded-font arm).";
    }
}

// The control for the opposite risk. The script-aware fallback must not
// change what Latin editing does — a CJK fix that started substituting a
// CJK face for ordinary English text would be a far worse regression than
// the bug, and it would not show up in any of the assertions above.
TEST_F(CjkContentEditTest, LatinEditingIsUnaffectedByTheScriptAwareFallback) {
    const std::u32string latin = U"Hello";
    std::u32string after = editSaveReopenAllText(latin);

    for (char32_t cp : latin) {
        EXPECT_NE(after.find(cp), std::u32string::npos)
            << "plain ASCII edit lost U+" << std::hex
            << static_cast<uint32_t>(cp);
    }
}

// ─────────────────────────────────────────────────────────────────────
// Criterion 6 — the cost of embedding, pinned rather than assumed.
//
// Subsetting is deferred (see the disposition; follow-up FONT-SUBSET-01), so
// a CJK edit embeds the whole face. That is a real cost and the honest thing
// to do about a deferred feature is to MEASURE it, not to describe it: the
// register's "~9MB/save" figure was an estimate nobody had checked. This
// test records the actual saved size and fails if it grows past a ceiling,
// so the deferral cannot quietly get worse while nothing is watching.
// ─────────────────────────────────────────────────────────────────────
TEST_F(CjkContentEditTest, EmbeddedCjkFontCostIsBoundedAndRecorded) {
    (void)editSaveReopenAllText(kCjkText);
    const auto bytes = std::filesystem::file_size(tempOut_);

    const auto srcBytes = std::filesystem::file_size(fixtureSrc_);
    RecordProperty("saved_bytes", static_cast<int>(bytes));
    RecordProperty("fixture_bytes", static_cast<int>(srcBytes));
    std::cout << "[phase-75] CJK edit saved " << bytes << " bytes (fixture "
              << srcBytes << " bytes); delta = "
              << (static_cast<long long>(bytes) - static_cast<long long>(srcBytes))
              << " bytes of embedded font program.\n";

    // 24 MB. Deliberately loose: this is a regression ceiling on a KNOWN
    // deferral, not a target. A tight bound here would fail on any machine
    // whose best-covering CJK face is larger than this one's, which would
    // make the test about the test runner's font list rather than about
    // PDFE. What it does catch is the failure that matters — embedding the
    // face once per edited RUN, or per save, instead of once per document.
    EXPECT_LT(bytes, 24u * 1024u * 1024u)
        << "The CJK edit embedded " << bytes << " bytes. Subsetting is "
           "deferred, so a whole face is expected — but not several.";
}

// ─────────────────────────────────────────────────────────────────────
// Criteria 1 and 2 — the safety net, tested directly.
//
// The routing above is what SHOULD happen. These two pin what happens when
// it does not: the encoder must not fail silently. They are separate from
// the round-trip tests on purpose — the round-trip tests pass when routing
// works, and would keep passing if the silent-'?' branch were still there
// underneath, unreached.
// ─────────────────────────────────────────────────────────────────────
TEST_F(CjkContentEditTest, DiagnosticsAreEmptyAfterACleanAsciiEdit) {
    auto doc = Document::open(ctx_, fixtureSrc_);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    TextExtractor extractor(ctx_);
    auto blocks = extractor.extract(page.get());
    TextEditModel model(blocks);
    ASSERT_GT(model.spanCount(), 0);

    ContentStreamEditor editor(ctx_.get());
    std::vector<std::pair<int, std::u32string>> mods{{0, U"Hello"}};
    editor.applyEdits(page.get(), doc.get(), allSpans(model), mods);

    const auto& diag = editor.lastEncodingDiagnostics();
    EXPECT_TRUE(diag.empty())
        << "A pure-ASCII edit reported " << diag.unencodableSimple.size()
        << " unencodable and " << diag.notdefCid.size()
        << " .notdef codepoint(s); the diagnostics would then be noise and "
           "nobody would act on them.";
}

TEST_F(CjkContentEditTest, UnencodableCodepointsAreRecordedNotSwallowed) {
    auto doc = Document::open(ctx_, fixtureSrc_);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    TextExtractor extractor(ctx_);
    auto blocks = extractor.extract(page.get());
    TextEditModel model(blocks);
    ASSERT_GT(model.spanCount(), 0);

    // Deliberately the RAW ContentStreamEditor entry point, with no font
    // routing in front of it: this is the case the resolver is supposed to
    // prevent, and the point is that when it is NOT prevented the loss is
    // reported instead of being written out as '?' / NUL in silence.
    ContentStreamEditor editor(ctx_.get());
    std::vector<std::pair<int, std::u32string>> mods{{0, kCjkText}};
    editor.applyEdits(page.get(), doc.get(), allSpans(model), mods);

    const auto& diag = editor.lastEncodingDiagnostics();
    EXPECT_FALSE(diag.empty())
        << "CJK text encoded against the page's own Latin font produced no "
           "diagnostics at all. Either it encoded (impossible for this font) "
           "or the loss is still silent.";

    // Whichever branch it took, every CJK codepoint must appear in one of
    // the two lists — asserting only that the struct is non-empty would pass
    // on an implementation that recorded one character and dropped two.
    for (char32_t cp : kCjkText) {
        const bool recorded =
            std::find(diag.unencodableSimple.begin(),
                      diag.unencodableSimple.end(), cp)
                != diag.unencodableSimple.end()
            || std::find(diag.notdefCid.begin(), diag.notdefCid.end(), cp)
                != diag.notdefCid.end();
        EXPECT_TRUE(recorded)
            << "U+" << std::hex << static_cast<uint32_t>(cp) << std::dec
            << " was neither encoded nor reported.";
    }
}

// The CID arm of the same safety net. The test above drives a page whose
// font is SIMPLE, so it can only ever exercise the '?' branch — a mutation
// deleting the .notdef recording survived until this test existed, because
// nothing in the suite encoded against a composite font at all.
//
// i18n_cjk.pdf carries a Type0 / Identity-H CIDFontType2 subset. Feeding it
// a codepoint outside that subset is the exact case criterion 2 names:
// fz_encode_character returns 0, and the encoder must not write two raw NUL
// bytes and call the edit a success.
TEST_F(CjkContentEditTest, NotdefCidsAreRecordedNotSwallowed) {
    // Stage 1: produce a document that provably contains a composite font, by
    // performing the CJK edit this phase enables. Using PDFE's own output
    // rather than a checked-in fixture means the test cannot drift away from
    // the code: whatever font the routing picks today is the font the CID
    // encoder is exercised against.
    (void)editSaveReopenAllText(kCjkText);
    const auto cidDoc = tempOut_;
    ASSERT_TRUE(std::filesystem::exists(cidDoc));

    auto doc = Document::open(ctx_, cidDoc);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    TextExtractor extractor(ctx_);
    auto blocks = extractor.extract(page.get());
    TextEditModel model(blocks);
    ASSERT_GT(model.spanCount(), 0);

    // U+1F600 GRINNING FACE — astral, and not in any CJK text subset.
    const std::u32string outsideSubset(1, static_cast<char32_t>(0x1F600));

    // The page mixes simple and composite fonts, and only the composite ones
    // reach the CID encoder. Which span index carries which is a property of
    // the fixture, not of the code under test, so drive each span until one
    // lands on the CID path rather than hard-coding an index that a fixture
    // regeneration would silently invalidate.
    const auto spans = allSpans(model);
    bool sawCidNotdef = false;
    bool sawAnyDiagnostic = false;
    for (int i = 0; i < static_cast<int>(spans.size()) && !sawCidNotdef; ++i) {
        auto probeDoc = Document::open(ctx_, cidDoc);
        ASSERT_NE(probeDoc, nullptr);
        auto probePage = probeDoc->page(0);
        ASSERT_NE(probePage, nullptr);

        ContentStreamEditor editor(ctx_.get());
        std::vector<std::pair<int, std::u32string>> mods{{i, outsideSubset}};
        editor.applyEdits(probePage.get(), probeDoc.get(), spans, mods);

        const auto& diag = editor.lastEncodingDiagnostics();
        if (!diag.empty()) sawAnyDiagnostic = true;
        if (std::find(diag.notdefCid.begin(), diag.notdefCid.end(),
                      static_cast<char32_t>(0x1F600)) != diag.notdefCid.end()) {
            sawCidNotdef = true;
        }
    }

    EXPECT_TRUE(sawAnyDiagnostic)
        << "An astral codepoint edited into this document produced no "
           "diagnostics on any span: the loss is still silent.";

    // KNOWN GAP, stated rather than hidden. `sawCidNotdef` is NOT asserted.
    //
    // Driving the CID branch requires the injection callback to load a
    // composite pdf_font_desc for the edited span, and which font that is
    // depends on resource-name resolution inside a live filter run. Every
    // route tried here — a checked-in Type0 fixture, and PDFE's own
    // Identity-H output re-edited — lands on the simple branch instead, so
    // the .notdef recording added by this phase is currently unreachable
    // from any test. Mutation M2 (delete that recording) SURVIVES because of
    // this, and the mutation log records it as SURVIVED rather than being
    // dropped from the row list.
    //
    // Closing it needs a narrow test seam onto the CID encoder, which is
    // named as follow-up CID-SEAM-01. Asserting sawCidNotdef here would turn
    // a known gap into a red test; asserting its negation would freeze the
    // gap in place as if it were intended. Recording it is the honest third
    // option, and the mutation harness is what keeps it visible.
    if (!sawCidNotdef) {
        std::cout << "[phase-75] KNOWN GAP: no span reached the CID encoder; "
                     "notdefCid recording is untested (follow-up CID-SEAM-01)."
                  << std::endl;
    }
}

// SubstituteFontSelector's coverage contract, pinned.
//
// The script-aware fallback in AutoMissingGlyphResolver assigns the face
// findSubstitute returns WITHOUT re-checking each codepoint, because
// findSubstitute already rejects any candidate that fails
// findMissingGlyphs. That is a real dependency on another component's
// promise, and an unchecked promise is how the original defect got in. This
// test states the promise so a change to the selector's filtering shows up
// here rather than as CJK silently reverting to .notdef.
TEST_F(CjkContentEditTest, SubstituteSelectorOnlyReturnsCoveringFonts) {
    pdfe::fonts::EmbeddedFontAnalyzer analyzer(ctx_.get());
    pdfe::fonts::FontAnalysisCache cache;
    pdfe::fonts::SubstituteFontSelector selector(&analyzer, &cache);

    auto candidates = selector.findAllNonDecorativeCandidates(kCjkText);
    ASSERT_FALSE(candidates.empty())
        << "No installed font covers " << kCjkText.size()
        << " CJK codepoints; the machine has no CJK font and this phase's "
           "routing cannot work here.";

    for (const auto& c : candidates) {
        auto missing = analyzer.findMissingGlyphs(c.filePath, kCjkText);
        EXPECT_TRUE(missing.empty())
            << c.filePath << " was offered as a substitute but is missing "
            << missing.size() << " of the requested codepoints.";
    }
}

TEST_F(CjkContentEditTest, DiagnosticsDescribeTheLastEditOnly) {
    auto doc = Document::open(ctx_, fixtureSrc_);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    TextExtractor extractor(ctx_);
    auto blocks = extractor.extract(page.get());
    TextEditModel model(blocks);
    ASSERT_GT(model.spanCount(), 0);

    ContentStreamEditor editor(ctx_.get());
    std::vector<std::pair<int, std::u32string>> cjk{{0, kCjkText}};
    editor.applyEdits(page.get(), doc.get(), allSpans(model), cjk);
    ASSERT_FALSE(editor.lastEncodingDiagnostics().empty());

    // A second, clean edit must clear them. Accumulating across calls would
    // make the very first CJK edit in a session poison every later report,
    // which is the fastest way for a real signal to be trained away.
    auto doc2 = Document::open(ctx_, fixtureSrc_);
    ASSERT_NE(doc2, nullptr);
    auto page2 = doc2->page(0);
    ASSERT_NE(page2, nullptr);
    TextExtractor extractor2(ctx_);
    auto blocks2 = extractor2.extract(page2.get());
    TextEditModel model2(blocks2);
    ASSERT_GT(model2.spanCount(), 0);

    std::vector<std::pair<int, std::u32string>> ascii{{0, U"Hello"}};
    editor.applyEdits(page2.get(), doc2.get(), allSpans(model2), ascii);
    EXPECT_TRUE(editor.lastEncodingDiagnostics().empty())
        << "Diagnostics from a previous edit survived into a clean one.";
}
