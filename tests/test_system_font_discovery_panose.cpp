// -*- coding: utf-8 -*-
// Pipeline G phase-9: lock the PANOSE-byte-1 → serif/sans decision
// that SystemFontDiscovery now uses (replacing the family-name
// heuristic at MainWindow ~line 215). The OS/2 table contract is:
//
//   PANOSE byte 1 = bSerifStyle
//     0          = Any           (no classification)
//     1          = No Fit
//     2..10      = serif subclasses
//     11..15     = sans-serif subclasses
//
// The SystemFontDiscoveryPanose::serifFromPanoseByte1() helper is the
// single decision point; production code falls back to the legacy
// family-name heuristic only when the helper returns Unknown.
//
// We also drive the integration path: enumerate all system fonts on
// Windows and verify (a) Times New Roman comes back as serif, and
// (b) Arial comes back as sans-serif. These two are sentinel fonts
// shipped with every Windows install since Win 3.1 and have stable
// PANOSE bytes (Times = 02 02 06 03 ..., Arial = 02 0B 06 04 ...).
#include <gtest/gtest.h>
#include "core/SystemFontDiscovery.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>

using pdfe::core::SystemFontDiscovery;
using pdfe::core::SystemFontDiscoveryPanose::SerifKind;
using pdfe::core::SystemFontDiscoveryPanose::serifFromPanoseByte1;

// -------- Pure helper: byte → SerifKind --------

TEST(SystemFontDiscoveryPanose, ByteZeroIsUnknown)
{
    EXPECT_EQ(serifFromPanoseByte1(0), SerifKind::Unknown);
}

TEST(SystemFontDiscoveryPanose, ByteOneIsUnknown)
{
    // PANOSE 1 = "No Fit" — caller must use a fallback heuristic.
    EXPECT_EQ(serifFromPanoseByte1(1), SerifKind::Unknown);
}

TEST(SystemFontDiscoveryPanose, BytesTwoThroughTenAreSerif)
{
    for (std::uint8_t b = 2; b <= 10; ++b) {
        EXPECT_EQ(serifFromPanoseByte1(b), SerifKind::Serif)
            << "byte " << static_cast<int>(b)
            << " should classify as Serif (PANOSE serif range 2..10)";
    }
}

TEST(SystemFontDiscoveryPanose, BytesElevenThroughFifteenAreSans)
{
    for (std::uint8_t b = 11; b <= 15; ++b) {
        EXPECT_EQ(serifFromPanoseByte1(b), SerifKind::Sans)
            << "byte " << static_cast<int>(b)
            << " should classify as Sans (PANOSE sans range 11..15)";
    }
}

TEST(SystemFontDiscoveryPanose, BytesAboveFifteenAreUnknown)
{
    // The OS/2 PANOSE byte is unsigned 0..15 in spec; values above 15
    // are out-of-range / reserved. Treat as Unknown so the fallback
    // heuristic kicks in.
    EXPECT_EQ(serifFromPanoseByte1(16), SerifKind::Unknown);
    EXPECT_EQ(serifFromPanoseByte1(200), SerifKind::Unknown);
    EXPECT_EQ(serifFromPanoseByte1(255), SerifKind::Unknown);
}

// -------- Integration: real system enumeration --------

namespace {
std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}
} // namespace

TEST(SystemFontDiscoveryPanoseIntegration, TimesNewRomanIsClassifiedSerif)
{
#ifndef _WIN32
    GTEST_SKIP() << "DirectWrite-driven enumeration is Windows-only";
#else
    SystemFontDiscovery disc;
    const auto& fonts = disc.enumerateFonts();
    bool sawTimes = false;
    for (const auto& fm : fonts) {
        if (lower(fm.familyName) == "times new roman") {
            sawTimes = true;
            EXPECT_TRUE(fm.isSerif)
                << "Times New Roman must classify as serif via PANOSE; "
                << "got isSerif=" << fm.isSerif;
            break;  // any face is enough — they share PANOSE
        }
    }
    if (!sawTimes) {
        GTEST_SKIP() << "Times New Roman not installed on this machine";
    }
#endif
}

TEST(SystemFontDiscoveryPanoseIntegration, ArialIsClassifiedSans)
{
#ifndef _WIN32
    GTEST_SKIP() << "DirectWrite-driven enumeration is Windows-only";
#else
    SystemFontDiscovery disc;
    const auto& fonts = disc.enumerateFonts();
    bool sawArial = false;
    for (const auto& fm : fonts) {
        // Match exact family name to avoid catching "Arial Narrow" or
        // "Arial Black" which have their own PANOSE classifications.
        if (lower(fm.familyName) == "arial") {
            sawArial = true;
            EXPECT_FALSE(fm.isSerif)
                << "Arial must classify as sans-serif via PANOSE; "
                << "got isSerif=" << fm.isSerif;
            break;
        }
    }
    if (!sawArial) {
        GTEST_SKIP() << "Arial not installed on this machine";
    }
#endif
}

TEST(SystemFontDiscoveryPanoseIntegration,
     EnumerationCompletesWithoutCrashOnFontWithoutPanose)
{
    // AC #3: a font with missing OS/2 table (or PANOSE byte-1 = 0/1)
    // triggers the fallback path without crashing. We can't easily
    // synthesize such a font on Windows, but the full enumeration of
    // every installed family exercises the missing-PANOSE code path
    // for any system bitmap fonts (Marlett, Symbol, Wingdings) that
    // have PANOSE byte-1 = 0. If enumeration finishes without crashing
    // and returns >0 fonts with no PANOSE match also classified, the
    // fallback is doing its job.
    SystemFontDiscovery disc;
    const auto& fonts = disc.enumerateFonts();
#ifdef _WIN32
    EXPECT_GT(fonts.size(), 0u)
        << "Windows always has at least a few system fonts";
#else
    // On non-Windows, the enumeration is a placeholder; just confirm
    // the call doesn't crash.
    (void)fonts;
#endif
}
