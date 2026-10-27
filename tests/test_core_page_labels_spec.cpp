// -*- coding: utf-8 -*-
//
// Spec-conformance tests for pdfe::core::PageLabelManager's PURE functions
// (src/core/PageLabels.{h,cpp}) -- GAP A1-37, src/core dedicated coverage.
//
// RELATIONSHIP TO test_page_labels_editor.cpp
// -------------------------------------------
// That file covers the read/write round-trip through a Document plus a handful
// of happy-path formatting samples. It contains no boundary, no ordering, no
// prefix, no out-of-range and no case-sensitivity coverage -- and the number
// formatter is where a page-label defect actually shows up (a viewer that
// displays "DCCCC" for 900, or "AB" for 28, is wrong on the user's screen).
// This file pins the formatter and the range-selection rule against the SPEC.
//
// AUTHORED GROUND TRUTH -- ISO 32000-1 (PDF 1.7) sec. 12.4.2, Table 159:
//
//   /S /D  Decimal arabic numerals
//   /S /R  Uppercase roman numerals
//   /S /r  Lowercase roman numerals
//   /S /A  Uppercase letters: "A to Z for the first 26 pages, AA to ZZ for the
//          next 26, and so on"          <- REPEATED letters, not base-26
//   /S /a  Lowercase letters, same rule
//   (/S absent)  no numeric portion -- the label is the /P prefix alone
//   /P     label prefix, prepended to the numeric portion
//   /St    the value of the numeric portion for the FIRST page in the range;
//          "the default value is 1"
//
// Range selection (same clause): the label for a page is produced by the
// number-tree entry with the greatest key <= that page's index; pages before
// the first entry have no label.
//
// Roman numerals follow the standard subtractive notation (IV, IX, XL, XC, CD,
// CM). PageLabels.cpp additionally documents that values above 3999 repeat 'M'
// rather than using an overline, "matching Acrobat's behavior" -- that is the
// authored contract for the out-of-classical-range case.

#include <gtest/gtest.h>

#include "core/PageLabels.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

using pdfe::core::PageLabelManager;
using pdfe::core::PageLabelRange;
using pdfe::core::PageLabelStyle;

namespace {

PageLabelRange range(int firstPage, PageLabelStyle style,
                     const std::string& prefix = "", int startValue = 1)
{
    PageLabelRange r;
    r.firstPage = firstPage;
    r.style = style;
    r.prefix = prefix;
    r.startValue = startValue;
    return r;
}

// ===========================================================================
// Roman numerals -- ISO 32000-1 sec. 12.4.2 /R and /r
// ===========================================================================

// Standard subtractive notation across every boundary the table encodes.
// KILLS: deleting any of the six subtractive rows {900,CM} {400,CD} {90,XC}
// {40,XL} {9,IX} {4,IV} -- each omission changes exactly one column below
// (e.g. losing {900,"CM"} turns 900 into "DCCCC" and 1994 into "MDCCCCXCIV").
TEST(PageLabelsSpec, RomanUpperUsesSubtractiveNotationAtEveryBoundary)
{
    struct Case { int n; const char* want; };
    const Case cases[] = {
        {1, "I"},      {2, "II"},     {3, "III"},    {4, "IV"},
        {5, "V"},      {6, "VI"},     {8, "VIII"},   {9, "IX"},
        {10, "X"},     {14, "XIV"},   {19, "XIX"},   {29, "XXIX"},
        {40, "XL"},    {44, "XLIV"},  {49, "XLIX"},  {50, "L"},
        {89, "LXXXIX"},{90, "XC"},    {99, "XCIX"},  {100, "C"},
        {400, "CD"},   {444, "CDXLIV"},              {490, "CDXC"},
        {500, "D"},    {900, "CM"},   {990, "CMXC"}, {999, "CMXCIX"},
        {1000, "M"},   {1994, "MCMXCIV"},            {2024, "MMXXIV"},
        {3888, "MMMDCCCLXXXVIII"},                   {3999, "MMMCMXCIX"},
    };
    for (const Case& c : cases) {
        EXPECT_EQ(PageLabelManager::formatNumber(c.n, PageLabelStyle::RomanUpper),
                  c.want)
            << "n=" << c.n;
    }
}

// PageLabels.cpp: "numbers > 3999 get repeated 'M's (matching Acrobat's
// behavior)". A page-label range can legitimately run past 3999 in a large
// scanned volume, and the alternative (an empty or overlined label) would be a
// visible defect.
// KILLS: clamping/returning empty above 3999.
TEST(PageLabelsSpec, RomanAboveThreeThousandNineHundredNinetyNineRepeatsM)
{
    EXPECT_EQ(PageLabelManager::formatNumber(4000, PageLabelStyle::RomanUpper),
              "MMMM");
    EXPECT_EQ(PageLabelManager::formatNumber(4999, PageLabelStyle::RomanUpper),
              "MMMMCMXCIX");
    EXPECT_EQ(PageLabelManager::formatNumber(10000, PageLabelStyle::RomanLower),
              std::string(10, 'm'));
}

// /r must be exactly /R lowercased -- two independent tables are two places
// for a typo, and a single wrong glyph would be near-invisible in review.
// KILLS: any divergence between the upper and lower tables (a swapped or
// mistyped numeral in either one).
TEST(PageLabelsSpec, RomanLowerIsExactlyRomanUpperLowercased)
{
    for (int n = 1; n <= 2000; ++n) {
        std::string upper =
            PageLabelManager::formatNumber(n, PageLabelStyle::RomanUpper);
        const std::string lower =
            PageLabelManager::formatNumber(n, PageLabelStyle::RomanLower);
        std::transform(upper.begin(), upper.end(), upper.begin(),
                       [](unsigned char ch) {
                           return static_cast<char>(std::tolower(ch));
                       });
        ASSERT_EQ(lower, upper) << "n=" << n;
    }
}

// Roman/alpha numbering starts at 1 (there is no roman zero, and /St's default
// is 1). Non-positive input is not representable and must yield the empty
// numeric portion rather than a garbage or infinite-loop result.
// KILLS: removing the `if (n <= 0) return {}` guards -- formatRoman would
// simply produce "" anyway, but formatAlpha would compute a negative
// repetition count and a char below 'A'.
TEST(PageLabelsSpec, NonPositiveInputYieldsAnEmptyNumericPortion)
{
    for (int n : {0, -1, -26, -1000}) {
        EXPECT_EQ(PageLabelManager::formatNumber(n, PageLabelStyle::RomanUpper),
                  "") << "n=" << n;
        EXPECT_EQ(PageLabelManager::formatNumber(n, PageLabelStyle::RomanLower),
                  "") << "n=" << n;
        EXPECT_EQ(PageLabelManager::formatNumber(n, PageLabelStyle::AlphaUpper),
                  "") << "n=" << n;
        EXPECT_EQ(PageLabelManager::formatNumber(n, PageLabelStyle::AlphaLower),
                  "") << "n=" << n;
    }
}

// ===========================================================================
// Alphabetic labels -- ISO 32000-1 sec. 12.4.2 /A and /a
// ===========================================================================

// The spec rule is REPEATED letters ("AA to ZZ for the next 26"), not
// spreadsheet-style base-26. The two agree only for the first 26 values, so a
// base-26 implementation passes any test that stops at Z.
// KILLS: implementing bijective base-26 (27 would become "AA" -- which happens
// to match -- but 28 would become "AB" instead of "BB", and 53 "BA" instead of
// "AAA").
TEST(PageLabelsSpec, AlphaUpperUsesRepeatedLettersNotBaseTwentySix)
{
    // First block: A..Z
    for (int n = 1; n <= 26; ++n) {
        const std::string want(1, static_cast<char>('A' + n - 1));
        EXPECT_EQ(PageLabelManager::formatNumber(n, PageLabelStyle::AlphaUpper),
                  want) << "n=" << n;
    }
    // Second block: AA..ZZ
    EXPECT_EQ(PageLabelManager::formatNumber(27, PageLabelStyle::AlphaUpper),
              "AA");
    EXPECT_EQ(PageLabelManager::formatNumber(28, PageLabelStyle::AlphaUpper),
              "BB");
    EXPECT_EQ(PageLabelManager::formatNumber(51, PageLabelStyle::AlphaUpper),
              "YY");
    EXPECT_EQ(PageLabelManager::formatNumber(52, PageLabelStyle::AlphaUpper),
              "ZZ");
    // Third block: AAA..ZZZ
    EXPECT_EQ(PageLabelManager::formatNumber(53, PageLabelStyle::AlphaUpper),
              "AAA");
    EXPECT_EQ(PageLabelManager::formatNumber(78, PageLabelStyle::AlphaUpper),
              "ZZZ");
    // Fourth block starts again at A, four wide.
    EXPECT_EQ(PageLabelManager::formatNumber(79, PageLabelStyle::AlphaUpper),
              "AAAA");
}

// Structural restatement of the same rule, checked over four full blocks: the
// letter cycles with period 26 and the width increases every 26 values.
// KILLS: an off-by-one in either `(n-1) % 26` or `(n-1) / 26 + 1`.
TEST(PageLabelsSpec, AlphaWidthAndLetterFollowTheBlockRuleAcrossBlocks)
{
    for (int n = 1; n <= 104; ++n) {
        const std::string got =
            PageLabelManager::formatNumber(n, PageLabelStyle::AlphaUpper);
        const std::size_t wantWidth =
            static_cast<std::size_t>((n - 1) / 26 + 1);
        const char wantChar = static_cast<char>('A' + (n - 1) % 26);
        ASSERT_EQ(got.size(), wantWidth) << "n=" << n << " got='" << got << "'";
        ASSERT_EQ(got, std::string(wantWidth, wantChar)) << "n=" << n;
    }
}

// /a is /A lowercased.
// KILLS: hard-coding 'A' as the base for both styles.
TEST(PageLabelsSpec, AlphaLowerIsExactlyAlphaUpperLowercased)
{
    for (int n = 1; n <= 200; ++n) {
        std::string upper =
            PageLabelManager::formatNumber(n, PageLabelStyle::AlphaUpper);
        const std::string lower =
            PageLabelManager::formatNumber(n, PageLabelStyle::AlphaLower);
        std::transform(upper.begin(), upper.end(), upper.begin(),
                       [](unsigned char ch) {
                           return static_cast<char>(std::tolower(ch));
                       });
        ASSERT_EQ(lower, upper) << "n=" << n;
    }
}

// ===========================================================================
// Style <-> /S name character
// ===========================================================================

// /R and /r are DIFFERENT styles (Table 159). A case-insensitive mapping would
// silently turn every lowercase-roman front matter into uppercase.
// KILLS: making pdfCharToStyle() case-insensitive, or collapsing /A and /a.
TEST(PageLabelsSpec, StyleCharacterMappingIsCaseSensitiveAndRoundTrips)
{
    const PageLabelStyle styles[] = {
        PageLabelStyle::None,       PageLabelStyle::Decimal,
        PageLabelStyle::RomanUpper, PageLabelStyle::RomanLower,
        PageLabelStyle::AlphaUpper, PageLabelStyle::AlphaLower};
    for (PageLabelStyle s : styles) {
        const char c = PageLabelManager::styleToPdfChar(s);
        EXPECT_EQ(PageLabelManager::pdfCharToStyle(c), s)
            << "style " << static_cast<int>(s) << " -> '" << c << "'";
    }

    EXPECT_EQ(PageLabelManager::styleToPdfChar(PageLabelStyle::Decimal), 'D');
    EXPECT_EQ(PageLabelManager::styleToPdfChar(PageLabelStyle::RomanUpper), 'R');
    EXPECT_EQ(PageLabelManager::styleToPdfChar(PageLabelStyle::RomanLower), 'r');
    EXPECT_EQ(PageLabelManager::styleToPdfChar(PageLabelStyle::AlphaUpper), 'A');
    EXPECT_EQ(PageLabelManager::styleToPdfChar(PageLabelStyle::AlphaLower), 'a');
    EXPECT_EQ(PageLabelManager::styleToPdfChar(PageLabelStyle::None), '\0');

    EXPECT_NE(PageLabelManager::pdfCharToStyle('R'),
              PageLabelManager::pdfCharToStyle('r'));
    EXPECT_NE(PageLabelManager::pdfCharToStyle('A'),
              PageLabelManager::pdfCharToStyle('a'));
}

// An unrecognised /S name must degrade to "no numeric portion" (prefix only),
// which is the spec's own behaviour for an absent /S -- never to a guessed
// style.
// KILLS: a default: case that returns Decimal.
TEST(PageLabelsSpec, UnknownStyleCharactersMapToNone)
{
    for (char c : {'\0', 'd', 'Z', 'B', 'i', 'I', '1', ' ', '/', '\x7f'}) {
        EXPECT_EQ(PageLabelManager::pdfCharToStyle(c), PageLabelStyle::None)
            << "char '" << c << "' (" << static_cast<int>(c) << ")";
    }
}

// Spec: with /S absent the label has no numeric portion.
// KILLS: formatNumber falling through to Decimal for None.
TEST(PageLabelsSpec, NoneStyleHasNoNumericPortion)
{
    EXPECT_EQ(PageLabelManager::formatNumber(1, PageLabelStyle::None), "");
    EXPECT_EQ(PageLabelManager::formatNumber(7, PageLabelStyle::None), "");
    EXPECT_EQ(PageLabelManager::formatNumber(0, PageLabelStyle::None), "");
}

// ===========================================================================
// labelFor() -- range selection, /St and /P
// ===========================================================================

// Spec: the numeric portion of the first page in a range is /St; subsequent
// pages increment by one.
// KILLS: ignoring startValue, or offsetting from the page index instead of
// from the range start.
TEST(PageLabelsSpec, StartValueSeedsTheRangeAndIncrementsPerPage)
{
    const std::vector<PageLabelRange> r{
        range(0, PageLabelStyle::Decimal, "", 1),
        range(4, PageLabelStyle::Decimal, "", 100),
    };
    EXPECT_EQ(PageLabelManager::labelFor(0, r), "1");
    EXPECT_EQ(PageLabelManager::labelFor(3, r), "4");
    EXPECT_EQ(PageLabelManager::labelFor(4, r), "100");
    EXPECT_EQ(PageLabelManager::labelFor(5, r), "101");
    EXPECT_EQ(PageLabelManager::labelFor(9, r), "105");
}

// Spec: /P is prepended to the numeric portion, verbatim. Prefixes are text
// strings and routinely contain spaces, punctuation and non-ASCII.
// KILLS: trimming, sanitising, or inserting a separator between prefix and
// number.
TEST(PageLabelsSpec, PrefixIsPrependedVerbatimIncludingSpacesAndNonAscii)
{
    const std::vector<PageLabelRange> r{
        range(0, PageLabelStyle::Decimal, "A-", 1),
        range(2, PageLabelStyle::RomanLower, "Appendix ", 1),
        // UTF-8 bytes for the Cyrillic prefix "Прил. ",
        // written as hex escapes so the literal is source-encoding independent.
        range(4, PageLabelStyle::Decimal, "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xBB. ", 1),
        range(6, PageLabelStyle::Decimal, "", 1),
    };
    EXPECT_EQ(PageLabelManager::labelFor(0, r), "A-1");
    EXPECT_EQ(PageLabelManager::labelFor(1, r), "A-2");
    EXPECT_EQ(PageLabelManager::labelFor(2, r), "Appendix i");
    EXPECT_EQ(PageLabelManager::labelFor(3, r), "Appendix ii");
    EXPECT_EQ(PageLabelManager::labelFor(4, r), "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xBB. 1");
    EXPECT_EQ(PageLabelManager::labelFor(6, r), "1");
}

// Spec: with /S absent the label IS the prefix -- for every page in the range,
// unchanged (there is nothing to increment).
// KILLS: appending an implicit number when /S is absent.
TEST(PageLabelsSpec, NoneStyleRangeYieldsTheBarePrefixOnEveryPage)
{
    const std::vector<PageLabelRange> r{
        range(0, PageLabelStyle::None, "Cover", 1)};
    EXPECT_EQ(PageLabelManager::labelFor(0, r), "Cover");
    EXPECT_EQ(PageLabelManager::labelFor(1, r), "Cover");
    EXPECT_EQ(PageLabelManager::labelFor(50, r), "Cover");

    // No prefix and no style: an empty label, not "0" or "1".
    const std::vector<PageLabelRange> bare{range(0, PageLabelStyle::None)};
    EXPECT_EQ(PageLabelManager::labelFor(0, bare), "");
}

// The number tree is unordered as a C++ container; labelFor() sorts before
// selecting. A caller that assembled ranges from a UI list, or a /Nums array
// written out of order, must still get the spec answer.
// KILLS: removing the std::sort in labelFor().
TEST(PageLabelsSpec, RangesAreNormalisedByFirstPageBeforeSelection)
{
    const std::vector<PageLabelRange> ordered{
        range(0, PageLabelStyle::RomanLower),
        range(3, PageLabelStyle::Decimal),
        range(6, PageLabelStyle::AlphaUpper, "A-"),
    };
    const std::vector<PageLabelRange> shuffled{
        range(6, PageLabelStyle::AlphaUpper, "A-"),
        range(0, PageLabelStyle::RomanLower),
        range(3, PageLabelStyle::Decimal),
    };
    const std::vector<PageLabelRange> reversed{
        range(6, PageLabelStyle::AlphaUpper, "A-"),
        range(3, PageLabelStyle::Decimal),
        range(0, PageLabelStyle::RomanLower),
    };

    for (int i = 0; i < 10; ++i) {
        const std::string want = PageLabelManager::labelFor(i, ordered);
        EXPECT_EQ(PageLabelManager::labelFor(i, shuffled), want) << "page " << i;
        EXPECT_EQ(PageLabelManager::labelFor(i, reversed), want) << "page " << i;
    }

    // ... and the ordered answers are themselves the spec answers.
    EXPECT_EQ(PageLabelManager::labelFor(0, ordered), "i");
    EXPECT_EQ(PageLabelManager::labelFor(2, ordered), "iii");
    EXPECT_EQ(PageLabelManager::labelFor(3, ordered), "1");
    EXPECT_EQ(PageLabelManager::labelFor(5, ordered), "3");
    EXPECT_EQ(PageLabelManager::labelFor(6, ordered), "A-A");
    EXPECT_EQ(PageLabelManager::labelFor(8, ordered), "A-C");
}

// The last range has no upper bound: it labels every remaining page.
// KILLS: bounding the last range to the next entry that does not exist.
TEST(PageLabelsSpec, LastRangeExtendsToEveryHigherPageIndex)
{
    const std::vector<PageLabelRange> r{range(0, PageLabelStyle::Decimal)};
    EXPECT_EQ(PageLabelManager::labelFor(0, r), "1");
    EXPECT_EQ(PageLabelManager::labelFor(999, r), "1000");
    EXPECT_EQ(PageLabelManager::labelFor(100000, r), "100001");
}

// Pages preceding the first number-tree entry have no label (a malformed but
// perfectly parseable /PageLabels whose first key is not 0), and a negative
// index is not a page at all.
// KILLS: extrapolating the first range backwards -- which would invent labels
// for pages the document never labelled, and would compute a negative numeric
// portion.
TEST(PageLabelsSpec, PagesBeforeTheFirstRangeAndNegativeIndicesHaveNoLabel)
{
    const std::vector<PageLabelRange> r{range(3, PageLabelStyle::Decimal, "", 1)};
    EXPECT_EQ(PageLabelManager::labelFor(0, r), "");
    EXPECT_EQ(PageLabelManager::labelFor(1, r), "");
    EXPECT_EQ(PageLabelManager::labelFor(2, r), "");
    EXPECT_EQ(PageLabelManager::labelFor(3, r), "1");
    EXPECT_EQ(PageLabelManager::labelFor(4, r), "2");

    EXPECT_EQ(PageLabelManager::labelFor(-1, r), "");
    EXPECT_EQ(PageLabelManager::labelFor(-100, r), "");

    const std::vector<PageLabelRange> fromZero{range(0, PageLabelStyle::Decimal)};
    EXPECT_EQ(PageLabelManager::labelFor(-1, fromZero), "");
}

// A document with no /PageLabels shows raw page numbers; labelFor() reports
// that as an empty label so the caller can fall back.
// KILLS: returning "1" (or the index) for an empty range list.
TEST(PageLabelsSpec, EmptyRangeListYieldsAnEmptyLabel)
{
    EXPECT_EQ(PageLabelManager::labelFor(0, {}), "");
    EXPECT_EQ(PageLabelManager::labelFor(5, {}), "");
    EXPECT_EQ(PageLabelManager::labelFor(-1, {}), "");
}

// Two entries sharing a firstPage is malformed; whichever wins, the result
// must be one of the two candidates and must be deterministic across calls --
// never a crash and never a blend.
// KILLS: an unstable selection that could read past the end of the sorted
// vector.
TEST(PageLabelsSpec, DuplicateFirstPageEntriesResolveDeterministically)
{
    const std::vector<PageLabelRange> dup{
        range(0, PageLabelStyle::Decimal, "X", 1),
        range(0, PageLabelStyle::RomanUpper, "Y", 1),
    };
    const std::string first = PageLabelManager::labelFor(0, dup);
    EXPECT_TRUE(first == "X1" || first == "YI")
        << "expected one of the two declared ranges, got '" << first << "'";
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(PageLabelManager::labelFor(0, dup), first)
            << "selection must be deterministic across repeated calls";
    }
}

// A range list is not required to be contiguous or to start at 0; the "greatest
// key <= index" rule must still hold across gaps.
// KILLS: selecting by array position instead of by firstPage comparison.
TEST(PageLabelsSpec, SparseRangeKeysStillSelectTheGreatestKeyNotExceedingTheIndex)
{
    const std::vector<PageLabelRange> r{
        range(2, PageLabelStyle::RomanLower, "", 1),
        range(10, PageLabelStyle::Decimal, "", 1),
        range(11, PageLabelStyle::AlphaLower, "", 3),
    };
    EXPECT_EQ(PageLabelManager::labelFor(1, r), "");
    EXPECT_EQ(PageLabelManager::labelFor(2, r), "i");
    EXPECT_EQ(PageLabelManager::labelFor(9, r), "viii");
    EXPECT_EQ(PageLabelManager::labelFor(10, r), "1");
    EXPECT_EQ(PageLabelManager::labelFor(11, r), "c");   // /St 3 -> 'c'
    EXPECT_EQ(PageLabelManager::labelFor(12, r), "d");
}

}  // namespace
