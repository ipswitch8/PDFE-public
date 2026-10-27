#include <gtest/gtest.h>
#include "fonts/FontAnalysisCache.h"
#include <filesystem>

using namespace pdfe::fonts;

class FontAnalysisCacheTest : public ::testing::Test {
protected:
    FontMetricProfile makeProfile(const std::string& name)
    {
        FontMetricProfile p;
        p.postScriptName = name;
        p.familyName = name;
        p.styleName = "Regular";
        p.unitsPerEm = 1000;
        p.ascender = 0.8f;
        p.descender = -0.2f;
        p.xHeight = 0.5f;
        p.capHeight = 0.7f;
        p.weightClass = 400;
        p.widthClass = 5;
        p.stemWeightV = 0.08f;
        p.stemWeightH = 0.06f;
        p.xToCapRatio = 0.714f;
        p.ascToCapRatio = 1.143f;
        p.avgCharWidth = 0.5f;
        p.isSerif = false;
        p.isMonospace = false;
        p.isItalic = false;
        p.italicAngle = 0.0f;
        p.serifExtent = 0.0f;
        p.glyphCount = 250;
        p.hasLatinBasic = true;
        p.hasLatinExtA = true;
        p.hasCyrillic = false;
        p.hasCJK = false;
        return p;
    }
};

TEST_F(FontAnalysisCacheTest, PutAndGetRoundTrip) {
    FontAnalysisCache cache;

    auto profile = makeProfile("ArialMT");
    cache.put(profile);

    auto result = cache.get("ArialMT");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->postScriptName, "ArialMT");
    EXPECT_EQ(result->familyName, "ArialMT");
    EXPECT_EQ(result->styleName, "Regular");
    EXPECT_EQ(result->weightClass, 400);
    EXPECT_FLOAT_EQ(result->stemWeightV, 0.08f);
    EXPECT_EQ(result->glyphCount, 250);
    EXPECT_TRUE(result->hasLatinBasic);
    EXPECT_TRUE(result->hasLatinExtA);
    EXPECT_FALSE(result->hasCyrillic);
}

TEST_F(FontAnalysisCacheTest, GetNonExistentReturnsNullopt) {
    FontAnalysisCache cache;

    auto result = cache.get("NonExistentFont");
    EXPECT_FALSE(result.has_value());
}

TEST_F(FontAnalysisCacheTest, ContainsCorrectness) {
    FontAnalysisCache cache;

    EXPECT_FALSE(cache.contains("ArialMT"));

    cache.put(makeProfile("ArialMT"));
    EXPECT_TRUE(cache.contains("ArialMT"));
    EXPECT_FALSE(cache.contains("TimesNewRoman"));
}

TEST_F(FontAnalysisCacheTest, LruEviction) {
    FontAnalysisCache cache;

    // Insert 257 entries (LRU capacity is 256)
    for (int i = 0; i < 257; ++i) {
        cache.put(makeProfile("Font_" + std::to_string(i)));
    }

    // Memory size should be capped at 256
    EXPECT_EQ(cache.memSize(), 256u);

    // SQLite should have all 257
    EXPECT_EQ(cache.size(), 257u);

    // Oldest (Font_0) should be evicted from LRU but still in SQLite
    auto result = cache.get("Font_0");
    EXPECT_TRUE(result.has_value()) << "Should still be in SQLite even after LRU eviction";
}

TEST_F(FontAnalysisCacheTest, ClearEmptiesBothTiers) {
    FontAnalysisCache cache;

    cache.put(makeProfile("ArialMT"));
    cache.put(makeProfile("TimesMT"));

    EXPECT_EQ(cache.size(), 2u);
    EXPECT_EQ(cache.memSize(), 2u);

    cache.clear();

    EXPECT_EQ(cache.size(), 0u);
    EXPECT_EQ(cache.memSize(), 0u);

    auto result = cache.get("ArialMT");
    EXPECT_FALSE(result.has_value());
}

TEST_F(FontAnalysisCacheTest, PersistenceAcrossInstances) {
    auto tempDir = std::filesystem::temp_directory_path();
    auto dbPath = tempDir / "test_font_cache.db";

    // Clean up from any previous test run
    std::filesystem::remove(dbPath);

    {
        FontAnalysisCache cache(dbPath);
        cache.put(makeProfile("ArialMT"));
        EXPECT_EQ(cache.size(), 1u);
    }

    // Create a new cache at the same path
    {
        FontAnalysisCache cache(dbPath);
        EXPECT_EQ(cache.size(), 1u);

        auto result = cache.get("ArialMT");
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->postScriptName, "ArialMT");
    }

    // Cleanup
    std::filesystem::remove(dbPath);
}

TEST_F(FontAnalysisCacheTest, InMemoryOnlyMode) {
    // Empty path = in-memory SQLite
    FontAnalysisCache cache;

    cache.put(makeProfile("TestFont"));
    EXPECT_EQ(cache.size(), 1u);
    EXPECT_EQ(cache.memSize(), 1u);

    auto result = cache.get("TestFont");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->postScriptName, "TestFont");
}
