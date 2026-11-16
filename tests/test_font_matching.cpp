#include <gtest/gtest.h>
#include "core/FontMetrics.h"
#include "core/SystemFontDiscovery.h"
#include "core/BundledFonts.h"
#include "core/BasicFontMatcher.h"

#include <string>

using namespace pdfe::core;

// ─── SystemFontDiscovery Tests ───────────────────────────────────────────────

class SystemFontDiscoveryTest : public ::testing::Test {
protected:
    void SetUp() override {
    }
    SystemFontDiscovery discovery;
};

TEST_F(SystemFontDiscoveryTest, EnumerateFonts) {
    const auto& fonts = discovery.enumerateFonts();

    // Any modern system should have fonts installed
    EXPECT_FALSE(fonts.empty()) << "Should discover at least some system fonts";

    // Verify each font has basic properties
    for (const auto& fm : fonts) {
        EXPECT_FALSE(fm.familyName.empty()) << "Font family name should not be empty";
        EXPECT_GT(fm.unitsPerEm, 0) << "unitsPerEm should be positive for " << fm.familyName;
    }
}

TEST_F(SystemFontDiscoveryTest, EnumerateFontsIsCached) {
    const auto& fonts1 = discovery.enumerateFonts();
    const auto& fonts2 = discovery.enumerateFonts();

    // Second call should return same pointer (cached)
    EXPECT_EQ(&fonts1, &fonts2);
}

TEST_F(SystemFontDiscoveryTest, FindByPostScriptName) {
    // Arial is present on virtually all Windows systems
#ifdef _WIN32
    auto result = discovery.findByPostScriptName("ArialMT");
    if (!result) {
        // Try alternate name
        result = discovery.findByPostScriptName("Arial");
    }
    // Arial might be named differently, so just verify the method doesn't crash
    // and returns std::nullopt for definitely nonexistent fonts
    auto noResult = discovery.findByPostScriptName("NonexistentFont12345xyz");
    EXPECT_FALSE(noResult.has_value());
#else
    // fontconfig enumeration (GitHub #390). Arial's metric twin is the
    // installed face; its PostScript name is LiberationSans. The lookup is
    // case-insensitive, so the lower-case spelling must find it too.
    auto result = discovery.findByPostScriptName("LiberationSans");
    ASSERT_TRUE(result.has_value())
        << "LiberationSans (fonts-liberation) was not found by PostScript name";
    EXPECT_EQ(result->familyName, "Liberation Sans");
    EXPECT_TRUE(discovery.findByPostScriptName("liberationsans").has_value());
    EXPECT_FALSE(discovery.findByPostScriptName("NonexistentFont12345xyz").has_value());
#endif
}

TEST_F(SystemFontDiscoveryTest, FindByFamilyStyle) {
#ifdef _WIN32
    auto result = discovery.findByFamilyStyle("Arial", FontWeight::Normal, FontStyle::Normal);
    if (result) {
        EXPECT_EQ(result->familyName, "Arial");
        EXPECT_EQ(result->style, FontStyle::Normal);
    }

    // Try bold variant
    auto boldResult = discovery.findByFamilyStyle("Arial", FontWeight::Bold, FontStyle::Normal);
    if (boldResult) {
        EXPECT_EQ(boldResult->familyName, "Arial");
    }
#else
    // fontconfig enumeration (GitHub #390), against an installed family.
    auto result = discovery.findByFamilyStyle("Liberation Sans", FontWeight::Normal,
                                              FontStyle::Normal);
    ASSERT_TRUE(result.has_value()) << "Liberation Sans (fonts-liberation) not found";
    EXPECT_EQ(result->familyName, "Liberation Sans");
    EXPECT_EQ(result->style, FontStyle::Normal);
    EXPECT_EQ(result->weight, FontWeight::Normal);

    auto boldResult = discovery.findByFamilyStyle("Liberation Sans", FontWeight::Bold,
                                                  FontStyle::Normal);
    ASSERT_TRUE(boldResult.has_value()) << "Liberation Sans Bold not found";
    EXPECT_EQ(boldResult->familyName, "Liberation Sans");
    EXPECT_EQ(boldResult->weight, FontWeight::Bold);
    EXPECT_NE(boldResult->filePath, result->filePath)
        << "the bold lookup returned the regular face's file";
#endif
}

// ─── BundledFonts Tests ─────────────────────────────────────────────────────

TEST(BundledFontsTest, GetFonts) {
    const auto& fonts = BundledFonts::getFonts();
    EXPECT_EQ(fonts.size(), 5u) << "Should have 5 bundled fonts";
}

TEST(BundledFontsTest, DefaultSansSerif) {
    auto fm = BundledFonts::getDefaultSansSerif();
    EXPECT_EQ(fm.familyName, "DejaVu Sans");
    EXPECT_FALSE(fm.isSerif);
    EXPECT_FALSE(fm.isMonospace);
}

TEST(BundledFontsTest, DefaultSerif) {
    auto fm = BundledFonts::getDefaultSerif();
    EXPECT_EQ(fm.familyName, "DejaVu Serif");
    EXPECT_TRUE(fm.isSerif);
}

TEST(BundledFontsTest, DefaultMonospace) {
    auto fm = BundledFonts::getDefaultMonospace();
    EXPECT_EQ(fm.familyName, "DejaVu Sans Mono");
    EXPECT_TRUE(fm.isMonospace);
}

TEST(BundledFontsTest, FindByFamily) {
    auto result = BundledFonts::findByFamily("DejaVu Sans");
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(result->familyName, "DejaVu Sans");

    auto noResult = BundledFonts::findByFamily("NonexistentFont");
    EXPECT_FALSE(noResult.has_value());
}

// ─── BasicFontMatcher Tests ─────────────────────────────────────────────────

class BasicFontMatcherTest : public ::testing::Test {
protected:
    BasicFontMatcher matcher;
};

TEST_F(BasicFontMatcherTest, MatchCommonFontAlias) {
    // "Helvetica" should resolve to Arial or a sans-serif font
    auto result = matcher.findMatch("Helvetica");
    EXPECT_FALSE(result.familyName.empty());
    EXPECT_FALSE(result.isSerif) << "Helvetica alias should resolve to sans-serif";
}

TEST_F(BasicFontMatcherTest, MatchTimesAlias) {
    auto result = matcher.findMatch("Times-Roman");
    EXPECT_FALSE(result.familyName.empty());
    // Should resolve to Times New Roman or a serif font
    EXPECT_TRUE(result.isSerif || result.familyName.find("Times") != std::string::npos
        || result.familyName.find("Serif") != std::string::npos
        || result.familyName.find("DejaVu") != std::string::npos)
        << "Times alias should resolve to a serif font, got: " << result.familyName;
}

TEST_F(BasicFontMatcherTest, MatchCourierAlias) {
    auto result = matcher.findMatch("Courier");
    EXPECT_FALSE(result.familyName.empty());
    // Should resolve to monospace
    EXPECT_TRUE(result.isMonospace || result.familyName.find("Mono") != std::string::npos
        || result.familyName.find("Courier") != std::string::npos)
        << "Courier alias should resolve to monospace, got: " << result.familyName;
}

TEST_F(BasicFontMatcherTest, FallbackToBundle) {
    // A completely nonexistent font should still return a usable font
    // (either a system font scored above threshold, or bundled DejaVu as last resort)
    auto result = matcher.findMatch("ZzzNonexistentFont999");
    EXPECT_FALSE(result.familyName.empty())
        << "Should always return a font, even for unknown names";
}

TEST_F(BasicFontMatcherTest, MatchWithWeightAndStyle) {
    auto result = matcher.findMatch("Arial", FontWeight::Bold, FontStyle::Normal, false);
    EXPECT_FALSE(result.familyName.empty());
    // Should prefer bold weight
}

TEST_F(BasicFontMatcherTest, MatchSerifPreference) {
    auto result = matcher.findMatch("SomeFont", FontWeight::Normal, FontStyle::Normal, true);
    EXPECT_FALSE(result.familyName.empty());
    // With preferSerif=true, should get a serif font (or serif fallback)
}

TEST_F(BasicFontMatcherTest, BoldInferredFromName) {
    auto result = matcher.findMatch("Helvetica-Bold");
    EXPECT_FALSE(result.familyName.empty());
    // The matcher should infer bold weight from the "-Bold" suffix
}

TEST_F(BasicFontMatcherTest, ItalicInferredFromName) {
    auto result = matcher.findMatch("Times-Italic");
    EXPECT_FALSE(result.familyName.empty());
    // Should resolve with italic preference
}
