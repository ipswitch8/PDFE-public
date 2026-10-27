// -*- coding: utf-8 -*-
//
// PDF-77 / GitHub #27 — COLUMN-MAJOR READING ORDER ON A REAL MULTI-COLUMN PAPER.
//
// WHY THIS FILE EXISTS
// --------------------
// tests/test_fidelity_r3_07_text_extraction.cpp already pins column-major
// reading order, but only against test_PDFs/multicolumn.pdf — a synthetic
// two-column Lorem Ipsum page that lives in the GITIGNORED external corpus, so
// on a clean checkout that oracle GTEST_SKIPs to green. Nothing asserted reading
// order against a COMMITTED, real-world, professionally typeset multi-column
// document.
//
// This file closes that. It uses tests/fixtures/realworld/arxiv_bert.pdf — the
// BERT paper, typeset in the two-column ACL style — and asserts that extraction
// reads the LEFT column top-to-bottom in full BEFORE starting the RIGHT column,
// even though material at the top of the right column sits physically HIGHER on
// the page than material at the bottom of the left column. A row-major
// extractor (scan each visual band left-to-right across both columns) inverts
// that order and fails here.
//
// ANCHOR SELECTION — WHY THESE TOKENS ARE STABLE
// ----------------------------------------------
// A reading-order oracle is only as good as the tokens it locates, and a badly
// chosen anchor turns the test into a tripwire that reddens for the wrong
// reason. Every anchor below is:
//
//   * a SINGLE hyphenated word, never a multi-word phrase. Nothing about the
//     assertion depends on how the extractor spaces words, so a change to space
//     synthesis or word-gap thresholds cannot move it.
//   * drawn from a SECTION HEADING, not body text. Headings are short, sit on
//     one line well inside the column measure, and are never reflowed,
//     re-hyphenated or line-broken by an extraction change.
//   * verified UNIQUE on the page by the test itself (occurrence count == 1),
//     so `find()` provably matched the heading and not some other occurrence.
//   * carrying only an AUTHORED hyphen (inside "Feature-based" /
//     "Fine-tuning"), never an end-of-line hyphen, so a de-hyphenation policy
//     change cannot rewrite it either.
//
// The assertions are on RELATIVE ORDER of anchors only — never on absolute
// offsets, indices, or the number of characters between them.
//
// Page 1 (0-based) of arxiv_bert.pdf, geometry measured from the fixture:
//
//     |<-- left column, x 72..290 -->|  |<-- right column, x 307..526 -->|
//     y=491  "2  Related Work"           y= 65  body continues
//     y=561  "2.1 Unsupervised            y=529  "2.2 Unsupervised
//             Feature-based Approaches"          Fine-tuning Approaches"
//     y=579  body ... down to y=766      y=550  body ... down to y=766
//
// (stext coordinates: y grows DOWNWARD, so y=529 is HIGHER on the page than
// y=561.) The right-column heading is therefore physically ABOVE the
// left-column heading, yet must be read AFTER it. That single inequality is the
// row-major killer.

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/Page.h"
#include "core/TextBlock.h"
#include "core/TextLine.h"
#include "core/TextChar.h"
#include "core/MuPdfContext.h"

#include <QRectF>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace pdfe::core;

namespace {

std::filesystem::path realworldDir() {
    return std::filesystem::path(TEST_FIXTURES_DIR) / "realworld";
}

std::size_t countOccurrences(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return 0;
    std::size_t n = 0;
    for (std::size_t p = haystack.find(needle); p != std::string::npos;
         p = haystack.find(needle, p + needle.size())) {
        ++n;
    }
    return n;
}

// Bounding box of the first extracted LINE whose text contains `needle`. Used
// for the geometry cross-check, which is derived from the STRUCTURED hierarchy
// and is therefore independent of the plain-text string search the ordering
// assertions use.
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

// The two-column body page of the BERT paper. Page 0 is the title page (single
// column abstract + two-column intro); page 1 is a clean, fully two-column body
// page, which is what the reading-order property needs.
constexpr int kTwoColumnPageIndex = 1;

// Anchors. See the header comment for why each is stable.
constexpr const char* kLeftUpper = "Related";        // left col, "2 Related Work"
constexpr const char* kLeftLower = "Feature-based";  // left col, "2.1 ... Approaches"
constexpr const char* kRightUpper = "Fine-tuning";   // RIGHT col, "2.2 ... Approaches"

}  // namespace

class ExtractionLayoutRealworld : public ::testing::Test {
protected:
    void SetUp() override { context = std::make_shared<MuPdfContext>(); }
    std::shared_ptr<MuPdfContext> context;
};

TEST_F(ExtractionLayoutRealworld, ArxivTwoColumnPageReadsColumnMajor) {
    const std::filesystem::path pdf = realworldDir() / "arxiv_bert.pdf";
    // Committed fixture, pinned in the tests/CMakeLists.txt fixture floor. A
    // missing file is a configure-time failure, never a silent GTEST_SKIP.
    ASSERT_TRUE(std::filesystem::exists(pdf)) << "Fixture missing: " << pdf;

    auto document = Document::open(context, pdf);
    ASSERT_NE(document, nullptr);
    ASSERT_GT(document->pageCount(), kTwoColumnPageIndex);

    auto page = document->page(kTwoColumnPageIndex);
    ASSERT_NE(page, nullptr);

    const std::string plain = page->extractPlainText();
    ASSERT_FALSE(plain.empty());

    // --- (1) THE ANCHORS EXIST AND ARE UNIQUE --------------------------------
    // Without this, a find() that happened to match a second occurrence
    // elsewhere on the page would make the ordering assertions measure
    // something other than what the comment claims. Uniqueness is what licenses
    // the use of a bare find() below.
    for (const char* anchor : {kLeftUpper, kLeftLower, kRightUpper}) {
        EXPECT_EQ(countOccurrences(plain, anchor), 1u)
            << "Anchor '" << anchor << "' is not unique on page "
            << kTwoColumnPageIndex << " of arxiv_bert.pdf (found "
            << countOccurrences(plain, anchor) << " occurrences). The ordering "
               "assertions below would then be measuring an arbitrary match. "
               "Pick a different heading token.";
    }

    const std::size_t posLeftUpper = plain.find(kLeftUpper);
    const std::size_t posLeftLower = plain.find(kLeftLower);
    const std::size_t posRightUpper = plain.find(kRightUpper);

    ASSERT_NE(posLeftUpper, std::string::npos)
        << "Left-column anchor '" << kLeftUpper << "' was not extracted at all";
    ASSERT_NE(posLeftLower, std::string::npos)
        << "Left-column anchor '" << kLeftLower << "' was not extracted at all";
    ASSERT_NE(posRightUpper, std::string::npos)
        << "Right-column anchor '" << kRightUpper << "' was not extracted at all";

    // --- (2) WITHIN THE LEFT COLUMN, ORDER IS TOP-TO-BOTTOM ------------------
    EXPECT_LT(posLeftUpper, posLeftLower)
        << "Within the LEFT column, the higher heading '" << kLeftUpper
        << "' must be read before the lower heading '" << kLeftLower
        << "'. Reading order is not even top-to-bottom within one column.";

    // --- (3) CORE ORACLE: LEFT COLUMN COMPLETES BEFORE THE RIGHT BEGINS ------
    // '2.2 Unsupervised Fine-tuning Approaches' sits at the TOP of the right
    // column, physically ABOVE '2.1 Unsupervised Feature-based Approaches' at
    // the BOTTOM of the left column (assertion (4) proves this from geometry).
    // A row-major extractor emits the higher one first and inverts this.
    EXPECT_LT(posLeftLower, posRightUpper)
        << "READING ORDER IS NOT COLUMN-MAJOR. The bottom-of-LEFT-column heading '"
        << kLeftLower << "' (offset " << posLeftLower << ") must precede the "
        << "RIGHT-column heading '" << kRightUpper << "' (offset " << posRightUpper
        << "), but does not. The right-column heading is physically HIGHER on the "
           "page, so a row-major (across-columns) extractor emits it first — that "
           "is exactly the defect this asserts against.";

    EXPECT_LT(posLeftUpper, posRightUpper)
        << "Second, independent left/right anchor pair: '" << kLeftUpper
        << "' (left column) must precede '" << kRightUpper << "' (right column) "
           "under column-major reading.";

    // --- (4) GEOMETRY CROSS-CHECK -------------------------------------------
    // Prove the anchors really are in different columns, and that the one read
    // LATER is the one sitting HIGHER. Without this the ordering assertions
    // above could be satisfied by a page that is not actually two-column, and
    // the test would pass while proving nothing.
    const std::vector<TextBlock> blocks = page->extractText();
    ASSERT_FALSE(blocks.empty());

    const auto leftBox = lineBBoxContaining(blocks, kLeftLower);
    const auto rightBox = lineBBoxContaining(blocks, kRightUpper);
    ASSERT_TRUE(leftBox.has_value())
        << "Could not locate a line bbox for '" << kLeftLower << "'";
    ASSERT_TRUE(rightBox.has_value())
        << "Could not locate a line bbox for '" << kRightUpper << "'";

    // x grows RIGHTWARD in stext page coordinates.
    EXPECT_GT(rightBox->left(), leftBox->left())
        << "'" << kRightUpper << "' (left=" << rightBox->left()
        << ") is not to the RIGHT of '" << kLeftLower << "' (left="
        << leftBox->left() << "): the two anchors are not in different columns, "
           "so the ordering assertion is not testing column-major reading.";

    // The two columns must not overlap horizontally at all, or "different
    // column" is a matter of opinion.
    EXPECT_GE(rightBox->left(), leftBox->right())
        << "The right-column anchor starts at x=" << rightBox->left()
        << " but the left-column anchor extends to x=" << leftBox->right()
        << " — the anchors' horizontal spans overlap, so they may not be in "
           "distinct columns.";

    // y grows DOWNWARD: a SMALLER top() is physically HIGHER on the page.
    EXPECT_LT(rightBox->top(), leftBox->top())
        << "'" << kRightUpper << "' (top=" << rightBox->top()
        << ") does not sit HIGHER on the page than '" << kLeftLower << "' (top="
        << leftBox->top() << "). If the right-column anchor is not physically "
           "above the left-column one, assertion (3) no longer distinguishes "
           "column-major from row-major reading and the anchors must be re-picked.";
}
