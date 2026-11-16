// Direct tests of the POSIX fontconfig layer (src/core/FontconfigSupport.cpp,
// GitHub #390). Before these, the layer was exercised only indirectly, through
// selector tests that need a large desktop font set. Each case states the claim
// it backs and is built so that it cannot pass vacuously.
//
// The whole file is POSIX-only by construction: FontconfigSupport does not
// exist on Windows, where SystemFontDiscovery enumerates through DirectWrite.

#include <gtest/gtest.h>

#if !defined(_WIN32)

#include "core/FontconfigSupport.h"
#include "core/FontMetrics.h"

#include <fontconfig/fontconfig.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <string>

namespace fc = pdfe::core::fontconfig;

namespace {

/// The container format from the file's own first bytes, independently of
/// fontconfig: "sfnt" (TrueType or OpenType/CFF), "ttc", or "other".
std::string sniffFormat(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    unsigned char b[4] = {0, 0, 0, 0};
    if (!f.read(reinterpret_cast<char*>(b), 4)) return "unreadable";
    if ((b[0] == 0x00 && b[1] == 0x01 && b[2] == 0x00 && b[3] == 0x00) ||
        std::memcmp(b, "true", 4) == 0 || std::memcmp(b, "OTTO", 4) == 0)
        return "sfnt";
    if (std::memcmp(b, "ttcf", 4) == 0) return "ttc";
    return "other";
}

/// How many Type 1 faces fontconfig itself knows about. The format filter is
/// only proven if there was something to filter.
int countType1Faces()
{
    FcPattern* pat = FcPatternCreate();
    FcObjectSet* os = FcObjectSetBuild(FC_FONTFORMAT, static_cast<char*>(nullptr));
    FcFontSet* set = FcFontList(nullptr, pat, os);
    int n = 0;
    for (int i = 0; set && i < set->nfont; ++i) {
        FcChar8* fmt = nullptr;
        if (FcPatternGetString(set->fonts[i], FC_FONTFORMAT, 0, &fmt) == FcResultMatch &&
            fmt && std::strcmp(reinterpret_cast<const char*>(fmt), "Type 1") == 0)
            ++n;
    }
    if (set) FcFontSetDestroy(set);
    FcObjectSetDestroy(os);
    FcPatternDestroy(pat);
    return n;
}

const pdfe::core::FontMetrics* findFamily(const std::vector<pdfe::core::FontMetrics>& fonts,
                                          const std::string& family)
{
    auto it = std::find_if(fonts.begin(), fonts.end(),
                           [&](const auto& m) { return m.familyName == family; });
    return it == fonts.end() ? nullptr : &*it;
}

}  // namespace

// CLAIM: listFonts() returns TrueType and CFF faces only. Type 1 (.t1/.pfb) and
// bitmap formats carry no OS/2 table, and a Type 1 Nimbus Roman was being
// scored as a sans candidate.
TEST(FontconfigSupport, ListFontsReturnsOnlySfntFaces)
{
    ASSERT_GT(countType1Faces(), 0)
        << "no Type 1 faces installed, so the format filter has nothing to "
           "filter and this case would pass vacuously. Install fonts-urw-base35 "
           "(docs/building-linux.md).";

    const auto fonts = fc::listFonts();
    ASSERT_FALSE(fonts.empty()) << "fontconfig enumerated no scalable faces";
    for (const auto& m : fonts) {
        const std::string fmt = sniffFormat(m.filePath);
        EXPECT_TRUE(fmt == "sfnt" || fmt == "ttc")
            << m.familyName << " (" << m.filePath << ") has container format "
            << fmt << "; only TrueType/CFF faces may be enumerated";
    }
}

// CLAIM: designedForLatin is false for a face fontconfig says does not support
// English, and true for a Latin face, so Latin-only text skips the former.
TEST(FontconfigSupport, DesignedForLatinFollowsFontconfigLanguageSupport)
{
    const auto fonts = fc::listFonts();
    const auto* latin = findFamily(fonts, "DejaVu Sans");
    ASSERT_NE(latin, nullptr) << "DejaVu Sans (fonts-dejavu-core) is required";
    EXPECT_TRUE(latin->designedForLatin);

    const auto nonLatin = std::find_if(fonts.begin(), fonts.end(),
        [](const auto& m) { return !m.designedForLatin; });
    ASSERT_NE(nonLatin, fonts.end())
        << "no face is marked not-designed-for-Latin; either the flag is never "
           "set or no script-only face is installed (fonts-noto-core has many)";

    // The flag must agree with fontconfig's own language coverage for that file.
    FcPattern* pat = FcPatternCreate();
    FcPatternAddString(pat, FC_FILE, reinterpret_cast<const FcChar8*>(nonLatin->filePath.c_str()));
    FcObjectSet* os = FcObjectSetBuild(FC_LANG, static_cast<char*>(nullptr));
    FcFontSet* set = FcFontList(nullptr, pat, os);
    ASSERT_TRUE(set && set->nfont > 0);
    FcLangSet* ls = nullptr;
    ASSERT_EQ(FcPatternGetLangSet(set->fonts[0], FC_LANG, 0, &ls), FcResultMatch);
    EXPECT_EQ(FcLangSetHasLang(ls, reinterpret_cast<const FcChar8*>("en")), FcLangDifferentLang)
        << nonLatin->familyName << " is marked not-designed-for-Latin but "
           "fontconfig says it supports English";
    FcFontSetDestroy(set);
    FcObjectSetDestroy(os);
    FcPatternDestroy(pat);
}

// CLAIM: serif classification comes from the family name, including classic
// serif families whose names never say "serif", and never from "sans" names.
TEST(FontconfigSupport, SerifClassificationFromFamilyName)
{
    const auto fonts = fc::listFonts();
    const auto* serif = findFamily(fonts, "Liberation Serif");
    const auto* sans = findFamily(fonts, "Liberation Sans");
    ASSERT_NE(serif, nullptr) << "fonts-liberation is required";
    ASSERT_NE(sans, nullptr) << "fonts-liberation is required";
    EXPECT_TRUE(serif->isSerif);
    EXPECT_FALSE(sans->isSerif);

    // "Tinos" names no "serif": the name list, not the word, must catch it.
    if (const auto* tinos = findFamily(fonts, "Tinos")) {
        EXPECT_TRUE(tinos->isSerif);
    }
    if (const auto* notoSerif = findFamily(fonts, "Noto Serif")) {
        EXPECT_TRUE(notoSerif->isSerif);
    }
    if (const auto* notoSans = findFamily(fonts, "Noto Sans")) {
        EXPECT_FALSE(notoSans->isSerif);
    }
}

// CLAIM: matchFamilyFile accepts the requested family or a substitute the
// configuration binds to it, and returns "" for a family the system lacks
// (fontconfig itself always returns SOME fallback face).
TEST(FontconfigSupport, MatchFamilyFileRefusesTheGenericFallback)
{
    EXPECT_EQ(fc::matchFamilyFile("NoSuchFontFamily12345Zz", false, false), "")
        << "an unknown family resolved; the generic fallback was accepted";

    const std::string direct = fc::matchFamilyFile("DejaVu Sans", false, false);
    EXPECT_FALSE(direct.empty()) << "an installed family did not resolve";

    // Arial is not installed; the configuration binds Liberation Sans to it.
    const std::string alias = fc::matchFamilyFile("Arial", false, false);
    EXPECT_FALSE(alias.empty())
        << "Arial did not resolve to its configured metric alias";
    EXPECT_NE(alias.find("Liberation"), std::string::npos)
        << "Arial resolved to " << alias << ", not its metric alias";
}

#endif  // !_WIN32
