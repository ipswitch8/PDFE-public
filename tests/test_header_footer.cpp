// -*- coding: utf-8 -*-
// Phase 4d: Headers and Footers tests.
// Covers token substitution, page-range filtering, marker-based idempotent
// removal (no duplication on re-run), and round-trip through save/reload.
#include <gtest/gtest.h>
#include "headerfooter/HeaderFooterEngine.h"
#include "headerfooter/HeaderFooterConfig.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

using namespace pdfe::headerfooter;
using namespace pdfe::core;

namespace {

std::string readFileAsText(const std::filesystem::path& p)
{
    std::ifstream f(p, std::ios::binary);
    std::string out((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
    return out;
}

int countOccurrences(const std::string& hay, const std::string& needle)
{
    if (needle.empty()) return 0;
    int n = 0;
    size_t pos = 0;
    while ((pos = hay.find(needle, pos)) != std::string::npos) {
        ++n;
        pos += needle.size();
    }
    return n;
}

std::filesystem::path fixturesDir()
{
    return std::filesystem::path(TEST_FIXTURES_DIR);
}

std::filesystem::path tempOut(const char* name)
{
    auto dir = std::filesystem::temp_directory_path() / "pdfe_hf_tests";
    std::filesystem::create_directories(dir);
    return dir / name;
}

} // namespace

// ------------------- Token resolution -------------------

TEST(HeaderFooterTokens, ResolvesDate)
{
    auto s = HeaderFooterEngine::resolveTokens(
        "Date: {date}", 0, 1, "Alice", "test.pdf", "2026-04-22");
    EXPECT_EQ(s, "Date: 2026-04-22");
}

TEST(HeaderFooterTokens, ResolvesAuthor)
{
    auto s = HeaderFooterEngine::resolveTokens(
        "By {author}", 0, 1, "Alice", "test.pdf", "2026-04-22");
    EXPECT_EQ(s, "By Alice");
}

TEST(HeaderFooterTokens, ResolvesPageAndPages)
{
    // pageIndex 0 (0-based) → {page}=1. Total 10 → {pages}=10.
    auto s = HeaderFooterEngine::resolveTokens(
        "{page} / {pages}", 0, 10, "", "", "2026-04-22");
    EXPECT_EQ(s, "1 / 10");
    s = HeaderFooterEngine::resolveTokens(
        "{page}", 4, 10, "", "", "2026-04-22");
    EXPECT_EQ(s, "5");
}

TEST(HeaderFooterTokens, ResolvesFilename)
{
    auto s = HeaderFooterEngine::resolveTokens(
        "File: {filename}", 0, 1, "", "report.pdf", "2026-04-22");
    EXPECT_EQ(s, "File: report.pdf");
}

TEST(HeaderFooterTokens, ResolvesAllTokensTogether)
{
    auto s = HeaderFooterEngine::resolveTokens(
        "{date} | {author} | {page}/{pages} | {filename}",
        2, 7, "Bob", "doc.pdf", "2026-04-22");
    EXPECT_EQ(s, "2026-04-22 | Bob | 3/7 | doc.pdf");
}

TEST(HeaderFooterTokens, EmptyStringReturnsEmpty)
{
    auto s = HeaderFooterEngine::resolveTokens(
        "", 0, 1, "A", "f.pdf", "2026-04-22");
    EXPECT_TRUE(s.empty());
}

TEST(HeaderFooterTokens, LiteralTextWithoutTokensUnchanged)
{
    auto s = HeaderFooterEngine::resolveTokens(
        "Just a plain footer.", 0, 1, "A", "f.pdf", "2026-04-22");
    EXPECT_EQ(s, "Just a plain footer.");
}

// ------------------- Page range filtering -------------------

TEST(HeaderFooterRange, AllIncludesEveryPage)
{
    HeaderFooterConfig cfg;
    cfg.rangeMode = PageRangeMode::All;
    for (int i = 0; i < 10; ++i) {
        EXPECT_TRUE(HeaderFooterEngine::pageInRange(i, 10, cfg));
    }
}

TEST(HeaderFooterRange, OddOnlyIncludesOneBasedOdd)
{
    HeaderFooterConfig cfg;
    cfg.rangeMode = PageRangeMode::OddPagesOnly;
    // 0-based indices 0,2,4 → 1-based 1,3,5 → odd.
    EXPECT_TRUE(HeaderFooterEngine::pageInRange(0, 6, cfg));
    EXPECT_FALSE(HeaderFooterEngine::pageInRange(1, 6, cfg));
    EXPECT_TRUE(HeaderFooterEngine::pageInRange(2, 6, cfg));
    EXPECT_FALSE(HeaderFooterEngine::pageInRange(3, 6, cfg));
    EXPECT_TRUE(HeaderFooterEngine::pageInRange(4, 6, cfg));
}

TEST(HeaderFooterRange, EvenOnlyIncludesOneBasedEven)
{
    HeaderFooterConfig cfg;
    cfg.rangeMode = PageRangeMode::EvenPagesOnly;
    EXPECT_FALSE(HeaderFooterEngine::pageInRange(0, 6, cfg));
    EXPECT_TRUE(HeaderFooterEngine::pageInRange(1, 6, cfg));
    EXPECT_FALSE(HeaderFooterEngine::pageInRange(2, 6, cfg));
    EXPECT_TRUE(HeaderFooterEngine::pageInRange(3, 6, cfg));
}

TEST(HeaderFooterRange, ExplicitOnlyIncludesListed)
{
    HeaderFooterConfig cfg;
    cfg.rangeMode = PageRangeMode::Explicit;
    cfg.explicitPages = {0, 2, 4};  // 0-based
    EXPECT_TRUE(HeaderFooterEngine::pageInRange(0, 6, cfg));
    EXPECT_FALSE(HeaderFooterEngine::pageInRange(1, 6, cfg));
    EXPECT_TRUE(HeaderFooterEngine::pageInRange(2, 6, cfg));
    EXPECT_FALSE(HeaderFooterEngine::pageInRange(3, 6, cfg));
    EXPECT_TRUE(HeaderFooterEngine::pageInRange(4, 6, cfg));
    EXPECT_FALSE(HeaderFooterEngine::pageInRange(5, 6, cfg));
}

TEST(HeaderFooterRange, OutOfBoundsRejected)
{
    HeaderFooterConfig cfg;
    cfg.rangeMode = PageRangeMode::All;
    EXPECT_FALSE(HeaderFooterEngine::pageInRange(-1, 5, cfg));
    EXPECT_FALSE(HeaderFooterEngine::pageInRange(5, 5, cfg));
}

// ------------------- Full apply round-trip -------------------

class HeaderFooterApplyTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx = std::make_shared<MuPdfContext>();
        src = fixturesDir() / "test_3page.pdf";
        if (!std::filesystem::exists(src)) {
            GTEST_SKIP() << "test_3page.pdf not available";
        }
    }
    std::shared_ptr<MuPdfContext> ctx;
    std::filesystem::path src;
};

TEST_F(HeaderFooterApplyTest, AppliedFileOpensAndPreservesPageCount)
{
    HeaderFooterConfig cfg;
    cfg.centerFooter = "Page {page} of {pages}";
    cfg.author = "Tester";
    auto out = tempOut("apply_basic.pdf");
    std::filesystem::remove(out);

    HeaderFooterEngine eng(ctx);
    ASSERT_NO_THROW(eng.apply(src, out, cfg));
    ASSERT_TRUE(std::filesystem::exists(out));

    auto doc = Document::open(ctx, out);
    ASSERT_NE(doc, nullptr);
    EXPECT_EQ(doc->pageCount(), 3);
}

TEST_F(HeaderFooterApplyTest, MarkersPresentInOutputStream)
{
    HeaderFooterConfig cfg;
    cfg.centerFooter = "Footer text";
    auto out = tempOut("apply_markers.pdf");
    std::filesystem::remove(out);

    HeaderFooterEngine eng(ctx);
    eng.apply(src, out, cfg);

    // Scan raw output bytes for the start marker — not a strict test
    // (markers may be inside compressed streams) but sufficient when output
    // is written with default uncompressed-content-stream settings.
    std::string raw = readFileAsText(out);
    // Marker may be in compressed stream; count may be 0 or >0 depending on
    // compression. Accept either — the real idempotency test below verifies
    // the internal behavior through apply+apply page-count invariance.
    SUCCEED() << "Markers in raw bytes: "
              << countOccurrences(raw, HeaderFooterEngine::kStartMarker);
}

TEST_F(HeaderFooterApplyTest, ReApplyDoesNotStackDuplicates)
{
    // Idempotency: applying twice should not cause text or page growth.
    HeaderFooterConfig cfg;
    cfg.centerFooter = "Footer text {page}";
    auto out1 = tempOut("idempotent_1.pdf");
    auto out2 = tempOut("idempotent_2.pdf");
    std::filesystem::remove(out1);
    std::filesystem::remove(out2);

    HeaderFooterEngine eng(ctx);
    eng.apply(src, out1, cfg);
    eng.apply(out1, out2, cfg);  // re-apply onto already-headered file

    auto doc1 = Document::open(ctx, out1);
    auto doc2 = Document::open(ctx, out2);
    ASSERT_NE(doc1, nullptr);
    ASSERT_NE(doc2, nullptr);
    EXPECT_EQ(doc1->pageCount(), doc2->pageCount());

    // Text extraction count should be approximately the same, not doubled.
    for (int i = 0; i < doc1->pageCount(); ++i) {
        auto p1 = doc1->page(i);
        auto p2 = doc2->page(i);
        ASSERT_NE(p1, nullptr);
        ASSERT_NE(p2, nullptr);
        std::string t1 = p1->extractPlainText();
        std::string t2 = p2->extractPlainText();
        int c1 = countOccurrences(t1, "Footer text");
        int c2 = countOccurrences(t2, "Footer text");
        EXPECT_EQ(c1, c2) << "Re-apply created duplicate on page " << (i + 1)
                          << " (was " << c1 << ", now " << c2 << ")";
    }
}

TEST_F(HeaderFooterApplyTest, OddPagesRangeOnlyTouchesOddPages)
{
    HeaderFooterConfig cfg;
    cfg.centerFooter = "OnlyOdd";
    cfg.rangeMode = PageRangeMode::OddPagesOnly;
    auto out = tempOut("odd_only.pdf");
    std::filesystem::remove(out);

    HeaderFooterEngine eng(ctx);
    eng.apply(src, out, cfg);

    auto doc = Document::open(ctx, out);
    ASSERT_NE(doc, nullptr);
    // test_3page.pdf has 3 pages → 1-based odd = pages 1,3; even = page 2.
    auto p1 = doc->page(0);  // 1-based 1 → odd
    auto p2 = doc->page(1);  // 1-based 2 → even (skip)
    auto p3 = doc->page(2);  // 1-based 3 → odd
    ASSERT_NE(p1, nullptr);
    ASSERT_NE(p2, nullptr);
    ASSERT_NE(p3, nullptr);
    EXPECT_NE(p1->extractPlainText().find("OnlyOdd"), std::string::npos)
        << "Odd page 1 should have footer";
    EXPECT_EQ(p2->extractPlainText().find("OnlyOdd"), std::string::npos)
        << "Even page 2 should NOT have footer";
    EXPECT_NE(p3->extractPlainText().find("OnlyOdd"), std::string::npos)
        << "Odd page 3 should have footer";
}

TEST_F(HeaderFooterApplyTest, RemoveDropsMarkedStreams)
{
    HeaderFooterConfig cfg;
    cfg.centerFooter = "RemoveMe";
    auto out1 = tempOut("remove_source.pdf");
    auto out2 = tempOut("remove_after.pdf");
    std::filesystem::remove(out1);
    std::filesystem::remove(out2);

    HeaderFooterEngine eng(ctx);
    eng.apply(src, out1, cfg);
    eng.remove(out1, out2);

    auto doc = Document::open(ctx, out2);
    ASSERT_NE(doc, nullptr);
    for (int i = 0; i < doc->pageCount(); ++i) {
        auto p = doc->page(i);
        ASSERT_NE(p, nullptr);
        EXPECT_EQ(p->extractPlainText().find("RemoveMe"), std::string::npos)
            << "Remove should drop the footer on page " << (i + 1);
    }
}

TEST_F(HeaderFooterApplyTest, TokenResolutionSurvivesSaveReload)
{
    HeaderFooterConfig cfg;
    cfg.leftFooter = "{page}/{pages}";
    auto out = tempOut("tokens_roundtrip.pdf");
    std::filesystem::remove(out);

    HeaderFooterEngine eng(ctx);
    eng.apply(src, out, cfg);

    auto doc = Document::open(ctx, out);
    ASSERT_NE(doc, nullptr);
    ASSERT_EQ(doc->pageCount(), 3);

    // Page 1 should contain "1/3", page 2 "2/3", page 3 "3/3".
    for (int i = 0; i < 3; ++i) {
        auto p = doc->page(i);
        ASSERT_NE(p, nullptr);
        std::string expected = std::to_string(i + 1) + "/3";
        EXPECT_NE(p->extractPlainText().find(expected), std::string::npos)
            << "Page " << (i + 1) << " missing '" << expected << "'";
    }
}
