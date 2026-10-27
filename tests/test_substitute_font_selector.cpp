// Copyright (c) 2025 PDFE Project
// Unit tests for SubstituteFontSelector

#include <gtest/gtest.h>
#include "fonts/SubstituteFontSelector.h"
#include "fonts/EmbeddedFontAnalyzer.h"
#include "fonts/FontAnalysisCache.h"
#include "fonts/FontMetricProfile.h"
#include <filesystem>
#include <memory>
#include <string>

using namespace pdfe::fonts;

namespace {

// Helper: Create a profile that mimics a Helvetica-like font
FontMetricProfile makeHelveticaLike() {
    FontMetricProfile p;
    p.postScriptName = "Helvetica";
    p.familyName = "Helvetica";
    p.styleName = "Regular";
    p.weightClass = 400;
    p.isSerif = false;
    p.isMonospace = false;
    p.isItalic = false;
    p.glyphCount = 500;
    p.hasLatinBasic = true;
    p.unitsPerEm = 1000;
    p.ascender = 0.75f;
    p.descender = -0.25f;
    p.xHeight = 0.5f;
    p.capHeight = 0.7f;
    p.stemWeightV = 0.08f;
    p.stemWeightH = 0.08f;
    p.avgCharWidth = 0.5f;
    p.serifExtent = 0.0f;
    return p;
}

// Helper: Create a profile that mimics CharisSILRegular as embedded in
// Lean Startup p2 — a Latin-script serif font that triggered the
// Nirmala UI substitution bug.
FontMetricProfile makeCharisSILLike() {
    FontMetricProfile p;
    p.postScriptName = "CharisSILRegular";
    p.familyName = "CharisSIL";
    p.styleName = "Regular";
    p.weightClass = 400;
    p.isSerif = true;
    p.isMonospace = false;
    p.isItalic = false;
    p.glyphCount = 1200;
    p.hasLatinBasic = true;
    p.hasLatinExtA = true;
    p.unitsPerEm = 2048;
    // Approximate metrics from the actual subset embedded in Lean Startup p2:
    // Ascent 731.4453, Descent -268.5547 normalized to 1000-em scale.
    p.ascender = 731.4453f / 1000.0f;
    p.descender = -268.5547f / 1000.0f;
    p.xHeight = 0.45f;
    p.capHeight = 0.66f;
    p.stemWeightV = 0.07f;
    p.stemWeightH = 0.04f;
    p.avgCharWidth = 0.45f;
    p.serifExtent = 0.13f;
    p.primaryScript = FontScript::Latin;
    return p;
}

// Helper: Create a profile that mimics a Times-like font
FontMetricProfile makeTimesLike() {
    FontMetricProfile p;
    p.postScriptName = "Times-Roman";
    p.familyName = "Times";
    p.styleName = "Roman";
    p.weightClass = 400;
    p.isSerif = true;
    p.isMonospace = false;
    p.isItalic = false;
    p.glyphCount = 500;
    p.hasLatinBasic = true;
    p.unitsPerEm = 1000;
    p.ascender = 0.68f;
    p.descender = -0.22f;
    p.xHeight = 0.45f;
    p.capHeight = 0.66f;
    p.stemWeightV = 0.09f;
    p.stemWeightH = 0.04f;
    p.avgCharWidth = 0.45f;
    p.serifExtent = 0.15f;
    return p;
}

// Helper: Create a profile that mimics a Courier-like font
FontMetricProfile makeCourierLike() {
    FontMetricProfile p;
    p.postScriptName = "Courier";
    p.familyName = "Courier";
    p.styleName = "Regular";
    p.weightClass = 400;
    p.isMonospace = true;
    p.isSerif = true;
    p.isItalic = false;
    p.glyphCount = 500;
    p.hasLatinBasic = true;
    p.unitsPerEm = 1000;
    p.ascender = 0.63f;
    p.descender = -0.16f;
    p.xHeight = 0.42f;
    p.capHeight = 0.57f;
    p.stemWeightV = 0.06f;
    p.stemWeightH = 0.06f;
    p.avgCharWidth = 0.6f;  // Fixed width
    p.serifExtent = 0.12f;
    return p;
}

} // anonymous namespace

class SubstituteFontSelectorTest : public ::testing::Test {
protected:
    void SetUp() override {
        // EmbeddedFontAnalyzer constructor accepts nullptr for MuPdfContext
        // when only using analyzeFromFile() and findMissingGlyphs()
        analyzer = std::make_unique<EmbeddedFontAnalyzer>(nullptr);

        // FontAnalysisCache with empty path creates in-memory cache
        cache = std::make_unique<FontAnalysisCache>();

        // Create the selector under test
        selector = std::make_unique<SubstituteFontSelector>(analyzer.get(), cache.get());
    }

    void TearDown() override {
        selector.reset();
        cache.reset();
        analyzer.reset();
    }

    std::unique_ptr<EmbeddedFontAnalyzer> analyzer;
    std::unique_ptr<FontAnalysisCache> cache;
    std::unique_ptr<SubstituteFontSelector> selector;
};

// Test 1: Known pair Helvetica returns Arial
TEST_F(SubstituteFontSelectorTest, KnownPairHelveticaReturnsArial) {
    // Skip if Arial is not available on this system
    if (!std::filesystem::exists("C:\\Windows\\Fonts\\arial.ttf")) {
        GTEST_SKIP() << "Arial font not available on this system";
    }

    FontMetricProfile helveticaProfile = makeHelveticaLike();
    std::u32string text = U"Hello World";

    auto result = selector->findSubstitute(helveticaProfile, text);

    ASSERT_TRUE(result.has_value()) << "Should find substitute for Helvetica";
    EXPECT_TRUE(result->isKnownPair) << "Should be identified as known pair";

    // Convert path to lowercase for case-insensitive comparison
    std::string filePath = result->filePath;
    std::transform(filePath.begin(), filePath.end(), filePath.begin(), ::tolower);
    EXPECT_NE(filePath.find("arial"), std::string::npos)
        << "File path should contain 'arial', got: " << result->filePath;
}

// Test 2: Known pair Times returns Times New Roman
TEST_F(SubstituteFontSelectorTest, KnownPairTimesReturnsTimesNR) {
    // Skip if Times New Roman is not available
    if (!std::filesystem::exists("C:\\Windows\\Fonts\\times.ttf") &&
        !std::filesystem::exists("C:\\Windows\\Fonts\\timesnewroman.ttf")) {
        GTEST_SKIP() << "Times New Roman font not available on this system";
    }

    FontMetricProfile timesProfile = makeTimesLike();
    std::u32string text = U"Hello World";

    auto result = selector->findSubstitute(timesProfile, text);

    ASSERT_TRUE(result.has_value()) << "Should find substitute for Times";
    EXPECT_TRUE(result->isKnownPair) << "Should be identified as known pair";

    // Convert path to lowercase for case-insensitive comparison
    std::string filePath = result->filePath;
    std::transform(filePath.begin(), filePath.end(), filePath.begin(), ::tolower);
    EXPECT_NE(filePath.find("times"), std::string::npos)
        << "File path should contain 'times', got: " << result->filePath;
}

// Test 3: Serif font gets serif substitute
TEST_F(SubstituteFontSelectorTest, SerifFontGetsSerifSubstitute) {
    FontMetricProfile timesProfile = makeTimesLike();
    std::u32string text = U"Test serif matching";

    auto result = selector->findSubstitute(timesProfile, text);

    if (result.has_value()) {
        // The profile in the result should indicate serif characteristics
        // Either through isSerif flag or serifExtent metric
        bool hasSerifCharacteristics = result->profile.isSerif ||
                                       result->profile.serifExtent > 0.0f;
        EXPECT_TRUE(hasSerifCharacteristics)
            << "Substitute for serif font should have serif characteristics";
    } else {
        GTEST_SKIP() << "No suitable serif substitute found on this system";
    }
}

// Test 4: Sans-serif font gets sans-serif substitute
TEST_F(SubstituteFontSelectorTest, SansFontGetsSansSubstitute) {
    FontMetricProfile helveticaProfile = makeHelveticaLike();
    std::u32string text = U"Test sans-serif matching";

    auto result = selector->findSubstitute(helveticaProfile, text);

    ASSERT_TRUE(result.has_value())
        << "Should find substitute for sans-serif font (common on all systems)";

    // Verify the result has a valid file path
    EXPECT_FALSE(result->filePath.empty()) << "Result should have a file path";
    EXPECT_TRUE(std::filesystem::exists(result->filePath))
        << "Font file should exist: " << result->filePath;
}

// Test 5: Monospace font gets monospace substitute
TEST_F(SubstituteFontSelectorTest, MonoFontGetsMonoSubstitute) {
    // Skip if Courier New is not available
    if (!std::filesystem::exists("C:\\Windows\\Fonts\\cour.ttf") &&
        !std::filesystem::exists("C:\\Windows\\Fonts\\couriernew.ttf")) {
        GTEST_SKIP() << "Courier New font not available on this system";
    }

    FontMetricProfile courierProfile = makeCourierLike();
    std::u32string text = U"Monospace text 123";

    auto result = selector->findSubstitute(courierProfile, text);

    ASSERT_TRUE(result.has_value()) << "Should find substitute for Courier";
    EXPECT_TRUE(result->isKnownPair) << "Courier should be a known pair";

    // Convert path to lowercase for case-insensitive comparison
    std::string filePath = result->filePath;
    std::transform(filePath.begin(), filePath.end(), filePath.begin(), ::tolower);
    EXPECT_NE(filePath.find("cour"), std::string::npos)
        << "File path should contain 'cour', got: " << result->filePath;
}

// Test 6: Empty text returns nullopt
TEST_F(SubstituteFontSelectorTest, EmptyTextReturnsNullopt) {
    FontMetricProfile helveticaProfile = makeHelveticaLike();
    std::u32string emptyText = U"";

    auto result = selector->findSubstitute(helveticaProfile, emptyText);

    EXPECT_FALSE(result.has_value())
        << "Should return nullopt for empty text";
}

// Test 7: Match score is reasonable for known pairs
TEST_F(SubstituteFontSelectorTest, MatchScoreIsReasonable) {
    // Skip if Arial is not available
    if (!std::filesystem::exists("C:\\Windows\\Fonts\\arial.ttf")) {
        GTEST_SKIP() << "Arial font not available on this system";
    }

    FontMetricProfile helveticaProfile = makeHelveticaLike();
    std::u32string text = U"Score test";

    auto result = selector->findSubstitute(helveticaProfile, text);

    ASSERT_TRUE(result.has_value()) << "Should find substitute";

    // Match score should be non-negative and finite
    EXPECT_GE(result->matchScore, 0.0f)
        << "Match score should be non-negative";
    EXPECT_TRUE(std::isfinite(result->matchScore))
        << "Match score should be finite";

    // For known pairs, score should typically be reasonable (not extremely high)
    // Lower score = better match in the distance metric
    if (result->isKnownPair) {
        EXPECT_LT(result->matchScore, 100.0f)
            << "Known pair score should be reasonable, got: " << result->matchScore;
    }
}

// Test 8: Cache hit on second call
TEST_F(SubstituteFontSelectorTest, CacheHitOnSecondCall) {
    FontMetricProfile helveticaProfile = makeHelveticaLike();
    std::u32string text = U"Cache test";

    // First call - may populate cache
    auto result1 = selector->findSubstitute(helveticaProfile, text);

    if (!result1.has_value()) {
        GTEST_SKIP() << "No substitute found - cannot test caching";
    }

    // Get initial cache size
    size_t cacheSize1 = cache->size();

    // Second call - should potentially use cache
    auto result2 = selector->findSubstitute(helveticaProfile, text);

    ASSERT_TRUE(result2.has_value()) << "Second call should also succeed";

    // Results should be consistent
    EXPECT_EQ(result1->filePath, result2->filePath)
        << "Both calls should return same substitute font";
    EXPECT_EQ(result1->isKnownPair, result2->isKnownPair)
        << "Known pair status should be consistent";

    // Cache should have entries (at least from system font analysis)
    size_t cacheSize2 = cache->size();
    EXPECT_GT(cacheSize2, 0) << "Cache should contain entries after font analysis";
}

// Test 9: Profile with special characters in name
TEST_F(SubstituteFontSelectorTest, HandlesSpecialCharactersInFontName) {
    FontMetricProfile profile = makeHelveticaLike();
    profile.postScriptName = "MyFont-BoldItalic";
    profile.familyName = "My Font";
    profile.styleName = "Bold Italic";

    std::u32string text = U"Test";

    auto result = selector->findSubstitute(profile, text);

    // Should not crash and should return either a result or nullopt
    // We don't enforce finding a substitute for unknown fonts
    if (result.has_value()) {
        EXPECT_FALSE(result->filePath.empty());
    }
}

// Test 10: Unicode text coverage
TEST_F(SubstituteFontSelectorTest, HandlesUnicodeText) {
    FontMetricProfile profile = makeHelveticaLike();

    // Text with various Unicode characters
    std::u32string unicodeText = U"Hello 世界 Привет";

    auto result = selector->findSubstitute(profile, unicodeText);

    // Should handle Unicode text without crashing
    // May or may not find a substitute depending on system fonts
    if (result.has_value()) {
        EXPECT_FALSE(result->filePath.empty());
        EXPECT_TRUE(std::filesystem::exists(result->filePath));
    }
}

// Test 11: Bold weight font matching
TEST_F(SubstituteFontSelectorTest, BoldWeightFontMatching) {
    FontMetricProfile boldProfile = makeHelveticaLike();
    boldProfile.postScriptName = "Helvetica-Bold";
    boldProfile.weightClass = 700;
    boldProfile.stemWeightV = 0.12f;  // Thicker stems
    boldProfile.stemWeightH = 0.12f;

    std::u32string text = U"Bold text";

    auto result = selector->findSubstitute(boldProfile, text);

    if (result.has_value()) {
        // Should find a substitute - likely Arial Bold or similar
        EXPECT_FALSE(result->filePath.empty());

        // The substitute should ideally have a higher weight class
        // (though this depends on what's available on the system)
        EXPECT_GT(result->profile.weightClass, 300)
            << "Substitute should not be an ultra-light font";
    }
}

// Test 12: Italic font matching
TEST_F(SubstituteFontSelectorTest, ItalicFontMatching) {
    FontMetricProfile italicProfile = makeHelveticaLike();
    italicProfile.postScriptName = "Helvetica-Oblique";
    italicProfile.isItalic = true;
    italicProfile.italicAngle = -12.0f;

    std::u32string text = U"Italic text";

    auto result = selector->findSubstitute(italicProfile, text);

    if (result.has_value()) {
        EXPECT_FALSE(result->filePath.empty());
        // Ideally would match to an italic font, but depends on system availability
    }
}

// Test 13: Very long text doesn't cause issues
TEST_F(SubstituteFontSelectorTest, HandlesLongText) {
    FontMetricProfile profile = makeHelveticaLike();

    // Create a very long text string
    std::u32string longText;
    longText.reserve(10000);
    for (int i = 0; i < 1000; i++) {
        longText += U"Long text ";
    }

    auto result = selector->findSubstitute(profile, longText);

    // Should handle long text without crashing or timing out
    if (result.has_value()) {
        EXPECT_FALSE(result->filePath.empty());
    }
}

// Test 14: Numeric-only text
TEST_F(SubstituteFontSelectorTest, HandlesNumericOnlyText) {
    FontMetricProfile profile = makeHelveticaLike();
    std::u32string numericText = U"0123456789";

    auto result = selector->findSubstitute(profile, numericText);

    if (result.has_value()) {
        EXPECT_FALSE(result->filePath.empty());
        EXPECT_TRUE(std::filesystem::exists(result->filePath));
    }
}

// Test 15: Profile validity affects matching
TEST_F(SubstituteFontSelectorTest, HandlesMinimalProfile) {
    // Create a minimal profile with only essential fields
    FontMetricProfile minimalProfile;
    minimalProfile.postScriptName = "UnknownFont";
    minimalProfile.familyName = "Unknown";
    minimalProfile.glyphCount = 100;
    minimalProfile.hasLatinBasic = true;

    std::u32string text = U"Test";

    auto result = selector->findSubstitute(minimalProfile, text);

    // Should not crash with minimal profile
    // May or may not find a substitute
    if (result.has_value()) {
        EXPECT_FALSE(result->filePath.empty());
    }
}

// ---------------------------------------------------------------------------
// findTopCandidates tests
// ---------------------------------------------------------------------------

TEST_F(SubstituteFontSelectorTest, FindTopCandidatesReturnsUpToN) {
    if (!std::filesystem::exists("C:\\Windows\\Fonts\\arial.ttf")) {
        GTEST_SKIP() << "Arial font not available on this system";
    }

    FontMetricProfile helveticaProfile = makeHelveticaLike();
    std::u32string text = U"Hello World";

    auto candidates = selector->findTopCandidates(helveticaProfile, text, 5);

    EXPECT_GT(candidates.size(), 0u) << "Should find at least one candidate";
    EXPECT_LE(candidates.size(), 5u) << "Should not exceed maxCandidates";

    // All candidates should have valid file paths
    for (const auto& c : candidates) {
        EXPECT_FALSE(c.filePath.empty()) << "Candidate should have file path";
    }
}

TEST_F(SubstituteFontSelectorTest, FindTopCandidatesSortedByScore) {
    FontMetricProfile helveticaProfile = makeHelveticaLike();
    std::u32string text = U"Test sorting";

    auto candidates = selector->findTopCandidates(helveticaProfile, text, 5);

    if (candidates.size() < 2) {
        GTEST_SKIP() << "Need at least 2 candidates to test sorting";
    }

    // All candidates should be sorted by matchScore ascending
    for (size_t i = 1; i < candidates.size(); ++i) {
        EXPECT_LE(candidates[i - 1].matchScore, candidates[i].matchScore)
            << "Candidates should be sorted by score at indices " << (i - 1) << " and " << i;
    }
}

TEST_F(SubstituteFontSelectorTest, FindTopCandidatesKnownPairIncludedInLargerSet) {
    if (!std::filesystem::exists("C:\\Windows\\Fonts\\arial.ttf")) {
        GTEST_SKIP() << "Arial font not available on this system";
    }

    FontMetricProfile helveticaProfile = makeHelveticaLike();
    std::u32string text = U"Known pair test";

    // Request more candidates to ensure the known pair is included
    auto candidates = selector->findTopCandidates(helveticaProfile, text, 20);

    ASSERT_GT(candidates.size(), 0u);

    // Known pair (Helvetica → Arial) should be somewhere in the results
    bool foundKnownPair = false;
    for (const auto& c : candidates) {
        std::string lower = c.filePath;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        if (lower.find("arial") != std::string::npos) {
            foundKnownPair = true;
            break;
        }
    }
    EXPECT_TRUE(foundKnownPair)
        << "Known pair (Arial) should be included in candidates";

    // Results should be sorted by matchScore ascending
    for (size_t i = 1; i < candidates.size(); ++i) {
        EXPECT_LE(candidates[i - 1].matchScore, candidates[i].matchScore)
            << "Candidates should be sorted by matchScore ascending";
    }
}



// Regression test: CharisSILRegular substitution must not pick Nirmala UI
// or any other multi-script Indian/CJK/Arabic family for Latin source text.
// On Lean Startup p2, the original scoring picked Nirmala UI (3.555) over
// Arial (3.615) by a 0.06 margin, producing visibly oversized text because
// Nirmala UI has Ascent=1079 vs CharisSIL's Ascent=731.
TEST_F(SubstituteFontSelectorTest, CharisSILDoesNotMatchToNirmalaUI) {
    if (!std::filesystem::exists("C:\\Windows\\Fonts\\arial.ttf")) {
        GTEST_SKIP() << "Arial not available; system fonts test requires it";
    }

    FontMetricProfile charisProfile = makeCharisSILLike();
    // Same text the user typed in the Lean Startup repro: starts with
    // U+201C (left double quotation mark, in General Punctuation block).
    std::u32string text = U"“Eric Ries unxravels the mysteries of "
                          U"entrepreneurship and reveals";

    auto result = selector->findSubstitute(charisProfile, text);
    ASSERT_TRUE(result.has_value())
        << "Should find a Latin substitute for CharisSILRegular";

    // The chosen family must not be a multi-script / non-Latin-primary
    // family. Lower-case for case-insensitive substring match.
    std::string family = result->profile.familyName;
    std::transform(family.begin(), family.end(), family.begin(), ::tolower);

    static const char* const forbidden[] = {
        "nirmala", "mangal", "kokila", "leelawadee", "myanmar",
        "padauk", "tibetan", "ebrima", "gadugi",
        "simsun", "simhei", "msgothic", "yu gothic",
        "malgun", "scheherazade", "amiri",
    };
    for (const char* f : forbidden) {
        EXPECT_EQ(family.find(f), std::string::npos)
            << "CharisSIL substitute must not be multi-script family. "
               "Got: " << result->profile.familyName;
    }
}
