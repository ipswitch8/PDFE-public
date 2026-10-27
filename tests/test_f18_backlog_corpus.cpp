// tests/test_f18_backlog_corpus.cpp
//
// F18 (meta-audit follow-up phase-18) — consumers for the phase-2 synthesis
// backlog fixtures the meta-audit recorded as ZERO-COVERAGE classes:
//
//   A3-48  ToUnicode-absent page  -> redaction-BY-SEARCH security false negative
//   A3-49  0-page document        -> downstream survival           (also A3-33)
//   A3-51  incremental /Prev chain-> latest-object-wins + shadowed-byte removal
//   A3-52  deeply nested array    -> deep-recursion parse boundary
//   A3-53  10000-page document    -> scale boundary                (also A3-33)
//   A3-50  CJK content preservation consumer for the ORPHAN fixture
//          tests/fixtures/phase3_cjk_text.pdf (R-34 orphan-fixture instance)
//
// EVERY expectation below is anchored to AUTHORED ground truth — the literals
// written by tests/fixtures/gen_f18_backlog_corpus.py (documented in its
// docstring), never to "what PDFE currently outputs". The generator is the
// oracle's source of truth; if PDFE disagrees with the generator, PDFE is wrong.

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "core/SearchEngine.h"
#include "core/SearchResult.h"
#include "core/TextExtractor.h"
#include "compliance/FontComplianceChecker.h"

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace pdfe::core;

namespace {

fs::path fixture(const char* name) { return fs::path(TEST_FIXTURES_DIR) / name; }

std::string readAllBytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

int countOccurrences(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return 0;
    int n = 0;
    for (size_t p = hay.find(needle); p != std::string::npos;
         p = hay.find(needle, p + needle.size()))
        ++n;
    return n;
}

// Concatenate every DECOMPRESSED stream in a PDF. A raw byte-grep is useless
// against a saved file because PDFE Flate-compresses content streams; the
// confidentiality question ("are the shadowed revision's bytes still in the
// output?") can only be answered after decoding.
std::string allDecodedStreams(const std::shared_ptr<MuPdfContext>& ctx,
                              const fs::path& path) {
    std::string out;
    const std::string p = path.string();
    ctx->execute([&](fz_context* c) {
        pdf_document* doc = pdf_open_document(c, p.c_str());
        const int n = pdf_count_objects(c, doc);
        for (int i = 1; i < n; ++i) {
            // pdf_obj_num_is_stream / pdf_load_stream_number take the object
            // NUMBER and consult the xref themselves, so they are safe here
            // (the resolved-dict trap only bites pdf_is_stream/pdf_load_stream).
            if (!pdf_obj_num_is_stream(c, doc, i)) continue;
            fz_buffer* b = nullptr;
            fz_try(c) {
                b = pdf_load_stream_number(c, doc, i);
                unsigned char* d = nullptr;
                const size_t len = fz_buffer_storage(c, b, &d);
                out.append(reinterpret_cast<char*>(d), len);
            }
            fz_always(c) { fz_drop_buffer(c, b); }
            fz_catch(c) { /* an undecodable stream cannot hold plaintext */ }
        }
        pdf_drop_document(c, doc);
    });
    return out;
}

// Unique per-test output path so repeated/concurrent runs never collide on a
// shared temp file (the repeat-safety lesson from gap #49).
fs::path uniqueOut(const char* tag) {
    return fs::temp_directory_path() /
           (std::string("pdfe_f18_") + tag + "_" +
            std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
            ".pdf");
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// A3-16 — search oracle anchored to AUTHORED counts.
//
// The register's cited tautology was `EXPECT_GE(result.totalMatches(), 0)` in
// tests/test_search.cpp — an assertion that cannot fail for a signed count of a
// vector's size. These replace it with the generator's literals:
//   NEEDLE x6 (case-sensitive), needle x2 (case-sensitive), 8 case-insensitive,
//   HAYSTACK x6, ABSENTTOKEN x0, per-page distribution 3/3/2 (insensitive).
// ═══════════════════════════════════════════════════════════════════════════
class F18SearchGroundTruth : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_shared<MuPdfContext>();
        const fs::path p = fixture("f18_search_ground_truth.pdf");
        ASSERT_TRUE(fs::exists(p)) << "pinned fixture missing: " << p;
        doc_ = Document::open(ctx_, p);
        ASSERT_NE(doc_, nullptr);
        ASSERT_EQ(doc_->pageCount(), 3);
    }
    std::shared_ptr<MuPdfContext> ctx_;
    std::unique_ptr<Document> doc_;
};

TEST_F(F18SearchGroundTruth, CaseInsensitiveTotalEqualsAuthoredCount) {
    SearchEngine engine(doc_.get());
    const SearchResult r = engine.search("needle", SearchFlags::None);
    EXPECT_EQ(r.query, "needle");
    // Authored: page0 2 upper + 1 lower, page1 3 upper, page2 1 upper + 1 lower.
    EXPECT_EQ(r.totalMatches(), 8)
        << "case-insensitive 'needle' must find every authored occurrence "
           "(2+1, 3+0, 1+1)";
}

TEST_F(F18SearchGroundTruth, CaseSensitiveSplitsUpperAndLowerAsAuthored) {
    SearchEngine engine(doc_.get());
    const SearchResult upper = engine.search("NEEDLE", SearchFlags::CaseSensitive);
    const SearchResult lower = engine.search("needle", SearchFlags::CaseSensitive);
    EXPECT_EQ(upper.totalMatches(), 6) << "authored NEEDLE occurrences";
    EXPECT_EQ(lower.totalMatches(), 2) << "authored lowercase needle occurrences";
    EXPECT_EQ(upper.totalMatches() + lower.totalMatches(), 8)
        << "the two case-sensitive halves must partition the insensitive total";
}

TEST_F(F18SearchGroundTruth, PerPageDistributionMatchesAuthoredLayout) {
    SearchEngine engine(doc_.get());
    const SearchResult r = engine.search("needle", SearchFlags::None);
    int perPage[3] = {0, 0, 0};
    for (const auto& hit : r.hits) {
        ASSERT_GE(hit.pageIndex, 0);
        ASSERT_LT(hit.pageIndex, 3);
        ++perPage[hit.pageIndex];
    }
    EXPECT_EQ(perPage[0], 3) << "authored page 0: NEEDLE x2 + needle x1";
    EXPECT_EQ(perPage[1], 3) << "authored page 1: NEEDLE x3";
    EXPECT_EQ(perPage[2], 2) << "authored page 2: NEEDLE x1 + needle x1";
}

TEST_F(F18SearchGroundTruth, AbsentTokenFindsExactlyZeroAndPresentFillerFindsSix) {
    SearchEngine engine(doc_.get());
    EXPECT_EQ(engine.search("ABSENTTOKEN").totalMatches(), 0)
        << "a token authored nowhere must not match";
    EXPECT_EQ(engine.search("HAYSTACK").totalMatches(), 6)
        << "authored HAYSTACK occurrences (3+2+1)";
}

TEST_F(F18SearchGroundTruth, EveryHitCarriesANonDegenerateQuadOnItsPage) {
    SearchEngine engine(doc_.get());
    const SearchResult r = engine.search("NEEDLE", SearchFlags::CaseSensitive);
    ASSERT_EQ(r.totalMatches(), 6);
    for (const auto& hit : r.hits) {
        ASSERT_FALSE(hit.quads.empty()) << "hit on page " << hit.pageIndex
                                        << " has no highlight quad";
        for (const QRectF& q : hit.quads) {
            EXPECT_GT(q.width(), 1.0) << "degenerate quad width";
            EXPECT_GT(q.height(), 1.0) << "degenerate quad height";
            // Authored text sits at x=72 on a 612x792 page.
            EXPECT_GE(q.left(), 60.0);
            EXPECT_LE(q.right(), 612.0);
        }
    }
}

// A3-16's second half: the file named "performance" asserted no budget at all.
// The ceiling below is deliberately generous (the measured Debug time for this
// 3-page document is on the order of a millisecond) but it is a REAL ceiling: a
// hang, an accidental O(n^2) rescan or a per-hit reopen blows straight through
// it. It is a wall-clock ceiling, not an equality, so it cannot flake on a busy
// runner while still failing on a genuine order-of-magnitude regression.
TEST_F(F18SearchGroundTruth, RepeatedSearchStaysWithinAWallClockCeiling) {
    SearchEngine engine(doc_.get());
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 50; ++i) {
        const SearchResult r = engine.search("needle", SearchFlags::None);
        ASSERT_EQ(r.totalMatches(), 8) << "iteration " << i
                                       << " must be stable across repeats";
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    EXPECT_LT(ms, 10000)
        << "50 searches of a 3-page document took " << ms
        << " ms; the Debug baseline is a few milliseconds per search";
}

// ═══════════════════════════════════════════════════════════════════════════
// A3-48 — ToUnicode-absent page: the redaction-by-search FALSE NEGATIVE class.
//
// The risk the backlog row names: an operator redacts by SEARCHING for a word;
// on a page whose font has no /ToUnicode the search cannot match, so nothing is
// redacted and the operator is told "0 matches" — a security false negative.
//
// ── 2026-09 UPDATE: THE CLASS NARROWED, IT DID NOT DISAPPEAR (GitHub #23) ──
// This test's central assertion USED TO READ:
//
//     EXPECT_EQ(engine.search("SECRET").totalMatches(), 0)
//         << "characterisation: if this ever becomes non-zero, PDFE gained a
//            glyph-name/Unicode recovery path ... — update A3-48";
//
// PDFE has now gained exactly that recovery path
// (src/core/ToUnicodeRecovery.cpp), so the assertion above is the OLD, broken
// behaviour and had to change or the suite would contradict itself. This is
// that update; A3-48's disposition row was updated in the same change.
//
// The fixture was reworked alongside it so that BOTH halves of the class stay
// under test. It now carries TWO Type 3 fonts that share the same six glyph
// procedures and the same /Widths — identical ink — differing only in their
// /Encoding /Differences glyph names:
//
//   /T3  /Ssmall /Esmall /Csmall /Rsmall /Esmall /Tsmall
//        Adobe Expert small-caps names. They are IN the Adobe Glyph List and map
//        to the Corporate Use Subarea (U+F773 …), so an unaided reader extracts
//        Private Use codepoints — text that LOOKS extracted and matches nothing.
//        PDFE strips the Expert suffix and recovers S,E,C,R,E,T, so a search for
//        the word the operator can see NOW HITS. Redaction-by-search works here.
//
//   /T4  /gx1 … /gx6
//        Opaque producer-invented names carrying no Unicode information at all.
//        Nothing can recover letters from them. This run is STILL unsearchable,
//        and it is why A3-48 is narrowed rather than closed: a search-driven
//        redaction on a page like this still silently redacts nothing.
//
// These tests do three separate things:
//   1. pin BOTH halves — the recoverable run is searchable, the unrecoverable
//      run is not — WHILE proving the page is otherwise searchable (the
//      Helvetica control matches), so each result is attributable to that run's
//      encoding and not to the page being unreadable;
//   2. assert PDFE can DETECT the condition — FontComplianceChecker must report
//      the page's font as lacking a ToUnicode CMap, which is what makes a
//      "search found nothing here" warning implementable rather than guesswork;
//   3. assert the SAFE path still works — the authored ink IS on the page, so a
//      redaction driven by geometry rather than by search has something to hit.
// ═══════════════════════════════════════════════════════════════════════════
TEST(F18NoToUnicode, SearchMissesTheType3RunButFindsTheHelveticaControl) {
    auto ctx = std::make_shared<MuPdfContext>();
    const fs::path p = fixture("f18_no_tounicode.pdf");
    ASSERT_TRUE(fs::exists(p));
    auto doc = Document::open(ctx, p);
    ASSERT_NE(doc, nullptr);
    ASSERT_EQ(doc->pageCount(), 1);

    SearchEngine engine(doc.get());
    // CONTROL: ordinary Helvetica text on the same page IS searchable. If this
    // ever fails the fixture is broken, not the claims below.
    EXPECT_EQ(engine.search("VISIBLEMARK").totalMatches(), 1)
        << "the Helvetica control token must be searchable; without it neither "
           "result below proves anything";

    // THE RECOVERED HALF: the /T3 Expert small-caps run. Unaided, MuPDF extracts
    // this as U+F773 U+F765 U+F763 U+F772 U+F765 U+F774 and a search for SECRET
    // finds nothing; with PDFE's glyph-name recovery the word the operator sees
    // is reachable, so redaction-by-search is no longer a false negative here.
    EXPECT_EQ(engine.search("SECRET").totalMatches(), 1)
        << "the /T3 run's Adobe Expert glyph names must be recovered to real "
           "letters (src/core/ToUnicodeRecovery.cpp). Zero here means the "
           "recovery path regressed and the A3-48 false negative is back";

    // THE RESIDUAL HALF: the /T4 run draws IDENTICAL ink from the SAME glyph
    // procedures, but its /gx1../gx6 names carry no Unicode information, so it
    // stays unsearchable. Nothing can honestly recover it — and a "recovery"
    // that claimed to would be inventing text. Searching for the six raw codes
    // is not something an operator can do, so the operational statement of the
    // residual class is: the page's visible ink outnumbers its searchable text.
    // Counted over the STRUCTURED extraction. MEASURED: under PDFE's stext
    // options the six unmappable codes surface as U+FFFD REPLACEMENT CHARACTER
    // (MuPDF's whitespace-preserving default would give the raw codes
    // 0x01..0x06 instead) — either way, not letters, and not searchable.
    int opaqueCodes = 0;
    for (const auto& block : doc->page(0)->extractText())
        for (const auto& line : block.lines)
            for (const auto& ch : line.chars)
                if (ch.codepoint == 0xFFFD ||
                    (ch.codepoint >= 0x01 && ch.codepoint <= 0x06))
                    ++opaqueCodes;
    EXPECT_EQ(opaqueCodes, 6)
        << "the /T4 run must STILL extract as six unmapped characters — it is "
           "the surviving half of the A3-48 class. If these became letters, the "
           "recovery path is inventing Unicode for glyph names that have none";
}

TEST(F18NoToUnicode, ComplianceCheckerDetectsTheMissingToUnicodeCmap) {
    auto ctx = std::make_shared<MuPdfContext>();
    const fs::path p = fixture("f18_no_tounicode.pdf");
    ASSERT_TRUE(fs::exists(p));
    auto doc = Document::open(ctx, p);
    ASSERT_NE(doc, nullptr);

    pdfe::compliance::FontComplianceChecker checker(ctx);
    const auto fonts = checker.checkFonts(doc.get(), /*requireUnicode=*/true);
    ASSERT_FALSE(fonts.empty()) << "the page declares three fonts";

    bool sawMissing = false;
    for (const auto& f : fonts)
        if (!f.hasToUnicode) sawMissing = true;
    EXPECT_TRUE(sawMissing)
        << "the /T4 Type 3 font carries NO /ToUnicode and nothing can synthesize "
           "one for it (its /gx1../gx6 glyph names carry no Unicode); a detector "
           "that cannot see that cannot warn an operator that a search-driven "
           "redaction is unreliable on this page";
}

TEST(F18NoToUnicode, TheUnsearchableInkIsActuallyOnThePage) {
    auto ctx = std::make_shared<MuPdfContext>();
    const fs::path p = fixture("f18_no_tounicode.pdf");
    auto doc = Document::open(ctx, p);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    // Authored: 6 Type 3 glyphs at Tm (20,100), 24pt, glyph box 60..(w-60)/1000.
    // Render and count dark pixels inside the authored band; the ink must exist,
    // otherwise "search misses it" would be trivially true because there is
    // nothing there.
    const QImage img = page->render(page->boundingBox(), 2.0f);
    ASSERT_FALSE(img.isNull());
    int dark = 0;
    const int x0 = static_cast<int>(20 * 2.0);
    const int x1 = std::min(img.width(), static_cast<int>(200 * 2.0));
    // PDF y=100..140 on a 200-high page -> device y = (200-140)*2 .. (200-100)*2
    const int y0 = static_cast<int>((200 - 140) * 2.0);
    const int y1 = std::min(img.height(), static_cast<int>((200 - 100) * 2.0));
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x)
            if (qGray(img.pixel(x, y)) < 128) ++dark;
    EXPECT_GT(dark, 500)
        << "the Type 3 word must render real ink in the authored band "
           "[20..200]x[100..140]; found " << dark << " dark pixels";
}

// ═══════════════════════════════════════════════════════════════════════════
// A3-49 / A3-33 — 0-page document survives the downstream paths.
// ═══════════════════════════════════════════════════════════════════════════
TEST(F18ZeroPage, OpensAndReportsExactlyZeroPages) {
    auto ctx = std::make_shared<MuPdfContext>();
    const fs::path p = fixture("f18_zero_page.pdf");
    ASSERT_TRUE(fs::exists(p));
    auto doc = Document::open(ctx, p);
    ASSERT_NE(doc, nullptr) << "a structurally valid 0-page PDF must still open";
    EXPECT_EQ(doc->pageCount(), 0)
        << "authored /Type /Pages /Count 0 /Kids []";
}

TEST(F18ZeroPage, PageAccessorRefusesRatherThanFaults) {
    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, fixture("f18_zero_page.pdf"));
    ASSERT_NE(doc, nullptr);
    // Out-of-range page access on an empty tree must be a refusal (null or a
    // thrown PdfeError), never an access violation.
    std::unique_ptr<Page> page;
    EXPECT_NO_FATAL_FAILURE({
        try {
            page = doc->page(0);
        } catch (const std::exception&) {
            page.reset();
        }
    });
    EXPECT_EQ(page, nullptr) << "there is no page 0 in a 0-page document";
}

TEST(F18ZeroPage, SearchOverAnEmptyPageTreeReturnsZeroWithoutFaulting) {
    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, fixture("f18_zero_page.pdf"));
    ASSERT_NE(doc, nullptr);
    SearchEngine engine(doc.get());
    const SearchResult r = engine.search("anything");
    EXPECT_EQ(r.totalMatches(), 0);
    EXPECT_TRUE(r.hits.empty());
}

TEST(F18ZeroPage, SaveAsProducesAReadableZeroPageFile) {
    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, fixture("f18_zero_page.pdf"));
    ASSERT_NE(doc, nullptr);
    const fs::path out = uniqueOut("zero_page");
    std::error_code ec;
    fs::remove(out, ec);
    ASSERT_NO_THROW(doc->saveAs(out));
    ASSERT_TRUE(fs::exists(out));
    doc.reset();

    auto reopened = Document::open(ctx, out);
    ASSERT_NE(reopened, nullptr) << "the round-tripped 0-page file must reopen";
    EXPECT_EQ(reopened->pageCount(), 0)
        << "save must not invent a page for an empty page tree";
    reopened.reset();
    fs::remove(out, ec);
}

// ═══════════════════════════════════════════════════════════════════════════
// A3-51 — incremental update: latest-object-wins, and the shadowed revision is
// a confidentiality surface until a full rewrite drops it.
// ═══════════════════════════════════════════════════════════════════════════
TEST(F18IncrementalShadow, LatestObjectWinsAcrossThePrevChain) {
    auto ctx = std::make_shared<MuPdfContext>();
    const fs::path p = fixture("f18_incremental_shadow.pdf");
    ASSERT_TRUE(fs::exists(p));

    // Both byte sequences are in the file — that is what makes this fixture a
    // shadow rather than a plain edit.
    const std::string raw = readAllBytes(p);
    ASSERT_EQ(countOccurrences(raw, "ORIGINALSECRET"), 1);
    ASSERT_EQ(countOccurrences(raw, "UPDATEDPUBLIC"), 1);

    auto doc = Document::open(ctx, p);
    ASSERT_NE(doc, nullptr);
    ASSERT_EQ(doc->pageCount(), 1);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);
    const std::string text = page->extractPlainText();
    EXPECT_NE(text.find("UPDATEDPUBLIC"), std::string::npos)
        << "the revision-2 definition of object 4 must win; got '" << text << "'";
    EXPECT_EQ(text.find("ORIGINALSECRET"), std::string::npos)
        << "the shadowed revision-1 definition must NOT be reachable; got '"
        << text << "'";
}

TEST(F18IncrementalShadow, FullRewriteSaveDropsTheShadowedRevisionBytes) {
    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, fixture("f18_incremental_shadow.pdf"));
    ASSERT_NE(doc, nullptr);
    const fs::path out = uniqueOut("incr_shadow");
    std::error_code ec;
    fs::remove(out, ec);
    ASSERT_NO_THROW(doc->saveAs(out));
    doc.reset();

    // Decode every stream in the output: PDFE Flate-compresses content, so a raw
    // byte-grep would report "absent" for BOTH tokens and prove nothing.
    const std::string decoded = allDecodedStreams(ctx, out);
    EXPECT_NE(decoded.find("UPDATEDPUBLIC"), std::string::npos)
        << "the live content must survive the rewrite (positive control: if this "
           "fails the scan is broken, not the product)";
    EXPECT_EQ(decoded.find("ORIGINALSECRET"), std::string::npos)
        << "a full-rewrite save must not carry the shadowed previous revision "
           "into the output — that is a confidentiality leak, not just bloat";

    // And the shadowed bytes must not survive as raw (uncompressed) file bytes
    // either — the two checks together cover both encodings.
    const std::string rawSaved = readAllBytes(out);
    EXPECT_EQ(rawSaved.find("ORIGINALSECRET"), std::string::npos)
        << "shadowed revision-1 bytes copied verbatim into the output";
    fs::remove(out, ec);
}

// ═══════════════════════════════════════════════════════════════════════════
// A3-52 — deep nested-array recursion boundary.
// The class the backlog names is "stack overflow on deep array recursion": the
// contract is that the parser either resolves the structure or refuses it, but
// never faults. NEST_DEPTH is 200 in the generator; the page must stay usable.
// ═══════════════════════════════════════════════════════════════════════════
TEST(F18DeeplyNestedArray, ParsesWithoutFaultingAndThePageStaysUsable) {
    auto ctx = std::make_shared<MuPdfContext>();
    const fs::path p = fixture("f18_deeply_nested_array.pdf");
    ASSERT_TRUE(fs::exists(p));
    auto doc = Document::open(ctx, p);
    ASSERT_NE(doc, nullptr) << "the nested array must not prevent opening";
    ASSERT_EQ(doc->pageCount(), 1);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);
    const std::string text = page->extractPlainText();
    EXPECT_NE(text.find("NESTHOST"), std::string::npos)
        << "the page's own content must still be reachable past the nest; got '"
        << text << "'";
}

TEST(F18DeeplyNestedArray, RoundTripsThroughSaveWithoutFaulting) {
    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, fixture("f18_deeply_nested_array.pdf"));
    ASSERT_NE(doc, nullptr);
    const fs::path out = uniqueOut("deep_nest");
    std::error_code ec;
    fs::remove(out, ec);
    ASSERT_NO_THROW(doc->saveAs(out));
    doc.reset();
    auto reopened = Document::open(ctx, out);
    ASSERT_NE(reopened, nullptr);
    EXPECT_EQ(reopened->pageCount(), 1);
    auto page = reopened->page(0);
    ASSERT_NE(page, nullptr);
    EXPECT_NE(page->extractPlainText().find("NESTHOST"), std::string::npos);
    page.reset();
    reopened.reset();
    fs::remove(out, ec);
}

// ═══════════════════════════════════════════════════════════════════════════
// A3-53 / A3-33 — 10000-page scale boundary.
// ═══════════════════════════════════════════════════════════════════════════
TEST(F18TenThousandPages, ReportsTheAuthoredPageCountAndReachesBothEnds) {
    auto ctx = std::make_shared<MuPdfContext>();
    const fs::path p = fixture("f18_10000page.pdf");
    ASSERT_TRUE(fs::exists(p));
    auto doc = Document::open(ctx, p);
    ASSERT_NE(doc, nullptr);
    EXPECT_EQ(doc->pageCount(), 10000) << "authored /Count";

    auto first = doc->page(0);
    ASSERT_NE(first, nullptr);
    EXPECT_NE(first->extractPlainText().find("FIRSTPAGE"), std::string::npos);
    first.reset();

    auto last = doc->page(9999);
    ASSERT_NE(last, nullptr);
    EXPECT_NE(last->extractPlainText().find("LASTPAGE"), std::string::npos)
        << "the last page of a 10k-page tree must be reachable (an off-by-one "
           "or a truncated /Kids walk shows up exactly here)";
    last.reset();

    auto middle = doc->page(5000);
    ASSERT_NE(middle, nullptr);
    EXPECT_NE(middle->extractPlainText().find("FILLERPAGE"), std::string::npos);
}

TEST(F18TenThousandPages, PageAccessAtScaleStaysWithinAWallClockCeiling) {
    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, fixture("f18_10000page.pdf"));
    ASSERT_NE(doc, nullptr);
    ASSERT_EQ(doc->pageCount(), 10000);

    // Touch 100 pages spread across the tree. A per-access linear /Kids walk is
    // still fine here; a per-access *reparse of the whole document* is not.
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 10000; i += 100) {
        auto pg = doc->page(i);
        ASSERT_NE(pg, nullptr) << "page " << i << " unreachable";
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    EXPECT_LT(ms, 60000) << "100 spread page loads over a 10k-page document took "
                         << ms << " ms";
}

TEST(F18TenThousandPages, OutOfRangeIndexIsRefusedNotFaulted) {
    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, fixture("f18_10000page.pdf"));
    ASSERT_NE(doc, nullptr);
    std::unique_ptr<Page> pg;
    try {
        pg = doc->page(10000);
    } catch (const std::exception&) {
        pg.reset();
    }
    EXPECT_EQ(pg, nullptr) << "index 10000 is one past the authored last page";
}

// ═══════════════════════════════════════════════════════════════════════════
// A3-50 — the ORPHAN fixture. tests/fixtures/phase3_cjk_text.pdf was committed,
// inventoried, and never consumed by a single .cpp (an R-34 orphan instance).
// This is its consumer: CJK text must survive an open -> save -> reopen cycle
// byte-identically at the *character* level, which is the content-preservation
// property the backlog row asks for.
// ═══════════════════════════════════════════════════════════════════════════
TEST(F18CjkContentPreservation, CjkTextSurvivesSaveAndReopen) {
    const fs::path p = fixture("phase3_cjk_text.pdf");
    ASSERT_TRUE(fs::exists(p))
        << "orphan fixture phase3_cjk_text.pdf must be present: " << p;

    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, p);
    ASSERT_NE(doc, nullptr);
    ASSERT_GT(doc->pageCount(), 0);

    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);
    const std::string before = page->extractPlainText();
    page.reset();

    // The fixture must actually contain non-ASCII (CJK) text — otherwise this
    // oracle would pass on any document and prove nothing.
    bool hasNonAscii = false;
    for (unsigned char c : before)
        if (c >= 0x80) hasNonAscii = true;
    ASSERT_TRUE(hasNonAscii)
        << "phase3_cjk_text.pdf must extract non-ASCII text; got '" << before
        << "'";

    const fs::path out = uniqueOut("cjk");
    std::error_code ec;
    fs::remove(out, ec);
    ASSERT_NO_THROW(doc->saveAs(out));
    doc.reset();

    auto reopened = Document::open(ctx, out);
    ASSERT_NE(reopened, nullptr);
    auto rpage = reopened->page(0);
    ASSERT_NE(rpage, nullptr);
    const std::string after = rpage->extractPlainText();
    rpage.reset();
    reopened.reset();
    fs::remove(out, ec);

    EXPECT_EQ(after, before)
        << "CJK page text must round-trip unchanged through save+reopen\n"
        << "before: " << before << "\nafter : " << after;
}
