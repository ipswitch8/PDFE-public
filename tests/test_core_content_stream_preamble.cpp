// -*- coding: utf-8 -*-
//
// Unit tests for pdfe::core::knownTextStateBasis()
// (src/core/ContentStreamPreamble.h) -- GAP A1-37, src/core dedicated coverage.
//
// WHY THIS MATTERS
// ----------------
// knownTextStateBasis() is the single definition of the "known basis" that
// every page-content INJECTOR (AlignmentWriter, HeaderFooterEngine, the batch
// watermark, TRE writeTextBlock) emits right after its `q`. PDF concatenates an
// appended content stream onto the preceding page content with NO graphics-state
// reset, so whatever the page left active leaks into the fragment. The
// content-stream state-continuity audit (docs/content-stream-state-continuity-
// audit.md, Register H1) traced a whole family of REAL defects to exactly that:
//
//   * an ambient non-black fill  -> injected text invisible,
//   * an ambient `-100 Tz`       -> injected text MIRRORED,
//   * an ambient Tc/Tw/Ts        -> distorted spacing / shifted baseline,
//   * an ambient Tr 3            -> invisible render mode.
//
// The only existing coverage (test_content_stream_ctm_reset.cpp) drives the
// four injectors END TO END and never asserts the emitted operator string, so
// the one function they all share had no unit oracle at all.
//
// AUTHORED GROUND TRUTH:
//   * PDF 32000-1 sec. 8.6.8 / Table 74 -- `rg` sets the nonstroking colour in
//     DeviceRGB from three components in the range 0.0-1.0, in RED GREEN BLUE
//     order.
//   * PDF 32000-1 sec. 9.3 / Table 105 -- Tz is a PERCENTAGE (100 = normal),
//     Tc / Tw / Ts are unscaled text-space units (0 = normal), Tr 0 = fill.
//   * ContentStreamPreamble.h -- "this preamble intentionally does NOT emit a
//     `cm`. A `cm` multiplies the current transform rather than replacing it,
//     so it cannot neutralise an arbitrary ambient CTM." CTM isolation belongs
//     to PageContentGuard, not here.

#include <gtest/gtest.h>

#include "core/ContentStreamPreamble.h"

#include <cstdint>
#include <string>

using pdfe::core::knownTextStateBasis;

namespace {

std::size_t indexOf(const std::string& s, const char* needle)
{
    return s.find(needle);
}

// The exact default emission. Every byte is spec-determined: black DeviceRGB
// fill, 100% horizontal scale, zero char/word spacing, zero rise, fill render
// mode -- each on its own line so the fragment is legal wherever it is spliced.
// KILLS: dropping ANY operator, reordering them, changing 100 Tz to 0 Tz,
// changing 0 Tr to 3 Tr, or emitting them space-separated on one line without
// the trailing newline.
TEST(CoreContentStreamPreamble, DefaultBasisIsTheExactDocumentedOperatorSequence)
{
    EXPECT_EQ(knownTextStateBasis(),
              "0 0 0 rg\n100 Tz\n0 Tc\n0 Tw\n0 Ts\n0 Tr\n");
}

// Each ambient-state defect from Register H1 has one operator that neutralises
// it. Naming them individually means a deletion reports WHICH defect class was
// re-opened, not just "string differs".
// KILLS: removing any single reset (each EXPECT names the defect it re-opens).
TEST(CoreContentStreamPreamble, EveryAmbientStateDefectHasItsNeutralisingOperator)
{
    const std::string s = knownTextStateBasis();

    EXPECT_NE(indexOf(s, "rg"), std::string::npos)
        << "no fill-colour reset: an ambient non-black/white fill hides the "
           "injected text";
    EXPECT_NE(indexOf(s, "100 Tz"), std::string::npos)
        << "no horizontal-scale reset: an ambient -100 Tz MIRRORS the text";
    EXPECT_NE(indexOf(s, "0 Tc"), std::string::npos)
        << "no character-spacing reset: an ambient Tc distorts spacing";
    EXPECT_NE(indexOf(s, "0 Tw"), std::string::npos)
        << "no word-spacing reset: an ambient Tw distorts spacing";
    EXPECT_NE(indexOf(s, "0 Ts"), std::string::npos)
        << "no text-rise reset: an ambient Ts shifts the baseline";
    EXPECT_NE(indexOf(s, "0 Tr"), std::string::npos)
        << "no render-mode reset: an ambient Tr 3 makes the text invisible";
}

// The colour must be established BEFORE the text-state operators, so a partial
// splice (or a reader stopping early) never applies text state under the
// ambient colour.
// KILLS: shuffling the snprintf format string.
TEST(CoreContentStreamPreamble, FillColourIsEstablishedBeforeTheTextState)
{
    const std::string s = knownTextStateBasis();
    const std::size_t rg = indexOf(s, "rg");
    const std::size_t tz = indexOf(s, "Tz");
    const std::size_t tc = indexOf(s, "Tc");
    const std::size_t tw = indexOf(s, "Tw");
    const std::size_t ts = indexOf(s, "Ts");
    const std::size_t tr = indexOf(s, "Tr");

    ASSERT_NE(rg, std::string::npos);
    EXPECT_LT(rg, tz);
    EXPECT_LT(tz, tc);
    EXPECT_LT(tc, tw);
    EXPECT_LT(tw, ts);
    EXPECT_LT(ts, tr);
}

// PDF 32000-1 Table 74: `rg` takes RED GREEN BLUE, each 0.0-1.0. The primaries
// are the only inputs that distinguish a correct channel order from BGR.
// KILLS: swapping the shift amounts (a BGR emission) -- 0xFF0000 would emit
// "0 0 1 rg" and every red watermark would come out blue.
TEST(CoreContentStreamPreamble, ChannelsAreEmittedInRedGreenBlueOrder)
{
    EXPECT_EQ(knownTextStateBasis(0xFF0000u).substr(0, 11), "1 0 0 rg\n10");
    EXPECT_EQ(knownTextStateBasis(0x00FF00u).substr(0, 11), "0 1 0 rg\n10");
    EXPECT_EQ(knownTextStateBasis(0x0000FFu).substr(0, 11), "0 0 1 rg\n10");
    EXPECT_EQ(knownTextStateBasis(0xFFFFFFu).substr(0, 11), "1 1 1 rg\n10");

    // An asymmetric colour pins all three channels at once.
    // 0x12/255 = 0.0705882..., 0x34/255 = 0.203922..., 0x56/255 = 0.337255...
    EXPECT_EQ(knownTextStateBasis(0x123456u),
              "0.0705882 0.203922 0.337255 rg\n"
              "100 Tz\n0 Tc\n0 Tw\n0 Ts\n0 Tr\n");
}

// Components are scaled by 255 (the maximum 8-bit value), not 256, and printed
// with six significant digits. Dividing by 256 shifts every non-zero channel
// and makes pure white 0.996094 instead of 1.
// KILLS: `/ 256.0`, `/ 255.0f` narrowing, or a different precision.
TEST(CoreContentStreamPreamble, ComponentsScaleByTwoFiveFiveWithSixSigDigits)
{
    EXPECT_EQ(knownTextStateBasis(0xFFFFFFu).substr(0, 8), "1 1 1 rg")
        << "255/255 must be exactly 1, so the divisor is 255 and not 256";
    EXPECT_EQ(knownTextStateBasis(0x808080u).substr(0, 26),
              "0.501961 0.501961 0.501961");
    EXPECT_EQ(knownTextStateBasis(0x7F7F7Fu).substr(0, 26),
              "0.498039 0.498039 0.498039");
    // Smallest non-zero components -- %.6g keeps six significant digits, not
    // six decimal places, so these must NOT collapse to 0.
    EXPECT_EQ(knownTextStateBasis(0x010203u).substr(0, 33),
              "0.00392157 0.00784314 0.0117647 r");
}

// Only the low 24 bits are colour. Callers pass through Qt/AppSettings values
// that can carry an alpha byte in bits 24-31; those must be masked off rather
// than driving the red channel out of range (a component > 1.0 is invalid per
// Table 74 and MuPDF clamps it, silently producing pure red).
// KILLS: removing the `& 0xFFu` masks.
TEST(CoreContentStreamPreamble, BitsAboveTwentyFourAreIgnored)
{
    EXPECT_EQ(knownTextStateBasis(0xFF000000u | 0x00FF00u),
              knownTextStateBasis(0x00FF00u));
    EXPECT_EQ(knownTextStateBasis(0xAB123456u), knownTextStateBasis(0x123456u));
    EXPECT_EQ(knownTextStateBasis(0xFFFFFFFFu), knownTextStateBasis(0xFFFFFFu));
}

// Header: the preamble "intentionally does NOT emit a `cm`". Adding one here
// would be actively wrong -- `cm` multiplies, so it cannot neutralise an
// arbitrary ambient CTM, and callers would stop routing through
// PageContentGuard's `q ... Q` bracket, which is what actually works.
// KILLS: "fixing" the CTM leak in the preamble instead of in PageContentGuard.
TEST(CoreContentStreamPreamble, EmitsNoCtmOperatorAndNoStateBracketing)
{
    const std::string s = knownTextStateBasis(0x336699u);

    EXPECT_EQ(indexOf(s, "cm"), std::string::npos)
        << "the preamble must not emit a `cm`; CTM isolation is "
           "PageContentGuard's q...Q bracket";
    EXPECT_EQ(indexOf(s, "BT"), std::string::npos)
        << "the preamble is state only; it must not open a text object";
    EXPECT_EQ(indexOf(s, "ET"), std::string::npos);
    EXPECT_EQ(indexOf(s, "Tf"), std::string::npos)
        << "the preamble must not select a font -- the caller owns that";
    // `q`/`Q` are the caller's responsibility too (the preamble is emitted
    // immediately AFTER the fragment's own `q`).
    EXPECT_EQ(s.find('q'), std::string::npos);
    EXPECT_EQ(s.find('Q'), std::string::npos);
}

// The result is appended directly onto a content stream, so it must end on a
// newline (otherwise the caller's next operator fuses onto "0 Tr") and must
// never be truncated by the fixed 96-byte snprintf buffer. The worst case is
// three 10-character components, which is 58 bytes -- but a shrunk buffer or a
// widened precision would silently clip the trailing operators.
// KILLS: shrinking `char buf[96]`, or widening the precision past what fits.
TEST(CoreContentStreamPreamble, EveryColourProducesACompleteNewlineTerminatedString)
{
    for (std::uint32_t rgb = 0; rgb <= 0xFFFFFFu; rgb += 0x0417u) {
        const std::string s = knownTextStateBasis(rgb);
        ASSERT_FALSE(s.empty()) << "rgb=" << rgb;
        ASSERT_EQ(s.back(), '\n')
            << "rgb=" << rgb << " does not end on a newline: '" << s << "'";
        ASSERT_NE(s.rfind("0 Tr\n"), std::string::npos)
            << "rgb=" << rgb << " truncated before the final operator: '" << s
            << "'";
        ASSERT_LT(s.size(), static_cast<std::size_t>(96))
            << "rgb=" << rgb << " overran the documented buffer size";
    }
}

}  // namespace
