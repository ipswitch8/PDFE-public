// -*- coding: utf-8 -*-
//
// R3-07 — TEXT-EXTRACTION FIDELITY correctness oracle (RED-first).
//
// WHY THIS FILE EXISTS
// --------------------
// The existing tests/test_text_extraction.cpp only asserts that extraction is
// non-empty and that quads/bboxes have a sane SHAPE (width/height >= 0). Its
// prime target, `PlainTextMatchesStructured` (~line 215-240), is VACUOUS: it
// only checks that both `extractPlainText()` and the block concatenation are
// non-empty — it never pins the CONTENT, the READING ORDER, ligature
// normalization, or script/block preservation. A mutation that reverses block
// order, reads multi-column pages row-major, drops ligature normalization, or
// loses non-Latin scripts would sail through it GREEN.
//
// This file replaces those vacuous checks with concrete, deterministic oracles
// that each kill a specific real mutation:
//   * MULTI-COLUMN READING ORDER — a word at the BOTTOM of the LEFT column must
//     appear in reading order BEFORE a word at the TOP of the RIGHT column, even
//     though the right-column word is physically HIGHER on the page. This is the
//     definition of column-major reading; a row-major extractor fails it.
//   * LIGATURE NORMALIZATION — glyphs drawn from ligature codepoints must extract
//     as their ASCII decomposition ("fi"/"fl"), NOT as U+FB0x presentation forms,
//     so search/copy/accessibility see real letters.
//   * RTL SCRIPT PRESERVATION — Arabic/Hebrew text must survive extraction as
//     characters in the correct Unicode BLOCK (base or presentation-form ranges),
//     not be dropped or mangled to Latin/'?'.
//
// Coordinate note (verified against src/core/TextExtractor.cpp): quads/bboxes are
// carried straight from MuPDF stext page coordinates — origin TOP-left, x grows
// RIGHTWARD, y grows DOWNWARD. So a smaller QRectF::top() == physically HIGHER on
// the page, and a larger QRectF::left() == further RIGHT.

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/Page.h"
#include "core/TextExtractor.h"
#include "core/TextBlock.h"
#include "core/TextLine.h"
#include "core/TextChar.h"
#include "core/MuPdfContext.h"
#include "core/ToUnicodeRecovery.h"

#include <QRectF>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace pdfe::core;

namespace {

// --- UTF-8 -> Unicode codepoints (self-contained; no external deps) ----------
std::vector<char32_t> toCodepoints(const std::string& utf8) {
    std::vector<char32_t> cps;
    size_t i = 0;
    const size_t n = utf8.size();
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(utf8[i]);
        char32_t cp = 0;
        int extra = 0;
        if (c < 0x80) { cp = c; extra = 0; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
        else { ++i; continue; }  // invalid lead byte — skip
        if (i + static_cast<size_t>(extra) >= n) break;
        bool ok = true;
        for (int k = 1; k <= extra; ++k) {
            const unsigned char cc = static_cast<unsigned char>(utf8[i + k]);
            if ((cc & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (!ok) { ++i; continue; }
        cps.push_back(cp);
        i += static_cast<size_t>(extra) + 1;
    }
    return cps;
}

bool containsCodepoint(const std::string& utf8, char32_t target) {
    for (char32_t cp : toCodepoints(utf8)) if (cp == target) return true;
    return false;
}

// Bounding box (in stext page coords) of the first extracted LINE whose text
// contains `needle`. Uses the structured hierarchy so the geometry cross-check
// is independent of the plain-text string search.
std::optional<QRectF> lineBBoxContaining(const std::vector<TextBlock>& blocks,
                                         const std::string& needle) {
    for (const auto& block : blocks) {
        if (block.type == TextBlockType::Image) continue;
        for (const auto& line : block.lines) {
            if (line.text().find(needle) != std::string::npos) {
                return line.boundingBox;
            }
        }
    }
    return std::nullopt;
}

// Trim ASCII whitespace from both ends (some extracted lines carry a trailing
// space, e.g. "Suite 200 "); used for exact-cell matching in the table oracles.
std::string trimmed(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\n' || s[b] == '\r')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\n' || s[e - 1] == '\r')) --e;
    return s.substr(b, e - b);
}

// Bounding box (stext page coords) of the first extracted LINE whose text, once
// trimmed, EXACTLY equals `target`. Used for table-cell anchors where a substring
// match would be ambiguous (e.g. "Amount" vs "Amount Paid"/"Amount Due").
std::optional<QRectF> lineBBoxWhereTrimmedEquals(const std::vector<TextBlock>& blocks,
                                                 const std::string& target) {
    for (const auto& block : blocks) {
        if (block.type == TextBlockType::Image) continue;
        for (const auto& line : block.lines) {
            if (trimmed(line.text()) == target) {
                return line.boundingBox;
            }
        }
    }
    return std::nullopt;
}

std::filesystem::path fixturesDir() {
    return std::filesystem::path(TEST_FIXTURES_DIR);
}

// multicolumn.pdf lives in <repo>/test_PDFs, not tests/fixtures. TEST_FIXTURES_DIR
// == <repo>/tests/fixtures, so climb two levels to reach the repo root.
std::filesystem::path testPdfsDir() {
    return fixturesDir().parent_path().parent_path() / "test_PDFs";
}

}  // namespace

class FidelityR3_07_TextExtraction : public ::testing::Test {
protected:
    void SetUp() override {
        context = std::make_shared<MuPdfContext>();
    }
    std::shared_ptr<MuPdfContext> context;
};

// -----------------------------------------------------------------------------
// MULTI-COLUMN READING ORDER — column-major, not row-major.
//
// Fixture: test_PDFs/multicolumn.pdf (real 2-column Lorem Ipsum doc, page 0).
// Ground-truth geometry (from the fixture): the unique word "libero." sits near
// the BOTTOM of the LEFT column (stext y0 ~= 653), while the unique word "quam."
// sits near the TOP of the RIGHT column (stext y0 ~= 260, x0 ~= 433). The right
// word is therefore physically HIGHER and further RIGHT than the left word.
//
// Column-major reading (correct) emits the entire left column top-to-bottom
// BEFORE starting the right column, so "libero." precedes "quam.". A row-major
// extractor (scan each visual row left-to-right across both columns) would emit
// the higher "quam." FIRST — reversing the order and failing this test.
// -----------------------------------------------------------------------------
TEST_F(FidelityR3_07_TextExtraction, MultiColumnReadingOrderIsColumnMajor) {
    const std::filesystem::path pdf = testPdfsDir() / "multicolumn.pdf";
    if (!std::filesystem::exists(pdf)) {
        GTEST_SKIP() << "Fixture missing (not a pass): " << pdf;
    }

    auto document = Document::open(context, pdf);
    ASSERT_NE(document, nullptr);
    ASSERT_GT(document->pageCount(), 0);
    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    const std::string plain = page->extractPlainText();
    ASSERT_FALSE(plain.empty());

    // Unique anchor words (whole-page substring count == 1 in this fixture).
    const std::string headerWord = "Two-Column";  // spans full width, comes first
    const std::string leftLowWord = "libero.";    // bottom of LEFT column
    const std::string leftLowWord2 = "tristique,"; // bottom of LEFT column
    const std::string rightHighWord = "quam.";     // top of RIGHT column

    const size_t posHeader = plain.find(headerWord);
    const size_t posLeftLow = plain.find(leftLowWord);
    const size_t posLeftLow2 = plain.find(leftLowWord2);
    const size_t posRightHigh = plain.find(rightHighWord);

    ASSERT_NE(posHeader, std::string::npos) << "Anchor '" << headerWord << "' not extracted";
    ASSERT_NE(posLeftLow, std::string::npos) << "Anchor '" << leftLowWord << "' not extracted";
    ASSERT_NE(posLeftLow2, std::string::npos) << "Anchor '" << leftLowWord2 << "' not extracted";
    ASSERT_NE(posRightHigh, std::string::npos) << "Anchor '" << rightHighWord << "' not extracted";

    // (1) The full-width header/title precedes the two-column body.
    EXPECT_LT(posHeader, posLeftLow)
        << "Header should be read before left-column body text";

    // (2) CORE ORACLE: the bottom-of-LEFT-column word precedes the
    //     top-of-RIGHT-column word — column-major reading order. A row-major
    //     extractor would place the visually higher '" << rightHighWord << "' first.
    EXPECT_LT(posLeftLow, posRightHigh)
        << "Reading order is NOT column-major: bottom-of-left-column '" << leftLowWord
        << "' (pos " << posLeftLow << ") must precede top-of-right-column '"
        << rightHighWord << "' (pos " << posRightHigh << "). A row-major (across-columns) "
        << "extractor fails here.";

    // (3) Second independent left/right anchor pair, same property.
    EXPECT_LT(posLeftLow2, posRightHigh)
        << "'" << leftLowWord2 << "' (left col) must precede '" << rightHighWord
        << "' (right col) under column-major reading";

    // (4) GEOMETRY CROSS-CHECK — prove the anchors really are in different
    //     columns and that the earlier-in-reading-order word is the LOWER one.
    //     This confirms the ordering assertion above is genuinely testing
    //     column-major behaviour and not a coincidence of the text stream.
    const std::vector<TextBlock> blocks = page->extractText();
    ASSERT_FALSE(blocks.empty());

    const auto leftBox = lineBBoxContaining(blocks, leftLowWord);
    const auto rightBox = lineBBoxContaining(blocks, rightHighWord);
    ASSERT_TRUE(leftBox.has_value()) << "Could not locate line bbox for '" << leftLowWord << "'";
    ASSERT_TRUE(rightBox.has_value()) << "Could not locate line bbox for '" << rightHighWord << "'";

    // Right-column anchor is further RIGHT (x grows rightward in stext coords).
    EXPECT_GT(rightBox->left(), leftBox->left())
        << "Right-column anchor '" << rightHighWord << "' (left=" << rightBox->left()
        << ") should be physically to the right of left-column anchor '" << leftLowWord
        << "' (left=" << leftBox->left() << ")";

    // Right-column anchor is physically HIGHER (smaller top() == higher; y grows
    // downward in stext coords). Together with (2) this is the row-major killer:
    // the higher-and-more-right word still reads LATER.
    EXPECT_LT(rightBox->top(), leftBox->top())
        << "Right-column anchor '" << rightHighWord << "' (top=" << rightBox->top()
        << ") should sit HIGHER on the page than left-column anchor '" << leftLowWord
        << "' (top=" << leftBox->top() << "), yet it is read later — that is column-major.";
}

// -----------------------------------------------------------------------------
// LIGATURE NORMALIZATION — extracted text must be ASCII, not presentation forms.
//
// Fixture: tests/fixtures/text_ligature.pdf. Drawn words include "office" (an
// "ffi"/"fi" ligature source) and "flourish" (an "fl" ligature source). Correct
// extraction (via the font's ToUnicode) yields ASCII letters, so search and copy
// see "fi"/"fl". A regression that surfaces the raw glyph as U+FB01 (fi), U+FB02
// (fl), U+FB03 (ffi) etc. breaks all downstream text handling.
// -----------------------------------------------------------------------------
TEST_F(FidelityR3_07_TextExtraction, LigaturesNormalizeToAscii) {
    const std::filesystem::path pdf = fixturesDir() / "text_ligature.pdf";
    if (!std::filesystem::exists(pdf)) {
        GTEST_SKIP() << "Fixture missing (not a pass): " << pdf;
    }

    auto document = Document::open(context, pdf);
    ASSERT_NE(document, nullptr);
    ASSERT_GT(document->pageCount(), 0);
    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    const std::string plain = page->extractPlainText();
    ASSERT_FALSE(plain.empty());

    // ASCII decomposition is present.
    EXPECT_NE(plain.find("fi"), std::string::npos)
        << "Expected ASCII 'fi' (from 'office') in extracted text: [" << plain << "]";
    EXPECT_NE(plain.find("fl"), std::string::npos)
        << "Expected ASCII 'fl' (from 'flourish') in extracted text: [" << plain << "]";
    EXPECT_NE(plain.find("office"), std::string::npos)
        << "Expected the whole ASCII word 'office' in extracted text: [" << plain << "]";
    EXPECT_NE(plain.find("flourish"), std::string::npos)
        << "Expected the whole ASCII word 'flourish' in extracted text: [" << plain << "]";

    // No Latin ligature presentation forms leaked through (U+FB00..U+FB06 cover
    // ff, fi, fl, ffi, ffl, long-s-t, st). The named targets are U+FB01 / U+FB03.
    EXPECT_FALSE(containsCodepoint(plain, 0xFB01))
        << "Extracted text still contains U+FB01 (fi ligature) — normalization missing";
    EXPECT_FALSE(containsCodepoint(plain, 0xFB03))
        << "Extracted text still contains U+FB03 (ffi ligature) — normalization missing";
    for (char32_t lig = 0xFB00; lig <= 0xFB06; ++lig) {
        EXPECT_FALSE(containsCodepoint(plain, lig))
            << "Extracted text contains ligature presentation form U+"
            << std::hex << static_cast<uint32_t>(lig)
            << " — expected ASCII decomposition instead";
    }
}

// -----------------------------------------------------------------------------
// RTL SCRIPT PRESERVATION — Arabic / Hebrew survive extraction in-block.
//
// Fixture: tests/fixtures/phase3_rtl_arabic.pdf (its .expected.json warns that
// Arabic may extract as presentation forms — still within the Arabic Unicode
// block). We therefore assert on Unicode BLOCK MEMBERSHIP, not exact source
// codepoints, and we do NOT assert visual order for RTL. A mutation that drops
// non-Latin scripts or replaces them with '?' fails this.
//   Arabic base:               U+0600..U+06FF
//   Arabic presentation-A:     U+FB50..U+FDFF
//   Arabic presentation-B:     U+FE70..U+FEFF
//   Hebrew:                    U+0590..U+05FF
// -----------------------------------------------------------------------------
TEST_F(FidelityR3_07_TextExtraction, RtlScriptsPreservedByUnicodeBlock) {
    const std::filesystem::path pdf = fixturesDir() / "phase3_rtl_arabic.pdf";
    if (!std::filesystem::exists(pdf)) {
        GTEST_SKIP() << "Fixture missing (not a pass): " << pdf;
    }

    auto document = Document::open(context, pdf);
    ASSERT_NE(document, nullptr);
    ASSERT_GT(document->pageCount(), 0);
    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    const std::string plain = page->extractPlainText();
    ASSERT_FALSE(plain.empty());

    const std::vector<char32_t> cps = toCodepoints(plain);

    auto inArabicBlock = [](char32_t c) {
        return (c >= 0x0600 && c <= 0x06FF) ||  // Arabic base
               (c >= 0xFB50 && c <= 0xFDFF) ||  // Arabic Presentation Forms-A
               (c >= 0xFE70 && c <= 0xFEFF);     // Arabic Presentation Forms-B
    };
    auto inHebrewBlock = [](char32_t c) {
        return c >= 0x0590 && c <= 0x05FF;
    };

    int arabicCount = 0;
    int hebrewCount = 0;
    for (char32_t c : cps) {
        if (inArabicBlock(c)) ++arabicCount;
        if (inHebrewBlock(c)) ++hebrewCount;
    }

    // Sanity: the fixture also carries the ASCII labels "Arabic"/"Hebrew".
    EXPECT_NE(plain.find("Arabic"), std::string::npos)
        << "ASCII label 'Arabic' should extract";
    EXPECT_NE(plain.find("Hebrew"), std::string::npos)
        << "ASCII label 'Hebrew' should extract";

    // Core oracle: real Arabic and Hebrew characters survived, in their blocks.
    EXPECT_GT(arabicCount, 0)
        << "No characters in the Arabic Unicode block(s) survived extraction — "
        << "RTL Arabic text was dropped or mangled";
    EXPECT_GT(hebrewCount, 0)
        << "No characters in the Hebrew Unicode block survived extraction — "
        << "RTL Hebrew text was dropped or mangled";

    // Replacement chars must not DOMINATE the RTL runs. This fixture's own
    // .expected.json warns Arabic may extract as presentation forms, and a
    // handful of glyphs in its embedded subset legitimately lack a clean
    // Unicode mapping (yielding U+FFFD) — that is a font-subset artifact, not a
    // PDFE regression. The real oracle is that genuine script codepoints
    // survived and outnumber any replacement chars, i.e. the script data was
    // not wholesale lost to '?'.
    int replacementCount = 0;
    for (char32_t c : cps) if (c == 0xFFFD) ++replacementCount;
    EXPECT_GT(arabicCount + hebrewCount, replacementCount)
        << "U+FFFD replacement characters (" << replacementCount << ") equal or "
        << "outnumber real Arabic+Hebrew script characters ("
        << (arabicCount + hebrewCount) << ") — extraction lost script data";
}

// =============================================================================
// TABLE-STRUCTURE RECONSTRUCTION  (R3-07 acceptance-criteria extension)
//
// Covered by TWO distinct fixtures so the contract is not pinned on a single
// synthetic case:
//   (1) table_extract.pdf        — deterministic 3x3 synthetic grid at known
//                                   coordinates (gen_table_extract.py). Exact,
//                                   token-encoded (R<r>C<c>), so every reading-
//                                   order and geometry relation is unambiguous.
//   (2) invoice_vector_heading.pdf — a REAL invoice document whose line-item
//                                   table header row (Qty | Item | Description |
//                                   Unit Price | Amount) is a genuine, real-world
//                                   table shape.
//
// The correct, defensible table reading contract (verified EMPIRICALLY against
// MuPDF stext before writing these oracles — see the per-fixture geometry notes
// below) is:
//   * cells in the same visual ROW share a y-band and extract LEFT-TO-RIGHT in
//     column order;
//   * the document reads ROW-MAJOR: every cell of an upper row precedes every
//     cell of a lower row (NOT column-major "read straight down each column").
// =============================================================================

// -----------------------------------------------------------------------------
// (1) Synthetic 3x3 grid — exact reading order + row/column geometry.
//
// Empirical MuPDF stext output for table_extract.pdf (origin TOP-left, y DOWN):
//   plain text : "R0C0\nR0C1\nR0C2\nR1C0\nR1C1\nR1C2\nR2C0\nR2C1\nR2C2\n"
//   structure  : 3 blocks (one per ROW, block bbox spans the full width), each
//                block holds 3 lines (one per cell) ordered left-to-right; so
//                the block->line traversal order == row-major.
//   geometry   : row tops ~= {79.1, 129.1, 179.1}; column lefts ~= {100,280,460}.
//
// MUTATIONS THIS KILLS:
//   * column-major mis-extraction (read down each column: R0C0,R1C0,R2C0,…) —
//     killed by "every row-0 cell precedes every row-1 cell" (posR0C2 < posR1C0).
//   * reversed column order within a row — killed by posR<r>C0 < posR<r>C1 < posR<r>C2.
//   * reversed row order (bottom-up) — killed by posR0C0 < posR1C0 < posR2C0.
//   * a block-order shuffle that breaks row grouping — killed by the geometry
//     cross-check (same-row cells share a y-band; same-column cells share x).
// -----------------------------------------------------------------------------
TEST_F(FidelityR3_07_TextExtraction, TableRowColumnStructure) {
    const std::filesystem::path pdf = fixturesDir() / "table_extract.pdf";
    if (!std::filesystem::exists(pdf)) {
        GTEST_SKIP() << "Fixture missing (not a pass): " << pdf;
    }

    auto document = Document::open(context, pdf);
    ASSERT_NE(document, nullptr);
    ASSERT_GT(document->pageCount(), 0);
    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    const std::string plain = page->extractPlainText();
    ASSERT_FALSE(plain.empty());

    // Row-major token sequence: R0C0, R0C1, R0C2, R1C0, … R2C2.
    std::vector<std::string> rowMajor;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            rowMajor.push_back("R" + std::to_string(r) + "C" + std::to_string(c));

    std::vector<size_t> pos;
    for (const auto& tok : rowMajor) {
        const size_t p = plain.find(tok);
        ASSERT_NE(p, std::string::npos) << "Table cell '" << tok << "' not extracted";
        pos.push_back(p);
    }

    // CORE ORACLE: reading order is STRICTLY row-major. Because the tokens are
    // unique and encode (row,col), a single strictly-increasing chain pins the
    // entire table order in one shot: left-to-right within a row AND top-to-
    // bottom across rows. Any column-major or reversed extraction breaks it.
    for (size_t i = 1; i < pos.size(); ++i) {
        EXPECT_LT(pos[i - 1], pos[i])
            << "Table reading order broken at '" << rowMajor[i - 1] << "' -> '"
            << rowMajor[i] << "': expected row-major (row 0 fully, then row 1, "
            << "then row 2; each row left-to-right). A column-major or reversed "
            << "extractor fails here.";
    }

    // The row-0 -> row-1 boundary is the specific killer for a column-major
    // extractor (which would emit R0C0,R1C0,R2C0,R0C1,… and place R1C0 before
    // R0C2). State it explicitly.
    EXPECT_LT(plain.find("R0C2"), plain.find("R1C0"))
        << "Last cell of row 0 (R0C2) must precede first cell of row 1 (R1C0) — "
        << "this is the row-major vs column-major discriminator.";

    // GEOMETRY CROSS-CHECK — prove the reading order above tracks the real grid.
    const std::vector<TextBlock> blocks = page->extractText();
    ASSERT_FALSE(blocks.empty());

    QRectF box[3][3];
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            const auto b = lineBBoxContaining(blocks, "R" + std::to_string(r) + "C" + std::to_string(c));
            ASSERT_TRUE(b.has_value())
                << "No line bbox for cell R" << r << "C" << c;
            box[r][c] = *b;
        }
    }

    constexpr double kBand = 6.0;   // same-row / same-column tolerance (identical
                                    // baselines extract to identical tops/lefts)
    constexpr double kGap = 20.0;   // minimum separation between distinct rows/cols

    // Same ROW -> shared y-band; ROWS increase downward (top grows with row idx).
    for (int r = 0; r < 3; ++r) {
        EXPECT_NEAR(box[r][0].top(), box[r][1].top(), kBand)
            << "Row " << r << " cells C0/C1 not in the same y-band";
        EXPECT_NEAR(box[r][1].top(), box[r][2].top(), kBand)
            << "Row " << r << " cells C1/C2 not in the same y-band";
    }
    EXPECT_LT(box[0][0].top() + kGap, box[1][0].top())
        << "Row 0 should sit ABOVE row 1 (smaller top in stext coords)";
    EXPECT_LT(box[1][0].top() + kGap, box[2][0].top())
        << "Row 1 should sit ABOVE row 2";

    // Same COLUMN -> shared x; COLUMNS increase rightward (left grows with col idx).
    for (int c = 0; c < 3; ++c) {
        EXPECT_NEAR(box[0][c].left(), box[1][c].left(), kBand)
            << "Column " << c << " cells R0/R1 not left-aligned (same column)";
        EXPECT_NEAR(box[1][c].left(), box[2][c].left(), kBand)
            << "Column " << c << " cells R1/R2 not left-aligned (same column)";
    }
    EXPECT_LT(box[0][0].left() + kGap, box[0][1].left())
        << "Column 0 should sit LEFT of column 1";
    EXPECT_LT(box[0][1].left() + kGap, box[0][2].left())
        << "Column 1 should sit LEFT of column 2";
}

// -----------------------------------------------------------------------------
// (2) REAL invoice line-item table — header-row column structure.
//
// Empirical MuPDF stext output for invoice_vector_heading.pdf (the real
// Examplecorp invoice, origin TOP-left): the line-item header row extracts as
// five separate lines that SHARE the baseline y0 ~= 256.3 and march left-to-
// right by x:
//     Qty (x~=20)  Item (x~=75)  Description (x~=263)  Unit Price (x~=455)  Amount (x~=528)
// and in plain-text reading order they appear consecutively, before the first
// data row's part-number cell "BX7N7U8#ABA".
//
// MUTATIONS THIS KILLS (on a real document, not a toy grid):
//   * dropping/re-ordering table header cells — killed by the exact-cell
//     presence + left-to-right ordering asserts.
//   * a reader that walks columns instead of the header row — killed by all five
//     header cells sharing ONE y-band while their x strictly increases.
//   * header/body row confusion — killed by "header row precedes the data cell".
// -----------------------------------------------------------------------------
TEST_F(FidelityR3_07_TextExtraction, TableRowColumnStructureRealInvoice) {
    const std::filesystem::path pdf = fixturesDir() / "invoice_vector_heading.pdf";
    if (!std::filesystem::exists(pdf)) {
        GTEST_SKIP() << "Fixture missing (not a pass): " << pdf;
    }

    auto document = Document::open(context, pdf);
    ASSERT_NE(document, nullptr);
    ASSERT_GT(document->pageCount(), 0);
    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    const std::vector<TextBlock> blocks = page->extractText();
    ASSERT_FALSE(blocks.empty());

    // The five header cells, in their intended left-to-right column order.
    const std::vector<std::string> headers = {
        "Qty", "Item", "Description", "Unit Price", "Amount"};

    std::vector<QRectF> hbox;
    for (const auto& h : headers) {
        const auto b = lineBBoxWhereTrimmedEquals(blocks, h);
        ASSERT_TRUE(b.has_value())
            << "Invoice table header cell '" << h << "' not extracted as its own line";
        hbox.push_back(*b);
    }

    // All header cells share ONE y-band == they are the SAME table row.
    constexpr double kBand = 6.0;
    for (size_t i = 1; i < hbox.size(); ++i) {
        EXPECT_NEAR(hbox[0].top(), hbox[i].top(), kBand)
            << "Header cell '" << headers[i] << "' (top=" << hbox[i].top()
            << ") is not in the header row's y-band (top=" << hbox[0].top()
            << ") — table row grouping is wrong";
    }

    // Column order: x strictly increases across the header, left-to-right.
    for (size_t i = 1; i < hbox.size(); ++i) {
        EXPECT_LT(hbox[i - 1].left(), hbox[i].left())
            << "Header column order broken: '" << headers[i - 1] << "' (left="
            << hbox[i - 1].left() << ") should be LEFT of '" << headers[i]
            << "' (left=" << hbox[i].left() << ")";
    }

    // Plain-text reading order: the header row reads left-to-right and precedes
    // the first data row's (unique) part-number cell.
    const std::string plain = page->extractPlainText();
    ASSERT_FALSE(plain.empty());
    const size_t pQty   = plain.find("Qty");
    const size_t pItem  = plain.find("Item");
    const size_t pDesc  = plain.find("Description");
    const size_t pUnit  = plain.find("Unit Price");
    const size_t pAmt   = plain.find("Amount");         // header "Amount"
    const size_t pData  = plain.find("BX7N7U8#ABA");    // first data-row part no.
    ASSERT_NE(pQty, std::string::npos);
    ASSERT_NE(pItem, std::string::npos);
    ASSERT_NE(pDesc, std::string::npos);
    ASSERT_NE(pUnit, std::string::npos);
    ASSERT_NE(pAmt, std::string::npos);
    ASSERT_NE(pData, std::string::npos);

    EXPECT_LT(pQty, pItem)  << "Header 'Qty' must read before 'Item'";
    EXPECT_LT(pItem, pDesc) << "Header 'Item' must read before 'Description'";
    EXPECT_LT(pDesc, pUnit) << "Header 'Description' must read before 'Unit Price'";
    EXPECT_LT(pUnit, pAmt)  << "Header 'Unit Price' must read before 'Amount'";
    EXPECT_LT(pAmt, pData)
        << "The whole header row must read BEFORE the first data row (part no.) — "
        << "row grouping / top-to-bottom order is wrong";
}

// =============================================================================
// HYPHENATION HANDLING  (R3-07 acceptance-criteria extension)
//
// Fixture: tests/fixtures/text_ligature.pdf, whose last two lines are the soft
// line-break of "cooperation" authored as "cooper-" / "ation".
//
// EMPIRICALLY VERIFIED CONTRACT (MuPDF stext, PDFE's pdfeStextOptions which do
// NOT set FZ_STEXT_DEHYPHENATE):
//   plain text = "office\nflourish\ncooper-\nation\n"
// i.e. the authored hyphen-minus (U+002D) is PRESERVED at the end of the first
// fragment and the two fragments stay on their own lines; the extractor does
// NOT silently de-hyphenate into a single "cooperation".
//
// WHY THIS IS THE CORRECT, DEFENSIBLE BEHAVIOUR FOR PDFE (a PDF *editor*):
// faithful extraction of the DISPLAYED glyphs is required so that search /
// Replace-All / in-place text editing operate on exactly what the content
// stream draws and what the user sees. De-hyphenation is a lossy reflow
// heuristic: it cannot distinguish a soft line-break hyphen from a genuine
// hyphenated compound ("well-being", "e-mail", "co-operate"), and joining
// would desynchronise extracted character offsets from the content-stream
// glyphs the editor must target. So PDFE preserves the authored hyphen; the
// hyphenated word remains fully RECOVERABLE from its two fragments, but is not
// fabricated into a form that was never drawn.
//
// MUTATIONS THIS KILLS:
//   * turning on FZ_STEXT_DEHYPHENATE (or any post-process that strips the
//     trailing hyphen and joins) — that produces "cooperation" and drops
//     "cooper-", failing every assertion below.
//   * dropping the hyphen glyph on a wrapped line — the first fragment would no
//     longer end in '-'.
// -----------------------------------------------------------------------------
TEST_F(FidelityR3_07_TextExtraction, HyphenationPreservesAuthoredHyphen) {
    const std::filesystem::path pdf = fixturesDir() / "text_ligature.pdf";
    if (!std::filesystem::exists(pdf)) {
        GTEST_SKIP() << "Fixture missing (not a pass): " << pdf;
    }

    auto document = Document::open(context, pdf);
    ASSERT_NE(document, nullptr);
    ASSERT_GT(document->pageCount(), 0);
    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    const std::string plain = page->extractPlainText();
    ASSERT_FALSE(plain.empty());

    // Both fragments are present and the word is RECOVERABLE from them.
    const size_t posFrag1 = plain.find("cooper-");
    const size_t posFrag2 = plain.find("ation");
    ASSERT_NE(posFrag1, std::string::npos)
        << "First hyphenated fragment 'cooper-' (with its authored hyphen) missing: ["
        << plain << "]";
    ASSERT_NE(posFrag2, std::string::npos)
        << "Continuation fragment 'ation' missing: [" << plain << "]";

    // The authored hyphen-minus is PRESERVED (not stripped by de-hyphenation).
    EXPECT_TRUE(containsCodepoint(plain, 0x002D))
        << "Authored soft-break hyphen (U+002D) was not preserved: [" << plain << "]";

    // The extractor did NOT silently de-hyphenate into a single joined word.
    // (This is the FZ_STEXT_DEHYPHENATE / join-heuristic mutation killer.)
    EXPECT_EQ(plain.find("cooperation"), std::string::npos)
        << "Extractor produced a de-hyphenated 'cooperation' — PDFE must preserve "
        << "the authored hyphen/glyphs for faithful search/replace/edit, not "
        << "reflow-join the fragments: [" << plain << "]";

    // Reading order: the first fragment precedes its continuation.
    EXPECT_LT(posFrag1, posFrag2)
        << "'cooper-' must read before its continuation 'ation'";

    // STRUCTURED cross-check: the wrapped word is two lines — a line ENDING in
    // the hyphen sits directly ABOVE a line beginning the continuation, left-
    // aligned (same column). This proves the preserved hyphen is a real end-of-
    // line glyph on the upper fragment, not stray text.
    const std::vector<TextBlock> blocks = page->extractText();
    ASSERT_FALSE(blocks.empty());

    const auto frag1Box = lineBBoxContaining(blocks, "cooper-");
    const auto frag2Box = lineBBoxContaining(blocks, "ation");
    ASSERT_TRUE(frag1Box.has_value()) << "No line bbox for 'cooper-'";
    ASSERT_TRUE(frag2Box.has_value()) << "No line bbox for 'ation'";

    // Find the actual line objects to inspect the terminal glyph + alignment.
    const TextLine* frag1Line = nullptr;
    for (const auto& block : blocks) {
        if (block.type == TextBlockType::Image) continue;
        for (const auto& line : block.lines) {
            if (line.text().find("cooper-") != std::string::npos) {
                frag1Line = &line;
                break;
            }
        }
        if (frag1Line) break;
    }
    ASSERT_NE(frag1Line, nullptr);
    ASSERT_FALSE(frag1Line->chars.empty());

    // The upper fragment's LAST glyph is the authored hyphen-minus.
    EXPECT_EQ(frag1Line->chars.back().codepoint, static_cast<char32_t>(0x002D))
        << "The wrapped 'cooper-' line must END with the authored hyphen (U+002D); "
        << "a de-hyphenating extractor drops it.";

    // Continuation sits BELOW the first fragment (larger top in stext coords) and
    // is left-aligned with it (same wrap column).
    EXPECT_GT(frag2Box->top(), frag1Box->top())
        << "'ation' should be on the line BELOW 'cooper-'";
    EXPECT_NEAR(frag2Box->left(), frag1Box->left(), 6.0)
        << "'ation' should wrap left-aligned under 'cooper-' (same column)";
}

// =============================================================================
// p84-remaining-open phase 7 — the CHARACTER/ENCODING extraction group.
//
// Four contracts that had no on-disk oracle at all: vertical writing mode,
// missing-/ToUnicode recovery, the soft hyphen, and simple-font encoding.
// Every fixture below is pinned in the CMake fixture floor
// (tests/CMakeLists.txt), so these tests ASSERT on presence rather than
// GTEST_SKIP — a skip that reads as green is the exact false positive this
// suite keeps finding.
// =============================================================================

namespace {

/// Every extracted character on the page, in extraction (reading) order, with
/// the writing mode of the line it came from. Flattening keeps the oracles
/// below independent of how MuPDF happens to group a run into lines — which
/// matters for vertical text, where each glyph is its own stext line.
struct FlatChar {
    char32_t cp;
    QRectF quad;
    int writingMode;
};

std::vector<FlatChar> flattenChars(const std::vector<TextBlock>& blocks) {
    std::vector<FlatChar> out;
    for (const auto& block : blocks) {
        if (block.type == TextBlockType::Image) continue;
        for (const auto& line : block.lines)
            for (const auto& ch : line.chars)
                out.push_back(FlatChar{ch.codepoint, ch.quad, line.writingMode});
    }
    return out;
}

/// The subset of `chars` whose line carried `mode`, order preserved.
std::vector<FlatChar> charsWithWritingMode(const std::vector<FlatChar>& chars,
                                           int mode) {
    std::vector<FlatChar> out;
    for (const auto& c : chars)
        if (c.writingMode == mode) out.push_back(c);
    return out;
}

/// The codepoints of one line, as a vector (so an exact sequence can be
/// compared in one assertion instead of char-by-char).
std::vector<char32_t> lineCodepoints(const TextLine& line) {
    std::vector<char32_t> out;
    out.reserve(line.chars.size());
    for (const auto& ch : line.chars) out.push_back(ch.codepoint);
    return out;
}

/// The first extracted line whose codepoint sequence STARTS with `prefix`.
/// Used where a line must be identified by content that is not ASCII and so
/// cannot be found with a std::string search.
const TextLine* lineStartingWith(const std::vector<TextBlock>& blocks,
                                 const std::vector<char32_t>& prefix) {
    for (const auto& block : blocks) {
        if (block.type == TextBlockType::Image) continue;
        for (const auto& line : block.lines) {
            const std::vector<char32_t> cps = lineCodepoints(line);
            if (cps.size() < prefix.size()) continue;
            if (std::equal(prefix.begin(), prefix.end(), cps.begin()))
                return &line;
        }
    }
    return nullptr;
}

std::string describe(const std::vector<char32_t>& cps) {
    std::string s;
    char buf[16];
    for (char32_t c : cps) {
        std::snprintf(buf, sizeof(buf), "U+%04X ", static_cast<unsigned>(c));
        s += buf;
    }
    return s;
}

}  // namespace

// -----------------------------------------------------------------------------
// #22 / PDF-71 — VERTICAL WRITING (tategaki): order AND the surfaced mode.
//
// Fixture: tests/fixtures/i18n_vertical_cjk.pdf (gen_vertical_cjk.py). One page
// draws the SAME three CJK characters twice with TWO Type0 fonts that share one
// descendant CIDFont, one /FontDescriptor and one embedded font program. The
// ONLY difference between the two font dictionaries is:
//     /FV -> /Encoding /Identity-V   (vertical, WMode 1)
//     /FH -> /Encoding /Identity-H   (horizontal, WMode 0)
// so every difference measured below is attributable to the writing mode alone.
//
// The vertical run is ONE Tj holding three CIDs. Nothing in the content stream
// positions the glyphs individually — the stacking comes from the vertical
// encoding plus /DW2. If the vertical path breaks, the glyphs really do not
// move, so the geometry assertion is a real oracle rather than a restatement of
// hand-placed coordinates.
//
// TWO INDEPENDENT PROPERTIES ARE ASSERTED, and the issue requires both:
//   (a) ORDER — the vertical run extracts 上, 中, 下 top-to-bottom (increasing
//       stext y), left-aligned in one column;
//   (b) MODE IS SURFACED — core::TextLine::writingMode is 1 for those lines and
//       0 for the horizontal run. This is the property the pre-existing
//       coverage could not have: test_writing_mode_autoplumb.cpp assigns
//       writingMode itself on a synthetic in-memory TextLine, so it would stay
//       green if TextExtractor stopped reading fz_stext_line::wmode entirely.
//
// MUTATION THIS KILLS: hard-coding `textLine.writingMode = 0` in
// src/core/TextExtractor.cpp — extraction still yields the right characters in
// the right places, and (b) is what notices.
// -----------------------------------------------------------------------------
TEST_F(FidelityR3_07_TextExtraction, VerticalWritingExtractsTopToBottom) {
    const std::filesystem::path pdf = fixturesDir() / "i18n_vertical_cjk.pdf";
    ASSERT_TRUE(std::filesystem::exists(pdf))
        << "Fixture missing (pinned in the CMake fixture floor): " << pdf;

    auto document = Document::open(context, pdf);
    ASSERT_NE(document, nullptr);
    ASSERT_EQ(document->pageCount(), 1);
    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    const std::vector<TextBlock> blocks = page->extractText();
    ASSERT_FALSE(blocks.empty());
    const std::vector<FlatChar> all = flattenChars(blocks);

    // Authored: 上 (above), 中 (middle), 下 (below) — three characters whose
    // meanings label the three bar positions the synthetic font draws.
    const std::vector<char32_t> expected = {0x4E0A, 0x4E2D, 0x4E0B};

    const std::vector<FlatChar> vertical = charsWithWritingMode(all, 1);
    const std::vector<FlatChar> horizontal = charsWithWritingMode(all, 0);

    // (b) THE MODE IS SURFACED AT ALL — and it discriminates. Both runs exist on
    // this page, so "writingMode is always 1" and "writingMode is always 0" are
    // each caught by one of these two assertions.
    ASSERT_EQ(vertical.size(), expected.size())
        << "Expected exactly " << expected.size() << " characters on lines whose "
        << "core::TextLine::writingMode is 1 (the /Identity-V run); got "
        << vertical.size() << ". If this is 0 the vertical writing mode is not "
        << "being surfaced from fz_stext_line::wmode at all.";
    ASSERT_EQ(horizontal.size(), expected.size())
        << "Expected exactly " << expected.size() << " characters on lines whose "
        << "writingMode is 0 (the /Identity-H control run); got "
        << horizontal.size() << ". If this is 0 then EVERY line is being reported "
        << "as vertical, which would make the vertical assertion vacuous.";

    // Both runs carry the same three characters in the same logical order — so
    // the difference between them is layout, not content.
    for (size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(vertical[i].cp, expected[i])
            << "Vertical run character " << i << " should be U+" << std::hex
            << static_cast<unsigned>(expected[i]) << std::dec;
        EXPECT_EQ(horizontal[i].cp, expected[i])
            << "Horizontal control character " << i << " should be U+" << std::hex
            << static_cast<unsigned>(expected[i]) << std::dec;
    }

    // (a) ORDER — the vertical run reads TOP TO BOTTOM. stext y grows downward,
    // so each successive character must sit strictly LOWER, by about one em
    // (32 pt at the authored size).
    for (size_t i = 1; i < vertical.size(); ++i) {
        EXPECT_GT(vertical[i].quad.top(), vertical[i - 1].quad.top() + 8.0)
            << "Vertical character " << i << " (top=" << vertical[i].quad.top()
            << ") must sit BELOW character " << (i - 1) << " (top="
            << vertical[i - 1].quad.top() << ") — tategaki reads top-to-bottom";
    }
    // ...and it is a COLUMN: the glyphs share an x position rather than marching
    // rightward. This is what fails if a vertical run is laid out horizontally
    // while still being labelled wmode 1.
    for (size_t i = 1; i < vertical.size(); ++i) {
        EXPECT_NEAR(vertical[i].quad.left(), vertical[0].quad.left(), 4.0)
            << "Vertical character " << i << " left the column (left="
            << vertical[i].quad.left() << " vs " << vertical[0].quad.left() << ")";
    }

    // The horizontal control gives the OPPOSITE answer to the SAME two
    // measurements — so neither measurement can be passing by accident.
    for (size_t i = 1; i < horizontal.size(); ++i) {
        EXPECT_GT(horizontal[i].quad.left(), horizontal[i - 1].quad.left() + 8.0)
            << "Horizontal control character " << i << " must sit to the RIGHT of "
            << "character " << (i - 1);
        EXPECT_NEAR(horizontal[i].quad.top(), horizontal[0].quad.top(), 4.0)
            << "Horizontal control character " << i << " left the baseline band";
    }
}

// -----------------------------------------------------------------------------
// #23 / PDF-72 — MISSING /ToUnicode: glyph-name / StandardEncoding recovery.
//
// Fixture: tests/fixtures/f18_no_tounicode.pdf (gen_f18_backlog_corpus.py). Two
// Type 3 fonts share the SAME six glyph procedures and the SAME /Widths array —
// identical ink — and differ only in their /Encoding /Differences glyph names:
//   /T3  /Ssmall /Esmall /Csmall /Rsmall /Esmall /Tsmall   (Adobe Expert set)
//   /T4  /gx1 .. /gx6                                      (opaque, no meaning)
// Neither carries a /ToUnicode.
//
// MEASURED behaviour of MuPDF 1.26 UNAIDED on this fixture (probed before this
// test was written, not assumed): the /T3 run extracts as
//   U+F773 U+F765 U+F763 U+F772 U+F765 U+F774
// — Corporate Use Subarea codepoints, because the Expert small-caps names ARE
// in the Adobe Glyph List and that is genuinely where they map. Text "extracts"
// and nothing matches it. The /T4 run extracts as the raw character codes
// U+0001..U+0006. Both are the A3-48 redaction-by-search false negative.
//
// PDFE's recovery (src/core/ToUnicodeRecovery.cpp) synthesizes a /ToUnicode for
// a simple font that has none, whenever its own glyph-name rules beat MuPDF's —
// here by stripping the Expert `small` suffix, so /Ssmall recovers U+0053.
//
// THREE THINGS ARE ASSERTED, and the third is what stops this being a test that
// merely says "some letters appeared":
//   1. the recoverable run is now real, searchable text ("SECRET");
//   2. no Private Use codepoint survived anywhere on the page;
//   3. the UNRECOVERABLE run is UNCHANGED — U+0001..U+0006 are still there, and
//      "SECRET" occurs exactly ONCE. A recovery path that invented letters for
//      glyph names carrying no information would break (3) while passing (1).
// -----------------------------------------------------------------------------
TEST_F(FidelityR3_07_TextExtraction,
       MissingToUnicodeFallsBackToGlyphNameOrStandardEncoding) {
    // --- the resolver's own contract, stated directly ------------------------
    // GLYPH-NAME half: the Adobe Expert descriptive suffixes, and the dotted
    // variant convention.
    EXPECT_EQ(pdfe::core::recoverUnicodeFromGlyphName("Ssmall"), 0x0053);
    EXPECT_EQ(pdfe::core::recoverUnicodeFromGlyphName("zerooldstyle"), 0x0030);
    EXPECT_EQ(pdfe::core::recoverUnicodeFromGlyphName("S.sc"), 0x0053);
    EXPECT_EQ(pdfe::core::recoverUnicodeFromGlyphName("uni0053"), 0x0053);
    // StandardEncoding half: a plain standard glyph name resolves to its letter.
    EXPECT_EQ(pdfe::core::recoverUnicodeFromGlyphName("quoteright"), 0x2019);
    // A name that carries NO Unicode information must recover NOTHING. Without
    // this, "recovery" could be an invention machine.
    EXPECT_EQ(pdfe::core::recoverUnicodeFromGlyphName("gx1"), 0);
    EXPECT_EQ(pdfe::core::recoverUnicodeFromGlyphName("cid42"), 0);
    // And the unusable-answer classifier must actually classify: the PUA answer
    // a strict AGL lookup gives for Ssmall is what makes the recovery necessary.
    EXPECT_TRUE(pdfe::core::isUnusableExtractedCodepoint(0xF773));
    EXPECT_FALSE(pdfe::core::isUnusableExtractedCodepoint(0x0053));

    // --- the document-level contract ----------------------------------------
    const std::filesystem::path pdf = fixturesDir() / "f18_no_tounicode.pdf";
    ASSERT_TRUE(std::filesystem::exists(pdf))
        << "Fixture missing (pinned in the CMake fixture floor): " << pdf;

    auto document = Document::open(context, pdf);
    ASSERT_NE(document, nullptr);
    ASSERT_EQ(document->pageCount(), 1);
    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    const std::string plain = page->extractPlainText();
    ASSERT_FALSE(plain.empty());
    const std::vector<char32_t> cps = toCodepoints(plain);
    const std::vector<TextBlock> blocks = page->extractText();
    ASSERT_FALSE(blocks.empty());

    // 1. The Expert small-caps run recovered to the letters the reader sees.
    EXPECT_NE(plain.find("SECRET"), std::string::npos)
        << "The /T3 run (/Ssmall /Esmall /Csmall /Rsmall /Esmall /Tsmall, no "
        << "/ToUnicode) must recover to SECRET. Extracted: [" << describe(cps) << "]";
    EXPECT_NE(plain.find("VISIBLEMARK"), std::string::npos)
        << "The ordinary /Helvetica control must still extract";

    // 2. No Private Use codepoint survived. This is the specific failure shape:
    //    U+F773.. LOOK like successful extraction and match nothing.
    for (char32_t c : cps) {
        EXPECT_FALSE(c >= 0xE000 && c <= 0xF8FF)
            << "A Private Use Area codepoint (U+" << std::hex
            << static_cast<unsigned>(c) << std::dec << ") survived extraction — "
            << "the Adobe Expert glyph names were not recovered";
    }

    // 3. The UNRECOVERABLE /T4 run is untouched, and nothing was invented.
    //    MEASURED: under PDFE's stext options the six unmappable codes come out
    //    as U+FFFD REPLACEMENT CHARACTER (with MuPDF's whitespace-preserving
    //    default they would be the raw codes U+0001..U+0006 instead — either
    //    way, not letters). Counted over the STRUCTURED extraction because that
    //    is the precise per-character oracle.
    const std::vector<FlatChar> allChars = flattenChars(blocks);
    int unmappedCount = 0;
    for (const auto& c : allChars)
        if (c.cp == 0xFFFD || (c.cp >= 0x0001 && c.cp <= 0x0006)) ++unmappedCount;
    EXPECT_EQ(unmappedCount, 6)
        << "The /T4 run's opaque /gx1../gx6 names carry no Unicode information, "
        << "so its six codes must still extract as unmapped characters. "
        << "Recovering letters from them would mean the recovery path is "
        << "inventing text; recovering FEWER than six would mean the residual "
        << "half of the A3-48 class is no longer under test.";

    size_t occurrences = 0;
    for (size_t p = plain.find("SECRET"); p != std::string::npos;
         p = plain.find("SECRET", p + 1))
        ++occurrences;
    EXPECT_EQ(occurrences, 1u)
        << "SECRET must appear exactly once — it is authored once (the /T3 run). "
        << "Two occurrences would mean the opaque /T4 run was also 'recovered'.";
}

// -----------------------------------------------------------------------------
// #24 / PDF-73 — U+00AD SOFT HYPHEN (the DISCRETIONARY hyphen).
//
// Fixture: tests/fixtures/text_soft_hyphen.pdf (gen_soft_hyphen.py). The soft
// hyphen is authored unambiguously: /Encoding /Differences maps code 0xAD to
// /uni00AD AND an explicit /ToUnicode states <AD> -> <00AD>. So "the file
// contains U+00AD" is a fact about the bytes, not an inference.
//
// THE CONTRACT PDFE PINS HERE, AND WHY.
// U+00AD is PRESERVED VERBATIM: not dropped, not folded to U+002D HYPHEN-MINUS,
// not turned into U+FFFD. This is the same principle already pinned by
// HyphenationPreservesAuthoredHyphen one screen above — PDFE is an EDITOR, so
// extraction must report exactly the characters the content stream declares:
//   * DROPPING it would desynchronise extracted character offsets from the
//     content-stream glyphs an in-place edit or a redaction quad must target;
//     the editor would be unable to address a character that is really there.
//   * FOLDING it to U+002D would fabricate a hard hyphen the document does not
//     contain, and would make "well-being" and "un<SHY>breakable" indis-
//     tinguishable to search-and-replace even though only one of them has a
//     hyphen a reader can see.
// Reflow (TRE) is where a discretionary hyphen may legitimately be honoured or
// suppressed; that is a LAYOUT decision made on top of faithful extraction, not
// a licence for extraction to lie about the bytes.
//
// MUTATION THIS KILLS: normalising the soft hyphen in
// src/core/TextExtractor.cpp — e.g. `codepoint = (ch->c == 0x00AD) ? 0x002D
// : ch->c`, or dropping the character. Both are exactly the "helpful" changes
// this test exists to forbid.
// -----------------------------------------------------------------------------
TEST_F(FidelityR3_07_TextExtraction, SoftHyphenU00ADHandling) {
    const std::filesystem::path pdf = fixturesDir() / "text_soft_hyphen.pdf";
    ASSERT_TRUE(std::filesystem::exists(pdf))
        << "Fixture missing (pinned in the CMake fixture floor): " << pdf;

    auto document = Document::open(context, pdf);
    ASSERT_NE(document, nullptr);
    ASSERT_EQ(document->pageCount(), 1);
    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    const std::vector<TextBlock> blocks = page->extractText();
    ASSERT_FALSE(blocks.empty());
    const std::vector<FlatChar> all = flattenChars(blocks);
    ASSERT_FALSE(all.empty());

    std::vector<char32_t> pageCps;
    pageCps.reserve(all.size());
    for (const auto& c : all) pageCps.push_back(c.cp);

    // Authored: exactly TWO soft hyphens (end of "hyphen", middle of
    // "un<SHY>breakable") and exactly ONE hyphen-minus (in "well-being").
    int softCount = 0;
    int hardCount = 0;
    for (char32_t c : pageCps) {
        if (c == 0x00AD) ++softCount;
        if (c == 0x002D) ++hardCount;
    }
    EXPECT_EQ(softCount, 2)
        << "Both authored U+00AD SOFT HYPHENs must survive extraction verbatim. "
        << "0 means they were dropped; a count that moved with hardCount means "
        << "they were folded to U+002D. Extracted: [" << describe(pageCps) << "]";
    EXPECT_EQ(hardCount, 1)
        << "Exactly one U+002D HYPHEN-MINUS is authored (in 'well-being'). More "
        << "than one means a soft hyphen was folded into a hard one; zero means "
        << "the control line is broken and the soft-hyphen count proves nothing. "
        << "Extracted: [" << describe(pageCps) << "]";

    // The two kinds are DISTINCT characters, not two spellings of one. The
    // line-break case: "hyphen" ends with the soft hyphen as its LAST glyph.
    const TextLine* broken = lineStartingWith(
        blocks, {'h', 'y', 'p', 'h', 'e', 'n'});
    ASSERT_NE(broken, nullptr) << "The 'hyphen' + U+00AD line was not extracted";
    ASSERT_FALSE(broken->chars.empty());
    EXPECT_EQ(broken->chars.back().codepoint, static_cast<char32_t>(0x00AD))
        << "The discretionary hyphen must be the LAST character of the broken "
        << "line, as U+00AD — not U+002D and not absent. Got U+" << std::hex
        << static_cast<unsigned>(broken->chars.back().codepoint) << std::dec;

    // The hard-hyphen CONTROL: same page, same font, and it stays U+002D. This
    // is what makes the assertion above a discrimination rather than a tautology.
    const TextLine* control = lineStartingWith(
        blocks, {'w', 'e', 'l', 'l'});
    ASSERT_NE(control, nullptr) << "The 'well-being' control line was not extracted";
    const std::vector<char32_t> controlCps = lineCodepoints(*control);
    const std::vector<char32_t> expectedControl = {
        'w', 'e', 'l', 'l', 0x002D, 'b', 'e', 'i', 'n', 'g'};
    EXPECT_EQ(controlCps, expectedControl)
        << "The authored hard hyphen must extract as U+002D. Got: ["
        << describe(controlCps) << "]";

    // The MID-LINE discretionary hyphen: it is INSIDE the word, so the word is
    // not searchable as "unbreakable" — the character really is there.
    const TextLine* midline = lineStartingWith(blocks, {'u', 'n'});
    ASSERT_NE(midline, nullptr) << "The 'un' + U+00AD + 'breakable' line was not extracted";
    const std::vector<char32_t> midCps = lineCodepoints(*midline);
    const std::vector<char32_t> expectedMid = {
        'u', 'n', 0x00AD, 'b', 'r', 'e', 'a', 'k', 'a', 'b', 'l', 'e'};
    EXPECT_EQ(midCps, expectedMid)
        << "A mid-line discretionary hyphen must be preserved in place. Got: ["
        << describe(midCps) << "]";

    // And extraction does NOT de-hyphenate across the line break: no fabricated
    // "hyphenation" appears, exactly as for the authored hard hyphen.
    const std::string plain = page->extractPlainText();
    EXPECT_EQ(plain.find("hyphenation"), std::string::npos)
        << "Extraction joined the discretionary break into 'hyphenation'. PDFE "
        << "preserves the authored glyph stream; reflow is TRE's job, not the "
        << "extractor's. [" << plain << "]";
}

// -----------------------------------------------------------------------------
// #25 / PDF-75 — /Differences and WinAnsi 0x80..0x9F, on the READ side.
//
// Fixture: tests/fixtures/encoding_edge_cases.pdf (gen_encoding_edge_cases.py),
// authored as RAW PDF BYTES. That is the whole point of the issue: a fixture
// written by PDFE's own writer and read back would be a round-trip against
// itself and would pass even if writer and reader shared a wrong mapping. The
// expected codepoints below come from PDF 32000-1 Annex D and the Adobe Glyph
// List, not from observing PDFE.
//
// 0x80..0x9F is the band where WinAnsi DIVERGES from Latin-1: those bytes are
// C1 control characters in Latin-1 and printable punctuation (euro, curly
// quotes, dashes, ellipsis, trade mark) in WinAnsi. A reader that passes the
// raw byte through therefore produces invisible controls that look like
// successful extraction and match nothing — which is precisely the mojibake
// class this test exists to catch.
//
// MUTATION THIS KILLS: raw-byte passthrough in src/core/TextExtractor.cpp
// (`codepoint = ch->c & 0xFF`), and any change that ignores /Differences.
// -----------------------------------------------------------------------------
TEST_F(FidelityR3_07_TextExtraction, EncodingDifferencesAndWinAnsiExtractCorrectly) {
    const std::filesystem::path pdf = fixturesDir() / "encoding_edge_cases.pdf";
    ASSERT_TRUE(std::filesystem::exists(pdf))
        << "Fixture missing (pinned in the CMake fixture floor): " << pdf;

    auto document = Document::open(context, pdf);
    ASSERT_NE(document, nullptr);
    ASSERT_EQ(document->pageCount(), 1);
    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    const std::vector<TextBlock> blocks = page->extractText();
    ASSERT_FALSE(blocks.empty());

    // --- (1) WinAnsi 0x80..0x9F, the divergence band -------------------------
    // Authored codes 80 85 91 92 93 94 96 97 99 through /Encoding
    // /WinAnsiEncoding, in that order.
    const std::vector<char32_t> expectedBand = {
        0x20AC,  // 0x80 Euro
        0x2026,  // 0x85 ellipsis
        0x2018,  // 0x91 quoteleft
        0x2019,  // 0x92 quoteright
        0x201C,  // 0x93 quotedblleft
        0x201D,  // 0x94 quotedblright
        0x2013,  // 0x96 endash
        0x2014,  // 0x97 emdash
        0x2122,  // 0x99 trademark
    };
    const TextLine* bandLine = lineStartingWith(blocks, {0x20AC});
    ASSERT_NE(bandLine, nullptr)
        << "The WinAnsi 0x80..0x9F line did not extract starting with U+20AC "
        << "EURO. A raw-byte reader would have produced U+0080 here.";
    EXPECT_EQ(lineCodepoints(*bandLine), expectedBand)
        << "WinAnsi 0x80..0x9F did not map to Annex D's characters. Got: ["
        << describe(lineCodepoints(*bandLine)) << "]";

    // --- (2) /Differences overrides the base encoding ------------------------
    // Codes 41 42 43 — the ASCII bytes for "ABC" — remapped by /Differences to
    // /bullet /Euro /trademark.
    const std::vector<char32_t> expectedDiff = {0x2022, 0x20AC, 0x2122};
    const TextLine* diffLine = lineStartingWith(blocks, {0x2022});
    ASSERT_NE(diffLine, nullptr)
        << "The /Differences line did not extract starting with U+2022 BULLET. "
        << "A reader that ignored /Differences would have produced 'ABC' — the "
        << "classic legacy-document mojibake.";
    EXPECT_EQ(lineCodepoints(*diffLine), expectedDiff)
        << "/Differences did not override the base encoding. Got: ["
        << describe(lineCodepoints(*diffLine)) << "]";

    // --- (3) The CONTROL: same three bytes, no /Differences ------------------
    // Without this the test would also pass on a reader that always said
    // bullet/euro/trademark for 0x41..0x43.
    const TextLine* controlLine = lineStartingWith(blocks, {'A'});
    ASSERT_NE(controlLine, nullptr)
        << "The control line (codes 41 42 43 under plain /WinAnsiEncoding) must "
        << "extract as 'ABC'; if it does not, the /Differences result above "
        << "proves nothing.";
    const std::vector<char32_t> expectedControl = {'A', 'B', 'C'};
    EXPECT_EQ(lineCodepoints(*controlLine), expectedControl)
        << "The no-/Differences control did not extract as ABC. Got: ["
        << describe(lineCodepoints(*controlLine)) << "]";

    // --- (4) Nothing anywhere came out as a raw C1 byte ----------------------
    const std::vector<FlatChar> all = flattenChars(blocks);
    for (const auto& c : all) {
        EXPECT_FALSE(c.cp >= 0x0080 && c.cp <= 0x009F)
            << "A C1 control (U+" << std::hex << static_cast<unsigned>(c.cp)
            << std::dec << ") survived extraction — a WinAnsi code in the "
            << "0x80..0x9F band was passed through as a raw byte instead of "
            << "being mapped through the encoding.";
    }
}

// -----------------------------------------------------------------------------
// PDF-76 / GitHub #26 — TJ NUMERIC-OFFSET WORD BOUNDARIES, WITH A KERNING CONTROL
//
// In real PDFs a word break is very often pure GEOMETRY rather than a character.
// A justified or kerned line is emitted as one TJ array whose word gaps are
// negative numeric offsets — `[(Hello)-400(World)]TJ` contains NO 0x20 byte at
// all — and the extractor must SYNTHESIZE the boundary from the advance.
//
// The control is the reason this test is not trivial. Tight inter-letter
// kerning uses the SAME MECHANISM at a smaller magnitude
// (`[(W)-20(a)-12(v)...]TJ`), and must NOT produce a break. An implementation
// that inserted a boundary at every numeric offset would satisfy the gap case
// while shredding every kerned word in every real document; only the control
// catches it. So the two halves are asserted independently:
//
//   GAP    (-400 => 9.6pt, -700 => 16.8pt): "Hello"/"World" and "Left"/"Right"
//          must be separated by whitespace, and must never appear fused.
//   CONTROL (largest offset -20 => 0.48pt): "Wavefront" and "Overlapping" must
//          extract as single contiguous tokens, with NO space CHARACTER added
//          to the structured char run.
//
// Fixture: tests/fixtures/text_word_boundary_tj.pdf, authored byte-for-byte by
// tests/fixtures/gen_word_boundary_corpus.py with an UNCOMPRESSED content stream
// and no whitespace inside the TJ arrays, so step (1) below can prove from the
// raw file bytes — with no PDF parsing whatsoever — that the space in the
// extracted "Hello World" was invented by the extractor and not read off disk.
// -----------------------------------------------------------------------------
namespace {

// True iff `a` occurs before `b` in `s` and the bytes strictly between them are
// non-empty and entirely ASCII whitespace — i.e. a word boundary of SOME kind
// (space or newline) separates them. Deliberately agnostic about WHICH
// whitespace: the property under test is "a boundary exists", not its spelling.
bool separatedByWhitespace(const std::string& s,
                           const std::string& a,
                           const std::string& b,
                           std::string* between = nullptr) {
    const size_t pa = s.find(a);
    if (pa == std::string::npos) return false;
    const size_t pb = s.find(b, pa + a.size());
    if (pb == std::string::npos) return false;
    const std::string mid = s.substr(pa + a.size(), pb - (pa + a.size()));
    if (between) *between = mid;
    if (mid.empty()) return false;
    for (char c : mid) {
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') return false;
    }
    return true;
}

std::string readAllBytes(const std::filesystem::path& p) {
    std::FILE* f = std::fopen(p.string().c_str(), "rb");
    if (!f) return {};
    std::string out;
    char buf[4096];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    std::fclose(f);
    return out;
}

}  // namespace

TEST_F(FidelityR3_07_TextExtraction, KernedAndTjOffsetGapsReconstructWordBoundaries) {
    const std::filesystem::path pdf = fixturesDir() / "text_word_boundary_tj.pdf";
    // Pinned in the tests/CMakeLists.txt fixture floor — a missing file is a
    // configure-time failure, never a silent GTEST_SKIP.
    ASSERT_TRUE(std::filesystem::exists(pdf)) << "Fixture missing: " << pdf;

    // --- (1) AUTHORED BYTES: no literal space anywhere in the drawn text -----
    // The generator writes the TJ arrays with zero internal whitespace, so these
    // are exact-byte assertions on the file itself. If they hold, every space in
    // the extracted text below was SYNTHESIZED from a numeric offset.
    const std::string raw = readAllBytes(pdf);
    ASSERT_FALSE(raw.empty()) << "Could not read fixture bytes: " << pdf;

    ASSERT_NE(raw.find("[(Hello)-400(World)]TJ"), std::string::npos)
        << "Fixture no longer carries the authored gap-case TJ array verbatim; "
           "regenerate with tests/fixtures/gen_word_boundary_corpus.py";
    ASSERT_NE(raw.find("[(W)-20(a)-12(v)-18(e)-10(f)-14(r)-12(o)-16(n)-14(t)]TJ"),
              std::string::npos)
        << "Fixture no longer carries the authored tight-kerning TJ array verbatim";
    ASSERT_NE(raw.find("[(Left)-700(Right)]TJ"), std::string::npos)
        << "Fixture no longer carries the authored wide-gap TJ array verbatim";
    ASSERT_NE(raw.find("[(Over)+15(lapping)]TJ"), std::string::npos)
        << "Fixture no longer carries the authored positive-offset TJ array verbatim";

    // The whole file — content stream is uncompressed — contains no spaced form
    // of any anchor pair. Nothing on disk says "Hello World".
    for (const char* forbidden : {"Hello World", "HelloWorld",
                                  "Left Right", "LeftRight",
                                  "Wave front", "Over lapping"}) {
        ASSERT_EQ(raw.find(forbidden), std::string::npos)
            << "Fixture bytes contain '" << forbidden << "' — the fixture no "
               "longer isolates geometry-only word boundaries";
    }

    auto document = Document::open(context, pdf);
    ASSERT_NE(document, nullptr);
    ASSERT_GT(document->pageCount(), 0);
    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    const std::string plain = page->extractPlainText();
    ASSERT_FALSE(plain.empty());

    // --- (2) GAP CASE: a numeric offset alone must reconstruct the boundary --
    std::string mid;
    EXPECT_TRUE(separatedByWhitespace(plain, "Hello", "World", &mid))
        << "TJ numeric-offset gap of -400 (9.6pt at 24pt Helvetica, ~2.7x MuPDF's "
           "space threshold) did NOT reconstruct a word boundary: 'Hello' and "
           "'World' are separated by [" << mid << "] in the extracted text. The "
           "authored bytes are '[(Hello)-400(World)]TJ' with no space byte, so "
           "the boundary must come from the advance.";
    EXPECT_EQ(plain.find("HelloWorld"), std::string::npos)
        << "'Hello' and 'World' extracted FUSED — the numeric-offset word gap "
           "was dropped entirely.";

    std::string mid2;
    EXPECT_TRUE(separatedByWhitespace(plain, "Left", "Right", &mid2))
        << "TJ numeric-offset gap of -700 (16.8pt) did NOT reconstruct a word "
           "boundary: 'Left' and 'Right' are separated by [" << mid2 << "].";
    EXPECT_EQ(plain.find("LeftRight"), std::string::npos)
        << "'Left' and 'Right' extracted FUSED at a 16.8pt authored gap.";

    // --- (3) CONTROL: the same mechanism at kerning magnitude must NOT break --
    EXPECT_NE(plain.find("Wavefront"), std::string::npos)
        << "TIGHT-KERNING CONTROL FAILED: the word authored as "
           "'[(W)-20(a)-12(v)-18(e)-10(f)-14(r)-12(o)-16(n)-14(t)]TJ' — the same "
           "numeric-offset mechanism as the gap case, but at most 0.48pt, ~7.5x "
           "BELOW the space threshold — did not extract as the contiguous token "
           "'Wavefront'. A word break was inserted at inter-letter kerning. "
           "Extracted text was:\n" << plain;
    EXPECT_NE(plain.find("Overlapping"), std::string::npos)
        << "POSITIVE-OFFSET CONTROL FAILED: '[(Over)+15(lapping)]TJ' pulls the "
           "second element LEFT (tighter than the natural advance), which can "
           "never be a word gap, yet 'Overlapping' did not extract contiguously. "
           "Extracted text was:\n" << plain;

    // --- (4) STRUCTURED SURFACE: count the CHARACTERS, not just the string ---
    // The plain-text checks above could in principle be satisfied by a string
    // that merely happens to contain the token. This pins the char run itself:
    // the control line must carry exactly its nine letters and NO space char,
    // while the gap line must carry a synthesized U+0020 between the words.
    const std::vector<TextBlock> blocks = page->extractText();
    ASSERT_FALSE(blocks.empty());

    // Lines are located by DOCUMENT ORDER, not by searching their text. A
    // text search would be circular here: the over-eager failure this test
    // exists to catch mangles line.text(), so a search for "front" would stop
    // finding the control line and the test would die at a setup assertion
    // instead of reporting the property. Order is stable — the four TJ arrays
    // are drawn top to bottom at fixed y (700/650/600/550).
    std::vector<const TextLine*> orderedLines;
    for (const auto& block : blocks) {
        if (block.type == TextBlockType::Image) continue;
        for (const auto& line : block.lines) orderedLines.push_back(&line);
    }
    ASSERT_EQ(orderedLines.size(), 4u)
        << "Expected exactly four extracted lines (gap / kerning control / wide "
           "gap / positive-offset control); got " << orderedLines.size()
        << ". The fixture or the line segmentation changed.";

    const TextLine* gapLine = orderedLines[0];
    const TextLine* kernLine = orderedLines[1];

    // Prove the order-based pick landed on the intended lines, using the CHAR
    // CODEPOINTS with whitespace removed — a view that no space-insertion
    // change can disturb.
    auto lettersOf = [](const TextLine* l) {
        std::string s;
        for (const auto& c : l->chars) {
            if (c.codepoint != U' ' && c.codepoint < 0x80) {
                s.push_back(static_cast<char>(c.codepoint));
            }
        }
        return s;
    };
    ASSERT_EQ(lettersOf(gapLine), std::string("HelloWorld"))
        << "Line 0 is not the gap case; got letters '" << lettersOf(gapLine) << "'";
    ASSERT_EQ(lettersOf(kernLine), std::string("Wavefront"))
        << "Line 1 is not the tight-kerning control; got letters '"
        << lettersOf(kernLine) << "'";

    EXPECT_EQ(trimmed(kernLine->text()), std::string("Wavefront"))
        << "TIGHT-KERNING CONTROL FAILED at the structured layer: the line "
           "extracted as '" << kernLine->text() << "' rather than 'Wavefront'.";

    int kernSpaces = 0;
    for (const auto& c : kernLine->chars) {
        if (c.codepoint == U' ') ++kernSpaces;
    }
    EXPECT_EQ(kernSpaces, 0)
        << "TIGHT-KERNING CONTROL FAILED: " << kernSpaces << " space character(s) "
           "were synthesized inside 'Wavefront'. Inter-letter kerning offsets "
           "must not produce word boundaries.";

    int gapSpaces = 0;
    for (const auto& c : gapLine->chars) {
        if (c.codepoint == U' ') ++gapSpaces;
    }
    EXPECT_EQ(gapSpaces, 1)
        << "The gap line extracted with " << gapSpaces << " space character(s); "
           "exactly one should have been synthesized from the -400 offset. Line "
           "text: '" << gapLine->text() << "'";

    // --- (5) PROVE THE CONTROL IS A REAL CONTROL -----------------------------
    // The control would be worthless if the kerned word simply had no offsets:
    // then "no break" would be trivially true and no over-eager implementation
    // could ever be caught by it. Measure the actual inter-letter gaps and
    // require them to be genuinely POSITIVE (the offsets did move the pen) yet
    // strictly SMALLER than the gap-case advance.
    ASSERT_GE(kernLine->chars.size(), 9u);
    double maxKernGap = -1e9;
    for (size_t i = 0; i + 1 < kernLine->chars.size(); ++i) {
        const auto& a = kernLine->chars[i];
        const auto& b = kernLine->chars[i + 1];
        maxKernGap = std::max(maxKernGap,
                              b.origin.x() - (a.origin.x() + a.quad.width()));
    }
    EXPECT_GT(maxKernGap, 0.0)
        << "The tight-kerning control has NO positive inter-letter gap "
           "(max=" << maxKernGap << "pt), so it does not exercise the numeric-"
           "offset mechanism at all and cannot catch an over-eager extractor. "
           "The fixture must be regenerated with negative TJ offsets.";
    EXPECT_LT(maxKernGap, 2.0)
        << "The tight-kerning control's largest inter-letter gap is "
        << maxKernGap << "pt — that is no longer 'tight kerning'. Authored value "
           "is -20/1000 * 24pt = 0.48pt.";

    // The gap-case advance must dwarf it. NOTE the measurement differs by
    // necessity: on the gap line the synthesized space is a real char that
    // OCCUPIES the gap, so adjacent-char arithmetic reports ~0 everywhere.
    // Measure across the space char instead — from the end of the last glyph
    // before it to the origin of the first glyph after it. That is the advance
    // MuPDF actually had to classify.
    size_t spaceIdx = gapLine->chars.size();
    for (size_t i = 0; i < gapLine->chars.size(); ++i) {
        if (gapLine->chars[i].codepoint == U' ') { spaceIdx = i; break; }
    }
    ASSERT_LT(spaceIdx, gapLine->chars.size())
        << "No space char on the gap line — cannot measure the gap advance";
    ASSERT_GT(spaceIdx, 0u);
    ASSERT_LT(spaceIdx + 1, gapLine->chars.size());

    const auto& beforeGap = gapLine->chars[spaceIdx - 1];
    const auto& afterGap = gapLine->chars[spaceIdx + 1];
    const double gapAdvance =
        afterGap.origin.x() - (beforeGap.origin.x() + beforeGap.quad.width());

    EXPECT_GT(gapAdvance, maxKernGap * 3.0)
        << "The gap case's word advance (" << gapAdvance << "pt, authored as "
           "-400/1000 * 24pt = 9.6pt) is not clearly larger than the kerning "
           "control's largest inter-letter gap (" << maxKernGap << "pt, authored "
           "as -20/1000 * 24pt = 0.48pt). The two cases no longer differ by "
           "magnitude, so the pair proves nothing about the threshold.";
}
