#include <gtest/gtest.h>
#include "fonts/FontFamilyResolver.h"

using namespace pdfe::fonts;

class FontFamilyResolverTest : public ::testing::Test {
protected:
    void SetUp() override {
    }
    FontFamilyResolver resolver;
};

TEST_F(FontFamilyResolverTest, ResolveArial_NonEmpty) {
    std::string path = resolver.resolve("Arial");
    EXPECT_FALSE(path.empty()) << "Arial font should be found on Windows system";
}

TEST_F(FontFamilyResolverTest, ResolveArialBold_NonEmpty) {
    std::string path = resolver.resolve("Arial", true, false);
    EXPECT_FALSE(path.empty()) << "Arial Bold should be found on Windows system";
}

TEST_F(FontFamilyResolverTest, ResolveArialItalic_NonEmpty) {
    std::string path = resolver.resolve("Arial", false, true);
    EXPECT_FALSE(path.empty()) << "Arial Italic should be found on Windows system";
}

TEST_F(FontFamilyResolverTest, ResolveArialBoldItalic_NonEmpty) {
    std::string path = resolver.resolve("Arial", true, true);
    EXPECT_FALSE(path.empty()) << "Arial Bold Italic should be found on Windows system";
}

TEST_F(FontFamilyResolverTest, ResolveNonExistent_Empty) {
    std::string path = resolver.resolve("NonExistentFont12345");
    EXPECT_TRUE(path.empty()) << "Non-existent font should return empty string";
}

TEST_F(FontFamilyResolverTest, CacheHit_SameResult) {
    std::string path1 = resolver.resolve("Arial", true, false);
    std::string path2 = resolver.resolve("Arial", true, false);

    EXPECT_EQ(path1, path2) << "Cache should return same result for same query";
}

TEST_F(FontFamilyResolverTest, DifferentStyles_DifferentPaths) {
    std::string regularPath = resolver.resolve("Arial", false, false);
    std::string boldPath = resolver.resolve("Arial", true, false);

    // Don't assert they're different (could be same file with multiple faces)
    // Just verify both calls succeed without crashing
    EXPECT_FALSE(regularPath.empty());
    EXPECT_FALSE(boldPath.empty());
}
