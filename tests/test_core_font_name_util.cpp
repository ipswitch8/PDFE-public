// -*- coding: utf-8 -*-
//
// Unit tests for src/core/FontNameUtil.{h,cpp} -- GAP A1-37 (src/core had zero
// dedicated test files). Before this file NOTHING in tests/ named
// buildBaseFontMap, resolveUntruncatedFontName, buildFontStyleMap or
// resolveFontStyle: the only coverage was one end-to-end overlay-italic
// assertion in test_textedit_font_identity.cpp, which exercises exactly one
// font, one truncation shape and none of the failure modes.
//
// WHAT MUST BE TRUE (all from FontNameUtil.h's own contract plus PDF 32000-1):
//
//  1. MuPDF stores a font name in `char name[32]`, so fz_font_name() clips any
//     /BaseFont past 31 characters. buildBaseFontMap keys the untruncated name
//     under that 31-char prefix, and ALSO under the subset-prefix-stripped
//     form, "so a lookup with the value from fz_font_name resolves regardless
//     of whether MuPDF kept the ABCDEF+ prefix".
//  2. resolveUntruncatedFontName upgrades only to a STRICTLY LONGER name --
//     "fonts whose name was never truncated are returned byte-identical".
//  3. "If two distinct /BaseFont values collide on the same 31-char key the
//     entry is marked ambiguous (resolution then yields no change, never a
//     wrong attribution)."
//  4. The walk descends "one level deep into Form XObject resources".
//  5. buildFontStyleMap reads italic/bold STRUCTURALLY from /FontDescriptor --
//     /Flags bit 7 (Italic, 64), non-zero /ItalicAngle, /Flags bit 19
//     (ForceBold, 0x40000), heavy /StemV -- and for a Type0 composite reads
//     the DESCENDANT CIDFont's descriptor (PDF 32000-1 sec. 9.7.4). It is
//     name-INDEPENDENT: that independence is the entire reason the module
//     exists, since name-scanning is what lost italic in the first place.
//  6. "Never throws / never longjmps"; a null ctx/page yields an empty map.
//
// Fixture: font_descriptor_matrix.pdf (tests/fixtures/gen_font_descriptor_
// matrix.py) authors every one of those shapes explicitly, with /BaseFont
// values deliberately named after the DESCRIPTOR FACT rather than the visual
// style, so a name-scanning implementation cannot pass.

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/FontNameUtil.h"
#include "core/MuPdfContext.h"

#include <filesystem>
#include <memory>
#include <string>

using pdfe::core::BaseFontMap;
using pdfe::core::buildBaseFontMap;
using pdfe::core::buildFontStyleMap;
using pdfe::core::Document;
using pdfe::core::FontStyleFlags;
using pdfe::core::FontStyleMap;
using pdfe::core::MuPdfContext;
using pdfe::core::resolveFontStyle;
using pdfe::core::resolveUntruncatedFontName;

namespace {

/// MuPDF's usable fz_font.name capacity (char[32] minus the terminator).
constexpr std::size_t kFzNameCap = 31;

/// The literal /BaseFont values authored by gen_font_descriptor_matrix.py.
constexpr const char* kCollideStem = "SameFirstThirtyOneCharsPrefixXX";
constexpr const char* kCollideAlpha = "SameFirstThirtyOneCharsPrefixXXAlpha";
constexpr const char* kCollideBeta = "SameFirstThirtyOneCharsPrefixXXBeta";
constexpr const char* kNestedFull = "AAAAAA+XObjectNestedLongFontNameItalic";
constexpr const char* kShortPrefixed = "BBBBBB+ShortPrefixedName";

std::string fixturesDir() { return TEST_FIXTURES_DIR; }

/// Truncate exactly the way fz_font_name() does.
std::string asFzName(const std::string& baseFont)
{
    return baseFont.size() > kFzNameCap ? baseFont.substr(0, kFzNameCap)
                                        : baseFont;
}

// ===========================================================================
// Pure resolver behaviour -- authored maps, no PDF involved
// ===========================================================================

// An empty map means "nothing recovered"; the caller must fall back to the
// truncated name rather than getting an empty string.
// KILLS: returning map[fzName] via operator[] / returning "" on a miss.
TEST(CoreFontNameUtilResolve, MissingKeyReturnsTheInputUnchanged)
{
    const BaseFontMap empty;
    EXPECT_EQ(resolveUntruncatedFontName(empty, "Helvetica"), "Helvetica");
    EXPECT_EQ(resolveUntruncatedFontName(empty, ""), "");

    BaseFontMap m;
    m["SomethingElse"] = "SomethingElseEntirelyLonger";
    EXPECT_EQ(resolveUntruncatedFontName(m, "Helvetica"), "Helvetica");
}

// Contract: the "strictly longer" guard. A stored name that is the same length
// as (or shorter than) the lookup key carries no information, and swapping it
// in would silently rewrite a name that was never truncated.
// KILLS: changing `it->second.size() > fzName.size()` to `!=`, `>=`, or
// dropping the comparison entirely.
TEST(CoreFontNameUtilResolve, OnlyAStrictlyLongerStoredNameIsSubstituted)
{
    BaseFontMap m;
    m["Foo"] = "Foo";               // equal length -> no gain
    m["Bar"] = "Ba";                // shorter -> must never be used
    m["Baz"] = "BazLongerVersion";  // longer -> substitute

    EXPECT_EQ(resolveUntruncatedFontName(m, "Foo"), "Foo");
    EXPECT_EQ(resolveUntruncatedFontName(m, "Bar"), "Bar")
        << "a shorter stored value must never replace the caller's name";
    EXPECT_EQ(resolveUntruncatedFontName(m, "Baz"), "BazLongerVersion");
}

// Lookup is by exact key. A prefix/substring match would attribute one font's
// full name to a different font.
// KILLS: replacing find() with a prefix scan over the map.
TEST(CoreFontNameUtilResolve, LookupIsExactNotPrefixMatched)
{
    BaseFontMap m;
    m["ArialMT"] = "ArialMT-ExtendedLongVariant";

    EXPECT_EQ(resolveUntruncatedFontName(m, "Arial"), "Arial");
    EXPECT_EQ(resolveUntruncatedFontName(m, "ArialM"), "ArialM");
    EXPECT_EQ(resolveUntruncatedFontName(m, "ArialMTBold"), "ArialMTBold");
    EXPECT_EQ(resolveUntruncatedFontName(m, "ArialMT"),
              "ArialMT-ExtendedLongVariant");
}

// Contract: absent entry => {false, false}, i.e. "unknown", not "not italic
// and definitely not bold". Callers use it to decide whether to fall back to
// name parsing at all.
// KILLS: returning an uninitialised FontStyleFlags, or defaulting to true.
TEST(CoreFontNameUtilResolve, StyleLookupDefaultsToBothFlagsFalse)
{
    const FontStyleMap empty;
    const FontStyleFlags none = resolveFontStyle(empty, "Whatever");
    EXPECT_FALSE(none.isItalic);
    EXPECT_FALSE(none.isBold);

    FontStyleMap m;
    m["A"] = FontStyleFlags{true, false};
    m["B"] = FontStyleFlags{false, true};
    m["C"] = FontStyleFlags{true, true};

    EXPECT_TRUE(resolveFontStyle(m, "A").isItalic);
    EXPECT_FALSE(resolveFontStyle(m, "A").isBold);
    EXPECT_FALSE(resolveFontStyle(m, "B").isItalic);
    EXPECT_TRUE(resolveFontStyle(m, "B").isBold);
    EXPECT_TRUE(resolveFontStyle(m, "C").isItalic);
    EXPECT_TRUE(resolveFontStyle(m, "C").isBold);
    EXPECT_FALSE(resolveFontStyle(m, "D").isItalic);
    EXPECT_FALSE(resolveFontStyle(m, "D").isBold);
}

// Contract: "Never throws / never longjmps". A null page is the shape reached
// whenever a caller holds a non-PDF fz_page (an image or XPS page).
// KILLS: removing the `if (!ctx || !page) return out;` guard -- a null
// dereference, not a wrong answer.
TEST(CoreFontNameUtilResolve, NullArgumentsYieldEmptyMapsWithoutCrashing)
{
    auto ctx = std::make_shared<MuPdfContext>();

    EXPECT_TRUE(buildBaseFontMap(nullptr, nullptr).empty());
    EXPECT_TRUE(buildBaseFontMap(ctx->get(), nullptr).empty());
    EXPECT_TRUE(buildFontStyleMap(nullptr, nullptr).empty());
    EXPECT_TRUE(buildFontStyleMap(ctx->get(), nullptr).empty());
}

// ===========================================================================
// Fixture-backed: the real resource walk
// ===========================================================================

// The maps are built from a raw fz_page, exactly as TextExtractor and
// AutoTagger call them (Page::fzPage() is private to friend classes, and
// going through MuPDF directly also keeps this file a UNIT test of
// FontNameUtil rather than of Document/Page).
class CoreFontNameUtilMatrix : public ::testing::Test {
protected:
    void SetUp() override
    {
        ctx_ = std::make_shared<MuPdfContext>();
        const auto p =
            std::filesystem::path(fixturesDir()) / "font_descriptor_matrix.pdf";
        ASSERT_TRUE(std::filesystem::exists(p))
            << "missing fixture " << p.string()
            << " -- regenerate with: python tests/fixtures/"
               "gen_font_descriptor_matrix.py";

        // Document::open() first, so this file still fails loudly if the
        // fixture ever stops being an openable document.
        auto opened = Document::open(ctx_, p);
        ASSERT_TRUE(opened) << "fixture is not openable as a Document";
        opened.reset();

        const std::string utf8 = p.string();
        fzDoc_ = ctx_->executeReturn<fz_document*>(
            [&utf8](fz_context* c) { return fz_open_document(c, utf8.c_str()); });
        ASSERT_NE(fzDoc_, nullptr);
        fzPage_ = ctx_->executeReturn<fz_page*>(
            [this](fz_context* c) { return fz_load_page(c, fzDoc_, 0); });
        ASSERT_NE(fzPage_, nullptr);

        baseMap_ = buildBaseFontMap(ctx_->get(), fzPage_);
        styleMap_ = buildFontStyleMap(ctx_->get(), fzPage_);
    }

    void TearDown() override
    {
        if (fzPage_) fz_drop_page(ctx_->get(), fzPage_);
        if (fzDoc_) fz_drop_document(ctx_->get(), fzDoc_);
        fzPage_ = nullptr;
        fzDoc_ = nullptr;
    }

    /// Style flags for `baseFont` looked up the way a caller does: with the
    /// name fz_font_name() would have produced.
    FontStyleFlags styleOf(const std::string& baseFont) const
    {
        return resolveFontStyle(styleMap_, asFzName(baseFont));
    }

    std::shared_ptr<MuPdfContext> ctx_;
    fz_document* fzDoc_ = nullptr;
    fz_page* fzPage_ = nullptr;
    BaseFontMap baseMap_;
    FontStyleMap styleMap_;
};

// The map must be keyed by the TRUNCATED name (what fz_font_name returns) and
// hold the FULL name -- that is the whole mechanism.
// KILLS: dropping the `candidate.substr(0, 31)` keying (the truncated lookup
// would miss and the caller would keep the clipped name), or storing the key
// instead of the value.
TEST_F(CoreFontNameUtilMatrix, TruncatedKeyResolvesToTheUntruncatedBaseFont)
{
    const std::string full = kNestedFull;                 // 38 chars
    const std::string clipped = asFzName(full);           // 31 chars
    ASSERT_GT(full.size(), kFzNameCap);
    ASSERT_EQ(clipped.size(), kFzNameCap);
    ASSERT_EQ(clipped.find("Italic"), std::string::npos)
        << "fixture invariant: the style keyword must sit PAST the 31-char cap";

    EXPECT_EQ(resolveUntruncatedFontName(baseMap_, clipped), full)
        << "the 31-char fz_font_name must resolve back to the full /BaseFont";
    EXPECT_NE(resolveUntruncatedFontName(baseMap_, clipped).find("Italic"),
              std::string::npos);
}

// Contract: keys are stored "both with and without the subset prefix".
// KILLS: deleting the second `add1(baseFont.substr(plus + 1))` call.
TEST_F(CoreFontNameUtilMatrix, SubsetPrefixStrippedAliasAlsoResolves)
{
    // Long name: the prefix-stripped form is 31 chars, so it is NOT truncated
    // and appears as its own key mapping to the full 38-char name.
    const std::string full = kNestedFull;
    const std::string stripped = full.substr(full.find('+') + 1);
    ASSERT_EQ(stripped.size(), kFzNameCap);
    EXPECT_EQ(resolveUntruncatedFontName(baseMap_, stripped), full)
        << "a lookup with the prefix-STRIPPED name must also recover the full "
           "/BaseFont, since MuPDF may or may not have kept the ABCDEF+ prefix";

    // Short name: both forms are present, and the with-prefix form is returned
    // byte-identical (it is already the full name, no gain).
    const std::string shortFull = kShortPrefixed;
    const std::string shortStripped =
        shortFull.substr(shortFull.find('+') + 1);
    ASSERT_LT(shortFull.size(), kFzNameCap);
    EXPECT_EQ(resolveUntruncatedFontName(baseMap_, shortFull), shortFull);
    EXPECT_EQ(resolveUntruncatedFontName(baseMap_, shortStripped), shortFull)
        << "the stripped alias must map back to the prefixed /BaseFont";
}

// Contract: "If two distinct /BaseFont values collide on the same 31-char key
// the entry is marked ambiguous (resolution then yields no change, NEVER a
// wrong attribution)." Guessing here means silently relabelling one font as
// another.
// KILLS: removing the collision sentinel (`it->second = key`) -- resolution
// would then return whichever of the two names happened to be inserted first.
TEST_F(CoreFontNameUtilMatrix, AmbiguousThirtyOneCharPrefixRefusesToGuess)
{
    const std::string alpha = kCollideAlpha;
    const std::string beta = kCollideBeta;
    const std::string key = kCollideStem;

    ASSERT_EQ(key.size(), kFzNameCap);
    ASSERT_EQ(asFzName(alpha), key);
    ASSERT_EQ(asFzName(beta), key);
    ASSERT_NE(alpha, beta);

    const std::string resolved = resolveUntruncatedFontName(baseMap_, key);
    EXPECT_EQ(resolved, key)
        << "an ambiguous 31-char prefix must resolve to itself, not to one of "
           "the colliding candidates; got '" << resolved << "'";
    EXPECT_NE(resolved, alpha);
    EXPECT_NE(resolved, beta);
}

// Contract: the walk descends "one level deep into Form XObject resources".
// A font used only inside a form XObject is a completely ordinary shape (every
// stamp, watermark and imported page produces one).
// KILLS: deleting the /XObject recursion in collectFonts().
TEST_F(CoreFontNameUtilMatrix, FontsInsideAFormXObjectAreReached)
{
    // kNestedFull lives ONLY in the form XObject's own /Resources /Font.
    const std::string clipped = asFzName(kNestedFull);
    EXPECT_EQ(resolveUntruncatedFontName(baseMap_, clipped), kNestedFull)
        << "a font reachable only through a Form XObject's resources was not "
           "collected -- the XObject recursion is gone";

    // Sanity: page-level fonts are still there too, so the test above cannot
    // pass by the walk having been replaced with something broader.
    EXPECT_FALSE(baseMap_.empty());
    EXPECT_EQ(resolveUntruncatedFontName(baseMap_, kShortPrefixed),
              kShortPrefixed);
}

// ---------------------------------------------------------------------------
// Descriptor-driven style flags. Authored ground truth per font, from
// gen_font_descriptor_matrix.py (PDF 32000-1 sec. 9.8.1 Table 122).
// ---------------------------------------------------------------------------

// /Flags bit 7 (value 64) is the Italic flag.
// KILLS: testing the wrong bit (e.g. 1<<7 instead of 1<<6), or dropping the
// /Flags read and relying on /ItalicAngle alone.
TEST_F(CoreFontNameUtilMatrix, ItalicFromFlagsBitSeven)
{
    // /Flags 64, /ItalicAngle 0, /StemV 80.
    const FontStyleFlags f = styleOf("ItalicByFlagBit");
    EXPECT_TRUE(f.isItalic) << "/Flags bit 7 (64) must set italic";
    EXPECT_FALSE(f.isBold) << "/StemV 80 with no ForceBold is not bold";
}

// A non-zero /ItalicAngle means italic even when the Italic flag bit is CLEAR
// -- a very common real-world combination, and the reason the descriptor read
// cannot be flags-only.
// KILLS: dropping the `|| italicAngle != 0.0` term.
TEST_F(CoreFontNameUtilMatrix, ItalicFromNonZeroItalicAngleWithFlagBitClear)
{
    // /Flags 32 (Nonsymbolic, italic bit CLEAR), /ItalicAngle -12, /StemV 80.
    const FontStyleFlags f = styleOf("ItalicByAngle");
    EXPECT_TRUE(f.isItalic)
        << "a non-zero /ItalicAngle must set italic even with /Flags bit 7 clear";
    EXPECT_FALSE(f.isBold);
}

// /Flags bit 19 (0x40000) is ForceBold.
// KILLS: dropping the ForceBold term, or testing the wrong bit.
TEST_F(CoreFontNameUtilMatrix, BoldFromForceBoldFlag)
{
    // /Flags 32|0x40000, /ItalicAngle 0, /StemV 80.
    const FontStyleFlags f = styleOf("BoldByForceBold");
    EXPECT_TRUE(f.isBold) << "/Flags bit 19 (ForceBold) must set bold";
    EXPECT_FALSE(f.isItalic) << "ForceBold says nothing about slant";
}

// The /StemV bold proxy is documented as `stemV >= 140.0`. Both sides of the
// threshold are pinned so the constant cannot drift unnoticed -- drifting it
// down would make ordinary regular faces render bold in the edit overlay.
// KILLS: changing the threshold in either direction, or switching `>=` to `>`.
TEST_F(CoreFontNameUtilMatrix, StemVBoldThresholdIsPinnedOnBothSides)
{
    // /StemV 140 -- exactly at the documented threshold.
    const FontStyleFlags at = styleOf("BoldByStemV140");
    EXPECT_TRUE(at.isBold) << "/StemV 140 is at the documented >= 140 boundary";
    EXPECT_FALSE(at.isItalic);

    // /StemV 139 -- one unit below.
    const FontStyleFlags below = styleOf("NotBoldStemV139");
    EXPECT_FALSE(below.isBold)
        << "/StemV 139 is below the documented threshold and must NOT be bold";
    EXPECT_FALSE(below.isItalic);
}

// Contract: "Handles a missing /FontDescriptor gracefully (entry defaults to
// false/false)." The 14 standard Type1 fonts have no descriptor at all.
// KILLS: dereferencing the absent descriptor, or falling back to name parsing.
TEST_F(CoreFontNameUtilMatrix, MissingFontDescriptorYieldsNeitherItalicNorBold)
{
    const FontStyleFlags f = styleOf("NoDescriptorFont");
    EXPECT_FALSE(f.isItalic);
    EXPECT_FALSE(f.isBold);
}

// PDF 32000-1 sec. 9.7.4: for a Type0 composite the /FontDescriptor lives on
// the descendant CIDFont, not on the top-level font dict.
// KILLS: deleting the /DescendantFonts fallback in readDescriptorStyle() --
// every CJK / Identity-H document would silently lose its style flags.
TEST_F(CoreFontNameUtilMatrix, Type0CompositeReadsTheDescendantCidFontDescriptor)
{
    // Descendant descriptor: /Flags 64, /ItalicAngle -15, /StemV 190.
    const FontStyleFlags f = styleOf("CidBoldItalicComposite");
    EXPECT_TRUE(f.isItalic)
        << "the descendant CIDFont's /FontDescriptor was not consulted";
    EXPECT_TRUE(f.isBold)
        << "the descendant CIDFont's /StemV 190 was not consulted";
}

// The module exists BECAUSE name-scanning is unreliable. Every /BaseFont in the
// fixture is named after its descriptor fact, and several names would produce
// the OPPOSITE answer under a keyword scan. This test states that directly.
// KILLS: any reintroduction of name-keyword sniffing inside buildFontStyleMap
// (e.g. `isItalic = name.contains("Italic")`).
TEST_F(CoreFontNameUtilMatrix, StyleFlagsAreNameIndependent)
{
    // "NotBoldStemV139" contains neither keyword and is correctly neither.
    EXPECT_FALSE(styleOf("NotBoldStemV139").isBold);

    // "BoldByForceBold" contains "Bold" AND is bold -- but "ItalicByFlagBit"
    // and "ItalicByAngle" contain "Italic" and are italic, while
    // "BoldByStemV140" contains "Bold" and is bold. The discriminating case is
    // the XObject-nested font: its /BaseFont ends in "Italic" yet it has NO
    // /FontDescriptor at all, so the structural answer is NOT italic.
    const FontStyleFlags nested = styleOf(kNestedFull);
    EXPECT_FALSE(nested.isItalic)
        << "'" << kNestedFull << "' has no /FontDescriptor; a structural read "
           "must report NOT italic. Reporting italic here means the "
           "implementation is scanning the NAME again -- the exact defect "
           "FontNameUtil was written to remove.";
    EXPECT_FALSE(nested.isBold);
}

// The style map must be keyed identically to the base-font map (header:
// "Keyed identically to BaseFontMap so a lookup with fz_font_name resolves"),
// including the subset-prefix alias.
// KILLS: keying the style map by the untruncated name only.
TEST_F(CoreFontNameUtilMatrix, StyleMapUsesTheSameTruncatedAndStrippedKeys)
{
    const std::string full = kNestedFull;
    const std::string clipped = asFzName(full);
    const std::string stripped = full.substr(full.find('+') + 1);

    EXPECT_EQ(styleMap_.count(clipped), 1u)
        << "style map must be reachable with the 31-char fz_font_name";
    EXPECT_EQ(styleMap_.count(stripped), 1u)
        << "style map must also carry the subset-prefix-stripped alias";

    // And a font whose name needs no truncation is keyed by itself.
    EXPECT_EQ(styleMap_.count("ItalicByFlagBit"), 1u);
}

}  // namespace
