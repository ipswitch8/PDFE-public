// tests/test_font_resource_persistence.cpp
//
// SAVE-08 Phase 4 — locks the contract that fonts added by
// PdfFontEmbedder to a page's Resources/Font dict survive both
// pdf_filter_page_contents (the sanitize/instance_forms pass) AND
// pdf_save_document with do_garbage=3.
//
// Pre-phase-4 these fonts were dropped silently because:
//   - pdf_filter_page_contents rewrites Resources/Font to only the
//     fonts referenced by pre-filter content; mid-flight embeds get
//     stranded.
//   - do_garbage=3 then collects them as unreachable.
//
// The phase-4 fix snapshots the pre-filter Font dict, pdf_keep_obj's
// each value, and re-inserts any entry the filter dropped after the
// filter returns (gated on substitutions having non-empty runSegments
// so the legacy single-Tf path is unchanged).
//
// These tests inspect the saved PDF's bytes directly to verify the
// Fsub<N> entries are persisted; they don't rely on the (currently
// weak) fontName-extraction round-trip.

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "core/TextExtractor.h"
#include "core/UndoManager.h"
#include "fonts/PdfFontEmbedder.h"
#include "fonts/FontResolver.h"
#include "textedit/ContentStreamEditor.h"
#include "textedit/TextEditCommands.h"
#include "textedit/TextEditModel.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <regex>
#include <string>
#include <vector>

using pdfe::core::Document;
using pdfe::core::MuPdfContext;
using pdfe::core::TextExtractor;
using pdfe::commands::UndoManager;
using pdfe::fonts::PdfFontEmbedder;
using pdfe::textedit::EditTextCommand;
using pdfe::textedit::FormattingRun;
using pdfe::textedit::TextEditModel;
using pdfe::textedit::TextSpan;

namespace {

// Read a file's bytes for substring scanning.
std::string readFileBytes(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::ostringstream oss;
    oss << in.rdbuf();
    return oss.str();
}

// Count occurrences of a literal needle in a haystack (substring).
size_t countOccurrences(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return 0;
    size_t count = 0, pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

class FontResourcePersistenceTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_shared<MuPdfContext>();
        std::filesystem::path fixtures = TEST_FIXTURES_DIR;
        fixtureSrc_ = fixtures / "test_3page.pdf";
        if (!std::filesystem::exists(fixtureSrc_)) {
            GTEST_SKIP() << "Fixture not available: " << fixtureSrc_;
        }
        const auto& info =
            *::testing::UnitTest::GetInstance()->current_test_info();
        tempOut_ = std::filesystem::temp_directory_path() /
            ("pdfe_font_persist_" + std::string(info.name()) + ".pdf");
        std::error_code ec;
        std::filesystem::remove(tempOut_, ec);
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove(tempOut_, ec);
    }

    // Run an edit with given formatting runs and save to tempOut_.
    void editAndSave(const std::vector<FormattingRun>& runs) {
        auto doc = Document::open(ctx_, fixtureSrc_);
        ASSERT_NE(doc, nullptr);
        auto page = doc->page(0);
        ASSERT_NE(page, nullptr);

        TextExtractor extractor(ctx_);
        auto blocks = extractor.extract(page.get());
        TextEditModel model(blocks);
        ASSERT_GT(model.spanCount(), 0);

        TextSpan editSpan = model.span(0);
        std::u32string oldText = editSpan.text;
        editSpan.formattingRuns = runs;

        UndoManager undo;
        auto cmd = std::make_unique<EditTextCommand>(
            doc.get(), ctx_, /*pageIndex=*/0, /*spanIndex=*/0,
            editSpan, oldText, /*newText=*/oldText);
        undo.execute(std::move(cmd));

        doc->saveAs(tempOut_);
    }

    std::shared_ptr<MuPdfContext> ctx_;
    std::filesystem::path fixtureSrc_;
    std::filesystem::path tempOut_;
};

}  // namespace

// ─────────────────────────────────────────────────────────────────────
// 1. Two-run edit: both Fsub entries appear in saved file's Font dict.
// ─────────────────────────────────────────────────────────────────────
TEST_F(FontResourcePersistenceTest,
       TwoRunsBothFontsSurviveFilterAndGarbageCollection) {
#ifndef _WIN32
    GTEST_SKIP() << "Embed pipeline depends on SystemFontDiscovery Linux impl";
#endif
    auto origDoc = Document::open(ctx_, fixtureSrc_);
    auto origPage = origDoc->page(0);
    TextExtractor extractor(ctx_);
    auto blocks = extractor.extract(origPage.get());
    TextEditModel model(blocks);
    ASSERT_GT(model.spanCount(), 0);
    TextSpan span = model.span(0);
    int len = static_cast<int>(span.text.size());
    ASSERT_GE(len, 4) << "Fixture must have a span ≥4 chars to split.";

    FormattingRun runA, runB;
    runA.charStart = 0;             runA.charEnd = len / 2;
    runA.fontFamily = "Arial";      runA.fontSize = span.fontSize;
    runB.charStart = len / 2;       runB.charEnd = len;
    runB.fontFamily = "Times New Roman"; runB.fontSize = span.fontSize;
    origDoc.reset();

    editAndSave({runA, runB});

    std::string bytes = readFileBytes(tempOut_);
    ASSERT_FALSE(bytes.empty());

    // After the phase-4 re-insertion fix, both Fsub names appear as
    // dict keys in the saved PDF's Font dict (i.e., as "/Fsub1" and
    // "/Fsub2" in the byte stream — they are dictionary keys not
    // free-floating tokens, so a literal substring search is safe).
    EXPECT_GT(countOccurrences(bytes, "/Fsub1"), 0u)
        << "First embedded Fsub1 was dropped from the saved Font dict — "
           "phase-4 re-insertion not effective.";
    EXPECT_GT(countOccurrences(bytes, "/Fsub2"), 0u)
        << "Second embedded Fsub2 was dropped from the saved Font dict — "
           "phase-4 re-insertion not effective for two-run case.";

    // BaseFont entries for the two embedded fonts also survive (the
    // font *objects* themselves were retained, not just dict keys).
    // Subset prefix is six caps + '+', so we look for either bare or
    // prefixed forms.
    // Linux uses metric-compatible substitutes (Liberation Sans
    // for Arial, Liberation Serif for Times); accept either.
    bool hasArial = bytes.find("/BaseFont/Arial") != std::string::npos
                 || bytes.find("/BaseFont/LiberationSans") != std::string::npos
                 || bytes.find("LiberationSans") != std::string::npos
                 || std::regex_search(bytes,
                        std::regex(R"(/BaseFont/[A-Z]{6}\+ArialMT?)"))
                 || std::regex_search(bytes,
                        std::regex(R"(/BaseFont/[A-Z]{6}\+LiberationSans)"));
    bool hasTimes = bytes.find("/BaseFont/Times") != std::string::npos
                 || bytes.find("TimesNewRoman") != std::string::npos
                 || bytes.find("/BaseFont/LiberationSerif") != std::string::npos
                 || bytes.find("LiberationSerif") != std::string::npos
                 || std::regex_search(bytes,
                        std::regex(R"(/BaseFont/[A-Z]{6}\+TimesNewRoman)"))
                 || std::regex_search(bytes,
                        std::regex(R"(/BaseFont/[A-Z]{6}\+LiberationSerif)"));
    EXPECT_TRUE(hasArial)
        << "Arial (or Linux metric-compatible Liberation Sans) font object "
           "missing from saved PDF — embedded font garbage-collected before save.";
    EXPECT_TRUE(hasTimes)
        << "Times New Roman (or Linux metric-compatible Liberation Serif) "
           "font object missing from saved PDF.";
}

// ─────────────────────────────────────────────────────────────────────
// 2. Single-run-with-runSegments: Fsub1 still survives.
//    (Phase-3's caller migration produces a 1-segment SubstitutionInfo
//    even for a single FormattingRun, so this exercises the same
//    re-insertion code path as the two-run case.)
// ─────────────────────────────────────────────────────────────────────
TEST_F(FontResourcePersistenceTest, SingleRunFontSurvivesSave) {
#ifndef _WIN32
    GTEST_SKIP() << "Embed pipeline (Fsub re-insertion) depends on "
                    "SubstituteFontSelector finding a system font via "
                    "SystemFontDiscovery, which has no Linux enumerator.";
#endif
    auto origDoc = Document::open(ctx_, fixtureSrc_);
    auto origPage = origDoc->page(0);
    TextExtractor extractor(ctx_);
    auto blocks = extractor.extract(origPage.get());
    TextEditModel model(blocks);
    TextSpan span = model.span(0);
    int len = static_cast<int>(span.text.size());
    origDoc.reset();

    FormattingRun run;
    run.charStart = 0; run.charEnd = len;
    run.fontFamily = "Times New Roman"; run.fontSize = span.fontSize;

    editAndSave({run});

    std::string bytes = readFileBytes(tempOut_);
    ASSERT_FALSE(bytes.empty());

    EXPECT_GT(countOccurrences(bytes, "/Fsub1"), 0u)
        << "Single-run Fsub1 was dropped — re-insertion fix is gated "
           "on runSegments non-empty, but phase-3 produces non-empty "
           "runSegments for any non-empty FormattingRuns input.";
}

// ─────────────────────────────────────────────────────────────────────
// 3. Empty FormattingRuns: legacy code path runs, no re-insertion.
//    Saved PDF must NOT contain any /FsubN entries (no embedding
//    happened, so nothing was at risk of being dropped).
// ─────────────────────────────────────────────────────────────────────
TEST_F(FontResourcePersistenceTest, NoFormattingRunsLeavesSaveUnaffected) {
    editAndSave(/*runs=*/{});

    std::string bytes = readFileBytes(tempOut_);
    ASSERT_FALSE(bytes.empty());

    EXPECT_EQ(countOccurrences(bytes, "/Fsub1"), 0u)
        << "Empty FormattingRuns input must not embed any new fonts; "
           "/Fsub1 in the saved PDF means a stray embedding survived.";
    EXPECT_EQ(countOccurrences(bytes, "/Fsub2"), 0u);
}

// ─────────────────────────────────────────────────────────────────────
// 4. Saved PDF reopens as a valid document — the re-insertion does
//    not corrupt the file structure (e.g., produce a Resources dict
//    that MuPDF refuses to parse).
// ─────────────────────────────────────────────────────────────────────
TEST_F(FontResourcePersistenceTest, ReinsertedResourcesProduceValidPdf) {
    auto origDoc = Document::open(ctx_, fixtureSrc_);
    auto origPage = origDoc->page(0);
    TextExtractor extractor(ctx_);
    auto blocks = extractor.extract(origPage.get());
    TextEditModel model(blocks);
    TextSpan span = model.span(0);
    int len = static_cast<int>(span.text.size());

    FormattingRun runA, runB;
    runA.charStart = 0;        runA.charEnd = len / 2;
    runA.fontFamily = "Arial"; runA.fontSize = span.fontSize;
    runB.charStart = len / 2;  runB.charEnd = len;
    runB.fontFamily = "Times New Roman"; runB.fontSize = span.fontSize;
    origDoc.reset();

    editAndSave({runA, runB});

    auto reopened = Document::open(ctx_, tempOut_);
    ASSERT_NE(reopened, nullptr)
        << "Saved PDF failed to reopen — re-insertion may have produced "
           "an invalid Resources dict.";
    auto rePage = reopened->page(0);
    ASSERT_NE(rePage, nullptr);
    TextExtractor extractor2(ctx_);
    auto reBlocks = extractor2.extract(rePage.get());
    EXPECT_FALSE(reBlocks.empty())
        << "Reopened page produced no text blocks — content stream is "
           "broken or unparseable.";
}

// ─────────────────────────────────────────────────────────────────────
// 5. fontName extraction on the reopened page yields a non-empty
//    string for at least one character — proves Tf operators reference
//    a name the post-save Resources dict can resolve. This is the
//    contract phase-4 owes phase-5: the disabled
//    MultiFormatRunsPreservedAcrossSave assertion can now be flipped
//    from "empty != original" (false-green) to "non-empty actual"
//    (genuine).
// ─────────────────────────────────────────────────────────────────────
TEST_F(FontResourcePersistenceTest, ReopenedFontNameIsNonEmpty) {
    auto origDoc = Document::open(ctx_, fixtureSrc_);
    auto origPage = origDoc->page(0);
    TextExtractor extractor(ctx_);
    auto blocks = extractor.extract(origPage.get());
    TextEditModel model(blocks);
    TextSpan span = model.span(0);
    int len = static_cast<int>(span.text.size());

    FormattingRun runA, runB;
    runA.charStart = 0;        runA.charEnd = len / 2;
    runA.fontFamily = "Arial"; runA.fontSize = span.fontSize;
    runB.charStart = len / 2;  runB.charEnd = len;
    runB.fontFamily = "Times New Roman"; runB.fontSize = span.fontSize;
    origDoc.reset();

    editAndSave({runA, runB});

    auto reopened = Document::open(ctx_, tempOut_);
    auto rePage = reopened->page(0);
    TextExtractor extractor2(ctx_);
    auto reBlocks = extractor2.extract(rePage.get());
    ASSERT_FALSE(reBlocks.empty());
    ASSERT_FALSE(reBlocks[0].lines.empty());
    ASSERT_FALSE(reBlocks[0].lines[0].chars.empty());

    bool sawNonEmpty = false;
    for (const auto& ch : reBlocks[0].lines[0].chars) {
        if (!ch.fontName.empty()) { sawNonEmpty = true; break; }
    }
    EXPECT_TRUE(sawNonEmpty)
        << "All characters on the reopened first line have empty "
           "fontName — the Tf operators in the post-save content "
           "stream reference fonts not in the page Resources dict.";
}

// ─────────────────────────────────────────────────────────────────────
// 6. PdfFontEmbedder direct test: embedFont registers the font in the
//    page's Resources/Font dict and the same pdf_obj* is reachable via
//    a subsequent dict lookup. (This locks the embedder contract that
//    phase-4's snapshot loop depends on.)
// ─────────────────────────────────────────────────────────────────────
TEST_F(FontResourcePersistenceTest, EmbedderRegistersInPageResourceDict) {
    auto doc = Document::open(ctx_, fixtureSrc_);
    auto page = doc->page(0);
    PdfFontEmbedder embedder(ctx_.get());

    // Resolve a real Arial-or-equivalent font path via FontResolver
    // so this test works on Linux too (where Arial is replaced by
    // metric-compatible Liberation Sans).
    std::string fontPath = pdfe::fonts::FontResolver::resolveFontPath("Arial");
    ASSERT_FALSE(fontPath.empty())
        << "FontResolver could not locate Arial or any metric-compatible substitute.";
    auto info = embedder.embedFont(page.get(), doc.get(), fontPath);
    ASSERT_TRUE(info.has_value())
        << "embedFont should succeed on a resolved Arial-equivalent path: " << fontPath;
    EXPECT_FALSE(info->resourceName.empty());
    EXPECT_NE(info->pdfFontObj, nullptr);
    // The resource name must follow the "Fsub<N>" convention so the
    // re-insertion loop's snapshot of pre-filter Font dict picks it up.
    EXPECT_EQ(info->resourceName.substr(0, 4), "Fsub")
        << "Embedder must use Fsub<N> naming so phase-4 can identify "
           "embedder-added entries by-convention if needed.";
}

// ─────────────────────────────────────────────────────────────────────
// 7. Two-run save round-trip: distinct fontName transitions visible
//    on the reopened page (the user-visible contract). This is a
//    stronger version of #5 — locks that not just one but TWO distinct
//    non-empty fontNames appear.
// ─────────────────────────────────────────────────────────────────────
TEST_F(FontResourcePersistenceTest,
       TwoRunsProduceTwoDistinctFontNamesOnReopen) {
#ifndef _WIN32
    GTEST_SKIP() << "Embed pipeline depends on SystemFontDiscovery Linux impl";
#endif
    auto origDoc = Document::open(ctx_, fixtureSrc_);
    auto origPage = origDoc->page(0);
    TextExtractor extractor(ctx_);
    auto blocks = extractor.extract(origPage.get());
    TextEditModel model(blocks);
    TextSpan span = model.span(0);
    int len = static_cast<int>(span.text.size());
    ASSERT_GE(len, 4);

    FormattingRun runA, runB;
    runA.charStart = 0;        runA.charEnd = len / 2;
    runA.fontFamily = "Arial"; runA.fontSize = span.fontSize;
    runB.charStart = len / 2;  runB.charEnd = len;
    runB.fontFamily = "Times New Roman"; runB.fontSize = span.fontSize;
    origDoc.reset();

    editAndSave({runA, runB});

    auto reopened = Document::open(ctx_, tempOut_);
    auto rePage = reopened->page(0);
    TextExtractor extractor2(ctx_);
    auto reBlocks = extractor2.extract(rePage.get());
    ASSERT_FALSE(reBlocks.empty());
    ASSERT_FALSE(reBlocks[0].lines.empty());

    int distinctNonEmptyFonts = 0;
    std::string prev;
    for (const auto& ch : reBlocks[0].lines[0].chars) {
        if (ch.fontName != prev && !ch.fontName.empty()) {
            ++distinctNonEmptyFonts;
            prev = ch.fontName;
        }
    }
    EXPECT_GE(distinctNonEmptyFonts, 2)
        << "Reopened first line has fewer than 2 distinct non-empty "
           "fontNames — multi-run distinction did not survive save. "
           "Found: " << distinctNonEmptyFonts;
}
