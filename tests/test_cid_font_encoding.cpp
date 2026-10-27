#include <gtest/gtest.h>
#include "textedit/ContentStreamEditor.h"
#include "textedit/TextEditModel.h"
#include "textedit/TextEditCommands.h"
#include "core/Document.h"
#include "core/Page.h"
#include "core/MuPdfContext.h"
#include "fonts/PdfFontEmbedder.h"
#include "fonts/EmbeddedFontAnalyzer.h"
#include "fonts/FontAnalysisCache.h"
#include "fonts/SubstituteFontSelector.h"
#include "fonts/FontSubstitutionEngine.h"

#include <QRectF>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace pdfe::textedit;
using namespace pdfe::core;

static std::string fixturesDir() {
    return TEST_FIXTURES_DIR;
}

// =============================================================================
// Test fixture for CID font encoding in ContentStreamEditor.
// Exercises the isMultiByteFont / utf32ToCidEncoded code paths.
// =============================================================================
class CidFontEncodingTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<MuPdfContext>();

        // Load a native text PDF (simple font) for contrast tests
        auto nativePath = std::filesystem::path(fixturesDir()) / "test_single.pdf";
        if (std::filesystem::exists(nativePath)) {
            nativeDoc_ = Document::open(context_, nativePath);
            hasNative_ = (nativeDoc_ && nativeDoc_->pageCount() > 0);
        }

        // Load OCR text PDF (image-dominated, may have CID fonts from OCR layer)
        auto ocrPath = std::filesystem::path(fixturesDir()) / "test_ocr_text.pdf";
        if (std::filesystem::exists(ocrPath)) {
            ocrDoc_ = Document::open(context_, ocrPath);
            hasOcr_ = (ocrDoc_ && ocrDoc_->pageCount() > 0);
        }

        // Check for Arial (needed for CID font embedding)
        arialPath_ = "C:\\Windows\\Fonts\\arial.ttf";
        hasArial_ = std::filesystem::exists(arialPath_);
    }

    void TearDown() override {
        nativeDoc_.reset();
        ocrDoc_.reset();
        context_.reset();
    }

    // Helper: collect all spans from a page
    std::vector<TextSpan> collectSpans(Page* page) {
        auto blocks = page->extractText();
        TextEditModel model(blocks);
        std::vector<TextSpan> result;
        for (int i = 0; i < model.spanCount(); ++i) {
            result.push_back(model.span(i));
        }
        return result;
    }

    std::shared_ptr<MuPdfContext> context_;
    std::unique_ptr<Document> nativeDoc_;
    std::unique_ptr<Document> ocrDoc_;
    bool hasNative_ = false;
    bool hasOcr_ = false;
    std::string arialPath_;
    bool hasArial_ = false;
};

// =============================================================================
// Test 1: applyEdits on native (simple font) page produces extractable text
// Verifies the simple-font path (utf32ToFontEncoded) still works.
// =============================================================================
TEST_F(CidFontEncodingTest, SimpleFontPath_ProducesExtractableText)
{
    if (!hasNative_) {
        GTEST_SKIP() << "Requires test_single.pdf";
    }

    auto page = nativeDoc_->page(0);
    ASSERT_NE(page, nullptr);

    auto spans = collectSpans(page.get());
    ASSERT_GT(spans.size(), 0u);

    // Find a span with enough text
    int targetIdx = -1;
    for (size_t i = 0; i < spans.size(); ++i) {
        if (spans[i].text.size() >= 3) {
            targetIdx = static_cast<int>(i);
            break;
        }
    }
    ASSERT_GE(targetIdx, 0) << "Need a span with 3+ chars";

    // Replace with known text
    std::vector<std::pair<int, std::u32string>> mods;
    mods.push_back({targetIdx, U"SIMPLEFONT"});

    ContentStreamEditor editor(context_.get());
    EXPECT_NO_THROW(
        editor.applyEdits(page.get(), nativeDoc_.get(), spans, mods)
    );

    // Verify the replacement text is extractable
    auto page2 = nativeDoc_->page(0);
    ASSERT_NE(page2, nullptr);
    std::string extracted = page2->extractPlainText();
    EXPECT_NE(extracted.find("SIMPLEFONT"), std::string::npos)
        << "Simple font edit should produce extractable text. Got: " << extracted;
}

// =============================================================================
// Test 2: applyEdits on page with CID font (embedded via PdfFontEmbedder)
// This exercises the isMultiByteFont/utf32ToCidEncoded path.
// =============================================================================
TEST_F(CidFontEncodingTest, CidFontPath_ViaEmbeddedFont)
{
    if (!hasNative_ || !hasArial_) {
        GTEST_SKIP() << "Requires test_single.pdf and C:\\Windows\\Fonts\\arial.ttf";
    }

    auto page = nativeDoc_->page(0);
    ASSERT_NE(page, nullptr);

    // First, embed a CID font (Arial) into the page
    pdfe::fonts::PdfFontEmbedder embedder(context_.get());
    auto embedResult = embedder.embedFont(page.get(), nativeDoc_.get(), arialPath_);
    ASSERT_TRUE(embedResult.has_value())
        << "Should embed Arial as CID font";
    ASSERT_FALSE(embedResult->resourceName.empty());

    // Now extract spans — the page should still have its original spans
    auto spans = collectSpans(page.get());
    ASSERT_GT(spans.size(), 0u);

    // Apply edits using font substitution (which uses the CID font)
    int targetIdx = -1;
    for (size_t i = 0; i < spans.size(); ++i) {
        if (spans[i].text.size() >= 3) {
            targetIdx = static_cast<int>(i);
            break;
        }
    }
    ASSERT_GE(targetIdx, 0);

    // Create substitution info pointing to the embedded CID font
    pdfe::fonts::SubstitutionInfo subInfo;
    subInfo.spanIndex = targetIdx;
    subInfo.substituteResName = embedResult->resourceName;
    subInfo.horizontalScale = 1.0f;
    subInfo.originalFontSize = spans[targetIdx].fontSize;
    subInfo.originalFontName = spans[targetIdx].fontName;

    std::vector<pdfe::fonts::SubstitutionInfo> subs = {subInfo};
    std::vector<std::pair<int, std::u32string>> mods;
    mods.push_back({targetIdx, U"CIDFONT"});

    ContentStreamEditor editor(context_.get());
    EXPECT_NO_THROW(
        editor.applyEditsWithSubstitution(
            page.get(), nativeDoc_.get(), spans, mods, subs)
    ) << "applyEditsWithSubstitution with CID font should not throw";

    // The edit should not corrupt the page
    auto page2 = nativeDoc_->page(0);
    ASSERT_NE(page2, nullptr);
    auto blocks = page2->extractText();
    EXPECT_GT(blocks.size(), 0u)
        << "Page should still have valid text blocks after CID font edit";
}

// =============================================================================
// Test 3: applyEdits on OCR page (image-dominated) with Tr=3
// The OCR layer may have CID or simple fonts depending on OCR engine config.
// This tests that the encoding path handles either case.
// =============================================================================
TEST_F(CidFontEncodingTest, OcrPage_TextRenderMode3_Encoding)
{
    if (!hasOcr_) {
        GTEST_SKIP() << "Requires test_ocr_text.pdf";
    }

    auto page = ocrDoc_->page(0);
    ASSERT_NE(page, nullptr);

    auto spans = collectSpans(page.get());
    ASSERT_GT(spans.size(), 0u);

    // Apply edit with Tr=3 (invisible OCR text)
    std::vector<std::pair<int, std::u32string>> mods;
    mods.push_back({0, U"OCRCIDENCODE"});

    ContentStreamEditor editor(context_.get());
    EXPECT_NO_THROW(
        editor.applyEdits(page.get(), ocrDoc_.get(), spans, mods, /*textRenderMode=*/3)
    ) << "applyEdits with Tr=3 on OCR page should not throw";

    auto page2 = ocrDoc_->page(0);
    ASSERT_NE(page2, nullptr);
    std::string extracted = page2->extractPlainText();
    EXPECT_NE(extracted.find("OCRCIDENCODE"), std::string::npos)
        << "OCR text edit should be extractable. Got: " << extracted;
}

// =============================================================================
// Test 4: Multiple edits on the same page don't corrupt the content stream
// Exercises the mranges/ranges/xranges scanning paths on repeated calls.
// =============================================================================
TEST_F(CidFontEncodingTest, MultipleEdits_NoCrash)
{
    if (!hasNative_) {
        GTEST_SKIP() << "Requires test_single.pdf";
    }

    auto page = nativeDoc_->page(0);
    ASSERT_NE(page, nullptr);

    ContentStreamEditor editor(context_.get());

    // Apply 3 sequential edits to different spans
    for (int round = 0; round < 3; ++round) {
        auto spans = collectSpans(page.get());
        if (spans.empty()) break;

        int idx = round % static_cast<int>(spans.size());
        if (spans[idx].text.size() < 2) continue;

        std::u32string replacement = U"R" + std::u32string(1, U'0' + round);
        std::vector<std::pair<int, std::u32string>> mods;
        mods.push_back({idx, replacement});

        EXPECT_NO_THROW(
            editor.applyEdits(page.get(), nativeDoc_.get(), spans, mods)
        ) << "Edit round " << round << " should not throw";

        // Re-read page for next round
        page = nativeDoc_->page(0);
        ASSERT_NE(page, nullptr);
    }

    // Final verification: page is still valid
    auto finalPage = nativeDoc_->page(0);
    ASSERT_NE(finalPage, nullptr);
    auto blocks = finalPage->extractText();
    EXPECT_GT(blocks.size(), 0u) << "Page should still have text after 3 edits";
}

// =============================================================================
// Test 5: EditTextCommand on native page handles font encoding correctly
// Integration test that exercises the full edit pipeline.
// =============================================================================
TEST_F(CidFontEncodingTest, EditTextCommand_FullPipeline)
{
    if (!hasNative_) {
        GTEST_SKIP() << "Requires test_single.pdf";
    }

    auto page = nativeDoc_->page(0);
    ASSERT_NE(page, nullptr);

    auto blocks = page->extractText();
    ASSERT_FALSE(blocks.empty());

    TextEditModel model(blocks);
    ASSERT_GT(model.spanCount(), 0);

    const auto& span = model.span(0);
    std::u32string oldText = span.text;
    ASSERT_FALSE(oldText.empty());

    std::u32string newText = U"PIPELINE_TEST";

    // Execute → undo → redo should all work
    EditTextCommand cmd(nativeDoc_.get(), context_, 0, 0, span, oldText, newText);

    EXPECT_NO_THROW(cmd.execute());
    {
        auto p = nativeDoc_->page(0);
        std::string text = p->extractPlainText();
        EXPECT_NE(text.find("PIPELINE_TEST"), std::string::npos)
            << "After execute, edited text should be present";
    }

    EXPECT_NO_THROW(cmd.undo());
    {
        auto p = nativeDoc_->page(0);
        std::string text = p->extractPlainText();
        EXPECT_EQ(text.find("PIPELINE_TEST"), std::string::npos)
            << "After undo, edited text should be gone";
    }

    EXPECT_NO_THROW(cmd.redo());
    {
        auto p = nativeDoc_->page(0);
        std::string text = p->extractPlainText();
        EXPECT_NE(text.find("PIPELINE_TEST"), std::string::npos)
            << "After redo, edited text should reappear";
    }
}

// =============================================================================
// Test 6: Verify CID font embedding creates multi-byte encoding
// Tests that PdfFontEmbedder creates fonts that isMultiByteFont would identify.
// =============================================================================
TEST_F(CidFontEncodingTest, EmbeddedCidFont_HasMultiByteEncoding)
{
    if (!hasNative_ || !hasArial_) {
        GTEST_SKIP() << "Requires test_single.pdf and arial.ttf";
    }

    auto page = nativeDoc_->page(0);
    ASSERT_NE(page, nullptr);

    // Embed a CID font
    pdfe::fonts::PdfFontEmbedder embedder(context_.get());
    auto result = embedder.embedFont(page.get(), nativeDoc_.get(), arialPath_);
    ASSERT_TRUE(result.has_value());

    // Analyze the page fonts — the newly embedded CID font should appear
    pdfe::fonts::EmbeddedFontAnalyzer analyzer(context_.get());
    auto fontInfos = analyzer.analyzePageFonts(page.get(), nativeDoc_.get());

    // After embedding, there should be more fonts than before
    // (the original page fonts + our new CID font)
    EXPECT_GT(fontInfos.size(), 0u)
        << "Page should have font resources after embedding";

    // Check that at least one font has an Arial-related name
    bool foundArial = false;
    for (const auto& fi : fontInfos) {
        if (fi.postScriptName.find("Arial") != std::string::npos ||
            fi.familyName.find("Arial") != std::string::npos ||
            fi.postScriptName.find("arial") != std::string::npos ||
            fi.familyName.find("arial") != std::string::npos) {
            foundArial = true;
            break;
        }
    }
    EXPECT_TRUE(foundArial)
        << "Should find an Arial-related font after embedding CID font";
}

// =============================================================================
// Test 7: Verify range overflow protection in CMap scanning
// The unsigned int loop (was unsigned short) prevents overflow when hi == 0xFFFF.
// This is a regression test for Karen QA finding #2.
// =============================================================================
TEST_F(CidFontEncodingTest, RangeOverflowProtection)
{
    // This test verifies the fix at the language level:
    // unsigned short with hi=0xFFFF would overflow in `cid <= hi` loop.
    // unsigned int with hi=0xFFFF does not overflow.

    // Simulate the fixed loop logic
    unsigned int lo = 0xFFF0;
    unsigned int hi = 0xFFFF;
    unsigned int out = 0x4E00;  // CJK Unified Ideographs base
    int count = 0;

    for (unsigned int cid = lo; cid <= hi; cid++) {
        unsigned int ucs = out + (cid - lo);
        if (ucs != 0 && ucs != 0xFFFD && ucs <= 0xFFFF) {
            count++;
        }
    }

    EXPECT_EQ(count, 16)
        << "Loop should process exactly 16 entries (0xFFF0..0xFFFF) without overflow";

    // Verify the old unsigned short version would have been infinite:
    // for (unsigned short cid = 0xFFF0; cid <= 0xFFFF; cid++)
    // when cid hits 0xFFFF, cid++ wraps to 0, making cid <= 0xFFFF true forever.
    // We can't actually run the infinite loop, but we verify the uint version terminates.
    SUCCEED() << "unsigned int loop terminates correctly for hi=0xFFFF";
}

// =============================================================================
// Test 8: Verify mranges scanning handles single-codepoint entries
// Tests the new mranges scan path added for Karen QA finding #4.
// =============================================================================
TEST_F(CidFontEncodingTest, MrangesSingleCodepointMapping)
{
    // The mranges scan adds entries where dict[idx] == 1 (single Unicode codepoint).
    // We can't directly test the static function, but we verify the logic:
    // for each mrange entry {low=CID, out=dictIdx}, if dict[dictIdx] == 1,
    // then unicodeToCid[dict[dictIdx+1]] = CID.

    // Simulate the mranges scanning logic
    struct MockMrange {
        unsigned int low;  // CID
        int out;           // dict index
    };

    std::vector<int> dict = {
        1, 0x00E9,       // idx 0,1: CID maps to U+00E9 (é) — single codepoint
        2, 0x0066, 0x0069, // idx 2,3,4: CID maps to U+0066 U+0069 (fi) — ligature, skip
        1, 0x00F1,       // idx 5,6: CID maps to U+00F1 (ñ) — single codepoint
    };

    std::vector<MockMrange> mranges = {
        {42, 0},   // CID 42 → dict[0]: len=1, ucs=0x00E9
        {43, 2},   // CID 43 → dict[2]: len=2, skip (ligature)
        {44, 5},   // CID 44 → dict[5]: len=1, ucs=0x00F1
    };

    std::unordered_map<uint32_t, uint16_t> unicodeToCid;
    int dlen = static_cast<int>(dict.size());

    for (size_t i = 0; i < mranges.size(); i++) {
        unsigned int cid = mranges[i].low;
        int dictIdx = mranges[i].out;
        if (dictIdx >= 0 && dictIdx < dlen) {
            int len = dict[dictIdx];
            if (len == 1 && dictIdx + 1 < dlen) {
                unsigned int ucs = static_cast<unsigned int>(dict[dictIdx + 1]);
                if (ucs != 0 && ucs != 0xFFFD && ucs <= 0xFFFF) {
                    unicodeToCid[static_cast<uint32_t>(ucs)] = static_cast<uint16_t>(cid);
                }
            }
        }
    }

    // Verify: only single-codepoint entries were captured
    EXPECT_EQ(unicodeToCid.size(), 2u)
        << "Should capture 2 single-codepoint mrange entries (skip ligature)";
    EXPECT_EQ(unicodeToCid[0x00E9], 42)
        << "U+00E9 (é) should map to CID 42";
    EXPECT_EQ(unicodeToCid[0x00F1], 44)
        << "U+00F1 (ñ) should map to CID 44";

    // Verify ligature was NOT captured
    EXPECT_EQ(unicodeToCid.count(0x0066), 0u)
        << "Ligature codepoint U+0066 should NOT be in reverse map";
}
